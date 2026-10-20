/* MafiaModLoader: the C ABI between the loader and its mods.
 *
 * A native mod is a 32-bit DLL (C, C++, Rust, Zig... anything that exports C functions) in its own folder under
 * <game>\mml\mods\<id>\ with a mod.json manifest. The loader copies the DLL to a fresh file before loading it, so the
 * original can be rebuilt while the game runs; `mod.reload <id>` (bridge, MCP) unloads and loads it again.
 *
 * The contract:
 * - Every call into a mod happens on the game thread, unless a function says otherwise.
 * - MmlHost only grows: new members are added at its end; `size` tells which exist (MML_HAS(host, member)).
 * - What a mod registers (hooks, event handlers, commands, services) is removed by the loader when the mod unloads:
 *   a mod never leaves a pointer into its own code behind.
 * - Memory functions never fault: they return 0 when an address cannot be read or written.
 * - Every string is UTF-8, paths too (game_dir, mod_dir, mod_data_dir, file_add, file_source): the game may be in a
 *   folder of any name ("C:\Игры\Mafia"). Open a path with a wide function (_wfopen, CreateFileW), not fopen:
 *   mml/path.h has mml_fopen and mml_widen.
 *
 * A mod exports:
 *   MML_EXPORT int  mml_mod_load(const MmlHost *host, MmlMod *mod);   0: loaded; else it is unloaded again
 *   MML_EXPORT void mml_mod_unload(MmlMod *mod);                       before its DLL is freed (reload, exit)
 *   MML_EXPORT uint32_t mml_mod_api(void);                             return MML_API_VERSION (optional)
 */
#ifndef MML_H
#define MML_H

#include <stddef.h>
#include <stdint.h>

#define MML_API_VERSION 1u

#ifdef __cplusplus
#define MML_EXTERN_C extern "C"
#else
#define MML_EXTERN_C
#endif
#define MML_EXPORT MML_EXTERN_C __declspec(dllexport)

/* Whether a host table has a member (the table only grows; an older loader has a smaller one). */
#define MML_HAS(table, member) ((size_t)(table)->size >= offsetof(MmlHost, member) + sizeof((table)->member))

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MmlMod MmlMod;       /* one loaded mod (a reload gives a new one) */
typedef struct MmlReply MmlReply;   /* a command's reply being written */

typedef enum MmlLogLevel { MML_LOG_DEBUG = 0, MML_LOG_INFO = 1, MML_LOG_WARN = 2, MML_LOG_ERROR = 3 } MmlLogLevel;

typedef struct MmlVec3 { float x, y, z; } MmlVec3;

typedef enum MmlEventType {
    MML_EVENT_FRAME = 1,             /* every frame in every game state (intro, menus, loading, missions); dt */
    MML_EVENT_MISSION_TICK = 2,      /* right after the mission's tick (the game's world moved); dt */
    MML_EVENT_MISSION_CLOSE = 3,     /* the mission's world is about to go (a mission change, a restart, the exit) */
    MML_EVENT_ACTOR_DESTROYED = 4,   /* an actor is about to be destroyed: actor */
    MML_EVENT_HUMAN_CREATED = 5,     /* a C_human (the player, NPCs, pooled traffic people) was constructed: actor */
    MML_EVENT_HUMAN_HIT = 6,         /* a C_human took a hit (after the game's Hit): actor, hit */
    MML_EVENT_SERVICES_CHANGED = 7,  /* a service was published or went away (its mod unloaded): name */
    MML_EVENT_MOD_LOADED = 8,        /* a mod was loaded or reloaded: name (its id) */
    MML_EVENT_MISSION_OPEN = 9,      /* a mission is being opened, its loading begins: name (its folder under
                                        missions\: "freeitaly", "mise01", "00menu" the main menu). The world of the
                                        mission before it has closed (MISSION_CLOSE came first) */
    MML_EVENT_MISSION_LOADED = 10,   /* the loading is done: the mission's world ticks for the first time: name */
    MML_EVENT_GAME_PAUSED = 11,      /* a menu stopped the mission's world (the pause menu): no MISSION_TICK until */
    MML_EVENT_GAME_RESUMED = 12,     /* ... the world runs again */
    MML_EVENT_CONFIG_CHANGED = 13,   /* this mod's settings changed (its config.json edited, or config.set): name
                                        (the mod's id). Only the mod itself gets it */
    MML_EVENT_FILE_CHANGED = 14,     /* what the game reads for a file's name changed: a mod's file of that name was
                                        changed on disk, or a mod's file came, went or took another's place (a mod
                                        loaded, unloaded, file_add). name: the game's name of the file, lower case
                                        ("maps\crate.bmp"). Every mod gets it, at the frame after the change */
    /* The frame's drawing (docs/MODDING.md "Drawing in the 3D world"), on the game thread, around LS3D's
     * I3D_driver::Render of the frame's scene (the mission's world, or a menu's scene: `world` says which). scene:
     * the I3D_scene drawn; device: the game's IDirect3DDevice8 (render_device gives its Direct3D 9 interface). */
    MML_EVENT_RENDER_BEGIN = 15,     /* the scene is about to be drawn: the game has posed it (write bones here) */
    MML_EVENT_RENDER_WORLD = 16,     /* its 3D world is drawn, its 2D (the HUD, the menus) not yet: what a mod draws
                                        with the device here is in the scene, depth-tested against it, under the
                                        HUD. The device's transforms are the scene's camera (render_camera). The
                                        loader saves the device's states, render target and depth buffer before and
                                        puts them back after (Direct3D 9) */
    MML_EVENT_RENDER_END = 17,       /* the scene is drawn (its 2D too) */
    MML_EVENT_PRESENT = 18,          /* the frame is complete, about to be shown: drawing here is over everything
                                        (the HUD, the menus) and in `screenshot`'s picture; states put back too */
    MML_EVENT_VIEW_WORLD = 19,       /* a scene view's world is drawn (scene_view): RENDER_WORLD for that drawing.
                                        view: its handle. The device's transforms and target are the view's: what a
                                        mod draws here is in the view (a thing of the mod's seen in a reflection);
                                        the device's state is kept around it as around RENDER_WORLD */
    /* what happens (docs/MODDING.md "What happens"; both builds) */
    MML_EVENT_SHOT = 20,             /* a shot into the game's shot queue (every weapon's): actor the shooter, shot */
    MML_EVENT_EXPLOSION = 21,        /* an explosion (grenades, cars, scripts): actor its source (may be NULL),
                                        explosion */
    MML_EVENT_HUMAN_DIED = 22,       /* a person's life went out, however (a hit, a fall, the water, a script): actor
                                        him, other the last who hit him in the 3 s before (NULL: none) */
    MML_EVENT_VEHICLE_ENTERED = 23,  /* a person uses a vehicle (a car, a tram), entering or seated: actor him, other
                                        the vehicle */
    MML_EVENT_VEHICLE_LEFT = 24,     /* ... no longer uses it: actor him, other the vehicle */
    MML_EVENT_CAR_EXPLODED = 25,     /* a car blew up: actor the car */
    MML_EVENT_GAME_SAVING = 26,      /* the game is about to write a save (slot, profile): save_put the state now */
    MML_EVENT_GAME_LOADED = 27,      /* a save was loaded (slot, profile), or a new game or a freeride began (slot -1):
                                        save_get (MML_SAVE_SLOT) gives that save's state now */
    MML_EVENT_PEDESTRIAN_HIT = 28,   /* a traffic pedestrian took a hit (after the game's): actor its element (the
                                        traffic system's walker, not a C_actor), other its model frame, hit (pos
                                        where it stands, was_alive / alive by its health) */
    MML_EVENT_BULLET_HIT = 29,       /* a bullet struck something that is not a person (people: HUMAN_HIT,
                                        PEDESTRIAN_HIT): the static world, a car, a door...: actor the shooter, bullet */
    MML_EVENT_CAR_CRASH = 30,        /* a car struck the world or another car (each car of the two is told): actor the
                                        car, other the car it struck (NULL: the world), crash. Contacts that change the
                                        car's speed by 1 m/s or more (a car pressed on a wall touches it every frame); a
                                        car striking a person is that person's HUMAN_HIT (hit type 5 or 8, the car its
                                        attacker) */
    MML_EVENT_HUMAN_FELL = 31,       /* a person landed after a fall (off an edge, dropped; a jump is an animation of
                                        the game's, not a fall): actor him, fall (its damage, if any,
                                        is a HUMAN_HIT of type 6 at the same frame) */
    MML_EVENT_ITEM_GAINED = 32,      /* a person got an item, or rounds for one he has, however (picked up, given by a
                                        script, bought): actor him, item */
    MML_EVENT_ITEM_LOST = 33,        /* ... an item left his inventory (dropped, thrown, taken, his death): actor
                                        him, item */
    MML_EVENT_MONEY_CHANGED = 34,    /* the freeride's money changed (money_set, the scripts, the game): value the
                                        money now, change by how much */
    MML_EVENT_COUNT_
} MmlEventType;

typedef enum MmlMissionState {
    MML_MISSION_NONE = 0,            /* no mission is open */
    MML_MISSION_LOADING = 1,         /* opened, not ticked yet */
    MML_MISSION_RUNNING = 2,
    MML_MISSION_PAUSED = 3           /* a menu stopped its world */
} MmlMissionState;

/* Drawing (MmlHost draw_*): what a mod queues is drawn by the game's own 2D drawing at the end of the frame's 2D
 * pass, over the HUD and the menus, and is in the frame `screenshot` returns. Coordinates are the menus' virtual
 * units, whatever the resolution: the screen is 800 wide and 600 high, (0, 0) its top left corner. Colours are
 * 0xAARRGGBB. A text's size is its line height in the same units (the HUD's money is 40, a menu's button 30). */
typedef enum MmlFont {
    MML_FONT_SMALL = 0,              /* the plain font of the HUD's messages and the subtitles */
    MML_FONT_BIG = 1,                /* the bold font of the HUD's numbers and the menus' buttons */
    MML_FONT_TITLE = 2,              /* the handwritten font of the menus' titles */
    MML_FONT_COUNT_
} MmlFont;
#define MML_DRAW_LEFT 0              /* x is the text's left edge */
#define MML_DRAW_CENTER 2            /* x is its centre */
#define MML_DRAW_RIGHT 4             /* x is its right edge */

typedef void (*MmlHotkeyFn)(void *user);

/* The game's own menus (MmlHost menu_*). Not an overlay: the items and pages are the game's own controls, made by
 * the game's own code, in its fonts and paper, clicked with its mouse and keys and sounds.
 * A page is described as the game's menu files (tables\menu\*.mnu) describe theirs (`menu.def {name}` on the bridge
 * shows any of the game's own, `menu.state` any open menu). The first control is the page's window ("wind": its
 * text the title); the others are inside it. A container ("wins", "wind", "winc") holds the controls that follow it
 * until "end" (which returns to the page's window: containers do not nest). x, y, w, h: 800x600 units, from the
 * paper's (or the container's) top left corner. The kinds (all read in the game's code and seen in a page):
 *   stxt  a text                        item  a row that is clicked (the pause menu's)    butt  a button
 *   line  a line (w its length)         pbar  a progress bar (the loading screen's), 0..1
 *   slid  a slider, 0..1 (dragged, clicked, its arrows)
 *   chck  a check box, 0 or 1           bslb  a switch between two texts: text "Off|On", 0 or 1
 *   bsel  a selector: text "Easy|Normal|Hard", the option's index (a click steps to the next)
 *   iten  a number in its text: text "Laps: %u", menu_set_range, the value (a click steps it)
 *   edit  a text field (typed into; menu_text reads it; at most 63 characters)
 *   rctt  a picture: text an image file (.bmp, .tga) the engine loads as a texture (a full path: in the mod's data
 *         folder; or relative to the game's folder)
 *   wins  a scrolled list: its controls may reach past its height; the game draws its scroll bar (value 0..1 the
 *         scroll); it clips what is drawn after it, so the loader puts it last in the page (the keyboard's order
 *         is by id, not by place)
 *   and the game's other kinds (edin, sldi, rari, orez, rect, ...: their values are not offered yet).
 * A control that changes (a click, Enter, a drag, a step) calls the page's function with its id; menu_value reads
 * what it is now. flags 0: the kind's usual ones, as the game's own pages have them (a centred button in the bold
 * font, a grey text in the plain one); else MML_MENU_* (shown and enabled are added). The colours are each kind's
 * own (read in their drawing code): a text "stxt" is grey, MML_MENU_TEXT_* other; a button "butt" is black (grey
 * while a menu over it is active), MML_MENU_BUTTON_* other; a row "item" is black, MML_MENU_ITEM_RED red. */
#define MML_MENU_FONT_SMALL 0x0      /* the font, as MmlFont (the low four bits) */
#define MML_MENU_FONT_BIG 0x1
#define MML_MENU_FONT_TITLE 0x2
#define MML_MENU_RIGHT 0x40          /* the text at the right edge of the control */
#define MML_MENU_CENTER 0x80         /* ... in its middle */
#define MML_MENU_TEXT_WHITE 0x100    /* "stxt" */
#define MML_MENU_TEXT_YELLOW 0x200
#define MML_MENU_TEXT_BLACK 0x300
#define MML_MENU_BUTTON_RED 0x100    /* "butt" */
#define MML_MENU_BUTTON_WHITE 0x200
#define MML_MENU_ITEM_RED 0x400      /* "item" */
typedef struct MmlMenuControl {
    int id;                 /* what a click on it sends (unique in the page; the keyboard walks the page by rising id) */
    const char *kind;       /* "wind", "stxt", "item", "butt", "slid", "chck", "bsel", "wins"... (above), or "end" */
    float x, y, w, h;
    const char *text;       /* or NULL; options "A|B|C" (bslb, bsel), a format (iten), a file (rctt) */
    uint32_t flags;
} MmlMenuControl;
typedef void (*MmlMenuFn)(int id, void *user);   /* a control was clicked, changed (or Enter on it): its id */

/* The game's own script machine (MmlHost script_*): a program in the mission scripts' language (the language of
 * the missions' .bin scripts: one command a line, "// comments"; e.g. "findactor 0, \"Tommy\"",
 * "actor_setpos 0, 1, 2, 3", "wait 500", "human_addweapon 0, 6, 50"), compiled and run by the game's own
 * interpreter (C_program), one step a mission tick, as the missions run theirs: every command the game has, with
 * the game's own behaviour (docs/SCRIPTS.md). Its registers: flt[] (floats), act[] (actors), frm[] (frames);
 * the loader gives every program 256 floats, 64 actors and 64 frames before it starts (a `dim_*` up to those keeps
 * them, a larger one makes new, zeroed registers), act[] filled from `acts`, flt[] from `flts`. A program ends at
 * its `end` (or its last line), or fails (a line that does not compile: `line` and `error` tell; a command that
 * faults), or is stopped (script_stop, its mod unloaded, the mission closed, its time out). `owner`: the actor the
 * program runs as, as a mission's script runs as its entity (null: none): what `explosion` (at its position),
 * `playsound`, `frm_seton`, `act_setstate` with no register of theirs use, and what `findnearactor` skips. */
