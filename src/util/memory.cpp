#include "memory.h"
#include <cctype>
#include <vector>

namespace S3FC {
namespace Memory {

// PATTERN SCANNER - Nibble-level wildcards for flexible matching. Ported
// verbatim from S3MM's util/memory.cpp - this logic is not S3MM-specific,
// it's a general byte-pattern scanner.

uintptr_t ScanPattern(BYTE* start, size_t size, const char* pattern) {
    std::vector<BYTE> bytes;
    std::vector<BYTE> masks; // 0xFF = exact, 0xF0 = high nibble, 0x0F = low nibble, 0x00 = wildcard

    auto parseNibble = [](char c) -> BYTE {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return 0;
    };

    const char* p = pattern;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        char hiChar = *p;
        char loChar = *(p + 1);

        if (!loChar || loChar == ' ') { return 0; }

        bool hiWild = (hiChar == '?');
        bool loWild = (loChar == '?');
        bool hiHex = isxdigit(static_cast<unsigned char>(hiChar)) != 0;
        bool loHex = isxdigit(static_cast<unsigned char>(loChar)) != 0;

        if ((!hiWild && !hiHex) || (!loWild && !loHex)) { return 0; }

        BYTE byteVal = 0;
        BYTE maskVal = 0;

        if (hiHex) {
            byteVal |= (parseNibble(hiChar) << 4);
            maskVal |= 0xF0;
        }
        if (loHex) {
            byteVal |= parseNibble(loChar);
            maskVal |= 0x0F;
        }

        bytes.push_back(byteVal);
        masks.push_back(maskVal);
        p += 2;
    }

    if (bytes.empty()) { return 0; }

    for (size_t i = 0; i <= size - bytes.size(); i++) {
        bool found = true;
        for (size_t j = 0; j < bytes.size(); j++) {
            if ((start[i + j] & masks[j]) != (bytes[j] & masks[j])) {
                found = false;
                break;
            }
        }
        if (found) { return reinterpret_cast<uintptr_t>(start + i); }
    }

    return 0;
}

size_t FindDirectCallSites(uintptr_t target, uintptr_t* out, size_t maxOut) {
    const ModuleInfo mod = GetMainModule();
    if (!mod.base || mod.size < 5) { return 0; }

    const BYTE* base = reinterpret_cast<const BYTE*>(mod.base);
    size_t count = 0;

    // The whole image rather than just its code sections. A four-byte
    // displacement that happens to compute to exactly this address out of
    // unrelated data is vanishingly unlikely, and if one ever did the caller's
    // "exactly one site" requirement rejects the whole thing rather than
    // patching the wrong place - the failure mode is a refusal, not a crash.
    for (size_t i = 0; i + 5 <= mod.size; ++i) {
        if (base[i] != 0xE8) { continue; }
        const int32_t rel = *reinterpret_cast<const int32_t*>(base + i + 1);
        const uintptr_t site = mod.base + i;
        const uintptr_t dest = static_cast<uintptr_t>(static_cast<intptr_t>(site + 5) + rel);
        if (dest == target) {
            if (count < maxOut) { out[count] = site; }
            ++count;
        }
    }

    return count;
}

uintptr_t ScanPattern(const char* pattern) {
    ModuleInfo mod = GetMainModule();
    return ScanPattern(reinterpret_cast<BYTE*>(mod.base), mod.size, pattern);
}

} // namespace Memory
} // namespace S3FC
