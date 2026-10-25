# Other projects' work on Mafia: what each holds, where to read it

Found 2026-10-14 (to take the most from what others have already reversed). The repositories are
cloned into `re/ext/` (ignored by git; `git clone --depth 1` each again on another machine). No code and no prose
of theirs is copied; read their documents in place, by the paths below. Their names, each checked against our files
by `tools/kb_external.py`, go to `knowledge/<module>/external.local.csv` on the machine that has the clones, never
into the repository: their terms do not let us redistribute them (MafiaHub's OSS license allows use only to
contribute to their work, Oakwood's forbids derivative works, reM and mafia-re state no terms). st0rm94 shared his
export for everyone's use and agreed to it: his names are committed (`external.csv`). What the loader's code says
it learned from a project (an address first seen there) was checked in the game's own code before it was used.

## The sources

| Source | Build | What it holds | Terms |
|---|---|---|---|
| [MafiaHub/MafiaOnline](https://github.com/MafiaHub/MafiaOnline) (multiplayer, active 2026) | **1.2**: the Steam `Game.exe` (SHA-256 `303eb95e...`, our `re/Game.exe`) | `docs/native_contract.md`: every hook and call with the original bytes, the convention, the callers and what it does, audited in IDA; `code/sdk/include/mafia1/sdk/`: address constants by area; `docs/*.md`: the scripting API, combat, pedestrians, vehicles, UI | MafiaHub OSS license (BSD-like, attribution) |
| [MafiaHub/rc1-oakwood](https://github.com/MafiaHub/rc1-oakwood) (multiplayer, 2020) | **1.2** (Steam, GOG) | `Vendors/mafia-sdk/MafiaSDK/`: classes with `FunctionsAddresses` enums and padded layouts (C_Human, C_Car, C_Vehicle, C_Game, C_Mission, C_Program, GM_Menu, C_Indicators, I3D_*) | its own license (SA-MP-derived) |
| [DavoSK/reM](https://github.com/DavoSK/reM) | **1.2** | `projects/`: reversed classes, hooked by address; `idbs/LS3DF.idc`: an IDA export of the 1.2 LS3DF with ~1200 names (C++ decorated) | none stated |
| [Marvisak/mafia-re](https://github.com/Marvisak/mafia-re) (active 2026) | **1.2** | a byte-exact decompilation (reccmp): `rw_data.dll`, `IJoy.dll` done; `LS3DF.dll` well along (2026-10-25: ~1080 functions, `// FUNCTION: LS3DF 0x...`, our 1.2 file byte for byte; the same code also annotated for a 1.1 LS3DF, `LS3DF11`); `Setup.exe` of every edition (`setup/`: the Steam 1.2's and the 1.0's are our labs' files byte for byte); `Game.exe` begun (`game/`: a handful, for another exe than ours, `Game.orig.exe` with the CD check) | none stated |
| [st0rm94/mafia-reverse-engineering-export](https://github.com/st0rm94/mafia-reverse-engineering-export) (active 2026, AI-assisted) | **1.0** (`Master 1.0`, the same code as our `re/v10/Game.exe`) | `ghidra/`: 5098 named functions with the PS2 build's symbol for each (`Game-ghidra.exe.functions.md`), the annotated decompile; `structs/`: 114 documents, one a class or system; `police-system/`, `basic/artifacts/` (game loop, HUD elements, interactions, racing, vehicle movement); `script-mapping/`: the script machine; `def-files-docs/`: data tables | none stated |
| [Sevenisko/mEdit](https://github.com/Sevenisko/mEdit) | LS3D of 1.2 | an editor; `Vendors/LS3D/Include/`: LS3D's interfaces (IGraph, I3D_driver, I3D_frame, materials, textures, sound) | GPL-3.0 |
| [MafiaHub/OpenMF-Archived](https://github.com/MafiaHub/OpenMF-Archived) | files | parsers of the game's formats (4ds, scene2.bin, cache.bin, check.bin, road.bin, scripts) | GPL-3.0 |
| [mice777/Insanity3D](https://github.com/mice777/Insanity3D) | LS3D's predecessor | the engine's ancestor by the same author, its source and `Docs/I3D_SDK.chm` (the I3D interfaces explained) | Apache-2.0 |

## What came into the knowledge base (`tools/kb_external.py`, 2026-10-14)

- `knowledge/game.exe/external.csv`: 3146 names. MafiaOnline 258 (the contract's rows only where their original
  bytes are in our file: 1 row was not and is left out), Oakwood 147, reM 15, st0rm94 2726.
- st0rm94's names are 1.0's, carried to 1.2 by pairing the functions of both builds by their code
  (`knowledge/game.exe/twins_10.csv`: exact code with addresses taken out, the strings a function uses, the call
  graph from every pair, the callers, the instructions' likeness; seeded with the pairs `game_build.h` holds).
  Checked: all 17 pairs found earlier by hand come out right; of 889 functions the decompile already names with a class, 851
  agree, 35 have another method name in the same class, 3 another class. Each row's note says how it was paired
  (`exact` and `known` are the surest, `loose` and `callers` the least). 2726 of 5098 are paired: the rest differ
  too much between the builds (or are too small to be told apart).
- `knowledge/ls3df.dll/external.csv`: 1141 names: reM's IDA export 919, mafia-re 129 (1332 after its clone was
  pulled again on 2026-10-25: `git -C re/ext/mafia-re pull`, then this tool), IGraph's 75 methods by slot
  from mEdit's header (checked by two slots we had read ourselves), Oakwood 18.
- `knowledge/game.exe/script_commands.csv`: the mission scripts' 396 commands of 1.2 with their opcodes and
  handlers, and the 389 of 1.0, read from the binaries (the keyword table DecodeInstruction builds, run in an
  emulator; the dispatcher's jump table). 1.2 has seven more (`FRM_SETON1`, `GRABER_SETFOCUS`, `MATH_ACOS`,
  `MATH_ASIN`, `MATH_ATAN`, `VECT_ANGLETO_2D`, `VECT_ROTATE_2D`), so its opcodes are shifted from 1.0's. A
  `default` handler is a command the dispatcher does not handle (st0rm94's `opcode-table.md` explains each:
  markers, commands handled by an override such as the detector's, commands that do not compile).
- IGraph's slots, now named, confirm the ones this project had read: 1 CreateITexture, 3 SetTexture, 15 Present,
  17/18 SetState/GetState, 22 DrawPrimitiveList, 38 UpdateKeyboardData, 39 ReadKey, 40 ReadChar, 50 TranslateKey,
  55 UpdateMouseData, 56-58 Mouse_rx/ry/rz, 62 GetMouseButtons (1.0's vtable differs: its keyboard read is slot 37, its mouse read 51).

## The Mafia demo's own names for LS3D (`tools/kb_demo.py`, 2026-10-20)

The demo (one mission, 2002-11) ships beside its LS3DF.dll (byte for byte the 1.0 lab's) an `LS3D.dll` linked
2002-07-04 (engine 3.76; 1.0's is 3.84) that still carries the linker's CodeView NB11 block: 9892 public symbols,
3109 of them functions with their mangled C++ signatures (the real classes, methods, argument types and calling
conventions), 406 globals and 106 vtables besides the string literals and constants. Its .text is 1.7 times 1.0's:
it also links qhull (349 functions), DirectShow's base classes and the editor's code (its debug drawing, image saving), and was
linked without identical-code folding. The rest is the same code: `tools/kb_demo.py` finds its functions in 1.0's
LS3DF and 1.0's in 1.2's (its docstring says how: exports, the SDK's tables, exact and loose shapes, strings, calls
and callers, vtable slots beside paired ones, the link order between pairs; pairs whose writable data moves against
what the other pairs say are refuted and the pairing made again), and the demo's functions in 1.2 straight as a
check of the two steps.

- **Matched** (2026-10-20): of the 3109 functions, 1798 in 1.0 and 1803 in 1.2, 122 more found folded into another
  of the same code (a second name in the note); 1638 high in both builds. Demo -> 1.0 -> 1.2 and demo -> 1.2 straight
  agree for 1622 and differ for 1 (two classes' alike getters 1.0 folded into one, apart again in 1.2: the straight
  pair is taken). Of the 1255 left, 349 are qhull's and most others DirectShow's, the editor's or tiny
  methods (AddRef, empty destructors). Globals and vtables: 382 placed in 1.0, 378 in 1.2 (the votes of the paired
  instructions' addresses). 1.0's LS3DF had about 20 names before (the SDK's); `ghidra_names` now gives it 2052.
- **Checked**: the 74 1.0 functions whose own messages name a method all carry it (15 differ only as copied
  messages do, or as a name windows.h turns into its `...A` form by a macro, which the demo's symbols carry too);
  every hand-verified name of `names.csv` and `names_10.csv` (PoseLod, CopySkinnedPositions, LightVertices, the
  m_AddFGroups and m_RenderFGroups...) agrees. Where another name says otherwise (`demo_conflicts.local.csv`: 54,
  most of them the SDK's own vocabulary, frameSlotNN, frameLimit, AntiCulling's visibility names) ours stay the
  primary name (rank 5 over the demo's 4) and the SDK's become aliases.
- **Where it goes**: the demo's licence forbids copying any part of it, so its names stay on the machine that has the
  demo: `knowledge/ls3df.dll/demo.local.csv` (1.2), `demo_10.local.csv` (1.0), `demo_matches.local.csv` (every
  symbol: its addresses, how it was paired, its score and confidence), `demo_conflicts.local.csv`. Committed:
  `knowledge/ls3df.dll/twins_10.csv`, the 1.0 twin of each 1.2 LS3DF function (2279; our own files' addresses only),
  through which 1.0 also takes 1.2's other names. The demo's signatures are the projects' prototypes in 1.0 (over
  1.2's carried), in 1.2 only where MafiaReverse has none (its methods may have changed in a year and a half).
- **Again on another machine**: the demo (`Mafia_demo_setup`, 188 MB) unpacked into `re/demo/` (its `LS3D.dll`
  only is read), then `python tools/kb_demo.py` (the labs of `mml.config.json`, or `--ls3d10`, `--ls3d12`; about a
  minute), then `python tools/ghidra_cli.py sync` and `--build 1.0 sync`. The demo's `Game.exe` (2002-10-31, not
  protected) has no symbols; `prog_dbg.bin` holds its mission script's debug names.

## Where to read, by topic

- **The engine's commands (spawn, teleport, weapons, time, weather)**: `script_commands.csv` for the handler of
  each command in 1.2; st0rm94 `script-mapping/artifacts/opcode-table.md` and `opcodes-verified.md` (what each
  does, its arguments); MafiaOnline `docs/native_script_command_mapping.md`; `code/sdk/.../script/native_program.h`.
- **Spawning people and cars**: MafiaOnline `code/sdk/.../player/native_human_factory.h`, `car/native_car.h`,
  `car/native_car_catalog.h`, and their rows in `docs/native_contract.md`.
- **Death, damage, combat**: MafiaOnline `player/native_death.h`, `docs/combat_status.md`,
  `docs/combat_simulation.md`; st0rm94 `structs/human.md`, `structs/actor_shot.md`.
- **The HUD and its messages (an "achievement unlocked" in the game's own style)**: MafiaOnline
  `ui/native_hud.h`, `ui/native_indicators.h` (they print chat into the game's own HUD console), `docs/ui.md`;
  st0rm94 `basic/artifacts/hud_elements.md`, `structs/hud.md`.
- **Menus**: st0rm94 `structs/game_menus.md`, `structs/gui_screen.md`, `structs/gui_widget.md`; MafiaOnline
  `menu/native_menu.h` (a menu of their own with edit fields, drawn by the game's renderer).
- **Police**: st0rm94 `police-system/`. **AI**: st0rm94 `structs/ai_*.md`. **Racing**: `structs/racing*.md`.
- **Saves and profiles**: st0rm94 `structs/loadsave.md`; MafiaOnline `profile/native_profile.h`.
- **Field layouts**: st0rm94 `structs/<class>.md` (1.0: most classes keep their layout in 1.2, a vehicle's fields
  do not, see `game.md` "Cars"), Oakwood's padded classes (1.2), reM's classes (1.2).
- **LS3D's interfaces**: mEdit `Vendors/LS3D/Include/`; Insanity3D `Docs/I3D_SDK.chm`.
- **File formats**: OpenMF-Archived, mEdit, st0rm94 `def-files-docs/`.

Their claims are leads, as every older tool's here: what code uses is read in our disassembly first
(AGENTS.md: correct over quick).
