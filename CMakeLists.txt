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
        host/bridge.cpp)
    # ($<0:> keeps Visual Studio from adding a per-configuration folder)
    set_target_properties(MafiaModLoader PROPERTIES SUFFIX ".asi" RUNTIME_OUTPUT_DIRECTORY ${MML_OUT}/$<0:>)
    target_include_directories(MafiaModLoader PRIVATE third_party/json host)
    target_link_libraries(MafiaModLoader PRIVATE minhook mml_sdk ws2_32 psapi user32)

    # mml_add_mod(<id> <sources...>): a native mod: <out>/mml/mods/<id>/<id>.dll and its mod.json (next to the sources)
    function(mml_add_mod id dir)
        add_library(mod_${id} SHARED ${ARGN})
        set(dest ${MML_OUT}/mml/mods/${id})
        set_target_properties(mod_${id} PROPERTIES OUTPUT_NAME ${id} RUNTIME_OUTPUT_DIRECTORY ${dest}/$<0:>)
        target_link_libraries(mod_${id} PRIVATE mml_sdk)
        add_custom_command(TARGET mod_${id} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dir}/mod.json ${dest}/mod.json)
    endfunction()

    mml_add_mod(core ${CMAKE_SOURCE_DIR}/mods/core mods/core/core.cpp)
    target_include_directories(mod_core PRIVATE third_party/json)
    mml_add_mod(hello_c ${CMAKE_SOURCE_DIR}/mods/examples/hello_c mods/examples/hello_c/hello.c)

    # ---- offline tests: a stand-in game that loads the loader and pumps its frames (tests/run_tests.py)
    add_executable(mml_harness tests/harness/harness.cpp)
    add_custom_command(TARGET MafiaModLoader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/harness
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:MafiaModLoader> ${CMAKE_BINARY_DIR}/harness/)
    set_target_properties(mml_harness PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/harness/$<0:>)
    function(mml_add_test_mod id)
        add_library(testmod_${id} SHARED tests/mods/${id}/${id}.cpp)
        set(dest ${CMAKE_BINARY_DIR}/harness/mml/mods/${id})
        set_target_properties(testmod_${id} PROPERTIES OUTPUT_NAME ${id} RUNTIME_OUTPUT_DIRECTORY ${dest}/$<0:>)
        target_link_libraries(testmod_${id} PRIVATE mml_sdk)
        add_custom_command(TARGET testmod_${id} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/tests/mods/${id}/mod.json ${dest}/mod.json)
    endfunction()
    mml_add_test_mod(hook_a)
    mml_add_test_mod(hook_b)
else()
    message(STATUS "64-bit configure: the sidecar only")
endif()
