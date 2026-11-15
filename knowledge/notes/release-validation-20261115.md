# Final cumulative-update validation, 2026-11-15

The owner authorized completing preparation and publishing to the existing main branch. This resolves
the two failures retained in the earlier preparation. Source contracts and publication status are in
[STATUS.md](../../docs/STATUS.md); the changes are included in the cumulative patch notes.

## Index revisions: observed writers are a prerequisite

The old `vb.selftest` created a 64-byte D3DPOOL_MANAGED index buffer and required a nonzero native
revision. The host had installed writer hooks using the D3DPOOL_DEFAULT dynamic class. Its production
tracker deliberately checks the Lock, Unlock and Release entries before promising unchanged content.

An independent x86 Direct3D9 process compared three real buffers on the same device, without the host:

| Pool/usage | Same Lock | Same Unlock | Same Release | Writable/read-only Lock |
|---|---|---|---|---|
| Default, dynamic + write-only (installation probe) | yes | yes | yes | succeed |
| Default, dynamic | yes | yes | yes | succeed |
| Managed, usage 0 | no | no | yes | succeed |

This establishes a fixture expectation error on this backend. It does not justify registering unobserved
writers or assuming that all drivers use these classes. `TrackNativeIndex`'s conservative production gate
is unchanged. The ordinary geometry-read path remains necessary when no proof is available.

The revised self-test exercises both real classes:

- The default dynamic case requires observation and a valid revision with the correct size/stride.
- Both bound and explicit-buffer queries agree; an identical binding is retained.
- A writable lock makes the proof unavailable; after unlock an observed revision changes.
- A read-only lock preserves the revision. Unbind and final release invalidate it.
- A class with unobserved writers must keep its proof zero before and after writing. If another driver
  shares the observed methods, that class instead satisfies the same positive revision assertions.

`native_index_profiles` reports each case's pool, observed flag and before/after-write revisions. The
existing positive checks were retained, with an added negative case. The self-test passes on the final host.

Source: [vbrename.cpp](../../host/vbrename.cpp). The local independent probe and its receipt are under
`build/publication-final-20261115/index_classes.cpp` and `index-classes.log`; neither is a distributed asset.

## Stack targets: recognize the emitted SIB absolute jump

The RTSS compatibility change emits `FF 24 25 abs32` for the hook-chain jump. The shared crash/profile
resolver followed relative jumps and the older `FF 25 abs32`, so it stopped at the new chain thunk.
It now also reads the SIB form's absolute slot at byte3 and follows the slot's current target. Existing
trampoline handling, hop bound and fault guard remain. Hook execution/relinking bytes are unchanged.

The existing hooked-crash regression checks main's call through the actual patch/chain to hook_b,
hook_b's original call to hook_a and hook_a's trampoline call to the native target. It passes with the
new resolver. The ordinary/no-TLS/filter-chain crash regressions pass as part of the complete suite.

Source: [stack.cpp](../../host/stack.cpp), regression [run_tests.py](../../tests/run_tests.py).

## Final evidence and limits

- Complete offline suite: ALL PASSED, exit0 (`build/publication-final-20261115/offline.log`).
- Full x86 build and MCP smoke: pass. Unchanged x64/MCP source already passed its preparation builds.
- Native get-up/save/car geometry/contact/support/model/audio/Ogg tests: 55 passed, zero failures in
  each of 1.2 and 1.0. Closed only the test games; the owner's game/settings were not changed.
- Final host SHA-256: `B5FD1DE0A280F86F24F75A27291B5CC59DE29CE36B9BB4C970D1E823706A2003`.

These are regression results, not final visual acceptance of unfinished water work or attribution of all
historical owner incidents. Earlier failures remain in the journal and ignored local receipt folders.
