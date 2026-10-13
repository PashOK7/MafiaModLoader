// The builds of the game the mod knows, and where each keeps what the mod uses.
//
// Mafia shipped as 1.0 (2002, LS3D 3.84) and was patched to 1.2 (Steam and GOG sell 1.2). The two Game.exe and the
// two LS3DF.dll hold the same code at other addresses, and a few structures changed size between them (the car's
// C_Vehicle, the engine's vertex buffer). Game.exe and LS3DF.dll are told apart independently, by their code: every
// build lists functions whose first bytes must be at its addresses (Detect). How each address was found and checked:
// docs/VERSIONS.md (tools/port_addrs.py, tools/port_map.py).
//
// Every module calls game::Detect once before it touches the game (the ASI, the host, the core: each DLL has its own
// copy of these variables). Nothing else may run when it fails.
#pragma once
#include <cstdint>
#include <cstring>
#include <excpt.h>
#include <initializer_list>

namespace game {

// Game.exe (image base 0x400000).
struct ExeBuild {
    const char *name;
    // globals
    uintptr_t missionPtr;          // C_mission*
    uintptr_t collision;           // g_collision
    uintptr_t pedElements;         // the traffic pedestrians' elements: [begin, end) at +0, +4
    uintptr_t pedManagerVtable;    // the traffic pedestrian manager (an actor)
    // functions
    uintptr_t missionTick, missionClose;       // thiscall(C_mission*, int dt_ms), thiscall(C_mission*)
    uintptr_t humanHit;                        // thiscall(C_human*, type, dir, pos, normal, float dmg, attacker, part, frame), ret 0x20
    uintptr_t actorDtor, humanCtor;            // thiscall(C_actor*), thiscall(C_human*)
    uintptr_t testLine;                        // thiscall(g_collision*, start, delta, hit, normal, uint flags) -> primitive
    uintptr_t frameWorldMatrix;                // fastcall(I3D_frame*) -> const float[16]
    uintptr_t actorSetPos, frameSetPos, sceneMoved, vehicleDeactivate, carReset;   // what the scripts' ACTOR_SETPOS calls
    uintptr_t vehicleExternalForce;            // thiscall(C_Vehicle*, force, point, int flags, scale), ret 0x10
    uintptr_t pedHit;                          // thiscall(element, type, dir, part, attacker, float damage), ret 0x14
    uintptr_t pedCarSweep, pedCarHit;          // the pedestrian system's vtable slots 11 and 8
    uintptr_t sphereSweep;                     // thunk: thiscall(g_collision*, 7 arguments)
    uintptr_t pedStepReturn1, pedStepReturn2;  // the pedestrian step's two sphere sweep calls return here
    // the game's own bugs game_fixes patches (each site's bytes are checked there)
    uintptr_t save1d, save1dGo, save1dNone, save1e, save1eGo, save1eNone;
    uintptr_t carExit, carExitGo, carExitDone;
    uintptr_t waterWalk, waterWalkPast, waterFall, waterFallPast;
    uintptr_t carWater, carIn, carInPast;
    // C_car and its C_Vehicle (the car's rigid body, at C_car + vehicle)
    int vehicle;
    int vehCentre, vehTurn, vehMass, vehVelocity, vehBalance, vehInertiaA, vehInertiaB;
    int carInWater;                            // C_car's "in the water" byte (the script's CAR_INWATER)
    // Bullets (hits.cpp; 0: not known for this build, bullets then pass bodies on the ground as in the game).
    // collRayTest: thiscall(g_collision*, from, delta, out hit, out normal, int) -> primitive (+0x44 its owner),
    // ret 0x14; bulletRay: the return address of its call in the bullets' tick; humanBoneRayTest:
    // thiscall(human + humanParts, from, delta, float length^2) -> the body part hit or 0, ret 0xC.
    // pedSystemVtable: the owner of every pedestrian's collision object (element + 0xC4, whose +4 is the element's
    // model frame); pedBulletHit: its Hit (slot +0x7C), thiscall(system, type, dir, from, pos, damage, attacker, ?,
    // frame), ret 0x20. It wakes the element up before it looks at its health (a dead one then cries out).
    uintptr_t collRayTest, bulletRay, humanBoneRayTest, pedSystemVtable, pedBulletHit;
    int humanColl, humanCollCount, humanParts;  // C_human: its collision objects (array, count), its parts' volumes
    // Getting up (getup.cpp; 0: not known for this build, a knocked-over person is handed back to the game as it
    // lies). pedAnimStart: fastcall(element), starts the pedestrian's animation (+0x164) when another one plays
    // (+0x160) and steps its sequence state (+0x14C) down. C_human's action queue: entries of 0x14 bytes from
    // humanActs ({type: 1 an animation, 3 the end; the animation's number; ...}), the one playing at humanActIndex;
    // humanState: the human's state (6: it runs the queue; 1 when the queue's end is reached alive).
    // collGridAdd: thiscall(g_collision, object), puts a collision object into the grid (nothing when it is in it:
    // its cell link +0xC is set); collGridRemove takes it out.
    uintptr_t pedAnimStart, collGridAdd;
    int humanActs, humanActIndex, humanState;
    // collGridMove: thiscall(g_collision, object), puts an object that is in the grid into the cell of its position
    // now (+0x10, +0x18 of the object); pedTick calls it for a walking pedestrian (state 0) only.
    uintptr_t collGridMove;
    // A vehicle a person walks in (a tram, actor type 8): C_human +0x98 the one he is in (his model is its frame's
    // child, his position +0x24 and facing +0x30 in its space), +0x9C the one to go into, +0x584 which of its cars
    // (0: its own frame, n: its frame list +0xC4's n-th). humanPlatform: thiscall(human, bool in): in (+0x9C -> +0x98,
    // position and facing into its space, the model given to it) or out (into the world, the model to the primary
    // sector; the player's camera out of first person). tramCarOf: thiscall(tram, float pos[3]) -> int, the car nearest.
    uintptr_t humanPlatform, tramCarOf;
    // Starting the game (the loader's start, host/start.cpp; 0: not known for this build). playVideo:
    // thiscall(const char *name, float w, float h) -> bool, ret 8; InitSystem plays logo1, logo3 and logo2 with it
    // before the profile menu. The menus (GM_Menu): menuTop, the active one (each menu's +4: the one under it); a
    // menu's command handler, what a click calls, is its vtable's +0x24, thiscall(menu, int command). The profile
    // menu (menuProfileVt): command 100 + i takes the i-th profile of the list loadSave (G_LoadSaveClass) has read
    // (+4 begin, +8 end; records of 0x54 bytes, +8 the profile's number, +0x10 its name). The main menu (menuMainVt):
    // 990105 opens the freeride menu (menuFreerideVt), whose 201 starts the freeride chosen (its +0x30; 28, the
    // default, is missions\freeitaly).
    uintptr_t playVideo, menuTop, loadSave, menuProfileVt, menuMainVt, menuFreerideVt;
};

// LS3DF.dll (relocated: RVAs).
struct Ls3dBuild {
    const char *name;
    uintptr_t render;          // I3D_driver::Render, stdcall(driver, scene)
    uintptr_t frameRelease;    // the frames' shared Release, stdcall(this) -> count
    uintptr_t meshRelease;     // the meshes'
    uintptr_t modelOpen;       // fastcall(file, flags, callback, ctx, parent)
    uintptr_t frameDup;        // I3D_frame::Duplicate, stdcall(this, source)
    uintptr_t frameLimit;      // the frame limiter the main loop calls every frame, stdcall(driver, a, b)
    uintptr_t emitorVtable, emitorSetParam;   // (diagnostics; 0: not known for this build)
    // functions the mod never hooks, only for recognising the build: I3D_frame's vtable slots +0x18, +0x38, +0x3C,
    // the material open
    uintptr_t frameSlot18, frameSlot38, frameSlot3C, materialOpen;
    // the engine's vertex buffer (a LOD's +0x0C): its FVF, stride and IDirect3DVertexBuffer8*
    int vbFvf, vbStride, vbD3d;
    // The shared index buffers (diagnostics, `ib_watch`; 0: not known for this build). ibAlloc: thiscall(manager,
    // usage, count, handle*) -> bool, a slice of a shared buffer for the owner's 3-word handle {chunk, IB, offset};
    // the chunk keeps the handle's address. ibFree: stdcall(handle*), the slice back. ibManager: the manager (its
    // tree of chunks: header node at +0x14, nil node ibNil); drvClose: I3D_driver::Close, stdcall(driver), which
    // zeroes the handle of every slice still held.
    uintptr_t ibAlloc, ibFree, ibManager, ibNil, drvClose;
    // The scene's camera put somewhere else for a frame (scene_camera): camRecompute, a byte Render reads as "the
    // camera moved" (its lights, sectors worked out once); frameUpdateWMatrix: I3D_frame::UpdateWMatrixProc, thiscall.
    uintptr_t camRecompute, frameUpdateWMatrix;
};

inline const ExeBuild kExe12 = {
    "1.2",
    0x63788C, 0x647F48, 0x63D768, 0x623FA0,
    0x5407E0, 0x5405E0, 0x5762A0, 0x405D90, 0x56EFD0, 0x5C74D0, 0x47ACD0,
    0x47AF50, 0x460980, 0x47ADF0, 0x4C31E0, 0x422EE0, 0x4CBBD0,
    0x4588D0, 0x44C6A0, 0x44CA50, 0x5D5940, 0x453619, 0x45394F,
    0x52CFCE, 0x52CFD6, 0x52CFE1, 0x52D117, 0x52D11F, 0x52D12A,
    0x573014, 0x573020, 0x57327F,
    0x57B8F2, 0x57BAB1, 0x57C2C2, 0x57C465,
    0x4216D8, 0x42658A, 0x426766,
    0x70, 0x398, 0x420, 0x5F4, 0x1F90, 0x438, 0xD68, 0xDA8, 0x2219,
    0x5C8440, 0x5A935C, 0x5D6FE0, 0x623EF8, 0x44CCF0,
    0x688, 0x690, 0x2DC,
    0x459450, 0x5C36C0, 0x23C, 0x2A0, 0x40C,
    0x5C3AC0,
    0x571B90, 0x492D60,
    0x55D460, 0x6BD890, 0x6D4628, 0x6279DC, 0x625EB8, 0x62736C,
};

inline const ExeBuild kExe10 = {
    "1.0",
    0x65115C, 0x67A588, 0x6560C4, 0x63C778,
    0x5FFD40, 0x5FFB40, 0x496710, 0x44B740, 0x48F430, 0x582600, 0x4F46C0,
    0x5C86F0, 0x45F000, 0x5C8600, 0x50EE60, 0x469DD0, 0x517F00,
    0x4BD0D0, 0x4B0EB0, 0x4B1260, 0x590A40, 0x4B7E69, 0x4B819F,
    0x4378E3, 0x4378EB, 0x4378F6, 0x437A2C, 0x437A34, 0x437A3F,
    0x493484, 0x493490, 0x4936EF,
    0x49BE52, 0x49C011, 0x49C822, 0x49C9C5,
    0x468567, 0x46D87A, 0x46DA56,
    // 1.0's C_Vehicle differs from 1.2's (most fields up to +0xD00 sit 0x7C higher, the velocity 0x84 lower); each
    // offset here is read from the 1.0 code that uses it (docs/VERSIONS.md). C_car's own fields sit 0xC0 lower.
    0x70, 0x414, 0x49C, 0x670, 0x1F0C, 0x4B4, 0xD64, 0xDA4, 0x2159,
    0, 0, 0, 0, 0,   // bullets: not looked for in 1.0 yet
    0, 0, 0,
    0, 0, 0, 0, 0,   // getting up: not looked for in 1.0 yet
    0,
    0, 0,            // trams: not looked for in 1.0 yet
    0, 0, 0, 0, 0, 0,   // the start: not looked for in 1.0 yet
};

inline const Ls3dBuild kLs3d12 = {"1.2", 0x62030, 0x1AAE0, 0x313A0, 0x603E0, 0x1C5B0, 0x72100, 0x9C538, 0x3DFF0,
                                  0x1B040, 0x1BFD0, 0x1C2A0, 0x2C000, 0x08, 0x18, 0x1C,
                                  0x5B6C0, 0x5B940, 0x1B50F8, 0x1B5124, 0x701A0,
                                  0xA93F0, 0x1C7D0};
// LS3D 3.84.5 (31.7.2002): the vertex buffer has no flags word, everything after +0x04 sits 4 bytes lower.
inline const Ls3dBuild kLs3d10 = {"1.0", 0x5ED80, 0x1A8A0, 0x305B0, 0x5D070, 0x1C350, 0x6E580, 0, 0,
                                  0x1AE00, 0x1BD60, 0x1C030, 0x2B540, 0x04, 0x14, 0x18,
                                  0, 0, 0, 0, 0,
                                  0, 0};   // the scene camera: not looked for in 1.0 yet

// The builds found (null until Detect succeeds).
inline const ExeBuild *g_exe;
inline const Ls3dBuild *g_ls3d;
inline const ExeBuild &Exe() { return *g_exe; }
inline const Ls3dBuild &Ls3d() { return *g_ls3d; }

// Functions whose first 16 bytes are the same in every build: a build is the one that has all of them at its
// addresses. None of them is one the mod (or its developer host and core) hooks: a hook's jump over their first
// bytes would make the build unknown to whoever looks next (the core looks after the ASI's or the host's hooks).
namespace detail {
struct Sig { uintptr_t ExeBuild::*exe; uintptr_t Ls3dBuild::*ls3d; uint8_t bytes[16]; };
inline const Sig kSigs[] = {
    {&ExeBuild::testLine, nullptr, {0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xBC, 0x24, 0xB0, 0x00, 0x00}},
    {&ExeBuild::vehicleExternalForce, nullptr, {0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0x8B, 0x84, 0x24, 0x9C, 0x00, 0x00, 0x00, 0x53, 0x8B, 0xD9}},
    {&ExeBuild::actorSetPos, nullptr, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x54, 0x24, 0x08, 0x83, 0xC1, 0x24, 0x89, 0x01, 0x8B, 0x44, 0x24}},
    {&ExeBuild::sceneMoved, nullptr, {0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0xF9, 0xF6, 0x86, 0xAC, 0x00, 0x00, 0x00, 0x20, 0x75}},
    {&ExeBuild::frameSetPos, nullptr, {0x8A, 0x81, 0xAC, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x78, 0x32, 0x8B, 0x54, 0x24, 0x04, 0x56, 0x8D}},
    {nullptr, &Ls3dBuild::frameSlot18, {0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x53, 0x8B, 0x9C, 0x24, 0x08, 0x01, 0x00, 0x00, 0x56, 0x8B}},
    {nullptr, &Ls3dBuild::frameSlot38, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x4C, 0x24, 0x08, 0x3B, 0x88, 0x1C, 0x01, 0x00, 0x00, 0x7D, 0x2E}},
    {nullptr, &Ls3dBuild::frameSlot3C, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x88, 0x24, 0x01, 0x00, 0x00, 0x85, 0xC9, 0x53, 0x55, 0x56, 0x57}},
    {nullptr, &Ls3dBuild::materialOpen, {0x81, 0xEC, 0x28, 0x03, 0x00, 0x00, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x08, 0x8B, 0x0D}},
};
// The bytes at `at` are `b` (false where the memory cannot be read: an address past another build's image).
inline bool CodeIs(uintptr_t at, const uint8_t *b, size_t n) {
    __try { return memcmp(reinterpret_cast<const void *>(at), b, n) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool ExeIs(const ExeBuild &e) {
    for (const Sig &s : kSigs) if (s.exe && !CodeIs(e.*s.exe, s.bytes, 16)) return false;
    return true;
}
inline bool Ls3dIs(const Ls3dBuild &l, uintptr_t base) {
    for (const Sig &s : kSigs) if (s.ls3d && !CodeIs(base + l.*s.ls3d, s.bytes, 16)) return false;
    return true;
}
} // namespace detail

// Finds the running builds; `ls3dBase` is LS3DF.dll's module handle. True when both are builds the mod knows; the
// mod must not touch the game otherwise (g_exe or g_ls3d is null then). Once found, they stay found (the builds do
// not change while the game runs).
inline bool Detect(uintptr_t ls3dBase) {
    if (g_exe && g_ls3d) return true;
    g_exe = nullptr;
    g_ls3d = nullptr;
    for (const ExeBuild *e : {&kExe12, &kExe10}) if (detail::ExeIs(*e)) { g_exe = e; break; }
    if (ls3dBase)
        for (const Ls3dBuild *l : {&kLs3d12, &kLs3d10}) if (detail::Ls3dIs(*l, ls3dBase)) { g_ls3d = l; break; }
    return g_exe && g_ls3d;
}
inline const char *ExeName() { return g_exe ? g_exe->name : "unknown"; }

// A section of Game.exe in memory (".text": the code, ".rdata": the vtables), from its headers; false if it has none.
inline bool ExeSection(const char *name, uintptr_t *lo, uintptr_t *hi) {
    const uint8_t *base = reinterpret_cast<const uint8_t *>(0x400000);
    const uint8_t *nt = base + *reinterpret_cast<const uint32_t *>(base + 0x3C);
    unsigned count = *reinterpret_cast<const uint16_t *>(nt + 6), optSize = *reinterpret_cast<const uint16_t *>(nt + 20);
    const uint8_t *sec = nt + 24 + optSize;
    for (unsigned i = 0; i < count; i++, sec += 40) {
        if (strncmp(reinterpret_cast<const char *>(sec), name, 8) != 0) continue;
        *lo = 0x400000 + *reinterpret_cast<const uint32_t *>(sec + 12);
        *hi = *lo + *reinterpret_cast<const uint32_t *>(sec + 8);
        return true;
    }
    return false;
}
inline bool InExeSection(const char *name, uintptr_t v) {
    uintptr_t lo, hi;
    return ExeSection(name, &lo, &hi) && v >= lo && v < hi;
}
inline const char *Ls3dName() { return g_ls3d ? g_ls3d->name : "unknown"; }

} // namespace game
