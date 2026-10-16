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
    void *other;            /* HUMAN_DIED: the killer; VEHICLE_*: the vehicle */
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
} MmlGfxFrame;
#define MML_GFX_HAS(f, member) ((size_t)(f)->size >= offsetof(MmlGfxFrame, member) + sizeof((f)->member))
#define MML_GFX_UI_OFF 0     /* gfx_ui: the 2D pass as the game draws it */
#define MML_GFX_UI_TAKE 1    /* ... also into MmlGfxFrame::ui (the frame unchanged) */
#define MML_GFX_UI_HIDE 2    /* ... into MmlGfxFrame::ui only: the HUD and the menus not on the frame (a VR panel) */
/* A pass may change any Direct3D 11 state: the bridge binds what it hands out again before each pass, the blend
 * state too (premultiplied over: each pass is laid over the passes before it). */
typedef void (*MmlGfxPassFn)(const MmlGfxFrame *frame, void *user);

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
     * drawn once, in the frame being made, and dropped. A text is in the game's own encoding (ASCII is safe) and
     * at most about 100 characters a call. A fault while a mod's items are drawn turns that mod's drawing off
     * until it is reloaded (the log tells); the game goes on. */
    void (*draw_text)(MmlMod *mod, float x, float y, float size, uint32_t argb, int font, int align, const char *text);
    float (*draw_text_width)(int font, float size, const char *text);   /* in the same units; 0 with no game */
    void (*draw_line)(MmlMod *mod, float x0, float y0, float x1, float y1, uint32_t argb);
    void (*draw_rect)(MmlMod *mod, float x, float y, float w, float h, uint32_t argb, int filled);

    /* ---- keys and actions, as the game got them at its last read (the keys and actions the loader injects for
     * agents, `input`, are seen the same way as real ones). A key is a DirectInput code (key_code("F5"), "Ctrl",
     * "Space", "A"...; -1: no such name). key_pressed is true for the one frame the key went down: ask in one
     * handler (FRAME or MISSION_TICK), each frame. An action is one of the game's binding slots (action_slot(
     * "Accelerate"), "Fire", "Action"...; -1: no such name; `input.actions` lists them): its value is 0..1 for a
     * button and the axis' value for the mouse; action_pressed is nonzero in the frame it was pressed. */
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
} MmlHost;

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
