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

- A MafiaReverse folder (full Hex-Rays decompiles of the 1.2 Game.exe and LS3DF.dll with indexes) brought
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

- **The owner's direction**: an achievements mod must sit in the game's real interface, an item of the
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

- **The owner's direction**: use the 1.0 lab and check everything for 1.0. Its `Game.exe` and `LS3DF.dll`
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

## 2026-10-14: 1.0 at 1.2's level: the start, the menus; the game in the background

- **The owner's direction**: "full support of 1.0 in MafiaModLoader, at the same level as 1.2" (and: maybe 1.0
  has no intro videos). Read first: st0rm94's 1.0 reverse (`structs/gui_screen.md`, `gui_widget.md`,
  `game_menus.md` and the annotated decompile), then each function in mafia-ragdoll's 1.0 Ghidra project
  (`tools/ghq.py --v10`): the calling conventions and every `ret`.
- **The logos**: 1.0 plays `logo1.avi` and `logo2.avi` only when `cd_find_game_file` finds them (on the disc); the
  owner was right that the lab plays none (its start was 2.5 s to the profile menu). The start answers "not found"
  for the two names all the same.
- **1.0's menus are another model**, not other offsets: no command handler (each screen's Tick switches on what
  the base `gui_menu_tick` returns), one design table for every screen, containers with no `end`, other tags. So
  the 1.0 layer is a file of its own (`host/menus10.cpp`, sharing the registry, the texts and the delivery with
  `menus.cpp` through `menus_shared.h`): `gui_menu_tick` is hooked and a command is its answer at the screen's next
  frame; the mods' items are added when a screen is first seen (its tick, or the loader's frame walking the stack)
  by the game's `gui_menu_clone_component_under`; a page is built by the game's builder from a design of the
  loader's and made the active screen. The ids are 1.2's, so the start and the example's "gamemenu after 108" item
  ran on 1.0 unchanged.
- **Seen in the 1.0 lab**: the start (profile "Run", the main menu, the freeride with police 0: the density
  globals 1, 1, 0) in 3.0 s; `menu.state` of every screen; the freeride opened by `menu.command 990105`, its
  slider set, OK clicked by the mouse; the example's item in the pause menu and its page: texts, the fuel bar, the
  check box and the selector (the game's drop-down) told to the mod (its settings written), Back; the mod reloaded
  with its item shown (taken over); two items in the main menu (rows 25 apart and 30 high: moved by the pitch); a
  bridge page with every kind (a number stepped, text typed, a list scrolled by the wheel, a selection set); a
  page with no menu open (the pause menu opened under it). And 1.2's lab afterwards: the start, the item, the page.
- **Found and fixed on the way**: the page's rows at first sat in its title (a 40-unit shift of mine: both builds
  measure a window's controls from under its title); a mod closing its page from its own click (Back) would have
  freed the page under the loader's tick (the page is looked up again after each delivery); a command given to an
  item's id skipped the item's delivery; a page put over the pause menu before its first tick kept the mods' items
  out of it (the frame now walks the stack); the freeride's kind was read at 1.2's offset ("kind 218169528").
- **The texts' code page**: the bridge turned "é" into "й" (the system's ANSI code page here is 1251); the game's
  texts are 1250 (its text database): `Utf8`/`Ansi` take 1250, on both builds.
- **The owner moved the mouse** during a click test ("sorry, I moved the mouse"): the menus' cursor went to the
  corner and the click missed. Not a tool fault: the game reads the real mouse while it is in front.
- **The game in the background** (the owner: "can we start the game without bringing it into focus?"): the MCP
  server starts it with `SW_SHOWNOACTIVATE` and `start.json`'s `background` makes the loader answer user32's
  activating calls for the game's windows; that alone still let the game come in front: LS3D's own `SetFocus` did
  it, now skipped while the game is not in front (with its `ClipCursor` and `SetCursorPos`, which would take the
  owner's cursor). `CreateWindowExA` could not be hooked (MinHook: unsupported) and was not needed. `game_launch`
  now tells `in_front`: false on both builds; keys, the menus' mouse and screenshots worked behind other windows.
  With the owner's go-ahead the 1.0 lab got dxwrapper's Direct3D 9 (`d3d8.dll`, `dxwrapper.dll`, `dxwrapper.ini`
  copied from the 1.2 lab; it had no `d3d8.dll` of its own), so screenshots work there without the game in front.
- `check_builds.py` checks 1.0's menu addresses (each tick, the base vtable, the builder's `stop` and `ret 0xC`, the
  cursor's floats, `cd_find_game_file` called with `logo1.avi`): ALL PASSED. `mml.config.json` back on the 1.2 lab.

## 2026-10-14: the game's script machine, for agents and mods

- **The owner's direction**: the next item of the roadmap, "the engine's own commands from MCP": "pump up the
  agent's hands, as many abilities as there can be", and the same for mods.
- **The way chosen**: not each command's handler called by hand, but the missions' own interpreter given a program:
  every command, with the game's own behaviour, at once. MafiaOnline does not run the game's programs (it rewrites
  the commands it needs); st0rm94's `structs/program.md` describes `C_program` whole (its layout, the same in both
  builds, its vtable, Compile, CallProcess, END leaving the program on it). A standalone `C_program` of the loader's:
  the game's new and constructor (the standalone vtable, whose Process is the full switch), Compile stopping at the
  first bad line (its offset gives the source line), the registers allocated with the game's new, CallProcess once
  a mission tick after the mission's own. Init is not called (it frees the registers and compiles again).
