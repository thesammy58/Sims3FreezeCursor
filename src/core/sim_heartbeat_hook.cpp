#include "sim_heartbeat_hook.h"
#include "addresses.h"
#include "freeze_watchdog.h"
#include "../util/config.h"
#include "../util/logger.h"
#include <Windows.h>
#include <detours/detours.h>
#include <intrin.h>
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
    SimHeartbeat();

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
            LOG_ERROR("[SimHeartbeatHook] Implausible pHost on entry: " + ToHex((uintptr_t)pHost) + " (sighting " + std::to_string(n) + ") - passed through unchanged; a [CrashCapture] block usually follows.");
        }
    }

    s_original(pHost);
}

// INERT DETOUR (SimHook=2) - a control, not a feature. Identical placement,
// identical trampoline, identical patched bytes at the target; the only
// difference is that it does no work of its own. If a stutter survives this,
// it is caused by the function being detoured, not by anything this mod
// computes - which points at Detours' trampoline, the modified code page, or
// whatever the game does differently once its prologue has moved.
static void __fastcall DetourProcessTasksInert(void* pHost) {
    s_original(pHost);
}

// PROFILING DETOUR (SimHook=3) - inert, plus an RDTSC bracket around the
// pass-through. It answers the question nobody has actually measured: on a
// machine where this hook makes the game unplayable, how long does a call to
// the real ProcessTasks take, and what share of the wall clock is that?
//
// Three outcomes, all useful. A large share means the time is going into the
// function or the trampoline. A tiny share means it is not going there at
// all - the hook would then be a trigger rather than a cost, changing the
// timing of something else in the process rather than consuming anything
// itself, and no amount of trimming our side would help.
//
// Single writer (the sim thread), so plain accumulation is fine. The 64-bit
// read on the watchdog thread can tear, which costs at most one bad sample
// out of a ten-second window.
static volatile uint64_t s_profCycles = 0;
static volatile uint32_t s_profCalls = 0;

static void __fastcall DetourProcessTasksProfiled(void* pHost) {
    const uint64_t start = __rdtsc();
    s_original(pHost);
    s_profCycles += (__rdtsc() - start);
    ++s_profCalls;
}

// COEXISTENCE - yield address resolution to whoever was here first.
//
// This mod and S3MM both locate MonoScriptHost::ProcessTasks by scanning for
// the SAME byte pattern, and both fall back to the SAME documented constant
// when the scan finds nothing. Detouring the function overwrites the very
// bytes that pattern matches on.
//
// So whichever mod installs first silently destroys the other's scan. The
// loser falls back to 0x00D7FC60, which is stale for this game build, and
// writes a Detours jump into code that is not the function - which is how
// execution ends up running bytes that were never instructions.
//
// Measured here: we installed at :31.324, S3MM's ScriptHostHook at :31.372.
// We won the race by 48ms, S3MM lost its scan, and the game died at world
// load. Either mod ALONE is fine, because its scan sees a clean image.
//
// The fix is ordering, not chaining - Detours chains happily, what it cannot
// do is un-break somebody else's pattern scan. Addresses::Initialize() already
// resolves our address early against a clean image, so we keep the correct
// address; we simply defer INSTALLING until any co-resident Mono modder has
// had its turn, then attach on top.
bool CoResidentMonoModderPresent() {
    return GetModuleHandleA("Sims3MonoModder.asi") != nullptr || GetModuleHandleA("Sims3MonoModder.dll") != nullptr;
}

bool LooksHooked(uintptr_t addr) {
    if (!addr || IsBadReadPtr(reinterpret_cast<void*>(addr), 2)) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
    return p[0] == 0xE9 || p[0] == 0xEB || (p[0] == 0xFF && p[1] == 0x25);
}

void YieldToCoResidentHooks(uintptr_t target) {
    if (!CoResidentMonoModderPresent()) return;

    LOG_INFO("[SimHeartbeatHook] Co-resident Mono mod detected - deferring install so its address scan sees unpatched bytes");

    const DWORD kTimeoutMs = 10000;
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < kTimeoutMs) {
        if (LooksHooked(target)) {
            LOG_INFO("[SimHeartbeatHook] Co-resident hook detected at " + ToHex(target) + " after " + std::to_string(GetTickCount() - start) + "ms - chaining");
            return;
        }
        Sleep(50);
    }
    LOG_WARN("[SimHeartbeatHook] No co-resident hook at " + ToHex(target) + " after " + std::to_string(kTimeoutMs) + "ms - installing. A mod resolving this symbol later may fall back to a stale address.");
}

namespace SimHeartbeatHook {

bool Initialize() {
    if (s_original) return true; // already installed

    if (!Addresses::MonoScriptHost_ProcessTasks) {
        LOG_ERROR("[SimHeartbeatHook] MonoScriptHost::ProcessTasks address unresolved - cannot install");
        return false;
    }

    s_original = (ProcessTasks_t)Addresses::MonoScriptHost_ProcessTasks;

    // Let any co-resident Mono modder resolve and install first - see the note
    // above YieldToCoResidentHooks. Safe to block here: FreezeWatchdog is
    // already running on its own thread and simply reports "no heartbeat yet"
    // until we attach.
    YieldToCoResidentHooks(Addresses::MonoScriptHost_ProcessTasks);

    // Detours explicitly supports attaching on top of a target another
    // module has already hooked (chaining) - if S3MM's own ProcessTasks
    // hook is already installed here, this attach still succeeds and both
    // hooks fire per frame, each calling through to whatever was there
    // before it. See addresses.h for the matching address-resolution side
    // of this same coexistence guarantee.
    const int mode = Config::SimHookMode();
    const bool inert = (mode == 2);
    const bool profiled = (mode == 3);
    if (inert) { LOG_WARN("[Config] SimHook=2 - installing an inert detour; the heartbeat will never fire and stall detection is disabled"); }
    if (profiled) { LOG_WARN("[Config] SimHook=3 - installing a profiling detour; the heartbeat will never fire and stall detection is disabled"); }

    void(__fastcall * chosen)(void*) = DetourProcessTasks;
    if (inert) { chosen = DetourProcessTasksInert; }
    if (profiled) { chosen = DetourProcessTasksProfiled; }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original, chosen);
    LONG error = DetourTransactionCommit();

    if (error != NO_ERROR) {
        LOG_ERROR("[SimHeartbeatHook] Failed to install hook, error: " + std::to_string(error));
        s_original = nullptr;
        return false;
    }

    LOG_INFO(std::string("[SimHeartbeatHook] Hooked MonoScriptHost::ProcessTasks @ ") + ToHex(Addresses::MonoScriptHost_ProcessTasks) + (inert ? " (inert)" : profiled ? " (profiling)" : ""));
    return true;
}

uint64_t ProfiledCycles() { return s_profCycles; }
uint32_t ProfiledCalls() { return s_profCalls; }

} // namespace SimHeartbeatHook
} // namespace S3FC
