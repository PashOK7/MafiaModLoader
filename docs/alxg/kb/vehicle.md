# vehicle

Sources: kb/_raw/re-weapon-vehicle-render.md (re/ read-only pass, 2026-10-14, vehicle section and the driving-input and seat lines); kb/_raw/vr-motion-controls.md (vehicle section and the car-pointer lines, 2026-10-14); kb/_raw/gog-patcher.md (vehicle-ffb section, FFB car facts on GOG 1.2); kb/_raw/vr-mod.md (seat truth lines); kb/_raw/vr-bench-and-practices.md (car world place lines). Each line keeps its own build, src and tag. A line naming two builds sits under the first-named build. No exact duplicates were found, so no lines were merged.

## Build 1.0 (EEBD15F7)

- Car actor constructor Vehicle_ConstructActor 0x00462AF0 sets vtable Class_0063BC08 and type 4 | build EEBD15F7 | src re/07-systems/vehicle.md:49 | tag [decomp]
- Vehicle_InitCarFromModel 0x00462D30 = slot 18 of the car class (vtbl+0x48); attaches the model, installs physics callbacks, collects AI, SM/LG, fuel and tank dummies; type comes from the model basename | build EEBD15F7 | src re/07-systems/vehicle.md:388 | tag [decomp]
- Car definition list tables\carindex.def: 164-byte records (83, index 0 empty), loaded into g_carTable 0x00671EF0 by Vehicle_LoadIndexTables 0x0055C370. Fields: +0 model prefix, +32 damaged model, +64 shadow, +96 display name, +128 class, +132 free-ride unlock letters | build EEBD15F7 | src re/07-systems/vehicle.md:420 | tag [data]
- Packed car id: bits 0-15 index, byte 2 colour 0..2, byte 3 variant 0..4 (0x55C710 / 0x55C7A0) | build EEBD15F7 | src re/07-systems/vehicle.md:423 | tag [decomp]
- Car records are 48 bytes after Vehicle_LoadCarTable 0x004637A0; tyre table 40-byte records from tables\tyres.bin via Vehicle_LoadTyreTable 0x00464430; tyre values multiplied by 1000 and degrees to radians | build EEBD15F7 | src re/07-systems/vehicle.md:388 | tag [decomp]
- Car variant record 4412 bytes (Vehicle_SwitchVariant 0x00465AD0); flat exported definition 4381 bytes (Vehicle_WriteVehicleRecord 0x00464FA0) | build EEBD15F7 | src re/07-systems/vehicle.md:65 | tag [decomp]
- Seat table is 160 bytes inside the car; Vehicle_GetSeatActor 0x00464EE0 is the most called (34 outside callers); Vehicle_SetSeatOccupant 0x004647C0 seats or unseats | build EEBD15F7 | src re/07-systems/vehicle.md:394 | tag [decomp]
- Human seat state: currentCar +0x98, seatIndex +0xAC, car being entered +0xA0, actionState +0x40C: 9 = seated, 5 = getting in or out; seatIndex 0 = driver | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-06-vr-mod-camera-modes-and-seat.md:66 | tag [decomp]
- Car physics part is car+0x70; frame of the car is car+0x68; car forward = row +0x30 of that frame (rows: X +0x10, Y +0x20, Z +0x30, position +0x40) | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-14-vr-motion-controls-car-forward-vector.md:50 | tag [decomp]
- Player_ApplyDrivingInput 0x004C91F0 gate: human +0x5D active, +0x40C in {1,2,9}, not (+0x1B0 and +0x1C8) | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-driving-analog.md:50 | tag [decomp]
- Analog drive: pairs 33/34 accelerate (second slot analog, raw [0,1]) -> Vehicle_SetThrottle 0x517460(part, v, 1); 35/36 brake -> Vehicle_SetBrake 0x517600 | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-driving-analog.md:53 | tag [decomp]
- Player chain while driving: mission = *(0x65115C); game = *(mission+0x24); player = *(game+0xE4). Field +0x24 of mission is the game object, not the player | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-dashboard-car-pointer.md:13 | tag [decomp]
- Driving HUD values: gear = car+0x648 (int), automatic = byte car+0x5B8, fed by Hud_SetField37344 each tick from Player_ApplyDrivingInput | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-car-dashboard-in-geometry.md:52 | tag [decomp]
- Car free-ride start: App_GameMain 0x005F99B0 cases 29-32 -> Scene_LoadMission("freeride") -> Unknown_ClearCarSlots 0x558020 -> Unknown_App_SpawnPlayerAndCar 0x5609A0 -> App_PutTommyInCar(0) 0x560EC0 | build EEBD15F7 | src re/07-systems/vehicle.md:408 | tag [decomp]
- Selected car is the packed id in dword_671ED0, written by Unknown_SetParams 0x005608F0 from the free-ride menu (0x57AD80, button 201); option flags to 0x671ED4, densities to 0x671EDC/E0/E4 | build EEBD15F7 | src re/07-systems/vehicle.md:411 | tag [decomp]
- The FREERIDE scene2.bin start script never names the car; changing it means writing 0x671ED0 before case 29 runs | build EEBD15F7 | src re/07-systems/vehicle.md:413 | tag [decomp]
- Car creation recipe (0x5609A0 / cheat opcode 0x185 with thunderbird00.i3d): Actor_CreateByType(4) 0x005FED70 -> Unknown_LoadModelCached 0x00448940 -> car vtbl+0x48 Vehicle_InitCarFromModel -> Unknown_AddActor 0x005E3220 -> Vehicle_ResetAfterPlacement 0x00469AD0 -> Vehicle_SimulateCarSettle 0x0051A920 -> Vehicle_SetSectorsDetached 0x00519D30 | build EEBD15F7 | src re/07-systems/vehicle.md:414 | tag [decomp]
- No script command creates a car from a model name | build EEBD15F7 | src re/07-systems/vehicle.md:419 | tag [decomp]
- Seat directly: Human_EnterCarSeat 0x0049E580 (ecx = human, ebx = 0, stack car and seat, ret 8); script HUMAN_FORCE_SETTOCAR opcode 0x69 | build EEBD15F7 | src re/07-systems/vehicle.md:424 | tag [decomp]
- Remove: script ACTOR_DELETE (0xDB) -> Actor_ScheduleDestroy 0x005E3400, deleted later by the mission loop (empty seats first); traffic pool cars are released by Unknown_RemoveCar 0x004F6F00 | build EEBD15F7 | src re/07-systems/vehicle.md:426 | tag [decomp]
- Ground placement: Unknown_GetGroundOffset 0x0048F330 casts a ray from pos + 1 up, 6 down; no free-spot helper exists | build EEBD15F7 | src re/07-systems/vehicle.md:428 | tag [decomp]
- Vehicle_RepairAndPlaceOnGround 0x00465760 re-seats a car on the ground below its stored position and starts the engine | build EEBD15F7 | src re/07-systems/vehicle.md:68 | tag [decomp]
- Vehicle_UpdateActor 0x00466B80 = main per-frame car actor update (distance flags, physics sync, drivers, damage, sounds, recorded input) | build EEBD15F7 | src re/07-systems/vehicle.md:76 | tag [decomp]
- Vehicle_Update 0x0052D480 and Vehicle_Update_52C3B0 run sounds, doors, skid marks, steering, throttle, torque, gears and rpm; which of the two runs first is open | build EEBD15F7 | src re/07-systems/vehicle.md:390 | tag [inferred]
- Vehicle_Explode 0x00468BC0 clears control flags, detonates, adds money, spawns debris and a wreck car, kills occupants | build EEBD15F7 | src re/07-systems/vehicle.md:78 | tag [decomp]
- Driver pistol is held in the LEFT hand (gun2) by Weapon_RefreshHeldWeapon 0x0049F180; lean-out is anim stance 0x200; hook 0x004A3AE0 is 6 bytes | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-14-vr-motion-controls-car-pistol-hand-and-hook.md:50 | tag [decomp]
- Global game object pointer 0x65115C (const pointer, image value 0x006F9440; no writer in .text); +0x10 = view/scene holder, +0x24 = mission manager | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/controlled-entity-chain.md:152 | tag [inferred] (runtime read pending)
- Mounted (turret-style) aim path A in fire function 0x4a4c28..0x5290: taken when CPlayer+0x98 type == 4, CPlayer+0xAC != -1 (seat index), +0x5E8 < 0 and +0x5E4 == 2 | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/weapon-object-field-map.md:151 | tag [disasm]
- Path A builds aim with its own normalize/recoil ops and drives effect object +0x6C0; does not reach trace 0x5e3df0 | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/weapon-object-field-map.md:151 | tag [disasm]
- CPlayer+0x584 argument to 0x4e82d0 on vehicle path | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/weapon-object-field-map.md:127 | tag [disasm]
- Vehicle entity sits on CPlayer+0x98 as target entity; type value of that entity drives camera origin branch | build EEBD15F7 (1.0) | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:211 | tag [disasm]
- Seated = actionState +0x40C == 9 with currentCar +0x98; enter and exit = actionState 5; driver = seatIndex +0xAC == 0 | build EEBD15F7 | src vr-mod/docs/09-topics/005-camera-onfoot-point/findings-2026-10-07.md:78-80 | tag [decomp]

