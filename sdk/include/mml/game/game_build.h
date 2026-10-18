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
    // The controls (host/input.cpp; 0: not known). controls: the object the game reads its actions from: +0xB4/+0xB8
    // the bindings (one 12-byte slot per action and binding: {u16 dir, u16 0, u16 device (1 key, 2 mouse, 3 joystick,
    // 0 none), u16 code, u32 joystick}), +0xC4/+0xD4 two float arrays parallel to them: held, and pressed this frame.
    // controlsUpdate: thiscall(controls, char) ret 4, fills the arrays from the devices once a frame (the game loop,
    // before C_mission::Tick; some menus). controlsKeyName: thiscall(controls, slot*) -> char*, a binding's name.
    // textDb, textGetOrNull: G_TextDatabase and its GetTextOrNULL, thiscall(db, id) -> char* (the localised texts).
    uintptr_t controls, controlsUpdate, controlsKeyName, textDb, textGetOrNull;
    // textGet: G_TextDatabase::GetText, thiscall(db, id) ret 4 -> char* (a placeholder string when the id is unknown);
    // the loader answers a range of ids of its own through it (host/menus.cpp: options of switches and selectors).
    uintptr_t textGet;
    // A menu's components by their id (host/start.cpp; 0: not known): menuSetValue thiscall(menu, id, float) ret 8
    // and menuGetValue thiscall(menu, id) -> float, ret 4: the component found (GM_Menu::FindComponentByID) and its
    // own setter / getter called (its vtable's +0x4C / +0x50): a slider's position, 0..1. The freeride menu's
    // sliders: 21 traffic, 22 pedestrians, 23 police patrols (its OnCreate 0x5E1920 sets them to 1 with this
    // setter; its start, command 201, reads them with this getter). They count only when the profile has opened
    // the freeride's settings (loadSave +0x1F0 >= 3).
    uintptr_t menuSetValue, menuGetValue;
    // The mouse sensitivity of the options (a float; host/input.cpp; 0: not known). On foot the player's code
    // (0x58F4B0) turns by mouseSensitivity * 5 * held["Aim X"] radians an update; a mouse axis' held value is its
    // counts / 400 (the ranges set at 0x4EFDB0). Measured in the lab: 141.3 degrees for a sum of 1 at 0.4932.
    uintptr_t mouseSensitivity;
    // The frame's 2D pass and the game's own 2D drawing (host/draw.cpp; 0: not known). draw2d: the callback the
    // game gives its scene, stdcall(int stage, ?, ?) ret 0xC: LS3D's render calls it with stage 1, and with stage 2
    // once the 3D frame is drawn: the game then draws the HUD (hud: G_IndicatorsClass, a static object, its
    // DrawAll), the open menu and the console. hudOutText: G_IndicatorsClass::OutText, thiscall(hud, text, float x,
    // y, width, height, uint argb, int flags, int font, const char *end) ret 0x24, in pixels: flags 2 centres the
    // text in the width, 4 ends it at x + width; the fonts are records of hudFontSize bytes from hud + hudFonts
    // (+8, a float: the font's size factor). hudTextSize: its TextSize, thiscall(hud, text, float height * the
    // font's factor, int font, const char *end) -> float width, ret 0x10. hudScale: hud + this, two floats: the
    // pixels in a unit of the 800x600 space the HUD and the menus are laid out in (x, y); hudXOffset: a float 1.2
    // adds to every x (0 at 4:3; 1.0 has none: 0 here). igraph, i3dDriver: where the game keeps its IGraph* and
    // I3D_driver* (LS3D). IGraph's
    // vtable: +0x0C SetTexture(tex), +0x44 SetState(id, value), +0x48 GetState(id), +0x58 DrawPrimitive(type,
    // count, vertices, 1) with 32-byte vertices {x, y, z, rhw, diffuse, specular, u, v} (type 1 a line list, 2 a
    // line strip, 5 a triangle strip). The driver's: +0x14 SetState(id, value), +0x18 GetState(id); the game sets
    // its state 3 to 0 for its 2D drawing.
    uintptr_t draw2d, hud, hudOutText, hudTextSize, hudXOffset, igraph, i3dDriver;
    int hudScale, hudFonts, hudFontSize;
    // The mission (host/game_hooks.cpp; 0: not known). missionOpen: C_mission::Open, thiscall(mission, const char
    // *name, bool, uint, bool) ret 0x10: it closes the open mission, then loads missions\<name>. missionName:
    // C_mission's own copy of that name (a char*).
    uintptr_t missionOpen;
    int missionName;
    // The car by name (game.h `Car`; 0: not known). hudCar: hud + this, the C_car the player drives or drove last
    // (what G_IndicatorsClass::RadarSetPlayerCar sets; the speedometer's car). Fields of C_Vehicle (C_car +
    // vehicle), each read in the code named: vehPos, the world position (C_Vehicle::CalcSpeed copies the car
    // frame's there); vehSpeed, |velocity| in m/s (CalcSpeed; what C_player's Action compares with 0.5);
    // vehSteerWanted, the steering angle asked for, radians, left negative (C_Vehicle::SetSteer writes it, input
    // times vehSteerMax, the limit, which shrinks with speed); vehSteer, the angle the wheels have now (it follows;
    // SaveVehicleState saves it); vehGear (-1 reverse, 0 neutral, 1...) and vehGearMax (SetGear); vehRps, the
    // engine's turns a second (C_Vehicle::GetRotations returns it times 60); vehFuel and vehFuelTank, litres
    // (SetFuel); vehEngineOn, a byte (GetRotations, SetEngineOn); vehEngineHealth and vehEngineHealthMax (the
    // engine's damage, 0x4D6720 in 1.2, lowers the first; the engine stops at 0).
    // A car's state saved and put back, the game's own way (its saved games do it for every car): vehStateSize,
    // thiscall(C_Vehicle*) -> bytes; vehStateSave, thiscall(C_Vehicle*, void *buffer) ret 4; vehStateLoad,
    // thiscall(C_Vehicle*, const void *buffer) -> bool, ret 4 (the buffer starts {u32 9, u32 size}).
    int hudCar;
    int vehPos, vehSpeed, vehSteerWanted, vehSteer, vehSteerMax, vehGear, vehGearMax, vehRps, vehFuel, vehFuelTank;
    int vehEngineOn, vehEngineHealth, vehEngineHealthMax;
    uintptr_t vehStateSize, vehStateSave, vehStateLoad;
    // The menus as objects (host/menus.cpp; 0: not known). A menu is a GM_Menu: +4 the menu under it, +8 its root
    // control, +0xC the focused one, +0x14/+0x18 its controls (a vector of pointers, in the order they were made),
    // +0x20 a byte "closing", +0x2C flags (0x10000: the menu under it is drawn too). Its vtable has 12 slots:
    // 0 its definition's name (tables\menu\<name>.mnu), 1 OnCreate(), 2 OnClose(), 7 OnKey(char key, shift) (the
    // base's closes the menu on Esc), 9 OnCommand(int id): what a click or Enter on a control sends, 10 Tick(ms).
    // menuBaseVt: GM_Menu's own vtable. menuCtor: thiscall(menu) on gameNew(0x30) (the game's operator new, cdecl;
    // the game deletes its menus itself). menuCreate: GM_Menu::Create, thiscall(menu): the controls made from the
    // definition by the factories of menuFactories (a list of {create fastcall(record, parent), clone
    // thiscall(control), tag, next}), then OnCreate. menuPush: stdcall(menu, arg) ret 8: the menu put over the
    // active one and created. menuClose: GM_Menu::MenuClose, thiscall(menu, int result) ret 4. menuDestroy:
    // thiscall(menu): its controls and itself deleted. menuFind: GM_Menu::FindComponentByID, thiscall(menu, id).
    // menuAddUnder: GM_Menu::PridejComponentPod, thiscall(menu, int id, int newId, int growId, int move1, move2,
    // move3) ret 0x18: a copy of control `id` made by its kind's clone and put right under it with the id newId;
    // the control growId grows by its height, the three others move down by it (how the game's own lists grow).
    // A control: +0 vtable (2 its kind's tag, 5 Draw(flags), 6 Layout, 16 SetText(const char *): the pointer kept,
    // 19 SetValue(float), 20 GetValue), +4 id, +8 flags (low 4 bits the font, 0x10 shown, 0x20 enabled (both handed
    // down to the children when drawn), 0x40 right, 0x80 centred, 0x100..0x400 the colour, each kind its own (read
    // in their Draw: a text 0x5DA090 grey, 0x100 white, 0x200 yellow, 0x300 black; a button 0x5DB7B0 black, 0x100
    // red, 0x200 white; a row 0x5DADC0 black, 0x400 red), 0x8000 it takes the focus, bit 31 focused), +0xC text id,
    // +0x10 text, +0x14 x, y, w, h on the screen (pixels), +0x24 x, y in its parent (pixels), +0x2C parent,
    // +0x34/+0x38 children; a slider's ("slid", SetValue 0x5DBA50) and a progress bar's ("pbar", 0x5DF350; its
    // GetValue is the base's, 0) value 0..1 at +0x40.
    uintptr_t menuCreate, menuDestroy, menuCtor, menuPush, menuClose, menuAddUnder, menuFind, menuBaseVt, menuFactories, gameNew;
    // The menus' mouse (host/input.cpp; 0: not known). menuCursor: the cursor, two ints in pixels; the menus' input
    // tick (0x5EA8C0 in 1.2) adds the mouse's lX, lY to it, focuses the control under it once it has moved more than
    // 10 pixels, and gives the buttons to the focused control (its vtable +0x24 hit test, +0x2C the click).
    // menuScale: two floats, the pixels in one of the menus' 800x600 units (x, y), set by the menus' loop; a
    // control's screen place is its definition's times these (GM_Control's constructor 0x5E9BE0).
    uintptr_t menuCursor, menuScale;
    // menuLang: the language's two letters (a u16), the suffix the definitions' loader (0x5EA370) tries first:
    // tables\menu\<name>_<ll>.mnu, then tables\menu\<name>.mnu. It is +4 of the record (0x238 bytes, "Lang", the
    // letters, the language's name, its fonts' map) the game reads from tables\lang_<ll>.def (1.2 0x5F7000, 1.0
    // 0x549060); the language also says the texts' code page (host GameCodePage).
    uintptr_t menuLang;
    // The controls' arrays in the controls object (host/input.cpp): controlsBind its binding slots' begin (the end
    // 4 further; 12 bytes a slot), controlsHeld and controlsPressed the float arrays "held" and "pressed this
    // frame". 1.0 keeps them 4 bytes lower than 1.2 (read in both builds' controlsUpdate).
    int controlsBind, controlsHeld, controlsPressed;
    // The 1.0 menus (host/menus10.cpp; 0 in 1.2, whose menus are the GM_Menu classes above). 1.0's screens are
    // other classes (docs/VERSIONS.md, knowledge/notes/interface.md "1.0"): a screen is +0x18 the one under it,
    // +0x20/+0x24 its controls (a vector of pointers), +0x2C the focused one, +0x40 flags, +0x44 the id it hands its
    // parent when it closes; its vtable has 6 slots: 0 the deleting destructor, 1 OnExchange, 2 Destroy (the
    // controls and itself deleted), 3 OnCloseChild(child id, result), 4 MenuClose(id, result), 5 Tick(7 arguments)
    // -> a code for the menus' loop (0: go on). There is no command handler: each screen's Tick calls the base's,
    // m10Tick (gui_menu_tick, thiscall(screen, key flags, key, mouse x, y, wheel, buttons, ms) ret 0x1C), which
    // returns the id of the control a click or Enter activated, and switches on it. m10Ctor: thiscall(screen,
    // parent, id) ret 8 (on gameNew(size)); m10Dtor: gui_menu_dtor, thiscall(screen), what every screen's
    // destructor ends in; m10Build: thiscall(screen, table, design, gate) ret 0xC, the controls made from the
    // design-th group of a table of 0x24-byte records {id, tag, x, y, w, h (800x600), text id, flags, colour}
    // (groups end in a record tagged "stop"; a container kind becomes the parent of the records after it);
    // m10Designs: the game's own table (tables\menu.def); m10UpdateAll: fastcall(screen), each control's slot 3
    // (its texts worked out again); m10CloneUnder: thiscall(screen, tag, id, newId) ret 0xC -> the copy of control
    // `id` (made as a `tag`), put right after it in the vector and one row under it (the game's profile list grows
    // so). m10BaseVt: the base class's vtable. m10Cursor: the menus' cursor, two floats in pixels (1.2's are ints);
    // m10CdFind: cd_find_game_file, fastcall(name, char *path) -> bool: the logos (logo1.avi, logo2.avi) are played
    // only when it finds them.
    uintptr_t m10Tick, m10Ctor, m10Dtor, m10Build, m10Designs, m10UpdateAll, m10CloneUnder, m10BaseVt, m10Cursor, m10CdFind;
    // The freeride menu's kind (28, the default, missions\freeitaly; its start reads it): its object + this (1.2's
    // command 201 handler reads +0x30, 1.0's tick +0x48).
    int freerideKind;
    // The mission scripts' machine (host/script.cpp; 0: not known): C_program, the class every scripted actor
    // carries (C_entity +0xA9C, C_detector +0x70), here a standalone one of the loader's. progCtor thiscall(prog) on
    // gameNew(the program's size, 0x68; 0x80 allocated); progSetSource thiscall(prog, const char *) ret 4 (copied,
    // kept for saves); progCompile thiscall(prog, char *text (lines cut in place), bool stop at the first error,
    // int *error's offset, int *its length) -> errors, ret 0x10: each line by the vtable's slot 6 (the command's name
    // to its opcode, -1 unknown) and slot 5 (its arguments to a payload); progCallProcess thiscall(prog, int ms) ->
    // bool, ret 4: the vtable's slot 7 (Process, the opcode switch) until an instruction gives the frame back (a
    // wait, a command that blocks; a command block keeps it); progDone thiscall(prog): the payloads and the
    // registers freed; progInvalidateActor thiscall(prog, actor) ret 4: that actor out of act[]. progVt: the
    // standalone program's vtable (slot 0 its deleting destructor, thiscall(prog, 1)). Its layout, the same in both
    // builds: +0x10/+0x14 the instructions ({opcode, payload*}, 8 bytes), +0x1C the current one, +0x20 the line
    // before (-1 at the start), +0x24 ms on the current one, +0x40 the call stack's head (a `call`'s), +0x44 its
    // own actor, +0x48/+0x4C flt[] (floats, count), +0x50/+0x54 frm[], +0x58/+0x5C act[], +0x64 in a command block.
    // END (opcode 6) ends a program: it stays on it.
    uintptr_t progCtor, progSetSource, progCompile, progCallProcess, progDone, progInvalidateActor, progVt;
    // Actors made new (host/world.cpp; 0: not known), the way the scripts' ACTOR_DUPLICATE makes its copy and
    // DAN_INTERNAL_1 its car (each read in both builds' code): a model frame from the I3D driver (its vtable's
    // drvCreateFrame, stdcall(driver, 9)), the model file opened into it by the model cache (modelOpen,
    // thiscall(modelCache, frame, const char *file, 0, 0, 0, 0) -> 0 when done, ret 0x18), the frame placed (its
    // vtable: +0x0C SetDir(frame, const float *dir, float roll), +0x18 Update(frame), +0x28 SetName(frame, name),
    // frameSetPos) and linked into the sector at its place (sceneMoved, on the mission's scene); then
    // missionCreateActor (C_mission::CreateActor, thiscall(mission, int type) -> actor, ret 4), the actor's vtable
    // +0x48 Init(frame) -> bool (+0x44 its Release when Init fails), and gameAddTemp (C_game::AddTemporaryActor,
    // thiscall(game, actor), ret 4: GameInit called, the actor ticked from then on). gameRemoveTemp
    // (RemoveTemporaryActor, ret 4) takes it out again (what ACTOR_DELETE calls; deleted in the game's next tick).
    // A car is then settled on its wheels: vehicleCalm (C_Vehicle::CalmVehicle, thiscall(vehicle, int), ret 4) with
    // 100, after its particles byte (C_car + carParticles) is set, as the cheat car does.
    uintptr_t modelCache, modelOpen, missionCreateActor, gameAddTemp, gameRemoveTemp, vehicleCalm;
    int drvCreateFrame, carParticles;
    // The game's tables (host/world.cpp; 0: not known). carDb: G_CarDatabase, {u32 count, records*}, carDbStride
    // bytes a record: +0x00 the model's prefix (the file is <prefix><colour, two digits>.i3d, as GetCarModel makes
    // it), +0x20 its wreck, +0x40 its shadow, +0x60 the name the menus show ("Bolt Ace Tudor"). items: the items of
    // tables\predmety.def (g_pItems, the count after it), 188 bytes a record in both builds: +0x00 the name,
    // +0x20 flags (0x04 a melee weapon, 0x20 a firearm or a throwable, 0x80 a throwable), +0x24 the model, +0x60
    // the rounds in a magazine, +0x64 the rounds in reserve (seen in the 1.2 game: Colt 1911 7 and 35, Thompson
    // 50 and 200); an item's id is its index (what HUMAN_ADDWEAPON takes). The tables' loader allocates them with
    // gameNew (the menus' fields), where host/items.cpp moves them.
    uintptr_t carDb, items;
    int carDbStride;
    // The main menu's flying camera (mods/examples/fps_unlock). menuCamTick: GM_Menu's TickCamera, fastcall(ms),
    // called by ExecuteMenu after the mission's tick while the menu's camera runs (1.0: inlined into its menu loop
    // 0x56CCC0, so 0). menuCam: its state, floats: +0x00 the look point's velocity, +0x0C the eye's velocity, +0x18
    // the look point's target, +0x24 the eye's target, +0x30 the look point, +0x3C the eye. Each tick moves the two
    // points by velocity * ms * 0.0003, then springs the velocities once a call: v = 0.98 v + 0.02 (target - point).
    uintptr_t menuCamTick, menuCam;
    // carRepair: C_car::RepairPosition, thiscall(C_car*, bool reset), what the scripts' CAR_REPAIR calls with true: the
    // car set on its wheels on the ground and reset (C_car::Reset: its body undented, its engine whole).
    uintptr_t carRepair;
    // 1.0's main menu camera, inlined into its menu loop (gui_menu_execute_modal 0x56CCC0): each frame a loop of 10
    // substeps (`mov eax, 10` at menuCamLoop, its `jne` back before menuCamLoopEnd) moves the look point and the eye
    // toward a middle point each (`* 0.001`, six `fmul dword [const]`, D8 0D <addr>) and the middle points toward
    // their targets (`* 0.002`, six more), with no time in it: the camera is 16 times as fast at 1000 fps. (1.2: 0;
    // its camera is menuCamTick's spring.)
    uintptr_t menuCamLoop, menuCamLoopEnd;
    // A person's step on foot (mods/examples/fps_unlock). humanSweep: the move's collision sweep
    // (C_human::DynSetNextCollOwnerMult, three spheres from the feet up), thiscall(C_human*, const float pos[3], const
    // float move[3], float out[3]) -> the surface touched or 0, ret 0xC; out is where the person ends (pos + move when
    // nothing is touched). humanSweepOwner: its twin for a person standing in a vehicle (TestMoveCollInOwner), the
    // same contract. Both aim the move 0.015 m lower once a call (`fsub dword [0.015]`). Their two callers get the
    // frame's time: humanMove (C_human::MovementIntern) and humanUpdate (C_human::Update), both thiscall(C_human*,
    // unsigned ms), ret 4. humanMoveState: the person's movement state (int; 1 and 3 on the ground, where a short
    // move is swept as a fixed 0.1 m probe).
    uintptr_t humanMove, humanUpdate, humanSweep, humanSweepOwner;
    int humanMoveState;
    // The sweeps' slides (2026-10-15; knowledge/notes/game.md "A person's step"): humanSlide (g_collision::TestPanak2,
    // thiscall(g_collision*, query*) -> the surface touched or 0, ret 4), what humanSweep calls: up to 10 sweeps of its
    // three spheres, each one's rest projected on the contact's plane and started again 0.01 m off it along the normal
    // (nine `fmul dword [0.01]`); humanSlideOwner, humanSweepOwner's twin of it (thiscall(g_collision*, the
    // vehicle's collision, query*), ret 8). The sweep starts its sphere 0.405 above
    // the feet (`fadd [0.405]`) and returns its centre minus 0.4; the vehicle sweep 0.4 and 0.4. Both aim 0.015 lower.
    uintptr_t humanSlide, humanSlideOwner;
    // A car's physics (mods/examples/fps_unlock hooks vehicleTick for its step probe; knowledge/notes/
    // vehicle_physics.md has the rest). vehicleTick: C_Vehicle::Tick, thiscall(C_Vehicle*, unsigned ms) ->
    // its flags' low byte, ret 4 (0 ms is taken as 1), called each frame by C_car::AI after the controls are set, and
    // by C_Vehicle::CalmVehicle with 83 ms; it runs the drivetrain and calls C_Vehicle::Engine (0x4E1CE0), which
    // moves the car by C_Vehicle::Move in equal substeps no longer than the vehicle's +0xC24 (0.028 s). vehicleFlags:
    // the vehicle's flags (+0x1A4: 1 awake, 2 calm: set by Move when it zeroed a car slower than 0.01 m/s and
    // 0.015 rad/s; 8 off the ground). vehicleStepTime: float, the last step Engine made (s); what SetBrake,
    // SetHandbrake and SetPower (in reverse) add to their ramps each call, so the ramps count that time once a call.
    uintptr_t vehicleTick;
    int vehicleFlags, vehicleStepTime;
    // The car's physics further (game.h `Car`, `Wheel`; each read in both builds' code, 2026-10-15). vehicleEngine:
    // C_Vehicle::Engine, thiscall(C_Vehicle*, float dt s, f1, f2), ret 0xC, what Tick calls: C_Vehicle::Move in equal
    // substeps no longer than vehSubstepMax (a float, s; 0.028 for the taxi), then vehicleStepTime = dt. vehicleMove:
    // C_Vehicle::Move, thiscall with 6 stack arguments (dt first), ret 0x18: one substep (Hex-Rays' decompile of it is
    // wrong: read the disassembly). vehicleWheelsColl: C_Vehicle::DoWheelsCollision, what Move calls twice: each
    // wheel's suspension and contact. vehAxes: the body's axes, three V3 (forward, right, up: the model frame's z, x
    // and y rows), turned each substep by vehTurn (the angular velocity, rad/s) through S_quat::RotationMatrix
    // (Ls3dBuild quatRotationMatrix: turns under 0.00049 rad a substep are lost there). vehWheels: the S_wheel*
    // array, vehWheelCount its length (Engine walks them). carSize: a C_car's bytes (C_mission::CreateActor's new
    // for type 4): the C_Vehicle runs from C_car + vehicle to its end.
    uintptr_t vehicleEngine, vehicleMove, vehicleWheelsColl;
    int vehSubstepMax, vehAxes, vehWheels, vehWheelCount, carSize;
    // What happens, as events (host/happen.cpp). newShoot: C_game::NewShoot, thiscall(C_game*, C_actor* shooter,
    // S_vector pos, S_vector dir (both by value), float damage, int particle, I3D_frame*, int shots), ret 0x2C: a
    // shot put into the game's shot queue (MafiaOnline's contract, checked). newExplosion: C_game::NewExplosion,
    // thiscall(C_game*, void* source, S_vector* pos, float radius, float damage, bool world, bool large, bool smoke,
    // int sound), ret 0x20. carExplosion: C_car::CarExplosion, thiscall(C_car*, uint flags), ret 4: a car blows up.
    // carCrash: C_car::Was_Collision, fastcall(C_car*, const S_vector* impulse, const S_vector*, const S_vector* pos,
    // float, const S_vector*, C_actor* struck (a car; null: the world), const prim* (the world's; null: a car), int),
    // ret 0x1C: each contact of a car's body or wheels (the debris and sparks: |impulse| / the car's mass).
    uintptr_t newShoot, newExplosion, carExplosion, carCrash;
    // Questions to the world (host/collide.cpp). testLineAll: g_collision::_TestLineH, thiscall(g_collision*, const
    // S_vector *from, const S_vector *delta, S_vector *hit, S_vector *normal, uint movingMask, uint staticMask) ->
    // the primitive or moving object hit (0: none), ret 0x18: the static world (a primitive whose first dword has a
    // bit of staticMask, or 0x20, is passed; -1: none tested) and the moving objects (one whose class +8 has a bit
    // of movingMask is passed; -1: none tested); a ray with no horizontal length goes to _TestLineV. The normal comes
    // out of the surface. TestLineHStatic (testLine) is it with movingMask -1. The sphere's sweep is the thunk
    // sphereSweep (TestSphereB with ecx = g_collision; stdcall(from, delta, float radius, S_vector *centre where it
    // stops, S_vector *normal, uint movingMask, uint staticMask), ret 0x1C). collExclude:
    // g_collision::DynSetNextCollOwnerMult, thiscall(g_collision*, objects*, int count, int), ret 0xC: the moving
    // objects the next tests pass (+0x2B8 the array, +0x2BC its count, +0x2B0 the third), as the game sets its own
    // shooter's and puts (0, 0, 0) back after.
    uintptr_t testLineAll, collExclude;
    // The game's state (host/state.cpp). gameSetScore: C_game::SetScore, thiscall(C_game*, int), ret 4: the
    // freeride's money (C_game +0x3640) set and, when it is shown (+0x363E), the HUD told (FREERIDE_SCORESET's call).
    // hudSetLives: G_IndicatorsClass::PlayerSetLives, thiscall(hud, unsigned value), ret 4: the HUD's health number,
    // which C_player::Hit sets to health +0x644 / maximum +0x604 * 100 rounded (at least 1 while alive).
    uintptr_t gameSetScore, hudSetLives;
    // The car's exhaust and the particles' table (host/effects.cpp; mafia-ragdoll's smoke_game.cpp read them in 1.2,
    // 1.0's paired instruction by instruction: the same code). vehExhaust: C_Vehicle::UpdateExhaustParticles,
    // thiscall(C_Vehicle*, int dt_ms), ret 4, every tick for each car: when the engine runs (vehState bit 0x200, and
    // vehEngineOn) and the car is slower than vehExMaxSpeed (a float), the shared emitter of the car's particle type
    // (vehExType, an int: 12 ordinary cars, 15 trucks; 0 none) is moved to each pipe and fired. The pipes:
    // [vehExPipes, +4) 24-byte records {position, direction} in the car's frame (its EXHAUST* dummies); the jet speed
    // is vehExJet (idle) to vehExJet + 4 (top) by vehRps / vehRpsMax; vehMatrix the body's world matrix (row-major).
    // emitterTable: [begin, end) of 0x1C-byte entries a particle type {emitter, velocity added at birth, at death}.
    // particleDefs: tables\particles.def, the records' pointer (count at +4), 0xA4 bytes each; C_car::LoadParticles
    // hands each to its emitter (SetParam) when the emitters are made. exEndShare: the float the exhaust takes for a
    // puff's velocity at death (that share of jet + carry, plus the emitter's own).
    uintptr_t vehExhaust, emitterTable, particleDefs, exEndShare;
    int vehState, vehExMaxSpeed, vehExType, vehExPipes, vehExJet, vehRpsMax, vehMatrix;
    // The weather (host/effects.cpp): LS3D's weather system is the mission scene's (its vtable +0xE8
    // SetWeatherParam, stdcall(scene, param, dword): a flag, an int or a float's bits by the parameter; +0xEC
    // GetWeatherParam, stdcall(scene, param) -> dword; +0xF0 its reset, stdcall(scene)), as the scripts'
    // WEATHER_SETPARAM and WEATHER_RESET call it. weatherSave: C_game::OsefujPocasi, thiscall(C_game*, bool save,
    // int param), ret 8, what WEATHER_SETPARAM calls after On (0) or MaxCount (10): both kept and applied again
    // scaled by the detail setting.
    uintptr_t weatherSave;
    // Music (host/sound.cpp). The game plays music through LS3D's IShow (IGraph's slot 0 CreateIShow; its slots,
    // stdcall: +0x04 Release, +0x0C Open(file, flags) (an .ogg or .mp3 streamed), +0x10 Close, +0x14 Play, +0x18 Stop,
    // +0x2C GetEvent, +0x30 SetCurrentPos, +0x34 GetCurrentPos, +0x38 SetVolume(float), +0x3C GetVolume), every
    // volume times musicVolume (a float: the options' music volume, as C_game::IShowSetVolume multiplies it).
    // cityMusicPaused: C_game::CityMusicPaused, thiscall(C_game*, bool), ret 4: the city's music (C_game::Tick's
    // sounds\music\city_music_<area>.ogg) faded out and kept off while true (C_game +0x2FC0).
    uintptr_t musicVolume, cityMusicPaused;
    // The player's control (host/control.cpp): playerLockControls, C_player::LockControls, thiscall(C_player*, bool),
    // ret 4; humanDoBreath, C_human::Do_Breath, thiscall(C_human*): what the scripts' PLAYER_LOCKCONTROLS calls, in
    // that order, on the player.
    uintptr_t playerLockControls, humanDoBreath;
    // A car's body as a mod steps it (host/control.cpp): vehMatrixPrev, the step before's body matrix (and its inverse
    // 0x40 after it), into which C_Vehicle::Engine copies vehMatrix and its inverse before its substeps. 1.2 keeps it
    // after vehMatrix's pair, 1.0 at the C_Vehicle's start (both read in each build's Engine).
    int vehMatrixPrev;
    // The HUD's own lines and marks (host/interact.cpp, host/hud.cpp), thiscall on the HUD (`hud`):
    // hudHelpAdd, G_IndicatorsClass::MissionHelpAdd(text id) ret 4 -> ms it shows: a line of the game's help (the
    // mission's hints above the crosshair, wrapped; 2500 ms + 40 a character); hudConsoleAdd, ConsoleAddText(const char *,
    // argb) ret 8: a line of the console (bottom left); hudCompassSet, CompassSetDestination(frame) ret 4: the compass'
    // target (a frame; null: none); hudRadarAdd, RadarAddObject(frame, argb) ret 8 and hudRadarRemove,
    // RadarRemoveObject(frame) ret 4: a mark on the radar (the map's corner).
    uintptr_t hudHelpAdd, hudConsoleAdd, hudCompassSet, hudRadarAdd, hudRadarRemove;
    // The using objects (host/interact.cpp): what the player's Action finds on foot (doors, cars' seats, switches,
    // bodies, weapons on the ground). A registry at C_game + 0x29EC (both builds) of S_using_object* (0x38 bytes:
    // +0x00 position, +0x0C the range squared (horizontal; 1.5 m up or down), +0x10 a world matrix the position is
    // in (null: the world's), +0x14 flags (1: freed by DelObject, 2: its frame +0x18 released), +0x20 an owner frame
    // (0: any), +0x28 the S_GameItem the menu shows: {1, the action's id (a text id: its line), 0, the record}).
    // uoAdd, C_using_object::AddObject, thiscall(registry, record) ret 4; uoDel, DelObject, likewise; uoFind,
    // FindNearObjects, thiscall(registry, const S_vector *at, const S_vector *dir, vector<S_GameItem> *out, frame)
    // ret 0x10: the items in reach, nearest first, which C_player::Do_AB_OwnerNULL hands to the action menu;
    // menuExecute, GM_Menu::ExecuteMenu, fastcall(menu, bool tick, int) ret 4: the modal menu's loop, its result
    // (the action menu's: 0xA9 + the item's index, 0xA8 take them all, other: nothing chosen).
    uintptr_t uoAdd, uoDel, uoFind, menuExecute;
    // The HUD's parts shown (host/hud.cpp): hudAddFlag, G_IndicatorsClass::AddFlag(flags), and hudRemoveFlag,
    // RemoveFlag(flags), thiscall(hud) ret 4: or / and-not into the HUD's flags at hud + hudFlags. 0x80: the compass
    // (the scripts' SETCOMPASS sets it with the compass' target, removes it with none); 0x80000: the radar, which
    // DrawAll draws from the mission's radar map (where a mission has none, a null is read: never set it).
    // hudCompassTarget: hud + this, the compass' target (the I3D_frame CompassSetDestination writes).
    uintptr_t hudAddFlag, hudRemoveFlag;
    int hudFlags, hudCompassTarget;
    // The game's saves (host/saves.cpp), thiscall on the profile manager (`loadSave`, G_LoadSaveClass), whose
    // +0x28 (saveProfile) is the profile's number and +0x7C (saveSlot) the save's: savegame\mafia%03d.%03d.
    // saveGameSave, SaveGameSave(bool quick) ret 4: writes that save; saveGameLoad, SaveGameLoad(): reads it (the
    // mission opened and its actors restored inside); saveGameNew, SaveGameNewGame(): a new game's or a freeride's
    // state cleared; profileDelete, ProfileDelete(index) ret 4: the index-th profile's .sav and every save of it
    // deleted.
    uintptr_t saveGameSave, saveGameLoad, saveGameNew, profileDelete;
    int saveProfile, saveSlot;
    // People's animations (host/humans.cpp), thiscall on a C_human: humanPlayAnim, C_human::Do_PlayAnim(const char
    // *name, bool simple, bool load) -> its length in ms (the scripts' PERSON_PLAYANIM: the animations machine's
    // SetAnim with a cross-fade; anims\<name>; the human's state +0x40C becomes 4 and goes back to 1 at the end);
    // humanStopAnim, Do_StopAnim() (PERSON_STOPANIM: the state back to 1, his own animation); humanChangeAnim,
    // ChangeAnim(const char *own, const char *with) (HUMAN_CHANGEANIM: one of his own animations played as another;
    // with null: his own again). animTable: the game's animations, animCount records {const char *file, int number}
    // (the number C_human +0x70 plays, +0x74 is asked for, and the action queue's entries take).
    uintptr_t humanPlayAnim, humanStopAnim, humanChangeAnim, animTable;
    int animCount;
    // humanAnimMachine: C_human + this, his animations machine (C_animations_machine_de_luxe; Do_PlayAnim's SetAnim
    // and GetActiveAnimTime are called on it). animTime, GetActiveAnimTime(int *a, int *b) ret 8: the active
    // animation's time and length in ms (Do_PlayAnim returns the second as the length).
    uintptr_t animTime;
    int humanAnimMachine;
    // A model's own animation (host/props.cpp; 0: not known), as the scripts' MODEL_PLAYANIM plays one on a frame of
    // type 9: animOpen, the animation cache's Open (C_I3D_cache<I3D_animation_set>::Open, thiscall(animCache, const
    // char *file, I3D_animation_set **out, 0, 0) ret 0x10: anims\<file>, a reference of its own handed out; the set's
    // vtable +0 Release, +8 its length in ms); then the model's vtable +0x60 SetAnimation, stdcall(model, set,
    // channel, float weight, mode) (0 the set's own mode, 2 looped: what the scripts set; anything else plays once)
    // and missionAddAnim (C_mission::AddAnimModel, thiscall(mission, model, bool cleared with the mission) ret 8:
    // the mission's tick then calls the model's +0x14 Tick(ms) after the scene's). MODEL_STOPANIM: +0x68
    // StopAnimation(model, channel) and missionDelAnim (DelAnimModel, thiscall(mission, model) ret 4).
    uintptr_t animCache, animOpen, missionAddAnim, missionDelAnim;
    // A model made a physical object, as the scripts' CREATE_PHYSICALOBJECT does: physCtor (C_public_physics's,
    // thiscall on gameNew(physSize)), its vtable +0x48 Init(frame) -> bool (+0x44 Release when it fails), physInitBox
    // (C_PhThing::InitBox, thiscall(phys, const float block[5]) ret 4: {mass (1 at least), velocity x, y, z, 0.45}:
    // a box from the frame's bounds), then gameAddTemp; its GameInit puts the box into the collision's dynamic
    // objects. physImpulse: C_PhThing::ExternalImpuls, thiscall(phys, const S_vector *impulse, const S_vector *at)
    // ret 8. DESTROY_PHYSICALOBJECT takes it out with gameRemoveTemp. The actor releases its frame in its destructor
    // when its byte +0x6C is set (C_actor::Init clears it).
    uintptr_t physCtor, physInitBox, physImpulse;
    int physSize;
    // The game's camera (host/gamecam.cpp; st0rm94's structs/camera.md, read in both builds): G_Camera lives in C_game
    // at gameCamera (+0x04 the I3D_camera it drives, +0x0C the car it follows, +0x10 its mode). cameraSetMode:
    // G_Camera::SetMode, thiscall(camera, unsigned mode, bool begin) ret 8 (End, the mode, Begin: what the camera-change
    // key does). cameraModeTable: the 17 records of 0x44 bytes G_Camera::Begin scans ({mode, restore mode, 10 floats,
    // its name[16] at +0x30, flags at +0x40}: "P Behind", "C Chase"...). cameraSetCar: G_Camera::SetCar,
    // thiscall(camera, C_car *car) ret 4: End, +0x0C the car, the mode the game last used in a vehicle (none: the last
    // on foot), Begin, the next pose cut (+0x64): what getting into a car does for the player, and the race's
    // cameras for the other racers (C_racing::SetRCamIndex).
    uintptr_t cameraSetMode, cameraModeTable, cameraSetCar;
    int gameCamera;
    // The airplane (host/control.cpp; st0rm94's structs/airplane.md, its layout the same in both builds: every field
    // AI and Update touch read in both): C_airplane, actor type 0x16, made only from a scene's placement (the
    // freeride Extreme's "Letadlo" and "UfoTalir", MISE10 and MISE16). planeAI: its vtable slot 13 (+0x34),
    // thiscall(plane, unsigned ms) ret 4: when its flight state (+0xB4: 0 idle, 1 its route, 2 the take-off run) is
    // not 0, the route's flight (C_airplane::AI_fly: the position +0x24 moved along the facing +0x30 times the speed
    // +0xB8, the rotation +0xD0 a slerp of the nodes'), the speed's ramps and the engine's sound; then always the
    // propellers, the sounds' fades and the damage's gauge. planeUpdate: slot 14 (+0x38), thiscall(plane, unsigned ms)
    // ret 4: the model frame put at +0x24 with the rotation +0xD0; when the flight state is not 0 the six collision
    // proxies follow it (g_collision::DynUpdate); people near its engines are struck.
    uintptr_t planeAI, planeUpdate;
    // Orders for people (host/orders.cpp; st0rm94's structs/entity.md, its layout the same in both builds):
    // entityForceScript, C_entity::ForceScript, thiscall(entity, bool keep) -> bool: with keep false, the entity's
    // AI hands it to its own script: SetEvent(its program at +0xA9C, 0xEA63) jumps the program to its `event 60003`
    // and the program's callback 0 sets the flag 0x200 (+0xF08) under which C_entity::AI runs the program and its
    // scriptTick; the AI state +0xF40 is 0 again and its actions cleared. The script's own commands (ENEMY_*, its
    // vtable's DecodeInstruction) are compiled into that program.
    uintptr_t entityForceScript;
    // The road network (host/world.cpp world.road, road_near; game.h kMissionRoads): roadsNearNode,
    // C_roads::GetNearNode, thiscall(C_roads*, float x, float y, float z) -> unsigned short, ret 0xC: the road node
    // nearest to a point (0xFFFF none), an index into the C_roads +0x18 vector (0x18-byte nodes sorted by x, the
    // position first; the search is a binary one on x), what C_sledovacka::AddCarMoveTo (ENEMY_CAR_MOVETO) plans a
    // drive between.
    uintptr_t roadsNearNode;
    // The game's HUD (host/hud.cpp hud.game, hud_game): hudDrawAll, G_IndicatorsClass::DrawAll, thiscall(hud) -> int,
    // the whole HUD drawn (the radar, the compass, the health, the ammo, the texts); the 2D callback (draw2d) calls it
    // first at stage 2, before the open menu and the console, and 1.0's menu scenes' callback too. Not called: no HUD
    // that frame, the menus and the console as they were (pet1kk's developer tools hide the HUD so, 2026-10-18).
    uintptr_t hudDrawAll;
    // A person's jump (mods/examples/devtools): humanJump, C_human::Do_Jump, thiscall(C_human*) -> bool, plain ret;
    // the player's jump key calls it (1.2 0x590098). Its first test is the person's +0x6F8 (both builds).
    uintptr_t humanJump;
    // The surfaces' table (host/collide.cpp world.materials, material_name): tables\materials.def read whole by
    // LoadMaterialsFromBin (1.2 0x60F5C0, 1.0 0x592080) into a block the game keeps a pointer to at `materials`, its
    // records' count at `materialCount`; a record is kMaterialSize bytes, its name the first 0x20 (the id is the
    // record's index: a static collision face's material, MmlHit's flags >> 24). Read in both builds' code.
    uintptr_t materials, materialCount;
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
    // Input and the frame (host/input.cpp, host/screenshot.cpp; 0: not known). keyboardUpdate: stdcall(IGraph*)
    // ret 4, the keyboard read once a frame (GetDeviceState into keyboardState, 256 bytes, 0x80 a key held; the
    // previous frame's 0x188 bytes further). d3dDevice: the IDirect3DDevice8* the driver renders with.
    uintptr_t keyboardUpdate, keyboardState, d3dDevice;
    // mouseUpdate: stdcall(IGraph*) ret 4, the mouse read once a frame (IGraph slot 55 in 1.2; the menus' loop and
    // the game's call it): GetDeviceState(16, mouseState), a DIMOUSESTATE {lX, lY, lZ (the wheel, 120 a notch),
    // buttons[4] (0x80 held)}, moves since the read before, in counts.
    uintptr_t mouseUpdate, mouseState;
    // The frame rate (mods/examples/fps_unlock; MafiaD3D11's A132). limiterMinJump: in frameLimit, the `jae +0x18`
    // (73 18) that skips the wait when the frame took the minimum (the main loop asks 16 ms, 83 ms the most);
    // `jmp` there (EB) never waits and returns the frame's real time, the maximum still applied. presentYield: in
    // IGraph::Present, `push 0; call <reg = Sleep>` (6A 00 FF D3 in 1.2, FF D7 in 1.0) before the device's Present;
    // `jmp +2` (EB 02) skips it.
    uintptr_t limiterMinJump, presentYield;
    // Quaternions (exports of LS3DF, S_quat = {w, x, y, z}; mods/examples/fps_unlock, knowledge/notes/
    // vehicle_physics.md). quatMake: S_quat::Make(axis, angle), stdcall(q, const S_vector *axis, float angle) ret 0xC:
    // w = cos(a/2), the axis times sin(a/2), normalized. quatRotationMatrix: S_quat::RotationMatrix, stdcall(const
    // S_quat *q, S_matrix *out) -> out, ret 8. matrixSetRot3: S_matrix::SetRot3, stdcall(S_matrix *m, const S_quat *q)
    // ret 8, the 3x3 part. Both give the identity when |w| is at least 1 (RotationMatrix) or 0.99999999 (SetRot3):
    // for a float that is w == 1, which a turn under about 0.00049 rad has, though its vector part is not zero, so
    // such a turn is lost whole (game.h QuatMatrix builds it).
    uintptr_t quatMake, quatRotationMatrix, matrixSetRot3;
    // The scene drawn from another camera (host/view.cpp; 0: not known). The layouts are the same in both builds
    // (read in each build's Render, the camera's constructor, update and setters). cameraVtable: I3D_camera's (its
    // update +0x44 thiscall: the view, the frustum, the sector from the world matrix; SetFOV +0x50, SetNearFOV +0x54,
    // SetRange +0x58, SetAspect +0x68, stdcall(this, ...): each works the projections out again). sceneVtable:
    // I3D_scene's; its +0xB4 is sceneGetSector, stdcall(scene, const S_vector *pos, sector *hint) -> the sector a
    // point is in (the camera's update asks it for its eye, cam + 0x40). queueVisual: thiscall(visual), a visual
    // found visible queued for drawing; a lens flare ('LENS' at the visual's +0x1D4) goes onto the scene's list at
    // +0x288 (count +0x290), whose fades Render works out at its end.
    uintptr_t cameraVtable, sceneVtable, sceneGetSector, queueVisual;
    // The face groups LS3D draws (host/geom.cpp): a visual's m_AddFGroups (its vtable +0x68) queues a 32-byte entry
    // for each face group of its mesh's LOD (+0x00 the visual, +0x10 the face group, +0x18 its material's texture),
    // and Render calls the visual's m_RenderFGroup (vtable +0x6C), thiscall(visual, entry), ret 4: SetIndices(group
    // +0x14, ...) and DrawIndexedPrimitive(..., group +0x18 first index, group +0x08 faces). Three of them draw every
    // mesh: objects, morphs and single meshes share fgRenderObject; lit (lightmapped) objects fgRenderLit;
    // billboards fgRenderBillboard. A face group: +0x04 material, +0x08 faces, +0x0C the next group; a material:
    // +0x08 flags, +0x0C its texture (the animation's frame now); a texture: +0x1C its file name without folders.
    uintptr_t fgRenderObject, fgRenderLit, fgRenderBillboard;
    // The scene's lights (host/lights.cpp): I3D_light's vtable (written by its constructor and destructor). Its
    // slots, stdcall(light, ...), the same in both builds: +0x24 SetOn(bool), +0x50 SetLightType, +0x54
    // SetColor(r, g, b), +0x58 SetColor(const S_vector *), +0x5C SetPower(float), +0x60 SetMode, +0x64
    // SetRange(near, far), +0x68 SetCone(inner, outer). A light frame (frame type 2): +0x140 the sectors it lights
    // (count +0x148), +0x154 its type (1 point, 2 spot, 3 directional, 4 ambient, 5 fog...), +0x158 power, +0x15C
    // colour, +0x168 and +0x16C its range, +0x170 and +0x174 its cone (each setter read in both builds).
    uintptr_t lightVtable;
    // The particles (host/effects.cpp): the vtables of LS3D's particle visuals, PRTB (written by the constructors that
    // store 'PRTB' at the visual's +0x1D4: Game.exe's pooled effects, and the base of every particle) and PRTE (an
    // LS3D emitter's puff, made from PRTB's constructor). Their +0x18 is the frame's Update, stdcall(frame), called
    // through the vtable after a particle is moved (its local position +0x80); their +0x2C SetParent, stdcall(frame,
    // parent, flags): into the scene when a particle starts, to no parent when it ends.
    uintptr_t prtbVtable, prteVtable;
    // Sound (host/sound.cpp): I3D_sound's vtable, a frame of type 4 the I3D driver makes (its constructor and
    // destructor write it). Its slots, stdcall(sound, ...), the same in both builds (each one's arguments read): +0x24
    // SetOn(bool) (on: it plays), +0x50 Open(const char *file, flags, cb, ctx) (the file as given, "sounds\\x.wav"; flags 8: the pitch can change),
    // +0x54 IsPlaying(bool), +0x58 SetSoundType (1 point, 3 ambient: the menus' own sounds are ambient sounds in no
    // scene), +0x5C SetRange(inner, outer, curve, the volume at outer), +0x60 SetCone(inner, outer), +0x64
    // SetOutVol, +0x68 SetVolume, +0x6C SetLoop(bool), +0x70 SetCurrTime(ms), +0x74 SetFrequency(factor), +0x80
    // GetCurrTime, +0x84 GetPlayTime (ms), +0x88 SetOn2(bool, bool) (the scripts' STOPSOUND: off, at once).
    uintptr_t soundVtable;
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
    0x647C30, 0x4F0620, 0x4F1830, 0x6D8714, 0x60FBA0,
    0x60FB40,
    0x5EB070, 0x5EB090,
    0x6D4B00,
    0x5BD8D0, 0x6BF980, 0x603880, 0x6036D0, 0x6C4500, 0x647EE0, 0x647ED8,
    0x40C0, 0, 0x101C,
    0x5409D0,
    0x68,
    0x44A8,
    0x320, 0x59C, 0x624, 0x628, 0x610, 0x560, 0x568, 0x548, 0xC30, 0x1FA0,
    0xC2C, 0x1B4, 0x1B8,
    0x4CEB30, 0x4CEE00, 0x4CFAB0,
    0x5EA2E0, 0x5EA690, 0x5EA1F0, 0x5EAD50, 0x5EAE10, 0x5EB7D0, 0x5EB2F0, 0x627834, 0x6BD8B0, 0x6106FC,
    0x6BD8A0, 0x6BD888, 0x6C42C4,
    0xB4, 0xC4, 0xD4,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // the 1.0 menus: none
    0x30,
    // the scripts' machine (2026-10-14: the decompile's C_program, st0rm94's 1.0 names carried over)
    0x460AD0, 0x461530, 0x46C770, 0x46CCB0, 0x4616A0, 0x46CEC0, 0x624858,
    // actors made new, the tables (2026-10-14: read in ACTOR_DUPLICATE 0x474A14, DAN_INTERNAL_1 0x47863E, ACTOR_DELETE
    // 0x474BDD, G_CarDatabase::GetCarName 0x60A510, LoadMaterialsFromBin 0x60F5C0)
    0x647DD0, 0x4087E0, 0x53F7D0, 0x5A77C0, 0x5A79A0, 0x4CE960,
    0x50, 0x220C,
    0x6D4560, 0x6D4C14,
    168,
    // the menu's camera (2026-10-14: read in ExecuteMenu 0x5EBA40 and 0x5EC8A0)
    0x5EC8A0, 0x6BD83C,
    0x41E690,   // (CAR_REPAIR, case 0x178 of C_program::Process)
    0, 0,
    // a person's step (2026-10-15: read in the disassembly; the curb in 12-02 measured in the lab)
    0x57B660, 0x572B80, 0x57CB60, 0x57D0E0,
    0x40C,
    0x5D59C0, 0x5D6430,   // (the sweeps' calls at 0x57CF1F and 0x57D233)
    // a car's physics (2026-10-15: C_car::AI 0x41FA2A calls Tick; Tick's end and Move's zeroing at 0x4E6386 read)
    0x4DF9A0, 0x1A4, 0x4C8,
    // the car's physics further (2026-10-15: Engine's substep loop and wheel walk, Move's body turn at 0x4E4A55,
    // CreateActor's new 0x221C before C_car's constructor 0x41BB80)
    0x4E1CE0, 0x4E2090, 0x4E6AF0,
    0xC24, 0xC3C, 0xC38, 0x4C4, 0x221C,
    // what happens (2026-10-16: MafiaOnline's addresses, their rets read: 0x5A8E68 ret 0x2C, 0x5AB66F ret 0x20,
    // 0x422ACB ret 4; carCrash st0rm94's C_car::Was_Collision, 0x426286 ret 0x1C, seen called by BodyCollision and
    // DoWheelsCollision)
    0x5A84A0, 0x5AAE10, 0x421D60, 0x4259E0,
    // questions to the world (2026-10-16: st0rm94's and MafiaOnline's names, both read in the disassembly: ret 0x18,
    // ret 0xC)
    0x5C8770, 0x5C3D60,
    // the game's state (2026-10-16: FREERIDE_SCOREGET reads C_game +0x3640 through 0x47B6B0, FREERIDE_SCORESET calls it)
    0x5B9FE0, 0x5F88E0,   // (PlayerSetLives: C_player::Hit's call at 0x5942AD, ecx = the HUD)
    // the exhaust and the particles (2026-10-16: mafia-ragdoll's addresses; C_Vehicle::Update calls the exhaust at 0x4E1271)
    0x4DEE90, 0x63D8A4, 0x6D4C1C, 0x6234BC,
    0xC68, 0x468, 0x460, 0x454, 0x46C, 0x1F9C, 0xCE8,
    0x5B9540,   // (WEATHER_SETPARAM's call at 0x474417)
    // music (2026-10-16: C_game::IShowSetVolume 0x5B6600 multiplies by it; CityMusicPaused writes C_game +0x2FC0)
    0x6D4B10, 0x5AE1B0,
    // the player's control (2026-10-16: PLAYER_LOCKCONTROLS 0x4702C8 calls them at 0x47030B and 0x470324)
    0x595F00, 0x57D310,
    0xDE8,   // (Engine's rep movsd at 0x4E1DC0 and 0x4E1DDD)
    // the HUD's lines and marks (2026-10-16: st0rm94's names, MafiaOnline's ConsoleAddText contract; rets read)
    0x5F9A00, 0x5F9D50, 0x5FA020, 0x5FA780, 0x5FA990,
    // the using objects (2026-10-16: MafiaOnline's and st0rm94's; Do_AB_OwnerNULL's calls at 0x5947B8 and 0x594891)
    0x55DD60, 0x55DF10, 0x55E180, 0x5EBA40,
    // the HUD's flags (2026-10-16: SETCOMPASS at 0x46E1D8 calls AddFlag(0x80) and RemoveFlag(0x80))
    0x47A080, 0x47A0A0, 0x40A4, 0x4278,
    // the saves (2026-10-16: st0rm94's names; the sprintf arguments read in SaveGameSave and SaveGameLoad)
    0x606EA0, 0x606150, 0x605FF0, 0x605740, 0x28, 0x7C,
    // people's animations (2026-10-16: the calls of PERSON_PLAYANIM, PERSON_STOPANIM, HUMAN_CHANGEANIM; the table
    // found by its records, "smrt pad end vstavani.I3D" 111 among them)
    0x585E80, 0x57D2E0, 0x57DD00, 0x62E488, 576,
    0x40E890, 0xBC,
    // a model's animation and its physics (2026-10-17: MODEL_PLAYANIM 0x471E90, MODEL_STOPANIM 0x471F41,
    // CREATE_PHYSICALOBJECT 0x4741D5, their calls read)
    0x647D80, 0x4104F0, 0x53F520, 0x53F710,
    0x47C3A0, 0x45EE90, 0x460000, 0x38C,
    // the game's camera (2026-10-17: st0rm94's G_Camera::SetMode twin; the table G_Camera::Begin scans; 2026-10-18:
    // SetCar, 1.0's twin paired by its calls, the same code)
    0x5ED400, 0x638700, 0x5ED190, 0x4C,
    // the airplane (2026-10-17: slots 13 and 14 of the vtable C_airplane's constructor 0x409390 puts, 0x623318)
    0x40BC10, 0x40BEC0,
    // orders (2026-10-18: C_entity::ForceScript, MafiaReverse's name; ENEMY_FORCESCRIPT's handler 0x470590 calls it)
    0x523F80,
    // the roads (2026-10-18: C_roads::GetNearNode, MafiaReverse's name; AddCarMoveTo 0x4A1220 calls it twice)
    0x55B370,
    // the HUD's DrawAll (2026-10-18: draw2d's call at 0x5BD8E8; pet1kk's developer tools take that call)
    0x5FB060,
    // the jump (2026-10-18: MafiaReverse's C_human::Do_Jump; pet1kk's developer tools hook it)
    0x586190,
    // the surfaces' table (2026-10-18: LoadMaterialsFromBin 0x60F5C0 writes both)
    0x6D4C24, 0x6D4C28,
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
    // bullets, getting up, trams (2026-10-16: the twins of 1.2's functions, their first instructions the same;
    // bulletRay the return of collRayTest's call in the bullets' tick 0x5E47C0, pedSystemVtable where pedBulletHit
    // sits at +0x7C; the C_human offsets read in Do_ShockDown, the bullets' tick and humanBoneRayTest's callers)
    0x583570, 0x5E4CAC, 0x592350, 0x63C6D8, 0x4B1500,
    0x688, 0x690, 0x2DC,
    0x4BDC50, 0x57E7F0, 0x23C, 0x2A0, 0x40C,
    0x57EBF0,
    0x492000, 0x4E3FB0,
    // the start (2026-10-14, read in 1.0's code): no Bink logos (the AVI ones: m10CdFind); the active screen; the
    // profile manager (+4/+8 the profiles, 0x54 bytes each, as in 1.2); the profile, main and freeride screens'
    // vtables (their Ticks take the same ids as 1.2's handlers: 100 + i, 990105, 201)
    0, 0x67A4D8, 0x671FB0, 0x63DF6C, 0x63DE74, 0x63DFCC,
    // the controls and the texts (2026-10-14, read against 1.2's code and run in the 1.0 lab): controlsUpdate the
    // same code (st0rm94's twin), its object the ECX of its six calls; the key name input_binding_get_label; the
    // text database the ECX of 91 calls of GetText
    0x6F92E0, 0x5D1F30, 0x5D3180, 0x67A57C, 0x57D8B0,
    0x57D850,
    0x56F1F0, 0x56F1C0, // the menus' components: gui_menu_set_component_float / get (a control's +0x2C, the sliders' 0..1)
    0x672488,           // the mouse sensitivity: the same place in the on-foot input (0x4C77B5, 1.2's 0x58FD95)
    // The kit (drawing, the mission, the car): each address is the 1.0 twin of the 1.2 function, found by its code
    // and checked against the 1.0 files (tools/check_builds.py); none of it has been run in a 1.0 game yet.
    0x5F86C0, 0x658330, 0x5551A0, 0x554FD0, 0, 0x6F9528, 0x6F9520,
    0x90C0, 0x5000, 0x101C,
    0x5FFF30,
    0x68,
    0x94A0,
    0x39C, 0x618, 0x6A0, 0x6A4, 0x68C, 0x5DC, 0x5E4, 0x5C4, 0xCAC, 0x1F1C,
    0xCA8, 0x234, 0x238,
    0x51AAC0, 0x51AD90, 0x51BA40,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0x624934,   // the menus as 1.2's objects: none (1.0's are below); the game's operator new
    0, 0x67A4C0, 0x661C6C,          // the menus' cursor (floats in 1.0: m10Cursor), their scale (g_gui_scale_x, y), the language
                                    // (+4 of tables\lang_<ll>.def's record, read at 0x661C68 by 0x549060)
    0xB0, 0xC0, 0xD0,               // the controls' arrays
    // the 1.0 menus (2026-10-14; st0rm94's 1.0 reverse, each function read in 1.0's code)
    0x56EA70, 0x56AE50, 0x56AF10, 0x56E500, 0x672630, 0x56F390, 0x56C1D0, 0x63D584, 0x672618, 0x623FC0,
    0x48,
    // the scripts' machine (st0rm94's program_*, each read in 1.0's code)
    0x5AF2F0, 0x5AF730, 0x5BA6A0, 0x5BABD0, 0x5AF8A0, 0x5BADE0, 0x63EAB0,
    // actors made new, the tables (1.0's ACTOR_DUPLICATE 0x5C2804 and DAN_INTERNAL_1 0x5C62E5: the driver's
    // CreateFrame is its slot +0x4C here; car_database_get_car_name 0x55C540: 164 bytes a record; data_load_def_tables
    // 0x592080: the items, 188 bytes a record)
    0x6F9418, 0x448940, 0x5FED70, 0x5E3220, 0x5E3400, 0x51A920,
    0x4C, 0x214C,
    0x671EF0, 0x6EFB94,
    164,
    0, 0,   // the menu's camera: inlined into gui_menu_execute_modal 0x56CCC0, not looked for yet
    0x465760,   // (st0rm94's car_repair_position_on_ground)
    0x56D858, 0x56DABC,   // (2026-10-14: read in st0rm94's annotated decompile and the disassembly)
    // a person's step (2026-10-15: st0rm94's human_movement_intern, human_update, human_test_move_coll(_in_owner),
    // read in the disassembly: the same code and the same 0.015 as 1.2's)
    0x49BBC0, 0x492FF0, 0x49D0C0, 0x49D640,
    0x40C,
    0x590AC0, 0x591530,   // (st0rm94's collision_test_character_proxy and its twin; the sweeps' calls at 0x49D47F, 0x49D793)
    // a car's physics (2026-10-15: st0rm94's vehicle_tick_drivetrain, called by car_ai_tick 0x466AEA; the flags read
    // at its end, the step time where vehicle_substep_move 0x52E6D0 stores it and vehicle_set_brake reads it)
    0x52C3B0, 0x224, 0x544,
    // the car's physics further (2026-10-15: st0rm94's vehicle_substep_move is Engine, its loop reads +0xCA0, +0x540,
    // +0xCB4; Move 0x52EA80 turns the axes at 0x531431; CreateActor's new 0x21F4 before car_ctor 0x462AF0; 1.2's
    // offsets plus 0x7C, each read here)
    0x52E6D0, 0x52EA80, 0x5334E0,
    0xCA0, 0xCB8, 0xCB4, 0x540, 0x21F4,
    // what happens (2026-10-16: st0rm94's game_new_shoot, game_new_explosion, car_explosion; their rets read:
    // 0x5E47B8 ret 0x2C, 0x5E7065 ret 0x20, 0x4699B7 ret 4; carCrash car_on_collision_apply_damage, 0x46D513 ret 0x1C,
    // the same code as 1.2's)
    0x5E3DF0, 0x5E6760, 0x468BC0, 0x46CC40,
    // questions to the world (2026-10-16: st0rm94's collision_test_line_horizontal and
    // collision_dyn_set_next_multiple_owners, the same code as 1.2's: ret 0x18, ret 0xC)
    0x5838A0, 0x57EE90,
    // the game's state (2026-10-16: the scripts' FREERIDE_SCORESET at 0x5C6915 calls it; the same code as 1.2's)
    0x5F5C30, 0x54A9D0,   // (st0rm94's hud_set_player_health_value; its calls at 0x49C00C... with ecx = the HUD)
    // the exhaust and the particles (2026-10-16: the exhaust found by its code, 1.2's paired with it instruction by
    // instruction, every offset shifted as the C_Vehicle is; the table read by car_load_particle_effects 0x463A30)
    0x52B8A0, 0x6561FC, 0x6EFB9C, 0x63B2D0,
    0xCE4, 0x4E4, 0x4DC, 0x4D0, 0x4E8, 0x1F18, 0xD64,
    0x5F51C0,   // (st0rm94's game_apply_weather_detail_level: the same code as 1.2's)
    // music (2026-10-16: game_media_apply_volumes 0x5F2280 multiplies by it; CityMusicPaused found by 1.2's bytes,
    // called at 0x5C1615)
    0x672498, 0x5E9DF0,
    // the player's control (2026-10-16: PLAYER_LOCKCONTROLS 0x5BE0BC calls them at 0x5BE0FF and 0x5BE118)
    0x4CD890, 0x49D870,
    0x68,    // (Engine's rep movsd at 0x52E7AD and 0x52E7CA)
    // the HUD's lines and marks (2026-10-16: st0rm94's hud_mission_help_add, hud_console_push_message,
    // hud_compass_set_destination_frame, hud_radar_add_object_with_color, hud_radar_remove_object; the same code)
    0x54BB20, 0x54BE80, 0x54C150, 0x54C8A0, 0x54CAB0,
    // the using objects (2026-10-16: st0rm94's; player_do_action_button_on_foot's calls at 0x4CC1B8 and 0x4CC274)
    0x61BB30, 0x61BCE0, 0x61BF50, 0x56CCC0,
    // the HUD's flags (2026-10-16: SETCOMPASS at 0x5BC005 calls them; the flags 0x5000 further, as the HUD's fonts)
    0x424E20, 0x5C7DB0, 0x90A4, 0x9270,   // (the compass' target is not 0x5000 further: 8 less)
    // the saves (2026-10-16: st0rm94's twins; the same code and the same manager offsets as 1.2's)
    0x558ED0, 0x558180, 0x558020, 0x5577D0, 0x28, 0x7C,
    // people's animations (2026-10-16: the same three handlers' calls; the same table, its numbers the same)
    0x4A63F0, 0x49D840, 0x49E260, 0x647808, 576,
    0x5AB3A0, 0xBC,
    // a model's animation and its physics (2026-10-17: the same handlers in 1.0, 0x5BFC80, 0x5BFD31, 0x5C1FC5; the
    // physical object is 0x24 bytes larger)
    0x6F93C8, 0x47FA10, 0x5FEAC0, 0x5FECB0,
    0x4CDE20, 0x4C41A0, 0x4C53F0, 0x3B0,
    // the game's camera (2026-10-17: st0rm94's camera_set_mode and g_camera_mode_table; 2026-10-18: his
    // camera_set_car)
    0x5D4D10, 0x650450, 0x5D4AA0, 0x4C,
    // the airplane (2026-10-17: slots 13 and 14 of the vtable airplane_ctor 0x44EF50 puts, 0x63B7C0)
    0x4516E0, 0x451990,
    // orders (2026-10-18: st0rm94's entity_force_script, 1.2's twin by its callers, the same 229 bytes' code)
    0x42E8A0,
    // the roads (2026-10-18: 1.2's twin, paired exactly; AddCarMoveTo 0x4ED0B0 calls it twice)
    0x619480,
    // the HUD's DrawAll (2026-10-18: 1.2's twin by its calls; draw2d's call at 0x5F86D8, the menu scenes' at 0x56CCAF)
    0x54D180,
    // the jump (2026-10-18: 1.2's twin by its callers, the same first instructions)
    0x4A6700,
    // the surfaces' table (2026-10-18: LoadMaterialsFromBin 0x592080 writes both, the same code as 1.2's)
    0x6EFBA4, 0x6EFBA8,
};

