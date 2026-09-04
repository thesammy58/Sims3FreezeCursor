#include "crash_capture.h"
#include <cstdint>
#include "../util/logger.h"
#include "glib_hash_guard.h"
#include <Psapi.h>
#include <intrin.h>
#include <string>
#include <sstream>
#include <cstdio>
#include <cstring>

namespace S3FC {

volatile LONG CrashCapture::s_inHandler = 0;
volatile LONG CrashCapture::s_captureCount = 0;

namespace {

// MODULE SNAPSHOT - refreshed at the start of every full capture (not
// cached across captures) so it reflects whatever was loaded at the time
// of THIS fault, including anything that loaded late.
struct ModuleRange {
    uintptr_t base;
    uintptr_t end;
    char name[64];
};

ModuleRange s_modules[256];
int s_moduleCount = 0;

void RefreshModuleSnapshot() {
    HMODULE hMods[1024];
    DWORD needed = 0;
    s_moduleCount = 0;
    if (!EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &needed)) return;

    int count = (int)(needed / sizeof(HMODULE));
    if (count > 256) count = 256;

    for (int i = 0; i < count; i++) {
        MODULEINFO mi = {};
        if (!GetModuleInformation(GetCurrentProcess(), hMods[i], &mi, sizeof(mi))) continue;

        char path[MAX_PATH] = {};
        GetModuleFileNameA(hMods[i], path, MAX_PATH);
        const char* fname = path;
        for (const char* p = path; *p; ++p) {
            if (*p == '\\' || *p == '/') fname = p + 1;
        }

        ModuleRange& r = s_modules[s_moduleCount++];
        r.base = (uintptr_t)mi.lpBaseOfDll;
        r.end = r.base + mi.SizeOfImage;
        strncpy_s(r.name, fname, sizeof(r.name) - 1);
    }
}

// Returns "ModuleName+0xOFFSET" or "" if addr isn't inside any loaded module.
std::string ResolveModule(uintptr_t addr) {
    for (int i = 0; i < s_moduleCount; i++) {
        if (addr >= s_modules[i].base && addr < s_modules[i].end) { return std::string(s_modules[i].name) + "+" + ToHex(addr - s_modules[i].base); }
    }
    return "";
}

// SAFE PROBES - POD-only frames, nothing here may construct a C++ object,
// because __try needs an object-free frame to unwind cleanly through if a
// SECOND fault happens while we're reading crash-time memory that may
// itself be garbage (same convention S3MM's exception_hook.cpp uses for
// TryQuarantineFaultingBakedChain - see that file's comment on why).
bool SafeReadDword(const void* addr, DWORD* out) {
    __try {
        *out = *reinterpret_cast<const DWORD*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

size_t SafeReadBytes(const void* addr, void* out, size_t len) {
    __try {
        memcpy(out, addr, len);
        return len;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// FORMATTING HELPERS
const char* ExceptionCodeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "IN_PAGE_ERROR";
    case EXCEPTION_STACK_OVERFLOW:
        return "STACK_OVERFLOW";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "NONCONTINUABLE_EXCEPTION";
    case 0xC000041D:
        return "FATAL_USER_CALLBACK_EXCEPTION"; // seen wrapping a fault inside a kernel-invoked WNDPROC callback in tonight's captures
    default:
        return "UNKNOWN";
    }
}

// Only these are worth a full capture - everything else (ordinary C++/CLR
// exceptions, debugger thread-naming exceptions, Mono's own internal SEH-
// based control flow) is common, benign, first-chance noise that would
// otherwise flood this log on every normal frame.
bool IsCaptureWorthy(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case 0xC000041D:
        return true;
    default:
        return false;
    }
}

std::string ProtectToString(DWORD protect) {
    DWORD base = protect & 0xFF;
    const char* s = "?";
    switch (base) {
    case PAGE_NOACCESS:
        s = "NOACCESS";
        break;
    case PAGE_READONLY:
        s = "READONLY";
        break;
    case PAGE_READWRITE:
        s = "READWRITE";
        break;
    case PAGE_WRITECOPY:
        s = "WRITECOPY";
        break;
    case PAGE_EXECUTE:
        s = "EXECUTE";
        break;
    case PAGE_EXECUTE_READ:
        s = "EXECUTE_READ";
        break;
    case PAGE_EXECUTE_READWRITE:
        s = "EXECUTE_READWRITE";
        break;
    case PAGE_EXECUTE_WRITECOPY:
        s = "EXECUTE_WRITECOPY";
        break;
    }
    std::string result = s;
    if (protect & PAGE_GUARD) result += "|GUARD";
    if (protect & PAGE_NOCACHE) result += "|NOCACHE";
    return result;
}

// DumpRegion - VirtualQuery is safe on any address, wild pointers included:
// it queries the OS's view of the address space, it never dereferences the
// address itself. No __try needed here.
void DumpRegion(const char* label, uintptr_t addr) {
    if (!addr) {
        LOG_ERROR(std::string("[CrashCapture]   ") + label + " = 0x00000000 (null)");
        return;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0) {
        LOG_ERROR(std::string("[CrashCapture]   ") + label + " = " + ToHex(addr) + " (VirtualQuery failed)");
        return;
    }

    const char* typeStr = (mbi.Type == MEM_IMAGE) ? "IMAGE" : (mbi.Type == MEM_MAPPED) ? "MAPPED" : (mbi.Type == MEM_PRIVATE) ? "PRIVATE" : "?";
    const char* stateStr = (mbi.State == MEM_COMMIT) ? "COMMIT" : (mbi.State == MEM_RESERVE) ? "RESERVE" : (mbi.State == MEM_FREE) ? "FREE" : "?";
    std::string modInfo = ResolveModule(addr);

    LOG_ERROR(std::string("[CrashCapture]   ") + label + " = " + ToHex(addr) + (modInfo.empty() ? "" : " (" + modInfo + ")") + " | region base=" + ToHex((uintptr_t)mbi.BaseAddress) + " size=" + ToHex(mbi.RegionSize) + " state=" + stateStr + " type=" + typeStr + " protect=" + ProtectToString(mbi.Protect));
}

void DumpBytesHex(const char* label, uintptr_t addr, size_t count) {
    if (count > 128) count = 128;
    BYTE buf[128];
    size_t got = SafeReadBytes((const void*)addr, buf, count);
    if (got == 0) {
        LOG_ERROR(std::string("[CrashCapture]   ") + label + " @ " + ToHex(addr) + ": <unreadable>");
        return;
    }

    std::ostringstream ss;
    ss << "[CrashCapture]   " << label << " @ " << ToHex(addr) << " (" << got << " bytes): ";
    for (size_t i = 0; i < got; i++) {
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X ", buf[i]);
        ss << hex;
    }
    LOG_ERROR(ss.str());
}

// THREAD STACK BOUNDS - straight out of the TEB (NT_TIB on x86: FS:[0x04] is
// StackBase, the high end; FS:[0x08] is StackLimit, the lowest committed
// page). A VEH runs on the faulting thread itself, so this is that thread's
// real stack, whatever nonsense ESP currently holds.
void GetThreadStackBounds(uintptr_t* base, uintptr_t* limit) {
    *base = (uintptr_t)__readfsdword(0x04);
    *limit = (uintptr_t)__readfsdword(0x08);
}

// Generic range scan for dwords that resolve into a loaded module. Walks low
// to high; because the stack grows down, that reports the MOST RECENT frames
// first when handed the real stack bounds.
void ScanRangeForModulePointers(uintptr_t from, uintptr_t to, const char* label, uintptr_t relativeTo, int maxHits) {
    if (!from || to <= from) return;
    int hits = 0;
    for (uintptr_t addr = from; addr < to && hits < maxHits; addr += 4) {
        DWORD val = 0;
        if (!SafeReadDword((const void*)addr, &val)) continue;
        std::string mod = ResolveModule(val);
        if (!mod.empty()) {
            LOG_ERROR(std::string("[CrashCapture]     [") + label + "+" + ToHex(addr - relativeTo) + "] = " + ToHex(val) + "  " + mod);
            hits++;
        }
    }
    if (hits == 0) { LOG_ERROR("[CrashCapture]     (no hits)"); }
}

// STACK SCAN - same technique as the stackscan.py used earlier tonight
// against the mini-dumps, just live and complete instead of limited to
// whatever a mini-dump happened to capture near ESP.
void ScanStackForModulePointers(uintptr_t esp) {
    if (!esp) return;
    LOG_ERROR("[CrashCapture]   Stack scan (ESP..ESP+0x1000, dwords resolving into a loaded module):");
    ScanRangeForModulePointers(esp, esp + 0x1000, "ESP", esp, 40);
}

// EBP CHAIN WALK - best-effort; optimized/FPO'd frames will break this
// early, which is itself informative (matches what we saw live in WinDbg
// tonight - FRAME_ONE_INVALID on a real corruption crash).
void WalkEbpChain(uintptr_t ebp) {
    if (!ebp) return;
    LOG_ERROR("[CrashCapture]   EBP chain walk:");
    uintptr_t cur = ebp;
    for (int depth = 0; depth < 20; depth++) {
        DWORD savedEbp = 0, retAddr = 0;
        if (!SafeReadDword((const void*)cur, &savedEbp) || !SafeReadDword((const void*)(cur + 4), &retAddr)) {
            LOG_ERROR("[CrashCapture]     depth " + std::to_string(depth) + ": EBP=" + ToHex(cur) + " <unreadable>");
            break;
        }
        std::string mod = ResolveModule(retAddr);
        LOG_ERROR("[CrashCapture]     depth " + std::to_string(depth) + ": EBP=" + ToHex(cur) + " retaddr=" + ToHex(retAddr) + (mod.empty() ? "" : "  " + mod));
        if (!savedEbp || savedEbp <= cur || (savedEbp - cur) > 0x100000) break;
        cur = savedEbp;
    }
}

// POD only - this crosses a __try boundary.
struct CaptureCtx {
    uintptr_t eip;
    uintptr_t esp;
    uintptr_t ebp;
    uintptr_t faultAddr;
    uintptr_t stackBase;
    uintptr_t stackLimit;
    bool espOnStack;
};

void DumpDetails(const CaptureCtx* c) {
    DumpRegion("EIP region", c->eip);
    DumpRegion("EBP region", c->ebp);
    if (c->faultAddr) { DumpRegion("Fault address region", c->faultAddr); }

    DumpBytesHex("Bytes at EIP", c->eip, 32);
    if (c->esp) { DumpBytesHex("Bytes at ESP", c->esp, 32); }

    ScanStackForModulePointers(c->esp);

    // When ESP has been trashed, the call chain that actually led here is on
    // the thread's real stack and nowhere near ESP. Walk it from the lowest
    // committed page upward, so the most recent frames report first.
    if (!c->espOnStack && c->stackLimit && c->stackBase > c->stackLimit) {
        LOG_ERROR("[CrashCapture]   REAL-STACK scan (TEB limit..base, most recent frames first) - this is the call chain ESP-based scans have been missing:");
        ScanRangeForModulePointers(c->stackLimit, c->stackBase, "stack", c->stackLimit, 60);
    }

    WalkEbpChain(c->ebp);
}

// Kept out of SafeRunDetails: LOG_ERROR builds a std::string, and MSVC rejects
// __try in any function that has objects requiring unwinding (C2712).
void ReportDetailFault() {
    LOG_ERROR("[CrashCapture]   (detail dump faulted - capture truncated here; the registers above are still valid)");
}

// Static, not a stack local: when ESP is broken the handler's own frame lives
// on whatever sliver of memory ESP points at, and the worker thread below has
// to read this from somewhere that is definitely valid. The s_inHandler guard
// means only one capture is ever in flight, so a single instance is enough.
CaptureCtx g_cap;

DWORD WINAPI DetailThreadProc(LPVOID param);

// POD-only frame, so __try has nothing to unwind. A secondary fault while
// dumping is swallowed and the capture closes cleanly instead of losing every
// line that had not been flushed yet.
void SafeRunDetails(const CaptureCtx* c) {
    __try {
        DumpDetails(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReportDetailFault();
    }
}

DWORD WINAPI DetailThreadProc(LPVOID param) {
    SafeRunDetails(reinterpret_cast<const CaptureCtx*>(param));
    return 0;
}

} // namespace

CrashCapture& CrashCapture::Get() {
    static CrashCapture instance;
    return instance;
}

bool CrashCapture::Initialize() {
    if (m_vehHandle) return true;

    m_vehHandle = AddVectoredExceptionHandler(1, VectoredHandler);
    if (m_vehHandle) {
        LOG_INFO("[CrashCapture] Installed process-wide VEH crash capture");
    } else {
        LOG_WARN("[CrashCapture] AddVectoredExceptionHandler failed");
    }
    return m_vehHandle != nullptr;
}

void CrashCapture::Shutdown() {
    if (m_vehHandle) {
        RemoveVectoredExceptionHandler(m_vehHandle);
        m_vehHandle = nullptr;
        LOG_INFO("[CrashCapture] Removed VEH crash capture");
    }
}

// VECTORED HANDLER - see crash_capture.h for the full design rationale.
// Fires for EVERY exception in the process; the IsCaptureWorthy filter and
// reentrancy guard keep the common case (the overwhelming majority of VEH
// calls are benign first-chance exceptions) essentially free.
LONG CALLBACK CrashCapture::VectoredHandler(PEXCEPTION_POINTERS exInfo) {
    if (InterlockedCompareExchange(&s_inHandler, 1, 0) != 0) { return EXCEPTION_CONTINUE_SEARCH; }

    DWORD code = exInfo->ExceptionRecord->ExceptionCode;
    if (!IsCaptureWorthy(code)) {
        InterlockedExchange(&s_inHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG count = InterlockedIncrement(&s_captureCount);
    if (count > 5) {
        if (count == 6) { LOG_ERROR("[CrashCapture] Further full captures suppressed this run (5 already recorded above)"); }
        InterlockedExchange(&s_inHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    RefreshModuleSnapshot();

    PEXCEPTION_RECORD er = exInfo->ExceptionRecord;
    PCONTEXT ctx = exInfo->ContextRecord;
    uintptr_t eip = ctx->Eip;
    uintptr_t esp = ctx->Esp;
    uintptr_t ebp = ctx->Ebp;
    uintptr_t faultAddr = (er->NumberParameters >= 2) ? (uintptr_t)er->ExceptionInformation[1] : 0;

    LOG_ERROR("=====================================================");
    LOG_ERROR("[CrashCapture] FAULT #" + std::to_string(count) + ": " + ExceptionCodeName(code) + " (" + ToHex(code) + ")");
    LOG_ERROR("[CrashCapture]   ThreadId=" + std::to_string(GetCurrentThreadId()) + "  GetLastError=" + std::to_string(GetLastError()));

    std::string exAddrMod = ResolveModule((uintptr_t)er->ExceptionAddress);
    LOG_ERROR("[CrashCapture]   ExceptionAddress=" + ToHex((uintptr_t)er->ExceptionAddress) + (exAddrMod.empty() ? "" : " (" + exAddrMod + ")"));

    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 1) {
        const char* kind = (er->ExceptionInformation[0] == 8) ? "EXECUTE" : (er->ExceptionInformation[0] == 1) ? "WRITE" : "READ";
        LOG_ERROR(std::string("[CrashCapture]   AV type: ") + kind + " @ " + ToHex(faultAddr));
    }

    LOG_ERROR("[CrashCapture]   EAX=" + ToHex(ctx->Eax) + " EBX=" + ToHex(ctx->Ebx) + " ECX=" + ToHex(ctx->Ecx) + " EDX=" + ToHex(ctx->Edx));
    LOG_ERROR("[CrashCapture]   ESI=" + ToHex(ctx->Esi) + " EDI=" + ToHex(ctx->Edi) + " ESP=" + ToHex(esp) + " EBP=" + ToHex(ebp));
    LOG_ERROR("[CrashCapture]   EIP=" + ToHex(eip) + " EFlags=" + ToHex(ctx->EFlags));

    if (esp % 4 != 0) {
        LOG_ERROR("[CrashCapture]   *** ESP IS MISALIGNED (not a multiple of 4) *** - no compiled PUSH/POP/CALL/RET sequence can produce this. Note this does NOT by itself mean garbage was executed: where EIP lands in RESERVE or unmapped memory, nothing ran there and ESP was ALREADY broken when control transferred - which is what a longjmp into a bad jmp_buf looks like, since it restores ESP and EIP together.");
    }

    // THE REAL STACK - every scan before this one started from ESP, and on
    // every capture so far ESP has been misaligned garbage, so those scans
    // were reading whatever ESP happened to point at rather than the call
    // chain. The faulting thread's actual stack is still intact; the TEB says
    // where it is.
    uintptr_t stackBase = 0, stackLimit = 0;
    GetThreadStackBounds(&stackBase, &stackLimit);
    const bool espOnStack = (stackLimit && esp >= stackLimit && esp < stackBase);
    LOG_ERROR("[CrashCapture]   Thread stack: limit=" + ToHex(stackLimit) + " base=" + ToHex(stackBase) + "  ESP is " + (espOnStack ? "INSIDE the real stack" : "*** OUTSIDE the real stack ***"));

    // Depth of the hash-lookup recursion this thread was sitting in. The
    // real-stack scans show that cycle running ~169KB deep at every crash; if
    // this is bounded and similar run to run it is legitimate nesting, and if
    // it climbs without limit it is the bug.
    LOG_ERROR("[CrashCapture]   GLib hash-lookup recursion: depth on THIS thread=" + std::to_string(GLibHashGuard::CurrentDepth()) + ", high-water this run=" + std::to_string(GLibHashGuard::MaxDepth()));

    g_cap.eip = eip;
    g_cap.esp = esp;
    g_cap.ebp = ebp;
    g_cap.faultAddr = faultAddr;
    g_cap.stackBase = stackBase;
    g_cap.stackLimit = stackLimit;
    g_cap.espOnStack = espOnStack;

    // A VEH runs on the FAULTING thread, so when ESP is off the rails this
    // handler is executing on it too - with however little valid memory
    // happens to sit under that pointer. That is what truncated the 11:26:11
    // and 12:07:20 captures partway through the dump, and it costs us exactly
    // the evidence we need. Hand the work to a thread with a real 1MB stack
    // and wait for it.
    if (espOnStack) {
        SafeRunDetails(&g_cap);
    } else {
        HANDLE h = CreateThread(nullptr, 0, DetailThreadProc, &g_cap, 0, nullptr);
        if (h) {
            WaitForSingleObject(h, 15000);
            CloseHandle(h);
        } else {
            SafeRunDetails(&g_cap); // no thread - try in place and hope
        }
    }

    LOG_ERROR("=====================================================");

    InterlockedExchange(&s_inHandler, 0);
    return EXCEPTION_CONTINUE_SEARCH; // always - this is an observer, never a handler
}

} // namespace S3FC
