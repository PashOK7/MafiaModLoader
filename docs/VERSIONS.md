# The game's builds: 1.2 and 1.0

The loader runs on both builds of `Game.exe` / `LS3DF.dll` that `game_build.h` knows (`game::Detect` picks one by
its code). 1.2 is the last patch and the Steam/GOG release (the 1.2 lab). 1.0 is the first retail build (the 1.0 lab:
the same files as mafia-ragdoll's `re/v10`, a French game).
The labs are listed in `mml.config.json` (`"labs": {"1.2": <folder>, "1.0": <folder>}`, the user's folders);
`python tools/lab.py use 1.0` points `game_dir` at the 1.0 lab (every tool and the MCP server read it on each
call), `use 1.2` back; `lab.py status` tells the build of the folder in use (by its `LS3DF.dll`). Work for both
builds (the loader, the SDK, the shipped mods) is tested in both labs, a mod for one build in its own
(`AGENTS.md`, "Builds").

## What runs on 1.0 (seen in the 1.0 lab, 2026-10-14)

Everything the loader offers on 1.2 runs on 1.0; where 1.0's code differs, the loader has a layer of its own for it
(below).

| Part | 1.0 | How it was seen |
|---|---|---|
| The host, the game's hooks, mods, the bridge | yes | every hook installed, mods loaded at the first frame |
| The mission as events | yes | `00menu` loaded when the main menu opened, `freeitaly` opened and loaded; paused / resumed with the pause menu |
| Drawing for mods and `draw.*` | yes | the telemetry overlay and a bridge text in the frame, over the HUD and the pause menu |
| The car (`car`, `game::Car`) and its state | yes | every field moving as in 1.2; `car.save`, a drive, `car.restore`: the position back to the last digit |
| Keys, hotkeys, settings, data folders | yes | F9 started the example's lap; `config.set units` took effect at once |
| Actions (`input.act`, `turn`) | yes | 60 slots in 1.2's order with the profile's bindings; `Steer Left` (bound to nothing) turned the wheels; turns of 90 and -45 degrees measured 90.0 and -45.0 |
| A mod's own files (`host/files.cpp`) | yes | the same `rw_data.dll` exports; seen in the 1.0 lab (2026-10-15): `freeride_picture`'s picture in the freeride menu, 1.0's LS3DF asking the same presaved name (`Maps\freeride0_1.565`, from `ls3d+74355`) and the loader hiding it, the game's own picture back at the mod's unload; a mod made for 1.2 only (`"builds"`) not loaded and its file not given |
| The render events (`host/render.cpp`) | yes | `I3D_driver::Render` 1.0 `+0x5ED80`, the scene's callback 0x5F86C0 called with stage 1 too, the mission's scene at `C_mission +0x10` as in 1.2; seen in the 1.0 lab (2026-10-15): the world told, stage 1 every frame, the camera, `world_marker` drawn under the HUD |
| A heavy mod (`mods/examples/boxes`) | yes | Jolt and its own Direct3D 11 device drawing in the 1.0 lab (2026-10-15); 10 reloads in a row: 0.0 MB, 0 handles, 0 threads a reload after the first (-1.2 MB, +1 thread: as the first unload in 1.2) |
| Screenshots | yes | the 1.0 lab now runs through dxwrapper's Direct3D 9 as the 1.2 lab does (2026-10-14): shots with the game behind other windows. On plain Direct3D 8 with antialiasing the loader copies the screen, only while the game is in front |
| The core mod's commands, `tap`, crash dumps | yes | `humans`, `frames.find`, `player`, `car` |
| The start (`game_launch` with `profile` / `go` and the densities) | yes | the logos skipped, profile "Run" taken, the main menu, the freeride with `police` 0 (the game's density globals: 1, 1, 0) in 3.0 s |
| The menus' commands and values (`menu.state`, `menu.command`, `menu.value`, `menu.def`, `menu.click`) | yes | every screen's controls with their places; the main menu's 990105 opened the freeride screen, its police slider set to 0, OK clicked by the mouse |
| The menus extended (`menu_item`, `menu_open`, the mouse in menus, `input.text`) | yes | the example's item in the pause menu (the window grown), its page (texts, the bar, the check box and the selector told to the mod, Back), the mod reloaded with its item shown (taken over), two items in the main menu, a page with every kind, a page with no menu open |
| In the background (`game_launch`'s `background`) | yes | the game launched, driven by keys and the menus' mouse and shot while another program was in front |
| The frame rate freed (`mods/examples/fps_unlock`) | yes | the limiter's jump at `+0x6E5A2` and Present's yield at `+0x69DC2` (`call edi`); 440..1315 fps; 1.0's own menu camera (inlined in the menu loop, counted in frames) fixed through the loop's operands, its flight measured equal to 62.5 fps; a person's step at curbs: the same functions as 1.2's (st0rm94's `human_movement_intern` 0x49BBC0..., the slides `collision_test_character_proxy` 0x590AC0 and 0x591530), the same lengths in them, run in the 1.0 lab (2026-10-15): its freeride's curb climbed head on as at 62.5 fps (unfixed: stopped); LS3D's small rotations: the same export of LS3DF (1.0 0x1002E140, 35 call sites through Game.exe's thunk 0x624316), run in the 1.0 lab at 140..185 fps (2026-10-15): a stopped taxi at rest 0.23..0.29 s after slowing down (62.5 fps: 0.37..2.19 s; unfixed: 3.9..7.6 s, creeping up to 13 cm); the car's Tick (`vehicle_tick_drivetrain` 0x52C3B0, hooked for the step probe), its flags `+0x224`, its step time `+0x544`, the angular velocity `+0x49C`, the body's axes `+0xCB8` forward, `+0xCC4` right, `+0xCD0` up (1.2's `+0x1A4`, `+0x4C8`, `+0x420`, `+0xC3C`...: 1.0's vehicle fields from there on are 1.2's plus 0x7C) |

## Where 1.0 differs (each read in both builds' code)

- **The controls object**: the same code (`controlsUpdate` 1.0 `0x5D1F30`, 1.2 `0x4F0620`), the arrays 4 bytes
  lower: bindings `+0xB0` (1.2 `+0xB4`), held `+0xC0` (`+0xC4`), pressed `+0xD0` (`+0xD4`); now
  `controlsBind`, `controlsHeld`, `controlsPressed` in `game_build.h`. The action slots are in the same order.
- **The logos**: 1.2's `InitSystem` plays `logo1`..`logo3` by `PlayVideo` (Bink); 1.0's `engine_init` plays
  `logo1.avi` and `logo2.avi` itself through IGraph's IShow, and only when `cd_find_game_file` (`0x623FC0`,
  fastcall(name, path) -> bool) finds them (on the game's disc: a copy installed without the disc plays none). The
  start answers "not found" for those two names (`m10CdFind`). The lab's `Logo3.bik` is played by nothing in 1.0.
- **The menus** are other classes (the loader's 1.0 layer: `host/menus10.cpp`; the layouts: `game_build.h` `m10*`,
  `knowledge/notes/interface.md` "1.0"):
  - A screen's vtable has 6 slots (0 the deleting destructor, 1 OnExchange, 2 Destroy, 3 OnCloseChild, 4
    MenuClose, 5 Tick). **There is no command handler**: each screen's Tick calls the base's `gui_menu_tick`
    (`0x56EA70`), which returns the id of the control a click or Enter activated, and switches on that id itself.
    So the loader's `menu.command` (and the start's steps) is that tick's answer the next time the screen ticks,
    a frame later: the screen does with it what a click does. The ids are 1.2's (the profiles 100 + i, the main
    menu's 990101..990113, the freeride's 201, the pause menu's 102, 103, 108, 111).
  - Every screen is built from one table, `tables\menu.def` (in memory at `0x672630`): groups of 0x24-byte records
    `{id, tag, x, y, w, h, text id, flags, colour}` ending in `stop`, a constructor builds its group by number
    (`gui_menu_build_components_from_design`, which takes the table as an argument: the loader's pages are built by
    it from a table of the loader's). There is no `end` record: a container (`wind`, `wint`, `winc`, `swnd`,
    `frac`, `frmt`) is the parent of every record after it. `menu.def {name}` decodes a screen's group (`{design}`
    any group).
  - A screen: `+0x18` the one under it (1.2: `+4`), `+0x20/+0x24` its controls (1.2 `+0x14/+0x18`), `+0x2C` the
    focused one; the active one `0x67A4D8`. A control: `+4` id, `+8` flags (1 selectable, 0x10 focused, 0x100
    hidden), `+0xC` text id, `+0x10` x, y, w, h in pixels from its parent's client origin (a container's own, on the
    screen, at `+0x38`; a titled window's is under its 40-unit title), `+0x20` and `+0x24` its record's flags and
    colour words (each kind's own: a text's font and colour, a check box's state, a number's value), `+0x2C` a
    float (sliders, bars), `+0x30` the text drawn, `+0x34` its parent. A kind is known by its vtable (53 kinds,
    `menus10.cpp`).
  - The kinds have other tags: 1.2's slider `slid` is 1.0's `scrl`, the scrolled list `wins` is `swnd`, `sldi` is
    `scls`, `edin` is `ednu`, the selector `bsel` and the switch `bslb` are both `cmbs` (options from a range of
    text ids, picked from the game's drop-down list), a text field `edit` is `edtl` (1.0's `edit` takes 10
    characters, `edtl` 31), a picture from a file `rctt` is `rctf` (1.0's `rctt` is the freeride screen's preview);
    alignment is the kind (`stxt`, `ctxt`, `rtxt`; `butl`, `butt`, `butr`). A page asking for 1.2's kinds gets
    1.0's; 1.0's own tags are taken as they are.
  - The names: a screen is named by its vtable; where 1.2 has the same screen, by 1.2's definition name
    (`mainmenu`, `gamemenu`, `freeride`, `loadgame`, `profileselect`, `optionsa` (audio and video), `optionsb`
    (options), `optionsc1`, `exityesno`, `questionbox`, `messagebox`, `combomenu`, `carcyclopedia`...), so a mod's
    `menu_item(..., "gamemenu", 108, ...)` works on both builds; the racing and multiplayer screens by st0rm94's names
    (`race_setup`, `mp_lobby`...).
  - The menus' cursor: two floats at `0x672618` (1.2: two ints), the scale at `0x67A4C0`; the modal loop
    (`0x56CCC0`) adds the mouse's moves to it, as 1.2's does.
- **The main menu and the mission**: 1.0's modal menu loop (`0x56CCC0`) is another function and does not tick
  the mission; the main menu's `00menu` is ticked (its `MISSION_LOADED` comes) once the main menu is open, the
  profile menu's not.
- **IGraph**: the same interface, but 1.0's vtable has its slots elsewhere from the keyboard on (its keyboard read
  is slot 37, 1.2's 38; its mouse read 51, 1.2's 55): addresses are taken from the code, not by slot number.
- **The text database**: the same code (`GetText` 1.0 `0x57D850`, `GetTextOrNULL` `0x57D8B0`), its object
  `0x67A57C`; 1.0's lab is French. The texts are in the language's code page, 1252 for French (2026-10-14: read as
  1250 before, "Piège" came out "Pičge"); the language is `+4` of `tables\lang_<ll>.def`'s record, `0x661C6C` in
  1.0 (`menuLang`), `0x6C42C4` in 1.2.
- **The start's other ways** (2026-10-14): the freeride's kind menu, its spawn menu and the load menu are the same
  ids and the same names in 1.0 (`freeridetrack` 401..405, `freeridestart` 101.., `loadgame` 100.. newest first, the
  saves' records at the profile manager's `+0x14`): the night city with Hoboken and the 17th chapter's save were
  started in the 1.0 lab by the same code as in 1.2.
- **The freeride's densities**: the same ids (21, 22, 23), set and read by `gui_menu_set_component_float` /
  `get` (`0x56F1F0`, `0x56F1C0`); the start stores them in `0x671EDC..` and the mission uses them only when the
  profile manager's `+0x1F0` is 3 or more, as in 1.2. The freeride's kind is the screen's `+0x48` (1.2's `+0x30`).
- **A vehicle's fields** sit elsewhere (most up to `+0xD00` 0x7C higher; `game_build.h` has each).

## How 1.0 addresses are found

Each 1.0 address is the twin of the 1.2 one, found by its code and checked: `knowledge/game.exe/twins_10.csv`
(2726 pairs by code and call graph, `tools/kb_external.py`), st0rm94's 1.0 names with the PS2 build's symbols
(`knowledge/notes/external.md`), then the two functions read side by side; a datum is the operand at the same
place in the twin. 1.0's menus have no 1.2 twin: they were read in st0rm94's annotated 1.0 decompile
(`re/ext/mafia-reverse-engineering-export/ghidra/Game-ghidra.exe.annotated.c`, `structs/gui_screen.md`,
`gui_widget.md`, `game_menus.md`) and in mafia-ragdoll's 1.0 Ghidra project (`tools/ghq.py --v10`).
`tools/check_builds.py` checks every one against the 1.0 files.
