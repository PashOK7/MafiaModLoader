# Game.exe and LS3DF.dll: findings

Facts about the 1.2 game that took work to find. Each says how it is known: **game** (seen in the running game),
**code** (read in the decompile or disassembly only). Function names are the decompile's where it has them; look
any of them up with `kb_function`.

## Starting, menus, missions, saves

- **code** `C_mission::Open` (0x5409D0, thiscall `(this, const char *name, bool, uint, bool)`) opens a mission by its
  folder name: `missions\<name>\scene.i3d`, `cache.bin`, `road.bin`, `check.bin`. The main menu is the mission
  `00menu`.
- **code** The start-up path (0x5BEBE0) plays `intro`, then `tutorial`, then opens `00menu`.
- **game** The intro videos (Bink, `binkw32.dll`) run before the frame loop: no frames meanwhile (the loader's mods
  load after them).
- **code** Profiles are `savegame\mafia%03d.sav`, saves `savegame\mafia%03d.%03d` (code 0x605260..0x606150,
  `G_LoadSaveClass`). The freeride menu's image is `freeride0.bmp` (0x5E1920). Racing records: `savegame\mr%03u.sav`,
  `savegame\mrtimes.sav`, `records\%s`.
- **game** From the launch to the freeride (recorded with `tap` while a person clicked, then driven by the loader's
  start, `host/start.cpp`):
  1. `InitSystem` (0x5BF5A0) plays `logo1`, `logo3`, `logo2` with `PlayVideo` (0x55D460), then
     `G_LoadSaveClass::Init` (0x604EE0): the profiles enumerated (`ProfileEnumFiles`), `00menu` opened, the profile
     menu (vtable 0x6279DC) run by `GM_Menu::ExecuteMenu` (0x5EBA40): 16 a profile taken, 15 quit. The profile
     taken is `G_LoadSaveClass` (0x6D4628) `+0x28` (its number) and `+0x30` (its name); `ProfileLoad` reads it.
  2. `MafiaEntryPoint`'s loop runs the main menu (vtable 0x625EB8) and switches on what it returns: 17 tutorial,
     18 new game (`intro`, then `mise01`), 19 a saved game (`SaveGameLoad`), 21 racing, 27 `extreme`,
     28 `freeitaly` (the freeride the menu starts with its defaults), 29 `freeride`, 30 `freeridenoc`,
     31 `freekrajina` (the case labels in the decompile are guesses: 28 is the default freeride).
  3. Freeride: `C_mission::Open` (the folder name) -> `SaveGameNewGame` -> `FreerideSetup` -> `C_game::Init`.
- **game** The menus: `GM_Menu::ExecuteMenu` (ECX the menu, DL a flag, one stack argument, `ret 4`) loops (frames,
  input, `C_mission::Tick`) until the result 0x6BD8A8 is set, which `GM_Menu::MenuCloseAll(menu, -1, result)` does;
  then it destroys the menu and returns the result. The active menu is 0x6BD890 (a submenu's `+4` is the one under
  it, `GM_Menu::Push` 0x5EAD50). A menu's command handler, what a click calls, is its vtable's `+0x24`,
  thiscall(menu, command): the profile menu's 100 + i (the i-th profile of the list `G_LoadSaveClass +4/+8`,
  records of 0x54 bytes: `+8` number, `+0x10` name), the main menu's 990105 (the freeride menu, vtable 0x62736C),
  the freeride menu's 201 (start; its `+0x30` the kind, `+0x34` the car, 23 by default). The loader's frame (LS3D's
  frame limiter) runs inside this loop, so commands sent from it are handled as a click's.
- **game** `C_mission` (a static object, `[0x63788C]` = 0x647DF8 in 1.2): `+0x10` its scene, `+0x24` its `C_game`,
  `+0x68` its name, a `char*` (`"freeitaly"`). `C_mission::Open` (`ret 0x10`) calls `C_mission::Close` itself
  before it loads; the loading is over at the mission's next `Tick` (2.6 s after the open for the freeride). The
  loader's `MISSION_OPEN` / `MISSION_LOADED` events hang on these two.
- **game** The pause: `GM_Menu::ExecuteMenu`'s loop calls `C_mission::Tick` (the call at 0x5EBC1E) only when its
  caller asked for it (a byte argument); otherwise it calls 0x5ED4C0 on `C_game + 0x4C`. The main menu's loop
  ticks (its mission `00menu` runs under it); the pause menu's (vtable 0x625E58, made in the game's loop on Esc)
  does not: the mission's tick count stood still for 278 frames in it. `C_game + 0x40` (a byte: the game runs)
  stays 1 in the pause; `g_uUIFlags` 0x6C3A24 goes to 1 while a menu runs (0x8000024 in the game).
