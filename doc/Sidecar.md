# Generic mesh / material / animation sidecar API

ABI-safe side channels for importer data that do **not** fit the public
`aiMesh` / `aiMaterial` / `aiAnimation` / `aiScene` layout (avoids the ABI break class of
[PR #6593](https://github.com/assimp/assimp/pull/6593)).

Header: `include/assimp/sidecar.h`

## What it stores

### Mesh sidecar (`aiMeshSidecar`)

Per mesh index (same indexing as `aiScene::mMeshes`):

| Field | Meaning |
|---|---|
| `aiSidecarBuffer[]` | Named attribute buffers (name, component type, tightly packed bytes) |
| `mExtrasJson` | UTF-8 JSON object: merged mesh + primitive `extras` |
| `mExtensionsJson` | UTF-8 JSON object: merged mesh + primitive `extensions` |

Scene metadata: `AI_METADATA_MESH_SIDECAR` (`bool`).

### Material sidecar (`aiMaterialSidecar`)

Per material index (same as `aiScene::mMaterials` / `aiMesh::mMaterialIndex`):

| Field | Meaning |
|---|---|
| `mExtensionsJson` | UTF-8 JSON object of **unmapped** material extensions |

Do **not** put material extensions on `aiMeshSidecar`. Scene metadata:
`AI_METADATA_MATERIAL_SIDECAR` (`bool`).

### Animation sidecar (`aiAnimationSidecar`)

Per animation index (same as `aiScene::mAnimations`):

| Field | Meaning |
|---|---|
| `mExtensionsJson` | UTF-8 JSON: at least `KHR_animation_pointer.channels[]` |
| `aiSidecarBuffer[]` | Named float sampler curves (`sampler_N_input` / `sampler_N_output`) |

Assimp does **not** play pointer animations. The host samples buffers and applies
JSON Pointers. Scene metadata: `AI_METADATA_ANIMATION_SIDECAR` (`bool`).

Lifetime = scene. **Not** preserved by `aiCopyScene` / SceneCombiner.

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
if (aiSceneHasMaterialSidecar(scene)) {
    const aiMaterialSidecar *msc = aiGetMaterialSidecar(scene, mesh->mMaterialIndex);
    // msc->mExtensionsJson: e.g. KHR_materials_iridescence object
}
if (aiSceneHasAnimationSidecar(scene)) {
    const aiAnimationSidecar *asc = aiGetAnimationSidecar(scene, 0);
    // asc->mExtensionsJson: KHR_animation_pointer channels
    // asc->mBuffers: float times / values named by inputBuffer / outputBuffer
}
```

## glTF2 importer behaviour

Standard semantics (`POSITION`, `NORMAL`, `TANGENT`, `TEXCOORD_*`, `COLOR_*`,
`JOINTS_*`, `WEIGHTS_*`) stay on `aiMesh`. Anything else (typically `_`-prefixed
application attributes) is extracted with the same vertex remapping as
positions and attached via `AttachMeshSidecar`.

Mesh- and primitive-level `extras` / `extensions` are serialized to JSON on the
same mesh sidecar bag.

### Material: `KHR_materials_iridescence`

Assimp does **not** yet write typed `AI_MATKEY_IRIDESCENCE_*` properties. Until
that exists, the glTF2 importer attaches a material sidecar whose
`mExtensionsJson` contains only keys **present in the source** (no Assimp
defaults such as factor=0 / ior=1.3 when omitted; no non-finite floats).

Texture objects include a resolved `path` (same rules as `aiMaterial::GetTexture`:
external URI or `*N` embedded). glTF `index` alone is never enough for the
RenderAssimp pack. `texCoord` / transform `scale` are emitted only when authored.

Fixtures: `test/models/glTF2/SidecarCustomAttr/`,
`test/models/glTF2/SidecarIridescence/`.

### Animation: `KHR_animation_pointer`

Classic channels (`target.node` + `translation` / `rotation` / `scale` / `weights`)
still fill `aiNodeAnim` / morph channels.

Channels with `path: "pointer"`:

| Pointer | Assimp behaviour |
|---|---|
| `/nodes/N/translation\|rotation\|scale\|weights` | Mapped to classic `aiNodeAnim` / morph (v1 convenience) |
| Anything else (materials, texture transforms, lights, cameras, ...) | Animation sidecar |

Sidecar JSON (authored keys only) example channel entry:

```json
{
  "sampler": 0,
  "pointer": "/materials/0/pbrMetallicRoughness/baseColorFactor",
  "interpolation": "LINEAR",
  "inputBuffer": "sampler_0_input",
  "outputBuffer": "sampler_0_output",
  "outputComponents": 4
}
```

Buffers are float (`5126`), tightly packed. Input is SCALAR times in seconds.
Output element count matches the accessor (`3x` input count for `CUBICSPLINE`).
An animation that has only pointer channels still creates `aiAnimation` (name /
duration / ticks) with `mNumChannels == 0`; duration is derived from sidecar
input times when needed. `aiProcess_ValidateDataStructure` allows that case when
an animation sidecar is attached.

Fixtures: `SidecarAnimPointerMaterial/`, `SidecarAnimPointerTexTransform/`,
`SidecarAnimPointerMixed/`.

## Babylon mapping (RenderAssimp2026 follow-up)

Goal: feed BabylonNative without `BABYLON.SceneLoader.Append`, using Assimp as
the only importer. Suggested mapping for a future app bridge:

| Assimp | Babylon |
|---|---|
| `aiMesh` positions / normals / UVs / indices | `VertexData` / `BABYLON.Mesh` |
| `aiMaterial` + `GltfMaterial.h` keys | `PBRMaterial` (MR, clearcoat, transmission, volume, sheen, …) |
| `aiAnimation` / node channels | `Animation` / `AnimationGroup` (group packaging is app-side) |
| `aiAnimationSidecar` (`KHR_animation_pointer`) | Host samples buffers + applies pointers (materials / UV transform / ...) |
| `aiMesh::mAnimMeshes` | morph target influences |
| `AI_MATKEY_UVTRANSFORM` | texture `uAng` / `uScale` / `uOffset` |
| Sidecar buffer `"_FOO"` | `mesh.setVerticesData("_FOO", floats, false, components)` or custom `VertexBuffer` |
| Sidecar `mExtrasJson` | `mesh.metadata` / `mesh.extras` |
| Sidecar `mExtensionsJson` | inspect for app-specific extensions not yet mapped to material keys |

Out of scope for the Assimp sidecar branch itself: Embedding bind, replacing
`look_babylon_scene.js` Append, NativeDraco path changes.

### Transmission: file data vs Babylon runtime (honest split)

Symptom often reported when skipping `SceneLoader.Append`:

> Transmission factors are set, but Babylon needs an opaque-scene RTT
> (`TransmissionHelper` from the glTF loader). Assimp pack without Append
> falls back to IBL refraction only - gloss/tint OK, no real see-through
> (e.g. cloth behind dragon glass).

| Piece | In the glTF/model file? | Assimp today | Sidecar? |
|---|---|---|---|
| `KHR_materials_transmission` factor / texture | **Yes** | Already on `aiMaterial` (`AI_MATKEY_TRANSMISSION_FACTOR` / `AI_MATKEY_TRANSMISSION_TEXTURE`) | **No** - not needed |
| `KHR_materials_volume` thickness / attenuation | **Yes** | Already on `aiMaterial` volume keys | **No** - not needed |
| Opaque-scene color buffer / `TransmissionHelper` RTT | **No** - runtime only | Cannot invent a framebuffer | **No** - never |
| Binding `subSurface.refractionTexture` to that RTT | **No** - engine wiring | App / Babylon JS | **No** |

So: Assimp (and sidecar) can deliver **authored** transmission numbers honestly.
They **cannot** deliver "true see-through" - that is Babylon painting an opaque
pass into a render target and sampling it as refraction, which
`babylonjs.loaders` + `TransmissionHelper` set up on Append. Without Append the
host must build the same pass (see `ensureOpaqueTransmissionPass` /
`refractionTexture` in RenderAssimp2026 `look_babylon_scene.js`).

Sidecar is for **missing file payload** (custom attrs, extras JSON), not for
**missing renderer features**.

## Physics sidecar

Scene-level bag for `KHR_implicit_shapes` + `KHR_physics_rigid_bodies` (see `AI_METADATA_PHYSICS_SIDECAR`).

- `aiSceneHasPhysicsSidecar` / `aiGetPhysicsSidecar`
- JSON is built generically by scanning the glTF document:
  - copy root `KHR_implicit_shapes` / `KHR_physics_rigid_bodies` when present
  - walk every `nodes[i]`; if it has `extensions.KHR_physics_rigid_bodies`, append
    `{index,name,KHR_physics_rigid_bodies}` to `nodes[]`
  - `nodeNames[]` lists those names in the same order (fallback `node_<index>` if unnamed)
- `index` is the glTF `nodes[]` array index (not DFS order under Assimp's synthetic ROOT).
- Host walks `nodeNames` / `nodes` - do not hardcode author-specific body names in Assimp.
- Assimp does not simulate; host (Bullet) steps and writes node TRS.
- Collider `geometry.mesh` stays in the JSON bag only. Do **not** promote those
  meshes onto `aiNode::mMeshes` (proxies are often intentionally mesh-less).

### SC4 orphan collider soups (`KHR_physics_collider_meshes`)

Meshes referenced only via `collider.geometry.mesh` (not on any draw node) are
Force-Retrieved into `aiScene::mMeshes` **without** attaching them to
`aiNode::mMeshes`, and their triangle soups are also embedded in the physics
JSON bag:

```json
"KHR_physics_collider_meshes": {
  "meshes": [
    { "index": 37, "name": "proxy-spiral", "positions": [x,y,z,...], "indices": [i0,i1,i2,...] }
  ]
}
```

- `index` = authored glTF mesh index (or BLEND `aiScene::mMeshes` index).
- `positions` = tightly packed float xyz; `indices` = uint triangle list.
- Host should prefer this bag for orphan soups (Presence); may also resolve by
  mesh name from `mMeshes` when Force-Retrieve included them.

Fixture: `test/models/glTF2/SidecarPhysicsRigidBodies/` (generic `BodyDynamic0` /
`BodyKinematic0` names only for local geometry; tests walk JSON structure).

### BLEND `BLEND_physics_*` inventory (Arris)

Soft/cloth/fluid/smoke/particles/rigidbody nodes share:

| Field | Meaning |
|---|---|
| `index` | Pre-order DFS under Assimp `<BlenderRoot>` (same order as Host `skinNodes`) |
| `name` | Blender Object name (`aiNode::mName`) |
| `modifier` / `type` / `settings` | Presence + DNA settings when available |

Fluid / smoke nodes additionally bind the visual mesh:

| Field | Meaning |
|---|---|
| `meshes` | `aiScene::mMeshes` indices on that node |
| `materialIndex` / `materialName` | Primary (`meshes[0]`) material |
| `materials[]` | Unique `{index,name}` for all meshes on the node |
| `settings.fluid_role` | `domain` \| `flow` \| `inflow` \| `outflow` \| `obstacle` \| `collision` \| `guide` \| (`particle`/`control` Elbeem) |
| `settings.domain_type` | authored string `gas` \| `liquid` (omit if unread); Elbeem Domain → `liquid` |
| `settings.cache` | directory + frame range when Domain/Fluidsim `surfdataPath` present |
| `settings.flow` | `type` + `flow_type` (`smoke`\|`fire`\|`smoke_fire`\|`liquid`) + density/fuel/temp/behavior |
| `settings.effector` | FluidEffector DNA (surface_distance, guide_mode, …) when EFFEC |
| legacy Fluidsim | `source:FluidsimModifierData` + `fluidsim_type` bits + bake/res/viscosity Presence |

`BLEND_physics_collision`: Collision modifier bind and/or `Object.pd` (PartDeflect) friction/damping Presence — no Name-Score.

`BLEND_modifier_curve`: CurveModifier DNA — `curveObject`, `defaxis`, optional `vgroup`.

`BLEND_physics_particles`: `part_kind` from DNA `eParticleType` (`hair`, `emitter`, `fluid_spray`, …); `ren_as_kind`; `path_start`/`path_end` when SDNA present. No invent for dust/damage (no DNA types).

`BLEND_physics_blast`: ExplodeModifier inventory (`index`/`settings`); Geometry Nodes fracture is listed under scene `skipped[]` (unevaluated - no invent chunks).

### BLEND `BLEND_mesh_cache` (0al — paths only)

Mesh Cache / Mesh Sequence Cache inventory in the **Physics** bag (`BLEND_mesh_cache.nodes[]`).
Assimp does **not** decode `.mdd` / `.abc` / `.pc2` into vertex buffers — Host loads
files (like textures / MTL).

| Field | Meaning |
|---|---|
| `index` / `name` / `meshes[]` | Same DFS bind as fluid domain |
| `type` | `mesh_cache` (MDD/PC2) or `mesh_sequence_cache` (Alembic/USD) |
| `settings.cacheKind` | `mdd` \| `pc2` \| `alembic` \| `usd` (DNA type, else authored path suffix) |
| `settings.cacheFileType` | Raw DNA `CacheFile.type` int when MeshSeqCache |
| `settings.cachePath` | DNA filepath Presence (`//…` or absolute); omit if empty |
| `settings.cachePathBlendRelative` | `true` when path starts with `//` |
| `settings.objectPath` | Archive object path (MeshSeqCache) when authored |
| `settings.cacheFile` / frame keys | CacheFile ID name + frame/offset Presence |
| `settings.eval_frame` / `frame_start` / … | MeshCache timing Presence (no invent) |

Missing path → no `cachePath` + optional scene `skipped[]` `BLEND_mesh_cache_no_path:…`.

KHR rigid (same bag):

- `collisionFilters` from `RigidBodyOb.col_groups` (`System_N` bits)
- `physicsJoints` + node `joint` from `RigidBodyCon` (limits/motors/springs when flagged)
- Passive children under a parent emit collider-only (Host compounds)
- `Shape_Trimesh` / `Shape_ConvexH` -> `collider.geometry.mesh` + soup in `KHR_physics_collider_meshes` (not empty implicit hull)
- `RBO_FLAG_DISABLED` -> node `trigger` (volume), not a simulated motion body

Domain meshes stay on `aiNode::mMeshes`; Host may hide opaque domains via sidecar.

### BLEND C6 packed Images (Arris)

Blender 2.77+ / 4.x store embeds in `Image.packedfiles` (`ImagePackedFile`), not only
deprecated `Image.packedfile`. Assimp reads both and emits `aiTexture` (`*N`) +
`aiTextureType_BASE_COLOR` / `DIFFUSE` when C6 finds Image Texture links (incl.
`ShaderNodeMix` chains) or falls back to packed paint maps (`layer_*_base.png`)
matched by material name (e.g. splash House of Chores). Grease Pencil strokes are
out of scope.

**Map inventory (2026-11-22):** `aiMaterialSidecar` may include `maps.inventory[]`
(`image` / `node` / `linked` / `embed`) for every `ShaderNodeTexImage` in the
material nodetree (incl. nested groups) - unlinked Previz nodes are inventory-only
(no typed BASE invent). Particle Fur slots: Physics `BLEND_physics_particles`
`settings.textures[]` from `ParticleSettings.mtex[]` when Image resolves.

### BLEND World Env / Skymap (Arris)

When `World.nodetree` has Environment Texture (or Image Tex) -> Background ->
World Output with a resolved Image, Assimp embeds it as `aiTexture` and emits
`SceneSidecarJson.BLEND_scene.worldEnv`:

| Key | Meaning |
|-----|---------|
| `image` / `path` | Image datablock name / filepath when present |
| `embed` | `*N` scene texture index when packed |
| `projection` | `equirectangular` (authored Env Texture) |
| `strength` | Background Strength only when socket resolved |

Presence: omit `worldEnv` entirely if no Image (solid Background is not a skymap).
BI procedural `TEX_ENVMAP` without Image stays in `skipped[]` (unchanged).

### BLEND Drivers (`BLEND_drivers`, C5)

Blender Drivers live on `AnimData.drivers` (FCurve list) with `FCurve.driver` ->
`ChannelDriver` (`expression`, `type`, `variables` / `DriverVar` / `DriverTarget`).
They are **not** Action/NLA FCurves. Assimp reads the DNA and emits Presence-only
JSON on a dedicated `aiAnimation` named `BLEND_drivers` (`mNumChannels == 0`) +
`aiAnimationSidecar`:

```json
"BLEND_drivers": {
  "owners": [
    {
      "idCode": "OB",
      "idName": "caruncle",
      "drivers": [
        {
          "rnaPath": "modifiers[\"Mesh Cache\"].eval_frame",
          "arrayIndex": 0,
          "type": 1,
          "typeName": "PYTHON",
          "flag": 25,
          "expression": "var+4",
          "variables": [
            {
              "name": "var",
              "type": 0,
              "typeName": "SINGLE_PROP",
              "targets": [
                {
                  "idCode": "OB",
                  "idName": "shading mask ctrl",
                  "idtype": 16975,
                  "rnaPath": "[\"cacheFrame\"]",
                  "transformChannel": 0,
                  "transformChannelName": "LOC_X"
                }
              ]
            }
          ],
          "keyInputBuffer": "drv0_times",
          "keyOutputBuffer": "drv0_values"
        }
      ]
    }
  ]
}
```

| Key | Meaning |
|-----|---------|
| `owners[].idCode` / `idName` | Driven ID (`OB` + Object name from `Object.adt`) |
| `rnaPath` / `arrayIndex` | Driven RNA path on that ID (FCurve) |
| `type` / `typeName` | DNA `eDriver_Types` (+ label when known) |
| `expression` | ChannelDriver expression when non-empty |
| `variables[]` | DriverVar name/type + `targets[]` (id, rnaPath, bone, transformChannel, ...) |
| `keyInputBuffer` / `keyOutputBuffer` | Optional generator FCurve keys (BezTriple) as float buffers |

Presence: omit empty expression / unresolved id / zero `num_targets` slots.
Assimp does **not** evaluate Python/expressions - Host samples deps + runs the
driver. Model-independent: any Object with `adt->drivers` contributes.

### BLEND C7 Sidecar-Graph + Host-Shader (no Assimp bake)

Policy: Assimp emits the authored Cycles/EEVEE graph for Host Looks to implement;
Assimp does **not** CPU-eval Brick/ColorRamp into typed maps.

- `BLEND_shader.addShader:true` when Material Output peels `ShaderNodeAddShader`.
- Emission sibling under Add: merge `emissionStrength` when resolved; procedural
  Emission Color stays in the network (no invent flat Ke).
- `BLEND_shader_network` (`eval:"host"`, `assimpEval:false`):
  - nodes: idname/name, `procedural`/`kind` (incl. `Mapping`), **unlinked input defaults**
    (`value` / `rgba` / `vector`), **ColorBand** stops for `ShaderNodeValToRGB`
  - **ShaderNodeMapping** Location/Rotation/Scale: Presence `vector` from SOCK_VECTOR
    socket defaults when unlinked; pre-2.81 files from DNA `TexMapping` storage
    (`source:"TexMapping"`) — Image and procedural paths.
    **Policy:** BLEND does **not** flatten Mapping into typed `$tex.uvtrafo`
    (`aiUVTransform`). DNA is 3D Loc/Rot/Scale + `vector_type` (Point/Texture/…);
    that is not a clean 2D UV trafo per map slot. Keep Mapping in this network bag;
    Host applies for Image and procedural (Brick, etc.).
  - Musgrave/Noise Presence: `musgraveType` (DNA `SHD_MUSGRAVE_*` /
    `NodeTexNoise.type`) + optional `musgraveTypeName` when known; omit if unread
  - links: from/to node + sockets
  - `hostProcedural`: `TexBrick` / `ColorRamp` when present
- Host Look may generate diffuse/emissive maps from that bag (Cityscapes facade /
  window lights). **Look-specific Bake/render** in the renderer remains an
  optional alternate path (not Assimp Authoring-Bake).

## USD / BLEND scene bag (C0 / S0-S8)

Scene timing/units/cameras/volumes/composition JSON lives on **`aiSceneSidecar`**
(heap UTF-8 via `aiGetSceneSidecar` / `aiSceneHasSceneSidecar`). Do **not** store
the bag as `aiString` under `SceneSidecarJson` (`AI_MAXLEN` truncates).

| Key / API | Type | Meaning |
|---|---|---|
| `aiGetSceneSidecar` / `AI_METADATA_SCENE_SIDECAR` (`HasSceneSidecar`) | heap JSON / bool | Full scene bag (unlimited) |
| `AI_METADATA_SCENE_SIDECAR_JSON` (`SceneSidecarJson`) | legacy name only | Deprecated as value channel; Host must use `aiGetSceneSidecar` |
| `AI_METADATA_SOURCE_FORMAT` | `aiString` | `usd` / `usda` / `usdc` / `usdz` / `blend` |

JSON shape (schemaVersion 1):

```json
{
  "schemaVersion": 1,
  "source": {
    "format": "usda",
    "upAxis": "Y|Z|X",
    "metersPerUnit": 1.0,
    "timeCodesPerSecond": 24.0,
    "framesPerSecond": 24.0,
    "startTimeCode": 1.0,
    "endTimeCode": 100.0
  }
}
```

Source: TinyUSDZ `Stage::metas()` (preferred) / `RenderScene::meta`.
Non-finite `endTimeCode` is omitted. Host uses this for Session frame loop;
Assimp does not evaluate USD composition beyond the loaded stage snapshot.

Fixture: `test/models/USD/SidecarSceneTiming/SidecarSceneTiming.usda`.
Test: `utUSDImport.sceneTimingSidecarS0Test` (requires `ASSIMP_BUILD_USD_IMPORTER`).

### USD S1-S8 category bags (`feature/usd-sidecar`)

| Slice | Channel | Contents |
|---|---|---|
| S1 | `aiMeshSidecar.mExtensionsJson` | `primPath`, `primvars` (texcoord slots, normals, displayColor/Opacity), blendShapes, skel |
| S2 | `aiAnimationSidecar` | `USD_skel_channels[]`, `blendshapeWeights[]` when TinyUSDZ emits clips |
| S3 | `aiMaterialSidecar` | `UsdPreviewSurface` remainder (diffuse/metallic/roughness/ior/clearcoat/…) |
| S4 | `aiSceneSidecar` | `cameras[]`, `lights[]` (RenderScene + stage walk) |
| S5 | `aiPhysicsSidecar` | `USD_physics_rigid.nodes[]` + optional `mass` from `physics:mass` |
| S6 | (same physics bag) | Soft/cloth USD schemas recorded when present; else listed under `skipped` |
| S7 | `aiSceneSidecar` + Physics bag | `volumes[]` / mat `BLEND_shader_volume` … `pointInstancers[]` stub. **BLEND C13** Physics `BLEND_point_instancers.nodes[]`: bind + `arraysResolved` + TRS when DNA `particles` **or** mesh-face eval (`arraysSource`); Presence `positionsSpace`/`scalesSpace`=`emitterLocal` (Host 0ai); FACE eval **object-space area-weight** + Presence `faceArea`/`emitWeight` per instance. **No** typed `Modifier:*`/`ParticleSystem:*` children (sidecar bind only). Scene bag heap. |
| S8 | `aiSceneSidecar` | `composition.defaultPrim/doc`, `skipped[]` (curves, MaterialX, Hydra, payload/reference, USDA sanitize notes) |

**BLEND Host read (2026-11-22 / 23):** Scene-Cam = `BLEND_scene.camera`/`cameraIndex` + `cameras[].sceneCamera` (not an `aiCamera` field); if missing → app settings active cam. C13 = `BLEND_point_instancers` (`totpart`; TRS when `arraysResolved`). **0ai spaces:** `positionsSpace`/`scalesSpace` = `emitterLocal` when arrays present. `positions[]` = emitter mesh-local - Host parents T under the emitter node. `scales[]` = isotropic Blender `size*path_len` in that same emitter-local frame (`mesh_face_eval`: `size*(1-randsize*r)*hair_length`, Presence `scalesMeaning=size_times_hair_length`; `dna_particles`: `ParticleData.size`). Keep reading `size` + `hair_length`/`normfac` separately. Host must **not** multiply `scales[]` by emitter Object.scale again when parenting (Blender `pamat*obmat` already carries Object.scale). Assimp does **not** bake Host `skinScale`/`skinCenter` into TRS. FACE area-weight remains object-space; thick emitters may still need Host plate filter. Highrise-style objects stay typed nodes outside the particle collection. Full Host guide: RenderAssimp `docs/ASSIMP_REQUEST_sidecars_blend_usd.md`.

Host-probe hardening (2026-11-19):

- **UsdGeomGprim tessellation:** `Cube` / `Sphere` / `Cylinder` / `Cone` / `Capsule` -> `aiMesh` when TinyUSDZ leaves them as Xform-only (`USDGprimMesh`).
- **Empty-mesh stages:** Volume-only / payload-only sets `AI_SCENE_FLAGS_INCOMPLETE` so ValidateDS does not hard-fail; sidecars still emit.
- **TinyUSDZ patch (not Assimp ASCII strip):** accept UsdPhysics `kilogramsPerUnit`; ignore other unknown stage/prim metas (e.g. `Usd_DCE_Params`); recognize distinct `Physics*API` vs Apple `Preliminary_Physics*API` tokens (`contrib/tinyusdz/patches/`).
- **Composition honesty:** `payload` / `references` -> `skipped[]` entry `composition:payload_or_reference:<path>` (no nullptr child path).

Fixtures: `test/models/USD/SidecarCategories/SidecarCategories.usda`,
`SidecarGprimPhysics/SidecarGprimPhysics.usda`,
`SidecarPayloadHonesty/SidecarPayloadHonesty.usda`.
Tests: `utUSDImport.categorySidecarsS1toS8Test`, `gprimPhysicsTessellationTest`,
`payloadCompositionHonestyTest`.
Spec map: RenderAssimp `docs/ASSIMP_REQUEST_sidecars_blend_usd.md`.

### BLEND compressed containers + DNA (Host probe 2026-11-19)

- Gzip (`.blend` with `1f 8b`) and **Zstandard** (`28 b5 2f fd`, Blender 3+ default,
  often multi-frame) are decompressed in `ParseMagicToken` before DNA parse
  (`ASSIMP_BLEND_SUPPORT_ZSTD`).
- Fixture: `test/models/BLEND/BlenderDefault_250_zstd.blend`
  (`utBlenderImporter.importBlenderDefault250ZstdCompressed`).
- Empty `mesh->mat[]` slots are skipped in UV/texture mapping and `BuildMaterials`
  (avoids SEGV on large fluid demos such as `lava_fluid-viscosity-demo.blend`).
- DNA `TYPE`/`TLEN`/`STRC` padding follows Blender `pad_up_4` relative to the
  **SDNA blob base** (not absolute file offset). DNA1 bodies can start at
  `file_off % 4 != 0` while markers stay aligned inside the DNA payload.

### BLEND file format version 0 vs 1 (Blender 5.0+ LargeBHead8)

Blender low-level container (see `BLO_core_blend_header.hh` / `BLO_core_bhead.hh`):

| Format | Header | Block header | Notes |
|---|---|---|---|
| **v0** | 12 bytes `BLENDER` + `[-_]` + `[vV]` + 3-digit ver | `BHead4` (4-byte ptr) or `SmallBHead8` (8-byte ptr) | Blender <=4.x write path |
| **v1** | 17 bytes `BLENDER` + `17` + `-` + `01` + `v` + 4-digit ver | always **`LargeBHead8`** (`code`, `SDNAnr`, `old`, `len` i64, `nr` i64) | Blender 5.0+; blocks can exceed 2GiB |

Assimp decodes v0/v1 before DNA walk. Unknown format version (`>01`) or bad header ->
`DeadlyImportError` with header dump (no silent mis-parse).

`SceneSidecarJson.source` / `BLEND_file` include:

| Key | Meaning |
|---|---|
| `formatVersion` | `0` or `1` |
| `blenderFileVersion` | integer from header (`282`, `500`, …) |

Fixture: `BlenderFormatV1_Header.blend` (ZSTD + `BLENDER17-01v0500`).
Test: `importBlenderFormatV1Header`.

Blender 5+ meshes often leave classic `*mvert` / `*mloop` / `*mpoly` null and
store geometry in `AttributeStorage` (see blender.git
`DNA_attribute_types.h` / `BKE_attribute_enums.hh`: `position` Float3 on Point,
`.corner_vert` Int32 on Corner) plus `face_offset_indices` (DNA rename of
`poly_offset_indices`). Assimp hydrates those into the classic arrays in
`BlenderMeshAttributes.cpp` before `ConvertMesh` (shared generated block
addresses are disambiguated by size and locality).

## BLEND typed vs sidecar map

See `doc/BLEND_DNA_EMIT_MAP.md` for DNA field ownership (typed `ai*` / node metadata vs sidecar bags) and ParticleSettings key table.
