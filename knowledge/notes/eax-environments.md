# EAX environment attribution (2026-11-14)

State/PIDs below are the November 14 capture. The owner subsequently reported that the pause hum did
decay and deferred this inquiry. It remains an open fidelity question, not a confirmed permanent loop or
proof against either the native authoring or provider. Current scope: [STATUS.md](../../docs/STATUS.md).

The owner reports accumulating indoor rumble and similar outdoor echoes. Neither symptom has an
acoustic capture tied to a sector transition yet. This inquiry reads the original engine and current
provider state; it does not tune away the symptoms or establish a faulty component.

## Processing chain and current configuration

Mafia/LS3D selects environment parameters -> DirectSound IKsPropertySet -> DSOAL -> OpenAL Soft's
EAX implementation and reverb DSP -> Windows output. Our host does not implement this reverb DSP.
The pinned DSOAL buffer.cpp forwards EAX30 listener properties through EAXSetDirect. Its immediate
path suspends/processes the OpenAL context and commits on success.

Owner PID 19940, build 1.0, W:/Mafia10Fixes: sound.compat reports native_flags 0x300002,
max_hw_3d 128, software_buffers false, changed false, gain_workaround false. The host allocation
fallback is inactive with this provider. The Ogg cooperative shutdown change is separate from EAX.
On-disk providers still match the installation recorded in build/eax-20261113/owner-install.json:

- DSOAL e11128250439d586483ee6598d1670ab52ab5e37, dsound.dll SHA256
  65D1CA9B56F022AB7DCF98D73726AA36915E2846911A834ED4E19380AAC3A595.
- OpenAL Soft 1.25.2, dsoal-aldrv.dll SHA256
  606CF8B4C22C7AE00813585279AC045115907BC66D1483EB26355907021EA4B0.

No game-local alsoft.ini/dsoal.ini or AppData/alsoft.ini was found. The agent shell has no
ALSOFT_CONF/ALSOFT_LOCAL_PATH value. This is not an exhaustive inventory of the running process's
environment or downstream Windows/device enhancements.

## Native authoring and selection, checked in both builds

The environment update is LS3D RVA 0x48600 in 1.0 and 0x4AAA0 in 1.2. It copies or blends the
26-dword sound-properties block at sector +0x168. With driver flag 0x200000, it converts/clamps
that block and submits EAX3 ALLPARAMETERS (property 1, 112 bytes). In both builds this packet uses
environment id 26 (custom/undefined), size 7.5 and flags 0x3F; it is not a geometrically measured
room size. Other acoustic values vary with the sector data and blend.

In 1.0 the native Render callers at 0x5FAD0/0x5FB07 use the camera's sector (+0x3F8), its preceding
sector and transition weight. This is an authored sector model, not per-reflection tracing of the
visible walls. It also means camera placement can select another environment. 1.2's updater limits
intermediate blend submissions to roughly 100 ms intervals; 1.0's paired updater has no such gate.
This code difference alone is not a demonstrated acoustic bug.

## Read-only live sample

Native listener-property getter 1.0 RVA 0x76FC0 was verified in assembly (stdcall, five arguments,
RET 20; the first argument is unused). native.call with args
`[0, 1, {"buf":112}, 112, {"buf":4}]` returned a 112-byte block, output byte count 112, and correct
stack balance. The native wrapper discards the underlying HRESULT, so its return 0 alone would
not establish success; the filled block and returned byte count are the useful evidence.

Camera global LS3D+0x1C0CA8 -> camera +0x3F8 -> sector 0x02BB14D0, inspected as I3D_frame:
name "Primary sector", type 5. Its native sound block contains 1000 ms decay, 300 ms reflections
delay, 100 ms late delay and diffusion 0.4. Two provider readbacks agree:

| Property | Value |
|---|---|
| Environment / size / diffusion | 26 / 7.5 / 0.4 |
| Room / relative HF / relative LF | -1500 / -1 / -1500 millibels |
| Decay / HF ratio / LF ratio | 1 s / 0.29 / 2 |
| Early reflections level / delay | -3051 millibels / 0.3 s |
| Late reverb level / additional delay | -1000 millibels / 0.1 s |
| Echo time / depth | 0.25 s / 0 |
| Modulation time / depth | 0.25 s / 0 |
| Air absorption HF / HF reference / LF reference | -5 millibels/m / 5000 Hz / 250 Hz |
| Room rolloff / flags | 0 / 0x3F |

