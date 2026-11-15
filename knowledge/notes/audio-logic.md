# Sound-driven gameplay in Mafia 1.0 (2026-11-13)

Research note with explicitly 1.0-only logic findings. This is not an update to the loader's shared gameplay
contract, a fix request or a speedrun-rule approval. The separate current audio fixes and validation limits
are in [STATUS.md](../../docs/STATUS.md) and [the update notes](../../docs/PATCHNOTES_2026-11-15.md).

Scope: original 1.0 game/LS3DF logic, investigated for the owner's speedrun questions. No gameplay
fix was requested or installed. Native calls, artificial channel pressure and MML lab fixtures are
diagnostic tools, not proof of a player-executable or leaderboard-approved strategy. See audio.md
for provider installation and the separate deferred-buffer pause defect.

## Engine start has an actual sound-dependent gate

Verified in 1.0 disassembly, not inferred from audible silence:

- C_Vehicle::SetEngineOn 0x5178E0 requests normal startup, or bypasses it in its silent argument path.
- C_Vehicle::EngineSnd 0x538220 checks four start/stop sound slots: vehicle +0x6C8, +0x6CC, +0x9A8,
  +0x9AC. Pending startup is +0x6B4; engine-on is +0xCA8. Sounds-enabled is +0x4AE.
- With sounds enabled, a slot holds startup while IsPlaying(false), length >300 ms, and unsigned
  length minus current position >300 ms. The 300-ms comparisons are at 0x538386..0x5383A5 and the
  corresponding later slots. When no slot holds the gate, engine-on becomes 1 at 0x5384BB onward.
- IsPlaying(false), 1.0 ls3d+54D60, is a logical state/wall-clock test, not DirectSound GetStatus.
  GetCurrTime, ls3d+55350 -> ISND_sound ls3d+78C80, uses the backend play cursor divided by source
  bytes/ms. GetPlayTime comes from source size / bytes/ms. A reverb tail is not added to that length.

The owner's two existing skips have distinct explanations. Disabled sound can leave sound slots
unopened/null, bypassing their checks. A distant camera makes C_car::Update (1.0 0x466B80) disable
vehicle sounds, bypassing the gate even if slots exist. Its squared distance limits are 3600 and,
with siren_on, 12100 (constants 0x63BCF0/0x63BCEC, verified bytes): approximately 60/110 world metres.
Other vehicle-active/frame conditions also participate. Huge coordinates are not required by this
specific comparison; a practical in-game way to cross the threshold was not reproduced here.
Bridge camera.set only overrides the rendered view and is not a substitute for that gameplay camera.

## Actual timing probes and their limits

Artifacts: build/eax-20261113/engine-start-*.json and engine-start-probe.py. Lab 1.0 only, Bolt Thrower,
native SetEngineOn(false,true), SoundOff, then SetEngineOn(true,false); observe engine-on in memory.
The start WAV length is 3344 ms. No direct engine-on field writes. Lab has MML core, fps_unlock and
river_water; this is not a clean speedrun-install certification. Timing starts before native-call
dispatch and includes scheduling/frame latency; polling bounds are retained.

Foreground probes, three starts per cell (milliseconds, first observed engine-on):

| Backend | Normal volume | Cars slider zero | Master slider zero |
|---|---|---|---|
| Windows DirectSound, flags 0x22 | 3156,3159,3177 | 3198,3209,3151 | 3095,3141,3186 |
| DSOAL/OpenAL Soft, EAX3, flags 0x300002 | 3064,3117,3137 | 3110,3140,3154 | 3119,3137,3107 |

All retain sounds_enabled=1. Neither slider gives instant start. These rows change the backend and
allocation policy as well as EAX support; their small difference is not an isolated EAX effect.

Matched background controls then keep DSOAL/OpenAL Soft and master volume zero in both runs:

| EAX extensions | Native flags | Three starts, ms | Median, ms |
|---|---|---|---|
| Enabled | 0x300002 | 3119,3135,3106 | 3119 |
| Disabled with game-local [eax] enable=false | 0x100002 | 3077,3121,3120 | 3120 |

No activation/focus requests occur in the background fixture. No instant start or repeatable EAX3
speed advantage established. Three sequential trials cannot prove equivalence or exclude subtler
effects, other vehicles, frame-rate dependence or pressure/mission-specific cases.

## Native channel management differs from EAX3 room effects

InitDSound, 1.0 ls3d+77AD0, takes the provider's free 3D-channel capacity. Values above 35 are reduced
to 30 at +77Bxx (global ls3d+1C1F58). DSOAL advertises 128, but Mafia uses 30 in this manager.
The channel list/queue are used and updated with detected IKsPropertySet flag 0x100000; EAX3 sector
effects use 0x200000. Disabling OpenAL's EAX extensions removes the latter but leaves the former.
Thus disabling audible EAX3 does not necessarily disable the native queue path.

Two errors in that path are relevant leads:

1. **Wrong buffer's cursor saved on eviction: reproduced.** ISND_sound::Play at ls3d+789F4..78A1D
   copies the victim's sound/buffer identity into its queue record, then queries this/incoming
   buffer +8 for that victim's position. Silent background pressure fixture fills the native
   30-channel pool with bridge-owned loops, lowers one victim's priority and introduces another
   sound. Victim cursor before eviction: 5732 bytes. Incoming cursor: 3528. Victim's saved position:
   3528. Evidence channel-eviction-v2.json; foreground HWND unchanged. This is an artificially
   arranged native-API reproduction, not a demonstrated gameplay route to an engine/dialogue skip.
2. **Byte position consumed as milliseconds: verified in code, consequence not reproduced.**
   Queue position comes directly from DirectSound GetCurrentPosition (e.g. ls3d+7893F..78946).
   UpdateAllTerminatedBuffs, ls3d+77F80, adds wall-clock milliseconds to it and passes it to
   ISND_sound::SetCurrTime, whose +78D08 multiplies the argument by bytes/ms. Normal GetCurrTime
   explicitly divides by bytes/ms at +78CBA. The mismatched units can cause wrong resume/expiry
   positions; a useful naturally triggered speedrun skip remains unestablished.

The first pressure fixture incorrectly used ISND vtable +0x10, which is Play(source,flags) in the
actual 1.0 table, and hit a caught native-call fault. It is discarded. The actual table at
ls3d+99958 puts Play(flags,priority) at +0x0C; verified before the successful retry, with a fresh lab
process. This illustrates why reconstructed C++ declaration order alone is insufficient ABI evidence.

## Other settings affecting the logical path

Dialogue volume has a nontrivial branch: 1.0 DabingSpeak 0x5473B0 checks volume 0x6724A0 against
approximately 0.01 and removes/omits the sound object below that threshold. DabingTick 0x547C10
then uses a delta-time/morph-data timeline rather than the sound cursor, with 40-ms morph frames
and an end-state delay. DabingIsSpeaking 0x547A80 is used by script processing and pumper AI.
That is a real change in the timing mechanism, but dialogue/morph asset durations and a concrete
mission must be compared before calling it a faster skip. No mission-level speech skip tested.

## Working preferences and final state

Owner explicitly instructed: no computer-use or desktop/window control; perform further research
in the background through the game bridge. Early foreground handoffs used scoped grants and then
computer-use activation; physical Escape stopped that tool. After the owner's clarification, only
background native tests continued, with no further activation calls.

Both labs closed, all temporary DSOAL DLLs and alsoft.ini moved out of the 1.0 lab into ignored
evidence folders. Lab manifests/mode and default game_dir unchanged. Owner PID 5988 was not restarted;
its EAX3 provider remains active. No host/mod source or binary change in this investigation.
