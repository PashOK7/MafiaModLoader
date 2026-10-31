# Water: what the game draws, and the sun over it

Findings behind `sdk/include/mml/water.h`, `mml/gfx/water.hlsli` and `mods/examples/river_water` (both builds).

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
