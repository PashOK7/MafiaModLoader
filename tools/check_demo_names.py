"""check_demo_names.py: no name taken from the Mafia demo's LS3D.dll in what the repository carries.

    python tools/check_demo_names.py [--list]

The demo's licence lets nothing of it be copied (re/demo/license.txt inside its installer: "may not be copied or
reproduced in any manner or medium, in whole or in part"), and its CodeView symbols are part of its LS3D.dll. Its
names stay on the machine that has the demo (knowledge/ls3df.dll/demo*.local.csv, made by tools/kb_demo.py); the
repository names those functions by names of its own. This check reads the demo's names from those local files and
looks for each one that no other source gives (MafiaReverse's functions.csv, the other projects' external.csv, the
SDK's game_build.h vocabulary, the Windows SDK's headers the sources include) in every file git tracks: a match is a name of the demo's that leaked. Without the
local files (a machine without the demo) there is nothing to check against: it says so and passes.
"""
import csv
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BEFORE_DEMO = '61770d2'   # the last commit before the demo's LS3D.dll was read (2026-10-20)
KB = os.path.join(ROOT, 'knowledge')
TEXT = ('.md', '.txt', '.py', '.rs', '.cpp', '.c', '.h', '.hpp', '.inc', '.hlsl', '.hlsli', '.csv', '.json', '.toml',
        '.cmake', '.in', '.ini', '.lua', '.bat', '.ps1', '.yml', '.yaml')


def rows(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding='utf-8', newline='') as f:
        return list(csv.reader(f))


def demo_names():
    names = set()
    for mod in ('ls3df.dll', 'game.exe'):
        for file in ('demo.local.csv', 'demo_10.local.csv'):
            for r in rows(os.path.join(KB, mod, file))[1:]:
                if len(r) > 2 and r[2]:
                    names.add(r[2])
    return names


def other_names():
    """Every name another source gives (a demo name that is also one of these is no leak)."""
    names = set()
    for mod in ('ls3df.dll', 'game.exe'):
        for r in rows(os.path.join(KB, mod, 'functions.csv'))[1:]:
            if len(r) > 1:
                names.add(r[1])
        for file in ('external.csv', 'external.local.csv', 'external_10.csv'):
            for r in rows(os.path.join(KB, mod, file))[1:]:
                if len(r) > 2:
                    names.add(r[2])
    return names


def public(name):
    """A name anyone has: C++'s operators, the runtime's, DirectShow's base classes and the DirectX SDK sample's texture
    renderer the engine built on (CBaseRenderer's overrides keep their SDK names)."""
    last = name.split('::')[-1]
    cls = name.split('::')[0] if '::' in name else ''
    return (last.startswith('operator') or name.startswith(('CBase', 'CMediaType', 'CRendererInputPin', 'CPosPassThru'))
            or cls in ('CTextureRenderer', 'CBaseRenderer', 'CBaseVideoRenderer')
            or last in ('CheckMediaType', 'DoRenderSample', 'SetMediaType', 'OnStartStreaming', 'OnStopStreaming'))


def sdk_names():
    """Every name the Windows SDK's headers that the repository's sources include declare (dxva2api.h's
    IDirect3DDeviceManager9::ResetDevice, the demo's LS3D also has a ResetDevice): Windows' names are anyone's. Only
    the headers included by name (every header of the SDK is 25 s to read), from the newest SDK on this machine."""
    kits = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Windows Kits', '10', 'Include')
    versions = sorted(v for v in os.listdir(kits) if v.startswith('10.')) if os.path.isdir(kits) else []
    if not versions:
        return set()
    included = subprocess.run(['git', 'grep', '-h', '-o', '-E', r'#include\s*<[^>]+>', '--', '*.c', '*.cpp', '*.h', '*.hpp',
                               '*.inc'], cwd=ROOT, capture_output=True, text=True).stdout
    headers = {m.group(1) for m in re.finditer(r'<([^>]+)>', included)}
    names = set()
    for header in headers:
        for sub in ('um', 'shared'):
            path = os.path.join(kits, versions[-1], sub, header)
            if os.path.isfile(path):
                with open(path, encoding='utf-8', errors='replace') as f:
                    names.update(re.findall(r'[A-Za-z_][A-Za-z0-9_]*', f.read()))
    return names


