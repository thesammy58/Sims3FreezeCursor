#include "glib_hash_guard.h"
#include "../util/logger.h"
#include "../util/memory.h"
#include <Windows.h>
#include <detours/detours.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace S3FC {

namespace {

// TS3's statically-linked g_hash_table_lookup_extended. Confirmed by
// disassembly at 0x00E45A40:
//
//   56              push esi
//   8B 74 24 08     mov  esi,[esp+8]      ; arg1 = GHashTable*
//   85 F6           test esi,esi          ; g_return_val_if_fail(table != NULL)
//   ...
//   8B 06           mov  eax,[esi]        ; table->hash_func
//   8B 7E 04        mov  edi,[esi+4]      ; table->key_equal_func
//   8B 4E 08        mov  ecx,[esi+8]      ; table->buckets
//   F7 76 0C        div  eax,[esi+0Ch]    ; hash % table->size
//   FF D7           call edi              ; key_equal_func(node->key, key)  <-- the derailment
//
// Args are cdecl: (GHashTable*, key, gpointer* orig_key, gpointer* value),
// read at [esp+8]/[esp+10h]/[esp+18h]/[esp+1Ch] once the prologue's pushes
// are accounted for. Returns gboolean.
const uintptr_t kHashLookup = 0x00E45A40;
const unsigned char kHashLookupPrefix[] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x85, 0xF6 };

using HashLookup_t = int(__cdecl*)(void* table, void* key, void** outKey, void** outValue);
HashLookup_t s_original = nullptr;

// true  -> a corrupt table makes the lookup return "not found" instead of
//          calling through the garbage pointer. Mono's generic-instantiation
//          cache treats a miss as "create a new entry", which leaks an
//          instantiation but does not crash. There is no safe way to search a
//          table whose comparator is data.
// false -> log only, let the original run, and crash exactly as it does now.
const bool kBlockCorruptLookups = true;

uintptr_t s_ts3Base = 0;
uintptr_t s_ts3End = 0;

volatile LONG s_reportCount = 0;
volatile LONG s_blockedCount = 0;

// Per-thread nesting depth of the lookup, plus a process-wide high-water mark.
thread_local int t_depth = 0;
volatile LONG s_maxDepth = 0;

bool IsTs3Code(uintptr_t a) { return a >= s_ts3Base && a < s_ts3End; }

// POD-only frames for anything that dereferences the table - it may be freed.
bool ReadTableFuncs(const void* table, uintptr_t* hashFn, uintptr_t* equalFn) {
    __try {
        const uintptr_t* t = reinterpret_cast<const uintptr_t*>(table);
        *hashFn = t[0];
        *equalFn = t[1];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int SafeReadDwords(const void* p, uint32_t* out, int count) {
    __try {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(p);
        for (int i = 0; i < count; i++) { out[i] = src[i]; }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Resolve an address to "module+offset" without enumerating anything:
// VirtualQuery gives the allocation base, which for MEM_IMAGE is the HMODULE.
std::string DescribeAddr(uintptr_t a) {
    if (!a) return "";
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == 0) return " (unmapped)";
    if (mbi.Type != MEM_IMAGE) return " (not a module - heap/private)";

    char path[MAX_PATH] = {};
    if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH) == 0) return "";
    const char* f = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') f = p + 1;
    }
    return std::string(" (") + f + "+" + ToHex(a - (uintptr_t)mbi.AllocationBase) + ")";
}

