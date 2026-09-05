#include "addresses.h"
#include "../util/memory.h"
#include "../util/logger.h"
#include <Windows.h>
#include <cstring>

namespace S3FC {
namespace Addresses {

uintptr_t MonoScriptHost_ProcessTasks = 0;

namespace {

// Pattern and fallback address ported byte-for-byte from S3MM's own
// core/addresses.cpp (same symbol, same documented Steam-build address) -
// this isn't something to rediscover independently, it's a fact about
// TS3.exe's own compiled code.
constexpr const char* kProcessTasksPattern = "55 8B EC 83 E4 F8 83 EC 50 53 55 56 8B D9 8B 83 ?? ?? ?? ?? 8D B3 ?? ?? ?? ?? 57 50 8B CE";
constexpr uintptr_t kProcessTasksFallback = 0x00D7FC60;

// Is this address actually inside the running executable's image? The fallback
// is a constant recorded from one specific build; on any other module - a
// different game version, or the 23KB TS3W.exe GameRunner launcher stub if an
// ASI loader ever injected there - it is just an address, and detouring it
// would write a jump into unmapped or unrelated memory.
bool IsWithinMainModule(uintptr_t addr) {
    const Memory::ModuleInfo main = Memory::GetMainModule();
    if (!main.base || !main.size) return false;
    return addr >= main.base && addr < main.base + main.size;
}

// Does the fallback address still look like the function it is supposed to be?
// Either the original prologue (the pattern's fixed head, 55 8B EC 83 E4 F8)
// or a detour jump written over it by a mod that got there first. Anything
// else means the constant is wrong for this build and must not be hooked.
bool FallbackLooksPlausible(uintptr_t addr) {
    if (!IsWithinMainModule(addr)) return false;
    if (IsBadReadPtr(reinterpret_cast<void*>(addr), 6)) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
    if (p[0] == 0xE9 || p[0] == 0xEB || (p[0] == 0xFF && p[1] == 0x25)) return true; // already hooked
    static const unsigned char kHead[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };
    return memcmp(p, kHead, sizeof(kHead)) == 0;
}

// Does this address hold a patched prologue rather than the original code?
// Detours overwrites an entry point with a relative jump (E9 rel32) or, for a
// hot-patchable target, an indirect one (FF 25 abs32); EB is the short form.
// Seeing one of those at the documented address means the function is exactly
// where it should be and somebody simply hooked it first.
bool LooksAlreadyHooked(uintptr_t addr) {
    if (IsBadReadPtr(reinterpret_cast<void*>(addr), 2)) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
    return p[0] == 0xE9 || p[0] == 0xEB || (p[0] == 0xFF && p[1] == 0x25);
}

} // namespace

bool Initialize() {
    const uintptr_t scanned = Memory::ScanPattern(kProcessTasksPattern);
    const bool fallbackHooked = LooksAlreadyHooked(kProcessTasksFallback);

    // CASE 1 - scan agrees with the documented address. Normal, unhooked game.
    if (scanned == kProcessTasksFallback) {
        MonoScriptHost_ProcessTasks = scanned;
        LOG_INFO("[Addresses] MonoScriptHost::ProcessTasks resolved via pattern scan @ " + ToHex(scanned));
        return true;
    }

    // CASE 2 - no match at all. Either a mod patched the prologue first, or this
    // is a different game build. The fallback covers the first case only, so it
    // is verified before use rather than trusted: TS3.exe is SecuROM-packed and
    // its addresses demonstrably shift between builds (S3MM's own constants are
    // all stale for this one), so a wrong constant here would mean detouring
    // whatever happens to live at that address.
    if (scanned == 0) {
        if (!FallbackLooksPlausible(kProcessTasksFallback)) {
            LOG_ERROR("[Addresses] Pattern scan found no match AND the documented address " + ToHex(kProcessTasksFallback) + " holds neither the expected prologue nor a detour jump - this is a different game build. Refusing to hook a guessed address; freeze detection is unavailable this run.");
            return false;
        }
        MonoScriptHost_ProcessTasks = kProcessTasksFallback;
        LOG_WARN("[Addresses] Pattern scan found no match (another mod hooked it first) - using documented address " + ToHex(kProcessTasksFallback) + ", verified present");
        return true;
    }

    // CASE 3 - the scan matched, but somewhere ELSE. This is the dangerous one,
    // and it is what actually happened here: S3MM loads first and detours the
    // real prologue, so the pattern stops matching at 0x00D7FC60, the scan keeps
    // walking, and its eight wildcard bytes let it land a false positive 0x3C0
    // further on. The old code treated any non-zero scan as success and
    // detoured that address - writing a jump into the wrong target, which is
    // precisely how execution ended up running bytes that were never
    // instructions.
    //
    // If the documented address is sitting behind a detour jump, the function
    // has not moved and the scan is simply wrong: trust the documented address.
    if (fallbackHooked) {
        MonoScriptHost_ProcessTasks = kProcessTasksFallback;

        LOG_WARN("[Addresses] Pattern scan matched " + ToHex(scanned) + " but the documented address " + ToHex(kProcessTasksFallback) + " already holds a detour jump - the function has not moved, the scan hit a false positive " + ToHex(scanned - kProcessTasksFallback) + " bytes past it. Using the documented address; hooking the scan result would inject a jump into the wrong code.");
        return true;
    }

    // Whatever we settled on, never hand a detour an address outside the image.
    if (!IsWithinMainModule(scanned)) {
        LOG_ERROR("[Addresses] Pattern scan matched " + ToHex(scanned) + ", which is outside the main module - refusing to hook it.");
        return false;
    }

    // CASE 4 - they disagree and the documented address is NOT hooked, so this
    // build simply has the function somewhere else. INFO, not WARN: the scan is
    // the authoritative mechanism and the constant is a recorded value that goes
    // stale with every game build - on this install every one of S3MM's recorded
    // addresses is shifted too. A disagreement here is the expected, healthy
    // outcome, not a fault, and warning about it on every launch would be
    // permanent noise about a non-problem. The delta is still logged, because it
    // is exactly the detail that identified the co-resident hook-ordering bug.
    MonoScriptHost_ProcessTasks = scanned;
    LOG_INFO("[Addresses] MonoScriptHost::ProcessTasks resolved via pattern scan @ " + ToHex(scanned) + " (documented address " + ToHex(kProcessTasksFallback) + " differs by " + ToHex(scanned - kProcessTasksFallback) + " and is not hooked - this build's addresses are shifted from the recorded ones, which is normal; the scan is authoritative)");
    return true;
}

} // namespace Addresses
} // namespace S3FC
