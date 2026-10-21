# 3D Gaussian Splatting in Assimp

ABI-safe side channel: `include/assimp/gaussian.h` (`aiGetGaussianSplat`).
Does **not** extend `aiMesh` / `aiScene` layout (see also `doc/Sidecar.md`).

## Canonical value space

Assimp stores splat attributes in **KHR_gaussian_splatting** (ellipse kernel) space:

| Field | Meaning |
|---|---|
| `mOpacity` | Linear opacity in `[0, 1]` |
| `mScale` | Linear scale, non-negative |
| `mRotation` | Unit quaternion `(r,g,b,a) == (x,y,z,w)` (glTF order) |
| `mDC` / `mRest` | SH coefficients as in KHR attributes (degree 0 + higher bands) |
| `mKernel` / `mColorSpace` / … | Authored KHR meta strings only (empty if not in source) |

Assimp does **not** invent `COLOR_0`, SH display bias, or missing meta.
Renderers apply the KHR lighting formulas (including the documented SH0 bias)
themselves. Constants such as \(Y_{0,0} \approx 0.2820947917738781\) belong in
the host, not in Assimp invents.

## Sources

### glTF2 — `KHR_gaussian_splatting` (ratified)

- Primitive `mode` = `POINTS` with extension object (`kernel`, `colorSpace` required).
- Required attrs promoted into `aiGaussianSplat`.
- Promoted `KHR_gaussian_splatting:*` attrs are **removed** from mesh-sidecar
  custom buffers to avoid duplication.
- Extension JSON remains on `aiMeshSidecar::mExtensionsJson`.
- Nested compression drafts (e.g. SPZ buffers inside glTF) are **not**
  decoded in-place; use decoded accessors, or load a standalone `.spz`
  with the SPZ importer.

Fixture: `test/models/glTF2/KHR_gaussian_splatting/`.

### SPZ — Niantic Labs `.spz`

- Standalone importer (`code/AssetLib/SPZ/`, vendored `contrib/spz`).
- Detects NGSP v4 (ZSTD) and legacy gzip v1–3.
- Converts on import: `opacity = sigmoid(logit)`, `scale = exp(logScale)`,
  quaternion kept as glTF xyzw, SH DC + rest as authored.
- Requires system `libzstd` at build time (`ASSIMP_BUILD_SPZ_IMPORTER`).
- Does not invent `mKernel` / `mColorSpace`.

Fixtures: `test/models/SPZ/tiny_dc.spz`; upstream samples under
`test/models-nonbsd/SPZ/` (hornedlizard, racoonfamily).

### SPLAT — antimatter15 `.splat`

- Standalone importer (`code/AssetLib/SPLAT/`).
- Headerless little-endian 32-byte records (`count = filesize / 32`).
- Layout: `pos` 3×f32, `scale` 3×f32 (linear), `RGBA` 4×u8, `rot` 4×u8
  (PLY `rot_0..3` / wxyz packed as `(v*128+128)`).
- Converts on import: opacity = `a/255`, scale as authored (non-negative),
  quat wxyz → glTF xyzw + normalize, SH DC recovered from baked RGB via
  `f_dc = (rgb - 0.5) / Y_0,0` (inverse of convert.py). No `mRest`.
- Does not invent `mKernel` / `mColorSpace`.

Fixture: `test/models/SPLAT/tiny_dc.splat`.

### PLY — graphdeco / INRIA convention

- Detects `f_dc_*`, `opacity`, `scale_*`, `rot_*` (+ optional contiguous `f_rest_*`).
- Converts on import: `opacity = sigmoid(logit)`, `scale = exp(logScale)`,
  `rot` wxyz → glTF xyzw, then normalize.
- Does not set `mKernel` / `mColorSpace` (not present in PLY).

See `doc/PLY.md`.

## Consumer

```cpp
#include <assimp/gaussian.h>

const aiGaussianSplat *gs = aiGetGaussianSplat(scene, meshIndex);
// Positions: scene->mMeshes[meshIndex]->mVertices
```

Lifetime = scene. Not copied by `aiCopyScene`.

### `assimp info`

When a scene carries splat data, `assimp info` prints a **Gaussian Splats**
block (per-mesh point count, rest coeffs, present attrs, authored kernel /
colorSpace / projection / sorting when set). Use `-v` for opacity/scale ranges.
Use `-r` for a raw import without MaxQuality post-process (avoids vertex joins
that can make vertex counts differ from splat point counts).

## Related material / physics sidecars

Transmission / volume / IOR use typed `AI_MATKEY_*` (complete).
Unmapped material extensions (iridescence, dispersion, diffuse transmission,
volume scatter, …) use `aiMaterialSidecar` — authored keys only.
Physics / animation pointer use their own sidecars (`doc/Sidecar.md`).
