"""check_builds.py: check sdk/include/mml/game/game_build.h against the game's files of every build, offline.

    python tools/check_builds.py <1.2 Game.exe> <1.2 LS3DF.dll> <1.0 Game.exe> <1.0 LS3DF.dll>

For each build's table: Detect's signatures are at its addresses in its own files and not at the other build's
(each file is recognised as its build and as nothing else), game_fixes' bytes are at its sites, the pedestrian
step's return addresses follow calls to its sphere sweep, and every function address starts a function the way it
does in the other build (the same first instruction). Exit code 1 on any failure.
"""
import os
import re
import sys

import capstone
import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def struct_fields(src, name):
    body = re.search(r'struct %s \{(.*?)\n\};' % name, src, re.S).group(1)
    fields = []
    for line in body.split('\n'):
        line = line.split('//')[0].strip()
        m = re.match(r'(const char \*|uintptr_t\s|int\s)\s*(.*);', line)
        if m:
            fields += [f.strip().lstrip('*') for f in m.group(2).split(',')]
    return fields


def table(src, var, fields):
    body = re.search(r'%s = \{(.*?)\};' % var, src, re.S).group(1)
    body = re.sub(r'//[^\n]*', '', body)
    vals = [v.strip() for v in body.replace('\n', ' ').split(',') if v.strip()]
    assert len(vals) == len(fields), (var, len(vals), len(fields))
    return {f: (v.strip('"') if f == 'name' else int(v, 0)) for f, v in zip(fields, vals)}


def sigs(src):
    out = []
    for m in re.finditer(r'\{&(ExeBuild|Ls3dBuild)::(\w+), \w+, \{([^}]*)\}\}|\{nullptr, &Ls3dBuild::(\w+), \{([^}]*)\}\}', src):
        if m.group(1):
            out.append(('exe', m.group(2), bytes(int(b, 16) for b in m.group(3).split(','))))
        else:
            out.append(('ls3d', m.group(4), bytes(int(b, 16) for b in m.group(5).split(','))))
    return out


class Img:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = self.pe.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def at(self, rva, n):
        return bytes(self.mem[rva:rva + n]) if 0 <= rva < len(self.mem) else b''

    def insn(self, rva):
        return next(self.md.disasm(self.at(rva, 16), self.base + rva), None)


