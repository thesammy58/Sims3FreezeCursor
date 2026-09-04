#include "context_guard.h"
#include "../util/logger.h"
#include <Windows.h>
#include <detours/detours.h>
#include <intrin.h>
#include <cstdint>
#include <string>

namespace S3FC {

namespace {

using NtContinue_t = LONG(NTAPI*)(PCONTEXT, BOOLEAN);
using RtlRestoreContext_t = void(NTAPI*)(PCONTEXT, PEXCEPTION_RECORD);

NtContinue_t s_origNtContinue = nullptr;
RtlRestoreContext_t s_origRtlRestoreContext = nullptr;

volatile LONG s_reportCount = 0;

// Cheap test first: the TEB gives this thread's real stack with two FS reads
// and no syscall. A context whose Esp is off its own stack is the signature we
// are hunting, and in normal operation it never happens - so the expensive
// checks only run once something is already wrong.
bool EspOnOwnStack(uintptr_t esp) {
    const uintptr_t base = (uintptr_t)__readfsdword(0x04);  // NT_TIB StackBase
    const uintptr_t limit = (uintptr_t)__readfsdword(0x08); // NT_TIB StackLimit
    if (!base || !limit || base <= limit) return true;      // can't tell - assume fine
    return esp >= limit && esp < base;
}

bool ReadContextRegs(const CONTEXT* ctx, uintptr_t* eip, uintptr_t* esp) {
    __try {
        *eip = (uintptr_t)ctx->Eip;
        *esp = (uintptr_t)ctx->Esp;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyContext(const CONTEXT* src, CONTEXT* dst) {
    __try {
        *dst = *src;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string DescribeAddr(uintptr_t a) {
    if (!a) return " (null)";
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == 0) return " (unmapped)";

    const char* state = (mbi.State == MEM_COMMIT) ? "COMMIT" : (mbi.State == MEM_RESERVE) ? "RESERVE" : "FREE";
    if (mbi.Type != MEM_IMAGE) { return std::string(" (") + state + ", not a module)"; }

    char path[MAX_PATH] = {};
    if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH) == 0) return std::string(" (") + state + ")";
    const char* f = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') f = p + 1;
    }
    return std::string(" (") + f + "+" + ToHex(a - (uintptr_t)mbi.AllocationBase) + ")";
}

void ReportBadContext(const CONTEXT* c, const void* retaddr, const char* site, LONG n) {
    CONTEXT copy = {};
    if (!CopyContext(c, &copy)) {
        LOG_ERROR(std::string("[ContextGuard] *** BAD CONTEXT via ") + site + " #" + std::to_string(n) + " *** - the CONTEXT itself is unreadable at " + ToHex((uintptr_t)c));
        return;
    }

    const uintptr_t base = (uintptr_t)__readfsdword(0x04);
    const uintptr_t limit = (uintptr_t)__readfsdword(0x08);

    LOG_ERROR("=====================================================");
    LOG_ERROR(std::string("[ContextGuard] *** BAD CONTEXT RESTORE via ") + site + " #" + std::to_string(n) + " ***");
    LOG_ERROR("[ContextGuard]   caller=" + ToHex((uintptr_t)retaddr) + DescribeAddr((uintptr_t)retaddr));
    LOG_ERROR("[ContextGuard]   CONTEXT at " + ToHex((uintptr_t)c) + DescribeAddr((uintptr_t)c));
    LOG_ERROR("[ContextGuard]   -> Eip=" + ToHex(copy.Eip) + DescribeAddr(copy.Eip));
    LOG_ERROR("[ContextGuard]   -> Esp=" + ToHex(copy.Esp) + "  this thread's stack is " + ToHex(limit) + ".." + ToHex(base));
    LOG_ERROR("[ContextGuard]   -> Eax=" + ToHex(copy.Eax) + " Ebx=" + ToHex(copy.Ebx) + " Ecx=" + ToHex(copy.Ecx) + " Edx=" + ToHex(copy.Edx));
    LOG_ERROR("[ContextGuard]   -> Esi=" + ToHex(copy.Esi) + " Edi=" + ToHex(copy.Edi) + " Ebp=" + ToHex(copy.Ebp) + " EFlags=" + ToHex(copy.EFlags));
    LOG_ERROR("[ContextGuard]   -> ContextFlags=" + ToHex(copy.ContextFlags) + " SegCs=" + ToHex(copy.SegCs) + " SegSs=" + ToHex(copy.SegSs));
    LOG_ERROR("[ContextGuard]   Forwarding anyway - the crash that follows is this restore landing.");
    LOG_ERROR("=====================================================");
}

// Returns true when this context would put the thread somewhere impossible.
//
// Deliberately syscall-free: NtContinue sits on the normal exception-dispatch
// path, so anything run here runs constantly. The Esp test is two FS reads and
// two compares, and an Esp off its own stack IS the signature we are hunting -
// it has been present in all eight captures. Vetting Eip properly needs
// VirtualQuery, so that is left to the report path, where it runs at most five
// times per process.
bool ContextLooksBad(const CONTEXT* c) {
    uintptr_t eip = 0, esp = 0;
    if (!ReadContextRegs(c, &eip, &esp)) return true; // unreadable CONTEXT is itself the bug
    if (!EspOnOwnStack(esp)) return true;             // the signature
    if (eip < 0x10000) return true;                   // null page - free, no syscall needed
    return false;
}

LONG NTAPI HookedNtContinue(PCONTEXT ctx, BOOLEAN testAlert) {
    if (ctx && ContextLooksBad(ctx)) {
        const LONG n = InterlockedIncrement(&s_reportCount);
        if (n <= 5) { ReportBadContext(ctx, _ReturnAddress(), "NtContinue", n); }
    }
    return s_origNtContinue(ctx, testAlert);
}

void NTAPI HookedRtlRestoreContext(PCONTEXT ctx, PEXCEPTION_RECORD rec) {
    if (ctx && ContextLooksBad(ctx)) {
        const LONG n = InterlockedIncrement(&s_reportCount);
        if (n <= 5) { ReportBadContext(ctx, _ReturnAddress(), "RtlRestoreContext", n); }
    }
    s_origRtlRestoreContext(ctx, rec);
}

} // namespace

namespace ContextGuard {

bool Initialize() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) {
        LOG_ERROR("[ContextGuard] ntdll not loaded?! - guard not installed");
        return false;
    }

