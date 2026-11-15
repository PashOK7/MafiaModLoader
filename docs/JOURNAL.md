# Journal

What was done, found and tried, in order (newest last). Agents: read the end first, append as you go.

This is chronological evidence, including rejected explanations and replaced implementations. Use
[STATUS.md](STATUS.md) for current conclusions and [the patch notes](PATCHNOTES_2026-11-15.md) for the
pending update. Old PIDs, hashes and local build paths do not identify the current/public build.

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

## 2026-10-21: the crash stacks seen in both labs (after the merges)

- **Merged** into main: the settings watched and new_mod.py's test (claude/gifted-germain-f1d806), the crash stacks
  checked and named (claude/quizzical-chatelet-f5c5e4); offline tests and mcp_smoke ALL PASSED on the result.
- **Seen**, both labs at once: a crash provoked in each (the player's model frame pointer, actor +0x68, written
  0x10: the game reads it next tick). 1.2: `C_game::Tick+0x328` (a read of 0xBC), `C_mission::Tick+0x8F`, the
  loader's `hkMissionTick+0x39`, `GameLoopSingle1+0x19F`, `MafiaEntryPoint+0x628` and the runtime's start; 14
  leftover return addresses dropped. 1.0: the same chain by 1.0's names (`C_game::Tick+0x302`, `GameLoopSingle`,
  `main`, `winmain`, `crt_startup`), 12 dropped. Every frame named, none left over between the game's and the
  loader's.

## 2026-10-21: the videos decoded by the GPU

- **Asked** (the owner, after asking whether a 4K 60 fps intro would play): hardware decoding in the loader's player.
- **Measured first** (six 10-second 4K60 clips, 600 pictures each: H.264, HEVC, HEVC 10-bit, VP9, AV1, AV1 10-bit, a
  hard picture: testsrc2 with moving noise; `video.play` from the menu, both labs one after the other; the machine a
  GeForce RTX 3070): the software path showed 595 to 600 pictures of the 8-bit ones, each costing swscale 8.9 to
  10.4 ms (4K to 4K BGRA) and the game thread 7 to 8 ms of upload (33 MB, peaks 36 ms); the 10-bit ones only 246 to
  292 pictures in 20 s (swscale 67 to 80 ms a picture), cut by the length guard, the picture behind the sound.
- **Made** (host/video_hw.cpp; FFmpeg rebuilt with D3D11VA and DXVA2 and its own AV1 decoder, which decodes only on a
  GPU): the driver asked before the decoder is opened whether it has the codec's profile at the stream's bit depth and
  size (D3D11: GetVideoDecoderConfigCount; DXVA2: the service's GUIDs, formats and configurations); D3D11VA on a
  Direct3D 11 device of the loader's on the game's adapter (its LUID), else DXVA2 on a Direct3D 9Ex device of its own;
  each picture turned into BGRA at its size on the screen by the GPU's video processor (D3D11 VideoProcessorBlt,
  DXVA2 VideoProcessBlt: the matrix and range from the stream) into a texture the game's 9Ex device made and shares,
  waited for with an event query; the game draws it and the slot is used again once an event query after the draw
  is done. Where sharing fails (DXVK) the small picture is copied through the CPU. A GPU failure mid-way (or a decoder
  failing on its first pictures) falls back to the CPU, logged. The software path got swscale on all cores
  (sws_scale_frame) and scaling to the size on the screen; late pictures are left out by the decoding thread.
  Untagged HD streams are BT.709 in every path (swscale alone took BT.601). `decode` for video.play and video.find,
  `path` and `timing` in VIDEO_ENDED and video.state.
- **Found on the way**: dxwrapper (D3d9to9Ex) wraps every Direct3D 9 device of the process, System32's too, and DXVA2
  cannot use its wrapper (GetDecoderDeviceGuids: E_FAIL, 0 decoders); dxwrapper's own IID_GetRealInterface gives the
  real device under it. DXVA2's VideoProcessBlt refuses (E_INVALIDARG) a ProcAmp of zeros: contrast and saturation 1.
  StretchRect from a decoder's surface works but leaves the studio range unexpanded (washed-out colours).
- **Seen** (both labs, the same clips): auto (D3D11VA, a shared texture) 600 of 600 for all six, the GPU's work and
  wait 0.9 to 1.5 ms a picture, no upload, the frame 0.5 ms; DXVA2 forced 599 to 600 (convert 0.7 to 1.0 ms); software
  forced 584 to 600 (convert 4.8 to 7.4 ms, upload 0.8 to 2.3); with DXVK's d3d9.dll in the 1.2 lab (taken out after)
  the copy path, 600 of 600 through D3D11VA (2.6 to 4.0 ms) and DXVA2 (5.2 to 5.5 ms). Screenshots of each path: the
  same colours.

## 2026-10-21: names of our own for the demo's

- **Why**: the Mafia demo's licence lets nothing of it be copied, its LS3D.dll's CodeView symbols included; the names
  taken from them on 2026-10-20 (item 28, the videos' lines) went into the repository. They stay on this machine now
  (`knowledge/ls3df.dll/demo*.local.csv`) and the repository says its own: I3D_singlemesh::PoseLod,
  CopyRigidPositions, CopySkinnedPositions, I3D_singlemorph::PoseLod, LightVertices, LightsOfSector, EnvMapCoords,
  SkinOneBone, SkinTwoBones (names.csv, names_10.csv, the notes, the hooks' names in host/skin.cpp, the docs).
- **Made**: `tools/check_demo_names.py` (in the offline tests): every name of the demo's that no other source gives,
  looked for in each file git tracks; names the repository had before the demo was read, and the names the Windows
  SDK's headers that the sources include declare (dxva2api.h's ResetDevice, the demo's LS3D has one too), are
  anyone's. Each line's words matched against a set (3.7 s; one regex of 1500 alternatives took 17 s and reported only
  the longest of overlapping names). `kb_demo.py --fetch` downloads the demo (the Internet Archive, SHA-256 pinned)
  into `re/demo/` and reads LS3D.dll out of it with 7-Zip, never installing it: another machine makes its own names.
- **History**: the 35 commits not yet pushed rewritten (git filter-repo, `origin/main..main`) with the same names, so
  none carries the demo's: the renaming commits' own hunks first, then word by word, `names*.csv` rows from the final
  tree; each commit then passed check_demo_names.py, the last tree is byte for byte the one before, 61770d2 kept its
  hash (check_demo_names.py's BEFORE_DEMO); the videos' renaming commit fell out empty. Pushed.

## 2026-10-21: the game's own intros as 4K60 HEVC, from beside Game.exe

- **Asked** (the owner): the intros in any format, played from the game's usual place, tried with 1.2's own
  re-encoded to 4K at 60 fps. Nothing to make: video.cpp already looks for each name the game asks for with every
  extension of kExts (mp4 first) among the mods' files and then loose in the game's folder.
- **Made** (the 1.2 lab, W:\MafiaLAB, beside the original .avi and .bik): logo1, Logo3, logo2, intro re-encoded from
  their .bik (ffmpeg: lanczos to fit 3840x2160 and padded to it, fps=60 by repeated frames, hevc_nvenc p5 cq 24, AAC
  192k; Logo3 silent), `<name>.mp4`: 1.0, 1.7, 83, 773 MB.
- **Seen** (the game's usual start, then a new game; RTX 3070): every one played from "the game's folder", D3D11VA
  into a shared texture, shown at 1280x720 in the 1280x960 window. logo1 301 of 302 pictures (1 dropped), Logo3 362
  of 362, logo2 2011 of 2011, intro 13733 of 13733 in 229.0 s, none late; a picture's decode 0.24 to 0.36 ms,
  convert 1.2 to 3.2 ms (the worst 14 ms), the frame 0.5 to 0.6 ms, no upload. The game went on to the intro mission's
  bar. At the intro's end FFmpeg's AAC decoder warned "Could not update timestamps for discarded samples" (the
  stream's last packet's padding): nothing missing.

## 2026-10-21: the ragdoll's toolkit, its first two blocks (item 32a, 32b)

- **Asked** (the owner): the ragdoll becomes a loader mod in a private repository of its own; first the loader gets
  every service it needs. What mafia-ragdoll does with raw hooks (hits.cpp, knock.cpp, core.cpp's pedestrians) is
  ported as host services for any mod, both builds (ROADMAP item 32).
- **Made** (32a): hit_filter (every hit a C_human or a traffic pedestrian takes, before the game: C_human::Hit,
  pedHit, and a bullet on a pedestrian at the pedestrian system's own Hit, before it wakes the element), human_hit (the
  game's own Hit: a person's vtable +0x7C, C_human::Hit in both builds; a pedestrian's pedHit), pedestrians_list
  (host/pedestrians.cpp: the managers' arrays and the game's list), MmlHumanHit's part, taken, shape. The bridge's
  own filter `hits.take` lets an agent test what a mod's filter would do. (32b) bullet_shape: a person's body as
  capsules for the bullets (collRayTest hooked outermost for the bullets' call alone, the exclusion list and a repeat
  guard as mafia-ragdoll's, the point given on the capsule's axis, never past the game's own hit); `hits.shape`.
- **Found**: game.h SetLocalPosition did not mark the frame changed (0x40000000, as LS3D's SetWorldPos does), so a
  bone moved by its position alone left its children (a corpse's pelvis moved, its legs not): fixed in game.h and the
  Rust crate. The bullets' tick asks the part test again with the length to the point given: the capsule's skin put
  the point before the part and the game dropped the hit; the axis point is taken now. A dead C_human's collision
  follows his bones (the game strikes a moved corpse itself); a dead pedestrian's is out of the grid (pedHit's
  collGridRemove), and 1.2's pedBulletHit goes no further for one (1.0's calls pedHit): the host tells the bullet
  itself, so both builds tell it (knowledge/notes/game.md).
- **Seen**, both labs: a pedestrian knocked down by human_hit (type 5, 20) with no health taken; a person knocked
  down (type 8 at 5 m/s: "smrt pad vpravo"); a lethal hit taken by hits.take leaves either alive, told `taken`;
  tests/game/test_hits.py (knock_down, hit_taken, bullets_on_moved_body: the pelvis takes its children 2.5 m, the
  bullets strike the moved body) 14 of 14 in each lab with world.car_hits_person; the player's bullets on a dead
  pedestrian's capsule: PEDESTRIAN_HIT with its shape in both builds, with and without a filter taking it.
- **Made** (32c): frame_watch (host/framelife.cpp): LS3D's frames' and meshes' shared Release hooked once something is
  watched; a Release that finds the count at 1 calls the frame's watchers before its memory goes (mafia-ragdoll kept
  the freed ones for a purge at its next tick). `frame.watch` records FRAME_RELEASED for agents; `frames.watched`.
- **Seen** (both labs): a prop's frame watched, prop.remove: FRAME_RELEASED at once. A spawned person's model after
  world.remove keeps one reference, not freed (ROADMAP Known gaps). 32f: a corpse posed 8 s after his death (the
  pelvis 1.2 m, an arm turned): the skin follows in both builds (screenshots); a dead pedestrian's element and model
  stayed 120 s beside the player, the element not given to a new pedestrian.
- **Fixed on the way**: run_tests.py's 20 reloads of boxes asked the process's threads per reload to be 0; a thread
  of the process's pool ending meanwhile made it -0.05 (44 to 43) twice in a row: a mod's thread left behind makes it
  positive, so it is `<= 0` now. And a lab install during the offline tests had taken new_mod.py's test mods from
  the build's output into both labs (taken out): the labs are installed with no build or test running.
- **Made** (32d): a traffic pedestrian's element is taken by human_skeleton, bone_set's poses (its model updated
  after RENDER_BEGIN as a person's), human_get_up and human_anim_state (humans.cpp's Posable: a person or an element;
  HumanKnown stays the C_humans'). Its get-up is mafia-ragdoll's way: put where the body lies and turned as a person
  is, its model placed by its own SetWorldPos and SetDir and updated, out of the wait by a car it left, its collision
  object at the body back into the grid (collGridMove, else collGridAdd), its sequence at the game's get-up (111 from
  the back, 338 from the front), pedAnimStart. The bridge's human.skeleton, human.pose, human.anim_state and
  human.get_up take `pedestrian`.
- **Seen** (both labs, a scratch script beside tests/game: the tests' start has no pedestrians): a pedestrian's 20
  bones read; knocked down by human_hit (type 5, 20), its pelvis posed 1.5 m away (its head with it), handed back:
  the get-up (111 or 338) played in its sequence where the body lay, then walking again (state 0) 0.87 m on, alive;
  7 of 7 in each build.
- **Made** (32e): draw_world (host/worlddraw.cpp): lines, boxes (turned by their axes), spheres (three great circles)
  and capsules drawn as lines over the 3D world at its stage 1 after the mods' RENDER_WORLD, depth tested without
  writing it, or over everything (MML_DRAW_ON_TOP); 0 ms for one frame, else held. Drawn on the game's Direct3D 9
  device with every state it needs set and the game's state block applied after (render.cpp's Keep, PutBack), so
  LS3D's record of the device stays true; LS3D's own line and box drawing (ls3d+16B20, +186A0) was read and not used (it
  writes that record and draws with the last object's world matrix). `draw.world`, `draw.world_state`.
- **Seen** (both labs, the same picture): a capsule, two spheres and a box half hidden by the taxi and the kerb, the
  sphere behind the taxi hidden, the one on top over it; a 0 ms shape drawn once and gone.


## 2026-10-22: what a ragdoll mod costs the loader

- **Measured** (`profile` in both labs, the ragdoll mod with six corpses and a dozen people near): the mod 0.84 ms a
  frame, most of it the loader's own work for it. bone_set (twenty bones a ragdoll, twice a frame) asked each frame's
  people and pedestrians for their models through ModelOf, which asked PedestrianKnown, which gathered every
  pedestrian again: a hundred gatherings of the actor list a frame; and each bone was VirtualQuery'd first. The
  models are read once a frame now with their owners (a person's +0x68, an element's +0x40), a bone of a known model
  is trusted (its parents read under a guard) and only another frame is asked of the memory. The scene meshes'
  walk (25 000 frames of the city, 2.3 ms over three or four frames) started over as soon as it ended, while only the
  scans every 250 ms read it: 0.6 ms a frame for nothing. A walk starts when a scan is due now and that scan waits
  for it; a focus that grew is scanned at once from the walk before.
- **Made**: MCP `profile {focus}`: every function whose name holds the text (a mod's DLL, a class), past the top
  lists' cut of 60 and 40 that left out all but a mod's heaviest four.
- **Seen**: the mod 0.30 ms a frame in both builds (its own tracking of the people near, the Jolt step, the hits),
  the loader's part of it out of the profile.
- **Fixed** (found while testing the ragdoll mod's dead driver): `vehicle.move` on a parked car moved nothing and
  said `moving: 1`. A car calm for a second is asleep (its flags' awake bit clear) and returns from C_Vehicle::Tick
  at its rest test before Engine, where the hook offers the steps: none came. The method wakes the car as Tick does
  (awake, not calm, its rest counters started again: host/control.cpp WakeVehicle). Seen in the 1.2 lab: a
  Bolt-Thrower spawned and left 3 s, moved 22 m by `vehicle.move`.
- **Fixed**: human_get_up handed back someone seated in a car: put on the street under the car (his position the
  ground there, the get-up queued) while still in his seat; the ragdoll mod had made a crash's driver a knock-down
  and got him up so, and he died there. It refuses one seated in a car now (the car he uses, +0x98, with his model
  under the car's), as it refuses the dead.

## 2026-10-22: a player's DLSS 5 crash reproduced: Direct3D 9Ex keeps every texture twice

- **Asked** (the owner): the friend's game with the DLSS 5 pack still crashes, with no dump; a lab of its own for his
  build (W:/MafiaDLSS: his pack from W:/Downloads/Mafia.zip, the game's archives it lacks, the dlss_nr pack of
  2026-10-21, his ReShade 6.8.0 as a Vulkan layer for that process only: `_launch_reshade.py` there), then the
  texture pack he uses (his own Project Upscaled 3.0 from moddb, five of its extras, three fixes; each replaced
  picture's presaved .DX copies removed).
- **Seen**: the freeride's load (freeitaly) dies at LS3DF+0x79923 (ITexture::Create's 16 MB allocation null) with 10 MB
  of the 4 GB address space left; the player: "Freeride Daytime crashes to Windows or freezes; without DLSS 5 it does
  not". Measured from outside (VirtualQueryEx at its lowest): 3.5 GB of private memory; with dlss_nr off the same
  (3.5 GB, the same crash); with dxwrapper's `D3d9to9Ex = 0` 578 MB, the freeride loaded, 2.9 GB free. 9Ex has no
  managed pool: the textures are kept a second time in the process. The pack's dxwrapper.ini turns 9Ex on for the
  bridge, which refuses a device that is not 9Ex underneath, DXVK's too. ROADMAP "Known gaps".
- **Seen too**: the crash's dump failed (0 bytes, 0x8007001F: no memory left to write it; the JSON report was
  written), and the 64-bit half (mml-sidecar64) outlived the dead game and kept its process object, so the folder's
  next start quit at once (the "freeze" the player may see is that). ROADMAP "Known gaps".

## 2026-10-22: managed textures in video memory on Direct3D 9Ex (dxwrapper)

- **Asked** (the owner): the doubled textures fixed properly, not by turning 9Ex off (the bridge, the shadow map
  shared with Direct3D 11 and the videos' hardware decoding need it); the cause confirmed by an experiment first.
- **Found** (dxwrapper's sources, d3d9/IDirect3DDevice9Ex.cpp): with D3d9to9Ex every managed texture (and volume,
  cube) became `D3DPOOL_DEFAULT | D3DUSAGE_DYNAMIC`; a dynamic texture is kept where the CPU reaches it, inside the
  game's address space. Counted (a lock counter built into dxwrapper for the experiment): loading the player's freeride
  made 1162 such textures of 3.2 GB (A8R8G8B8 all: 3194 MB, A4R4G4B4 38 MB, no DXT) against 3.5 GB of the process's
  private memory. DYNAMIC dropped alone: the game's locks fail and LS3D writes through null (LS3DF+0x79FA9). Managed
  kept on 9Ex: DXVK refuses to make such a texture. LS3D locks its levels through their surfaces, about ten a texture
  at its load, never READONLY (one lock in a whole run), never DISCARD; some mip chains (A8R8G8B8, A4R4G4B4,
  A1R5G5B5; 12 MB in the 1.2 lab, 277 MB with the pack) are locked again later.
- **Made** (third_party/dxwrapper/MmlManaged.h, patch.py's change 3): a managed 2D texture is a default-pool one
  without DYNAMIC; a level locked gets a copy in system memory (a one-level SYSTEMMEM texture), filled from the video
  memory when the level holds something (StretchRect into a render target, GetRenderTargetData) and sent back at
  the unlock (UpdateSurface of the rectangle written). The copies stay while used, the least recently used let go past
  256 MB (`MML_MANAGED_BUDGET_MB` in the environment changes it); a format the device cannot read back that way
  (DXT1, A4R4G4B4 here) keeps its copies as long as its texture. UpdateSurface, UpdateTexture, StretchRect and
  ColorFill into such a texture mark its copies out of date; GetDC on a level goes through its copy. Volume, cube and
  AUTOGENMIPMAP textures keep the DYNAMIC conversion. dxwrapper's log says each 30 s what it holds.
- **Seen**: the player's build (W:/MafiaDLSS, his 8.4 GB of textures, DXVK, ReShade, 9Ex, dlss_nr on): the freeride
  loads, 610 MB private (3.5 GB before), 2.8 GB free; the world as with 9Ex off. The 1.2 and 1.0 labs (Windows'
  Direct3D 9): the freeride, a drive, the pause menu as before. The read-back tested with a budget of 0 (every lock
  after a let-go reads back): 1461 levels in 1.2, 1462 under DXVK with the pack, no content lost, the pictures right.
  Offline tests: ALL PASSED.
- **Seen on the way** (ROADMAP "Known gaps"): a game on DXVK that crashed hangs in its exit (a thread left waiting
  in DXVK's d3d9 for one ExitProcess killed); dlss_nr on our RTX 3070 in the player's build draws the sky alone.

## 2026-10-22: the bridge keeps its depth buffer against a mod that puts the game's back

- **Asked** (the owner): why dlss_nr in the player's build draws the sky alone on our RTX 3070.
- **Found**: not dlss_nr (NGX refuses the 3070: its pass returns at once) but the graphics bridge around it: gfx.state
  said 2 frames of the bridge's and `not_our_depth` 483. Without ReShade the same; without the player's
  `MafiaWaterShaderMod.asi` the world is back. A tap on dxwrapper's m_IDirect3DDevice9Ex::SetDepthStencilSurface
  (its address from dxwrapper.pdb with cdb) and on d3d8to9's Direct3DDevice8::SetRenderTarget: the water mod calls
  Direct3D 8's SetRenderTarget in pairs around each of its water draws (about 36 a frame), its own depth buffer and
  then the one it kept from its start, the game's: the bridge's readable depth buffer (INTZ), bound before the world,
  was put away, and the world was drawn into the game's own, which nobody had cleared this frame (LS3D cleared the
  bridge's): the sky alone, drawn without depth, was seen. With the bridge off the game's is the bound one: nothing to
  see. The player's "Kill Sam: graphics go crazy" may be this too.
- **Made** (host/gfx.cpp): the device's SetDepthStencilSurface followed (in HookState's chain): while the bridge's
  depth buffer stands for the game's, a set of the game's own binds the bridge's; out of that time a set of the
  bridge's (a mod that kept it meanwhile) binds the game's. The bridge's own sets ordered round the flag.
  `gfx.state` tells `depth_redirects` (PROTOCOL.md).
- **Seen**: the player's build (ReShade, the water mod, dlss_nr on, his textures, the managed textures' dxwrapper):
  the world drawn, the bridge on every frame (1160 frames, not_our_depth 0, 38949 redirects in them), the river's
  reflections the water mod's. The 1.2 and 1.0 labs with the bridge on: the frame right, 0 redirects. Offline tests:
  ALL PASSED.

## 2026-10-22: a late frame no longer turns the graphics bridge off

- **The player's logs** (RTX 5060 Laptop, DXVK, dlss_nr): "DLSS 5 NR: the graphics bridge is off" in the daytime
  freeride. Three runs of mml.log: 5 to 13 s after `freeride` loaded, `gfx: off: Direct3D 9 did not finish the
  frame's copy in time` (twice) and `the textures' copy on DXVK's queue did not finish in time` (once); another run
  of the same freeride kept the bridge 45 minutes. One frame over 250 ms just after the big load (DXVK sending the
  textures up, making shaders) turned the bridge off for the rest of the game.
- **Made** (host/gfx.cpp, host/gfx_vk.cpp): a device late at a hand-over is no failure: the frame goes without the
  passes; while that work still runs (its event query pending, or a copy on DXVK's queue not done: `gfxvk::Busy`)
  the next frames go without too and touch nothing shared; the bridge goes off only after 10 s of late frames in a
  row. A late DXVK copy's command buffer and fence are no longer reset while the GPU holds them (they were: the
  fence reset after a time-out). `gfx.state` `late` (`frames`, `runs`, `now`); the first 10 runs logged.
  `MML_GFX_WAIT_MS` in the game's environment sets the waits (a test of the late frames).
- **Seen**: both labs, Windows' 9Ex and DXVK 3.1.1, the freeride with depth_pulse: as before (`late` 0). 1.2 with
  `MML_GFX_WAIT_MS=0.3` (9Ex) and `2` (DXVK): hundreds of late runs of 1 to 9 frames, the bridge on throughout; 1.0
  with `0` (every frame late): off after 10 s, `(late for 10 s, 1416 frames)`. A copy late on DXVK's queue itself
  was not caught in the lab (the Direct3D 9 wait runs out first). Offline tests: ALL PASSED.
- **Seen too**: the player's `mml\ffmpeg` is missing (`avutil-61.dll`, error 126): the game's own player shows the
  logos; the pack has it.
- **Lab**: as before (DXVK out, depth_pulse off).

## 2026-10-22: the player's upscaled intro through the loader's video player

- **Asked** (the owner): The Renegade's upscaled intro (moddb "Upscaled Intro", `intro_3_starlight_mini.zip`, Topaz
  Video AI's Starlight Mini; its author could not get it into the game: as `.bik` it played at 12 frames a second).
- **The file**: one MKV, HEVC 3200x1680 25 fps, Vorbis 44.1 kHz stereo, 228.9 s, 1.74 GB.
- **Seen**: as `files\intro.mkv` of a files-only mod, `video.play {"name": "intro"}`: 1.2 and 1.0 on Windows'
  9Ex, decoded by D3D11VA, a shared texture: 5722 pictures of 5722, none dropped or late (converting 1.1 to 1.3 ms a
  picture); 1.2 under DXVK 3.1.1, copied through the CPU: 5722 of 5722, none dropped (converting 4.3 ms, at most
  26 ms; uploading 0.8 ms). The picture sharp, its aspect kept (bars above and below at 1280x960). It needs
  `mml\ffmpeg` beside the game, which the player's install lacks (his log: `avutil-61.dll`, error 126).
- **Lab**: as before (the test mod and DXVK out).

## 2026-10-23: the video player's sound in waves

- **Heard** (the owner, the DXVK lab of the player's build): The Renegade's upscaled intro (MKV, Vorbis) broke off
  every block. `timing.sound` added to the player's record (video.state, VIDEO_ENDED) measured two causes:
  Matroska's laced Vorbis packets given times up to 57 ms off (a quarter of the sound dropped as late), and the
  decoding thread stopped at a full queue of pictures before the sound behind them in the file (heard as waves once
  the first was fixed). Both fixed in host/video.cpp (knowledge/notes/videos.md "The sound in waves"): the sound one
  stream, the file read on with video packets queued, the clock started with half a second of sound.
- **Seen**: under DXVK the whole intro, 5722 of 5722 pictures, nothing dropped or put in, heard clean by the owner;
  1.2 (9Ex) MP4 with AAC 4K 60 fps and a silent one, 1.0 the intro: clean. The game's own Bink files were not played
  again (the lab's MP4 copies come first). Offline tests: ALL PASSED.

## 2026-10-23: the size things are drawn at ("Bounce")

- **Asked** (the owner): cars, people, trams pulsing in size to music, their collision and physics unchanged, as the
  GTA and NFS "Bounce" videos have them.
- **Read**: LS3D's frame scale (`+0x90`) and how UpdateMatrix and UpdateWMatrixProc build the matrices from it, the
  frame's hierarchy box (knowledge/notes/scale.md).
- **Seen**: a scale written into the taxi's frame held through driving, but the owner, watching, saw the car drive
  differently: the game reads the frames' matrices back. So the loader scales a model frame's local matrix for the
  world's Render alone and puts it back after (host/scale.cpp; render.cpp around Render). An A/B from one restore
  point (1.2, Accelerate 3 s): -1985.51 -4.805 18.913 at its own size, -1985.67 -4.803 18.916 drawn at 2.
- **Made**: `actor_scale` (mml.h, the Rust crate's `actor_scale`), `actor.scale`, `actors.scaled`; `camera.set
  {follow}` (the owner: "a camera that follows the train?"); `mods/examples/bounce` (cars by the collision's moving
  objects: the parked and traffic cars are not actors; people, the traffic's walkers, trams and trains).
- **Seen in both labs**: the taxi at 2 with its wheels on the road (bottom pivot), a parked traffic car and the taxi
  pulsing at 120 bpm, Paulie at 2 (1.0), a tram stretched 1.6 and pulsing, filmed from its own space (1.2); bounce
  holding 38 (1.2) and 30 (1.0), no faults. Offline tests: ALL PASSED.
- **Lab**: bounce loaded by `mod.load` only (its `enabled` stays false).
- **Asked again** (the owner, watching): Tommy in his car grew with the car and again on his own, larger than the
  car; and the pulse should not be a heartbeat but a smooth squash, tall and narrow to low and wide, eased, never
  longer (people not deeper either). Now a frame inside another scaled one this frame (any parent up to 32 levels)
  is not scaled again, and `MML_PULSE_SQUASH` (x * (1 - a c), y * (1 + a c), z kept; c a cosine of the beat) is
  bounce's default (amount 0.2).
- **Seen**: both labs, bounce's squash: the taxi tall and narrow, then low and wide, its length the same; 30 held, 29
  drawn (the player in the taxi left to it). Offline tests: ALL PASSED.
- **The build** (the owner: "seven minutes for this small mod?"): MSBuild compiled each project's files one by one
  (no `/MP`) and the projects one by one (no `/m`): the host's ~70 files took 7 to 10 minutes after a change to
  mml.h. Now `/MP /FS` in CMakeLists and `/m` in the build presets: 105 s for everything after mml.h, 16 s with no
  change (a cl.exe a core in the Task Manager meanwhile).
- **Seen by the owner**: a two-car elevated train pulsed in its first car alone. Its second car is a model frame of
  its own (C_railway +0xC4, +0x1A8; knowledge/notes/scale.md): actor_scale now scales every car of a train. 1.2: 40
  held, 43 frames drawn (the trains' cars; the player in his taxi left out). Offline tests: ALL PASSED.
- **Asked** (the owner): bounce stays an example, and what it found by hand should be the SDK's. The traffic's cars
  (C_car actors kept out of the game's lists, parked ones too) are now in world_near and world.near (game_build.h
  trafficElements, both builds, check_builds ALL PASSED); mml.h names the types (MML_ACTOR_PLAYER, _RAILWAY,
  _AIRPLANE); scale_beat replaces actor_scale with no actor and a flag. bounce now asks world_near by type and
  pedestrians_list: no collision owners, no classes. Seen: world.near found 11 traffic cars (1.2) and 5 (1.0, the
  traffic still coming); bounce 42 held, 45 drawn (1.2), 19 (1.0), squashing, no faults. Offline tests: ALL PASSED.

## 2026-10-23: shared GPU fences; dlss_nr pipelined, with DLSS Super Resolution

- **Asked** (the owner): the friend's RTX 5060 Laptop gives 62.5 fps without DLSS (the game's limit) and 20 to 30 with
  DLSS 5 NR; make the frame pipelined and add DLSS Super Resolution with its presets (and frame generation after).
- **Why it was slow**: every frame ran the halves one after the other with the CPU waiting at each step (the pass spun
  on its inputs' event query, the half on its fence, the pass on the half's answer): DLSS-NR's 12 to 13 ms at
  2560x1600 were added to the frame whole, and NR ran at the frame's full size.
- **Made (the SDK)**: shared fences between the halves: side.h `fence_create`/`fence_remove` (a Direct3D 12 fence
  made shared, its handle given to the game as the textures' are: sidecar/proto.h kFence, kFenceGone), mml.h
  `gfx_shared_fence` (the bridge opens it as an ID3D11Fence through ID3D11Device5::OpenSharedFence); the Rust crate's
  `gfx_shared_fence`; side_test checks the refusal outside a pass (offline tests).
- **Made (dlss_nr)**: two slots of textures, the fences "ready" and "done" (dlss_nr.h): the pass draws the frame
  before's picture (its GPU waiting for "done"), then this frame's inputs, signals "ready" and writes the view; the
  64-bit half queues the work on the slot's own command buffer (waiting for "ready" N, signalling "done" N, timeline
  semaphores imported from the fences) and answers at once. DLSS Super Resolution (NGX SuperSampling,
  nvngx_dlss.dll pinned by deps.py with Frame Generation's DLL and headers): its modes give the render size (NGX's
  optimal-settings callback); the inputs at that size, jittered (Halton); DLSS-NR at the render size, SR to the frame's.
  Settings: `nr`, `upscale`, `pipeline`, `sr_preset`, `jitter_sign`. pack.py ships nvngx_dlss.dll; the pack's README.
- **Seen** (an RTX 3070: DLSS-NR refused there, SR runs): the player's build under DXVK (SR every mode switched in the
  game's Options, textures made anew each time, no frame late); the jitter's sign measured (above, MODDING.md); 1.2 and
  1.0 on 9Ex: the picture right, every frame with it after the first 30 (the textures made). 1.2 with fps_unlock:
  142.8 fps without, 121.5 SR Quality waited for, 132.6 pipelined. DLSS-NR's own path at the render size is not seen
  (no RTX 50 here): the friend's test.
- **Seen too**: the player's build exits with an access violation in LS3D's drvClose (its index buffers' release, LS3DF+0x5BA73),
  the game's code alone on the stack (after the session's own mode switches); its trees are white with DLSS off as on
  (the 1.2 lab's are green): both to look at.

## 2026-10-23: frames shown between the frames (gfx_show, DLSS-FG); a repeatable pass test; DLSS SR's smear measured

- **Made (the SDK)**: mml.h `gfx_show` (MmlGfxShow): at the game's Present the bridge copies the back buffer (the HUD on
  it) into a shared texture, the one mod that holds the showing draws up to 3 pictures, the bridge shows each by the
  game's own Present spaced over the frames' interval, then the frame (`gfx.state` `show`: gap_ms, gap_error_ms). The
  Rust crate's `gfx_show`. A mod holds it only while it uses it (whoever holds it costs a back buffer copy a frame).
- **Made (dlss_nr)**: DLSS Frame Generation through NGX (nvngx_dlssg.dll; `frame_gen` 2x..4x, MFG up to the GPU's
  MultiFrameCountMax): the back buffer into "fg_color", the shown DLSS frame's slot's depth and motion vectors, the
  camera's clip matrices; its pictures drawn into the bridge's targets; its own thread, channels and fences in the
  64-bit half. `fg_stand_in` blends the frames for a GPU without DLSS-FG (the RTX 3070 here: DLSS-FG itself not seen).
- **Measured** (1.2 lab, its own 61.7 fps limit, the stand-in): the spacing within 0.5 ms of even, but the waits are
  the game thread's: 2x showed 86 a second with the game at 43 fps, 4x 99.5 with the game at 25. To be useful the
  pictures must be shown from another thread (the back buffer kept from the game: next).
- **Made (a test)**: `screenshot.when {actor, at, normal}`: the shot taken in the Present of the frame the actor
  crosses a plane. `tools/pass_test.py`: the owner's scene (the taxi driven past a still camera in the 1.2 lab), a shot
  as it crosses the camera's line of sight, once per mode (off, SR modes, NR, the debug views), the car's crops side
  by side: the car at the same place within 6 cm each run. dlss_nr's debug view 7 shows the motion vectors as DLSS
  gets them; `mv_scale` scales them (a test).
- **Found**: the motion vectors are right in size and sign (the check darkest at x1, the render size's the same), but
  some of the car has none of its own (its front spare wheel and fender) and its shadow none (a decal: it keeps the
  ground's): DLSS SR leaves a ghost behind the moving car (the friend's "smear"), most at Quality where the car itself
  is sharp. The pipeline shows the world one frame behind (the car shifted a frame's travel, not blurred); the test
  turns it off. To fix in the bridge's motion vectors: next.

## 2026-10-24: the bridge's motion vectors whole on a moving car (its cut-out parts, its shadow)

- **Found** (the pass test's motion views, `tools/pass_test.py --modes motion,motion_check`): the holes in the car's
  front were not draws left out but the depth test of PsMotionAll. The wheels and the spare wheel are quads with
  alpha-tested textures; drawn again with the alpha test off, their clear parts gave the objects' target their depth
  over the fender seen through them, and the fender's pixels then failed the comparison with the frame's depth. With
  the comparison taken out the front was whole and the wheels square.
- **Fixed**: the motion draw repeats the draw's own alpha test (`AlphaCut`: the stage-0 texture's alpha against
  ALPHAREF, GREATER and GREATEREQUAL, in the draw's own FVF so its texture coordinates come along; the diffuse alpha the
  stage may modulate by is left out).
- **Found**: a car's shadow is no decal of the car's: it is LS3D's shadow projector, whose own visual cuts the
  receivers' faces under it out and draws them textured with the shadow's picture, through its own m_RenderFGroup
  (the shadow visual's vtable +0x6C: 1.2 LS3DF+0x53EE0, 1.0 +0x51690; its shadow projector at +0x1E0, whose world matrix the
  picture follows) and IGraph's DrawIndexedPrimitive (a dynamic buffer, then the device's). Its entry in Render's list is
  a copy, elsewhere each frame.
- **Made**: that m_RenderFGroup hooked for the motion vectors alone (kind 4; `game_build.h` fgRenderShadow, checked by
  `tools/check_builds.py`): the faces drawn again with the motion of the shadow's projector (a point of them goes back
  through the projector's inverse and its last matrix), cut where the picture's alpha is under 0.2; keyed by the
  entry's sort key. What writes no depth is no longer left out: the frame's own depth test and PsMotionAll's comparison
  keep it where it is what is seen (a shadow on the road, not glass over the street far behind it). `gfx.state`
  motion `shadows`, `shadows_again`.
- **Measured** (the pass test, the same modes before and after): the motion view's car whole (fender, spare wheel,
  bumper), round wheels, the shadow's shape with the car's motion; the check's mean 12.75 -> 11.40, its bright bands
  under the car gone. SR Ultra Performance: the shadow without its doubled edge and trail. 1.0 lab: the hook on, the
  same pictures. The cost: the road under the shadow moves with it in the vectors (its texture faint there).

## 2026-10-24: DLSS SR still when still (the colour filtered at the jittered point)

- **Seen** (the owner, by eye): Ultra Performance shook by its pixels on a still picture. `tools/still_test.py`
  (new: the camera held still, frames in a row compared) measured it: 0.67 mean difference a frame in Ultra
  Performance against 0.16 in Quality and 0.12 without DLSS, the pipeline no matter.
- **Why**: PsPrepare took for each render pixel the frame's pixel under its jittered point. The sample moved to that
  pixel's centre, by another amount in each pixel where the frame's size is no whole multiple of the render size
  (1280 / 427), against the jitter DLSS was told.
- **Fixed**: the colour filtered (bilinear) at the jittered point itself; depth and motion stay the nearest pixel's.
  Ultra Performance 0.67 -> 0.23, the pixels changed by more than 8 from 0.5 % to none; Quality 0.15.

## 2026-10-24: moving objects' motion vectors half a pixel off (Direct3D 9's pixel centres)

- **Seen** (the owner, by eye): under DLSS SR every moving object shook by a pixel a frame at any speed, as the still
  picture had before; a car moved by `vehicle.move` past a still camera did not show it to the eye.
- **Why**: PsMotion took this pixel's place as vpos + 0.5 and where it was through ToScreen, which puts a pixel's
  centre at + 0.5 of Direct3D 11's convention; Direct3D 9 rasterises with the centre at the whole coordinate. Every
  moving object's vector was (-0.5, -0.5) pixels off whatever its speed (still things take the camera's path, not this).
- **Fixed**: the place now through the same mapping, ToScreen(now). `tools/pass_test.py --speed 1.4` (new: a steady
  5 km/h by vehicle.move) measured the check of the car: 7.80 -> 2.59.

## 2026-10-24: the world's jitter (gfx_jitter): DLAA anti-aliases, SR gathers below the game's pixels

- **Seen** (the owner): DLAA's picture was OFF's, the wires as stepped and broken. **Why**: the game drew every frame
  from the pixels' centres and dlss_nr only sampled that frame: in DLAA no sub-pixel sample at all (its jitter was 0),
  in the SR modes nothing finer than the game's own pixels.
- **Made (the SDK)**: mml.h `gfx_jitter` and MmlGfxFrame's `jitter`; the Rust crate's. The bridge offsets every
  perspective projection the game sets while the frame's world is drawn (GfxBeforeWorld .. GfxAfterWorld; the one
  set before taken too: LS3D sets it only when it changes), the scene views, the sun's map and the 2D pass not; it
  gives back the game's own after the world. The frame's view, proj and constants are without it; the motion
  vectors take the last frame's camera through this frame's jitter, so it cancels while the objects are still drawn
  again where the depth buffer has them. `gfx.state` `jitter`.
- **Made (dlss_nr)**: each frame's sample j (render pixels, Halton over 8 x scale^2 phases) split: its whole frame
  pixels where PsPrepare samples, the rest the game's offset; DLSS told jr - jg / scale with the offset the frame was
  really drawn with (f->jitter); the offset let go in a frame without DLSS.
- **Measured** (still_test, 1.2): DLAA 0.02, Quality 0.04, Ultra Performance 0.16 (0.23 before). 1.0: the offset
  applied, DLAA 0.06, Quality 0.14, Ultra 0.25; the slow car's check 1.16. The owner by eye, 1.2: every mode
  "perfect", the wires whole.

## 2026-10-24: deploy no longer switches on the mods that are off

- **Seen**: a whole build deployed into the running 1.2 lab loaded every mod it copied, the ones switched off too
  (boxes, physx_*, flow_smoke...): `deploy` called `mod.reload` for each, and `mod.reload` loads a mod not loaded.
- **Fixed** (the MCP server): only the mods `mods.list` says are loaded are reloaded; the others are named in
  `not_loaded`. tests/mcp_smoke.py deploys a mod that is off into the running stand-in game and checks it stays off.

## 2026-10-24: frame generation shown from the bridge's own thread

- **Made**: while a mod holds gfx_show, the game's Present copies the back buffer (the frame, not yet shown) and the
  mod's pictures into one of two slots and returns at once; a thread of the bridge's shows them, spaced, through a
  swap chain of its own on the game's window (Direct3D 9 lets a swap chain present between another thread's
  BeginScene and EndScene when its back buffer is not the target). The device is made with D3DCREATE_MULTITHREADED
  when a mod has "gfx": true; the thread and its swap chain are let go before a Reset. A full-screen device or DXVK:
  the game's thread shows them as before (`gfx.state` show.thread says which and why). A Windows thread, not a
  std::thread (one left joinable at the process's end terminates it).
- **Measured** (`tools/fg_test.py`, new; the stand-in, 1.2 with fps_unlock): the game 152 fps off, 144 at 2x
  (287 shown a second), 148 at 3x (445), 135 at 4x (540); the spacing within 0.2 ms. Before: 2x took the game to 43,
  4x to 25. 1.0: 131 / 119 / 124 / 134, the same. Both games closed with 2x on: a clean exit, no dump.
- **Not done**: the pictures are shown however fast the game runs (the game's presentation interval, immediate in
  the labs): over the display's refresh most of them tear or are never seen; a cap to the refresh (or the game's
  limit at refresh / (n + 1)) is the next step for FG to be useful.

## 2026-10-24: the friend's RTX 5060 Laptop: DLSS at the frame's size in every mode; the GPU's time growing

- **Seen** (The Renegade's logs, 2560x1600, DXVK, NR on): "NR + Ultra Performance, 2560x1600 to 2560x1600": the
  64-bit half never made the inputs smaller in any mode (no "the inputs at" line after a mode change), so NR ran at
  the frame's size always. In the lab (an RTX 3070, NR not loaded) NGX's optimal settings give 427x320 for Ultra
  Performance at 1280x960. The likeliest reason: with nvngx_dlssnr.dll loaded the capability block's
  DLSSOptimalSettingsCallback answers the frame's size (not checked: no RTX 50 here).
- **Fixed**: NGX's answer is taken only when it is smaller than the frame; else DLSS's own ratio of the mode. Each
  size chosen is logged with its source and NGX's answer, so the next logs say which it was.
- **Seen too**: the GPU's time over a session in the city grew from 15 ms (NR 14, SR 1.1 at DLAA) to 90-100 ms
  (NR 85, SR 10): SR at the same size taking 9 times longer means the GPU itself slowed (a laptop's 8 GB filling up
  as the city's textures load, or its heat), not more work of ours: the lab's bridge cost, measured across seven
  districts, stays the same (0.8 ms, the motion 0.1-1.7 ms, DLSS 1 ms). Frame generation was shown from the game's
  thread there (DXVK: `gfx.state` show.thread), hence its low frame rate. The game also faulted at its exit in
  LS3DF+0x5AC2E (from +0x5BA73, the index buffers' release), as on 2026-10-23: Known gaps.
- **Not a leak** (measured after): Windows' "GPU Process Memory" counters gave the sidecar 13 GB of dedicated memory
  on the 8 GB RTX 3070 after a few hundred mode switches, growing 128 MB a switch that remade the shared textures;
  the adapter's own counters stayed at 2.1 GB through 45 switches and a sidecar restart. A process's counter keeps
  counting a shared texture it made after it is gone, it seems: `tools/vram_test.py` (new) reads the adapter's. The
  friend's slower GPU over a session is then his 8 GB laptop's (a texture pack, NR at the frame's size), not ours.

## 2026-10-24: frame generation on DXVK, and never faster than the display

- **Made**: the showing's own thread on DXVK too (by the time it shows, the pictures are in Direct3D 9 surfaces:
  DXVK makes the bridge's swap chain a Vulkan swap chain of its own on the window, FIFO, beside the game's). The
  swap chain waits for the refresh (D3DPRESENT_INTERVAL_ONE), and the thread keeps the display's period between two
  things shown itself: DXVK's Present returns at once (its own thread shows). The two slots then hold the game at the
  refresh / (pictures + 1). `gfx.state` show.thread_present: the thread's last Present.
- **Measured** (`tools/fg_test.py`, a 199 Hz display): 9Ex 2x the game 100 shown 200, 3x 66 / 199, 4x 49 / 196;
  DXVK 100 / 200, 67 / 202, 51 / 202, the spacing 5 ms within 0.05-0.2; 1.0 (9Ex) 83 / 166, 58 / 175, 44 / 175.
  The owner by eye, DXVK 2x: smooth.

## 2026-10-24: the RTX 5060's next logs: the render size right; the game in exclusive full screen

- **Seen** (The Renegade, the update with the render size's fallback): "NGX answered 2560x1600, result 0x00000001,
  its callback there" in every mode: NGX's optimal settings answer the frame's size with NR loaded; the ratios took
  over (Balanced 1485x928: NR 5 ms instead of 15). He: "now the DLSS working good", Balanced in the free ride,
  Quality in the story. Frame generation still slowed his game: "the showing on the game's thread: a full-screen
  device" (an exclusive full screen has one swap chain alone).
- **Made**: the dlss_nr pack's dxwrapper.ini makes the game's full screen a borderless window over the screen
  (EnableWindowMode, FullscreenWindowMode; the brightness by WindowModeGammaShader): pack.py's `dxwrapper` lines. In
  the lab with them: the showing's thread on, its Present shown. The game still faults at its exit in drvClose.

## 2026-10-24: a gamepad plugged in while the game runs (the first of the game's bugs fixed through the loader)

- **Found** (both builds' code, IJoy.dll through mafia-re's byte-exact decompilation; knowledge/notes/ijoy.md): the
  game opens its DirectInput controllers once, in InitSystem (`joy_devices_init`: CreateJoyBase, then an IJoy each
  into a vector; `input_init_devices`: the controls' own list). With none attached CreateJoyBase fails and the game
  has no base at all. A binding names a joystick by its index, and `input_set_binding` drops a joystick's binding
  whose index is not below the count: a pad absent at the start loses its bindings in the controls (the profile
  keeps them) until a restart. Also: `joy_devices_init` calls through a null IJoy when a CreateJoy fails (a crash
  at the start), and the force feedback manager indexes the vector by a binding's index unchecked (safe only because
  the controls never keep an index past the count). An older note had CreateJoyBase's arguments in the wrong order.
- **Made**: host/joyplug.cpp. The HID device interfaces watched (CM_Register_Notification, registered at the first
  controls' update, not in DllMain; a poll thread where it is missing); on the game thread before the controls'
  update, 750 ms after the devices went quiet and again 3 s later, DirectInput's attached controllers compared with
  the base's; one new (or back and neither pollable nor acquirable) has the game's own functions run again in the
  start's order, the old devices unacquired first, the profile's bindings applied again. An unplugged controller
  changes nothing (no index moves). `joy.state`, `joy.rescan {force}`, `joy.hotplug {on}`, `joy.drop` (the
  joysticks let go as a start without them leaves them, in one frame: a plug-in tested without hardware).
  game_build.h "The joysticks" (checked by tools/check_builds.py in both builds' files).
- **Seen in both labs**: this machine's Keychron keyboards are DirectInput game controllers, so each game had two
  IJoys. `joy.rescan {force}`: opened again, the 60 bindings the same, no fault; a joystick binding put in the
  profile (slot 50, joystick 1 button 0), `joy.drop` (opened 0, the slot dropped to none, as at a start without
  them), `joy.rescan` "new: Keychron K4 HE" (opened 2, the slot a joystick's again); the cars driven after it.
  A first try of the drop by separate `native.call`s crashed 1.2 in C_player::AI: each call is a frame of its own,
  and the controls had no bindings in between; `joy.drop` does it in one call.
- **Not seen**: a real pad plugged in (the notification's path), the poll thread (Windows 7, Wine): the owner's test.

## 2026-10-24: DLSS as designed: the world at the render size, the devices ordered on the GPU

- **Seen** (The Renegade's logs, RTX 5060 laptop, 2560x1600, DXVK): "always 30 fps" in every mode and setting, though
  DLSS's own GPU time was 7 to 12 ms (dlss_nr's minute lines: 1681 to 2223 frames a minute in the free ride; the menus
  60). Two reasons in the bridge, not the GPU: (1) the game drew its whole frame at 2560x1600 and dlss_nr sampled it
  at the render size, so Super Resolution saved nothing of the game's own drawing; (2) at each hand-over the game's
  thread waited for the device on the CPU (Wait9, a fence-waited copy on DXVK's queue, Wait11 behind DLSS's "done" of
  the frame before, the copy back, and again at the showing): the CPU and the GPU took turns, a frame cost their sum.
  The lab's profile (RTX 3070, 1280x960, 9Ex, 141 fps): Wait9 + Wait11 10.7 % of the game thread.
- **Made (the bridge, host/gfx_vk.cpp)**: on DXVK two fences of Direct3D 11 (11.4, shared NT handles) imported into
  DXVK's Vulkan device as timeline semaphores (VK_KHR_external_semaphore_win32, which DXVK enables where it can):
  To11's submission signals "to 11" after its copies and Direct3D 11's context waits for it; Direct3D 11 signals
  "to 9" after the passes and To9's submission waits for it before its copies (a fill of a small buffer anchors the
  wait, the barrier after it holds DXVK's next commands). The order on DXVK's queue: FlushRenderingCommands, then
  LockSubmissionQueue, which (DXVK 3.1.1's DxvkDevice::lockSubmission) synchronises its submission thread first, so a
  submission of ours comes after everything recorded before it. The command buffers go round a ring of 8. No CPU wait
  for either device on DXVK (`gfx.state` timing.sync, d3d9); 9Ex keeps its event queries (no fence there).
- **Made (the SDK)**: mml.h `gfx_upscale(mod, w, h, fn, user)` and MmlGfxFrame's `render_width`, `render_height`;
  the Rust crate's `gfx_upscale`, `GfxFrame::render_size`, `jitter`. From the world's start to stage 1 every viewport
  the game sets on the frame's target (and the one a SetRenderTarget gives: LS3D sets its own only when its rectangle
  changes) is scaled to the render size; the game's own is put back at stage 1, so LS3D's near pass, the lens flares
  and the 2D pass stay at the frame's size; the jitter (in the render size's pixels now) is taken off there too. The
  depth and the motion vectors are made at the render size (bridge9.hlsl's g_drawn, c3); the upscaler's function runs
  first, the bridge stretches the smaller picture when it returns 0 (or a frame goes without the passes); the passes
  after it get the frame brought up to its size. Every sampler's mip bias is the game's plus log2(render / frame)
  while the world is drawn smaller (DLSS's guide; -1.58 in Ultra Performance).
- **Made (dlss_nr)**: the upscaler while DLSS is on, at the render size the 64-bit half answered for the frame's size
  and mode (the frame's own until it has); the inputs read one to one, the whole Halton sample in the projection, DLSS
  told -jg. The debug views of the inputs show them at the render size (they ask the half for its size).
- **Made (the frame's times)**: Direct3D 9 timestamps from a frame's first drawing to its Present, the game thread's
  waits and its time in Present and handing over: `gfx.state` timing, and a line in the log once a minute ("the game
  thread waited X ms of it for the devices ... the GPU: the frame Y ms"), so the next player's log says what holds the
  frame. `gfx.state {"mip_bias": x}` tries another bias.
- **Seen by the owner**: textures at a grazing angle blurred, much worse in Ultra Performance. Without DLSS: the game
  asks for trilinear filtering only: the loader's dxwrapper.ini now has `AnisotropicFiltering = 1` (16x, dxwrapper's
  own; its log "Setting Anisotropic Filtering at 16x"). With DLSS: the mips were chosen for the render size: the bias
  above. The parking lot and the pavement at a grazing angle, Ultra Performance, bias 0 against -1.58: the slabs'
  joints and the facades sharp again.
- **Measured** (1.2 lab, RTX 3070, 1280x960): on 9Ex the lab is CPU-bound (5.5 ms a frame, the GPU 1 to 2.3 ms): 181
  fps off, 180 DLAA, 164 Quality, 176 Performance, 184 Ultra; the world drawn at 853x640, 640x480, 427x320. On DXVK
  (3.1.1): the waits 0, the hand-overs 0.7 to 1.6 ms a frame of the game thread (DXVK's command stream flushed), 95 fps
  (DXVK's own CPU cost here: 14 % of the game thread in ucrtbase's copies under LS3D's drawing). The moving taxi
  (`tools/pass_test.py --modes motion_check_dlaa,motion_check_perf,sr_dlaa,sr_perf --speed 1.4`): the checks black but
  the spinning wheels, SR Performance without a trail. Frame generation's stand-in on DXVK (`tools/fg_test.py`, a
  200 Hz display): 2x the game 100.7 shown 201, 4x 50.2 / 201, the spacing within 0.08 ms. 1.0 lab (9Ex): the world at
  427x320 in Ultra Performance, the picture whole, the bias applied. A player's run in the 1.2 lab on DXVK with DLAA
  exited cleanly.
- **The pack**: dlss_nr's ships DXVK 3.1.1's d3d9.dll (its license beside), the README says why and what to send.

## 2026-10-24: Mafia Fusion Fix and the dlss_nr pack in one layout

- **Asked** (The Renegade): a version that works with Mafia Fusion Fix (ThirteenAG; its source in WidescreenFixesPack,
  `source/Mafia.FusionFix`), which ships Ultimate ASI Loader as d3d8.dll with crosire's d3d8to9 inside
  (`d3d8.ini` `[GlobalSets] UseD3D8to9=1`) and its .asi in scripts\; with the pack's dxwrapper as d3d8.dll the two
  collide (two ASI loaders, two d3d8.dll). And: "the game opens in a quarter of the screen" after a resolution or a
  borderless switch.
- **Read** (its sources): it reaches Direct3D 9 by QueryInterface(IDirect3DDevice9) on the game's device (any d3d8to9
  answers), mid-hooks LS3DF by patterns (CreateWindowExA for its borderless window, centred at the game's size;
  GetSystemMetrics answered 0; the shadows' ZBIAS; the camera's FOV), rw_data's archive lookup (its eye animations: a
  mid-hook inside the function whose entry the loader hooks), the registry (WriteSettingsToFile: savegame\settings.bin,
  written by Setup.exe, which loads the same d3d8.dll: without that file the game opens Setup.exe instead of starting;
  the lab did so once).
- **Made**: the bridge takes DXVK's plain Direct3D 9 (d3d8to9 makes no 9Ex; DXVK answers IDirect3DDevice9Ex only for
  an extended device), the adapter's LUID from DXVK's Vulkan device; the loader stays out of Setup.exe (no log, no
  hooks: it would rotate the game's log away); it makes the process DPI aware itself (dxwrapper did: a window on a
  scaled screen); it draws the game's gamma ramp (the brightness slider) over the frame in a window on DXVK, which
  applies a ramp in a full screen only (host/gamma.cpp, bridge9.hlsl PsGamma; `gfx.state` gamma; seen with
  IGraph::SetGammaRamp called through native.call: the frame brighter, then back). The dlss_nr pack: Ultimate ASI
  Loader 10.0.0 as d3d8.dll with d3d8.ini (`tools/deps.py ual10`; the same as Fusion Fix's: either may overwrite the
  other), DXVK's d3d9.dll with dxvk.conf (`d3d9.samplerAnisotropy = 16`), no dxwrapper, no dinput8.dll; the README:
  Setup's Fullscreen off at the screen's resolution (frame generation needs a window; DLSS makes the render smaller
  itself), Fusion Fix's settings.bin and draw distance.
- **Seen**: 1.2 and 1.0 labs in that layout with Fusion Fix in scripts\: the bridge on DXVK's plain device, the devices
  ordered on the GPU, DLSS SR's render size and mip bias, Fusion Fix's 800 m draw distance; Setup.exe started with the
  loader beside it: mml.log untouched. The labs put back in their dxwrapper layout after.
- **Seen by the owner**: with Fusion Fix the game darker and harsher, the loading screen too. My first answer (its
  draw distance moving the fog away) was a guess, and wrong. Measured (1.2 lab, DLSS off, the world still, two fixed
  cameras, the frame's luminance): without Fusion Fix (dxwrapper, or UAL + DXVK alike) the mean 85.8 / 66.4, the 5th
  percentile 21 / 19; Fusion Fix with the tester's ini 73.3 / 52.4, 9 / 7; its draw distance off (the fog back at
  84..280 m) the same 73.4 / 52.5; ConsoleGamma 0 (the tester's ini had 1, its default is 0) 86.5 / 66.5, 22 / 19:
  the game's own. ConsoleGamma, Fusion Fix's "Xbox 360" curve drawn at LS3D's render's end, is all of it; the pack's
  README says so.

## 2026-10-25: Mafia Fusion Fix as a mod of the loader

- **Asked** (the owner, after the pack's layout change broke the tester's other mods): understand from its sources
  what Fusion Fix does and build it as a mod of the loader, first the SDK's mid-function hooks (8621d75, 7406eb6),
  then its modules one by one, checked in both labs.
- **Read**: WidescreenFixesPack `source/Mafia.FusionFix` at d35057d (2026-10-04; our copy the same file for file):
  185 byte patterns (`tools/check_sites.py`: 184 found in the labs' files, 1 in neither build), found at run time
  (Hooking.Patterns: every match, the first of a list of alternatives that matches), `injector::WriteMemory` (69
  patches in 1.2, 85 in 1.0), safetyhook mid and inline hooks, IAT entries. Most of it is the HUD's and the menus'
  800-unit constants made 600 x aspect; the FOV's half-angle multiplier made Hor+ at each projection's site; the
  rest restores what the patches took out or adds the Xbox's radar and a free camera.
- **Made**: `mods/fusion_fix`: its modules kept as written (`widescreen.cpp`, `ls3df.cpp`, `cutscene.cpp`, `ui.cpp`,
  `camera.cpp`, `restored.cpp`, `ijoy.cpp`) against `ff.h`. Its patterns are looked for in each module's file laid
  out as in memory (the loader's own hook on the HUD's DrawAll covers the first bytes of its radar's pattern), the
  module's memory when the file finds nothing (a packed exe). Fusion Fix reads the resolution with a hook in
  InitSystem; the loader's mods come after it, so the mod reads the same globals at once and calls the game's
  `G_IndicatorsClass::ResolutionChanged` again (`game_build.h` hudResolutionChanged, both builds) to lay the HUD out
  from the patched constants, and again from the game's own when it unloads. Its ini is the mod's settings (Options
  > Mods); a change takes every patch and hook back and makes them again. Not ported: its settings.bin in place of
  the registry, ConsoleGamma and SMAA (ROADMAP Known gaps).
- **Found in Fusion Fix, fixed in the port**: (1) 1.0's camera update has a C++ exception frame and its pattern
  finds the code after the prologue; Fusion Fix hooks that place as the function's entry: the game left silently at
  its first camera frame (seen: 1.0 exited during the start with every part on, ran with the camera part off; the
  disassembly at 0x5D4DD0, the twin of 1.2's 0x5ED4C0). The hook goes on the prologue. (2) Its free camera reads the
  car's speed at `car + 1548`, 1.2's C_Vehicle speed; 1.0's is elsewhere (game_build.h vehicle + vehSpeed). (3) Its
  z-bias hook reads SetRenderState's arguments from esp at each site; one site is a tail jump (LS3DF 1.2
  +0x6D12D), where they are past the return address (read in the disassembly; its deduce_args reads from esp + 0).
- **Measured**: the mod's first Apply took 1.4..1.7 s: 1 ms its patches, 343 ms its patterns, 1234 ms its 31 hooks.
  MinHook freezes the process's threads at each hook enabled, and finds them with CreateToolhelp32Snapshot, every
  thread of the system. `third_party/minhook/src/hook.c` now walks the process's own with NtGetNextThread (the
  snapshot the fallback): 15 ms for the same 31 hooks, the whole Apply 336 ms (1.2) and 192 ms (1.0), a settings
  change 90 ms; the loader's own start hooks faster with it too.
- **Seen**, both labs at once, 1.2 at 1920x1080 and 1.0 at 2560x1080 (`game_launch {"resolution"}`, new: the
  game's `LS3D_setup` read answered with that size, the registry left alone): every part on in both builds; the HUD
  and the radar round and in the screen's corners, the pause menu at 4:3 proportions in the middle, the radar's city
  map, the free camera turned by the mouse; the FOV Hor+ (the camera's cached projection x 1.0711 at 16:9, Fusion
  Fix's formula), and with the mod unloaded the game's Vert- picture and stretched HUD back (after the camera's
  SetFOV is called again: the projection stayed Fusion Fix's until then, which first made the A/B look the same).
- **Found in the loader**: the mods' 2D space followed the HUD's own units, so with Fusion Fix x 400 was left of the
  middle and 800 short of the right edge (seen). host/draw.cpp's ModSpace keeps 800 x 600 over the HUD's area
  (`(width - 2 * hudXOffset) / 800`): the same as before without Fusion Fix, the screen's middle and edge with it
  (seen in both labs), the texts square in either.
- **The tester's frame generation** (his 2026-10-24 log, read now): 800x600 full screen (Fusion Fix's empty
  settings.bin), "the showing on the game's thread: a full-screen device": the generated pictures presented on the
  game's thread one after another. His 20 fps and lag fit that; not measured here yet (ROADMAP Known gaps).

## 2026-10-25: frame generation on a full-screen device, measured

- **Asked** (the owner: go on from the handoff): the tester's 20 fps and input lag with frame generation on a
  full-screen device (his log: 800x600 full screen, "the showing on the game's thread: a full-screen device").
- **Read**: Setup's Fullscreen is `LS3D_setup` + 0x15 (both builds: InitSystem turns it into IGraph::Init's flag 2,
  mafia-re's IGRAPH_FULLSCREEN, whose Init makes a topmost window the mode's size and a device with Windowed false;
  1.2 0x647EF9, 1.0 0x6F9541); the labs' registry has 0 (a window). On the game's thread (`ShowPictures`) the pictures
  are spaced over `g_interval`, the time from one of the game's Presents to the next, which holds the waits between
  the pictures themselves: the game's frame becomes its work times (pictures + 1).
- **Made**: `gfx.state {"show_thread": false}` (a test): the game's thread shows gfx_show's pictures, as on a
  full-screen device, in a window.
- **Measured** (1.2 lab, RTX 3070, 1280x960 in a window, 9Ex, dlss_nr Ultra Performance, the stand-in,
  `tools/fg_test.py`): off 140..151 fps; 2x the game 93.0 shown 186.0 from the bridge's thread, 53.1 / 106.2 from the
  game's; 4x 46.3 / 185.0 against 21.5 / 85.8. The tester's 20 fps is this way. Full screen itself was not run here.
- **Asked** (the owner, given the measurement): a borderless window in place of the exclusive full screen; the full
  screen tried on the third monitor.
- **Made**: `host/fullscreen.cpp`: while a mod has "gfx": true, CreateDevice's and Reset's full-screen parameters
  (Direct3D 8's) are made a window's (Windowed, no refresh, the default interval; the mode's back buffer kept); LS3D's
  window loses its frame and topmost, covers the adapter's monitor (IDirect3D8::GetAdapterMonitor), the picture goes
  to the mode's proportions in its middle (Present's destination: the game's and the showing thread's), the bars
  painted black by GDI (LS3D's window erases nothing: WM_ERASEBKGND returns 0). The showing then has its thread.
  `game_launch {"fullscreen", "adapter"}` answer LS3D_setup's +0x15 and +1 for the run (start.cpp), the registry
  left alone; `gfx.state` `fullscreen` says what was done.
- **Seen** (the third monitor, 1920x1080, the window read with PrintWindow): 1.2 at 800x600 and 1.0 at 1024x768 on
  9Ex, the picture 1440x1080 in the middle, black bars, the HUD whole; frame generation's stand-in from the bridge's
  thread: 1.2 2x 99.7 shown 200.0, 4x 49.8 / 199.4; 1.0 2x 96.8 / 193.7, 4x 50.0 / 200.0. The pack's layout (Ultimate
  ASI Loader 10's d3d8to9 over DXVK 3.1.1, swapped into the 1.2 lab and back): the same, 2x 72.4 / 144.8, 4x 36.8 /
  147.4 (the primary monitor's refresh: DXVK's adapter 2 was the primary monitor), the bars black on DXVK too.

## 2026-10-25: frame generation never slows the game

- **Asked** (the owner): frame generation must not lower the game's own frame rate (the game at refresh / 4 with 4x
  felt like 15 fps: the controls, the input lag, the artifacts); the game runs as fast as it can, the generated frames
  only add smoothness.
- **Made** (host/gfx.cpp): the mod is asked for as many pictures as fit between two of the game's frames at the
  display's refresh (ShowFit: the game's own interval over the display's period, minus the frame; up at 4 % to spare,
  down at 2 % short); none fitting, the showing stops after 1 s and the game shows its frames by its own Present. The
  game never waits for the showing: three slots, its frame takes the waiting one's place (`dropped`), a newer frame cuts
  the pictures not yet shown (`cut`). The display's rate is the monitor's mode or the desktop composition's when slower
  (DwmGetCompositionTimingInfo: the right monitor here is 240 Hz, its windows are composed at the 200 Hz primary's).
  The showing went from a Direct3D 9 swap chain of the game's device to a flip-model swap chain of the bridge's
  Direct3D 11 device (FLIP_DISCARD, a frame latency waitable object waited for outside every lock, the context
  ID3D11Multithread-protected) in a child window laid over the game's picture (WS_EX_NOREDIRECTIONBITMAP, HTTRANSPARENT:
  the mouse goes to the game's window); the pictures stay on Direct3D 11 (no copy back to Direct3D 9), and the game's
  device is no longer made D3DCREATE_MULTITHREADED. dlss_nr's stand-in blends only with the frame just before.
- **Measured** (fps_unlock's max_fps caps the game; `fg_caps`/`fg_ab` scripts: the game's own rate with 4x asked):
  the Direct3D 9 swap chain from a second thread held the game's device: on DXVK the game 139..146 -> 114..121 fps with
  no picture at all, 146 with the thread's Direct3D 9 calls left out; on 9Ex its vertical sync inside the device's lock
  88.5 -> 72 (IMMEDIATE: 86.4). With the Direct3D 11 swap chain, DXVK (the pack's layout, 1.2, 1920x1080): capped 90:
  88.7 without, 89.2 with (shown 178.5, 2x fits); 60: 59.4 / 60.0 (179.9, 3x); 45: 45.0 / 45.0 (180.0, 4x); uncapped
  142.1 / 142.9 (none fits). 9Ex 1.2: 60: 59.6 / 60.0 (borderless 1920x1080), 59.8 / 57.2 in a 1280x960 window; 45:
  44.6 / 45.0; 90: 88.7 / 75.6 (9Ex's CPU waits: ROADMAP). 9Ex 1.0 at 800x600: 90: 87.0 / 86.1, 60: 60.1 / 60.0, 45:
  45.0 / 45.0. The child window's picture read with PrintWindow: whole, letterboxed at 800x600; dlss_nr reloaded while
  showing: the showing made again.

## 2026-10-25: one log to send; fps_unlock's settings and what reaches the display

- **Asked** (the owner): a player sends many logs from many places (mml.log, DXVK's Game_d3d9.log in the game's
  folder, five NGX logs in mml\data\dlss_nr, the sidecar's, LS3D's log.txt): one log. And fps_unlock without a page in
  Options > Mods, without the frames shown with frame generation, without a key for its line.
- **Made**: host/gather.cpp: DXVK_LOG_PATH set to mml\logs at the loader's start when the player has not set it (DXVK
  reads it when d3d9.dll loads: after the loader under Ultimate ASI Loader's d3d8to9); a thread reads every other
  log as it grows (mml\logs\*.log, the game's folder's *.log and log.txt, mml\data\<mod>\*.log) into mml.log under its
  file's tag ([dxvk], [ls3d], [nvngx]...), an earlier run's file only from what is added now; once more at the exit
  and in the crash report's thread. The sidecar's own file is left out (its lines are in mml.log). The pack's README
  asks for mml.log alone (and mml\crash). mml.h `gfx_show_stats` (MmlGfxShowStats: the game's frames, the things shown,
  the pictures among them, the pictures that fit, the display's rate; the Rust crate's `gfx_show_stats`). fps_unlock:
  its settings page (mod.json), `show_hotkey` (Shift+F11) switching its line, the line's "shown 180 fps (3x at 200 Hz)"
  with frame generation, `fps_unlock.status`'s shown_fps, pictures_fps, showing_thread, fit, refresh_hz.
- **Seen** (1.2 lab in the pack's DXVK layout): Game_d3d9.log written in mml\logs, 354 [dxvk] lines, 29 [ls3d],
  NGX's [nvngx] lines in mml.log; the FPS page in Options > Mods (panel.state); Shift+F11 by the game's input showed
  "61 fps 16.7 ms (17.2 max) shown 180 fps (3x at 200 Hz)" (capped at 60, the stand-in at 4x); status the same.
- **Heard** (The Renegade, 2026-10-25 14:18): the pack of the day "doesn't run, the setup.exe as well", after he
  removed the old files; he suspects d3d8.dll, d3d8.ini, d3d9.dll. Not reproduced: his logs awaited.

## 2026-10-25: the dlss_nr pack back to dinput8.dll and dxwrapper; the brightness in a window on any Direct3D 9

- **Heard and reproduced** (The Renegade's pack of the day did not start, Setup.exe neither): in a copy of his folder,
  Ultimate ASI Loader as d3d8.dll is loaded by Setup.exe too, so his ASI mods (scripts\: MafiaLostCheats, MSE,
  Patcher, water shader, XboxRainDroplets, libMafiaCountrysideMap2.0) loaded there and showed "exe version isn't
  supported"; Setup never opened, and the game opens Setup when the registry has no settings. Under dinput8.dll
  Setup.exe loads no plugin. (Windows asks administrator rights for any exe named "setup": `__COMPAT_LAYER=
  RunAsInvoker` runs it without them for a test.)
- **Decided** (the owner): the original layout again, no d3d8.dll crutches.
- **Made**: tools/pack.py: Ultimate ASI Loader (9.7.4) as dinput8.dll, our dxwrapper as d3d8.dll over DXVK's
  d3d9.dll and dxvk.conf; a pack's `dxwrapper` is a dict of dxwrapper.ini's keys set in place (`D3d9to9Ex = 0`:
  DXVK's plain Direct3D 9); the update's notes say to delete d3d8.ini; the README's install and uninstall lists.
  host/gamma.cpp: the old rule "dxwrapper present: it draws a window's ramp" was wrong: dxwrapper draws it only with
  WindowModeGammaShader 2, or with it (1 by default) and EnableWindowMode, which the labs and the packs leave off, so
  in a window nobody applied the brightness. Now dxwrapper.ini beside dxwrapper.dll is read (DxwrapperDrawsGamma);
  otherwise the loader draws the ramp on any Direct3D 9, DXVK's or 9Ex, in a window.
- **Seen**: both labs (9Ex, dxwrapper): "gamma: on (Direct3D 9 ...)", IGraph::SetGammaRamp (1.2 ls3d+0x72710, 1.0
  ls3d+0x6EB70, the IGraph* at 0x647EE0 / 0x6F9528) given a brighter ramp through native.call: the frame brighter,
  the identity: back, `gfx.state` gamma "the ramp the identity". The copy of his folder with the update (dxwrapper
  over DXVK): "on (DXVK ...)", the same. Offline tests ALL PASSED. Pack and update rebuilt.
- **Seen in passing**: the trees' leaves white in that copy, also without his scripts\, his maps\ (8932 loose
  textures), DXVK, and on 9Ex: green in the 1.2 lab with the same loader and dxwrapper. His game's own files; left
  (ROADMAP Known gaps).

## 2026-10-25: the intro's cutscene with fps_unlock; three bugs of the game itself (mods/game_fixes)

- **Asked** (the owner, while the tester tests): (1) 1.2 forgets some options after a restart (the level of details
  at the most and the shadows off, Sniper Mode's key), 1.0 does not; (2) the controls' key names broken in a Russian
  layout; (3) with fps_unlock the first seconds of the intro's cutscene (New Game, the video skipped) broken, even
  capped at 60 fps.
- **(3) Found**: C_records::TimeLine tapped at the intro: unlocked, its first step 0 ms (then 56, 83...); the game's
  own pacing 16 (then 41, 83). The limiter's `jae` made `jmp` never waits, so the frame after the load takes no time
  at all; the recording's people stand in their bind pose and the camera is elsewhere until the next keys. **Made**:
  fps_unlock hooks the limiter (chained with the loader's) and asks it for 1 ms at least instead of 16 (it waits only
  for a frame of 0 ms); Present's Sleep(0) stays jumped over; the limiter's jump is only read (another tool's `jmp`:
  left to it, its 0 ms said in the log). **Seen**: 1.2 the first step 1, the bartender then Tommy at the door as with
  the game's own pacing; 1.0 the same moment right (it had the man by the fireplace in his bind pose).
- **(1) Found**: the options are LS3D_setup's (the registry's, as Setup.exe's), copied to the mission's settings at the
  start and back at the menu's Quit: 1.2 reads HKCU and writes HKLM (the VirtualStore held the written one, details
  2.0); 1.0 reads and writes HKLM. Sniper Mode (slot 29, new in 1.2): ProfileLoad tests find_conflict's -1 against 0
  and resets the binding at every load. **Made**: mods/game_fixes (both builds, its fixes per build, game_build.h
  settingsSaveRoot, sniperConflictTest, sniperRTest, tools/check_builds.py): the push of HKLM made HKCU; both tests
  `inc eax; nop`. host/start.cpp also takes Game.exe's RegSetValueExA: a run's `resolution`, `fullscreen`, `adapter`
  are given their registry's values back when the game writes LS3D_setup, so the options are kept and the screen
  stays Setup.exe's. **Seen** (1.2): LOD and shadows set by the game's menu, Quit, HKCU holds details 2.0 and shadows
  0 with 1280x960 (the run asked 1600x900), the next start's menu shows them; X written to the profile and loaded:
  none without the fix, X with it. The owner's HKCU value was put back after. 1.0: the fixes say "not in this build";
  its write to HKLM refused in the lab (5), so the guard was not exercised there (ROADMAP Known gaps).
- **(2) Found**: 1.2's controlsKeyName names keys by DirectInput (the layout's character, U+0446 for W) turned into the
  system's code page; the font draws 1252: "ö û ô â é" for W S A D Q. 1.0 has an English table of its own (read from its
  code: 103 names). Joysticks: IJoy's names in the system's language, both builds ("Îñü Y"). **Made**: game_fixes
  `key_names`, a hook on controlsKeyName: a name kept when the font draws it as it is (in 1.2 a key's taken from
  DirectInput's wide name into the game's code page), else English (1.0's key names, "Y Axis", "Button n", "POV n").
  **Seen**: 1.2 W S A D Q F R X on the player's page, "Joy0 / Y Axis / -" on the car's; 1.0 "Joy0 / Îñü Y / -" with the
  fix off, "Y Axis" with it.
- **Decided** (the owner, the same day): the game's own bugs are fixed always, as the gamepad's hot plug is: no mod,
  no setting. **Made**: host/fixes.cpp (FixesInit after the game's hooks: the two patches through PatchAdd, the
  names through HookAdd, a log line each); mods/game_fixes removed, the pack without it, its README's section and
  the update's notes say the loader does it. **Seen**: both labs log the fixes (1.0 the names alone), 1.2's controls
  W S A D Q, Sniper Mode X after a restart, "Joy0 / Y Axis" in both.

## 2026-10-25: the tester's crashes; the countryside map

- **Asked** (the owner): the tester's game crashed after 1.5 hours (maybe not ours); why. And the countryside map in
  the radar "from Fusion Fix" does not work.
- **Read** (his logcrash.zip: five dumps and reports, mml.log): four at the menu's Quit, LS3DF+0x5AC2E in
  C_IBManager::m_FreeAllIBuffers from IGraph::Close (the known exit crash). One in the freeride at
  C_traffic_car::AI+0x2B8, a null read; crash_triage: the game's own code, the loader in the stack only as the
  mission's tick hook. The dump read (minidump; a 32-bit process's high addresses are sign-extended there): the
  traffic manager's slots +0x74, 25 of them (+0x220), 16 filled; the loop that runs when a policeman asks for a car
  (ai_police_action, the list at 0x63D86C) reads every slot without the null test the other loops have; 1.0 has the
  same code. **Made**: host/fixes.cpp, the loop's `cmp byte ptr [eax], 0; jz next` made a jump to a stub that takes
  an empty slot as an inactive car (game_build.h trafficSlotTest, check_builds.py). **Seen**: the stubs in both labs'
  memory; the crash's condition not made again.
- **Seen in passing**: the loader's own stack reader (host/stack.cpp AfterCallAt) caught access violations at
  0x3FFFFB in his game and the copy of his folder: Game.exe's first page (its headers) had been made executable by
  some tool, so its base 0x400000 on a stack was taken for code and read below. Caught, but dxwrapper logs each as an
  error. **Made**: LiveKind takes a module's first page out of its code.
- **The countryside map**: not Fusion Fix's (its RadarMap draws the city's 0mapar.bmp alone, in our port and in
  ThirteenAG's newest); it is the tester's libMafiaCountrysideMap2.0.asi (fullmap.bmp, the Tab map). It writes a call
  into C_game::Init (0x5A3955) and floats into .rdata without VirtualProtect: both fault in every layout, also with
  the loader taken out and with UAL 10 as d3d8.dll (A/B in the copy of his folder: 0x5A3955 the game's bytes each
  time). A trial with .text made writable by the loader let the call in and the .rdata write still faulted; taken
  out again (ROADMAP Known gaps).

## 2026-10-25: crash dumps of a few MB

- **Asked** (the owner): do the dumps need to be so heavy (the tester's: 0.3 to 2.3 GB, a 922 MB archive to send)?
- **Measured**: what the traffic crash needed from its 2.3 GB: the object in EBX, the array it points to, the slots'
  cars. **Made** (host/crash.cpp): no WithPrivateReadWriteMemory; a MemoryCallback gives windows around the faulting
  thread's registers (-1 KB, +4 KB), around the pointers in them (+1 KB) and in those (+256 B), within committed
  readable regions, 16 MB the most; a ModuleCallback keeps the globals of Game.exe, LS3DF, rw_data, IJoy, the loader
  and the mods only (an NVIDIA driver's were 25 MB of a 43 MB first try). MML_FULL_DUMP=1 writes the heaps whole.
- **Seen** (C_traffic_car::AI's first bytes made `mov eax, [0]` in both labs: ECX the traffic manager): 1.2 5.2 MB,
  1.0 5.3 MB; the manager, its slots and every slot's car read from the dump; crash_triage's verdict and named stack,
  crash_inspect's region, as before. Offline: the stand-in's dumps under 48 MB (a check).

## 2026-10-26: the whole city in one frame (ROADMAP 31a)

- **Asked** (the owner): draw the whole city, without culling, occluders, sectors or a draw distance; far objects
  were still missing, maybe something follows the player's position.
- **Found**: four rules of the game's own leave the far city out (knowledge/notes/game.md "What leaves the far city
  out"): the city's blocks streamed within 200 m of the scene's camera (CacheTick) and each block's objects faded in
  over 700 ms of game time (left at alpha 0 while the time is held: the missing far walls of 2026-10-25); the
  occluders (with them on, from 120 m above the start, near buildings stayed hidden); each visual's last LOD
  distance (GetObjectLOD returns no LOD past it); the game camera's far plane, 600 m in the city, set once at the
  load. The dark ground left in the frame is real: the middle of a block nobody built (the owner), and the void past
  the city's west edge (a ray straight down found nothing).
- **Made**: `render.set {whole_city: true}` (host/render.cpp): CacheTick hooked and held, every block made active by
  the game's ActivateBlock and FinishTimeTransparentObjects called (again after a mission's load), the occluders off,
  GetObjectLOD hooked (a visual past its last LOD's fade start given that start: drawn whole at its last LOD, the
  nearer LODs the game's), the scene camera's far plane held at 5000 m (`far`); `false` gives all of it back
  (the far blocks put away by the game's next tick, the occluders and the far plane as they were). render.state's
  `city` (`blocks`, `active`, `activated`, `lods_kept`, `far`, `game_far`). game_build.h cityCache, cacheTick,
  cacheActivate, missionFinishFades, getObjectLod, rsLodQuality, checked by tools/check_builds.py (ALL PASSED).
  Before: `render.set {occlusion, lod_quality}`, `camera.set {near, far}`, prof's hot sites per visual class.
- **Seen**: 1.2 and 1.0, the camera at (-2100, 120, 0) towards (0, 0, 300): the whole city to the hills, the same in
  both; given back: 85 (1.2) and 60 (1.0) blocks active, far 600, occluders on; a mission reloaded (restore) while
  held: the new cache's 328 blocks active, far 5000. Offline tests ALL PASSED.
- **Measured** (1.2 alone, the lab with dlss_nr and fusion_fix on: not a baseline): that view 16.7 ms with the game's
  own (capped at 60), 91 ms whole (11 fps); the process 1.03 GB of address space, 548 MB private, the same either
  way (the blocks' instances are pooled). The profile: host/motion.cpp's redraws for dlss_nr's motion vectors about
  17 % of the game thread at 35 000 draws (ROADMAP Known gaps).

## 2026-10-26: the standard scenes (ROADMAP 31a)

- **Made**: the MCP `scene` tool (mcp/src/main.rs): `near` (the street east of the city freeride's start, eye
  (-1985, -2, -4) towards (-1935, -4, -4)), `far` (whole_city, eye (-2100, 120, 0) towards (0, 0, 300)), `night` (the
  street in freeridenoc), `crowd` (48 people of four models, Paulie, Sam, SalMan01K, Tommy, in six rows across the
  street, each sent 45 m east by human.go, the time going); the game started in the scene's freeride with no police,
  traffic or pedestrians when it is elsewhere (`profile`, `fresh`); the camera held an hour, the world still
  (`state.time` 0) but in the crowd, the game's HUD hidden (the freeride's opening hints stay on a still world); an
  earlier scene's camera, whole_city, people and HUD put back first. tests/mcp_smoke.py: the tool listed, an unknown
  scene and a missing profile refused.
- **Seen**: all four in both labs; 1.2's and 1.0's frames of the same scene alike (the street, the night street's
  lamps, the crowd walking).
- **Made** (the same day): `bench {picture: true, threshold}`: after each configuration's warm-up its frame is taken
  raw and the two compared per channel, R G B (mcp/src/shot.rs read_raw, compare: the mean and the worst of 255, the
  pixels over the threshold, default 8, the difference times 8 as a JPEG beside both frames). A unit test of the
  comparison. **Seen** (1.2, the far scene, lod_quality 1 against 100, counters off): mean 0.39 0.31 0.24, worst 227
  (a hoarding's finer texture), 0.51 % of the pixels over 8; 76.1 against 79.8 ms, +3.7 ms (95 % interval 2.2; the
  lab with dlss_nr on: not a baseline).

## 2026-10-26: the baselines of item 31 (ROADMAP 31a)

- **The labs made clean** (the owner's rule: the labs are a workbench): dlss_nr, fusion_fix, gravity_gun, ragdoll
  (and 1.0's devtools) switched off in both labs' mod.json; core and fps_unlock (`max_fps` 0, its line hidden) left.
  One lab running at a time; each scene by the MCP `scene` tool, then `bench` A/A' (2 blocks of 4 segments of 100
  frames, the counters on) and `profile` (3 s). RTX 3070, 1280x960 window, dxwrapper (Direct3D 8 on 9Ex).

  | scene | build | frame ms | game thread CPU | GPU | draws | triangles | locks (ms) | driver under draws | noise |
  |---|---|---|---|---|---|---|---|---|---|
  | near | 1.2 | 1.41 | 1.22 | 0.70 | 444 | 4 974 | 36 (0.30) | 0.19 | 0.02 |
  | near | 1.0 | 1.38 | 1.22 | 0.59 | 444 | 4 974 | 36 (0.19) | 0.19 | 0.08 |
  | night | 1.2 | 1.65 | 1.43 | 0.75 | 462 | 5 389 | 36 (0.19) | 0.20 | 0.06 |
  | night | 1.0 | 1.83 | 1.62 | 0.77 | 476 | 5 580 | 36 (0.26) | 0.20 | 0.23 |
  | crowd | 1.2 | 6.42 | 5.79 | 1.44 | 684 | 26 965 | 69 (0.74) | 0.29 | 0.10 |
  | crowd | 1.0 | 7.74 | 6.24 | 2.08 | 682 | 25 608 | 66 (2.12) | 0.29 | 0.39 |
  | far | 1.2 | 39.5 | 38.0 | 32.1 | 34 870 | 416 732 | 221 (2.63) | 12.4 | 0.37 |
  | far | 1.0 | 40.3 | 39.1 | 33.2 | 34 738 | 416 315 | 224 (2.38) | 12.6 | 0.43 |

  Where the game thread goes (`profile`): near and night, LS3D's visibility (SetVisibleRecMainPass 15 %, 1.0's
  CTP_EnumProcFirst 16 %) and the occluders (`Occlusion` 15 to 20 %); the crowd, the people's skinning on the CPU
  (hot_ms: visuals.singlemesh 1.0 to 1.1 ms, skin.update_lod 0.4, skin.lighting 0.3, skin.copy 0.2: about 2 ms of
  6.4), I3D_frame::UpdateChildren 5 %; far, the draws themselves (m_RenderFGroup 57 % in total, d3d9.dll 35 % self,
  the driver 12.4 ms under 35 000 draws, SetTransform 3.7 ms), visibility 9 %, the vertex locks 2.5 ms. So far is
  draw-bound (31g first there), the crowd skinning-bound (31d), near and night visibility-bound (31h).
- **The reference shaders on every draw against the game's own** (`bench` A: `shader.remove {all}`, B: that and
  `shader.add` reference.hlsl, `picture: true`): near 1.2 1.52 to 5.20 ms (+3.6 ± 1.0), 1.0 1.66 to 6.04 (+4.4);
  far 1.2 42.7 to 301 ms (+258), 1.0 42.9 to 303 (+260 ± 0.1). The game thread's CPU carries it (41.6 to 299 ms):
  the price of today's way, the number 31b and 31c must beat.
- **Found and fixed** (the pictures): the reference drew the far scene's untextured ground twice too bright (the game
  (28, 19, 14), the reference (56, 38, 28); `shader.survey`: 250 draws `modulate2x(texture, diffuse)` with no texture
  bound, which the game's fixed pipeline gives as the vertex colour). game.hlsli's mml_FixedStage took a stage with no
  texture as white; now it passes what came before. Far: the mean difference 0.99 0.72 0.55 to 0.23 0.20 0.16 of 255,
  the pixels over 8 from 3.74 % to 0.87 % (both builds alike); near 0.05, 0.09 %. What is left: thin edges (the
  vertices transformed in the shader, the depth at near 0.05 and far 5000) and the bridge's shadow on the water (the
  owner).

## 2026-10-26: the device's state followed, not asked (ROADMAP 31b, first step)

- **Found** (`profile`, 1.2's far scene with the reference on every draw, +258 ms): host/shaders.cpp's `Constants`
  55 % of the game thread: some 60 Get* calls of the device a draw, and 117 registers written whole to both shader
  stages a draw (d3d9's copies in msvcrt 29 %).
- **Made**: host/d3dmirror.cpp: while a shader rule draws, the device's Set* calls are hooked on the game's device's
  vtable and kept (the transforms, every render state, the stages 0..7 and their textures, samplers 0..3, the
  material, lights 0..7, the viewport, the shaders, both stages' constant registers); MultiplyTransform's product
  read back, SetRenderTarget's viewport read back at the next use; a state block's Apply, a Reset, another thread's
  call, another device: read again whole at the next use; nothing kept while a state block records; once a frame
  read again whole (a wrapper's own state set behind the vtable lasts a frame at most). shaders.cpp reads the state
  from it (StateOf: without it, the old Get* calls into a copy), and `MirrorConstants` writes only the registers that
  differ from what the device holds (SSE compares, runs of changed registers one call each). `shader.mirror {check}`
  compares the mirror with the device's own answers draw by draw.
- **Seen**: the check, 300 draws of the far scene in both labs and 500 of the crowd with GPU skinning (1.0): no
  mismatch. The reference on every draw (the game's own drawing against it, `bench`, one lab at a time): 1.2 far
  +258 to +77 ms (41.2 against 117.9), near +3.6 to +1.1; 1.0 far +260 to +76 (41.4 against 117.6), near +4.4 to +1.3;
  the pictures unchanged (far 0.87 % of the pixels over 8). Offline tests ALL PASSED.
- **What is left** (`profile`, far): the compares 15 % (234 registers a draw), the Set* calls into Direct3D some 30 %
  (the shaders set and put back, the constants' runs: d3d9 and its locks), ShadowsConstants 4 %. Next: write only
  the registers the bound shaders read (their CTAB), then 31c (shaders that stay bound).
- **Made** (the same day): a rule's shaders' constant tables read at its making (shaders.cpp ReadsOf: the 'CTAB'
  comment of the code, its float4 records' registers): only the registers each stage's shader reads are written at
  its draws, none for a stage it leaves to the game (the host's skinning shaders: all). No table: all, as before.
  **Seen** (one lab at a time): the reference on every draw, far: 1.2 +77 to +51.7 ms (41.1 against 92.8), 1.0 +76
  to +51.7 (41.3 against 93.0); near: +1.1 to +0.9 in both; the pictures unchanged; the mirror's check 300 draws, no
  mismatch. From +258 ms at the morning's baseline to +52: what is left is the Set* calls themselves (the shaders set
  and put back at every draw: 31c).

## 2026-10-26: shaders that stay bound (ROADMAP 31c)

- **Made** (host/d3dmirror.cpp, shaders.cpp, shadows.cpp, geom.cpp): a rule's shaders lent (MirrorLend): left bound
  after its draw, the game's shaders kept aside and given back only before a draw no rule takes (DrawPrimitive, UP,
  IndexedUP, a DrawIndexedPrimitive without a rule), a Get*Shader, a state block made, begun, captured (none holds a
  lent shader; one applied ends the loan of the stage it sets), Present and the frame's end; a shader the game sets
  ends the loan. While a vertex shader is lent the game's SetTransform of the world, view and projection is kept and
  sent only when it matters (given back, asked, multiplied, a state block). The constants: the groups a rule reads
  decided at its making (Direct3D's lights, the shadow map, the game's lights near), the time once a frame, a
  serial of the mirror's state (all but the world and the textures) so a draw on the same state makes only the world,
  its product and the stages' textures anew and compares only those (when the rule and the device's constants are
  as at the last draw); runs written over short gaps in one call.
- **Seen** (one lab at a time; `shader.mirror {check}` 500 to 800 draws, no mismatch, far, crowd with GPU skinning,
  night, both builds): the far scene's 34 800 draws all through shaders, 12 shader sets a frame, switches 0; the
  driver's time under the draws 12.7 to 7.7 ms; the game's transform calls 3.9 to 1.2 ms. The reference on every draw
  against the game's own drawing: far 1.2 +5.5 ± 0.2 ms (41.0 against 46.6), 1.0 +5.6; near +0.23 / +0.18; night
  1.0 +0.27; the pictures as before (far 0.89 % of the pixels over 8, night 0.05 %). From the morning's +258 ms.
  What is left: 32 400 constants calls a frame (4.9 ms, the world's matrix a draw) and the host's own work a draw.

## 2026-10-26: people skinned on the GPU, on that base (ROADMAP 31d)

- **Measured first** (1.0's crowd scene, one lab): the GPU's skinning against LS3D's on the CPU +1.8 ms without a
  rule, +0.9 with the reference: LS3D's skinning gone (visuals.singlemesh 0.96 to 0.25 ms) but the host's draws of
  people cost more. `profile`: the device asked at every person's draw (no rule: the mirror not on), a dynamic
  texture locked a person a frame for the palette and another for the one before.
- **Made** (host/skin.cpp, shaders.cpp, skin.hlsli, skin9.hlsl): the mirror on while the GPU skins; every person's
  palette a row of one texture (`Atlas`: rows kept by GpuLod, grown by halves), the rows posed this frame gathered at
  its first skinned draw and filled with one lock, again only for a person posed after it (a view's); the one
  before (motion vectors) only when they are asked; skin.hlsli's mml_PaletteTexel reads the row from
  mml_SkinEnv.w. The people's light block (c120..c155) through MirrorConstants; a person no rule takes goes the
  rules' way for game.hlsli's registers (the host's skinning shaders' reads from their code, GetFunction).
  A first version gathered every row at every draw (37 % of a still frame): gathered once a frame now.
- **Seen**: 1.0 and 1.2, the crowd: the people as LS3D draws them (a still crowd, CPU against GPU: 0.13 % of the
  pixels over 8); moving: 1.0 -0.4 ± 1.2 ms, 1.2 -0.1 ± 0.5 (even); still: +0.68 ms (the host's work a person's
  draw, LS3D doing nothing while the bones stand). Offline tests ALL PASSED.

## 2026-10-26: the world's static objects in batches (ROADMAP 31f, 31g)

- **Why**: the far scene is draw-bound (31a): 34 800 draws, LS3D's m_RenderFGroup 57 % of the frame, the driver
  12.4 ms. A static object's group is the same from frame to frame (LS3D lights it only when the lights change), so
  it can be drawn with every group of its state at once.
- **Made** (host/batch.cpp; geom.cpp's hkRenderFg and DrawOn, render.cpp's render.set and render.state, game_hooks.cpp):
  `render.set {batch: true}`. A group of an I3D_object ('OBJ_' at +0x1D4) not yet captured is drawn by LS3D with the
  capture listening: its one DrawIndexedPrimitive read (the pool's vertices with D3DLOCK_NOOVERWRITE: a READONLY lock
  alone waited for the GPU, 0.5 ms a group), put into the world by the device's world matrix, appended to the batch
  of the device's state then (`Sig`: both stages' textures (held by a reference: LS3D frees textures and another takes
  the address, which once drew the sky on a tower), their operations, the alpha test, culling, depth and its bias,
  the fog Render sets an entry from its sector, the samplers). After: LS3D's call for a group whose world matrix,
  LOD record and its stamps, texture (an animated material's frame) and fog are as captured is not made; it is asked
  for. Before the frame's first blended draw and at the end of its world, each batch asked for is drawn once, its
  index buffer the groups asked for (LS3D's own visibility), in LS3D's order of first asking, with D3DRS_CLIPPING on
  (an entry's +0x08 is Render's clipping flag, not a portal's) and every state put back (LS3D's cache of the states).
  Captures cost at most 3 ms a frame; a batch whose dead groups pass half is made again of its live ones. Left to
  LS3D: blended, environment-mapped (a second stage), the backdrop (a view without a place), no depth write or
  test, more than one draw, a moving object (moved twice within 60 frames: a city block's pooled instance moved by
  the streaming once is captured again), and an object any of whose groups is LS3D's (its coplanar layers keep
  LS3D's order). Counters: render.state's batch (`left_last_frame`, `changed`), render.batch_probe {at, radius}.
- **Seen** (one lab at a time, `bench` with pictures): the whole city 1.2 41.2 -> 25.3 ms (34 800 draws -> 6 200 +
  913 batches), 1.0 40.2 -> 24.1 ms; near 1.6 / 1.6, night 1.7 / 1.7, the crowd 6.6 / 6.7 (even: few draws there).
  The picture: 1.2 far mean 0.53 / 0.45 / 0.39 of 255, 1.04 % of the pixels over 8 (the silhouettes' edges), near
  0.03; 1.0 far 0.79, near 0.19 (a texture's noise: ROADMAP Known gaps). Driving with the game's own camera: the
  groups taken (85 to 240 a frame near the ground), no artefact seen. Found on the way and fixed: the sky box batched
  (in the world, cut through the city), environment maps frozen, the fog of the flush's moment, a tower's facade layers
  out of LS3D's order, the people captured each frame (+3.7 ms in the crowd), Render's clipping flag taken for a
  portal's (nothing batched with the game's camera).
- **Then** (the same night): `profile` of the batched far frame: the request's own check 25 % (the members' hash map,
  the fog compared value by value with a call to the mirror each group). Now a flat table (open addressing) finds
  a group's member and the fog is one key made again only when the mirror's state changed: 25.3 to 22.4 ms. The
  picture had gone to 0.79 of 255 once groups flagged for clipping (Render's +0x08) were batched too: an A/B (those
  objects refused again: 0.53) put it on them, the city's ground among them; drawn at the first blended draw, the
  ground covered what LS3D had drawn on it before (a road's marks, decals, shadows). The batches asked for are now
  drawn before every draw LS3D makes itself, which keeps LS3D's order exactly and costs nothing (its list is sorted
  by material: still 922 batch draws). Far 0.27 / 0.23 / 0.19 in both builds (1.03 % of the pixels over 8: edges),
  near 0.04, night 0.015; 1.2 40.1 -> 22.7 ms, 1.0 40.1 -> 22.8. The "1.0 texture noise" written in Known gaps an hour
  earlier was this: taken out.

## 2026-10-26: the request made cheaper, and where block 31 stands (ROADMAP 31e, 31g, 31h)

- **The request's fast path** (host/batch.cpp's Group): a group already captured is looked up first and only what may
  change is checked (the visual's two fades times the material's alpha kept at capture, the world matrix, the LOD
  record and its stamps, the textures, the fog key); the visual's type, the refusals, the material's blending and the
  capture budget are read only for a group not captured. 1.2's far scene (one lab): LS3D 41.7 ms, batched 23.25,
  B - A -18.58 +- 0.49 ms; the picture 0.271 / 0.228 / 0.187 of 255 as before. Not yet seen in 1.0.
- **What the batched far frame still spends** (`profile`, 1.2): LS3D's Render's own loop 15 %, the request (before
  the fast path) about 20 %, I3D_object::m_AddFGroups 6 to 7 %, the visibility (I3D_frame::SetVisibleRecMainPass,
  ls3d+41DF0) about 13 % (3 ms; near 15 %). SetVisibleRecMainPass tests each child against the camera's four planes
  again, whatever its parent's result: an "inside" mask handed down would spare most of the tests (31h).
- **31e weighed**: LS3D's LightVertices are about 0.3 ms of a frame in the night scene: a small share, it waits
  (the roadmap's own rule) unless the batches make it larger.
- **Next, the idea under study**: an object whose groups are all in batches and unchanged need not go through
  m_AddFGroups at all (ls3d 1.2 0x38020, 1.0 0x369E0, game_build.h's addObject): its groups asked for at once. What
  must be kept is the relight m_AddFGroups makes inside: `if (lit flag || lodRecord[+4] != sector[+0x140]) {
  lodRecord[+4] = sector[+0x140]; LightingSetSector(sector); ... }` (the sector at ls3d's DAT_101c4c74 in 1.2), and the
  LOD it picks (GetObjectLOD with the squared distance from the visual's +0x19C to the camera) and the fade. Then
  measure far, near, night and the crowd, one lab at a time, with pictures, in both builds.

## 2026-10-26: the downloads checked by Windows (tools/fetch.py)

- **A player's `cmake --preset x86` stopped at `deps.py ffmpeg`**: `CERTIFICATE_VERIFY_FAILED: certificate has
  expired`. ffmpeg.org's and videolan.org's chains were valid (Let's Encrypt YE1/YE2, Root YE, ISRG Root X2
  cross-signed by X1 until 2032). Reproduced here: Python's ssl given ISRG Root X1 and the old X2 cross-sign (expired
  2025-09-15) fails on ffmpeg.org with exactly that message, while Windows' CertGetCertificateChain given the same old
  cross-sign beside the server's certificates builds a valid chain (status 0), even with the old one alone above
  Root YE. Python on Windows hands OpenSSL every certificate of the ROOT and CA stores as an anchor, expired ones too
  (the CA store is Windows' cache of the intermediates it has seen: one appeared there in this very test), and never
  fetches a missing root.
- **tools/fetch.py**: a download through WinHTTP (ctypes, standard library only), so Windows' own chain engine checks
  the server, as its browsers do: the time-valid path, roots from Windows Update, the machine's own (a proxy's, an
  antivirus') roots, the system's proxy. A refused certificate is said in words from WinHTTP's secure-failure flags
  (out of its dates: the clock, with the time it says; an unknown root: a proxy or an antivirus; another name);
  any status but 200 stops it; redirects followed (GitHub's releases), HTTPS to HTTP never. deps.py and kb_demo.py use
  it. Checked: ffmpeg, dav1d, Ultimate ASI Loader (a GitHub redirect) and its raw licence by their SHA-256, DXVK
  through deps.py; badssl.com's expired, untrusted-root and wrong-host refused with the right reasons; a 404; an
  unknown host. Offline test: fetch_tests (127.0.0.1: 3 MB in 64 KB reads after a redirect, a 404).

## 2026-10-26: old ASI mods' unprotected writes let through (host/asi_compat.cpp); the countryside map

- **Asked** (the owner): make the ASI mods that write Game.exe without VirtualProtect work (libMafiaCountrysideMap
  2.0, the tester's), properly, and see the countryside map with fullmap.bmp in the 1.2 lab.
- **Read** (the plugin's exports and code): a timer from DllMain tells the exe and writes a call into C_game::Init's
  end, the map's transform into .rdata and "fullmap.bmp" over "0mapar.bmp" (knowledge/notes/game.md). Nothing else
  is needed from the loader than letting those writes happen.
- **Made**: host/asi_compat.cpp, a vectored handler first in the process's list: an access violation writing into
  Game.exe's or LS3DF.dll's image, on a committed page that is only not writable, by an instruction of an .asi other
  than the loader, gets that page made writable (execute kept) and the write run again; anything else goes on as
  before. No TLS, no allocation in the handler; each page logged once (a thread without TLS: at the frame). The
  stand-in game loads scripts\*.asi after the loader; tests/harness/writer_asi.cpp writes its .rdata and code from
  DllMain and from a thread without TLS; the harness's own write into its .rdata (`crash_rdata`) still crashes with
  its dump. Offline tests ALL PASSED (517).
- **Seen** (1.2 lab, the plugin and the pack's fullmap.bmp): the three pages logged (.text 0x5A3000, .rdata 0x623000
  and 0x627000), dxwrapper's log without a fault, `hud + 0x46EC` 1 in freekrajina. The radar in the corner showed
  no map, and that is not the plugin's place: the map is the Tab map (DrawAll draws it while flag 0x2000 is on, set
  by C_player::InputDriving while "City Map" is held: a `watch` on the flags, a `tap` on its DrawPrimitive). Held,
  it was drawn and invisible with the pack's fullmap_1.565 (its presave, copied beside the .bmp) as the texture;
  with 0mapar.bmp put back by hand the city map showed, with the .bmp under a new name (no presave) the whole map.
  (Later the same day the same presave showed in the tester's copy, on DXVK: not "another game's presave", but the
  lab's 9Ex path; not isolated, ROADMAP Known gaps.) A clean start with the plugin and fullmap.bmp alone: Tab shows
  the countryside map, the arrow on the
  road north of Chinatown. 1.0 lab: the handler installed, the countryside freeride loaded (the plugin supports 1.2
  only). The plugin and the map taken out of the lab again.
- **The tester's copy** (W:/MafiaDLSS, mml.config.json's new `players`, AGENTS.md): his maps\ has no fullmap.bmp.

## 2026-10-26: the tester's copy made whole (W:/MafiaDLSS)

- **Found**: Project Upscaled 3.0 Final (the tester's own texture pack, moddb) in his copy without 107 of its maps\
  files (4770 the same, 4 its "en textures" variants), among them fullmap.bmp, fullmap_1.565 and the trees' bark,
  birch, stems and needles (kora*, breza, stonka*, konare*, ihl*). **Made**: the 107 copied in (none overwritten),
  the new loader put in (the pack's kept aside). **Seen**: the city freeride's trees green (they were white: the
  missing textures); the plugin's three pages logged, dxwrapper's log without a fault; Tab in the countryside shows
  the whole map, with the pack's presave too (8 s held, six frames). Fusion Fix's RadarMap (on in his copy) now
  draws the full map's texture with the city's transform: the radar shows the wrong place (it opens "0mapar.bmp"
  into the full map's slot and maps x, z with the Xbox's numbers, which are the game's own city transform).

## 2026-10-26: Fusion Fix's radar map follows the game's own map (the countryside on the radar)

- **Asked** (the owner): Fusion Fix's radar with the countryside map plugin in the tester's copy.
- **Made** (mods/fusion_fix/restored.cpp): the radar's map takes the game's own map instead of the Xbox's constants:
  its file name (G_IndicatorsClass::NahrajMapu's `push`, 1.2 0x639310, 1.0 0x64D48C), DrawAll's world x, z to u, v
  (1.2 0x62791C..0x627928, 1.0 0x63D28C..0x63D298) and the Tab map's window (u half, v half, width, height), found
  by patterns once each in both builds (and the tester's exe) and read at every draw, so a plugin that writes them
  later is followed. The radar puts the world where the Tab map puts the player's arrow (the window's middle):
  `u + width / 2 - uHalf`, `v + height / 2 - vHalf`, 0 in the game's own numbers; the plugin's window (0.1833 high,
  0.275 kept) moves v by -0.183. Not all found: the Xbox's constants, as before (logged).
- **Seen**: the tester's copy (W:/MafiaDLSS, the plugin, fullmap.bmp): the log names the game's map; in the
  countryside freeride the radar shows the full map, the road through its middle where Tab's arrow stands (north of
  Chinatown); driven off the road to the right, the radar's mark is right of the red road. 1.2 and 1.0 labs (no
  plugin, the city freeride): the city map in the radar as before, the player on the parking lot by the Shopping
  Center. fusion_fix switched off in the labs again.
- **The tester's update** (`tools/pack.py dlss_nr --since` the archive of 2026-10-25's evening, built from the
  commits alone: batch.cpp's work in progress stashed): the loader, core, dlss_nr, fps_unlock, fusion_fix, the
  README (the old ASI mods let through, the radar's map); 2.0 MB. Run in his copy from the build packed: the
  plugin's pages logged, the radar's countryside map, DLSS SR on the 3070.

## 2026-10-26: the batches in the main menu (asked: what they give there)

- **Measured** (1.2 lab alone, the main menu, `bench` batch off against on, 10 blocks of 600 frames): the frame
  1.20 ms with LS3D's drawing (about 830 fps, 169 draws), 1.28 with the batches: +0.08 +- 0.04 ms, slower. The
  office has 2.5 batches a frame to make, and the batched path asks the device 593 Get* a frame (0.06 ms): a cost
  that pays off only with thousands of draws (the whole city: 40 to 22.7 ms). The pictures differ by the menu
  camera's sway alone. The batches are opt-in: a player's menu is as it was. To look at: the Get* the batched path
  makes (host) where it has little to batch.
- **Where the menu's 1.2 ms go** (1.2 lab alone, the main menu, `bench` counters and `profile` at 0.25 ms): the game
  thread's work 0.76 ms, Present 0.39 ms (0.11 of it on the CPU: the rest waiting), the GPU 0.12 ms. The locks:
  12 a frame, 0.32 ms. The profile (its sampling slows the frame to 3.5 ms, so shares only): half the samples in
  ZwWaitForSingleObject under lod::SetUV+0x108 (ls3d 1.2 0x36D00), its pool vertex buffer's
  `Lock(0, 0, &data, 0)`: the whole buffer, no flags, once a frame (cVBMan's list, unlocked by
  C_VBManager::m_UnlockAllVBuffers), called by I3D_object::m_AddFGroups+0x6E7 for the env-mapped objects
  (EnvMapCoords on the CPU: the office's metal). A lock without flags on a buffer the GPU drew from last frame
  waits for that frame: the CPU never runs ahead of the GPU. win32u's D3DKMT Lock2, Unlock2,
  WaitForSynchronizationObjectFromCpu beside it. Then Present (the host's hook calling the real one, windowed:
  14.7 %), EnvMapCoords 4.9 %, LS3D's visibility 2.7 %, LightVertices 1.9 %, the particles 1.4 %.
- **The same locks in the scenes** (31a's table, the clean run of 2026-10-26): near 36 a frame, 0.30 ms of 1.41;
  night 0.19 to 0.26 of 1.65 to 1.83; crowd 0.74 of 6.42 (1.2), 2.12 of 7.74 (1.0); far 221, 2.6 ms of 39.5. A far
  run today with the machine busy (33 % other processes, the GPU's span 61 ms) had the same 211 locks take 22 to
  25 ms: the wait grows with how far the GPU is behind. Only measured, nothing changed.

## 2026-10-27: the game's buffers renamed (host/vbrename.cpp; ROADMAP 31i)

- **Asked** (the owner): take the vertex buffers' lock stall out properly, measure the menu, near, the crowd and far in
  both labs, one lab at a time.
- **Read**: LS3D's pools (C_VBuffer, 1.2 ls3d+688B0 and its creator ls3d+68CF0; 1.0 C_VBuffer::m_Init ls3d+65750,
  m_Lock ls3d+A9D0): 0x20000 bytes or more, DEFAULT unless managed for the meshes' sources, locked whole with no flags
  (1.0: NOSYSLOCK), unlocked together by C_VBManager::m_UnlockAllVBuffers. dxwrapper's d3d8to9 and its Direct3D 9
  wrapper pass Lock and SetStreamSource straight on (d3d8to9 drops DISCARD from a buffer not dynamic and write-only).
- **Made**: host/vbrename.cpp, installed at the game's CreateDevice (gfx.cpp) on the Direct3D 9 device under the
  wrapper: CreateVertexBuffer and CreateIndexBuffer outermost (the caller's return address: the game's, not the host's
  or a mod's), the rest innermost. A copy in memory with MEM_WRITE_WATCH, copies of ours (dynamic, from 3 up to 8 MB a
  buffer, 192 MB in all), event queries, the bindings followed (Set/Get, a state block's Apply, Reset); NOOVERWRITE and
  DISCARD written into the copy drawn with the same flag, a discarded copy written whole; buffers written only so
  given back after 8 unlocks. `vb.set {rename: false}`: the game's own buffer locked at the game's Lock with its
  flags (its wait where it was), written at the Unlock and drawn: the A of the A/B (near in it: 1.435 ms, 31a's
  baseline 1.41). `vb.check` reads every copy drawn back (only the pages it holds). `vb.selftest` (offline): a device
  of its own, 24 frames written and drawn, the copy drawn against what was written, GetStreamSource, a state block,
  a DISCARD between plain locks, a ring given back, the release.
- **Found on the way**: (1) the first measures ran with the machine busy (the GPU 83 %, other programs): every wait
  for the GPU grew (near 6.9 ms with the game's locks, 2.2 renamed); the menu's 1.2 ms of the day before was partly
  that. (2) The first version renamed at every Unlock: LS3D's rings (1.2's shadows, NOOVERWRITE appends and a
  DISCARD) ran out of copies and waited, worse than the game; and the A mode then took their flags off. (3) 1.0's
  people's shadows: IGraph::DrawIndexedPrimitiveList (ls3d 1.0 0x69F30) locks its one vertex buffer and its index
  buffer whole with NOSYSLOCK for each shadow, writes at the same place and draws: each waited for the shadow before
  (about 1 ms of the crowd's frame); three copies were too few, so a buffer gets more, up to 8 MB. (4) A copy
  written with DISCARD kept only the pages written after it; a plain lock drawing it later would have lost the rest:
  now written whole (vb.check had found an index buffer's 17 pages differing).
- **Measured** (one lab at a time, `bench` A/B, counters off, a quiet machine): see ROADMAP 31i: the menu -7 %,
  near -16 % (1.2) and -18 % (1.0), the crowd -10 % and -18 %, far -6.5 % and -7 %, far batched -10 %. The pictures
  the game's (a mode against itself differs as much: the scenes' own flicker). `vb.check` 0 of 5252 (1.2) and 5556
  (1.0) pages. The A mode under batches differs at the silhouettes (ROADMAP Known gaps). fps_unlock's line costs
  nothing measurable (the crowd, 0.005 +- 0.109 ms). Offline tests ALL PASSED.

## 2026-10-27: objects whole in batches not given to LS3D (host/batch.cpp; ROADMAP 31f/31g)

- **Measured first** (1.2's batched far frame, `profile`): the request a group (batch.cpp's Group, ~29 700 a frame)
  19.7 %, the batches' draws 16 %, LS3D's render loop 15.4 %, the visibility 13.8 %, I3D_object::m_AddFGroups 6.6 %,
  RadixSort32 1.6 %: an object that never reaches m_AddFGroups takes all of these off but the draws and the
  visibility.
- **Read** (both builds, the same code): m_AddFGroups picks the LOD (GetObjectLOD from the camera's place
  AddFGHlpStruct), fades the last LOD, relights the vertices when the sector's lights' stamp (+0x140) or the
  visual's +0x118 differ from the LOD record's +0x04, +0x08, animates the uv, the environment maps, then queues an
  entry a face group (32 bytes: +0x04 the sort key, +0x10 the group, +0x14 the sector). Render sorts them, sets
  each one's sector fog, clipping and depth bias, and calls the visual's +0x6C. Render draws the backdrop sector
  first with another projection (pViewProjMatrix the camera's +0x324 in 1.0) and each mirror's reflection with its
  own camera before the main pass (the camera's +0x1E4): the skip only in the main pass, only without mirrors.
  game_build.h: addFgCamera, addFgSector, renderScene, currentCamera, viewProj, lodFadeMin, fogOn (check_builds.py:
  read by m_AddFGroups, used by Render, ALL PASSED).
- **Made**: an object all of whose groups were taken in a frame is learnt (its LOD record and stamps, its world
  matrix, its sector and the sector's fog as one number, each group's sort key); the frames after, its m_AddFGroups
  (hooked) is not made while all of that stays (GetObjectLOD through the host's chain, no fade, no uv animation, no
  animated material or environment map): its groups are asked for at once. The batches keep LS3D's order by rank
  (the least sort key they were asked with): those asked early are drawn before an entry of a greater key, all
  before the first blended draw, the rest at the frame's end. A batch drawn and asked for again later in the frame
  is drawn again (it was not: a latent loss). `render.set {batch_objects}`; render.state's batch `objects_*`.
- **Measured** (one lab at a time, `bench` A/B `batch_objects`, batches on): far 1.2 21.96 to 18.48 ms (-3.48 +-
  0.03), 1.0 21.90 to 18.56 (-3.30 +- 0.16); near -0.049 and -0.064; night -0.036 and -0.030; the crowd unchanged.
  The pictures: 1.2 the same but for the FPS line's digits; 1.0 against LS3D 0.276 / 0.233 / 0.191 of 255, 1.04 %,
  the batches' own reference (31f/g). The whole city in 1.0: LS3D 36.5 ms, batched and skipped 18.55.
- **Found**: the batches' memory grows with captures again (ROADMAP Known gaps); 1.0's Game.exe is not
  large-address-aware: host/vbrename.cpp now takes no buffer and makes no copy below 384 MB of free address space
  (`vb.state` `address_space_free_mb`, `refused_space`).
- **Asked on the way** (the owner): is the city behind the camera drawn? No: LS3D's frustum culling stays in the whole
  city's scene; looking up from the same place the frame is 1.3 ms, 10 draws.

## 2026-10-27: the batches' memory held (host/batch.cpp)

- **Found**: in 1.0 (Game.exe not large-address-aware: 2 GB) after the whole city, the street and a drive a
  screenshot's copy failed with E_OUTOFMEMORY; far, near, far took the batches from 3.6 to 6.5 million vertices and
  the free address space from 857 to 608 MB. Not the dead groups alone: members 41 778 against 29 600 asked for a
  frame. The city's streaming makes its blocks' objects again (`activated` 618 after 314): the old objects' groups
  were never asked for again and never dead, their vertices held until the mission closed. And a batch was compacted
  only at its draw, only past half dead and 4096 indices: after one round of captures again exactly half were dead;
  the dead spread over a thousand small batches stayed.
- **Made**: a group not asked for in 30 s (by the clock: frames a sweep apart) is dead; every 300 frames every batch
  more than a quarter dead is rebuilt of its live groups (at a draw: 4096 indices at least), a batch with none lets
  its buffers go. render.state's batch `compacted`, `stale`.
- **Seen**: 1.0, far, near (45 s), far, three rounds: 3.6 million vertices each time far (3 605 528 in 1.2, the same
  round), 174 dead indices, 845 to 850 MB free; the pictures as before. Offline tests ALL PASSED.

## 2026-10-27: the loader's own settings in Options > Mods (host/settings.cpp)

- **Asked**: the rendering's optimisations switched on and off in the game's own Options > Mods.
- **Made**: a page "Rendering (loader)", the Mods list's first row, made by panel.cpp from a block of the loader's
  as a mod's from its manifest: Batched drawing (render.set batch), Objects bypass LS3D (batch_objects), Buffer
  renaming (vb.set rename), Whole city drawn (whole_city). config.cpp keeps it as a config of the same kind under the
  id `loader` (ConfigOwn: the defaults the loader's code, the changes in mml\data\loader\config.json, watched as a
  mod's; a mod with that id is refused); settings.cpp applies at the next frame only what changed, through render.set
  and vb.set (before LS3D's driver is made it waits). The defaults are what the loader did with none: renaming on,
  batches off (opt-in until driven through the missions), the whole city off. panel.cpp's config calls now take the
  settings' id, not the mod.
- **Seen**: 1.0: the page drawn (the game's paper, four check boxes, Defaults, Back); Batched drawing and Whole city
  clicked: applied at once (render.state: batch on, 328 of 328 blocks); the game closed and started again: applied
  at the first frame in the main menu, held through the freeride's load; Defaults: both given back; Buffer renaming
  off and on: vb.state's rename followed. 1.2: config.set {"mod": "loader"} applied (vb.set, render.set), panel.state
  lists the page first. Offline tests ALL PASSED (the page laid out cleanly, config.get / set by "loader").

## 2026-10-27: fps_unlock's ceiling near 1000 fps lifted on request (`zero_ms`)

- **Asked** (the owner): the main menu held at 990-997 fps; is there a cap? Yes, fps_unlock's own: it asks LS3D's
  limiter (IGraph::GetTimer, ls3d+72100) for 1 ms at least, and the limiter's clock counts whole milliseconds, so a
  frame inside one tick sleeps to the next.
- **Made**: the setting `zero_ms` (off by default; "Frames under 1 ms (a test)" on its Options > Mods page) asks the
  limiter for 0: no frame is waited for and the game is given steps of 0 ms, which none of its systems expects (the
  intro's cutscene, JOURNAL 2026-10-25): for seeing a light scene's real limit, not for playing. The mod's fixes take
  a step of 0 without a division (the menu camera's spring k = 0, the walk's lengths 0). The status and the line on
  the screen say so.
- **Seen**: 1.2 in a freeride, steps of 0 ms counted (13 a second); 1.0's profile screen 885 to 1092 fps, 146 steps
  of 0 a second (both labs running).

## 2026-10-28: beautiful water (mods/examples/river_water made anew)

- **Asked** (the owner): beautiful water, a visual, not a physical simulation (two earlier tries in mafia-ragdoll:
  one beautiful but overdone, one physics-first; the owner had liked that one's exact planar reflection and not its
  stripes or its hard sun).
- **Found** in the old example: its waves (eight sines) cut the reflection into regular stripes; its level was the
  mean of every `00test*` triangle captured (the river at -9.99 and a fountain at 2.41: -8.61, the reflection's
  plane 1.4 m off); its reflection was drawn even where no water was on the screen.
- **A fault of the loader's** (host/geom.cpp): a `geom_capture` asked from a graphics pass (after the world was
  drawn) ended with that frame, empty, every time. Now a capture asked during a frame waits for the whole next one
  (`g_frameOpen`; mml.h says so). Seen: the mod's captures from its pass find both planes.
- **Made** (river_water 0.2.0): the groups captured every 3 s, kept by their key, sorted into planes by level; an
  occlusion query a plane counts its pixels (read a few frames late, never waited for); the reflection view placed
  for the plane with the most, none drawn while none is seen (1.2 at the freeride's start: 0 views a frame). The
  ripples: a JONSWAP spectrum for the wind and its fetch, spread about the wind, three cascades (37.3, 9.17, 2.31 m,
  each its band of wavenumbers), turned and made into slopes by an inverse FFT on the GPU (Stockham, groupshared;
  river_waves.hlsl), the slopes and their squares mipmapped: each pixel's mean slope is its normal, their variance
  the roughness of what it cannot draw. The reflection followed along the reflected ray through the reflection's
  depth (three steps), blurred by that roughness: a Gaussian of 2 sigma t / (dist + t), up and down whole, sideways
  times the cosine (eight taps along the screen's vertical, jittered by interleaved gradient noise; the mip as wide
  as the sideways spread). The body: Beer-Lambert absorption down to the bed and back (`clarity`, `tint`), the light
  it scatters (`colour`) lit as the game lights its own (ambient + sun, display terms, clipped), the bed bent by the
  ripples; the sun's glitter as Cox and Munk's facets (their slopes' distribution, the sun's disc and the pixel's
  angle added), shadowed by the shadow map and where the reflected ray meets something; foam where the bed is under
  45 cm; the fog (the reflection already holds its own). Settings on Options > Mods ("Water"); `debug` views 1..6.
- **Tuned by looking**: JONSWAP's open-sea slopes (0.0275 at 3 m/s) and Cox and Munk's 0.003 at no wind (a sea's
  swell) turned the far bank's reflection into sky and blur; a calm city river's slope variance is now 0.0012 a m/s
  of wind, the default wind 1 m/s. The hardware's anisotropic filtering, asked for a footprint 8 to 30 times taller
  than wide, blurred both ways (the far bank a smear): the explicit vertical taps give the streaks. The game's light
  values are display multipliers (the sun 1.6, 1.19, 0.72): `Lin()` of them made the body two to three times too
  bright.
- **Seen**: 1.2 and 1.0, the city freeride at Little Italy's embankment: the bridges, their piers, the passing ship
  and the opened bascule bridge reflected and streaked, the sun glittering on the ripples, the dark green body. Cost
  (`bench`, one lab, the world held still, 1280x960): 1.2 8.03 to 16.1 ms (+8.0 ± 0.15; the reflection +7.1, the
  shadow map +0.3, not significant; the water itself +0.9), 1.0 2.43 to 4.97 ms (+2.5 ± 0.2). Offline tests ALL
  PASSED.
- **Left** (ROADMAP Known gaps, "The water"): the game's dark layer under the bridges shows as black lines at the
  waterline; the reflection's cost; one reflection plane.

## 2026-10-29: the sun where the sky shows it; light in the water; choppy crests (river_water)

- **Asked** (the owner): realistic water without heavy physics, the sun handled exactly and in the right place, a
  thought-out shader with light scattered in the water's body and through the crests, shadows of objects on the
  water; whatever the SDK lacks added to the loader.
- **Found**: the game has two suns. The world is lit (and its lightmaps' shadows baked) from the directional light,
  65 degrees up in the city by day; the sun the sky shows is the backdrop's lens flare, 14 degrees up, 55 degrees
  away (knowledge/notes/water.md). The glitter was drawn towards the light: a path of light on the water under an
  empty sky.
- **Made in the loader**: game_build.h `lensflareVtable` (both builds, tools/check_builds.py checks its constructor,
  destructor and Draw's reads), game.h the scene's sectors (`kScenePrimary`, `kSceneBackdrop`, `kSectorScene`);
  lights.cpp walks the backdrop sector too and finds the far flares; mml.h `scene_sun_seen` (the Rust crate's too),
  `MmlGfxConstants::sun_seen`, frame.hlsli `g_sunSeen`; `lights.list`'s `sun.seen`, gfx.state's `world.sun_seen`.
- **Made in the SDK's water**: the ripples' FFT also gives the heights and Tessendorf's choppy displacement's
  derivatives (a second pair of complex fields): the normal of the displaced surface and its Jacobian (sharper
  crests, flatter troughs, whitecaps where it folds); `WaterSunScatter` (the sun scattered once in the water: the
  slab's exact single scattering through the refracted rays, Henyey-Greenstein; and the crest's forward share
  scattered out of a face in its crest's shadow), `WaterThroughCrest` (the sky and the sun seen through a steep
  crest), `WaterSunSeen`, `WaterReflectionRise`; MakeSpectrum's slope variance grows like a clean sea's past 4 m/s
  (the river's film torn up). river_water: the glitter and the light in the water towards the seen sun, shadows on
  and in the water (the shadow map at the surface and a scattering length down), settings `scattering`, `phase`,
  `crests`, `chop`.
- **Fixed on the owner's reports**: a light band at the waterline under walls (the reflection's blur reached the
  sky the reflection's clip plane leaves past its waterline: taps there left out); a bright wedge along walls, rocks
  and hulls (the foam at walls lit by depth alone, on calm water too: now the waves' work, a few crest heights wide).
  A gap between a thing and its reflection at the waterline, measured to its cause (the owner: "stop guessing"): the
  reflection's clip plane 2 cm over the water cut the feet out (a band close to the water); the game's models end
  8 mm under the water, so a plane under it showed the sky beneath them (a dashed line); at the plane itself the
  foot's pixels are covered in part, and the dash pixels read sky (depth 100+) where the pixel beside held the rock at
  the water point's own distance. Now the clip plane is the water's and the lookup fills such holes with the foot
  beside them (water.hlsli `WaterFillFoot`); the dashed line gone in 1.2 at the owner's place.
- **Seen**: 1.2 and 1.0 (both labs): the glitter path under the seen sun, the light at 8 m/s as wind-driven water;
  `lights.list` finds the same two flares in both. Reflection's cost facing the city across the river (1.2, one
  lab): +2.2 ms (CPU 2.2: 700 more draws; GPU 2.4) of a 6.1 ms frame. Offline tests ALL PASSED (before the last
  fixes).
- **Left**: the water is drawn by a Direct3D 11 pass over the finished frame: translucent things the game draws
  over the water (glass, particles, smoke) are covered by it; see ROADMAP Known gaps "The water".

## 2026-10-30: Claude-to-Codex handoff audit

- **Asked**: check for Claude-specific setup and project rules missed during the first read.
- **Found**: AGENTS.md was 42,480 bytes; Codex's default instruction budget is 32 KiB, and this chat's injected
  prefix stopped during the intro-video note. Read the rest from disk. CLAUDE.md imports only AGENTS.md; the
  separate Claude setting is a Bash escaping guard, not a Codex/PowerShell hook. README's claim that `.mcp.json`
  registers the server for any MCP client was too broad: Codex uses config.toml.
- **Checked**: Codex's configured servers and this chat's tools had no `mafia`. The existing terminal helper's
  `game_status` and a lens-flare `kb_search` succeeded (names for both builds and the water notes); both labs were
  stopped, the default 1.2 lab in loader mode. The helper prints text only: an image result through it does not
  show the agent the frame. No game was started, closed or changed; no client configuration was changed.
- **Documented**: docs/AGENT_SETUP.md has a concrete Codex config example, the full-read warning, fallback limits,
  verification evidence and the easy-to-miss rules (both-build tests, generated SDK/Rust/methods, UTF-8, TLS,
  graphics startup, tester copies, permission boundaries). AGENTS.md points there before the truncation boundary,
  records the owner's re-read/hash and backup requirement, and labels the Bash fault as Claude-specific. README
  and ROADMAP Known gaps now distinguish native client setup from a working standalone server.
- **Scope**: documentation only; the existing water and loader changes and three old Claude worktrees were
  preserved. No build or game regression test was run. Connecting native Mafia tools remains a client setup step.

## 2026-10-30: Mafia MCP registered in Codex

- **Asked and authorized**: the owner wants full game interaction and explicitly asked the agent to connect the
  server itself. Registered `mafia` with `codex mcp add`, using the absolute existing executable and MML_REPO.
- **Preserved**: checked the current global config's SHA-256 before writing, made and hash-verified
  `C:/Users/PC/.codex/config.toml.before-mafia-20261030-171623.bak`, then used the supported CLI. Compared parsed
  settings afterwards: existing settings equivalent (omitted empty STDIO args and numeric formatting normalized).
- **Verified**: `codex mcp get mafia --json` and list report enabled, no tool allowlist/denylist. A separate client
  using the saved command/environment from outside the repository initialized the MCP server, listed 35 tools
  (including screenshots, input, launch/close, memory, deploy, Ghidra and crash triage), and queried both labs'
  stopped state. No game was launched or changed.
- **Left**: the existing chat's native tool inventory still lacks Mafia. The owner must restart the Codex app,
  reopen the chat (or start a new project chat if needed), then verify native status and screenshot access. This
  has not been called a live-game or visual test. AGENT_SETUP and ROADMAP now record registration and the pending
  client activation. The instruction-size setting and game code were left unchanged.

## 2026-10-30: native Codex MCP verified in both games after restart

- **Checked**: the owner restarted Codex; native Mafia tools appeared. game_status first confirmed both labs
  stopped. Launched 1.2 (profile 4, pid 11520) and 1.0 (profile Run, pid 15384) together into the city-small
  freeride, in background windows, police/traffic/pedestrians zero. Both reached a live player in a car, with
  start.state done and no start error, neither window in front.
- **Input and eyes**: native input Accelerate 600 ms, Brake 1000 ms returned resolved action slots 33 and 35 and
  player positions about 0.8 m from their launch positions in each lab. Both screenshot image blocks (1280x960)
  arrived and were visually inspected in chat: the street, car HUD and game frame visible. Existing devtools free
  camera was active in 1.2; its configuration was left alone. Screenshots remain in each lab's mml/shots/1.jpg.
- **Cleanup**: confirmed the launched pids through game_status, closed only those labs with native game_close
  (11520 and 15384), then game_status confirmed both stopped. No mode/default switch, configuration edit, build,
  deploy, save or regression suite. Native connection and basic live interaction passed; not every tool tested.
- **Documented**: AGENT_SETUP now records the live check; ROADMAP's pending client activation is resolved, with
  the instruction-size limitation kept. Source code and the earlier water work were not changed.

## 2026-10-30: current water inspected; free camera left for the owner

- **Asked**: inspect the current water, then leave the free camera to the owner. Inspection only: no shader,
  loader, build or deploy changes. Started only 1.2, profile 4, whole-city day freeride, police zero, background
  window (pid 5856); the 1.0 lab remained stopped. Moved the player's car to the documented safe embankment.
- **Found in live settings**: river_water on, reflection/shadows on, debug 0, wind 1 m/s, direction 210 degrees,
  clarity 2 m, but waves=0 (default 1): the previous waterline-debug setting suppresses the ripples. Captured the
  mirror-like current surface (shots/2.jpg and 3.jpg); temporarily set only waves=1 to inspect the intended ripples
  (4.jpg and 5.jpg), the sun's glitter (6.jpg), and the contact with walls/piers (7.jpg). The reflection, moving
  ripples and the glitter beneath the visible sun work in those views. A narrow pale strip remains along the wall
  at the low view; its cause was not diagnosed. This does not close the water's remaining renderer-order issues.
- **Telemetry, not a benchmark**: across the river with waves=1, river_water.status reported one 54-triangle plane
  at -9.992 m, about 620k visible pixels, reflection active; frame avg 9.97 ms, views 3.25 ms, bridge 1.66 ms,
  mod 0.017 ms. No A/B or GPU cost claim; these depend on the view and running scene.
- **Handoff**: restored waves=0, preserving the owner's existing water settings. Seeded devtools's actual free
  camera from the inspection view by toggling its existing freecam flag off/on while the bridge's camera was drawn,
  then camera.release returned held=false, override=devtools. Verified devtools.state freecam=true and camera.state
  eye [-1540,-6,-65], forward approximately [-0.4851,0.0485,0.8731]. Final image shots/10.jpg. Speed 10 m/s.
  Live bindings: WASD move, mouse look, Space up, Q or RightCtrl down, Shift faster, wheel changes speed, F3 toggles.
  The game remains running for the owner, with no bridge camera timer holding it. Default lab and mode unchanged.
- **Documentation**: ROADMAP no longer says the old bridge shade's texture must be found: the frames and the
  existing hide rule were already documented in knowledge/notes/water.md and present in the running mod.

## 2026-10-30: DLSS enabled during the owner's water inspection

- **Asked**: enable DLSS without losing the free camera. Loaded dlss_nr into the running 1.2 lab through mod.load;
  its graphics bridge was already active for river_water, so no game restart or manifest change was needed.
  The mod is loaded for this session; its manifest's enabled flag remains false.
- **Verified**: dlss_nr.state/side identify an RTX 3070, DLSS SR DLL 310.9.16.0 available, the saved mode DLAA
  (1280x960 in/out), pipeline on, frame generation off. The side reported hundreds of SR evaluations, host drawn
  grew to 4195, late=0 and last_error empty; screenshot 11.jpg was inspected. Neural Rendering did not initialize:
  nvngx_dlssnr.dll is absent (and this project's NR implementation requires RTX 50). Set the live nr request false
  to leave the supported DLAA path selected; other DLSS settings were kept. No binaries downloaded or deployed.
- **Free-camera question**: the owner expected E to rise because Q descends. Read devtools.cpp Wanted: vertical
  movement uses Jump minus Crouch. Current input.actions binds Jump to Space, Crouch to Q/RightCtrl; E has no
  separate camera binding. Explained the current controls; no global action rebinding or code change. Verified
  devtools.state freecam=true while the owner continued moving it. The game was left running for them.

## 2026-10-30: opaque uniform-depth water and the moving DLSS waterline fixed

- **Owner's report**: submerged objects showed through a river with no complete bed; a gap opened where a wall
  ended at the water when the camera rose. Especially visible at 10 FPS, like a frame of lag. The initial static
  reflection-foot repair (contact probes up to three texels and a point sample at contact) did not solve the
  motion problem; the owner correctly rejected that as a complete fix.
- **Water body**: replaced refraction strength with a uniform virtual vertical depth, default 10 m. Refracted
  ray length drives absorption/scattering; no underlying scene colour, geometry-derived bed depth, bed foam or
  subsurface object shadows. Surface sunlight/shadows and whitecaps remain. Both labs checked with a scaled
  2bbat1 prop above the river and below it: visible above, hidden below, then removed. The first attempted move
  used the wrong prop.place key; repeated correctly with `pos` before accepting the result.
- **Root cause and fix**: host/gfx.cpp ran the upscaler before all graphics passes. Pipelined DLSS returned the
  previous colour frame, while river_water used this frame's camera, depth, mask and reflection. Graphics passes
  with negative order now run at the render size first; a bridge shader composites their premultiplied colour
  with the current world into a separate input texture before the upscaler. Water order is -120. Nonnegative
  passes remain after it, the no-upscaler path keeps increasing order, and the initial/no-answer fallback retains
  the prepared colour. Resources are released with the bridge's sized resources. No new ABI or game addresses.
- **Validation**: x86-release loader and water built; full tests/run_tests.py returned ALL PASSED after the host
  change. Installed loader/PDB in both stopped labs, then launched both together (1.2 pid 24256, 1.0 pid 21908).
  Native graphics state on, fresh reflection views, no DLSS error. Actual upward free-camera input at a 10 FPS
  cap with pipeline enabled: 1.2 DLAA (1280x960), 1.0 Quality (853x640 render to 1280x960). Moving contacts remained
  joined in the inspected sequences (1.2 shots/19..21.jpg, 1.0 shots/11..13.jpg). Owner: "everything is fixed".
  Colour synchronization is fixed; water-surface depth/motion replacement and translucent ordering remain future
  work. No performance claim or new benchmark.
- **Backups**: build/water-opaque-backup-20261030-174332 and build/render-sync-backup-20261030-181001 contain the
  affected pre-edit sources/docs. Previous installed ASI/PDB saved in each lab's mml_lab/render-sync-20261030-18110x.
  Unrelated uncommitted work preserved; no commit or push, no lab mode/default switch.
- **Optional capture work**: the owner had also requested a screenshot plus the previous second of camera poses.
  Added disabled-by-default camera_capture, Ctrl+F11, BMP/JSON in mml/data/camera_capture, commands capture/list/
  replay/stop. Built and capture files checked in both builds; path traversal refused. Uses existing render,
  hotkey and camera services, no addresses or ABI additions. Records per-frame poses, projection metadata and
  settings; does not save world actors or wave time. Replay command accepted in 1.0, but exact replay validation
  remains pending. It can capture a shorter history immediately after loading. Left unloaded after restart when
  the owner redirected priority to fixing the lag; it was not required to diagnose or fix the cause.
- **Handoff**: restored 1.2 max_fps=0, DLAA and pipeline on, waves=0 as before. Returned actual devtools freecam
  to the owner's pre-restart eye [-1569.384,-1.891,7.412], released the bridge camera (held=false), verified
  freecam=true. Restored 1.0's max_fps=0, upscale=5, pipeline=false and freecam=false, closed only its lab pid
  21908. 1.2 remains running for the owner.

## 2026-10-30: water shadows follow the visible sun by default; source selectable

- **Requested**: choose whether water shadows follow the visible sun or the scene's directional light, since the
  two do not coincide in Mafia. Default to the visible sun. Added Water's Shadows checkbox and Shadows from
  selector: Sun (`shadow_source=0`, default), Scene light (`1`). Source changes are live; debug 8 shows the map's
  visibility on the water. Existing baked scene shadows, scene illumination and glitter direction are unchanged.
- **Implementation**: appended shader_shadows_from to MmlHost, preserving the old ABI layout and legacy
  shader_shadows. Explicit MML_SHADOW_SUN requests use SceneSunSeen for the actual map camera, falling back to
  SceneSun only when no sky sun exists; MML_SHADOW_SCENE uses SceneSun. A selected light below the horizon draws
  no map. One map is shared: the latest active explicit request wins, legacy requests have no source preference,
  release/unload restores the preceding request or scene default. Invalid source leaves the old request intact.
  Added the Rust safe wrapper and regenerated sys.rs. Bridge shader.shadows accepts source sun/scene; shader.state
  reports source, source_used, source_owner and actual unit direction. Updated PROTOCOL and generated methods.inc.
- **Validation**: x86-release loader/water and testmod_kit/rust_kit_rs built. Full tests/run_tests.py ALL PASSED,
  including explicit SDK selection, legacy coexistence, invalid request preservation, precedence and release,
  generated C/Rust contracts and the existing rendering/loader tests. Log: build/test-shadow-source-20261030-rerun.log.
  The first run used an old kit DLL (the runner does not build test mods); rebuilt it before the successful run.
  git diff --check clean. No new addresses or layouts, no performance claim.
- **Both labs together**: installed hash-matching ASI (875E749056911AA7098746AE565ECA9844FB0AC656300583321CC0E2D9BC5D80)
  and water DLL, started 1.2 pid 20704/profile 4 and 1.0 pid 23252/profile Run, full-city day, police zero, background.
  Both initially selected sun without a saved override, map drawn, source_owner river_water. Actual visible-sun
  direction [-0.4623,0.2480,0.8513]; scene mode [-0.2431,0.9026,0.3552]. Same held camera
  [-1490,30,20] looking at [-1590,-10,-25]: debug-8 sun maps showed long building shadows over the river, scene maps
  short bank shadows. Both directions visually checked, then debug restored to 0 and Sun selected.
- **Menu verification**: inspected both Water pages; labels fit. 1.2's selector cycles Sun/Scene light on click;
  1.0's dropdown was opened and Scene light selected with native in-game input. Both changed the saved setting.
  menu.value {id:303,value:1} did not set the selector, despite returning success/value 0: read start.cpp
  SafeSetValue and the kind-aware setters in menus.cpp/menus10.cpp, recorded the bridge gap in ROADMAP. No tool
  code changed. Native menu interaction confirmed the feature independently.
- **Preservation**: source/docs/SDK/test backups in build/water-shadow-source-backup-20261030-1830; previous
  installed ASI/PDB in each lab's mml_lab/water-shadow-source-20261030-1830. The first 1.2 deploy hit a closing
  process's DLL lock; confirmed its old pid gone, retried, then verified hashes. No mode/default change, no save,
  commit or push. Existing uncommitted work kept.
- **Handoff**: both menus closed, debug 0, shadow source Sun. Closed only 1.0 pid 23252. Left 1.2 running, loaded
  DLSS again after restart and verified DLAA/pipeline true with no error; frame generation off. Restored the owner's
  pre-restart devtools camera [-1553.206,1.334,-20.439], released camera.set (held=false), freecam true.

## 2026-10-30: current posed shadow casters, depth-state repair and persistent DLSS

- The owner reported solid leaf/fence shadows, slow updates and a missing train wagon. The default mesh path
  had no material alpha and refreshed scene snapshots every 0.25 s; railway actors were excluded. Switched the
  default to LS3D's actual posed drawing every frame. Shadow-only box planes correct CullThruPortal's perspective
  overwrite; camera-distance LOD and screen occlusion no longer discard nearby casters from the light camera.
  New build-table fields/bytes verified in both games by tools/check_builds.py. Cut-out textures and alpha tests
  are preserved; translucent non-cut-out groups are skipped. The owner independently verified both wagons.
- During this implementation an auxiliary scene render leaked ZWRITEENABLE=0. The owner saw far city geometry
  overwrite nearer surfaces. Surveys confirmed every opaque main-world group had depth writes off; restoring
  that state immediately restored the city. LS3D's ResetRenderProps state block omits it. PassGame now explicitly
  saves/restores depth writes, including failure paths, and returns the borrowed shaders. Added real-device
  tests/game/test_shadow_render.py: repeated shadow off/on, opaque zwrite and actual textured cut-out casters.
  Eight assertions passed in each lab. The owner confirmed the city fixed. shader.survey {shadows:true} records
  actual caster groups/visuals and world matrices for future pose regressions.
- Water's SunShadowSoft adds a bounded blocker search and contact-hardening PCF. Shadow softness scales the
  solar angular radius (default 1); zero retains fixed PCF. Approximate filter, maximum 32 texel radius.
- The owner also caught DLSS missing after our restart. Earlier mod.load was session-only while its installed
  manifest remained disabled. Enabled dlss_nr in W:/MafiaLAB/mml/mods/dlss_nr/mod.json; the shipped example remains
  opt-in. Automatic startup was verified on the next launch: DLAA, pipeline true, increasing drawn, no last_error.
  No manual mod.load after that launch. Deploy preserves the lab's own enabled flag. This corrects the earlier
  handoff practice, rather than repeatedly reloading DLSS by hand.
- Built loader ASI B187B2BBDED309C98B22C4C7E54158A21211E80CC0EB265C0483964D0668DDAA, hash-matched in both labs.
  Full offline suite passed after the host fixes: build/test-shadow-casters-final-20261030.log. Source backups in
  build/water-shadow-casters-backup-20261030; preceding installed loader/PDB in each lab's
  mml_lab/shadow-casters-20261030. Shared rendering changes tested in 1.2 and 1.0 together.

## 2026-10-30: actual tessellated 3D waves

- Requested: keep the water's appearance, but give waves real volume. The FFT already produced height; the
  original water vertex shader left all captured triangles flat. The unused second complex FFT pair now packs
  horizontal Dx+iDz. Resolve exports mipmapped height/Dx/Dz; no extra FFT pass or CPU readback per frame.
- sdk/include/mml/water_mesh.h clips captured triangles to a fixed 16 m grid, preserving original footprint and
  water levels. GPU hull/domain shaders tessellate by distance, use symmetric shared-edge factors, reject patches
  outside the view conservatively and displace the actual vertices vertically and towards crests. Normals retain
  the undisplaced material phase; fine wavelengths remain pixel shading, coarse geometry uses filtered waves.
- Separate D32 water depth orders overlapping crests; main-world depth still clips city geometry. Render target,
  depth target/state are restored and hull/domain shaders/resources unbound after drawing, preserving later
  passes and DLSS. Reflection depth attenuates waves at cut wall feet; its camera stays on the mean plane. No
  underwater renderer, physical water-level change, or water depth/motion replacement for DLSS in this block.
  Those approximation limits are in ROADMAP and knowledge/notes/water.md.
- Added live Water controls 3D waves (default on) and Wave height (0..8 multiplier, default 1). Debug 14 shows
  actual wireframe; status reports patches and asynchronous GPU domain vertices/clipped triangles. The river's
  54 triangles became 91946 bounded patches; the inspected low view in both builds reported about 704397 domain
  vertices and 514275 clipped triangles. These counts describe that view, not a performance benchmark.
- Verified visible crests/troughs and near-bank views in both running labs, solid/wireframe, menu labels fit in
  both versions. The owner: "very beautiful"; kept the approved live 1.2 settings (wind 10, waves 1, wave_height 3).
  New tests/game/test_water_geometry.py passed in both builds, including actual tessellation, fresh reflection,
  nonzero height, live off/on and DLSS still drawing in 1.2. Offline water_mesh_test checks signed area, bounds,
  maximum patch size and exact watertight shared edges; integrated into tests/run_tests.py. Full offline suite
  ALL PASSED: build/test-water-geometry-20261030.log; diff --check clean. Build/install river DLL hashes match:
  A6A1A946D8ECBC552D34E8E51503847CC5C611B414DD8F7586C6A76273A3BBCA in both labs.
- Backed up affected sources before the geometry changes in build/water-geometry-backup-20261030. Existing work
  preserved, no save/commit/push or lab mode/default change. Restored 1.0 wind/default height and closed only its
  pid 25068. Left 1.2 pid 20848 running, DLSS autoload enabled, debug 0. Seeded actual devtools freecam at the
  inspection view [-1510,-8.9,-25] and released the bridge camera (held=false, override=devtools); menus closed.

## 2026-10-30/31: heavy water view, temporal inputs, auxiliary audio and fitted shadows

- Saved the owner's exact settings and 35--40 FPS camera in docs/WATER_RENDER_PLAN.md and local evidence under
  build/water-perf-20261030 before experiments. Backups of source/installed host retained. No quality controls
  lowered, no feature disabled for the result. Root causes and remaining budget are recorded in the plan.
- The second shadow was hidden baked watercity.stin geometry entering the unnamed light camera. Auxiliary hide
  rules now include shadow rendering and projected game shadows are not recast. Shadow requests keep the tracked
  device mirror alive; per-draw constants follow compiled shader read masks, including GPU skinning. Removed
  about 75521 redundant device Get* calls/frame; native mirror checks 5000 draws, zero mismatches.
- LS3D Render updated area/environment sound from every auxiliary camera even without a listener. A host-owned
  mid hook skips only this block, preserving hardware mirror preparation. Both builds' audio RVAs disassembled
  and guarded by check_builds; owner confirms sound no longer lags. view.state reports main/auxiliary counts.
- Appended optional MmlGfxFrame temporal_target MRT and host merge before upscaling; displaced water supplies
  linear depth and previous camera/FFT motion with matching city jitter. C/Rust ABI tails and wrappers updated.
  Owner confirms DLSS works well; native depth view and both builds' Quality/pipeline checks agree.
- Appended shader_shadows_fit API and Rust wrapper: fit visible frustum/receiver slab, rectangular orthographic
  camera, density-preserving map growth up to device caps. Fixes the visible fixed-radius boundary. PCSS uses
  independent X/Y world scales. Shader compilation moved to the build; nonblocking GPU queries split FFT/surface.
- Full offline tests ALL PASSED; both live builds passed 22 shadow/depth/geometry/temporal checks each, concurrent.
  Raised/turned coverage and upward freecam at a 10 FPS cap inspected in both. Final single-lab checkpoint sample
  14.68 ms / 68.14 FPS versus baseline 26.96 ms / 37.09 FPS; not a paired estimate (world/load differ, DLSS now on
  and shadow coverage larger). Static batching was slower by +0.427 +/- 0.204 ms (95%); kept off. Visibility,
  reflection and required device ownership waits remain costs, not claimed perfectly optimized.
- Hash-matched final loader FCEF199A9D8410A003EC5492F0EF826E2046E5548014596D9C4C10AB6CB27BA5 and river DLL
  CE17573F64FEEFC34328DF3E46B65745A1D978D2AC357B62C9BAA44628DA2CAD in both labs. Restored 1.0 overrides,
  closed only pid 11988. Left 1.2 pid 18456 at the checkpoint, owner's water settings intact, DLAA/pipeline on,
  max_fps=0, no menu, actual devtools freecam active and bridge camera hold released. No commit/push/lab switch.

## 2026-10-31: owner found the game closed after handoff

- At 08:43 1.2 pid 18456 was gone; the owner says they had not closed it. The agent had closed only test 1.0
  pid 11988. Crash triage returned an unrelated October 27 report; no new dump. Last normal log: 01:34:16,
  DLAA running normally, no process-exit marker. Windows Application/System checks over ten hours found no
  matching game failure, GPU reset, resource exhaustion or shutdown/sleep record. Cause/exit time unresolved;
  documented in WATER_RENDER_PLAN rather than claiming a confirmed crash. MCP's detached launch retains no
  process handle from which to obtain the ended process's exit code.
- Saved previous run's log to build/water-unexpected-exit-20261031/mml.log, relaunched 1.2 only (pid 19916),
  restored benchmark camera and owner's exact water settings, verified DLAA/pipeline drawing with no error.
  Actual devtools freecam active, bridge hold released. No code changes or unsupported attribution of cause.

## 2026-10-31: reviewed the owner's mafia-ragdoll water work

- Read the sibling AGENTS, WATER/architecture notes and implementation of the surface, FFT, reflection, shore
  maps, current and offline bench. Compared actual shader/resource paths with this repository's water. No sibling
  edits, external builds or live game changes. Findings and source locations: docs/WATER_RAGDOLL_REVIEW.md.
- Useful differences: Gaussian/EWA reflection footprint filtering, full slope covariance and variance-preserving
  tiling/flow, a persistent geometry-derived shore distance map and supersampled motion-reference methodology.
- Direct clipmap replacement is not a measured optimization: its configured 128x128x2x9 submits 294912 patches,
  ours recorded 91946. Its position-only cached shadow buffer also lacks our material cut-outs; retain our live
  posed path. Reflection culling to water remains unfinished there too. Added targeted candidates to the plan;
  no claim of an FPS improvement or a ready drop-in port, no approved appearance/settings altered.

## 2026-10-31: bookmarked the tiny-water slowdown, diagnosis deferred by owner

- Captured the current camera, native screenshot and runtime/config state without changing the view or settings.
  Owner reports about 19 to 5 FPS when a tiny lower-right piece of water enters the view; screenshot shows 5 FPS,
  202.7 ms. Evidence: build/water-tiny-patch-20261031/checkpoint.json and frame.jpg. Eye [-1407.48035,7.42366,
  -798.74927], forward [0.706159,0.129395,0.696129], FOV 55.4129, actual far 4993.269 m. This is a separate
  reproduction from the earlier bridge view. Added the deferred checkpoint to WATER_RENDER_PLAN.
- Sky work is ongoing separately: a painted-sky HDR prefilter replaced the independent analytic glitter; owner
  liked the cloud shape and requested angular restriction. Live 1.2 has a soft sun_radius setting (default 18 deg).
  Geometry visibility/recovery edits exist in source/build but have not been deployed or regression-tested yet.
  These changes are not marked finished; both-build validation and documentation remain before completion.

## 2026-10-31: completed painted sky light and conservative offscreen visibility

- Owner steered the sibling review away from water physics and toward visibility and the sun/cloud painting.
  Replaced the independently added analytic glitter with sky-depth HDR expansion before reflection mips. The
  painting supplies the source's shape/hue; a soft cone around the visible sun limits it. New sun_radius control:
  0.5..60 deg, default 18, Water 2/2. Owner confirms the current sun in the reflection looks good. LDR-to-HDR
  expansion remains an explicit reconstruction assumption, not recovered physical radiance.
- Original water triangles receive conservative expanded-frustum rejection before reflection/full surface/FFT.
  Camera-epoch occlusion queries invalidate old zero counts on movement/settings changes; returning water gets
  a current reflection without waiting for old queries. Pending GPU queries are not overwritten. No resolution,
  visible update cadence, wave detail or feature reduction. Ordinary instancing cannot eliminate another camera's
  projection and rasterization; shared scene resources are already reused. Tiny visible-patch cost stays deferred.
- Actual CsPaintedSky tested on WARP (cloud/geometry mask, angular support/falloff, hue and off identity), integrated
  into offline tests. Full suite ALL PASSED. Native visibility/recovery passed both builds concurrently; integrated
  1.0 run passed 29 checks with initialized DLSS Quality/pipeline. Initial drawn=0 was the not-yet-ready sidecar,
  confirmed by its state, not a rendering error. Final settings-invalidation delta rechecked in both builds.
- Radius label fits the 1.0 second settings page. Opening another page while the same owner's page is open is
  refused by MenuPageOpen; the existing native tab performs deferred close/reopen correctly (wait for its tick).
  Source, log and UI inspected; no tool workaround/fix needed. Final DLL 4266CFDB8AFBE3DC6A7DBD333EE8428F07FD9BEFE72432F15A71BFFE2A0CF130
  hash-matched in both labs. Evidence/source backup: build/water-sky-20261031. No physics work or tiny-patch
  diagnosis; the owner's accepted settings and separate checkpoint are preserved.
- Restored 1.0 DLSS overrides and closed only its test pid 21728. Left 1.2 pid 19916 running with the owner's
  current settings, actual devtools freecam active, bridge hold released. The owner currently has DLSS processing
  off for comparison; left that choice intact (autoload stays enabled). Final snapshot in the evidence folder.

## 2026-10-31 — Infinite-distance tiny-water optimization and HUD regression

- Reproduced the saved camera at all 328 blocks / about 5 km: 222 ms/frame, 103 ms shadow pass, 45 ms reflection;
  the actual water surface GPU cost was under 0.5 ms. Source/installed-host backups and baseline archived in
  build/water-tiny-opt-20261031. User's settings captured before measurement.
- Rejected current-depth late shadow Render: it reduced shadow work to about 4 ms but corrupted radar and font
  blending. The owner caught the font regression. Disabled it immediately; device/IGraph state restores did not
  fix the architecture. Removed all late-path changes from gfx/view/render, its shader and target; rejected source
  retained only in ignored evidence. No claim that the underlying re-entrant LS3D state problem is solved.
- Added append-only shader_shadows_surface ABI and safe Rust wrapper: copied finite triangle footprint, a bound
  on displacement per axis, current-frustum clipping with conservative expanded planes and light-space bounds.
  The shadow pass remains before the main world. PCSS padding retained; smaller world-box quantization avoids
  excessive unrelated city submissions. Texture size, wave detail, reflection and update cadence retained.
- Tiny-view native caster groups about 53922 to 41263, shadow CPU about 103 to 70 ms; texel density improves despite
  the hardware cap. First footprint-only A/B had no gain; tighter extents are necessary. Full-frame short A/B
  medians 190.9 to 174.7 ms, but its 95% interval includes zero: no precise FPS claim. Reflection still about 41 ms.
- Offline suite ALL PASSED. Both live builds: 29 integrated shadow/depth/alpha, wave, sky, DLSS Quality/pipeline
  checks, plus 5 far-view bounds checks. An initial 1.2 tessellation run expected DLSS while owner had it off;
  repeated after enabling and waiting for the sidecar. HUD screenshots in both builds show normal radar/text.
  Final host 6546BC65C8284D30B6271B7A6012774F9BB0F87B811ABD57CC2D3C6278BD99C0 and water
  BC6FB364AD06935BB500072425F76B643107FA1293419CC198B42A20B99DBCED installed/hash-verified in both labs.

## 2026-10-31 — Current-depth shadow fit recovered with the native state contract

- Continued at the owner's request and reproduced the additional freeride-start view: both water queries zero,
  reflection stopped, but a full shadow camera still rendered about 78 ms. Disabling DLSS also left motion replay
  requested unconditionally in mml_mod_load. ReadSettings now requests it only while S.on; native off-state shows
  no motion owner, no replay draws and no motion passes. DLSS remains installed/enabled as the owner chose.
- Used both Ghidra projects to identify the actual state left outside D3D: ResetRenderProps writes 19 cached
  render properties, and GetViewPort returns LS3D's own 24-byte viewport. Restoring the former fixes fonts;
  restoring the latter fixes the rectangular scene behind the radar. The rejected ResetRenderProps call alone
  did not preserve the outer state. Addresses in game_build.h, all store destinations checked by check_builds;
  Rust game tables regenerated. Native comparison now restores both caches plus a full D3D state block.
- Receiver-only long-range shadows use the same frame's opaque depth and conservative finite receiver boxes,
  followed by CPU intersection with the actual displaced triangle footprint. No history or stale zero gates.
  Empty receivers skip the shadow camera. World rules/surveys, view-pass consumers and populated near sectors
  retain early maps (Render consumes a mutable near queue after stage 1). Shadow children request no callbacks.
- Same-run start-view ABBA/BAAB, one game, held camera and world, batch off, DLSS processing off: early finite-fit
  109.648 ms, current-depth 56.392 ms; paired -53.268 ms +/-0.146 ms (95%), -48.6%. Picture comparison exactly
  zero in every channel, including HUD/radar. This measures depth fitting separately from the earlier idle-DLSS
  fix and is not a claim of an ideal-optimum renderer. Evidence: depth2-bench-start.json.
- Evaluated existing static batches under the same conditions: 56.148 to 27.347 ms, -51.5% +/-3.65 ms. Image mean
  difference 0.023/255 and 0.04% above 8/255, with isolated silhouette changes. Left batches OFF under the owner's
  no-visual-loss requirement; no batch code changed. batch-evaluation.json and native difference image retained.
- Final triangle intersection removes all shadow caster submissions in the hidden-water street view. Unit bounds
  tests pass; final native run passed 41 checks per build, including current-frame fitting after camera changes,
  density, opaque depth/cut-outs, waves, immediate visibility recovery and DLSS. The offline suite passed; binary
  signatures/cache stores and generated Rust checked again. A cold hidden-only test probed 1.0 before its light
  state was ready; subsequent state confirmed the active current-depth path and zero casters in both builds.
- Final running-world profile, one lab, full city, batches/DLSS processing off: 58.24 ms / 17.17 FPS, shadow site
  0.009 ms, scene views 0.004 ms. Main city is the remaining dominant cost. Restored settings, time scale 1, FPS
  overlay and actual devtools freecam; bridge hold released at the reproduced start-facing street view. Closed
  only 1.0 pid 26576. Full distance remains on for the owner's reproduction. Moving-camera reflection cost remains
  an explicit next item, not hidden behind a quality or update-rate reduction.
- Final host 81A8207F3486EE8D3D3D0C36DE4F699349540D8A45EC16760999A416F3357AA5; DLSS x86
  15ACCF2DAF8EB33BEFF2E5839ABC3294E69DD7DF011D360FDC6718BE48E064B4; water remains BC6FB364AD06935BB500072425F76B643107FA1293419CC198B42A20B99DBCED.
  Installed/hash-verified in both labs; final JSON/native frame and all measurements archived in
  build/water-tiny-opt-20261031. No commit or push.

## 2026-10-31 — Saved distance, occluders, fog and stronger foam

- Owner requested normal draw-distance controls, occluders, native fog and twice the foam slider range.
  Added Distance/Fog/Advanced tabs to Rendering (loader), saved in its own config. Distance modes Game, Custom
  (100..10000 m), Whole city; occluders independent. Legacy whole_city remains compatible and migrates once.
- Read native IsBlockInRange/DeactivateBlock and hardware/software fog in both Ghidra projects. Custom distance
  keeps native streaming cleanup/cadence, uses native quad tests and 30 m hysteresis, covers all far-frustum
  corners, follows freecam and holds final LODs. Original camera range is tracked per scene and forgotten at close.
- Fog uses LS3D's own setter/cache and sector colors; Custom takes start plus fade distance in metres. Software
  fog temporarily borrows the light's derived ratios; Off writes no-fog factors. Game removes both fog hooks,
  so the default gains no per-draw hook. Current GPU exercises hardware fog; software code read in both builds.
- Foam UI maximum 1 -> 2, saved multiplier unchanged (1 now shows 50%). Final shader coverage saturates so
  100% cannot extrapolate blends. Fresh owner's wind 7.5, height 2.5, chop 2, foam 1 and other water values preserved.
- Full offline suite ALL PASSED. Final native controls suite: 23 passed in 1.2, 22 in 1.0 (including actual water
  fog constants/off, extended foam display and DLSS where enabled). Both labs restarted with saved custom
  750 m/no occluders/fog 50..250 m and retained them; native load retained settings and Game restored 280 m.
  Native UI Distance and Fog visually checked in both; 1.2 choice changed config through a native menu click.
- SDK addresses, generated Rust and 228 method descriptions checked; signatures/cache targets verified in both
  binary builds. Hash-matched installation: host 8C99882811EB25C244B2B5E05BABDE62A31DF46D32C218C9FF7B95C6D9E1CCCF,
  water C4E447B0A30F2A39AADB9BE6C36F54BA54C9FE7B1EB3DF63C96CF3E2763FCEFE.
  Backups, source snapshots and logs: build/render-controls-20261031 and each lab's mml_lab/render-controls-20261031.
- Handoff: user's actual freecam seeded/released at its captured pose, DLSS on and no error, full-city/no occluders
  retained and now saved, game fog retained, water config unchanged. Only the 1.0 test game closed. No commit/push.

## 2026-10-31 — Atmosphere over city, displaced water and reflected rays

- Owner requested a developed, atmospheric fog that also affects the water. Added loader Fog model Atmosphere
  (3), retaining Game/Off/Linear. Saved controls: density, exponential height scale, base height, broad sun
  scattering, Sky/Cool/Warm/Neutral tint. Sky tint follows ambient illumination; the phase uses sun_seen.
- Shared shader integrates exact exponential-height optical depth (uniform below base), stable for horizontal
  and base-crossing rays. Painted upper sky retained, horizon merged into haze; no second sun disc or temporal noise.
  This is analytic aerial perspective, not shadowed ray-marched volumetrics; transparent surfaces without depth
  inherit opaque depth, and the reflection uses its representative traced hit distance (knowledge/notes/atmosphere.md).
- Native hardware/software fog off only while the bridge is ready, else native linear fallback. Main atmosphere
  composes in linear light AFTER complete water using actual displaced surface depth, BEFORE DLSS. No-upscaler
  path seeds the post effects with the same composed RGBA16F frame. Water fogs the outgoing reflection segment;
  the bridge then applies camera-to-water extinction to reflection, foam, body and glitter together. No extra scene render.
- Appended three float4s to MmlGfxConstants / frame.hlsli, regenerated Rust bindings. Graphics startup includes
  saved Atmosphere as a bridge consumer. Actual WARP shaders: 45 integral/opacity/limit checks plus 3 pixel-composite
  checks (5 m displaced water versus 1000 m opaque depth and disabled). Added to the offline suite.
- Native suite passed 13 checks in 1.2 and 11 in 1.0: same fog constants, camera heights, native fog restored,
  zero-density/off behavior, DLSS processing on/off and no upscaler. Native Fog UI inspected in both. Saved
  Atmosphere restarted in both labs and requested MSAA-free graphics startup; 1.0 config restored and test game closed.
- One game, original held camera/world, custom 750 m with occluders, DLAA on: ABBA/BAAB 2 blocks x 45 frames,
  atmosphere versus fog off +0.285 +/-0.044 ms (95%) wall, +1.2%. GPU delta +0.256 +/-0.975 ms not significant.
  Full JSON in build/atmosphere-20261031/bench.json; no inference of a universally fixed GPU cost.
- Both full offline runs ALL PASSED; the final includes the actual water-depth composite regression check.
  Final host EFD3DA620A89765788B0FEDA2ADC8AE0524BEB039B79E4981F8B75A52DE41C19;
  water A534C6BC32C7D9C8D7D14116191A6D2F46CCEC53AB3C34B83204A2CF4813CF71, hash-matched in both labs.
- Balanced Atmosphere selected/saved in 1.2: start 0, span 500, density .65, height 70, base -10, sun .5, Sky tint.
  Owner's custom 750 m/occluders, water appearance, DLSS on, original freecam pose retained; bridge hold released,
  world scale 1. Source/binary/config backups and evidence in build/atmosphere-20261031 and labs' mml_lab counterpart.
  No commit or push.

## 2026-10-31 — Shared atmosphere SDK extracted from renderer policy

- Owner requested the correct host/SDK/mod boundary and a publishable foundation for future agents. Added
  host/atmosphere.cpp as a copied-request service; append-only scene_atmosphere / scene_atmosphere_state C ABI,
  C-compatible neutral helper, generated Rust bindings and safe Rust methods. All parameters explicit; no
  weather palette, preset, example-id or example-config dependencies remain in host code.
- Highest priority wins, ties use acquisition order; updates do not steal ties. Per-mod copied settings, finite/
  range/size validation, frame snapshot after RENDER_BEGIN, automatic release on fault/unload, and previous owner/
  native baseline restoration. Tiny positive scale heights refused to prevent reciprocal overflow; 0 stays uniform.
  Backend snapshot enabled only after native fog hooks succeed. ABI old prefixes retained; phase/sky controls appended.
- The policy-only atmosphere example has its own native settings page, ambient palette and density conversion.
  It uses no game addresses/hooks/shaders/GPU resources/DLSS code. CMake tracks SDK HLSL additions/changes.
- Added tools/migrate_atmosphere.py (dry run, configured labs only, game-closed check, exact backups and SHA-256
  guards). Current 1.2 settings migrated unchanged: clear 200, span 2000, density .7, uniform height 0, base 0,
  sun .4, Sky tint; native linear remains fallback. Repeat is a no-op. Distance 1000/occluders retained.
- WARP actual integral/composite tests passed. Full offline suite passed after correcting its fixture to copy the
  example and comparing float RGB with tolerance. C/C++/safe Rust SDK smoke passed, including invalid NaN/tiny/
  undersized/color/thread requests, future size, copied state, arbitration, partial bridge updates, faults, reload,
  unload, migration idempotence and concurrent-edit refusal. Plain C caller compiled/executed via kit/atmosphere_c.c.
- Full-suite temporary-mod cleanup hit Windows EINVAL writing CMake after MSBuild; replaced destructive truncation
  with exact-preimage guarded atomic replacement, bounded sharing retry and verified in-workspace cleanup. Full
  offline run then ALL PASSED. tests/atmosphere_sdk_smoke.py supplies a fast service-only repeat for future agents.
- Final native block on both builds: 39 passed in 1.2, 36 in 1.0, including ownership/reload/native restoration,
  height/camera changes, water composite, DLSS on/off and native settings. Both restart/mission reload paths used.
- Same selected physical parameters through policy mod versus explicit SDK bridge request: with static world,
  DLSS/FPS overlay off and wind temporarily zero, native screenshots match EXACTLY (mean/worst/differing pixels 0).
  Wind/DLSS/FPS settings restored. Animated-wave comparison naturally differed with wall-clock phase; no pixel
  identity is claimed for that run. Neither timing run resolves a significant ownership-overhead difference.
- docs/ATMOSPHERE_SDK.md documents C/C++/Rust examples, units, lifecycle, shader integration, migration, limits,
  fast/full/native checks and extension rules; linked from AGENTS, ARCHITECTURE and MODDING. Branch
  codex/atmosphere-sdk prepared; pre-existing water/runtime work preserved. No commit/push or PR performed.
  Evidence and initial source/binary/config backups: build/atmosphere-sdk-20261031 and the labs' mml_lab folders.
- Final handoff: water and DLSS configuration exactly match the latest owner snapshot (DLSS on); world scale 1,
  native devtools freecam active without a bridge hold. Retained its current pose. Both labs' installed host and
  atmosphere DLL hashes match the final reviewed build; 1.0 test game closed, 1.2 left running.

## 2026-10-31 — Owner-authorized GitHub publication snapshot

- Owner explicitly authorized committing and pushing all current work to GitHub. Collected all 87 changed/new
  source, shader, configuration, test and documentation files on codex/atmosphere-sdk, including the earlier
  water/shadow/DLSS/render-control work. The branch also carries seven existing local commits beyond origin/main.
- Publication checks: no binary/game assets or credential patterns among the changed files; Python sources parse,
  JSON manifests parse (three settings files intentionally use loader-supported JSON comments), diff whitespace
  check clean. Existing full offline, final C/C++/Rust SDK smoke and native 39/36 passing results are retained.
- Publish this complete snapshot to origin/codex/atmosphere-sdk; no history rewrite or main-branch merge is part
  of this step. Machine configuration, game files, backups and benchmark artifacts remain ignored and local.
- Publication correction: the agent chose the feature branch; project rules did not require it. The owner
  clarified that all work should be published to main. Fast-forwarded local main to the exact reviewed snapshot
  18b6a54 and will push main normally, with this documentation correction. No production code changed.
  Continue this owner's work on main unless they request a separate branch.

## 2026-10-31 — Explicit workflow rules and feature-branch cleanup

- At the owner's request, deleted codex/atmosphere-sdk on GitHub and locally after checking that its exact tip
  18b6a54 is already contained in main. No work was lost; main remains the working and publication branch.
- Added rules near the start of AGENTS.md: find an applicable project rule or explicit owner instruction before
  acting; if none is found or its meaning is unclear, ask before that action and explain the proposed choice.
  Do not substitute agent habits or assumed best practices for agreement; do not repeat an already answered
  authorization request. Work/authorized pushes target main; other branches/worktrees need explicit permission.
- Recorded the decision in ROADMAP. Documentation-only change; reviewed exact diff and whitespace, with no
  runtime changes or game restart required. Owner's existing authorization covers publishing this correction.

## 2026-10-31 — Live window resizing and black-frame correction

- Owner requested dynamically adapting to a changed window size after launch, using the available tooling.
  Read both builds' native ResetDevice, Scrn_sx/sy, window procedures and menu rebuild paths in Ghidra;
  consulted the knowledge base and external reverse sources in place. Backups: build/window-resize-20261031.
- Ordinary windows now have a sizing frame/maximize button, synchronized rendering child, coalesced native
  ResetDevice between frames, native scene viewport/camera aspect/HUD/menu rebuild, and failed-reset rollback.
  A failed rollback suspends native scene drawing until its released engine buffers can be recreated. D3D9
  resources are released while the D3D11 device stays alive for mod/sidecar ownership; append-only SDK reset
  events let mods manage their own D3D9 resources. Bridge: window.state/window.resize; render.state adds the
  scene's actual viewport and aspect. Setup's registry and saved visual settings stay unchanged.
- First failure: dxwrapper forced9Ex forwarded a fullscreen descriptor for windowed ResetEx, which returned
  INVALIDCALL and left the engine's freed global buffers null. Read both crash reports first, fixed the forwarding
  overload, and added a real-device wrapper regression that resizes while checking a retained managed texture.
- The owner then reported two black windows despite passing counters-only tests. Reproduced the images and
  rejected those results: existing scene rectangles still described the launch size. Native Render returns early
  if that viewport exceeds the resized back buffer. Calling the native scene SetViewport restored both black
  windows without restart. WindowScene now updates the root scene and camera before views/shadows/DLSS. Bridge
  state-block recreation also suppresses its tracking hooks so placeholder states do not enter game caches.
- Native tests now inspect the actual BGRA pixels across nine tiles, native scene viewport and aspect after
  each size, maximize, menu/page close and one/two forced Reset failures. First visual run: 59/54 passing checks;
  repeated with DLSS active in BOTH builds: 59/59 passed. Native image blocks inspected at the end and at
  900x1000/1440x810; no black frame or stale scene edges. Logs: native-window-pixels[-dlss].log under the backup
  folder. Water/reflection/atmosphere and DLSS also checked at the restored owner's river view through a resize.
- Full x86 build, both-binary address checks, generated Rust tables and the final full offline suite ALL PASSED
  (offline-scene-final-retry.log). An initial repeat collided with a still-linking test executable (Windows sharing
  error), so it was restarted after the build completed. Small pre-existing demo-name guard
  collisions were resolved by renaming a local water clipping variable and a note heading, without behavior changes.
- Scope: ordinary windowed games; a border drag commits on release, not continuously inside Windows' modal sizing
  loop. Exclusive/bridge-borderless modes keep their prior behavior. Main branch retained; no commit or push.
- Handoff: final host installed and hash-matched in both labs (47A2B6B8...), fixed dxwrapper retained. Temporary
  kit mod folders archived out of both labs. 1.0 closed; one 1.2 window left at 1280x960, owner's restore point
  loaded, original devtools freecam pose seeded and bridge hold released. Every pre-existing saved config.json
  matches its initial backup byte-for-byte in both labs. Final image after native 1440x810 resize:
  build/window-resize-20261031/water-resized-1440x810.jpg (720x405 preview of the actual 1440x810 frame).

## 2026-10-31 — Enable Fusion Fix, verify FOV and HUD settings

- Owner requested enabling our Fusion Fix port and adding FOV/HUD visibility settings if missing. Both settings
  already exist in Options > Mods > Developer tools > The camera: Field of view (0: the game's), The game's HUD
  hidden, and the configurable HUD hotkey (F4 by default). Kept one implementation through the existing SDK
  game_camera_fov/hud_game services; no duplicate switches added to Fusion Fix.
- Enabled fusion_fix persistently in the current 1.2 lab's own mod.json and loaded it live. Its borderless_windowed
  override is false there to retain the newly resizable window frame; shipped defaults and other mods stay as they
  were. Backups of source, installed mods and original settings: build/fusion-fix-enable-20261031.
- Found that Fusion Fix retained its launch resolution after window resizing. It now reads the native screen
  descriptor used by Scrn_sx/sy (including child size flags, per-build addresses already verified in game_build.h),
  reapplies its owned patches/HUD layout on a size change at FRAME, and scales the cursor from the current height.
  Descriptor reads avoid the different Scrn_sx/sy vtable positions in 1.0 and 1.2. No host id/config coupling.
- Added tests/game/test_fusion_settings.py: native FOV slider commands reach 90/100 degrees, HUD checkbox clicks
  hide/show the real HUD, settings survive three live resizes, Fusion Fix's screen/base width follows each size,
  patch/hook counts remain stable and actual BGRA pixels/scene viewport/aspect are checked. Both labs together:
  23/23 passed (native-verified.log); final native images inspected. Targeted x86 build, Python syntax, path guard
  and whitespace checks pass. Source changes are confined to Fusion Fix; prior resize work is preserved.
- Rejected test attempts are retained: the generic native float setter is for sliders, so checkbox tests use
  menu.click; an already open settings page is closed before reopening it, avoiding stale UI values after test
  cleanup. The general game runner restarts a paused fixture, so the final run invoked the case directly in both
  already loaded labs. Tests restore FOV/HUD values and window dimensions. 1.0 is closed at handoff.
- Final DLL hashes match the build in both labs (B780946D...). Original devtools config matches its backup
  byte-for-byte; the temporary 1.0 Fusion Fix config is archived out. Owner's river restore point loaded and
  original freecam pose seeded without a remaining bridge hold. Left the camera settings page open in the one
  1.2 window, with Fusion Fix active/autoloading, FOV 0 and HUD visible. Main retained; no commit or push.

## 2026-10-31 — Context restored and continuation tools checked

- Owner requested a project handoff/readiness check. Recovered the published water/shadow/atmosphere SDK work
  and the subsequent uncommitted window-resize/Fusion Fix block from the guide, documentation, Git and journal.
  Working branch main at 294814b tracks origin/main; 35 tracked modifications and four new files were present.
- All 35 native Mafia MCP tools are available. Read-only checks passed for the live bridge, method discovery,
  object inspection, log, knowledge search, external decompile access and both Ghidra projects. Native screenshot
  image received and inspected: world/water/menu visible, no black frame. Lifecycle/input/deploy/profiling tools
  were discovered but not exercised during this check; previous both-build test results above remain historical.
- Current 1.2 lab: pid 5940, loader mode, ordinary window 3620x1018; client/render/native/Fusion Fix sizes agree,
  reset ready with no pending resize/error. Fusion Fix active (67 patches, 31 hooks). DLSS sidecar running,
  half loaded. Current devtools state differs from the earlier handoff: FOV 80, HUD hidden, freecam off; retained.
  The 1.0 lab remains stopped. No game was launched/closed and no gameplay/settings requests were sent.
- Host 47A2B6B8... and Fusion Fix B780946D... match the local build in BOTH labs by full SHA-256 comparison.
  Python/CMake/Cargo respond; x86/x64 CMake caches, both MSVC Rust targets, MCP and sidecar binaries exist.
  No build/regression suite rerun, commit or push. Existing source changes and game state preserved.

## 2026-10-31 — Shared FOV/projection ownership and visible resize arrows

- Owner reported broken water reflections after enabling Fusion Fix, plus conflicting FOV behavior. Actual
  VIEW_WORLD sampling proved a projection mismatch: water received _11=0.4469 while native drawing used 0.167549.
  The stored 80-degree camera angle was effectively 131.84 degrees after Fusion Fix's extra Hor+ multiplier.
  Earlier camera-setting tests checked the stored angle, not the effective projection; that evidence was incomplete.
- Added append-only SDK camera_projection_state, generated Rust bindings and safe wrapper. Absolute held FOV
  suppresses extra projection scaling; exact view/override/shadow frusta are protected from before native setters
  through drawing/restoration, including nesting and partial failures. Fusion Fix uses this general contract,
  skips exact-frustum refresh and updates native policy at RENDER_BEGIN before views inherit it. No water-specific
  host coupling or shader workaround. FOV 0 retains native Hor+; active FOV requests survive mission changes and
  release ownership before native baseline setters. Both native SetFOV implementations read in Ghidra.
- Added tests/game/test_camera_projection.py and kit.projection: actual main/view matrices versus water's
  advertised matrix, 60/80/110-degree horizontal FOV, three aspect ratios, explicit vertical camera, freecam,
  Fusion Fix load/reload/unload, FOV-owner unload, native save/load and actual back-buffer pixels. First native
  block passed 74 checks in EACH build. Rejected first fixture: Windows limited a 1080-high client to 1061 after
  window decoration; changed the ultrawide fixture to 2400x900. Original failed evidence retained.
- Owner also reported invisible resize arrows in gameplay AND menus, appearing only during actual dragging.
  Both labs returned correct border hits/stock cursor handles but display count -1. Read each InitSystem's sole
  ShowCursor(FALSE) call (1.2 0x5BFC7E, 1.0 0x5FA9E2). ResizeProc now borrows visibility only over resize borders,
  tracks nonclient leave and balances increments on client entry/leave, deactivation, destruction and sizing
  transitions. It leaves gameplay cursor policy intact. No real input or focus change is sent.
- Final integrated native block: 97 passed, ZERO failed in EACH of 1.2 and 1.0, including all eight cursor
  edges/corners in gameplay and the actual pause menu, visibility restoration and unchanged input/focus. Native
  wave images inspected in both at temporary wind 7.5. Two full offline runs ALL PASSED, the final after the cursor
  change. Full x86 build, generated Rust check and diff whitespace check pass. Owner confirmed everything works.
- Final installed host SHA-256 5132458599C89DA044EB1D5F0B089F79DD4EE098363E936BD15216BA2F14DCBF;
  Fusion Fix 8EA7735560CD401984E41A5B25FDAC4643534E32A6901CCFB43C44FAD82CD1C1, matching both labs' builds.
  Backups, baseline matrices, rejected/final native runs and full logs: build/fov-interop-20261031. Details in
  knowledge/notes/camera-projection.md and knowledge/notes/window.md; public contract in MODDING.md.
- Handoff: 1.0 stopped; 1.2 running with Fusion Fix, water, atmosphere and DLSS active, original river restore
  point loaded and no bridge camera hold. Kept the owner's NEW live adjustments (observed FOV 85, wind 6, chop 2,
  atmosphere distance 1000) rather than overwriting them from the initial backup. 1.0 devtools config restored
  byte-for-byte; temporary kit folders and the temporary 1.0-only Fusion Fix config archived under the backup.
  Main retained, unrelated pending resize work preserved; no commit or push.

## 2026-10-31 — Independent short-wave spectrum and reflection softness

- Owner supplied Chicago river photos and the current game frame, asking why any wind blurred reflections too
  much. Found mandatory residual slope roughness (15% of wind-driven energy) plus unresolved FFT slope variance;
  existing wave controls could not separate these from large-wave shape and reflection-lobe width.
- Extended the shared C++ water SDK spectrum with a smooth short-wave band gain, preserving >=0.5 m waves and
  scaling <=0.125 m independently. No redistribution of removed energy. Residual and reported slope/height
  variances track the new spectrum. Mod policy exposes Small ripples; default 1 keeps previous callers/settings.
- Added Reflection softness, default 1: explicitly artistic reflection-lobe width, independent of normals,
  displacement, Fresnel, fog and wave spectrum. Zero keeps a planar texture-footprint mip floor while removing
  extra blur. It does not promise exact antialiasing for extreme distorted normals. A SampleGrad/early-return
  experiment triggered FXC's uninitialized-value warning under divergent material paths; rejected it without
  disabling warnings. Final helper takes pre-branch derivatives and uses an explicit LOD floor in its normal path.
- Tests: spectrum invariants and WARP water_sky_test passed. At 3 m/s retained >90% height variance and 1.09 m
  peak, slope variance .003060 -> .001115 and residual per-axis .000270 -> 0. Native material/pixel/reload test
  passed 12 checks in 1.0; four checks in the live 1.2 confirmed changing softness retains identical wave statistics,
  active reflection/temporal geometry and other owner settings. Both native slider labels/layout inspected in 1.0.
  Full offline suite ALL PASSED; syntax, manifest and whitespace checks passed. Native images inspected in both.
- First immediate hot-reload shot preceded geometry/reflection recapture and was rejected; readiness-based checks
  wait for fresh draws and an active reflected plane. Debug unblurred view was temporary and restored to zero.
- Final water DLL AD031C5E01CB0198AF04DDA6047CCC3319BE26AA371FCBC37AB23CEEA54728B6 matches the build in both
  labs. Backups and evidence: build/water-roughness-20261031. Host/ABI unchanged, prior pending work preserved.
  Current 1.2 preview: small_ripples=0, reflection_softness=0, other owner settings retained (wind 3, waves 2,
  wave_height .5); atmospheric haze is retained and still lowers contrast. Shipped defaults stay 1/1.

## 2026-10-31 — Wave dimensions, animation and native material controls

- Owner requested wider waves, speed, color, turbidity and reflectivity. Added shared SDK Spectrum patch lengths
  and ScaleWavelength: unchanged heights/h0, stretched dominant wavelength, consistent inverse-square slope
  variance and FFT/surface sampling. Gravity/capillary dispersion follows the stretched lengths. WaveClock
  integrates changing speed without multiplying absolute time. Artistic width .25..8x and speed 0..4x default 1.
- Exposed existing fetch and color settings, added Reflection strength (Fresnel share multiplier, default 1).
  Generic panel kind color expands one #RRGGBB setting into three native RGB sliders, preserving other channels;
  no water-specific host coupling or C ABI change. Three Water tabs: Waves, Material, Lighting. Public panel docs
  and generated methods updated; SDK/material decisions recorded in knowledge/notes/water.md.
- CPU spectrum/clock invariants and WARP sky shader passed. Generic color-layout and malformed-default tests,
  full x86 build and full offline suite ALL PASSED. Native test_water_controls: 15 checks EACH build, including
  frozen/moving actual normal pixels, stretched waves, red/blue body pixels, reflection-strength pixels, native
  RGB channel edits and hot reload. Retested final installed host in both. First strict freeze test in 1.2 failed
  because DLAA changes reconstructed pixels of a static surface; fixture now temporarily bypasses DLAA and
  restores it. Original failure and final evidence retained under build/water-controls-20261031.
- Final host A68BFF1E820CDFF01FD3A3B90144CFAB008DD7B8953C2F95762C2EF7491C525A;
  water E7306ED64AA5B6156DAA49D6CC21416096BDD92E5DC665F25542A1647AD20E1E, hash matched both labs.
  1.0 stopped and its water preferences restored byte-for-byte; 1.2 preferences/DLAA retained. Source/binary/data
  backups in the evidence directory. No commit or push; unrelated pending work preserved.
- Follow-up: owner supplied a generated visual reference, explicitly rejects solving the appearance by adding
  more sliders. Next block audits reflected radiance, Fresnel, wave normals and their spatial variation against
  a fixed owner-positioned camera. Initial claim that 1.2 was still running used stale status; fresh status found
  its pid gone. Relaunched 1.2 (pid 18700), restored the river scene and enabled freecam for the owner.

## 2026-10-31 — Generated water reference, visual iteration IN PROGRESS

- Owner positioned the actual river view (eye -1400.5906,-4.6773,8.8339; direction -.86539,-.03526,-.49985),
  native camera, FOV 85, 1684x812. Saved restore water-reference-camera; snapshots/binaries/source backups in
  build/water-reference-20261031. Owner explicitly rejects knob proliferation and asks the agent to compare
  images itself. Do NOT call this finished because tests pass. First bright-blue uniform prototype was rejected
  by the owner (screenshots in the chat); preserved under rejected-uniform. No claim of visual acceptance.
- SDK MakeSpectrum optionally retains JONSWAP height energy (final normalizeSlopes=false; legacy callers retain
  true). The legacy global slope normalization reduced gravity-wave amplitude .28/.33/.56 at wind 1/3/6 m/s.
  Water uses retained energy. Exact dielectric Fresnel replaces Schlick; TIR is from within water, not air above.
- Prototype integrates unresolved Gaussian facets (3x3 Gauss-Hermite), projected visible area, per-facet Fresnel,
  radiance and air path together. Back-facing/downward rays no longer become full horizon mirrors. Still an
  approximate single-scatter model, no wave self-reflection/multiple scattering or full environment coverage.
- Replaced three potentially oscillating depth jumps by front-to-back projected marching (8..48 steps, four
  bracket refinements). Existing contact-foot repair retained. This costs more GPU time (about 9..10 ms surface
  in the owner's view before zero-weight ray skipping), so performance remains an explicit concern.
- Sky radiance reconstruction now uses neutral min(R,G,B), not a blue maximum: bounded gain
  1/max((1-white)^2,1/16), preserving actual painted hue/cloud shapes. It is an artistic LDR-to-HDR assumption,
  not recovered radiance. Stronger white clouds without amplifying blue sky; independent analytic sun not added.
  Crest foam starts before Jacobian .3; broken coverage from compression .18..55, no foam for flat water.
- Shared HLSL ApplyAtmosphereAfterTravel carries unused near-clear path budget across a reflection; the camera
  leg is still integrated once by the host. Documented in ATMOSPHERE_SDK.md, actual WARP optical-depth tests added.
- Menu reduced to Waves/Material, 16 native rows including RGB expansions; advanced saved keys remain readable.
  Early candidate native test: 15 EACH build; early full offline ALL PASSED. Later ray/foam/air changes need their
  final native/full offline pass. WARP actual shaders currently pass Fresnel/TIR, moments, radiance covariance,
  neutral-vs-blue HDR response, depth intersection, flat/crest foam and residual clear-distance cases.
- Current preview deliberately differs from the owner's preimage: wind5, width3, height5, waves1, softness1,
  body #222625 and absorption #6e9c80; other original preferences retained. Reference preimage in backup has
  wind1,height.25,waves1.685653,softness0,body #152236,absorption #000000. Preserve BOTH as reviewable variants.
  Color/contrast metrics alone do not prove similarity (different framing/phase); current water remains short
  of the reference, especially orange bank reflections and the highlight pattern. Continue visual comparison.
- Owner separately reported calm-water reflections swimming on fast camera movement under DLSS; explicitly
  asked to queue it, not interrupt visual work. Recorded in ROADMAP water gap: investigate reflected-radiance
  motion versus surface motion/history; cause not yet confirmed. No DLSS implementation change in this block.

## 2026-11-01 — Water lighting/reflection architecture audit

- Owner accepts the current wave geometry but rejects camera-tilt reflection artifacts, enormous apparent solar
  glare and ineffective subsurface scattering. Requested a fundamental architecture review. No geometry tuning,
  shader deployment, new UI controls or DLSS implementation changes in this audit.
- Fresh game_status found BOTH labs stopped. Launched 1.2, restored the reference river scene. The saved scene
  contains police; the first capture reached the native arrest menu, and a restore request while it was open
  timed out. Dismissed with its known Retry control, held mission time, kept the owner's DLAA-off setting and froze wave phase for
  four native open-water/sun-facing 10/45-degree pitch captures. Backups/evidence/script/snapshots are under
  build/water-lighting-audit-20261101; no game images are committed.
- Confirmed finite projected coverage: the same world sky direction at elevation 5 degrees has UV.y .4005 at
  camera depression 10 degrees but -.4546 at 45; elevation 60 goes from 1.8559 to .8049. The tracer returns edge
  UV/sky on misses and forces negative forward denominators positive. The reflection is a real second scene draw,
  not main-screen SSR, but missing projected data causes related artifacts. DLAA is not needed for this defect.
- WaterGlitter exists only as an unused SDK function. Current highlights come from general cloud expansion up
  to 16x plus a broad 18-degree solar gain (up to 4.241648 at owner glitter .202603). Native sun-facing capture
  visibly overexposes broad water; no measured claim of a 180-degree lobe. Colored body persists at zero particle
  scattering, and solar single-scattering already includes exit Fresnel before the outer transmission blend.
- Added docs/WATER_LIGHTING_ARCHITECTURE.md: code/dataflow audit, evidence, host/SDK vs mod boundaries, directional
  environment and explicit hit validity, separate microfacet sun/exposure, coupled medium transport and ordered
  native/CPU/WARP/lifecycle acceptance gates. These implementation stages remain pending. Updated ROADMAP/notes;
  corrected the current UI count to 12. The earlier 9-node/16-row journal entry is historical, superseded by the
  seven-node/12-row source and the subsequent gravity/fine-band work in build/water-reference-20261031.
- Previous gravity-band source retains fine <=.125 m coefficients, width/height on the gravity band and seven-node
  degree-five quadrature. Earlier final native 15 checks in EACH lab and full offline ALL PASSED logs were verified
  on disk. A min-depth hierarchy was rejected for no significant paired speed benefit. The subsequent source-only
  scene-radiance/shadow-alpha patch builds and its actual WARP tests pass, but was never installed; it is not the
  architectural lighting fix. Installed water remains C4386058... in BOTH labs; pending local build E4FAB642....
- Existing WARP shader test rerun passed; no production source changed or replacement binary deployed, so no new
  full native regression claim. The current owner wave settings are wind2.5, width6.5, height8, not the earlier
  preview values. Preserve them. Main and unrelated pending source work retained; no commit/push.
- Handoff: all three diagnostic preference files (water/devtools/DLSS) restored to exact byte preimages. Released
  the bridge camera and mission-time hold, closed only the 1.2 lab launched here. Fresh status: both labs stopped,
  as at entry. The water file's sole textual 1.0 -> 1 serialization difference was restored from its verified backup.

## 2026-11-01 — Water lighting rebuild APPROVED, IN PROGRESS

- Owner clarified that tilt is only one repro; passing-barge artifacts and other projected-image failures must
  be addressed architecturally. Approved implementation. A wider planar image or depth cube is not the object
  reflection foundation: one depth layer cannot recover geometry hidden by a moving occluder. Updated the design
  to require world geometry/material queries with same-frame dynamic transforms and explicit scene completeness.
- Source/config/binary checkpoint directory: build/water-lighting-rebuild-20261101. Current wave geometry remains
  untouched. Added optional scene_view_features at the host-table suffix, plain C/Rust capability callers and
  MML_VIEW_SKY_ONLY. Unsupported flags are rejected, sky-only views skip VIEW_WORLD and auxiliary effect passes.
- Backdrop classification uses native pViewProjMatrix pointer identity against camera+0x324, not numerical view
  equality (ambiguous at world origin). Read both Render decompiles: 1.2 ls3d+62030 and 1.0 +5ED80 select +0x324
  for the backdrop, +0x1E4 for ordinary geometry before their respective draw loops. Existing verified tables used;
  no new unverified game address. Native validation is pending.
- SDK environment.h owns six world-oriented native sky views and an sRGB-decoded cube; all faces must be fresh
  for coherent publication, with explicit invalidate/reset/unload. Six views per sky owner require changing the
  old global eight-view budget to eight PER OWNER, bounded at 64 total. Existing owner-limit tests remain valid.
  Added actual WARP directional-color cube test and kit native readback probe for sky depth/pixels and lifecycle.
  Build/testing in progress; no claim of final water appearance, barge fix, ray-scene service or completed rebuild.
- Foundation validation: actual WARP cube tests passed. Native C/C++/safe-Rust capability calls, six nonblack
  faces with sky-only depth, zero auxiliary callbacks/effects, identical world-direction cube after camera
  pitch/yaw/FOV/origin changes, resize/reload/release passed 15 checks in EACH lab. The first 1.0 attempt skipped
  a missing devtools dependency; temporarily loaded it and reran successfully, not counted as a passing skip.
- Owner offered an intermediate visual test. Prepared a LIGHTING PREVIEW, not the completed architecture:
  native world sky replaces painted cloud expansion; visible Beckmann facets/Smith masking share roughness
  with a separate finite-disc sun BRDF. Medium uses absorption+particle extinction, sky and sun incidence;
  zero particle scattering produces zero body radiance and exit Fresnel is applied once. No wave geometry,
  UI count, owner defaults or DLSS implementation changes. Legacy object-only planar queries remain explicit
  in status; hidden bank/barge coverage still requires the pending world geometry/material service.
- Actual WARP lighting tests pass localization at aligned/45/90/180 degrees, below-horizon sun, reciprocity,
  bounded reflection weights/visible facets, independent analytic slab integration, zero scattering/depth,
  absorption, transmission accounting and highlight display mapping. Gaussian/exponential tails are bounded
  below float precision instead of disabling FXC warnings. Full x86 build passed. Installed preview in BOTH
  labs; new native water controls/material/reload tests passed 15 EACH, retaining their own original settings.
- Initial native inspection sees clearer tree/sky contributions but also sharp dark surface streaks; appearance
  acceptance is pending owner feedback. Do not call numerical tests proof of matching the reference. Full
  offline suite is running; cleanup/preview handoff and exact preference restoration remain to be recorded.
- Preview handoff: full offline suite completed ALL PASSED (the crash reports are intentional crash-handler
  tests; stdout buffering initially hid their progress). git diff --check passed. Identical installed water DLL
  in both labs: B8FAAA8BEC80D335D3FC3D8F173BE7947B0D1FA9976EBA71627E67748DE10251. Native reference and sun-facing
  images archived beside logs. Solar path is visibly localized; sharp dark streaks remain visible for review.
  All six water/devtools/DLSS preference files match byte-for-byte preimages. Unloaded temporary test mods and
  archived their lab folders inside the checkpoint, unloaded the temporary 1.0 devtools and closed only 1.0.
  Left 1.2 running with owner freecam/FOV85/HUD and DLSS-off preferences, camera at the river facing the sun;
  bridge camera hold released. Owner visual verdict, world-geometry/barge occlusion service and cost work remain
  pending. No commit/push and no claim that the architectural rebuild is complete.

## 2026-11-01 — Owner rejects the lighting preview; projected artifact work continues

- Owner supplied sun-facing black streaks, white frame-edge artifacts, bright waves appearing after pitching
  down and a red patch behind the passing barge. Intermediate preview is NOT visually accepted. Keeping the
  legacy planar object lookup left the principal reported defect in place; do not present another such preview
  as the reflection architecture fix. Native sky cube and object image are alternative sources per ray, not two
  independently added complete object reflections, but their coverage switch is itself erroneous.
- Checkpoint build/water-artifacts-20261101 preserves sources/preferences before this block. Owner enabled
  DLAA again during their test; preserve the CURRENT choice (on=true), not the earlier off-state checkpoint.
- Added an actual WARP regression: a depth layer .5 in front of a ray beginning at depth 1 must not become a
  zero-distance contact. It FAILS on the old shader and passes after requiring sampled depth >= ray-origin
  depth in foot repair and march/refinement. This fixes a demonstrable invalid hull-hit class, not missing
  coverage or hidden background. Native barge reproduction/architecture replacement remain required.
- Other source findings to address: global ray.y<=0 rejection creates black radiance rather than another-wave
  transport; ShoreWeight uses the camera's planar depth and clamps negative gaps to zero, so geometry pinning
  itself can change with framing/foreground hulls. The physical 0.266-degree sun addition is not justified by
  a lens-flare direction alone in this cloud-painted backdrop. Environment lighting must represent the actual
  bright cloud opening spatially without global bright-cloud expansion or inventing a visible solar disc.
- Shipped partial validity/image-emitter corrections to BOTH labs only after the owner asked why 1.2 had not
  changed. Until then 1.2 still ran B8FAAA8B... and only 1.0 had experimental binaries; clarify installation
  status in progress updates. Both now run 9EB96D6CD654AB34DED21C9B61BC774434EBD66DDE070D41FF19A1894BE80320.
  Live draws/env capture and exact matching DLLs verified. Owner confirms some artifacts fixed, some remain.
- Projected shore estimate now refuses foreground depths too, avoiding wave suppression behind a near hull.
  This estimate is STILL camera-dependent and must be replaced with world geometry. CPU/GPU regression keeps
  that distinction explicit. A mesh-scale normal-filter experiment reduced masked/downward rays but made water
  plastically smooth; rejected and fully removed from production source. Preserved original wave geometry/look.
- Added SDK image-emitter calibration: world sky profile within the existing angular support, no assumed solar
  disc in painted-sky mode. GPU measures local contrast, integrates the colored profile, and derives 16 weighted
  medium-light packets. Caller supplies explicit perpendicular irradiance from native lighting; this is an
  LDR-light estimation approximation, NOT HDR recovery. No readback/global cloud boost/new sliders. Actual WARP
  tests an off-center cloud opening, zero influence outside support, uniform sky/no extra emitter, finite packets,
  release and integrated power within 2.5%. Updated native 1.0 controls/pixels/reload: 15 passed; 1.2 live owner
  comparison confirmed partial improvement. Full build + offline suite ALL PASSED before the next host stage.
- Added reusable CPU mesh/TLAS builder and actual D3D11 world-ray query core (ray_scene.h / gfx/ray_scene.hlsli).
  WARP covers hidden background after moving/removing/scaling a foreground hull, local-to-world ray parameter,
  cutout traversal, unknown material/coverage, backward/near-clipped rays and invalid inputs. No camera/image in
  this query. It is NOT yet connected to a complete native material/posed-scene snapshot or production water.
- Next host stage IN PROGRESS: optional scene_draw_observe + on-demand scene_draw_geometry C ABI, safe Rust
  callback/owned vertex+index copy, current native transform, prelit color/UV, two texture-stage states and alpha/
  culling/blending metadata. Exact native draw buffers, not collision meshes or an image lookup. Shader/GPU-pose
  sources are explicitly unavailable instead of copying rest geometry. Batching bypass only while observed;
  owner unload/fault cleanup. C/C++/Rust native probes being built; host builds and x86 Rust check pass. No native
  deployment/complete-world capture/production geometry-ray integration claimed at this point.
- Native source stage, 1.0: fixed an initial bad module-base calculation before callbacks (g_ls3d is a build-table
  pointer, NOT the LS3DF module base). Native Render caught the access violation; crash_triage returned an old
  October report, not a new crash. Corrected to ModuleBase("LS3DF.dll") + the verified per-build table offsets;
  restarted the lab and verified actual source callbacks. No exposure of the faulty host to the 1.2 owner lab.
- Added tests/game/test_scene_source.py: 11 native checks passed in 1.0. C/C++ normalized triangles, finite UV/
  positions, C size guards, expired borrow refusal and safe-Rust owned copies passed. Probe sweeps face groups,
  including non-quad geometry; native GetTexture/GetTransform agree with the source metadata. Self-unregister,
  duplicate removal, independent owners, reload, deliberate callback fault and recovery passed. Both observers
  released afterward. Full post-host build/offline regression running; 1.2 new-host validation still pending.
- Post-host full build and offline suite ALL PASSED. Installed identical host AC81242E... in BOTH labs. First
  1.2 deploy was refused while the closing game still held its DLL; an unchecked dependent launch restarted the
  old host. Corrected by closing that exact lab PID, waiting for process exit, checking deploy success AND hash
  before relaunch. Restored the freshly saved world (water-source-host-20261101), current owner freecam pose and
  freecam/FOV85/HUD/DLAA preferences. 11 source/lifecycle checks then passed in 1.2 too.
- Added mml/native_texture.h: original native mip texels/BC blocks copied into an owned CPU snapshot and a
  typed D3D11 resource, without scene-image sampling, implicit sRGB conversion or placeholder texture fallback.
  Unsupported/unlockable materials stay explicitly unavailable. Current formats include native BGRA/RGBA,
  565/1555/4444 and BC1/2/3; animated data requires explicit refresh. Native tests swept 24 different textures
  per lab: 4,097,376 bytes/200 mips in 1.0, 1,882,128 bytes/159 mips in 1.2; every uploaded mip read back
  byte-identically, zero read/format/upload errors. Source tests extended to 12 checks; ALL PASSED in BOTH labs.
  Temporary C++/Rust probes unloaded. Texture transfer alone is not ray-hit material evaluation or complete-world
  collection; both are still required before replacing the production water's projected object query.
- Added explicit bounded scene_draw_collect C ABI and safe-Rust owned result. Native main RENDER_WORLD stage
  only, requesting observer only; current full world-sector walk, actual sector lighting, native m_AddFGroups /
  m_RenderFGroup and a scoped finest-LOD override. DIP is observed but not rasterized; no six city views, no
  image/depth lookup. Backdrop/near-sector/effects excluded. Inactive streamed blocks and unsupported billboards
  become conservative missing boxes. Native D3D9 states, 19 LS3D property caches, queue and FPU restored even
  across nested callback faults; hidden water is deliberately absent, not a city-sized unknown box.
- Collection checkpoint: build/water-artifacts-20261101/collection-preimages. Verified both native bound-update
  entries (1.2 +5A510 / 1.0 +57650), resolved by their existing exported symbol, and existing per-build AddFG
  tables. Read native BuildAllVBuffers/NextFFrameProc as a preparation precedent, but did NOT call that routine:
  its dummy sector loses actual object lighting. Collection uses the real ancestor sector instead.
- Native 1.0 test_scene_collection.py: 15 passed, C/C++/safe Rust. About 67k triangles/1,377 mesh visuals from
  the test box, over 1,300 absent from ordinary camera draws; exact position/topology multiset hash unchanged by
  pitch/yaw. An isolated off-camera 144-triangle prop survives FOV changes, moves by exactly 25m in the next
  snapshot and disappears on removal. Device states/bindings restored, invalid context/bounds/no observer
  refused; deliberate fault DURING collection unloads owner and a new generation reacquires the service.
  Full copy 38.8ms vs metadata-only 10.2ms (diagnostic consumer included): do NOT collect/rebuild everything
  each frame as the final solution. Immutable geometry cache/current instances, material evaluation and water
  integration remain required. New collection host only in 1.0 at this point; 1.2 retains AC81242E... stable
  source/texture stage. Owner correctly asked about no water in 1.0's frame: clarified these are WORLD SOURCE
  tests, not a completed reflection/appearance test; native river/barge acceptance is still pending.
- Full post-collection x86/offline ALL PASSED; installed B80B41F4... in BOTH labs. Source/texture lifecycle tests
  pass 12 EACH. Collection passes 15 in 1.0 but only 13/15 in 1.2: pitch/yaw change the number of readable
  geometry packets despite the same selected mesh/draw count. Gate FAILED, not a completed source feature.
  Kept owner camera/freecam/DLAA preferences; owner continues moving the camera, so do not reuse stale poses.
- Actual 1.0 native most_parnik source check with the camera still on the street: a child cached at z=-459 was
  refreshed by native preparation to its current parent near z=519; 30 packets, 996 triangles, zero unreadable
  sources in the tight barge box. This proves native pose refresh outside the camera, NOT final water appearance.
- 1.2 diagnostic failures are flags=COLLECTION only, FVF142 (no GPU-skin/shader substitution), with subsequent
  VB/IB READONLY locks succeeding, native usage8. Do not blame GPU skinning or merely a refused Lock. Found a
  missing native preparation boundary: Render calls both pooled-buffer UnlockAll methods before consuming
  face groups. The collector prepared then consumed immediately. Added the same native publication calls;
  verified function/ECX-manager pairs in BOTH Render disassemblies, appended source-only table fields and Rust
  tables, check_builds ALL PASSED. Pool-sync build 31F7A2A3... is SOURCE/BUILD ONLY pending native retest. The
  legacy production water query and black-streak lighting remain unchanged; explicitly told the owner.
- Pool publication calls were required but did NOT resolve all refusals. Added stage diagnostics without
  weakening index/vertex buffer bounds: failure was step7, e.g. index19 with vertex_count18. Root cause was the
  collector's bare return0 in GetObjectLOD: this native function also writes object.cur_lod (+20C), then the
  native face-group renderer uses that stored level for its buffer/count. Preparation used LOD0 but consumption
  retained an ordinary-camera LOD. Read BOTH GetObjectLOD decompiles (+61E30/+5EB80), including +20C stores,
  callback bit400 and LOD_CHANGED2000000. Source now calls the original with distance0, suppresses gameplay LOD
  callbacks, and reverse-restores all queried visuals' current LOD/changed bit at transaction end (proxies too).
- Installed 42F1B2AE... in BOTH labs. Strengthened native regression to require all CPU geometry packets readable:
  16 checks pass in EACH build, including the previously failing 1.2 pitch/yaw cases, move/removal and nested
  callback fault. Example 1.0 tight world box: 3,835/3,835 packets, 69,930 triangles, zero unavailable. All three
  owner water/devtools/DLAA values compare unchanged; latest owner pose restored from water-source-lod-20261101.
  Full post-LOD build/offline regression remains next. Production water is still the prior partial shader; no
  final reflection/black-streak fix claimed. Next: GPU material/scene integration and immutable source caching.
- Post-LOD full x86 build and offline regression completed: ALL PASSED (lod-final-full-build.log and
  lod-final-offline.log). This validates the source-service block, not final water appearance.
- Owner supplied W:/Downloads/MafiaVoda.c for comparison. Read shader/reflection, wave, bed transport and
  integration paths; recorded source hash and findings in knowledge/notes/water.md. No build/install or code
  import. It uses main-camera SSR INCLUDING sky, a forced upward reflection ray, flat normal-only waves and
  an empirical point-direction glint. Potentially useful rain rings/visible-bed fallback; it cannot resolve
  hidden geometry, barge disocclusion or correct wave self reflection. Our production water is still the
  prior partial shader and its black-streak defect remains open; no visual improvement claimed from review.
- Owner prioritized fixing black bands. Launched both previously stopped labs; restored 1.2's actual world
  from water-source-lod-20261101, preserved water/devtools/DLAA values. Reproduced the lost downward-ray and
  back-facing fine-normal branches with native debug19, saved verified source/lab DLL preimages.
- Implemented shared bounded world height-field intersections and river FFT/chop evaluation. First-hit repairs
  and two further water interactions retain Fresnel/medium transport; no upward ray clamp or added sky fill.
  First-hit depth/motion follow the corrected water point. Reused shared slope/shape normal calculation;
  four stratified facet samples reduce query work. Developer debug21 retains the prior branch for same-phase
  pixel comparisons, without adding artistic controls. Full architecture/approximation limits in water.md.
- Full x86 build/offline ALL PASSED. Installed water 896F21FA... in BOTH labs, hashes verified. Native
  test_water_visibility.py passes 5 EACH: identical frozen wave field, old black bands reproduced, near-black
  pixels 8923->591 (1.2) /15553->233 (1.0), no blanket brightening, flat-water radiance within one byte. Native
  low and steeper views inspected. This does NOT prove every angle/phase artifact or object reflection fixed.
- Single-lab low-water GPU comparison: old branch about9.5ms, new14.7ms (+5ms); acceleration still needed.
  Source service and legacy object-query integration remain unfinished; DLSS calm-water swimming still queued.
  Disabled temporary kit/kit_rs AUTOLOAD in both lab manifests (verified backups); source tests can load them
  explicitly. They are not user mods and no longer reappear as a kit overlay at the next launch.
- Final lab state: 1.2 running (pid22400), freecam control returned, mission/waves advancing; 1.0 closed after
  its final native checks to free GPU time. Water/devtools/DLAA effective values compare equal to BOTH labs'
  entry snapshots. Camera hand-back starts devtools from the native game camera: its camera_ray does not read
  the temporary bridge pose, so do not claim camera.set seeds freecam. No host/tool change made for that.
- Owner explicitly accepted the black-band fix, then requested a small-step plan to finish object reflections.
  Read current source/SDK and recorded docs/WATER_REFLECTION_PLAN.md (28 ordered checkboxes with individual gates).
  Early native hit and colored-material checkpoints prevent another source-only completion claim. Included
  current transforms, alpha/blend/deformation providers, streaming coverage, immutable caches, wave/object
  nearest-hit integration, world shore constraint, old-view removal, lifecycle/cost, both-build spatial tests
  and the previously queued DLSS issue. Added current-status links to the historical architecture audit and
  roadmap. Planning/documentation only this turn; no game/config/binary change or implementation claim.
- Owner asked why no hardware RT and whether it should fix the roughly20FPS water. Read current D3D11 bridge
  creation and existing D3D12 side_picture sharing path; checked official Microsoft DXR/Khronos API guidance and
  NVIDIA RTX3070 information. D3D11 software traversal was a convenient testable prototype, not a measured
  optimum. A missing EARLY hardware comparison in the previous plan was an oversight; added prerequisite0
  and made scene-upload step4 backend-dependent. No RT implementation or runtime capability success claimed.
- Fresh read-only 1.2 snapshot, RTX3070/1440x810: frame68.619ms (~14.57FPS), water surface29.4ms GPU,
  FFT.173ms, about775k water triangles; old object path still legacy. These are live-view counters, not a paired
  benchmark, and overlapping wait/GPU counters must not be summed. Previous frozen low-water sample14.7ms is
  another pose, not contradictory. Hardware object queries cannot alone explain/fix today's slowdown because
  the new software object BVH is not in production yet. Plan0 includes FFT-wave representation costs and
  interop/synchronization as well as triangle query speed. No game/config/binary change this turn.
- Owner accepts the entire CURRENT WATER LOOK, identifies only object-reflection correctness and missing
  water shadows as present visual defects, and explicitly prioritizes reflections -> shadows -> optimization.
  This supersedes the preceding hardware-first proposal. Updated WATER_REFLECTION_PLAN.md and ROADMAP.md:
  phaseA current-backend reflection correctness, phaseB trace/restore existing shadow path (S1-S6), queued
  temporal validation, phaseC hardware comparison/optimization. Existing caches/publication retain necessary
  correctness/lifetime scope; speed-only tuning is deferred. Freeze accepted waves/color/foam/highlights/
  lighting/filtering; a visually worse optimization is rejected rather than silently retained for FPS.
- Saved build/water-accepted-visual-20261101: 50 source/SDK/lab binary/settings files with verified per-file
  SHA256 and a manifest, no automatic bulk restore. Both saved water DLLs match the accepted 896F21FA... hash.
  This is a rollback/reference checkpoint, not a completed repeatable visual-case capture. No shader, game
  configuration, runtime light or water parameter changed while updating priorities and preserving this baseline.
- Owner explicitly authorized executing ALL reflection-block steps in order. Started with actual baseline/
  inventory, both labs together. Saved fresh 1.2 entry restore point; per-build baseline restore points and
  settings/camera files under build/reflection-block-20261101. Wave speed alone did not freeze moving foam:
  added nonpersistent river_water.capture_clock {seconds|release:true}, affecting both clocks only while held;
  ordinary shader bytecode, optical constants/settings behavior unchanged. Build E62C218E... installed both
  for capture support. Clock released and user water/DLSS settings restored after captures; kit unloaded.
- Eight repeated native cases EACH: sun low/down, bank, bridge, actual native barge, two controlled depth
  layers before/after foreground movement, flat water. Every repeat below diagnostic overlay is byte-identical.
  Verified 1.2/v3 barge and 1.0/v2 barge images on water. Earlier v1 barge camera was under land, and one v2
  candidate hit hull/land; kept those failed evidence files, corrected camera selection rather than counting
  them as passing. Native ground rays identify water or raise the camera above an intervening deck/bank.
- Expanded kit's one-shot source inventory. 1.2 broad case: 13,764/13,764 CPU packets readable, 233k+ triangles;
  two UV/stages, no generated UV/sRGB/GPU skin/custom shaders in required cases. Native ADD/MODULATE2X/
  BLENDFACTORALPHA, alpha thresholds1/200, alpha/additive blending require support. Missing506 billboard
  visuals include tunnel glow/traffic lights; 1.0 also has unloaded city boxes (89 in initial case). These remain
  explicit required providers/coverage, not sky. No production reflection fix claimed from source counts.
- Completed plan1-3; defined docs/SCENE_RADIANCE_SNAPSHOT.md (owned data, source revisions, caller world-cycle
  token/current-frame transaction, primitive/material/opacity links, unknown coverage and lifecycle). Next4/5:
  reusable GPU upload and native ray-hit diagnostics before materials. Shadows/performance remain deferred.
- 2026-11-02, reflection4/5: added reusable SDK scene_snapshot.h/ray_scene_gpu.h and independent exhaustive
  double-precision ray_scene_reference.h. Same-cycle/device readiness, atomic candidate upload, failed/stale
  publication, transform/move/remove/reset/recreation and borrowed-data ownership pass actual WARP tests.
  Native river_water.scene_probe passes14 checks EACH in1.2/1.0: three hidden depth layers at9.94865/19.94865/
  29.94865m, camera pitch/yaw/FOV invariance, movement/removal, complete miss and unknown outside coverage.
  CPU instance/primitive/distance/barycentrics agree. Tests: tests/game/test_water_world_rays.py; evidence:
  build/reflection-block-20261101/native-world-rays.log and native-diag-build3.log. Installed both water DLLs
  8702F8ADAAAF4E869BC87AB36B3060CAFEE7055FCD6D629F3C52343E6315AA14; default PsWater SHEX155696bytes
  is byte-identical to the accepted preimage. Only diagnostic shader/resources were added, no optics retuning.
- debug22 uses actual water mean-facet world rays and instance colors, with complete miss blue, unknown magenta,
  invalid orange. Native images saved under each build's geometry-checkpoint. The 1.0 broad river image is all
  unknown because unloaded block coverage intersects the rays; 1.2 shows unknown stripes near unavailable
  contributors. These are explicitly diagnosed source gaps for steps13/14, NOT a final reflection image or a
  reason to fill missing data with the old capture. The one-ray report in those broad captures retains the
  preceding isolated-test origin outside the city box (unknown at.02), so it is not counted as a river-hit test.
  Both debug modes/clocks/time/camera/DLSS restored; props removed. Normal water still uses its old object path.
  Next6: establish native stage/prelighting/color/fog semantics before texture storage/material integration.
- Reflection6: verified both FogProcess implementations (1.2+685D0,1.0+65180) and native object/lit face-group
  renderers. Software fog can be a separate overlay pass; collected radiance must exclude it. Added scoped
  source-radiance fog suppression in host/render.cpp/geom.cpp, restoring native caches/device state even on
  owner fault. No ABI field/layout changed. Installed host CE4EA5FAF7FDBC8153DE16CA496C132C1CE5D2E98535DDEAAB5B185A42664065
  in BOTH labs after saved reflection-material-entry-20261102 points; restored worlds. material-source-tests2.log:
  29 checks/lab, C/C++/safe Rust/lifetime/fault/state restoration, camera-independent prelit color/UV hashes.
  Full offline suite material-offline.log ALL PASSED. Native fixed-stage evaluator native_material.hlsli passes
  64 actual D3D9 RGB/alpha patches versus D3D11 (native-material-test.log), <=1.1/255 rounding. Encoded operations,
  final RGB linear decode, native prelighting once, path atmosphere once; no relighting/brightness adjustment.
  Geometry diagnostic remains optional; normal water optics/source path unchanged. Next7: native texture pages.
- Reflection7: sdk/native_texture_pages.h and gfx/native_texture_pages.hlsli store original raw mip rectangles
  in64KiB logical pages, no resize/atlas-border bleed. Explicit wrap/mirror/clamp and point/bilinear/trilinear;
  original RGBA/BGRA/packed565/1555/4444 and BC1/2/3. Re-read source texture bytes during each borrow, content
  hash plus exact equality deduplicates; pointer reuse cannot select old pixels. Snapshot/material ownership
  extended without changing host ABI. Explicit full-copy diagnostic only, caching still step16.
- texture-pages-test9.log:1512 samples, cross-page132x132 source, original mips, replacement of same backing
  storage, failed candidate invalidation/release. Typed D3D11/uncompressed D3D9 worst1.4839/255. D3D9 refuses
  A8B8G8R8 on this device, so its original bytes are checked through typed D3D11. Legacy D3D9 BC samples differ
  up to4.8/255 from the canonical D3D11 decoder; recorded separate tolerance, not described as byte identity.
  Required native cases use A8R8G8B8. Native captures in both labs:889 textures/72,514,012bytes (1.2),497/
  42,733,792bytes (1.0), all readable, final page padding34,340/61,216bytes. Evidence native-texture-pages-tests.log
  and native-texture-pages-results.json. Water23C2FDA820F4AC770A03DDAEA32C8C69FCB44D2937F876073E89223FF56C933A
  installed both; normal shader unchanged. Step7-pre-integration contains10 verified source preimages.
  Next8: GPU primitive/attribute binding before opaque material shading. Source completeness remains13/14.
- Reflection8: shared scene_radiance_gpu.h atomically publishes geometry, native texture pages, original
  primitive attributes and packed material tables in one candidate; failed/stale/device generations refuse
  binds. gfx/scene_surface.hlsli interpolates original UV0/UV1/prelit ARGB using stable primitive IDs after BVH
  sorting. Actual WARP checker/UV/color tests include nonuniform instances and invalid primitive publication
  (attributes-unit-test2.log). Native CPU/GPU ray+attributes pass19 EACH in1.2/1.0 (native-attributes-tests.log).
  Both lab test clocks/camera released, isolated props removed. This remains diagnostic, not production color
  reflections. Next9: evaluate actual opaque native materials at the world hit.
- Reflection9: MmlSurfaceEncoded evaluates original native stages/UV0/UV1/textures/prelit color at the world
  primitive; final RGB decode only. Extra unverified stage/specular/sRGB/result-register states are explicitly
  unavailable. Added diagnostic material values and native-triangle sampling (CPU/GPU same hit, never images).
  Native ANISOTROPIC MIN initially refused all material samples; at zero footprint it correctly uses MAG.
  Minification still requires gradients and remains unavailable until20; no isotropic replacement is hidden.
  opaque-material-unit-test.log passes actual WARP hit texture/prelighting/linear decode. opaque-native-tests3.log
  passes five samples EACH: tunnel Arc02, most_parnik hull, k_stromy tree mesh, newdum house, most zab12 bridge.
  Color is reconstructed without a scene camera. This proves material evaluation, not finished water reflections.
- Native material fixture used a TEMPORARY whole-city activation to isolate material correctness from14's
  streaming gaps. First fixture incorrectly read city.whole_city rather than city.whole for restoration;
  corrected it and restored1.2 whole=true/1.0=false, then reran successful fixtures. Failed initial evidence
  retained. Source-level anisotropic rejection is fixed, not bypassed by brightening. Both diagnostics/time
  released; normal water still uses the accepted old object path. Step10-preimages saves seven verified inputs
  before alpha/sidedness traversal changes. Next10: cutouts and native winding before blended continuation.
- 2026-11-02, owner reports a computer reboot and asks to continue. Verified persisted source/evidence and
  both labs stopped; mode remained loader. Rebuilt current sources and repeated actual WARP/native D3D9
  tests (post-reboot-build.log, post-reboot-tests.log). Current cutout traversal rejects alpha before nearest
  hit, respects native CW/CCW sidedness and negative-determinant instances; unsupported blend remains unknown
  pending11. Native D3D9 reference passes224 alpha comparisons, including fractional diffuse alpha, and six
  sidedness cases. Byte-domain alpha comparison avoids a 200/255 normalization mismatch; geometry/reference
  tests pass hidden cutout background and stale-publication cases. Step10 still pending native foliage gate.
- Installed F45B8CCD69047FE2C900312DC1E86D80275C0A0BA93B027A8DD72A7D522D6F43 both labs after verified
  preimages, launched background1.2 pid11628 /1.0 pid22720, restored per-build reflection-material-entry-20261102.
  post-reboot-native-world-rays.log passes19/lab with native sidedness and original attributes; normal water
  object path remains the accepted legacy one. Native cutout query fixture now scans actual foliage alpha;
  test camera/time/source resources are released after fixtures. No shadow/optimization work started.
- Reflection10 native gate passes BOTH builds (native-cutouts-tests2.log/native-cutouts-v2.json): real
  zabradli04 alpha0.1582031 fails native GE200 at0.049952m; traversal continues to a deeper real primitive at
  10.751377m with its own material. Empty holes return complete bounded misses. Initial named tree fixtures
  had no alpha-test material and were unsuitable, so the fixture now selects actual alpha-tested unblended
  native draws. Full native geometry/attribute regression still19/lab. Geometry-only CPU reference naturally
  sees the cutout's front triangle; actual opacity is independently checked by D3D9 and WARP material tests.
  Default PsWater SHEX155696bytes still byte-identical to accepted preimage after reboot/rebuild. Step10 complete
  as a query mechanism; normal water has not switched yet. Step11-preimages saves seven inputs for blending.
- Reflection11 complete: gfx/scene_query.hlsli orders native alpha/additive contributions in linear light,
  retaining deeper geometry. Coplanar distinct draws survive; one draw's shared triangle edge contributes once.
  Exact ties use source order, not BVH placement order.32-layer overflow, missing material and coverage stay
  UNKNOWN with their incomplete prefix; no transparency threshold or silent sky fill. Actual WARP fixture
  passes (blend-unit-test3.log); native D3D9 blend factors pass10 cases alongside224 alpha/six winding tests
  (native-blend-reference-test.log). native-blends-v2.json passes2/lab: alpha0.3372549 and isolated native
  taxi additive taillight alpha0.1404099 followed by correct deeper material. Initial broad-city additive1.2
  fixture hit missing billboard regions; retained failed v1 evidence, did not remove those unknown boxes.
  blend-offline.log ALL PASSED, including the new scene_blend_test. Installed440EB4BD1B03BF86CB359824AED453CFABDE8FABDA640226CF281458FF133915
  both labs; verified previous DLLs in step11-installed-preimages. Default PsWater SHEX remains byte-identical
  to accepted155696-byte shader. debug25/new one-ray blend probe only; normal object path still legacy.
  Native fixtures removed taxi props and restored time/city state. Next12: explicit rotation/current-instance
  regression, then required billboard/deformation provider13 and independent streaming14, before color preview15.
- Reflection12 complete: the current owned native transform path passes a new asymmetric off-camera taxi
  regression in BOTH builds. Same world ray changes7.698921->9.400332m after90-degree Y rotation, misses after
  translation, restores7.698921 exactly on pose return, and reports zero triangles/miss immediately after
  deletion. Each published native frame follows the mutation and agrees with independent exhaustive geometry.
  native-current-instances-tests.log29/lab passed (10 new +19 existing geometry/attribute checks). This adds a
  regression for the already-current native matrix copy rather than another transform mechanism. Tests release
  scene resources, camera/time and all props; accepted default water remains unchanged. Next13: missing native
  billboard/deformation semantics, followed by14 streaming coverage; neither is excused by diagnostic UNKNOWN.
- Reflection13 in progress: verified native ProductBillMatrix+6090/+6030 and AddFGroups+73C0/+71C0 in both
  builds;1.2 mode+220 is uint32,1.0 byte boolean, common axis+21C. Current native inventory270/141 billboards
  uses only full-facing axis2/mode0 and cylindricalY axis1/mode1. Independent CPU/HLSL helpers match24 actual
  native matrix fixtures within2.92e-6 (billboard-math-test1.log); actual WARP procedural intersection/bounds/
  degenerate UNKNOWN tests pass (billboard-rays-test1.log). Added opt-in host suffix calls, owned descriptor,
  safe Rust support/generated bindings and native C/C++/Rust probes. Native integration/lifecycle/prelighting
  acceptance is pending; step13 remains unchecked. Nine verified source preimages in step13-source-preimages;
  test kit DLL preimages in step13-kit-preimages. Labs currently still have the previous host and accepted
  normal water. No reflection cutover, shadow restoration or FPS tuning was substituted for these gates.
- Procedural source native acceptance now passes9 EACH build: legacy compatibility, opt-in geometry, native
  state restoration, C descriptor size guards, safe Rust callbacks/owned results, stable original prelit/UV/
  vertex fingerprints under main-camera pitch/FOV, nested owner fault/reacquisition. Evidence
  native-billboard-provider-v2.json. Actual native full-facing billboard GPU ray/material/additive continuation
  passes7/lab from three observer directions with fixed-world-ray camera invariance (native-billboard-rays-v1.json).
  Installed hostD0E5C99306475AD4C42088203BF37F710ABF9552725E4558ECD932665F39A676,
  water4126C154A4987B6ED69C9AB66909384A759F50655921AC5FD6494F436BA4E285 both labs;
  host/offline tests ALL PASSED. Default PsWater SHEX remains identical. Source/installed preimages retained;
  native restart restore point reflection-step13-entry-20261102. Current pids14808/25044 after background restart.
- Off-camera posed-person fixture exposed a real collector omission: SMMR (524D4D53), the people's native
  single-morph class, was absent from the type selection and silently skipped. The failing deformation-v1
  evidence is retained. Verified constructors LS3DF+56E80/+53FA0 in BOTH builds and existing native skinning
  contract (game_build.h/skin.cpp); include SMMR through its native virtual preparation/posed vertex buffers.
  Rebuild/native deformation regression pending; step13 stays unchecked until current-pose gate passes.
- Reflection13 completed. SMMR inclusion passes native-deformation-v2.json5/lab: real off-camera Paulie,
  native posed-buffer geometry changes under a held pelvis pose, topology stays constant, old GPU body hit
  disappears/new position hits, original vertices/hit return with the pose. Added permanent regression to
  tests/game/test_water_world_rays.py; singlemorph-official-regression.log32/lab passes. Repeated procedural
  C/C++/Rust/restoration/fault9/lab and native GPU billboard material7/lab tests pass after this correction
  (native-billboard-provider-v3.json, native-billboard-rays-v2.json). No required GPU-skin/custom source observed;
  their unverified flags remain explicit UNKNOWN. Actual native geometry is used, never rest/collision mesh.
  Host822425DC64CE842711D2023ADA5AEA994A4F10F21387DF759FDF0D0A7FE20684 installed BOTH; water4126C154...
  unchanged. Background pids22292/5580, entry restore point reused, all test actors/poses/camera/time released.
  Normal water still has the accepted legacy object path; next14 is owner-scoped city streaming coverage,
  then15 an explicit colored native preview. Step13 changes do not claim final reflection or shadow completion.
- singlemorph-offline.log ALL PASSED. Reflection14 implementation in progress: owner-scoped copied source
  regions, native pre-render block activation, tick deactivation protection across combined owners, automatic
  unload/fault/mission release, C ABI suffix and safe Rust. Native ActivateBlock/DeactivateBlock/block walk/
  AddTimeTransparentObject/FinishTimeTransparentObjects read in BOTH builds first. Only fade entries appended
  by this activation are completed/released; unrelated prefix is retained. No global FinishFades or saved
  rendering/FOV/far/LOD/occluder override. Contract docs/SCENE_RESIDENCY.md;8 verified source preimages in
  step14-preimages. C/C++/Rust probes and native-residency fixture prepared. Builds/native acceptance pending;
  step14 remains unchecked and labs still run the completed step13 host822425DC... /water4126C154....
- Reflection14 complete: native-residency-v3.json12/lab passes actual bounded source loading with native
  distance mode0 (no whole-city setting),103/103 requested blocks, zero stream gaps, hidden building geometry/
  native material behind BOTH old capture cameras and invariant under turn/FOV. C size/invalid guards,
  safe Rust second owner, release preserving another owner, last-owner fault and native deactivation pass.
  The native fade prefix, including an unrelated test fade, stays byte-identical. Initialv1 found same-mission
  restore retained ownership: fixed at SaveGameLoad/NewGame boundary (native mission close need not occur),
  scene generation advances and pending diagnostic/history clears on GAME_LOADED. Failedv1/v2 retained;
  v2 also assumed a suitable pre-existing camera heading and used an insufficient5s poll for a broad material
  copy; v3 deliberately looks away from a real building and probes its local box with30s call allowance.
  residency-load-offline.log ALL PASSED. Both labs installed hostBD5A53507F5EA740156C4C278F3DB076110B6A2573EC6F7012C200BCFF218DEC,
  water287375E71F756968A3B6C0DE39EDCC53D88C0E3DE5618798C208EDF518FA1CC0; pids21492/12416 after
  background restart. step14-installed-preimages has8 verified preceding binaries; restore points retained.
  Tests unload both probes, release regions/camera/time and restore saved scene/distance mode. Native default
  water optics/shader are still unchanged; step15 is an explicit colored preview, not production cutover.
- Reflection15 in progress: debug26 combines current native geometry/material/layer queries with the native
  directional cube only after a complete bounded miss. Unknown data stays magenta, with no projected-image
  fallback. This is explicitly raw first-facet incident radiance, not final water shading; production remains
  unchanged until18. Two verified source preimages in step15-preimages. Shader build/native captures pending;
  step15 is not checked complete. Status now identifies the preview and current snapshot readiness/counts.
- Pre-preview regression initially found288 extra triangles in each empty fixture: two leftover bridge-created
  off-map fade-test props per lab. Read props.cpp/game_hooks.cpp: PropsMissionClose alone clears the registry;
  same-mission native load need not call it. The earlier residency fixture's automatic-cleanup assumption was
  wrong. Removed only those identified test props, corrected fixture cleanup and recorded the host prop/save
  limitation in Known gaps. No geometry was filtered to hide it. Repeated32 native current-instance/geometry/
  posed-person checks now pass EACH lab (pre-preview-instance-regression2.log); initial failed log retained.
- Reflection15 build step15-build2.log passed; installed water3ECF201748ADF8C03552BC7FC108C4B9EB31A7E7D1DDC09A5DA0075E1F49D049
  in BOTH labs by hot reload, hostBD5A5350... unchanged. Native-preview-v1 saves actual raw640x480 bank frames,
  flat/rippled:410509/397253 triangles, zero missing boxes.1.0 reflects native colored houses absent from the
  main frame. Compiler resource bindings prove preview PS has no old reflection color/depth/painted-sky or
  main SceneColor input (main SceneDepth is only primary visibility). Normal PsWater SHEX155696 bytes still
  exactly matches accepted baseline; shader-contract.json records hash/bindings. Full diagnostic copies are
  about10s/frame CPU in this broad box, not a production performance result. First barge fixture is rejected:
  its moving root had reached z977, outside bank coverage for300m rays, and longitudinal50m camera was too
  close to the bow. It correctly showed UNKNOWN magenta. V2 uses a lateral80m camera and root-centered bounds;
  native capture/visual acceptance still pending. No range extension is silently filled with a scene image.
- Reflection15 completed: inspected actual native bank flat/waves in BOTH labs and actual barge color previews
  v2/1.0, v3/1.2. V2's1.2 camera stood on the street with a narrow water strip, so v3 verifies over-water
  placement with world.ground before capture. Correct white/black/rust hull and superstructure colors, deeper
  bank/clouds visible; opaque hull blocks deeper sky. Finite300m/coverage horizon remains explicitly unknown.
  These are raw first-facet LOD0 diagnostics, with filtering/continued waves/final composition still18/20/21.
  post-preview-instance-regression.log32/lab passes with the installed3ECF2017... shader. Verified owner configs,
  DLSS on, time1, window1440x810 and freecam ownership restored; props.list empty in both. Native geometry and
  accepted PsWater remain separate paths until18. Next16: verified immutable-resource lifetime/content reuse,
  followed by17 current dynamic publication. No shadow/FPS/optical retuning was substituted for these gates.
- Reflection16 implementation started with7 verified preimages in step16-preimages. No ABI extension or
  native-address guess: immutable CPU mesh/texel reuse validates the actual CURRENT borrowed content before
  any hit. Hash/pointer only select candidates, exact bytes establish equality; failed reads cannot reuse old
  pixels. Meshes reuse local BVHs and rebase current primitive/material bindings during packed assembly;
  shared texture payloads retain no native COM pointer. Bounded owner caches retire unseen content and release
  on reset/unload. Host preparation/packed GPU copy still remain; builds/tests are pending, step16 unchecked.
- Step16 targeted builds pass. Newly rebuilt ray_scene_test initially exposed old fake host tables advertising
  the post13 full ABI size but leaving scene_draw_collect_ex NULL (CDB null-call stack; retained debug logs).
  Prior full-suite runs had used the older test executable. Corrected the fake legacy size and populated the
  opt-in callback in current attribute/opacity mocks; no game/SDK capability was weakened. step16-rays3.log
  passes all current WARP/source/cache/material/cutout/billboard assertions; step16-material1.log passes actual
  D3D9 every-mip texture mutation/metadata/missing verification plus1512 samples/224 alpha/blend references;
  step16-blend1.log passes ordered composition. Added9 verified preimages total. Native acceptance is next.
- Reflection16 complete: native-scene-cache-v2.json9/lab validates actual current native buffers/texels, same
  query color/UV/prelit/depth, moved/returned/deleted/recreated lifetime and owner release. Owned payload
  allocation capacities (not merely vector sizes) count toward cache limits; Release frees map/vector storage.
  Native-preview-v4 full bank/actual-barge frames show zero mesh builds/texture copies after current-content
  verification (about25k draws in broad boxes). step16-offline.log ALL PASSED; targeted current rays/material/
  blends also pass after diagnostic changes. Installed B8C0B95E0F2E1D8758B3012CAEE1F617F077672D0E8F254FF5B0599AC7D714E6
  BOTH labs, hostBD5A5350... unchanged. Normal shader still accepted.10 source preimages plus3ECF.../4112...
  installed preimages retained. Added source publication stage/HRESULT diagnostics instead of guessing failure.
- Owner correctly identified native-preview-v4/1.0 dark triangles: the water pass was absent, exposing what
  is underneath, NOT black reflected objects. Reproduced native-black-v3: texture-page-build throws bad_alloc,
  reported E_OUTOFMEMORY8007000E; repeated allocation/repacking of about83MiB of texture pages is the failed
  link. First ray may publish successfully, later identical copies fail; the current CPU scene remains valid.
  Failed v1/v2/v3 artifacts are retained. Current publication failure intentionally never presents old geometry.
  Next17 must share unchanged verified GPU tables and bound staging memory, while keeping current poses/frame
  constants and no CPU readback in the continuous path. Do not retune optics/shadows or hide this with old images.
- Reflection17 implementation started. Five verified source and both B8C0... installed preimages retained in
  step17-preimages. Current immutable snapshot leases let exact CPU-byte validation share unchanged D3D11
  buffers without another full CPU payload copy. Reused resources must take the CURRENT lease (otherwise
  independently changing tables could retain many whole old scenes). Texture sharing is certified by current
  verified immutable texel identities; current per-frame transforms/constants still publish coherently. No
  rendering samples/optics are changed. Builds/native allocation-failure acceptance remain pending.
- Step17 current build/native checkpoint: immutable GPU buffers compare exact current CPU bytes and lease the
  CURRENT Snapshot; texture pages share only identical verified owned texel sequences. Page staging preflights
  all mip rectangles then allocates exactly once, preserving every byte/offset. step17-rays2/material2/blend2
  pass current GPU/source/texture/opacity/lifecycle tests, including11 shared payloads, changed surface upload
  and weak old-Snapshot expiry. Installed17B334638F0BE5DA42429C11C33DBAB70862BF0136A359A03ED227472E8C67C3 BOTH labs.
  Reproduced full1.0 allocation case now passes5/5 queries (native-publication-v1):11 GPU payloads reused,
 64 bytes current frame constants uploaded, no E_OUTOFMEMORY. CPU source validation still costly: sample
  native collect1686ms inclusive, geometry copy118ms, texture verification1021ms, instance build18ms,
  publication47ms; not a final FPS claim. Native9 cache +32 pose/geometry checks per lab pass, full preview-v5
  and step17-offline.log ALL PASSED. Continuous fixture initially assumed a new source must be first copied
  in the probed frame, but the continuous publisher had already copied it in an earlier current frame; the
  asserted new content revision was correct. Adjusted this timing assumption, retaining strict lifecycle
  identity/current hit checks. Continuous recheck and step17 final documentation pending.
- Reflection17 completed. native-continuous-tests3.log10/lab passes GPU-persistent same-frame transform,
  removal/recreation/release checks; current WARP changed-buffer/CPU-lease tests pass. Begin now drops its
  previous CPU owner (otherwise a failed publication could leave three complete CPU scenes next frame).
  Installed6E926BF469EBEA12B5E6550742F63367B9D95951662792B5883273FFB5211ABF hash-verified BOTH labs, hostBD5A5350...
  unchanged, pids21492/12416. Native publication-v1 allocation repro5/5 and inspected preview-v5 bank/actual
  barge frames retain the actual water; no E_OUTOFMEMORY. step17-offline.log ALL PASSED; targeted tests after
  the final owner-release correction also pass. No native resource pointer/TTL or shader quality shortcut.
  Normal accepted PsWater remains byte-identical and uses the legacy object path until18. Lab configs/time/
  window and camera ownership restored, no test props. Optional1.0 devtools/DLSS/Fusion modules are OFF in
  their original manifests; restored config values must not be described as active DLSS in that lab.
  Next18: integrate per-segment geometry/material queries in the normal optical material, with nearer hulls
  winning over continued water hits; coverage must follow the resolved main camera with a declared finite
  world-path range. Then19 shore,20 filtering,21 travel/light,22 lifecycle,23 obsolete capture removal,25 spatial.
- Reflection18 started with verified source preimages and fresh native restore points in both labs. Added an
  owner-scoped camera-relative residency suffix, resolved AFTER the current main camera override and BEFORE
  auxiliary/main drawing; no preceding-frame eye or global distance override. C/C++/Rust support is being
  validated. New normal shader queries each object segment before a possible continued water hit; accepted
  PsWater remains an explicit comparison entry. Finite total camera/water/object range600m bounds source
  residency (608m cube with numerical guard); infinitely distant sky remains directional. Integration/native
  appearance, shoreline, footprint and per-layer atmosphere gates remain pending; no completion claim yet.
- Step18 camera-region native gate passes7/lab (native-camera-residency-v1.json), with three changed camera
  centers/FOVs, invalid C arguments, C++/safe Rust shared ownership and last-owner unload. First fixture run
  passed1.2 checks but failed in its result writer (wrong Game.failed attribute); corrected to existing
  passes/fails fields, repeated both. Host428AC20B3AD9738CCC408E729D6CE1AD644716ACC7DFB960831D0D6CA4C82EDD
  installed/hash-verified both; native entry saves/config/camera/binaries retained. Normal PsWaterWorld still
  compiling; no native material cutover acceptance yet. C ABI generated Rust suffix matches the current header.
- Current host full offline suite ALL PASSED (step18-offline1.log). Native residency-v4 repeats the complete
 12/lab gate with current camera-resolution ordering: hidden real building/material, view invariance, independent
  streaming, unchanged unrelated fade prefix, shared owner/release/fault and same-mission load all pass.
  The accepted PsWater SHEX remains byte-identical155696 bytes after adding the separate world entry.
  PsWaterWorld now compiles successfully; remaining diagnostic entries/DLL and native image gates pending.
- Step18 code audit caught a finite-water-search correctness issue before deployment: unresolved downward
  continuation cannot query objects over the remaining600m beyond the traversed water prefix (that could
  expose submerged hulls). Object and water now use the same biased origin; only that bounded prefix is
  queried, and the retained two-water-bounce limit ends a downward residual. No new ray-direction clamp.
  Moved the expensive world entry behind a private wrapper define; other shader entries exclude its unused
  body at preprocessing. Shader build dependencies now include sibling HLSL as well as HLSLI for wrappers.
  The first build predates this correction and MUST NOT be deployed as current; rebuild/native gates pending.
- Follow-up design constraints retained for19/20/21: shoreline must use local world geometry (moving hulls
  may constrain nearby contact, never flatten remote water through a projected image). Texture footprint must
  be derived per native primitive/stage, including actual texture dimensions and anisotropic minification;
  passing one global positiveLOD to the current zero-footprint sampler would deliberately return unknown.
  Procedural billboards also need a path observer independent of the ray's water-surface origin: reflect the
  camera across the sampled facet, and update this virtual observer at continued facets. Current generic ray
  query uses its origin as observer; preserve that default/reference and explicitly extend the water path.
  Outgoing atmosphere is per layer/segment; additive emissions attenuate without adding another fog source.
- Reflection18 native gate complete: native-normal-v1 has8 inspected frames EACH lab (bank flat/waves,
  barge level/down, each vs explicit accepted debug27). Current water actually renders, zero diagnostic-magenta
  pixels in these cases, accepted wave/foam/overall optical appearance retained. Flat native objects are too
  unfogged at this intermediate stage, explicitly21; LOD0 is20. Normal PS has no g_reflDepth resource; only
  diagnostic10/16 old-color bindings remain pending23, not a projected object trace. Lowered barge view still
  samples world geometry with the hull outside main frame; no old image fallback. Full current native instance/
  layer/posed-body regression32/lab passes (step18-instance-regression1.log). E51042CF... was the native image
  build. Added CPP-only source_residency readiness/current-source diagnostics, rebuilt with unchanged shaders,
  installed/hash-verified3804791DFC1E2F33C25334458B6CB55C206A32B4C006DBF2D08722BBA0A9CE48 BOTH labs. Status
  diagnostics queried both successfully. Host428AC20B... unchanged, pids26244/9060. Temporary on=false in both
  during installation/testing; original on=true and full owner settings remain in step18-preimages/lab-entry.json
  and MUST be restored before handoff. No artistic settings retuned. Next19 local world waterline, then20/21.
- Reflection19 in progress; verified step19-preimages retain source and both3804791D... binaries. Added SDK
  scene_waterline.h: intersect CURRENT owned native triangles/affine poses with each mean water plane, build
  a local nearest-section BVH, preserve missing/uncertain coverage; no image, texture-name bank heuristic or
  collision proxy. View-facing billboard cards are not physical boundaries. CPU analytic edge/corner, rotated/
  scaled pose, moving local hull, missing/alpha, billboard and coplanar tests pass; source is in the offline
  suite now. Initial compile failures were Bounds::Add braced overload and test Instance name ambiguity,
  corrected without changing SDK ray code. Added degenerate coplanar-distance guard and strict unknown-
  distance comparison (a known boundary at zero remains proven even beside another uncertain boundary).
  GPU publication header scene_waterline_gpu.h exists but is not built/bound yet; no world-shore HLSL yet.
  Normal shoreline is STILL projected, so19 remains unchecked. Native source-v1/v2/v3 BOTH find2331 definite
  sections +36 alpha-uncertain sections in113 ready blocks (~4-5ms CPU model build). All36 are three alpha-test
  GEQUAL200 native meshes off27seg41/31/30.Object02, below the water with scaled geometry. Need certify their
  alpha coverage along the plane section before treating any card as a fluid boundary; no fabricated opaque
  or transparent assumption. Current native source diagnostics are saved, original settings remain pending
  restoration (on=false). Next: conservative native alpha bounds, then GPU/DS world constraint and native gates.
- Step19 native alpha-bounds provider and tests added: original mip0 formats/BC alpha, native addressing/
  filter support, fixed-stage interval alpha and integer-byte comparisons. Clear/opaque sections can be
  certified; partial/unsupported coverage remains explicit. Native source-v4 still has36 uncertain sections;
  no false opaque/clear classification was forced. Actual WARP nearest-section BVH and current immutable GPU
  publication pass CPU-reference, unknown, weak old CPU lease expiry, invalid publication, frame/device and
  release gates (step19-gpu-test1.log). FXC point keyword/early-return dataflow errors were corrected; native
  domain compilation needs bitwise finite checks instead of isfinite on a known constant radius (same pattern
  as ray_scene.hlsli). Added normal DsWaterWorld and local contact in continued wave sampling; compiling now
  (step19-world-build2.log). Nothing in this19 shader integration is installed/accepted yet; native cuts/limits
  remain pending and checkbox19 is still open. Owner flagged excessive infrastructure/testing: focus remaining
  work on visible19..23 changes, repeat only checks relevant to actual changes/failures, then spatial25.
- Step19 first world-shore build6D4B4530... installed both, but native-shore-v1 FAILS at the actual barge:
  bank images render, barge world frames expose underwater black/sky geometry while debug27 still draws
  water. This is NOT accepted and19 stays open. Both scene and line publications say ready/complete, but
  gfx.state reveals the actual failed link: bridge OFF after Direct3D11 passes late10s/10frames, GPU frame
  ~1058ms. Nested BVH contact queries inside the retained optical height solver made the pass too expensive;
  do not blame reflection color or alter accepted wave/ray settings. Need exact local spatial bins for the
  bounded1m contact query, retaining BVH as a generic wider-radius reference; no quality cut or timeout bypass.
  native-shore-debug-v1 captured0, then waited indefinitely once the bridge was off. Stopped only its verified
  Python pid21628; on=false/debug0 and capture clock released, gfx.reset requested for1.2. Preserve original
  owner config/time/window/camera checkpoint for final handoff. The game itself did not crash (pids unchanged).
- 2026-11-03 resumed after owner-reported shutdown: both labs were stopped; restarted1.2/1.0 in background,
  water kept off until replacement installed. Step19 now indexes exact plane sections into local CSR cells
  expanded by the supported1m query radius; this changes candidate selection, not distances/waves/ray counts.
  Five owned GPU buffers and80-byte constants publish atomically. WARP local cells vs CPU/BVH, negative cell
  boundaries, smaller/wider radius, uncertainty and existing publication lifecycle pass (step19-grid-test1.log).
  Normal pixel/domain paths use the local-only entry; generic wider queries retain BVH. Native frame helper
  now fails on graphics-bridge shutdown/lateness instead of waiting forever or accepting counters as pixels.
  Main shader compiling in step19-grid-build1.log; native barge acceptance remains pending,19 still open.
- While the long normal-shader FXC build runs, prepared (not integrated/installed) step20 path-footprint
  providers: affine/billboard-aware barycentric differentials, original-texel elliptical mip/anisotropy
  sampling and candidate/final-hit context. Snapshot now owns native MAXANISOTROPY, extra.y carries it in
  the existing GPU stage layout; no ABI change. The footprint sampler compiles independently
  (step20-texture-compile1.log). Native texture harness has focused dimension/mip and anisotropy cases queued.
  These are preparation only: no step20 gate or native integration is claimed before19 passes.
- Step19 CSR build3F980D61... hash-installed BOTH. native-shore-grid-v1:1.2 bank flat/waves, FOV40/85 and
  actual barge-level images render with zero magenta; native water remains visible (bank GPU~11-74ms,
  barge-level~192ms). Barge-DOWN still fails: bridge late/off after11s/9frames, no down image accepted;
  helper now catches the real shutdown.1.0 was not reached, so19 is NOT complete. No timeout or quality cut.
  Next bounded change keeps the identical three inverse-displacement iterations in a dynamic loop for
  WORLD only, avoiding shader-code duplication and register pressure. Legacy accepted comparison unchanged.
  Step20 SDK optional context/observer/texture hooks prepared, default behavior unchanged; no water integration yet.
- Focused step20 material harness passes (step20-material-test1.log): path dimensions/mips/native formats,
  native anisotropy bounds plus the existing encoded-stage/alpha/blend checks. Source is still not connected
  to normal water. FXC rejects zero-argument function macros: context callbacks now take a dummy hit argument;
  step19-loop-build1 failed immediately at preprocessing, corrected in build2. No runtime install from either
  loop build yet.19's bank cases need not be repeated in1.2: next native gate starts directly at the failed
  barge-down case, then completes1.0 coverage.
- Inspection found the other shader duplication multiplier: seven bisection refinements also unrolled the
  world-contact/height solver. Stopped ONLY the verified in-progress PsWaterWorld FXC pid18764 (build2) to
  avoid waiting for an intermediate shader that keeps this multiplier. Generic water_trace.hlsli now accepts
  MML_WATER_TRACE_REFINE_LOOP, defaulting to the original unroll; world wrapper opts into loop. ALL original
  three inverse iterations,96 search steps,7 refinements, ray counts and bounce bounds remain. Build3 running;
  no wait-limit change. Legacy shader equality and native down pixels still gate acceptance.
- CSR3F980D61...1.0 native-shore-grid-v2 completes all10 frames: bank flat/waves, FOV40/85, actual barge level/
  down vs accepted comparison. Native down/FOV images inspected: water present, zero magenta, bridge on.
  Complete1.0 result passes;1.2 still requires down with ORIGINAL owner's wave settings after compact loops.
  Added owned native MAXMIPLEVEL/LODBIAS beside MAXANISOTROPY for step20 (existing GPU extra.zw slots),
  not consumed by the current material yet. Reference: Microsoft D3DSAMPLERSTATETYPE and D3D11.3 spec5.9.5;
  MIPFILTER NONE ignores mip selection (ANGLE's D3D9 ConvertMinFilter confirms the native level0 behavior).
- Step19 COMPLETE as a mechanism gate: compact-loop build0ECC4EBF8573A510A1F3FB1573AC44B5A575DFFD0139DBE55FECB9F96EED0DCA
  installed/hash-matched both. Native-shore-loop-v1/v2 BOTH produce actual down-view water, zero magenta,
  bridge stays on.1.2 owner's original wave settings retained; no timeout/ray/iteration quality cut.
  World PS instructions20955 ->10537, temp vectors311 ->157. Accepted comparison SHEX still identical.
  Prior CSR bank/FOV/actual-barge images plus current down regression establish camera-independent contact;
  production resolution/full spatial25 and performance are NOT closed. Step20 integration preimages verified.
  Material harness step20-material-test3 passes added native mip bias/clamp/NONE cases; sampler keyword
  collision in test struct fixed to samplingState. No step20 water integration claimed before this point.
- Step20 integrated in SOURCE, compiling step20-world-build1.log (not installed): normal wrapper imports
  river_world_material.hlsli; each retained facet carries pixel ray differentials and its reflected virtual
  observer through further wave hits. Original native triangle/affine/billboard transform gives UV0/UV1
  gradients; candidate opacity and final selected layer restore the SAME footprint. Native anisotropy,
  mip bias and mip clamp retained; unresolved slopes stay in the accepted facet quadrature, not a second
  uniform roughness blur. New ray_footprint/native_texture_footprint SDK providers are optional; default
  scene-query users keep the original zero-footprint contract. Step21 air helper PREPARED ONLY, not included:
  per-layer actual hit travel, additive attenuation without duplicated inscatter, native sky fallback intact.
  Do not close20 until BOTH native image/mip/coverage cases pass; production resolution/spatial25 still open.
- Step20 build1 World FXC exceeded40 CPU minutes without output; stopped only verified pid3616. New optional
  MML_SCENE_STAGE_LOOP keeps both original texture stages but compiles the world provider as a loop;
  raw/default clients retain unroll. Also propagate scene-query UNKNOWN to a full pixel marker, so a small
  Fresnel weight cannot hide an unsupported path from native acceptance. A failed multi-file patch was
  atomic (verified targets); corrected pixel variable context. Accidentally overlapped build2/build3, then
  stopped ONLY verified own FXC22892/25804 and waited for BOTH builds to exit. One build4 now runs; no20 DLL
  installed. Existing0ECC4EBF... remains both labs. Stage count, mip precision and optical samples unchanged.

### 2026-11-03 — stop reflection work; bound and isolate shader compilation

- Owner explicitly stopped the wait/build/test loop and requested immediate compilation failures instead.
  Verified and stopped only this repository's cmake23040/MSBuild11460/FXC17256. Build4 had finished
  PsWaterWorld, then spent time in PsWaterGeometry; no step20 DLL installed. No game launches/tests/deploys.
- Audit: mml_shaders had no timeout, wrote final headers directly and declared every neighboring/SDK HLSL
  as a dependency. Thus a small material edit rebuilt unrelated entries. PsWaterGeometry also calls PsWater
  before native traversal, so this diagnostic is not a small compilation unit. FXC optimization of large
  nested/inlined material/traversal code is the costly phase; preprocessing measured 0.05–0.08 s. The exact
  optimizer internals are not established; no claim that the heavy shader itself is now cheap.
- Hash-verified preimages: build/shader-build-guard-20261103. Added tools/compile_shader.py and CMake
  wiring: actual preprocessor depfiles, target-aware DXC preprocessing (FXC /P refuses /T and /E),
  10/60 s phase limits, exact-child
  termination/reaping, atomic good-header publication and concurrent-writer rejection. Timed-out exact
  inputs/options/compiler are recorded and refuse a fresh optimization attempt. No O3/WX/quality fallback.
  Isolated <target>_shader_<name> targets avoid asking for a whole mod before its changed heavy entry passes.
- Five focused tests passed in 0.25 s (kill/reap, lock refusal, unchanged timeout retry, changed source
  retry, concurrent-edit output rejection, dependency escaping). Initial test-only Windows lock/read and
  StringIO/fileno errors were corrected. Real PsWaterGeometry probe failed at its deliberately short 2 s
  limit and its unchanged retry skipped optimization; no FXC left alive. Real CMake VsWater succeeded
  in 0.05 s, repeat target was a no-op; DXC PsPulse ps_6_0 succeeded in 0.12 s. No full water build.
  Standalone Rust mml-build is explicitly outside this CMake change. Reflection step20 stays pending;
  existing 0ECC4EBF water remains the last installed version.
- Final end-to-end MSBuild check used only mod_river_water_shader_PsWaterGeometry with a temporary 2 s
  limit: exit1/MSB8066, original header SHA256 unchanged; immediate repeat again exit1 after preprocessing
  without starting the optimizer. Restored MML_SHADER_TIMEOUT=60 in the x86 cache. DXC with explicit target
  preprocessing also passed. No compiler/build processes remain. Reflection work stays stopped at owner request.

### 2026-11-03 — resume reflection20; isolate FXC optimization cost

- Owner authorized continuation. Both existing lab processes are alive (1.2 pid3256,1.0 pid19120), no stale
  compilers. No new water DLL deployed. Verified preimages in build/shader-compact-20261103.
- Optional MML_NATIVE_TEXTURE_COMPACT retains the exact point/bilinear/trilinear texels and lerp order, but
  loops over taps/mips instead of inlining every raw/BC decoder eight times. The anisotropic provider has
  one shared sampling call site. Focused native_material_test passes all1512 native texture samples, formats,
  address/filter/mip/bias/clamp and anisotropy checks, plus alpha/cull/blend regressions (material-test1.log).
- Diagnostic PsWaterGeometry no longer calls full PsWater merely to obtain depth/motion before replacing
  all its color. It computes the same primary repair/depth/motion explicitly. Normal wave/lighting policy
  is unchanged. Normal World excludes the legacy planar branch and unrelated painted-sky/probe entries at
  preprocessing; old PsWater remains the comparison. View registration cleanup still belongs to23.
- World builds2–5 each stopped at the unchanged60 s limit; no automatic timeout increases/retries. Compact
  texels, localized fastopt traversal/outer-loop hints and reusing the winning candidate's already evaluated
  encoded color did not independently resolve the optimizer cost. The selected color/material cache follows
  the same hit save/restore as footprint context; default SDK clients keep their original evaluation.
- Isolated query-only geometry/opacity/radiance probes also exceed10 s without water illumination, narrowing
  the cost to the query/provider unit rather than primary medium/foam. They are compilation probes only,
  not native reflection acceptance. World build6 excludes obsolete code in SOURCE; no success claim yet.
  Initial compact-array scalar initialization was rejected by FXC and corrected to explicit float4 zeros;
  one multi-file patch failed atomically, verified before retry. All evidence remains in shader-compact-20261103.
  Microsoft documents fastopt loop-analysis behavior: https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-for.
- Concrete isolation: pure geometry compiles in0.59 s. New SDK scene_radiance_ordered.hlsli separates
  geometry from material evaluation: strict (distance, descending draw ID, primitive ID) cursor, alpha-reject
  continuation without epsilon tMin jumps, same32 contributing-layer bound and source-draw dedup. The world
  provider now saves only the selected affine instance, derives its footprint after traversal, and samples
  that material once. Earlier encoded-candidate cache trial is superseded. Default scene_query is unchanged.
  Ordered query plus footprint compiles in6.23 s. Scene-blend WARP fixtures pass original coplanar/alpha/
  additive/unknown/order/32-layer cases and new80 rejected shared-edge cutout triangles before an opaque
  background, plus rejection of a later coplanar draw revealing the earlier draw. Duplicate-draw filtering
  was subsequently moved outside traversal too; blend-test3 passes. No native water acceptance yet.
- Full world shader builds7–10 still stop at60 s; no stale headers installed. Rolled normal-field gathering
  preserves the exact three spatial samples/finite differences at a bounce. All3/96/7 steps and optical
  samples remain. An explicit O1 diagnostic also exceeds its10 s probe limit; it was not installed and no
  production optimization flag changed. SM5 [call] switch support was verified with actual call/label code;
  build11 outlines physical wave and native-object queries at their existing conditional call sites.
  Function-level [call] is rejected by FXC; only the documented switch attribute is used. This is pending
  compilation, not an accepted optimization. https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-switch.

### 2026-11-04 — owner preview and usability priority

- SM5 switch outlining failed with FXC internal storage errors. Local layer history and D3D11 dynamic linkage
  still failed the bounded full shader build. Removed both outlining and the unbound linkage trial. Isolated
  water-height trace compiles in1.59 s, ordered material query in6.23 s. Step20 is not deployed.
- Opened 1.2 foreground at the owner's1440x810 on the hash-verified step19 0ECC4EBF build. Restored water on,
  free camera and DLSS. The user liked the image and reported1 FPS. Native frame W:/MafiaLAB/mml/shots/1.jpg
  showed1188 ms. Profile: RENDER_WORLD1057 ms inclusive, observer972 ms inside it. A2 s CPU sample put54.6%
  in TextureSnapshot::Verify. GPU surface135 ms in the pictured frame and320 ms in another. The bridge
  stopped after repeated late passes. No current CPU/GPU speedup is claimed from the compile experiments.
- Owner explicitly moved lab usability ahead of the remaining reflection steps. A deferred managed-texture
  trial was superseded before deployment: the lab's dxwrapper exposes managed resources as default-pool
  copies. The pinned wrapper now publishes a nonzero content/lifetime revision when its tracked copy is
  stable. Untracked textures retain byte verification. Mesh lookup uses word hashing followed by exact
  equality. The dxwrapper test verifies writable-level invalidation, a new token after the write and stable
  bytes/token across read-only locks and three resets. Source preimages in
  build/shader-compact-20261103/before-playable; verified lab and build-output copies in
  build/playable-water-preview/lab-preimages.
- Both labs received dxwrapper D46555DB... and the CPU-only water preview 4FA9CC26..., then hot-reloaded
  water 84768FAF... with layout reuse. All 14 embedded shaders match the accepted 0ECC4EBF... bytecode.
  Native settings were preserved. Reordered and subset texture tables now share their immutable pages
  with remapped current bindings. Scene vectors reserve from the last successful collection. The WARP
  ray scene suite passes, including reordered cutout colors and a removed texture ID remaining unknown.
  Layout source and lab preimages are in build/playable-water-preview/before-layout-reuse and
  build/playable-water-preview/layout-deploy-preimages.
- 1.2 live snapshots showed about120 MB/frame before layout reuse and43 MB in a frame with reused pages
  after it. A new texture can still trigger a full repack. Moving city view remained about2.6–2.9 FPS.
  With the world briefly held at the same bank camera and 1.0 closed for the measurement, current frames
  reused11 GPU buffers and uploaded64 bytes, with no mesh or texture rebuilds. Surface timing reached178 ms
  in that view, and scene collection remained the main CPU cost. A separate 2 s live sample had83.3%
  inclusive in SnapshotBuilder::Collect and21.4% in CopySceneGeometry. The graphics bridge stayed active
  through the short test. The time and camera holds were released. Preview usability is still open.

### 2026-11-04 — reflection filtering, focused checks

- Owner resumed step20 and deferred final motion acceptance while the labs run at3–4 FPS.
  The selected native instance now carries only its buffer index through BLAS/TLAS. Fetch the
  affine transform and compute the same path footprint after rejecting duplicate draws, just
  before sampling their material. The32-layer history uses a bounded loop. World-only optical
  and medium loops retain all iterations and samples without forced expansion.
- The WARP blend fixture calls the production water material provider. An identical world hit
  returns the original red mip with a narrow footprint and the blue minified mip with a wide one.
  Changing the filtered mip's alpha reveals the deeper geometry. Coplanar ordering, shared-edge
  deduplication, alpha/additive blending,32 contributing layers and80 rejected cutouts pass.
- The isolated complete path, including wave continuation and native material filtering, compiles
  in23.67 s (build/shader-compact-20261103/path-filtering-restart.h.log). The full PsWaterWorld
  still times out at60 s. Bounded O0/O1, old FXC, flow-control and switch outlining probes did
  not produce a full shader. Removed the experimental outline. Labs retain84768FAF... and the
  previous filtering. No live motion or image acceptance was run. Source preimages are in
  build/shader-compact-20261103/before-filtering-restart.

### 2026-11-04 — reflection filtering installed in the labs

- The first split kept primary wave intersection and reflected transport in the same shader and
  still hit the compile limit. The installed split rasterizes the exact ray and its quad differentials,
  traces and accumulates each retained facet in compute, then composes the current surface. Native
  geometry and materials stay in world space. Shader compile times were2.22/24.20/4.84 s for
  PsWaterSeed/CsWaterTransport/PsWaterWorld respectively. No timeout override or optical sample reduction.
- The first runnable DLL 5DB98C7D... was built with the three current production shaders and verified
  accepted bytecode for the other entries. The normal target had reached a separate compile timeout
  in the old geometry diagnostic. Preimages in build/shader-compact-20261103/transport-deploy-preimages.
- Hot-reloaded 1.2 (pid26476, water generation3), deployed to 1.0 and launched its Run profile at800x600.
  Inspected normal-mode water at the bank in both labs, W:/MafiaLAB/mml/shots/3.jpg (1440x810) and
  W:/Mafia10LAB/mml/shots/1.jpg (800x600). Both published current native geometry, advanced the water
  draw counter and reported the graphics bridge active with no late frames during the check. Camera
  holds released. Motion, visual parity and frame-cost acceptance are deferred.
- Split the diagnostic geometry/opacity and layered-radiance entry points in both PS and CS.
  The normal x86-release mod_river_water target now builds entirely from source at the existing
  per-entry60 s limit. Its three production transport headers are byte-identical to the tested
  preview. Hot-reloaded the standard DLL AD227F370568AD904C6AF00C55D9C322F6E8566C4703682C4B6438EBC8C646B5
  into 1.2 generation4 and 1.0 generation2. Verified installed and live file hashes in both labs.
  Both advanced the normal-mode draw counter with ready native scene data. An additional 1.2 bank
  capture with both labs running timed out and logged three late passes, then recovered with
  the bridge active. The initial stills remain the visual evidence. No performance claim.

### 2026-11-04 — resolution-preserving water optimization

- The owner moved optimization ahead of remaining acceptance and required original resolution and visual
  quality. At1440x810 the original filter build reached406 ms for water and the bridge shut down. Temporary
  diagnostic960x540 tests before the resolution instruction measured239 ms transport with objects and
  16–26 ms with object transport disabled. Both original resolution and object reflections were restored.
- Added GPU stage timestamps that can be read without flushing even after the bridge stops. Ordinary
  world-water now skips the legacy mirrored city render. Diagnostics retain it. The world path keeps
  ray distance, sample/bounce counts and hit/material filtering. Prepared slabs reuse the reciprocal
  per world ray and local instance, with conservative node traversal at rounded boundaries. Exact
  triangle tests and unknown-coverage box tests still decide the result. Layer traversal skips the
  already confirmed prefix with a roundoff margin and skips the duplicate scan before the first layer.
- Both focused WARP suites pass native mip/filtered alpha, ordered coplanar layers,80 rejected cutouts,
  moving/scaled/mirrored geometry, unknown coverage and billboard bounds. Full normal target builds.
  On the fixed1440x810 bank view, sampled water stages changed from418 to147 ms and transport from324
  to98 ms after the slab/capture changes and a lab restart. The native scene was not byte-identical
  between runs. This is GPU stage evidence, not a paired end-to-end speedup. The bridge still later
  timed out in the Direct3D9 copy. A follow-up CPU sample attributed50% inclusive to the shadow pass.
- Read-only vertex/index shadow unlocks bypass the write-watch commit. On the updated host, plain
  writable locks also bypass the driver when their written pages exactly equal successfully uploaded
  bytes. Comparison storage is capped at32 MiB and freed with the buffer. Rings retain their original
  NOOVERWRITE/DISCARD handling. The extended vb.selftest passed in 1.0's menu, including nested read
  plus write, identical plain writes, changed bytes and the existing rename/discard checks. The in-city
  read-only host had already exhausted its ordinary copy budget, so its earlier full selftest reported
  no rename, while the fresh menu run passed. Verified backups are under build/water-optimization-20261104.
- When the graphics bridge is off, water drops its source residency and its shadow-map request. They
  return before drawing the first frame after bridge recovery. Active shadow settings and quality stay
  as configured.
- Both labs now run host806EF8DD... and water8B9C4D67..., installed hashes checked. 1.2 restored the
  user's mission and1440x810 window, and every water setting compares equal to the checkpoint.
  The fixed heavy bank check still timed out on the D3D9 copy. During the labeled window the host
  skipped2,514 identical writable unlocks and112,504 read-only unlocks, with2,309 device commits and
  3,538 ms accumulated waiting on busy copies. After the failed-bridge cleanup, several subsequent
  frames took15–19 ms with the replacement pass unavailable. Returned to the owner's camera and
  reset the bridge. It stayed on in the restored view at about2.6 FPS. 1.0 rendered the bank at
  its original800x600 with no late frames in the short native smoke test, and skipped5,394 identical
  writes since startup. No continuous-motion acceptance or stable bank FPS claim.

### 2026-11-04 — Full-quality water performance gate

- The 1.2 owner view at 1440x810 initially averaged 384.2 ms over 600 frames (2.60 FPS).
  Source collection cost 280.2 ms inclusive, of which draw observers accounted for 182.6 ms.
  The water graphics pass averaged 35.7 ms on CPU and the host bridge 62.7 ms inclusive.
  The shadow pass averaged 0.5 ms in this view. The previous bank profile had a much heavier shadow pass.
- Coalesced plain source-scope writes in vbrename and allowed a busy dynamic buffer to repopulate
  initialized pages through DISCARD. Extended vb.selftest passed in both labs. It verified the shadow
  during the transaction and checked all bytes of the final single device publication. The 1.2 run
  showed 238,596 deferred unlocks and 18,148 source flushes since launch, with no buffer-lock waits
  over that startup and ordinary-view sample. Its 3.40 FPS aggregate includes startup and is not the
  controlled river-bank benchmark. The first build was installed into both labs.
- Installed the exact source-byte geometry cache in both labs. The subsequent 1.2 profile still assigned
  75.8% of sampled CPU time to source collection. The bank check timed the bridge as late for 14 frames
  and did not produce a fresh bank image. Returning to the owner's camera and resetting the bridge
  recovered water publication. No frames with the bridge off count toward the performance target.
- Split the four repeated water seed raster passes into two full-precision raster passes. They store the
  original quad's surface and adjacent optical frames. The same four optical samples and world paths run
  in compute. At the lab 1.0 baseline of 800x600 with the bank camera and held wave phase, the GPU seed
  sample changed from 19.87 to 10.26 ms and the full water GPU sample from 72.16 to 55.46 ms.
  These samples came from two runs with the other lab active. The two 800x600 JPEGs agreed to a mean
  maximum-channel error of 0.14/255 in the lower half. They are `mml/shots/1.jpg` and `mml/shots/2.jpg`
  in the 1.0 lab. The new seed layout adds four RGBA32F slices, about 71.2 MiB at 1440x810.
- The new GPU table upload can clone shared byte ranges on-device and upload only changed bytes into a
  private candidate. WARP readback of a partial update plus a new tail passed for both the current and
  previous buffers. The focused ray/material and ordered blend test executables passed.
- The new 1.2 bank run at 1440x810 sampled 78.78 ms for a completed water pass with a 16384 shadow
  map bound, but did not sustain water publication. Five labeled wall intervals spanned 2.03 to
  7.51 seconds while CPU execution occupied 353 to 471 ms per frame. This is a bridge/paging lead,
  not a measured improvement to FPS. The bridge's shadow-map alias now retires before a replacement
  map is allocated. The map dimensions, precision and caster drawing are unchanged.
- The native C/C++ and Rust probes ran in both labs. The C++ probe compared 224 captured material
  details in 1.2 and 216 in 1.0 against the live D3D9 sampler and render state with zero mismatches.
  The old geometry prefix and its untouched suffix sentinel also passed. Rust copied 22 real draws
  in each lab. Test mods were unloaded afterwards.
- After the shadow-map lifetime change, the next bank run sampled completed water at 106.36 ms,
  with the current bridge reporting 12 late frames and no fresh snapshot at the end of that sample.
  Labeled wall intervals after settling were approximately 470 to 725 ms. The owner's restored
  ordinary view held an active bridge at about 333 ms per frame. These are separate native runs.
- The one-dispatch experiment kept all optical paths but exceeded the existing 60-second compiler
  limit on CsWaterTransport. It was reverted before deployment. The two-raster, four-dispatch
  implementation remains the verified baseline.
- The ordered ray consumer can now traverse its existing BVH in either direction. The split axis
  at the root selects a closer-first order. The same triangles and tie rules apply. The targeted
  shader compiled in 51.16 seconds. WARP passed the reverse TLAS and rebased BLAS with its nearest
  hit, cutouts, coplanar ties and ray-hit footprints. Native timings are the next gate.
- With the larger source cache and direct shadow reads, the 1.2 ordinary sample reported 8.53 million
  exact cache hits and 8.99 million shadow reads by frame519. One warm rolling sample averaged
  273.35 ms with an active bridge. A later profile included hitches and averaged 391.42 ms. In that
  profile OwnMesh occupied 6.8% of CPU samples compared with 16.7% after the earlier cache, while the
  complete collector remained the dominant CPU work. Neither measurement is the river performance gate.
- The native 1.0 A/B run could not compare traversal speed. Both builds hit 0x8007000E in the
  texture-page-build stage and had no fresh water. Keep the 32 MiB normalized cache on a 2 GiB
  process. The texture publisher can stage the identical mip pages in 4 MiB pieces after a failed
  contiguous CPU allocation. Native verification remains required.
- A local hardware probe on the NVIDIA GeForce RTX 3070 (LUID 0:D62C) reported DXR tier enum 12 and
  shader model 6.5. The x64 inline RayQuery prototype passed nearest-hit distances and barycentrics,
  moved placements, deterministic coplanar ties, candidate rejection and a miss against the software
  reference. The prototype has no game integration or native frame-time claim.
- A full x86 rebuild hit MSVC C1060 with unbounded intra-project /MP. The local retry caps the
  compiler at four jobs using `-- /p:CL_MPCount=4`. The shader compiler's 60-second limit stays in place.
- The 1.0 river run at its original 800x600 recovered a fresh water publication with the bridge active
  and a 4096 shadow map bound. Its measured completed water pass was 78.6 ms. CS2 was active during
  the run, so it is evidence of native functionality, with no comparable FPS result.
- A source-scope binding mirror avoids repeated device getters for adopted stream and index buffers.
  Raw duplicate bytes are released after a successful producer-revision proof. Water now shares local
  BLAS data across identical instances while retaining the material and primitive order of each draw.
  The targeted transport shader compiled in 48.73 seconds. WARP matched copied geometry, per-instance
  material, primitive order and filtered cutouts. Both labs received the matching host and water binaries.
  `vb.selftest` passed in both. Bounded TLAS refit then passed a WARP comparison with a full rebuild,
  including a moved occluder, and rebuilt when an occluder was removed.
- With CS2 closed and only the 1.2 lab running, the 1440x810 bank view presented 36 frames averaging
  303.53 ms (3.29 FPS), with 240.64 ms on the game thread and p95 of 339.25 ms. Water covered about
  349,000 pixels, with fresh reflection data, a bound 8192 sun map and zero late bridge frames in
  this sample. DLAA was configured at native size, but the bridge reported zero produced DLAA frames.
- The controlled comparison held the same 1.2 river camera and paused simulation. With the mod on,
  the two arms averaged 280.57 and 285.83 ms (3.56 and 3.50 FPS). With stock water, the middle arm
  averaged 17.89 ms (55.90 FPS). The water collector alone took 133.09 and 138.84 ms, including
  62.15 and 64.30 ms in callbacks for 15,957 source draws. The bridge took about 130 ms, inclusive
  of about 30 ms for the water pass's CPU work and the GPU handoff. Water GPU surface timings
  were 73.19 to 84.24 ms (seed 24.26 to 27.19, ray transport 35.88 to 42.11, composition 13.05
  to 14.94). Stable geometry still took about 25 ms to publish despite only 64 uploaded bytes.
  The shadow map averaged 28.40 to 30.07 ms, a separate diagnostic with possible bridge overlap.
  Restored the mod setting, original game time and camera. Treat this as a cost isolation test,
  not the moving-water acceptance sample.
- The hot reload while CS2 was active reached `0x8007000E` allocating the transport texture.
  A clean start published water. The next revision splits the twelve full-precision seed targets
  across three arrays of four, reducing the largest individual allocation without changing samples.
- The next package preserves physical BLAS slots across small changes, appending replacement geometry
  and rebuilding compactly when excess data would exceed one eighth or 8 MiB. Only current instances
  reference those slots. The offline snapshot test verifies that a changed mesh uses a new slot, the
  unchanged mesh keeps its root, and no current instance references the retired slot.
- Both labs received river_water SHA256 `19664B3278CC80B84D61CB00C78DED1D4DC953E0898CF913625A1D9E9F0A757C`.
  Only 1.2 ran after this deployment. The 36-frame native river sample at 1440x810 averaged 294.87 ms
  (3.39 FPS, game-thread CPU 229.45 ms). Four sampled frames published fresh water with an 8192 sun
  shadow, an active bridge and zero late frames. Three uploaded 2.74 to 2.94 MB and one uploaded
  21.79 MB. Publication still cost 22.48 to 26.21 ms. The preceding moving sample uploaded about
  22 MB per observed frame. These separate runs do not establish a sustained FPS improvement.
- Instanced water collection now builds bounded local BVH jobs on the Windows thread pool while the
  game thread resolves more native draw groups. The x86 publisher streams native texture banks of at
  least 32 MiB in chunks of up to 4 MiB. The serial and threaded local-tree fixtures match byte for
  byte. The existing WARP test also compares every streamed mip and its padding to the packed upload.
- In 1.2 at 1440x810, the first worker build published live water with zero late frames. Across four
  observations it built 193 to 212 local trees on workers and waited 1.01 to 1.25 ms for them. Native
  collection still took 120 to 122 ms, geometry copy 32.6 to 33.1 ms within collection, and packed
  placement 13.7 to 20.7 ms afterwards. The 36-frame sample averaged 291.64 ms (3.43 FPS). The
  preceding 3.39 FPS sample had different water coverage, so this is not evidence of a frame-rate gain.
- The same worker lane now computes half of the current instance transforms in parallel, writing into
  the original output array. The serial and parallel instance and TLAS fixtures match byte for byte.
  Local BVH construction writes sorted triangles into their final buffer, avoiding another triangle array.
- The 1.0 lab at 800x600 published the current scene with zero late bridge frames and computed about
  7900 instances on the worker. Its game-process private memory was about 1.25 GiB in the sample.
  The native source-copy stage still occupied 1.02 to 1.47 seconds per sampled frame. A native stack
  sample put 68.9% of samples directly in CopySceneGeometry. Hot PCs resolve to geom.cpp:658-662,
  the vertex decode loop, rather than the index-buffer lock. The compact indexed-source path decodes
  only referenced vertices for sparse spans and retains no raw copies of the skipped range.
- The first compact 1.0 run skipped 19.6 million unreferenced vertices over the process lifetime. Its
  sampled copy stage was 667 to 809 ms with about 51 to 52 MB of retained mesh cache, compared with
  76 to 77 MB before compaction. Direct decoding preserves float bytes and uses the exponent field for
  finite-position validation. Counters showed about 510,000 of 814,000 decoded vertices from native
  read-only locks rather than CPU shadows in the next sample.
- A 64 KiB contiguous scratch stage for native locked reads lowered the observed copy stage to
  367 to 552 ms in a separate 1.0 river run. It averaged 702.41 ms per frame (1.42 FPS) over 13 frames,
  with fresh water and zero late bridge frames. Private memory in that sample was 1.23 GiB. The older
  direct-decode run averaged 1007.17 ms across nine frames. Native content varies between these runs.
- An SSE4.1 streaming-load experiment averaged 927.10 ms in the same 1.0 bank view and was removed.
  The retained path breaks sparse driver reads at gaps larger than one unused vertex, with the same
  single 64 KiB scratch buffer. The next native run averaged 503.47 ms across 14 frames (1.99 FPS),
  with the source copy at 220 to 246 ms, current water and zero late frames. Private memory was
  about 1.26 GiB. The simulation remained live in these runs.
- The 1.2 lab on the same package at 1440x810 averaged 341.15 ms across 26 live frames (2.93 FPS).
  It ran 139 to 192 mesh jobs and about 7800 instance transforms on workers in the observed frames.
  Its sampled native copy was 35 to 37 ms and collection 137 to 145 ms. The whole-frame number
  includes high GPU variance. DLAA remained configured but produced zero frames in this sample.
- A second 1.2 comparison used the same restore point, fixed camera, paused simulation, and held
  wave phase. Both arms ran the same threaded water DLL. The preceding loader averaged 275.30 ms
  over 20 frames, and the compact/staged loader averaged 266.82 ms over 22 frames, with zero late
  frames in both. Observed mesh cache dropped from 75.30 MB to 50.03 MB. Triangle counts and visible
  water coverage differed slightly between launches, so treat this as a check on cost and memory,
  not a visual-equivalence or moving-water FPS result. The newest host and water DLL were restored
  to both labs, with their hashes matching the build. Only 1.2 was left running.

### 2026-11-05 — Native index revisions and bridge timing

- The previous 1.2 main-thread profile assigned 59.9% of samples to the source collector. Native
  `m_RenderFGroup` accounted for 42.7% inclusive, geometry copying for 16.3%, `GetIndices` for 4.6%,
  and D3D buffer locks for 5.5%. `UnlockAll` accounted for about 1.5%. The old host had no verified
  index ranges, despite 58.0 million confirmed vertex revisions over its run.
- The candidate tracks the whole-buffer revision for unrenamed, non-shared index buffers whose
  driver hooks can observe all writes. It skips the driver lookup and lock when the same live binding,
  revision and range match the normalized cache. Untracked and changed buffers keep the checked read
  path. The cap is 4096 small metadata records. No CPU buffer shadow was added.
- The x86 host built as `40EE812027DD1DB12D19C3B319B0878A3340A8D5D830B07A44F46DD15C3A7BF4`.
  Lab 1.2 received it with the previous water DLL. At the 1440x810 bank camera one live observation
  recorded 234,843 native index proofs across 234,899 shadow reads, with 85.51 ms collection and
  7.76 ms within it for geometry copy. A second observation measured 80.72 and 6.95 ms, with 267,026
  proofs across 267,063 shadow reads. The preceding moving run measured 137–145 and 35–37 ms.
- No comparable FPS result yet. Both runs eventually disabled the bridge on consecutive D3D9 query
  timeouts, and the window contained frames without water. DLAA produced 181 frames at the native
  resolution before the bridge stopped. A previous-loader launch had also failed inside the game's
  texture conversion, before rendering the river. A standalone `vb.selftest` attempt could not create
  a D3D9 device in that session. The next runtime gates are native revision lifecycle in each lab and
  a fresh-water timing window with the bridge active throughout.
- The extended `vb.selftest` passed inside both native labs. The 1.0 river view at 800x600 averaged
  227.85 ms (4.39 FPS) over 45 live frames, with fresh water and zero late frames. The geometry copy
  was 9.47–9.60 ms within 119–130 ms collection, with 807,719 proofs across 808,341 shadow reads.
  The previous separate 1.0 run averaged 503.47 ms, including 220–246 ms geometry copies.
- A 1.2 diagnostic with DLAA temporarily off at native 1440x810 averaged 226.56 ms (4.41 FPS) over
  46 live frames and produced fresh water with zero late frames. The index-only host spent 77–79 ms
  on collection and 6.9–7.0 ms on geometry copy. The bridge's differential mean was 0.435 ms wait9,
  63.37 ms wait11, 0 ms hand-over and 0.42 ms Present. GPU graphics passes averaged 63.06 ms.
  DLAA was restored and its config hash matched its preimage. Its native D3D9 timeouts in the bank
  view remain reproducible. Frames without fresh water are excluded from the quality gate.
- Source collection can keep an identical driver buffer binding when the actual copy, stream range
  and logical entry match. It can also defer world-transform writes until an actual consumer asks
  for them. The state block and UP paths still revoke cached bindings, and getters, vertex processing
  and state blocks flush deferred transforms. In 1.2 the `C6D2B52E` host passed `vb.selftest` and had
  no late frames during a frozen native-size diagnostic. Collection was about 68 ms in three of four
  observations, versus about 70–73 ms on the preceding host. Frame median was 211.88 ms with a
  slightly different resident scene, so the result does not show a whole-frame speedup.
- A separate water DLL experiment shared the sky sample and evaluated PCSS once per covered pixel.
  The same 1.2 process and index-only host with the held camera, time and waves measured median
  209.23→208.92 ms (20 and 19 frames), with no late frames. GPU water median was 63.53→63.24 ms
  across four samples. The later restore changed triangle counts from 246,060 to 250,341. JPEG100
  screenshots had mean maximum-channel error 0.000123/255 in the bottom half. The performance gain
  was unproven, so the lab's original `B1BFBE22` water DLL and its PDB were restored and verified.

### 2026-11-05 — Native placement, ordered raster and DXVK material cost

- A reusable, bounded traversal table removed per-frame visited-node allocations while keeping
  traversal order and the 200,000-frame guard. On host `57BA2C4B` 1.0 passed `vb.selftest`. Its
  live bank sample averaged 191.76 ms (5.21 FPS) across 42 frames, with 45 fresh water frames and
  no late frames. Collection took 90–96 ms and copy took about 7.8 ms. Previous samples used a
  different resident population. No supported geometry was intentionally excluded.
- Ordered raster v2 in 1.2 with 247,556 triangles gave the same 198,000 water pixels in matched
  JPEG100 crops across a two-pass control and two ordered-pass frames. The median seed cost
  was 22.143→15.128 ms and GPU water 65.369→57.753 ms, with zero late frames. The previous
  ordered variant let FXC move gradients into a divergent branch and failed that equality check.
  In 1.0 the selected ordered pass produced 46 fresh water frames with zero late frames. Seed
  took 10.39 ms and full GPU water 37.59 ms in the observed frame. The 43-frame live sample
  averaged 210.23 ms (4.76 FPS), so it does not demonstrate a whole-frame gain against the
  earlier separate run. The 800x600 frame in the 1.0 lab's `mml/shots/1.jpg` was inspected.
- Exact placement reuse passed `ray_scene_test` on WARP. It reused 1026 matching placements on
  the existing bounded worker path, byte for byte, and recomputed a moved set. A changed local mesh
  kept the other 17 placements current. The next package was host `970CDC81`, water `8EECEEEF`.
- A narrow motion state block restores only its 13 recorded raster states and viewport. Both the
  buffer and render-state mirrors now avoid assuming it changed all bindings. Before the bank
  move in 1.2 the candidate had 20,646 narrow applies and 389 full mirror reads. DLAA still
  timed out on native D3D9Ex in the bank view. The last late frame reported 197 motion redraws
  and 189.5 ms for their CPU dispatch. The failed interval is excluded from the FPS gate.
- A temporary pinned DXVK layout ordered D3D9 and D3D11 on the GPU. DLAA and water published
  with zero late frames, zero Wait11, and about 1.43 ms for 153 motion redraws in one observation.
  The live bank test still averaged 757.42 ms (1.32 FPS), with 536–561 ms in collection. The CPU
  profile put 60.6% of samples in exact mip verification. DXVK's ordinary managed textures had
  no producer revision, so the snapshot repeatedly revalidated their full mip payloads. The
  next wrapper candidate stamps fully initialized managed textures at their observed writes
  without adding a CPU shadow. Until tested, the old wrapper and the lab's original layout are
  backed up under `build/water-native-texture-revision-20261105-014557807` and
  `build/water-motion-state-20261105-012926942` respectively.

- Wrapper `F98B472A` gave 1534 confirmed ordinary managed texture revisions and no managed
  shadow-copy bytes in the 1.2 DXVK river run. With host `970CDC81` and water `8EECEEEF`,
  collection dropped to 74–79 ms. At 1440x810 with DLAA, 37 live frames averaged 342 ms
  (2.92 FPS), with 256.77 ms median, 630.52 ms p95, 40 fresh water frames and zero late frames.
  One publication still sent 38.5 MB to the GPU in 22.65 ms.
- Water `3C1DE954` retained a bounded immutable texture prefix, copied the prefix on the GPU
  and added a deterministic parallel TLAS build on the existing worker pool. WARP matched
  reordered source texture IDs and material readbacks against a fresh bank, and matched
  the serial and parallel TLAS. A small-bank cutout fixture required preserving at least
  one page's worth of retired content so removed texture IDs still get remapped.
  In 1.2 on the same DXVK process, 59 live frames averaged 299.46 ms (3.34 FPS), median
  258.98 ms, p95 619.38 ms, with 62 fresh water frames and zero late frames. The sampled
  collection was 70–74 ms and packing/placement 12.5–13.3 ms. One TLAS rebuild used
  the worker for 8093 placements. Water GPU was 45.73 ms including 23.90 ms for trace.
  These are moving windows with changing resident geometry. The last upload was still
  38.5 MB and 25.17 ms, so the overall speedup and isolated benefit of each change
  cannot be determined from those windows.
- Water `B386E6C7` proves the previously validated BLAS prefix from the exact producer
  lease, skipping repeat scans only when the preceding GPU publication owns that source.
  WARP rejected a public copy with a damaged node, shared the original payload and
  rebuilt on a generation change. The new counters isolated one 25.15 MB upload to
  4.05 MB geometry, zero texture data and 21.10 MB attributes and materials.
- Water `B0DD500D` packs original UV and vertex colors in physical BLAS triangle order.
  The current draw supplies its material. Per-instance primitive numbering, world
  footprints and ordered coplanar ties stay unchanged. The same shader compares
  the two layouts in WARP for coplanar blending, filtered mip selection and alpha
  cutouts, with exact output. The ray and waterline fixtures also passed.
  At a fixed camera, water clock and simulation in the same 1.2 DXVK process,
  both builds published 270,993 logical triangles, 211,395 BLAS triangles,
  11 reused buffers and 64 uploaded bytes. Observed upload CPU time was
  16.77→10.84 ms. With DLAA on, two JPEG100 samples per arm had water-crop mean
  maximum-channel differences of 0.73–0.81 within an arm and 0.88–0.91 across
  arms, so this run does not demonstrate pixel equality.
- The next moving run overlapped a host compile. Two frames had 585–638 ms waiting
  for the small mesh jobs, within 734–773 ms collection. A later frame collected
  in 67.30 ms with a 1.08 ms wait. Water `D3251DE9` lets the game thread claim
  a queued CPU job that has not started, cancels its unused callback and joins
  any active work as before. The pending-job counter is exposed separately from
  blocking wait time. Targeted x86 build and WARP passed. In sequential moving
  1.2 DXVK/DLAA windows without compiler load, B0DD measured 3.45 FPS and 462 ms
  p95 and D325 measured 3.56 FPS and 417 ms p95. Both had zero late frames and
  about 2 ms worker wait. A separate 12-worker bounded CPU stress run with D325
  claimed 252/260 and 244/250 pending jobs on the game thread in two sampled
  frames. Join wait was 0.050/0.026 ms, collection 123–138 ms. The old
  compiler-overlap result is a different stress case. All stress workers exited.
- Water `662D759B` interns equal native material records for the compact
  attribute layout. The fixture shares identical materials and preserves a
  differing alpha reference. A 1.2 sample published 2684 unique records for
  16,027 draws. The material upload was 558,272 bytes, versus about 3.33 MB
  for an equivalent number of individual records. The total upload was
  6.38 MB. At 1440x810, DLAA on and 16384 shadow map, 83 live frames averaged
  150.09 ms (6.66 FPS) with 165.96 ms p95, 86 fresh water frames and zero
  late frames. GPU and scene population varied between runs, so this is a
  checkpoint rather than a speedup attributed to material interning.

### 2026-11-05 — Native source FVF and DXVK shadow transfer

- Host `7E4768FB` remembers the current source FVF only on a verified hooked
  D3D9 vtable. Successful FVF and declaration setters, ordinary state-block
  invalidation, Reset and device changes retain the real GetFVF fallback when
  required. Both labs completed 256 sampled mirror comparisons with zero
  mismatches. Their source counters showed 9.19 million avoided queries in
  1.2 and 8.24 million in 1.0 over the observed sessions.
- The same host removes the late D3D9 CPU fence wait only when DXVK's GPU
  fence orders the actual shadow transfer after the current D3D9 draws.
  In the 1.2 moving sample, mean Wait9 fell from 22.14 ms in the separate
  diagnostic control to zero, and the worst late-shadow fence span from
  101.7 ms to zero. The 71-frame sample measured 3.68 FPS, 378.15 ms p95,
  70 fresh water frames, 69 DLAA frames, both 8192 and 16384 shadow maps, and
  zero late frames. Post-shadow latency could still reach 515 ms.
- Lab 1.0 ran `7E4768FB` and `662D759B` with the same wrapper `F98B472A` on
  native D3D9Ex at 800x600. It averaged 184.78 ms (5.41 FPS) over 62 moving
  frames with 207.83 ms p95, 65 fresh water frames and zero late frames.
  `vb.selftest` passed, and the river image was inspected with the test
  camera still applied. The previous 1.0 wrapper is backed up under
  `build/water-native10-wrapper-20261105-025315154`.
- Host `265FA5B3` folds native texture/frame names during rule matching and
  precomputes main-view rule masks. A mixed-case, views-only frame rule in
  1.0 matched one group/10 faces, and removing it restored both original
  rule handles. The test had 47 fresh water frames and zero late frames.
  Both labs received the same host, water DLL and wrapper, with only one
  running at a time. The 1.2 lab retained the reversible DXVK layout.
- The optional Vulkan timestamps in `265FA5B3` isolated one 16384-square
  R32F shadow handover spanning 178.008 ms on the GPU. Latest CPU submit
  for the same texture size was 0.140 ms. The 1440x810 frame-input copies
  measured 0.049 ms and return copies at most 0.105 ms in the sample.
  The whole 1.2 window averaged 3.58 FPS across 68 moving frames, 402 ms
  p95, 71 fresh water frames and zero late frames. This adds a measured
  shadow copy cost alongside variable receiver and bridge spans.
- The initial standalone alias probe used 256-square R32F textures and
  exact readback. Both native and DXVK D3D11 rejected the DXVK D3D9Ex-created
  handle with E_INVALIDARG. The reverse D3D11-created handle opened in
  DXVK D3D9Ex and exposed all 65,536 exact pixels written by D3D11. A
  D3D9 event drain alone did not expose the subsequent D3D9 write to D3D11.
  Explicit external Vulkan queue ownership transfer made both directions exact
  with a CPU fence and exact readback. For a GPU-only handover, a DXVK clear
  without a following draw stayed deferred. A native raster draw followed by
  the release barrier made the same readback exact. Native D3D11 and legacy
  DXVK D3D9 repeated the GPU-only roundtrip for two frames each at 256, 8192
  and 16384 square. The small test checked all 65,536 pixels per frame. Large
  tests checked 1280 pixels in five D3D11 tiles and 256 pre-write D3D9 pixels
  per frame. There were zero mismatches. The standalone program is under
  `build/water-30fps-20261104/dxvk_shadow_alias_probe.cpp`.

### 2026-11-05 — Ordinary LS3D source replay experiment

- The ordinary hardware object branch of `m_RenderFGroup` is at LS3D
  `+39280` in 1.2 and `+37940` in 1.0. The experimental source path accepts
  only the simple current-world-stamp branch with already matching alpha,
  texture stages, cull and format. The first 256 eligible draws still run
  native and compare the current draw operands. Both native unlocks and
  `m_AddFGroups` stay in place.
- Frozen 1.2 A/B/A on host `2188D440` checked 256 calls without mismatch.
  About 68.5% of source draws used the equivalent five device calls.
  All arms reported 257,535 logical triangles, 197,759 BLAS triangles and
  2,699 materials. Median frame times were 118.495, 118.574 and 118.878 ms.
  Skipping the LS3D function body alone gave no measurable speedup.
- The next comparison reuses normalized buffer bytes only after proving
  both revisions, format and ranges against the live device resources.
  It keeps the native texture and world state and flushes the last stream
  and index binding before native work resumes. Both lab switches default
  to off while the packet path is being tested.
- Frozen 1.2 A/B/A on `331DEEA0` accepted 582,734 proven packets with 174,385
  tail flushes during the B sample. Median frame times were 116.673, 115.462,
  and 115.344 ms across 56, 57 and 56 frames. Logical and packed triangle
  counts stayed at 259,089 and 199,427, materials stayed at 2,672 and there
  were zero late frames. The geometry-copy subspan dropped from about
  6.4 ms to 3.6 ms but the frame gain was within run variation. Both
  experimental branches were removed after the comparison. The tested
  source is backed up under `build/water-source-packet-20261105-033723442`.
- An eight-second moving CPU sample on the same host placed 44.9% of samples
  under `SceneDrawCollect`, including 24.2% under `ObserveDraw` and 19.3%
  under the snapshot's draw callback. `m_AddFGroups` accounted for 2.7%,
  while `PackMeshes` and `UploadOwned` accounted for 8.3% and 8.6% separately.
  In the frozen test, packing plus placement took about 10 ms on frames with
  zero new meshes and only 64 bytes of GPU buffer updates.

### 2026-11-05 — Retained packed BLAS and ordered-surface resolve

- Host `813917B8` and water `169D8260` use append-only packed CPU storage
  with fixed-length leases for compact BLAS/attributes. Old publications
  retain their exact prefix and never see an appended tail. If capacity or
  producer continuity fails, a new allocation receives the proven prefix.
  x86 WARP ray scene, blend and waterline suites passed. The snapshot
  fixture checks exact old bytes across append, reallocation and release.
- Frozen 1.2 1440x810 DXVK/DLAA A/B/A in the same PID measured median pack
  plus placement at 9.856, 2.899 and 9.892 ms; median frame time was 117.076,
  109.057 and 114.392 ms. All arms used 254,339 logical and 195,507 packed
  triangles, 2,689 materials, 64 GPU-upload bytes, and zero late frames.
  With DLAA disabled for pixel comparison only, 198,000 decoded JPEG100
  water-crop pixels were equal across all three arms.
- With DLAA on and the scene moving, A/B/A pack medians were about 19.62,
  6.68 and 19.23 ms; mean rates were 7.03, 7.28 and 7.19 FPS, p95 158.43,
  158.03 and 155.95 ms with zero late frames. Scene composition and GPU
  upload bytes changed across arms, and end-to-end improvement in that
  moving window was within its variation.
- The optional ordered-surface resolve reduced measured composition from
  about 10 ms to 0.27 ms and total water GPU time from about 47 ms to 36 ms
  in a separate 1.2 frozen A/B/A with DLAA disabled. All 198,000 water-crop
  pixels matched. Frame medians were 97.813, 97.845 and 97.768 ms. The
  resolve path remained opt-in while checking temporal and native variants.
- In 1.2 moving with DLAA on, the resolve A/B/A published 47, 48 and 48
  temporal frames and the same numbers of DLAA frames, with zero late
  frames. Frame rates were 7.28, 7.20 and 7.28 FPS, while the active GPU
  surface stage sampled about 37–38 ms instead of 45–55 ms.
- Native 1.0 at 800x600 used the same artifact hashes. The moving packed
  arms published 39 and 40 fresh water frames with zero late frames.
  Its fixed comparison gave identical 76,800-pixel water crops for
  independent packing and resolve arms. The resolve remained default-off.

### 2026-11-05 — Exact radial source domain

- The production water path already bounds the total optical segment at
  600 m from the resolved eye. The independent waterline path checks local
  contacts within 600 m and searches 1 m around each contact point. Both
  fit the 608 m horizontal disk inside the existing resident camera box.
  Native AABBs outside that disk are skipped before group preparation.
  Reflected geometry inside the old ray domain, shadow maps, residency,
  samples and render size are unchanged.
- Frozen 1.2 DXVK at 1440x810 with DLAA off for pixel comparison: an opt-in
  lab A/B/A (`0CD9587B`) kept 12,220 of 16,123 draws and skipped 1,432
  visual bounds per frame. Frame medians were 95.315, 84.795 and 95.264 ms,
  with zero late frames. All 198,000 decoded JPEG100 water-crop pixels
  matched. This test excluded the optional resolve.
- Production host `D9831315` and water `EA136071` are deployed to both labs.
  The new feature flag preserves the original box behavior for other
  callers and older hosts. Native C and Rust tests agreed in each lab.
  Their fixture emitted 13,841/12,318 box/disk draws in 1.2 and
  13,469/11,949 in 1.0. C verified invalid-flag refusal and device state
  restoration. A moving 1.2 run at 1440x810 DXVK/DLAA measured 8.49 FPS
  over 72 fresh water frames, 136.53 ms p95 and zero late frames. Median
  game-thread CPU was 109.13 ms, Present took 0.012 ms, and both bridge
  waits were zero. The FPS gate remains open.

### 2026-11-05 — Host-owned CPU scene finalization

- Host `10A4CD28` and water `7053A415` are installed in both labs. The
  owner-scoped worker prepares packed CPU meshes/placements after native
  observers, material copies and their existing build jobs have returned.
  The loader owns the pending callback and drains it before unload even
  for a faulted mod. The water pass joins the same cycle before uploading.
  Native C and Rust job smoke checks passed in both labs; WARP covered
  deferred publication, exact geometry and explicit cancellation.
- Moving 1.2 DXVK/DLAA at 1440x810 A/B/A measured 8.15, 8.69 and 8.45 FPS
  and 141.95, 135.68 and 139.70 ms p95. Median CPU was 116.43, 102.65 and
  109.26 ms. Waiting for B's finished CPU publication took 0.003–0.004 ms
  in the six sampled frames. Each arm published current water with zero
  late frames. The fixed comparison matched 198,000 decoded water pixels.
  Native 1.0 published 61 fresh frames with zero late frames. The worker
  waited about 4–9 ms there because less render work overlaps its stage.
- The optional D3D11 append path passed exact readback for the same
  backing allocation, both view lengths, and three payloads in WARP.
  In the first moving A/B it had zero appends in the eight B snapshots:
  posed meshes exceeded the retained-prefix budget. It defaults off.
  The follow-up separates recent geometry into a volatile tail so an
  old pose cannot force a rebuild of the stable prefix every frame.
- The packed alpha-contact helper had still accessed the default public
  vector. It now reads the retained snapshot's surface span. The targeted
  cutout regression verifies an entirely transparent packed primitive
  produces no waterline boundary. Earlier exact screenshots checked the
  sampled bank crop, while this test exercises that separate alpha path.

### 2026-11-05 — Stable geometry prefix and staged material/placement assembly

- `7427A726` keeps recently created geometry in a replaceable tail. Only
  exact current mesh revisions qualify for retained slots. Age selects
  storage policy; it never proves identical data. A previous publication
  can prove only the stable prefix instead of retaining its old poses.
  WARP checked exact prefix, changed tail, allocation growth, immutable
  views and transparent packed waterline cutouts.
- 1.2 DXVK/DLAA moving A/B/A of `7053A415` / `7427A726` / `7053A415`
  measured 8.81/9.08/8.91 FPS, 137.25/124.01/133.01 ms p95 and upload
  medians 14.352/6.845/14.050 ms. B retained about 145–161k stable BLAS
  triangles. All arms had zero late frames.
- `B931AB94` stages owned draw/material packets so the existing host-owned
  finalizer assembles material IDs and placements without delaying the
  native draw loop. Staging is capped at 8 MiB and flushes in native order
  on the caller if it runs out of space. Original texture verification
  and borrowed D3D reads remain on the game thread.
- Moving 1.2 A/B/A of the staging change measured 8.36/8.93/8.59 FPS,
  CPU medians 106.03/97.57/103.88 ms and native-collection medians
  58.116/47.940/55.158 ms. B staged around 12,341 draws with a maximum
  sampled publication wait of 0.006 ms. p95 was 136.85/124.99/128.19 ms,
  zero late frames. The frozen 198,000-pixel water crop matched exactly.
- Both labs received host `10A4CD28` and water `B931AB94`. Native 1.0
  at 800x600 ran at 7.57 FPS over 42 timing frames, 145.26 ms p95,
  44 fresh water frames and zero late frames. A sampled snapshot had
  198,511 logical and 154,935 stable triangles, 12,102 staged draws,
  5.18 ms of upload and 15.44 ms of wait for CPU finalization.

### 2026-11-05 — Waterline construction overlaps the native bridge

- Water `A65EAF1E` uses the scene finalizer to build the same waterline
  model from the owned current snapshot and a copy of its plane heights.
  The graphics pass verifies the exact heights, cycle and native frame
  before upload. A changed plane set or failed preparation falls back to
  the original synchronous build. Both labs use host `10A4CD28` and this
  water DLL. The WARP scene, blend and waterline suites passed.
- Moving 1.2 DXVK/DLAA A/B/A at 1440x810 measured 10.11/10.45/8.74 FPS,
  122.98/119.21/156.63 ms p95 and 76.73/71.73/84.49 ms median CPU.
  The moving arms varied enough to prevent assigning their full
  difference to this change. B built the model in about 3.9 ms and
  waited at most 0.004 ms at publication. All arms had zero late
  frames, and 198,000 frozen water-crop pixels matched exactly.
- Native 1.0 at 800x600 measured 8.80 FPS, 123.50 ms p95, 50 fresh
  water frames and zero late frames. In the sampled frame, waterline
  construction took 4.18 ms and the wait was 19.57 ms. Its shorter
  native bridge overlaps less CPU preparation.
- A fresh 2,278-sample 1.2 profile put 39.1% of samples in source
  collection, 15.7% in the same-frame receiver reduction/readback,
  and 20.9% in the late shadow pass. Current receiver fitting reads
  exact depth before choosing the native caster camera. The next
  scheduling experiment will keep that depth dependency and overlap
  its readback with owner-opted-in native source collection.

### 2026-11-05 — Same-frame receiver/source overlap experiment

- Candidate host `CFD7E3D0` submitted the original receiver reduction,
  collected the original off-camera native geometry on the game thread,
  and then mapped the same 28-byte result before drawing the shadow map.
  The DXVK fence path alone opted in. Frozen A/B/A kept all 341,713
  receiver samples, exact light-space bounds, the 8192-square map,
  160,414 scene triangles and 1,853 materials. Every one of 198,000 water
  crop pixels matched.
- Moving A/B/A/B reached 10.25/10.30/10.34/10.13 FPS with zero late
  frames. Fixed A/B/A/B medians were 69.58/70.38/69.51/69.18 ms.
  The native replay and GPU schedules did not yield a reproducible
  end-to-end gain. Restored the byte-verified preceding host and water
  sources; the tested candidate is retained under
  `build/water-receiver-overlap-20261105`.

### 2026-11-05 — Native state writes and exact DXR candidate pruning

- A temporary host `B37140F3` skipped only repeat render-state, texture,
  sampler, texture-stage or FVF setters whose identical last value had
  succeeded on the current D3D9 device and valid mirror. It saved about
  1,500 render-state and 1,000 texture setter calls per frame. Moving
  A/B/A/B still measured 10.13/10.05/10.32/10.32 FPS, zero late frames
  and 198,000 identical fixed water-crop pixels. Restored host
  `10A4CD28` and water `A65EAF1E` in the labs.
- A standalone x64 D3D12 test traced 1,166,400 rays through 10,031
  instances and 160,496 logical triangles. It included exact coplanar
  source-order ties, candidate rejection and explicit unknown coverage.
  The same narrow phase on procedural AABBs matched all 32-byte hits
  bit for bit. Hardware enumeration alone took 1.69–1.97 ms versus
  software 1.17–1.78 ms. Committing the accepted exact distance to
  bound farther candidates took 0.64–0.65 ms and retained all matches.
  This is a synthetic traversal result before native materials and
  interop costs, not the river pass's speed.

### 2026-11-05 — Reuse the native observer's profiler record

- The native source observer calls one fixed profiler site thousands
  of times per frame. Host `B085D40A` caches that owner's existing
  statistics record and sampler index until the profiler resets or
  another owner uses the site. Every call is still timed and counted.
  The fault boundary and the native and mod floating-point words are
  unchanged.
- In fixed 1.2 A/B/A/B, both arms collected 168,548 logical triangles
  and averaged 9,662 recorded observer calls. Median main-thread CPU
  was 62.04/58.88/61.56/58.87 ms, while median frame times stayed
  between 68.81 and 69.14 ms. In the moving sample frame rates were
  10.10/10.03/10.20/10.05 FPS, with no consistent change in CPU time.
  All eight runs had zero late frames.
- Native 1.0 at 800x600 ran 36 frames at 9.19 FPS, 115.42 ms p95,
  36 fresh water frames, 166,338 logical triangles and zero late
  frames. Both labs now have the same host `B085D40A` and water
  `A65EAF1E`; both test processes were closed.

### 2026-11-05 — Vulkan shares current full-precision water resources

- The installed RTX 3070 Vulkan driver reports ray query and
  acceleration structures in x64, while the x86 enumeration on the
  same adapter has neither extension. The full water transport with
  conservative procedural candidates and exact-distance pruning
  compiled to Vulkan 1.2 SPIR-V in 2.26 s.
- `vk_water_shared.cpp` imported D3D11's legacy KMT texture memory
  directly on a separate x64 Vulkan device. With the shared GPU
  timeline and external queue ownership transitions, D3D11 and Vulkan
  alternated as writer of the same `RGBA32_FLOAT` storage image.
  Every texel in 1440x810 with three and four slices matched in both
  directions. The `R32_FLOAT` 8192- and 16384-square shadow shapes
  passed a 64x64 far-corner read without a full-map copy.
- The x86 executable also created the same four-slice D3D11 texture
  and passed its KMT identity and a single inherited GPU fence to a
  separate x64 Vulkan process. The child checked every input texel and
  the x86 parent checked every returned texel. No pixel differed in
  either direction.
- The synthetic 1,166,400-ray workload also ran as Vulkan SPIR-V.
  With device-local scene and result buffers, three runs measured
  1.372/1.377/2.025 ms software and 0.421/0.420/0.654 ms hardware,
  with exact complete hits in each run. A preliminary system-memory
  output path took tens of milliseconds and is unsuitable for live
  transport. The production per-frame Vulkan path and original
  30 FPS gate remain open.

### 2026-11-05 — Move unresolved CPU mesh preparation off the game thread

- Water `CC38A32B` copies unresolved normalized vertices and indices
  while the native draw is valid. The scene finalizer hashes and builds
  those copies in native order. A verified prior revision still takes
  the direct path. Material texture reads stay on the game thread.
  The separate geometry staging budget is 8 MiB, with the original
  ordered path for exhausted or uncertain packets. WARP passed exact
  packed geometry, source order and cancellation.
- Moving 1.2 A/B/A/B at 1440x810 DLAA used 0.65–1.12 MB of mesh
  staging in sampled B frames. Median main-thread CPU was
  74.21/71.81/74.58/72.41 ms. Mean FPS was 10.15/10.13/10.03/10.22
  and p95 was 125.17/121.45/121.38/119.71 ms. All frames published
  fresh water and the 198,000 fixed water-crop pixels were identical.
- Native 1.0 A/B/A/B kept fresh water with zero late frames. Median
  CPU was 60.99/58.30/58.31/56.61 ms and FPS was 8.63/8.68/9.14/8.71.
  The B wait increased to roughly 20–26 ms. Both labs received this
  first variant before a trial restoring the existing bounded parallel
  mesh builds inside finalization.
- The four-sample fused shader compiled to Vulkan SPIR-V in 2.35 s.
  The same scratch loop hit the 60-second FXC limit and was terminated;
  the game still uses the verified four dispatches.

- Water `7B7AA5AA` also reuses the two existing parallel mesh build
  workers inside finalization. All three WARP suites passed. Native
  1.0 A/B/A/B at 800x600 delivered 8.886/8.486/8.503/8.673 FPS and
  p95 121.066/128.266/127.435/121.084 ms, with fresh water and zero
  late frames. The peak sampled owned mesh data was 1.715 MB. Both
  labs received this variant with profiler host `B085D40A`.
- The 1.2 empty buffer publication probe `AFB3FF86` verified the
  native early-return instructions before reading the queue count.
  It skipped about 3,800 empty index publications per frame. The
  selected native vertex queue was never empty. Fixed water remained
  pixel-exact and moving A/B/A/B ran 10.548/10.435/10.485/10.245 FPS.
  Source, build output and lab returned to the original publishers.
- Standalone x64 Vulkan also imported the three 256-square RGBA16F
  wave arrays with nine mips and a 512-square typeless cube with the
  sRGB view and ten mips. This proved image and view creation. Native
  sampler parity is still a separate gate.

### 2026-11-05 — Batch native source buffer publication probe

- Host `113CEFD3` grouped up to 16 consecutive ordinary source
  visuals in the same sector, published their prepared native pools
  once, then replayed the original groups in order. The fixed 1.2
  A/B/A matched all 9,791 native draws and copies, world, material
  attribute and billboard fingerprints, device state, and all
  198,000 water crop pixels. There were zero missing packets.
- In moving 1.2 at 1440x810 DLAA, A/B/A/B measured
  10.166/10.382/9.912/10.165 FPS and
  124.757/120.143/126.109/127.591 ms p95. Main-thread CPU medians
  were 69.212/70.813/71.655/71.936 ms. All arms had zero late
  frames. The two B arms grouped about 6.5 visuals per batch.
- The limited CPU gain did not justify another native replay path.
  Source and binaries returned byte-for-byte to host `B085D40A`.
  Both labs are closed with the same host and water `7B7AA5AA`.
  Candidate preimages and build are in `build/water-native-batch-20261105`.

### 2026-11-05 — Hardware queries preserve native layer selection

- The x64 standalone `vk_material_probe` ran the original WARP blend
  fixture and the same material shader with Vulkan ray queries. It
  took geometry, packed source attributes, native mip pages and
  material constants from the original D3D11 publication. Vulkan
  used conservative AABBs and the unchanged intersection and order
  rules in HLSL.
- In 17 fixtures and 68 rays, status, contributing-layer count,
  instance and distance were exact. 91 of the 136 output vectors were
  byte-identical. The largest radiance/transmission float difference
  was 4.47034836e-08. The tests cover compact shared meshes, additive
  and alpha layers, equal-depth order, unknown coverage, and fine
  versus coarse mip alpha tests. The game still uses its existing
  D3D11 path. Scratch code is under `build/water-30fps-20261104`.

- A complete 1440x810 synthetic input exercised the unchanged four
  `CsWaterTransport` dispatches. Software traversal and ray queries
  produced all 3,499,200 output float4s bit-for-bit, with 993,600
  covered pixels and 99,200 first object hits. With a measured-emitter
  fixture, 518,138 pixels received its directional contribution and
  the full comparison remained exact. The scratch transport variant
  that filters PCSS only where the path contributes emitter light
  was also exact. This used the native material fixture and synthetic
  seed, sky, wave, and shadow data. Live integration remains separate.

### 2026-11-05 — Skip impossible emitter shadow work in the native shader

- A native FXC candidate skipped PCSS for upward reflected rays outside
  the measured emitter cap. Downward paths kept the same receiver shadow
  because they can bounce into the cap. The executable branch was verified
  in the DXBC and `CsWaterTransport` compiled in 30.47 seconds.
- In one frozen 1440x810 1.2 session, the ordinary river and sun-facing
  arms matched 169,002 and 214,329 scene triangles respectively, with an
  8192 shadow map and zero late frames. Both 198,000-pixel water crops
  were exact. Median trace time was 21.267 to 21.475 ms at the river
  and 45.432 to 46.326 ms facing the sun. The trial did not yield a gain.
- Restored the source and water `7B7AA5AA` in the build and lab, then
  restored the phase, game clock, camera and DLAA and closed lab 1.2.
  Evidence and candidate `A4B259D9` are in `build/water-emitter-shadow-20261105`.

### 2026-11-05 — Reuse already assigned texture publication slots

- Water `59D0BF6D` checks the current producer revision before returning
  a texture slot already assigned in the same collection. A separate
  publication serial distinguishes retries within one render cycle.
  An exactly identical ordered immutable texture bank can also reuse its
  GPU pages without constructing a remap. No native texture verification
  is skipped for a changed or unavailable producer revision.
- Native texture regression covers repeated slots, a second collection
  with a different order in the same render cycle, and a changed texture
  revision during collection. The original native material suite and
  three WARP scene suites passed.
- 1.2 moving A/B/A/B at 1440x810 DLAA measured
  10.282/10.207/10.735/10.944 FPS, p95
  122.060/120.966/115.411/117.289 ms and median collection
  31.972/29.922/30.383/30.177 ms. The improvement is small compared
  with total frame time. Both frozen water comparisons were pixel-exact,
  with identical source triangle and material counts, and all arms had
  zero late frames.
- 1.0 at 800x600 kept all 480,000 screenshot pixels identical across
  A/B. The moving B sample ran 50 frames at 9.386 FPS, 115.023 ms
  p95, 51 fresh water frames and zero late frames. Its native 4096
  shadow map remained bound. Both labs received `59D0BF6D` and then
  closed, with clock and camera restored. The 1.2 DLAA setting was
  restored. 1.0 has no loaded DLSS mod. Evidence is in
  `build/water-texture-slot-20261105`.

### 2026-11-05 — Separate native source costs by visual class

- `geom.state` now has opt-in `timing_sources` and `timing_reset`
  diagnostics. Counters use integer clock ticks and retain six fixed
  native-class rows. Times are nested. The default remains disabled.
  The second build `14B4CA23` also separates cache pruning, state setup,
  traversal and native restoration. It is currently in lab 1.2 with
  water `789CB90D`; 1.0 still has host `B085D40A`.
- In 47 moving 1.2 collections at 1440x810 DLAA, per-frame ordinary
  `OBJ_` sources issued 8,345 draws. Their preparation took 16.894 ms
  including replay 13.846 ms, observation 10.847 ms and the mod callback
  9.604 ms. Geometry acquisition inside that callback took 3.251 ms.
  8,261 packets reused geometry content and 6,712 had producer-revision
  vertex proofs. The LMAP callback took 0.884 ms for 757 draws.
- Cache pruning cost 0.440 ms, state setup 0.113 ms, full traversal
  29.745 ms and restoration 1.107 ms. Thus the traversal outside selected
  visual preparation remains about 8.3 ms. All device-buffer commits
  together took 1.921 ms with zero measured lock wait and about 3.62 MiB
  written per collection. This contradicts a hypothesis that the bulk
  of the remaining cost comes from source-end GPU uploads or state blocks.
- Frozen ordinary callbacks still took 9.183 ms. The expensive path
  persists without changing actor geometry. Evidence is in
  `build/water-source-timing-20261105`. Static material work and native
  traversal remain the next CPU targets. The earlier fog hook churn
  hypothesis is inactive in this scene because atmosphere/fog already
  holds the pass-through hooks.

### 2026-11-05 — Direct child enumeration did not improve the frame

- A guarded collector-only direct child loop checked the complete native
  enumeration code in both builds and kept the original callback path for
  overrides. In 1.2, A/B/A had identical 9,845 draw packets, geometry,
  attribute and billboard hashes, 168,691 source triangles, 1,931 materials,
  no missing sources and exact 198,000-pixel water crops.
- Moving A/B/A/B gave 11.617/11.566/11.533/11.326 FPS and p95
  105.538/104.057/103.671/106.652 ms, with zero late frames. Source collection
  stayed near 29 to 31 ms. The experiment was removed and all six source
  files restored exactly. Evidence is under
  `build/water-source-children-20261105`.
- Water now exposes its existing texture timing as `scene_cpu.texture_ms`.
  One current sample spent 2.318 ms on texture acquisition inside a 31.646 ms
  collection. Texture acquisition alone does not explain that cost.
- Native hierarchical bounds cannot be used as an unconditional replacement
  broad phase. Their builder intentionally omits sector descendants and
  frame type 11. Any subtree pruning needs a conservative eligibility proof.

### 2026-11-05 — Verified warm native preparation was exact but not faster

- Host `DF29148A` reused native queued entries only for ordinary warm OBJ_
  sources. It checked the full preparation function, native transform and
  sector-light revisions, UV values, creation/alpha dirtiness, opaque/cutout
  nonanimated materials, current group order and texture identities. Original
  native face-group rendering, geometry revisions and texture verification
  remained active. Its cache budget was 2 MiB.
- Frozen 1.2 A/B/A preserved 12,480 native draws, all geometry/attribute/
  billboard hashes, 200,549 source triangles, 2,316 materials, no missing
  sources and exact 198,000-pixel water crops. B reused 79,322 visual
  preparations during the capture arm.
- Moving A/B/A/B measured 10.439/10.359/10.442/10.315 FPS and p95
  112.500/113.721/111.463/109.031 ms, zero late frames. Collection stayed
  near 36 to 39 ms. The candidate was removed. Three source files and the
  build host were restored exactly. Evidence is under
  `build/water-warm-prepare-20261105`.
- Both labs now contain host `14B4CA23` and water `F6B6E7CA`. A serial
  1.0 native check produced 17 current collections and a ready water
  publication. Optional source timing was disabled afterward and camera
  control released. Both labs closed. Water `F6B6E7CA` adds only the
  existing texture timing field to root's accepted default-resolve build.

### 2026-11-05 — Recycle a fully retired packed archive by owned revisions

- Added a CPU finalizer split. In the moving 1.2 river view, staged draw
  assembly took about 11 to 13 ms, packed geometry 8 to 10 ms, placement
  1.4 ms with a refit and 4 to 6 ms with a rebuild. Texture acquisition
  remained about 1.5 to 2.6 ms inside native collection.
- Retaining the entire previous pose tail forced compaction when changing
  meshes exceeded the existing stale-byte allowance. Candidate `E5C2FE0F`
  preserved frozen pixels but increased packing to about 11.8 ms. Removed
  that change. Evidence is in `build/water-packed-append-20261105`.
- Water `23ED634D` keeps one retired private packed allocation, capped at
  32 MiB. A sole archive owner proves that published Snapshot/Scene readers
  have released it. The matching stable owned-revision sequence proves
  which prefix bytes remain exact. The builder reuses those bytes and
  replaces the retired allocation's tail with current geometry and bindings.
  Existing packed-byte and reserved-capacity bounds remain in force.
- WARP regression covers repeated allocation reuse, an explicitly pinned
  old reader, declined reuse while that reader remains, exact old bytes and
  current GPU uploads. The original ray, material, alpha, shared-edge,
  billboard and partial-publication suites passed.
- 1.2 A/B/A/B at 1440x810 DLAA measured
  10.117/10.521/10.282/10.578 FPS and p95
  110.889/109.505/112.262/108.757 ms. Packed-geometry medians were
  9.755/2.697/8.257/2.694 ms and total finalization
  24.364/17.062/21.598/16.457 ms. B retained about 26 MiB and reused
  22 to 24 MiB of proven prefix bytes per frame. Zero late frames.
- 1.0 native 800x600 A/B/A/B measured 7.989/8.121/8.027/8.645 FPS,
  p95 146.901/138.624/142.062/132.163 ms and packed geometry
  7.478/2.256/7.215/2.092 ms. Zero late frames. The frozen water crops
  in both labs were pixel-exact. Both labs received host `14B4CA23`
  and water `23ED634D`. Camera and clocks were released after comparison.
  Results and matching images are in `build/water-archive-recycle-20261105`.
- Grouping eight small owned meshes under each of the existing two workers
  preserved native water pixels in both labs and passed the 34-job ordered
  regression. An in-process 1.2 ABBA/BAAB comparison of 240 moving DLAA
  frames gave -1.157 ms wall time with a 4.731 ms 95% interval and +0.155 ms
  game-thread CPU with a 13.745 ms interval. The gain was unconfirmed and
  the mesh-job grouping and diagnostic toggle were removed.
- Water `C4363344` keeps archive recycling and releases the optional slot
  when available x86 virtual space falls below 160 MiB. It restores the
  original two-worker mesh scheduling. Three source files were restored
  before this memory guard was added, and the WARP ray suite passed again.
  Grouping evidence is in `build/water-mesh-batch-20261105`.
- Serial smoke of `C4363344` succeeded in both labs with current water and
  recycled allocations. Both labs closed after releasing the camera.
- Full-frame visual inspection found a magenta UNKNOWN region on distant
  water in the existing 1.2 baseline. A/B/A each contained the same 647
  thresholded magenta pixels. The reflection/shore unavailability marker
  remains an open baseline defect. Exact optimization comparisons establish
  preservation of this baseline; the underlying UNKNOWN case still needs
  diagnosis.

- Ordinary native source metadata replay was evaluated and removed. The
  opt-in source-only prototype validated normalized complete renderer code
  in both builds, verified current producer vertex/index revisions and
  checked 256 initial native draws with zero mismatches. Frozen A/B/A
  kept all 12,540 draws and exact world, material and billboard hashes.
  However, first moving A/B/A/B gave 10.690/10.175/10.638/10.145 FPS.
  Native binding restoration before every AddFGroups erased the savings.
  A second candidate deferred restoration across verified warm hardware
  preparation. It preserved frozen hashes again but measured only
  10.292/10.210/10.297/10.147 FPS with zero late frames. No gain was
  established. All 13 source preimages, SDK flags and diagnostic toggles
  were restored exactly, preserving the accepted archive recycling.
  Evidence is in `build/water-source-metadata-20261105`. This experiment
  does not close the static-source preparation or 30 FPS gates.

- In-progress hardware checkpoint. The first native GPU-view dump was
  rejected before use because a 1 MiB copy crossed structured element
  boundaries. Each copy now ends on the source stride. Structural checks
  validate source IDs, bounds, roots and captured frame IDs in both labs.
  The 1.0 dataset contains 12,259 placements, 160,704 physical triangles,
  10,583 affine transforms, 404 facing billboards, 59 Y billboards and
  three mirrored instances. A single world-AABB BLAS plus the original
  local narrow phase matches Vulkan software traversal in all 64 float4
  vectors from 32 actual-city rays. Against D3D11 hardware/FXC, strict DXC
  arithmetic reduces radiance error to 1.58e-6. One distance still differs
  by 2.38e-7 m. Native status, layer count and source IDs all match.
- A temporary full capture now holds immutable shared copies of the eight
  real transport images, the full shadow extent included, with a shared
  D3D11 fence. Structured buffers still export through <=1 MiB staging.
  An independent x64 fixture now owns those resources after producer exit,
  with an identical texture-sample hash. Both labs are closed. Temporary
  exporter and normalized-geometry diagnostics were removed, restoring
  their source preimages exactly. Normalized revision reuse suppressed
  about 500 redundant revisions/frame but gave 10.670/10.876/10.914/10.651
  FPS, so it was discarded. The IEEE finite-check candidate passed WARP
  and serial native A/B/A in both builds with exact frozen water crops.
  Moving 1.2 measured 11.381/11.189/11.054/11.169 FPS, without an overall
  gain. Current lab package is host 14B4CA23 and finite water BACEC26B.
  Accepted pre-candidate water C4363344 remains archived.
- Full actual-city water transport now runs independently of the game.
  At 1440x810, all 3,499,200 Vulkan software/hardware float4 vectors match
  bit-for-bit, including 323,286 covered pixels, native material order,
  four transport samples and the full 8192 shadow map. First cold GPU
  timings are 152.995/75.350 ms. These are fixture timings, not game FPS.
  D3D11/FXC versus Vulkan has matching coverage but a maximum output
  difference of 0.0299457 and 583,958 components above 2e-5. Investigate
  this remaining cross-API shading difference before production use.
- Cross-API isolation locates the widespread Vulkan difference in the
  original anisotropic cube sampler at fractional explicit mip levels.
  Whole mips, input LODs and the shadow calculation match exactly. A
  same-source trilinear control on both APIs is exact, but is diagnostic
  only. D3D12 preserves the original anisotropic sampling exactly in the
  dedicated fixed-direction test. Full D3D12 transport runs at 18.239/
  12.576 ms software/hardware, again with exact software/hardware vectors.
  Versus D3D11/FXC, 1,004 components exceed 2e-5, with rare color and hit
  distance differences of 0.04450 and 111.94 m. Resolve compiler edge
  sensitivity before production integration. Labs remain unchanged.
- DXC's normal arithmetic improves the D3D12/FXC comparison to 749
  components above 2e-5. The 111.94 m distance outlier is eliminated,
  with maximum distance error now 0.00349 m. Color error remains at
  0.03997 in a rare pixel. Software/hardware output remains bit-exact,
  with 18.101/12.356 ms transport in this fixture.
- D3D11-created NT-shared textures open on D3D12 in every required
  format, including RGBA32F. This differs from the previously rejected
  D3D12-created direction. Explicit keyed-mutex ownership is mandatory.
  Omitting it blocked the first scratch GPU exchange. The retained game
  textures were opened in a fresh helper before terminating the blocked
  helper, preserving hash 6E0AAB108987A046. Correct ownership plus GPU
  fences passes two sequential 1440x810x4 R32_UINT frames, with all
  4,665,600 words/frame exact, including arbitrary NaN encodings.
  The active fixture is capture_host_recover PID 23516, interactive
  exec session 83243, holding the original native transport capture.
  Both labs remain closed on host 14B4CA23 and water BACEC26B. Next steps
  are full-format GPU sharing and the production same-frame relay, with
  an acknowledged submission before each output wait and native fallback.
- Full-format two-frame sharing now passes exact texels for RGBA32F
  1440x810x4, RGBA16F 256x256x3 with nine mips, typeless BGRA 512x512x6
  with ten mips, and R32F 8192x8192. The shared sky uses an ordinary
  six-layer array, with a cube SRV on D3D12. All eight actual captured
  transport images now reach D3D12 through GPU CopyResource and one
  shared input fence, without CPU image readback/upload. Full transport
  preserves the preceding error counts exactly and measures 17.968/
  12.147 ms software/hardware. Geometry-buffer relay and production
  integration remain open, as does the final FXC/DXIL visual comparison.
- The structured-buffer relay now runs as GPU-only pack/unpack through
  R32_UINT atlases. All 23,406,202 words in 21 actual captured bindings
  were verified bit-for-bit, including encoded native texture words,
  material/geometry/instance records and constants. With all image and
  buffer inputs relayed, full D3D12 transport gives 17.532/11.705 ms,
  retaining exact software/hardware vectors and unchanged FXC comparison
  errors. Fixture initialization still recreates original D3D11 buffers
  from the ignored capture. Production must use the existing live buffers
  and cache verified immutable allocations instead of uploading them again.
- An opt-in early DXVK submission before source collection was rejected.
  Its first moving A/B/A/B gave 10.68/10.89/10.70/10.98 FPS. A subsequent
  eight-arm ABBA/BAAB check reversed the apparent gain, with mean wall
  times 94.265 ms off and 95.183 ms on. All arms had zero late frames.
  The extra submission cost 0.077-0.283 ms/collection in that check.
  All six source preimages, protocol metadata and host output were restored
  exactly. Both closed labs now have host 14B4CA23 and water BACEC26B.
  Evidence and the rejected host 40EE32DB are retained under
  `build/water-early-submit-20261105`. Native CPU replay remains open.
- The full isolated D3D11 -> D3D12 transport -> D3D11 GPU chain now passes.
  All 3,499,200 float4 vectors from each software and hardware arm return
  exactly through a shared RGBA32F array and native buffer unpack. Input
  ready, output submitted and native consumed fence values are ordered
  on the GPU. The native output wait is queued only after the D3D12
  submission and signal succeed. Two consecutive fixture executions pass
  with the same 749 FXC/DXIL comparison outliers as before. This completes
  the isolated relay proof, not production lifecycle or the visual gate.
  Both labs remain closed on host 14B4CA23 and water BACEC26B. The fixture
  keeper remains PID 23516, session 83243. No hardware path is installed.
- Final-color checkpoint. A temporary 1.2 capture includes the actual
  resolve surface and depth. The same original FXC PsWaterResolve shades
  native FXC and DXR/DXIL transport with identical remaining inputs.
  Among 337,754 covered pixels, 34 differ, only two by more than 1/255,
  with maximum 16/255. Full-frame mean maximum-channel error is
  0.0000463/255. Coverage and transport status match. This bounds the
  remaining compiler edge sensitivity but is not a moving native gate.
  The temporary exporter was removed with an exact water.cpp preimage
  restoration. Both closed labs again have 14B4CA23 + BACEC26B.
  Additional fixture PID 21640, session 37216, retains ten immutable
  images with hash 378C756622269AE3 from `native-final-1.2.bin`.
- Production relay implementation checkpoint. On the same RTX 3070 LUID,
  D3D12 reports raytracing tier 0 in x86 and tier 12 in x64. Keep DXR in
  the x64 half. New SDK producer/consumer helpers exchange a fixed 3,680-byte
  metadata request, GPU word atlases, complete images and a shared fence.
  Their serial cross-process x86 -> x64 -> x86 test passes four frames,
  24,576 exact float4 results/frame, immutable reuse and replacement,
  mutable constants/images, stale-generation rejection and device-generation
  restart. A further 32,768 arbitrary integer words/frame match exactly,
  including all exponent patterns and NaN encodings. These helpers remain
  unconnected to the live water mod pending worker/fallback integration.
- The SDK DXR broad phase now builds conservative instance AABBs directly
  from the GPU instance buffer. No CPU geometry/bounds export is needed.
  Its full native 1.2 fixture matches all 3,499,200 software transport vectors
  exactly and retains the preceding final-color comparison. Existing SM5
  traversal is behind the unchanged default branch. Native both-build
  deployment and 30 FPS remain open. Evidence is in
  `build/water-production-relay-20261105`.

- Live relay checkpoint, 2026-11-05. The opt-in water x64 worker now
  receives a 4,064-byte metadata request with separate physical allocation
  ids and content revisions. Native inputs stay on the GPU. The worker
  queues the four unchanged transport samples and acknowledges only the
  matching successful output submission. The normal SM5 path remains the
  default and the same-frame fallback. Lab 1.2 PID 1388 has the candidate,
  while closed lab 1.0 retains host 14B4CA23 and water BACEC26B.
  All fixture keepers were closed after archiving their complete images
  under `build/water-native-scene-20261105`; archive recreation verifies
  the original image hash 378C756622269AE3.
- Initial live DXR integration fell to about 2 FPS despite faster isolated
  transport. Sampling plus the compiled callsite identified a hidden driver
  wait in per-frame IDXGIKeyedMutex::AcquireSync(0,0). Persistent allocation
  ownership and GPU fences remove that call. A single deferred D3D11 flush
  then reduces the producer CPU cost from about 33 ms to 1.2 ms. The exact
  four-frame cross-process test still passes, including arbitrary integer
  bits, allocation reuse, image growth/shrinkage and generation restart.
- A bounded 20 ms wait for the preceding consumed fence avoids repeatedly
  submitting the complete software path while a preceding hardware frame
  is still in flight. The full river camera at [-1336.83,-4.94,198.30],
  direction [-.755,-.175,-.632], FOV 40, 1440x810 and DLAA gives 11.59 FPS
  over 120 moving frames, p95 90.53 ms, with 465 hardware uses / 5 fallbacks
  at the later status checkpoint. A preceding software-only 60-frame arm
  gives 10.88 FPS / p95 96.88 ms. A fresh paired arm is pending, so these
  are preliminary measurements. The unchanged baseline has a distant
  magenta UNKNOWN water band. Moving visual equivalence, lifecycle gates,
  both-build installation and the 30 FPS target remain open. No quality,
  coverage, resolution or sample reductions were made. The main remaining
  CPU source collection is about 31 ms in this camera.
- Paired live timing completes with 100 moving frames per arm in the same
  1.2 river camera. Software gives 11.024 FPS / p95 94.842 ms, hardware
  11.629 FPS / p95 91.450 ms. Serial 1.0 compatibility at 1440x810 under
  native D3D9Ex, without DLAA, also succeeds. Its local hardware/software
  arms give 8.358/7.472 FPS, with a different world state from 1.2. Toggle
  off/on reinitializes the worker cleanly, with 754 hardware uses / 4
  fallbacks and zero reported producer/worker errors at the checkpoint.
  Frozen 1.0 A-B-A water ROI (238,000 pixels) has byte-identical hardware
  A1/A2 decoded JPEG100 pixels. Software differs in 529 pixels, mean
  maximum-channel error 0.00324/255 and maximum 18/255, retaining the rare
  FXC/DXIL edge sensitivity. Hardware remains opt-in.
- A five-timestamp nonblocking D3D12 query separates worker GPU input
  unpack, bounds/AS build, transport and output packing. It reads only
  completed timing values, never native texels or geometry. Both labs now
  have host 14B4CA23 with water x86 35F346A1 / x64 CB3A52A0, hardware off
  by default. Lab 1.2 PID 26492 is active with the diagnostic enabled and
  lab 1.0 is closed. One warm full-water sample is 0.137 / 0.385 / 29.624 /
  0.264 ms respectively. Native seed raster is 23.509 ms and CPU collection
  33.960 ms. The remaining GPU cost is real transport/raster work, with
  source preparation also above the budget. The 30 FPS goal is still open.
- Rejected DXR committed-distance pruning. A padded hardware ray cap after
  the exact native narrow phase preserves all 3,499,200 transport vectors,
  output SHA256 314AC25E64E15D0F3D7967C7EC44EDF51F15E5033B7FEA788B75EEF1B45A11F8,
  and the preceding final-color comparison. However the live full-river
  candidate gives 8.46 FPS over 120 frames and worker trace about 39 ms,
  versus about 11.6 FPS / 30 ms before it. Source is restored exactly to
  unpruned C766A970. The extra hardware traversal state is a possible
  occupancy cost; this causal explanation has not been profiled. No
  committed-distance pruning is retained.
- The preceding live pruning slowdown is not a clean causal estimate.
  A background ProjectZomboid64 process was subsequently observed through
  the GPU process inventory. It was left alone. Further timing uses the
  two candidates in one isolated GPU submission. The production pruning
  source remains reverted. Both closed labs have 35F346A1 / 8872E62D and
  hardware remains off by default.
- Isolated finer broad-phase experiments retain native triangle arithmetic,
  material order and billboard queries. Conservative GPU-built world boxes
  for 196,535 triangles and then 64,697 native BVH leaves both reproduce
  all 3,499,200 output vectors, hash 314AC25E. The same-submission object/
  triangle comparison gives 16.282/17.243 ms. Eight repeated transports per
  arm give object/leaf 20.521/20.182 ms, before the extra AS build cost.
  These candidates are confined to ignored build fixtures and do not justify
  production integration. Ray-box traversal alone is not the remaining
  GPU explanation. Next isolate repeated per-pixel transport setup/shadow
  work and wave-path evaluation, retaining every sample and bounce.
- A single-dispatch four-sample transport plus shared PCSS remains exact,
  but costs 22.608 ms versus 17.143 ms for four dispatches in the same
  command list. It remains an ignored prototype. A separate lazy-PCSS
  candidate keeps four dispatches and reuses the exact float through the
  existing intermediate output fields. All 3,499,200 vectors match after
  preserving the original integer-normalized diagnostic counters. Warmed
  repeated ABAB gives 21.801/20.966 ms. No production integration yet.
- Conservative empty-height-region skipping was prototyped with the actual
  captured FFT extrema (per-cascade absolute maxima .486816/.188843/.011444 m).
  It keeps the old .3 m steps, 96-step limit and seven refinements while
  avoiding height evaluation where the old stride is provably clamped.
  Transport timing gives no material gain, and 1,149 vectors differ after
  an expression-preserving refinement. Final resolve differs in 88 pixels,
  versus the retained 34-pixel FXC/DXIL difference. It is not integrated.
  The seed camera-repair pass might benefit more, but its arithmetic gate
  must be resolved before production. All scratch keepers are now closed.

### Optimization audit and review boundary, 2026-11-05

The owner rejected the progress after sixteen hours below 15 FPS. The root agent
directed a temporary stop to experiments/integration for an architectural review.
The optimization objective remains active. At the initial review checkpoint, lab 1.2
PID 25176 remains active on host 14B4CA23, water 35F346A1 / 8872E62D, hardware
off, diagnostic river camera held and devtools freecam temporarily false.
Game/wave time runs normally and source timing was disabled. Lab 1.0 is closed
with the same artifacts. All scratch GPU keepers are closed and all required
native inputs are archived, including complete images and hash 378C756622269AE3.

The controlled paired 1.2 1440x810 DLAA result is only 11.024 to 11.629 FPS,
90.712 to 85.988 ms/frame, with p95 94.842 to 91.450 ms. Hardware object
traversal therefore yields about 5.5 percent full-frame gain. It does not
approach the 33.3 ms objective. A later source-profile run in the same camera
measured 126.813 ms/frame across 35 frames. Its RENDER_WORLD site is 49.777 ms
including its 17.419 ms observer site. The bridge is 62.374 ms including
synchronous receiver reduction/readback and late shadow work. These nested
numbers must not be added as exclusive phases. ReceiverBounds has 28.7 percent
of sampled stacks in a system wait, approximately 36 ms at that frame rate.
The shadow-map site reports 22.532 ms and can be nested in the bridge.

A separate 207-collection QPC source split gives these exclusive differences
in milliseconds per collection. Walk/bounds outside preparation 13.198,
preparation outside native draws 12.275, native render/bindings outside the
observer 7.879, observer dispatch outside callback 2.922, mod work outside
geometry proof/copy 14.658, geometry proof/copy 7.680. Their 58.613 ms sum is
the instrumented collection walk. Setup/restoration/pruning add .156/1.652/.683.
This was a distinct interval, so it cannot be substituted into the preceding
35-frame profile. Skipping only native RenderFGroup does not remove the
larger callback, preparation and walk costs.

The GPU water timestamps are the decisive lower bound. A warm full-water
sample has seed 23.509 ms, transport interval 35.297 ms and compose .393 ms.
Inside that interval the x64 worker reports input unpack .137 ms, bounds/AS
.385 ms, actual transport 29.624 ms and output packing .264 ms. Seed plus
actual transport alone exceed 53 ms. Moving source CPU work into the current
ReceiverBounds wait could overlap a substantial CPU phase with GPU work,
but that scheduling change alone cannot yield 30 FPS.

WaterTraceWaves performs up to 96 adaptive steps and seven refinements.
Each WaterTraceHeight calls WaterTraceBase with three inverse-displacement
iterations, then evaluates displacement again. Three cascades mean twelve
FFT texture samples and up to four WorldShoreWeight queries per height
evaluation. Shore queries include exact candidate segment/triangle distance
and unknown-region checks. Normals and neighboring-ray footprints repeat
related field evaluation. The primary seed pass also repairs raster geometry
with that continuous solver. Current raster geometry and optical intersection
are separate surface representations. Geometry-only hardware acceleration
leaves this work in place. Triangle/leaf AABB prototypes preserved outputs
without a material gain. PCSS reuse is bit-exact but warmed ABAB saves only
about .835 ms. Single-dispatch four-ray batching is slower. The attempted
global-height skip gave no transport gain and changed some pixels. None of
these prototypes is integrated. A precise exclusive GPU attribution inside
wave marching versus shore distance versus material BVH is still missing.

The next architecture to evaluate is one authoritative parametric displaced
water surface for primary visibility, reflected-wave intersections, normals
and temporal positions. A GPU-built conservative patch/min-max hierarchy
should reject empty ray intervals, followed by a local solve on the same
surface. Bounds must include all three cascades, horizontal chop, shoreline
damping and potential folds. Candidate order must preserve the nearest
surface. Returned surface coordinates can evaluate the previous displacement
directly for motion. Existing material, sky, shadow and four optical samples
can remain consumers of that common surface. This addresses the measured
seed/march duplication rather than merely changing object traversal.

No 33 ms end-to-end result is established for this design. A bounded deciding
proof should use the retained actual scene and wave inputs, build the complete
hierarchy on GPU, replace primary/secondary water intersection together, and
measure hierarchy rebuild, all four samples, relay and composition. Preserve
the accepted wave silhouette, shore contacts, reflection visibility, sun glare
and motion in the reference views. Internal marcher steps need not match the
old algorithm. The owner's gate is absence of visual degradation. If the
complete water path cannot fit the residual frame budget after measured
world/shadow/DLAA costs, reject the design before further integration. A
water-only gain that still leaves the full frame above 33.3 ms is insufficient.
- Safe pause cleanup is complete. After checking exact lab PID 25176,
  released its diagnostic camera and restored devtools freecam=true, the
  value read before testing. Lab 1.2 closed normally. Both labs are now
  closed on the identical software-comparison/opt-in-DXR candidate above.
  This is a retained development checkpoint, not accepted DXR visual quality.

### 2026-11-06 — Frozen full-source removal ceiling

- A temporary water-only diagnostic retained the completed scene snapshot, GPU publication and waterline,
  removing collection, finalization and upload together. It was tested in one 1.2 process at 1440x810 DLAA,
  river eye [-1336.83,-4.94,198.30], direction [-.755,-.175,-.632], FOV 40, game time stopped and the optical
  phase held at 12.5 seconds. Hardware transport remained off. The source held frame 1309 throughout B,
  with zero upload time and the same 202,198 logical triangles, 159,120 BLAS triangles and 2,288 materials.
- A/B/A measured 11.416/10.980/11.009 FPS and median 85.616/88.568/89.747 ms across 76/73/73 frames.
  Native GPU frame means were 87.705/90.693/90.584 ms. All arms published fresh water without late bridge
  frames. Water GPU samples were 66.520/71.595/68.434 ms. The hold removes all source CPU work, but does
  not improve this already GPU-bound frame. This is a diagnostic ceiling, not a usable optimization.
- Prioritize the water GPU path before further static-source work. CPU collection can become limiting after
  GPU reduction, so retain its existing profiling and ownership requirements. Do not infer a speedup by adding
  CPU collection time to the wall clock or subtracting it from the frame.
- Temporary source restored exactly to 9981532E and x86 output/lab to 35F346A1. Host remains 14B4CA23,
  x64 water 8872E62D. The diagnostic override, clock and time were released, freecam restored to its read
  original true value, and exact PID 24456 closed. Both labs are closed. Probe and preimages are retained
  under build/water-performance-reset-20261106. No diagnostic source-hold feature is shipped.

### 2026-11-06 — Actual transport cost attribution

- One isolated shader-ablation batch uses the archived native frame at 1440x810,
  337,754 covered pixels and all four optical samples. Full controls bracket each
  candidate in the same command list. Outputs are retained separately. Full output
  hash remains 314AC25E. Production sources/labs are unchanged and the keeper closed.
- Marginal transport timings, candidate versus adjacent full mean, are wave bypass
  9.767/14.971 ms, shore bypass 12.417/14.031, full world material/radiance query
  bypass 3.754/12.891, PCSS bypass 13.134/12.692, wave+shore 8.912/12.950,
  all combined .6825/13.5659. The largest measured category is the complete scene
  material/radiance query. It includes traversal, ordered alpha, texture decode,
  filtering and footprint work. Its exclusive sub-costs are not established.
  These substitutions change branches and outputs. The marginal changes overlap
  and are not additive or accepted quality results. Do not infer that wave marching
  alone explains the transport cost.
- Cold fixture unpack/first AS is 40.572/3.037 ms. Diagnostic pack plus 56MB CPU
  retention copy is about 8.9 ms/arm. These are not the shipping steady-frame costs.
  Retain the measured live worker unpack/AS/pack .137/.385/.264 ms and the larger
  native transport interval when budgeting integration. The matched source-hold
  GPU frame 90.693 less water 71.595 leaves about 19.10 ms of observed residual.
  Full GPU below 30 ms currently leaves only about 10.90 ms for complete water.
  The fixture has different coverage, so its absolute transport milliseconds cannot
  be directly subtracted from that native frame to predict FPS.
- Evidence and exact bypass meanings are retained in
  build/water-gpu-attribution-20261106/result.md, timings.csv and seven output buffers.
  Next choose transport material evaluation together with root's primary seed
  attribution. Repeated raw/BC texture decoding and manual filtering are a concrete
  candidate, still requiring an isolated measurement before a performance claim.

### 2026-11-06 — Tester shutdown crash, reported logs and crashlog.zip

- Inspected the latest Downloads mml log, its previous run, the DXVK log and the archive. The current report
  and screenshot agree on C0000005 at LS3DF+5AC2E writing 92CA3CF4. The archived run is from November 3,
  around 30 minutes after startup. It returns from freeride to 00menu before system shutdown.
- MCP crash_triage identifies C_IBuffer destruction through FreeAllIBuffers, drvClose and CloseSystem.
  Native disassembly and captured memory prove one stale slice owner reference into a reserved region.
  Buffer 71951A68 points through its one-element slice array to block 96F4D6E0, then owner slot 92CA3CF4.
  The attempted write clears that slot. Address space still has 2220 MiB free with a 1402 MiB largest region.
  This repeats the known October exit failure. Faulting code alone does not identify the earlier lifetime error.
- The custom river_water mod is absent. The process includes core, game_fixes, dlss_nr, fps_unlock and
  fusion_fix, plus the tester's script ASIs, including a separate MafiaWaterShaderMod.asi. No offending mod
  is assigned from this dump. The previous log ends normally, so the exit failure is not universal.
- The x86 dump sign-extends high memory-map bases. A separate parser finds the owner slot MEM_RESERVE;
  MCP's unsigned-address lookup misses it. Added that tool limitation to Known gaps without changing tools.
- Extracted only the two named archive entries under ignored re/crashes/renato-20261103-205542, preserving
  their original data. Dump SHA256 is 47475661f0226936a7150d3598cefd33bb609c7df61948f4ca4d488ea519b4e8.
  No production code or game configuration changed for this inspection. Water work was held for this report.

### 2026-11-06 — Targeted 1.2 shutdown index-owner recovery

- Added host-owned recovery at the verified C_IBuffer owner stores. Only the final IGraph::Close ->
  FreeAllIBuffers path is changed, selected by the native return at ESP+44. All 12 owner bytes must be committed,
  writable, non-guarded, and still match this buffer, its D3D buffer and the slice index before clearing.
  Otherwise leave the owner untouched and log it. Native metadata deletion and D3D resource cleanup continue;
  ordinary buffer destruction is unchanged. This prevents the observed exit write, not the earlier lifetime error.
- SDK table stores both RVAs; 1.0's addresses are zero because its disassembled destructor has no equivalent
  owner-clear loop. Regenerated Rust tables. Both-build check_builds and gen_rust_game --check passed.
  Built MafiaModLoader and index_owner_test only. Standalone tests pass for identity mismatch, valid ownership,
  null/overflow, reserved/decommitted memory, read-only/guarded and writable cross-page owners. Added to offline
  suite entry; the full graphics/mod suite was not rerun for this narrowly scoped shutdown change.
- Unmodified configured tester copy W:/MafiaDLSS, PID 8496, loaded city, returned to 00menu and quit normally
  without a new dump. It retains its original host, mods, textures and ASIs. Startup's 45-second timeout was a
  long asset load, not a crash: where showed engine texture loading and start.state later completed.
- Backed up lab 1.2's host/PDB under build/index-owner-exit-20261106/lab-host-before and installed candidate
  08FFD90E963407C8A15E43CA4984561AB4DF034664272A7A6DDAA5A30E0ED2E5. Two controlled native tests (7736,
  14124) allocated an engine slice of 65,538 indices, decommitted its separately allocated owner page, and
  quit through menu commands. Each logged that exact stale owner without a new dump. An open process handle
  for PID 14124 confirmed normal exit code 0. Both labs and the tester copy are closed. 1.0 is unchanged.
- Sources and preimages are retained in the same ignored build folder. No water source, shader, configuration
  or quality changed. Root-cause attribution still needs the original owner's allocation/free history.

### 2026-11-06 — Separate October gameplay crash: negative navigation node

- Inspected the explicitly supplied W:/Downloads/crashlogs.zip (one dump/report, current and previous mml
  logs, DXVK log). Extracted only its five named entries to ignored re/crashes/renato-20261029-150915 after
  checking that the destination did not exist. Dump SHA256:
  98dcdb4bd9ecc4a3a611b8c1c5b5b98692cf3f50924d2f6582da56c7aada2981.
- crash_triage first: October 29 at 15:09:15, about 61 minutes after startup, in mise18-mesto on the main
  thread. Game+1007B2 faults reading 569CEFEE through ai_path::GetSpecNodeC -> UpdateSpec -> Update ->
  C_entity AI. This is not the exit failure. The archive's previous log does contain LS3DF+5AC2E at shutdown.
- Verified the actual instruction sequence in the dump and original disassembly. Path 568D1D70's normal
  vector holds one 60-byte waypoint at 3C9109F0, with node index -1 at +34. The unchecked stride calculation
  produces EDX=-15; EAX=569CF00C; the floating read is base-30 into committed PAGE_NOACCESS memory.
  Free address space is 2309 MiB with largest 2044 MiB. No GPU-loss evidence appears in the supplied logs.
- Read the 1.0 twin 40A0C0 too: same lookup with its own waypoint-container offset. Recent events associate
  C_entity 3C546208 with vehicle exit and falling, but do not establish which component created the bad route.
  No river_water mod is loaded. Presence of DLSS, Fusion Fix, fps_unlock and external ASIs does not assign blame.
  The old loader offset in the ancestor stack is its call into MissionTick; current PDB symbol labels for an old
  binary must not be treated as proof of a file-stream failure. The existing shutdown guard does not cover AI.
- Recorded the separate navigation gap. No production code, binaries, game settings or running processes changed.

### 2026-11-07 — Single-waypoint navigation detour fixed

- Verified that native UpdateSpec already handles node index -1 with GetSpecNodeB in its general arm.
  The one-waypoint arm always used GetSpecNodeC, which reads the web checkpoint. Host fixes now redirect
  a single waypoint with index -1 to that general arm. The game's own plane, collision checks, detour
  allocation and status handling run there. A real node continues through GetSpecNodeC.
- Verified the count branch, hook bytes and destination in both executables with check_builds.py. Regenerated
  and checked the Rust tables. The targeted MafiaModLoader build passed. Original source and lab host/PDB
  preimages are under build/path-node-fix-20261106. Both labs carry host SHA256
  76911DC5936C73BBCD18B2775C69705FACF6DD404C0570AD29F12512FF61FA83.
- Ran labs serially (1.2 PID 20544, 1.0 PID 13372). Called native UpdateSpec with a test-owned 0x9c path,
  one 0x3c waypoint, cursor zero and a route crossing the parked starting car. For index -1, the caller-filtered
  taps recorded B once and C zero times. Each returned 2 and generated one detour point around the car.
  Control index 2972 recorded B zero times and C once, returned 2 and produced its detour. Native stack checks
  passed in all four calls. Used native ai_path_free_buffers for the detour data, released the fixture, removed
  the test actor and closed each lab. The original report's route state was reproduced in a controlled scene.

### 2026-11-07 — Water sampler, primary repair and native DXR facets

- All 5,819 mips of the retained 1440x810 native city fixture are BGRA8. Addressing alone took
  16.918 ms versus 19.226 ms in the adjacent full controls. Simplified the normalized tap-address
  calculation for wrap, mirror and clamp without changing the texel decoder or filtering order.
  All 3,499,200 transport vectors matched. The x86 native material harness passed 1,512 texture
  samples, packed and BC formats, mip and alpha handling, and native fixed-stage comparisons.
- Native 1.2 primary seed control, repair bypass, repair shore-lookup bypass and repeated control
  took 25.465 / 1.407 / 11.785 / 26.057 ms. The bypasses change optical outputs and only establish
  marginal costs. The exact-stride prototype used an empty shore-cell certificate and actual GPU
  FFT extrema to avoid evaluations only where both sign and the clamped stride were proven.
  Control / empty-cell cache / cache plus bounds / repeat took 26.435 / 23.437 / 21.807 / 25.799 ms.
  DLAA history varied even across the two controls. The prototype was removed from the lab.
- On the retained city fixture, 196,072 expanded world-space triangle candidates and 463
  procedural billboard candidates replace 12,067 procedural object boxes. The local hit,
  orientation, alpha, material, ordering, footprints and all four samples remain authoritative.
  The complete DXR transport took 13.406 ms versus 18.856 ms across adjacent full controls,
  and 12.665 ms with the address optimization. Cold AS preparation was 1.811 versus .899 ms.
  Both alternatives matched all transport vectors and the complete 1440x810 RGBA bit for bit.
  The evidence is under build/water-native-triangles-20261107. The one fixture does not establish
  conservative candidate coverage across all transforms and grazing rays.
- The live opt-in facet path sends only a compact range table from the existing scene snapshot
  worker. Geometry, triangle expansion and AS builds stay on the GPU. The table and materials
  carry the current cycle and frame. `river_water.hardware_mode {"on":true,"facets":true}` selects
  it. `facets:false` selects object-box DXR when enabled. Default remains the software reference.
- Native 1.2 1440x810 DLAA, fixed river view: object/facet/object/facet/software median FPS
  10.67/11.64/10.64/11.27/9.62. Worker object trace 40.39/40.59 ms versus facet trace
  32.43/32.71 ms. AS build .35/.36 versus 1.59/1.57 ms. Seed remained about 25 ms.
  River, grazing and downward camera views completed on the facet path. Grazing exposed the same
  existing thin magenta unknown-coverage band on the software reference. DLAA live images are a visual
  check, not bitwise motion parity. A separate user Game.exe was present, so the figures are local paired
  measurements, not uncontended machine ceilings.
- Native 1.0 at the same 1440x810 tested serially: object/facet/software 7.15/7.48/6.90 FPS.
  The 9Ex software control reported 75.61 ms waiting for D3D11 and 74.90 ms of D3D11 pass time.
  With a temporary pinned DXVK 3.1.1 lab layout, the same facet renderer measured 14.60 FPS
  median, 68.48 ms over 55 frames, with zero CPU handoff wait. Worker tracing remained about 31 ms.
  This was a cross-API diagnostic, not a pixel-equivalence claim. Restored the exact original
  dxwrapper.ini and removed the temporary d3d9.dll. Both labs retain the facet build behind opt-in.
- The owner directed a review of Remix's optimization techniques. Its acceleration manager reuses
  static and dynamic acceleration groups with explicit invalidation and keeps hardware traversal and
  material handling separate. Updated WATER_REFLECTION_PLAN.md with the corresponding work for the
  existing renderer, including source draw ownership, ray execution, wave intersection and queue order.
- Added acceleration reuse keyed by producer identity, generation and exact immutable geometry, instance
  and range revisions. Stationary 1.2 GPU AS work fell from about 1.6 ms to the timestamp's zero display
  precision. Camera movement changed the captured native extent from 198,390 to 199,339 triangles and
  triggered a rebuild, then stable frames resumed reuse. At fixed camera, one no-op prop placement
  triggered no rebuild and five distinct translations each triggered one. The current-frame source and
  output acknowledgement still run each frame. Removed the test prop.
- The panned 1.2 view showed only a narrow river band above the foreground road. Worker tracing fell
  from about 31.5 ms to 3.9 ms. Primary seed still took 22–27 ms because the ordered raster path checks
  opaque depth after wave repair and quad derivatives. Visibility belongs ahead of the expensive solve
  wherever the bounds prove that doing so preserves the result and its adjacent derivatives.
- A GPU shore tile certificate and whole-search no-crossing bound saved about 3.3 ms of seed time alone,
  or 4.6 ms with the previous empty-cell cache, in one bracketed diagnostic. The no-crossing test retained
  the original 96-step reach and conservatively excluded unknown shore samples. Complete frame controls
  varied from 116 to 125 ms. Restored the installed pair and kept the prototype under
  build/water-material-fast-20261107. A tighter spatial surface representation is still needed.
- Splitting upward and downward optical rays into full-size dispatches was slower in the exact same
  captured input. Transport took 14.47–14.51 ms versus 12.87–13.84 ms. There were 797 differing float4
  vectors and an identical final RGBA image. This does not test compact work queues. Kept only as a fixture.
- A D3D12 texture bank with the original 852 BGRA8 textures and 5,819 mips moved each anisotropic tap's
  trilinear work to hardware. Existing tap coordinates, LOD, count and alpha/blend sampling stayed in the
  original policy. The tapped variant took 10.963 ms versus its adjacent control mean 12.863 ms. It changed
  123 of 337,754 water pixels by at most 1/255 and changed no coverage/status. Replacing the whole footprint
  by native SampleGrad saved less, changed 9,053 water pixels and reached 25/255. The tap candidate is
  retained in build/water-native-tmu-20261107. Native GPU publication and moving-frame parity are open.
- Final installed water hashes on both labs are 599B7469 (x86) and 85530BB4 (x64). The targeted release
  builds passed. Native 1.0 verified 14 stable reuses after its first build, then one rebuild after the camera
  changed the extent from 201,604 to 202,553 triangles, followed by 19 reuses. Worker rejected zero requests.
  The 1.2 run verified explicit object movement as well as a camera change. Camera and wave holds were
  released, the original 1.2 freecam setting restored, and both labs closed serially. The two crash fixes
  remain in the byte-verified host. Facet transport still requires explicit opt-in. The 30 FPS gate is open.

### 2026-11-07 — Spatial wave bounds and live frame budget

- Fresh uncontended 1.2 facet checkpoint at 1440x810 DLAA, full river, fixed camera and phase:
  41 frames, 82.43 ms median and 87.52 ms p95, about 12.13 FPS. Sampled GPU seed 21.89 ms,
  worker trace 32.74 ms and complete water surface 57.05 ms. The separate source instrument
  over 27 collections measured 35.27 ms/frame in the tree walk with native draws. OBJ_ prepare
  accounted for 21.92 ms nested, including 12.38 ms in owned snapshot callbacks. The stage timers
  overlap other work. In a subsequent diagnostic frame, late shadow receivers and raster took 4.66
  and 9.06 ms respectively. The DXVK handoff copies took about .05 and .03 ms on the GPU.
- The isolated spatial repair prototype builds min/max over all current displacement texels, with
  original wrap, bilinear tap coverage, inverse horizontal displacement and shore attenuation bounded.
  It skips only complete ray intervals with no possible crossing or unknown shore classification.
  The 1.2 seed took 17.84–18.42 ms versus 22.81–24.59 ms controls. Sixteen-step skips retain the
  original float additions and only help when the exact clamped stride is proved. They saved under
  1 ms over spatial bounds in a subsequent bracketed run. Complete-frame timing varied with transport.
- Added a one-shot same-native-frame comparison in the ignored diagnostic. It reads only eight GPU
  counts and compares exact bits in the three seed arrays, resolve depth and surface. For the spatial
  and chunk arm, 618,788 pixels matched across all fields. The integral shore mask also matched all
  619,318 full-river and 62,396 panned pixels. Its seed savings were under 1 ms over coarse shore tiles.
  The native coverage is nearly twice the 337,754 pixels in the old fixture, which limits comparisons
  between absolute fixture and live transport timings. Evidence is under build/water-material-fast-20261107.
- A native nearest-triangle query supplied a conservative distance hint to the reference object AABB
  traversal only after exact local intersection and cursor validation. The entire archived output matched.
  Hybrid took 18.512 ms versus 18.868 ms across adjacent controls. Native facet enumeration remained
  faster at 14.660 ms in the same submission. Kept only the fixture under build/water-hybrid-trace-20261107.
- Rejected water seeds before repair only if the nominal surface and all possible repair brackets
  are behind opaque depth, and only if all four lanes of that primitive's derivative quad qualify.
  Same-frame exact comparison matched every covered pixel, 618,947 full and 62,525 behind the road.
  The panned seed fell from 28.40 ms to 6.29–6.62 ms. The test retains resolution and optical samples.
- The production ordered seed now uses these bounds, with 37 KiB of GPU storage and an unsaved
  reference toggle. The bounded release build passed after renaming an FXC-reserved parameter.
  With these shaders and native facet transport, full-river 1.2 medians were 76.95 ms before and
  75.03/76.60 ms after, with sampled seed times 26.37 and 17.00/15.93 ms. Holding the existing
  full-river source still gave 75.67 ms median. The GPU remains the limit in the full view.
- Behind the road, holding the already published source for a fixed camera, world and wave phase
  gave 27.57 ms median (36.28 FPS) and 32.45 ms p95. Fresh collection before/after cost 59.42/57.76 ms
  median and 65.36/63.08 ms p95. Native collection was 34–36 ms. The hold only measures the CPU
  ceiling and cannot serve as moving-scene publication. The diagnostic binary has been removed.
- The native source owner investigation confirmed block activation/deactivation in both builds.
  The block root and auxiliary roots are enabled/disabled together, and pooled object records also
  link/unlink through a distinct path. Pool slot reuse may preserve the frame address while changing
  block membership and pose. A retained record must check that membership plus its draw dependencies.
- An isolated compact transport builds two GPU queues per sample by direction and dispatches their
  active ranges indirectly. It preserves all four optical samples and each pixel's sample order.
  Transport including queue construction took 11.19–11.83 ms against 12.83–13.82 ms controls.
  Of 3,499,200 float4 vectors, 797 differed by at most 1.35e-6, the same split-shader differences
  seen previously, and the final RGBA bytes matched. Evidence is in build/water-transport-queue-20261107.
  This is an archived-frame result. Native frame ordering and moving-image acceptance are open.
- Serial native release verification installed x86 1FBDB529 and x64 85530BB4 in both labs. In the
  1.2 full-river facet run, median fell from 81.17 to 74.31 ms and p95 from 87.14 to 84.43 ms
  (12.32 to 13.46 FPS). In native 1.0 9Ex it fell from 137.32 to 123.71 ms and p95 from 141.82
  to 130.39 ms (7.28 to 8.08 FPS). Panned software-path seeds were 5.41 and 6.90 ms respectively.
  The controls and holds were released, the original freecam settings restored, and both labs closed.
  The release gate for motion parity and sustained 30 FPS in the full view remains open.

### 2026-11-07 — Compact native ray dispatch and source ownership

- The compact dispatch now uses one identical trace body for both direction queues. The archived
  production control and compact arm match all 3,499,200 float4 vectors and final RGBA bytes.
  Transport was 10.38–10.60 ms versus 12.81–14.10 ms controls. Queue classification decides scheduling
  only. It cannot select a different optical branch or discard any active sample.
- Alternating native facet dispatch in one 1.2 run reduced worker trace median from 32.58 to 24.02 ms.
  Frame medians were 74.25 and 67.15 ms, p95 83.53 and 78.60 ms (111 and 108 frames).
  The paired 1.0 9Ex run reduced worker median from 30.85 to 24.65 ms and frame median from 125.15
  to 118.60 ms, p95 133.16 and 126.91 ms (125 and 121 frames). Worker rejected zero requests.
  Production uses compact queues when the opt-in facet mode is active and scratch allocation fits.
  The existing full dispatch handles other cases with the same image, including larger extents.
  Queues use 8.9 MiB of GPU storage at 1440x810 and no x86 staging or new cross-process traffic.
- Targeted x86/x64 release builds passed and both labs have 1336AF98 and 9FD421B7. Release 1.2
  recorded 69.62 ms median and 73.32 ms p95 in a separate run with the owner's game present.
  Its first camera-change sample used the existing current-frame software fallback. Later samples and
  all five 1.0 movement samples used hardware and published new sources with zero worker rejections.
  Separate 1.0 wall measurements were affected by variable contention, so the same-process pair is
  the comparison. Both temporary binaries were removed and original lab controls restored.
- Current source inventory in 1.2 maps 10,629 of 12,041 draw records to active native block roots,
  auxiliary roots or occupied pooled slots. Of 9,251 ordinary draws meeting the warm pose/lighting
  criteria, 8,839 matched owner, pose, flags, mesh and material pointer on a second fixed capture.
  After panning, 8,586 still matched, 416 changed and 134 were new. This measures eligibility for
  further dependency proof. It is not sufficient to reuse geometry or texture contents. The probe
  is under build/water-source-membership-20261107 and is no longer installed.
- The D3D12 texture-bank prototype now moves raw mip texels entirely on the GPU from the existing
  word buffer, using one GPU alignment pass and copy into 852 original BGRA8 textures (5,819 mips).
  Cold GPU build was 5.06 ms, using 75.4 MiB of temporary GPU staging and 181.8 KiB of mip-job
  metadata. The native tap arm matched the preceding CPU-built texture bank bitwise. It was 11.14 ms
  versus adjacent raw controls averaging 13.24 ms. Archive evidence is in
  build/water-native-tmu-gpu-20261107. Retain the raw opacity route and add a revision-gated live
  metadata publication before considering use in the labs.
- A scratch 1.2 host bypassed native device binding for a simple OBJ_ draw only after proving current
  draw operands, native render state and both buffer range revisions. The first 1,024 eligible draws
  still executed natively and matched the predicted texture, transform and index arguments. The B arm
  replayed 397,442 owned packets. Panned A/B/A scene collect was 36.72/33.28/36.17 ms, while frame
  medians were 58.33/57.78/57.22 ms with the same 198,421 triangles, 2,283 materials and zero missing
  regions. This is insufficient to justify adoption. Skipping binds leaves native preparation and
  the per-draw callback in place. Future retained publication needs both out of the unchanged path.
  The probe is preserved in build/water-source-direct-20261107. Both labs retain the original host.
- The live x64 bank probe reads only the two bounded mip metadata buffers after the preceding
  worker fence. It uses all three producer resource identities and exact revisions as one key.
  The texel buffer remains on the GPU. Unready or unsupported tables fall back to the raw sampler.
  On an uncontended fixed 1.2 view, compact raw and native tap trace medians were 17.89 and 14.41 ms.
  Frame medians were 61.53 and 61.31 ms, p95 73.94 and 68.61 ms (88 and 87 frames). Worker rejected
  zero requests; startup had 14 software fallbacks and then stayed stable. The lab's x64 release
  binary was restored. The probe remains separate pending moving visual and cold-build validation.
- A private host/consumer batch avoided individual guarded callbacks for consecutive proven OBJ_
  packets and also tried bypassing native preparation for verified warm visuals. The batches averaged
  about nine draws because interleaved native draws forced flushes. All arms retained 198,305 triangles,
  2,296 materials and zero missing regions. Panned control/bulk/bulk+warm/control frame medians were
  60.88/59.28/62.51/61.87 ms. The 555 KiB warm cache did not yield a stable frame gain. The scratch
  binaries were removed. The next retained path must avoid the unchanged block's native tree walk,
  per-draw proof and replay together, then insert current dirty/dynamic records by original ordering.
- An upward-only compact trace separated the wave-free branch with a grazing margin. On native 1.2
  the worker median was 21.64 ms versus 23.54 ms for the generic queue, but full frame median was
  64.64 ms versus 63.18 ms. Kept only the fixture under build/water-transport-up-live-20261107.
- A separate host probe observes LS3D LinkTo, SetOn, SetMesh and final release, plus native city block
  activation/deactivation and scene reset. The relevant functions, return conventions and child links
  were checked in both disassemblies. Serial native runs saw real prop link, visibility, mesh and
  release changes. A repeated hide produced no additional visibility event. Changing the camera region
  caused 8 activations/76 releases in 1.2 and 25/69 in 1.0, without ring overflow. The native hooks
  are evidence for invalidating a retained directory, not an enabled cache. Removed the prop and
  scratch hosts and restored the release hashes in both labs. Probe: build/water-source-epochs-20261107.
- A 1.2 topology probe reused the ordered visual list after checking the fields of all 38,190
  reachable frames. Its initial guard correctly rejected the live image because LS3DF was
  rebased. The retry normalized the three verified PE relocation fields and checked every other
  byte of both native enumeration routines. The enabled/disabled/enabled run collected
  199,661 triangles, 2,305 materials and zero missing regions in each arm. Collection was
  29.67/33.82/30.05 ms and wall median 57.28/59.92/56.91 ms. Wall p95 was
  65.59/61.98/65.04 ms. Transient frame frees caused broad invalidation even with the camera
  held. The prototype remains under build/water-source-topology-20261107. The release host
  was restored after closing the exact 1.2 lab process.
- A compact transport shader cached empty shoreline cells for repeated fixed-point steps.
  The first fixture used the release shader's relative include and exercised only the empty-cell
  exit. A rerun with the complete candidate include chain matched all 3,499,200 archived vectors.
  The fixture's optical rays all pointed upward. It therefore cannot measure the shoreline
  cache's effect on wave continuations. The fixture is under build/water-transport-shore-20261107.
- A scratch host queried existing vertex/index range revisions before rebinding the draws.
  The retained token records buffer identity, allocation lifetime and byte range, and the
  lookup rejects a lock, retired allocation, device mismatch or untracked range. It read about
  12,100 records in 1.66–1.98 ms without adding copies. Around 10,500–10,800 ranges were
  initially unchanged, but 381–398 in 1.2 and 709–740 in 1.0 acquired different producer
  revisions during the original native collection. Initial validation alone cannot authorize
  retained geometry publication. Revalidate after native preparation or isolate immutable
  resources from writes during collection. Only one lab ran at a time. Both release hosts
  were restored. Probe: build/water-source-resource-20261107.
- Preserve the revision of a write-watch page when the existing comparison proves it still
  contains the published bytes. A/B/A resource checks found 375/38/368 mid-collection
  invalidations in 1.2 and 685/30/685 in 1.0 with unchanged source counts. No extra shadow
  copies are needed. Uncertain writes still advance the revision. The existing vb.selftest
  now checks both a same-byte write and a changed range. It passed with the lab's DXVK DLL.
  Stock Windows D3D9's managed-index fixture failed in the same two assertions in the preimage
  and new host. The failures are separate from the renamed page checks.
- The pinned x86 release build passed. Host 7CA5833A is installed in both labs. Sequential
  live vb.check read 5,659 pages in 1.2 and 5,277 in 1.0, with zero differing or unread pages.
  The current geometry publication stayed ready during camera moves, with no worker rejections
  in the fixed runs. The observed fixed medians, 83.48 and 139.26 ms, do not establish a frame
  speedup. Both lab processes were closed and their diagnostic controls restored.
- Split the compact hardware upward queue into exact nearest-hit search and shading. Misses
  and opaque hits complete in a specialized shader. Alpha, blend and grazing cases use the
  original ordered tracer. The packed version emits a compact continuation queue and writes
  the full hit payload only for an opaque hit. On the archived 1440x810 city frame it took
  9.09–9.31 ms versus adjacent warm controls at 10.59–10.80 ms. All coverage, distance, opacity
  and facet fields matched. 966 reflected RGB components differed by at most 0.00000135,
  and the final resolved RGBA image matched byte for byte.
- In live 1.2 A/B/A the compact stage split reduced worker GPU trace median from 25.806
  to 18.413 ms. Frame median changed from 82.542 to 80.604 ms, p95 92.853 to 91.272 ms.
  The standalone frame budget remains open. Same-frame comparisons with moving waves and
  changing camera checked 3,499,200 vectors on each of six frames per lab. Both builds
  preserved coverage, distance, opacity and facet fields, with no nonfinite differences.
  Maximum reflected RGB difference was 0.0000446 in 1.2 and 0.0000423 in 1.0. Zero worker
  rejections. The release shader bytecode matches all six tested shader blobs.
  The reference/candidate fixture and moving comparison are kept under
  build/water-transport-staged-compact-20261107 and build/water-transport-staged-parity-20261107.
- Both lab releases now use host 7CA5833A, water x86 83EADFE9 and water x64 07E816FD.
  The split is available on the existing opt-in hardware facet path. The x86 and x64
  targets compiled successfully. Serial 1440x810 runs measured 61.57 ms median in 1.2
  and 109.73 ms in 1.0 on 9Ex. Both kept fresh source data during camera moves with zero
  worker rejections and zero late graphics frames. On a matched 1.2 frame the GPU span
  was 60.46 ms, including 7.21 ms receiver fitting, 9.93 ms late shadow draw and 40.24 ms
  after the shadow stage. The water seed, transport interval and composition were about
  15.76, 21.64 and 0.40 ms. These timings are nested.
- A temporary 1.0 DXVK run of the same release reached 74.71 ms median and 85.44 ms p95
  at the same resolution and four samples. The native scene collector still took 53.35 ms
  in the sampled frame. The run used GPU-ordered interop and had zero late frames or worker
  rejections. The temporary DLL was removed and the byte-verified 9Ex configuration restored.
- An isolated current-ray broad phase tested every retained optical segment against
  world-space instance boxes, keeping off-camera candidates. The archived view selected
  3,241 of 12,067 source instances. Separately, 164 boxes crossed a visible water level.
  Their union was 3,362. The selected acceleration structure held 51,813 triangles and
  87 billboard boxes, versus 196,072 and 463 for the full scene. Retaining the original draw
  IDs and material data gave bitwise identical transport and final RGBA with the filtered
  instances. The mask itself took 5.76–6.68 ms while the adjacent transport arms varied
  between 7.66 and 8.81 ms. The mask is a source demand bound, with no net GPU gain when
  built as a second ray pass. The archive had no downward rays. See
  build/water-source-ray-mask-20261107 and build/water-source-ray-filter-20261107.
- A native packet probe batches consecutive draws without changing native preparation.
  The host takes the current normalized geometry, pins its cache entry through delivery,
  and checks each borrowed texture revision at the first use and before the batch callback.
  The x86 packet arena is about 104 KiB. One 1.2 panned run delivered 572,350 records
  in 16,450 spans with zero revision mismatches. All arms had 195,469 triangles,
  2,261 materials and zero missing regions. Control/candidate/control wall medians were
  63.33/65.78/66.79 ms. This does not establish an end-to-end gain. The experiment
  remains in build/water-source-native-packets-20261107. The lab release was restored.
- A repair queue rasterized requests, solved the original wave trace in compute, then
  used the result only if the ray key matched during ordered raster. In 1.2 the
  same-frame comparison retained all 617,682 covered pixels, but 2,496 seed values,
  2,517 derivative values, 1,562 adjacent frames and 811 depths differed. The full
  seed pass was 17.34 ms versus 16.68 and 15.80 ms in the adjacent controls. The
  full-frame p95 did not improve consistently. The queue was rejected and the release
  restored. Shader and probe remain in build/water-seed-repair-queue-20261107.
- A receiver-fit probe combined both endpoints locally and split group-shared
  atomics across eight banks. The WARP fixture passed. In three serial 1.2
  launches the receiver spans were 5.32/7.28/5.33 ms and wall medians were
  60.34/59.29/66.33 ms. The processes had different native caster poses and
  sample counts, so the wall windows cannot establish a receiver speedup.
  The original reduction and lab host were restored.
- A pre-shadow seed probe reused the same opaque depth and generated the
  existing ordered water seed before the native shadow draw. A native-size
  DLAA run confirmed the early seed was consumed by the late transport pass
  without additional worker rejections. Control/early/control medians were
  95.64/78.97/70.98 ms, with p95 289.96/90.73/91.03 ms. The early run moved
  the seed into the measured receiver interval, from about 5.5 to 29.4 ms
  while the later bridge interval went from about 35 to 17.9 ms. The source
  and timing windows varied, and reordering did not establish an end-to-end
  gain. The isolated host/water pair remains in build/water-pre-shadow-seed-20261107.
  The original host source and both lab files were restored.
- A conservative prefix skip reused the existing local wave height envelope
  to skip runs of saturated 0.3 m steps. A same-frame comparison kept
  coverage but changed 529,980 of 619,186 seeds and 303,465 depths. The seed
  pass changed from 17.57/17.30 ms to 15.92 ms, with no stable whole-frame
  gain. The probe remains isolated in build/water-seed-prefix-20261107.
- The 1.0 buffer publication calls at LS3D RVAs 66250 and 58960 traverse
  entire native buffer trees to find locked records. The 1.2 equivalents
  at 697E0 and 5B960 iterate only the locked list. An isolated probe
  queued up to 256 consecutive native object/lightmap preparations before
  calling both original publication functions. In 1.0 the combined cost
  fell from 13.26–13.81 to 4.25 ms per collection. Full-scene geometry,
  attributes, billboards, material inventory and restored device state
  matched in 1.0 and 1.2, including a second camera. The production change
  enables this order for matching native functions in 1.0. Full-frame
  timing remains open. See build/water-source-prepare-batch-20261107.
- Release host 64E9B1F0 passed serial native checks and is installed in both
  labs. The 1.0 capture copied 16,017 draws and 252,294 triangles, and 1.2
  copied 16,039 draws and 253,297 triangles. No unreadable draws or missing
  preparation. Device state restored in both labs. A moved prop's new
  position appeared in the next collection, and a removed prop left no
  entry. The live full-view windows ran with independent GPU activity
  from Horizon Client, so those frame times are not the quiet-system gate.
- The sky-shading probe evaluated ray differentials only for opaque hits
  and queried soft shadow only when the cloud opening contributes. On the
  archived native frame its 8.64–8.65 ms staged transport overlapped the
  8.62–8.82 ms controls. The final RGBA was byte-identical, with 966
  changed RGB components in raw transport at a maximum absolute
  difference of 0.00000135. No metadata changed. The candidate did not
  improve the interval consistently and remains in build/water-transport-sky-20261107.
- A fixed 1.2 view reused the existing shadow map in an isolated ceiling
  probe. Wall medians were 58.12 ms current, 53.66 ms with held shadow,
  60.28 ms with both source and shadow held, 48.62 ms with source held,
  and 56.34 ms after restoration. One both-held GPU span was 37.89 ms,
  but the water transport interval rose to 42.25 ms while its worker
  trace was 15.68 ms. This is a scheduling lead, not an accepted stale
  resource mode or a 30 FPS result. Probe and release preimage are in
  build/water-shadow-ceiling-20261107.
- Telemetry confirmed the single-slot GPU relay waits for the previous
  consumed fence on the x86 thread. With frozen source and shadow, it
  waited up to 24.50 ms and refused 23 of 35 submissions in one sampled
  interval. Each refusal ran the software transport for that frame.
  The two-slot nonblocking probe only reuses resources after the prior
  read is queued on the same immediate context. It cut the sampled
  refusal count to 9 on the producer, but the worker then rejected
  another 9 requests because its selected allocator was still busy.
  The normal fresh-source frame showed no gain. This is a bounded
  scheduling experiment in build/water-transport-pipeline-20261107.
- The second relay probe waits at most 40 ms for the chosen worker slot
  only after the exact prior completion fence reports that it is busy.
  The producer orders reuse on the same D3D11 context after the previous
  read. With source and shadow held, 36 consecutive submissions used the
  hardware path without a refusal or software fallback. Median was
  40.73 ms and p95 42.36 ms, versus 64.93/66.28 ms adjacent medians with
  22 fallbacks in each control. The fresh-source runs were 69.94 ms for
  the candidate and 61.71 ms for the control. The relay remains isolated.
- On the current NVIDIA driver D3D12 accepted the D3D11 legacy shadow
  handle directly. The cross-process probe kept the 8192-square R32F
  allocation in x86 D3D11 and read 320 exact texels on each of two
  consecutive frames in x64 D3D12. Only the separate NT fence handle
  was duplicated. Vulkan did not advertise cross-type re-export for
  any of the queried R32F handle classes. The direct KMT path is a
  driver observation. The documented D3D12 import contract uses an
  [NT handle](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-opensharedhandle).
  Native fallback and ownership after an unacknowledged submission
  remain required before any live use. See build/water-shadow-handle-20261107.
- Separate same-frame source collections used the normal XZ domain and
  a 4 cm Y slab around the current native water level. In each build the
  full view reduced about 12,000 instances to 444 and produced the same
  2,177 waterline elements, grid cells, cell ordering and unknown bounds.
  The panned view used 452 instances and exactly matched 2,243 elements.
  The same comparison passed with game time running. Each cold narrow
  snapshot cost 62–70 ms, so the current independent builder is not a
  fast bootstrap until its material and mesh caches are reused.
  See build/water-shore-source-split-20261107.
- Reusing the narrow builder's owned caches cut subsequent collections
  to 15.97–16.83 ms, including 0.19–0.55 ms to verify or import textures.
  The separate builder retained about 13.6 MB of texture data and 3.1 MB
  of mesh data. Both fixed and panned waterline models stayed identical.
  This cost is still comparable to the entire ordered wave seed interval.
- An archived native frame reused the D3D11 shadow and seed arrays as
  direct D3D12 inputs. Copied and direct inputs produced identical raw
  transport and final RGBA files. Worker transport was 7.83–8.15 ms
  with copies and 8.03–8.06 ms with direct inputs. This verifies the
  driver path and isolates the copies from worker tracing time. Live
  ownership and the full frame interval still require separate proof.
  See build/water-transport-direct-images-20261107.
- A current-displacement upper bound per 4 m world cell skipped height
  evaluations at positive gaps where the original stride was already
  saturated. The same-frame comparisons covered 604,692 full-view and
  57,319 panned pixels with identical seeds, derivatives, adjacent samples,
  depth and surface output. The 1.2 seed took 15.02/13.43/15.56 ms in
  control/probe/control, while frame medians were 63.62/62.61/58.50 ms.
  The probe remains isolated in build/water-seed-cell-bounds-20261107.
- Serial release checks used the full river, native 1440x810, fresh
  sources and opt-in DXR facets. Across 45 frames per lab, medians were
  64.61 ms (15.48 FPS) in 1.2 DXVK and 97.56 ms (10.25 FPS) in 1.0 9Ex.
  P95 was 71.98 and 117.90 ms. Sampled source collection was 34.22 and
  38.27 ms, and 1.0's D3D11 wait was 35.56 ms overlapping GPU work.
  Both labs had host 64E9B1F0 and water 83EADFE9 / 07E816FD.
- The owner asked whether a small wave adjustment could close the frame
  gap. In one 1.2 process, 35 frames per arm at the same camera and
  native 1440x810 gave frame medians 81.73/61.06/60.12/59.96 ms for
  original amplitude, amplitude multiplied by .8, 3D displacement
  disabled with normal-map ripples retained, and the restored original.
  GPU seed samples were 16.40/13.84/.92/14.54 ms. With 3D displacement
  off, transport was 22.80 ms versus 18.69 ms after restoring it. The
  first control was slower, so the later arms establish no repeatable
  large whole-frame gain. The original settings were restored and both
  labs closed.

### 2026-11-07 — C++ and Rust retained-cache replay

- The owner authorized an isolated language comparison for the retained
  scene cache. The x64 replay uses the original 96-byte GPU instance
  records from the 1.2 and 1.0 archives, 12,067 and 12,259 rows. Spatial
  groups and the 96-frame event sequences are synthetic. They exercise
  motion, removal, slot reuse, material changes and scene reset. Updates
  preserve the original instance slots and regenerate the affected group
  bounds. The full control regenerates all groups from the current rows.
- Each executable compares the full and retained publications word for
  word on every frame. MSVC, clang-cl and Rust agree on the cumulative
  checksums C53634D4B4EE63E9 and 9F28AF94F0C849DA. Timed runs also agree
  on their final output. They run serially on the same logical CPU, in
  nine rotated orders of 30 repetitions per arm. The binaries use
  MSVC 19.44.35211 or clang-cl 21.1.8 with /O2 /fp:precise, and Rust
  1.94.0 with opt-level=3, codegen-units=1 and target-cpu=x86-64.
- For the 1.2 moving case, full refresh takes 79.17/80.59/82.63 us and
  retained refresh takes 0.220/0.205/0.217 us in MSVC/clang-cl/Rust order.
  For 1.0 the same medians are 79.74/81.98/83.53 us and
  0.212/0.203/0.222 us. The retained streaming interval is 0.91–0.95 us
  across both inputs. The language differences at retained times are
  small relative to run variation. The steady update refreshes 24 rows
  instead of about 12,000. These measurements start after native data and
  dirty groups are known. Native discovery, revision proof, native draw
  bypass, GPU publication and frame time remain separate gates. No lab
  launch, installation or shader change was needed for this comparison.
- Sources, input generator, runner and raw results are under
  build/water-cache-language-20261107. The input archives remain read-only.

### 2026-11-07 — Native one-block preparation bypass in Rust

- The owner chose Rust for the new cache and asked to revise the
  algorithm. The existing SDK accepts a source region and has no per-block
  exclusion. An isolated x86 Rust mod scopes the existing native OBJ_ and
  LMAP preparation detours to one synchronous owner collection. Both
  thiscall signatures were rechecked in Ghidra using the existing build
  tables. The Rust probe leaves ordinary world rendering alone.
- In the two lab builds, two camera positions each exercised a native
  city sector block. The matched reference/bypass/reference captures
  skip 33 actual native preparation calls and 73 observer draws.
  Remaining geometry digests, native draw order, unknown boxes,
  transforms, effective material state, detailed sampler states and
  billboard metadata match. Both unmodified controls match in each case.
- The raw stage-1 texture binding changes on six remaining draws because
  its previous value is inherited. Every affected draw disables stage 1
  through D3DTOP_DISABLE. Both native D3D9 and the existing reflection
  material shader skip this stage. The Rust comparison canonicalizes
  only this unused binding. See the
  [D3D9 texture stage contract](https://learn.microsoft.com/en-us/windows/win32/direct3d9/texture-blending-operations-and-arguments).
- This diagnostic compares same-frame borrowed geometry, state and texture
  identities. It does not retain texture contents, prove block revisions
  across frames or publish the result to the water shader. The measured
  capture windows include the diagnostic and show no stable speedup from
  one block. The next gate is an owned Rust block publication whose
  generation expires on native membership/resource changes, with dynamic
  draws taken live. Both labs ran serially, unloaded the probe and closed.
  The Rust source and native-results.json are in
  build/water-rust-block-20261107.

### 2026-11-07 — Rust wave intersection alternatives on the native archive

- The owner requested sustained work and delegated the live retained source
  implementation to the water_rust_optimization agent. The root isolated
  the wave solver work to avoid concurrent lab use and shared source edits.
- The Rust replay reads the 1.2 native 1440x810 archive's captured FFT
  displacement, slopes, shape, shoreline cells, camera, and water seeds.
  It evaluates 9,123 sampled primary rays against a CPU bilinear reference
  of the current three cascades and three inverse displacement steps.
  Removing the shortest geometry band while retaining its shading normal
  removes 24 hits and adds seven. Shared hit distances differ by 0.0254 m
  at p95 and 2.7222 m at the maximum. Normal deltas are 5.42 degrees at
  p95 and 28.58 at the maximum. Reducing inversion to two steps with all
  three cascades changes three hit states and shifts a shared hit by up
  to 0.2093 m. Both approximations remain outside the live renderer.
- Conservative signed height intervals omit a displacement evaluation
  only when the original step is saturated at 0.3 m and its gap sign is
  proved. The whole-field bound reduces displacement samples from
  4,498,524 to 3,721,716 and the per-ray bound to 3,494,820. Both match
  all reference hit states and distances bitwise, including misses. The
  per-ray interval uses the existing 8-texel tile hierarchy, a 29.2 m
  travel bound and an outward horizontal chop margin. The expanded
  replay restricts omission to current known shoreline coverage and
  tracks the unknown flag. On all 337,754 covered rays, the global and
  per-ray bounds preserve the same 310,923 hits, exact distance words
  and shoreline status. The reference evaluates 166,801,800 displacement
  samples, compared with 137,927,160 and 129,560,124 respectively.
  This archived view has zero uncertain traces. Live wave/shore
  coverage, GPU seed and image parity, and frame timing remain separate
  gates. Source is in build/water-rust-wave-solver-20261107.
- A Rust analysis of the same frame's 2,270 packed materials found
  1,905 distinct raw values. Canonicalizing terminated texture stages
  retained 1,905, excluding unused alpha-test operands reduced it to
  1,399, and excluding unused blend operands reduced it to 1,251.
  Active resources, unsupported state, culling, flags and texture
  factor remain part of the key. This checks the packed shader contract
  after material validation. The live producer must still prove active
  content, and no material IDs or shaders changed in this analysis.
- The Rust archive comparison verifies identical payload bytes after
  rebasing local BLAS offsets and resolving raw mip page descriptors.
  Separate 1.2 captures share 838 of 852 textures and 8,097 of 8,181
  active BLAS payloads. They account for 98.93 percent of current mip
  bytes and 95.26 percent of current node/triangle bytes. The stable
  texture bank retains 68,285,548 initial bytes, appends 727,724 bytes
  and publishes 199,840 current descriptor bytes. Both old and current
  page tables still resolve every archived mip byte. A 1.0-to-1.2
  replay also passes, with 484,008 new bytes and 74,271,160 resident bytes.
  This models an owned 64-bit/GPU resource bank, independently of native
  object identities. Live publication still needs producer generations,
  GPU fences and bounded retirement. Source is archive_reuse.rs in
  build/water-rust-wave-solver-20261107.
- The same Rust bank also interns local BLAS payloads with exact node
  and triangle checks after rebasing addresses. For two separate 1.2
  captures, the first publication occupies 5,318,688 bytes and the next
  appends 469,680 bytes. Both old and current instance tables recover
  the exact original mesh and all other instance words. The current
  instance table is 1,158,432 bytes. For the 1.0-to-1.2 replay the
  corresponding BLAS sizes are 5,809,200 and 530,592 bytes.
- The first isolated archived GPU replay showed a CRT assertion because
  its working directory lacked native_pack_transport.dxil. Only that
  test process was stopped. A Rust launcher now checks the complete
  input set, records stderr/stdout and places the existing fixture on a
  private, unshown desktop with a 20-second process deadline. A negative
  check confirms a missing shader is reported before process creation.
  See [STARTUPINFOW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfow)
  and [CreateDesktopW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createdesktopw).

### 2026-11-07 — Rust retained source in the river collector

- The isolated implementation in build/water-rust-retained-20261107
  matches a retained draw to its active city block, LOD0 native group,
  transform, effective material and texture content, and exact vertex and
  index allocation/range revisions. Rust controls admission and lifetime.
  The existing native producer lends its current normalized geometry
  synchronously. The retained cache holds draw metadata only. A failed
  validation returns to the full collector in the same frame.
- The 1440x810 river view at (-1336.83, -4.94, 198.3), with a 608 m
  source bound, was checked serially in both builds. The 1.2 parity probe
  replayed 1,523 draws from 470 earlier-frame visuals in their current
  traversal positions among 12,095 draws. In 1.0 it replayed 1,746
  from 583 visuals among 12,035 draws. Add, move, hide, show and remove
  cases matched a separate native control on geometry, draw order,
  effective material, billboard state and missing-source bounds.
  Inactive alpha-test references and disabled stage texture pointers are
  ignored in the comparison. The tested active operands were exact.
- In the actual 1.2 water collector V5 used 6,675 to 7,360 retained
  rows across 2,061 to 2,348 native visuals in the measured warm frames.
  The run replayed 136,498 rows successfully with one early full fallback
  and reseed. Warm preflight was 12 to 13 ms, native collection 35 to
  41 ms, and postflight 4.5 to 4.8 ms. The latter two proof phases still
  repeat work per native group. The 1.0 batched vertex-pool boundary
  remains in the trial host, with the marker in original replay order.
  The integrated frame-time comparison and stream-out/reload gates are
  open. Both labs were restored to their pre-test binaries.

### 2026-11-07 — Stable Rust banks pass the existing GPU renderer

- The Rust bank generates a rebound archive for the unchanged D3D12
  water transport and D3D11 resolve shaders. The first rebound only
  moved local nodes and triangles, and changed 36,696 final pixels.
  Compact shading uses the physical triangle address to read t26 UV
  and prelit attributes. The accepted archive interns that exact data
  with t16 nodes and t17 triangles, moves t26 in lockstep and leaves
  instance transforms, draw IDs and material IDs current.
- Across separate 1.2 captures, the exact geometry/attribute bank
  reuses 7,927 of 8,181 pages and 91.21 percent of current bytes. It
  appends 1,904,816 bytes to 21,928,704 initial bytes. The texture bank
  appends 727,724 bytes and publishes 199,840 bytes of current page
  descriptors. Both prior and current publications still resolve the
  exact original payload and all instance words except the rebased root.
- On the RTX 3070 archived GPU replay, 196,072 triangle candidates and
  463 billboard boxes feed the same four optical samples at 1440x810.
  Raw transport SHA256 F7896E5A and final RGBA SHA256 DCE6745B match
  the original archive byte for byte, for both copied and direct image
  inputs. The test uploaded the complete fixture once, so it proves
  GPU addressing and visual equivalence independently of live upload
  scheduling. The Rust launcher completed on a private desktop without
  a visible window. It now preflights native_unpack_words.dxil from the
  nested relay dependency as well as the top-level shaders.
- Source and GPU evidence are in build/water-rust-wave-solver-20261107,
  with the corrected archive and outputs under gpu-bank-exact.
- The Rust D3D12 uploader allocates the four atlas buffers once, copies
  the initial 90,214,252 bytes, then appends just 2,632,540 bytes to
  the same resources. Separate readbacks match every byte in both
  publications. On the RTX 3070 the initial and append copy intervals
  including upload allocation, CPU staging, submission and fence wait
  are 41.130 and 1.638 ms. These are isolated transfers, with the
  changing descriptor and instance tables outside the measurement.
  Each submission uses a single access mode per default-heap buffer,
  following the D3D12 COMMON promotion and decay rules documented at
  https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12.

### 2026-11-07 — Live retained proof and consumer timing

- Bulk guarded reads move visual, record and mesh validation out of the
  per-group loop. Native group membership is checked as a complete chain.
  The trial only reuses a row at the original native marker, where the
  current vertex and index allocation/range revisions still match.
  The batched revision adapter uses one scoped device and returns the
  exact per-row decisions to Rust. Warm preflight falls from 12–13 ms
  in V5 to about 3 ms in V9. Warm postflight falls from 3.4–3.8 ms in
  V8 to 1.31–1.37 ms in V9. The optional diagnostic full producer
  sweep is off in the later timings.
- Rust interns the consumer's immutable validated material words and
  holds the active texture proof. The fast SDK adapter references the
  existing owned mesh and uses the same current texture/material IDs
  at the first ordered occurrence. It skips the ordinary Add path for
  6,659 and 7,178 rows across 925 and 967 templates in 1.2 and 1.0.
  The matched 1.2 warm staged worker sample falls from 9.42 to 8.00 ms,
  and the 1.0 sample falls from 9.03 to 8.33 ms. The worker sees only
  owned C++ scene objects and numeric template keys. No resource from
  the game is retained by Rust beyond a single collection.
- V9 full-frame A/B/A medians at native 1440x810 are
  66.83/65.71/71.88 ms in 1.2 and 109.41/112.06/108.52 ms in 1.0.
  Candidate p95s are 96.93 and 129.19 ms. The retained source path
  does not establish a consistent full-frame gain. V7 instrumented
  OBJ_ native preparation fell from 22.44 to 10.85 ms in 1.2, but the
  complete native walk fell from just 36.30 to 35.14 ms. The GPU
  water surface itself took about 38–42 ms.
- Independent cursors in the bounded lifecycle log allow the parity
  probe and the live consumer to observe the same native changes.
  A hot reload clears all retained state in each build and learns it
  again. The stream-away/back case changes active blocks from
  244 to 213 to 244 in 1.2 and from 113 to 75 to 113 in 1.0.
  1.2 parity matches both transitions. The initial 1.0 stream-away
  diagnostic detected five changed retained rows and 35 producer
  revisions after the marker. Its marker did not validate current
  ranges. The integrated path did. The diagnostic correction and
  repeat are a separate gate before claiming stream parity.
- Both labs were run serially and restored. Phase measurements,
  warm/frame metadata and all V9 transition results are in
  build/water-rust-retained-20261107/timing-v6-v9.json.

### 2026-11-07 — Signed wave limits and ordered interval search

- A shader trial reuses the existing three-cascade height hierarchy to
  bound the complete current repair ray, including horizontal chop.
  It skips height evaluation only when known shore coverage and the
  signed gap together prove the old saturated stride. In one 1.2
  process, same-frame comparisons match all six seed, derivative,
  adjacent, depth and surface checks on 605,067 full-view pixels and
  57,603 panned pixels. An earlier comparison matched 618,947 pixels.
  The first test script treated the second coverage count as a mismatch.
  The corrected gate checks both coverage counts and the six delta
  counters separately.
- Serial control/trial/control medians over 30 frames per arm were
  61.93/61.56/65.91 ms at native 1440x810 with four optical samples
  and hardware facets. Seed samples were 15.77/15.64/18.02 ms.
  The controls vary enough that a full-frame gain is not established.
  The scratch shader remains under
  build/water-rust-wave-solver-20261107/seed-signed. The verified
  release 83EADFE9 was restored after closing both 1.2 test PIDs.
- The Rust interval-search candidate visits only conservative intervals
  that might cross, then evaluates the full original height field at
  fine leaves. On 9,123 archived rays it uses 4,146,780 displacement
  samples versus 4,498,524 and finds 372 additional hits. Matched hits
  have 0.094 mm p95 and 0.144 m maximum distance change. The candidate
  costs 122.63 ms versus 115.64 ms and changes coverage. Keep the
  original solver in the release.
- An exact grouped-stride Rust variant preserves the sequence of
  rounded 0.3 m additions within each IEEE exponent range. It skips
  multiple iterations only when both endpoints have the same certified
  signed saturated gap. Across all 337,754 archived rays, hit status,
  distance and shore uncertainty match bitwise, including 310,923
  hits. Compared with the per-ray bounds loop, both execute 129,560,124
  displacement samples. The grouped CPU loop took 3.476 versus 3.609
  seconds. This is not a GPU or live frame-time result.
- The same archived ray set performs 10,796,677 full height evaluations
  after the existing signed bound. Deduplicating its world-space demand
  into 16-cell tiles would need 697,646, 2,220,387 or 7,353,027 corner
  evaluations at .5, .25 or .125 m spacing respectively. These are
  demand counts, not GPU costs. The initial sparse .5 and .25 m
  interpolation proposals then refine against the original three
  cascades. On a 9,123-ray sample, matched p95 distances differ by less
  than .24 mm, but some ambiguous rays choose a different crest up to
  2.72 m away and 8/9 extra hits appear. This is a failed visual
  equivalence gate. The sparse atlas is still isolated.
- A grazing-incidence cutoff removes the extra hits in the sample
  but still selects a different crest by up to 2.72 m. Adding a
  local approximate turning-point guard leaves up to 1.18 m of error.
  A strictly conservative two-metre cell cache instead preserves
  all archived hit words. It reduces the already bounded 10,796,677
  height evaluations to 10,234,989 and takes 3.530 versus 3.449 s in
  the Rust replay. No GPU build of the cell cache.

### 2026-11-07 — Rasterizing the same inverse water field

- The experimental domain shader samples the full three-cascade
  inverse height field used by world-space wave rays. It retains
  native patch coverage, shoreline inputs, four optical samples
  and the existing radiance shader. Its direct raster mode removes
  the long per-fragment primary repair.
- In a 1.2 stock/field-only/field-with-exact-repair/stock run at
  native 1440x810, water seed times are 15.93/1.83/19.06/15.96 ms.
  Transport intervals are 19.79/19.81/19.43/19.93 ms, with no
  sampled worker fallback. Frame medians are 62.28/71.44/63.33/61.70
  ms. The measured receiver and bridge timing varies. The seed
  reduction does not produce a full-frame gain in this run.
- The direct field mode reintroduces black streaks in the native
  low-angle capture. Its central grazing crop has 1,140 pixels
  with all RGB channels below 24, versus zero in the stock
  frame. Exact repair restores zero in the same crop. The test's
  opt-in hardware path also shows the known magenta unknown-source
  marks in both controls. This rejects the unguarded direct raster
  path. The release DLL is restored and both test processes closed.
  Captures and scratch shaders are under
  build/water-rust-wave-solver-20261107/field-raster.

### 2026-11-07 — Spatial hardware facet publication on Rust

- A Rust planner partitions rigid native facets in XZ by the center
  of each current world bound. Each group's reuse is exact ordered
  equality of the world-to-local transform and the nine local vertex
  words per triangle, independent of reassigned source addresses.
  The source order, material and physical triangle index are still
  carried by the current candidate map. Billboards stay procedural.
  Comparing 1.2 frames 33 and 296, 32 m groups retain 837 of 852
  groups and 177,548 of 196,072 triangles. Sixteen metre cells retain
  179,792 triangles in 2,104 groups, and 64 m cells retain 171,921
  triangles in 280 groups. These are independent archived captures.
- The x64 Rust publisher built 852 triangle BLASes and a billboard
  BLAS on the RTX 3070. It suballocates the bottom results at 256-byte
  offsets inside a 14,507,008-byte arena and uses 3,669,248 bytes of
  scratch. UAV barriers order bottom writes before the top build and
  the top write before tracing. The current map rebases the GPU
  candidate using cluster InstanceID plus PrimitiveIndex.
- The archived 1440x810 image still uses all four optical samples and
  the original shader. Both copied and direct inputs match the flat
  AS control bitwise, including the full transport and final RGBA
  files. The clustered cold build took 2.306 ms. Warm tracing ranged
  7.73 to 8.81 ms in the isolated run. This first GPU run builds every
  group; reusing resident BLAS addresses in a second publication
  remains the next gate. No lab or release change. Evidence:
  build/water-rust-wave-solver-20261107/gpu-cluster-exact.
- The second publisher builds the previous real capture and retains
  837 GPU addresses in the current capture, with the current TLAS
  mapping all groups to its own facet order. It rebuilds 15 dirty
  triangle groups and the billboard group. New BLAS bytes are
  1,342,720 versus 14,712,320 in the previous capture. The current
  preparation interval is .749 ms. The separate cold build of the
  same current capture was 2.306 ms. The previous capture's 4.746 ms
  uses upload-heap input and is not a paired cold control. Both
  current full transport and RGBA files remain bitwise identical.
  The Rust result arena leases, source plans and replay are in
  build/water-rust-wave-solver-20261107/gpu-cluster-reuse. This is a
  two-capture GPU transition, not a live frame-rate result.
- A live scratch pair uses a small x86 Rust directory of owned
  mesh revisions and exact placement words. A separate channel
  carries group spans with the current owner, cycle, frame and
  device generation. The x64 publisher keeps previous arenas only
  within a bounded retention budget. It continues with the existing
  flat AS if the new group plan is unavailable. The original shader
  and material tables are unchanged.
- Fixed-view A/B/A medians over 30 frames are 55.96/57.88/55.61 ms
  in 1.2 and 89.94/91.81/88.15 ms in 1.0. Both native paths reuse
  the whole unchanged AS. After moving the camera, the cluster
  path retained 798 and 794 groups respectively with zero missing
  source regions.
- A 13-position camera out-and-back at the same 1440x810 resolution
  and optical sample count gives 64.78/62.13/59.50 ms in 1.2 and
  80.60/80.49/82.92 ms in 1.0. Candidate p95s are 81.21 and
  99.52 ms. Moving clustered frames reuse approximately 830 to
  845 groups. The candidate does not establish a repeatable
  frame-time gain. Both labs restored the release x86/x64 hashes
  83EADFE9/07E816FD and removed the exact experimental DLLs after
  closing each test PID. Scratch: build/water-rust-wave-solver-20261107/live-cluster.

### 2026-11-07 — Complete native branches and replay fallback

- The Rust census follows both builds' verified EnumFrames and
  EnumSubtree code, normalizing the three PE-relocated operands in
  the signature. Only a complete native branch can skip traversal.
  Its current active block, root/parent, topology and visibility
  words, ordered visual set, coverage bounds and all producer
  receipts must agree. Link updates record both parents in the
  topology log. Insertion occurs at the original traversal position.
  1.0 replaces one original preparation batch slot with each accepted
  branch marker. A failed range check runs the corresponding native
  preparation at that marker and preserves the remaining batch spans.
- Census: 1,583 complete 1.2 roots contain 4,021 topology nodes and
  6,289 retained source rows. 1.0 has 1,700 roots, 4,278 nodes and
  6,644 rows. The roots are often small pooled models, independently
  associated with their logical city blocks. V12 emitted 1,625 and
  1,550 branches in the live collectors, replaying 6,703 and 7,113
  rows from Rust respectively. Branch planning sampled 0.96 and
  1.36 ms. Warm native controls compared 413 and 391 branch markers,
  including add, move, hide, show and remove cases. All packet
  geometry, effective materials, order and missing bounds agreed.
- The corrected 1.0 stream-away parity marker rejected six visuals
  on the live receipt check. Seven emitted receipts then changed
  later in the same collection. The final proof refused that
  candidate and kept the same-frame full native control. The return
  view matched all 12,070 current draws. The integrated path preserves
  this fallback and may relearn when current native data is stable.
- V11 whole-frame A/row/branch/A medians were
  70.44/68.16/67.86/65.13 ms in 1.2 and
  105.12/103.06/103.29/105.81 ms in 1.0. The V11 1.0 branch
  sample had two full fallbacks before the single-slot fallback
  correction. No repeatable whole-frame gain follows from this test.
  Both labs restored their original binaries. Raw proof and timing
  records are in build/water-rust-retained-20261107/timing-v10-v12.json.

### 2026-11-07 — Ordered dirty spans and owned mesh replay

- V13 creates bounded Rust-owned dirty ranges from the current and
  preceding immutable CPU publications. Records keep their exact
  current slots. The D3D11 adapter copies the common GPU buffer and
  writes only those spans, with the original path on dense changes.
  At most 16 ranges are submitted per resource, merging the smallest
  unchanged gaps. The current t19 placements, t20 order and material
  words use the planner. Two x86 Rust tests cover exact reconstruction,
  grow/shrink and dense fallback.
- Live accepted plans in 1.2 stage 3,224,348 of 53,326,812 logical
  bytes and in 1.0 stage 952,196 of 42,868,268 bytes. Planning averages
  0.53 ms per accepted plan in each build. The D3D11 copy and existing
  x64 full import still run. A/B/A medians of 71.85/80.44/88.47 ms
  and 104.09/104.21/110.23 ms establish no repeatable frame benefit.
- V14 checks all current VB/IB receipts and hide rules at a branch
  marker. The SDK then accepts the range only when every row's exact
  owned mesh token still resolves. Reused rows skip borrowing or
  maintaining the host's normalized geometry. They still use the
  current material/texture publication and draw order. The producer
  checks after capture remain in place. The old path handles a miss.
- On the full native river view the warm owned range supplies all
  6,728 and 6,956 retained rows at the 8,192-row metadata limit.
  A separate 12,288-row trial supplies 7,317 and 7,600 respectively.
  Host normalized source bytes fall from 22.53 to 11.85 to 10.92 MB
  in 1.2 and from 22.46 to 11.39 to 10.27 MB in 1.0. Those figures
  exclude Rust metadata and are not process memory totals. A/B/C
  medians are 70.89/69.42/68.94 ms and 110.88/108.88/110.83 ms.
  Warm source collection still takes around 38–46 ms inclusive.
- Current worker timing samples show approximately 0.15 ms for input
  unpack, 1.4–1.7 ms for bounds/AS and 15.4–17.9 ms for the native
  transport trace. The larger remaining budgets are source collection
  and optical transport. Both labs restored the original binaries.
  Evidence: build/water-rust-retained-20261107/timing-v13-v14.json.

### 2026-11-07 — Shared native stamp and allocation proofs

- V15 compares the same visual fields without allocating a new parent
  path on every read. During one preflight it memoizes current parent,
  sector light and native group words by exact address. The 1.2
  sample validates 7,192 retained rows using 1,469 unique group reads
  and one sector light stamp. Preflight including branch planning
  samples 5.10 versus 4.78 ms and whole-frame median 66.10 versus
  64.81 ms. This is a modest sample, with no final frame claim.
- In 1.2, 7,052 retained rows share 109 renamed vertex allocations and
  11 native index allocations. They reference 7,052 and 1,451 distinct
  byte spans. V16 records exact allocation lifetime IDs in Rust and
  after native collection copies an owned page revision vector for
  each active renamed allocation. An unrenamed index buffer supplies
  one current whole-allocation revision. Rust reconstructs exact
  maximum page revisions for the original ranges. The current marker
  checks remain unchanged. Missing ownership or an unavailable
  vector restores direct range validation.
- Live same-phase audit against every direct native decision returns
  zero disagreements across 119/122 allocations, 2,547/2,646 pages
  and about 7,100/7,700 current rows in 1.2 and 1.0. Grouped and direct
  postflight samples are 1.62/1.50 and 1.71/1.83 ms. Full-frame A/B
  medians are 67.82/66.37 and 102.21/102.28 ms. No repeatable frame
  gain was established. Both labs restored the original binaries.
  Evidence: build/water-rust-retained-20261107/timing-v15-v16.json.

### 2026-11-07 — Current ordered topology tape and partial repair

- V17 records the native frame topology as an ordered forest in Rust.
  The host exports owned topology stamps and reads the scoped ordered
  visual list synchronously. Current dynamic entries still use native
  preparation. Both scene roots, raw link and visibility fields, live
  producer receipts and lifecycle cursors must agree. This preserves
  the original 1.0 preparation batches and current native order.
- V18 isolates changed subtrees beneath the same ordered outer roots.
  V19 updates the part directory in place and reuses previous node
  storage. Parent links, child order and exact retained stamps gate
  repair. A detached or ambiguous part loses its retained publication.
  In the 1.2 timing sample, one of 14,490 parts required rebuilding.
- Same-frame V19 cold and warm controls matched native draw order,
  effective inputs and visits in both builds. The 1.2 controls visited
  37,975/37,976 nodes with 12,061 draws and 1,464 reused rows. The 1.0
  controls visited 25,397 nodes with 12,020 draws and 1,711 reused
  rows. The warm 1.2 control exercised partial repair.
- Full native 1440x810 V19 A/B frame medians were 71.15/66.78 ms in
  1.2 and 110.79/108.65 ms in 1.0. Native collection sampled
  28.91/23.82 ms and 32.49/31.25 ms. In 1.2 the full preflight
  sample rose from 5.77 to 10.22 ms, offsetting most of the sampled
  collection savings. V17 and V18 timing was mixed. This does not
  establish a repeatable gain or approach the 30 FPS gate. Both labs
  restored the original binaries.
  Evidence: build/water-rust-retained-20261107/timing-v17-v19.json.

### 2026-11-07 — Queued optical facets and owned row planning

- The optical transport probe carries the original 32-bit facet
  components in each queued sample. First-hit, opaque and ordered
  dispatches reuse that facet; adjacent normal differentials and
  all continuation rays are unchanged. The native 1440x810 archive
  reproduces both raw and composed output byte for byte with all four
  optical samples. Warm adjacent control passes take 8.88/8.82 ms
  and the two candidate passes take 8.84/8.84 ms. The queue allocation
  grows by 40.0 MiB in x64 GPU memory. No live variant was warranted.
  Evidence: build/water-rust-optical-cache-20261107/timings.csv.
- The V19 profile sample records 7,258 retained of 12,079 staged
  draws and 11,775 previously compatible instance shapes. Staging
  publication still takes 9.92 ms, full CPU packing takes 14.00 ms,
  and the game thread spends 2.96 ms joining the owned worker.
  The source take remains current. The profile is saved as
  build/water-rust-retained-20261107/profile-v19.json.
- A 32-bit Rust replay assigns current first-occurrence IDs and
  ordered primitive bases to all 12,067 archived draws. Full material
  and mesh equality yields 8,181 mesh IDs and 1,905 material IDs.
  The measured replay is 0.354 ms median, 0.378 ms p95. Removal,
  reordering and a new revision agree with the reference at
  0.353 ms median, 0.370 ms p95. The timing excludes native collection
  and the SDK adapter. The trial planner does not hold input pointers
  after the worker invocation.

### 2026-11-07 — Local wave repair and sparse water shadow demand

- The locally bracketed field trial costs 62.03/67.53/60.08 ms over
  stock, candidate and restored stock. The geometric visible-normal
  trial costs 63.42/66.76/56.24 ms. Its seed costs 16.60/16.74/14.75 ms.
  Both trials retain the low-angle dark-band check with zero dark
  pixels in the tested crop. The visible-normal candidate runs its
  raster path there. The trials stay isolated because they do not save
  repeatable frame time. The original 1.2 water DLL is restored.
  Evidence: build/water-rust-wave-solver-20261107/field-raster.
- The native 1440x810 frame has 337,754 water shadow receivers inside
  the 8192-square map. Rust projects their seeds and expands them by
  33 texels to cover the 32-texel PCSS radius and rounding. The union
  takes 6,269 of 65,536 tiles at 32 texels per side. Erasing 56,944,864
  nonclear texels outside those tiles preserves the complete raw
  transport and composed RGBA byte for byte on the RTX 3070, through
  both copied and direct input paths with four optical samples.
  Evidence: build/water-rust-wave-solver-20261107/gpu-shadow-mask.
- Bounding the same actual hits in 8x8 screen groups preserves the
  per-pixel mask in a superset of 21,843 tiles. Rust coalesces it into
  73 disjoint rectangles in 0.322 ms. The 16x16 screen grouping needs
  22,257 tiles and 61 rectangles in 0.242 ms. This replay uses known
  seeds. The current-frame conservative depth-slab coverage can be
  larger and its effect on native raster time is still unmeasured.

### 2026-11-07 — Live Rust owned row planning and the 1.0 bridge

- V20 calls the Rust planner from the owned x86 snapshot worker.
  The C++ adapter resolves pending mesh objects and current textures
  first. Rust assigns first occurrence IDs and original primitive
  ranges, and maps the previous packed mesh prefix to current IDs.
  It keeps only numeric proof metadata. The original staged path
  handles incomplete input or failed planning.
- A live same-frame comparison with the original C++ publication
  checks 12,005 draws in 1.2 and 12,067 in 1.0 with zero material,
  mesh or primitive-base differences. Both frames have zero missing
  regions. The audit runs only for the comparison.
- Serial A/B/A native medians are 77.80/65.00/66.79 ms in 1.2 and
  105.46/103.16/104.79 ms in 1.0. Their sampled staged publication
  is 9.25/6.07/8.40 and 8.93/6.79/7.85 ms. The worker stage is
  faster in both runs. Overall timing and join cost remain variable.
- A temporary 1.0 DXVK run uses the same V20 host and water code
  at 1440x810. Its B/A/B medians are 60.43/61.65/66.26 ms.
  The handoff reports zero D3D11 CPU wait, zero late frames and
  zero worker rejections. The worker continues processing the
  current four-sample frame. Its native collection still takes
  34–38 ms and the finalize wait samples 14–16 ms. The original
  9Ex INI and both binaries are restored. The 30 FPS gate is open.
  Evidence: build/water-rust-retained-20261107/timing-v20.json.

### 2026-11-08 — Opaque waterline cache and current source attribution

- V21 keeps exact opaque contact elements in Rust, keyed by current
  owned mesh token, all world matrix words, plane levels and floating
  point controls. Cutouts, blends, uncertain regions and revised poses
  still take the original current calculation. The 2 MiB element
  budget belongs to this bounded trial.
- Same-frame full model comparison passes cold and warm in both labs.
  Each model has 2,177 elements, 769 nodes and 36 uncertain sections.
  Warm frames reuse 141 source rows. The comparison includes original
  element and tree order, bins, unknown regions and provenance.
- Frame A/B/A medians are 63.94/63.03/62.78 ms in 1.2 and
  95.33/97.98/101.65 ms in 1.0. Waterline samples are
  3.92/4.52/3.90 and 3.87/3.69/4.09 ms respectively. No useful
  gain was established. Both original lab binaries are restored.
  Evidence: build/water-rust-retained-20261107/timing-v21.json.
- A separate 33-frame source attribution with timing_sources enabled
  puts the native walk at 26.05 ms per capture. Nested ordinary
  object preparation takes 9.47 ms, lightmaps 2.05 ms, billboards
  3.00 ms and posed meshes 1.26 ms. Ordinary callback time accounts
  for 5.30 ms inside its preparation. Rust marker processing, bounds,
  filtering and other traversal account for the remainder. Preflight
  samples 10.22 ms, including 6.65 ms in topology verification and
  repair, with 1.69 ms postflight. These intervals overlap and should
  not be added as independent consumers.
  Evidence: build/water-rust-retained-20261107/profile-v20-sources.json.

### 2026-11-08 — Current receiver tiles and native stencil trial

- An isolated native receiver reduction publishes current 8x8
  screen-group light-space bounds in the same readback as the existing
  six bounds. A 1.2 frame has 625,525 conservative ray intervals and
  9,991 active groups. Rust projects these into 39.01% of 32x32 shadow
  tiles and coalesces 52 rectangles in 1.04 ms.
- A Rust static library projects those owned bounds with the first
  actual native caster matrix and includes the full PCSS footprint
  plus arithmetic error. The C++ adapter uses D24S8 when available,
  clears stencil coverage and keeps native depth/alpha cut-outs inside.
  The existing native view state block restores stencil. Inputs belong
  to the current frame. Early shadow consumers keep the complete map.
- The first version unnecessarily restored nine states per caster,
  increasing the late shadow GPU interval from 10.92 to 15.96 ms.
  Retaining stencil state within the native view removes that overhead.
  The second trial still costs 10.35/10.96 ms for control/candidate
  late shadows and 62.95/64.36 ms per complete frame. Rust planning
  samples 0.68 ms. A later restored-control pass rises to 202 ms and
  cannot support a stable three-pass frame-time comparison.
- The 1.2 water crop differs by at most three 8-bit levels at p95,
  like the restored control. The 1.0 launch uses early shadows and
  correctly leaves the tile path inactive. The runner initially
  expected activation and stopped at that check. Both original host
  hashes are restored. The mask stays isolated because native caster
  traversal/submission still offsets the saved raster work.
  Evidence: build/water-rust-wave-solver-20261107/live-shadow-tiles.
  Native clear rectangles use Microsoft's viewport coordinates and
  D3DCLEAR_STENCIL contract.
  https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-clear
- The combined inverse height field and visible geometric normal
  trial cuts 1.2 seed GPU time from 15.84 to 1.94 ms and total water
  GPU time from 38.22 to 19.50 ms. Whole-frame A/B/A stays
  58.80/59.91/56.22 ms, exposing the remaining source/caster critical
  path. The four optical samples and native 1440x810 inputs remain.
  The water crop p95 difference is 10 levels, versus four for the
  restored control. Near the wave envelope the trial uses the original
  seed, with zero dark pixels in the low-angle regression crop.
  Existing opt-in unknown-coverage magenta is present in both controls
  and candidate. Further appearance and combined source testing remain.
  The original water DLL is restored. Evidence: build/water-rust-wave-solver-20261107/field-fast.

### 2026-11-08 — Separate original 1.0 copy and frame pacing report

- Owner paused water work and requested a separate 1.0 game containing
  only original-game technical fixes. Created W:/Mafia10Fixes from the
  configured 1.0 lab using copied original archives and engine files,
  with independent saves. Recorded source and copy hashes under
  build/fixes-only-1.0-20261108. The local players map records this copy.
- The initial selection included conservative Fusion Fix, FPS timing
  corrections and dxwrapper. The owner explicitly excluded all three
  and requested native rendering, crash fixes and gamepad hotplug.
  Moved those components, FFmpeg and every mod outside the copy into
  the repository's ignored backup. Only MafiaModLoader.asi and the ASI
  loading DLL augment the original game. The installed mods folder
  is empty. The original French data language is retained.
- Native 1.0 reaches the menu and city-small with host 64E9B1F0.
  The process loads Windows' own d3d8.dll; no dxwrapper is loaded.
  Graphics bridge generation remains zero with no passes. The log
  confirms the traffic-slot, negative waypoint and readable-control-name
  fixes. joy.state reports hotplug enabled and device-interface watching
  after entering gameplay. No physical reconnect was performed.
- Owner then reported periodic native 62 FPS hitches disappearing after
  applying and removing an RTSS cap below 62. Verified both native
  GetTimer implementations: Sleep is followed by adding the requested
  duration to the internal time, without measuring actual sleep.
  The current timer resolution changes from 9973 to 4960 units of
  100 ns after the owner's RTSS transition. This is a causal hypothesis
  pending a fresh-process and controlled timer request comparison.
- The native D3D8 frame profiler ring is empty. Existing tap snapshots
  can read the host Frame QPC pair from the exact matching PDB without
  modifying frame pacing. After RTSS, 499 measured intervals have
  median 15.873 ms, p95 16.865, p99 17.360 and one interval over 20 ms.
  The coarse GetTickCount values in tap logs are not precise frame times.
  Evidence: build/native-frametime-20261108.

### 2026-11-08 — Native frame wait timer precision fixed

- A fresh unmodified 1.0 process reproduces the original hitches even
  with a system timer query reporting about 0.5 ms. Its 499 QPC
  intervals have p95 30.753 ms and 84 intervals over 20 ms. Calling
  timeBeginPeriod(1) on the game's main thread gives p95 16.863 ms,
  maximum 17.363 ms and zero over 20 ms. Releasing that request with
  timeEndPeriod(1) brings the periodic long frames back, with p95
  30.749 ms and 48 of 299 intervals over 20 ms.
- The timer request must belong to this process. Microsoft's
  timeBeginPeriod documentation describes the per-process behavior
  since Windows 10 version 2004 and the visibility caveat on Windows 11.
  https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod
- Added one lazy process-owned 1 ms multimedia timer request immediately
  before the first native frame limiter call. It runs outside DLL
  initialization, resolves LS3DF's existing winmm import and releases
  exactly the successful request at host teardown. Failure logs a warning
  and retains native waits. Native min/max arguments, frame step,
  Present, rendering and the original 62.5 FPS ceiling are preserved.
- Targeted MafiaModLoader build and path checks passed. Installed host
  SHA256 3024670110A7A6145DA68D6EC26AFF4BEF8649CDE52D4222CE559B99002F2296
  in the fix-only copy and both configured labs. Matching PDBs accompany
  both lab hosts. Preimages are under build/native-frametime-20261108.
- Fresh native D3D8 gameplay in W:/Mafia10Fixes records 600 of 600
  returned steps at 16 ms, with 599 measured intervals, p95 16.865 ms,
  p99 16.873 ms, maximum 17.361 ms and zero over 20 ms. The exact
  rebuilt PDB's data section moved by 0x1000. A first snapshot used the
  previous section RVA, yielded zeros and was discarded before repeating
  the real QPC sample with validated frequency 10,000,000.
- Serial lab checks unload optional mods for the current run, preserving
  their saved manifests/settings. 1.2 has 400 of 400 returned steps at
  16 ms and 399 intervals with p95 16.865 ms, maximum 17.383 ms. 1.0
  has 400 of 400 steps at 16 ms and p95 16.371 ms, maximum 17.487 ms.
  Neither sample has an interval over 20 ms. All taps are removed and
  the exact test PIDs are closed. These are native timing checks, not
  the complete offline graphics/mod regression suite.

### 2026-11-08 — Foreground cursor confinement and crash-report verification

- Owner reported background lists scrolling under the invisible Windows pointer while the game has focus.
  Read MouseInit in both builds: mode 0 requests DirectInput cooperative flags 6 (nonexclusive + foreground),
  mode 1 requests flags 5 (exclusive + foreground). Hiding the Windows cursor alone does not confine it.
  Owner selected confinement over intercepting wheel events.
- Windowed and bridge-borderless primary windows now confine the pointer to their full window rectangle,
  including the resize frame. The window procedure releases immediately on deactivation, destruction,
  disabling and modal move/size/menu entry. Bounds refresh on placement and between frames; background
  launches do not clip. Release only undoes a rectangle still matching this host's last successful clip.
  Exclusive fullscreen retains native DirectInput ownership. No global wheel-routing preference is changed.
- Targeted host build, path check and diff whitespace checks passed. Host SHA256
  DF4E874538EEC307D8BB57F8FF10CD4B38C36D38E5B183E184458A7FE3ABA680 installed in both labs and
  W:/Mafia10Fixes, with matching PDBs in the labs. Preimages and isolated tests: build/input-crash-20261108.
- Native D3D8 1.0 gameplay (fix-only copy, PID 1460) and serial 1.2 lab (PID 9320) each passed nine actual
  Win32 assertions: foreground confinement, immediate release and restoration for app/window activation,
  move/size and system-menu messages. These checks send only window messages inside the game, never real
  mouse/keyboard input. The 1.2 kit also verifies all eight resize arrows remain visible and correctly shaped,
  with balanced cursor visibility and no pointer/focus changes. A normal background 1.0 lab remains unconfined.
- The owner had enabled RUNASADMIN for the fix-only copy, so plain MCP CreateProcess returned error 740.
  With explicit owner authorization, ShellExecute elevation via Start-Process -Verb RunAs launches it normally.
  Compatibility registry settings are preserved. Test processes are closed by their verified identities;
  lab mod manifests remain unchanged (optional mods unloaded only in the current run).
- Verified the active fix-only copy's crash filter remains first and chains the game's filter. Ran the existing
  five-case crash regressions in an isolated hash-verified harness, without crashing the owner's game.
  Every case saves its log, paired JSON report and minidump: normal AV, hooked call chain, later foreign filter,
  native read-only write, and an injector-style thread without TLS. All assertions pass. crash_triage opens the
  final saved dump and identifies the expected null write on the non-main thread. The crash source is unchanged;
  this is verification of the existing reporter, not a promise that every future failure can be repaired.

### 2026-11-08 — RTSS automatic detection recovered after reboot

- Before reboot, RTSSHooks.dll was absent from the owner's running native 1.0 copy. Game and RTSS both had
  elevated tokens in the same session; extension-point mitigations were off. Restarting RTSS, including its
  64-bit helper, did not recover automatic injection. With owner-authorized restarts, the same copy also failed
  to autoattach with saved pre-timer host 64E9B1F0 and timer-only host 30246701. Thus a direct causal link to the
  timer fix was not established. Restored the current DF4E8745 host afterward. No RTSS profile was created.
- An isolated D3D8 probe was abandoned without a useful result after the owner directed the investigation to
  the exact before/after builds. Later manual DLL loading was diagnostic only, not an automatic-injection fix.
  The reboot ends that diagnostic process and its temporary callback tap. No host source change was made for RTSS.
- After the owner rebooted, launched the exact same W:/Mafia10Fixes copy with current host
  DF4E874538EEC307D8BB57F8FF10CD4B38C36D38E5B183E184458A7FE3ABA680 and native Windows D3D8, PID 29056.
  RTSSHooks.dll loads automatically without a native.call LoadLibrary. The documented RTSS shared-memory list
  registers Game.exe with API flags 0x3 (D3D8), and live counters report 61 frames in 1015 ms, about 60.10 FPS,
  latest frametime 16772 us. The native frame timer fix and cursor confinement remain installed.
- RTSS shared Flags remain 5 (OSD visible plus limiter disabled); the configured 12 FPS limit is not enabled by
  this verification. Did not change RTSS settings or re-enable its global limiter. Automatic attachment and live
  frame measurement are confirmed; the precise pre-reboot cause remains unknown. Read-only diagnostics and
  restart evidence are under build/rtss-20261108. The game is left running for the owner.

### 2026-11-08 — Ragdoll connected to the original-renderer 1.0 copy

- Owner explicitly requested adding the ragdoll mod to W:/Mafia10Fixes. Installed the existing loader mod
  ragdoll 0.1.0 from the configured 1.0 lab; the 1.2 lab has identical four source files. Manifest supports
  both builds, has no dependencies and requests no graphics bridge. DLL imports only KERNEL32.dll.
- Hash-verified every copied file and retained the matching PDB. Installed DLL SHA256
  36E042466C59EEA672BDBB084EA118A46B0FCC166CE92310F5400A89A510BB66. Set only the destination manifest's
  enabled flag true; retained the existing mod defaults (including debug drawing off). Other labs' files
  and saved mod enablement were untouched. Backups/evidence: build/ragdoll-fixes10-20261108.
- mods.rescan hot-loads generation 1 in the already-running original 1.0 process, PID 29056; no restart,
  renderer replacement or core/Fusion Fix/FPS unlock installation. ragdoll.state reports enabled true,
  world true, seven tracked people and scenery with 29 shapes. Log confirms Jolt 5.6.0 starts, the 1.0 dodge
  fix is active and the static scenery set builds successfully. This is an installation/load check, not a
  fresh full physics regression. Updated the copy's README, local description and installed-file record.

### 2026-11-08 — Ragdoll port parity: source audit and handover restoration (in progress)

- Compared the standalone reference read-only with mafia-ragdoll-mml. Jolt bodies/joints/masses/muscles remain
  substantially the same; the missing behavior is in lifecycle binding. Restored staged get-up hold, actual
  pelvis yaw/horizontal fit, animation-side check, real root placement and pedestrian grid following.
- Added owner-scoped cancellation, mission-time progression, validated explicit physical-pose handover and
  Rust bindings. The port cancels an old handover on re-knockdown and supplies the physical pose before removal.
  Verified the track-lift field in both native PersonAnim implementations and suppressed it for this back get-up.
- 1.0 initial regression: both body sides with the pelvis displaced three metres, native completion and pause
  passed seven assertions. Re-knockdown cancels the active handover. Final SDK/port path and 1.2 remain to verify.
- A pedestrian's car-hit event pos is its root, not contact: restored reference centre for fallback car impulses.
  Taxi-waver collision investigation remains separate; do not alter the native boarding script or clear flags
  speculatively. Full comparison: docs/RAGDOLL_PORT_REVIEW.md. Backups: build/ragdoll-review-20261108/before.
  Labs have water temporarily disabled for these tests; restore their saved manifests afterwards. The owner's
  running W:/Mafia10Fixes process and files have not received these new fixes yet.

### 2026-11-08 — Ragdoll parity block validated and installed

- Completed the preceding in-progress block: both 1.0 and 1.2 passed all 18 focused assertions in
  tests/game/test_getup.py, run serially. Physical-pose fitting, both sides displaced three metres, pause,
  re-knockdown cancellation, owner unload, native completion and pedestrian grid following are covered.
  Gentle car touches no longer invent falls; real native knockdowns still become physical.
- Added shared human_vehicle_contact (C ABI, Rust and bridge): validated living target/current car,
  occupant rejection, native pedestrian car-hit path including taxi/damage reactions, native C_human hit.
  The mod gathers actual car-hull/living-capsule contacts in a sensor layer and handles misses after the
  physics step. Only the car's own approach qualifies; no animated-arm attack against a parked car.
- An external strike had incorrectly invoked the open-door occupant exemption, disabling the entire car hull.
  The striking hull now stays solid. Retained true seated/doorway behavior and the original articulated solver.
  Pedestrian fallback impulses use the centre rather than an event root misinterpreted as contact.
- Natural taxi wavers with deliberately removed native proxies were hit through the new measured path in both
  builds: final-code 1.0/1.2 contact at about 2.38/2.40 m before the pedestrian, with native hit/wave termination
  and solid hull. This validates fallback coverage; it does not prove every original native miss has one cause.
  Native boarding scripts/flags and solver tuning are unchanged. Full evidence: docs/RAGDOLL_PORT_REVIEW.md.
- Hash-verified final host 5A8902E8A3611B2C8AC657F362C102D9212EDE3FB5E3A8AA2FC106EE9CA339A2 and mod
  9B6447E668972692832AC829245E051EE8DF1CA406C05A5123CCF3248D8DEAE4 with matching PDBs in both labs and
  W:/Mafia10Fixes. Elevated restart consumed the prepared start request; PID 15356 passed physical-pose fit
  and native completion checks. Windows' system D3D8, only ragdoll loaded, no graphics pass; RTSS autoattached.
- Both labs are closed; exact saved water manifests restored. Default game_dir and saved ragdoll options remain.
  C example compilation, Rust i686 check, generated methods/game tables and targeted whitespace checks passed.
  No new crash in the focused checks. No claim of exhaustive mission/vehicle coverage. Initial backups and
  final evidence: build/ragdoll-review-20261108. AGENTS.md now explicitly preserves the owner's authorization
  for necessary close/install/restart cycles during requested work, without asking again.

### 2026-11-08 — Follow-up: visible pedestrian get-up teleport

- Owner identified the visible 3.778 m displacement in the latest get-up. Archived the actual session log before
  restarting. A real native pedestrian fall reproduced a 3.966 m jump after the initial fit had reported zero
  horizontal pelvis error. The old test's instantaneous fit assertion was insufficient.
- Native SetAnim retained the old fall's TCK trajectory, and FitGetUp moved only the model/element. Native Tick
  subsequently wrote the old cached origin back. Discard obsolete root tracks when starting this handover;
  after fitting, use native RecompileMatrix to rebase the active trajectory and its offset. Rebase pedestrian
  sweep history and travel vector as well. Verified the implementation in both builds (40E900 / 5AB410, ret 4),
  shared address/layout tables updated and Rust regenerated. Skeletal pose blending remains active.
- Both builds passed all 20 expanded assertions serially, including a two-second real native fall/get-up
  continuity check. Repeated 1.0 traces no longer jump metres (largest sampled step 0.162 m). Excluded a later
  dead-target attempt from get-up evidence. Address check, Rust i686 check and generated tables/methods passed.
- Investigated the low-speed stone-like dodge report separately. A sharp stop at z=117.63 also occurs on a fresh
  native run without ragdoll (6.58 to 0.72 m/s); world.shapes identifies the curb. The controlled free-street
  native dodge/strike did not reproduce the reported stop. The cause remains open, not fixed by assumption.
  Added bounded same-frame hard-stop logging with Jolt contribution and nearest pedestrian's animation,
  physical state and element/model separation. Evidence/backups: build/ragdoll-followup-20261108.
- A final 1.0 controlled strike at about 4.5 m/s had at most 0.114 m/s of sampled speed loss, no hard stop.
  The below-knockdown-threshold probe also found repeated native weak contacts in both modded and fresh
  modless runs (largest sampled loss about 0.57 m/s). These probes do not identify the owner's unique incident.
- Final host F78DCF7E539DC0ED0BC4D8E92F21561A62F6B55E7C9A934E7E1A2E87C5EB544A and mod
  FB195440FBA066B0EE2B6014F9D64057710209C57DCD284715EDD77E2BCDCD5C, with matching PDBs, are installed
  in both labs and W:/Mafia10Fixes. The owner's native D3D8 copy passed the new continuity checks too;
  final restart PID 15164 has only ragdoll loaded, RTSS automatic attachment confirmed. Both labs closed,
  original water manifests restored exactly. No new runtime faults/crashes observed in these checks.

### 2026-11-08 — Exact live corpse spine-frame diagnosis

- Owner supplied a twisted-body image and asked for direct memory inspection, explicitly prohibiting computer
  use. Captured the exact closest pedestrian 1AB5AAF4 from PID 15164, including physical quaternions, initial
  landmarks and skin bindings, with compiler-verified x86 offsets and loaded-DLL hash verification. Evidence
  and pre-edit/back-up binaries: build/ragdoll-joints-20261108. The mesh followed physics within 0.04 degrees.
- Found a real construction error: near-horizontal curved spine fronts were independently signed against the
  model's standing heading. One flipped almost 180 degrees; initial-pose widening then allowed the wrong
  rotation. Shared anatomical landmark axes replace that sign rule in port Setup and Jolt Add. Solver/tuning
  unchanged. Previous get-up/impulse fixes did not address this distinct cause.
- Added ragdoll.inspect with actual physical joint ranges and binding errors. Header geometry regression and
  corrected native-lying A/B runtime tests pass. Rejected the first render-only pose fixture after initial
  landmarks showed it actually captured an upright native pose. Final focused block: 11 passed per build,
  serial labs, including current-pose handover/cancel and native fall/get-up continuity.
- One intervening continuity check recorded a 0.721 m step; no timing/state retained for that sample. A full
  follow-up trace was smooth (max 0.063 m); final runs passed. Kept the outlier open in Known gaps, not hidden
  by the passes. Native D3D8 screenshot timed out despite a ticking game; capture-tool gap also recorded.
- New mod B3CC97E11B1170C06E4DC0ADF27DF4D82D36BF24D872551AF5D960D96236A596, PDB
  34B044A4EC4779B8E034A6E0E1EC6C184F37460823E39142D5EE2C869FCA72D8 installed in both labs and Mafia10Fixes.
  Host unchanged F78DCF7E... . Exact water manifests restored, labs closed; owner copy restarted elevated.
- First native restart PID 24904 failed at RTSSHooks.dll+12AB (read 0B6B90DE), before any physical ragdoll.
  crash_triage was the first diagnostic; stack through ProcessWinMessages/scene Render, dump/report retained
  in the game's mml/crash. No RTSS/global settings altered. An unchanged restart PID 3308 passed six native
  lying assertions, actual live-DLL hash and only-ragdoll checks (eight total). RTSS attached; render.state
  confirms direct3d9=false, original native renderer. Continued ticking after the checks; interaction cause open.

### 2026-11-08 — RTSS focus-return root cause, and broader ragdoll anatomy work

- Owner's next crash (PID 3308, Game_3308_20261108_224720.dmp) repeated RTSSHooks+12AB on focus return by
  mouse after Alt+Tab. The earlier short successful restart was insufficient and should not have been accepted.
  crash_triage first; disassembly then proved the interaction: our FF 25 abs32 chain thunk is read as an
  RIP-relative stub. thunk 028F0310 plus 6 plus head-cell 08AC78E8 equals invalid read 0B3B7BFE exactly;
  the earlier dump similarly gives 0B6B90DE. Both actual head cells contain a valid host detour.
- Use equivalent x86 FF 24 25 abs32 via no-base/no-index SIB. No register/flags/stack changes, mutable head
  remains a pointer. Host 751B4154E45530CB1F342765BA6F4FE3D3AE6FA627011A4D80E8FC63DB23B96D and PDB
  2C97AE1428FFE4D06E57308757512273A5F3F6D877C387B45A1B4F3FB8DB362C installed in all three copies.
  Actual live RTSS recognizer under native.call's SEH guard reproduces the old fault at the exact offset;
  new encoding returns normally 20 times. Temporary RW data page released, no executable allocation.
  Isolated real chain/mid-hook regression passes 23 checks, integrated into run_tests.py. Thirty internal
  activation-message cycles pass per game (both serial labs and native user copy); these are not physical
  Alt+Tab automation. Owner's manual clicks work; rarity explicitly acknowledged. No RTSS settings changed.
  Evidence: build/rtss-focus-20261108. No computer use. Elevated launch initially left a hidden window;
  window.resize restore showed it and Win32 visibility was verified before repeating the user test.
- Owner then reported prone body/head inverted, twisted arms, and falling while walking over a stationary
  corpse. Exact sleeping pedestrian 18BE9C94 captured with all physical bodies/bindings/skeleton and joint
  report (anatomy-live-1794167898534748000.json). Neck allowed -172..70; elbows/ankles also widened. Initial
  skin/head alignment was plausible, but physical evolution inverted it. This is broader than trunk signs:
  head/nose and hand/foot helpers used standing horizontal facing; joint frames/limits were derived from
  a posed death instead of the model's rest skeleton. Current port work calibrates against SDK model_nodes,
  retains full posed bone orientation and removes permanent initial-pose widening. Initial 1.0 checks pass.
- Walking feedback: old PersonBlows used each limb's point velocity after earlier pushes, allowing a walker's
  own push/spin to be reinterpreted as an incoming body. Work now samples incoming assembly momentum before
  pushes, gates a blow on whole-body translation and preserves current-velocity resolution for the push itself.
  This was the intermediate state; final validation/install and the additional finite-effort correction follow.

### 2026-11-08 — Ragdoll finite effort, final anatomy/contact installation

- Completed rest-skeleton calibration, exact work attribution and shared anatomical segment masses in the port.
  Owner's heavy-Tommy/light-corpse report found that every part supplied fresh 20 kg impacts every frame outside
  the force budget. All locomotion impacts and overlap pushes now share maxForce*dt per person; the body's own
  incoming reaction remains separate. Body mass remains 78 kg, existing 700 N default unchanged. Diagnostics
  are in ragdoll.state.person_push, not another UI slider. Native walkers remain kinematic (finite-effort coupling).
- Final ECB39D43... DLL / 75FDA2FB... PDB: 43 assertions passed in each serial lab. Actual 9 m/s whole-body
  launches still knocked the same Tommy over after he had pushed the corpse, 39/40 N s. Both native death
  models/lying variants retain head fronts, elbow axes and anatomical limits; get-up/cancel checks pass.
  Evidence: build/ragdoll-anatomy-20261108/final-1.0-1794170951420789800.json,
  final-1.2-1794171039691990900.json and latest thrown-1.0/1.2 records.
- Installed exact DLL/PDB in both labs and Mafia10Fixes with guarded backups in build/ragdoll-contact-budget-20261108.
  Owner native D3D8 process 14248: 28 anatomy assertions passed; the initial walking fixture's 400 ms velocity
  samples missed its >0.25 m/s peak. Replaced that fixture metric with actual >1 cm physical displacement from
  the asleep pose. Nine final contact/hash/only-ragdoll/native-render checks passed, native-final-1794171230960644800.json.
  Total four-crossing effort 962.60 N s, maximum 700 N. No production change for that fixture failure.
- Both labs are closed and water manifests restored to original SHA 45F98C11...; default game_dir unchanged.
  Owner's exact game launched elevated, window explicitly restored and IsWindowVisible verified. No computer use,
  real keyboard/mouse input, RTSS configuration changes, extra gameplay/renderer mods or reference-repo edits.
  Rare car hard stop during dodge and the intermittent get-up sample remain open; focused passing cases do not
  establish their elimination. RTSS host 751B4154... remains installed; latest crash dump still predates its fix.

### 2026-11-09 — Corpse limb shots and palm contact

- The owner's live native copy had 117 taken corpse hits and zero registered capsule hits. Read the two bodies
  by the car and their actual capsules/bones. The closest body had stable elbow constraints and skin binding.
  The wrist was bent relative to a straight forearm collision shape. Live capture is in
  build/ragdoll-bullets-20261109/live-1794171841189400100.json. Controlled shots at both palms, ankle and trunk
  through the native NewShoot queue initially produced no hit in the downward fixture.
- Two independent causes: the engine's native volumes could win before a ragdoll limb, and its part test and
  one-metre terminal-stretch gate could discard the capsule result. Reviewed the exact call sites in both
  builds. `host/hits.cpp` replaces native volumes for registered dead actors and preserves scene obstruction,
  direction, surface point, owner and original exclusion state. The native Hit receives the winning capsule
  exactly once. The scoped terminal hook is byte-verified. Living actors keep the native part test.
- The port routes the local share to the body selected by the bullet capsule. A palm uses the bound wrist
  pose for both its Jolt compound shape and bullet capsule. A lab shot still missed with only the new bullet
  geometry because the physical forearm let the hand dip into the ground. It passed once the ground used
  that same palm shape. The palm is part of the forearm rigid body, with the wrist bend retained from the pose.
- Windows shutdown produced Game_14248_20261109_000801.dmp. CDB with the exact PDB put the fault in
  Jolt BodyManager::DestroyBodies from the ragdoll entries' CRT destructor after the world was destroyed.
  Explicit Start/Stop owns the world now, so a process exit cannot destroy it before the entries. The exact
  LS3DF ExitProcess import in the 1.0 lab returned process exit code 0 with an active corpse. The 1.2 lab's
  direct ExitProcess test gave a separate WER fail-fast (0xc0000409, parameter 7) even with ragdoll unloaded.
  Neither 1.2 run generated a fresh dump. Normal menu exit was not part of that reproduction.
- Both lab builds passed the 43 anatomy/get-up/walking assertions with the final ragdoll, plus seven focused
  bullet and native hit checks. The same seven passed in Mafia10Fixes on native D3D8. Both palms selected
  their exact capsule and corresponding forearm. Offline hook-chain tests passed 23 checks.
  Evidence: build/ragdoll-bullets-20261109 and the latest 1.0/1.2 records in build/ragdoll-anatomy-20261108.
- Final host 6E969ED2461B95892788C4C3AD744A55BAEB42DC855463C068084B8B55CBEA56, PDB 44C77A41...,
  and ragdoll AE8804E3991C7B73349A6AF2C94D56C24F4A25E2DE4041474ABE248E390AD807, PDB AE939B4E...,
  installed in both labs and the owner copy with guarded backups. Restored lab water manifests byte-for-byte.
  Owner PID 21500 has the exact loaded hashes, original D3D8 and only ragdoll. The window is visible and
  Tommy is on foot near a settled civil15 for manual shots. Two additional owner-copy projectiles selected
  the right foot and chest respectively, with local response in each selected body.

### 2026-11-09 — Get-up after a carried player's blast knockdown

- In Mafia10Fixes PID 21500, `mise10-saliery` to `mise10-mesto` kept Tommy at actor `0x0DBDD0F8`.
  Events 1264, 1309, 1519–1520 and 1572 show the constructor and the two scene closures followed by
  load, with no intervening player destructor or constructor. At 18:34:12 he survived native blast
  damage (events 1616 and 1620). At 18:34:15 the mod logged `could not get up: no current skeleton`.
  The actor still existed, but `human.skeleton` and `human.anim_state` both failed to resolve it.
- Native `CHANGE_MISSION` detaches the player before closing the old scene. The host used to erase its
  actor registry on each close, so the carried actor vanished from the SDK until another constructor.
  `HumansMissionClosed` now clears scene caches only. Native destruction still removes registry entries.
- In each lab, a native mission transfer kept Tommy registered once, destroyed the old NPC, and
  resolved Tommy's skeleton. A native explosion then knocked him down alive. Back get-up track 111
  played with a fitted physical handover and returned him to the native standing controller. Six
  assertions passed in each lab, run serially. Records and backups: `build/ragdoll-getup-carry-20261109`.
- Host SHA 1C9EB262DB2D89C29380BF0F99D153E1454AFF1721A74CD90DA29FB1A080C64B, PDB 74E1468D...,
  installed in both labs. Closed both labs and restored their water manifests to their original hashes.
  The owner's paused story process kept its loaded host. Re-registered the exact live Tommy through
  the matching host's `HumansCreated` entry point, verified by module hash, PDB and live bytes. Skeleton
  (20 bones) and animation state resolved again at the same position. Permanent installation in the
  owner copy requires a closed process. This result concerns the missing get-up track after a scene transfer.

### 2026-11-09 — Rare vehicle launch after loading Omerta

- Mafia10Fixes PID 8516 loaded profile 1 slot 230 (`mise10-mesto`) at event time 54.232. The player's car
  `0x15BFC6C0` logged two upward contacts of about 1 m/s impulse per unit mass at 55.892. At 56.323 the car
  struck a world surface (material 33) with a 114765 N s horizontal impulse. `CAR_CRASH.speed` is the
  impulse divided by vehicle mass, not its travel speed. No source of the preceding acceleration was
  recorded. Explosion followed at 70.309, about 1037 m below the scene's vertical origin.
- Replayed the exact save in 1.0, first with the lab wrapper and then with system D3D8 and the owner's
  host and ragdoll binary hashes. With a 2.3 s throttle input the native runs reached 12.79 m/s with
  ragdoll and 12.72 m/s without. The native replay of two Paulie apartment loads, profile switch and
  Omerta reached 12.41 m/s with 2.2 s throttle and ragdoll. None repeated the launch. This does not
  recreate incidental traffic, pedestrian positions or the original controls. No code or owner game
  changes. Restored the lab binaries and removed the temporary save copy.
- Do not repeat save loads to search for a rare contact. A future capture needs a bounded pre-trigger
  trace of car velocity before and after native physics and applied Jolt deltas, with contact actor or
  surface and controls. Original events and replay curves: `build/ragdoll-getup-carry-20261109/car-launch-8516.json`.

### 2026-11-09 — Vehicle incident recorder

- The host keeps 512 occupied-car ticks and 512 contact/mod records in two fixed rings (176128 bytes).
  Snapshots bracket the native mission tick and each mission-tick mod callback. The native snapshot
  includes input, gear, RPM, wheel loads/compression and step time. The existing collision hook adds
  impulse, primitive flags, caller and struck actor. The trace also includes relevant human/pedestrian
  hits and nearby explosions reported inside the tick.
- A gain of 8 m/s in one tick, 12 m/s in 900 ms, or 8 m/s upward over that window saves up to 3.5 s
  immediately, with a 15 s automatic cooldown. No disk I/O in ordinary ticks. `vehicle.trace` reports
  buffer status and can explicitly save a capture. The latest and preceding files survive a restart.
- Four focused assertions passed in each lab serially. An injected vertical speed change preserved
  quiet prehistory, was visible before the native step, and persisted automatically. A native 1.0
  throttle run accelerated inside the native boundary, and a collision named the second car and its
  impulse. In 1.2 a drive into a physical body yielded 37 records attributed to the ragdoll callback.
  The recorder distinguishes callback changes from native-tick changes and records contacts the game
  actually reports. It does not infer an unreported actor from proximity alone.
- Host C5C00132A2ADF687305FB13F74B5E9F452A3D90CC8D6A34A95D41C385B459535 and PDB 80B50866... installed
  in both labs and Mafia10Fixes with guarded backups. Lab captures archived under
  `build/vehicle-trace-20261109`. Restarted the owner copy from Game Over into profile 1 slot 230,
  elevated as configured. PID 18276 confirmed system D3D8, ragdoll only and 512 recorded samples.

### 2026-11-09 — Vehicle launch identified and airport shot telemetry

- The repeat at 20:09 in Mafia10Fixes PID 20804, profile 1 slot 230, triggered at tick 118.
  The first excess velocity came from the ragdoll callback at ticks 117 and 118, 3.0 m/s each.
  The native step added about 0.1 m/s longitudinally on each tick. The wall collision followed later.
  A second capture retained 220 consecutive mod increments while the car was mostly airborne.
- Read the matching live Jolt objects while the game was paused. The car had zero Jolt contacts in the
  last step, but a compound local COM of [-1599.21, -12.50, 24.70] m. Requested COM speed was
  15348.97 m/s, actual clamped speed 500.00 m/s. The exact difference was exported as a car kick.
  The 3 m/s cap turned that false correction into a sustained push. Evidence under
  `build/vehicle-trace-20261109/owner-20804-*`.
- On the same car model, `actor.meshes` gave `g_car_0.Object03` and `Object04` near world zero
  even as their native frame matrices and spheres followed the car. Both objects had bit 0x40 clear at
  visual +0x1D8. Both `I3D_object::m_RenderFGroup` implementations select identity only when that bit is
  set and otherwise use the visual's world matrix. `ActorMeshes` now follows that rule for `OBJ_`,
  independent of the sphere guess. The remaining lightmapped cache is cleared at mission close.
  The port reads back the actual clamped velocity after setting it, before comparing the solver result.
- Exact slot 230 in the 1.0 lab gave both offending meshes their car-relative pose (all parts within
  3.36 m of the car) and zero ragdoll deltas during the 2.35 s drive. The 1.2 car's pieces were within
  3.21 m and a genuine corpse contact still produced ragdoll reaction deltas. Evidence: `launch-fix.*.json`.
- In the separate airport firefight, shotgun events 1719, 1729, 1741, 1743 and 1746 recorded shots with
  no human hit. Event 1754 then produced three native hits on 0x19F0FAE8 and 1784 killed him. Living hits
  were not marked taken. `BULLET_HIT` was absent throughout because the outer capsule hook changed the
  inner event hook's return address. The outer hook now passes the final ray to the same reporter.
  A real projectile on static ground with a capsule registered emitted exactly one event in both labs.
  The original five misses remain unattributed. `owner-20804-airport-bullets.json` holds the events.
- Both labs have host D8D4B506... and ragdoll B4A6C097... and are closed. The owner's airport mission
  is paused in the original process. The final pair is staged in the build outputs for its next install.

### 2026-11-09 — Airport launch recovery and owner install

- The old owner PID 20804 reproduced the launch in `mise10-letiste` at 20:30. The recorder captured six
  successive 3 m/s ragdoll deltas at ticks 15073–15078. In the last three native steps the car was already
  airborne and its vertical change was gravity, about -0.17 m/s each. Preserved the exact capture at
  `build/vehicle-trace-20261109/owner-20804-airport-launch.json`.
- Returned the same occupied car to unobstructed ground at [76, 0.78, -376] without repairing it and zeroed
  the flight velocity. Saved the current mission with the engine's restore service as
  `airport-launch-recovery-20261109` before closing PID 20804. The profile's slot 240 hash was preserved.
- Hash-verified host D8D4B506... and ragdoll B4A6C097... installed with their PDBs and preimage backups.
  Elevated restart into profile 1 and restored the whole airport mission in PID 25280. Verified Tommy's
  250.88 health, six corpses, stationary upright car, and all 17 car pieces within 3.43 m of its body,
  including the two formerly misplaced visuals. Recorder active and game window responding.

### 2026-11-09 — Living bullet regression and ragdoll interaction boundary

- Owner PID 25280 airport shotgun events 104, 116, 131 and 137 fired ten pellets each and emitted no
  player-attributed human or world hit. Other shooters emitted world impacts in the same window. Enemy08+
  was alive at full health. Native moving and bone rays could reach him, and the correctly called fastcall
  `Can_Hit_FilterHumanType` allowed the pair. Raw events and collision state are in
  `build/vehicle-trace-20261109/owner-25280-airport-bullets.json`.
- Replayed the reported origin and direction against that enemy in the 1.0 lab using slot 240. The old host
  hit 1/1 and 10/10 before registering a distant corpse. With the distant corpse active the same replay hit
  0/1 and 0/10. The terminal gate at 0x5E56AF (1.0), 0x5A9D5F (1.2) begins with JNZ. The shared mid-hook
  stub ran `sub esp, 4` before PUSHFD, clearing the game's ZF before the trampoline used it. `lea esp, [esp-4]`
  reserves the same slot with the original EFLAGS. The terminal callback continues to set ZF for the matched
  dead-body capsule only. With the corrected host and capsules active, the exact replay hit 1/1 and 10/10.
  `airport-shot-replay-fixed.jsonl` holds the complete result. The focused stand-in suite passed 24 checks,
  including both incoming ZF values alongside the existing register, x87 and continuation checks.
- The port now publishes capsules by actual dead state, leaving living knockdowns and debug puppets to native
  hit detection. Added a game regression for living targets with a separate registered corpse. Both builds
  passed that case, both corpse palm shots, world hit reporting and the native gentle/hard car-hit threshold.
  The wider 1.2 walking case recorded all four crossings with zero knockdowns. Its timed setup assertion did
  not see the sleeping flag after 3.74 s, though every measured part velocity was zero.
- Car feedback now requires a solid dynamic ragdoll contact during the solver step. The old step result is
  cleared even on a paused step. Removed the pedestrian movement sweep's proximity guess: it could drop a
  native obstacle hit within 3.5 m of a moving car without identifying its collider. The dedicated pedestrian
  car collision correction remains. A separate 1.2 taxi-wave probe drove into a different area from its earlier
  successful fixture and failed to meet the waver (2.11 m minimum separation). Its recorded native car slowdown
  had zero Jolt delta. This did not verify the special taxi-wave fallback or reproduce the owner's rare stop.
- Final port tests also checked that a living puppet has no capsules and the same actor acquires 17 when killed.
  The player drove with 0 m/s mod delta over 51 ticks (1.0) and 50 ticks (1.2). An actual corpse collision still
  produced 45 and 46 reaction ticks, peaking at 0.222 and 0.191 m/s. `car-contact-boundary.1.0.txt` and
  `car-contact-boundary.1.2.txt` contain the measurements and fixture state.
- Installed host 59EA85E8... and port 11F1CBCE... with paired PDBs and backups in both labs and Mafia10Fixes.
  Saved and restored `airport-bullet-boundary-20261109` into owner PID 18072. Verified exact active DLL hash,
  only ragdoll loaded, normal window and 237.3801 player health. Left the restored mission paused at normal time
  scale, with original slot 240 hash intact. The 1.2 walking sleep assertion and rare low-speed hard stop remain
  separate observations.

### 2026-11-09 — Sideways pedestrian slide into a wall after a crowd strike

- Read the still-paused Mafia10Fixes PID 18072, Omerta `mise10-mesto`. The likely body is the last pedestrian,
  0x18E6E20C (event 1853). The car hit at velocity [-0.52, 0.22, -14.43] m/s. The body entered Jolt with
  animation-derived pelvis velocity [11.96, 0.04, -1.02] m/s, at the 12 m/s seed limit. The simultaneous
  hit on 0x18E6EC0C (1852) inherited [11.89, 0.01, -1.66] m/s. The earlier 1851 body started with
  [-1.69, 0.23, -0.06] m/s and its sampled trajectory followed the street rather than the wall.
- At the pause, 1853's pelvis had shifted 2.63 m in +X and -0.29 m in Z. A native box ray found the wall at
  X=-108.808 (surface 33). The head center was 0.21 m across the native face, while the Jolt inspection also
  saw static-world contacts. The evidence identifies the sideways seed before the solver step. It cannot
  distinguish a true native dodge burst from a pose discontinuity. The raw pre-impact pose pair was no
  longer in the tracking map, and the Jolt static mesh need not coincide with the native collision box.
- Evidence in `build/vehicle-trace-20261109/owner-18072-pedestrians-wall.json`. Read-only inspection of the
  live game. The existing fixed-speed clamp retains almost 12 m/s of sideways speed for this seed.

### 2026-11-09 — Separate pedestrian locomotion from animation when starting physics

- A later live car hit on 0x18E7140C repeated the limit in the opposite direction (pelvis [-11.98, 0.02, -0.60]
  m/s at 21:40:25.259). The hit was from a real car hull/capsule fallback, before any launch velocity had been
  carried over. The native movement code in both builds uses element +0x10 for the accepted pedestrian position
  and +0x160 for the current clip. `pedAnimStart` records the TCK endpoint separately at +0x174.
- The port takes the body's bulk speed from consecutive native element positions of the same model and clip.
  Missing, discontinuous and over-limit samples cannot provide a launch seed. Same-clip bone motion still
  contributes local linear and angular velocities. The mass-weighted linear mean is removed before adding
  the native motion. A shared scale keeps the relative velocities bounded while preserving zero net momentum.
- The standalone regression runs production `SetMotionFrom`: a 20 cm visual jump in 16 ms gives the old pose
  estimate 12 m/s and the new stationary native-body seed zero. A 3 m/s native run with an animated forearm
  preserves net 3 m/s and local limb motion. Both labs separately passed all 11 checks for real pedestrian
  knockdown/get-up, grid continuity, native car-hit threshold and palm bullets (`ped-seed.1.0.txt` and
  `ped-seed.1.2.txt` in the incident directory). The exact owner crowd strike and wall crossing were not replayed.
- Port DLL 1D8E110B... and PDB 7F05DFF4... installed and backed up in both labs and Mafia10Fixes. The owner
  was already in `mise11-mesto`, with zero active ragdolls. Hot-reloaded in PID 18072, verified generation 2's
  exact live DLL hash and Jolt startup, and left the mission in place. Both labs were closed.

### 2026-11-09 — Garage blast sequence, scripted car transfer and recovery

- Owner PID 18072 in `mise12-garage`. `forVfor4` (0x16EBFC30) was the car under the camera ray and the one
  occupied in event 5118. Its own native explosion (5125) was followed by Tommy's death and transfer to the
  generated wreck actor (5126–5128). The original scene's `detector_energy` first checks Enemy1–Enemy5 and
  the camera area before arming `detector_Enemy_gr4`; that detector calls ACTOR_SETPLACEMENT for `forVfor4`
  at `position_AUTO2`. In the paused owner run the latter program was still on its first inactive step, and
  `detector_energy` was at its initial wait with flt[6]=0 and flt[0]=55.2065, exactly Enemy1's current health.
  The prior two-blast attempt had left Enemy1 alive at approximately 8.08 health. The native explosion happened
  at the garage start location. Raw events and current script state: `owner-18072-garage-blast.json`.
- A/B on the exact 1.0 slot 290 with two native C_car::CarExplosion calls (`forVfor3`, then `AUTO`, 10.14 s
  apart). Stock Enemy1 remained at [-5.274, 0.265, -162.409] after the first blast. The old mod moved him 2.00 m
  and reduced second-hit damage from 60.44 to 50.09, leaving 5.08 health instead of killing him. Native damage
  processing itself is shared by both. The port now leaves initial nonlethal NPC blast reactions native,
  retaining Tommy's optional blast knockdown and the impulses on existing physical bodies. Replayed with
  the fix: five gate NPCs dead, the native detector moved the same `forVfor4` to [9.580, 6.123, -74.183] with
  Tommy still seated and alive. The test used the owner's slot and native blasts with controlled positioning
  through the original trigger, not a full real-time speedrun input replay. `garage-blast-*.jsonl` has results.
- The separate owner blast at 21:51:42.802 put Tommy into physics. The native get-up was queued only at
  21:51:52.806, the port's ten-second timeout. Recovery now accepts a grounded body below the linear and
  angular motion threshold for 0.30 s, after the existing 2.5 s minimum. Jolt's inactive flag is still a valid
  early reason after the minimum; the scenery wait no longer masquerades as sleep. Replayed the recorded
  blast (24.62 damage in the lab versus 24.86 live) with Jolt sleep disabled only in the disposable test world:
  it stayed awake and began native recovery at 2.512 s, with 1.111 s of quiet motion. Seven live checks passed
  serially on each build, including nonlethal NPC blast damage and Tommy's animation through native control.
- Port 544F3E04... and PDB 9FA62F93... installed with manifest label and backups in both labs and Mafia10Fixes.
  Hot-reloaded owner PID 18072 as generation 3 while the game was paused and nobody was getting up. Verified
  exact active DLL hash, same Tommy health and `forVfor4` under the camera, plus the original slot 290 hash.
  Closed both labs and removed only the temporary copy of slot 290 in the 1.0 lab.

### 2026-11-09 — Garage collision slab and ragdoll restart

- Owner PID 18072, `mise12-garage` slot 290. The second blast was near [1.50, 0.76, -164.04], with Tommy near
  [1.07, 0.27, -173.28]. The game reused the scene on Retry (event 5469), and Tommy was found alive at
  [-0.168, -337.056, -190.082]. The slot's SHA-256 was unchanged. A native ray at z = -174.26 found a
  type-0x83 floor at y = 0.255. A ray from below hit the same plane. A 0.1 m sphere swept from above stopped
  at y = 0.355. Native support ends around z = -180 there.
- A controlled `C_game::NewExplosion` with the recorded position, damage and radius reproduced the initial fall.
  The old port was already below y = -2.8 at z = -175 while still over the native floor. Jolt had only the
  visual meshes. The port now includes the finite, two-sided 0x83 collision slabs in its local scenery cache.
  Both labs kept Tommy on the floor in the blast fixture and returned him to the native get-up.
- A separate off-map death followed by the real Game Over Retry restored the checkpoint with the port unloaded.
  With the old port it respawned the same actor at y = -75.5. The native load can render with the old actor
  and frame still alive. The loader now reports `MML_MISSION_LOADING` inside the native load/new calls and
  clears transient physical-pose blends. The port stops writing to the scene during loading and drops old
  simulation state on `GAME_LOADED` even if the mission never closes. Both builds retried from an active
  off-map corpse to [-3.088, 0.156, -128.031] on the original actor, with no old ragdoll.
- A forced two-car destruction replay on the old 1.0 lab build crashed in LS3D's render pass after creating a
  wreck (PID 24000 dump). The passing blast replay uses the game's native explosion routine without creating a
  wreck. This change is scoped to the floor collision and the load boundary. See
  `build/vehicle-trace-20261109/garage-fall-*.jsonl` for the A/B and `test_same_mission_load_releases_ragdoll`
  for the reusable live-pose regression. It and the carried-player blast/get-up case passed all nine checks
  per build in serial lab runs (`garage-fall-live-1.0.txt`, `garage-fall-live-1.2.txt`).
- Host 7B7E4040... and port BAED20C1... with matching PDBs installed in both labs and Mafia10Fixes.
  Owner PID 15344 is running the new host and verified live DLL after restoring the pre-install native snapshot
  to [-6.007, 0.010, -107.530]. Slot 290 retained SHA-256 FDC26BD7.... Previous files are in
  `build/vehicle-trace-20261109/garage-fall-binaries-before`. Both labs were closed after the checks and the
  temporary exact owner slot was removed from the 1.0 lab.

### 2026-11-09 — Follow Tommy's native explosion reaction

- Owner PID 15344, garage events 302 and 320. A car explosion of strength 200 and radius 10 delivered 27.53
  damage to Tommy at roughly 8.6 m. The old port made a body on any surviving type-2 blast hit. An A/B with
  the same native explosion in the 1.0 lab took 27.16 health in both runs. Stock kept Tommy standing and
  the old port started a physical fall. At 6.7 m stock played a hit animation, and at 4 m it fell and got up.
- Both native `C_human::Hit` implementations use strict type-2 damage thresholds of 50 and 100. The
  constants are at 0x63B44C / 0x63B31C in 1.0 and 0x6234A0 / 0x6234A8 in 1.2. The port now uses physics for
  a living Tommy only when native damage exceeds 100 and the engine selected a fall animation (106–108 or
  114). The engine still controls damage and the lighter reactions.
- Twelve checks passed per build in serial labs, including native damage and no new body at 8.6 and 6.7 m,
  strong blast get-up and same-mission load. The baseline and test logs are in
  `build/vehicle-trace-20261109/blast-reaction-*`. Port A2B8068D... and PDB CB4BAF5F... were installed in
  both labs and hot-reloaded in Mafia10Fixes PID 15344, generation 2. The live hash, actor position, health
  and slot 290 checksum were verified. The hot reload cleared seven sleeping corpse simulations. Both labs closed.

### 2026-11-09 — Diagnose the 1.0 garage load crash

- Owner PID 15344 crashed at 23:03:16 while loading a save in `mise12-garage`, about five seconds after
  car 0x179DB6C8 exploded and the engine created wreck actor 0x17AFEFE8. The latest game event before
  the fault was `GAME_PAUSED`. The native `G_LoadSaveClass::SaveGameLoad` call at 0x55822D had entered
  `C_game::Done` to clean up the current state.
- At 0x5DFD19 the engine called slot +0x18 (I3D frame Update) on a model from a native 0x1C-byte
  record in the vector at C_game +0x2A4. The dump names the frame `ForAca` (0x109D7E78). Five other
  records have I3D_model vtable 0x10098100. The faulting frame's vtable field instead reads 0x1E29D978,
  with the attempted slot read 0x1E29D990 in a reserved, uncommitted region. The stack passes through
  the loader's save hook. The fault itself is in the game's cleanup. The dump establishes an invalid
  vtable but does not establish which code invalidated it or whether it reproduces with the port unloaded.
- Evidence: `W:/Mafia10Fixes/mml/crash/Game_15344_20261109_230316.{dmp,json}`.

### 2026-11-09 — Guard the native vehicle-shadow cleanup

- The records at C_game +0x2A4 are native vehicle fake shadows. `C_car::ShadowCarOn` registers them through
  `C_game::AddFakeShadow`. The dump's ForAca is a separate parked car (matching position in the lab);
  the exploded car is AUTO. The dump also retained the requested save slot, 290.
- Guard the native receiver Update at 0x5DFD16 (1.0) and 0x5A42CC (1.2), after the engine detaches and releases
  its shadow. A readable, callable receiver follows the original instruction. An invalid receiver skips
  the virtual call before any stack arguments are pushed. The native name free and loop continuation still
  execute. The startup byte check and `tools/check_builds.py` verify both native sequences.
- The local `shadow_frame_test` passed with a callable receiver, missing receiver, non-executable target
  and reserved memory, including a reserved vtable as in the dump. Native garage lab runs exploded
  `forVfor3` and `AUTO` and loaded slot 290 with ragdoll active on both builds. The 1.0 pre-fix runs with
  and without ragdoll also loaded successfully, so the original corruption was not reproduced naturally.
  Evidence: `build/vehicle-trace-20261109/garage-shadow-*.jsonl`.
- Host 39B83CBB... and PDB 28E99893... installed in both labs and Mafia10Fixes. Confirmed the hook and
  slot 290 loaded in owner PID 21044. The original slot hash is FDC26BD7.... Both labs closed and the
  temporary 1.0 slot copy removed.

### 2026-11-10 — Garage follow-up and crash summaries

- After the receiver guard was installed, PID 21044 stopped producing frames in the live garage mission on
  November 9. Repeated samples and the active stack put the main thread in heap validation while
  `I3D_shadow::Close` freed the projection buffer of Enemy_gr4_3. This was the regular human-shadow tick.
  On November 10, PID 22436 faulted in the loader's `ProfileStat` during mission-tick dispatch just after
  the garage blast. Neither trace establishes the first invalid write or a common root cause.
- At the owner's request, paused crash debugging and removed the current and rotated loader logs, the two
  paired JSON crash reports and the captured hang snapshot from November 9–10. Preserved the minidumps,
  the older reports and the separate vehicle telemetry.
- MCP `crash_triage` now defaults to the failure site, recorded call stack, mission, mods and recent event
  types with timestamps. Raw crash context is available with `details: true`. The host still records full
  diagnostics locally. A dump with no report yields the fault site in the summary, with build-aware symbols
  from the recorded image stamps. Existing dump-only triage had used the current log to choose the build.
- The rebuilt MCP server passed all 14 Rust tests and the complete MCP smoke test on rerun. Direct checks
  passed on paired and standalone dumps from both game builds. The new server is at the configured path.
  Already connected clients need to reconnect. Source and binary preimages were retained in the build tree.

### 2026-11-10 — Controller arrival before the startup watch

- Mafia10Fixes PID 14360 started at 23:08:24. Windows recorded arrival of the Keychron K4 HE game controller
  at 23:08:28. The loader registered for notifications at 23:08:32.578. The game had only Keychron Link open,
  although DirectInput listed both devices. The watch showed zero events and zero checks. At 23:10:49
  `joy.rescan` opened both devices without faults and restored six joystick bindings. The owner confirmed
  physical input worked in the running game.
- The first controls update now checks for missing attached devices after registering, and retries three
  seconds later. Real notifications retain their debounce and retry. The first pass on each candidate lab
  reported one check and zero notifications. A synthetic `joy.drop` before the delayed pass reopened both
  actual controllers on that pass in both builds with zero notifications and no manual rescan. Both test
  PIDs reached Free Ride and were closed by identity. A second physical plug-in was not performed.
- Targeted x86 build and path checks passed. Installed host ECE3199A... with PDB E50D9CEF... in both labs and
  the stopped Mafia10Fixes copy. Preimages are in `build/joy-startup-20261110`. Verified the owner's slot 290
  hash FDC26BD7... after the copy. The owner game is ready for its next launch.
- The owner game had exited on its own at 23:12:59, more than two minutes after the successful rescan.
  `Game_14360_20261110_231259.dmp` records `C_actor::ResetPlacement` in `C_game::Done` during a save load.
  The dump identifies the failure site. The relationship to the rescan or to prior garage faults is unverified.

### 2026-11-10 — Garage shadow index corruption

- Mafia10Fixes PID 7084 faulted in the native software triangle rasterizer at 23:17:53, about
  0.45 s after the second garage explosion. Its input was a 631-vertex, 492-triangle shadow. The 27th
  triangle contains indices beyond the projected vertex count. The rasterizer treated data from beyond
  that buffer as screen coordinates and faulted while accessing the scanline table.
- The intact 1.0 garage shadow geometry matches the recorded index bytes around a 20-byte overwrite.
  The first 16 changed bytes start at the frame quaternion offset and decode to a unit quaternion.
  The next four start at the frame flag offset and match the engine's rotation flag update applied to
  the intact bytes. This identifies a frame rotation write onto an index allocation. Native and SDK
  code can produce that pattern, so the dump does not identify the writer.
- The exact owner checkpoint was replayed in the 1.0 lab with native Direct3D 8 and the installed
  ragdoll mod. Two native car explosions 4.5 seconds apart activated the matching shadow geometry
  without reproducing the overwrite. Player positioning was controlled and the original shooting
  sequence was not replayed. The lab's original checkpoint, wrapper and manifests were restored by hash.
- The owner also reported a car launching after being shot. There was no new recorder file, and the
  current recorder follows the occupied player car. That report remains unattributed.

### 2026-11-10 — Check frame layout before writing a pose

- Fresh native D3D8 A/B at the owner's exact garage checkpoint. The run without ragdoll survived both
  explosions. The run with ragdoll faulted after the second blast, in a different path from the owner
  rasterizer fault. See `build/garage-raster-20261110/ab-replay.txt` and the 1.0 lab's PID 17760 dump.
- A direct test of the old SDK pose writer passed it a scratch allocation with the same index and flag
  preimage as the owner's corrupted shadow. It returned success and changed exactly the quaternion and
  flag fields. The writer now verifies the shared LS3D Release slot and positive reference count before
  writing. The check applies to mod poses and the bridge's held poses. Cached frame identity still
  depends on the caller's lifetime tracking.
- With the guard, three fresh 1.0 native D3D8 replays with ragdoll each logged a rejected pose target
  after the second blast, before the artificial probe ran, and completed without a crash. The scratch
  allocation stayed intact and the player's 20 valid frame handles were accepted on each game build.
  Evidence and repeatable probes are in `build/garage-raster-20261110`.
- The parallel get-up and load checks passed 18 assertions in 1.2 and 17 in 1.0, with one 1.0 completion
  assertion sampled while the native clip was still playing. The 1.0 case passed all six assertions in
  a standalone rerun. The traffic pedestrian case was skipped on both builds because the fixture
  had no pedestrians. Both labs were closed and the temporary garage files restored by hash.
- Installed host 4004F86B... with matching PDB 4DEECD1F... in both labs and the stopped Mafia10Fixes
  copy. Verified the owner's slot 290 checksum. The original binaries are in the replay folder's
  `pose-guard-before`. The production copy has not replayed the incident with this host yet.

### 2026-11-10 — Reject out-of-range shadow indices before rasterization

- The first owner replay with that pose guard logged a rejected pose target 14 ms after the second
  garage explosion. PID 22328 then faulted at 23:42:53 in the software shadow rasterizer's scanline
  write. The minidump records triangle 474 indexing projected vertex 16256 in a 645-vertex,
  502-triangle shadow. Its position matches the shadow from the November 9 heap-validation hang.
  The corrupted bytes differ from the earlier 631-vertex incident. The dump cannot identify the
  writer or establish that the two failures share a lifetime bug.
- Both native shadow-update implementations already unlink a shadow when
  texture rasterization returns false. The host validates the 16-bit triangle indices just before
  the native update would rasterize, and uses that existing failure path for invalid geometry.
  Cached textures and native failure handling are preserved. The first rejection per run logs
  the triangle, offending index and vertex count when readable.
- The two executable pairs passed the address checks and the generated Rust tables matched.
  The final host refused a controlled out-of-range index twice per build without writing to
  the scratch object, logged one warning per process with the expected counts, preserved the
  native cached and disabled branches, and left six live shadow textures on each build.
- The 1.0 lab survived two native D3D8 garage replays with ragdoll and the index guard. The second
  used 7.2 seconds between native explosions and moved the player towards the latest shadow
  position. Neither replay naturally produced a rejected raster index. They did not reproduce
  the owner's exact route and shooting sequence. The latest replay still rejected a pose write.
- Final range review removed a vertex-count cap that the native caller does not impose and kept
  count/span arithmetic overflow checks. Both lab builds passed the controlled probes again.
- Installed host 3CC7C9AB... and PDB 00A067BB... in both labs and the stopped Mafia10Fixes copy.
  Verified the owner checkpoint hash and restored the temporary 1.0 lab files by hash.
  Preimages and the probes are in `build/garage-raster-20261110`. The source of the remaining
  corruption is open. An index rejection during normal play can identify the affected geometry
  for a write watch in a subsequent reproduction.

### 2026-11-11 — External wheel support for native car usability

- The owner reports mission failure after driving onto physical bodies. The port delivers Jolt's
  car reaction to native motion but omitted its wheel support from `ISCARUSABLE`. The native
  `C_car::IsCarUsable` checks damage and detached wheels, at least three supporting wheels at
  low speed, and support in both index-parity groups. Both builds' implementations and AL-only
  boolean returns were read and added to the build witnesses.
- Added optional owner-scoped `vehicle_support_hook`. It augments the wheel on-ground bits only
  while the original gameplay predicate runs and restores them on return or unwind. Provider
  fault and unload fall back to native support. The native suspension retains its own state.
  C and C++ callers compiled, the Rust bindings regenerated and its safe wrapper checked for x86.
- The port tags each wheel hull with its actual native wheel index before compound cooking.
  Dynamic body contacts below a wheel, normal from the car with y < -0.5, supply support. Side
  and sensor contacts do not. Support is collected independently of the diagnostic contact cap,
  cleared each physics step/world reset, and omitted when car reactions are disabled.
- The initial test reader used an int return for a native bool. A rollover exposed the error
  because false leaves upper EAX bytes unspecified. Corrected both native bindings to bool and
  made raw native-call tests inspect AL. A regression sets upper wheel flag bits and checks the
  boolean reader. Earlier whole-register observations are superseded by the corrected runs.
- The final host passed 15 targeted live checks per build, including side/count requirements,
  detached wheels, exact flag restoration, unregister, owner reload/unload and a deliberately
  faulted provider. Both versions passed the physical-body fixture with a single-frame native
  snapshot showing road-contact mask 0 and usable=true from real external support. Support
  disappeared after leaving the bodies. Native angular motion produced a genuine inverted car
  without external support, and its native query returned false. The story's exact driving
  sequence has not been replayed. The repeated fixture is in `build/wheel-support-20261111/repro.py`.
- Address checks for both builds, generated C/Rust tables, generated method help and Rust sys
  checks passed. Preserved unrelated dirty work. Source and runtime preimages are in the same
  build folder. Both laboratory games were closed after verification.
- Installed host B0845893... / PDB A825E76A... and port E2B4012C... / PDB 1E8F3EBF... in the owner
  copy. All 184 save-file hashes matched during installation. Saved native point
  `before_wheel_support_20261111` before ending PID 15704. Its close request timed out after 10 s,
  so the tool ended that process intentionally.
- Windows refused the first restart with 740. The executable has the user's RUNASADMIN compatibility
  setting. A process-local RunAsInvoker launch started PID 10768 with ordinary rights in a window.
  Loaded the existing garage checkpoint through native menus, then loaded the saved point successfully.
  Verified mission mise12-garage, profile 1, health 400, the active ragdoll support provider and zero
  provider faults. Native save files still match the pre-install hashes. The profile's MML stats sidecar
  changed after the new run. Its Windows compatibility setting was not edited.

### 2026-11-11 — Pedestrian position at car-to-ragdoll handover

- Preserved the owner's PID 25528 state, four recent pedestrian hits and the vehicle trace before
  changing the mod. Their initial pelvis XZ matches the recorded hit root; no sample of the visible
  skeleton immediately before those hits exists. Do not attribute that specific episode from these logs.
- Found the port's old dodge fix unconditionally restoring temporary positions after native car sweep,
  including targets whose hit handler had committed the animated position. Restoration now checks
  model identity and an unchanged temporary value, skips committed hits, and uses per-call vectors
  with nested sweep scopes. Native changes survive; cleanup runs on unwind.
- A same-command SDK fixture changes the visible pelvis by 1.5 m, then invokes native vehicle contact.
  In 1.0 the old initialization selected the last tick's cached pose and measured a 1.500 m gap.
  The port now captures immediately before native contact reaction and validates all skeleton frame
  identities against a fresh snapshot before using it. Model/skeleton replacement discards old motion.
  Continuous same-clip history still provides relative limb motion; native locomotion supplies bulk speed.
- Added tests/game/test_pedestrian_handover.py and kit.pedestrian_handover. Final fixture passed four
  checks per build; ten existing get-up/contact checks per build also passed, with real traffic enabled.
  Offline position restoration and pedestrian motion regressions passed. Initial distant native contacts
  returned success without a hit (native deactivation at 20 m from the camera); those are not passes.
  The fixture explicitly seats the player and moves the car/camera near its target. An intermediate
  reused 1.0 run with an unseated driver failed fixture setup; the corrected setup passes both labs.
- Port diagnostics retain 16 transitions in ragdoll.state.car_handovers: visible pre-hit and initial
  pelvis, maximum skeleton gap, sweep commitment and first solver step. This distinguishes a pose
  rollback from later collision displacement. The exact original owner incident is still unclassified.
- Built port SHA256 5A0DA231821F0C45E98C54728401DEABBE795B2881003F31904350418ED3CCDA
  (PDB E5FA51EB7731BF42C1A1DBDF84FC2ECCE02761B836707ED685ABDF484141535B), installed in both
  labs and hot-reloaded in Mafia10Fixes PID 25528 generation 2. All 206 native save-file hashes matched
  before/after. The host binary and settings were not changed. Preimages, owner evidence and install
  receipt are in build/pedestrian-handover-20261111. Lab river_water was unloaded during this fixture
  after a slow frame/command timeout; this is not evidence that ragdoll caused the delay.

### 2026-11-11 — Client cursor return, executing bridge timeout and follow-up body evidence

- The owner reported a persistent Windows cursor over the game's own after focus return. A controlled
  active 1.0 client request with visible ShowCursor count retained a system cursor 30/30 times. The host
  now selects no system cursor in the active primary client and halts class-cursor default processing.
  Focus return and ordinary frames also reconcile the actual pointer location, covering stationary
  returns. Other windows, resize borders and modal loops retain their cursor policy; no client counter
  decrements accumulate. Added window.state cursor_shape/client-hide count and protocol help.
- Both builds passed 23 targeted checks: foreground and background requests, eight border shapes and
  visibility, balanced display counts, stationary frame repair, and unchanged input during fixtures.
  One test originally rejected the render child as outside the root; corrected the fixture and retained
  both reports. A synthetic focus-message experiment warped the pointer through native handlers, so
  the final selection fixture omits those messages. The owner activated one lab normally after Windows
  refused programmatic activation; subsequent focused tests restored the owner window.
- Lab PID 23212 fell after a slow bridge command timed out. Its stack ends at string cleanup in
  ExecCommand; source inspection found a definite lifetime race: Handle queued a pointer to a stack Job,
  while BridgeFrame popped it and unlocked before executing. Timeout removed only queued pointers and
  destroyed an executing job's storage. Shared ownership now retains that job through completion.
  Queued expired jobs are still removed. This does not establish the cause of all earlier owner crashes.
- tests/test_bridge_timeout.py passed 12 executing timeouts with long request/reply storage and 12
  cancelled queued jobs, with exact execution counts and a clean process exit. Its exercise is now also
  called by the full offline runner. No timeout error is interpreted as proof an executing action stopped.
- One broad offline run failed three checks: vb.selftest, demo-name licence names and main's walked call
  through the harness_add chain. No clean baseline was captured; the full suite is not claimed green.
  Production cursor/bridge code passed its targeted checks. Source whitespace checks passed.
- The owner then reported one unnatural pedestrian fall and one teleport. Retained contacts 8/9 had zero
  initial pose gap and first solver movements 0.0414/0.0017 m. The second car trace covers 3.5 s, including
  the native hit at 8.6 m/s; it does not record the pedestrian's subsequent displayed pose. Neither report
  is explained by those initial values. Extended the port's bounded trace through tick/render-begin pose
  application for 3.5 s, read separately with ragdoll.handover. Seven contact/render checks passed per
  build. The fixture now probes the actual native distance gate: byte +0x50 bypasses its 20 m test, read
  in both native implementations. The byte's broader meaning is not assigned from that branch alone.
- Audio stutter remains unclassified. Preserved 600-frame statistics (median 16.054, p95 35.287, maximum
  84.196 ms) and callback cost about 0.361 ms/frame. Test GPU load may have contributed; no audio timing
  or output was captured. Both labs were closed and their temporary water manifests restored bytewise.
- Installed loader FC5257CB... / PDB 6D9D1014... and port 3542AF0C... / PDB 7E294708... in both labs and
  Mafia10Fixes. Saved before_cursor_bridge_20261111 in mise15-mesto/profile 1. PID 25528 did not close
  within 10 s and was intentionally ended. In-place ASI overwrite was refused; verified its preimage and
  used the documented delete/rename deployment pattern. Binary replacement left native save hashes
  unchanged at the installation check. Process-local RunAsInvoker retained the user's Windows setting.
- New owner PID 26876 runs in its original 1280x960 client with only ragdoll loaded. MCP launch timed out
  despite host start.state=done because it waits for core's absent player command (source verified).
  Host restore loaded the saved mission in 2125 ms; optional MCP player was null. Verified profile 1,
  health 362.622/max 400, new cursor fields and ragdoll.handover registration. The two failed installer
  receipts and successful owner verification are preserved rather than rewritten. Cursor/crash evidence,
  backups and receipts: build/cursor-return-20261111; body evidence: build/pedestrian-handover-20261111.

### 2026-11-12 — Separate pedestrian initialization, world matrices and car contacts

- Owner PID 21276, Mafia10Fixes, reported several pedestrians struck at the right car corner appearing
  at the left. Five retained transitions had zero initial pose gaps; their traces did not cover the
  preceding world render. The attacker was the Bolt-Thrower, Models/thunderbird00.4ds.
- Added a separate post-world matrix snapshot for nearby tracked pedestrians and stage 5 samples for
  new bodies. Same-model/frame identity and two-tick freshness gates prevent reuse across replacement.
  These are matrices after world rendering, not proof the actor was drawn or its skin pixels matched.
  Corrected the fixture's wording: its 1.5 m same-command bone edit is not displayed before the hit.
- Observer E4CA19BB... was hot-reloaded into the owner game as generation 2. The owner confirmed another
  occurrence and paused. Saved six transitions: previous-world/pre-hit and initial pose gaps all zero,
  post-world pelvis follows physics. The last body starts accelerating around age 0.048 s; it moves
  left about 0.73 m over the first 0.528 s. The data does not yet explain the perceived transfer.
- Added bounded, per-body car-contact diagnostics to ragdoll.handover: normal, depth, part/body id,
  sensor state, callback velocity and actual Jolt car axes/origin. Contact loss is explicit at both
  the global step limit and per-transition limit. Paused steps clear stale contact diagnostics.
  No collision shapes, impulses or physical response policy were changed by these observers.
- Built port 2F791D64AA4536F6B0F776C72DD79B65F014DB6ABF563B4510BF4DF1FC2714FC,
  PDB 4B161450391554DFCCB544A6A9A69EA9E746C1317A1BFAC940C989E6C7B5B9A0.
  Installed in both labs and owner PID 21276 generation 3, with hash checks and binary preimages.
  Owner settings, loader and mission were not replaced. Another owner recurrence is requested.
- The nine handover/render checks pass in each build. The first 1.0 run returned accepted=false and
  no transition despite a camera-eligible probe; it failed, and is not treated as a physics regression
  pass. A subsequent 1.0 run passed all nine. Native Bolt-Thrower corner runs recorded actual solver
  contacts in both builds; no exact owner transfer was reproduced. One crowded 1.2 trace records global
  contact loss explicitly. Missed targets and native deactivations are not successful reproductions.
- Evidence, complete owner transitions, native corner traces, backups and installation receipts:
  build/pedestrian-teleport-20261112. The intermittent bug remains open.
- Closed test PIDs 20388 (1.0) and 7120 (1.2). Both temporarily disabled lab river_water manifests
  were restored bytewise (SHA256 45F98C11...). Default lab selection was not changed. The owner game
  was not closed. Contacts from both actual car sides have outward lateral normals; the requested
  approach offset alone is not a reliable side label because pedestrians and cars move before contact.

### 2026-11-12 — Native sound restart gain repair and explicit window activation

- Read the owner-supplied Medmis handoff. Revalidated Mafia10Fixes PID 27376, neutral, full throttle,
  about 4800 rpm, paused; only ragdoll loaded. No core mod was added. The first transition was tested
  in the background and is invalid as an audible reproduction; the owner correctly challenged it.
- Added actual foreground guards and endpoint loopback capture. The owner stopped Edge playback.
  SoundKeeper64 remains as a known background source; paused samples establish the floor. This Windows
  build is 19045, so recordings are endpoint mixes, not isolated process loopback. A later user sound
  settings change made the difference clear: about -33 versus -24 dBFS after throttle reapplication;
  another run about -41 versus -24. Equal rpm and backend reported volume do not imply equal output.
- All 16 populated native car sound pointers were queried. Three redline loops remain logically and
  actually playing with unchanged frequency, requested/backend volume and spatial parameters. A same-value
  SetVolume does not repair the audible gain. A 0.01 dB downward transition followed by the exact original
  attenuation does, without releasing throttle. Individual no-seek voice restarts reproduce attenuation
  loss. This supports a narrow playback-gain compatibility repair; Windows internal cause remains unproven.
- Implemented a guarded paired ISND_sound::Play hook (1.0 ls3d+786E0 / 1.2 +7CAA0), process-only
  sound.compat diagnostic toggle/counters, bounded gain helper and offline failure/silence regression.
  New address byte/ABI witnesses pass both files; generated Rust tables and protocol methods are current.
  The helper test passes. Final loader A86E5049... is installed in both labs and Mafia10Fixes.
  1.0 foreground A/B: disabled loses 8.12 dB relative to post-throttle; enabled differs by 0.59 dB
  at 4800 rpm. 1.2 enabled differs by 0.61 dB; its off run has a smaller change, not the strong 1.0 loss.
- The alleged Windows focus refusal was our own background-launch interception: hkSetForeground returns
  TRUE for this process's windows without forwarding. Native calls therefore reported success without
  activation. External Win32 activation of the same HWND succeeds, verified after message processing.
  Added explicit window.focus through the original native entry, preserving ordinary background launch
  guards and reporting real foreground HWND/PID separately from the API return. Minimized windows restore.
  No synthetic desktop input or global foreground-policy changes. The owner's challenge corrected the
  tool diagnosis; this is not a requirement for a human click. Five focused assertions pass in each
  real build, including minimized-window restoration and retention of the ordinary launch guard.
- Saved full native restore engine_gain_before_update_20261112 and backed up the owner's savegame
  and old binaries before installation. PID 27376 did not exit in the normal 10-second window;
  game_close terminated it. The immediate binary unlink was briefly denied; bounded retry installed
  hash-verified ASI/PDB. New PID 19440 restored profile 1, mise16-mesto, health 300/300, only ragdoll.
  Restore from the main menu is refused; loading a native mission first then the restore succeeded.
- Final owner-fixed.wav: -39.69 dBFS post-pause versus -40.56 post-throttle at identical 4800 rpm,
  with zero repair failures. The game is paused. Unsaved audio settings reverted at restart; restored
  captured master 0.1611328274 and music 0 through native controls. In 1.0 menu.value needs the matching
  menu.command id to commit an options change. Other slider preferences await the owner's reply.
  Both labs are closed, river_water manifests restored bytewise (45F98C11...), default lab unchanged.
- Backups, unmodified and edited-setting audio, full state/time records and compatibility experiments:
  build/engine-pause-20261112. Mechanism/limits: knowledge/notes/audio.md. Targeted helper, generated
  protocol/Rust freshness and paired ABI checks pass; no claim that the full offline suite is green.

### 2026-11-12 — Owner rejects engine audio workaround; causal investigation reopened

- Owner still hears a timbre change after pause. The previous conclusion based on aggregate RMS was
  premature: a small total-level difference cannot validate spectral or layer equivalence. The native
  gain-transition hook is a workaround, not an accepted repair; disabled in PID 19440 for diagnosis.
- Captured new native audio options exactly: sounds 1, music 0, cars 1, dialogue 0. No further audio
  setting guessing. Investigate native restoration sequence and individual rendered layers.

### 2026-11-12 — Isolated deferred-buffer gain/rate rollback; replaced the workaround

- character-on2 proves the owner's timbre report: near-equal total RMS conceals a dominant line at
  93.75 Hz post-pause versus 161.13 after throttle, with high-band differences around 7 dB. All
  reported native rates remain equal. Native buffer PCM matches the game's source assets exactly.
- A separate x86 DirectSound sine test reproduces the fault without Mafia: LOCDEFER playback updated
  from 16000/-1500 to 30869/-411 returns to actual old rate/gain after Stop/Play, despite correct
  getters. Volume transition alone restores only gain. Seeking before Play does not prevent it.
  Software allocation at creation preserves actual gain and rate across all starts/seeks.
- Removed the ISND Play gain hook, sound_gain helper and its insufficient fake-backend test. Paired
  driver Init hook selects native ISNDINIT_NOHWCHANNELS (0x20) when hardware 3D capacity is zero and
  no EAX provider exists; hardware/EAX/query-failure paths keep native policy. No per-Play changes.
  First capability gate also required zero generic mixing slots and did not activate: Windows
  reports 1 generic slot but 0 hardware 3D slots. Corrected the gate to the relevant 3D capacity.
- Final ASI FF4E79EC... built and installed in both labs and owner. Five actual-buffer assertions
  per build pass (caps 0xB8, spatial controls retained, no LOCDEFER), both foreground lab captures,
  standalone acoustic checks, paired Init ABI and generated table checks pass. New standalone
  regression sources/recording/analyzer in tests/dsound; game check test_sound_allocation.py.
- Backed up complete savegame/binaries before owner update and saved full native restore
  engine_allocation_before_update_20261112. PID 19440 closed normally; PID 10752 restored profile 1,
  mise16-mesto, health 300/300, only ragdoll. Exact native audio options 1/0/1/0 captured and restored.
- Owner long recording: -16.060/-15.992/-16.046 dBFS at 4800 rpm, dominant line 161.13 Hz in all stages;
  post-pause/post-throttle 80 Hz..20 kHz bands within 0.09 dB. Lowest 20..80 Hz band differs 0.61 dB;
  approximate PCM fits infer correct actual rates but leave 18..22% RMS residual. Do not claim every
  waveform/band is identical or exact isolated gains. Owner listening check of the new build pending.
- Foreground handoff between labs can require the foreground game to AllowSetForegroundWindow for
  the target PID; explicit window.focus then succeeds. Used only scoped grants, no real input or
  global policy changes. Invalid focus-loss captures were retained, not counted as audio results.
- Both labs closed; original river_water manifests restored (45F98C11...), default lab unchanged.
  Owner paused. Evidence/limits and retired investigation: knowledge/notes/audio.md and
  build/engine-pause-20261112. Separate stutter report remains open; no full offline-suite green claim.
- Final background check on the owner replacement: actual foreground loss yields -120.15 dBFS
  versus -16.42 active and -16.09 restored. No background game sound. SW_SHOWMINNOACTIVE initially
  left the game as GetForegroundWindow even while minimized; that invalid fixture was discarded.
  Short level-preserving listening clips engine-old-check.wav and engine-native-check.wav contain
  pre-pause, post-pause and post-throttle stages. Owner remains paused, health 300/300, only ragdoll.
- Owner recognized Setup's Software mixing only option. Verified its byte +0x20 is converted to the
  same native 0x20 driver flag in both InitSystem disassemblies (1.0 5FA7CF, 1.2 5BFA7F). On this
  configuration the host selects that existing mode automatically, not a new mixer or an internal
  DirectSound repair. Setup sets it before Init (also skipping EAX discovery); the host gates it
  after Init and preserves detected EAX providers. Old voices already reported software placement.
  CPU/latency deltas and hardware/EAX wrappers remain unmeasured; documented the distinction/tradeoffs.
- Owner clarified the requested listening comparison is normal-game feature benefit, not the pause
  fault. The engine clips do not answer it. Verified LS3D's EAX3 sector-environment path (1.2 +4AAA0)
  and pre-change PID 27376 driver data: flags 0x12 already include NOEAX; both EAX interfaces null.
  Current 0x32 adds NOHW only. No active EAX was removed here, but room reflections/reverb/decay
  are potentially valuable features. Actual EAX-on/off quality A/B remains unperformed; DSOAL is
  only a candidate, not installed. No game/audio configuration change for this inquiry.
- Owner enabled EAX and software-only in Setup and relaunched: PID 4264, native settings confirm
  disable_eax=false/software_mixing=true. Actual flags 0x22; host changed=false. Native flag 0x20
  skips InitEAX, explaining absent effects with this combination. Both EAX interfaces null;
  in-process modules shows Windows DSOUND, no loaded EAX wrapper. Read-only diagnosis; game untouched.

## 2026-11-13 — Owner's game launched with an initialized EAX3 provider

- Explicit owner request: start Mafia with EAX. Installed game-local x86 DSOAL, built from upstream
  commit e11128250439d586483ee6598d1670ab52ab5e37, and official OpenAL Soft 1.25.2 Win32 binary
  as dsoal-aldrv.dll in Mafia10Fixes only. Upstream license notices copied to licenses/eax.
  No host/mod binary change, system DLL replacement or COM registration. Both labs remain closed.
- Native Setup value is HKLM32, not the existing HKCU VirtualStore value. Initial normal write was
  denied and ordinary game_launch returned error 740 (Game.exe's RUNASADMIN compatibility flag).
  RunAsInvoker test PID 27464 loaded DSOAL but still read software-only=true from HKLM; it was closed.
  The unused VirtualStore edit was restored exactly. Owner explicitly permitted elevation;
  an elevated helper backed up the 63-byte native value and changed only byte +0x20 from 1 to 0.
  EAX was already allowed. No permanent compatibility-layer edit.
- Final PID 24008: native flags 0x300002 (EAX and EAX3 enabled, NOHW clear), both native EAX property
  interfaces non-null, DSOAL exposes 128 hardware-compatible 3D/mixing slots. sound.compat.changed=false:
  the loader did not force software allocation. Both provider modules resolve to Mafia10Fixes.
- Evidence, archive/DLL hashes, exact settings before/after and rollback preimage:
  build/eax-20261113. EAX3 initialization is verified; normal-game indoor/outdoor acoustic A/B and
  pause/stutter regression with this provider remain unmeasured. Game is at profile 1's main menu;
  explicit activation still reports another foreground process, so do not claim focus was obtained.

## 2026-11-13 — Sound-dependent gameplay and EAX speedrun audit (1.0)

- EngineSnd 0x538220 really gates engine-on on logical sound state and backend cursor, with a
  300-ms end margin. Camera-distance sound disabling is a separate path from volume zero; verified
  ordinary/siren thresholds 60/110 m in C_car::Update. Owner reports existing Setup-sound-off and
  distant-camera skips; no new naturally executable instant-start strategy established.
- Foreground lab: three starts each with normal/Cars=0/master=0 on Windows DS and DSOAL+EAX3,
  all about 3.06..3.21 s with sounds_enabled=1. No zero-volume instant start. Matched silent
  background DSOAL tests with EAX extensions on/off: medians 3119/3120 ms, three trials each.
  Different mechanisms, limited sample/frame scheduling: no general timing-equivalence claim.
- Reproduced native wrong-cursor channel eviction silently in the background: victim 5732 bytes,
  incoming 3528, victim queue stores 3528. Also verified a byte/ms queue-position mismatch in 1.0
  disassembly; useful gameplay consequence remains unproven. These bugs were investigated, not fixed.
  Dialogue volume <=~0.01 selects a morph/delta-time fallback; mission-level impact untested.
- Initial native fixture used the wrong overloaded Play vtable slot and caught a fault; discarded,
  ABI checked, fresh lab retry succeeded. Early focus fixtures were aborted when actual activation
  failed. Computer-use activation worked once, then physical Escape stopped it. Owner explicitly
  requests no desktop/window control going forward; subsequent fixtures ran entirely in background.
- Both labs closed and temporary provider/config removed from 1.0 lab into ignored evidence folders.
  Owner PID 5988 remains running with EAX3, no restart. No host/mod changes. Findings, limits and
  reproduction receipts: knowledge/notes/audio-logic.md; build/eax-20261113.

## 2026-11-14 — Owner's long hang on the next mission segment

- PID 21004, Mafia10Fixes 1.0: Windows events 1001/1002 at 11:33:58 Moscow report AppHangB1,
  cross-thread. Owner waited through a long loading hang and accepted Windows' offer to end it.
  No fresh exception dump. crash_triage's default November 10 result is unrelated/stale.
- Archived fresh WER report and logs/traces with hash checks in build/hang-20261114-113358.
  Last loaded scene mise18-mesto, no next-scene open line. Cleanup is a candidate, not a located
  failure. EAX/host hashes unchanged; EAX3 and ragdoll present, RTSS not listed. No cause or fix claimed.
- Added an ignored, temporary external hang recorder: exact owner path/PID validation, read-only
  hung-window checks, three snapshots/bridge where replies, no activation/input/game termination.
  Native dump self-check passes; background x86 lab dump parses with 42 threads and where replies.
  Actual-hang trigger/capture still untested. Hidden recorder PID 14696 watches for four hours.
- Backed up the owner's complete saves before restarting profile 1/save 495 in background. PID 9596
  paused, EAX3 0x300002 retained, recorder confirms dump_access=true. No host/mod/config changes;
  lab closed. Owner correctly notes a negative rare-event replay proves nothing: no route replay
  requested. Restart was readiness verification, not a reproduction or a successful-fix test.
- Evidence, uncertainties, recorder limits and state: knowledge/notes/hang-20261114.md.

## 2026-11-14 — Seconds-long native Ogg cancellation reproducer

- Owner rejects waiting/playing four hours. Negative tests (100 muted eight-stream stop/reopen
  cycles, 12 alternating different-scene save loads) remain negative evidence only.
- Controlled scheduling reproduces a native deadlock: verified decoder TID 24764 of test PID 9596
  held rw_data's shared critical section; normal IShow::Close terminated it, leaving the lock orphaned;
  main thread blocked in ov_clear/close_func/dtaClose. Three dumps/where replies captured. Closed only
  that deliberately hung owner test process. This proves the native mechanism, not PID 21004's cause.
- Read native worker/refill/Close in both LS3DF builds. Added cooperative stop and thread join in
  host/ogg.cpp, retaining native refill, DirectSound backend, and per-build EOF notification counts.
  Native Close no longer terminates that live reader; decoder cleanup runs after its normal return.
  Thread/notify/stop handles have explicit ownership; early Close shares the same stop state.
- Backed up affected source/docs and all three installs' ASI/PDB before changes. Added SDK addresses,
  ABI/layout witnesses, regenerated Rust/method tables and read-only sound.compat.ogg_shutdown counters.
  Built host; check_builds and generators pass. The first build lacked mmsystem.h before dsound.h;
  corrected include order, rebuild succeeds.
- Both real-game labs pass adversarial archive-lock close, 32 immediate closes and existing native
  sound allocation checks: 12 checks/build, zero failed (game-tests-2.log). Initial run's final
  diagnostic query hit a pre-existing MCP timeout-metadata/strict-read-only mismatch; the lock/exit
  assertions already passed, test query corrected, mismatch recorded in roadmap.
- Further verification: 100 eight-stream cycles and six native restore loads per lab, zero Ogg
  failures. Corrected stress fixture that requested restore while paused (deferred native loop,
  tool timeout, not a game hang). Full offline suite has THREE failures; a control build without
  ogg.cpp and with pre-change fixes.cpp reproduces the same three (native-index revision self-test,
  existing demo-only identifiers, crash report call-target traversal of SIB thunk). No new failure
  observed; whole suite remains non-green and gaps are recorded separately in roadmap.
- Restored candidate source bytewise, rebuilt final host, repeated the exact adversarial/32-close/
  buffer-policy checks on the final binary in BOTH labs: 12 passed/build, zero failed. Labs closed.
  Hash-verified only ASI/PDB installation in both labs and owner: host BB4B59FDE80592B410CD18E43AF5A98D3CD4F8886A1EE793033BAD4D4CDB4BD7,
  PDB D9190B6AA04A19B40F6FB57547C3A7451098276DD5ADACE56371F2E60E3EAF48.
- Owner PID 5332 with DSOAL/EAX 0x300002: controlled archive-lock close and 32 immediate closes pass,
  then 100 eight-stream cycles; later repeated fixture passes too. Separate native state-driven
  fixture completes twelve alternating save loads 490/495, ends paused at profile 1/495. Final Ogg
  counters 297 started / 296 exited and joined / 1 current state / 0 failures. Provider, game binary,
  mod DLLs/manifests and permanent audio/registry settings unchanged.
- Initial owner launch timeout was the MCP completion check's dependency on disabled core.player,
  despite native start.state=done/live player. Load fixture retries corrected early save-list/player
  assertions to await native completion; failed receipts retained. These were tooling/fixture errors,
  not evidence of another game crash. No desktop activation/input and no four-hour replay requested.
- Evidence/implementation/limits: knowledge/notes/hang-20261114.md; build/ogg-shutdown-fix-20261114.
- Final state verified: owner PID 5332 paused at profile 1/495, saves_written=0, EAX retained,
  zero Ogg failures. Stopped only the verified temporary recorder PID 14696; archived captures kept.
- Follow-up diagnostics are active for the current owner session. Recorder PID 1276 verified
  the exact game image for PID 5332 and reports dump access. Session mode exits with that game
  process. A disposable-process test verified exit detection. No new crash or hang report at check time.

### 2026-11-14 — Missing ragdoll on a traffic pedestrian with a marked model name

- Owner PID 5332, 14:26:20 local time: three type-5 contacts on the same `civil31` pedestrian
  within 96 ms, the third lethal. All three handover records have `started=false`, and the
  log records repeated rest-skeleton lookup failures for `Models\civil31.I3d!`, including
  the attempt to adopt the dead body. The game's death animation was still active when queried.
- Recorded element coordinates match before and after each native hit at trace precision.
  The later element is 6.19 m from the last hit. No samples cover that interval because
  ragdoll creation failed, so the timing and cause of the reported visual jump remain open.
- Both native hierarchy loaders replace the model suffix with `.4ds`. The SDK converter now
  also recognizes the observed `.i3d!` alias case-insensitively. Raw `model.file` naming is
  preserved. Both labs parse the same nonempty rest skeleton through either name. In a separate
  check both build a physical ragdoll on a spawned `civil31` bearing the exact marked name.
- Lab ASI/PDB preimages and incident receipt are in build/pedestrian-no-ragdoll-20261114.
  Both labs closed after testing. The user's running copy still has its previous host build.

### 2026-11-14 — Second marked-model hit and independent trajectory capture

- Owner's screenshot at the 14:39:12 contact matched a second `civil31.I3d!` skeleton failure.
  No body started, all three immediate hit pre/post element positions match at trace precision,
  and the later position is 6.86 m from the lethal hit. There are no samples inside that interval
  for this incident. The capture is in build/pedestrian-trajectory-20261114/owner-incident.json.
- Installed the verified model alias fix and instrumented ragdoll. Both labs passed native contact
  with and without a body, using an injected 1.5 m pelvis offset to verify where the observer first
  sees it. All four captures reached disk with zero dropped samples. Both labs closed after tests.
- Backed up owner ASI/PDB, ragdoll DLL/PDB and profile/slot-500 files, then closed the exact PID 5332.
  Restarted only the configured owner copy in the background. Verified PID 17684 at save 500 on pause,
  the new trajectory command and 23 nodes through the marked model name. Full EAX flags 0x300002.
  The session hang recorder followed the old process to exit and was rearmed for PID 17684 (PID 22096,
  dump access confirmed). Owner did not need to replay either incident for the test.

### 2026-11-14 — Bumper camera removed the ragdoll's car geometry

- Owner clarified that the new behavior was bodies passing through the car with similar scripted
  pushes. Current host/mod DLLs were verified. In PID 17684 the player TruckBx00 root flags were
  0x2000003C (hidden) and actor.meshes returned zero pieces. The last pedestrian handover had
  zero car contacts. With no physical car, OnHit uses its existing velocity/damage fallback impulse.
  This explains the camera-dependent missing collision and the uniform shove.
- Added appended SDK actor_meshes_ex with MML_ACTOR_MESH_HIDDEN_ROOT, plus Rust binding/safe method
  and bridge actor.meshes hidden_root option. Original visibility selection remains the default.
  Only the actor-model root may be bypassed. Hidden children and nested models remain excluded.
  The ragdoll uses this explicit selection when building cars from their loaded native visual meshes.
  Geometry remains per-visual convex hulls built from native finest-LOD triangles, with wheel mapping.
- Both labs pass four C ABI visibility/unknown-flag/restoration checks. Separate bumper-camera
  fixtures start the physics world with the root already hidden, acquire TruckBx00's 1375 triangles,
  then drive the actual truck into an existing physical corpse. Measured car contacts, >1 m body
  displacement and >7 m/s peak limb speed in each build. No living-target hit supplies the impulse.
  Camera return preserves physical geometry. Rust all-feature check and bindgen/method checks pass.
- Initial kit fixture omitted game::Detect and faulted safely in both labs. Corrected before the
  passing run. First collision fixture was confounded by an overlapping earlier truck and native
  actor placement in a cabin, then by a time-separated visible-part count. Replaced with isolated
  restore/drive fixture. All failed receipts are retained. Full offline suite still has the same
  three baseline failures (vb native revision, old demo identifiers, SIB-thunk crash call-target test).
- Owner's new movement trace also records a 5.202 m native model-root rewrite while physical pelvis
  moves 0.095 m, then the pose application returns the model. The native animation trajectory is a
  separate investigation. That internal transient alone does not establish a visible rendered teleport.
- Saved complete native restore point before_hidden_root_20261114 (147039 bytes, mise18-pristav),
  backed up binaries/config/slot files, and updated only owner ASI/PDB and ragdoll DLL/PDB. Host
  1BEB2DD9A4F19623ACE6D48F639E65A885A5E2DF9F685443A6E27BB452E11421, port
  97B7E11E8EB7ED5E4C3A5CB54FC1DD67633E940766B5C7DBF67DC3A6297C56B4.
- Restore validation initially asserted against the earlier moving-car snapshot and left the game
  running after that assertion. Restored the native point again while simulation time was held.
  The owner saw this intentional time hold as a possible hang. Process/bridge remained responsive.
  Returned ordinary time scale 1 and native menu pause. Health matches before, saved-point position
  differs 1.95 m from the pre-save snapshot taken before briefly resuming the moving car.
- Final owner PID 19940, camera 12, visible meshes zero, physical meshes 20 / 1379 triangles and
  registered dynamic 3000 kg car. Full EAX 0x300002, health 370.659, paused in gamemenu. Both labs
  closed. Session recorder PID 25872 confirmed dump access for PID 19940. All receipts/preimages in
  build/vehicle-hidden-root-20261114. No desktop activation or real keyboard/mouse used.

### 2026-11-14 — EAX authoring versus provider behavior

- Owner reports the ragdoll currently works and resumes EAX investigation: indoor rumble building
  up and similar outdoor echoes. Read native environment updaters in both builds (LS3D 1.0 RVA
  0x48600 / 1.2 0x4AAA0), their camera-sector selection and pinned DSOAL property forwarding.
  Native sector sound properties are blended into custom 112-byte EAX3 packets. No host reverb DSP.
- Verified 1.0 listener getter ABI in assembly and read the active provider twice in PID 19940.
  Primary sector has 1 s decay, 300 ms early delay, 100 ms additional late delay, diffusion 0.4,
  EchoDepth 0. The native sector contains the same delays. EchoDepth zero does not disable reverb.
  Provider hashes match the original pinned installation; sound.compat allocation fallback inactive.
- Documented controls, camera dependence, historical-reference limits and the missing affected-room
  audio/parameter/source capture in knowledge/notes/eax-environments.md and ROADMAP. No cause is
  assigned to the rumble. No game state/settings, binaries or desktop focus changed; labs stay closed.

### 2026-11-14 — Source-driven artistic Soundscape

- Owner requested artistic sound waves, echoes and room visualization. Added an optional independent
  soundscape mod with cyan source waves, amber/violet EAX decay trails, a static-collision boundary
  probe and acoustic card. Existing SDK drawing/projection supports both labs and native Direct3D 8;
  no host or audio-provider changes. Shift+F10, Options > Mods > Soundscape.
- Verified native registered sound vector/fields/query ABI in both builds. Snapshot reads real
  engine-owned sources, including repeated onsets on an existing frame. EAX uses property Get with
  HRESULT/byte-count validation. Limits are explicit: artistic slowed overlays, configured gain,
  illustrative loop cadence, predicted EAX envelope, partial static probes; no PCM or reflection tracing.
- Fixed initial C++ vector namespace/overload errors. Initial live fixture tried to load an already
  loaded mod; corrected. Final tests pass 15 checks/build, including native source data and restart,
  draw budget, pause, hide, unload/reload and unchanged allocation policy. Preview frames inspected;
  later refinement increased ring segments. Lab 1.0 sample average cost ~0.072 ms/frame (scene-specific).
- Installed DLL 4E0C5105AFF31DA92B797EF688F40F19E0BAB5F9E55EEE61F0D934CB090DA6BF into
  Mafia10Fixes, enabled its local manifest and hot-loaded in PID 19940. It reads active EAX3 values
  matching the native getter. Full 112-byte provider block identical before/after mod load. Both labs
  closed; temporary river_water manifest edits restored to original hashes. No desktop activation.
- During development owner reported another pedestrian teleport, immediately at impact. Preserved
  current jump/live/handover data and distinguished the stale completed-contact file. Current
  subject has matching initial tracked pose and no large sampled world-draw pelvis displacement;
  native animation produces intermediate root rewrites. Cause of the visible report remains open.
  knowledge/notes/teleport-20261114-1604.md; build/pedestrian-teleport-20261114-1604.

### 2026-11-14 — Volumetric Soundscape and native draw incident capture

- Soundscape 0.2 uses native PCM levels, wall reflections, room collision triangles and a
  separate modeled EAX envelope. Fixed overlapping wave saturation in both lab previews.
  Sixteen offline and 22 live checks per build pass. Hot-loaded into owner PID 19940.
  Native D3D8 rendered the mesh and restored state. The before/after EAX packets match.
- Preserved the second teleport. Its visible cause is open. Added Draw Probe using native D3D8
  skin copy/range matching or the D3D9 scene source. Both builds detect a controlled 2.5 m
  shift, with seven checks each. An initial native path returned transforms without vertex
  matches. Reading the CPU skin source and matching the bound range resolved that in the lab.
  Automatic visible-hit capture on native 1.0 retained 116 matched pre-hit and 160 post-hit ranges.
  Enabled the updated probe for the owner's next launch.

### 2026-11-14 — Five-second wait after a rifle kill

- The owner log confirms immediate ragdoll creation followed by a five-second wait for static
  scenery in Old Prison. Reproduced in both builds. Skipped placement failures qualified as
  fresh work each scan and starved later visuals. Remember the inputs to a failed placement
  and progress through outstanding imports under their own budget.
- The final test in the same mission and at the same actor records 0.05 s in native 1.0 and
  0.04 s in wrapped 1.2, both released by ground readiness. Built host SHA256
  6D9AA29A94E00C4D669F644019487AEB2E8530B8A3FAB014BF55AEC7439ED305 installed with its
  matching PDB in the two labs and the closed owner copy. See the scenery note and local receipts.

### 2026-11-14 — Soundscape continuity in the prison corridor

- The owner screenshot identified sector chodba12. The initial mesh contained 19 drawn triangles
  despite almost every ray hitting something. Seed hits now spread over connected coplanar native
  triangles. The cutaway and crease outlines expose the room surfaces and preserve openings.
- Direct wave vertices stay on one sphere with a smooth fade at the collision distance. Reflected
  paths move their own amber pulses. The EAX tail history survives retirement of dry fronts and
  uses a separate timeline graph.
- Both labs show connected surfaces and reflected pulses in the same corridor. Fifteen seed
  triangles produce 72 clipped triangles in the fixed test view. The owner view reports 57.
  The labs have EAX inactive, and the owner has active EAX with 2.42 s decay. The full EAX packet
  matched across hot load into PID 27624. The first preview accidentally ran in freeride after the
  standard fixture changed the scene. The corridor fixture now asserts the mission before capture.
- Eighteen offline checks and 22 live checks per build pass. Local previews and rollback files
  are in build/soundscape-continuity-20261114.

### 2026-11-14 — Sustained audio during the prison pause

- Confirmed continuous output in the owner game's Windows session with game time frozen for ten
  seconds. None of 643 attached LS3D DirectSound buffers reports playing. A separate Edge session
  prevented clean endpoint loopback, so the evidence is per-process level and native buffer state.
- The corridor's EAX3 decay is 2.42 s, with a low-frequency ratio of 2 and zero explicit echo depth.
  Persistent output exceeds ordinary decay. Attribution to an untracked input or provider processing
  remains open. See knowledge/notes/eax-environments.md and build/eax-pause-20261114.

### 2026-11-14 — Confirm and fix a pedestrian dodge rewind

- The owner reported that the pause hum eventually decays and deferred audio work. The next live
  pedestrian incident established a 2.388 m rendered move before pedCarHit, while the first physical
  step moved about 3 mm. The incident uses native dodge 323 and the car-right projection is -2.357 m.
- Both native builds rewind by approximately 2.4 m when a car enters the part of the dodge path already
  walked. The paired ragdoll port starts the two root-motion collision sweeps at the last accepted root
  during dodge clips, with the original destination and response. Both labs verify obstacle retention
  ahead and the original pre-hit handover contract. See knowledge/notes/pedestrian-dodge-20261114.md.
  Local evidence and preimages are in build/pedestrian-teleport-20261114-2100.
- Port 87379006... was installed in both labs and staged in Mafia10Fixes. The existing owner process
  still has 97B7E11E... loaded with several pedestrian bodies active. The new module will load at
  the next game start. The running owner process was left in its own pause menu.
- The separate get-up smoke case passed on both builds on some runs and reported a roughly 3.9 m
  displacement on other runs. The dodge A/B and the immediate native-hit handover checks passed.
- Follow-up captured the get-up test failure in both builds. Repeated hits killed the selected
  pedestrian, leaving knockdown=false and puppet=false. The test then removed a dead ragdoll and
  compared its physical pose to the resumed native corpse pose. Restrict the fixture to a healthy
  target and require that the same model survives in a physical knockdown before calling drop.
  The final run passes 16 checks per build. Raw phase records are in build/pedestrian-getup-sequence-results.json.
- Hot-loaded port 87379006... into the owner process PID 27624, generation 2, after confirming
  no active physical bodies. The live module's SHA-256 matches the installed DLL. Owner replay
  with the new sweep remains to be observed. The earlier reports still need their own evidence.

### 2026-11-15 — Remove the artificial car-impact branch and implicit save data

- Owner PID 31972 had a native current car with usable hidden-root model geometry which world_near
  omitted. Its latest hit had a pre-physics upwards velocity and no physical car contacts. Fixed both
  nearby discovery and native-contact validation for current/last/active-traffic vehicles. The port
  now registers the striking native-mesh compound immediately and retains its reference near the body.
- Removed the car_push setting, its artificial upwards/forwards Impulse and the unused helper.
  Vehicle-impact bodies use collision forces with animation-driven muscle targets disabled. Geometry
  extraction with no valid parts retries. See knowledge/notes/car-impact-20261115.md.
- Statistics collection now starts with a mod's first valid stats_get and stops with the last consumer's
  unload. Counters remain in memory. Durable state belongs to explicit mod save_put records. Legacy
  mml/stats is readable but omitted from subsequent writes. stats.state is an observation and reports
  collecting. SDK and Rust documentation describe the new lifetime/persistence contract.
- Both labs pass 29 checks, including empty/legacy/explicit-state saves, consumer unload, detached car
  discovery/contact and actual hidden-camera truck contacts. Full offline tests still report three
  failures documented in the investigation note. The first live block exposed the contact-validation
  gap, repaired before the final run.
- Installed host 34B23E81... and ragdoll 5505A21D... in both labs and Mafia10Fixes. Thirty-one companion
  files containing only automatic statistics were backed up under the game's mml/legacy-statistics-20261114
  and removed from savegame. The owner native restore point is before_collision_only_20261114.
- Windows launch error 740 came from the owner's RUNASADMIN compatibility flag. A child-process-only
  RunAsInvoker override launched the game in background. restore.load first required a played mission,
  and slot 561 was absent from the current profile. Loaded slot 135 and restored the full saved scene,
  mise07b-saliery, in 188 ms. The game is in its native pause menu at time scale 1. The owner then ended
  the state-restoration work. Audio native flags still report 0x300002.
- Final read-only check found a subsequent owner launch, PID 27392, with the installed updated host
  and ragdoll active, statistics collection false and zero .mml files in savegame. No further scene
  changes were made after the owner's instruction to stop restoration. The test labs were closed.

### 2026-11-15 — Recheck the reported carried-player registry failure

- The owner forwarded a report that Tommy disappears from the human registry on a map change.
  This matches the previously captured 2026-11-09 failure. Current HumansMissionClosed preserves
  the constructor registry and discards scene caches only. Destruction removes the actor normally.
- Verified SHA-256 34B23E81... in both labs and Mafia10Fixes. The existing native carried-player
  blast/get-up regression passes seven checks per build. The exact actor survives CHANGE_MISSION
  once, the old NPC is destroyed, the carried skeleton resolves, and a real blast's physical knockdown
  fits a get-up and returns Tommy to native control. This report supplies no new reproducible failure
  in the installed build. No additional production edit was made for the recheck.

### 2026-11-15 — Check publication of the carried-player fix

- Queried GitHub directly. Local HEAD and remote main both point to 294814bf8f6f72437c443c2c8c430929a9c2141e,
  whose commit date is 2026-10-31. That version still clears g_humans in HumansMissionClosed.
  The carried-player fix exists in the uncommitted working tree and installed local binaries only.
- GitHub lists no releases. The loader working tree contains 242 changed/untracked paths; publication
  requires selecting and reviewing the intended changes, rather than assuming the installed build is public.
  The sibling mafia-ragdoll-mml checkout has no configured Git remote. No commit or push was performed.

### 2026-11-15 — Prepare the cumulative update and reconcile project documentation

- The owner requested the full pending update with a large agent-facing patch note, then clarified that
  all documentation must be reconciled. Inventoried 48 existing Markdown documents and backed them up
  with source/backup hash comparisons under build/publication-prep-20261115/docs-complete-before.
- Added docs/README.md (complete map/evidence scopes), STATUS.md (current contracts/limits/publication)
  and PATCHNOTES_2026-11-15.md (cumulative changes, 18 optional host suffixes and migration).
  Updated the entry instructions, architecture, protocol/modding/setup/build guides, roadmap, rendering
  plans, topic notes and dependency descriptions. Clarified dated evidence versus current state, separate
  ragdoll ownership, software allocation versus provider EAX, and unfinished water acceptance.
- Removed false current claims about absent sidecar/GPU PhysX, unported ragdoll, automatic statistics
  persistence, old water integration and a blanket both-build success. Moved resolved save/car-impact items
  to Done, removed duplicate readiness entries and linked unmatched incidents to the later specific findings.
  Historical journal outcomes remain dated; three restricted-name references use independent descriptions.
- Replaced the same identifier-only leaks in three source comments/diagnostic labels. No behavior/address/
  ABI change. Excluded sdk/rust/mml/target and its standalone library test lockfile from publication; consumer
  lockfiles remain tracked. No game assets, dumps, local symbols/config or sibling-port source were added.
- Full x86 build first failed at an ExternalProject stamp write. The initial overlapping offline attempt
  later hit the boxes reload socket timeout and is retained as invalid acceptance. Sequential x86 retry and
  final builds pass; x64 and MCP builds pass. The running MCP binary was renamed before its build as required.
  MCP smoke passes. Method/game generators, path/name checks and both-build address/ABI checks pass.
- Complete offline retry reproduced the three known failures. After the identifier cleanup the final full
  run completes with two remaining failures: native-index revision self-test and call-target walking through
  the SIB hook-chain thunk. No claim of a clean full suite; both gaps remain in STATUS/ROADMAP.
- Final host 0DFBA5501AFEB0C0FC4DEAFC18B6B83AC3954F5B50BC3C5C2EF766DE871547DB was deployed only to
  the two labs (core selected, other installed mods retained). Fresh carried-player/get-up and save ownership
  cases pass 15 assertions each in 1.2 and 1.0. Closed only test PIDs 24796/29528. The owner copy, saves,
  provider and settings were not changed; default lab/mode remain 1.2/loader.
- Local audit receipts cover all 51 project Markdown documents, their hashes and resolved local links.
  Candidate packaging uses a disposable index so new headers/shaders/tests are included in the full diff
  without staging the owner's real index. It verifies source/archive hashes and excludes ignored artifacts.
  Patch/archive/manifests are review outputs under the preparation folder. No commit, push, tag or release.

### 2026-11-15 — Clear validation blockers and authorize cumulative main publication

- Owner explicitly requested completing everything needed before publication and publishing. Verified
  the destination is PashOK7/MafiaModLoader, main, and the remote still starts at 294814bf... . GitHub
  reports this repository public; corrected the outdated private-repository statement without changing visibility.
- Reproduced the index fixture assumption independently in an x86 native D3D9 process: default dynamic
  index buffers share the installation probe's Lock/Unlock/Release entries, while managed buffers use
  different Lock/Unlock entries on this backend. The production proof gate was correctly conservative.
  The fixture now tests a required positive default class and the managed class with observation-dependent
  expectations, retaining writable/read-only/explicit/bound/unbind/release checks. Unsupported writers
  remain without a revision proof. Added native_index_profiles diagnostics and documented the contract.
- The crash/profile resolver now follows FF 24 25 absolute SIB jumps, matching the already emitted RTSS-safe
  chain thunk. The existing hooked-crash check resolves main through the chain to the actual outer detour.
  Existing relative/older absolute/trampoline handling and fault protection remain.
- Complete offline suite now finishes ALL PASSED, exit0. Full x86 build and MCP smoke pass. The unchanged
  x64/MCP source passed the earlier preparation builds. Generated method/game tables remain current;
  path, identifier and both-build native-address checks pass. All earlier failed receipts are retained.
- Host B5FD1DE0A280F86F24F75A27291B5CC59DE29CE36B9BB4C970D1E823706A2003 installed/hash-verified
  only in both labs. Final native get-up, save ownership, vehicle geometry/contact/support, model alias,
  sound allocation and adversarial/immediate Ogg-close cases pass 55 checks per build, zero failures.
  Closed only test PIDs 17576/22188. Owner copy/settings and the default lab/mode were not changed.
- Added knowledge/notes/release-validation-20261115.md and reconciled STATUS, patch notes, roadmap,
  architecture, protocol and snapshot contract. Current validation is green; unfinished water and open
  incident/fidelity work remain explicitly separate. Documentation inventory is now 52 Markdown files.
- Publication includes the reviewed cumulative source, new headers/shaders/tests, generated bindings and
  English agent handoff. Build products, game assets/saves/dumps, local configuration/symbols and the sibling
  port remain excluded. The source publication receipt is recorded after verifying the normal main push.

### 2026-11-15 — Cumulative source update published to main

- Staged exactly the 268 source paths in the hash-verified publication manifest. The complete candidate
  check included new files, generated bindings, local Markdown links and restricted-name checking.
  No build products, game files, dumps, local config/symbols or sibling-port source entered the commit.
- Commit 9d1be1ffa0b22cc66d812c606ea7a303169de208, "Publish the November cumulative host, SDK and agent update",
  includes 31,082 insertions and 955 deletions. The working tree was clean before pushing.
- Normal fast-forward push to https://github.com/PashOK7/MafiaModLoader.git main succeeded from 294814bf... .
  A direct ls-remote returned the exact published 9d1be1f... commit. Updated STATUS/patch notes/roadmap with
  this verified source identity. This follow-up changes documentation only; no binary release/tag was created.
- Runtime validation remains the passing complete offline suite, MCP smoke and 55 native checks per build
  on host B5FD1DE0... . Test labs are closed; owner game/settings and configured default are unchanged.
