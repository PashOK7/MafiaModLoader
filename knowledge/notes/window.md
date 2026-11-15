# Live window resizing

2026-10-31. Owner requested resizing an already running game. Implementation: host/fullscreen.cpp and gfx.cpp.

- The ordinary LS3D window has a top-level window and a child rendering window. D3D8's presentation HWND is
  the child. Resize the root with a sizing frame/maximize button and fit its child to the client; do not reject
  the ordinary window just because its presentation target has WS_CHILD.
- Scrn_sx/sy select the child size when bit 0x80 of the native initialization flags is set. In 1.2 the description's
  width/height start at LS3DF+0x1C55CC; child width/height at +0x10/+0x14, flags +0x1C. In 1.0: +0x1C156C with the
  same relative fields. Read in both getter disassemblies, verified by tools/check_builds.py.
- The native device recreation function is fastcall(bool): LS3DF+0x6E1A0 / +0x6A8A0. It frees the engine's buffers,
  calls D3D8 Reset with the parameters at +0x1C52C0 / +0x1C1260, rebuilds its buffers/shaders/state. Calling only
  the COM Reset would omit that engine lifecycle. Call between frames, outside Render/Present and window callbacks.
- Recalculate the native IGraph viewport (slot 19), HUD ResolutionChanged and open menus. Native menu exchange
  is 1.2 Game.exe 0x5EAD80. The loader must supply a page's virtual file while rebuilding it, just as on initial
  creation. 1.0's OnExchange recursively rebuilds native screens; loader pages use their existing builder and
  the controls-only free at 0x56E850. Preserve page values and reinsert mods' menu items.
- Window messages only record/coalesce size changes. A sizing drag commits after WM_EXITSIZEMOVE; maximize,
  restore and programmatic sizes settle between frames. Minimize never requests a zero-size device. The game
  still uses its native modal window sizing loop while a border is held; continuous world simulation during
  that loop is not added. Exclusive fullscreen and the existing bridge-borderless mode keep their own behavior.

## Reset failure found and fixed

Initial resize tests crashed both builds after Reset returned 0x8876086C. The engine had already released its
global buffers and proceeded to draw indexed geometry with a null buffer. Dumps: the labs' Game_14432 and
Game_18304 reports, 2026-10-31 15:25. crash_triage was read first; no attribution to water/DLSS was assumed.

