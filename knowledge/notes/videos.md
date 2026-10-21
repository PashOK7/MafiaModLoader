# The game's videos: the logos, the intro, and why a replaced video does not play

Read in both builds' code (Ghidra, 2026-10-20) and measured in both labs with `tap` and `native.call` (the
addresses are 1.2's unless marked 1.0; `ls3d+` is LS3DF.dll's RVA). This is the game's own playing; since
2026-10-21 the loader plays the videos itself with FFmpeg (host/video.cpp, docs/MODDING.md "Videos": the end of
this note), and the game's players below get only what FFmpeg cannot play, or every video when FFmpeg is not there.

## Which files, in what order

- **1.2** plays four videos, each by `PlayVideo` (Game.exe `0x55D460`, `bool __thiscall(const char *name, float w,
  float h)`): `InitSystem` (`0x5BF5A0`) plays `logo1` (800 x 600), `logo3` (800 x 600), `logo2` (704 x 400) before
  the profile menu; the intro mission's script command `PLAY_AVI_INTRO` (`0x4787FD`) plays `intro` (800 x 440). The
  sizes are only hints for the frame's rectangle (below).
- `PlayVideo` tries **`<name>.avi` first**, then `<name>.bik`: `cd_find_game_file` (`0x5662E0`) looks for the
  `.avi`; found, it is opened; not found, or its opening failed, the `.bik` is looked for and opened. Neither: the
  function returns false and the game goes on. The strings are at `0x636620` (`bik`, `avi`, `.`).
- `cd_find_game_file` is `CreateFileA` on the bare name (the game's folder, its working directory), shared with
  nobody (share mode 0), then `X:\MafiaGame\<name>` on every drive C: to Z: (the disc's layout). The videos are
  loose files: never from the `.dta` archives, never through the game's `dtaOpen`, so a loader mod's `files\` does not
  reach them (host/files.cpp answers `dtaOpen`); Bink and DirectShow then open the path with their own file code.
- **1.0** has no Bink at all: its LS3DF.dll imports no `binkw32.dll` (a `binkw32.dll` 1.5p and a `Logo3.bik` in
  the 1.0 lab are leftovers nothing loads; only the ASI loader's `dinput8.dll` names `binkw32` as one of the DLLs it
  can stand in for). `InitSystem` (`0x5FA320`, inlined) plays `logo1.avi` and `logo2.avi`, each only when
  `m10CdFind` (`0x623FC0`, the same search as 1.2's) finds it; `intro.avi` comes from `0x5C652D`. The 1.0 lab has
  none of them, so its game starts without logos.
- The 1.2 lab has both kinds: `logo1`, `logo2`, `intro` as `.avi` and `.bik`, `Logo3` as `.bik` only.

| file (1.2 lab) | codec | size | frames |
|---|---|---|---|
| `logo1.avi`, `logo2.avi`, `intro.avi` | Indeo 5 (`IV50`), MS ADPCM 44.1 kHz stereo | 640x480, 704x400, 800x420 | 25 a second |
| `logo1.bik`, `Logo3.bik` | Bink 1 `BIKi` (Bink audio, `Logo3` silent) | 640x480 | 25 a second |
| `logo2.bik`, `intro.bik` | Bink 1 `BIKi` | 720x400 | 25 a second |

## Who plays them: LS3D's IShow

`IGraph` slot 0 (`IGraph::CreateIShow`, ls3d+6C1D0; 1.0 ls3d+68AD0) makes an `IShow`. Its slots: `+0x04` Release,
`+0x0C` Open (`LS3D_RESULT __stdcall Open(IShow *, const char *name, unsigned flags)`, ls3d+73BA0; 1.0 ls3d+6FB70),
`+0x10` Close, `+0x14` Play, `+0x20` SetRect, `+0x50` the full-screen loop (ls3d+74D70; in 1.0 `+0x4C`,
ls3d+70A10: 1.0's vtable has one slot fewer before it, so its frame's draw is `+0x48` against 1.2's `+0x4C`).
`PlayVideo` (and 1.0's inlined copy): CreateIShow, Open(path, 0), the rectangle from the screen's width and height
(`IGraph` `+0x70`, `+0x74`; narrowed on screens wider than 2:1 and 3:1), SetRect, Play,
the full-screen loop, Release.

IShow's open chooses by the name's extension (`strstr`):

- `.ogg`: Vorbis through `dtaOpen` (the music; also 1.0).
- `.bik` (1.2 only): `BinkSetSoundSystem(BinkOpenDirectSound, LS3D's DirectSound)`, `BinkOpen(name, 0)`; then
  `CTextureRenderer::SetMediaType(width, height, ishow)` (ls3d+739B0) with the Bink's own size.
- `.avi`: IShow's DirectShow graph setup (ls3d+74F40; 1.0 ls3d+70BB0): a DirectShow graph (`CoCreateInstance`
  of the filter graph) with LS3D's own renderer (`CTextureRenderer`, a `CBaseVideoRenderer`) and the file's source
  filter, connected by the graph (it puts in the AVI Splitter, the decoder, a colour converter); the AVI Splitter's
  audio pin goes to DirectSound's renderer.
- anything else: `IGraphBuilder::RenderFile` (DirectShow's own renderers, not LS3D's textures).
- A failure is only a `dbgPrintf` of an error with the name: seen only by a debugger. Nothing on the screen, nothing in a
  log: the video is just not there.

### The frames

`CTextureRenderer::SetMediaType` cuts the frame into tiles of at most LS3D's largest texture (the device's caps)
(1.2 ls3d+1C593C, ls3d+1C5588; 1.0 ls3d+1C1640, ls3d+1C1528: 16384 x 16384 in both labs, Direct3D 9 through
dxwrapper) and makes an LS3D texture for each, of `ishow.flags & 0x400 | 0x8000`. **No size limit**: a video
larger than the device's largest texture is drawn from several. LS3D rounds a texture to powers of two (a 640-wide
video's tile has a 2048-byte pitch in 16 bits). The format is 16-bit (R5G6B5 when the device has it) unless the
Open's flags hold `0x400` (LS3D's true-colour texture format); `PlayVideo` passes 0: **every video is shown in 16-bit
colour**, so an upscaled one bands in gradients.

