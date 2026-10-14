// A mod's DLLs in the process: the folder each load runs from, the threads still in its code when it unloads, what
// the process gave and did not get back (see mods.cpp, docs/MODDING.md "A heavy mod").
//
// Each load of a mod runs from its own folder, <game>\mml\live\<id>_<pid>_<n>\: the mod's DLL (named
// mod_<id>_<pid>_<n>.dll, so stacks and crash reports name it) and a copy of every other DLL in the mod's folder (its
// libraries: PhysX, Jolt, OpenVR...). The mod's folder is never locked, so a build or a deploy can replace any of its
// files while the game runs, and the libraries a DLL imports are found next to it (LOAD_WITH_ALTERED_SEARCH_PATH).
// While mml_mod_load runs, the folder is also the process's DLL directory (SetDllDirectory), so a library loaded by
// a bare name in it is found too; later, a mod loads by the full path (next to its own DLL: GetModuleFileName).
#include "host.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>

namespace host {
namespace {

bool InDir(const std::string &path, const std::string &dir) {
    return path.size() > dir.size() && !_strnicmp(path.c_str(), dir.c_str(), dir.size());
}

std::string ModulePath(HMODULE m) {
    char path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(m, path, sizeof path);
    return n ? std::string(path, n) : std::string();
}

// This process's threads by id. NtGetNextThread walks only ours (a Toolhelp snapshot walks every thread of the
// system: about 30 ms here, at each unload); the snapshot stays for a Windows without it.
std::vector<DWORD> ThreadIds() {
    using NextFn = LONG(NTAPI *)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, HANDLE *);
    static auto next = reinterpret_cast<NextFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtGetNextThread"));
    std::vector<DWORD> out;
    if (next) {
        HANDLE cur = nullptr, nxt = nullptr;
        while (next(GetCurrentProcess(), cur, THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &nxt) >= 0) {
            if (cur) CloseHandle(cur);
            cur = nxt;
            out.push_back(GetThreadId(cur));
        }
        if (cur) CloseHandle(cur);
        return out;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{sizeof te};
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid) out.push_back(te.th32ThreadID);
    CloseHandle(snap);
    return out;
}

std::vector<HMODULE> Modules() {
    std::vector<HMODULE> mods(512);
    DWORD need = 0;
    for (;;) {
        if (!EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &need)) return {};
        if (need <= mods.size() * sizeof(HMODULE)) break;
        mods.resize(need / sizeof(HMODULE) + 16);
    }
    mods.resize(need / sizeof(HMODULE));
    return mods;
}

struct Range { uintptr_t lo, hi, base; size_t module; };

// The executable sections of every module loaded from dir.
void CodeRanges(const std::string &dir, std::vector<Range> *ranges, std::vector<std::string> *names) {
    for (HMODULE m : Modules()) {
        std::string path = ModulePath(m);
        if (!InDir(path, dir)) continue;
        auto base = reinterpret_cast<uintptr_t>(m);
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
            if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                ranges->push_back({base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize, base, names->size()});
        names->push_back(path.substr(path.find_last_of('\\') + 1));
    }
}

const Range *Hit(const std::vector<Range> &ranges, uintptr_t a) {
    for (const Range &r : ranges) if (a >= r.lo && a < r.hi) return &r;
    return nullptr;
}

// A return address into the ranges among the frames the thread will return through: the chain of saved EBPs, from
// its EBP up to the top of its stack. Not every word of the stack: a frame's uninitialised locals hold stale return
// addresses (DLL_THREAD_ATTACH runs every DLL's CRT on each new thread's stack), and one of those kept a test's DLL
// loaded for good. The chain passes over frames compiled without a frame pointer (they keep EBP) and gives the return
// address into such a function from a callee that has one: Windows' own functions all do, so a thread waiting in a
// system call made from a mod's code is seen. A library that uses EBP as a plain register ends the chain early.
// Nothing here allocates: the thread is suspended and may hold the heap's lock.
uintptr_t WalkFrames(const std::vector<Range> &ranges, uintptr_t esp, uintptr_t ebp) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void *>(esp), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    uintptr_t top = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    __try {
        for (int i = 0; i < 512 && ebp >= esp && ebp + 8 <= top && !(ebp & 3); i++) {
            const uintptr_t *frame = reinterpret_cast<const uintptr_t *>(ebp);
            if (Hit(ranges, frame[1])) return frame[1];
            if (frame[0] <= ebp) break;
            ebp = frame[0];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

uintptr_t StartAddress(HANDLE thread) {   // where the thread began (ThreadQuerySetWin32StartAddress)
    using QueryFn = LONG(NTAPI *)(HANDLE, ULONG, void *, ULONG, ULONG *);
    static auto query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
    uintptr_t start = 0;
    if (!query || query(thread, 9, &start, sizeof start, nullptr) < 0) return 0;
    return start;
}

void DeleteTree(const std::string &dir) {   // one level of folders is all the live folder has
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
            std::string p = dir + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p + "\\");
            else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());   // (a file still mapped stays, and its folder with it)
}
} // namespace

