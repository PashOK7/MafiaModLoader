# Mafia knowledge base (kb/)

Facts about Mafia: The City of Lost Heaven (2002) collected from Alex's Mafia projects for AI agents.

Line format: `- <fact> | build <EEBD15F7 (1.0) / GOG 1.2 / ?> | src <path>:<line> | tag <[decomp]/[disasm]/[data]/live/[inferred]>`
- `src` paths are relative to `E:/!Claude/Mafia` (or `E:/!Claude` for `docs/...`). The FILE is exact; the LINE is
  approximate (re-checks 2026-10-19: 0 wrong facts of 70, but 30 of 70 line pointers off by a few lines) - grep
  the file for the address.
- `UNSOURCED` = no source found for the fact; do not use it without checking.
- `## Conflicts` lists places where our sources disagree; both values are kept, nothing chosen. Each is a request to
  mafia-re (the project that owns the disassembly).

| file | subsystem |
|---|---|
| camera.md | camera, view, FOV, culling |
| player-human.md | player, human, aim, hands, animation |
| frame.md | I3D frames, transforms |
| weapon.md | weapons, shot, bullets |
| vehicle.md | cars, car physics, FFB inputs |
| render.md | render, D3D8, HUD |
| physics-input.md | physics, collision, ray casts, input |
| techniques.md | how to hook, patch, test, run headless |
| builds.md | every build, ids, address maps |
| compare-game_build.md | MafiaModLoader game_build.h 1.0 values vs our 1.0 <-> 1.2 maps |
| hook-overlap.md | where our mods and the loader hook the same code (1.0) |
| _raw/ | the per-source collections the files above were merged from |
