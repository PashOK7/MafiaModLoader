# Architecture

```
   agent (Claude Code, Codex, ...)                          the game process (32-bit)
   ───────────────────────────────                          ───────────────────────────────────────────
   MCP (stdio)                                              Game.exe, LS3DF.dll
      │                                                       │  Ultimate ASI Loader (dinput8.dll)
   mml-mcp.exe (Rust, 64-bit) ──── TCP 127.0.0.1:47100 ────►  MafiaModLoader.asi  (the host: never unloaded)
      │  game process: launch, close (exact pid)               ├─ game hooks → events
      │  log on disk, crash dumps (minidump)                   ├─ hook chains (MinHook once per function)
      │  deploy (build → game folder → mod.reload)             ├─ bridge (NDJSON, token)
      │  knowledge base (knowledge/, MafiaReverse)              │
      │                                                         ├─ crash filter (full dumps), `where`, `tap`
      │                                                         ├─ the start (logos, profile, menus)
      │                                                         ├─ input, screenshots; for mods: drawing in the
      │                                                         │  frame, the game's menus extended, keys and
      │                                                         │  hotkeys, settings, data folders
      │                                                         └─ mods: mml\mods\<id>\ (each hot-reloadable)
      │                                                              ├─ core   (the game's live state)
      │                                                              ├─ telemetry, hello_c, your mods (C, C++, Rust...)
      │                                                              └─ (to come) the LuaJIT runtime, the sidecar link
   (to come) mml-sidecar64.exe: 64-bit mods, past the 32-bit address space, shared memory with the game
```

## The host (`host/`)

