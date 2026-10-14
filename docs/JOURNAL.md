# Journal

What was done, found and tried, in order (newest last). Agents: read the end first, append as you go.

## 2026-10-13: the base

- The host (`MafiaModLoader.asi` through Ultimate ASI Loader 9.7.4), the C ABI (`mml.h`), hook chains, native mods
  with hot reload, the bridge, crash dumps, the core mod, the Rust MCP server, offline tests (`run_tests.py`,
  `mcp_smoke.py`). Committed as the first commit.
- First live run in the lab (1.2): `game_launch` started the game, the loader detected 1.2 and hooked. While the Bink
  intros play there are no frames (`where` showed `binkw32.dll`); the mods loaded with the first frame of the menu.
  `game` answered (mission `00menu`, the scene); `player` is none in the menu.

## 2026-10-13: the knowledge base

- The owner's MafiaReverse folder (full Hex-Rays decompiles of the 1.2 Game.exe and LS3DF.dll with indexes) brought
  in as `knowledge/` (`tools/kb_import.py`): 4833 + 1599 functions, the call graph, string sites; the decompiled
  code stays out of the repository and is read at run time from `reverse_dir`. Our own names merged in
  (`names.csv`: mafia-ragdoll's Ghidra project, AntiCulling's report, `game_build.h`), and notes
  (`knowledge/notes/`: the DTA archives and rw_data.dll, IJoy.dll, findings about the game).
- MCP: `kb_search`, `kb_function`; `crash_inspect` and `where` name the functions of the addresses they print.
- The bridge took a fixed port: a test harness started while the game ran could not listen and the MCP server read
  the harness's token against the game's bridge ("bad token"). Now the bridge takes the next free port and writes
  `bridge.json` only once it listens (a stale one is deleted at start).

## 2026-10-13: tap, and the start

- `tap` (from mafia-ragdoll's host) moved into the loader's host as a link of the hook chain: the bridge method
  `tap`, the MCP tool `tap` (callers named from the knowledge base), startup taps from `mml\config.json`. Snapshots
  that start with text show it as a string.
- Recorded the way into the freeride: 14 startup taps (menus, profiles, saves, missions) while the owner launched
  the game, took a profile and clicked Freeride. Then read in the decompile: the menus loop in
  `GM_Menu::ExecuteMenu` until `MenuCloseAll` sets the result; a click calls the menu's command handler (vtable
  `+0x24`). Details: `knowledge/notes/game.md`.
- The start (`host/start.cpp`): `mml\start.json` asks for it once; the logo videos are answered "played", then in
  each menu's frame the loader sends the command a click would (the profile's 100 + i, the main menu's 990105, the
  freeride menu's 201). First try in the lab: from the launch to the freeride in 2.6 s; through MCP
  (`game_launch {"profile": 4, "go": "freeride"}`) 5.9 s until the player exists. The game's default freeride is
  `freeitaly` (code 28); the decompile's case labels for 28..31 were guesses.

## 2026-10-13: eyes and hands

- **Where the game takes its input** (read in the decompile, checked in the disassembly and live): one controls
  object (0x647C30) with 60 binding slots (`+0xB4/+0xB8`, 12 bytes each) and two float arrays parallel to them,
  "held" (`+0xC4`) and "pressed this frame" (`+0xD4`); its update (0x4F0620) fills them from the devices once a
  frame, called by the game's loop before `C_mission::Tick` and by the menus' loop. `C_player::AI` (0x5937B0) and
  its on-foot (0x58F4B0) and driving (0x5917F0) parts read the arrays by slot number. So an action is injected by
  writing its slot after the update: bindings do not matter (checked: `Left`, unbound in the owner's profile,
  steps aside). LS3D's keyboard read (`ls3d+0x717F0`) fills a 256-byte DirectInput state: keys are injected there
  (checked: `Esc` opens and closes the pause menu, `W` walks).
- `host/input.cpp`: `input.actions`, `input.act`, `input.key`, `input.seq`, `input.release`, `input.state`,
  `text.get`, `text.find`. The slots' names: on foot from the menu, driving from what 0x5917F0 does with each pair
  (33/34 accelerate ... 55/56 clutch, 57 gearbox, 58 camera, 59 replace car); not 32 as first guessed (the first
  try pressed slot 32 and the car stood still).