- **game** The game's loop (`GameLoopSingle1` 0x5BE750), one turn: `IGraph` slots 11, 55, 38 (the keyboard's read),
  39, 64 (the frame limiter: the loader's frame), the controls' update, `C_mission::Tick`, the scene's render with
  the 2D callback (`knowledge/notes/interface.md`), `IGraph` slot 15 (present).
- **code** Mission scripts: `C_program::DecodeParams` 0x461C6C, `DecodeInstruction` 0x463960, `Process` 0x46D3B0
  (the script commands' strings: `HUMAN_*`, `PLAYER_*`, `DETECTOR_*`...).

- **game** The freeride menu (`GM_FreeRide`, vtable 0x62736C): `OnCreate` 0x5E1920 sets its sliders 21 (traffic
  density), 22 (pedestrian density), 23 (police patrols density) to 1.0 with 0x5EB070 (`thiscall(menu, id, float)`:
  `GM_Menu::FindComponentByID` 0x5EB2F0, then the component's vtable `+0x4C`); the start (command 201) reads them
  with 0x5EB090 (`+0x50`) and the checks 25, 26, 27 (`+0x60`: flags 1, 2, 0x100) into `FreerideStoreA` 0x60E920.
  Seen in the menu after setting each (2026-10-13): 23 at 0 is a city without patrols. With `loadSave + 0x1F0`
  (the freeride's opened level) below 3 the menu hides the sliders and the game ignores the values (0x60E980).

## Controls and input

- **game** One controls object, 0x647C30. `+0xB4/+0xB8`: the binding slots, 12 bytes each `{u16 direction (an
  axis' half: 1, 2), u16 0, u16 device (0 none, 1 key, 2 mouse, 3 joystick), u16 code (a DirectInput key; mouse:
  button 1, 2 or axis 0x100 X, 0x200 Y, 0x300 Z), u32 joystick}`; 60 slots in 1.2. The profile's copy of them:
  `G_LoadSaveClass + 0x1F4` (0x6D481C). `+0xC4`: a float per slot, "held"; `+0xD4`: "pressed this frame".
  `+0xA4..+0xAC`: the mouse axes' ranges (400 each, set by 0x4EFDB0): an axis' held value is its counts / 400.
- **game** Its update, 0x4F0620 (`thiscall(controls, char)`, `ret 4`), fills both arrays from the devices once a
  frame: called by the game's loop (0x5BE80B, before `C_mission::Tick`) and by the menus' loop
  (`GM_Menu::ExecuteMenu`). After a cheat's character the loop zeroes both arrays. The readers take the arrays
  (0x4F01A0) or one value (0x4F0240) and index by slot: `C_player::AI` 0x5937B0 (58 change camera, 30 objectives,
  10/11 action), its on-foot part 0x58F4B0, its driving part 0x5917F0. Writing a slot after the update presses
  the action whatever it is bound to (the loader's `input.act`).
- **game** The slots. On foot, two each (primary, secondary): 0 forwards, 2 backwards, 4 left, 6 right (steps
  aside), 8 walk/run, 10 action, 12 fire, 14 crouch, 16 jump; 18 and 19 the mouse axes X and Y (the menu's one row
  "Aiming"); 20 cycle up, 22 cycle down; then one each: 24 inventory, 25 hide weapon, 26 drop weapon, 27 toggle
  run/walk, 28 reload, 29 sniper mode, 30 objectives, 31 city map. 32: read by nothing found. Driving, two each:
  33 accelerate, 35 brake/reverse, 37 steer left, 39 steer right, 41 handbrake, 43 speed limiter, 45 gear up,
  47 gear down, 49 horn, 51 look left, 53 look right, 55 clutch; one each: 57 manual/auto gearing, 58 change
  camera, 59 replace car (racing). The menu's texts: 301..325 and 330..344 (`G_TextDatabase` 0x6D8714,
  `GetTextOrNULL` 0x60FBA0).
- **game** On foot the player turns by `mouse sensitivity (float 0x6D4B00) * 5 * held[18]` radians an update
  (0x58F4B0). Measured: 141.3 degrees for a sum of 1 at sensitivity 0.4932, linear from 0.005 to 0.1 an update;
  right is positive, and on the Y axis down is positive. A value of 1 is 400 counts in one frame.
- **game** `Action` in a car (0x594FA0, from `C_player::AI` when "pressed" of slot 10 or 11 is 0.25 or more): with
  the car's speed (`car + 0x60C`) 0.5 or more, nothing; with the engine on (`car + 0xC9C`), the engine stopped
  (`C_Vehicle::SetEngineOn(0)`) and nothing else; otherwise the player gets out. In a race, nothing.
- **game** LS3D reads the keyboard once a frame (`ls3d+0x717F0`, `stdcall(IGraph*)`): the 256-byte DirectInput
  state at `ls3d+0x1C5300` (0x80: held), the previous frame's copy at `ls3d+0x1C5488`. A key set there after the
  read is pressed for every reader (the menus: `Esc` opens and closes the pause menu).
- **game** The freeride's player starts with a Thompson in his hands and a second long weapon: `Hide Weapon` has
  nowhere to put it, and a police patrol that sees it opens fire.
- **game** The device LS3D renders with: `ls3d+0x1C597C` (an `IDirect3DDevice8*`; under dxwrapper it answers
  `QueryInterface(IDirect3DDevice9)`). The lab's back buffer is multisampled.

## Cars

A `C_car` is an actor of type 4; its rigid body, a `C_Vehicle`, is inside it at `C_car + 0x70`. Offsets below are
the `C_Vehicle`'s, 1.2 first and 1.0 after the slash (1.0's are the twins of the 1.2 code, checked against the 1.0
file by `tools/check_builds.py`, not run). They are in `game_build.h` (`veh*`) and by name in `game.h` (`Car`).
**game**: watched in the running 1.2 game (2026-10-13, the freeride's taxi driven by `input`); **code**: read in
the function named.

- **game** The car the player sits in: `C_human + 0x98` (the player's; an actor of type 4, or a tram). The car he
  drives or drove last: the HUD's, `0x6BF980 + 0x44A8` (`Game.exe+0x2C3E28`; 1.0 `0x658330 + 0x94A0`,
  `Game.exe+0x2617D0`), set by `G_IndicatorsClass::RadarSetPlayerCar` 0x5FA500 (1.0 0x54C620).
- **game** Position, world metres: `+0x320` / `+0x39C` (`C_Vehicle::CalcSpeed` 0x4CDE90 copies the car frame's
  translation there every tick). The car frame: `+0x34C` / `+0x3C8`. Velocity, m/s: `+0x1F90` / `+0x1F0C`; the
  angular one `+0x420` / `+0x49C`.
- **game** Speed: `+0x59C` / `+0x618`, the velocity's length in m/s (`CalcSpeed`; what `Action` compares with 0.5).
  Driving: 8.906 there while the HUD's own number (`0x6C3AF4`) said 32.08: the HUD's is km/h, the dial shows mph.
- **game** Steering: the angle asked for `+0x624` / `+0x6A0` (`C_Vehicle::SetSteer` 0x4CB4D0: the input -1..1,
  shaped by the linearity `+0x198`, times the limit), the wheels' angle now `+0x628` / `+0x6A4` (it follows over a
  few frames: -0.289, -0.658, -0.707 after the wanted one jumped to -0.696; `SaveVehicleState` saves it), the limit
  `+0x610` / `+0x68C`, radians, shrinking with speed (0.785 standing, 0.696 at 32 km/h). Left is negative.
- **game** Gear: `+0x560` / `+0x5DC` (-1 reverse, 0 neutral, 1...), the highest `+0x568` / `+0x5E4`
  (`C_Vehicle::SetGear` 0x4CB070). Seen 1, 2 and -1.
- **game** Engine: turns a second `+0x548` / `+0x5C4` (`C_Vehicle::GetRotations` 0x4CD590 returns it times 60 when
  the engine is on; 13.33 idling: 800 rpm), on: a byte `+0xC2C` / `+0xCA8` (`car + 0xC9C` in 1.2, what `Action`
  tests; `C_Vehicle::SetEngineOn` 0x4CB5B0).
- **game** Fuel, litres: `+0xC30` / `+0xCAC`, the tank's size `+0x1FA0` / `+0x1F1C` (`C_Vehicle::SetFuel`
  0x4CB6E0). The taxi: 59.993 of 60, falling as it drove.
- **game** The engine's health: `+0x1B4` / `+0x234`, of `+0x1B8` / `+0x238` (100 of 100 new; 86 after a tree at
  30 km/h; a light touch of another car changed nothing). The damage function 0x4D6720 lowers it by more than the
  engine's "resistance"; at 0 the engine stops. `+0x1B0`: the health as a 0..1 factor on the throttle. The
  gearbox's health: `+0x1EC` (**code**). The HUD's damage figure is one of the three (gearbox, engine, fuel tank)
  over its maximum (0x429450, **code**).
- **game** A car's whole state, the game's own way (its saved games do this for every car):
  `C_Vehicle::GetVehicleStateSize` 0x4CEB30 (`thiscall` -> bytes; 1525 for the taxi: it holds the body's dents, so
  it differs by model), `SaveVehicleState` 0x4CEE00 (`thiscall(vehicle, buffer)`, `ret 4`; the buffer starts `{u32
  9, u32 size, position, rotation, velocity, angular velocity, ...}`), `LoadVehicleState` 0x4CFAB0
  (`thiscall(vehicle, buffer) -> bool`, `ret 4`; refuses a version other than 9, deactivates the vehicle, then
  sets everything). 1.0: 0x51AAC0, 0x51AD90, 0x51BA40. Tried in the lab: saved, driven away in reverse round a
  corner, loaded while rolling: position, axes, gear, engine speed and fuel back to the last digit, the car drove
  on. `game::CarStateSave` / `CarStateLoad`, the core's `car.save` / `car.restore`.
- **game** A state's size grows with the body's dents (`GetVehicleStateSize` counts the dented vertices of each
  body part): the freeride taxi is 1237 bytes whole, 1285 after a scrape, 1437 after a crash. The load (read
  2026-10-14) checks only the version; after the fixed head (`u32` counts at +352 lights, +356 body parts, +360
  seats, +364 wheels) each part's record is found by its code (`CodeIndexDan`) and writes only the vertices it lists
  (the original vertex plus a 6-bit delta times 0.02 per axis); the other vertices stay as they are. So a state with
  fewer dents leaves the car's extra dents unless the car is repaired first: `C_car::RepairPosition(car, true)`
  0x41E690 (1.0 0x465760), what the scripts' CAR_REPAIR calls (it calls `C_car::Reset`). Seen: repair, then load,
  gives the saved size back exactly.

### What older tools said (TrackMafia, Telemetry2, LiveDelta)

Racing tools made before this repository (a ghost car replayed from recorded laps, lap telemetry, an
autosplitter; kept outside it, not ported). Checked against the above:

- **game** (1.2), **code** (1.0) Right: the player-car pointer, the position (`car + 0x390` / `+0x40C`), the
  steering (`car + 0x698` / `+0x714`: the wheels' angle), and for 1.0 the engine health (`car + 0x2A4`) and the
  fuel (`car + 0xD1C`): each is the field above, the 1.0 ones exactly the twins found by matching code.
- **game** Wrong for 1.2: its speed (`car + 0x594`, "60 of it is 40 mph"). That float stayed 13.333 while the car
  went from 0 to 32 km/h (it is the engine's idle turns a second). Use `+0x59C` of the vehicle.
- **code** Its recipe for a car's state (the object is 0x2220 bytes; copy `0..0xED8` and `0x2000..0x21B4`, keep the
  target's own values at about 130 listed pointer fields, zero two ranges; other numbers for 1.0) works by leaving
  out every pointer somebody found by hand. Not offered by the SDK: the lists are per build and only as complete
  as the crashes that built them, and the game's own `SaveVehicleState` / `LoadVehicleState` do the job knowing
  the layout. What the raw copy carries and the game's functions may not (unchecked): whatever is not in a saved
  game, if a ghost car needs it.
- **tool** Not checked: the racing ghost car `[[Game.exe+0x247E30] + 0x88]` (1.2), `[[Game.exe+0x255360] + 0x88]`
  (1.0).
- **tool** 1.0 only (LiveDelta), not checked: the mission's name through `Game.exe+0x2F94A8` (ours, both builds:
  `C_mission + 0x68`, a `char*`; `C_mission` is `[0x63788C]` / `[0x65115C]`); loading flags `+0x272584`,
  `+0x2F94BC`, and `[+0x2F9464] + 0x40`; the player's position `[[[+0x2F9464] + 0x54] + 0x68] + 0x40`; the police
  factor `+0x2552D0`; a damage site at `+0x967A9` (the enemy's health at `+0x644`); a car explosion site at
  `+0x67E28` (shots taken and their limit at `car + 0x2094`, `+0x2098`).

## People

- **game** An actor's position is in its model's parent's space (the world in the street, the tram's frame for
  someone in a tram). The world position is the model frame's world matrix (`+0x10`, translation `+0x40`).
- **game** Standing on a tram: `humanPlatform` 0x571B90, thiscall `(human, bool in)`; the tram's car: `tramCarOf`
  0x492D60.
- **code** `C_human` keeps its collision objects at `+0x688`, `+0x690`, `+0x694` (removed by 0x575F60, added by
  0x575FC0).
- **game** `C_human::Hit` 0x5762A0. A car knocks a person down only when the relative speed squared exceeds 9
  (constant at 0x624058); a pedestrian hit (type 5) knocks down fully above strength 16 (0x624078). Falling and
  landing damage is hit type 6 (from 0x57B660).

## The start's other ways (host/start.cpp; read 2026-10-14, run in both labs)

- **code** `MafiaEntryPoint`'s switch on the main menu's result (the jump table at 0x5BF4C4, result − 17) opens:
  17 `tutorial`, 18 `intro` then `mise01` (a new game), 19 the save selected (`SaveGameLoad`), 27 `extreme`,
  28 `freeitaly`, 29 `freeride`, 30 `freeridenoc`, 31 `freekrajina`, 32 `freekrajinanoc` (read: each case's
  `C_mission::Open` argument). The notes' earlier "guesses" were right for 27..31; 32 is the countryside at night.
- **game** The freeride menu's command 100 opens its kind menu (`freeridetrack`, vtable 0x6274C4): rows 401 City -
  Small (28), 402 City - Night (30), 403 Countryside - Night (32), 404 City - Daytime (29), 405 Countryside - Daytime
  (31); its handler (0x5E1EC0) closes it with that number, which the freeride menu keeps at `+0x30`. Its OnCreate
  hides 402..405 when the profile manager's `+0x1F0` is 0, 404 and 405 when it is 1. The start (201) of a kind other
  than 28 opens the spawn menu (`freeridestart`, vtable 0x627464): rows 101.. (City: Little Italy, Central Island,
  Down Town, Hoboken; the countryside kinds other texts, 0xFAA..), whose handler stores the row − 100
  (`FreerideStoreB`) and closes everything with the kind.
- **game** The load menu (`loadgame`, vtable 0x62739C): its OnCreate (0x5E2400) enumerates the profile's saves
  (`SaveGameEnumFiles` 0x605B20: `savegame\mafiaNNN.<n>` for each n < 600 with a text 5000 + n) into the profile
  manager's `+0x14/+0x18`, 24 bytes a save (`+8` its number, `+0x14` its text id: "17-03 Election Campaign - Old
  Prison" is 465); rows 100.. are the newest first; a row's command selects count + 99 − row (`SaveGameSelect`
  0x605E40) and closes with 19.
- **game** Starting a freeride writes the profile's `mafiaNNN.570` and `mafiaNNN.sav` (seen after every freeride
  start, both labs); loading a chapter wrote nothing until it was played.