## Build GOG 1.2

- Car capture on GOG: *(*(0x63788C)+0x24) yields a sane car pointer; pointer live from t+2.8 s, stable afterward | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:270 | tag live
- Speed read at car+0x2A0C is correct on GOG (m/s); K_INIT 0x51 field d = speed x 100 | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:136-154 | tag live
- Wheel enumerates and acquires without the user turning it to lock | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:130-132 | tag live
- DirectInput effect spring rises with speed; damper falls from 19999 at standstill to 5250 in motion (reference constants K_DAMP_DI_STATIC 20000, K_DAMP_DI_MOVING 5250) | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:173-181 | tag live
- K_STEER and K_TELEM are integer channels with fixed scalings, not floats; reading them as floats gives wrong conclusions | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:165-167 | tag live
- Slip reads 0.00 on a straight drive; it means the condition never occurred, not a broken signal | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:183-186 | tag live
- LS3DF car frame at car+0x58 gave no matrix until matrix offset fixed; car_mtx was 0 bytes; fixed value 0x0E4 in place of 0x164 | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:195-203,242 | tag live
- Prop contact list at car+0x0018 and pedestrian manifold list at car+0x02FC are live on GOG | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:237-240 | tag live
- Effect codes from GOG drive: 0xC0 sharp crash (1), 0xC7 F3 prop tap (5), 0xCB F4 pedestrian tap (3) | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:230-236 | tag live
- Two FFB stacks coexist: game's own FFB (works, collisions only) and ours (curbs, props, pedestrians, roll); stock build answers yes to impact on its own | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:10-22 | tag live
- Suppression of game FFB probably via our DISCL_EXCLUSIVE device acquisition; not verified | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:31-34 | tag [inferred]
- Two copies of the .asi in one folder fought over DirectInput exclusivity (neighbour history) | build custom | src gog-patcher/docs/FFB-PORT.md:33-34 | tag [inferred]
- Keyboard-driven drives are replayable; wheel input is not captured by KeyRec (keyboard + mouse only) | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:215-218 | tag live
- Collision solver walks car contact array; stride 0x5C; contact-begin at car+0x1D8, end at car+0x1DC | build GOG 1.2 | src gog-patcher/docs/COLLISION-CONTACT-RECORD.md:13-18 | tag [decomp]
- FFB ground-channel sign: negated at source (want = -(sat + detail + caster)); undo ground_invert = 1 | build GOG 1.2 | src gog-patcher/docs/DECISIONS.md:16-31 | tag live
- Caster steering damper ground_caster_d = 600; undo 0 | build GOG 1.2 | src gog-patcher/docs/DECISIONS.md:35-46 | tag live
- SAT slip-angle sign taken from kinematic front angle (beta + a*yaw/v - delta) with 30 mrad band; undo ground_sign_band = 0 | build GOG 1.2 | src gog-patcher/docs/DECISIONS.md:159-177 | tag live
- Hitched tick clamps dtMs into [0,100] instead of zeroing it | build GOG 1.2 | src gog-patcher/docs/DECISIONS.md:186 | tag live
- Damper proportion raised to GTA MT ratio (150 pct moving); falloff 15; caster square law 2400 exp 200 | build GOG 1.2 | src gog-patcher/docs/DECISIONS.md:202-217 | tag live
- Ground model ships on (ground = 1); key O toggles it; I restores | build GOG 1.2 | src gog-patcher/docs/DECISIONS.md:99-117 | tag live
- FFB constants are reference (v7.17 table) ported without retuning | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:272 | tag [inferred]