- Bink: each frame IShow's frame draw (ls3d+74B70): `BinkWait`, `BinkDoFrame`, ls3d+738A0 copies the frame into
  every tile with `BinkCopyToBufferRect` (surface type by the tile's format: 32-bit 3, R5G6B5 10, X1R5G5B5 9,
  `| BINKCOPYALL`), `BinkNextFrame` until the last frame.
- DirectShow: `CTextureRenderer::DoRenderSample` (ls3d+755C0; 1.0 ls3d+713F0) on DirectShow's streaming thread.
  `CheckMediaType` (ls3d+75480) takes only video in a VIDEOINFOHEADER of RGB565 (16-bit textures) or RGB32 (1.0:
  16 or 32 bits): the graph needs a decoder (or a converter after it) that gives RGB.
- The skip: the full-screen loop draws until the video ends (Bink: its frame number reaches its count;
  DirectShow: the graph's completion event) or `IGraph` `+0x9C` reports a key: any key skips.
- The videos are played before the frame loop (InitSystem) or inside a script command: the loader's game-thread
  methods (and `screenshot`) do not answer while one plays; `where` shows the loop (ls3d+74D70).

## Bink: what the game's binkw32.dll reads

- The 1.2 lab's `binkw32.dll` is **Bink 1.9c** (RAD, 2008; 171008 bytes). `BinkOpen` reads the header and accepts
  only four revisions: `BIKf`, `BIKg`, `BIKh`, `BIKi` (`cmp eax, 'BIKf'..'BIKi'` at `0x1800D780` of its image);
  anything else is `BinkSetError("Not a Bink file.")` and a null handle. So it refuses Bink 1's last revision
  `BIKk` and every Bink 2 file (`KB2a`..`KB2n`, which also need RAD's other DLL, `bink2w32.dll`).
- **RAD Video Tools 2026.09** (radgametools.com, the free tools, `radvideo64.exe`) writes the header in one place
  (`0x1400BDDC2`): the compressor's `/V#` below 101 writes `BIKi`, 101 to 199 `BIKk`, 200 and up Bink 2 (`KB2j`,
  `KB2m`, `KB2n`; a licence is needed). Its list shows "Bink 1" (`/V100`) and "Bink 1.1" (`/V101`): **"Bink 1.1"
  and Bink 2 make files this game cannot open**; "Bink 1" makes a `BIKi`.
- Nothing else in LS3D limits a Bink: the size is the file's (tiles above), the frame rate the file's (`BinkWait`),
  the audio Bink's own through DirectSound.

## Measured (2026-10-20)

1.2 lab, the game's usual start (`tap` on IShow's open, ls3d+73BA0): `logo3.bik` opened (0); `logo2.avi` failed (0x80000001:
DirectShow has no Indeo 5 decoder here: `ir50_32.dll` is in SysWOW64 but `vidc.iv50` is not registered) and
`logo2.bik` was opened after it (0). Then through `native.call` of `PlayVideo` from the main menu, with taps on
BinkOpen, BinkCopyToBufferRect, SetMediaType and DoRenderSample:

