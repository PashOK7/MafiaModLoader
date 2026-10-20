//! mml-mcp: the MCP server of MafiaModLoader (stdio).
//!
//! It runs outside the game and talks to the loader inside it over the bridge (127.0.0.1, the port and a per-run token
//! in `<game>\mml\bridge.json`; see docs/PROTOCOL.md). It survives the game's restarts and crashes: it launches and
//! closes the game, reads the loader's log from disk when the game is gone, and opens the crash dumps the loader writes.
//!
//! The game's folder: `MML_GAME_DIR`, else `game_dir` in `mml.config.json` at the repository's root.

mod bench;
mod bridge;
mod crash;
mod game;
mod kb;
mod script;
mod shot;
mod stacks;
mod structs;
mod syms;
mod ghidra;

use base64::Engine;
use rmcp::{
    ServerHandler, ServiceExt,
    handler::server::{router::tool::ToolRouter, wrapper::Parameters},
    model::{CallToolResult, ContentBlock, ServerCapabilities, ServerConfig},
    tool, tool_handler, tool_router,
};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::sync::Arc;

type Out = Result<String, String>;

fn pretty(v: &Value) -> String {
    serde_json::to_string_pretty(v).unwrap_or_else(|_| v.to_string())
}

#[derive(Deserialize, JsonSchema)]
struct CallArgs {
    /// The bridge method: a host method (`mods.list`, `mod.reload`, `mem.read`, `where`...) or a mod's command
    /// (`player`, `humans`, `hello_c.status`...). `commands_list` lists them.
    method: String,
    /// Its parameters (a JSON object).
    #[serde(default)]
    params: Option<Value>,
    /// How long to wait for the game thread (ms, default 5000).
    #[serde(default)]
    timeout_ms: Option<u64>,
}

#[derive(Deserialize, JsonSchema)]
struct MemReadArgs {
    /// "0x5762A0", "game+0x1762A0", "ls3d+1AAE0", "<module>+<hex>".
    addr: String,
    /// Bytes to read (default 64, at most 1 MiB).
    #[serde(default)]
    size: Option<u32>,
    /// Decode as: u8 i8 u16 i16 u32 i32 f32 f64 ptr str.
    #[serde(default, rename = "as")]
    as_: Option<String>,
}

#[derive(Deserialize, JsonSchema)]
struct MemWriteArgs {
    /// Where (as for mem_read).
    addr: String,
    /// The bytes as hex ("90 90 C3"), or use `as` + `values`.
    #[serde(default)]
    hex: Option<String>,
    /// The values' type: u8 i8 u16 i16 u32 i32 ptr f32 f64.
    #[serde(default, rename = "as")]
    as_: Option<String>,
    /// The values to write, in order.
    #[serde(default)]
    values: Option<Vec<Value>>,
}

#[derive(Deserialize, JsonSchema)]
struct LogArgs {
    /// How many of the last lines (default 100).
    #[serde(default)]
    lines: Option<u32>,
    /// The previous run's log (mml.prev.log) instead of the current one.
    #[serde(default)]
    previous: Option<bool>,
}

#[derive(Deserialize, JsonSchema)]
struct IdArgs {
    /// The mod's id (its folder under mml\mods); "*" reloads every loaded mod.
    id: String,
}

#[derive(Deserialize, JsonSchema)]
struct LaunchArgs {
    /// Seconds to wait for the loader's bridge to answer (default 60), and for the start to finish.
    #[serde(default)]
    wait_s: Option<u64>,
    /// Where to take the game by its own menus: "menu" (the main menu), "freeride" (with `kind`, `place`), "load" (a
    /// save, with `save`: a chapter of the story), "new" (a new game: the intro, then the first mission), "extreme"
    /// (the freeride extreme). Omitted (and no profile): the game's usual start, the player at the keyboard.
    #[serde(default)]
    go: Option<String>,
    /// The freeride's kind: "city-small" (the default, missions\freeitaly), "city-night" (freeridenoc),
    /// "countryside-night" (freekrajinanoc), "city" (freeride: the whole city by day), "countryside" (freekrajina).
    /// The game has no time of day: night is these kinds.
    #[serde(default)]
    kind: Option<String>,
    /// Where a freeride other than city-small starts: the spawn menu's row (1..; default 1) or a part of its text
    /// ("Down Town", "Hoboken").
    #[serde(default)]
    place: Option<Value>,
    /// The save "load" takes: its number (savegame\mafiaNNN.<number>, of the profile) or a part of its name as the
    /// load menu shows it ("Old Prison", "20-05"). Unknown or several: the error lists them.
    #[serde(default)]
    save: Option<Value>,
    /// The profile to take in the profile menu: its number (savegame\mafiaNNN.sav) or its name.
    #[serde(default)]
    profile: Option<Value>,
    /// Skip the three logo videos before the profile menu (default true when go or profile is given).
    #[serde(default)]
    skip_logos: Option<bool>,
    /// The freeride's police patrols, 0..1 (the freeride menu's slider; default: the menu's own, 1). 0: no police,
    /// so nobody minds the weapon in the player's hands.
    #[serde(default)]
    police: Option<f64>,
    /// The freeride's traffic density, 0..1 (default: the menu's own, 1).
    #[serde(default)]
    traffic: Option<f64>,
    /// The freeride's pedestrian density, 0..1 (default: the menu's own, 1).
    #[serde(default)]
    pedestrians: Option<f64>,
    /// Keep the game's window in the background (default true): neither the launch nor the game itself activates it,
    /// so whoever works in the window in front is not disturbed (a click on the game's window activates it as
    /// usual). The game runs and draws there; `screenshot` reads its frame where the renderer allows it (Direct3D 9:
    /// the labs run through dxwrapper).
    #[serde(default)]
    background: Option<bool>,
}

#[derive(Deserialize, JsonSchema)]
struct CloseArgs {
    /// Kill at once instead of asking the window to close first.
    #[serde(default)]
    force: Option<bool>,
}

#[derive(Deserialize, JsonSchema)]
struct CrashArgs {
    /// The dump's file name in mml\crash (default: the newest).
    #[serde(default)]
    file: Option<String>,
}

#[derive(Deserialize, JsonSchema)]
struct DeployArgs {
    /// Only these mods (ids); default every mod the build defines. The loader itself (MafiaModLoader.asi) is copied
    /// only while the game is not running (Windows keeps it locked).
    #[serde(default)]
    mods: Option<Vec<String>>,
    /// Reload the deployed mods in the running game afterwards (default true).
    #[serde(default)]
    reload: Option<bool>,
}

#[derive(Deserialize, JsonSchema)]
struct ScriptArgs {
    /// The program, one command a line ("// comments"; commands case-insensitive; arguments separated by commas;
    /// strings in quotes, except the kind names of findnearactor). Registers: flt[0..255] (floats), act[0..63]
    /// (actors), frm[0..63] (frames), given before it starts (no dim_* needed). E.g. teleport the car:
    /// "getactorframe 0, 0\nfrm_setpos 0, 0\nactor_setpos 0, 0\ncar_repair 0" with act {"0": "car"} and flt
    /// {"0": x, "1": y, "2": z}. `script_commands` tells each command's arguments; docs/SCRIPTS.md has recipes.
    code: String,
    /// act[] given before it starts: {"0": "player", "1": "car", "2": "0x0F923CF0"} (an actor's address, as `actors`
    /// or `humans` list them).
    #[serde(default)]
    act: Option<Value>,
    /// flt[] given before it starts: {"0": -1940.5, "1": -4.5}.
    #[serde(default)]
    flt: Option<Value>,
    /// The actor the program runs as, as a mission's script runs as its entity ("player", "car", an address): where
    /// `explosion` goes off, what `playsound`, `frm_seton`, `act_setstate` use with no register, what
    /// `findnearactor` skips. Default none.
    #[serde(default)]
    owner: Option<Value>,
    /// Stopped after this long (ms, default 10000).
    #[serde(default)]
    timeout_ms: Option<u64>,
    /// Return at once with the program's id (default false: wait until it ends); `call script.state {id}` then.
    #[serde(default)]
    no_wait: Option<bool>,
}

#[derive(Deserialize, Serialize, JsonSchema)]
struct SpawnArgs {
    /// A car: its name in the game's car list ("Bolt-Thrower", "bolt ace tudor": a part of the name will do when only
    /// one car has it) or its number; `call world.cars {query}` lists them.
    #[serde(default)]
    car: Option<Value>,
    /// The car's colour: which of its model files (<prefix>00.i3d, 01...; default 0).
    #[serde(default)]
    color: Option<u32>,
    /// A person: his model ("Paulie", "Sam", "Tommy": the game's models\<name>.i3d; `call humans` shows models in
    /// use). He stands, armed with nothing, until told (give, script).
    #[serde(default)]
    human: Option<String>,
    /// Where: [x, y, z] (the world's metres, +y up), or an actor ("player" the default, "car", "0x...") with
    /// `ahead` metres in front of it (the car for a player who sits in one).
    #[serde(default)]
    at: Option<Value>,
    /// Metres in front of the `at` actor (default 8 for a car, 2.5 for a person).
    #[serde(default)]
    ahead: Option<f64>,
    /// The direction it faces, [x, y, z] (default: a car as the `at` actor, a person facing it; +z at coordinates).
    #[serde(default)]
    dir: Option<Value>,
    /// Put on the ground under the place (default true).
    #[serde(default)]
    ground: Option<bool>,
    /// A person put at once into this car's seat ("car": the player's, or an address).
    #[serde(rename = "in", default)]
    in_car: Option<String>,
    /// That seat: 0 the driver's, 1 beside him (default), 2 and 3 behind.
    #[serde(default)]
    seat: Option<u32>,
}

#[derive(Deserialize, Serialize, JsonSchema)]
struct TeleportArgs {
    /// Who: "player" (default; a player in a car moves the car), "car", an actor's address.
    #[serde(default)]
    actor: Option<String>,
    /// Where: [x, y, z], or an actor ("car", "0x...") with `ahead` metres in front of it.
    to: Value,
    /// Metres in front of the `to` actor (default 3).
    #[serde(default)]
    ahead: Option<f64>,
    /// Turned to face [x, y, z] (default: as it was).
    #[serde(default)]
    dir: Option<Value>,
    /// A car: stopped, set on its wheels and repaired (the scripts' car_repair; default true).
    #[serde(default)]
    repair: Option<bool>,
}

