# BLEND DNA emit map - typed vs sidecar

Branch split (Arris policy):

| Layer | Branch | What lands here |
| --- | --- | --- |
| Assimp typed | `(improve)-blender-2x-5x` | `aiMesh` / `aiMaterial` / `aiTexture` / `aiLight` / `aiCamera` / `aiNode` / `aiBone` / `aiAnimation`+`aiNodeAnim` |
| Sidecar bags | `feature/blend-sidecar` (on top of improve) | `aiMaterialSidecar` / `aiAnimationSidecar` / `aiSceneSidecar` / Physics JSON (`BLEND_*`) |
| Integration | `LocalMergeForEasyRebuild` | merge only |

Presence: emit a key only when DNA/SDNA actually resolved it. No invent.

**aiMetadata policy (Arris):** do **not** put BLEND domain bags (particles, physics, shader network, volumes) into `aiNode`/`aiScene` metadata. Those live in sidecars. Scene `aiMetadata` may only carry standard discovery flags (`Has*Sidecar`, `AI_METADATA_SOURCE_FORMAT*`) like other sidecar importers.

## Object classes

| DNA / Blender | Typed Assimp | Sidecar (feature) |
| --- | --- | --- |
| Mesh / CustomData / UV | `aiMesh` (+ UV names) | Mesh sidecar layers bag when needed |
| Material / Principled / maps | `aiMaterial` + embeds `aiTexture` | `BLEND_shader*` / network / volume JSON |
| Lamp | `aiLight` | Scene `lights[]` inventory (+ extras) |
| Camera (lens/sensor/ortho/clip) | `aiCamera` (`mHorizontalFOV`/`mAspect`/`mOrthographicWidth`) | `cameras[]` DOF / `sceneCamera` |
| Object TRS / hierarchy | `aiNode` | - |
| Armature / pose.bones Action | bone `aiNode` + `aiNodeAnim` | - |
| Camera / Empty TRS Action | `aiNodeAnim` | Modifier-prop / driver channels if not typed |
| World env image | typed env map when Assimp slot exists | Scene/World bag remainder |
| ParticleSettings / ParticleSystem | - (no `aiParticle*`) | `BLEND_physics_particles` + `BLEND_point_instancers` |
| Soft/Cloth/Fluid/Rigid/Ocean | - | Physics bags / KHR-shaped |
| Scene.active camera / world | - | `BLEND_scene` |

## ParticleSettings fields (emission / instance)

All keys below are **sidecar JSON only** (`settings` / PI node).

| DNA / RNA | Sidecar JSON key | Notes |
| --- | --- | --- |
| `type` | `part_type` | |
| `from` | `from` | emit_from VERT/FACE/VOLUME |
| `distr` | `distr` | JIT/RAND/GRID |
| `ren_as` | `ren_as` | 7=Object 8=Collection |
| `phystype` | `phystype` | Presence |
| `draw_as` / `draw` | `draw_as` / `draw` | Presence |
| `totpart` | `totpart` | |
| `userjit` / `grid_res` / `grid_rand` | same | Presence |
| `mass` / `size` / `randsize` / `jitfac` | same | |
| `lifetime` / `sta` / `end` | `lifetime` / `frame_start` / `frame_end` | |
| `randlife` / `randlength` / `hair_step` | same | Presence |
| `normfac` / RNA `hair_length` | `normfac` / `hair_length` | `hair_length = normfac * 4` when `normfac` present |
| `rad_root` / `rad_tip` / `rad_scale` | same | Hair thickness Presence; Cycles `r=rad_scale*0.5`, root/tip=`r*rad_*`. Not `size`. |
| `shape` / `shape_flag` | same | Hair shape + `PART_SHAPE_CLOSE_TIP` → optional `use_close_tip` |
| `ParticleSystem.seed` | `seed` | |
| instance collection/object | `collection` / `instanceObject` | |
| Instance TRS arrays | `positions` / `orients` / `scales` / `protoIndices` + `arraysResolved` | only when DNA particles present or Arris bake |
