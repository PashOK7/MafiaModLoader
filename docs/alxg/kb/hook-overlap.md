# Hook overlap - Game.exe 1.0 EEBD15F7 - our mods vs MafiaModLoader

Date: 2026-10-19. Read-only survey. Nothing built, run or fetched.

Path base for citations: `E:/!Claude/Mafia/` (so `re/06-functions.md:6126` = `E:/!Claude/Mafia/re/06-functions.md` line 6126).

Sources:
- `re/06-functions.md` - our "We change" column (vr-mod, mafia-re). It is THIN: it lacks the vr-motion-controls sites (e.g. 0x004A4B70 shows "-").
- `vr-motion-controls/docs/for-mafia-re/hooks-and-patches.md` - full site table for vr-motion-controls (generated 2026-10-06 from git HEAD 7402e84). Used as the source for those sites.
- `re/docs/03-tools.md:174-198` - mafia_re.asi camera=1 replacement and its prologue check.
- `mod-loader/docs/09-topics/001-upstream/findings-B-api.md` - loader events (1.0 column), address table, missing list (line 10).
- `vr-motion-controls/docs/decompile/mouse-to-yaw.md:33,69` - LS3DF 0x6E2A0 = IGraph::PollMouse, calls DI GetDeviceState at +0xCC.

