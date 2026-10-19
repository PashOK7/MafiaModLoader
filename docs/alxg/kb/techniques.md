# techniques

Sources: E:/!Claude/Mafia/mod-loader/kb/_raw/re-weapon-vehicle-render.md (section techniques, plus input line 175), kb/_raw/gog-patcher.md (section techniques, plus key-map line 143), kb/_raw/vr-motion-controls.md (section techniques), kb/_raw/vr-mod.md (section vr-techniques), kb/_raw/vr-bench-and-practices.md (sections headless-testing and practices). Each line keeps its raw build, src and tag. Lines are grouped by source area below; every line is verbatim from its raw extract.

## Build 1.0 (EEBD15F7)

- Build-specific technique lines are listed under ## Techniques with their build field kept in each line.

## Build GOG 1.2

- Build-specific technique lines are listed under ## Techniques with their build field kept in each line.

## Build unknown

- Build-unspecified technique lines are listed under ## Techniques with their build field kept in each line.

## Techniques

### re (weapon, input, render, hooks)

- Hook recipe: check the bytes at the hook address before patching (refuse out loud on mismatch); patch only whole non-relative instructions, at least 5 bytes; stub saves registers and flags, calls handler, restores, runs the stolen instructions, jumps back | build EEBD15F7 | src docs/09-topics/003-mod-api/point-camera-onfoot.md:79 | tag [decomp]
- Control test for any hook: a stub that corrupts must be caught (camera.onfoot control caught 51/51); a pass-through stub must be identical 1999/1999 | build EEBD15F7 | src docs/09-topics/003-mod-api/point-camera-onfoot.md:105 | tag [decomp]
- x87 discipline: a handler must leave the x87 stack empty when the next instruction is a call; game runs at 24-bit precision without FPU_PRESERVE, so test at 24 and 53 bits | build EEBD15F7 | src docs/09-topics/003-mod-api/point-camera-onfoot.md:79 | tag [decomp]
- Input seam: wrap Input_UpdateActions (call original, then write VALUE / PRESSED of the slot if this == 0x6F92E0); emulate a key as the keyboard writer does (VALUE held, PRESSED on the first tick only) | build EEBD15F7 | src re/07-systems/input.md:220 | tag [disasm]
- Render seam (Tommy arm rig): E9 rel32 at 0x5F9811 plus ten NOPs; stub saves, performs the rig write, runs the five stolen instructions (15 bytes: 8B 0D 5C 11 65 00 / 8B 41 10 / 50 / 8B 10 / FF 52 ...), restores, jumps to 0x5F9820. Probe on the bench | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-07-vr-motion-controls-render-seam-recipe.md:41 | tag [disasm]
- Clone a body part: drv = *(0x6F9520); CreateFrame via vtbl[0x48/4] with type 'OBJ_'; Duplicate via vtbl[0x3C/4] from a source found by FindChild vtbl[0x38/4]; LinkTo via vtbl[0x2C/4]; register in the scene with vtbl[0xBC/4] | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-clone-part-frame.md:18 | tag [decomp]
- Frame tree walk: name, parent, first child, next sibling, flags and local position/quaternion per I3D_frame table in frame.md (tree dump for arm bone names) | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-07-vr-motion-controls-arm-rig-render-only.md:41 | tag [decomp]
- Read a 4DS model without the game: python tools/fourds_dump.py <model.4ds> <out.json> (read-only; JSON with materials and frames) | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-4ds-reader-tool.md:4 | tag [data]
- Reload split for a mod: eject = Unknown_HandleReloadAnimEvent(h, 73) with sequence at type 3, zero clip per the 0x2000000 rule, HUD via Unknown_SetRatioText; insert = event 77 then 74, then Weapon_Reload(h+0x480), +0x4A2 = 0 | build EEBD15F7 | src re/07-systems/weapon.md:314 | tag [decomp]
- Do not call Human_BeginReload from a mod that wants eject and insert separately: it plays the whole animation and moves the ammo at the end by time | build EEBD15F7 | src re/07-systems/weapon.md:314 | tag [decomp]
- Pump casing suppression: write *(int*)(player+0x20C) = 0 every frame before Human_UpdateCombatAnim for player id 11; mind the heavy vs light casing rings (game+0x5B4 vs game+0x538) | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-13-vr-motion-controls-pump-shot-casing.md:234 | tag [decomp]
- Keep a thrown body asleep and drive its frame: flags +0x2EC < +0x2F0 and +0x9C <= 4000 keep the step off; on release set +0x24..+0x2C and +0x84..+0x8C to the frame world position; costs no model reload (removing the actor frees its model) | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-throw-hold-prop-in-hand.md:123 | tag [decomp]
- Back-kill suppression (player only): two sites with one-byte patches (0x4918D3 7A 25 -> EB 25 for humans; 0x4B18C5 7A 17 -> EB 17 for traffic pedestrians), or a 6-byte jump to a cave for the player only | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-melee-backstab-kill-site.md:51 | tag [decomp]
- Stop the object-level pickup path with action points: swap one probe point argument of 0x61BF50 | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-13-vr-motion-controls-player-use-target.md:226 | tag [decomp]
- Frame scale in a mod: write +0x90/+0x94/+0x98 and clear +0xAC bit 0x4; use flags & 0xBFFFFEC3 | 0x40000008 | build ? (LS3DF BE2F678C) | src re/inbox/TO-MAFIA-RE-2026-10-14-vr-motion-controls-frame-scale-drawn.md:316 | tag [decomp]
- Body root: the model frame player+0x68 local position is copied from feet player+0x24 on every Human_Update at 0x493DC0; the crouch lowers the neck, and the on-foot camera (mode 3) is neck + 0.2 m, so crouch drifts a rig built on the camera | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-13-vr-motion-controls-crouch-body-height.md:187 | tag [decomp]
- Input device pitfall: a keybd_event scan 0x1D without KEYEVENTF_EXTENDEDKEY is LEFT Ctrl, not bound to crouch by default (crouch is RCtrl, extended) | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-13-vr-motion-controls-player-crouch-call.md:218 | tag [decomp]
- Mouse injection: use mouse_event(MOUSEEVENTF_MOVE) for relative turn; SetCursorPos alone turns nothing; mouse BUTTONS from SendInput never reach DirectInput | build EEBD15F7 | src re/07-systems/input.md:119 | tag live
- Compiled pseudocode verification: test with NaN inputs carrying payloads; keep a control that must fail (diff_control) | build EEBD15F7 | src re/docs/03-tools.md:132 | tag [data]
- Aim-point ray: the camera-centre ray is cast 100 m along the camera forward (Physics_RayCastSectorsMasked from the camera world position) | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-player-burst-aim-climb.md:69 | tag [decomp]
- Recoil: a player shot adds weaponDef+0x84 to +0xAD0 and the same value goes to game+0xD8; decay 1.0/s (0.05/s in camera mode 5); the follow camera adds 0.2 * forward-scaled recoil to its position | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-player-burst-aim-climb.md:67 | tag [decomp]
- Wall-in-front check for a lowered weapon: Unknown_IsFrontClear 0x004AAD00 (ecx = human) returns 1 when CLEAR; Player_Update 0x004CB803 raises a ray from frame +1.5 m | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-wall-lowers-the-weapon.md:170 | tag [decomp]
- Idle lower timer runs only while +0x1E5 != 0, +0x1FE == 0, +0x1EC == 0, +0x1E4 == 0, +0xAF5 == 0 and game+0x5C != 5 (Player_Update 0x004CB7D1) | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-player-weapon-lower.md:91 | tag [decomp]
- Pedestrians are not Humans: 512-byte entries of the pedestrian manager, actor type 18 (health +0x15C; hits via type-19 proxy slot 31) | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-14-vr-motion-controls-traffic-pedestrian-list.md:378 | tag [decomp]
- Walk all actors: game = *(*(0x65115C)+0x24); begin = *(game+0x124), end = *(game+0x128), 4-byte pointers; allocator slot +0x120 | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-throw-small-props-in-the-world.md:148 | tag [decomp]
- A pickup of a lying weapon is not a type-37 actor once it rests (Unknown_OnDetectorStateChange 0x48A1D0 destroys it); the game's pickup is the use action in Player_HandleVehicleEntry 0x4CBD00 via Unknown_CollectNearbyPoints | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-13-vr-motion-controls-pickup-weapon-body-by-hand.md:210 | tag [decomp]
- Drop the held weapon the game's way: end0 = *(game+0x128); call Human_DropCarriedOrWeapon 0x004A6300 (thiscall, no stack args) only when not carrying a box or body | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-12-vr-motion-controls-throw-held-weapon-as-pickup.md:116 | tag [decomp]
- Human_StoreOrDropPickup refuses melee items (bat id 4 has flag 0x200000 long item, coat slot holder+0x80 only); Human_AddWeaponToInventory 0x55B2D0 | build EEBD15F7 | src re/inbox/TO-MAFIA-RE-2026-10-14-vr-motion-controls-stow-melee-item.md:371 | tag [decomp]
- Live measurement practice from the Mafia answers: check every claim against its build (EEBD15F7 vs LS3DF BE2F678C vs GOG); decompiler output alone is not checked by a run | build ? | src re/07-systems/weapon.md:~ (header notes) | tag [inferred]
- A mod's write seam: wrap Input_UpdateActions; the first 8 bytes 83 EC 58 / 53 / 8A 5C 24 60 are whole and non-relative; write between the return at 0x5F9605 and Unknown_UpdateWorld at 0x5F96DA where Player_Update runs | build EEBD15F7 | src re/07-systems/input.md:220 | tag [disasm]