## Build unknown

- Car = same pointer as controlled entity ([[0x65115C]+0x24]); car frame at car+0x58 (signal for "in a vehicle"; periodically NULL while driving) | build ? | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:25 | tag live (FFB report) + [disasm]
- Car fields: speed +0x2A0C (m/s), position Y +0x2A10, Z (height) +0x2A14, X +0x2A18 | build ? | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:35 | tag live (FFB report)
- Car LS3D frame rotation 3x3 row-major at frame+0x164 (flagged as heap-layout dependent) | build ? | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:142 | tag live
- Room-scale: on foot real steps move Tommy through game collision (0x49D0C0); inside a car nothing moves; head shift in car not yet done | build ? | src vr-motion-controls/docs/08-state.md:75 | tag live
- Dashboard: real HUD pixels (speedometer, radar) from vr-mod hud_mode=4, quads placed inside the steering wheel outline, depth-tested behind wheel and hands | build ? | src vr-motion-controls/docs/08-state.md:106 | tag live
- Dashboard wheel object: *(car+0x70+0xCA4); dashboard plane 0.6 m from eye on the line to wheel, follows car, no wheel rotation | build ? | src vr-motion-controls/docs/08-state.md:106 | tag live
- Car stick-drive mapping (stick_drive=1) uses slots 34/36/38/40; not yet run on bench | build ? | src vr-motion-controls/docs/08-state.md:111 | tag design
- Current seat gate is statistical: car+0x58, 80-frame window, 70 percent, 1 s delay. Candidate to replace with actionState 9 | build ? | src vr-mod/docs/09-topics/005-camera-onfoot-point/findings-2026-10-07.md:81-82 | tag [inferred]
- Car world place: [car+0x68]+0x40 / +0x48 (carx/carz). The car object's own +0x40 / +0x48 held -5.03 / 0.003 and never moved | build ? | src Mafia/vr-bench/docs/findings/2026-10-12-carx-carz-never-moved.md:3-4 | tag live
- Car object slots +0x68, +0x320, +0x368 hold pointers; +0x68 points to a frame whose position tracks the scene camera's eye | build ? | src Mafia/vr-bench/docs/findings/2026-10-12-carx-carz-never-moved.md:14-17 | tag live
- Car drives 9.69 m with the eye 0.45-0.50 m from the car's seat throughout (driver seat) | build ? | src Mafia/vr-bench/docs/findings/2026-10-12-carx-carz-never-moved.md:20-21 | tag live
- Taxi steering: arrows drive, Left steers, A does not; Left held 3 s gave heading +36 then -25 deg in one run | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:227 | tag live
- Walking into a car on the mod build C605BE7C stalls for about 1.2 s (head 1.02 m ahead of body) | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:228 | tag live

