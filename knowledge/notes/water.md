# Water: what the game draws, and the sun over it

Findings behind `sdk/include/mml/water.h`, `mml/gfx/water.hlsli` and `mods/examples/river_water` (both builds).

The dated sections below retain earlier renderer findings and experiments. Current source integration and
unfinished acceptance gates are in [WATER_REFLECTION_PLAN.md](../../docs/WATER_REFLECTION_PLAN.md) and
[STATUS.md](../../docs/STATUS.md). In particular, the normal world-source/shore path supersedes the original
finite-image source description; later lighting/shadow/temporal/performance acceptance is still open.


## The game's water

- The city's river and the other water of the city's model `watercity` are drawn with the animated texture
  `00TEST01..20.BMP`: the river one plane of 54 triangles at y -9.99 under the whole city, a fountain at 2.41
  (`geom.survey` finds the names; `world.water` answers water everywhere over the plane).
- The shade under the bridges: the frames `watercity.stin01..18`, flat, 0.3 m over the river, blended to darken the
  game's water, with no texture. Over a water drawn anew they show as black and white bands: hidden with the water
  (`geom_hide "frame:water*.stin*"`).

## Two suns (2026-10-29)

- The world is lit by the scene's directional light (`light03` in the city by day: towards it (-0.24, 0.90, 0.36),
  65 degrees up); the lightmaps' shadows follow it.
- The sun a player sees is the backdrop sector's lens flare: two `I3D_lensflare` frames (vtable for I3D_frame: 1.2
  ls3d+9B5C8, 1.0 ls3d+97588) at the same place under the model `zapad` ("west") of `Backdrop sector`, their world
  position (-358.9, 192.5, 661.0): towards (-0.46, 0.25, 0.85), 14 degrees up, 55 degrees from the light.
- `I3D_lensflare::Draw` (1.2 ls3d+9890, vtable +0x90; 1.0 ls3d+9310, +0x8C) reads +0x264: 0 nothing, 1 a flare at
  its place (checked against the range at +0x274), else a flare infinitely far towards its normalised world
  position (+0x40); +0x234 its fade. The city's two: 8 and 3.
- The scene's sectors (I3D_scene's constructor, both builds): +0x210 Primary sector, +0x214 Backdrop sector, +0x218
  Omni sector, +0x21C Near sector; each sector's +0x214 its scene. A walk from the player's topmost frame (the
  primary sector) does not reach the backdrop's frames.
- So: what is seen in the sun (the glitter, light through crests, forward scattering) goes towards the flare
  (`scene_sun_seen`, frame.hlsli `g_sunSeen`); what is lit and shadowed as the world is, towards the light
  (`scene_sun`, the shadow map).
- Since 2026-10-30 water explicitly requests its shadow direction with `shader_shadows_from`: visible sun by
  default (`shadow_source=0`), scene light optionally (`1`). This changes the actual shadow-map camera, not a
  colour multiplier or a sampling offset. With no visible sun it falls back to the scene light. The shared map's
  most recent active explicit request wins; removing water's request restores the preceding source. Existing
  lightmaps and scene illumination remain as the game authored them.

## Pitfalls met

- The game's models end at the water: the city's rocks (`1SKALA4`) and a bridge (`0FMOST`) at y -10.00, 8 mm under
  the river's -9.99; the game's own water hid the cut. A planar reflection's clip plane must be the water's own:
  2 cm over it cut every foot out (close to the water a band of what lay behind showed between a thing and its
  reflection); 5 cm under it let the reflection camera look under the cut models at the sky (a dashed light line).
  At the plane itself the pixels at a foot are covered in part and some show the backdrop: a water point at the foot
  that reads one sees the sky. `WaterFollowReflection` fills such a hole with what stands up to three pixels further from the
  cut when that stands at the water point itself (measured: the dash pixels' flat reflection was sky at depth 100+
  with the water point at 39.6 m, the pixel beside it rock at 40 m).

- The owner's moving gap (2026-10-30, especially clear at 10 FPS) had a different cause: `RunPasses` ran DLSS
  before water. Pipeline mode returned the previous scene colour, then the water pass used the current depth,
  camera and reflection. The static contact-texel repair did not fix that temporal mismatch. Negative pass orders
  now run at the render size before the upscaler, and their premultiplied result is composited with the current
  scene colour into its input. Water uses -120. Upward motion verified in both builds at 10 FPS with pipeline on
  (1.2 DLAA, 1.0 Quality); the owner confirmed it fixed. No depth/motion replacement for the water surface yet.

