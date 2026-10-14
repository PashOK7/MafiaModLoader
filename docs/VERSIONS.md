# The game's builds: 1.2 and 1.0

The loader runs on both builds of `Game.exe` / `LS3DF.dll` that `game_build.h` knows (`game::Detect` picks one by
its code). 1.2 is the last patch and the Steam/GOG release (the lab in `mml.config.json`, `W:\MafiaLAB`). 1.0 is the
first retail build, in the 1.0 lab (`W:\Mafia10LAB`: the same files as mafia-ragdoll's `re/v10`, a French game).
To work on 1.0, set `game_dir` in `mml.config.json` to the 1.0 lab (every tool and the MCP server read it on each
call) and `python tools/lab.py mode loader`; set it back afterwards.

## What runs on 1.0 (seen in the 1.0 lab, 2026-10-14)

| Part | 1.0 | How it was seen |
|---|---|---|
| The host, the game's hooks, mods, the bridge | yes | every hook installed, mods loaded at the first frame |
| The mission as events | yes | `00menu` loaded when the main menu opened, `freeitaly` opened and loaded; paused / resumed with the pause menu |
| Drawing for mods and `draw.*` | yes | the telemetry overlay and a bridge text in the frame, over the HUD and the pause menu |
| The car (`car`, `game::Car`) and its state | yes | every field moving as in 1.2; `car.save`, a drive, `car.restore`: the position back to the last digit |
| Keys, hotkeys, settings, data folders | yes | F9 started the example's lap; `config.set units` took effect at once |
| Actions (`input.act`, `turn`) | yes (ported 2026-10-14) | 60 slots in 1.2's order with the profile's bindings; `Steer Left` (bound to nothing) turned the wheels; turns of 90 and -45 degrees measured 90.0 and -45.0 |
| Screenshots | yes, with a limit | the 1.0 lab renders on plain Direct3D 8 with a multisampled back buffer, which nothing can read: the loader copies the screen (`GetFrontBuffer`), only while the game's window is the one in front |
| The core mod's commands, `tap`, crash dumps | yes | `humans`, `frames.find`, `player`, `car` |
| The start (`game_launch` with `profile` / `go`) | **no** | see below: 1.0's logos and menus are other code |
| The menus' commands and values (`menu.state`, `menu.command`, `menu.value`) | **no** | 1.0's menus are other classes |
| The menus extended (`menu_item`, `menu_open`, the mouse in menus) | **no** | the same |

## Where 1.0 differs (each read in both builds' code)

- **The controls object**: the same code (`controlsUpdate` 1.0 `0x5D1F30`, 1.2 `0x4F0620`), the arrays 4 bytes
  lower: bindings `+0xB0` (1.2 `+0xB4`), held `+0xC0` (`+0xC4`), pressed `+0xD0` (`+0xD4`); now
  `controlsBind`, `controlsHeld`, `controlsPressed` in `game_build.h`. The action slots are in the same order.
- **The logos**: 1.2's `InitSystem` plays `logo1`..`logo3` by `PlayVideo` (Bink); 1.0's plays `logo1.avi` and
  `logo2.avi` itself through IGraph's IShow, after `cd_find_game_file` (`0x623FC0`, fastcall(name, path)) has
  found them: answering "not found" for those two names would skip them (not done yet).
- **The menus**: 1.0's `GM_Menu` is another class. A menu's own vtable has 6 slots (0 the destructor, 1
  OnExchange, 2 Destroy, 3 OnCloseChild, 4 MenuClose, 5 OnClick: what a click sends, 1.2's slot 9); its OnCreate is
  its constructor (not virtual); its controls are a vector at `+0x20..+0x28` (1.2 `+0x14..+0x1C`); the active menu
  is `0x67A4D8`. A control is laid out differently: `+4` id, `+8` its state, `+0xC` its text id, `+0x10..+0x1C` its
  rectangle (no text pointer at `+0x10`). The vtables found: the main menu `0x63DE74`, the profiles `0x63DF6C`,
  the freeride `0x63DFCC`, the pause menu `0x63DEA4`. The loader's menus (`host/menus.cpp`) and the start
  (`host/start.cpp`) are written against 1.2's classes: for 1.0 they need their own layer.
- **The main menu and the mission**: 1.0's modal menu loop (`0x56CCC0`) is another function and does not tick
  the mission; the main menu's `00menu` is ticked (its `MISSION_LOADED` comes) once the main menu is open, the
  profile menu's not.
- **IGraph**: the same interface, but 1.0's vtable has its slots elsewhere from the keyboard on (its keyboard read
  is slot 37, 1.2's 38; its mouse read 51, 1.2's 55): addresses are taken from the code, not by slot number.
- **The text database**: the same code (`GetText` 1.0 `0x57D850`, `GetTextOrNULL` `0x57D8B0`), its object
  `0x67A57C`; 1.0's lab is French (the texts the bridge reads are the game's language).
- **A vehicle's fields** sit elsewhere (most up to `+0xD00` 0x7C higher; `game_build.h` has each).

## How 1.0 addresses are found

Each 1.0 address is the twin of the 1.2 one, found by its code and checked: `knowledge/game.exe/twins_10.csv`
(2726 pairs by code and call graph, `tools/kb_external.py`), st0rm94's 1.0 names with the PS2 build's symbols
(`knowledge/notes/external.md`), then the two functions read side by side; a datum is the operand at the same
place in the twin. `tools/check_builds.py` checks every one against the 1.0 files.
