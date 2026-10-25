// The player's one log: what the game's other parts write to files of their own is read as it grows and written into
// mml.log, each line under its file's tag, so a player sends mml.log alone (and mml\crash after a crash). Read:
//   DXVK's Direct3D 9 log ([dxvk]): DXVK_LOG_PATH is set to mml\logs at the start when the player has not set it
//     (DXVK reads it when its d3d9.dll loads: after the loader under Ultimate ASI Loader's d3d8to9; under dxwrapper it
//     may load first, and its <exe>_d3d9.log stays in the game's folder: read there),
//   LS3D's own log.txt in the game's folder ([ls3d]: the adapter, the display mode, the device it made),
//   the mods' logs in their data folders (mml\data\<mod>\*.log: NVIDIA NGX's, which dlss_nr's 64-bit half has written
//     there), and other ASI mods' *.log in the game's folder.
// A file last written before this run is read only from what is added to it now (an earlier run's lines stay out). The
// sidecar's own sidecar.log is left out: its lines are in mml.log already. Polled twice a second by a thread of its own,
// once more when the process ends.
#include "host.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace host {
namespace {

struct Source {
    std::string path, tag;
    uint64_t offset = 0;
    std::string partial;   // a line not ended yet
    bool acp = false;      // the system's code page (LS3D's log.txt), else UTF-8
};
std::mutex g_mx;
std::vector<Source> g_src;
FILETIME g_start;           // the process's start: a file written before it is an earlier run's
HANDLE g_thread;
double g_lastScan;

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string TagOf(const std::string &name) {
    std::string n = Lower(name);
    if (n == "log.txt") return "ls3d";
    if (n.size() > 9 && n.compare(n.size() - 9, 9, "_d3d9.log") == 0) return "dxvk";
    return name.substr(0, name.find_last_of('.'));
}

bool Known(const std::string &path) {
    for (const Source &s : g_src) if (s.path == path) return true;
    return false;
}

void Add(const std::string &dir, const FoundFile &f) {
    if (f.dir()) return;
    std::string path = dir + f.name;
    if (Known(path)) return;
    Source s;
    s.path = path;
    s.tag = TagOf(f.name);
    s.acp = Lower(f.name) == "log.txt";
    s.offset = CompareFileTime(&f.written, &g_start) < 0 ? f.size : 0;   // (an earlier run's: only what is added now)
    g_src.push_back(s);
}

void Scan() {
    const std::string logs = MmlDir() + "logs\\";
    for (const FoundFile &f : ListDir(logs + "*.log")) {
        std::string n = Lower(f.name);
        if (n != "mml.log" && n != "mml.prev.log") Add(logs, f);
    }
    for (const FoundFile &f : ListDir(GameDir() + "*.log")) Add(GameDir(), f);
    for (const FoundFile &f : ListDir(GameDir() + "log.txt")) Add(GameDir(), f);
    for (const FoundFile &d : ListDir(MmlDir() + "data\\*")) {
        if (!d.dir()) continue;
        const std::string dir = MmlDir() + "data\\" + d.name + "\\";
        for (const FoundFile &f : ListDir(dir + "*.log")) Add(dir, f);
    }
}

void Read(Source &s) {
    HANDLE h = PathCreateFile(s.path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    const uint64_t n = static_cast<uint64_t>(size.QuadPart);
    if (n < s.offset) s.offset = 0, s.partial.clear();   // (written anew)
    while (s.offset < n) {
        char buf[16384];
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>(s.offset);
        DWORD got = 0;
        if (!SetFilePointerEx(h, at, nullptr, FILE_BEGIN) ||
            !ReadFile(h, buf, static_cast<DWORD>(std::min<uint64_t>(sizeof buf, n - s.offset)), &got, nullptr) || !got)
            break;
        s.offset += got;
        s.partial.append(buf, got);
        size_t start = 0, end;
        while ((end = s.partial.find('\n', start)) != std::string::npos) {
            std::string line = s.partial.substr(start, end - start);
            start = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line.empty()) continue;
            if (s.acp) line = AcpToUtf8(line.c_str());
            Log(MML_LOG_INFO, s.tag.c_str(), "%s", line.c_str());
        }
        s.partial.erase(0, start);
        if (s.partial.size() > 8192) s.partial.clear();   // (no line that long: not text)
    }
    CloseHandle(h);
}

void Poll(bool scan) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (scan) Scan();
    for (Source &s : g_src) Read(s);
}

DWORD WINAPI Thread(void *) {
    for (unsigned i = 0;; i++) {
        Sleep(500);
        Poll(i % 4 == 0);   // (new files looked for every 2 s)
    }
}
} // namespace

void GatherStart() {
    GetSystemTimeAsFileTime(&g_start);
    ULARGE_INTEGER t{};
    t.LowPart = g_start.dwLowDateTime, t.HighPart = g_start.dwHighDateTime;
    t.QuadPart -= 20000000;   // (2 s: a file written by this run's own start, before the loader)
    g_start.dwLowDateTime = t.LowPart, g_start.dwHighDateTime = t.HighPart;
    wchar_t had[8];
    if (!GetEnvironmentVariableW(L"DXVK_LOG_PATH", had, 8) && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        std::string logs = MmlDir() + "logs";
        SetEnvironmentVariableW(L"DXVK_LOG_PATH", Wide(logs).c_str());
    }
    g_thread = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr);
    HLOG("logs: DXVK's, NGX's, LS3D's log.txt and other mods' *.log written into this log as they grow ([dxvk], [ls3d], ...)");
}

void GatherFinal() {
    if (!g_thread) return;
    if (g_mx.try_lock()) {   // (the thread was ended inside a poll: its lock is gone with it, nothing read again)
        g_mx.unlock();
        Poll(true);
    }
}

} // namespace host
