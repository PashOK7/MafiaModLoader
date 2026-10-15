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
| `mod.reload` / `mod.load` / `mod.unload` | `id` (`"*"`: every loaded mod, for reload); reload: `times` (1..200) | the mod; with `times`: `rounds` (the process after each load) and `per_reload` (what one reload did not give back) |
| `commands.list` | | every method, the mods' with their help |
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
| `world.items` | `query` | the items of `tables\predmety.def`: `id` (what `human_addweapon` takes), `name`, `kind` (`firearm`, `throwable`, `melee`, `item`), `model`, `magazine`, `reserve`, `flags` |
| `world.spawned` | | `known`, the actors `world.spawn` and mods made that are still there: actor, by (the mod, or `bridge`), model, pos |
| `start.state` | | the start asked by `<game>\mml\start.json`: `waiting`, `profile`, `main`, `freeride`, `place`, `done`, `failed` (+ `error`; `kind`; `note`: a kind the profile had not opened) |
| `input.actions` | | every action slot: its name, primary or secondary binding, the device and key it is bound to, its values now |
| `input.act` | `action` (a name or a slot number), `ms` or `frames` (default 1 frame), `value` (default 1), `after_ms` or `after_frames`; or `turn` (degrees, to the right when positive), `frames` | the action held: pressed whatever it is bound to. A mouse axis (`"Aim X"`, `"Aim Y"`) takes small values (see below); `turn` turns the player on foot by exactly that angle |
| `input.key` | `key` (`"Esc"`, `"W"`, `"F1"`, `"Up"`... or a DirectInput code), `ms` or `frames`, `after_ms` or `after_frames` | the key held in LS3D's keyboard state: for menus, for mods' keys and hotkeys, and whatever is not an action |
| `input.mouse` | `mouse` [x, y] (800x600 units), or `button` (`"left"`, `"right"`, `"middle"`) with `ms` or `frames`, or `wheel` (notches, up positive); `after_ms` or `after_frames` | the menus' mouse, injected after LS3D's read of the mouse: a move puts the menus' cursor exactly there (only while a menu is open; it waits up to 2 s for one), a button is held, the wheel turned (the game gives the wheel to the focused control; its lists do not scroll by it) |
| `input.text` | `text` (up to 200 characters of the US layout), `every_ms` (default 120), `ms` (each key held, default 60) | each character typed as a key press, with Shift where it needs it: the menus' text fields take what LS3D makes of the keys |
| `input.seq` | `steps`: up to 256 of the above (each with `action`, `key`, `turn`, `mouse`, `button` or `wheel`) | all of them started on one clock (their `after_ms`), or none when one is wrong |
| `input.release` | `what`: `"all"` (default), an action's or a key's name | holds dropped |
| `input.state` | | the hooks, the holds running and waiting, the keys down at the last read, the mods' hotkeys |
| `text.get` / `text.find` | `id` / `text`, `from`, `to` | the game's localised texts (its text database), by id or by a part of the text |
| `screenshot` | | asks for the next frame the game presents: `{id}` |
| `screenshot.state` | `id` | `done`, `ok` or `error`, `path` (`<game>\mml\shots\<id>.bgra`: 16 bytes `"MMLS"`, width, height, 0; then BGRA rows, top first), `width`, `height` |
| `draw.text` | `x`, `y`, `text`, `size` (default 20), `color` (`"#AARRGGBB"`, `"#RRGGBB"` or a number; default white), `font` (0 small, 1 big, 2 title), `align` (`"left"`, `"center"`, `"right"`), `ms` (default 3000) | a text held in the game's frame for that long: `held`, `hooked` (false: no known build, nothing is drawn) |
| `draw.rect` / `draw.line` | `x`, `y`, `w`, `h`, `filled` (default true) / `x0`, `y0`, `x1`, `y1`; `color`, `ms` | a rectangle (or its outline) / a line, held the same way |
| `draw.clear` / `draw.state` | | the held items dropped / the 2D pass hooked or not, passes seen, items queued by mods last frame and drawn in the last pass, items held, mods whose drawing faulted (`off`), the last fault |
| `config.get` | `mod`, `key` (a path: `"overlay.x"`; none: the whole config) | `value` (the defaults with the changes over them), `default`, `known`, `changes` (what differs from the defaults), `files` |
| `config.set` | `mod`, `key`, `value` (any JSON; `null`: back to the default) | the same; the change is written to `<game>\mml\data\<mod>\config.json` and the mod gets `MML_EVENT_CONFIG_CHANGED` at the next frame |
| `files.list` | `mod` (only its), `name` (a part of the names) | the mods with files (`id`, `load_order`, `files`, `added`: by `file_add`, `folder`, `watched`) and each name a mod answers (`name`, `mod`, `path`, `size`, `opens`: how often the game opened it, `over`: the mods under it, `added`), at most 1000; `presaves_hidden`: the game's presaved textures not given for a mod's picture, how often |
| `files.which` | `name` (the game's name of a file: `"maps\\crate.bmp"`) | who answers it: `mod`, `path`, `over` (the mods under it), `game_has`, `game_size` (its folder's or archives' own), `hidden_for` (a presaved texture of a mod's picture, not given), `answer` |
| `files.read` | `name`, `game` (the game's own under a mod's), `offset`, `size` (default 256, at most 65536) | `source` (`mod`, `game`), `size` (the whole file's), `hex`, `text` (when it is text) |
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