#define MML_SCRIPT_RUNNING 0
#define MML_SCRIPT_DONE 1
#define MML_SCRIPT_FAILED 2
#define MML_SCRIPT_STOPPED 3
typedef struct MmlScriptResult {
    uint32_t size;
    int handle;
    int state;                    /* MML_SCRIPT_DONE, _FAILED, _STOPPED */
    const char *error;            /* why it failed or stopped; "" when done */
    int line;                     /* the source line (1-based) that failed to compile or faulted; 0: none */
    const float *flt;             /* the registers as they ended (valid during the call) */
    int flt_count;
    void *const *act;
    int act_count;
    void *const *frm;
    int frm_count;
} MmlScriptResult;
typedef void (*MmlScriptFn)(const MmlScriptResult *result, void *user);

typedef struct MmlHumanHit {
    int type;               /* the game's hit type (0 bullet, 2 tram/plane, 3 explosion, 5 car, 6 fall, 8 car...) */
    MmlVec3 dir, pos;       /* as the game passed them (may be zero) */
    float damage;
    void *attacker;         /* the actor that caused it, or null */
    int was_alive, alive;   /* the human's alive flag before and after the game's Hit */
} MmlHumanHit;

typedef struct MmlShot {
    void *shooter;          /* the actor that fired */
    MmlVec3 pos, dir;       /* where from and towards (the world; dir as the game passed it) */
    float damage;
    int shots;              /* pellets in it (a shotgun's several) */
} MmlShot;
typedef struct MmlExplosion {
    void *source;           /* what blew up or threw it (a grenade, a car), may be NULL */
    MmlVec3 pos;
    float radius, damage;
} MmlExplosion;
typedef struct MmlBulletHit {
    void *shooter;          /* the actor that fired */
    MmlVec3 pos, normal;    /* where it struck; the surface's normal there (as MmlHit's) */
    MmlVec3 dir;            /* the bullet's direction (a unit vector) */
    float damage;
    void *actor;            /* what it struck: a moving object's owner (a car, a door...), NULL the static world */
    uint32_t flags;         /* as MmlHit's: the primitive's kind, a static one's surface in the top byte */
    const void *prim;       /* the game's primitive or object */
} MmlBulletHit;
typedef struct MmlCarCrash {
    MmlVec3 pos;            /* where they touched (the world) */
    MmlVec3 impulse;        /* N s: what the contact gave the car */
    float speed;            /* m/s: the change of the car's speed it made (|impulse| / its mass; the game's own sparks
                               begin at 5, its debris at 6) */
    uint32_t flags;         /* the world's primitive struck: as MmlHit's (its surface in the top byte); 0 a car */
} MmlCarCrash;
typedef struct MmlFall {
    float speed;            /* m/s: how fast he fell when he landed (the game hurts from 8 up) */
    float height;           /* m: from the highest point of the fall down to where he landed */
} MmlFall;
typedef struct MmlItemChange {
    int item;               /* the item's id in the game's list (world.items) */
    int count;              /* items of that id: +1 gained, -1 lost; 0: only rounds for one he has */
    int rounds;             /* rounds that came or went with it (loaded and in reserve) */
} MmlItemChange;

typedef struct MmlEvent {
    uint32_t size;          /* sizeof(MmlEvent) as the loader built it */
    MmlEventType type;
    float dt;               /* FRAME, MISSION_TICK: seconds */
    void *actor;            /* ACTOR_DESTROYED, HUMAN_CREATED, HUMAN_HIT */
    const MmlHumanHit *hit; /* HUMAN_HIT */
    const char *name;       /* SERVICES_CHANGED, MOD_LOADED, MISSION_OPEN, MISSION_LOADED, CONFIG_CHANGED */
    /* (members after this one: MML_EVENT_HAS(ev, member)) */
    void *scene;            /* RENDER_*: the I3D_scene drawn */
    int world;              /* RENDER_*: 1 the mission's world scene, 0 another (a menu's) */
    void *device;           /* RENDER_*, PRESENT: the game's IDirect3DDevice8 */
    int view;               /* VIEW_WORLD: the scene view's handle; 0 in every other event */
    const struct MmlShot *shot;            /* SHOT */
    const struct MmlExplosion *explosion;  /* EXPLOSION */
    void *other;            /* HUMAN_DIED: the killer; VEHICLE_*: the vehicle; CAR_CRASH: the car struck */
    int slot, profile;      /* GAME_SAVING, GAME_LOADED: the save's number (savegame\mafia<profile>.<slot>; -1: a new
                               game) and the profile's */
    const struct MmlBulletHit *bullet;     /* BULLET_HIT */
    const struct MmlCarCrash *crash;       /* CAR_CRASH */
    const struct MmlFall *fall;            /* HUMAN_FELL */
    const struct MmlItemChange *item;      /* ITEM_GAINED, ITEM_LOST */
    int value, change;      /* MONEY_CHANGED: the money now, and by how much */
} MmlEvent;

#define MML_EVENT_HAS(ev, member) ((size_t)(ev)->size >= offsetof(MmlEvent, member) + sizeof((ev)->member))

/* The camera the frame's scene is drawn with (render_camera), Direct3D's row vectors: a point p (row) is on the
 * screen at p * view * proj. eye, right, up, fwd: the camera in the world (metres; y up). */
typedef struct MmlRenderCamera {
    uint32_t size;
    float view[16], proj[16];
    MmlVec3 eye, right, up, fwd;
    float fov_x, fov_y;     /* the full fields of view, radians */
    float near_z, far_z;
    int width, height;      /* the viewport, pixels */
} MmlRenderCamera;

/* The graphics bridge's frame (gfx_pass). Its constants are cbuffer MmlFrame of mml/gfx/frame.hlsli (b0), which a
 * pass's shaders include. */
typedef struct MmlGfxConstants {
    float eye[4];           /* the camera's position, w: the time in seconds */
    float right[4], up[4], fwd[4];   /* its axes in the world */
    float proj[4];          /* the projection's _11, _22, _31, _32 */
    float size[4];          /* width, height, 1/width, 1/height */
    float sun[4];           /* towards the sun (unit), w: 1 the game's (its Direct3D light, else the scene's sun) */
    float depth_proj[4];    /* the projection's _33, _43; the sky's linear depth */
    float sun_color[4];     /* w: 1 the game's */
    float fog[4];           /* start, end, density, mode (0 none, 1 exp, 2 exp2, 3 linear) */
    float fog_color[4];     /* w: 1 when on */
    float ambient[4];       /* w: 1 the scene's ambient light */
    /* the shadow map from the sun (shader_shadows, while a mod wants it): MmlGfxFrame::sun_shadow */
    float sun_shadow_view_proj[16];   /* the world to the map's clip space (row vectors; x, y -1..1, z 0..1 its depth) */
    float sun_shadow_info[4];   /* x 1 when the map was drawn for this frame, y one texel (1 / size), z the depth's
                                   bias, w its size */
} MmlGfxConstants;
typedef struct MmlGfxView MmlGfxView;   /* a scene view as the passes get it (below) */
typedef struct MmlGfxFrame {
    uint32_t size;
    void *device;           /* ID3D11Device* (the bridge's, shared by every mod) */
    void *context;          /* ID3D11DeviceContext*: its immediate context (game thread only) */
    unsigned generation;    /* changes when the device is made anew: whatever a mod made on the old one is gone */
    const MmlGfxConstants *k;
    void *constants;        /* ID3D11Buffer* holding k, bound to b0 (VS, PS, CS) */
    void *depth;            /* ID3D11ShaderResourceView*: linear view depth, metres (t0) */
    void *color;            /* ID3D11ShaderResourceView*: the frame so far (t1) */
    void *point, *linear;   /* ID3D11SamplerState* (s0, s1) */
    void *target;           /* ID3D11RenderTargetView*: the effects target, premultiplied RGBA16F, bound with the
                               frame's viewport; cleared to 0 before the first pass */
    void *full_screen;      /* ID3D11VertexShader*: VsFullScreen (bound): Draw(3) covers the frame */
    unsigned width, height;
    float view[16], proj[16];   /* the camera's (row vectors) */
    /* (members after this one: MML_GFX_HAS(frame, member)) */
    void *ui;               /* ID3D11ShaderResourceView*: the frame's 2D pass (the HUD, the menus, draw_*) as the last
                               frame drew it, premultiplied RGBA8 (gfx_ui asked for; else NULL) */
    unsigned ui_frames;     /* 2D passes taken so far (0: none yet) */
    /* (MML_GFX_HAS(frame, views)) */
    const MmlGfxView *views;     /* every mod's scene views drawn so far (scene_view), in their order */
    unsigned view_count;
    /* (MML_GFX_HAS(frame, in_view)) */
    int in_view;            /* 0: the frame. A scene view's handle: the pass runs over that view's picture
                               (gfx_pass_views): depth, color, target, width, height, view, proj and the constants are
                               the view's; views and ui are NULL */
    /* (MML_GFX_HAS(frame, sun_shadow)) */
    void *sun_shadow;       /* ID3D11ShaderResourceView*: the sun's shadow map (R32_FLOAT, its depth; 1 nothing),
                               bound to t2, while a mod wants it (shader_shadows) and it was drawn for this frame; else
                               NULL. Its matrix and size: k->sun_shadow_view_proj, k->sun_shadow_info (frame.hlsli
                               SunShadow, SunLit). The same map in a scene view's passes (drawn around the frame's
                               camera) */
    /* (MML_GFX_HAS(frame, motion)) */
    void *motion;           /* ID3D11ShaderResourceView*: each pixel's motion in pixels (R16G16_FLOAT): this pixel plus
                               its motion is where the surface it shows was the frame before (DLSS's and TAA's
                               convention), the camera's motion and what moved by itself (cars, their wheels, doors,
                               props, people as wholes); bound to t3 (frame.hlsli g_motion), while a mod wants them
                               (gfx_motion) and the frame before was drawn; else NULL (and in the scene views) */
} MmlGfxFrame;
#define MML_GFX_HAS(f, member) ((size_t)(f)->size >= offsetof(MmlGfxFrame, member) + sizeof((f)->member))
#define MML_GFX_UI_OFF 0     /* gfx_ui: the 2D pass as the game draws it */
#define MML_GFX_UI_TAKE 1    /* ... also into MmlGfxFrame::ui (the frame unchanged) */
#define MML_GFX_UI_HIDE 2    /* ... into MmlGfxFrame::ui only: the HUD and the menus not on the frame (a VR panel) */
/* A pass may change any Direct3D 11 state: the bridge binds what it hands out again before each pass, the blend
 * state too (premultiplied over: each pass is laid over the passes before it). */
typedef void (*MmlGfxPassFn)(const MmlGfxFrame *frame, void *user);

/* A texture of a mod's 64-bit half, opened on the graphics bridge's device (gfx_shared_texture). */
typedef struct MmlSharedTexture {
    uint32_t size;          /* sizeof(MmlSharedTexture), set by the caller */
    void *texture;          /* ID3D11Texture2D* */
    void *srv;              /* ID3D11ShaderResourceView* (its format's own view; a typeless one: NULL) */
    uint32_t width, height;
    uint32_t format;        /* DXGI_FORMAT */
    uint32_t generation;    /* the half's: changes when it makes the texture anew */
} MmlSharedTexture;
typedef struct MmlChannel MmlChannel;   /* shared memory with the mod's 64-bit half (channel_open) */

/* The mission's scene drawn again, by LS3D itself, from another camera into a target of the bridge's (scene_view;
 * docs/MODDING.md "The scene from another camera"): a planar reflection (a mirrored camera and a clip plane), the
 * eyes of a headset (asymmetric frusta), a rear-view mirror, a security camera. Before the frame's own drawing, each
 * frame of the mission's world, each view's function says where its camera is; LS3D draws the whole scene from there
 * (its rooms, its lights, its culling for that camera) and the view's colour and linear depth go to the passes of the
 * same frame (MmlGfxFrame::views). The game's camera, its sound listener and its lens flares are as if nothing had
 * happened. */
#define MML_VIEW_CLIP 0x1           /* clip: only what lies on the plane's positive side is drawn (a reflection: what
                                       is above the water) */
#define MML_VIEW_OWN_SECTOR 0x2     /* the room (LS3D's sector) the view sees from is looked up at its own eye; without
                                       it the game camera's is kept (a mirrored eye is under the water, outside every
                                       room: what is seen in the water is what the game camera's room sees) */
typedef struct MmlViewCamera {
    uint32_t size;
    float world[16];        /* the camera's pose as LS3D keeps a frame's (row vectors): row 0 right, 1 up, 2 forward
                               (a left-handed basis, as the game camera's: right x up = forward), row 3 the eye
                               (metres, y up); _14, _24, _34 0, _44 1. Rows 0..2 are made orthonormal from forward
                               and up before use */
    float tan_left, tan_right;   /* the frustum's sides: a point is in view where x/z (the view's axes) is between
                                    -tan_left and tan_right ... */
    float tan_down, tan_up;      /* ... and y/z between -tan_down and tan_up. Equal pairs: a symmetric frustum.
                                    OpenXR's XrFovf: tan(-angleLeft), tan(angleRight), tan(-angleDown), tan(angleUp) */
    float near_z, far_z;    /* metres; as given: the game camera's (change them only when the view needs to) */
    float clip[4];          /* MML_VIEW_CLIP: a plane in the world, a x + b y + c z + d = 0; what has it below 0 is
                               not drawn */
    uint32_t flags;         /* MML_VIEW_* */
} MmlViewCamera;
/* Called before the view is drawn, each frame of the mission's world (game thread, behind the mod's fault guard):
 * `camera` comes filled with the game camera's pose, frustum and range (the view drawn as it is would be the frame);
 * change what differs. Return 0 to leave the view out of this frame (its last picture stays in MmlGfxFrame). */
typedef int (*MmlViewFn)(MmlViewCamera *camera, void *user);

/* The game's own camera (game_camera; G_Camera, the camera manager the game drives every frame). */
typedef struct MmlGameCamera {
    uint32_t size;
    int mode;               /* MML_GAMECAM_*: 1..3 on foot, 5 the sniper's scope, 6 a death, 7..15 in a vehicle, 0x11
                               linked to a frame, 0x13 a dialogue, 0x14 a race, 0x15 looking at, 0x16 locked, 0x17 a
                               truck's bed, 0x18 a menu's */
    char mode_name[16];     /* the game's own name for it ("P Behind", "C Chase"; "" for a mode with no record) */
    void *car;              /* the vehicle it follows (C_car), or NULL */
    float fov;              /* the field of view the game draws with, radians, across (I3D_camera's) */
    float near_z, far_z;    /* its range, metres */
    int fov_held;           /* 1: a mod's field of view is held (game_camera_fov) */
} MmlGameCamera;
#define MML_GAMECAM_BEHIND 1        /* on foot, behind the player */
#define MML_GAMECAM_EYES 3          /* on foot, from his eyes */
#define MML_GAMECAM_CAR_BEHIND 7    /* in a vehicle: behind ("C Beh. Hellboy") */
#define MML_GAMECAM_CAR_BEHIND_2 8  /* ("C Behind Emeth") */
#define MML_GAMECAM_CAR_BEHIND_3 9  /* ("C Behind3") */
#define MML_GAMECAM_CAR_WHEEL 12    /* by a front wheel ("C Chase") */
#define MML_GAMECAM_CAR_IN 13       /* from the driver's head ("C In") */
#define MML_GAMECAM_CAR_OVER 14     /* over the bonnet ("C Over") */