## Actors made new, taken out (host/world.cpp; read 2026-10-14 in both builds, run in both labs)

- **code** The scripts' `ACTOR_DUPLICATE` (0x474A14; 1.0 0x5C2804): `driver->CreateFrame(9)` (the I3D driver's vtable
  `+0x50`, 1.0 `+0x4C`), the model cache's `Open(frame, file, 0, 0, 0, 0)` (0x4087E0 on 0x647DD0; 0 is success, ret
  0x18), the frame's `SetName` (`+0x28`; LS3D copies the string), `LinkTo` (`+0x2C`) to the source's parent,
  `SetPos` 0x460980, `SetRot`, `Update` (`+0x18`), `C_mission::CreateActor(type)` 0x53F7D0, the actor's
  `Init(frame)` (vtable `+0x48`; `+0x44` releases it when Init fails), the byte `actor + 0x6C` = 1 (the actor's
  destructor then releases its frame: `C_actor::~C_actor` 0x405D90), `C_game::AddTemporaryActor` 0x5A77C0 (it calls
  the actor's `+0x64`, GameInit). The frame is not released by the case: the actor owns it.
- **code** `DAN_INTERNAL_1` (0x47863E; 1.0 0x5C62E5) makes a Bolt-Thrower named "cheat_car" at a fixed place: the
  frame linked with `I3D_scene::SetFrameSectorPos` (0x47ADF0 on the mission's scene `+0x10`: the sector at its
  position), `C_car + 0x220C` (1.0 `+0x214C`) = 1 (its particles), `InternTempActAdd`, `CalmVehicle(100)` on the
  vehicle (`car + 0x70`), `actor + 0x65` = 1 (out of saved games), the frame released.
- **game** A removed temporary actor's model stays in the scene: after `RemoveTemporaryActor` (what `ACTOR_DELETE`
  calls) and the actor's destruction, its frame was still the primary sector's child and drawn where the person
  stood. The sector keeps a reference; `LinkTo(frame, null)` takes it out (the loader does it in the destructor's
  hook).
