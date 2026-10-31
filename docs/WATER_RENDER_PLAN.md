# Water rendering: correctness and performance plan

## SDK-owned atmosphere (2026-10-31)

- Shared mechanism extracted to host/atmosphere.cpp and append-only C ABI; safe Rust methods and neutral
  initialization helpers. Priority/acquisition ownership, copied requests, invalid-value rejection and snapshots.
- Weather palette, density conversion and UI moved to the policy-only atmosphere example. No example ids or
  config dependencies remain in the host. Legacy loader settings migrate with previews, hash guards and backups.
- Native atmosphere/render-controls suite: 39 checks in 1.2, 36 in 1.0 passed, including hot reload, owner override/
  restoration, unload to native fog and both upscaler paths. Actual shader WARP tests retained. Extension guide:
  docs/ATMOSPHERE_SDK.md. Original artistic values and camera/preferences retained during handoff.

## Atmospheric fog (2026-10-31, complete)

- Atmosphere is an opt-in SDK-owned service with a small policy mod; native Game/Off/Linear remains the baseline. Exact exponential-height
  optical-depth integral, uniform density below its base, smooth near clearing, sky-derived tint and broad
  scattering towards the sun the painted sky shows. No noise, delayed history or additional scene renders.
- Composite after replacement water using its actual displaced surface depth, before DLSS. Fog the reflected
  ray inside the water shader using its traced object distance; then the camera-to-water leg once in the bridge.
  Foam, sun glitter, body and reflection must all fade together. Native fog off only while atmosphere is ready;
  unavailable graphics bridge retains linear native fog as a fallback.
- WARP: 45 integral/opacity/limit cases plus 3 ACTUAL pre-composite shader cases distinguish displaced water
  at 5 m from opaque scenery at 1000 m, including disabled mode. Native 13 checks in 1.2 / 11 in 1.0 passed,
  including camera heights, native-fog restoration and DLSS on/off. Native UI checked in both; offline passed.
- Held original view, one game, DLAA on, custom 750 m/occluders on: ABBA/BAAB atmosphere versus fog off added
  0.285 ms +/-0.044 (95%) wall time, 1.2%; GPU difference not significant at this sample count. No new city pass.
  Owner's latest distance/occluders/water/DLSS preferences preserved; balanced new atmosphere selected.

## Saved rendering controls (2026-10-31, complete)

- Native Options > Mods > Rendering distance mode (game/custom/whole city), custom metres and independent
  occluders added. Custom streaming uses native cache quad tests, pool cleanup and hysteresis, and follows freecam.
- Native fog: game/off/custom start plus fade distance; preserve each sector's color and fog-free HUD/backdrop.
  Hardware and software vertex fog agree. Restoration, restart and load checked in both builds.
- Foam slider range 0..2: existing multiplier 1 is now 50%, with no stored-value migration or appearance change.
  Clamp final whitecap coverage so the stronger range cannot extrapolate body/reflection blends.
- Fresh user water/DLSS preferences and camera preserved (build/render-controls-20261031). Native 23 checks in
  1.2 and 22 in 1.0 passed; distance transitions, UI layout and native fog/water output checked. Offline suite passed.
  The native software path's ratios and both addresses were read/checked; current GPU uses hardware fog.

Owner's report, 2026-10-30. Preserve the chosen water appearance and every existing feature. Measure and fix
causes; do not trade away shadows, reflections, 3D geometry, resolution or material detail to raise FPS.

## Reproduction checkpoint

- Full-city daytime freeride, 1.2 lab W:/MafiaLAB, profile 4, police 0. Owner reports 35–40 FPS in this view.
- Eye: `[-1614.91552734375, 13.04620361328125, -264.55108642578125]`.
- Forward: `[0.7969084978103638, -0.4683259725570678, 0.3815856873989105]`.
- Frame 1280x960, FOV approximately 55.4 degrees; hold the actual view/projection from the saved checkpoint.
- Exact live settings/camera snapshot: `build/water-perf-20261030/checkpoint.json` (local, not game assets).
- Chosen water: wind 6.5, direction 210, waves 0.751953, height 3.5, chop 2, foam 1, glitter 0.380859,
  crests 0.922852, scattering 0.7, clarity 2, uniform depth 10; reflection, 3D waves and sun shadows on.
