# Agent client setup

The loader and its stdio MCP server do not depend on Claude Code. Client configuration and hooks do.
Read all of `AGENTS.md` from disk before working: an automatically injected prefix is not the whole guide.

## Codex

Codex reads `AGENTS.md`. `CLAUDE.md` contains only Claude Code's `@AGENTS.md` import; there is no separate set of
project rules there. Codex's default combined instruction budget is 32 KiB; at the 2026-10-30 audit this project's
guide alone was 42,480 bytes. The injected copy ended during the intro-video note, before the rules on editing,
UTF-8 paths, TLS, graphics startup and generated Rust bindings. An early warning now points here. Read the full
guide manually in the current chat even if a larger limit will be configured for future chats.

Codex configures MCP in `~/.codex/config.toml` or a trusted project's `.codex/config.toml`. The root `.mcp.json`
is not a substitute for this configuration. A concrete local configuration for this checkout is:

```toml
project_doc_max_bytes = 65536

[mcp_servers.mafia]
command = 'W:/github/MafiaModLoader/mcp/target/release/mml-mcp.exe'
cwd = 'W:/github/MafiaModLoader'

[mcp_servers.mafia.env]
MML_REPO = 'W:/github/MafiaModLoader'
```

Use the actual checkout's absolute paths elsewhere. This example is an alternative project configuration; the
global connection installed later on 2026-10-30 is described below. Do not add a duplicate registration.
Keep machine-specific paths local. Changing the global Codex configuration is outside this repository and needs
the owner's permission under AGENTS.md. Project configuration is loaded only for trusted projects. Restart the
client session after configuration and verify both `codex mcp list` and the tools actually available in the chat;
successful standalone calls do not prove that a running chat has connected the server.

### Connection installed on this machine (2026-10-30)

At the owner's explicit request, registered the global server using the supported CLI:

```powershell
codex mcp add mafia --env MML_REPO=W:/github/MafiaModLoader -- W:/github/MafiaModLoader/mcp/target/release/mml-mcp.exe
```

The entry in `C:/Users/PC/.codex/config.toml` is enabled. The previous configuration was backed up beside it as
`config.toml.before-mafia-20261030-171623.bak`; parsed settings were compared afterwards (apart from the new mafia
entry, previous settings are equivalent, treating omitted STDIO args as the default empty list). No project-local
config was added and the instruction size limit was not changed; continue to read AGENTS.md in full.

Verification used the saved command and environment, starting outside the repository: MCP initialization and
tools/list succeeded, with 35 tools including screenshot, input, game launch/close, memory, deploy, knowledge,
Ghidra and crash triage. game_status saw both labs stopped. This initial check was standalone; no game was launched
during registration.

After the owner restarted Codex on the same day, native Mafia tools appeared in this chat. Native game_status,
parallel game_launch for 1.2 (profile 4, pid 11520) and 1.0 (profile Run, pid 15384), and input with screenshots
all passed. Both freerides ran in background windows with police, traffic and pedestrians at zero. Accelerate
600 ms then Brake 1000 ms moved the cars about 0.8 m in each lab; both 1280x960 image blocks were received and
visually inspected in the chat. Native game_close closed those two pids, and game_status confirmed both stopped.
The default game and lab modes were not switched. Native connection and basic live-game interaction are verified;
this was not a build/regression test or a test of every advertised tool.

The repository's `.claude/settings.json` runs `tools/bash_guard.py` before Claude's Bash calls. It is a guard for a
measured Claude-specific escaping fault, not a Codex hook. Do not assume it protects Codex's PowerShell commands,
and do not transfer Bash quoting or here-documents into PowerShell. Use `apply_patch` for file contents, or
`tools/replace.py` with a spec written by the editor. Re-read and check the target before every write. The latter
checks that OLD occurs once and preserves UTF-8/BOM and line endings, but does not itself compare the current file
with a hash from the agent's earlier read or make a backup.

