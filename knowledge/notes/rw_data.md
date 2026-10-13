# rw_data.dll: the game's file layer and the DTA archives

From MafiaReverse (`rw_data_notes.md`, `WORKLOG.md`, 2026-05; read-only work on a clean 1.2 install, the archive
parsing verified by a scanner over every present archive: 27018 entries). Not yet checked through the loader.

`rw_data.dll` is what `Game.exe` and `LS3DF.dll` read every file through: loose files on disk and entries of the
encrypted `.dta` archives. A mod that wants to read a game file the way the game does (an archive entry by its
path) calls these exports.

## Exports (stdcall)

| Export | Prototype (inferred) | Notes |
|---|---|---|
| `dtaOpen@8` | `uint32_t dtaOpen(const char *path, char forceDirect)` | a handle index or `0xFFFFFFFF`; a 0x8000 read cache |
| `dtaRead@12` | `uint32_t dtaRead(uint32_t h, void *dst, uint32_t size)` | loose files and archive entries (blocks, compression) |
| `dtaSeek@12` | `uint32_t dtaSeek(uint32_t h, uint32_t offset, int origin)` | origin 0 begin, 1 current, 2 end; new position |
| `dtaClose@4` | `uint32_t dtaClose(uint32_t h)` | commits temp writes in some modes |
| `dtaOpenWrite@8` | `uint32_t dtaOpenWrite(const char *path, uint32_t flags)` | flag 0x100: write to a temp file, moved on close |
| `dtaWrite@12` | `uint32_t dtaWrite(uint32_t h, const void *src, uint32_t size)` | |
| `dtaCreate@4` | `int *dtaCreate(const char *path)` | an archive object (see below) |
| `dtaDelete@4` | `bool dtaDelete(const char *path)` | `DeleteFileA` |
| `dtaGetTime@12` | `bool dtaGetTime(uint32_t h, FILETIME *out, char mode)` | |
| `dtaSetDtaFirstForce@0` | `void ()` | archives before loose files |
| `dtaBin2Text@8`, `dtaDumpMemoryLeaks@4` | | minor |

Handles index a global table of 0x58-byte entries (first dword `-1` when free), guarded by one critical section.

## The archives

`Game.exe` keeps the archive objects in an array at **0x647BE4** (a Ghidra comment says 0x647BE0: one dword early)
and initializes each with two keys through the object's vtable `+0x0C` (`rw_data` 0x100013B0, thiscall, `ret 8`).
That method XORs them: `k1 ^= 0x39475694`, `k2 ^= 0x34985762`; the cipher is then dword0 ^= k1, dword1 ^= k2 on
every 8-byte block.

| # | Archive | Key 1 | Key 2 |
|---:|---|---:|---:|
| 0 | a8.dta | 0x6A63FA71 | 0xEC45D8CE |
| 1 | b8.dta (absent) | 0x79E21CDB | 0xB60F823F |
| 2 | a2.dta | 0x1417D340 | 0xB6399E19 |
| 3 | a6.dta | 0x728E2DB9 | 0x5055DA68 |
| 4 | a0.dta (absent) | 0x7F3D9B74 | 0xEC48FE17 |
| 5 | a1.dta | 0xE7375F59 | 0x0900210E |
| 6 | a3.dta | 0xA94B8D3C | 0x771F3888 |
| 7 | ac.dta | 0xA94B8D3C | 0x771F3888 |
| 8 | a4.dta | 0xA94B8D3C | 0x771F3888 |
| 9 | aa.dta | 0xD4AD90C6 | 0x67DA216E |
| 10 | a5.dta | 0x4F4BB0C6 | 0xEA340420 |
| 11 | a7.dta | 0xF4F03A72 | 0xE266FE62 |
| 12 | a9.dta | 0x959D1117 | 0x5B763446 |
| 13 | ab.dta (absent) | 0x7F3D9B74 | 0xEC48FE17 |

Layout (after the magic `ISD0`, decrypted):

- header: entry count, entry table offset, entry table size (= count x 0x1C), a flag-like dword;
- entry (0x1C bytes): `+0x00` lookup hash `(path_len << 16) | byte_sum` of the uppercase path; `+0x04` metadata
  offset; `+0x08` payload offset (and the metadata's end); `+0x0C` the last 16 bytes of the uppercase path;
- metadata: a 0x20-byte encrypted header (`+0x10` unpacked size, `+0x14` block count, `+0x18` low 15 bits the name's
  length, bit 0x8000 payload encrypted), then the full name, encrypted the same way;
- payload: blocks, each with a 4-byte header (low 16 bits the packed size); the block's first byte is its mode:
  below 8 and not 1 raw, 1 an LZ bit stream (scripts, text), 0x0C..0x20 in steps of 4 audio predictors
  (`rw_data` 0x10004250, not ported).
