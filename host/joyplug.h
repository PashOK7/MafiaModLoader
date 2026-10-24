// The joysticks plugged in while the game runs (host/joyplug.cpp), called from host/input.cpp.
#pragma once
#include <string>

namespace host {
void JoyBeforeControls();      // the controls' update hook, before the game's own: the watch started at the first
                               // (not in DllMain: the notifications' registration talks to a service), a check
                               // when the devices changed
bool JoyMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
} // namespace host
