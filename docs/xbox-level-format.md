# Xbox course format

Work in progress. The goal is a description of the Xbox SSX Tricky course files complete enough to *write* them,
so that [OpenSlope](https://github.com/swax/OpenSlope)'s Snowknife can build a course for the Xbox disc (and so for
OpenTricky) the way it already builds one for the PS2 disc.

Each fact says where it comes from:

- **file**: measured on the retail Garibaldi course (`data/models/gari.big`, USA disc);
- **loader**: the game's own code, as `fork_regen.sh` translates it (function names are their Xbox addresses);
- **runtime**: OpenTricky code that reads the data while the game runs (file and line given);
- **SSX-Library**: OpenSlope's file library. Its `Xbox/XBDHandler.cs` reads the header and the patches only;
  `PS2/PBDHandler.cs` is the PS2 counterpart Snowknife reads and writes in full.

No retail bytes are reproduced here beyond a few header values.

## The course archive

`data/models/<course>.big` is an EA `C0FB` archive: a big-endian header (`C0 FB`, then a `u16` holding the
header's length less 4, then the member count as a `u16`), then per member a 3-byte offset, a 3-byte size and a
NUL-terminated name. Members start on 0x80-byte boundaries and the file ends on the last one, unpadded. Each
member is compressed with EA RefPack (`10 FB`, then the 3-byte unpacked size). **file**

`tools/xbox_course/course_big.py` unpacks and repacks these archives. Writing back what it reads gives the
retail archive byte for byte (Garibaldi, Pipe Dream, the trick course), and every member it recompresses
unpacks to the same bytes (`tools/xbox_course/test_course_big.py`).

Garibaldi holds 13 members:

| Member | Unpacked | PS2 counterpart |
|---|---|---|
| `gari.xbd` | 5.97 MB | `.pbd`: terrain, instances, materials, lights, splines, models |
| `gari.xsh` | 1.46 MB | `.ssh`: texture bank, 121 textures |
| `gari_B.xsh` | 2.24 MB | none: 52 normal maps, Xbox only |
| `gari_L.xsh` | 1.05 MB | `_L.ssh`: 16 lightmap pages |
| `gari_sky.xbd`, `gari_sky.xsh`, `gari_sky.map` | | `_sky.pbd`, `_sky.ssh`, `_sky.map` |
| `gari.xsf` | 0.37 MB | `.ssf`: collision |
| `gari.ltg` | 0.72 MB | `.ltg`: world grid |
| `gari.map`, `gari.aip`, `gari.sop`, `gari.adl` | | same names |

Snowknife's member list (`docs/repack-technical-reference.md`) has no `_B` bank: the Xbox terrain asks for a
normal map that PS2 terrain does not have (see the patch record below).

## `.xbd` header: 0xA0 bytes, little-endian

A magic word (Garibaldi: `00 18 1E 02`), 18 counts at 0x04–0x48, then 21 offsets at 0x4C–0x9C, each relative to
the start of the file. The patch array starts right after, at 0xA0. **file**

The game turns each offset into a pointer by adding the file's base address, skipping zeros, and hands each
section to a handler together with its count; that is `sub_0013B14C` (and its twin `sub_001435FC`). The pairs
it uses are what name the sections below. **loader**

| Count | Offset | Section | Record | Garibaldi |
|---|---|---|---|---|
| 0x04 | 0x4C | player starts | | 0, offset 0 |
| 0x08 | 0x50 | terrain patches | 0x2D0 | 3885 |
| | 0x54 | patch source points | 16 × 20 per patch | 3885 × 320 |
| 0x0C, 0x10 | 0x58 | vertex pool (see below); the two counts' link to it is a guess | 32 | 1, 1 |
| 0x14 | 0x5C | instances | 144 | 3393 |
| 0x18 | 0x60 | particle instances | 112 | 10 |
| 0x1C | 0x64 | materials | 72 | 125 |
| 0x20 | 0x68 | material blocks | `u32` n, `u32`[n] | 648 |
| 0x24 | 0x6C | lights | 92 | 942 |
| 0x28 | 0x70 | splines | 40 | 169 |
| 0x2C | 0x74 | spline segments | 128 | 542 |
| 0x30 | 0x78 | texture flipbooks | variable | 16 |
| 0x34 | 0x7C, 0x80 | model pointers, models | variable | 648 |
| 0x38 | 0x84, 0x88 | particle model pointers, particle models | variable | 10 |
| 0x3C | | textures (matches the 121 in `gari.xsh`) | | 121 |
| 0x40 | 0x8C, 0x90 | camera pointers, cameras | | 0 |
| 0x44 | 0x94 | hash data, as on PS2 (not yet confirmed) | variable | count 0, 34,732 bytes |
| 0x48 | 0x98 | unknown | 12 | 4098 |
| | 0x9C | unused on Garibaldi | | 0 |

### Records shared with the PS2

Read against SSX-Library's `PBDHandler.cs`, these records have the PS2 size and the PS2 field order, with
plausible values everywhere on Garibaldi (indices in range, unit normals, colours): particle instances
(112), materials (72: texture id first, flipbook id last), lights (92: type, sprite, colour, direction,
position, bounding box), splines (40) and spline segments (128). **file**

### Instances (props): 144 bytes, PS2 256

| Offset | Field |
|---|---|
| 0x00 | placement matrix, 4 × 4 floats |
| 0x40 | model id (0–647 on Garibaldi), previous and next instance (−1 = none); the loader turns all three into pointers (`sub_0013B320`) |
| 0x4C | bounding box, low then high |
| 0x64 | three `u32`, zero on every Garibaldi instance |
| 0x70 | material block id, PS2's `ModelID2`: the loader resolves it through the material blocks, and it equals the model's own block on all 3393 |
| 0x74 | packed ARGB colour (0xFF7F7F7F and similar on 450 instances), else 0 or −1 |
| 0x78 | 1 on every instance |
| 0x7C, 0x80 | indices or −1 (43 and 29 distinct values) |
| 0x84 | float, 1.0 or 0 on most, varied on the rest |
| 0x88, 0x8C | zero on every instance; the loader clears 0x88 itself |

The PS2 instance carries a second 4 × 4 matrix of light directions and four light colours after the placement
matrix (128 bytes); the Xbox has neither, and adds the 16 bytes from 0x80. A PS2 instance converts by dropping
those 128 bytes and appending four words. The loader leaves 0x74–0x84 alone, so they are read later, while the
course runs; what a converter should write there is still open (copying the most common values, 0, −1, −1, 1.0, is
the safe guess).

### Models

The game walks models in a base class whose methods fix offsets into pointers and hand each part to the next
method (vtable at 0x1A7AE8, models from slot 0x34, `sub_0013AA20`, then `sub_0013AA50`, `sub_0013AA80`,
`sub_0013AB20`; the level loaders at 0x19AAC0 and 0x1A7C68 override only the dispatcher and the mesh leaf,
`sub_001437F0`). Every layout below is what those methods read, and it parses all 648 Garibaldi models with
nothing left over. **loader**, **file**

**Model header**, 15 `u32`, the PS2 header:

| Offset | Field |
|---|---|
| 0x00 | total length; the next model starts here (true for all 648) |
| 0x04 | object count |
| 0x08 | offset of the object table, from the model (always 0x3C) |
| 0x0C | material block id; the loader swaps it for a pointer into the material blocks |
| 0x10, 0x14 | unknown, animation time (as PS2) |
| 0x18 | three floats, PS2's "scale" |
| 0x24 | mesh count of the high-detail meshes |
| 0x28 | PS2's vertex count; not the number of vertices the strips use (model 0: 68 for 162) |
| 0x2C | strip count of the high-detail meshes |
| 0x30 | total strip indices of the high-detail meshes (model 0: 247 + 22 = 269) |
| 0x34 | PS2's "non-tri" count |

Mesh count, strip count and index total agree with the meshes on all 648 models.

**Object**, 0x18 bytes, the PS2 layout: parent (−1 = none), offsets of the high, medium and low detail mesh
headers (from the object; equal offsets share one header), offset of the animation (0 = none), offset of the
matrix (−1 = identity).

**Mesh header**: length, bounding box (low, high), flags, mesh count (0x20), a count at 0x24 that is not the
triangle count, then at 0x28 the offset (from the mesh header, always 0x2C) of a table of `mesh count` words,
each the offset of one mesh from that word itself. The PS2 header has the same fields up to 0x28.

**Mesh**, where the PS2 keeps VIF packets with their own vertices:

| Offset | Field |
|---|---|
| 0x00 | length; the next mesh starts here |
| 0x04 | material: index into the model's material block, whose entry is a material index (valid on all 824 meshes) |
| 0x08 | offset of the strip data, from the mesh (always 0xC) |
| 0x0C | `u16` strip count (1 on every Garibaldi mesh), then per strip a `u16` length and that many `u16` indices |

The strips are triangle strips joined by repeated indices. Their indices point into the course's **vertex pool**
(offset 0x58): one model's indices carry on where the previous model's stopped (0–161, 162–213, 214–357, …), and
together they cover the pool exactly (0–29,505 of 29,506). So each model's vertices are a contiguous run of the
pool, in model order, with position, normal and UV per vertex.

For a converter this means: append each model's vertices to one pool, write its strips with indices offset by
the pool's length so far, and give each mesh its material slot. Nothing in the mesh carries lighting.

Record sizes come from each section's length divided by its count, which comes out exact (splines leave 4 bytes
of alignment padding). **file** The record sizes for instances (144), particle instances (112), materials (72),
lights (92) and spline segments (128) also match the structures sketched in the comments of SSX-Library's
`XBDHandler.cs`.

