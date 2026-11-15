# A player's vehicle missing from physical car impacts

In Mafia10Fixes PID 31972 the owner reported the characteristic upwards/forwards body launch. The
latest pedestrian handover (actor 0FCEC5F4, car 0E469CD0) contained zero measured car contacts. Before
the first physics step its pelvis already had velocity (3.195, 3.171, 10.560) m/s. The current player
vehicle pointer was the same car. actor.meshes with hidden_root returned 15 pieces and 2442 triangles,
while world.near around that car omitted it and ragdoll.vehicle reported no physical body.

Two host discovery paths omitted the current vehicle when it was absent from mission/temporary/active
traffic lists. world_near now includes the native PlayerCar and LastCar references and deduplicates
them with the general lists. human_vehicle_contact validates these references and active traffic cars
as well as the mission lists. The kit removes only the occupied car's general-list references within
one synchronous game-thread command, queries discovery and invokes contact, then restores each value.
Both builds find exactly one copy and accept the native contact.

The port's OnHit fallback explicitly added a direction plus 0.3 upwards, with a minimum speed derived
from damage. That branch, Ragdoll::Impulse and the car_push setting have been removed. Car impacts
are marked vehicleImpact, which disables muscle targets including native death-animation following.
Both living knockdowns and lethal impacts use the physical vehicle/body contact. Other causes of
death retain their muscle policy.

The striking vehicle's actual model mover is registered before making its victim's physical body.
The same mesh/compound creation code is shared with ordinary mover registration. The impact reference
keeps the vehicle in the nearby body's mover list, independently of discovery refresh. Actor destruction
clears that reference. A failed extraction with no usable parts is retried instead of cached as success.
Vehicle shapes remain compounds of native model-part hulls, with the original wheel identities.

The final live block passes 29 checks per build, covering detached discovery/contact, absence of the
old launch, the original pre-hit pose handover, dodge collision continuity, and a real truck pushing a
corpse under the hidden-root bumper camera. The first block found the additional contact-validation
gap, fixed before the final block. Receipts and preimages are in build/car-impact-20261114.

The full offline suite still reports three failures in the buffer revision self-test, the demo-name
check and a crash-stack hook-target expectation. Its rerun reports the same three. These are recorded
in build/car-impact-20261114/offline-verification-2.log. The demo-name hits are in earlier renderer
notes, game_build.h and check_builds.py. This work has no all-tests-passed receipt.
