"""kb_structs.py: the game's structures by name (classes, their fields, their vtables) into knowledge/structs/.

    python tools/kb_structs.py [--game12 <1.2 Game.exe>] [--game10 <1.0 Game.exe>] [--ext re/ext]

The Game.exe files default to the labs of mml.config.json. Every row says where it comes from (`source`):

  game.h, game_build.h   the SDK's own fields (what the loader's code reads and writes, seen in both builds' games),
                         named once in SDK_FIELDS below; their offsets are read from the headers themselves, so the
                         two never part (game_build.h's per-build tables give each build its own)
  code                   read from both builds' code here: a class's vtable, its actor kind, its base class and the
                         classes embedded in it, from its constructor (1.2's named C_x::C_x and its 1.0 twin), its size
                         from the `new` before the constructor's calls
  st0rm94                the fields of st0rm94's structs/*.md (1.0, the PC build he reversed), carried to 1.2 through
                         the code: every memory access of the class's functions is aligned with its twin's in the
                         other build (twins_10.csv), and a field's 1.2 offset is the one its 1.0 offset's accesses
                         agree on (most of them, and at least two); a field with no such agreement keeps no 1.2 offset
  anything else          rows written by hand (`mml`: found in the running game); kept as they are

Writes knowledge/structs/structs.csv (struct,base,size,size_10,vtable,vtable_10,kind,source,note) and
knowledge/structs/fields.csv (struct,offset,offset_10,name,type,source,note); offsets are each build's (empty: not
known in that build). Types: u8 i8 u16 i16 u32 i32 f32 f64 bool, ptr, ptr:<struct>, str (a char*), chars (inline
text), <struct> (embedded), each with [n] for an array.
"""
import argparse
import csv
import difflib
import json
import os
import re
import sys
from collections import Counter, defaultdict

import capstone
from capstone import x86 as X

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_builds   # noqa: E402  (game_build.h's tables)
from kb_external import Image, st0rm94_functions   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KB = os.path.join(ROOT, 'knowledge')
OUT = os.path.join(KB, 'structs')
GENERATED = {'game.h', 'game_build.h', 'code', 'st0rm94'}