- The city has no complete bed. Using the underlying frame as transmitted water colour exposed objects and made
  optical depth depend on arbitrary geometry. The example now uses one virtual vertical depth (default 10 m),
  divided by the refracted ray's downward component for the path length. No underlying colour, bed-depth foam or
  subsurface geometry shadows; the surface still receives sunlight and shadows. Both labs: a scaled bat visible
  above the surface disappeared below it. `prop.place` takes `pos`, not `at`.

- A planar reflection's clip plane does not cut the sky backdrop: past the line where the reflection camera's rays
  stop crossing the water plane it shows the sky. A blur that reaches across it puts a band of sky under every wall
  at the waterline (`WaterReflectionRise` leaves those taps out).
- A foam band set by depth alone (what is seen under the surface less than 45 cm) lights a bright wedge along every
  wall, rock and hull, on calm water too: foam at walls is the waves' work, its band now a few crest heights wide.
- Calm water has no glowing crests: the eye's refracted ray runs some 45 degrees down, so forward scattering and the
  sky seen through a crest need faces steeper than about 25 degrees (choppy, wind-driven waves).

## Actual shadow casters and 3D surface (2026-10-30)

- The legacy shadow mesh path omitted railway actors and alpha UVs and used scene snapshots refreshed at 0.25 s.
  The default now asks LS3D to draw current posed visuals and original alpha-tested materials every world frame.
- CullThruPortal overwrites orthographic visibility with perspective planes; the camera's own side planes also
  failed the inside-box test. A shadow-only mid hook supplies five inward box planes from the actual world and
  projection matrices, both to the camera (+0x404 normals, +0x440 distances) and CullThruPortal's globals. Hook RVAs
  1.2 0x445C9, 1.0 0x42839; normals 0x1B45A4/0x1B056C, distances 0x1B43F0/0x1B03B8. Verified disassembly and byte
  checks in both builds. Occlusion state is restored and the artificial light-camera LOD distance is removed only
  during the shadow render. Host-owned mid hooks must run directly: ModCallAnyThread rejects a null mod owner.
- An experimental pass leaked ZWRITEENABLE=0: distant city geometry drew over nearby surfaces. LS3D's
  ResetRenderProps state block does not restore that state. PassGame now saves/restores it explicitly; surveys of
  opaque main-world draws show zwrite=1 after shadow on/off cycles in both builds. Never rely on its cache here.
- The waves already had FFT height but only normals used it. The unused second FFT pair now packs Dx+iDz;
  Resolve exports height/Dx/Dz to a mipmapped texture. A grid-clipped mesh and hull/domain shaders displace the
  actual surface; its separate D32 depth buffer orders crests. Keep the base material xz for sampling normals.
  Horizontal movement is minus chop*D, matching WaterRipples' tangents (1-chop*dDx/dx etc.).
- The mean-plane reflection is still an approximation. Its contact depth pins displacement near cut wall feet;
  this is view-dependent and unavailable without a fresh reflection. The game's physical water remains flat.
  Water colour enters DLSS before temporal upscaling. Water now replaces its temporal depth/motion through the
  optional pre-upscale MRT: previous FFT displacement and camera, unjittered motion, matching city jitter.
  Shore attenuation uses the current reflection for previous displacement too (an approximation). No underwater
  renderer is supplied.

## Heavy bridge view: fixes and limits (2026-10-30)

- Hidden watercity.stin meshes were excluded from world/views but entered the unnamed shadow camera. They cast
  the apparent second shadow. Include ShadowViewNow in the auxiliary hide bit; do not recast projected shadows.
- The shadow path lacked a MirrorUse reference and read the entire device per caster: about 75521 extra Get*
  calls per frame in the saved view. Keep Set*-tracked state alive and upload only shader-read registers.
- A detached sound listener alone does not stop area/environment audio updates in I3D_scene::Render. Skip only
  that block for auxiliary renders: RVAs 0x624C9..0x62DBC (1.2), 0x5F219..0x5FB0C (1.0). Preserve the subsequent
  hardware-mirror preparation. Both blocks and byte guards were inspected; view.state exposes update counts.
