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
| `mod.json` | the manifest: `id` (its folder, its DLL, its commands' prefix), `name`, `version`, `entry`, `enabled`, `load_order`, `depends`, `gfx` (true: it draws through the graphics bridge: "Effects over the frame"), `settings` (its page in the game's Options > Mods: "Settings in the game's Options"), `builds` (the game's builds it is made for, `["1.2", "1.0"]`: on another it is not loaded and gives the game none of its files, `mods.list` says why; absent: every build) |
| `config.json` | the defaults of its settings (optional) |

A mod is a 32-bit DLL with three exports; `sdk/include/mml/mml.h` is the whole contract (read it once, it is short):

```c
MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }
MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) { ... return 0; }   /* not 0: it is unloaded again */
MML_EXPORT void mml_mod_unload(MmlMod *mod) { ... }
```

`host` is a table of functions; `mod` is this load of the mod (pass it back to the host). Plain C works too
(`mods/examples/hello_c`); C++ mods may include the game's layouts (`mml/game/game.h`).

### Mods in Rust

```bat
python tools/new_mod.py my_mod --rust
```

makes `Cargo.toml` (a `cdylib` on the crate `sdk/rust/mml`, its `json` feature on), `.cargo/config.toml` (the
target `i686-pc-windows-msvc`: the game is 32-bit, so `cargo build` in the folder makes the DLL it loads),
`src/lib.rs` (the same working frame as the C++ one), `mod.json`, `config.json`, and the line
`mml_add_rust_mod(my_mod ...)` in `CMakeLists.txt`: the CMake build runs cargo (every Rust mod in one target folder,
`build/x86/rust`) and copies the DLL, its `.pdb` (the fault reports and `crash_triage` name its functions) and the
manifests as for a C++ mod; `deploy` takes it the same way. Once: `rustup target add i686-pc-windows-msvc` (without
it the configure leaves the Rust mods out and says so).

```rust
fn load(host: mml::Host) -> Result<(), String> {
    let frames = std::rc::Rc::new(std::cell::Cell::new(0u32));
    let f = frames.clone();
    host.subscribe(mml::EventType::FRAME, move |_| f.set(f.get() + 1));
    host.command_json("status", "frames seen", move |()| Ok(frames.get()));   // serde params and reply
    mml::info!(host, "hello from Rust (game {})", host.game_build());
    Ok(())                                                                     // Err: not loaded, logged
}
mml::entry!(load);   // mml_mod_api, mml_mod_load, mml_mod_unload
```

- **The crate** has two layers. `mml::sys` is all of `mml.h` as it is (made by `tools/rust_sys` with bindgen and
  committed: no libclang needed to build a mod; `tests/run_tests.py` checks it is what `mml.h` gives): every struct,
  constant and host function, the named enums as newtypes (`EventType::FRAME`, `Font::SMALL`). Over it a safe layer
  for the whole host table, `impl Host` by area, each method named after the host's member:
  [`lib.rs`](../sdk/rust/mml/src/lib.rs) the log (`info!`, `warn!`...), memory (`read::<T>`, `write`), hooks
  (`hook` with a `static Original`), events (`subscribe`, `Event`'s accessors), commands (`command`,
  `command_json`), state across reloads, settings, drawing, keys, actions, hotkeys (`Hotkey::rebind`), the mission,
  menus' items, the script machine, the world's actors, the game's state, the HUD's lines, sound, timers, zones,
  use-points; [`gfx.rs`](../sdk/rust/mml/src/gfx.rs) the render events' device and camera, the graphics bridge's
  passes (`gfx_pass`, `GfxFrame`), scene views, the frame's camera overridden, the game's camera, the geometry hidden
  and captured, shaders on the game's drawing (`shader_add` with a `ShaderRule`), the shadow map, the meshes the game
  draws (`scene_pieces`, `actor_meshes`), the frame's measures; [`people.rs`](../sdk/rust/mml/src/people.rs)
  skeletons, poses, animations, getting up, rounds, a mod's own items; [`world.rs`](../sdk/rust/mml/src/world.rs)
  sweeps, shapes, the water, zones for everyone, props (animations, physics), lights, the sun, the exhaust,
  particles.def, the particles' and the exhaust's hooks, the weather; [`media.rs`](../sdk/rust/mml/src/media.rs)
  sound, music, pictures, markers, a menu page of the game's controls (`menu_open` with `MenuControl`s);
  [`control.rs`](../sdk/rust/mml/src/control.rs) keys and actions kept from the game, the player locked, a vehicle's
  physics replaced (`vehicle_hook`, `VehicleStep`), bodies; [`data.rs`](../sdk/rust/mml/src/data.rs) a mod's files,
  the game's files, `.4ds` models (`Model`, closed when dropped), the saves' state, the statistics, the profile,
  services between mods. What the host keeps for a time is borrowed for that time (a mesh's arrays inside the
  closure `scene_pieces` calls, a model's while the `Model` lives); a member an older loader lacks is a panic that
  names it (`mml::host_fn!(host, member)` asks first: `None`).
- **The game itself**, [`mml::game`](../sdk/rust/mml/src/game/mod.rs): `Game::detect(host)` tells the builds by
  their code as `game::Detect` does, then `g.exe`, `g.ls3d`, `g.racing` are `game_build.h`'s and `racing.h`'s
  tables for the build running (`g.exe.missionPtr`, `g.exe.vehSpeed`: the C++ names), `kActorFrame` and the other
  layouts, the scripts' commands' numbers (`g.script_opcode("detector_inrange")`), and game.h's helpers member for
  member: `player`, `player_car`, `last_car`, `list_actors`, `actor_world_pos`, `children` and `descendants`
  (EnumFrames), `find_child`, `world_matrix`, `to_parent_point`, `test_line`, `car(actor)` (a `Car`: speed, gear,
  rpm, fuel, steering, axes, wheels...), `car_state_save` and `car_state_load`, `set_local_rotation`, `text_utf8`,
  and mathx.h's (`quat_matrix`, `quat_of`, `inverse`). Reads go through the host (never fault); what calls the
  game's own code is `unsafe`. The tables are made from the C++ headers by `tools/gen_rust_game.py` (`--check` in
  the offline tests): one source for both languages.
- **Graphics**: the feature `windows` gives the COM objects typed and borrowed (`frame.d3d_device()`,
  `d3d_context()`, `d3d_depth()`...: the `windows` crate's `ID3D11Device`; `host.d3d9_device()`), and
  `sdk/rust/mml-build` compiles HLSL in the mod's `build.rs` as `mml_shaders` does for C++ (fxc or dxc, optimised,
  warnings as errors, `sdk/include` on the include path): `mml_build::hlsl("pulse.hlsl", "PsPulse", "ps_5_0")`,
  then `mml::include_shader!("PsPulse")` in the mod. `mods/examples/depth_pulse_rs` is depth_pulse in Rust.
- **Callbacks are closures** (`Fn + 'static`), freed at the unload (or when their timer, zone or use-point is
  removed). They run on the game thread, one at a time, and may be entered again from inside themselves (a call into
  the host that sends an event): keep their state in `Cell` / `RefCell` behind an `Rc`. `Host` is `Copy` and not
  `Send`; `host.poster()` is, for a thread of the mod's own (`poster.post(|host| ...)` runs at the next frame; one
  that has not run when the mod unloads is dropped then, unrun).
  `host.on_unload(...)` runs at the unload, before the closures are freed: what must survive a reload goes there.
- **A panic** is caught at the edge of the mod, logged (`panicked in a command: <message> at src/lib.rs:12`) and
  raised as the exception `0xE0525553` (`mml::PANIC_EXCEPTION`): the loader takes it as any fault of a mod's (the
  mod unloaded at the next frame, `MOD_FAULTED`, the game goes on). Keep `panic = "unwind"` (Cargo's default): with
  `"abort"` a panic ends the game. A faulted mod's `on_unload` does not run (as a C mod's `mml_mod_unload`).