# The SDK's fields: a constant of game.h (both builds) or a member of game_build.h's ExeBuild (each build's value)
# -> (struct, field, type). The comments beside them in the headers say how each was found.
SDK_FIELDS = {
    # game.h
    'kActorType': ('C_actor', 'kind', 'i32'), 'kActorPos': ('C_actor', 'position', 'f32[3]'),
    'kActorFacing': ('C_actor', 'facing', 'f32[3]'), 'kActorAlive': ('C_actor', 'alive', 'u8'),
    'kActorDead': ('C_actor', 'dead', 'u8'), 'kActorFrame': ('C_actor', 'frame', 'ptr:I3D_frame'),
    'kHumanVehicle': ('C_human', 'vehicle', 'ptr'), 'kHumanInventory': ('C_human', 'inventory', 'G_Inventory'),
    'kHumanFallSpeed': ('C_human', 'fall_speed', 'f32'), 'kHumanHealthMax': ('C_human', 'health_max', 'f32'),
    'kHumanHealth': ('C_human', 'health', 'f32'),
    'kFrameWorld': ('I3D_frame', 'world', 'f32[16]'), 'kFrameLocal': ('I3D_frame', 'local', 'f32[16]'),
    'kFrameScale': ('I3D_frame', 'scale', 'f32[3]'), 'kFrameRot': ('I3D_frame', 'rotation', 'f32[4]'),
    'kFrameFlags': ('I3D_frame', 'flags', 'u32'), 'kFrameName': ('I3D_frame', 'name', 'str'),
    'kFrameType': ('I3D_frame', 'type', 'u32'), 'kFrameParent': ('I3D_frame', 'parent', 'ptr:I3D_frame'),
    'kModelFile': ('I3D_frame', 'model_file', 'str'),
    'kMissionScene': ('C_mission', 'scene', 'ptr:I3D_scene'), 'kMissionGame': ('C_mission', 'game', 'ptr:C_game'),
    'kGamePlayer': ('C_game', 'player', 'ptr:C_player'), 'kGameScore': ('C_game', 'score', 'i32'),
    'kGameScoreShown': ('C_game', 'score_shown', 'u8'), 'kGameShots': ('C_game', 'shots', 'ptr:S_shot'),
    'kShotFrom': ('S_shot', 'from', 'f32[3]'), 'kShotWay': ('S_shot', 'way', 'f32[3]'),
    'kShotNow': ('S_shot', 'now', 'f32[3]'), 'kShotDamage': ('S_shot', 'damage', 'f32'),
    'kShotShooter': ('S_shot', 'shooter', 'ptr'), 'kShotPrim': ('S_shot', 'prim', 'ptr'),
    'kShotHitPos': ('S_shot', 'hit_pos', 'f32[3]'), 'kShotHitNormal': ('S_shot', 'hit_normal', 'f32[3]'),
    'kShotHits': ('S_shot', 'hits', 'u8'), 'kShotSurface': ('S_shot', 'surface', 'u8'),
    'kPlaneState': ('C_airplane', 'flight_state', 'i32'), 'kPlaneSpeed': ('C_airplane', 'speed', 'f32'),
    'kPlaneRot': ('C_airplane', 'rotation', 'f32[4]'),
    'kWheelFrame': ('S_wheel', 'frame', 'ptr:I3D_frame'), 'kWheelSteerFrame': ('S_wheel', 'steer_frame', 'ptr:I3D_frame'),
    'kWheelLoad': ('S_wheel', 'load', 'f32'), 'kWheelStiffness': ('S_wheel', 'stiffness', 'f32'),
    'kWheelFreeLength': ('S_wheel', 'free_length', 'f32'), 'kWheelForce': ('S_wheel', 'force', 'f32'),
    'kWheelCompression': ('S_wheel', 'compression', 'f32'), 'kWheelDamper': ('S_wheel', 'damper', 'f32[2]'),
    'kWheelFlags': ('S_wheel', 'flags', 'u32'),
    # game_build.h (ExeBuild)
    'vehicle': ('C_car', 'vehicle', 'C_Vehicle'), 'carInWater': ('C_car', 'in_water', 'u8'),
    'carParticles': ('C_car', 'particles', 'u8'),
    'vehCentre': ('C_Vehicle', 'centre', 'f32[3]'), 'vehTurn': ('C_Vehicle', 'angular_velocity', 'f32[3]'),
    'vehMass': ('C_Vehicle', 'mass', 'f32'), 'vehVelocity': ('C_Vehicle', 'velocity', 'f32[3]'),
    # (vehInertiaA, vehInertiaB are left out: what GetRVelMassJ reads there is not the same thing in both builds;
    # 1.0's are the body's matrix and its inverse, vehMatrix's offsets, where 1.2 reads the pair after them)
    'vehBalance': ('C_Vehicle', 'balance', 'f32[3]'), 'vehPos': ('C_Vehicle', 'position', 'f32[3]'),
    'vehSpeed': ('C_Vehicle', 'speed', 'f32'), 'vehSteerWanted': ('C_Vehicle', 'steer_wanted', 'f32'),
    'vehSteer': ('C_Vehicle', 'steer', 'f32'), 'vehSteerMax': ('C_Vehicle', 'steer_max', 'f32'),
    'vehGear': ('C_Vehicle', 'gear', 'i32'), 'vehGearMax': ('C_Vehicle', 'gear_max', 'i32'),
    'vehRps': ('C_Vehicle', 'engine_rps', 'f32'), 'vehRpsMax': ('C_Vehicle', 'engine_rps_max', 'f32'),
    'vehFuel': ('C_Vehicle', 'fuel', 'f32'), 'vehFuelTank': ('C_Vehicle', 'fuel_tank', 'f32'),
    'vehEngineOn': ('C_Vehicle', 'engine_on', 'u8'), 'vehEngineHealth': ('C_Vehicle', 'engine_health', 'f32'),
    'vehEngineHealthMax': ('C_Vehicle', 'engine_health_max', 'f32'), 'vehicleFlags': ('C_Vehicle', 'flags', 'u32'),
    'vehicleStepTime': ('C_Vehicle', 'step_time', 'f32'), 'vehSubstepMax': ('C_Vehicle', 'substep_max', 'f32'),
    'vehAxes': ('C_Vehicle', 'axes', 'f32[9]'), 'vehWheels': ('C_Vehicle', 'wheels', 'ptr'),
    'vehWheelCount': ('C_Vehicle', 'wheel_count', 'i32'), 'vehState': ('C_Vehicle', 'state', 'u32'),
    'vehExMaxSpeed': ('C_Vehicle', 'exhaust_max_speed', 'f32'), 'vehExType': ('C_Vehicle', 'exhaust_type', 'i32'),
    'vehExPipes': ('C_Vehicle', 'exhaust_pipes', 'ptr'), 'vehExJet': ('C_Vehicle', 'exhaust_jet', 'f32[2]'),
    'vehMatrix': ('C_Vehicle', 'matrix', 'f32[16]'), 'vehMatrixPrev': ('C_Vehicle', 'matrix_prev', 'f32[16]'),
    'humanColl': ('C_human', 'collisions', 'ptr'), 'humanCollCount': ('C_human', 'collision_count', 'i32'),
    'humanParts': ('C_human', 'parts', 'u8'), 'humanActs': ('C_human', 'actions', 'u8'),
    'humanActIndex': ('C_human', 'action_index', 'i32'), 'humanState': ('C_human', 'state', 'i32'),
    'humanAnimMachine': ('C_human', 'anim_machine', 'C_animations_machine_de_luxe'),
    'missionName': ('C_mission', 'name', 'chars[32]'), 'gameCamera': ('C_game', 'camera', 'G_Camera'),
}
# The SDK's sizes (game_build.h): struct -> member.
SDK_SIZES = {'C_car': 'carSize', 'C_public_physics': 'physSize'}
# Fields game.h tells in its comments only (both builds alike; how each was found is said there).
SDK_EXTRA = [
    ('G_Inventory', 0x08, 'extra_count', 'u32', 'game.h kHumanInventory'),
    ('G_Inventory', 0x20, 'in_hand', 'S_GameItem', 'game.h kHumanInventory'),
    ('G_Inventory', 0x30, 'belt', 'S_GameItem[5]', 'game.h kHumanInventory'),
    ('G_Inventory', 0x80, 'coat', 'S_GameItem', 'game.h kHumanInventory'),
    ('G_Inventory', 0x90, 'extra', 'S_GameItem[4]', 'game.h kHumanInventory'),
    ('G_Inventory', 0xD0, 'put_away', 'S_GameItem', 'game.h kHumanInventory'),
    ('S_GameItem', 0x00, 'id', 'u16', 'game.h kHumanInventory'),
    ('S_GameItem', 0x04, 'loaded', 'u32', 'game.h kHumanInventory'),
    ('S_GameItem', 0x08, 'reserve', 'u32', 'game.h kHumanInventory'),
    ('G_Camera', 0x04, 'camera', 'ptr:I3D_camera', 'game_build.h gameCamera'),
    ('G_Camera', 0x0C, 'car', 'ptr:C_car', 'game_build.h gameCamera'),
    ('G_Camera', 0x10, 'mode', 'i32', 'game_build.h gameCamera'),
]
SDK_STRUCTS = {'S_GameItem': 0x10, 'G_Inventory': 0xE0, 'S_shot': 0x5C, 'S_wheel': 0x1D4}


