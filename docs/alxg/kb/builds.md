# builds

Sources: E:/!Claude/Mafia/mod-loader/kb/_raw/re-weapon-vehicle-render.md, kb/_raw/vr-motion-controls.md, kb/_raw/gog-patcher.md, kb/_raw/vr-mod.md, kb/_raw/vr-bench-and-practices.md (raw extracts, read 2026-10-14). Scope: build identity (md5, size, labels), per-build address maps and build-diff facts, and what the sources say about named mod builds. Game behaviour facts are in their subsystem files. Crossbuild address maps (TSV tables in E:/!Claude/Mafia/re/docs/09-topics/004-steam12-knowledge-base/): crossbuild-Game-EEBD15F7-to-gog-303eb95e.tsv (Game, 1.0 to GOG 303eb95e) and crossbuild-LS3DF-BE2F678C-to-gog-e211c850.tsv (LS3DF, custom to GOG e211c850); summary in findings-crossbuild-map.md. Reference build D6EA53FB: no line in any of the five raw extracts names it, so it has no entry here.

## Build 1.0 (EEBD15F7)

- Game.exe build EEBD15F7 is the 1.0 custom install; md5 EEBD15F7980FB07D5B1FBFA173889CE5 (raw header of vr-motion-controls.md, line 4; not a numbered raw line) | build EEBD15F7 (1.0) | src kb/_raw/vr-motion-controls.md:4 (raw header) | tag [data]
- Build default: EEBD15F7 = Game.exe 1.0 (custom build, deployed to E:/Games/MafiaVR) (raw header of vr-mod.md, line 3; not a numbered raw line) | build EEBD15F7 | src kb/_raw/vr-mod.md:3 (raw header) | tag [data]
- Custom Game.exe md5 EEBD15F7 (2.4 MB) is the legacy install, not stock; all its fixed VAs are invalid on GOG | build custom | src gog-patcher/docs/APPROACH.md:93 | tag live
- Custom exe carries SecuROM sections stxt774 and stxt371; GOG has none | build custom | src gog-patcher/docs/APPROACH.md:141 | tag [data]
- Custom Game.exe is itself patched (FOV at minimum); signature scans must mask patched bytes | build custom | src gog-patcher/docs/APPROACH.md:285-286 | tag [inferred]
- Same push 300.0f exists in custom at 0x00542FEB | build custom | src gog-patcher/docs/GRAPHICS-FIXES-CANDIDATES.md:49 | tag [disasm]
- Game.exe 1.0 and MafiaVR deploy: IDA export named Game-1.0.0.0-EEBD15F7 (4638 functions); LS3DF-noversion-BE2F678C (1943 functions). | build EEBD15F7 | src vr-mod/docs/03-tools.md:17-18 | tag [data]
- Draw distance patched in Game.exe on disk: 300 to 600 m. Patched file md5 24903FBA; backup Game.exe.drawdist.bak. | build EEBD15F7 | src vr-mod/docs/03-tools.md:43 | tag [data]
- Draw distance 300 to 600 m via Game.exe on disk. | build EEBD15F7 | src vr-mod/docs/03-tools.md:43 | tag [data]
- Head-camera global 0x006F9140 (decimal 7311680) = camera yaw for modes 7 to 0xB; head_cam=1 wrote the head yaw there in build 6b80226. | build EEBD15F7 | src vr-mod/docs/09-topics/001-c1-car-culling-behind/leads-2026-10-04.md:18-19 | tag [decomp]
- Game EEBD15F7 -> GOG 303eb95e crossbuild: 4985 functions, 1937 paired (39%), 2076 absent, 964 ambiguous, 8 dropped as sharing a target | build EEBD15F7 | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:26 | tag [data]
- Pairing method: mask bytes the file relocates and rel32 operands of call/jmp, grow the pattern until unique in the other build; game pairs are unique by first 24+ bytes only, so they are strong candidates, not facts | build EEBD15F7 | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:28 | tag [data]
- Positive control: all ten LS3DF pairs by hand are right; game run places Weapon_FireBulletsAndImpact 0x005E3DF0 at GOG 0x005A84A0, the address the archive gives for the shot | build EEBD15F7 | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:30 | tag [data]
- Large game functions absent from the crossbuild (Player_UpdateOnFoot, Hud_Render, Camera_UpdateController): prologues differ between 1.0 and 1.2 (frame sizes, inlining); next match by callers, strings, archive names | build EEBD15F7 | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:31 | tag [data]
- Example row: Ai_InitStimulusMemory ours 0x00401000 size 86 = GOG 0x004F6A60, 24-byte pattern; Unknown_SetProjectorParams 0x10001010 = same address in GOG (16-byte pattern) | build EEBD15F7 | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:~40 | tag [data]
- Game-run "ambiguous" = pattern matches more than one place; "neighbours" = placed by the paired neighbours' shift; "absent" = no match at 24+ bytes; these three are not pairs | build EEBD15F7 | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:30 | tag [data]
- Game-run pairs carry no control beyond the one positive control (0x005E3DF0); treat the other ~1900 as candidates until a second fact agrees | build EEBD15F7 | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:30 | tag [inferred]
- Game reads LOOSE models\ files (Tommy.4ds, stock md5 D0E5A042) over the scene data | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:251-252 | tag live
- Game.exe in the bench copy differs from the stand copy in two bytes: the far plane at 0x542FEE | build ? | src Mafia/vr-bench/docs/findings/2026-10-03-mc-build-5137760D-loses-its-d3d8-original.md:36-37 | tag data