- Fit shadow receivers by clipping the actual frustum to the water's conservative height slab. A rectangular
  orthographic camera reduces unrelated land casters; growing the texture keeps world density. In the held view,
  4096 texels over half-extents [240,60] retains the former maximum 0.1171875 m/texel. Hardware texture caps,
  camera far plane and geometry actually loaded by the game remain limits; this is not infinite-world rendering.
- Shadow HLSL is compiled by the build, including the diagnostic mesh path, avoiding game-thread compilation
  on map-size changes. Water GPU queries use a nonblocking ring; FFT and surface cost are reported separately.
- Static city batching is slower in this camera (+0.427 +/- 0.204 ms, 95% interval); leave it off. Remaining
  visibility/occlusion and shared-device waits require measured changes, not removal of synchronization.

## Painted sky and visibility follow-up (2026-10-31)

- The flare's direction is not evidence of an uncovered solar disc: the opaque backdrop paints clouds and the
  bright opening. The owner rejected a compact analytic solar spot, accepted reflection of the actual painting,
  then requested angular restriction. Current source expands only sky-depth texels in linear RGBA16F before
  roughness mips, preserving hue/cloud shape. A smooth cosine-cone support uses the visible sun anchor;
  sun_radius is its outer angular radius (18 degrees default), not the physical sun's angular size.
- The bounded inverse-tone highlight expansion is an explicit assumption: the lost HDR radiance is unknowable
  from a clipped LDR sky. Do not describe it as physically recovered light. Fully lit pixels use the HDR reflection;
  surface shadows retain the original reflection, blending the extra light only in penumbras. Object-depth
  texels are never expanded. The existing mean-plane reflection/contact limitations still apply.
- Conservative rejection applies the hull shader's expanded homogeneous side/eye tests to the original water
  triangles before submission. Their clipped patches lie inside them; no far-plane rejection is introduced.
  A rejected view skips reflection, FFT and the full surface pass. In-frustum occlusion still uses GPU queries.
- Query results carry a camera epoch and are ignored after a camera move; unknown visibility is treated as
  potentially visible. This prevents delayed reflection/FFT recovery from old zero counts. Changing displacement
  settings invalidates visibility too. An in-flight query is not reissued until completion; no CPU wait is added.
- Objects are not cloned for the reflected camera: the same LS3D scene/buffers/materials are drawn again from a
  different view/target. Ordinary instancing does not remove that second projection/visibility/rasterization.
  High wave height alone does not justify a coarser reflection: contact/smooth regions can still need fine detail.

## Receiver-only late shadows: native cache contract (2026-10-31)

- A D3D9 state block alone cannot surround I3D_driver::Render inside stage 1. LS3D caches 19 render properties
  outside the device. Read IGraph::ResetRenderProps in BOTH builds: 1.2 ls3d+6CCA0, 1.0 +69580. The cache addresses
  now live in game_build.h and check_builds compares every destination with that function's actual stores.
  Restore the original cached values alongside a full device state block; calling ResetRenderProps instead
  does not preserve the outer pass. The mismatch caused thick black font edges in the rejected first attempt.
- There is a second cache: IGraph::GetViewPort (slot 20, 1.2 +6E780, 1.0 +6AE30) returns LS3D's six-dword
  D3DVIEWPORT8, NOT a device GetViewport result. Render changes it for the auxiliary target. Restore all 24 bytes
  with the device viewport. Otherwise the HUD's radar saves/restores the shadow-map viewport and shows a rectangle
  containing the main scene. Both effects reproduced separately and corrected in native images of both builds.
- A depth-only child must use scene callback mask 0. A stage-1 callback clears Render's global in-callback flag
  when it returns, prematurely ending the enclosing BeginScene. No mod/world callbacks belong to a shadow camera.
- The near sector (scene+0x21C, visual count +0x124, both Render decompiles) is consumed after stage 1. Late shadows
  are disabled when it contains visuals; first-person/near queues retain the established pre-world pass. World
  shader rules, surveys and graphics passes inside auxiliary views also retain early shadow availability.