def config_labs():
    try:
        cfg = json.load(open(os.path.join(ROOT, 'mml.config.json'), encoding='utf-8'))
    except OSError:
        return {}
    return cfg.get('labs') or {}


def hx(v):
    return '' if v is None else '0x%X' % v


# ---- the SDK's rows
def sdk_rows():
    hdr = os.path.join(ROOT, 'sdk', 'include', 'mml', 'game')
    gh = open(os.path.join(hdr, 'game.h'), encoding='utf-8').read()
    consts = {}
    for m in re.finditer(r'constexpr\s+int\s+([^;]+);', gh):
        for part in m.group(1).split(','):
            k, _, v = part.partition('=')
            v = v.strip()
            if re.fullmatch(r'0x[0-9A-Fa-f]+|\d+', v):
                consts[k.strip()] = int(v, 0)
    gb = open(os.path.join(hdr, 'game_build.h'), encoding='utf-8').read()
    ef = check_builds.struct_fields(gb, 'ExeBuild')
    t12, t10 = check_builds.table(gb, 'kExe12', ef), check_builds.table(gb, 'kExe10', ef)
    fields, missing = [], []
    for k, (s, name, typ) in SDK_FIELDS.items():
        if k in consts:
            fields.append([s, consts[k], consts[k], name, typ, 'game.h', k])
        elif k in t12:
            a, b = t12[k] or None, t10[k] or None
            if a is None and b is None:
                continue
            fields.append([s, a, b, name, typ, 'game_build.h', k])
        else:
            missing.append(k)
    if missing:
        sys.exit('SDK_FIELDS names what the headers no longer have: ' + ', '.join(missing))
    for s, off, name, typ, note in SDK_EXTRA:
        fields.append([s, off, off, name, typ, 'game.h', note])
    sizes = {s: (t12[m] or None, t10[m] or None) for s, m in SDK_SIZES.items()}
    sizes.update({s: (n, n) for s, n in SDK_STRUCTS.items()})
    return fields, sizes


# ---- st0rm94's documents
CTYPES = {'float': 'f32', 'double': 'f64', 'uint8_t': 'u8', 'unsigned char': 'u8', 'byte': 'u8', 'BYTE': 'u8',
          'uchar': 'u8', 'int8_t': 'i8', 'char': 'i8', 'signed char': 'i8', 'uint16_t': 'u16', 'unsigned short': 'u16',
          'ushort': 'u16', 'WORD': 'u16', 'int16_t': 'i16', 'short': 'i16', 'uint32_t': 'u32', 'unsigned': 'u32',
          'unsigned int': 'u32', 'uint': 'u32', 'DWORD': 'u32', 'unsigned long': 'u32', 'int32_t': 'i32', 'int': 'i32',
          'long': 'i32', 'BOOL': 'i32', 'bool': 'bool'}
# LS3D's vectors and matrices: floats (I3D's own headers, mEdit's LS3D.h).
VECTORS = {'S_vector': ('f32', 3), 'S_vector2': ('f32', 2), 'S_vector4': ('f32', 4), 'S_quat': ('f32', 4),
           'S_matrix': ('f32', 16), 'S_plane': ('f32', 4)}
FIELD = re.compile(r'^\s*/\*\s*\+0x([0-9A-Fa-f]+)\s*\*/\s*([^;]+?)\s*;\s*(?://\s*(.*))?$')


