# The game's interface: menus, drawing, the HUD, script commands

From pet1kk's notes (2026-10-13), the reverse behind his front-end mods for the 1.2 game (`menu_bertone.asi`,
`menu_pomsta.asi`: a main menu drawn by the mod over a Bink video, the game's own menu kept alive underneath and
its click handler called for every action). His addresses were all used in working code. Each fact says how it is
known here: **pet1kk** (his notes only), **game** (also checked in our running 1.2 game or its bytes), **ours**
(found by us before, the same address in his notes).

## Menus

- **ours** The active menu: `0x6BD890`; the main menu's vtable `0x625EB8`; the profile menu's `0x6279DC`; a menu
  run modally: `GM_Menu::ExecuteMenu` `0x5EBA40`; the profile manager (`G_LoadSaveClass`) `0x6D4628`, its profiles
  `+4/+8`, 0x54 bytes each, the name at `+0x10`. A click: the menu's vtable slot 9 (`+0x24`) with the button's id.
- **pet1kk** `GM_Menu::Draw` `0x5EA7E0`, `thiscall(menu, flags)`, `ret 4`: hooked at its entry, a mod draws its
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
- **pet1kk** The menus' input tick `0x5EA8C0`, `thiscall(menu, rx, ry, rz, buttons, key, shift, ms)`, `ret 0x1C`,
  called from the modal loop at `0x5EBD22`: `key` a DirectInput code, `buttons` bit 0 the left mouse button.
  Redirecting that one call takes the input over while a menu is on top. The cursor, in pixels: `0x6BD8A0` (x),
  `0x6BD8A4` (y) (both 0 with no menu open: **game**). This is the way to the menus' mouse for `input`.
- **pet1kk** The layout pass: `0x5E9DA0`, `thiscall(root, flags)`, recursive (a control's vtable slot 6). A
  control draws in its vtable slot 5. The menu scene's renderer `0x5E3D40` (its call returns to `0x5E3D85`) also
  draws the loading screens, with no menu open.
- **pet1kk** Controls by vtable: window `0x6260F8`, `0x626200`; text `0x626280`, `0x626300`, `0x626380`,
  `0x626400`; a wrapped text box `0x627210` (its lines in a buffer at `+0x44`, split by `~`); line `0x626480`;
  rectangle `0x626500`, `0x626580`; button `0x626900`, `0x626980`, `0x626880`, `0x626B00` (a button draws at
  `0x5DB7B0`); list item `0x626700`, `0x626780`, `0x626800` (draws at `0x5DADC0`: the profile list, yes/no);
  slider `0x626A00`, `0x626A80`; selector `0x626C80`, `0x626D00`; edit box `0x626B80`; video `0x626E00`.
- **pet1kk** `.MNU` files: `tables\menu\<name>_<lang>.mnu`, else `tables\menu\<name>.mnu` (the loader `0x5EA370`):
  a header, then controls with a type tag, x/y/w/h in 800x600 units, a text id, flags, children.
- **pet1kk** The texts' file: `tables\textdb_<lang>.def`: u32 count, u32 0, count times (u32 id, u32 offset), then
  the strings, 0-terminated, in the language's code page (Czech 1250, French and English 1252: measured on "Piège"
  in the French 1.0, 2026-10-14). (Ours: the database in memory `0x6D8714`, `GetTextOrNULL` `0x60FBA0`.)
- **code** The language: `tables\lang_<ll>.def` (`lang_cz.def`, `lang_en.def` the fallbacks), a record of 0x238
  bytes read by 0x5F7000 (1.0 0x549060) to 0x6C42C0 (1.0 0x661C68): "Lang", the two letters at `+4`, the language's
  name at `+8` ("English", "French"), its fonts' suffix at `+0x28` ("_lat"), then the fonts' map of the bytes.
- **pet1kk** Profiles: `0x605570` `thiscall(manager, index)` selects one, `0x6055C0` `thiscall(manager, name)`
  creates and selects one (1: done). The profile menu runs at the start (the call at `0x604FAF`) and from "change
  profile" (`0x6050DF`). The file name formats are pushed at `0x6052D4`, `0x605693`, `0x60576E`, `0x60583B`,
  `0x605A3C` (profiles: `push 0x6395F4`) and `0x60578E`, `0x605BBE`, `0x6060CA`, `0x6061B3`, `0x606F11` (saves:
  `push 0x639618`): pointing those at another format moves every save file.
