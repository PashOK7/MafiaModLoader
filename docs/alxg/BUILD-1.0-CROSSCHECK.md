# Address comparison: theirs (mod-loader findings-B-api) vs ours (re crossbuild TSVs)

## Status against your main 4b8e352 (2026-10-19) - read this first

Everything below was measured against your `9ef8199`. Your `sdk/rust/mml/src/game/tables.rs` at `4b8e352` already
holds the 1.0 value for 17 of the 20 fields we offered (10 FILL + 10 FOUND), and **every one equals ours**:
collRayTest, humanBoneRayTest, pedAnimStart, collGridAdd, collGridMove, humanPlatform, tramCarOf, textGetOrNull,
frameUpdateWMatrix, bulletRay, pedBulletHit, controlsUpdate, textGet, menuSetValue, menuGetValue, gameNew, loadSave.
Two independent methods landing on the same 17 addresses is a check of both.

**Still 0 in your `LS3D_10`, and ours (RVA, LS3DF.dll 1.0 BE2F678C):**

| field | your 1.2 | our 1.0 | fact |
|---|---|---|---|
| drvClose | 0x701A0 | **0x6C7E0** | callee overlap 2/2 (weaker) |
| emitorSetParam | 0x3DFF0 | **0x3C460** | call-graph pair |
| ibAlloc | 0x5B6C0 | **0x58700** | call-graph pair |

**Retracted: "camRecompute 1.2 is wrong".** We checked it as a function; your header says it is a byte Render
reads ("the camera moved"), and your 1.0 value 0xA5628 is in now. Our objection does not stand.

Scope: every FUNCTION-address field in findings-B-api.md "Address table" (struct offsets, data globals and vtables skipped).
85 fields: 67 Game.exe (VA, base 0x400000), 18 LS3DF.dll (shown as VA = RVA + 0x10000000).

Method: 1.2 (GOG) value looked up in the GOG column of the crossbuild TSV (exact row first; if absent, containing
function = GOG start <= address < start + our size). Our 1.0 value = "ours" column of that row. Confidence = the
TSV "bytes" column (matched pattern length; 16/24/32 bytes, or neighbours/absent where listed).

Verdicts: MATCH = same 1.0 address. CONFLICT = different 1.0 address. FILL = their 1.0 is 0* and we have one.
NOT-IN-MAP = the 1.2 address has no row in our map. "(containing)" = the 1.2 site is mid-function; the containing
function is shown, and MATCH (containing) means their 1.0 site lies inside our mapped 1.0 function.