/* A scene view as the passes get it (MmlGfxFrame::views). */
struct MmlGfxView {
    uint32_t size;
    int handle;             /* scene_view's */
    void *color;            /* ID3D11ShaderResourceView*: what LS3D drew, RGBA8 (the game's gamma-encoded colours; its
                               alpha is whatever the game left there: not coverage) */
    void *depth;            /* ID3D11ShaderResourceView*: linear depth along the view's forward, metres, R32F; where
                               nothing was drawn, k->depth_proj.z (the sky) */
    void *color_texture;    /* ID3D11Texture2D* of `color` (to copy it: a headset's swap chain) */
    unsigned width, height;
    float view[16], proj[16];   /* the matrices it was drawn with (row vectors): a world point p is at p * view * proj */
    float eye[4];           /* the eye it was drawn from; w 0 */
    unsigned frames;        /* times it was drawn */
    int fresh;              /* 1: drawn for this frame; 0: the last one's (its function said 0, or it failed) */
};

/* The game's own geometry (geom_hide, geom_capture below). */
#define MML_GEOM_FRAME 0x1          /* geom_hide: not drawn in the frame */
#define MML_GEOM_VIEWS 0x2          /* ... not drawn in the scene views (mirrors, reflections) */
typedef struct MmlGeomGroup {
    const void *visual;     /* the LS3D visual drawn (its I3D_frame) */
    const void *group;      /* the face group (with `visual`, the key to keep it by across captures) */
    const char *texture;    /* its texture's file name, as matched */
    unsigned first, count;  /* its triangles in MmlGeomCapture::tris */
} MmlGeomGroup;
typedef struct MmlGeomCapture {
    uint32_t size;
    const float *tris;      /* count triangles, three points of three floats each: the world (metres, y up) */
    unsigned count;
    const MmlGeomGroup *groups;
    unsigned group_count;
    unsigned unreadable;    /* face groups whose buffers could not be read (not in tris) */
} MmlGeomCapture;
typedef void (*MmlGeomFn)(const MmlGeomCapture *capture, void *user);

/* A person's skeleton (human_skeleton below): the bones in this order, each an LS3D frame. */
enum {
    MML_BONE_PELVIS, MML_BONE_BACK1, MML_BONE_BACK2, MML_BONE_BACK3, MML_BONE_NECK,   /* (the head moves with the neck) */
    MML_BONE_R_SHOULDER, MML_BONE_R_ARM, MML_BONE_R_ELBOW, MML_BONE_R_HAND,
    MML_BONE_L_SHOULDER, MML_BONE_L_ARM, MML_BONE_L_ELBOW, MML_BONE_L_HAND,
    MML_BONE_R_THIGH, MML_BONE_R_SHIN, MML_BONE_R_FOOT, MML_BONE_L_THIGH, MML_BONE_L_SHIN, MML_BONE_L_FOOT,
    MML_BONE_ROOT,          /* the model's root frame: the pelvis's parent */
    MML_BONE_COUNT
};
typedef struct MmlBone {
    const char *name;       /* "pelvis", "back1"... "root" (the frame's name in the model, but pelvis and root) */
    void *frame;            /* its I3D_frame; NULL: this model has no such bone */
    float world[16];        /* its world matrix now (row vectors: rows 0..2 its axes, row 3 its origin; metres) */
    float rot[4];           /* its local rotation, w x y z, in its parent's space */
    float pos[3];           /* its local position, in its parent's space */
} MmlBone;

/* Questions to the world (world_ray, world_sweep, world_shapes below). */
#define MML_WORLD_STATIC    0x1u     /* the static world: buildings, the ground, fences, poles */
#define MML_WORLD_MOVING    0x2u     /* the moving objects: people, cars, doors... */
#define MML_WORLD_ALL       0x3u
#define MML_WORLD_NO_PEOPLE 0x100u   /* moving objects of people passed (their class bit) */
#define MML_WORLD_NO_CARS   0x200u   /* ... of cars */
typedef struct MmlHit {
    uint32_t size;          /* set by the host */
    float pos[3];           /* the point hit; a sweep's: the sphere's centre where it stops */
    float normal[3];        /* the surface's, out of it (a unit vector) */
    float fraction;         /* how far from `from` to `to`, 0..1 */
    void *actor;            /* a moving object's owner (an actor; the traffic's pedestrians: their system), NULL static */
    uint32_t flags;         /* the game's first dword of what was hit: its kind in the low byte (static: 1..7 a
                               triangle, 0x80 a box, 0x81 an axis-aligned box, 0x82 a sphere, 0x83 a box, 0x84 a pole;
                               moving: 0x40 a box, 0x42 and 0x44 spheres); a static one's surface in the top byte */
    uint32_t classes;       /* a moving object's class bits (0x100 a person, 0x200 a car), 0 for the static world */
    const void *prim;       /* the game's primitive or object */
} MmlHit;
typedef struct MmlWater {
    uint32_t size;          /* sizeof(MmlWater) */
    float height;           /* the water's surface (y) there */
    int surface;            /* 31: deep water (the river, the sea: deadly to a person on foot and to a car's crew), 44:
                               shallow water */
    float depth;            /* metres from the surface down to the next static face (the bed), -1: none within 100 m */
    int covered;            /* 1: something of the static world between the point and the water (a bridge, a pier) */
    const void *prim;       /* the water's face */
} MmlWater;
/* a prop's animation (prop_anim_state) */
typedef struct MmlPropAnim {
    char anim[64];          /* the file it was given ("" none) */
    int playing;            /* 1 while it runs (a looped one until it is stopped; one played once until its end) */
    int loop;
    int time_ms;            /* where it is (a looped one counts on past its length) */
    int length_ms;
    float weight;
} MmlPropAnim;
#define MML_PROP_ANIM_LOOP 0x1u
/* a prop made a physical object (prop_physics) */
typedef struct MmlPropPhysics {
    float mass;             /* kg (1 at least) */
    float velocity[3];      /* at the start, m/s ({0, 0, 0}: it lies still until something strikes or pushes it) */
} MmlPropPhysics;
/* a .4ds model read (model_open): its nodes, their meshes, its materials */
typedef struct MmlModelNode {
    char name[64];
    int parent;             /* the index of its parent node, -1: the model's root */
    int frame_type;         /* the .4ds frame type: 1 a visual, 2 a light, 5 a sector, 6 a dummy, 7 a target, 10 a joint
                               (a bone), 12 an occluder */
    int visual_type;        /* a visual's: 0 object, 1 lit object, 2 single mesh (skinned), 3 single morph, 4 billboard,
                               5 morph, 6 lens flare, 7 projector, 8 mirror, 9 emitter; -1 none */
    float pos[3], scale[3], rot[4];   /* in its parent; rot w x y z */
    int lods;               /* its own meshes (0: none, or another node's: instance_of) */
    int instance_of;        /* the node whose mesh it shows, -1: its own */
} MmlModelNode;
typedef struct MmlModelMesh {
    const float *pos;       /* 3 floats a vertex, in its node's space */
    const float *normal;    /* 3 a vertex */
    const float *uv;        /* 2 a vertex */
    int vertices;
    const uint16_t *index;  /* 3 a triangle */
    const uint16_t *material;   /* a triangle's material, 1-based (model_materials' index + 1), 0 none */
    int triangles;
} MmlModelMesh;
typedef struct MmlModelMaterial {
    uint32_t flags;         /* the .4ds material flags (0x40000 a diffuse texture, 0x40000000 an opacity texture...) */
    float ambient[3], diffuse[3], emissive[3];
    float opacity;
    char texture[64];       /* its diffuse picture (maps\ implied), "" none */
    char opacity_texture[64];
} MmlModelMaterial;
/* a mesh the game draws (scene_pieces, actor_meshes): one visual's triangles at its finest LOD, as LS3D holds them */
typedef struct MmlMeshPiece {
    void *visual;           /* the LS3D visual frame */
    float world[16];        /* its vertices' space to the world now (row vectors: rows 0..2 axes with scale, row 3 the
                               position); a piece holding world coordinates has the identity */
    const float *pos;       /* 3 a vertex */
    const float *normal;    /* 3 a vertex (the side a face is seen from), NULL: none */
    int vertices;
    const uint32_t *index;  /* 3 a triangle */
    const uint32_t *material;   /* a triangle's material flags (the .4ds bits: 0x10000000 two-sided, 0x20000000 a
                                   colour key's cut-outs, 0x40000000 translucent, 0x80000000 additive light) */
    int triangles;
    uint32_t version;       /* changes when its mesh or its place changed */
    int moving;             /* 1: it moved within the last second (a door, a gate) */
} MmlMeshPiece;
/* shader_add's rule: which of the game's draws take a mod's shaders, and the shaders (Direct3D 9) */
typedef struct MmlShaderRule {
    uint32_t size;          /* sizeof(MmlShaderRule) */
    const char *pattern;    /* the draws' textures' names ("2kolo*.bmp"; * and ?, without regard to case); NULL: every one */
    uint32_t kinds;         /* MML_SHADE_OBJECT | _LIGHTMAPPED | _BILLBOARD; 0: every kind */
    uint32_t classes;       /* MML_SHADE_OPAQUE | _ALPHATEST | _TRANSLUCENT | _ADDITIVE (the draw's blending); 0: every class */
    uint32_t where;         /* MML_GEOM_FRAME | MML_GEOM_VIEWS; 0: both */
    uint32_t flags;         /* MML_SHADE_ALL_PASSES: a face group's further passes too (else its first draw only) */
    const char *source;     /* HLSL the host compiles (#include "mml/game.hlsli": the constants), or NULL */
    const char *vs_entry, *ps_entry;     /* its entry points; NULL: that stage stays the game's fixed one */
    const char *vs_target, *ps_target;   /* NULL: "vs_3_0", "ps_3_0" */
    const void *vs_code; size_t vs_size; /* or compiled shaders (fxc's bytecode; mml_shaders() in a mod's build) */
    const void *ps_code; size_t ps_size;
} MmlShaderRule;
#define MML_SHADE_OBJECT      0x1u   /* objects, morphs, people (LS3D's object drawing) */
#define MML_SHADE_LIGHTMAPPED 0x2u
#define MML_SHADE_BILLBOARD   0x4u
#define MML_SHADE_OPAQUE      0x1u
#define MML_SHADE_ALPHATEST   0x2u
#define MML_SHADE_TRANSLUCENT 0x4u
#define MML_SHADE_ADDITIVE    0x8u
#define MML_SHADE_ALL_PASSES  0x1u
/* people's skinning (skin_set, skin_get): a single mesh's LOD skinned with up to 8 bones a vertex */
#define MML_SKIN_INFLUENCES 8
typedef struct MmlSkinBone {
    const char *joint;      /* the joint's name in the .4ds ("r_elbow": any joint under the mesh); NULL or "": the
                               mesh's own space (the bone that never moves) */
    float bind[16];         /* the mesh's space at rest to the joint's (row vectors: a point times it; the .4ds's bone
                               records hold LS3D's own); ignored for the mesh's own space */
} MmlSkinBone;
typedef struct MmlSkinLod {
    uint32_t vertices;      /* the LOD's vertices, in the .4ds's order (LS3D's: checked against the game's mesh) */
    uint32_t bones;
    const MmlSkinBone *bone;
    const uint8_t *count;   /* a vertex: its bones, 0..8 (0: weighted by no bone; only the LOD's last vertices may
                               be, the ones the .4ds weights by none: they stay where they are) */
    const uint16_t *index;  /* 8 a vertex: into bone */
    const float *weight;    /* 8 a vertex (the host makes each vertex's sum 1) */
} MmlSkinLod;
/* skin_get: LS3D's own skin of a model's single mesh, read from its .4ds (two bones a vertex at most), with its rest
   mesh: the starting point of a mod's own. The arrays are the host's until the mod's next skin_get. */
typedef struct MmlSkinMesh {
    char mesh[64];          /* the single mesh's name in the .4ds */
    int lods;               /* the mesh's LODs */
    MmlSkinLod skin;        /* this LOD's: bone 0 the mesh's own space, then the .4ds's records, their joints named */
    uint32_t rigid;         /* the vertices weighted by no bone (the last ones) */
    const float *pos;       /* 3 a vertex, the mesh's space at rest */
    const float *normal;    /* 3 a vertex */
    const float *uv;        /* 2 a vertex */
    const uint16_t *index;  /* 3 a triangle */
    int triangles;
} MmlSkinMesh;

/* shader_shadows: the shadow map from the sun the rules' shaders sample (game.hlsli mml_Shadow) */
typedef struct MmlShadowSettings {
    int size;               /* its texels a side: 512, 1024, 2048 (the default), 4096 */
    float extent;           /* metres from the camera to the map's edge (default 60) */
    float bias;             /* metres along the sun's ray a receiver is taken to be nearer the sun than it is, on top
                               of the slope-scaled bias the map is drawn with (0: the default, 0.05) */
} MmlShadowSettings;
/* game_files' function: one file's name (as the game opens it) and where it comes from ("a2.dta", "folder", a mod's
   id); return nonzero to stop */
typedef int (*MmlFileFn)(const char *name, const char *source, void *user);
/* A node of the pedestrians' web (web_nodes): its place, its index in the web (what the links name), the game's bits
   of its kind and its id, its links to other nodes (the first MML_WEB_LINKS of link_count). */
#define MML_WEB_LINKS 8
#define MML_WEB_ONE_WAY 0x80u    /* a link's flags: walked this way only */
typedef struct MmlWebNode {
    float pos[3];
    int index;
    uint16_t kind;
    uint16_t id;
    int link_count;
    int link[MML_WEB_LINKS];             /* the other node's index */
    uint16_t link_flags[MML_WEB_LINKS];
    float link_cost[MML_WEB_LINKS];
} MmlWebNode;
enum { MML_SHAPE_TRIANGLE = 1, MML_SHAPE_BOX = 2, MML_SHAPE_SPHERE = 3, MML_SHAPE_POLE = 4 };
typedef struct MmlShape {
    uint32_t kind;          /* MML_SHAPE_* */
    uint32_t flags;         /* as MmlHit's */
    uint32_t classes;       /* as MmlHit's */
    float corner[3][3];     /* a triangle's corners */
    float normal[3];        /* a triangle's, out of the solid */
    float center[3];        /* a box's, a sphere's; a pole's x and z (it stands from far below to far above) */
    float half[3];          /* a box's half sizes along its axes; a sphere's, a pole's radius in half[0] */
    float axes[9];          /* a box's axes (rows: x, y, z; unit vectors) */
    void *actor;            /* a moving object's owner, NULL static */
    const void *prim;       /* the game's primitive or object */
} MmlShape;

