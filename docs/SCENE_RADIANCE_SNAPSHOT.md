# Native world-radiance snapshot contract

The ownership/publication rules apply to current SDK consumers. Later sections retain dated experiments
and local probes; they are not all shipped implementations or a final water acceptance report.
World-source water integration is present at reflection steps18/19, while later gates remain open in
[WATER_REFLECTION_PLAN.md](WATER_REFLECTION_PLAN.md). See [STATUS.md](STATUS.md) for current validation.

2026-11-01. Reflection plan step3. The existing native draw/geometry ABI is the source; this document defines
its consumer-owned CPU/GPU publication semantics. It does not add an unversioned ABI or a water-specific host
dependency. The first consumer uses D3D11; native triangles/attributes/materials remain backend independent.

## Source and ownership

Collect only in the owner's main-world RENDER_WORLD callback, with that owner's observer registered. Copy all
borrowed vertices/indices, transforms, stage states and needed texture bytes before the source borrow expires.
Never retain host scratch pointers. The existing C/C++/Rust source ABI/lifetime guards remain applicable.
Use the exact prepared native buffers, not collision geometry, a rest pose, the main frame or the old mirror.
Native visual/group/texture pointers identify sources DURING a borrow; they do not prove durable lifetime or
unchanged content. Stage/vertex strides are explicit; size/capability checks remain mandatory.

A CPU snapshot owns geometry, per-primitive attributes, material states, original texture mips, current
instance transforms and conservative unavailable regions. Primitive/material links survive BVH sorting by
explicit IDs, not by assuming sorted triangle positions are the original index. Positions use metres/Y-up;
world transforms use the native row-vector layout. Preserve ray distance under nonuniform instance scaling.
Record sidedness, opacity and native stage semantics; unsupported data never gets an opaque default material.

## Frame publication

The consumer increments a main-world render-cycle token on RENDER_BEGIN and immediately marks its scene
unavailable for that cycle. Scene change, unload/fault and reset invalidate all earlier scene/device generations.
Collection builds a candidate snapshot; it becomes current only after the whole collection and validation
succeed. A failed update may retain old allocated storage, but cannot label old data current.

MmlSceneCollection.frame is the host's game-frame provenance. MmlGfxFrame has no matching public frame-id
field: do not compare it to an unrelated GPU/Present counter. The caller passes its current render-cycle token
to upload/bind and verifies it before querying in the world pass. Render callbacks and the pass run on the game
thread; view callbacks are not main-world cycles. Test failed/skipped collection as well as the ordinary sequence.

GPU resources publish coherently: build/upload candidate buffers and constants, validate the complete set,
then swap. Allocation/upload failure leaves the candidate unavailable; partial new buffers cannot be combined
with old instance/material tables. Device generation is part of ownership. Reset/unload releases every buffer,
view and retained native reference; original D3D9 default-pool textures must not be held across reset.

The initial implementation may copy complete snapshots as a diagnostic. Later reuse requires lifetime/content
evidence. A native pointer or time-to-live alone is not an immutable-mesh/animated-texture cache key. Hashes may
select a candidate, but equality/revision evidence must exclude stale data and pointer reuse.

Step16 verified in both labs: owned mesh content (all current borrowed vertices/indices, FVF and cull)
is hashed and then compared exactly before reusing its immutable local BVH/attributes. Transform/material
bindings remain current per publication. Mesh content identities never dereference a past native pointer.
Native texture provenance selects an owned candidate. The pinned dxwrapper can publish the optional
`NativeTextureRevision` D3D9 private-data token for its managed copies. A matching nonzero token proves
the same resource lifetime and published content. Writable locks and uncertain contents invalidate the
token. Without matching producer evidence, Verify compares current mip metadata and useful row bytes.
Changed format/size/mips/content triggers a new copy; unreadable current data stays unavailable. No
age-based validity or borrowed COM reference survives.
Retention is bounded (128MiB meshes/256MiB texture candidates), unseen entries retire after collection and
explicit Release/reset/unload drops all caches. Host borrow preparation and packed GPU-table assembly/copy
still occur; this is not a claim of zero native collection/upload cost. Native9/lab cache/lifetime checks and
current WARP/material/alpha/blend regressions pass. The subsequent step17 addresses the reproduced repeated
texture-page-build E_OUTOFMEMORY (which exposed underwater geometry by skipping the entire water pass).