#[derive(Deserialize, Serialize, JsonSchema)]
struct GiveArgs {
    /// Who: "player" (default) or a character's address.
    #[serde(default)]
    actor: Option<String>,
    /// The item: its name ("Thompson 1928", "colt 1911", "grenade", "bat") or its number; `call world.items` lists
    /// them.
    item: Value,
    /// Rounds loaded (default: a full magazine).
    #[serde(default)]
    loaded: Option<i32>,
    /// Rounds in reserve (default: the item's usual).
    #[serde(default)]
    reserve: Option<i32>,
}

/// The JSON object of a tool's arguments, as the bridge method takes them (absent fields left out).
fn params_of<T: Serialize>(a: &T) -> Value {
    let mut v = serde_json::to_value(a).unwrap_or(Value::Null);
    if let Some(o) = v.as_object_mut() { o.retain(|_, x| !x.is_null()); }
    v
}

#[derive(Deserialize, JsonSchema)]
struct ScriptCommandsArgs {
    /// A part of the commands' names ("human_", "car_", "actor", "frm_set", "weather"); empty: every name.
    #[serde(default)]
    query: Option<String>,
    /// Add each command's note (its arguments and what it does) from st0rm94's 1.0 reverse when it is cloned in re/ext
    /// (default true; at most 40 commands).
    #[serde(default)]
    notes: Option<bool>,
}

#[derive(Deserialize, JsonSchema)]
struct KbSearchArgs {
    /// Text to find (case-insensitive) in a function's name, class, signature, topics or the strings it uses:
    /// "C_mission", "scene.i3d", "savegame", "Hit", or an address fragment.
    query: String,
    /// "game.exe" or "ls3df.dll" (default both).
    #[serde(default)]
    module: Option<String>,
    /// At most this many (default 40).
    #[serde(default)]
    limit: Option<usize>,
}

#[derive(Deserialize, JsonSchema)]
struct RestoreArgs {
    /// Keep the game as it is now under this name (letters, digits, `_`, `-`, `.`): the game's own save, whole.
    #[serde(default)]
    save: Option<String>,
    /// Put the game back to that point (it returns once the game has loaded it, with the player's state).
    #[serde(default)]
    load: Option<String>,
    /// Remove a point.
    #[serde(default)]
    delete: Option<String>,
    /// Seconds to wait for a load (default 60: another mission's point opens that mission).
    #[serde(default)]
    timeout_s: Option<f64>,
}

#[derive(Deserialize, JsonSchema)]
struct InspectArgs {
    /// The object: an address ("0x0A1B2C30", "Game.exe+0x2F9464"), or "player", "car" (driven now, else last),
    /// "mission", "game" (C_game), each followed by fields if wanted: "player.inventory", "player.vehicle",
    /// "game.camera", "car.vehicle", "player.inventory.belt[1]" (an embedded field is its place, a pointer what it
    /// points at). Absent: the layout of `as` alone, both builds' offsets, no game needed.
    #[serde(default)]
    at: Option<String>,
    /// The structure ("C_human", "C_car", "C_Vehicle", "I3D_frame", "G_Inventory"...): absent, it is told by the
    /// object's vtable (or by the field it was reached through).
    #[serde(default, rename = "as")]
    as_: Option<String>,
    /// Keep the fields whose name holds one of these ("health", "pos").
    #[serde(default)]
    fields: Option<Vec<String>>,
    /// Follow the pointers to known structures this many levels (default 0, at most 2).
    #[serde(default)]
    depth: Option<u32>,
    /// Also the non-zero dwords no field covers, as integers and floats: what is not named yet.
    #[serde(default)]
    gaps: Option<bool>,
}

#[derive(Deserialize, Serialize, JsonSchema)]
struct GhidraReadArgs {
    /// "decompile" (the function holding `at`, made a function if none is there), "asm" (from `at`, the names of
    /// what each instruction refers to), "function" (entry, size, prototype, where its name came from, references,
    /// callees), "callers" (call sites, and pointers to it: vtables), "callees" (every call in it, direct or not),
    /// "xrefs" (every reference to `at`: reads, writes, calls, data), "vtable" (a table of pointers, each named),
    /// "bytes", "twin" (the other build's function paired with it by its code), "status" (the project).
    #[serde(skip_serializing)]
    op: String,
    /// "1.2" (default) or "1.0": each build has its own project and server; both may be asked at once.
    #[serde(default, skip_serializing)]
    build: Option<String>,
    /// An address: 0x5762A0 or 5762A0 (Game.exe; from 0x10000000 LS3DF.dll), "ls3d+1AAE0" (an LS3DF RVA, as the
    /// loader writes them), "game+0x1762A0", or a name ("C_human::Hit", "humanHit").
    #[serde(default, skip_serializing_if = "Option::is_none")]
    at: Option<String>,
    /// decompile: only the lines matching this regex (with `context` lines around them).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    grep: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    context: Option<u32>,
    /// decompile: a line range "40-90".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    range: Option<String>,
    /// At most this many lines or items (decompile 400, the lists 80).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    max: Option<u32>,
    /// asm: instructions (40); vtable: entries (32); bytes: bytes (32).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    count: Option<u32>,
}

#[derive(Deserialize, Serialize, JsonSchema)]
struct GhidraFindArgs {
    /// "imm" (instructions using `value` as an immediate or a displacement such as [ECX + 0x670]: who touches a
    /// structure's field), "string" (defined strings matching `regex`, with the functions using them), "find"
    /// (where the image holds `what`: a 32-bit value, a pointer no code names, or a byte pattern "8B 4C 24"),
    /// "names" (the names given in this project; `all`: the imported ones too), "functions" (by a name regex).
    #[serde(skip_serializing)]
    op: String,
    /// "1.2" (default) or "1.0".
    #[serde(default, skip_serializing)]
    build: Option<String>,
    /// imm: the value, hex ("670").
    #[serde(default, skip_serializing_if = "Option::is_none")]
    value: Option<String>,
    /// string, functions: a regex (case-insensitive).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    regex: Option<String>,
    /// find: a hex value or a byte pattern with spaces.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    what: Option<String>,
    /// "game" or "ls3d" (default both).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    module: Option<String>,
    /// imm: only in the function holding this address.
    #[serde(default, rename = "in", skip_serializing_if = "Option::is_none")]
    in_: Option<String>,
    /// imm: only instructions writing to memory.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    writes: Option<bool>,
    /// imm: only instructions whose text matches this regex ("CALL": calls of a vtable slot).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    text: Option<String>,
    /// names: the imported names too.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    all: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    max: Option<u32>,
}

#[derive(Deserialize, Serialize, JsonSchema)]
struct GhidraWriteArgs {
    /// "rename" (a function at its entry, else a label; "C_x::Method" puts it into the class), "comment" (a
    /// function's plate comment at its entry, else a comment before the instruction), "proto" (a C prototype:
    /// "int __thiscall Hit(C_human *this, int type)"), "make_function", "unfunction" (a function wrongly made;
    /// its name stays as a label), "sync" (the knowledge base's names and annotations onto the project again).
    /// rename, comment and proto are saved into the project and recorded in the repository at once:
    /// knowledge/<module>/names.csv (1.2) or names_10.csv (1.0), annotations.csv.
    #[serde(skip_serializing)]
    op: String,
    /// "1.2" (default) or "1.0".
    #[serde(default, skip_serializing)]
    build: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    at: Option<String>,
    /// rename: the new name.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    name: Option<String>,
    /// rename: how it was found (the names.csv note).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    note: Option<String>,
    /// comment: the text.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    text: Option<String>,
    /// proto: the C prototype.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    proto: Option<String>,
}

#[derive(Deserialize, JsonSchema)]
struct KbFunctionArgs {
    /// An address of the 1.2 image ("5409D0", "0x100AC2E", "game+0x1409D0", "ls3d+5AC2E": any address inside the
    /// function) or a name ("C_mission::Open").
    key: String,
    /// Include the decompiled code (read from the reverse folder; default true).
    #[serde(default)]
    code: Option<bool>,
    /// At most this many lines of code (default 400).
    #[serde(default)]
    max_lines: Option<usize>,
}

#[derive(Deserialize, JsonSchema)]
struct TapArgs {
    /// "<at> [n=4] [ret|fret] [ptr=<src>[+off][:len],...] [from=<ret addr>] [log] [max=N]" starts watching a function
    /// (at: "5409D0", "ls3d+1AAE0", "game+0x1409D0"; ptr src: 1..8 an argument, c ECX, d EDX, r the result; a
    /// snapshot that starts with text shows the string); "show <id> [k]", "list", "clear <id>", "off <id>|all".
    cmd: String,
}

#[derive(Deserialize, JsonSchema)]
struct WatchArgs {
    /// Start a watch on this address ("0x0EB9F024", "game+0x2D8714"): the game thread's every write of it (or every
    /// access with `mode: "access"`) is caught by a debug register (four at most).
    set: Option<String>,
    /// "write" (default: only writes that change it) or "access" (reads too).
    mode: Option<String>,
    /// Bytes watched: 1, 2 or 4 (default 4; the address aligned to it).
    len: Option<u32>,
    /// Show what a watch caught: each piece of code (named), how often, its callers on the stack, the values.
    show: Option<u32>,
    /// Take a watch off (its id), or "all".
    clear: Option<Value>,
}

#[derive(Deserialize, JsonSchema)]
struct CommandsListArgs {
    /// Keep the methods whose name, params or help hold this text (any case); none: every one.
    query: Option<String>,
}

