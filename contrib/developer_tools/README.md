# Developer Tools (standalone ASI, Mafia 1.2)

An in-game debug overlay and toolbox for **Mafia: The City of Lost Heaven, Game.exe 1.2**.
It is a plain **Ultimate ASI Loader** plugin (`developer_tools.asi`), **not** a MafiaModLoader mod:
it does not use `mml.h`, has no `mod.json` and is not built by the CMake/cargo build of this repository.
It lives in `contrib/` as a separate contribution, with its own build command (below).

Everything is driven from the game's ESC menu: **ESC -> DEVELOPER TOOLS** opens an extra options screen
(`tables/menu/devtools.mnu`) with the switches and sliders. Settings are saved to `developer_tools.ini`.

## Features

- **Script triggers** - for every running mission script it draws what the script is waiting on:
  - `detector_inrange` -> sphere around the detector (magenta, red when someone is inside),
  - `getactorsdist` + `iftlinrange` -> line between the two actors + threshold sphere (yellow, orange when met),
  - trigger volumes of detectors (`DUNB` frames of the scene) -> box / sphere,
  - a label with the script name / actor above each one.
- **Collision** - the level's static collision (faces, boxes, spheres, cylinders) as wireframe or filled,
  colour by material, optional material names, optional "see through" depth test.
- **check.bin / road.bin** - pedestrian path network and road waypoints / lanes.
- **Names** of all objects (actors, frames) within a distance.
- **God mode**, **infinite ammo**, **flying** (WSAD / Space / Ctrl / Shift / mouse wheel), **teleport**
  (X / Y / Z fields + "My position" button).
- **Wireframe** render mode, **free camera** (mouse + WSAD), **FOV** slider, **hide HUD**.
- **DebugText** panel (embedded former `DebugText.asi`): the game's `debug_text` script command output.
- UI in **Czech or English** (`tables/devtools_en.def` for English texts).

## Install

A prebuilt `developer_tools.asi` is included in this folder (built from these sources with the command in *Build*),
so you do not have to compile anything.

1. Ultimate ASI Loader in the game folder (any ASI loader that loads `*.asi` from the game folder or `scripts/`).
2. Copy `developer_tools.asi` and `developer_tools.ini` next to `Game.exe` (or into `scripts/`).
3. Copy `devtools.mnu` to `<game>/tables/menu/` and `devtools_en.def` to `<game>/tables/`.
4. Generate the patched `gamemenu.mnu` and `textdb_cz.def` (they are the game's own files plus
   our menu item / texts, so they are not shipped here):

   ```
   python gen_mnu.py <game>/tables/menu/gamemenu.mnu <game>/tables/textdb_cz.def
   ```

   It reads the originals (never writes them) and writes `devtools.mnu`, `gamemenu.mnu`, `textdb_cz.def`
   and `devtools_en.def` next to the script. Back up the originals, then copy the outputs into `tables/menu/`
   and `tables/`.

Only one copy may be loaded - a second copy logs `ANOTHER COPY OF DeveloperTools IS ALREADY LOADED` and does nothing.
Log: `developer_tools.log` next to the `.asi`.

## Build

Zig (as a cross C++ compiler, works on Linux and Windows):

```
F="-target x86-windows-gnu -mcpu=pentium4 -O2 -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin"
zig c++ $F -c developer_tools.cpp -o dt_main.o
zig c++ $F -DDT_EMBED -c debug_text.cpp -o dt_text.o
zig c++ $F -DMT_EMBED -c mafiatools_embed.cpp -o dt_tools.o
zig c++ -target x86-windows-gnu -shared -nostdlib -s -Wl,--entry=DllMain@12 \
    dt_main.o dt_text.o dt_tools.o -o developer_tools.asi -lkernel32 -luser32 -lgdi32
```

No CRT, no exceptions, no RTTI - the DLL only imports `kernel32`, `user32` and `gdi32`.

## Files

| File | What it is |
|------|------------|
| `developer_tools.asi` | prebuilt plugin (Game.exe 1.2) |
| `developer_tools.cpp` | the plugin: hooks, drawing, menu screen, cheats |
| `debug_text.cpp` | DebugText panel (built with `-DDT_EMBED`) |
| `mafiatools_embed.cpp` | wireframe + free camera (built with `-DMT_EMBED`) |
| `developer_tools.ini` | all settings with comments |
| `devtools.mnu`, `devtools_en.def` | the options screen and its English texts |
| `gen_mnu.py`, `mnu.py` | generator of the menu files / reader-writer of the `.mnu` format |

## Reverse engineering notes (Game.exe 1.2)

All addresses are for the **1.2** executable (absolute, image base 0x400000). They are named constants
at the top of each section of `developer_tools.cpp`, for example:

| Address | What |
|---------|------|
| `0x0063788C` | mission pointer |
| `0x00647F48` | `g_collision` (static collision); `+0x284` = level loaded |
| `0x005D5930` | raycast |
| `0x0046D3B0` | `C_program::Process` base dispatcher (thiscall, ret 4) |
| `0x00433D10` | `C_detector`'s `C_program` override |
| `0x005EA1F0` | `GM_Menu` base ctor |
| `0x00625E7C` | game menu vtable, slot 9 (click handler, original `0x5E0980`) |
| `0x0062733C` | options screen vtable template (12 slots) |
| `0x00626C00` / `0x00626A00` / `0x00626B80` | checkbox / slider / edit widget vtables |
| `0x0047AF50` | actor set position (thiscall x, y, z) |
| `0x00647EE0` | `IGraph*` (LS3DF) |
| `0x005BD8E8` | call of `G_IndicatorsClass::Render` (2D HUD), used for hide HUD |

The `.mnu` menu format is documented at the top of `mnu.py`.

## Status

Tested by the author on Mafia 1.2 (Game.exe 1.2). Not tested on 1.0 / the Definitive Edition.
