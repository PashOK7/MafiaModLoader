//! mml-mcp: the MCP server of MafiaModLoader (stdio).
//!
//! It runs outside the game and talks to the loader inside it over the bridge (127.0.0.1, the port and a per-run token
//! in `<game>\mml\bridge.json`; see docs/PROTOCOL.md). It survives the game's restarts and crashes: it launches and
//! closes the game, reads the loader's log from disk when the game is gone, and opens the crash dumps the loader writes.
//!
//! The game's folder: `MML_GAME_DIR`, else `game_dir` in `mml.config.json` at the repository's root.

mod bridge;
mod crash;
mod game;
mod kb;

use rmcp::{
    ServerHandler, ServiceExt,
    handler::server::{router::tool::ToolRouter, wrapper::Parameters},
    model::{ServerCapabilities, ServerConfig},
    tool, tool_handler, tool_router,
};
use schemars::JsonSchema;
use serde::Deserialize;
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
    /// Where to take the game by its own menus: "menu" (the main menu) or "freeride" (the default freeride,
    /// missions\freeitaly). Omitted (and no profile): the game's usual start, the player at the keyboard.
    #[serde(default)]
    go: Option<String>,
    /// The profile to take in the profile menu: its number (savegame\mafiaNNN.sav) or its name.
    #[serde(default)]
    profile: Option<Value>,
    /// Skip the three logo videos before the profile menu (default true when go or profile is given).
    #[serde(default)]
    skip_logos: Option<bool>,
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
    /// Only these mods (ids); default every mod in the build. The loader itself (MafiaModLoader.asi) is copied
    /// only while the game is not running (Windows keeps it locked).
    #[serde(default)]
    mods: Option<Vec<String>>,
    /// Reload the deployed mods in the running game afterwards (default true).
    #[serde(default)]
    reload: Option<bool>,
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

    /// Is the game running with the loader, and what state is it in: pid, build (1.2/1.0), frames, the mods.
    #[tool(annotations(read_only_hint = true))]
    async fn game_status(&self) -> Out {
        let dir = game::game_dir()?;
        let ping = self.bridge.call("ping", json!({}), 3000).await;
        match ping {
            Ok(p) => {
                let mods = self.bridge.call("mods.list", json!({}), 3000).await.unwrap_or(Value::Null);
                Ok(pretty(&json!({"running": true, "game_dir": dir, "ping": p, "mods": mods})))
            }
            Err(e) => Ok(pretty(&json!({"running": false, "game_dir": dir, "why": e,
                "hint": "game_launch starts it; log_tail reads the last run's log from disk"}))),
        }
    }

    /// Call any bridge method or mod command with JSON params: full access to the loader and every mod.
    #[tool]
    async fn call(&self, Parameters(a): Parameters<CallArgs>) -> Out {
        self.call_value(&a.method, a.params.unwrap_or(Value::Null), a.timeout_ms).await.map(|v| pretty(&v))
    }

    /// Every method the bridge answers: the host's and those the loaded mods registered, with their help.
    #[tool(annotations(read_only_hint = true))]
    async fn commands_list(&self) -> Out {
        self.call_value("commands.list", Value::Null, None).await.map(|v| pretty(&v))
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
    /// "freeride"} returns once the player is in the freeride (a few seconds).
    #[tool]
    async fn game_launch(&self, Parameters(a): Parameters<LaunchArgs>) -> Out {
        if self.bridge.call("ping", json!({}), 1500).await.is_ok() {
            return Err("the game is already running (game_close first)".into());
        }
        let asked = a.go.is_some() || a.profile.is_some() || a.skip_logos == Some(true);
        if asked {
            // read (and deleted) by the loader at the game's start: host/start.cpp
            let mut req = json!({"skip_logos": a.skip_logos.unwrap_or(true), "go": a.go.clone().unwrap_or_default()});
            if let Some(p) = &a.profile { req["profile"] = p.clone(); }
            let path = game::game_dir()?.join("mml").join("start.json");
            std::fs::write(&path, req.to_string()).map_err(|e| format!("{}: {e}", path.display()))?;
        }
        let pid = game::launch()?;
        self.bridge.reset().await;
        let wait = a.wait_s.unwrap_or(60);
        let start = std::time::Instant::now();
        let mut ping = None;
        while start.elapsed().as_secs() < wait {
            tokio::time::sleep(std::time::Duration::from_millis(500)).await;
            if let Ok(p) = self.bridge.call("ping", json!({}), 1500).await {
                if p.get("pid").and_then(|v| v.as_u64()) == Some(pid as u64) { ping = Some(p); break; }
            }
            if !game::alive(pid) { return Err(format!("Game.exe (pid {pid}) exited; log_tail shows why")); }
        }
        let Some(ping) = ping else {
            return Err(format!("Game.exe started (pid {pid}) but the bridge did not answer in {wait} s: is MafiaModLoader.asi installed and Ultimate ASI Loader present?"));
        };
        if !asked || a.go.as_deref().unwrap_or("").is_empty() {
            return Ok(pretty(&json!({"started": pid, "ping": ping})));
        }
        // the start's steps run in the menus' frames; then (freeride) the mission loads until the player exists
        while start.elapsed().as_secs() < wait {
            if let Ok(s) = self.call_value("start.state", Value::Null, Some(2000)).await {
                match s["state"].as_str() {
                    Some("failed") => return Err(format!("the start failed: {}; the menus are left as they are", s["error"])),
                    Some("done") => {
                        if a.go.as_deref() != Some("freeride") { return Ok(pretty(&json!({"started": pid, "start": s}))); }
                        if let Ok(p) = self.call_value("player", Value::Null, Some(2000)).await {
                            if p.get("actor").is_some() { return Ok(pretty(&json!({"started": pid, "start": s, "player": p}))); }
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

    /// Watch any function of the running game without writing a hook: who calls it, with what arguments (ECX, the
    /// stack), what memory behind them holds (strings shown as text), what it returns. Start a tap, let the game (or
    /// the owner) do the thing, then "show <id>". Works before the first frame too (during the intro videos).
    #[tool]
    async fn tap(&self, Parameters(a): Parameters<TapArgs>) -> Out {
        let v = self.bridge.call("tap", json!({"cmd": a.cmd}), 5000).await?;
        let text = v.as_str().map(String::from).unwrap_or_else(|| pretty(&v));
        if text.starts_with("error") { Err(text) } else { Ok(kb::annotate_tap(&text)) }
    }

    /// Search the game's knowledge base (every function of Game.exe and LS3DF.dll 1.2 from the full decompiles:
    /// names, classes, signatures, topics, the strings each uses; our own names from knowledge/<module>/names.csv;
    /// the matching lines of the notes in knowledge/notes). Start here for any question about the game.
    #[tool(annotations(read_only_hint = true))]
    async fn kb_search(&self, Parameters(a): Parameters<KbSearchArgs>) -> Out {
        kb::search(&a.query, a.module.as_deref(), a.limit.unwrap_or(40)).map(|v| pretty(&v))
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
    /// running game. The loader itself is copied only while the game is not running.
    #[tool]
    async fn deploy(&self, Parameters(a): Parameters<DeployArgs>) -> Out {
        let running = self.bridge.call("ping", json!({}), 1500).await.is_ok();
        let report = game::deploy(a.mods.as_deref(), running)?;
        let mut out = json!({"copied": report.copied, "skipped": report.skipped});
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
game_status first (is it running?); game_launch / game_close start and stop it. \
`call` reaches every bridge method and mod command (commands_list lists them): the core mod answers \
`game`, `player`, `humans`, `actors`, `frame`, `frames.find`. mem_read / mem_write read and write any address \
(\"game+0x1762A0\", \"ls3d+1AAE0\"). The loop for a mod: edit -> build (cmake --build --preset x86-release) -> \
deploy -> call its commands -> log_tail. After a crash: crash_list, crash_inspect, log_tail with previous. \
What a game function does, where a thing lives: kb_search, then kb_function (facts, calls, decompiled code); \
knowledge/notes/ holds the hand-written findings.";

#[tool_handler(router = self.tool_router)]
impl ServerHandler for Server {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(ServerCapabilities::builder().enable_tools().build()).with_instructions(INSTRUCTIONS)
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let service = Server::new().serve(rmcp::transport::stdio()).await?;
    service.waiting().await?;
    Ok(())
}