The native collector reads the authoritative buffer shadows during its draw-free source scope. Changed plain
writes in that scope accumulate dirty pages and are published once per buffer before native device state is
restored. Busy dynamic buffers can use DISCARD and rebuild every initialized page from the shadow.
At source-scope entry the collector queries the device's current bindings. Successful game-thread
bindings keep the tracked stream, offset, stride and index buffer current. State blocks, UP draws and
failed or external binding changes invalidate this shortcut. The checked device getters, GetDesc and
read-only Lock remain available when bindings or shadows cannot be proven.

Normalized geometry has a 32 MiB cache in a 2 GiB process and a 96 MiB cache when more address space is
available. A draw key only selects a candidate. Both current
locked byte ranges and their layouts must match before the host returns the retained normalized arrays and
their revision. The optional size-tagged `MmlSceneGeometry.content_revision` lets the snapshot builder reuse
the corresponding local mesh with the same cull mode. Zero or a missing suffix keeps full content validation.
Tracked buffers can prove an unchanged range from its offset, extent and monotonic page content stamps.
A plain write that exactly matches the already published bytes preserves the stamp. Other writes
retain the conservative invalidation path, including buffers without a reliable published copy.
After a successful proof, the normalized payload can drop its duplicate raw bytes for that range. A
later unprovable range without a raw copy is decoded again.
Unrenamed non-shared index buffers with fully observed D3D9 Lock, Unlock and Release methods keep a
whole-buffer revision without a CPU shadow. Creation and writable locks advance that revision. An active
lock, an unobserved binding, or a different draw range makes collection read the actual buffer. A matching
bound revision and range can skip `GetIndices`, `GetDesc` and the read-only lock. `geom.state` counts these
as `native_index_proofs`. The metadata table holds at most 4096 buffers. No native pointer is dereferenced
through the cache.
An unobserved buffer class must return zero proof even when its pointer/binding is unchanged. Native D3D9
managed index buffers can have different Lock/Unlock implementations from the default-pool class; the
collector retains its ordinary data-read path there. The self-test covers both classes without weakening
this rule. See [final validation findings](../knowledge/notes/release-validation-20261115.md).
When an indexed group's pooled vertex span exceeds twice its index count, explicit collection decodes
only the distinct referenced vertices. It remaps indices without changing their order and keeps the
original offsets for subsequent verified draws. No raw copy of the unused range is retained. Without a
vertex revision proof, equality of the normalized referenced vertices still gates reuse. `geom.state`
counts sparse decodes and skipped vertices.
Native locked vertex reads can use one 64 KiB staging buffer. Sparse reads stage bounded runs of
referenced vertices. CPU-shadow reads go directly to the owned shadow.
A newly adopted shadow receives a new identity. Failed or unexpected write-watch results revoke that
proof and the collector continues comparing the current bytes directly.
Materials, transforms, draw order and the current world geometry selection still come from that frame.
The optional `MmlSceneGeometry.material_detail` suffix carries the current native stage and sampler details
from the host's device-state mirror. If the mirror could not read those fields, the builder uses the
per-draw device queries as before. The borrow is valid only within the callback.

The water builder packs one local BLAS per verified mesh content revision, in stable revision order.
Each instance still carries its own current material, placement and logical primitive range. The
logical draw order remains the tie-break for coincident hits. The default builder API also supports
the original per-draw BLAS layout. WARP compares shared instances with separately packed copies,
including their distinct materials and texture-filtered cutouts.

The water builder can refit a previously published TLAS when its instance count is unchanged and
at most one sixteenth of the instance bounds change. The preceding immutable lease supplies the
topology only. Current bounds and transforms replace every placement. Rebuild after 16 changed
refits, broader movement, or insertion/removal. A weak reference avoids retaining an extra scene.