- Current opaque-depth reduction intersects conservative per-level finite receiver boxes, including displacement,
  then reduces light-space endpoints on the GPU and reads 28 bytes in the SAME frame. No previous zero counts,
  culling prediction or shadow update-rate reduction. Failure falls back to the finite/frustum coverage.

## Wavy water with sharp reflections (2026-10-31)

The owner supplied Chicago river photos and an in-game view: visible waves should be able to distort recognisable
reflections rather than immediately turn them into broad smears. The old controls coupled all scales: MakeSpectrum
allocated 85% of its wind-driven slope energy to FFT waves and a mandatory 15% to unresolved roughness. WaterRipples
adds mip-filtered slope variance too; WaterReflectionSpread turns it into a broad reflection lobe. Wind/wave height
alone could not control large waves independently from fine roughness and artistic reflection filtering.

- SDK MakeSpectrum appends optional smallRipples=1 and smallWavelength=0.25 m. SmallWaveGain preserves wavelengths
  >=0.5 m exactly, scales <=0.125 m by smallRipples, and smoothly transitions in log wavelength between them.
  Removed energy is NOT renormalized into the retained long waves. The unresolved residual scales by amplitude
  squared; actual drawn slope/height statistics track the filtered spectrum. Existing calls default to the old
  spectrum. This is a controllable band filter, not a measured urban-river surfactant simulation.
- Water > Small ripples controls that amplitude (0 smooth short-wave band, 1 legacy, up to 2). Wind/Ripples/3D
  waves still determine the retained larger moving waves; smallRipples=0 does not make them flat.
- Water > Reflection softness is a separate ARTISTIC multiplier of reflection-lobe width. 1 retains the old
  statistical width, 0 removes that extra streak/blur while keeping ordinary planar texture-footprint mip filtering.
  It changes no normals, displacement, Fresnel, fog, spectrum or glitter. Narrowing the lobe is a deliberate
  departure from the full unresolved-wave BRDF; extreme distorted normals can still alias. It is not a claim of
  physically exact antialiasing. The SDK helper accepts pre-branch planar UV derivatives; no forced coarse mip
  or low-resolution reflection is used as a performance shortcut.
- A 3 m/s spectrum at smallRipples=0 retains >90% of height variance and its 1.09 m peak; drawn slope variance
  falls from .003060 to .001115, residual per axis .000270 to zero. CPU invariants and the actual WARP sky shader
  pass in water_sky_test. Native 1.0 material/reload/pixel case: 12 passed. Current 1.2: four checks passed,
  including identical wave statistics when changing only reflection softness and untouched other owner settings.
  Both new sliders are visible on the first Water page. Full offline suite ALL PASSED.

The current city's haze reduces reflected-object contrast too: changing material roughness does not undo
atmospheric extinction or invent the clear, high-contrast Chicago lighting. Owner's 1.2 preview uses both new
controls at zero, retaining their wind 3, Ripples 2 and wave height .5 and all other existing settings. Shipped
defaults remain 1/1. Source/binary/config backups and test evidence: build/water-roughness-20261031.

## Wave width, speed and editable material (2026-10-31)

The owner then asked for wider waves, independent speed, color, turbidity and mirror strength. The formulas already
use wind/fetch for JONSWAP's dominant wavelength and energy; individual modes use gravity/capillary dispersion
`omega=sqrt(9.81*k+7.4e-5*k^3)`. Wind is not a direct animation-time multiplier. Fixed cascade lengths and missing
UI controls made these relationships difficult to tune. Material body/absorption colors existed as hex settings
but had no native controls; reflection softness changed width, not the reflection share.

- SDK Spectrum now carries each cascade's world length. ScaleWavelength stretches these lengths while retaining
  h0 and height variance: peak scales by width, slope variances by its inverse square. FFT dispersion/derivatives
  AND all surface/geometry sampling use the same lengths. The short-wave band's world cutoff stretches too.
  Mod Wave width is .25..8x (default 1); Wind fetch exposes the existing physical fetch setting separately.
- Wave speed 0..4x (default 1) uses SDK WaveClock, integrating elapsed time with the previous speed before a setting
  change. Speed adjustments do not multiply the accumulated absolute time; zero freezes the phase. Foam keeps its
  own wall time. This is an artistic animation rate, not a river current or a new fluid simulation.
