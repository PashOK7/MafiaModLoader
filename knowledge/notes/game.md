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
- **game** From the launch to the freeride (recorded with `tap` while the owner clicked, then driven by the loader's
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

### What the owner's older tools said (TrackMafia, Telemetry2, LiveDelta)

Racing tools the owner made before this repository (a ghost car replayed from recorded laps, lap telemetry, an
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

## Rendering (LS3DF.dll)

- **code** `I3D_frame::SetVisibleRecMainPass` (0x10041DF0): the main visibility pass, a recursive walk with frustum
  tests against global planes; frame `+0xAC` flags, `+0xF0..+0xFC` bounding sphere, `+0x110` type, `+0x120`
  parent, `+0x124` first child, `+0x12C` next sibling, `+0x13C` traversal stamp (AntiCulling's report).
- **game** The frame limiter (`frameLimit`, `ls3d+0x72100`) is called once per frame by the main loop: the loader's
  frame event hangs on it.

## The exit crash (seen in mafia-ragdoll, not solved)

- **game** At exit the first fault is in `Game.exe+0x7AD00`, a name comparator (0x46D954's code) reading an object
  already freed (memory reserved, not committed). Then `LS3DF.dll+0x5AC2E`: the index buffer manager's `FreeAll`
  writing to owners already gone (24 stale slices made before anything of ours ran).