## Build GOG 1.2

- GOG Game.exe md5 B500437F340B8A2F1E847E10BB974A06, size 2.2 MB, is the reference build the patcher anchors to | build GOG 1.2 | src gog-patcher/docs/APPROACH.md:55 | tag live
- Project brief says GOG "1.3"; Widescreen Fix pack identifies the GOG exe as MAFIA_1_2_ENG, so 1.2 is the working label | build GOG 1.2 | src gog-patcher/docs/APPROACH.md:14 | tag [inferred]
- Widescreen Fix version check: custom exe = DWORD [0x0063C000]==0x004C6150 (1.0 ENG); GOG exe = [0x00624000]==0x0044E120 (1.2 ENG); both read from PE sections | build custom / GOG 1.2 | src gog-patcher/docs/GRAPHICS-FIXES-CANDIDATES.md:26-30 | tag [disasm]
- Version resource is useless as an anchor: both exes report Master 1.0 / Illusion Softworks / MAFIA | build GOG 1.2 / custom | src gog-patcher/docs/APPROACH.md:129-130 | tag live
- GOG identity: buildId 50649417607880051, gameId 1595659240 (goggame-*.info), languages ["en"] | build GOG 1.2 | src gog-patcher/docs/APPROACH.md:99 | tag [data]
- GOG Game.exe is a different build, not custom minus patches: .text VSize 0x2212AA vs 0x23959C, .data VA 0x22E000 vs 0x246000, EntryPoint 0x2123B4 vs 0x226F1A | build GOG 1.2 / custom | src gog-patcher/docs/APPROACH.md:137-141 | tag [data]
- Ultimate ASI Loader as dinput8.dll is absent on GOG and must be installed by patcher; loader route proven by execution (our .asi ran, thread wrote log) | build GOG 1.2 | src gog-patcher/docs/CAMERA-STATE.md:18 | tag live
- IJoy.dll byte-identical in both installs (md5 D1A3FBEA...); IJoy DirectInputCreateEx IAT at base +0xD000 | build custom / GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:53 | tag [data]
- Game global VA_GAMEPTR: custom 0x65115C, GOG 0x63788C; car = *(*(ptr)+0x24) | build custom / GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:50 | tag live
- Probe splice site VA_CAR_PROBE: custom 0x5E2061, GOG 0x5A6619; 40 bytes around it identical in both builds | build custom / GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:51,74-76 | tag [disasm]
- Splice verified live: first byte at probe becomes 0xE8 (call); K_INIT 0x50 shows 8B50008B before patch | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:121-124 | tag live
- Engine-object layout (car struct) carried over custom->GOG unchanged: speed +0x2A0C, world pos +0x2A10/+0x2A18, frame ptr +0x58, ped list +0x2FC, contact list +0x18 | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:93-94,236-239 | tag live
- Car world position +0x2A10/+0x2A18 not yet verified on GOG | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:246 | tag ?
- FOV constant sites: custom 0xD3445, 0x10C8B0, 0x23D2EC; GOG 0x196275 (VA 0x596275), 0x0C0C10 (VA 0x4C0C10), 0x227974 (VA 0x627974) | build custom / GOG 1.2 | src gog-patcher/docs/FOV-PORT.md:20-24 | tag [disasm]
- Decoy 86.0f on GOG at 0x202407 (stock, in custom .bak too); patcher must NOT touch it | build GOG 1.2 | src gog-patcher/docs/FOV-PORT.md:27-31 | tag [disasm]
- Stock GOG Game.exe round trip: md5 B500437F -> patched 310E8E12 -> reverted B500437F; size 2355200 unchanged | build GOG 1.2 | src gog-patcher/docs/FOV-PORT.md:51-53 | tag live
- Patcher verification rule: Game.exe size+md5 must equal expected GOG build, else refuse with zero writes | build GOG 1.2 | src gog-patcher/docs/APPROACH.md:262-266 | tag [inferred]
- FFB build recognition is by 40-byte code signature at the probe site in memory, not md5; no match = log K_INIT 0x60 and stay inert (no patch, no threads, no DirectInput) | build GOG 1.2 / custom | src gog-patcher/docs/FFB-PORT.md:103-108,257-258 | tag live
- The FFB build-select doc gives two signature sizes (26-byte and 40-byte); the 40-byte figure is the measured one, the 26 is an earlier note | build GOG 1.2 / custom | src gog-patcher/docs/FFB-PORT.md:255 | tag ?
- Game ships its own force feedback in stock build (works); it was disabled in the custom install and replaced | build GOG 1.2 / custom | src gog-patcher/docs/FFB-PORT.md:10-17 | tag live
- Unresolved: prior note "game sends NO FFB" conflicts with the stock-FFB statement; not settled | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:36-40 | tag ?
- FOV 70.0f (00 00 8C 42) -> 86.0f (00 00 AC 42), three same-length float patches in Game.exe; 86 chosen for 16:9 at 1920x1080 by eye | build GOG 1.2 / custom | src gog-patcher/docs/FOV-PORT.md:8-10 | tag [inferred]
- FOV patch sites in GOG: player-fov 0x596275 (.text, call rel32), setup-block 0x4C0C10 (.text), rdata-table 0x627974 (.rdata) | build GOG 1.2 | src gog-patcher/docs/FOV-PORT.md:20-24 | tag [disasm]
- Camera on GOG: runtime FOV bet UNPROVEN; no camera object found on GOG yet; offline predictions refuted (vtable 0x9B690 wrong, 0x9B6D0 not confirmed) | build GOG 1.2 | src gog-patcher/docs/CAMERA-STATE.md:7-12,56-62 | tag live
- Car capture on GOG: *(*(0x63788C)+0x24) yields a sane car pointer; pointer live from t+2.8 s, stable afterward | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:150 | tag live
- Speed read at car+0x2A0C is correct on GOG (m/s); K_INIT 0x51 field d = speed x 100 | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:136-154 | tag live
- LS3DF car frame at car+0x58 gave no matrix until matrix offset fixed; car_mtx was 0 bytes; fixed value 0x0E4 in place of 0x164 | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:195-203,242 | tag live
- Draw distance: push 300.0f at 0x0059ABD0 (+1 float); stock values 300 view, 200 at 0x00402201+4, 230 at 0x00402209+4 | build GOG 1.2 | src gog-patcher/docs/GRAPHICS-FIXES-CANDIDATES.md:48-56 | tag [disasm]
- Frame limiter: push 16 at 0x005BE7CC (sleep 16 ms, about 60 fps cap); custom at 0x005F95BC | build GOG 1.2 / custom | src gog-patcher/docs/GRAPHICS-FIXES-CANDIDATES.md:79-82 | tag [disasm]
- Frame limiter file offset 0x1BE7CD on GOG reads 16 ms; custom does not match the signature | build GOG 1.2 / custom | src gog-patcher/docs/GRAPHICS-FIXES-CANDIDATES.md:174-177 | tag live
- GOG and custom Game.exe have different offsets for the same field (FFB matrix 0xE4 on GOG vs 0x164 custom); the GOG trainer chain (CWorld 0x647E1C, PP_ offsets) was REFUTED on 1.0 | build GOG 1.2 / EEBD15F7 | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:13 | tag live trap
- Game executable builds 1.0 and 1.2 are linked in different order, so the game run uses --no-order with 24-byte patterns or more | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:28 | tag [data]