def main(a):
    src = open(os.path.join(ROOT, 'sdk', 'include', 'mml', 'game', 'game_build.h')).read()
    ef, lf = struct_fields(src, 'ExeBuild'), struct_fields(src, 'Ls3dBuild')
    builds = {'1.2': (table(src, 'kExe12', ef), table(src, 'kLs3d12', lf), Img(a[0]), Img(a[1])),
              '1.0': (table(src, 'kExe10', ef), table(src, 'kLs3d10', lf), Img(a[2]), Img(a[3]))}
    sg = sigs(src)
    fails = 0

    def check(ok, what):
        nonlocal fails
        if not ok:
            fails += 1
            print('FAIL', what)

    # Detect looks again after hooks are in (the core after the ASI's or the host's): never at a hooked function.
    hooked = {'missionTick', 'missionClose', 'humanHit', 'actorDtor', 'humanCtor', 'pedHit', 'pedCarSweep', 'pedCarHit',
              'sphereSweep', 'render', 'frameRelease', 'meshRelease', 'modelOpen', 'frameDup', 'frameLimit'}
    for k, f, b in sg:
        check(f not in hooked, f'signature on {f}, a function the mod hooks')
    for name, (exe, ls, gexe, gls) in builds.items():
        for other, (_, _, oexe, ols) in builds.items():
            hits_exe = all(oexe.at(exe[f] - 0x400000, 16) == b for k, f, b in sg if k == 'exe')
            hits_ls = all(ols.at(ls[f], 16) == b for k, f, b in sg if k == 'ls3d')
            check(hits_exe == (other == name), f'{name} Game.exe table on the {other} file: recognised {hits_exe}')
            check(hits_ls == (other == name), f'{name} LS3DF table on the {other} file: recognised {hits_ls}')
        fixes = src[src.find('const uint8_t kSaveBytes'):]
        site_bytes = {
            'save1d': '8B4068 68FFFF0000', 'save1e': '8B4068 68FFFF0000', 'carExit': '8B86AC000000 8BBE98000000',
            'waterWalk': '0F85B9010000', 'waterFall': '0F859D010000', 'carWater': '7409', 'carIn': '0F85D6010000'}
        for f, hx in site_bytes.items():
            check(gexe.at(exe[f] - 0x400000, len(bytes.fromhex(hx.replace(' ', '')))) == bytes.fromhex(hx.replace(' ', '')),
                  f'{name} {f} bytes')
        for f, past in (('waterWalk', 'waterWalkPast'), ('waterFall', 'waterFallPast'), ('carIn', 'carInPast')):
            i = gexe.insn(exe[f] - 0x400000)
            check(i and i.mnemonic in ('jne', 'je') and int(i.op_str, 16) == exe[past], f'{name} {f} jumps to {past}')
        for f in ('pedStepReturn1', 'pedStepReturn2'):
            i = gexe.insn(exe[f] - 5 - 0x400000)
            check(i and i.mnemonic == 'call' and int(i.op_str, 16) == exe['sphereSweep'], f'{name} {f} after a sphere sweep call')
        check(gexe.at(exe['save1dGo'] - 0x400000, 1) == b'\x68' and gexe.at(exe['save1eGo'] - 0x400000, 1) == b'\x68',
              f'{name} save go sites (push)')
        print(f'{name}: checked ({len(sg)} signatures, {len(site_bytes)} patch sites)')
    # Functions: the same first instruction in both builds (mnemonic and operands without addresses).
    e12, l12, g12, s12 = builds['1.2']
    e10, l10, g10, s10 = builds['1.0']
    norm = lambda i: re.sub(r'0x[0-9a-f]{5,}', '@', f'{i.mnemonic} {i.op_str}') if i else None
    funcs = ['missionTick', 'missionClose', 'humanHit', 'actorDtor', 'humanCtor', 'testLine', 'frameWorldMatrix',
             'actorSetPos', 'frameSetPos', 'sceneMoved', 'vehicleDeactivate', 'carReset', 'vehicleExternalForce',
             'pedHit', 'pedCarSweep', 'pedCarHit', 'sphereSweep', 'save1dNone', 'save1eNone', 'carExitGo', 'carExitDone']
    for f in funcs:
        a, b = norm(g12.insn(e12[f] - 0x400000)), norm(g10.insn(e10[f] - 0x400000))
        check(a == b, f'{f}: 1.2 "{a}" vs 1.0 "{b}"')
    for f in ('render', 'frameRelease', 'meshRelease', 'modelOpen', 'frameDup', 'frameLimit'):
        a, b = norm(s12.insn(l12[f])), norm(s10.insn(l10[f]))
        check(a == b, f'LS3DF {f}: 1.2 "{a}" vs 1.0 "{b}"')
    # Layouts: each offset must be in the code that uses it (the witnesses: C_Vehicle::GetRVelMassJ and ExternalForce,
    # the car's water check before carIn, LS3D's morph update that locks a LOD's vertex buffer).
    witness = {'1.2': {'getRVelMassJ': 0x4CC4B0, 'morph': 0x34570}, '1.0': {'getRVelMassJ': 0x518500, 'morph': 0x33810}}
    for name, (exe, ls, gexe, gls) in builds.items():
        w = witness[name]

        def disps(img, va, n):
            md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
            md.detail = True
            out = set()
            for i in md.disasm(img.at(va - img.base, n), va):
                for op in i.operands:
                    if op.type == capstone.x86.X86_OP_MEM:
                        out.add(op.mem.disp)
                    elif op.type == capstone.x86.X86_OP_IMM:
                        out.add(op.imm)
            return out
        d = disps(gexe, w['getRVelMassJ'], 0x170)
        for f in ('vehCentre', 'vehTurn', 'vehMass', 'vehVelocity', 'vehBalance', 'vehInertiaA', 'vehInertiaB'):
            check(exe[f] in d, f'{name} {f} {exe[f]:#x} in GetRVelMassJ')
        check(exe['vehCentre'] in disps(gexe, exe['vehicleExternalForce'], 0x100), f'{name} vehCentre in ExternalForce')
        i = gexe.insn(exe['carIn'] - 8 - 0x400000)
        check(i and f'+ {exe["carInWater"]:#x}]' in i.op_str, f'{name} carInWater read before carIn')
        d = disps(gls, gls.base + w['morph'], 0x100)
        check(ls['vbStride'] // 4 * 4 == ls['vbStride'] and (ls['vbD3d'] in d) and (ls['vbStride'] in d or
              ls['vbStride'] // 4 in d), f'{name} LS3DF vertex buffer stride/D3D in the morph update')
    # The start (host/start.cpp; 1.2 only so far): the menus' vtables hold their command handlers at +0x24 (read in
    # the decompile: the profile menu's takes 100 + i, the main menu's 990105, the freeride menu's 201), PlayVideo
    # starts with its frame.
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['menuTop']:
            continue
        for f, handler in (('menuProfileVt', 0x5E88D0), ('menuMainVt', 0x5E06E0), ('menuFreerideVt', 0x5E1B40)):
            got = int.from_bytes(gexe.at(exe[f] + 0x24 - 0x400000, 4), 'little')
            check(got == handler, f'{name} {f}+0x24 is {got:#x}, not the command handler {handler:#x}')
        i = gexe.insn(exe['playVideo'] - 0x400000)
        check(i and f'{i.mnemonic} {i.op_str}' == 'sub esp, 0x214', f'{name} playVideo starts "{i.mnemonic} {i.op_str}"')
    print('ALL PASSED' if not fails else f'{fails} FAILED')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