- **data** The car list: G_CarDatabase 0x6D4560 (1.0 0x671EF0), `{count, records}`, 168 bytes a record (1.0 164):
  `+0x00` the model's prefix (the file `<prefix><colour %02d>.i3d`, GetCarModel 0x60A6A0), `+0x20` its wreck,
  `+0x40` its shadow, `+0x60` its name. Record 0 is "-". 83 records in 1.2.
- **data** The items (`tables\predmety.def`): `g_pItems` 0x6D4C14 and its count after it (1.0 0x6EFB94), 188 bytes a
  record in both builds: `+0x00` the name ("Thompson 1928"; 31, a sword, is named by the one byte 0x85, "…"), `+0x20` flags,
  `+0x24` the model ("2tommy.I3D"), `+0x60` the rounds in a magazine, `+0x64` in reserve (Colt 1911 7 and 35,
  Thompson 50 and 200: what the gun shop and `human_addweapon` callers use).

## Rendering (LS3DF.dll)

- **code** `I3D_frame::SetVisibleRecMainPass` (0x10041DF0): the main visibility pass, a recursive walk with frustum
  tests against global planes; frame `+0xAC` flags, `+0xF0..+0xFC` bounding sphere, `+0x110` type, `+0x120`
  parent, `+0x124` first child, `+0x12C` next sibling, `+0x13C` traversal stamp (AntiCulling's report).
- **game** The frame limiter (`frameLimit`, `ls3d+0x72100`) is called once per frame by the main loop: the loader's
  frame event hangs on it.

## The frame rate (mods/examples/fps_unlock; read and measured 2026-10-14, 1.2 lab)

- **game** The limiter is the game's only clock: stdcall(IGraph, min, max) -> ms; it reads LS3D's millisecond clock
  (`ls3d+0x1C52B8` in 1.2, `0x1C1258` in 1.0), and when the frame was shorter than `min` it Sleeps the rest, moves
  the clock on by it and returns `min`; a longer one is cut to `max`. `GameLoopSingle1` (0x5BE750) asks (16, 83)
  and hands the answer to `C_mission::Tick`, the recordings, the cache, the map and the cutscenes; `ExecuteMenu`
  (0x5EBA40) asks (16, 249) and hands it to the mission's tick and the menu's camera. So the game never runs above
  62.5 fps. Present also yields (`Sleep(0)`) before the device's Present.
- **code** MafiaD3D11's A132 (a local checkout, `src/bridge/MafiaVR_AsiBridge.cpp`): the limiter's
  `cmp eax, edi; jae +0x18` (`limiterMinJump`, 1.2 `+0x72122`, 1.0 `+0x6E5A2`) made `jmp`, and Present's
  `push 0; call <reg = Sleep>` (`presentYield`, 1.2 `+0x6D592` with `call ebx`, 1.0 `+0x69DC2` with `call edi`)
  jumped over. The game's step is then the frame's real time in whole milliseconds: 4..6 ms at 200 fps, 0..3 ms in
  the main menu at 800..1100 fps (frames of 0 ms happen).
- **game** Measured unlocked against 62.5 fps, the same within a few per cent: the taxi's acceleration (21.4 m in 4 s
  at 224 fps; 21.5 and 21.7 m locked), Tommy running (14.90 m in 3 s at about 210 fps, 14.88 m locked). The gameplay
  camera (`G_Camera::Tick` 0x5ED4C0) scales its steps by the frame's time (cut to 83 ms).