def c_type(decl):
    """'float pos[3]' -> ('pos', 'f32[3]'); None when it is not a plain declaration."""
    decl = re.sub(r'\b(const|volatile|struct|unsigned(?=\s+(?:char|short|int|long)\b))\b', lambda m: '' if m.group(1) != 'unsigned' else 'unsigned', decl)
    if '(' in decl:   # a function pointer
        m = re.search(r'\(\s*\*\s*([A-Za-z_]\w*)\s*\)', decl)
        return (m.group(1), 'ptr') if m else None
    if ':' in decl:   # a bit field
        return None
    m = re.match(r'^(.*?)(\**)\s*([A-Za-z_]\w*)\s*((?:\[[^\]]*\]\s*)*)$', decl.strip())
    if not m:
        return None
    base, stars, name, dims = ' '.join(m.group(1).split()), m.group(2), m.group(3), m.group(4)
    if base.endswith('*'):
        stars += '*' * (len(base) - len(base.rstrip('*')))
        base = base.rstrip('* ').strip()
    if not base:
        return None
    n = 1
    for d in re.findall(r'\[([^\]]*)\]', dims):
        d = d.strip()
        n *= int(d, 0) if re.fullmatch(r'0x[0-9A-Fa-f]+|\d+', d) else 1
    if stars:
        t = 'str' if base == 'char' and len(stars) == 1 else 'ptr' if base in CTYPES or base == 'void' or len(stars) > 1 else 'ptr:' + base
    elif base == 'char' and dims:
        return name, 'chars[%d]' % n
    elif base in VECTORS:
        t, k = VECTORS[base]
        return name, '%s[%d]' % (t, k * n)
    else:
        t = CTYPES.get(base, base)
    return name, t + ('[%d]' % n if dims and n > 1 else '')


def st0rm94_docs(ext):
    """[{struct, base, prefix, tu, fields: [(off10, name, type, note)]}] from structs/*.md's C blocks."""
    out = []
    d = os.path.join(ext, 'mafia-reverse-engineering-export', 'structs')
    if not os.path.isdir(d):
        return out
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.md'):
            continue
        text = open(os.path.join(d, fn), encoding='utf-8').read()
        title = re.search(r'^#\s+`([A-Za-z_]\w*)`', text, re.M)
        prefix = re.search(r'^\| PC prefix(?:es)? \|[^`\n]*`([A-Za-z_]\w*?)_?\*?`', text, re.M)
        tu = re.search(r'^\| PC translation unit \|[^\n]*?\[0x([0-9a-fA-F]+),\s*0x([0-9a-fA-F]+)\)', text, re.M)
        for block in re.findall(r'```c\n(.*?)```', text, re.S):
            lines = block.split('\n')
            head = next((l for l in lines if 'typedef struct' in l or re.match(r'\s*struct\s+\w+\s*\{', l)), None)
            if head is None:
                continue
            name = re.search(r'struct\s+([A-Za-z_]\w*)\s*\{', head)
            tail = re.search(r'\}\s*([A-Za-z_]\w*)\s*;', block)
            struct = name.group(1) if name else tail.group(1) if tail else None
            if not struct:
                continue
            base = re.search(r'extends\s+`?([A-Za-z_]\w*)', head)
            fields = []
            for l in lines:
                m = FIELD.match(l)
                if not m:
                    continue
                ct = c_type(m.group(2))
                if not ct:
                    continue
                note = ' '.join((m.group(3) or '').split())
                note = re.split(r'(?<=[.;])\s', note)[0][:140].rstrip(';,. ')
                fields.append((int(m.group(1), 16), ct[0], ct[1], note))
            if fields:
                main = title and title.group(1) == struct
                out.append({'struct': struct, 'base': base.group(1) if base else None, 'doc': fn,
                            'prefix': (prefix.group(1).rstrip('_') + '_') if prefix and main else None,
                            'tu': (int(tu.group(1), 16), int(tu.group(2), 16)) if tu and main else None,
                            'fields': fields})
    return out


# ---- the code of both builds
class Code:
    def __init__(self, path, starts):
        self.img = Image(path)
        self.starts = sorted(starts)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.cache = {}

    def end_of(self, a):
        import bisect
        i = bisect.bisect_right(self.starts, a)
        return self.starts[i] if i < len(self.starts) else a + 0x400

    def insns(self, a):
        if a not in self.cache:
            end = min(self.end_of(a), a + 0x6000)
            self.cache[a] = [i for i in self.md.disasm(self.img.at(a, end - a), a) if i.address < end]
        return self.cache[a]

    def rdata(self, va):
        return self.img.is_data(va)

    def dword(self, va):
        b = self.img.at(va, 4)
        return int.from_bytes(b, 'little') if len(b) == 4 else None


def token(i):
    return i.mnemonic + ' ' + re.sub(r'0x[0-9a-f]+|\b\d+\b', 'N', i.op_str)


