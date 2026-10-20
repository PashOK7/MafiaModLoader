# The game knowledge base

What is known about Mafia's code (Steam 1.2 `Game.exe` and `LS3DF.dll`), in a form agents query through the MCP
server (`kb_search`, `kb_function`) and the loader uses to name addresses (`crash_inspect`, `where`).

| Path | What | Made by |
|---|---|---|
| `<module>/functions.csv` | every function: address, name, class, signature, topics, its lines in the decompile (the strings column is empty: the game's text is not committed) | `tools/kb_import.py` |
| `<module>/calls.csv` | the call graph (caller, callee) | `tools/kb_import.py` |
| `<module>/strings.local.csv` | **made on your machine**, not committed: the strings each function uses, read from your own 1.2 `Game.exe` and `LS3DF.dll` (`python tools/kb_strings.py`, a few seconds); `kb_search` finds functions by them | `tools/kb_strings.py` |
| `<module>/names.csv` | **our own names**: functions, labels (code sites, data) found in the running game and in other projects | by hand, `ghidra_write rename` (1.2), plus `game_build.h` rows from `kb_import.py` |
| `<module>/names_10.csv` | our own names of **1.0** addresses (same columns); `where` and `crash_triage` in 1.0 name an address by them before the 1.2 twin's | `ghidra_write rename` in the 1.0 project |
| `<module>/annotations.csv` | our comments and prototypes for each build (`build,address,kind,text,source`, kind `comment` or `proto`) | `ghidra_write comment`, `proto` |
| `<module>/external.csv` | **other projects' names the repository may carry**: st0rm94's 1.0 names carried to 1.2 (shared by him for everyone's use), each address checked against the game's files; `source` says whose, `note` how it was found | `tools/kb_external.py` |
| `<module>/external.local.csv` | **made on your machine**, not committed: the other projects' names (MafiaOnline, Oakwood, reM, mafia-re, IGraph's methods from mEdit), from your own clones of them in `re/ext/` (their terms do not let us redistribute them) | `tools/kb_external.py` |
| `game.exe/external_10.csv` | st0rm94's names at **their own 1.0 addresses**, every one of his functions (the twinless too: 1.0's own code); the 1.0 Ghidra project, `where`, `crash_triage` and `kb_search` (`functions_10`) in 1.0 | `tools/kb_external.py` |
| `game.exe/twins_10.csv` | the 1.0 twin of each 1.2 function that could be paired by its code (va12, va10, how) | `tools/kb_external.py` |
| `ls3df.dll/twins_10.csv` | the same for LS3DF.dll (va12, va10, how, score) | `tools/kb_demo.py` |
| `ls3df.dll/demo.local.csv`, `demo_10.local.csv` | **made on your machine**, not committed: the Mafia demo's own names for LS3D's functions, globals and vtables (its `LS3D.dll` kept its CodeView publics), found in 1.2's and 1.0's LS3DF by their code; the note: confidence, how, score, the demo's address, the full signature; `demo_matches.local.csv` every symbol's pairing, `demo_conflicts.local.csv` where another name says otherwise (notes/external.md) | `tools/kb_demo.py` |
| `game.exe/script_commands.csv` | the mission scripts' commands: opcode, name, handler in 1.2; opcode, handler in 1.0 (read from the binaries) | `tools/kb_external.py` |
| `notes/*.md` | findings that do not fit a table: formats, layouts, how things work (`notes/external.md`: the other projects and where to read them by topic) | by hand |
| `structs/structs.csv` | the game's classes and structures: base, size, vtable and actor kind, each build's (`struct,base,size,size_10,vtable,vtable_10,kind,source,note`) | `tools/kb_structs.py` |
| `structs/fields.csv` | their fields: each build's offset (empty: not known in that build), name, type, source, note (`struct,offset,offset_10,name,type,source,note`); MCP `inspect` reads objects of the running game through them | `tools/kb_structs.py`, plus rows by hand |

`<module>` is `game.exe` (addresses are virtual addresses, image base 0x400000) or `ls3df.dll` (image base
0x10000000 as in the decompile; at run time the DLL loads elsewhere: use `ls3d+<rva>`).

## Sources

- **MafiaReverse** (a private folder, not published; `reverse_dir` in `mml.config.json`): full Hex-Rays decompiles of the 1.2
  `Game.exe` (4833 functions, 1399 with real class and method names) and `LS3DF.dll` (1599), with generated indexes.
  The repository takes the facts only. **The decompiled code is never committed**: `kb_function` reads it from the
  reverse folder at run time, so without that folder the facts still work and the code is absent.
- **mafia-ragdoll**'s Ghidra project: the names given there while making the ragdoll mod (exported with
  `ghq.py seeds`), source `mafia-ragdoll`.
