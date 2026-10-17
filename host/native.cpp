// native.call: any function of the game (or of any module) called from the bridge, a hypothesis tried in one call
// (docs/PROTOCOL.md "native.call"). Game thread (where the game calls its own functions), behind a fault guard.
//
// {"at": "game+0x47B590" | "ls3d+0x1AAE0" | "0x47B590", "conv": "stdcall" | "cdecl" | "thiscall" | "fastcall",
//  "this": <ECX for thiscall>, "args": [...], "ret": "int" | "uint" | "ptr" | "float" | "double" | "int64" | "str" | "void"}
// An argument: a number (an int32 / uint32), an address ("game+0x...", "0x..."), {"float": x}, {"double": x} (two
// stack words), {"str": "text"} (a pointer to a NUL-terminated copy), {"buf": n} or {"buf": n, "hex": "..."} (a
// pointer to n bytes, zeroed or filled; returned after the call as hex, words and floats: out-parameters).
// The x87 stack is left as it was found: whatever the function left in ST0 is popped (and returned) whatever `ret`
// says. ESP is measured around the call and put back: the callee's popping is compared with what the convention
// says it pops, so a wrong convention or count of arguments shows in the reply.
#include "host.h"
#include <cmath>
#include <cstring>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

struct Regs {
    DWORD eax = 0, edx = 0;
    double st0 = 0;
    DWORD st0Full = 0;     // 1: the function left a value in ST0
    DWORD espBefore = 0, espAfter = 0;
};

// Pushes args[n-1]..args[0], ECX and EDX as given, calls, keeps EAX, EDX, ST0 and ESP, puts ESP back.
void __declspec(noinline) Raw(void *fn, DWORD ecxv, DWORD edxv, const DWORD *args, DWORD n, Regs *r) {
    DWORD eaxv = 0, edxo = 0, before = 0, after = 0, full = 0;
    double st0 = 0;
    __asm {
        mov before, esp
        mov esi, args
        mov ebx, n
    push_loop:
        test ebx, ebx
        jz pushed
        dec ebx
        push dword ptr [esi + ebx * 4]
        jmp push_loop
    pushed:
        mov ecx, ecxv
        mov edx, edxv
        call fn
        mov eaxv, eax
        mov edxo, edx
        mov after, esp
        mov esp, before
        // ST0 empty? (fxam: C3 C0 = 1 0 1 ... "empty" is C3=1, C0=1, C2=0)
        fxam
        fnstsw ax
        and ah, 0x45
        cmp ah, 0x41
        je st0_empty
        fstp qword ptr st0
        mov full, 1
    st0_empty:
    }
    r->eax = eaxv;
    r->edx = edxo;
    r->espBefore = before;
    r->espAfter = after;
    r->st0 = st0;
    r->st0Full = full;
}

// (the game's function with the game's own x87 word, whatever the caller's: fpu.cpp)
DWORD SafeRaw(void *fn, DWORD ecxv, DWORD edxv, const DWORD *args, DWORD n, Regs *r, DWORD *where) {
    uint16_t fpu = FpuWord();
    FpuSet(FpuGameWord());
    DWORD e = 0;
    __try {
        Raw(fn, ecxv, edxv, args, n, r);
    } __except (*where = reinterpret_cast<DWORD>(GetExceptionInformation()->ExceptionRecord->ExceptionAddress), EXCEPTION_EXECUTE_HANDLER) {
        e = GetExceptionCode();
    }
    FpuRestore(fpu);
    return e;
}

std::string Hex8(DWORD v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(v));
    return b;
}

std::string HexBytes(const std::vector<uint8_t> &v) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (uint8_t c : v) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

// A word for a number or an address; false: neither.
bool Word(const json &a, DWORD *out, std::string *err) {
    if (a.is_number_integer()) { *out = static_cast<DWORD>(a.get<int64_t>()); return true; }
    if (a.is_number_unsigned()) { *out = static_cast<DWORD>(a.get<uint64_t>()); return true; }
    if (a.is_boolean()) { *out = a.get<bool>() ? 1 : 0; return true; }
    if (a.is_null()) { *out = 0; return true; }
    if (a.is_string()) {
        uintptr_t v = ParseAddress(a.get<std::string>());
        if (!v && a.get<std::string>() != "0") { *err = "\"" + a.get<std::string>() + "\": not an address (a text is {\"str\": ...})"; return false; }
        *out = static_cast<DWORD>(v);
        return true;
    }
    *err = "an argument not understood: " + a.dump(-1, ' ', false, json::error_handler_t::replace);
    return false;
}

} // namespace