#[derive(Deserialize, JsonSchema)]
struct WaitArgs {
    /// The event types to wait for, comma-separated: MISSION_LOADED, MISSION_OPEN, MISSION_CLOSE, GAME_PAUSED,
    /// GAME_RESUMED, HUMAN_CREATED, HUMAN_HIT, HUMAN_DIED, SHOT, EXPLOSION, CAR_EXPLODED, VEHICLE_ENTERED,
    /// VEHICLE_LEFT, ACTOR_DESTROYED, MOD_LOADED, MOD_UNLOADED, MOD_FAULTED, SCRIPT_ENDED, CONFIG_CHANGED,
    /// FILE_CHANGED, SERVICES_CHANGED, ZONE_ENTERED, ZONE_LEFT, USED, TIMER (the bridge's zones, use-points and
    /// timers), GAME_SAVING, GAME_LOADED (a save written or loaded, a new game: `slot`, `profile`), PEDESTRIAN_HIT,
    /// BULLET_HIT (a bullet struck the world or a car: `pos`, `struck`, `surface`), CAR_CRASH (`speed` the change
    /// of the car's speed, `struck`), HUMAN_FELL (`speed`, `height`), ITEM_GAINED, ITEM_LOST (`item`, `name`,
    /// `count`, `rounds`), MONEY_CHANGED (`money`, `change`) (`call events.since` lists the recent ones).
    types: String,
    /// Fields the event's data must have, equal (strings without regard to case): {"name": "freeitaly"},
    /// {"killed": true}, {"actor": "0x12345678"}.
    filter: Option<Value>,
    /// Seconds to wait (default 30).
    timeout_s: Option<f64>,
    /// Count events after this sequence number (an earlier reply's `latest`); default: only those that come after
    /// this call (so an action, then `wait` in the next message, can miss a quick event: pass `since` from before).
    since: Option<u64>,
}

#[derive(Deserialize, JsonSchema, Clone)]
struct BenchCall {
    /// A bridge method or a mod's command (`skin.gpu`, `config.set`, `prof.counters`...).
    method: String,
    /// Its parameters.
    #[serde(default)]
    params: Option<Value>,
}

#[derive(Deserialize, JsonSchema)]
struct BenchArgs {
    /// The calls that make configuration A, made before each of its segments (none: the game as it is), e.g.
    /// [{"method": "skin.gpu", "params": {"on": false}}].
    #[serde(default)]
    a: Option<Vec<BenchCall>>,
    /// The calls that make configuration B. None: A against itself (the measure's own noise: no difference
    /// should be found).
    #[serde(default)]
    b: Option<Vec<BenchCall>>,
    /// Their names (default "A", "B"; "A", "A'" when B is A).
    #[serde(default)]
    names: Option<Vec<String>>,
    /// Blocks of four segments, ABBA then BAAB (default 4, 2..20): the interval narrows with more.
    #[serde(default)]
    blocks: Option<u32>,
    /// Frames measured a segment (default 300, 30..4000).
    #[serde(default)]
    frames: Option<u32>,
    /// Frames let pass after each switch before a segment's own (default 60: shaders made, caches filled).
    #[serde(default)]
    settle: Option<u32>,
    /// Count and time the device's calls by kind and caller, and the host's costly sites (default true). Their own
    /// cost, measured: some 35 ns a call of the device, 2 % of a 2.8 ms frame; both configurations bear it alike.
    #[serde(default)]
    counters: Option<bool>,
    /// The calls made at the end (default A's).
    #[serde(default)]
    after: Option<Vec<BenchCall>>,
}

#[derive(Deserialize, JsonSchema)]
struct ProfileArgs {
    /// How long the game thread is sampled (seconds, default 3, 0.5..30): let the game do the thing meanwhile.
    seconds: Option<f64>,
    /// Also the frame's measures since the last reset (prof.state: each mod's time by site, the frame times, the
    /// hitches); default true. `reset: true` starts them over first.
    state: Option<bool>,
    reset: Option<bool>,
    /// The time between samples (ms, default 1: a timer, no core kept busy; down to 0.25 by a wait on the clock).
    interval_ms: Option<f64>,
}

#[derive(Deserialize, JsonSchema)]
struct ShotArgs {
    /// How many frames (default 1, at most 8): several show motion.
    #[serde(default)]
    count: Option<u32>,
    /// The time between them (ms, default 250).
    #[serde(default)]
    every_ms: Option<u64>,
    /// The widest picture returned (default 1280): a wider frame is shrunk by a whole factor. 0: as it is.
    #[serde(default)]
    max_width: Option<u32>,
    /// JPEG quality, 1..100 (default 85).
    #[serde(default)]
    quality: Option<u8>,
}

#[derive(Deserialize, JsonSchema)]
struct InputStep {
    /// An action of the game, whatever it is bound to (even to nothing): its name ("Forwards", "Left", "Fire",
    /// "Action", "Jump", "Aim X", "Accelerate", "Steer Left"...: `call input.actions` lists them all with their
    /// bindings) or its slot number.
    #[serde(default)]
    action: Option<Value>,
    /// Or a key, for what is not an action (menus, cheats): "Esc", "Enter", "F1", "W", "Up"... or a DirectInput
    /// code. LS3D sees it pressed; the real keyboard is not touched.
    #[serde(default)]
    key: Option<Value>,
    /// How long it is held (ms, default 100: a tap).
    #[serde(default)]
    ms: Option<u64>,
    /// The action's value (default 1: pressed). The mouse axes "Aim X" / "Aim Y" (right / down positive) take
    /// small values every frame (0.01 is about 1.4 degrees a frame on foot): use `turn` for an angle.
    #[serde(default)]
    value: Option<f64>,
    /// Instead of an action or a key: turn the player on foot by this many degrees (to the right when positive),
    /// exactly, whatever the mouse sensitivity is. Takes `ms` (default 300), longer for a wide turn.
    #[serde(default)]
    turn: Option<f64>,
    /// When it starts, in ms from the start of the sequence. Default: when the step before it ends (the same
    /// `at_ms` on two steps holds them together).
    #[serde(default)]
    at_ms: Option<u64>,
    /// Instead of an action or a key: nothing for this long before the next step.
    #[serde(default)]
    wait_ms: Option<u64>,
    /// The menus' mouse: the cursor moved to [x, y] in the menus' 800x600 units (exactly; the control under it
    /// gets the focus, as under a real mouse). Only while a menu is open (it waits up to 2 s for one).
    #[serde(default)]
    mouse: Option<[f64; 2]>,
    /// The cursor moved to [x, y], then the left button pressed there for `ms` (default 100): a click.
    #[serde(default)]
    click: Option<[f64; 2]>,
    /// A mouse button held for `ms`: "left", "right" or "middle" (where the cursor is).
    #[serde(default)]
    button: Option<String>,
    /// The mouse wheel turned by this many notches (up / away positive): lists scroll.
    #[serde(default)]
    wheel: Option<f64>,
}

#[derive(Deserialize, JsonSchema)]
struct InputArgs {
    /// What to do, in order.
    steps: Vec<InputStep>,
    /// Return at once, not when the last step has ended (default false).
    #[serde(default)]
    no_wait: Option<bool>,
    /// A screenshot when the steps have ended (default false).
    #[serde(default)]
    screenshot: Option<bool>,
}

#[derive(Clone)]
struct Server {
    bridge: Arc<bridge::Bridge>,
    tool_router: ToolRouter<Self>,
}

#[tool_router]
impl Server {
    fn new() -> Self {
        Self { bridge: Arc::new(bridge::Bridge::new()), tool_router: Self::tool_router() }
    }

    async fn call_value(&self, method: &str, params: Value, timeout_ms: Option<u64>) -> Result<Value, String> {
        let mut params = if params.is_null() { json!({}) } else { params };
        if let (Some(t), Some(obj)) = (timeout_ms, params.as_object_mut()) {
            obj.insert("timeout_ms".into(), json!(t));
        }
        self.bridge.call(method, params, timeout_ms.unwrap_or(5000) + 2000).await
    }

    /// One segment of `bench`: the configuration's calls made, the frames labelled after `settle`, `frames` of them
    /// read back (`since`: the last frame read, carried from segment to segment).
    async fn bench_segment(&self, calls: &[BenchCall], label: &str, settle: u32, frames: usize, since: &mut u64) -> Result<Vec<bench::Frame>, String> {
        for c in calls {
            self.call_value(&c.method, c.params.clone().unwrap_or(Value::Null), None).await
                .map_err(|e| format!("bench: {} failed: {e}", c.method))?;
        }
        self.call_value("prof.mark", json!({"label": label, "settle": settle}), None).await?;
        let mut got = vec![];
        let mut last_new = std::time::Instant::now();
        while got.len() < frames {
            tokio::time::sleep(std::time::Duration::from_millis(150)).await;
            let r = self.call_value("prof.frames", json!({"since": *since, "limit": 4096}), None).await?;
            let next = r["next"].as_u64().unwrap_or(*since);
            if next > *since { last_new = std::time::Instant::now(); }
            *since = next;
            got.extend(bench::frames_of(&r, label));
            if last_new.elapsed().as_secs() > 15 { return Err(format!("bench: no frames for 15 s in {label} (the game paused, a menu, a load?)")); }
        }
        got.truncate(frames);
        Ok(got)
    }

    /// The next frame the game presents, as a JPEG in `<game>\mml\shots` (the loader writes it raw).
    async fn take_shot(&self, max_width: u32, quality: u8) -> Result<shot::Shot, String> {
        let asked = self.call_value("screenshot", Value::Null, Some(3000)).await?;
        let id = asked["id"].as_u64().ok_or("screenshot: no id")?;
        let start = std::time::Instant::now();
        loop {
            let s = self.call_value("screenshot.state", json!({"id": id}), Some(3000)).await?;
            if s["done"].as_bool() == Some(true) {
                if s["ok"].as_bool() != Some(true) {
                    return Err(format!("screenshot: {}", s["error"].as_str().unwrap_or("failed")));
                }
                let path = std::path::PathBuf::from(s["path"].as_str().ok_or("screenshot: no path")?);
                return tokio::task::spawn_blocking(move || shot::convert(&path, max_width, quality)).await.map_err(|e| e.to_string())?;
            }
            if start.elapsed().as_millis() > 3000 {
                return Err("screenshot: the game presented no frame in 3 s (minimised? loading? `where` tells)".into());
            }
            tokio::time::sleep(std::time::Duration::from_millis(20)).await;
        }
    }

    fn shot_content(s: &shot::Shot) -> (Value, ContentBlock) {
        let info = json!({"file": s.path, "width": s.width, "height": s.height, "frame": [s.frame.0, s.frame.1]});
        (info, ContentBlock::image(base64::engine::general_purpose::STANDARD.encode(&s.jpeg), "image/jpeg"))
    }