- DLSS module autoloads, but the owner has switched its processing off at this checkpoint. Saved mode DLAA,
  pipeline true, frame generation off. Compare enabled/disabled at this same camera, preserve all other settings.

## Issues and acceptance

1. **Two apparent shadow directions.** A long bridge shadow plus another shape looks as if Sun and Scene light
   both cast. Isolate the actual shadow visibility, planar reflection, original bridge shade and any baked layer.
   Acceptance: one selected shadow direction, correct alpha and complete moving casters; reflection remains valid.
2. **DLSS blurs the water.** Real displaced water currently enters colour before DLSS but does not replace its
   depth/motion. Inspect frame synchronization, displacement history, water depth, motion and specular history.
   Acceptance: crisp moving water without ghosting or the previously fixed moving waterline gap; DLSS stays on.
3. **Severe frame cost.** Record CPU and GPU baselines, call counts, variance and hot sites for shadow visibility,
   caster draws/constants, reflection, FFT, mesh/tessellation, water shading, bridge transfers/waits and DLSS.
   Acceptance: documented paired measurements from the same view, preserved image/features and correct state.
4. **Audio stutters/lags.** Correlate with long game-thread frames and device waits; inspect audio update/streaming
   separately if the correlation does not explain it. Acceptance: stable audio while moving the camera and during
   the heavy view. Agent measurements cannot replace the owner's listening check.
5. **Visible shadow-distance cutoff.** The owner added that shadows abruptly disappear in the distance. Cover
   every visible water receiver, retain requested world-space density, include distant casters and avoid rendering
   unrelated land. Acceptance: no fixed-radius edge visible in the water; move/raise/turn the camera to verify.

## Work order

- [x] Save chosen settings and exact reproduction camera before experiments.
- [x] Capture baseline images and CPU/GPU timings with one lab running; keep reproduction repeatable.
- [x] Identify both visible shadow shapes and correct unintended duplication; both builds verified.
- [x] Supply correct temporal inputs for displaced water; owner confirmed DLSS now works well.
- [x] Fit shadow coverage to all visible water, retaining density and excluding unneeded caster regions.
- [x] Remove measured per-caster query/constant overhead; reject a slower batching option by paired measurement.
- [x] Isolate auxiliary-camera audio updates; owner currently hears normal sound (watch for recurrence).
- [x] Regression: both builds, alpha, city depth, tessellation, temporal inputs, DLSS Quality and 10 FPS motion.
      Both wagons were owner-verified in the preceding caster block; no caster features removed here.
- [x] Record final evidence and remaining limits; restore chosen settings and actual free camera to the owner.
- [ ] Further measured work on engine visibility/occlusion, reflection culling and shared-device transfer cost.

## Evidence and findings

- Baseline at the saved view: 26.96 ms mean / 37.09 FPS; p95 29.23 ms. A/B isolation measured shadow-enabled
  27.23 ms versus temporarily shadow-disabled 9.82 ms (difference -17.27 +/- 1.52 ms, 95% interval). This isolates
  cost only; turning shadows off is not the fix. Native benchmark and sampler reports are archived beside the checkpoint.
- Actual shadow surveys found watercity.stin1/12/13/15 among casters. Hidden replacements were applied only to the
  main world and named views; the shadow camera counted as neither. Auxiliary hide rules now include it, and
  already-projected game shadows are never treated as solid casters. The second striped band is gone in debug 8.
- Shadow rendering now holds a reference to the existing Set*-tracked device mirror and uploads only constants
  read by each compiled shader. Previously about 75521 extra device Get* calls per shadowed frame. Native mirror
  cross-check: 5000 draws, zero mismatches. Initial post-fix sample: 14.12 ms / 70.83 FPS, p95 17.06 ms, same view,
  unchanged settings, all effects retained. Not a final paired performance claim; further stages remain.
- Detaching the listener did not suppress LS3D's area-sound/environment update block. It ran from light/reflection
  cameras and repeatedly started/stopped sounds. A checked mid hook now bypasses only that block in auxiliary
  renders, retaining hardware-mirror preparation; normal world updates remain. Both builds' disassembly/byte
  checks passed. Owner reports sound currently normal, with a request to revisit if it recurs.
