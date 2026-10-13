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
- **code** Mission scripts: `C_program::DecodeParams` 0x461C6C, `DecodeInstruction` 0x463960, `Process` 0x46D3B0
  (the script commands' strings: `HUMAN_*`, `PLAYER_*`, `DETECTOR_*`...).

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
