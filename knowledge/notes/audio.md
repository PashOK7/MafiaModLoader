# Native sound restart: deferred allocation rolls back effective gain and rate (2026-11-12)

Current allocation/shutdown/provider boundaries are summarized in [STATUS.md](../../docs/STATUS.md).
This note retains the November 12 diagnosis and earlier rejected work below. PIDs, hashes and acoustic
numbers identify those recordings; they do not identify the currently loaded game/provider. Ogg shutdown
and EAX-environment fidelity are separate investigations. No per-play gain workaround remains.

## Current causal finding and implementation

The owner correctly rejected the earlier gain-only workaround: equal aggregate RMS concealed a
large pitch/timbre change. character-on2.wav has nearly equal total levels (-16.17/-16.05 dBFS),
but its dominant line changes from 93.75 to 161.13 Hz after throttle reapplication. High bands
change by 6.8..7.6 dB. The prior conclusion was premature. The gain-transition hook and its helper
test were removed, not extended with another parameter transition.

A standalone 32-bit DirectSound reproduction uses only a generated 311 Hz sine, outside Mafia:

1. Duplicate a mono 3D buffer created with DSBCAPS_LOCDEFER. Play at 16000 samples/s, -15 dB.
2. While playing, change to 30869 samples/s and -4.11 dB: actual tone changes to 435 Hz, -25.22 dBFS.
3. Stop/Play with DSBPLAY_LOCSOFTWARE: output returns to the old 225.6 Hz, -36.12 dBFS, although
   GetFrequency/GetVolume still report 30869 and -411. Seeking before Play does not prevent it.
4. A volume transition repairs gain only; a frequency transition repairs the separate rate rollback.
5. With DSBCAPS_LOCSOFTWARE selected at creation instead, every restart/seek retains 435 Hz and
   -25.22 dBFS (within 0.016 dB) without any parameter transitions.

This isolates the failing allocation/restart path on this Windows backend. Microsoft's internal
cache implementation was not inspected; the observed rollback is to allocation-time values.
The [DirectSound capabilities contract](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416818(v=vs.85))
defines deferred allocation separately from software allocation. The fix selects the latter via
LS3D's own ISNDINIT_NOHWCHANNELS flag (0x20) at driver initialization when hardware 3D capacity is zero.
This Windows device reports max hardware 3D buffers 0 and generic mixing buffers 1; that generic
slot is not evidence of 3D capacity. Existing hardware/EAX policies and capability-query failures
are left alone. Spatial controls, sample assets, pitch, volume and native pause/seek behavior remain.
No per-Play hook, parameter nudges, process audio reset or Windows audio setting change remains.

Paired Init entries: 1.0 ls3d+77E30, 1.2 ls3d+7C1F0. Both are stdcall(driver, hwnd, frequency,
channels, bits, flags), ret 0x18 on both return paths. The driver fields +0x0C (IDirectSound8) and
+0x30 (creation flags), and source allocation branches, were read in both builds. Source Open:
1.0 ls3d+791C0, 1.2 ls3d+7D590. sound.compat is now read-only and reports capabilities/policy;
old restart_gain requests are rejected. Actual owner buffer caps changed from 0x400B0 to 0xB8:
no LOCDEFER, software allocation, with 3D/frequency/volume controls retained.

## Verification and installation recorded on November 12

### Relation to Setup's Software mixing only

The owner correctly identified the existing option. Setup stores S_settings::software_mixing_only
at byte +0x20. Native InitSystem converts it into the same ISNDINIT_NOHWCHANNELS bit 0x20:
1.2 Game.exe 0x5BFA7F..0x5BFA98 reads settings 0x647F04; 1.0 0x5FA7CF..0x5FA7E6 reads 0x6F954C.
Both paths were checked in disassembly. Thus on the owner's no-hardware/no-EAX configuration the
implemented buffer policy is effectively the existing Software mixing only mode, selected at runtime.
It is an automatic capability-based compatibility choice, not a repair of Microsoft's internals.

