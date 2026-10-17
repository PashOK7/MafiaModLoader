"""ghidra_cli.py: the game's two builds in Ghidra, for agents and people: read, search, name, annotate.

    python tools/deps.py ghidra                       once: Ghidra 12.1.4 and its PyGhidra venv (third_party/_deps)
    python tools/ghidra_cli.py setup [--build 1.2|1.0]    the projects: import, analysis, the knowledge base's names
    python tools/ghidra_cli.py [--build 1.0] <op> [args]  ask (the build's server starts at the first call, ~20 s)
    python tools/ghidra_cli.py status | stop [--build]

The projects (re/ghidra/, never committed) are a cache: `setup` makes them from the labs' Game.exe and LS3DF.dll
(mml.config.json `labs`) and the repository. What is learned in them goes back into the repository at once: a name
into knowledge/<module>/names.csv (1.2) or names_10.csv (1.0), a comment or a prototype into
knowledge/<module>/annotations.csv, so `setup` on another machine makes the same projects. The names a project
starts with, by rank (a higher one wins at an address): other projects' (external.local.csv), st0rm94's
(external.csv; his own 1.0 addresses in 1.0), MafiaReverse's (functions.csv, with their classes), the SDK's
(game_build.h), ours (names.csv, names_10.csv); 1.0 also takes 1.2's names through twins_10.csv.

One server a build (1.2 on port 47031, 1.0 on 47032), each holding its project open with both modules, so both
builds are asked at once; the MCP server's ghidra_* tools talk to the same servers. Ops (`help` lists them):

  read:  decompile <at> [grep= context= range= max=]   asm <at> [count=]   function <at>   callers <at>
         callees <at>   xrefs <at>   vtable <at> [count=]   bytes <at> [count=]   twin <at> (the other build's)
  find:  imm <hex> [module= in= writes= text=]   string <regex> [module=]   value <hex> | bytes "8B 4C 24" [module=]
         names [module= all=]   functions <regex> [module=]
  write: rename <at> <name> [note=]   comment <at> <text>   proto <at> <C prototype>   make_function <at>
         unfunction <at>   forget <at> (ours taken back: the repository's rows, the project's name, the
         repository's other names and prototype again)   sync (the repository onto the project)   orphans [fix=1]

`setup` imports and analyses what a project lacks (Game.exe about 4 minutes, LS3DF.dll 1), makes functions of the
code the analysis left outside them (reached only through pointers: `orphans fix=1`), then `sync`s: the names, the
SDK's aliases, MafiaReverse's prototypes (ghidra_names.c_signature) and our annotations. The JDK: mml.config.json
`java_home`, JAVA_HOME, else the newest 21+ in Program Files.

Addresses: 0x5762A0 or 5762A0 (Game.exe; from 0x10000000: LS3DF.dll), ls3d+1AAE0 (an LS3DF RVA, as the loader
writes them), game+0x1762A0, or a name (Game.exe first, then LS3DF.dll).
"""
import csv
import json
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, 'third_party', '_deps')
GHIDRA = os.path.join(DEPS, 'ghidra')
VENV_PY = os.path.join(DEPS, 'pyghidra', 'Scripts', 'python.exe')
PROJECTS = os.path.join(ROOT, 're', 'ghidra')
KB = os.path.join(ROOT, 'knowledge')
BUILDS = {'1.2': ('mafia12', 47031), '1.0': ('mafia10', 47032)}
MODULES = ('Game.exe', 'LS3DF.dll')
LS3D_BASE = 0x10000000
GAME_BASE = 0x400000


def config():
    p = os.path.join(ROOT, 'mml.config.json')
    return json.load(open(p, encoding='utf-8')) if os.path.exists(p) else {}


def lab(build):
    d = (config().get('labs') or {}).get(build)
    if not d:
        sys.exit('ghidra_cli.py: no lab for %s in mml.config.json ("labs")' % build)
    return d


# ---------------------------------------------------------------- the JDK

def java_version(home):
    """The major version of a JDK folder (its `release` file), 0 when it is none."""
    try:
        text = open(os.path.join(home, 'release'), encoding='utf-8', errors='replace').read()
    except OSError:
        return 0
    m = re.search(r'JAVA_VERSION="(\d+)', text)
    return int(m.group(1)) if m and os.path.exists(os.path.join(home, 'bin', 'java.exe')) else 0


def java_home():
    """A JDK 21 or later: mml.config.json's `java_home`, JAVA_HOME, else the newest in the usual folders."""
    for c in (config().get('java_home'), os.environ.get('JAVA_HOME')):
        if c and java_version(c) >= 21:
            return c
    found = []
    for base in (r'C:\Program Files', r'C:\Program Files (x86)'):
        for vendor in ('Amazon Corretto', 'Eclipse Adoptium', 'Microsoft', 'Java', 'Zulu', 'BellSoft', 'OpenJDK'):
            d = os.path.join(base, vendor)
            for e in (os.listdir(d) if os.path.isdir(d) else []):
                v = java_version(os.path.join(d, e))
                if v >= 21:
                    found.append((v, os.path.join(d, e)))
    if not found:
        sys.exit('ghidra_cli.py: no JDK 21 or later (Ghidra 12 needs one): install one, or name it in mml.config.json '
                 '"java_home"')
    return max(found)[1]


def need_install():
    if not os.path.exists(os.path.join(GHIDRA, 'ghidraRun.bat')) or not os.path.exists(VENV_PY):
        sys.exit('ghidra_cli.py: Ghidra is not installed: python tools/deps.py ghidra')


# ---------------------------------------------------------------- setup: import and analysis