- Displaced water now emits linear depth and current-to-previous unjittered motion as an optional pre-upscale MRT.
  The host merges this with the original world inputs. Previous FFT displacement and camera are retained; water
  also receives the same projection jitter as the city. Native DLSS depth view now shows water surface depth.
  The owner has moved the camera and confirmed DLSS is good and sound no longer lags. The newer NVIDIA guide
  warns against the legacy BiasCurrentColor mask on current models, so it was not substituted for correct inputs:
  [NVIDIA programming guide, section 3.15](https://raw.githubusercontent.com/NVIDIA/DLSS/main/doc/DLSS_Programming_Guide_Release.pdf).
- Static city batching was measured, not assumed beneficial: +0.427 +/- 0.204 ms (95%), +4102 Get* calls, no
  significant draw-count reduction in this view. Kept off. This option's known far-city benefit does not generalize.

## Completed block and remaining budget (2026-10-31)

- Receiver fitting covers the current frustum's water-height slab, including conservative wave displacement.
  Separate light-space extents [240,60] m and a 4096 map retain maximum 0.1171875 m/texel in the checkpoint view.
  Raised and turned camera views in both labs remain covered, without a fixed 120 m edge. Far plane, loaded
  geometry and device texture limits still apply; `density_limited` reports the latter. Shadow shaders are now
  built ahead of time, avoiding runtime compilation when the fitted map changes size.
- Final single-lab sample, saved camera/settings, DLAA processing on, all effects and expanded shadows retained:
  **14.68 ms / 68.14 FPS**, median 14.52, p95 15.82, max 19.20 ms (411 frames). Baseline 26.96 ms / 37.09 FPS.
  This is a before/after reference, not a paired causal estimate: traffic/world state and machine load differ;
  baseline DLSS processing was off, and final coverage is larger. No image-quality setting was lowered.
- CPU sites overlap: shadow map 5.35 ms, reflection 1.91 ms, graphics bridge 2.95 ms (includes device waits).
  Nonblocking water GPU query: FFT 0.192 ms, surface 1.635 ms (includes reflection copy/mips and drawing).
  Bridge telemetry: wait11 2.42 ms, wait9 0.176 ms, DLSS GPU about 0.8 ms. Do not add CPU/GPU values as separate
  serial costs. Visibility recursion 8.8% self, occlusion 6.6%, Wait11 16.7% total remain measurable work.
- Full offline suite ALL PASSED (`offline-tests.log`); real-device regression 22 checks passed per build, both
  running concurrently, including DLSS Quality/pipeline, alpha/depth state and temporal water inputs. Additional
  upward freecam movement at a 10 FPS cap inspected in both builds; no reopened moving waterline gap observed.
  Address checks passed in both builds. Evidence: final-profile.json, final-state.json, coverage-tests.json,
  baseline.jpg/final.jpg and regression-1.2.log/regression-1.0.log under build/water-perf-20261030.
- Final loader SHA256 FCEF199A9D8410A003EC5492F0EF826E2046E5548014596D9C4C10AB6CB27BA5; water DLL
  CE17573F64FEEFC34328DF3E46B65745A1D978D2AC357B62C9BAA44628DA2CAD. Installed hashes agree in both labs.
- 1.0 test overrides restored and pid 11988 closed. 1.2 pid 18456 remains at the saved view with the owner's
  exact water settings, DLAA/pipeline on, max_fps=0, debug=0, menus closed. Actual devtools freecam is active,
  bridge hold released. This block improves measured costs substantially; further optimization is not complete.

## Unexpected process loss reported 2026-10-31

- [ ] Determine why 1.2 pid 18456 disappeared after handoff. The owner did not close it; the agent's final
  explicit close targeted only 1.0 pid 11988. At 08:43 neither game was running. The last normal 1.2 log entry
  is 01:34:16, with DLAA processing normally; no ExitProcess marker or new crash report. Latest native dump is
  from October 27 and is unrelated. Windows Application/System queries over the preceding ten hours found no
  matching Game.exe failure, resource-exhaustion, display-reset or shutdown/sleep event. This does not establish
  a crash or an external termination; the cause and exact exit time remain unknown. MCP launches detached and
  closes its process handle immediately, so its status cannot recover this process's exit code after the fact.
- Preserved original log in build/water-unexpected-exit-20261031/mml.log before relaunch. Restored saved camera
  and unchanged water settings in pid 19916; DLAA/pipeline auto-started, drawn increases, no last_error. Actual
  freecam active and bridge hold released. No speculative rendering fix made for this unexplained exit.

## Sibling water source review (2026-10-31)

Read the owner's mafia-ragdoll water implementation and compared it with the current renderer. Detailed source
references and tradeoffs: [WATER_RAGDOLL_REVIEW.md](WATER_RAGDOLL_REVIEW.md). Useful candidates: Gaussian/EWA
reflection filtering, a geometry-derived shore map, variance-preserving flow/tiling and a supersampled bench.
No direct performance win established: that clipmap submits 294912 patches, versus our recorded 91946; its
reflection march/filter can do more work; its own restricted reflection-culling task remains unfinished.

- [ ] Measure conservative spatial rejection of existing patches before hull tessellation, preserving output.
- [ ] Evaluate accurate reflection-footprint filtering against a fixed motion/supersampling reference and GPU cost.
- [ ] Stable shore distance map when addressing view-dependent contact, without changing virtual water depth.

Keep current posed alpha-aware shadow casters and temporal DLSS inputs. River current/wakes/impacts are separate
features rather than an optimization shortcut. Review only: no source port, setting change or game restart.

## Deferred checkpoint: tiny water patch, severe slowdown (2026-10-31)

Owner explicitly requested capture only and deferred diagnosis. About 19 FPS without the small lower-right
water patch, about 5 FPS when it becomes visible. Native screenshot confirms 5 FPS / 202.7 ms at capture.

- Eye `[-1407.4803466796875, 7.423660278320312, -798.749267578125]`.
- Forward `[0.7061591744422913, 0.12939536571502686, 0.6961293816566467]`.
- 1280x960, vertical FOV 55.41292190551758 degrees, near 0.05, actual far 4993.26904296875 m.
- Exact camera, rendering switches, water overrides, DLSS, FPS settings and native timing/state snapshot:
  `build/water-tiny-patch-20261031/checkpoint.json`; native picture: `frame.jpg` in that folder.
- Preserve the rendering switches and far plane when reproducing; they differ from the earlier 280 m checkpoint.
  Water at capture: wind 6.5, waves 1.09375, height 1.75, chop 2, glitter 0.380859; all other values in the JSON.
- [ ] Revisit only when the owner resumes this issue. No camera move, setting change, cap or diagnosis made for
  this capture. Do not classify the cause from the screenshot alone.

## Completed sky and wholly-offscreen visibility block (2026-10-31)

- Owner excludes water physics from the sibling work to adopt. Accepted: reflection of the actual cloudy sky,
  then soft angular restriction around the visible sun. `sun_radius` is 0.5..60 deg, default 18, on Water 2/2.
  The painted sky's bright shape/hue are expanded in a linear HDR target before roughness filtering; no separate
  clear disc. This reconstructs an assumed radiance range from LDR rather than recovering lost physical energy.
- Water and reflection now have a conservative current-frustum preflight using captured original triangles and
  the same displacement guard as tessellation. Entirely offscreen water skips the full surface and FFT too.
  Camera-epoch queries avoid stale-zero suppression on return, without waiting for the GPU. Setting changes
  invalidate preceding visibility. Resolution, reflection update rate while visible and geometric detail retained.
- This is a limited visibility improvement, not completion of the visible-reflection optimization request.
  The tiny-patch 19-to-5 FPS checkpoint remains deferred. Roughness already selects reflection mips; height alone
  cannot guarantee that sharp contacts may be rendered coarsely. The same scene resources already serve both
  cameras; ordinary instancing does not make the second projection/rasterization free.
- Tests: actual compute shader on WARP verifies bright opening, dark cloud, angular support/falloff, retained hue,
  unchanged foreground geometry and zero-strength identity. Full offline suite ALL PASSED. Both live builds
  passed surface/reflection rejection and immediate recovery tests; 1.0 additionally passed 29 integrated checks
  including shadow cut-outs/depth, tessellated waves and DLSS Quality/pipeline temporal recovery. An initial
  integration attempt started before the DLSS sidecar initialized (drawn=0); the ready-sidecar run passed.
- New radius label/value visually checked in 1.0 Water 2/2; the owner accepted the 1.2 sky appearance. Evidence:
  build/water-sky-20261031 (offline-tests.log, integration-ready-1.0.log, final-visibility-1.2.log,
  final-visibility-1.0.log). Source backups are there too. Final water DLL SHA256:
  4266CFDB8AFBE3DC6A7DBD333EE8428F07FD9BEFE72432F15A71BFFE2A0CF130, installed in both labs.

## Tiny-patch optimization resumed (2026-10-31)

Owner resumed the deferred issue and confirmed infinite draw distance. Restored the exact bookmark, whole_city
true (all 328 blocks, roughly 5 km far plane), original wave settings; saved the owner's current settings for handoff.
New baseline with the accepted sky renderer: 222.16 ms / 4.50 FPS. CPU sites: shadow map 102.98 ms, scene views
45.28 ms. Water GPU surface about 0.42 ms, FFT 0.32 ms. Shadow map 16384 squared, 53922 caster groups,
804835 triangles, 0.46875 m/texel and density limited. Main cause is extra city rendering, not water pixel shading.

- Rejected experiment: current-frame opaque-depth reduction reduced the shadow pass to about 4 ms, but running
  LS3D Render inside main stage 1 corrupted the radar and font blending. Complete device state restore and IGraph
  ResetRenderProps did not repair it. Removed this path, shader and test target; source archived in
  build/water-tiny-opt-20261031/rejected-depth. The owner reported the font regression; disabled it immediately.
- Delivered replacement: finite receiver triangles with a conservative displacement bound, clipped against
  the expanded current camera frustum before the original pre-world shadow pass. No history/readback or mid-frame
  Render. The entire actual water footprint remains eligible at infinite draw distance; only nonexistent surface
  beyond its triangles is excluded. World box extents use 12.5% steps instead of doubling each axis; texture
  dimensions still grow normally. Maximum PCSS footprint plus snapping is reserved around the receiver bounds.
  The first finite-surface-only A/B found no significant gain; the overly rounded box still covered the city.
  With tighter extents: about 41263 groups / 613142 triangles instead of 53922 / 804835; shadow pass about 70 ms
  instead of 103 ms. Requested 0.117 m density remains hardware-limited at this extreme view, but actual density
  improves from 0.469 to 0.254 m/texel. Resolution stays 16384; no range, wave or reflection-quality reduction.
- Same-run ABBA/BAAB (one game, 2 blocks x 4 segments x 30 frames): medians 190.863 ms old fit, 174.695 ms finite
  tight box. Block estimate -17.641 ms, 95% interval +/-86.032 ms: NOT statistically significant in this short
  noisy run; do not promise a precise global FPS gain. Separate profile: 171.38 ms frame, shadows 70.29 ms,
  reflection 40.67 ms. This is partial progress, not resolution of the full 19-to-5 FPS issue.
- Tests: finite/displaced/asymmetric-frustum CPU bounds test passed, full offline suite ALL PASSED; both live
  builds passed 29 integrated shadow, wave, sky and DLSS checks plus 5 far-view surface checks. Initial 1.2 temporal
  checks ran with DLSS processing off and were repeated with initialized Quality/pipeline DLSS in both builds.
  Native HUD screenshots confirmed the rejected path's rectangular radar and thick font outlines are gone.
  Host SHA256 6546BC65C8284D30B6271B7A6012774F9BB0F87B811ABD57CC2D3C6278BD99C0;
  water BC6FB364AD06935BB500072425F76B643107FA1293419CC198B42A20B99DBCED, hash-matched in both labs.
- Reflection visibility/cost is a separate next measurement. Do not lower resolution, range, material detail or
  visible update cadence, and do not assume occluders crossing the water clip plane are safe without verification.

## Freeride start facing the buildings (2026-10-31)

Owner identified an additional 7 FPS view with all water behind the buildings as the initial free-camera pose.
Reproduced the composition near eye [-1985,-2,28], direction [1,.03,0], whole_city true, 5 km range. This is
separate from the tiny visible-patch bookmark. Both water-plane occlusion queries settle at zero; reflection
correctly stops (scene-view site about 0.006 ms). The shadow map remains active: about 38894 groups / 585089
triangles, 78 ms CPU despite no visible water. A short held-camera profile measured 165.6 ms/frame with shadows,
98.4 ms without; settings restored afterwards. Infinite-distance main-city cost is also large. Evidence:
build/water-tiny-opt-20261031/start-occluded.json. This is a confirmed remaining defect, not solved by finite
surface fitting. Camera movement deliberately invalidates previous occlusion counts to avoid late recovery;
simply disabling shadows on a stale zero would cause delayed shadows when camera/occluders expose water.

## Current-depth receiver fit, final native state contract (2026-10-31)

The rejected experiment's root causes are now identified and repaired: native IGraph's 19 render-property cache
values AND its 24-byte cached viewport must be restored alongside the full D3D state block. The first omission
changed font blending; the second left the radar restoring the shadow-map viewport. Both addresses/functions
were read in both Ghidra projects; check_builds verifies the exact cache write destinations. Shadow-only Render
requests no stage callbacks. The near sector's mutable queue is an explicit early-path guard.

- Long-range receiver-only requests fit from THIS frame's opaque depth, first using finite level boxes including
  displacement, then CPU clipping against the actual displaced triangle footprint. The final intersection removes
  false candidates in empty parts of the boxes: the held start view now submits zero shadow casters/triangles.
  GPU readback is 28 bytes, measured about 0.25--0.36 ms here. No prior frame's occlusion gate is used for shadows.
- Ordinary shader/survey consumers, graphics passes inside views, populated near sectors and unsupported resources
  retain the established early map. Readback failure falls back to full finite/frustum fitting. Texture caps and
  PCSS padding retained; quantization cannot reduce requested texel density below what the texture can hold.
- Disabled DLSS releases gfx_motion: native state shows no motion owner or replay. Autoload and user's off/on choice
  are preserved. Existing static batch mode evaluated but remains OFF: silhouette differences need separate work.
- Same-run held-camera/world ABBA/BAAB, one game, infinite draw distance, batches off and DLSS processing off:
  pre-world finite shadows 109.648 ms, current-depth 56.392 ms; -53.268 ms +/-0.146 ms (95%), -48.6%. Both captured
  frames match exactly (zero mean, worst, and differing pixels). This preceded the final triangle-intersection
  refinement (which removes the remaining ~3 ms tiny map); it is kept as a conservative measured improvement.
- Full offline suite passed after cache/depth changes. Native integration: 41 checks each build passed after the
  density correction; one parallel run's 1.2 screenshot timed out during startup, then the ready-sidecar rerun passed.
  Final triangle-intersection unit tests include random displaced points inside current bounds; GPU WARP test uses
  the actual finite-box depth shader. Final binary hashes and final native runs recorded below when complete.
- Reflection on camera movement still uses the conservative immediate-recovery path: the old zero query is invalid
  after movement, so a hidden view may be rendered again. Main whole-city rendering is also still a large cost.
  Neither is claimed ideally optimized; no reflection resolution/range/update-rate reduction has been introduced.

Final handoff: native-depth2-surface-final.log passed 41 checks in EACH build on the final triangle-intersection
binary. Both GPU/CPU bounds tests passed. Initial cold-start hidden-only probe reached 1.0 before its sun/graphics
state initialized; after readiness both builds report no visible receivers and zero caster submissions. Final
1.2 profile (one game, world running, DLSS off as the owner left it, batch off, full city) averaged 58.24 ms /
17.17 FPS, median 57.65 ms; shadow-map bookkeeping 0.009 ms, scene views 0.004 ms, bridge 1.671 ms. The remaining
time is chiefly the main city. Actual devtools freecam is seeded at the reproduced street pose and released;
whole_city true preserved, time scale 1, FPS overlay restored. Only 1.0 test pid 26576 was closed.
Host 81A8207F3486EE8D3D3D0C36DE4F699349540D8A45EC16760999A416F3357AA5;
DLSS x86 15ACCF2DAF8EB33BEFF2E5839ABC3294E69DD7DF011D360FDC6718BE48E064B4;
water BC6FB364AD06935BB500072425F76B643107FA1293419CC198B42A20B99DBCED. Hash-matched in both labs.
Snapshots: final-state.json, final-profile.json, final-frame.jpg. Builds/signatures, generated Rust bindings,
offline-depth2.log, native tests and precise A/B artifacts are retained in the same evidence directory.
