#pragma once

namespace S3FC {

// SIM HEARTBEAT HOOK - Detours MonoScriptHost::ProcessTasks (the per-frame
// managed-task pump inside MonoScriptHost::Simulate) purely to stamp
// FreezeWatchdog's heartbeat on entry. Always calls through to the real
// function afterward - never skipped, never short-circuited - so this mod
// has zero effect on game behavior by itself, matching S3MM's own detour's
// documented purely-observational contract on the same function (see
// addresses.h for how this hook stays correct even if S3MM's own copy of
// this same hook is also installed).
namespace SimHeartbeatHook {

// Requires Addresses::Initialize() to have resolved
// Addresses::MonoScriptHost_ProcessTasks first. Unlike CursorHook, this
// doesn't need a retry loop: the target is a native function inside
// TS3.exe's own image, resolvable as soon as the main module is loaded -
// i.e. immediately, with no dependency on Mono having started yet. Safe to
// call once at startup.
bool Initialize();

} // namespace SimHeartbeatHook
} // namespace S3FC
