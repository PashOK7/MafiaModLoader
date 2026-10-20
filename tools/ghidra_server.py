"""ghidra_server.py: one build's Ghidra project held open and asked (started by tools/ghidra_cli.py, in the PyGhidra venv).

A request is one JSON line {"op", "args"}; the answer one JSON object {"ok", "text"}, then the connection closes.
`args` holds the op's parameters by name ("at", "name", "grep"...); "_" a list of positional ones (the CLI's), taken
in the op's own order. Writes are saved into the project at once and recorded in the repository's knowledge base
(ghidra_names.record_*), so the project can always be made again by `ghidra_cli.py setup`.

Built on what mafia-ragdoll's tools/ghq.py found to work with PyGhidra and Ghidra 12.1.4.
"""
import json
import os
import re
import socket
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghidra_cli as cfg        # noqa: E402  (paths, ports, the JDK; not "ghidra": the Java package of that name)
import ghidra_names as kbn      # noqa: E402

LS3D_BASE, GAME_BASE = cfg.LS3D_BASE, cfg.GAME_BASE

# each op's positional parameters, in order; the last one takes the rest (a text with spaces)
POSITIONAL = {
    'decompile': ['at'], 'asm': ['at', 'count'], 'function': ['at'], 'callers': ['at'], 'callees': ['at'],
    'xrefs': ['at'], 'vtable': ['at', 'count'], 'bytes': ['at', 'count'], 'twin': ['at'],
    'imm': ['value'], 'string': ['regex'], 'find': ['what'], 'names': [], 'functions': ['regex'],
    'rename': ['at', 'name'], 'comment': ['at', 'text'], 'proto': ['at', 'proto'], 'make_function': ['at'],
    'unfunction': ['at'], 'forget': ['at'], 'sync': [], 'orphans': [], 'status': [], 'help': [], 'stop': [],
}
OPS_READ = ('decompile', 'asm', 'function', 'callers', 'callees', 'xrefs', 'vtable', 'bytes', 'twin')
OPS_FIND = ('imm', 'string', 'find', 'names', 'functions')
OPS_WRITE = ('rename', 'comment', 'proto', 'make_function', 'unfunction', 'forget')


class Fail(Exception):
    pass


def norm_args(op, args):
    a = dict(args or {})
    pos = list(a.pop('_', []) or [])
    names = POSITIONAL.get(op, [])
    for i, n in enumerate(names):
        if i >= len(pos):
            break
        a.setdefault(n, ' '.join(pos[i:]) if i == len(names) - 1 else pos[i])
    return a


def num(a, key, default):
    v = a.get(key)
    if v is None or v == '':
        return default
    return int(v, 0) if isinstance(v, str) else int(v)


def flag(a, key):
    v = a.get(key)
    return v is True or (isinstance(v, str) and v.lower() in ('1', 'true', 'yes', 'on'))