### gog-patcher

- Signature remap: disassemble both exes with llvm-objdump, build signatures from whole instructions, wildcard call/jmp rel32, short jumps rel8 and any in-image dword operand | build custom / GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:55-63 | tag [inferred]
- Verify a remap by byte identity around the site (40 bytes, 0 differences) and by reference frequency of the global, with neighbour control addresses | build custom / GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:69-87 | tag live
- Test a signature's uniqueness offline: exactly one entry matches each image with 0 differences, other differs in all bytes | build custom / GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:105-108 | tag live
- Pair sites across builds by context distance; accept only when each signature has exactly one hit (FOV) | build custom / GOG 1.2 | src gog-patcher/docs/FOV-PORT.md:33-37,45 | tag live
- Bracket unknown offsets between export anchors; a piecewise delta table gives brackets, not answers | build custom / GOG 1.2 | src gog-patcher/docs/CAMERA-STATE.md:36-53 | tag [inferred]
- Camera object identity test: VIEW 3x3 is transpose of WORLD 3x3, plus orthonormality, excluding identity | build custom (vr-mod) / GOG 1.2 | src gog-patcher/docs/CAMERA-STATE.md:77-81 | tag live
- Judge FOV by projection matrix, not screen (GDI capture returns black in fullscreen exclusive) | build GOG 1.2 | src gog-patcher/docs/CAMERA-STATE.md:121-124 | tag live
- Hardware watchpoint (DR0) on quaternion across all threads to find read order before the detour | build custom (vr-mod) | src gog-patcher/docs/FP-CAMERA-HANDOVER.md:122-124 | tag live
- Patch round trip: md5 stock -> patched -> revert -> stock, with .bak written before first write and kept on re-apply | build GOG 1.2 | src gog-patcher/docs/FOV-PORT.md:51-53 | tag live
- Live verification: unattended launch-to-quit run (22 s) reads game log and FFB diag output | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:99-101 | tag live
- Diagnostic-only build first (effects off, snapshots on), then enable effects | build GOG 1.2 | src gog-patcher/docs/APPROACH.md:226-233 | tag [inferred]
- Silent fallback is the worst behaviour for a patcher: report loudly when a module cannot apply | build ? | src gog-patcher/docs/FP-CAMERA-HANDOVER.md:205-206 | tag [inferred]
- A write to a matrix before the quaternion is refreshed is overwritten downstream; set dirty bit or write quaternion | build custom (vr-mod) | src gog-patcher/docs/FP-CAMERA-HANDOVER.md:127-130 | tag live
- Encode integer telemetry channels with fixed scalings; do not read them as floats | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:165-167 | tag live
- A feel test on the GOG build must first establish which stack is talking (run with our asi absent) | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:24-30 | tag [inferred]
- Count-of-contacts is a cleaner signal than thresholding a derived speed | build GOG 1.2 | src gog-patcher/docs/COLLISION-CONTACT-RECORD.md:115-124 | tag [inferred]
- Same-length patches only in scene2.bin; insertions corrupt LS3D multi-level size fields | build custom | src gog-patcher/docs/APPROACH.md:281-282 | tag live
- Do not copy custom-install data wholesale; derive patches from GOG's own files | build custom / GOG 1.2 | src gog-patcher/docs/APPROACH.md:283-284 | tag [inferred]
- Keyboard recordings replay; wheel recordings do not | build GOG 1.2 | src gog-patcher/docs/FFB-PORT.md:215-218 | tag live
- Test a guard or patch by asking it to permit what it should refuse and vice versa | build ? | src gog-patcher/docs/APPROACH.md:86-87 | tag [inferred]
- Keep the reference install untouched; patch only a working copy; re-mirror from reference with /PURGE, never /MIR | build GOG 1.2 | src gog-patcher/docs/APPROACH.md:58-69 | tag [inferred]
- Before binding a key: check keyboard has it, grep GetAsyncKeyState and key_ reads, check game defaults | build ? | src gog-patcher/docs/KEY-MAP.md:101-107 | tag [inferred]

