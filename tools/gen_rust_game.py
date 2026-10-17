"""gen_rust_game.py: the game's addresses and layouts for Rust mods, from the C++ SDK's headers.

    python tools/gen_rust_game.py           write sdk/rust/mml/src/game/tables.rs
    python tools/gen_rust_game.py --check   exit 1 when tables.rs is not what the headers give

sdk/include/mml/game/game_build.h holds every address the code uses, per build (ExeBuild for Game.exe, Ls3dBuild for
LS3DF.dll's RVAs, the tables kExe12, kExe10, kLs3d12, kLs3d10) and the signatures game::Detect tells the builds by;
racing.h the race's layouts (RacingLayout, kRacing12, kRacing10); game.h and racing.h the layouts' offsets
(`constexpr int kActorFrame = 0x68;`); knowledge/game.exe/script_commands.csv the scripts' commands' numbers in each
build. The Rust crate gets the same, made from them here: one source for both languages (game_build.h is also
mafia-ragdoll's, and tools/check_builds.py checks it against the game's files). The fields keep the C++ names
(missionPtr, vehSpeed, kActorFrame), so a field is found by the same name in both and in docs/VERSIONS.md.
tests/run_tests.py runs the check.
"""
import csv
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(ROOT, 'sdk', 'include', 'mml', 'game')
CSV = os.path.join(ROOT, 'knowledge', 'game.exe', 'script_commands.csv')
OUT = os.path.join(ROOT, 'sdk', 'rust', 'mml', 'src', 'game', 'tables.rs')

TYPES = {'uintptr_t': 'usize', 'int': 'i32', 'uint32_t': 'u32', 'float': 'f32', 'const char *': '&\'static str'}


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)


def fields(text, struct):
    """A struct's members in order: [(name, rust type)]."""
    m = re.search(r'struct %s \{(.*?)\n\};' % struct, text, re.S)
    if not m:
        sys.exit('%s: no struct %s' % (OUT, struct))
    out = []
    for decl in strip_comments(m.group(1)).split(';'):
        decl = ' '.join(decl.split())
        if not decl:
            continue
        if decl.startswith('const char *'):
            out.append((decl[len('const char *'):].strip(), TYPES['const char *']))
            continue
        ty, _, names = decl.partition(' ')
        if ty not in TYPES:
            sys.exit('struct %s: a member of a type this tool does not know: %r' % (struct, decl))
        for n in names.split(','):
            n = n.strip()
            if not re.fullmatch(r'[A-Za-z_]\w*', n):
                sys.exit('struct %s: %r is not a plain member' % (struct, n))
            out.append((n, TYPES[ty]))
    return out


def initializer(text, name):
    """A table's values in order: the literals of `... name = { ... };`."""
    m = re.search(r'\b%s = \{(.*?)\};' % name, strip_comments(text), re.S)
    if not m:
        sys.exit('no table %s' % name)
    vals = [v.strip() for v in m.group(1).split(',') if v.strip()]
    for v in vals:
        if not re.fullmatch(r'"[^"]*"|-?0x[0-9A-Fa-f]+|-?\d+', v):
            sys.exit('table %s: %r is not a literal this tool reads' % (name, v))
    return vals


def table(struct, rust, name, flds, vals):
    if len(vals) != len(flds):
        sys.exit('table %s: %d values for the %d members of %s' % (name, len(vals), len(flds), struct))
    rows = ''.join('    %s: %s,\n' % (f, v) for (f, _), v in zip(flds, vals))
    return 'pub static %s: %s = %s {\n%s};\n' % (rust, struct_name(struct), struct_name(struct), rows)


def struct_name(c):
    return {'ExeBuild': 'Exe', 'Ls3dBuild': 'Ls3d', 'RacingLayout': 'Racing'}[c]


def struct_def(c, flds, doc):
    rows = ''.join('    pub %s: %s,\n' % (f, t) for f, t in flds)
    return '%s#[derive(Debug)]\npub struct %s {\n%s}\n' % (doc, struct_name(c), rows)


def constants(text, where):
    out = []
    for stmt in re.findall(r'(?:inline )?constexpr ([^;]*);', strip_comments(text)):
        stmt = ' '.join(stmt.split())
        ty, _, rest = stmt.partition(' ')
        if ty not in ('int', 'uint32_t', 'float'):
            continue   # (aggregates: the tables, read above)
        for part in rest.split(','):
            n, _, v = (s.strip() for s in part.partition('='))
            if ty == 'float':
                v = v.rstrip('f')
                if '.' not in v:
                    v += '.0'
            if not re.fullmatch(r'k\w+', n) or not re.fullmatch(r'-?0x[0-9A-Fa-f]+|-?\d+(\.\d+)?', v):
                sys.exit('%s: a constant this tool does not read: %r' % (where, part))
            out.append('pub const %s: %s = %s;\n' % (n, TYPES[ty], v))
    return out