dxwrapper's forced D3D9Ex Reset passed a fullscreen display descriptor made from the windowed parameters into
the driver. Its width/format/refresh need not describe a valid fullscreen mode. The forwarding overload now
passes null when the final parameters are windowed. Source patch: third_party/dxwrapper/patch.py. A real-device
regression in tests/dxwrapper/resize.h resizes through the wrapper repeatedly while retaining and checking a
managed texture. [Microsoft's ResetEx contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9ex-resetex)
documents persistent resources and the fullscreen descriptor. Native tests now succeed through this path.

On reset failure restore both the previous client size and parameters before retrying (zero back-buffer sizes
are implicit client sizes). If rollback fails too, skip scene drawing/presentation and retry between frames;
the engine must never consume the buffers it just freed. Tests can fail COM Reset once or twice through the
test-only kit mod; no failure injection exists in the production bridge.

## Black-frame regression rejected by visual QA

The first native test checked sizes, process liveness and advancing DLSS counters only. It passed while both
windows were black; the owner correctly rejected that evidence. A freshly launched 1.0 game could resize to
a larger buffer and recover forced Reset failures, narrowing the error to scene dimensions rather than driver
failure recovery. Its world still occupied the original rectangle, leaving old menu pixels outside it.

Native Render compares scene+0x23C's rectangle with IGraph's cached viewport. If a stale scene rectangle exceeds
the resized back buffer, device SetViewport fails and Render returns without drawing. ResetDevice does not resize
existing scenes. Calling I3D_scene::SetViewport (1.2 +0x4A9C0, 1.0 +0x48520, stdcall(scene,0,0,right,bottom)) restored
both black windows without a restart. It also updates the scene center and half-size fields; writing the rectangle
alone is insufficient. Read both functions in Ghidra and added checks for their stores in tools/check_builds.py.

WindowScene now adjusts root scenes before auxiliary views, shadows and the graphics bridge, and calls the native
camera SetAspectRatio (slot 0x68/4) so the world has the window's proportions. Existing horizontal FOV stays under
the game's/mod's control. Bridge state-block recreation runs with its tracking hooks quiet so placeholder states
do not become the game's cached viewport/fog. Tests read the actual raw BGRA back buffer, require varied nonblack
pixels throughout nine interior tiles, and check native scene viewport/camera aspect after every resize,
maximization, page close and forced-reset recovery. Pixel checks apply to the daylight city fixture, not arbitrary
dark scenes. Native image blocks were also inspected after the entire sequence in both builds, with DLSS active.

## Shared resources and settings

Reset releases the host's D3D9 default-pool targets, state blocks, motion, shadows, batching, skin/attribute buffers
and view targets. Keep the D3D11 device alive: mods and sidecars can still own textures on it. The usual next-frame
size checks rebuild the bridge, water and DLSS targets. New append-only DEVICE_RESET_BEGIN/END events let native
mods release their own D3D9 default-pool resources; END's value reports success, recreation should be lazy.

Window size is local to the process. Setup's registry and the saved rendering/water/atmosphere settings are not
changed. Native tests and backups are kept under build/window-resize-20261031; final results are in JOURNAL.

## Resize cursor visibility

The owner reported no resize arrows on hover, in gameplay and pause menus, although arrows appear while actually
dragging. Native probes in BOTH builds correctly hit-test all eight edges/corners and select the expected stock
cursor. Its display count remains -1: the game hides the Windows cursor while drawing its own. ShowCursor(FALSE)
is called once from InitSystem (1.2 Game.exe 0x5BFC7E through 0x6231E4; 1.0 0x5FA9E2 through 0x63B210). Both
calls were read in Ghidra; the imports have no other game callers. The Windows modal sizing loop temporarily
makes the cursor visible, explaining why dragging appeared to repair it.

The ordinary-window ResizeProc now selects the stock border cursor and borrows only the ShowCursor increments
needed to reach display count zero. Repeated hover messages do not accumulate increments. Client entry,
nonclient leave, deactivation, destruction and sizing-loop transitions return exactly the borrowed increments.
TrackMouseEvent(TME_NONCLIENT|TME_LEAVE) covers leaving the window directly. Client/game cursor behavior and
exclusive/borderless modes retain their existing policy. No real pointer movement or focus change is needed.

kit.resize_cursor tests actual WM_NCHITTEST/WM_SETCURSOR, stock handles AND visibility with an initially hidden
cursor, then checks balanced restoration on leave/client entry and unchanged input/focus. The initial display
count was -1 in both labs, with correct shapes but invisible arrows on all eight edges before the fix.

## Foreground cursor confinement

2026-11-08. The owner reported wheel-scrolling a background list under the hidden Windows cursor and selected
confinement rather than a global wheel hook. MouseInit (1.2 LS3DF+0x71CA0, 1.0 +0x6E120, read in both) passes
cooperative flags 6 for mode 0 (nonexclusive foreground) and 5 for mode 1 (exclusive foreground). A hidden
cursor with nonexclusive DirectInput remains a Windows pointer and can target another window for scrolling.

WindowDevice subclasses the primary root window for ordinary and bridge-borderless windowed devices.
ClipCursor uses the outer window bounds, keeping borders and caption controls reachable. The procedure releases
on deactivation, destruction, disabling and modal move/size/menu entry, not just at the next game frame. Placement
messages and WindowFrame refresh bounds. Only the actual foreground primary window can clip; native exclusive
fullscreen retains its own DirectInput policy. Release checks the current rectangle against the host's last clip
before clearing shared desktop state. No wheel-routing registry/system preference is changed.

[Microsoft's ClipCursor contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-clipcursor)
requires release when relinquishing control. tests/game/test_window.py tests actual confinement and immediate
release/restoration through the game's window procedure, without real input or pointer warps. The focused native
1.0 fix-only copy and focused 1.2 lab each pass all nine checks; a background 1.0 lab remains unconfined. The 1.2
kit.resize_cursor regression also passes all eight border shape/visibility and balanced visibility checks.

## System cursor after focus return

2026-11-11. A foreground 1.0 fixture first raised the display count to visible, selected an arrow and sent
30 client WM_SETCURSOR requests. The old procedure left a non-null system cursor on all 30. It only returned
its own borrowed resize-arrow increments, so an already-visible cursor was outside that restoration logic.
The exact external source of the intermittent visibility change was not established.

CursorClipProc now handles active primary-client WM_SETCURSOR by selecting NULL and returning TRUE. Focus
notifications and WindowFrame also reconcile it when the physical pointer is over that primary root/render
child's client, including a return without mouse movement. Actual foreground, suspension, visibility,
enabled state, client bounds and WindowFromPoint gate that repair. Other windows, borders and modal loops
retain their own cursor selection. ShowCursor display counts are not changed by this client policy.

This follows Microsoft's [SetCursor contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursor)
and [WM_SETCURSOR parent/return handling](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-setcursor).
An initially attempted synthetic activation-message fixture invoked native cursor warping and changed the
display count; it was replaced by client-selection requests without focus messages. Final checks use actual
foreground windows, eight real border hit tests and a stationary frame repair, with borrowed counts restored
exactly. Each lab passed 23 checks. The child render window must count as inside the game: an initial fixture
incorrectly required WindowFromPoint to return the root, despite the correct repair already occurring.
Evidence and source/runtime preimages: build/cursor-return-20261111.
