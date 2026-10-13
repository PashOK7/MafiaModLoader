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
| `mods.list` | | every mod: id, version, loaded, generation, error |
| `mods.rescan` | | new mods in `mml\mods` loaded |
| `mod.reload` / `mod.load` / `mod.unload` | `id` (`"*"`: every loaded mod, for reload) | the mod |
| `commands.list` | | every method, the mods' with their help |
| `hooks.list` | | every hooked function and its chain |
| `menu.state` | | the open menus (the active one first: vtable, kind, command handler) and the profiles the game has read |
| `menu.command` | `command` | the command sent to the active menu as a click sends it (its vtable's `+0x24`); the new `menu.state` |
| `start.state` | | the start asked by `<game>\mml\start.json`: `waiting`, `profile`, `main`, `done`, `failed` (+ `error`) |
| `<command>` | the command's | a mod's command: the core mod's without a prefix (`player`), others' as `<mod>.<name>` |

The core mod's commands: `game`, `player`, `humans` (`radius`), `actors`, `frame` (`addr`), `frames.find`
(`name`, `limit`).
