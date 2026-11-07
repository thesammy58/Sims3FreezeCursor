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
// SIM HEARTBEAT COUNTER - bumped by the ProcessTasks detour, on the sim
// thread, once per call.
//
// Deliberately a bare counter at namespace scope, and deliberately NOT a
// timestamp. The first version of this called QueryPerformanceCounter in the
// detour. On most machines that is a ~20ns RDTSC read - but when Windows
// cannot use the TSC as its QPC source it falls back to the HPET or the ACPI
// power-management timer, and the same call becomes a hardware bus read
// costing one to three MICROSECONDS. Which one a player gets depends on
// their chipset, firmware and BCD settings, not on anything visible from
// here. On a task pump that drains its queue in bursts, that difference is
// the difference between a hook nobody can measure and a visible hitch
// several times a second.
//
// Nor is it FreezeWatchdog::Get().OnSimHeartbeat() any more: that accessor
// is a function-local static, so every single call paid a guard load and a
// call through it before doing any work.
//
// The hot path is now one increment. The watchdog thread polls at 150ms and
// owns the clock - "this number stopped moving", plus a timestamp taken on
// the watchdog's own thread, is all the stall detector ever needed. It costs
// 150ms of granularity against a one-second threshold, which the log was
// already quantised to anyway.
//
// A lost increment from a torn read does not matter: nothing reads the
// value, only whether it changed.
extern volatile LONG g_simHeartbeatTicks;

inline void SimHeartbeat() { ++g_simHeartbeatTicks; }

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

    // Polled by CursorHook. Lock-free, safe from any thread.
    WatchdogState GetState() const { return m_state.load(std::memory_order_relaxed); }

    // Threshold, in seconds. Long enough that an ordinary hitch doesn't
    // trigger it, short enough to show up while you're still wondering.
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

    std::atomic<WatchdogState> m_state{WatchdogState::Ok};

    int64_t m_qpcFrequency = 1; // set in Initialize(); QueryPerformanceFrequency is never 0 on any supported Windows version
};

} // namespace S3FC
