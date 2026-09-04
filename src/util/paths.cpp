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

namespace {

// Folder this .asi was loaded from, with trailing separator. Resolved from our
// own module handle rather than the exe's, so it follows the .asi wherever it
// is installed.
std::wstring ModuleDirectory() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self) || !self) { return {}; }

    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0) return {};

    std::wstring p(path);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return p.substr(0, slash + 1);
}

// Can we actually create files here? Game\Bin usually lives under Program
// Files, where an unelevated process cannot write - so this is a real
// possibility, not a formality.
bool DirectoryIsWritable(const std::wstring& dir) {
    if (dir.empty()) return false;
    const std::wstring probe = dir + L"s3fc_write_test.tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h); // FILE_FLAG_DELETE_ON_CLOSE removes it
    return true;
}

std::wstring DocumentsDirectory() {
    wchar_t docPath[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docPath))) return {};
    return std::wstring(docPath) + L"\\Electronic Arts\\" + ResolveLocalizedGameFolder() + L"\\Sims3FreezeCursor\\";
}

} // namespace

// Prefers the folder the .asi itself sits in (Game\Bin), which keeps the log
// next to the mod where anyone reporting a problem will look for it. Falls
// back to Documents when that folder is read-only - a normal outcome for an
// unelevated game under Program Files, not an error.
const std::wstring& GetDirectory() {
    static std::wstring dir;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        const std::wstring here = ModuleDirectory();
        if (DirectoryIsWritable(here)) {
            dir = here;
        } else {
            dir = DocumentsDirectory();
        }
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
