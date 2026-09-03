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

uintptr_t ScanPattern(const char* pattern) {
    ModuleInfo mod = GetMainModule();
    return ScanPattern(reinterpret_cast<BYTE*>(mod.base), mod.size, pattern);
}

} // namespace Memory
} // namespace S3FC
