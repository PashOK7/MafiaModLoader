/* MafiaModLoader: the host's paths (UTF-8) opened by a mod.
 *
 * Every path the host gives a mod (game_dir, mod_dir, mod_data_dir, file_source) is UTF-8, so a game in a folder of
 * any name works. fopen and the ANSI functions (CreateFileA...) read a path in the system's code page instead and
 * fail, or open another file, on any character outside ASCII: widen the path and use the wide functions.
 *
 *   FILE *f = mml_fopen(path, "rb");                      (the CRT's _wfopen)
 *   wchar_t w[1024]; mml_widen(path, w, 1024);            (for CreateFileW, LoadLibraryW, FindFirstFileW...)
 *
 * Header-only, in the mod's own runtime (a FILE* is never passed between the mod and the host).
 */
#ifndef MML_PATH_H
#define MML_PATH_H

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

__declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int code_page, unsigned long flags, const char *str,
                                                        int len, wchar_t *wide, int wide_len);

/* path (UTF-8) as UTF-16 into out (cap characters, terminated); the characters written without the terminator, 0
 * when it does not fit or is not UTF-8. */
static inline int mml_widen(const char *path, wchar_t *out, int cap) {
    int n;
    if (!path || !out || cap <= 0) return 0;
    n = MultiByteToWideChar(65001 /* CP_UTF8 */, 8 /* MB_ERR_INVALID_CHARS */, path, -1, out, cap);
    if (n <= 0) { out[0] = 0; return 0; }
    return n - 1;
}

/* fopen for a UTF-8 path (mode as fopen's: "rb", "w", "a"...). */
static inline FILE *mml_fopen(const char *path, const char *mode) {
    wchar_t wpath[1024], wmode[16];
    int i;
    if (!mml_widen(path, wpath, 1024) || !mode) return NULL;
    for (i = 0; mode[i] && i < 15; i++) wmode[i] = (wchar_t)(unsigned char)mode[i];
    wmode[i] = 0;
    return _wfopen(wpath, wmode);
}

#ifdef __cplusplus
}
#endif

#endif /* MML_PATH_H */
