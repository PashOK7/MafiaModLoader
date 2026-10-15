// A mod's own files in the game (mml.h file_add, file_source, file_read, MML_EVENT_FILE_CHANGED; docs/MODDING.md
// "A mod's own files in the game").
//
// The game reads every file through rw_data.dll, Game.exe and LS3DF.dll alike (models, textures, animations,
// sounds, tables, menu definitions, missions): dtaOpen(name) opens a loose file of the game's folder or an entry of
// its .dta archives, by a name relative to the game's folder ("Models\thunderbird00.4ds", "Maps\2CTBIRD.BMP",
// "tables\vehicles.bin", "Sounds\th_1.wav"). The loader hooks dtaOpen from the process's start (before the game
// reads its first table): a name a loaded mod ships is opened from the mod's file instead, by rw_data itself (its
// own dtaOpen given the file's full path), so the reads, seeks, times and closes that follow are the game's own and
// nothing else is hooked. Every other name goes to the game as before.
//
// What a mod ships: every file under <mod's folder>\files\, by its path there (files\maps\crate.bmp answers
// "maps\crate.bmp"), and the files it names with file_add. Names are compared as the archives compare them (case
// ignored, '/' as '\'). A mod's files are in the game while it is loaded: at the process's start for every enabled
// mod (the game reads its tables before the first frame, when mods load), taken out when it unloads (and when its
// load fails), kept across a reload and looked at again. Two mods with one name: the later in load order answers.
//
// rw_data opens a loose file sharing only reading: a mod's file the game holds open (a sound streamed, a file read
// in a loading screen) could not be replaced by a deploy. For the mods' files the loader adds write and delete
// sharing (rw_data's own CreateFileA import, only during that call): a file can be replaced while the game reads it
// (the game goes on reading the old one until it closes it).
//
// Changes on disk: each loaded mod's files\ folder is watched (a change notification, looked at every frame, read
// again a quarter of a second after the last change), file_add's files are looked at once a second. A name whose
// answer changed (content, a file come or gone, another mod's in its place) is logged and every mod gets
// MML_EVENT_FILE_CHANGED at the next frame. The game reads the new file when it next opens that name; what it has
// already read and keeps (a texture in use, a model in the model cache, a table read at its start) is its own
// (docs/MODDING.md tells where the engine reads again).
//
// A mod's picture replaces a texture whole: LS3D reads a presaved copy of a texture (<name>_<mips>.565 and the like,
// converted once and shipped in the archives) in place of the picture when its size matches, so the presaved copies
// of a mod's pictures are not given to the game (unless a mod ships them itself): LS3D makes them from the picture.
//
// Bridge methods (game thread): files.list {mod, name} (each name answered by a mod: who, from where, the others
// under it, how often the game opened it), files.which {name}, files.read {name, game, offset, size} (any file as
// the game reads it, or the game's own: an archive's entry by its name).
#include "host.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;   // dtaOpen's failure
using DtaOpenFn = uint32_t(__stdcall *)(const char *path, int direct);
using DtaReadFn = uint32_t(__stdcall *)(uint32_t handle, void *dst, uint32_t size);
using DtaSeekFn = uint32_t(__stdcall *)(uint32_t handle, int32_t offset, int origin);
using DtaCloseFn = uint32_t(__stdcall *)(uint32_t handle);
using CreateFileAFn = HANDLE(WINAPI *)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

DtaOpenFn oDtaOpen;                    // the chain under the loader's link (rw_data's own at its end)
DtaOpenFn gDtaOpen;                    // the export: what the game calls (every link)
DtaReadFn gDtaRead;
DtaSeekFn gDtaSeek;
DtaCloseFn gDtaClose;
CreateFileAFn oCreateFileA;            // rw_data's import, as it was
thread_local bool t_shareAll;          // this thread's dtaOpen opens a mod's file: share writing and deleting