- `mods/examples/hello_rust`: hello_c's counts kept across reloads, a line at the top left (the mission, the
  player's place, health, money), a hotkey from the settings bound again when they change, and `hello_rust.status`,
  `.say {"text"}`, `.spawn {"car"}` (a car of the game's list 6 m in front of the player).
- `tests/mods/kit_rs`: every part of the safe layer checked: `kit_rs.offline` with no game (the offline tests),
  `kit_rs.live` in a running freeride (47 checks, each area against the host's own answers and the game's layouts;
  twice: the second sees the first's scans), `kit_rs.view` (a scene view seen by a pass), `kit_rs.page` (a menu
  page), `kit_rs.d3d9 {"on": true}` (the game's Direct3D 9 device in RENDER_WORLD: a square drawn), `kit_rs.item`
  (a pause menu item, `{"move": true}` moves it by `MenuItem::set`). The offline tests also fail when a member of
  `MmlHost` has no safe method in the crate. In a lab: copy `build/x86/harness/mml/mods/kit_rs` into the lab's `mml/mods`, `mod.load` it, and take it
  out after.

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

### Tests in the real game

What a mod does in the game can be a test that runs again: `tests/game/test_<topic>.py`, a function `test_<name>(g)`
that drives the game by the same tools (`g.tool('spawn', ...)`, `g.call('human.go', ...)`, `g.wait('ORDER_ENDED',
...)`, `g.sleep(ms)`, `g.pos(actor)`) and says what it saw with `g.check(name, cond, detail)`
(`tests/game/mml_game.py`; `tests/game/test_orders.py` is a whole one).

```bat
python tests/game/run.py --profile 1.2=4 --profile 1.0=Run     :: every case, in both labs at once
python tests/game/run.py orders --game 1.0                      :: one topic, one lab
```

Each lab's game is driven by its own MCP server, the labs at the same time. A case starts in the city's small
freeride without police, pedestrians or traffic (what a case does is then all that happens), the player in his car at its start (the same place in both builds): the runner launches
the games anew (`--reuse` takes a running freeride as it is), keeps that moment as a restore point and loads it
before each case; what a case spawns through `g.spawn_human` is removed after it (out of a car first). The profile must be one that has opened the freeride's
settings once (the game ignores the police slider otherwise): `--profile`, or mml.config.json's
`"test_profiles": {"1.2": 4, "1.0": "Run"}`. `BUILDS = ("1.2",)` keeps a module's cases to one build; `START =
"city"` starts them in the whole city instead (the river: `tests/game/test_river.py`). The game puts to sleep what
it has not drawn lately (act state 1, knowledge/notes/game.md): a case takes the player where its things are
(`g.tool('teleport', actor='car', to=...)`). Cases now: the orders, a car hitting a person, a camera turned, a prop
in a hand, a car made over the river, an exhaust under the elevated railway.

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
| `MML_EVENT_FILE_CHANGED` | what the game reads for a file's name changed (a mod's file changed on disk, came, went, or took another's place) | `name` (the game's name, lower case: `"maps\\crate.bmp"`) |
| `MML_EVENT_RENDER_BEGIN`, `_WORLD`, `_END`, `MML_EVENT_PRESENT` | the frame's drawing ("Drawing in the 3D world") | `scene`, `world`, `device` |
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
- **Under the menus**: the mods' drawing is a layer of the HUD's, drawn right after it (the loader's hook of the
  HUD's DrawAll), so the game's menus and console are drawn over it: an overlay need not hide itself when the pause
  menu opens. Over everything: the device at `MML_EVENT_PRESENT`.
- **Coordinates**: the menus' virtual units, whatever the resolution: the screen is 800 wide and 600 high, (0, 0)
  the top left corner. A text's size is its line height in these units (the HUD's money is 40, a menu's button 30).
- **Colours**: `0xAARRGGBB`. **Fonts** (the game's own): `MML_FONT_SMALL` (plain), `MML_FONT_BIG` (bold, the
  HUD's numbers), `MML_FONT_TITLE` (the menus' handwriting). Text is UTF-8, about 100 characters a call: the host
  turns it into the code page of the game's fonts (its language's: 1250, 1251 or 1252; a character with no place
  there is a `?`). A text read from the game (its texts, its frames' names) is in that code page:
  `game::TextUtf8(text)` (mml/game/game.h) before drawing it, logging it or putting it in a reply. `MML_DRAW_LEFT`,
  `_CENTER`, `_RIGHT`: what `x` is.
- **4096 items a frame**, every mod's together: what a mod queues past them is dropped (the log says so once, and
  `call draw.state` counts `dropped_last_frame`). A mod that draws in the world (lines over the scene) draws what is
  near, and one line a straight edge: cut it at the camera's near plane rather than into many pieces
  (mods/examples/debug_views/triggers.cpp, `Line3`).
- It is the game's own 2D drawing, at the end of its 2D pass (over the HUD and the menus), so `screenshot` shows
  it. Nothing is drawn while the game loads.
- A fault while your items are drawn turns your drawing off until the mod is reloaded (the log says so); the game
  goes on.
- To try a layout without building anything: `call draw.text {"x": 400, "y": 100, "text": "hello", "size": 30,
  "color": "#FFFFD040", "font": 1, "align": "center", "ms": 10000}` (also `draw.rect`, `draw.line`, `draw.clear`).

### Drawing in the 3D world (the render events)

LS3D draws each frame's scene (the mission's world, or a menu's 3D scene) in one call; the loader tells mods
around it, on the game thread, in both builds:

| Event | When | What to do there |
|---|---|---|
| `MML_EVENT_RENDER_BEGIN` | the scene is posed and about to be drawn | write bones, move things for this frame's picture |
| `MML_EVENT_RENDER_WORLD` | its 3D world is drawn, its 2D (HUD, menus) not yet | draw with the game's device into the scene: depth-tested against the world, under the HUD |
| `MML_EVENT_RENDER_END` | the scene is drawn | put back what BEGIN changed |
| `MML_EVENT_PRESENT` | the frame is complete, about to be shown | draw over everything (it is in `screenshot`'s picture) |

The event carries `scene` (the I3D_scene), `world` (1: the mission's world; the main menu's `00menu` is a world
too: check `mission_name()`), `device` (the game's `IDirect3DDevice8`); ask `MML_EVENT_HAS(ev, world)` first.

```c
#include <d3d9.h>
IDirect3DDevice9 *d = (IDirect3DDevice9 *)H->render_device(9);   /* NULL on plain Direct3D 8 */
MmlRenderCamera cam = {sizeof cam};
H->render_camera(&cam);          /* view, proj (the device's), eye, axes, fields of view, near, far, viewport */
/* in RENDER_WORLD: set the world matrix and the states you need, draw; the view and projection are the camera's */
```

- **The game's state is kept for you**: around `RENDER_WORLD` and `PRESENT` the loader captures every Direct3D 9
  state (a `D3DSBT_ALL` state block), the render target and the depth buffer, and puts them back after the mods
  (nothing is done in a frame no mod subscribed). So set every state you rely on (the game leaves whatever it drew
  last: lighting, fog, blending, textures, shaders) and leave them as they are.
- The device is the game's Direct3D 8 one; on a wrapper that runs the game on Direct3D 9 (dxwrapper's d3d8to9, as
  in the labs) `render_device(9)` gives its Direct3D 9 interface. On plain Direct3D 8 that is NULL and nothing is
  kept: draw through the Direct3D 8 interface and put back what you change.
- Every frame of a mission draws the world once (`call render.state`: renders, world renders, stage 1, presents,
  the camera now).
- `mods/examples/world_marker` (not enabled by default): a diamond and a beam over the player, fixed-function
  Direct3D 9, hidden by what stands in front (`world_marker.at {x, y, z}` puts it anywhere).
- `mods/examples/debug_views` (not enabled by default), its triggers view: lines over the frame instead (through
  walls), with `camera_project`: the mission scripts' detectors and areas (F7; Options > Mods > Debug views >
  Triggers; `debug_views.triggers.list`). It reads the scripts' programs and names their commands with `mml/game/script_commands.h`
  (`game::ScriptCommandName(opcode)`, `game::ScriptOpcode("detector_inrange")`: the numbers differ between the builds).
- Mods that use `game.h` (the player, cars) call `game::Detect(H->module_base("LS3DF.dll"))` in `mml_mod_load`
  first: its helpers read the build's addresses.

### Effects over the frame (the graphics bridge)

For effects that need the frame's depth (water, smoke, fog volumes, outlines, a scanner's wave) a mod draws with
Direct3D 11 on one device the loader shares between every mod, beside the game's Direct3D 9Ex (the labs' wrapper):

```c
#include <d3d11.h>
#include "shaders/PsMine.h"      /* mml_add_mod(... SHADERS mine.hlsl:PsMine:ps_5_0) */
void Pass(const MmlGfxFrame *f, void *user) {
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)f->context;
    /* make your objects on f->device the first time, and again when f->generation changes */
    ctx->PSSetShader(g_ps, NULL, 0);
    ctx->Draw(3, 0);             /* the full-screen triangle: its vertex shader is bound */
}
H->gfx_pass(M, 100, Pass, NULL);   /* in mml_mod_load; lower orders run first; the unload takes it back */
```

```hlsl
#include "mml/gfx/frame.hlsli"   // the frame's constants (b0), depth (t0), colour (t1), samplers, helpers
float4 PsMine(float4 pos : SV_Position) : SV_Target {
    float z = SceneDepth(pos.xy);
    if (IsSky(z)) return 0;
    float3 p = g_eye.xyz + ViewRay(pos.xy) * z;      // the world point this pixel shows
    float a = (1 - FogAmount(z)) * 0.5;
    return float4(float3(1, 0.5, 0) * a, a);         // premultiplied: rgb times alpha
}
```

- **`"gfx": true` in `mod.json`.** A readable depth buffer cannot be multisampled, so while an enabled mod says so
  the loader makes the game's device without multisampling (at its creation and at a Reset; the game's own setting
  is left as it is). A pass registered by a mod without it, on a multisampled device, is not run: `gfx.state` says
  why. Enabling such a mod takes a restart of the game.
- **Whether the passes run**: `H->gfx_state(M, why, cap)` (1 on, 0 off and why, -1 not yet), for a mod to tell its
  player why its effect is missing (dlss_nr does so in the game's console, once a mission and at its hotkey).
- **Under DXVK** (its `d3d9.dll` beside the game, for ReShade on Vulkan or its own sake: dxwrapper loads the game
  folder's `d3d9.dll` before the system's) the game's Direct3D 9 runs on Vulkan, whose shared textures Direct3D 11
  cannot open. The bridge then keeps two of each texture and copies them on DXVK's own queue at each hand-over
  (DXVK's interop interfaces, `host/gfx_vk.cpp`): the passes see the same textures as on 9Ex, about 0.5 ms a copy at
  1280x960 on an RTX 3070. `gfx.state`'s `d3d9` says which way the frame goes.
- **Each frame of the mission's world** (not the menus): the frame's depth buffer is a readable one while the world
  is drawn; at the point where the world is drawn and the HUD not yet (after the mods' `RENDER_WORLD` drawing, so
  their Direct3D 9 drawing is in the frame the passes see), the frame's colour and linear depth (metres along the
  view, `g_depthProj.z` for the sky) are handed to the passes, which draw in order into one effects target
  (premultiplied RGBA16F, cleared to 0), each laid over the ones before it (the blend state the bridge binds: ONE,
  INV_SRC_ALPHA; a pass returning 0 leaves them); the target is blended over the frame (`frame * (1 - a) + rgb`),
  under the HUD.
- **The constants** (`MmlGfxConstants`, `cbuffer MmlFrame`): the camera (eye, axes, projection), the frame's size,
  the time, the world's fog (as the world was drawn: start, end, mode, colour) and a sun (`w` 1 when the game's: its
  Direct3D light, else the scene's own sun from LS3D's light frames; a default from above when there is none), the
  ambient light (the scene's). `frame.hlsli`: `ViewRay`, `ScenePoint`, `SceneDepth`, `IsSky`, `FogAmount`,
  `ApplyFog`, `ToScreen`, `Lin`/`Enc` (the game's colours are gamma-encoded).
- **The sun's shadow map** (while a mod asks for it with `shader_shadows`, the same map the shaders on the game's
  drawing get, below): bound to t2 (`g_sunShadow`, R32F: the depth from the sun, 1 where nothing is), its matrix and
  size in the constants (`g_sunShadowViewProj`, `g_sunShadowInfo`; `MmlGfxFrame::sun_shadow` NULL when there is
  none this frame: no sun above the horizon). `SunLit(world)` (one texel: for many samples a pixel, a ray marched
  through the air), `SunShadow(world)` (filtered: a surface's). The map covers `extent` metres around the camera.
- `mods/examples/sun_shafts`: the sun's light in the air where the sun reaches it (volumetric light, god rays): each
  pixel's ray marched through the shadow map at half the frame's size, a blur and an upsample that keep depth edges;
  its settings on the game's Options > Mods page (below, "Settings in the game's Options"), F7 on and off.
- **State**: the bridge binds the target, the viewport, t0/t1, s0/s1, b0 and the full-screen vertex shader before
  each pass; anything else a pass sets is cleared after the passes. A pass runs on the game thread behind its mod's
  fault guard.
- **Costs** are measured: `call gfx.state` gives each pass's CPU and GPU time (timestamps), the bridge's copy and
  waits (about 0.65 ms of CPU a frame for the whole bridge at 1280x960 in the labs), the device, the frame's size,
  the fog and sun found.
- `mods/examples/depth_pulse`: a scanner's wave over the world from the player (`depth_pulse.now`, F11).
- **The 2D pass for a mod** (`gfx_ui(M, MML_GFX_UI_HIDE)` or `_TAKE`): the HUD, the menus over the world and the
  loader's `draw_*` are drawn into a shared target (premultiplied RGBA8, the frame's size) that the passes get as
  `MmlGfxFrame::ui` (the last frame's: the 2D pass comes after them); `_HIDE` keeps it off the screen (a VR mod
  shows it on a panel), `_TAKE` puts it back over the frame as well. The game's additive 2D (glows) is put back as
  blended, not added. Input stays the screen's: a mod that shows the menus elsewhere maps the cursor itself.
  `mods/examples/hud_panel` (not enabled by default): the HUD on a panel in the world (`hud_panel.place`,
  `follow`, `mode`).

### The scene from another camera (a reflection, a mirror, a headset's eyes)

LS3D itself draws the mission's scene again, before the frame's own drawing, from a camera of the mod's, into
targets of the graphics bridge: the whole scene as the game draws it (its rooms, lights, culling, the people and
cars), so water reflects the street and a headset's eye sees what the screen would.

```c
int PlaceMirror(MmlViewCamera *c, void *user) {
    /* c comes as the game camera: world (rows right, up, forward, eye), tan_left/right/down/up, near_z, far_z */
    for (int i = 0; i < 3; i++) { c->world[i] = -c->world[i]; c->world[8 + i] = -c->world[8 + i]; }   /* look back */
    return 1;                    /* 0: not this frame (the last picture stays) */
}
int view = H->scene_view(M, 640, 200, PlaceMirror, NULL);   /* 0, 0: the frame's size; "gfx": true in mod.json */
/* in a pass: f->views[i] (MML_GFX_HAS(f, views)): color, depth (linear, metres), view, proj, eye, fresh */
```

- **The camera**: `world` is LS3D's frame matrix (row vectors; right x up = forward, left-handed); its rotation is made
  orthonormal from forward and up (right is made again: a mirrored, improper basis becomes a proper one, so the picture
  is flipped left to right: flip it back when it is shown). The frustum is four tangents, so asymmetric ones (a
  headset's eyes, OpenXR's `XrFovf`, a part of the screen) are drawn as given; LS3D culls with a symmetric frustum
  that holds them. `near_z`, `far_z` as given are the game camera's.
- **A reflection**: the eye, forward and up mirrored in the plane, `MML_VIEW_CLIP` with the plane (`clip`: `a x + b
  y + c z + d >= 0` is drawn; put it a few centimetres above the surface, so the ground itself is left out). Without
  `MML_VIEW_OWN_SECTOR` the view sees from the game camera's room (LS3D's sector), which is what a mirrored eye under
  the ground needs; with it, the room is looked up at the view's own eye (a security camera elsewhere in the city).
  The plane is not applied to the sky: LS3D draws its backdrop with the camera's view without its position, so the
  sky lies about the world's origin, and the loader turns the plane off while that view is set (`gfx.state`
  `view_sky_sets`).
- **A mirror in the world** (any plane, not the ground): the camera mirrored in it, the plane as the clip plane on
  the camera's side; in the pass, intersect each pixel's ray (`ViewRay`) with the mirror, keep it where it is nearer
  than `SceneDepth` (what stands in front hides the mirror), and sample the view where the hit point lands through
  the view's own `view x proj` (`MmlGfxView`): exact for any frustum, and the flip of the mirrored basis does not
  matter. Seen from behind, return 0 from the camera's function (no view drawn) and draw its back.
- **Effects in the views**: `H->gfx_pass_views(M, pass, 1)` runs a pass also over each view's picture (at the
  view's stage 1, `MmlGfxFrame::in_view` the view's handle; its depth, colour, constants and size are the view's;
  its target blended over the view's colour), so a scanner's wave or water shows in mirrors too. Off by default; a
  pass that shows views leaves it off (a mirror would draw itself in its reflection). Each view with such passes
  waits for both devices once more (in the labs a frame-sized mirror with one such pass: about 4 ms of CPU).
- **What is put back**: the game camera byte for byte (pose, projections, frustum, room), the scene's callback and
  rectangle, its sound listener (detached while a view draws), its lens flares (kept out of the views: their fades
  are the frame's). `check` mode of the example compares a view with the frame: the same pixels.
- **Drawing into a view**: `MML_EVENT_VIEW_WORLD` (with `ev->view`) is `RENDER_WORLD` for each view's drawing: the
  device's transforms and target are the view's, its state kept around the mods as there. A mod that draws in the
  world subscribes to both, so its things are seen in mirrors and reflections (`world_marker` does).
- **Costs**: each view is a whole second drawing of the scene: in the 1.2 lab at 1280x960, a 640x200 mirror about
  1.5..1.7 ms of CPU, a reflection of the frame's size about 2.9 ms (`call view.state`: each view's CPU time, times
  drawn, skipped and why, the camera). At most eight views. `call view.show {"handle", "rect": [x, y, w, h]}` copies
  a view onto the frame (800x600 units; no rect: off), to look at one without a pass.
- `mods/examples/rear_mirror` (not enabled by default; `rear_mirror.mode`): `"mirror"` (a rear-view mirror),
  `"stereo"` (two eyes of a headset, asymmetric), `"reflect"` (a puddle around the player: a planar reflection with
  its clip plane), `"spin"` (a mirror standing in the world, turning; `rear_mirror.spin {"at": [x, y, z]}`),
  `"check"` (the left half of the frame drawn again and compared: black when equal). `depth_pulse` runs its pass in
  the views: its wave is seen in the mirror.

### Motion vectors

What a temporal effect needs (TAA, motion blur, an upscaler, DLSS): each pixel's motion, in pixels, to where the
surface it shows was the frame before. A mod asks once, `H->gfx_motion(M, 1)` (Rust `host.gfx_motion(true)`), and
from the next frame of the mission's world its passes get them: `f->motion` (`MML_GFX_HAS(f, motion)`; an
`ID3D11ShaderResourceView*`, R16G16_FLOAT), bound to t3, `g_motion` and `PixelBefore(pixel)` in `frame.hlsli`:

```hlsl
float2 before = PixelBefore(pos.xy);                         // pos.xy plus its motion
float3 history = g_myHistory.SampleLevel(g_linear, before * g_size.zw, 0).rgb;
```

- **The camera's** part, for everything: the frame's linear depth reprojected through the last frame's camera (the
  sky by its direction alone).
- **What moves by itself** (cars, their wheels, doors, props, people): LS3D draws every face group through
  m_RenderFGroup (`host/geom.cpp`), which knows the visual it draws; a draw whose world transform is not the one it
  had the frame before is drawn again right after LS3D's own draw (its buffers are LS3D's at that moment: it refills
  dynamic buffers as it goes), into a target of the objects' motion, every vertex through this frame's and the last
  frame's world, view and projection (`host/motion.cpp`, `host/shaders/bridge9.hlsl` VsMotion, PsMotion). At the
  frame's stage 1, PsMotionAll takes an object's motion where the object is what is finally seen (its depth is the
  frame's) and the camera's elsewhere. Each draw is known by its visual, its face group, the how-many-th time that
  pair is drawn this frame (one visual draws a car's four wheels) and its place in the call.
- **A person's limbs**: LS3D skins people on the CPU and copies the positions into the vertex buffer it draws
  (`game_build.h` smCopySkinned). The host keeps each copy (from the skinning's cache and the mesh's own buffer,
  system memory: the drawn buffer is video memory, which the CPU reads uncached) and the one the world drew the frame
  before; a person's draw goes again from a buffer of the host's with both positions for each vertex (VsMotionSkin),
  so a planted foot is still and a swinging arm moves by its own.
- **Not**: billboards (as the camera), what writes no depth (a car's shadow decal, glass, smoke: the motion of what is behind;
  the edge of a shadow that moves with its car mismatches either way), the scene views (`f->motion` is NULL there).
- **Its cost**: only while a mod wants them; the moving draws again (in the freeride's traffic 15 to 90 of them,
  0.2 to 0.4 ms of the game thread at 1280x960 on an RTX 3070; a walking person's copies 0.005 ms) and one
  full-screen pass. `gfx.state`'s `motion`: who asked, the draws known, moved and drawn again, the people's
  (`skinned_known`, `skinned_again`, `limbs_moving`, `limbs_most_m`), their time (`ms`, `ms_copies`). Under DXVK the vectors go to Direct3D 11 with the
  other shared textures.
- `dlss_nr`'s debug views show them: `1` as colours (grey still; red to the right, green down), `5` the frame before
  moved by them against this one (black where they are right), `6` the same without them.

### The camera: the frame drawn from a mod's, a world point on the screen

```c
int Place(MmlViewCamera *c, void *user) {      /* each frame of the mission's world, before anything is drawn */
    /* c comes as the game camera (as a scene view's function gets it): change its pose, frustum, range */
    c->world[12] += 0; c->world[13] += 5; c->world[14] += 0;     /* the eye 5 m higher */
    return 1;                                  /* 0: this frame from the game's camera */
}
H->camera_override(M, Place, NULL);            /* the last mod that asked owns it; fn NULL (or the unload) gives it back */
float s[3];
if (H->camera_project(M, world, s)) { /* s[0], s[1]: 800x600 units on the last frame drawn; s[2]: its depth */ }
float o[3], d[3];
H->camera_ray(M, 400, 300, o, d);              /* the ray through the screen's middle: an aim, a pick */
```

- **Only the drawing moves**: the game's camera logic, the player's aim and crosshair, the sound's listener stay
  the game's; the camera's bytes are put back after the frame is drawn (as a scene view's). The scene views start
  from the overridden camera (a mirror shows what the overridden eye would see in it). A chase camera, a free
  camera, a skateboard's or a plane's: the mod moves its eye each frame from whatever it follows.
- `MML_VIEW_OWN_SECTOR` in `c->flags` looks the room up at the new eye (the game camera's room otherwise: wrong for
  an eye far from it, right for one beside it).
- **Projecting**: `camera_project` and `camera_ray` use the camera the last frame of the world was drawn with
  (overridden or not), so a label over a person's head lands on him whatever drew the frame. 0 before the first
  frame or behind the camera.
- `call camera.set {"eye": [x, y, z], "at": [x, y, z] | "dir": [...], "up", "fov": 60, "ms": 10000}` holds a
  camera from the bridge (an agent's look at a place: then `screenshot`), `camera.release` gives it back,
  `camera.state` (who owns it, frames drawn, declined, skipped and why, the last frame's eye and forward).
- `mods/examples/orbit_cam` (not enabled by default; F8, `orbit_cam.on {"on": true}`): the camera circles the
  player while he is played as usual; a label over his head through `camera_project`.

### The game's camera: its modes, its field of view

`camera_override` draws the frame from a camera of the mod's while the game's camera goes on as it was. The game's
own camera manager (G_Camera, driven every frame by the game) is reached too:

```c
MmlGameCamera c = {sizeof c};
H->game_camera(M, &c);                      /* c.mode, c.mode_name ("C Chase"), c.car, c.fov (radians, across) */
H->game_camera_mode(M, MML_GAMECAM_EYES);   /* on foot: from the player's eyes, by the game's own SetMode */
H->game_camera_fov(M, 1.6f);                /* held: put back whenever the game changes it; 0 gives the game's back */
```

- **Modes** are switched by the game's own `G_Camera::SetMode`, as its camera-change key does, so the game drives the
  new mode as its own (collisions, the car's sway, the head's place). Only what the player's state allows: on foot
  `MML_GAMECAM_BEHIND` (1) and `_EYES` (3); in a vehicle 7, 8, 9 (behind), 12 (by a wheel), 13 (the driver's head), 14
  (over the bonnet). The game's other modes (the scope, a dialogue, a death, a lock, a race) belong to what sets
  them; the call is refused then, and the log says which modes the camera can go to.
- **The field of view** is set through the camera's own setters (the near pass' too) and held: the game changes it
  only at a few moments (a mode pushed and popped, a dialogue, the scope), and the held value is put back at the
  next frame. The game's own value (70 degrees in the city) comes back with 0, or when the holder unloads.
- **Another car** (the bridge's `camera.game {"follow": car}`, a car or a person sitting in one): the game's own
  `G_Camera::SetCar`, what getting into a car does for the player, so the game's camera goes behind that car in its
  last vehicle mode, with its sway and collisions: a driven car watched as the player would see his own (a car the
  game draws does not sleep). `"follow": "player"` (or null) gives it back; the game takes it back itself when the
  player gets into or out of a car, and the loader does when the followed car is destroyed.
- `call camera.game {"mode": 13, "fov": 1.6}` tries both from the bridge (`"fov": null`: the game's back);
  `call camera.game` alone shows the state and `modes_now`.

### The game's files and models (the archives' names, a model's file, .4ds read)

```c
static int OnFile(const char *name, const char *source, void *user) { /* "models\\taxi00.4ds", "a2.dta" */ return 0; }
H->game_files(M, "models\\*.4ds", OnFile, NULL);           /* every model the game has: 3223 */
char file[128];
H->model_file(M, H->actor_frame(M, car), file, sizeof file); /* "Models\\taxi00.i3d" */
int m = H->model_open(M, file);                             /* models\taxi00.4ds, parsed */
MmlModelNode nodes[128];
int n = H->model_nodes(M, m, nodes, 128);
MmlModelMesh mesh;
for (int i = 0; i < n && i < 128; i++)
    if (H->model_mesh(M, m, i, 0, &mesh)) { /* mesh.pos (3 a vertex, in nodes[i]'s space), mesh.index (3 a triangle) */ }
H->model_close(M, m);
```

- **`game_files`** lists every file the game can open: the archives' directories read (the same keys open both
  builds' archives; each name once, from the archive the game looks in first: a8, the patch, before a2...), the
  game's folder's own under the same folders (the game opens those first) and the mods' (before both). The pattern
  takes `*` and `?` without regard to case. Read once, at the first question (a few tenths of a second).
- **`model_file`**: LS3D keeps the name every model was opened by in the model frame (`+0x154`: the game's model
  cache, the scenes' actors, a prop alike), so any frame under a model (a bone, a car's door) tells its file. The
  mission's scene's own frames are under no model: none.
- **`model_open`** reads a `.4ds` (version 29: 3220 of the game's 3223; two scenes of version 27 and one of 58 are
  refused, said why) through the game's file layer, a mod's own `files\models\` too: its nodes (their parent, frame
  and visual type, local placement), each visual's meshes per LOD (positions, normals, uv, triangles with their
  material) and its materials (textures). Skinned meshes and morphs give their base mesh; the weights and morph
  targets are skipped. The arrays live until `model_close` (or the mod's unload).
- `call files.game {"match": "models\\tommy*"}`, `model.file {"actor": "car" | "frame" | "prop"}`, `model.info
  {"file": "taxi00", "meshes"}`.

### The meshes the game draws (the scenery around a place, an actor's parts posed)

```c
const float lo[3] = {x - 30, y - 10, z - 30}, hi[3] = {x + 30, y + 20, z + 30};
H->scene_focus(M, lo, hi);                           /* once, and again when the place moves */
/* later frames: */
MmlMeshPiece pieces[512];
int n = H->scene_pieces(M, pieces, 512);
for (int i = 0; i < n && i < 512; i++) {
    const MmlMeshPiece *p = &pieces[i];               /* world = (pos, 1) * p->world; p->index: 3 a triangle */
}
n = H->actor_meshes(M, car, pieces, 512);            /* the car's body, doors, wheels, where they are now */
```

- Read from LS3D's own vertex and index buffers (the finest LOD of each visual), not from files: what the game
  draws, as it is now (a car's dents are in its buffers).
- **The scenery** is shared by every mod: the host walks the mission's scene a little each frame (a city's 24 000
  frames in about 2 ms, spread over frames), and every 0.25 s takes the plain and lightmapped objects whose bounds
  touch any mod's box, shown, not a part of an actor (people, cars: `actor_meshes`), each mesh read once within 3 ms
  a scan (the rest at the next scans; the first scan of a mission takes its time). `scene_pieces` gives those
  touching this mod's box. A piece's `world` puts its vertices in the world (most meshes are in their frame's space,
  the terrain's and some large pieces in the world's: the host finds which by the engine's bounding sphere);
  `version` changes when it moves (a door), `moving` says it moved within the last second. Billboards, particles,
  skinned and morphed meshes, mirrors are not pieces.
- **`actor_meshes`**: every shown visual under the actor's model, posed by their frames now (a door open, the
  wheels turned), without another model hung on it (a person sitting in the car).
- The arrays are the host's until the next frame: copy what is kept.
- `call scene.focus {"at": "player", "radius": 30}`, `scene.pieces {"limit"}` (with the scan's numbers),
  `actor.meshes {"actor": "car"}`.

### Questions to the world (rays, sweeps, the ground, the water, what is near, the collision's shapes)

```c
MmlHit h;
float from[3] = {x, y + 1.5f, z}, to[3] = {x + 100 * dx, y + 1.5f, z + 100 * dz};
if (H->world_ray(M, from, to, MML_WORLD_ALL, H->human_player(M), &h)) {
    /* h.pos, h.normal, h.fraction; h.actor: whose object (NULL: the static world); h.classes 0x100 a person,
       0x200 a car; h.flags >> 24: a static surface's material */
}
H->world_sweep(M, from, to, 0.3f, MML_WORLD_ALL, NULL, &h);    /* h.pos: the sphere's centre where it stops */
void *people[32];
int n = H->world_near(M, from, 20, 2, people, 32);             /* type 2 people, 4 cars, 0 any; nearest first */
MmlShape s[256];
int k = H->world_shapes(M, lo, hi, MML_WORLD_STATIC, s, 256);  /* triangles, boxes, spheres, poles in a box */
```

- **The game's own tests answer**: `world_ray` is the test the bullets and the AI cast (`g_collision::_TestLineH`),
  `world_sweep` the one a person's and a car's moves sweep (`TestSphereB`): the static world (buildings, the
  ground, fences, poles: the level's collision, not the drawn meshes) and the moving objects (each person's
  spheres, each car's boxes, doors). `MML_WORLD_NO_PEOPLE`, `MML_WORLD_NO_CARS` leave those out.
- **`ignore`**: an actor's own objects are passed the game's way (its exclusion list, as a shooter's own body is
  passed by his bullets), and the list is put back after. A ray that starts inside a person does not hit him.
- **The ground**: a ray straight down with `MML_WORLD_STATIC` (`call world.ground {"at"}` does that).
- **The water** (`world_water`): the static world's faces of the water's surfaces under a point, as the game finds
  them under a person and a car (31: the river and the sea, deadly; 44: shallow water a car drives through): its
  `height`, the `depth` to the bed (the city's river has none in the collision: -1), and `covered` when the static
  world is between the point and the water. The city's river is one plane under the whole city (y -9.99), so the
  ground everywhere stands over it, `covered`: open water is the one not covered.
- **The shapes** (`world_shapes`): what the game keeps in its grids, in the world's space: the static world's
  triangles (normal out of the solid), boxes (centre, half sizes, axes), spheres and poles (vertical cylinders);
  the moving objects' boxes and spheres with their owner. What a physics library builds its world from near a
  body (a ragdoll's ground and walls, a fish's riverbed, a skateboard's ramps).
- **The surfaces' names** (`material_name(n)`): a static face's surface (its material, `flags >> 24` of a hit or a
  shape) by the game's own table, tables\materials.def as the game read it (its records' first 32 bytes), in UTF-8;
  NULL for a number with no record. 31 is the deadly water, 33 the walls nothing draws.
- **The pedestrians' web** (`web_nodes(M, at, radius, out, max)`): the mission's check.bin as the game keeps it, the
  places people walk between: each node's place, its index (what the links name), the game's bits of its kind and its
  id, and its links (the other node's index, MML_WEB_ONE_WAY, the cost), nearest first. What a person's route is built
  on (`ai_path`'s waypoints name these nodes); the road network for cars is `road_near`.
- Called on the game thread (in any event of the mod's or a command); every call is the game's work: a ray costs
  little, a large box of shapes more (tens of microseconds for a few metres around a person).
- `call world.ray {"from": "camera"}` (what the crosshair looks at; `"screen": [x, y]`), `{"from": "player", "to":
  "car"}`, `{"from": [x, y, z], "dir": [x, y, z], "length": 50, "what": "static", "ignore": ""}`; `world.sweep`
  (the same and `radius`), `world.ground {"at"}`, `world.near {"at", "radius", "type"}`, `world.shapes {"at",
  "radius"}` or `{"lo", "hi"}` (counts by kind, the first `max` shapes, each static one's `material` named),
  `world.water {"at"}`, `world.materials`, `world.web {"at", "radius", "max"}`.
- `mods/examples/debug_views` draws them: `collision` the world's whole collision (the static shapes by surface with
  the surfaces' names, the moving objects') as solids, a graphics pass: drawn into a depth buffer of its own, so each
  pixel shows the nearest collision surface, then laid over the frame against its depth with a tolerance (the
  collision wins over the world's own surfaces it lies on), with an ambient occlusion and screen-space outlines;
  `web` the pedestrians' web (lines in the scene through the game's device at RENDER_WORLD).
- `mods/examples/probe` (not enabled by default): what the crosshair looks at, the ground under the player, a
  sphere swept ahead of him, the people near him, and the static world's collision around him drawn as lines.

### The game's state: money, health, the inventory

```c
int money = H->money(M);                         /* the freeride's money (the HUD's $); INT32_MIN: no game */
H->money_set(M, money + 50);                     /* the game's own setter: the HUD shows it at once */
void *p = H->human_player(M);
float max, hp = H->human_health(M, p, &max);     /* 200 of 200 for the freeride's Tommy */
H->human_health_set(M, p, hp + 10);              /* above 0, at most max; the player's HUD number told */
MmlItem items[MML_SLOT_COUNT];
int n = H->human_items(M, p, items, MML_SLOT_COUNT);   /* slot, id, loaded, reserve */
H->item_ammo_set(M, p, MML_SLOT_HAND, -1, 100);  /* the reserve of the weapon in his hand (-1: as it is) */
H->human_take_item(M, p, 12);                    /* the scripts' human_delweapon; give_item gives */
```

- **The money** is the freeride's (`C_game +0x3640`, what the scripts' `freeride_scoreget` reads); the story has
  none. It is set through the game's `C_game::SetScore`, so the HUD shows the new sum.
- **Health**: a person's current and maximal health (`C_human +0x644`, `+0x604`); a write changes the number the
  game's hits subtract from, and for the player the HUD's number is told as a hit tells it (the percentage of the
  maximum). A death is not written: it is the game's (a hit, a fall, a script's `human_death`).
- **The inventory** is the game's `G_Inventory` (`C_human +0x480`): the hand, five belt slots, the coat's big weapon,
  four extra items, the one put away in a car (`MML_SLOT_*`). Rounds are written in place (the HUD shows them);
  items are given and taken by the scripts' own commands (`give_item`, `human_take_item`), so the weapon models,
  the cycle and the HUD follow. Item ids are `world.items`'s.
- `call state.money {"set": 1000}` (or `"add"`), `human.health {"actor", "set"}`, `human.items {"actor"}`,
  `human.ammo {"actor", "slot", "loaded", "reserve"}`, `human.take {"actor", "item": 12 | "Saw off shotgun"}`.
- `mods/examples/bounty` (not enabled by default): each person the player kills pays `reward` dollars, his health
  comes back once nothing has hit him for a while, `endless_ammo` keeps the hand's weapon's reserve full.

### The developer's switches: a person kept from harm, the game's HUD hidden

```c
void *p = H->human_player(M);
H->human_invulnerable(M, p, 1);   /* the game's harm not run for him: 1 now; 0 lets go; -1 only asks */
H->hud_game(M, 0);                /* the game's HUD not drawn (1 gives it back, -1 asks): 0 hidden now */
```

- **Kept from harm**: every harm reaches a person through his `Hit` (C_human::Hit, vtable slot 31: bullets, blows,
  explosions, a car that strikes him through C_human::Collision, a fall's landing, a hit of type 6). The loader's
  hook of it returns before the game's for him, as `C_player::Hit` itself skips it while a scene holds the player:
  no damage, no reaction, no HUMAN_HIT. Each mod (and the bridge) holds him on its own; he stays so while one of
  them does, until his removal or the mission's close. For the player, a hit still drops the sniper's scope
  (C_player::Hit's own step after the call).
- **The HUD hidden**: the game's G_IndicatorsClass::DrawAll (the health, the ammo, the radar, the compass, its
  texts: the first thing of the 2D pass) is not run while any mod or the bridge hides it; the menus, the console and
  every mod's drawing stay. Hooked at the first hide.
- `call human.invulnerable {"actor": "player", "on": true}`, `hud.game {"show": false}`.
- `mods/examples/devtools` (not enabled by default): these two, a weapon that never runs dry, flying (the game's
  movement and jump hooked for the player: `game_build.h` `humanMove`, `humanJump`; his place set after each tick as
  the scripts' ACTOR_SETPOS sets it), a free camera (`camera_override` and the mouse's actions captured) and the
  field of view (`game_camera_fov`), each a switch on its page in the game's Options > Mods. A port of pet1kk's
  Developer Tools (a standalone ASI for 1.2, PR 2) onto the loader, for both builds.

### A mod's own items

```c
MmlItemDef rod = {sizeof rod, "rod", "Fishing rod", "2rod.i3d", 17 /* made like the bucket */, -1, NULL};
int id = H->item_define(M, &rod);                /* 35 and up; the same id for "rod" in every run */
MmlItemDef fish = {sizeof fish, "trout", "Trout", NULL, 0 /* a key: kept among the extra items */, -1, "A trout!"};
H->item_define(M, &fish);
H->give_item(M, H->human_player(M), id, 0, 0);   /* given, held, dropped, picked up as the game's own */
```

- **What it is**: one more record of the game's items (`tables\predmety.def`, as the game loaded it), copied from
  the game's item it is made `like` (where it is kept, how it is held, whether it is a weapon), with its label,
  the model the game puts on the ground when it is dropped (`models\`, or the mod's own `files\models\`) and its
  flags (`MML_ITEM_*`, -1: like's). The game shows the label itself: the pickup menu, the line when it comes into
  the hand, "<label> collected" (or `collected`).
- **Its id** (35 and up) is kept for the mod's name of it in `<game>\mml\items.json`, so a save holding the item
  finds it again; defining the same name again gives the same id and changes the record (a hot reload does). An
  id kept from an earlier run whose mod is not loaded is an inert item named by its key. Room for about 60 items:
  an id is given only where the game's own texts 3300 + id, 3400 + id and 3500 + id are free.
- `item_find(M, "rod")` (this mod's) or `"othermod/rod"`; `world.items` lists them with their `mod`;
  `call items.define {"name", "label", "model", "like", "flags", "collected"}` (the bridge's own), `items.state`.

### Statistics, the profile's progress

```c
MmlStats s = {sizeof s};
H->stats_get(M, MML_SAVE_PROFILE, &s);          /* the profile's totals; MML_SAVE_SLOT: the game being played */
MmlProgress p = {sizeof p};
H->profile_progress(M, &p);                      /* chapter 20, freeride_cars "A".."T" as bits, the garage... */
int slots[600], n = H->profile_saves(M, slots, 600);   /* its saves on disk, by slot */
```

- **Statistics**: the game keeps none, so the loader counts what the player does from the events: people he killed
  (his weapons, his car), his deaths, his shots, the hits he gave, the cars he got into, his car's crashes of 5 m/s
  and more, the falls that hurt him, the items that came to him, the distance on foot (on the ground) and in
  vehicles (a move faster than a runner or a car is a teleport, not counted), the seconds the mission's world ran,
  the freeride's money gained and lost. Two sets: the game being played (kept in its saves, read back with them; a
  new game and a freeride start at 0) and the profile's totals (beside its `.sav`, written at each save, at the
  mission's close and every 10 s of play).
- **The profile's progress** is the game's own, as its `.sav` keeps it: the chapter of its newest story save, its
  saves (slot `n` named by the game's text 5000 + `n`, "05-03 Fairplay - The Racing Circuit"), the freeride's level
  word, the car groups unlocked for racing, for stealing and as the freeride's bonus cars (bit `n`: the letter
  `'A' + n`), and the garage's cars (the car database's indices).
- `call stats.state`, `profile.progress` (the letters, the garage's car names, the saves' names).

### The scene's lights

```c
MmlLight lights[256];
int n = H->scene_lights(M, lights, 256);         /* name, type, on, color, power, range, cone, pos, dir, sectors */
for (int i = 0; i < n && i < 256; i++)
    if (lights[i].type == MML_LIGHT_DIRECTIONAL) {
        const float warm[3] = {1.0f, 0.6f, 0.4f};
        H->light_set(M, lights[i].frame, warm, lights[i].power * 0.6f, NAN, NAN, -1);   /* NAN, -1: kept */
    }
float toSun[3], sunColor[3], ambient[3];
H->scene_sun(M, toSun, sunColor, ambient);       /* the level's sun and the ambient light beside it */
```

- **The lights are the level's own light frames** (LS3D's `I3D_light`: point, spot, directional, ambient, fog);
  the freeride's city has about 40. A change goes through LS3D's own setters. In the labs a change of the sun and
  the ambient light changed the whole picture (the buildings, the sky, the people and cars), at once.
- The list is found once a mission (the walk over every frame of the scene: about 13 ms, so not each frame);
  lights that come later (a car's lamps) are found with `call lights.list {"rescan": true}`. A mod that changed
  lights puts them back when it unloads; a new mission brings its own.
- The graphics bridge's constants (`g_sun`, `g_sunColor`, `g_ambient`) take the scene's sun and ambient light
  when the frame has no Direct3D light (`gfx.state` `sun_from`).
- `call lights.list {"type": "directional", "all": true, "max", "rescan"}` (the lights that are on; `all`: every
  one; and the sun), `light.set {"light", "color", "power", "range": [near, far], "on"}`.
- `mods/examples/dusk` (not enabled by default): the sun and the ambient lights tinted towards a dusk, put back
  when it is turned off or unloaded.

### The game's effects: the car exhaust, particles.def, the particles, the weather

```c
int OnExhaust(const MmlExhaust *e, void *user) {   /* every tick, every car whose exhaust the game is about to make */
    /* e->pipes, e->pos[i], e->dir[i] (where it blows), e->carry[i] (the car's velocity there), e->jet_speed,
       e->emitting (the game puffs now), e->type (its particles.def record), e->dt_ms */
    return 1;                                       /* the game's puffs for this car left out: the mod makes its own */
}
H->exhaust_hook(M, OnExhaust, NULL);
MmlParticleDef d;
H->particle_def(M, 12, &d);                         /* the ordinary cars' exhaust: model, rate, life, growth, tints */
d.tint_birth[0] = 1; d.tint_birth[1] = d.tint_birth[2] = 0;
H->particle_def_set(M, 12, &d);                     /* in MML_EVENT_MISSION_OPEN: the game's emitters take it then */
```

- **The exhaust** is the game's own `C_Vehicle::UpdateExhaustParticles`, read as the game reads it: each pipe (the
  model's EXHAUST dummies) in the world, the way it blows, the jet's speed by the engine's turns, the car's velocity
  at the pipe, what the emitter adds to a puff's velocity at its birth and death. A smoke mod takes these as its
  sources; a mod that returns 1 replaces the game's puffs for that car. `car_exhaust` reads one car's at any time.
- **`tables\particles.def`**: 23 records in both builds (the exhausts are 12 and 15; dust, grass, stones, leaves,
  smoke). The game hands a record to its emitter when it makes it, at a mission's load: a change made in
  `MML_EVENT_MISSION_OPEN` shows in that mission (seen: the taxi's exhaust red), one made later in the next.
- `call exhaust.state {"car"}` (the hook, how many cars it was told of and skipped, a car's exhaust),
  `particles.defs {"index"}`, `particles.def.set {"index", "tint_birth": [r, g, b], ...}`.
- **The particles**: every particle the game shows is an LS3D particle visual, whichever system makes it: the
  emitters' puffs (`MML_PARTICLE_EMITTED`: the exhaust, dust, grass) and the game's pooled effects
  (`MML_PARTICLE_POOLED`: fire, steam, vents, leaves). `particle_hook`'s function is told of each move, after the game
  made it: where it is (`pos`), how far the game moved it (`moved`, its own motion), since when (`dt_ms`), whether
  it is the first move of its life; change `pos` and return 1 and the particle is drawn there and goes on from
  there. A wind, PhysX smoke moving the game's own puffs (mafia-ragdoll's), a counter. The host patches two slots of
  the particle visuals' vtables (Update, SetParent): only particles pay. In the freeride's city: some 10 000 moves
  a second.
- `call particles.state {"patch": true}` (moves counted by kind without a mod, a few tracked particles' places).
- **The weather** is LS3D's weather system of the mission's scene (the rain's drops), what the scripts'
  `weather_setparam` changes: `weather_get` / `weather_set` with `MML_WEATHER_*` (on, the drops' colours, speed,
  length, width, how far and how high they are made, how many, the way they fall), values as doubles (a flag 0/1,
  a count, a colour 0xAARRGGBB, a float); `weather_reset` the scene's preset (`on` is left as it is). As the scripts
  do, `on` and `max_count` are applied again for the player's detail setting (fewer drops at a low one). The fog
  and the sky are not the weather: the fog is the scene's (the graphics bridge's `g_fog`), the sky a model.
- `call weather.state`, `weather.set {"on": true, "max_count": 20000, "speed": 30}`, `weather.reset`.
- `mods/examples/exhaust_fx` (not enabled by default): the game's exhaust replaced by the mod's own puffs (drawn as
  squares over the frame: an example of the data, not of drawing smoke), or with `game_tint` the game's own tinted.
  `mods/examples/wind` (not enabled by default): every particle of the game blown by a gusty wind.
  `mods/examples/storm` (not enabled by default): rain over the city slanted by a turning wind, the scene's own
  weather put back when it is off or unloaded.

### Props: models that are not actors

```c
MmlBone b[MML_BONE_COUNT];
H->human_skeleton(M, H->human_player(M), b, MML_BONE_COUNT);
const float pos[3] = {0, -0.05f, -0.2f};                    /* in the bone's space: y up the spine, z out of the chest */
const float rot[4] = {0.9848078f, 0, 0, 0.1736482f};        /* w x y z: 20 degrees about z */
void *bat = H->prop_create(M, "2bbat1.i3d", b[MML_BONE_BACK3].frame, pos, rot, 1);   /* on his back */
const float at[3] = {-1984.5f, -3.3f, 19.4f};
void *stone = H->prop_create(M, "2stone.i3d", NULL, at, NULL, 0.25f);                 /* in the world, small */
H->prop_place(M, stone, at, NULL, 0);                       /* moved (each frame if it must: cheap) */
H->prop_attach(M, bat, NULL, 1);                            /* off his back, left in the world where it is */
float w[16];
H->frame_world(M, H->actor_frame(M, H->human_player(M)), w); /* any frame's world matrix: rows 0..2 axes, 3 position */
```

- **A prop is an LS3D model frame**, made as the game makes an actor's model (the I3D driver's frame, the file opened
  by the game's model cache: the game's `models\` and its archives, or a mod's own `files\models\`, which the cache
  opens as any other file), with no actor on it: nothing the game's logic sees, and nothing collides with it until
  it is made physical (below). In the world it is linked into the room (sector) at its
  place, as a spawned actor's is, again each time `prop_place` moves it; attached, it is a child of the frame (a bone
  from `human_skeleton`, an actor's model from `actor_frame`, a car's part found under it, another prop) and follows
  its every move: a bat on a person's back follows his animation, his fall, his arrest.
- `pos`, `rot` (w x y z) and `scale` are in the parent's space (the world's without one); NULL / 0 keep what it has.
  `prop_attach` with `keep_world` 1 keeps it where it is in the world (its local place computed anew); 0 keeps its
  local place in the new parent. A model's own axes are its file's: the bat lies along its y.
- **Who removes it**: the mod with `prop_remove`; the host at the mod's unload (so a hot reload starts clean), and
  every prop when the mission closes (a mod makes them again at `MML_EVENT_MISSION_LOADED`). A prop on an actor that
  is destroyed is put into the world where it was; one on another prop that is removed, too. A game over's retry
  (the same mission, the player the same actor) keeps them.
- **Its animation**: `prop_anim_play(M, prop, "walk1", MML_PROP_ANIM_LOOP, 1)` plays a file of `anims\` (the game's
  `.5ds`, named `.i3d` as the scripts name them; a mod's own `files\anims\`) on the prop's model as the scripts'
  `model_playanim` does: LS3D's SetAnimation, the model on the mission's list of animated models, stepped by its
  tick (it stands while the game is paused). An animation moves the model's frames that have its tracks' names: a
  person's animation on a person's model (a prop of `Tommy.i3d` walks in place), a door's or a crane's on its model.
  Returns its length in ms (0: no such file). Once, it stops at its end in its last pose; `prop_anim_stop` leaves the
  pose it reached; `prop_anim_state` says where it is.
- **Its physics**: `prop_physics(M, prop, &(MmlPropPhysics){.mass = 2})` makes a prop in the world a physical object
  as the scripts' `create_physicalobject` makes one (C_public_physics: a box from its model's bounds put among the
  collision's moving objects): a car is stopped by it, it falls when pushed or given a `velocity`, and it lies still
  (in the air too) until something wakes it: `prop_push(M, prop, impulse, at)` (kg m/s at a point of the world;
  NULL its centre). `prop_physics(M, prop, NULL)` leaves it where it lies, an ordinary prop again. A physical prop
  moved by `prop_place` is made again at rest at its new place at the next frame (the game deletes its old object
  at its next tick); attached to a frame, it is no longer physical. `prop_body` gives its object (an actor).
- `call props.list`, `prop.create {"model", "at": [x, y, z] | "player", "ahead", "up", "parent": "player" | "car" |
  "0x...", "bone": "r_hand", "frame", "pos", "rot" | "dir", "scale"}`, `prop.place`, `prop.attach {"prop", "parent",
  "keep"}`, `prop.show {"prop", "on"}`, `prop.remove {"prop" | "all"}`, `prop.anim {"prop", "play", "loop", "weight"
  | "stop": true}`, `prop.physics {"prop", "on", "mass", "velocity"}`, `prop.push {"prop", "impulse", "at"}`.
- `mods/examples/props_demo` (not enabled by default): a baseball bat on the player's back, stones circling his head
  (placed each frame around his neck bone); `props_demo.drop` leaves the bat in the air, `props_demo.wear` puts it
  back.

### Sound and music

```c
int ui = H->sound_play(M, "menuopen.wav", NULL, NULL, 1, 1, 0, 0, 0);          /* 2D: heard the same everywhere */
const float at[3] = {-1940, -3, 25};
int bell = H->sound_play(M, "sounds\\x.wav", NULL, at, 1, 1, 5, 40, 0);       /* 3D at a place: full to 5 m, none past 40 */
int note = H->sound_play(M, "th_1.wav", car_frame, NULL, 0.8f, 0.6f, 4, 60, MML_SOUND_LOOP);   /* on a car, looped */
H->sound_set(M, note, NAN, 0.6f + 1.2f * rpm_share);                          /* the pitch follows the engine */
int track = H->music_play(M, "13 - Game Over.ogg", 1, MML_SOUND_LOOP);       /* sounds\music\..., streamed */
H->music_game(M, 0);                                                          /* the city's own music faded out */
```

- **A sound is LS3D's own sound frame** (I3D_sound), opened and played as the game plays its own: the file from the
  game's `sounds\` and archives, or a mod's `files\sounds\` (a name without a folder is put under `sounds\`, one
  without an extension gets `.wav`). In 2D it is what the menus' own sounds are (it plays in a menu too); in 3D it
  is placed in the mission's world (a place, or a frame it follows: a bone, `actor_frame`'s model, a car, a prop),
  heard by the game's listener with its range (full volume to near, nothing past far). The game's own engine sounds
  are such sounds: `th_1.wav` is the Bolt-Thrower's.
- `volume` is 0..1, `pitch` a factor of the file's own rate (seen at the DirectSound buffer: 22050 Hz times 0.5 and
  1.5), both changed live with `sound_set`. A sound that is not looped ends by itself (its id is then unknown).
- **The pause**: LS3D holds the world's (3D) sounds itself while the game is paused and goes on with them after; a
  2D sound plays on (a menu's sound), unless `MML_SOUND_PAUSABLE` holds it too.
- **Who stops them**: the mod (`sound_stop`, `music_stop`); the host at the mod's unload (the city's music given
  back too), and the 3D sounds when the mission closes (a mod plays them again after `MML_EVENT_MISSION_LOADED`).
- **Music** is the game's own streaming (LS3D's IShow: .ogg, .mp3), in a menu too, its volume times the options'
  music volume (as the game's). `.mp3` and `.ogg` are tried for each other, as the game does (the scripts name
  "13 - Game Over.mp3"; the archives have the .ogg). A looped track starts again at its end, another is released.
- `call sounds.list`, `sound.play {"file", "at" | "on": "car", "bone", "pos", "volume", "pitch", "range", "loop"}`,
  `sound.set {"sound", "volume", "pitch", "at"...}`, `sound.stop {"sound" | "all"}`, `music.play {"file", "volume",
  "loop"}`, `music.set`, `music.stop`, `music.game {"on": false}`.
- `mods/examples/engine_note` (not enabled by default): a second engine voice on the player's car, its pitch
  following the revolutions; F9 turns it off; `engine_note.music {"file": "13 - Game Over.ogg"}` plays a track in
  place of the city's music.

### Control: keys kept from the game, the player locked, a car's physics replaced

```c
int up = H->key_code("Up");
H->key_capture(M, up, 1);                        /* the game reads Up as up; the mod still sees it (key_down) */
int accel = H->action_slot("Accelerate");
H->action_capture(M, accel, 1);                  /* Accelerate (both bindings) reads 0 for the game ... */
float pedal = H->action_held(accel);             /* ... and what the player does for the mod */
H->player_lock(M, 1);                            /* he stands; the mod moves him (actor_teleport, poses) */

int OnStep(MmlVehicleStep *s, void *user) {      /* every car's physics step (C_Vehicle::Engine) */
    if (!s->player) return 0;                    /* the game steps the others */
    s->body.pos[1] += 0.5f * s->dt;              /* the mod's own flight: position, axes, velocities */
    return 1;                                    /* the game's step skipped: this body is the car's */
}
H->vehicle_hook(M, OnStep, NULL);
```

- **Keys and actions kept from the game**: `key_capture` clears the key in LS3D's keyboard state after mods have
  read it (every reader of the game sees it up: the controls, the menus, cheats); `action_capture` writes 0 into an
  action's slots after the game's update (and after the agents' `input`), keeping what the player did for
  `action_held` / `action_pressed`. An action's two bindings are one action to the game: on foot either at 0.25 or
  more; driving the primary while it is not 0 (ramped by the car), else the secondary (not ramped: a joystick's
  axis) (knowledge/notes/game.md, "Controls and input"). Both go back at the mod's unload.
- **The player locked**: the scripts' own PLAYER_LOCKCONTROLS (`C_player::LockControls`): he stands where he is,
  walking, shooting and driving leave him alone; the controls are still read. Given back at the mod's unload; a new
  mission starts free.
- **A car's physics replaced**: the function is told of every car's step with its body (`pos`, `axes` forward,
  right, up, `velocity`, `turn`) and the step's time; returning 1 the game's step is skipped and the host makes the
  car what the body says: the body's fields, its matrices (the one the exhaust and others read), the model frame
  (placed as the game's Move places it), the car kept awake (Tick's rest counters started again, so it falls
  under the game's physics at once when the mod lets it go). One mod at a time. `vehicle_body` reads or writes a
  car's body at any time.
- **An airplane flown by a mod**: the game's airplane (`C_airplane`: the freeride Extreme's plane over the city and
  its flying saucer, the planes of two missions) has no rigid body: its AI flies a route. `H->vehicle_kinds(M,
  MML_VEHICLES_CARS | MML_VEHICLES_AIRPLANES)` and the same function is told of each airplane's AI step too
  (`s->kind == MML_VEHICLE_AIRPLANE`, `s->player` 0); returning 1 the route's flight is left out and the body is the
  plane's: its place, its rotation (the model's axes), its speed `|velocity|` along the velocity's direction (the
  propellers' blur and the engine's sound follow the speed); its collision moves with it, its propellers, sounds and
  damage gauge go on. The velocity and turn the mod wrote come back in the next step's body (the plane keeps only a
  speed). Let go (return 0), it turns back to its route from where it is. Find one with `call actors` (type 22).
- `call input.capture {"key": "W"}` / `{"action": "Accelerate"}` / `{"all": true, "on": false}`, `player.lock
  {"on"}`, `vehicle.body {"car", "pos", "velocity", "turn"}` (a car or an airplane), `vehicle.move {"car",
  "velocity", "turn", "ms"}` (the host moves a car or an airplane by them, the game's physics or flight left out:
  to try a vehicle's surroundings, or fly the plane somewhere), `control.state`.
- `mods/examples/hover` (not enabled by default): F8 and the player's car floats `height` metres over the ground
  under it (the game's own ray), Accelerate and Brake push it, the steering turns it; F8 again and it drops back.

### Interaction: zones, use-points, timers

```c
void OnZone(int zone, int inside, void *actor, void *user) { /* 1 entered, 0 left: the player (on foot or in his car) */ }
void OnUse(int point, void *user) { /* he took it (Action, or its line in the game's choice menu) */ }
void OnTimer(int timer, void *user) { /* the mission's time went by */ }
const float dock[3] = {-1598.5f, -6.2f, -30.8f};
H->zone_add(M, dock, 10, NULL, OnZone, NULL);
H->use_point_add(M, dock, 2, NULL, "Fish here", OnUse, NULL);   /* a mission must be loaded */
H->timer_add(M, 5000, 0, OnTimer, NULL);          /* once, 5 s of the mission's clock (the pause stands it still) */
```

- **Zones** are spheres in the world, or following a frame (`frame`: a bone, an actor's model, a prop; `pos` in
  its space); the player is in one when his model is (in a car: the car's place). `zone_watch(M, zone,
  MML_ZONE_PEOPLE | MML_ZONE_CARS, NULL)` makes one watch every person the loader knows (the player too) and every
  car, or `MML_ZONE_ACTOR` one actor: its function is told each one's crossing with that actor; an actor destroyed
  inside leaves without a crossing. `call zone.add {"who": "people" | "cars" | "all" | "car" | "0x..."}`.
- **Use-points** are **the game's own using objects**, what `Action` finds on foot (doors, the cars' seats, a weapon
  on the ground): within `radius` (horizontal; 1.5 m up or down) the game offers it as it offers a door. Alone, his
  `Action` takes it at once; with others near, the game shows its own choice menu ("Pickup/Use": "1. Get in the
  front", "2. Fish here", "0. Exit"), the label being its line (the loader gives the text an id in the game's text
  database). The mod's function is called when he takes it; the game does nothing else for it, and what is beside
  it stays the game's (the car is still entered from the same menu). On a frame (`frame`: an actor's model, a bone,
  a prop) the point follows it, and goes when that actor is destroyed.
- **Timers** run on the mission's clock (`MML_EVENT_MISSION_TICK`'s time): they stand while the game is paused or
  loads. `repeat` calls again every `ms`.
- `interact_remove` takes any of them; a mod's go with its unload, every one with the mission.
- `call interact.list`, `zone.add {"at" | "on", "bone", "pos", "radius"}`, `use.add {..., "label"}`, `timer.add
  {"ms", "repeat"}`, `interact.remove {"id" | "all"}`; the crossings, uses and timers of the bridge's go to
  `events.since` (`ZONE_ENTERED`, `ZONE_LEFT`, `USED`, `TIMER`) and MCP `wait`.

### The HUD: pictures, the game's messages, markers

```c
int fish = H->picture_load(M, "fish.tga", NULL);       /* the mod's files\Maps\fish.tga (or the game's maps\) */
/* every frame (MML_EVENT_FRAME), as draw_rect: */
H->draw_picture(M, fish, 700, 20, 64, 32, 0, 0xFFFFFFFF, NULL);   /* x, y, w, h, angle, tint, uv */

H->hud_console(M, "Caught a trout", 0x80FF80);         /* a line of the game's console */
H->hud_help(M, "Find a quiet spot by the river");      /* a line of its help */

MmlHudMarker m = {sizeof m};
m.pos[0] = -1598.5f; m.pos[1] = -6.2f; m.pos[2] = -30.8f;
m.flags = MML_MARKER_SCREEN | MML_MARKER_DISTANCE | MML_MARKER_EDGE | MML_MARKER_COMPASS;
m.argb = 0xFF40C0FF;
m.label = "Fishing spot";
int spot = H->hud_marker_add(M, &m);                   /* hud_marker_set to change it, hud_marker_remove */
```

- **Pictures** are opened by the game's own texture loader (`IGraph::CreateITexture`), as its HUD opens its own:
  a file of `maps\` (the archives', or the mod's `files\Maps\`): a `.tga` with its alpha channel, a `.bmp` opaque
  or with its alpha in a twin (`"icon.bmp"`, `"icon+.bmp"`: white shows, black is clear). `picture_size` gives its
  pixels. `draw_picture` draws it as `draw_rect` draws a rectangle (each frame it is to be seen, in 800x600 units),
  turned about its centre (`angle`, degrees clockwise: a needle, an instrument), tinted (`argb` multiplies it),
  and `uv` picks a part of it (a sheet of icons). A picture stays until `picture_free` or the mod's unload.
- **Messages in the game's style**: `hud_console` adds a line to the game's console (bottom left, five lines, each
  for 5 s, at most 63 characters; its colour is `argb`'s RGB); `hud_help` shows a line of the game's help (the
  mission's hints, in the middle above the crosshair) and returns how long it stays.
- **Markers** are places shown by the HUD: on the screen (`MML_MARKER_SCREEN`: the picture, or a diamond of its
  colour, and the label under it; `MML_MARKER_DISTANCE` adds "35 m" from the player; `MML_MARKER_EDGE` keeps it at
  the screen's edge, toward it, when it is off the screen or behind), on the game's radar (`MML_MARKER_RADAR`) and
  as the compass' target (`MML_MARKER_COMPASS`: the game's compass is shown and points at it, as a mission's
  `setcompass` does; one target: the newest marker's, and when the last of them goes the compass goes back to what
  the mission had). A marker is at a place in the world, or follows a frame (`frame`: an actor's model from
  `actor_frame`, a bone, a prop; `pos` in its space; it goes when the actor is destroyed). Markers are drawn while
  the mission runs (not under the pause menu); a mod's go with its unload, every one with the mission.
- `call draw.picture {"file", "alpha", "x", "y", "w", "h", "angle", "color", "uv", "ms"}`, `hud.console {"text",
  "color"}`, `hud.help {"text"}`, `hud.marker {"at" | "on", "bone", "pos", "label", "color", "picture", "scale",
  "screen", "distance", "edge", "radar", "compass"}` (`"id"` changes one), `hud.remove {"id" | "all"}`, `hud.list`.

### A mod's state in the saves

```c
void OnEvent(const MmlEvent *ev, void *user) {
    if (ev->type == MML_EVENT_GAME_SAVING)                      /* the game writes a save now */
        H->save_put(M, MML_SAVE_SLOT, "fish", &g_fish, sizeof g_fish);
    if (ev->type == MML_EVENT_GAME_LOADED) {                    /* a save loaded (or a new game: slot -1) */
        g_fish = 0;
        H->save_get(M, MML_SAVE_SLOT, "fish", &g_fish, sizeof g_fish);
    }
}
H->subscribe(M, MML_EVENT_GAME_SAVING, OnEvent, NULL);
H->subscribe(M, MML_EVENT_GAME_LOADED, OnEvent, NULL);
H->save_put(M, MML_SAVE_PROFILE, "record", &best, sizeof best);   /* the profile's, written at once */
```

- **`MML_SAVE_SLOT`** is the state of the game being played. The loader writes it beside every save the game writes
  (`savegame\mafia<profile>.<slot>.mml`) and reads it back when the game loads that save, whatever loads it (the
  load menu, a retry, the pause menu's "Load Game"). A new game and every freeride start empty. Put the state at
  any time, or in `MML_EVENT_GAME_SAVING`; read it in `MML_EVENT_GAME_LOADED` (`slot` and `profile` in the event).
- **`MML_SAVE_PROFILE`** follows the profile across its saves (records, unlocks): written to
  `savegame\mafia<profile>.sav.mml` as soon as it is put.
- Keys are the mod's own (kept under its id: a reload keeps them); `save_put` with no data removes a key. The
  game's own files are never changed: a save made without mods has no state, a `.mml` is never read without its save.
  The game deleting a profile deletes its `.mml` files too.
- `call save.state` (the profile, the slot, every key), `save.get {"mod", "key", "scope"}`, `save.put {"key",
  "value", "scope"}` (the bridge's own keys); `GAME_SAVING` and `GAME_LOADED` go to `events.since` and MCP `wait`.

### What happens (shots, hits, crashes, falls, deaths, vehicles, items, money)

| Event | When | `actor` | more |
|---|---|---|---|
| `MML_EVENT_SHOT` | a shot goes into the game's shot queue (every weapon) | the shooter | `shot`: `pos`, `dir` (its length is the weapon's reach, about 120 m for a Thompson), `damage`, `shots` (pellets) |
| `MML_EVENT_HUMAN_HIT` | the game hits a person (a bullet, a car, an explosion, a fall) | him | `hit`: type, damage, attacker, alive before and after |
| `MML_EVENT_EXPLOSION` | an explosion (grenades, cars, the scripts' `explosion`) | its source or NULL | `explosion`: `pos`, `radius`, `damage` (sent after the game applied it: its hits come first) |
| `MML_EVENT_CAR_EXPLODED` | a car blows up | the car | |
| `MML_EVENT_HUMAN_DIED` | a person's life goes out, however (a hit, a fall, the water, a script) | him | `other`: the last who hit him in the 3 s before, or NULL |
| `MML_EVENT_VEHICLE_ENTERED`, `_LEFT` | a person starts or stops using a vehicle (a car, a tram) | him | `other`: the vehicle |
| `MML_EVENT_PEDESTRIAN_HIT` | the game hits a traffic pedestrian (the city's walkers are the traffic system's elements, not actors) | its element | `hit` (`pos` where it stands, alive by its health), `other`: its model frame |
| `MML_EVENT_BULLET_HIT` | a bullet strikes something that is not a person (people: `HUMAN_HIT`, `PEDESTRIAN_HIT`): the world, a car, a door | the shooter | `bullet`: `pos`, `normal`, `dir`, `damage`, `actor` what it struck (NULL: the static world), `flags` (as `MmlHit`'s: the world's surface in the top byte) |
| `MML_EVENT_CAR_CRASH` | a car strikes the world or another car hard enough to change its speed by 1 m/s (each car of two is told) | the car | `other`: the car struck (NULL: the world); `crash`: `pos`, `impulse`, `speed` (that change), `flags` |
| `MML_EVENT_HUMAN_FELL` | a person lands after a fall (a jump is an animation of the game's, not a fall) | him | `fall`: `speed` (the game hurts from 8 m/s, with a `HUMAN_HIT` of type 6), `height` (from the highest point) |
| `MML_EVENT_ITEM_GAINED`, `_LOST` | an item comes into a person's inventory or leaves it, however (picked up, given, bought; dropped, thrown, taken); rounds gained for an item he has | him | `item`: `item` (its id, `world.items`), `count` (+1, -1; 0: only rounds), `rounds` |
| `MML_EVENT_MONEY_CHANGED` | the freeride's money changes | | `value` the money now, `change` |

A car striking a person is that person's `HUMAN_HIT` (hit type 5 or 8, the car its attacker); a car pressed on a
wall touches it every frame, hence the 1 m/s (the game's own sparks begin at 5, its debris at 6). Deaths, vehicles,
landings, items and the money are read every frame from each person the loader knows and from the game (a frame
late at most); shots, explosions, bullets and crashes are the game's own functions hooked. Every one of them also reaches agents (`events.since`, MCP
`wait {"types": "HUMAN_DIED"}`). `mods/examples/tally` (not enabled by default) counts the player's shots and
kills, deaths, explosions, cars blown up and rides in a panel, the base an achievements mod stands on.

### People, skeletons, poses

```c
void *list[512];
int n = H->humans_list(M, list, 512);           /* every C_human made and not destroyed; the player: human_player */
MmlBone b[MML_BONE_COUNT];
if (H->human_skeleton(M, H->human_player(M), b, MML_BONE_COUNT)) {
    const float out[4] = {1, 0, 0, 0};           /* w x y z: the identity, the bone along its parent */
    H->bone_set(M, b[MML_BONE_R_ARM].frame, out, NULL);   /* in MML_EVENT_RENDER_BEGIN, every frame */
}
```

- **The skeleton** is the 18 joint frames of the game's human models found by name (`back1`..`back3`, `neck`,
  `r_shoulder`, `r_arm`, `r_elbow`, `r_hand`, the left ones, `r_thigh`, `r_shin`, `r_foot`, the left ones; the head
  moves with the neck), the pelvis (the thighs' parent) and the model's root, in `MML_BONE_*` order: each one's
  frame, world matrix, local rotation (w x y z) and position. An arm bone's x axis runs along it; the identity puts
  a bone along its parent (both arms: a T-pose).
- **Poses**: the game's animation sets the bones in its tick (`C_human::Update` runs LS3D's animation, found with
  `watch`), so a pose written in `RENDER_BEGIN` is what this frame draws, the screen and the scene views alike;
  the next tick sets the animation again (write it every frame). The loader updates each posed model once after
  `RENDER_BEGIN`'s mods. The game's logic, collision and aim stay the animation's.
- `call humans.list` (actor, model, position, health, alive, the player, a vehicle), `human.skeleton {"actor"}`,
  `human.pose {"actor", "bones": {"r_arm": {"rot": [w, x, y, z], "pos": [x, y, z]}}, "ms"}` (held for `ms`, drawn
  every frame: an agent's test of a pose).
- `mods/examples/puppet` (not enabled by default): the player waves (`"wave"`) or holds a T-pose (`"tpose"`), any
  person with `puppet.target {"actor"}`.

### Animations, getting up

```c
int ms = H->human_anim_play(M, paulie, "x panika drep stativ", 0);   /* anims\...: its length, 0 no such file */
H->human_anim_stop(M, paulie);                                       /* his own again at once */
H->human_anim_replace(M, H->human_player(M), "breath05a.i3d", "x panika drep stativ.i3d");  /* NULL: his own */
MmlHumanAnim s = {sizeof s};
H->human_anim_state(M, paulie, &s);       /* s.time_ms, s.length_ms, s.lying, s.state, s.playing, s.name */
H->human_get_up(M, paulie, 600);          /* a body the mod posed handed back to the game */
```

- **Playing** is the game's own way, as a mission plays an animation (`person_playanim`): a file of `anims\` (the
  archives', or the mod's `files\anims\`; with or without `.i3d`), cross-faded from what he does; at its end he
  goes back to his own life (an NPC to his AI). `MML_ANIM_SIMPLE` is the scripts' `SIMPLE`. The game's own
  animations are named in its table: `call human.anims {"query": "panika"}` lists them from the running game
  (walking, running, panics, deaths, fights, getting into cars...).
- **Replacing** one of his own animations (`human_changeanim`): the game plays the other wherever it would play
  his (his standing, walking...), until it is given back (NULL), the mod unloads or he is destroyed.
- **The state**: `time_ms` and `length_ms` are his animations machine's active animation (the one played, or his
  own); `lying` 1 on the back, -1 on the front (his front from his shoulders, hips and spine, as last drawn);
  `state` 1 his own life, 6 his action queue (a knock-down, a get-up).
- **Getting up**: a mod that posed a body (a ragdoll) stops posing it and calls `human_get_up`. The loader takes
  the pose last drawn, puts the person where the body lies (the pelvis over the ground) and turned the way it lies,
  and queues the game's own get-up from the back or the front, as a knock-down queues it; for `blend_ms` the host
  draws each bone from the body's pose into the animation's. He then lives on as the game's again.
- `call human.anim {"actor", "play" | "stop" | "replace" with "with"}`, `human.anim_state {"actor"}`,
  `human.get_up {"actor", "blend_ms"}`.

### People's skinning (more than two bones a vertex)

LS3D skins a person on the CPU with two bones a vertex at most (the .4ds holds no more): a joint bent far (a knee,
an elbow, a ragdoll's limbs) folds the mesh along a hard crease. A mod gives a model's single mesh a skin of its own:
up to 8 bones a vertex, any joint under the mesh (not only the 64 LS3D binds), each bone with its own matrix at rest.
Every person of that model takes it from the next frame; every other model keeps LS3D's own code. The host skins into
LS3D's own cache of the mesh, so the game's lighting, its drawing and whatever reads the cache take the result as
they took LS3D's (both builds, any renderer, plain Direct3D 8 too).

Without code: a file `models\<model>.skin.json` among the mod's files (beside `models\Tommy.4ds`: the .4ds stays as
it is, readable by every tool):

```json
{"format": "mml-skin", "version": 1, "model": "models\\tommy.4ds",
 "meshes": [{"mesh": "base", "lods": [
   {"vertices": 778,
    "bones": [{"joint": ""}, {"joint": "r_thigh", "bind": [16 numbers]}, ...],
    "influences": [[1, 0.6, 2, 0.3, 3, 0.1], ...]}]}]}
```

- `mesh`: the single mesh's name in the .4ds (`call model.info {"file": "Tommy"}` lists the nodes; people's is `base`).
  `lods`: LOD 0 the finest; a LOD not given keeps LS3D's skin.
- `vertices`: the LOD's count, in the .4ds's order (checked against the game's mesh: a mismatch keeps LS3D's skin and
  says so in the log and `skin.state`).
- `bones`: `joint` a joint's name in the .4ds (`""`: the mesh's own space, the bone that never moves); `bind` the
  mesh's space at rest to the joint's (row vectors: a point times it), as the .4ds's own bone records hold it.
- `influences`: a list a vertex of bone, weight pairs, 8 at most (the weights are made to sum to 1). Only the LOD's
  last vertices, the ones the .4ds weights by no bone, may have none (they stay where they are).
- The normals go through each bone's matrix with its columns normalised, as LS3D's own do.

**A starting point**: `call skin.export {"model": "Tommy"}` writes LS3D's own skin of a model in this form, with its
rest mesh (`"rest"`: positions, normals, uvs, triangles) to `<game>\mml\skin\tommy.skin.json`; given back unchanged
(`skin.set {"file"}`, then `skin.check {"mesh"}`) it comes out as LS3D's to the float's last bit. An exporter writes
the file from its own weights (Blender's painted ones); `python tools/skin_smooth.py in.skin.json out.skin.json
--rings 3 --max 4` spreads an exported skin over more bones (each vertex's weights averaged with its neighbours'
along the mesh, the uv map's seams kept closed): a joint's crease becomes a band of 3 or 4 bones.

From code (the arrays the mod's; LOD `i` of `lods` is the mesh's LOD `i`; `count` 0 takes the mod's skin away):

```c
MmlSkinMesh m;
if (H->skin_get(M, "Tommy", "base", 0, &m)) {   /* LS3D's own, from the .4ds: bone 0 the mesh's space, then its records */
    /* m.skin: bones, count, index, weight (8 a vertex); m.pos, m.normal, m.uv, m.index (triangles): the rest mesh */
    ... change m.skin's weights (copy the arrays first: the host's until the next skin_get) ...
    char err[256];
    H->skin_set(M, "Tommy", "base", &mine, 1, err, sizeof err);
}
```

- A mod's skin replaces a file's for the same mesh while the mod holds it; the bridge's (`skin.set`) replaces both
  until `skin.reload`. A mod's go at its unload.
- `call skin.state` (the skins, the files read and their errors, the meshes bound, the time), `skin.export`,
  `skin.set {"file"}` or `{"model", "mesh", "lods"}`, `skin.check {"mesh", "lod"}`, `skin.reload`.
- Cost (Tommy, 778 vertices, measured): LS3D's own two bones 0.017 ms a skinning, the host's with the same weights
  0.023 ms, with 4 bones a vertex 0.04 ms.

**On the GPU** (`H->skin_gpu(M, 1)`, `call skin.gpu {"on": true}`; under a Direct3D 9 wrapper with shader model 3:
dxwrapper, DXVK): every person's draw (LS3D's single meshes, a mod's skin or LS3D's own) goes through a vertex shader
that skins his rest pose (the face's morph in it) with his bones' palette and lights him as LS3D's CPU did: LS3D's own
`LightVertices` still chooses his 8 lights, run on one vertex, and its choice becomes the shader's constants; the
same 128 scale, the material's alpha and the fading, the environment map's uv (modes 0 and 1). The picture is the
same (measured: 0.16 of 255 on average over a person). LS3D's skinning into the cache, its copies into the drawn
buffer and its lighting of the vertices are skipped. What a mod gets:

- A rule's pixel shader (shader_add) on a person's draw reads the world's normal and MikkTSpace tangent (Blender's and
  the bakers' convention) besides reference.hlsl's outputs: `float3 normal : TEXCOORD2; float4 tangent : TEXCOORD3`
  (w the bitangent's sign). Normal maps and per-pixel light on people.
- With `MML_SHADE_SKINNED` in the rule's flags (`"skinned": true` from the bridge) its own vertex shader takes the
  person's draws too: `#include "mml/skin.hlsli"`, `MmlSkinIn` its input, `mml_Skin(i)` the skinned place, normal and
  tangent, `mml_SkinLight(pos, normal)` LS3D's colour, `mml_SkinEnvUv`; host/shaders/skin9.hlsl's VsSkin is the
  reference to copy.