- Reflection strength 0..1 multiplies the Fresnel reflection share; default 1 preserves the previous material.
  Strength zero shows the body, independent of Reflection softness (lobe width). Body color, Absorption color,
  Clarity, Turbidity and virtual Depth are in Material. Depth remains a uniformly deep opaque slab, not captured
  bathymetry or scene refraction. Lighting controls and waves occupy their own tabs.
- Shared panel kind `color` expands a #RRGGBB setting into native RGB sliders (0..255), preserving other channels
  and the original key/string representation. No water-specific host rule or ABI change. Invalid live values
  fall back to the manifest default; invalid defaults are rejected. panel.state reports color_channel 0/1/2.
- CPU invariants verify unchanged heights/h0, scaled wavelengths/slopes and continuous/frozen phase. Actual native
  tests verify frozen/moving normal pixels, stretched waves, red/blue body pixels, reflection strength, RGB slider
  writes preserving other channels, three tabs and hot reload: 15 checks in EACH build. Exact pixel freeze must
  temporarily bypass DLAA: temporal jitter/reconstruction changes postprocessed pixels of a static surface.
  The original strict post-DLAA failure is retained under build/water-controls-20261031. Owner DLAA is restored.
  Full offline suite (including generic color layouts and malformed defaults) ALL PASSED.

## Reference-driven optical iteration (2026-10-31, not visually accepted yet)

See the JOURNAL entry and build/water-reference-20261031 for the fixed native camera, preimages, rejected blue
prototype and current comparison. Passing numeric tests is not the appearance gate. The owner requires the
agent itself to compare color, bright/dark facet structure, foam and recognizable bank reflections with the
generated reference; do not ask the owner to enumerate differences already visible in those images.

Current mechanisms: optional unnormalized gravity-wave energy; exact dielectric Fresnel; projected-area-weighted
Gaussian facet quadrature; per-facet reflected radiance and atmosphere; bounded neutral-brightness expansion of
the painted sky; projected front-to-back depth tracing; compression-based crest foam; remaining near-clear path
budget across a bounce. All are shared SDK math except the example's artistic palette/sky reconstruction.
The SDK spectrum's default remains legacy; the example opts out of global slope normalization. No extra UI knob:
only Waves and Material are shown; old advanced keys are preserved in config. Full environment coverage, wave
multiple scattering and reflected-object motion under DLSS remain limitations, not solved by these changes.

## Lighting architecture audit (2026-11-01)

Owner accepts current wave geometry; rejects lighting, camera-tilt reflection artifacts and broad solar glare.
Read docs/WATER_LIGHTING_ARCHITECTURE.md before continuing optical work. It supersedes the earlier acceptance
language about painted-sky solar support: WaterGlitter is not called by the current material; a global up-to-16x
cloud expansion plus an 18-degree solar multiplier supplies the highlights. This is not a solar BRDF.

The planar scene is really rendered, but its finite projected depth/color is not a directional environment.
WaterFollowReflection forces forward denominators positive and returns edge UV/sky on an unknown march miss.
Native fixed-world-ray coverage changes with camera tilt even with DLAA off and waves frozen: elevation 5 degrees
is covered at camera depression 10 but not 45; elevation 60 behaves oppositely. Sampling the border cannot repair
that missing information. A sky cube alone also cannot supply missing off-frustum buildings.

WaterBody has a colored source with sigma_s=0; WaterSunScatter already includes resolved exit transmission before
the final outer body/transmission blend. Coupled medium transport and one interface-accounting convention are
needed. Preserve wave geometry and the compact UI. Host/SDK directional environment and typed reflection validity,
separate solar BRDF/exposure, then absorption/scattering transport are the next implementation blocks, with
C/C++/Rust lifecycle and both-native-build gates. DLSS reflected-radiance history remains queued separately.

This turn audited/captured the installed C4386058... DLL, without deploying the E4FAB642... source radiance patch.
Existing WARP tests pass but omit these architecture invariants. Evidence, source/config preimages and native
camera snapshots: build/water-lighting-audit-20261101. No claim of completed lighting or reference acceptance.

## Native mesh-source preparation boundary (2026-11-01, in progress)