struct Src {                           // one file a mod ships for a name
    std::string mod, path;
    int order = 0;
    bool added = false;                // file_add's (else the files\ folder's)
    uint64_t time = 0, size = 0;
};
struct ModFiles {
    std::string id, dir, filesDir;
    int order = 100;
    std::map<std::string, Src> files;           // by game name: the folder's, file_add's over them
    std::map<std::string, std::string> added;   // file_add: game name -> its file
    HANDLE watch = INVALID_HANDLE_VALUE;        // the files\ folder's change notification
    ULONGLONG dirtyAt = 0;                      // a change seen: read again a little later
};
std::map<std::string, ModFiles> g_mods;         // by id (game thread, and Init)

struct Answer { std::string mod, path; uint64_t time = 0, size = 0; };
SRWLOCK g_lock = SRWLOCK_INIT;                  // g_answers, g_all: read by any thread that opens a file
std::map<std::string, Answer> g_answers;        // each name's file, the last mod's in load order
std::map<std::string, std::vector<Src>> g_all;  // each name's files, in load order
std::map<std::string, std::string> g_pictures;  // a mod's picture (.bmp, .tga, .dds) by its name without the
                                                // extension ("maps\2intmph"): its game name
std::atomic<long> g_count{0};
std::map<std::string, unsigned> g_hidden;       // the game's presaved textures not given for a mod's picture (under g_opensMx)
std::mutex g_opensMx;
std::map<std::string, unsigned> g_opens;        // names opened from a mod's file, how often
std::set<std::string> g_failedOnce;             // a mod's file that could not be opened (logged once)
std::vector<std::string> g_pending;             // names whose answer changed: MML_EVENT_FILE_CHANGED next frame
ULONGLONG g_lastAddedLook;
std::string g_gameDirLower;

std::string Lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

// The game's name of a file: relative to the game's folder, lower case, '\' between parts, no "." or "..". False:
// not a name inside the game's folder.
bool GameName(const char *in, std::string *out) {
    if (!in || !*in) return false;
    std::string s;
    for (const char *p = in; *p; ++p) {
        char c = *p == '/' ? '\\' : *p;
        s += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    }
    if (!g_gameDirLower.empty() && s.compare(0, g_gameDirLower.size(), g_gameDirLower) == 0) s.erase(0, g_gameDirLower.size());
    else if ((s.size() > 1 && s[1] == ':') || s[0] == '\\') return false;   // another folder, a share
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('\\', i);
        if (j == std::string::npos) j = s.size();
        std::string part = s.substr(i, j - i);
        if (part == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        i = j + 1;
    }
    out->clear();
    for (const std::string &p : parts) *out += (out->empty() ? "" : "\\") + p;
    return !out->empty();
}

bool Stat(const std::string &path, uint64_t *time, uint64_t *size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    *time = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    *size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}

// Every file under root (a folder, trailing backslash), by its game name.
void ScanDir(const std::string &root, const std::string &rel, ModFiles *m, int depth) {
    if (depth > 16) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + rel + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ScanDir(root, rel + n + "\\", m, depth + 1); continue; }
        std::string low = Lower(n);
        if (low.size() > 7 && low.compare(low.size() - 7, 7, ".mmltmp") == 0) continue;   // a deploy's copy on its way
        if (root.size() + rel.size() + n.size() >= MAX_PATH) {   // (rw_data copies a name into MAX_PATH bytes)
            Log(MML_LOG_WARN, m->id.c_str(), "files: %s%s%s: the path is too long for the game (%d characters at most): left out",
                root.c_str(), rel.c_str(), n.c_str(), MAX_PATH - 1);
            continue;
        }
        Src s;
        s.mod = m->id;
        s.order = m->order;
        s.path = root + rel + n;
        s.time = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        s.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        m->files[Lower(rel + n)] = s;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void ScanMod(ModFiles *m) {
    m->files.clear();
    ScanDir(m->filesDir, "", m, 0);
    for (auto &kv : m->added) {
        Src s;
        s.mod = m->id;
        s.order = m->order;
        s.path = kv.second;
        s.added = true;
        if (Stat(s.path, &s.time, &s.size)) m->files[kv.first] = s;
    }
}

