# Native draw distance and fog controls

2026-10-31. Both builds read in Ghidra; host/render.cpp and host/settings.cpp.

| Function | 1.2 | 1.0 | Convention |
|---|---|---|---|
| C_cache_base_block::IsBlockInRange | 0x402F90 | 0x5A93C0 | thiscall(cache, block, x, z, radius), ret 16 |
| C_cache_base_block::DeactivateBlock | 0x402A50 | 0x5A8E80 | thiscall(cache, block), ret 4 |
| setNewFogState | LS3DF+0x4E390 | LS3DF+0x4BE00 | fastcall(state, type, start, end, color), ret 12 |
| FogProcess | LS3DF+0x685D0 | LS3DF+0x65180 | fastcall(alpha, vertices, stride, count, light, mode), ret 16 |

CacheTick's activation radius is 200 m (550 m in 1.2's larger-distance mission mode), deactivation 230/580 m.
IsBlockInRange performs the native circle rejection and exact block quad tests. Custom distance retains the
native CacheTick (pool cleanup, cadence, free-camera translation and activation); changes its range-test radius;
suppresses only that tick's old-radius deactivation and reconciles active blocks with the same native quad test
at radius + 30. Other DeactivateBlock callers, including mission teardown, are untouched. A conservative sphere
around all far-frustum corners supplies the horizontal radius, so a tilted/wide freecam cannot expose unloaded
corners. Native near LODs remain; the last LOD stops fading out before the chosen camera far plane.

The native linear fog light stores near/end PERCENTAGES: I3D_light::SetRange (1.2 +0x20B20, 1.0 +0x20650)
writes near_range/far_range at +0x168/+0x16C and scales them by .01 into +0x19C/+0x1D0. Native rendering uses
end = camera.far * light[0x1D0], start = end * light[0x19C]. FogProcess repeats exactly that for software vertex
fog (mode 0: visibility alpha; mode 1: its inverse). Merely overriding D3D fog misses this path.

Custom fog overrides the native setter's enabled requests inside the world's Render, retaining its colors and
disabled requests (HUD, sky, fog-free sectors). The software path borrows the two derived ratios for that call
and restores them immediately; off writes the native no-fog vertex factors. Returning to Game removes both
hooks, leaving no additional per-draw hook. Native IGraph's cache remains authoritative throughout.

Options > Mods > Rendering (loader): Distance, Fog, Advanced tabs. Distance modes 0 Game / 1 Custom / 2 Whole city;
custom metres 100..10000. Whole city retains the established 5000 m plane, every cache block and final LODs.
Occluders are independent. Fog modes 0 Game / 1 Off / 2 Custom; start and fade distance are metres, end = sum.
Saved in mml/data/loader/config.json. Legacy whole_city true migrates once to distance_mode 2; explicit new mode
wins on later starts. Direct render.set remains a temporary override until its corresponding saved setting changes.

Foam's saved multiplier remains unchanged; the UI range is now 0..2. Thus old 100% (1) displays 50%; new 100%
is multiplier 2. Final whitecap coverage saturates to [0,1], preventing blend extrapolation in the extended range.