def signatures(text):
    m = re.search(r'kSigs\[\] = \{(.*?)\n\};', text, re.S)
    out, fns = [], []
    for exe, ls3d, body in re.findall(r'\{(&ExeBuild::\w+|nullptr), (&Ls3dBuild::\w+|nullptr), \{([^}]*)\}\}', m.group(1)):
        b = [x.strip() for x in body.split(',')]
        if len(b) != 16:
            sys.exit('kSigs: a signature of %d bytes' % len(b))
        if exe != 'nullptr':
            f = exe.split('::')[1]
            out.append('    Sig { exe: Some(sig_exe_%s), ls3d: None, bytes: [%s] },\n' % (f, ', '.join(b)))
            fns.append('fn sig_exe_%s(e: &Exe) -> usize {\n    e.%s\n}\n' % (f, f))
        else:
            f = ls3d.split('::')[1]
            out.append('    Sig { exe: None, ls3d: Some(sig_ls3d_%s), bytes: [%s] },\n' % (f, ', '.join(b)))
            fns.append('fn sig_ls3d_%s(l: &Ls3d) -> usize {\n    l.%s\n}\n' % (f, f))
    if not out:
        sys.exit('game_build.h: no kSigs read')
    return ('/// What game::Detect checks: functions whose first 16 bytes are the same in every build, at each build\'s address.\n'
            'pub struct Sig {\n    pub exe: Option<fn(&Exe) -> usize>,\n    pub ls3d: Option<fn(&Ls3d) -> usize>,\n'
            '    pub bytes: [u8; 16],\n}\n' + ''.join(fns) + 'pub static SIGS: &[Sig] = &[\n' + ''.join(out) + '];\n')


def commands():
    rows = []
    for r in csv.DictReader(open(CSV, encoding='utf-8')):
        name = r['command'].strip().lower()
        if not name:
            continue
        op12 = int(r['opcode']) if r['opcode'].strip() else -1
        op10 = int(r['opcode_10']) if r['opcode_10'].strip() else -1
        rows.append('    ("%s", %d, %d),\n' % (name, op12, op10))
    return ('/// The scripts\' commands: (name, its number in 1.2, in 1.0; -1 not in that build).\n'
            'pub static SCRIPT_COMMANDS: &[(&str, i16, i16)] = &[\n' + ''.join(rows) + '];\n')


def render():
    build = open(os.path.join(GAME, 'game_build.h'), encoding='utf-8').read()
    racing = open(os.path.join(GAME, 'racing.h'), encoding='utf-8').read()
    game = open(os.path.join(GAME, 'game.h'), encoding='utf-8').read()
    exe, ls3d, rac = fields(build, 'ExeBuild'), fields(build, 'Ls3dBuild'), fields(racing, 'RacingLayout')
    parts = [
        '// Made by tools/gen_rust_game.py from sdk/include/mml/game/game_build.h, racing.h, game.h and\n'
        '// knowledge/game.exe/script_commands.csv: do not edit (run it again after a change there). What each field\n'
        '// is: the C++ header, member by member, under the same name.\n'
        '#![allow(non_snake_case, non_upper_case_globals, clippy::all)]\n\n',
        struct_def('ExeBuild', exe, '/// Game.exe\'s addresses (absolute; 0: not known for this build): game_build.h ExeBuild.\n'),
        struct_def('Ls3dBuild', ls3d, '/// LS3DF.dll\'s, as RVAs (add the module\'s base; the int members are offsets): game_build.h Ls3dBuild.\n'),
        struct_def('RacingLayout', rac, '/// The race of mission 6: racing.h RacingLayout.\n'),
        table('ExeBuild', 'EXE_12', 'kExe12', exe, initializer(build, 'kExe12')),
        table('ExeBuild', 'EXE_10', 'kExe10', exe, initializer(build, 'kExe10')),
        table('Ls3dBuild', 'LS3D_12', 'kLs3d12', ls3d, initializer(build, 'kLs3d12')),
        table('Ls3dBuild', 'LS3D_10', 'kLs3d10', ls3d, initializer(build, 'kLs3d10')),
        table('RacingLayout', 'RACING_12', 'kRacing12', rac, initializer(racing, 'kRacing12')),
        table('RacingLayout', 'RACING_10', 'kRacing10', rac, initializer(racing, 'kRacing10')),
        signatures(build),
        '\n// game.h\'s layouts (the same in both builds)\n',
        ''.join(constants(game, 'game.h')),
        '\n// racing.h\'s\n',
        ''.join(constants(racing, 'racing.h')),
        '\n',
        commands(),
    ]
    return '\n'.join(p if p.endswith('\n') else p + '\n' for p in parts).replace('\n\n\n', '\n\n')


def main():
    text = render()
    if '--check' in sys.argv:
        old = open(OUT, encoding='utf-8').read().replace('\r\n', '\n') if os.path.exists(OUT) else ''
        if old != text:
            print('sdk/rust/mml/src/game/tables.rs is not what the game headers give: run tools/gen_rust_game.py')
            sys.exit(1)
        print('tables.rs: up to date')
        return
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('written', os.path.relpath(OUT, ROOT))


if __name__ == '__main__':
    main()