    /// See the game: its own frame (the back buffer as the game drew it, before it is shown), whether or not the
    /// window is visible or focused. Returns the picture(s) and the file(s) (`<game>\mml\shots\<n>.jpg`).
    #[tool(annotations(read_only_hint = true))]
    async fn screenshot(&self, Parameters(a): Parameters<ShotArgs>) -> Result<CallToolResult, String> {
        let count = a.count.unwrap_or(1).clamp(1, 8);
        let every = a.every_ms.unwrap_or(250).min(10_000);
        let mut infos = vec![];
        let mut content = vec![];
        for i in 0..count {
            if i > 0 { tokio::time::sleep(std::time::Duration::from_millis(every)).await; }
            let s = self.take_shot(a.max_width.unwrap_or(1280), a.quality.unwrap_or(85)).await?;
            let (info, image) = Self::shot_content(&s);
            infos.push(info);
            content.push(image);
        }
        content.insert(0, ContentBlock::text(pretty(&json!(infos))));
        Ok(CallToolResult::success(content))
    }

    /// Act in the game: a sequence of the game's own actions (and keys), timed by the game's frames, injected
    /// inside the game (the real keyboard and mouse are not touched, the window needs no focus). Returns when the
    /// last step has ended, with the player's state (and a screenshot when asked). E.g. turn right, walk 2 s,
    /// jump: [{"turn": 90}, {"action": "Forwards", "ms": 2000}, {"action": "Jump"}]. In a car "Action" does
    /// nothing while it moves, stops the engine when it runs, and only then gets out. Menus by the mouse: {"click":
    /// [x, y]} in 800x600 units (`call menu.state` gives the controls' places), {"wheel": -3} scrolls. Stop
    /// everything: `call input.release`.
    #[tool]
    async fn input(&self, Parameters(a): Parameters<InputArgs>) -> Result<CallToolResult, String> {
        let mut steps = vec![];
        let (mut cursor, mut total) = (0u64, 0u64);
        for (i, s) in a.steps.iter().enumerate() {
            let at = s.at_ms.unwrap_or(cursor);
            let mouse_kinds = [s.mouse.is_some(), s.click.is_some(), s.button.is_some(), s.wheel.is_some()].iter().filter(|b| **b).count();
            if mouse_kinds > 0 {
                if mouse_kinds > 1 || s.action.is_some() || s.key.is_some() || s.turn.is_some() {
                    return Err(format!("step {i}: one of action, key, turn, mouse, click, button, wheel"));
                }
                // a move and a wheel turn are made at the next read of the mouse; 40 ms is allowed for it here
                if let Some(p) = s.mouse.or(s.click) {
                    steps.push(json!({"mouse": p, "after_ms": at}));
                    cursor = at + 40;
                }
                if let Some(n) = s.wheel {
                    steps.push(json!({"wheel": n, "after_ms": at}));
                    cursor = at + 40;
                }
                let button = if s.click.is_some() { Some("left".to_string()) } else { s.button.clone() };
                if let Some(b) = button {
                    let start = if s.click.is_some() { at + 60 } else { at };
                    let ms = s.ms.unwrap_or(100).max(1);
                    steps.push(json!({"button": b, "ms": ms, "after_ms": start}));
                    cursor = start + ms;
                }
                total = total.max(cursor);
                continue;
            }
            if let Some(deg) = s.turn {
                if s.action.is_some() || s.key.is_some() { return Err(format!("step {i}: a turn, an action or a key: one of them")); }
                // the loader turns in frames (at most 5 degrees each); 20 ms is allowed for a frame here
                let frames = (s.ms.unwrap_or(300) / 20).max((deg.abs() / 5.0) as u64 + 1);
                steps.push(json!({"turn": deg, "frames": frames, "after_ms": at}));
                cursor = at + frames * 20;
                total = total.max(cursor);
                continue;
            }
            if s.action.is_none() && s.key.is_none() {
                cursor += s.wait_ms.ok_or(format!("step {i}: action, key, turn or wait_ms"))?;
                total = total.max(cursor);
                continue;
            }
            let ms = s.ms.unwrap_or(100).max(1);
            let mut step = json!({"ms": ms, "after_ms": at});
            match (&s.action, &s.key) {
                (Some(_), Some(_)) => return Err(format!("step {i}: an action or a key, not both")),
                (Some(x), _) => {
                    step["action"] = x.clone();
                    if let Some(v) = s.value { step["value"] = json!(v); }
                }
                (_, Some(k)) => step["key"] = k.clone(),
                _ => unreachable!(),
            }
            steps.push(step);
            cursor = at + ms;
            total = total.max(cursor);
        }
        let mut out = json!({"total_ms": total});
        if !steps.is_empty() {
            out["steps"] = self.call_value("input.seq", json!({"steps": steps}), None).await?;
        }
        if a.no_wait.unwrap_or(false) {
            return Ok(CallToolResult::success(vec![ContentBlock::text(pretty(&out))]));
        }
        // a frame or two more: the game's code reads what the last step let go
        tokio::time::sleep(std::time::Duration::from_millis(total + 100)).await;
        if let Ok(p) = self.call_value("player", Value::Null, Some(2000)).await { out["player"] = p; }
        let mut content = vec![];
        if a.screenshot.unwrap_or(false) {
            match self.take_shot(1280, 85).await {
                Ok(s) => {
                    let (info, image) = Self::shot_content(&s);
                    out["screenshot"] = info;
                    content.push(image);
                }
                Err(e) => out["screenshot"] = json!({"error": e}),
            }
        }
        content.insert(0, ContentBlock::text(pretty(&out)));
        Ok(CallToolResult::success(content))
    }

    /// Is the game running with the loader, and what state is it in: pid, build (1.2/1.0), frames, the mods.
    #[tool(annotations(read_only_hint = true))]
    async fn game_status(&self) -> Out {
        let dir = game::game_dir()?;
        // every lab: is a game of it running with the loader (its bridge.json's pid alive)
        let labs: Vec<Value> = game::labs().into_iter().map(|(build, d)| {
            let pid = game::running_pid(&d);
            json!({"game": build, "dir": d, "running": pid.is_some(), "pid": pid})
        }).collect();
        let ping = self.bridge.call("ping", json!({}), 3000).await;
        match ping {
            Ok(p) => {
                let mods = self.bridge.call("mods.list", json!({}), 3000).await.unwrap_or(Value::Null);
                Ok(pretty(&json!({"running": true, "game_dir": dir, "ping": p, "mods": mods, "labs": labs})))
            }
            Err(e) => Ok(pretty(&json!({"running": false, "game_dir": dir, "why": e, "labs": labs,
                "hint": "game_launch starts it; log_tail reads the last run's log from disk; `game` picks a lab"}))),
        }
    }

    /// Call any bridge method or mod command with JSON params: full access to the loader and every mod.
    #[tool]
    async fn call(&self, Parameters(a): Parameters<CallArgs>) -> Out {
        self.call_value(&a.method, a.params.unwrap_or(Value::Null), a.timeout_ms).await.map(|v| pretty(&v))
    }

    /// Every method the bridge answers: the host's (each with its params, its result and the thread that answers
    /// it, from docs/PROTOCOL.md) and those the loaded mods registered, with their help. `query` keeps the ones
    /// whose name or text holds it ("world.", "light", "camera", "human"): the whole list is long.
    #[tool(annotations(read_only_hint = true))]
    async fn commands_list(&self, Parameters(a): Parameters<CommandsListArgs>) -> Out {
        let v = self.call_value("commands.list", Value::Null, None).await?;
        let q = a.query.unwrap_or_default().to_lowercase();
        if q.is_empty() {
            return Ok(pretty(&v));
        }
        let kept: Vec<Value> = v
            .as_array()
            .map(|all| all.iter().filter(|c| c.to_string().to_lowercase().contains(&q)).cloned().collect())
            .unwrap_or_default();
        Ok(pretty(&Value::Array(kept)))
    }

    /// Read the game's memory (never faults: an unreadable address is an error). Returns hex, and the values when
    /// `as` is given.
    #[tool(annotations(read_only_hint = true))]
    async fn mem_read(&self, Parameters(a): Parameters<MemReadArgs>) -> Out {
        let mut p = json!({"addr": a.addr, "size": a.size.unwrap_or(64)});
        if let Some(t) = a.as_ { p["as"] = json!(t); }
        self.call_value("mem.read", p, None).await.map(|v| pretty(&v))
    }

    /// Write the game's memory (code pages are unprotected for the write).
    #[tool(annotations(destructive_hint = true))]
    async fn mem_write(&self, Parameters(a): Parameters<MemWriteArgs>) -> Out {
        let mut p = json!({"addr": a.addr});
        if let Some(h) = a.hex { p["hex"] = json!(h); }
        if let Some(t) = a.as_ { p["as"] = json!(t); }
        if let Some(v) = a.values { p["values"] = json!(v); }
        self.call_value("mem.write", p, None).await.map(|v| pretty(&v))
    }

    /// The loader's log: the live tail from the running game, or the file on disk when the game is not running.
    #[tool(annotations(read_only_hint = true))]
    async fn log_tail(&self, Parameters(a): Parameters<LogArgs>) -> Out {
        let lines = a.lines.unwrap_or(100);
        if !a.previous.unwrap_or(false) {
            if let Ok(v) = self.bridge.call("log.tail", json!({"lines": lines}), 3000).await {
                let text: Vec<String> = v.as_array().map(|a| a.iter().filter_map(|l| l.as_str().map(String::from)).collect()).unwrap_or_default();
                return Ok(text.join("\n"));
            }
        }
        game::log_file_tail(a.previous.unwrap_or(false), lines as usize)
    }

    /// The mods: id, version, loaded or not (and why not), generation (hot reloads).
    #[tool(annotations(read_only_hint = true))]
    async fn mods_list(&self) -> Out {
        self.call_value("mods.list", Value::Null, None).await.map(|v| pretty(&v))
    }

    /// Hot-reload a mod: unloaded (everything it registered goes), its DLL copied again, loaded. "*": every mod.
    #[tool]
    async fn mod_reload(&self, Parameters(a): Parameters<IdArgs>) -> Out {
        self.call_value("mod.reload", json!({"id": a.id}), Some(15000)).await.map(|v| pretty(&v))
    }

    /// What the game's main thread is doing now (sampled 5 times): for a hang, a loading screen, a pause.
    #[tool(name = "where", annotations(read_only_hint = true))]
    async fn main_thread_where(&self) -> Out {
        self.bridge.call("where", json!({}), 5000).await.map(|v| kb::annotate(&v.as_str().map(String::from).unwrap_or_else(|| pretty(&v))))
    }