## Build LS3DF BE2F678C

- Game.exe 1.0 and MafiaVR deploy: IDA export named Game-1.0.0.0-EEBD15F7 (4638 functions); LS3DF-noversion-BE2F678C (1943 functions). | build EEBD15F7 | src vr-mod/docs/03-tools.md:17-18 | tag [data]
- Scene renderer is LS3DF.dll Render_RenderScene 0x1005ED80 = I3D_driver::Render; stdcall(driver, scene), retn 8; driver vtbl slot 7 (GOG crossbuild: 0x62030) | build ? (LS3DF BE2F678C) | src re/inbox/TO-MAFIA-RE-2026-10-14-vr-motion-controls-scope-pass-recipe-on-ours.md:57 | tag [decomp]
- LS3DF BE2F678C -> GOG e211c850 crossbuild: 2007 functions, 1497 paired (75%), 145 absent, 352 ambiguous, 13 dropped off the monotone chain | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:25 | tag [data]
- LS3DF GOG-to-ours shift: monotone, between -0x7E0 and +0x3B80 | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:18 | tag [data]
- LS3DF pairs by bytes: Camera_BuildView GOG 0xBA30 = ours 0x1000C0F0 (46 of 48 bytes equal); UpdateWMatrixProc 0x1C7D0 = 0x1001C570 (46/48); SetVisibleRecMainPass 0x41DF0 = Render_ClassifyFramesToFrustum 0x10040060 (48/48) | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:10 | tag [data]
- LS3DF pairs by bytes (continued): I3D_driver::Render 0x62030 = Render_RenderScene 0x1005ED80 (45/48); frame-limiter 0x72100 = Graph_LimitFrameTime 0x1006E580 (42/48); BuildFrustumAndDispatchVisibility 0x437F0 = Cull_BuildFrustumPlanes 0x10041A60 (40/48) | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:10 | tag [data]
- GOG RVA 0xB400 ("camera projection builder"): NO MATCH; GOG prologue mov edx,ecx / lea ebx,[edx+0x1A4] absent in ours; archive pairing by role wrong or names another function | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:20 | tag [data]
- Camera_BuildProjection ours 0x1000BBE0 is stdcall (not the GOG thiscall shape) | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:20 | tag [data]
- LS3DF GOG 0x43000 SetVisibleRecSecondaryPass = ours Render_ClassifyFramesToFrustumRecursive 0x10041270 (46 of 48 bytes) | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:11 | tag [data]
- LS3DF GOG 0x4A1D0 GetSector (scene vtbl+0xB4) = ours Unknown_FindSectorAtPoint 0x10047D20; GOG 0x4E550 TestFrameSphereAgainstFrustum (scene slot 30) = ours Unknown_Scene_IsFrameInView 0x1004BFC0; both prologue-only matches | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:14 | tag [data]
- Archive names for LS3DF functions carry over to ours wherever a prologue is found; the two LS3DF builds are the same functions shifted by a monotone offset | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:20 | tag [data]
- Real engine version string in game log.txt: "LS3D Engine V3.95.1 Jun 24 2003 GetVer:395"; LS3DF exports GetEngineVersion | build GOG 1.2 / custom | src gog-patcher/docs/APPROACH.md:131 | tag live
- LS3DF.dll md5 differs: custom BE2F678C..., GOG 9244A71A...; grew by exactly 0x5000 | build custom / GOG 1.2 | src gog-patcher/docs/APPROACH.md:94,147 | tag [data]
- LS3DF export NAME set identical (49 exports); only RVAs differ, so vr-mod cross-reference method transfers | build GOG 1.2 / custom | src gog-patcher/docs/APPROACH.md:147-150 | tag [data]
- LS3DF export RVA deltas (custom->GOG): 0x01C570..0x01CD30 +0x260; 0x02C2E0..0x02F580 +0xAC0; 0x02F6C0..0x02F9C0 +0xA80; 0x057650 +0x2EC0; 0x067FE0..0x0681A0 +0x36F0 | build custom->GOG 1.2 | src gog-patcher/docs/CAMERA-STATE.md:38-47 | tag [data]
- LS3DF relocates on GOG: observed bases 0x024B0000 and 0x10000000 in different launches; never hardcode, use GetModuleHandleA("LS3DF.dll") + offset | build GOG 1.2 | src gog-patcher/docs/CAMERA-STATE.md:21 | tag live
- GOG LS3DF section map: .text 0x1000-0x9B000, .rdata 0x9B000-0xA6000, .data 0xA6000-0x1CC0C4 | build GOG 1.2 | src gog-patcher/docs/CAMERA-STATE.md:31-33 | tag [data]
- GOG LS3DF imports DINPUT8.dll; DirectInput8Create IAT slot at LS3DF base +0x9B000 (custom +0x97000) | build GOG 1.2 / custom | src gog-patcher/docs/FFB-PORT.md:52 | tag live
- GOG LS3DF imports d3d8.dll, so d3d8.dll slot stays free for a d3d8to9 shim; dinput8.dll is the ASI loader slot | build GOG 1.2 | src gog-patcher/docs/APPROACH.md:124-127 | tag [data]
- LS3DF frame matrix offset moved 0x164 -> 0x0E4 between builds, not a constant delta | build custom / GOG 1.2 | src gog-patcher/docs/FP-CAMERA-HANDOVER.md:42-45 | tag live
- LS3DF loads at a base other than the preferred 0x10000000 (seen 0x00D30000): use offsets from the module base, not preferred VAs | build ? | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:196 | tag live trap

