# Native source-region residency

Introduced 2026-11-02 at reflection step14. The later step18 integrated current-camera residency into the
water source path. Full reflection acceptance remains open; see [WATER_REFLECTION_PLAN.md](WATER_REFLECTION_PLAN.md)
and [STATUS.md](STATUS.md). Dated binaries/test counts below record specific completed gates.

The host owns copied per-mod world boxes through the ABI suffix scene_residency / scene_residency_state.
Requests combine across owners, are released on unload/fault and cleared with the mission generation. They
do not alter saved rendering settings, main projection/far plane, occluders, LOD or native camera position.
SDK callers register their region before main-world drawing and check readiness before scene collection.
The existing ordinary collection still reports inactive blocks as unknown; residency does not replace that
coverage check or make geometry beyond the declared ray segment complete.

Before main world/auxiliary views, the host activates required inactive native cache blocks. Selection uses
their conservative enclosing XZ circle bounds, matching the collector's missing-box coverage rather than
the main frustum. Native CacheTick still runs. Its ordinary deactivation cannot release a block requested by
any source owner; mission teardown/non-tick release remains native. Custom-distance reconciliation likewise
respects these requests. Removing/replacing a request schedules the native cadence to reconcile unowned
blocks using its own range and hysteresis; no forced removal of blocks needed by the main camera/other owner.

Native facts verified in both builds before implementation:

- ActivateBlock: Game.exe4025D0 /5A8A20, thiscall(cache,index). Block stride74h, active byte+4, bounding
  circle x/z/r at18h/1Ch/20h. Native pooled model duplication/placement is retained.
- DeactivateBlock:402A50 /5A8E80, thiscall(cache,index). Native sector/pooled-instance release is retained.
- Block walk helper402B60 /5A8F90 appends per-object700ms fade entries through AddTimeTransparentObject
  5457A0 /604CE0. It appends, not edits, the mission's existing8-byte records (begin/end at84h/88h).
- FinishTimeTransparentObjects5459B0 /604EF0 completes EVERY fade: unsuitable for a region owner. The new
  scoped activation records the previous count and completes/releases ONLY appended entries through the
  same native virtual transparency setter (+84h) and release (+0), preserving the existing prefix. This
  matters when game time is held; unrelated mission fades must remain untouched.

State contains copied bounds, owner count, native block counts, per-owner required/available counts and scene
generation. ready means required native blocks are currently active with no failed activation. It does not
prove textures/materials/deformations are supported: those remain separate snapshot validation/unknowns.
Failed/skipped collection cannot reuse an earlier scene snapshot. Older hosts lack the suffix and the normal
missing-box path remains explicit. Native acceptance passes12 EACH build (native-residency-v3.json):103
required blocks ready with default distance mode0, zero stream gaps, a real building behind main/old planar
cameras queried with its actual material, stable hit/color under camera turn/FOV, combined C/C++/Rust owners,
release/last-owner fault and same-mission restore. The unrelated fade prefix remains byte-identical. Same-
mission loads do not necessarily close the native mission: residency clears at SaveGameLoad/NewGame before
native loading, with a new scene generation; water's diagnostic snapshot/history also invalidates on GAME_LOADED.
residency-load-offline.log ALL PASSED. Initial failed native fixtures are retained, not counted as acceptance.

The step18 suffix scene_residency_camera(mod,radius) replaces the owner's absolute region with a cube around
the current resolved main camera. Radius0 releases; finite0.1..10000m is accepted. Resolution occurs after
FrameCameraBegin and before shadows/auxiliary views, using the same verified native camera reader as scene
views. No previous-frame eye is used. State exposes the applied absolute bounds; no camera means not ready.
Absolute requests retain their behavior and combine with relative owners. Native camera-residency-v1 passes
7 checks EACH build: invalid C arguments, C++ acceptance, three moved/FOV-varied resolved camera centers,
combined Rust owner, release with another owner retained and last-owner unload. Host428AC20B... is installed
and hash-verified in both labs. Full current offline validation ALL PASSED; residency-v4 repeats the complete
12/lab absolute-region, hidden-building/material, fade-prefix, owner/fault/load gates after the render-order
change. Normal material cutover remains18.
