# IJoy.dll: the game's joystick layer

The same file in both builds (and the demo). Marvisak/mafia-re has it decompiled byte for byte
(`re/ext/mafia-re/ijoy/`: `ijoy.h` the interfaces in vtable order, `joy.h` the classes); its names are guesses (the
DLL has no symbols). `Game.exe` imports one export, `CreateJoyBase@20` (`0x10001040`):
`bool CreateJoyBase(IJoyBase **out, HINSTANCE, HWND, IDirectInput7A *di_or_null, uint flags)` (the game's call,
1.2 0x5BD5F0: `push 0` flags, `push 0` di, `push hwnd` (IGraph slot +0x10), `push hinstance`, `push &joyBase`; an
older note here had the order wrong). With no DirectInput given it makes its own (`DirectInputCreateEx(..., 0x700)`).
The interfaces are C++ virtuals (thiscall, `ecx` = the object), not COM.

- **EnumDevices** (`DIDEVTYPE_JOYSTICK`, `DIEDFL_ATTACHEDONLY`) runs once, in the base's constructor: a device
  record each (`CreateDeviceEx`, the objects enumerated, the caps). With **no controller attached the enumeration's
  `ok` stays false and CreateJoyBase fails**: the game has no base at all. One device that cannot be created stops
  the enumeration and fails the base too.
