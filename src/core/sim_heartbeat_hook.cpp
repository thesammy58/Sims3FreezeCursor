#include "sim_heartbeat_hook.h"
#include "addresses.h"
#include "freeze_watchdog.h"
#include "../util/config.h"
#include "../util/memory.h"
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

// CALL-SITE HOOK - rewrite the one call instruction instead of the function.
//
// Suggested by Just Harry, and better than it first looks. A prologue detour
// has to relocate the target's opening instructions into a trampoline and
// satisfy the ABI around them. Rewriting the four displacement bytes of the
// single `call` that reaches the function does none of that, and leaves the
// target's own bytes exactly as the game shipped them.
//
// Two consequences beyond the saved work. Any other mod that finds this
// function by scanning for its prologue still finds it - that is the whole
// coexistence problem (see COEXISTENCE below), removed at the root rather than worked
// around by waiting for the other mod to go first. And on a build where
// patching that prologue is somehow catastrophic, this never touches it.
//
// On Steam 1.67 the call site is a single instruction:
//
//   00D81946:  8B CF           mov  ecx, edi          ; this
//   00D81948:  E8 13 E3 FF FF  call 00D7FC60          ; the five bytes
//   00D8194D:  8D 4C 24 18     lea  ecx, [esp+18h]
//
// `mov ecx, edi` immediately before it is the __fastcall/thiscall `this`, so
// the rewritten call still lands on a plain void __fastcall(void*) that cleans
// no stack - which means the detour bodies above serve as thunks unchanged,
// with s_original pointing at the real function rather than a trampoline.
static uintptr_t s_callSite = 0;
static LONG s_originalDisplacement = 0;

static bool InstallCallSiteHook(uintptr_t target, void(__fastcall* thunk)(void*)) {
    uintptr_t sites[8] = {};
    const size_t found = Memory::FindDirectCallSites(target, sites, 8);

    LOG_INFO("[SimHeartbeatHook] Direct call sites targeting " + ToHex(target) + ": " + std::to_string(found));
    for (size_t i = 0; i < found && i < 8; ++i) { LOG_INFO("[SimHeartbeatHook]   call at " + ToHex(sites[i])); }

    if (found == 0) {
        // Worth saying plainly, because it questions the address rather than
        // the technique: nothing in the image calls this directly.
        LOG_WARN("[SimHeartbeatHook] Nothing calls " + ToHex(target) + " directly - it is reached indirectly, or it is not a function entry");
        return false;
    }
    if (found != 1) {
        LOG_WARN("[SimHeartbeatHook] " + std::to_string(found) + " call sites; rewriting one would miss the others");
        return false;
    }

    const uintptr_t site = sites[0];
    LONG* displacement = reinterpret_cast<LONG*>(site + 1);
    const LONG rewritten = static_cast<LONG>(reinterpret_cast<uintptr_t>(thunk) - (site + 5));

    DWORD oldProtect = 0;
    if (!VirtualProtect(displacement, sizeof(LONG), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("[SimHeartbeatHook] VirtualProtect failed at " + ToHex(site) + ", GetLastError=" + std::to_string(GetLastError()));
        return false;
    }

    // One locked four-byte write. x86 keeps a locked operation atomic even
    // when it straddles an alignment boundary - and this one does, since the
    // displacement sits one byte into the instruction - so a thread executing
    // this call while it is being patched sees either the old target or the
    // new one, never four bytes of each. Thread suspension is what a prologue
    // detour needs to get the same guarantee.
    s_originalDisplacement = InterlockedExchange(displacement, rewritten);

    DWORD restored = 0;
    VirtualProtect(displacement, sizeof(LONG), oldProtect, &restored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);

    s_callSite = site;
    LOG_INFO("[SimHeartbeatHook] Rewrote the call at " + ToHex(site) + " (target's own bytes left untouched)");
    return true;
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

    const int mode = Config::SimHookMode();
    const bool inert = (mode == 2);
    const bool profiled = (mode == 3);
    if (inert) { LOG_WARN("[Config] SimHook=2 - installing an inert hook; the heartbeat will never fire and stall detection is disabled"); }
    if (profiled) { LOG_WARN("[Config] SimHook=3 - installing a profiling hook; the heartbeat will never fire and stall detection is disabled"); }

    void(__fastcall * chosen)(void*) = DetourProcessTasks;
    if (inert) { chosen = DetourProcessTasksInert; }
    if (profiled) { chosen = DetourProcessTasksProfiled; }

    // CALL SITE, OR NOTHING. Rewriting the one call that reaches this function
    // leaves the function itself as the game shipped it, so nothing else's
    // pattern scan is disturbed and no trampoline exists to go wrong.
    //
    // There is deliberately no automatic fallback to the prologue detour. One
    // build in the wild became unplayable under it and nothing we found
    // explains why, so falling back would mean quietly choosing a
    // mechanism known to have ruined someone's game, on a build nobody has
    // tested. A mod that does nothing and says so is the better failure.
    // HookMethod=2 still asks for the old method explicitly.
    const int method = Config::HookMethod();
    if (method != 2) {
        if (InstallCallSiteHook(Addresses::MonoScriptHost_ProcessTasks, chosen)) {
            LOG_INFO(std::string("[SimHeartbeatHook] Hooked MonoScriptHost::ProcessTasks @ ") + ToHex(Addresses::MonoScriptHost_ProcessTasks) + " via its call site" + (inert ? " (inert)" : profiled ? " (profiling)" : ""));
            return true;
        }
        LOG_ERROR("[SimHeartbeatHook] This game build has no single call site to rewrite - not installing. Freeze detection is off for this session. Set HookMethod=2 in Sims3FreezeCursor.ini to use the older prologue detour instead.");
        s_original = nullptr;
        return false;
    }

    LOG_WARN("[SimHeartbeatHook] HookMethod=2 - using the prologue detour by request");

    // Only the detour path needs to wait. A call-site rewrite leaves the
    // prologue intact, so there is nobody to yield to.
    YieldToCoResidentHooks(Addresses::MonoScriptHost_ProcessTasks);

    // Detours explicitly supports attaching on top of a target another
    // module has already hooked (chaining) - if S3MM's own ProcessTasks
    // hook is already installed here, this attach still succeeds and both
    // hooks fire per frame, each calling through to whatever was there
    // before it. See addresses.h for the matching address-resolution side
    // of this same coexistence guarantee.
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
