# Xbox course format (.xbd)

Work in progress. The goal is a description of the Xbox SSX Tricky course files complete enough to *write* them,
so that [OpenSlope](https://github.com/swax/OpenSlope)'s Snowknife can build a course for the Xbox disc (and so for
OpenTricky) the way it already builds one for the PS2 disc.

Each fact below says where it comes from:

- **runtime**: OpenTricky reads it from the game's memory while the game runs (file and line given);
- **SSX-Library**: OpenSlope's file library, `FileHandlers/LevelFiles/Tricky/Xbox/XBDHandler.cs`, which loads
  the header and the patches and nothing else;
- **PS2**: the same library's `PS2/PBDHandler.cs`, the PS2 counterpart, which Snowknife reads and writes in full.

## What differs from the PS2 files

| PS2 | Xbox | Holds |
|---|---|---|
| `.pbd` | `.xbd` | terrain, models, instances, materials, lights, splines |
| `.ssh` | `.xsh` | texture bank (`_L` lightmaps, `_sky` sky) |
| `.ssf` | `.xsf` | collision |

SSX-Library's `.xsh` and `.xsf` classes are empty placeholders. The `.ltg`, `.map`, `.aip`/`.sop` and `.adl`
handlers sit outside its `PS2/` and `Xbox/` folders, which suggests they are shared; that is not yet checked
against an Xbox disc.

## Header

SSX-Library reads a little-endian header of 20 `u32` words (magic, then 19 counts, the third being the patch
count) followed by 21 `u32` offsets, the first being the patch array. The PS2 `.pbd` header has 15 counts and
18 offsets in this order: player starts, patches, instances, particle instances, materials, material blocks,
lights, splines, spline segments, texture flipbooks, models, particle models, textures, cameras, lightmap size.

Open question: whether the Xbox header keeps that order and appends to it. Needs a retail `.xbd`.

## Terrain patch: 0x2D0 bytes

One bicubic patch per record. The size is confirmed twice: the runtime walks the array in strides of 0x2D0
(`port/src/terrain_seams.c:4`, `REC_SIZE`), and SSX-Library's field list adds up to 0x2D0. In memory the game
keeps the array pointer at `level+0x9CC0` and the count at `level+0x2C` (`terrain_seams.c:592`).

| Offset | Type | Field | Source |
|---|---|---|---|
| 0x000 | vec4 | lightmap rect in the atlas: u at +0x00, v at +0x04 (times 128 = texel); the other two floats not yet traced | runtime (`terrain_seams.c:802`) |
| 0x010 | vec4 ×4 | texture u, v at the corners (a, b) = (0,0), (0,1), (1,0), (1,1) | runtime (`terrain_seams.c:121`) |
| 0x050 | vec4 ×16 | control points `C[r][k]` at `0x50 + 0x40r + 0x10k`; `P(a,b) = Σ B_r(b) B_k(a) C[r][k]`, `B = (t³, t², t, 1)` | runtime (`terrain_seams.c:86`) |
| 0x150 | vec3 ×2 | bounding box, low then high | SSX-Library |
| 0x168 | u32 ×6 | unknown; not in the PS2 record | SSX-Library |
| 0x180 | vec4 ×4 | corner points | SSX-Library |
| 0x1C0 | i16 | snow (diffuse) texture, sampler t0 | runtime (`terrain_seams.c:700`) |
| 0x1C2 | i16 | normal map texture, sampler t2, same coordinates as t0 | runtime (`terrain_seams.c:13`) |
| 0x1C4 | i16 | lightmap atlas page, sampler t1 (seen up to ~170) | runtime (`terrain_seams.c:760`) |
| 0x1C6 | u16 | unknown | SSX-Library |
| 0x1C8 | u16 ×130 | unknown, 260 bytes; not in the PS2 record | SSX-Library |
| 0x2CC | u32 | unknown | SSX-Library |

The game draws each patch as 8 × 8 vertices of 0x30 bytes, one 112-index strip (`terrain_seams.c:4-17`).

### Compared with the PS2 patch (0x1C0 bytes)

The first 0x168 bytes have the same layout: lightmap rect, four corner UVs, 16 control points, bounding box.
The PS2 record then goes straight to the corner points (0x168) and ends with surface type, visibility,
texture and lightmap ids (0x1A8–0x1C0). The Xbox record inserts six words before the corner points, keeps
three texture ids (adding a normal map), and its tail after the corner points is 272 bytes instead of 24.

So turning PS2 terrain into Xbox terrain is mostly a copy plus three things still to find out: what the six
words at 0x168 hold, which word carries the surface type, and what the 130-entry table at 0x1C8 is (its size
is close to the vertex grid, so it may be precomputed normals or lighting; unconfirmed).

### A mistake in SSX-Library's Xbox reader

`XBDHandler.cs` reads the four corner UVs first and the lightmap rect fifth. The runtime reads the lightmap
rect at 0x00 and the UVs at 0x10–0x4F, which is also the PS2 order. Any tool built on that reader gets those
two fields swapped; worth reporting upstream.

## Next

The rest (instances, models, materials, lights, splines, the `.xsh` texture bank and the `.xsf` collision) needs
either a retail `.xbd`/`.xsh`/`.xsf`, or the translated game code in `port/src/recomp/gen/`, which a build makes
from the owner's own disc and which this repository does not carry. The loader functions to trace first are
the ones that fill `level+0x9CC0` and `Mesh_RegisterVertexBuffers` (`xboxrecomp/src/kernel/xbox_memory_layout.h:372`).
