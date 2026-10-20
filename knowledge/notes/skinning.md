# People's skinning in LS3D

How LS3D skins, lights and draws a single mesh (`SNGM`) and a single morph (`SMMR`: the people, their faces morphed),
read in both builds' LS3DF.dll (2026-10-20). The names are the demo's `LS3D.dll` CodeView publics (re/demo, a build
of 2002-07-04 with the same code; knowledge/notes/external.md). Offsets are the same in both builds unless said.

## The objects

- **The single mesh** (an I3D_object): `+0x1D8` flags (`0x10000` an instance: `+0x08` the mesh whose LODs it uses, its
  PoseLod and m_AddFGroups go to that one's; `0x40` no world matrix; `0x80` the whole LOD lit one colour), `+0x1E0`
  its I3D_mesh, `+0x1E4 + 4 * lod` the LOD's record (`lod`), `+0x21C` the joints bound, `+0x220` their array (by the
  joint's id `+0x180`: f(this) 1.2 ls3d+54C20 collects the joints (frame type 10) of its subtree, 64 at most, and sets
  each joint's `+0x208` to the mesh), `+0x224` the bones' stamp (raised when a joint moves), `+0x22C` the morph
  (`SMMR`; bit 0 on: the morph's own LOD step writes the mesh's own vertices before the skinning), `+0x214`, `+0x218`
  the alpha factors, `+0x210` the lights' factor, `+0x144` the inverse world matrix (built when `+0x1D8` bit
  `0x100000` is clear).
- **The I3D_mesh**: `+0x08` the LODs, `+0x0C + 4 * lod` the MeshLevel: `+0x08` its vertices, `+0x0C` its own
  vertex buffer (position, normal, uv: 0x20 bytes; system memory), `+0x14` its first vertex there, `+0x1C` its face
  groups (`+0x04` the material: `+0x20..0x28` diffuse, `+0x14..0x1C` ambient, `+0x2C..0x34` emissive, `+0x38` opacity),
  `+0x38` its LOD distance, `+0x3C` flags (`8`: an environment map, a second uv set made by EnvMapCoords).