    /// Start the game (Game.exe in the game's folder) and wait for the loader's bridge. With `go` (and `profile`),
    /// the loader takes it there by the game's own menus, the logo videos skipped: e.g. {"profile": 4, "go":
    /// "freeride", "police": 0} returns once the player is in the freeride (a few seconds), in a city without
    /// police patrols; {"profile": 4, "go": "freeride", "kind": "city-night", "place": "Down Town"} the night city;
    /// {"profile": 4, "go": "load", "save": "Old Prison"} a chapter from the profile's saves (it returns once the
    /// player exists: a mission's opening cutscene may run before that; `wait_s` for long loads).
    #[tool]
    async fn game_launch(&self, Parameters(a): Parameters<LaunchArgs>) -> Out {
        if self.bridge.call("ping", json!({}), 1500).await.is_ok() {
            return Err("the game is already running (game_close first)".into());
        }
        let asked = a.go.is_some() || a.profile.is_some() || a.skip_logos == Some(true);
        let background = a.background.unwrap_or(true);
        if asked || background {
            // read (and deleted) by the loader at the game's start: host/start.cpp
            let skip = if asked { a.skip_logos.unwrap_or(true) } else { false };
            let mut req = json!({"skip_logos": skip, "go": a.go.clone().unwrap_or_default(), "background": background});
            if let Some(p) = &a.profile { req["profile"] = p.clone(); }
            if let Some(k) = &a.kind { req["kind"] = json!(k); }
            if let Some(p) = &a.place { req["place"] = p.clone(); }
            if let Some(s) = &a.save { req["save"] = s.clone(); }
            for (name, v) in [("traffic", a.traffic), ("pedestrians", a.pedestrians), ("police", a.police)] {
                if let Some(v) = v { req[name] = json!(v); }
            }
            let path = game::game_dir()?.join("mml").join("start.json");
            std::fs::write(&path, req.to_string()).map_err(|e| format!("{}: {e}", path.display()))?;
        }
        let pid = game::launch(background)?;
        self.bridge.reset().await;
        let wait = a.wait_s.unwrap_or(60);
        let start = std::time::Instant::now();
        let mut ping = None;
        while start.elapsed().as_secs() < wait {
            tokio::time::sleep(std::time::Duration::from_millis(500)).await;
            if let Ok(p) = self.bridge.call("ping", json!({}), 1500).await {
                if p.get("pid").and_then(|v| v.as_u64()) == Some(pid as u64) { ping = Some(p); break; }
            }
            if !game::alive(pid) {
                return Err(format!("Game.exe (pid {pid}) exited; log_tail shows why (a game of this folder already \
                                    running makes another quit at once: one game a folder, as the game allows)"));
            }
        }
        let Some(ping) = ping else {
            return Err(format!("Game.exe started (pid {pid}) but the bridge did not answer in {wait} s: is MafiaModLoader.asi installed and Ultimate ASI Loader present?"));
        };
        let front = move || game::foreground_pid() == pid;   // (the game's window the one in front)
        if !asked || a.go.as_deref().unwrap_or("").is_empty() {
            return Ok(pretty(&json!({"started": pid, "ping": ping, "in_front": front()})));
        }
        // the start's steps run in the menus' frames; then (freeride) the mission loads until the player exists
        while start.elapsed().as_secs() < wait {
            if let Ok(s) = self.call_value("start.state", Value::Null, Some(2000)).await {
                match s["state"].as_str() {
                    Some("failed") => return Err(format!("the start failed: {}; the menus are left as they are", s["error"])),
                    Some("done") => {
                        if a.go.as_deref() == Some("menu") { return Ok(pretty(&json!({"started": pid, "start": s, "in_front": front()}))); }
                        if let Ok(p) = self.call_value("player", Value::Null, Some(2000)).await {
                            if p.get("actor").is_some() { return Ok(pretty(&json!({"started": pid, "start": s, "player": p, "in_front": front()}))); }
                        }
                    }
                    _ => {}
                }
            }
            if !game::alive(pid) { return Err(format!("Game.exe (pid {pid}) exited during the start; log_tail shows why")); }
            tokio::time::sleep(std::time::Duration::from_millis(500)).await;
        }
        Err(format!("Game.exe (pid {pid}) runs but the start did not finish in {wait} s (call start.state, menu.state)"))
    }

    /// Close the game: its window asked to close, killed (by its exact pid) if it does not within 10 s.
    #[tool(annotations(destructive_hint = true))]
    async fn game_close(&self, Parameters(a): Parameters<CloseArgs>) -> Out {
        let pid = game::bridge_pid().ok_or("no mml\\bridge.json: the game has not run with the loader")?;
        if !game::alive(pid) { return Ok(format!("pid {pid} is not running")); }
        let r = game::close(pid, a.force.unwrap_or(false)).await;
        self.bridge.reset().await;
        r
    }

    /// The crash dumps the loader wrote (mml\crash), newest first.
    #[tool(annotations(read_only_hint = true))]
    async fn crash_list(&self) -> Out {
        crash::list().map(|v| pretty(&v))
    }

    /// Open a crash dump: the exception, the faulting module+offset, the registers, the faulting address's memory
    /// region (freed? decommitted?), the code addresses on the crashing thread's stack.
    #[tool(annotations(read_only_hint = true))]
    async fn crash_inspect(&self, Parameters(a): Parameters<CrashArgs>) -> Out {
        crash::inspect(a.file).map(|v| kb::annotate(&pretty(&v)))
    }

    /// Start here after a crash: everything about it in one answer. The newest dump (or `file`) with the report the
    /// loader wrote beside it: a verdict (what, where, through whose code: the loader, a mod, or the game alone), the
    /// call stack with the game's functions and the mods named, the access and its region, the registers, the log's
    /// last lines, the taps' last calls, the last events, the mods and the mission. When the game is running: also
    /// the mods that faulted in this run (the game went on), each with its stack.
    #[tool(annotations(read_only_hint = true))]
    async fn crash_triage(&self, Parameters(a): Parameters<CrashArgs>) -> Out {
        let crash = crash::triage(a.file);
        let mut out = json!({});
        let mut running = false;
        if let Ok(p) = self.bridge.call("ping", json!({}), 2000).await {
            running = true;
            let ev = self.bridge.call("events.since", json!({"seq": 0, "types": "MOD_FAULTED"}), 3000).await.unwrap_or(Value::Null);
            let faults = ev["events"].as_array().cloned().unwrap_or_default();
            out["running"] = json!({"pid": p["pid"], "build": p["build"], "uptime_s": p["uptime_s"],
                                    "faults": if faults.is_empty() { json!("no mod faulted in this run") } else { json!(faults) }});
        }
        match crash {
            Ok(c) => out["crash"] = c,
            Err(e) if running => out["crash"] = json!(e),
            Err(e) => return Err(e),
        }
        Ok(kb::annotate(&pretty(&out)))
    }

    /// Watch any function of the running game without writing a hook: who calls it, with what arguments (ECX, the
    /// stack), what memory behind them holds (strings shown as text), what it returns. Start a tap, let the game (or
    /// the user) do the thing, then "show <id>". Works before the first frame too (during the intro videos).
    #[tool]
    async fn tap(&self, Parameters(a): Parameters<TapArgs>) -> Out {
        let v = self.bridge.call("tap", json!({"cmd": a.cmd}), 5000).await?;
        let text = v.as_str().map(String::from).unwrap_or_else(|| pretty(&v));
        if text.starts_with("error") { Err(text) } else { Ok(kb::annotate_tap(&text)) }
    }

    /// Who writes (or reads) a field: a hardware watchpoint on the game thread. {"set": "0x0EB9F024"} starts one
    /// (its id), let the game do the thing, {"show": id} gives every piece of code that wrote it (named from the
    /// knowledge base), how often, its callers and the values before and after; {"clear": id | "all"}.
    #[tool]
    async fn watch(&self, Parameters(a): Parameters<WatchArgs>) -> Out {
        let v = if let Some(addr) = &a.set {
            let mut p = json!({"addr": addr, "mode": a.mode.clone().unwrap_or_else(|| "write".into())});
            if let Some(l) = a.len { p["len"] = json!(l); }
            self.call_value("watch.set", p, None).await?
        } else if let Some(id) = a.show {
            self.call_value("watch.show", json!({"id": id}), None).await?
        } else if let Some(c) = &a.clear {
            let p = if c.as_str() == Some("all") { json!({"all": true}) } else { json!({"id": c}) };
            self.call_value("watch.clear", p, None).await?
        } else {
            return Err("watch: {\"set\": addr} | {\"show\": id} | {\"clear\": id | \"all\"}".into());
        };
        Ok(kb::annotate(&pretty(&v)))
    }

    /// Wait for something to happen in the game: until an event of `types` (with `filter`'s fields) comes, or
    /// `timeout_s` passes. Returns the event (its sequence number, time and data) and `latest`, to wait again from.
    /// "Until the mission is loaded": {"types": "MISSION_LOADED"}; "until he dies": {"types": "HUMAN_HIT",
    /// "filter": {"actor": "0x...", "killed": true}}; a script's end: {"types": "SCRIPT_ENDED"}.
    #[tool(annotations(read_only_hint = true))]
    async fn wait(&self, Parameters(a): Parameters<WaitArgs>) -> Out {
        let mut seq = match a.since {
            Some(s) => s,
            None => self.bridge.call("events.since", json!({"seq": u64::MAX, "limit": 1}), 3000).await?["latest"].as_u64().unwrap_or(0),
        };
        let begun = std::time::Instant::now();
        let timeout = a.timeout_s.unwrap_or(30.0).max(0.1);
        loop {
            let mut p = json!({"seq": seq, "types": a.types, "limit": 1});
            if let Some(f) = &a.filter { p["filter"] = f.clone(); }
            match self.bridge.call("events.since", p, 3000).await {
                Ok(r) => {
                    if let Some(e) = r["events"].as_array().and_then(|v| v.first()) {
                        return Ok(pretty(&json!({"event": e, "latest": r["latest"], "waited_s": begun.elapsed().as_secs_f64()})));
                    }
                    seq = seq.max(r["latest"].as_u64().unwrap_or(seq));
                }
                Err(e) if begun.elapsed().as_secs_f64() > timeout => return Err(format!("no answer from the game: {e}")),
                Err(_) => {}   // (the game restarting or busy: tried again)
            }
            if begun.elapsed().as_secs_f64() > timeout {
                return Err(format!("no {} in {:.1} s (latest {}): `call events.since` shows what came", a.types, timeout, seq));
            }
            tokio::time::sleep(std::time::Duration::from_millis(100)).await;
        }
    }