void Watch(ModFiles *m) {
    if (m->watch != INVALID_HANDLE_VALUE) return;
    DWORD a = GetFileAttributesA(m->filesDir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;
    m->watch = FindFirstChangeNotificationA(m->filesDir.c_str(), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
}
void Unwatch(ModFiles *m) {
    if (m->watch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(m->watch);
    m->watch = INVALID_HANDLE_VALUE;
}

// The answers made again from every mod's files, in load order (the last answers); the names whose answer changed.
std::vector<std::string> Rebuild() {
    std::vector<const ModFiles *> order;
    for (auto &kv : g_mods) order.push_back(&kv.second);
    std::stable_sort(order.begin(), order.end(), [](const ModFiles *a, const ModFiles *b) {
        return a->order != b->order ? a->order < b->order : a->id < b->id;
    });
    std::map<std::string, std::vector<Src>> all;
    for (const ModFiles *m : order)
        for (auto &kv : m->files) all[kv.first].push_back(kv.second);
    std::map<std::string, Answer> answers;
    std::map<std::string, std::string> pictures;
    for (auto &kv : all) {
        const Src &w = kv.second.back();
        answers[kv.first] = Answer{w.mod, w.path, w.time, w.size};
        size_t dot = kv.first.rfind('.');
        std::string ext = dot == std::string::npos ? "" : kv.first.substr(dot + 1);
        if (ext == "bmp" || ext == "tga" || ext == "dds") pictures[kv.first.substr(0, dot)] = kv.first;
    }
    std::vector<std::string> changed;
    AcquireSRWLockExclusive(&g_lock);
    for (auto &kv : answers) {
        auto it = g_answers.find(kv.first);
        if (it == g_answers.end() || it->second.path != kv.second.path || it->second.time != kv.second.time ||
            it->second.size != kv.second.size)
            changed.push_back(kv.first);
    }
    for (auto &kv : g_answers) if (!answers.count(kv.first)) changed.push_back(kv.first);
    g_answers.swap(answers);
    g_all.swap(all);
    g_pictures.swap(pictures);
    g_count = static_cast<long>(g_answers.size());
    ReleaseSRWLockExclusive(&g_lock);
    std::sort(changed.begin(), changed.end());
    return changed;
}

// Of these names, those that several mods ship: who answers, over whom (said when a name's answer changes).
void LogOrder(const std::string &id, const std::vector<std::string> &names) {
    int n = 0, more = 0;
    AcquireSRWLockShared(&g_lock);
    for (const std::string &name : names) {
        auto kv = g_all.find(name);
        if (kv == g_all.end() || kv->second.size() < 2) continue;
        if (++n > 20) { more++; continue; }
        std::string under;
        for (size_t i = kv->second.size() - 1; i-- > 0;)
            under += (under.empty() ? "" : ", ") + kv->second[i].mod + " (" + std::to_string(kv->second[i].order) + ")";
        Log(MML_LOG_INFO, "host", "files: %s: mod %s (load order %d) answers, over %s", kv->first.c_str(),
            kv->second.back().mod.c_str(), kv->second.back().order, under.c_str());
    }
    ReleaseSRWLockShared(&g_lock);
    if (more) Log(MML_LOG_INFO, "host", "files: %d more names of mod %s shared with other mods (files.list)", more, id.c_str());
}

void Changed(const std::vector<std::string> &names, bool tell) {
    if (tell) g_pending.insert(g_pending.end(), names.begin(), names.end());
}

// A presaved texture's name (LS3DF's ITexture::m_TryPresaved_555_565 and m_TryPresavedDXT, read in the 1.2
// decompile): <picture without its extension>[_<alpha picture without its extension>]_<mip levels>[L].555, .565 or
// .DX<n>, a picture converted once and kept in the archives. LS3D reads one in place of its picture when its size
// matches, whatever the picture holds: a mod's picture would be ignored. The mod's picture it was made of (its game
// name), or "". Under g_lock.
std::string PresaveOf(const std::string &name) {
    if (g_pictures.empty()) return "";
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = name.substr(dot + 1);
    if (ext != "555" && ext != "565" && !(ext.size() == 3 && ext[0] == 'd' && ext[1] == 'x' && ext[2] >= '0' && ext[2] <= '9'))
        return "";
    size_t end = dot;
    if (end > 0 && name[end - 1] == 'l') end--;   // (a half-size texture's)
    size_t d = end;
    while (d > 0 && name[d - 1] >= '0' && name[d - 1] <= '9') d--;
    if (d == end || d < 2 || name[d - 1] != '_') return "";
    std::string stem = name.substr(0, d - 1);
    auto it = g_pictures.find(stem);
    if (it != g_pictures.end()) return it->second;
    size_t slash = stem.rfind('\\');
    std::string dir = slash == std::string::npos ? "" : stem.substr(0, slash + 1);
    for (size_t u = stem.find('_', dir.size()); u != std::string::npos; u = stem.find('_', u + 1)) {   // with an alpha picture
        std::string alpha = stem.substr(u + 1);
        for (const std::string &n : {stem.substr(0, u), alpha, dir + alpha}) {
            auto p = g_pictures.find(n);
            if (p != g_pictures.end()) return p->second;
        }
    }
    return "";
}

// ---- the hooks
uint32_t __stdcall hkDtaOpen(const char *path, int direct) {
    if (g_count.load(std::memory_order_relaxed) > 0 && path) {
        std::string name, file, picture;
        if (GameName(path, &name)) {
            AcquireSRWLockShared(&g_lock);
            auto it = g_answers.find(name);
            if (it != g_answers.end()) file = it->second.path;
            else picture = PresaveOf(name);
            ReleaseSRWLockShared(&g_lock);
        }
        if (!picture.empty()) {   // LS3D makes the texture from the mod's picture instead
            std::lock_guard<std::mutex> lk(g_opensMx);
            if (g_hidden[name]++ == 0)
                Log(MML_LOG_INFO, "host", "files: %s: the game's presaved texture, not given (a mod's %s is the picture)",
                    name.c_str(), picture.c_str());
            SetLastError(ERROR_FILE_NOT_FOUND);
            return kNone;
        }
        if (!file.empty()) {
            t_shareAll = true;
            uint32_t h = oDtaOpen(file.c_str(), 0);
            DWORD code = GetLastError();
            t_shareAll = false;
            std::lock_guard<std::mutex> lk(g_opensMx);
            if (h != kNone) {
                g_opens[name]++;
                return h;
            }
            if (g_failedOnce.insert(name).second)
                Log(MML_LOG_WARN, "host", "files: %s: the mod's file %s could not be opened (error %lu): the game's own is read",
                    name.c_str(), file.c_str(), static_cast<unsigned long>(code));
        }
    }
    return oDtaOpen(path, direct);
}

HANDLE WINAPI hkCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                            HANDLE tmpl) {
    if (t_shareAll) share |= FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    return oCreateFileA(name, access, share, sa, disposition, flags, tmpl);
}

// A file read through the game's file layer: by the chain (what the game reads) or under the loader's link (the
// game's own). The whole size, or -1.
int64_t ReadFile_(const std::string &name, bool gameOnly, uint64_t offset, std::vector<uint8_t> *out, size_t cap) {
    if (!gDtaOpen || !gDtaRead || !gDtaSeek || !gDtaClose) return -1;
    DtaOpenFn open = gameOnly ? oDtaOpen : gDtaOpen;
    uint32_t h = open(name.c_str(), 0);
    if (h == kNone) return -1;
    uint32_t size = gDtaSeek(h, 0, 2);
    if (size == kNone) size = 0;
    if (out && cap && offset < size) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(cap, size - offset));
        out->resize(n);
        gDtaSeek(h, static_cast<int32_t>(offset), 0);
        uint32_t got = gDtaRead(h, out->data(), static_cast<uint32_t>(n));
        out->resize(got == kNone ? 0 : std::min<size_t>(got, n));
    } else if (out) {
        out->clear();
    }
    gDtaClose(h);
    return size;
}