| field | 1.2 | theirs 1.0 | ours 1.0 (confidence, our name) | verdict |
|---|---|---|---|---|
| missionTick | 0x5407E0 | 0x5FFD40 | 0x5FFD40 (24, Unknown_UpdateWorld) | MATCH |
| missionClose | 0x5405E0 | 0x5FFB40 | 0x5FFB40 (24, Scene_UnloadMission) | MATCH |
| missionOpen | 0x5409D0 | 0x5FFF30 | - | NOT-IN-MAP |
| humanHit | 0x5762A0 | 0x496710 | 0x496710 (24, Human_ReceiveHit) | MATCH |
| actorDtor | 0x405D90 | 0x44B740 | - | NOT-IN-MAP |
| humanCtor | 0x56EFD0 | 0x48F430 | - | NOT-IN-MAP |
| testLine | 0x5C74D0 | 0x582600 | 0x582600 (24, Physics_RayCastSectors) | MATCH |
| frameWorldMatrix | 0x47ACD0 | 0x4F46C0 | - | NOT-IN-MAP |
| actorSetPos | 0x47AF50 | 0x5C86F0 | 0x5C86F0 (24, Unknown_SetVec36) | MATCH |
| frameSetPos | 0x460980 | 0x45F000 | 0x45F000 (24, Unknown_SetTargetPosition) | MATCH |
| sceneMoved | 0x47ADF0 | 0x5C8600 | 0x5C8600 (24, Scene_UpdateFrameInSector) | MATCH |
| vehicleDeactivate | 0x4C31E0 | 0x50EE60 | - | NOT-IN-MAP |
| carReset | 0x422EE0 | 0x469DD0 | - | NOT-IN-MAP |
| vehicleExternalForce | 0x4CBBD0 | 0x517F00 | 0x517F00 (24, Vehicle_AddForce) | MATCH |
| pedHit | 0x4588D0 | 0x4BD0D0 | - | NOT-IN-MAP |
| pedCarSweep | 0x44C6A0 | 0x4B0EB0 | - | NOT-IN-MAP |
| pedCarHit | 0x44CA50 | 0x4B1260 | 0x4B1260 (24, Unknown_OnPedRunOver) | MATCH |
| sphereSweep | 0x5D5940 | 0x590A40 | - | NOT-IN-MAP |
| pedStepReturn1 | 0x453619 | 0x4B7E69 | - | NOT-IN-MAP |
| pedStepReturn2 | 0x45394F | 0x4B819F | - | NOT-IN-MAP |
| save1d | 0x52CFCE | 0x4378E3 | 0x436CF0 (24, Unknown_ReadEnemyActionQueues; containing) | MATCH (containing) |
| save1dGo | 0x52CFD6 | 0x4378EB | 0x436CF0 (24, Unknown_ReadEnemyActionQueues; containing) | MATCH (containing) |
| save1dNone | 0x52CFE1 | 0x4378F6 | 0x436CF0 (24, Unknown_ReadEnemyActionQueues; containing) | MATCH (containing) |
| save1e | 0x52D117 | 0x437A2C | 0x436CF0 (24, Unknown_ReadEnemyActionQueues; containing) | MATCH (containing) |
| save1eGo | 0x52D11F | 0x437A34 | 0x436CF0 (24, Unknown_ReadEnemyActionQueues; containing) | MATCH (containing) |
| save1eNone | 0x52D12A | 0x437A3F | 0x436CF0 (24, Unknown_ReadEnemyActionQueues; containing) | MATCH (containing) |
| carExit | 0x573014 | 0x493484 | 0x492FF0 (24, Human_Update; containing) | MATCH (containing) |
| carExitGo | 0x573020 | 0x493490 | 0x492FF0 (24, Human_Update; containing) | MATCH (containing) |
| carExitDone | 0x57327F | 0x4936EF | 0x492FF0 (24, Human_Update; containing) | MATCH (containing) |
| waterWalk | 0x57B8F2 | 0x49BE52 | 0x49BBC0 (24, Human_MoveWithCollision_49BBC0; containing) | MATCH (containing) |
| waterWalkPast | 0x57BAB1 | 0x49C011 | 0x49BBC0 (24, Human_MoveWithCollision_49BBC0; containing) | MATCH (containing) |
| waterFall | 0x57C2C2 | 0x49C822 | 0x49BBC0 (24, Human_MoveWithCollision_49BBC0; containing) | MATCH (containing) |
| waterFallPast | 0x57C465 | 0x49C9C5 | 0x49BBC0 (24, Human_MoveWithCollision_49BBC0; containing) | MATCH (containing) |
| carWater | 0x4216D8 | 0x468567 | - | NOT-IN-MAP |
| carIn | 0x42658A | 0x46D87A | 0x46D630 (24, Vehicle_OnCollision; containing) | MATCH (containing) |
| carInPast | 0x426766 | 0x46DA56 | 0x46D630 (24, Vehicle_OnCollision; containing) | MATCH (containing) |
| vehicleStateSize | 0x4CEB30 | 0x51AAC0 | - | NOT-IN-MAP |
| vehicleSave | 0x4CEE00 | 0x51AD90 | 0x51AD90 (24, Vehicle_SerializeState) | MATCH |
| vehicleLoad | 0x4CFAB0 | 0x51BA40 | - | NOT-IN-MAP |
| collRayTest | 0x5C8440 | 0* | 0x583570 (24, Physics_RayCastWorld) | FILL |
| bulletRay | 0x5A935C | 0* | - | NOT-IN-MAP |
| humanBoneRayTest | 0x5D6FE0 | 0* | 0x592350 (24, Unknown_FindHitBodyPart) | FILL |
| pedBulletHit | 0x44CCF0 | 0* | - | NOT-IN-MAP |
| pedAnimStart | 0x459450 | 0* | 0x4BDC50 (24, Unknown_UpdatePedAnim) | FILL |
| collGridAdd | 0x5C36C0 | 0* | 0x57E7F0 (24, Scene_AddObjectToSector) | FILL |
| collGridMove | 0x5C3AC0 | 0* | 0x57EBF0 (24, Scene_MoveObjectToSector) | FILL |
| humanPlatform | 0x571B90 | 0* | 0x492000 (24, Human_CompleteVehicleTransition) | FILL |
| tramCarOf | 0x492D60 | 0* | 0x4E3FB0 (24, Unknown_FindNearestPathPoint) | FILL |
| playVideo | 0x55D460 | 0* | - | NOT-IN-MAP |
| loadSave | 0x6D4628 | 0* | - | NOT-IN-MAP |
| controlsUpdate | 0x4F0620 | 0* | - | NOT-IN-MAP |
| controlsKeyName | 0x4F1830 | 0* | - | NOT-IN-MAP |
| textGetOrNull | 0x60FBA0 | 0* | 0x57D8B0 (24, Text_LookupValue) | FILL |
| textGet | 0x60FB40 | 0* | - | NOT-IN-MAP |
| menuSetValue | 0x5EB070 | 0* | - | NOT-IN-MAP |
| menuGetValue | 0x5EB090 | 0* | - | NOT-IN-MAP |
| menuCreate | 0x5EA2E0 | 0* | - | NOT-IN-MAP |
| menuDestroy | 0x5EA690 | 0* | - | NOT-IN-MAP |
| menuCtor | 0x5EA1F0 | 0* | - | NOT-IN-MAP |
| menuPush | 0x5EAD50 | 0* | - | NOT-IN-MAP |
| menuClose | 0x5EAE10 | 0* | - | NOT-IN-MAP |
| menuAddUnder | 0x5EB7D0 | 0* | - | NOT-IN-MAP |
| menuFind | 0x5EB2F0 | 0* | - | NOT-IN-MAP |
| gameNew | 0x6106FC | 0* | - | NOT-IN-MAP |
| draw2d | 0x5BD8D0 | 0x5F86C0 | - | NOT-IN-MAP |
| hudOutText | 0x603880 | 0x5551A0 | 0x5551A0 (24, Ui_DrawFontString) | MATCH |
| hudTextSize | 0x6036D0 | 0x554FD0 | - | NOT-IN-MAP |
| render (LS3DF) | 0x10062030 | 0x1005ED80 | 0x1005ED80 (16, Render_RenderScene) | MATCH |
| frameRelease (LS3DF) | 0x1001AAE0 | 0x1001A8A0 | 0x1001A8A0 (16, Frame_Release) | MATCH |
| meshRelease (LS3DF) | 0x100313A0 | 0x100305B0 | 0x100305B0 (16, Visual_ReleaseMeshObject) | MATCH |
| modelOpen (LS3DF) | 0x100603E0 | 0x1005D070 | 0x1005D070 (16, Visual_OpenFile4ds) | MATCH |
| frameDup (LS3DF) | 0x1001C5B0 | 0x1001C350 | 0x1001C350 (16, Frame_Duplicate) | MATCH |
| frameLimit (LS3DF) | 0x10072100 | 0x1006E580 | 0x1006E580 (16, Graph_LimitFrameTime) | MATCH |
| frameSlot18 (LS3DF) | 0x1001B040 | 0x1001AE00 | 0x1001AE00 (32, Frame_SetWorldDir) | MATCH |
| frameSlot38 (LS3DF) | 0x1001BFD0 | 0x1001BD60 | 0x1001BD60 (16, Frame_GetChildByIndex) | MATCH |
| frameSlot3C (LS3DF) | 0x1001C2A0 | 0x1001C030 | 0x1001C030 (16, Frame_EnumChildren) | MATCH |
| materialOpen (LS3DF) | 0x1002C000 | 0x1002B540 | 0x1002B540 (16, Material_LoadTexturesFromFile) | MATCH |
| keyboardUpdate (LS3DF) | 0x100717F0 | 0x1006DD40 | 0x1006DD40 (16, Graph_PollKeyboard) | MATCH |
| mouseUpdate (LS3DF) | 0x10071E20 | 0x1006E2A0 | 0x1006E2A0 (16, Graph_UpdateMouse) | MATCH |
| emitorSetParam (LS3DF) | 0x1003DFF0 | 0* | - | NOT-IN-MAP |
| ibAlloc (LS3DF) | 0x1005B6C0 | 0* | - | NOT-IN-MAP |
| ibFree (LS3DF) | 0x1005B940 | 0* | - | NOT-IN-MAP |
| drvClose (LS3DF) | 0x100701A0 | 0* | 0x1006C7E0 (16, Graph_CloseDevice) | FILL |
| camRecompute (LS3DF) | 0x100A93F0 | 0* | - | NOT-IN-MAP |
| frameUpdateWMatrix (LS3DF) | 0x1001C7D0 | 0* | 0x1001C570 (16, I3D_frame::UpdateWMatrixProc) | FILL |

