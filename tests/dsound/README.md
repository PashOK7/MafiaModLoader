# DirectSound restart regression

This opt-in test plays a synthetic tone and records the default render endpoint through WASAPI.
It uses no game files and no microphone. Keep other audio quiet; the recorder rejects other active
enumerated sessions with a nonzero signal. A new session appearing later is not enumerated again.
Run separately from game audio captures and GPU/performance tests.

```
cmake -S tests/dsound -B build/dsound-regression -A Win32
cmake --build build/dsound-regression --config Release
python tests/dsound/run.py --executables build/dsound-regression/Release --prefix build/dsound-regression/check
```

Two cases duplicate a mono spatial buffer, start at 16000 samples/s and -15 dB, change it while
playing to 30869 samples/s and -4.11 dB, then stop/restart and seek. The deferred case uses
`DSBCAPS_LOCDEFER` with `DSBPLAY_LOCSOFTWARE`; the control uses `DSBCAPS_LOCSOFTWARE` at creation.
The test measures the rendered tone's rate and level, separately from DirectSound's getters.
On the affected Windows backend, deferred restarts return to the allocation-time rate and gain
despite getters reporting the newer values. A volume transition repairs only gain. The software
case must retain actual gain and rate across every restart/seek (0.1 dB tolerance).

The deferred bug may be absent on another Windows/audio backend; that is reported, not treated as
a failure. The software control must pass. `--analyze-only` checks saved recordings without audio
playback. An explicitly known background session can be allowed with `--known-background-pid`.

The live-game counterpart is `tests/game/test_sound_allocation.py`, which checks that LS3D's
capability policy reaches real spatial buffers in both game builds. See `knowledge/notes/audio.md`.