- Motion vectors (gfx_motion) and the sun's shadow map (shader_shadows) take people the GPU skins from the same
  palettes (the last frame's kept for the motion).
- `skin.state`'s `gpu`: `lods` drawn by the GPU, `lods_cpu` and `why_cpu` the ones left to LS3D (an environment map of
  modes 2 to 5, a mesh lit flat), `draws_last_frame`, `ms_last_frame` (the CPU's part), `draw_ms_last_frame`.
- **Cost, measured** (RTX 3070, 48 people in view, about 300 of their draws a frame, one game alone, fps_unlock): the
  CPU's part falls to 0.2 to 0.3 ms, but each of those draws now switches the device from the fixed pipeline to
  shaders and back: 1.2 7.3 ms a frame with LS3D's skinning, 8.2 ms on the GPU; 1.0 7.7 and 9.0. With a rule already
  shading every object (reference.hlsl over all) the two are even (1.0: 11.2 to 11.9 ms against 11.5 to 12.1). It
  does not make the plain game faster: it is for mods that shade people (normals, tangents) and for models of many
  bones or weights, and it stays off until a mod asks.

### Shaders on the game's own drawing (per-object shading)

```c
/* the game's whole world drawn by your shaders: start from the reference, which draws it as the game does */
static const char kSource[] = "#include \"mml/game.hlsli\"\n ...";   /* or read sdk/include/mml/shaders/reference.hlsl */
MmlShaderRule r = {sizeof r};
r.pattern = "*";                         /* the textures' names: "2kolo*.bmp" the wheels, "^*" the lightmapped... */
r.kinds = MML_SHADE_OBJECT | MML_SHADE_LIGHTMAPPED;
r.classes = MML_SHADE_OPAQUE | MML_SHADE_ALPHATEST;
r.source = kSource; r.vs_entry = "mml_vs"; r.ps_entry = "mml_ps";
char err[512];
int h = H->shader_add(M, &r, err, sizeof err);   /* 0: refused, err says why (the compiler's message too) */
```

- **What it is**: item 3's passes see the finished frame; these shaders draw the game's own surfaces. The game draws
  with Direct3D 8's fixed pipeline; under a Direct3D 9 wrapper (dxwrapper in the labs) the host sets a rule's vertex
  and pixel shaders on each draw the rule picks (LS3D's face groups: the texture's name, the kind, the draw's
  blending), with the frame's constants, and puts the game's fixed pipeline back after it. The game's blending,
  alpha test and depth stay its own around them. The last rule added that matches takes a draw; a mod's rules go at
  its unload.
- **The contract**: `sdk/include/mml/shaders/game.hlsli` (`#include "mml/game.hlsli"` in source the host compiles;
  the host keeps its own copy). The matrices (`mml_World`, `mml_View`, `mml_Proj`), the eye and the time, the fog,
  the texture stages' operations, the texture factor; `mml_Tex0`, `mml_Tex1` the textures the game set.
- **What the game hands its draws** (measured, `call shader.survey`, alike in both builds): LS3D lights the
  vertices on the CPU, so a vertex carries its lit colour and **no normal** (FVF 0x142: position, colour, uv; 0x242
  a second uv: a lightmap's, or a car's reflection's); Direct3D's lighting is off. Stage 0 is `modulate2x(texture,
  colour)` (lightmapped: `add`, then `modulate2x(lightmap)`; a car's paint: then `blendfactoralpha(reflection)`
  by the texture factor's alpha); the fog is the vertex fog, linear. `mml_FixedStage` does a stage as Direct3D
  does; `mml_FaceNormal` gives a pixel the face's normal from its world position's derivatives; `mml_FogFactor`.
- **The reference** (`sdk/include/mml/shaders/reference.hlsl`, `mml_vs` and `mml_ps`) draws every kind as the
  game does: switched on for every draw, the frame was the game's (a mean difference of 0.25 of 255 in a JPEG, the
  car's moving exhaust aside, in both labs). Copy it and change it.
- **Compute the position as the game does**: `mul(mul(mul(pos, mml_World), mml_View), mml_Proj)`, not
  `mml_WorldViewProj` in one step. The game's models hide things inside one another: a shop's sign stands just
  behind a wall; from 80 m the depth buffer cannot tell them apart, and only the last bits of the arithmetic decide
  which wins. In the game's order the wall wins (as the game shows it); in another, the sign shows through the wall.
- **The sun's shadow and the game's lights** (`shader_shadows`): a mod that asks for it gets a shadow map from the
  scene's sun drawn before each frame of the mission's world (`MmlShadowSettings`: its size, 2048 texels; its
  extent, 60 m from the camera; the bias, metres along the sun's ray on top of the slope-scaled one the map is drawn
  with), its casters the scene's meshes around the camera and the cars
  near it (block 6d's `scene_pieces` and `actor_meshes`: people are not among them, nor billboards). The rules'
  shaders read it with `mml_Shadow(worldPos)` (1 lit, 0 in the shadow, 1 when there is no map); the sun is
  `mml_SunDir`, `mml_SunColor`, `mml_SunAmbient`. Always there, asked for or not: the game's own point and spot
  lights that reach the drawn visual (up to eight, nearest to its bounding sphere's centre first; a car's
  headlights are spots of its own), `mml_GameLightInfo.x` of them, `mml_GameLight(i, worldPos, normal)` one's light
  at a point as LS3D falls it off. The game's lightmaps already hold the static shadows and its vertex colours the
  lights: what to add, and where, is the shader's choice (a surface facing away from the sun: `dot(normal,
  mml_SunDir)`). `call shader.shadows {"size", "extent", "bias"}` tries it (`"on": false` stops it).
- Shaders compiled in a mod's build: `mml_shaders()` (fxc, `vs_3_0` / `ps_3_0`), the bytecode in `vs_code` /
  `ps_code`; `sdk/include` on fxc's include path (`#include "mml/shaders/game.hlsli"`).
- **Registers of the mod's own** (`H->shader_constants(M, handle, 160, values, count, err, cap)`; handle 0: every
  rule of the mod): c160..c223 in both stages (`mml_Mod[64]` in game.hlsli), set at each draw the rule takes, kept
  until set again: a mod's parameters (a wetness, a tint, a time of its own) without a shader made again.
- **Vertex data** (`MML_SHADE_VERTEX_DATA`): the rule's draws of objects carry a second stream its vertex shader
  adds to its input: `float3 normal : TEXCOORD5` (the mesh's own, at rest, the object's space), `float4 tangent :
  TEXCOORD6` (MikkTSpace's, w the bitangent's sign), `float4 attr : TEXCOORD7` (the mod's: `H->shader_vertex_data(M,
  "TomyhoAuto.Object04", lod, attr, vertices)`, a float4 a vertex in LS3D's order, the .4ds's; 0 where none is given).
  The game's draws come from pools LS3D refills on the CPU; the second stream is a mirror of each pool, a LOD's
  vertices at the same place (written when the place gets a new owner). Not for people (their single meshes: the GPU
  skinning gives them normals and tangents) nor for a lightmapped object drawn from a vertex set of its own (zeros:
  ROADMAP Known gaps). `shader.state`'s `vertex_data` counts them, `why_left_out` says why.
- **People** skinned on the GPU (`skin_gpu`, "People's skinning" above) reach a rule's pixel shader with the world's
  normal (`TEXCOORD2`) and tangent (`TEXCOORD3`); its vertex shader takes them only with `MML_SHADE_SKINNED`
  (`mml/skin.hlsli`).
- **Cost**: each draw a rule takes reads the device's states for the constants (some 60 calls): measured, a rule
  over every object (about 750 draws) cost 1.0 some 4 ms a frame (ROADMAP Known gaps).
- `call shader.add {"source", "vs", "ps", "pattern", "kinds", "classes", "where", "all_passes", "skinned", "vertex_data"}` (an agent
  tries a shader in the running game without building anything), `shader.remove`, `shader.state` (each rule's draws
  in the last frame), `shader.survey`, `shader.constants {"handle", "reg", "values"}`, `shader.vertex_data {"visual", "lod", "values"}`.

### The game's own geometry hidden or replaced (water, by its texture)

LS3D draws its meshes face group by face group (a part of a mesh with one material); the loader stands where it
draws each one, so a mod can leave the game's own geometry out of the picture and draw its own in its place, chosen
by the file name of the group's texture:

```c
int h = H->geom_hide(M, "00test*", 0);          /* the city's river (00TEST01..20.BMP); 0: the frame and the views */
H->geom_capture(M, "00test*", OnWater, NULL);   /* its triangles, the world's space, after the next frame */
void OnWater(const MmlGeomCapture *c, void *user) {
    /* c->tris: c->count triangles (9 floats each); c->groups: which face group each run came from */
}
```

- **Finding the names**: `call geom.survey` (then `geom.survey {"id", "filter": "*test*"}`) lists every texture the
  frame drew with its face groups, faces, the kinds of visual (`OBJ_` objects, `LMAP` lit, `SNGM` / `SMMR` people's
  meshes) and the material's flags (`0x04000000`: an animated texture, matched by the frame it shows now:
  `00test*`). Patterns: case-insensitive, `*` and `?`, the file name without folders.
- **Hiding** touches nothing but LS3D's drawing: the face group's `m_RenderFGroup` is not run (in the frame, the
  scene views or both: `MML_GEOM_FRAME`, `MML_GEOM_VIEWS`); the game's collision, logic and memory are as they were
  (the river still drowns). No pointer into the game is kept between frames: a destroyed object cannot be written to.
- **Capturing** reads what LS3D hands Direct3D for each matching group (its DrawIndexedPrimitive's buffers, base
  vertex and world transform), so it is exact for any kind of visual. Only what the frame drew is in it: ask again as
  the camera moves and keep the groups by `group`. The city's river is one plane of 54 triangles at y -9.99, drawn
  always: one capture is all of it.
- **Replacing**: draw the captured triangles in a pass with your own vertex shader (`mul(float4(p, 1), view x proj)`
  from `MmlGfxFrame::view`, `proj`) and discard where `SceneDepth` is nearer (what stands in front); with the game's
  water hidden, `SceneDepth` and `g_sceneColor` there are the riverbed, seen through your water. A planar reflection
  is a scene view of the camera mirrored in the water's level with that plane as its clip plane.
- `call geom.hide {"pattern", "where"}`, `geom.show {"handle"}`, `geom.state` (rules, groups and faces hidden last
  frame), `geom.capture {"pattern"}` then `{"id"}` (the bounds, triangles by texture, the first few): the same from
  the bridge.
- `mods/examples/river_water` (not enabled by default): the river hidden, captured and drawn anew: waves (each
  fading where it would be shorter than a few pixels), the reflection bent by them, the bed darker the deeper it
  lies, the sun's glint, the fog. In the labs at 1280x960 its reflection (a second drawing of the city, with
  `depth_pulse`'s pass in it) costs about 7.7 ms of CPU.

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

### Orders for people

```c
float to[3] = {-1985.0f, -5.0f, 6.0f};
int id = H->human_order(M, paulie, "enemy_move_to_frame 0, 1, run", NULL, 0, NULL, 0, to, 1);   /* frm[0] at `to` */
void *player[1] = {game::Player()};
H->human_order(M, paulie, "enemy_action_follow 0, 2", player, 1, NULL, 0, NULL, 0);           /* act[0]: him */
MmlOrderState s = {sizeof s};
if (H->human_order_state(M, paulie, &s) && !s.running) { /* done; the bridge also told ORDER_ENDED {id, actor} */ }
```

- A person with a script of his own (a `C_entity`: `actor_spawn`'s people, the missions' NPCs; not the player, not
  the traffic's pedestrians) is told what to do the way a mission tells him: `code` in the missions' language and
  the people's own commands (`enemy_move_to_frame`, `enemy_action_follow`, `enemy_action_fire`,
  `enemy_action_runaway`, `enemy_move_to_car` with `enemy_usecar`, `enemy_stop`...: docs/SCRIPTS.md "Orders") goes
  into his own program, and the game's AI runs it: he walks around what is in the way, runs, shoots with what he
  holds (arm him first: `give_item`), gets into a car. Register numbers: `act[i]` from `acts`, `flt[i]` from
  `flts`, `frm[i]` a frame of the loader's at `points[3*i..]`.
- One order a person: a new one replaces his last (whatever it was doing). The code needs no `end`: the loader ends
  it with a wait of its own (label 60003, keep that number free), where he stands, still ticked, his AI awake
  (shot at, he reacts as the game's people do); an `end` of the code's own is a jump there. (A program that reaches
  its END takes its person out of the game's ticks for good: a mission's NPC whose own script has ended stands
  frozen, and an order to him is refused.)
- What lasts and what ends: `go` (`enemy_move_to_frame`) ends at the place (its number is the radius); `enter` ends
  when he sits; `follow`, `flee` and `attack` last until another order (or until the game's own action ends:
  the target dead, out of sight). `stop` ends at once.
- Driving: `enemy_car_moveto 0, 0, stop` (act[0] the car he sits in, frm[0] the place) is the game's own drive by the
  road network, its traffic lights and bridges; `human.drive {actor, car, to}` on the bridge gets him in first. The
  game ends a drive it gives up (the car stuck, pushed off its way) the same as one that arrived, so the bridge's
  named orders measure their goal at the end: ORDER_ENDED says `arrived` and `distance` (go, drive), `seated`
  (enter), and with `retries` an order that ended short of its goal is given again after `retry_ms` (a drive goes on
  from where the car is; `retried` counts them). A mod measures its own: `human_order_state` tells the end,
  `actor_*` the place.
- How a drive starts and goes (`human.drive`'s `start`, `speed`, `calm`): the game's drive only goes forward, so a car
  facing away from its way's first leg (a parking lot, a car that arrived the other way) turned on full lock across
  the pavement into what was there. By default the loader backs such a car round first with the game's own reverse
  (what the chase does when a car is stuck: gear -1, at most 3 m/s, until it faces its way), and ends the game's own
  unsticking of a car that stood 2 s once it has got 6 m away (left alone, it lasts 15 s at the chase's speed, with
  no limit of its own: 22 to 27 m/s in town). `speed` caps the way's speeds (the road's are 9 to 13 m/s in town);
  ORDER_ENDED tells `start`, `start_angle` and `unstuck`. `enemy_car_moveto` in a mod's own order has none of this.
- The road network: `road_near(M, at, out)` (`world.road {at}`) is the node nearest to a place, where a drive's way
  ends (`C_roads::GetNearNode`); the nodes are the crossings and bends, tens of metres apart. A drive's place need
  not be on a road.
- The bridge has the same: `human.order {actor, code, act, flt, at}` and the named ones, `human.go`, `human.follow`,
  `human.attack`, `human.flee`, `human.enter`, `human.stop`, `human.order_state` (docs/PROTOCOL.md); MCP `wait
  {"types": "ORDER_ENDED"}`. Seen in both labs: tests/game/test_orders.py does each of them.

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
// its rigid body: what C_Vehicle::Move integrates (knowledge/notes/vehicle_physics.md)
car.AngularVelocity(); car.Forward(); car.Right(); car.Up(); car.Calm(); car.Awake(); car.StepTime();
for (int i = 0; i < car.WheelCount(); i++)
    if (void *w = car.Wheel(i)) { game::at<float>(w, game::kWheelLoad); game::at<uint32_t>(w, game::kWheelFlags) & game::kWheelOnGround; }
// the car's functions to hook: game::Exe().vehicleTick, vehicleEngine, vehicleMove, vehicleWheelsColl; LS3D's
// quaternions: H->module_base("LS3DF.dll") + game::Ls3d().quatRotationMatrix (QuatMatrix in mathx.h is its formula)
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
- **Paths are UTF-8** (`game_dir`, `mod_dir`, `mod_data_dir`, `file_source`, `file_add`'s): players keep the game in
  folders of any name (`C:\Игры\Mafia`). `fopen` and the ANSI functions (`CreateFileA`, `LoadLibraryA`...) read a
  path in the system's code page and fail on such a folder: open files with `mml/path.h`'s `mml_fopen(path, "rb")`,
  or widen the path (`mml_widen`) for the wide functions (`CreateFileW`, `FindFirstFileW`).

### Settings in the game's Options (a page with no code)

The game's Options (`optionsb`, opened from the pause menu: seen in both builds) has a row **Mods** under Audio and
Video: it lists every
loaded mod whose `mod.json` has a `"settings"` block, and each opens a page of the game's own controls made from it,
as the freeride's density sliders and the Options' own are made. A change is made at once: the mod gets
`MML_EVENT_CONFIG_CHANGED` at the next frame (read your settings there, as for any change), so what a slider does is
seen while it is dragged; the file in the data folder is written once the changes stop, and at Back.

```json
"settings": {
  "title": "God rays",
  "controls": [
    {"key": "on", "label": "On"},
    {"key": "density", "label": "Haze", "min": 0, "max": 0.05},
    {"key": "distance", "label": "Reach", "min": 30, "max": 200, "step": 10, "unit": "m"},
    {"key": "steps", "label": "Samples", "min": 8, "max": 64, "step": 8, "show": "value"},
    {"key": "mode", "label": "Mode", "options": [{"value": 0, "label": "Soft"}, {"value": 1, "label": "Hard"}]}
  ]
}
```

- **The kinds** follow the default in `config.json` (or `"kind"`: `slider`, `check`, `choice`, `key`): a boolean is a
  check box; a number a slider, `min` to `max` (both needed), snapped to `step`, a whole number when the step and the
  ends are whole; `options` a selector of those values (`{"value", "label"}`, or plain values shown as they are).
- **A key** (`"kind": "key"`, the setting a hotkey's combo: `"F7"`, `"Ctrl+Shift+R"`): its row shows the combo; a
  click waits for the next key pressed, with the modifiers held, and makes it the setting (Esc keeps the one there);
  meanwhile the game gets no key, and the key taken is kept from it until it is let go. Bind the hotkey in the
  function that reads your settings (`H->hotkey(M, H->config_string(M, "hotkey", "F7"), fn, NULL)`: the same
  function again is bound anew), so a key chosen there works at once.
- **The value beside a slider**: its place in the range in % by default; with `unit` (or `"show": "value"`) the
  value itself.
- **The page**: the title (`title`, else the mod's `name` up to a colon), a row a control, **Defaults** (every key on
  the page back to `config.json`'s) and **Back**. Esc on the page returns to the list, and on the list to Options,
  as the game's own pages do. A key is a path (`"overlay.x"`), as for `config_number`.
- **Sections** (a mod of several parts): `"sections": [{"title": "Routes", "controls": [...]}, ...]` in place of (or
  after) `"controls"`. Each is a tab, as the game's own Controls page has them (Player, Car, Other): buttons along the
  bottom of the page before Defaults and Back (rows of their own above them when they do not fit there; a tab as wide
  as its title needs), the page titled with the tab shown ("Debug views: Routes"); a tab makes the page again with its controls (Defaults resets that tab's). Keys
  under a section's name keep them apart in `config.json` (`"routes.show"`, `{"routes": {"show": true}}`):
  mods/examples/debug_views does so.
- **The page never runs off the screen**: a section (or the one list) whose rows do not fit in the 600 units is cut
  into parts of about the same height, "Race 1/2", "Race 2/2", tabs as the sections are (some 17 rows of one line
  fit a page). Short labels (20 characters: one line) keep a section whole.
- `call panel.state` shows what the panel made of every mod's block (each control's kind, value and what is shown,
  its sections and the parts its page is cut into); `call panel.open {"mod": "<id>", "part": "Race 2/2"}` opens a
  page (a part by its number or its name) without the clicks. A control the panel cannot make (a slider with
  no `max`, a choice with no `options`) is left out, and the log says why.
- **Checked offline**: `call panel.layout {"settings": {...}, "defaults": {...}}` lays a manifest's pages out with no
  game (every part: each control's place, its text, the estimated width of its text); `tests/run_tests.py` does so
  for every mod's `mod.json` in the repository and fails on a control or a text over another, anything out of its
  window or the screen, a button's or a selector's text wider than it, or a label cut (more than two lines of 20
  characters). The widths are the loader's estimate of the game's menu fonts (measured on its pages), a little wide.

### A mod's own files in the game

The game opens every file by a name relative to its folder (`models\crate.4ds`, `maps\crate.bmp`, `anims\x.5ds`,
`sounds\x.wav`, `tables\vehicles.bin`, `tables\menu\gamemenu.mnu`, `missions\<name>\scene2.bin`), from its folder
or its `.dta` archives, through one function (`rw_data.dll`'s `dtaOpen`: Game.exe and LS3D alike). The loader
answers those names from the mods first:

- **Every file under the mod's `files\` folder is a game file by its path there**: `files\Maps\crate.bmp` is
  `maps\crate.bmp` (names compared as the archives compare them: case ignored, `/` as `\`). While the mod is loaded
  the game opens the mod's file for that name, whether the game has one (replaced) or not (new); unloaded, the
  game's own again. The build copies `files\` with the mod (`mml_add_mod`), `deploy` too (a file the game holds
  open is replaced all the same: it reads the old one until it closes it).
- **A mod of files alone** needs no DLL and no code: `"type": "files"` in `mod.json`, a `files\` folder, and in
  `CMakeLists.txt` `mml_add_files_mod(<id> <dir>)` (`mods/examples/freeride_picture`: the freeride menu's picture
  replaced). It loads, unloads and reloads like any mod (`mod.unload`, `mods.list` counts its `files`).
- **Two mods with one name**: the later in load order answers (a higher `load_order`; the same: the id later in the
  alphabet). The log says who answers over whom; `call files.which {"name": "maps\\crate.bmp"}` and `files.list`
  tell it at any time.
- **When the game reads it**: a file is read when the game opens it, and what it keeps once read is not read again
  by itself. Measured in 1.2: every mission's load opens its textures, models, animations, sounds and its tables
  again (the freeride: about 1300 pictures, 720 models, 240 sounds each time); a menu opens its pictures each time
  it opens. So a changed texture or model shows at the next load of a mission, a menu's picture at its next opening;
  a texture in use in the running mission stays as it was. The mods' files are in the game from the process's start
  (before the game reads its first tables, before the first frame); a mod loaded later (`mod.load`, a new mod) is
  read from then on, so a table the game reads only at its start needs a restart.
- **Textures**: LS3D keeps presaved copies of some textures in the archives (`<name>_<mips>.565`, `.555`,
  `.DX<n>`: converted once) and reads one in place of the picture when its size matches. For a picture a mod
  ships, the loader hides the game's presaved copies (the log says so once a name), so LS3D makes the texture from
  the mod's picture. A picture is a `.bmp` or `.tga` of 8, 16, 24 or 32 bits (LS3D's `LoadImage`).
- **Changes on disk** are seen: the `files\` folder is watched (a quarter of a second after the last change), and
  every mod gets `MML_EVENT_FILE_CHANGED` with the game's name of each file whose answer changed (its content, a
  file come or gone, another mod's in its place; not for a change under another mod's file of the same name, nor
  for a reload that changed nothing). A mod that keeps something made of a file (its own model, a table it parsed)
  makes it again then.

From code:

```c
/* a game file answered by a file of the mod's choosing: a patched copy of a game's table in its data folder */
char path[MAX_PATH];
snprintf(path, sizeof path, "%spatched_vehicles.bin", H->mod_data_dir(M));
int64_t n = H->file_read(M, "tables\\vehicles.bin", NULL, 0, MML_FILE_GAME);   /* the game's own: its size */
/* ... read it (buf, n), change it, write it to path ... */
H->file_add(M, "tables\\vehicles.bin", path);    /* 1: done; path NULL or "": taken back */
char who[MAX_PATH];
if (H->file_source("maps\\crate.bmp", who, sizeof who)) { /* a mod's file answers: who is its full path */ }
```

- `file_read(mod, name, buf, cap, flags)` reads a file as the game reads it (a mod's, or the archives'), or the
  game's own whatever the mods ship (`MML_FILE_GAME`): the whole size, `cap` bytes of it copied (`buf` NULL to ask
  the size), -1 when there is no such file. Any thread.
- `file_add` puts a file of the mod's in its place in the order, over its own `files\` of that name; it is looked at
  for changes once a second. Names outside the game's folder and files that do not exist are refused (0).
- Agents: `files.list {mod, name}`, `files.which {name}`, `files.read {name, game, offset, size}` (`docs/PROTOCOL.md`).

### Videos: the logos and the intro replaced

The loader plays the game's videos itself, with FFmpeg (`host/video.cpp`, `knowledge/notes/videos.md`), in both
builds: the three logos before the profile menu (`logo1`, `logo3` (1.2 only), `logo2`) and the new game's intro
(`intro`). A mod replaces one by shipping a file of that base name in its `files\` folder; nothing else is needed (a
mod of files alone: `"type": "files"`).

- **Where**: `files\logo1.mp4` (any of the extensions below), loose at the top of `files\`. For each video the game
  asks for, its base name is looked for with each extension in this order: `mp4`, `mkv`, `webm`, `mov`, `bik`,
  `avi`; first in the loaded mods' files (the later mod in load order wins a name), then loose in the game's folder
  (where 1.2 keeps its own `.bik` and `.avi`). The first file FFmpeg opens with a video it decodes is played; one it
  cannot is logged with the reason and the next is tried; none: the game's own player gets the name, as before.
- **Which formats**: MP4, MKV, WebM, MOV, AVI, Bink, Smacker, Ogg, MPEG-PS/TS, FLV containers; H.264, HEVC
  (H.265), VP8, VP9, AV1, MPEG-1/2/4, MJPEG, Theora, ProRes, Bink 1 of every revision FFmpeg reads (BIKb, BIKf to BIKk,
  where the game's own Bink 1.9c reads only BIKf..BIKi), Indeo, Cinepak, WMV; sound AAC, MP3, Opus, Vorbis, FLAC,
  AC-3, E-AC-3, Bink audio, WMA, PCM and ADPCM. **Not Bink 2** (`KB2…`: no open decoder exists; the log says so).
  The easy choice: **MP4 with H.264 and AAC**, at any size and frame rate.
- **How it looks**: at the file's own size (up to the device's largest texture) in 32-bit colour, fitted to the
  screen with its aspect kept (black bars), its sound through the game's own DirectSound. Any key skips it, as in the
  game. The game's own players (1.2's Bink, DirectShow for AVI) show 16-bit colour and refuse what they cannot read
  without a word; the loader logs every video it plays or passes over (`[video]` lines).
- **Without FFmpeg** (no `mml\ffmpeg\` beside the game: a package without it) the game's own players play the
  videos as they always did; the log says why once a video.
- **Try one** without restarting: `call video.play {"name": "logo1"}` plays it as the game would (from the menus;
  `wait {"types": "VIDEO_ENDED"}`), `call video.find {"name": "intro"}` lists the files found in order and what
  FFmpeg says of each, `video.state` tells FFmpeg's version and the last videos played, `video.skip` ends the one
  playing. A `screenshot` works while a video plays.

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

### Doubles on the game thread

The game's Direct3D leaves the game thread's x87 at 24 bits of precision (`0x007F`, both builds), and 32-bit code
computes part of its double arithmetic on the x87: at 24 bits a double comes out as a float. The loader runs every
call into a mod (its events, commands, posted calls, graphics passes, menu callbacks, load and unload) with the x87
at 53 bits and gives the game its own word back after, so a mod's doubles are doubles, as in the offline tests.
`call fpu.state` shows the game's word and the mods'.

- **A detour on a game function** (`H->hook`) is called by the game itself and runs with the game's 24 bits. One
  that needs doubles sets 53 bits and gives the game's word back before it returns:

  ```c
  unsigned int old, tmp;
  _controlfp_s(&old, 0, 0);
  _controlfp_s(&tmp, _PC_53, _MCW_PC);
  /* ... double maths ... */
  _controlfp_s(&tmp, old & _MCW_PC, _MCW_PC);
  ```
- A game function a mod calls from its callbacks runs with the mod's 53 bits (its float maths a little more precise
  there). A mod's own threads have their own word (53 bits, Windows' default).

### Measuring the frame

Every entry the loader makes into a mod is timed (each event by name, its passes, its views' functions, commands,
hotkeys, menus' and scripts' callbacks, posted calls), and so are the loader's heavy parts (the scene views, the
graphics bridge); a frame is Present to Present.

```c
MmlFrameStats fs = {sizeof fs};
if (MML_HAS(H, frame_stats) && H->frame_stats(M, &fs) && fs.p95_ms > 11.1f)
    LowerQuality();             /* 90 Hz: 11.1 ms a frame; fs.mod_ms is this mod's own, fs.views_ms the views' */
```

- `call prof.state`: the frame's average, median, 95th percentile and worst (the last 600 frames), each mod's time
  a frame by site with its worst frame and its calls a frame, the hitches (a frame 2.5 times the median and 8 ms
  longer: what took its time); `prof.reset` starts them over.
- **The game thread sampled** (`prof.sample {"seconds", "interval_ms"}`, then `{"id"}`; MCP: `profile {seconds}`,
  which waits and makes the stacks functions): every millisecond (a timer: no core kept busy; down to 0.25 ms by a
  wait on the clock) its address, its phase (the game's own work, LS3D drawing the frame, the scene views, the
  graphics bridge), the mod whose call it is in, and the return addresses into the game's, the loader's and the
  mods' code on its stack (the top 16 KB, copied while it is stopped: `stopped_percent`, about 1.5 %, is the
  measure's own cost). `profile`'s `functions`: each function's `self` (the thread in it), `calls_out` (in Direct3D,
  the driver or the system, called by it: a draw's or a lock's cost) and `total` (on the stack), the game's named by
  the knowledge base (1.0's only by the function that holds the address: `knowledge/<module>/bounds_10.csv`), the
  loader's and the mods' by their PDBs. A stack holds stale return addresses too (left in a frame's unwritten
  locals); they are told apart: the system's and the loader's frames are walked by their frame pointers (the loader
  is built with /Oy-) up to the first frame of the game's, and above it a return address is kept only where its
  call's target (a direct call, or one through a pointer at a fixed place: an import, a hook's original) is the
  function of the frame below; a call through a vtable cannot be checked and is kept (`stale_left_out_percent`).
- What the mods hook themselves (a hook's detour inside the game's function) is the game's time in these
  measures, not the mod's: it shows in the sampler under the mod's DLL.
- `river_water.status` shows the frame's numbers as the mod sees them.

#### Each frame's record, and two configurations compared

- **The frame's anatomy** (`prof.frames {since}`): a frame is from one return of the game's Present to the next;
  each has its time (`wall_ms`), the time Present blocked (`present_ms`: the GPU or the display ahead of the game),
  the game thread's own CPU time from its cycles (`cpu_ms`: what it lacks of the time outside Present it was not
  running, preempted by other processes or waiting), and the GPU's span between the frame's start and its Present
  (`gpu_ms`, timestamp queries read a few frames later). A frame is CPU-bound when Present hardly blocks and
  `cpu_ms` fills it.
- **The device's calls** (`prof.counters {on}`): every call of the game's Direct3D 9 counted and timed a frame by
  kind (draws, render states, textures, transforms, locks, gets...) and by caller (`game`: its Direct3D 8 through
  the wrapper; `host`: the loader), the time in the driver under the draws, the draws by pipeline (fixed, shaders)
  and the switches between them; and the loader's own costly sites (`hot_ms`: people's skinning, its `.game` share
  LS3D's own function). Their cost, measured: some 35 ns a call, 2 % of a 2.8 ms frame (off by default).
- **A comparison** (MCP `bench {a, b}`: each a list of calls that make the configuration, e.g.
  `[{"method": "skin.gpu", "params": {"on": true}}]`): the configurations take turns in one run, in blocks ABBA
  then BAAB (a drift of the machine or the scene falls on both alike), segments of `frames` frames (100) after
  `settle` let pass (30), each segment's median; B less A is the mean over the blocks (10) with its 95 % interval,
  `noise` the spread of one configuration measured twice in a block. Without `b`, A against itself: the check of
  the measure (no difference should be found; a 95 % interval finds one in about one figure of twenty by chance).
  Measured in the labs: 1.2's freeride, 2.8 ms frames: ±0.02 ms (0.6 %) with the machine quiet; ±0.07 ms when other
  processes kept 12 to 17 % of it busy. The run's `conditions` name what disturbs it: other games, the machine's
  other load, frames whose Present failed (an occluded window), the sampler meanwhile.
- **A still world** (`call state.time {"scale": 0}`): the step LS3D's frame limiter gives every system of the game,
  scaled; a frame whose step is 0 does not tick the mission at all, as the game's pause (a car's physics takes a
  step of 0 as 1 ms: the traffic would roll on). Every frame then draws the same scene: what the drawing costs, free
  of what moves (people's skinning does not run: compare it with the time going). Slower or faster: 0.25, 2.
- `tools/mcp_call.py <tool> '<json>'` calls a tool of the MCP server just built, from a terminal (a session keeps
  the server it started with).

### A heavy mod: its own libraries, its threads, a clean unload

- **Libraries beside the DLL.** Put every DLL your mod needs (PhysX, Jolt, OpenVR, a runtime) in the mod's folder,
  next to its own DLL. Each load runs from a folder of its own, `<game>\mml\live\<id>_<pid>_<n>\`: your DLL
  (named `mod_<id>_<pid>_<n>.dll` there) and a copy of every other `*.dll` of your folder. So the libraries your DLL
  imports are found, and the mod's folder is never locked: a build or a deploy replaces any file while the game runs.
- **Loading a library yourself.** In `mml_mod_load` a bare name works (`LoadLibraryA("PhysXCooking.dll")`: the load's
  folder is the DLL directory while it runs). Later, load by the full path, from the folder your own DLL is in
  (`GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)`, then `GetModuleFileNameA`). Never from
  `H->mod_dir(M)`: that would lock the mod's folder.
- **Unload what you made, in `mml_mod_unload`**: stop and join your threads (and your libraries' worker threads: shut
  their SDK down), release Direct3D objects and physics worlds, free the libraries you loaded, delete the game's
  objects you made. The loader takes back what you registered with it, not what you made yourself.
- **What the loader checks when you unload.** A thread still in your code (running in it, returning to it through
  its frames, or begun in it) would crash the game when the DLL went: the loader waits up to a second, then keeps
  that load's DLLs mapped (logged as an error, `kept_generations` in `mods.list`) and frees them once no thread is
  in them. A library still loaded after your DLL is freed is named (`stays_loaded`), and the next load is warned
  when one of its libraries is that old copy (Windows reuses a loaded DLL of the same name). Each unload logs what
  the whole process did not get back since the load (address space, its largest free block, handles, threads):
  `last_unload` in `mods.list`.
- **Measure your reloads**: `call mod.reload {"id": "<id>", "times": 20}` reloads it 20 times and returns the process
  after each load and the loss `per_reload`. A clean mod loses nothing (core and telemetry: 0 MB, 0 handles in the
  1.2 lab). A 32-bit game has 2 to 4 GB of address space: a mod that leaks a few MB a reload ends the game's life
  after some dozens of reloads.

### A heavy mod's build: third-party libraries, shaders, a Direct3D 11 device

`mods/examples/boxes` (not enabled by default) is the whole way in one mod: read it with this section.

- **A third-party library** is pinned in `tools/deps.py` (its URL and SHA-256) and built by `CMakeLists.txt` as a
  part of the loader's build, for x86 and with the mods' runtime (`/MT`): the first `cmake --preset x86` fetches it
  (and fetches it again when its pin changes). Jolt Physics 5.6.0 is there (target `Jolt`: upstream's 32-bit SSE2
  configuration, without the GPU compute back ends, the profiler, the debug renderer and object streams); a mod links
  it and gets its definitions with it:

  ```cmake
  target_link_libraries(mod_my_mod PRIVATE Jolt)
  ```

  It is a static library: Jolt lives inside your DLL, its globals too, so each load sets them up and each unload
  takes them down (`JPH::RegisterDefaultAllocator`, `Factory::sInstance`, `RegisterTypes` in `mml_mod_load`;
  `UnregisterTypes` and the factory deleted in `mml_mod_unload`). Call `JPH::VerifyJoltVersionID()` once: it catches
  headers compiled with other definitions than the library. Jolt is built without exceptions and RTTI (its choice);
  your mod keeps its own. Another library: pin it in `deps.py`, add it to `CMakeLists.txt` the same way (its own
  CMake when it has one, `add_subdirectory` with its options set), with a line in `third_party/README.md`.
- **Shaders** are compiled in your mod's build, an option of `mml_add_mod`:

  ```cmake
  mml_add_mod(my_mod ${CMAKE_SOURCE_DIR}/mods/my_mod mods/my_mod/my_mod.cpp
      SHADERS fx.hlsl:VsMain:vs_5_0 fx.hlsl:PsMain:ps_5_0 fx.hlsl:CsBlur:cs_5_0:kBlurCs)
  ```

  Each `<file>:<entry>:<profile>[:<name>]` (the file relative to the mod's folder) becomes a header
  `shaders/<name>.h` (the entry's name by default) holding `const BYTE k<name>[]` (dxc's: `const unsigned char`):
  include `<windows.h>`, then
  `#include "shaders/VsMain.h"`, and give `kVsMain, sizeof kVsMain` to `CreateVertexShader`. Shader model 5 and lower
  (Direct3D 9 to 11) go through the Windows SDK's `fxc`, 6 and up (Direct3D 12) through its `dxc`; optimised, a
  warning fails the build, an `.hlsli` beside the file compiles it again when it changes. The bytecode is in your
  DLL, so a shader's change is one build and one `deploy` away.
- **A Direct3D 11 device of your own** (until the loader's graphics bridge gives one shared device beside the game's:
  `docs/ROADMAP.md`, Tier 1, item 3): make it in `mml_mod_load` (`D3D11CreateDevice`; WARP when the hardware refuses),
  use it in `MML_EVENT_FRAME`, release it in `mml_mod_unload`: `ClearState`, `Flush`, every object made from it, the
  context, the device last. The count the device's last `Release` returns must be 0 (`boxes` logs it and tells the
  next load): a reference left keeps the device and its memory in the process after your DLL is gone. Two things that
  cost time:
  - a device with no swap chain has no `Present` to send its commands: `Flush` once a frame, or a read-back that does
    not wait (`D3D11_MAP_FLAG_DO_NOT_WAIT`) finds them never done;
  - `GetDeviceRemovedReason` every frame: a device can be lost (a driver reset, a hung GPU); release everything and
    make it again later, not in every frame.
