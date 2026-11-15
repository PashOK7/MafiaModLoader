# Water object reflections: completion plan

2026-11-01. Owner requested a small-step execution plan after accepting the black-band correction.
This is the current implementation checklist; WATER_LIGHTING_ARCHITECTURE.md retains the original audit/design.
LATEST OWNER PRIORITY supersedes the earlier hardware-first proposal: preserve today's accepted appearance;
finish reflection correctness FIRST, restore missing water shadows SECOND, optimize THIRD. The earlier
hardware/software comparison is deferred to the optimization stage, not a prerequisite for reflection fixes.

## Current handoff (2026-11-15)

Steps18/19 integrated world-source object queries and world-space shore constraints. Step20 has filtering
code and static native previews, but its moving-camera/object gate is deferred; later lighting, lifecycle,
old-capture removal, shadows, temporal and final performance/visual gates remain open. Do not interpret
the original starting point below as a description of the current normal shader. See [STATUS.md](STATUS.md)
for validation limits and [the update notes](PATCHNOTES_2026-11-15.md) for public SDK changes.

## Original starting point (2026-11-01)

- Owner explicitly accepts that checkpoint's OVERALL WATER APPEARANCE: waves, color, foam, highlights, lighting,
  roughness/filtering and the black-band correction. The two current visual defects are object reflections and
  missing shadows. Preserve everything else; do not trade the accepted appearance for frame rate.
- BOTH labs at that checkpoint had water 896F21FA9450F6445B33E2FB1A0DDBC696D80DF40ACBEF8196EC53C721D74824.
  Verified checkpoint: build/water-accepted-visual-20261101/manifest.json (50 source/SDK/lab binary/settings
  files, per-file SHA256). No automatic bulk restore: preserve unrelated subsequent edits. Repeated native
  visual cases/phase captures in step1 are still required; a file checkpoint alone does not complete step1.
- At that checkpoint the production object lookup still read the finite planar reflection image. Steps18/19
  below supersede the integration status; full hidden-surface/disocclusion acceptance remains open.
- Available: unculled native draw collection, original UV/colors/material state, same-frame native preparation,
  exact native texture/mip transfer, CPU mesh/instance BVH core and tested D3D11 traversal HLSL.
- Native source/texture tests passed 12 each build; collection/context/pose/fault tests passed 16 each build.
  These establish source behavior, not final reflected appearance or production performance.
- Remaining: live scene consumer, GPU snapshot uploader, material storage/evaluation, stable caches, missing
  geometry/streaming coverage, production integration and spatial/temporal/performance acceptance.
- The current D3D11 software query is a tested reference/prototype, not an established performance winner.
  RTX3070 hardware tracing is a deferred optimization candidate. The existing x64 sidecar supports D3D12/shared output;
  that is useful infrastructure, NOT an already implemented DXR scene/ray transport service.

## Execution rules

Each numbered checkbox is one reviewable implementation/test result. Record the result and evidence in
JOURNAL.md, update this checklist and ROADMAP.md, and stop calling a step pending only after its gate passes.
Work sequentially; no subagents are requested. Read/hash-check targets immediately before edits, preserve
unrelated work and save verified preimages before broad changes. Stay on main; no publication is authorized.

Shared mechanisms belong in host/SDK; water material policy belongs in the mod. A necessary new ABI suffix
gets size/capability guards, generated Rust bindings, safe Rust support and C/C++/Rust lifecycle tests.
Do not add API merely to implement an artistic preset. Native address/layout work is verified in BOTH builds
immediately. Every completed feature block is tested in both labs before it is called done.

Keep the accepted wave controls/geometry, cloud-opening light and existing interface/atmosphere convention.
No added artistic sliders, collision-mesh substitution, guessed stale geometry, upward ray clamp, screen-edge
fade or silent fallback to the old reflection image. A diagnostic preview is labeled and never counted as final.
Use background lab launches and bridge input only. Preserve owner settings and release test camera/time holds.

## Binding phase order and visual regression rule

2026-11-04 owner override: step20 is installed and optimization starts now. Defer final moving-camera and
moving-object acceptance until the preview is fast enough to evaluate motion. Keep the accepted resolution,
sample counts, material and wave quality. Preserve the build timeout and measure performance changes with
matching render resolution, wave settings, camera and phase.

A. Reflection correctness: steps 1-23 and 25, with early native checkpoints 5/15. Steps 16/17 establish the
   lifetime/coherent-publication behavior necessary for correctness; defer speed-only tuning. Step4 may use
   the already-tested D3D11 reference backend. Backend-neutral source data keeps later hardware migration open.
B. Restore the existing water shadows: S1-S6 below. Then run queued temporal reflection checks (step26).
C. Optimize the accepted correct image: O1 below, then step24. Finish final delivery/docs with steps27/28.

The numbered identities below are stable references. The owner authorized optimization before the remaining
reflection and shadow acceptance. Correctness, lifecycle and resource bounds remain required during optimization.
Do not retune the spectrum, wave dimensions/speed/chop, color/medium, foam, cloud-opening source, reflection
softness, sample distribution or exposure to make a slower path appear faster. Any later optimization is
compared against the SAME accepted phase/pose/settings, including moving sequences and both DLSS states.
Check hit/material/depth and native images, not only FPS or scalar brightness. A visual regression invalidates
the optimization: restore the accepted implementation rather than keeping the loss by default. Intentional
visible differences during A/B are confined to corrected reflections/shadows and necessary disocclusion.