/* A person's inventory (human_items below): the slots as the game keeps them. */
enum {
    MML_SLOT_HAND = 0,      /* the item in his hand */
    MML_SLOT_BELT = 1,      /* 1..5: the belt's five weapon slots */
    MML_SLOT_COAT = 6,      /* a big weapon (a Thompson, a shotgun) */
    MML_SLOT_EXTRA = 7,     /* 7..10: four other items */
    MML_SLOT_CAR = 11,      /* the item put away while he sits in a car */
    MML_SLOT_COUNT = 12
};
typedef struct MmlItem {
    int slot;               /* MML_SLOT_* */
    int id;                 /* the item (world.items: 9 Colt 1911, 10 Thompson 1928...) */
    int loaded, reserve;    /* rounds in it, in reserve */
} MmlItem;

/* What the player did (stats_get), counted by the loader. */
typedef struct MmlStats {
    uint32_t size;          /* sizeof(MmlStats) */
    int kills;              /* people he killed (his weapons, his car), traffic pedestrians not counted */
    int deaths;             /* his own */
    int shots;              /* shots he fired (a shotgun's: one) */
    int hits;               /* hits he gave people and traffic pedestrians (bullets, blows, his car) */
    int cars_driven;        /* times he got into a car or a vehicle */
    int crashes;            /* his car's crashes of 5 m/s and more */
    int falls;              /* falls that hurt him */
    int items;              /* items that came into his inventory (picked up, given, bought) */
    float on_foot_m;        /* metres walked and run, on the ground (a fall, a teleport not counted) */
    float in_vehicle_m;     /* metres in vehicles */
    float played_s;         /* seconds of the mission's world running (menus and the pause not counted) */
    int money_earned;       /* the freeride's money gained */
    int money_spent;        /* ... and lost */
} MmlStats;

/* The profile's progress (profile_progress): the game's own, as its .sav keeps it. */
typedef struct MmlProgress {
    uint32_t size;          /* sizeof(MmlProgress) */
    int profile;            /* savegame\mafia<profile>.sav; -1: none selected */
    char name[32];          /* its name (the game's ANSI) */
    int chapter;            /* the chapter of its newest story save (1..20), 0: none */
    int newest_slot;        /* that save's slot, -1: none */
    int saves;              /* how many saves it has on disk */
    int freeride_level;     /* the freeride's level word (the manager's +0x1F0; 3 and up: the freeride's settings
                               count) */
    uint32_t race_cars;     /* the car groups unlocked for racing: bit n the letter 'A' + n */
    uint32_t steal_cars;    /* the groups the player may steal (the freeride: the race groups) */
    uint32_t freeride_cars; /* the freeride's bonus cars unlocked (bit n: 'A' + n; production cars always are) */
    int garage[25];         /* the garage's cars: the car database's index, -1 an empty place */
} MmlProgress;

/* A mod's own item (item_define). */
typedef struct MmlItemDef {
    uint32_t size;          /* sizeof(MmlItemDef) */
    const char *name;       /* the mod's name for it ("rod": letters, digits, '_', '-', '.'): its id is kept for it */
    const char *label;      /* what the game shows, UTF-8 ("Fishing rod"; NULL: the name) */
    const char *model;      /* the model put on the ground when dropped ("2rod.i3d", at most 31 characters; NULL:
                               like's) */
    int like;               /* the game's item it is made like (0: 16, a key: kept among the extra items; 17 a bucket:
                               carried, the coat's slot; 4 a baseball bat: a melee weapon...) */
    int flags;              /* MML_ITEM_* (predmety.def's flags); -1: like's */
    const char *collected;  /* the line when it is picked up (NULL: "<label> collected") */
} MmlItemDef;

/* A light of the scene (scene_lights below). */
enum { MML_LIGHT_POINT = 1, MML_LIGHT_SPOT = 2, MML_LIGHT_DIRECTIONAL = 3, MML_LIGHT_AMBIENT = 4, MML_LIGHT_FOG = 5 };
typedef struct MmlLight {
    void *frame;            /* its I3D_light (a frame of type 2): light_set's handle */
    char name[32];          /* the frame's name */
    int type;               /* MML_LIGHT_* (LS3D's types; 6 and more: others) */
    int on;                 /* it and every frame above it are on */
    float color[3];         /* 0..1 */
    float power;
    float range[2];         /* near, far (metres) */
    float cone[2];          /* a spot's inner and outer angles (radians) */
    float pos[3];           /* in the world */
    float dir[3];           /* where it shines (its z axis) */
    int sectors;            /* how many sectors (rooms) it lights */
} MmlLight;

/* A car's exhaust as the game makes it (car_exhaust, exhaust_hook below). */
#define MML_EXHAUST_PIPES 8
typedef struct MmlExhaust {
    uint32_t size;
    void *car;              /* the C_car */
    int dt_ms;              /* the tick's length (exhaust_hook; 0 from car_exhaust) */
    int type;               /* its particle type: its particles.def record and emitter (12 ordinary cars, 15 the big
                               trucks, the fire engine, racing cars); 0 none */
    int emitting;           /* the game puffs now: the engine runs, the car is slower than max_speed, it has a type */
    float jet_speed;        /* m/s out of the pipes: the idle's to the top's by the engine's turns */
    float max_speed;        /* m/s: no puffs above it */
    int pipes;              /* how many (its model's EXHAUST dummies), at most MML_EXHAUST_PIPES */
    float pos[MML_EXHAUST_PIPES][3];     /* each pipe's mouth in the world */
    float dir[MML_EXHAUST_PIPES][3];     /* the way it blows (unit) */
    float carry[MML_EXHAUST_PIPES][3];   /* the car's velocity at the pipe, as the game works it out for a puff */
    float add_birth[3], add_death[3];    /* what the type's emitter adds to a puff's velocity at its birth, its death */
    float death_share;      /* a puff's velocity at death: death_share x (jet + carry) + add_death */
} MmlExhaust;
/* exhaust_hook's function: every tick, each car whose exhaust the game is about to make. Return 1: the game's puffs
 * for this car this tick are left out (the mod makes its own); 0: the game's go on. */
typedef int (*MmlExhaustFn)(const MmlExhaust *exhaust, void *user);

/* A particle of the game's as it moves (particle_hook below): every particle the game shows is an LS3D particle
 * visual, whichever system makes it. */
enum { MML_PARTICLE_POOLED = 1, MML_PARTICLE_EMITTED = 2 };
typedef struct MmlParticle {
    uint32_t size;
    void *frame;            /* its visual (a frame): the same while its life lasts; a pooled one is reused after */
    int kind;               /* MML_PARTICLE_POOLED: the game's effects (fire, steam, vents...); _EMITTED: an LS3D
                               emitter's puff (the exhaust, dust, grass) */
    int first;              /* 1: the first move of its life the host sees */
    int dt_ms;              /* since its last move (0 at the first) */
    float pos[3];           /* where the game has just put it (world): change it and return 1 to put it there */
    float moved[3];         /* how far the game moved it since its last move (world): its own motion */
} MmlParticle;
typedef int (*MmlParticleFn)(MmlParticle *particle, void *user);

/* The weather's parameters (weather_get, weather_set below): LS3D's weather system of the mission's scene, in the
 * order of the scripts' WEATHER_SETPARAM keywords (ON, SECTORS, DUMMIES, COLOR_HIGH...). */
enum {
    MML_WEATHER_ON,         /* 0 or 1: rain (the drops) on */
    MML_WEATHER_SECTORS,    /* 0 or 1 */
    MML_WEATHER_DUMMIES,    /* 0 or 1 */
    MML_WEATHER_COLOR_HIGH, /* the drops' colour, 0xAARRGGBB, at their top */
    MML_WEATHER_COLOR_LOW,  /* ... at their bottom */
    MML_WEATHER_SPEED,      /* how fast they fall */
    MML_WEATHER_LENGTH,     /* a drop's length */
    MML_WEATHER_WIDTH,      /* its width */
    MML_WEATHER_MAX_DIST,   /* how far from the camera drops are made */
    MML_WEATHER_MAX_HEIGHT, /* how high above it */
    MML_WEATHER_MAX_COUNT,  /* how many drops at most (scaled by the player's detail setting) */
    MML_WEATHER_DIR_X, MML_WEATHER_DIR_Y, MML_WEATHER_DIR_Z,   /* the way they fall (a slant: wind) */
    MML_WEATHER_MODE,       /* 0 or 1: the reset's other preset */
    MML_WEATHER_PARAMS
};

/* A record of tables\particles.def (particle_def below): how an emitter of the game puffs. */
typedef struct MmlParticleDef {
    uint32_t size;
    char model[32];         /* the puff's model (models\<name>.4ds: one textured quad) */
    int interval_ms, interval_rand_ms;   /* from puff to puff (+ a random part) */
    int life_ms, life_rand_ms;           /* a puff's life */
    float growth_min, growth_max;        /* g: the size is times 1 + 0.01 g every 10 ms */
    float birth_cube;                    /* the cube a puff is born in (metres) */
    float dir_jitter;                    /* its end direction's jitter */
    int turn_ms, turn_rand_ms;           /* over which its direction turns to the end one (0: its life) */
    float fade_in, fade_out;             /* the share of its life the alpha ramps up; the share after which it fades */
    float spin_min, spin_max;            /* rad/s */
    float scale;                         /* the model's scale at birth */
    float tint_birth[3], tint_death[3];  /* its colour at birth and at death */
    uint32_t raw[41];                    /* the whole record (0xA4 bytes), for what has no name here */
} MmlParticleDef;

/* Sound and music (sound_play, music_play below). */
#define MML_SOUND_LOOP 1u          /* played again and again until stopped */
#define MML_SOUND_PAUSABLE 2u      /* a 2D sound held while the game is paused (a pause menu open); 3D ones always are,
                                      by LS3D, as the world's own */
typedef struct MmlSoundState {
    uint32_t size;
    int playing;            /* LS3D plays it now (a 3D sound out of its range may not be) */
    int looped, in_world;   /* MML_SOUND_LOOP; 1: a 3D sound (at a place, on a frame), 0: 2D */
    int ms, length_ms;      /* where it is in its file, the file's length (ms) */
    float volume, pitch;
    float pos[3];           /* a 3D sound's place in the world now */
} MmlSoundState;

/* A vehicle's body (vehicle_body, vehicle_hook below), in the world's axes: a car's is what the game's physics
 * integrates; an airplane's (C_airplane: the freeride Extreme's plane and saucer, two missions') is its place and its
 * rotation, its velocity the facing times its speed (it flies where it points) and no turn, unless a mod or the bridge
 * flew its last step: then the velocity and turn written then. */
typedef struct MmlVehicleBody {
    float pos[3];           /* the body's position (metres) */
    float axes[3][3];       /* forward, right, up: unit vectors (the model frame's z, x and y rows) */
    float velocity[3];      /* m/s */
    float turn[3];          /* the angular velocity, rad/s */
} MmlVehicleBody;
typedef struct MmlVehicleStep {
    uint32_t size;
    void *car;              /* the vehicle: a car (an actor of type 4) or an airplane (kind) */
    float dt;               /* this step's time (s): the frame's, cut by the game into Move's substeps */
    int player;             /* the player's car */
    MmlVehicleBody body;    /* before the step; written by the mod when it returns 1 */
    int kind;               /* the vehicle's actor type: MML_VEHICLE_CAR or MML_VEHICLE_AIRPLANE (absent in a loader
                             * before airplanes: check size; then a car) */
} MmlVehicleStep;
#define MML_VEHICLE_CAR 4
#define MML_VEHICLE_AIRPLANE 0x16
/* vehicle_kinds' bits: which vehicles a mod's vehicle_hook is told of (cars only until it asks) */
#define MML_VEHICLES_CARS 1u
#define MML_VEHICLES_AIRPLANES 2u
/* Return 1: the game's physics skipped for this vehicle's step, `body` is where it is after it; 0: the game steps.
 * An airplane's step skipped is its route's flight (its propellers, sounds and damage gauge go on; its collision
 * follows the body; the speed it is given is |velocity|, its facing the velocity's direction); given back (0), it
 * turns back to its route from where it is. */
typedef int (*MmlVehicleFn)(MmlVehicleStep *step, void *user);

/* Interaction (zone_add, use_point_add, timer_add below). inside: 1 entered, 0 left; actor: who crossed (the player,
 * or one zone_watch watches). */
typedef void (*MmlZoneFn)(int zone, int inside, void *actor, void *user);
typedef void (*MmlUseFn)(int point, void *user);
typedef void (*MmlTimerFn)(int timer, void *user);

/* What a person plays (human_anim_state). */
typedef struct MmlHumanAnim {
    uint32_t size;          /* sizeof(MmlHumanAnim) */
    int playing;            /* the game's number of the animation playing (C_human +0x70), 0: none */
    int asked;              /* the one asked for (+0x74) */
    const char *name;       /* the file the game's table gives for `playing` ("" when none or not in the table) */
    int state;              /* his state (+0x40C): 1 his own life (an animation human_anim_play started plays in it
                               too), 4 an animation with a track of its own (.tck), 6 his action queue (a knock-down,
                               a get-up), others the game's */
    int lying;              /* 1 on the back, -1 on the front, 0 neither: from his chest's axis as drawn now */
    int time_ms, length_ms; /* his animations machine's active animation (the one human_anim_play started, or his
                               own): where it is and how long it is (GetActiveAnimTime); 0 0 with none */
} MmlHumanAnim;

/* A marker on the HUD (hud_marker_add): a place in the world shown on the screen, the radar or the compass. */
typedef struct MmlHudMarker {
    uint32_t size;          /* sizeof(MmlHudMarker) */
    float pos[3];           /* in the world, or in frame's space when frame is given */
    void *frame;            /* followed (actor_frame's model, a bone from human_skeleton, a prop); NULL: the world */
    uint32_t flags;         /* MML_MARKER_* */
    uint32_t argb;          /* its colour on the screen (a picture's tint) and on the radar; 0: white */
    int picture;            /* drawn at its place on the screen (picture_load); 0: a diamond of its colour */
    float scale;            /* its height on the screen in 800x600 units; 0: 24 */
    const char *label;      /* written under it on the screen (UTF-8; NULL: none); copied */
} MmlHudMarker;
#define MML_MARKER_SCREEN   0x01   /* drawn where it is on the screen (its picture or a diamond, its label) */
#define MML_MARKER_DISTANCE 0x02   /* with its distance from the player under it ("35 m") */
#define MML_MARKER_EDGE     0x04   /* off the screen or behind the camera: held at the screen's edge, toward it */
#define MML_MARKER_RADAR    0x08   /* a mark on the game's radar (shown where the mission shows its radar) */
#define MML_MARKER_COMPASS  0x10   /* the compass' target, the compass shown (one target: the newest marker's) */