The same previous lease can retain physical BLAS slots while new geometry appends. Unused slots
are unreachable from current instances. Compact when unused nodes and triangles would exceed one
eighth of the live payload or 8 MiB, whichever is smaller. The water status separates live from
packed BLAS triangle counts. The GPU still uses exact payload comparisons before sharing buffers.

Instanced collection can opt into parallel local BVH construction. The game thread resolves the native
draw and owns its vertices, triangles, attributes and texture bindings. At most two Windows thread-pool
jobs sort and build local trees from owned triangle vectors while collection continues. Each job takes
16 to 2048 triangles and inherits the submitting thread's floating-point controls. Small or very large
meshes, low address-space headroom (under 128 MiB), and unavailable worker slots use the serial builder.
All jobs finish before assembly and publication. Revision assignment and draw order stay on the game
thread, and the worker never calls LS3D, the host ABI, or Direct3D. The water status exposes worker counts,
wait time, and the retained mesh and texture payload sizes.
When the pool has yet to start a submitted job at the join, the game thread may claim the job and
cancel its unused callback. The same bounded scratch and arithmetic controls apply on either thread.
After the mesh jobs finish, a collection with at least 1024 placements can reuse one worker for half of
the current instance transforms and bounds. Both ranges write into the same preallocated instance array.
The top-level build or refit consumes that ordered array after the worker joins.
An actual top-level rebuild can split at the root median. The game thread and one worker build
disjoint preallocated depth-first ranges. The serial and parallel layouts have the same node and
primitive order. A topology refit still uses the previous order.
An earlier immutable publication can supply an instance's inverse and conservative bounds when the
current world matrix, local mesh bounds, billboard mode and arithmetic controls match exactly. The
builder still binds the current mesh root, material, draw ID and primitive interval. Missing, moved or
reordered inputs can build normally without keeping another scene or a copy of the placement list.

Step17 publishes an owned immutable Snapshot lease. Unchanged GPU buffers compare exact CURRENT owned
bytes before sharing; even shared handles take the current CPU lease, so independently changing tables do
not retain many whole old scenes. Texture pages can serve a reordered set or subset of the previous
verified immutable texel identities. Current material bindings are remapped to the retained page indices.
When new identities arrive within the retention limit, the previous bank supplies an exact GPU-copied
prefix and only the new mip payload and padding are staged on the CPU. Unused content is bounded by
one eighth of the current payload, with a one-page minimum and an 8 MiB maximum. Beyond that limit the
current ordered set rebuilds the bank. Current transforms/TLAS/frame constants remain separate
and coherence still commits as one publication. A failed candidate cannot expose old data as current.
On x86 a native texture bank of at least 32 MiB uses the existing 4 MiB streaming path immediately,
without allocating a second contiguous copy of the source mips. Page formats, padding and mip bytes
match the packed path.
For large buffers with at least half of their current bytes identical at the same offsets, the new GPU
allocation clones the common range on the device and uploads only changed regions. The previous GPU
resource stays immutable. A focused WARP readback checks both publications, including the new tail.
If the builder preserved an earlier packed mesh prefix, only the exact source snapshot can supply
that proof to the matching previous GPU publication. A mutable or public snapshot copy still has its
payload checked. The proof does not hold a strong reference to any preceding CPU snapshot.
The optional compact attribute layout follows physical BLAS triangle order, with one set of UVs and
colors per owned triangle. The ordered material query takes the current instance's material and still
orders native hits using the original per-draw primitive IDs. Waterline alpha tests use the same mesh
attributes and current material. The default SDK layout continues to index attributes by primitive ID.
In the compact layout equal native material records share one current slot. Texture provenance and IDs,
numeric stage and sampler states, alpha, blend, factor and support flags must match exactly. Material
slots never decide the draw order or replace the current draw's primitive interval.
If the contiguous CPU allocation for native texture pages fails, the same validated mip layout can be
uploaded in 4 MiB blocks. Mip offsets, pixel formats and padding are identical and the candidate is
published only after all resources have been prepared.
Begin releases the builder's previous snapshot owner before collecting again; GPU/external leases may keep
their own immutable publication. Page construction preflights all mips and allocates the exact padded array
once. Native broad1.0 failure repro now passes5/5,11 shared payloads/64 current constant bytes; continuous
move/remove/recreate/release10/lab, pose/geometry32/lab and bank/actual-barge captures pass. No readback is
used by continuous publication. Native collection still copies/verifies source data: broad1.0 sample1686ms
inclusive, source geometry copy118ms, texture verification1021ms, instance build18ms, publication47ms. These
are correctness/lifetime measurements, not final performance acceptance.