The complete initialization paths differ: Setup supplies the flag before Init and suppresses hardware
channel claiming/InitEAX; the host applies it after successful native Init only when 3D capacity is
zero and no EAX provider was detected. Setup/registry is not edited; genuine hardware/EAX paths remain.
Forcing the Setup option generally trades hardware mixing/EAX capabilities for CPU mixing. The old
owner recordings already show DSBSTATUS_LOCSOFTWARE (0x15 with playing/looping), so affected voices were
already software mixed: this change fixes allocation lifetime, not a newly imposed CPU mixing stage.
Ordinary spatial controls are retained (CTRL3D), as are LS3D's distance-volume calculations. Exact CPU
and latency deltas were not benchmarked; hardware/EAX wrapper configurations were not tested.

### Normal-game feature quality is a separate question

The owner requested audible evidence of feature benefits, not another pause-defect comparison.
The supplied old/new engine clips do not answer that. In LS3D 1.2 m_UpdateSoundProperties
(ls3d+4AAA0, checked in disassembly), sector environments are blended and submitted only when
ISNDINIT_EAX30 is set. The sector contract includes room levels, reflections, reverb, decay,
echo, frequency absorption and environment blending. This is potentially meaningful game acoustics;
software allocation alone is not a substitute for a functioning EAX provider.

However, the pre-change PID 27376 recorded-spatial.json driver snapshot already has flags 0x12
(NOEAX 0x10 set), eax_property_set +0x10 = null and ds_property +0x14 = null. Thus active EAX effects
were not removed by the new allocation policy on this configuration. Current PID 10752 reports
flags 0x32, retaining the existing NOEAX setting and adding only NOHWCHANNELS. This does not establish
that EAX would be useless or that basic software playback matches fully working EAX sound.

