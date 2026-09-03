#include "addresses.h"
#include "../util/memory.h"
#include "../util/logger.h"

namespace S3FC {
namespace Addresses {

uintptr_t MonoScriptHost_ProcessTasks = 0;

namespace {

// Pattern and fallback address ported byte-for-byte from S3MM's own
// core/addresses.cpp (same symbol, same documented Steam-build address) -
// this isn't something to rediscover independently, it's a fact about
// TS3.exe's own compiled code.
constexpr const char* kProcessTasksPattern = "55 8B EC 83 E4 F8 83 EC 50 53 55 56 8B D9 8B 83 ?? ?? ?? ?? 8D B3 ?? ?? ?? ?? 57 50 8B CE";
constexpr uintptr_t kProcessTasksFallback = 0x00D7FC60;

} // namespace

bool Initialize() {
    MonoScriptHost_ProcessTasks = Memory::ScanPattern(kProcessTasksPattern);
    if (MonoScriptHost_ProcessTasks) {
        LOG_INFO("[Addresses] MonoScriptHost::ProcessTasks resolved via pattern scan @ " + ToHex(MonoScriptHost_ProcessTasks));
        return true;
    }

    LOG_WARN("[Addresses] MonoScriptHost::ProcessTasks pattern scan found no match (different game build, or another mod already hooked it first) - using fallback address " + ToHex(kProcessTasksFallback));
    MonoScriptHost_ProcessTasks = kProcessTasksFallback;
    return true;
}

} // namespace Addresses
} // namespace S3FC