- **game** **The main menu's camera is not**: `GM_Menu::TickCamera` (0x5EC8A0, fastcall(ms); `menuCam` 0x6BD83C:
  the look point's and the eye's velocities, targets and positions) moves the points by velocity * ms * 0.0003 but
  springs the velocities once a call: `v = 0.98 v + 0.02 (target - point)`. At 62.5 fps the velocity builds up over
  about 0.8 s; at 1000 fps the spring is gone and the camera starts at full speed. Measured on one flight (hovering
  Free Ride, then Credits): 10 / 50 / 90 % of the way at 0.86 / 2.78 / 6.53 s locked, 0.43 / 2.36 / 7.67 s at about
  1000 fps (a first-order approach at 0.3/s, as the model says); stepped by the time (`k = 1 - 0.98^(ms/16)`,
  fps_unlock's fix) 0.88 / 2.77 / 6.54 s at about 1000 fps. Each item of the main menu has its own place for the
  camera (ids 9901xx); a submenu's items do not move it (only its slow sway, already timed).
- **game** **1.0's menu camera is another one**, inlined into `gui_menu_execute_modal` (0x56CCC0), its state the
  loop's own locals: each frame 10 substeps (`mov eax, 10` at 0x56D858, the `jne` back at 0x56DAB6) move the look
  point and the eye toward a middle point each (`* 0.001`) and the middle points toward their targets (`* 0.002`),
  with no time at all (only the sway uses it). The constants are shared (0x63B348, 0.001, 218 uses; 0x63CFBC,
  0.002, 20): fps_unlock points the loop's 12 `fmul dword [const]` operands at floats of its own instead, set each
  frame to `1 - (1 - a)^(ms/16)`. The camera frame both builds' menus move: the scene's `+0x17C` (`GameCamera`).
  Measured in the 1.0 lab by that frame (its sway included, so the 90 % mark is noise): 10 / 50 % of one flight at
  0.74 / 2.66 s at 62.5 fps, 0.04 / 0.16 s at 1315 fps, 0.76 / 2.79 s fixed at 1284 fps.
- **game** **A person's step stops at curbs above 62.5 fps** (the owner's report; 2026-10-15, both builds' code, the
  1.2 lab). `C_human::MovementIntern` (0x57B660, thiscall(ms)) builds the frame's move from the speed times the time,
  adds the fall (`fall += 9.81 t`, `move.y -= fall t`; on the ground the fall restarts from 0 each frame, so a
  frame's drop is `9.81 t^2`) and hands it to the sweep `C_human::DynSetNextCollOwnerMult` (0x57CB60,
  thiscall(pos, move, out), ret 0xC; `TestMoveCollInOwner` 0x57D0E0 for one standing in a vehicle): three spheres
  (radius 0.4 at the feet + 0.405, 0.25 and 0.35 above it) swept by `TestPanak2` along the move aimed 0.015 m lower
  (`fsub [0.015]`, a constant per call), and where they touch, `out` is where the sliding sphere ends minus 0.4;
  touching nothing, `out` is `pos + move`. A curb is climbed by that slide on its edge, and the slide's direction
  depends on the step: walking (1.7 m/s) a 16 ms step is 0.027 m forward against 0.015 + 0.0025 down and slides up;
  a 2.5 ms step is 0.004 m against 0.015 and slides down: Tommy walks into the curb for good (running, 5 m/s, still
  climbs). `C_human::Update` (0x572B80, thiscall(ms)) sweeps an animation-driven move the same way (no fall in it).
  `MovementIntern` also sweeps a fixed 0.1 m probe whatever the time when a move is shorter than 0.1 m in states 1
  and 3 (`+0x40C`).