## Ordered small steps

### Establish a visible geometry checkpoint

1. [x] **Freeze the acceptance cases.** Save current settings, installed hashes, restore points and baseline
   frames: hidden building, two background depths behind a moving occluder, passing native barge, shoreline,
   bridge, sun-facing waves and flat water. Gate: cases repeat with the same world/wave phase in both labs;
   comparisons track the same world ray/hit, not an unrelated pixel after a camera move.
   Evidence: build/reflection-block-20261101/1.2/v3 and /1.0/v2, eight native cases each; repeated captures
   identical below the diagnostic overlay. Per-build restore point/settings and zero wave/foam phase recorded.
2. [x] **Inventory actual source gaps.** Count and identify unreadable geometry, billboards, GPU deformation,
   extra material stages, blended/cutout draws and inactive city blocks in those cases. Gate: every missing
   contributor has a reason and a required provider; none is silently treated as transparent sky.
   Evidence: same baseline inventories; no refused CPU packets, two native texture stages. Missing billboard
   glow/light sources (506 in the broad 1.2 box) and inactive 1.0 city blocks explicitly identified; see contract.
3. [x] **Define the consumer snapshot.** Specify scene/frame generation, mesh/material identity and revisions,
   current instances, attribute ownership and complete/unknown coverage. Gate: lifetime/reuse and failure rules
   are explicit; a previous-frame snapshot cannot be published as current after a failed update.
   Contract: docs/SCENE_RADIANCE_SNAPSHOT.md. Existing ABI unchanged; caller-owned world render-cycle token
   guards source-to-pass publication because MmlGfxFrame has no matching public host-frame ID.
4. [x] **Add the selected backend's reusable scene uploader.** Upload backend-neutral native triangles,
   attributes, instances and missing coverage coherently; build hardware BLAS/TLAS for the DXR path, or upload
   the existing software BVH for the reference/compatibility path. Validate sizes/ownership and release all
   resources. Gate: CPU-reference/selected-GPU answers agree after mesh reuse, nonuniform transforms, moving/removing
   instances and device recreation. A serialized CPU BVH is not a hardware acceleration-structure format.
   Use the current D3D11 path for correctness work; hardware comparison belongs to phase C under the latest
   owner priority. Do not reopen the accepted wave/lighting implementation to increase FPS during this step.
   Evidence: ray_scene_test actual WARP lifecycle/transform tests and native-world-rays.log (14 checks/lab).
   SDK ray_scene_gpu.h publishes coherent cycle/device-tagged buffers; scene_snapshot.h owns native borrows.
5. [x] **Connect native collection to live GPU hit diagnostics.** Show instance/primitive ID, distance and
   barycentrics from actual water rays, including a background hidden from both old capture cameras. A slow
   full-copy frozen preview is allowed here only as a diagnostic. Gate: native hidden-object and moving-layer
   tests pass in BOTH labs. This is the first visible checkpoint, before further caching infrastructure.
   Evidence: river_water.scene_probe, native-world-rays.log: off-camera layers at 9.94865/19.94865/29.94865m,
   identical under camera changes, current removal/miss/unknown and CPU distance/barycentric agreement.
   debug22 water-ray ID captures: */geometry-checkpoint. 1.0's broad river view is UNKNOWN (magenta) because
   inactive block bounds cover the rays; 1.2 also exposes missing billboard/coverage regions. This is a working
   geometry diagnostic, NOT final colored reflections; providers/streaming remain required at13/14.

### Give those hits the real materials

6. [x] **Specify native color/material semantics.** Verify fixed-stage operations, vertex/texture color space,
   UV transforms, prelighting and any already-applied fog against actual native material patches. Gate: the
   reflected material can be reconstructed without double lighting, double fog or arbitrary brightening.
   Evidence: native-material-test.log (64 D3D9 fixed-pipeline patches versus D3D11, RGB+alpha), and
   material-source-tests2.log (29/lab, camera-independent prelit colors/UVs, C/Rust/fault/state restoration).
   Native collection now suppresses/restores camera fog/overlay passes. Decode only the final encoded stages;
   do not apply another light to prelit source colors. docs/SCENE_RADIANCE_SNAPSHOT.md records the contract.
7. [x] **Build GPU texture storage.** Start with paged storage of native mip rectangles and metadata, preserving
   native dimensions/alpha. Implement native addressing/filtering with no cross-allocation bleed. Gate:
   wrap/mirror/clamp, mip transitions, supported compressed formats and animated texture replacement pass;
   pointer reuse cannot return another texture's old pixels. Measure allocation cost before finalizing packing.
   Evidence: texture-pages-test9.log, 1512 original-mip samples/9 formats, packed formats and BC1/2/3,
   wrap/mirror/clamp, point/bilinear/trilinear, page boundaries and same-storage replacement/failure/lifecycle.
   Typed D3D11/uncompressed D3D9 difference <=1.484/255; legacy D3D9 BC decoder differs up to4.8/255 (recorded
   separately, not claimed byte-identical). Native snapshots: 889 textures/72,514,012bytes in1.2 and497/
   42,733,792bytes in1.0, all readable; page padding <64KiB each. Native content is re-read and deduplicated by
   hash PLUS exact bytes, never by pointer alone. No production water material switch yet.