## Techniques

- Walk all actors: game = *(*(0x65115C)+0x24); begin = *(game+0x124), end = *(game+0x128), 4-byte pointers; allocator slot +0x120 | build EEBD15F7 (1.0) | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-throw-small-props-in-the-world.md:62 | tag [decomp]

## Conflicts

- CONFLICT: car frame pointer: car+0x68 (inbox/TO-MAFIA-RE-2026-10-14-vr-motion-controls-car-forward-vector.md:277, 1.0) vs car+0x58 (vr-motion/docs/for-mafia-re/fields-and-globals.md:25, build ?; GOG FFB-PORT.md:195-203 also reads car+0x58 for the car LS3D frame)
- CONFLICT: object at [[0x65115C]+0x24]: game object, player = +0xE4 (inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-dashboard-car-pointer.md:27, 1.0) vs car pointer (vr-motion/docs/for-mafia-re/fields-and-globals.md:25, build ?; gog-patcher/docs/FFB-PORT.md:50 GOG car = *(*(ptr)+0x24)) vs mission manager (vr-motion/docs/decompile/controlled-entity-chain.md:152, 1.0, inferred)
- CONFLICT: car world position: car+0x2A10 / +0x2A18 (vr-motion/docs/for-mafia-re/fields-and-globals.md:35, build ?; gog-patcher/docs/FFB-PORT.md:93-94, GOG) vs [car+0x68]+0x40 / +0x48 (vr-bench/docs/findings/2026-10-12-carx-carz-never-moved.md:3-4, build ?)