ModFiles *ModOf(MmlMod *mod) {
    auto it = mod ? g_mods.find(ModIdOf(mod)) : g_mods.end();
    return it == g_mods.end() ? nullptr : &it->second;
}

std::string Hex(const std::vector<uint8_t> &b) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}
} // namespace

void FilesInit() {
    g_gameDirLower = Lower(GameDir());
    HMODULE rw = GetModuleHandleA("rw_data.dll");
    if (!rw) { HWARN("files: rw_data.dll is not loaded: the mods' files are not given to the game"); return; }
    auto open = reinterpret_cast<DtaOpenFn>(GetProcAddress(rw, "_dtaOpen@8"));
    gDtaRead = reinterpret_cast<DtaReadFn>(GetProcAddress(rw, "_dtaRead@12"));
    gDtaSeek = reinterpret_cast<DtaSeekFn>(GetProcAddress(rw, "_dtaSeek@12"));
    gDtaClose = reinterpret_cast<DtaCloseFn>(GetProcAddress(rw, "_dtaClose@4"));
    std::string err;
    if (!open || !gDtaRead || !gDtaSeek || !gDtaClose) { HWARN("files: rw_data.dll's dtaOpen, dtaRead, dtaSeek, dtaClose are not found"); return; }
    if (!HookAdd(nullptr, reinterpret_cast<uintptr_t>(open), reinterpret_cast<void *>(&hkDtaOpen), reinterpret_cast<void **>(&oDtaOpen), &err)) {
        HWARN("files: dtaOpen not hooked: %s", err.c_str());
        return;
    }
    gDtaOpen = open;
    void *old = nullptr;
    if (PatchImport(rw, "kernel32.dll", "CreateFileA", reinterpret_cast<void *>(&hkCreateFileA), &old)) oCreateFileA = reinterpret_cast<CreateFileAFn>(old);
    else HWARN("files: rw_data.dll's CreateFileA import not found: a mod's file the game holds open cannot be replaced meanwhile");
    if (!oCreateFileA) oCreateFileA = CreateFileA;
    HLOG("files: rw_data.dll's dtaOpen hooked (the mods' files answer the game's names)");
}