A genuine feature A/B requires the same game scene/actions with a verified EAX3 provider on/off,
including an authored indoor environment and an outdoor control. No such local A/B exists yet.
[DSOAL's own README](https://github.com/kcat/dsoal) describes software DirectSound3D/EAX support;
it is a candidate for isolated lab evaluation, not an installed or validated component of this pack.
The owner game and Windows audio settings were not changed during this feature-quality inquiry.

Owner subsequent Setup test: new Mafia10Fixes PID 4264, EAX allowed and Software mixing only enabled.
Read native settings bytes 0x6F9549..0x6F954D = 00 00 00 01 01: disable_eax is false and software
mixing true. sound.compat reports native_flags 0x22, changed=false, hardware 3D capacity 0. Thus the
host did not add a flag in this launch; Setup had already requested software-only. LS3D's native
InitDSound skips InitEAX when flag 0x20 is set, so this combination does not initialize EAX3.
Both live EAX interfaces are null. Bridge modules confirms Windows SYSTEM32/DSOUND.dll, with no
loaded DSOAL/OpenAL EAX implementation. Terminal module enumeration failed; the in-process modules
method provided the evidence instead. No game focus, settings or process lifecycle was changed.
An actual EAX comparison requires software-only off at native initialization plus a working EAX3
provider; merely allowing EAX in Setup cannot supply the missing implementation.

### 2026-11-13: owner-requested EAX launch

DSOAL is now installed in Mafia10Fixes only, superseding the candidate status above. Built x86
from upstream commit e11128250439d586483ee6598d1670ab52ab5e37; dsound.dll SHA256
65d1ca9b56f022ab7dcf98d73726aa36915e2846911a834ed4e19380aac3a595. Official OpenAL Soft
1.25.2 Win32 soft_oal.dll installed as dsoal-aldrv.dll, SHA256
606cf8b4c22c7ae00813585279ac045115907bc66d1483eb26355907021ea4b0. Downloads use fetch.py;
archive hashes, upstream commit response and build are retained in build/eax-20261113.
Licenses are in the game's licenses/eax. No system audio DLL or COM registration changed.

The game reads HKLM32 Software/Illusion Softworks/Mafia, LS3D_setup. Normal writes were denied;
RunAsInvoker allowed launch but did not make this process consume its existing VirtualStore setup.
That unsuccessful user-local test was reverted bytewise. After the owner's explicit permission to
elevate, a hidden elevated helper backed up the native 63-byte blob and changed only software-only
byte +0x20 from 1 to 0. EAX-disable was already 0. HKCU's ordinary Setup value remains unchanged;
the RUNASADMIN compatibility preference also remains unchanged. This HKLM Setup value is shared by
native 1.0 installs; the game-local provider is installed only in the owner's copy.

Final PID 24008, build 1.0: native flags 0x300002, both EAX property interfaces non-null, provider
capacity 128 3D/mixing slots, software_buffers=false, changed=false. Both loaded provider modules
are from Mafia10Fixes. This establishes actual EAX3 initialization. It does not yet establish an
audible benefit in an authored room or stable pause/restart behavior with this different backend.
Profile 1 main menu, both labs closed. window.focus still reports another foreground process;
neither successful launch nor EAX initialization proves the game's window was activated.

Installation/elevation/verification receipts and original Setup blobs are in build/eax-20261113.
Rollback with the game closed: remove only the two provider DLLs installed here and restore the
original software-only byte from before-owner-eax/HKLM32-LS3D_setup.bin, comparing current settings
first so later unrelated Setup changes are preserved. Restoring that byte requires elevation.

- Standalone acoustic regression: tests/dsound (synthetic source, WASAPI endpoint capture, standard
  library analyzer). The deferred rollback reproduces; the software control preserves actual rate
  and gain. It also proves why the removed gain-only test could not establish a complete repair.
- tests/game/test_sound_allocation.py: five real assertions per build, including actual buffer caps.
  Loader build, paired ABI witnesses, generated protocol/Rust freshness pass. Full offline-suite
  success is not claimed. Hardware/EAX-provider branches were preserved but not exercised here.
- Both real labs: allocation-lab10-clean and allocation-lab12-clean. Paused/reapplied dominant-rate
  behavior stays stable at 4800 rpm; actual buffers use software allocation. Earlier aborted probes
  remain recorded, including focus loss and the initial policy incorrectly gated on generic mixing.
- Owner long foreground recording: baseline/post-pause/post-throttle -16.060/-15.992/-16.046 dBFS.
  Dominant line remains 161.13 Hz throughout. Post-pause/post-throttle bands from 80 Hz to 20 kHz
  differ by at most 0.09 dB. The 20..80 Hz band varies by 0.61 dB (baseline/post-pause 1.22 dB);
  do not claim the entire waveform or every low band is identical.
  While throttle is held without a transition, two-second 20..80 Hz windows within a single stage
  already span 1.17..3.13 dB. Thus this low-band difference alone does not isolate a pause defect.
  Independent-phase PCM fits infer
  all three actual playback rates within 0.05 Hz of 28664/30869/44099 in each stage. These fits leave
  18..22% RMS residual (other sounds/filter/interpolation), so they are not exact per-voice gain proof.
  Capture has no timestamp errors. The owner has not yet listened to this new build.
- Foreground/background regression in owner PID 10752: -16.42 dBFS foreground, -120.15 while
  actually background, -16.09 after restoration. The game is silent in background with the new
  allocation path. The first fixture using SW_SHOWMINNOACTIVE retained the game's foreground HWND
  and was rejected; minimization alone is not a valid foreground-loss assertion. The valid fixture
  uses explicit minimization/restore and checks actual foreground on both transitions.

Final ASI SHA256 FF4E79EC3932DA3126398EFC9B7FBC4F849CB6980B909297E76EEBE415929DB3;
PDB E3568C392F09A7DAF774F7F1CECCE540F66D5D3372E82ABA2325D4D6B39763FA.
Installed in both labs and Mafia10Fixes. Owner PID 19440 closed normally for this update; new
PID 10752 restored profile 1, mise16-mesto, health 300/300, only ragdoll enabled. Full restore
engine_allocation_before_update_20261112 and before-owner-allocation/savegame precede installation.
Captured and restored exact native options: sounds 1, music 0, cars 1, dialogue 0. Game paused;
both labs closed, original river_water manifests restored, default lab unchanged.

Focus detail: bypassing our launch guard does not waive Windows foreground eligibility. When another
lab owns foreground, that cooperating game can grant the target PID via AllowSetForegroundWindow;
explicit window.focus then succeeds. This was verified without real input, queue attachment or
global policy changes. An external eligible controller also activated a lab while Medmis owned focus.

Artifacts: build/engine-pause-20261112. Separate audio stutter remains unresolved.
Short engine-old-check.wav and engine-native-check.wav preserve the recorded levels; each contains
before pause, after pause, then after throttle reapplication, with silence between the excerpts.

## Superseded gain-only investigation

The following paragraphs retain earlier observations and installation history for audit. They describe
the retired A86E5049 build and its rejected workaround, not the current implementation or validation.

**Open, owner rejected the claimed resolution:** the timbre still changes after pause. Aggregate
RMS within 1 dB does not establish equal layer balance or spectrum. The volume-transition hook is
an unaccepted workaround, disabled in owner PID 19440 for further causal investigation. Previous
validation below records measurements, not a complete fix. New owner options: sounds 1, music 0,
cars 1, dialogue 0; captured from the native menu before further experiments.

Owner case: Mafia10Fixes, native Direct3D 8, only ragdoll enabled. At redline in neutral,
pause and resume while retaining throttle. Engine roar becomes quieter; releasing and pressing
throttle restores it. The first agent reproduction was in the background and is NOT a valid
audible reproduction. Subsequent recordings verify actual foreground at their sample boundaries.

## Evidence and limits

- The owner's later music/effects adjustments made the difference measurable in endpoint loopback.
  At the same 4800 rpm, recorded-settings.wav is about -33 dBFS after resume versus -24 dBFS
  after throttle release/reapplication. A following run measured about -41 versus -24 dBFS.
- All 16 populated car sound pointers were inspected, including those absent from the pause vector.
  The same three loops play at steady redline: th_2.wav, th_4.wav and arrow_3.wav. Their requested
  volume, DirectSound GetVolume attenuation, frequency and playing/looping/software flags agree
  in the quiet and loud states. A playing flag is insufficient evidence of the rendered gain.
- DirectSound3D source/listener parameters and native driver volume also agree apart from small
  car/camera movements. These observations do not establish an internal Windows implementation cause.
  The live module is Windows' DSOUND.dll, not a game-folder wrapper.
- While retaining throttle and rpm, reapplying exactly the current DirectSound volume did not
  recover the gain. Setting each affected buffer one hundredth of a decibel lower and immediately
  restoring its original attenuation did recover normal output. Frequency refresh was not sufficient.
  Restarting individual native voices without seeking reproduced attenuation loss. These are
  compatibility experiments, not proof that the game's engine-layer selection is defective.
- Recordings are endpoint mixes on Windows 10 build 19045, not process-isolated recordings.
  Edge playback was stopped by the owner. SoundKeeper64 remained as a known background source;
  pause samples establish its very low background floor. The recorder rejects other nonzero
  enumerated sessions. It cannot rule out a new unenumerated session appearing during capture.

Artifacts and complete timing/native-state records: build/engine-pause-20261112. The initial
before.json probe labels +0x188/+0x18C backwards; raw bytes are retained. Correct mapping below.

## Native witnesses

| Function | Game 1.0 | Game 1.2 |
|---|---|---|
| C_Vehicle::EngineSnd | 0x538220 | 0x4EBAB0 |
| C_Vehicle::EngineFreq | 0x5385B0 | 0x4EBE40 |
| C_game::PauseAllSounds | 0x5F40B0 | 0x5B8430 |
| recursive scene pause | 0x5F41C0 | 0x5B8540 |
| C_game::PlayAllSounds | 0x5F4270 | 0x5B85F0 |
| ISND_sound::Play(flags, priority) | ls3d+786E0 | ls3d+7CAA0 |

ISND Play is an exact paired function. Both start with `83 EC 10 53 8B 5C 24 18 8B 43 08`;
the null-buffer exit at +0x20 is `ret 0x0C`: stdcall(voice, flags, priority). Voice +8 is its
DirectSound buffer, +0x10 its flags (0x200: 3D, 0x2: no volume control), +0x18 its linear volume.
ISND_LOOP=0x100 and ISND_NOREWIND=0x400. Play may return success for a queued voice, so a gain
repair must check that the backend buffer is actually playing before touching its attenuation.

I3D_sound: +0x140 dirty/state flags, +0x144 logical state, +0x15C ISND voice, +0x160 source,
+0x164 loop, +0x188 out volume, +0x18C volume, +0x190 computed attenuated volume, +0x194 pitch.
SetOutVol is vtable +0x64; SetVolume +0x68. IsPlaying is +0x54: false queries logical state,
true the backend. Its bool result is in AL; upper EAX bits are not a boolean value.
Pause vector at C_game+0x2D8 is a sequence of 20-byte records: sound, volume, out-volume, loop,
position in milliseconds. PlayAllSounds starts a voice before restoring its saved cursor.
Reordering that sequence was not tested and is not claimed as the cause or fix.

## Implementation and validation

host/fixes.cpp hooks the paired ISND Play entry. On native success, a 3D playing buffer with
volume control receives the 0.01 dB downward transition followed by its exact original attenuation.
No seek, audio reset, frequency/loop edit or persistent setting change. Fully muted and queued
buffers stay untouched. The original playback result is retained. sound.compat exposes a
process-only A/B toggle and refresh/failure counters. The helper's offline test covers exact
restoration, attenuation bounds, stopped/muted buffers and failed backend operations.
Foreground endpoint A/B at 4800 rpm (dBFS: baseline, post-pause, post-throttle):

| Build | Compatibility off | Compatibility on |
|---|---|---|
| 1.0 | -50.91, -61.00, -52.88 | -51.55, -50.36, -50.95 |
| 1.2 | -35.68, -38.81, -38.09 | -38.60, -38.69, -38.08 |

The strong loss reproduces in 1.0; the 1.2 off run does not reproduce its magnitude. The enabled
path remains stable in both. Final owner recording owner-fixed.wav measures -39.69 dBFS after
pause and -40.56 after throttle reapplication at the same 4800 rpm: no prior 9..17 dB loss.
Backend repair failures remain zero. This verifies the narrow compatibility behavior on this
machine; it does not establish Windows' internal cause or resolve the separate stutter report.

Final loader SHA256 A86E504967D6070E9D32A65B8991A6FCDBB3CFDA5209C3B1F88DF5504FAC673D
is installed in both labs and Mafia10Fixes. Owner PID 27376 required termination after the normal
10-second close timeout; full native restore engine_gain_before_update_20261112 and a savegame
backup preceded it. PID 19440 restored profile 1, mise16-mesto, health 300/300; only ragdoll loaded.
The unsaved audio settings reverted on restart. Captured master volume 0.1611328274 and music 0
were restored through the native options controls. In 1.0 menu.value changes the control only;
menu.command with that control's id commits its native action. Returning alone does not apply it.
Cars/dialogue slider preferences were not captured before restart; the owner was asked for them.

The initial apparent Windows focus refusal was our own background-launch SetForegroundWindow
detour returning TRUE without forwarding. Explicit window.focus now uses the original entry and
reports actual foreground HWND/PID separately. Five focus assertions pass per build, including
restoration from minimization; the ordinary background-launch guard still suppresses activation.
Both labs were closed after testing and their river_water manifests restored bytewise.
