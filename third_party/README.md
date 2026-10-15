# Third-party code

Everything here is unmodified upstream code, pinned. Bigger dependencies are not kept in the repository:
`python tools/deps.py` downloads them at pinned versions, checks their SHA-256 and unpacks them into
`third_party/_deps/` (ignored by git).

| Folder | What | Version | Source | License |
|---|---|---|---|---|
| `minhook/` | MinHook, x86/x64 API hooking | v1.3.4 (commit `c3fcafdc10146beb5919319d0683e44e3c30d537`) | https://github.com/TsudaKageyu/minhook | BSD 2-Clause (`minhook/LICENSE.txt`) |
| `json/` | nlohmann/json, single header | v3.12.0, `json.hpp` SHA-256 `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` | https://github.com/nlohmann/json/releases/tag/v3.12.0 | MIT (`json/LICENSE.MIT`) |

Fetched by `tools/deps.py` (see the table in that script for the exact URLs and hashes):

| What | Why |
|---|---|
| LuaJIT 2.1 (pinned commit) | the Lua runtime for script mods (JIT, FFI) |
| Ultimate ASI Loader (pinned release, x86) | loads `MafiaModLoader.asi` into the game (as `dinput8.dll`) |
| Jolt Physics 5.6.0 (pinned commit, MIT) | rigid bodies, joints, ragdolls for mods: built for x86 by `CMakeLists.txt` (target `Jolt`, its 32-bit SSE2 configuration, `/MT`), fetched by the first configure; `mods/examples/boxes` uses it |

The build asks `tools/deps.py` for each library it builds (`python tools/deps.py jolt`) at every configure: a library
is fetched again when its pin changes or its unpacking was not completed (`_deps/<name>/.pinned` holds the hash it was
unpacked from).