// A mod's files in the game (its load, or the process's start: initial, nobody told), or looked at again (a reload).
void FilesModOn(const std::string &id, const std::string &dir, int order, bool initial) {
    ModFiles &m = g_mods[id];
    size_t before = m.files.size();
    bool had = !m.id.empty();
    m.id = id;
    m.dir = dir;
    m.order = order;
    m.filesDir = dir + "files\\";
    m.dirtyAt = 0;
    ScanMod(&m);
    Watch(&m);
    std::vector<std::string> changed = Rebuild();
    Changed(changed, !initial);
    if (!m.files.empty() && (!had || m.files.size() != before || !changed.empty()))
        Log(MML_LOG_INFO, "host", "files: mod %s: %zu files in the game%s", id.c_str(), m.files.size(),
            had && !changed.empty() ? (" (" + std::to_string(changed.size()) + " names changed)").c_str() : "");
    if (had) {
        LogOrder(id, changed);
    } else {   // (a mod come under another's: its name's answer did not change, the order is said all the same)
        std::vector<std::string> names;
        for (auto &kv : m.files) names.push_back(kv.first);
        LogOrder(id, names);
    }
}

void FilesModOff(const std::string &id) {
    auto it = g_mods.find(id);
    if (it == g_mods.end()) return;
    size_t n = it->second.files.size();
    Unwatch(&it->second);
    g_mods.erase(it);
    std::vector<std::string> changed = Rebuild();
    Changed(changed, true);
    if (n) Log(MML_LOG_INFO, "host", "files: mod %s: its %zu files out of the game (the game's own, or another mod's, read again)", id.c_str(), n);
}

size_t FilesCount(const std::string &id) {
    auto it = g_mods.find(id);
    return it == g_mods.end() ? 0 : it->second.files.size();
}

