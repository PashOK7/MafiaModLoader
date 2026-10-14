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
    print('ALL PASSED' if not fails else f'{fails} FAILED')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
