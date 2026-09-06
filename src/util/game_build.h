#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

namespace S3FC {

// GAME BUILD - which copy of The Sims 3 this is.
//
// The three distributions are different builds, not different wrappers around
// one build. Steam ships 1.67.2.024037 and names the real 14MB executable
// TS3W.exe; the EA App ships 1.69.x as TS3.exe and leaves a 23KB TS3W.exe
// launcher stub beside it. Function addresses move between them - a bug
// report that does not say which build it came from cannot be read.
//
// The PE TimeDateStamp identifies the build exactly and costs a header read.
// The stamps are S3SS's (patch_system.h), which is where the version names
// come from too, so reports line up between the two mods.
namespace GameBuild {

struct KnownBuild {
    uint32_t stamp;
    const char* name;
};

inline const KnownBuild kKnownBuilds[] = {
    {0x52D872DA, "Retail 1.67.2.024002"},
    {0x52DEC247, "Steam 1.67.2.024037"},
    {0x6707155C, "EA App 1.69.47.024017"},
    {0x568D4BAC, "EA App 1.69.43.024017"},
};

inline uint32_t TimeDateStamp() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) return 0;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->FileHeader.TimeDateStamp;
}

inline std::string Name() {
    const uint32_t stamp = TimeDateStamp();
    for (const auto& b : kKnownBuilds) {
        if (b.stamp == stamp) return b.name;
    }
    return "unrecognised build";
}

} // namespace GameBuild
} // namespace S3FC