inline const Ls3dBuild kLs3d12 = {"1.2", 0x62030, 0x1AAE0, 0x313A0, 0x603E0, 0x1C5B0, 0x72100, 0x9C538, 0x3DFF0,
                                  0x1B040, 0x1BFD0, 0x1C2A0, 0x2C000, 0x08, 0x18, 0x1C,
                                  0x5B6C0, 0x5B940, 0x1B50F8, 0x1B5124, 0x701A0,
                                  0xA93F0, 0x1C7D0,
                                  0x717F0, 0x1C5300, 0x1C597C,
                                  0x71E20, 0x1C56F0,
                                  0x72122, 0x6D592,
                                  0x2EE00, 0x2EC00, 0x2E070,
                                  0x9B6D0, 0x9C8F8, 0x4A1D0, 0x65560,
                                  0x39280, 0x28D40, 0x083B0,
                                  0x9BC88,   // (I3D_light's vtable: its constructor 0x201F0 writes it at 0x2027D)
                                  0x9C660, 0x9C5B8,   // (written at 0x3FA3B and 0x406D8, PRTE's at 0x3FB72)
                                  0x9CE78};   // (I3D_sound's: written at 0x5774F and 0x57802)
// LS3D 3.84.5 (31.7.2002): the vertex buffer has no flags word, everything after +0x04 sits 4 bytes lower.
inline const Ls3dBuild kLs3d10 = {"1.0", 0x5ED80, 0x1A8A0, 0x305B0, 0x5D070, 0x1C350, 0x6E580, 0, 0,
                                  0x1AE00, 0x1BD60, 0x1C030, 0x2B540, 0x04, 0x14, 0x18,
                                  0, 0, 0, 0, 0,
                                  0xA5628, 0x1C570,   // (2026-10-15: Render's test of the camera's serial, its calls)
                                  0x6DD40, 0x1C12A0, 0x1C167C,    // (twins by code; not run in a 1.0 game yet)
                                  0x6E2A0, 0x1C160C,
                                  0x6E5A2, 0x69DC2,
                                  0x2E340, 0x2E140, 0x2D5B0,
                                  // (2026-10-15: the camera's constructor 0xAA20, the vtable holding GetSector,
                                  // Render's calls of QueueVisual)
                                  0x97690, 0x98898, 0x47D20, 0x622D0,
                                  0x37940, 0x28670, 0x07E50,   // (the vtables +0x6C of OBJ_, LMAP, billboards)
                                  0x97C28,   // (I3D_light's vtable: its slots' code the same as 1.2's, written at 0x2002D)
                                  0x98600, 0x98558,   // (written at 0x3DDFB and 0x3EB08, PRTE's at 0x3DF22)
                                  0x98E10};   // (I3D_sound's: its slots' code the same as 1.2's, written at 0x5486F)

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
