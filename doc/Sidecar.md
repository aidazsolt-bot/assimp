# Generic mesh sidecar API

ABI-safe side channel for importer data that does **not** fit the public
`aiMesh` / `aiScene` layout (avoids the ABI break class of
[PR #6593](https://github.com/assimp/assimp/pull/6593)).

Header: `include/assimp/sidecar.h`

## What it stores

Per mesh index (same indexing as `aiScene::mMeshes`):

| Field | Meaning |
|---|---|
| `aiSidecarBuffer[]` | Named attribute buffers (name, component type, tightly packed bytes) |
| `mExtrasJson` | UTF-8 JSON object: merged mesh + primitive `extras` |
| `mExtensionsJson` | UTF-8 JSON object: merged mesh + primitive `extensions` |

Lifetime = scene. **Not** preserved by `aiCopyScene` / SceneCombiner.

Scene metadata key when any sidecar is present: `AI_METADATA_MESH_SIDECAR` (`bool`).

## Consumer example

```cpp
#include <assimp/sidecar.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>

Assimp::Importer importer;
const aiScene *scene = importer.ReadFile("model.gltf", aiProcess_ValidateDataStructure);
if (aiSceneHasMeshSidecar(scene)) {
    const aiMeshSidecar *sc = aiGetMeshSidecar(scene, 0);
    for (unsigned int i = 0; i < sc->mNumBuffers; ++i) {
        const aiSidecarBuffer &b = sc->mBuffers[i];
        // b.mName e.g. "_SCALE"; b.mData tightly packed floats/ints
    }
    if (sc->mExtrasJson) {
        // parse JSON string of length sc->mExtrasJsonLength
    }
}
```

## glTF2 importer behaviour

Standard semantics (`POSITION`, `NORMAL`, `TANGENT`, `TEXCOORD_*`, `COLOR_*`,
`JOINTS_*`, `WEIGHTS_*`) stay on `aiMesh`. Anything else (typically `_`-prefixed
application attributes) is extracted with the same vertex remapping as
positions and attached via `AttachMeshSidecar`.

Mesh- and primitive-level `extras` / `extensions` are serialized to JSON on the
same sidecar bag.

Fixture: `test/models/glTF2/SidecarCustomAttr/SidecarCustomAttr.gltf`.

## Babylon mapping (RenderAssimp2026 follow-up)

Goal: feed BabylonNative without `BABYLON.SceneLoader.Append`, using Assimp as
the only importer. Suggested mapping for a future app bridge:

| Assimp | Babylon |
|---|---|
| `aiMesh` positions / normals / UVs / indices | `VertexData` / `BABYLON.Mesh` |
| `aiMaterial` + `GltfMaterial.h` keys | `PBRMaterial` (MR, clearcoat, transmission, volume, sheen, …) |
| `aiAnimation` / node channels | `Animation` / `AnimationGroup` (group packaging is app-side) |
| `aiMesh::mAnimMeshes` | morph target influences |
| `AI_MATKEY_UVTRANSFORM` | texture `uAng` / `uScale` / `uOffset` |
| Sidecar buffer `"_FOO"` | `mesh.setVerticesData("_FOO", floats, false, components)` or custom `VertexBuffer` |
| Sidecar `mExtrasJson` | `mesh.metadata` / `mesh.extras` |
| Sidecar `mExtensionsJson` | inspect for app-specific extensions not yet mapped to material keys |

Out of scope for the Assimp sidecar branch itself: Embedding bind, replacing
`look_babylon_scene.js` Append, NativeDraco path changes.
