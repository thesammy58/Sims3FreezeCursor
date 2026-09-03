#pragma once
#include <cstdint>

namespace S3FC {

// ADDRESSES - Trimmed from S3MM's core/addresses.h/.cpp down to the single
// symbol this mod actually needs: MonoScriptHost::ProcessTasks, the
// per-frame managed-task pump inside MonoScriptHost::Simulate that
// FreezeWatchdog's heartbeat is built on (see freeze_watchdog.h). S3MM
// resolves dozens of symbols through a batched pattern-scan system; this
// mod only needs one, so a direct scan is simpler and sufficient.
namespace Addresses {

extern uintptr_t MonoScriptHost_ProcessTasks;

// Resolves MonoScriptHost_ProcessTasks. Tries a byte-pattern scan of the
// main module first; falls back to the hardcoded Steam-build address if the
// scan doesn't find a match. That fallback path also covers a specific
// coexistence case: if another native mod (e.g. Sims3MonoModder) is ALSO
// installed and loaded first, it will have already hooked this same
// function via MS Detours, which overwrites the function's opening bytes
// with a jump - so the ORIGINAL byte pattern won't match anymore, even
// though the function's address itself hasn't moved. Falling back to the
// known address in that case still lands correctly, and Detours is
// explicitly designed to let a second hook attach on top of a first one
// (see sim_heartbeat_hook.cpp) - so this mod works whether it loads before
// or after S3MM.
bool Initialize();

} // namespace Addresses
} // namespace S3FC
