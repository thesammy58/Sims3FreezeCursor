#include "paths.h"
#include <Windows.h>
#include <Shlobj.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace S3FC {
namespace Paths {

namespace {

// Resolve the localized game folder name (e.g. "Die Sims 3") by scanning
// the exe's own string table, the same way the game does internally.
// Ported verbatim from S3MM's util/config_paths.cpp - this is a property of
// TS3.exe's own string table layout, not of either mod, so both mods
// resolve it identically. String table is organized in blocks of 100 per
// locale: base+0 = locale code (e.g. "fr-fr"), base+1 = display name with
// TM, base+2 = folder name.
std::wstring ResolveLocalizedGameFolder() {
    HMODULE exe = GetModuleHandle(NULL);
    if (!exe) return L"The Sims 3";

    wchar_t sysLocale[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(sysLocale, LOCALE_NAME_MAX_LENGTH) == 0) { return L"The Sims 3"; }
    _wcslwr_s(sysLocale);

    std::wstring sysLang(sysLocale);
    auto hyphen = sysLang.find(L'-');
    if (hyphen != std::wstring::npos) sysLang.resize(hyphen);

    wchar_t blockLocale[16];
    wchar_t gameName[MAX_PATH];
    UINT prefixMatch = 0;

    for (UINT base = 1000; base < 3600; base += 100) {
        if (LoadStringW(exe, base, blockLocale, 16) > 0) {
            _wcslwr_s(blockLocale);
            if (wcscmp(sysLocale, blockLocale) == 0 && LoadStringW(exe, base + 2, gameName, MAX_PATH) > 0) { return gameName; }
            if (prefixMatch == 0 && wcsncmp(sysLang.c_str(), blockLocale, sysLang.size()) == 0) { prefixMatch = base; }
        }
    }
    if (prefixMatch != 0 && LoadStringW(exe, prefixMatch + 2, gameName, MAX_PATH) > 0) { return gameName; }
    return L"The Sims 3";
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

} // namespace

const std::wstring& GetDirectory() {
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t docPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docPath))) { dir = std::wstring(docPath) + L"\\Electronic Arts\\" + ResolveLocalizedGameFolder() + L"\\Sims3FreezeCursor\\"; }
    }
    return dir;
}

std::string GetLogPath() {
    return WideToUtf8(GetDirectory()) + "Sims3FreezeCursor.log";
}

bool EnsureDirectoryExists() {
    const std::wstring& dir = GetDirectory();
    if (dir.empty()) return false;

    std::error_code ec;
    fs::create_directories(dir, ec);
    return !ec && fs::exists(dir);
}

} // namespace Paths
} // namespace S3FC