Compared with the PS2 `.pbd` header (15 counts, 18 offsets), the order is the same with three additions: the
two counts at 0x0C/0x10 and the offsets at 0x54 and 0x58, which the Xbox uses for the patch source points and
the vertex pool. SSX-Library reads 20 counts and 21 offsets, one word too many on each side, so its patch
offset lands on the wrong word; its header needs the split moved to 18/21.

### Vertex pool (offset 0x58)

A `u32` count (Garibaldi 29,506), two `u32` zeros, then `count` records of 32 bytes: position (3 floats), unit
normal (3 floats), UV (2 floats). **file** Model-local coordinates and unit normals suggest this is the mesh
data the PS2 keeps in its own `MeshData` section, already in the form the Xbox GPU draws
(`Mesh_RegisterVertexBuffers`, `xboxrecomp/src/kernel/xbox_memory_layout.h:372`). That is inferred, not traced.

## Terrain patch: 0x2D0 bytes

One bicubic patch per record. The size is confirmed three ways: the file (count × 0x2D0 ends exactly at the
next section), the runtime (`port/src/terrain_seams.c`, `REC_SIZE`), and SSX-Library's field list. The game
keeps the array pointer at `level+0x9CC0` and the count at `level+0x2C`; `sub_000F7C90` stores both and builds
the draw cache from the records. **loader**