    /// Where the game thread's time goes: samples it every `interval_ms` for `seconds` (the phase: the game's own
    /// work, LS3D drawing the frame, the scene views, the graphics bridge; inside which mod's call; the address, and
    /// for samples in Direct3D or the driver the game's code that called them, or the loader's); `functions`: each
    /// function's self time, its calls into Direct3D and the system (calls_out) and its total (on the stack, checked
    /// frame by frame), the game's named from the knowledge base and the loader's and mods' from their PDBs; and what
    /// each mod costs a frame by site, the frame times and the hitches since the last reset.
    #[tool(annotations(read_only_hint = true))]
    async fn profile(&self, Parameters(a): Parameters<ProfileArgs>) -> Out {
        if a.reset == Some(true) { self.call_value("prof.reset", Value::Null, None).await?; }
        let secs = a.seconds.unwrap_or(3.0).clamp(0.5, 30.0);
        let mut ask = json!({"seconds": secs});
        if let Some(i) = a.interval_ms { ask["interval_ms"] = json!(i); }
        let start = self.call_value("prof.sample", ask, None).await?;
        let id = start["id"].as_u64().ok_or("prof.sample gave no id")?;
        let begun = std::time::Instant::now();
        let report = loop {
            tokio::time::sleep(std::time::Duration::from_millis(300)).await;
            let r = self.call_value("prof.sample", json!({"id": id}), None).await?;
            if r["done"].as_bool() == Some(true) { break r; }
            if begun.elapsed().as_secs_f64() > secs + 20.0 { return Err("the sampler did not finish (the game thread paused or hung?)".into()); }
        };
        let mut report = report;
        let functions = tokio::task::spawn_blocking({
            let r = report.clone();
            let dir = game::game_dir()?;   // (the call's game: the build that names the addresses, its PDBs)
            move || game::with_game_sync(dir, || stacks::by_function(&r))
        }).await.map_err(|e| e.to_string())?;
        if let Some(o) = report.as_object_mut() { o.remove("addresses"); o.remove("chains"); }
        let mut out = json!({"sample": report, "functions": functions});
        if a.state.unwrap_or(true) { out["frame"] = self.call_value("prof.state", Value::Null, None).await?; }
        Ok(kb::annotate(&pretty(&out)))
    }

