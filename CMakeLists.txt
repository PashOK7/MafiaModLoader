# MafiaModLoader: the in-game part (32-bit: the loader, the mods, the offline test harness).
# The 64-bit sidecar has its own configure preset (x64); the MCP server is a Cargo project (mcp/).
cmake_minimum_required(VERSION 3.24)
project(MafiaModLoader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT: players need no Visual C++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W3 /utf-8 /permissive- /Zi)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
add_link_options(/DEBUG /OPT:REF /OPT:ICF)

set(MML_OUT ${CMAKE_BINARY_DIR}/out)   # the layout of <game>: MafiaModLoader.asi, mml/mods/<id>/...

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # ---- MinHook (third_party/minhook, unmodified)
    add_library(minhook STATIC
        third_party/minhook/src/buffer.c third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c
        third_party/minhook/src/hde/hde32.c third_party/minhook/src/hde/hde64.c)
    target_include_directories(minhook PUBLIC third_party/minhook/include)
    # MikkTSpace (Morten S. Mikkelsen's reference, zlib licence): the tangents of people skinned on the GPU (host/skin.cpp)
    add_library(mikktspace STATIC third_party/mikktspace/mikktspace.c)
    target_include_directories(mikktspace PUBLIC third_party/mikktspace)

    # ---- the SDK: headers only (mml/mml.h, the game knowledge in mml/game/)
    add_library(mml_sdk INTERFACE)
    target_include_directories(mml_sdk INTERFACE sdk/include)

    # ---- the loader
    add_library(MafiaModLoader SHARED
        host/main.cpp host/paths.cpp host/log.cpp host/memory.cpp host/crash.cpp host/stack.cpp host/hooks.cpp host/mods.cpp host/game_hooks.cpp
        host/bridge.cpp host/tap.cpp host/start.cpp host/input.cpp host/screenshot.cpp host/draw.cpp host/config.cpp host/menus.cpp host/menus10.cpp host/script.cpp host/orders.cpp host/world.cpp host/lifecycle.cpp host/files.cpp host/render.cpp host/gfx.cpp host/gfx_vk.cpp host/motion.cpp host/view.cpp host/geom.cpp host/prof.cpp host/d3dstats.cpp host/events.cpp host/native.cpp host/humans.cpp host/watch.cpp host/happen.cpp host/collide.cpp host/state.cpp host/lights.cpp host/effects.cpp host/props.cpp host/sound.cpp host/control.cpp host/interact.cpp host/hud.cpp host/saves.cpp host/anims.cpp host/hits.cpp host/pedestrians.cpp host/framelife.cpp host/worlddraw.cpp host/items.cpp host/stats.cpp host/models.cpp host/meshes.cpp host/shaders.cpp host/skin.cpp host/vdata.cpp host/shadows.cpp host/panel.cpp host/fpu.cpp host/gamecam.cpp host/sidecar.cpp host/video.cpp host/video_hw.cpp)
    # ($<0:> keeps Visual Studio from adding a per-configuration folder)
    set_target_properties(MafiaModLoader PROPERTIES SUFFIX ".asi" RUNTIME_OUTPUT_DIRECTORY ${MML_OUT}/$<0:>)
    target_include_directories(MafiaModLoader PRIVATE third_party/json host ${CMAKE_BINARY_DIR}/gen)
    # the shaders' contract (sdk/include/mml/shaders/game.hlsli) inside the host, for the HLSL it compiles
    file(READ ${CMAKE_SOURCE_DIR}/sdk/include/mml/shaders/game.hlsli MML_GAME_HLSLI)
    file(READ ${CMAKE_SOURCE_DIR}/sdk/include/mml/shaders/skin.hlsli MML_SKIN_HLSLI)
    configure_file(host/gen/shaders_hlsli.inc.in ${CMAKE_BINARY_DIR}/gen/shaders_hlsli.inc @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/sdk/include/mml/shaders/game.hlsli
                 ${CMAKE_SOURCE_DIR}/sdk/include/mml/shaders/skin.hlsli)
    target_link_libraries(MafiaModLoader PRIVATE minhook mikktspace mml_sdk ws2_32 psapi user32 ole32)
    # frame pointers kept (/Oy-): the sampler (prof.cpp) walks the loader's frames by EBP as it walks the system's
    target_compile_options(MafiaModLoader PRIVATE /Oy-)

    # ---- third-party libraries for mods: pinned by tools/deps.py (downloaded at the first configure, hash checked),
    # built here for x86 with the mods' runtime (/MT). A mod links the target: target_link_libraries(mod_<id> PRIVATE Jolt)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/deps.py)   # a new pin: fetched
    function(mml_dep name)
        execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/deps.py ${name} RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "tools/deps.py ${name} failed: the library is not there")
        endif()
    endfunction()
    # the Vulkan headers for the graphics bridge on DXVK (host/gfx_vk.cpp: DXVK's own Vulkan device, its vulkan-1.dll)
    mml_dep(vulkan)
    target_include_directories(MafiaModLoader PRIVATE third_party/_deps/vulkan)
    # FFmpeg for the game's videos (host/video.cpp): its sources pinned by tools/deps.py ffmpeg, built for x86 once by
    # third_party/ffmpeg/build.py at the configure (some minutes the first time: MSVC, MSYS2, NASM, Meson), its DLLs
    # delay-loaded (the loader runs without them: the game's own players then) and put in <out>/mml/ffmpeg/ with their
    # PDBs, licences and BUILD.txt; tools/lab.py install copies them into a lab.
    option(MML_FFMPEG "the loader plays the game's videos with FFmpeg (third_party/ffmpeg/build.py)" ON)
    if(MML_FFMPEG)
        mml_dep(ffmpeg)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/build.py)
        execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/build.py RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "third_party/ffmpeg/build.py failed: FFmpeg is not built (MML_FFMPEG=OFF builds without it)")
        endif()
        set(ff ${CMAKE_SOURCE_DIR}/third_party/_deps/ffmpeg_build/out)
        file(GLOB ff_dlls ${ff}/bin/*.dll)
        file(GLOB ff_pdbs ${ff}/bin/*.pdb)
        target_include_directories(MafiaModLoader PRIVATE ${ff}/include)
        target_link_libraries(MafiaModLoader PRIVATE ${ff}/bin/avformat.lib ${ff}/bin/avcodec.lib ${ff}/bin/avutil.lib
            ${ff}/bin/swscale.lib ${ff}/bin/swresample.lib delayimp)
        foreach(dll IN LISTS ff_dlls)
            cmake_path(GET dll FILENAME name)
            target_link_options(MafiaModLoader PRIVATE /DELAYLOAD:${name})
        endforeach()
        add_custom_command(TARGET MafiaModLoader POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${MML_OUT}/mml/ffmpeg/licenses
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ff_dlls} ${ff_pdbs} ${ff}/BUILD.txt ${MML_OUT}/mml/ffmpeg/
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${ff}/licenses ${MML_OUT}/mml/ffmpeg/licenses)
    else()
        target_compile_definitions(MafiaModLoader PRIVATE MML_NO_FFMPEG)
    endif()

    # Jolt Physics (rigid bodies, joints, ragdolls): upstream's own CMake, as a part of this build. Its 32-bit
    # configuration (SSE2: any x86 CPU), without what mods do not need (the GPU compute back ends, the profiler, the
    # debug renderer, object streams). Its definitions reach a mod through the target (a mismatch is caught by
    # JPH::VerifyJoltVersionID()). Jolt is compiled without exceptions and RTTI, its own choice; a mod keeps its own.
    option(MML_JOLT "Jolt Physics for mods (third_party/_deps/jolt)" ON)
    if(MML_JOLT)
        mml_dep(jolt)
        foreach(o USE_SSE4_1 USE_SSE4_2 USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD
                  JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL INTERPROCEDURAL_OPTIMIZATION FLOATING_POINT_EXCEPTIONS_ENABLED
                  PROFILER_IN_DEBUG_AND_RELEASE DEBUG_RENDERER_IN_DEBUG_AND_RELEASE ENABLE_OBJECT_STREAM ENABLE_INSTALL
                  ENABLE_ALL_WARNINGS)
            set(${o} OFF)
        endforeach()
        set(USE_STATIC_MSVC_RUNTIME_LIBRARY ON)
        add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/_deps/jolt/Build ${CMAKE_BINARY_DIR}/jolt EXCLUDE_FROM_ALL)
        target_compile_options(Jolt PUBLIC /Zc:__cplusplus)   # (Jolt's headers read __cplusplus)
    endif()

    # ---- shaders compiled in a mod's build: fxc (shader model 5 and lower: Direct3D 9 to 11) or dxc (6 and up:
    # Direct3D 12), both from the Windows SDK this build uses
    cmake_host_system_information(RESULT kits QUERY WINDOWS_REGISTRY
        "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10 VIEW BOTH)
    if(NOT kits)
        set(kits "C:/Program Files (x86)/Windows Kits/10")
    endif()
    set(MML_SDK_BIN "${kits}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
    find_program(MML_FXC fxc HINTS ${MML_SDK_BIN} NO_DEFAULT_PATH)
    find_program(MML_FXC fxc)
    find_program(MML_DXC dxc HINTS ${MML_SDK_BIN} NO_DEFAULT_PATH)
    find_program(MML_DXC dxc)

    # mml_shaders(<target> <tag> <dir> <file>:<entry>:<profile>[:<name>] ...): each entry point compiled at build time
    # (optimised, warnings as errors) into a header `shaders/<name>.h` of the target's, its bytecode as `k<name>[]`
    # (fxc: const BYTE, so <windows.h> first; dxc: const unsigned char). <file> is relative to <dir>; the SDK's
    # sdk/include is on the include path (`#include "mml/gfx/frame.hlsli"`); a change to the file or to any .hlsli
    # beside it compiles it again.
    function(mml_shaders target tag dir)
        set(gen ${CMAKE_BINARY_DIR}/shaders/${tag})
        set(headers)
        foreach(spec ${ARGN})
            string(REPLACE ":" ";" parts "${spec}")
            list(LENGTH parts n)
            if(n LESS 3)
                message(FATAL_ERROR "${tag} SHADERS ${spec}: <file>:<entry>:<profile>[:<name>]")
            endif()
            list(GET parts 0 file)
            list(GET parts 1 entry)
            list(GET parts 2 profile)
            set(name ${entry})
            if(n GREATER 3)
                list(GET parts 3 name)
            endif()
            cmake_path(ABSOLUTE_PATH file BASE_DIRECTORY ${dir} NORMALIZE OUTPUT_VARIABLE src)
            cmake_path(GET src PARENT_PATH srcdir)
            file(GLOB includes ${srcdir}/*.hlsli ${CMAKE_SOURCE_DIR}/sdk/include/mml/gfx/*.hlsli
                 ${CMAKE_SOURCE_DIR}/sdk/include/mml/shaders/*.hlsli ${CMAKE_SOURCE_DIR}/sdk/include/mml/shaders/*.hlsl)
            set(out ${gen}/shaders/${name}.h)
            set(sdkinc ${CMAKE_SOURCE_DIR}/sdk/include)
            if(profile MATCHES "_6_")
                if(NOT MML_DXC)
                    message(FATAL_ERROR "${tag}: ${profile} needs dxc (the Windows SDK's), not found")
                endif()
                set(cmd ${MML_DXC} -nologo -O3 -WX -T ${profile} -E ${entry} -I ${srcdir} -I ${sdkinc} -Vn k${name} -Fh ${out} ${src})
            else()
                if(NOT MML_FXC)
                    message(FATAL_ERROR "${tag}: ${profile} needs fxc (the Windows SDK's), not found")
                endif()
                set(cmd ${MML_FXC} /nologo /O3 /WX /T ${profile} /E ${entry} /I ${srcdir} /I ${sdkinc} /Vn k${name} /Fh ${out} ${src})
            endif()
            add_custom_command(OUTPUT ${out} COMMAND ${CMAKE_COMMAND} -E make_directory ${gen}/shaders
                COMMAND ${cmd} DEPENDS ${src} ${includes} COMMENT "${tag}: ${file} ${entry} (${profile})" VERBATIM)
            list(APPEND headers ${out})
        endforeach()
        target_sources(${target} PRIVATE ${headers})
        target_include_directories(${target} PRIVATE ${gen})
    endfunction()

    # the loader's own shaders (host/gfx.cpp: the graphics bridge)
    mml_shaders(MafiaModLoader host ${CMAKE_SOURCE_DIR}/host/shaders
        bridge9.hlsl:VsQuad:vs_3_0 bridge9.hlsl:PsDepth:ps_3_0 bridge9.hlsl:PsComposite:ps_3_0
        bridge9.hlsl:VsMotion:vs_3_0 bridge9.hlsl:VsMotionSkin:vs_3_0 bridge9.hlsl:PsMotion:ps_3_0 bridge9.hlsl:PsMotionAll:ps_3_0
        bridge11.hlsl:VsFullScreen:vs_5_0
        skin9.hlsl:VsSkin:vs_3_0 skin9.hlsl:PsSkin:ps_3_0 skin9.hlsl:VsSkinDepth:vs_3_0 skin9.hlsl:VsSkinMotion:vs_3_0)

    # mml_add_mod(<id> <dir> <sources...> [SHADERS <file>:<entry>:<profile>[:<name>] ...]): a native mod:
    # <out>/mml/mods/<id>/<id>.dll, its mod.json and, when it has them, its config.json (the defaults of its settings)
    # and its files\ folder (game files: copied over, a file deleted from <dir> stays in <out> until it is cleaned),
    # all from <dir>. SHADERS: each entry point compiled at build time (optimised, warnings as errors) into a header
    # `shaders/<name>.h` holding the bytecode as `k<name>[]` (fxc: const BYTE, so <windows.h> first; dxc: const
    # unsigned char) (<name>: the entry's name by default);
    # <file> is relative to <dir>, and changes to it or to any .hlsli beside it compile it again.
    function(mml_add_mod id dir)
        cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "SHADERS")
        set_property(GLOBAL APPEND PROPERTY MML_MODS ${id})
        add_library(mod_${id} SHARED ${arg_UNPARSED_ARGUMENTS})
        set(dest ${MML_OUT}/mml/mods/${id})
        set_target_properties(mod_${id} PROPERTIES OUTPUT_NAME ${id} RUNTIME_OUTPUT_DIRECTORY ${dest}/$<0:>)
        target_link_libraries(mod_${id} PRIVATE mml_sdk)
        add_custom_command(TARGET mod_${id} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dir}/mod.json ${dest}/mod.json)
        if(EXISTS ${dir}/config.json)
            add_custom_command(TARGET mod_${id} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dir}/config.json ${dest}/config.json)
        endif()
        if(EXISTS ${dir}/files)   # the files it gives the game (docs/MODDING.md "A mod's own files in the game")
            add_custom_command(TARGET mod_${id} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory ${dir}/files ${dest}/files)
        endif()
        if(arg_SHADERS)
            mml_shaders(mod_${id} ${id} ${dir} ${arg_SHADERS})
        endif()
    endfunction()

    # mml_add_files_mod(<id> <dir>): a mod of files alone ("type": "files" in its mod.json, no DLL): its mod.json,
    # config.json and files\ folder copied to <out>/mml/mods/<id>/ by the target files_<id>, built with everything
    function(mml_add_files_mod id dir)
        set_property(GLOBAL APPEND PROPERTY MML_MODS ${id})
        set(dest ${MML_OUT}/mml/mods/${id})
        set(cmds COMMAND ${CMAKE_COMMAND} -E make_directory ${dest}
                 COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dir}/mod.json ${dest}/mod.json)
        if(EXISTS ${dir}/config.json)
            list(APPEND cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dir}/config.json ${dest}/config.json)
        endif()
        if(EXISTS ${dir}/files)
            list(APPEND cmds COMMAND ${CMAKE_COMMAND} -E copy_directory ${dir}/files ${dest}/files)
        endif()
        add_custom_target(files_${id} ALL ${cmds} COMMENT "${id}: a mod of files" VERBATIM)
    endfunction()

    # mml_add_rust_mod(<id> <dir> [DEST <folder>]): a native mod in Rust (docs/MODDING.md "Mods in Rust"): the crate
    # in <dir> (a cdylib named <id>, on sdk/rust/mml) built by cargo for i686-pc-windows-msvc into <build>/rust (one
    # target folder for every Rust mod: the mml crate and the dependencies built once), then <id>.dll, its .pdb,
    # mod.json, config.json and files\ copied to <out>/mml/mods/<id>/ (or DEST: a test mod's, not a mod of the build).
    # Cargo runs at every build of the target (it knows what changed). No cargo, or no i686 target installed: the
    # Rust mods are left out, saying so (`rustup target add i686-pc-windows-msvc`).
    find_program(MML_CARGO cargo HINTS $ENV{USERPROFILE}/.cargo/bin)
    find_program(MML_RUSTC rustc HINTS $ENV{USERPROFILE}/.cargo/bin)
    set(MML_RUST OFF)
    if(MML_CARGO AND MML_RUSTC)
        execute_process(COMMAND ${MML_RUSTC} --print sysroot OUTPUT_VARIABLE rust_sysroot OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(EXISTS ${rust_sysroot}/lib/rustlib/i686-pc-windows-msvc)
            set(MML_RUST ON)
        else()
            message(WARNING "Rust mods left out: no i686-pc-windows-msvc target (rustup target add i686-pc-windows-msvc)")
        endif()
    else()
        message(STATUS "Rust mods left out: no cargo")
    endif()
    function(mml_add_rust_mod id dir)
        cmake_parse_arguments(PARSE_ARGV 2 arg "" "DEST" "")
        if(NOT MML_RUST)
            return()
        endif()
        if(arg_DEST)
            set(dest ${arg_DEST})
        else()
            set_property(GLOBAL APPEND PROPERTY MML_MODS ${id})
            set(dest ${MML_OUT}/mml/mods/${id})
        endif()
        set(tdir ${CMAKE_BINARY_DIR}/rust)
        set(built ${tdir}/i686-pc-windows-msvc/$<IF:$<CONFIG:Debug>,debug,release>)
        # (the shaders' compilers this build found, for mml-build in the mod's build.rs)
        set(cmds COMMAND ${CMAKE_COMMAND} -E env MML_FXC=${MML_FXC} MML_DXC=${MML_DXC}
                     ${MML_CARGO} build --manifest-path ${dir}/Cargo.toml --target i686-pc-windows-msvc
                     --target-dir ${tdir} $<$<NOT:$<CONFIG:Debug>>:--release>
                 COMMAND ${CMAKE_COMMAND} -E make_directory ${dest}
                 COMMAND ${CMAKE_COMMAND} -E copy_if_different ${built}/${id}.dll ${built}/${id}.pdb ${dest}
                 COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dir}/mod.json ${dest}/mod.json)
        if(EXISTS ${dir}/config.json)
            list(APPEND cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dir}/config.json ${dest}/config.json)
        endif()
        if(EXISTS ${dir}/files)
            list(APPEND cmds COMMAND ${CMAKE_COMMAND} -E copy_directory ${dir}/files ${dest}/files)
        endif()
        add_custom_target(rust_${id} ALL ${cmds} WORKING_DIRECTORY ${dir} COMMENT "${id}: a mod in Rust (cargo)" VERBATIM)
    endfunction()

    mml_add_mod(core ${CMAKE_SOURCE_DIR}/mods/core mods/core/core.cpp)
    target_include_directories(mod_core PRIVATE third_party/json)
    mml_add_mod(hello_c ${CMAKE_SOURCE_DIR}/mods/examples/hello_c mods/examples/hello_c/hello.c)
    mml_add_mod(telemetry ${CMAKE_SOURCE_DIR}/mods/examples/telemetry mods/examples/telemetry/telemetry.cpp)
    mml_add_mod(fps_unlock ${CMAKE_SOURCE_DIR}/mods/examples/fps_unlock mods/examples/fps_unlock/fps_unlock.cpp)
    if(MML_JOLT)   # a heavy mod: a pinned library, its own shaders, a Direct3D 11 device
        mml_add_mod(boxes ${CMAKE_SOURCE_DIR}/mods/examples/boxes mods/examples/boxes/boxes.cpp
            SHADERS boxes.hlsl:VsBox:vs_5_0 boxes.hlsl:PsBox:ps_5_0 boxes.hlsl:CsCoverage:cs_5_0)
        target_link_libraries(mod_boxes PRIVATE Jolt d3d11)
    endif()
    mml_add_mod(world_marker ${CMAKE_SOURCE_DIR}/mods/examples/world_marker mods/examples/world_marker/world_marker.cpp)
    mml_add_mod(depth_pulse ${CMAKE_SOURCE_DIR}/mods/examples/depth_pulse mods/examples/depth_pulse/depth_pulse.cpp
        SHADERS depth_pulse.hlsl:PsPulse:ps_5_0)
    mml_add_mod(hud_panel ${CMAKE_SOURCE_DIR}/mods/examples/hud_panel mods/examples/hud_panel/hud_panel.cpp
        SHADERS hud_panel.hlsl:PsPanel:ps_5_0)
    mml_add_mod(rear_mirror ${CMAKE_SOURCE_DIR}/mods/examples/rear_mirror mods/examples/rear_mirror/rear_mirror.cpp
        SHADERS rear_mirror.hlsl:PsMirror:ps_5_0)
    mml_add_mod(river_water ${CMAKE_SOURCE_DIR}/mods/examples/river_water mods/examples/river_water/river_water.cpp
        SHADERS river_water.hlsl:VsWater:vs_5_0 river_water.hlsl:PsWater:ps_5_0)
    mml_add_mod(sun_shafts ${CMAKE_SOURCE_DIR}/mods/examples/sun_shafts mods/examples/sun_shafts/sun_shafts.cpp
        SHADERS sun_shafts.hlsl:PsMarch:ps_5_0 sun_shafts.hlsl:PsBlurX:ps_5_0 sun_shafts.hlsl:PsBlurY:ps_5_0
        sun_shafts.hlsl:PsComposite:ps_5_0)
    mml_add_files_mod(freeride_picture ${CMAKE_SOURCE_DIR}/mods/examples/freeride_picture)
    # a mod with no 32-bit DLL, its 64-bit half alone (built by the x64 preset into build/x64/out; deploy merges them)
    mml_add_files_mod(hello_side ${CMAKE_SOURCE_DIR}/mods/examples/hello_side)
    # a mod in two halves: this 32-bit one shows what its 64-bit half (the x64 preset) draws with Direct3D 12
    mml_add_mod(side_picture ${CMAKE_SOURCE_DIR}/mods/examples/side_picture mods/examples/side_picture/side_picture.cpp
        SHADERS side_picture.hlsl:PsPicture:ps_5_0)
    # a mod in two halves: this 32-bit one shows the crates its 64-bit half (the x64 preset) steps with PhysX 5
    mml_add_mod(physx_crates ${CMAKE_SOURCE_DIR}/mods/examples/physx_crates mods/examples/physx_crates/physx_crates.cpp)
    target_include_directories(mod_physx_crates PRIVATE third_party/json)
    # a mod in two halves: this 32-bit one draws the water, cloth and jelly its 64-bit half steps with PhysX's GPU part
    mml_add_mod(physx_gpu ${CMAKE_SOURCE_DIR}/mods/examples/physx_gpu mods/examples/physx_gpu/physx_gpu.cpp
        SHADERS physx_gpu.hlsl:VsMesh:vs_5_0 physx_gpu.hlsl:PsMesh:ps_5_0 physx_gpu.hlsl:CsNormals:cs_5_0
        physx_gpu.hlsl:VsSplat:vs_5_0 physx_gpu.hlsl:PsSplatDepth:ps_5_0 physx_gpu.hlsl:PsSplatThick:ps_5_0
        physx_gpu.hlsl:PsSmooth:ps_5_0 physx_gpu.hlsl:PsFluid:ps_5_0)
    target_include_directories(mod_physx_gpu PRIVATE third_party/json)
    target_link_libraries(mod_physx_gpu PRIVATE d3d11)
    # a mod in two halves: this 32-bit one hands the frame to NVIDIA Flow in its 64-bit half and shows the smoke and fire
    mml_add_mod(flow_smoke ${CMAKE_SOURCE_DIR}/mods/examples/flow_smoke mods/examples/flow_smoke/flow_smoke.cpp
        SHADERS flow_smoke.hlsl:PsPrepare:ps_5_0 flow_smoke.hlsl:PsComposite:ps_5_0)
    target_include_directories(mod_flow_smoke PRIVATE third_party/json)
    target_link_libraries(mod_flow_smoke PRIVATE d3d11)
    # a mod in two halves: this 32-bit one hands the frame to NVIDIA's DLSS 5 Neural Rendering in its 64-bit half and
    # shows DLSS-NR's picture as the frame
    mml_add_mod(dlss_nr ${CMAKE_SOURCE_DIR}/mods/examples/dlss_nr mods/examples/dlss_nr/dlss_nr.cpp
        SHADERS dlss_nr.hlsl:PsPrepare:ps_5_0 dlss_nr.hlsl:PsComposite:ps_5_0 dlss_nr.hlsl:PsDebug:ps_5_0)
    target_include_directories(mod_dlss_nr PRIVATE third_party/json)
    target_link_libraries(mod_dlss_nr PRIVATE d3d11)
    mml_add_mod(puppet ${CMAKE_SOURCE_DIR}/mods/examples/puppet mods/examples/puppet/puppet.cpp)
    mml_add_mod(tally ${CMAKE_SOURCE_DIR}/mods/examples/tally mods/examples/tally/tally.cpp)
    mml_add_mod(orbit_cam ${CMAKE_SOURCE_DIR}/mods/examples/orbit_cam mods/examples/orbit_cam/orbit_cam.cpp)
    mml_add_mod(probe ${CMAKE_SOURCE_DIR}/mods/examples/probe mods/examples/probe/probe.cpp)
    mml_add_mod(bounty ${CMAKE_SOURCE_DIR}/mods/examples/bounty mods/examples/bounty/bounty.cpp)
    mml_add_mod(dusk ${CMAKE_SOURCE_DIR}/mods/examples/dusk mods/examples/dusk/dusk.cpp)
    mml_add_mod(exhaust_fx ${CMAKE_SOURCE_DIR}/mods/examples/exhaust_fx mods/examples/exhaust_fx/exhaust_fx.cpp)
    mml_add_mod(wind ${CMAKE_SOURCE_DIR}/mods/examples/wind mods/examples/wind/wind.cpp)
    mml_add_mod(storm ${CMAKE_SOURCE_DIR}/mods/examples/storm mods/examples/storm/storm.cpp)
    mml_add_mod(props_demo ${CMAKE_SOURCE_DIR}/mods/examples/props_demo mods/examples/props_demo/props_demo.cpp)
    mml_add_mod(engine_note ${CMAKE_SOURCE_DIR}/mods/examples/engine_note mods/examples/engine_note/engine_note.cpp)
    mml_add_mod(hover ${CMAKE_SOURCE_DIR}/mods/examples/hover mods/examples/hover/hover.cpp)
    mml_add_mod(debug_views ${CMAKE_SOURCE_DIR}/mods/examples/debug_views mods/examples/debug_views/debug_views.cpp
        mods/examples/debug_views/routes.cpp mods/examples/debug_views/triggers.cpp mods/examples/debug_views/race.cpp
        mods/examples/debug_views/walls.cpp mods/examples/debug_views/collision.cpp mods/examples/debug_views/web.cpp
        mods/examples/debug_views/names.cpp
        SHADERS race.hlsl:PsRace:ps_5_0 walls.hlsl:PsWalls:ps_5_0
        collision.hlsl:VsCollision:vs_5_0 collision.hlsl:PsCollisionG:ps_5_0 collision.hlsl:PsCollision:ps_5_0)
    mml_add_rust_mod(hello_rust ${CMAKE_SOURCE_DIR}/mods/examples/hello_rust)
    mml_add_rust_mod(depth_pulse_rs ${CMAKE_SOURCE_DIR}/mods/examples/depth_pulse_rs)
    mml_add_mod(devtools ${CMAKE_SOURCE_DIR}/mods/examples/devtools mods/examples/devtools/devtools.cpp)
    # (tools/new_mod.py adds a mod's line above this one)

    # The mods this build defines: <out>/mml/build_mods.txt, one id a line (written only when it changes), which deploy
    # (mcp/src/game.rs) and tools/lab.py install copy and nothing else. CMake never deletes a removed target's
    # outputs, so a mod taken out of this file (or one whose option is off: boxes without MML_JOLT) has its old
    # folder in <out>/mml/mods removed here, at the configure that taking it out causes.
    get_property(mml_mods GLOBAL PROPERTY MML_MODS)
    list(JOIN mml_mods "\n" mml_mods_text)
    file(CONFIGURE OUTPUT ${MML_OUT}/mml/build_mods.txt CONTENT "${mml_mods_text}\n")
    file(GLOB mml_mod_outs LIST_DIRECTORIES true ${MML_OUT}/mml/mods/*)
    foreach(path IN LISTS mml_mod_outs)
        cmake_path(GET path FILENAME id)
        if(IS_DIRECTORY ${path} AND NOT id IN_LIST mml_mods)
            message(STATUS "${id}: not a mod of this build any more, its old output removed (${path})")
            file(REMOVE_RECURSE ${path})
        endif()
    endforeach()

    # ---- offline tests: a stand-in game that loads the loader and pumps its frames (tests/run_tests.py)
    add_executable(mml_harness tests/harness/harness.cpp)
    add_custom_command(TARGET MafiaModLoader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/harness
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:MafiaModLoader> $<TARGET_PDB_FILE:MafiaModLoader>
            ${CMAKE_BINARY_DIR}/harness/)   # (its PDB beside it, as deploy puts it: crash_triage names its frames)
    set_target_properties(mml_harness PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/harness/$<0:>)
    add_library(harness_rw_data SHARED tests/harness/rw_data.cpp)   # the game's file layer, stood in for
    set_target_properties(harness_rw_data PROPERTIES OUTPUT_NAME rw_data RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/harness/$<0:>)
    add_dependencies(mml_harness harness_rw_data)
    function(mml_add_test_mod id)
        add_library(testmod_${id} SHARED tests/mods/${id}/${id}.cpp)
        set(dest ${CMAKE_BINARY_DIR}/harness/mml/mods/${id})
        set_target_properties(testmod_${id} PROPERTIES OUTPUT_NAME ${id} RUNTIME_OUTPUT_DIRECTORY ${dest}/$<0:>)
        target_link_libraries(testmod_${id} PRIVATE mml_sdk)
        add_custom_command(TARGET testmod_${id} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/tests/mods/${id}/mod.json ${dest}/mod.json)
        if(EXISTS ${CMAKE_SOURCE_DIR}/tests/mods/${id}/config.json)
            add_custom_command(TARGET testmod_${id} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/tests/mods/${id}/config.json ${dest}/config.json)
        endif()
    endfunction()
    mml_add_test_mod(hook_a)
    mml_add_test_mod(hook_b)
    mml_add_test_mod(kit)
    mml_add_test_mod(heavy)
    mml_add_test_mod(files_b)   # (its files\ folder and files_a, a mod of files alone, are copied by run_tests.py)
    mml_add_test_mod(side_test) # its 32-bit half (the 64-bit one: the x64 preset; run_tests.py copies it into x64\)
    mml_add_rust_mod(kit_rs ${CMAKE_SOURCE_DIR}/tests/mods/kit_rs DEST ${CMAKE_BINARY_DIR}/harness/mml/mods/kit_rs)
    # its libraries in its folder: one it imports, one it loads by name (each load's own folder: host/lifecycle.cpp)
    foreach(lib heavylib heavyrt)
        add_library(testlib_${lib} SHARED tests/mods/heavy/${lib}.c)
        set_target_properties(testlib_${lib} PROPERTIES OUTPUT_NAME ${lib}
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/harness/mml/mods/heavy/$<0:>)
    endforeach()
    target_link_libraries(testmod_heavy PRIVATE testlib_heavylib)
    add_dependencies(testmod_heavy testlib_heavyrt)

    # ---- dxwrapper: the game's Direct3D 8 on Direct3D 9Ex (the d3d8.dll stub and dxwrapper.dll beside Game.exe, as
    # both labs run it; the graphics bridge, screenshots behind other windows and the shaders need 9Ex). Its sources
    # pinned by tools/deps.py dxwrapper, our changes by third_party/dxwrapper/patch.py (its hooks safe on threads
    # without TLS: a player's game died in one), its own Visual Studio projects built by MSBuild as an external project
    # (MakeShader first: it compiles dxwrapper's shaders). Out to <out>/dxwrapper/ with its PDB and the settings the
    # game needs (third_party/dxwrapper/dxwrapper.ini); tools/lab.py install puts it in a lab.
    # tests/dxwrapper/notls.cpp (run_tests.py) calls its hooks from a thread without TLS.
    option(MML_DXWRAPPER "dxwrapper built from its sources with our patch (tools/deps.py dxwrapper)" ON)
    if(MML_DXWRAPPER)
        include(ExternalProject)
        mml_dep(dxwrapper)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/dxwrapper/patch.py)
        execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/third_party/dxwrapper/patch.py RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "third_party/dxwrapper/patch.py failed: dxwrapper's sources are not the ones it knows")
        endif()
        set(dxw_dir ${CMAKE_SOURCE_DIR}/third_party/_deps/dxwrapper)
        # ($(SolutionDir): its projects' custom steps run MakeShader by it; forward slashes, as a native path's final
        # backslash would escape the command line's closing quote)
        set(dxw_msb ${CMAKE_VS_MSBUILD_COMMAND} -p:Configuration=Release -p:Platform=Win32 -p:SolutionDir=${dxw_dir}/
            -nologo -v:minimal)
        ExternalProject_Add(dxwrapper_build
            SOURCE_DIR ${dxw_dir}
            BUILD_IN_SOURCE ON
            CONFIGURE_COMMAND ""
            # (the pre-build step counts the build number up and writes d3d8\BuildNo.rc: patch.py writes both instead)
            BUILD_COMMAND ${dxw_msb} ${dxw_dir}/MakeShader/MakeShader.vcxproj
                COMMAND ${dxw_msb} -m -p:PreBuildEventUseInBuild=false ${dxw_dir}/dxwrapper.vcxproj
                COMMAND ${dxw_msb} ${dxw_dir}/Stub/stub.vcxproj
            INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${MML_OUT}/dxwrapper
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dxw_dir}/bin/Release/dxwrapper.dll
                    ${dxw_dir}/bin/Release/dxwrapper.pdb ${dxw_dir}/bin/Release/Build/Stub/d3d8.dll ${dxw_dir}/License.txt
                    ${CMAKE_SOURCE_DIR}/third_party/dxwrapper/dxwrapper.ini ${MML_OUT}/dxwrapper/
            BUILD_BYPRODUCTS ${dxw_dir}/bin/Release/dxwrapper.dll
            BUILD_ALWAYS ON)   # (MSBuild knows what changed: a second build is a check of a few seconds)
        # the test: dxwrapper's hooks called from a thread made without TLS (it crashed the release v1.8.8600.25)
        add_executable(dxwrapper_notls tests/dxwrapper/notls.cpp)
        set(dxw_test ${CMAKE_BINARY_DIR}/dxwrapper_test)
        set_target_properties(dxwrapper_notls PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${dxw_test}/$<0:>)
        target_link_libraries(dxwrapper_notls PRIVATE advapi32 user32)
        add_dependencies(dxwrapper_notls dxwrapper_build)
        add_custom_command(TARGET dxwrapper_notls POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MML_OUT}/dxwrapper/dxwrapper.dll ${MML_OUT}/dxwrapper/d3d8.dll
                ${MML_OUT}/dxwrapper/dxwrapper.ini ${dxw_test}/)
    endif()
else()
    # ---- 64 bits: the sidecar (mml-sidecar64.exe: the mods' 64-bit halves, sdk/include/mml/side.h) and those halves
    message(STATUS "64-bit configure: the sidecar and the mods' 64-bit halves")
    add_executable(mml_sidecar64 sidecar/main.cpp)
    set_target_properties(mml_sidecar64 PROPERTIES OUTPUT_NAME mml-sidecar64 RUNTIME_OUTPUT_DIRECTORY ${MML_OUT}/mml/sidecar/$<0:>)
    target_include_directories(mml_sidecar64 PRIVATE third_party/json sdk/include sidecar)
    target_link_libraries(mml_sidecar64 PRIVATE d3d12 dxgi)

    # A mod's 64-bit half: mml_mod64(<id> <sources...>) builds <game>/mml/mods/<id>/x64/<id>.dll (its manifest:
    # "entry64": "x64/<id>.dll"; the libraries it needs go beside it in x64/).
    function(mml_mod64 id)
        add_library(mod64_${id} SHARED ${ARGN})
        set_target_properties(mod64_${id} PROPERTIES OUTPUT_NAME ${id} RUNTIME_OUTPUT_DIRECTORY ${MML_OUT}/mml/mods/${id}/x64/$<0:>)
        target_include_directories(mod64_${id} PRIVATE sdk/include)
    endfunction()
    mml_mod64(hello_side mods/examples/hello_side/hello_side64.cpp)
    mml_mod64(side_picture mods/examples/side_picture/side_picture64.cpp)
    target_link_libraries(mod64_side_picture PRIVATE d3d12)

    # ---- PhysX 5 for the 64-bit halves: pinned by tools/deps.py (downloaded at the first configure, hash checked).
    # Its CMake makes the whole build's configurations its own (debug, checked, profile, release: CMAKE_CONFIGURATION_TYPES
    # forced), so it is an external project, configured and built in build/x64/physx by itself: static libraries, the
    # static CRT (/MT, as the halves), no snippets or PVD runtime. Its release libraries go with this build's
    # RelWithDebInfo and Release, its debug ones with Debug. A half links the target:
    # target_link_libraries(mod64_<id> PRIVATE PhysX5)
    #
    # With MML_PHYSX_GPU (the default) PhysX's GPU part is built too: particles, cloth, deformable bodies, GPU rigid
    # bodies (CUDA; a Volta GPU or newer to run). CUDA is the parts of NVIDIA's toolkit tools/deps.py pins (nothing
    # installed), given to Visual Studio as its CUDA toolset. PhysXGpu.dll (shared: PhysX loads it when a CUDA
    # context manager is made) goes beside a half that asks for it: mml_physx_gpu(<id>). The kernels are PhysX's own
    # choice: machine code for Volta to Blackwell (sm_70, 75, 80, 86, 89, 90, 100, 120; 75 is CMAKE_CUDA_ARCHITECTURES,
    # upstream's default) and PTX for later GPUs. Without it, PhysX is built for the CPU alone (DISABLE_CUDA_PHYSX).
    option(MML_PHYSX "PhysX 5 for the mods' 64-bit halves (third_party/_deps/physx)" ON)
    option(MML_PHYSX_GPU "PhysX's GPU part for the 64-bit halves (CUDA from tools/deps.py cuda)" ON)
    # (nvcc compiles each kernel for its nine architectures on every core at once: a lower number keeps the machine
    # usable while PhysX's GPU part builds: 8 minutes with 2 at once on 12 cores; empty: every project at once)
    set(MML_PHYSX_JOBS "" CACHE STRING "PhysX's own build: how many of its projects at once (cmake --build --parallel)")
    if(MML_PHYSX)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/deps.py)
        set(px_deps physx)
        if(MML_PHYSX_GPU)
            list(APPEND px_deps cuda)
        endif()
        execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/deps.py ${px_deps} RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "tools/deps.py ${px_deps} failed: the library is not there")
        endif()
        include(ExternalProject)
        set(px_libs PhysX PhysXCommon PhysXFoundation PhysXExtensions PhysXCooking PhysXPvdSDK)
        set(px_args -DPHYSX_PRESET= -DPX_GENERATE_STATIC_LIBRARIES=ON -DNV_USE_STATIC_WINCRT=ON -DPX_BUILDSNIPPETS=OFF
            -DPX_BUILDPVDRUNTIME=OFF)
        set(px_toolset)
        if(MML_PHYSX_GPU)
            set(MML_CUDA ${CMAKE_SOURCE_DIR}/third_party/_deps/cuda)
            set(px_dir ${CMAKE_BINARY_DIR}/physx-gpu)
            set(px_args ${px_args} -DPX_GENERATE_GPU_PROJECTS=ON -DPX_GENERATE_GPU_STATIC_LIBRARIES=OFF
                -DCUDAToolkit_ROOT=${MML_CUDA} -DCMAKE_CUDA_COMPILER=${MML_CUDA}/bin/nvcc.exe
                -DCMAKE_CUDA_ARCHITECTURES=75)
            set(px_toolset CMAKE_GENERATOR ${CMAKE_GENERATOR} CMAKE_GENERATOR_PLATFORM ${CMAKE_GENERATOR_PLATFORM}
                CMAKE_GENERATOR_TOOLSET cuda=${MML_CUDA})
        else()
            set(px_dir ${CMAKE_BINARY_DIR}/physx)
            set(px_args ${px_args} -DPX_GENERATE_GPU_PROJECTS=OFF)
        endif()
        # (PhysX's own layout of its output: bin/win.x86_64.vc<toolset>.mt/<its configuration>/<lib>_static_64.lib)
        set(px_out ${px_dir}/out/bin/win.x86_64.vc${MSVC_TOOLSET_VERSION}.mt)
        set(px_byproducts)
        foreach(lib ${px_libs})
            list(APPEND px_byproducts ${px_out}/release/${lib}_static_64.lib ${px_out}/debug/${lib}_static_64.lib)
        endforeach()
        set(px_targets ${px_libs})
        if(MML_PHYSX_GPU)
            list(APPEND px_targets PhysXGpu)
            list(APPEND px_byproducts ${px_out}/release/PhysXGpu.dll ${px_out}/debug/PhysXGpu.dll)
        endif()
        ExternalProject_Add(physx_build
            SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/_deps/physx
            BINARY_DIR ${px_dir}/build
            ${px_toolset}
            CMAKE_ARGS ${px_args}
                -DPX_OUTPUT_LIB_DIR=${px_dir}/out -DPX_OUTPUT_BIN_DIR=${px_dir}/out -DPX_OUTPUT_DLL_DIR=${px_dir}/out
            BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<IF:$<CONFIG:Debug>,debug,release> --parallel ${MML_PHYSX_JOBS}
                --target ${px_targets}
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS ${px_byproducts}
            BUILD_ALWAYS ON   # (its own build knows what changed: a second build is a check of a few seconds)
            EXCLUDE_FROM_ALL ON)
        add_library(PhysX5 INTERFACE)
        target_include_directories(PhysX5 INTERFACE ${CMAKE_SOURCE_DIR}/third_party/_deps/physx/include)
        # (what the libraries were compiled with: static; without the GPU part, PX_SUPPORT_GPU_PHYSX 0 in the headers)
        target_compile_definitions(PhysX5 INTERFACE PX_PHYSX_STATIC_LIB)
        if(NOT MML_PHYSX_GPU)
            target_compile_definitions(PhysX5 INTERFACE DISABLE_CUDA_PHYSX)
        endif()
        foreach(lib ${px_libs})
            target_link_libraries(PhysX5 INTERFACE
                $<IF:$<CONFIG:Debug>,${px_out}/debug/${lib}_static_64.lib,${px_out}/release/${lib}_static_64.lib>)
        endforeach()
        add_dependencies(PhysX5 physx_build)
        # mml_physx_gpu(<id>): PhysXGpu.dll copied beside the half's DLL (x64/), CUDA's headers and the driver API's
        # import library given to the half (cuda.h: its own CUDA calls, e.g. memory shared with Direct3D 12)
        function(mml_physx_gpu id)
            target_include_directories(mod64_${id} PRIVATE ${MML_CUDA}/include)
            target_link_libraries(mod64_${id} PRIVATE ${MML_CUDA}/lib/x64/cuda.lib)
            add_custom_command(TARGET mod64_${id} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${px_out}/$<IF:$<CONFIG:Debug>,debug,release>/PhysXGpu.dll $<TARGET_FILE_DIR:mod64_${id}>)
        endfunction()
        mml_mod64(physx_crates mods/examples/physx_crates/physx_crates64.cpp)
        target_link_libraries(mod64_physx_crates PRIVATE PhysX5)
        if(MML_PHYSX_GPU)
            mml_mod64(physx_gpu mods/examples/physx_gpu/physx_gpu64.cpp)
            target_link_libraries(mod64_physx_gpu PRIVATE PhysX5 d3d12)
            mml_physx_gpu(physx_gpu)
        endif()
    endif()

    # ---- NVIDIA Flow 2 for the 64-bit halves (smoke and fire on the GPU, Vulkan): pinned by tools/deps.py flow (its
    # sources with the Slang, premake and DXC releases that build them). Flow's own build is premake's Visual Studio
    # solution: an external project that runs premake through third_party/flow/premake5.lua (Flow's script under the
    # newest premake) and MSBuild on its release configuration, in Flow's folder (its tools find each other by paths
    # relative to it). The shader compiler makes Vulkan's SPIR-V (and DXIL) of its several hundred compute shaders:
    # its first build takes some minutes, a second one a few seconds. A half uses Flow through its loader
    # (NvFlowLoader.h) and CUDA (Flow's buffers to the game's shared textures): mml_flow(<id>) gives it the headers, CUDA's
    # driver API and nvflow.dll, nvflowext.dll beside it.
    include(ExternalProject)
    option(MML_FLOW "NVIDIA Flow 2 for the mods' 64-bit halves (tools/deps.py flow; CUDA from tools/deps.py cuda)" ON)
    if(MML_FLOW)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/deps.py
            ${CMAKE_SOURCE_DIR}/third_party/flow/premake5.lua ${CMAKE_SOURCE_DIR}/third_party/flow/patch.py)
        execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/deps.py flow cuda RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "tools/deps.py flow cuda failed: the library is not there")
        endif()
        # (our changes to Flow's sources: its Vulkan pipelines through a cache kept in a file; third_party/flow/patch.py)
        execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/third_party/flow/patch.py RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "third_party/flow/patch.py failed: Flow's sources are not the ones it knows")
        endif()
        set(MML_CUDA ${CMAKE_SOURCE_DIR}/third_party/_deps/cuda)
        set(flow_dir ${CMAKE_SOURCE_DIR}/third_party/_deps/flow)
        set(flow_out ${flow_dir}/_build/windows-x86_64/release)
        # (premake5.lua takes the compiler's and MSBuild's folders from these: the ones this build uses)
        get_filename_component(flow_msvc ${CMAKE_CXX_COMPILER} DIRECTORY)
        get_filename_component(flow_msvc ${flow_msvc}/../../.. ABSOLUTE)
        get_filename_component(flow_msbuild ${CMAKE_VS_MSBUILD_COMMAND} DIRECTORY)
        ExternalProject_Add(flow_build
            SOURCE_DIR ${flow_dir}
            BUILD_IN_SOURCE ON
            CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env MSVS_TOOLSET=${flow_msvc} MSBUILD_DIR=${flow_msbuild}
                ${flow_dir}/external/premake/windows-x86_64/premake5.exe --file=${CMAKE_SOURCE_DIR}/third_party/flow/premake5.lua
                --flow=${flow_dir} vs2022
            BUILD_COMMAND ${CMAKE_COMMAND} -E env MSVS_TOOLSET=${flow_msvc} MSBUILD_DIR=${flow_msbuild}
                ${CMAKE_VS_MSBUILD_COMMAND} ${flow_dir}/_compiler/vs2022/nvflow.sln -t:nvflowext -p:Configuration=release
                -p:Platform=x64 -m -v:m -nologo
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS ${flow_out}/nvflow.dll ${flow_out}/nvflowext.dll
            BUILD_ALWAYS ON   # (MSBuild knows what changed: a second build is a check of a few seconds)
            EXCLUDE_FROM_ALL ON)
        function(mml_flow id)
            target_include_directories(mod64_${id} PRIVATE ${flow_dir}/include/nvflow ${flow_dir}/include/nvflowext ${MML_CUDA}/include)
            target_link_libraries(mod64_${id} PRIVATE ${MML_CUDA}/lib/x64/cuda.lib)
            add_dependencies(mod64_${id} flow_build)
            add_custom_command(TARGET mod64_${id} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${flow_out}/nvflow.dll ${flow_out}/nvflowext.dll $<TARGET_FILE_DIR:mod64_${id}>)
        endfunction()
        mml_mod64(flow_smoke mods/examples/flow_smoke/flow_smoke64.cpp)
        target_link_libraries(mod64_flow_smoke PRIVATE d3d12)
        target_include_directories(mod64_flow_smoke PRIVATE third_party/json)
        mml_flow(flow_smoke)
    endif()

    # ---- NVIDIA NGX for the 64-bit halves (DLSS 5 Neural Rendering: dlss_nr): pinned by tools/deps.py ngx vulkan (the
    # DLSS SDK's headers and static library, /MT; the DLSSNR headers from NVIDIA's RTX Remix package; the Vulkan
    # headers: the half loads vulkan-1.dll itself). Under NVIDIA's RTX SDKs license: built against, never committed or
    # shipped. nvngx_dlssnr.dll (NVIDIA's runtime, 266 MB) is not copied by the build: the player puts it into the mod's
    # folder (docs/MODDING.md "DLSS 5 Neural Rendering"); third_party/_deps/ngx/bin/ has the pinned one.
    option(MML_NGX "NVIDIA NGX (DLSS 5 Neural Rendering) for the mods' 64-bit halves (tools/deps.py ngx vulkan)" ON)
    if(MML_NGX)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/deps.py ngx vulkan RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "tools/deps.py ngx vulkan failed: NGX's SDK is not there")
        endif()
        set(ngx_dir ${CMAKE_SOURCE_DIR}/third_party/_deps/ngx)
        mml_mod64(dlss_nr mods/examples/dlss_nr/dlss_nr64.cpp)
        target_include_directories(mod64_dlss_nr PRIVATE ${ngx_dir}/include third_party/_deps/vulkan third_party/json)
        target_link_libraries(mod64_dlss_nr PRIVATE ${ngx_dir}/lib/nvsdk_ngx_s.lib version)
    endif()
    # the offline tests' 64-bit half (tests/run_tests.py copies it and the sidecar into the stand-in game's folder)
    add_library(testmod64_side_test SHARED tests/mods/side_test/side_test64.cpp)
    set_target_properties(testmod64_side_test PROPERTIES OUTPUT_NAME side_test RUNTIME_OUTPUT_DIRECTORY ${MML_OUT}/tests/side_test/x64/$<0:>)
    target_include_directories(testmod64_side_test PRIVATE sdk/include)
endif()
