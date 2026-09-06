#include "freeze_watchdog.h"
#include "crash_capture.h"
#include "cursor_hook.h"
#include "../util/config.h"
#include "../util/logger.h"
#include <cstdio>

namespace S3FC {

volatile LONG g_simHeartbeatTicks = 0;

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
    LONG windowStartTicks = g_simHeartbeatTicks;
    long windowStartMessages = CursorHook::SetCursorMessages();
    long windowStartSwaps = CursorHook::CursorSwaps();
    double windowWorstGap = 0.0;
    unsigned windowStallEntries = 0;

    // The watchdog owns the clock (see freeze_watchdog.h). It samples the
    // counter the detour bumps and timestamps only the changes, so the sim
    // thread never reads a clock at all.
    LONG lastSeenTicks = g_simHeartbeatTicks;
    int64_t lastChangeQpc = 0;
    bool everFired = false;

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

        const LONG ticks = g_simHeartbeatTicks;
        if (ticks != lastSeenTicks) {
            lastSeenTicks = ticks;
            lastChangeQpc = now.QuadPart;
            everFired = true;
        }

        // "Never fired yet" is not the same as "stale" - before the first
        // heartbeat arrives there is nothing to be late.
        double simStalled = 0.0;
        if (everFired) {
            int64_t delta = now.QuadPart - lastChangeQpc;
            if (delta < 0) delta = 0; // guard against QPC/TSC weirdness across cores
            simStalled = (double)delta / (double)m_qpcFrequency;
        }

        WatchdogState newState = (simStalled >= kStallThresholdSeconds) ? WatchdogState::Stalled : WatchdogState::Ok;
        m_state.store(newState, std::memory_order_relaxed);

        if (newState != lastLoggedState) {
            LOG_INFO("[FreezeWatchdog] state -> " + std::string(newState == WatchdogState::Stalled ? "Stalled" : "Ok") + " (simStalled=" + std::to_string(simStalled) + "s)");
            if (newState == WatchdogState::Stalled) { ++windowStallEntries; }
            lastLoggedState = newState;
        }

        if (simStalled > windowWorstGap) { windowWorstGap = simStalled; }

        if (statsSeconds > 0) {
            const DWORD elapsedMs = GetTickCount() - windowStartMs;
            if (elapsedMs >= static_cast<DWORD>(statsSeconds) * 1000) {
                const LONG beats = g_simHeartbeatTicks - windowStartTicks;
                const double rate = elapsedMs ? (beats * 1000.0 / elapsedMs) : 0.0;
                const long messages = CursorHook::SetCursorMessages() - windowStartMessages;
                const long swaps = CursorHook::CursorSwaps() - windowStartSwaps;

                char line[256];
                sprintf_s(line,
                          "[FreezeWatchdog] %lums: pump %ld calls (%.1f/s), worst gap %.3fs, %u stalls, %ld WM_SETCURSOR, %ld cursor swaps",
                          static_cast<unsigned long>(elapsedMs), static_cast<long>(beats), rate,
                          windowWorstGap, windowStallEntries, messages, swaps);
                LOG_INFO(line);

                windowStartMs = GetTickCount();
                windowStartTicks = g_simHeartbeatTicks;
                windowStartMessages = CursorHook::SetCursorMessages();
                windowStartSwaps = CursorHook::CursorSwaps();
                windowWorstGap = 0.0;
                windowStallEntries = 0;
            }
        }

        CrashCapture::Get().RetireIfSpent();

        Sleep(kPollIntervalMs);
    }
}

} // namespace S3FC