### vr-motion-controls

- Do not hook the function entry for aim: writes to +0x200 happen after the entry and before the read; hook the last write site before the read (0x4a555c) or the argument of the shoot entry | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/shot-spawn-call-graph.md:31 | tag [inferred from listing]
- Single-seam option: detour Fire 0x4a3ae0 and replace pAimPoint argument; covers the first bullet only, bullets 2..n enter 0x4a4b70 without it | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/shot-spawn-call-graph.md:111 | tag [inferred]
- Prefer calling Fire(bStart=1) on the press edge and Fire(0) on release; send only edges, let the animation pace the repeat (rate limit and ammo untouched) | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/trigger-to-fire-seam.md:12 | tag [inferred] (design)
- Calling Fire directly bypasses DirectInput; synthetic mouse-button (mouse_event) rejected as seam: needs a second seam for aim | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/trigger-to-fire-seam.md:184 | tag [inferred]
- Hook must be a whole-instruction stolen prologue: check no branch lands inside the stolen range (grep each jump target) | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/shot-spawn-call-graph.md:141 | tag technique
- Bullet-spawn argument rewrite at 0x4a5cea is safe: the callee copies origin and dir by value into its frame, so overwriting the argument block does not alias game state | build EEBD15F7 (1.0) | src vr-motion-controls/docs/aim-map/SUMMARY.md:70 | tag [inferred]
- Watchpoint (invwatch) on one slot of one object, with call stacks, names the real writers; static "dead site" claims need it | build ? | src vr-motion-controls/docs/aim-layer/SPEC.md:155 | tag technique (run 059)
- Offset coincidence trap: a write to an offset in a smaller class's constructor produced false leads for CPlayer+0x210; check the class size before accepting a writer | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/weapon-frame-writers.md:21 | tag trap
- Read .rdata float constants by byte offset (dataat tool); decode 0x63B918 = pi/2 as the arc limit | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/shot-spawn-call-graph.md:226 | tag technique
- AngleTo is __stdcall with this as the last pushed argument; S_vector::Set is __thiscall and returns this in eax | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/shot-origin-and-firing-arc.md:348 | tag [disasm] trap
- llvm-objdump prints x87 de e9 as fsubrp, which is the GAS-swapped name; memory-operand forms are not swapped | build EEBD15F7 (1.0) | src vr-motion-controls/docs/decompile/weapon-object-field-map.md:166 | tag [inferred] trap
- LS3DF loads at a base other than the preferred 0x10000000 (seen 0x00D30000): use offsets from the module base, not preferred VAs | build ? | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:196 | tag live trap
- GOG and custom Game.exe have different offsets for the same field (FFB matrix 0xE4 on GOG vs 0x164 custom); the GOG trainer chain (CWorld 0x647E1C, PP_ offsets) was REFUTED on 1.0 | build GOG 1.2 / EEBD15F7 | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:13 | tag live trap
- Code byte patches: FPS limiter at 0x5F95BC (push imm8 16, refused unless bytes match 6A 10) | build EEBD15F7 (1.0) | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:197 | tag technique
- Code byte patch 0x4A5BD9 (cmpl $8 immediate) changed to 0x7F to disable camera branch; restore function has no caller, so the patch lasts until exit | build EEBD15F7 (1.0) | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:198 | tag live trap
- Mod-side: an injected DLL must not hook with a 5-byte jump that overlaps another mod's stub (b5hook vs UpdateWMatrixProc prologue E9 .. 90 emptied a probe) | build ? | src vr-motion-controls/docs/08-state.md:29 | tag live trap
- Trace and frame access by listing: several hypotheses (body vs head, +0x6E0 null) need a runtime read; one log line on entry settles each | build ? | src vr-motion-controls/docs/decompile/muzzle-position.md:133 | tag technique
- Read back the frame's world position only after a dirty-flag test; reading a dirty frame gives last frame's position (the weapon frame bug of 2026-08-12 showed muzzle displaced) | build EEBD15F7 (1.0) | src vr-motion-controls/docs/aim-map/SUMMARY.md:41 | tag technique
- Count and log muzzle-vs-hand residual every shot; a 280 m error hid behind a plausible-looking aim point | build ? | src vr-motion-controls/docs/aim-layer/SPEC.md:23 | tag technique
- Hand aim should refuse (valid=0) when the muzzle frame is not valid rather than shoot from an unknown origin | build ? | src vr-motion-controls/docs/aim-layer/SPEC.md:110 | tag design
- Keep the ammo/reload state in the engine: the mod sends animation events and reads the holder; it never edits slot clip/reserve directly | build ? | src vr-motion-controls/docs/09-topics/004-colt-reload/protocol.md:22 | tag design
- Use fail-closed reload transactions with a sum check on rounds (MP2VR pattern) | build ? | src vr-motion-controls/docs/09-topics/004-colt-reload/protocol.md:22 | tag design
- Set CPlayer+0x1ED = 1 to bypass the arc veto; read the game's own value first and restore it after; the game writes it with +0x1EC in lockstep | build EEBD15F7 (1.0) | src vr-motion-controls/docs/for-mafia-re/fields-and-globals.md:60 | tag technique (mod already does this)
- Count refused shots and print the count next to the zero (arc veto is silent otherwise) | build ? | src vr-motion-controls/docs/aim-layer/SPEC.md:83 | tag technique
- Decision function pure: VrAim_Decide computes and never writes; the owner of each actuator writes; valid=0 is an answer meaning nobody writes | build ? | src vr-motion-controls/docs/aim-layer/SPEC.md:73 | tag technique (design)
- Read the raised state from the same bytes the fire code branches on (+0x1E5 and stage at +0x1F0/+0x1F4) | build EEBD15F7 (1.0) | src vr-motion-controls/docs/aim-layer/SPEC.md:33 | tag technique