8. [x] **Bind attributes to stable primitives.** Keep original UV0/UV1, colors and material IDs linked to each
   primitive through BVH sorting and GPU upload. Gate: barycentric checker textures/vertex colors agree with
   native triangles, including shared meshes with different instance transforms.
   Evidence: attributes-unit-test2.log actual WARP original primitive/UV0/UV1/prelit color/checker texel checks
   under nonuniform instances; native-attributes-tests.log19/lab includes original native attribute agreement.
   RadianceGpu publishes geometry, textures, attributes and material tables together; failed/stale candidates
   cannot bind. Default water has not switched to this object path.
9. [x] **Shade opaque ray hits.** Evaluate the verified native texture stages and prelit color at the hit,
   converting their final result to the shared linear-light convention. Gate: native reflected walls, trees,
   bridge and barge have their actual colors without sampling scene-frame color.
   Evidence: opaque-material-unit-test.log and opaque-native-tests3.log, five native source/material samples
   per build: tunnel, barge hull, tree mesh, house and bridge. Actual UV/texture/prelighting/final linear values;
   no scene-color sample. Test fixture temporarily activates the whole city and restores the prior flag;
   this is NOT the owner-scoped production streaming solution of14. Native ANISOTROPIC MIN is supported at
   zero footprint via its MAG path; anisotropic minification remains explicitly unavailable until20.
10. [x] **Implement alpha-test and culling traversal.** Test opacity before accepting the nearest hit; continue
    through a cutout to the correct deeper layer and honor material sidedness. Gate: a fence/foliage hole shows
    the correct hidden object, with no opaque rectangle or white silhouette halo.
    Evidence: actual WARP hidden-background/cutout checks, 224 D3D9 alpha comparisons and six native winding/
    mirrored cases. native-cutouts-v2.json passes BOTH labs: zabradli04 alpha0.1582031 fails GE200 at0.049952m;
    the ray finds the correct deeper fence primitive/material at10.751377m. Empty holes return complete misses.
    The first named tree fixtures had no alpha-test material and were rejected as unsuitable test cases.
    Production water cutover is still18; this gate proves the new native query/material path, not old-image fixes.
11. [x] **Handle blended materials explicitly.** Implement the ordered contributions required by actual case
    materials, with bounded continuation and a stated limit. Gate: glass/additive/translucent draws neither
    become opaque blockers nor disappear silently; unsupported contributors remain visible in diagnostics.
    Evidence: blend-unit-test3.log actual WARP linear radiance/ordered deeper layers, exact coplanar draws,
    shared-edge dedup, unknown/unsupported sources and explicit32-layer overflow. native-blend-reference-test.log
    adds10 actual D3D9 blend-factor cases. native-blends-v2.json passes alpha/additive sources in BOTH labs:
    alpha0.3372549 and native taxi taillight additive alpha0.1404099, each followed by a deeper colored surface.
    Initial city-only additive fixture in1.2 was blocked by declared missing billboard boxes, not silently
    bypassed; isolated native taxi verifies composition separately. Providers remain13. Full offline suite passes.
    debug25 is a labeled linear composition diagnostic over a blue miss marker, not final water lighting.
12. [x] **Update current instance state.** Consume every moving instance's SAME-frame world transform and
    lifetime independently of the main view. Gate: move/rotate/remove the barge or a test prop out of frame;
    reflection hit identity/depth changes immediately, with no previous-position ghost.
    Evidence: native-current-instances-tests.log29 checks EACH build, including10 new transform/lifetime
    checks and19 geometry/attribute regressions. Native taxi outside both capture views: same world ray depth
    7.698921m ->9.400332m after90-degree rotation; moving away gives a complete miss, returning the pose restores
    7.698921m exactly, deletion leaves zero triangles. Every snapshot's native frame is post-mutation and agrees
    with the exhaustive independent geometry reference. Existing owned source copies current native matrices;
    no transform cache or camera image is involved. All props/camera/time test overrides released.
13. [x] **Complete required deformation providers.** Supply current GPU-skinned/morphed geometry and native
    billboard semantics for reflection queries where the inventory requires them. Do not freeze a main-camera
    billboard or substitute a rest pose. Gate: required animated/off-camera contributors pass in both builds.
    Evidence: billboard-math-test1.log24 native matrices/CPU/WARP; billboard-rays-test1.log per-ray rotation/
    conservative bounds/degenerate UNKNOWN. native-billboard-provider-v3.json9/lab: opt-in C/C++/Rust source,
    native restoration, prelit/UV/vertex camera invariance and owner fault/reacquisition. Actual native glow
    GPU materials/additive continuation pass7/lab (native-billboard-rays-v2.json). Off-camera SMMR people were
    missing from the type selection; verified both constructors and included their native posed-buffer path.
    native-deformation-v2.json5/lab and permanent singlemorph-official-regression.log32/lab prove changed pose
    changes actual vertices, removes old GPU hit, supplies new hit and restores original geometry on return.
    Required inventory uses CPU skin/morph; no required GPU-skin/custom-shader source was observed. Those
    unverified configurations still report unavailable rather than substituting a rest pose. Streaming is14.
