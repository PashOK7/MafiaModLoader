// The log (see host.h).
#include "host.h"
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace host {
namespace {
std::mutex g_mx;
FILE *g_file;
bool g_quiet;                     // no file (a second game of a running one's folder: its log is not ours)
std::deque<std::string> g_tail;   // the last lines, for the bridge's log.tail
constexpr size_t kTail = 2000;
const char *kLevel[] = {"debug", "info", "warn", "error"};

void Open() {
    std::string dir = MmlDir() + "logs\\";
    MakeDir(MmlDir());
    MakeDir(dir);
    // the previous run's log is kept: a crash at the game's exit is often looked at only after the next start
    PathMove(dir + "mml.log", dir + "mml.prev.log", MOVEFILE_REPLACE_EXISTING);
    g_file = FileOpen(dir + "mml.log", "w");
}
} // namespace

void LogV(MmlLogLevel level, const char *who, const char *fmt, va_list ap) {
    char msg[4096];
    vsnprintf(msg, sizeof msg, fmt, ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[4200];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d %-5s [%s] %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
             kLevel[level < 0 || level > 3 ? 1 : level], who ? who : "?", msg);
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_file && !g_quiet) Open();
    if (g_file) { fputs(line, g_file); fputc('\n', g_file); fflush(g_file); }
    g_tail.emplace_back(line);
    if (g_tail.size() > kTail) g_tail.pop_front();
}

void Log(MmlLogLevel level, const char *who, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(level, who, fmt, ap);
    va_end(ap);
}

void LogQuiet() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_quiet = true;
}

std::vector<std::string> LogTail(size_t lines) {
    std::lock_guard<std::mutex> lk(g_mx);
    size_t n = lines < g_tail.size() ? lines : g_tail.size();
    return std::vector<std::string>(g_tail.end() - n, g_tail.end());
}

} // namespace host
