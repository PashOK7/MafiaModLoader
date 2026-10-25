// ijoy.cpp: the sticks' dead zones (Fusion Fix's dllmain.cpp InitIJoy, on the loader: ff.h).
//
// Fusion Fix replaces each stick's FILD/TEST/FADD with a call that loads the stick's value with its dead zone taken
// out; here a hook before the FILD writes those values back into the state the FILD reads (DIJOYSTATE's axes,
// -10000..10000), so the game's own instructions load them: the same values, whole numbers.
#include "ff.h"
#include <algorithm>
#include <cmath>

namespace ff {

namespace {
constexpr float pad_max = 10000.0f; //[ -10000 0 10000 ]
float fLeftStickDeadzone, fRightStickDeadzone;

void Deadzone(int32_t* axes, float deadzone)
{
    float x = static_cast<float>(axes[0]) / pad_max;
    float y = static_cast<float>(axes[1]) / pad_max;

    auto len = sqrtf(x * x + y * y);
    if (len < deadzone)
    {
        x = 0.0f;
        y = 0.0f;
    }
    else
    {
        auto scale = std::min(1.0f, (len - deadzone) / (1.0f - deadzone)) / len;
        x *= scale * pad_max;
        y *= scale * pad_max;
    };

    axes[0] = static_cast<int32_t>(x);
    axes[1] = static_cast<int32_t>(y);
}
} // namespace

void InitIJoy()
{
    uintptr_t ijoy = H->module_base("IJoy.dll");
    fLeftStickDeadzone = std::clamp(S.leftStickDeadzone, 0.0f, 99.0f) / 100.0f;
    fRightStickDeadzone = std::clamp(S.rightStickDeadzone, 0.0f, 99.0f) / 100.0f;

    if (fLeftStickDeadzone)
    {
        auto pattern = find_module_pattern(ijoy, "DB 06 F6 C1 04 D8 85"); //0x10002F71
        if (pattern.size() == 1)
            Mid(pattern.get_first(), [](MmlMidContext& regs) { Deadzone(reinterpret_cast<int32_t*>(regs.esi), fLeftStickDeadzone); });
    }

    if (fRightStickDeadzone)
    {
        auto pattern = find_module_pattern(ijoy, "DB 45 2C F6 C1 04 D8 85");
        if (pattern.size() == 1)
            Mid(pattern.get_first(), [](MmlMidContext& regs) { Deadzone(reinterpret_cast<int32_t*>(regs.ebp + 0x2C), fRightStickDeadzone); });
    }
}

} // namespace ff