14. [x] **Close streaming coverage gaps.** Ensure relevant native city blocks stay available for the declared
    reflection visibility range independently of main-camera culling, with owner-scoped native activation and
    release if needed. Gate: a building does not vanish from reflection when it leaves the main view; missing
    data and finite-range policy are explicit. Do not promise geometry beyond owned/loaded scene coverage.
    Evidence: native-residency-v3.json12 EACH build; default distance mode0 retained,103 required blocks ready,
    no streaming gaps, actual hidden building GPU material invariant under camera turn/FOV, combined C/C++/
    Rust owners, last-owner fault/release and same-mission save reload generation. Unrelated native fade prefix
    stays byte-identical; only entries appended by scoped activation are completed. New residency ABI/service
    does not change saved far/FOV/LOD/occluder settings. docs/SCENE_RESIDENCY.md. residency-load-offline.log
    ALL PASSED. Initial same-mission reload omission was fixed in the native save/new-game boundary; failed
    v1/v2 evidence retained (v2 also used an unsuitable camera-dependent building selection/5s poll timeout).
15. [x] **Show colored native geometry reflections.** Combine steps 4-14 in an explicit preview, using geometry
    hits/materials for objects and the directional cube only after a complete miss within the declared range.
    Gate: the hidden building and barge reveal the correct deeper colors, not just diagnostic triangles.
    This is the second visible checkpoint and an opportunity for owner review of a concrete lab build.
    Evidence: native-preview-v1 actual bank flat/rippled frames BOTH builds, including colored houses absent
    from1.0's main frame; native-preview-v2/1.0 and v3/1.2 show the actual barge hull/superstructures and deeper
    bank colors. Agent inspected native PNGs. Initial v1 barge exceeded bank coverage; v2's1.2 street camera
    left too little visible water, so v3 verifies an over-water lateral camera through native ground queries.
    Out-of-range horizon remains explicit magenta, never false sky. Preview PS resource bindings exclude the
    legacy object color/depth/painted-sky and SceneColor; main SceneDepth only occludes the primary surface.
    Normal PsWater SHEX is byte-identical to accepted155696-byte code. Post-preview native regression32/lab
    passes, settings/time/window/DLSS restored, no test props. debug26 is RAW incident radiance at LOD0/300m,
    not final water/Fresnel/body/foam/continued-wave/atmosphere/filtering. Those remain18/20/21. Full scene copy
    makes the broad diagnostic about10s/frame CPU; step16/17 must address lifetime/coherent reuse before cutover.

### Make the path usable every frame and integrate it

16. [x] **Cache immutable mesh data correctly.** Introduce verified buffer/mesh lifetime and content revisions,
    cache local acceleration structures and invalidate on native writes, reallocation, LOD/material change or
    reuse. Gate: an unchanged mesh is not recopied/rebuilt each frame, and changed data is never a stale cache
    hit. Pointer identity or a time-to-live alone is insufficient.
    Evidence: native-scene-cache-v2.json9 EACH build proves current native content reuse, exact hit/UV/prelit/
    color, moved/returned/removed/recreated instance and explicit release. Exact full borrowed vertex/index/
    FVF/cull comparison supplies owned immutable mesh identities; changed pose/UV/color/topology/cull cannot
    reuse them. Every native mip is compared before sharing CPU texels; actual D3D9 every-mip mutation tests
    pass. Native-preview-v4 reaches zero mesh builds/texture copies with25k+ verified draws per frame; host
    source preparation and packed CPU/GPU publication still occur. Cache payload capacities are bounded and
    release drops all owned allocations. Full offline/targeted WARP checks pass. Large1.0 repeated GPU
    publication still hits E_OUTOFMEMORY in texture-page-build (native-black-v3); this skips WATER entirely,
    not an object reflection artifact. This is the next17 publication/resource gate, not a claim of final speed.
17. [x] **Publish dynamic snapshots efficiently.** Update instances and genuinely changed/deformed data, refit
    or rebuild the top-level structure as needed, and publish a coherent frame without waiting on GPU readback.
    Gate: same-frame movement remains correct; source copy/build/upload costs are measured separately.
    Required reproduced failure: broad1.0 copies allocate/repack about83MiB of texture pages every frame and
    may run out of address space, skipping the water pass. Reuse verified immutable GPU tables, retain coherent
    current instance/frame constants and bound staging allocations. Do not conceal failure with a stale image.
    Evidence: native-publication-v1 repeats the failing broad1.0 case:5/5 current GPU queries succeed,11
    unchanged payloads share and only64 frame-constant bytes upload. Exact current owned bytes gate buffer
    reuse; verified immutable texel sequences gate page reuse. Changed transforms/geometry/materials still
    publish current data. Native continuous-v3 passes10/lab; current instance/posed-body regression32/lab and
    preview-v5 bank/actual-barge captures pass. WARP tests prove changed attribute upload, generation rejection
    and old CPU lease expiry; step17-offline.log ALL PASSED. Begin drops the builder's preceding CPU owner,
    avoiding three full scenes after a failed frame. Page staging validates/sizes before one exact allocation.
    Cost fields separate source copy, local build, texture verification, TLAS and upload; source validation
    remains slow (~1.7s inclusive broad1.0 sample), reserved for phase C. No optics/sampling retune or readback
    in the continuous path. Installed6E926BF4... both labs. Normal water still legacy until18.