| build | file | result |
|---|---|---|
| 1.2 | `logo1.bik` copied (640x480 `BIKi`) | BinkOpen ok, 126 frames copied (pitch 0x800: a 16-bit tile 1024 wide) |
| 1.2 | `logo1` upscaled 2x to 1280x960, MJPEG + PCM in AVI | DirectShow: SetMediaType 1280 x 960, 126 frames rendered |
| 1.2 | the same as H.264 in AVI | Open 0x80000001 (Windows' H.264 decoder gives no RGB to LS3D's renderer); PlayVideo false |
| 1.0 | `logo1.avi` (Indeo 5) | Open 0x80000001: the original 1.0 logos cannot play on this Windows |
| 1.0 | the 1280x960 MJPEG AVI | Open 0, SetMediaType, 126 frames rendered |

## Why a modder's upscaled intros do not play

In order of likelihood, each a silent failure:

1. **The file is not one binkw32 1.9c reads** (1.2): an upscale encoded as "Bink 1.1" (`BIKk`) or Bink 2 (`KB2*`)
   gets "Not a Bink file."; the game skips the video and says nothing. Encode as "Bink 1" (`/V100`: `BIKi`).
2. **The `.avi` beside it is played instead** (1.2): `PlayVideo` opens `<name>.avi` first. On a machine whose
   DirectShow decodes Indeo 5 (a codec pack, LAV Filters) the original AVI plays and the new `.bik` is never opened.
   Take the `.avi` out (or replace it too).
3. **The wrong place or name**: loose in the game's folder (`logo1.bik`, `logo2.bik`, `logo3.bik`, `intro.bik`), not
   in an archive, not in a mod's `files\`; nothing else may hold the file open (share mode 0).
4. **1.0** plays no `.bik` at all: only `logo1.avi`, `logo2.avi`, `intro.avi` through DirectShow, which must decode
   it to RGB (MJPEG works with Windows alone; H.264 in AVI does not; Indeo 5, the originals, does not either).

## Lifting the limits

- A later Bink 1 runtime that reads `BIKk` (not tested here: none is on this machine) would have to keep the
  eleven exports LS3D imports and the `BINK` fields it reads (Width `+0`, Height `+4`, Frames `+8`, FrameNum
  `+0xC`); it comes with RAD's licensed SDK or with other games: not ours to ship.
- **Done (2026-10-21): the loader plays them** (host/video.cpp). Its hooks, in both builds: IShow's open
  (game_build.h showOpen: a video's name taken over when FFmpeg plays one of its files), IShow's close (showClose:
  the loader's video dropped with the game's object) and the full-screen loop (showRenderUntilKey: the
  loader's own loop in its place, doing each turn what the game's does: ProcessWinMessages, keyboardUpdate, ReadKey,
  Clear, BeginScene, the picture, EndScene, Present), and `cd_find_game_file` (cdFind: "found" for a name the loader
  plays from a mod). The hooks are the innermost links: the start's logo skip (start.cpp) stays in front. The
  device's states are kept in a Direct3D 9 state block around the loader's quad, so LS3D's shadow copies of them
  (its cached texture-stage and render states) stay true. The order of the files, the formats and the bridge's methods:
  docs/MODDING.md "Videos". Seen in both labs (the journal, 2026-10-21).
- IShow's vtable differs between the builds: 1.2's has one more slot before the full-screen loop (`+0x50`;
  1.0 `+0x4C`), and IGraph's slots from `+0x64` on are 4 bytes lower in 1.0 (no GetResInitDesc): keyboardUpdate and
  ReadKey are `+0x98`, `+0x9C` in 1.2, `+0x94`, `+0x98` in 1.0. The loader calls them by their addresses
  (game_build.h keyboardUpdate, readKey).
