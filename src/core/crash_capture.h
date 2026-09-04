#pragma once
#include <Windows.h>

namespace S3FC {

// CRASH CAPTURE - process-wide Vectored Exception Handler that captures a
// rich, structured snapshot of any fatal-looking native fault (access
// violation, illegal instruction, stack overflow, ...) ANYWHERE in the
// process, and writes it to Sims3FreezeCursor.log before letting normal
// crash handling proceed untouched.
//
// Why this exists: post-mortem analysis of tonight's TS3 crashes (mini-
// dumps + WinDbg) kept running into the same wall - a mini-dump only
// captures memory near the FINAL fault site, but these crashes are
// execute-type NX faults reached only after the CPU has already been
// running garbage-as-instructions for a while (proven by a misaligned ESP
// at the fault - normal compiled PUSH/POP/CALL/RET always keeps it 4-byte
// aligned). By the time the process finally dies, the real corruption
// event already happened earlier and its evidence is usually gone.
// Capturing everything live, in-process, at the FIRST fault - full
// registers, a stack scan for pointers into known modules, an EBP chain
// walk, VirtualQuery region info, raw bytes around EIP - recovers evidence
// no post-mortem dump can.
//
// This is deliberately NOT S3MM's ExceptionHook (see S3MM's own
// core/exception_hook.cpp, whose VEH-installation and __try/__except-
// isolated-probe conventions this borrows). That handler narrowly targets
// crashes *inside* Mono's interp_exec_state dispatch loop and can attempt
// a surgical EIP-rewind recovery for one specific, signature-verified
// crash pattern. This handler has no such address range to scope to - the
// whole point is catching crashes we don't already understand - so it
// never attempts recovery and never touches ContextRecord. It only
// observes.
//
// ALWAYS returns EXCEPTION_CONTINUE_SEARCH. This is a passive second
// opinion, not a handler - the existing xcpt/WER crash pipeline still runs
// exactly as it always has, untouched, immediately afterward.
class CrashCapture {
  public:
    static CrashCapture& Get();

    // Installs the VEH. Safe to call once; idempotent. Call as early as
    // possible - right after Logger::Get().Initialize() in MainThread - so
    // the capture window covers as much of startup and this mod's own
    // subsequent initialization as possible.
    bool Initialize();
    void Shutdown();

    bool IsInstalled() const { return m_vehHandle != nullptr; }

  private:
    CrashCapture() = default;
    ~CrashCapture() = default;
    CrashCapture(const CrashCapture&) = delete;
    CrashCapture& operator=(const CrashCapture&) = delete;

    static LONG CALLBACK VectoredHandler(PEXCEPTION_POINTERS exInfo);

    PVOID m_vehHandle = nullptr;

    static volatile LONG s_inHandler;    // reentrancy guard - a fault while capturing bails immediately rather than recursing
    static volatile LONG s_captureCount; // caps FULL captures per process lifetime so a crash-loop can't spam the log/disk
};

} // namespace S3FC
