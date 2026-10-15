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

typedef struct MmlEvent {
    uint32_t size;          /* sizeof(MmlEvent) as the loader built it */
    MmlEventType type;
    float dt;               /* FRAME, MISSION_TICK: seconds */
    void *actor;            /* ACTOR_DESTROYED, HUMAN_CREATED, HUMAN_HIT */
    const MmlHumanHit *hit; /* HUMAN_HIT */
    const char *name;       /* SERVICES_CHANGED, MOD_LOADED, MISSION_OPEN, MISSION_LOADED, CONFIG_CHANGED */
} MmlEvent;

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