class Server:
    def __init__(self, build):
        os.environ['JAVA_HOME'] = cfg.java_home()
        import pyghidra
        pyghidra.start(install_dir=cfg.GHIDRA)
        from ghidra.base.project import GhidraProject
        from ghidra.util.task import ConsoleTaskMonitor
        self.build = build
        name, self.port = cfg.BUILDS[build]
        self.gp = GhidraProject.openProject(cfg.PROJECTS, name, True)
        self.monitor = ConsoleTaskMonitor()
        self.progs = {m: self.gp.openProgram('/', m, False) for m in cfg.MODULES}
        self.game, self.ls3d = self.progs['Game.exe'], self.progs['LS3DF.dll']
        self.decomps, self.imm_index, self.strings, self.images = {}, {}, {}, {}

    # ------------------------------------------------------------ addresses
    def module_of(self, prog):
        return 'LS3DF.dll' if prog is self.ls3d else 'Game.exe'

    def mod(self, key):
        if key in (None, '', 'game', 'g', 'exe', 'game.exe'):
            return self.game
        if str(key).lower() in ('ls3d', 'l', 'ls3df', 'dll', 'ls3df.dll'):
            return self.ls3d
        raise Fail('module: game or ls3d')

    def mods(self, a):
        return [self.mod(a['module'])] if a.get('module') else [self.game, self.ls3d]

    def prog_of(self, off):
        return self.ls3d if off >= LS3D_BASE else self.game

    def addr(self, prog, off):
        return prog.getAddressFactory().getDefaultAddressSpace().getAddress(off)

    def at(self, text):
        """(program, Address) of an address text or a name."""
        if text is None or str(text).strip() == '':
            raise Fail('at: an address or a name')
        t = str(text).strip()
        m = re.fullmatch(r'(?i)(ls3df?|l|game|g)\+(0x)?([0-9a-f]+)', t)
        if m:
            rva = int(m.group(3), 16)
            if m.group(1).lower().startswith('l'):
                return self.ls3d, self.addr(self.ls3d, LS3D_BASE + rva)
            return self.game, self.addr(self.game, GAME_BASE + rva)
        m = re.fullmatch(r'(?i)(0x)?([0-9a-f]{5,8})', t)
        if m and (m.group(1) or not re.search(r'[g-z_]', t, re.I)):
            off = int(m.group(2), 16)
            p = self.prog_of(off)
            return p, self.addr(p, off)
        for p in (self.game, self.ls3d):
            syms = list(p.getSymbolTable().getGlobalSymbols(t))
            if syms:
                return p, syms[0].getAddress()
        last = t.split('::')[-1]
        for p in (self.game, self.ls3d):
            for s in p.getSymbolTable().getSymbols(last):
                if s.getName(True) == t or s.getName() == t:
                    return p, s.getAddress()
        raise Fail('no address or name: ' + t)

    def tag(self, prog, a):
        off = a.getOffset()
        return ('ls3d+%X' % (off - LS3D_BASE)) if prog is self.ls3d else ('%X' % off)

    def fname(self, prog, a):
        f = prog.getFunctionManager().getFunctionContaining(a)
        if f is None:
            return '?'
        d = a.getOffset() - f.getEntryPoint().getOffset()
        return f.getName(True) + ('+%X' % d if d else '')

    def where(self, prog, ad):
        f = self.fname(prog, ad)
        if f != '?':
            return f
        sym = prog.getSymbolTable().getPrimarySymbol(ad)
        if sym is None:
            d = prog.getListing().getDataContaining(ad)
            if d is not None:
                sym = prog.getSymbolTable().getPrimarySymbol(d.getAddress())
        if sym is None:
            return '?'
        d = ad.getOffset() - sym.getAddress().getOffset()
        return sym.getName(True) + ('+%X' % d if d else '')

    # ------------------------------------------------------------ helpers
    def decomp(self, prog):
        di = self.decomps.get(prog)
        if di is None:
            from ghidra.app.decompiler import DecompInterface
            di = DecompInterface()
            di.openProgram(prog)
            self.decomps[prog] = di
        return di

    def write(self, prog, what, fn):
        tx = prog.startTransaction(what)
        ok = False
        try:
            r = fn()
            ok = True
            return r
        finally:
            prog.endTransaction(tx, ok)
            if ok:
                self.gp.save(prog)

    def func(self, prog, a, create=True):
        fm = prog.getFunctionManager()
        f = fm.getFunctionContaining(a)
        if f is None and create:
            from ghidra.app.cmd.disassemble import DisassembleCommand
            from ghidra.app.cmd.function import CreateFunctionCmd

            def mk():
                DisassembleCommand(a, None, True).applyTo(prog, self.monitor)
                CreateFunctionCmd(a).applyTo(prog, self.monitor)
            self.write(prog, 'mml: function', mk)
            f = fm.getFunctionContaining(a)
        return f

    def decompile_text(self, prog, f):
        res = self.decomp(prog).decompileFunction(f, 120, self.monitor)
        if not res.decompileCompleted():
            return '// decompile failed: ' + str(res.getErrorMessage())
        return str(res.getDecompiledFunction().getC())

    def capped(self, head, items, n, more='(max=)'):
        out = head + '\n' + '\n'.join(items[:n])
        if len(items) > n:
            out += '\n  ... %d more %s' % (len(items) - n, more)
        return out + '\n'

    # ------------------------------------------------------------ reading
    def op_decompile(self, a):
        prog, ad = self.at(a.get('at'))
        f = self.func(prog, ad)
        if f is None:
            raise Fail('no function at ' + str(a.get('at')))
        n_callers = sum(1 for r in prog.getReferenceManager().getReferencesTo(f.getEntryPoint()) if r.getReferenceType().isCall())
        head = '// %s @ %s  size %d  callers %d  %s  (%s)\n' % (f.getName(True), self.tag(prog, f.getEntryPoint()),
                                                             f.getBody().getNumAddresses(), n_callers,
                                                             f.getCallingConventionName(), self.build)
        lines = [l for l in self.decompile_text(prog, f).splitlines() if l.strip()]
        if a.get('range'):
            lo, hi = (int(x) for x in str(a['range']).split('-'))
            keep = list(range(max(1, lo), min(len(lines), hi) + 1))
        elif a.get('grep'):
            rx, ctx = re.compile(a['grep'], re.I), num(a, 'context', 1)
            hits = [i + 1 for i, l in enumerate(lines) if rx.search(l)]
            keep = sorted({j for h in hits for j in range(h - ctx, h + ctx + 1) if 1 <= j <= len(lines)})
        else:
            keep = list(range(1, len(lines) + 1))
        cap = num(a, 'max', 400)
        out, prev = [], None
        for j in keep[:cap]:
            if prev is not None and j != prev + 1:
                out.append('    ...')
            out.append('%4d %s' % (j, lines[j - 1]))
            prev = j
        if len(keep) > cap:
            out.append('... %d more lines (max=, range=)' % (len(keep) - cap))
        return head + '\n'.join(out) + '\n'

    def ref_names(self, prog, ins):
        names = []
        for r in ins.getReferencesFrom():
            s = prog.getSymbolTable().getPrimarySymbol(r.getToAddress())
            if s is not None:
                names.append(s.getName(True))
        return names

    def op_asm(self, a):
        prog, ad = self.at(a.get('at'))
        n = num(a, 'count', 40)
        lst = prog.getListing()
        ins = lst.getInstructionContaining(ad) or lst.getInstructionAt(ad)
        if ins is None:
            raise Fail('no instruction at %s (not disassembled: make_function)' % a.get('at'))
        out = []
        for _ in range(n):
            if ins is None:
                break
            s = prog.getSymbolTable().getPrimarySymbol(ins.getAddress())
            if s is not None and not s.getName().startswith('LAB_'):
                out.append('%s:' % s.getName(True))
            names = self.ref_names(prog, ins)
            out.append('  %-9s %-40s%s' % (self.tag(prog, ins.getAddress()), str(ins), ('  ; ' + ', '.join(names)) if names else ''))
            ins = ins.getNext()
        return '\n'.join(out) + '\n'

    def op_function(self, a):
        prog, ad = self.at(a.get('at'))
        f = self.func(prog, ad, create=False)
        if f is None:
            raise Fail('no function holds %s' % a.get('at'))
        e = f.getEntryPoint()
        refs = list(prog.getReferenceManager().getReferencesTo(e))
        callees = sorted({c.getName(True) for c in f.getCalledFunctions(self.monitor)})
        named = [c for c in callees if not c.startswith('FUN_')]
        sym = prog.getSymbolTable().getPrimarySymbol(e)
        comment = f.getComment()
        return ('%s @ %s  size %d  %s  (%s)\n  %s\n  name from: %s\n  references to it: %d (%s)\n  calls %d functions; named: %s\n%s' % (
            f.getName(True), self.tag(prog, e), f.getBody().getNumAddresses(), f.getCallingConventionName(), self.build,
            f.getPrototypeString(False, False), sym.getSource() if sym else '?', len(refs),
            ', '.join(sorted({str(r.getReferenceType()) for r in refs})), len(callees), ', '.join(named) or '-',
            ('  comment: %s\n' % comment) if comment else ''))

    def op_callers(self, a):
        prog, ad = self.at(a.get('at'))
        f = self.func(prog, ad, create=False)
        e = f.getEntryPoint() if f is not None else ad
        items = ['  %-9s %-16s in %s' % (self.tag(prog, r.getFromAddress()), r.getReferenceType(), self.fname(prog, r.getFromAddress()))
                 for r in prog.getReferenceManager().getReferencesTo(e)]
        return self.capped('%d references to %s' % (len(items), self.where(prog, e)), items, num(a, 'max', 80))

    def op_callees(self, a):
        prog, ad = self.at(a.get('at'))
        f = self.func(prog, ad, create=False)
        if f is None:
            raise Fail('no function holds %s' % a.get('at'))
        out = []
        for ins in prog.getListing().getInstructions(f.getBody(), True):
            if not ins.getFlowType().isCall():
                continue
            flows = list(ins.getFlows())
            tgt = ', '.join('%s (%s)' % (self.fname(prog, t), self.tag(prog, t)) for t in flows) if flows else 'indirect: ' + str(ins)
            out.append('  %-9s -> %s' % (self.tag(prog, ins.getAddress()), tgt))
        return '%s: %d calls\n' % (f.getName(True), len(out)) + '\n'.join(out) + '\n'

    def op_xrefs(self, a):
        prog, ad = self.at(a.get('at'))
        refs = list(prog.getReferenceManager().getReferencesTo(ad))
        items = ['  %-9s %-16s in %s' % (self.tag(prog, r.getFromAddress()), r.getReferenceType(), self.fname(prog, r.getFromAddress()))
                 for r in refs]
        return self.capped('%d references to %s' % (len(refs), self.where(prog, ad)), items, num(a, 'max', 80))

    def op_vtable(self, a):
        prog, ad = self.at(a.get('at'))
        mem, out = prog.getMemory(), []
        for i in range(num(a, 'count', 32)):
            v = mem.getInt(ad.add(4 * i)) & 0xFFFFFFFF
            if self.prog_of(v) is not prog or not mem.contains(self.addr(prog, v)):
                out.append('  +%-4X %08X (not in this module)' % (4 * i, v))
                continue
            out.append('  +%-4X %-9s %s' % (4 * i, self.tag(prog, self.addr(prog, v)), self.where(prog, self.addr(prog, v))))
        return '\n'.join(out) + '\n'

    def op_bytes(self, a):
        import jpype
        prog, ad = self.at(a.get('at'))
        n = num(a, 'count', 32)
        buf = jpype.JArray(jpype.JByte)(n)
        prog.getMemory().getBytes(ad, buf)
        b = bytes((x & 0xFF) for x in buf)
        return '\n'.join('  %-9s %s' % (self.tag(prog, ad.add(i)), b[i:i + 16].hex(' ')) for i in range(0, n, 16)) + '\n'

    def op_twin(self, a):
        """The other build's function paired with this one by its code (knowledge/<module>/twins_10.csv)."""
        prog, ad = self.at(a.get('at'))
        module = self.module_of(prog)
        f = self.func(prog, ad, create=False)
        off = (f.getEntryPoint() if f is not None else ad).getOffset()
        other = '1.0' if self.build == '1.2' else '1.2'
        pairs = [(int(r['va12'], 16), int(r['va10'], 16), r['how']) for r in kbn.rows(kbn.path(module, 'twins_10.csv'))]
        found = [(v10 if self.build == '1.2' else v12, how) for v12, v10, how in pairs
                 if (v12 if self.build == '1.2' else v10) == off]
        if not found:
            return '%X in %s: no twin known in %s (knowledge/%s/twins_10.csv)\n' % (off, self.build, other,
                                                                                  kbn.MODULE_DIRS[module])
        theirs = kbn.names(module, other)
        return ''.join('%s %X -> %s %X  %s  (paired by %s)\n' % (self.build, off, other, t, theirs.best[t].name if t in theirs.best else '', how)
                       for t, how in found)

    # ------------------------------------------------------------ finding
    def build_imm(self, prog):
        from ghidra.program.model.scalar import Scalar
        idx = {}
        for ins in prog.getListing().getInstructions(True):
            off = ins.getAddress().getOffset()
            for i in range(ins.getNumOperands()):
                for ob in ins.getOpObjects(i):
                    if isinstance(ob, Scalar):
                        idx.setdefault(ob.getUnsignedValue() & 0xFFFFFFFF, []).append(off)
        return idx

    def op_imm(self, a):
        """Instructions using a value: an immediate or a displacement ([ECX + 0x670]): who touches a field."""
        val = int(str(a.get('value')), 16) & 0xFFFFFFFF
        trx = re.compile(a['text'], re.I) if a.get('text') else None
        if a.get('in'):
            prog, ad = self.at(a['in'])
            f = self.func(prog, ad, create=False)
            if f is None:
                raise Fail('no function at %s (in= takes a function: an address or a name)' % a['in'])
            from ghidra.program.model.scalar import Scalar
            hits = []
            for ins in prog.getListing().getInstructions(f.getBody(), True):
                for i in range(ins.getNumOperands()):
                    if any(isinstance(ob, Scalar) and (ob.getUnsignedValue() & 0xFFFFFFFF) == val for ob in ins.getOpObjects(i)):
                        hits.append(ins.getAddress().getOffset())
                        break
            progs = [(prog, hits)]
        else:
            progs = []
            for prog in self.mods(a):
                if prog not in self.imm_index:
                    self.imm_index[prog] = self.build_imm(prog)
                progs.append((prog, sorted(set(self.imm_index[prog].get(val, [])))))
        items = []
        for prog, hits in progs:
            for off in hits:
                ins = prog.getListing().getInstructionAt(self.addr(prog, off))
                if ins is None:
                    continue
                text = str(ins)
                if flag(a, 'writes') and not is_mem_write(text):
                    continue
                if trx is not None and not trx.search(text):
                    continue
                items.append('  %-9s %-40s in %s' % (self.tag(prog, ins.getAddress()), text, self.fname(prog, ins.getAddress())))
        return self.capped('%d instructions use 0x%X' % (len(items), val), items, num(a, 'max', 80))

    def op_string(self, a):
        rx = re.compile(str(a.get('regex', '')), re.I)
        items = []
        for prog in self.mods(a):
            if prog not in self.strings:
                self.strings[prog] = [(d.getAddress(), str(d.getValue())) for d in prog.getListing().getDefinedData(True)
                                      if d.hasStringValue()]
            for ad, s in self.strings[prog]:
                if rx.search(s):
                    users = sorted({self.fname(prog, r.getFromAddress()).split('+')[0]
                                    for r in prog.getReferenceManager().getReferencesTo(ad)})
                    items.append('  %-9s %-50r used by %s' % (self.tag(prog, ad), s[:120], ', '.join(users) or '-'))
        return self.capped('%d strings' % len(items), items, num(a, 'max', 40))

    def image(self, prog):
        if prog not in self.images:
            import jpype
            blocks = []
            for blk in prog.getMemory().getBlocks():
                if blk.isInitialized():
                    buf = jpype.JArray(jpype.JByte)(int(blk.getSize()))
                    blk.getBytes(blk.getStart(), buf)
                    blocks.append((blk.getStart(), blk.getName(), bytes((x & 0xFF) for x in buf)))
            self.images[prog] = blocks
        return self.images[prog]

    def op_find(self, a):
        """Where the image holds a 32-bit value (a pointer no code names) or a byte pattern ("8B 4C 24")."""
        what = str(a.get('what', '')).strip()
        pat = bytes.fromhex(what) if ' ' in what else (int(what, 16) & 0xFFFFFFFF).to_bytes(4, 'little')
        items = []
        for prog in self.mods(a):
            for st, block, data in self.image(prog):
                i = data.find(pat)
                while i >= 0:
                    ad = st.add(i)
                    items.append('  %-9s %-6s %s' % (self.tag(prog, ad), block, self.where(prog, ad)))
                    i = data.find(pat, i + 1)
        return self.capped('%d places' % len(items), items, num(a, 'max', 40))

    def op_names(self, a):
        """The names people gave (USER_DEFINED: ours, through `rename`); all=1: the imported ones too."""
        from ghidra.program.model.symbol import SourceType
        keep = (SourceType.USER_DEFINED,) + ((SourceType.IMPORTED,) if flag(a, 'all') else ())
        items = []
        for prog in self.mods(a):
            fm = prog.getFunctionManager()
            for s in prog.getSymbolTable().getAllSymbols(True):
                if s.getSource() in keep and not s.isExternal() and s.getAddress().isMemoryAddress():
                    kind = 'function' if fm.getFunctionAt(s.getAddress()) is not None and s.isPrimary() else 'label'
                    items.append('  %-9s %-8s %s' % (self.tag(prog, s.getAddress()), kind, s.getName(True)))
        return self.capped('%d names' % len(items), items, num(a, 'max', 200))

    def op_functions(self, a):
        """Functions whose name (with its namespace) matches a regex."""
        rx = re.compile(str(a.get('regex', '')), re.I)
        items = []
        for prog in self.mods(a):
            for f in prog.getFunctionManager().getFunctions(True):
                n = f.getName(True)
                if rx.search(n):
                    items.append('  %-9s %-50s %s' % (self.tag(prog, f.getEntryPoint()), n, f.getCallingConventionName()))
        return self.capped('%d functions' % len(items), items, num(a, 'max', 80))

    # ------------------------------------------------------------ writing (into the project and the repository)
    def ghidra_name(self, name):
        """A name as Ghidra keeps it: its namespace path and its last part, each with Ghidra's invalid characters
        (spaces) replaced."""
        from ghidra.program.model.symbol import SymbolUtilities
        ns_path, last = kbn.split_name(name)
        return '::'.join(SymbolUtilities.replaceInvalidChars(p, True) for p in ns_path + [last])

    def set_name(self, prog, ad, name, source):
        """A function's name (its namespace path created) or a label; returns its kind."""
        from ghidra.app.util import NamespaceUtils
        from ghidra.program.model.symbol import SymbolUtilities
        ns_path, last = kbn.split_name(name)
        last = SymbolUtilities.replaceInvalidChars(last, True)
        ns = prog.getGlobalNamespace()
        if ns_path:
            path = '::'.join(SymbolUtilities.replaceInvalidChars(p, True) for p in ns_path)
            ns = NamespaceUtils.createNamespaceHierarchy(path, None, prog, source)
        f = prog.getFunctionManager().getFunctionAt(ad)
        if f is not None:
            # a label of that name at its entry (an export's) would make the name a duplicate: the function takes it
            for s in list(prog.getSymbolTable().getSymbols(ad)):
                if s.getName() == last and s != f.getSymbol():
                    s.delete()
            f.setParentNamespace(ns)
            f.setName(last, source)
            return 'function'
        same = [s for s in prog.getSymbolTable().getSymbols(ad) if s.getName() == last]
        if same:   # already a name there (an export's, an import's): made the primary one
            same[0].setPrimary()
            return 'label'
        prog.getSymbolTable().createLabel(ad, last, ns, source).setPrimary()
        return 'label'

    def op_rename(self, a):
        from ghidra.program.model.symbol import SourceType
        prog, ad = self.at(a.get('at'))
        name = str(a.get('name') or '').strip()
        if not name:
            raise Fail('name: the new name')
        kind = self.write(prog, 'mml: rename', lambda: self.set_name(prog, ad, name, SourceType.USER_DEFINED))
        kbn.record_name(self.module_of(prog), self.build, ad.getOffset(), kind, name, a.get('note'))
        return '%s %s -> %s  (%s; recorded in knowledge/%s/%s)\n' % (kind, self.tag(prog, ad), name, self.build,
                                                                    kbn.MODULE_DIRS[self.module_of(prog)],
                                                                    os.path.basename(kbn.names_file(self.module_of(prog), self.build)))

    def set_comment(self, prog, ad, text):
        from ghidra.program.model.listing import CodeUnit
        f = prog.getFunctionManager().getFunctionAt(ad)
        if f is not None:
            f.setComment(text)
        else:
            prog.getListing().setComment(ad, CodeUnit.PRE_COMMENT, text)

    def op_comment(self, a):
        prog, ad = self.at(a.get('at'))
        text = str(a.get('text') or '')
        self.write(prog, 'mml: comment', lambda: self.set_comment(prog, ad, text))
        kbn.record_annotation(self.module_of(prog), self.build, ad.getOffset(), 'comment', text)
        return 'comment at %s (%s; recorded in annotations.csv)\n' % (self.tag(prog, ad), self.build)

    def set_proto(self, prog, f, text, source):
        from ghidra.app.util.cparser.C import CParserUtils
        from ghidra.app.cmd.function import ApplyFunctionSignatureCmd
        import jpype
        from ghidra.app.services import DataTypeManagerService
        # the calling convention is not a part of the C its parser takes: taken out, set on the definition after
        conv = None
        m = re.search(r'\b(__thiscall|__stdcall|__fastcall|__cdecl)\b', text)
        if m:
            conv, text = m.group(1), (text[:m.start()] + text[m.end():]).replace('  ', ' ')
        # (a typed null: parseSignature has overloads JPype cannot choose between for None; with no service it
        # parses with the program's types and the built-in ones)
        sig = CParserUtils.parseSignature(jpype.JObject(None, DataTypeManagerService), prog, text)
        if sig is None:
            raise Fail('the prototype could not be parsed: ' + text)
        if conv:
            sig.setCallingConvention(conv)
        # the name stays (the prototype's is a placeholder: "f"; the command's default renames an unnamed function
        # to it, which once named 1179 of 1.2's LS3DF functions "f")
        from ghidra.app.cmd.function import FunctionRenameOption
        ApplyFunctionSignatureCmd(f.getEntryPoint(), sig, source, False, FunctionRenameOption.NO_CHANGE).applyTo(prog, self.monitor)

    def op_proto(self, a):
        from ghidra.program.model.symbol import SourceType
        prog, ad = self.at(a.get('at'))
        f = self.func(prog, ad)
        text = str(a.get('proto') or '')
        self.write(prog, 'mml: proto', lambda: self.set_proto(prog, f, text, SourceType.USER_DEFINED))
        kbn.record_annotation(self.module_of(prog), self.build, f.getEntryPoint().getOffset(), 'proto', text)
        return '%s: %s  (%s; recorded in annotations.csv)\n' % (self.tag(prog, f.getEntryPoint()), f.getPrototypeString(False, False), self.build)

    def op_make_function(self, a):
        prog, ad = self.at(a.get('at'))
        f = self.func(prog, ad, create=True)
        if f is None:
            raise Fail('no function could be made at ' + str(a.get('at')))
        return 'function %s at %s, size %d\n' % (f.getName(True), self.tag(prog, f.getEntryPoint()), f.getBody().getNumAddresses())

    def op_unfunction(self, a):
        from ghidra.program.model.symbol import SourceType
        prog, ad = self.at(a.get('at'))
        f = prog.getFunctionManager().getFunctionAt(ad)
        if f is None:
            raise Fail('no function starts at %s' % a.get('at'))
        name = f.getName()

        def do():
            prog.getFunctionManager().removeFunction(ad)
            if not name.startswith('FUN_'):
                prog.getSymbolTable().createLabel(ad, name, SourceType.USER_DEFINED).setPrimary()
        self.write(prog, 'mml: unfunction', do)
        return 'removed the function at %s (its name kept as a label)\n' % self.tag(prog, ad)

    def op_forget(self, a):
        """What we gave an address taken back: its row in names.csv / names_10.csv and in annotations.csv, its name
        and comment in the project; then the repository's other names for it (MafiaReverse's, the SDK's...) again."""
        from ghidra.program.model.listing import CodeUnit
        from ghidra.program.model.symbol import SourceType
        prog, ad = self.at(a.get('at'))
        module, off = self.module_of(prog), ad.getOffset()
        gone = kbn.forget(module, self.build, off)

        def do():
            f = prog.getFunctionManager().getFunctionAt(ad)
            for s in list(prog.getSymbolTable().getSymbols(ad)):
                if s.getSource() == SourceType.USER_DEFINED:
                    if f is not None and s == f.getSymbol():
                        f.setName(None, SourceType.DEFAULT)
                        f.setParentNamespace(prog.getGlobalNamespace())
                    else:
                        s.delete()
            if f is not None:
                f.setComment(None)
            prog.getListing().setComment(ad, CodeUnit.PRE_COMMENT, None)
            n = kbn.names(module, self.build).best.get(off)
            if n is not None:
                self.set_name(prog, ad, n.name, SourceType.IMPORTED)
            proto = kbn.signatures(module, self.build).get(off)
            if f is not None and proto:   # the repository's prototype again (ours is gone)
                self.set_proto(prog, f, proto, SourceType.IMPORTED)
            return n
        n = self.write(prog, 'mml: forget', do)
        return 'forgot %s (%s): %s; its name now %s\n' % (self.tag(prog, ad), self.build, ', '.join(gone) or 'nothing of ours',
                                                       '%s (%s)' % (n.name, n.source) if n else 'the default')

    # ------------------------------------------------------------ the repository's names onto the project
    def op_sync(self, a):
        """The knowledge base's names and annotations onto the project. A name a person gave in this project
        (USER_DEFINED) stays; default and analysis names, and those an earlier sync imported, take the
        repository's. Code at a name's address that the analysis left outside every function becomes one."""
        from ghidra.program.model.symbol import SourceType
        from ghidra.app.cmd.disassemble import DisassembleCommand
        from ghidra.app.cmd.function import CreateFunctionCmd
        out = []
        for module, prog in self.progs.items():
            names = kbn.names(module, self.build)
            ns, sigs = names.best, kbn.signatures(module, self.build)
            fm, st, mem = prog.getFunctionManager(), prog.getSymbolTable(), prog.getMemory()
            stats = {'named': 0, 'kept': 0, 'same': 0, 'outside': 0, 'made': 0, 'failed': 0, 'aliases': 0,
                     'protos': 0, 'protos_kept': 0, 'unnamed': 0}
            failures = []

            def do():
                # the "f" an older set_proto gave unnamed functions (the prototypes' placeholder name): default again
                for f in list(fm.getFunctions(True)):
                    if (f.getName() == 'f' and f.getSymbol().getSource() == SourceType.IMPORTED
                            and f.getEntryPoint().getOffset() not in ns):
                        f.setName(None, SourceType.DEFAULT)
                        stats['unnamed'] += 1
                for off in sorted(ns):
                    n = ns[off]
                    ad = self.addr(prog, off)
                    if not mem.contains(ad):
                        stats['outside'] += 1
                        continue
                    blk = mem.getBlock(ad)
                    if (n.kind in ('function', 'sdk') and blk.isExecute() and fm.getFunctionContaining(ad) is None
                            and prog.getListing().getDefinedDataContaining(ad) is None):
                        DisassembleCommand(ad, None, True).applyTo(prog, self.monitor)
                        if CreateFunctionCmd(ad).applyTo(prog, self.monitor):
                            stats['made'] += 1
                    s = st.getPrimarySymbol(ad)
                    if s is not None and s.getSource() == SourceType.USER_DEFINED:
                        stats['kept'] += 1
                        continue
                    if s is not None and s.getName(True) == self.ghidra_name(n.name):
                        stats['same'] += 1
                        continue
                    try:
                        self.set_name(prog, ad, n.name, SourceType.IMPORTED)
                        stats['named'] += 1
                    except Exception as e:
                        stats['failed'] += 1
                        failures.append('  %s %s (%s): %s' % (self.tag(prog, ad), n.name, n.source, str(e).splitlines()[0][:160]))
                # the SDK's names that are not the primary one: second names at their address
                for off, extra in names.aliases.items():
                    ad = self.addr(prog, off)
                    if not mem.contains(ad):
                        continue
                    for alias in extra:
                        if any(s.getName() == alias for s in st.getSymbols(ad)):
                            continue
                        try:
                            st.createLabel(ad, alias, SourceType.IMPORTED)
                            stats['aliases'] += 1
                        except Exception as e:
                            stats['failed'] += 1
                            failures.append('  %s alias %s: %s' % (self.tag(prog, ad), alias, str(e).splitlines()[0][:160]))
                # MafiaReverse's prototypes where the function's signature is still the analysis' own
                for off, proto in sorted(sigs.items()):
                    f = fm.getFunctionAt(self.addr(prog, off))
                    if f is None:
                        continue
                    if f.getSignatureSource() in (SourceType.USER_DEFINED, SourceType.IMPORTED):
                        stats['protos_kept'] += 1
                        continue
                    try:
                        self.set_proto(prog, f, proto, SourceType.IMPORTED)
                        stats['protos'] += 1
                    except Exception as e:
                        stats['failed'] += 1
                        failures.append('  %s proto %r: %s' % (self.tag(prog, f.getEntryPoint()), proto, str(e).splitlines()[0][:160]))
                for r in kbn.annotations(module, self.build):
                    ad = self.addr(prog, int(r['address'], 16))
                    try:
                        if r['kind'] == 'comment':
                            self.set_comment(prog, ad, r['text'])
                        elif r['kind'] == 'proto':
                            f = fm.getFunctionContaining(ad)
                            if f is not None:
                                self.set_proto(prog, f, r['text'], SourceType.IMPORTED)
                    except Exception as e:
                        stats['failed'] += 1
                        failures.append('  %s %s: %s' % (self.tag(prog, ad), r['kind'], str(e).splitlines()[0][:160]))
            self.write(prog, 'mml: sync', do)
            self.imm_index.pop(prog, None)
            out.append('%s %s: %d names in the repository: %d named, %d the same, %d kept (named here), %d SDK aliases '
                       'added, %d functions made, %d outside the image; %d prototypes set, %d kept; %d failed; %d '
                       'functions in all%s' % (
                           self.build, module, len(ns), stats['named'], stats['same'], stats['kept'], stats['aliases'],
                           stats['made'], stats['outside'], stats['protos'], stats['protos_kept'], stats['failed'],
                           fm.getFunctionCount(),
                           '; %d named "f" by an older prototype made unnamed again' % stats['unnamed'] if stats['unnamed'] else ''))
            out.extend(failures[:20])
        return '\n'.join(out) + '\n'

    def orphan_starts(self, prog):
        """Instructions outside every function that no orphan instruction falls through to: where code the
        analysis left without a function begins (functions reached only through vtables and pointers)."""
        fm, starts, total = prog.getFunctionManager(), [], 0
        prev_orphan, prev = False, None
        for ins in prog.getListing().getInstructions(True):
            ad = ins.getAddress()
            orphan = fm.getFunctionContaining(ad) is None
            if orphan:
                total += 1
                if not (prev is not None and prev_orphan and prev.getFallThrough() == ad):
                    starts.append(ad)
            prev_orphan, prev = orphan, ins
        return starts, total

    def op_orphans(self, a):
        """Code the analysis disassembled but left outside every function; fix=1 makes a function at each start
        (rounds until none is made). `setup` runs it before the names."""
        from ghidra.app.cmd.function import CreateFunctionCmd
        out = []
        for prog in self.mods(a):
            fm = prog.getFunctionManager()
            starts, total = self.orphan_starts(prog)
            out.append('%s %s: %d instructions outside functions, %d starts, %d functions'
                       % (self.build, prog.getName(), total, len(starts), fm.getFunctionCount()))
            rounds = 0
            while flag(a, 'fix') and starts and rounds < 8:
                rounds += 1

                def do():
                    made = 0
                    for ad in starts:   # ascending: an entry before its later blocks
                        if fm.getFunctionContaining(ad) is None:
                            CreateFunctionCmd(ad).applyTo(prog, self.monitor)
                            made += fm.getFunctionAt(ad) is not None
                    return made
                made = self.write(prog, 'mml: orphans', do)
                starts, total = self.orphan_starts(prog)
                out.append('  round %d: %d functions made; now %d instructions outside, %d starts, %d functions'
                           % (rounds, made, total, len(starts), fm.getFunctionCount()))
                if made == 0:
                    break
            self.imm_index.pop(prog, None)
        return '\n'.join(out) + '\n'

    def op_status(self, a):
        out = ['%s: project %s (%s)' % (self.build, cfg.BUILDS[self.build][0], cfg.PROJECTS)]
        for module, prog in self.progs.items():
            out.append('  %s: %d functions, image %s, %s' % (module, prog.getFunctionManager().getFunctionCount(),
                                                            prog.getImageBase(), prog.getExecutablePath()))
        return '\n'.join(out) + '\n'

    def op_help(self, a):
        return cfg.__doc__

    # ------------------------------------------------------------ serving
    def handle(self, req):
        op = req.get('op', '')
        fn = getattr(self, 'op_' + op, None)
        if fn is None:
            return {'ok': False, 'text': 'unknown op %r (ops: %s)' % (op, ', '.join(OPS_READ + OPS_FIND + OPS_WRITE))}
        try:
            return {'ok': True, 'text': fn(norm_args(op, req.get('args')))}
        except Fail as e:
            return {'ok': False, 'text': str(e)}
        except Exception as e:   # keep serving
            return {'ok': False, 'text': 'error: %s\n%s' % (e, traceback.format_exc(limit=4))}

    def serve(self):
        ls = socket.socket()
        ls.bind(('127.0.0.1', self.port))
        ls.listen(8)
        print('ghidra: serving %s on %d' % (self.build, self.port), flush=True)
        while True:
            c, _ = ls.accept()
            with c:
                try:
                    line = c.makefile('rb').readline().decode('utf-8', 'replace')
                    req = json.loads(line) if line.strip() else None
                except (OSError, ValueError):
                    req = None
                if req is None:   # a probe (a client looking at the port)
                    continue
                if req.get('op') == 'stop':
                    for p in self.progs.values():
                        self.gp.save(p)
                    self.gp.close()
                    c.sendall(json.dumps({'ok': True, 'text': 'stopped (saved)\n'}).encode())
                    return
                try:
                    c.sendall(json.dumps(self.handle(req)).encode('utf-8', 'replace'))
                except OSError:
                    pass


def is_mem_write(text):
    m = re.match(r'(MOV\w*|ADD|SUB|AND|OR|XOR|INC|DEC|FSTP?|FISTP?|STOS\w*|NOT|NEG|SH[LR]|SAR|SET\w+|XCHG)\s+(.*)', text)
    return bool(m) and m.group(2).split(',')[0].strip().find('[') >= 0


def serve(build):
    Server(build).serve()