- **Measured in the 1.2 lab** (2026-10-15, `boxes`: 48 Jolt bodies on 2 job threads, a 256x256 picture drawn and its
  pixels counted every frame): 20 reloads with frames between them and 100 more in a row (`mod.reload {"times":
  100}`), 143 devices made: each drew after its reload, none lost, every one released to its last reference, 0.0 MB
  of address space, 0 handles and 0 threads lost a reload. A reload with a device takes about 125 ms (the device 25
  ms of it). In the stand-in game (no game holding Direct3D) each unload takes `d3d11.dll` and the driver out of the
  process again, and NVIDIA's driver keeps a section and a mutex of each device it made (2 handles a device; WARP
  none): a driver's own, not the mod's. mafia-ragdoll lost its device after 22 reloads of its core in one game
  (removed after a 671 ms frame, then no address space left for a thread's stack; its core leaked about 1 MB of
  address space a reload; what each reload left was never measured there): the measure is the address space, and a
  device made and given back as above costs none.

### 64-bit halves (the sidecar)

The game is a 32-bit process: a library made only for 64 bits (PhysX 5, NVIDIA Flow, a modern GPU or ML runtime)
cannot be loaded into it. A mod may have a 64-bit half for it, run by the sidecar (`mml-sidecar64.exe`), a 64-bit
process the loader starts beside the game in a job object (it ends with the game) and starts again when it ends
without being asked. `sdk/include/mml/side.h` is the half's ABI (read it whole); `mods/examples/hello_side` is the
smallest half alone, `mods/examples/side_picture` a mod in two halves that hands the frame a picture,
`mods/examples/physx_crates` one whose 64-bit half steps PhysX 5 and hands the frame the bodies' poses,
`mods/examples/flow_smoke` one whose 64-bit half simulates and ray-marches NVIDIA Flow's smoke and fire into the frame.