- **The LOD record** (`lod`): `+0x04` the lights' stamp it was lit with (`-700`: light again), `+0x08`, `+0x0C` the
  lights' sum, `+0x10` the drawn vertex buffer (pooled; FVF 0x142, or 0x242 with an environment map), `+0x18` its
  first vertex there (the draw's BaseVertexIndex), `+0x2C` the bones moved, `+0x2D` the alpha to write again, `+0x2E`
  the alpha written, `+0x30` the skin (`+0x08` the bone records, `+0x0C` the vertices weighted by no bone: the LOD's
  last), `+0x34` the cache (POS_NORM, 0x18 bytes a vertex: what the skinning writes and the lighting reads), `+0x3C`
  its first vertex there, `+0x40` the bones' stamp it was skinned with.
- **A bone record** (0x68 bytes, one a joint, in the joints' ids' order): `+0x00` the mesh's space at rest to the
  joint's, `+0x40` the vertices wholly its own, `+0x44` those blended with another bone, `+0x48` that bone (1-based;
  0 the mesh's own space), `+0x4C` its bounds, `+0x64` the blended vertices' weights (this bone's; the other gets
  1 - w). The .4ds holds the same (models.cpp ReadSingleMesh): two bones a vertex at most, in the data itself.
- **A joint** (frame type 10): `+0x50` its local matrix (I3D_frame::UpdateMatrix builds it from `+0x90` scale,
  `+0x9C` rotation, `+0x70` position while `+0xAC` bit 0x10 is clear), `+0x1C4` its matrix in the mesh's space, valid
  while `+0x204` bit 0x10 is set: PoseLod builds it as its local matrix times its parents' up to a visual, a sector
  or a model (types 1, 5, 9), or up to a joint whose own is valid.
- **LS3D's vertex buffers** (its own wrapper): `+0` the locked data or null; 1.2 `+0x04` flags (0x10: unlock first),
  `+0x18` stride, `+0x1C` the IDirect3DVertexBuffer8, `+0x48` its place in the locked list (`0x101C5014`: data, room,
  count; appended by ls3d+41700); 1.0 `+0x14` stride, `+0x18` the buffer, locked with D3DLOCK_NOSYSLOCK, no list.

## The functions

| | 1.2 | 1.0 |
|---|---|---|
| I3D_singlemesh::m_AddFGroups (vtable +0x68) | ls3d+55930 | ls3d+53030 |
| I3D_singlemesh::PoseLod (vtable +0x9C / +0x98) | ls3d+54F60 | ls3d+526A0 |
| I3D_singlemorph::PoseLod (morph, then the above) | ls3d+57590 | ls3d+546B0 |
| I3D_singlemesh::CopyRigidPositions (rigid positions into the drawn buffer) | ls3d+55640 | ls3d+52EA0 |
| I3D_singlemesh::CopySkinnedPositions (skinned positions, from the cache) | ls3d+557C0 | ls3d+52F70 |
| SkinOneBone (one bone), SkinTwoBones (two, by weight) | ls3d+68360, +68430 | ls3d+64F10, +64FE0 |
| LightsOfSector, LightVertices | ls3d+65A60, +65A70 | ls3d+62630, +62640 |
| EnvMapCoords (the environment map's uv from the normals) | ls3d+67D20 | ls3d+648F0 |
| I3D_frame::UpdateMatrix | ls3d+1A6E0 | ls3d+1A4A0 |
| S_matrix::SetRot (normals' matrix: each column normalised unless within 1e-8 of 1) | ls3d+2E1F0 | ls3d+2D730 |

m_AddFGroups, per frame and LOD drawn: when the LOD's `+0x40` is not the mesh's `+0x224`, PoseLod (the skin into
the cache), then CopyRigidPositions and CopySkinnedPositions into the drawn buffer; when the bones moved or the sector's lights'
stamp changed, LightVertices (the colour of each vertex from the cache's normals); the alpha (ls3d+66B50) when only
the fading changed; EnvMapCoords for an environment-mapped LOD. With `0x101C563C` bit 0 clear (1.0 `0x101C15DC`) LS3D
would light through Direct3D's ProcessVertices instead: not taken in either lab (0xE9).

## LightVertices (what a person's vertex colour is)

`bool __fastcall LightVertices(void *dest, uint destStride, MeshLevel *, void *posNorm, uint stride,
I3D_object *, S_matrix const &invWorld, uint mask, bool changed, lod *)`, the vertices in the mesh's own space:

- The lights of the sector LightsOfSector named (`+0x144` its lights, `+0x14C` their count) whose `+0x150` mask
  meets `mask` and that are on (`+0xAC` bit 0x1): ambient ones (type 4) summed (colour `+0x190..0x198` times the mesh's
  `+0x210`); the others whose range (`+0x1D0` far) reaches the mesh's bounding sphere, at most 8, the strongest kept
  ((r + g + b) / distance); each one's position (`+0x178` world) and direction (`+0x184`) taken into the mesh's space
  through invWorld, its ranges scaled by invWorld's scale, its colour times the mesh's `+0x210` times the first face
  group's material diffuse (unless the material's flags have 0x10 without 0x20). The ambient times the material's
  ambient plus its emissive. Kept in globals: the 8 slots (1.2 `0x101C4D80`, 0x50 bytes each: position, direction,
  colour, near, far, far squared, 1 / (far - near), the light), their count `0x101C5000`, the ambient `0x101C4D30`,
  the alpha byte `0x101C4D70` (material opacity times `+0x214` times `+0x218`, shifted to the top byte).
- A vertex: the ambient, plus for each slot N.L (unnormalised: the cache's normal, the direction to the light divided
  by the distance) times the colour; a point light's past its near range times (far - d) / (far - near), nothing past
  far; a spot's within its cone (`+0x1A0` cos of the outer half-angle, `+0x1B0` the inner's, `+0x1C0` 1 / their
  difference) likewise; a directional's N.L. Each channel times 255, clamped to 255, the alpha byte over it.

## What the loader does with it

host/skin.cpp: PoseLod hooked; a model whose skin a mod gives (`models\<name>.skin.json`, skin_set) is skinned by
the host into the same cache (up to 8 bones a vertex; the joints' matrices LS3D's own, built the same way;
CopyRigidPositions hooked to take the rigid vertices a skin moves from the cache). `skin.export` writes a model's own skin
in that form; given back it matches LS3D's within one float step (1.2e-7 m, both builds, 2026-10-20).