def analyze(build, force):
    """Each module of the build's lab imported into its project and analysed by Ghidra's headless analyser (Game.exe
    about 4 minutes, LS3DF.dll 1). A module already in the project is left as it is unless `force`."""
    need_install()
    name, _ = BUILDS[build]
    os.makedirs(PROJECTS, exist_ok=True)
    env = dict(os.environ, JAVA_HOME=java_home())
    env['PATH'] = os.path.join(env['JAVA_HOME'], 'bin') + os.pathsep + env.get('PATH', '')
    rep = os.path.join(PROJECTS, name + '.rep')
    for module in MODULES:
        there = os.path.isdir(rep) and module_in_project(rep, module)
        if there and not force:
            print('%s %s: in the project' % (build, module))
            continue
        src = os.path.join(lab(build), module)
        log = os.path.join(PROJECTS, '%s_%s.log' % (name, module))
        print('%s %s: importing and analysing %s (log %s)' % (build, module, src, log), flush=True)
        t0 = time.time()
        r = subprocess.run([os.path.join(GHIDRA, 'support', 'analyzeHeadless.bat'), PROJECTS, name, '-import', src,
                            '-overwrite', '-log', log], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        ok = r.returncode == 0 and 'Import succeeded' in open(log, encoding='utf-8', errors='replace').read()
        print('%s %s: %s in %.0f s' % (build, module, 'analysed' if ok else 'FAILED (see the log)', time.time() - t0), flush=True)
        if not ok:
            sys.exit(1)


def module_in_project(rep, module):
    """Whether the project holds a program of that name (its index files name each one)."""
    for dp, _, files in os.walk(rep):
        for f in files:
            if f.endswith('.prp'):
                try:
                    if ('VALUE="%s"' % module) in open(os.path.join(dp, f), encoding='utf-8', errors='replace').read():
                        return True
                except OSError:
                    pass
    return False


# ---------------------------------------------------------------- the client

def up(port):
    try:
        socket.create_connection(('127.0.0.1', port), timeout=1).close()
        return True
    except OSError:
        return False


def start(build, wait=True):
    """The build's server, started when it is not running (a hidden process of its own; its log in re/ghidra)."""
    need_install()
    name, port = BUILDS[build]
    if up(port):
        return
    log = os.path.join(PROJECTS, 'server_%s.log' % name)
    os.makedirs(PROJECTS, exist_ok=True)
    flags = 0x00000200 | 0x08000000  # CREATE_NEW_PROCESS_GROUP, CREATE_NO_WINDOW
    subprocess.Popen([VENV_PY, os.path.abspath(__file__), '--serve', build], cwd=ROOT, stdout=open(log, 'w'),
                     stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=flags, close_fds=True)
    t0 = time.time()
    while wait and not up(port):
        if time.time() - t0 > 300:
            sys.exit('ghidra_cli.py: the %s server did not start; see %s' % (build, log))
        time.sleep(0.5)


def ask(build, op, args, timeout=900):
    """One request to the build's server: {"op", "args"} -> {"ok", "text"}. A server that is going away (a `stop`
    just asked) is waited for and started again."""
    _, port = BUILDS[build]
    for attempt in range(3):
        start(build)
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=timeout) as s:
                s.sendall((json.dumps({'op': op, 'args': args}) + '\n').encode())
                buf = b''
                while True:
                    chunk = s.recv(1 << 16)
                    if not chunk:
                        break
                    buf += chunk
            if buf:
                return json.loads(buf.decode('utf-8', 'replace'))
        except (ConnectionResetError, ConnectionRefusedError, ConnectionAbortedError):
            pass
        wait_down(port, 30)
    sys.exit('ghidra_cli.py: the %s server did not answer' % build)


def wait_down(port, seconds):
    t0 = time.time()
    while up(port) and time.time() - t0 < seconds:
        time.sleep(0.3)


def cli_args(argv):
    """`decompile 5762A0 grep=damage` -> {"at": "5762A0", "grep": "damage"}: key=value pairs, the rest positional
    (the op's own order: at, then name / text / value)."""
    pos, kw = [], {}
    for a in argv:
        m = re.fullmatch(r'([a-z_]+)=(.*)', a, re.S)
        if m:
            kw[m.group(1)] = m.group(2)
        else:
            pos.append(a)
    kw['_'] = pos
    return kw


def main():
    argv = sys.argv[1:]
    if argv[:1] == ['--serve']:
        from ghidra_server import serve   # (tools/ghidra_server.py: runs inside the PyGhidra venv)
        serve(argv[1])
        return
    build = '1.2'
    if argv[:1] == ['--build']:
        build, argv = argv[1], argv[2:]
    if build not in BUILDS:
        sys.exit('--build: 1.2 or 1.0')
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__)
        return
    op, rest = argv[0], argv[1:]
    if op == 'setup':
        builds = [build] if '--build' in sys.argv else list(BUILDS)
        for b in builds:
            analyze(b, '--force' in rest)
        for b in builds:
            print(ask(b, 'orphans', {'fix': True})['text'])   # the functions reached only through pointers
            print(ask(b, 'sync', {})['text'])
        return
    if op == 'stop':
        _, port = BUILDS[build]
        print(ask(build, 'stop', {})['text'] if up(port) else 'not running')
        wait_down(port, 60)
        return
    if op == 'status':
        for b, (name, port) in BUILDS.items():
            print('%s: project %s %s, server %s' % (b, name, 'there' if os.path.isdir(os.path.join(PROJECTS, name + '.rep'))
                                                    else 'not made (setup)', 'up on %d' % port if up(port) else 'down'))
        return
    r = ask(build, op, cli_args(rest))
    sys.stdout.write(r['text'] if r['text'].endswith('\n') else r['text'] + '\n')
    sys.exit(0 if r['ok'] else 1)


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
