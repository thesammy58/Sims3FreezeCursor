// SIMS3FREEZECURSOR - standalone, GUI-free freeze-detection cursor swap
//
// Shows TS3.exe's own busy cursor whenever the simulation thread stalls for
// more than a second, so a frozen game (blank stare at the last frame,
// input doing nothing) reads visually the same way a legitimate load
// screen already does, instead of leaving the player staring at an
// unchanged normal cursor with no signal anything is wrong.
//
// This is deliberately JUST the cursor swap: no on-screen corner indicator,
// no 20-second escalation dialog, no ImGui/D3D9 dependency. Those live in
// Sims3MonoModder (S3MM), which this project was split out of. This mod is
// built to run either fully standalone, or side by side with S3MM's own
// still-intact copy of the same feature - see addresses.h, sim_heartbeat_
// hook.cpp, and cursor_hook.h for the specific reasoning behind why both
// the native-function hook and the WndProc subclass are safe to install
// twice, in either load order.

#include "util/logger.h"
#include "util/paths.h"
#include "util/memory.h"
#include "core/addresses.h"
#include "core/freeze_watchdog.h"
#include "core/sim_heartbeat_hook.h"
#include "core/crash_capture.h"
#include "util/config.h"
#include "util/game_build.h"
#include <Windows.h>
#include <string>

namespace S3FC {

DWORD WINAPI MainThread(LPVOID /*lpParam*/) {
    // Bail if injected into a non-game process (e.g. a launcher)
    if (!Memory::IsGameProcess()) { return 0; }

    // Windows "ghosts" a window and substitutes its own hang cursor/
    // "(Not Responding)" UI whenever that window's message pump falls
    // behind - a real OS-level mechanism, entirely separate from
    // FreezeWatchdog and CursorHook's own WM_SETCURSOR handling. TS3
    // triggers this on its own during genuinely long, apparently-
    // synchronous main-thread load operations. Disabling it here (ported
    // from S3MM's dllmain.cpp, same reasoning) keeps OUR cursor - or the
    // game's own - in control of what the player sees during a load, or a
    // real freeze, rather than Windows' generic ghost-window substitute.
    DisableProcessWindowsGhosting();

    Paths::EnsureDirectoryExists();
    Logger::Get().Initialize(Paths::GetLogPath());
    LOG_INFO("Sims3FreezeCursor starting");

    // Steam, retail and the EA App ship different builds with different
    // addresses, so every log says which one it came from.
    LOG_INFO("Game build: " + GameBuild::Name() + " (TimeDateStamp " + ToHex(GameBuild::TimeDateStamp()) + ")");

    // CRASH CAPTURE - opt-in, and off by default. When it is on it installs
    // before everything below, so a fault during this mod's own startup is
    // captured too. See config.h for why a player does not get this by
    // default, and crash_capture.h for what it collects.
    if (Config::CrashCaptureEnabled()) {
        LOG_INFO("[Config] CrashCapture enabled in ini");
        CrashCapture::Get().Initialize();
    }

    // FREEZE WATCHDOG - starts immediately; independent of Addresses/hooks,
    // so it's running (and can already report "no heartbeat yet") for as
    // much of startup as possible.
    if (!FreezeWatchdog::Get().Initialize()) {
        LOG_ERROR("Failed to start FreezeWatchdog - freeze detection unavailable");
        return 0;
    }

    // ADDRESS RESOLUTION + SIM HEARTBEAT HOOK - the only native hook this
    // mod installs. Both are skipped when SimHook is turned off in the ini,
    // which leaves the watchdog running with a heartbeat that never fires:
    // it reports Ok forever and the cursor never changes, so the mod is
    // inert rather than half-installed. That is what makes it a usable
    // bisect step.
    if (Config::SimHookEnabled()) {
        // A native function inside the game's own image, so unlike a Mono
        // icall it is resolvable this early - no need to wait for Mono or
        // game startup to reach any particular stage first.
        if (!Addresses::Initialize()) {
            LOG_ERROR("Failed to resolve MonoScriptHost::ProcessTasks - freeze detection unavailable");
            return 0;
        }

        if (!SimHeartbeatHook::Initialize()) {
            LOG_ERROR("Failed to install sim-heartbeat hook - freeze detection unavailable");
            return 0;
        }
    } else {
        LOG_WARN("[Config] SimHook disabled in ini - no stall detection this run");
    }

    // CursorHook itself is NOT initialized here - the game window usually
    // doesn't exist yet this early in startup. FreezeWatchdog's own poll
    // loop retries CursorHook::Initialize() every 150ms until it succeeds
    // (see freeze_watchdog.cpp), so nothing further is needed on this
    // thread.

    LOG_INFO("Ready - sim stall threshold " + std::to_string((int)FreezeWatchdog::kStallThresholdSeconds) + "s");
    return 0;
}

} // namespace S3FC

// DLL ENTRY POINT - process attach spawns the main thread
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        HANDLE hThread = CreateThread(nullptr, 0, S3FC::MainThread, nullptr, 0, nullptr);
        if (hThread) { CloseHandle(hThread); }
    }

    return TRUE;
}