18. [x] **Switch the normal water material to world queries.** Replace the projected object search on every
    reflected path, including continued wave-bounce rays. Choose the nearest valid object/water interaction
    rather than always visiting another wave before a closer hull. Gate: the normal mode shows the new path;
    known-missing data is diagnosed and never silently filled with the old image.
    Evidence: native-normal-v1 has8 inspected native frames/lab (bank flat/waves and actual barge level/down,
    each vs byte-identical accepted debug27). Normal PsWaterWorld binds native geometry/materials; its RDEF
    excludes projected object depth. Every retained transport segment queries nearer objects before the next
    water hit; unresolved downward searches only expose their traversed prefix, never submerged distant hulls.
    Camera-relative residency native7/lab and complete prior residency12/lab pass; post-cutover native instance/
    pose regression32/lab passes. Installed3804791D... both labs (diagnostic-only CPP addition after E51042CF...
    image build; shader unchanged). source_residency exposes current readiness independently of upload errors.
    Declared total camera/water/object range600m, collection cube608m. No old-image fallback. Accepted optical
    comparison SHEX remains155696 identical bytes. Native colors are deliberately unfogged until21, LOD0
    filtering until20, and projected shore/auxiliary capture remain19/23. Overall reflection acceptance is25.
19. [x] **Replace projected shoreline dependence.** Derive the bank/waterline constraint from world geometry
    with defined distance/coverage semantics. Gate: bank contact stays stable under pitch/FOV changes and a
    foreground hull cannot flatten waves on distant water. Preserve accepted wave dimensions.
    Result: owned current native plane sections, exact1m local distances/CSR GPU candidates; normal PS/DS
    have no projected shore-depth binding. Native CSR bank flat/waves/FOV40/85/barge frames inspected both
    labs. Compact-loop0ECC4EBF... installed/hash-matched both; failed1.2 barge-down now renders,1.0 down
    regression passes. Original3/96/7 iterations and optical samples preserved, accepted comparison SHEX
    still identical. Evidence native-shore-grid-v1/v2 and native-shore-loop-v1/v2. Full spatial acceptance25
    and production resolution/frame cost remain open; this checkbox establishes the world-contact mechanism.
20. [ ] **Filter ray-hit radiance consistently.** Derive material mip/footprint and unresolved-facet filtering
    from actual paths. Gate: sharp calm reflections and rippled reflections retain detail without frame-edge
    smear, unstable mip selection or uniform blur across unrelated objects.
    The focused D3D11 test calls the real water material provider. Widening a ray footprint changes the
    native mip and filtered alpha exposes deeper geometry. The production path now compiles as raster
    seed, compute transport and final composition. Both labs render static bank views with filtering
    enabled. The moving-camera/object visual gate is deferred at the owner's request.
21. [ ] **Verify lighting and travel distances.** Use hit distance and the correct ray segments for atmosphere;
    preserve the existing Fresnel, water body and cloud-opening source, with no duplicated incoming fog/light.
    Gate: sun/side/opposite views and object silhouettes match the intended contribution diagnostics; the
    accepted black-band and flat-water regressions still pass.
22. [ ] **Verify complete lifecycle behavior.** Exercise unload, guarded owner fault, reload, scene transition,
    resize/reset and repeated texture/mesh replacement. Gate: no lingering native overrides, stale handles,
    locked D3D9 resources, mixed-frame GPU snapshots or growing owner allocations in either build.
23. [ ] **Stop the obsolete production capture.** Disable the second city render and remove normal-mode reads
    of its object color/depth and shore estimate. Keep old comparison code only if explicitly diagnostic.
    Gate: the object path still works with that view unregistered and its textures unbound, proving there is
    no duplicate projected reflection source underneath the new result.

### Finish spatial acceptance, restore shadows, then optimize

24. [ ] **Meet the frame-cost gate.** Measure one lab at a time, at identical phase/resolution/settings, with
    paired CPU/GPU median/p95, memory and upload figures. Optimize collection, cache, ray traversal and wave
    continuation where measured. Target: recover the removed capture cost and avoid a p95 frame regression
    against the accepted 896F21FA baseline; this is a target to measure, not a promised unmeasured result.
    Gate: no full-scene copy/rebuild per frame or recurring streaming/reset hitch; document any remaining
    cost/tradeoff and obtain owner acceptance before calling an expensive preview final.
25. [ ] **Pass the full spatial matrix in BOTH labs.** Repeat the step-1 cases with DLSS off: pitch/yaw/FOV,
    low/high camera, near/far hits, screen boundaries, barge crossing, cutouts, streaming and removal. Gate:
    correct hit/material/depth and native pixels, no hull-color leaks/white edges/duplicate images, plus the
    owner's accepted wave/black-band regressions. Geometry counts alone cannot satisfy this gate.