| Offset | Type | Field | Source |
|---|---|---|---|
| 0x000 | float ×4 | lightmap rect in the page: u, v, width, height (Garibaldi's first: 0, 0, 1/16, 1/16) | loader (`sub_000F7C90` reads +0/+4/+8/+0xC), file |
| 0x010 | vec4 ×4 | texture u, v at the corners (a, b) = (0,0), (0,1), (1,0), (1,1); z, w = 1 | runtime (`terrain_seams.c:121`), file |
| 0x050 | vec4 ×16 | control points `C[r][k]` at `0x50 + 0x40r + 0x10k`; `P(a,b) = Σ B_r(b) B_k(a) C[r][k]`, `B = (t³, t², t, 1)` | runtime (`terrain_seams.c:86`), loader |
| 0x150 | vec3 ×2 | bounding box, low then high | file |
| 0x168 | u32 ×6 | unknown; not in the PS2 record. Garibaldi's first patch: 0, 4, 4, 4, 4, 23 | file |
| 0x180 | vec4 ×4 | corner points, w = 1 (the first equals the first source point) | file |
| 0x1C0 | i16 | diffuse texture, index into `.xsh`, sampler t0 | runtime (`terrain_seams.c:700`) |
| 0x1C2 | i16 | normal map, index into `_B.xsh`, sampler t2, same coordinates as t0 | runtime (`terrain_seams.c:13`) |
| 0x1C4 | i16 | lightmap page, index into `_L.xsh`, sampler t1 | runtime (`terrain_seams.c:760`) |
| 0x1C6 | u16 | unknown; 0xFFFF on the first patch | file |
| 0x1C8 | u16 ×130 | unknown, 260 bytes; not in the PS2 record | SSX-Library |
| 0x2CC | u32 | unknown | SSX-Library |

The game draws each patch as 8 × 8 vertices of 0x30 bytes in one 112-index strip (`terrain_seams.c:4-17`).

### Compared with the PS2 patch (0x1C0 bytes)

The first 0x168 bytes have the same layout: lightmap rect, four corner UVs, 16 control points, bounding box.
The PS2 record then goes straight to the corner points (0x168) and ends with surface type, visibility, texture
and lightmap ids (0x1A8–0x1C0). The Xbox record inserts six words before the corner points, has three texture
ids (adding the normal map), and its tail after the corner points is 272 bytes instead of 24.

So PS2 terrain converts to Xbox terrain mostly by copying. What is still missing: what the six words at 0x168
hold, where the surface type went, and what the 130-entry table at 0x1C8 is.

### A mistake in SSX-Library's Xbox reader

`XBDHandler.cs` reads the four corner UVs first and the lightmap rect fifth. The retail file has the lightmap
rect at 0x00 and the UVs at 0x10–0x4F, the same order as the PS2. Any tool built on that reader gets those
two fields swapped; worth reporting upstream.

## Texture banks (`.xsh`)

`SHPX`, then the file size, the texture count (`u32`), a 4-byte tag (`G295`, `G278`), then per texture a 4-byte
name and a `u32` offset. Each texture starts with a 16-byte header: a type byte, a 3-byte offset to the next
block, then `u16` width and height. **file** This is EA's FSH/SSH shape layout with Xbox pixel formats:

| Type | Garibaldi use | Format (EA FSH codes; matches the block sizes) |
|---|---|---|
| 0x60 | most terrain and sky | DXT1 |
| 0x61 | 21 terrain textures | DXT3 |
| 0x78 | all 52 `_B` normal maps | R5G6B5 |
| 0x7D | all 16 lightmap pages, 128 × 128 | A8R8G8B8, no mip levels |

The 128 × 128 terrain textures carry four mip levels (128 down to 16); a block's size matches its format exactly.
The PS2 bank uses 8-bit indexed pages (type 2) instead. OpenTricky reads lightmap texels with a swizzle
(`terrain_seams.c:793`), so the uncompressed Xbox formats are probably stored swizzled. Not yet confirmed.

## The other members

- `.xsf` (collision): its header is the PS2 `.ssf` header word for word (`SSFHandler.cs`): effect slots,
  physics, collision models, effects, functions, object properties, instances and splines, each a count and an
  offset. On Garibaldi the instance and spline counts (3393, 169) equal the `.xbd`'s, and the 78 effect slots
  of 28 bytes end exactly where the physics section starts. The records themselves are not compared yet. **file**
- `.aip`, `.sop` (AI and race paths): SSX-Library's shared `AIPSOPHandler.cs` walks all 90 and 32 paths and
  stops exactly at the race-line offset plus the 16 bytes it skips, as on PS2. **file**
- `.adl` (sounds): `ADLHandler.cs`'s layout holds: 3085 entries, every sound offset inside the file. **file**
- `.ltg`: same header layout as `LTGHandler.cs`; the world-grid cell size at 0x28 reads 10,000 as it does there.
  Only the tool revision byte differs (Xbox `0x1E`, PS2 `0x1B`). **file**
- `.map`: plain text, and its header itself says "PS2 Version", tool revision 30. **file**

So the paths, sounds, grid and map should carry over from a PS2 build as they are, and collision probably does.
What needs real conversion is the `.xbd` (terrain tail fields, instances, model meshes into the vertex pool) and
the texture banks, plus a `_B.xsh` of normal maps.

## Next

1. Trace where the running game reads the instance words at 0x74–0x84, and the patch fields at 0x168 and 0x1C6–0x2CF.
2. The 0x48/0x98 section (4098 records of 12 bytes).
3. Compare the `.xsf` collision records with the PS2 ones.
4. Then write the converter: a PS2 course (or a Slopesmith export) in, an Xbox `.big` out.

Done: a Garibaldi repacked with `course_big.py` (every member recompressed by it) loads and races normally in
the Switch build (2026-10-20). The archive and compression side of a writer is settled.
