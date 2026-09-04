#pragma once

namespace S3FC {

// GLIB HASH GUARD - validates TS3's GLib hash-table lookups before they call
// through the table's own function pointers.
//
// Every crash in this investigation ends the same way: the CPU executing heap
// data as instructions. Live debugging traced the derailment to one
// instruction - `call edi` at TS3+0xA45A94, inside TS3's statically-linked
// g_hash_table_lookup_extended. EDI there is loaded as [table+4], the table's
// key_equal_func. In a healthy call it holds a real TS3 function (0x00E45E20
// was observed). At every captured crash it held a Mono-heap pointer instead
// (0x414C0048, 0x437C0048, 0x424D0048, 0x40780048, 0x41FC0048 - all in the
// 0x40-0x43M range, none of them code). So the table's function pointers are
// garbage and the lookup calls straight into data.
//
// A WinDbg conditional breakpoint on that instruction proved the theory
// unusable in practice - the lookup runs constantly during a world load, and
// trapping to the debugger per hit slowed the game ~100x. This does the same
// check in-process for the cost of two pointer reads and two comparisons.
namespace GLibHashGuard {

// Byte-verifies the target before hooking; a mismatched TS3 build is skipped
// rather than patched blindly.
bool Initialize();

} // namespace GLibHashGuard
} // namespace S3FC