## Counts

| verdict | count |
|---|---|
| MATCH (exact) | 23 |
| MATCH (containing, mid-function site) | 15 |
| CONFLICT | 0 |
| FILL | 10 |
| NOT-IN-MAP | 37 |
| total | 85 |

## CONFLICTs

None. Every 1.2 address that our map covers gives the same 1.0 address as theirs, or fills their 0*.

## Caveats

- The 1.0 names are the TSV "name" column. 06-functions.md was not grepped per address.
- Containing-function checks use our function size as the GOG extent (approximate). The 15 containing MATCHes rest on that.
- loadSave (0x6D4628) lies in the data range; it is treated as a function because it is listed in the function-style block. Unverified.
- 37 NOT-IN-MAP: 33 Game.exe and 4 LS3DF. Most large functions are absent from the TSV (prologues differ between 1.0 and 1.2), so these are unknown, not conflicting.
- FILL rows (10) are candidates from byte-pattern matching, not confirmed by a second fact.

## mafia-re verdict (2026-10-19, commit 25f593f9)

Source: re/inbox/TO-MAFIA-RE-2026-10-14-mafia-mod-loader-upstream-10-fill.md. Ship CONFIRMED and FOUND rows; candidates are one fact only; their camRecompute 1.2 value 0x100A93F0 lies in LS3DF data (retracted 2026-10-19: it is a data byte by design).