void FilesFrame() {
    if (!g_pending.empty()) {
        std::vector<std::string> names;
        names.swap(g_pending);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        for (const std::string &n : names) {
            MmlEvent ev{};
            ev.size = sizeof ev;
            ev.type = MML_EVENT_FILE_CHANGED;
            ev.name = n.c_str();
            Dispatch(ev);
        }
    }
    if (g_mods.empty()) return;
    ULONGLONG now = GetTickCount64();
    bool lookAdded = now - g_lastAddedLook >= 1000;
    if (lookAdded) g_lastAddedLook = now;
    for (auto &kv : g_mods) {
        ModFiles &m = kv.second;
        if (m.watch != INVALID_HANDLE_VALUE && WaitForSingleObject(m.watch, 0) == WAIT_OBJECT_0) {
            if (!FindNextChangeNotification(m.watch)) Unwatch(&m);
            m.dirtyAt = now;
        }
        bool again = m.dirtyAt && now - m.dirtyAt >= 250;
        if (lookAdded && !again)
            for (auto &a : m.added) {
                uint64_t t = 0, s = 0;
                bool there = Stat(a.second, &t, &s);
                auto f = m.files.find(a.first);
                bool known = f != m.files.end() && f->second.added;
                if (there != known || (there && (f->second.time != t || f->second.size != s))) { again = true; break; }
            }
        if (lookAdded && m.watch == INVALID_HANDLE_VALUE) Watch(&m);   // a files\ folder made since
        if (!again) continue;
        m.dirtyAt = 0;
        ScanMod(&m);
        std::vector<std::string> changed = Rebuild();
        for (const std::string &n : changed) {
            std::string who;
            AcquireSRWLockShared(&g_lock);
            auto a = g_answers.find(n);
            if (a != g_answers.end()) who = a->second.mod;
            ReleaseSRWLockShared(&g_lock);
            Log(MML_LOG_INFO, "host", "files: %s changed on disk (%s): the game reads it again when it next opens it", n.c_str(),
                who.empty() ? "gone: the game's own" : ("mod " + who).c_str());
        }
        if (!changed.empty()) LogOrder(m.id, changed);
        Changed(changed, true);
    }
}

int FilesAdd(MmlMod *mod, const char *name, const char *path) {
    ModFiles *m = ModOf(mod);
    std::string n;
    if (!m || !GameName(name, &n)) {
        Log(MML_LOG_WARN, ModIdOf(mod), "file_add: '%s' is not a name inside the game's folder", name ? name : "");
        return 0;
    }
    if (!path || !*path) {
        if (!m->added.erase(n)) return 1;
    } else {
        std::string full = path;
        if (!(full.size() > 1 && full[1] == ':') && full[0] != '\\' && full[0] != '/') full = m->dir + full;
        uint64_t t, s;
        if (!Stat(full, &t, &s)) {
            Log(MML_LOG_WARN, ModIdOf(mod), "file_add: %s: no such file", full.c_str());
            return 0;
        }
        m->added[n] = full;
    }
    ScanMod(m);
    std::vector<std::string> changed = Rebuild();
    Changed(changed, true);
    if (!changed.empty()) LogOrder(m->id, changed);
    return 1;
}

int FilesSource(const char *name, char *path, int size) {
    std::string n, file;
    if (!GameName(name, &n)) return 0;
    AcquireSRWLockShared(&g_lock);
    auto it = g_answers.find(n);
    if (it != g_answers.end()) file = it->second.path;
    ReleaseSRWLockShared(&g_lock);
    if (path && size > 0) {
        size_t k = std::min<size_t>(file.size(), static_cast<size_t>(size - 1));
        memcpy(path, file.data(), k);
        path[k] = 0;
    }
    return file.empty() ? 0 : 1;
}

int64_t FilesRead(const char *name, void *buf, size_t cap, int flags) {
    std::string n;
    if (!GameName(name, &n)) return -1;
    std::vector<uint8_t> data;
    int64_t size = ReadFile_(n, (flags & MML_FILE_GAME) != 0, 0, buf ? &data : nullptr, buf ? cap : 0);
    if (size >= 0 && buf && !data.empty()) memcpy(buf, data.data(), data.size());
    return size;
}

