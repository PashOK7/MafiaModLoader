# The game's script machine: agents' and mods' hands in the world

The missions of Mafia are programs in a small language of the game's own (the scripts in each mission's
`scene2.bin`), compiled and run by the game's interpreter, `C_program`: about 390 commands that move actors, give
weapons, kill, spawn copies, drive cars, change the weather, the traffic, the camera, the HUD. The loader gives that
machine to agents and mods: a program sent to it is compiled by the game's own compiler and run by the game's own
interpreter, one step each mission tick, exactly as a mission runs its scripts. Whatever a mission can do, a program
can.

- **Agents** (MCP): `script {code, act, flt, owner, timeout_ms}` runs a program and waits until it ends: its state
  (`done`, `failed` with the line, `stopped`) and its registers. `script_commands {query}` lists the commands with
  their opcodes and handlers in both builds and, where st0rm94's 1.0 reverse is cloned in `re/ext`, its note on each
  command's arguments and behaviour (read in place; a lead: check in the game). On the bridge: `script.run`,
  `script.state {id}`, `script.stop {id | "all"}`, `script.list` (docs/PROTOCOL.md).
- **Mods** (`mml.h`): `script_run(mod, code, owner, acts, n, flts, n, timeout_ms, fn, user)` returns a handle; `fn`
  gets an `MmlScriptResult` (the state, the error and its line, the registers) when the program ends;
  `script_stop`, `script_state`. A mod's programs stop when it unloads. The telemetry example's F7 repairs the car
  this way.

A mission must be running: programs step in the mission's tick, so they wait while the pause menu is open. Both
builds (seen in the 1.2 and the 1.0 labs, 2026-10-14).

The common things need no program: see "The world without a program" below (`spawn`, `teleport`, `give`).

## The world without a program

What agents and mods do most is named (host/world.cpp; MCP `spawn`, `teleport`, `give`; the bridge's `world.*`,
docs/PROTOCOL.md; mods' `actor_spawn`, `actor_remove`, `actor_teleport`, `actor_seat`, `give_item`, `item_id`,
docs/MODDING.md). Seen in both labs (2026-10-14).

- **A car or a person of any model, made new** (`spawn {"car": "Bolt-Thrower"}`, `{"human": "Paulie", "in":
  "car"}`): no command of the missions makes an actor from a model file (`actor_duplicate` copies one, `model_create`
  makes a bare frame), so the loader does what the game's own `ACTOR_DUPLICATE` case does with a source and
  `DAN_INTERNAL_1` (a developer's command that makes a "cheat_car", a Bolt-Thrower) does with a fixed model: a model
  frame from the I3D driver, the file opened by the model cache, the frame named (`<model>_mml<n>`), placed, turned
  and linked into the sector at its place, then `C_mission::CreateActor` (4 a car, 27 a person: the game's NPC
  kind), the actor's `Init` on the frame, and `C_game::AddTemporaryActor`. The actor owns its frame (the
  duplicate's flag) and stays out of saved games (the cheat car's); a car's particles are on and it is settled on its
  wheels (`CalmVehicle`, as the cheat car) when there is ground within 5 m under it: CalmVehicle steps the physics
  for 100 ms of real time, and a car over the river fell 315 km in that one call; elsewhere it falls in the game's
  time. The water's faces (surfaces 31, 44) are static faces the game's ray meets, but nothing stands on them: they
  are not ground for `spawn` (a place over the river keeps its height, a car put there falls in). A model a person is made from must have a person's bones (the frames the
  shot code reads without checking; MafiaOnline's list): another model is refused. The spawned car was entered
  ("Get in the car as the driver"), driven and its HUD shown; a spawned person held a Thompson given to him; one put
  into the player's car rode in it.
- **The names**: `world.cars` reads the game's car list (G_CarDatabase: the names the menus show, the models'
  prefixes; a car's file is `<prefix><colour>.i3d`); `world.items` the items of `tables\predmety.def` (the name, the
  kind from its flags, the model, the rounds in a magazine and in reserve). Names are matched without case, a part
  of one when only one item has it.