The explicit source collector initially called m_AddFGroups then consumed m_RenderFGroup immediately. Native
Render first publishes pending pooled-buffer writes through BOTH UnlockAll managers. Skipping this boundary
is invalid even when a raw D3D9 READONLY Lock succeeds: source/index bytes or vbrename's current copies may be
stale. The initial 1.2 pitch/yaw gate failed (unavailable packets varied, flags16/FVF142, no skin/shader flags).
Subsequent diagnostic VB/IB locks succeeded (usage8); the publication fix still requires native verification.

Verified Render sequences: 1.2 +630B4 loads ECX=101C5014, calls +697E0; +630BE loads ECX=101B50F8, calls +5B960.
1.0 +5FDC0 loads ECX=101C0FC4, calls +66250; +5FDCA loads ECX=101B10B8, calls +58960. All thiscall(manager), no
arguments. 1.2 managers drain locked-buffer lists; 1.0 walks its pool trees. New sourceVb*/sourceIb* table fields
are separate from the older allocation ibManager contract (absent in 1.0). check_builds verifies native call/ECX
pairs; Rust game tables regenerated. Preparation copies no scene image and rasterizes no triangles.

The empty fast path is specific to unmodified 1.2 functions: +697E0 reads
the VB pending count at manager+8 and jumps to +69812 if it is zero.
+5B960 checks the IB count at the same offset and jumps to +5B9B7.
Each taken branch only stores zero back to that field and returns.
The test host verified those entry and return bytes at collection start.
It skipped about 3,800 empty IB publications per frame and no VB
publications in the river scene. The unchanged FPS led to removing
the shortcut and restoring both native calls for each source visual.
1.0 uses a tree of pooled buffers with per-node lock pointers, so the
count-field shortcut is unavailable there.

Core frame/frames.find reads cached world matrices; an off-camera animated barge child's displayed cached pose
can be old while its parent moves. Native GetWBoundVolume/WorldMatrix preparation updates it. The 1.0 tight-box
barge source check refreshed a z=-459 cached child to current z=519, emitted 30 packets/996 triangles with zero
unreadable geometry. This source-data check is distinct from reflecting those triangles in the water shader.

The publication boundary alone did not fix the camera-dependent source refusals. Step diagnostics located
an out-of-range native index (19 for a declared 18 vertices). The collector had returned 0 from GetObjectLOD
without calling it. That function is NOT pure: BOTH builds store the chosen level into object+20C (1.2 +61E30,
1.0 +5EB80); m_RenderFGroup reads it to pick the buffer/count. Thus AddFGroups prepared LOD0 while rendering
consumed the last main-camera LOD. Call the original with distance0. During source preparation suppress native
LOD callbacks (frame+AC bit400), record each queried visual's current LOD and vis_flags+1D8 bit2000000, then
reverse-restore them on every exit. Proxy/delegated visuals may be queried repeatedly. Never relax the index
range check to conceal this mismatch. After this correction all CPU packets read successfully; native source
collection tests pass 16 in EACH lab, including 1.2's failed pitch/yaw gate. The water shader is not integrated yet.

## Owner-provided MafiaVoda.c review (2026-11-01)

Read-only source review of W:/Downloads/MafiaVoda.c, SHA256
842F3EA331ABC4EEEB189C4228A8C48F3CCB141945D5A2349B4753EADA7F8C45. Not built or installed;
visual quality/performance are not measured. This is a separate 1.2-only D3D8/dgVoodoo D3D11 water effect,
not a world-reflection implementation. No third-party source was copied into the repository.

- WATER_HLSL psw, lines 367-476: intersects a FLAT water plane; WaveH only perturbs the shading normal.
  Four sines plus value noise replace neither our displaced FFT geometry nor its unresolved slope statistics.
- Lines 405-446: reflects the current camera's sky pixels and ray-marches its depth/color (40 growing steps,
  5 bisections). Screen-edge/distance fades hide missing coverage; hidden/off-screen objects remain unavailable.
  Hit tolerance max(.6, distance*.3) is a depth heuristic, not a triangle intersection.
- Lines 400-403 force R.y=max(R.y,.02), then normalize. This avoids downward reflection rays by bending
  them into the sky; it does not solve water-facet self reflection/masking. Our current skip of downward
  rays is ALSO incomplete and causes black streaks. Neither behavior is the desired transport solution.