- **The manifest** names the half with `"entry64"` (a path inside the mod's folder: `"x64/my_mod.dll"`; that folder
  is the half's own, its libraries go beside it). With `"entry"` too the mod has two halves, loaded, reloaded and
  unloaded together (`mod.reload` reloads both, each from a fresh copy; a fault in the 32-bit half unloads both at the
  next frame, a fault in the 64-bit half that half alone: `MOD_FAULTED` with `"half": "x64"`, `mod.reload` brings it
  back); with `"entry64"` alone it is a 64-bit half alone. `mods.list` shows the
  half under `x64`, `sidecar.state` the sidecar and every half.
- **The build**: the 64-bit half is built by the `x64` preset (`cmake --preset x64`, `cmake --build --preset
  x64-release`), `mml_mod64(<id> <sources...>)` in `CMakeLists.txt` (into `build/x64/out/mml/mods/<id>/x64/`); the
  32-bit half by the usual `mml_add_mod`. `deploy` merges both outputs into the mod's folder and copies the sidecar
  (while the game is closed: its exe is locked).
- **What a half has** (side.h): the whole bridge as an agent has it (`call`, from any thread: `player`,
  `world.near`, any mod's command); the game's events as they come (`subscribe`: `FRAME`, `MISSION_TICK` with their
  `dt`, every event kept for agents, as JSON); commands of its own (`command`: `<mod>.<name>`, answered by the sidecar
  without the game thread); shared textures and channels (below). Its callbacks run on the sidecar's mod thread, one
  at a time, for every half: heavy work goes on threads of the half's own (stopped in `mml_side_unload`); the host's
  functions other than `call` and the channels' are the mod thread's.
- **Channels** (`channel_open(mod, name, size)` in both halves, the same name and size): a named block of shared
  memory, each `channel_write` a whole new version (at most `size` bytes), each `channel_read` a copy of the latest
  whole version and its number (0: nothing written yet). One writer a channel; a reader never gets half of two
  versions (the offline tests read 30 000 versions a second while the other process writes as fast as it can: none
  torn, none gone back). Any thread, both halves; open with another size: refused. A channel lives while either half
  has it open (both unloaded: it is gone, the numbers start again). A reload may change a channel's size: the new
  32-bit half's `channel_open` then waits (at most 5 s, logged) for the sidecar to say the old 64-bit half is gone,
  and opens it anew. In Rust: `host.channel_open(name, size)`,
  `Channel::write_value(&T)`, `read_value::<T>()` for a `#[repr(C)]` struct.
- **Shared textures**: the 64-bit half makes one with `texture_create(mod, name, w, h, format, &out)` (a Direct3D 12
  texture on the adapter the game draws on, `out.d3d12_resource`; Vulkan imports `out.shared_handle`). The formats
  are the ones Direct3D 11 opens from Direct3D 12 (side.h lists them: `R8G8B8A8_UNORM`, `R16G16B16A16_FLOAT`,
  `R32_UINT`, `R32_FLOAT`...); the texture is a render target with simultaneous access, so it needs no barriers (its
  state is promoted from COMMON). The adapter is known once the graphics bridge runs (a mod with `"gfx": true` on at
  the start: the mod itself, as a rule); until then `texture_d3d12_device` returns null and `texture_create` fails:
  try again later (each second, not each frame). The 32-bit half opens it inside a graphics pass with
  `gfx_shared_texture(mod, name, &t)`: `t.texture`, `t.srv` (the loader's, not to be released, opened again whenever
  the half makes it anew or the bridge's device is new), its size, format and the half's generation; 0 while it is
  not there. There is no lock between the processes: **the 64-bit half writes a texture only when the 32-bit half is
  not reading it** (two textures, written in turn), waits for its GPU work (a fence) and only then says in a channel
  which texture is whole; the 32-bit half draws the one the channel names.
- **The frame's round trip** (`side_picture`, measured 2026-10-18 in both labs, an RTX 3070): the 32-bit half writes
  the frame's number into a channel at `FRAME` (the frame's start); the sidecar is told of `FRAME` right after, the
  half's worker draws that frame's picture (two clears) and waits for its GPU: 0.4 ms from the frame's start to the
  picture whole; the pass, later in the same frame (the mission's tick and the world's drawing come between, 6 to 7 ms
  here), draws it. Every frame of 646 in each build drew its own frame's picture (`late` 0); 7 frames had none while
  the mod reloaded. So a 64-bit simulation that takes the frame's state at `FRAME` and gives its result back within
  the frame's own work is not a frame late.
- **PhysX 5** (5.9.0, pinned in `tools/deps.py`) is built by the `x64` preset: target `PhysX5` (static, `/MT`, CPU
  only: PhysX, Common, Foundation, Extensions, Cooking, PvdSDK; its own CMake as an external project in
  `build/x64/physx`, since it takes the build's configurations for its own). A half links it:

  ```cmake
  mml_mod64(my_mod mods/my_mod/my_mod64.cpp)
  target_link_libraries(mod64_my_mod PRIVATE PhysX5)
  ```

  PhysX lives inside the half's DLL: `PxCreateFoundation` in `mml_side_load`, everything released in
  `mml_side_unload` (a reload is a fresh copy of the DLL and a fresh foundation). Its error callback and its
  dispatcher's threads must not call the host (keep the errors, log them on the mod thread). The game's quaternions are
  PhysX's conjugate (the game's matrix for the same four numbers is PhysX's transposed): give `prop_place` (w, -x, -y,
  -z); a box from the game's axes (`MmlShape::axes`, rows) is `PxQuat(PxMat33(x, y, z))`.
- **`physx_crates`, the frame's round trip with PhysX** (measured 2026-10-18 in both labs at once, PhysX on
  2 threads of its own): the 32-bit half writes the frame's state at `FRAME` (the frame, its dt, the people's and
  cars' collision shapes around the pile from `world_shapes`, about 0.01 ms); the 64-bit half's worker, woken by
  `FRAME`, moves them as kinematic bodies over two substeps and steps 27 crates on the game's own static collision
  (its triangles cooked into a PhysX mesh at each new region, about 0.5 ms): the poses were whole 0.30 ms (1.2) and
  0.38 ms (1.0) after the frame's start, the step 0.2 to 0.3 ms; the 32-bit half places the props at `RENDER_BEGIN`,
  1.4 to 1.5 ms after the frame's start, so the poses of 2343 frames of 2344 (1.2) and 2345 of 2346 (1.0) were that
  very frame's step (Tommy walking through the pile and the owner pushing it meanwhile); the game thread spent 0.022
  to 0.024 ms a frame on the mod. A channel needs no way to wait for this: the step is over long before the frame
  needs it. A reload of both halves (state kept with `state_put`, the pile made again where it was) left 24 or 25
  frames without new poses (the half loads PhysX); a fault in the 64-bit half left the crates where they were, the
  game and the 32-bit half going on, and `mod.reload` brought it back in 9 to 12 frames. Ten reloads in a row: no
  handle, thread or memory left behind.
- **PhysX's GPU part: `physx_gpu`** (`mods/examples/physx_gpu`; PhysX built with CUDA, `PhysXGpu.dll` beside the
  half): water (PBD particles), a cloth and a jelly (FEM) stepped on the GPU on the game's static collision (its
  triangles cooked with `buildGPUData`), their points copied into a shared texture without the CPU and drawn by the
  32-bit half's pass (the water as a screen-space surface, the cloth and the jelly as lit meshes). What it took
  (2026-10-19, both labs): the game's ground is a one-sided triangle mesh, where NVIDIA's snippet has planes, and a
  deep block of water falling on it pushed particles through it for good (17 m under the ground in a second):
  speculative CCD for the particles (`PxParticleFlag::eENABLE_SPECULATIVE_CCD`) and 8 position iterations keep it up.
  Water on flat ground spreads into a sheet one particle thick (10 cm particles have no thinner film): the splats
  are drawn 1.8 times the particles' size so the sheet is whole, a thin sheet takes the normal of what is under it
  (the world's, from its depth: a flat, evenly lit film), darkens what it wets and fades at its edges (premultiplied
  over the frame). `physx_gpu.points` reads the particles' bounds back from the GPU.
