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

    # ---- the SDK: headers only (mml/mml.h, the game knowledge in mml/game/)
    add_library(mml_sdk INTERFACE)
    target_include_directories(mml_sdk INTERFACE sdk/include)

    # ---- the loader
    add_library(MafiaModLoader SHARED
        host/main.cpp host/log.cpp host/memory.cpp host/crash.cpp host/hooks.cpp host/mods.cpp host/game_hooks.cpp
        host/bridge.cpp host/tap.cpp host/start.cpp host/input.cpp host/screenshot.cpp host/draw.cpp host/config.cpp host/menus.cpp host/menus10.cpp host/script.cpp host/world.cpp host/lifecycle.cpp host/files.cpp host/render.cpp host/gfx.cpp host/view.cpp host/geom.cpp host/prof.cpp host/events.cpp host/native.cpp host/humans.cpp host/watch.cpp host/happen.cpp host/collide.cpp host/state.cpp host/lights.cpp host/effects.cpp)
    # ($<0:> keeps Visual Studio from adding a per-configuration folder)
    set_target_properties(MafiaModLoader PROPERTIES SUFFIX ".asi" RUNTIME_OUTPUT_DIRECTORY ${MML_OUT}/$<0:>)
    target_include_directories(MafiaModLoader PRIVATE third_party/json host)
    target_link_libraries(MafiaModLoader PRIVATE minhook mml_sdk ws2_32 psapi user32)

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
            file(GLOB includes ${srcdir}/*.hlsli ${CMAKE_SOURCE_DIR}/sdk/include/mml/gfx/*.hlsli)
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
        bridge11.hlsl:VsFullScreen:vs_5_0)

    # mml_add_mod(<id> <dir> <sources...> [SHADERS <file>:<entry>:<profile>[:<name>] ...]): a native mod:
    # <out>/mml/mods/<id>/<id>.dll, its mod.json and, when it has them, its config.json (the defaults of its settings)
    # and its files\ folder (game files: copied over, a file deleted from <dir> stays in <out> until it is cleaned),
    # all from <dir>. SHADERS: each entry point compiled at build time (optimised, warnings as errors) into a header
    # `shaders/<name>.h` holding the bytecode as `k<name>[]` (fxc: const BYTE, so <windows.h> first; dxc: const
    # unsigned char) (<name>: the entry's name by default);
    # <file> is relative to <dir>, and changes to it or to any .hlsli beside it compile it again.
    function(mml_add_mod id dir)
        cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "SHADERS")
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
    mml_add_files_mod(freeride_picture ${CMAKE_SOURCE_DIR}/mods/examples/freeride_picture)
    mml_add_mod(puppet ${CMAKE_SOURCE_DIR}/mods/examples/puppet mods/examples/puppet/puppet.cpp)
    mml_add_mod(tally ${CMAKE_SOURCE_DIR}/mods/examples/tally mods/examples/tally/tally.cpp)
    mml_add_mod(orbit_cam ${CMAKE_SOURCE_DIR}/mods/examples/orbit_cam mods/examples/orbit_cam/orbit_cam.cpp)
    mml_add_mod(probe ${CMAKE_SOURCE_DIR}/mods/examples/probe mods/examples/probe/probe.cpp)
    mml_add_mod(bounty ${CMAKE_SOURCE_DIR}/mods/examples/bounty mods/examples/bounty/bounty.cpp)
    mml_add_mod(dusk ${CMAKE_SOURCE_DIR}/mods/examples/dusk mods/examples/dusk/dusk.cpp)
    mml_add_mod(exhaust_fx ${CMAKE_SOURCE_DIR}/mods/examples/exhaust_fx mods/examples/exhaust_fx/exhaust_fx.cpp)
    mml_add_mod(wind ${CMAKE_SOURCE_DIR}/mods/examples/wind mods/examples/wind/wind.cpp)
    # (tools/new_mod.py adds a mod's line above this one)

    # ---- offline tests: a stand-in game that loads the loader and pumps its frames (tests/run_tests.py)
    add_executable(mml_harness tests/harness/harness.cpp)
    add_custom_command(TARGET MafiaModLoader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/harness
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:MafiaModLoader> ${CMAKE_BINARY_DIR}/harness/)
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
    # its libraries in its folder: one it imports, one it loads by name (each load's own folder: host/lifecycle.cpp)
    foreach(lib heavylib heavyrt)
        add_library(testlib_${lib} SHARED tests/mods/heavy/${lib}.c)
        set_target_properties(testlib_${lib} PROPERTIES OUTPUT_NAME ${lib}
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/harness/mml/mods/heavy/$<0:>)
    endforeach()
    target_link_libraries(testmod_heavy PRIVATE testlib_heavylib)
    add_dependencies(testmod_heavy testlib_heavyrt)
else()
    message(STATUS "64-bit configure: the sidecar only")
endif()