26. [ ] **Resolve the queued temporal issue.** After step25 and restored spatial shadows, reproduce the owner's calm-water swimming with
    rapid camera movement and DLSS on. Audit the temporal inputs actually supported by the current pipeline;
    represent reflection motion/disocclusion/confidence through that contract without replacing all mixed
    water color motion with a hull/surface vector or disabling DLSS globally. Gate: calm and moving waves,
    moving reflected objects and newly exposed backgrounds remain stable with DLSS both off and on.
27. [ ] **Deliver a verified review build.** Install/hash-check both labs, verify actual running state, restore
    user settings and hand back camera/time control. Provide the tested cases and before/after images, then
    let the owner test the finished reflection behavior. Gate: explicit visual acceptance or concrete remaining
    defects recorded and returned to the relevant earlier step; no extra permission request for routine work.
28. [ ] **Close the work accurately.** Update architecture notes, SDK usage, this checklist, ROADMAP and JOURNAL
    with final installed hashes, evidence, costs and any declared engine-range limits; remove obsolete default
    resources/temporary autoloads. Gate: no required item is merely described as future work while reflections
    are called complete. Commit/push only if separately authorized.

## Phase B: restore missing water shadows

S1. [ ] **Reproduce the missing shadow without changing the look.** Save cases below a bridge/bank object and
    near a moving hull, with current lighting/settings. Gate: visible receiver/occluder positions and expected
    light direction are recorded in both labs; do not assume the loss is a disabled configuration toggle.
S2. [ ] **Trace the existing shadow path.** Check source request/direction, shadow-map contents, readiness,
    binding, constants and the water receiver's sample using existing diagnostics. Gate: locate the first
    failed link with native evidence; a bound texture alone does not prove a valid water shadow.
S3. [ ] **Restore the failed mechanism.** Repair the existing host/SDK/material path at that link and preserve
    native scene state and ownership. Gate: the known blocker visibly shadows water with the correct source;
    no blanket water darkening, unrelated sun retuning or new artistic shadow slider.
S4. [ ] **Check the actual light contributions.** Apply visibility to the appropriate direct illumination and
    medium terms without arbitrarily multiplying all reflected sky/object radiance. Gate: shadowed/unshadowed
    native contribution views agree with the shared lighting convention and accepted sun/cloud appearance.
S5. [ ] **Verify motion and lifecycle in BOTH labs.** Move blocker/camera, cross the old image boundary, change
    wave phase, reload/reset and test DLSS off/on. Gate: correct world placement, no stale/flickering shadow,
    no white edges/black-band regression, and unchanged accepted appearance outside the intended shadow effect.
S6. [ ] **Record the accepted reflection-and-shadow baseline.** Install/hash-check the review build, capture
    matching static/moving references and obtain the owner's visual verdict. Gate: both visual defects are
    resolved or concrete remaining cases go back to A/B. Only then start optimization experiments.

## Phase C: deferred hardware comparison

The owner now requires sustained 30 FPS in the native 1.2 river view at 1440x810 with the current image.
Measure the full frame with an active bridge and a fresh water publication. Keep all optical samples,
scene coverage, geometry, material and shadow settings. Prioritize the CPU collector, GPU upload and
ray traversal separately. Record timing before accepting a change.

The current CPU cost is dominated by replaying native source groups and assembling all placements and
material bindings every frame. Carry forward immutable source data only with a verified buffer or
native content revision. Keep transform, visibility, residency, animated material and posed-actor
changes current. Track source preparation, callback/copy, placement and upload separately. A GPU
backend comparison must include these costs and the full-resolution shadow map lifetime.

O1. [ ] **Choose the faster backend without changing the accepted image.** After A/B and temporal validation,
    split current FFT, tessellation/shading/wave-continuation, scene, upload and wait costs. Query actual DXR
    capability and prototype hardware triangle queries through D3D12/x64/shared resources (or evaluate Vulkan
    if integration evidence favors it). Compare identical geometry, rays, materials, samples and frame IDs,
    including acceleration-structure build/update, transfer and synchronization. Keep the accepted wave
    representation unless an alternative passes image/motion equivalence. Gate: the COMPLETE path improves
    measured CPU/GPU median/p95/memory with no visual regression; isolated dispatch speed is insufficient.
    Software remains a correctness reference/compatibility option. No whole-game renderer port is presumed.

Current O1 evidence includes full native x64 transport and an opt-in
same-frame D3D11/D3D12 GPU relay in both builds. D3D12 retains the native
anisotropic cube sampling that differs under the Vulkan compiler path.
The 1.2 1440x810 DLAA paired run improves 11.02 to 11.63 FPS. Physical
allocation ids, content revisions and output submission acknowledgements
preserve current-frame inputs and fallback. Rare FXC/DXIL final-color edge
differences remain, so the hardware path stays opt-in. Full native visual,
lifecycle and 30 FPS performance gates are open. See the journal and water
lighting architecture for the current installed checkpoint.

