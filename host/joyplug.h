// The joysticks plugged in while the game runs (host/joyplug.cpp), called from host/input.cpp.
#pragma once
#include <string>

namespace host {
void JoyBeforeControls();      // starts the watch outside DllMain and checks at startup and after device changes
bool JoyMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
} // namespace host