- Lines 449-460: refracts the already-visible bed by an offset into another marked water pixel and applies
  exponential RGB absorption. Useful reference for a bounded visible-bed fallback, not hidden bed recovery
  or Snell-law scene tracing. Body color is an ambient-scaled empirical source, not particle scattering.
- Lines 464-468: Schlick Fresnel plus an independently added pow(dot(R,sun),power) highlight. Sun direction
  comes from settings and color is fixed (1,.86,.62), not the bright cloud opening or a normalized light model.
- RainRipples (347-364) offers a compact world-positioned rain-ring idea. Up to eight detected water planes
  are marked in target alpha; the mask/replay plumbing is separate from illumination. These are potentially
  useful features, not reasons to replace the current reflection architecture. No foam model is present.
- Integration targets dgVoodoo D3D11 with direct shared-vtable hooks, 1.2 addresses and background polling
  threads. It has no loader owner lifecycle, 1.0 contract or water motion/depth output for our temporal path.

Decision: retain the planned world geometry/material query. Do not import its SSR, upward-ray clamp, arbitrary
sun highlight or flat normal-only water as fixes for the owner's existing coverage/black-streak reports.

## Wave visibility and the black-band correction (2026-11-01)

Native debug19 reproduced two lost-radiance mechanisms: a water reflection traveling below the global mean
plane was simply skipped, and the fine FFT normal could face away from the eye while the coarser tessellated
surface still covered the pixel. Both paths returned essentially no reflected/body light. This is independent
of the incomplete projected OBJECT reflection query; increasing its texture size cannot repair these zeros.

SDK gfx/water_trace.hlsli now brackets air-to-water crossings in a caller-supplied world height field, with
explicit escaped/hit/unresolved status. It does not use a camera matrix, framebuffer or depth image, and never
bends a ray upward. The river evaluates its current three FFT displacement cascades, including horizontal
chop inversion (three fixed-point iterations). Existing slope/shape derivatives supply the hit normal through
shared WaterSurfaceSamples. A fine back-facing first point queries a nearer continuous-water hit; downward
reflections may take two further water interactions, with Fresnel attenuation and medium scattering at each.
Corrected first hits write their own hardware/temporal depth and previous material-point motion. The existing
planar object lookup is applied from the continued ray origin; it is NOT upgraded to a world geometry query.

Limits: finite adaptive marching (96 steps, 7 bisections, secondary range40m), height envelope from the existing
wave bound, three-iteration inverse chop, and two additional reflections. These are bounded approximations,
not conservative proofs for arbitrarily narrow/folded/self-overlapping waves or full water multiple scattering.
Unresolved downward paths do not become invented sky. Only the near-horizontal/downward lost-ray family is
continued here; general upward-ray occlusion by other waves remains a separate completeness improvement.
The raster mesh still owns coverage/silhouettes. No normal smoothing, sky floor, blanket brightness offset,
wave dimension change, new tuning slider or DLSS reconstruction change was used to hide the bands.

Four stratified visible-normal samples reduce per-path query cost (the previous eight retained in developer
debug21 for A/B). Exact native frozen-phase RGB tests, DLSS temporarily off, 1440x810, sun-facing low camera:
1.2 nearly-black pixels 8923 -> 591 /186624; 1.0 15553 -> 233 /186624. All five checks pass in EACH lab, including
unchanged wave field and flat-water radiance within one byte, not just synthetic geometry counts. Remaining
dark pixels are counted, not claimed universally fixed. Manual low/steeper native views inspected too.
Actual WARP tests cover upward escape, flat/wave first hits, horizontal next crest and finite-search unknown,
alongside existing Fresnel/medium/energy/localized sun cases. Full x86/offline suite ALL PASSED.

Installed water SHA256 BOTH labs: 896F21FA9450F6445B33E2FB1A0DDBC696D80DF40ACBEF8196EC53C721D74824.
Single-lab low-water 1.2 GPU surface samples: old branch 9.747/9.247ms, new 14.521/14.885ms (same phase/pose,
DLAA on). This is a roughly +5ms cost, not an optimized final implementation or an FPS benchmark. Evidence:
build/water-artifacts-20261101/black-wave-*.log, source/lab DLL preimages in black-wave-preimages.