- **code** **The cause: three lengths made once a call** (2026-10-15, read in both builds' disassembly; the decompile
  agrees). The slide `TestPanak2` (`g_collision`, 0x5D59C0, 1.0 0x590AC0; thiscall(query) ret 4; the vehicle sweep's
  twin 0x5D6430, 1.0 0x591530, ret 8) is a collide-and-slide: up to 10 rounds, each sweeping the three spheres
  (`TestSphereB` 0x5D39A0) along the move, stopping at the nearest touch, and going on with the rest projected on
  the contact's plane plus 0.01 m along its normal (`fmul [0.01]`, nine: x, y, z for each sphere). In the sweep the
  sphere starts at the feet + 0.405 (`fadd [0.405]`) and the result is its centre minus 0.4 (an immediate), the move
  aimed 0.015 lower (`fsub [0.015]`); the vehicle sweep starts at + 0.4 (`fadd [0.4]`), returns minus 0.4 (`fsub
  [0.4]`, the same constant) and aims 0.015 lower. So once a call: the start 0.005 above the last result, the aim
  0.015 down, the skin 0.01 off each contact (which is also why a person stands 0.01 above the ground: everyone's y is
  the ground's + 0.01). At 16 ms they balance as 0.31, 0.94 and 0.63 m/s; at 2.5 ms they are six times as much, and a
  curb's edge (its normal tilted up, the slide's direction decided by the forward step against them) is slid down.
  Scaling the aim alone left the start's lift and the skin's push (it has an upward part on any edge below the
  sphere's centre) made once a call: Tommy climbed walls (measured again with the probe: up to 0.34 m up the shop's
  ledge). The fall on the ground is a fourth such length: the fall's speed restarts every frame there, so a frame's
  move goes down `9.81 t^2` (2.5 mm at 16 ms, 0.06 mm at 2.5 ms), a 0.16 m/s push at 62.5 fps that vanishes above.