    /// Compare two configurations of the running game in one run: what B costs or saves against A, with a 95 %
    /// interval, free of the run's drifts. They take turns in blocks (ABBA, BAAB), each turn a segment of `frames`
    /// frames after `settle` let pass; each frame's record (prof.frames) gives its time, the time Present blocked,
    /// the game thread's own CPU time (off_cpu_ms: what it was not running outside Present: other processes, waits),
    /// the process's other threads, the GPU's span, and while counted the device's calls by kind and caller (the game,
    /// the host), the driver's time under the draws, the pipeline's switches and the host's costly sites. Returns
    /// each configuration's figures, B less A with its interval and the measure's own noise, and the run's conditions
    /// (other games, the machine's other load, frames not presented) with warnings. Without `b`, A against itself:
    /// the check of the measure. Keep the scene still meanwhile (a camera set, no traffic): what moves adds noise.
    #[tool]
    async fn bench(&self, Parameters(a): Parameters<BenchArgs>) -> Out {
        let blocks = a.blocks.unwrap_or(10).clamp(2, 40) as usize;
        let frames = a.frames.unwrap_or(100).clamp(30, 4000) as usize;
        let settle = a.settle.unwrap_or(30).min(1000);
        let counters = a.counters.unwrap_or(true);
        let same = a.b.is_none();
        let calls = [a.a.clone().unwrap_or_default(), a.b.clone().unwrap_or_else(|| a.a.clone().unwrap_or_default())];
        let mut names = a.names.clone().unwrap_or_default();
        if names.len() < 2 { names = if same { vec!["A".into(), "A'".into()] } else { vec!["A".into(), "B".into()] }; }
        if names[0] == names[1] { names[1].push('\''); }
        let before = self.call_value("prof.state", Value::Null, None).await?;
        let was_counting = before["counters"].as_bool().unwrap_or(false);
        // the counters when asked (they need the game's Direct3D 9); the GPU's time whenever the device allows it
        let mut gpu_note = Value::Null;
        if counters {
            self.call_value("prof.counters", json!({"on": true, "gpu": true}), None).await
                .map_err(|e| format!("bench: prof.counters: {e} (a loader with prof.frames is needed: deploy it with the game closed)"))?;
        } else {
            match self.call_value("prof.counters", json!({"on": false, "gpu": true}), None).await {
                Ok(r) if r["gpu"].as_str() != Some("measured") => gpu_note = json!(format!("no GPU time: {}", r["gpu"].as_str().unwrap_or("?"))),
                Err(e) => gpu_note = json!(format!("no GPU time: {e}")),
                _ => {}
            }
        }
        let pid = game::running_pid(&game::game_dir()?);
        let games = bench::game_processes();
        let cpu0 = bench::cpu_mark(pid);
        let mut since = before["seq"].as_u64().unwrap_or(1).saturating_sub(1);
        let run = format!("bench{}", std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() % 100000).unwrap_or(0));
        let mut segs = vec![];
        let mut flags = [0usize; 2];   // frames Present answered otherwise, frames the sampler stopped
        let result: Result<(), String> = async {
            // each configuration once first: hooks made, shaders compiled, caches filled
            for c in 0..2 {
                self.bench_segment(&calls[c], &format!("{run}/warmup/{c}"), settle, settle.max(30) as usize, &mut since).await?;
            }
            for b in 0..blocks {
                let order = if b % 2 == 0 { [0, 1, 1, 0] } else { [1, 0, 0, 1] };
                for (k, &c) in order.iter().enumerate() {
                    let f = self.bench_segment(&calls[c], &format!("{run}/{b}/{k}"), settle, frames, &mut since).await?;
                    for fr in &f {
                        let fl = fr.get("flags").copied().unwrap_or(0.0) as u32;
                        if fl & 2 != 0 { flags[0] += 1; }
                        if fl & 8 != 0 { flags[1] += 1; }
                    }
                    segs.push(bench::Segment { config: c, block: b, frames: f.len(), stats: bench::segment_stats(&f) });
                }
            }
            Ok(())
        }.await;
        let cpu1 = bench::cpu_mark(pid);
        // the game left as asked (A's by default), labels and counting as they were
        let _ = self.call_value("prof.mark", json!({"label": ""}), None).await;
        for c in a.after.clone().unwrap_or_else(|| calls[0].clone()) {
            let _ = self.call_value(&c.method, c.params.clone().unwrap_or(Value::Null), None).await;
        }
        if !was_counting { let _ = self.call_value("prof.counters", json!({"on": false, "gpu": false}), None).await; }
        result?;
        let state = self.call_value("prof.state", Value::Null, None).await.unwrap_or(Value::Null);
        let mut out = bench::compare(&segs, [&names[0], &names[1]]);
        let cpu = bench::cpu_between(&cpu0, &cpu1);
        let others: Vec<u32> = games.iter().copied().filter(|p| Some(*p) != pid).collect();
        let mut warnings = vec![];
        if !others.is_empty() { warnings.push(format!("{} other Game.exe running (pids {:?}): they share the machine", others.len(), others)); }
        if cpu["others_percent"].as_f64().unwrap_or(0.0) > 10.0 {
            warnings.push(format!("other processes kept {}% of the machine busy", cpu["others_percent"]));
        }
        if flags[0] > 0 { warnings.push(format!("{} frames Present did not answer S_OK (the window occluded, the device lost)", flags[0])); }
        if flags[1] > 0 { warnings.push(format!("{} frames the sampler stopped the game thread in (prof.sample ran meanwhile)", flags[1])); }
        for (i, n) in names.iter().enumerate() {
            let off = out["configs"][n]["off_cpu_ms"].as_f64().unwrap_or(0.0);
            let work = out["configs"][n]["work_ms"].as_f64().unwrap_or(0.0);
            if work > 0.0 && off > 0.1 * work {
                warnings.push(format!("{n}: the game thread was not running {off:.2} ms of its {work:.2} ms of work a frame (other processes, the game's waits)"));
            }
            let _ = i;
        }
        // the game's other threads (the driver's, the sound's, the bridge's): the process's cores over the run less the
        // game thread's (its frames' own CPU time over their time)
        let mut cpu = cpu;
        if let (Some(gp), Some(cores)) = (cpu["game_percent"].as_f64(), cpu["cores"].as_f64()) {
            let (c, w): (f64, f64) = segs.iter().fold((0.0, 0.0), |acc, s| {
                (acc.0 + s.stats.get("cpu_ms").copied().unwrap_or(0.0), acc.1 + s.stats.get("wall_ms").copied().unwrap_or(0.0))
            });
            if w > 0.0 {
                let main = c / w;
                cpu["game_thread_cores"] = json!((main * 100.0).round() / 100.0);
                cpu["game_other_threads_cores"] = json!(((gp / 100.0 * cores - main).max(0.0) * 100.0).round() / 100.0);
            }
        }
        if !gpu_note.is_null() { warnings.push(gpu_note.as_str().unwrap_or("").to_string()); }
        out["conditions"] = json!({"game_processes": games.len(), "cpu": cpu, "in_front": state["in_front"],
                                   "frames_present_not_ok": flags[0], "frames_sampled": flags[1], "warnings": warnings});
        out["names"] = json!(names);
        out["settings"] = json!({"blocks": blocks, "frames": frames, "settle": settle, "counters": counters, "same": same,
                                 "a": calls[0].iter().map(|c| json!({"method": c.method, "params": c.params})).collect::<Vec<_>>(),
                                 "b": calls[1].iter().map(|c| json!({"method": c.method, "params": c.params})).collect::<Vec<_>>()});
        out["segments"] = json!(segs.iter().map(|s| json!({"config": names[s.config], "block": s.block, "frames": s.frames,
            "wall_ms": s.stats.get("wall_ms"), "cpu_ms": s.stats.get("cpu_ms")})).collect::<Vec<_>>());
        Ok(pretty(&out))
    }

    /// Run a program on the game's own script machine (the mission scripts' language, compiled and run by the game's
    /// interpreter, one step a mission tick): every command the game's missions have (teleport, spawn a copy of an
    /// actor, give weapons, kill, cars, weather, traffic, camera, fades, the HUD's console...), with the game's own
    /// behaviour. Waits until it ends; returns its state (done / failed with the line / stopped) and its registers
    /// (flt[], act[] with each actor's type and model, frm[]). A mission must be running (not paused: the pause
    /// does not tick it).
    #[tool]
    async fn script(&self, Parameters(a): Parameters<ScriptArgs>) -> Out {
        let timeout = a.timeout_ms.unwrap_or(10000);
        let mut req = json!({"code": a.code, "timeout_ms": timeout});
        if let Some(v) = a.act { req["act"] = v; }
        if let Some(v) = a.flt { req["flt"] = v; }
        if let Some(v) = a.owner { req["owner"] = v; }
        let started = self.bridge.call("script.run", req, 5000).await?;
        let id = started["id"].as_u64().ok_or("script.run: no id")?;
        if a.no_wait.unwrap_or(false) { return Ok(pretty(&started)); }
        let t0 = std::time::Instant::now();
        loop {
            tokio::time::sleep(std::time::Duration::from_millis(100)).await;
            let s = self.bridge.call("script.state", json!({"id": id}), 3000).await?;
            if s["state"].as_str() != Some("running") {
                return if s["state"].as_str() == Some("done") { Ok(pretty(&s)) } else { Err(pretty(&s)) };
            }
            if t0.elapsed().as_millis() as u64 > timeout + 3000 {
                return Err(format!("script {id} still runs after {} ms (the mission paused?): {}", timeout + 3000, pretty(&s)));
            }
        }
    }

    /// Make a new car of any model, or a person of any model, in the world, by the game's own functions (as its
    /// scripts' actor_duplicate makes a copy): a real actor, driven, hit, entered like the mission's own. E.g.
    /// {"car": "Bolt-Thrower"} in front of the player, {"human": "Paulie", "in": "car"} beside him in his car.
    /// Returns the actor (its address for `teleport`, `give`, `script`; `call world.remove {actor}` takes it out).
    #[tool]
    async fn spawn(&self, Parameters(a): Parameters<SpawnArgs>) -> Out {
        self.call_value("world.spawn", params_of(&a), None).await.map(|v| pretty(&v))
    }

    /// Move an actor (the player on foot or in his car, a car, a character) to a place or in front of another
    /// actor, turned when asked: the scripts' actor_setpos, run at once (a car is also stopped, set on its wheels
    /// and repaired unless `repair` is false). Returns where it is now.
    #[tool]
    async fn teleport(&self, Parameters(a): Parameters<TeleportArgs>) -> Out {
        self.call_value("world.teleport", params_of(&a), None).await.map(|v| pretty(&v))
    }

    /// Give a weapon or an item by its name to the player (into his inventory) or a character (in his hands at
    /// once): the scripts' human_addweapon, with a full magazine and the usual reserve unless told otherwise.
    #[tool]
    async fn give(&self, Parameters(a): Parameters<GiveArgs>) -> Out {
        self.call_value("world.give", params_of(&a), None).await.map(|v| pretty(&v))
    }

    /// The mission scripts' commands (the language of `script`): each one's opcode and handler in 1.2 and 1.0
    /// ("runs": false where the standalone program has no case for it), with its arguments and what it does where
    /// st0rm94's annotated 1.0 reverse is cloned (re/ext/mafia-reverse-engineering-export; a lead, read in place).
    #[tool(annotations(read_only_hint = true))]
    async fn script_commands(&self, Parameters(a): Parameters<ScriptCommandsArgs>) -> Out {
        script::commands(a.query.as_deref().unwrap_or(""), a.notes.unwrap_or(true)).map(|v| pretty(&v))
    }

    /// Search the game's knowledge base (every function of Game.exe and LS3DF.dll 1.2 from the full decompiles:
    /// names, classes, signatures, topics, the strings each uses; our own names from knowledge/<module>/names.csv;
    /// 1.0's names, st0rm94's and ours, by their 1.0 addresses in `functions_10`; the matching lines of the notes in
    /// knowledge/notes). Start here for any question about the game.
    #[tool(annotations(read_only_hint = true))]
    async fn kb_search(&self, Parameters(a): Parameters<KbSearchArgs>) -> Out {
        let limit = a.limit.unwrap_or(40);
        let mut v = kb::search(&a.query, a.module.as_deref(), limit)?;
        v["structs"] = structs::search(&a.query.to_lowercase(), limit);
        Ok(pretty(&v))
    }

    /// Restore points: repeat an experiment from the same moment. `save` keeps the game as it is now (every actor,
    /// the mission's programs, the mods' state: the game's own save, whole, in mml\restore; the profile's saves and
    /// its list untouched), `load` puts the game back there by the game's own load (at the game loop's next frame;
    /// another mission's point opens that mission) and returns once it is loaded, with the player's state;
    /// `delete` removes one; with none of them, the list. A mission in play only (not the main menu; a load waits
    /// for the pause menu to close).
    #[tool]
    async fn restore(&self, Parameters(a): Parameters<RestoreArgs>) -> Out {
        if let Some(n) = a.save {
            return self.call_value("restore.save", json!({"name": n}), None).await.map(|v| pretty(&v));
        }
        if let Some(n) = a.delete {
            return self.call_value("restore.delete", json!({"name": n}), None).await.map(|v| pretty(&v));
        }
        let Some(n) = a.load else {
            return self.call_value("restore.list", json!({}), None).await.map(|v| pretty(&v));
        };
        let mut seq = self.bridge.call("events.since", json!({"seq": u64::MAX, "limit": 1}), 3000).await?["latest"].as_u64().unwrap_or(0);
        let asked = self.call_value("restore.load", json!({"name": n}), None).await?;
        let begun = std::time::Instant::now();
        let timeout = a.timeout_s.unwrap_or(60.0).max(1.0);
        loop {
            if let Ok(r) = self.bridge.call("events.since", json!({"seq": seq, "types": "RESTORE_LOADED", "limit": 1}), 3000).await {
                if let Some(e) = r["events"].as_array().and_then(|v| v.first()) {
                    let player = self.call_value("player", json!({}), None).await.unwrap_or(Value::Null);
                    return Ok(pretty(&json!({"loaded": e["data"], "player": player})));
                }
                seq = seq.max(r["latest"].as_u64().unwrap_or(seq));
            }
            if begun.elapsed().as_secs_f64() > timeout {
                return Err(format!("{n}: asked ({asked}), not loaded in {timeout:.0} s (a menu open? the game loop loads it once it runs)"));
            }
            tokio::time::sleep(std::time::Duration::from_millis(100)).await;
        }
    }

    /// An object of the running game read by its structure (knowledge/structs: the SDK's fields, the classes read
    /// from both builds' constructors, st0rm94's fields carried to 1.2): its class told by its vtable, every field
    /// named, typed and valued for the build running, pointers named (a class, a frame's name, a module's place)
    /// and followed with `depth`. With no `at`: a structure's layout, both builds' offsets with their sources.
    /// kb_search finds structures and fields by name.
    #[tool(annotations(read_only_hint = true))]
    async fn inspect(&self, Parameters(a): Parameters<InspectArgs>) -> Out {
        let st = structs::get()?;
        let filter = a.fields.unwrap_or_default();
        let Some(at) = a.at else {
            let name = a.as_.ok_or("`at` (an object of the game) or `as` (a structure's layout)")?;
            return structs::layout(&name, &filter).map(|v| pretty(&v));
        };
        let ping = self.bridge.call("ping", json!({}), 3000).await?;
        let build = ping["build"].as_str().unwrap_or_default().to_string();
        let b = structs::build_index(&build).ok_or_else(|| format!("the game's build is not known (\"{build}\")"))?;
        let ins = structs::Inspector { bridge: &self.bridge, b, st };
        let (addr, reached, how) = ins.resolve(&at).await?;
        let (s, how) = match a.as_ {
            Some(n) => (st.find(&n).ok_or_else(|| format!("no struct \"{n}\" (kb_search finds them by name)"))?, "as asked".to_string()),
            None => match ins.detect(addr).await {
                Some((s, why)) => (s, why),
                None => match reached {
                    Some(s) => (s, format!("as {how} is typed")),
                    None => return Err(format!("0x{addr:08X}: its vtable is none known; say `as`")),
                },
            },
        };
        let opts = structs::Opts { filter, depth: a.depth.unwrap_or(0).min(2), gaps: a.gaps.unwrap_or(false) };
        let mut v = ins.inspect(addr, s, &opts).await?;
        v["how"] = json!(how);
        Ok(pretty(&v))
    }

    /// Read the game's code in Ghidra, either build (`build` "1.2" or "1.0": a project each, both modules, the
    /// knowledge base's names on them): a function decompiled, its disassembly, callers, callees, every reference
    /// to an address, a vtable, the other build's twin. Where kb_function has no answer (1.0, cross-references,
    /// a function the decompile split): this. tools/ghidra_cli.py is the same from a terminal.
    #[tool(annotations(read_only_hint = true))]
    async fn ghidra_read(&self, Parameters(a): Parameters<GhidraReadArgs>) -> Out {
        ghidra::check(&a.op, ghidra::READ, "ghidra_read")?;
        let args = serde_json::to_value(&a).map_err(|e| e.to_string())?;
        ghidra::ask(a.build.as_deref().unwrap_or("1.2"), &a.op, args).await
    }

    /// Search the game's code in Ghidra, either build: who uses a value or touches a structure's field (an
    /// immediate or a displacement), the strings and who uses them, where the image holds a pointer or a byte
    /// pattern, the names given, functions by name.
    #[tool(annotations(read_only_hint = true))]
    async fn ghidra_find(&self, Parameters(a): Parameters<GhidraFindArgs>) -> Out {
        ghidra::check(&a.op, ghidra::FIND, "ghidra_find")?;
        let args = serde_json::to_value(&a).map_err(|e| e.to_string())?;
        ghidra::ask(a.build.as_deref().unwrap_or("1.2"), &a.op, args).await
    }

    /// Write what was learned into the build's Ghidra project and the repository at once: a name (into
    /// knowledge/<module>/names.csv or names_10.csv), a comment, a prototype (annotations.csv), a function made or
    /// removed. The projects are a cache made from the repository (`python tools/ghidra_cli.py setup`).
    #[tool]
    async fn ghidra_write(&self, Parameters(a): Parameters<GhidraWriteArgs>) -> Out {
        ghidra::check(&a.op, ghidra::WRITE, "ghidra_write")?;
        let args = serde_json::to_value(&a).map_err(|e| e.to_string())?;
        ghidra::ask(a.build.as_deref().unwrap_or("1.2"), &a.op, args).await
    }

    /// One function of the game: its facts, callers and callees (named where known), and its decompiled code.
    #[tool(annotations(read_only_hint = true))]
    async fn kb_function(&self, Parameters(a): Parameters<KbFunctionArgs>) -> Out {
        let v = kb::function(&a.key, a.code.unwrap_or(true), a.max_lines.unwrap_or(400))?;
        let code = v.get("code").and_then(|c| c.as_str()).map(String::from);
        let mut head = v.clone();
        if let Some(o) = head.as_object_mut() { o.remove("code"); }
        Ok(match code { Some(c) => format!("{}\n\n{}", pretty(&head), c), None => pretty(&head) })
    }

    /// Copy the build (build/x86/out: the loader and the mods) into the game's folder, and reload the mods in the
    /// running game. The loader itself is copied only while the game is not running. A mod's folder goes with its
    /// subfolders (its `files\`: game files), files unchanged left alone (`copied` lists what changed); a game file
    /// the game holds open is replaced all the same (it reads the old one until it closes it). A mod switched on or
    /// off in the game's folder (its mod.json there says `enabled`) stays so: `kept` names them. Only the mods the
    /// build defines go (build/x86/out/mml/build_mods.txt): a removed mod's old output folder is named in `skipped`, and
    /// the game folder's mods the build does not define are left as they are, named in `others`.
    #[tool]
    async fn deploy(&self, Parameters(a): Parameters<DeployArgs>) -> Out {
        let running = self.bridge.call("ping", json!({}), 1500).await.is_ok();
        let report = game::deploy(a.mods.as_deref(), running)?;
        let mut out = json!({"copied": report.copied, "skipped": report.skipped});
        if !report.kept.is_empty() { out["kept"] = json!(report.kept); }
        if !report.others.is_empty() { out["others"] = json!(report.others); }
        if running && a.reload.unwrap_or(true) {
            let mut reloads = vec![];
            for id in &report.mods {
                let r = self.call_value("mod.reload", json!({"id": id}), Some(15000)).await;
                reloads.push(json!({"id": id, "ok": r.is_ok(), "detail": r.map_or_else(|e| json!(e), |v| v)}));
            }
            out["reloaded"] = json!(reloads);
        }
        Ok(pretty(&out))
    }
}