### vr-mod

- Stereo injector duplicates draw calls into two eye viewports and swaps per-eye VIEW/PROJECTION. Pre-transformed draws (D3D8 XYZRHW) must be excluded from that swap. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:7-15 | tag [decomp]
- Never cache D3D state across draws: state blocks (ApplyStateBlock), extra devices, own code and hooks installed late all bypass a cached record. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:37-46 | tag [inferred]
- Hook points used by mod: HookD3D8, Direct3DCreate8, CreateDevice, "device vtable hooked" (the sync point for the keyboard recorder). | build ? | src vr-mod/docs/03-tools.md:126 | tag [decomp]
- Render-side read points used by theater mode: CopyRects, GetBackBuffer, LockRect, CreateImageSurface. | build ? | src vr-mod/docs/03-tools.md:126 | tag [decomp]
- Impostor reprojection gate is impostor_mode; when off it prints "reproj: OFF (impostor_mode=%d)". | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:101-102 | tag [decomp]
- Per-draw refusal must leave the whole draw as issued; partial transform of a triangle stretches polygons across the frame. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:119-125 | tag live
- Validate with a cheap read-only pass (four floats per vertex) before the expensive vertex-buffer copy; the first version locked and copied everything and ran at 10 fps. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:109-117 | tag live
- Four draw entry points, seven vertex formats; two of four pass vertices as arguments. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:174-176 | tag live
- Draw counters per path: drawtally, sitetally, hooktally, blendtally, pooltally, rhwtally, rhwtex, reproj, remapbuf, fvfcheck. | build ? | src vr-mod/docs/03-tools.md:126 | tag [decomp]
- Classify by data, not by label: dump the first vertices as bytes; a plausible count with bad coordinates looks like a working fix. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:78-107 | tag live
- Cross-check two sources describing one draw (FVF size vs stride, index format vs count, RT size vs viewport). | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:55-76 | tag live
- Refusal counters: one per failure branch, both sides of every split, print zero out loud. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:92-107 | tag live
- Every draw or none: a per-vertex refusal must abandon the whole primitive. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:119-125 | tag live
- Write the test as the user's sentence (pitch must move the tree; one-eye object disparity ~100x a 200 m one). | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:127-137 | tag live
- Mutation testing: one mutant survived because all fixtures had identity view; a non-identity case killed it. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:139-146 | tag live
- Keep an explicit table of which transform carries which axis and where it is applied. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:148-157 | tag [inferred]
- Every fix reveals the next defect; read it as the instrument improving, not as a regression. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:159-166 | tag live
- Checklist for a new engine: enumerate vertex paths, cross-check per path, decide axis split, build counters before feature, build the leave-alone path first. | build ? | src vr-mod/docs/STEREO-RENDER-PLAYBOOK.en.md:172-183 | tag [inferred]
- Static to runtime: a listing names what CAN happen; only a run says what DID. | build EEBD15F7 | src vr-mod/docs/camera-basis-static-candidates.md:5-9 | tag [inferred]
- Offline replay of game controller bytes: pass-through 1999 of 1999 cases, +30 deg where Frame_SetDirection reads it, 252 of 252 at 24 and 53 bit x87; broken stub caught 51 of 51. | build EEBD15F7 | src vr-mod/docs/09-topics/005-camera-onfoot-point/findings-2026-10-07.md:16-18 | tag live
- Hook installer refuses if the 7 target bytes differ and names the foreign module when the jump enters it; bytes read back after patch. | build EEBD15F7 | src vr-mod/docs/09-topics/005-camera-onfoot-point/findings-2026-10-07.md:26-29 | tag [decomp]
- Mod exports its patched code; mafia_re.asi refused when it already replaced the same function (both would race). | build EEBD15F7 | src vr-mod/docs/09-topics/005-camera-onfoot-point/findings-2026-10-07.md:26-28 | tag [decomp]
- Categorized logger: LOG() does not evaluate arguments of an off category; work only for the log goes under LOG_ON. Old logmsg evaluates always and truncates at 512 bytes silently. | build ? | src vr-mod/docs/03-tools.md:57-61 | tag [decomp]
- A logged category is verified by catlog-moved-check (prefix of format output vs old logmsg); a blind format with no first word needs --blind. | build ? | src vr-mod/docs/03-tools.md:79-80 | tag [decomp]
- Two-file log reader vrlogs merges vr_diag.txt with the new categorized log by clock line. | build ? | src vr-mod/docs/03-tools.md:94-97 | tag [decomp]
- Recording ring vr_diag.bin of 76-byte records, ~94 MB per run; off by default (rec = off). | build ? | src vr-mod/docs/03-tools.md:67-70 | tag live
- Judge a point by its effect: direction the engine read, not the value written; the rule for a camera point is the bench M1 (camera vs head) at 0.000 with body at 1.000. | build EEBD15F7 | src vr-mod/docs/09-topics/005-camera-onfoot-point/findings-2026-10-07.md:54-58 | tag live
- Replacing a car-presence seat gate with the game's own actionState (9 = seated) is a candidate; keep statistical gate until then. | build ? | src vr-mod/docs/09-topics/005-camera-onfoot-point/findings-2026-10-07.md:78-82 | tag [inferred]
- Injection stands down via vrm_injection_stands_down(body_turn, body_mode, seat); writers never run together (two correct fixes cancel when they rotate the same angle). | build ? | src vr-mod/docs/09-topics/001-c1-car-culling-behind/leads-2026-10-04.md:37-41 | tag [decomp]
- Render-sync point for keyboard recorder: "device vtable hooked" line marks start of key recording. | build ? | src vr-mod/docs/03-tools.md:126 | tag [decomp]
- Engine camera basis rebuild: hooking one UpdateWMatrixProc return only covers one of 62 consumers; write the source fields instead of the derived matrix (candidate). | build EEBD15F7 | src vr-mod/docs/camera-basis-static-candidates.md:92-96 | tag [inferred]

