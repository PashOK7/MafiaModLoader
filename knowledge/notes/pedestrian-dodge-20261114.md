# Pedestrian model rewinds during a car dodge

The owner capture at 21:00 in Mafia10Fixes matched draw incident 26 to ragdoll handover 7 by pedestrian,
model and visual identity. Across draw frames 134855 and 134856 the same 93 ranges moved 2.388 m.
The move projected 2.357 m toward the car's left. The element and model were already at the new point
at pedCarHit entry, with native animation 323 active. The first physical pelvis step was about 3 mm.
The CPU snapshot labelled previous_world_draw also had the new position. Use the captured draw ranges
for the frame-to-frame observation. The old CPU snapshot does not establish actual draw continuity.

The two C_traffic_element::Tick root-motion branches call TestSphereB from element+0x34 (the start of
the clip, plus 0.6 m), with delta equal to the animation's proposed element+0x10 minus the clip start.
Addresses after the calls are 4B7E69/4B819F (1.0) and 453619/45394F (1.2). On collision the code writes
the stop into the element and adds its difference from the proposed location to animation-machine+0x114.
UpdateModel then copies the element position into the model. If a car moves into the already travelled
path, the old sweep can undo metres of accepted movement before the ragdoll receives the hit.

The live kit drives the actual 323 clip on a traffic pedestrian via the full native tick and adds a native
Bolt-Thrower across the already travelled path. Both builds reproduce a rewind of about 2.4 m before any
ragdoll step. The port now passes the tick-entry accepted root to only these two sweeps during dodge clips
0x142–0x149. The sweep endpoint and native collision resolution are preserved. The A/B regression verifies
a normal 3–4 cm step with the car behind and verifies that a car on the next part of the path still stops
the pedestrian. See tests/game/test_pedestrian_dodge.py.

The user capture did not record the individual sweep invocation. The causal reproduction matches its native
clip, pre-hit phase and size of movement. The earlier 16:04 and 16:37 reports cannot be attributed using their
captures alone. A subsequent native-animation move after a ragdoll already exists is a separate phase.

Evidence and preimages are local under build/pedestrian-teleport-20261114-2100.

The additional get-up smoke failure was traced separately. On the third repeated hit in the diagnostic
sequence, the selected pedestrian died. Both builds reported knockdown=false and puppet=false before
ragdoll.drop, which therefore removed a corpse and intentionally skipped GetUp. Its native corpse pose
then differed from the physical pose by 4.38/4.51 m. The get-up fixture now requires a healthy candidate,
unchanged model and a surviving physical knockdown. The corrected final test sequence passes 16 checks
per build. Raw records are in build/pedestrian-getup-sequence-results.json.

Port 87379006... is active in Mafia10Fixes PID 27624, generation 2. At hot reload there were zero
active bodies. The actual live DLL hash matches the installed artifact. A subsequent owner replay
with this version has yet to be observed.