Step18 integration in progress: camera-relative source residency is now a host/SDK request resolved after the
current main camera override and before any auxiliary world draw. Native7/lab checks and current offline pass.
The normal consumer declares600m TOTAL geometric camera/water/object path length with608m collection cube
radius. Subtracting all preceding segment lengths ensures supported object segments stay inside that cube,
independent of camera pitch/FOV or image bounds. Infinite directional environment remains a separate source.
Unknown source/material data INSIDE the supported segment remains diagnosed, never old-image-filled.
Speed-only work remains phase C.

## Geometry and material queries

### Verified native material convention (2026-11-02)

Collection suppresses native camera fog, including separate overlay passes and software vertex factors, and
restores native properties/device states afterward. The source remains prelit by the native sector preparation.
Both native builds retain identical color/UV fingerprints under pitch/yaw/FOV changes. Do not relight these
colors or retain the main-camera fog. Apply outgoing-path atmosphere once after final native color evaluation.

Native fixed stages operate on encoded diffuse/texture values (sRGB sampler/write disabled in the inventory).
Evaluate RGB and alpha operations independently, clamp per stage, then decode FINAL RGB into linear light;
alpha is coverage. A NULL texture terminates the native textured chain. Observed stages require MODULATE2X,
ADD, SELECTARG1 and MODULATE alpha, plus BLENDFACTORALPHA at stage1. Arguments select diffuse/current/texture/
factor; preserve UV0/UV1 selection. Unsupported operations/arguments are unavailable, not opaque white.
sdk/gfx/native_material.hlsli is compared against64 actual D3D9 fixed-stage patches by native_material_test,
including alpha and absent textures (<=1.1/255 rounding tolerance). No arbitrary radiance gain is introduced.
Reference operation/argument definitions: https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dtextureop
and https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dta .

Return a positive-distance nearest hit with current instance, primitive, barycentrics and material; a complete
bounded-segment miss; unknown/incomplete; or invalid input. Query both layers behind moving occluders. Alpha
test rejects a candidate before nearest-hit acceptance; blending can require ordered continuation. Missing
alpha/material data is unknown, not transparent. Unknown before a known hit blocks a complete answer.

Ordered material continuation (SDK gfx/scene_query.hlsli) supports the observed SRCALPHA/INVSRCALPHA and
SRCALPHA/ONE with ADD blending. Evaluate each native material, decode its final RGB, then compose LINEAR
radiance front to back. Alpha attenuates the remaining background for INVSRCALPHA; ONE preserves it. Native
D3D9 tests establish blend-factor arithmetic, not identity with the legacy gamma-encoded framebuffer.
Opaque layers stop continuation; cutout failures contribute nothing. Unsupported state and unknown coverage
return an incomplete prefix with its remaining transmission, never an invented complete sky result.

Continuation keeps the original segment and excludes visited draw/depth pairs rather than advancing tMin:
different source draws at the same depth must survive. Exact depth ties use later source draw first, then stable
primitive identity within a draw. Shared edges of one draw contribute once; indistinguishable same-draw depths
use four float world-coordinate ulps. The required inventory has no same-group multipass draws (all pass0).
The explicit bound is32 contributing layers. An unfinished path at that bound remains UNKNOWN, even if its
transmission is small; no arbitrary transparency threshold claims completion. Independent WARP tests cover
coplanar overlays, shared edges, reordered BVH placements, opaque/additive ties, deeper coverage and overflow.
Native blended-material checks pass both builds (native-blends-v2.json): actual alpha0.3372549 and additive
taxi taillight alpha0.1404099 each continue to a deeper native colored surface. City-only additive sampling
in1.2 intersects pending billboard-provider UNKNOWN boxes; an isolated native taxi separates material
composition from that then-pending provider. Normal water switched to world queries at step18; the raw
diagnostics retain their explicit zero-footprint sampling contract.

