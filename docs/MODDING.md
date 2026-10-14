# Writing a mod

The whole way from nothing to a mod running in the game, for an agent (or a person) who has this repository built
(`AGENTS.md`, "The loop") and the MCP server `mafia` connected. A mod here needs no thread, no window, no hook on
Windows and no memory scanner of its own: the host gives it the frame, the keys, the screen, the game's objects and
its settings.

## 1. Make it

```bat
python tools/new_mod.py my_mod --name "My mod"
```

makes `mods/my_mod/` and adds its line to `CMakeLists.txt`:

| File | What |
|---|---|
| `my_mod.cpp` | a small working mod: an overlay line, a hotkey from its settings, a command. Cut what you do not need |
| `mod.json` | the manifest: `id` (its folder, its DLL, its commands' prefix), `name`, `version`, `entry`, `enabled`, `load_order`, `depends` |
| `config.json` | the defaults of its settings (optional) |

A mod is a 32-bit DLL with three exports; `sdk/include/mml/mml.h` is the whole contract (read it once, it is short):

```c
MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }
MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) { ... return 0; }   /* not 0: it is unloaded again */
MML_EXPORT void mml_mod_unload(MmlMod *mod) { ... }
```

`host` is a table of functions; `mod` is this load of the mod (pass it back to the host). Plain C works too
(`mods/examples/hello_c`); C++ mods may include the game's layouts (`mml/game/game.h`).

## 2. Build it, put it into the game, look

```bat
cmake --build --preset x86-release        :: build/x86/out/mml/mods/my_mod/
```

Then over MCP:

| Step | Tool |
|---|---|
| the game, in the freeride, no police | `game_launch {"profile": 4, "go": "freeride", "police": 0}` (once; it stays open) |
| the build into the game, the mod (re)loaded | `deploy {"mods": ["my_mod"]}` |
| is it loaded, what did it log | `mods_list`, `log_tail` |
| its commands | `call {"method": "my_mod.status"}` |
| what it draws | `screenshot` |
| drive the game | `input {"steps": [{"action": "Accelerate", "ms": 3000}], "screenshot": true}`, `{"key": "F9"}` for a hotkey |
| a setting | `call {"method": "config.set", "params": {"mod": "my_mod", "key": "show", "value": false}}` |

Edit, build, `deploy` again: the mod is unloaded and the new DLL loaded while the game runs (a second or two for
the whole turn). Only a change of the loader itself (`host/`, `mml.h`'s host side) needs the game closed
(`game_close`, `deploy`, `game_launch`).

## 3. What the host gives a mod

Everything below is a member of `MmlHost` (`H->...`); `M` is the mod. Calls into a mod happen on the game thread.

### Events (`H->subscribe(M, type, fn, user)`)

| Event | When | Carries |
|---|---|---|
| `MML_EVENT_FRAME` | every frame, in every state (menus, the pause): draw here, read keys here | `dt` |
| `MML_EVENT_MISSION_TICK` | the mission's world moved (not while paused or loading): game logic here | `dt` |
| `MML_EVENT_MISSION_OPEN` | a mission starts loading; the old world has just closed | `name` (`"freeitaly"`, `"mise01"`, `"00menu"` the main menu) |
| `MML_EVENT_MISSION_LOADED` | its world ticks for the first time: the player and the cars exist | `name` |
| `MML_EVENT_MISSION_CLOSE` | the mission's world is about to go: drop every pointer into it | |
| `MML_EVENT_GAME_PAUSED` / `_RESUMED` | a menu stopped the world (the pause menu) / it runs again | |
| `MML_EVENT_ACTOR_DESTROYED`, `MML_EVENT_HUMAN_CREATED`, `MML_EVENT_HUMAN_HIT` | the game's objects | `actor`, `hit` |
| `MML_EVENT_CONFIG_CHANGED` | this mod's settings changed (only this mod is told) | `name` (its id) |
| `MML_EVENT_SERVICES_CHANGED`, `MML_EVENT_MOD_LOADED` | another mod came or went | `name` |

A mod loaded in the middle of a mission (every hot reload) gets no `MISSION_OPEN`: ask `H->mission_name()` and
`H->mission_state()` (`MML_MISSION_NONE`, `_LOADING`, `_RUNNING`, `_PAUSED`) in `mml_mod_load`.

### Drawing in the game's frame

```c
H->draw_rect(M, 560, 240, 220, 60, 0xA0101010, 1);                 /* x, y, w, h, colour, filled */
H->draw_rect(M, 560, 240, 220, 60, 0xFF606060, 0);                 /* its outline */
H->draw_line(M, 566, 262, 774, 262, 0xFF606060);
H->draw_text(M, 670, 266, 30, 0xFFFFFFFF, MML_FONT_BIG, MML_DRAW_CENTER, "00:12.345");
float w = H->draw_text_width(MML_FONT_SMALL, 14, "how wide is this");
```

- **Every frame**: what is queued is drawn in the frame being made and dropped. Queue from `MML_EVENT_FRAME` to be
  seen in the pause and the menus too.
- **Coordinates**: the menus' virtual units, whatever the resolution: the screen is 800 wide and 600 high, (0, 0)
  the top left corner. A text's size is its line height in these units (the HUD's money is 40, a menu's button 30).
- **Colours**: `0xAARRGGBB`. **Fonts** (the game's own): `MML_FONT_SMALL` (plain), `MML_FONT_BIG` (bold, the
  HUD's numbers), `MML_FONT_TITLE` (the menus' handwriting). Text is in the game's encoding (ASCII is safe), about
  100 characters a call. `MML_DRAW_LEFT`, `_CENTER`, `_RIGHT`: what `x` is.
- It is the game's own 2D drawing, at the end of its 2D pass (over the HUD and the menus), so `screenshot` shows
  it. Nothing is drawn while the game loads.
- A fault while your items are drawn turns your drawing off until the mod is reloaded (the log says so); the game
  goes on.
- To try a layout without building anything: `call draw.text {"x": 400, "y": 100, "text": "hello", "size": 30,
  "color": "#FFFFD040", "font": 1, "align": "center", "ms": 10000}` (also `draw.rect`, `draw.line`, `draw.clear`).

### The game's own menus: an item, a page

Not an overlay: a mod's item and page are controls of the game's own menu system, made by the game's own code, in
its paper, fonts, keys, mouse, sounds and focus. An achievements mod, a mod's settings, a lap table: an item in the
pause menu (or the main menu) that opens a page.

```c
static void OnPage(int id, void *user) {                  /* a control was clicked or changed: its id */
    float v;
    if (id == 31 && H->menu_value(M, 31, &v)) { /* the check box: 0 or 1 */ }
    if (id == 400) H->menu_close(M);                       /* Back (Esc closes the page by itself) */
}
static void OnItem(int id, void *user) {
    const MmlMenuControl page[] = {
        {1, "wind", 270, 40, 500, 330, "Achievements:", 0},   /* the page's window: its text is the title */
        {2, "wins", 10, 10, 470, 160, NULL, 0},               /* a scrolled list: what follows, until "end" */
        {100, "rctt", 10, 5, 32, 32, "C:\\...\\mml\\data\\achievements\\first_ride.bmp", 0},   /* a picture */
        {101, "stxt", 50, 12, 300, 18, "First ride", MML_MENU_FONT_BIG},
        {102, "pbar", 50, 30, 300, 8, NULL, 0},               /* a progress bar */
        /* ... as many rows as there are: the list scrolls ... */
        {0, "end", 0, 0, 0, 0, NULL, 0},
        {30, "stxt", 20, 190, 150, 18, "Show hints", 0},
        {31, "chck", 200, 188, 20, 20, NULL, 0},              /* a check box */
        {32, "stxt", 20, 215, 150, 18, "Notifications", 0},
        {33, "bsel", 200, 213, 200, 20, "Off|Quiet|Loud", 0}, /* a selector: its options */
        {400, "butt", 0, 260, 500, 25, "Back", 0},
    };
    if (!H->menu_open(M, "achievements", page, sizeof page / sizeof page[0], OnPage, NULL)) return;
    H->menu_set_value(M, 102, 0.4f);                       /* applied when the game makes the page */
    H->menu_set_value(M, 31, 1);
}
/* in mml_mod_load: under Options (108) in the pause menu, from its next opening */
H->menu_item(M, "gamemenu", 108, "Achievements", OnItem, NULL);
```

- **An item** (`menu_item`) is a copy of a control of that menu (`after_id`) put right under it by the game's own
  function for growing its lists, then moved one row's pitch down: the window grows, what is below moves down, the
  keyboard and the mouse reach it in its place. The menu is named by its definition (`tables\menu\<name>.mnu`):
  `"gamemenu"` is the pause menu (rows 102 Exit Game, 103 Load Game, 108 Options, 111 Resume Game), `"mainmenu"`
  the main menu (buttons 990101..990113; after 990106 Free Ride Extreme the id 990107 is free). `call menu.state`
  shows the name, ids, kinds, places and flags of every open menu. After a reload the mod asks for the same item
  again (from `mml_mod_load`) and the item stays where it is.
- **A page** (`menu_open`) is put over the open menu as the game puts its Options over the pause menu; with no
  menu open the pause menu is opened first. The game builds it from the descriptions as it builds its own pages
  from their files (`call menu.def {"name": "optionsa"}` shows one of the game's own, to copy). One page a mod at a
  time; ids rise in the order the keyboard walks them.
- **The page's shape**: the first control is the page's window; a container (`wins`, or a `wind` inside it) holds
  the controls that follow it until `"end"`, which returns to the page's window (the game's own rule: containers
  do not nest). Units are the menus' 800x600, from the paper's (or the container's) top left corner.
- **The kinds** (each read in the game's code and seen on a page in the lab):

  | kind | what | text | value (`menu_set_value`, `menu_value`) |
  |---|---|---|---|
  | `stxt` | a text | the text | |
  | `item` | a row that is clicked (the pause menu's) | the text | |
  | `butt` | a button | the text | |
  | `line` | a line (`w` its length) | | |
  | `pbar` | a progress bar (the loading screen's) | | 0..1 |
  | `slid` | a slider (dragged, clicked, its arrows) | | 0..1 |
  | `chck` | a check box | | 0 or 1 |
  | `bslb` | a switch between two texts | `"Off\|On"` | 0 or 1 |
  | `bsel` | a selector (a click steps to the next option) | `"Easy\|Normal\|Hard"` | the option's index |
  | `iten` | a number in a text (a click steps it) | a format, `"Laps: %u"` | the number; `menu_set_range` |
  | `edit` | a text field (typed into) | its text | `menu_text` reads what was typed (63 at most) |
  | `rctt` | a picture | an image file (`.bmp`, `.tga`): a full path (the mod's data folder) or relative to the game's folder | |
  | `wins` | a scrolled list: its controls may reach past its height, the game draws the scroll bar | | the scroll, 0..1 |

  A `wins` clips what is drawn after it, so the loader puts it last in the page whatever its place in the list.
  The game has more kinds (`edin`, `sldi`, `rari`, `orez`, `rect`, `list`, `comb`, `grid`, `vidc`: `menu.def` shows
  where its own pages use them); they are made, but their values are not offered yet.
- **Changes call your function**: a click, Enter, a drag, a step calls the page's function with the control's id;
  `menu_value` (or `menu_text`) reads what it is now.
- **Flags** 0 give each kind the look the game's own pages give it (a centred bold button, a grey plain text).
  Otherwise: the font (`MML_MENU_FONT_*`), `MML_MENU_CENTER` / `_RIGHT`, and a colour, which is each kind's own:
  `MML_MENU_TEXT_WHITE` / `_YELLOW` / `_BLACK` for `stxt`, `MML_MENU_BUTTON_RED` / `_WHITE` for `butt`,
  `MML_MENU_ITEM_RED` for `item`.
- `menu_set_text`, `menu_set_value`, `menu_set_range` change the open page (a button's text after a click, a bar as
  a value changes, a picture by its file); called right after `menu_open` they are applied when the game makes the
  page.
- **Try a page without building anything**: `call menu.page {"name": "x", "controls": [{"id": 1, "kind": "wind",
  "x": 270, "y": 100, "w": 400, "h": 200, "text": "Title:"}, {"id": 2, "kind": "chck", "x": 20, "y": 20, "w": 20,
  "h": 20, "value": 1}, ...]}` (a control takes `"value"` and `"range"` too), `menu.page.set {id, text, value,
  range}`, `menu.item {menu, after, text}`; their clicks are listed by `menu.items`.
- **Drive it as a player would**: `call menu.click {"id": 31}` clicks a control by the injected mouse (the game's
  own hit test and focus); the `input` tool takes `{"click": [x, y]}`, `{"mouse": [x, y]}`, `{"wheel": -3}`
  (`menu.state` gives each control's `screen` place); `call input.text {"text": "Tommy"}` types into a text field;
  keys (`Up`, `Down`, `Enter`, `Esc`) walk a menu.
- Both builds. 1.0's menus are other classes (`docs/VERSIONS.md`): the screens have 1.2's names where 1.2 has the
  same screen (`gamemenu`, `mainmenu`... with the same ids), and 1.2's kinds asked for become 1.0's that do the same
  (`slid` a `scrl`, `wins` a `swnd`, `bsel` and `bslb` a drop-down `cmbs`, `edit` an `edtl`, `rctt` an `rctf`); a
  button's or a row's colour flag is not 1.0's (`docs/ROADMAP.md`, Known gaps). `menu.def` and `menu.state` show
  1.0's own.

### The game's script machine: a mission's commands

```c
void OnDone(const MmlScriptResult *r, void *user) {   /* when it ends, on the game thread */
    if (r->state != MML_SCRIPT_DONE) H->log(M, MML_LOG_WARN, "%s (line %d)", r->error, r->line);
}
void *acts[1] = {game::LastCar()};
H->script_run(M, "car_repair 0", NULL, acts, 1, NULL, 0, 2000, OnDone, NULL);   /* the game's own car_repair */
```

- A program in the missions' own language, compiled and run by the game's interpreter, one step a mission tick:
  every command the missions have (teleport, copies of actors, weapons, deaths, cars, weather, traffic, the camera,
  fades). The language, the registers and tested recipes: docs/SCRIPTS.md; each command: `script_commands` over MCP.
- `acts` and `flts` fill `act[]` and `flt[]` before it starts; `owner` is the actor it runs as (where `explosion`
  goes off). A line that does not compile: 0 and the log says which. `fn` gets the state and the registers;
  `script_stop`, `script_state`. Stopped when the mod unloads, the mission closes, or its time is out.
- The telemetry example's F7 (`OnRepairKey`) is a complete use.

### The world: actors made new, moved, armed

```c
MmlVec3 at = {-1985.0f, -4.4f, 15.0f}, dir = {0, 0, -1};
void *car = H->actor_spawn(M, MML_ACTOR_CAR, "Bolt-Thrower", &at, &dir);   /* a name of the car list, or a file */
at.x += 3;
void *paulie = H->actor_spawn(M, MML_ACTOR_HUMAN, "Paulie", &at, NULL);
H->give_item(M, paulie, H->item_id("Thompson 1928"), -1, -1);   /* a full magazine, the usual reserve */
H->actor_seat(M, paulie, car, 0);                               /* at the wheel */
H->actor_teleport(M, game::Player(), &at, NULL);                /* the scripts' actor_setpos */
```

- A car or a person of any of the game's models, made by the game's own functions (as its scripts'
  `actor_duplicate` makes a copy): a real actor of the game, ticked, hit, entered and driven like the mission's own.
  A person is the game's NPC kind with no script of his own: he stands until a program (`script_run`) tells him
  more. Null: the log says why (no such model, not a person's model, no mission).
- `actor_remove` takes out what `actor_spawn` made (yours or anyone's): not while a person sits in a car. Spawned
  actors stay when the mod unloads; the mission's close takes them all (forget your pointers at MISSION_OPEN).
- `actor_teleport`, `give_item`, `actor_seat` are the scripts' `actor_setpos`, `human_addweapon`,
  `human_force_settocar`, run by the game's interpreter at once (not in a later tick, as `script_run` does).
- The names: `call world.cars` and `call world.items` on the bridge list the game's cars and items.
- The telemetry example's F6 (`OnRivalKey`) is a complete use.

### Keys, actions, hotkeys

```c
int f8 = H->key_code("F8");                      /* a DirectInput code; -1: no such name */
if (H->key_down(f8)) ...                         /* held now */
if (H->key_pressed(f8)) ...                      /* went down this frame: ask in one handler, every frame */
int gas = H->action_slot("Accelerate");          /* one of the game's actions (`call input.actions` lists them) */
float throttle = H->action_held(gas);            /* 0..1; an action has two slots: gas (primary), gas + 1 (secondary) */
H->hotkey(M, "Ctrl+F9", OnKey, NULL);            /* OnKey(user) on the game thread when the combination goes down */
```

- Keys and actions are what the game itself read this frame, so the keys and actions agents inject (`input`) are
  seen exactly like real ones: an agent tests your hotkey with `input {"steps": [{"key": "F9"}]}`.
- A hotkey fires on exactly its modifiers (`"F9"` does not fire while Ctrl is held). Calling `hotkey` again with
  the same function and user replaces the combination: do it when your settings change.
- Pick keys the game does not use: `call input.actions` shows what each action is bound to in the profile (in the
  lab's, F5 is the speed limiter and Tab the city map).

### The game's objects (C++: `mml/game/game.h`)

```cpp
#include "mml/game/game_build.h"
#include "mml/game/game.h"
if (!game::Detect(H->module_base("LS3DF.dll"))) return 2;      // once, in mml_mod_load: which build is this
game::Car car(game::PlayerCar());                               // the car the player sits in; game::LastCar(): drove last
if (car) { car.Position(); car.Speed(); car.SpeedKmh(); car.Gear(); car.Rpm(); car.Steering(); car.Fuel(); car.Damage(); }
game::MissionName();  game::Player();  game::ActorWorldPos(actor);  game::ListActors(&all);
std::vector<uint8_t> state;  game::CarStateSave(car.car, &state);  game::CarStateLoad(car.car, state);   // the whole car
```

- Addresses and offsets differ between the game's builds: they are in `game_build.h`, per build
  (`game::Exe().<name>`); never write an address into a mod. A field you need that is not there: find it
  (`kb_search`, `kb_function`, `tap`, `mem_read` in the running game), add it to both tables, run
  `tools/check_builds.py`, write it down in `knowledge/`.
- These are direct reads of the game's memory: valid on the game thread while the mission runs
  (`H->mission_state() >= MML_MISSION_RUNNING`). Game objects die under you (missions close, the city streams):
  ask again every frame, never keep a pointer across `MML_EVENT_MISSION_CLOSE`. For a pointer that may be stale use
  `H->mem_read` (it never faults).
- The core mod answers the same things to agents: `call car`, `player`, `humans`, `game`, `actors`.

### Files and settings

```c
const char *dir = H->mod_data_dir(M);                            /* <game>\mml\data\<id>\ : write here */
double x = H->config_number(M, "overlay.x", 560);                /* a path into config.json; the default if absent */
const char *key = H->config_string(M, "hotkey", "F9");           /* valid until the settings change */
H->config_set(M, "finish", "{\"x\": 1.5, \"z\": -2}");           /* any JSON value; "null": back to the default */
```

- `config.json` next to the DLL holds the **defaults** (shipped with the mod, replaced by every deploy). What was
  **changed** is kept in `<data folder>\config.json` (written by `config_set` and by the bridge's `config.set`, or
  by hand): only the changed keys. The mod reads the two merged.
- Either file changing on disk is noticed within half a second: `MML_EVENT_CONFIG_CHANGED`. Read your settings in
  one function and call it from `mml_mod_load` and from that event.
- Agents change settings with `call config.set {"mod": "<id>", "key": "units", "value": "mph"}` and read them with
  `config.get`: no settings window is needed.
- Never write into the mod's own folder: a deploy replaces it.

### Commands, the log, state, hooks, services

- `H->command(M, "status", "help text", fn, user)`: agents call it as `<id>.status` with JSON params; reply JSON
  text with `H->reply_append`, or `H->reply_error`. Give every mod a `status`: it is how an agent sees inside.
- `H->log(M, MML_LOG_INFO, "lap %.3f", t)`: `<game>\mml\logs\mml.log`, read with `log_tail`.
- `H->state_put` / `state_get`: blobs kept across this mod's own reloads while the game runs (what a hot reload
  must not lose).
- `H->hook(M, address, detour, &original)`: any game function, chained with other mods' hooks; removed when the mod
  unloads. `H->mem_read` / `mem_write` never fault.
- `H->service_publish` / `service_find`: a versioned table of functions other mods call; find it again after
  `MML_EVENT_SERVICES_CHANGED`. `H->post`: run something on the game thread from another thread.

## 4. A complete example

`mods/examples/telemetry/telemetry.cpp`: a speed, gear and lap-time overlay between two checkpoints, a hotkey that
starts a lap, the best lap kept in the data folder, everything adjustable in `config.json`, and an item
"Telemetry" in the pause menu opening a page (the best and last laps, the fuel as the game's progress bar, a check
box and a switch that change settings, Back). It uses each part above once, in 350 lines, and says at its top
where. In the freeride it works as it is:
`input {"steps": [{"action": "Accelerate", "ms": 5000}], "screenshot": true}` drives the taxi through its two
default checkpoints.

## Rules

- Everything runs on the game thread unless said otherwise; do not block it (no sleeping, no waiting on a file).
- A fault in your callback unloads your mod at the next frame (the game goes on); `log_tail` shows where. A fault
  inside a game function you called may leave the game in a bad state: check what you pass.
- Never keep a pointer into another mod's code or data across `MML_EVENT_SERVICES_CHANGED` / `MML_EVENT_MOD_LOADED`.
- `MmlHost` only grows. A mod that must run on an older loader asks `MML_HAS(host, member)` before using a newer
  member; `mml_mod_load` returning non-zero says "not for this loader".
- Everything in the repository is English.

## Not there yet

LuaJIT script mods, a Rust crate, 64-bit sidecar mods: `docs/ROADMAP.md`.