### vr-bench (headless testing)

- Game runs in the background via an in-process sandbox: virtual keyboard and mouse, never foreground, fullscreen turned windowed, dialogs and crashes absorbed | build ? | src Mafia/CLAUDE.md:? (vr-bench row) | tag live
- Game window parked off every screen: PrintWindow returns white; pictures come from the D3D9 back buffer at Present, independent of window position | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:27-29,547-548 | tag live
- User32 input calls (SendInput, keybd_event, mouse_event, SetCursorPos) are answered by the sandbox: offered / served / passed counters; passed is always 0 | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:424-429 | tag live
- Synthetic key: a tap down+up in one call is not seen; hold the key across frames (200 ms hold works, tap does not) | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:431-436 | tag live
- OS-level key synthesis into a DirectInput game: send KEYEVENTF_SCANCODE with a zeroed virtual key; a bare VK is ignored on other layouts | build ? | src docs/VR-CONVERSION-PLAYBOOK.md:158-161 | tag live
- Frame probes are read inside the game's Present at every frame into frame-probes.csv (frame, OpenXR frame xf, ms, read cost, value) | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:416-418 | tag live
- Per-OpenXR-frame CSV (xr-frames.csv) joins mod log and bench by frame number f; positions in metres, quaternions x y z w; FOV in RADIANS (-0.855 = -49 deg) | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:395,410-414 | tag live
- Window video of the game: 10 fps at 480 wide costs 0.41 ms per game frame; 59.0 fps against 59.06 without it | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:397 | tag live
- Eye video at 10 fps, 960 per eye: +2.85 ms on a frame that takes a picture, 0.50 ms per game frame on average, game at 56.0 fps, 0 frames dropped | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:398 | tag live
- Game races its own start: bench.ps1 -StartGraceMs holds the first window (1..5000 ms) for builds that race; 6b80226 lost 7 of 7 starts on 2026-10-05 | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:444-446 | tag live
- Lost start (runtime never loaded) is re-run up to 2 times and listed; a build that loses some starts and not others has a start race | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:67-72 | tag live
- Shared queue for one game at a time: tickets in vr-bench\queue, first come first served, waits up to 1 h then NO DATA | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:450-455 | tag live
- Live escape check: run stops if the game owns the foreground or a game window covers more than 16 px of a monitor other than the bench's | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:242-244 | tag live
- Free-disk gate: a run needs twice the largest of the last 30 run folders, at least 2 GB; refused before the game starts | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:49-56 | tag live
- Eye-video and the E1 method: phase correlation reads a 2x picture as about 0.76 at 0.5 s bins; per 0.1 s pairs with a search through the eye FOV read 2.07 (1-3 deg), 1.72 (3-6), 1.71 (6-10), 1.81 (10-20) | build ? | src Mafia/vr-bench/docs/findings/2026-10-05-e1-the-recording-method-underreads-a-2x-picture.md:24-30 | tag live
- Black wedges (about 7% of each eye picture at the edges) spike the correlation at 0 deg and read fast turns as no turn; mask the wedge (pixels under 8) | build ? | src Mafia/vr-bench/docs/findings/2026-10-05-e1m-black-wedge/README.md:6-19 | tag live
- A check that plants known turns into the run's own picture cannot see screen-fixed artefacts (the wedge moves with the plant) | build ? | src Mafia/vr-bench/docs/findings/2026-10-05-e1m-black-wedge/README.md:21-24 | tag live
- Holds on the mod build read cleanly (~2.0 on 873D98C3 in every state); the mask's effect on holds is not settled | build ? | src Mafia/vr-bench/docs/findings/2026-10-05-e1m-black-wedge/README.md:36-39 | tag live
- Session run (several cards in one game start) vs alone: 16 cards in 7 game starts gave the same word and lists as 32 alone runs | build ? | src Mafia/vr-bench/docs/findings/2026-10-12-whole-suite-alone-vs-session.md:21-25 | tag live
- 4x4 series 2026-10-06 (4 alone + 4 session): h1 0 of 35 outside, a1 0 of 75, t1 0 of 38 (splits 35, 35, 20) | build ? | src Mafia/vr-bench/docs/findings/2026-10-12-whole-suite-alone-vs-session.md:31-33 | tag live
- Two alone runs and one session run: only 3 splits, p never below 1/3; cannot reject at alpha 0.05 | build ? | src Mafia/vr-bench/docs/findings/2026-10-12-whole-suite-alone-vs-session.md:29-31 | tag live
- Head-tracking lag: the submitted view carries the previous frame's pose; one frame late = 0.42 deg at p50, 2.45 deg at p95, 6.95 deg at most (on Alex's own head) | build ? (mod 873D98C3) | src Mafia/vr-bench/USING-THE-BENCH.md:85 | tag live
- Replay of a real head: a recorded head's telemetry is turned into a trajectory with head_from_telemetry.py (--scan, --from-s/--to-s) | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:85,354-355 | tag live
- Virtual Desktop optics and rate: xr fov eye l -49 45 48 -50; xr fov eye r -45 49 48 -50; xr refresh 90 | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:361 | tag live
- Trajectories: holds (gen_holds.py), whole turns (gen_turns.py), six-axis motion (gen_motion.py); rows advance one per game commit, so write at the game's rate | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:330-335,361-362 | tag live
- Controller inputs of the OpenXR 1.0 Touch profile are driven and journaled; a sensor reads touched while its control is engaged | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:337-345 | tag live
- Mod-side pictures: the bench's markers drawn in eye captures show where the runtime says each hand is; pale yellow mod gizmo centroid 19.3-19.5 deg below the hand | build ? (5137760D) | src Mafia/vr-bench/docs/findings/2026-10-04-mc1-5137760D-bullets-leave-the-aim-point.md:94-97 | tag live
- A pitch error that swings with a 16-19 pull (about 20 s) period while nothing fed changes: lag-1 +0.72 vs shuffled p95 +0.28; identical pulls spread 56.9 deg at most | build ? (5137760D) | src Mafia/vr-bench/docs/findings/2026-10-04-mc1-5137760D-bullets-leave-the-aim-point.md:140-153 | tag live
- Shuffled-null test for time-correlated repeats: lag-1 autocorrelation of the errors vs the same values shuffled (p95) before trusting a spread | build ? | src Mafia/vr-bench/docs/findings/2026-10-04-mc1-5137760D-bullets-leave-the-aim-point.md:132-135 | tag live
- Hook DLL in the game's process must detach its std::thread objects in DllMain when lpReserved != NULL; otherwise the process exits with 0xC0000409 | build ? | src docs/A-JOINABLE-STD-THREAD-KILLS-THE-PROCESS-AT-EXIT.md:36-40 | tag live
- Test for the exit fault: child process loads the DLL, returns from main without shutdown; parent reads exit code 0 (fixed) or 0xC0000409 (broken) | build ? | src docs/A-JOINABLE-STD-THREAD-KILLS-THE-PROCESS-AT-EXIT.md:45-50 | tag live
- Start-race defect of mod build 5137760D: saved original Direct3DCreate8 nulled by PatchIATInModule re-scan at ~100 ms; game dies 390 ms after start (exception c0000005 at 00000000) | build ? (5137760D) | src Mafia/vr-bench/docs/findings/2026-10-03-mc-build-5137760D-loses-its-d3d8-original.md:8-27 | tag disasm
- Start race under the old sandbox: the game's Direct3DCreate8 landed after the re-scan; after 2026-10-04 it starts at 172 ms logged (about 62 ms started) | build ? (5137760D) | src Mafia/vr-bench/docs/findings/2026-10-03-mc-build-5137760D-loses-its-d3d8-original.md:63-77 | tag live
- Read-only probe of a write-discard mapping is uncached: a fade in place in a D3D11 WRITE_DISCARD map turned 16-33 ms frames into 1135 ms; fixed by copying from ordinary memory (33.2 ms max, 0 over 250 ms) | build ? | src docs/READING-A-WRITE-DISCARD-MAPPING-IS-UNCACHED.md:5-18 | tag live
- Differential test of compiled decompiler output vs the original code, with NaN payloads, and a positive control that must fail (1 case of 4000 without the gate; 48 of 197 with it) | build ? | src docs/HEX-RAYS-PSEUDOCODE-COMPILED-AS-C-IS-ANOTHER-PROGRAM.md:35-42 | tag live
- A probe that returns a plausible constant is indistinguishable from a quiet world until something moves; every place-probe needs one run where it moves | build ? | src Mafia/vr-bench/docs/findings/2026-10-12-carx-carz-never-moved.md:23 | tag live
- Car-slot scan (carscan.py): ReadProcessMemory of the car object's first 1024 bytes every 0.25 s, then each pointer slot's floats at +0x40..+0x48 | build ? | src Mafia/vr-bench/docs/findings/2026-10-12-carx-carz-never-moved.md:12-15 | tag live
- Game clock values: gtime, shotms (game clock at last shot), orgms; a shot = a change in shotms | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:313-322 | tag live

