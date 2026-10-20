"""ghidra_names.py: the names, prototypes and comments the repository gives each build's Ghidra project.

Standard library only (tools/ghidra_server.py applies them; tests/run_tests.py checks them with no Ghidra). Each
name has a rank; at one address the highest is the primary name:

  1 other projects'   knowledge/<module>/external.local.csv (made on this machine, never committed)
  2 st0rm94's         knowledge/<module>/external.csv (his 1.0 names carried to 1.2)
  3 the SDK's         game_build.h's tables (1.2's rows also in names.csv, source game_build.h, from kb_import.py)
  4 MafiaReverse's    knowledge/<module>/functions.csv (the decompile's own names, with their classes); over
                      them the Mafia demo's (LS3DF.dll: knowledge/ls3df.dll/demo.local.csv, demo_10.local.csv, made
                      on this machine by tools/kb_demo.py from the demo's LS3D.dll, never committed)
  5 ours              knowledge/<module>/names.csv (1.2), names_10.csv (1.0), but the SDK's names among them
                      (the game_build.h rows, and any that is one of its fields: mafia-ragdoll's)

The SDK's names are the loader's vocabulary, not better names: one that is not the primary is kept as a second
name at its address (an alias: `at=humanHit` still finds C_human::Hit). 1.0 takes every 1.2 name of a function that
has a twin (knowledge/<module>/twins_10.csv) at the twin's address, at the rank it has in 1.2, then its own:
st0rm94's at his 1.0 addresses (external_10.csv, rank 2: the twinless functions too), the demo's (demo_10.local.csv),
the SDK's 1.0 tables and names_10.csv. Prototypes: MafiaReverse's signatures (functions.csv), turned into C Ghidra
parses, where they are plain enough (`c_signature`); the demo's own (`demo_signature`: the mangled names' types)
over them in 1.0, beside them in 1.2; ours in knowledge/<module>/annotations.csv (`build,address,kind,text,source`, kind proto or comment)
over all.
"""
import csv
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KB = os.path.join(ROOT, 'knowledge')
MODULE_DIRS = {'Game.exe': 'game.exe', 'LS3DF.dll': 'ls3df.dll'}
NAME_COLUMNS = ['address', 'kind', 'name', 'source', 'note']
NOTE_COLUMNS = ['build', 'address', 'kind', 'text', 'source']
SDK = 'game_build.h'
DEMO = {'1.2': 'demo.local.csv', '1.0': 'demo_10.local.csv'}   # tools/kb_demo.py's names (LS3DF.dll)


def path(module, file):
    return os.path.join(KB, MODULE_DIRS[module], file)


def names_file(module, build):
    return path(module, 'names.csv' if build == '1.2' else 'names_10.csv')


def rows(p):
    if not os.path.exists(p):
        return []
    with open(p, encoding='utf-8', newline='') as f:
        return list(csv.DictReader(f))


class Name:
    __slots__ = ('address', 'kind', 'name', 'source', 'rank')

    def __init__(self, address, kind, name, source, rank):
        self.address, self.kind, self.name, self.source, self.rank = address, kind, name, source, rank

    def __repr__(self):
        return '%08X %s %s (%s, %d)' % (self.address, self.kind, self.name, self.source, self.rank)


class Names:
    """The names of a module of a build: `best` {address: Name}, `aliases` {address: [SDK names not the primary]}."""

    def __init__(self):
        self.best, self.aliases = {}, {}

    def put(self, n):
        cur = self.best.get(n.address)
        if cur is None or n.rank >= cur.rank:
            self.best[n.address] = n
        if n.source == SDK:
            self.aliases.setdefault(n.address, [])
            if n.name not in self.aliases[n.address]:
                self.aliases[n.address].append(n.name)

    def done(self):
        for at in list(self.aliases):
            self.aliases[at] = [a for a in self.aliases[at] if a != self.best[at].name]
            if not self.aliases[at]:
                del self.aliases[at]
        return self