def parts(name):
    """The pieces a name is written as in text: the whole one and its last part (a method's own name) when that is
    long and specific enough to tell (not `Tick`, `Open`, a destructor...)."""
    name = name.strip()
    out = {name}
    last = name.split('::')[-1]
    if len(last) >= 9 and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', last) and not last.startswith(('m_', 'Get', 'Set')):
        out.add(last)
    return out


def main():
    demo = demo_names()
    if not demo:
        print('demo names: none on this machine (no knowledge/ls3df.dll/demo*.local.csv): nothing to check')
        return 0
    others = other_names()
    other_parts = set()
    for n in others:
        other_parts |= parts(n)
    own = set()
    for n in demo - others:
        if public(n):
            continue
        for p in parts(n):
            if p not in other_parts and re.search(r'[A-Za-z]', p) and not public(p):
                own.add(p)
    # what the repository carried before the demo was read is ours (or the runtime's, Windows', DirectShow's: names
    # anyone has), whatever the demo also calls so
    before = subprocess.run(['git', 'grep', '-h', '-o', '-I', '-E', r'[A-Za-z_][A-Za-z0-9_]*(::[~A-Za-z_][A-Za-z0-9_]*)*',
                             BEFORE_DEMO], cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if before.returncode not in (0, 1):
        print('demo names: the commit before the demo (%s) is not in this clone (a shallow one?): %s'
              % (BEFORE_DEMO, before.stderr.strip()[:200]))
        return 1
    known = set(before.stdout.split()) | sdk_names()
    own = {p for p in own if p not in known}
    files = subprocess.run(['git', 'ls-files'], cwd=ROOT, capture_output=True, text=True).stdout.split('\n')
    # a name is looked for among each line's words (`a::b::c` and every run of its parts): one regex of 1500
    # alternatives over every line took 16 s; the few names that are not plain words (templates...) keep the regex
    word = re.compile(r'[~A-Za-z_][A-Za-z0-9_]*(?:::[~A-Za-z_][A-Za-z0-9_]*)*')
    plain = {p for p in own if word.fullmatch(p)}
    rest = own - plain
    pattern = rest and re.compile(r'(?<![A-Za-z0-9_])(' + '|'.join(re.escape(p) for p in sorted(rest, key=len, reverse=True))
                                  + r')(?![A-Za-z0-9_])')
    hits = {}
    for rel in files:
        if not rel or not rel.lower().endswith(TEXT) or '.local.' in rel:
            continue
        try:
            text = open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for i, line in enumerate(text.split('\n'), 1):
            found = []
            for w in word.findall(line):
                ps = w.split('::')
                found += [n for n in ('::'.join(ps[a:b]) for a in range(len(ps)) for b in range(a + 1, len(ps) + 1))
                          if n in plain]
            if pattern:
                found += [m.group(1) for m in pattern.finditer(line)]
            for n in found:
                hits.setdefault(n, []).append('%s:%d' % (rel, i))
    if '--list' in sys.argv:
        for n in sorted(hits, key=lambda k: -len(hits[k])):
            print('%-50s %3d  %s' % (n, len(hits[n]), ', '.join(hits[n][:4])))
    if hits:
        print('demo names: %d of the demo\'s own names in %d places of the repository (python tools/check_demo_names.py '
              '--list)' % (len(hits), sum(len(v) for v in hits.values())))
        return 1
    print('demo names: none of the demo\'s %d own names in the repository' % len(own))
    return 0


if __name__ == '__main__':
    sys.exit(main())
