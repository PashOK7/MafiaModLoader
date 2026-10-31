# Review of mafia-ragdoll water work

2026-10-31. Read-only review requested by the owner of `W:/github/mafia-ragdoll`, compared with the current
MafiaModLoader river renderer. Sources are the current working files, not a released or benchmarked package.
The sibling repository has no commits; its WATER document describes an evolving prototype, including disabled
layers and outstanding acceptance items. No sibling sources, installed binaries or live game state were changed.

## Useful differences

| Mechanism and source | Current loader | Value and cost of a possible port |
|---|---|---|
| Gaussian reflection pyramid and elliptical weighted filtering: `src/shaders/reflection_pyramid.hlsl`, `src/shaders/water_surface.hlsl` ReflectionAt around line 998, `src/water_surface.cpp` BuildReflectionPyramid around 1243 | Generic generated mips, eight mostly vertical taps in `sdk/include/mml/gfx/water.hlsli` WaterStreakedReflection | Better control of grazing-angle aliasing and the actual pixel footprint. The covariance includes ray differentials and unresolved slopes, with up to 15 taps. Additional compute/memory work; not an established speedup. Preserve contact-foot guards and temporal phase when adapting it. |
| Full slope covariance and variance-preserving stochastic tiling: water_surface.hlsl SlopeStats/RippleCascade around 874/909 | Mean and diagonal second moments already present in WaterRipples; periodic FFT lookup, no stochastic tiling or current advection | Can reduce repeated patterns and account for the reflection ellipse's direction. Naively adds many texture reads; the sibling documents about 1.7 ms for its 36-read ripple path. If adopted, evaluate a camera-local field once, including moments and displacement history, rather than multiplying work per water pixel. |
| Shore distance/normal map from geometry: `src/shaders/water_map.hlsl` CsShore around 102, `src/water_surface.cpp` BuildMaps around 1140 | ShoreWeight in river_water.hlsl uses reflection depth; depends on the current view and available reflection | Strong correctness candidate: independent contact attenuation, foundation for wet stone/foam/fetch. Fine map is 1024 over 512 m (0.5 m texels), moved in 64 m steps; cached until geometry/settings/origin change. Requires actual scene/water intersections, separate levels and conservative bounds, not just a water-polygon boundary. Retain the owner's uniform virtual depth. |
| Geometric clipmap, `src/water_surface.h` line 351 and water_surface.cpp DrawIndexedInstanced around 1540 | Exact captured footprint, 16 m clipped patches, GPU frustum rejection and distance tessellation | The architectural idea scales with view extent, but this implementation submits 128*128*2*9 = 294912 triangle patches before hull rejection, versus our recorded 91946. Not evidence of a faster replacement. First test conservative spatial batches of our existing triangles before tessellation, with the same visible geometry/detail. |
| Steady current/turbulence, `src/river_flow.cpp` and WATER.md River current | Wind-driven surface; no river-current field | Worker solve and versioned disk cache, flow around islands/piers, visual ripples and a basis for wakes. A substantial new feature, not a solution to current frame cost. Its bed depth/friction are modeled assumptions; do not confuse them with a real river bed supplied by Mafia. |
| Supersampled offline reference, `bench/water_render.cpp` lines 1..10, WATER.md Benches | Native game checks, live device counters and saved camera; no isolated supersampled water reference | Useful way to judge shimmer and quality before accepting an optimization. Fixed seed/time/camera, subpixel movement and supersampled reference complement, rather than replace, both-build live tests and DLSS checks. |

## Mechanisms already present or unsuitable for a direct replacement

- Both have three 256x256 FFT cascades, actual displaced tessellation, unresolved-wave roughness, planar reflection
  and asynchronous water-visibility queries. These are not new optimizations to port. Cascade scales and the
  resulting appearance differ; preserve the owner's chosen wind, wave height, colour and glare.
- The sibling's BuildShadow around water_surface.cpp 934 uploads a position-only caster buffer and binds no
  pixel shader. It rebuilds on dirty caster data/light/map movement. It cannot preserve our material alpha tests
  by itself. Keep the live LS3D posed/material caster path, complete railway parts and fitted visible receivers.
- Reflection margin 1.25 widens both target dimensions and the view; it helps rays bent beyond the screen, but
  increases target area by 1.5625 and can add more scene work. It is not a free performance option.
- Its own WATER.md still lists reflection culling restricted to visible water as unfinished. There is no completed
  solution there for our measured LS3D visibility/occlusion cost or required Direct3D 9Ex/11 ownership waits.
- Subsurface transmission, buoyancy, liquid impacts and spray are separate layers. Do not reintroduce visible
  submerged city geometry, an arbitrary real bed, or physics into the currently accepted opaque visual water.

## Proposed order within the existing optimization work

1. Establish isolated surface geometry/filter timings and a fixed visual reference. Test spatial rejection of
   existing water patch batches before tessellation; retain exactly the same displacement, history and coverage.
   The measured surface budget was 1.635 ms and FFT 0.192 ms, so even eliminating those cannot explain the whole
   frame. Keep engine shadow visibility, reflections and device handover in the performance plan.
2. Adapt the reflection-footprint/Gaussian filtering as a quality experiment, with before/after cost and camera
   motion comparisons. Preserve the fixed waterline and DLSS inputs. Accept only a demonstrated benefit.
3. Build a stable shore map if tackling view-dependent contact attenuation, wet edges or fetch. Then current,
   stochastic ripples and wakes can be separate features with independent settings and costs.

No performance gain is claimed from this source review. No code was ported or runtime test required for the
documentation-only change. The existing renderer and the owner's free camera remain as handed over.