- **probe** `fps_unlock.sweepprobe` calls the sweep as a walk would (the move `v t`, the fall `9.81 t` a step, restarted
  on a touch) from where the player stands, any step and any lengths, and puts everything back. At the owner's curb
  (12-02, Tommy from (-10.1, -0.029, -153.19) facing -z, 1.7 m/s): 16 ms climbs at 1.312 s, 8 ms barely, 4 and 2.5 ms
  never (stopped at z -155.227); all three lengths times k = ms / 16, with the feet kept 0.01 above the ground (the
  result's reference 0.39 + skin), climb at 1.316 / 1.313 / 1.312 s (4 / 2.5 / 1 ms), 3 mm and 2..3 cm from 16 ms at
  1.5 s, and agree with one another within 1 cm. Running into the shop's 0.3 m ledge (5 m/s): stopped at z -158.920
  against 16 ms's -158.912 (the smaller skin stands 8 mm nearer the wall), not climbing it. Keeping the skin at
  0.01 (only the start and the aim scaled): the wall's distance as at 16 ms, but the curb climbed 0.03 s early and 13
  cm further on, and the feet bobbing 4..8 mm on flat ground. Counting the ground's fall too (its 16 ms share added to
  the aim on a step whose move goes down exactly `9.81 t^2`): the curb about the same (running up at 0.448 / 0.450 s
  against 0.448), the stairs much nearer (below: 3..5 cm behind 16 ms at 2 s instead of 13..16 cm ahead): taken.
  Stairs in 12-02 (from (2.25, 0.01, -118.86) facing -z, running 4.6 m/s, up 2.2 m, the last step higher): 16 ms runs
  up them, 2.5 ms with the game's lengths stops at the bottom, fixed it ends where 16 ms does (2.64, 2.21, -126.37).
  The freeride's spawn curb (z 26.2, its height growing towards -x; faced from z 24.5): running, 16 ms climbs it from
  x -1982.5 to -1960 (0.19..0.23 m at the high end) and not at -1985 and beyond; walking from -1975; the fixed 2.5 ms
  climbs exactly there, the game's 2.5 ms nowhere from -1982.5 to -1970 running and -1975 to -1965 walking. Along a curb
  at 45 degrees (1.0's freeride, 11 m from the start towards +x +z) the probe's
  steps disagree by up to 1 s on when a slow grind climbs; the real game there (below) does not climb in 10 s at any
  rate, all three paths within 5 mm.
- **code** fps_unlock's fix (`fix_walk`): the 22 operands pointed at four floats of the mod's (the process's heap),
  which hold the game's values but during a call of a sweep, when they are the lengths of the call's step (k = ms /
  16; k = 1 at 16 ms and longer; the aim with the ground fall's share on a ground step of MovementIntern's: about 60 %
  of the sweeps while running about); the result's 0.4 corrected after the call; the fixed 0.1 m probe keeps the game's.
  Its previous fix (each short step asked about a 16 ms step) is gone. **game**, `fps_unlock.path` every frame,
  62.5 fps / fixed at 190..410 fps: 1.2, the owner's curb walking: up at 1.407 / 1.404 s, both at z -155.281 (unfixed:
  stopped at -155.23); running: up at 0.544 / 0.541..0.544 s, stopped by the ledge at z -158.912 / -158.920, sliding
  along it to x -10.083 / -10.084; down the curb walking at 0.768 / 0.778 s, running at 0.383 / 0.397 s. 1.0, its
  freeride's curb head on (from (-1973, -4.86, 24.5) facing +z): up between 1.2 and 1.5 s at both (y -4.828 / -4.829
  at 1.2 s, -4.686 / -4.679 at 1.5 s; unfixed: stopped at z 26.17). 1.2 at 260..730 fps: the stairs running, up to y
  2.211 / 2.207 (unfixed: stopped before the last step at 1.96); the spawn curb at x -1980 running and -1972.5
  walking climbed at the same heights as 62.5 fps (unfixed: stopped), at -1985 running climbed by none.