def sdk_tables(build):
    """(address, name) of game_build.h's address fields for one build: Game.exe absolute, LS3DF.dll at 0x10000000."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gen_rust_game as g
    text = open(os.path.join(ROOT, 'sdk', 'include', 'mml', 'game', 'game_build.h'), encoding='utf-8').read()
    out = []
    for struct, table, base in (('ExeBuild', 'kExe12' if build == '1.2' else 'kExe10', 0),
                                ('Ls3dBuild', 'kLs3d12' if build == '1.2' else 'kLs3d10', 0x10000000)):
        for (field, ty), v in zip(g.fields(text, struct), g.initializer(text, table)):
            if ty == 'usize' and not v.startswith('"'):
                off = int(v, 0)
                if off:
                    out.append((base + off, field))
    return out


_SDK_FIELDS = None


def sdk_fields():
    """Every address field's name of game_build.h's tables (the loader's vocabulary: missionTick, humanHit...)."""
    global _SDK_FIELDS
    if _SDK_FIELDS is None:
        _SDK_FIELDS = {f for b in ('1.2', '1.0') for _, f in sdk_tables(b)}
    return _SDK_FIELDS


def ours(r):
    """A names.csv row's rank: a game_build.h row, or a name that is one of its fields (mafia-ragdoll gave those
    names to the addresses its SDK uses), is the SDK's: an alias beside a better name."""
    return 3 if r['source'] == SDK or r['name'] in sdk_fields() else 5


def names_12(module):
    ns = Names()
    for r in rows(path(module, 'external.local.csv')):
        ns.put(Name(int(r['address'], 16), r['kind'], r['name'], r['source'], 1))
    for r in rows(path(module, 'external.csv')):
        ns.put(Name(int(r['address'], 16), r['kind'], r['name'], r['source'], 2))
    for at, field in sdk_tables('1.2'):
        if (at >= 0x10000000) == (module == 'LS3DF.dll'):
            ns.put(Name(at, 'sdk', field, SDK, 3))
    for r in rows(path(module, 'functions.csv')):
        if r['name']:
            ns.put(Name(int(r['address'], 16), 'function', r['name'], 'MafiaReverse', 4))
    for r in rows(path(module, DEMO['1.2'])):   # (after MafiaReverse's: at one rank the later wins)
        ns.put(Name(int(r['address'], 16), r['kind'], r['name'], r['source'], 4))
    for r in rows(names_file(module, '1.2')):
        ns.put(Name(int(r['address'], 16), r['kind'], r['name'], r['source'], ours(r)))
    return ns.done()


def twins(module='Game.exe'):
    """1.2 address -> 1.0 address of a module's functions paired by their code (Game.exe: tools/kb_external.py,
    LS3DF.dll: tools/kb_demo.py)."""
    return {int(r['va12'], 16): int(r['va10'], 16) for r in rows(path(module, 'twins_10.csv'))}


def names(module, build):
    """The names the repository gives a module of a build (Names: best, aliases)."""
    if build == '1.2':
        return names_12(module)
    ns = Names()
    tw, n12 = twins(module), names_12(module)
    for at12, n in n12.best.items():
        if at12 in tw and n.kind in ('function', 'sdk'):
            ns.put(Name(tw[at12], 'function', n.name, n.source + ' (1.2 %08X)' % at12, n.rank))
    # st0rm94's own names at their 1.0 addresses (external_10.csv): every function of his, the twinless too
    for r in rows(path(module, 'external_10.csv')):
        ns.put(Name(int(r['address'], 16), r['kind'], r['name'], r['source'], 2))
    # the demo's at their own 1.0 addresses: its globals, and the functions with no 1.2 twin
    for r in rows(path(module, DEMO['1.0'])):
        ns.put(Name(int(r['address'], 16), r['kind'], r['name'], r['source'], 4))
    for at, field in sdk_tables('1.0'):
        if (at >= 0x10000000) == (module == 'LS3DF.dll'):
            ns.put(Name(at, 'sdk', field, SDK, 3))
    for r in rows(names_file(module, '1.0')):
        ns.put(Name(int(r['address'], 16), r['kind'], r['name'], r['source'], ours(r)))
    return ns.done()


# ---------------------------------------------------------------- prototypes

# Hex-Rays' own types, as C Ghidra's parser knows
TYPE_WORDS = {
    '_DWORD': 'unsigned int', '_BYTE': 'unsigned char', '_WORD': 'unsigned short', '_QWORD': 'unsigned long long',
    '__int8': 'char', '__int16': 'short', '__int32': 'int', '__int64': 'long long', '_BOOL1': 'bool', '_BOOL4': 'int',
    'BOOL': 'int', 'DWORD': 'unsigned int', 'WORD': 'unsigned short', 'BYTE': 'unsigned char', 'LPVOID': 'void *',
    'LPCVOID': 'void *', 'HANDLE': 'void *', 'LPCSTR': 'char *', 'LPSTR': 'char *', 'size_t': 'unsigned int',
}
C_WORDS = {'void', 'char', 'short', 'int', 'long', 'float', 'double', 'bool', 'signed', 'unsigned', 'const',
           'volatile', 'struct'}
CONVENTIONS = ('__thiscall', '__stdcall', '__fastcall', '__cdecl')


def c_type(t):
    """A Hex-Rays type as C Ghidra parses: its own words mapped, a pointer to an unknown type `void *` (as deep);
    None for a value of an unknown type (a struct by value: its size is not known here)."""
    t = ' '.join(t.replace('*', ' * ').split())
    for k, v in TYPE_WORDS.items():
        t = re.sub(r'\b%s\b' % re.escape(k), v, t)
    t = re.sub(r'\bsigned\s+(?=char|short|int|long)', '', t)
    stars = t.count('*')
    base = ' '.join(w for w in t.replace('*', ' ').split() if w not in ('const', 'volatile', 'struct'))
    words = base.split()
    if not words:
        return None
    if all(w in C_WORDS for w in words):
        return base + (' ' + '*' * stars if stars else '')
    if stars:
        return 'void ' + '*' * stars
    return None


def c_signature(sig):
    """A functions.csv signature as a C prototype Ghidra's parser takes ("int __thiscall f(int a2)"), or None:
    register-passed arguments (__usercall, __userpurge), varargs, a value of an unknown type, a failed analysis.
    __thiscall's first argument (this, in ECX) is left out: Ghidra gives a thiscall function its own."""
    if not sig or 'error' in sig or 'failed' in sig or '__user' in sig or '...' in sig or '@<' in sig:
        return None
    m = re.fullmatch(r'\s*(.*?)\s*\b(__thiscall|__stdcall|__fastcall|__cdecl)?\s*([~A-Za-z_][\w:~<>,]*)\s*\((.*)\)\s*', sig)
    if not m:
        return None
    ret, conv, _, params = m.groups()
    ret = c_type(ret)
    if ret is None:
        return None
    out = []
    for i, p in enumerate(x.strip() for x in params.split(',') if x.strip() and x.strip() != 'void'):
        pm = re.fullmatch(r'(.*?)([A-Za-z_]\w*)', p)
        if not pm:
            return None
        ty, name = c_type(pm.group(1)), pm.group(2)
        if ty is None:
            return None
        if i == 0 and conv == '__thiscall':   # (ECX: whatever Hex-Rays called it)
            continue
        out.append('%s %s' % (ty, name))
    return '%s %sf(%s)' % (ret, (conv + ' ') if conv else '', ', '.join(out) or 'void')


def demo_signature(sig):
    """A demangled signature of the demo's ("public: virtual bool __stdcall C_x::IsOn(void)") as a C
    prototype Ghidra's parser takes, or None: a struct by value (in or out: its size is not known here), varargs, a
    function pointer's. A member's `this`: __thiscall's in ECX (Ghidra's own), any other convention's the first
    argument on the stack."""
    m = re.fullmatch(r'\s*(?:(?:public|protected|private): )?((?:virtual |static )*)(.*?)\s*\b(__thiscall|__stdcall|'
                     r'__fastcall|__cdecl)\s+([^(]+)\((.*)\)\s*(const)?\s*', sig or '')
    if not m or '(' in m.group(5) or '...' in m.group(5):
        return None
    flags, ret, conv, name, params = m.groups()[:5]
    # (only a member is demangled with its access; a static one has no this)
    member = bool(re.match(r'\s*(public|protected|private):', sig)) and 'static' not in flags
    if not ret:   # a constructor or a destructor
        ret = 'void'

    def ty(t):
        t = t.strip().replace('class ', '').replace('struct ', '').replace('enum ', 'enum:')
        if t.startswith('enum:'):
            return 'int' if '*' not in t and '&' not in t else 'void *'
        if '&' in t or '*' in t:
            base = t.replace('&', '').replace('*', '').replace('const', '').strip()
            stars = max(1, t.count('*') + t.count('&'))
            return (c_type(base) or 'void') + ' ' + '*' * stars if c_type(base) else 'void ' + '*' * stars
        return c_type(t.replace('const', ''))
    r = ty(ret)
    if r is None:
        return None
    out = []
    for i, p in enumerate(x for x in params.split(',') if x.strip() and x.strip() != 'void'):
        t = ty(p)
        if t is None:
            return None
        out.append('%s a%d' % (t, i + 1))
    if member and conv != '__thiscall':
        out.insert(0, 'void * this')
    return '%s %s f(%s)' % (r, conv, ', '.join(out) or 'void')


def demo_signatures(module, build):
    """{address: C prototype} of the demo's names' signatures (the names file's note: "confidence; how score; demo
    address[ via 1.0 address]; signature[; ...]")."""
    out = {}
    if module != 'LS3DF.dll':
        return out
    for r in rows(path(module, DEMO[build])):
        parts = r['note'].split('; ')
        if r['kind'] == 'function' and len(parts) >= 4 and parts[0] == 'high':
            c = demo_signature(parts[3])
            if c:
                out[int(r['address'], 16)] = c
    return out


def signatures(module, build):
    """{address: C prototype} of MafiaReverse's signatures (1.2, and 1.0 through the twins) and the demo's: in 1.0
    (four weeks after the demo's LS3D.dll) the demo's over 1.2's, in 1.2 (a year and a half after it: a method's
    arguments may have changed) only where MafiaReverse has none."""
    out = {}
    for r in rows(path(module, 'functions.csv')):
        c = c_signature(r['signature'])
        if c:
            out[int(r['address'], 16)] = c
    if build == '1.0':
        tw = twins(module)
        out = {tw[a]: c for a, c in out.items() if a in tw}
    for a, c in demo_signatures(module, build).items():
        if build == '1.0' or a not in out:
            out[a] = c
    return out


def annotations(module, build):
    return [r for r in rows(path(module, 'annotations.csv')) if r['build'] == build]


def split_name(name):
    """A C++ name's namespace path and its last part: "C_human::Hit" -> (["C_human"], "Hit"); "::" inside <> or ()
    (templates, operators' arguments) does not split."""
    parts, depth, cur, i = [], 0, '', 0
    while i < len(name):
        c = name[i]
        if c in '<(':
            depth += 1
        elif c in '>)' and depth:
            depth -= 1
        if depth == 0 and name.startswith('::', i):
            parts.append(cur)
            cur, i = '', i + 2
            continue
        cur += c
        i += 1
    parts.append(cur)
    parts = [p for p in parts if p]
    return parts[:-1], (parts[-1] if parts else name)


def upsert(p, columns, key, row):
    """A row put into a CSV by its key columns (replaced when there, else added), sorted, LF endings."""
    have = rows(p)
    k = tuple(row[c] for c in key)
    have = [r for r in have if tuple(r[c] for c in key) != k] + [row]
    have.sort(key=lambda r: tuple(r[c] for c in key))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8', newline='') as f:
        w = csv.DictWriter(f, fieldnames=columns, lineterminator='\n')
        w.writeheader()
        for r in have:
            w.writerow({c: r.get(c, '') for c in columns})


def record_name(module, build, address, kind, name, note, source='mml'):
    upsert(names_file(module, build), NAME_COLUMNS, ['address'],
           {'address': '%08X' % address, 'kind': kind, 'name': name, 'source': source, 'note': note or ''})


def forget(module, build, address):
    """Our rows for an address taken out of names.csv / names_10.csv and annotations.csv; what was removed."""
    gone = []
    for p, cols, keep in ((names_file(module, build), NAME_COLUMNS, lambda r: int(r['address'], 16) != address),
                          (path(module, 'annotations.csv'), NOTE_COLUMNS,
                           lambda r: r['build'] != build or int(r['address'], 16) != address)):
        have = rows(p)
        left = [r for r in have if keep(r)]
        if len(left) == len(have):
            continue
        gone += ['%s %s' % (os.path.basename(p), r.get('name') or r.get('kind')) for r in have if not keep(r)]
        if not left:   # (a file of ours left with no row: gone)
            os.remove(p)
            continue
        with open(p, 'w', encoding='utf-8', newline='') as f:
            w = csv.DictWriter(f, fieldnames=cols, lineterminator='\n')
            w.writeheader()
            for r in left:
                w.writerow({c: r.get(c, '') for c in cols})
    return gone


def record_annotation(module, build, address, kind, text, source='mml'):
    upsert(path(module, 'annotations.csv'), NOTE_COLUMNS, ['build', 'address', 'kind'],
           {'build': build, 'address': '%08X' % address, 'kind': kind, 'text': text, 'source': source})


if __name__ == '__main__':   # a summary: python tools/ghidra_names.py
    for b in ('1.2', '1.0'):
        for m in MODULE_DIRS:
            ns = names(m, b)
            by = {}
            for n in ns.best.values():
                by[n.rank] = by.get(n.rank, 0) + 1
            print('%s %s: %d names %s, %d with SDK aliases, %d prototypes' % (
                b, m, len(ns.best), dict(sorted(by.items())), len(ns.aliases), len(signatures(m, b))))