const INSTRUCTIONS: &str = "MafiaModLoader: the live Mafia (2002) game, from the inside. \
game_status first (is it running? and every lab's state); game_launch / game_close start and stop it. Several \
games run side by side, one a folder (the 1.2 and the 1.0 labs together, beside a player's own): every tool that \
touches a game takes `game` (\"1.2\", \"1.0\": mml.config.json's labs, or a game's folder; default its game_dir), \
and calls to different games run at the same time. \
`call` reaches every bridge method and mod command (commands_list lists them): the core mod answers \
`game`, `player`, `car` (the player's car: speed, gear, fuel...; `car.save` / `car.restore` its whole state), \
`humans`, `actors`, `frame`, `frames.find`. mem_read / mem_write read and write any address \
(\"game+0x1762A0\", \"ls3d+1AAE0\"). Eyes and hands: `screenshot` returns the game's frame as a picture; `input` \
plays a timed sequence of the game's own actions (walk, drive, shoot, aim) and keys inside the game. \
Draw into the game's frame without a mod: `call draw.text {x, y, text, size, color, ms}` (800x600 units; \
draw.rect, draw.line, draw.clear). A mod's settings: `call config.get {mod}`, `config.set {mod, key, value}` \
(the mod is told at once). \
The game's own menus, extended (real controls, not an overlay): `call menu.state` shows the open menus' controls; \
`menu.item {menu, after, text}` adds an item to one of them (\"gamemenu\": the pause menu); `menu.page {name, \
controls}` opens a page made by the game from control descriptions; a mod does the same with menu_item, menu_open. \
A new mod: python tools/new_mod.py <id>, then docs/MODDING.md (mods/examples/telemetry is a complete one). \
The loop for a mod: edit -> build (cmake --build --preset x86-release) -> \
deploy -> call its commands -> log_tail. A moment to come back to: restore {save: name}, then restore {load: name} (the game's own save and load, whole). After a crash: crash_triage (the verdict, the named stack, the log, the taps' and the events' last ones, in one call). \
The world without a program: `spawn {car | human, at, in}` makes a car or a person of any model (the game's car list: \
`call world.cars`), `teleport {actor, to}` moves one, `give {actor, item}` arms by name (`call world.items`), \
`call world.seat {actor, car, seat}`, `world.remove {actor}`; mods: actor_spawn, actor_teleport, give_item, actor_seat. \
The game's own script machine: `script {code, act, flt}` runs a program in the missions' language (teleport, spawn, weapons, kill, cars, weather, camera: every command the missions have), `script_commands {query}` tells the commands' arguments, docs/SCRIPTS.md has recipes; a mod does the same with script_run. What a game function does, where a thing lives: kb_search, then kb_function (facts, calls, decompiled code); an object by its structure: `inspect {at: player}` (or car.vehicle, game.camera, an address: its class by its vtable, every field named and valued), `inspect {as: C_human}` a layout; \
knowledge/notes/ holds the hand-written findings. \
More through `call` (commands_list {query} gives each one's params and result; docs/PROTOCOL.md has them all): \
people: `humans.list`, `human.skeleton {actor}`, `human.pose {actor, bones, ms}`, `human.anim {actor, play | stop | \
replace, with}`, `human.anim_state`, `human.get_up {actor, blend_ms}`, `human.anims {query}`; the camera: `camera.set {eye, at, \
fov, ms}` (look at any place, then screenshot), `camera.state`, `camera.release`; questions to the world: \
`world.ray {from: \"camera\"}` (what the crosshair looks at), `world.sweep`, `world.ground {at}`, `world.near {at, \
radius}`, `world.shapes`; the game's state: `state.money {set}`, `human.health {set}`, `human.items`, \
`human.ammo {slot, loaded, reserve}`, `human.take {item}`; the scene's lights: `lights.list`, `light.set`; the \
effects: `exhaust.state`, `particles.defs`, `weather.state`, `weather.set`; props (models that are not actors): \
`prop.create {model, at | parent, bone, pos, dir, scale}`, `prop.place`, `prop.attach`, `prop.show`, \
`prop.remove`, `props.list`; sound: `sound.play {file, at | on, loop, pitch}`, `sound.set`, `sound.stop`, \
`music.play {file, loop}`, `music.game {on}`, `sounds.list`; control: `input.capture {key | action}`, \
`player.lock {on}`, `vehicle.body {car}`, `control.state`; interaction: `use.add {label}` (a line in the game's Action menu), `zone.add {at, \
radius}`, `timer.add {ms}` (then wait for USED, ZONE_ENTERED, TIMER), `interact.list`; the HUD: `hud.marker {at | on, \
label, compass, radar}`, `hud.console {text}`, `hud.help {text}`, `draw.picture {file, alpha, x, y, w, h, angle}`, \
`hud.list`; a mod's state in the game's saves: `save.state`, `save.put {key, value, scope}`, `save.get` (wait for \
GAME_SAVING, GAME_LOADED); what \
happened: `events.since` (or the wait tool); the frame: `prof.state` (or the profile tool), `gfx.state`, \
`view.state`, `geom.survey`; any game function: `native.call {at, conv, this, args, ret}`; who writes a field: \
the watch tool.";

/// The tools that do not touch a game (no `game` argument).
const NO_GAME: &[&str] = &["kb_search", "kb_function", "script_commands"];
const GAME_ARG: &str = "Which game, when several run side by side: a lab's build (\"1.2\", \"1.0\": mml.config.json's \
`labs`) or a game's folder. Default: mml.config.json's game_dir. Each game has its own bridge, log, crashes and \
deploy; calls to different games run at the same time.";

#[tool_handler(router = self.tool_router)]
impl ServerHandler for Server {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(ServerCapabilities::builder().enable_tools().build()).with_instructions(INSTRUCTIONS)
    }

    /// Every tool that touches a game takes `game` (GAME_ARG): it is taken out of the arguments and the call runs
    /// for that game's folder (game::with_game).
    async fn call_tool(
        &self,
        mut request: rmcp::model::CallToolRequestParams,
        context: rmcp::service::RequestContext<rmcp::RoleServer>,
    ) -> Result<rmcp::model::CallToolResponse, rmcp::ErrorData> {
        let game = request.arguments.as_mut().and_then(|a| a.remove("game"));
        let dir = match game {
            None | Some(Value::Null) => None,
            Some(Value::String(g)) => match game::resolve(&g) {
                Ok(d) => Some(d),
                Err(e) => return Ok(CallToolResult::error(vec![ContentBlock::text(e)]).into()),
            },
            Some(v) => return Ok(CallToolResult::error(vec![ContentBlock::text(format!("game: a string, not {v}"))]).into()),
        };
        let tcc = rmcp::handler::server::tool::ToolCallContext::new(self, request, context);
        game::with_game(dir, self.tool_router.call(tcc)).await
    }

    async fn list_tools(
        &self,
        _request: Option<rmcp::model::PaginatedRequestParams>,
        _context: rmcp::service::RequestContext<rmcp::RoleServer>,
    ) -> Result<rmcp::model::ListToolsResult, rmcp::ErrorData> {
        let mut tools = self.tool_router.list_all();
        for t in tools.iter_mut().filter(|t| !NO_GAME.contains(&t.name.as_ref())) {
            let mut schema = (*t.input_schema).clone();
            let props = schema.entry("properties").or_insert_with(|| json!({}));
            if let Some(p) = props.as_object_mut() {
                p.insert("game".into(), json!({"type": ["string", "null"], "default": null, "description": GAME_ARG}));
            }
            schema.entry("type").or_insert_with(|| json!("object"));
            t.input_schema = Arc::new(schema);
        }
        Ok(rmcp::model::ListToolsResult::with_all_items(tools))
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let service = Server::new().serve(rmcp::transport::stdio()).await?;
    service.waiting().await?;
    Ok(())
}