## Build unknown

- Crossbuild TSV columns: ours, name, size, theirs (GOG address), bytes (pattern length or neighbours / ambiguous / absent) | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:~35 | tag [data]
- Purpose: a GOG database with names moved onto 3434 GOG functions; vr-motion-controls work carried to the FFB-port build | build ? | src docs/09-topics/004-steam12-knowledge-base/findings-crossbuild-map.md:37 | tag [data]
- Patcher 'refuses wrong version' is a design rule in the plan; no shipped-code citation in these docs | build ? | src gog-patcher/docs/APPROACH.md:262-266 | tag [inferred]
- A1 aim by head pitch: aim share slope 0.984 (= 180/183, i.e. 0.98 deg short at 60 deg); both rails exact; residual at return to 0 = 0.000; camera ray equals aim | build ? (deployed 873D98C3 mod) | src Mafia/vr-bench/USING-THE-BENCH.md:90 | tag live
- Direct3DCreate8 logged at 141 ms on the bench vs 141-407 ms in vr-mod's stand logs (the mod's own timing, not the game's) | build ? (873D98C3) | src Mafia/vr-bench/USING-THE-BENCH.md:540-541 | tag live
- Holster draw test with no holster build: 0 of 6 gestures changed the weapon on the deployed 873D98C3 (null) | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:87 | tag live
- Head-tracking lag: the submitted view carries the previous frame's pose; one frame late = 0.42 deg at p50, 2.45 deg at p95, 6.95 deg at most (on Alex's own head) | build ? (mod 873D98C3) | src Mafia/vr-bench/USING-THE-BENCH.md:85 | tag live
- Walking into a car on the mod build C605BE7C stalls for about 1.2 s (head 1.02 m ahead of body) | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:228 | tag live
- Holster grip gestures do NOT change the game's weapon on the 5137760D motion-controls build (10 and not raised throughout); the belt/shoulder zones pulse as built | build ? | src Mafia/vr-bench/docs/findings/2026-10-04-g1-5137760D-holsters-pulse-but-never-draw.md:17-29 | tag live
- Body and walker heading on a 180 turn: camera snaps back 180 to the walker's heading 6-10 frames after the weapon lowers; bullets follow the camera (0.0 deg) | build ? (B8DF1735) | src Mafia/vr-bench/USING-THE-BENCH.md:185-191 | tag live
- The walker never turns with the body on the B8DF1735 mod build (0 of 2160 deg); the engine camera does turn; camera snaps 25-56 deg about once a second on a half circle | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:185-190 | tag live
- Game.exe probes are chosen by Game.exe md5; the scene the bench menu path loads is missions\FREEITALY\scene2.bin | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:257,309 | tag live
- Game races its own start: bench.ps1 -StartGraceMs holds the first window (1..5000 ms) for builds that race; 6b80226 lost 7 of 7 starts on 2026-10-05 | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:444-446 | tag live
- Start-race defect of mod build 5137760D: saved original Direct3DCreate8 nulled by PatchIATInModule re-scan at ~100 ms; game dies 390 ms after start (exception c0000005 at 00000000) | build ? (5137760D) | src Mafia/vr-bench/docs/findings/2026-10-03-mc-build-5137760D-loses-its-d3d8-original.md:8-27 | tag disasm
- Start race under the old sandbox: the game's Direct3DCreate8 landed after the re-scan; after 2026-10-04 it starts at 172 ms logged (about 62 ms started) | build ? (5137760D) | src Mafia/vr-bench/docs/findings/2026-10-03-mc-build-5137760D-loses-its-d3d8-original.md:63-77 | tag live
- Mod-side pictures: the bench's markers drawn in eye captures show where the runtime says each hand is; pale yellow mod gizmo centroid 19.3-19.5 deg below the hand | build ? (5137760D) | src Mafia/vr-bench/docs/findings/2026-10-04-mc1-5137760D-bullets-leave-the-aim-point.md:94-97 | tag live
- A pitch error that swings with a 16-19 pull (about 20 s) period while nothing fed changes: lag-1 +0.72 vs shuffled p95 +0.28; identical pulls spread 56.9 deg at most | build ? (5137760D) | src Mafia/vr-bench/docs/findings/2026-10-04-mc1-5137760D-bullets-leave-the-aim-point.md:140-153 | tag live
- Live log of the car run: yaw=-0.0 almost everywhere and -45.0 at times while head turned 180 deg. Good build 6b80226 shows yaw following head (-45, -90, -135, -180 ...). | build ? | src vr-mod/docs/09-topics/001-c1-car-culling-behind/leads-2026-10-04.md:44-51 | tag live
- Key bindings live in the savegame profile (mafia000.sav, encrypted, md5 E00EF722); F held 200 ms leaves the Thompson in hand there; G drops it | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:437-439 | tag live
- Log: catlog.h (md5 C1F28706, as in vr-mod); files logs\mafia_re-YYYYMMDD-HHMMSS.log next to the ASI, 10 newest kept; line = [ms][category][LEVEL] text, flushed at once; mafia_re.ini [log] per category off|error|warn|info|debug|trace; categories hooks and camera | build EEBD15F7 | src re/docs/03-tools.md:186 | tag [data]