def plain_mem(op):
    return (op.type == X.X86_OP_MEM and op.mem.index == 0 and op.mem.segment == 0
            and op.mem.base not in (0, X.X86_REG_ESP, X.X86_REG_EBP))


def votes_of(c10, c12, pairs):
    """{disp10: Counter(disp12)} over the aligned memory accesses of the function pairs (a10, a12)."""
    votes = defaultdict(Counter)
    for a10, a12 in pairs:
        i10, i12 = c10.insns(a10), c12.insns(a12)
        sm = difflib.SequenceMatcher(None, [token(i) for i in i10], [token(i) for i in i12], autojunk=False)
        for tag, x1, x2, y1, y2 in sm.get_opcodes():
            if tag != 'equal':
                continue
            for a, b in zip(i10[x1:x2], i12[y1:y2]):
                ma = [o for o in a.operands if o.type == X.X86_OP_MEM]
                mb = [o for o in b.operands if o.type == X.X86_OP_MEM]
                for p, q in zip(ma, mb):
                    if plain_mem(p) and plain_mem(q) and 0 <= p.mem.disp < 0x10000 and 0 <= q.mem.disp < 0x10000:
                        votes[p.mem.disp][q.mem.disp] += 1
    return votes


def carry(votes, off10):
    """(the 1.2 offset most of off10's accesses agree on or None, the evidence, whether every access agrees)."""
    c = votes.get(off10)
    if not c:
        return None, '1.2 not carried: no access in the paired code', False
    (best, n), total = c.most_common(1)[0], sum(c.values())
    if n >= 2 and n >= 0.75 * total:
        return best, '1.2 carried: %d of %d accesses' % (n, total), n >= 4 and n == total
    return None, '1.2 not carried: accesses disagree %s' % dict((hx(k), v) for k, v in c.most_common(3)), False


REG32 = {}
for r32, subs in ((X.X86_REG_EAX, ('AL', 'AH', 'AX')), (X.X86_REG_EBX, ('BL', 'BH', 'BX')), (X.X86_REG_ECX, ('CL', 'CH', 'CX')),
                  (X.X86_REG_EDX, ('DL', 'DH', 'DX')), (X.X86_REG_ESI, ('SI', 'SIL')), (X.X86_REG_EDI, ('DI', 'DIL')), (X.X86_REG_EBP, ('BP', 'BPL'))):
    REG32[r32] = r32
    for s in subs:
        REG32[getattr(X, 'X86_REG_' + s)] = r32


def ctor_facts(code, start, ctors):
    """What a constructor does to its object: the classes' constructors it calls on it ([(offset, class)]: offset 0 is
    its base) and the dwords it stores into it ([(offset, value)]), following which registers hold `this` (+n)."""
    regs, calls, stores = {X.X86_REG_ECX: 0}, [], []
    for i in code.insns(start):
        ops = i.operands
        if i.mnemonic == 'ret':
            break
        if i.mnemonic == 'call':
            if ops and ops[0].type == X.X86_OP_IMM and ops[0].imm in ctors and regs.get(X.X86_REG_ECX) is not None:
                calls.append((regs[X.X86_REG_ECX], ctors[ops[0].imm]))
                ret = regs[X.X86_REG_ECX]   # a constructor returns its `this`
            else:
                ret = None
            for r in (X.X86_REG_EAX, X.X86_REG_ECX, X.X86_REG_EDX):
                regs.pop(r, None)
            if ret is not None:
                regs[X.X86_REG_EAX] = ret
            continue
        if i.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG:
            d, s = REG32.get(ops[0].reg), REG32.get(ops[1].reg)
            if d and ops[0].size == 4:
                if s in regs:
                    regs[d] = regs[s]
                else:
                    regs.pop(d, None)
                continue
        if (i.mnemonic == 'lea' and REG32.get(ops[0].reg) and ops[1].mem.index == 0
                and REG32.get(ops[1].mem.base) in regs):
            regs[REG32[ops[0].reg]] = regs[REG32[ops[1].mem.base]] + ops[1].mem.disp
            continue
        if (i.mnemonic == 'mov' and len(ops) == 2 and ops[0].type == X.X86_OP_MEM and ops[0].size == 4
                and ops[1].type == X.X86_OP_IMM and ops[0].mem.index == 0 and REG32.get(ops[0].mem.base) in regs):
            stores.append((regs[REG32[ops[0].mem.base]] + ops[0].mem.disp, ops[1].imm & 0xFFFFFFFF))
        _, written = i.regs_access()
        for r in written:
            if REG32.get(r) in regs:
                regs.pop(REG32[r])
    return calls, stores


def vtable_slots(code, vt, limit=200):
    out = []
    while len(out) < limit:
        f = code.dword(vt + 4 * len(out))
        if f is None or not code.img.is_code(f):
            break
        out.append(f)
    return out


