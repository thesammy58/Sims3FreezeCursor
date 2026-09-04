#pragma once

namespace S3FC {

// CONTEXT GUARD - watches whole-CPU-context restores for a bad target.
//
// Across eight captures the crash state is rigidly deterministic:
//
//   EBP  always 0x010D6088          EBX/ESI always exactly 0x170 apart
//   EDI  always 0x4???0048          ECX always 0xFFFFFFFF
//   EDX  always 0x76A700F3          EAX always ~0x0F4-0x0FE million
//   ESP  always off the thread's stack
//   EIP  always in FREE or RESERVE memory - nothing ever executed there
//
// ECX and EDX are VOLATILE. Nothing preserves them across a call, so they
// should hold whatever the last operation left. Bit-identical values across
// eight separate runs cannot come from execution - they have to be LOADED,
// together, from one structure. A jmp_buf does not save ECX/EDX; a CONTEXT
// does. NtContinue and RtlRestoreContext restore the entire register file
// from one in one shot, which is the only mechanism that accounts for all of
// it at once - including ESP and EIP changing together with no instruction
// executing in between.
//
// Earlier SetThreadContext/GetThreadContext breakpoints never fired because
// those are the wrong entry points. These are the right ones, and unlike
// every hardcoded TS3 address in this investigation they are named ntdll
// exports, so no byte-matching or pattern scan is involved.
//
// Purely observational: the context is inspected and reported, never altered,
// and the call is always forwarded.
namespace ContextGuard {

bool Initialize();

} // namespace ContextGuard
} // namespace S3FC