- **game** Still different above 62.5 fps: running up a flight of stairs is about 7 % faster (12-02's stairs: y 1.85
  against 1.73 at 1.5 s), fixed or not (unfixed 1.89): not the sweep's lengths; not found yet. A person stands 8 mm
  nearer a wall (the skin's distance, k times 0.01).
- **game** **A stopped car creeps above 62.5 fps** (the owner's report: a mission waiting for the car to stop waits;
  2026-10-15, both builds' code, the 1.2 lab). `C_car::AI` (0x41F780) sets the controls (SetPower, SetBrake,
  SetSteer) and calls `C_Vehicle::Tick` (0x4DF9A0, thiscall(ms), 0 ms taken as 1) every frame; Tick runs the
  drivetrain and calls `C_Vehicle::Engine` (0x4E1CE0, thiscall(dt s, ...)), which runs `C_Vehicle::Move` in equal
  substeps no longer than the vehicle's `+0xC24` (0.028 s): at 62.5 fps one Move of 16 ms a frame, at 400 fps one of
  2.5 ms, and at the end stores the step at `+0x4C8`. Move zeroes the velocity (`+0x1F90`), the angular velocity
  (`+0x420`) and the speed and sets the calm flag 2 in `+0x1A4` when the speed is under 0.01 m/s and the angular
  speed under 0.015 rad/s (0x4E6386..0x4E676A); Tick's end lets a calm car sleep after 1000 ms of its counter
  (`+0x1A8`), or after 4000 ms within 0.15 m (`+4`, the place `+8`: dist^2 < 0.0225). Those thresholds are in
  m/s; what is not is the damping of the car's small motions, much weaker in short steps: measured at the freeride's
  start (the taxi from (-1984.86, -4.74, 22.63) facing -z, 2 s of throttle, 1 s of brake), at 62.5 fps the residual
  speed falls x0.89 a frame (0.053, 0.032, 0.020, 0.013 m/s every 64 ms) and is zeroed at 4.42 s (the car stopped at
  4.35 s); at 300..400 fps it keeps circling at 0.01..0.03 m/s, the velocity and the angular velocity turning
  slowly together (a period of 4..5 s), a loop of about 4 x 3 cm, and the car sleeps at 12.5 s.
- **code** **The cause: LS3D drops small rotations** (`knowledge/notes/vehicle_physics.md`, "Small rotations are
  lost"): `S_quat::RotationMatrix` returns the identity for `|w| >= 1`, and a turn under 0.00049 rad has `w == 1.0f`,
  so the car's body cannot turn slower than 0.00049 rad a step (0.03 rad/s at 62.5 fps, 0.2 rad/s at 400 fps): its
  rocking on the springs is never answered and it creeps; its slow yaw has a dead zone. fps_unlock's fix
  (`fix_rotation`) hooks the export and builds the matrix of such a quaternion by the game's own formula. Measured
  in the 1.2 lab at 330..380 fps, with the frame's own steps (the same test as above, five or more runs each): at rest
  (zeroed) 0.23..0.43 s after slowing under 0.1 m/s, against 0.07..1.47 s at 62.5 fps (unfixed: still creeping after
  7.8 s); 0.15 of steering for 3 s: a yaw rate of 0.161..0.167 rad/s and a sideways velocity of -0.02..-0.01 m/s,
  against 0.161 and -0.002 at 62.5 fps (unfixed: 0.019..0.055 rad/s, +0.15 m/s). At 62.5 fps the fix keeps 200..230
  turns in a stop test that the game drops; its stops and steering stayed inside 62.5 fps's run-to-run spread.
  **1.0** (its lab runs at 140..185 fps unlocked; the same test, its fields at 1.2's plus 0x7C): at rest 0.23..0.29 s
  after slowing down, against 0.37..2.19 s at 62.5 fps and 3.9..7.6 s unfixed (creeping 6..13 cm); the steering's
  yaw rate 0.180..0.183 rad/s fixed, 0.169..0.175 unfixed, 0.174..0.178 at 62.5 fps (at 150 fps the lost turns are
  those under 0.07 rad/s: this yaw is mostly above it). Standing, 1.0's `Move` hands `RotationMatrix` about 200
  quaternions a second with `w` exactly 1 (seen by `tap` with the fix off).
- **code** Replaced: a first fix (`fix_car`) ticked a car slower than 1 m/s in whole 16 ms steps. It reproduced
  62.5 fps to the digit (a teleported taxi settling read 1.4291, 0.2371, 0.7913 m/s at 400 fps as at 62.5) but hid
  the cause. What it taught: the controls' ramps (SetBrake, SetHandbrake, SetPower in reverse: `+0x3CC += +0x4C8` a
  call) are fed every frame and count the step Engine left, so ticking a car in other steps than the frame's must put
  the frame's time back at `+0x4C8` (left at 16 ms they braked 6 times as hard: 7 m/s^2 in the first 0.25 s
  against 3).
- **code** A scan of the decompile for what is done once a call instead of by the time (decay `x *= c`, smoothing
  `x += (y - x) * c`, a fixed step `x += c`, in functions that get the frame's time), each hit read (2026-10-14):
  - `bendable_plant_ai` 0x58EAD0 (st0rm94's name; the bushes and trees that bend): the force is integrated by the
    time (`* ms * 0.001`) but the bend's velocity is damped `* 0.93` once a call: above 62.5 fps the plants stop
    swinging many times sooner. Not seen in the game yet.
  - `C_player::AI_walk` 0x58F4B0: the turn on foot is `mouseSensitivity * 5 * held[18]` a frame (0.05 of it in the
    sniper view). A mouse's value is its counts since the last read, so its sum does not depend on the rate; an
    analogue axis (a gamepad's stick through IJoy) held at a value would turn faster with the frame rate. Not checked
    how IJoy hands the axis over.
  - Read and not a fault: `G_Camera::Tick` (timed, cut to 83 ms), `C_human::TickWeaponEffect` and `C_player::AI`'s
    timers (timed), the pedestrians' tick 0x452CD0 (its 0.8 / 0.75 are one-off weights, its `+0.8` a probe's height),
    `C_Break_plain_glass::Tick` 0x5611B0 (`+0.05` once on a collision), and the rest of the hits: units (degrees to
    radians 0.01745, rpm to turns a second 0.01667, per cent 0.01), random scatter, thresholds, angles wrapped by 2 pi
    or 360, one-off events (a hit, a shot, the init).
  - Not scanned: random events rolled once a tick (`rand() % n` in a tick happens more often at a higher rate), and
    counters of frames.

## The exit crash (seen in mafia-ragdoll, not solved)

- **game** At exit the first fault is in `Game.exe+0x7AD00`, a name comparator (0x46D954's code) reading an object
  already freed (memory reserved, not committed). Then `LS3DF.dll+0x5AC2E`: the index buffer manager's `FreeAll`
  writing to owners already gone (24 stale slices made before anything of ours ran).
