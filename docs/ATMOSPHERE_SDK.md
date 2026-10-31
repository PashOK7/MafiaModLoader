# Atmosphere: shared renderer service, small policy mods

Read this before extending fog, weather, aerial perspective or water/fog integration. Both builds use the same
public contract. Start with `sdk/include/mml/atmosphere.h` and `mods/examples/atmosphere/atmosphere.cpp`.

## The boundary

| Layer | Owns | Does not own |
|---|---|---|
| Host (`host/atmosphere.cpp`) | Validation, copied requests, priority/ownership, frame snapshots | Weather presets, automatic palette choices, a particular mod id |
| Host renderer (`render.cpp`, `gfx.cpp`) | Native fog suppression/restoration, current depth, composition before DLSS | Mod configuration or artistic tuning |
| SDK (`mml.h`, `atmosphere.h`, `gfx/atmosphere.hlsli`, Rust `gfx.rs`) | Stable C ABI, safe Rust methods, shared HLSL, explicit units | Game addresses in a mod |
| Example mod (`mods/examples/atmosphere`) | Settings/UI, fade-distance-to-extinction conversion, ambient palette and presets | Hooks, shaders, GPU resources, DLSS logic |
| Water mod | Material/waves; fogging its outgoing reflection leg with the SDK helper | A private fog model or camera-to-water double fog |

The example is deliberately disabled by default, as other examples are. Enable it before starting a game
(`"gfx": true` requests readable depth/MSAA-free device creation). The host has no dependency on this example:
any C/C++/Rust mod can request the same service with explicit parameters.

## C and C++

```c
#include "mml/atmosphere.h"

/* During load, FRAME, CONFIG_CHANGED or world RENDER_BEGIN; always on the game thread. */
if (!MML_HAS(host, scene_atmosphere_state)) return 1; /* old loader */
MmlAtmosphereSettings air = mml_atmosphere_defaults();
air.extinction = 0.001f;      /* per metre at/below base_height */
air.base_height = -10.0f;     /* world Y in metres */
air.scale_height = 70.0f;     /* 0 = uniform; otherwise exponential falloff above base */
air.color[0] = 0.72f; air.color[1] = 0.76f; air.color[2] = 0.80f; /* display-space RGB */
air.sun_scattering = 0.5f;
host->scene_atmosphere(mod, &air); /* copied now; no retained pointer */

/* Off or explicit release. Unload and fault release automatically too. */
host->scene_atmosphere(mod, NULL);
```

Do not submit uninitialized structs. The helper fills the complete versioned `size`, clear-air extinction,
neutral color, sky range/blend, opacity limit and phase parameters. Set the values your weather needs. The ABI
accepts larger future struct sizes but refuses undersized/nonfinite/out-of-range requests without replacing
the owner's existing request. Color is display-space; the renderer integrates/blends in linear light.
Positive scale heights must be at least 0.001 m; zero selects uniform density. This prevents reciprocal overflow
from subnormal inputs while preserving useful world-space heights.

## Rust

```rust
let mut air = mml::AtmosphereSettings::neutral();
air.extinction = 0.001;
air.base_height = -10.0;
air.scale_height = 70.0;
air.color = [0.72, 0.76, 0.80];
air.sun_scattering = 0.5;
assert!(host.scene_atmosphere(Some(air)));
let state = host.scene_atmosphere_state().unwrap();
assert!(state.requested != 0);
host.scene_atmosphere(None);
```

The Rust API wraps the same ABI; ownership is by mod instance, not Rust object lifetime. Use the host-table
capability check when targeting older loaders, as for other optional SDK additions. Call from game-thread events.

## Ownership and timing

- One copied request per mod. Highest integer priority wins. At equal priority the most recently ACQUIRED
  request wins; updating an existing request retains its acquisition order. Per-frame palette updates therefore
  cannot repeatedly steal an equal-priority request. To reacquire intentionally, release then request again.
- Disabling/unloading/faulting an owner reveals the preceding request, or the configured native Game/Off/Linear
  baseline if none remain. The example never overwrites the loader's native fog settings.
- Requests survive scene changes; the host clears only the old scene's effective snapshot. The mod can keep a
  weather preset across missions and update its palette when the next scene appears.
- World RENDER_BEGIN callbacks run before the service snapshots parameters and before shadow/reflection cameras.
  All consumers of that world frame see the same snapshot. A request made later takes effect next frame.