- **AntiCulling** (`LS3DF_Function_Rename_Report.md`): LS3DF's visibility functions, source `AntiCulling`.
- **`sdk/include/mml/game/game_build.h`**: every address the SDK uses, source `game_build.h` (rebuilt by
  `kb_import.py`; 1.0's addresses stay in that file, the knowledge base is 1.2's).

- **Other projects** (`notes/external.md`): MafiaHub's MafiaOnline and Oakwood, DavoSK's reM, Marvisak's mafia-re
  (1.2), st0rm94's reverse-engineering export (1.0), mEdit, OpenMF, Insanity3D. Cloned into `re/ext/` (never
  committed); `python tools/kb_external.py --game12 <1.2 Game.exe> --ls3d12 <1.2 LS3DF.dll> --game10 <1.0 Game.exe>`
  makes the tables above again (pull the clones first): the names of st0rm94's export into `external.csv`, every
  other project's into `external.local.csv`, which stays on your machine.
- **Your own game**: `python tools/kb_strings.py` (the 1.2 lab of `mml.config.json`, or `--game12`) reads the
  strings each function uses into `strings.local.csv`. Nothing of the game's own text is in the repository.

## Ghidra: both builds, read and written

The knowledge base is also every build's Ghidra project's source (`re/ghidra/`, never committed, a cache):
`python tools/deps.py ghidra` once (Ghidra 12.1.4 pinned, its PyGhidra venv; a JDK 21 or later), then `python
tools/ghidra_cli.py setup` imports both labs' Game.exe and LS3DF.dll, analyses them, makes functions of the code
reached only through pointers, and puts on them what this folder says: the names by rank (`tools/ghidra_names.py`:
other projects' < st0rm94's < the SDK's < MafiaReverse's < the Mafia demo's (LS3DF) < ours; the SDK's names that
lose kept as aliases), 1.0's through `twins_10.csv` (each module's), MafiaReverse's and the demo's signatures as
prototypes, our annotations. Agents ask with MCP `ghidra_read`
(decompile, asm, callers, xrefs, vtable, twin...), `ghidra_find` (who uses a field's offset, strings, pointers,
names) and `ghidra_write` (rename, comment, proto: saved into the project and into this folder at once; `forget`
takes ours back), `build` "1.2" or "1.0"; `tools/ghidra_cli.py` is the same from a terminal.

## On a new machine

```bat
python tools/kb_strings.py                          :: the functions' strings, from your 1.2 Game.exe and LS3DF.dll
:: optional: the other projects' names (clone them into re\ext first: notes/external.md lists them)
python tools/kb_external.py --game12 <1.2 Game.exe> --ls3d12 <1.2 LS3DF.dll> --game10 <1.0 Game.exe>
```

## Growing it

- A function or site understood in the game: a row in `<module>/names.csv` (`address,kind,name,source,note`;
  kind `function`, `label` or `site`; source who found it, e.g. `mml`), its address in `game_build.h` if code uses
  it. Prefer the decompile's class names (`C_human::Hit`) where it has them.
- A format, a layout, a behaviour: a note in `notes/`, with how it was checked (in the game, by a test, read only).
- After MafiaReverse changes: `python tools/kb_import.py` (keeps the hand-made rows).
- A field understood in the running game: a row in `structs/fields.csv` with source `mml` and how it was seen in the
  note (both builds' offsets where both were seen). `python tools/kb_structs.py` (the labs' Game.exe, st0rm94's
  clone in `re/ext`) makes the generated rows again and keeps the hand-made ones; after changing `game.h` or
  `game_build.h` run it (their fields are named in its `SDK_FIELDS`; a name it lists that the headers lost stops it).

## The structures (`structs/`)

Each row says where it comes from:

- `game.h`, `game_build.h`: the SDK's own fields (what the loader's code uses, seen in both builds' games), their
  offsets read from the headers by `tools/kb_structs.py`, so the table and the code never part.
- `code`: read from both builds' Game.exe: a class's vtable, its actor kind (`+0x10`), its base class and the
  classes embedded in it (the constructors it calls on `this` and on `this+n`), its size (the `new` before its
  constructor). The constructors are 1.2's named `C_x::C_x` and their 1.0 twins; a 1.0 vtable is taken when its
  slots' functions are twins of the 1.2 vtable's.
- `st0rm94`: the fields of his `structs/*.md` (1.0), carried to 1.2 through the code: every memory access of the
  class's functions is aligned with its twin's in the other build, and a field's 1.2 offset is the one its 1.0
  offset's accesses agree on (three of four at least, two at least), unless the sure fields on both sides of it move
  by another step (a class moves in steps between the builds: 1.0's C_Vehicle is 1.2's + 0x7C past its start). The
  note says how many accesses agreed; a field none placed keeps no 1.2 offset (`inspect` counts them as
  `not_placed`).
