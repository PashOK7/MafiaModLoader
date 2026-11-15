# Camera projection ownership and Fusion Fix

2026-10-31. The owner reported broken water reflections and conflicting FOV controls after enabling Fusion Fix.

## Cause verified in the running game

Fusion Fix's CorrectFov changes the native half-angle multiplier. RefreshCameraFov runs inside native drawing
and rebuilds a camera if its projection does not match that policy. SDK scene views use the same native camera
temporarily, with an exact frustum. Their projection is copied for consumers BEFORE native drawing. Refresh then
rebuilt that exact projection using another Hor+ correction: the reflected texture was drawn with a different
camera from the matrix given to the water shader. No water-material workaround can repair that mismatch.

At the owner's 3620x1018 view, the advertised reflection projection _11 was 0.4469; actual VIEW_WORLD sampling
read 0.167549. The ordinary main projection was 0.446853. Developer tools held 80 degrees in the camera field,
but its effective horizontal FOV was about 131.85 degrees because Hor+ also multiplied the held angle. Earlier
FOV tests checked the field alone and did not prove the resulting picture.

Read native SetFOV in both projects: LS3DF+0xB690 (1.2) and +0xAF50 (1.0). Both store +0x140 and rebuild the
projection; 1.0 duplicates the calculation instead of calling 1.2's shared routine. Existing SDK setters/layouts
and the native scene-camera pointer are unchanged. External camera source was consulted in place under re/ext.

## Shared contract

Append-only MmlHost.camera_projection_state(mod, camera) queries host ownership. NULL selects the game camera.
MML_CAMERA_PROJECTION_FOV means an absolute game_camera_fov request. MML_CAMERA_PROJECTION_EXACT protects an
explicit scene_view, camera_override, camera.set or shadow-camera transaction, including its native setters.
SaveCam/RestoreCam retain the enclosing ownership across nested cameras; partial setter faults restore it too.
Rust has generated ABI bindings and a safe query. No host dependency on Fusion Fix or the water mod is added.

Fusion Fix uses multiplier 1 for owned FOV/frusta and skips RefreshCameraFov entirely for exact projections.
Its RENDER_BEGIN callback refreshes native policy before SDK frame/reflection cameras take their snapshots,
also after a resize or cutscene transition. On older hosts without this query, its projection correction is
disabled with a warning; the other parts still load. With FOV 0 native Hor+ remains enabled. A held FOV is released
BEFORE restoring native setters, and its live owner's request survives mission changes with a fresh baseline.

## Regression evidence

tests/game/test_camera_projection.py uses kit.projection to sample the actual native matrix in RENDER_WORLD
and VIEW_WORLD, comparing it with water's advertised reflection matrix. It verifies 60/80/110 degree effective
horizontal FOV at 1280x960, 1440x810 and 2400x900, native Hor+ restoration, inherited and explicit 40 degree
vertical views, freecam, Fusion Fix load/reload/unload, FOV-owner unload and native save/load. Back-buffer pixels,
viewport and aspect are checked too. Initial pass: 74 checks per build, both passed. Final integrated results
and installed hashes are in JOURNAL; evidence and exact pre-change backups are in build/fov-interop-20261031.

The first test requested a 1080-pixel CLIENT height on a 1080-pixel desktop. Windows constrained it to 1061 after
the frame/title bar, so that run was rejected as a fixture-size failure; the final ultrawide case fits the desktop.
Native wave images were inspected in both labs at wind 7.5, then the original water settings were restored.