- **pet1kk** The menus' sounds (LS3D sound objects): `0x6BD8B8` open, `0x6BD8BC` close, `0x6BD8C0` change. The
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
- **game** Options is `optionsb` in both builds (the string at `0x5E0900` in 1.2): window 2, rows 100 Controls, 101
  Audio and Video, 400 Back (`butt` in 1.2, `item` in 1.0); the loader's Mods row goes under 101 (host/panel.cpp).
  Audio and Video is `optionsa` (its sliders `scrl` in 1.0, `slid` in 1.2). A window's controls start about 44 units
  under its top (the title's band), and the game's pages leave about 25 under their last row (seen on both builds'
  pages, 2026-10-17).

### 1.0's menus (2026-10-14; st0rm94's annotated 1.0 decompile and its `structs/gui_*.md`, each address read in 1.0's code; used by `host/menus10.cpp`)

- **game** A screen (`GM_Menu`, the base 0x48 bytes; `gui_menu_ctor` `0x56AE50` thiscall(screen, parent, id) ret 8
  puts the base vtable `0x63D584`): `+0x18` the screen under it, `+0x20/+0x24` its controls (pointers), `+0x2C`
  the focused one, `+0x31` "the mouse moved" (over 10 pixels), `+0x34/+0x38` the cursor it last saw (ints),
  `+0x40` flags (0x10000: the screen under it drawn too; 0x80000: the menu camera), `+0x44` the id it gives its
  parent when it closes. The active one `0x67A4D8`. Its vtable has 6 slots: 0 the deleting destructor (ends in
  `gui_menu_dtor` `0x56AF10`), 1 OnExchange (rebuilt in place), 2 Destroy (`0x56E8A0`: the controls and itself),
  3 OnCloseChild(child id, result) (the base `0x56E8F0` re-arms the input, ret 8; the classes' take their own
  children's ids), 4 MenuClose(id, result) (`0x56E930`: the parent made active, its slot 3, then slot 2), 5 Tick.
- **game** **No command handler.** `gui_menu_tick` `0x56EA70`, thiscall(screen, key flags, key, x, y, wheel,
  buttons, ms) ret 0x1C: the blink, the layout of controls flagged 0x20, the mouse (hover focus, then the focused
  control's slot 6 with the buttons) or the keyboard (the focused control's slot 5; Up/Down walk by id, Left,
  Right, Tab by the list), and it returns the id of the control activated. Every screen's Tick calls it and
  switches on the answer (an Esc is `id == 0 && key == 1`). The loader hooks it: an answer of its own is a command,
  an item's id is delivered to its mod. The menus' loop (`gui_menu_execute_modal` `0x56CCC0`) calls the frame
  limiter first, then reads the mouse (IGraph `+0xCC`), adds its moves to the cursor's floats `0x672618/0x67261C`
  (clamped), and calls the active screen's Tick with `ftol` of them; a non-zero answer ends the loop.
- **game** The designs: `tables\menu.def`, read whole at the start into `0x672630` (0x7E90 bytes): groups of
  0x24-byte records `{id, tag, x, y, w, h (800x600), text id, flags, colour}` ending in `stop`; `gate` and `base`
  records steer a group, `vers` is skipped. `gui_menu_build_components_from_design` `0x56E500`, thiscall(screen,
  table, design, gate) ret 0xC, sets the scale (`0x67A4C0/C4`: the screen's size / 800, / 600), makes a control a
  record by `gui_widget_create_from_design` `0x56AFB0` (fastcall(record, &parent, raw)) and focuses the first
  selectable one. The factory's container arms (`wind`, `wint`, `winc`, `swnd`, `frac`, `frmt`) write the
  out-slot: such a control is the parent of every record after it (no `end`). The builder adds a `base` record's
  offsets into the table's records themselves (the loader builds its pages from a copy).
- **game** A control (the base 0x38 bytes, `gui_widget_ctor` `0x5704B0`): `+4` id, `+8` flags (1 selectable,
  0x10 focused, 0x20 to be laid out, 0x100 hidden; the factory adds 1 or 3), `+0xC` text id (resolved by
  `GetText` into `+0x30` at its making), `+0x10` x, y, w, h in pixels, from its parent's client origin (`+0x38`,
  `+0x3C` of a container, on the screen; a `wint`'s is 40 units under its top), `+0x20` the record's flags and
  `+0x24` its colour (a text: its font and ARGB, 0 the game's own dark; a check box: its state; a number: its
  value), `+0x28` the colour as a float, `+0x2C` a float (a slider's and a bar's 0..1), `+0x30` the text drawn,
  `+0x34` the parent. Its vtable has 10 slots: 2 Draw, 3 ForceUpdate, 4 IsOverActive(x, y), 5 Key, 6 Mouse, 7
  Tick, 8 SetText (an edit box copies, `rctf` loads the file as a texture), 9 a getter. The 53 kinds by vtable:
  `host/menus10.cpp` (`kKinds`, from `structs/gui_widget.md`, seen in `menu.state`).
- **game** The kinds' values, read in their code: `scrl` (the slider, `0x572B80` its mouse) and `pbar` `+0x2C`;
  `chck` `+0x20` (Space or a click toggles it, sending no id); `iten` `+0x24` printed as "%s %d" (its key and
  mouse return its id, a left first takes 2: the screen adds 1); `cmbs` the text ids from `+0x3C` (the record's
  flags) to `+0x40` (its colour), the one shown `+0xC`, picked from the game's drop-down screen (`combomenu`,
  `0x575C50`) that writes `+0xC` when it closes; `edtl` a 32-byte buffer at `+0x38` (31 characters; `edit` 10;
  typed from IGraph `+0x9C`); `swnd` scrolls to `+0x44` in `-(+0x28)..0` (the screens scroll it themselves; the
  load menu by the wheel).
- **game** `gui_menu_clone_component_under` `0x56C1D0`, thiscall(screen, tag, id, newId) ret 0xC: a copy of
  control `id` (made as `tag`, 11 tags have a copy: `item` among them) one row under it, after it in the vector;
  the profile screen grows its list so and then moves its buttons and grows its windows itself. The game's
  operator new `0x624934`.
- **game** The screens by vtable (their designs): the main menu `0x63DE74` (6), the pause menu `0x63DEA4` (7: 102
  Exit, 103 Load, 108 Options, 111 Resume, as 1.2's `gamemenu`), the profiles `0x63DF6C` (4: rows 100 + i, its
  `+0x48` a pending action), the freeride `0x63DFCC` (14: 21, 22, 23 the densities, 100 the kind, 201 start; its
  kind `+0x48`), the options `0x63DEDC` (9), audio and video `0x63DEC4` (8), the load menu `0x63DFB4` (12); every
  one in `host/menus10.cpp` (`kScreens`).

## Drawing

- **game** The text: `G_IndicatorsClass::OutText` `0x603880`, `thiscall(hud, text, float x, y, width, height, uint
  argb, int flags, int font, const char *end)`, `ret 0x24`, a float back in ST0 (callers pop it). `hud` is the HUD
  object `0x6BF980`, not a font (pet1kk's "font object"): the fonts are records in it. In **pixels**. Seen with
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
  restyles every built-in menu (**pet1kk**).
- **game** The frame's 2D pass: the game gives its scene one callback, `0x5BD8D0` (`stdcall(int stage, ?, ?)`,
  `ret 0xC`); LS3D's render calls it with stage 1 (from `ls3d+0x64C26`) and, once the 3D frame is drawn, stage 2
  (`ls3d+0x65521`): then `G_IndicatorsClass::DrawAll` `0x5FB060` (the HUD), `GM_Menu::Draw` of the active menu,
  the console `0x4F6850`. Called twice a frame in the game and in the pause menu alike (`tap`). `DrawAll` first
  sets `IGraph` state 0 to 1 and the driver's (`I3D_driver*` at `0x647ED8`) state 3 to 0; the console's drawing
  saves and restores the driver's state 3 around itself. The loading screens are not this pass
  (`GM_LoadProgress::Present` `0x5E3D40`: `IGraph` slots 16, 12 clear, 13, 17, the menu drawn, 14, 15 present).
- **game** `IGraph*` is at `0x647EE0`; its vtable is `ls3d+0x9D500` (1.0: `ls3d+0x99498`). Slots, read in LS3DF's
  code: 3 (`+0x0C`) `SetTexture(tex)` (the device's `SetTexture(0, tex + 8)`), 15 (`+0x3C`) `Present` (pet1kk's, the game's loops call it last), 17 (`+0x44`) `SetState(id, value)` (0: blending: 0 off, 1 alpha
  blending with the alpha test, 3 the alpha test only; 1: the blend functions), 18 (`+0x48`) `GetState(id)`,
  22 (`+0x58`) `DrawPrimitive(type, count, vertices, format)` (`ls3d+0x6D640`: type 0 or 6 points, 1 a line list,
  2 a line strip, 3 a triangle list, 4 a fan, 5 a strip; format 1 the 32-byte vertex above, 0 a 24-byte one),
  23 (`+0x5C`) the same with indices, 38 the keyboard's read, 64 the frame limiter. The same slots in 1.0's LS3DF
  (compared by `SetTexture` and `DrawPrimitive`). **ours** The `IDirect3DDevice8*`: `ls3d+0x1C597C`.
- **pet1kk** Drawing one's own on the device: in the `GM_Menu::Draw` hook (the front end) or late, before
  `Present` (in the game). Text as a glyph atlas (GDI into a DIB, one A8R8G8B8 texture), quads by `DrawPrimitiveUP`
  (`XYZRHW | DIFFUSE | TEX1`). Every render and texture-stage state touched must be saved and put back, or the
  game's own rendering breaks. Textures die at a device reset or alt-tab: make them again when the device pointer
  changes. Game.exe does not import `binkw32.dll`; 1.2's LS3DF.dll does (1.0's
  has no Bink: `knowledge/notes/videos.md`).

## The HUD

- **ours** The HUD object (`G_IndicatorsClass`, static, 0x40A4 bytes and more): `0x6BF980` (1.0: `0x658330`, 0x5000
  bytes bigger). **game** `+0x44A8`: the car the player drives or drove last (`RadarSetPlayerCar` `0x5FA500` sets
  it; the address TrackMafia read, `Game.exe+0x2C3E28`); `+0x4174` (`0x6C3AF4`): that car's speed in km/h as the
  speedometer takes it (32.08 beside the car's own 8.906 m/s).
- **code** Its layout (read 2026-10-25 in both builds; mods/fusion_fix): `G_IndicatorsClass::ResolutionChanged`
  (1.2 0x601520, 1.0 0x5532E0, thiscall(hud), plain ret) computes the screen's width (`hud + 0x40B8`, 1.0 `+0x90B8`),
  the scale of the 800x600 units (`+0x40C0`/`+0x40C4`: x from the width times 1/800, y from the height times 1/600)
  and the gauges' places from constants in its own code; `G_LoadSaveClass::Init` calls it once at the start, before
  the loader's mods load. It only computes: called again, the HUD follows its constants as they are then (Fusion
  Fix patches the 1/800 to 1/(600 x aspect): square units, the HUD 600 x aspect wide). 1.2 also lays it out for a
  Matrox triple head there (`hudXOffset` = width / 3), Fusion Fix jumps over that.
- **pet1kk** Its visibility flags: `[hud + 0x40A4]`; `0x47A070` sets them, `0x47A080` ors them, `0x47A0E0` saves
  and `0x47A2A0` restores them. The cinema bars: `0x5FA300` `HUD::LetterBox(bool show, int ms)`; `0x5FA050` builds
  the bars' geometry (the cutscene loader passes 0.5405405).

## Mission script commands

- **ours** `C_program::DecodeInstruction` `0x463960` (a keyword to its opcode).
- **pet1kk** The rest of the chain: the arguments parsed at `0x461C70` (opcode to payload), the dispatch at
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

## The HUD's parts for mods (read and seen 2026-10-16, both labs; host/hud.cpp)

- **game** The HUD's flags (`hud + 0x40A4`, 1.0 `+ 0x90A4`) say what DrawAll draws: `0x80` the compass, which points
  at the frame `hud + 0x4278` (1.0 `+ 0x9270`: not 0x5000 further as the rest, 8 less) holds; the scripts'
  SETCOMPASS (1.2 0x46E1D8, 1.0 0x5BC005) is CompassSetDestination(frame), AddFlag(0x80), SetPlayerNaviPoint(frame)
  (a global nothing reads), and with no frame RemoveFlag(0x80). `0x80000`: the radar (DrawAll 0x5FDD5F), drawn from
  the mission's radar map: set in `freeitaly` (which has none) DrawAll read a null (LS3DF+0x2C8FA). `0x801` hides
  the lot. The freeride's flags: `0x08000024`.
- **game** The console (ConsoleAddText): five lines in a ring, 64 bytes each copied with `strncpy` (a 64-byte text is
  not ended), the colour's RGB, each kept 5 s (`0x40A00000`); drawn bottom left in both builds. MissionHelpAdd's
  line is above the crosshair, in the middle.
- **game** Pictures: IGraph slot 1, `CreateITexture(diffuse, alpha, flags, ITexture **out)` (stdcall, `0` on
  success), looks in `maps\`; the HUD's own: `"1numbers.tga", 0, 2`, `"2blood.bmp", "2blood+.bmp", 4`,
  `"2sight.bmp", "2sight+.bmp", 0x404`; the menus' GM_Picture `(file, 0, 2)`. Flag 4 loads the alpha file (LS3D
  sets it itself when an alpha file is given without it, with a warning). An ITexture's Release is its vtable's
  slot 0; its width and height are u16 at `+0x28`, `+0x2A` (128 x 128 for `2sight.bmp`, 128 x 16 for
  `1numbers.tga`, read). Drawn as the HUD draws its textured rectangles (SetTexture, a strip of FVF 0x1C4 vertices):
  the vertex colour multiplies the texture (a white `.tga` drawn red), its alpha blends.
- **ours** LS3D's dummy frame (I3D_FRAME_TYPE 6, made by the driver's CreateFrame, placed by SetPos, not linked) is
  a target the compass and the radar take: they read its world matrix (UpdateWMatrixProc).