### vr-bench (practices)

- A warm-up call does not remove a one-time cost; it moves it to whoever calls first | build ? | src docs/A-ONE-TIME-COST-MOVES-TO-WHOEVER-CALLS-FIRST.md:30-31 | tag live
- Hook creation exports at load (code patching only, safe in DllMain) and finish at the program's first call to keep its timeline (window 172 -> 31 ms) | build ? | src docs/A-ONE-TIME-COST-MOVES-TO-WHOEVER-CALLS-FIRST.md:33-36 | tag live
- Log the START of a call when comparing timelines; a time at a return carries one-time costs | build ? | src docs/A-ONE-TIME-COST-MOVES-TO-WHOEVER-CALLS-FIRST.md:26-27,37-38 | tag live
- Compare the same build only; a number from another build is a hint (stand logs 141-407 ms for one line) | build ? | src docs/A-ONE-TIME-COST-MOVES-TO-WHOEVER-CALLS-FIRST.md:39-41 | tag live
- Link with -Wl,--no-insert-timestamp (MSVC /Brepro) and build twice to check: the linker timestamp changes the md5 of an identical source | build ? | src docs/A-REBUILD-OF-THE-SAME-SOURCE-IS-A-NEW-MD5.md:6-8,25-27 | tag live
- The output FILE NAME is in the md5 too (DLL export directory records -o name): 91470DBB vs A79E447D for the same source | build ? | src docs/A-REBUILD-OF-THE-SAME-SOURCE-IS-A-NEW-MD5.md:34-39 | tag live
- Hex-Rays decompiled C: _DWORD is unsigned in defs.h; 6 of 24 integer operators in the camera controller had the wrong sign | build ? | src docs/HEX-RAYS-PSEUDOCODE-COMPILED-AS-C-IS-ANOTHER-PROGRAM.md:8-18 | tag decomp
- Hex-Rays dropped fcom: 3 comparisons read a flag nothing assigns; 3 folded negations (fchs) differ for NaN | build ? | src docs/HEX-RAYS-PSEUDOCODE-COMPILED-AS-C-IS-ANOTHER-PROGRAM.md:8-12,22-31 | tag decomp
- LLVM folds -a + b to b - a even at -O0; negation inside an expression must go through a volatile temporary | build ? | src docs/HEX-RAYS-PSEUDOCODE-COMPILED-AS-C-IS-ANOTHER-PROGRAM.md:13,31 | tag decomp
- x87 fadd / fmul of two quiet NaNs with equal significands returns the positive one, whatever the operand order (measured) | build ? | src docs/HEX-RAYS-PSEUDOCODE-COMPILED-AS-C-IS-ANOTHER-PROGRAM.md:44-45 | tag live
- Joinable std::thread in an injected DLL terminates the host at exit (0xC0000409); never join under the loader lock | build ? | src docs/A-JOINABLE-STD-THREAD-KILLS-THE-PROCESS-AT-EXIT.md:3-5,38-42 | tag live
- Never read from a WRITE_DISCARD / upload mapping; keep the old value in your own memory | build ? | src docs/READING-A-WRITE-DISCARD-MAPPING-IS-UNCACHED.md:15-19 | tag live
- E1-type picture-turn read: the recording method (phase correlation) underreads fast turns; use per-pair search, not 0.5 s slope | build ? | src Mafia/vr-bench/docs/findings/2026-10-05-e1-the-recording-method-underreads-a-2x-picture.md:31-36 | tag live
- Plant a turn into the run's own picture and the check still passes if a screen-fixed artefact moves with it - the check must use a screen-fixed mask | build ? | src Mafia/vr-bench/docs/findings/2026-10-05-e1m-black-wedge/README.md:21-24 | tag live
- Judge a build by the card's word only when all analyzers are VALID; FAIL is only an analyzer's own exit 1; a crashed analyzer or refused run is NO DATA | build ? | src Mafia/vr-bench/USING-THE-BENCH.md:46-48 | tag live
- Keep the state of a repeat-trial null honest: draw the error against trial number before using a spread | build ? | src Mafia/vr-bench/docs/findings/2026-10-04-mc1-5137760D-bullets-leave-the-aim-point.md:132-134 | tag live
- Identify a build by md5 only after two builds of identical source give the same md5 | build ? | src docs/A-REBUILD-OF-THE-SAME-SOURCE-IS-A-NEW-MD5.md:23-30 | tag live
- Measure, do not assume, synthetic input: emit a known signal and measure the response before building on it (camera correlation -0.98 over 1660 frames for the synthetic mouse) | build ? | src docs/VR-CONVERSION-PLAYBOOK.md:153-157 | tag live
- Drive the ENGINE's own FOV from the headset; a narrower engine FOV than the display culls geometry at the periphery; widening to 145 deg costs about 28% more draws, no loss of draw distance | build ? | src docs/VR-CONVERSION-PLAYBOOK.md:111-120 | tag live
- Pace the game to the compositor: unpaced 140-150 fps into 90 Hz judders; ~72 fps looked perfect (FEAR2VR, another engine; cited for pacing) | build ? | src docs/VR-CONVERSION-PLAYBOOK.md:105-109 | tag inferred
- One writer per rotation: two correct fixes that both write the heading cancel exactly (644 of 677 samples identity) | build ? | src docs/VR-CONVERSION-PLAYBOOK.md:36-47 | tag live
- Publish the pose the frame was rendered from; the readback can be pipelined one frame late (issue N, lock N-1) at about one frame of latency | build ? | src docs/VR-CONVERSION-PLAYBOOK.md:21-25,28-33 | tag live
- Try XR_RUNTIME_JSON pointing at a 32-bit runtime before splitting into a 64-bit host process | build ? | src docs/VR-CONVERSION-PLAYBOOK.md:173-182 | tag inferred
- Refuse a run whose world did not move: a constant probe needs one run in which the thing moves; the car slot probe was blind for 13 m of driving | build ? | src Mafia/vr-bench/docs/findings/2026-10-12-carx-carz-never-moved.md:8-11,20-21 | tag live
- Per-pair picture-turn bands: a 2x picture is read 2.07/1.72/1.71/1.81 per band by the per-pair search but 0.29 or 0.00 by phase correlation above 10 deg per 0.1 s | build ? | src Mafia/vr-bench/docs/findings/2026-10-05-e1-the-recording-method-underreads-a-2x-picture.md:24-30 | tag live

## Conflicts

- none found among these lines; the technique lines do not state contradictory values for the same thing.
