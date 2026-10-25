# Game.exe and LS3DF.dll: findings

Facts about the 1.2 game that took work to find. Each says how it is known: **game** (seen in the running game),
**code** (read in the decompile or disassembly only). Function names are the decompile's where it has them; look
any of them up with `kb_function`.

## Starting, menus, missions, saves

- **code** `C_mission::Open` (0x5409D0, thiscall `(this, const char *name, bool, uint, bool)`) opens a mission by its
  folder name: `missions\<name>\scene.i3d`, `cache.bin`, `road.bin`, `check.bin`. The main menu is the mission
  `00menu`.
- **code** The start-up path (0x5BEBE0) plays `intro`, then `tutorial`, then opens `00menu`.
- **game** The intro videos (1.2: Bink, `binkw32.dll`, or an AVI through DirectShow; 1.0: AVI only) run before the
  frame loop: no frames meanwhile (the loader's mods load after them). Which files, the order, the limits:
  `knowledge/notes/videos.md`.
- **code** Profiles are `savegame\mafia%03d.sav`, saves `savegame\mafia%03d.%03d` (code 0x605260..0x606150,
  `G_LoadSaveClass`). The freeride menu's image is `freeride0.bmp` (0x5E1920). Racing records: `savegame\mr%03u.sav`,
  `savegame\mrtimes.sav`, `records\%s`.
- **code**, **game** Saving and loading (read 2026-10-17 in both builds, run in both labs; host/saves.cpp's restore
  points): `SaveGameSave(quick)` writes the game's state, the AI groups and then the actors: `quick` 1 every actor of
  the mission (`g_vActors`, with their programs: C_program saves its registers and place), 0 only the temporary ones
  (C_game's). The scripts' AUTOSAVEGAME saves with 0, AUTOSAVEGAMEFULL with 1, both through SaveGameFromScript
  (SaveGameCreate(id - 5000), SaveGameSave, ProfileSave). A chapter's first save is 439 bytes (1.0's 050): loading it
  opens the mission anew. A whole save is 55..210 KB (Salieri's bar, Old Prison; the freeride 112 KB). The game loop
  (GameLoopSingle1, both builds) loads the save slot when C_game `+0x48` is 3 (0 playing, 1 leaving): an arrest (the
  police manager's tick) and the Game Over screen's control 0x4FD set it; a save of the same mission is loaded in
  place (0.3 s), another mission's opens it (1.2..3.7 s). SaveGameLoad sets the manager's one-shot flag `+0x1D4`, and
  the next SaveGameFromScript is skipped (a program saved inside AUTOSAVEGAME runs it again). 1.2 names each save by
  the text 5000 + slot, up to 5590 "Freeride Extreme - Subquest" (5591..5599 none); 1.0's texts name the chapters
  only (5570 none, though the freeride saves 570).
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
- **code** The primary and the secondary binding are one action read two ways (2026-10-16, 1.2's decompile; the
  owner asked what really tells them apart). On foot (0x58F4B0) every pair is either at 0.25 or more (forwards,
  backwards, the steps aside, walk/run, fire, crouch: `a4[0] >= 0.25 || a4[1] >= 0.25`): no difference. Driving
  (0x5917F0), Accelerate (33/34), Brake (35/36), Clutch (55/56) and the steering (37/38 with 39/40): the primary
  wins while it is not 0, and the player's flags (bits 2, 4, 8, 1) remember that it was in use and where the
  secondary stood then (0x6478AC, 0x6478A8, 0x6478A4, 0x6478A0/0x64789C); once the primary is 0 the secondary is
  taken only after it has moved 0.5 or more from there (a joystick's axis resting off its centre does not drive).
  With the primary in use the car ramps the value (`C_Vehicle` `m_bDontInterpolateBrake`, `..Clutch`,
  `..Steering` and the power's 0: interpolated); with the secondary it is applied as it is, not ramped (an analog
  axis). The other driving pairs (handbrake, limiter, gears, horn, looks) are either nonzero. So an agent's or a
  mod's value written to a secondary slot steers or accelerates exactly, a primary one through the car's ramps.
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

### The airplane (read 2026-10-17 in both builds, run in both labs; host/control.cpp)

- **code** `C_airplane` (actor type 0x16, 0x3F0 bytes; st0rm94's structs/airplane.md) is made only from a scene's
  placement: the freeride Extreme has `Letadlo` (idle at 64 m over the airfield, flight state 0) and `UfoTalir`;
  MISE10 and MISE16 one each. Its vtable (1.2 0x623318, 1.0 0x63B7C0) has AI at slot 13 and Update at 14, both
  thiscall(plane, unsigned ms) ret 4, the same fields in both builds.
- **code** AI: with the flight state +0xB4 not 0 (1 the route, 2 the take-off run: the scripts' `airplane_start`,
  `airplane_start2`), AI_fly moves the place (+0x24) along the facing (+0x30) at the speed (+0xB8), the facing aimed
  at the route's point, the rotation (+0xD0, {w, x, y, z}) a slerp of the nodes' (+0xF0, +0x100 by +0xBC); then
  always the propellers (blades or the blur disc by the speed against +0x7C), the sounds' fades, the damage gauge.
- **code** Update: the model frame's local place and rotation from +0x24 and +0xD0 (its parent the primary sector:
  the world); only while the flight state is not 0, the six collision proxies (DynUpdate) and the matrices at +0x184;
  always, people within 1.6 m of its engines struck.
- **game** The model's nose is its frame's +z (the facing and the frame's z row were the same at the start).

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
  landing damage is hit type 6 (from 0x57B660). An explosion is hit type 2: `C_game::NewExplosion` 0x5AAE10 calls
  every near actor's Hit (vtable +0x7C) with 2, its centre as both the direction's and the place's argument (seen in
  both labs: the scripts' `explosion 6, 400` gave 67 of damage at 5 m, 200 and more within 3 m, `hit_type` 2).

## A person's script and his AI (read 2026-10-18 in both builds' code, seen in both labs; host/orders.cpp)

- **game** Which actors the game ticks: `C_game::Tick` (1.2 0x5A51C0) ticks the list of act state 0 (`C_actor
  +0x1C`); 1 is asleep: every 5 s the same tick puts a ticked actor whose `+0x5F` is 0 to sleep and wakes a
  sleeping one whose model frame's `+0x13C` equals the scene's `+0x13C` (a counter: 0x1230 when read), and
  `C_actor::Tick` sets `+0x5F` from that same comparison, so it reads "drawn lately" (it is not the sector: a car
  made over the open river, 45 m from the player and drawn on the screen, kept `+0x13C` at -1 and slept, floating
  where it was made; why its stamp is never set was not looked for); 2 is set by `C_program::ClearAfterEnd` (1.2 0x46CCF0, 1.0 0x5BAC10) when the owner's program reaches
  END, and `SetActState` (0x406DA0, 1.0 0x44C750) never leaves 2. Any change sets `C_game +0x10C`;
  `ChangeActState` (0x5A5070) then moves actors between the ticked list (`+0xF0`) and the asleep one (`+0x100`),
  and one of state 2 out of both into neither: it is not ticked at all, no AI, no animation (a person stands
  frozen; measured: a spawned Paulie after an order's `end`). `C_actor::Rebirth` (0x406DD0, 1.0 0x44C820) makes
  2 a 0 but adds to no list: the game's callers (the police's pool, 0x49D860) follow it with
  `C_game::InternTempActAdd` (0x5A7B70, 1.0 0x5E35D0), which returns at once for an actor already among the
  temporary ones (`+0x124`: spawn's are). The loader's own programs (`script`) are freed when they stand on their
  END, before it runs: their owner keeps state 0 (seen with `commandblock` too).
- **game** `C_entity::AI` is 0x508150 (the vtable's slot 13, `+0x34`; st0rm94's pairing put it at 0x50814B, a stray
  call before it: a tap there never fires). In AI state 0 (`+0xF40`) it runs the program (`CallProcess`,
  `scriptTick`) while `+0xF44` is 0; while `+0xF44` is 1 (an action's phase) it runs the program only with 0x200 of
  `+0xF08` and the action queue (`actionsTick` 0x51B050) with 0x800 or without 0x200; then `ScriptSwitch`
  (0x523C91: its entry by the AI's call) moves `+0xF44`: 0 -> 1 when the program's `+0x60` (entity `+0xAFC`) is
  below `+0xF58`, 1 -> 0 (and 0x400 raised when 0x200 is set) when `+0xF58` is below it.
- **game** An action command (`ENEMY_MOVE_TO_FRAME`... 16 of them) adds a job, calls `ForceAI(0, 1)` (`+0xF44` 1,
  `+0xF58` 1.0) and sets 0x800 and the dispatched flag `+0xF0C`; it is done when 0x400 is set (cleared with 0x800).
  `actionsTick` puts `+0xF58` at 0 when the queue is empty (0x51B101). So the program's `+0x60` is the hand-back
  level: `C_program::Init` (0x461580, 1.0 0x5AF780) makes it 0.5, or 0 when the program has no instructions, and
  a person whose program was empty at Init (spawn's) never takes a finished action back until it is set.
- **game** `ai_action` type 0 is the move (`NewMove(pos, 0, 0x13, flags, float, path, expiry)`): flags 1 run, 2
  "done within the float" (MOVE_TO_FRAME always sets 2; the float is its radius). Type 9 (`blocked_way`) is queued
  when the route is blocked: a person sent behind the player's car stood there.
- **game** `ENEMY_MOVE_TO_CAR` only walks to the seat's door; `ENEMY_USECAR act, seat` gets in (`Use_Actor(car, 1,
  seat)`: the seat free, the car's speed `+0x688` at most 0.1, within 3 m); with no arguments it gets out.
- **game** `ENEMY_ACTION_RUNAWAY` (action type 0x0A) lasts while the person senses the one he runs from (sees,
  hears, smells: `ai_sensors`); `ENEMY_ACTION_FOLLOW` lasts.

## Drives and the road network (read 2026-10-18 in both builds' code, seen in both labs; host/orders.cpp, world.cpp)

- **game** `ENEMY_CAR_MOVETO act, frm[, stop|vzad|stopvzad[, natvrdo]]` (opcode 0x100E) takes only someone sitting in
  the car (`C_entity +0x98` == act's car), then calls `C_sledovacka::AddCarMoveTo` (1.2 0x4A1220, 1.0 0x4ED0B0) on
  the chase manager at `C_game +0x2B18` (both builds): a record of 0x44 bytes in its vector at `+0x14`/`+0x18`
  (`[0]` the car, `[1]` a new `C_driver`, 0x29C bytes; `+0x20` its status), the car's engine started, a way planned
  by `C_roads::FindPath` between `C_roads::GetNearNode` of the car and of the place. The command polls
  `C_sledovacka::ActionStatus` (1.2 0x4A8670, 1.0 0x4F4470): 1 arrived, 3 given up (both end the command and drop
  the record), 2 goes on. Seen: traffic pushed a driven car onto a bridge's low railing and it gave up, ending the
  command as an arrival does.
- **game** The drive's speed is the road's (1.0 read, seen in the lab): RecompilePath copies each way node's speed
  from the road network into the path node (`+0x48`: a crossing's `+0x10`, a point's `+0x0C`), and the driver's
  FillStartValues (1.0 0x485EC0) makes its limit `+0x17C` that times a factor redrawn every 5 s from 0.65..1.0
  (`+0x1C8`), handed to `C_Vehicle::SetSpeedLimit`; the city's streets gave 9 to 12 m/s. AddCarMoveTo sets `+0x218`
  (the speed at the way's end) 1000, or 0 with `stop`. What made a drive "aggressive" was not the speed: the car
  started facing away from the way's first leg (a parking lot; a car that had arrived the other way) and the driver
  turned on full lock (`+0x1E0` -0.7) across the pavement, then cut back to its target node, which it changes only
  within 20..40 m of it (`+0x1D4`, redrawn per node). Put on the way's first node facing its second (the way read
  from the driver's path at `+0x164`, stride 0x70, the position first), the same 800 m went in its lane.
- **code** The way (AddCarMoveTo, both builds the same code): node 0 the car's own place (`+0x408` 1.2, `+0x484`
  1.0), then the road nodes FindPath gives (the first dropped when the car lies between it and the next), then the
  place asked; the cursor `+0x170` starts at 1. The driver's speed limit `+0x17C` is the cursor node's `+0x48` (m/s)
  times `+0x1C8`, or 1000 when `+0x208` is not 0; MinV (1.2 0x43FD40) takes the corners' speeds from the nodes'
  `+0x48` too, while `+0x208` is 0 or the byte `+0x298` is set. Capping the way's `+0x48` (human.drive `speed` 8)
  gave 5 to 8 m/s (both labs).
- **code** The driver's reverse (C_driver::Tick, 1.2 0x43C4F0 lines ~1083..1126, 1.0 0x483360 the same): while bit 0
  of `+0x19C` is set the limit is 3 m/s (`+0x17C`), the gear -1, the wheels at the angle between the car's facing
  (C_actor `+0x30`, copied to `+0xA0`) and the way to its target point projected flat (`+0xD0`), signed by the side
  (`+0xAC`, the car's right); the bit is cleared once that angle is under 0.34 rad (1.2 0x623BF0, 1.0 0x63BCDC) in
  reverse gear, or when the car is blocked behind (car `+0x2215` 1.2, `+0x2155` 1.0). Tick sets it itself only when
  the car is blocked ahead (`+0x2216`), slow and a sphere test hits.
- **code** The chase manager's unsticking (C_sledovacka::Tick, 1.2 0x4A3E70, 1.0 0x4EFCF0): a record's `+0x28`
  counts the ms its car stands with the engine on; past 2000 (15000 when its cursor node has a traffic light) it
  sets the record's flag 0x100000 and the driver's `+0x298`, and from then on drives it with `+0x208` 1 (TickDriver:
  the chase's way round what is in front, the limit 1000) for 15 s of `+0x28`, then clears both. A cursor of 0 after
  the driver's Tick is an arrival (status 1): the unsticking's Tick sets the cursor back to `+0x78`, which only
  Tick's own stuck branch fills (with the cursor), so a fresh driver given the flag "arrived" at once (seen in both
  labs before `+0x78` was set by hand).
- **game** What a drive looked like (1.2 and 1.0, the city, a car facing east on the street at z 126.5, its way west
  then north, 190 m): left to the game, full lock forward over the pavement, 4 to 7 knocks of 1 to 6.7 m/s, stood,
  then the unsticking at 22 to 27 m/s, a car of the traffic struck; with the reverse set at the start the car backed
  round in 10 s (3.3 m/s, no knock) and went on at 9 to 13 m/s; with the unsticking ended once the car had got 6 m
  away, no stretch went above the road's speed. A car that has stopped 4 m short of a `stop` stands with
  `+0x19C` 0xC0000000 (a collision foreseen: PredvidaciKolize) until the unsticking moves it the rest of the way.
- **code** `G_Camera::SetCar` (1.2 0x5ED190, 1.0 0x5D4AA0; thiscall(camera, car) ret 4): End, `+0x0C` the car, the
  mode the game last used in a vehicle (none: the last on foot), Begin, the next pose cut (`+0x64`). End shows the
  followed car's model again through its frame (`car +0x68`, vtable `+0x24`): with a car whose model is gone
  (world.remove unlinks it first) it faults, so `+0x0C` is cleared before (camera.game `follow`, host/gamecam.cpp).
- **game** `C_roads` is `C_mission +0x30` (what `C_sledovacka +0x20` takes, both builds). `GetNearNode` (1.2
  0x55B370, 1.0 0x619480), thiscall `(roads, x, y, z)`, ret 0xC: a binary search on x over the nodes at `+0x18`
  (0x18 bytes, the position first), the nearest's index (0xFFFF none). Nodes are crossings and bends: the start of
  the city's freeride is 44.7 m from its nearest (node 216, the same in both builds).

## A person's route: ai_path (read 2026-10-17 in both builds' code, seen in both labs; mods/examples/debug_views, routes)

- **code** `ai_path::Tick` (1.2 0x5026C0, 1.0 0x40C970): thiscall `(ai_path*, uint, C_human *owner, bool, bool)`,
  ret 0x10, every tick for a person sent along a route. It hands `HumanMoveTo` (1.2 0x446470, 1.0 0x4B0280;
  fastcall: ECX the person, EDX `&path+0x74`, then five stack arguments) the point at `+0x74`, and counts the
  waypoint reached when he is within `sqrt(+0x90)` of the point at `+0x80`; then `GetNextNormalWaypoint` stages the
  next one. State `+0x8C`: 1 a waypoint, 2 a detour point (`+0x74` is then the detour point; `+0x80` stays the
  waypoint's), 3 invalidated, 4 abandoned; cursors `+0x58` (waypoints), `+0x5C` (detour points).
- **code** The waypoints are a vector of 0x3C-byte records at `+0x1C` in 1.2, `+0x04` in 1.0; the detour points a
  vector of 0x18-byte records (the point first) at `+0x2C` in 1.2, `+0x14` in 1.0 (1.2: GetNormalWaypoint
  0x501C60, isLastNormalWaypoint 0x501F90, GetNextSpecWaypoint 0x501EF0; 1.0: 0x40BF30, 0x40C260, 0x40C1C0). The
  rest of the object is at the same offsets in both builds.
- **code** A record (both builds): gate A `+0x00`, gate B `+0x0C` (the passage the route was built through, across
  the pavement: 2 to 5 m apart), the waypoint `+0x24` (what is copied to `+0x74`, where the person walks), a web
  node's index `+0x34`, its kind `+0x38`: 0 reached at the gates' midpoint (copied to `+0x80`), 1 and 2 at the web
  node (`[[mission]+0x2C]+4`, 30-byte nodes, the position first; the reach the node's `+0x10 & 0xFFFF` times a
  constant). The waypoint is near the gates' midpoint, not always on it.
- **game** Sara's walk of mission 07-02 (both labs) lies 0.06 m from the line through her route's waypoints on
  average, 0.13 m at most, on a stretch without detours. Gate A alone (read as "the waypoint" at first) lies 1 to
  3.5 m to one side: what looked like an offset of the walk was the wrong field.
- **game** A route object lives a few seconds: the person's actions free it and build a new one, often at the old
  one's address (the same size, 0x9C); a pointer to it kept past its tick soon points at another object (a menu
  was found there). Read it inside the tick, and tell routes apart by their waypoints, not by their address.
- **ours** st0rm94's `structs/ai_path.md` (1.0) calls `+0x74` the "staged point" and `+0x80` "where the actor is
  actually steering": in the code `+0x74` is what HumanMoveTo walks to and `+0x80` only the arrival test's centre.
  His container offsets are 1.0's; 1.2's differ (above). Reported to him:
  https://github.com/st0rm94/mafia-reverse-engineering-export/issues/2

## A car's path: C_driver (read 2026-10-17 in both builds' code, seen in both labs; mods/examples/debug_views, routes)

- **code** Every car the game drives has a `C_driver`: the traffic (`C_traffic_car::AI`, 1.2 0x4AFC40 calls it),
  a chase (`C_sledovacka::TickDriver`, 1.2 0x4A2A80: mission 15-07's Sergio), the police and the racers.
  `C_driver::Tick` (1.2 0x43C4F0, 1.0 0x483360): thiscall `(C_driver*, int dt_ms)`, ret 4, returns a char (0
  without a car or a path). The fields used here are at the same offsets in both builds: the car `+0x10`, the
  path a vector of 0x70-byte nodes at `+0x160` (begin `+0x164`, end `+0x168`; Tick divides by 0x70 into `+0x64`),
  the node it drives to `+0x170` (`+0x174` the one before), the point it steers to `+0x6C`.
- **code** A node: four lane points across the road at `+0x00`, `+0x0C`, `+0x18`, `+0x24` (some equal), the lane
  taken a byte at `+0x44`, flags a word at `+0x60` (0x8000: an edge). FillStartValues (1.2 0x43F050, 1.0
  0x485EC0) puts the cursor node's lane point at `+0x6C`, the lane its own byte's, or the node before's when both
  nodes have 0x8000. PathCorrection (1.2 0x43FF30, 1.0 0x486D70) then moves `+0x6C` back along the segment
  towards the car.
- **game** `+0x6C` lies on the segment from the node before to the cursor's node: about 8.2 to 8.6 m in front of a
  traffic car in town (13 cars, 1.2), 41 m in front of the chase car at speed (1.0); the node itself when it is
  nearer. The cars' way follows the line through the nodes' taken lanes: traffic 0.02 to 0.33 m from it on average
  (1.2 and 1.0), the chase car 0.51 m (1.86 at most, 94 samples at speed, 1.0).
- **game** A traffic car's path is 6 to 8 nodes, the cursor at 1 to 5, made anew as it goes; the chase's path in
  15-07 is 137 nodes across the city. Its `dt_ms` was 10 in every call tapped (1.0).

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
- **code** The display settings (read 2026-10-25 in both builds, run in both labs): Setup.exe's choice is the
  registry's `HKCU\Software\Illusion Softworks\Mafia` `LS3D_setup` (REG_BINARY, 63 bytes; one value for every game
  on the machine). `MafiaEntryPoint` (1.2 0x5BEBE0) reads it once, before `InitSystem`, with `RegQueryValueExA`
  into `InitSettings` (1.2 0x647EE4, 1.0 0x6F952C) when it is 63 bytes and byte 0 is the version 0x14 (else
  `DefaultInitSettings`'): the width a u32 at +2, the height at +6, the colour depth at +10. `InitSystem` reads the
  size from there (1.2 0x5BF814, 1.0 0x5FA58C). `game_launch {"resolution": [w, h]}` answers that read with another
  size for the one run (host/start.cpp AskResolution), the registry left as it is.

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
- **game** **A step of 0 ms breaks the intro's cutscene** (the owner's report; 2026-10-25, both labs). With the
  limiter's `jae` made `jmp` (A132's way) the first frame after a mission's load is given 0 ms: `C_records::TimeLine`
  (0x59B130, tapped at the intro: `0` then 56, 83, ...; with the game's own pacing 16, 41, 83) and the recording's
  tracks then start with the people in their bind pose (the man at the bar's fireplace, arms out) and the camera at
  another key for the first seconds; the cap at 60 fps changes nothing. Asked for 1 ms at least (fps_unlock's hook on
  the limiter), the first step is 1 and the cutscene is the game's own (the bartender, then Tommy at the door, frame
  for frame). No system of the game expects a frame of 0 ms.

## The game's own bugs (host/fixes.cpp, always fixed; read and seen 2026-10-25, both labs)

- **code** **1.2's options are not kept after a restart** (the owner's report: the level of details, the shadows).
  The video options of Options > Audio & Video are the mission's settings (`missionPtr + 0x90`: +0x90 the details'
  scale, +0x94 the draw distance's, +0x9C effects, +0xA0 sounds, +0xA4 shadows through a table {0, 1, 3, 6, 9, 15},
  +0xA8 particles), not the profile's. MafiaEntryPoint copies them from InitSettings (LS3D_setup's 63 bytes: +0x27
  details, +0x2B distance, +0x2F effects, +0x33 sounds, +0x37 the shadows' index, +0x3B particles; DefaultInitSettings
  0x5BF520) at its start and back into it when its loop ends (the menu's Quit), then writes it to the registry. 1.2
  reads `HKEY_CURRENT_USER\Software\Illusion Softworks\Mafia` (RegOpenKeyA 0x80000001 at 0x5BEC61; its Setup.exe
  writes there) but writes `HKEY_LOCAL_MACHINE` (RegCreateKeyExA 0x80000002, the push at 0x5BF465), which Windows
  keeps in its VirtualStore for the game (seen there: details 2.0) and 1.2 never reads. 1.0 opens and writes HKLM alike
  (0x5F9A43, 0x5FA1FE); in the 1.0 lab its RegCreateKeyExA returned 5 (no virtualization for that process): 1.0 kept
  nothing there either. Fixed (the push made 0x80000001): LOD 2.0 and shadows 0 written to HKCU at the Quit, read at
  the next start (the menu: Level of Details 1.0, Shadows Very Low).
- **code** **1.2 takes Sniper Mode's key away at every profile load**. `G_LoadSaveClass::ProfileLoad` (0x6057F0; 1.0's
  0x557880 has no such test) checks slot 29 (Sniper Mode, new in 1.2) with `loadsave_binding_find_conflict` (0x607460:
  the conflicting slot, or -1) and tests the result against 0 (`test eax, eax; jz` at 0x60592F): with no conflict the
  binding is reset, to R when R is free (the second test, 0x60595E, the same) or to none. Seen: X written into the
  profile (+0x350), ProfileSave, ProfileLoad: none; with each `test` made `inc eax; nop`: X.
- **code** **The controls' key names in another keyboard layout** (the owner's report: a Russian layout). 1.2's
  `controlsKeyName` (0x4F1830) names a key by IGraph's GetKeyName (LS3DF+0x71C40, its vtable's +0xCC: DirectInput 8's
  GetObjectInfo, DIDEVICEOBJECTINSTANCEW, the key's character in the thread's layout: W is U+0446 "ц") and turns it
  into the system's code page (0x4F1A30: WideCharToMultiByte(CP_ACP)); the font draws the game's language's code page
  (1252 here): W S A D Q became "ö û ô â é". 1.0's controlsKeyName (0x5D3180) has its own English table instead (103
  names, Escape..Menu) and no such bug. A joystick's names are IJoy's, DirectInput's object names in the system's
  language, in both builds ("Ось Y" drawn "Îñü Y"; controlsKeyName's joystick case: code < 0x100 an axis by IJoy's
  index X, Y, Z, RX, RY, RZ, two sliders; 0x100.. a button; 0x200.. a hat). The loader gives a name the font cannot
  draw in English (1.0's key names; "Y Axis", "Button n", "POV n"): seen in both labs.

## The exit crash (seen in mafia-ragdoll, not solved)

- **game** At exit the first fault is in `Game.exe+0x7AD00`, a name comparator (0x46D954's code) reading an object
  already freed (memory reserved, not committed). Then `LS3DF.dll+0x5AC2E`: the index buffer manager's `FreeAll`
  writing to owners already gone (24 stale slices made before anything of ours ran).

## The water (seen 2026-10-16, both labs; host/collide.cpp world_water)

- **game** The city's river is in the static collision: a face of surface 31 at y -9.99 under the ray at (-1590,
  -34), in both builds (the drawn plane's height); nothing under it within 100 m. The same plane lies under the
  whole city: at the embankment (-1598.2, -31.2) the ray meets the ground 3.9 m above it first. materials.def (the
  1.0 reverse's `def-files-docs/materialy.md`): 31 deadly water, 44 shallow water for cars, 40 and 41 scripted death
  volumes. A person put at a place with no ground over the water falls into it (Paulie, 5.5 m).

## The profile's progress (read and seen 2026-10-16, both labs; host/stats.cpp)

- **code** `G_LoadSaveClass::ProfileSave` (0x6059F0; 1.0 0x557A20) writes `savegame\mafia%03d.sav` through G_Stream
  (obfuscated, as the saves are: their bytes cannot be read off the disk): the header +0x20 (0x54 bytes: 'Prof',
  version 1, the number +0x28, the name +0x30; +0x64, +0x68, +0x70 copies of +0x1F0, +0x9C, +0x94), then +0x1F4 (the
  key bindings, 0x2D0), +0x4C4 (the settings, 0x5C), +0x520 (the network settings, 0x9C). The masks: +0x94 race
  unlocks, +0x98 steal permission (the freeride copies the race mask into it), +0x9C the freeride's bonus cars (bit
  n: the letter 'A' + n; carindex.def's +0xA0); +0xA0 the garage, 25 places {packed car id, C_car*}. The same
  layout in 1.0 (the 1.0 reverse's structs/loadsave.md). Profile 4 of the 1.2 lab: +0x1F0 3, +0x94 = +0x98 0xC5FFF,
  +0x9C 0x283FF.
- **game** The saves' names: text 5000 + slot ("01 An Offer You Can't Refuse" at 5000, "20-06 The Death Of Art -
  Sam" at 5561, "Freeride" 5584, "Freeride Extreme - Home" 5585, "... - Subquest" 5590; 5570 and 5580 blank).
  SaveGameEnumFiles lists a slot only when its text exists and its file's record says 16.

## The items' table and a mod's items (read and seen 2026-10-16, both labs; host/items.cpp)

- **code** `LoadMaterialsFromBin` (0x60F5C0; 1.0 0x592080), once from `G_LoadSaveClass::Init`: `tables\predmety.def`
  read whole into `operator new` memory (0x6106FC; 1.0 0x624934), `g_pItems` and the count (size / 188). A record:
  +0x00 the name (32), +0x20 the flags (`def-files-docs/predmety.md` in the 1.0 reverse), +0x24 the model (32),
  +0x44.. a firearm's figures (+0x60 magazine, +0x64 reserve, +0x68.. floats), +0x88 1.0 in every record. The
  inventory, the hand, dropping and picking up, the pickup menu and `human_addweapon` read it by id, bounded by the
  count: a table grown in place (more records, the count raised) took a key's and a bucket's copies at 35 and 36.
- **game** The names the game shows: text 3300 + id (the pickup menu's line, the line when it comes into the hand:
  0x595EC5), 3400 + id ("... collected", 0x609C99 pushes 0xD48), 3500 + id (the gun shop, 0x5E29F2..). 3333..3399,
  3433..3437 and 3533..3599 are blank (" ") in the 1.2 lab's English database; 3438 "You already have this weapon",
  3439 "The weapon is fully loaded". 1.0 uses the same bases (0x4CD855, 0x55BCB9, 0x57899D..).
- **game** A big item (the coat's slot, 0x200000) is refused by `human_addweapon` while the coat holds another.

## What bullets, crashes and falls do (read and seen 2026-10-16, both labs; host/happen.cpp)

- **code** `C_game::NewShoot` (0x5A84A0; 1.0 0x5E3DF0) finds the static world's impact at once (`testLine`
  0x5C74D0 with the shot's whole way) and keeps it in the shot's record, pushed to the queue at `C_game + 0x1D8`
  (records of 0x5C bytes): +0x00 from, +0x0C the way (to the impact, or the weapon's reach), +0x1C where it is now,
  +0x28 damage, +0x2C shooter, +0x30 a glass's frame, +0x34 the static primitive, +0x38 its point, +0x44 its normal
  (toward the shooter), +0x50 (byte) 1 when there is one, +0x58 (byte) its surface. `C_game::TickShoot` (0x5A8E70;
  1.0 0x5E47C0, the same code at the same offsets) moves each bullet 0.3 m a ms and asks `collRayTest` (its call
  returns at 0x5A935C) only for the moving objects on the frame's stretch (it returned 0 for a wall 1.5 m away);
  people (types 2, 27) get the parts test (`humanBoneRayTest`) and their Hit, others their Hit (vtable +0x7C); at the
  end of the way (`|now - from|^2 + 1 > |way|^2`) it makes the static impact's sparks and particles.
- **code** `C_car::Was_Collision` (0x4259E0; 1.0 0x46CC40, the same code), fastcall(car, impulse*, impulse copy*,
  pos*, float, S_vector*, C_actor* struck, prim*, int), ret 0x1C: called by `C_Vehicle::BodyCollision` and
  `DoWheelsCollision` for each contact, every frame of a car pressed on a wall (about 65 N s against a wall at
  rest, 15 000 N s for the taxi at 10 m/s). It divides |impulse| by the mass (C_car + 0x664; 1.0 + 0x6E0): debris
  from 6 m/s, a flash from 5. The taxi's mass: 1520 kg. A car striking a person goes by `C_car::Call_Collision`
  (0x425480) to the person's vtable +0x78 and then his Hit (type 8 for a moving car, the car the attacker); the
  driver of a car that strikes a wall gets a Hit of type 5 from his car (zero position and direction).
- **game** A person's fall: `C_human::MovementIntern` keeps the state +0x40C at 3 in the air, the fall's speed
  +0x418 growing by 9.81 t; on landing the state turns 1 and from 8 m/s (18 with the byte +0x41C) he is hurt with a
  Hit of type 6, `(speed - 2) * 10` (12 instead of 2 with +0x41C). A jump is an animation (state 4). A person
  teleported into the air stays there until he moves.
- **game** Bullets on the dead (2026-10-21, both labs; host/hits.cpp, tests/game/test_hits.py): a dead C_human keeps
  his collision objects, and they follow his bones: a corpse whose pelvis a mod moved 2.5 m was struck there by the
  game's own test (a part, the blood, his Hit with was_alive 0), minutes after his death. A traffic pedestrian's
  corpse is out of the collision grid from pedHit's death on (`collGridRemove(element + 0xC4)`): bullets pass it.
  The pedestrian system's bullet Hit (pedBulletHit) for a dead element goes no further in 1.2; 1.0's calls pedHit,
  which returns at once (health below 0). After a hit the bullets' tick asks the person's part test again with the
  length to the point it was given (`humanBoneRayTest(..., |hit - from|^2)`): a point before the part's volume (a
  fat capsule's skin) is no hit.
- **code** LS3D's frames are rebuilt from the "changed" flag 0x40000000 (`I3D_frame::UpdateChildren`, ls3d+1C9A0:
  a frame with 0x40002000 marks each child 0x2000 and clears its 0x120): `SetWorldPos` (ls3d+1B160) sets it with a
  position. A position written without it moved the frame and left its children (game.h SetLocalPosition, fixed).
- **game** A dropped weapon falls as an actor for about 3 s, then is destroyed and becomes a using object to pick up
  (1.2 offered it with the rest in the "Pickup/Use" menu, `itempickup`, which pauses the game; the 1.0 lab took the
  only one at once). Its keys: digit 2 closed the menu here without taking; `menu.click` on the line took it.

## The using objects: what Action finds on foot (read and seen 2026-10-16, both labs)

- **game** `C_player::Do_AB_OwnerNULL` (0x594300; 1.0 0x4CBD00) asks `FindNearObjects` (0x55E180) of the registry
  at `C_game + 0x29EC` for the records near a point 0.8 m ahead of the player and 0.9 m up, then **always** runs the
  action menu (`GM_Menu`, vtable 0x625920) through `ExecuteMenu`, even for one record: alone, it is taken without the
  menu showing. The result is `0xA9 + the item's index` (0xA8: take them all, the weapons and items); an item of
  type 1 with an actor (`record + 0x1C`) goes to the actor's code (cars, type 27 people: 2012 search, 2013 a body),
  one without switches on its id (0x7D0.. the doors, seats...) and passes over an id it does not know.
- **game** A record (`S_using_object`, 0x38 bytes): +0x00 the place, +0x0C the reach squared (horizontal; 1.5 m up
  or down), +0x10 a world matrix the place is in (null: the world), +0x14 flags (1: DelObject frees it, 2: its frame
  +0x18 released), +0x1C an actor, +0x20 an owner frame (0: any), +0x28 the S_GameItem the menu lists: {type 1, the
  action's id, 0, the record}. The menu's line is the game's text of the action's id: the loader's use-points give
  it a text id of their own (host/interact.cpp) and are listed with the car's seats ("2. Fish here").

## People's animations (read and seen 2026-10-16, both labs; host/anims.cpp)

- **game** C_human::Do_PlayAnim (1.2 0x585E80, 1.0 0x4A63F0): thiscall(name, bool simple, bool load) ret 0xC ->
  the length in ms; it calls SetAnim on the animations machine at C_human +0xBC (a cross-fade of 4.0) and, only when
  the machine found a track (.tck: machine +0x2C != -1), sets the state +0x40C to 4. GetActiveAnimTime (1.2
  0x40E890, 1.0 0x5AB3A0) thiscall(machine, int *time, int *length) ret 8. Do_StopAnim (1.2 0x57D2E0, 1.0 0x49D840):
  the state back to 1 and his own animation. ChangeAnim (1.2 0x57DD00, 1.0 0x49E260): (own, with | null).
- **game** The table of animations (1.2 0x62E488, 1.0 0x647808): 576 records {const char *file, int number}, the
  numbers not consecutive, those above 511 a set's variants (513 of 1, 4097 the player's standing `breath05a`).
  C_human +0x70 is the number playing in his own life (an animation Do_PlayAnim started is not there), +0x74 asked.
- **game** The action queue (Do_ShockDown 1.2 0x587C20, 1.0 0x4A8190): entries of 0x14 bytes from +0x23C ({1, an
  animation's number, ?, a byte, ?}, then {3}), the index +0x2A0, the state +0x40C = 6; the get-ups are 111 and 338.
- **ours** A person's bones' axes do not tell the body's front: a standing Paulie's `back3` z points down. Right is
  the model's x (x x y = z, the facing): front = (right shoulder - left + right thigh - left) x (neck - pelvis).
- **ours** Lying on the back, a person's pelvis's local rotation relative to his model frame (the pelvis's parent) is
  (w x y z) (0, 0, 0.7071, 0.7071) and on the front (0.7071, -0.7071, 0, 0), the pelvis 0.15 m up (both builds).

## A model's own animation, physical objects (read and seen 2026-10-17, both labs; host/props.cpp)

- **game** MODEL_PLAYANIM (1.2 0x471E90, 1.0 0x5BFC80): the animation cache (1.2 0x647D80, 1.0 0x6F93C8)'s Open
  (1.2 0x4104F0, 1.0 0x47FA10; thiscall(cache, name, &set, 0, 0) ret 0x10), the set's mode +8 set to 2, the model's
  vtable +0x60 SetAnimation(model, set, channel 0, 1.0f, mode 0), C_mission::AddAnimModel(model, true) (1.2 0x53F520,
  1.0 0x5FEAC0), the set released. AddAnimModel takes the model only when its +0x64 (the channel's set) is not null;
  C_mission::Tick calls each listed model's +0x14 Tick(ms) after the scene's (C_mission +0x18/+0x1C the list, 8 bytes
  an entry: model, cleared with the mission). MODEL_STOPANIM: +0x68 StopAnimation(model, 0), DelAnimModel.
- **ls3d** The model's animation channels: +0x150, made by the first SetAnimation (0xF8 bytes: +0 the model, 8
  channels of 0x1C bytes from +8: the set, a byte "on", the mode, the time in ms, -40, a word, the weight; +0xE8/+0xF0
  the frames moved). Its tick adds the ms to each channel on; past the set's length (its vtable +8) a mode 2 channel
  wraps, any other mode turns off and stays in its last pose. SetAnimation's mode 0 takes the set's own (+8). The
  same layout in 1.0 (its model vtable 0x10098100, 1.2's 0x1009C160).
- **game** CREATE_PHYSICALOBJECT (1.2 0x4741D5, 1.0 0x5C1FC5): new C_public_physics (0x38C bytes, 1.0 0x3B0; ctor
  1.2 0x47C3A0, 1.0 0x4CDE20: C_PhThing's with the type 0x20), its vtable +0x48 Init(frame) (C_PhThing::Init: the
  frame's world matrix copied), C_PhThing::InitBox (1.2 0x45EE90, 1.0 0x4C41A0; {mass >= 1, velocity, 0.45}),
  C_game::AddTemporaryActor; its GameInit puts a box of the frame's bounds (+0xB0, LS3D's hierarchy bound) into the
  collision's dynamic objects (DynInsert). The kick: ExternalImpuls (1.2 0x460000, 1.0 0x4C53F0; thiscall(impulse,
  at) ret 8, the point minus the object's position the lever). With no velocity it stays where it was made, in the
  air too, until pushed.
- **game** C_actor::GameDone (when RemoveTemporaryActor's deletion comes, the next tick) relinks the frame to the
  actor's +0x58 (its parent at GameInit) and writes back +0x3C (the local position) and +0x48 (the rotation, w x y
  z): a physical object taken out puts its frame back where it was made, into the scene, even one unlinked since.
  Written before the removal, they decide where it is left. C_actor::Init clears +0x6C, the byte that makes the
  destructor release the frame; the frame's reference count is its +4.

## The archives' names, a model's file (read and seen 2026-10-17, both labs; host/models.cpp)

- **ls3d** I3D_model::Open (the model's vtable +0x50: 1.2 0x100335A0, 1.0 0x100327B0) loads through the model loader
  (1.2 ls3d+0x603E0, fastcall(file, flags, callback, ctx, model)) and keeps a copy of the name it was given at the
  model's +0x154 (a counted string: the count at -4). A tap on the loader over the freeride's load (1.2) saw 756
  opens, 754 from model Open, 2 from the scene's Open: every model of the game carries its file ("Models\Tommy.i3d",
  the player's and the taxi's in both builds). LS3D names models `.i3d`; their files are `.4ds`.
- **rw_data** The same archive keys open both builds' archives (1.0 ships A0 and AB, 1.2 not): 3223 distinct
  `.4ds` names (a2 3045, a8 the patch's 98, a1 80); `models\` names are in a2 and a8 (`MODELS\` upper case in a8).
- **ours** The .4ds parser read 3220 of 3223 (4.38 million triangles at LOD 0); the others are version 27
  (`missions\CARCYCLOPEDIA\scene.4ds` and one more) and 58 (`models\tvoda01.4DS`).

## The meshes LS3D holds (seen 2026-10-17, both labs; host/meshes.cpp)

- **ls3d** A visual: +0x1D4 its kind (`OBJ_`, `LMAP`, `BBRD` billboards, `PRTE` particles, `SNGM` skinned,
  `SMMR`...), +0x1E0 its mesh, +0x19C its world bounding sphere's centre, +0x1A8 its radius; the mesh's LODs and
  face groups as host/meshes.cpp says, the same in both builds. At the freeride's start, 140 m around the player: 109
  to 127 `OBJ_` and 12 `LMAP` visuals touch the box, 26 of them actors'.

## How the game's world reaches Direct3D (measured 2026-10-17, both labs; host/shaders.cpp shader.survey)

- **ours** The game thread's x87 control word as the game enters the loader's frame: `0x007F` in both builds (24
  bits of precision, round to nearest, every exception masked): the game's Direct3D made without
  `D3DCREATE_FPU_PRESERVE` (measured 2026-10-17, `fpu.state`; host/fpu.cpp raises it to `0x027F` around the mods).
- **ours** A texture shared from Direct3D 9Ex to Direct3D 11 (R32F, a render target) drawn with
  `D3DRS_COLORWRITEENABLE` red only: Direct3D 11 read it as its clear value and none of the draws, while Direct3D 9
  read the draws (an NVIDIA RTX 3070 under dxwrapper's 9Ex, 2026-10-17). Written with every channel, both see it.
  The device the loader's Direct3D 9 and 9Ex interfaces come from is one object (QueryInterface of the d3d8 device
  for either gives the same pointer).
- **ls3d** LS3D's camera has an orthographic projection of its own: I3D_camera `+0x15C` (a byte: on), `+0x160` the
  scale (SetOrthoScale `1000B8D0` in 1.2's LS3DF: "Invalid ortho scale"), the projection made by `1000B400` (the
  ortho branch: `1/(far - near)` and `-near/(far - near)` for z, the scale for x and y times the aspect at `+0x158`).
  The lead for a shadow map drawn by the game itself (ROADMAP Known gaps).

- **ls3d** Every face group LS3D draws (the freeride's start: 362 to 497 draws a frame) goes with Direct3D's
  lighting off, no light set: LS3D lights the vertices on the CPU and hands their colour. FVF 0x142 (position,
  colour, one uv) or 0x242 (two uv). Stage 0: `modulate2x(texture, diffuse)`, alpha `modulate`; lightmapped
  objects: stage 0 `add(texture, diffuse)`, alpha `selectarg1`, stage 1 `modulate2x(lightmap, current)`; a car's
  paint: stage 1 `blendfactoralpha(reflection, current)` with the texture factor 0x26000000 (its alpha 0.15). Vertex
  fog, linear (FOGVERTEXMODE 3, no table fog). Blending SRCALPHA / INVSRCALPHA (translucent) or SRCALPHA / ONE
  (additive); alpha test GREATEREQUAL with 200 or 1. Skinned people come the same way (skinned on the CPU). One
  pass a face group.
- **ls3d** The city by night (freeridenoc, measured 2026-10-17 in both labs): 1053 lights in the scene (1025
  point lights: the street lamps; 21 spots, among them each car's headlights: a spot of the car's own at its front,
  range 25 to 60 m, cones 0.52 and 1.22 rad, its direction the car's forward). The cone of light the headlights
  throw on the street is not drawn by the face groups (every one at pass 0 in a survey): another path, not found.
- **ours** A face's normal from a pixel's world position is `cross(ddx(p), ddy(p))` under the game's projection
  (the street's comes out y up); the other order points it away (game.hlsli's `mml_FaceNormal` had it so at first).
- **ours** The depth's last bits matter: a sign behind a wall of the building across the street from the
  freeride's start is hidden from the parking lot by the fixed pipeline's arithmetic and shows through the wall
  when a shader computes the position with the matrices multiplied together first.

## pet1kk's developer tools (MafiaModLoader PR 2, read and checked 2026-10-18 in both builds' code; not run yet)

A standalone ASI for 1.2 (its source in the PR; on this machine in `re/ext/developer_tools/`, never committed: a
prebuilt binary and the game's own menu files patched). What it found, each checked in Ghidra in both builds before
it was used here; what the loader makes of it: host human_invulnerable, hud_game, web_nodes, material_name;
mods/examples/devtools; debug_views' distances, collision, web and names.

- **code** `C_player::Hit` starts at 1.2 0x594210 (C_player's vtable 0x625878, slot 31 +0x7C); MafiaReverse's
  0x594209 is 7 bytes early, in the previous function's padding (renamed: the Ghidra project and names.csv). It
  returns 0 without calling C_human::Hit while +0xADA is set and SemTrefenaLocklaKurva refuses (the player held by a
  scene); else C_human::Hit, then PlayerSetLives (and SetSniperMode(0) in the scope). 1.0's is st0rm94's
  player_hit, 0x4CBC10. C_player::Collision (0x5941C0, 1.0 0x4CBBC0, slot 30) has the same gate; C_human::Collision calls the Hit slot (+0x7C) for a
  car's blow, and a fall's landing is a hit of type 6: every harm is a Hit.
- **code** The scripts' distance tests (C_program::Process's switch, 1.2 table 0x4799BC, 1.0 0x5C7700, the opcode
  minus 1; the handlers read in both): GETACTORSDIST {int a, int b, int f}: flt[f] = 0, then the distance of
  act[a]'s and act[b]'s frames; IFFLTINRANGE {float lo, float hi, int f}: lo <= flt[f] < hi; IF {int left, float
  left_imm, int right, float right_imm, int cmp}: an index -1 takes the immediate; cmp 1 '<', 2 '>', 3 '=', 4 '!='
  (the x87 compare's flags), its two labels at +0x14 and +0x24. game.h ScriptDistParams and the rest.
- **code** The pedestrians' web: C_mission +0x2C C_web_path, read by C_web_path::LoadNodes (1.2 0x54A8C0, 1.0
  0x608CF0, the same code): magic 0x1ABCEDF, the count, 0x1E-byte nodes read whole (+0x0C the kind's bits, 0x4000
  out of searches; +0x0E the id, FindNodeID's), then each node's links (+0x1D their count) into a block at +0x18,
  8 bytes each: the other node's index (FlipOneWay finds a link by it), flags (0x80 one way), a float. The nodes are
  sorted by x (GetNearNode's binary search). C_roads at +0x30 (road.bin) the same in both.
- **code** The surfaces' names: LoadMaterialsFromBin (1.2 0x60F5C0, 1.0 0x592080) reads tables\materials.def whole
  into [0x6D4C24] (1.0 [0x6EFBA4]), the count (size / 0x170) at 0x6D4C28 (0x6EFBA8); a record's first 0x20 bytes its
  name. The PR's own list of names (some marked "?") is not used: the game's table is.
- **code** The actors' kinds: C_mission::CreateActor's switch (1.2 0x53F7D0, 1.0 0x5FED70, the same) makes 1 C_actor,
  2 C_player, 4 C_car, 5 C_detector, 6 C_door, 8 C_railway, 9 C_villa (the PR's "box"), 10 C_bottle, 0xC
  C_traffic_car, 0x12 its generator, 0x14 C_bridge, 0x15 C_dog, 0x16 C_airplane, 0x17 the seagulls, 0x18 C_turnout,
  0x19 C_pumper, 0x1B C_entity, 0x1C C_racecam, 0x1D a bending plant, 0x1E C_wagon, 0x1F C_irenka, 0x20
  C_public_physics, 0x21 C_actor_shot, 0x22 C_clock, 0x23 C_box_new, 0x24 C_car_ex, 0x26 C_rail_generator
  (game.h ActorTypeName).
- **code** The HUD: G_IndicatorsClass::DrawAll (1.2 0x5FB060, 1.0 0x54D180; thiscall(hud)) is draw2d's first call at
  stage 2 (1.2 0x5BD8E8), and 1.0's menu scenes' callback calls it too (0x56CCAF). The jump: C_human::Do_Jump (1.2
  0x586190, 1.0 0x4A6700; thiscall -> bool). The movement: C_human::MovementIntern (game_build.h humanMove).
- **code** GM_Menu: 0x5EB210 is SetFocus (thiscall(menu, id); renamed), 0x5EAE10 MenuClose, 0x5EAE40 MenuCloseAll,
  0x5EB2F0 FindComponentByID (MafiaReverse's names already). C_detector::Process 1.2 0x433D10, 1.0 0x47A840.
- **lead** Wireframe: Insanity3D's I3D_driver keeps it as its own state (RS_WIREFRAME = 7: D3DRS_FILLMODE set by the
  driver, its flag kept); LS3D's driver SetState (its vtable +0x14) was not found yet. The PR's other plugin sets the
  fill mode on each draw of the device instead. ROADMAP.
- **lead** DEBUG_TEXT: the retail game knows the keyword but compiles nothing for it (the PR's claim: its DebugText
  hooks DecodeInstruction 1.2 0x463960 (1.0 0x5B1B60), ParseArgs 0x461C70 and the dispatch 0x46D3FE to run it).
  Not checked here yet. ROADMAP.

## A person's two spheres in the moving objects' grid (read 2026-10-18 in 1.2's code; seen in the 1.2 lab)

- **code** `C_human::CreateDynColls` (1.2 0x575CA0) makes two moving objects of kind 0x42 (a sphere) for a person and
  puts them into the dynamic grid (DynInsert): class 0x101, radius 0.3 (0x3E99999A), at his model frame's place; and,
  when the model has a frame named `base` (the string at 0x632444; its vtable +0x38 finds it), class 0x102, radius
  1.2 (0x3F99999A), at that frame's place. Their owner (+0x44) is the person. A car has the same pair: 0x201 its body
  over the wheels, 0x202 its whole hull (physx_crates' notes).
- **code** A test's moving mask is an exclusion: `g_collision::SphereInDynamics` (0x5CAFB0) tests an object only when
  `(query mask & object classes) == 0` (its +0x48 against the object's +8). So 0x102 (G_Camera::WantPos' camera
  sphere of 0.29, C_human::Can_EnterToCar's 0.3) leaves out every person (0x100) and every "wide" shape (0x2): it
  meets car bodies alone; a mask of 1 leaves out the narrow shapes and meets the wide ones and car hulls; 0 meets all.
- **code** The wide sphere is the people's broad phase for the aim: `C_player::InputOnFoot` (0x58F4B0, with
  IndicatorSetAimed) and 0x592B70 cast a 100 m ray with mask 1 (`TestLineH`); when it meets a moving object with an
  owner, the scene's `TestColGeo` tests that owner's own geometry along the ray, and when the model is missed the ray
  goes on past the sphere (a loop). `C_airplane::AI_fly` (0x40AE30) sweeps a 2.7 m sphere ahead with mask 0 and looks
  for people (types 2, 3, 27) in it. Who meets the narrow sphere (a mask without 0x100 and 0x1) was not looked for
  yet.
