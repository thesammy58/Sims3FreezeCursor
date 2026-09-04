#include "sim_heartbeat_hook.h"
#include "addresses.h"
#include "freeze_watchdog.h"
#include "../util/logger.h"
#include <Windows.h>
#include <detours/detours.h>
#include <cstdint>
#include <string>

namespace S3FC {

// MonoScriptHost::ProcessTasks - __fastcall(pHost in ECX), per-frame task
// pump inside Simulate. Calling convention and role confirmed against
// S3MM's own core/script_host_hook.cpp, which hooks the identical function.
using ProcessTasks_t = void(__fastcall*)(void* pHost);
static ProcessTasks_t s_original = nullptr;

// PHOST PLAUSIBILITY - cheap enough to run on a per-frame path: a real
// MonoScriptHost is a 4-byte-aligned, user-space heap address. This is NOT
// a full validation, just enough to catch 0xFFFFFFFF, null, and small
// integers - which is exactly what the crash captures keep showing.
static bool IsPlausibleHostPointer(void* p) {
    uintptr_t v = (uintptr_t)p;
    if (v < 0x00010000 || v >= 0x7FFF0000) return false; // null page, or sentinel/kernel range
    if (v & 0x3) return false;                           // misaligned - never a real object
    return true;
}

// Rate-limited: this detour runs once per simulation frame, so an
// unconditional log would flood the file within seconds.
static volatile LONG s_badHostCount = 0;

static void __fastcall DetourProcessTasks(void* pHost) {
    // Stamped at entry, not after the call returns: if a Task inside
    // Simulate() later hangs without yielding, Simulate() never returns and
    // this detour is never re-entered next frame either way, so the
    // heartbeat correctly goes stale in both cases. See freeze_watchdog.h.
    FreezeWatchdog::Get().OnSimHeartbeat();

    // BAD-THIS TRIPWIRE - every crash captured so far bottoms out with
    // ECX = 0xFFFFFFFF and EBP left pointing at a TS3.exe vtable in .rdata.
    // ECX is the __fastcall `this` for THIS function, so that fingerprint
    // says a virtual dispatch is happening on a destroyed/poisoned host
    // object. Logging it here answers the one question the post-mortem
    // captures cannot: whether pHost is ALREADY bad when it reaches this
    // hook, or only becomes bad further down the chain (S3MM's own detour
    // on this same function, or the real ProcessTasks itself). Purely
    // observational - the call is always passed through unchanged.
    if (!IsPlausibleHostPointer(pHost)) {
        LONG n = InterlockedIncrement(&s_badHostCount);
        if (n <= 5) {
            LOG_ERROR("[SimHeartbeatHook] *** IMPLAUSIBLE pHost on entry: " + ToHex((uintptr_t)pHost) + " (sighting " + std::to_string(n) + ") - this is the bad `this` the crash captures bottom out in. Passing through unchanged; expect a [CrashCapture] block shortly after.");
        }
    }

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
