# Atmospheric fog over city and water

2026-10-31. Public owned SDK service: host/atmosphere.cpp validates/arbitrates/snapshots explicit parameters;
render.cpp suppresses/restores native fog and gfx.cpp composes before DLSS. The policy-only atmosphere example
owns UI, presets and ambient tint. Read docs/ATMOSPHERE_SDK.md before extending this feature.
SDK gfx/atmosphere.hlsli shares the integral with river_water.hlsl's outgoing reflection leg.

Density is constant below base Y and exp(-(Y-base)/scaleHeight) above it. The exact line integral splits any base
crossing; the horizontal limit uses a series for (1-exp(-q))/q. The example maps extinction = 3*densityMultiplier/fadeDistance,
so the fade slider is approximately 95% opacity at base height (before directional lighting). Clear distance is
excluded only on the camera leg. The example's tint follows scene ambient lighting or Cool/Warm/Neutral; the default phase 0.55 uses
the sun the sky shows. The upper painted sky is retained; haze merges its horizon with the world.

Native hardware/software fog is suppressed for a ready selected SDK request; otherwise the configured native
Game/Off/Linear baseline remains. Priorities, acquisition order and automatic unload/fault cleanup are host services.
Pre-composition reads water's displaced depth/motion target, falls back to opaque depth where no replacement
exists, blends in linear light, and enters DLSS with the same current-frame geometry/jitter. Without DLSS, the
RGBA16F composed frame seeds the ordinary post effects. No additional world render, half-resolution blur,
stale depth or temporal noise. Reflection fog integrates the representative traced outgoing water ray before
Fresnel/body/foam composition; it does not render or post-process an extra reflection view.

This is analytic single-scattering aerial perspective; it does not add shadowed volumetric shafts or a 3D
multiple-scattering simulation. Blended surfaces without their own depth inherit the opaque depth behind them.
The sky is already painted with atmosphere; its extra fog fades rapidly above the horizon rather than erasing
the cloudy sun. Reflection roughness uses its representative traced hit distance, retaining the established filter.

MmlGfxConstants appends four float4s (old prefix intact), Rust bindings regenerated. The SDK accepts explicit
extinction/color/height/phase/sky parameters, with versioned size and finite/range checks. Example settings now
persist under mml/data/atmosphere/config.json; tools/migrate_atmosphere.py moves the old loader policy with backups.
Graphics startup is requested through the mod's normal gfx manifest. The host has no example-id/config knowledge.

WARP runs 45 numerical integral/opacity cases and the actual bridge PsPreComposite on synthetic 5 m replacement
water versus 1000 m opaque depth (plus disabled case). Native tests cover both builds and both DLSS paths.
One-game original view ABBA/BAAB (2 blocks, 45 frames/segment): +0.285 +/-0.044 ms (95%) wall, 1.2%, versus fog off
with DLAA on. This sample does not establish a significant GPU delta; no broader performance claim is made.