- Base (`C_joy_base`, 0x2C bytes): `+0x00` vtable, `+0x04` refs, `+0x08` the enumeration's context (`+0x08` the
  DirectInput, `+0x0C` the window, `+0x10` the device records, `+0x14` ok), `+0x18` HINSTANCE, `+0x1C` the
  DirectInput, `+0x20` HWND, `+0x24` the device count, `+0x28` the IJoys made (a list). Its vtable (0x1000D100):
  `+0x04` AddRef, `+0x08` Release, `+0x18` NumDevices, `+0x20` GetDirectInput, `+0x24` GetDeviceInfo(info, i),
  `+0x2C` CreateJoy(IJoy **, i, flags: 1 background, else foreground; always exclusive). A device record:
  `+0x00` its DIDEVICEINSTANCEA (`+0x04` guidInstance, `+0x28` the instance name, `+0x12C` the product name),
  `+0x04` the device while it is enumerated, `+0x08` DIDEVCAPS, `+0x34` the objects, `+0x38` the next record
  (checked in IJoy.dll's GetDeviceInfo 0x100013B0).
- An IJoy (`C_joy`, 0xF4C bytes): `+0x08` the base (AddRef'd), `+0x0C` its device record, `+0x18` its
  IDirectInputDevice7A, `+0x1B8` a bool "acquired" (C_joy::Acquire 0x10003A60 writes it, then Acquires or
  Unacquires). Update (`+0x58`, 0x10002C90): Poll; when it fails and the bool is set, Acquire again; with
  `JOY_UPDATE_RESET` (1, what the game passes) a device that cannot be read reports nothing. Its vtable
  (0x1000D134): `+0x30` SetAxisMode, `+0x50` GetState, `+0x54` GetStateChanges, `+0x58` Update, `+0x5C`
  GetAxisName, `+0x64` GetButtonName, `+0x68` GetInfo, `+0x6C` GetPOVName, `+0x84` SendFFCommand, `+0x88`
  CreateEffect. An effect AddRefs its IJoy (and keeps its device).

Keyboard and mouse do not go through it: LS3D reads them with DirectInput 8 itself (mafia-ragdoll's `game_input`
holds them back from the game by hooking those reads). mafia-ragdoll's host is a proxy `IJoy.dll`; the loader uses
Ultimate ASI Loader instead and leaves `IJoy.dll` alone.

## How the game keeps its joysticks (both builds, 2026-10-24)

Read in 1.2's code, 1.0's twins the same code (addresses: `game_build.h` "The joysticks").

- **InitSystem** calls `joy_devices_init` (1.2 0x5BD5F0, 1.0 0x5F83E0; fastcall, ecx = the HINSTANCE) once:
  CreateJoyBase into `joyBase` (1.2 0x647F24, 1.0 0x6F956C), then for each device `CreateJoy(i, 1)` pushed to a
  `vector<IJoy*>` (1.2 begin 0x647C20, 1.0 0x6F92D0; the vector object 4 bytes before), each sent
  SendFFCommand 3, 6, 4, 5 (reset, stop all, actuators off, on). A CreateJoy that fails is followed by a call
  through its null IJoy (`[ecx]` with ecx 0): the game crashes at its start when a device it enumerated cannot be
  opened. Then `input_init_devices` (1.2 0x4EFDE0, 1.0 0x5D1770; thiscall(controls, IGraph*, IJoy **, count) ret
  0xC) gives them to the controls: their own list at `controls + 8 / +0xC`, 0x8C bytes an entry (`+0x00` the IJoy,
  AddRef'd; `+0x04` its S_joy_state, `+0x48` its changes); it also zeroes the key list (`+0x20`, 64 codes), its
  counts (`+0x60`) and the mouse buttons' (`+0xA0`).
- Each frame the game loop (GameLoopSingle 0x549465) and the menus' loop (GM_Menu::ExecuteMenu) call Update(1) of
  each IJoy of the vector, then `controlsUpdate`, which reads GetState and GetStateChanges of each of the controls'
  list. A binding `{dir, 0, device 3, code, u32 joystick}` reads the entry at that index (when it is below the
  count): code < 0x100 an axis (0..7: X, Y, Z, Rx, Ry, Rz, two sliders), 0x100..0x1FF a button, from 0x200 a POV.
- **A joystick's binding is dropped when its index is not below the count**: `input_set_binding` (1.2 0x4F02B0,
  1.0 0x5D1C30) returns 0 and leaves the slot empty, and `loadsave_apply_key_bindings` (1.2 0x607A60, 1.0 0x559A90;
  thiscall(G_LoadSaveClass)) gives the profile's 60 bindings (its `+0x1F4`) to the controls through it after
  `input_clear_bindings`. So with the pad absent at the start its bindings stay in the profile (and its file) but
  not in the controls. The Options > Controls menu edits the profile's copy itself and applies it at its close.
- `input_release_all` (1.2 0x4EFC80, 1.0 0x5D1610; thiscall(controls)) empties the bindings' three arrays and
  releases the controls' list; `joy_devices_done` (1.2 0x5BD870, 1.0 0x5F8660) releases the vector's IJoys
  (end = begin) and the base (joyBase = 0). Nothing else holds the base; only the force feedback manager
  (C_ff_manager, 1.2 0x647B28) reads the vector: C_game::Init makes its effects (on the IJoy of binding 1's or
  0x26's joystick, by its index into the vector, unchecked; safe because the controls never keep an index past the
  count) and its GameInit reads whether that IJoy has force feedback. The controls menu's binding widgets copy
  the names they get (`controlsKeyName` returns IJoy's strings) into their own buffers.

So a pad plugged in after the start is never opened, and one there at the start and plugged in again works only
if its DirectInput device object recovers on Acquire. The loader opens them again (host/joyplug.cpp): on a HID
device interface's arrival (CM_Register_Notification), once the devices are quiet, a controller DirectInput lists
that the base does not have, or has but cannot poll or acquire, has the game's own functions run again in the
start's order: the old IJoys' devices unacquired (a force feedback effect keeps its IJoy alive, and that IJoy's
exclusive acquisition would keep the device from the new one; its "acquired" bool cleared so it never takes it
back), `input_release_all`, `joy_devices_done`, `joy_devices_init` (its fault on a failed CreateJoy caught),
`input_init_devices`, `loadsave_apply_key_bindings`. An unplugged controller is no reason to open them again: its
IJoy stays where it is, so no index moves. The running mission's force feedback effects stay on the old IJoys
until the next mission's load makes them again.