void ReportCorruptTable(const void* table, const void* key, uintptr_t hashFn, uintptr_t equalFn, const void* retaddr, LONG n) {
    uint32_t raw[8] = {};
    const int got = SafeReadDwords(table, raw, 8);

    std::string msg = "[GLibHashGuard] *** CORRUPT GHashTable #" + std::to_string(n) + " ***";
    msg += " table=" + ToHex((uintptr_t)table) + DescribeAddr((uintptr_t)table);
    msg += " | hash_func=" + ToHex(hashFn) + (IsTs3Code(hashFn) ? " ok" : " BAD") + DescribeAddr(hashFn);
    msg += " | key_equal_func=" + ToHex(equalFn) + (IsTs3Code(equalFn) ? " ok" : " BAD") + DescribeAddr(equalFn);
    msg += " | key=" + ToHex((uintptr_t)key);
    msg += " | caller=" + ToHex((uintptr_t)retaddr) + DescribeAddr((uintptr_t)retaddr);

    msg += " | table[0..7]=";
    if (got) {
        for (int i = 0; i < got; i++) { msg += ToHex(raw[i]) + " "; }
    } else {
        msg += "<unreadable>";
    }
    LOG_ERROR(msg);
}

// HOOK - two guarded reads and two range compares on a very hot path.
int __cdecl HookedHashLookup(void* table, void* key, void** outKey, void** outValue) {
    if (table) {
        uintptr_t hashFn = 0, equalFn = 0;
        if (ReadTableFuncs(table, &hashFn, &equalFn) && (!IsTs3Code(hashFn) || !IsTs3Code(equalFn))) {
            const LONG n = InterlockedIncrement(&s_reportCount);
            if (n <= 5) { ReportCorruptTable(table, key, hashFn, equalFn, _ReturnAddress(), n); }

            if (kBlockCorruptLookups) {
                const LONG b = InterlockedIncrement(&s_blockedCount);
                if (b == 1) { LOG_ERROR("[GLibHashGuard] Dropping the lookup rather than calling through a data pointer. The game keeps running; further occurrences are counted, not logged."); }
                if (outKey) *outKey = nullptr;
                if (outValue) *outValue = nullptr;
                return 0; // gboolean FALSE - "key not found"
            }
        }
    }
    // Depth accounting around the real call. Two increments and a compare on a
    // hot path; the high-water mark is only touched when it actually rises.
    const int depth = ++t_depth;
    if (depth > s_maxDepth) { InterlockedExchange(&s_maxDepth, depth); }

    const int r = s_original(table, key, outKey, outValue);

    --t_depth;
    return r;
}

} // namespace

namespace GLibHashGuard {

bool Initialize() {
    Memory::ModuleInfo main = Memory::GetMainModule();
    s_ts3Base = main.base;
    s_ts3End = main.base + main.size;
    if (!s_ts3Base || s_ts3End <= s_ts3Base) {
        LOG_ERROR("[GLibHashGuard] Could not resolve the main module range - guard not installed");
        return false;
    }

    unsigned char* target = reinterpret_cast<unsigned char*>(kHashLookup);
    if (IsBadReadPtr(target, sizeof(kHashLookupPrefix))) {
        LOG_WARN("[GLibHashGuard] target unreadable at " + ToHex(kHashLookup) + " - skipped");
        return false;
    }
    if (memcmp(target, kHashLookupPrefix, sizeof(kHashLookupPrefix)) != 0) {
        LOG_WARN("[GLibHashGuard] byte check FAILED at " + ToHex(kHashLookup) + " - different TS3 build? skipped (hardcoded, version-specific address)");
        return false;
    }

    s_original = (HashLookup_t)kHashLookup;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)s_original, HookedHashLookup);
    LONG error = DetourTransactionCommit();

    if (error != NO_ERROR) {
        LOG_ERROR("[GLibHashGuard] Failed to install hook, error: " + std::to_string(error));
        s_original = nullptr;
        return false;
    }

    LOG_INFO("[GLibHashGuard] Watching g_hash_table_lookup_extended @ " + ToHex(kHashLookup) + " (TS3 code range " + ToHex(s_ts3Base) + ".." + ToHex(s_ts3End) + ", corrupt lookups " + (kBlockCorruptLookups ? "DROPPED" : "logged only") + ")");
    return true;
}

int CurrentDepth() { return t_depth; }
int MaxDepth() { return (int)s_maxDepth; }

} // namespace GLibHashGuard
} // namespace S3FC
