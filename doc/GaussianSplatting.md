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
- Nested compression drafts (e.g. SPZ) are **not** decoded; import needs
  decoded accessors.

Fixture: `test/models/glTF2/KHR_gaussian_splatting/`.

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

## Related material / physics sidecars

Transmission / volume / IOR use typed `AI_MATKEY_*` (complete).
Unmapped material extensions (iridescence, dispersion, diffuse transmission,
volume scatter, …) use `aiMaterialSidecar` — authored keys only.
Physics / animation pointer use their own sidecars (`doc/Sidecar.md`).