bool NativeMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "native.call") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) { *reply = "native.call {at, conv, this, args, ret}"; return true; }
    std::string err;
    DWORD at = 0;
    if (!p.contains("at") || !Word(p["at"], &at, &err) || !at) { *reply = "native.call: `at`: " + (err.empty() ? "no address" : err); return true; }
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery(reinterpret_cast<void *>(at), &mi, sizeof mi) || mi.State != MEM_COMMIT ||
        !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        *reply = "native.call: " + Hex8(at) + " is not executable code (" + DescribeAddress(at) + ")";
        return true;
    }
    std::string conv = p.value("conv", "stdcall"), ret = p.value("ret", "int");
    if (conv != "stdcall" && conv != "cdecl" && conv != "thiscall" && conv != "fastcall") { *reply = "native.call: conv is stdcall, cdecl, thiscall or fastcall"; return true; }
    // the arguments as stack words (and the buffers they point to)
    std::vector<DWORD> words;
    std::vector<std::vector<uint8_t>> bufs;   // (kept to the end of the call: strings and buffers)
    std::vector<int> bufArg;                  // which argument each buffer is
    json args = p.contains("args") && p["args"].is_array() ? p["args"] : json::array();
    for (size_t i = 0; i < args.size(); i++) {
        const json &a = args[i];
        if (a.is_object() && a.contains("float")) {
            float f = a["float"].get<float>();
            DWORD w;
            memcpy(&w, &f, 4);
            words.push_back(w);
        } else if (a.is_object() && a.contains("double")) {
            double d = a["double"].get<double>();
            DWORD w[2];
            memcpy(w, &d, 8);
            words.push_back(w[0]);
            words.push_back(w[1]);
        } else if (a.is_object() && a.contains("str")) {
            std::string s = a["str"].get<std::string>();
            bufs.emplace_back(s.begin(), s.end());
            bufs.back().push_back(0);
            bufArg.push_back(-1);
            words.push_back(static_cast<DWORD>(reinterpret_cast<uintptr_t>(bufs.back().data())));
        } else if (a.is_object() && a.contains("buf")) {
            size_t n = std::min<size_t>(a["buf"].get<size_t>(), 65536);
            std::vector<uint8_t> b(std::max<size_t>(n, 4), 0);
            std::string hx = a.value("hex", "");
            for (size_t k = 0; k + 1 < hx.size() && k / 2 < b.size(); k += 2) b[k / 2] = static_cast<uint8_t>(std::stoul(hx.substr(k, 2), nullptr, 16));
            bufs.push_back(std::move(b));
            bufArg.push_back(static_cast<int>(i));
            words.push_back(static_cast<DWORD>(reinterpret_cast<uintptr_t>(bufs.back().data())));
        } else {
            DWORD w;
            if (!Word(a, &w, &err)) { *reply = "native.call: argument " + std::to_string(i) + ": " + err; return true; }
            words.push_back(w);
        }
    }
    // (bufs never grows after this: the pointers handed out stay valid)
    DWORD ecxv = 0, edxv = 0;
    size_t first = 0;
    if (conv == "thiscall") {
        if (!p.contains("this") || !Word(p["this"], &ecxv, &err)) { *reply = "native.call: thiscall needs `this` (ECX)" + (err.empty() ? "" : ": " + err); return true; }
    } else if (conv == "fastcall") {
        if (words.size() > 0) ecxv = words[0];
        if (words.size() > 1) edxv = words[1];
        first = std::min<size_t>(words.size(), 2);
    }
    std::vector<DWORD> stack(words.begin() + static_cast<std::ptrdiff_t>(first), words.end());
    Regs r;
    DWORD where = 0;
    double t0 = ProfNow();
    DWORD ex = SafeRaw(reinterpret_cast<void *>(static_cast<uintptr_t>(at)), ecxv, edxv, stack.data(), static_cast<DWORD>(stack.size()), &r, &where);
    double ms = ProfNow() - t0;
    if (ex) {
        char b[160];
        snprintf(b, sizeof b, "native.call: the function faulted (exception %08X at %s); nothing else was touched by the loader",
                 static_cast<unsigned>(ex), DescribeAddress(where).c_str());
        *reply = b;
        return true;
    }
    // the stack: what the callee popped against what the convention says it pops
    long pushed = static_cast<long>(stack.size() * 4);
    long popped = static_cast<long>(r.espAfter) - (static_cast<long>(r.espBefore) - pushed);   // (call and ret: even)
    long expected = conv == "cdecl" ? 0 : pushed;
    json out = {{"at", Hex8(at)}, {"where", DescribeAddress(at)}, {"conv", conv}, {"eax", Hex8(r.eax)}, {"edx", Hex8(r.edx)},
                {"ms", std::round(ms * 1000) / 1000},
                {"stack", {{"pushed", pushed}, {"popped_by_callee", popped}, {"expected", expected}, {"ok", popped == expected}}}};
    if (popped != expected)
        out["stack"]["note"] = popped > expected ? "the function popped more than the convention says: a stdcall/thiscall taking more arguments than given?"
                                                 : "the function popped less: a cdecl one, or fewer arguments than given?";
    if (r.st0Full) out["st0"] = r.st0;
    if (ret == "int") out["ret"] = static_cast<int32_t>(r.eax);
    else if (ret == "uint") out["ret"] = static_cast<uint32_t>(r.eax);
    else if (ret == "ptr") { out["ret"] = Hex8(r.eax); out["ret_where"] = DescribeAddress(r.eax); }
    else if (ret == "int64") out["ret"] = static_cast<int64_t>((static_cast<uint64_t>(r.edx) << 32) | r.eax);
    else if (ret == "float" || ret == "double") {
        if (r.st0Full) out["ret"] = r.st0;
        else out["ret"] = nullptr, out["note"] = "the function left nothing on the x87 stack: not a float function?";
    } else if (ret == "str") {
        char s[512] = "";
        if (r.eax && MemRead(r.eax, s, sizeof s - 1)) out["ret"] = std::string(s);
        else out["ret"] = nullptr;
    }
    json bo = json::array();
    for (size_t k = 0; k < bufs.size(); k++) {
        if (bufArg[k] < 0) continue;
        const std::vector<uint8_t> &b = bufs[k];
        json w = json::array(), f = json::array();
        for (size_t i = 0; i + 4 <= b.size() && i < 64; i += 4) {
            uint32_t u;
            float x;
            memcpy(&u, &b[i], 4);
            memcpy(&x, &b[i], 4);
            w.push_back(Hex8(u));
            f.push_back(std::isfinite(x) ? json(x) : json(nullptr));
        }
        bo.push_back({{"arg", bufArg[k]}, {"hex", HexBytes(b)}, {"words", w}, {"floats", f}});
    }
    if (!bo.empty()) out["buffers"] = bo;
    *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