## Techniques

- Build identity is read from the lines above; technique lines are in techniques.md.

## Authoritative build table (mafia-re, 2026-10-19, commit 23aed795)

Source: re/inbox/TO-MAFIA-RE-2026-10-19-mafia-mod-loader-kb-build-identity.md (every held file hashed). Id rule: an
8-digit id is the md5 prefix (uppercase); topic 004 of mafia-re wrote GOG ids as sha256 prefixes (lowercase) - aliases.

| build (md5) | file | sha256 alias | crc32 | size | role |
|---|---|---|---|---|---|
| EEBD15F7 | Game.exe 1.0 | 30ec840d | 2C2AB966 | 2486272 | custom 1.0; reference for every 1.0 address |
| D6EA53FB | Game.exe 1.0 | 1f7811b4 | E0057CD9 | 2486272 | pre-FOV-patch .bak; same code as EEBD15F7 |
| 24903FBA | Game.exe 1.0 | 019f604d | E05E2A09 | 2486272 | vr-mod's patched 1.0; same code as EEBD15F7 |
| BE2F678C | LS3DF.dll 1.0 | fbbe56c5 | 4BC702AF | 851968 | 1.0 engine |
| B500437F | Game.exe GOG 1.2 | 303eb95e | 0FE21781 | 2355200 | GOG release, untouched |
| 310E8E12 | Game.exe GOG 1.2 | 74c61982 | 4080977A | 2355200 | GOG after gog-patcher's patch (--revert gives B500437F) |
| E93902B7 | Game.exe GOG 1.2 | 24036466 | 3712389E | 2355200 | gog-patcher work copy, not a release |
| 9244A71A | LS3DF.dll GOG 1.2 | e211c850 | 53DC8737 | 872448 | GOG engine |

For a loader that identifies builds by code signatures: EEBD15F7 / D6EA53FB / 24903FBA are one build "1.0";
B500437F / 310E8E12 / E93902B7 are one build "GOG 1.2"; they differ only in patched bytes.

## Conflicts - resolved by mafia-re 2026-10-19

- RESOLVED: GOG main exe 303eb95e vs B500437F - same file, sha256 vs md5 prefix.
- RESOLVED: GOG LS3DF e211c850 vs 9244A71A - same file, sha256 vs md5 prefix.
- RESOLVED: GOG version - the file says only "Master 1.0"; community and gog-patcher call it 1.2; "1.3" is gog-patcher's own alias.
- PARTLY: probe signature - 40 bytes per gog-patcher/docs/FFB-PORT.md:51,105 (identical in 1.0 and GOG); any 26-byte figure is gog-patcher's to confirm.
- RESOLVED: stock GOG sends NO force feedback (force-feedback project, 2026-08-04).
- RESOLVED: 310E8E12 = GOG exe patched by gog-patcher.
