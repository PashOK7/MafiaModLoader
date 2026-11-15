## Delayed ragdoll in Old Prison, 2026-11-14

Two rifle hits in the owner's 1.0 run registered lethal damage 18 ms after firing. The mod created
each body in the same hit callback. The first body waited from 17:21:56.835 to 17:22:01.825 and the
second from 17:22:01.218 to 17:22:06.210. Both exhausted the 5.01 s simulation-time guard while the
Jolt queue was empty and the SDK still reported the body's 3 m box outside completed coverage.

Reproduced with the same rooftop Hoolig10 in both builds of mise17-vezeni. Separate the body
creation, host scan and Jolt import stages. Hundreds of visuals in the host's scope stayed deferred
after dozens of scans. A visual rejected by SpaceOf lost its piece entry and qualified as fresh
again. The same early range consumed the time budget on each scan, and the later candidates never
completed. The Jolt queue could be empty because the host had yet to publish coverage.

The host now remembers placement rejections until mesh layout, matrices or bounds change. Fresh
imports get a separate time budget and a rotating cursor. A failed scan clears its coverage. The
existing placement checks and the ragdoll's ready/timeout policy still apply. Out-of-place geometry
remains excluded. The change addresses starvation in this mission, and does not establish that
every rejected visual has a valid world transform.

Final comparison for the same actor and a 100-damage native hit event, both at save 465:

| Build | Before | After | Renderer after |
|---|---:|---:|---|
| 1.0 | 5.00 s, timeout | 0.05 s, ground ready | native D3D8 |
| 1.2 | 5.01 s, timeout | 0.04 s, ground ready | d3d8to9 |

Both queues reached zero deferred candidates. Fresh scans took roughly 13–15 ms on this fixture
and later scans generally took 1.5–2.4 ms. The timing fixture exercises the native hit callback
with equivalent bullet damage. Owner events and logs establish the rifle-specific timing.
Preimages and lab receipts are in build/scenery-ready-20261114.