- **NVIDIA Flow 2 in a half, rendered into the same frame: `flow_smoke`** (`mods/examples/flow_smoke`). Flow (smoke and
  fire as gas on a sparse voxel grid, ray-marched with self-shadowing) runs only on 64 bits and draws through Vulkan.
  `mml_flow(<id>)` in `CMakeLists.txt` gives a half its headers, CUDA's driver API and `nvflow.dll`, `nvflowext.dll`
  beside it (Flow itself is built by its own premake through `third_party/flow/premake5.lua`, `tools/deps.py flow`).
  The frame: right after the mission's tick (`MISSION_TICK`: at `FRAME`, before the tick, the cars were a tick behind
  where the frame draws them, 0.13 m on average and 0.9 m at most) the 32-bit half writes the sources and the people's
  spheres and the cars' boxes near them
  (the game's own collision shapes, `world_shapes`: Flow emitters that carry the gas at their velocity; the ground a
  box). With `flow_smoke.collision {"mode": "meshes"}` (off: the owner found it still strange) it writes instead the
  world the gas meets as the game draws it: the
  scenery around the sources (`scene_focus`, `scene_pieces`: one soup of triangles, written again only when the scan's
  pieces or their versions change), each car's own meshes posed now (`actor_meshes`, each vertex with the body's
  velocity, v + turn x r) and each person as capsules along his bones (`human_skeleton`: the host has no skinned
  meshes), each bone's velocity from its last place. In the half they are Flow's mesh emitters in physics-collision
  mode (every layer that collides): what is behind a face is solid (Flow's signed distance to the nearest face is
  negative behind it; the game's faces are seen from outside: `flow_smoke.winding` measures it), 0.6 m deep for cars
  and people, 0.5 m for the scenery (a fence's single face makes no block behind it), and a layer of 0.03 m outside
  holds the gas at the body's velocity, so a fire under a car's nose rises round its bonnet and a passer-by drags a
  wisp behind him. The depth is the cost: Flow searches the nearest face that far (3 m for cars: 10 ms of GPU). The half's worker steps Flow
  then, submitted without a wait (the GPU does it while the game draws). In its graphics pass the 32-bit half draws
  the frame's linear colour and its depth as the game's projection makes it (`_33 + _43 / z`, the sky 1) into two
  shared textures, waits for the GPU to have them (an event query), and writes the camera into a channel; the half
  copies them into Flow's buffers with CUDA (Vulkan's memory exported as an opaque Win32 handle, the Direct3D 12
  texture imported as a resource: both on the game's adapter, found by LUID), ray-marches the gas over them, copies
  the result into a third shared texture and answers; the pass (`channel_wait`) draws it as the frame. Measured
  2026-10-18 at 1280x960 on an RTX 3070: Flow's GPU time 0.3 to 0.4 ms, the round trip 2.8 (1.2) and 3.0 ms (1.0) on
  the game thread, 3243 frames of 3245 (1.2) and 1083 of 1085 (1.0) with their own frame's gas. With the meshes and a
  fire on a 4 cm grid (the smoke's 10 cm), 1.2: Flow's GPU time 3.3 to 4.9 ms, of it the ray march 1.7 ms and the
  frame's copies in and out 0.6 ms, the meshes' obstacles 0.09 ms (1500 triangles of scenery, a car's 1300, a person's
  350); the game thread's part 0.23 ms (the meshes gathered), the round trip 3.3 ms.
  **No thread waits for a GPU** since 2026-10-19: the 32-bit half makes two shared Direct3D 11.4 fences
  (`ID3D11Device5::CreateFence`, `D3D11_FENCE_FLAG_SHARED`, named for the game's pid and a generation), signals "in"
  after drawing the two textures (`ID3D11DeviceContext4::Signal`); the half imports both into CUDA by name
  (`CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE`), and its CUDA stream waits for "in", copies, signals one of
  Flow's exported Vulkan semaphores (Flow's `createSemaphore`, `CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32`);
  Flow's flush waits for it and signals a second one; the stream waits for that, copies out, signals "out". The
  half answers once all of it is queued; the pass has its device wait for "out" (`ID3D11DeviceContext4::Wait`) before
  the composite. The fences are used only once the half has confirmed it imported them (`Done::fenceGen`); until
  then, and on a device without Direct3D 11.4's fences, every step is waited for on the CPU as before
  (`flow_smoke.wait {"fences": false}` too). Measured 2026-10-19 on one scene, 1280x960, an RTX 3070: the pass on
  the game thread 2.22 ms (CPU waits) and 0.40 ms (fences) in 1.2, 0.39 ms in 1.0; the round trip 1.85 and 0.33 ms
  (`flow_smoke.state` `pass_ms`, `round_trip_ms`, `fenced`).
  **The gas alone, at half size** since 2026-10-19: Flow as patched (`third_party/flow/patch.py`, its
  `RayMarchBlockCS`) gives the gas's light premultiplied and its transmittance in alpha instead of laying it over the
  colour it is handed, so the half gets the frame's depth only (the colour's copy in is gone) and marches at half the
  frame's size each way (`render`'s width and height apart from the scene depth's; the `march_half` setting, on by
  default). The pass brings it up to the frame by the depth (four march pixels, each weighted by its distance and by
  how near its depth is to the pixel's) and lays `Enc(gas + T * Lin(frame))` over the frame as premultiplied "over":
  what passes ordered before it drew shows through the gas by T (it was drawn over before). Measured on the
  freeride's start (a fire, a smoke): Flow's ray march 0.56 ms at full size, 0.25 to 0.33 at half; the frame's colour
  in (0.22 ms) and out (0.35) gone, the gas out 0.08 ms. A Flow buffer is never asked again at a size it had: an
  allocation Flow's pool hands out again after CUDA imported it once is refused a second import, so the buffers only
  grow (the gas's at the frame's size, so the setting switches without one).
  What it cost to find: Flow's defaults are Omniverse's centimetres and z up (gravity, the attenuations: per unit of
  length, a hundred times larger in metres); its buoyancy and cooling are part of combustion (off, warm smoke never
  rises); its colour map is looked up by temperature and its smoke map is clear at 0 (cooled smoke vanishes: a map
  of one grey, alpha 0.9, keeps it), and multiplied by 2.5 (Omniverse's HDR); the first `createGrid` on a machine has
  the driver compile several hundred compute shaders (1.5 to 5 minutes; NVIDIA's disk cache, kept by the
  executable's path, makes the next start take under a second). Flow as built here keeps a Vulkan pipeline cache of
  its own besides (`third_party/flow/patch.py`: Flow made its pipelines with none): the half names a file in the
  mod's data folder (`vk_pipelines.bin`, through the environment variable `NV_FLOW_VK_PIPELINE_CACHE`), the device
  starts its cache from it when the file is the same GPU's and driver's, and writes it back once new pipelines stop
  coming (2 s after the last, from its queue's flush: the sidecar is killed with the game, so not at an orderly end).
  `flow_smoke.side` tells `pipeline_cache` (`read`, `written`) and `start_ms`.
  Its settings (Options > Mods, `config.json`): the grids' cells (`smoke_cell`, `fire_cell`) and the fire layer's
  parameters, each named as Flow names it; the smoke's are `flow_smoke.tune`'s. A source on the player's car is put
  just behind its hull (the game's box of it, `world_shapes`): inside the hull the car's own box holds the gas out and
  nothing shows. `"jet": true` makes it a jet leaving the car's back at `speed` (a rocket's exhaust), `local` places
  its nozzle in the car's frame.
- **NVIDIA DLSS 5 Neural Rendering over the game's frame: `dlss_nr`** (`mods/examples/dlss_nr`). DLSS-NR ("DLSS 5 with
  3D-guided neural rendering", NGX feature DLSSNR, `nvngx_dlssnr.dll`) adds lighting and material detail to the frame
  an engine drew (one frame in, one out; no upscaling, no frame generation). NGX exists for 64 bits only and DLSS-NR's
  API for Vulkan only, so the 64-bit half makes a Vulkan device of its own on the game's adapter (the sidecar's
  `adapter_luid` matched to `VkPhysicalDeviceIDProperties::deviceLUID`; `vulkan-1.dll` loaded at run time, the headers
  from `tools/deps.py vulkan`), with the instance and device extensions NGX asks for DLSSNR
  (`NVSDK_NGX_VULKAN_GetFeature*ExtensionRequirements`: `VK_NVX_binary_import`, `VK_NVX_image_view_handle`,
  `VK_KHR_buffer_device_address`, `VK_KHR_push_descriptor`) and `VK_KHR_external_memory_win32`, and starts NGX with a
  project id of its own (`NVSDK_NGX_VULKAN_Init_with_ProjectID`, a custom engine). The frame: the pass draws the
  world's colour (as the game drew it, in the display's gamma, 0..1: DLSS-NR's model works on gamma values, as RTX
  Remix gives it pow(x, 1/2.2) of its tone-mapped colour; given linear values it made the picture far too dark, seen
  on a player's RTX 5060), its depth as the projection makes it
  (0 near .. 1 far), each pixel's motion vector in pixels (to where it was the frame before: the depth reprojected
  through the last frame's camera; the game has no motion vectors, so its moving actors move as the world behind
  them) and a control mask (1 everywhere, the sky's share a setting) into four shared textures, and waits; the half
  imports them as Direct3D 12 resources (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`, a dedicated
  allocation, layout GENERAL taken from and given back to `VK_QUEUE_FAMILY_EXTERNAL` each frame), copies them into
  images of its own (DLSS-NR reads sampled and writes storage images; the shared textures are render targets with no
  UAV), evaluates DLSS-NR (`NGX_VULKAN_EVALUATE_DLSSNR_EXT`, RTX Remix's defaults: intensity 1, tone 0.3, structure
  0.7, preset B), copies its picture into `color_out` and answers; the pass draws it as the frame, under the HUD. Its
  settings (NVIDIA's parameters, a split screen to compare, what it is given as debug views, a round trip of the frame
  through the half untouched) are on the Options > Mods page; F12 switches it (`dlss_nr.state`, `dlss_nr.toggle`).
  **It runs on a GeForce RTX 50 only**: NGX's DLL checks the GPU's architecture itself (measured 2026-10-19 on an RTX
  3070, driver 617.42: `GetFeatureRequirements` AdapterUnsupported (4), MinHWArchitecture 0x1B0, Blackwell;
  `DLSSNR.Available` 0, `FeatureInitResult` 0xBAD0000B; NGX's log in the mod's data folder: "snippet is using newer
  GPU arch 1B0 > 170"). The half says so (`dlss_nr.side`, the game's console: "needs a GeForce RTX 50 GPU") and the
  game's own frame stays. A game drawn on another vendor's GPU (a CPU's own graphics beside the GeForce: a player's
  RTX 5060 machine ran the game on "AMD Radeon(TM) Graphics") is told apart before NGX: stage `other_gpu`, the
  message naming both GPUs and Windows' per-application GPU setting (Settings > System > Display > Graphics). The round trip, the debug views and the motion vectors' check (`debug` 5: the frame before
  moved by them against this one; 6 the same without them) work on any GPU: measured 2026-10-19 on the RTX 3070 at
  1280x960, the round trip 0.56 ms (1.2) and 0.50 ms (1.0) on the game thread, 0.07 to 0.08 ms of the half's GPU, no
  frame late. `nvngx_dlssnr.dll` is NVIDIA's (the RTX SDKs license): not in the repository nor in the build's output;
  `tools/deps.py ngx` pins the newest (310.9.16, from NVIDIA's RTX Remix package; the public DLSS SDK has no DLSSNR) in
  `third_party/_deps/ngx/bin/`, and a player puts it beside the mod's `mod.json` (`<game>\mml\mods\dlss_nr\`, not in
  `x64\`: the sidecar copies that folder at each load, and the DLL is 266 MB). Bypassing NGX's check of the GPU is
  against NVIDIA's license (4.d): the mod does not and will not.

## 4. A complete example

`mods/examples/telemetry/telemetry.cpp`: a speed, gear and lap-time overlay between two checkpoints, a hotkey that
starts a lap, the best lap kept in the data folder, everything adjustable in `config.json`, and an item
"Telemetry" in the pause menu opening a page (the best and last laps, the fuel as the game's progress bar, a check
box and a switch that change settings, Back). It uses each part above once, in 350 lines, and says at its top
where. In the freeride it works as it is:
`input {"steps": [{"action": "Accelerate", "ms": 5000}], "screenshot": true}` drives the taxi through its two
default checkpoints.

`mods/examples/boxes/boxes.cpp`: a heavy mod. A pile of boxes falls onto a floor and a plank in a Jolt world of the
mod's own (pinned, built with the loader, 2 job threads); a Direct3D 11 device draws them every frame with the mod's
HLSL (`boxes.hlsl`, compiled by its build) and counts the pixels on the GPU; the overlay draws the pile from the side
with the game's 2D lines. The world goes on across a reload (its bodies in `state_put`), the device is made anew
each load and given back whole. `call boxes.status` (the world, the device, the counts across loads),
`boxes.drop` (or F10), `boxes.snapshot` (the GPU's picture as a BMP in the data folder); `mod.reload {"id": "boxes",
"times": 20}` measures it.

`mods/examples/fps_unlock/fps_unlock.cpp`: a mod that changes the game itself. It takes the game's 62.5 fps limit
away (MafiaD3D11's A132: two places in LS3D, the addresses from `game_build.h`, the bytes checked before they are
written and put back at the unload, so a hot reload or the mod switched off leaves the game as it was), fixes a
system that did once a frame what it should do by the time (the main menu's camera: a hook that calls the game's
function and corrects its spring), gives a game function lengths of the time where it made them once a call (a
person's step at a curb: the sweep's operands pointed at the mod's floats, set for each call from its step), fixes a function of the engine
hooked in LS3DF.dll by its export name (`S_quat::RotationMatrix` dropped every turn under 0.00049 rad: with them
kept, a stopped car comes to rest and steers as at 62.5 fps; the fix checked against the game's function at load,
everything else handed to it), caps the frame rate on a
high-resolution timer if asked, and shows the frame rate and the game's clock step. `call fps_unlock.status`; `config.set {"mod": "fps_unlock", "key": "enabled",
"value": false}` (or F11) compares with the game's own pacing at once.

## Rules

- Everything runs on the game thread unless said otherwise; do not block it (no sleeping, no waiting on a file).
- A fault in your callback unloads your mod at the next frame (the game goes on); `log_tail` shows where. A fault
  inside a game function you called may leave the game in a bad state: check what you pass.
- Never keep a pointer into another mod's code or data across `MML_EVENT_SERVICES_CHANGED` / `MML_EVENT_MOD_LOADED`.
- `MmlHost` only grows. A mod that must run on an older loader asks `MML_HAS(host, member)` before using a newer
  member; `mml_mod_load` returning non-zero says "not for this loader".
- Everything in the repository is English.

## Not there yet

PhysX 5's GPU part (particles, cloth, deformable bodies: CUDA) for the 64-bit halves: `docs/ROADMAP.md`. Mods are native (a DLL through the C ABI: C, C++, Rust through
`sdk/rust/mml`, or anything that exports C functions); there are no script mods (Lua was considered and dropped,
2026-10-17).
