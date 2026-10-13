# IJoy.dll: the game's joystick layer

From MafiaReverse (`IJoy_notes.md`). `Game.exe` imports one export, `CreateJoyBase@20` (`0x10001040`):
`uint32_t CreateJoyBase(HINSTANCE, void **outBase, HWND, uint32_t mode, IDirectInputA *di_or_null)`. It makes a
0x2C-byte base object (DirectInput 7 through `DirectInputCreateEx(…, 0x700, …)` unless one is given), enumerates
game controllers (list at `+0x10`, count `+0x24`) and opens per-device wrappers (0xF4C bytes, axes in
-10000..10000; list at `+0x28`).

Base object: `+0x00` vtable, `+0x04` use count, `+0x0C` window copy, `+0x10` device records, `+0x14` enum ok,
`+0x18` HINSTANCE, `+0x1C` the DirectInput interface, `+0x20` HWND, `+0x24` device count, `+0x28` opened devices.

Keyboard and mouse do not go through it: the game reads them with DirectInput 8 itself (mafia-ragdoll's
`game_input` holds them back from the game by hooking those reads). mafia-ragdoll's host is a proxy `IJoy.dll`;
the loader uses Ultimate ASI Loader instead and leaves `IJoy.dll` alone.
