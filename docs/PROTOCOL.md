# The bridge protocol

TCP on `127.0.0.1` only, port 47100 (or `bridge_port` in `<game>\mml\config.json`; when another process holds it,
the next free one of the 32 after it). The port in use, the pid and the token (new every run) are in
`<game>\mml\bridge.json`, written once the bridge listens. One JSON object per line, both ways. Every connection
starts with `hello` and that token:

```json
-> {"id": 1, "method": "hello", "params": {"token": "8f3c..."}}
<- {"id": 1, "result": {"pid": 25736, "build": "1.2", "api": 1}}
-> {"id": 2, "method": "mem.read", "params": {"addr": "game+0x2479F8", "size": 4, "as": "ptr"}}
<- {"id": 2, "result": {"addr": "Game.exe+0x2479F8", "abs": 6584824, "size": 4, "hex": "f87d6400", "value": ["Game.exe+0x247DF8"]}}
<- {"id": 3, "error": {"message": "..."}}
```

Addresses: `"0x5762A0"`, `"5762A0"`, `"game+0x1762A0"`, `"ls3d+1AAE0"`, `"<module>+<hex>"`, or a number.

## Methods answered at once (the bridge's thread)

| Method | Params | Result |
|---|---|---|
| `ping` | | pid, build, frames, api, uptime, game_dir |
| `log.tail` | `lines` | the last lines of the loader's log |
| `events.since` | `seq`, `types` (a list or "A,B"), `filter` (fields equal; strings in any case), `limit` | the events kept after `seq` (the last 2000; not each frame's: FRAME, MISSION_TICK, RENDER_*, PRESENT, VIEW_WORLD): `seq`, `t` (s), `type`, `data` (`actor`, `name`; a hit's `hit_type`, `damage`, `pos`, `dir`, `attacker`, `killed`; `MOD_FAULTED` `where`, `exception`, `at`; `MOD_UNLOADED` `why`; `SCRIPT_ENDED` `id`, `owner`, `state`, `why`; `SHOT` `pos`, `dir`, `damage`, `shots`; `EXPLOSION` `pos`, `radius`, `damage`; `HUMAN_DIED` `killer`; `VEHICLE_ENTERED`, `VEHICLE_LEFT` `vehicle`; `CAR_EXPLODED`; `GAME_SAVING`, `GAME_LOADED` `slot`, `profile`; `PEDESTRIAN_HIT` a hit's fields, `actor` the element, `model`; `BULLET_HIT` `actor` the shooter, `pos`, `normal`, `dir`, `damage`, `struck` (none: the world), `kind`, `surface`; `CAR_CRASH` `actor` the car, `struck` (none: the world), `pos`, `impulse`, `speed`, `surface`; `HUMAN_FELL` `speed`, `height`; `ITEM_GAINED`, `ITEM_LOST` `item`, `name`, `count`, `rounds`; `MONEY_CHANGED` `money`, `change`); `latest` (MCP `wait` waits on it) |
| `mem.read` | `addr`, `size` (≤ 1 MiB), `as` (u8 i8 u16 i16 u32 i32 f32 f64 ptr str) | hex (and the values) |
| `mem.write` | `addr`, `hex` or `as` + `values` | bytes written (code pages unprotected for it) |
| `addr.describe` | `addr` | module+offset |
| `modules` | | every loaded module: name, base, size, path |
| `where` | | the main thread sampled 5 times, then its stack's code addresses |
| `tap` | `cmd`: `"<at> [n=4] [ret\|fret] [ptr=<src>[+off][:len],...] [from=<ret>] [log] [max=N]"`, `"show <id> [k]"`, `"list"`, `"clear <id>"`, `"off <id>\|all"` | text: watch any function's calls (callers, ECX, stack arguments, memory behind them, strings as text, the result) |

Taps can also start with the game, before its first frame: `"taps": ["5409D0 n=4 ptr=1:32 log", ...]` in
`<game>\mml\config.json`.

## Methods for the game thread (answered at its next frame; `timeout_ms`, default 5000)

| Method | Params | Result |
|---|---|---|
| `mods.list` | | every mod: id, version, loaded, generation, error; `live_dir` (the load's folder), `last_unload` (threads found in its code and whether it was `kept`, libraries that `stays_loaded`, the process `since_load`: `free_mb`, `largest_free_mb`, `handles`, `threads`), `kept_generations` (loads not freed yet) |
| `mods.rescan` | | new mods in `mml\mods` loaded |
| `humans.list` | | every person (`actor`, `model`, `pos`, `health`, `alive`, `player`, `vehicle`), `count` |
| `human.skeleton` | `actor` (default `"player"`) | `bones`: `name`, `frame`, `world_pos`, `rot` (w x y z), `pos` (local), `x_axis`, `y_axis`, `z_axis` |
| `human.pose` | `actor`, `bones` (`{"r_arm": {"rot": [w, x, y, z], "pos": [x, y, z]}}`), `ms` (default 3000), `clear` | the bones held in that pose, drawn every frame for `ms` |
| `human.anim` | `actor` (default `"player"`), and `play` (a file of `anims\`, `"x.i3d"` or `"x"`; `simple`), or `stop: true`, or `replace` (one of his own animations) with `with` (another; empty: his own again) | played the game's way (Do_PlayAnim): `ms` its length (0: no such file); `stopped`; `replaced` |
| `human.anim_state` | `actor` | `playing` (the game's number), `asked`, `name` (its file), `state` (1 his own, 4 an animation with a track, 6 his action queue), `lying` (1 back, -1 front, 0 neither), `time_ms`, `length_ms` (the animations machine's active one), `blending`, `pedestrian_hits`, `pedestrians_hooked` |
| `human.get_up` | `actor`, `blend_ms` (default 600) | a posed body handed back: put where it lies, the game's get-up played: `from` (`back`, `front`), `animation` |
| `human.anims` | `query` (a part of a file's name) | the game's animation table, read from the running game: `anims` (each `number`, `file`), `count` |
| `world.ray` | `from` (`[x, y, z]`, an actor (1.5 m up), `"camera"` (default: the last frame's eye through `screen`, default the middle)), `to` (`[x, y, z]`, an actor) or `dir` and `length` (100), `what` (`all`, `static`, `moving`), `no_people`, `no_cars`, `ignore` (an actor; default the `from` actor, the player for `"camera"`; `""` none) | the game's ray test: `hit`, `pos`, `normal`, `distance`, `fraction`, `kind`, `flags`, `prim`, `surface` (static) or `actor`, `name`, `classes` (moving); `from`, `to`, `ignored` |
| `world.sweep` | as `world.ray`, `radius` (0.3) | the game's sphere sweep: the same, `pos` the sphere's centre where it stops |
| `world.ground` | `at` (`[x, y, z]` or an actor, 0.5 m up; default the player), `depth` (100) | the static world straight below: `height`, `normal`, `surface`... |
| `world.water` | `at` (`[x, y, z]` or an actor; default the player) | the game's water under it (looked for from 2 m above, 200 m down): `water`, `height`, `surface` (31 deep, 44 shallow), `deep`, `depth` (to the bed; -1 none in 100 m), `covered` (a bridge above it), `below` (its height under `at`), `prim` |
| `world.near` | `at`, `radius` (30), `type` (0 any, 2 people, 4 cars), `max` (50) | the actors within `radius`, nearest first: `actor`, `type`, `name`, `pos`, `distance`; `count` |
| `world.shapes` | `at` and `radius` (3), or `lo`, `hi`; `what`, `max` (100) | the collision shapes there: `count`, `kinds` (counts), `shapes` (`kind`, `corners` and `normal`, or `center`, `half`, `axes`, `radius`; `surface` or `actor`, `classes`) |
| `exhaust.state` | `car` (default `"car"`: the one the player drives, else drove last) | the exhaust hook (`hooked`, `hook` (a mod's id or `none`), `calls`, `skipped`, `faults`) and the car's exhaust: `type`, `emitting`, `jet_speed`, `max_speed`, `pipes` (`pos`, `dir`, `carry`), `add_birth`, `add_death`, `death_share` |
| `weather.state` | | the mission scene's weather (LS3D's): `on`, `sectors`, `dummies`, `color_high`, `color_low` (0xAARRGGBB), `speed`, `length`, `width`, `max_dist`, `max_height`, `max_count`, `dir_x`, `dir_y`, `dir_z`, `mode` |
| `weather.set` | any of those names with a value (a colour also as `"0x..."`) | set through LS3D's own setter, as the scripts' `weather_setparam` (`on` and `max_count` applied again for the detail setting); the weather after |
| `weather.reset` | | the scene's weather back to its preset; the weather after |
| `particles.state` | `patch` (true: the particles' vtables patched now, to count without a mod) | the particle hook: `patched`, `hook` (a mod's id or `none`), `moves_pooled`, `moves_emitted` (the game's moves seen), `moved_by_hook`, `lives_started_or_ended`, `tracked`, `faults`, `sample` (a few tracked particles: `frame`, `pos`, `ms_ago`) |
| `particles.defs` | `index` | `tables\particles.def`: `count`, `records` (each `index`, `model`, `life_ms`, `interval_ms`; with `index` the whole record: `growth`, `birth_cube`, `dir_jitter`, `turn_ms`, `fade`, `spin`, `scale`, `tint_birth`, `tint_death`) |
| `particles.def.set` | `index` and any of `interval_ms` [ms, random], `life_ms`, `growth`, `turn_ms`, `fade`, `spin` (pairs), `birth_cube`, `dir_jitter`, `scale`, `tint_birth`, `tint_death` ([r, g, b]) | the record written; the game's emitters take it when they are made (from the next mission's load) |
| `props.list` | | `known`, the props: `prop`, `model`, `by` (a mod or `bridge`), `on`, `parent` (`world` or a frame), `parent_name`, `name`, `pos`, `axes` (the world's), `anim` (`file`, `playing`, `loop`, `ms`, `length_ms`, `weight`), `physics` (`actor`, `mass`; `pending` while it is made again) |
| `prop.create` | `model` (`"2tommy.i3d"`...), `parent` (`"world"` default, an actor: `"player"`, `"car"`, `"0x..."`) and `bone` (a frame's name in its model), or `frame` (any frame, another prop); `pos` (the parent's space; in the world without it: `at` [x, y, z] or an actor, `ahead` (2), `up` (1)), `rot` [w, x, y, z] or `dir` [x, y, z] (where its z axis points), `scale` | a prop made by the game's model cache: its entry as in `props.list` |
| `prop.place` | `prop`, `pos`, `rot` or `dir`, `scale` | moved in its parent's space |
| `prop.attach` | `prop`, a parent as `prop.create`'s (`"world"`: none), `keep` (default true: it stays where it is in the world; false: then `pos`, `rot`, `scale` in the new parent) | given another parent |
| `prop.show` | `prop`, `on` | shown or hidden (the frame's SetOn) |
| `prop.remove` | `prop` or `all` | taken out of the scene and released |
| `prop.anim` | `prop`, `play` (an animation of anims\: `"walk1"`), `loop`, `weight`; or `stop: true` | played on the prop's model as the scripts' `model_playanim` (its frames move the model's frames of the same names); its entry |
| `prop.physics` | `prop`, `on` (default true), `mass` (kg, default 10), `velocity` [x, y, z] | a prop in the world made a physical object as the scripts' `create_physicalobject` (a box from its bounds: it falls, is struck, stood on, shot); `on: false` takes it out; its entry |
| `prop.push` | `prop`, `impulse` [x, y, z] (kg m/s), `at` (a point of the world; default its centre) | an impulse to a physical prop; its entry |
| `files.game` | `match` (a pattern: `"models\*.4ds"`, `"*.5ds"`; `*` and `?`, without regard to case; default every one), `limit` (500) | every file the game can open by that name: `count`, `files` (`name`, `from`: an archive, `folder` (the game's), or a mod's id), `from` (how many from each), `scanned` |
| `model.file` | `frame`, `prop` or `actor` (default `"player"`) | the file the model above it was opened by (LS3D keeps it: `"Models\Tommy.i3d"`), `4ds` (the file's name) |
| `shader.add` | `source` (HLSL: `#include "mml/game.hlsli"`), `vs`, `ps` (its entry points; one may be left out: that stage stays the game's), `pattern` (the textures' names, default `"*"`), `kinds` (`object`, `lightmapped`, `billboard`), `classes` (`opaque`, `alphatest`, `translucent`, `additive`), `where` (`frame`, `views`), `all_passes` | the game's draws it picks drawn with these shaders (vs_3_0, ps_3_0): `handle`; a compiler's error says where |
| `shader.remove` | `handle` or `all` | the bridge's rule taken away: `rules` left |
| `shader.state` | | every rule: `owner`, `handle`, `pattern`, `kinds`, `classes`, `vs` and `ps` (bytes), `draws_last_frame`, `error`; `faults`; `shadows` (as `shader.shadows`) |
| `shader.shadows` | `on` (default true), `size` (2048), `extent` (60 m), `bias` (metres, 0.05) | the shadow map from the sun the rules sample (the bridge's wish): `wanted`, `drawn`, `size`, `extent`, `bias`, `casters`, `triangles`, `pass_ms`, `meshes_uploaded`, `lights` (the scene's on), `sun`, `why` |
| `shader.survey` | `id` (none: a survey of the next frame starts) | the device states the game draws with, by combination: `kind`, `pass`, `fvf`, `lighting`, `fog`, the alpha test, `blend` (`src`, `dst`), `stages` (each `color`, `alpha`, `texcoord`, `texture`), `lights`, with `draws`, `faces` and a few `textures` |
| `scene.focus` | `lo` and `hi` [x, y, z], or `at` (a point or an actor, default `"player"`) and `radius` (20); `off: true` | the bridge's box: the scene's meshes around it scanned (every 0.25 s) |
| `scene.pieces` | `limit` (20) | the pieces of the last scan in the bridge's box: `pieces`, `triangles`, `list` (`visual`, `triangles`, `vertices`, `at`, `moving`, `version`), the walk's and the scan's numbers (`walked_frames`, `walk_ms`, `scan_ms`, `left_for_later`, `hidden`, `of_actors`, `unreadable`, `out_of_place`, `meshes_cached`) |
| `actor.meshes` | `actor` (default `"car"`) | its visuals' meshes now: `pieces`, `triangles`, `list` (`visual`, `triangles`, `lo`, `hi`: the world's box of its posed vertices) |
| `model.info` | `file` (`"Tommy"`, `"models\Tommy.4ds"`), `meshes` (default true) | a .4ds read and parsed: `nodes` (`index`, `name`, `type`, `parent`, `visual`, `instance_of`, `lod0` vertices and triangles, `lods`), `materials` (`flags`, `texture`, `opacity_texture`) |
| `sounds.list` | | `known`, `music_known`, `sounds` (`sound`, `file`, `by`, `playing`, `looped`, `in_world`, `ms`, `length_ms`, `volume`, `pitch`, `held`, `pos`, `follows`, `frame`: its I3D_sound), `music` (`music`, `file`, `by`, `volume`, `looped`, `pos`), `music_volume_option`, `city_music_paused` |
| `sound.play` | `file` (`"sounds\\menuopen.wav"`, `"x.wav"`: under `sounds\`), where: none (2D), `at` [x, y, z], or `on` an actor (`"player"`, `"car"`, `"0x..."`) and `bone`, or `frame`, with `pos` in its space; `volume` (0..1), `pitch` (a factor), `range` (far, or [near, far]), `loop`, `pausable` | the sound as in `sounds.list` (`sound`: its id) |
| `sound.set` | `sound`, `volume`, `pitch`; a new place as `sound.play`'s (`at`, `on`, `bone`, `frame`, `pos`) | the sound after |
| `sound.stop` | `sound` or `all` | stopped and released |
| `music.play` | `file` (an .ogg or .mp3; `"x.ogg"`: under `sounds\music\`), `volume`, `loop` | `music` (its id), `file` |
| `music.set` | `music`, `volume` | the volume (times the options' music volume) |
| `music.stop` | `music` or `all` | stopped and released |
| `music.game` | `on` (false: the game's city music faded out and kept off) | `city_music` |
| `player.lock` | `on` (default true) | the player's own control locked (the scripts' PLAYER_LOCKCONTROLS) or given back: `locked` |
| `vehicle.body` | `car` (`"car"` default, `"0x..."`), and to write: `pos`, `velocity`, `turn` ([x, y, z]) | the car's body: `pos`, `velocity`, `turn`, `forward`, `right`, `up` |
| `interact.list` | | `items` (each `id`, `kind` zone, use or timer, `by`; a zone's and a use-point's `at`, `radius`, `inside`, `label`, `text_id`; a timer's `ms`, `left`, `repeat`, `fired`), `player`, `on_foot`, `using_objects` (the hooks), `action_menus`, `picked` |
| `zone.add` | `at` [x, y, z] (default: where the player is), or `on` an actor and `bone`, `pos`; `radius` (3); `who` (`player` default, `people`, `cars`, `all`, or an actor) | `id`, `at`, `who`: the crossings go to `events.since` (ZONE_ENTERED, ZONE_LEFT, with the `actor`) |
| `use.add` | as `zone.add`, `radius` (2), `label` (its line in the game's Action choice menu) | `id`, `at`: the player's taking it goes to `events.since` (USED) |
| `timer.add` | `ms`, `repeat` | `id`: TIMER in `events.since` after ms of the mission's time |
| `interact.remove` | `id` or `all` (the bridge's) | removed |
| `hud.console` | `text`, `color` (its RGB) | a line of the game's console (five lines, 5 s each, 63 characters): `shown` |
| `hud.help` | `text` | a line of the game's help (the mission's hints, above the crosshair): `ms` it is shown |
| `hud.marker` | `at` [x, y, z], or `on` an actor and `bone`, `pos`; `label`, `color`, `picture` (a file of `maps\`, `alpha` its twin), `scale` (24), `screen`, `distance`, `edge` (default true), `radar`, `compass` (default false); `id`: one of the bridge's changed | `id`, `at`, `flags`: drawn where it is on the screen (at its edge when off it) with its distance from the player; a mark on the game's radar; the compass' target |
| `hud.remove` | `id` or `all` (the bridge's) | removed |
| `save.state` | | `hooked`, the `profile` and `slot` selected, `slot_loaded`, `slot_keys` and `profile_keys` (each `mod`, `key`, `size`), `saves_written`, `saves_loaded`, `new_games`, `last_file`, `profile_file`, `last_error` |
| `save.get` | `mod` (default the bridge's), `key`, `scope` (`"slot"` default, `"profile"`) | `value` (text, or `{"hex"}`), `size` |
| `save.put` | `key`, `value` (a string, or JSON kept as its text; empty: removed), `scope` | the bridge's own key put: written with the next save (`slot`) or at once (`profile`) |
| `hud.list` | | `markers` (each `id`, `by`, `label`, `flags`, `at`, `radar`, `compass`), `pictures` (`id`, `by`, `file`, `alpha`, `w`, `h`), `compass_by`, `hud_flags`, `compass_target` |
| `control.state` | | `lock_known`, `locked`, `locked_by`, `vehicle_hook` (a mod or `none`), `engine` (the step's hook), `steps`, `skipped` (steps a mod made), `faults` |
| `lights.list` | `type` (`point`, `spot`, `directional`, `ambient`, `fog`), `all` (the lights that are off too), `max` (100), `rescan` | the scene's light frames: `count`, `kinds`, `lights` (`light`, `name`, `type`, `on`, `color`, `power`, `range`, `cone`, `pos`, `dir`, `sectors`), `sun` (`to_sun`, `color`, `ambient`) |
| `light.set` | `light` (from `lights.list`), `color`, `power`, `range` ([near, far]), `on` | the light changed through LS3D's setters; the light after |
| `state.money` | `set` or `add` | the freeride's money (`money`, null with no game; `shown`: the HUD shows it), set through the game's `C_game::SetScore` |
| `human.health` | `actor` (default `"player"`), `set` | `health`, `max`; `set` above 0, at most `max` (the player's HUD number told) |
| `human.items` | `actor` | `items`: `slot` (0 the hand, 1..5 the belt, 6 the coat, 7..10 extra, 11 put away in a car), `where`, `id`, `name`, `loaded`, `reserve` |
| `human.ammo` | `actor`, `slot` (0), `loaded`, `reserve` (-1 or absent: as it is) | the rounds of the item in that slot written; `items` after |
| `human.take` | `actor`, `item` (an id or a name) | the item taken (the scripts' `human_delweapon`); `items` after |
| `camera.state` | | the frame's camera: `override` (a mod's id, `the bridge (camera.set)`, `none`), `frames_overridden`, `declined` (the mod's function returned 0), `skipped`, `last` (drawn, or why not), `frame_camera` (`eye`, `fwd` of the last frame drawn) |
| `camera.set` | `eye`, `at` or `dir`, `up` (default `[0, 1, 0]`), `fov` (vertical degrees; default the game's), `ms` (default 10000) | the frame drawn from that camera from the next frame for `ms` (over a mod's `camera_override` while held); only the drawing |
| `camera.release` | | the bridge's camera given back |
| `watch.set` | `addr`, `mode` (`write`: changes only; `access`: reads too), `len` (1, 2, 4; aligned) | a hardware watchpoint on the game thread (four at most): `id` |
| `watch.show` | `id` | `hits`, `sites` (each piece of code: `at`, `count`, `stack` (the game's return addresses), `before`, `after`), `value` |
| `watch.clear` | `id` or `all` | `cleared` |
| `native.call` | `at` (an address: `"game+0x..."`, `"ls3d+..."`, `"0x..."`), `conv` (`stdcall` default, `cdecl`, `thiscall`, `fastcall`), `this` (thiscall's ECX), `args` (numbers, addresses, `{"float"}`, `{"double"}`, `{"str"}`, `{"buf": n, "hex"}`), `ret` (`int` default, `uint`, `ptr`, `float`, `double`, `int64`, `str`, `void`) | any function called on the game thread behind a fault guard: `ret`, `eax`, `edx`, `st0` (what it left on the x87 stack, always popped), `buffers` (each `buf` after the call: `hex`, `words`, `floats`), `stack` (`pushed`, `popped_by_callee`, `expected`, `ok`: a wrong convention or count shows here; ESP is put back either way), `ms`; a fault: an error with where |
| `mod.reload` / `mod.load` / `mod.unload` | `id` (`"*"`: every loaded mod, for reload); reload: `times` (1..200) | the mod; with `times`: `rounds` (the process after each load) and `per_reload` (what one reload did not give back) |
| `commands.list` | | every method: the host's with `thread`, `params` and `result` (these tables, through `host/methods.inc`: `python tools/gen_methods.py` after a change here), the mods' commands with their `help` |
| `hooks.list` | | every hooked function and its chain |
| `menu.state` | | the open menus (the active one first: vtable, kind, command handler, its definition's `name`, its `controls`: id, kind, text, x, y, w, h in 800x600 units inside the parent, `screen` [x, y, w, h] on the screen (where to click), flags, parent, focused, `value` (sliders, bars, checks, switches, selectors, numbers, scrolled lists), `item_of` / `page_of` a mod's) and the profiles the game has read |
| `menu.command` | `command` | the command sent to the active menu as a click sends it (1.2: its vtable's `+0x24`; 1.0: the screen's next tick answers it as the control activated, a frame later); the new `menu.state` (on 1.0 still the one before) |
| `menu.value` | `id`, `value` (optional) | a component of the active menu by its id: its value set (when given) and read, by the menu's own setter and getter (a slider or a progress bar: 0..1; a check, a switch, a number, a selector: read where the control keeps it; the freeride menu's 21 traffic, 22 pedestrians, 23 police) |
| `menu.item` | `menu` (a definition's name: `"gamemenu"` the pause menu), `after` (a control's id), `text` | an item of the bridge's own, added under that control whenever the menu is made (from its next opening), as a mod's `menu_item` is: `handle`; its clicks are logged and listed by `menu.items` |
| `menu.item.remove` | `handle` | one of the bridge's items forgotten (gone at the menu's next opening) |
| `menu.items` | | `known` (the menus' functions are known for this build), the items asked for (mods' and the bridge's), the pages, the bridge's last clicks |
| `menu.page` | `name`, `controls`: `[{id, kind, x, y, w, h, text, flags, value, range}]` (`"wind"` first; a container (`wins`, `wind`) holds what follows until `"end"`; units 800x600, from the paper's corner; `text` is the options of `bslb` / `bsel` (`"Off\|On"`), the format of `iten` (`"Laps: %u"`), the image file of `rctt`; `range` [min, max] of `iten`) | the bridge's page put over the open menu (with none open, the pause menu is opened first), made by the game from these descriptions as mods' `menu_open` pages are; Esc or `menu.page.close` closes it. The kinds: docs/MODDING.md |
| `menu.page.set` | `id`, `text`, `value`, `range` (each optional) | a control of the bridge's page changed; `now` its value, `now_text` its text (a text field's: what was typed) |
| `menu.page.close` | | the bridge's page closed |
| `fpu.state` | | the x87's precision control on the game thread: `game` (the word as the game entered the loader's frame: `word`, `bits` 24, 53 or 64), `frame_exit` (as the frame gave it back), `bridge_call` (now, in this call), `mods` (as a mod's callback sees it), `scopes` (calls into mods so far), `raised` (those that needed 53 bits set) |
| `panel.state` | | the mods' settings in Options > Mods (their manifests' `settings`): `item` (the Mods row's handle), `mods` (each `mod`, `title`, `controls`: `key`, `label`, `kind`, `value`, `shown`, a slider's `min`, `max`, `step`), `page` (the mod whose page was opened last) |
| `panel.open` | `mod` (none: the list of mods) | that page put over the open menu (the pause menu first when none is), as Options > Mods opens it: `opened` |
| `menu.def` | `name` (`"gamemenu"`, `"mainmenu"`, `"optionsa"`, `"loadgame"`, `"profilecreate"`...); 1.0 also `design` | one of the game's own definitions (`tables\menu\<name>.mnu`, the language's first) read through its file layer: `file`, `header`, `controls` [{id, kind, x, y, w, h, flags, text_id, text, depth}]: how the game builds its pages, to copy. 1.0: the screen's group of `tables\menu.def` (`design`, `controls` [{id, kind, x, y, w, h, flags, colour, text_id, text}]) |
| `menu.click` | `id`, `menu` (optional: another open menu's name) | that control clicked in its middle by the injected mouse (moved there, the left button 80 ms later for 100 ms): the game's own hit test, focus and click |
| `script.run` | `code` (a program in the missions' language, docs/SCRIPTS.md), `act` {index: `"player"`, `"car"` or an actor's address}, `flt` {index: number}, `owner` (an actor, as in `act`), `timeout_ms` (default 30000) | compiled by the game's own compiler (a line it does not take: an error naming the line), then run by the game's interpreter from the next mission tick: `id`, `instructions` |
| `script.state` | `id` | `state` (`running`, `done`, `failed`, `stopped`), `error`, `line`, `ticks`, `ms`, the registers: `flt` (up to the last that is not 0), `act` [{i, actor, type, model}], `frm` [{i, frame, name}]; the last 32 finished programs are kept |
| `script.stop` | `id` or `"all"` | `stopped`: how many |
| `script.list` | | `known` (the machine's functions are known for this build), the programs: id, owner, state |
| `world.spawn` | `car` (a name of the game's car list, a part of one, or its number) and `color` (its file: `<prefix>00.i3d`, `01`...), or `human` (a model: `"Paulie"`), or `model` with `kind` (`"car"`, `"human"`); `at` ([x, y, z], or an actor: `"player"` the default, `"car"`, an address), `ahead` (metres in front of the `at` actor: 8 for a car, 2.5 for a person), `dir` [x, y, z], `ground` (default true: put on the ground under the place), `in` (a car: the person put into it at once) and `seat` (0 the driver's, 1 beside him: the default, 2, 3) | a new actor made by the game's own functions (docs/SCRIPTS.md, "The world without a program"): `actor`, `type`, `name` (its frame's), `model`, `pos`, `facing` |
| `world.remove` | `actor` | an actor `world.spawn` (or a mod's `actor_spawn`) made, taken out of the world (the game deletes it at its next tick; its model goes with it); refused while a person sits in a car (either of them) |
| `world.teleport` | `actor` (default `"player"`; a player in a car moves the car), `to` ([x, y, z], or an actor with `ahead` metres in front of it: 3 by default), `dir`, `repair` (a car: stopped, set on its wheels and repaired; default true) | the scripts' `actor_setpos` run at once: where it is now |
| `world.seat` | `actor` (a person), `car` (default `"car"`), `seat` (default 1) | the scripts' `human_force_settocar`: in the seat at once, no walk, no door |
| `world.give` | `actor` (default `"player"`), `item` (a name: `"thompson 1928"`, `"colt"`, `"grenade"`, or a number), `loaded`, `reserve` (default: a full magazine, the item's usual reserve) | the scripts' `human_addweapon`: a character holds it at once, the player gets it in his inventory |
| `world.cars` | `query` (a part of a name or a model) | the game's car list: `id`, `name`, `model` |
| `world.items` | `query` | the items of `tables\predmety.def`: `id` (what `human_addweapon` takes), `name`, `kind` (`firearm`, `throwable`, `melee`, `item`), `model`, `magazine`, `reserve`, `flags`; a mod's item its `mod` ("mod/name") |
| `items.define` | `name` (the bridge's own), `label`, `model`, `like` (the game's item copied, default 16 a key), `flags` (-1: like's), `collected` | `id` (35 and up, kept for the name in `mml\items.json`), `key`, `name` |
| `stats.state` | | what the player did, counted by the loader: `game` (the game being played, kept in its saves) and `profile` (the profile's totals), each `kills`, `deaths`, `shots`, `hits`, `cars_driven`, `crashes`, `falls`, `items`, `on_foot_m`, `in_vehicle_m`, `played_s`, `money_earned`, `money_spent`; `profile_number` |
| `profile.progress` | | the profile's own progress: `profile`, `name`, `chapter` (its newest story save's), `newest_slot`, `saves_count`, `saves` (each `slot`, `name`), `freeride_level`, `race_cars`, `steal_cars`, `freeride_cars` (the car groups' letters), `garage` (each `index`, `car`) |
| `items.state` | | `known`, `game_items`, `count`, `installed` (the table moved to the loader's), `room`, `file`, `items` (each `id`, `key`, `defined` this run; `label`, `like`, `model`, `flags`) |
| `world.spawned` | | `known`, the actors `world.spawn` and mods made that are still there: actor, by (the mod, or `bridge`), model, pos |
| `start.state` | | the start asked by `<game>\mml\start.json`: `waiting`, `profile`, `main`, `freeride`, `place`, `done`, `failed` (+ `error`; `kind`; `note`: a kind the profile had not opened) |
| `input.actions` | | every action slot: its name, primary or secondary binding, the device and key it is bound to, its values now |
| `input.act` | `action` (a name or a slot number), `ms` or `frames` (default 1 frame), `value` (default 1), `after_ms` or `after_frames`; or `turn` (degrees, to the right when positive), `frames` | the action held: pressed whatever it is bound to. A mouse axis (`"Aim X"`, `"Aim Y"`) takes small values (see below); `turn` turns the player on foot by exactly that angle |
| `input.key` | `key` (`"Esc"`, `"W"`, `"F1"`, `"Up"`... or a DirectInput code), `ms` or `frames`, `after_ms` or `after_frames` | the key held in LS3D's keyboard state: for menus, for mods' keys and hotkeys, and whatever is not an action |
| `input.mouse` | `mouse` [x, y] (800x600 units), or `button` (`"left"`, `"right"`, `"middle"`) with `ms` or `frames`, or `wheel` (notches, up positive); `after_ms` or `after_frames` | the menus' mouse, injected after LS3D's read of the mouse: a move puts the menus' cursor exactly there (only while a menu is open; it waits up to 2 s for one), a button is held, the wheel turned (the game gives the wheel to the focused control; its lists do not scroll by it) |
| `input.text` | `text` (up to 200 characters of the US layout), `every_ms` (default 120), `ms` (each key held, default 60) | each character typed as a key press, with Shift where it needs it: the menus' text fields take what LS3D makes of the keys |
| `input.seq` | `steps`: up to 256 of the above (each with `action`, `key`, `turn`, `mouse`, `button` or `wheel`) | all of them started on one clock (their `after_ms`), or none when one is wrong |
| `input.release` | `what`: `"all"` (default), an action's or a key's name | holds dropped |
| `input.state` | | the hooks, the holds running and waiting, the keys down at the last read, the mods' hotkeys, `captures` |
| `input.capture` | `key` (a name or code), or `action` (a name or slot: the action, its primary and secondary bindings both), `on` (default true); or `all` with `on` false | kept from the game (it reads the key up, the slot 0) while mods and `input.state` still see them: `captures` |
| `text.get` / `text.find` | `id` / `text`, `from`, `to` | the game's localised texts (its text database), by id or by a part of the text |
| `screenshot` | | asks for the next frame the game presents: `{id}` |
| `screenshot.state` | `id` | `done`, `ok` or `error`, `path` (`<game>\mml\shots\<id>.bgra`: 16 bytes `"MMLS"`, width, height, 0; then BGRA rows, top first), `width`, `height` |
| `draw.text` | `x`, `y`, `text`, `size` (default 20), `color` (`"#AARRGGBB"`, `"#RRGGBB"` or a number; default white), `font` (0 small, 1 big, 2 title), `align` (`"left"`, `"center"`, `"right"`), `ms` (default 3000) | a text held in the game's frame for that long: `held`, `hooked` (false: no known build, nothing is drawn) |
| `draw.rect` / `draw.line` | `x`, `y`, `w`, `h`, `filled` (default true) / `x0`, `y0`, `x1`, `y1`; `color`, `ms` | a rectangle (or its outline) / a line, held the same way |
| `draw.picture` | `file` (a picture of the game's `maps\`: the archives' or a mod's `files\Maps\`; a .tga with its alpha, a .bmp), `alpha` (a .bmp's `+` twin), `x`, `y`, `w`, `h` (default its pixels), `angle` (degrees clockwise), `color` (a tint; default white), `uv` [u0, v0, u1, v1], `ms` | a picture opened by the game's own texture loader, held the same way |
| `draw.clear` / `draw.state` | | the held items dropped / the 2D pass hooked or not, passes seen, items queued by mods last frame and drawn in the last pass, items held, mods whose drawing faulted (`off`), the last fault |
| `config.get` | `mod`, `key` (a path: `"overlay.x"`; none: the whole config) | `value` (the defaults with the changes over them), `default`, `known`, `changes` (what differs from the defaults), `files` |
| `config.set` | `mod`, `key`, `value` (any JSON; `null`: back to the default) | the same; the change is written to `<game>\mml\data\<mod>\config.json` and the mod gets `MML_EVENT_CONFIG_CHANGED` at the next frame |
| `files.list` | `mod` (only its), `name` (a part of the names) | the mods with files (`id`, `load_order`, `files`, `added`: by `file_add`, `folder`, `watched`) and each name a mod answers (`name`, `mod`, `path`, `size`, `opens`: how often the game opened it, `over`: the mods under it, `added`), at most 1000; `presaves_hidden`: the game's presaved textures not given for a mod's picture, how often |
| `files.which` | `name` (the game's name of a file: `"maps\\crate.bmp"`) | who answers it: `mod`, `path`, `over` (the mods under it), `game_has`, `game_size` (its folder's or archives' own), `hidden_for` (a presaved texture of a mod's picture, not given), `answer` |
| `files.read` | `name`, `game` (the game's own under a mod's), `offset`, `size` (default 256, at most 65536) | `source` (`mod`, `game`), `size` (the whole file's), `hex`, `text` (when it is text) |
| `render.state` | | the frame's drawing: `hooked`, `renders` (frames' scenes drawn), `world_renders`, `stage1` (RENDER_WORLD points), `presents`, `last_scene`, `last_scene_world`, `device8`, `direct3d9` (a wrapper's Direct3D 9 interface), `camera` (`eye`, `fwd`, `fov_y_deg`, `near`, `far`, `viewport`) |
| `prof.state` | | the frame: `frame` (`avg_ms`, `median_ms`, `p95_ms`, `max_ms`, `last_ms`, `fps`, the last 600 frames), `mods` (each: `ms_per_frame`, `sites` with `ms_per_frame`, `worst_frame_ms`, `calls_per_frame`; `host`: the scene views, the graphics bridge), `hitches` (`ms`, `median_ms`, `parts`), `sampling` |
| `prof.reset` | | the measures start over |
| `prof.sample` | `seconds` or `id` | `seconds` (0.5..30, default 3): the game thread sampled every 0.5 ms (`id`); `id`: `samples`, `phases`, `by_module`, `by_mod`, `spots` (`at`, `percent`, `phase`), `callers` (the game's code that called Direct3D or the driver) |
| `gfx.state` | | the graphics bridge: `state` (on, off and why), `adapter`, `size`, `frames`, `generation`, `passes` (each: `mod`, `order`, `cpu_ms`, `gpu_ms`, `in_views`), `ms` (`copy_9`, `passes_11`, `wait_11`, `all`, `gpu_passes`), `multisampling` (off for which mods), `ui` (`mode`, `frames`: the 2D pass taken), `views` (each scene view's targets: `handle`, `size`, `frames`, `fresh`, `pass_runs`, `pass_fails`), `view_sky_sets` (the sky's view seen while a clipped view drew), `world` (`eye`, `fog`, `sun`) |
| `gfx.reset` | | after a failure: try again at the next frame |
| `view.state` | | the scene views (mods' `scene_view`): `hooked`, `views` (each: `mod`, `handle`, `size` (0, 0: the frame's), `drawn`, `skipped`, `faults`, `last` (drawn, or why not), `cpu_ms`, `eye`, `fwd`), `note` (why none is drawn) |
| `geom.hide` | `pattern`, `where` (`frame`, `views`, `all`) | the game's face groups whose texture's file name matches (`*`, `?`, case-insensitive) left out of LS3D's drawing: `handle` |
| `geom.show` | `handle` | a `geom.hide` taken back |
| `geom.state` | | `hooked`, `rules` (each: `owner`, `pattern`, `where`, `groups_last_frame`, `faces_last_frame`), `capture_hooked`, `faults`, `capture_fails` |
| `geom.survey` | `id`, `filter`, `limit` | none: the next frame is surveyed (`id`); with `id`: every texture it drew (`texture`, `groups`, `faces`, `visuals`, `material_flags`, `hidden`), the most faces first |
| `geom.capture` | `pattern` or `id` | `pattern`: the next frame's matching face groups are read back (`id`); `id`: `groups`, `triangles`, `bounds`, `by_texture`, `first` (a few triangles, the world's space), `unreadable` |
| `view.show` | `handle`, `rect` | a view copied onto the frame at `[x, y, w, h]` (800x600 units) each frame; no `rect`: not any more |
| `<command>` | the command's | a mod's command: the core mod's without a prefix (`player`), others' as `<mod>.<name>` |

The core mod's commands: `game` (the build, the mission's name and state), `player`, `car` (the car the player
drives now, else drove last; or `car`: an address): position, velocity, speed (m/s, km/h, mph), steering, gear, rpm,
fuel, engine health and damage; `state_bytes` (its saved state's size: it grows with the
body's dents); `body` (the rigid body as the physics keeps it: `awake`, `calm`, `angular_velocity` rad/s, `forward`,
`right`, `up`, `step_s`, `substep_max_s`) and `wheels` (each: its frame's name, `on_ground`, `load_n`, `force_n`,
`compression_m`, `free_length_m`, `stiffness`, `flags`); `car.save` / `car.restore` (the car's whole state kept in memory, across the core's reloads, and put
back into it or another car of its model: repaired, then the game's own `SaveVehicleState` / `LoadVehicleState`), `humans` (`radius`), `actors`, `frame` (`addr`),
`frames.find` (`name`, `limit`).

## Drawing: the coordinates

`draw.*` and the mods' `draw_*` (mml.h) use the menus' virtual units: the screen is 800 x 600 whatever the
resolution, (0, 0) the top left corner; a text's `size` is its line height in these units (the HUD's money: 40).
The items are drawn by the game's own 2D drawing at the end of its 2D pass, over the HUD and the menus, and are in
the frame `screenshot` returns. Nothing is drawn while the game loads.

## Input: the actions and their values

The actions' names (matched without case, spaces or punctuation; `input.actions` lists them with their slots). On
foot: `Forwards`, `Backwards`, `Left`, `Right` (steps aside), `Walk/Run`, `Action`, `Fire`, `Crouch`, `Jump`,
`Aim X`, `Aim Y`, `Cycle Up`, `Cycle Down`, `Inventory`, `Hide Weapon`, `Drop Weapon`, `Toggle Run/Walk`, `Reload`,
`Sniper Mode`, `Objectives`, `City Map`. Driving: `Accelerate`, `Brake`, `Steer Left`, `Steer Right`, `Handbrake`,
`Speed Limiter`, `Gear Up`, `Gear Down`, `Horn`, `Look Left`, `Look Right`, `Clutch`, `Manual/Auto Gearing`,
`Change Camera`, `Replace Car`.

A button's value is 1: "held" for as long as asked, "pressed" in its first frame (what the game reads for things
done once: `Action`, `Jump`, `Reload`). A mouse axis' value is what the game makes of the mouse in a frame, its
counts / 400: right and down are positive. On foot the player turns by `sensitivity * 5 * value` radians a frame
(the options' mouse sensitivity), so values are small: 0.01 a frame is about 1.4 degrees at sensitivity 0.49.
`turn` does this sum for an angle (at most 5 degrees a frame) and is exact; in a car and in the free look the
camera has its own code, not measured yet.

`<game>\mml\start.json` (a start, read once at the game's launch): `{"skip_logos": true, "profile": 4 | "name",
"go": "menu" | "freeride" | "load" | "new" | "extreme", "kind": "city-small" | "city-night" | "countryside-night" |
"city" | "countryside", "place": 1.. | "Hoboken", "save": 465 | "Old Prison", "traffic": 0..1, "pedestrians": 0..1,
"police": 0..1, "background": true}`
(`background`: the game never activates its own window; `game_launch` asks for it by default).
