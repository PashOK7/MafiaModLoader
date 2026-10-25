# Third-party code

Everything here is upstream code, pinned, unmodified but for one change in MinHook: `src/hook.c` walks the
process's own threads with `NtGetNextThread` before it falls back to `CreateToolhelp32Snapshot`, which takes every
thread of the system at each hook enabled or disabled (some 40 ms each on a busy desktop: a mod's 31 hooks took
1.2 s; the change is marked `MafiaModLoader:` in the file). Bigger dependencies are not kept in the repository:
`python tools/deps.py` downloads them at pinned versions, checks their SHA-256 and unpacks them into
`third_party/_deps/` (ignored by git).

| Folder | What | Version | Source | License |
|---|---|---|---|---|
| `minhook/` | MinHook, x86/x64 API hooking | v1.3.4 (commit `c3fcafdc10146beb5919319d0683e44e3c30d537`) | https://github.com/TsudaKageyu/minhook | BSD 2-Clause (`minhook/LICENSE.txt`) |
| `json/` | nlohmann/json, single header | v3.12.0, `json.hpp` SHA-256 `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` | https://github.com/nlohmann/json/releases/tag/v3.12.0 | MIT (`json/LICENSE.MIT`) |

Fetched by `tools/deps.py` (see the table in that script for the exact URLs and hashes):

| What | Why |
|---|---|
| Ultimate ASI Loader (pinned release, x86) | loads `MafiaModLoader.asi` into the game (as `dinput8.dll`) |
| Jolt Physics 5.6.0 (pinned commit, MIT) | rigid bodies, joints, ragdolls for mods: built for x86 by `CMakeLists.txt` (target `Jolt`, its 32-bit SSE2 configuration, `/MT`), fetched by the first configure; `mods/examples/boxes` uses it |
| NVIDIA PhysX SDK 5.9.0 (pinned commit of `NVIDIA-Omniverse/PhysX`, BSD 3-Clause; only its `physx/` folder kept) | rigid bodies, joints, articulations, cooking for the mods' 64-bit halves (PhysX 5 exists only for 64 bits): built by the x64 preset as an external project (target `PhysX5`: static, `/MT`, CPU only), fetched by the first `cmake --preset x64`; `mods/examples/physx_crates` uses it. A mod that ships it carries PhysX's license (`third_party/_deps/physx/LICENSE.md`) |
| FFmpeg 9.0.2 (ffmpeg.org's release, its PGP signature checked; LGPL 2.1 or later as built) and dav1d 1.5.4 (VideoLAN, BSD 2-Clause) | the game's videos played by the loader (`host/video.cpp`): built for x86 from their sources by `third_party/ffmpeg/build.py` (MSVC, MSYS2's make, NASM, Meson; only the decoders, demuxers and parsers a video needs, no GPL parts), at the first configure; the DLLs go to `mml/ffmpeg/` with their licences and `BUILD.txt` (the sources and the configure line), delay-loaded |

The build asks `tools/deps.py` for each library it builds (`python tools/deps.py jolt`, `physx`) at every configure: a library
is fetched again when its pin changes or its unpacking was not completed (`_deps/<name>/.pinned` holds the hash it was
unpacked from).