Official references: [MCP configuration](https://learn.chatgpt.com/docs/extend/mcp?surface=cli) and
[instruction discovery and size limits](https://learn.chatgpt.com/docs/agent-configuration/agents-md).

## Terminal fallback and its limits

The existing helper starts a fresh server for one tool call:

```powershell
python tools/mcp_call.py game_status
python tools/mcp_call.py kb_search '@build/agent-query.json'
```

The second command expects an existing UTF-8 JSON file of arguments (for example `{"query":"I3D_lensflare","limit":3}`),
written with the editor. `@file.json` is useful when a tool's arguments contain quotes or backslashes. Quote that
argument in PowerShell so it is passed as a string. The file is only an example; it is not shipped.

The helper sets `MML_REPO` to its own repository and prints text content only. It discards MCP image blocks;
successful `screenshot` calls through it are not visual inspection. For visual QA, use the native MCP image result
or inspect the screenshot file in the configured game's `mml/shots` through an image-capable tool. Do not call a
visual check passed from text alone. Dependent calls stay sequential; independent queries to the two labs may
run together. A new helper process has no persistent connection or session state from an earlier invocation.

When rebuilding a server held by a client, follow AGENTS.md's binary-renaming rule; use the helper to check the
new binary and reconnect the native client afterwards.

## Rules easy to miss during handoff

- The user's game and the labs are distinct. Use only configured folders; never close a game outside the labs.
  Reproduce a tester's report in their configured player copy, not a clean lab. Restore the original default lab
  and lab mode after changes. Drive the game through its bridge, never the user's mouse or keyboard.
- Shared loader, SDK, MCP and shipped-mod changes finish with tests in both 1.2 and 1.0. Read both builds' code
  immediately when introducing different addresses or layouts. Use knowledge/README.md and external notes before
  reversing a system anew. After a crash, start with `crash_triage`.
- An unexpected tool result is investigated in its code and the disassembly, not worked around. Ask the owner
  before fixing a faulty tool; record unresolved faults in ROADMAP Known gaps.
- A bridge method gets its documentation row in PROTOCOL.md and generated `host/methods.inc`
  (`python tools/gen_methods.py`). Header address/constant changes require `tools/check_builds.py` and
  `tools/gen_rust_game.py`; SDK layout changes also require the structures workflow in knowledge/README.md.
  A new MmlHost member requires regenerating Rust sys.rs and adding its safe wrapper by hand.
- Paths are UTF-8 and game text uses the game's language code page. Follow the existing path helpers and
  `tools/check_paths.py`; downloads use `tools/fetch.py` (Windows certificate verification).
- Hooks reachable on arbitrary Windows threads must handle threads without TLS. A dynamically initialized
  function-local static can use TLS too. Game-thread callbacks must not block. Hook detours are not covered by
  the mod callback fault guard. `MmlHost` only grows; preserve ABI compatibility.
- Enable a graphics mod before game startup: the device must be created without multisampling. Deploy preserves
  each lab's enabled flags; a build manifest alone does not turn a mod on. The host ASI is replaced with the game
  closed; mods can hot-reload. Benchmark one lab at a time even though functional checks use both labs.
- Repository text is English; conversation follows the user's language. Ask before pushing, publishing, opening
  PRs or making changes outside this repository and configured game folders. Preserve existing uncommitted work.

These are reminders, not replacements for AGENTS.md, MODDING.md, PROTOCOL.md and the knowledge notes.

## Audit evidence (2026-10-30)

- `codex-cli 0.164.0-alpha.11.1`: `codex mcp list --json` had no `mafia` entry; this chat also had no native Mafia
  tools. No project `.codex/config.toml` existed. No active Codex configuration was changed during the audit.
- `python tools/mcp_call.py game_status` succeeded: both configured labs were stopped. Default `W:/MafiaLAB`
  (1.2) was in loader mode with the pinned ASI loader and the project's dxwrapper (`python tools/lab.py status`).
- `kb_search` for `I3D_lensflare` through the helper returned names for both builds and notes/water.md. This checks
  standalone MCP and knowledge access; it does not check live game commands, screenshots or a new build.
- Python 3.10.9, CMake 4.1.0-rc2 and Cargo 1.94.0 were available. The toolchain was not rebuilt or regression-tested.
- Three old Claude worktrees existed (detached HEAD). Their presence does not authorize cleanup or reuse; inspect
  their state before touching them. They were left unchanged, along with the existing uncommitted water work.