**Method.** Two facts independent of the first-bytes map.
(a) `tools/crossbuild_calls.py` pairs functions by the CALL GRAPH. It aligns each paired caller's ordered call list
with its 1.0 twin (LCS on paired targets) and scores callee overlap, then propagates from the 1931 byte pairs
(game: 2517 pairs after 6 rounds; the graph pairs are in
`docs/09-topics/004-steam12-knowledge-base/crossbuild-graph-Game-EEBD15F7-to-gog-303eb95e.tsv`). Held-out test, 300
random byte pairs forgotten and re-found: game 130 right, 1 wrong, 169 missing; LS3DF 85 right, 0 wrong, 215
missing. A graph pair is about 99% right; about half of the functions are not reached.
(b) For a mid-function site, or to check your own 1.0 value: 32 bytes compared with relocated bytes and rel32
operands masked. Null: the same GOG bytes against 1500 random 1.0 function starts or 3000 random offsets give p99
4-6 equal bytes, max 7-17.
Names are ours, from `06-functions.md`.

**FILL - all 10 CONFIRMED** (byte pair forgotten first, so the graph had to find it by itself; none disagreed)

| field | 1.2 | 1.0 | our name | second fact |
|---|---|---|---|---|
| collRayTest | 0x5C8440 | **0x583570** | Physics_RayCastWorld | graph pair |
| humanBoneRayTest | 0x5D6FE0 | **0x592350** | Unknown_FindHitBodyPart | graph pair |
| pedAnimStart | 0x459450 | **0x4BDC50** | Unknown_UpdatePedAnim | graph pair |
| collGridAdd | 0x5C36C0 | **0x57E7F0** | Scene_AddObjectToSector | graph pair |
| collGridMove | 0x5C3AC0 | **0x57EBF0** | Scene_MoveObjectToSector | graph pair |
| humanPlatform | 0x571B90 | **0x492000** | Human_CompleteVehicleTransition | graph pair |
| tramCarOf | 0x492D60 | **0x4E3FB0** | Unknown_FindNearestPathPoint | 1 caller vote agrees (weaker: 1 of 3 callers paired) |
| textGetOrNull | 0x60FBA0 | **0x57D8B0** | Text_LookupValue | graph pair |
| drvClose (LS3DF) | 0x100701A0 | **0x1006C7E0** | Graph_CloseDevice | callee overlap 2/2 (weaker) |
| frameUpdateWMatrix (LS3DF) | 0x1001C7D0 | **0x1001C570** | I3D_frame::UpdateWMatrixProc | graph pair |

