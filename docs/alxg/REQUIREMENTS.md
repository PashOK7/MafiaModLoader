# Requirements of our Mafia mods on MafiaModLoader (draft for PashOK7, 2026-10-14)

From: Alex's projects (vr-mod, vr-motion-controls, gog-patcher FFB, mafia-re). Marked against upstream
`9ef8199` (2026-10-13). Evidence: our reading of your repo, `docs/09-topics/001-upstream/findings-*.md`.
HAS = there today; PARTLY = some of it; MISSING = not there.

| # | Need | Why (which mod) | Mark | What exists / what is missing |
|---|---|---|---|---|
| R1 | Our ASIs load beside yours in a defined order (`mafia_vr.asi`, `mafia_re.asi`, FFB; later ragdoll as a mod) | all | HAS | UAL via dinput8 + mod load order and depends |
| R2 | One owner per patched site, or a shared chain with defined order; outside patches coordinated | vr-mod: two writers of one rotation cancel exactly | PARTLY | MinHook chains shared by mods (hooks.cpp:95-96); no owner/priority; patches by outside ASIs are invisible to the chain |
| R3 | Build detection, refusal of unknown builds, per-build tables: 1.0 `EEBD15F7`, GOG/Steam 1.2 | all | PARTLY | 16-byte code signatures (game_build.h:306-342), no md5; 1.0 tables are code twins never run; unknown build = no events at all, bridge game-thread calls hang |
| R4 | Detours and mid-function hooks for `__thiscall` / `__userpurge`; named points where a mod receives a computed value and may replace it | mafia-re 003-mod-api, all VR | PARTLY | generic hook(); no mid-function helper, no named replace-points |
| R5 | Typed access: player, human (heading, frame, car, seat, inventory, hands, muzzle, aim point), car, I3D frames, actor list | vr-motion-controls, vr-mod | PARTLY | pos/facing/alive/health/vehicle, frames, actor lists, car state; no weapon, hands, aim, camera, seat, inventory |
| R6 | Engine calls: ray casts, actor create/destroy, seat a human, spawn a car, inventory, damage, the shot | vr-motion-controls | PARTLY | world ray test only; HUMAN_HIT receive-only; spawn/seat/inventory/damage/shot missing (engine commands are your ROADMAP Next 1) |
| R7 | Per-frame events: game tick, camera update, render begin/end, a second scene pass into our own target, 2D overlay stage, Present | vr-mod (engine camera + FOV from the headset), scope | MISSING (2D stage internal) | FRAME and MISSION_TICK only; no camera, render or second-pass events; Present hooked only for screenshots |
| R8 | D3D8 device and back buffer for VR submission; HUD-vs-world draw classification | vr-mod | MISSING | d3dDevice address known (ls3d+0x1C597C) but not offered |
| R9 | Own text and quads over the HUD, 10-14 lines | vr-motion-controls hints | HAS (enough) | draw_text 127 bytes, 4096 items, 800x600 virtual units |
| R10 | Input injection and interception without focus: DirectInput, keys, mouse deltas, analog driving | vr-bench, vr-motion-controls | PARTLY | keys and mouse buttons after the game's read; mouse moves only in menus; actions 1.2 only; no joystick/analog |
| R11 | Force feedback data per tick: speed, slip, contacts | gog-patcher FFB | MISSING | car speed/steer/rps readable via core; no slip or contacts, no FFB |
| R12 | Ragdoll and physics objects as a service a VR mod can drive (place a part at a controller, hand velocity on release, read contacts) | vr-motion-controls | MISSING (planned) | your ROADMAP Later: mafia-ragdoll on the loader, physics as a service |
| R13 | Categorised logs that cost nothing when off | all | PARTLY | levels only, one file; no categories |
| R14 | Config per mod with live reload | all | HAS | config.json polled 500 ms, CONFIG_CHANGED |
| R15 | Headless test path (background game, virtual input) | vr-bench | PARTLY | MCP drives a live game (game_launch, input, screenshot); foreground only, no background/headless |
| R16 | Reproducible builds (`-Wl,--no-insert-timestamp` / `/Brepro`), build identified by md5 | all | MISSING | not set in CMake; no md5 identity |

What we can give back at once: the 1.0 knowledge base (every function described, `EEBD15F7`), the 1.0 <-> GOG 1.2
cross-build maps (fills your kExe10 zeros), `catlog.h` (R13), and our VR test bench design (R15).