/* The frame's measures (frame_stats below): the frames Present to Present, the last 600 of them. */
typedef struct MmlFrameStats {
    uint32_t size;
    float avg_ms, median_ms, p95_ms, last_ms;   /* the frame's time */
    float mod_ms;           /* the asking mod's own time a frame (every entry into it: events, passes, its views'
                               functions, commands...), a moving average over about 60 frames */
    float views_ms;         /* the host drawing the scene views a frame (every mod's), the same average */
    float bridge_ms;        /* the graphics bridge a frame (copies, every mod's passes, waits), the same average */
    unsigned frames;        /* frames measured (0: none yet) */
} MmlFrameStats;

/* An order's state (human_order_state): where a person's own program stands. */
typedef struct MmlOrderState {
    uint32_t size;          /* sizeof(MmlOrderState) */
    int id;                 /* the order's (human_order's), 0: none of the loader's for him */
    int running;            /* 1 while his program has not reached the order's end (the loader's wait, or an `end`) */
    int line;               /* the line of the order's code it stands on (1-based), 0 at its start or its end */
    int script_drives;      /* 1: his script drives him (the flag the AI runs his program under); 0: his AI does */
    int ai_state;           /* the AI's state (C_entity +0xF40; 0 while the script drives) */
    int action;             /* 1: an action of the order runs (a move, a follow, a fire...) */
} MmlOrderState;

typedef void (*MmlEventFn)(const MmlEvent *ev, void *user);

/* A command, reachable through the bridge (and so by agents over MCP) as "<mod id>.<name>" or, for the core mod,
 * as "<name>". args: JSON text (an object, or "null"). Write the reply with reply_append (JSON text) or reply_error.
 * Returns 0 on success. Commands run on the game thread. */
typedef int (*MmlCommandFn)(const char *args_json, MmlReply *reply, void *user);

