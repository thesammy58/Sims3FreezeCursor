#include "sim_heartbeat_hook.h"
#include "addresses.h"
#include "freeze_watchdog.h"
#include "../util/logger.h"
#include <Windows.h>
#include <detours/detours.h>

namespace S3FC {

// MonoScriptHost::ProcessTasks - __fastcall(pHost in ECX), per-frame task
// pump inside Simulate. Calling convention and role confirmed against
// S3MM's own core/script_host_hook.cpp, which hooks the identical function.
using ProcessTasks_t = void(__fastcall*)(void* pHost);
static ProcessTasks_t s_original = nullptr;

static void __fastcall DetourProcessTasks(void* pHost) {
    // Stamped at entry, not after the call returns: if a Task inside
    // Simulate() later hangs without yielding, Simulate() never returns and
    // this detour is never re-entered next frame either way, so the
    // heartbeat correctly goes stale in both cases. See freeze_watchdog.h.
    FreezeWatchdog::Get().OnSimHeartbeat();
    s_original(pHost);
}

namespace SimHeartbeatHook {

bool Initialize() {
    if (s_original) return true; // already installed

    if (!Addresses::MonoScriptHost_ProcessTasks) {
        LOG_ERROR("[SimHeartbeatHook] MonoScriptHost::ProcessTasks address unresolved - cannot install");
        return false;
    }

    s_original = (ProcessTasks_t)Addresses::MonoScriptHost_ProcessTasks;

    // Detours explicitly supports attaching on top of a target another
    // module has already hooked (chaining) - if S3MM's own ProcessTasks
    // hook is already installed here, this attach still succeeds and both
    // hooks fire per frame, each calling through to whatever was there
    // before it. See addresses.h for the matching address-resolution side
    // of this same coexistence guarantee.
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original, DetourProcessTasks);
    LONG error = DetourTransactionCommit();

    if (error != NO_ERROR) {
        LOG_ERROR("[SimHeartbeatHook] Failed to install hook, error: " + std::to_string(error));
        s_original = nullptr;
        return false;
    }

    LOG_INFO("[SimHeartbeatHook] Hooked MonoScriptHost::ProcessTasks @ " + ToHex(Addresses::MonoScriptHost_ProcessTasks));
    return true;
}

} // namespace SimHeartbeatHook
} // namespace S3FC
