"""check_builds.py: check sdk/include/mml/game/game_build.h against the game's files of every build, offline.

    python tools/check_builds.py <1.2 Game.exe> <1.2 LS3DF.dll> <1.0 Game.exe> <1.0 LS3DF.dll>

For each build's table: Detect's signatures are at its addresses in its own files and not at the other build's
(each file is recognised as its build and as nothing else), game_fixes' bytes are at its sites, the pedestrian
step's return addresses follow calls to its sphere sweep, and every function address starts a function the way it
does in the other build (the same first instruction). The mod kit's tables (the 2D drawing, the mission, the car):
every address is where the code that was read is, and every offset is in the function that uses it, in both
builds. The menus as objects (1.2) and 1.0's screens: each function is where the code that was read is. Exit code 1
on any failure.
"""
import os
import re
import struct
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
    def operands(img, va, n):
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True
        out = set()
        for i in md.disasm(img.at(va - img.base, n), va):
            for op in i.operands:
                if op.type == capstone.x86.X86_OP_MEM:
                    out.add(op.mem.disp & 0xFFFFFFFF)
                elif op.type == capstone.x86.X86_OP_IMM:
                    out.add(op.imm & 0xFFFFFFFF)
        return out

    # Functions: the same first instruction in both builds (mnemonic and operands without addresses).
    e12, l12, g12, s12 = builds['1.2']
    e10, l10, g10, s10 = builds['1.0']
    norm = lambda i: re.sub(r'0x[0-9a-f]{5,}', '@', f'{i.mnemonic} {i.op_str}') if i else None
    funcs = ['missionTick', 'missionClose', 'humanHit', 'actorDtor', 'humanCtor', 'testLine', 'frameWorldMatrix',
             'actorSetPos', 'frameSetPos', 'sceneMoved', 'vehicleDeactivate', 'carReset', 'vehicleExternalForce',
             'pedHit', 'pedCarSweep', 'pedCarHit', 'sphereSweep', 'save1dNone', 'save1eNone', 'carExitGo', 'carExitDone', 'carRepair']
    for f in funcs:
        a, b = norm(g12.insn(e12[f] - 0x400000)), norm(g10.insn(e10[f] - 0x400000))
        check(a == b, f'{f}: 1.2 "{a}" vs 1.0 "{b}"')
    for f in ('render', 'frameRelease', 'meshRelease', 'modelOpen', 'frameDup', 'frameLimit'):
        a, b = norm(s12.insn(l12[f])), norm(s10.insn(l10[f]))
        check(a == b, f'LS3DF {f}: 1.2 "{a}" vs 1.0 "{b}"')
    # carRepair is what the scripts' CAR_REPAIR calls (its handler: knowledge/game.exe/script_commands.csv)
    handlers = {'1.2': 0x478F69, '1.0': 0x5C6D4F}
    for name, (exe, ls, gexe, gls) in builds.items():
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        calls = [int(i.op_str, 16) for i in md.disasm(gexe.at(handlers[name] - 0x400000, 0x60), handlers[name])
                 if i.mnemonic == 'call' and i.op_str.startswith('0x')]
        check(exe['carRepair'] in calls, f'{name} carRepair: called by CAR_REPAIR')
    # The menu's camera (fps_unlock): the tick uses the spring's 0.02 and 0.98 and every vector of the state.
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['menuCamTick']:
            continue
        body = gexe.at(exe['menuCamTick'] - 0x400000, 0x650)
        refs = operands(gexe, exe['menuCamTick'], 0x650)
        check(body.count(bytes.fromhex('0ad7a33c')) == 2 and body.count(bytes.fromhex('48e17a3f')) == 2,
              f'{name} menuCamTick: the spring 0.02 / 0.98 twice')
        check(all(exe['menuCam'] + o in refs for o in (0x00, 0x0C, 0x18, 0x24, 0x30, 0x3C)),
              f'{name} menuCamTick: uses each vector of menuCam')
    # 1.0's inlined menu camera: `mov eax, 10`, the loop's `jne` back right before the end, six `fmul [0.001]` and six
    # `fmul [0.002]` between (fps_unlock points those operands at floats of its own)
    for name, (exe, ls, gexe, gls) in builds.items():
        lo, hi = exe['menuCamLoop'], exe['menuCamLoopEnd']
        if not lo:
            continue
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        ins = list(md.disasm(gexe.at(lo - 0x400000, hi - lo), lo))
        consts = {}
        for i in ins:
            b = bytes(i.bytes)
            if b[:2] == b'\xd8\x0d':
                c = int.from_bytes(b[2:6], 'little')
                v = struct.unpack('<f', gexe.at(c - 0x400000, 4))[0]
                consts.setdefault(round(v, 6), []).append(i.address)
        check(ins[0].mnemonic == 'mov' and ins[0].op_str == 'eax, 0xa', f'{name} menuCamLoop: mov eax, 10')
        check(ins[-1].mnemonic == 'jne' and int(ins[-1].op_str, 16) == lo + 5, f'{name} menuCamLoopEnd: the loop jumps back')
        check(len(consts.get(0.001, [])) == 6 and len(consts.get(0.002, [])) == 6 and len(consts) == 2,
              f'{name} menuCamLoop: six fmul [0.001], six fmul [0.002]')
    # A person's step (fps_unlock): each sweep holds one `fsub dword [0.015]` and returns with ret 0xC; humanMove and
    # humanUpdate call both sweeps and return with ret 4; humanMove compares humanMoveState with 3 and 1.
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['humanMove']:
            continue
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.detail = True
        for f in ('humanSweep', 'humanSweepOwner'):
            ins = list(md.disasm(gexe.at(exe[f] - 0x400000, 0x480), exe[f]))
            subs = [i for i in ins if bytes(i.bytes[:2]) == b'\xd8\x25' and abs(struct.unpack(
                '<f', gexe.at(int.from_bytes(bytes(i.bytes[2:6]), 'little') - 0x400000, 4))[0] - 0.015) < 1e-7]
            check(len(subs) == 1, f'{name} {f}: one fsub dword [0.015]')
            check(any(i.mnemonic == 'ret' and i.op_str == '0xc' for i in ins), f'{name} {f}: ret 0xc')
        for f, size in (('humanMove', 0x1500), ('humanUpdate', 0x2000)):
            ins = list(md.disasm(gexe.at(exe[f] - 0x400000, size), exe[f]))
            calls = {int(i.op_str, 16) for i in ins if i.mnemonic == 'call' and i.op_str.startswith('0x')}
            check(exe['humanSweep'] in calls and exe['humanSweepOwner'] in calls, f'{name} {f}: calls both sweeps')
            check(any(i.mnemonic == 'ret' and i.op_str == '4' for i in ins), f'{name} {f}: ret 4')
            if f == 'humanMove':   # the probe's test: `mov eax, [esi + state]`, then `cmp eax, 3` and `cmp eax, 1`
                ok = any(a.mnemonic == 'mov' and a.op_str.startswith('eax, dword ptr [esi + ')
                         and a.op_str.endswith(f'+ {exe["humanMoveState"]:#x}]') and
                         [f'{b.mnemonic} {b.op_str}' for b in ins[k + 1:k + 6] if b.mnemonic == 'cmp'] == ['cmp eax, 3', 'cmp eax, 1']
                         for k, a in enumerate(ins))
                check(ok, f'{name} humanMoveState loaded and compared with 3 and 1')
    # The sweeps' lengths (fps_unlock's fix_walk finds the same operands): humanSweep calls humanSlide and holds one
    # `fadd [0.405]` and one `fsub [0.015]`, its result the sphere's centre minus an immediate 0.4 (`mov [esp + 0x18],
    # 0.4f`); humanSweepOwner calls humanSlideOwner and holds one `fadd [0.4]` and one `fsub [0.015]`; each slide nine
    # `fmul [0.01]`, humanSlide ret 4, humanSlideOwner ret 8. The spans are the functions' lengths, as the mod reads them.
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['humanSlide']:
            continue
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.skipdata = True

        def ops(va, span, opcode, value):
            code = gexe.at(va - 0x400000, span)
            n = 0
            for i in range(len(code) - 6):
                if code[i] == 0xD8 and code[i + 1] == opcode:
                    c = int.from_bytes(code[i + 2:i + 6], 'little')
                    v = gexe.at(c - 0x400000, 4)
                    n += len(v) == 4 and struct.unpack('<f', v)[0] == struct.unpack('<f', struct.pack('<f', value))[0]
            return n

        calls = lambda va, span: {int(i.op_str, 16) for i in md.disasm(gexe.at(va - 0x400000, span), va)
                                  if i.mnemonic == 'call' and i.op_str.startswith('0x')}
        check(exe['humanSlide'] in calls(exe['humanSweep'], 0x480), f'{name} humanSweep calls humanSlide')
        check(exe['humanSlideOwner'] in calls(exe['humanSweepOwner'], 0x200), f'{name} humanSweepOwner calls humanSlideOwner')
        check(ops(exe['humanSweep'], 0x480, 0x05, 0.405) == 1 and ops(exe['humanSweep'], 0x480, 0x25, 0.015) == 1,
              f'{name} humanSweep: one fadd [0.405], one fsub [0.015]')
        check(b'\xc7\x44\x24\x18\xcd\xcc\xcc\x3e' in gexe.at(exe['humanSweep'] - 0x400000, 0x480), f'{name} humanSweep: its result 0.4 below')
        check(ops(exe['humanSweepOwner'], 0x200, 0x05, 0.4) == 1 and ops(exe['humanSweepOwner'], 0x200, 0x25, 0.015) == 1,
              f'{name} humanSweepOwner: one fadd [0.4], one fsub [0.015]')
        for f, ret in (('humanSlide', 'ret 4'), ('humanSlideOwner', 'ret 8')):
            body = list(md.disasm(gexe.at(exe[f] - 0x400000, 0xA80), exe[f]))
            check(ops(exe[f], 0xA80, 0x0D, 0.01) == 9 and any(f'{i.mnemonic} {i.op_str}' == ret for i in body),
                  f'{name} {f}: nine fmul [0.01], {ret}')
    # A car's tick (fps_unlock): vehicleTick is what C_car::AI calls on its C_Vehicle (`lea ecx, [esi + vehicle]`),
    # turns 0 ms into 1, ends by testing vehicleFlags for 2 and its counter for 1000 ms (ret 4); vehicleStepTime is
    # stored by the Engine that Tick calls (right before its `ret 0xc`) and added by SetBrake.
    ai = {'1.2': (0x41F780, 0x4CB2D0), '1.0': (0x466840, 0x517600)}
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['vehicleTick']:
            continue
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.skipdata = True
        car_ai, set_brake = ai[name]
        ins = list(md.disasm(gexe.at(car_ai - 0x400000, 0x400), car_ai))
        k = next((k for k, i in enumerate(ins) if i.mnemonic == 'call' and i.op_str == hex(exe['vehicleTick'])), -1)
        check(k > 0 and ins[k - 1].op_str == f'ecx, [esi + {exe["vehicle"]:#x}]', f'{name} vehicleTick: called by C_car::AI on its vehicle')
        tick = list(md.disasm(gexe.at(exe['vehicleTick'] - 0x400000, 0x1200), exe['vehicleTick']))
        check([f'{i.mnemonic} {i.op_str}' for i in tick[:12]].count('mov ecx, 1') == 1, f'{name} vehicleTick: 0 ms taken as 1')
        flags = f'dword ptr [esi + {exe["vehicleFlags"]:#x}]'
        end = max((k for k, i in enumerate(tick) if i.mnemonic == 'cmp' and i.op_str == 'ecx, 0x3e8'), default=-1)
        check(end > 0 and any(i.mnemonic == 'mov' and i.op_str == f'eax, {flags}' for i in tick[end - 16:end]) and
              any(i.mnemonic == 'test' and i.op_str == 'al, 2' for i in tick[end - 16:end]),
              f'{name} vehicleFlags: Tick tests it for 2 before its last 1000 ms')
        calls = [int(i.op_str, 16) for i in tick if i.mnemonic == 'call' and i.op_str.startswith('0x')]
        stored = False
        for c in set(calls):
            body = list(md.disasm(gexe.at(c - 0x400000, 0x400), c))
            for j, i in enumerate(body):
                if i.mnemonic == 'ret' and i.op_str == '0xc':
                    stored |= any(b.mnemonic == 'mov' and b.op_str.endswith(f'+ {exe["vehicleStepTime"]:#x}], edx') for b in body[max(0, j - 6):j])
                    break
        # (host/control.cpp: the rest counters a mod's step starts again: +4 the ms near the place at +8, compared
        # with 4000; vehicleFlags + 4 the ms calm, written by Tick)
        tops = [f'{i.mnemonic} {i.op_str}' for i in tick]
        check('cmp dword ptr [esi + 4], 0xfa0' in tops and 'mov dword ptr [esi + 4], 0' in tops and
              f'mov dword ptr [esi + {exe["vehicleFlags"] + 4:#x}], 0' in tops,
              f'{name} vehicleTick: the rest counters at +4 (place +8) and vehicleFlags + 4')
        # (host/control.cpp: Engine copies the body's matrix vehMatrix and its inverse (+0x40) into the step before's
        # (vehMatrixPrev and +0x40), 16 dwords each)
        eng = [f'{i.mnemonic} {i.op_str}' for i in md.disasm(gexe.at(exe['vehicleEngine'] - 0x400000, 0x200), exe['vehicleEngine'])]
        vm = exe['vehMatrix']
        check(all(any(e.startswith('lea') and e.endswith(f'+ {o:#x}]') for e in eng) for o in (vm, vm + 0x40, exe['vehMatrixPrev'], exe['vehMatrixPrev'] + 0x40)),
              f'{name} vehMatrix: Engine copies it and its inverse into vehMatrixPrev')
        brake = list(md.disasm(gexe.at(set_brake - 0x400000, 0x120), set_brake))
        check(stored and any(i.mnemonic == 'fld' and i.op_str.endswith(f'+ {exe["vehicleStepTime"]:#x}]') for i in brake),
              f'{name} vehicleStepTime: stored by the Engine Tick calls, read by SetBrake')
    # The car's physics further (game.h Car, the wheel's layout): Tick calls Engine; Engine compares and divides its dt
    # by vehSubstepMax, walks vehWheelCount wheels of vehWheels, calls Move, ret 0xC; Move turns the axes at vehAxes
    # (forward, right, up: `lea` of the three before the turn), stores vehTurn, calls the wheels' collision and
    # RotationMatrix (through Game.exe's thunk of LS3DF's export), ret 0x18; the wheels' collision runs the suspension
    # on game.h's kWheel* fields; CreateActor news carSize bytes for C_car's constructor, the wheel's maker kWheelSize.
    gh = open(os.path.join(ROOT, 'sdk', 'include', 'mml', 'game', 'game.h')).read()
    wheel = {m.group(1): int(m.group(2), 0) for m in re.finditer(r'\b(kWheel\w+) = (0x[0-9A-Fa-f]+|\d+)', gh)}
    phys = {'1.2': {'carCtor': 0x41BB80, 'wheelNew': 0x4D0DD0}, '1.0': {'carCtor': 0x462AF0, 'wheelNew': 0x51CCF0}}
    rotmat = b'?RotationMatrix@S_quat@@QBG?AUS_matrix@@XZ'
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['vehicleEngine']:
            continue
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md.skipdata = True
        dis = lambda va, n: list(md.disasm(gexe.at(va - 0x400000, n), va))
        txt = lambda ins: [f'{i.mnemonic} {i.op_str}' for i in ins]
        calls = lambda ins: {int(i.op_str, 16) for i in ins if i.mnemonic == 'call' and i.op_str.startswith('0x')}
        check(exe['vehicleEngine'] in calls(dis(exe['vehicleTick'], 0x1200)), f'{name} vehicleTick calls vehicleEngine')
        eng = dis(exe['vehicleEngine'], 0x400)
        e = txt(eng)
        sub = f'dword ptr [ebx + {exe["vehSubstepMax"]:#x}]'
        check(f'fcomp {sub}' in e and f'fdiv {sub}' in e, f'{name} vehSubstepMax: Engine compares and divides dt by it')
        check(f'mov eax, dword ptr [ebx + {exe["vehWheelCount"]:#x}]' in e and f'mov ecx, dword ptr [ebx + {exe["vehWheels"]:#x}]' in e,
              f'{name} vehWheelCount, vehWheels: Engine walks the wheels')
        check(exe['vehicleMove'] in calls(eng) and 'ret 0xc' in e, f'{name} vehicleEngine calls vehicleMove, ret 0xc')
        mv = dis(exe['vehicleMove'], 0x4A60)   # (to its ret 0x18: 0x4A54 in both builds)
        m = txt(mv)
        ax = exe['vehAxes']
        k = next((k for k, s in enumerate(m) if s == f'lea ebx, [ebp + {ax:#x}]'), -1)
        check(k >= 0 and f'lea esi, [ebp + {ax + 0xC:#x}]' in m[k:k + 4] and f'lea edi, [ebp + {ax + 0x18:#x}]' in m[k:k + 4],
              f'{name} vehAxes: Move takes the forward, right and up axes')
        check(f'fstp dword ptr [ebp + {exe["vehTurn"]:#x}]' in m, f'{name} vehTurn: Move stores the angular velocity')
        check(exe['vehicleWheelsColl'] in calls(mv), f'{name} vehicleMove calls vehicleWheelsColl')
        # RotationMatrix through its thunk: `jmp [IAT]`, the IAT entry the import of LS3DF's export
        gexe.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
        iat = {imp.address for d in gexe.pe.DIRECTORY_ENTRY_IMPORT for imp in d.imports if imp.name == rotmat}
        thunks = {c for c in calls(mv) if gexe.at(c - 0x400000, 2) == b'\xff\x25' and
                  struct.unpack('<I', gexe.at(c - 0x400000 + 2, 4))[0] in iat}
        check(len(thunks) == 1 and 'ret 0x18' in m, f'{name} vehicleMove calls RotationMatrix (its thunk), ret 0x18')
        # the suspension: fld [r + free length]; fsub; fnstsw ax; fmul [r + stiffness] ... fst [r + force] ... the
        # damper, the on-ground bit cleared from the flags, the compression stored
        wc = txt(dis(exe['vehicleWheelsColl'], 0x2400))
        regs = [s.split('[')[1].split(' ')[0] for s in wc if s.startswith('fmul dword ptr [e') and s.endswith(f'+ {wheel["kWheelStiffness"]:#x}]')]
        ok = False
        for r in set(regs):
            f = lambda o: f'dword ptr [{r} + {wheel[o]:#x}]'
            for j, s in enumerate(wc):
                if s == f'fld {f("kWheelFreeLength")}' and f'fmul {f("kWheelStiffness")}' in wc[j:j + 4]:
                    block = wc[j:j + 40]
                    ok = (f'fst {f("kWheelForce")}' in block and f'fmul {f("kWheelDamper")}' in block and
                          f'mov eax, {f("kWheelFlags")}' in block and 'and al, 0xf7' in block and
                          any(s2.startswith(f'mov {f("kWheelCompression")}, e') for s2 in block) and
                          f'mov al, byte ptr [{r} + {wheel["kWheelFlags"]:#x}]' in block and
                          f'test al, {wheel["kWheelOnGround"]}' in block)
                    if ok:
                        break
            if ok:
                break
        check(ok, f'{name} game.h kWheel*: the suspension in vehicleWheelsColl')
        ca = txt(dis(exe['missionCreateActor'], 0x200))
        j = next((j for j, s in enumerate(ca) if s == f'push {exe["carSize"]:#x}'), -1)
        check(j >= 0 and f'call {phys[name]["carCtor"]:#x}' in ca[j:j + 12], f'{name} carSize: CreateActor news it for C_car')
        check(f'push {wheel["kWheelSize"]:#x}' in txt(dis(phys[name]['wheelNew'], 0x10)), f'{name} game.h kWheelSize: the wheel\'s new')
    # LS3DF's quaternions: each address is its export's (by name), Make returns with ret 0xC, and RotationMatrix and
    # SetRot3 begin with their |w| test against 1.0 and 0.99999999 (the identity for a w rounded to 1)
    names = {'quatMake': b'?Make@S_quat@@QAGXABUS_vector@@M@Z', 'quatRotationMatrix': rotmat,
             'matrixSetRot3': b'?SetRot3@S_matrix@@QAGXABUS_quat@@@Z'}
    for name, (exe, ls, gexe, gls) in builds.items():
        if not ls['quatRotationMatrix']:
            continue
        gls.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
        exports = {e.name: e.address for e in gls.pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
        for f, n in names.items():
            check(exports.get(n) == ls[f], f'{name} LS3DF {f} {ls[f]:#x}: the export {n.decode()}')
        check(b'\xc2\x0c\x00' in gls.at(ls['quatMake'], 0x100), f'{name} LS3DF quatMake returns with ret 0xc')
        for f, limit in (('quatRotationMatrix', 1.0), ('matrixSetRot3', 0.9999999900000001)):
            head = list(gls.md.disasm(gls.at(ls[f], 0x20), gls.base + ls[f]))
            fc = next((i for i in head if i.mnemonic == 'fcomp' and i.op_str.startswith('qword ptr [0x')), None)
            c = struct.unpack('<d', gls.at(int(fc.op_str[11:-1], 16) - gls.base, 8))[0] if fc else None
            check(any(i.mnemonic == 'fabs' for i in head) and c == limit, f'{name} LS3DF {f}: |w| compared with {limit}')
    # What happens (host/happen.cpp): NewShoot, NewExplosion and CarExplosion begin the same in both builds and end
    # with their arguments' ret (0x2C, 0x20, 4).
    for f, ret in (('newShoot', '0x2c'), ('newExplosion', '0x20'), ('carExplosion', '4'), ('carCrash', '0x1c')):
        a, b = norm(g12.insn(e12[f] - 0x400000)), norm(g10.insn(e10[f] - 0x400000))
        check(a == b, f'{f}: 1.2 "{a}" vs 1.0 "{b}"')
        for name, (exe, ls, gexe, gls) in builds.items():
            first = next((i for i in gexe.md.disasm(gexe.at(exe[f] - 0x400000, 0x2000), exe[f]) if i.mnemonic == 'ret'), None)
            check(first is not None and first.op_str == ret, f'{name} {f}: its first ret is ret {ret}')
    # The car's crash divides |impulse| by the car's mass (C_car + vehicle + vehMass), and its prim's surface is its
    # byte +3; the shot queue is C_game +0x1D8 (the bullets' tick reads it before bulletRay); a person's landing
    # (MovementIntern, after waterFall) compares the fall's speed +0x418 in the state 3 and sets the state 1.
    gk = {m.group(1): int(m.group(2), 0) for m in re.finditer(r'\b(kHuman\w+) = (0x[0-9A-Fa-f]+|\d+)', gh)}
    for name, (exe, ls, gexe, gls) in builds.items():
        t = '\n'.join(f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['carCrash'] - 0x400000, 0x90), exe['carCrash']))
        check(f'fld dword ptr [esi + {exe["vehicle"] + exe["vehMass"]:#x}]' in t and 'mov al, byte ptr [ecx + 3]' in t,
              f'{name} carCrash: |impulse| / [car + {exe["vehicle"] + exe["vehMass"]:#x}] (the mass), the surface byte +3')
        t = ' '.join(f'{i.op_str}' for i in gexe.md.disasm(gexe.at(exe['bulletRay'] - 0x600 - 0x400000, 0x600), exe['bulletRay'] - 0x600))
        check('[ebp + 0x1d8]' in t and '[ebp + 0x1dc]' in t, f'{name} game.h kGameShots: the bullets\' tick reads C_game +0x1D8, +0x1DC')
        t = ' '.join(f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['bulletRay'] - 0x400000, 0x1000), exe['bulletRay']))
        check('mov al, byte ptr [esi + 0x50]' in t, f'{name} game.h kShotHits: the bullets\' tick reads the record\'s static impact flag +0x50')
        t = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['waterFall'] - 0x400000, 0x400), exe['waterFall'])]
        land = lambda k: (f'fld dword ptr [esi + {gk["kHumanFallSpeed"]:#x}]' in t[k:k + 8] and
                          f'mov dword ptr [esi + {exe["humanState"]:#x}], edi' in t[k:k + 8])
        check(any(s == f'cmp eax, {gk["kHumanStateAir"]}' and land(j) for j, s in enumerate(t)),
              f'{name} game.h kHumanFallSpeed: the landing reads +{gk["kHumanFallSpeed"]:#x} in the state {gk["kHumanStateAir"]}')
    # Questions to the world (host/collide.cpp): _TestLineH and DynSetNextCollOwnerMult begin the same in both builds
    # and end with ret 0x18 and ret 0xC; the latter writes the exclusion list (+0x2B8, +0x2BC, +0x2B0); the sphere
    # sweep's thunk sets ecx to g_collision and jumps to a function ending with ret 0x1C.
    for f, ret in (('testLineAll', '0x18'), ('collExclude', '0xc')):
        a, b = norm(g12.insn(e12[f] - 0x400000)), norm(g10.insn(e10[f] - 0x400000))
        check(a == b, f'{f}: 1.2 "{a}" vs 1.0 "{b}"')
        for name, (exe, ls, gexe, gls) in builds.items():
            first = next((i for i in gexe.md.disasm(gexe.at(exe[f] - 0x400000, 0x2000), exe[f]) if i.mnemonic == 'ret'), None)
            check(first is not None and first.op_str == ret, f'{name} {f}: its first ret is ret {ret}')
    # The scene's lights (host/lights.cpp): I3D_light's vtable is written by a constructor (mov [reg], imm) in both
    # builds; its slots' code is the same in both (the first 16 bytes); SetPower writes +0x158, SetRange +0x168 and
    # +0x16C, SetLightType +0x154.
    vt12, vt10 = builds['1.2'][1]['lightVtable'], builds['1.0'][1]['lightVtable']
    gl12, gl10 = builds['1.2'][3], builds['1.0'][3]
    for name, gls, vt in (('1.2', gl12, vt12), ('1.0', gl10, vt10)):
        imm = (gls.base + vt).to_bytes(4, 'little')
        check(any(b'\xc7' + bytes([r]) + imm in bytes(gls.mem) for r in range(8)), f'{name} LS3DF lightVtable written by a constructor')
    sl = lambda g, vt, o: struct.unpack('<I', g.at(vt + o, 4))[0] - g.base
    for o in (0x24, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64, 0x68):
        check(gl12.at(sl(gl12, vt12, o), 16) == gl10.at(sl(gl10, vt10, o), 16), f'LS3DF lightVtable+{o:#x}: the same code in both builds')
    for name, gls, vt in (('1.2', gl12, vt12), ('1.0', gl10, vt10)):
        for o, offs in ((0x5C, ('0x158',)), (0x64, ('0x168', '0x16c')), (0x50, ('0x154',))):
            code = ' '.join(f'{i.mnemonic} {i.op_str}' for i in gls.md.disasm(gls.at(sl(gls, vt, o), 0x60), gls.base + sl(gls, vt, o)))
            check(all(f'+ {x}]' in code for x in offs), f'{name} LS3DF lightVtable+{o:#x} writes {offs}')
    # The exhaust (host/effects.cpp): its first instruction the same in both builds, ret 4; it reads every field
    # game_build.h names for it, the emitter table, and C_Vehicle::Update calls it. The particles' table: read in
    # 20 places and more (C_car::LoadParticles), its count beside it.
    a, b = norm(g12.insn(e12['vehExhaust'] - 0x400000)), norm(g10.insn(e10['vehExhaust'] - 0x400000))
    check(a == b, f'vehExhaust: 1.2 "{a}" vs 1.0 "{b}"')
    for name, (exe, ls, gexe, gls) in builds.items():
        ins = list(gexe.md.disasm(gexe.at(exe['vehExhaust'] - 0x400000, 0x700), exe['vehExhaust']))
        body = ' '.join(f'{i.mnemonic} {i.op_str}' for i in ins)
        first = next((i for i in ins if i.mnemonic == 'ret'), None)
        check(first is not None and first.op_str == '4', f'{name} vehExhaust: its first ret is ret 4')
        fields = ('vehState', 'vehEngineOn', 'vehSpeed', 'vehExMaxSpeed', 'vehExType', 'vehExPipes', 'vehExJet', 'vehRps', 'vehRpsMax',
                  'vehMatrix', 'vehVelocity', 'vehTurn', 'vehCentre')
        missing = [f for f in fields if f'+ {exe[f]:#x}]' not in body]
        check(not missing and f'[{exe["emitterTable"]:#x}]' in body and f'[{exe["exEndShare"]:#x}]' in body, f'{name} vehExhaust reads its fields and the emitter table {missing}')
        code = bytes(gexe.mem)
        n = code.count(exe['particleDefs'].to_bytes(4, 'little'))
        c = code.count((exe['particleDefs'] + 4).to_bytes(4, 'little'))
        check(n >= 20 and c >= 1, f'{name} particleDefs: read in {n} places, its count in {c}')
    # The particles (host/effects.cpp): each vtable is the one a constructor writes before it stores the visual's
    # type ('PRTB', 'PRTE') at +0x1D4; its +0x18 and +0x2C are the frame's Update and SetParent (the same as
    # I3D_light's: one I3D_frame's code).
    for name, (exe, ls, gexe, gls) in builds.items():
        mem = bytes(gls.mem)
        for f, code in (('prtbVtable', b'PRTB'), ('prteVtable', b'PRTE')):
            vt = ls[f]
            imm = (gls.base + vt).to_bytes(4, 'little')
            ok = False
            for m in re.finditer(re.escape(b'\xd4\x01\x00\x00' + code), mem):
                k = m.start() - 2
                if mem[k] == 0xC7 and imm in mem[k - 0x200:k]:
                    ok = True
            check(ok, f'{name} LS3DF {f}: written before a {code.decode()} store at +0x1D4')
            for o in (0x18, 0x2C):
                check(gls.at(vt + o, 4) == gls.at(ls['lightVtable'] + o, 4), f'{name} LS3DF {f}+{o:#x}: the frame\'s own (as I3D_light\'s)')
    # The weather (host/effects.cpp): OsefujPocasi begins the same in both builds and ends with ret 8; the scene's
    # vtable +0xE8 / +0xEC / +0xF0 are the weather's setter (a switch on 16 parameters), getter (the same) and reset
    # (the same code in both builds).
    a, b = norm(g12.insn(e12['weatherSave'] - 0x400000)), norm(g10.insn(e10['weatherSave'] - 0x400000))
    check(a == b, f'weatherSave: 1.2 "{a}" vs 1.0 "{b}"')
    for name, (exe, ls, gexe, gls) in builds.items():
        first = next((i for i in gexe.md.disasm(gexe.at(exe['weatherSave'] - 0x400000, 0x200), exe['weatherSave']) if i.mnemonic == 'ret'), None)
        check(first is not None and first.op_str == '8', f'{name} weatherSave: its first ret is ret 8')
    sv = lambda g, vt, o: struct.unpack('<I', g.at(vt + o, 4))[0] - g.base
    for o in (0xE8, 0xEC):
        for name, (exe, ls, gexe, gls) in builds.items():
            head = ' '.join(f'{i.mnemonic} {i.op_str}' for i in gls.md.disasm(gls.at(sv(gls, ls['sceneVtable'], o), 0x20), 0))
            check('cmp eax, 0xf' in head, f'{name} LS3DF sceneVtable+{o:#x}: the weather\'s switch on 16 parameters')
    check(gl12.at(sv(gl12, builds['1.2'][1]['sceneVtable'], 0xF0), 16) == gl10.at(sv(gl10, builds['1.0'][1]['sceneVtable'], 0xF0), 16),
          'LS3DF sceneVtable+0xF0: the weather\'s reset, the same code in both builds')
    # Sound (host/sound.cpp): I3D_sound's vtable is written by a constructor in both builds; the slots the loader calls
    # have the same code in both (the first 16 bytes) and the arguments game_build.h gives (their first ret); SetVolume
    # writes +0x18C, SetFrequency +0x194. Music: CityMusicPaused writes C_game +0x2FC0 and ends with ret 4; the music
    # volume is what IShowSetVolume multiplies by (fld [musicVolume] before IShow's +0x38).
    s12, s10 = builds['1.2'][1]['soundVtable'], builds['1.0'][1]['soundVtable']
    for name, gls, vt in (('1.2', gl12, s12), ('1.0', gl10, s10)):
        imm = (gls.base + vt).to_bytes(4, 'little')
        check(any(b'\xc7' + bytes([r]) + imm in bytes(gls.mem) for r in range(8)), f'{name} LS3DF soundVtable written by a constructor')
    rets = {0x50: '0x14', 0x54: '8', 0x58: '8', 0x5C: '0x14', 0x60: '0xc', 0x64: '8', 0x68: '8', 0x6C: '8', 0x70: '8', 0x74: '8',
            0x80: '4', 0x84: '4', 0x88: '0xc'}
    for o, ret in rets.items():
        if o not in (0x50, 0x5C, 0x60):   # (Open reads the sound driver's global; SetRange, SetCone: their rets checked)
            check(gl12.at(sl(gl12, s12, o), 16) == gl10.at(sl(gl10, s10, o), 16), f'LS3DF soundVtable+{o:#x}: the same code in both builds')
        for name, gls, vt in (('1.2', gl12, s12), ('1.0', gl10, s10)):
            first = next((i for i in gls.md.disasm(gls.at(sl(gls, vt, o), 0x200), 0) if i.mnemonic == 'ret'), None)
            check(first is not None and first.op_str == ret, f'{name} LS3DF soundVtable+{o:#x}: its first ret is ret {ret}')
    for name, gls, vt in (('1.2', gl12, s12), ('1.0', gl10, s10)):
        for o, off in ((0x68, '0x18c'), (0x74, '0x194')):
            code = ' '.join(f'{i.mnemonic} {i.op_str}' for i in gls.md.disasm(gls.at(sl(gls, vt, o), 0x40), 0))
            check(f'+ {off}]' in code, f'{name} LS3DF soundVtable+{o:#x} writes {off}')
    # A single mesh's skinned vertices (host/motion.cpp): smCopySkinned walks an instance up to the mesh it shares (+0x1D8,
    # +0x08), reads the LOD's record (+0x1E4), its rigid part (+0x30), the cache (+0x34, +0x3C) and the I3D_mesh's LOD
    # (+0x1E0), ends with ret 0xC, and is called by the m_AddFGroups at +0x68 of the vtables the 'SNGM' (single meshes)
    # and 'SMMR' (single morphs: the people, their faces morphed) constructors write.
    for name, (exe, ls, gexe, gls) in builds.items():
        at = ls['smCopySkinned']
        body = [f'{i.mnemonic} {i.op_str}' for i in gls.md.disasm(gls.at(at, 0x200), gls.base + at)]
        rets = [b for b in body if b.startswith('ret')]
        joined = ' '.join(body)
        check(rets[:1] == ['ret 0xc'] and all(f'+ {o}]' in joined for o in ('0x1d8', '0x1e4', '0x1e0', '0x30', '0x34', '0x3c')),
              f"{name} LS3DF smCopySkinned: the LOD's record, its rigid part and cache, ret 0xc")
        for kind in (b'SNGM', b'SMMR'):
            ctor = bytes(gls.mem).find(bytes([0xC7, 0x86, 0xD4, 0x01, 0, 0]) + kind)   # (mov dword ptr [esi + 0x1d4], kind)
            before = gls.at(ctor - 0x20, 0x20) if ctor > 0 else b''
            w = before.rfind(bytes([0xC7, 0x06]))   # (the vtable written just before: mov dword ptr [esi], imm32)
            vt = struct.unpack('<I', before[w + 2:w + 6])[0] - gls.base if w >= 0 else None
            add = struct.unpack('<I', gls.at(vt + 0x68, 4))[0] - gls.base if vt is not None else None
            calls = add is not None and any(i.mnemonic == 'call' and i.op_str == f'{gls.base + at:#x}'
                                            for i in gls.md.disasm(gls.at(add, 0x1000), gls.base + add))
            check(calls, f"{name} LS3DF smCopySkinned: called by the m_AddFGroups at +0x68 of the {kind.decode()} vtable")
    for name, (exe, ls, gexe, gls) in builds.items():
        ins =[f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['cityMusicPaused'] - 0x400000, 0x20), exe['cityMusicPaused'])]
        check('mov byte ptr [ecx + 0x2fc0], al' in ins and 'ret 4' in ins, f'{name} cityMusicPaused writes C_game +0x2FC0, ret 4')
        imm = b'\xd9\x05' + exe['musicVolume'].to_bytes(4, 'little') + b'\xd8\x4c'
        check(bytes(gexe.mem).count(imm) >= 1, f'{name} musicVolume: loaded and multiplied (IShowSetVolume)')
    # The HUD's lines and marks (host/interact.cpp, host/hud.cpp): each begins as its twin does (the HUD's fields
    # and globals told apart) and ends with the ret game_build.h gives.
    for f, ret in (('hudHelpAdd', '4'), ('hudConsoleAdd', '8'), ('hudCompassSet', '4'), ('hudRadarAdd', '8'), ('hudRadarRemove', '4')):
        mn = lambda g, a: [i.mnemonic for i in list(g.md.disasm(g.at(a - 0x400000, 0x40), a))[:6]]
        check(mn(g12, e12[f]) == mn(g10, e10[f]), f'{f}: the same first instructions in both builds')
        for name, (exe, ls, gexe, gls) in builds.items():
            first = next((i for i in gexe.md.disasm(gexe.at(exe[f] - 0x400000, 0x400), exe[f]) if i.mnemonic == 'ret'), None)
            check(first is not None and first.op_str == ret, f'{name} {f}: its first ret is ret {ret}')
    # The HUD's flags (host/hud.cpp): AddFlag ors, RemoveFlag and-nots its argument into hud + hudFlags, ret 4; the
    # scripts' SETCOMPASS calls CompassSetDestination, then AddFlag(0x80), on the HUD.
    sc = {'1.2': 0x46E1D8, '1.0': 0x5BC005}
    for name, (exe, ls, gexe, gls) in builds.items():
        fl = exe['hudFlags']
        for f, op in (('hudAddFlag', 'or edx, eax'), ('hudRemoveFlag', 'and edx, eax')):
            ins = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe[f] - 0x400000, 0x18), exe[f])]
            check(f'mov edx, dword ptr [ecx + {fl:#x}]' in ins and op in ins and f'mov dword ptr [ecx + {fl:#x}], edx' in ins
                  and 'ret 4' in ins, f'{name} {f}: {op} into hud + {fl:#x}, ret 4')
        ins = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(sc[name] - 0x400000, 0x60), sc[name])]
        k = next((k for k, t in enumerate(ins) if t == f'call {exe["hudCompassSet"]:#x}'), -1)
        check(k > 0 and ins[k + 1:k + 4] == ['push 0x80', f'mov ecx, {exe["hud"]:#x}', f'call {exe["hudAddFlag"]:#x}'],
              f'{name} SETCOMPASS: the compass\' target, then hudAddFlag(0x80) on the HUD')
        cs = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['hudCompassSet'] - 0x400000, 0x10), exe['hudCompassSet'])]
        check(cs[:3] == ['mov eax, dword ptr [esp + 4]', f'mov dword ptr [ecx + {exe["hudCompassTarget"]:#x}], eax', 'ret 4'],
              f'{name} hudCompassSet writes hud + {exe["hudCompassTarget"]:#x}')
    # The saves (host/saves.cpp): SaveGameSave and SaveGameLoad format "savegame\mafia%03d.%03d" from the manager's
    # saveProfile and saveSlot (ESI the manager); SaveGameNewGame clears +0x1E8 and +0x98; ProfileDelete reads the
    # profile's number from its list (+4, 0x54 bytes a profile, +8) and ends ret 4; SaveGameSave ends ret 4.
    for name, (exe, ls, gexe, gls) in builds.items():
        fmt = None
        for f in ('saveGameSave', 'saveGameLoad'):
            ins = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe[f] - 0x400000, 0x120), exe[f])]
            k = next((k for k, t in enumerate(ins) if t == f'mov eax, dword ptr [esi + {exe["saveSlot"]:#x}]'), -1)
            ok = k >= 0 and ins[k + 1] == f'mov ecx, dword ptr [esi + {exe["saveProfile"]:#x}]'
            push = next((t for t in ins[k:k + 8] if t.startswith('push 0x')), None) if ok else None
            if push:
                va = int(push[5:], 16)
                fmt = bytes(gexe.at(va - 0x400000, 24)).split(b'\0')[0]
            check(ok and fmt == b'savegame\\mafia%03d.%03d', f'{name} {f}: the save\'s name from the manager\'s profile and slot')
        ng = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['saveGameNew'] - 0x400000, 0x24), exe['saveGameNew'])]
        check(ng[:2] == ['mov dword ptr [ecx + 0x1e8], 0', 'mov dword ptr [ecx + 0x98], 0'] and 'ret ' in ng,
              f'{name} saveGameNew: clears +0x1E8 and +0x98')
        pd = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['profileDelete'] - 0x400000, 0x200), exe['profileDelete'])]
        check('mov ebx, dword ptr [edi + eax + 8]' in pd and 'ret 4' in pd, f'{name} profileDelete: the profile\'s number read, ret 4')
        first = next((i for i in gexe.md.disasm(gexe.at(exe['saveGameSave'] - 0x400000, 0x800), exe['saveGameSave']) if i.mnemonic == 'ret'), None)
        check(first is not None and first.op_str == '4', f'{name} saveGameSave: ret 4')
    # People (host/humans.cpp). The scripts' animation commands call humanPlayAnim, humanStopAnim, humanChangeAnim;
    # the animation table's records name .i3d files with their numbers (111 the get-up from the back, 338 from the
    # front). Do_ShockDown (1.2 0x587C20, 1.0 0x4A8190) fills the action queue: humanActs, the state humanState 6,
    # humanActIndex 0. The fields mafia-ragdoll's code uses (bullets, getting up, trams): twins of 1.2's functions
    # with the same first instructions (addresses aside), bulletRay right after a call of collRayTest,
    # pedSystemVtable + 0x7C holding pedBulletHit, and the C_human offsets in the code that reads them.
    handlers = {'1.2': (0x47032E, 0x4703BF, 0x46F586), '1.0': (0x5BE122, 0x5BE1B3, 0x5BD38F)}
    shock = {'1.2': 0x587C20, '1.0': 0x4A8190}
    for name, (exe, ls, gexe, gls) in builds.items():
        for h, f in zip(handlers[name], ('humanPlayAnim', 'humanStopAnim', 'humanChangeAnim')):
            ins = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(h - 0x400000, 0xA0), h)]
            check(f'call {exe[f]:#x}' in ins, f'{name} {f}: called by the scripts\' command')
        recs = [struct.unpack_from('<II', bytes(gexe.at(exe['animTable'] - 0x400000 + 8 * k, 8))) for k in range(exe['animCount'])]
        names = {}
        for p, n in recs:
            s = bytes(gexe.at(p - 0x400000, 64)).split(b'\0')[0]
            names.setdefault(n, s.lower())
        check(all(bytes(gexe.at(p - 0x400000, 64)).split(b'\0')[0].lower().endswith(b'.i3d') for p, n in recs)
              and names.get(111) == b'smrt pad end vstavani.i3d' and names.get(338) == b'x panika leh nahoru.i3d',
              f'{name} animTable: {exe["animCount"]} records of .i3d files, 111 and 338 the get-ups')
        pa = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['humanPlayAnim'] - 0x400000, 0xC0), exe['humanPlayAnim'])]
        check(f'lea edi, [esi + {exe["humanAnimMachine"]:#x}]' in pa and f'call {exe["animTime"]:#x}' in pa,
              f'{name} humanPlayAnim: its animations machine at +{exe["humanAnimMachine"]:#x}, animTime called on it')
        sd = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(shock[name] - 0x400000, 0x90), shock[name])]
        check(f'mov dword ptr [ecx + {exe["humanActs"]:#x}], esi' in sd and f'mov dword ptr [ecx + {exe["humanState"]:#x}], 6' in sd
              and f'mov dword ptr [ecx + {exe["humanActIndex"]:#x}], edx' in sd, f'{name} Do_ShockDown: the action queue, its state and index')
    def first14(g, a):   # (up to its first ret: a short function's padding differs)
        out = []
        for i in list(g.md.disasm(g.at(a - 0x400000, 0xC8), a))[:14]:
            out.append(i.mnemonic + ' ' + re.sub(r'0x[0-9a-f]{5,}', 'A', i.op_str))
            if i.mnemonic == 'ret':
                break
        return out
    for f in ('collRayTest', 'humanBoneRayTest', 'pedBulletHit', 'pedAnimStart', 'collGridAdd', 'collGridMove', 'humanPlatform',
              'tramCarOf', 'pedHit', 'humanPlayAnim', 'humanStopAnim', 'humanChangeAnim'):
        check(first14(g12, e12[f]) == first14(g10, e10[f]), f'{f}: the same first instructions in both builds')
    for name, (exe, ls, gexe, gls) in builds.items():
        before = list(gexe.md.disasm(gexe.at(exe['bulletRay'] - 5 - 0x400000, 5), exe['bulletRay'] - 5))
        check(len(before) == 1 and before[0].mnemonic == 'call' and int(before[0].op_str, 16) == exe['collRayTest'],
              f'{name} bulletRay: right after a call of collRayTest')
        slot = struct.unpack('<I', bytes(gexe.at(exe['pedSystemVtable'] + 0x7C - 0x400000, 4)))[0]
        check(slot == exe['pedBulletHit'], f'{name} pedSystemVtable + 0x7C: pedBulletHit')
    # The using objects (host/interact.cpp): the player's Action on foot calls FindNearObjects on C_game + 0x29EC, then
    # ExecuteMenu (`xor dl, dl` before it) and compares its result with 0xA8; AddObject and DelObject end ret 4,
    # FindNearObjects ret 0x10, ExecuteMenu ret 4.
    ab = {'1.2': 0x594300, '1.0': 0x4CBD00}
    for name, (exe, ls, gexe, gls) in builds.items():
        ins = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(ab[name] - 0x400000, 0x1000), ab[name])]
        k = next((k for k, t in enumerate(ins) if t == f'call {exe["uoFind"]:#x}'), -1)
        check(k > 0 and ins[k - 1] == 'lea ecx, [eax + 0x29ec]', f'{name} uoFind: the Action calls it on C_game + 0x29EC')
        m = next((m for m, t in enumerate(ins) if t == f'call {exe["menuExecute"]:#x}'), -1)
        check(m > k and 'xor dl, dl' in ins[m - 4:m] and ins[m + 1] == 'cmp eax, 0xa8', f'{name} menuExecute: the action menu run, its result against 0xA8')
        for f, ret in (('uoAdd', '4'), ('uoDel', '4'), ('uoFind', '0x10'), ('menuExecute', '4')):
            first = next((i for i in gexe.md.disasm(gexe.at(exe[f] - 0x400000, 0x800), exe[f]) if i.mnemonic == 'ret'), None)
            check(first is not None and first.op_str == ret, f'{name} {f}: its first ret is ret {ret}')
    # The game's state (host/state.cpp): SetScore writes C_game +0x3640, reads +0x363E, ends with ret 4.
    for name, (exe, ls, gexe, gls) in builds.items():
        sc = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['gameSetScore'] - 0x400000, 0x30), exe['gameSetScore'])]
        check('mov dword ptr [ecx + 0x3640], eax' in sc and 'mov dl, byte ptr [ecx + 0x363e]' in sc and 'ret 4' in sc,
              f'{name} gameSetScore: +0x3640 written, +0x363E read, ret 4')
        hl = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['hudSetLives'] - 0x400000, 0x30), exe['hudSetLives'])]
        imm = b'\xb9' + exe['hud'].to_bytes(4, 'little') + b'\xe8'
        code = bytes(gexe.mem)
        called = any(k + 10 + struct.unpack('<i', code[k + 6:k + 10])[0] == exe['hudSetLives'] - 0x400000
                     for k in (m.start() for m in re.finditer(re.escape(imm), code)))
        check(hl[0] == 'mov eax, dword ptr [esp + 4]' and hl[1].startswith('mov dword ptr [ecx + 0x') and called,
              f'{name} hudSetLives: stores its value, called with ecx = the HUD')
    for name, (exe, ls, gexe, gls) in builds.items():
        ce = ' '.join(f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['collExclude'] - 0x400000, 0x30), exe['collExclude']))
        check(all(f'[ecx + {o:#x}]' in ce for o in (0x2B8, 0x2BC, 0x2B0)), f'{name} collExclude writes +0x2B8, +0x2BC, +0x2B0')
        th = list(gexe.md.disasm(gexe.at(exe['sphereSweep'] - 0x400000, 0x20), exe['sphereSweep']))
        mv = next((i for i in th if i.mnemonic == 'mov' and i.op_str.startswith('ecx, 0x')), None)
        jm = next((i for i in th if i.mnemonic == 'jmp'), None)
        ok = mv is not None and int(mv.op_str[5:], 16) == exe['collision'] and jm is not None
        if ok:
            tgt = int(jm.op_str, 16)
            first = next((i for i in gexe.md.disasm(gexe.at(tgt - 0x400000, 0x2000), tgt) if i.mnemonic == 'ret'), None)
            ok = first is not None and first.op_str == '0x1c'
        check(ok, f'{name} sphereSweep: ecx = g_collision, a jump to a function with ret 0x1c')
    # The scene drawn from another camera (host/view.cpp): I3D_camera's vtable is written by its constructor; its
    # update (+0x44) asks the scene's GetSector (+0xB4) for the sector it keeps at +0x3F8 and makes the view (+0x164),
    # the view x projection (+0x1E4) and the backdrop's and the near pass' (+0x324, +0x3A4); the field of view's
    # setters (+0x50, +0x54) take one float, the range's (+0x58) two; the scene's vtable holds GetSector at +0xB4;
    # QueueVisual puts a 'LENS' (its type at +0x1D4) onto the scene's list at +0x288 (count +0x290); Render reads
    # camRecompute.
    for name, (exe, ls, gexe, gls) in builds.items():
        if not ls['cameraVtable']:
            continue
        vt, code = ls['cameraVtable'], bytes(gls.mem)
        slot = lambda v, o: struct.unpack('<I', gls.at(v + o, 4))[0] - gls.base
        ins = lambda rva, n: [f'{i.mnemonic} {i.op_str}' for i in gls.md.disasm(gls.at(rva, n), gls.base + rva)]
        imm = (gls.base + vt).to_bytes(4, 'little')
        check(any(b'\xc7' + bytes([r]) + imm in code for r in range(8)), f'{name} LS3DF cameraVtable written by a constructor')
        up = ins(slot(vt, 0x44), 0x200)
        check('call dword ptr [ecx + 0xb4]' in up and any(s.endswith('+ 0x3f8], eax') for s in up) and
              all(f'+ {o:#x}]' in ' '.join(up) for o in (0x164, 0x1E4, 0x324, 0x3A4)),
              f'{name} LS3DF cameraVtable+0x44: the update (GetSector, the sector at +0x3F8, the matrices)')
        for o, first in ((0x50, 'fld dword ptr [esp + 8]'), (0x54, 'fld dword ptr [esp + 8]'), (0x58, 'fld dword ptr [esp + 0xc]')):
            check(ins(slot(vt, o), 8)[:1] == [first], f'{name} LS3DF cameraVtable+{o:#x}: a setter ({first})')
        check(slot(ls['sceneVtable'], 0xB4) == ls['sceneGetSector'], f'{name} LS3DF sceneVtable+0xB4 is sceneGetSector')
        qv = ' '.join(ins(ls['queueVisual'], 0x1C0))
        check('mov eax, dword ptr [edi + 0x1d4]' in qv and 'cmp eax, 0x534e454c' in qv and '+ 0x288]' in qv and '+ 0x290]' in qv,
              f'{name} LS3DF queueVisual: a LENS onto the scene\'s list at +0x288')
        check(f'byte ptr [{gls.base + ls["camRecompute"]:#x}]' in ' '.join(ins(ls['render'], 0x600)),
              f'{name} LS3DF camRecompute read by Render')
        # The face groups' drawing (host/geom.cpp): each fgRender* is at +0x6C of a visual's vtable (slot 0 the frames'
        # Release), reads the entry's face group (+0x10) and its material (+0x04) at its start, returns with ret 4, and
        # calls SetIndices (the device's +0x154) and DrawIndexedPrimitive (+0x11C); the object's is I3D_object's.
        release = (gls.base + ls['frameRelease']).to_bytes(4, 'little')
        vts = {s for s in range(0, len(code) - 0x70, 4) if code[s:s + 4] == release}
        for f in ('fgRenderObject', 'fgRenderLit', 'fgRenderBillboard'):
            fn = (gls.base + ls[f]).to_bytes(4, 'little')
            body = ins(ls[f], 0x80)
            whole = ' '.join(ins(ls[f], 0x1400))
            check(any(code[s + 0x6C:s + 0x70] == fn for s in vts), f'{name} LS3DF {f}: at +0x6C of a visual\'s vtable')
            check(any(s.endswith('+ 0x10]') for s in body) and any(s.endswith('+ 4]') for s in body) and 'ret 4' in whole and
                  'call dword ptr [' in whole and '+ 0x154]' in whole and '+ 0x11c]' in whole,
                  f'{name} LS3DF {f}: the entry\'s face group and material, SetIndices, DrawIndexedPrimitive, ret 4')
    # The frame rate's sites (fps_unlock): the limiter's minimum jump inside frameLimit, after `cmp eax, edi` and
    # before its Sleep; Present's yield: `mov reg, [Sleep]` then `push 0; call reg`.
    for name, (exe, ls, gexe, gls) in builds.items():
        j = ls['limiterMinJump']
        check(ls['frameLimit'] < j < ls['frameLimit'] + 0x40 and gls.at(j - 2, 4) == b'\x3b\xc7\x73\x18',
              f'{name} limiterMinJump: cmp eax, edi; jae +0x18 inside frameLimit')
        sleep = gls.at(j + 9, 4)   # the limiter's own `call [Sleep]` (FF 15 <IAT>) after `mov esi, edi; sub esi, eax; push esi`
        check(gls.at(j + 7, 2) == b'\xff\x15', f'{name} limiterMinJump: the limiter calls Sleep after it')
        # the clock fps_unlock reads: `mov eax, [clock]` 13 bytes before the jump, the clock the limiter loads first
        check(gls.at(j - 0x0D, 1) == b'\xa1' and gls.at(j - 0x0C, 4) == gls.at(ls['frameLimit'] + 9, 4),
              f'{name} limiterMinJump: mov eax, [clock] before it')
        y = ls['presentYield']
        mov = gls.at(y - 6, 6)
        reg = {0x1D: 0xD3, 0x3D: 0xD7}.get(mov[1]) if len(mov) == 6 else None
        check(mov[:1] == b'\x8b' and mov[2:] == sleep and reg is not None and gls.at(y, 4) == bytes([0x6A, 0x00, 0xFF, reg]),
              f'{name} presentYield: mov reg, [Sleep]; push 0; call reg')
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
    # Restore points (host/saves.cpp; game.h kGameState, kSaveSuppress): the game loop loads the save slot when
    # C_game +0x48 is 3 (mov ecx, [edx + 0x48]; ...; cmp ecx, 3; ...; call saveGameLoad), and SaveGameFromScript
    # reads and clears the manager's one-shot flag +0x1D4 (its first instructions).
    loops = {'1.2': (0x5BE9EB, 0x6072F0), '1.0': (0x5F97D5, 0x559320)}
    for name, (exe, ls, gexe, gls) in builds.items():
        at, from_script = loops[name]
        ins = list(capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32).disasm(gexe.at(at - 0x400000, 0x40), at))
        text = [f'{i.mnemonic} {i.op_str}' for i in ins]
        check(len(text) > 12 and text[2] == 'mov ecx, dword ptr [edx + 0x48]' and 'cmp ecx, 3' in text
              and f'call {exe["saveGameLoad"]:#x}' in text, f'{name} the game loop loads the slot on C_game +0x48 == 3: {text[:13]}')
        ins = list(capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32).disasm(gexe.at(from_script - 0x400000, 0x20), from_script))
        check(any('+ 0x1d4]' in i.op_str for i in ins[:4]), f'{name} SaveGameFromScript reads +0x1D4 first')
    # The start (host/start.cpp): 1.2's menus' vtables hold their command handlers at +0x24, 1.0's screens their
    # ticks at +0x14 (read in the decompile: the profile menu's takes 100 + i, the main menu's 990105, the freeride
    # menu's 201, and the freeride menu's kind is the field its handler reads); PlayVideo starts with its frame.
    handlers = {'1.2': (0x24, {'menuProfileVt': 0x5E88D0, 'menuMainVt': 0x5E06E0, 'menuFreerideVt': 0x5E1B40}),
                '1.0': (0x14, {'menuProfileVt': 0x579CC0, 'menuMainVt': 0x576A20, 'menuFreerideVt': 0x57AD80})}
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['menuProfileVt']:
            continue
        slot, hs = handlers[name]
        for f, handler in hs.items():
            got = int.from_bytes(gexe.at(exe[f] + slot - 0x400000, 4), 'little')
            check(got == handler, f'{name} {f}+{slot:#x} is {got:#x}, not the handler {handler:#x}')
        check(exe['freerideKind'] in operands(gexe, hs['menuFreerideVt'], 0x200), f'{name} freerideKind {exe["freerideKind"]:#x} in the freeride start')
        if exe['playVideo']:
            i = gexe.insn(exe['playVideo'] - 0x400000)
            check(i and f'{i.mnemonic} {i.op_str}' == 'sub esp, 0x214', f'{name} playVideo starts "{i.mnemonic} {i.op_str}"')
    # The controls, the texts, the menus' components (host/input.cpp, host/start.cpp; both builds where known): the
    # controls object is ECX where the game calls its update, and the update uses the three arrays' offsets; the key
    # name reads the binding's device and returns with ret 4; the texts' lookups are the same code in both builds and
    # the text database is their ECX; the mouse sensitivity is loaded as a float; the menus' setter and getter call
    # the component's vtable +0x4C / +0x50 and return with ret 8 / ret 4; LS3D's keyboard read fills the state the
    # loader writes keys into.
    for name, (exe, ls, gexe, gls) in builds.items():
        if exe['controls']:
            code, calls = bytes(gexe.mem), 0
            at = code.find(b'\xb9' + exe['controls'].to_bytes(4, 'little'))
            while at >= 0:
                for i in gexe.md.disasm(code[at:at + 24], 0x400000 + at):
                    calls += i.mnemonic == 'call' and i.op_str == hex(exe['controlsUpdate'])
                at = code.find(b'\xb9' + exe['controls'].to_bytes(4, 'little'), at + 1)
            check(calls >= 2, f'{name} controls is ECX of {calls} calls of controlsUpdate')
            body = gexe.at(exe['controlsUpdate'] - 0x400000, 0x400)
            for f in ('controlsBind', 'controlsHeld', 'controlsPressed'):
                check(b'\x8b\x85' + exe[f].to_bytes(4, 'little') in body or b'\x8b\x8d' + exe[f].to_bytes(4, 'little') in body
                      or b'\x8b\x95' + exe[f].to_bytes(4, 'little') in body, f'{name} {f} {exe[f]:#x} read in controlsUpdate')
            body = gexe.at(exe['controlsKeyName'] - 0x400000, 0x40)
            check(b'\x66\x8b' in body and b'\xc2\x04\x00' in gexe.at(exe['controlsKeyName'] - 0x400000, 0x200),
                  f'{name} controlsKeyName reads a 16-bit device and returns with ret 4')
            for f in ('textGetOrNull', 'textGet'):
                i = gexe.insn(exe[f] - 0x400000)
                check(i and f'{i.mnemonic} {i.op_str}' == 'mov edx, dword ptr [ecx + 4]' and b'\xc2\x04\x00' in gexe.at(exe[f] - 0x400000, 0x18),
                      f'{name} {f} starts as read and returns with ret 4')
            check(code.count(b'\xb9' + exe['textDb'].to_bytes(4, 'little')) >= 20, f'{name} textDb is not the ECX of the texts\' lookups')
            sens = exe['mouseSensitivity'].to_bytes(4, 'little')
            check(code.find(b'\xd8\x0d' + sens) >= 0 or code.find(b'\xd9\x05' + sens) >= 0, f'{name} mouseSensitivity is never a float operand')
        if exe['menuSetValue'] and not exe['m10Tick']:   # 1.2: the component's own setter / getter
            for f, call, ret in (('menuSetValue', b'\xff\x52\x4c', b'\xc2\x08\x00'), ('menuGetValue', b'\xff\x52\x50', b'\xc2\x04\x00')):
                body = gexe.at(exe[f] - 0x400000, 0x20)
                check(call in body and ret in body, f'{name} {f}: the component\'s call and the ret')
        elif exe['menuSetValue']:   # 1.0: the control's +0x2C written / read, ret 8 / ret 4
            check(0x2C in operands(gexe, exe['menuSetValue'], 0x2A) and b'\xc2\x08\x00' in gexe.at(exe['menuSetValue'] - 0x400000, 0x30),
                  f'{name} menuSetValue writes +0x2C, ret 8')
            check(0x2C in operands(gexe, exe['menuGetValue'], 0x30) and b'\xc2\x04\x00' in gexe.at(exe['menuGetValue'] - 0x400000, 0x30),
                  f'{name} menuGetValue reads +0x2C, ret 4')
        if ls['keyboardUpdate']:
            body = gls.at(ls['keyboardUpdate'], 0x60)
            check((gls.base + ls['keyboardState']).to_bytes(4, 'little') in body, f'{name} LS3DF keyboardState in keyboardUpdate')
            check(b'\xc2\x04\x00' in body, f'{name} LS3DF keyboardUpdate returns with ret 4')
    # The mod kit (host/draw.cpp, the mission's events, game.h's Car; both builds): every address is where the code
    # that was read is, and every offset is in the function that uses it. The witnesses (functions the tables do not
    # hold): the HUD's text in 800x600 units (it scales by hudScale and adds hudXOffset), G_IndicatorsClass::DrawAll
    # (it takes the IGraph and the driver first), RadarSetPlayerCar (it writes hudCar), and C_Vehicle's SetFuel,
    # SetGear, SetSteer, GetRotations, CalcSpeed and its engine damage.
    kit = {'1.2': {'textUnits': 0x60E5C0, 'drawAll': 0x5FB060, 'setCar': 0x5FA500, 'setFuel': 0x4CB6E0, 'setGear': 0x4CB070,
                   'setSteer': 0x4CB4D0, 'getRotations': 0x4CD590, 'calcSpeed': 0x4CDE90, 'engineDamage': 0x4D6720},
           '1.0': {'textUnits': 0x560550, 'drawAll': 0x54D180, 'setCar': 0x54C620, 'setFuel': 0x517A10, 'setGear': 0x5173A0,
                   'setSteer': 0x517800, 'getRotations': 0x5195E0, 'calcSpeed': 0x519E50, 'engineDamage': 0x522F70}}

    def text(img, va):
        i = img.insn(va - img.base)
        return f'{i.mnemonic} {i.op_str}' if i else None
    for f in ('draw2d', 'hudOutText', 'hudTextSize', 'missionOpen', 'vehStateSize', 'vehStateSave'):
        a, b = norm(g12.insn(e12[f] - 0x400000)), norm(g10.insn(e10[f] - 0x400000))
        check(a == b, f'{f}: 1.2 "{a}" vs 1.0 "{b}"')
    for f in ('draw2d', 'missionOpen'):
        check(f not in [s[1] for s in sg], f'signature on {f}, a function the loader hooks')
    for name, (exe, ls, gexe, gls) in builds.items():
        w = kit[name]
        if not exe['draw2d']:
            continue
        body = gexe.at(exe['draw2d'] - 0x400000, 0x40)
        check(b'\xb9' + exe['hud'].to_bytes(4, 'little') in body and b'\xc2\x0c\x00' in body, f'{name} draw2d: the hud in ECX, ret 0xC')
        check(b'\xc2\x24\x00' in gexe.at(exe['hudOutText'] - 0x400000, 0x40), f'{name} hudOutText returns with ret 0x24')
        check(b'\xc2\x10\x00' in gexe.at(exe['hudTextSize'] - 0x400000, 0x20), f'{name} hudTextSize returns with ret 0x10')
        check(exe['hudFonts'] + 0x18 in operands(gexe, exe['hudOutText'], 0x80), f'{name} hudFonts: the first font\'s field in OutText')
        d = operands(gexe, w['textUnits'], gexe.at(w['textUnits'] - 0x400000, 0x80).find(bytes.fromhex('c22400')))   # up to its ret 0x24
        check(exe['hudScale'] in d and exe['hudScale'] + 4 in d and exe['hudOutText'] in d, f'{name} hudScale in the text\'s 800x600 wrapper')
        check((exe['hudXOffset'] in d) if exe['hudXOffset'] else not any(0x600000 <= v < 0x700000 for v in d - {exe['hudOutText']}),
              f'{name} hudXOffset in the wrapper (or none there)')
        check(text(gexe, w['drawAll']) == f'mov eax, dword ptr [{exe["igraph"]:#x}]', f'{name} igraph: DrawAll starts "{text(gexe, w["drawAll"])}"')
        check(exe['i3dDriver'] in operands(gexe, w['drawAll'], 0x40), f'{name} i3dDriver in DrawAll')
        check(b'\xc2\x10\x00' in gexe.at(exe['missionOpen'] - 0x400000, 0x80), f'{name} missionOpen returns with ret 0x10')
        d = operands(gexe, exe['missionOpen'], 0x100)
        check(exe['missionName'] in d and exe['missionClose'] in d, f'{name} missionOpen: the name\'s field, the close called')
        check(exe['hudCar'] in operands(gexe, w['setCar'], 0x10), f'{name} hudCar in RadarSetPlayerCar')
        for fn, n, fields in (('setFuel', 0x28, ('vehFuel', 'vehFuelTank')), ('setGear', 0x40, ('vehGear', 'vehGearMax')),
                              ('setSteer', 0xA0, ('vehSteerWanted', 'vehSteerMax')), ('getRotations', 0x20, ('vehEngineOn', 'vehRps')),
                              ('calcSpeed', 0x300, ('vehPos', 'vehSpeed', 'vehVelocity')),
                              ('engineDamage', 0x90, ('vehEngineHealth', 'vehEngineHealthMax'))):
            d = operands(gexe, w[fn], n)
            for f in fields:
                check(exe[f] in d, f'{name} {f} {exe[f]:#x} in {fn}')
        d = operands(gexe, exe['vehStateSave'], 0x440)
        for f in ('vehPos', 'vehVelocity', 'vehTurn', 'vehSteer', 'vehEngineHealth', 'vehGear', 'vehRps'):
            check(exe[f] in d, f'{name} {f} {exe[f]:#x} in SaveVehicleState')
        check(exe['vehStateSize'] in d and b'\xc7\x07\x09\x00\x00\x00' in gexe.at(exe['vehStateSave'] - 0x400000, 0x20),
              f'{name} vehStateSave: the version 9 written, vehStateSize called')
        body = gexe.at(exe['vehStateLoad'] - 0x400000, 0x40)
        check(b'\x83\x3b\x09' in body and b'\xc2\x04\x00' in body and exe['vehicleDeactivate'] in operands(gexe, exe['vehStateLoad'], 0x40),
              f'{name} vehStateLoad: the version 9 asked for, ret 4, the vehicle deactivated first')
        if ls['d3dDevice']:
            code = bytes(gls.mem)
            check(code.count(b'\xa1' + (gls.base + ls['d3dDevice']).to_bytes(4, 'little')) >= 20, f'{name} LS3DF d3dDevice is not the device the code takes')
    # The menus as objects (host/menus.cpp; 1.2 only so far): each function starts as it was read; the push makes the
    # menu the active one and creates it, ret 8; the constructor puts GM_Menu's vtable; PridejComponentPod finds the
    # control, makes the copy with the game's operator new and returns with ret 0x18; the definition's loader (the
    # first call of Create) walks the factories' list.
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['menuCreate']:
            continue
        first = {'menuCreate': 'push esi', 'menuDestroy': 'push esi', 'menuCtor': 'push -1', 'menuPush': 'mov ecx, dword ptr [esp + 4]',
                 'menuClose': 'push esi', 'menuAddUnder': 'sub esp, 0x10', 'menuFind': 'mov eax, dword ptr [ecx + 0x14]', 'gameNew': 'push 1'}
        for f, want in first.items():
            check(text(gexe, exe[f]) == want, f'{name} {f} starts "{text(gexe, exe[f])}", not "{want}"')
        d = operands(gexe, exe['menuPush'], 0x20)
        check(exe['menuTop'] in d and exe['menuCreate'] in d and b'\xc2\x08\x00' in gexe.at(exe['menuPush'] - 0x400000, 0x24),
              f'{name} menuPush: the active menu set, Create called, ret 8')
        check(exe['menuBaseVt'] in operands(gexe, exe['menuCtor'], 0x80), f'{name} menuBaseVt put by menuCtor')
        d = operands(gexe, exe['menuAddUnder'], 0x270)
        check(exe['menuFind'] in d and exe['gameNew'] in d and b'\xc2\x18\x00' in gexe.at(exe['menuAddUnder'] - 0x400000, 0x270),
              f'{name} menuAddUnder: the control found, the copy allocated, ret 0x18')
        check(b'\xc2\x04\x00' in gexe.at(exe['menuClose'] - 0x400000, 0x30), f'{name} menuClose returns with ret 4')
        loaders = [i.op_str for i in gexe.md.disasm(gexe.at(exe['menuCreate'] - 0x400000, 0x40), exe['menuCreate']) if i.mnemonic == 'call' and i.op_str.startswith('0x')]
        check(loaders and exe['menuFactories'] in operands(gexe, int(loaders[0], 16), 0x200), f'{name} menuFactories in the definition\'s loader')
        # the menus' mouse: the input tick (0x5EA8C0 in 1.2) moves menuCursor; GM_Control's constructor scales by
        # menuScale; the language's getter the definitions' loader calls reads menuLang; GetText starts as read
        tick, ctor, lang = 0x5EA8C0, 0x5E9BE0, 0x5F70D0
        d = operands(gexe, tick, 0x80)
        check(exe['menuCursor'] in d and exe['menuCursor'] + 4 in d, f'{name} menuCursor in the menus\' input tick')
        d = operands(gexe, ctor, 0x60)
        check(exe['menuScale'] in d and exe['menuScale'] + 4 in d, f'{name} menuScale in GM_Control\'s constructor')
        check(text(gexe, lang) == f'mov eax, dword ptr [{exe["menuLang"]:#x}]', f'{name} menuLang: the language getter starts "{text(gexe, lang)}"')
        check(text(gexe, exe['textGet']) == 'mov edx, dword ptr [ecx + 4]' and b'\xc2\x04\x00' in gexe.at(exe['textGet'] - 0x400000, 0x20),
              f'{name} textGet starts as read and returns with ret 4')
    # 1.0's screens (host/menus10.cpp): the base tick returns with ret 0x1C and is the base vtable's slot 5 and what
    # the three screens' ticks call; the constructor puts the base vtable, ret 8, the destructor too; the builder
    # looks for "stop" records and returns with ret 0xC, the screens' constructors give it the design table; the
    # update calls each control's slot 3; the copy-under allocates with the game's new, ret 0xC; the menus' loop adds
    # the mouse's moves to the cursor's two floats; the builder sets the scale; cd_find_game_file is called with
    # logo1.avi in ECX.
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['m10Tick']:
            continue
        body = lambda f, n: gexe.at(exe[f] - 0x400000, n)
        check(b'\xc2\x1c\x00' in body('m10Tick', 0x360), f'{name} m10Tick returns with ret 0x1C')
        check(int.from_bytes(gexe.at(exe['m10BaseVt'] + 0x14 - 0x400000, 4), 'little') == exe['m10Tick'], f'{name} m10BaseVt slot 5 is m10Tick')
        for f in ('menuProfileVt', 'menuMainVt', 'menuFreerideVt'):
            tick = int.from_bytes(gexe.at(exe[f] + 0x14 - 0x400000, 4), 'little')
            check(exe['m10Tick'] in operands(gexe, tick, 0x140), f'{name} {f}\'s tick calls m10Tick')
        check(exe['m10BaseVt'] in operands(gexe, exe['m10Ctor'], 0x90) and b'\xc2\x08\x00' in body('m10Ctor', 0xA0), f'{name} m10Ctor: the base vtable, ret 8')
        check(exe['m10BaseVt'] in operands(gexe, exe['m10Dtor'], 0x30), f'{name} m10Dtor puts the base vtable')
        check(0x73746F70 in operands(gexe, exe['m10Build'], 0xD0) and b'\xc2\x0c\x00' in body('m10Build', 0x350), f'{name} m10Build: "stop", ret 0xC')
        check(exe['m10Designs'] in operands(gexe, 0x57AAF0, 0x40), f'{name} m10Designs given to the builder by the freeride screen\'s constructor')
        check(b'\xff\x50\x0c' in body('m10UpdateAll', 0x20), f'{name} m10UpdateAll calls each control\'s slot 3')
        check(exe['gameNew'] in operands(gexe, exe['m10CloneUnder'], 0x430) and b'\xc2\x0c\x00' in body('m10CloneUnder', 0x430),
              f'{name} m10CloneUnder: the game\'s new, ret 0xC')
        code = bytes(gexe.mem)
        for off in (0, 4):
            check(code.find(b'\xd8\x05' + (exe['m10Cursor'] + off).to_bytes(4, 'little')) >= 0, f'{name} m10Cursor+{off}: a move added to it')
        for off in (0, 4):
            check(exe['menuScale'] + off in operands(gexe, exe['m10Build'], 0x60), f'{name} menuScale+{off} set by the builder')
        logo = code.find(b'logo1.avi\x00')
        at = code.find(b'\xb9' + (0x400000 + logo).to_bytes(4, 'little')) if logo >= 0 else -1
        calls = [i for i in gexe.md.disasm(code[at:at + 24], 0x400000 + at) if i.mnemonic == 'call'] if at >= 0 else []
        check(calls and int(calls[0].op_str, 16) == exe['m10CdFind'], f'{name} m10CdFind called with logo1.avi in ECX')
    # The scripts' machine (host/script.cpp; both builds): the constructor puts the standalone vtable, whose slot 6
    # is DecodeInstruction and slot 7 the opcode switch (read in the decompiles); CallProcess calls slot 7 and returns
    # with ret 4; Compile calls slots 6 and 5, ret 0x10; SetSourceCode ret 4; InvalidateActorReg walks act[] (+0x58,
    # +0x5C), ret 4; Done frees the three register files (+0x48, +0x50, +0x58).
    machine = {'1.2': {'decode': 0x463960, 'process': 0x46D3B0}, '1.0': {'decode': 0x5B1B60, 'process': 0x5BB2D0}}
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['progCtor']:
            continue
        body = lambda f, n: gexe.at(exe[f] - 0x400000, n)
        slot = lambda i: int.from_bytes(gexe.at(exe['progVt'] + 4 * i - 0x400000, 4), 'little')
        check(exe['progVt'] in operands(gexe, exe['progCtor'], 0x60), f'{name} progCtor puts progVt')
        check(slot(6) == machine[name]['decode'] and slot(7) == machine[name]['process'], f'{name} progVt slots 6, 7: {slot(6):#x}, {slot(7):#x}')
        check(any(b'\xff' + bytes([m]) + b'\x1c' in body('progCallProcess', 0x40) for m in (0x50, 0x51, 0x52, 0x53, 0x56, 0x57))
              and b'\xc2\x04\x00' in body('progCallProcess', 0x40), f'{name} progCallProcess calls slot 7, ret 4')
        cb = body('progCompile', 0x300)
        check(any(b'\xff' + bytes([m]) + b'\x18' in cb for m in range(0x50, 0x58)) and any(b'\xff' + bytes([m]) + b'\x14' in cb for m in range(0x50, 0x58))
              and b'\xc2\x10\x00' in cb, f'{name} progCompile calls slots 6 and 5, ret 0x10')
        check(b'\xc2\x04\x00' in body('progSetSource', 0x50), f'{name} progSetSource returns with ret 4')
        d = operands(gexe, exe['progInvalidateActor'], 0x2A)
        check(0x58 in d and 0x5C in d and b'\xc2\x04\x00' in body('progInvalidateActor', 0x30), f'{name} progInvalidateActor walks act[], ret 4')
        d = operands(gexe, exe['progDone'], 0x130)
        check(0x48 in d and 0x50 in d and 0x58 in d, f'{name} progDone frees flt[], frm[], act[]')
    for f in ('progSetSource', 'progCallProcess', 'progInvalidateActor'):
        a, b = norm(g12.insn(e12[f] - 0x400000)), norm(g10.insn(e10[f] - 0x400000))
        check(a == b, f'{f}: 1.2 "{a}" vs 1.0 "{b}"')
    # Actors made new and the game's tables (host/world.cpp; both builds): the scripts' ACTOR_DUPLICATE case makes its
    # copy with the driver's CreateFrame slot, the model cache's Open, frameSetPos, C_mission::CreateActor and
    # C_game::AddTemporaryActor; DAN_INTERNAL_1 (the cheat car) links its frame with sceneMoved, sets the car's
    # particles byte (through a setter that writes carParticles) and calls CalmVehicle; ACTOR_DELETE's tail calls
    # RemoveTemporaryActor; the model cache's Open returns with ret 0x18; the car list is ECX of GetCarName's calls; the
    # items' table and count are written by the tables' loader.
    world = {'1.2': {'dup': (0x474A14, 0x1B0), 'cheat': (0x47863E, 0x140), 'delete': 0x478CF4, 'carName': 0x60A510, 'tables': 0x60F5C0},
             '1.0': {'dup': (0x5C2804, 0x1D0), 'cheat': (0x5C62E5, 0x140), 'delete': 0x5C6ADA, 'carName': 0x55C540, 'tables': 0x592080}}
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['modelCache']:
            continue
        w = world[name]
        calls = lambda va, n: {int(i.op_str, 16) for i in gexe.md.disasm(gexe.at(va - 0x400000, n), va)
                               if i.mnemonic == 'call' and i.op_str.startswith('0x')}
        dup = calls(*w['dup'])
        for f in ('modelOpen', 'frameSetPos', 'missionCreateActor', 'gameAddTemp'):
            check(exe[f] in dup, f'{name} ACTOR_DUPLICATE calls {f} {exe[f]:#x}')
        dupOps = operands(gexe, *w['dup'])
        check(exe['modelCache'] in dupOps and exe['drvCreateFrame'] in dupOps and exe['i3dDriver'] in dupOps,
              f'{name} ACTOR_DUPLICATE: modelCache, the driver and its slot {exe["drvCreateFrame"]:#x}')
        cheat = calls(*w['cheat'])
        check(exe['sceneMoved'] in cheat and exe['vehicleCalm'] in cheat, f'{name} the cheat car calls sceneMoved and vehicleCalm')
        check(any(exe['carParticles'] in operands(gexe, c, 0x10) for c in cheat), f'{name} the cheat car sets carParticles {exe["carParticles"]:#x}')
        check(exe['gameRemoveTemp'] in calls(w['delete'], 0x12), f'{name} ACTOR_DELETE calls gameRemoveTemp')
        check(b'\xc2\x18\x00' in gexe.at(exe['modelOpen'] - 0x400000, 0x480), f'{name} modelOpen returns with ret 0x18')
        code, n = bytes(gexe.mem), 0
        at = code.find(b'\xb9' + exe['carDb'].to_bytes(4, 'little'))
        while at >= 0 and n < 2:
            n += any(i.mnemonic == 'call' and i.op_str == hex(w['carName']) for i in gexe.md.disasm(code[at:at + 16], 0x400000 + at))
            at = code.find(b'\xb9' + exe['carDb'].to_bytes(4, 'little'), at + 1)
        check(n >= 1, f'{name} carDb {exe["carDb"]:#x} is ECX of GetCarName')
        d = operands(gexe, w['tables'], 0x90)
        check(exe['items'] in d and exe['items'] + 4 in d, f'{name} items {exe["items"]:#x} and its count written by the tables\' loader')
        # the items' memory comes from gameNew (the loader's fifth call: dtaOpen, delete, the size's seek, the seek back,
        # new), which pushes 1 and the size and calls the CRT's allocator (the new handler's mode)
        tc = [int(i.op_str, 16) for i in gexe.md.disasm(gexe.at(w['tables'] - 0x400000, 0x60), w['tables']) if i.mnemonic == 'call']
        nw = [f'{i.mnemonic} {i.op_str}' for i in gexe.md.disasm(gexe.at(exe['gameNew'] - 0x400000, 8), exe['gameNew'])]
        check(len(tc) >= 5 and tc[4] == exe['gameNew'] and tc[2] == tc[3] and nw[:2] == ['push 1', 'push dword ptr [esp + 8]'],   # (items.cpp)
              f'{name} gameNew {exe["gameNew"]:#x}: the tables\' loader allocates the items with it')
    for f in ('modelOpen', 'missionCreateActor', 'gameAddTemp', 'gameRemoveTemp', 'vehicleCalm'):
        a, b = norm(g12.insn(e12[f] - 0x400000)), norm(g10.insn(e10[f] - 0x400000))
        check(a == b, f'{f}: 1.2 "{a}" vs 1.0 "{b}"')
    # The language (both builds): the loader of tables\lang_<ll>.def reads its 0x238-byte record to menuLang - 4
    # (the letters at +4; host GameCodePage and the definitions' loader read them).
    lang_loaders = {'1.2': 0x5F7000, '1.0': 0x549060}
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['menuLang']:
            continue
        body = gexe.at(lang_loaders[name] - 0x400000, 0xC0)
        check(b'\x68\x38\x02\x00\x00\x68' + (exe['menuLang'] - 4).to_bytes(4, 'little') in body,
              f'{name} menuLang {exe["menuLang"]:#x}: the language file read to it - 4')
    # LS3D's mouse read (both builds): GetDeviceState(16, mouseState) in it, ret 4
    for name, (exe, ls, gexe, gls) in builds.items():
        if not ls['mouseUpdate']:
            continue
        body = gls.at(ls['mouseUpdate'], 0x40)
        check(b'\x68' + (gls.base + ls['mouseState']).to_bytes(4, 'little') + b'\x6a\x10' in body, f'{name} LS3DF mouseState pushed with 16 in mouseUpdate')
        check(b'\xc2\x04\x00' in body, f'{name} LS3DF mouseUpdate returns with ret 4')
    # A model's animation and its physics (host/props.cpp): MODEL_PLAYANIM opens the animation with ECX = animCache and
    # calls missionAddAnim, MODEL_STOPANIM missionDelAnim; CREATE_PHYSICALOBJECT news physSize for physCtor, then
    # InitBox, gameAddTemp and the kick's physImpulse. The model's vtable +0x60/+0x64/+0x68 and its channels at
    # +0x150 (LS3DF): SetAnimation and the channel reader read +0x150, 0x1C bytes a channel.
    handlers = {'1.2': (0x471E90, 0x471F41, 0x4741D5), '1.0': (0x5BFC80, 0x5BFD31, 0x5C1FC5)}
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['animOpen']:
            continue
        play, stop, phys = handlers[name]
        ins = lambda va, n: list(gexe.md.disasm(gexe.at(va - 0x400000, n), va))
        calls = lambda l: [int(i.op_str, 16) for i in l if i.mnemonic == 'call' and i.op_str.startswith('0x')]
        p = ins(play, 0xB0)
        k = next((k for k, i in enumerate(p) if i.mnemonic == 'call' and i.op_str == hex(exe['animOpen'])), -1)
        check(k > 0 and f'mov ecx, {exe["animCache"]:#x}' in [f'{i.mnemonic} {i.op_str}' for i in p[k - 3:k]],
              f'{name} animOpen called on animCache by MODEL_PLAYANIM')
        check(exe['missionAddAnim'] in calls(p) and 'call dword ptr [ecx + 0x60]' in [f'{i.mnemonic} {i.op_str}' for i in p],
              f'{name} MODEL_PLAYANIM: SetAnimation (+0x60) and missionAddAnim')
        s = ins(stop, 0x68)
        check(exe['missionDelAnim'] in calls(s) and 'call dword ptr [ecx + 0x68]' in [f'{i.mnemonic} {i.op_str}' for i in s],
              f'{name} MODEL_STOPANIM: StopAnimation (+0x68) and missionDelAnim')
        c = ins(phys, 0x190)
        t = [f'{i.mnemonic} {i.op_str}' for i in c]
        cc = calls(c)
        check(f'push {exe["physSize"]:#x}' in t and exe['gameNew'] in cc and exe['physCtor'] in cc and exe['physInitBox'] in cc and
              exe['gameAddTemp'] in cc and exe['physImpulse'] in cc,
              f'{name} CREATE_PHYSICALOBJECT: gameNew({exe["physSize"]:#x}), physCtor, physInitBox, gameAddTemp, physImpulse')
        for f, ret in (('physInitBox', '4'), ('physImpulse', '8'), ('missionAddAnim', '8'), ('missionDelAnim', '4'), ('animOpen', '0x10')):
            first = next((i for i in gexe.md.disasm(gexe.at(exe[f] - 0x400000, 0x3000), exe[f]) if i.mnemonic == 'ret'), None)
            check(first is not None and first.op_str == ret, f'{name} {f}: its first ret is ret {ret}')
    for f in ('animOpen', 'missionAddAnim', 'missionDelAnim', 'physCtor', 'physImpulse'):   # (InitBox's prologue is scheduled otherwise in 1.0)
        a, b = norm(g12.insn(e12[f] - 0x400000)), norm(g10.insn(e10[f] - 0x400000))
        check(a == b, f'{f}: 1.2 "{a}" vs 1.0 "{b}"')
    # The game's camera (host/gamecam.cpp): the mode table's 17 records ("P Behind" first, "C Racing" last), scanned by
    # G_Camera::Begin from its start; SetMode returns with ret 8 and calls Begin.
    begins = {'1.2': 0x5F2EA0, '1.0': 0x5DA4B0}
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['cameraSetMode']:
            continue
        recs = gexe.at(exe['cameraModeTable'] - 0x400000, 17 * 0x44)
        names = [recs[i * 0x44 + 0x30:i * 0x44 + 0x40].split(b'\0')[0] for i in range(17)]
        check(names[0] == b'P Behind' and names[7] == b'C Chase' and names[16] == b'C Racing',
              f'{name} cameraModeTable: 17 records of 0x44, "P Behind" .. "C Racing"')
        check(exe['cameraModeTable'].to_bytes(4, 'little') in gexe.at(begins[name] - 0x400000, 0x60),
              f'{name} G_Camera::Begin scans cameraModeTable')
        body = list(gexe.md.disasm(gexe.at(exe['cameraSetMode'] - 0x400000, 0x80), exe['cameraSetMode']))
        ret = next((i for i in body if i.mnemonic == 'ret'), None)
        check(ret is not None and ret.op_str == '8' and any(i.mnemonic in ('call', 'jmp') and i.op_str == hex(begins[name]) for i in body),
              f'{name} cameraSetMode: ret 8, Begin called')
        # SetCar: its argument stored at +0x0C, Begin called, ret 4 (both of its returns)
        body = list(gexe.md.disasm(gexe.at(exe['cameraSetCar'] - 0x400000, 0x49), exe['cameraSetCar']))
        t = [f'{i.mnemonic} {i.op_str}' for i in body]
        check('mov dword ptr [esi + 0xc], eax' in t and t.count('ret 4') == 2 and
              sum(1 for i in body if i.mnemonic == 'call' and i.op_str == hex(begins[name])) == 2,
              f'{name} cameraSetCar: the car at +0x0C, Begin called, ret 4')
    a, b = norm(g12.insn(e12['cameraSetMode'] - 0x400000)), norm(g10.insn(e10['cameraSetMode'] - 0x400000))
    check(a == b, f'cameraSetMode: 1.2 "{a}" vs 1.0 "{b}"')
    # The airplane (host/control.cpp): planeAI and planeUpdate are slots 13 and 14 of the vtable C_airplane's
    # constructor puts, each thiscall with ret 4; AI tests the flight state +0xB4 first; both touch the same fields in
    # both builds (game.h kPlaneState, kPlaneSpeed, kPlaneRot).
    ctors = {'1.2': 0x409390, '1.0': 0x44EF50}
    fields = {}
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe['planeAI']:
            continue
        vt = None
        for i in gexe.md.disasm(gexe.at(ctors[name] - 0x400000, 0x170), ctors[name]):
            m = re.match(r'dword ptr \[esi\], (0x[0-9a-f]+)$', i.op_str)
            if i.mnemonic == 'mov' and m:
                vt = int(m.group(1), 16)
            if i.mnemonic == 'ret':
                break
        slots = [int.from_bytes(gexe.at(vt - 0x400000 + s * 4, 4), 'little') for s in (13, 14)] if vt else []
        check(slots == [exe['planeAI'], exe['planeUpdate']], f'{name} planeAI, planeUpdate: slots 13 and 14 of the airplane\'s vtable')
        for f, n in (('planeAI', 0x2B0), ('planeUpdate', 0x670)):
            body = list(gexe.md.disasm(gexe.at(exe[f] - 0x400000, n), exe[f]))
            ret = next((i for i in body if i.mnemonic == 'ret'), None)
            check(ret is not None and ret.op_str == '4', f'{name} {f}: ret 4')
            seen = set()
            for i in body[:body.index(ret) + 1] if ret else body:
                seen.update(int(m.group(1), 16) for m in re.finditer(r'\[e[a-z]{2} \+ (0x[0-9a-f]+)\]', i.op_str))
            fields.setdefault(f, []).append(seen)
            if f == 'planeAI':
                check(any(i.op_str == 'dword ptr [esi + 0xb4], ebp' for i in body[:10]), f'{name} planeAI tests the flight state +0xB4 first')
        check({0xB4, 0xB8, 0xD0, 0x24}.issubset(fields['planeAI'][-1]) and {0xB4, 0xD0, 0x68}.issubset(fields['planeUpdate'][-1]),
              f'{name} the airplane\'s fields: +0x24, +0xB4, +0xB8, +0xD0 (AI), +0x68, +0xB4, +0xD0 (Update)')
    if all(len(v) == 2 for v in fields.values()) and fields:
        check(all(v[0] == v[1] for v in fields.values()), 'the airplane: AI and Update touch the same fields in both builds')
    # Orders (host/orders.cpp): entityForceScript is C_entity::ForceScript: it reads the AI state +0xF40, raises the
    # event 0xEA63 on the program at +0xA9C, and ends with ret 4; the same instructions in both builds.
    shapes = []
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe.get('entityForceScript'):
            continue
        body = list(gexe.md.disasm(gexe.at(exe['entityForceScript'] - 0x400000, 0xF0), exe['entityForceScript']))
        ret = next((i for i in body if i.mnemonic == 'ret'), None)
        text = ' | '.join('%s %s' % (i.mnemonic, i.op_str) for i in body)
        check(ret is not None and ret.op_str == '4', f'{name} entityForceScript: ret 4')
        check('0xea63' in text and '0xa9c' in text and '0xf40' in text, f'{name} entityForceScript: +0xF40, the event 0xEA63 on +0xA9C')
        shapes.append([norm(gexe.insn(i.address - 0x400000)) for i in body[:12]])
    if len(shapes) == 2:
        check(shapes[0] == shapes[1], 'entityForceScript: the same first instructions in both builds')

    # roadsNearNode is C_roads::GetNearNode: the nodes' vector at +0x18 / +0x1C, divided by 0x18 (0x2aaaaaab, sar 2),
    # ret 0xC; the same first instructions in both builds.
    shapes = []
    for name, (exe, ls, gexe, gls) in builds.items():
        if not exe.get('roadsNearNode'):
            continue
        body = list(gexe.md.disasm(gexe.at(exe['roadsNearNode'] - 0x400000, 0x210), exe['roadsNearNode']))
        text = ' | '.join('%s %s' % (i.mnemonic, i.op_str) for i in body)
        check('[ecx + 0x18]' in text and '[ecx + 0x1c]' in text and '0x2aaaaaab' in text,
              f'{name} roadsNearNode: the nodes at +0x18 / +0x1C, 0x18 bytes each')
        check(any(i.mnemonic == 'ret' and i.op_str == '0xc' for i in body), f'{name} roadsNearNode: ret 0xC')
        shapes.append([norm(gexe.insn(i.address - 0x400000)) for i in body[:12]])
    if len(shapes) == 2:
        check(shapes[0] == shapes[1], 'roadsNearNode: the same first instructions in both builds')
    print('ALL PASSED' if not fails else f'{fails} FAILED')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