Coverage is an owned complete region and a declared scene visibility range, not the main-camera frustum.
Inactive native city blocks, unavailable deformations/materials and failed geometry contribute conservative
unknown regions. An unknown ray cannot consume painted sky as if scene geometry had proved its absence.
The native sky cube is infinitely distant lighting; it does not supply hidden buildings. Finite visibility range
and streaming ownership must be explicit before production cutover. Required case contributors cannot remain
missing just because a diagnostic has an unknown color.

The accepted water field decides initial/continued ray geometry. At integration, compare object and water-hit
distances: a nearer hull wins over a later wave. Materials evaluate actual native attributes/textures; no
scene-image sample substitutes for shading. Apply the documented encoded fixed-stage operations before the
agreed linear conversion, and establish whether native prelighting already contains fog/shadow contributions.
Keep the existing accepted water optics, medium, cloud emitter, foam and sample distribution unchanged.

## Observed inventory and required providers

Baseline evidence: build/reflection-block-20261101/1.2/v3 and /1.0/v2. Eight cases per build, repeated native
pixel captures identical below the diagnostic overlay with both water/foam clocks held. Main 1.2 box yielded
13,764 CPU packets/233k+ triangles, no unreadable packet; both source FVFs are XYZ/prelit/one-or-two 2D UVs.
Observed state: two active stages maximum, no UV transform/generated coordinates, no sRGB sampler, no custom
shader/GPU skin/fixed-function lighting in these cases. Stage0 MODULATE2X/ADD and stage1 MODULATE2X or
BLENDFACTORALPHA require exact evaluation; alpha thresholds1/200, source-alpha blending and additive blends
are present. Original textures observed here are native A8R8G8B8; the transfer helper also supports tested BC mips.

1.2 reports 506 unavailable billboard visuals in the broad box. Examined examples are tunnel glow and traffic
light sprites, not unreadable ordinary triangle buffers. They require an explicit native billboard/emissive
provider with correct reflected-view semantics, opacity and visibility. Do not freeze the main-camera quad.
1.0 additionally reports inactive city-block boxes (89 in the initial low-water case); independent streaming
coverage is required. Reinventory different scenes/features rather than generalizing these counts universally.

Step13 closes the required procedural-source gap: opt-in native collection supplies owned original billboard
geometry/materials and rotation metadata, and query traversal evaluates its orientation per ray observer.
Native prelit/UV/vertex fingerprints are invariant under main pitch/FOV; both builds pass C/C++/Rust and fault
restoration, plus actual native glow GPU color/additive continuation. SMMR people had been silently omitted
by the original type selector despite sharing SNGM's preparation; this is corrected after both native
constructors were verified. Off-camera changed skeletal poses now reach the source/GPU query in both builds.
The required cases use CPU skin/morph; unsupported GPU/custom shader configurations remain explicit unknown.
Step14 now provides owner-scoped native block residency (SCENE_RESIDENCY.md), including same-mission load
invalidation. This closes the observed native streaming gaps without altering the owner's distance mode.
Normal per-segment object-path integration followed at step18; final spatial acceptance remains step25.

Step15's explicit debug26 preview traces the first resolved mean-facet reflection through the native material
layers and samples the native directional sky cube ONLY after a complete bounded miss. UNKNOWN coverage,
unsupported material, traversal overflow or unavailable sky remain magenta; the legacy image never fills them.
This is raw incident radiance, not the final water material: continued wave interactions, outgoing atmosphere,
material footprints and accepted Fresnel/body/foam composition still follow in18/20/21. debug25 retains its
blue miss marker for independent composition checks. Native step15 checkpoint passes in both builds:
bank flat/rippled native-preview-v1 and actual barge v2/1.0, v3/1.2. Compiled preview resource bindings contain
no projected object color/depth, painted-sky or main SceneColor input. Agent inspected the native images;
raw incident-radiance detail/aliasing is not final material filtering. Broad full copies remain about10s/frame
CPU and are diagnostic only. Normal PsWater SHEX stays byte-identical to the accepted shader. Production
cutover18 subsequently passed; its full spatial gate25 remains pending.