**NOT-IN-MAP - your non-zero 1.0 values: all 15 CONFIRMED, none rejected**

| field | 1.2 | 1.0 (yours = ours) | our name | fact |
|---|---|---|---|---|
| missionOpen | 0x5409D0 | 0x5FFF30 | Scene_LoadMission | graph pair |
| actorDtor | 0x405D90 | 0x44B740 | Actor_DestructBase | bytes 17/32 (null p99 5) |
| humanCtor | 0x56EFD0 | 0x48F430 | Human_ConstructActor_48F430 | graph pair |
| frameWorldMatrix | 0x47ACD0 | 0x4F46C0 | Actor_GetFrameWorldMatrix | bytes 28/28 |
| vehicleDeactivate | 0x4C31E0 | 0x50EE60 | Vehicle_SyncAfterTransform | graph pair |
| carReset | 0x422EE0 | 0x469DD0 | Vehicle_RefreshAfterMove | graph pair |
| pedHit | 0x4588D0 | 0x4BD0D0 | Traffic_OnPedHit | graph pair |
| pedCarSweep | 0x44C6A0 | 0x4B0EB0 | Unknown_FindProjectileHitTime | bytes 26/32 (null p99 5) |
| sphereSweep | 0x5D5940 | 0x590A40 | Physics_SweepSphereWorldGlobal | graph pair |
| pedStepReturn1 | 0x453619 | 0x4B7E69 | in Unknown_Ped_Update 0x4B7520 | bytes 25/28 (mid-site null p99 4) |
| pedStepReturn2 | 0x45394F | 0x4B819F | in Unknown_Ped_Update 0x4B7520 | bytes 25/28 |
| carWater | 0x4216D8 | 0x468567 | in Vehicle_UpdateActor 0x466B80 | bytes 22/28 |
| vehicleStateSize | 0x4CEB30 | 0x51AAC0 | Vehicle_CalcSaveSize | graph pair |
| vehicleLoad | 0x4CFAB0 | 0x51BA40 | Unknown_ApplyRemoteCarState (our name is weak; the graph pairs it with your vehicleLoad) | graph pair |
| draw2d | 0x5BD8D0 | 0x5F86C0 | App_RenderOverlayCallback | bytes 12/20 (null p99 6, max 7) |
| hudTextSize | 0x6036D0 | 0x554FD0 | Unknown_GetTextWidth | graph pair |