- **Moving, seating, arming** are the missions' own commands, run by the game's interpreter at once, in the same
  frame (a program compiled and stepped until it stands on its end; a command that waits for frames cannot be run
  so): `teleport` is `actor_setpos` (a car also `car_repair`, unless `repair` is false; a turn first, as
  `ACTOR_SETDIR` does it: the actor's facing, its frame's SetDir), `world.seat` is `human_force_settocar`, `give` is
  `human_addweapon` with the item's own magazine and reserve.
- **Taking out**: `world.remove` (`actor_remove`) is `C_game::RemoveTemporaryActor` (what `actor_delete` calls); the
  game deletes the actor at its next tick. Its model would stay in the scene (the sector holds the frame: seen, a
  removed Paulie's model stood where he was), so the loader takes the frame out of the scene when the actor's
  destructor runs. Refused while a person sits in a car, him or the car: the car's frame is his parent.

## The language

- One command a line; `//` starts a comment; empty lines are skipped (the loader strips them, and leading blanks,
  before the game compiles). Command names are case-insensitive. Arguments are separated by commas. Numbers are
  floats. A name (an actor's, a frame's, a model file) is in double quotes: `findactor 0, "Tommy"`; a few commands
  take bare keywords instead: `findnearactor 0, 1, Car`, `weather_setparam ON, 1` (in quotes the keyword is not
  recognised and the command finds or sets nothing).
- **Registers**: `flt[]` floats, `act[]` actors, `frm[]` frames, addressed by index in the commands' arguments
  (`actor_setpos 0, 1`: actor `act[0]` to frame `frm[1]`). The loader gives every program 256 floats, 64 actors and
  64 frames before it starts, `act[]` and `flt[]` filled as asked (`act: {"0": "player", "1": "car", "2":
  "0x0F923CF0"}`, `flt: {"0": -1940.5}`); `dim_flt`, `dim_act`, `dim_frm` are not needed (a larger one makes new,
  zeroed registers, losing what was given).
- `let flt[0] = flt[1] * 3`: one operator (`+ - * /`), no precedence, no parentheses; the left side must be a
  `flt[]`.
- `label 10` ... `goto 10`; `if flt[0] = 1, 10, -1`: to label 10 when true, else to `-1` (no such label: the next
  line). Comparisons `<`, `>`, `=`, `!` (not equal).
- `wait 500` (ms): the program gives the frame back and goes on later. Most commands take one step; a few wait by
  themselves (`human_waittoready`).
- `commandblock 1` ... `commandblock 0`: the lines between run in one frame. A loop with no `wait` inside a block
  never gives the frame back: the game hangs.
- `end` ends the program (also the last line does). The loader stops a program after its time (`timeout_ms`,
  10 s from MCP, 30 s from the bridge and for mods).
- **Arguments are read leniently**: a word where a number is expected is 0, a missing argument is 0. A line the
  compiler does not take (an unknown command, arguments it cannot parse) fails the whole program before it runs:
  `line 4: "humna_addweapon 0, 2, 3": not a command of the game's`.
- **The owner**: a mission's script runs as its entity; a program of the loader's runs as `owner` (default none).
  Commands that act on "the script's own actor" use it: `explosion` goes off at its position, `playsound`,
  `frm_seton`, `act_setstate` with no register of theirs; `findnearactor` skips it.

## Finding what to act on

- The player: `act: {"0": "player"}`, or in the program `getactiveplayer 0`. His car (or the last one he drove):
  `"car"`. Any actor by its address, as `call actors` and `call humans` list them (type 2 the player, 4 a car, 27
  an NPC with a script).
- By name: `findactor 0, "Tommy"` (an actor by its model frame's name), `findframe 0, "police1"` (a frame of the
  scene).
- The nearest of a kind: `findnearactor 0, 1, Car` puts the nearest car to `act[0]` into `act[1]`. The kinds: Player,
  Enemy, Car, Detector, Door, Railway, Box, Bottle, Traffic_person, Traffic_car, Bridge, Dog, Airplane, Turnout,
  Pumper. Traffic's cars are not single actors (`Traffic_car` finds the traffic manager).
- An actor's own frame: `getactorframe 0, 1` (`frm[1]` = `act[0]`'s model frame).

