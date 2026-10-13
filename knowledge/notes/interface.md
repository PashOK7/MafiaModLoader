# The game's interface: menus, drawing, the HUD, script commands

From pet1kk's notes (2026-10-13), the reverse behind his front-end mods for the 1.2 game (`menu_bertone.asi`,
`menu_pomsta.asi`: a main menu drawn by the mod over a Bink video, the game's own menu kept alive underneath and
its click handler called for every action). His addresses were all used in working code. Each fact says how it is
known here: **friend** (his notes only), **game** (also checked in our running 1.2 game or its bytes), **ours**
(found by us before, the same address in his notes).

## Menus

- **ours** The active menu: `0x6BD890`; the main menu's vtable `0x625EB8`; the profile menu's `0x6279DC`; a menu
  run modally: `GM_Menu::ExecuteMenu` `0x5EBA40`; the profile manager (`G_LoadSaveClass`) `0x6D4628`, its profiles
  `+4/+8`, 0x54 bytes each, the name at `+0x10`. A click: the menu's vtable slot 9 (`+0x24`) with the button's id.
- **friend** `GM_Menu::Draw` `0x5EA7E0`, `thiscall(menu, flags)`, `ret 4`: hooked at its entry, a mod draws its
  own screen instead of (or behind) a menu.
- **game** The menus' mouse (read in the disassembly, driven in the lab 2026-10-13): LS3D reads the mouse once a frame
  (`ls3d+0x71E20`, IGraph slot 55 in 1.2; 1.0 `ls3d+0x6E2A0`, its slots are shifted) into a DIMOUSESTATE at
  `ls3d+0x1C56F0` (1.0 `+0x1C160C`): `lX, lY, lZ` moves since the last read, then 4 button bytes (0x80 held). The
  menus' loop (`ExecuteMenu`) calls it, then hands IGraph slots 56/57/58 (the moves) and 62 (the buttons as bits,
  with their edges) to the input tick, which adds the moves to the cursor `0x6BD8A0/A4` (pixels, clamped to the
  screen). Once the cursor has moved more than 10 pixels since the last reference (`0x6BD898`; keyboard focus
  resets it) the control under it (`0x5E9DE0`: shown, enabled, 0x8000, its slot 8 contains / slot 9 hits) gets the
  focus. Buttons or the wheel go to the focused control: its slot 9 hit test (skipped when its flags have 0x4000:
  scrolled lists, sliders' drags), then its slot 11 `(x, y, wheel, buttons, ms)`, whose answer is the command the
  menu's slot 9 gets. So the loader injects a move as `lX = target - cursor` after the read. The game's lists do not
  scroll by the wheel (it goes to the focused row). Typing: IGraph slot 39 (`ls3d+0x71960`) finds a newly pressed
  key in the keyboard state (injected keys too, modifiers skipped) and latches its character (slot 50: the key with
  Shift's state) at `ls3d+0x1C546C`; slot 40 hands it to a text field and clears it.
- **friend** The menus' input tick `0x5EA8C0`, `thiscall(menu, rx, ry, rz, buttons, key, shift, ms)`, `ret 0x1C`,
  called from the modal loop at `0x5EBD22`: `key` a DirectInput code, `buttons` bit 0 the left mouse button.
  Redirecting that one call takes the input over while a menu is on top. The cursor, in pixels: `0x6BD8A0` (x),
  `0x6BD8A4` (y) (both 0 with no menu open: **game**). This is the way to the menus' mouse for `input`.
- **friend** The layout pass: `0x5E9DA0`, `thiscall(root, flags)`, recursive (a control's vtable slot 6). A
  control draws in its vtable slot 5. The menu scene's renderer `0x5E3D40` (its call returns to `0x5E3D85`) also
  draws the loading screens, with no menu open.
- **friend** Controls by vtable: window `0x6260F8`, `0x626200`; text `0x626280`, `0x626300`, `0x626380`,
  `0x626400`; a wrapped text box `0x627210` (its lines in a buffer at `+0x44`, split by `~`); line `0x626480`;
  rectangle `0x626500`, `0x626580`; button `0x626900`, `0x626980`, `0x626880`, `0x626B00` (a button draws at
  `0x5DB7B0`); list item `0x626700`, `0x626780`, `0x626800` (draws at `0x5DADC0`: the profile list, yes/no);
  slider `0x626A00`, `0x626A80`; selector `0x626C80`, `0x626D00`; edit box `0x626B80`; video `0x626E00`.
- **friend** `.MNU` files: `tables\menu\<name>_<lang>.mnu`, else `tables\menu\<name>.mnu` (the loader `0x5EA370`):
  a header, then controls with a type tag, x/y/w/h in 800x600 units, a text id, flags, children.
- **friend** The texts' file: `tables\textdb_<lang>.def`: u32 count, u32 0, count times (u32 id, u32 offset), then
  the strings, 0-terminated, cp1250. (Ours: the database in memory `0x6D8714`, `GetTextOrNULL` `0x60FBA0`.)
- **friend** Profiles: `0x605570` `thiscall(manager, index)` selects one, `0x6055C0` `thiscall(manager, name)`
  creates and selects one (1: done). The profile menu runs at the start (the call at `0x604FAF`) and from "change
  profile" (`0x6050DF`). The file name formats are pushed at `0x6052D4`, `0x605693`, `0x60576E`, `0x60583B`,
  `0x605A3C` (profiles: `push 0x6395F4`) and `0x60578E`, `0x605BBE`, `0x6060CA`, `0x6061B3`, `0x606F11` (saves:
  `push 0x639618`): pointing those at another format moves every save file.
- **friend** The menus' sounds (LS3D sound objects): `0x6BD8B8` open, `0x6BD8BC` close, `0x6BD8C0` change. The
  menus' paper textures: `0x6BD8C8` (`papir3.tga`, a window's body), `0x6BD8C4` (`papir5a.tga`, its title strip).

### The menus as objects (ours, 2026-10-13; used by `host/menus.cpp`, addresses in `game_build.h`)

- **game** A menu is a `GM_Menu` (0x30 bytes, the game's operator new `0x6106FC`, constructor `0x5EA1F0` puts the
  base vtable `0x627834`): `+4` the menu under it, `+8` its root control, `+0xC` the focused one, `+0x14/+0x18` its
  controls (a vector of pointers in the order they were made), `+0x2C` flags (0x10000: the menu under it drawn too,
  greyed). Its vtable: 0 the definition's name, 1 OnCreate, 9 OnCommand(id) (what a click or Enter sends; the
  three are replaced in the loader's pages and seen working), and, read in the decompile, 2 OnClose, 7 OnKey (the
  base closes the menu on Esc: seen), 10 Tick. The active menu `0x6BD890`; `0x5EAD50` (stdcall(menu, arg), ret 8)
  puts a menu over it and calls `GM_Menu::Create` `0x5EA2E0`, which reads the definition (`0x5EA370`: through
  `rw_data.dll`'s `dtaOpen`/`dtaRead`/`dtaClose`, `tables\menu\<name>_<lang>.mnu` first) and makes the controls.
  `GM_Menu::MenuClose` `0x5EAE10` (ret 4); the game deletes closed menus itself (`0x5EA690`).
- **game** The definition: a 16-byte header (the count of records at `+8`), then 32-byte records `{id, tag, x, y, w,
  h (floats, 800x600), text id, flags}`; a window's children follow it until a record tagged `pupu`. A record with
  text id 0 has no text (the loader sets texts afterwards with the control's SetText, vtable slot 16, which keeps
  the pointer). The loader's pages are read this way from records it builds: every control made by the game.
- **game** The factories: one chain at `0x6BD8B0` of `{create fastcall(record, parent), clone thiscall(control), tag,
  next}`, 38 kinds, walked live: `grid` `0x5E0360`, `stxb`, `tinp`, `vscr`, `lstc`, `list`, `cmbw`, `comb`, `vidc`,
  `edin` `0x5DD1E0`, `bslb` `0x5DCF40`, `bsel` `0x5DCDB0`, `chck` `0x5DCA60`, `edit` `0x5DC7A0`, `netp`, `sldi`,
  `slid` `0x5DB9D0`, `butc`, `butt` `0x5DB730`, `rari` `0x5DB480`, `iten` `0x5DAFF0`, `itmm`, `item` `0x5DAD40`,
  `pbar` `0x5DABE0`, `rctt` `0x5DA9E0`, `orez`, `rect`, `line` `0x5DA660`, `htxt`, `ntxt`, `mtxt`, `stxt`
  `0x5DA020`, `wins` `0x5D9980`, `winc`, `wind` `0x5D94D0`, `upup`, `xxxx`, `comp`. (An earlier note here missed the
  chain's second half and said there was no `chck`: there is.) Where the game's own definitions use them:
  `menu.def` (vidc: the controls' bindings; edit: the profile's name; bsel / bslb: Options' choices; iten: a race's
  laps; rari: championship rounds; wins: the load menu's saves; vscr + lstc: the Carcyclopedia; cmbw + butc: the
  combo menu; mtxt / ntxt / htxt / orez: the race tables).
- **game** A definition as the loader (`GM_Menu::CreateCompsByDef` `0x5EA370`) reads it: header `{"Menu", 1, count,
  0}`, then records. A record's parent is the current container; a control whose slot 4 answers true (`wind`,
  `winc`, `wins`, `grid`) becomes the current container; `pupu` makes the topmost window the current one again (it
  walks up the parents to the top, not one level). The first control is the menu's root (its x moved by the
  widescreen offset `0x6C4500`). The file is `tables\menu\<name>_<ll>.mnu` (`ll` the language's two letters, a
  u16 at `0x6C42C4`), else `<name>.mnu`.
- **game** The kinds' values (each function read in the disassembly, each kind made on a page and set, read and
  clicked in the lab): `chck` slot 23 SetCheck(bool) / 24 GetCheck, `+0x40` (`0x5DCCC0`); `bslb` slot 29
  SetTexts(text id off, on) (`0x5DCFD0`), 23 SetState (`+0x48`, the shown text by slot 14 SetTextID); `bsel` slot 29
  SetRange(first text id, last) (`0x5DCE30`), its option is its own text id `+0xC`, a click / Enter / Space steps
  it and wraps; `iten` and `edin` slot 29 SetRange(min, max), 21 SetValue(int) clamped into `+0x48` and printed
  into `+0x4C` with its own text as the format (`0x5DB0F0`: so it formats in its constructor and needs a text);
  `sldi` 21 SetValue(int) (`0x5DBF20`); `edit` 16 SetText (copied into `+0x40`), 18 GetText, 21 SetMaxLength (at
  most 63; `0x5DC8B0`), typed characters from IGraph slot 40; `rari` 23 SetChecked (its mark is the text 0x1A9);
  `rctt` 17 SetString(file): the old texture released, IGraph slot 1 CreateTexture(file, 0, 2, &`+0x40`)
  (`0x5DAA70`; full paths and paths from the game's folder both load), drawn by `0x5DAAA0` as the filled
  rectangle with it; `wins` 19 SetScroll(pixels) (`0x5D9AB0`), 27 SetRange(0, pixels) (`0x5D9AF0`; the load menu
  gives the last row's bottom - its height + 4, `0x5E2531`), Up / Down scroll by its bar's width, its Draw ends
  with SetViewport(itself) (`0x5E98A0`) and nothing puts it back for what is drawn after it; `orez` only sets that
  clip (a crop for what follows). A changing control's slot 10 / 11 answers its id: the menu's slot 9 gets it.
- **game** The text database (`0x6D8714`): `G_TextDatabase::GetText` `0x60FB40` and `GetTextOrNULL` `0x60FBA0`,
  thiscall(db, id) ret 4, a binary search over `{id, text}` (GetText answers a placeholder `0x638390` when not
  found). The loader hooks both and answers ids from `0x4D4D0001` with its own strings (pages' texts, options).
- **game** A control (made by `0x5E9BE0` from a record): `+4` id, `+8` flags, `+0xC` text id, `+0x10` text, `+0x14`
  x, y, w, h on the screen in pixels, `+0x24` x, y in its parent, `+0x2C` parent, `+0x34/+0x38` children. Flags: the
  low four bits the font, 0x10 shown, 0x20 enabled (both handed down to the children: `0x5E9D60` draws a control
  with `(flags | 0xFF000000) & parent's`), 0x40 right, 0x80 centred, 0x8000 takes the focus (the `item` and
  `butt` constructors set it), bit 31 focused (drawn underlined). The colour is each kind's own, read in its Draw
  (vtable slot 5) and seen in the lab: `stxt` `0x5DA090` grey, 0x100 white, 0x200 yellow, 0x300 black; `butt`
  `0x5DB7B0` black (grey when not enabled), 0x100 red, 0x200 white; `item` `0x5DADC0` black, 0x400 red. The game's
  own pages: labels `stxt` 0x30 (grey, plain), headings 0x31 (bold), buttons 0x80B1, sliders 0x8030.
- **game** Values: `slid`'s SetValue (slot 19, `0x5DBA50`) and `pbar`'s (`0x5DF350`) keep a float 0..1 at `+0x40`;
  the slider's GetValue (slot 20) reads it, the bar's is the base's (answers 0). The menu's `0x5EB070` /
  `0x5EB090` find a control by id and call those slots. A `pbar` is drawn by `0x5DAC70` as the loading screen's
  bar: grey segments, red ones for the value.
- **game** `GM_Menu::PridejComponentPod` `0x5EB7D0` (thiscall(menu, id, newId, growId, move1, move2, move3), ret
  0x18): the control `id` cloned by its kind's clone, put right under it with `newId`; `growId` grows by its height
  and the three others move down by it. The game grows its profile and save lists with it; the loader adds mods'
  items with it (the pause menu `gamemenu`: window 1, rows 102, 103, 108, 111 as `item`s, 30 high).

## Drawing

- **game** The text: `G_IndicatorsClass::OutText` `0x603880`, `thiscall(hud, text, float x, y, width, height, uint
  argb, int flags, int font, const char *end)`, `ret 0x24`, a float back in ST0 (callers pop it). `hud` is the HUD
  object `0x6BF980`, not a font (the friend's "font object"): the fonts are records in it. In **pixels**. Seen with
  `tap` 2026-10-13 at 1280x960: the HUD's money `"$0"` at (1216, 32), width 32, height 64, colour FFFFFFFF, flags
  4, font 1; the pause menu's buttons at x 88, width 347.2, height 48, colour FF000000, flags 2, font 1; its title
  in font 2. Flags (read in the code, 2 and 4 seen): 2 the text centred in the width, 4 its end at x + width, 8 x
  moved by half the height, 0x10 squeezed into the width, 1 a line under it. `end` (or 0) stops the text early. At
  most 100 glyphs a call (400 vertices on its stack; it starts `mov eax, 0x38E4`, `_chkstk`). Called by the loader
  for mods' texts (`host/draw.cpp`): the three fonts drawn and seen in a screenshot.
- **game** The fonts: records of 0x101C bytes from the HUD's start in 1.2 (from `hud + 0x5000` in 1.0), the font's
  number times that: `+8` a size factor, `+0x1C` on four floats a glyph; the character maps `0x6C4078 + 4 * font`
  (a byte to its glyph). 0: the plain font (messages, subtitles); 1: the bold one (the HUD's numbers, the menus'
  buttons); 2: the menus' handwriting. `TextSize` `0x6036D0`: `thiscall(hud, text, float height * the font's
  factor, int font, end) -> float width`, `ret 0x10`.
- **game** The 800x600 units of the menus and the HUD: `hud + 0x40C0` and `+0x40C4` are the pixels in a unit, x and
  y (1.6 at 1280x960; `+0x40B8`, `+0x40BC` the screen's size); `0x6C4500` is an x offset added in 1.2 (0 at 4:3;
  1.0 has none). `0x60E5C0` is the game's own text in units: the same arguments, scaled, then `OutText`.
- **game** Rectangles: `0x5E8F60` filled, `fastcall(ECX argb, EDX texture or 0, float x, y, w, h)`, `ret 0x10`;
  `0x5E8E00` its outline, `thiscall(ECX argb, float x, y, w, h)`. Both are two `IGraph` calls: `SetTexture` and
  `DrawPrimitive` (a triangle strip of 4, a line strip of 5) with 32-byte vertices `{x, y, z 0, rhw 1, diffuse,
  specular, u, v}` (FVF 0x1C4, `DrawPrimitiveUP`). Read in the decompile and the disassembly; the loader draws
  mods' rectangles and lines with the same two calls (seen in a screenshot). Hooking the two game functions
  restyles every built-in menu (**friend**).
- **game** The frame's 2D pass: the game gives its scene one callback, `0x5BD8D0` (`stdcall(int stage, ?, ?)`,
  `ret 0xC`); LS3D's render calls it with stage 1 (from `ls3d+0x64C26`) and, once the 3D frame is drawn, stage 2
  (`ls3d+0x65521`): then `G_IndicatorsClass::DrawAll` `0x5FB060` (the HUD), `GM_Menu::Draw` of the active menu,
  the console `0x4F6850`. Called twice a frame in the game and in the pause menu alike (`tap`). `DrawAll` first
  sets `IGraph` state 0 to 1 and the driver's (`I3D_driver*` at `0x647ED8`) state 3 to 0; the console's drawing
  saves and restores the driver's state 3 around itself. The loading screens are not this pass
  (`GM_LoadProgress::Present` `0x5E3D40`: `IGraph` slots 16, 12 clear, 13, 17, the menu drawn, 14, 15 present).
- **game** `IGraph*` is at `0x647EE0`; its vtable is `ls3d+0x9D500` (1.0: `ls3d+0x99498`). Slots, read in LS3DF's
  code: 3 (`+0x0C`) `SetTexture(tex)` (the device's `SetTexture(0, tex + 8)`), 15 (`+0x3C`) `Present` (the
  friend's, the game's loops call it last), 17 (`+0x44`) `SetState(id, value)` (0: blending: 0 off, 1 alpha
  blending with the alpha test, 3 the alpha test only; 1: the blend functions), 18 (`+0x48`) `GetState(id)`,
  22 (`+0x58`) `DrawPrimitive(type, count, vertices, format)` (`ls3d+0x6D640`: type 0 or 6 points, 1 a line list,
  2 a line strip, 3 a triangle list, 4 a fan, 5 a strip; format 1 the 32-byte vertex above, 0 a 24-byte one),
  23 (`+0x5C`) the same with indices, 38 the keyboard's read, 64 the frame limiter. The same slots in 1.0's LS3DF
  (compared by `SetTexture` and `DrawPrimitive`). **ours** The `IDirect3DDevice8*`: `ls3d+0x1C597C`.
- **friend** Drawing one's own on the device: in the `GM_Menu::Draw` hook (the front end) or late, before
  `Present` (in the game). Text as a glyph atlas (GDI into a DIB, one A8R8G8B8 texture), quads by `DrawPrimitiveUP`
  (`XYZRHW | DIFFUSE | TEX1`). Every render and texture-stage state touched must be saved and put back, or the
  game's own rendering breaks. Textures die at a device reset or alt-tab: make them again when the device pointer
  changes. Game.exe does not import `binkw32.dll`; LS3DF.dll does.

## The HUD

- **ours** The HUD object (`G_IndicatorsClass`, static, 0x40A4 bytes and more): `0x6BF980` (1.0: `0x658330`, 0x5000
  bytes bigger). **game** `+0x44A8`: the car the player drives or drove last (`RadarSetPlayerCar` `0x5FA500` sets
  it; the address TrackMafia read, `Game.exe+0x2C3E28`); `+0x4174` (`0x6C3AF4`): that car's speed in km/h as the
  speedometer takes it (32.08 beside the car's own 8.906 m/s).
- **friend** Its visibility flags: `[hud + 0x40A4]`; `0x47A070` sets them, `0x47A080` ors them, `0x47A0E0` saves
  and `0x47A2A0` restores them. The cinema bars: `0x5FA300` `HUD::LetterBox(bool show, int ms)`; `0x5FA050` builds
  the bars' geometry (the cutscene loader passes 0.5405405).

## Mission script commands

- **ours** `C_program::DecodeInstruction` `0x463960` (a keyword to its opcode).
- **friend** The rest of the chain: the arguments parsed at `0x461C70` (opcode to payload), the dispatch at
  `0x46D3FE` (its bound check `ja` at `0x46D406`), the default case `0x479996`, and `0x47999F`: the end of an
  instruction *without* moving to the next one, which is how a command waits. Payloads are allocated with the
  game's `malloc` (`0x6106FC` holds it) and freed by the game. New commands take free opcodes (his: `0xB1A`, the
  same as MafiaExtras' black bars, and `0xB1C..0xB21`), hooked into all three steps. This is the entry for "the
  engine's own commands from MCP" (docs/ROADMAP.md) and for mods adding script commands.

## His practice, worth keeping

- Before a patch, compare the bytes at the site with the expected ones; if another mod's jump is already there,
  follow it and chain instead of overwriting (his menu lives beside MafiaExtras this way). The loader's hook chains
  do this between its own mods; a foreign `.asi`'s patch is not looked for yet.
- A mod's drawing code guarded so that a fault turns the drawing off and logs it instead of taking the game down.
  (The loader does this for every mod's queued drawing: `host/draw.cpp`. Provoked 2026-10-13 by pointing the title
  font's character map `0x6C4080` at nothing while a mod drew in that font: caught, logged, the game went on.)
