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
| `mod.json` | the manifest: `id` (its folder, its DLL, its commands' prefix), `name`, `version`, `entry`, `enabled`, `load_order`, `depends`, `gfx` (true: it draws through the graphics bridge: "Effects over the frame"), `builds` (the game's builds it is made for, `["1.2", "1.0"]`: on another it is not loaded and gives the game none of its files, `mods.list` says why; absent: every build) |
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
- **Each frame of the mission's world** (not the menus): the frame's depth buffer is a readable one while the world
  is drawn; at the point where the world is drawn and the HUD not yet (after the mods' `RENDER_WORLD` drawing, so
  their Direct3D 9 drawing is in the frame the passes see), the frame's colour and linear depth (metres along the
  view, `g_depthProj.z` for the sky) are handed to the passes, which draw in order into one effects target
  (premultiplied RGBA16F, cleared to 0), each laid over the ones before it (the blend state the bridge binds: ONE,
  INV_SRC_ALPHA; a pass returning 0 leaves them); the target is blended over the frame (`frame * (1 - a) + rgb`),
  under the HUD.
- **The constants** (`MmlGfxConstants`, `cbuffer MmlFrame`): the camera (eye, axes, projection), the frame's size,
  the time, the world's fog (as the world was drawn: start, end, mode, colour) and a sun (`w` 1 when the game's;
  in the freeride the game draws its world with no Direct3D light, so it is a default from above: the scene's own
  light is not read yet), the ambient light (a default). `frame.hlsli`: `ViewRay`, `ScenePoint`, `SceneDepth`,
  `IsSky`, `FogAmount`, `ApplyFog`, `ToScreen`, `Lin`/`Enc` (the game's colours are gamma-encoded).
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

### Questions to the world (rays, sweeps, the ground, what is near, the collision's shapes)

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
- **The shapes** (`world_shapes`): what the game keeps in its grids, in the world's space: the static world's
  triangles (normal out of the solid), boxes (centre, half sizes, axes), spheres and poles (vertical cylinders);
  the moving objects' boxes and spheres with their owner. What a physics library builds its world from near a
  body (a ragdoll's ground and walls, a fish's riverbed, a skateboard's ramps).
- Called on the game thread (in any event of the mod's or a command); every call is the game's work: a ray costs
  little, a large box of shapes more (tens of microseconds for a few metres around a person).
- `call world.ray {"from": "camera"}` (what the crosshair looks at; `"screen": [x, y]`), `{"from": "player", "to":
  "car"}`, `{"from": [x, y, z], "dir": [x, y, z], "length": 50, "what": "static", "ignore": ""}`; `world.sweep`
  (the same and `radius`), `world.ground {"at"}`, `world.near {"at", "radius", "type"}`, `world.shapes {"at",
  "radius"}` or `{"lo", "hi"}` (counts by kind, the first `max` shapes).
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

### What happens (shots, explosions, deaths, vehicles)

| Event | When | `actor` | more |
|---|---|---|---|
| `MML_EVENT_SHOT` | a shot goes into the game's shot queue (every weapon) | the shooter | `shot`: `pos`, `dir` (its length is the weapon's reach, about 120 m for a Thompson), `damage`, `shots` (pellets) |
| `MML_EVENT_HUMAN_HIT` | the game hits a person (a bullet, a car, an explosion, a fall) | him | `hit`: type, damage, attacker, alive before and after |
| `MML_EVENT_EXPLOSION` | an explosion (grenades, cars, the scripts' `explosion`) | its source or NULL | `explosion`: `pos`, `radius`, `damage` (sent after the game applied it: its hits come first) |
| `MML_EVENT_CAR_EXPLODED` | a car blows up | the car | |
| `MML_EVENT_HUMAN_DIED` | a person's life goes out, however (a hit, a fall, the water, a script) | him | `other`: the last who hit him in the 3 s before, or NULL |
| `MML_EVENT_VEHICLE_ENTERED`, `_LEFT` | a person starts or stops using a vehicle (a car, a tram) | him | `other`: the vehicle |

Deaths and vehicles are read every frame from each person the loader knows (a frame late at most); shots and
explosions are the game's own functions hooked. Every one of them also reaches agents (`events.since`, MCP
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
- **The game thread sampled** (`prof.sample {"seconds"}`, then `{"id"}`; MCP: `profile {seconds}`, which waits and
  names the game's functions from the knowledge base): every half millisecond its address, its phase (the game's
  own work, LS3D drawing the frame, the scene views, the graphics bridge) and the mod whose call it is in; by
  module, by mod, by phase, the hot spots, and for samples in Direct3D or the driver the game's code that called
  them. In the 1.2 lab, the city's freeride with `river_water`: the views 41 % of the game thread, the frame's
  drawing 26 %, LS3D's visibility recursion (`I3D_frame::SetVisibleRecMainPass`) the hottest function in both.
- What the mods hook themselves (a hook's detour inside the game's function) is the game's time in these
  measures, not the mod's: it shows in the sampler under the mod's DLL.
- `river_water.status` shows the frame's numbers as the mod sees them.

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

LuaJIT script mods, a Rust crate, 64-bit sidecar mods: `docs/ROADMAP.md`.