## Performance reset after the 16-hour review (2026-11-05)

The owner rejected the current rate of progress. The accepted image still runs at roughly 11 FPS.
The paired live hardware path improved 11.024 to 11.629 FPS, about 5.5 percent. That result does not
support continuing isolated object-traversal improvements as the principal route to 30 FPS. Pause new
micro-optimizations and integrations until the following critical-path review selects the next change.

1. [ ] Retain one reproducible full-river baseline with camera, FOV, resolution, wave phase policy,
   scene coverage, DLAA and installed hashes recorded. Use one lab at a time. Record whole-frame
   median/p95, exclusive CPU stages, GPU timestamps and dependency waits. CPU waits and overlapping
   GPU stages must not be added as independent work. Reuse existing evidence where the configuration matches.
2. [ ] Establish the whole-frame ceiling of removing the source replay and repeated wave searches,
   separately and together, using one frozen-frame diagnostic with retained identical outputs. This
   diagnostic selects the architecture to change. It does not count as an accepted performance result.
3. [ ] Replace recurring static-source replay with retained scene records invalidated by verified native
   content/lifetime changes. Update current transforms, poses and animated material state separately.
   Account for discovery, invalidation and publication together. The planning budget for source
   collection is at most 8 ms. Reject a prototype that only removes callbacks while retaining most
   native preparation and cannot approach that budget.
   Scheduling update, 2026-11-06. Completely retaining the frozen scene, GPU publication and waterline gave
   11.416/10.980/11.009 FPS in A/B/A, with zero source upload in B. Defer this implementation until GPU
   improvements make source collection part of the limiting path. Prioritize step 4 after GPU attribution.
4. [ ] Replace the repeated water-surface search with a consistent surface/intersection representation.
   Evaluate GPU-built water geometry or a conservative spatial hierarchy with an exact local solve.
   The present seed and transport sample costs, about 23 and 30 ms, already exceed 33.3 ms together.
   Include rebuilding the water acceleration structure, contact handling, all optical samples and
   interop in the prototype. Aim for at most 20 ms for the complete water GPU work, with the full GPU
   frame below 30 ms. These are engineering budgets to demonstrate, not measured claims.
5. [ ] Compare appearance and motion against the accepted water, including banks, hidden objects,
   the moving barge, sunlight, calm water and DLAA. Preserve resolution and quality. Internal bitwise
   identity is useful for substitutions that claim identical arithmetic. A new intersection algorithm
   is judged by surface correctness and visible quality, rather than reproducing every old numerical
   edge case. Retain the accepted renderer while that gate is open.
6. [ ] Integrate only after the candidate's measured complete cost supports the frame budget. The
   final gate remains sustained native 30 FPS with p95 at most 33.3 ms and fresh water, followed by
   both-build validation. Another standalone speedup or numerically exact fixture does not close it.

### Frame architecture review after the RTX Remix comparison (2026-11-07)

NVIDIA's [acceleration manager](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/src/dxvk/rtx_render/rtx_accel_manager.cpp)
retains acceleration structures and checks for removals, content and transform changes before rebuilding dirty
groups. Its [shader organization](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/src/dxvk/shaders/rtx/README.md)
separates ray tracing, material data and lighting stages. These are architectural references for the existing
water renderer. The 30 FPS comparison is at native resolution with all four current optical samples, rather
than a result using fewer samples or frame generation.

1. [ ] Retain verified static native draw records and GPU geometry. Derive invalidation from current native
   scene membership, buffer lifetime/revisions and material writes. Keep off-camera residency. Rebuild only
   affected geometry groups and publish current transforms, posed meshes and animated materials each frame.
   Measure the complete collection and AS publication together. The x86 half owns bounded metadata and native
   state observations. The x64/GPU side owns the retained tracing structures.
   1.0 now queues verified native rigid preparation in bounded batches before the original pool
   publication. The matched probe saves about 9 ms of tree scans per collection. Native
   geometry and material inventory matched in both builds.
   Fixed-source diagnostic behind the bank reached 36.28 FPS with 32.45 ms p95, versus 16.83–17.31 FPS
   with current collection. Native block deactivation and pooled relinks are distinct membership edges.
   The current inventory resolves 10,629 of 12,041 draw owners. 8,839 ordinary draws have matching
   owner, pose and material pointer stamps in two fixed captures. Prove native binding and resource
   revisions before retaining any of those records, and keep the native replay for uncertain draws.
   A strict device-bind bypass saves about 3 ms of collection and does not improve full frame time.
   A topology-only native probe reused the ordered visual list and saved about 4 ms of collection
   in 1.2. It still ran preparation and callbacks and did not improve frame p95. The probe exposed
   unrelated transient frame releases that rebuild a directory guarded by one global epoch.
   Producer revisions now preserve proven same-byte writes using the existing published page copy.
   The resource probe observed 38 in-collection revision changes instead of 375 in 1.2 and 30
   instead of 685 in 1.0. Bindings and current content still need final validation before reuse.
   The retained path needs one bulk owned publication for unchanged draws, with no per-group native
   preparation or callback. Preserve the ordered native path for animated, dirty and unsupported input.