typedef struct MmlHost {
    uint32_t size;          /* sizeof(MmlHost) as the loader built it */
    uint32_t api_version;   /* MML_API_VERSION of the loader */

    /* ---- identity */
    const char *(*game_build)(void);             /* "1.2", "1.0", or "unknown" (the loader still runs: no game hooks) */
    const char *(*game_dir)(void);               /* the game's folder, with a trailing backslash */
    const char *(*mod_id)(const MmlMod *mod);
    const char *(*mod_dir)(const MmlMod *mod);   /* the mod's folder (its manifest, its files), trailing backslash */

    /* ---- log: <game>\mml\logs\mml.log (and the bridge's log tail) */
    void (*log)(const MmlMod *mod, MmlLogLevel level, const char *fmt, ...);

    /* ---- memory (any thread; never faults) */
    int (*mem_read)(uintptr_t addr, void *dst, size_t n);
    int (*mem_write)(uintptr_t addr, const void *src, size_t n);   /* code pages are unprotected for the write */
    uintptr_t (*module_base)(const char *module);                   /* "Game.exe", "LS3DF.dll"...; 0: not loaded */

    /* ---- hooks: chained. Several mods may hook one function; each detour calls *original for the rest of the chain
     * (the loader keeps *original right as hooks come and go). Removed when the mod unloads. 0 on failure. */
    int (*hook)(MmlMod *mod, uintptr_t target, void *detour, void **original);
    int (*unhook)(MmlMod *mod, uintptr_t target);

    /* ---- events */
    int (*subscribe)(MmlMod *mod, MmlEventType type, MmlEventFn fn, void *user);

    /* ---- commands (bridge, MCP) */
    int (*command)(MmlMod *mod, const char *name, const char *help, MmlCommandFn fn, void *user);
    void (*reply_append)(MmlReply *reply, const char *text, size_t len);   /* len (size_t)-1: up to the NUL */
    void (*reply_error)(MmlReply *reply, const char *message);

    /* ---- state kept across the mod's own reloads (blobs by key; gone when the game exits) */
    int (*state_put)(MmlMod *mod, const char *key, const void *data, size_t n);
    size_t (*state_get)(MmlMod *mod, const char *key, void *data, size_t cap);   /* the blob's size; 0: none */

    /* ---- services: a mod offers a versioned table of functions under a name; other mods find it. A table lives as
     * long as the mod that published it: re-find it after MML_EVENT_SERVICES_CHANGED. */
    int (*service_publish)(MmlMod *mod, const char *name, uint32_t version, const void *table);
    const void *(*service_find)(const char *name, uint32_t min_version, uint32_t *version);

    /* ---- run fn(user) on the game thread at the next frame (callable from any thread) */
    void (*post)(MmlMod *mod, void (*fn)(void *user), void *user);

    /* ---- drawing in the game's frame (see MmlFont above). Call these every frame the thing is to be seen (from
     * MML_EVENT_FRAME: every frame, the menus and the pause too; or MML_EVENT_MISSION_TICK): what is queued is
     * drawn once, in the frame being made, and dropped: right after the game's HUD, so the game's menus and console
     * are drawn over it (an overlay stays under the pause menu). A text is UTF-8 (turned into the code page of the game's
     * fonts: a character that has none there is a '?'; game::TextUtf8 turns a game text into UTF-8) and at most
     * about 100 characters a call. A frame holds 4096 items, every mod's together: past them a mod's are dropped
     * (the log tells once; draw.state `dropped_last_frame`), so a mod that draws much keeps to what is near. A
     * fault while a mod's items are drawn turns that mod's drawing off until it is reloaded (the log tells); the
     * game goes on. */
    void (*draw_text)(MmlMod *mod, float x, float y, float size, uint32_t argb, int font, int align, const char *text);
    float (*draw_text_width)(int font, float size, const char *text);   /* in the same units; 0 with no game */
    void (*draw_line)(MmlMod *mod, float x0, float y0, float x1, float y1, uint32_t argb);
    void (*draw_rect)(MmlMod *mod, float x, float y, float w, float h, uint32_t argb, int filled);

    /* ---- keys and actions, as the game got them at its last read (the keys and actions the loader injects for
     * agents, `input`, are seen the same way as real ones). A key is a DirectInput code (key_code("F5"), "Ctrl",
     * "Space", "A"...; -1: no such name). key_pressed is true for the one frame the key went down: ask in one
     * handler (FRAME or MISSION_TICK), each frame. An action is named by one of the game's binding slots
     * (action_slot("Accelerate"), "Fire", "Action"...: its primary slot; -1: no such name; `input.actions` lists
     * them): action_held and action_pressed give the action's value from every binding of it (the controls menu's
     * primary and secondary rows: the one pushed furthest, as the game takes them), whichever of its slots is
     * asked; 0..1 for a button and the axis' value for the mouse; action_pressed is nonzero in the frame it was
     * pressed. */
    int (*key_code)(const char *name);
    int (*key_down)(int key);
    int (*key_pressed)(int key);
    int (*action_slot)(const char *name);
    float (*action_held)(int slot);
    float (*action_pressed)(int slot);
    /* A hotkey: fn(user) is called (game thread, before MML_EVENT_FRAME) when the combination goes down:
     * "F5", "Ctrl+F5", "Ctrl+Shift+R" (modifiers: Ctrl, Shift, Alt, either side; exactly those held). The same
     * fn and user again: the combination replaced (a setting changed). 0: the combination is not understood. */
    int (*hotkey)(MmlMod *mod, const char *combo, MmlHotkeyFn fn, void *user);

    /* ---- the mission (MML_EVENT_MISSION_OPEN, _LOADED, _CLOSE, MML_EVENT_GAME_PAUSED, _RESUMED tell the changes) */
    const char *(*mission_name)(void);   /* the open mission's folder name ("freeitaly"); "" when none */
    int (*mission_state)(void);          /* MmlMissionState */

    /* ---- a mod's own files and settings.
     * mod_data_dir: <game>\mml\data\<id>\ (trailing backslash; made when first asked for): what the mod writes
     * (records, logs, saved state). The mod's own folder is replaced by every deploy: write nothing there.
     * Settings: config.json in the mod's folder holds the defaults (shipped with the mod); config.json in its data
     * folder holds what was changed (by config_set, by the bridge's `config.set`, by hand). The mod reads the two
     * merged. When either file changes, they are read again and the mod gets MML_EVENT_CONFIG_CHANGED. A key may
     * be a path ("overlay.x"). What config_string and config_json return stays valid until the settings change.
     * config_json: the value at the key as JSON text ("" or NULL key: the whole config); NULL when there is none.
     * config_set: value_json is JSON text ("12.5", "\"F5\"", "[1, 2, 3]", "null" removes the key); 0: not JSON,
     * or the file could not be written. */
    const char *(*mod_data_dir)(MmlMod *mod);
    double (*config_number)(MmlMod *mod, const char *key, double def);   /* a number, or a boolean as 0 / 1 */
    const char *(*config_string)(MmlMod *mod, const char *key, const char *def);
    const char *(*config_json)(MmlMod *mod, const char *key);
    int (*config_set)(MmlMod *mod, const char *key, const char *value_json);

    /* ---- the game's own menus (see MmlMenuControl above; both builds: on 1.0 the same names and ids, and 1.2's
     * kinds made as 1.0's that do the same, docs/VERSIONS.md).
     * menu_item: an item added to one of the game's menus, the way the game itself adds rows to its lists (its
     * PridejComponentPod): whenever that menu is made, a copy of the control `after_id` is put right under it with
     * this text, the window grows by its height and what is below moves down. The copy looks, sounds and is
     * walked by the keyboard as its neighbours. menu: the menu's definition name ("gamemenu": the pause menu, its
     * rows 102 Exit Game, 103 Load Game, 108 Options, 111 Resume Game; `menu.state` on the bridge shows the name
     * and the controls of any open menu). A click calls fn(id, user). Returns a handle, 0 on failure. The same
     * mod, menu, fn and user again: the text and the place replaced. The item is in the menu from its next opening.
     * menu_open: a page of the mod's own put over the open menu, as the game's Options is put over the pause menu
     * (from an item's fn, usually); with no menu open the pause menu is opened first. The game builds the page
     * from `controls` as it builds its own from a .mnu file. A click calls fn(id, user); Esc closes the page.
     * name: letters, digits, _ (other mods add items to "<mod id>_<name>"). One page a mod at a time. 0: refused.
     * menu_close: the mod's page closed (from its fn: a Back button). menu_set_text: a control's text on the
     * mod's page (a picture's file). menu_set_value: a control's value by its own setter (slid, pbar, wins: 0..1;
     * chck, bslb: 0 or 1; bsel: the option's index; iten: the number); menu_value reads it (what the player made
     * it). menu_set_range: a number's ("iten") range. menu_text: a text field's ("edit") text, or any control's.
     * All of them may be called right after menu_open: what is set before the game has made the page is applied
     * when it is made. 1: done, 0: no such control on the mod's page, or a kind without it. */
    int (*menu_item)(MmlMod *mod, const char *menu, int after_id, const char *text, MmlMenuFn fn, void *user);
    int (*menu_open)(MmlMod *mod, const char *name, const MmlMenuControl *controls, int count, MmlMenuFn fn, void *user);
    void (*menu_close)(MmlMod *mod);
    int (*menu_set_text)(MmlMod *mod, int id, const char *text);
    int (*menu_set_value)(MmlMod *mod, int id, float value);
    int (*menu_value)(MmlMod *mod, int id, float *value);
    int (*menu_set_range)(MmlMod *mod, int id, int min, int max);
    int (*menu_text)(MmlMod *mod, int id, char *buf, int size);

    /* ---- the game's script machine (see MmlScriptResult above; both builds; while a mission runs).
     * script_run: `code` compiled at once (a line that does not compile: 0, the log tells which), then run from the
     * next mission tick; acts[i] (null: none) into act[i], flts[i] into flt[i]; timeout_ms: stopped after that long
     * (0: 30 s). fn(result, user) is called once it ends, on the game thread (fn may be null). Returns a handle,
     * 0 when refused. script_stop: stopped now (fn told, MML_SCRIPT_STOPPED). script_state: MML_SCRIPT_* (or -1:
     * no such handle of the mod's). A mod's programs are stopped when it unloads (fn not called). */
    int (*script_run)(MmlMod *mod, const char *code, void *owner, void *const *acts, int act_count, const float *flts,
                      int flt_count, uint32_t timeout_ms, MmlScriptFn fn, void *user);
    void (*script_stop)(MmlMod *mod, int handle);
    int (*script_state)(MmlMod *mod, int handle);

    /* ---- the world: actors made new, moved, armed, by the game's own functions (both builds; while a mission is
     * open; game thread). Positions are the world's, metres, +y up; a direction is where the actor faces (its
     * length does not matter; y is ignored for a person).
     * actor_spawn: a new actor of `kind` (MML_ACTOR_CAR, MML_ACTOR_HUMAN) made as the scripts' actor_duplicate
     * makes its copy: the model file opened by the game's model cache, the actor made by the mission, initialised
     * and added to the game's temporary actors (ticked, hit, driven like the mission's own). model: a file of the
     * game's models ("fordtud00.i3d", "Paulie.i3d"; ".i3d" is added when missing) or, for a car, its name in the
     * game's car list ("Bolt Ace Tudor"; `world.cars` lists them). A car is set on its wheels; a person stands
     * (the game's NPC kind, with no script: he does nothing until told, by script_run). dir null: +z. Returns the
     * actor, or null (the log tells why: no such model, not a person's model, no mission).
     * actor_remove: an actor made by actor_spawn (any mod's, or the bridge's) taken out of the world (the game
     * deletes it at its next tick); 0: not one of those, or the player sits in it. Spawned actors stay when their
     * mod unloads; a mission's close takes them all.
     * actor_teleport: an actor moved (the scripts' actor_setpos: a car stopped; a person in a car moves the car),
     * turned to `dir` when not null; a car is also set on its wheels and repaired (car_repair). 1: done.
     * item_id: an item of tables\predmety.def by its name ("Thompson", "colt 1911": the case is ignored, a part of
     * the name will do when only one item has it) or its number as text ("10"); -1: none.
     * give_item: a weapon or an item given (human_addweapon): a person holds it at once, the player gets it in his
     * inventory; loaded, reserve: the rounds (-1: a full magazine, the item's usual reserve). 1: done.
     * actor_seat: a person put into a car's seat at once, with no walk and no door (human_force_settocar, how the
     * missions' passengers appear): 0 the driver's, 1 beside him, 2 and 3 behind. 1: done. */
    void *(*actor_spawn)(MmlMod *mod, int kind, const char *model, const MmlVec3 *pos, const MmlVec3 *dir);
    int (*actor_remove)(MmlMod *mod, void *actor);
    int (*actor_teleport)(MmlMod *mod, void *actor, const MmlVec3 *pos, const MmlVec3 *dir);
    int (*item_id)(const char *name);
    int (*give_item)(MmlMod *mod, void *human, int item, int loaded, int reserve);
    int (*actor_seat)(MmlMod *mod, void *human, void *car, int seat);

    /* ---- a mod's own files in the game (docs/MODDING.md "A mod's own files in the game"; both builds). The game
     * opens every file by a name relative to its folder ("models\crate.4ds", "maps\crate.bmp", "tables\credits.def",
     * "sounds\x.wav", "anims\x.5ds", "tables\menu\gamemenu.mnu"), from its folder or its .dta archives. Each file
     * under the mod's folder's `files\` is a game file by its path there (files\maps\crate.bmp is "maps\crate.bmp"):
     * while the mod is loaded the game opens the mod's file for that name, whether the game has one (replaced) or not
     * (new); when it unloads, the game's own is read again. Nothing to call: the folder is enough (a mod of files
     * alone needs no DLL: "type": "files" in mod.json). Two mods with one name: the later in load order answers
     * (a higher load_order; the same: the id later in the alphabet). A file is read when the game opens it: what
     * the game keeps once read (a texture or a model in use, a table read at its start) is not read again by itself
     * (docs/MODDING.md says where the engine reads again; MML_EVENT_FILE_CHANGED tells a mod when to make it).
     * file_add: a game file answered from a file of the mod's choosing (path: absolute, or relative to the mod's
     * folder; a file it wrote in its data folder: a patched copy of a game's table), in the mod's place in the order,
     * over its own `files\` of that name; path NULL or "": taken back. Looked at for changes as the folder is.
     * 1: done, 0: refused (no such file, a name outside the game's folder).
     * file_source: who answers a name now: 1 a mod's file (its full path written into path), 0 the game's own.
     * file_read: a file read as the game reads it (by its name, from a mod's file or the archives: flags 0), or the
     * game's own whatever the mods ship (MML_FILE_GAME): the whole size, cap bytes of it copied into buf (buf may be
     * NULL to ask the size); -1: the game has no such file. Any thread. */
    int (*file_add)(MmlMod *mod, const char *name, const char *path);
    int (*file_source)(const char *name, char *path, int size);
    int64_t (*file_read)(MmlMod *mod, const char *name, void *buf, size_t cap, int flags);

    /* ---- the frame's drawing (docs/MODDING.md "Drawing in the 3D world"; both builds). render_device: the game's
     * Direct3D device: version 8 its IDirect3DDevice8, 9 its IDirect3DDevice9 (the game run on Direct3D 9 by a
     * wrapper, dxwrapper's d3d8to9 as in the labs; NULL on plain Direct3D 8). No reference is added: valid while the
     * game keeps the device (ask again each frame). render_camera: the frame scene's camera as the device has it
     * now (during RENDER_WORLD: the world's); 1: done, 0: no device yet. Game thread. */
    void *(*render_device)(int version);
    int (*render_camera)(MmlRenderCamera *out);

    /* ---- effects over the frame: the graphics bridge (docs/MODDING.md "Effects over the frame"; both builds; the
     * game on Direct3D 9Ex through a wrapper, as in the labs). One Direct3D 11 device on the game's adapter, shared
     * by every mod. While a pass is registered, each frame of the mission's world: the frame's depth buffer is a
     * readable one, and once the world is drawn (after RENDER_WORLD's mods) its colour and linear depth are handed to
     * every pass, in `order` (lower first; the same: registration), all drawing into one effects target that is then
     * composited over the frame, under the HUD. gfx_pass: a handle (> 0), 0 refused (no such fn; the log says why);
     * gfx_pass_remove takes one back; a mod's passes go with its unload. A pass runs on the game thread behind the
     * mod's fault guard; a fault unloads the mod. */
    int (*gfx_pass)(MmlMod *mod, int order, MmlGfxPassFn fn, void *user);
    void (*gfx_pass_remove)(MmlMod *mod, int handle);
    /* The frame's 2D pass (the HUD, the menus over the world, the loader's draw_*) for the mod's passes: mode
     * MML_GFX_UI_TAKE or _HIDE (the strongest any mod asks for counts), _OFF taken back (and at the unload). The 2D
     * pass is drawn after the passes, so they get the last frame's. In the mission's world only, like the passes. */
    void (*gfx_ui)(MmlMod *mod, int mode);

    /* ---- the scene from another camera (see MmlViewCamera above; both builds; the graphics bridge's Direct3D 9Ex).
     * scene_view: a view of the mission's scene into a target of width x height pixels (0, 0: the frame's size),
     * drawn each frame of the mission's world before the frame itself, in the order views were made; fn(camera,
     * user) places it. Its colour and depth reach the passes (MmlGfxFrame::views; a mod with views has a pass and
     * says "gfx": true in its mod.json). Each view is a whole second drawing of the scene by LS3D: about the frame's
     * own cost. Returns a handle (> 0), 0 refused (no fn, a size over 8192, eight views already; the log says why).
     * scene_view_remove takes one back; a mod's views go with its unload. MML_EVENT_VIEW_WORLD is sent inside each
     * view's drawing. */
    int (*scene_view)(MmlMod *mod, int width, int height, MmlViewFn fn, void *user);
    void (*scene_view_remove)(MmlMod *mod, int handle);
    /* A pass of the mod's (gfx_pass's handle) also run over every scene view's picture, so its effect is seen in
     * mirrors and reflections (on 1, off 0): at the view's stage 1, with MmlGfxFrame::in_view its handle and everything
     * in the frame the view's; its target blended over the view's colour before any pass gets the view. Returns 1, 0
     * no such pass of the mod's. A pass that shows views (a mirror's) leaves this off. */
    int (*gfx_pass_views)(MmlMod *mod, int pass, int on);

    /* ---- the game's own geometry, left out of LS3D's drawing or captured (both builds; docs/MODDING.md "The game's
     * own geometry hidden or replaced"). A face group is a part of a mesh drawn with one material; it is matched by
     * the file name of its material's texture as LS3D draws it now (the animation's frame of an animated one), without
     * folders, case-insensitive, '*' any run and '?' one character: "00test*.bmp". geom.survey on the bridge lists
     * what the game draws. Only LS3D's drawing is touched: the game's collision, its logic and its memory are not.
     * geom_hide: every face group whose texture matches is not drawn in the drawings `where` names (MML_GEOM_*, 0:
     * all); a handle (> 0), 0 refused. geom_show takes one back (0: all the mod's); a mod's go with its unload. */
    int (*geom_hide)(MmlMod *mod, const char *pattern, int where);
    void (*geom_show)(MmlMod *mod, int handle);
    /* geom_capture: at the next frame of the mission's world, every face group the frame draws whose texture matches
     * (hidden or not) is read from the buffers LS3D draws it from, in the world's space; fn gets them after that
     * frame's drawing (game thread, behind the mod's fault guard), once. What LS3D did not draw that frame (out of
     * view, culled) is not in it: ask again as the camera moves and keep the groups by their `group` key. 1: asked;
     * 0 refused (no fn, an empty pattern). */
    int (*geom_capture)(MmlMod *mod, const char *pattern, MmlGeomFn fn, void *user);

    /* ---- measuring the frame (docs/MODDING.md "Measuring the frame"): the frame's times and the mod's own cost, so
     * a heavy mod can lower its quality when the frame runs over its budget. Fills `out` (set out->size first); 1 done.
     * prof.state, prof.sample and MCP's `profile` show every mod and the game thread's time. */
    int (*frame_stats)(MmlMod *mod, MmlFrameStats *out);

    /* ---- people and their skeletons (both builds; docs/MODDING.md "People, skeletons, poses").
     * humans_list: every person the game made and has not destroyed (C_human: the player, the mission's people, those
     * spawned), up to `max` into out; returns how many there are. human_skeleton: his bones (MML_BONE_COUNT of them,
     * in MML_BONE_* order) into out (max at least MML_BONE_COUNT); 0: not a person known, or no skeleton.
     * bone_set: a bone's local rotation (w x y z; NULL: as it is) and position (NULL: as it is) written, its world
     * matrix and its children's made again. The game's animation sets the bones every frame: a pose a mod wants drawn
     * is written in MML_EVENT_RENDER_BEGIN, every frame (the game's logic and collision are not moved by it). */
    int (*humans_list)(MmlMod *mod, void **out, int max);
    int (*human_skeleton)(MmlMod *mod, void *human, MmlBone *out, int max);
    int (*bone_set)(MmlMod *mod, void *frame, const float *rot, const float *pos);
    void *(*human_player)(MmlMod *mod);   /* the player (his C_human), NULL with no mission */

    /* ---- the camera (both builds; docs/MODDING.md "The camera").
     * camera_override: the frame drawn from a camera of the mod's: fn(camera, user) is called each frame of the
     * mission's world before anything is drawn, `camera` filled as the game's (as a scene view's function gets it);
     * change it and return 1 to draw this frame from it (0: the game's). One override at a time, the last mod's that
     * asked; fn NULL gives it back (and the mod's unload). Only the drawing: the game's camera logic, the aim and the
     * sound's listener stay the game's; the scene views start from the overridden camera.
     * camera_project: a world point on the screen of the last frame drawn (800x600 units; screen[2] its linear
     * depth): 0 behind the camera or no frame yet. camera_ray: the ray through a screen point (800x600 units), from
     * the eye, its direction a unit vector. */
    int (*camera_override)(MmlMod *mod, MmlViewFn fn, void *user);
    int (*camera_project)(MmlMod *mod, const float world[3], float screen[3]);
    int (*camera_ray)(MmlMod *mod, float x, float y, float origin[3], float dir[3]);

    /* ---- questions to the world: the game's own collision tests (both builds; docs/MODDING.md "Questions to the
     * world"). `what`: MML_WORLD_STATIC (buildings, the ground, fences, poles), MML_WORLD_MOVING (people, cars,
     * doors: the game's moving objects), both; MML_WORLD_NO_PEOPLE, MML_WORLD_NO_CARS leave those out. `ignore`:
     * an actor whose own objects are passed (the shooter's, the player's), or NULL. 1: hit, filled; 0: nothing (or no
     * mission). Called on the game thread (any event of the mod's, a command). */
    int (*world_ray)(MmlMod *mod, const float from[3], const float to[3], uint32_t what, void *ignore, MmlHit *hit);
    /* a sphere of `radius` moved from `from` to `to`: where it stops (hit->pos: its centre there), what it touched */
    int (*world_sweep)(MmlMod *mod, const float from[3], const float to[3], float radius, uint32_t what, void *ignore, MmlHit *hit);
    /* the actors within `radius` of a point (their model's world position), nearest first; `type` 0 any, else the
     * game's actor type (2 a person, 4 a car); returns how many there are (more than `max`: the first max given) */
    int (*world_near)(MmlMod *mod, const float at[3], float radius, int type, void **out, int max);
    /* the collision shapes in a box (the world's axes): the static world's primitives and the moving objects, as
     * the game keeps them in its grids; returns how many there are (more than `max`: the first max given) */
    int (*world_shapes)(MmlMod *mod, const float lo[3], const float hi[3], uint32_t what, MmlShape *out, int max);

    /* ---- the game's state (both builds; docs/MODDING.md "The game's state: money, health, the inventory").
     * money: the freeride's money (the HUD's $; the story has none), INT32_MIN with no game; money_set: through the
     * game's own setter, the HUD told when it shows it. */
    int (*money)(MmlMod *mod);
    int (*money_set)(MmlMod *mod, int value);
    /* a person's health (the player: 0..200 in the freeride, his maximum in *max), -1 for no person; health_set
     * writes it (above 0, at most his maximum: the game's hits and deaths stay the game's) */
    float (*human_health)(MmlMod *mod, void *human, float *max);
    int (*human_health_set)(MmlMod *mod, void *human, float health);
    /* a person's inventory, slot by slot (MmlItem: the slot, the item's id in world.items' table, rounds in it and in
     * reserve); returns how many slots hold something */
    int (*human_items)(MmlMod *mod, void *human, MmlItem *out, int max);
    /* the rounds of the item in a slot (loaded, reserve; -1 leaves one as it is) */
    int (*item_ammo_set)(MmlMod *mod, void *human, int slot, int loaded, int reserve);
    /* an item taken from a person (the scripts' human_delweapon: every slot with it; his weapon model changed) */
    int (*human_take_item)(MmlMod *mod, void *human, int item);

    /* ---- the scene's lights (both builds; docs/MODDING.md "The scene's lights"). The level's light frames as LS3D
     * keeps them (the world itself is lit ahead of time: lightmaps; the lights light people, cars and objects as
     * they move). scene_lights: every light of the mission's scene; returns how many there are. */
    int (*scene_lights)(MmlMod *mod, MmlLight *out, int max);
    /* a light changed through LS3D's own setters: color (NULL: kept), power, near and far range (NAN: kept), on (1, 0;
     * -1 kept) */
    int (*light_set)(MmlMod *mod, void *light, const float color[3], float power, float range_near, float range_far, int on);
    /* the scene's sun (its brightest directional light that is on: towards it, its colour times its power) and the
     * ambient light beside it; 0: none */
    int (*scene_sun)(MmlMod *mod, float to_sun[3], float sun_color[3], float ambient[3]);

    /* ---- the game's effects (both builds; docs/MODDING.md "The game's effects"). car_exhaust: a car's exhaust now
     * (its pipes, its jet, whether the game puffs); 0: not a car. exhaust_hook: fn told every tick of every car whose
     * exhaust the game makes, before it does (fn NULL: no more; the last mod that asked replaces the one before). */
    int (*car_exhaust)(MmlMod *mod, void *car, MmlExhaust *out);
    int (*exhaust_hook)(MmlMod *mod, MmlExhaustFn fn, void *user);
    /* tables\particles.def: how many records (particle_def(mod, -1, NULL)), one read; particle_def_set writes the named
     * fields of `def` into record `index` (not `model`, not `raw`): the game hands the records to its emitters when it
     * makes them, at a mission's load, so a change shows from the next mission (set it in MML_EVENT_MISSION_OPEN) */
    int (*particle_def)(MmlMod *mod, int index, MmlParticleDef *out);
    int (*particle_def_set)(MmlMod *mod, int index, const MmlParticleDef *def);
    /* fn told of every move of every particle of the game's, after the game moved it and before LS3D takes the move
     * (fn NULL: no more; the last mod that asked replaces the one before): a wind, PhysX smoke moving the game's own
     * particles, a particle counter */
    int (*particle_hook)(MmlMod *mod, MmlParticleFn fn, void *user);
    /* the weather (MML_WEATHER_*): a parameter read (a flag 0/1, an int, a colour 0xAARRGGBB or a float, as a double)
     * and set through LS3D's own weather functions, as the scripts' weather_setparam does (On and MaxCount applied
     * again for the detail setting, as it does); weather_reset: the scene's weather back to its preset. 0: no mission */
    int (*weather_get)(MmlMod *mod, int param, double *value);
    int (*weather_set)(MmlMod *mod, int param, double value);
    int (*weather_reset)(MmlMod *mod);

    /* ---- props: models in the world that are not actors (both builds; docs/MODDING.md "Props"). prop_create: a model
     * of the game's (or a mod's own files\models\) made as an LS3D frame, in the world (parent NULL: pos is the world's)
     * or attached to a frame (a bone from human_skeleton, actor_frame's model, another prop: pos, rot and scale are in
     * that frame's space); rot w x y z (NULL: none), scale (<= 0: 1). Returns the prop (its frame) or NULL. The mod's
     * props go with its unload, every prop with the mission; one on an actor that is destroyed is put into the world. */
    void *(*prop_create)(MmlMod *mod, const char *model, void *parent, const float pos[3], const float rot[4], float scale);
    /* moved, turned, scaled in its parent's space (NULL, scale <= 0: as it is) */
    int (*prop_place)(MmlMod *mod, void *prop, const float pos[3], const float rot[4], float scale);
    /* given another parent (NULL: the world); keep_world 1: it stays where it is in the world */
    int (*prop_attach)(MmlMod *mod, void *prop, void *parent, int keep_world);
    int (*prop_show)(MmlMod *mod, void *prop, int on);
    int (*prop_remove)(MmlMod *mod, void *prop);
    /* an actor's model frame (a person's, a car's: a parent for props, FindChild's root), NULL for none */
    void *(*actor_frame)(MmlMod *mod, void *actor);
    /* any frame's world matrix now (row vectors: rows 0..2 its axes with their scale, row 3 its position) */
    int (*frame_world)(MmlMod *mod, void *frame, float out[16]);

    /* ---- sound (both builds; docs/MODDING.md "Sound"). sound_play: a sound of the game's (its archives' sounds\...) or
     * a mod's own (files\sounds\...) played as LS3D plays the game's: "sounds\\x.wav", or "x.wav" (sounds\ put before
     * it). In 2D (frame and pos NULL: heard the same everywhere, as the menus' own sounds, in a menu too), at a place
     * in the world (pos), or following a frame (a bone, an actor's model from actor_frame, a prop; pos in its space,
     * NULL its origin). volume 0..1, pitch a factor (1 as recorded; 2 an octave up), range_near / range_far in metres
     * (3D: full volume to near, nothing past far; <= 0: 5 and 40), flags MML_SOUND_*. Returns the sound's id, 0 when
     * it could not be played (the log says why). A sound that is not looped ends by itself (its id is then unknown);
     * the mod's sounds are stopped at its unload, the 3D ones at the mission's close. */
    int (*sound_play)(MmlMod *mod, const char *file, void *frame, const float pos[3], float volume, float pitch, float range_near,
                      float range_far, uint32_t flags);
    /* volume, pitch changed while it plays (NAN: kept): an engine's note following its turns */
    int (*sound_set)(MmlMod *mod, int sound, float volume, float pitch);
    /* moved (frame NULL: to pos in the world) or attached to another frame (pos in its space) */
    int (*sound_move)(MmlMod *mod, int sound, void *frame, const float pos[3]);
    int (*sound_stop)(MmlMod *mod, int sound);
    /* 1 while it is known: out (may be NULL) its state */
    int (*sound_state)(MmlMod *mod, int sound, MmlSoundState *out);
    /* ---- music: an .ogg (or .mp3) streamed as the game streams its own music (LS3D's IShow), in a menu too; volume
     * 0..1 times the options' music volume. Returns its id, 0 when it could not be opened. */
    int (*music_play)(MmlMod *mod, const char *file, float volume, uint32_t flags);
    int (*music_set)(MmlMod *mod, int music, float volume);
    int (*music_stop)(MmlMod *mod, int music);
    /* the game's own city music: 0 faded out and kept off (C_game::CityMusicPaused), 1 let play again. -1 when no
     * mission; the mod's choice is undone at its unload. */
    int (*music_game)(MmlMod *mod, int on);

    /* ---- control (both builds; docs/MODDING.md "Control"). key_capture: the key kept from the game while on (the game
     * reads it up; the mod still sees it: key_down, key_pressed, its hotkeys). action_capture: the action a slot
     * belongs to (action_slot's), its primary and secondary bindings both, kept from the game: the game reads 0,
     * action_held / action_pressed still give what the player does. Let go with on 0 and at the mod's unload.
     * 0: not known (the key or the slot). */
    int (*key_capture)(MmlMod *mod, int key, int on);
    int (*action_capture)(MmlMod *mod, int slot, int on);
    /* the player's own control locked as the scripts' PLAYER_LOCKCONTROLS does (C_player::LockControls): he stands,
     * the game's walking, shooting and driving leave him alone, the controls are still read; the mod moves him.
     * 0 gives it back (also at the mod's unload; a new mission starts free). */
    int (*player_lock)(MmlMod *mod, int on);
    /* a car's physics replaced: fn is told of every car's step (C_Vehicle::Engine) and may move the body itself
     * (return 1). One mod at a time (a second replaces the first; NULL: none). */
    int (*vehicle_hook)(MmlMod *mod, MmlVehicleFn fn, void *user);
    /* a car's body read (read, may be NULL) and/or written (write, may be NULL) now */
    int (*vehicle_body)(MmlMod *mod, void *car, MmlVehicleBody *read, const MmlVehicleBody *write);

    /* ---- interaction (both builds; docs/MODDING.md "Interaction"). zone_add: a sphere (pos in the world, or in a
     * frame's space when frame is given: it follows the frame) the player enters and leaves, on foot or in his car:
     * fn at each crossing (game thread, a frame's). use_point_add: one of the game's own using objects (what Action
     * finds on foot: doors, the cars' seats): within radius (horizontal; 1.5 m up or down) Action offers it as the
     * game offers a door, with the others near it in the game's own choice menu (label: its line, UTF-8), and fn is
     * called when the player takes it; the car beside it is still the game's. A mission must be loaded. timer_add:
     * fn after ms of the mission's time (it stands while the game is paused or loads), again every ms when repeat.
     * Each returns its id (0: refused, the log says why); interact_remove takes any of them away. A mod's go with
     * its unload, every one with the mission's close, one on a frame of an actor with the actor. */
    int (*zone_add)(MmlMod *mod, const float pos[3], float radius, void *frame, MmlZoneFn fn, void *user);
    int (*use_point_add)(MmlMod *mod, const float pos[3], float radius, void *frame, const char *label, MmlUseFn fn, void *user);
    int (*timer_add)(MmlMod *mod, int ms, int repeat, MmlTimerFn fn, void *user);
    int (*interact_remove)(MmlMod *mod, int id);

    /* ---- the HUD (both builds; docs/MODDING.md "The HUD"). Pictures: picture_load opens a texture by the game's own
     * loader (IGraph::CreateITexture, as the HUD opens its own): a file of the game's maps\ (the archives, or a mod's
     * files\Maps\), a .tga with its alpha, a .bmp opaque or with alpha_file (its "+" twin: "icon.bmp", "icon+.bmp");
     * its handle, 0: not loaded (the log says why). It stays until picture_free or the mod's unload; the same file
     * twice is the same texture. picture_size: its pixels. draw_picture: as draw_rect, the picture on it, turned by
     * angle (degrees, clockwise, about its centre), tinted by argb (0xFFFFFFFF: as it is), uv the part of it
     * ({u0, v0, u1, v1}; NULL: all). */
    int (*picture_load)(MmlMod *mod, const char *file, const char *alpha_file);
    int (*picture_size)(int picture, int *width, int *height);
    void (*picture_free)(MmlMod *mod, int picture);
    void (*draw_picture)(MmlMod *mod, int picture, float x, float y, float w, float h, float angle, uint32_t argb, const float uv[4]);
    /* Messages in the game's style: hud_console a line of the game's console (bottom left; five lines, 5 s each, at
     * most 63 characters; UTF-8, its colour argb's RGB); hud_help a line of its help (the mission's hints, above the
     * crosshair): the ms it is shown, 0: not shown. */
    int (*hud_console)(MmlMod *mod, const char *text, uint32_t argb);
    int (*hud_help)(MmlMod *mod, const char *text);
    /* Markers (MmlHudMarker above): hud_marker_add its id (0: refused, the log says why), hud_marker_set changes it,
     * hud_marker_remove takes it away. A mod's go with its unload, every one with the mission's close, one on an
     * actor's frame with the actor; the compass goes back to the mission's target when the marker that took it goes. */
    int (*hud_marker_add)(MmlMod *mod, const MmlHudMarker *marker);
    int (*hud_marker_set)(MmlMod *mod, int marker, const MmlHudMarker *m);
    int (*hud_marker_remove)(MmlMod *mod, int marker);

    /* ---- a mod's state in the game's saves (both builds; docs/MODDING.md "A mod's state in the saves"). Blobs by
     * key, the mod's own. MML_SAVE_SLOT: the state of the game being played: written beside each save the game
     * writes (MML_EVENT_GAME_SAVING asks for it just before), read back when that save is loaded
     * (MML_EVENT_GAME_LOADED), empty in a new game and a freeride. MML_SAVE_PROFILE: the profile's, across its saves
     * (written at once). save_put with data NULL (or n 0) removes the key; save_get returns the blob's size (0:
     * none), copying at most cap bytes. Files: savegame\mafia<profile>.<slot>.mml, savegame\mafia<profile>.sav.mml;
     * the game deleting a profile deletes them too. */
    int (*save_put)(MmlMod *mod, int scope, const char *key, const void *data, size_t n);
    size_t (*save_get)(MmlMod *mod, int scope, const char *key, void *data, size_t cap);

    /* ---- people's animations and getting up (both builds; docs/MODDING.md "Animations, getting up"). human_anim_play:
     * an animation played as a mission plays one (C_human::Do_PlayAnim, the scripts' person_playanim): a file of
     * anims\ (the archives', or a mod's files\anims\; "x.i3d" or "x"), cross-faded from what he does; at its end he
     * goes back to his own. flags MML_ANIM_SIMPLE: the scripts' SIMPLE. Returns its length in ms (0: no such file),
     * -1: refused (not a person). human_anim_stop: his own animation again at once. human_anim_replace: one of his own
     * animations (walking, standing...: "ssss.i3d") played as another from now on (C_human::ChangeAnim); with NULL his
     * own again (also at the mod's unload). human_anim_state: what he plays. human_get_up: a body a mod posed handed
     * back to the game: he is put where it lies and turned the way it lies, and plays the game's own get-up from the
     * back or the front (his action queue, as a knock-down queues it); the host draws from the body's pose into the
     * animation over blend_ms (0: at once). A mod stops posing him before. Game thread. */
    int (*human_anim_play)(MmlMod *mod, void *human, const char *anim, int flags);
    int (*human_anim_stop)(MmlMod *mod, void *human);
    int (*human_anim_replace)(MmlMod *mod, void *human, const char *own, const char *with);
    int (*human_anim_state)(MmlMod *mod, void *human, MmlHumanAnim *out);
    int (*human_get_up)(MmlMod *mod, void *human, int blend_ms);

    /* ---- a mod's own items (both builds; docs/MODDING.md "A mod's own items"). item_define: an item of the mod's own
     * added to the game's items (tables\predmety.def's records, as the game loaded them), made like one of the game's
     * (its record copied: its kind, where it is kept, how it is held), with its label (what the game shows: the
     * pickup menu, the line when it comes into the hand, "... collected"), its model (the model the game puts on the
     * ground when it is dropped; models\, or the mod's files\models\) and its flags. Its id (35 and up) is kept for
     * the mod's name of it across runs (mml\items.json), so a save holding it loads it again; the same name again:
     * the same id, the record changed. It is then given, taken, dropped and picked up as the game's own (world_give,
     * human_take_item, human_items, ITEM_GAINED). Returns the id, 0: refused (the log says why). item_find: the id
     * of "mod/name" (any mod's; "name" alone: this mod's), 0: none. Game thread. */
    int (*item_define)(MmlMod *mod, const MmlItemDef *def);
    int (*item_find)(MmlMod *mod, const char *name);

    /* ---- statistics and the profile's progress (both builds; docs/MODDING.md "Statistics, the profile's progress").
     * stats_get: what the player did, counted by the loader from the events (the game keeps no statistics):
     * MML_SAVE_SLOT the game being played (kept in its saves, read back with them; a new game and a freeride start
     * at 0), MML_SAVE_PROFILE the profile's totals (kept beside its .sav). profile_progress: the game's own progress
     * of the profile selected (its .sav: the freeride's level, the car groups unlocked, the garage), and the chapter
     * its newest save reached. profile_saves: the slots of its saves on disk (savegame\mafia<p>.<slot>; slot 0..599,
     * named by the game's text 5000 + slot), ascending; returns how many there are. Game thread. */
    int (*stats_get)(MmlMod *mod, int scope, MmlStats *out);
    int (*profile_progress)(MmlMod *mod, MmlProgress *out);
    int (*profile_saves)(MmlMod *mod, int *slots, int max);

    /* ---- the water, zones for everyone (both builds; docs/MODDING.md "Questions to the world", "Interaction").
     * world_water: the game's water under a point (x, z; looked for down from 2 m above at's y): the static world's
     * faces of the water's surfaces (31 the river and the sea: deadly, 44 shallow water a car drives through), as
     * the game finds them under a person and a car. 1: found, filled; 0: none within 200 m down (or no mission).
     * zone_watch: who a zone watches (zone_add: the player): MML_ZONE_PLAYER, _PEOPLE (every person the loader knows,
     * the player too), _CARS (every car), _ACTOR (that actor alone); the zone's function is told of each one's
     * crossing with that actor. 1: done. Game thread. */
    int (*world_water)(MmlMod *mod, const float at[3], MmlWater *out);
    int (*zone_watch)(MmlMod *mod, int zone, uint32_t who, void *actor);

    /* ---- a prop's animation and physics (both builds; docs/MODDING.md "Props"). prop_anim_play: an animation of
     * anims\ (the archives', or a mod's files\anims\; "x.i3d" or "x") played on the prop's model as the scripts'
     * model_playanim plays one (LS3D's SetAnimation; the mission's tick steps it): its frames move the model's
     * frames of the same names (a door's leaf, a person's bones on a person's model). flags MML_PROP_ANIM_LOOP,
     * weight 0..1 (<= 0: 1). Returns its length in ms, 0: no such file, -1 refused. prop_anim_stop: the model stays
     * in the pose it reached. prop_physics: a prop in the world (not attached) made a physical object as the
     * scripts' create_physicalobject makes one (a box from its model's bounds among the collision's moving objects:
     * it falls, people and cars strike and stand on it, bullets hit it); NULL: no longer (it stays where it lies).
     * A physical prop moved by prop_place is made again at rest at its new place (at the next frame). prop_push: an
     * impulse (kg m/s) at a point of the world (NULL: its centre). prop_body: its physical object (an actor), NULL
     * none. Game thread. */
    int (*prop_anim_play)(MmlMod *mod, void *prop, const char *anim, uint32_t flags, float weight);
    int (*prop_anim_stop)(MmlMod *mod, void *prop);
    int (*prop_anim_state)(MmlMod *mod, void *prop, MmlPropAnim *out);
    int (*prop_physics)(MmlMod *mod, void *prop, const MmlPropPhysics *physics);
    int (*prop_push)(MmlMod *mod, void *prop, const float impulse[3], const float at[3]);
    void *(*prop_body)(MmlMod *mod, void *prop);

    /* ---- the game's files and models (both builds; docs/MODDING.md "The game's files and models"). game_files: every
     * file the game can open whose name matches pattern ("models\\*.4ds", "*.5ds"; * and ? within a name, without
     * regard to case; NULL every one): the archives' (their directories read, each name once, from the archive the
     * game looks in first), the game's folder's own (which the game opens before the archives) and the mods'; fn told
     * of each in order of the names; returns how many matched. model_file: the file a model frame came from (the
     * nearest model above any frame: a bone, a car's part), as LS3D keeps it: the name its model was opened by
     * ("Models\\Tommy.i3d"); the name's length, 0 none (a frame of the mission's scene, no model). model_open: a .4ds
     * read through the game's file layer (a mod's own too) and parsed: "Tommy", "Tommy.i3d", "models\\Tommy.4ds";
     * its handle, 0 none (the log says why). model_nodes, model_materials: how many it has, filling out up to max;
     * model_mesh: a node's mesh (lod 0 the finest; an instance's: the node it shows), 1 filled (the arrays the host's
     * until model_close), 0 none. A mod's models are closed at its unload. Game thread. */
    int (*game_files)(MmlMod *mod, const char *pattern, MmlFileFn fn, void *user);
    int (*model_file)(MmlMod *mod, void *frame, char *out, int cap);
    int (*model_open)(MmlMod *mod, const char *file);
    int (*model_nodes)(MmlMod *mod, int model, MmlModelNode *out, int max);
    int (*model_mesh)(MmlMod *mod, int model, int node, int lod, MmlModelMesh *out);
    int (*model_materials)(MmlMod *mod, int model, MmlModelMaterial *out, int max);
    void (*model_close)(MmlMod *mod, int model);

    /* ---- the meshes the game draws (both builds; docs/MODDING.md "The meshes the game draws"). scene_focus: the box
     * (world) whose scenery the mod wants (NULL, NULL: none): the host walks the mission's scene a little each frame
     * and every 0.25 s takes the visuals of objects around the boxes of every mod (shown, not a part of an actor),
     * their meshes read once from LS3D's own buffers within a few milliseconds a scan (the rest at the next scans).
     * scene_pieces: the pieces of the last scan touching the mod's box; how many there are, filling out up to max.
     * actor_meshes: an actor's visuals now (a car's body, doors, wheels, each in its frame's pose; -1: no model;
     * people's skinned meshes are not in it). The arrays are the host's until the next frame. Game thread. */
    int (*scene_focus)(MmlMod *mod, const float lo[3], const float hi[3]);
    int (*scene_pieces)(MmlMod *mod, MmlMeshPiece *out, int max);
    int (*actor_meshes)(MmlMod *mod, void *actor, MmlMeshPiece *out, int max);

    /* ---- shaders on the game's own drawing (both builds, under a Direct3D 9 wrapper such as dxwrapper; docs/MODDING.md
     * "Shaders on the game's own drawing"; sdk/include/mml/shaders/game.hlsli the constants). shader_add: the draws
     * the rule picks are drawn with its vertex and/or pixel shader on the game's device, the frame's constants set
     * in game.hlsli's registers, the game's fixed pipeline (its textures, blending, depth) around them; the game's
     * own state is put back after each draw. The last rule added that matches a draw takes it. Returns its handle, 0:
     * refused (error, cap bytes, says why: a compiler's message too). shader_remove: one (0: every one of the mod's);
     * a mod's go at its unload. sdk/include/mml/shaders/reference.hlsl draws every kind as the game does: the
     * starting point to copy. Game thread. */
    int (*shader_add)(MmlMod *mod, const MmlShaderRule *rule, char *error, int cap);
    int (*shader_remove)(MmlMod *mod, int handle);
    /* shader_shadows: the host draws a shadow map from the scene's sun before each frame of the mission's world (the
     * scene's meshes around the camera and the cars near it: scene_pieces', actor_meshes' geometry), for the rules'
     * shaders (sampler 2, game.hlsli mml_ShadowViewProj, mml_Shadow); NULL: the mod no longer wants it (also at its
     * unload). Several mods: the largest of each setting. The game's lights near each draw (up to eight point and
     * spot lights, nearest first) and the sun are in game.hlsli's registers always. Game thread. */
    int (*shader_shadows)(MmlMod *mod, const MmlShadowSettings *settings);

    /* ---- the game's own camera logic (both builds; docs/MODDING.md "The game's camera").
     * game_camera: its state now (MmlGameCamera; size set by the caller); 0 with no mission.
     * game_camera_mode: the game's camera switched to `mode` by its own G_Camera::SetMode, as the camera-change key
     * does (the game then drives it as its own: the eyes, a wheel, the bonnet): only a mode the player's state allows
     * (on foot MML_GAMECAM_BEHIND, _EYES; in a vehicle 7, 8, 9, 12, 13, 14); 0 refused.
     * game_camera_fov: the field of view (radians, across, 0.1..3.0) the game draws with, through the camera's own
     * setters, held: set again whenever the game changes it (a mode's push and pop, a dialogue, the scope); 0 gives
     * the game's back (the value it had before the first change). One holder, the last mod that asked; its unload
     * gives it back. Game thread. */
    int (*game_camera)(MmlMod *mod, MmlGameCamera *out);
    int (*game_camera_mode)(MmlMod *mod, int mode);
    int (*game_camera_fov)(MmlMod *mod, float fov);

    /* ---- another kind of vehicle (both builds; docs/MODDING.md "Control").
     * vehicle_kinds: which vehicles this mod's vehicle_hook is told of (MML_VEHICLES_*; cars only until it asks):
     * MML_VEHICLES_AIRPLANES adds the game's airplanes (their steps' kind MML_VEHICLE_AIRPLANE, dt their AI's).
     * vehicle_body reads and writes an airplane's body as a car's. 0: not known for this build. Game thread. */
    int (*vehicle_kinds)(MmlMod *mod, unsigned kinds);

    /* ---- orders for people (both builds; docs/MODDING.md "Orders for people"). A person with a script (a C_entity:
     * actor_spawn's people, the missions' NPCs; not the player, not the traffic's pedestrians) has a program of his
     * own the game's AI runs; human_order puts `code` into it (his old program replaced), in the missions' language
     * and its people's own commands, ENEMY_* (enemy_move_to_frame 0, 1, run; enemy_action_follow 0, 2;
     * enemy_action_fire 0; enemy_action_runaway 0, 20; enemy_move_to_car 0, 0 then enemy_usecar 0, 0; enemy_stop:
     * docs/SCRIPTS.md "Orders"), and hands him to it (C_entity::ForceScript): his AI then runs it, a command that
     * queues an action waits for it. The code needs no `end`: the loader ends it with a wait of its own (label 60003)
     * where he stays ticked, and an `end` of the code's is a jump there (a program that reached END would take him
     * out of the game's ticks for good). Their numbers are registers: act[i] from acts, flt[i] from flts, frm[i] a frame of the loader's
     * at points[3*i..3*i+2] (the world). Returns the order's id (its end is the bridge's event ORDER_ENDED: `wait
     * {"types": "ORDER_ENDED"}`), 0 refused (the log says why: not such a person, a line that does not compile, a
     * mission's NPC whose own script has ended: the game ticks him no more). A new order
     * replaces his last; the frames go with it, his death or the mission's close. human_order_state fills out (size
     * set): 1 when an order of the loader's is known for him. Game thread. */
    int (*human_order)(MmlMod *mod, void *human, const char *code, void *const *acts, int act_count, const float *flts, int flt_count,
                       const float *points, int point_count);
    int (*human_order_state)(MmlMod *mod, void *human, MmlOrderState *out);
    /* ---- the road network (both builds; docs/MODDING.md "Orders for people"): the node of the game's road network
     * nearest to `at` (C_roads::GetNearNode: what the game's drives, ENEMY_CAR_MOVETO, plan their way between), its
     * place into out. Its index + 1; 0 none (no mission, a mission with no road network). Game thread. */
    int (*road_near)(MmlMod *mod, const float at[3], float out[3]);

    /* ---- a mod's 64-bit half (both builds; mml/side.h, docs/MODDING.md "64-bit halves"): what the two halves share.
     * channel_open: "<name>" of this mod, the same block of shared memory its 64-bit half opens with
     * side.h channel_open (the same size: else NULL); one half writes, the other reads. channel_write: a whole new
     * version (at most size bytes); channel_read: the latest whole version copied (its length; 0 none yet), its
     * number in *version. Any thread; the channels close with the mod's unload.
     * gfx_shared_texture: the 64-bit half's texture of that name (side.h texture_create) on the graphics bridge's
     * device, opened at the first ask and again whenever the half makes it anew (its generation): out's texture and
     * srv (ID3D11Texture2D*, ID3D11ShaderResourceView*: the loader's, not to be released), size and format. 1 there,
     * 0 not (yet). Inside a graphics pass (the bridge's device). The half writes it when the 32-bit half does not read
     * it: a channel tells which version is whole (two textures, written in turn). */
    MmlChannel *(*channel_open)(MmlMod *mod, const char *name, uint32_t size);
    int (*channel_write)(MmlChannel *ch, const void *data, uint32_t bytes);
    uint32_t (*channel_read)(MmlChannel *ch, void *data, uint32_t cap, uint64_t *version);
    int (*gfx_shared_texture)(MmlMod *mod, const char *name, MmlSharedTexture *out);
    /* channel_wait: until the channel holds a version after `after` (the number channel_read gave), at most timeout_ms
     * (0: only looks); that version's number, 0 when none came in time. The writer's channel_write wakes it at once
     * (an event a channel: one waiter a channel, its reader). Any thread; on the game thread a wait is the frame's
     * time: a pass waiting for its 64-bit half's result of this frame waits at most a few milliseconds. */
    uint64_t (*channel_wait)(MmlChannel *ch, uint64_t after, uint32_t timeout_ms);

    /* ---- the developer's switches (both builds; docs/MODDING.md "The developer's switches").
     * human_invulnerable: a person the game's harm leaves alone (on 1) or not (0); -1 only asks. Every harm reaches a
     * person through his Hit (C_human::Hit: bullets, blows, explosions, a car that strikes him, a fall): it is not run
     * for him, as the game itself skips it for the player while a scene holds him. Held by each mod (and the bridge)
     * on its own: he stays so while one of them holds him, until his removal, the mission's close or the holder's
     * unload. Returns his state now (1, 0), -1 not a person. Game thread.
     * hud_game: the game's HUD (the health, the ammo, the radar, the compass, its texts) drawn (show 1) or not (0)
     * from the next frame; -1 only asks. Hidden while any mod (or the bridge) hides it; the menus, the console and the
     * mods' drawing stay. Returns 1 shown, 0 hidden, -1 not known for this build. */
    int (*human_invulnerable)(MmlMod *mod, void *human, int on);
    int (*hud_game)(MmlMod *mod, int show);
    /* ---- the pedestrians' web and the surfaces (both builds; docs/MODDING.md "Questions to the world").
     * web_nodes: the nodes of the mission's web (missions\<name>\check.bin: the places people walk between, what
     * their routes are built on) within `radius` of `at`, nearest first, up to max into out; returns how many there
     * are within the radius (more than max: the first max written), 0 none (no mission, a mission with no web).
     * material_name: a surface's name in the game's table (tables\materials.def: a static collision face's material,
     * MmlHit's and MmlShape's flags >> 24), UTF-8; NULL not in the table (or no table yet). Game thread. */
    int (*web_nodes)(MmlMod *mod, const float at[3], float radius, MmlWebNode *out, int max);
    const char *(*material_name)(int material);
    /* ---- the graphics bridge's state (both builds; docs/MODDING.md "Effects over the frame").
     * gfx_state: 1 on (the passes run at each frame of the mission's world), 0 off and why (the log's reason, UTF-8,
     * cut to cap: a game device with multisampling, a Direct3D 9 that shares nothing...), -1 not yet (no pass
     * registered, or no frame of the mission's world drawn since). A mod tells its player why its effect is missing.
     * Game thread. */
    int (*gfx_state)(MmlMod *mod, char *why, int cap);
    /* ---- motion vectors (both builds, under a Direct3D 9 wrapper; docs/MODDING.md "Motion vectors").
     * gfx_motion: the mod wants each frame's motion vectors (on 1) or no more (0): MmlGfxFrame's motion from the next
     * frame of the mission's world while any mod wants them (their cost, the moving objects drawn again, is paid
     * only then). Returns 1; 0 when the game's drawing cannot be followed (no wrapper's Direct3D 9). Game thread. */
    int (*gfx_motion)(MmlMod *mod, int on);
    /* ---- people's skinning (both builds; docs/MODDING.md "People's skinning").
     * skin_set: a model's single mesh ("Tommy" or "models\\Tommy.4ds", the mesh's name in it: "base") skinned with
     * these LODs from its next skinning on, every person of that model alike: up to 8 bones a vertex, any joint under
     * the mesh, each bone with its own matrix at rest; LOD `i` of lods is the mesh's LOD i (count of them; a LOD the
     * mod does not give keeps LS3D's own skin). The same cache LS3D skins into: its lighting, its drawing and whatever
     * reads the cache take the result as before. count 0: the mod's skin of that mesh taken away (also at its unload).
     * A mod's file models\<name>.skin.json among its files gives the same without code (the format in MODDING.md).
     * Returns 1; 0 refused (error, cap bytes, says why). skin_get: LS3D's own skin of a model's mesh (mesh NULL: its
     * first single mesh) and LOD from its .4ds, in skin_set's form with the rest mesh; 1 filled, 0 none. Game thread. */
    int (*skin_set)(MmlMod *mod, const char *model, const char *mesh, const MmlSkinLod *lods, int count, char *error, int cap);
    int (*skin_get)(MmlMod *mod, const char *model, const char *mesh, int lod, MmlSkinMesh *out);
} MmlHost;

