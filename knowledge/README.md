# The game knowledge base

What is known about Mafia's code (Steam 1.2 `Game.exe` and `LS3DF.dll`), in a form agents query through the MCP
server (`kb_search`, `kb_function`) and the loader uses to name addresses (`crash_inspect`, `where`).

| Path | What | Made by |
|---|---|---|
| `<module>/functions.csv` | every function: address, name, class, signature, topics, the strings it uses, its lines in the decompile | `tools/kb_import.py` |
| `<module>/calls.csv` | the call graph (caller, callee) | `tools/kb_import.py` |
| `<module>/strings.csv` | the string sites (address, the strings there) | `tools/kb_import.py` |
| `<module>/names.csv` | **our own names**: functions, labels (code sites, data) found in the running game and in other projects | by hand, plus `game_build.h` rows from `kb_import.py` |
| `notes/*.md` | findings that do not fit a table: formats, layouts, how things work | by hand |

`<module>` is `game.exe` (addresses are virtual addresses, image base 0x400000) or `ls3df.dll` (image base
0x10000000 as in the decompile; at run time the DLL loads elsewhere: use `ls3d+<rva>`).

## Sources

- **MafiaReverse** (the owner's folder; `reverse_dir` in `mml.config.json`): full Hex-Rays decompiles of the 1.2
  `Game.exe` (4833 functions, 1399 with real class and method names) and `LS3DF.dll` (1599), with generated indexes.
  The repository takes the facts only. **The decompiled code is never committed**: `kb_function` reads it from the
  reverse folder at run time, so without that folder the facts still work and the code is absent.
- **mafia-ragdoll**'s Ghidra project: the names given there while making the ragdoll mod (exported with
  `ghq.py seeds`), source `mafia-ragdoll`.
- **AntiCulling** (`LS3DF_Function_Rename_Report.md`): LS3DF's visibility functions, source `AntiCulling`.
- **`sdk/include/mml/game/game_build.h`**: every address the SDK uses, source `game_build.h` (rebuilt by
  `kb_import.py`; 1.0's addresses stay in that file, the knowledge base is 1.2's).

## Growing it

- A function or site understood in the game: a row in `<module>/names.csv` (`address,kind,name,source,note`;
  kind `function`, `label` or `site`; source who found it, e.g. `mml`), its address in `game_build.h` if code uses
  it. Prefer the decompile's class names (`C_human::Hit`) where it has them.
- A format, a layout, a behaviour: a note in `notes/`, with how it was checked (in the game, by a test, read only).
- After MafiaReverse changes: `python tools/kb_import.py` (keeps the hand-made rows).
