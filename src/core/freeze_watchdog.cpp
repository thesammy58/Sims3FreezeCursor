#include "freeze_watchdog.h"
#include "crash_capture.h"
#include "cursor_hook.h"
#include "../util/config.h"
#include "../util/logger.h"
#include <cstdio>

namespace S3FC {

FreezeWatchdog& FreezeWatchdog::Get() {
    static FreezeWatchdog instance;
    return instance;
}

bool FreezeWatchdog::Initialize() {
    if (m_initialized.exchange(true)) return true;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    m_qpcFrequency = freq.QuadPart;

    m_stopRequested.store(false, std::memory_order_relaxed);

    HANDLE hThread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!hThread) {
        LOG_ERROR("[FreezeWatchdog] Failed to create watchdog thread, GetLastError=" + std::to_string(GetLastError()));
        m_initialized.store(false);
        return false;
    }
    m_threadHandle = hThread;

    LOG_INFO("[FreezeWatchdog] Watchdog thread started (threshold=" + std::to_string((int)kStallThresholdSeconds) + "s, poll=" + std::to_string(kPollIntervalMs) + "ms)");
    return true;
}

void FreezeWatchdog::Shutdown() {
    if (!m_initialized.exchange(false)) return;

    m_stopRequested.store(true, std::memory_order_relaxed);

    if (m_threadHandle) {
        WaitForSingleObject(m_threadHandle, 2000);
        CloseHandle(m_threadHandle);
        m_threadHandle = nullptr;
    }

    LOG_INFO("[FreezeWatchdog] Watchdog thread stopped");
}

void FreezeWatchdog::OnSimHeartbeat() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    m_lastSimHeartbeatQpc.store(now.QuadPart, std::memory_order_relaxed);
    m_heartbeatCount.fetch_add(1, std::memory_order_relaxed);
}

DWORD WINAPI FreezeWatchdog::ThreadProc(LPVOID param) {
    reinterpret_cast<FreezeWatchdog*>(param)->PollLoop();
    return 0;
}

void FreezeWatchdog::PollLoop() {
    WatchdogState lastLoggedState = WatchdogState::Ok;

    // Throughput sampling, off unless the ini asks for it. Without this the
    // watchdog logs transitions and nothing else, so a game running at a few
    // frames a second while the sim pump keeps up writes an empty log - the
    // exact shape of a report that cannot be acted on.
    const int statsSeconds = Config::HeartbeatStatsSeconds();
    DWORD windowStartMs = GetTickCount();
    uint64_t windowStartBeats = m_heartbeatCount.load(std::memory_order_relaxed);
    double windowWorstGap = 0.0;

    // The game window may not exist yet this early (created partway through
    // TS3's own startup) - retry every poll interval until CursorHook
    // manages to find and subclass it. Once installed, Initialize() is a
    // cheap no-op (m_installed guard), so this costs nothing for the rest
    // of the session. Ported from the identical retry pattern S3MM uses for
    // its own GameCursorHook.
    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        if (Config::CursorHookEnabled() && !CursorHook::Get().IsInstalled()) { CursorHook::Get().Initialize(); }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        int64_t last = m_lastSimHeartbeatQpc.load(std::memory_order_relaxed);
        double simStalled = 0.0;
        if (last != 0) {
            int64_t delta = now.QuadPart - last;
            if (delta < 0) delta = 0; // guard against QueryPerformanceCounter/TSC weirdness across cores
            simStalled = (double)delta / (double)m_qpcFrequency;
        }

        WatchdogState newState = (simStalled >= kStallThresholdSeconds) ? WatchdogState::Stalled : WatchdogState::Ok;
        m_state.store(newState, std::memory_order_relaxed);

        if (newState != lastLoggedState) {
            LOG_INFO("[FreezeWatchdog] state -> " + std::string(newState == WatchdogState::Stalled ? "Stalled" : "Ok") + " (simStalled=" + std::to_string(simStalled) + "s)");
            lastLoggedState = newState;
        }

        if (simStalled > windowWorstGap) { windowWorstGap = simStalled; }

        if (statsSeconds > 0) {
            const DWORD elapsedMs = GetTickCount() - windowStartMs;
            if (elapsedMs >= static_cast<DWORD>(statsSeconds) * 1000) {
                const uint64_t beats = m_heartbeatCount.load(std::memory_order_relaxed) - windowStartBeats;
                const double rate = elapsedMs ? (beats * 1000.0 / elapsedMs) : 0.0;
                char line[192];
                sprintf_s(line, "[FreezeWatchdog] sim pump: %llu calls in %lums (%.1f/s), worst gap %.3fs",
                          static_cast<unsigned long long>(beats), static_cast<unsigned long>(elapsedMs), rate, windowWorstGap);
                LOG_INFO(line);
                windowStartMs = GetTickCount();
                windowStartBeats = m_heartbeatCount.load(std::memory_order_relaxed);
                windowWorstGap = 0.0;
            }
        }

        CrashCapture::Get().RetireIfSpent();

        Sleep(kPollIntervalMs);
    }
}

} // namespace S3FC