## Current contact and path filtering (2026-11-03)

Step19 normal water uses `scene_waterline.h` / `scene_waterline_gpu.h` and `gfx/waterline.hlsli`: CURRENT
owned native triangles/affine poses intersect each fluid plane. Segment/coplanar-triangle distances are
exact within the declared1m local query; CSR cells select candidates, not sampled distance approximations.
Native missing/partial alpha coverage stays explicit. Billboard cards are not physical fluid boundaries.
CPU cycle/device generation and all five owned buffers publish together. Normal DS/PS have no projected
shore-depth binding. Native bank/FOV/actual-barge gates pass both labs; production resolution/spatial25
and final cost are separate gates. Original3 inverse iterations/96 search steps/7 refinements are retained;
the normal wrapper uses compact loops, while the accepted diagnostic27 keeps its original compiled optics.

Step20 path filtering is integrated in SOURCE and compiling; it is not yet a native acceptance claim.
`gfx/ray_footprint.hlsli` transforms pixel ray differentials into the current affine/billboard-local triangle
and derives barycentric differentials. `gfx/native_texture_footprint.hlsli` uses original texture dimensions,
mips, addressing and native sampler anisotropy/bias/clamp. MIPFILTER NONE retains original level0. Native
MAXANISOTROPY/MAXMIPLEVEL/LODBIAS are copied during collection into owned SourceMaterial and GPU stage
extra.yzw; no borrowed sampler/device state or C ABI expansion is introduced.

The optional shader providers default to the existing raw-query behavior:

- `MML_RAY_OBSERVER(origin)` supplies a virtual observer independent of the ray origin for billboards.
- `MML_RAY_HIT_CONTEXT(object,origin,direction,index,distance)` prepares each candidate's footprint BEFORE
  opacity. `MML_RAY_SELECTED_CONTEXT(hit)` saves it when that candidate wins; `MML_RAY_FINISH_CONTEXT(hit)`
  restores the winning context before final layer shading. A later rejected candidate cannot change its mip.
- `MML_SURFACE_TEXTURE(primitive,state,uv,lod,valid)` selects a footprint-aware sampler; the default retains
  the explicit raw LOD sampler. Invalid required inputs remain UNKNOWN, never old-image-filled.
- `MML_SURFACE_ENCODED_RESULT(color,material)` can retain a successfully evaluated candidate. The optional
  `MML_SCENE_RADIANCE_ENCODED(primitive,bary,lod,color,material)` then supplies its selected result instead of
  decoding/filtering twice. These optional default-no-op hooks remain available to the original query;
  the water trial using them was superseded by geometry-first ordering below.
- `MML_NATIVE_TEXTURE_COMPACT` preserves original taps/mips and horizontal/vertical lerp order in rolled
  loops, reducing duplicate decoder code. Traversal/layer/outer-loop annotations are compile policy only;
  they do not change ray/iteration/layer/sample limits. FXC cost remains a pending integration gate.

Water carries its reflected virtual observer and pixel differentials through the retained wave interactions.
The original facet quadrature already integrates unresolved slopes; there is no second uniform roughness
blur on the native textures. No artistic settings or extra optical samples are introduced.

The world provider now uses `scene_radiance_ordered.hlsli`: BLAS/TLAS returns the next geometric intersection
without embedding the texture/material evaluator. The cursor is strict distance, descending source draw ID,
then primitive ID. Alpha-rejected triangles advance that cursor, not tMin by an epsilon. Thus other coplanar
draws and arbitrarily many rejected cutouts remain visible without consuming the32 contributing-layer bound.
Accepted native draws retain the original same-draw/four-ulp dedup. Dedup and encoded material evaluation
both run outside traversal; a rejected candidate may require another geometry query, not another optical ray.
Unknown source coverage/materials and the finite segment bounds remain explicit.
`MML_RAY_SELECTED_INSTANCE_INDEX(index)` saves the winning instance's buffer index. The layer callback
fetches its affine pose after duplicate rejection and derives the triangle footprint before its single
material/alpha evaluation. There is no candidate/final mip mismatch. Focused layer, alpha, coplanar and
unknown cases pass, including a mip change driven by the same world hit's ray footprint.

