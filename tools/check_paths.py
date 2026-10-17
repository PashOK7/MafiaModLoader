"""check_paths.py: the loader's, the SDK's and the mods' C and C++ sources kept free of what breaks a game folder
named outside ASCII (a player's "C:\\Игры\\Mafia").

    python tools/check_paths.py        list what it finds; exit 1 when anything is found

A path in the loader is UTF-8 and Windows is asked through its wide functions (host/paths.cpp; a mod: mml/path.h).
The C runtime's and Windows' ANSI functions read a path in the system's code page instead: on a folder that code
page cannot spell they fail, and their names, handed on as UTF-8, are invalid: json's dump threw on one at the
bridge's start and every start crashed (2026-10-17). So this refuses:

- a path given to an ANSI function: fopen and the CRT's file functions, the Windows ...A functions that take a
  path, std::filesystem (its narrow paths are the code page's), a file stream opened by anything but Wide(...);
- in the host, a json dump without the replace handler (a name in the code page, a mod's text: strict dumping
  throws on invalid UTF-8, and nothing catches it on the game's thread).

A line that must do one of these says why with "ansi-ok:" in a comment on it (the stand-in game's rw_data, which
plays the game's ANSI file layer). A system DLL loaded by its bare name (LoadLibraryA("dbghelp.dll")) is no path.
tests/run_tests.py runs it.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ('host', 'sdk', 'mods', os.path.join('tests', 'harness'), os.path.join('tests', 'mods'))
EXTS = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.inc')
# the files that are the way: the wide functions themselves
SKIP = {os.path.join('host', 'paths.cpp'), os.path.join('sdk', 'include', 'mml', 'path.h')}

ANSI_WIN = ('CreateFileA', 'LoadLibraryA', 'LoadLibraryExA', 'GetModuleFileNameA', 'FindFirstFileA', 'FindFirstFileExA',
            'GetFileAttributesA', 'GetFileAttributesExA', 'SetFileAttributesA', 'CreateDirectoryA', 'RemoveDirectoryA',
            'DeleteFileA', 'MoveFileA', 'MoveFileExA', 'CopyFileA', 'ReplaceFileA', 'GetFullPathNameA',
            'GetLongPathNameA', 'GetShortPathNameA', 'GetCurrentDirectoryA', 'SetCurrentDirectoryA', 'GetTempPathA',
            'GetTempFileNameA', 'SearchPathA', 'ShellExecuteA', 'ShellExecuteExA', 'CreateProcessA',
            'GetPrivateProfileStringA', 'GetPrivateProfileIntA', 'WritePrivateProfileStringA',
            'FindFirstChangeNotificationA', 'SHGetFolderPathA', 'SHGetSpecialFolderPathA', 'SHCreateDirectoryExA',
            'PathFileExistsA', 'PathCombineA', 'GetModuleFileNameExA', 'CreateHardLinkA', 'CreateSymbolicLinkA',
            'GetDiskFreeSpaceExA', 'GetVolumeInformationA', 'OpenFile', '_lopen', '_lcreat')
CRT = ('fopen', 'fopen_s', '_fsopen', 'freopen', 'freopen_s', '_open', '_sopen', '_sopen_s', '_creat', '_stat',
       '_stat32', '_stat64', '_stati64', '_access', '_access_s', '_mkdir', '_rmdir', '_chdir', '_unlink', '_getcwd',
       '_fullpath', '_findfirst', '_findfirst64', '_splitpath', 'opendir', 'remove', 'rename', 'stat', 'access',
       'mkdir', 'rmdir', 'unlink', 'getcwd')

RULES = [
    (re.compile(r'\b(%s)\s*\(' % '|'.join(ANSI_WIN)), 'an ANSI Windows function with a path: its W twin, Wide(path)'),
    # (not a member, not std::remove's algorithm, not a mod's mml_fopen)
    (re.compile(r'(?<![\w:.>])(%s)\s*\(' % '|'.join(CRT)), 'a C runtime function with a narrow path: host FileOpen / mml_fopen, or the W functions'),
    (re.compile(r'std::filesystem|\bfs::path\b|<filesystem>'), 'std::filesystem: a narrow path is the code page\'s; Wide(path)'),
    (re.compile(r'\b(?:std::)?[io]?fstream\b(?:\s+\w+)?\s*\(\s*(?!Wide\()[^)\s]'), 'a file stream opened by a narrow path: open it by Wide(path)'),
]
DUMP = re.compile(r'\.dump\(([^;]*)')
LIBRARY_BY_NAME = re.compile(r'\bLoadLibrary(?:Ex)?A\s*\(\s*"[^"/\\]+"')   # a system DLL by its bare name


def strip_comment(line):
    """The code of a line (a // comment and string literals' contents left out of the rules' sight)."""
    out, i, quote = [], 0, None
    while i < len(line):
        c = line[i]
        if quote:
            if c == '\\':
                i += 2
                continue
            if c == quote:
                quote = None
                out.append(c)
            i += 1
            continue
        if c in '"\'':
            quote = c
            out.append(c)
        elif line.startswith('//', i):
            break
        else:
            out.append(c)
        i += 1
    return ''.join(out)


def scan():
    found = []
    for d in DIRS:
        for dirpath, dirs, files in os.walk(os.path.join(ROOT, d)):
            dirs[:] = [x for x in dirs if x not in ('build', 'third_party', 'shaders')]
            for name in files:
                if not name.endswith(EXTS):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, ROOT)
                if rel in SKIP:
                    continue
                try:
                    lines = open(path, encoding='utf-8').read().splitlines()
                except UnicodeDecodeError:
                    found.append((rel, 0, 'not UTF-8', ''))
                    continue
                in_block = False
                for no, line in enumerate(lines, 1):
                    if 'ansi-ok:' in line:
                        continue
                    text = line
                    if in_block:
                        end = text.find('*/')
                        if end < 0:
                            continue
                        text, in_block = text[end + 2:], False
                    while '/*' in text:
                        start = text.find('/*')
                        end = text.find('*/', start + 2)
                        if end < 0:
                            text, in_block = text[:start], True
                            break
                        text = text[:start] + text[end + 2:]
                    code = strip_comment(text)
                    raw = LIBRARY_BY_NAME.sub('', text)   # (literals kept: a bare DLL name is told by its literal)
                    for rule, why in RULES:
                        if rule.search(strip_comment(raw)):
                            found.append((rel, no, why, line.strip()))
                            break
                    if rel.startswith('host' + os.sep):
                        for m in DUMP.finditer(code):
                            if 'error_handler_t::replace' not in m.group(1):
                                found.append((rel, no, 'json dump without error_handler_t::replace', line.strip()))
    return found


def main():
    found = scan()
    for rel, no, why, line in found:
        print('%s:%d: %s\n    %s' % (rel, no, why, line))
    print('%d found' % len(found) if found else 'paths: nothing found')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
