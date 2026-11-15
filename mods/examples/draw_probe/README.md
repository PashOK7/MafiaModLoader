## Draw probe

Optional diagnostic mod for 1.0 and 1.2. It records draw transforms and the world-space centre
and bounds of the associated vertex range. It tracks living actors before contact, preserves
up to 500 ms of recent samples at a traffic pedestrian hit or lethal human hit, and records
up to 1.6 s or 4 s respectively afterwards. The ring and each incident have fixed entry limits.
Completed reports use exclusive file creation in mml/data/draw_probe.

Native Direct3D 8 uses the positions read by the engine's CPU skin copy and the buffer/range
bound for the indexed draw. It observes skinned vertices inside the requested min/count range.
A wrapper with the Direct3D 9 source interface uses the source's actual unique triangle indices.
Missing or unsupported samples have matched:false. The diagnostic observes CPU geometry at
submission. Occlusion, coverage, and pixel output remain outside that measurement.

Use draw_probe.status and draw_probe.incident for the current session. The optional manual
target and mark commands let the lab fixture validate displacement without requiring a real hit.
The repository manifest defaults to off. The owner copy is configured to load it on startup.
Unload the mod to detach the diagnostic hooks at runtime.