The water pass separates raster ray seeding, world transport, and composition into bounded shader
compilation units. The seed stores the selected water point, optical basis and the inputs needed to derive
ray/observer differentials from the original raster quad. The compute stage traces that seed against
the same frame's native geometry, texture pages and shore data. It accumulates the original facet
quadrature in order. Composition reads only the result at the same pixel of the current water draw.
The intermediate never supplies scene radiance or neighborhoods to the world query. It is recreated
with the water depth target on resize and released with the mod's graphics resources. The path uses
twelve float4 seed slices and three float4 accumulation records per render pixel. The seed can be written
in a single raster pass on D3D11 devices with rasterizer ordering, using an extra R32_FLOAT depth surface.
The two-pass render-target path produces the same seed layout on other devices. The frame-cost gate remains
separate from the fixed-camera image comparison in `WATER_LIGHTING_ARCHITECTURE.md`.

Sampler references: [native D3D9 states](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dsamplerstatetype),
[Direct3D11.3 filtering specification](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm),
and [ANGLE's native NONE/base-level handling](https://chromium.googlesource.com/angle/angle/+/9d737966acdf628a7cb4e12e4781179ea3be5d7b/src/libANGLE/renderer/d3d/d3d9/renderer9_utils.cpp).

## Shared packed storage and query domain

The compact snapshot builder can opt into shared packed storage. Read that publication through
`geometry.Nodes()`, `geometry.Triangles()`, `geometry.Meshes()` and `Surfaces()`. The default builder
still populates the original public vectors. Each shared publication leases fixed-length read-only
views of its BLAS and compact attributes. The builder can append new meshes into reserved capacity
after confirming the old lease ends at the current archive tail. No old view can observe that suffix.
If storage must grow, the existing prefix is copied into a new allocation before publication. The
normal packed-byte budget still governs when unreachable slots retire. Reserved headroom is limited
to one eighth of each payload and 2 MiB per array. Placement and material assignments remain fresh,
and the D3D11 publisher retains the lease associated with its current buffer spans.

The builder can recycle one private retired packed allocation up to 32 MiB.
Sole archive ownership establishes that all published Snapshot and Scene
leases have finished with it. A matching stable content-revision sequence
establishes the reusable prefix's exact node, triangle, mesh and compact
attribute bytes. Packing preserves that prefix in the retired allocation
and rebuilds its current tail. A retained reader keeps its original bytes
and causes a fresh destination allocation. Existing stale-byte and capacity
bounds also govern recycled destinations. DropSnapshot and Release drain
the worker before releasing the retired slot.

The optional CPU finalizer starts after native draw observation, all native data copies and the
first mesh jobs are complete. It works only on owned arrays and caches. The render pass calls
`Complete()` before requesting that cycle's lease. A skipped pass, device reset, unload or fault
drains or cancels the job without publishing a stale snapshot. Older hosts prepare synchronously.
The optional prepared callback can consume that owned snapshot before publication. Water uses
it to build its shore-contact model from copied plane heights. The render pass compares the
exact heights and both frame identifiers before upload, and rebuilds synchronously if they differ.

Material interning and placement assembly can also run on that worker. Native draw order and
texture IDs are preserved by copying owned packets before finalization. At the 8 MiB cap, pending
packets are assembled on the caller before continuing. Recently built meshes use a volatile tail;
only a verified prefix from the preceding owner can prove a partial GPU update.

Unresolved geometry can be staged separately in a bounded 8 MiB arena.
The observer checks a nondegenerate first triangle before assigning native
texture IDs and copies normalized vertices and indices from the borrowed draw.
The finalizer consumes those copies in draw order. Degenerate and oversize
packets use the original ordered path. Published materials, geometry and
instance identities still come from the current native collection.

Repeated native texture references may reuse the current publication's
assigned slot after checking the producer revision again. A collection
retry starts a new slot publication even within the same render cycle.
Without a reliable unchanged revision, the original texel verification
still runs. Identical ordered immutable banks share their GPU pages
without rebuilding the source-to-page remap.

Production water declares a 600 m total camera-to-water-to-object path and a 600 m surface-contact
region with a 1 m waterline search. The 608 m source box is still resident. On supporting hosts,
collection skips native visuals whose conservative bounds lie outside the 608 m horizontal disk.
The triangle inequality places every world ray point within that disk. Waterline filtering stays
inside it as well. Debug collectors and older hosts retain their original rectangular coverage.

## Retained source publication under evaluation

The 1.0 collector groups consecutive native object and lightmapped-object
preparations in a bounded 256-row scratch batch. Each batch publishes both
native buffer managers once before replaying the same queued entries in source
order. Other visual classes or replaced preparation functions keep the
per-visual path. The 1.2 native managers already track only locked buffers.
An interrupted batch drains pending native writes before restoring state.
This changes the scheduling of current-frame native preparation and retains
no geometry or resource pointers across frames.

Native city block activation and release cover sector, auxiliary and pooled-model roots. A pool slot
can be reused without changing its pointer. A retained key therefore needs a scene generation, a
block membership generation and a live resource revision. Keep static owned geometry on the worker/GPU.
The x86 collector should retain a bounded directory of native membership and invalidation records.

The topology-only probe in 1.2 verifies the native enumerator across PE relocation and checks
the live parent, child, sibling, virtual entry, frame type and visibility of each retained row.
Its fixed-camera run saved about 4 ms of collection while preserving source counts. A global
epoch makes unrelated transient releases rebuild the list, and the collector still prepares
each visual. Use affected membership to isolate invalidation from unrelated frame lifetime.

The resource probe rechecks tracked buffer allocation identity and revisions independently of the
current bindings. Its initial sweep proves about 10,500–10,800 native draw ranges. Many changes
during native preparation are identical writes. Comparing against the existing published copy
reduces these mid-collection revision changes from hundreds to tens, with the same draw counts.
The remaining writes still require validation before any retained publication can be consumed.

An isolated same-frame ray demand mask selected 3,241 of 12,067 current instance boxes
on the archived view, including boxes outside the main camera. Separately retaining
waterline candidates brought the union to 3,362. The filtered accelerator retained
the original draw and material IDs and reproduced every transport and final color bit.
The extra ray pass took about 6 ms. A useful source demand mechanism needs cheaper
coarse queries or reuse across verified current geometry, with coverage for downward
and continued rays.

The next prototype separates changed roots and per-frame dynamic draws from unchanged static blocks.
Native frame link, visibility and mesh changes dirty their affected records. Stream teardown, same-map
load and device recreation invalidate their generations. Buffer and texture writers still need current
producer revisions. Build a new coherent publication from changed records and confirmed existing records.
Retain native order for coplanar draws and exact material data, including transparent and unsupported
regions. Verify an unchanged block really bypasses preparation and callbacks, and that hidden and
reappearing actors, slot reuse and moved assets update within the same frame.

A scoped Rust native probe now demonstrates the preparation bypass for one active
sector block in both builds. Comparing the same frame to two full controls preserved
the remaining ordered geometry and effective material inputs. The raw inherited
texture pointer in a disabled stage can differ. A retained material identity must
exclude that unused binding while preserving active texture identity and content
revisions. The probe is a same-frame dependency check. Cross-frame ownership,
resource revision validation and worker/GPU publication are still required.

The offline Rust stable-bank publication keeps a local BLAS page and its
compact t26 surface attributes at the same triangle offsets. The current
ordered instance table changes only the root address in the archive test.
Texture IDs remain current through rewritten mip-page descriptors. The
existing DXR transport and final compose match the original image and raw
transport bitwise. This proves addressing for an owned bank. The live
producer still needs current resource generations, delta upload and
GPU-fence retirement before it can replace the existing path.

## Acceptance

Reusable uploader: actual GPU answers for moved/scaled/removed/hidden geometry, complete misses and unknown
regions; failed publication and device recreation do not permit stale reads. Live consumer: diagnostics from
actual native source and water rays in BOTH labs. Material provider: original mips/UV/alpha and final native
colors verified separately. Production: the required hidden building/barge/shore cases work with the old
reflection view unregistered and its images unbound. These are successive gates, not claims made by this spec.
