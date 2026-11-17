# Native radar car lifetime

The HUD keeps a borrowed `C_car*` used by the vehicle radar and last-car queries. Its
lifetime follows the actual actor, rather than the mission's scene.

`CHANGE_MISSION` can detach the same player and car and attach them to the next scene.
Clearing the HUD car at every mission close loses that valid binding: the radar follows
the world origin until entering the car again. Keep it across scene cleanup. Clear it
when that exact car is destroyed, before `ACTOR_DESTROYED` subscribers can query stale
memory. Destroying another actor must leave the live binding alone. Genuine teardown
still clears the reference through the native actor destructor hook.

Both native setters assign one field and return with `RET4`:

| Build | RadarSetPlayerCar | HUD object | Car field |
|---|---|---|---|
| 1.2 | `0x5FA500` | `0x6BF980` | `+0x44A8` |
| 1.0 | `0x54C620` | `0x658330` | `+0x94A0` |

The real-game carry test runs the native command
`change_mission "freeitaly", "TomyhoAuto", 0` from the city freeride, with the player as
program owner. It verifies the exact player/car identities and retained radar binding.
Separate tests exercise actual native car deletion, another actor's deletion and return
to the main menu. This distinguishes scene transitions from destruction and ordinary
full-save loading, whose native human load calls `Intern_UseCar`.

```text
python tests/game/run.py hud_car_carry last_car_lifetime --profile 1.2=4 --profile 1.0=Run
```

Validated on 2026-11-17: the blanket-clear variant fails the carry assertion in both
builds; the actor-lifetime implementation passes all eight checks per build. Full offline
regression also passes. No game files, dumps or decompiled source are required by this patch.