(16 rows: draw2d and hudTextSize had non-zero 1.0 values too.)

**NOT-IN-MAP - your 1.0 was 0: FOUND**

| field | 1.2 | 1.0 | our name | fact |
|---|---|---|---|---|
| bulletRay | 0x5A935C | **0x5E4CAC** | in Weapon_UpdateBullets 0x5E47C0, at the same offset +0x4EC | bytes 25/28 (next best 9) |
| pedBulletHit | 0x44CCF0 | **0x4B1500** | Unknown_OnPedHitByWeapon | graph pair |
| controlsUpdate | 0x4F0620 | **0x5D1F30** | Input_UpdateActions | graph pair |
| textGet | 0x60FB40 | **0x57D850** | Text_LookupString | graph pair |
| menuSetValue | 0x5EB070 | **0x56F1F0** | Ui_SetWidgetValue44 | graph pair |
| menuGetValue | 0x5EB090 | **0x56F1C0** | Ui_GetWidgetValue44 | graph pair (in 1.0 get comes before set) |
| gameNew | 0x6106FC | **0x624934** | `operator new` (??2@YAPAXI@Z) | bytes: `6A01 FF742408 E8.. 5959 C3` identical, i.e. `_nh_malloc(size, 1)`. It is the CRT operator new, not a game function |
| loadSave | 0x6D4628 | **0x671FB0** | g_gameState | a DATA object, not a function: the GOG code does `mov ecx, 0x6D4628` in 120+ places (a `this`); the paired Human_LoadSaveState, Human_InteractWithActor and Player_Update load `mov ecx, 0x671FB0` at the same sites (4 of 4) |
| emitorSetParam (LS3DF) | 0x1003DFF0 | **0x1003C460** | Visual_EmitorSetParam | graph pair |
| ibAlloc (LS3DF) | 0x1005B6C0 | **0x10058700** | I3d_AllocIndexBufferRange | graph pair |

**Candidates, NOT confirmed** (one fact only; do not ship them as addresses)

| field | 1.2 | candidate 1.0 | why it is only a candidate |
|---|---|---|---|
| menuAddUnder | 0x5EB7D0 | 0x56C600 Ui_CloneWidgetByType_56C600 | 5 caller votes, but 2 for 0x56F190 and callee overlap ties 4 functions at 3/3 |
| menuDestroy | 0x5EA690 | 0x56E850 Ui_DestroyAllWidgets | first bytes 29/32 (next 20); no graph fact. Start bytes fail the control on short UI functions (menuSet/Get got 9 at random places) |
| menuClose | 0x5EAE10 | 0x56F390 Ui_NotifyAllWidgets | 2 caller votes against 1 |
| ibFree (LS3DF) | 0x1005B940 | 0x10058230 I3d_IbFreeBlock | in GOG a 32-byte wrapper (`[arg]` -> test -> `jmp` to GOG 0x1005B1B0) between alloc and unlock; 1.0 has no function in that place (alloc is followed directly by Unknown_UnlockIndexBuffers 0x10058960), and 6 caller votes point at I3d_IbFreeBlock. The 1.0 callers probably call it directly |

**Not found** (no fact reached them): playVideo 0x55D460, controlsKeyName 0x4F1830, menuCreate 0x5EA2E0, menuCtor
0x5EA1F0, menuPush 0x5EAD50, menuFind 0x5EB2F0 (start bytes tie three Ui_ getters at 30/32: Ui_FindWidget
0x56F450 is a guess by name only). The GOG menu module was restructured, and its functions are short. A fact for
them would need the string or vtable references. Ask again if PashOK7 needs one of them.

~~Your 1.2 value is wrong~~ RETRACTED 2026-10-19: camRecompute (LS3DF) 0x100A93F0 lies in LS3DF's data section,
and that is right - it is a byte flag, not a function (your game_build.h: "a byte Render reads"). mafia-re's check
assumed a function. See the status section at the top.
