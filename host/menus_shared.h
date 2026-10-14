// What the two layers of the game's menus share: menus.cpp (1.2's GM_Menu classes, and every build's registry of
// the mods' items) and menus10.cpp (1.0's screens). Only these two files include it.
#pragma once
#include "host.h"
#include <string>
#include <vector>

namespace host {

// ---- from menus.cpp
const char *MenuIntern(const std::string &s);            // a string the game's controls may point at: never freed
// Consecutive text ids the game's text database answers with these strings (0: the database is not hooked); the
// same strings again: the same ids.
uint32_t MenuTextIds(const std::vector<std::string> &strings);
bool MenuTextsHook();                                     // the text database hooked (once); false: it cannot be
std::string MenuUtf8(const char *ansi);                   // the game's strings are ANSI, the bridge's UTF-8
std::string MenuAnsi(const std::string &utf8);
std::vector<std::string> MenuSplitOptions(const std::string &text);   // "A|B|C"
// A click delivered: to a mod behind its fault guard, or (mod null) the bridge's, logged and kept for menu.items.
void MenuDeliver(MmlMod *mod, MmlMenuFn fn, void *user, int id, const std::string &what);
struct MenuItemInfo {
    int handle;
    MmlMod *mod;             // null: the bridge's
    std::string menu;        // lower case
    int after;
    const char *text;        // interned
    MmlMenuFn fn;
    void *user;
};
std::vector<MenuItemInfo> MenuItemsFor(const std::string &menu);   // the items asked for in this menu (live mods)
bool MenuItemByHandle(int handle, MenuItemInfo *out);

// ---- from menus10.cpp: the same services on 1.0's screens (docs/VERSIONS.md). Game thread.
namespace m10 {
bool Known();                                   // 1.0's screens are known (the build is 1.0 and the hooks are in)
void Init();
void Frame();
uintptr_t Top();                                // the active screen, 0: none
uintptr_t Under(uintptr_t screen);              // the screen it was opened over
std::string Name(uintptr_t screen);             // its name (1.2's definition name of the same screen where there is one)
bool Command(uintptr_t screen, int id);         // its Tick gets `id` as the control activated, at its next frame
std::string Describe(uintptr_t screen);         // JSON: name, controls
bool ControlValue(uintptr_t screen, int id, float *value);
void ItemAsked(int handle);                     // a mod's item (again, after a reload: its control taken over)
void ModRemoved(MmlMod *mod);                   // before its items leave the registry
int PageOpen(MmlMod *mod, bool bridge, const char *name, const MmlMenuControl *controls, int count, MmlMenuFn fn, void *user,
             std::string *err);
void PageClose(MmlMod *mod, bool bridge);
int PageText(MmlMod *mod, bool bridge, int id, const char *text);
int PageValue(MmlMod *mod, bool bridge, int id, float value);
int PageGetValue(MmlMod *mod, bool bridge, int id, float *value);
int PageRange(MmlMod *mod, bool bridge, int id, int lo, int hi);
int PageGetText(MmlMod *mod, bool bridge, int id, std::string *text);
std::string PagesJson();                        // JSON array: {name, owner, open}
// menu.click, menu.def, menu.page, menu.page.set, menu.page.close on 1.0. False: not one of these.
bool Method(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok);
}

} // namespace host