ProcessMeasure MeasureProcess() {
    ProcessMeasure m{};
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t a = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    uint64_t freeBytes = 0, largest = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end && VirtualQuery(reinterpret_cast<void *>(a), &mbi, sizeof mbi)) {
        if (mbi.State == MEM_FREE) {
            freeBytes += mbi.RegionSize;
            largest = std::max<uint64_t>(largest, mbi.RegionSize);
        }
        uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a) break;
        a = next;
    }
    m.freeMb = freeBytes / 1048576.0;
    m.largestFreeMb = largest / 1048576.0;
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    m.handles = static_cast<int>(handles);
    m.threads = static_cast<int>(ThreadIds().size());
    return m;
}

bool LiveCopy(const std::string &srcDir, const std::string &entry, const std::string &liveDir, const std::string &entryPath,
              std::vector<std::string> *libs, std::string *err) {
    CreateDirectoryA((MmlDir() + "live\\").c_str(), nullptr);
    if (!CreateDirectoryA(liveDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        *err = "cannot make " + liveDir;
        return false;
    }
    if (!CopyFileA((srcDir + entry).c_str(), entryPath.c_str(), FALSE)) { *err = "cannot copy " + srcDir + entry; return false; }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((srcDir + "*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !_stricmp(fd.cFileName, entry.c_str())) continue;
        if (!CopyFileA((srcDir + fd.cFileName).c_str(), (liveDir + fd.cFileName).c_str(), FALSE)) {
            *err = std::string("cannot copy its library ") + fd.cFileName;
            ok = false;
            break;
        }
        libs->push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

DllDirScope::DllDirScope(const std::string &dir) {
    char prev[MAX_PATH * 2];
    DWORD n = GetDllDirectoryA(sizeof prev, prev);
    had = n > 0 && n < sizeof prev;
    if (had) previous.assign(prev, n);
    SetDllDirectoryA(dir.c_str());
}
DllDirScope::~DllDirScope() { SetDllDirectoryA(had ? previous.c_str() : nullptr); }

std::vector<std::string> LibsElsewhere(const std::string &liveDir, const std::vector<std::string> &libs) {
    std::vector<std::string> out;
    for (const std::string &name : libs) {
        HMODULE m = GetModuleHandleA(name.c_str());
        if (!m) continue;   // not loaded (yet): the mod may load it later
        std::string path = ModulePath(m);
        if (!InDir(path, liveDir)) out.push_back(name + " is " + path);
    }
    return out;
}

std::vector<BusyThread> ThreadsInDir(const std::string &liveDir) {
    std::vector<Range> ranges;
    std::vector<std::string> names;
    CodeRanges(liveDir, &ranges, &names);
    if (ranges.empty()) return {};
    std::vector<DWORD> tids = ThreadIds();
    tids.erase(std::remove(tids.begin(), tids.end(), GetCurrentThreadId()), tids.end());
    struct Seen { DWORD tid; uintptr_t eip, hit; const char *how; };
    std::vector<Seen> seen(tids.size(), Seen{0, 0, 0, nullptr});   // allocated before any thread is suspended
    for (size_t i = 0; i < tids.size(); i++) {
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[i]);
        if (!t) continue;
        uintptr_t start = StartAddress(t);
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;   // (Eip, Esp, Ebp)
            if (GetThreadContext(t, &c)) {
                uintptr_t ret = 0;
                if (Hit(ranges, c.Eip)) seen[i] = Seen{tids[i], c.Eip, c.Eip, "at"};
                else if ((ret = WalkFrames(ranges, c.Esp, c.Ebp)) != 0) seen[i] = Seen{tids[i], c.Eip, ret, "returns to"};
                else if (Hit(ranges, start)) seen[i] = Seen{tids[i], c.Eip, start, "began at"};
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    std::vector<BusyThread> out;
    for (const Seen &s : seen) {
        if (!s.hit) continue;
        const Range *r = Hit(ranges, s.hit);
        char where[160];
        snprintf(where, sizeof where, "%s+0x%X", names[r->module].c_str(), static_cast<unsigned>(s.hit - r->base));
        out.push_back({s.tid, s.eip, s.how, where});
    }
    return out;
}

std::vector<std::string> ModulesInDir(const std::string &liveDir) {
    std::vector<std::string> out;
    for (HMODULE m : Modules()) {
        std::string path = ModulePath(m);
        if (InDir(path, liveDir)) out.push_back(path.substr(path.find_last_of('\\') + 1));
    }
    return out;
}

void LiveRemove(const std::string &liveDir) { DeleteTree(liveDir); }

void LiveCleanAll() {
    // what earlier runs left (this run's folders are deleted as their loads unload); a file of a game still running
    // is mapped and stays
    std::string dir = MmlDir() + "live\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        std::string p = dir + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p + "\\");
        else DeleteFileA(p.c_str());   // (the copies of the loader's before 2026-10-14 sat here as files)
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

} // namespace host