Caveats (read before using):
1. Loader 1.0 addresses are code twins from `upstream/sdk/include/mml/game/game_build.h`; no 1.0 game was ever run with them (findings-B-api.md:4-5). Every row below is "unverified at runtime".
2. LS3DF rows are matched by RVA only. Our LS3DF build is LS3DF-noversion-BE2F678C (06-functions.md:12). Whether the loader's kLs3d10 targets that same build is NOT checked (no build id in findings-B). Name checks (06:1195 Graph_PollKeyboard, 06:1209 Graph_UpdateMouse, 06:1218 Graph_LimitFrameTime, 06:1048 Render_RenderScene at RVA 0x5ED80) agree with the loader's labels.
3. vr-mod hook sites are taken from the 06 "We change" rows only (vr-mod's own source was not opened). vr-mod's D3D/DirectInput hooks are not catalogued anywhere in 06.
4. gog-patcher: no row in 06 and no hook table found. It patches the GOG build by bytes, not this 1.0 build. Not examined further.
5. Calls, not hooks, are excluded: Human_UpdateAttackAction 0x004A3AE0 (06:3291), Physics_RayCastSectors 0x00582600 (06:4961), Human_SelectWeaponById 0x0055A1F0, Weapon_FireBulletsAndImpact 0x005E3DF0 (tail-jump target of mc row 3).

## 1. Overlap table

Overlap key: SAME-SITE = same bytes overwritten. SAME-FUNCTION = same function, different site. NONE = no shared site. "loader use" is what the loader does at that address in 1.0.

| # | address (1.0) | our project(s) and what they change | loader event/use (1.0) | overlap | source |
|---|---|---|---|---|---|
| H01 | 0x005FFD40 Unknown_UpdateWorld | none | MISSION_TICK | NONE | re/06-functions.md:6126; findings-B-api.md:17 |
| H02 | 0x005FFB40 Scene_UnloadMission | none | MISSION_CLOSE | NONE | re/06-functions.md:6125; findings-B-api.md:18 |
| H03 | 0x005FFF30 Scene_LoadMission | none | MISSION_OPEN | NONE | re/06-functions.md:6128; findings-B-api.md:19 |
| H04 | 0x00496710 Human_ReceiveHit | none (planned call only, not in code) | HUMAN_HIT | NONE | re/06-functions.md:3244; vr-motion-controls/docs/09-topics/010-throwing/spec.md:30; findings-B-api.md:24 |
| H05 | 0x0044B740 Actor_DestructBase | none | ACTOR_DESTROYED | NONE | re/06-functions.md:2657; findings-B-api.md:22 |
| H06 | 0x0048F430 Human_ConstructActor_48F430 | none | HUMAN_CREATED | NONE | re/06-functions.md:3210; findings-B-api.md:23 |
| H07 | 0x005F86C0 App_RenderOverlayCallback | none in 06 | draw2d stage 2 (2D overlay) | NONE | re/06-functions.md:6047; findings-B-api.md:59, 78 |
| H08 | 0x0067A4D8 menuTop (data, not code) | not catalogued | GAME_PAUSED / RESUMED (unverified) | NONE | findings-B-api.md:21, 59, 87 |
| H09 | 0x005F9540 App_MainLoop | vr-mod: FPS limiter (FpsLimitByte, ApplyFpsLimit, cfg fps_limit_ms) | none | NONE | re/06-functions.md:45, 6049 |
| H10 | 0x005F95BD (App_MainLoop+0x7D, operand of push 16) | vr-motion-controls row 5: rewrites the same frame-limiter operand; F3 hotkey not cfg-gated | none | NONE (loader); SAME-SITE vr-mod vs mc, see S1 | vr-motion-controls/docs/for-mafia-re/hooks-and-patches.md:25 |
| H11 | 0x004A4B70 Weapon_Human_UpdateWeaponFire | vr-motion-controls row 1: inline detour, weaponhook | none (their "weapon state" is in the not-offered list) | NONE | hooks-and-patches.md:21; findings-B-api.md:10 |
| H12 | 0x004A555C (+0x9EC) | vr-motion-controls row 2: aim stub, aimstub | none | NONE | hooks-and-patches.md:22 |
| H13 | 0x004A5CEA (+0x117A) | vr-motion-controls row 3: call-site retarget to H_ShotSpawn, shothook | none | NONE | hooks-and-patches.md:23 |
| H14 | 0x004A5BD9 (+0x1069) | vr-motion-controls row 4: byte patch 0x08 to 0x7F, muzzle_from_gun | none | NONE | hooks-and-patches.md:24 |
| H15 | 0x004CB1B0 Player_Update | vr-motion-controls row 36: inline detour, tickhook | none (06 shows "-") | NONE | hooks-and-patches.md:56; re/06-functions.md:3519 |
| H16 | 0x005D1BC0 Input_ReadAxis | vr-mod: axis probe, cfg axisprobe=1 | none | NONE | re/06-functions.md:40, 5686 |
| H17 | 0x005D4DD0 Camera_UpdateController | vr-mod: H_CamUpd, cfg campath=1 (inline). mafia-re: whole-function replacement when camera=1 | none; loader missing list has "camera update" | NONE (loader); SAME-SITE vr-mod vs mafia-re, see S2 | re/06-functions.md:41-42, 5749; re/docs/03-tools.md:197-198; findings-B-api.md:10 |
| H18 | 0x005D9710 Camera_UpdateChaseCollision | mafia-re: replacement when camera=1 | none | NONE | re/06-functions.md:43, 5752 |
| H19 | 0x005D9B90 Camera_UpdateFollowView | mafia-re: replacement when camera=1 | none | NONE | re/06-functions.md:44, 5753 |
| H20 | 0x005427F0 Unknown_LoadFromFile | vr-mod: tools/set-draw-distance.ps1 patches Game.exe ON DISK (offline, not a runtime hook) | none | NONE | re/06-functions.md:39, 4154 |
| H21 | cfg-chosen VA in 0x401000-0x700000 (debug registers) | vr-motion-controls rows 10-12: hardware watchpoints, execwatch / camwatch / invwatch, default off. Address unknown here | none | NONE (cannot map statically) | hooks-and-patches.md:30-32 |
| H22 | LS3DF 0x6E580 (RVA) Graph_LimitFrameTime | none | FRAME event (frame limiter) | NONE | re/06-functions.md:1218; findings-B-api.md:16 |
| H23 | LS3DF 0x6DD40 (RVA) Graph_PollKeyboard | none | keyboard read hook | NONE | re/06-functions.md:1195; findings-B-api.md:63, 74 |
| H24 | LS3DF 0x6E2A0 (RVA) Graph_UpdateMouse (= IGraph::PollMouse) | none directly; calls our row H33 (DI GetDeviceState) one level down | mouse read hook | NONE (direct); CHAIN via H33, see section 2 | re/06-functions.md:1209; vr-motion-controls/docs/decompile/mouse-to-yaw.md:33, 69; findings-B-api.md:74 |
| H25 | LS3DF 0x1C570 (RVA) ?UpdateWMatrixProc | vr-mod: cam_postwm / cam_prehook / B5 hook (installed at 13520, 13532). vr-motion-controls row 6: inline detour, same cfg keys | none; loader missing list has "camera update" | NONE (loader); SAME-SITE vr-mod vs mc, see S3 | re/06-functions.md:46, 280; hooks-and-patches.md:26 |
| H26 | LS3DF 0x1C800 (RVA) Frame_Update | vr-mod: frame_exit=1 hook | none | NONE | re/06-functions.md:47, 282 |
| H27 | LS3DF 0x2C5F0 (RVA) ?Inverse@S_matrix | vr-mod: cam_inverse / cam_postwm. vr-motion-controls row 7: inline detour, same keys | none | NONE (loader); SAME-SITE vr-mod vs mc, see S4 | re/06-functions.md:48, 439; hooks-and-patches.md:27 |
| H28 | LS3DF 0x2DF50 (RVA) ?RotateByMatrix@S_vector | vr-mod: H_RotByMat, camhook=1 (deployed). vr-motion-controls row 9: cullprobe, only with camhook=1 | none | NONE (loader); SAME-SITE vr-mod vs mc, see S5 | re/06-functions.md:49-50, 458; hooks-and-patches.md:29 |
| H29 | LS3DF 0x313C0 (RVA) Cull_TestMirror | vr-mod: H_VisFn, vis288=1 | none | NONE | re/06-functions.md:50, 514 |
| H30 | LS3DF 0x5ED80 (RVA) Render_RenderScene | vr-mod: H_RenderScene, camhook=1 (deployed; writes FOV, seat offset, head yaw). vr-motion-controls row 8: same entry, camhook default 0 | address table only ("render"); NO event. Missing list: render begin/end, second pass | NONE (loader); SAME-SITE vr-mod vs mc, see S6 | re/06-functions.md:51, 1048; hooks-and-patches.md:28; findings-B-api.md:10, 59 |
| H31 | 0x10079600 Direct3DCreate8 thunk; d3d8.dll export (runtime) | vr-motion-controls rows 13-14: IAT patch and inline detour, cfg enabled=1 | none; D3D device not offered | NONE | re/06-functions.md:1410; hooks-and-patches.md:33-34; findings-B-api.md:10 |
| H32 | IDirect3DDevice8 vtable slots 14, 15 (Present), 31, 34, 35, 36, 37, 40, 70-73, 76 | vr-motion-controls rows 15-28: H_Present (slot 15) is the per-frame driver of drains, probes and VR submit; others count or redirect draws | Present not offered (internal for screenshots); device not offered | NONE | hooks-and-patches.md:35-48; findings-B-api.md:10 |
| H33 | DirectInput: DirectInput8Create thunk 0x1007BF94; IDirectInput slots 3, 9; device slots 9 (GetDeviceState), 10 (GetDeviceData) | vr-motion-controls rows 29-35: unconditional at startup (cfg enabled only). GetDeviceState adds look delta and button bits | keyboard/mouse read is through LS3DF H23/H24 | NONE (direct); CHAIN via H24, see section 2 | re/06-functions.md:1412; hooks-and-patches.md:49-55 |
| H34 | 0x0065115C (data: game global pointer) | vr-motion-controls: read-only chain for CPlayer, table 3 | missionPtr 0x65115C (table) | NONE (shared read, no write by either side known) | hooks-and-patches.md:119; findings-B-api.md:46 |

Counts: 34 rows. Loader-vs-us SAME-SITE: 0. Loader-vs-us SAME-FUNCTION: 0.

Not in the table on purpose: the call-only sites in caveat 5, and the loader's 1.2-only addresses (not part of build 1.0).

## 2. SAME-SITE and SAME-FUNCTION rows (and chains)

All SAME-SITE rows are between OUR projects. The loader hooks none of these sites today.

| id | overlap | sites | risk |
|---|---|---|---|
| S1 | SAME-SITE (inferred from identical wording; the bytes were not compared) | H09 vr-mod FpsLimitByte vs H10 mc row 5, App_MainLoop operand 0x5F95BD | Two writers of one operand. Last writer wins at start. mc's F3 hotkey writes live with no cfg gate, so it can race vr-mod's ApplyFpsLimit. Order dependence. Verify whether both write the same byte before any merge. |
| S2 | SAME-SITE | H17 0x5D4DD0: vr-mod H_CamUpd (campath=1) vs mafia-re replacement (camera=1) | mafia-re checks the first bytes against the EEBD export and refuses on mismatch (re/docs/03-tools.md:197-198). If vr-mod's 6-byte patch is in place first, mafia-re is refused. If mafia-re wins, vr-mod's hook is silently skipped. Not verified in vr-mod code. |
| S3 | SAME-SITE | H25 0x1C570: vr-mod B5/cam_postwm hook vs mc row 6 (both InstallDetour, prologue 83 EC 40 55 8B E9) | Second E9 patch reads the first patch as "original bytes". Either refuses, or the trampoline jumps back into a patch. Only when both are enabled in one process (same cfg key names in both trees). |
| S4 | SAME-SITE | H27 0x2C5F0 Inverse: vr-mod cam_inverse vs mc row 7 (prologue 8B 4C 24 08 D9 41 14) | Same as S3. Also a write-side hook (vr-mod rotates the source basis before the call), so order against mc's read-only sampling changes the sampled value. |
| S5 | SAME-SITE | H28 0x2DF50 RotateByMatrix: vr-mod H_RotByMat vs mc row 9 cullprobe (both camhook=1 path) | Double detour on one prologue. mc's return-address test (LS3DF+0x31557) is valid only if the trampoline chain returns to the original caller. Check after any merge. |
| S6 | SAME-SITE | H30 0x5ED80 Render_RenderScene: vr-mod H_RenderScene vs mc row 8 (prologue 55 8B EC 83 EC 3C) | Double render-entry detour. Render_RenderScene is reentrant and used for a second pass (vr-motion-controls/docs/09-topics/015-scope/README.md:28), so the FOV and head-yaw writes could run twice per frame. |
| S7 | CHAIN (not same site): loader H24 -> our H33 | LS3DF Graph_UpdateMouse 0x6E2A0 calls DI GetDeviceState (mouse-to-yaw.md:69). Our slot-9 hook adds the look delta and button bits (hooks-and-patches.md:54) | If the loader hooks H24 and runs AFTER the original, its mouse event sees our injected delta. If it runs BEFORE, it does not. The loader doc says mods see state after the game's read (findings-B-api.md:74), so a mouse event would see our delta. Decide on purpose, and do not put two writers into one value. |

## 3. Our hooks that would become loader events

The loader's "not offered" list (findings-B-api.md:10) has five items we use or would use. Read-only candidates only; a write cannot move to an observer event.

| loader event (missing) | our hook candidate | what it does now | fit |
|---|---|---|---|
| camera update | H17 0x5D4DD0 (vr-mod H_CamUpd, campath) | Probe of the camera mode (named InstallCamPathProbe, re/06-functions.md:42) | Good fit if H_CamUpd only reads. Confirm in vr-mod source. Not the same as mafia-re, which replaces the body. |
| camera update (second) | H25 0x1C570 and H27 0x2C5F0 | cam_postwm writes the WORLD matrix after the original; Inverse rewrites the source basis before the call | No fit. These write. Keep as our detours. |
| render begin/end | H30 0x5ED80 H_RenderScene | Writes FOV, seat offset and head yaw BEFORE the original (hooks-and-patches.md:28) | Partial. An observer event at "begin" would have to run before our write, so the order must be fixed in the loader. "End" after the original fits a read-only probe. |
| second pass | H30 0x5ED80 (reentrant call, 015-scope README.md:28) | Our code calls o_RenderScene itself for the second pass | Possible if the loader passes the same scene argument. Otherwise no. |
| Present | H32 slot 15 H_Present | Drives drains, probes and VR submit (hooks-and-patches.md:37, 13263) | Partial. A post-Present event fits timers and read-only probes. VR submit must stay in our hook (ordering not verified here). |
| D3D device | H32 slot 15 CreateDevice hook (hooks-and-patches.md:15 row) and device vtable patches (rows 16-28) | Hook_CreateDevice captures the device and patches its vtable | A device-created event would replace only the CreateDevice hook. Vtable patches stay (they need per-slot hooks). |

Not in the five, but in the loader's not-offered sentence (findings-B-api.md:10): weapon, hands, camera state. Our H11-H15 (weapon fire, aim, shot spawn, byte patch, Player_Update) and H16 would be candidates for that category if the loader ever offered it.

## 4. Open items

- Verify the loader's 1.0 addresses against a 1.0 game before any event is trusted (findings-B-api.md:4-5).
- Confirm our LS3DF build (BE2F678C) is the one the loader's kLs3d10 targets.
- Open vr-mod's source for: H_CamUpd read/write status (H17); the exact operand written by FpsLimitByte (H09 vs H10); the prologue bytes it compares (S2, S3, S4).
- Catalogue vr-mod's D3D and DirectInput hooks (H31-H33 are documented only in the vr-motion-controls tree).
- 06's "We change" column omits vr-motion-controls sites (for example 0x004A4B70 shows "-"). Update 06 from hooks-and-patches.md before relying on it again.
