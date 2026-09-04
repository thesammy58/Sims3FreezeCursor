#include "paths.h"
#include <Windows.h>
#include <Shlobj.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace S3FC {
namespace Paths {

namespace {

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

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

// Last resort only. Deliberately NOT Documents: that path needed the game's
// localized folder name (scraped from TS3's own string table) and
// CSIDL_PERSONAL, which lands somewhere unexpected under OneDrive
// redirection. Both are more fragile than the Game\Bin write they were
// meant to protect against. LOCALAPPDATA needs neither and is always
// writable.
std::wstring FallbackDirectory() {
    wchar_t appData[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData))) return {};
    return std::wstring(appData) + L"\\Sims3FreezeCursor\\";
}

} // namespace

// The log lives next to the .asi (Game\Bin), where ASI mods conventionally
// put theirs and where anyone reporting a problem will look. Game\Bin is
// normally writable in practice; the fallback exists only for locked-down
// installs and is LOCALAPPDATA rather than Documents, so no locale lookup or
// Documents redirection is involved.
const std::wstring& GetDirectory() {
    static std::wstring dir;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        const std::wstring here = ModuleDirectory();
        if (DirectoryIsWritable(here)) {
            dir = here;
        } else {
            dir = FallbackDirectory();
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