- **The mouse axes are not counts.** `Aim X` with values of 1..20 a frame spun the player several times round and
  looked random (the owner watched it). The on-foot code turns by `sensitivity (0x6D4B00) * 5 * held[18]` radians
  an update, and the update makes the value of counts / 400. Measured: 141.3 degrees for a sum of 1 at sensitivity
  0.4932, linear from 0.005 to 0.1 a frame. `turn` (degrees) works the values out: +90, -90, 45, 180, -10, 360
  each landed within 0.01 degree.
- **"Action did not work" twice, and it was the game both times**: 0x594FA0 does nothing while the car moves,
  stops the engine when it runs, and only the next press gets the player out. Found after the owner said what he
  saw (the engine went quiet); now in AGENTS.md, with the rule "doubt the tools" the owner asked for.
- `host/screenshot.cpp`: the back buffer at `Present`. The lab's back buffer is multisampled and Direct3D 8's
  `CopyRects` cannot read it; dxwrapper's device answers `QueryInterface(IDirect3DDevice9)`, so the buffer is
  resolved with `StretchRect` and read back (as mafia-ragdoll's `gfx grab` does). The MCP server's `screenshot`
  turns the raw file into a JPEG (150 KB at 1280x960) and returns it as image content.
- MCP `input`: steps laid out in time (one after another, or at `at_ms`), sent as one `input.seq`; returns the
  player's state and, when asked, a screenshot. A whole run by an agent, no human: out of the car, turn 90, walk,
  jump, look at the picture.
- **The freeride without police.** Out of the car with the freeride's Thompson in his hands, the player was shot
  at by a patrol (the owner killed it and told me; hiding the weapon is impossible: two long weapons). The owner's
  idea: the freeride menu's settings. `GM_FreeRide::OnCreate` (0x5E1920) sets the sliders 21, 22, 23 to 1 through
  0x5EB070 (a menu's component by id, its vtable `+0x4C`); its start (201) reads them back (0x5EB090, `+0x50`) into
  `FreerideStoreA`. Checked by screenshots of the menu after `menu.value`: 21 traffic, 22 pedestrians, 23 police.
  The start takes `"police": 0` (`game_launch`, `tools/start.py --police 0`).
- The MCP server's binary is locked while a session runs it: renamed, rebuilt, and the new one tested with a small
  client of my own; the session's own tools were the old binary all along (the new ones need its restart).

## 2026-10-13: the mod author's kit

- **The owner's measure**: how easily another agent writes a mod. TrackMafia (lap telemetry, a ghost car, an
  overlay) had to make its own threads, a transparent window over the game, a settings window, hotkeys read from
  Windows, guarded memory reads and build detection. Not ported; the host now gives each of those.
- **Where the game draws 2D** (decompile, disassembly, then `tap` in the lab). The game gives its scene one
  callback (0x5BD8D0, `stdcall(stage, ?, ?)`, `ret 0xC`); LS3D's render calls it with stage 1 and, once the 3D frame
  is drawn, stage 2: there the game draws the HUD (`G_IndicatorsClass::DrawAll` 0x5FB060 on the static object
  0x6BF980), the open menu (`GM_Menu::Draw`) and the console. The taps showed it called twice a frame in the game
  and in the pause menu alike. Text is `G_IndicatorsClass::OutText` 0x603880: `thiscall(hud, text, x, y, width,
  height, argb, flags, font, end)`, `ret 0x24`, a float back in ST0, in **pixels**: the HUD's money came as
  (1216, 32), width 32, height 64, font 1, flags 4 at 1280x960, the pause menu's buttons as x 88, width 347.2,
  height 48, flags 2. So the 800x600 units of the menus are a scale kept in the HUD object (`+0x40C0`, 1.6 here;
  0x60E5C0 is the game's own wrapper that applies it, plus an x offset 0x6C4500 that 1.0 does not have). The
  pet1kk notes had the two addresses right and called the object "a font": it is the HUD, the fonts are records
  of 0x101C bytes in it, chosen by the ninth argument (0 plain, 1 bold, 2 the menus' handwriting).
  Rectangles (0x5E8E00 outline, 0x5E8F60 filled) are two `IGraph` calls: `SetTexture` (vtable `+0x0C`) and
  `DrawPrimitive` (`+0x58`: LS3D's own primitive numbers, 1 a line list, 2 a line strip, 5 a triangle strip;
  32-byte screen-space vertices, FVF 0x1C4).
- `host/draw.cpp`: the callback hooked; after the game's own drawing, the queue drawn with `OutText` and those two
  `IGraph` calls, between the state the HUD sets for itself (IGraph's blending, the driver's state 3) saved and
  put back. No device call of our own, so nothing to say about Direct3D 8 against 9. First try in the lab: three
  fonts, colours, a translucent box, an outline, a line, all in the `screenshot`.
- **The fault guard, provoked**: the title font's character map (0x6C4080) pointed at nothing for a third of a
  second while a mod and the bridge drew in that font. Two access violations inside the game's `OutText`, both
  caught: the mod's drawing off (back after its reload), the bridge's items dropped, the log told, the other mod
  went on drawing, the game went on.
- **Keys for mods**: the edges are the loader's own (state against the read before, taken right after LS3D's read
  and the injected keys), hotkeys are called at the next loader frame. With no game the keyboard is what
  `input.key` holds, so the offline tests press keys.
- **A press that was lost, and why**: one injected F5 did not fire the example's hotkey. Not the key: the press was
  pending when a `config.set` arrived in the same frame; the mod bound its hotkey again from its settings, and
  binding again dropped the pending press. Now the hotkeys are called before the settings' events, and binding the
  same combination again keeps a pending press. (Two more "misses" were mine: tool calls sent in one message run
  at the same time, so the screenshot was taken before the key; and a lap that "did not start" had started and
  finished in the same tick, the car standing in the finish circle.)
- **The car, field by field** (each read in the function named in `game_build.h`, then watched while driving):
  position `C_Vehicle + 0x320`, speed `+0x59C` in m/s (8.906 with the HUD's own number 32.08 km/h beside it),
  steering wanted `+0x624` and the wheels' angle `+0x628` following it, the limit `+0x610` shrinking with speed
  (0.785 standing, 0.696 at 32 km/h), gear `+0x560` (1, 2, -1 in reverse), engine turns `+0x548` (13.33 a second
  idling: 800 rpm), fuel `+0xC30` falling from 59.993 of 60, the engine's health `+0x1B4` of `+0x1B8` (100 of 100;
  86 after a tree at 30 km/h).
- **TrackMafia's offsets, checked**: its position, steering, fuel and engine health offsets are these same fields
  (its 1.0 numbers are exactly the 1.0 twins found by matching code); its "speed" offset (`car + 0x594`) is not a
  speed in 1.2: it stayed 13.333 while the car went from 0 to 32 km/h.
- **A car's state**: the game has its own (`C_Vehicle::GetVehicleStateSize` 0x4CEB30, `SaveVehicleState` 0x4CEE00,
  `LoadVehicleState` 0x4CFAB0; its saved games call them for every car). Tried: saved (1525 bytes for the taxi),
  reversed away round a corner, loaded: the position, the axes, the gear, the engine speed and the fuel were back
  to the last digit, and the car drove on. TrackMafia's recipe (copy 0x2220 bytes of the object except 130 listed
  pointer fields) is not offered: the lists are per build and per whatever the object points at that day, and the
  game's own functions do the same job knowing its own layout.