def sizes_from_callers(code, ctor, callers):
    """The size `new` was given before each call of the constructor: the push right before a call, a few
    instructions before it."""
    found = Counter()
    for c in callers:
        ins = code.insns(c)
        for k, i in enumerate(ins):
            if i.mnemonic == 'call' and i.operands and i.operands[0].type == X.X86_OP_IMM and i.operands[0].imm == ctor:
                for j in range(k - 1, max(k - 14, 1), -1):
                    a, b = ins[j - 1], ins[j]
                    if b.mnemonic == 'call' and a.mnemonic == 'push' and a.operands[0].type == X.X86_OP_IMM:
                        found[a.operands[0].imm] += 1
                        break
    if not found:
        return None
    (v, n), total = found.most_common(1)[0], sum(found.values())
    return v if n == total else None


def main():
    ap = argparse.ArgumentParser()
    labs = config_labs()
    ap.add_argument('--game12', default=labs.get('1.2') and os.path.join(labs['1.2'], 'Game.exe'))
    ap.add_argument('--game10', default=labs.get('1.0') and os.path.join(labs['1.0'], 'Game.exe'))
    ap.add_argument('--ext', default=os.path.join(ROOT, 're', 'ext'))
    a = ap.parse_args()
    if not a.game12 or not a.game10:
        sys.exit('the 1.2 and 1.0 Game.exe: --game12, --game10 (or the labs in mml.config.json)')

    funcs12 = {int(r['address'], 16): (r['name'], r['class']) for r in csv.DictReader(open(os.path.join(KB, 'game.exe', 'functions.csv'), encoding='utf-8'))}
    names10 = st0rm94_functions(a.ext)
    if not names10:
        sys.exit('st0rm94\'s export is not in ' + a.ext)
    twin12, twin10 = {}, {}
    for r in csv.DictReader(open(os.path.join(KB, 'game.exe', 'twins_10.csv'), encoding='utf-8')):
        twin12[int(r['va10'], 16)] = int(r['va12'], 16)
        twin10[int(r['va12'], 16)] = int(r['va10'], 16)
    callers12 = defaultdict(list)
    for r in csv.reader(open(os.path.join(KB, 'game.exe', 'calls.csv'), encoding='utf-8')):
        if r and r[0] != 'caller':
            try:
                callers12[int(r[1], 16)].append(int(r[0], 16))
            except ValueError:
                pass
    c12, c10 = Code(a.game12, funcs12), Code(a.game10, names10)

    structs = {}   # name -> dict
    fields = []    # [struct, off12, off10, name, type, source, note]

    def S(name):
        return structs.setdefault(name, {'base': None, 'size': None, 'size_10': None, 'vtable': None, 'vtable_10': None,
                                         'kind': None, 'source': set(), 'note': []})

    # 1. the SDK's
    sdk_f, sdk_sizes = sdk_rows()
    for r in sdk_f:
        S(r[0])['source'].add(r[5])
    fields += sdk_f
    for s, (n12, n10) in sdk_sizes.items():
        S(s)['size'], S(s)['size_10'] = n12, n10
        S(s)['source'].add('game.h' if s in SDK_STRUCTS else 'game_build.h')

    # 2. the constructors: 1.2's named C_x::C_x, and their 1.0 twins
    ctors12 = {}
    for va, (name, cls) in funcs12.items():
        m = re.fullmatch(r'([A-Za-z_]\w*)::\1', name)
        if m and m.group(1) == cls:
            ctors12[va] = cls
    ctors10 = {twin10[va]: cls for va, cls in ctors12.items() if va in twin10}
    facts = {}
    for va, cls in ctors12.items():
        f12 = ctor_facts(c12, va, ctors12)
        f10 = ctor_facts(c10, twin10[va], ctors10) if va in twin10 else None
        facts[cls] = (va, f12, f10)
    vt_checked = {}
    for cls, (va, (calls12, st12), f10) in facts.items():
        s = S(cls)
        vt12 = next((v for o, v in reversed(st12) if o == 0 and c12.rdata(v)), None)
        vt10 = next((v for o, v in reversed(f10[1]) if o == 0 and c10.rdata(v)), None) if f10 else None
        if vt12 and vt10:
            # the same class in both builds: its slots' functions are twins (1.0's vtables of the actors have a slot
            # more, and a vtable's end is only where the next one starts: the slots are compared as sets)
            sl12, sl10 = set(vtable_slots(c12, vt12)), vtable_slots(c10, vt10)
            paired = [twin12[x] for x in sl10 if x in twin12]
            agree = sum(y in sl12 for y in paired)
            vt_checked[cls] = (agree, len(paired))
            if agree < max(1, 0.5 * len(paired)):
                s['note'].append('1.0 vtable not taken: %d of %d slots twins' % (agree, len(paired)))
                vt10 = None
        s['vtable'], s['vtable_10'] = vt12, vt10
        if vt12:
            s['source'].add('code')
        base12 = next((c for o, c in calls12 if o == 0), None)
        if base12:
            s['base'] = base12
        # the members: the classes' constructors called on this+n, paired with 1.0's in their order
        members12 = defaultdict(list)
        for o, c in calls12:
            if o > 0:
                members12[c].append(o)
        members10 = defaultdict(list)
        for o, c in (f10[0] if f10 else []):
            if o > 0:
                members10[c].append(o)
        for c, offs in members12.items():
            o10s = members10.get(c, [])
            for k, o in enumerate(offs):
                o10 = o10s[k] if len(o10s) == len(offs) else None
                name = (c[2:] if c.startswith(('C_', 'G_', 'S_')) else c).lower() + ('' if len(offs) == 1 else str(k))
                typ = '%s[%d]' % VECTORS[c] if c in VECTORS else c
                fields.append([cls, o, o10, name, typ, 'code', 'its constructor called on this+offset by ' + cls + "'s"])
        n12 = sizes_from_callers(c12, va, callers12.get(va, []))
        n10 = sizes_from_callers(c10, twin10[va], [twin10[x] for x in callers12.get(va, []) if x in twin10]) if va in twin10 else None
        if n12 and not s['size']:
            s['size'], s['size_10'] = n12, n10
    # an actor's kind: the dword its constructor stores at +0x10 last, for C_actor's line
    def actor_line(c, seen=()):
        while c and c not in seen:
            if c == 'C_actor':
                return True
            seen += (c,)
            c = structs.get(c, {}).get('base')
        return False
    for cls, (va, (calls12, st12), f10) in facts.items():
        if actor_line(cls):
            k12 = next((v for o, v in reversed(st12) if o == 0x10 and v < 0x100), None)
            k10 = next((v for o, v in reversed(f10[1]) if o == 0x10 and v < 0x100), None) if f10 else None
            if k12 is not None:
                S(cls)['kind'] = k12
                if k10 is not None and k10 != k12:
                    S(cls)['note'].append('1.0 kind 0x%X' % k10)

    # 3. st0rm94's fields, carried to 1.2 through the paired code
    docs = st0rm94_docs(a.ext)
    by_struct = defaultdict(list)
    for d in docs:
        by_struct[d['struct']].append(d)
    carried = Counter()
    sdk_at = defaultdict(set)
    for r in fields:
        if r[1] is not None:
            sdk_at[(r[0], 12)].add(r[1])
        if r[2] is not None:
            sdk_at[(r[0], 10)].add(r[2])
    doc_votes, cands = {}, []
    for d in docs:
        st = d['struct']
        f10 = set()
        if d['tu']:
            f10 |= {x for x in names10 if d['tu'][0] <= x < d['tu'][1]}
        if d['prefix']:
            f10 |= {x for x, (n, _) in names10.items() if n.startswith(d['prefix'])}
        pairs = {(x, twin12[x]) for x in f10 if x in twin12}
        pairs |= {(twin10[x], x) for x, (_, cls) in funcs12.items() if cls == st and x in twin10}
        key = (d['doc'], st)
        if key not in doc_votes:
            doc_votes[key] = votes_of(c10, c12, sorted(pairs))
        votes = doc_votes[key]
        s = S(st)
        s['source'].add('st0rm94')
        if d['base'] and not s['base']:
            s['base'] = d['base']
        for off10, name, typ, note in d['fields']:
            if off10 in sdk_at[(st, 10)]:
                carried['under the SDK\'s'] += 1
                for r in fields:   # the SDK's row keeps its name; st0rm94's is told in its note
                    if r[0] == st and r[2] == off10 and r[5] in ('game.h', 'game_build.h') and 'st0rm94' not in r[6]:
                        r[6] += '; st0rm94: ' + name
                continue
            off12, why, strong = carry(votes, off10)
            if off12 is not None and off12 in sdk_at[(st, 12)]:
                off12, why = None, '1.2 not carried: its accesses point at another field of the SDK\'s (%s)' % hx(off12)
            cands.append([st, off12, off10, name, typ, 'st0rm94', note, d['doc'], why, strong])
    # A class's layout moves in steps between the builds (1.0's C_Vehicle is 1.2's + 0x7C past its start): a field
    # carried against the step of the sure fields on both sides of it (the SDK's, and carried fields every access
    # agrees on) is not taken: its accesses were another object's.
    anchors = defaultdict(list)
    for r in fields:
        if r[5] in ('game.h', 'game_build.h') and r[1] is not None and r[2] is not None:
            anchors[r[0]].append((r[2], r[1] - r[2]))
    for c in cands:
        if c[1] is not None and c[9]:
            anchors[c[0]].append((c[2], c[1] - c[2]))
    for k in anchors:
        anchors[k].sort()
    for st, off12, off10, name, typ, src, note, doc, why, strong in cands:
        if off12 is not None:
            lo = [dl for o, dl in anchors[st] if o < off10]
            hi = [dl for o, dl in anchors[st] if o > off10]
            if lo and hi and lo[-1] == hi[0] and off12 - off10 != lo[-1]:
                off12, why = None, '1.2 not carried: its accesses say %s, the fields around it move by %s' % (
                    hx(off12), ('-' if lo[-1] < 0 else '+') + hx(abs(lo[-1])))
        carried['carried' if off12 is not None else 'not carried'] += 1
        fields.append([st, off12, off10, name, typ, src, (note + ' (' + doc + '; ' + why + ')').strip()])

    # One row a place: a member found in a constructor where the SDK names the place already, a field of st0rm94's
    # where a base class has one, are left out; a name taken twice in a class gets its 1.0 offset.
    sdk_place = {(r[0], b, o) for r in fields if r[5] in ('game.h', 'game_build.h')
                 for b, o in ((12, r[1]), (10, r[2])) if o is not None}
    def bases(st):
        out, c = [], structs.get(st, {}).get('base')
        while c and c not in out:
            out.append(c)
            c = structs.get(c, {}).get('base')
        return out
    place = {(r[0], b, o) for r in fields for b, o in ((12, r[1]), (10, r[2])) if o is not None}
    kept, used = [], defaultdict(set)
    for r in fields:
        if r[5] == 'code' and ((r[0], 12, r[1]) in sdk_place or (r[0], 10, r[2]) in sdk_place):
            continue
        if r[5] == 'st0rm94' and any((b, 12, r[1]) in place or (b, 10, r[2]) in place for b in bases(r[0])):
            continue
        kept.append(r)
    for r in sorted(kept, key=lambda r: r[5] == 'st0rm94'):
        if r[3] in used[r[0]]:
            r[3] += '_%x' % (r[2] if r[2] is not None else r[1])
        used[r[0]].add(r[3])
    # a field of st0rm94's inside a member the SDK or the constructors embed (the member's own layout has it)
    def size12(typ):
        m = re.fullmatch(r'([A-Za-z_]\w*?)(?:\[(\d+)\])?', typ)
        if not m or m.group(1) not in structs:
            return None
        n = structs[m.group(1)]['size']
        return n * int(m.group(2) or 1) if n else None
    inside = defaultdict(list)   # struct -> [(start, end)] of its embedded members (1.2)
    for r in kept:
        if r[5] != 'st0rm94' and r[1] is not None and size12(r[4]):
            inside[r[0]].append((r[1], r[1] + size12(r[4])))

    def within_member(r):
        return r[5] == 'st0rm94' and r[1] is not None and any(lo <= r[1] < hi for lo, hi in inside[r[0]])
    fields = [r for r in kept if not within_member(r)]

    # 4. the rows written by hand
    path_f, path_s = os.path.join(OUT, 'fields.csv'), os.path.join(OUT, 'structs.csv')
    kept_f = [r for r in csv.DictReader(open(path_f, encoding='utf-8'))] if os.path.exists(path_f) else []
    kept_s = [r for r in csv.DictReader(open(path_s, encoding='utf-8'))] if os.path.exists(path_s) else []
    kept_f = [r for r in kept_f if r['source'] not in GENERATED]
    kept_s = [r for r in kept_s if not set(r['source'].split('+')) <= GENERATED]

    os.makedirs(OUT, exist_ok=True)
    num = lambda v: int(v, 16) if isinstance(v, str) and v else v if isinstance(v, int) else 1 << 30
    rows_f = [[r[0], hx(r[1]), hx(r[2]), r[3], r[4], r[5], r[6]] for r in fields]
    rows_f += [[r['struct'], r['offset'], r['offset_10'], r['name'], r['type'], r['source'], r['note']] for r in kept_f]
    rows_f.sort(key=lambda r: (r[0].lower(), min(num(r[1]), num(r[2])), r[3]))
    with open(path_f, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(['struct', 'offset', 'offset_10', 'name', 'type', 'source', 'note'])
        w.writerows(rows_f)
    rows_s = []
    for name, s in structs.items():
        rows_s.append([name, s['base'] or '', hx(s['size']), hx(s['size_10']), hx(s['vtable']), hx(s['vtable_10']),
                       hx(s['kind']), '+'.join(sorted(s['source'])), '; '.join(s['note'])])
    names_kept = {r['struct'] for r in kept_s}
    rows_s = [r for r in rows_s if r[0] not in names_kept]
    rows_s += [[r['struct'], r['base'], r['size'], r['size_10'], r['vtable'], r['vtable_10'], r['kind'], r['source'], r['note']] for r in kept_s]
    rows_s.sort(key=lambda r: r[0].lower())
    with open(path_s, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(['struct', 'base', 'size', 'size_10', 'vtable', 'vtable_10', 'kind', 'source', 'note'])
        w.writerows(rows_s)

    nvt = sum(1 for s in structs.values() if s['vtable'])
    nvt10 = sum(1 for s in structs.values() if s['vtable_10'])
    print('structs: %d (vtables 1.2 %d, 1.0 %d); fields: %d; st0rm94\'s fields: %s' % (
        len(rows_s), nvt, nvt10, len(rows_f), dict(carried)))
    weak = sorted(c for c, (agree, paired) in vt_checked.items() if agree < max(1, 0.5 * paired))
    if weak:
        print('1.0 vtables not taken (their slots are not twins of 1.2\'s):', ', '.join(weak))


if __name__ == '__main__':
    main()
