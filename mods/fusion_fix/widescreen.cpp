// widescreen.cpp: the resolution, the HUD, the map, the texts, the menus and the draw distance at the screen's
// aspect (Fusion Fix's WidescreenFix.ixx, HUD.ixx, Menu.ixx, DrawDistance.ixx, on the loader: ff.h).
#include "ff.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>

namespace ff {

// WidescreenFix.ixx (based on behar's original Mafia widescreen fix). Fusion Fix reads the resolution with a hook in
// InitSystem, before the game first uses it; the loader's mods load after InitSystem, so it is read here at once,
// from the same globals (the pattern's operands). The parts it sets up (Apply in fusion_fix.cpp) follow.
bool InitWidescreenFix()
{
    auto pattern = find_pattern("A1 ? ? ? ? 8B 3D ? ? ? ? 52 89 4C 24 30");
    nGameVersion = MAFIA_1_0_ENG;
    if (pattern.empty())
    {
        pattern = find_pattern("A1 ? ? ? ? 8B 0D ? ? ? ? 50 89 54 24 24");
        nGameVersion = MAFIA_1_1_ENG;
    }
    if (pattern.size() != 1) return false;
    if (nGameVersion == MAFIA_1_0_ENG)
    {
        pResolutionWidth = *pattern.get_first<int32_t*>(1);
        pResolutionHeight = pResolutionWidth + 1;
    }
    else
    {
        pResolutionHeight = *pattern.get_first<int32_t*>(1);
        pResolutionWidth = *pattern.get_first<int32_t*>(7);
        auto pHudCreditsText2X6 = find_pattern("68 00 00 C3 43 52 B9 ? ? ? ? E8 ? ? ? ?");
        if (pHudCreditsText2X6.size() == 1) nGameVersion = MAFIA_1_2_ENG;
    }
    if (*pResolutionWidth <= 0 || *pResolutionHeight <= 0) return false;
    if (auto hModule = Ls3dBase())
        ImportReplace(hModule, "USER32.DLL", "LoadCursorA", reinterpret_cast<void*>(LoadCursorHook), nullptr);

    if (nGameVersion != MAFIA_1_0_ENG)
    {
        auto pMatroxWidth = find_pattern("75 ? B9 01 00 00 00 89 0D ? ? ? ? D9 44 24 10");
        auto pMatroxHeight = find_pattern("75 ? B9 02 00 00 00 89 0D ? ? ? ? A1 ? ? ? ?");
        auto pMatroxCamera = find_pattern("7E ? A1 ? ? ? ? 89 7C 24 14 50 8B 10 FF 52 70");
        if (pMatroxWidth.size() == 1 && pMatroxHeight.size() == 1 && pMatroxCamera.size() == 1)
        {
            WriteMemory(pMatroxWidth.get_first(), uint8_t{0xEB});
            WriteMemory(pMatroxHeight.get_first(), uint8_t{0xEB});
            WriteMemory(pMatroxCamera.get_first(), uint8_t{0xEB});
        }
    }
    Screen.fWidth = static_cast<float>(*pResolutionWidth);
    Screen.fHeight = static_cast<float>(*pResolutionHeight);
    Screen.fAspectRatio = Screen.fWidth / Screen.fHeight;
    Screen.fBaseWidth = 600.0f * Screen.fAspectRatio;
    Screen.fInvBaseWidth = 1.0f / Screen.fBaseWidth;
    fMenuOffset = (Screen.fWidth - Screen.fHeight * (4.0f / 3.0f)) * 0.5f;
    fMovieAspect = Screen.fAspectRatio / (4.0f / 3.0f);
    return true;
}

void InitHUD()
{
    auto pOnePCurBaseWidth = find_pattern("D8 0D ? ? ? ? D9 5C 24 18 8B 4C 24 18 89 8D C0 90 00 00",
        "D8 0D ? ? ? ? D9 5C 24 20 8B 4C 24 20 89 8E C0 40 00 00 A1 ? ? ? ? 50 8B 08 FF 51 74 89 44 24 20 A1 ? ? ? ? DB 44 24 20 8B 10 50 D8 0D ? ? ? ? D9 5C 24 14 FF 52 70 89 44 24 20 DB 44 24 20 D8 0D ? ? ? ? D9 5C 24 20 E9 ? ? ? ?");
    auto pOnePCurBaseWidth2 = find_pattern("D8 0D ? ? ? ? 8B 44 24 10 8D 9D E4 91 00 00",
        "D8 0D ? ? ? ? D9 5C 24 20 E9 ? ? ? ? FF 51 74");
    auto pHudSpeedometerX = find_pattern("D8 0D ? ? ? ? D9 43 04 D8 0D ? ? ? ? D9 44 24 20 D8 E2 89 44 24 1C",
        "D8 0D ? ? ? ? D9 43 04 D8 0D ? ? ? ? D9 05 ? ? ? ? D8 E2 D9 5C 24 20 D9 05 ? ? ? ? D8 E1 8B 44 24 20");
    auto pHudTachometerX = find_pattern("D8 0D ? ? ? ? D9 43 04 D8 0D ? ? ? ? D9 44 24 20 D8 E2 D9 5C 24 18",
        "D8 0D ? ? ? ? D9 43 04 D8 0D ? ? ? ? D9 05 ? ? ? ? D8 E2 D9 5C 24 20 D9 05 ? ? ? ? D8 E1 8B 54 24 20");
    auto pHudSpeedLimiterX = find_pattern("C7 44 24 30 00 00 3E 44 C7 44 24 34 00 00 BE 43",
        "C7 44 24 30 00 00 20 C2 C7 44 24 34 00 00 5C C3");
    auto pHudTimerX = find_pattern("C7 44 24 18 00 80 2D 44 8D 4C 24 28 53 51 8D 4C 24 20",
        "C7 44 24 20 00 00 D4 C2 C7 44 24 24 00 00 A0 41");
    auto pHudCompasX = find_pattern("D8 0D ? ? ? ? 83 CE FF C7 84 24 84 00 00 00 00 00 00 00",
        "D8 0D ? ? ? ? D9 85 C4 40 00 00 D8 0D ? ? ? ? D9 C9 D8 05 ? ? ? ? 83 CE FF");
    auto pHudMinimapX = find_pattern("D8 0D ? ? ? ? D9 54 24 18 D9 85 C4 90 00 00",
        "D8 0D ? ? ? ? D9 85 C4 40 00 00 D8 0D ? ? ? ? D9 C9 D8 05 ? ? ? ? D9 5C 24 20");
    auto pHudTransmissionX = find_pattern("D8 0D ? ? ? ? D9 85 C4 90 00 00 D8 0D ? ? ? ? 8A 85 FC 91 00 00",
        "D8 0D ? ? ? ? 8A 85 04 42 00 00 C7 84 24 E8 00 00 00 00 00 80 3F");
    auto pHudFuelLedX = find_pattern("D8 0D ? ? ? ? D9 85 C4 90 00 00 D8 0D ? ? ? ? C7 84 24 84 00 00 00 00 00 00 00",
        "D8 0D ? ? ? ? C7 84 24 94 00 00 00 00 00 00 00 C7 84 24 98 00 00 00 00 00 80 3F 89 9C 24 9C 00 00 00");
    auto pHudActionBarX = find_pattern("D8 0D ? ? ? ? 8D 0C 76 89 7C 24 44 C1 E0 18",
        "D8 0D ? ? ? ? 8D 0C 76 89 7C 24 40 C1 E0 18");
    auto pHudHealthBarX = find_pattern("D8 0D ? ? ? ? 2B DA BA 00 00 80 3F 89 5C 24 10");
    auto pHudHealthBarTextX = find_pattern("D8 0D ? ? ? ? 8D 04 90 BA 3C 02 00 00 03 C0",
        "D8 0D ? ? ? ? 8D 14 40 57 33 FF D8 05 ? ? ? ?");
    auto pHudAmmoBarX = find_pattern("D8 0D ? ? ? ? D9 81 C4 90 00 00 D8 0D ? ? ? ? 8B 54 24 08",
        "D8 0D ? ? ? ? 8B 54 24 08 8B 44 24 04 55 56");
    auto pHudAmmoBarTextX = find_pattern("D8 0D ? ? ? ? D9 81 C4 90 00 00 D8 0D ? ? ? ? 53",
        "D8 0D ? ? ? ? 53 55 56 8B 74 24 24 D8 05 ? ? ? ?");
    auto pHudSearchBarX = find_pattern("D8 0D ? ? ? ? 6A 01 C7 84 24 88 00 00 00 00 00 00 00",
        "D8 0D ? ? ? ? 6A 01 C7 84 24 98 00 00 00 00 00 00 00");
    auto pHudProgressBarX = find_pattern("F6 C4 10 0F 84 ? ? ? ? D9 85 C0 90 00 00 D8 0D ? ? ? ?",
        "F6 C4 10 0F 84 ? ? ? ? D9 85 C0 40 00 00 D8 0D ? ? ? ?");
    auto pHudProgressBar2X = find_pattern("F7 85 A4 90 00 00 00 00 04 00 0F 84 ? ? ? ?",
        "F7 85 A4 40 00 00 00 00 04 00 0F 84 ? ? ? ?");
    auto pHudLeftPopupTextX = find_pattern("D8 0D ? ? ? ? 68 00 00 20 41 50 51 8D 8C 2E 74 92 00 00",
        "D8 0D ? ? ? ? 68 00 00 20 41 50 51 8D 8C 2E 7C 42 00 00");
    auto pHudMoneyRightTextX = find_pattern("D8 0D ? ? ? ? D9 1C 24 50 E8 ? ? ? ? DD D8 8B 9D A4 90 00 00",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 50 E8 ? ? ? ? DD D8 8B 9D A4 40 00 00");
    auto pHudRace321GoX = find_pattern("D8 0D ? ? ? ? D9 85 C4 90 00 00 D8 0D ? ? ? ? D9 5C 24 44 D9 44 24 4C DC 0D ? ? ? ? DE E9 74 ? D9 05 ? ? ? ? EB ? D9 44 24 10",
        "68 00 00 C8 43 8D 8C 24 90 00 00 00 E8 ? ? ? ? 8B C8 E8 ? ? ? ? 50");
    auto pHudRace321GoX2 = find_pattern("D8 0D ? ? ? ? D9 85 C4 90 00 00 D8 0D ? ? ? ? D9 5C 24 44 D9 44 24 4C DC 0D ? ? ? ? DE E9 74 ? D9 05 ? ? ? ? EB ? D9 05 ? ? ? ?",
        "68 00 00 C8 43 8D 8C 24 90 00 00 00 E8 ? ? ? ? 8B C8 E8 ? ? ? ? 8D 4C 24 48");
    auto pHudRaceWrongWayX = find_pattern("68 00 00 88 43 8D 8C 24 80 00 00 00 E8 ? ? ? ?",
        "68 00 00 88 43 8D 8C 24 90 00 00 00 E8 ? ? ? ?");
    auto pHudRaceCheckpointX = find_pattern("C7 44 24 48 00 00 C3 43 C7 44 24 4C 00 00 16 43",
        "68 00 00 C3 43 8D 8C 24 90 00 00 00 E8 ? ? ? ?");
    auto pHudRaceRightTextX = find_pattern("D8 0D ? ? ? ? 6A 00 6A 01 6A 04 6A FF 8D 95 CC 94 00 00",
        "D8 0D ? ? ? ? 6A 00 6A 01 6A 04 6A FF D8 05 ? ? ? ?");
    auto pHudMapTextX = find_pattern("D8 0D ? ? ? ? D9 1C 24 52 E8 ? ? ? ? DD D8 8A 85 A4 90 00 00",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 52 E8 ? ? ? ? DD D8 8A 85 A4 40 00 00");
    auto pHudBlackCentredTextX = find_pattern("D8 2D ? ? ? ? F7 EE D9 5C 24 14 D9 44 24 1C");
    auto pHudCreditsText2X = find_pattern("D8 0D ? ? ? ? D9 1C 24 50 E8 ? ? ? ? DD D8 5E",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 50 E8 ? ? ? ? DD D8 5E");
    auto pHudCreditsText2X2 = find_pattern("D8 0D ? ? ? ? D9 1C 24 51 B9 ? ? ? ? E8 ? ? ? ? DD D8 D9 05 ? ? ? ?",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 51 B9 ? ? ? ? E8 ? ? ? ? DD D8 D9 05 ? ? ? ? D8 0D ? ? ? ? 6A 00 6A 00 6A 02 6A FF 83 EC 10 83 C6 0C D9 5C 24 0C D9 05 ? ? ? ?");
    auto pHudCreditsText2X3 = find_pattern("D8 0D ? ? ? ? E9 ? ? ? ? D9 05 ? ? ? ? D8 0D ? ? ? ? 6A 00 6A 01");
    auto pHudCreditsText1X = find_pattern("D8 0D ? ? ? ? D9 1C 24 52 E8 ? ? ? ? DD D8 D9 05 ? ? ? ?",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 52 E8 ? ? ? ? DD D8 D9 05 ? ? ? ?");
    auto pHudCreditsText3X = find_pattern("D8 0D ? ? ? ? D9 5C 24 08 D9 05 ? ? ? ? D8 4C 24 2C D9 5C 24 04 D9 05 ? ? ? ? D8 0D ? ? ? ? D9 1C 24 50 E8 ? ? ? ? DD D8 D9 05 ? ? ? ? D8 0D ? ? ? ? 6A 00 6A 00 6A 02",
        "D8 0D ? ? ? ? D9 5C 24 08 D9 05 ? ? ? ? D8 4C 24 2C D9 5C 24 04 D9 05 ? ? ? ? D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 50 E8 ? ? ? ? DD D8 D9 05 ? ? ? ? D8 0D ? ? ? ? 6A 00 6A 00 6A 02 6A FF 51 D9 5C 24 18 D9 05 ? ? ? ? D8 0D ? ? ? ? 8B 4C 24 18 D9 1C 24 D9 05 ? ? ? ?",
        "D8 0D ? ? ? ? D9 5C 24 08 D9 05 ? ? ? ? D8 4C 24 2C D9 5C 24 04 D9 05 ? ? ? ? D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 50 E8 ? ? ? ? DD D8 D9 05 ? ? ? ? D8 0D ? ? ? ? 6A 00 6A 00 6A 02");
    auto pHudCreditsText3X2 = find_pattern("D8 0D ? ? ? ? 6A 00 6A 00 6A 02 6A FF 51 83 C6 4C",
        "D8 0D ? ? ? ? 6A 00 6A 00 6A 02 6A FF 51 D9 5C 24 18");
    auto pHudCreditsText2X4 = find_pattern("D8 0D ? ? ? ? D9 1C 24 51 B9 ? ? ? ? E8 ? ? ? ? DD D8 5E",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 52 E8 ? ? ? ? DD D8 5E");
    auto pHudCreditsText2X5 = find_pattern("68 00 00 C3 43 56 B9 ? ? ? ? E8 ? ? ? ?");
    auto pHudCreditsText1X2 = find_pattern("D8 0D ? ? ? ? D9 1C 24 50 E8 ? ? ? ? DD D8 D9 05 ? ? ? ? D8 0D ? ? ? ? 6A 00 6A 00 6A 00",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 51 B9 ? ? ? ? E8 ? ? ? ? DD D8 D9 05 ? ? ? ? D8 0D ? ? ? ? 6A 00 6A 00 6A 00",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 50 E8 ? ? ? ? DD D8 D9 05 ? ? ? ? D8 0D ? ? ? ? 6A 00 6A 00 6A 00");
    auto pHudCreditsText4X = find_pattern("D8 0D ? ? ? ? D9 1C 24 56 B9 ? ? ? ? E8 ? ? ? ?",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? B9 ? ? ? ?");
    auto pHudLiveBarX = find_pattern("D8 0D ? ? ? ? C7 84 24 84 00 00 00 00 00 00 00 C7 84 24 88 00 00 00 00 00 80 3F 89 9C 24 8C 00 00 00 C7 84 24 94 00 00 00 00 00 00 00 C7 84 24 98 00 00 00 00 00 40 3E");
    auto pHudCreditsText2X6 = find_pattern("68 00 00 C3 43 52 B9 ? ? ? ? E8 ? ? ? ?");

    if (pOnePCurBaseWidth.size() != 1 ||
        pOnePCurBaseWidth2.size() != 1 ||
        pHudSpeedometerX.size() != 1 ||
        pHudTachometerX.size() != 1 ||
        pHudSpeedLimiterX.size() != 1 ||
        pHudTimerX.size() != 1 ||
        pHudCompasX.size() != 1 ||
        pHudMinimapX.size() != 1 ||
        pHudTransmissionX.size() != 1 ||
        pHudFuelLedX.size() != 1 ||
        pHudActionBarX.size() != 1 ||
        pHudHealthBarX.size() != 1 ||
        pHudHealthBarTextX.size() != 1 ||
        pHudAmmoBarX.size() != 1 ||
        pHudAmmoBarTextX.size() != 1 ||
        pHudSearchBarX.size() != 1 ||
        pHudProgressBarX.size() != 1 ||
        pHudProgressBar2X.size() != 1 ||
        pHudLeftPopupTextX.size() != 1 ||
        pHudMoneyRightTextX.size() != 1 ||
        pHudRace321GoX.size() != 1 ||
        pHudRace321GoX2.size() != 1 ||
        pHudRaceWrongWayX.size() != 1 ||
        pHudRaceCheckpointX.size() != 1 ||
        pHudRaceRightTextX.size() != 1 ||
        pHudMapTextX.size() != 1 ||
        pHudBlackCentredTextX.size() != 1 ||
        pHudCreditsText2X.size() != 1 ||
        pHudCreditsText2X2.size() != 1 ||
        pHudCreditsText2X3.size() != 1 ||
        pHudCreditsText1X.size() != 1 ||
        pHudCreditsText3X.size() != 1 ||
        pHudCreditsText3X2.size() != 1 ||
        pHudCreditsText2X4.size() != 1 ||
        pHudCreditsText2X5.size() != 1 ||
        pHudCreditsText1X2.size() != 1 ||
        pHudCreditsText4X.size() != 1)
        return;
    if (nGameVersion == MAFIA_1_0_ENG && (pHudLiveBarX.size() != 1)) return;

    WriteMemory(pOnePCurBaseWidth.get_first(0x2), &Screen.fInvBaseWidth);
    WriteMemory(pOnePCurBaseWidth2.get_first(0x2), &Screen.fInvBaseWidth);

    float fixHudLeft;
    float fixHudCenter;
    float fixHudRight;
    if (!S.widescreenHud)
    {
        fixHudLeft = (Screen.fBaseWidth - 800.0f) / 2.0f;
        fixHudCenter = (Screen.fBaseWidth - 800.0f) / 2.0f;
        fixHudRight = (Screen.fBaseWidth - 800.0f) / 2.0f;
    }
    else
    {
        fixHudLeft = 0.0f;
        fixHudCenter = (Screen.fBaseWidth - 800.0f) / 2.0f;
        fixHudRight = Screen.fBaseWidth - 800.0f;
    }

    static float hudSpeedometerX; hudSpeedometerX = 189.0f + fixHudLeft;
    WriteMemory(pHudSpeedometerX.get_first(0x2), &hudSpeedometerX);
    static float hudTachometerX; hudTachometerX = 259.0f + fixHudLeft;
    WriteMemory(pHudTachometerX.get_first(0x2), &hudTachometerX);
    float hudSpeedLimiterX;
    if (nGameVersion == MAFIA_1_0_ENG)
    {
        hudSpeedLimiterX = 760.0f + fixHudRight;
    }
    else
    {
        hudSpeedLimiterX = -40.0f - fixHudLeft;
    }
    WriteMemory(pHudSpeedLimiterX.get_first(0x4), hudSpeedLimiterX);
    float hudTimerX;
    if (nGameVersion == MAFIA_1_0_ENG)
    {
        hudTimerX = 694.0f + fixHudRight;
    }
    else
    {
        hudTimerX = -106.0f - fixHudLeft;
    }
    WriteMemory(pHudTimerX.get_first(0x4), hudTimerX);
    static float hudCompasX; hudCompasX = 0.0f + fixHudLeft;
    WriteMemory(pHudCompasX.get_first(0x2), &hudCompasX);
    static float hudMinimapX; hudMinimapX = 29.0f + fixHudLeft;
    WriteMemory(pHudMinimapX.get_first(0x2), &hudMinimapX);
    static float hudTransmissionX;
    if (nGameVersion == MAFIA_1_0_ENG)
    {
        hudTransmissionX = 758.0f + fixHudRight;
    }
    else
    {
        hudTransmissionX = 42.0f + fixHudLeft;
    }
    WriteMemory(pHudTransmissionX.get_first(0x2), &hudTransmissionX);
    static float hudFuelLedX;
    if (nGameVersion == MAFIA_1_0_ENG)
    {
        hudFuelLedX = 668.0f + fixHudRight;
    }
    else
    {
        hudFuelLedX = 132.0f + fixHudLeft;
    }
    WriteMemory(pHudFuelLedX.get_first(0x2), &hudFuelLedX);
    static float hudActionBarX; hudActionBarX = 9.0f + fixHudLeft;
    WriteMemory(pHudActionBarX.get_first(0x2), &hudActionBarX);
    static float hudHealthBarX; hudHealthBarX = 7.0f + fixHudLeft;
    WriteMemory(pHudHealthBarX.get_first(0x2), &hudHealthBarX);
    static float hudHealthBarTextX; hudHealthBarTextX = 42.0f + fixHudLeft;
    WriteMemory(pHudHealthBarTextX.get_first(0x2), &hudHealthBarTextX);
    static float hudAmmoBarX; hudAmmoBarX = 62.0f + fixHudLeft;
    WriteMemory(pHudAmmoBarX.get_first(0x2), &hudAmmoBarX);
    static float hudAmmoBarTextX; hudAmmoBarTextX = 112.0f + fixHudLeft;
    WriteMemory(pHudAmmoBarTextX.get_first(0x2), &hudAmmoBarTextX);
    static float hudSearchBarX; hudSearchBarX = 336.0f + fixHudCenter;
    WriteMemory(pHudSearchBarX.get_first(0x2), &hudSearchBarX);
    static float hudLiveBarX; hudLiveBarX = 368.0f + fixHudCenter;
    if (nGameVersion == MAFIA_1_0_ENG)
    {
        WriteMemory(pHudLiveBarX.get_first(0x2), &hudLiveBarX);
    }
    static float hudProgressBarX; hudProgressBarX = 250.0f + fixHudCenter;
    WriteMemory(pHudProgressBarX.get_first(0x11), &hudProgressBarX);
    static float hudProgressBar2X; hudProgressBar2X = 350.0f + fixHudCenter;
    WriteMemory(pHudProgressBar2X.get_first(0x18), &hudProgressBar2X);
    static float hudLeftPopupTextX; hudLeftPopupTextX = 9.0f + fixHudLeft;
    WriteMemory(pHudLeftPopupTextX.get_first(0x2), &hudLeftPopupTextX);
    static float hudMoneyRightTextX; hudMoneyRightTextX = 760.0f + fixHudRight;
    WriteMemory(pHudMoneyRightTextX.get_first(0x2), &hudMoneyRightTextX);

    static float hudRace321goX; hudRace321goX = 400.0f + fixHudCenter;
    if (nGameVersion == MAFIA_1_0_ENG)
    {
        WriteMemory(pHudRace321GoX.get_first(0x2), &hudRace321goX);
        WriteMemory(pHudRace321GoX2.get_first(0x2), &hudRace321goX);
    }
    else
    {
        WriteMemory(pHudRace321GoX.get_first(0x1), hudRace321goX);
        WriteMemory(pHudRace321GoX2.get_first(0x1), hudRace321goX);
    }
    float hudRaceWrongWayX = 272.0f + fixHudCenter;
    WriteMemory(pHudRaceWrongWayX.get_first(0x1), hudRaceWrongWayX);
    float hudRaceCheckpointX = 390.0f + fixHudCenter;
    if (nGameVersion == MAFIA_1_0_ENG)
    {
        WriteMemory(pHudRaceCheckpointX.get_first(0x4), hudRaceCheckpointX);
    }
    else
    {
        WriteMemory(pHudRaceCheckpointX.get_first(0x1), hudRaceCheckpointX);
    }
    static float hudRaceRightTextX; hudRaceRightTextX = 780.0f + fixHudRight;
    WriteMemory(pHudRaceRightTextX.get_first(0x2), &hudRaceRightTextX);

    static float hudMapTextX; hudMapTextX = 350.0f + fixHudCenter;
    WriteMemory(pHudMapTextX.get_first(0x2), &hudMapTextX);

    static float hudBlackCentredTextX; hudBlackCentredTextX = 400.0f + fixHudCenter;
    WriteMemory(pHudBlackCentredTextX.get_first(0x2), &hudBlackCentredTextX);
    static float hudCreditsText1X; hudCreditsText1X = 370.0f + fixHudCenter;
    fCreditsTextX = 390.0f + fixHudCenter;
    static float hudCreditsText3X; hudCreditsText3X = 400.0f + fixHudCenter;
    static float hudCreditsText4X; hudCreditsText4X = 410.0f + fixHudCenter;
    WriteMemory(pHudCreditsText2X.get_first(0x2), &fCreditsTextX);
    WriteMemory(pHudCreditsText2X2.get_first(0x2), &fCreditsTextX);
    WriteMemory(pHudCreditsText2X3.get_first(0x2), &fCreditsTextX);
    WriteMemory(pHudCreditsText1X.get_first(0x2), &hudCreditsText1X);
    WriteMemory(pHudCreditsText3X.get_first(0x2), &hudCreditsText3X);
    WriteMemory(pHudCreditsText3X2.get_first(0x2), &hudCreditsText3X);
    WriteMemory(pHudCreditsText2X4.get_first(0x2), &fCreditsTextX);
    WriteMemory(pHudCreditsText2X5.get_first(0x1), fCreditsTextX);
    if (nGameVersion == MAFIA_1_2_ENG)
    {
        WriteMemory(pHudCreditsText2X6.get_first(0x1), fCreditsTextX);
    }
    WriteMemory(pHudCreditsText1X2.get_first(0x2), &hudCreditsText1X);
    WriteMemory(pHudCreditsText4X.get_first(0x2), &hudCreditsText4X);

}

void InitMap()
{
    auto pMapLeftX = find_pattern("D8 0D ? ? ? ? 8B 4C 24 34 8B 54 24 38 89 8C 24 94 00 00 00",
        "D8 0D ? ? ? ? D9 44 24 14 D8 0D ? ? ? ? D9 44 24 10");
    auto pMapRightX = find_pattern("D8 0D ? ? ? ? 8B 54 24 50 C7 84 24 84 00 00 00 00 00 00 00",
        "D8 0D ? ? ? ? C7 84 24 94 00 00 00 00 00 00 00 C7 84 24 98 00 00 00 00 00 80 3F C7 84 24 D4 00 00 00 00 00 00 00");
    auto pMapPlayer = find_pattern("D8 8D B8 90 00 00 D9 C9 D8 8D BC 90 00 00 D9 5C 24 44",
        "D8 4C 24 10 D9 C9 D8 4C 24 14 D9 5C 24 58 DC C0");
    auto pMapWidth = find_pattern("D8 0D ? ? ? ? 84 C9 D9 5C 24 40 D8 0D ? ? ? ?",
        "D8 0D ? ? ? ? 84 C9 D9 5C 24 54 D8 0D ? ? ? ?");
    auto pMapScaleX = find_pattern("D8 05 ? ? ? ? D8 1D ? ? ? ? DF E0 25 00 41 00 00 75 ? C7 44 24 34 9A 99 19 3F",
        "D8 05 ? ? ? ? D8 1D ? ? ? ? DF E0 25 00 41 00 00 75 ? C7 44 24 18 9A 99 19 3F");
    auto pConstant10FMapScaleX = find_pattern("C7 44 24 34 9A 99 19 3F D9 44 24 38 D8 05 ? ? ? ?",
        "C7 44 24 18 9A 99 19 3F D9 44 24 1C D8 05 ? ? ? ?");
    auto pMapScaleX2 = find_pattern("D8 05 ? ? ? ? D9 94 24 BC 00 00 00 D9 9C 24 FC 00 00 00",
        "D8 05 ? ? ? ? 89 8C 24 F0 00 00 00 8B 8D 90 40 00 00");
    auto pMapPosX = find_pattern("D8 25 ? ? ? ? D9 5C 24 34 D9 44 24 14 D8 25 ? ? ? ?",
        "D8 25 ? ? ? ? D9 5C 24 18 D9 44 24 24 D8 25 ? ? ? ?");
    auto pMapPosX2 = find_pattern("D8 25 ? ? ? ? D9 5C 24 40 D8 25 ? ? ? ?",
        "D8 25 ? ? ? ? D9 5C 24 54 D8 25 ? ? ? ?");
    auto pMapPosX3 = find_pattern("D8 25 ? ? ? ? 32 C9 D9 5C 24 40 D8 25 ? ? ? ?",
        "D8 25 ? ? ? ? 32 C9 D9 5C 24 3C D8 25 ? ? ? ?");
    auto pMapTargetPosX = find_pattern("D8 0D ? ? ? ? D9 5C 24 4C 8B 44 24 4C D8 0D ? ? ? ?",
        "D8 0D ? ? ? ? D9 5C 24 48 8B 44 24 48 D8 0D ? ? ? ?");

    if (pMapLeftX.size() != 1 ||
        pMapRightX.size() != 1 ||
        pMapPlayer.size() != 1 ||
        pMapWidth.size() != 1 ||
        pMapScaleX.size() != 1 ||
        pConstant10FMapScaleX.size() != 1 ||
        pMapScaleX2.size() != 1 ||
        pMapPosX.size() != 1 ||
        pMapPosX2.size() != 1 ||
        pMapPosX3.size() != 1 ||
        pMapTargetPosX.size() != 1)
        return;

    float fixMapLeft;
    if (!S.widescreenMap)
    {
        fixMapLeft = (Screen.fBaseWidth - 800.0f) / 2.0f;
    }
    else
    {
        fixMapLeft = 0.0f;
    }
    static float mapLeftX; mapLeftX = (800.0f * 0.1f + fixMapLeft) / Screen.fBaseWidth;
    WriteMemory(pMapLeftX.get_first(0x2), &mapLeftX);
    static float mapRightX; mapRightX = 1.0f - mapLeftX;
    WriteMemory(pMapRightX.get_first(0x2), &mapRightX);
    fMapPlayerWidth = Screen.fHeight * (4.0f / 3.0f);
    if (nGameVersion == MAFIA_1_0_ENG)
    {
        // Replace FMUL [ebp+HUD.width] with FMUL [fMapPlayerWidth]. Both
        // encodings occupy six bytes; the following native FXCH stays intact.
        WriteMemory(pMapPlayer.get_first(), uint16_t{0x0DD8});
        WriteMemory(pMapPlayer.get_first(0x2), &fMapPlayerWidth);
    }
    else
    {
        // Later versions multiply by a stack-local width. Preserve the native
        // FMUL/FXCH sequence and restore the width before the next operation.
        // Copy integer bits here: both hooks run with a live x87 stack.
        // (The loader: the width is put back only when the first hook took it, so either hook alone changes nothing.)
        static uint32_t savedWidth;
        static bool saved;
        Mid(pMapPlayer.get_first(6), [](MmlMidContext& regs)
        {
            if (!saved) return;
            Field<uint32_t>(regs.esp, 0x10) = savedWidth;
            saved = false;
        });
        Mid(pMapPlayer.get_first(), [](MmlMidContext& regs)
        {
            savedWidth = Field<uint32_t>(regs.esp, 0x10);
            saved = true;
            Field<uint32_t>(regs.esp, 0x10) = std::bit_cast<uint32_t>(fMapPlayerWidth);
        });
    }
    static float mapWidth; mapWidth = mapRightX - mapLeftX;
    WriteMemory(pMapWidth.get_first(0x2), &mapWidth);

    float mapAspect = (mapWidth / (((800.0f * 0.9f + fixMapLeft) / Screen.fBaseWidth) - mapLeftX));
    static float mapScaleX; mapScaleX = 0.4f * mapAspect;
    WriteMemory(pMapScaleX.get_first(0x2), &mapScaleX);
    WriteMemory(pConstant10FMapScaleX.get_first(0x4), 1.0f - mapScaleX);
    WriteMemory(pMapScaleX2.get_first(0x2), &mapScaleX);
    static float mapPosX; mapPosX = 0.2f * mapAspect;
    WriteMemory(pMapPosX.get_first(0x2), &mapPosX);
    WriteMemory(pMapPosX2.get_first(0x2), &mapPosX);
    WriteMemory(pMapPosX3.get_first(0x2), &mapPosX);
    static float mapTargetPosX; mapTargetPosX = 2.5f / mapAspect;
    WriteMemory(pMapTargetPosX.get_first(0x2), &mapTargetPosX);
}

void InitText()
{
    auto pTextWidth = find_pattern("D8 0D ? ? ? ? D9 1C 24 55 E8 ? ? ? ? 8B F0 85 F6 74 ? 80 3E 00 74 ? 85 FF",
        "D8 0D ? ? ? ? 52 51 8B CB D9 1C 24 55 E8 ? ? ? ?");
    auto pTextWidth2 = find_pattern("D8 0D ? ? ? ? D9 1C 24 55 E8 ? ? ? ? 8B F0 85 F6 75 ? 33 D2",
        "D8 0D ? ? ? ? 6A 00 50 51 8D 6E 01 8B CB D9 1C 24");
    auto pTextWidth3 = find_pattern("D8 0D ? ? ? ? D9 1C 24 57 E8 ? ? ? ? 8B F0 85 F6 74 ?",
        "D8 0D ? ? ? ? 50 51 8B CD D9 1C 24 57 E8 ? ? ? ?");
    auto pTextWidth4 = find_pattern("D8 0D ? ? ? ? D9 1C 24 57 E8 ? ? ? ? 8B F0 85 F6 75 ?",
        "D8 0D ? ? ? ? 6A 00 51 51 8D 7E 01 8B CD D9 1C 24");
    auto pTextWidth5 = find_pattern("D8 0D ? ? ? ? D9 1C 24 55 E8 ? ? ? ? 8B F0 85 F6 74 ? 80 3E 00 74 ? 8B 93 FC 93 00 00",
        "D8 0D ? ? ? ? F7 D1 49 6A 00 6A 00 8D 04 89");
    auto pTextWidth6 = find_pattern("D8 0D ? ? ? ? D9 1C 24 55 E8 ? ? ? ? 8B F0 85 F6 75 ? 8B 93 00 94 00 00",
        "D8 0D ? ? ? ? 6A 00 51 51 8D 6E 01 8B CB D9 1C 24");

    if (pTextWidth.size() != 1 ||
        pTextWidth2.size() != 1 ||
        pTextWidth3.size() != 1 ||
        pTextWidth4.size() != 1 ||
        pTextWidth5.size() != 1 ||
        pTextWidth6.size() != 1)
        return;

    static float textWidth;

    textWidth = (800.0f * 0.9f) / Screen.fBaseWidth;

    WriteMemory(pTextWidth.get_first(0x2), &textWidth);
    WriteMemory(pTextWidth2.get_first(0x2), &textWidth);
    WriteMemory(pTextWidth3.get_first(0x2), &textWidth);
    WriteMemory(pTextWidth4.get_first(0x2), &textWidth);
    WriteMemory(pTextWidth5.get_first(0x2), &textWidth);
    WriteMemory(pTextWidth6.get_first(0x2), &textWidth);
}

void InitOthers()
{
    auto pFrameDelay = find_pattern("6A 10 8B 08 50 FF 91 F0 00 00 00 33 ED 89 44 24 10",
        "6A 10 8B 08 50 FF 91 00 01 00 00 33 ED 89 44 24 10");
    auto pCreditsCaseCount = find_pattern("83 F8 06 0F 87 ? ? ? ? FF 24 85 ? ? ? ? D9 05 ? ? ? ?");

    if (S.fpsLimit > 0 && pFrameDelay.size() == 1)
        WriteMemory(pFrameDelay.get_first(0x1), static_cast<uint8_t>(std::clamp(1000 / S.fpsLimit, 0, 127)));
    if (pCreditsCaseCount.size() != 1) return;
    auto dispatch = reinterpret_cast<uintptr_t>(pCreditsCaseCount.get_first()) + 9;
    static uintptr_t creditsEnd;
    static uintptr_t creditsHUD;
    static uintptr_t creditsDraw;
    creditsEnd = reinterpret_cast<uintptr_t>(pCreditsCaseCount.get_first()) + 9 + Field<int32_t>(reinterpret_cast<uintptr_t>(pCreditsCaseCount.get_first()), 5);
    auto pDraw = find_pattern("8B 44 24 24 8B 54 24 20 D9 44 24 14 D8 89 C4 90 00 00",
        "8B 44 24 24 8B 54 24 20 D9 44 24 14 D8 89 C4 40 00 00");
    auto draw = pDraw.size() == 1 ? reinterpret_cast<uintptr_t>(pDraw.get_first()) : 0;
    // Native case C uses the same HUD singleton as the missing line.
    auto table = Field<uintptr_t>(dispatch, 3);
    auto nativeCase = Field<uintptr_t>(table, 8);
    uintptr_t hud = 0;
    for (size_t i = 0; i < 100; ++i)
        if (Field<uint8_t>(nativeCase, i) == 0xB9 && Field<uint8_t>(nativeCase, i + 5) == 0xE8)
        { hud = Field<uintptr_t>(nativeCase, i + 1); break; }
    if (!draw || !hud) return;
    creditsDraw = draw;
    creditsHUD = hud;
    Mid(reinterpret_cast<uintptr_t>(pCreditsCaseCount.get_first()), [](MmlMidContext& regs)
    {
        if (regs.eax != 7) return;
        using OutTextUnits = int(__thiscall*)(void*, int, float, float, float, float, int, int, int, int);
        reinterpret_cast<OutTextUnits>(creditsDraw)(reinterpret_cast<void*>(creditsHUD), Field<int>(regs.esi, 8),
            fCreditsTextX, Field<float>(regs.esp, 0xC), 20.0f, 30.0f, -1, 2, 1, 0);
        regs.eip = creditsEnd;
    });
}

uintptr_t menuRawCoordinates;
std::array<uintptr_t, 5> menuCallers;
bool menuHooksInstalled;   // (false again at each Apply: ResetWidescreen)

void InstallMenuHooks()
{
    auto pMenuImagesOld = find_pattern("E8 ? ? ? ? D9 45 10 8B 45 34 C7 45 00 ? ? ? ? D9 55 38 D9 45 14 3B C3 D9 54 24 20 D9 5D 3C 74 ? D8 40 38 D9 5D 38 D9 44 24 20 D8 40 3C C7 45 00 ? ? ? ? 8B C5 D9 5D 3C E9 ? ? ? ? DD D8 C7 45 00 ? ? ? ? 8B C5 E9 ? ? ? ? 81 E9 64 6E 69 77");
    auto pMenuPanelsOld = find_pattern("E8 ? ? ? ? D9 45 10 8B 45 34 C7 45 00 ? ? ? ? D9 55 38 D9 45 14 3B C3 D9 54 24 20 D9 5D 3C 74 ? D8 40 38 D9 5D 38 D9 44 24 20 D8 40 3C D9 5D 3C EB ? DD D8 C7 45 00 ? ? ? ? 8B C5");
    auto pMenuGarageOld = find_pattern("E8 ? ? ? ? D9 45 10 8B 45 34 C7 45 00 ? ? ? ? D9 55 38 D9 45 14 3B C3 D9 54 24 20 D9 5D 3C 74 ? D8 40 38 D9 5D 38 D9 44 24 20 D8 40 3C 8B C5");
    auto pMenuRaceOld = find_pattern("E8 ? ? ? ? D9 46 10 8B 46 34 C7 06 ? ? ? ?");
    auto pMenuCarsOld = find_pattern("E8 ? ? ? ? 8B 46 08 C7 06 ? ? ? ? 0D 01 00 01 00");
    auto pMenuPanelsNew = find_pattern("E8 ? ? ? ? C7 06 ? ? ? ? 8B C6 8B 4C 24 10 5F 5E 5B 64 89 0D 00 00 00 00 83 C4 10 C3 90 90 90 90 90 90 B8 64 6E 69 77");
    auto pMenuImagesNew = find_pattern("E8 ? ? ? ? C7 06 ? ? ? ? 8B C6 8B 4C 24 10 5F 5E 5B 64 89 0D 00 00 00 00 83 C4 10 C3 90 90 90 90 90 90 B8 63 6E 69 77");
    if (menuHooksInstalled) return;
    menuHooksInstalled = true;
    if (nGameVersion == MAFIA_1_0_ENG)
    {
        if (pMenuImagesOld.size() != 1 ||
            pMenuPanelsOld.size() != 1 ||
            pMenuGarageOld.size() != 1 ||
            pMenuRaceOld.size() != 1 ||
            pMenuCarsOld.size() != 1)
            return;
        menuCallers = {reinterpret_cast<uintptr_t>(pMenuImagesOld.get_first()) + 5, reinterpret_cast<uintptr_t>(pMenuPanelsOld.get_first()) + 5,
            reinterpret_cast<uintptr_t>(pMenuGarageOld.get_first()) + 5, reinterpret_cast<uintptr_t>(pMenuRaceOld.get_first()) + 5, reinterpret_cast<uintptr_t>(pMenuCarsOld.get_first()) + 5};
        auto pCoordinateMode = find_pattern("8A 15 ? ? ? ? 3A D1 74 ? 8B 50 08 89 56 10");
        auto flag = pCoordinateMode.size() == 1 ? reinterpret_cast<uintptr_t>(pCoordinateMode.get_first()) : 0;
        auto pPosition = find_pattern("8B 50 1C 89 4C 24 08 89 56 20 8B 50 20");
        auto position = pPosition.size() == 1 ? reinterpret_cast<uintptr_t>(pPosition.get_first()) : 0;
        if (!flag || !position) return;
        menuRawCoordinates = Field<uintptr_t>(flag, 2);
        Mid(position, [](MmlMidContext& regs)
        {
            if (Field<uint8_t>(menuRawCoordinates, 0)) return;
            auto caller = Field<uintptr_t>(regs.esp, 0xC);
            auto templateOffset = regs.eax - menuRawCoordinates;
            // Offsets within the original menu-template buffer, whose base is
            // discovered from the constructor's native coordinate-mode flag.
            static constexpr std::array<uint32_t, 23> panels = {
                0x2B4, 0x200, 0x638, 0x2474, 0x2840, 0x695C, 0x4148, 0x4484, 0x45C8,
                0xDF4, 0xECC, 0xF14, 0xF5C, 0x8E4, 0x6CBC, 0x80C, 0x2A80, 0x2AC8,
                0x4E80, 0x4F58, 0x2E4C, 0x65D8, 0x6470};
            bool shift = caller == menuCallers[1] && std::ranges::find(panels, templateOffset) != panels.end();
            shift |= caller == menuCallers[0] && (templateOffset == 0x6788 || templateOffset == 0x266C);
            shift |= (caller == menuCallers[2] || caller == menuCallers[3] || caller == menuCallers[4]) &&
                (templateOffset == 0x5030 || templateOffset == 0x4970 || templateOffset == 0x278C);
            if (shift) Field<float>(regs.esi, 0x10) += fMenuOffset;
        });
    }
    else
    {
        if (pMenuPanelsNew.size() != 1 ||
            pMenuImagesNew.size() != 1)
            return;
        menuCallers[0] = reinterpret_cast<uintptr_t>(pMenuPanelsNew.get_first()) + 5;
        menuCallers[1] = reinterpret_cast<uintptr_t>(pMenuImagesNew.get_first()) + 5;
        // Both X copies and Y are stored here; the x87 stack is empty. Shift
        // before the native constructor attaches the object to its parent.
        auto pPosition = find_pattern("D9 5E 18 D9 05 ? ? ? ? D8 48 10 D9 5E 1C D9 05 ? ? ? ? D8 48 14");
        auto position = pPosition.size() == 1 ? reinterpret_cast<uintptr_t>(pPosition.get_first()) : 0;
        if (!position) return;
        Mid(position + 3, [](MmlMidContext& regs)
        {
            auto caller = Field<uintptr_t>(regs.esp, 0x18);
            auto type = Field<uint32_t>(regs.eax, 0);
            auto flags = Field<uint32_t>(regs.eax, 0x1C);
            bool shift = caller == menuCallers[0] && (type == 1 || type == 3 || type == 0x82 ||
                (type == 2 && (flags == 0xFF00B2 || flags == 0xFF0032 || flags == 0x1F0032)));
            shift |= caller == menuCallers[1] && type >= 1 && type <= 3;
            if (shift)
            {
                Field<float>(regs.esi, 0x14) += fMenuOffset;
                Field<float>(regs.esi, 0x24) += fMenuOffset;
            }
        });
    }
}

void InitMenu()
{
    auto pOnePCurBaseWidth4 = find_pattern("D8 0D ? ? ? ? D9 1D ? ? ? ? 8B 10 FF 52 70",
        "D8 0D ? ? ? ? EB ? 8B 10 FF 52 70 89 44 24 18");
    auto pOnePCurBaseWidth3 = find_pattern("D8 0D ? ? ? ? EB ? 8B 10 FF 52 70 89 44 24 1C");
    auto pHudPleaseWaitTextX = find_pattern("D8 0D ? ? ? ? D9 1C 24 50 E8 ? ? ? ? A1 ? ? ? ? 50 8B 10",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 50 E8 ? ? ? ? A1 ? ? ? ? 50 8B 08");
    auto pHudPleaseWaitTextX2 = find_pattern("D8 0D ? ? ? ? D9 1C 24 50 E8 ? ? ? ? A1 ? ? ? ? 50 8B 08",
        "D8 0D ? ? ? ? D8 05 ? ? ? ? D9 1C 24 50 E8 ? ? ? ? A1 ? ? ? ? 50 8B 10");
    if (pOnePCurBaseWidth4.size() != 1 ||
        pHudPleaseWaitTextX.size() != 1 ||
        pHudPleaseWaitTextX2.size() != 1)
        return;
    if (nGameVersion != MAFIA_1_0_ENG && (pOnePCurBaseWidth3.size() != 1)) return;


    if (nGameVersion == MAFIA_1_0_ENG)
    {
        WriteMemory(pOnePCurBaseWidth4.get_first(0x2), &Screen.fInvBaseWidth);
    }
    else
    {
        WriteMemory(pOnePCurBaseWidth3.get_first(0x2), &Screen.fInvBaseWidth);
        WriteMemory(pOnePCurBaseWidth4.get_first(0x2), &Screen.fInvBaseWidth);
    }

    float fixMenuRight = (Screen.fBaseWidth - 800.0f) / 2.0f;

    static float menuPleaseWaitTextX; menuPleaseWaitTextX = 780.0f + fixMenuRight;
    WriteMemory(pHudPleaseWaitTextX.get_first(0x2), &menuPleaseWaitTextX);
    WriteMemory(pHudPleaseWaitTextX2.get_first(0x2), &menuPleaseWaitTextX);
    fMenuOffset = (Screen.fWidth - (Screen.fHeight * (4.0f / 3.0f))) / 2.0f;
    InstallMenuHooks();
}

void InitDrawDistance()
{
    auto pDrawDistance = find_pattern("8B 8C 24 B4 00 00 00 8B 03 51 68 CD CC 4C 3D 53");
    auto pDrawDistValue = find_pattern("68 00 00 96 43 68 CD CC CC 3D 8B 51 10 8B 82 7C 01 00 00");
    auto pDrawDistValue1 = find_pattern("68 00 00 48 43 4F 52 50 57 8B CE 83 ED 74 E8 ? ? ? ?");
    auto pDrawDistValue2 = find_pattern("D8 05 ? ? ? ? D9 44 24 28 D8 4C 24 28 D9 44 24 24");
    auto pDrawDistValue22 = find_pattern("D8 1D ? ? ? ? DF E0 F6 C4 41 7B ? D9 44 24 44");
    auto pDrawDistValue23 = find_pattern("D8 1D ? ? ? ? DF E0 F6 C4 41 7B ? D9 44 24 3C");
    auto pDrawDistValue24 = find_pattern("D8 1D ? ? ? ? DF E0 F6 C4 41 7A ? EB ? 33 ED");
    auto pDrawDistValue25 = find_pattern("D8 1D ? ? ? ? DF E0 F6 C4 41 7B ? 83 C5 08");
    auto pDrawDistValue12 = find_pattern("C7 44 24 14 00 00 48 43 C7 44 24 50 00 00 66 43");
    auto pDrawDistValue26 = find_pattern("C7 44 24 50 00 00 66 43 8B 11 89 56 2C 8B 41 04");

    if (pDrawDistance.size() != 1 ||
        pDrawDistValue.size() != 1)
        return;
    if (nGameVersion == MAFIA_1_0_ENG && (pDrawDistValue1.size() != 1 || pDrawDistValue2.size() != 1 || pDrawDistValue22.size() != 1 || pDrawDistValue23.size() != 1 || pDrawDistValue24.size() != 1 || pDrawDistValue25.size() != 1)) return;
    if (nGameVersion != MAFIA_1_0_ENG && (pDrawDistValue12.size() != 1 || pDrawDistValue26.size() != 1)) return;

    static float drawDistValue1; drawDistValue1 = S.drawDistanceValue - 50.0f;
    static float drawDistValue2; drawDistValue2 = S.drawDistanceValue - 20.0f;

    Mid(pDrawDistance.get_first(), [](MmlMidContext& regs)
    {
        auto& distance = Field<float>(regs.esp, 0xB4);
        if (distance > S.drawDistanceMin && distance < S.drawDistanceMax)
            distance = S.drawDistanceValue;
    });

    if ((300.0f > S.drawDistanceMin) && (300.0f < S.drawDistanceMax))
    {
        WriteMemory(pDrawDistValue.get_first(0x1), S.drawDistanceValue);
    }

    if (nGameVersion == MAFIA_1_0_ENG)
    {
        WriteMemory(pDrawDistValue1.get_first(0x1), drawDistValue1);

        WriteMemory(pDrawDistValue2.get_first(0x2), &drawDistValue2);
        WriteMemory(pDrawDistValue22.get_first(0x2), &drawDistValue2);
        WriteMemory(pDrawDistValue23.get_first(0x2), &drawDistValue2);
        WriteMemory(pDrawDistValue24.get_first(0x2), &drawDistValue2);
        WriteMemory(pDrawDistValue25.get_first(0x2), &drawDistValue2);
    }
    else
    {
        WriteMemory(pDrawDistValue12.get_first(0x4), drawDistValue1);

        WriteMemory(pDrawDistValue26.get_first(0x4), drawDistValue2);
    }
}

void ResetWidescreen() { menuHooksInstalled = false; }

} // namespace ff