- **The mission as events**: `C_mission::Open` 0x5409D0 hooked. It closes the open mission itself first, so
  `MISSION_OPEN` is sent from the close's hook, after `MISSION_CLOSE`. `MISSION_LOADED` is the first tick (2.6 s
  after the open for the freeride). The pause: the menus' loop (`ExecuteMenu`) ticks the mission only when its
  caller says so: the main menu's does (`00menu` runs under it), the pause menu's does not (the tick count stood
  still for 278 frames). So "paused" is a frame without a tick while a menu is open.
- **1.0**: a small tool of the session (a 1.2 function's bytes with every 32-bit displacement, immediate and call
  target left open, searched in the 1.0 file) found the twin of each function the kit uses, and from the twins'
  operands the 1.0 globals and offsets (the HUD is 0x5000 bytes bigger in 1.0: scale `+0x90C0`, fonts from
  `+0x5000`, the car `+0x94A0`). `check_builds.py` now checks every one of them in both files. `IGraph`'s vtable
  has the same slots in both LS3DFs (compared by the two functions found in each). Nothing of it was run in a 1.0
  game.
- `mods/examples/telemetry` (303 lines, each part of the kit once) ran in the lab: the overlay while driving, the
  lap started on leaving the start circle and finished 25 m on (1.616 s, kept in `mml\data\telemetry\`), "paused"
  in the pause menu, the details while F8 was held, mph after `config.set`. `tools/new_mod.py` made a mod that
  built and drew its line in the game without an edit.
- The example's first keys were F5 and Tab; in the lab's profile those are the speed limiter and the city map
  (`input.actions` shows the bindings). Now F9 and F8.

## 2026-10-13: the game's own menus, extended

- **The owner's direction**: a friend's achievements mod must sit in the game's real interface, an item of the
  pause menu that is a real item (the window grown, clicked, walked by the keys), not an overlay. Not the mod
  itself: the base every such mod stands on.
- **How the game builds its menus** (decompile, disassembly, then the lab): `GM_Menu` objects whose controls the
  game makes from definitions (`tables\menu\<name>.mnu`: 32-byte records) with one factory a kind (the list at
  0x6BD8B0, read live: 33 kinds); a click or Enter gives the control's id to the menu's vtable slot 9. The game
  grows its own lists (profiles, saves) with `GM_Menu::PridejComponentPod` 0x5EB7D0: a clone of a control put under
  it, the window grown, what is below moved. Details: `knowledge/notes/interface.md`.
- `host/menus.cpp` (begun in the session before, which ran out at its first screenshot): an item is added with
  that function when the menu is made (`GM_Menu::Create` hooked), with an id between its neighbours' (the keyboard
  walks a menu by rising id); the menu object gets a copy of its class's vtable with the loader's command slot. A
  page is a `GM_Menu` made as the game makes its submenus and pushed over the open menu; its definition is answered
  through the game's own file layer (`rw_data.dll`'s `dtaOpen`/`dtaRead`/`dtaClose`), so every control is made,
  laid out and focused by the game. Seen: "Achievements" between Options and Resume Game, Down x3 underlined it,
  Enter delivered id 109; a page "Achievements:" over the pause menu (the pause menu greyed under it as under
  Options), Back delivered, Esc closed it.
- **`mml.h`'s colour flags were a guess** (`MML_MENU_RED 0x100` for every kind). Each kind's Draw reads them its
  own way: a text 0x100 white, 0x200 yellow, 0x300 black, none grey; a button 0x100 red, 0x200 white; a row 0x400
  red. Now `MML_MENU_TEXT_*`, `MML_MENU_BUTTON_*`, `MML_MENU_ITEM_RED`, each seen on a page. A kind `chck` it
  listed does not exist (wrong: it does, see the next entry); `pbar` (the loading screen's bar) does, and is what an achievement's progress wants.
- `menu_set_value` / `menu_value`: a slider's and a progress bar's value by the control's own setter (slot 19; both
  keep it at `+0x40`). **`menu.value` said 0.0 for a bar set to 0.4**: the bar has no getter of its own (slot 20 is
  the base's, which answers 0); `menu.value` now reads a slider's or bar's value where the control keeps it.
- **A page asked for with no menu open was dropped at once**: `asked = GetTickCount() | 1`, and on an even tick
  count `GetTickCount() - asked` is 0xFFFFFFFF, past the 2-second limit in the first frame. Now a flag and a time.
  Then: the pause menu opened by the loader, the page over it, the bar's value set before the page existed applied
  when it was made.
- **A mod's reload left a hole** where its item was (hidden, not removed: the game owns the control). Now the
  reloaded mod asking for the same item takes the live control over (clicked after the reload: the new
  generation's page opened); only items nobody takes are hidden, at the next frame.
- The telemetry example has an item "Telemetry" in the pause menu and a page (best and last lap, the fuel as the
  game's bar, "Overlay: on/off" and "Units" buttons that write settings and change their own text, Back): all of
  it driven by keys in the lab. The kit test mod asks for an item and a page with no game: the item kept, the page
  refused, the setters 0. `check_builds.py` checks the menus' addresses in the 1.2 files.

## 2026-10-13: the menus' mouse, the main menu, a constructor of the game's interface

- **The owner's direction**: the cursor driven by agents, the main menu tried, sliders and scrolled lists, "and
  whatever else the game has: as complete a constructor of the interface as it allows".
- **The menus' mouse** (disassembly, then the lab): the menus' loop calls LS3D's mouse read (`ls3d+0x71E20`, a
  DIMOUSESTATE at `ls3d+0x1C56F0`) and gives the moves, the wheel and the buttons to the input tick, which adds the
  moves to its cursor (`0x6BD8A0`, pixels), focuses the control under it once it moved more than 10 pixels, and
  hands the buttons to the focused control. The read is hooked like the keyboard's: a move is written as the
  difference to the cursor, so it lands exactly. Seen: the cursor on (150, 256) and Carcyclopedia underlined by the
  game; Free Ride opened, the police slider clicked (0.19) and dragged (0.81), OK clicked (the freeride started),
  Exit Game and the dialog's Yes clicked, a profile chosen, the load menu's scroll arrow held (12 rows down). The
  wheel reaches the focused control; the game's lists do not scroll by it.
- **A misclick, and a tool for it**: `menu.state`'s `x`, `y` are inside the parent; clicking at them started Free
  Ride Extreme instead of an item. `menu.state` now gives each control's `screen` place, and `menu.click {id}`
  clicks a control by id.
- **Items in the main menu**: its buttons are 30 high and 25 or 26 apart, and the game's PridejComponentPod puts a
  copy at the bottom of its source and moves by the height: the first item overlapped Carcyclopedia. The loader now
  moves the copy and everything below by the rows' pitch (two items: 163, 189, Carcyclopedia to 215, the window 52
  higher), as the main menu was rebuilt after a freeride.
- **The kinds**: `menu.def` reads any of the game's definitions through its file layer (38 menus, their kinds
  tallied). The factory chain has 38 kinds; **the last session's note that there is no `chck` was wrong** (it had
  read only the chain's second half): there is, with `edit`, `edin`, `bsel`, `bslb`. Each value function read in
  the disassembly (`knowledge/notes/interface.md`); a page with every kind made over the main menu and each
  clicked: the check box 1 to 0, the switch km/h to mph, the selector Normal to Hard, the number 3 to 4, the text
  field typed into ("Don Salieri, 1930!" by `input.text`), the slider, the bar, the scrolled list.
- **Options are text ids**: a selector walks a range of text ids and shows the database's string for each. The
  loader hooks `G_TextDatabase::GetText` / `GetTextOrNULL` and answers a range of ids of its own; a page's texts are
  now given as ids, as the game's files give them. That also fixed a fault: a number field formats its text in its
  constructor, and with no text id it formatted a null (the page faulted while the game made it; the guard caught
  it). **A fault while a page is made used to crash the game a frame later**: the push had made the half-made menu
  the active one, and it was drawn. Now the menu under it is made active again and the page is dropped.
- **A scrolled list hid the page's other controls**: its Draw ends with SetViewport(itself) and nothing restores
  the clip for what is drawn after it (the load menu has nothing after its list). The loader puts the list's block
  last in the definition (the keyboard goes by id, not by place). In a definition "end" returns to the topmost
  window, not one level up: containers do not nest.
- **Pictures**: `rctt`'s slot 17 makes an engine texture from a file name (the load menu shows saves' shots so).
  A bitmap in the data folder loaded by full path and by a path from the game's folder: icons for achievements.
- The owner clicked about on the test page by hand while it was being checked (and said so): the slider's and the
  number's extra changes in that first check were his, not the tools'.

## 2026-10-14: other projects' knowledge

- **The owner's direction**: find open-source work on Mafia and take the most from it, so as not to reverse
  everything anew. Searched GitHub (repositories and code, by the game's class names): MafiaHub's MafiaOnline (a
  live multiplayer for exactly our 1.2 `Game.exe`: its SHA-256 is our `re/Game.exe`'s) and Oakwood (1.2), reM and
  Marvisak's byte-exact mafia-re (1.2), st0rm94's reverse-engineering export (1.0, 5098 named functions, 114 class
  documents, the script machine), mEdit's LS3D headers, OpenMF's format parsers, Insanity3D (LS3D's ancestor, its
  SDK documentation). Cloned into `re/ext/` (ignored); `knowledge/notes/external.md` says what each holds, on
  what terms, and where to read by topic. Only facts come in, each with its source.
- `tools/kb_external.py`: every address checked against our files before it is kept (a function's must start
  one; MafiaOnline's contract rows only where their original bytes are ours: one was not). 3146 Game.exe names,
  1141 LS3DF names; the MCP server reads them beside ours (`external.csv`).
- **1.0 names to 1.2**: st0rm94's `Game.exe` 1.0 is the same code as our `re/v10/Game.exe` (its dispatcher at the
  same address), though not byte for byte the same file. The functions of the two builds are paired by their code: exact code with
  the image's addresses taken out, the strings a function uses, then the call graph from every pair (the calls
  aligned in order, the callers chosen by the likeness of their instructions), seeded with `game_build.h`'s pairs.
  The first matcher (exact code and same-length call lists) paired 1587 and missed 7 of 17 known pairs (a
  vehicle's fields sit 0x7C apart between the builds, so its functions' code differs); the second pairs 2726, all
  17 right, and agrees with the decompile's class on 886 of 889 named functions.
- **The script commands, read from the binaries**: the keyword table is built on DecodeInstruction's stack (1.0
  from immediates, 1.2 from copies of data), so it was taken by running that function in an emulator (unicorn) on
  an unknown word and reading the stack; the handlers from the dispatcher's jump table. 1.2 has 396 commands, 1.0
  389: seven new ones (inverse trigonometry, 2D vector rotation, `GRABER_SETFOCUS`, `FRM_SETON1`) shift 1.2's
  opcodes, so 1.0's table cannot be used for 1.2 as it is. The 1.0 handlers read here equal st0rm94's.
- **IGraph's interface** (mEdit's header) names the slots this project had found by reading LS3D's code: all of
  them agree (CreateITexture 1, Present 15, SetState 17, DrawPrimitiveList 22, UpdateKeyboardData 38, ReadKey 39,
  ReadChar 40, UpdateMouseData 55, Mouse_rx/ry/rz 56-58, GetMouseButtons 62).
- A here-document turned `\b` into a backspace character in the tool once (AGENTS.md's trap again); the sources
  touched were scanned for control characters afterwards.

## 2026-10-14: 1.0, in the 1.0 lab

- **The owner's direction**: use `W:\Mafia10LAB` (1.0) and check everything for 1.0. Its `Game.exe` and `LS3DF.dll`
  are byte for byte mafia-ragdoll's `re/v10`. `game_dir` switched to it, the loader installed (`lab.py mode
  loader`), the game driven by keys (its start and menus are not known for 1.0).
- **What ran as it was**: the host and every game hook, the mission's events (`00menu`, `freeitaly`, the pause),
  the drawing (the example's overlay and a bridge text over the HUD and the pause menu), the car (every field; a
  state saved, the car driven 11 m, the state loaded: the position back to the last digit), keys and hotkeys (F9
  started the example's lap), settings (`config.set units`), the core mod's commands. None of the kit had been run
  on 1.0 before: its 1.0 addresses, found by code match, all held.
- **What did not, and was fixed**:
  - `screenshot`: the 1.0 lab renders on plain Direct3D 8 with a multisampled back buffer, which `CopyRects`
    cannot read. The fallback copies the front buffer (the screen) and cuts the game's client area out of it. **The
    first time, the owner had another program in front, and the shot held that window, not the game**: the file
    was deleted unread and the fallback now refuses unless the game's window is the foreground one.
  - `input` refused every step on 1.0 ("not known for this build") because the actions were not known, though the
    keys were: now each step is checked for what it needs.
  - The actions: the controls' update is the same code in 1.0, its arrays 4 bytes lower (bindings `+0xB0`, held
    `+0xC0`, pressed `+0xD0`, read in both builds' code); now per-build fields. 60 slots in 1.2's order; `Steer
    Left`, bound to nothing, turned the wheels to -0.785; `turn` 90 and -45 measured 90.0 and -45.0 (the mouse
    sensitivity found at the same place of the on-foot input). The texts' database: the same code, its object the
    ECX of 91 calls.
  - The MCP server named 1.0 addresses with 1.2's knowledge base (`tap` on 1.0's `C_mission::Tick` printed a 1.2
    HUD function): now it reads the build from the loader's log and names a 1.0 address through its function's 1.2
    twin, or not at all (the session's own server gets it at its next start).
- **What is other code in 1.0** (read, not ported): the logos (AVI files played by `InitSystem` through IShow,
  after `cd_find_game_file`), the modal menu loop (it does not tick the mission), and the menus themselves: a
  6-slot vtable a menu (OnClick in slot 5), the OnCreate its constructor, the controls at `+0x20`, a control laid
  out differently (no text pointer). The start, `menu.*` and the menus extended need a 1.0 layer
  (`docs/VERSIONS.md`).
- `tools/check_builds.py`'s controls checks were 1.2's first instructions; they are now the facts the code relies
  on (the object is ECX of the update's calls, the arrays' offsets in its code...) and run for both builds.
