#pragma once
#include <atomic>
#include <cstdint>
#include <Windows.h>

namespace S3FC {

// FREEZE WATCHDOG
//
// Detects when the game's simulation (sim thread) has stopped making
// progress, using a purely passive heartbeat timestamp stamped by
// sim_heartbeat_hook.cpp's MonoScriptHost::ProcessTasks detour. The
// watchdog's own polling thread NEVER calls back into game/Mono code to ask
// "are you alive" - it only reads an atomic that other code stamps as a
// side effect of its own normal execution, since the sim thread could be
// blocked inside a Mono GC pause or a stuck interaction, and any call INTO
// that machinery from an outside thread is a bet the watchdog can't afford
// to make. Ported from S3MM's core/freeze_watchdog.h/.cpp, trimmed down:
//
//   - No RenderStalled state / OnRenderHeartbeat() - that requires a D3D9
//     EndScene hook, which this mod deliberately doesn't have (no on-screen
//     UI to render in the first place - see project notes).
//   - No SimEscalated state, no game-busy-cursor gating - those exist in
//     S3MM solely to keep a legitimate 20+ second load screen from
//     triggering its escalation DIALOG. This mod has no dialog and no
//     distinction between "just stalled" and "stalled a long time" - both
//     look identical at the cursor (busy), so there's nothing to gate.
//   - No StuckCallTracker poll - that's a livelock/true-hang classifier
//     that only matters for deciding what a dialog should say.
enum class WatchdogState : int {
    Ok = 0,  // sim heartbeat fresh
    Stalled, // sim heartbeat stale (>= kStallThresholdSeconds)
};

class FreezeWatchdog {
  public:
    static FreezeWatchdog& Get();

    // Starts the background polling thread. Safe to call before the game
    // window exists - the heartbeat simply hasn't fired yet (GetState()
    // stays Ok; "never fired" is not the same as "stale").
    bool Initialize();
    void Shutdown();

    // Called from the MonoScriptHost::ProcessTasks detour, once per sim
    // frame under normal operation (see sim_heartbeat_hook.cpp).
    void OnSimHeartbeat();

    // Polled by CursorHook. Lock-free, safe from any thread.
    WatchdogState GetState() const { return m_state.load(std::memory_order_relaxed); }

    // Threshold, in seconds, matching S3MM's own indicator threshold (its
    // design doc's brief: 1s to first show something's wrong).
    static constexpr double kStallThresholdSeconds = 1.0;
    static constexpr DWORD kPollIntervalMs = 150;

  private:
    FreezeWatchdog() = default;
    ~FreezeWatchdog() = default;
    FreezeWatchdog(const FreezeWatchdog&) = delete;
    FreezeWatchdog& operator=(const FreezeWatchdog&) = delete;

    static DWORD WINAPI ThreadProc(LPVOID param);
    void PollLoop();

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_stopRequested{false};
    HANDLE m_threadHandle = nullptr;

    // QueryPerformanceCounter ticks. 0 means "never fired yet".
    std::atomic<int64_t> m_lastSimHeartbeatQpc{0};

    std::atomic<WatchdogState> m_state{WatchdogState::Ok};

    int64_t m_qpcFrequency = 1; // set in Initialize(); QueryPerformanceFrequency is never 0 on any supported Windows version
};

} // namespace S3FC
