// Native IShow::Close terminates its Ogg thread, even inside rw_data's shared critical section.
// Keep the native refill/backend and notification semantics, but give the worker a stop event and join it
// before native Close releases anything. See knowledge/notes/hang-20261114.md for the controlled reproducer.
#include "host.h"
#include "mml/game/game_build.h"
#include <mmsystem.h>
#include <dsound.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace host {
namespace {
using CloseFn = int(__stdcall *)(void *);
using FillFn = int(__thiscall *)(void *, bool);
CloseFn oClose;
void *oWorker; // HookAdd's trampoline; never call the original infinite, non-cooperative worker.
FillFn g_fill;
bool g_hooked;
int g_flags, g_event, g_buffer, g_count, g_threshold;
std::atomic<unsigned> g_started{}, g_exited{}, g_joined{}, g_failures{};

struct Worker {
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Worker() { if (!stop) throw std::runtime_error("Ogg stop event unavailable"); }
    ~Worker() { CloseHandle(stop); }
};
// Global initialization: no function-local dynamic statics/TLS on native audio threads.
std::mutex g_mutex;
std::unordered_map<void *, std::shared_ptr<Worker>> g_workers;

std::shared_ptr<Worker> GetWorker(void *show) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_workers.find(show);
    if (it != g_workers.end()) return it->second;
    auto state = std::make_shared<Worker>();
    g_workers.emplace(show, state);
    return state;
}

template<class T> T &Field(void *show, int offset) {
    return *reinterpret_cast<T *>(static_cast<char *>(show) + offset);
}

DWORD WINAPI OggWorker(void *show) {
    ++g_started;
    // Close may arrive before the new thread gets scheduled. Both paths obtain the SAME state;
    // a pre-signalled stop then exits without touching the decoder or DirectSound buffer.
    const auto state = GetWorker(show);
    const HANDLE events[] = {state->stop, Field<HANDLE>(show, g_event)};
    for (;;) {
        Sleep(0); // retain the native yield/message-wait behavior
        const DWORD result = MsgWaitForMultipleObjects(2, events, FALSE, INFINITE, 0xBF);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_FAILED) { ++g_failures; break; }
        if (result != WAIT_OBJECT_0 + 1) continue;
        auto &flags = Field<volatile DWORD>(show, g_flags);
        auto &count = Field<int>(show, g_count);
        if (count >= g_threshold) {
            if (flags & 0x40000000) {
                auto *buffer = Field<IDirectSoundBuffer *>(show, g_buffer);
                buffer->Stop();
                buffer->SetCurrentPosition(0);
                flags &= 0x97FFFFFF;
            }
            count = 0;
        }
        if (flags & 0x20000000) {
            g_fill(show, false); // original ov_read/read_func/dtaRead, all locks leave normally
            ++count;
        }
    }
    ++g_exited;
    return 0;
}

int __stdcall CloseShow(void *show) {
    HANDLE notify = nullptr;
    HANDLE thread = Field<HANDLE>(show, g_event - 4);
    if (Field<HANDLE>(show, g_event) && thread) {
        const auto state = GetWorker(show);
        SetEvent(state->stop);
        // No timeout and no TerminateThread fallback: the current native refill must unwind first.
        // Native callers close from the game thread, never from the decoder itself.
        const DWORD joined = WaitForSingleObject(thread, INFINITE);
        if (joined != WAIT_OBJECT_0) {
            ++g_failures;
            // A bad native handle invalidates the lifecycle contract. Do not free a live decoder.
            RaiseFailFastException(nullptr, nullptr, 0);
        }
        ++g_joined;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_workers.erase(show);
        }
        // Hide the joined worker/event from native TerminateThread entirely. We now own both handles.
        notify = Field<HANDLE>(show, g_event);
        Field<HANDLE>(show, g_event) = nullptr;
        Field<HANDLE>(show, g_event - 4) = nullptr;
        Field<DWORD>(show, g_event - 8) = 0;
        CloseHandle(thread);
    }
    const int result = oClose(show);
    if (notify) CloseHandle(notify);
    return result;
}
} // namespace

void OggFixInit() {
    if (!game::g_ls3d) return;
    const auto &l = game::Ls3d();
    const uintptr_t base = ModuleBase("LS3DF.dll"), worker = base + l.oggThreadProc, close = base + l.showClose;
    const bool old = !strcmp(l.name, "1.0");
    g_flags = old ? 0x24 : 0x28;
    g_event = old ? 0x64 : 0x68;
    g_buffer = g_event + 8;
    g_count = g_event + 12;
    g_threshold = old ? 22 : 11;
    g_fill = reinterpret_cast<FillFn>(base + l.oggFillBuffer);
    // VideoInit already chains Close; its first five bytes are MinHook's jump. Validate untouched code.
    const uint8_t closeBytes[] = {0x8B,0x06,0x56,0xFF,0x50,0x18};
    uint8_t bytes[sizeof closeBytes];
    const uint8_t workerExpected[] = {0x55,0x56,0x8B,0x74,0x24,0x10,0x57};
    uint8_t workerBytes[sizeof workerExpected];
    if (!l.oggThreadProc || !l.oggFillBuffer || !MemRead(close + 7, bytes, sizeof bytes) ||
        memcmp(bytes, closeBytes, sizeof bytes) || !MemRead(worker + 7, workerBytes, sizeof workerBytes) ||
        memcmp(workerBytes, workerExpected, sizeof workerBytes)) {
        HWARN("fixes: native Ogg code differs; cooperative shutdown not installed");
        return;
    }
    std::string error;
    if (!HookAdd(nullptr, worker, reinterpret_cast<void *>(&OggWorker), &oWorker, &error)) {
        HWARN("fixes: Ogg worker hook failed (%s)", error.c_str());
        return;
    }
    if (!HookAdd(nullptr, close, reinterpret_cast<void *>(&CloseShow), reinterpret_cast<void **>(&oClose), &error)) {
        HookRemoveLink(nullptr, worker, reinterpret_cast<void *>(&OggWorker));
        HWARN("fixes: Ogg close hook failed (%s)", error.c_str());
        return;
    }
    g_hooked = true;
    HLOG("fixes: native Ogg readers exit cooperatively before decoder/archive close");
}

std::string OggFixState() {
    unsigned states;
    { std::lock_guard<std::mutex> lock(g_mutex); states = static_cast<unsigned>(g_workers.size()); }
    char text[256];
    snprintf(text, sizeof text, "{\"hooked\":%s,\"started\":%u,\"exited\":%u,\"joined\":%u,\"states\":%u,\"failures\":%u}",
             g_hooked ? "true" : "false", g_started.load(), g_exited.load(), g_joined.load(), states, g_failures.load());
    return text;
}
} // namespace host
