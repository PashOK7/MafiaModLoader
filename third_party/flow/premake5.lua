-- NVIDIA Flow's own build (third_party/_deps/flow/premake5.lua, tools/deps.py flow) run by the newest premake:
--   premake5 --file=third_party/flow/premake5.lua --flow=<flow's folder> vs2022
-- Flow's script was written for premake 5.0.0-beta2, whose `flags` and `{COPY}` premake 5.0.0 took out: each flag it
-- uses is given to the setting that replaced it, each copy to the command that replaced it, and Flow's script is
-- included as it is. Then what this build adds: MSVC
-- 14.44's C4756 (an overflow in a constant expression: Flow's INFINITY in NvFlowRayMarchUtils.h and EmitterMesh.cpp)
-- is not one of Flow's warnings-as-errors, and the shader compiler tool is C++17 (below).

newoption {
    trigger = "flow",
    value = "PATH",
    description = "NVIDIA Flow's folder (its premake5.lua)",
}

local replaced = {
    FatalCompileWarnings = function() fatalwarnings { "All" } end,
    NoPCH = function() enablepch "Off" end,
    NoIncrementalLink = function() incrementallink "Off" end,
    ExcludeFromBuild = function() excludefrombuild "On" end,
}

function flags(list)
    for _, flag in ipairs(list) do
        local f = replaced[flag]
        if not f then
            error("flags: " .. flag .. " has no replacement here (third_party/flow/premake5.lua)")
        end
        f()
    end
end

-- ({COPY}, its build commands' copy, is {COPYFILE} now: Windows' copy, which takes a path's / for a switch)
for _, api in ipairs({ "prebuildcommands", "postbuildcommands" }) do
    local set = _G[api]
    _G[api] = function(list)
        local out = {}
        for i, cmd in ipairs(list) do
            local args = cmd:match("^{COPY} (.*)$")
            out[i] = args and ("{COPYFILE} " .. args:gsub("/", "\\")) or cmd
        end
        set(out)
    end
end

if not _OPTIONS["flow"] then
    error("--flow=<NVIDIA Flow's folder> is needed")
end
include(path.getabsolute(path.join(_OPTIONS["flow"], "premake5.lua"), _WORKING_DIR))

workspace "nvflow"
    filter {}
    disablewarnings { "4756" }

-- (the newest Slang's slang.h has inline variables: C++17 for the build's own shader compiler, which includes it; its
-- slang.dll forwards to slang-compiler.dll, which loads slang-glsl-module.dll: beside the compiler too)
project "nvflowshadercompiler"
    cppdialect "C++17"
    for _, dll in ipairs({ "slang-compiler.dll", "slang-glsl-module.dll" }) do
        prebuildcommands { "{COPYFILE} ..\\..\\..\\external\\slang\\bin\\" .. dll .. " ..\\..\\..\\_build\\windows-x86_64\\%{cfg.buildcfg}" }
    end