2. [ ] Give hardware the nearest-hit search for proven geometry. Carry the exact alpha, coplanar ordering
   and unsupported-coverage contracts through a separate material pass. Prototype a conservative distance
   bound from verified hits, then profile whether separating active wave, geometry and shading work reduces
   divergence. Retain the current path as reference. The native facet experiment validates candidate mapping on
   one captured frame, with more coverage work required before enabling it.
   Whole-scene AS reuse now checks all three immutable resource revisions. Full-size up/down dispatches were
   slower. Compact indirect queues now use the identical trace body and match all archived output bits.
   They reduce native worker trace median by 8.56 ms in 1.2 and 6.20 ms in 1.0 and are used by the opt-in
   facet path. The upward stage split now separates exact hit selection from shading. A compact
   continuation queue keeps the original alpha and blend processing. Same-frame motion comparisons
   in both builds preserved coverage and transport metadata. 1.2 GPU trace fell from 25.81 to
   18.41 ms, while the complete frame improved only 1.94 ms. The frame budget remains open.
   A GPU-built native texture bank matches the prior CPU-built bank bitwise. The bank's
   live revision-gated metadata publication and native sampler moving-image gate remain open.
   Rust's spatial facet partition retains exact world placements and all local vertex words.
   Across two full 1.2 captures, 32 m groups retain 177,548 of 196,072 current triangle
   candidates in 837 unchanged groups. Current draw IDs, local triangle indices and materials
   stay in the current mapping. The two-capture Rust DXR replay retains the 837 GPU
   addresses and rebuilds 15 dirty groups plus billboards. Its 1440x810 transport
   and final RGBA match bitwise. Serial live trials reuse about 830 to 845
   groups while panning. The stock whole-AS reuse already avoids most
   builds on this route. The cluster path stays isolated because neither
   lab shows a reliable full-frame gain.
3. [ ] Use one authoritative wave intersection representation for the initial hit and continuations, with a
   cheap conservative interval rejection before the local solve. Keep the same contact model, waves and rays.
   Move the wave work and optical transport together onto the GPU timeline. The current repair alone is
   about 25 ms in both labs, so queue synchronization alone cannot meet the frame budget.
   Bring opaque visibility forward with conservative depth bounds and preserve helper lanes and derivatives
   at silhouettes. The ordered seed now uses current displacement min/max tiles and whole-quad opaque rejection.
   The same-frame checks matched all covered seeds, derivatives, resolve depth and surface. Sampled seed is
   about 16–17 ms over the full river and 6 ms behind the road. Full native GPU time remains over budget.
   The signed clamped-stride candidate preserves these outputs in both tested 1.2 views,
   with no reliable frame-time gain. A different interval-search prototype changed the number
   of water hits and remains isolated.
   Rasterizing the same full inverse height field cuts the seed from about 16 to 2 ms,
   but direct raster reintroduces black bands at the native low-angle check. Keeping the
   original exact repair preserves that check and costs about 19 ms. The experimental
   local bracket and geometric visible-normal trials also preserve the low-angle
   dark-band check. Neither saves repeatable whole-frame time.
4. [ ] Profile a GPU-ordered end-to-end frame before changing the default graphics path. A temporary 1.0
   DXVK run at 1440x810 reduced the facet path's median from 133.61 to 68.48 ms without changing its samples.
   The staged release measured 109.73 ms through 9Ex and 74.71 ms through temporary DXVK
   in separate native runs. Both used the same four samples. DXVK still collected sources for about 53 ms.
   The 9Ex path spends about 75.6 ms of CPU wait on the D3D11 passes in the software control. That wait
   overlaps GPU work and must not be summed with shader time. The 1.2 DXVK path already has GPU-ordered
   interop and still needs substantially less shader work.
   A bounded two-slot relay avoided software fallback in the fixed-view
   ceiling probe and reached 40.73 ms median and 42.36 ms p95 with both
   source and shadow held. Fresh-source timing did not improve. The
   current source and shadow passes remain required in the released path.
   Rust's archived water shadow demand retains 9.57% of an 8192-square map
   at 32-texel tile granularity, preserving raw transport and final RGBA
   bitwise with four optical samples. Before applying a mask to live casters,
   prove that current opaque depth and displacement bounds cover every
   required sample, including the full filter footprint. Shadow consumers
   outside that receiver contract require the complete map.
   Live 1.2 conservative bounds require about 39% of the map. Rust/native
   stencil keeps the sampled water view, but its late shadow interval
   is 10.96 ms against 10.35 ms control. Native caster traversal remains
   complete, so this trial stays isolated.

## Completion definition

An object inside supported reflection range is reflected from the appropriate water ray even when it is absent
from the main frame or hidden from the old reflected camera. A moving occluder reveals the correct deeper
surface in the same frame. Materials/opacity, scene lifetime/coverage, both builds, performance and the queued
DLSS stability case and restored water shadows all pass. The accepted overall water appearance remains intact.

This does not promise infinite reflection distance, exact full path tracing through folded water or a new fluid
simulation. Any finite-range/unsupported-data limit must be explicit and must not reintroduce silent projected
image artifacts in the required cases. Those limits do not excuse missing required barge/building materials.