    s_origNtContinue = (NtContinue_t)GetProcAddress(ntdll, "NtContinue");
    s_origRtlRestoreContext = (RtlRestoreContext_t)GetProcAddress(ntdll, "RtlRestoreContext");

    if (!s_origNtContinue && !s_origRtlRestoreContext) {
        LOG_ERROR("[ContextGuard] neither NtContinue nor RtlRestoreContext could be resolved");
        return false;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    int hooks = 0;
    if (s_origNtContinue) {
        DetourAttach(&(PVOID&)s_origNtContinue, HookedNtContinue);
        hooks++;
    }
    if (s_origRtlRestoreContext) {
        DetourAttach(&(PVOID&)s_origRtlRestoreContext, HookedRtlRestoreContext);
        hooks++;
    }
    LONG error = DetourTransactionCommit();

    if (error != NO_ERROR) {
        LOG_ERROR("[ContextGuard] Failed to install hooks, error: " + std::to_string(error));
        s_origNtContinue = nullptr;
        s_origRtlRestoreContext = nullptr;
        return false;
    }

    LOG_INFO("[ContextGuard] Watching " + std::to_string(hooks) + " context-restore entry point(s): NtContinue=" + ToHex((uintptr_t)s_origNtContinue) + " RtlRestoreContext=" + ToHex((uintptr_t)s_origRtlRestoreContext));
    return true;
}

} // namespace ContextGuard
} // namespace S3FC