#define MML_ZONE_PLAYER 0x1u
#define MML_ZONE_PEOPLE 0x2u
#define MML_ZONE_CARS   0x4u
#define MML_ZONE_ACTOR  0x8u

/* predmety.def's flags (MmlItemDef.flags; tables\predmety.def's +0x20 word, read in the game's code) */
#define MML_ITEM_MELEE      0x4u        /* a melee weapon (struck with) */
#define MML_ITEM_RANGED     0x20u       /* a firearm or a throwable */
#define MML_ITEM_THROWN     0x80u       /* a throwable */
#define MML_ITEM_KEPT       0x100u      /* kept, not held (keys, papers: the inventory's row is not usable) */
#define MML_ITEM_IN_HAND    0x2000u     /* taken straight into the hand */
#define MML_ITEM_BIG        0x200000u   /* a big item: the coat's one slot */
#define MML_ITEM_NOT_SOLD   0x800000u   /* not in the gun shop */

#define MML_ANIM_SIMPLE 0x1

#define MML_SAVE_SLOT 0
#define MML_SAVE_PROFILE 1

#define MML_FILE_GAME 1              /* file_read: the game's own file, not a mod's of the same name */

/* actor_spawn's kinds: the game's actor types (C_actor +0x10) */
#define MML_ACTOR_CAR 4
#define MML_ACTOR_HUMAN 27

typedef int (*MmlModLoadFn)(const MmlHost *host, MmlMod *mod);
typedef void (*MmlModUnloadFn)(MmlMod *mod);
typedef uint32_t (*MmlModApiFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* MML_H */