- `scene_atmosphere_state` reports the caller's request/selection, selected owner/settings and bridge readiness
  from the last frame boundary. For actual uploaded values inspect `gfx.state.world.atmosphere`.
- If readable graphics are unavailable, the request remains owned but native fog is retained. `ready` stays false;
  a graphics consumer enabled after an MSAA device was created may require restarting the game.

## Agent/MCP access

`atmosphere.state` lists owners, acquisition order, priorities and selected parameters. `atmosphere.set` is a
separate temporary BRIDGE request, useful for experiments; it never changes a mod's request or saves settings.
Its omitted parameters retain its own values, even while another owner wins. `{"on": false}` releases it.
Mods call the SDK directly, not the bridge method. Use `config.set {mod:"atmosphere", key, value}` to tune the
example persistently, or its Options > Mods > Atmosphere page.

Example bridge experiment:

```json
{"method":"atmosphere.set","params":{"priority":100,"extinction":0.002,"scale_height":60,"base_height":-10,"color":[0.72,0.76,0.80]}}
```

Release the bridge request after experiments so it cannot mask the weather mod.

## Water and other replacement surfaces

The bridge fogs the COMPLETE camera-to-surface pixel after negative-order replacement passes and before DLSS.
Supply actual replacement depth through `MmlGfxFrame.temporal_target`, including without an upscaler. Water
already does so with its displaced wave depth. Its body, foam, glitter and reflection then attenuate together.

For a reflected ray, call `ApplyAtmosphere(linearColor, origin, unitDirection, distance, false, isSky)` in the
material using the traced hit distance (or `g_atmosphereInfo.y` for sky). The bridge handles the incoming camera
leg once; do not apply that leg again in the material. Main and reflection share the same b0 constants and HLSL.

This is analytic single-scattering aerial perspective, not shadowed volumetric ray marching. Transparent surfaces
without their own depth inherit opaque depth behind them; rough reflection filtering uses its representative hit.
The painted sky has a separate horizon blend/falloff to retain existing clouds. These limits are explicit, so an
agent extending them can change the shared mechanism rather than patching a particular water shader privately.

## Migrating the old loader setting

The old experimental `loader.render.fog_mode = 3` and `fog_density/height/base/sun/tint` are no longer a host
weather feature. Deploy the new host, water and `atmosphere` example with the game CLOSED, then:

```bat
python tools/migrate_atmosphere.py --game 1.2
python tools/migrate_atmosphere.py --game 1.2 --apply
```

Use `--game 1.0` for its own lab. The first command only shows the plan. Apply validates SHA-256 preimages,
backs up exact files under that game's `mml_lab/atmosphere-sdk-<timestamp>/`, copies old artistic values to the
example, enables its manifest, and leaves native linear as the fallback. Existing destination settings win.
Distance/occluders and unrelated settings remain unchanged. Completed migration is a no-op; no game assets
are touched. If a write fails, stop and restore the named config/manifest preimages from the backup before retrying.

## Extending and validating

1. Add general parameters to the versioned ABI and host validation/snapshot, not a dependency on an example id.
2. Append optional host-table functions; preserve the old prefix. Regenerate `tools/rust_sys` and add safe methods.
3. Share shader math in the SDK; keep weather defaults/palette logic in a mod. CMake tracks SDK HLSL dependencies.
4. Update PROTOCOL and run `tools/gen_methods.py` for bridge changes; keep this guide, roadmap and journal current.
5. Run `atmosphere_test` (actual WARP integral and displaced-water composition), the offline SDK ownership/fault/
   migration tests, and native `atmosphere`/`render_controls` tests in BOTH labs. Verify hot reload and unloading
   restore the previous owner/native baseline, both DLSS paths, saved settings, and user's camera/preferences.

The renderer also supports future mods with explicit colors/densities and other weather policies without changes
to the host or water. No game's files, decompiles, screenshots, dumps or local benchmark artifacts belong in Git.

For fast service iteration after building the harness, `testmod_kit`, `rust_kit_rs` and `mod_atmosphere`, run
`python tests/atmosphere_sdk_smoke.py`. It exercises plain C, C++, safe Rust, arbitration, invalid/thread/future-size
requests, faults, reload and migration without a real game or the full suite's temporary-mod build cycle.