## Recipes (each run in the lab)

Teleport a car (or the player on foot) to `flt[0..2]`:

```
getactorframe 0, 0
frm_setpos 0, 0
actor_setpos 0, 0
car_repair 0          // a car: stopped, repaired and put on its wheels where it is
```

with `act: {"0": "car"}`, `flt: {"0": x, "1": y, "2": z}` (world metres, y up). The player sitting in a car is the
car's child: his frame is in the car's space, so move the car, not him.

A copy of an actor (here a car) next to the player:

```
actor_duplicate 1, 2, 0     // act[1] cloned into act[2] (a temporary actor: actor_delete 2 removes it)
getactorframe 2, 1
frm_setpos 1, 0
actor_setpos 2, 1
car_repair 2
```

with `act: {"1": "0x0F923CF0"}` (any car from `actors`) and the place in `flt[0..2]`. The clone's actor is in the
result's `act[2]`.

A weapon: `human_addweapon 0, 6, 8, 40` (the character `act[0]`, the weapon's id from the game's
`tables\predmety.def`, rounds loaded, rounds in reserve). An NPC holds it at once; the player gets it in his
inventory.

An explosion at an actor: `explosion 6, 400` (radius in metres, force; a grenade's is 400) with `owner` that actor.

The weather and the city: `weather_setparam ON, 1`, `weather_setparam MAX_CNT, 20000`, `weather_setparam SPEED, 30`
(rain on, the drops, their speed; `ON, 0` stops it; the player's detail setting caps the drops);
`setcitytrafficvisible 0` hides the city's traffic (it goes on driving, without collisions), `1` shows it.

Time: `getgametime 0` (ms since the game started into `flt[0]`), `wait`, a loop:

```
label 1
let flt[0] = flt[0] + 1
wait 100
goto 1
```

(stopped by its time, or `script.stop`).

Others worth knowing (see `script_commands`): `human_death`, `human_changemodel`, `human_force_settocar` (a
character into a car's seat), `car_setspeed` (km/h), `car_lock`, `zatmyse` (the screen faded to black and back),
`camera_lock` / `camera_unlock`, `console_addtext` (a text of the game's database in the HUD's console),
`player_lockcontrols`, `explosion`, `create_physicalobject`, `actor_delete`, `frm_seton` (a frame shown or hidden),
the vector and matrix commands (`vect_*`, `matrix_*`, `quat_*`) on runs of `flt[]`.

## Orders (a person's own program; host/orders.cpp)

The people the missions place, and those `spawn` makes, are `C_entity` actors: each carries a program of his own
whose vtable also knows the people's commands (`ENEMY_*`, and the same as `ENTITY_*`: st0rm94's structs/entity.md
§4d lists the 64). `human.order {actor, code, act, flt, at}` (mods: `human_order`) puts `code` into that program and
hands him to it; his AI runs it. Their arguments are register numbers (`act[i]`, `frm[i]`) and numbers, as the
decoder reads them (`C_entity::DecodeParams`); the ones the named orders use, seen in both labs:

| command | arguments | what he does |
|---|---|---|
| `enemy_move_to_frame` | `frm`, `radius`[, `run`] | walks (runs) to the frame and stands within `radius` metres of it; done there |
| `enemy_action_follow` | `act`, `distance`[, `CROUCH`, `RETURN`, `NOLINK`...] | keeps `distance` from the actor, on foot or into his car; lasts |
| `enemy_action_fire` | `act`[, `distance`] | fights the actor with what he holds; lasts while the fight does |
| `enemy_action_runaway` | `act`, `distance`[, `return`] | runs from the actor while he sees, hears or smells him; lasts |
| `enemy_move_to_car` | `act` (a car), `seat`[, `run`] | walks to the seat's door; done there |
| `enemy_usecar` | `act` (a car), `seat` | gets into that seat (free, the car still, within 3 m); with no arguments, out of his car |
| `enemy_stop` | | the command and his animation stopped |

`at` gives the frames: `at: [[x, y, z], ...]` makes `frm[0]`, `frm[1]`... dummy frames of the loader's there (freed
with the order). Each command that queues an action of the AI (a move, a follow, a fire: 16 of them) waits until
the AI has done it; the others take one step.

```
enemy_move_to_frame 0, 1, run      // to frm[0], at: [[x, y, z]]
enemy_move_to_car 0, 1             // to act[0]'s door beside the driver
enemy_usecar 0, 1                  // and in
```

How the game runs it, read in both builds (the same code, 1.2's `C_entity::*` and st0rm94's 1.0 names):

- `C_entity::ForceScript(entity, false)` (what the scripts' `ENEMY_FORCESCRIPT` calls) raises the event 0xEA63 on
  his program, `SetEvent` jumps it to its `event 60003` (the loader's first line), and its callback sets the flag
  0x200 of `+0xF08`: `C_entity::AI` then runs the program each tick (`CallProcess`, `scriptTick`).
- An action command adds its job to his action queue, sets 0x800 and gives the AI the entity (`ForceAI(0, 1)`:
  `+0xF44` 1, `+0xF58` 1.0). When the queue is empty, `actionsTick` puts `+0xF58` at 0 and `ScriptSwitch` hands
  him back to the script once `+0xF58` is below the program's `+0x60` (raising 0x400: the command is done).
  `C_program::Init` sets that `+0x60` to 0.5 for a program with instructions and to 0 for an empty one, so a
  person made with no script would never be handed back: the loader sets it as Init does.
- `END` calls `C_program::ClearAfterEnd`, which puts the owner's act state (`C_actor +0x1C`) at 2: the next
  `C_game::ChangeActState` takes him out of both lists of actors (the ticked, state 0; the asleep, 1) and into
  neither, and `SetActState` never changes a 2, so he is no longer ticked at all (no AI, no animation).
  `C_actor::Rebirth` makes the state 0 again but puts him into no list (the game's own callers then add him by
  `C_game::InternTempActAdd`, which does nothing for an actor already among the temporary ones, as spawn's are).
  Hence the loader's own end (`label 60003`, `wait 1000`, `goto 60003`), the code's `end` lines made `goto 60003`,
  and an order to a mission's NPC whose own script did end refused. (The loader's own programs, `script`, are freed
  when they stand on their END, before the game runs it: their `owner` keeps his state.)

## What does not run here

- Commands whose dispatcher arm is the default one (`"runs": false` in `script_commands`: `DEBUG_TEXT` and other
  markers) do nothing in a standalone program; some are handled only by a mission entity's or a detector's own
  override.
- `callsubroutine`, `call` reach other actors' programs and the mission's global ones: they behave as the game
  does, untested here.
- Events (`settimeoutevent`, `setplayerfireevent`...) register a C_program to be raised later: a program of the
  loader's ends when it reaches `end`, so an event to it may come after it is gone (untested).

## How it works (host/script.cpp; the addresses: game_build.h `prog*`)

A program is a standalone `C_program` (the class every mission entity carries at `+0xA9C`, a detector at `+0x70`)
made with the game's operator new and constructor; its source is compiled by the game's `Compile` (each line by the
vtable's DecodeInstruction, then its DecodeParams), stopping at the first line that fails; its registers are
allocated with the game's new as its own `dim_*` would; then `CallProcess` (the vtable's Process: the opcode
switch, until an instruction gives the frame back) once a mission tick, after the mission's own tick. `END` leaves
the program standing on it: that is its end. A fault in a command stops the program (the guard tells the line and
the address); the mission's close stops every program (their actors go with it); an actor deleted is taken out of
every program's `act[]` by the game's `InvalidateActorReg`. The game's `Done` and deleting destructor free it.