bool FilesMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method.compare(0, 6, "files.") != 0) return false;
    *ok = false;
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "files.list") {
            // {mod, name: a part of the names}: the mods with files and every name they answer
            std::string mod = p.value("mod", ""), part = Lower(p.value("name", ""));
            json mods = json::array(), files = json::array();
            for (auto &kv : g_mods) {
                if (!mod.empty() && kv.first != mod) continue;
                mods.push_back({{"id", kv.first}, {"load_order", kv.second.order}, {"files", kv.second.files.size()},
                                {"added", kv.second.added.size()}, {"folder", kv.second.filesDir},
                                {"watched", kv.second.watch != INVALID_HANDLE_VALUE}});
            }
            size_t shown = 0, total = 0;
            std::map<std::string, unsigned> opens, hidden;
            {
                std::lock_guard<std::mutex> lk(g_opensMx);
                opens = g_opens;
                hidden = g_hidden;
            }
            AcquireSRWLockShared(&g_lock);
            for (auto &kv : g_all) {
                bool mine = mod.empty();
                for (const Src &s : kv.second) mine |= s.mod == mod;
                if (!mine || (!part.empty() && kv.first.find(part) == std::string::npos)) continue;
                total++;
                if (shown >= 1000) continue;
                shown++;
                const Src &w = kv.second.back();
                json under = json::array();
                for (size_t i = kv.second.size() - 1; i-- > 0;) under.push_back(kv.second[i].mod);
                json f = {{"name", kv.first}, {"mod", w.mod}, {"path", w.path}, {"size", w.size}, {"opens", opens[kv.first]}};
                if (w.added) f["added"] = true;
                if (!under.empty()) f["over"] = under;
                files.push_back(f);
            }
            ReleaseSRWLockShared(&g_lock);
            json out = {{"mods", mods}, {"files", files}, {"count", total}};
            if (!hidden.empty()) out["presaves_hidden"] = hidden;   // the game's presaved textures not given, how often
            if (shown < total) out["note"] = "the first 1000 shown: give name (a part of the names) or mod";
            *reply = dump(out);
            return *ok = true;
        }
        if (method == "files.which" || method == "files.read") {
            std::string raw = p.value("name", ""), n;
            if (!GameName(raw.c_str(), &n)) { *reply = "name: a file's name inside the game's folder (\"tables\\\\credits.def\")"; return true; }
            json out = {{"name", n}};
            AcquireSRWLockShared(&g_lock);
            auto it = g_all.find(n);
            if (it != g_all.end()) {
                out["mod"] = it->second.back().mod;
                out["path"] = it->second.back().path;
                json under = json::array();
                for (size_t i = it->second.size() - 1; i-- > 0;) under.push_back({{"mod", it->second[i].mod}, {"path", it->second[i].path}});
                if (!under.empty()) out["over"] = under;
            } else {
                std::string pic = PresaveOf(n);
                if (!pic.empty()) out["hidden_for"] = pic;   // a presaved texture of a mod's picture
            }
            ReleaseSRWLockShared(&g_lock);
            if (method == "files.which") {
                int64_t own = ReadFile_(n, true, 0, nullptr, 0);
                out["game_has"] = own >= 0;   // its folder or archives
                if (own >= 0) out["game_size"] = own;
                if (out.contains("hidden_for"))
                    out["answer"] = "nothing: the game's presaved texture is hidden, LS3D makes it from the mod's " +
                                    out["hidden_for"].get<std::string>();
                else if (!out.contains("mod"))
                    out["answer"] = own >= 0 ? "the game's own" : "nothing: the game has no such file";
                *reply = dump(out);
                return *ok = true;
            }
            bool game = p.value("game", false);
            uint64_t offset = p.value("offset", 0ull);
            size_t size = std::min<size_t>(p.value("size", 256), 65536);
            std::vector<uint8_t> data;
            int64_t total = ReadFile_(n, game, offset, &data, size);
            if (total < 0) { *reply = "files.read: " + n + ": the game has no such file" + std::string(game ? " of its own" : ""); return true; }
            out["source"] = game || !out.contains("mod") ? "game" : "mod";
            if (game) { out.erase("mod"); out.erase("path"); out.erase("over"); }
            out["size"] = total;
            out["offset"] = offset;
            out["hex"] = Hex(data);
            bool text = !data.empty();
            for (uint8_t c : data) text &= c >= 32 || c == '\r' || c == '\n' || c == '\t';
            if (text) out["text"] = std::string(data.begin(), data.end());
            *reply = dump(out);
            return *ok = true;
        }
        *reply = "unknown method '" + method + "' (files.list, files.which, files.read)";
        return true;
    } catch (const std::exception &e) {
        *reply = std::string("params: ") + e.what();
        return true;
    }
}

} // namespace host
