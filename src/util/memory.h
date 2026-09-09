#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <cstdint>

namespace S3FC {

// MEMORY UTILITIES - Pattern scanning. Trimmed from S3MM's util/memory.h:
// keeps only what this mod actually needs (ScanPattern, GetMainModule,
// IsGameProcess) and drops the co-resident-module snapshot / crash-log
// attribution helpers, which belong to S3MM's exception/VEH system that
// this standalone mod doesn't have.
namespace Memory {

// Pattern scanning with nibble-level wildcards. Pattern format: "55 8B EC ?? ?? 83".
// Supports: "FF" (exact), "??" (full wildcard), "?F" (low nibble), "F?" (high nibble).
// Returns 0 if not found.
uintptr_t ScanPattern(BYTE* start, size_t size, const char* pattern);

// Convenience: scan main module
uintptr_t ScanPattern(const char* pattern);

// Every direct `call rel32` (E8) in the main module whose computed target is
// `target`. Writes up to maxOut addresses into out and returns the TOTAL found,
// which may exceed maxOut.
//
// Two uses. It locates the instruction a call-site hook rewrites - and it is a
// free sanity check on an address resolved by pattern scan, because a function
// nothing calls is not the function you think you found.
size_t FindDirectCallSites(uintptr_t target, uintptr_t* out, size_t maxOut);

struct ModuleInfo {
    uintptr_t base;
    size_t size;
};

inline ModuleInfo GetMainModule() {
    HMODULE hModule = GetModuleHandle(nullptr);
    MODULEINFO mi = {};
    GetModuleInformation(GetCurrentProcess(), hModule, &mi, sizeof(mi));
    return {(uintptr_t)mi.lpBaseOfDll, mi.SizeOfImage};
}

// Check if the host process is a known Sims 3 game executable (TS3.exe or TS3W.exe).
// Returns false if running inside a launcher or other non-game process.
inline bool IsGameProcess() {
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0) { return false; }
    const char* fname = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') fname = p + 1;
    }
    return (_stricmp(fname, "TS3.exe") == 0 || _stricmp(fname, "TS3W.exe") == 0);
}

} // namespace Memory
} // namespace S3FC
