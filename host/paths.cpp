// Paths: UTF-8 in the host, wide (W) functions to Windows, the system's code page to the game (see host.h).
#include "host.h"
#include <cstdio>

namespace host {

std::wstring Wide(const std::string &utf8) {
    if (utf8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &w[0], n);
    return w;
}

std::string Utf8Of(const wchar_t *wide, int n) {
    if (!wide || n == 0 || (n < 0 && !*wide)) return std::string();
    int m = WideCharToMultiByte(CP_UTF8, 0, wide, n, nullptr, 0, nullptr, nullptr);
    std::string s(m > 0 ? m : 0, '\0');
    if (m > 0) WideCharToMultiByte(CP_UTF8, 0, wide, n, &s[0], m, nullptr, nullptr);
    if (n < 0 && !s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::string AcpToUtf8(const char *acp) {
    if (!acp || !*acp) return std::string();
    int n = MultiByteToWideChar(CP_ACP, 0, acp, -1, nullptr, 0);
    if (n <= 0) return std::string();
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, acp, -1, &w[0], n);
    return Utf8Of(w.c_str());
}

namespace {
// The code page's spelling of a wide string; *lossless false when a character has none.
std::string Acp(const std::wstring &w, bool *lossless) {
    *lossless = true;
    if (w.empty()) return std::string();
    BOOL lost = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, &lost);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, &lost);
    *lossless = !lost && n > 0;
    return s;
}
} // namespace

std::string AcpOf(const std::string &utf8) {
    bool lossless;
    return Acp(Wide(utf8), &lossless);
}

std::string AcpPath(const std::string &utf8, bool *spelled) {
    std::wstring w = Wide(utf8);
    bool lossless;
    std::string s = Acp(w, &lossless);
    if (spelled) *spelled = true;
    if (lossless) return s;
    DWORD n = GetShortPathNameW(w.c_str(), nullptr, 0);
    if (n) {
        std::wstring shortPath(n, L'\0');
        DWORD got = GetShortPathNameW(w.c_str(), &shortPath[0], n);
        if (got && got < n) {
            shortPath.resize(got);
            bool ok;
            std::string t = Acp(shortPath, &ok);
            if (ok) return t;
        }
    }
    if (spelled) *spelled = false;
    return s;
}

std::string ModulePath(HMODULE module) {
    std::wstring w(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(module, &w[0], static_cast<DWORD>(w.size()));
        if (!n) return std::string();
        if (n < w.size()) { w.resize(n); return Utf8Of(w.c_str()); }
        if (w.size() >= 32768) return std::string();
        w.resize(w.size() * 2);
    }
}

FILE *FileOpen(const std::string &path, const char *mode) {
    std::wstring m;
    for (const char *p = mode; *p; ++p) m += static_cast<wchar_t>(*p);
    return _wfopen(Wide(path).c_str(), m.c_str());
}

DWORD PathAttributes(const std::string &path) { return GetFileAttributesW(Wide(path).c_str()); }
bool PathExists(const std::string &path) { return PathAttributes(path) != INVALID_FILE_ATTRIBUTES; }
bool PathAttributesEx(const std::string &path, WIN32_FILE_ATTRIBUTE_DATA *out) {
    return GetFileAttributesExW(Wide(path).c_str(), GetFileExInfoStandard, out) != FALSE;
}
bool MakeDir(const std::string &path) {
    return CreateDirectoryW(Wide(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
bool RemoveDir(const std::string &path) { return RemoveDirectoryW(Wide(path).c_str()) != FALSE; }
bool PathDelete(const std::string &path) { return DeleteFileW(Wide(path).c_str()) != FALSE; }
bool PathMove(const std::string &from, const std::string &to, DWORD flags) {
    return MoveFileExW(Wide(from).c_str(), Wide(to).c_str(), flags) != FALSE;
}
bool PathCopy(const std::string &from, const std::string &to, bool failIfExists) {
    return CopyFileW(Wide(from).c_str(), Wide(to).c_str(), failIfExists) != FALSE;
}
HANDLE PathCreateFile(const std::string &path, DWORD access, DWORD share, DWORD disposition, DWORD flags) {
    return CreateFileW(Wide(path).c_str(), access, share, nullptr, disposition, flags, nullptr);
}
HMODULE PathLoadLibrary(const std::string &path, DWORD flags) { return LoadLibraryExW(Wide(path).c_str(), nullptr, flags); }

std::vector<FoundFile> ListDir(const std::string &pattern) {
    std::vector<FoundFile> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Wide(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        out.push_back({Utf8Of(fd.cFileName), fd.dwFileAttributes,
                       (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, fd.ftLastWriteTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

} // namespace host
