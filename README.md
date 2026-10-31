# MafiaModLoader

A mod loader for **Mafia: The City of Lost Heaven** (2002; Steam/GOG 1.2 and the original 1.0), made for AI agents.
An agent connects over MCP and works with the live game: starts it, plays it, changes it, writes mods for it and
reloads them while it runs.

Status: in development, before the first release. Everything below works in both builds.
[docs/ROADMAP.md](docs/ROADMAP.md) has what is next and the known gaps.

## What your AI agent can do with it

- **Run the game.** Start it straight into any chapter of the story, a new game or any freeride, skipping the logos.
  Close it. Take screenshots, even with the window in the background.
- **Control the character.** Walk, run, turn, jump, shoot, drive, use things: the game's own controls, injected.
  Click and type in the game's menus.
- **Spawn and remove anything.** Cars and people of any model, seated in cars, armed, moved anywhere. Any model as a
  prop: in the world or attached to a bone, a car or another prop.
- **Read and change the game's state.** The player, cars, people and their skeletons, money, health, weapons, ammo,
  lights and sun, weather, particles. Events for shots, hits, deaths, explosions, cars entered and left.
- **Run the game's own scripts.** Every command the missions use.
- **Take over input.** Keys, the game's actions, hotkeys. Keep a key or an action from the game, lock the player,
  replace a car's physics.
- **Add interaction.** Zones, use-points in the game's own Action menu, timers on the mission's clock.
- **Change the interface.** Text, shapes and pictures on screen in the game's fonts. Lines in the game's console and
  help line. Markers on the screen, on the radar and as the compass' target. New items and whole pages in the game's
  own menus, built from its real controls.
- **Draw in 3D.** Draw into the world with the game's Direct3D device. Replace the shaders of the game's own objects
  (HLSL, Shader Model 3, through the d3d8to9 wrapper), with the game's lights near each one and a shadow map from the
  sun. Direct3D 11 passes over the frame with its colour, depth, camera and the sun's shadow map (a god rays example).
  Hide the game's meshes and draw your own instead.
- **Use more cameras.** Move the game camera or drive it from a mod. Render the scene again from other cameras
  (mirrors, reflections, stereo views) into textures.
- **Play sound.** The game's sounds or your own, 2D or 3D, with live volume and pitch. Music.
- **Replace and add files.** Textures, models, sounds and tables from a mod's folder, loaded by the game by name.
  The game's folder stays untouched.
- **Query the world.** Rays, sphere sweeps, the ground under a point, actors nearby, collision shapes.
- **Develop fast.** Hot-reload mods while the game runs. A crashing mod is switched off, the game goes on. Per-mod
  settings changed live (also on a page in the game's Options > Mods, made from the mod's manifest), per-frame cost
  per mod, watch any function or memory field, read crash dumps with names, a
  knowledge base of the game's code. Mods in C, C++, Rust or anything with a C ABI.

## What is in it

| Part | What it is |
|---|---|
| `host/` | `MafiaModLoader.asi`: the loader inside the game (loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)) |
| `sdk/` | `mml.h`: the one C header every mod is written against |
| `mcp/` | the MCP server (Rust): the agent's tools |
| `mods/` | `core` (the game's live state) and small examples, all off by default |
| `knowledge/` | what is known about the game's code: functions, names, calls, notes |

## Quick start

Needs Windows, Visual Studio 2022 (C++ x86), CMake 3.24+, Rust, Python 3.10+ and the game (1.2 or 1.0).

```bat
python tools/deps.py                                  :: pinned downloads (hash-checked)
copy mml.config.example.json mml.config.json          :: set game_dir to your game's folder
cmake --preset x86
cmake --build --preset x86-release                    :: the loader and the mods
cargo build --release --manifest-path mcp/Cargo.toml  :: the MCP server
python tests/run_tests.py                             :: offline tests
python tools/lab.py mode loader                       :: install into the game
python tools/kb_strings.py                            :: the knowledge base's strings, from your own game
```

`.mcp.json` registers the server as `mafia` for Claude Code and clients supporting that configuration format.
For Codex, see [agent client setup](docs/AGENT_SETUP.md); it uses `config.toml`. The agent starts with
[AGENTS.md](AGENTS.md), read in full (it exceeds Codex's default instruction size limit).

## Documents

[AGENTS.md](AGENTS.md) (for agents) · [MODDING.md](docs/MODDING.md) (writing a mod) ·
[PROTOCOL.md](docs/PROTOCOL.md) (every bridge method) · [SCRIPTS.md](docs/SCRIPTS.md) (the game's scripts) ·
[ARCHITECTURE.md](docs/ARCHITECTURE.md) · [VERSIONS.md](docs/VERSIONS.md) · [ROADMAP.md](docs/ROADMAP.md) ·
[knowledge/](knowledge/README.md)

## License

MIT ([LICENSE](LICENSE)). Third-party libraries keep their own licenses: MinHook and nlohmann/json (vendored),
Ultimate ASI Loader and Jolt Physics (downloaded by `tools/deps.py`).

Unofficial fan project, not affiliated with 2K, Take-Two Interactive or Illusion Softworks. Mafia: The City of Lost
Heaven is a trademark of Take-Two Interactive Software, Inc. You need your own copy of the game; no game files are
in this repository.