`MafiaModLoader.asi` is the one part that is never unloaded. Ultimate ASI Loader loads it early, on the game's main
thread, under the loader lock: there it only sets up (paths, log, crash filter, MinHook writes, the bridge's thread).
Mods load at the **first frame** (the game's frame limiter, LS3DF `0x72100` in 1.2: called once a frame in every state
except the intro videos), on the game thread, where `LoadLibrary` is safe.

- **Events.** The game's hooks (`game_hooks.cpp`) become `MmlEvent`s: `FRAME` (every frame), `MISSION_TICK`,
  `MISSION_CLOSE`, `ACTOR_DESTROYED`, `HUMAN_CREATED`, `HUMAN_HIT`; and the loader's own `SERVICES_CHANGED`,
  `MOD_LOADED`. Addresses come from `sdk/include/mml/game/game_build.h` (per build; `game::Detect` picks the running
  one by its code). An unknown build: no game events, everything else runs. `MISSION_OPEN`, `MISSION_LOADED`,
  `GAME_PAUSED`, `GAME_RESUMED` and `CONFIG_CHANGED` are told below.
- **Hook chains** (`hooks.cpp`). MinHook allows one detour per function; mods may want the same function. Each hooked
  function gets one MinHook hook whose detour is a 6-byte thunk `jmp [head]`; the chain is a list of detours, the last
  one hooked outermost. Adding or removing a link only rewrites pointers (innermost first), so the game's bytes are
  written once and a running detour always has a valid next step. Tested in every load and unload order
  (`tests/run_tests.py`).
- **Mods** (`mods.cpp`). A mod's DLL is copied to `mml\live\mod_<id>_<pid>_<n>.dll` and loaded from there: the
  original can be rebuilt while the game runs. Unloading removes everything it registered (hooks, handlers, commands,
  services, posted calls): no pointer into its code is left behind. Every call into a mod is behind `__try`: a fault
  marks it, it is unloaded at the next frame, the game goes on. `state_put`/`state_get` keep blobs across a mod's own
  reloads.
- **Services**: a mod publishes a versioned table of functions under a name; others find it. This is how separate
  mods share features at run time (the ragdoll mod's physics for a VR mod) without linking to each other.
- **The bridge** (`bridge.cpp`): see `PROTOCOL.md`. Memory and log methods answer on the bridge's thread (even while
  the game thread hangs); mod commands wait for the game thread's next frame.
- **Crashes** (`crash.cpp`): an unhandled exception is logged (module+offset, registers, a stack scan) and written as
  a minidump with the heaps and every region's state (`mml\crash\`). The game's own filter is chained, and ours is put
  back in front every frame.

- **The start** (`start.cpp`): `<game>\mml\start.json` (`{"skip_logos", "profile", "go":
  "menu"|"freeride"|"load"|"new"|"extreme", "kind", "place", "save", "traffic", "pedestrians", "police",
  "background"}`, written by `game_launch` or `tools/start.py`, deleted when read) takes the game to a profile and a
  freeride of any kind, a save or a new game by its own menus (the later screens found by their definitions'
  names, so both builds' screens alike): the logo videos answered "played"
  (PlayVideo; 1.0's AVI logos "not found" by `cd_find_game_file`), then in each menu's frame the command a click
  would send (1.2: the menu's vtable `+0x24`; 1.0: the screen's next tick's answer, `menus10.cpp`); the freeride
  menu's density sliders are set first, by the setter the menu sets them with. `menu.state` / `menu.command` /
  `menu.value` drive any menu the same way. `background`: user32's activating calls answered for the game's own
  windows, LS3D's focus, cursor clip and cursor moves skipped while it is not in front.
- **Input** (`input.cpp`): hands for agents, inside the game. *Actions*: the game reads its controls from one
  object, a list of binding slots (two an action: primary, secondary) and two float arrays parallel to it ("held",
  "pressed this frame") that its update fills from the devices once a frame, before the mission's tick reads them.
  The loader hooks that update and writes the values asked for after it: an action is pressed whatever it is bound
  to, even to nothing. *Keys*, for what is not an action (menus): LS3D's keyboard read is hooked and the keys asked
  for are set in its DirectInput state. Holds last for frames or milliseconds, may start after a delay
  (`input.seq`: a whole sequence on one clock); `turn` works the mouse axis out of an angle and the game's
  sensitivity. *The menus' mouse*: LS3D's mouse read is hooked too; after it the loader writes into its DirectInput
  state the move that takes the menus' cursor exactly to the point asked for (the difference to the cursor, which the
  menus' input tick adds), the buttons held and wheel notches; so the game's own code focuses what is under the
  cursor and clicks it (`input.mouse`, `menu.click`, MCP `input`'s `click`). Text is typed as keys: LS3D makes the
  characters of the keys it read (`input.text`). The user's keyboard and mouse are never touched, the window needs
  no focus.
- **Screenshots** (`screenshot.cpp`): eyes for agents. The device's `Present` is hooked at the first request; when
  a shot is asked for, the back buffer is copied before the frame is shown (through Direct3D 9 when a wrapper such
  as dxwrapper runs the game on it: a multisampled buffer is resolved with `StretchRect`; else Direct3D 8's
  `CopyRects`) and written raw to `<game>\mml\shots\<n>.bgra`. The MCP server makes the JPEG.
- **Drawing** (`draw.cpp`): text, lines and rectangles for mods (`mml.h` `draw_*`) and agents (`draw.*`). The game
  draws its 2D things (the HUD, the open menu, the console) in the callback it gives its scene, which LS3D's render
  calls once the 3D frame is drawn. The loader hooks that callback and draws what was queued after the game's own,
  with the game's own means: `G_IndicatorsClass::OutText` (the HUD's and the menus' fonts) and the two `IGraph`
  calls the game's rectangles are made of (`SetTexture`, `DrawPrimitive` with screen-space vertices). No device call
  of the loader's own: it works under plain Direct3D 8 and under a wrapper alike, and what is drawn is in the back
  buffer `screenshot` copies. The three engine states the HUD sets for itself are set and put back. Coordinates
  are the menus' 800x600 units, scaled as the HUD's. Mods' items live one frame; each owner's items are drawn behind
  a fault guard (a fault: that mod's drawing off until it is reloaded, logged).
- **The game's script machine** (`script.cpp`, docs/SCRIPTS.md): programs of agents and mods in the missions'
  language, each a standalone `C_program` of the loader's (the game's new and constructor), compiled by the game's
  `Compile` (the first line it does not take fails the program, named), its registers given before it starts, then
  stepped by the game's `CallProcess` once a mission tick, after the mission's own. Stopped at `END`, by a fault in a
  command (the guard), by its time, its mod's unload or the mission's close; actors deleted are taken out of its
  `act[]` by the game's `InvalidateActorReg`. `ScriptExec` runs a program at once instead (stepped until it stands on
  its end, in the same frame): what the world's named actions use.
- **The world** (`world.cpp`, docs/SCRIPTS.md "The world without a program"): actors made new from any model by the
  game's own functions (the scripts' `ACTOR_DUPLICATE` recipe with a model file of the caller's), kept in a list of
  their own (only those are taken out again, with their frame unlinked when their destructor runs); moving, seating
  and arming as the scripts' commands run by `ScriptExec`; the game's car list and items read for names.
- **The game's own menus, extended** (`menus.cpp`; 1.0's screens, other classes: `menus10.cpp`, the same services
  through the tick every screen calls, `docs/VERSIONS.md`): items and pages for mods (`menu_*`) and agents (`menu.item`,
  `menu.page`), made of the game's controls by the game's code; nothing drawn by the loader. The game's menus are
  `GM_Menu` objects holding controls it makes from definitions (`tables\menu\<name>.mnu`: 32-byte records) with one
  factory a kind; a click or Enter gives the control's id to the menu's command handler (vtable slot 9).
  *An item*: `GM_Menu::Create` is hooked; when a menu a mod asked for is made, the item is added by the game's own
  `PridejComponentPod` (how its profile and save lists grow: a copy of a control under it, the window grown, what is
  below moved), with an id between its neighbours'; the menu object gets a copy of its class's vtable whose command
  slot is the loader's (the items' ids to their mods, every other one to the class's handler). A mod's reload takes
  its live item over. *A page*: a `GM_Menu` made as the game makes its submenus (its operator new, constructor, a
  copy of the base class's vtable with the loader's name, OnCreate and command slots), put over the open menu by the
  game's push; its definition is read through the game's file layer (`rw_data.dll`'s `dtaOpen`/`dtaRead`/`dtaClose`
  hooked; that one name answered with records built from the description), so the factories make, lay out and
  focus every control. Texts are given as the game's files give them, as text ids: the game's text database
  (`G_TextDatabase::GetText`, `GetTextOrNULL`) is hooked to answer a range of ids of the loader's, which also gives
  switches and selectors their options (they walk a range of text ids). Values, ranges, pictures and a scrolled
  list's range are set in OnCreate by each control's own functions (`mml.h` lists the kinds). A scrolled list clips
  what is drawn after it, so its block goes last in the definition. A fault while the game makes a page drops it
  and makes the menu under it the active one again.
- **Keys, actions, hotkeys for mods** (`input.cpp`): after LS3D's keyboard read (and the injected keys set in it)
  the loader takes the state and its edges against the read before: `key_down`, `key_pressed`; hotkeys whose
  combination went down are called at the next loader frame. Actions are read from the controls' two arrays. So
  what agents inject is seen by mods exactly as a real key or action. With no game (the offline tests) the keyboard
  is what `input.key` holds.
- **The mission** (`game_hooks.cpp`): `C_mission::Open` is hooked: `MISSION_OPEN` (the name) once the old world has
  closed, `MISSION_LOADED` at the new one's first tick. `GAME_PAUSED` / `GAME_RESUMED`: a frame went by without the
  mission's tick while a menu is open (the pause menu's loop does not tick the mission; the main menu's does).
- **A mod's files and settings** (`config.cpp`): `<game>\mml\data\<id>\` for what a mod writes; its settings are
  `config.json` in its own folder (the defaults, shipped) with `config.json` of its data folder merged over them
  (the changes: `config_set`, `config.set`, a hand). Both files' times are looked at twice a second; a change is
  read and the mod gets `CONFIG_CHANGED`.
- **Tap** (`tap.cpp`, from mafia-ragdoll): a link of the hook chain (owner: the host) whose thunk books every call
  of a function (callers, registers, arguments, memory, the result) into fixed tables; `tap` on the bridge, startup
  taps from `<game>\mml\config.json`.

## The MCP server (`mcp/`)

A separate 64-bit process, so it outlives the game: it launches and closes the game, reads the loader's log file
when the game is gone, opens crash dumps (`minidump`, Mozilla's), and deploys builds. It talks to the host through
the bridge only. Tools: `game_status`, `game_launch`, `game_close`, `call` (any method), `commands_list`, `mem_read`,
`mem_write`, `log_tail`, `mods_list`, `mod_reload`, `where`, `tap`, `crash_list`, `crash_inspect`, `deploy`,
`kb_search`, `kb_function`, `screenshot`, `input`. (Drawing, settings and the car need no tools of their own:
`call draw.text`, `call config.set`, `call car`.) `game_launch` takes `go`, `profile` and the freeride's densities
(the start) and returns once the player exists. `screenshot` asks the loader for the next frame, turns the raw file
into `<n>.jpg` (`shot.rs`: shrunk by a whole factor to `max_width`, the newest 200 kept) and returns it as MCP image
content; `count` and `every_ms` give a short sequence. `input` lays its steps out in time (one after another, or at
`at_ms`), sends them as one `input.seq`, waits for the last to end and returns the player's state (and a
screenshot).

The knowledge base (`kb.rs`, `knowledge/README.md`) is loaded once from `knowledge/<module>/*.csv`: functions sorted
by address (an address finds the function holding it), the call graph both ways, our names; the decompiled code is
read on demand from `reverse_dir` (`mml.config.json`) by the function's line range. `crash_inspect` and `where` pass
their text through it, so `Game.exe+0x...` and `LS3DF.dll+0x...` get the function's name.

## Files in the game folder

```
<game>\MafiaModLoader.asi           the host
<game>\dinput8.dll                  Ultimate ASI Loader (pinned, tools/deps.py)
<game>\mml\config.json              optional: {"bridge_port": 47100, "taps": [...]}
<game>\mml\start.json               a start asked for (one launch; deleted when read)
<game>\mml\bridge.json              written once the bridge listens: port (47100 or the next free), token (new every run), pid
<game>\mml\mods\<id>\mod.json, ...  the mods (config.json: the defaults of a mod's settings)
<game>\mml\data\<id>\               a mod's own files; config.json: its settings that were changed
<game>\mml\live\                    the loaded copies of the mods' DLLs
<game>\mml\logs\mml.log             this run's log (mml.prev.log: the previous run's)
<game>\mml\crash\*.dmp              crash dumps
<game>\mml\shots\<n>.jpg            screenshots (the loader's raw <n>.bgra, made a JPEG by the MCP server)
```
