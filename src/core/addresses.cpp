#include "addresses.h"
#include "../util/memory.h"
#include "../util/logger.h"
#include <Windows.h>

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

    // CASE 2 - no match at all. The prologue was patched by a mod that loaded
    // first, which is what the fallback exists for.
    if (scanned == 0) {
        MonoScriptHost_ProcessTasks = kProcessTasksFallback;
        LOG_WARN("[Addresses] Pattern scan found no match (another mod hooked it first, or a different game build) - using documented address " + ToHex(kProcessTasksFallback));
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

    // CASE 4 - they disagree and the documented address is NOT hooked. Most
    // likely a genuinely different game build where the function really did
    // move, so the scan is the better source. Loud, because it is unproven.
    MonoScriptHost_ProcessTasks = scanned;
    LOG_WARN("[Addresses] Pattern scan matched " + ToHex(scanned) + ", which disagrees with the documented address " + ToHex(kProcessTasksFallback) + ", and that address is not hooked. Assuming a different game build and trusting the scan - if the sim heartbeat misbehaves, this is the first thing to check.");
    return true;
}

} // namespace Addresses
} // namespace S3FC