- **Learned in the lab** (1.2, then 1.0): `wait 300` took 22 ticks; an unknown command fails with its line; the
  argument parser is lenient (`actor_setpos zz, qq` compiles to 0, 0). The player in a car is the car's child:
  `frm_setpos` on his frame moved him in the car's space (put back by hand); a car teleports with
  `getactorframe` + `frm_setpos` + `actor_setpos` + `car_repair`. `findnearactor` with its kind in quotes (as
  st0rm94's note writes it) finds nothing: the compile does not recognise the kind; bare it works (`Car`). Traffic's
  cars are not actors of their own (`Traffic_car` finds the manager). `actor_duplicate` cloned a car, placed ahead;
  `explosion` needs an owner (the actor the program runs as: a new `owner` argument), it went off at the clone.
  Rain on and off, traffic hidden and shown. The program's source texts of the freeride's 67 scripted actors (read
  from their `C_program +0x08`) showed the real syntax (`FINDFRAME 0, "police1"`, `if flt[0] = 0, -1, 101`).
- **For mods**: `script_run(mod, code, owner, acts, flts, timeout, fn, user)`, `script_stop`, `script_state`; the
  telemetry example's F7 repairs the car through it: hot-reloaded into the running 1.0 game, F7 pressed, "car
  repaired" in the log one tick later.
- **MCP**: `script` (runs and waits; failed and stopped programs are errors with their state) and `script_commands`
  (opcodes and handlers of both builds from `script_commands.csv`; st0rm94's note on each case read in place from
  `re/ext`, never copied). The tap tool's text still said "the owner": now "the user".
- The tests drove the 1.0 lab through `MML_GAME_DIR` (the MCP server's and the bridge client's override), so the
  user's `mml.config.json` was not touched. `check_builds.py` checks the machine's addresses in both builds.

## 2026-10-14: the world without a program

- **The owner's direction**: "continue the work": the roadmap's next item, more hands past the script machine.
  Done here: actors of any model made new, moved, seated, armed by name, taken out; the rest (orders for the people
  made, the time of day, a knock-down that always works, the scripts' events) stays next.
- **The way chosen**: no mission command makes an actor from a model file, but two cases of the interpreter do it
  for their own ends: `ACTOR_DUPLICATE` (a copy of an actor's model) and `DAN_INTERNAL_1` (a developer's command
  making a "cheat_car", a Bolt-Thrower, at a fixed place). Both were read in both builds' disassembly; the loader does
  what they do with a model file of the caller's (MafiaOnline's human factory and car code do the same, and gave the
  list of bones a person's model must have). Moving, seating and arming are the missions' own commands, run by the
  game's interpreter at once (`ScriptExec`: compiled and stepped until it stands on its end, in the same frame),
  not by hand-made copies of their handlers (`ACTOR_SETPOS` alone ends in a light-sampling tail and a car's reset).
- **Learned in the lab** (1.2): the spawned Bolt-Thrower was a car of the game's in every way ("Get in the car as
  the driver", driven 14 m, its speedometer shown); a spawned Paulie held a given Thompson; `human_force_settocar`
  put him beside Tommy and he rode along. The table of items: 188 bytes a record, the magazine at `+0x60` and the
  reserve at `+0x64` (read against the Colt's 7 and the Thompson's 50); the car list's names and the models' prefixes.
- **A fault of mine, seen by the owner** ("Paulie still stands there"): a removed Paulie's model stayed in the scene.
  The actor was gone, its frame still the primary sector's child: the sector holds the frame. The loader now
  unlinks a removed actor's frame from the scene in the destructor's hook (`LinkTo(null)`), and frames.find shows
  nothing left. Also refused now: removing a car someone sits in or a person sitting in a car (the car's frame is
  his parent; MafiaOnline notes the game's removal does not get them out).
- **1.0** (through `MML_GAME_DIR`, the user's config untouched): the same tables at their 1.0 places (the car list
  164 bytes a record; the driver's CreateFrame one slot lower, +0x4C), spawned a car, Paulie into the player's car,
  Sam with a Colt, teleported the car, removed Sam (his model gone), refused Paulie. The 1.0 profile's camera is the
  bumper's: a car spawned 6 m in front was right before the lens; the default distance for a car is 8 m now.
- **For mods**: the telemetry example's F6 makes a car of its settings' model in front of the player and takes out
  the one it made before (seen in the 1.2 lab). The items' names are code page 1250 text (a sword's is not ASCII):
  read as UTF-8 for the bridge.
- `check_builds.py` checks the recipe's calls and the tables' places in both builds (ALL PASSED; a wrong driver slot
  put in by hand failed it). A Python here-document ate backslashes again while editing it (AGENTS.md's trap): the
  edit was redone with the editor's tool.

## 2026-10-14: the roadmap reviewed; the start completed; a guard against the shell's backslashes

- **The owner**: "the roadmap was not mine but yours, written long ago: there may be absurd items, I hope you notice
  them". Each item was weighed by whether a real user exists for it, checked in the game where it could be: the
  time of day has no clock to set (night is a separate scene, `GAME_NIGHTMISSION` only switches lamps); the
  knock-down was the ragdoll's need; scenarios run by the loader would be a second scripting engine in the game;
  LuaJIT was argued for with a need the tools now cover. The 64-bit sidecar was taken out as having no user too,
  and **the owner put it back**: the latest PhysX, and modern libraries like it, exist only for 64 bits; a 32-bit
  game reaches them only through a 64-bit process beside it. It is item 2 of "Next", with PhysX 5 as its first user
  and a measured round trip before its design is fixed. A memory note keeps the lesson: a library the game cannot
  load is a user.
- **The backslashes** (the owner: "maybe fix this trap for good?"): measured again with `od -c`, the agent's Bash
  tool turns every doubled backslash into one, in here-documents, single quotes and echo alike, not only
  here-documents as AGENTS.md said. It cannot be fixed in the tool, so a PreToolUse hook (`.claude/settings.json`,
  `tools/bash_guard.py`) refuses any Bash command with a doubled backslash and says what to do instead. It fired in
  this session's own next command that had one.
- **The start, completed**: the main menu's handler, the freeride menu's, its kind menu's and spawn menu's and the
  load menu's were read in 1.2's code (the kinds' missions from `MafiaEntryPoint`'s jump table, not from the
  decompile's case labels), then driven by commands by hand before the code was written: the kind menu's choice
  lands in the freeride menu's `+0x30`, its OK opens the spawn menu. The steps find the screens by their
  definitions' names, so the same code drove 1.0's screens (other classes) unchanged.
- **A fault of the loader's, seen on the way**: a French save's name came out "Pičge": the texts were read as code
  page 1250 everywhere, and `input.cpp` even used the system's (1251 here). The language is `+4` of
  `tables\lang_<ll>.def`'s record (1.2 had it as `menuLang`; 1.0's found at 0x661C6C); its code page is used now.
  English and French were seen; the others are a table of Windows' code pages.
- **Not run**: `"go": "new"` and `"extreme"`: a new game writes the profile's first saves over the owner's, and that
  is not this session's to do. A freeride start writes `mafiaNNN.570` and `.sav`, as every freeride start did before.
- A doubt about a tool was settled on the way: `player` said Tommy sat in a car while the screenshot showed him on
  foot; read again, he was on foot: the first reading was taken in the chapter's opening scene, where he arrives by
  car.

## 2026-10-14: the roadmap aimed at four mods

- **The owner**: no ragdoll port, no VR, water or smoke mod here: the toolkit, so that another agent can write a
  ragdoll, shader water, realistic smoke and a VR mod on the loader; the old tasks stay, duplicates merged, sorted
  by importance.
- **How the list was made**: mafia-ragdoll's sources and documents were read (not changed) for what its host and core
  build for themselves: a Direct3D 11 bridge beside the game's Direct3D 9Ex (`gfx.cpp`), the scene drawn again from a
  mirrored camera (`reflection.cpp`), the world's triangles from the visuals (`scenery.cpp`), every human in a
  registry, poses written right before Render, shots and pedestrians' hits hooked, PhysX 2.8.4 and NVIDIA Flow and
  FleX loaded beside it. Each became a service of the loader in "Next", tiered: what blocks the four mods (a heavy
  mod's lifecycle, render events and the bridge, the scene from another camera, the frame measured), the game's
  world as data (people and skeletons, shots, geometry, the camera, effects, a tuning panel), the 64-bit sidecar,
  then the agents' own tools.
- **Found on the way**: the loader loads a mod's copy from `mml\live\` with a plain `LoadLibrary`, so a DLL a mod
  ships beside itself (PhysX, Flow) would not be found: item 1, the first and the smallest.

## 2026-10-14: a heavy mod's lifecycle, the loader's part

- **The owner**: "continue the work": the roadmap's first item. The loader's part is done here; Jolt in
  `tools/deps.py` and shaders compiled in a mod's build stay as item 1.
- **Libraries**: each load now runs from its own folder in `mml\live\` (the mod's DLL as `mod_<id>_<pid>_<n>.dll`,
  so crash reports and `where` keep naming it, and a copy of every other DLL of its folder). Copies, not the mod's
  folder in the search path: a library loaded from the mod's folder would lock it, and a deploy could not replace it.
  `LOAD_WITH_ALTERED_SEARCH_PATH` finds the imports; `SetDllDirectory` during `mml_mod_load` finds bare-name loads.
  Windows reuses a loaded DLL of the same base name, so a library an older load never freed is used by the next load:
  the loader now says so.
- **Threads at the unload**: every other thread suspended in turn (nothing allocated while one is suspended: it may
  hold the heap's lock) and looked at. A first version scanned every word of the stack and kept a test DLL loaded for
  good: a thread pool thread held `heavyrt.dll+0x3017` (the CRT's `try_get_function`) in stale locals, left there by
  `DLL_THREAD_ATTACH`. Now: its EIP, the return addresses along its saved-EBP chain (Windows' functions keep frame
  pointers, so a wait called from a mod's code is seen; frames without one keep EBP and are passed over), and its
  start address. A pool work item sleeping in the mod's code is seen by the chain alone (tested).
- **A doubt of a tool's, settled**: the wait for threads that are ending was counted in `Sleep(25)` steps, but one
  look at the threads took about 30 ms (a Toolhelp snapshot walks every thread of the system), so "a second" was
  2.5 s and the test's thread ended inside it. The wait is on the clock now, and `NtGetNextThread` walks only the
  game's threads: 0.2 ms a look, 0.4 ms a measure. The rest of a reload (about 85 ms for three new DLLs) is
  `LoadLibrary` itself, likely the antivirus reading new files.
- **Measured in the 1.2 lab**: `mod.reload {times: 20}` on core and telemetry: 0 MB of address space, 0 handles, 0
  threads lost a reload (3381 MB free, the game is large-address aware: its largest free block 2046 MB); nothing kept
  by mistake among the game's 18 threads; telemetry's overlay drawn after the 40 reloads.
- `tests/run_tests.py`'s `check` could not print a tuple as its detail (a latent fault: it raised instead of
  failing); fixed.

## 2026-10-14: the frame rate freed, the example mod fps_unlock

- **The owner**: a correct FPS unlock with the game's functions freed from the frame rate, done on the loader and
  kept as an example. They remembered it in MafiaReverse; it was not there (decompiles and indexes only), nor in
  mafia-ragdoll; it was MafiaD3D11's A132 (a local checkout, read only), and A132 is the last version
  of its timing in every copy of it (the newest, `-lf-thr2d2-patchcheck`, changed only its log). The owner: "look at
  the pure A132". The fixes of what breaks above 62.5 fps that the owner remembers were found in no copy of that
  project (its runtime touches only d3d8's vtables and those two LS3D places), so they were looked for anew.
- **A132, read in the game's code**: the limiter (stdcall(IGraph, min, max)) Sleeps a short frame up to `min` and
  returns it; the main loop asks (16, 83), the menu loop (16, 249), and the answer is every system's time. Two bytes:
  the `jae` before the Sleep made `jmp`, Present's `push 0; call Sleep` jumped over. 1.0's places found by the same
  code (the limiter byte for byte, Present's call through `edi`), the shapes checked by `check_builds.py` (a wrong
  address put in by hand failed it).
- **The mod**: patches only bytes it finds as the game's (another tool's patch is left to it), puts them back at the
  unload, a cap of its own (a high-resolution waitable timer and half a millisecond spun), the frame rate and the
  game's clock step drawn and in `fps_unlock.status`. Seen: 194..224 fps in the freeride, 700..1100 in the main menu
  with steps of 0..3 ms; `enabled` false gives 63 fps and steps of 16 back at once.
- **Measured unlocked against locked**: the taxi's 4 s of throttle (21.4 m against 21.5 and 21.7), 3 s of running
  (14.90 against 14.88 m). The owner then named the clearest breakage: the main menu's camera flies too fast. Its
  tick (0x5EC8A0, inlined in 1.0) moves by the time but springs the velocities once a call (0.98 / 0.02). A trace of
  the camera in the mod (`fps_unlock.camera`) measured one flight: 10 / 50 / 90 % of the way at 0.86 / 2.78 / 6.53 s
  locked, 0.43 / 2.36 / 7.67 s unlocked (the model's first-order 0.3/s), 0.88 / 2.77 / 6.54 s with the spring stepped
  by the time (`k = 1 - 0.98^(ms/16)`, a hook that corrects the velocities after the game's tick).
- **Doubts of the tools, settled on the way**: `input`'s `ms` is wall time (`GetTickCount`), not frames, so its
  measurements hold at any rate. `car.restore` refused a state saved from the same taxi: the size counts the dents,
  and the taxi had touched the curb (Known gaps; not changed without reading `LoadVehicleState`). `frames.find` has
  no scene root in the main menu. A first test hovered a submenu's items: those do not move the camera (only the main
  menu's do), so nothing moved; the next ones hovered the main menu.
- `game_build.h` grew (the two LS3D places, the menu camera): mafia-ragdoll's copy is not changed from here.

## 2026-10-14: car.restore with dents

- **The owner**: "do car.restore". The game's load (0x4CFAB0, read) checks only the format; each body part's record
  is found by its code and writes only the vertices it lists, so the core's "same size" test was wrong twice: it
  refused the same car with other dents, and a looser test alone would leave a crashed car's extra dents. Now
  `CarStateLoad` repairs first (the scripts' CAR_REPAIR function, checked by `check_builds.py` as what its handler
  calls in both builds) and `CarStateFits` compares the head's counts (lights, body parts, seats, wheels).
- A first model test compared the frames' names, and refused another taxi of the same model: every car's frame has a
  name of its own (`taxi00_mml2`). Measured in the 1.2 lab as written in the roadmap's Done; `car.save` now
  survives the core's reload (`state_put`), and `car` tells `state_bytes`.

## 2026-10-14: the main menu camera in 1.0

- **The owner**: "then the camera in 1.0". 1.0 has no TickCamera: st0rm94's decompile marks it inlined into the
  menu loop (0x56CCC0), and it is another camera, not 1.2's spring: 10 substeps a frame of two lags (`* 0.001`,
  `* 0.002`) with no time in them, its state the loop's locals. So unlocked it ran 17 times as fast (1315 fps).
- The fix points the loop's 12 `fmul dword [const]` operands (found by the disassembly, checked by
  `check_builds.py`: six of each constant between the loop's counter and its jump back) at two floats of the mod's,
  set each frame from the game's step; the constants themselves are shared (0.001 has 218 uses). The floats live in
  the process's heap and the operand list is kept with `state_put` at once: a faulted mod's unload is not called, so
  the game's code must never be left pointing into a freed DLL.
- Measured through the scene camera's frame (`+0x17C`, `GameCamera` in both builds), as the menu camera's state is on
  the stack in 1.0: the method was first run on 1.2 (1.1 / 3.8 / 9.4 s there, not comparable with the eye's own
  numbers: the frame carries the sway). The 1.0 lab driven through `MML_GAME_DIR` (a scratch bridge client), the
  user's config untouched; the mod's settings put back to their defaults and the 1.0 game closed by its pid after.

## 2026-10-15: a person's step at curbs above 62.5 fps

- **The owner**: the breakage they remembered is Tommy walking into a curb instead of stepping up (not the bending
  plants, which a first look went to: `bendable_plant_ai` stays a lead in the notes). They showed the curb (12-02 "The
  Parking Lot", Tommy at (-8.49, -0.03, -153.19) facing -z) and asked that the movement's results stay the game's
  own: the logic may be rewritten, the outcome may not change.
- **Found in the code**: the move's sweep (`C_human::DynSetNextCollOwnerMult`, three spheres) is aimed 0.015 m lower
  than the move once a call; a curb is climbed by the slide on its edge, and a 2.5 ms walking step slides down where
  a 16 ms one slides up (`knowledge/notes/game.md`, "The frame rate"). Proved in the lab by pointing the one
  operand at the game's 0.004: walking climbed.
- **A first fix was wrong**: the push scaled by the time (then with the fall a 16 ms frame has) climbed the curb, and
  also a 0.3 m ledge under a shop window that Tommy cannot climb at 62.5 fps; the owner saw him climb walls. Many
  small slides follow a sphere's curve where one 16 ms step is stopped. The fix now asks the game's own sweep about
  the 16 ms step from the same place and moves the frame's share of it.
- **A doubt of a measurement, settled**: the first 62.5 fps walks stalled 0.7 s before the curb and drifted sideways,
  differently each time; a per-frame trace (`fps_unlock.path`) showed the stall 0.6 m before the edge, and `humans`
  showed Paulie and Sam standing there (they follow Tommy to where the last walk ended). With both moved away first,
  62.5 fps walks straight up, and the fix matches it (the numbers in the notes). End points alone (`input`'s answer)
  could not have told the two apart.
- A power cut closed the game midway; the curb's place was kept in a scratch file and reached again by `teleport`.
- 1.0's twins found by st0rm94's names and the disassembly (the same code, the same `+0x40C`), checked by
  `check_builds.py` (a wrong state offset put in by hand fails it), not run in the 1.0 lab.

## 2026-10-15: a stopped car at rest above 300 fps

- **The owner**: above 300 fps a stopped car keeps drifting by millimetres and comes to rest late, which stalls
  missions whose scripts wait for the car to stop; at 62.5 fps it is still at once.
- **Measured first** (`fps_unlock.path` now records the player's car: position, speed, velocity and any vehicle field
  by offset): at 400 fps the taxi stopped, then circled at 0.01..0.03 m/s in a 4 x 3 cm loop for 8 s; at 62.5 fps its
  residual speed fell x0.89 a frame and the game zeroed it 0.07..0.75 s after the stop. The game's rest logic was read
  (Move's zeroing under 0.01 m/s and 0.015 rad/s, Tick's sleep after 1 s calm or 4 s within 0.15 m): all in m/s and
  ms. What differs is the physics' step: Engine runs Move in substeps up to 28 ms, so one 16 ms step a frame at
  62.5 fps and one 2.5 ms step at 400 fps, and the small motions are damped less in short steps.
- **Two wrong turns, each measured**: stepping only Engine in 16 ms braked harder; so did stepping the whole Tick.
  Both because SetBrake's ramp adds the vehicle's last step time each call, and the controls are set every frame:
  16 ms a frame ramped the brake 6 times as fast. Leaving the frame's own time there after each frame fixed it.
- **The fix**: a car slower than 1 m/s is ticked in whole 16 ms steps. Those steps reproduce 62.5 fps to the digit
  (a teleported car's settling, read at 4 decimals). The threshold keeps faster cars in the frame's steps: a car
  ticked at 62.5 Hz at speed would stutter on screen (nothing draws it between steps yet; Known gaps).
- Both builds' addresses in `game_build.h` (1.0's offsets read in its code: the flags `+0x224`, the step `+0x544`),
  checked by `check_builds.py` (wrong offsets put in by hand fail it).

## 2026-10-15: a car's physics above 62.5 fps, the cause fixed: LS3D's small rotations

- **The owner**: no 16 ms ticks or other hiding: fix the cause so the physics is right at any step and gives the
  original's results; the same for walking (the curb fix that asks a 16 ms sweep is to be replaced too). Everything
  reversed goes to the knowledge base / SDK so other agents need not redo it.
- **Found**: LS3D's `S_quat::RotationMatrix` (0x1002EC00) returns the identity whenever `|w| >= 1`, and a turn under
  ~0.00049 rad has `w == 1.0f`: the car's body cannot turn slower than 0.03 rad/s at 62.5 fps, 0.2 rad/s at 400 fps
  (the steering's dead zone, the stopped car's creep). Proved with `fps_unlock.stepprobe` (axes frozen bit for bit).
  All of it, with the C_Vehicle step pipeline, S_wheel layout and rest logic: `knowledge/notes/vehicle_physics.md`.
- **The fix**: `fps_unlock` hooks the export by its name (`fix_rotation`): a unit quaternion whose `w` rounded to 1
  gets its matrix by the game's formula, everything else is the game's. Measured in the 1.2 lab at 330..380 fps with
  a scratch bridge client (each test five or more runs): at rest 0.23..0.43 s after slowing down (62.5 fps:
  0.07..1.47; unfixed: still creeping at 7.8 s); 0.15 of steering, a yaw rate of 0.161..0.167 rad/s (62.5 fps: 0.161;
  unfixed: 0.019..0.055). The 16 ms car ticks (`fix_car`, `car_fixed_below`) are gone: the frame's own steps give
  those results. `vehicleTick` stays hooked for the step probe; its fields in `game_build.h` stay as knowledge.
- **At 62.5 fps** the fix changes the original a little: it keeps 200..230 turns a stop test that the game drops
  (under 0.03 rad/s); the stop's time and the steering stayed inside 62.5 fps's own spread. Said so in the notes.
- **A measurement's doubt, settled**: the plan's steering numbers (-1.86 against -0.49 m/s "sideways") did not come
  back; read along the body's right axis the sideways velocity is near zero at both rates. By their size they were
  the world's x velocity, the heading more than the slip. The body's axes were also told apart on the way: `+0xC3C`
  is forward, `+0xC48` right, `+0xC54` up (read against the frame's world matrix).
- **Other users**: 26 call sites in 1.2 (35 in 1.0), listed by function in the notes. Watched with `tap`: driving
  calls it from `Move` only, walking adds `C_human::PoseInterpolate` (a bone's absolute rotation: the fix makes a
  near-identity one exact). The airplane, `C_PhThing`, bottles and the traffic turn by a step's angle and were not
  watched.
- **1.0, run** (the owner: "check it on 1.0"): the 1.0 lab driven through `MML_GAME_DIR` (the repository's MCP
  server started by a scratch client, `mml.config.json` untouched), the 1.2 game closed first so the two did not
  share the machine. 140..185 fps there. A stopped taxi at rest 0.23..0.29 s after slowing down (62.5 fps:
  0.37..2.19 s; unfixed 3.9..7.6 s); steering hardly differs at that rate (the lost turns are under 0.07 rad/s at
  150 fps). The test's first 1.0 runs read 1.2's offsets (the angular velocity a constant 0.78); 1.0's body axes and
  angular velocity are 1.2's plus 0x7C, found by matching the frame's world matrix in the vehicle's memory.
- **Still open**: the wheels' spin fields change differently in one 16 ms step and four of 4 ms (oscillation or a
  per-call integration: not known); walking (`Stepped`, the 16 ms sweep) is still the next real fix; the car's
  fields for the SDK (`game.h`, `check_builds.py`).
- **Lab state**: the lab's `fps_unlock` changes put back to the defaults (`{}`); the freeride left running.

## 2026-10-15: the car's physics in the SDK

- **The owner**: commit the rotation fix locally (done: 9981de6, not pushed), then put the car's fields and
  RotationMatrix into the SDK (`game_build.h`, `game.h`, `check_builds.py`) for both builds, so that other agents
  need not read them again.
- **Read in both builds' code**: Engine's substep loop (`+0xC24` / `+0xCA0`, the wheels `+0xC38` / `+0xCB4`,
  their count `+0x4C4` / `+0x540`), Move's body turn (`lea` of the three axes, the angular velocity stored), its calls
  of the wheels' collision and of RotationMatrix's thunk, the suspension's block (byte for byte the same in 1.0),
  C_car's size in CreateActor (0x221C / 0x21F4), the wheel's new (0x1D4 in both), LS3DF's exports. 1.0's fields are
  1.2's plus 0x7C here, each read, not assumed.
- **Found on the way**: `S_matrix::SetRot3` and `SetRot` (about 90 callers) drop small rotations the same way
  (`|w| >= 0.99999999`); in the notes and Known gaps, not fixed. A wheel's second frame is its dummy (`DWHL0`).
  fps_unlock's step probe read 1.2's wheel offsets in 1.0 (it would have read garbage there): it now uses the SDK.
- **Checked**: `check_builds.py` ALL PASSED, and eight wrong values put in by hand (an offset per build, a wheel
  field, an export) each failed it; the core's `car` lists the body and each wheel, the same values in both labs
  (68000 N/m, 0.2 m, 4488 N on the front wheels); fps_unlock's stop at rest 0.25 s (1.2) / 0.23 s (1.0) through
  the SDK; offline tests ALL PASSED. Both games closed after; their `fps_unlock` settings at the defaults.

## 2026-10-15: a person's step at curbs, the cause fixed

- **The owner**: find the real cause of walking into curbs, no crutch (the earlier fix asked the sweep about a 16 ms
  step); later: "running shows it too, the step only has to be a bit higher", and the freeride spawn's curb grows in
  height along its length: a ready test.
- **Read** (both builds' disassembly): the sweep starts its sphere 0.405 above the feet and returns its centre minus
  0.4, aims 0.015 lower; its slide (TestPanak2, st0rm94's collision_test_character_proxy) is a 10-round collide-and-
  slide that starts each round 0.01 off the contact. Three lengths once a call, plus the ground's fall restarted each
  frame: balanced at 16 ms, a curb's edge slid down at short steps. Why scaling the 0.015 alone (tried before) made
  Tommy climb walls: the start's lift and the skin's push stayed once a call.
- **Measured with a new probe** (`fps_unlock.sweepprobe`: the game's sweep called from one place at any step and with
  any lengths, everything put back): the bug reproduced without the game around it; each candidate compared at 1, 2.5,
  4, 8 and 16 ms at the owner's curb, the shop's ledge, stairs, a 45-degree grind and the spawn's curb. Chosen: all
  four as lengths of the time, the feet kept 1 cm up. Rejected: the aim alone (walls), keeping the skin (feet bobbing,
  curbs climbed early and far).
- **The fix**: 22 operands of the sweeps and slides pointed at four floats in the heap (as 1.0's menu camera),
  holding the game's values but during a call; the result's immediate corrected after it. `Stepped` is gone.
  `game_build.h` gained `humanSlide`, `humanSlideOwner` for both builds, `check_builds.py` checks the operands.
- **In the game** (both labs, 190..730 fps against 62.5): the owner's curb walking and running, the ledge (stopped,
  no climbing), down the curb, the stairs (unfixed: stopped before their last, higher step: the owner's report), the
  spawn's curb at three heights (one too high for 62.5 fps: too high fixed too), 1.0's curb head on: the same within a
  few cm and 0.02 s. Not the same: running up stairs about 7 % faster, fixed or not (another cause); 8 mm nearer walls.
- A doubt of a tool, settled: the probe's 45-degree grind disagreed between steps by up to a second; the real game
  there does not climb at any rate (the probe's fall is simpler than the game's); the real game is the measure.

## 2026-10-15: a heavy mod's build: Jolt pinned, shaders in a mod's build, the `boxes` example

- **The task** (the roadmap's "Next" item 1, the build's part): libraries for mods pinned by `tools/deps.py`, Jolt
  first, built for x86; shaders compiled in a mod's build; a heavy example using both, reloaded 20 times in the lab,
  measuring whether a mod's Direct3D 11 device survives (mafia-ragdoll lost its device after 22 reloads).
- **Jolt**: v5.6.0, the commit mafia-ragdoll uses. The GitHub archive's 1250 files were compared with that clone (a
  clean checkout of the tag): all identical; its SHA-256 pinned. `deps.py` takes names (`deps.py jolt`) and keeps the
  hash it unpacked from (`.pinned`), so a changed pin or a broken unpack is fetched again; the configure runs it for
  Jolt (and again when `deps.py` changes). Jolt's own CMake as a subdirectory (it is written for that: only the
  library, its definitions on the target), its options set before: SSE2 only, no GPU compute back ends, profiler,
  debug renderer, object streams, `/WX` or LTO; `/Zc:__cplusplus` given to its users.
- **Shaders**: `mml_add_mod(... SHADERS <file>:<entry>:<profile>[:<name>])`, fxc for shader model 5 and lower, dxc for
  6, both found in the Windows SDK the build targets (its root read from the registry; `CMAKE_WINDOWS_KITS_10_DIR`
  is not set for the Visual Studio generator). A header of bytecode a shader, in the mod's own include folder.
- **`mods/examples/boxes`**: 48 boxes fall onto a floor and a plank in a Jolt world on 2 job threads; a Direct3D 11
  device draws the world every frame with the mod's HLSL (instanced, a structured buffer of the bodies' matrices) into
  a 256x256 target, and a compute shader counts the pixels drawn, read back two frames later without waiting; the
  overlay draws the pile from the side; `boxes.snapshot` writes the picture as a BMP (looked at: the pile, the floor,
  the plank). Its world is kept across reloads with `state_put`.
- **Found on the way**: the counter was never read back in the stand-in game until a snapshot came: a device with no
  swap chain has no Present, its commands wait in the driver's buffer and a `DO_NOT_WAIT` map never sees them done;
  a `Flush` a frame. The first picture was mirrored and then upside down: Jolt is right-handed, the projection
  left-handed; the camera's right is forward x up there.
- **Measured, the stand-in game** (`tests/run_tests.py` `boxes_tests`, and scratch probes that read the harness's
  threads by the module they began in, its modules and handles from outside): each unload takes `d3d11.dll`, `dxgi`
  and NVIDIA's driver (16 threads) out of the process; Jolt's 2 threads end with the pool's destruction; a reload
  loses 0 MB and 0 threads, and 2 handles: NVIDIA's driver keeps a section and a mutex of each device made (2 a device
  made and released inside one load, d3d11 staying loaded; WARP 0). The first unload's `since_load` showed -118 MB,
  +123 handles, +2 threads: that load's life held every other test (the `heavy` test's leaks on purpose) and the
  process's first device; not the host's fault, the test reads the next load's life instead.
- **Measured, the 1.2 lab** (the running freeride, 250 fps with fps_unlock): `mod.reload {"times": 20}`: 0.0 MB,
  0 handles, 0 threads a reload; 20 reloads with half a second of frames between, each followed by `boxes.status`:
  every device drew (about 137 frames, 53.4 % of its pixels), none lost, every one released to its last reference,
  the game's threads 37 -> 37, handles 632 -> 632, its modules the same; then `times: 100` (0.0 MB, -0.04 handles,
  0 threads a reload): 143 devices in one game, the last drawing. A reload with a device takes about 125 ms, the
  device 25 ms (the game keeps Direct3D's DLLs and NVIDIA's 16 threads loaded, so they are not made again). F10 and
  `boxes.drop` drop the pile again. A `since_load` of +2 threads seen once was the system's thread pool (its workers
  begin in ntdll and come and go).
- So a mod's own device survives reloads when it is released whole; what ends a 32-bit game is address space, and
  `mod.reload {"times"}` measures it. The device shared by every mod is still the graphics bridge (item 3).


## 2026-10-15: a mod's own files in the game

- **The task** (the roadmap's "Next" item 2): models, textures, sounds, tables shipped in a mod's folder and opened
  by the game by name, new or in place of the game's; the order of two mods; changes on disk seen where the engine
  reads again. An earlier session had written `host/files.cpp` and the `mml.h` members, not wired or tested.
- **Read**: mafia-re's `rw_data.cpp` (byte-exact): `dtaOpen` opens a loose file before the archives and an absolute
  path as it is, so a mod's file is opened by rw_data itself from its full path and everything after the open is
  the game's own. A tap on `dtaOpen` during a freeride's load: 3002 opens of every kind (pictures, models, sounds,
  tables, the scene); a second load opened them all again.
- **Found on the way**: LS3D reads a presaved copy of a texture (`<name>_<mips>.565/.555/.DX<n>`, LS3DF
  `m_TryPresaved_555_565`, `m_TryPresavedDXT`) in place of its picture when the size matches, and the archives ship
  such copies (`freeride0_1.565`, the speedometer's `2intmph_1.555`): a mod's picture of the same size would have
  been ignored. The loader answers "no such file" for the presaved copies of a mod's picture.
- **Wired**: the mods' `files\` folders read in `Init` (before the game's first table; the stand-in game checks a
  read before the first frame), taken out at an unload or a failed load, kept across a reload (nothing told when
  nothing changed); `"type": "files"` mods with no DLL (`mml_add_files_mod`, `new_mod.py --files`); the host table's
  `file_add`, `file_source`, `file_read`; `files.list`, `files.which`, `files.read`.
- **A tool fixed**: `deploy` copied a mod's top folder only (its `files\` would never have reached the game); now
  subfolders, unchanged files left alone, a changed one put beside the old, the old deleted, the new renamed:
  Windows refuses a rename over a file open even with delete sharing (measured), and the loader opens the mods'
  files sharing deletion (rw_data's `CreateFileA` import) so the game reads the old one to its close.
- **Tests**: a stand-in `rw_data.dll` for the harness (loose files and an `archive\` folder as its archives),
  `files_a` (files alone) and `files_b` (opens names by rw_data's export as the game does): 46 checks; two older
  checks assumed no rw_data in the harness and were made exact; `mcp_smoke.py` checks `deploy`. ALL PASSED both.
- **In the 1.2 lab**: `mods/examples/freeride_picture` (a picture drawn by its own script) in the freeride menu, the
  presaved copy hidden (logged); a new picture deployed into the running game showed at the menu's next opening;
  `mod.unload`: the game's own picture again. Not seen in the game: a model, a sound, a table of a mod's; the 1.0
  lab (Known gaps). Also noticed: `menu.state` did not list the pause menu's "Exit Game?" confirmation.

## 2026-10-15: both builds tested, by rule; the files in the 1.0 lab

- **The owner**: why was 1.0 left as "the owner's decision"? The SDK must work on 1.0 and 1.2, so everything for
  both is tested in both labs (at the end of a block of work, or at once where 1.0's code differs; not every small
  test); a mod for one build stays in its lab.
- **The cause**: an agent-written rule in AGENTS.md ("`game_dir` is the user's choice: change it only when they ask
  for another build"), the same in VERSIONS.md, the roadmap's "in the 1.0 lab once its addresses are found (1.2
  first)", and above all the 1.0 lab's folder written nowhere (one `game_dir`, and searching the disk is not
  allowed): an agent could not have tested 1.0 even wanting to.
- **Changed**: `mml.config.json` lists the labs (`"labs": {"1.2", "1.0"}`); `tools/lab.py use 1.0|1.2` switches
  `game_dir` (the build told by LS3DF.dll's link time, checked against the lab's name), `status` tells the build
  and the labs; AGENTS.md "Builds" says what is tested where; a mod's `"builds"` in `mod.json`: on another build the
  loader does not load it nor give the game its files (`mods.list` says why); every shipped mod and `new_mod.py`
  name both. `lab.py install` copied a mod's top folder only (as `deploy` did): now its subfolders too.
- **In the 1.0 lab**: the files work there as in 1.2 (above, VERSIONS.md); `boxes` ran there for the first time.
  Seen once: `menu.click` on the freeride screen's "Retour" right after it opened did not close it (Known gaps).
  The lab put back on 1.2 after.

## 2026-10-15: the render events (item 3, block 3a)

- **Item 3 split** into blocks: 3a the render events, 3b the Direct3D 11 bridge, 3c the 2D pass into a mod's
  target; each block seen in both labs at its end.
- **Read**: mafia-ragdoll's render hook (`core.cpp` `hkRender`, `smoke_game.cpp`, `gfx.cpp`). In the 1.2 lab the
  mission's scene (`C_mission +0x10`) has the game's callback 0x5BD8D0 at `+0x140` and the mask 6 at `+0x144`: the
  game asks for stages 1 and 2, and does nothing at 1 (its code tests stage 2 only): so the stage-1 point needs no
  callback of the loader's, only `draw.cpp`'s hook. The device's transforms there are the scene's camera.
- **Made**: `host/render.cpp` (events, the device, the camera, the state kept around mods' drawing), the example
  `world_marker`. Its first build faulted (`game::g_exe` null: it never called `game::Detect`); the log said only
  "faulted in an event handler", so the loader now logs where (`<module>+<offset>`), what the access touched and the
  event's number, also for a mod's load and unload (offline test: hook_b's crash at `mod_hook_b_...dll+0x10A2`,
  touching 0x00000000).
- **In the labs**: 1.2: the marker over the taxi; behind a building it is hidden whole, in front of the facade seen,
  the frame otherwise unchanged; 1.0: the same events and the marker under the HUD's tips. (A camera looking up in
  one shot was the owner's.) Both labs back on 1.2.

## 2026-10-15: the graphics bridge (item 3, block 3b)

- **Made**: `host/gfx.cpp` after mafia-ragdoll's `gfx.cpp` (its design: the INTZ swap, the shared textures made by
  9Ex and opened by 11, the event queries, the state block of exactly the states it changes, the fog and sun tallied
  from the game's draw calls), for any number of mods through `gfx_pass`, with GPU timestamps a pass, `gfx.state`;
  `mml/gfx/frame.hlsli` for mods' shaders; `mml_shaders` (the loader's own shaders and the mods' compiled alike, the
  SDK on the include path). `d3d11.dll` is loaded at the first frame with a pass: a game without effect mods never
  loads it.
- **Found in the lab**: the bridge's first frame refused: the lab's game asks for 4x multisampling, and a readable
  depth buffer cannot be multisampled. mafia-ragdoll makes the device without it, always; the loader does it only
  while an enabled mod says `"gfx": true` (its manifest is read before the device is made): `Direct3DCreate8`'s
  import patched, `IDirect3D8::CreateDevice` (slot 15) and the device's `Reset` (slot 14) take the multisample type
  out of the present parameters, the log says so.
- **The example** `depth_pulse`: a wave from the player over the world's surfaces, a grid behind it, into the fog.
  In the 1.2 lab: 0.65 ms of CPU a frame for the whole bridge (copy 0.23, passes and their wait 0.40), the pass
  0.07..0.14 ms of GPU; the world's fog found linear 84..280 m; no directional light in the freeride's world (a
  default sun). 10 reloads in a row: 0 MB, 0 handles, 0 threads, drawing after each. Fixed on the way: `gfx.state`
  said "no pass" after a reload (its text was not set again when a pass came back).
- **In the 1.0 lab**: the same (no address of the game's is used): the device made without multisampling, the wave
  drawn, the same fog. Both labs back on 1.2.

## 2026-10-15: the 2D pass for mods (item 3, block 3c)

- **Made**: `gfx_ui` (take, hide); at the scene's stage 2 (`draw.cpp`'s hook) the render target is a shared RGBA8 of
  the bridge's while the game draws its HUD, menus and console and the loader its `draw_*`; separate alpha blending
  (ONE, INVSRCALPHA) makes the alpha the coverage, so the target is premultiplied; with `take` it is blended back
  over the frame (the game draws its 2D once). The passes get the last frame's (`MmlGfxFrame::ui`).
- **The example** `hud_panel` (off by default): the HUD on a turned panel ahead and right of the camera, hidden by
  the world in front; seen in the 1.2 lab (the HUD, the mods' overlays, the pause menu on it; with `take` the HUD on
  the screen as before) and in the 1.0 lab. Both labs back on 1.2.

## 2026-10-15: the scene from another camera (item 4, block 4a)

- **Item 4 split**: 4a the views (the scene drawn again from a mod's camera), 4b the game's geometry hidden or
  replaced around a drawing (water's faces). An earlier session had written `host/view.cpp`, the bridge's targets
  for views (`gfx.cpp`), `scene_view` and `MML_EVENT_VIEW_WORLD` in `mml.h`, 1.0's addresses and the example
  `rear_mirror`, built but not run nor written down.
- **Read** (both builds' LS3DF, `tools/check_builds.py` now checks each): I3D_camera's vtable written by its
  constructor; its update (+0x44) asks the scene's GetSector (+0xB4) and keeps the sector at +0x3F8, makes the view
  (+0x164) and the products (+0x1E4, +0x324, +0x3A4); its setters (+0x50, +0x54, +0x58); QueueVisual sends a
  `'LENS'` visual (type at +0x1D4) to the scene's list at +0x288 (count +0x290), every other type to its vtable
  +0x68; Render reads `camRecompute`. The same offsets in 1.2 and 1.0.
- **In the 1.2 lab**: `check` (the left half of the game camera's frustum drawn again, half the frame wide, compared
  pixel for pixel): black, so the asymmetric frustum and the camera put back are exact. `mirror` first showed a brown
  blur: the camera stood against a wall behind it (the owner confirmed); 11 m forward it showed the facade behind.
  `stereo`: two eyes, the street whole. A new mode `reflect` (the camera mirrored in the plane at the player's car,
  `MML_VIEW_CLIP` 5 cm above it, the frame's size; the pass lays it on the ground within 6 m of the player): the taxi
  upside down meeting its wheels, the lamp post from its base, the facade, the world marker's diamond (drawn in the
  views by `VIEW_WORLD`). Costs: mirror 1.5..1.7 ms of CPU, reflection 2.9 ms. Reloads with frames between: +2 handles
  for the first few, then 0; mode changes (views made and taken back) 0. `mod.unload`: the frame as before.
- **In the 1.0 lab** (profile `Run`: 1.0's lab has no profile 4): `check` black, `reflect` and `stereo` drawn,
  reloads 0 once warm, no fault. The lab put back on 1.2.
- **Offline**: the kit's `views` (refusals, eight at most, taken back), `view.state` with no game, `view.show` of no
  such view, a mod's views gone with its reload. ALL PASSED.
- **Not seen**: a lens flare kept out of a view, the listener detached (Known gaps).

## 2026-10-15: a turning mirror in the world (the owner's test of 4a)

- **The owner**: a mirror turning in the world that draws what it reflects; the diamond (`world_marker`) and the
  falling boxes' overlay (`boxes`) out of the picture, the scanner's wave (`depth_pulse`) kept. `world_marker` and
  `boxes` now ship disabled (`"enabled": false`; the harness turns `boxes` on in its copy).
- **Made**: `rear_mirror` mode `spin`: a 2.4 x 1.8 m mirror with a wooden frame turning at 30 degrees a second; the
  game camera mirrored in its plane, the plane as the clip plane; the pass intersects each pixel's ray with the
  mirror, behind what the frame's depth shows in front of it, and samples the view through the view's own
  `view x proj`. Seen from behind: no view, a grey back.
- **The owner, watching**: no sky in the mirror. The cause: LS3D draws the backdrop with the camera's view without
  its position (`cam + 0x264`), so the sky's vertices lie about the origin and the world-space clip plane
  (`-n . at`, about 1984 sin a here) cut them away for half of each turn (the puddle's level plane kept them). Fixed
  in the bridge: `SetTransform` followed while a clipped view draws; the plane is off while the backdrop's view is
  set (matched bit for bit once a drawing, every drawing, in both labs: `view_sky_sets`).
- **The owner**: the scanner's wave is not in the reflection. The passes ran over the frame only; now a mod may ask
  (`gfx_pass_views`) for its pass over every view too: at the view's stage 1, the view's depth, colour, constants and
  size, its own effects target blended over the view's colour (`MmlGfxFrame::in_view`). `depth_pulse` asks; a pass
  that shows views does not. Its wave is seen on the reflected player and buildings.
- **The owner**: the reflection moves as if at 10 fps. My mod's fault: a mirror placed by `rear_mirror.spin {at}`
  kept a start time of 0, so the angle was the machine's uptime in seconds times 0.52, cast to a float: in steps of
  about 0.03 rad, some 15 a second. The start time is set at every placing, the angle wrapped in double. The owner:
  smooth now.
- **Both labs**: the mirror, the sky and the wave in it in 1.2 and 1.0 (the owner was playing in both); in 1.0 the
  frame-sized view with the wave in it costs 4.3 ms of CPU. Offline: `gfx_pass_views` on a pass and on no such pass,
  `in_views` in `gfx.state`. ALL PASSED.

## 2026-10-15: the game's own geometry hidden or replaced (item 4, block 4b)

- **Read**: mafia-ragdoll's `game_water.cpp` zeroes face groups' face counts around Render, keeping pointers to them
  from scans every 2 s (a freed mesh would be written to) and finding textures' names by scanning memory for
  strings. Read instead in the 1.2 decompile: every visual's `m_AddFGroups` (vtable +0x68) queues a 32-byte entry a
  face group (+0x00 the visual, +0x10 the group, +0x18 its texture), Render sorts them and calls `m_RenderFGroup`
  (vtable +0x6C), thiscall(visual, entry), ret 4; three functions draw every mesh (objects / morphs / single meshes,
  lit objects, billboards). A face group: +0x04 material, +0x08 faces, +0x0C the next (a list: mEdit's header has a
  faces pointer there, wrong), +0x14 index buffer, +0x18 first index; a material: +0x0C its texture (SetTexture);
  a texture: +0x1C its file name (mEdit's ITexture, seen live: `SKY 5 5.BMP` after `Maps\`). The same in 1.0.
- **Made**: `host/geom.cpp`: the three hooked; a group whose texture matches a rule is not drawn in the frame, the
  views or both; `geom.survey` (what a frame draws by texture); captures read the group's own DrawIndexedPrimitive
  (its buffers, base vertex, world transform), so any kind of visual comes out right. `mml.h` `geom_hide`,
  `geom_show`, `geom_capture`; `geom.*` on the bridge.
- **Found**: the city's river is one plane, 54 triangles at y -9.99 under the whole city (`00TEST01..20.BMP`),
  drawn every frame. Teleported onto it, Tommy drowned twice; the owner drove to the embankment by Little Italy's
  bridge (AGENTS.md has the place).
- **A bridge fault fixed**: the passes were drawn with no blending, so a later pass replaced an earlier one even
  where it returned 0 (`depth_pulse` would have erased any water drawn before it). The bridge binds a premultiplied
  "over" blend state now; the shipped passes' outputs were already premultiplied.
- **The example** `river_water`: the river hidden (frame and views), captured, a reflection view mirrored in its
  level, the water drawn on its triangles (waves, the reflection bent, the bed through it, glint, fog). The first
  waves aliased into stripes far off; each now fades where it is shorter than a few pixels. In the 1.2 lab: the
  bridge, its piers, a passing train and the far trees in the water; the reflection's view 7.7 ms of CPU (with
  `depth_pulse` in it). In the 1.0 lab the same (`00TEST16.BMP` read by the same offsets); five reloads with frames
  between: one of -17 MB, +3 handles, +1 thread (the city streaming), then 0.
- **Checks**: `check_builds.py` (the three at +0x6C of visuals' vtables, the entry's group and material read,
  SetIndices, DrawIndexedPrimitive, ret 4: both builds), offline (`kit.geom`: refused with no game; `geom.*` say
  why). ALL PASSED both.

## 2026-10-16: measuring the frame (item 5)

- **The owner left me working overnight**: the roadmap in order (with items 20 and 21, the agent's own tools, moved
  up), a local commit a block, no push; a check that needs something found in the vanilla game is left for the
  owner rather than searched for at length.
- **Read**: mafia-ragdoll's `prof.h` (scopes summed a frame) and `sampler.cpp` (a thread stopping the game thread
  every half millisecond, by phase).
- **Made** `host/prof.cpp`: every entry the host makes into a mod is timed where it makes it (Dispatch by event
  name, ModCall by its `where`, commands, posted calls, load and unload), and the host's heavy parts (the scene
  views, the graphics bridge); frames Present to Present in a ring of 600; hitches (2.5 times the median and 8 ms
  more) kept with what took them. The sampler adds to ragdoll's: the mod whose call the thread is in, the phase
  from the host (game, frame drawing, scene views, graphics bridge), and for a sample in Direct3D, the driver or the
  system the first return address into Game.exe or LS3DF.dll on the stack (a call checked before it). The report is
  made on the sampler's own thread; the thread is detached (a live std::thread at the process's end would abort it).
  `frame_stats` for mods; MCP `profile` waits for the run and names the functions (`kb::annotate`).
- **In the 1.2 lab**: the small freeride 5.1 ms a frame (194 fps), the bridge 0.87 ms, the mods thousandths. The
  city with `river_water` at the embankment: 7.8 ms (p95 10.3), the views 3.3 ms a frame, 41 % of the game thread
  in the views, 26 % in the frame's drawing, `I3D_frame::SetVisibleRecMainPass` the hottest in both; LS3D's render
  (`render+0x2BF6`) the main caller of the driver. Names added to `knowledge/ls3df.dll/names.csv`: the face
  groups' `m_RenderFGroup` and `m_AddFGroups` of five classes (read in 4b).
- **In the 1.0 lab**: the same (3768 samples, the views 35 %), Game.exe named through its 1.2 twins, LS3DF not.
- **Tests**: offline `prof.state` (an event and a command by site) and `prof.sample` (the harness's thread sleeps
  between its frames: all ntdll, so the check asks for samples, not modules); `mcp_smoke.py` calls `profile`.
  ALL PASSED both.

## 2026-10-16: waiting for what happens (item 20)

- Moved before item 6 (the owner's overnight order allows it): every test of a later item waits on something.
- **Made** `host/events.cpp`: a ring of 2000 events, written on the game thread from `Dispatch` (each frame's
  events left out) and by the host (MOD_FAULTED with where and the exception, MOD_UNLOADED with why, SCRIPT_ENDED
  with its state), read on the bridge's thread (`events.since`: answered while the game thread loads). MCP `wait`
  takes the latest sequence number at the call (or `since`) and polls every 100 ms. The roadmap's bridge
  `events.wait` was not made: the MCP server has one line to the bridge, and a call that blocked would hold it.
- **In the 1.2 lab**: a freeride's start kept as MISSION_CLOSE, MISSION_OPEN 00menu, MISSION_LOADED 00menu,
  MISSION_CLOSE twice (the menu's mission closed, then a close before freeitaly opens), MISSION_OPEN and
  MISSION_LOADED freeitaly; a spawned Paulie as HUMAN_CREATED; `human_death` gave no HUMAN_HIT (the game's death
  queue, not a hit: deaths are item 7's), the script's end SCRIPT_ENDED.
- **Tests**: offline (the fault, the unload after it, nothing after `latest`); `mcp_smoke.py` (`wait` from `since`,
  `wait` timing out with a plain error). ALL PASSED both.

## 2026-10-16: native.call (item 21)

- **Made** `host/native.cpp`: the arguments pushed from inline assembly (the last first), ECX and EDX set for
  thiscall and fastcall, EAX, EDX and ESP kept after the call, ESP put back from the one before (so a wrong
  convention cannot unbalance the game thread's stack), ST0 examined with `fxam` and popped when it holds a value
  (a float function's result must not stay on the x87 stack). The reply compares what the callee popped with what
  the convention says. Strings and buffers live in the host for the call; buffers come back as hex, words, floats.
- **Offline**: test functions in the kit (one a convention, a float, a double, an out-buffer, a string, a fault):
  11 checks, a cdecl function called as stdcall reported as such. **In the labs**: the text lookup (`textGet`,
  thiscall on `textDb`) gave "Game Over" in 1.2 and "Fin de partie" in the French 1.0.
- The session's usage limit ran out in the middle of the 1.0 check; resumed after it reset.

## 2026-10-16: hardware watchpoints (item 22) and people's skeletons (item 6, block 6a)

- **6a first**: the host keeps every `C_human` (its constructor and the actors' destructor, cleared at a mission's
  close), the skeleton by name (LS3D's FindChildFrame on the model: mafia-ragdoll's 18 joints, the pelvis, the
  root; a `head` asked for too was not there: the head moves with the neck), `bone_set` and the bridge's
  `human.pose` (held, written at RENDER_BEGIN).
- **A pose that did not show**: the bones held the pose (read back on a pause) but the arms hung down in the mirror.
  Rather than guess, item 22 was made first: `host/watch.cpp` after mafia-ragdoll's `hwwatch.cpp`, four debug
  registers, write or access, the sites recorded once with their stacks and values, nothing read in access mode.
  A write watch on the bone's quaternion: 2536 writes by LS3D (`+0x1DB4E`, `+0x301C7`) from `C_human::Update`
  (`C_game::Tick` > `C_actor::Tick` > `C_player::AI`): the animation is the tick's, before RENDER_BEGIN, so the
  write there wins. The pose itself was at fault: 90 degrees about the upper arm's z keeps it pointing down (its x
  axis runs along the arm; the elbow measured 27 cm under the shoulder); the identity (a T-pose) showed at once on
  the screen and in the mirror. Each posed model is updated once after RENDER_BEGIN's mods (as mafia-ragdoll updates
  its posed models; not measured to be needed).
- **The example** `puppet`: a wave (the arm out sideways and swung up: negative about z) and a T-pose, seen in the
  1.2 and 1.0 labs on the screen and in `rear_mirror`'s turning mirror placed before Tommy.
- `human_player` added to the C ABI (a first draft of the example took the list's first person as the player).
- **Found**: 36 pooled people (`road_bl_E`, at the origin) in a freeride's list in both builds (Known gaps).
- **Tests**: offline people with no game, a watch on a kit field written by a command (found twice, its before and
  after), access mode, clear, refusals; `mcp_smoke.py` has `watch`. ALL PASSED both. One `native.call` of an
  arbitrary address made by mistake in the 1.0 lab faulted inside its guard; the game went on.

## 2026-10-16: what happens, as events (item 7, block 7a)

- **Read**: the knowledge base had the functions with other projects' contracts: `C_game::NewShoot` (0x5A84A0;
  MafiaOnline: thiscall, the shooter, the position and direction by value, damage, particle, frame, shots, ret
  0x2C), `C_game::NewExplosion` (0x5AAE10; MafiaOnline's hook: the source, a position pointer, radius, damage,
  three flags, a sound, ret 0x20; Hex-Rays named registers it does not take), `C_car::CarExplosion` (0x421D60, ret
  4); 1.0's from st0rm94 (game_new_shoot, game_new_explosion, car_explosion). Each one's first instruction and first
  ret read in both builds; `check_builds.py` checks them.
- **Made** `host/happen.cpp`: the three hooked (SHOT, EXPLOSION after the game applied it, CAR_EXPLODED); deaths and
  vehicles read every frame from the people `humans.cpp` keeps (the alive flag +0x5D, the vehicle +0x98), the killer
  the last attacker of a HUMAN_HIT in 3 s. Reading, not hooking, so every way into a car or out of life is seen.
- **In the labs**: 1.2 and 1.0 alike: the player leaving the taxi (VEHICLE_LEFT), the Thompson's shots (50 damage,
  one every 0.1 s, a direction about 120 m long), a spawned person killed by a script (HUMAN_DIED, no killer), a car
  blown up by a script (EXPLOSION radius 10 damage 200, CAR_EXPLODED, and the HUMAN_HIT the blast gave the player
  before them). A killer was not seen: the player's shots missed the person (Known gaps).
- **The example** `tally`: a panel of the player's shots and kills, deaths, explosions, cars blown up, rides, a
  note for the last thing ("a car blew up"), counts kept across reloads.

## 2026-10-16: the camera (item 9, block 9a)

- **Made**: the scene view's camera change (`PutCamera`: the pose made orthonormal, LS3D's symmetric frustum and
  its update, the sides of the three projections) taken out of `view.cpp`'s `Put` and used for the frame's own
  camera too: `FrameCameraBegin` before the views and the frame's drawing (a mod's `camera_override` function, or
  the bridge's held `camera.set`), `FrameCameraEnd` after LS3D's Render puts the camera's bytes back. The scene
  views start from the overridden camera. `camera_project` / `camera_ray` use the camera taken at the world's stage
  1 (`render_camera`'s reading), so they agree with what was drawn.
- **In the labs**: `camera.set` from 10 m above the taxi in the 1.2 and 1.0 labs (the parking lot from above, the
  HUD as it was, `depth_pulse`'s wave on the ground); `orbit_cam` circling Tommy in both (F8 in 1.0, the command in
  1.2), its label over his head through `camera_project`; unloading it gave the camera back (`override: none`).
  The crosshair stays in the screen's middle: the aim is the game camera's (Known gaps).
- `camera.state` counted the frames the mod's function declined (returned 0) as skipped: now `declined`.
- **Tests**: offline the override taken and given back, project and ray with no frame, `camera.set` held, released,
  refused without an eye or with the point at the eye. ALL PASSED.

## 2026-10-16: questions to the world (item 8, block 8a)

- **Read**: the knowledge base had g_collision's tests by st0rm94's and MafiaOnline's names. `_TestLineH`
  (0x5C8770; 1.0 0x5838A0) takes two masks, the moving objects' (-1: none) and the static world's, and goes to
  `_TestLineV` for a ray with no horizontal length; `TestLineHStatic` (game.h's TestLine) is it with the moving
  part off, `TestLineHDynamic` the moving part alone. `TestSphereB` (through the stdcall thunk 0x5D5940 that sets
  ecx) takes the same two masks (-1 skips a part). The moving tests pass the objects in the exclusion list at
  g_collision +0x2B8 (count +0x2BC), which `DynSetNextCollOwnerMult` sets as the game sets its shooter's. A moving
  object passes when its class (+8) has a bit of the mask: 0x100 people, 0x200 cars. All read in both builds'
  disassembly (the same code), checked by `check_builds.py`.
- **Made** `host/collide.cpp`: `world_ray`, `world_sweep` (an actor ignored through the exclusion list, each step
  guarded, the list put back as it was: the game leaves its array pointing at its own single slot with the count
  0), `world_near` (the actors, by their model's world position), `world_shapes` (the grids read as
  mafia-ragdoll's game_collision.cpp reads them); five bridge methods.
- **In the labs** (1.2 and 1.0 alike, to the millimetre): the ground under Tommy at -5.025 (a triangle, surface 4);
  a ray across him hit his sphere (class 0x102) and, ignoring him, the taxi's box (0x202) 3.84 m on; a 0.4 m sphere
  swept from his chest stopped 0.46 m on, at the taxi; 11 shapes within 3 m (7 triangles, the taxi's two boxes,
  his two spheres); the crosshair's ray from the camera hit a building 78 m away. A ray starting inside him does
  not hit him. The game's exclusion list read after the tests: as before them.
- **The water**: rays down over the river by Little Italy's bridge found the embankment's triangles (-6.2..-5.5),
  not a water surface: the water's height is left for the owner (Known gaps).
- **The example** `probe`: what the crosshair looks at, the ground under the player, a sweep ahead, the people
  near, the static collision around him drawn as lines; seen in both labs.
- **Tests**: offline the four functions with no game, the bridge's refusals. ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: the game's state (item 10, block 10a)

- **Read**: the scripts' `FREERIDE_SCOREGET` reads `C_game +0x3640` (0x47B6B0), `FREERIDE_SCORESET` calls
  `C_game::SetScore` (1.2 0x5B9FE0, 1.0 0x5F5C30: the value written, the HUD told when +0x363E shows the money).
  The inventory at C_human +0x480 from MafiaOnline's `native_inventory.h` (1.2) and st0rm94's
  `structs/inventory.md` (1.0), confirmed by the scripts' `HUMAN_DELWEAPON` taking `[edi + 0x480]` in both builds.
  The HUD's health number is set by `G_IndicatorsClass::PlayerSetLives` (1.2 0x5F88E0, 1.0 0x54A9D0, st0rm94's
  `hud_set_player_health_value`; ecx the HUD, `Exe().hud`), which C_player::Hit calls with health / maximum * 100.
- **Made** `host/state.cpp` and seven functions in the C ABI; five bridge methods.
- **In the labs**: the freeride's Tommy holds a Thompson (50 + 100), a Colt (7 + 21), a Magnum (6 + 18) and the
  sawn-off in the coat (2 + 20) in both builds; money set to $1234 / $777 shown at once; rounds written (the HUD's
  3/7, 4/9); the Magnum and the shotgun taken; health 120 of 200: the HUD kept 100 until PlayerSetLives was added,
  then 60 (1.2) and 45 for 90 (1.0).
- **The killer seen**: Paulie spawned 6 m ahead, `world.ray {"from": "camera"}` hitting him, the Thompson fired:
  `HUMAN_DIED` with the player as the killer in both builds, and `bounty` paid $50 (the HUD's $50, 827$). The 7a
  gap is closed.
- **Tests**: offline the seven functions with no game, the bridge's refusals. ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: the scene's lights (item 11, block 11a)

- **Read**: mafia-ragdoll's scene_light.cpp had the light frames' fields (type +0x154, power +0x158, colour +0x15C,
  range +0x168, sectors +0x140/+0x148) and the way to the sun. The knowledge base named I3D_light's setters in 1.2
  (reM, Oakwood); they are not exported, so the vtable was found holding SetPower (LS3DF 0x9BC88, written by the
  constructor) and 1.0's as the table whose slots hold the same code (0x97C28). Slots +0x24 SetOn, +0x58 SetColor,
  +0x5C SetPower, +0x64 SetRange; the setters write the same offsets in both builds (`check_builds.py`).
- **Made** `host/lights.cpp` (`scene_lights`, `light_set`, `scene_sun`, `lights.list`, `light.set`); the graphics
  bridge takes the scene's sun and ambient light when the frame has no Direct3D light (`gfx.state` `sun_from`: in
  both labs' freeride there is none, so the passes had a default sun from above until now).
- **In the labs**: the freeride's city has 39..44 lights (19 point, 14..19 spot, 3 ambient, 2 fog, 1 directional:
  `light03`, power 2, a warm colour, the same in both builds); `dusk` tinted the sun and the ambient lights and the
  whole picture went to a dusk (the sky and the buildings too, not only what moves), and back at its unload.
- **A hitch found and fixed**: the first version scanned the scene's frames every two seconds; `prof.state` showed a
  13 ms "scene lights scan" in a hitch every two seconds. Now once a mission (and on `rescan`).
- **Tests**: offline the three functions and the bridge's refusals with no game. ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: the bridge's methods found by agents

- **The gap**: every block made bridge methods, but `commands.list` named the host's 80 methods without their
  params or results (and left out the bridge thread's and `world.*`), the MCP server's instructions told of none of
  the new blocks, and AGENTS.md of only some.
- **Made**: docs/PROTOCOL.md's tables are the one description; `tools/gen_methods.py` makes `host/methods.inc` from
  them, and `commands.list` gives each host method (97) with `thread`, `params` and `result`. MCP `commands_list`
  takes a `query` (the list is long); its instructions list the methods by topic; `wait`'s event types include
  7a's. AGENTS.md: "Finding a method", the state, people and lights, the map's new files and examples.
- **Tests**: `run_tests.py` checks that the file is current, that every documented method is listed with its help
  and answered (none an unknown method), and that every method the host's code compares a name with is documented;
  `mcp_smoke.py` checks `commands_list {query}`. ALL PASSED both; `commands.list` read in the 1.2 lab.

## 2026-10-16: the car exhaust and particles.def (item 11, block 11b)

- **Read**: mafia-ragdoll's smoke_game.cpp had 1.2's exhaust (0x4DEE90) and its fields. The knowledge base's twins
  had no 1.0 pair for it (C_Vehicle::Update's calls in 1.0 are in another order), so 1.0's was found by its code: a
  function reading `[reg + 0xCE4]` (1.2's 0xC68 + 0x7C), testing bit 0x200, dividing by 28 (the emitter table's
  entries): 0x52B8A0. Paired with 1.2's instruction by instruction (740 and 747 instructions, the same mnemonics):
  every C_Vehicle field 0x7C higher but the velocity and the turns' maximum (0x84 lower), the emitter table 0x6561FC,
  the death share's float 0x63B2D0. `ebx` is saved before it is used (Hex-Rays' `__userpurge` was wrong): thiscall,
  ret 4. particles.def: C_car::LoadParticles and its twin read 0x6D4C1C / 0x6EFB9C, the same record offsets.
- **Made** `host/effects.cpp`, four functions in the C ABI, three bridge methods.
- **In the labs**: the taxi's single pipe, type 12, jet 1.13..2.07 m/s, the emitter adding 0.2 m/s along z at birth,
  in both builds; 23 records (exhausts 12 and 15, dust, grass, stones, leaves). `exhaust_fx` replaced the taxi's puffs
  (the hook called for every car, about 5 a tick; the taxi's skipped). A record changed in the freeride did not
  change the running emitter; changed at the mission's opening (by `exhaust_fx`'s `game_tint`, the mod enabled for
  the launch), the taxi's own exhaust came out red.
- **Tests**: offline the four functions and the bridge with no game. ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: the game's particles (item 11, block 11c)

- **Read**: mafia-ragdoll moves the game's particles from hooks on LS3D's frame Update and SetParent, functions every
  frame of the scene goes through. Here the particle visuals' own vtables are patched instead: PRTB (1.2 LS3DF
  0x9C660, 1.0 0x98600) and PRTE (0x9C5B8, 0x98558), each found as the vtable a constructor writes before it stores
  its type ('PRTB', 'PRTE') at the visual's +0x1D4; their +0x18 and +0x2C are the frame's Update and SetParent. The
  emitters' puff tick calls Update through the vtable (`call [eax + 0x18]` after `lea eax, [esi + 0x80]`); LS3DF's
  one direct call of Update is in I3D_light's code. SetParent has five direct calls (not reached: the end of a life
  is then found by silence).
- **In the labs**: 1.0 with the vtables patched and no mod: 27 239 emitted and 9 409 pooled moves in a few seconds,
  337 lives begun or ended; `wind` pushed 54 020 moves, no fault. 1.2: the taxi's puffs born at x -1985.3 were at
  -1977.8 when they died (4 m/s for about 1.8 s), the pooled effects' too.

## 2026-10-16: the weather (item 11, block 11d)

- **Read**: the scripts' WEATHER_SETPARAM (1.2 0x474380, 1.0 0x5C2170) calls the mission scene's vtable +0xE8 with
  the parameter and a dword, and C_game::OsefujPocasi after On and MaxCount; WEATHER_RESET calls +0xF0. LS3D's
  setter and getter (+0xEC) switch on 16 parameters; each case of the setter reads its dword as a flag, an int or a
  float's bits (speed, length, width, distance, height, direction). MafiaOnline's native_environment.h named the
  first 15 by the scripts' keyword table. A literal in a script passes its float's bits; a register is truncated
  to an int first (a float parameter set from a register is wrong in the game itself: not the loader's).
- **In the labs**: the freeride's weather in both builds: off, 1000 drops, speed 25, length 1.5, width 0.06,
  25 m around and 12 m above, falling (0.199, -0.980, 0). `storm` rained over the city (the 1.0 lab's detail
  setting showed fewer drops) and put every value back at its unload; `weather.set` round-tripped 12.5, 0.2 and
  0xFFFF2020; `weather.reset` restored the preset but left the rain on.

## 2026-10-16: props (item 12, block 12a)

- **Made** `host/props.cpp`: a model made as world.cpp makes an actor's (the I3D driver's CreateFrame, the model
  cache's Open) with no actor; in the world linked into the sector at its place (the scene's `sceneMoved`, as a spawned
  actor's), or a child of any frame (LinkTo); its local place written as the game's setters write it (SetPos, the
  rotation's quaternion and flags, the scale at +0x90), then the frame's Update. Seven functions in the C ABI, six
  bridge methods; the host removes a mod's props at its unload, every prop at the mission's close (before the
  game's), and an actor's destructor's hook puts props on it into the world.
- **In the labs** (both builds alike): a bat made 4 m ahead of the taxi, scaled 3, turned, attached to the car with
  its world place kept: 13.2 m driven, the bat 13.2 m on with the car's turn; hidden and shown. `props_demo`: the bat
  on `back3` (its length is the model's y; the bone's y runs up the spine, z out of the chest) followed the walk, the
  arrest's fall and a game over's retry (1.0's retry keeps the mission and the player: no prop was lost); six stones
  around the neck (`2stone` is a boulder: scaled 0.25); `drop` left the bat in the air. A bat in Paulie's `r_hand`
  went into the world when `world.remove` destroyed him. The mod's unload, a hot reload and the mission's close left
  no prop and no fault.
- **A trap**: 1.0's freeride profile "FullSave" never opened the freeride's settings, so `"police": 0` was not used
  (the start's log says so) and a patrolman arrested Tommy for his Thompson.
- **Tests**: offline the seven functions and the bridge's refusals with no game. ALL PASSED; `mcp_smoke.py` ALL
  PASSED (the MCP instructions name the effects' and the props' methods).

## 2026-10-16: sound and music (item 13)

- **Read**: MafiaOnline's sound service (I3D_sound as a frame of type 4, the sound cache, C_game::Play3DSound) and
  mEdit's I3D_sound header gave the slots; each was read in both builds (the same code, the same arguments:
  `check_builds.py`). The menus play `sounds\menuopen.wav` as an ambient I3D_sound in no scene, opened by its own
  Open: that is a 2D sound. I3D_sound::Open turns its flag 8 into ISND_FREQUENCY (the cars' engine sounds pass it);
  without it SetFrequency does nothing. Music: C_game streams the city's music through LS3D's IShow (IGraph slot 0),
  times the options' music volume (1.2 0x6D4B10, 1.0 0x672498); IShow's position for an .ogg is the decoder's
  (ov_time_tell, ahead and in steps), its GetEvent tells the end (1). CityMusicPaused (1.0's found by 1.2's bytes).
- **Made** `host/sound.cpp`: nine functions in the C ABI, eight bridge methods.
- **In the labs** (both builds alike): sounds play (IsPlaying, the position moving), a one-shot is released at its
  end; the pitch reached the DirectSound buffer (22050 Hz: 11025 at 0.5, 33075 at 1.5), the volume too (0.5:
  -30.6 dB); `th_1.wav` (the Bolt-Thrower's engine, found by tapping I3D_sound::Open while a car was made) on the
  taxi followed it 45 m; in the pause menu LS3D held the world's sounds itself (their time stood) and went on with
  them after; "13 - Game Over" played to its end and was released, looped it started again; the city's music paused
  (C_game +0x2FC0 = 1). `engine_note`'s pitch followed the revolutions (0.89 idle, 1.08 after a pull); its unload
  stopped its sounds and music and gave the city's music back. IShow leaks 3 handles a track (the game's own way of
  stopping one: LS3D's).
- **Nobody listened**: the owner hears it once (Known gaps).
- **Tests**: offline the nine functions and the bridge's refusals with no game. ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: control (item 14, block 14a)

- **Made** `host/control.cpp` (the player locked, a car's physics replaced, a car's body) and the captures in
  `host/input.cpp`; five functions in the C ABI, four bridge methods.
- **The primary and the secondary binding** (the owner asked what really tells them apart; read in 1.2's
  decompile): on foot one action, either at 0.25; driving the primary wins while it is not 0 and is ramped by the
  car, the secondary is taken once it has moved 0.5 from where it stood and is applied as it is (a joystick).
  Captures take the whole action. Written in knowledge/notes/game.md.
- **A car's body**: skipping C_Vehicle::Engine left the model where it was (the body moved 63 m alone): Move places
  the model frame itself each substep, and the body's matrix (vehMatrix) is the frame's world matrix. The host now
  writes them; 1.0 keeps the step before's matrices at the C_Vehicle's start, not after vehMatrix (found when the
  check failed: `vehMatrixPrev`). Let go, the car stayed in the air until a pedal (the owner saw it fall at a key):
  Tick's rest counters had put it to sleep; the host starts them again at every step a mod makes, and it falls at
  once (both builds).
- **In the labs**: a captured Accelerate, W (1.0) or Up (1.2) kept the taxi still, 5 m without; the locked player
  stood (0.6..2.2 m unlocked); `hover` floated the taxi 1.5 m over the street, flew it 44..60 m, turned it left, and
  dropped it when off.
- **Tests**: offline the five functions and the bridge's refusals with no game. ALL PASSED; check_builds ALL PASSED
  (the rest counters, the matrices' copy in each build's Engine).

## 2026-10-16: interaction (item 15)

- **Read**: the game has no prompt of its own for using things; its help line (G_IndicatorsClass::MissionHelpAdd, the
  objectives' lines) takes a text id, and the loader already answers ids of its own in the game's text database
  (menus.cpp, for the selectors' options): a mod's prompt gets one. MissionHelpAdd, ConsoleAddText,
  CompassSetDestination, RadarAddObject / RemoveObject paired in both builds (`check_builds.py`).
- **Made** `host/interact.cpp`: zones, use-points (the help line; Action captured by the loader while the player
  stands in one), timers on the mission's clock; four functions in the C ABI, five bridge methods, four event types.
- **In the 1.0 lab**: "Fish here: press Action" at the top in the game's font; an Action beside the taxi: USED and no
  car entered; the point removed, the next Action entered the taxi; a zone entered at 2.46 m of a 2.5 m sphere; a
  2 s timer fired.
- **A trap in a test**: MSVC evaluates a call's arguments right to left: two `interact_remove` calls as snprintf's
  arguments ran in the other order (the test now calls them first).
- **Tests**: offline the four functions and the bridge with no game. ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: use-points in the game's own Action menu (item 15, reworked)

- **Why**: the owner pointed out that the game has its own choice when several things are near (Action beside a
  car and a door). The help line and a captured Action took the press from everything near; the game's own way is
  its using objects.
- **Read**: `C_player::Do_AB_OwnerNULL` asks `C_using_object::FindNearObjects` for the records near the player and
  always runs the action menu through `ExecuteMenu` (alone, a record is taken without the menu showing); the result
  is 0xA9 + the item's index; an action id the executor does not know is passed over (knowledge/notes/game.md, "The
  using objects"). AddObject, DelObject, FindNearObjects and ExecuteMenu paired in both builds (`check_builds.py`).
- **Made**: a use-point is a record of the loader's in the registry (its label a text id of the loader's); the pick
  is FindNearObjects' list (its hook) at ExecuteMenu's result (its hook). No Action capture, no help line. A point
  on an actor's frame goes with the actor (its destructor's hook: the record reads the frame's matrix).
- **In both labs** alike: "2. Fish here" under "1. Get in the front" ("Monte devant" in the 1.0 lab's French) beside
  the taxi; its pick a USED, no car entered; the car's pick entered it; a point alone taken by Action at once; one
  on Paulie's model taken, gone when `world.remove` destroyed him; leaving to the main menu (1.2) left none and no
  fault.
- **Tests**: offline ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: the HUD (item 16)

- **Read**: the HUD's pictures (G_IndicatorsClass::Create: IGraph's CreateITexture with the file, its alpha twin and
  flags), the console (ConsoleAddText), the compass (SETCOMPASS: CompassSetDestination and the HUD's flag 0x80) and
  the radar (RadarAddObject; DrawAll draws it under the flag 0x80000). The map's "target" (SetPlayerNaviPoint) is a
  global nothing reads. 1.0's AddFlag, RemoveFlag found by their bytes, paired by SETCOMPASS's calls in both builds;
  1.0's compass target is at +0x9270, not +0x9278 (`check_builds.py` reads it in CompassSetDestination).
- **Made** `host/hud.cpp` (pictures, the console and help lines, markers on the screen, the radar and the compass;
  a place given to the radar and the compass as an LS3D dummy frame) and pictures and the pass's items in
  `host/draw.cpp` (the markers projected at each 2D pass with that frame's camera); nine functions in the C ABI,
  `draw.picture` and five `hud.*` bridge methods.
- **A trap**: the radar's flag set in the city freeride made DrawAll read a null (the mission has no radar map): the
  game hung in the crash handler. The loader never sets that flag; the radar's marks show where a mission shows it.
- **In both labs** alike (details in ROADMAP "Done"): markers at the edge with their distance, the compass taken,
  handed between markers and given back with the HUD's flags as they were; pictures with alpha, tinted, turned;
  console lines bottom left, the help line above the crosshair; nothing left after the mission's close.
- **Tests**: offline the nine functions and the bridge's refusals with no game. ALL PASSED; `mcp_smoke.py` ALL
  PASSED; check_builds ALL PASSED.

## 2026-10-16: ready to be public (the owner's decision)

- **Audit**: no game files, dumps, decompiles or secrets in the history; the two pictures are ours (one drawn by
  `make_picture.py`, one a 7-byte stand-in). What could not go public: other projects' names whose terms forbid it or
  say nothing (MafiaHub's OSS license: use only to contribute to their work; Oakwood's: no derivative works; reM and
  mafia-re: no terms), and the game's own text (the strings each function uses).
- **Made**: `external.csv` keeps st0rm94's names (he shared them for everyone and agreed); `tools/kb_external.py`
  writes every other project's names to `external.local.csv` on the machine that has the clones. The functions'
  strings come from the user's own game: `tools/kb_strings.py` disassembles each function of `functions.csv` and
  keeps the NUL-ended printable runs its operands point at (1087 Game.exe functions in 15 s, against the 239 the
  decompile's hints had; LS3DF 350 against 112); the MCP server reads both local files. MIT license; every example
  off by default (only `core` loads); a disclaimer in the README; the commits' address to GitHub's noreply one.

## 2026-10-16: a mod's state in the game's saves (item 17)

- **Read**: G_LoadSaveClass (1.2 0x6D4628, 1.0 0x671FB0) formats `savegame\mafia%03d.%03d` from its +0x28 (the
  profile) and +0x7C (the slot) in SaveGameSave (writes; ret 4) and SaveGameLoad (reads; opens the mission and
  restores the actors inside); SaveGameNewGame clears a new game's or a freeride's state; ProfileDelete deletes a
  profile's .sav and its 600 possible saves. 1.0 has the same code at st0rm94's twins (`check_builds.py`).
- **Made** `host/saves.cpp`: the slot's state beside each save (`.mml`), the profile's in `mafia<p>.sav.mml`;
  `save_put`, `save_get`, MML_EVENT_GAME_SAVING, MML_EVENT_GAME_LOADED (MmlEvent's `slot`, `profile`); `save.state`,
  `save.get`, `save.put`.
- **A slip**: `ProfileNow()` read the build's table before asking whether a game is there: the offline test faulted
  (the harness has none); it asks first now.
- **In both labs**: details in ROADMAP "Done". The freeride saves slot 570 at its start (a GAME_SAVING right after
  MISSION_LOADED). A save made by calling SaveGameSave from the bridge at the start of 1.2's "Old Prison" loaded
  into a Game Over (Known gaps); both labs' saves were restored from copies and the test's `.mml` files deleted.
- **Tests**: offline ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: animations, getting up, the pedestrians' hits (item 6, block 6b)

- **Read**: the scripts' PERSON_PLAYANIM calls C_human::Do_PlayAnim(name, simple, load), which hands the file to the
  person's animations machine (C_human +0xBC: SetAnim with a cross-fade, GetActiveAnimTime for its time and length)
  and returns the length; PERSON_STOPANIM Do_StopAnim; HUMAN_CHANGEANIM ChangeAnim(own, with). The game's table of
  animations ({file, number}, 576 records, numbers above 511 a set's variants) is the same in both builds; the
  get-ups are 111 (from the back) and 338 (from the front). Do_ShockDown shows the action queue (+0x23C, 0x14 bytes
  an entry), its index (+0x2A0) and the state 6 (+0x40C), the same offsets in 1.0. The traffic walkers' Hit (pedHit)
  is byte for byte the same code in both builds.
- **Made** `host/anims.cpp` (play, stop, replace, state, get up, PEDESTRIAN_HIT) and the last drawn pose kept in
  `host/humans.cpp`; five functions in the C ABI, four bridge methods. 1.0's fields for bullets, getting up and
  trams found as twins of 1.2's functions (the same first instructions) and checked by `check_builds.py`.
- **Two wrong first guesses, measured**: (1) the chest's z axis is not "out of the chest" (a standing Paulie's
  `back3` z points down): lying on the back or the front is now the front from right x spine (the model's x x y is
  its z, the facing; checked on a standing body); (2) between frames the game's animation has the bones again, so a
  pose a mod drew in RENDER_BEGIN cannot be read later: humans.cpp keeps each posed person's last drawn bones.
- **A slip of the reference**: mafia-ragdoll masks +0x74 with 0x1FF; the table's numbers go above 511 (4097 is the
  player's standing), so the loader reports them whole and finds the file by the whole number, then by its base.
- **In both labs**: details in ROADMAP "Done". Hitting a walking pedestrian with the player's Thompson took too long
  to aim from here; a scripted explosion beside them gave the events.
- **Tests**: offline ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: what a bullet struck, cars' crashes, falls, items, money (item 7, block 7b)

- **Read**: NewShoot finds the static world's impact at once (testLine) and keeps it in the shot's record; the
  bullets' tick asks collRayTest only for the moving objects on each frame's stretch (it returned 0 for a wall) and
  makes the static impact when the bullet comes to it. C_car::Was_Collision is each contact of a car (body and
  wheels), every frame of one pressed on a wall; it divides |impulse| by the car's mass. The fall is the state 3 and
  the speed +0x418 of MovementIntern (a jump is state 4, an animation). The ground's pickup happens inside the
  "itempickup" menu, not through G_Inventory::Insert (tapped: not called). Both builds' code is the same at the same
  offsets (`check_builds.py`: Was_Collision paired, its mass and surface reads, the shot queue's +0x1D8, the record's
  impact flag +0x50, the landing's +0x418 in the state 3).
- **Made** in `host/happen.cpp`: BULLET_HIT (collRayTest hooked at the tick's call; the record found by its place),
  CAR_CRASH (Was_Collision hooked, 1 m/s and more), HUMAN_FELL, ITEM_GAINED, ITEM_LOST and MONEY_CHANGED (read every
  frame); the frame's events now sent after the people are looked at (a handler's call could add to the list being
  walked). Four structs and six event types in `mml.h`; `events.since` fields; a static_assert that every event has
  a name.
- **A wrong first guess, measured**: BULLET_HIT from collRayTest alone gave nothing for walls; the static impact is
  the record's.
- **In both labs** alike (ROADMAP "Done"): the taxi into the wall, bullets into the wall, the taxi and Paulie, a fall
  of 8.44 m, the Thompson dropped and picked up from the ground, a Colt given, the Magnum taken, the money set.
- **Tests**: offline ALL PASSED; `mcp_smoke.py` ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: a mod's own items (item 10, block 10b)

- **Read**: the items are `tables\predmety.def` read whole once (LoadMaterialsFromBin, at G_LoadSaveClass::Init)
  into operator new memory; everything reads a record by id, bounded by the count. The game's names for an item
  are its texts 3300, 3400 ("... collected") and 3500 + id, the same bases in 1.0; 3438 and 3439 are other lines.
- **Tried by hand first**: the table copied into game memory one record longer (a bucket's and a key's copies at 35,
  36): given, held, dropped, picked up from the ground, no fault.
- **Made** `host/items.cpp`: `item_define` (like a game item, its label, model, flags; ids kept in
  `mml\items.json`, an unloaded mod's id an inert placeholder), `item_find`; the game's text ids answered from the
  loader (menus.cpp GameTextSet, only where the game's own are blank); `items.define`, `items.state`; world.items'
  `mod`. The game's operator new was already in the table (the menus'); its use by the tables' loader is checked.
- **In both labs** alike (ROADMAP "Done"): rod and trout at 35 and 36, the rod dropped and picked up ("Fishing rod
  collected"), the ids kept after a restart.
- **Tests**: offline ALL PASSED (item_define refused with no game, items.define's error); check_builds ALL PASSED.

## 2026-10-16: statistics and the profile's progress (item 10, block 10c)

- **Read**: the game keeps no statistics (nothing in the knowledge base, the 1.0 reverse or the profile's file). The
  profile's progress is the manager's: ProfileSave streams its header and three blocks; the race, steal and freeride
  bonus-car masks (letters), the garage, the freeride's level word sit in the same layout in both builds; the saves
  and their files are obfuscated by G_Stream, their names are the texts 5000 + slot (the chapter their number).
- **Made** `host/stats.cpp`: the player's counts from the events (Dispatch) and his place (each frame), the game's
  in its saves and the profile's beside its .sav under the host's own owner "mml" (saves.cpp SavePutAs, SaveGetAs);
  `stats_get`, `profile_progress`, `profile_saves`; `stats.state`, `profile.progress`.
- **Two corrections, measured**: a teleport of 10 m was counted as walking (a 50 m limit): now a move faster than a
  runner (15 m/s) or a car (80 m/s); a fall was counted as walked (on foot only the ground's distance now). The
  profile's totals were written every minute and the last 16 s of a run were lost at the game's close: every 10 s.
- **Not found**: a car stolen (Known gaps).
- **In both labs** alike (ROADMAP "Done").
- **Tests**: offline ALL PASSED; check_builds ALL PASSED.

## 2026-10-16: the water, zones for people and cars (block 6a of the order)

- **Measured**: the city's river is in the static collision after all (a face of surface 31 at y -9.99), lying
  under the whole city; the gap written at 8a ("not in the collision") was wrong: the rays then were cast where the
  ground covers it. materials.def: 31 deadly water, 44 shallow, 40 and 41 death volumes.
- **Made**: `world_water` (host/collide.cpp: rays down going on below what covers the water, the bed under it),
  `zone_watch` (host/interact.cpp: people, cars, one actor; the crossings with the actor, an actor destroyed inside
  dropped); `world.water`, `zone.add {who}`.
- **In both labs** alike (ROADMAP "Done").
- **Tests**: offline ALL PASSED.

## 2026-10-17: a prop's animation and physics (block 6b of the order)

- **Read**: MODEL_PLAYANIM, MODEL_STOPANIM and CREATE_PHYSICALOBJECT in both builds (their calls, the model's
  vtable +0x60/+0x64/+0x68 and its channels at +0x150 in both LS3DFs); C_PhThing's Init and GameInit (a box of the
  frame's bounds in the dynamic collision); C_actor::GameDone.
- **Made** in `host/props.cpp`: `prop_anim_play`, `prop_anim_stop`, `prop_anim_state`, `prop_physics`, `prop_push`,
  `prop_body`; `prop.anim`, `prop.physics`, `prop.push`; game_build.h's fields checked by `check_builds.py`.
- **A wrong first try, measured**: a physical bat moved by `prop_place` came back to where its physics was made. The
  old object's GameDone (at its deletion, the next tick) relinks the frame to its parent of then and writes back its
  placement of then (C_actor +0x3C, +0x48, +0x58): those are now written before the object is taken out (where the
  prop is, or no parent for a prop removed, which would otherwise come back into the scene).
- **In both labs** alike (ROADMAP "Done").
- **Tests**: offline ALL PASSED; check_builds ALL PASSED.

## 2026-10-17: the game's files, a model's file, .4ds read (block 6c of the order)

- **Read and measured**: a model keeps its file's name at +0x154 (its Open's copy); every model opens through it
  (a tap on LS3D's model loader: 754 of 756 calls from the models' Open). The roadmap's "hooked from the process's
  start" was not needed. The archive keys of the game's table open both builds' archives.
- **Made** `host/models.cpp`: `game_files`, `model_file`, `model_open`, `model_nodes`, `model_mesh`,
  `model_materials`, `model_close`; `files.game`, `model.file`, `model.info`; files.cpp's `FilesNames`.
- **Found**: the 1.0 lab has another project's `scripts\MafiaRagdolls.asi`, loaded beside the loader (a tap on
  LS3D's model loader in 1.0 named it as every caller): left as it is, written into the Known gaps.
- **In both labs** alike (ROADMAP "Done"): every `.4ds` listed and parsed through the bridge (3220 of 3223).
- **Tests**: offline ALL PASSED.

## 2026-10-17: the meshes the game draws, an actor's posed (block 6d of the order)

- **Made** `host/meshes.cpp` after mafia-ragdoll's scene_walk, scenery and visual_meshes: the scene walked a
  little each frame, scanned every 0.25 s around the mods' boxes, meshes read from LS3D's buffers through their
  Direct3D 8 vtables; `scene_focus`, `scene_pieces`, `actor_meshes`; `scene.focus`, `scene.pieces`, `actor.meshes`.
- **A first result corrected**: `actor_meshes` of the taxi gave the player's Thompson and its clip (the player in a
  car is the car's child): the walk now stops at another model (type 9) and below a hidden frame.
- **In both labs** alike (ROADMAP "Done").
- **Tests**: offline ALL PASSED; check_builds ALL PASSED.

## 2026-10-17: shaders on the game's own drawing (blocks 7a and 7b of the new order)

- **The owner's decision**: the base for "real" shaders (per-object, on the game's device) before the camera and
  control; the roadmap's order changed (item 27, order 7).
- **Measured first** (`shader.survey`, both builds): the game's draws carry lit colours and no normals, Direct3D's
  lighting off, a handful of stage set-ups (knowledge/notes/game.md).
- **Made** `host/shaders.cpp` (rules, the constants, HLSL through d3dcompiler_47, the survey), geom.cpp's hooks
  calling in, `sdk/include/mml/shaders/game.hlsli` (made into the host by the build) and `reference.hlsl`.
- **A first version corrected, by the owner's eye**: the reference showed a shop's sign the game hides; the owner
  drove there and found it behind the wall. The position computed with World x View x Proj multiplied first lost
  the depth's last bits; the game's order (World, then View, then Proj) hides it as the game does.
- **In both labs** alike (ROADMAP "Done"): the reference on every draw, the frame the game's.
- **Tests**: offline ALL PASSED (a shader that does not compile refused with the compiler's message; the reference
  compiled with its header).

## 2026-10-17: the sun's shadow and the game's lights for shaders (block 7c of the order)

- **Made** `host/shadows.cpp`: a shadow map from the scene's sun (R32F, an orthographic box around the camera, its
  texels snapped) drawn on the game's device before the mission's world from block 6d's meshes (the scene around
  the camera, the cars near it), every state put back; the scene's point and spot lights reaching each draw a rule
  takes, nearest first; `shader_shadows`, `shader.shadows`; game.hlsli's registers c76..c116, `mml_Shadow`,
  `mml_GameLight`.
- **Seen**: by day, a debug shader showing `mml_Shadow` alone, then the reference darkened by it: the taxi's shadow
  on the parking lot, the building's on the pavement (the sun behind it), alike in both labs.
- **A wrong conclusion, caught by the owner**: the first night's picture was taken for the headlights lighting the
  street through `mml_GameLight`; the owner saw only the game's own cone there. Measured: the street came out black
  (no light at all), the cone was the game's, drawn by a path the rules do not take (the survey has every face
  group at pass 0). The cause: `mml_FaceNormal` (block 7b) pointed away from the eye (a shader drawing the normal:
  the street's y down), so every light's facing term was 0. Corrected (`cross(ddx, ddy)`); then the street lit by
  the car's own spot (a wide fan ahead of it) and the lamps, alike in both labs.
- **Not found**: what draws the headlights' cone on the street (Known gaps).
- **Tests**: offline ALL PASSED (the shadows asked for and let go by a mod and the bridge; a shader using
  `mml_Shadow` and `mml_GameLight` compiled).

## 2026-10-17: the sun's shadow map in the frame's passes, god rays, the tuning panel

- **The owner's demonstration**: god rays. A first try marched the haze inside the shaders on the game's draws; the
  owner asked for the proper way every time ("the correct, thought-out, optimised variant"), so it became a pass of
  the graphics bridge over the frame's depth: one march a pixel at half size, not one per drawn surface.
- **Made**: the map shared from Direct3D 9Ex and opened by the bridge (t2, its matrix in `cbuffer MmlFrame`,
  frame.hlsli `SunLit`, `SunShadow`); the map drawn with a slope-scaled bias, the bias in metres;
  `mods/examples/sun_shafts` (single scattering, Henyey-Greenstein normalised, Beer-Lambert, a soft curve over LDR,
  a depth-aware blur and upsample); host/panel.cpp, the tuning panel (item 18): Options > Mods, pages from the
  manifests' `settings`, live changes (config.cpp's in-memory set, the file written once the changes stop).
- **Measured, not guessed**: Direct3D 11 saw the shared map as only its clear value. A debug view of the map
  through Direct3D 11 (red where it held 1) showed it empty; the device interfaces were one object (their pointers
  printed); the cause was the draws' colour write mask (red only): with every channel written, both APIs see it.
  A first debug view was wrong itself (frac of depth x 4 near 0.5 is 0) and was replaced before drawing conclusions.
- **Found**: the casters of 7c are not what the game draws (leaves as whole quads, no billboards, no people): the
  proper way, LS3D drawing the map through an orthographic camera of its own, is in Known gaps with its leads.
- **Found**: doubles on the game thread come out float-precise where x87 computes them (the panel's rounded values
  as the nearest floats): the game's Direct3D leaves the FPU at 24 bits. Known gaps (the owner: later).
- **In both labs** alike: the shadows in the air, the rays, Options > Mods > God rays with the game's sliders, a
  change seen by the mod at once, Back to the list, the file written.
- **Tests**: offline ALL PASSED (the panel read from a test mod's manifest: a slider in %, one with a unit, a check
  box, a choice, a slider with no max left out; panel.open refused with no game).

## 2026-10-17: doubles in the mods' code on the game thread

- **Measured first** (`fpu.state`, both builds): the game enters the loader's frame with the x87 control word
  `0x007F`: 24 bits of precision. The settings panel's rounded values had come out as the nearest floats.
- **Made** `host/fpu.cpp`: the precision raised to 53 bits (only the precision bits) in mods.cpp's Safe* gates, the
  one way into a mod's code (events, commands, posted calls, passes, menu callbacks, load, unload), and around the
  loader's own pages; the game's word given back after, a fault too; `native.call` calls the game with the game's
  word. A mod's detour on a game function keeps the game's word (it is the game's call): MODDING says how a detour
  gets doubles.
- **Tests**: the stand-in game now keeps 24 bits as the game does; a test mod divides 1/3 on the x87 in a command
  (the double, not the float) and the stand-in checks after every frame that it got its 24 bits back.
- **In both labs** alike: the game `0x007F`, the mods `0x027F`, the frame given back at `0x007F`, the panel's values
  written clean, sun_shafts drawn.

## 2026-10-17 (night): the shadow map drawn by LS3D, an option for now

- **Made**: view.cpp `ViewDrawShadow` (LS3D's own orthographic camera: SetOrthogonal, SetOrthoScale, its update's box
  frustum), shaders.cpp's depth drawing in that view (the game's alpha test kept, the sky told apart by its view, what
  lets light through left out, a fence blended and tested drawn as a cut-out with its blending put back for LS3D's
  cache of states), shadows.cpp's two ways (`casters`: "meshes" the default, "game"), the device's Clear hooked so
  LS3D's clear is "nothing" in the map; the map read with the drawing's own matrices.
- **Found on the way**: no group reached the depth shader at first: `ShadersGroupBegin` leaves a group inactive when
  it is neither the frame's nor a view's; the drawing from the sun now counts.
- **Measured**: the ground in the map matches the ground (a debug view of the map's depth against each point's);
  leaves are cut out; but LS3D leaves out most visuals for such a camera (about 100 face groups against the meshes'
  200 and their 10 000 triangles; houses, cars, people missing), the same with a perspective camera at that pose, and
  fewer with its eye nearer. Left as an option (Known gaps: where LS3D drops a visual is the next thing to read); the
  meshes stay the default. The owner asked to drop such a hunt overnight and go on.
- **In both labs**: the default map as before (the trees' and the building's shadows), `casters: "game"` drawn without
  harm to the frame.
- **Tests**: offline ALL PASSED.

## 2026-10-17 (night): the game's own camera (block 8a of the order)

- **Read**: st0rm94's structs/camera.md (G_Camera inside C_game at +0x4C: the I3D_camera at +0x04, the car +0x0C, the
  mode +0x10; the 17-record mode table) against 1.2's code (G_Camera::Reset, SetMode, Begin: the same layout); both
  builds' tables dumped from their Game.exe (the records' floats are smoothing factors, not distances).
- **Measured, not used**: G_Camera +0x24 (5.0, "camera distance") is not what the camera on foot keeps behind the
  player: 12 written, the camera stayed (1.0); in a car it changes nothing either. Known gaps.
- **Made** `host/gamecam.cpp`: `game_camera`, `game_camera_mode` (the game's SetMode, only the modes the player's
  state allows), `game_camera_fov` (the camera's own setters, held each frame, the game's value back); `camera.game`;
  game_build.h `cameraSetMode`, `cameraModeTable`, `gameCamera` with check_builds.py's checks (ALL PASSED).
- **In both labs** alike: the bonnet's (14), the driver's head's (13) and the eyes' (3) cameras, 103 degrees held
  through the game's own changes and getting out of the car, a car's mode refused on foot with the modes it can take.
- **Tests**: offline ALL PASSED (no game: refused).

## 2026-10-17 (night): a mod's keys chosen in the game's menus (block 8b of the order)

- **Read**: the game's Controls screen (`menu.def optionsc1`): its rows are `vidc` controls, the binding slot in
  their flags' low bits (0..59), on the controls object's slot arrays (knowledge/notes/game.md) and the profile's
  fixed copy of them. A mod's action there would grow the game's arrays and its profiles: not done (Known gaps).
- **Made**: input.cpp's key taken for a setting (KeyGrab*: the game gets no key meanwhile; the key taken kept from it
  until it is let go; Esc cancels), panel.cpp's `"key"` rows (PanelFrame takes the key, the setting made live),
  sun_shafts' on-and-off key on its page, its hotkey bound anew when its settings change.
- **Found and fixed**: 1.2's pages lost texts set before the game made them (menus.cpp's OnCreate set a text only on
  a control with none in its description): seen on a page opened with no menu open (it waits for the pause menu).
- **In both labs**: F9 (1.0) and F10 (1.2) taken from the page and saved, the new key toggling the effect, Esc keeping
  the one there.
- **Tests**: offline ALL PASSED (a "key" row read from a manifest).

## 2026-10-17 (night): another kind of vehicle, the game's airplane (block 8c of the order)

- **Read**: st0rm94's structs/airplane.md (C_airplane, type 0x16: its flight state +0xB4, speed +0xB8, rotation
  +0xD0, the route's nodes), 1.2's AI (0x40BC10), AI_fly and Update (0x40BEC0): the flight moves the place along the
  facing at the speed and slerps the rotation between the nodes; Update gives the model frame the place and the
  rotation and, only while the plane flies, moves its six collision proxies. Both builds' vtables found from the
  constructors (1.2 0x623318, 1.0 0x63B7C0): slots 13 and 14, ret 4, the same fields touched (check_builds.py).
- **Made**: control.cpp's airplane steps (AI and Update hooked when first asked for), `vehicle_kinds` in mml.h (a
  mod's vehicle hook told of airplanes once it asks; the step's `kind`), `vehicle_body` on airplanes (the velocity
  and turn a mod wrote kept for its next step: the plane keeps only a speed), `vehicle.move` (the bridge's driver
  for a car or an airplane).
- **In both labs**, the freeride Extreme's plane (`Letadlo`, idle at 64 m): `vehicle.move` lifted and turned it, the
  frame at the same place to the millimetre in both builds; a ray found its collision box there; a 70-degree roll's
  axes the frame's; a throwaway probe mod (not committed) flew it in a climbing circle through `vehicle_hook`, cars no
  longer told to it once it asked for airplanes only. The plane given back to its route was not seen (it stands
  until a script starts it: Known gaps).
- **Tests**: offline ALL PASSED (`vehicle_kinds`, `vehicle.move` with no game).

## 2026-10-17: structures by name (block 9a of the order)

- **Read**: what holds layouts: game.h and game_build.h (the SDK's fields, per build), st0rm94's structs/*.md (1.0's
  fields in C blocks, with his confidence tags), the 1.2 decompile (fields named, but no type definitions: no offsets
  to take). 1.0 and 1.2 layouts move in steps (C_Vehicle: 1.2's + 0x7C), so st0rm94's offsets could not be taken as
  1.2's.
- **Made** `tools/kb_structs.py` -> `knowledge/structs/` (structs.csv, fields.csv): the SDK's fields named once and
  their offsets read from the headers; each class's vtable, actor kind, base and embedded members from its
  constructor in both builds (which registers hold `this` + n), its size from the `new` before it; st0rm94's fields
  carried to 1.2 by aligning each paired function's instructions with its twin's and voting on the displacements, a
  vote against the step of the sure fields around it refused (it had placed two of C_Vehicle's fields on 1.0's
  offsets: other objects' accesses). MCP `inspect` (mcp/src/structs.rs): an object's class by its vtable, its fields
  valued, pointers named (a class, a frame's name, a module's place) and followed, paths (`player.inventory`,
  `car.vehicle`, `game.camera`), `gaps`; a layout with no game; `kb_search` finds fields.
- **Checked against what was known**: the constructors' kinds (human 3, player 2, car 4, airplane 0x16, entity 0x1B)
  and the airplane's vtables (game_build.h) are what was known; C_car's sizes 0x221C and 0x21F4 are game_build.h's
  `carSize`, C_public_physics's `physSize`.
- **Found**: game_build.h's `vehInertiaA` 1.0 is `vehMatrix`'s offset: 1.0's GetRVelMassJ reads the body's matrix pair
  where 1.2's reads the next pair (knowledge/notes/vehicle_physics.md). Not a wrong offset (each is what its build
  reads), but not one field: left out of the tables. With `gaps`: G_Camera +0x08 holds the player (in a car, 1.2).
- **In both labs**: the player told by its vtable, its inventory and frames, its car and C_Vehicle; st0rm94's carried
  fields read in 1.2 what they read in 1.0.
- **Tests**: Rust unit tests (the types, the tables), mcp_smoke (a layout, both builds' offsets, kb_search's fields,
  an unknown build refused), offline ALL PASSED.

## 2026-10-17: crash triage in one call (block 9b of the order)

- **Made**: host/crash.cpp: the faulting thread's stack is the code addresses with a call right before them, a direct
  call kept only when its target (its jumps followed: a hooked function's, an import's thunk) holds one of the frames
  above (the first version kept every leftover: ntdll's four times in the stand-in, a loader address between
  C_game::Tick and the LS3D frame in 1.2); the report beside each dump, written on its own thread with a 4 s limit
  (the crashing thread may hold the log's, the events' or a tap's lock): the stack with the mods named, the log's
  last 60 lines, the taps' last calls, the last 40 events, the mods, the mission; the dump written again without the
  heaps when the full one fails. mods.cpp: a mod's fault gets the same stack (its log line, `MOD_FAULTED` `stack`,
  `touched`). MCP `crash_triage`: a verdict (what, where, the nearest code not the game's, and whether the game's own
  code ran from there), the frames named (1.0's through their twins), the taps' callers named, the running game's
  faults. The stand-in game crashes on a `crash` file (no Windows dialog).
- **Found**: MiniDumpWriteDump fails with 0x8007012B (a partial copy) in the stand-in and in 1.2 (in 1.0 it was
  written whole): the heaps are what it cannot copy. The 1.0 lab runs `scripts\MafiaRagdolls.asi` (mafia-ragdoll's
  mod) in the loader's mode: it is on the crash's stack (it reads the player's frame, and hooks the mission's tick);
  `lab.py status` does not tell of it.
- **In both labs**: a crash made on purpose (the player's frame pointer set to 0x10): `C_game::Tick+0x328` (1.2),
  its twin `+0x302` (1.0), a read of 0xBC, called from C_mission::Tick under the loader's mission hook; the taps'
  581 calls of C_human::Update from C_actor::Tick in the report.
- **Tests**: offline (a mod's fault's stack names the mod; a crash: the dump, the report, its stack, log, taps,
  events, mods), mcp_smoke (crash_triage with no crash, with a mod's fault, after a crash), ALL PASSED.

## 2026-10-17: restore points (block 9c of the order)

- **Read**: SaveGameSave's argument is which actors it writes: 1 every actor of the mission (g_vActors, their programs
  with them), 0 only C_game's temporary ones; the scripts' AUTOSAVEGAME passes 0, AUTOSAVEGAMEFULL 1. That is the
  1215-byte bridge save of 2026-10-16 that loaded into a Game Over. The game loop loads the save slot when C_game
  +0x48 is 3 (an arrest, the Game Over's retry set it), at its own place, in place when it is the same mission;
  SaveGameLoad sets the one-shot flag +0x1D4 that skips the next script save. Both builds the same
  (check_builds.py).
- **Tried by hand first** (1.0 freeride): slot 599 set, SaveGameSave(1) by native.call (112 KB), the car moved and
  the money changed, C_game +0x48 = 3: everything back in 0.3 s, the flag set and the slot left at 599 (both put back
  by hand, the file removed).
- **Made**: host/saves.cpp `restore.save`, `restore.load`, `restore.list`, `restore.delete` (slot 599 used only
  between a save and its move, or a load and its end; the game's slot and the flag put back; a save of the game's
  found there stops them); MCP `restore`; game.h kGameState, kSaveSuppress.
- **Found and fixed**: a load in the mission was told as changes (MONEY_CHANGED of the whole difference, items),
  and the statistics counted it as money spent: happen.cpp takes its baselines anew at every load and new game.
  The tests' money had gone into the labs' loader statistics: put back from the copies.
- **Found**: slot 599 is free by 1.2's texts (5591..5599 none); 1.0's texts do not name the freerides' saves, so its
  slot is free by its missions being 1.2's: the guard stays.
- **In both labs**: the freeride (car, money, a spawned car, the mods' state), a chapter (Salieri's bar in 1.0, Old
  Prison in 1.2: put back to the centimetre after a 40 m teleport, the mission going on with its objective), another
  mission's point opened and back (1.2..3.7 s); the saves of both labs byte for byte as before (copies compared).
- **Tests**: offline (restore.* with no game), mcp_smoke (`restore` list and refusal), check_builds ALL PASSED.
- **Later the same day**: at the owner's word mafia-ragdoll's files were taken out of the 1.0 lab
  (`scripts\MafiaRagdolls.asi`, `MafiaRagdolls.log`, `ragdoll_models.idx`, kept in `Mafia10LAB-backup\ragdoll-2026-10-17`;
  the ragdoll is to be made again on the loader): the game starts without it, only the loader's ASI loaded.

## 2026-10-17: a game folder named in Cyrillic

- **Reported** (a speedrunner trying the test archive): the game crashes at its start when its folder has Russian
  letters in its name, and starts without MafiaModLoader.asi.
- **Reproduced** in the stand-in game copied to `Игры\Мафия`: exit 0xC0000409, a C++ exception (E06D7363) on the
  bridge's thread, named from the PDB: json's dump of `mml\bridge.json` (bridge.cpp). The loader read its folder
  with GetModuleFileNameA (the system's code page, 1251 here), json takes only UTF-8: invalid UTF-8 threw, nothing
  caught it.
- **Made**: every path in the host is UTF-8 and Windows is asked through its wide functions (host/paths.cpp: Wide,
  FileOpen, ListDir, PathMove... about 100 calls in 20 files moved to them). The game's own names stay in the
  system's code page: a mod's file is given to rw_data's dtaOpen by AcpPath (the code page's spelling, else the
  short name), and the names the mods and the bridge give are turned into the game's (files.cpp GameNameOf). The SDK
  says paths are UTF-8 (mml.h) and gives `mml/path.h` (mml_fopen, mml_widen); telemetry, boxes and the test mods
  use it. bridge.json and the crash report are dumped with json's replace handler too.
- **Tests**: unicode_dir_tests (the stand-in game in a Cyrillic folder and in one no code page spells: bridge.json,
  ping, the mods, a mod's data folder, files.which, the log, a clean exit; a mod's file through the stand-in
  rw_data's CreateFileA where the folder can be spelled); the stand-in game loads its DLLs by wide paths, as Windows
  loads the game's. Offline and mcp_smoke ALL PASSED.
- **In both labs**, each started through a junction named `build\Игра 1.0` / `build\Игра 1.2` (game_dir pointed
  there, put back after): the freeride, core, trigger_view and telemetry loaded, freeride_picture's file read through
  the game's own rw_data (`files.read`: source mod), a setting written into the data folder, the log in UTF-8.
- **Later the same day, so that it does not come back** (the owner: "never again a problem with Cyrillic in the
  path"):
  - **Read**: rw_data's dtaOpen (0x100018A0, the same file in both labs) hands the name it is given to its
    CreateFileA import as it is, and does nothing else with it on that path: the loader's link on that import now
    opens a mod's file by its wide path (CreateFileW), so a folder no code page spells works too (it had been left
    out on W:, a volume with no 8.3 names).
  - **Found**: 38 json dumps in the host without the replace handler (events, saves, items, replies): a game name in
    its code page there would throw, uncaught on the game thread; all have it now. `draw_text` was documented as
    "the game's encoding" while mml.h said every string is UTF-8, and the bridge's `draw.text` handed JSON's UTF-8
    to the fonts as it was: it takes UTF-8 now (valid UTF-8 past ASCII turned into the fonts' code page, anything
    else drawn as before). `game::TextCodePage` / `game::TextUtf8` in game.h (the host's GameCodePage is it).
  - **Made**: `tools/check_paths.py`, run by the offline tests: no ANSI path function (fopen, CreateFileA,
    GetModuleFileNameA...), file stream by a narrow path, std::filesystem, or strict json dump in host, sdk, mods
    and tests (self-tested on a probe file; it found the heavy test mod's GetModuleFileNameA).
  - **In both labs**, through junctions `build\Игра 1.x` and `build\Mafia ☃ 試 1.x`: the freeride, a mod's file read
    by the game's rw_data (`source: mod`). In `Mafia ☃ 試` dxwrapper's stub cannot find its DLL (an ANSI path: its
    dialog, then plain Direct3D 8): Known gaps. `lab.py status` counts any Game.exe as the lab's (the owner's own game
    from another folder was running): conservative on purpose, left.

## 2026-10-17: trigger view

- **Checked** the example a session before had left unfinished (`mods/examples/trigger_view`, `script_commands.h`,
  game.h's detectors and areas): it loaded, but in the freeride its detectors were never drawn.
- **Found**: the host holds 4096 drawn items a frame for every mod together and dropped the rest without a word;
  the view cut every edge of the city's areas (districts hundreds of metres long) into 2 m pieces and took them all
  before its detectors. Now the host logs it once per mod and counts `dropped_last_frame` (draw.state); the view
  cuts a line at the camera's near plane and draws it as one, keeps to `range`, draws its detectors first and makes
  its circles of 8 to 32 pieces by their size on the screen (the freeride 40 items, Old Prison 1054).
- **Found**: a use point's +0x0C is its reach squared and horizontal, with 1.5 m up and down (FindNearObjects 1.2
  0x55E180, 1.0 0x61BF50: 2.25 the height squared in both), its place in the space of the matrix at +0x10, +0x2C the
  action's id whose text is the menu's line: the view drew a 1.5 m sphere; now a cylinder of 1.22 m and the line
  (game.h kUseReach2, kUseMatrix, kUseText, kUseHeight). C_human's type (+0x5FC, both builds) is game.h kHumanType.
- **In both labs**: 1.2's freeride (20 areas: the music zones and districts, the player inside two), Old Prison (46
  detectors: the commands they wait in, their radii, the crowbar's "used within 1.22 m: Pick up crowbar", a
  door's box to hit; 4 areas tested by the scripts), 1.0's freeride and Salieri's bar (1.0's command numbers named).
  Cost: 0.6 ms a frame, 3.6 ms the frame that walks the scene for areas (every 3 s; 12 000 frames in Old Prison).

## 2026-10-17: games side by side

- **Asked** (the owner): the agent must always be able to start its own game, whatever the owner runs (a game the
  owner had open from another folder at the start of the day had kept the agent's checks guessing), and test 1.2
  and 1.0 in parallel in one test; the labs' saves are the agent's (the owner keeps a backup; the builds' saves are
  the same format).
- **Read**: Game.exe's entry (MafiaEntryPoint, 1.2 0x5BEBE0, 1.0 0x5F99B0) makes the mutex "Mafia Launcher Super
  Mutex Shit" and quits with no word when it existed: one Mafia on the machine. The loader's bridge already took
  the next free port; the MCP server knew one game (game_dir); lab.py counted any Game.exe as the lab's.
- **Made**: host/main.cpp makes a mutex of the game's folder first thing (its volume serial and file index: a
  junction is the same folder) and links Game.exe's CreateMutexA import to it: other folders run together, the
  same folder twice is refused as the game refuses it, and the second one's loader leaves the running one's log,
  bridge.json and hooks alone. The MCP server: `game` on every tool that touches a game (call_tool takes it out and
  runs the call in that folder's task-local scope; list_tools adds it to the schemas), one bridge connection a game,
  `game_status` with every lab. lab.py: a lab's game by its process's path (EnumProcesses,
  QueryFullProcessImageNameW, realpath through junctions); `use` no longer refuses while a game runs.
- **Tests**: offline instances_tests (two folders together while another game holds the game's own mutex, each its
  bridge; the same folder again and through a junction quit; the running one's log and bridge.json untouched),
  mcp_smoke two_games (`game` on a second stand-in folder: ping, a mod command, its log; an unknown game refused).
- **In both labs at once**: launched together into their freerides (8.7 s, ports 47101 and 47100, each its folder's
  mutex), two 3 s `wait`s in 3.0 s, trigger_view loaded and listed in both, screenshots of both; closed by their
  pids.

## 2026-10-17: race view

- **Asked**: a debug view for the race of mission 06-03 "Before The Race" (the autodrom, `mise06-autodrom`): trigger_view
  draws the scripts' triggers, nothing of the race.
- **Read**: 06-03 is the lab profile's save 105 (439 bytes: the chapter's start, loads the mission anew). The race
  is started by `racing_mission6_init` (1.2 `RacingMisse06` 0x549D00, which writes `g_pRacing` at 0x646DBC; 1.0's
  `g_racing` at 0x6F958C, racing.md). The race's menu before the start: "Opponents:" (`damageanddifficulty`, OK is
  id 500) in 1.2; 1.0 has no difficulty choice and starts after its cutscene.
- **Found, live in both labs** (C_racing's circuit, its 27 gates, the 13 racers, the HUD's place and lap checked
  against the readings: 1.2 at the start 7 of 13 then 12 of 13; 1.0 12 of 13 and lap 1 of 5):
  - 1.2: the circuit at +0x2C, the player's entrant embedded at +0x3C ("Tommy Angelo" at its +0), the 20 live racer
    slots at +0x238, laps +0x28C; the gates vector at the circuit's +0x44 (begin, end at +0x48). An entrant: car +0x28,
    the player's C_player +0x2C, zone t +0x4C, its gate +0x54, place +0x5C, laps +0x64.
  - 1.0: the circuit +0x30, the player's entrant +0x40, the slots +0x228, laps +0x27C; the gates at +0x44; an entrant:
    zone +0x4C, gate +0x5C, place +0x64, laps +0x6C. Where the two builds' tables (racing.md's PC column is 1.0's)
    differ, the value is the one read here.
  - A gate: its track node at +0, its outer marker at +4, the midpoint at +8 (the average of the two nodes: the
    error is 0.0000 in both builds), the next gate at +0x14, its number at +0x1C. A web node starts with its position.
  - The racers' tick (1.2 0x555E40 calls 0x553310 = C_opponent::Tick for each slot): a tap on 0x553310 gave the
    entrants at once.
- **Mistake, found and fixed**: the 1.2 gate vector was read as +0x40 by hand from a hex dump; the word list (the
  dump with indices) says +0x44, the same as 1.0's. A second mistake: the gates were read once, when the circuit was
  chosen, before the race builds them (`C_racing::GameInit`): the cache is keyed on the circuit and its vector's
  begin and end now.
- **Made**: `sdk/include/mml/game/racing.h` (both builds' layouts, `game::Racing()` by `g_exe`); `mods/examples/race_view`
  (gates as lines, numbered; racers as poles, labels with place, name, gate, lap and zone, lines to their targets;
  F8; `race_view.state`: the race as data, the gates, the midpoint check, each racer). The player's name "Tommy
  Angelo" at the layout's place is the check that a build's layout is right: nothing is drawn otherwise.
- **In both labs**: 1.2 into the race (OK clicked in "Opponents:"), 1.0 into it after its cutscene; the state of both:
  27 gates, midpoint error 0, places 1..13 with no repeat, the player at 12 of 13 and lap 0; screenshots of both
  (gate labels, the player's yellow gate, a pole and a target line per racer).
- **Tests**: run_tests ALL PASSED; check_paths: nothing found.
- **Not done**: `check_builds.py` does not cover racing.h's addresses (the mod's name check guards them at run time);
  the cost of a frame with the race (13 racers, about 8 reads each) is not measured; nothing committed.

## 2026-10-17: the race's lap glitch, seen in 1.2 with the race trace

- **Asked** (the owner): how a race "skip" speedrun works; logs of a run in 1.2. The owner drove the race (mission
  06-03) and the trace below is that run.
- **Made**: race_view's `trace` setting (off by default; `config.set {"mod": "race_view", "key": "trace", "value": true}`):
  each racer's lap, gate and place when they change, a gate reached by more than one step (a "JUMP": lost, or moved),
  the player's car jumping more than 10 m at over 80 m/s, and the player's position every 5 s. Written to the loader's
  log (`log_tail`); the header gives the player's entrant and the addresses of its gate, lap and place.
- **Seen** (1.2, one run; the log's times in ms of the game's clock): the player drives gates 0..11 normally from
  8.19 s to 8.23 s (about 1 min of play). At 8.231 s the gate goes 11 -> 12 and the place 12 -> 4; at 8.233 s the gate
  goes 12 -> 0 (-12 steps, "lost": the place 4 -> 1). Then the car stays on the hill beside gates 10-11 (x 280..320,
  z 230..300) and each crossing of the line x = 317.3 forward gives a lap: laps 1..5 at 8.238, 8.245, 8.256, 8.264,
  8.270 s; between them each time the gate goes 2 -> 0 (lost again). 5 laps in about 37 s after the glitch, instead of
  about 6.7 min of driving. At 8.274 s the lap goes 5 -> 4 (the car crossed back); no heartbeat after 8.275 s.
- **Watches** (hardware write watches on the player's lap and gate fields, 1.2): the lap field is written only by
  `C_racing::DoGates` (+0x8F: 5 increments, +0x2EF: 1 decrement: the same six as the trace). The gate pointer is
  written by DoGates at +0x296 (21), +0x337 (5), +0x315 (1).
- **Matches** `re/ext/.../racing/lap-gates-and-lap-glitch.md` (its author's, from the PC build: its numbers are 1.0's;
  the same rules are seen here in 1.2): the finish zone is the strip between the infinite lines of gate 26 and gate 0;
  a lost racer gets the first gate whose zone holds the car (gate 0 for a car on the hill); a forward step from gate 0
  is a lap. The doc calls the hill "checkpoint 9": in race_view's numbering it is gates 10-11.
- **Not checked**: that the race ends after lap 5 in 1.2 (no screen of it); the lap glitch in 1.0's lab with this
  trace; the trace code is not committed.

## 2026-10-17: the gate test drawn (race_view, follow-up)

- **Read** (1.2 `C_racing::TestGate` 0x557DD0, `InternTestGate` 0x557F00, `TestAllGates` 0x557E80, `C_car::GetAIPos`
  0x429510): the racer's zone test uses the car's AI probe point A, not its centre: `GetAIPos(car, 1)` stores it at
  `car+0x2098` (1.2) and `car+0x1FD8` (1.0, the PC table's place). Checked by calling the game's own function in both
  running races: the probe is 2.38 m from the car's centre in both builds (ahead of it), so the probe offset is not
  the cause of the glitch. The zone: `InternTestGate` tests the probe between the INFINITE lines of the gate's
  predecessor and of the gate itself (t 0 on the first, 1 on the second); `TestGate` tries the current gate, then the
  next, then the previous; in none: `TestAllGates` takes the first gate from 0 whose zone holds the probe.
- **Made** (race_view, settings `lines`, `trail`, `probe`): every gate's infinite line within range (gate 0's yellow,
  the current green, its neighbours blue), the probe's path coloured by its answer (green current, blue next, orange
  previous, red lost), the probe now with its answer (ours and the game's `t` at +0x4C), and the laps counted where
  the probe was. `racing.h`: `carProbe` (1.2 0x2098, 1.0 0x1FD8). `race_view.state` has the probe.
- **Checked**: our `t` against the game's `t` in both builds: 0.685 vs 0.669 (1.2) and 0.691 vs 0.675 (1.0): the same
  0.016 gap, the probe read a second before the game's write (the probe changes about once a second). A zero probe
  (not yet computed before the first test of the race) is no probe.
- **The finish** (1.2 run, the results screen): the race ended on the 5th lap crossing (8.270 s of the log), the
  screen says "You finished 1st", total 1:38.614 (the real time from the start, about 99 s by the log), and 12 lap
  entries, the short ones (5.5 to 20 s) being the glitch laps and the counter's back-and-forth.
- **Not done**: the place field and the HUD disagreed at the grid in 1.0 (9 against 7, read a second apart: not
  checked further); nothing committed.

## 2026-10-17: the race's lines painted on the world (race_view, `gfx`)

- **Asked** (the owner): the gate lines, the zone and the probe drawn in the world like paint on the ground, not as
  overlay lines (they went through walls and under the ground, and their depth was not readable).
- **Principle**: `depth_pulse`'s (the scanner's wave): a graphics pass (Direct3D 11) reads the frame's linear depth
  and finds each pixel's world point (`ScenePoint`). A gate's line is infinite in the ground plane, so the set of
  world points at a distance s from it is a vertical plane: where a surface meets that plane, the surface gets the
  paint. Walls and the ground are painted where they cross it, what stands in front hides it, nothing shows under the
  ground. The zone (between the infinite lines of the previous and the current gate) is shaded the same way. The probe
  is a ring of device lines on the car's ground, the path and the racers' poles and targets are device lines drawn at
  `RENDER_WORLD` (depth-tested, as `world_marker`).
- **Made**: `mods/examples/race_view/race_view.hlsl` (`PsRace`: the lines one pixel wide, antialiased; the current gate's
  real segment brighter than its extension; the fog fades it), `race_view.cpp` (`PaintPass` through `gfx_pass`, the
  constants in `cbuffer RacePaint` b1, 1152 bytes; `OnRenderWorld`; the unload releases the pass's objects), the
  overlay lines and poles removed (the labels stay: text is the overlay's job), `"gfx": true` in `mod.json`.
- **Found**: a mod with `gfx` gets a device without multisampling only if it is ENABLED when the game starts
  (`host/gfx.cpp` `ModsWantingGfx`: `e->enabled && e->gfx`): a mod loaded at run time (`mod.load`) on the game's 4x
  device leaves the pass off (`gfx.state` says "multisampled ... the game is started again"). The labs' copies of
  `mod.json` are enabled for the test (the repository's stays `false` as every example's). A reload of a gfx mod is
  clean: `free_mb 0`, 2 handles, 0 threads since the unload's release of the pass's objects (before: 6 handles,
  -211 MB of address space, the objects were never released).
- **Checked**: `gfx.state` "on" in both labs (the pass 0.003 ms of CPU in 1.0, 0.02 ms in 1.2). A top-down view
  (`camera.set` over the finish gate): the real gate's segment is the bright bar across the track and the infinite
  line runs on the grass beyond the track edges, where there is ground. The race in both labs: the current gate's
  line on the ground and up the hill's slope, the probe ring under the car, the racers' poles and target lines.
- **Not checked**: the trail's path on the hill during the glitch in a live drive (the state shows the path's points
  and the lines drawn: 26 at the start); the zone's shading far from the car; nothing committed.

## 2026-10-17: invisible walls shown (invisible_walls, a graphics pass)

- **Asked** (the owner, standing at one in 1.0 on the hill beside gates 10-11): the walls nothing draws, shown as a
  shader in the world.
- **Found**: the static collision's triangles carry a material (`MmlShape::flags >> 24`, the game's surface number):
  material 33 is a wall with no drawing, 50 m wide and up to 90 m high beside the hill's road (1.0: triangles with a
  horizontal normal at x 248..351, z 268..316, y 29..91; `world.shapes` and `world.ray` both name them surface 33).
  1.2 has the same material along the start straight (z about -155, up to 55 m high). The walls are triangles, not
  boxes.
- **Made**: `mods/examples/invisible_walls`: every 250 ms the static world's triangles of the material within 60 m of
  the player are read (`world_shapes`), the nearest 128 kept; the pass (`invisible_walls.hlsl`, `PsWalls`) tests each
  pixel's ray (the frame's `ViewRay`, its parameter the linear depth) against them and paints the nearest hit that is
  in front of the scene (or over the sky): a translucent face, a glowing edge, a grid in the wall's plane, a fresnel
  rim and a band rising through the wall every 8 s. Settings: show (F9), material, range, grid, period, strength,
  colour. `invisible_walls.state` gives the walls near the player.
- **Found** (the first run faulted): `game::Detect` is needed before any game helper (here `game::Player`), and
  `game::ActorWorldPos` dereferences the frame directly: the player's frame is read through `H->mem_read` and
  `H->frame_world`, as `race_view` does.
- **In 1.0**: the wall beside the car: the grid's face on the hill's side, the edge on the grass, the top in the sky,
  the tree in front of it hides it by depth. Cost: 0.125 ms on the GPU, 0.004 ms on the CPU.
- **Not checked**: the start straight's walls in 1.2 in the picture (the state there finds them: not looked at), the
  walls of other missions, whether material 33 is always a wall nothing draws (the screenshot shows none where it
  stands); nothing committed.

## 2026-10-17: the settings page: long labels, and the material that could not be put back

- **Asked** (the owner, from the page in 1.0): a label that is too long runs over its slider (the material's, the
  race view's); the material, once changed, hides the walls and cannot be set back except by Defaults.
- **Found** (`host/panel.cpp`): a label is a 160 px box at x 30 and the slider starts at x 200, so about 20 characters
  of the game's font fit. A slider's value is shown as a percentage of its range unless the manifest says
  `"show": "value"`: the material (0..255) showed "13%" for 33, and 32 and 34 showed "13%" too, so the page could not
  say which value hides the walls. The apply path (0..1 -> value, snapped to the step) is correct: `config.get`
  shows the value the page sets.
- **Made**: `LabelLines` in `host/panel.cpp`: a label longer than 20 characters takes two lines, split at the last
  space that fits (the second line cut at 20 if still longer); a two-line row is 36 px, its control centred between
  the lines, a one-line row stays 25 px. The second line's control id is `kLabel2` (600). The manifests: the
  material shows its number (`"show": "value"`); the race view's labels shortened to two lines of at most 20 characters.
- **Checked** in 1.0 and 1.2 (screenshots of both pages): every label is on two lines or one, none runs over its
  control. Material: the page shows 33 and the walls are back at 33 (`config.set`: 8 walls at the start straight).
- **Not fixed** (a known gap): a mouse click on the material's bar moves it about 3 values a click at the 800x600
  coordinates, and the keyboard arrows about 10% a press: exact values need the real window's finer mouse, or a
  typed value (no such control yet). The page does not refresh its values when they change from elsewhere (the
  material read 57 on the page while the config said 33).
- **Logs**: no fault in 1.2 or 1.0 after the restart; the one warning in 1.0 is a second page opened over the first.
- **Not committed.**

## 2026-10-17: the material slider removed (invisible_walls)

- **Asked** (the owner): why a slider for the material when nothing but 33 shows a wall. The answer: the slider was
  there to find other no-draw walls. The state command now counts the triangles by material near the player
  (`materials`): at the start straight 1.0 shows 0: 11, 1: 12, 33: 8, 4: 40, so the other materials there are the road
  and the grass, not walls. The owner decided: the slider goes, 33 stays (`kWallMaterial`).
- **Made**: the control out of `mod.json`, the key out of `config.json`; `kWallMaterial = 33` in the code.
- **Checked** in 1.0: the state reports material 33 and 8 walls at the start; the page has no material row; a reload
  of the mod without a mission change: handles +4, threads +1, free -1.3 MB (before, a reload gave 0: not yet
  explained, not chased; the measure also counts whatever the game loads in the same window).
- **Not committed.**

## 2026-10-17: the scope's breath and the shot (aim_debug, measured in 1.0; not settled)

- **Asked** (the owner): the scope's crosshair breathes, the bullet seems to ignore it: where does the shot really go?
- **Made**: `mods/examples/aim_debug`: per frame the crosshair ray (`camera_ray` at the screen's centre) and its history;
  per player shot (`MML_EVENT_SHOT`, the game's own `dir`) the angles to the crosshair in the same frame, to the closest
  ray of the history (and its age), to the breath's centre (the average of the last 3 s) and to the weapon's frames
  (the player's offsets 0x564, 0x568, 0x56C, 0x570); the strike (`BULLET_HIT`); the picture as drawn (`render_camera`'s
  view and proj, p * view * proj). In the world (device lines at RENDER_WORLD): the crosshair ray (cyan), its path over
  the last seconds (green), the last shot (yellow) and the strike (red). `aim_debug.state` gives all of it as data.
- **Measured** (1.0, Mosin-Nagant in the scope, the player standing still):
  - the crosshair's point at 30 m is on the drawn picture's centre (`picture.cross` 399.7..400.5, 299.5..300.8 of
    400, 300): the picture is not shifted (`proj_offset` 0, 0);
  - breath: the crosshair moves up to 0.2..3 degrees in a few seconds (`sway_deg_max`);
  - shots: 0.0000 deg from the crosshair in one shot (its match 0.0 to a frame 47..93 ms earlier), 0.2094 deg in two
    others, with no frame of the last 4 s closer than 0.09..0.11 deg; the weapon's frames are 4..170 deg off (not the
    source); the breath's centre is 0.14..0.20 deg off the shot as well.
- **Open**: which vector the game uses for the shot (the logical aim, updated per tick, or the camera at another
  moment). The next step is C_player's aim fields (the mouse-driven angles) read at the shot, and the same test with
  the breath at its largest.
- **Note**: the lab's player moved between calls (not by the agent): the mission was restarted by the owner for the
  ammo. The mod survives a mission restart.
- **Removed** (the owner, the same day: the mod was rough, and what he wanted to know he had learnt from it). The
  measurements above stay as they were; aim_debug was never committed.


## 2026-10-17: an actor's route (actor_route; read in both builds' code, seen on Sara of mission 07-02 in both labs)

- **Found**: a person sent along a route walks in `ai_path::Tick` (1.2 0x5026C0, 1.0 0x40C970), thiscall
  `(ai_path*, uint, C_human*, bool, bool)`, ret 0x10. It gives `HumanMoveTo` the point at `+0x74` and counts the
  waypoint reached within `sqrt(+0x90)` of `+0x80`. The waypoints are 0x3C-byte records (gate A `+0x00`, gate B
  `+0x0C`, the waypoint `+0x24`, web node `+0x34`, kind `+0x38`) in a vector at `+0x1C` (1.2) / `+0x04` (1.0); the
  detour points at `+0x2C` / `+0x14`; the rest at the same offsets in both. All in knowledge/notes/game.md
  ("A person's route").
- **The first version was wrong** (an earlier session, not committed): it drew a record's first three floats (gate
  A, one side of the passage) as the waypoint, and `+0x80` as the point the person steers to. So it measured her
  walk 1.2 to 3.5 m off "the route" and looked for a cause: there was none, the field was the wrong one. Its hook
  declared the uint and the first bool as floats (passed on through x87 loads, a bool's garbage bits could change);
  it read the route object from the frame event, after the game had freed it (the game over menu was found at the
  address it held), and took a new route at the old address for the same route. st0rm94's `ai_path.md` (1.0)
  names `+0x80` the steering point; the code says otherwise (the note says where).
- **Mod, redone**: `mods/examples/actor_route`, both builds (a layout per build; the hook only on Tick's known
  first bytes). It copies the route inside its hook, while the game holds it, and reads no game memory later; a
  new route is told by its waypoints. Drawn: the waypoints and their gates, the walk, the point he walks to, the
  arrival circle, the detour points. Walkers go when the game destroys them or the mission closes.
- **Seen**: 1.0 (restore point `sara_walk`): her walk on a 9-waypoint route lies 0.06 m from it on average, 0.13 m
  at most; `+0x80` is the gates' midpoint, `+0x74` the record's `+0x24`, on the kind-0 waypoints she walked to (kind 1 and 2 not seen current). 1.2 (save
  130 "07-02", restore point `sara_start`): the same layout read at the 1.2 offsets, drawn on her walk.
- **The walk kept for `trail_s` seconds** (10 by default, on the settings page) of the world's time (the mission's
  ticks summed: a pause keeps it). Seen in both labs: 9.97 s kept at 10, 2.96 s at 3; the pause menu open, the
  trail and the ticks stood still. The walk is now measured from where he stood when the route began: after a
  route made anew mid-walk the way to its first waypoint was counted as an offset (1.2 m); now 0.11 m (1.0).
- **Not committed.**

## 2026-10-17: the cars' paths (actor_route; read in both builds' code, seen in both labs)

- **Asked** (the owner): the route a car the game drives follows, as for the people (15-07's chase of Sergio as the
  example).
- **Found**: every driven car has a `C_driver` (traffic, chases, police, racers), ticked by `C_driver::Tick` (1.2
  0x43C4F0, 1.0 0x483360: the same code and the same offsets in both). Its path is a vector of 0x70-byte nodes, four
  lane points each and the lane taken; `+0x6C` the point it steers to, on the segment before the cursor's node, a
  look-ahead (8.5 m in town, 41 m at the chase's speed). knowledge/notes/game.md, "A car's path".
- **Mod**: actor_route hooks `C_driver::Tick` too (only on its known first bytes) and copies the path at the end
  of the tick, the nodes again when the vector moves or every 0.25 s of the world's time. Drawn: the path through
  the taken lanes (blue, the node driven to in white, those behind dimmer), each node's lanes across the road
  (grey), the steering point (magenta) and the way gone (cyan). Settings `people` and `cars` choose whose; the
  labels made short (one line each).
- **Seen**: 1.2 city freeride, 13 traffic cars: the way 0.02 to 0.33 m from the drawn path, the target on it; the
  drawing on a car turning at a crossing. 1.0, 15-07 (restore point `sergio_chase`): the chase car's 137-node path,
  its way 0.51 m from it at speed (1.86 at most), the path drawn along the street.

## 2026-10-17: debug_views (four views in one mod), tabs on the settings pages, the traffic's cars left out

- **Asked** (the owner): the debug views made one mod, its settings in tabs that never run off the screen; the routes
  of the traffic's own cars left out (only the missions' actors).
- **The traffic's cars**: told by the call their `C_driver::Tick` comes from, `C_traffic_car::AI` + 0xD5B in both builds
  (1.2 0x4B099B, 1.0 0x4FC7DB; the E8 call checked at load). The detour is the hook chain's first link, entered by the
  thunk's jmp, so `_ReturnAddress()` is the game's call (behind another mod's detour it would be that detour's, and
  the car drawn). Setting `routes.traffic`, off: their cars are not even copied. Seen: 1.2 city, 0 drivers off, 14
  on (all traffic); 1.0 15-07, the chase car alone (137 nodes, not traffic).
- **debug_views** (`mods/examples/debug_views`): routes (F6, was actor_route), triggers (F7, trigger_view), race (F8,
  race_view), walls (F9, invisible_walls), a file each (`git mv`: their history kept), each with its own Load and
  Unload (views.h), hooks, events, command (`debug_views.routes.state`, `.triggers.list`, `.race.state`,
  `.walls.state`) and its settings under its section of the config (`"routes.show"`: a `SECTION` prefix glued to
  every key at compile time). Both shaders in its build. The four old mods are gone (their folders in the labs too;
  a lab's `mml\data\<old id>\config.json` stays, unread: 1.0's actor_route had range 500, trail 3 s, hidden).
  Triggers and race no longer draw their 2D lines and labels while a menu stops the world (they went over the
  settings' own page). aim_debug removed.
- **Settings sections and tabs** (host/panel.cpp): a manifest's `"sections"` (title, controls); a section whose rows
  do not fit the 600 units is cut into parts of about the same height. Tabs as the game's Controls has them
  (optionsc1: Player, Car, Other as buttons at the bottom, then Reset to Defaults and Back, the window titled with
  the tab): buttons in the bottom row before Defaults and Back (a row of their own past four), the window 580 wide
  and titled "Debug views: Routes". A tab closes the page and opens the other once the game has let the first go
  (`MenuPageIsOpen`, new, both builds). The first try, a selector at the page's top, was not clear to the owner.
  - The underline (the focus): the game gives it to the page's lowest id when it makes it (1.2; 1.0 the first
    record); a focus flag in the definition only added a second underline. The tab shown gets the first id and the
    first record: its underline says which tab is open (seen in both builds with the cursor elsewhere).
  - The race's labels shortened to one line: its 14 rows on one tab. `panel.state` gives `sections` and `parts`,
    `panel.open {mod, part}` a tab by number or name; tests: the kit's 20-row section cut 11 and 9.
- **Seen**: 1.2 and 1.0, the four views loaded, the page's tabs clicked as a player would (Routes, Race, back), Walls
  opened by `panel.open` with its underline. Offline: ALL PASSED (one run before had boxes' 20 reloads lose 2 handles
  a reload, not again in the next: not chased).

## 2026-10-17: the race's paint and the walls missing; deploy keeps a lab's `enabled`

- **Seen** (the owner, 1.0, the race of mission 6): no gates painted, no zone, no walls. The log: `gfx: off: the
  game's device is multisampled (... no mod with "gfx": true)`. debug_views was loaded with `mod.load` after the game
  had started; race_view had been switched on by hand in the lab's mod.json, debug_views (like every example) ships
  off. Switched on in both labs' copies and the game started again into the race (save 105): the bridge on, both
  passes running (the paint 0.15 ms, the walls 0.09 ms of GPU), the owner then set the zone's paint on its tab.
- **deploy** (mcp/src/game.rs): a lab's mod.json that says `enabled` keeps it: the build's manifest goes in with the
  lab's value (the key changed in its text, or put first; read back, comments allowed as the loader allows them; the
  whole object rewritten only if that fails), left alone when that is what is there; the reply's `kept` names them.
  mcp_smoke: a mod switched on in the game's folder stays on and its file is not touched; a changed manifest is put
  over it with the lab's `enabled` kept. The session that built it still runs the old server (its deploy would
  switch the labs' debug_views off again): the new one from the next session on.

## 2026-10-17: Lua dropped

- **The owner**: no script mods; Lua (LuaJIT) is out for good. Mods stay native, through the C ABI. LuaJIT is no
  longer pinned in `tools/deps.py` (its local download removed); README, ARCHITECTURE, MODDING, AGENTS and the
  roadmap say so ("Taken out or changed").

## 2026-10-17: deploy copies only the mods the build defines

- **The gap**: CMake never deletes a removed target's outputs, so the folders of the mods merged into debug_views
  (actor_route, trigger_view, race_view, invisible_walls) and of aim_debug stayed in `build/x86/out/mml/mods`, and
  `deploy` (and `tools/lab.py install`) copied every folder there: the removed mods came back into the labs (cleaned
  by hand once).
- **The build**: `mml_add_mod` and `mml_add_files_mod` add their id to a global property; the configure writes
  `build/x86/out/mml/build_mods.txt` (one id a line, only when it changes; not named `mods.list`, the bridge's
  method) and removes the folders in `out/mml/mods` that are not in it, saying so (taking a mod out of
  CMakeLists.txt causes that configure; boxes without `MML_JOLT` goes too).
- **deploy** (mcp/src/game.rs): copies the listed mods only; a leftover folder in the build's output goes to
  `skipped`, named; a named mod the build does not define, or one not built yet, likewise. Nothing in the game's
  folder is deleted: its mods the build does not define (a removed one's, a player's own) are named in `others`.
  The loader's `.asi` and `.pdb` go through `put_file` like the mods' files (unchanged: left alone, not in
  `copied`). No `build_mods.txt`: an error saying to configure and build. `lab.py install` reads the same list.
- **Tests**: mcp_smoke deploys the whole build into a game folder of its own (`game`: a folder with Game.exe) with a
  leftover `smoke_removed` in the build's output and a player's mod in the game's: every listed mod there, the
  leftover skipped and named, the player's mod in `others` and untouched; named mods the build does not define
  skipped, nothing copied. The running session keeps the old server until its next start.

## 2026-10-17: mods in Rust

- **The crate** `sdk/rust/mml`: `sys.rs` made from mml.h by bindgen 0.71.1 (`tools/rust_sys`, LLVM's libclang on
  this machine; `--check` in the offline tests). mml.h's comments follow their members, so clang would hand each to
  the next member: no comments in sys.rs (mml.h is the documentation). The named enums are newtypes (an unknown
  value from a newer loader is no undefined behaviour), their variants without the prefix; the anonymous ones plain
  `c_int` constants. The layout asserts are compile-time for i686 (MmlHost 684, MmlEvent 84): the crate refuses
  any other target with a message saying which.
- **Panics and faults**: the loader calls a mod behind `__try`/`__except`, which unwinds through the mod's frames.
  An `extern "C"` Rust function aborts when anything unwinds through it, so every trampoline (and the exports) is
  `extern "C-unwind"` (cast to the C types: the same cdecl). A panic is caught there (`catch_unwind`), its message
  and place taken from a panic hook of the mod's (each Rust DLL has its own std), logged, and raised as the
  exception 0xE0525553: the loader's guard takes it as any fault (MOD_FAULTED, unloaded at the next frame).
- **Closures**: boxed and handed to the host as its `user`; freed at the unload, a one-shot timer's and a script's
  after their call, a removed zone's, use-point's or timer's when no closure of the mod runs (a closure may remove
  itself). The host's registrations go after `mml_mod_unload`, so the boxes freed in it are never called again.
- **A wrong test of mine**: "a timer with no mission is refused" failed; the host keeps a timer with no mission (it
  counts the mission's time, `timer_add` refuses only ms <= 0), as interact.cpp says and the C kit's test checks.
  The test now adds one, removes it, and has 0 ms refused. And `command_json` first refused the bridge's `{}` for a
  command of no parameters (`()` takes only null): no parameters is now `null` or `{}`, whichever the type takes.
- **The build**: `mml_add_rust_mod` (cargo per mod, one target folder `build/x86/rust`; the DLL, its .pdb, the
  manifests copied); no cargo or no i686 target: the Rust mods left out at the configure, saying so. Cargo reads a
  `.cargo/config.toml` from the folder it runs in, not the manifest's: the build passes `--target` itself and runs
  cargo in the mod's folder; the generator's own build stays in `tools/rust_sys/target` (ignored).
- **Seen**: 1.2 and 1.0 freerides at once: hello_rust's line on the frame, `spawn` (a Bolt-Thrower 6 m ahead),
  `say` and Ctrl+F9 in the game's console, the hotkey moved to Ctrl+F11 by `config.set` (the old one silent, the new
  one counted), 10 reloads each: 0 MB, 0 handles, 0 threads a reload, the counts kept (load 11). Offline: ALL
  PASSED (the new `rust_tests`), mcp_smoke ALL PASSED.

## 2026-10-17: everything in Rust

- **The owner**: the gap closes at once, everything must be possible in Rust. The safe layer now covers every
  member of `MmlHost`, split by area into `gfx.rs`, `people.rs`, `world.rs`, `media.rs`, `control.rs`, `data.rs`
  (each an `impl Host`). What the host keeps for a while is borrowed for that while: a capture's and a mesh's arrays
  inside the closure that gets them, a `.4ds` model's while its `Model` lives (closed when dropped). The hooks the
  host keeps one of (exhaust, particles, vehicles, the camera override, the menu page) are slots: a new one frees the
  closure before it. Removable registrations (interaction ids, passes, views) are counted by kind, freed when
  removed. `game_files` calls back from inside the host's own loop, so a panic there is caught, stops the walk,
  and unwinds again after the host has returned. `menu_item` returns a `MenuItem` whose `set` is the host's "same
  function and user again" (each call in Rust is a new closure, so a second call would have been a second item).
- **mml::game**: the builds' tables, the layouts' constants, Detect's signatures and the scripts' commands are made
  from the C++ headers by `tools/gen_rust_game.py` (struct members and positional initializers parsed; every value
  a literal, the tool refuses anything else), so game_build.h stays the one source (mafia-ragdoll shares it).
  game.h's helpers ported with the host's never-faulting reads; what calls the game's code is unsafe.
- **Graphics**: the feature `windows` (the `windows` crate 0.62) gives the bridge's objects typed and borrowed
  (`from_raw_borrowed`: no reference taken); `sdk/rust/mml-build` runs fxc or dxc from a mod's build.rs (the CMake
  build passes its MML_FXC and MML_DXC; cargo alone finds the newest Windows SDK's). `include_shader!` lives in
  `mml`, not in mml-build: a build-dependency's macros are not the mod's.
- **My expectations corrected by the host** (each read in the code before changing the test): `gfx_pass` and
  `scene_view` are kept with the bridge off (mml.h: refused only for no function, a size over 8192, eight views);
  `frame_stats` counts Present to Present, none in the harness; `file_read` with flags 0 answers files_b's file the
  files tests left over the archive's (MML_FILE_GAME gives the archive's); a model's frames are named after it
  ("Tommy.back1"); particles.def's record 0 has no model in both builds; `scene_lights` gives every light while
  `lights.list` shows those on (its first is not the scene's first). `camera.set` for 6 s "did not show" from above:
  369 frames were overridden, the hold had ended before the screenshot (the calls' round trips took longer).
- **Seen**: both labs at once, `kit_rs.live` 47 of 47 twice (the second with the scans and hooks counted),
  depth_pulse_rs's ring from above, a scene view seen by a pass, a page of the game's controls from Rust closed by
  its Back; 10 reloads of depth_pulse_rs: 0 MB, 0 handles, 0 threads. kit_rs taken out of the labs again,
  depth_pulse_rs left off in their manifests. Offline: ALL PASSED, mcp_smoke ALL PASSED.

## 2026-10-17: what was left open in Rust, done

- **The owner** asked why the open items were left: there was no reason; "not seen in the game" is the next step,
  not an end. Each is done and seen in both labs:
  - **A member without a safe method fails the tests** (run_tests.py: every `pub` member of sys.rs's MmlHost named
    by an `f!` or `host_fn!` call in the crate; 169, none missing).
  - **Posted calls**: kept in the mod's own list by an id (the host's user pointer), taken by the trampoline, the
    rest dropped at the unload (the host drops a gone mod's posts unrun, so nothing calls them after). kit_rs posts
    one inside its own `on_unload`: "dropped unrun" logged at each of 10 reloads, never run.
  - **Direct3D 9 in RENDER_WORLD** (kit_rs `d3d9`): `host.d3d9_device()` typed, GetViewport 1280x960, a magenta
    square by Clear in the world's picture (under the pause menu, which the world goes on drawing behind).
  - **`MenuItem::set`** (kit_rs `item`): the item under Options, then moved under Load Game and renamed: one item at
    the menu's next opening, its click (menu.click by its id 104) counted by the closure.
- **The reload loop of kit_rs**: the first reload measures the game's whole start (the mod was loaded with it), the
  second and third +2 and +1 threads, -2.5 and -1.2 MB, then 0 for the rest; no thread was left in the mod's code
  (the loader names one): the process warming, as the C++ mods' loops show.
- The roadmap's last Rust gap was partly wrong: racing.h has only layouts in C++ too, and the scripts' table only
  its two lookups, both in mml::game.

## 2026-10-18: both builds in Ghidra

- **The owner**: the Ghidra item had sat in Later on the argument "until cross-references by hand become the slow
  part"; the journal shows they had been the slow part all along (the 1.0 port, every service's "who calls, who
  writes"). Made properly: mafia-ragdoll's `ghq.py` (its server, its PyGhidra calls that work on 12.1.4) was the
  reference, not a dependency: its paths, its locks and its projects stay that project's.
- **Install**: Ghidra 12.1.4 (the newest, 2026-09-21; the release's SHA-256) in `third_party/_deps/ghidra`, an
  optional pin of `tools/deps.py` (570 MB: not fetched by a plain `deps.py`); the venv from the archive's own
  PyGhidra 3.1.0 and JPype wheels (no index). The JDK: Corretto 21 is installed, but `java` on the PATH is 17:
  `java_home()` picks a 21+ (mml.config.json `java_home`, JAVA_HOME, Program Files).
- **The projects**: analysed headless (1.2 Game.exe 242 s, LS3DF 83 s; 1.0 250 s and 73 s). Ghidra left 51 557
  (1.2) and 53 885 (1.0) instructions outside functions: `orphans fix=1` made 1560, 752, 1382 and 901 functions,
  none left outside.
- **Found on the way**: the CLI first called `tools/ghidra.py` shadowed Ghidra's own Java package `ghidra` in the
  server's process (PyGhidra's `from ghidra import GhidraApplicationLayout` found the script): renamed
  `ghidra_cli.py`. `CParserUtils.parseSignature(None, ...)` is ambiguous to JPype; the ServiceProvider overload
  dereferences null; the DataTypeManagerService one takes it. Its parser takes no calling convention: taken out
  and set on the definition. Names with spaces (operators) are kept by Ghidra with `_`: compared so, a second sync
  changes nothing. An export's label of a name made the function's rename a duplicate: the label goes.
- **Ranks**: game_build.h's rows in names.csv (and mafia-ragdoll's names, which are the same vocabulary) had first
  outranked the decompile's class names (`humanHit` over C_human::Hit): they are aliases now.

## 2026-10-18: the knowledge base for 1.0

- **st0rm94's own 1.0 names** (`knowledge/game.exe/external_10.csv`, `tools/kb_external.py`): all 5028 named
  functions of his export at their 1.0 addresses; until now only the 2741 paired with a 1.2 twin reached the
  repository (carried to 1.2). His binary is not the lab's (another MD5, the same 2 486 272 bytes: two no-CD copies
  of the build; he says his PE timestamp was overwritten by the crack), so it was checked first in the 1.0 Ghidra
  project: 4911 of his 5098 addresses start a function, 112 lie inside one (joined by Ghidra), 75 in none. The
  regeneration left the other tables as they were (only their line endings).
- **Where they go**: the 1.0 Ghidra project (2801 names -> 5082; 71 functions made at his addresses), `where` and
  `crash_triage` in 1.0 (names_10.csv, then his, then the 1.2 twin's), `kb_search`'s `functions_10`.

## 2026-10-18: orders for people (item 26), tests in the real game (item 23 begun)

- **What**: `human.order {actor, code, act, flt, at}` and the named `human.go`, `follow`, `attack`, `flee`,
  `enter`, `stop`, `human.order_state`, the event `ORDER_ENDED`; mods' `human_order`, `human_order_state` (C, Rust).
  A person's own program (C_entity +0xA9C) is made anew with the order's code, compiled by its own vtable (the
  people's ENEMY_* commands), and handed to him by the game's `C_entity::ForceScript`: the game's AI runs it.
- **Three things kept a spawned person from doing it, each found by reading the code the game ran** (taps,
  watchpoints, Ghidra in both builds):
  - the start event: ForceScript raises 0xEA63, which is 60003, not the 59999 first written (the EVENT line never
    matched: 0x200 never set);
  - the hand-back: an action command gives him to the AI phase, and `ScriptSwitch` gives him back to the script
    only when `+0xF58` (put at 0 by `actionsTick` when the queue is empty) is below the program's `+0x60`, which
    `C_program::Init` makes 0 for a program with no instructions (spawn's): he walked, arrived and never took the
    command as done. Set to 0.5 as Init sets it for a program with some (one write 0.5 by hand: ORDER_ENDED at once);
  - END: `ClearAfterEnd` puts the owner's act state at 2, and `C_game::ChangeActState` then drops him from both
    lists of actors to tick: the second order of the first version (the first had ended on `end`) found Paulie never
    ticked again (no tap in C_entity::AI, no read of his fields; frozen). `C_actor::Rebirth` was tried: it makes the
    state 0 but adds to no list (the game's callers add with `InternTempActAdd`, which skips an actor already
    temporary, as spawn's are): the tests showed him still frozen, so it went. Now an order never reaches END: the
    loader's wait (`label 60003`, `wait 1000`, `goto 60003`) ends it and the code's `end` lines jump there.
- **Found on the way**: st0rm94's `C_entity::AI` is 5 bytes early (0x50814B, a stray call; the vtable's slot is
  0x508150: the Ghidra project corrected, names.csv). `ENEMY_MOVE_TO_CAR` only walks to the door: `ENEMY_USECAR`
  gets in. The 1.0 lab's profile FullSave never opened the freeride's settings, so police 0 did nothing: a patrol
  arrested the armed player (Game Over) during a follow; the profile Run works. The loader's own programs
  (`script`) were checked: they are freed standing on END before the game runs it, so their owner keeps state 0.
- **Tests in the real game** (`tests/game/`, item 23): `mml_game.py` (a Game a lab, over its own MCP server),
  `run.py` (the labs at once, a fresh launch, a restore point loaded before each case), `test_orders.py`: two goes,
  an order with its own `end` and one after it, follow while the player walks off, stop, attack, flee, enter.
  Its first runs found the frozen person above (9 checks failed in each build at the same step), then a test's own
  leftover (a person left seated from the run before took the seat).

## 2026-10-18: the measuring mods' cases in the real game (item 23)

- **The cases** (`tests/game/test_world.py`, `test_river.py`): a car driven into a person (HUMAN_HIT), a camera
  turned and given back (camera.state's eye and direction, the picture changed), a prop in the right hand (it stays
  on the bone when he turns and walks), a car made over the open river stays where it was asked, an exhaust under the elevated
  railway (the pipe behind the car, pointing back, emitting; a ray up meets nothing in the open street and the el's
  deck under it). The river and the el are in the whole city (`START = "city"`: the runner launches each freeride's
  kind once and keeps a restore point a kind).
- **A tool's bug found by them**: `spawn` of a car over the open river put it at y -315418. `CalmVehicle(100)` (as
  the cheat car) steps the car's physics in 83 ms steps until it is still or 100 ms of real time are gone: with no
  ground under it, it fell all that while in one call. Two wrong tries first: the ground asked after the car was
  made met the car's own collision; asked before, it met the river: the water's faces are static faces the game's
  TestLine returns (surface 31, flags 0x1F000005), and spawn's own Ground() had put the car on the water at -9.5.
  Now water is no ground for either, and a car is calmed only with ground within 5 m under it.
- **What the vanilla world does not do, left as it is** (no long hunt): a person put in the air over the river
  stands there (a standing NPC is not moved by the game: no fall without walking), and an order to walk over the
  water was not taken (no route there; the debug view shows collision walls over the river). "A body falls into
  the river" is the ragdoll mod's to do. A car made there does not fall either: drawn on the screen, it sleeps
  (act state 1: its frame's stamp +0x13C, what wakes a sleeping actor, stays -1); Known gaps. No road tunnel was
  looked for: the el's road (a deck 5.5 m up, found by rays up along the embankment) is the roof. In the small
  city every water point reported "covered" at -3.99: the embankment's ground over the river's plane, which lies
  under the whole city; the open river is east of the embankment.
- **What the game has not drawn lately sleeps** (C_game::Tick's code: `+0x5F` and the frame's stamp `+0x13C`
  against the scene's, looked at every 5 s; first misread here as the player's sector): the river's cases take the
  player there first. (The floating Paulie above read state 0: he did not fall because he stood.)

## 2026-10-18: driving orders, their outcome, the road network

- **What was missing** for scenes with driven cars: no named drive (only `human.order` with a command not yet
  read), no way to tell an order that reached its goal from one the game gave up (both end the command alike), no
  road point to send a car to (a destination was picked by hand), no retry.
- **Read in both builds** (Ghidra; the same offsets): ENEMY_CAR_MOVETO hands the car to the chase manager
  (`C_sledovacka` at C_game +0x2B18): a record a car, a new C_driver, a way planned by C_roads::FindPath between
  `GetNearNode` of the car and of the place; the command ends on status 1 (arrived) or 3 (given up) alike and the
  record is dropped then. C_roads is C_mission +0x30; its nodes (crossings and bends, tens of metres apart) are what
  `GetNearNode` searches.
- **Made**: `human.drive` (in as the driver unless seated, then the drive), the named orders' outcome in ORDER_ENDED
  and `human.order_state` (`arrived`/`distance`, `seated`), `retries`/`retry_ms` (a short order given again in the
  mission's time, a drive from where the car is), `world.road` and mods' `road_near` (C, Rust).
- **Found on the way** (the checks in both labs): iterating `Outcome(o).items()` of a temporary dangled (a json
  exception); a retry compiled its lines after the first's (ScriptCompileInto appends), so its end was looked for
  past the program and it never ended: cleared first.

## 2026-10-18: a calmer drive, the game's camera behind any car

- **What was wrong** (the owner watched Paulie drive a Bolt-Thrower in the 1.0 lab: "aggressive", into everything):
  measured in both labs on one street (the car facing east at z 126.5, its way west then north). Two things of the
  game's own: (1) its chase drive only goes forward, so a car facing away from its way's first leg turned on full
  lock across the pavement (4 to 7 knocks of 1 to 6.7 m/s); (2) a car that stood 2 s was "unstuck" by the chase
  manager: 15 s with the hunt's way round obstacles and a limit of 1000 (22 to 27 m/s in town, a car of the traffic
  struck). Not the road's speed (9 to 13 m/s).
- **Read in both builds** (Ghidra, the same code): C_driver::Tick's reverse (`+0x19C` bit 0: gear -1, 3 m/s, the
  wheels turned until the car faces within 0.34 rad of its target point), C_sledovacka::Tick's unsticking (the
  record's flag 0x100000, its ms at `+0x28`, the driver's `+0x298`), AddCarMoveTo's way (node 0 the car), and
  G_Camera::SetCar (1.2 0x5ED190, 1.0 0x5D4AA0; game_build.h `cameraSetCar`, check_builds.py checks it).
- **Made**: `human.drive` `start` ("turn", the default: the game's own unsticking with its reverse given at once,
  ended when the reverse ends; "place": put on its way; "as_is"), `calm` (the game's own unsticking ended once the
  car has got 6 m away or after 6 s), `speed` (the way's nodes' speeds capped); ORDER_ENDED `start`, `start_angle`,
  `unstuck`. `camera.game {follow}`: the game's own camera behind any car, or a person's car, as for the player's
  own (the owner's suggestion, over a camera put in the air with `camera.set`). Cases: `test_orders.py`
  drive_facing_away (no knock above 3 m/s in 15 s, arrives), `test_world.py` camera_follows_car.
- **Found on the way**: the first try set the unsticking on a fresh driver and the drive "arrived" at once in the
  same tick: the unsticking's Tick puts the cursor back to `+0x78`, which only Tick's own stuck branch fills (0 on a
  fresh driver), and a cursor of 0 is an arrival; the loader now fills it as that branch does. The first version of
  the case spawned the car 20 m north of the freeride's start, which is inside a house (the owner saw the
  Bolt-Thrower stuck in a wall): it was unstuck 9 times in 2 minutes and never arrived. The camera given back when
  the followed car was removed faulted inside the game's G_Camera::End (it shows the car's model again through its
  frame, already gone after world.remove); the car is now taken off the camera first. `attack: he hits the target`
  failed once in 1.2 (the game's aim, 45 s) and passed on the next run.
- **Seen**: both labs, `tests/game/run.py orders world`: 1.0 40 of 40, 1.2 39 of 40 then the failed case again
  19 of 19; offline tests and mcp_smoke ALL PASSED.

## 2026-10-18: one drive in both labs at once

- **Asked** (the owner): Paulie in a taxi in both builds at the same moment, along the same way to a far point of
  the city without traffic, the game's camera on his car, carefully, at most 60 km/h. Made with the tools as they
  are: `spawn` (Falconer Yellowcar), `camera.game {follow}` (both labs in mode 9), `human.drive {speed: 16.67}`
  to road node 1126 (17.8, 21.1, -658), the same in both builds; the way planned was the same 133 nodes in both.
- **First try**: each lab from its own freeride start. The owner saw them part: other phases of the traffic lights
  and trams (the freerides started at other moments), and in 1.2 the tunnel taken at 18 to 20 m/s (ROADMAP Known
  gaps: not seen again, its cause not recorded).
- **Second try**: one restore point of 1.2 copied into 1.0's `mml\restore` (the saves are one format: it loaded
  in 1.0 as it is) and loaded in both at once, the drives given together: both arrived (3218 and 3217 m in 348
  and 354 s), no knock, at most 13.6 and 13.9 m/s (49, 50 km/h), the game's unsticking at the same two stops in
  both (a crossing's wait), the two cars 0 to 34 m apart on the way (the driver's random speed factor).

## 2026-10-18: the 64-bit sidecar, and what its two halves share

- **Made** (the commit before this one): `mml-sidecar64.exe`, a 64-bit process the loader starts in a job object
  beside the game, loading the mods' 64-bit halves (`"entry64"`, `sdk/include/mml/side.h`) from fresh copies; two
  rings of messages in shared memory carry the bridge's calls, the events, the halves' commands, their shared
  textures' handles. Then the 32-bit side of what the halves share: `channel_open`, `channel_write`, `channel_read`
  and `gfx_shared_texture` in `MmlHost` (host/sidecar.cpp, host/gfx.cpp; Rust: `Host::channel_open`,
  `Channel::write_value`/`read_value`, `Host::gfx_shared_texture`, `d3d_shared_texture`). A shared texture is opened
  on the graphics bridge's device at a pass's first ask (`OpenSharedResource1` of a copy of its NT handle, the
  sidecar's kept by the loader), again when the half makes it anew (a stamp the loader gives each one) or the device
  is new; a failed open is kept, not tried every frame.
- **Measured**: `mods/examples/side_picture` (two halves): the 32-bit half writes the frame's number into a channel
  at `FRAME`, the 64-bit half's worker (woken by `FRAME`, which the sidecar is told of right after the mods) clears
  one of two Direct3D 12 textures to that frame's colour and bar, waits for its fence, and names the texture in
  another channel; the 32-bit half's pass draws it in the frame's corner. Both labs at once: 646 frames of 646 drew
  their own frame's picture; the picture was whole 0.4 ms after the frame's start (6 to 7 ms before the pass);
  a reload gave 7 frames with none, then the same. So the frame's state out at `FRAME` and a result back within the
  frame is not a frame late: what PhysX 5 needs.
- **Found on the way**: `side::ChannelOpen` (sidecar/channel.h, both processes) read the existing channel's size for
  its error after unmapping its view: an open with another size faulted (the offline tests' new refusal case found
  it). The session's MCP server was built before the sidecar's commit, so its `deploy` did not merge the 64-bit halves
  (copied by hand once; the server is built again for the next session).
- **Seen**: offline tests ALL PASSED (side_test now has both halves: both channels' ways, another size refused, a
  read while the other process writes as fast as it can: about 30 000 versions in a second, none torn or gone back);
  both labs (above).

## 2026-10-18: PhysX 5 in the 64-bit half, the sidecar's first heavy user

- **Asked** (item 19's rest): PhysX 5 pinned and built for the x64 preset, an example in two halves, the frame's
  round trip measured in both labs, reloads and a fault.
- **Made**: PhysX SDK 5.9.0 pinned in `tools/deps.py` (the archive is NVIDIA-Omniverse's whole repository: `deps.py`
  now keeps one folder of an archive, and its license files). PhysX's own CMake forces the whole build's
  configurations to its own (debug, checked, profile, release), so it is an external project in `build/x64/physx`
  (static, `/MT`, CPU only), its libraries an interface target `PhysX5`; its first build takes 100 s, a second one a
  few seconds. `mods/examples/physx_crates`: the 32-bit half drops a pile of the game's crates (`bedna00`: a cube of
  0.648 m about its origin, read from its `.4ds`), sends the static world around it (`world_shapes`) in a channel,
  and at each `FRAME` the moving objects' collision shapes; the 64-bit half's worker steps them (the statics' triangles
  cooked into a mesh at each new region; the movers kinematic over two substeps; CCD), the poses come back in a
  channel and the 32-bit half places the props at `RENDER_BEGIN`.
- **Found on the way**: the game's quaternions are PhysX's conjugate (a prop made with 45 degrees about y had its x axis
  at +z where PhysX's is at -z): `prop_place` gets (w, -x, -y, -z). A person has two collision spheres (both builds):
  0.3 m about the waist (classes 0x101) and 1.2 m (0x102); a car two boxes: its body over the wheels (0x201) and its
  whole hull to the ground (0x202). With the wide sphere the crates were pushed a metre away from Tommy; the narrow
  one alone passes over a crate on the ground, so a person is that sphere drawn out into a capsule from his feet to his
  head. The first region of statics (16 m around the pile) lost crates that a car or the owner pushed out of it; the
  region is now the crates' box with 8 m more, taken again when a crate is half a second from its side; a crate flung
  farther than the widest region (64 m) kept it being taken again every quarter second until such crates were made
  to leave the pile.
- **Seen** (both labs at once): poses of 2343 of 2344 frames (1.2) and 2345 of 2346 (1.0) were the frame's own step,
  whole 0.30 and 0.38 ms after the frame's start, placed 1.4 to 1.5 ms after it; the step 0.2 to 0.3 ms; 0.02 ms a
  frame on the game thread. A car driven through the pile in 1.2 scattered it, nothing lost. Ten reloads in a row in
  each build kept the pile, no handle, thread or memory left; 24 or 25 frames without new poses after a reload. A fault
  in the 64-bit half (`physx_crates.fault`): `MOD_FAULTED` with `"half": "x64"`, the crates stood, the game went on;
  `mod.reload` brought it back in 9 to 12 frames. The owner pushed the crates around in the 1.0 lab meanwhile.
- **Next** (the owner): what 32 bits cannot do, PhysX's GPU part (particles, cloth, deformable bodies: ROADMAP 19a),
  and collision from the visible models (19b).

## 2026-10-18: NVIDIA Flow 2's smoke and fire in the frame, through the 64-bit half

- **Asked** (the owner): smoke like NVIDIA's demos, through the 64-bit bridge and NVIDIA's modern libraries, as the
  proof that what was never made for 32 bits goes into Mafia; the newest libraries; anything may be downloaded and
  installed, the proper way always.
- **Chosen**: NVIDIA Flow 2.2.0 (the newest Flow, in the PhysX 5.9.0 repository; 64-bit only, Vulkan), not PhysX's
  PBD particles: a sparse voxel gas with vorticity, combustion and a self-shadowing ray march, NVIDIA's own renderer.
  mafia-ragdoll used Flow 1.0 (2017, Direct3D 11, a win32 DLL); Flow 2 has no 32-bit build at all.
- **Made**: `tools/deps.py flow` (Flow's folder of the pinned archive, Slang 2026.20, premake 5.0.0, DXC 1.9.2609:
  their releases instead of packman), `third_party/flow/premake5.lua` (Flow's premake script under premake 5.0),
  the x64 preset's external project `flow_build` and `mml_flow(<id>)`, `mods/examples/flow_smoke` (records
  `flow_smoke.h`; the 32-bit half's sources, movers and pass; the 64-bit half's Flow, CUDA bridge and `tune`).
- **Found on the way**: Flow's build under the newest tools: premake 5.0 took out `flags` and `{COPY}`, Slang 2026's
  `slang.dll` forwards to `slang-compiler.dll` and its header needs C++17, MSVC 14.44 warns C4756 on Flow's INFINITY
  (warnings are errors there). A long path under the scratchpad broke MSBuild's trackers (MAX_PATH). Flow's editor's
  `--headless` still opens a window (it hung on the owner's desktop once: never run it outside a lab). The 64-bit half
  first looked hung in `createGrid`: Debugging Tools for Windows were installed (SDK 10.0.28000's debuggers; the
  owner: install what the work needs) and cdb's stacks showed NVIDIA's compiler (`nvgpucomp64`) compiling Flow's
  shaders: minutes the first time, under a second from the driver's cache. Flow in metres: its defaults are
  Omniverse's centimetres (attenuations a hundred times larger), its buoyancy lives in combustion, its smoke colour
  map is by temperature and clear when cold, and scaled by 2.5. The owner saw the first smoke as "very transparent"
  (the centimetres' attenuation) and drove around the fire in the 1.2 lab while it burnt.
- **Seen**: both labs (1.2, then 1.0): smoke columns and a fire with black smoke, pushed by a passer-by and the
  owner's car; Flow 0.3 to 0.4 ms of GPU, the round trip about 3 ms; reload keeps the sources; a fault of the 64-bit
  half: no gas and no wait, `mod.reload` brings it back. Offline tests and both presets' builds.
- **Next** (the owner): only the 1.2 lab for now: a finer fire, and the gas meeting the visible models, not boxes.

## 2026-10-18: flow_smoke: a finer fire, the gas meeting the drawn meshes (1.2 only)

- **Asked** (the owner): only the 1.2 lab for now; a fire of a higher "resolution"; collision with the real visible
  models, not boxes. No long in-game test suites (the owner: they take too long for what they give).
- **Made**: the fire's layer on its own 4 cm grid (the smoke's stays 10 cm; `tune` `fire_cell`, `cell_size`); both
  layers collide; the scenery around the sources as one soup of the drawn meshes' triangles (`scene_pieces`, channel
  `scenery`, written when the scan changes), every car near them as its own posed meshes with each vertex's velocity
  (`actor_meshes`, `vehicle_body`: v + turn x r), every person as capsules along his bones (`human_skeleton`), all
  Flow mesh emitters in physics-collision mode, a shell of 0.15 m each side (`wall`, `wall_couple`). Flow's
  profiler's dearest passes in `flow_smoke.side` (`gpu_top`).
- **Seen** (1.2, the owner playing meanwhile: he parked the taxi in the fire): the flames in thin tongues that bend
  round the car's bonnet, the smoke kept off it; 1500 triangles of scenery, a car's 1300, a person's 350; Flow's GPU
  time 3.3 to 4.9 ms (the ray march 1.7, the frame's copies 0.6, the meshes' obstacles 0.09), the game thread's
  gathering 0.23 ms. Offline tests not run for this step (a mod's change only).
- **Next**: the ray march at half size (a Flow patch for its alpha, a depth-aware upsample): ROADMAP Known gaps.
- **Then** (the owner: "the collision by the visual models works rather strangely"; a picture: the fire under the
  taxi's back came out through its roof): the meshes were a shell of 0.15 m each side of their faces, the bodies'
  insides air, so gas born under or in a car rose through it. Now a body is solid behind its faces (Flow's signed
  distance to the nearest face: negative behind it), 0.6 m deep for cars and people, 0.5 m for the scenery (the ground
  and the walls solid, a fence's single face makes no block behind it), and 0.03 m outside a surface hold the gas at
  its velocity. The sign was measured, not assumed (`flow_smoke.winding`: of the ground's horizontal faces 883 point
  up and 7 down by Flow's normal (v2 - v1) x (v3 - v1), a car's 776 out and 537 in, its cabin's inner panels), and
  read in Flow's `EmitterMeshClosestCS` (Moller-Trumbore along the normal: behind it is negative); the people's
  capsules were wound inside out and are turned. A 3 m depth for cars first made Flow's nearest-face search 10 ms of
  GPU (181 frames late): 0.6 m costs 0.28 ms. The scenery was written again 57 times in a minute (its signature hung
  on the scan's order): once now. Seen: the fire at the taxi's nose rises round the bonnet and the wing.
- **Then** (the owner: "bring the collision by boxes and spheres back, something is still wrong"): the game's shapes
  are the default again (a person's sphere, a car's hull box, the ground's box); the meshes' collision stays, off,
  behind `flow_smoke.collision {"mode": "meshes"}` (with `winding` and `tune`'s `car_inside`, `scene_inside`,
  `wall_out`), for a later look at what is wrong with it. F5 (the fire where the crosshair looks) seen used by the
  owner. A reload that changed the frame record's size failed once: the new 32-bit half opened "frame" at the new size
  while the old 64-bit half still held it at the old one (refused); the next `mod.reload` loaded (ROADMAP Known gaps).
- **Then** (the owner, a picture of a car driving through the fire: "why does the fire lag behind the cars' fronts,
  is it late?"): measured, not guessed: the cars' boxes sent to Flow against the same boxes in the frame's pass were
  0.13 m apart on average and 0.91 m at most (835 frames of moving cars): the record was written at `FRAME`, before
  the mission's tick moved the world. Now at `MISSION_TICK`: 0.00 m (913). Two more causes in Flow's use: its fixed 60
  steps a second left the gas standing in some frames of a faster game (now a step of each frame's dt, two above
  25 ms), and a box's couple rate of 20 gave the gas 27 % of the car's velocity a step at 75 fps (Flow's rate is
  clamp(dt * rate, 0, 1)): 200 now (`tune` `shape_couple`).

## 2026-10-18: flow_smoke: the grids' cells and the fire's parameters as settings

- **Asked** (the owner): a setting for the fire's grid cell (he quoted 50%; the code had 4 cm for the fire and 10 cm for
  the smoke, 40% of it, and no 50% anywhere: the fire's ray march runs at the frame's full size, the half size is the
  next step). Then, for the grids of fire and smoke, and every fire parameter Flow gives.
- **Made**: `smoke_cell` and `fire_cell` (metres, the defaults unchanged) and 30 fire parameters, each named as Flow
  names it (`fsm::Fire` in the frame record; `combustion`, `ignition`, `burn_per_temp`... to `block_lifetime`), in the
  Options' Mods page in sections, and in `config.json`. The 32-bit half reads them at load and on
  `MML_EVENT_CONFIG_CHANGED`; the 64-bit half applies the fire layer's ones in `BuildParams`. The smoke keeps `tune`.
  The `tune` command lost `cell_size` and `fire_cell`: the settings are their one source.
- **Not exposed** (Flow's, left at their defaults): the second-order blend of each advection channel, the vorticity's
  masks, pressure, the colour map's points (the fire's colours), the shadow of the fire (shared with the smoke's
  `tune shadow_*`), and the smoke layer's settings beyond `tune`.
- **Checked**: both halves build; the offline suite passed before this step (not run again on request: the limits).
  Lab 1.2 started with the new build; the settings' effect in the game was not looked at.

## 2026-10-18: pet1kk's developer tools (PR 2) brought into the loader, offline

- **Asked** (the owner): a pull request came (pet1kk, "Add files via upload": `contrib/developer_tools/`, a standalone
  ASI for 1.2 with its sources and a prebuilt `developer_tools.asi`: script triggers, collision, check.bin and
  road.bin, names, god mode, endless ammunition, flying, wireframe, a free camera, the field of view, the HUD hidden,
  DebugText). Can it be taken, and made the loader's? Then: "do it all properly"; then: "do not start the game, all
  offline".
- **Not merged as it is**: a binary nobody can check against its sources, the game's own gamemenu.mnu and
  textdb_cz.def patched (the loader adds menu items and pages without that), hooks of places the loader keeps (the
  game menu's vtable slot 9, the HUD's call, C_program::Process, C_player's vtable), 1.2 only, Czech texts. Its files
  are kept on this machine in `re/ext/developer_tools/` (never committed) and the local branch `pr-2-review`; the PR
  stays open, the reply to its author is the owner's to send.
- **Read and checked** in Ghidra in both builds (knowledge/notes/game.md "pet1kk's developer tools"): C_player::Hit's
  start (MafiaReverse's 0x594209 was 7 bytes early: renamed at 0x594210 in the project and names.csv), the player's
  Hit and Collision gates, the distance tests' params (the handlers of GETACTORSDIST, IFFLTINRANGE, IF in both
  builds' switch tables), the pedestrians' web (C_web_path::LoadNodes, FindNodeID, FlipOneWay, GetNearNode), the
  surfaces' table (LoadMaterialsFromBin: the game's own names, not the PR's hand list), the actors' kinds
  (C_mission::CreateActor: 9 is C_villa, not a box), DrawAll and Do_Jump with their 1.0 twins, GM_Menu::SetFocus
  (named).
- **Made**: in the host and the SDK (C, Rust): `human_invulnerable` (the loader's hook of C_human::Hit returns before
  the game's for a person held), `hud_game` (DrawAll not run while held), `web_nodes`, `material_name`; the bridge's
  `human.invulnerable`, `hud.game`, `world.web`, `world.materials` (`world.shapes` names a face's surface);
  game_build.h `hudDrawAll`, `humanJump`, `materials`, `materialCount` for both builds; game.h's distance params, the
  web's and the items' layouts, `ActorTypeName`. `mods/examples/devtools` (off by default): kept from harm, endless
  ammunition, flying (the game's movement and jump hooked for the player, his place set as ACTOR_SETPOS sets it), a
  free camera, the field of view, the HUD; a page in Options > Mods. `debug_views`: triggers draws the distances the
  scripts test; three new views, `collision` (every static shape by surface, the names from the game's table, the
  surface under the crosshair), `web` (check.bin's nodes and ways) and `names` (actors' names and kinds), the lines
  drawn into the scene through the game's device at RENDER_WORLD (`lines.h`).
- **Left** (ROADMAP Known gaps): wireframe (LS3D's own driver state not found yet; a mod setting the fill mode would
  reach the HUD), DEBUG_TEXT's compile (three hooks in the script machine, the PR's claim not checked), the throwables'
  trajectory the PR's source holds but its README does not name.
- **Checked**: the loader and every mod build; `tools/check_builds.py` ALL PASSED; the offline suite ALL PASSED after
  the host's part. Not run in either lab (the owner's word): every new switch and view is untested in the game.

## 2026-10-18: the developer tools in the 1.2 lab: the owner's findings, the actions' bindings, a check of the pages

- **Found by the owner** (in the 1.2 lab): flying answered the primary bindings only (the arrows), not the secondary
  (W A S D). The tool's fault, not the mod's: `action_slot` names an action by its primary slot and `action_held`
  read that one slot, so every mod reading actions saw half of what the player gave (the game takes both rows as one
  action). Now `action_held` / `action_pressed` give the action's value from all its slots, whichever is asked
  (host/input.cpp ActionValue; mml.h says so). Seen: Forwards' secondary slot (W) flew the player 45 m in a second.
- **Asked** (the owner): the collision as solids, coloured and filled, not as frames. `debug_views` collision now draws
  every shape's faces translucent (`fill`), lit by their facing, with darker outlines (`edges`), lifted off the
  world's own surfaces along their normals (more far away); boxes as six sides, spheres, poles as closed cylinders.
  Seen in the lab: the street's walls and pavements as coloured solids, the surfaces' names over them.
- **Asked** (the owner): look through the rest for the same kind of thing. Found and fixed: flying would have pulled
  the player out of a tram (only a car was tested; now any vehicle, +0x98); the free camera's mouse was a constant of
  its own (now the options' sensitivity times 5, as the player turns); the hotkeys Ctrl+F1..F3 are F2..F4 (the
  profile's bindings checked: F1 is Objectives, F5 the speed limiter, F2..F4 free); the surfaces' names were read in
  the system's code page (1251 here: "Chodnнk"): the table is its makers' Czech, Windows-1250, in every build
  ("Chodník" now); the labels of `collision`, `web`, `names` neither avoided each other nor stayed on the screen, and
  `race` and `triggers` each kept their own list of places: one label function now (`lines.h`), one list for every
  view, cleared once a frame, the labels kept on the screen.
- **Asked** (the owner): an offline test that no interface lays elements or texts over each other. A page is now laid
  out apart from the game (panel.cpp LayoutPage, LayoutList) and made from that layout; `panel.layout` gives it for
  any manifest with no game; `tests/run_tests.py` ui_tests lays out every mod's `mod.json` in the repository and
  fails on controls or texts over each other, out of the window or the screen, a button's text wider than it, a
  label cut. The texts' widths are the loader's estimate of the game's menu fonts, measured on its pages in the lab
  (a little above each measure). It found: flow_smoke's tab "Heat and motion" (139 units) in a 90-wide tab, over its
  neighbours (the tabs are now as wide as their text and flow into rows; seen in the lab), and the key rows' prompt
  "Press a key (Esc: keep)" wider than its button (now "Press a key (Esc)").
- **Checked**: built; the offline suite ALL PASSED (the new page check among it); in the 1.2 lab: the materials'
  names, the web's nodes, the invulnerable switch, the HUD hidden and back (F4), flying on W, the free camera (F3),
  the endless ammunition, the filled collision, flow_smoke's tabs. Not in the 1.0 lab yet.

## 2026-10-18: the mods' drawing under the game's menus; the collision view in the scene, in order, with the cars

- **Asked** (the owner): why everything hides when a menu opens: the views hid themselves under the pause menu (so as
  not to be drawn over it), a crutch. The loader drew the mods' 2D queue at the end of the game's 2D pass, over the
  HUD, the open menu and the console. Now it draws it right after the HUD (its hook of G_IndicatorsClass::DrawAll,
  always installed, the HUD's hiding a flag of it), so the menus and the console are drawn over the mods' drawing as
  over the HUD; a pass with no HUD drawn falls back to the pass's end. The views (triggers, race, collision, web,
  names) and devtools' line are drawn under the pause menu now. `draw.state` `under_menus`. Seen in the 1.2 lab: the
  pause menu over the collision view, the view under it.
- **Asked** (the owner): the collision drawn in the world (what stands before it in front of it: Tommy could not be
  seen at full opacity), in order (far faces were laid over near ones), and with the cars (they are solid too).
  Tommy hidden: the owner's lab had `through` on (no depth test); with the depth test the scene's people and cars are
  in front of the fills (seen at a fill of 0.9). The order: the translucent faces are now drawn farthest first (the
  static faces sorted again when the eye moves 1 m or every 0.5 s, the moving ones every frame, the two merged). The
  cars: the view read the static grid only; it reads the moving objects too, every frame (`moving`: the cars' hulls in
  white, doors and the rest grey; people's spheres with `people`, off by default: a sphere round a person hides him),
  colours outside the surfaces' palette (a car in the concrete's orange looked like concrete). The line under the
  crosshair names a moving object too, and its place is kept free of every view's labels (lines.h Reserve).
- **Checked**: built; the offline suite ALL PASSED; the 1.2 lab as above. Not in 1.0 yet.
- **Then** (the owner: people are solid too, why no box or capsule): a person has none. The game keeps two spheres
  for him in its moving objects' grid, read in the lab again: 0.3 m about his waist (classes 0x101) and 1.2 m (0x102);
  his own moves sweep three spheres against the world, bullets test his bones. The view now draws both (`people`,
  on) as balls like every other shape (first as outlines, out of a fear that a filled ball hides him: the owner said
  balls, and the translucent fill is in the scene, so he shows through it), the wide one at half the fill; a car's
  body over its wheels (0x201) is outlined inside its filled hull (0x202) instead of a second fill. Seen in the 1.2
  lab: Tommy inside his two balls, seen through the wide one. Then, the wide one read in the code (the aim's broad
  phase: knowledge/notes/game.md "A person's two spheres"), the owner: not needed; it is not drawn now.

## 2026-10-19: the collision view as a graphics pass

- **The owner's findings** with the device-drawn fills: far parts of large triangles showed through small near ones
  (a painter's order by the triangles' centres fails for large ones), no sense of volume, the world's own surfaces
  fighting the fills, gaps between the ground's triangles (made by the view itself: each face was moved along its
  normal by an amount taken at its own centre, so neighbours moved apart). Asked: where they fight the collision wins,
  where the world really is nearer the world is drawn; something like ambient occlusion.
- **Made**: the view is a pass of the graphics bridge (collision.hlsl). Its triangles (each vertex its face's normal;
  no face moved) go into the mod's own colour and normal-and-depth targets with a depth buffer of its own: each pixel
  keeps the nearest collision surface alone, so nothing shows through. A full-screen step lays it over the frame:
  hidden only where the scene's linear depth is nearer by more than `tolerance` (0.15 m) plus 0.4 % of the depth, lit
  by its facing, darkened by an ambient occlusion read from its own depth and normals (`ao`), outlined (`edges`) where
  its normal turns, a neighbour's point leaves its plane (the distance to the plane: a ground at a grazing angle has
  no false lines) or its surface changes: the faces of one plane have no seams. A car's body inside its hull is no
  longer drawn (the hull covers it). Seen in the 1.2 lab (fill 0.75, edges on): solid walls and boxes, the concrete
  square one plane, Tommy in front.
- **Also**: devtools switched on in the 1.2 lab's manifest (the examples ship off; it had been loaded by hand).
- **Both labs** (2026-10-19, before the commit): in 1.0, the surfaces' names (the same table, Czech in 1250), the web,
  devtools' hooks, the 2D drawing under the menus (`draw.state` `under_menus`), the HUD hidden under the pause menu
  (French: "Menu du jeu" over the views), flying on the secondary W and up on Jump, the free camera, the collision
  pass with its defaults; invulnerability by a fall: 16.6 m took 163.8 health (a hit of type 6), the same fall held
  invulnerable none and no HUMAN_HIT. In 1.2: four frames in a row of the collision pass, no flicker. The offline
  suite ALL PASSED.

## 2026-10-19: a world grid on the collision

- **Asked** (the owner): the relief is hard to read on the flat colours; perhaps a light texture. Made: a world grid in
  the collision pass instead of a texture (the collision has no UVs; a grid in world space gives the scale, the slopes
  and the distance at once, as prototype materials do): triplanar (each face the grid of the plane it faces most: an
  upright one on walls, a level one on the ground, a blend on slopes), a line every `grid` metres (1 by default; 0
  none) and a bolder one every 5, about a pixel and a half wide, antialiased by the surface point's change to the next
  pixels (worked out on the surface's own plane, so also right at a shape's edge, and no derivatives in the shader's
  branches), faded where the cells shrink under a few pixels. Seen in both labs; the offline suite ALL PASSED.

## 2026-10-19: debug_views' walls are part of the race view

- **Asked** (the owner): with the collision view drawing every shape, the walls view (F9) is of no use on its own: the
  walls nothing draws matter in the race, outside it the collision view shows them. Made: walls.cpp is a part of the
  race view: its settings are the race's section (`race.walls`, `walls_range`, `walls_grid`, `walls_period`,
  `walls_strength`, `walls_color`), its own section, page and hotkey are gone, and it reads and paints the walls only
  while the race view shows a race (race.cpp `RaceShown`; `debug_views.walls.state` says `race_shown`). Seen in the
  1.2 lab: the Race page's second part holds the walls' controls with no overlap; outside the race the state reads no
  walls. The offline suite (the pages' layout check included) ALL PASSED.

## 2026-10-19: DLSS 5 Neural Rendering over the game's frame (dlss_nr)

- **Asked** (the owner, through another session): the newest "DLSS 5 NR" running in Mafia 1.2, for a guide for a
  friend's RTX 5060; then (the owner): the vanilla game, no ray tracing, with DLSS 5 NR, as a proper mod (settings, a
  hotkey, everything a game needs).
- **What it is** (NVIDIA's pages): "DLSS 5 with 3D-Guided Neural Rendering", announced March 2026, shipped 2026-09-03
  in NBA 2K27 (driver 616.64), a 2x faster model on 2026-10-13/16. GeForce RTX 50 only. Its runtime is
  `nvngx_dlssnr.dll`, newest 310.9.16.0 (signed by NVIDIA). The public DLSS SDK (NVIDIA/DLSS v310.9.16) has no DLSSNR;
  RTX Remix's runtime has it since dxvk-remix's commit of 2026-10-16 (CI builds; the release with it announced for
  2026-10-23), from NVIDIA's package `rtx-remix-ngx_sdk_dlnr` 2 (headers: a Vulkan API; the DLL).
- **Tried first**: RTX Remix's CI runtime with crosire's d3d8to9 in the 1.2 lab: it stopped at its driver check
  (616.56 < 616.92), and it path-traces the scene (not the vanilla look the owner wanted). Taken out again.
- **Measured**: a 64-bit NGX probe (Vulkan, NGX from the DLSS SDK, the DLL beside it) on the RTX 3070, driver 617.42:
  DLSSNR MinHWArchitecture 0x1B0 (Blackwell), FeatureSupported AdapterUnsupported, Available 0; NGX's log: "GPU
  architecture : 0x170, Snippet expects at least : 0x1B0". Bypassing it is against the RTX SDKs license (4.d). Also:
  before the owner's driver update the RTX 3070's adapter key had no VulkanDriverName (only Vulkan SC; a GTX 760 on
  475.14 beside it): the system's Vulkan loader found no driver for it; 617.42 fixed that.
- **Made**: `mods/examples/dlss_nr` (docs/MODDING.md "64-bit halves"): the 32-bit pass gives the frame (linear),
  its depth (0..1), camera motion vectors and a mask through four shared textures; the 64-bit half runs NGX on a Vulkan
  device of its own on the game's adapter (by LUID), imports them (D3D12 resources), copies them into its own images,
  evaluates DLSS-NR, copies the picture back; the pass shows it under the HUD. Settings page (Picture, Compare,
  Advanced), F12, console lines in the game's style, `dlss_nr.state`, `dlss_nr.side`, debug views (motion, depth,
  mask, the round trip, the motion vectors' check). `tools/deps.py ngx vulkan`: the DLSS SDK's headers and static
  library from its tag, NVIDIA's DLSSNR package (a 7z: 7-Zip's own `7zr.exe` 26.04 unpacks it), the Vulkan headers
  1.4.363; `deps.py` learned single files and 7z parts.
- **Checked** in both labs on the RTX 3070 (the mod on, the DLL beside it): NGX started (project id, custom engine),
  its verdict "unsupported" in the log, in `dlss_nr.state` and in the game's console ("DLSS 5 NR: needs a GeForce RTX
  50 GPU", at F12 too); the round trip of the frame through the half untouched: 1.2 200 frames of 219 (the first 19
  waited for the textures), 0.56 ms on the game thread, 1.0 168 of 187, 0.50 ms, none late, the picture right; the
  motion vectors by the check view while Tommy turned (a building's windows vanish with them, stand out without
  them); the Options > Mods page (its first tab and Advanced) in 1.2; a hot reload of both halves (NGX up again). The offline
  suite (the pages' layout check included) ALL PASSED. Not seen: DLSS-NR's feature made and evaluated (an RTX 50).
- **Lab**: both labs back as they were (dlss_nr off, the DLL taken out, debug_views' views in 1.2 back on).

## 2026-10-19: the roadmap: people's skinning, the renderer's bottleneck, the stages

- **Asked** (the owner, from the community's Discord): more than two bone weights a vertex, a mod's own shader
  constants and vertex data, tangents for normal maps. Read in 1.2's LS3DF how people are skinned: on the CPU
  (`I3D_singlemesh` slot 0x9C, `ls3d+54F60`), two influences at most in the data itself, then lit on the CPU
  (`ls3d+65A70`); the draw gets no normals or bones. Item 28 of the roadmap (28a N weights on the CPU in the same
  cache, 28b on the GPU with item 27, a mod's constants and a second vertex stream).
- **Asked**: why the whole city in view runs at about 30 fps on any machine. Not measured yet (the owner asked for a
  short answer first): item 29 holds the leads and the measurement to make first.
- **Decided** (the owner): the stages. The original game is reversed and made better with its style kept until
  nothing more can be done; NVIDIA RTX Remix through its own API is the next stage after that, not an item of the
  roadmap (ROADMAP "Decisions").
- Seen on the way: LS3D's skinned drawing has a second vertex layout (position, normal, UV, 0x20 bytes) chosen by a
  global flag (`ls3d+1C563C` bit 0): a hardware-lit path, perhaps; not tried.

## 2026-10-19: flow_smoke: its settings seen, a car's source behind its hull, jets, a pipeline cache of its own

- The settings of 2026-10-18 (the grids' cells, the fire's 30 parameters) seen in the 1.2 lab: a fire 6 m ahead,
  then `fire_cell` 0.15 and `fire_color_scale` 6 through `config.set`: the fire coarser and brighter at once (each
  frame's record carries them), then put back. The lab's own changes held `fire_block_lifetime` 1 from an earlier
  try; put back to the default, 8.
- `emit` with `"jet": true` (a car's exhaust leaving its back at `speed`; `local` its nozzle; made 2026-10-18 for a
  rocket car, not written down then) gave nothing at its first try: the default place, 2.4 m behind the car's
  centre, is inside the hull of a long car (the box of `spawn {"car": "Bolt-Thrower"}`'s thunderbird ends about
  3.0 m behind it), where the car's own box holds the gas out. A source on a car is now put just behind its hull,
  read from the game's box of it (`world_shapes`, class 0x200 and 0x2, its extent along the car's forward): the jet
  streams back at the camera behind the car. Seen in both labs (1.0: Tommy's car at the freeride's start).
- **A Vulkan pipeline cache for Flow** (ROADMAP Known gaps): Flow made its several hundred compute pipelines with no
  `VkPipelineCache`. `third_party/flow/patch.py` (run by CMake after `tools/deps.py flow`; it keeps Flow's own text
  beside each file it changes, `<file>.mml-orig`, and makes the file again from it, so a newer patch replaces an older
  one) gives Flow's Vulkan device a cache started from the file `NV_FLOW_VK_PIPELINE_CACHE` names, when its header is
  the same GPU's and driver's (vendor, device, pipelineCacheUUID), passes it to every pipeline, and writes it back
  (a .tmp renamed into place) 2 s after the last new pipeline, from the queue's flush, and when the device goes: the
  sidecar is killed with the game. `flow_smoke64` names `<data>\vk_pipelines.bin` and tells `pipeline_cache` (read,
  written) and `start_ms` in `flow_smoke.side`. Measured: 1.0 lab, its sidecar rebuilt (the driver's cache missed):
  110 s, 8.4 MB written; the sidecar rebuilt again and the game started anew: 0.49 s, the file read and nothing
  written. 1.2 lab, the driver's cache warm: 0.87 s without the file, 0.45 s with it. `__GL_SHADER_DISK_CACHE=0` set in
  the sidecar did not turn the driver's cache off (tried for a cold measurement in 1.2; taken out). The pristine
  copies of the four files came from the pinned commit's raw files on GitHub (the archive is not kept).

## 2026-10-19: a two-halves mod reloaded in one go when a channel changes size

- The Known gap of 2026-10-18: `mod.reload` loaded the new 32-bit half while the old 64-bit half still held its
  channel at the old size (the sidecar is told to unload by a message it reads when it can), so the new half's
  `channel_open` at the new size was refused and the load failed; a second reload worked.
- **Made**: the loader keeps the halves it asked the sidecar to unload (their generation) until the sidecar says
  "unloaded" (host/sidecar.cpp); a `channel_open` refused while the same mod's old 64-bit half is still going waits
  for that (at most 5 s, logged) and opens the channel anew. The sidecar now says "unloaded" for a faulted half too
  (its channels are closed then); the loader keeps such a half's state "faulted" until it is loaded again.
- **Tested** offline (tests/run_tests.py, `side_test`): a channel `grow` sized by a file in the mod's data folder, the
  64-bit half taking 300 ms to unload; `mod.reload` once at 4096 and once at 8192 bytes: both halves loaded, the new
  version whole. With the old host the 8192 reload fails (`mml_mod_load returned 1`), as it did in the game. The
  1.2 and 1.0 labs ran the new loader and sidecar (flow_smoke reloaded several times in each, the sidecar
  restarted); no reload of a changed channel size was made in a lab.

## 2026-10-19: flow_smoke's round trip on the GPUs alone (shared fences)

- The Known gap: the round trip was waited for on the CPU three times (the pass's event query for the frame's two
  textures; in the half `cuStreamSynchronize` after the copies in, Flow's `waitForFrame`, `cuStreamSynchronize` after
  the copy out), so the game thread waited for the whole of it each frame.
- **Made**: the 32-bit half makes two shared Direct3D 11.4 fences on the bridge's device, named
  `mml-flow_smoke-<pid>-<generation>-in/out` (a new generation each time: CUDA's import keeps an old name alive), and
  sends their generation and the frame's value in `View`; the half imports them into CUDA by name (D3D11_FENCE) and
  says so in `Done::fenceGen`; from then the pass signals "in" instead of waiting, the half's CUDA stream waits for
  it, copies in, signals Flow's exported Vulkan semaphore, Flow's flush waits for it and signals a second one, the
  stream waits for that, copies out and signals "out"; the half answers as soon as that is queued, and the pass has
  its device wait for "out" before the composite. Binary semaphores in pairs: a failure after Flow's signal makes them
  again. Without the fences (no Direct3D 11.4, an import that fails, `flow_smoke.wait {"fences": false}`) the old CPU
  waits stay. The View and Done records grew (176, 224 bytes): their reload in the 1.2 lab went in one go.
- **Measured** (1280x960, RTX 3070, the freeride's start, two fires): 1.2, the pass on the game thread 0.40 ms with
  the fences against 2.22 ms with the CPU waits on the same scene (from two snapshots of `flow_smoke.state` 736
  frames apart), the round trip 0.33 against 1.85 ms; 1.0, 0.39 ms and 0.32 ms. Every frame drawn was fenced (824
  of 825 in 1.2, 629 of 630 in 1.0: the first one confirms them); none late. Screenshots in a row: the fire whole,
  the traffic moving.

## 2026-10-19: flow_smoke's gas alone and at half size

- The Known gaps: Flow composited the gas over the colour it was handed and gave no alpha, so the half had to get the
  whole frame's colour and send the whole frame back (0.22 + 0.35 ms of copies), the ray march ran at the frame's size,
  and the passes ordered before flow_smoke were under the gas.
- **Made**: `third_party/flow/patch.py` change 2: `RayMarchBlockCS` writes the gas alone (its light premultiplied, its
  transmittance in alpha). The half gets the frame's depth only (`depth_in`), Flow marches at the march size the
  view names (`View::marchWidth/Height`: half the frame each way with the new `march_half` setting, on by default),
  `color_out` is at that size. The pass's PsComposite brings it up by the depth (four taps: bilinear weights times
  exp(-(dz/z)^2 * 400)) and writes `Enc(Bright(gas + T Lin(frame))) - T frame` with alpha 1 - T into the effects
  target as premultiplied "over": exact where nothing else was drawn, earlier passes show through by T.
- **Found on the way**: switching the setting failed with "cuImportExternalMemory (Flow's buffer): OS call failed".
  `FreeBuf` never freed the CUDA mapping of a buffer (`cuExternalMemoryGetMappedBuffer`'s pointer is CUDA's to free:
  now `cuMemFree`), but that alone did not cure it: Flow's context pools its buffers by their description, and the
  allocation it hands out again after CUDA imported it once is refused a second import. The buffers now only grow
  (a new size is a new allocation), the gas's sized for the whole frame, so the setting switches with no new buffer.
  The copies queued without a wait (the fences) made two more races: a buffer or a texture freed while the stream
  still copied into it. The worker now holds the textures' lock until its copies are queued, and both the buffers'
  growth and the textures' remaking wait for the stream first.
- **Measured** (1.2, the freeride's start, a fire and a smoke): Flow's ray march 0.56 ms at full size, 0.25 to 0.33 at
  half; the colour in and out gone, the gas out 0.08 ms; the car's roof against the fire behind it crisp (crops of the
  two screenshots compared). The setting switched both ways in the game, no error. 1.0: the fire at half size, fenced,
  none late.

## 2026-10-19: physx_gpu's water looked at in the game

- The 1.2 lab with `physx_gpu` on at the start (the lab's mod.json `enabled`): CUDA and PhysX 5.9.0 up, `pour` of
  32 768 particles steps at 5.8 ms on the RTX 3070, 373 frames of 382 drawn with their own step. On the screen no
  water but specks over the whole parking lot.
- `physx_gpu.points` (new: the particles' bounds read back from the GPU by the worker after a step) told why: right
  after the pour the block is where it should be (3.1 m a side, 1.3 m above the asphalt); 1.2 s later it is 39 x 34 m
  and reaches 17 m below the ground. Poured from 0.2 m (8000 particles) it lands whole and in about three seconds
  spreads into a sheet 30 x 19 m, one particle thick: what the screen-space surface draws as specks. So two things:
  particles falling through the game's ground at speed (the static world is a cooked triangle mesh here, NVIDIA's
  snippet has planes; its particle settings are the same as ours), and water with no depth to keep (10 cm particles
  on flat ground spread as far as they can). In ROADMAP 19a; not fixed in this block.

## 2026-10-19: physx_gpu's water fixed: it stays on the ground and looks wet

- **Asked** (the owner): go on with the water (the owner asked first whether the screenshots were simply taken after
  the water had fallen: partly, `points` showed both, see the entry before).
- **Falling through**: the static world in the half is the game's ground as a cooked, one-sided triangle mesh (its
  faces turned by the host's normals already: not the cause); NVIDIA's snippet has planes, whose inside is infinite. A
  3 m block of 32 768 particles landing from 1.5 m pushed its lower layers through the faces, after which nothing
  brings them back. `PxParticleFlag::eENABLE_SPECULATIVE_CCD` and 8 position iterations (default 4): the same pour
  stays above the ground (`points`: lowest -5.33 m, the road beside the lot) in 1.2 and 1.0 (-5.14).
- **Specks**: the water spreads into a sheet one particle thick, 10 cm particles a spacing apart, drawn at their own
  size: specks with the ground between them, each with its own bump in the smoothed depth (glints). The splats are
  now 1.8 times the particles (the thickness scaled back to the water's volume), a thin sheet takes the normal of
  the world under it (from the frame's depth) and so lies flat, what it wets is darkened (to 0.55), and its edge
  fades with its thickness (premultiplied over, which also keeps passes before it under the water). Seen: the block
  falling as a clear body with the frame refracted through it, landing, the lot dark and wet around it, glints where
  the water is deep; the cloth lying in folds; the jelly a red cube standing (from behind the car it had looked
  flattened: the side view showed it whole).
- Both labs' copies of the manifest put back to `enabled: false` (on at the next start only if switched again).

## 2026-10-19: a player's game died in dxwrapper on a thread without TLS; dxwrapper built from source

- **Asked** (the owner): why the DLSS 5 NR package (dlss_nr, made earlier today) does not start for a friend with an
  RTX 5060; then: fix everything found.
- **What the player sent**: dxwrapper's log, two lines "Error occurred in the application: code=0xC0000005
  addr=02E7ED96 bad=00000030 module=dxwrapper.dll EAX=0 ECX=0xC" on thread 37208 (dxwrapper was loaded by thread
  40196, 8 ms before), and two loader logs that stop after their third and fourth lines (no crash line, no dump).
  The same log: Windows 11 24H2 (26100), the GPU "AMD Radeon(TM) Graphics", `__COMPAT_LAYER` "RunAsAdmin HighDpiAware
  DetectorsAdminProtection Installer"; the game a Czech retail 1.2 with a mod pack (Freeride Deluxe, the 4 GB patch).
  Later the player said the package runs on a clean game (Options > Mods > DLSS 5 NR there).
- **Found**: the address is dxwrapper.dll+0xAED96 (its base 0x02DD0000: the only candidate RVA ending in ED96 whose
  instruction fits, `mov ecx, [eax+ecx*4]` with EAX 0, ECX 0xC = 0x30), in DDrawCompat's `mitigateBusyWaiting`
  (Win32/WaitFunctions.cpp): `fs:[0x2C]` (the thread's TLS vector) read for its `thread_local`. dxwrapper puts it in
  front of GetTickCount, timeGetTime, PeekMessage, GetMessage and the waits in every thread whenever D3d8to9 is on
  (`InstallDd7to9Hooks`, no setting turns it off; the labs' log shows "Installing Win32 hooks" too). A thread made
  with NtCreateThreadEx's SkipThreadAttach has no TLS vector (m417z's write-up of Windhawk's injected threads): some
  program in the player's game made one. The loader's own crash filter then faulted the same way (its re-entry guard
  was a `thread_local`), so the game died with no log line and no dump.
- **Made**: the loader: the crash filter's guard is a table of thread ids; a tap's thunk keeps no shadow stack on a
  thread without TLS (`ThreadHasTls()`, host.h). dxwrapper: built from its sources (v1.8.8600.25, the release the
  labs ran, with its submodules' archives: `tools/deps.py dxwrapper`) by the x86 preset (MSBuild on its projects as
  an external project, out to build/x86/out/dxwrapper/), with `third_party/dxwrapper/patch.py`: every HOOK_FUNCTION
  and HOOK_SHIM_FUNCTION hook calls the original on a thread without TLS (the registry hooks' function static and
  EnumDisplaySettings' thread_local were the same trap), the busy-waiting mitigation skipped there, the build number
  fixed (its pre-build step counted up and failed to write d3d8\BuildNo.rc here), and its log's first line says the
  build is ours. `tools/lab.py install` puts it in a lab (the previous kept in mml_lab, back with `mode ragdoll`) and
  now keeps a lab's own `enabled` (it had turned the labs' switched-on mods off: put back from the last runs' logs).
  dlss_nr: a game drawn on another vendor's GPU is stage `other_gpu`, its message naming both GPUs and Windows'
  per-application GPU setting, before NGX (which would have said "the driver may be too old"). `tools/pack.py
  dlss_nr` makes the player's archive (build/dist), with Ultimate ASI Loader's license now pinned beside it.
- **Tested**: offline, a thread made with SkipThreadAttach crashes in the stand-in game: the old loader left no dump
  and no report (as for the player), the new one both, the stack at harness_crash, a tap with its return on it;
  `dxwrapper_notls` calls 18 hooked functions from such a thread: the release dxwrapper dies at dxwrapper.dll+0xAED96
  with the player's log line, ours returns from all. In both labs (dlss_nr on, our dxwrapper and loader): the log's
  first line ours, no error, the graphics bridge on 9Ex, DLSS 5 NR "unsupported" on the RTX 3070, the frame's round
  trip through the half 372 of 396 frames (1.2) and 327 of 344 (1.0), 0.46 to 0.48 ms, none late, the picture right.
- **Lab**: both labs run our dxwrapper (release copies in mml_lab); dlss_nr off and its DLL out in both; flow_smoke,
  debug_views and devtools on in both, as the last runs before had them (physx_gpu off, as its session left it).

## 2026-10-19: the graphics bridge on DXVK (Direct3D 9 on Vulkan)

- **Asked** (the owner, from the player's logs; "make everything work", not a roadmap line): the player's Steam game
  ran the fixed package, but `gfx: off: Direct3D 11 could not open the shared colour texture (0x80070057)`, so
  DLSS 5 NR got no frame (the player: "everything is checked and nothing happens"). The player uses ReShade on Vulkan
  through DXVK (their dxvk.conf).
- **Found**: dxwrapper loads the real `d3d9.dll` by `LoadLibraryA("d3d9.dll")` (Wrappers/wrapper.cpp), the game's
  folder first: DXVK's `d3d9.dll` there runs the game's Direct3D 9 on Vulkan. DXVK makes a Direct3D 9 shared
  texture from Vulkan memory exported as `OPAQUE_WIN32_KMT` (d3d9_common_texture.cpp), which Direct3D 11 cannot open.
  Reproduced in the 1.2 lab with DXVK 3.1.1's `d3d9.dll` (the same error). The rest of the player's logs: the game on
  "NVIDIA GeForce RTX 5060 Laptop GPU" (dxwrapper's first line names the system's first GPU, an AMD Radeon: not the
  game's), NGX started and found the DLL; `scripts\libMafiaCountrysideMap2.0.asi` faulted once (dxwrapper went on).
- **Made**: `host/gfx_vk.cpp`: when the game's device answers DXVK's `ID3D9VkInteropDevice` (through dxwrapper's
  QueryInterface, which passes unknown IIDs on), each texture the bridge shares is two: the Direct3D 9 one (DXVK's
  VkImage, `ID3D9VkInteropTexture`) and a Direct3D 11 one made shared by Direct3D 11 (a KMT handle; an NT one where the
  driver imports only those) and imported into DXVK's VkDevice (`D3D11_TEXTURE_KMT`, a dedicated allocation; DXVK
  enables `VK_KHR_external_memory_win32` when the driver has it). At each hand-over (after Wait9: Direct3D 9's pictures
  to Direct3D 11; after Wait11: the effects back) a command buffer of ours on DXVK's queue (locked for the submit)
  copies them, DXVK's image taken from its layout and given back, the imported one from and to
  `VK_QUEUE_FAMILY_EXTERNAL` in GENERAL, waited for by a fence (DXVK's interop lock does not drain its own submission
  thread: the event query waited for before is what makes the copy safe). Every shared texture goes so: the frame's
  colour, depth and effects, the 2D pass's target, the views' colour, depth and effects, the sun's shadow map
  (shadows.cpp makes it unshared on DXVK). `gfx.state` `d3d9` says which way. A new host member `gfx_state` (1 on,
  0 off and why, -1 not yet; the Rust crate's `gfx_state()`): dlss_nr tells its player when the bridge is off ("DLSS 5
  NR: the graphics bridge is off (see the log)", the reason in the log), once a mission and at F12, and
  `dlss_nr.state` `bridge_off`. `tools/deps.py dxvk` pins DXVK 3.1.1's 32-bit `d3d9.dll` for the labs (deps.py
  learned `.tar.gz`).
- **Seen**: 1.2 lab with DXVK: the bridge on, `debug_views`' collision solids over the world in place, dlss_nr's round
  trip 333 of 359 frames (0.58 ms, none late), the picture right; `sun_shafts` with the shadow map bound, `rear_mirror`
  (its view 1548 frames), `depth_pulse` over the view (608 runs, no failure), `hud_panel` (the HUD on its panel); about
  0.5 to 1 ms a hand-over at 1280x960. 1.0 lab with DXVK: the bridge on, the collision solids, the round trip 225 of
  250. 1.2 without DXVK: as before (`Direct3D 9Ex: textures shared by handle`). The bridge off on purpose (1.0, no
  gfx mod at the start, dlss_nr loaded later: a multisampled device): the console line and the reason in the log.
- **Lab**: both labs as they were (DXVK, dlss_nr's DLL out; dlss_nr off; debug_views, devtools, flow_smoke on).

## 2026-10-19: DLSS 5 NR's first picture on an RTX 50: too dark; a crash for want of address space

- **The player's logs** (RTX 5060 Laptop, Steam 1.2 with DXVK, ReShade, MafiaWaterShaderMod.asi and a texture pack):
  `DLSS 5 NR ready on NVIDIA GeForce RTX 5060 Laptop GPU`, NGX's log made the DLSSNR feature in two missions: the
  first time NVIDIA's model ran in this mod. The player's screenshot: the menus fine, the missions' picture far too
  dark.
- **Why dark**: the mod gave DLSS-NR the frame in linear values (Lin, pow 2.2) and showed its answer re-encoded (Enc).
  RTX Remix, the one public integration (dxvk-remix: rtx_context.cpp dispatchDlssNR, the fast tone mapping shader),
  gives it `linearToGamma(ACES(hdr * exposure))` (pow 1/2.2) and reads its picture back with `gammaToLinear`:
  DLSS-NR's model works on gamma values. Given linear ones it saw a far darker scene and drew it so (the round trip
  could not show it: its encoding is symmetric). Now `color_in` is the frame as the game drew it (already in the
  display's gamma) and `color_out` is shown as it comes. Not seen yet on an RTX 50 (the labs' RTX 3070 cannot run the
  model); the round trip in both labs as bright as before.
- **The crash** (the run before the player's 4 GB patch, mise14-parnik loading): LS3DF.dll+0x79923, a `rep stosd`
  through a null pointer: `operator new(1024 * 1024 * 16)` in ITexture::Create (+0x77500) returned null, so the game
  had no 16 MB left in its 2 GB (a texture pack, DXVK, ReShade, the drivers, the bridge). The player's 4 GB patch
  (Large Address Aware) cured it. The loader now writes the address space at its start (`address space: total,
  free, the largest piece; Game.exe Large Address Aware or not`, with the advice when not) and in a crash report
  (`address_space`, tested offline); the pack's README advises the 4 GB patch.

## 2026-10-19: the bridge takes Windows' own d3d11.dll and dxgi.dll

- **The player's next logs**: two starts died at the bridge's first frame in `d3d11.dll+0x9E06F` (a read through
  null), inside D3D11CreateDevice (called through ReShade's hook of it: ReShade 6.8 runs as a global Vulkan layer and
  hooks Windows' d3d9, dxgi and d3d11 in the game). DXVK's `game_dxgi.log` beside it: the player had put DXVK's
  `dxgi.dll` in the game folder too; the bridge loaded `dxgi.dll` by its bare name, got DXVK's, and Windows'
  d3d11.dll, given DXVK's adapter, asked it for its private interfaces ("DxgiAdapter::QueryInterface: Unknown
  interface query") and faulted on the null answer. Also in those logs: the 4 GB patch on (4095 MB of address
  space), the address space line and the crash report's `address_space` working.
- **Made**: gfx.cpp loads Windows' `dxgi.dll` then `d3d11.dll` by their full paths in System32 (d3d11's import of
  dxgi binds to that one), as soon as the game makes its Direct3D 8 while a mod wants the bridge: before the mods
  load, so their own imports of d3d11 by name find these already loaded. `tools/deps.py dxvk` now also gives DXVK's
  `dxgi.dll` and `d3d11.dll`.
- **Seen**: 1.2 lab with DXVK's d3d9.dll, dxgi.dll and d3d11.dll in the game folder: the bridge on, no DXVK dxgi or
  d3d11 log (neither loaded), dlss_nr's round trip (146 frames, none late). 1.0 lab without DXVK: the bridge on.
  ReShade's layer itself was not in the labs (system-wide).

## 2026-10-19: dlss_nr writes what its frames did into the log

- **The player's next logs** (after the d3d11 fix): the game ran, DLSS 5 NR ready and its feature made in
  mise20-galery, no error anywhere (NGX, DXVK, dxwrapper, the loader), two runs of a few seconds. The player: "still
  same ... sometimes it is good but then it looks horrible". Nothing in the log could say what the frames did:
  dlss_nr's counts were only in `dlss_nr.state` (MCP, which a player has not).
- **Made**: dlss_nr logs, once a minute in a mission and at its close, the frames since: how many had DLSS-NR's
  picture, late (over wait_ms), without a picture, waiting for the textures, the history's resets (and how many at a
  camera cut), the round trip (average, most) and DLSS-NR's GPU time. Seen in the 1.2 lab (the round trip mode):
  "3085 frames, 3063 with DLSS-NR's picture, 0 late ... reset 1 times ... 0.36 ms (at most 1.72)".
- **Open**: what "horrible" is. Candidates: the motion vectors are the camera's only (moving cars and people smear),
  the model's own look on the old game, a Steam screenshot on F12 switching dlss_nr each time. The next log and a
  split-screen pair of screenshots tell.

## 2026-10-20: motion vectors for the graphics passes (the camera's and what moves by itself)

- **Asked** (the owner): item 4 of the list offered, per-object motion vectors in the bridge (DLSS's smearing of cars
  and people, and any temporal effect: TAA, motion blur).
- **Made**: `mml.h` gfx_motion (a mod asks), MmlGfxFrame `motion` (R16G16_FLOAT, pixels, bound to t3; `frame.hlsli`
  `g_motion`, `PixelBefore`; Rust `gfx_motion()`, `GfxFrame::motion()`). `host/motion.cpp`: geom.cpp's m_RenderFGroup
  hooks tell which visual and face group LS3D draws; at each of its DrawIndexedPrimitive calls the world transform is
  compared with the same draw's of the frame before (keyed by the visual, the group, the how-many-th time this frame
  that pair is drawn, the draw's place in the call); a draw that moved is drawn again at once (LS3D refills dynamic
  buffers as it goes, so later would read other vertices) into an A16B16G16R16F target with VsMotion and PsMotion
  (this frame's and the last frame's world * view * projection; motion, view depth, 1), depth-tested LESSEQUAL
  against the depth it just wrote; what writes no depth is left out. At stage 1 PsMotionAll (G16R16F, shared like
  the rest, DXVK's path too) takes an object's motion where its depth is the frame's, else reprojects the linear
  depth through the last frame's camera (inverse(view) * view before * projection before; the sky as a direction).
  dlss_nr asks for them and gives them to DLSS-NR (its own textures moved to t5, t6).
- **Found on the way**: a car's four wheels are one visual and one face group drawn four times a frame (keyed by the
  visual alone, three wheels were compared with the first one's last place: their spokes lit up in the check view);
  a car's shadow decal writes no depth, so its object motion was taken for the road under it (left out now).
  `vehicle.move` on a car just spawned does not move it (it sleeps): the test drove the player's taxi into the
  traffic instead.
- **Seen** (dlss_nr debug 5: the frame before moved by the vectors against this one, black where right): 1.2 on 9Ex
  and 1.0 on DXVK, the player's taxi driven into the freeride's traffic: the passing cars' bodies dark (debug 6,
  without vectors: bright), the taxi dark while the camera follows it, the background dark; left bright: people's
  limbs (next block), spokes' edges, the taxi's shadow's edge. 15 to 90 draws again a frame, 0.17 to 0.42 ms.

## 2026-10-20: the motion vectors of people's limbs

- **Asked**: the block after the motion vectors (the owner's item 4): a person's arms and legs with their own motion.
- **Found**: LS3D skins people on the CPU. The m_AddFGroups that single meshes ('SNGM') and single morphs ('SMMR':
  the people, their faces morphed; ls3d+55930 in 1.2, +53030 in 1.0) share skins the LOD it draws into a cache of
  positions and normals (its vtable +0x9C in 1.2, +0x98 in 1.0) when the bones changed, then copies the positions
  into the LOD's drawn buffer (a pooled FVF 0x142 buffer of some 5461 vertices): the rigid vertices from the mesh's
  own buffer (ls3d+55640, +52EA0) and then the skinned ones from the cache (smCopySkinned: ls3d+557C0, +52F70). Its
  draw (I3D_object's m_RenderFGroup) gives the LOD's first vertex in the pool as SetIndices' base vertex: the draw
  and the copy are matched by it. The hardware path (ProcessVertices into pretransformed vertices) is not taken in
  either lab (ls3d+1C563C bit 0 set: 0xE9). The layouts are the same in both builds.
- **Made**: smCopySkinned hooked (game_build.h, checked by check_builds.py through both constructors' vtables);
  each copy kept per mesh and LOD, with the one the world drew the frame before (by versions, not frame numbers:
  the sun's shadow pass and the scene views may skin a LOD before the frame's own drawing does). A person's draw
  that moved goes again from a ring of the host's (a dynamic buffer, 24 bytes a vertex: the position now and
  before; VsMotionSkin), its base vertex moved to the LOD's place in the ring.
- **Its cost**, measured: read from the drawn buffer the copies took 1.5 to 1.8 ms a person (video memory, read
  uncached by the CPU; SSE4.1's streaming loads made it 3.6 ms for two meshes against 3.1 without: no help, so it
  is most likely not write-combined). Read where the copies read (the skinning's cache and the mesh's own buffer,
  system memory the game's lighting reads too): 0.005 ms for a walking person, the whole motion 0.15 ms. The
  streamed read is kept for a source that is not locked (`copies_streamed` counts them: none seen).
- **Seen**: a copy of Tommy walking across a held camera, both labs at once: 1.2's debug 1 shows the planted foot
  still and the swinging leg at twice the body's motion; 1.0's debug 5 dark over the body and the limbs, the edges of
  what he uncovers bright (no history there: no vector can help). Not yet: DXVK, a crowd's distant LODs.
- **Then under DXVK** (1.0 with DXVK 3.1.1): the same, a walking copy and the city freeride's crowd at 30 to 40 m
  (21 LODs known, 16 with limbs moving, 80 draws again, 0.011 ms of copies, the whole motion 0.9 ms with the
  traffic's 192 moving draws); debug 5 dark over them. People who stand without an animation are not skinned again,
  so they have no copies (and need none). DXVK taken out of the lab after.

## 2026-10-20: a game in the background no longer comes up over the user's windows

- **Seen** (the owner: "why do you start the game in front?"): the start's background mode kept the game from being
  activated, but its window still came up over the user's: a window shown goes on top of the order even when it is
  not activated. Measured with a script polling the foreground window and the order every 100 ms through a launch:
  LS3D's "LS3D engine" window (later "Mafia") was second under the topmost overlays, over the user's window in
  front, the focus still the user's; the same with and without DXVK.
- **Made** (host/start.cpp): while the game is not in front, its top-level windows are shown at the bottom of the
  order (LS3D makes its window with WS_VISIBLE: made hidden, then shown at the bottom; ShowWindow the same) and
  SetWindowPos keeps their place (SWP_NOZORDER); a click brings them up as before. user32's CreateWindowExA could not
  be hooked (MinHook: unsupported function): its first bytes are already a hot-patch jump, `EB F9` back to a `jmp`
  into dxwrapper.dll+189E40, dxwrapper's own hook; LS3DF.dll's import of it is taken instead.
- **Seen**: 1.0, with DXVK and without: the game's window never above the user's through the launch, at the bottom
  after it; screenshots from the background as before. Not yet in the 1.2 lab (the owner was playing there).

## 2026-10-20: who ends the process, in the log

- **Seen**: a player's game (RTX 5060 Laptop, dlss_nr working, DXVK, ReShade 6.8.0 as a Vulkan layer, ThirteenAG's
  scripts) quit by itself 0.7 s after coming back from a mission to the main menu, twice, with no dump and no word:
  the log's last line was "process detaching", dxwrapper and ReShade wrote their normal exits. So it was ExitProcess,
  not a fault. His whole pack (his loader, his ASIs, his DXVK and settings, ReShade 6.8.0 loaded as a Vulkan layer
  for that process tree only, his saves, his route: a save of mission 20, the pause menu, the main menu) ran in a
  copy of his folder here (RTX 3070: NGX refuses DLSS-NR, the 64-bit half's round trip ran instead) and never quit.
  The one part not run here is DLSS-NR itself.
- **Made** (host/main.cpp, host/crash.cpp ExitStack): DllMain's detach at the process's end runs on the thread that
  called ExitProcess, so the log now says "process exiting (ExitProcess on thread N, the game's): <the calls>" from a
  scan of that thread's stack (as a crash's report finds them); "process detaching" stays for the loader unloaded.
- **Seen**: 1.2, the main menu's Quit: Game.exe+0x1216, the CRT's exit (Game.exe+0x2161E3), ExitProcess; 1.0, the
  window closed: through ls3df.dll. The player gets the new loader in the dlss_nr pack: his next log names who ends
  his game.

## 2026-10-20: dlss_nr with ReShade's effects

- **Seen**, in the player's folder: ReShade 6.8.0 (a Vulkan layer over DXVK) with his preset's six effects compiled
  (crosire/reshade-shaders legacy and slim's headers, SweetFX) and dlss_nr's round trip running (debug 4): his route
  (a save of mission 20, the pause menu, the main menu) kept going. A test mode that sent the frame back upside down
  (to see the round trip at a glance) was tried in the labs and taken out again: the released mod shows DLSS-NR's
  picture only.
- The player's update: `tools/pack.py dlss_nr --since` the archive he has (Discord takes 20 MB at most).

## 2026-10-20: the Mafia demo's LS3D.dll carries the engine's names

- **Asked** (the owner): whether the demo (one mission, the full game's 12, "Great Deal!") makes the engine easier
  to reverse. Downloaded from the Internet Archive (`Mafia_201405/Mafia.exe`, a RAR, not run: unpacked with 7-Zip
  into `re/demo/`, ignored by git).
- **Found**: its LS3DF.dll is byte for byte the 1.0 lab's (3.84.5, 31.7.2002); beside it an `LS3D.dll` of 2002-07-04
  with CodeView NB11 debug information: 9892 public symbols with their C++ signatures (3109 functions of `.text`,
  2294 globals, the vtables by class), no types (5 records). The code is the same as LS3DF's (I3D_singlemesh::
  m_AddFGroups 3232 bytes against 1.0's 3083, PoseLod about 2000 against 1954). Its Game.exe (2002-10-31) has no
  SafeDisc sections and no symbols; `prog_dbg.bin` the demo mission script's names.
- **Used at once** (item 28): PoseLod, CopyRigidPositions, CopySkinnedPositions, LightVertices (with its parameters' types),
  LightsOfSector, EnvMapCoords, SkinOneBone(W), I3D_singlemorph::PoseLod named in both builds' Ghidra and
  `knowledge/ls3df.dll/names*.csv`. The whole import (matching the demo's functions to 1.0's LS3DF, carrying them to
  1.2) is a session of its own (`re/demo/cv_publics.py` dumps the publics).

## 2026-10-20: people's skinning with up to 8 bones a vertex (item 28a)

- **Asked** (the owner): item 28 whole. Read in both builds (knowledge/notes/skinning.md): I3D_singlemesh::PoseLod
  skins a LOD into its cache with two bones a vertex at most (the .4ds holds no more), the joints' matrices built in
  the mesh's space and kept at `+0x1C4`; m_AddFGroups copies the cache's positions into the drawn buffer and lights
  the vertices from its normals.
- **Made** (host/skin.cpp, mml.h skin_set and skin_get, the Rust crate's): PoseLod hooked; a model's single mesh a
  mod gives a skin (`models\<name>.skin.json` among its files, skin_set, the bridge's `skin.set`) is skinned by the
  host into LS3D's own cache: up to 8 bones a vertex, any joint of the mesh's subtree, each bone with its own matrix
  at rest; the joints' matrices are built as PoseLod builds them (I3D_frame::UpdateMatrix called on the way), the
  normals through each matrix's columns normalised (S_matrix::SetRot), the buffers locked as LS3D locks them (1.2's
  list of locked buffers, 1.0's NOSYSLOCK). CopyRigidPositions hooked for a skin that moves the rigid vertices too. Every
  other model goes straight to LS3D's code (a mesh's skin is looked up once until a skin comes or goes).
  `skin.export` writes a model's own skin from its .4ds (models.cpp now keeps the bone records and the joints' ids);
  `tools/skin_smooth.py` spreads one over more bones along the mesh (the uv seams welded by position).
- **Seen**, both labs at once: Tommy's own skin exported and given back skins as LS3D does to 1.2e-7 m (normals
  exact; `skin.check`: LS3D 0.017 ms, the host 0.023 ms for 778 vertices); smoothed to 4 bones a vertex (492 of 778
  vertices with more than two) a copy of Tommy with a knee bent 100 degrees: the hard fold of the trousers behind the
  knee becomes a rounded bend in both builds (0.04 ms a skinning).

## 2026-10-20: people skinned on the GPU (item 28b)

- **Made** (host/skin.cpp, host/shaders/skin9.hlsl, sdk/include/mml/shaders/skin.hlsli, mml.h skin_gpu and
  MML_SHADE_SKINNED, the bridge's `skin.gpu`): while a mod asks, every single mesh's LOD that can be is drawn by a
  vertex shader of shader model 3. The palette (6 texels a bone: the position's matrix's columns, the normal's, a
  float texture read by the vertex shader), the rest pose (a single morph's after its morph: the face moves) and a
  buffer of what does not move (uv, a MikkTSpace tangent from the .4ds's triangles: third_party/mikktspace, the bones
  and weights, 8 a vertex) are the host's; PoseLod only builds the palette, CopyRigidPositions and CopySkinnedPositions copy
  nothing. LightVertices is LS3D's own still: run on one vertex of a copy of the LOD's level, its choice of the 8
  lights (the slots it keeps in its globals: their place and direction in the mesh's space, colours times the
  material, near and far, the spot's cone) becomes the vertex shader's constants; mml_SkinLight repeats its sum (the
  128 scale found at ls3d+A9470: the stage's modulate2x doubles it back), mml_SkinEnvUv EnvMapCoords's modes 0 and 1.
  shaders.cpp draws a person with VsSkin and PsSkin (reference.hlsl's mml_ps), or a rule's pixel shader with the
  normal and tangent among its inputs, or a rule's own vertex shader with MML_SHADE_SKINNED; the sun's map with
  VsSkinDepth; motion.cpp's pass with VsSkinMotion and the last frame's palette.
- **Seen**, both labs: a posed Tommy drawn by LS3D and by the GPU differ by 0.16 of 255 on average (1.2 and 1.0); with
  a mod's 4-bone skin the GPU and the CPU path the same (0.16); a rule's pixel shader showing the normals and the
  tangents on a crowd of 96; the motion vectors (dlss_nr's debug 5): a walking Paulie disappears with them, stands
  out without them. The motion pass first used one matrix where VsSkin uses three: its depth test against the draw's
  own depth then failed on parts of a swinging arm; now the same three.
- **Measured** (fps_unlock on; the owner's point: one game at a time, the two labs at once add to each other's
  times): 48 people, about 300 draws: 1.2 7.3 ms a frame with LS3D's skinning, 8.2 ms on the GPU; 1.0 7.7 and 9.0.
  The CPU's part falls to 0.2 ms; the switch of the device between the fixed pipeline and shaders at each person's
  draw costs more (the driver: nvd3dum 6 % of the samples against 1.4). With a rule shading every object both are
  even. A first version read all of game.hlsli's registers and saved every sampler at each draw: +3.5 ms; now 25
  calls (CoreConstants) and only stream 0 and the FVF put back. Not the plain game's speed-up, then: what it is for
  is in MODDING ("People's skinning", "On the GPU") and the costs in ROADMAP Known gaps.

## 2026-10-20: a mod's own registers and vertex data for the shader rules (item 28c)

- **Made** (host/vdata.cpp, shaders.cpp, mml.h shader_constants, shader_vertex_data, MML_SHADE_VERTEX_DATA,
  game.hlsli mml_Mod and the stream's semantics; the bridge's `shader.constants`, `shader.vertex_data`): a rule's
  registers c160..c223 set at each draw it takes; with MML_SHADE_VERTEX_DATA its draws of objects carry a second
  stream (TEXCOORD5 the mesh's normal, TEXCOORD6 its MikkTSpace tangent, TEXCOORD7 the mod's float4). LS3D draws its
  objects from pools it refills on the CPU (a LOD's record +0x10, +0x18 its place: the draw's BaseVertexIndex, which
  Direct3D adds to every stream), so the stream is a mirror of each pool, a LOD's vertices at the same place, written
  when the place changes hands; the vertex layout is the draw's FVF with the three elements after it. The normals
  and uvs are the LOD's own vertex buffer's (LS3D keeps some locked: read there), the triangles its face groups'
  index buffers; MikkTSpace is now one function (vdata.cpp) for people and objects.
- **Seen**, 1.2 and 1.0 alone each: the city's objects shaded by their normals (buildings, cars, trees, the
  pavement), a register's tint changed live, a mod's float4 a vertex on the player's car body (bands of 40 vertices).
  Found on the way: a lightmapped object's draw may come from a vertex set of its own (no LOD record, another base,
  one more vertex): left with zeros (ROADMAP Known gaps); the people's single meshes are left to skin_gpu.
- The owner's asks for later: every format of models, textures and animations (converted at the first load: ROADMAP
  Later); motion vectors checked without a mod (ROADMAP Later).

## 2026-10-20: the Mafia demo's own names for LS3D, on both builds' LS3DF

- **Found**: the Mafia demo's `LS3D.dll` (linked 2002-07-04, engine 3.76) still carries the linker's CodeView NB11
  publics: 3109 functions with their mangled signatures, 406 globals, 106 vtables. Its LS3DF.dll is the 1.0 lab's,
  byte for byte; its code is the same engine, plus qhull, DirectShow's base classes and the editor's code, linked
  without identical-code folding.
- **Made**: `tools/kb_demo.py` reads the publics, finds the functions of the three DLLs itself (exports, the code
  reached from them, switch tables, relocated pointers), pairs demo -> 1.0 -> 1.2 by their code (the exports, the
  SDK's tables, the names we gave alike in both builds, exact and loose shapes, strings, calls, callers, vtable
  slots, the link order) and demo -> 1.2 straight as a check, then the globals by the addresses the paired
  instructions use. The demo's names stay on this machine (`demo.local.csv`, `demo_10.local.csv`: its licence lets
  nothing of it be copied); `knowledge/ls3df.dll/twins_10.csv` (1.0's twin of 2279 1.2 LS3DF functions) is
  committed and gives 1.0's LS3DF 1.2's names too. `ghidra_names` ranks the demo's names over MafiaReverse's, under
  ours; the demo's signatures are 1.0's prototypes (`demo_signature`: a stdcall method's `this` on the stack). MCP
  `kb_search` finds them in both builds; `ghidra_read twin` answers for LS3DF.
- **Two traps met**: the vtables' slots moved between the demo and 1.0 (methods added), and three IGraph keyboard
  methods, the same code but for one byte of the keyboard's state each, came out shifted by one: a slot is now
  paired by its place only beside a slot paired in the same place, and a pair whose writable data moves against the
  other pairs' is refuted (the read-only literals are placed in no order: not used). 1.0 folds identical functions
  (two classes' alike getters) that 1.2 keeps apart: where the two ways differ the straight one wins.
- **A tool's fault fixed** (ghidra_server.py): applying a prototype (`... f(...)`) renamed an unnamed function "f"
  (Ghidra's ApplyFunctionSignatureCmd renames a default-named function by default): 947 Game.exe and 1179 LS3DF
  functions of the 1.2 project were called "f". set_proto keeps the name now (FunctionRenameOption.NO_CHANGE), and
  `sync` gives those back their default names.
- **Seen**: 1798 of the demo's functions in 1.0, 1803 in 1.2, 1638 high in both, 1622 found both ways alike, 1
  differs; the 74 functions whose messages name their method all right; the hand-checked names of names.csv and
  names_10.csv all agree. `sync` run on a copy of both projects (not the shared ones): 1.0's LS3DF 1990 named, 1535
  prototypes; 1.2's 1674 named; a second sync changes nothing. The shared projects get them after a merge: `python
  tools/ghidra_cli.py sync` and `--build 1.0 sync`.

## 2026-10-20: measuring without guessing: each frame's record, the device's calls, comparisons, a still world

- **Asked** (the owner, before item 29 and a GPU skinning that costs more than it saves): the tools for measuring
  conveniently and without errors, the missing ones made.
- **Made**: each frame's record (prof.cpp, `prof.frames`): a frame from one return of the game's Present to the
  next, its time, Present's blocking, the game thread's own CPU time from its cycles, the GPU's span (timestamp
  queries, host/d3dstats.cpp); `prof.counters`: every call of the game's Direct3D 9 counted and timed by kind and by
  caller (the game, the loader: an outer link kept outermost in the hook chains, hooks.cpp HookAddOuter, sees the
  caller's own return address), the driver's time under the draws (an inner link kept innermost), the draws by
  pipeline and the switches between them, the loader's skinning hooks timed (`hot_ms`); `prof.mark` labels frames;
  MCP `bench {a, b}`: two configurations taking turns in ABBA/BAAB blocks, medians, B less A with a 95 % interval
  and the noise of one configuration measured twice, the run's conditions (other games, the machine's load, frames
  not presented); `state.time {scale}`: the game's step scaled, 0 a still world; `tools/mcp_call.py` (a tool of the
  server just built, from a terminal).
- **The sampler made right** (prof.cpp, mcp/src/stacks.rs, syms.rs): a timer between samples (no core kept busy;
  1 ms is the least the timer gives), the top of the stack copied while stopped (`stopped_percent` 1.3 to 1.7 %),
  the return addresses on it with their direct calls' targets, `profile`'s functions self, calls out and total. The
  first try counted every return address on the stack: `std::vector::_Tidy` "on 72 % of the stacks", a stale address
  in a frame's unwritten locals. Now the system's and the loader's frames are walked by their frame pointers (the
  loader built /Oy-) up to the game's first frame, and above it a return address is kept only where its call leads
  to the frame below (calls through a pointer at a fixed place resolved too). The loader's `Camera` seemed to spend
  14 % of the frame in Direct3D: a stale address in d3d9's frames, gone with the walk. The loader's and mods'
  functions named from their PDBs (the `pdb` crate).
- **Three faults of the tools found and fixed** on the way: 1.0's addresses were named by the nearest name below
  within 64 KB (kb.rs name_10: `I3D_projector::ProcessRecList` for 1.0's main loop): the Ghidra projects' function
  bounds are now in `knowledge/<module>/bounds.csv` and `bounds_10.csv` (`ghidra_cli.py bounds`), and an address is
  named only by the function that holds it; MCP's work moved to `spawn_blocking` lost the call's `game` (a task-local:
  1.0's samples named as 1.2's): `game::with_game_sync`; the frame's record first read the process's cycles each
  frame (QueryProcessCycleTime walks every thread: 2 % of a 2.8 ms frame): dropped, `bench` takes the process's
  CPU over the run.
- **A still world, twice**: the first `state.time 0` only gave every system a step of 0; the owner saw the traffic
  roll on. C_Vehicle::Tick takes 0 ms as 1 (knowledge/notes/vehicle_physics.md): at 250 frames a second the cars
  went at a quarter of their speed (6 cars of the traffic found through `C_driver::Tick`'s `tap`: 4 to 14 m in 2 s).
  Now a frame whose step comes out 0 does not tick the mission at all (the game's caller reads no result, as in its
  pause): the same cars 0.0000 m in 2 s, and 4 to 39 m with the time back, in 1.2 and 1.0.
- **Seen**, both labs: `bench` A against itself finds nothing (1.2: +0.007 ± 0.018 ms on 2.8 ms frames, the machine
  quiet; 1.0: -0.02 ± 0.08 ms on 5 ms frames, 11 % of the machine busy elsewhere); the counters' own cost +0.05 ms a
  frame (some 35 ns a call); a still world's frame 2.09 ms against 2.8 with the mission ticking. The shares the
  sampler gives: 1.2's freeride, LS3D's visibility (the portals' walk, `SetVisibleRecMainPass` 14 % self), the tram
  (`C_railway::Update` 15 %), Present 15 %; 1.0's portal culling and its vertex buffers' Lock
  14 %. The loader's own `EnsureCrashFilter` (SetUnhandledExceptionFilter every frame) 1.2 to 2.2 % of the frame:
  ROADMAP Known gaps.

## 2026-10-20: the crash filter no longer set again every frame

- **Seen** (item 30's sampler): the loader's `EnsureCrashFilter`, called each frame to put its filter back in front
  of any other program's, spent 1.2 to 2.2 % of the frame inside KernelBase's SetUnhandledExceptionFilter. Not
  hooked by anything (its first bytes the system's own prologue), but on Windows 10 it does far more than swap a
  pointer (a 0x224-byte frame).
- **Made** (host/crash.cpp): SetUnhandledExceptionFilter hooked at the start (KernelBase's: kernel32's jumps there).
  A filter another sets goes after ours, its caller given what came before it (what it would chain to): ours stays
  first without being set again. The hook runs on any thread (no TLS, no log: the frame logs what it counted).
  Without the hook, ours is set every frame as before. `crash.filter`: the state (`hooked`, `others_set`,
  `chained_to`), and `every_frame` to bring the old way back for a comparison. Offline: the harness sets a filter of
  its own after the loader; the loader's dump is written, then the harness's filter called, and it was given its own
  runtime's, not ours.
- **Measured** with MCP `bench` (every frame against hooked, 12 blocks, the counters off): 1.2 -0.056 ± 0.020 ms a
  frame of 2.68 (the game thread's CPU -0.061 ms), 1.0 -0.062 ± 0.032 ms of 2.68. Both games set their own filter after
  the loader starts (1.2 once, Game.exe+0x218D64; 1.0 twice): now chained after ours.
## 2026-10-20: the game's videos, and why a modder's upscaled intros did not play

- **Asked** (the owner: a modder upscaled the intros, put them in the game, and they do not play, with no visible
  reason): how the videos are played in both builds, what limits them, why the upscale fails, how to lift it.
- **Found** (both builds' code, `knowledge/notes/videos.md`): 1.2's `PlayVideo` (`0x55D460`) opens `<name>.avi`
  first through DirectShow and only then `<name>.bik` through Bink; 1.0's LS3DF has no Bink at all (AVI only; the
  1.0 lab's `binkw32.dll` 1.5p is a leftover nothing loads). The files are loose (`CreateFileA` in the game's folder,
  then `X:\MafiaGame\` on every drive): no archive, no `dtaOpen`, so a mod's `files\` does not reach them. LS3D cuts
  a video into textures of the device's largest size (16384 in both labs): no size limit, but 16-bit colour
  (`PlayVideo` gives Open no `0x400`). A failure is only a `dbgPrintf`.
- **The reason**: 1.2's `binkw32.dll` is Bink 1.9c (2008) and its BinkOpen accepts only the revisions `BIKf`..`BIKi`
  ("Not a Bink file." otherwise). RAD Video Tools 2026.09 (the free tools, downloaded to `W:\tools\radvideo`)
  writes `BIKi` for "Bink 1" (`/V100`), `BIKk` for "Bink 1.1" (`/V101`) and `KB2*` for Bink 2: an upscale made as
  Bink 1.1 or Bink 2 is skipped silently. A second way to the same symptom: on a machine whose DirectShow decodes
  Indeo 5, the original `.avi` beside the new `.bik` plays instead.
- **Seen**, with `tap` and `native.call` of the game's own players from the main menu: 1.2 at start, `logo2.avi`
  failed (no Indeo 5 decoder: 0x80000001) and `logo2.bik` played; a 640x480 `BIKi` played (126 frames copied); a
  1280x960 MJPEG AVI played in 1.2 and in 1.0 (126 frames each, one 1280 x 960 tile); H.264 in AVI and the original
  Indeo AVI did not open in either (0x80000001).
- **Not done**: an upscale encoded by RAD's compressor and played in the lab (`BIKi` from `/V100` should play,
  `BIKk` should not): the compressor's first start shows RAD's licence, which is the owner's to accept.
- **Next**: ROADMAP "Later", "The game's videos": the loader takes the videos over in both builds (a mod's files,
  the reason logged, 32-bit textures, MP4 through Media Foundation).

## 2026-10-21: the game's videos played by the loader, with FFmpeg

- **Asked** (the owner, after the investigation above): the loader plays the intro videos itself through FFmpeg
  (every main codec and old Bink 1 of any revision), in both builds, found in the mods' `files\`, in 32-bit colour,
  skipped by a key.
- **FFmpeg**: no 32-bit LGPL build newer than 6.0 (2023) is published, so it is built here: FFmpeg 9.0.2 (its PGP
  signature checked against the release key) and dav1d 1.5.4 (VideoLAN's published hash), pinned in tools/deps.py,
  built by third_party/ffmpeg/build.py with MSVC for x86 (dav1d by Meson, FFmpeg's configure and make in MSYS2's
  shell; MSYS2's pkgconf and Meson installed for it), only the decoders, demuxers and parsers a video needs: avcodec
  8 MB. LGPL 2.1 or later (no GPL, no nonfree); `BUILD.txt` says where the sources are and how they were configured.
  The first build failed in the dependency files' awk: Git's awk (another MSYS runtime) came first on PATH and lost
  the backslashes make gave it; MSYS2's tools now come right after MSVC's.
- **Made** (host/video.cpp): IShow's open, close and full-screen loop hooked in both builds, and the
  game's file search (cd_find_game_file) for a video's name; the file looked for by its base name in the mods' files
  then the game's folder (mp4, mkv, webm, mov, bik, avi); FFmpeg's thread decodes into BGRA and 16-bit stereo; the
  game thread shows each picture when the clock reaches it on the game's Direct3D 9 device (a state block around the
  quad: LS3D shadows the states it sets), through IGraph's Clear, BeginScene, EndScene, Present; the sound on LS3D's
  own IDirectSound, each sample placed on the clock; any key ends it (the game's ReadKey), and the bridge is served
  meanwhile. `video.state`, `video.find`, `video.play`, `video.skip`; VIDEO_STARTED, VIDEO_ENDED, VIDEO_REFUSED.
  BridgeFrame no longer runs inside itself (a command that plays a video). game_build.h: cdFind, showOpen,
  showClose, showRenderUntilKey, sndDriver, readKey (tools/check_builds.py checks them in both builds' files).
- **Found on the way**: 1.2's IShow has one vtable slot more than 1.0's before the loop (+0x50 against +0x4C), and
  1.0's IGraph lacks GetResInitDesc, so its slots from +0x64 on are 4 bytes lower (keyboardUpdate and ReadKey +0x94,
  +0x98 against +0x98, +0x9C): check_builds' new check caught the note's +0x4C for 1.2.
- **Seen**, both labs at once, the game's usual start: 1.2 played logo1.bik and logo3.bik (BIKi) through FFmpeg
  (126 and 151 pictures, none dropped) after passing over a mod's fake Bink 2 logo1.bik with its reason; a mod's
  1920x1080 H.264 logo2.mp4 in both builds (1.2: 837 pictures in 33.6 s, 1 dropped; 1.0: skipped by `video.skip`
  after 526, none dropped), letterboxed into the 1280x960 frame, screenshots taken while it played; 1.0 showed its
  logos for the first time on this machine (its own are Indeo AVIs Windows cannot decode). 1.2's intro played by the
  new game's script command from a mod's AV1 intro.mkv (300 pictures), 1.0's by `video.play` from a VP9 intro.webm.
  `video.find` opened the game's own intro.bik and its Indeo 5 intro.avi alike. Without mml\ffmpeg: the game's own
  Bink played the logos, the log said why. Offline: tests/run_tests.py video_tests (the harness: FFmpeg loaded, the
  order, Bink 2 refused, an H.264 clip opened).
- **Not seen**: a real `BIKk` (none at hand; RAD's compressor needs its licence accepted: the owner's call).


## 2026-10-21: settings watched, not polled; tools/new_mod.py in the offline tests

- **Settings** (host/config.cpp): the two config.json files were looked at twice a second (two attribute reads a
  mod a look, a change seen up to 500 ms late). Now each folder that holds one (the mod's folder, its data folder)
  is watched with ReadDirectoryChangesW, overlapped, and no thread: the game thread looks at each watch's OVERLAPPED
  every frame (a read of memory; nothing is called while nothing changes), takes the names when it completed and
  issues the next read. A name that is one of a config's files (or a folder above it, renamed or removed) makes it
  stale; it is read once no change came for 10 ms (an editor's save, a burst of writes, is one change), so the mod
  is told at the second frame after the write. A buffer that overflowed or a watch that broke (its folder removed)
  makes every config of that folder stale; a broken watch is opened again twice a second. Our own writes
  (config.set, a live change flushed) come back as changes: read, the same, not told.
- **A file its writer holds**: it was read with ifstream, and one that could not be opened counted as no file (the
  defaults for a moment, then the values again: two events). Now CreateFile tells: no file (the defaults), held
  (a sharing or lock violation, a delete pending: the values in use stay, read again at a frame, no warning; after
  5 s a warning and no more tries until the next change), anything else (logged, the values stay).
- **Measured offline** (the harness's frames are some 31 ms: Sleep(16) at the default timer): five edits seen by
  the mod in 122 to 125 ms, 4 frames each counted from the call before the write to the call that saw the event
  (the event itself at the second frame after the write); a burst of 10 writes in 20 ms one event with the last
  values; the defaults' file edited and put back; a file held with no sharing not read until let go; the changes'
  file deleted: the defaults in 93 ms.
- **tools/new_mod.py in the offline tests** (`new_mod_build`, `new_mod_tests` in tests/run_tests.py): the tool run
  as an author runs it, a C++ mod and a Rust one (`--rust`, skipped with a note when the build has no Rust mods),
  their sources in `build/x86/new_mod_test/` (`--dir`), configured and built by the project's build (their targets
  only), copied into the harness switched off; then the CMake lines taken out again (and any an interrupted run
  left), the sources removed, the build configured again (so it forgets their targets and removes their output
  folders) and CMakeLists.txt checked to be what it was. In the harness each loads, answers `<id>.status` and
  unloads; its folders there are removed after.

## 2026-10-21: a crash's stack checked and named as a profile's

- **Why**: the crash filter's stack was a scan checked only roughly (a direct call's target within 32 KiB below a
  frame kept), so leftovers stayed after an indirect call (an LS3DF address between C_game::Tick and
  C_mission::Tick, seen), and `crash_triage` named LS3DF 1.0's and the mods' frames only as module+offset. The
  profiler (2026-10-20) already read stacks exactly.
- **Made**: host/stack.cpp, one reader for both: the walk by frame pointers through the system's and our frames to
  the first frame of the game's (each return address on the way must follow a call), then every return address into
  the game's or our code with its call's target. The crash filter asks the modules as it goes (VirtualQuery, the
  regions remembered; no TLS, no snapshot); the sampler by its snapshot, now searched by base. The report's `calls`
  keep each target ("frame" when walked); MCP's `checked_calls` (stacks.rs, shared with `profile`) keeps the checked
  ones, syms.rs names them (the PDB found beside any module, matched by GUID with the dump's CodeView record).
- **Found on the way**: today's MSVC calls a hook's original as `mov eax, [g_original]` / `call eax` (the test mods'
  detours, seen in their disassembly): those were unchecked; CallTarget now reads the load before the call (only
  pushes and loads into other registers between). A detour's call of the original lands in MinHook's trampoline,
  not in any function: hooks.cpp keeps a lock-free table of the chains' trampolines and their targets. A tail call
  (a function ending in a jump to another) made its caller's return address look stale, and every frame above it
  went with it: such a frame is now held and kept when a frame above leads to it.
- **Offline**: run_tests.py: the first call of `crash` is main's call of harness_crash; `crash_hooked` (main ->
  hook_b's detour -> hook_a's -> the trampoline -> harness_add -> harness_crash) has each call's target right
  through the patch, the chain's thunk, `call eax` and the trampoline; the exit line walks from the loader's
  DllMain through ntdll to the harness. mcp_smoke.py: `crash_triage`'s stack is exactly harness_crash, harness_add,
  hook_a's Detour (by its PDB), main..., no loader's leftovers, the dropped counted; `crash_mod` (a fault in hook_b's
  detour: a mod's hook runs unguarded) gets the verdict "in mod_hook_b_... (mod hook_b: ...Detour+0x..)". Not yet
  seen on a lab crash: the labs were in another session's use.