The substantial reflection delays in this sample are already present in the native sector.
EchoDepth=0 disables the explicit echo component, not early reflections or the reverberant tail.
This sample is not the owner's reported indoor-rumble event. No foreground activation, audio
recording, settings change, game restart or binary deployment was performed.

## Controls and remaining attribution

- Original Setup: EAX allow/disable and software-only selection. Native software-only skips EAX
  initialization; these are not interchangeable controls.
- Provider configuration: EAX extension enable, global reverb output boost, output layout/HRTF,
  buffering and logging. OpenAL Soft 1.25.2 documents [reverb] boost=0 by default and optional
  [eax] trace-commits. Reducing boost is a listening preference, not evidence of a root-cause fix.
- Native environment API: the room/filter levels, decay and frequency ratios, reflection/reverb
  gains and delays, diffusion, echo/modulation and per-source obstruction/occlusion exist. The
  host has no dedicated EAX environment command/settings page. A persistent override would need
  arbitration with the engine's next sector update; no such override was added here.

Repeated/reused outdoor sector parameters can legitimately give similar reverberation. A steady
loop can excite a reverberant low-frequency tail and build toward a steady level. Continued growth
or a tail that fails to decay after all contributing sources stop needs investigation, including
unnoticed looped sources. Neither observation identifies the provider or the game by itself.

For attribution, associate a short recorded event with native sector/blend parameters, the EAX
request and provider readback, plus active sound sources. Replay the same dry input and parameter
sequence against a reference provider in isolation. Equal property readback verifies parameter
transport, not historical Creative hardware's waveform. A literal claim about the original sound
requires a specified historical device/driver or trustworthy reference capture.

Primary references: [DSOAL](https://github.com/kcat/dsoal),
[OpenAL Soft 1.25.2 settings](https://github.com/kcat/openal-soft/blob/1.25.2/alsoftrc.sample),
[EAX property contract](https://github.com/kcat/openal-soft/blob/1.25.2/al/eax/api.h),
[EAX-to-reverb conversion](https://github.com/kcat/openal-soft/blob/1.25.2/al/effects/reverb.cpp).

## Sustained output during pause in chodba12, 2026-11-14

Owner PID 27624 was in the native pause menu in the prison corridor. Two independent ten-second
WASAPI session-meter runs found a continuous signal in the game's own Windows audio session.
With game time held at 94.8624, the second run's one-second peak values ranged from 0.086 to 0.111.
All 400 meter reads in that window were nonzero. Edge also had an audible session, so endpoint
loopback was rejected and no mixed recording was retained. Session peaks establish the producing
process and persistence, but do not identify the waveform or the producing mixer component.

Soundscape reported no logically playing sources. Read-only DirectSound GetStatus calls on all
643 attached buffers of the 863 registered LS3D sound frames found zero DSBSTATUS_PLAYING.
The 79 flags of 0x8 indicate hardware placement, not playback. The scan began and ended at the
same frozen game time. sounds.list had no bridge-owned audio and no active game music.
The old Soundscape filter alone would not have established the buffer result.

The current EAX3 property packet reports 2.42 s decay, LF ratio 2, 50 ms early delay, 30 ms
additional late delay, and zero EchoDepth and ModulationDepth. A long low-frequency tail fits the
authored parameters, but a signal holding level over ten seconds does not fit ordinary decay
after all inputs go quiet. The native PauseAllSounds at 0x5F40B0 pauses dialogue, scene sounds,
and tracked streams. DSOAL maps secondary buffer Stop to OpenAL source pause. The installed
provider hashes still match the pinned DSOAL and OpenAL Soft 1.25.2 build.

Registered LS3D buffers are only one class of mixer inputs. An untracked DirectSound source or
persistent output inside the provider remains possible. This capture does not assign the fault
to either or establish the historical Creative hardware behavior. A lab A/B should independently
remove the reverb contribution and inspect provider-side sources before changing pause policy.
Evidence is in build/eax-pause-20261114, including native-buffers.json and post-buffer-sessions.csv.
The owner game and audio settings were left in their observed state.
