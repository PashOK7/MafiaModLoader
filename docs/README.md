# Documentation map

Reviewed for the cumulative November update on 2026-11-15. Repository text is English. Instructions for
working on this project are in [AGENTS.md](../AGENTS.md); `CLAUDE.md` imports the same instructions.

## Start here

1. [Current status](STATUS.md): what is implemented, what remains unresolved, validation and publication state.
2. [November patch notes](PATCHNOTES_2026-11-15.md): changes since October 31 and how an existing agent/mod updates.
3. [Architecture](ARCHITECTURE.md): ownership, processes, lifecycle and subsystems.
4. [Agent setup](AGENT_SETUP.md): MCP connection, restart and terminal fallback.
5. [Modding guide](MODDING.md) and [protocol](PROTOCOL.md): supported contracts and exact command parameters.
6. [Roadmap](ROADMAP.md): remaining work. [Journal](JOURNAL.md): dated experiments and receipts, newest last.

The C ABI is [mml.h](../sdk/include/mml/mml.h). Check structure sizes and optional suffixes against the
running host. A newer checkout, an older installed ASI and an already-running MCP server can coexist.
Do not infer installed capabilities or publication from a document date or API version 1 alone.

## Guides and active specifications

| Document | Use and scope |
|---|---|
| [Root README](../README.md) | Entry point and build prerequisites; not a complete acceptance report |
| [VERSIONS](VERSIONS.md) | Per-build differences; older acceptance tables are dated, not exhaustive |
| [SCRIPTS](SCRIPTS.md) | Original mission language, registers and tested recipes |
| [ATMOSPHERE_SDK](ATMOSPHERE_SDK.md) | Shared host mechanism, public SDK and small policy mods |
| [SCENE_RADIANCE_SNAPSHOT](SCENE_RADIANCE_SNAPSHOT.md) | Owned geometry/material snapshots, current-frame publication, unknown coverage |
| [SCENE_RESIDENCY](SCENE_RESIDENCY.md) | Owner-scoped source coverage independent of the main camera frustum |
| [WATER_REFLECTION_PLAN](WATER_REFLECTION_PLAN.md) | Active reflection/shadow/optimization gates; work is unfinished |
| [Knowledge base](../knowledge/README.md) | Functions, twins, structures, Ghidra and source restrictions |
| [Third-party dependencies](../third_party/README.md) | Pinned sources, build layout and licences |
| [Soundscape](../mods/examples/soundscape/README.md) | Optional visualization and its measurement/model limits |
| [Draw probe](../mods/examples/draw_probe/README.md) | Optional bounded CPU draw-submission telemetry; not a pixel proof |
| [Radar car lifetime](HUD_CAR_LIFETIME.md) | Native car carry across missions, borrowed HUD references and both-build regression |
| [DirectSound regression](../tests/dsound/README.md) | Opt-in synthetic playback/capture, separate from silent background tests |

## Reference notes

These explain verified native mechanisms. Their individual dates define the scope of the measurements.

| Topic | Notes |
|---|---|
| Game objects and gameplay | [Game](../knowledge/notes/game.md), [vehicle physics](../knowledge/notes/vehicle_physics.md), [scale](../knowledge/notes/scale.md) |
| Interface, input and windows | [Interface](../knowledge/notes/interface.md), [IJoy](../knowledge/notes/ijoy.md), [windows](../knowledge/notes/window.md) |
| Native rendering | [Camera projection](../knowledge/notes/camera-projection.md), [render controls](../knowledge/notes/render-controls.md), [skinning](../knowledge/notes/skinning.md), [billboards](../knowledge/notes/scene-billboards.md) |
| Atmosphere and water | [Atmosphere](../knowledge/notes/atmosphere.md), [water](../knowledge/notes/water.md) |
| Files and media | [rw_data](../knowledge/notes/rw_data.md), [videos](../knowledge/notes/videos.md), [audio allocation](../knowledge/notes/audio.md) |
| Research with open conclusions | [EAX environments](../knowledge/notes/eax-environments.md), [sound-dependent gameplay](../knowledge/notes/audio-logic.md) |
| Other reverse-engineering sources | [External sources](../knowledge/notes/external.md); local-only material must remain local |

## Dated investigations and superseded plans

| Document | How to read it now |
|---|---|
| [Ragdoll port review](RAGDOLL_PORT_REVIEW.md) | November 8–9 comparison; later car-impact policy replaces its old impulse branch |
| [Car-impact investigation](../knowledge/notes/car-impact-20261115.md) | Current/last-car discovery, immediate real-mesh collision and removed artificial launch |
| [Pedestrian dodge](../knowledge/notes/pedestrian-dodge-20261114.md) | A reproduced pre-physics rewind fixed in the separate ragdoll port |
| [Scenery readiness](../knowledge/notes/ragdoll-scenery-20261114.md) | Reproduced scan starvation and delayed activation; separate from pose teleports |
| [Teleport at 16:04](../knowledge/notes/teleport-20261114-1604.md) | A retained incident, not a general conclusion that all movement is explained |
| [Teleport at 16:37](../knowledge/notes/teleport-20261114-1637.md) | Intermediate transforms versus visible displacement; later dodge evidence has a distinct identity |
| [Mission-load hang](../knowledge/notes/hang-20261114.md) | Historical event unassigned; induced native Ogg deadlock separately reproduced and fixed |
| [Final validation fixes](../knowledge/notes/release-validation-20261115.md) | Observed/unobserved index-buffer classes, SIB stack targets and the passing final regression |
| [Water render plan](WATER_RENDER_PLAN.md) | Earlier rendering/optimization work; active order is in WATER_REFLECTION_PLAN |
| [Water/ragdoll review](WATER_RAGDOLL_REVIEW.md) | Dated source comparison, not an instruction to port all proposed mechanisms |
| [Water lighting audit](WATER_LIGHTING_ARCHITECTURE.md) | Original November 1 audit/design; its baseline hashes and findings are historical |

## Evidence rules

- `build/`, `re/`, dumps, local symbol CSVs, saves, game assets and machine configuration are not distributed
  with the source. A path to them records local evidence; a GitHub reader must run the named test or obtain
  an explicitly shared report. Missing local evidence is not a shipped asset dependency.
- Dated PIDs and hashes identify one experiment. Check the current exact folder, PID, loaded module and hash.
- A passing fixture proves its assertions. It does not prove the cause of an earlier uncaptured event or the
  absence of all intermittent faults. Failed receipts stay available and current limitations stay visible.
- Current contracts belong in MODDING/PROTOCOL/SDK. Rejected approaches remain in the journal with their
  replacement identified. A patch note does not override AGENTS.md or grant permission to publish.
