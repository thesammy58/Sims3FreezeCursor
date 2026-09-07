#pragma once
#include "paths.h"
#include <Windows.h>
#include <string>

namespace S3FC {

// SETTINGS - read from Sims3FreezeCursor.ini beside the .asi, if one exists.
// Every key has a working default, so a normal install ships no ini at all.
// The file is here so a player hitting a problem can switch one piece off at
// a time and report which one mattered, without needing a custom build. That
// is the same bisect that isolated the ProcessTasks hook-ordering bug.
//
//   [Sims3FreezeCursor]
//   CrashCapture=0
//   SimHook=1
//   CursorHook=1
//   HeartbeatStatsSeconds=0
namespace Config {

inline std::wstring IniPath() { return Paths::GetDirectory() + L"Sims3FreezeCursor.ini"; }

inline int ReadInt(const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(L"Sims3FreezeCursor", key, fallback, IniPath().c_str()));
}

// SimHook is a mode, not a flag:
//   0 - not installed at all
//   1 - installed; the detour stamps the heartbeat (normal operation)
//   2 - installed; the detour does nothing but call straight through
//
// Mode 2 exists because arithmetic ruled out the obvious explanation. The
// detour's own work cannot cost what the reporter is losing: the sim pump
// runs at a few hundred to ~1200 calls a second, so even the most expensive
// clock read measured (1144ns on the HPET) accounts for about a millisecond
// per second. Whatever the hook costs on that machine, it is not the work
// inside it - and mode 2 is what separates "this function is hooked at all"
// from "this hook does something".
inline int SimHookMode() {
    static const int v = ReadInt(L"SimHook", 1);
    return v;
}
inline bool SimHookEnabled() { return SimHookMode() != 0; }
inline bool CursorHookEnabled() {
    static const bool v = ReadInt(L"CursorHook", 1) != 0;
    return v;
}

// The VEH crash capture is a debugging tool, not a feature, and it is off
// unless asked for. A registered vectored handler is called for EVERY
// exception raised anywhere in the process, on every thread, before any
// __except gets a look - and TS3 with Mono raises a great many that it
// handles itself as a matter of course. That dispatch is not free, and none
// of it buys a player anything: the capture only ever writes to a log.
inline bool CrashCaptureEnabled() {
    static const bool v = ReadInt(L"CrashCapture", 0) != 0;
    return v;
}

// 0 = off. Any other value writes a sim-pump throughput line every N seconds.
// Worth setting when investigating a report, because the watchdog otherwise
// logs only state transitions: a game that stutters badly while the sim keeps
// up produces a completely empty log, which is indistinguishable from a
// healthy one.
inline int HeartbeatStatsSeconds() {
    static const int v = ReadInt(L"HeartbeatStatsSeconds", 0);
    return v;
}

} // namespace Config
} // namespace S3FC
