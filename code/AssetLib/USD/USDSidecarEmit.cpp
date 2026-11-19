/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
All rights reserved.
----------------------------------------------------------------------
*/

#ifndef ASSIMP_BUILD_NO_USD_IMPORTER

#include "USDSidecarEmit.h"
#include "USDLoaderUtil.h"

#include <assimp/commonMetaData.h>
#include <assimp/scene.h>
#include <assimp/sidecar.h>

#include <climits>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace Assimp {
namespace {

char *DupJson(const std::string &s, size_t &outLen) {
    outLen = s.size();
    char *p = new char[outLen + 1];
    std::memcpy(p, s.c_str(), outLen + 1);
    return p;
}

std::string JsonEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

void EnsureMeta(aiScene *scene) {
    if (scene->mMetaData == nullptr) {
        scene->mMetaData = new aiMetadata();
    }
}

void FlagSidecarMeta(aiScene *scene) {
    EnsureMeta(scene);
    const bool t = true;
    if (aiSceneHasMeshSidecar(scene)) {
        scene->mMetaData->Add(AI_METADATA_MESH_SIDECAR, t);
    }
    if (aiSceneHasMaterialSidecar(scene)) {
        scene->mMetaData->Add(AI_METADATA_MATERIAL_SIDECAR, t);
    }
    if (aiSceneHasAnimationSidecar(scene)) {
        scene->mMetaData->Add(AI_METADATA_ANIMATION_SIDECAR, t);
    }
    if (aiSceneHasPhysicsSidecar(scene)) {
        scene->mMetaData->Add(AI_METADATA_PHYSICS_SIDECAR, t);
    }
}

struct StageWalkAccum {
    std::ostringstream cameras;
    std::ostringstream lights;
    std::ostringstream volumes;
    std::ostringstream instancers;
    std::ostringstream physicsNodes;
    std::ostringstream composition;
    std::vector<std::string> skipped;
    bool firstCam{ true };
    bool firstLight{ true };
    bool firstVol{ true };
    bool firstInst{ true };
    bool firstPhys{ true };
};

bool IsLightType(const std::string &ty) {
    return ty.find("Light") != std::string::npos;
}

bool IsPhysicsApi(lightusd::APISchemas::APIName n) {
    using N = lightusd::APISchemas::APIName;
    // Apple USDZ Preliminary_* and OpenUSD UsdPhysics Physics*API are distinct schemas.
    return n == N::Preliminary_PhysicsRigidBodyAPI ||
           n == N::Preliminary_PhysicsColliderAPI ||
           n == N::Preliminary_PhysicsMaterialAPI ||
           n == N::PhysicsRigidBodyAPI ||
           n == N::PhysicsCollisionAPI ||
           n == N::PhysicsMaterialAPI;
}

const char *PhysicsApiName(lightusd::APISchemas::APIName n) {
    using N = lightusd::APISchemas::APIName;
    switch (n) {
    case N::Preliminary_PhysicsRigidBodyAPI:
        return "Preliminary_PhysicsRigidBodyAPI";
    case N::Preliminary_PhysicsColliderAPI:
        return "Preliminary_PhysicsColliderAPI";
    case N::Preliminary_PhysicsMaterialAPI:
        return "Preliminary_PhysicsMaterialAPI";
    case N::PhysicsRigidBodyAPI:
        return "PhysicsRigidBodyAPI";
    case N::PhysicsCollisionAPI:
        return "PhysicsCollisionAPI";
    case N::PhysicsMaterialAPI:
        return "PhysicsMaterialAPI";
    default:
        return nullptr;
    }
}

void WalkPrim(const lightusd::Prim &prim, StageWalkAccum &acc) {
    const std::string ty = prim.type_name();
    const std::string path = prim.absolute_path().full_path_name();
    const std::string name = prim.element_name();

    if (ty == "Camera" || ty.find("Camera") != std::string::npos) {
        if (!acc.firstCam) {
            acc.cameras << ',';
        }
        acc.firstCam = false;
        acc.cameras << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path) << "\",\"type\":\"" << JsonEscape(ty) << "\"}";
    } else if (IsLightType(ty)) {
        if (!acc.firstLight) {
            acc.lights << ',';
        }
        acc.firstLight = false;
        acc.lights << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path) << "\",\"type\":\"" << JsonEscape(ty) << "\"}";
    } else if (ty == "Volume" || ty == "OpenVDBAsset") {
        if (!acc.firstVol) {
            acc.volumes << ',';
        }
        acc.firstVol = false;
        acc.volumes << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path) << "\",\"type\":\"" << JsonEscape(ty) << "\"}";
    } else if (ty == "PointInstancer") {
        if (!acc.firstInst) {
            acc.instancers << ',';
        }
        acc.firstInst = false;
        acc.instancers << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path) << "\"}";
    } else if (ty == "BasisCurves" || ty == "NurbsCurves" || ty == "Hair") {
        acc.skipped.push_back(std::string("curves:") + path);
    }

    if (prim.metas().references.has_value() || prim.metas().payload.has_value()) {
        acc.skipped.push_back(std::string("composition:payload_or_reference:") + path);
    }

    {
        std::ostringstream apis;
        bool firstApi = true;
        bool anyPhys = false;
        if (prim.metas().apiSchemas) {
            for (const auto &pair : prim.metas().apiSchemas->names) {
                const char *pn = PhysicsApiName(pair.first);
                if (pn == nullptr && !IsPhysicsApi(pair.first)) {
                    continue;
                }
                anyPhys = true;
                if (!firstApi) {
                    apis << ',';
                }
                firstApi = false;
                apis << '"' << (pn ? pn : "PhysicsAPI") << '"';
            }
        }
        // Fallback: authored physics:* props without apiSchemas recognition.
        if (!anyPhys) {
            const lightusd::GPrim *gprim = nullptr;
            if (const auto *c = prim.as<lightusd::GeomCube>()) {
                gprim = c;
            } else if (const auto *s = prim.as<lightusd::GeomSphere>()) {
                gprim = s;
            } else if (const auto *y = prim.as<lightusd::GeomCylinder>()) {
                gprim = y;
            } else if (const auto *o = prim.as<lightusd::GeomCone>()) {
                gprim = o;
            } else if (const auto *a = prim.as<lightusd::GeomCapsule>()) {
                gprim = a;
            } else if (const auto *m = prim.as<lightusd::GeomMesh>()) {
                gprim = m;
            }
            if (gprim != nullptr) {
                for (const auto &prop : gprim->props) {
                    if (prop.first.size() >= 8 && prop.first.compare(0, 8, "physics:") == 0) {
                        anyPhys = true;
                        if (!firstApi) {
                            apis << ',';
                        }
                        firstApi = false;
                        apis << "\"physics:properties\"";
                        break;
                    }
                }
            }
        }
        if (anyPhys) {
            if (!acc.firstPhys) {
                acc.physicsNodes << ',';
            }
            acc.firstPhys = false;
            acc.physicsNodes << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path)
                             << "\",\"apiSchemas\":[" << apis.str() << "]}";
        }
    }

    for (const auto &child : prim.children()) {
        WalkPrim(child, acc);
    }
}

void EmitMeshSidecars(aiScene *scene, const lightusd::tydra::RenderScene &rs) {
    for (size_t i = 0; i < rs.meshes.size() && i < scene->mNumMeshes; ++i) {
        const auto &m = rs.meshes[i];
        std::ostringstream json;
        json << "{\"schemaVersion\":1,\"source\":{\"format\":\"usd\"},\"primPath\":\"" << JsonEscape(m.abs_path)
             << "\",\"primName\":\"" << JsonEscape(m.prim_name) << "\"";
        if (!m.display_name.empty()) {
            json << ",\"displayName\":\"" << JsonEscape(m.display_name) << '"';
        }
        json << ",\"primvars\":{";
        bool first = true;
        for (auto it = m.texcoordSlotIdMap.s_begin(); it != m.texcoordSlotIdMap.s_end(); ++it) {
            if (!first) {
                json << ',';
            }
            first = false;
            json << '"' << JsonEscape(it->first) << "\":{\"slot\":" << it->second << ",\"semantic\":\"texcoord\"}";
        }
        if (m.normals.get_data().size()) {
            if (!first) {
                json << ',';
            }
            first = false;
            json << "\"normals\":{\"semantic\":\"normal\"}";
        }
        if (m.vertex_colors.get_data().size()) {
            if (!first) {
                json << ',';
            }
            first = false;
            json << "\"displayColor\":{\"semantic\":\"color\"}";
        }
        if (m.vertex_opacities.get_data().size()) {
            if (!first) {
                json << ',';
            }
            first = false;
            json << "\"displayOpacity\":{\"semantic\":\"opacity\"}";
        }
        json << '}';
        if (m.skel_id >= 0) {
            json << ",\"skel\":{\"skelId\":" << m.skel_id << "}";
        }
        if (!m.targets.empty()) {
            json << ",\"blendShapes\":[";
            bool fb = true;
            for (const auto &t : m.targets) {
                if (!fb) {
                    json << ',';
                }
                fb = false;
                json << '"' << JsonEscape(t.first) << '"';
            }
            json << ']';
        }
        json << '}';

        auto *sc = new aiMeshSidecar();
        sc->mExtensionsJson = DupJson(json.str(), sc->mExtensionsJsonLength);
        AttachMeshSidecar(scene, static_cast<unsigned int>(i), sc);
    }
}

void EmitMaterialSidecars(aiScene *scene, const lightusd::tydra::RenderScene &rs) {
    for (size_t i = 0; i < rs.materials.size() && i < scene->mNumMaterials; ++i) {
        const auto &mat = rs.materials[i];
        std::ostringstream json;
        json.setf(std::ios::fixed);
        json.precision(6);
        json << "{\"schemaVersion\":1,\"source\":{\"format\":\"usd\"},\"primPath\":\"" << JsonEscape(mat.abs_path)
             << "\",\"name\":\"" << JsonEscape(mat.name) << "\",\"UsdPreviewSurface\":{";
#if defined(ASSIMP_BUILD_USD_BETA)
        if (mat.surfaceShader.has_value()) {
            const auto &sh = *mat.surfaceShader;
#else
        {
            const auto &sh = mat.surfaceShader;
#endif
            json << "\"diffuseColor\":[" << sh.diffuseColor.value[0] << ',' << sh.diffuseColor.value[1] << ',' << sh.diffuseColor.value[2] << ']'
                 << ",\"metallic\":" << sh.metallic.value
                 << ",\"roughness\":" << sh.roughness.value
                 << ",\"opacity\":" << sh.opacity.value
                 << ",\"ior\":" << sh.ior.value
                 << ",\"clearcoat\":" << sh.clearcoat.value
                 << ",\"clearcoatRoughness\":" << sh.clearcoatRoughness.value;
        }
        json << "}}";

        auto *sc = new aiMaterialSidecar();
        sc->mExtensionsJson = DupJson(json.str(), sc->mExtensionsJsonLength);
        AttachMaterialSidecar(scene, static_cast<unsigned int>(i), sc);
    }
}

void EmitAnimSidecars(aiScene *scene, const lightusd::tydra::RenderScene &rs) {
    if (rs.animations.empty() || scene->mNumAnimations == 0) {
        return;
    }
    for (size_t i = 0; i < rs.animations.size() && i < scene->mNumAnimations; ++i) {
        const auto &anim = rs.animations[i];
        std::ostringstream json;
        json << "{\"schemaVersion\":1,\"source\":{\"format\":\"usd\"},\"path\":\"" << JsonEscape(anim.abs_path)
             << "\",\"USD_skel_channels\":[";
        bool first = true;
        for (const auto &ch : anim.channels_map) {
            if (!first) {
                json << ',';
            }
            first = false;
            json << "{\"joint\":\"" << JsonEscape(ch.first) << "\",\"numChannelTypes\":" << ch.second.size() << '}';
        }
        json << "],\"blendshapeWeights\":[";
        first = true;
        for (const auto &bw : anim.blendshape_weights_map) {
            if (!first) {
                json << ',';
            }
            first = false;
            json << '"' << JsonEscape(bw.first) << '"';
        }
        json << "]}";

        auto *sc = new aiAnimationSidecar();
        sc->mExtensionsJson = DupJson(json.str(), sc->mExtensionsJsonLength);
        AttachAnimationSidecar(scene, static_cast<unsigned int>(i), sc);
    }
}

void EmitPhysicsSidecar(aiScene *scene, const StageWalkAccum &acc, const char *formatTok) {
    if (acc.firstPhys) {
        return;
    }
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"source\":{\"format\":\"" << formatTok
         << "\"},\"USD_physics_rigid\":{\"nodes\":[" << acc.physicsNodes.str() << "]}}";
    auto *sc = new aiPhysicsSidecar();
    sc->mExtensionsJson = DupJson(json.str(), sc->mExtensionsJsonLength);
    AttachPhysicsSidecar(scene, sc);
}

std::string BuildSceneCategoryJson(
        const char *formatTok,
        const lightusd::tydra::SceneMetadata &meta,
        const lightusd::Stage &stage,
        const StageWalkAccum &acc,
        const lightusd::tydra::RenderScene &rs) {
    std::ostringstream json;
    json.setf(std::ios::fixed);
    json.precision(9);
    json << "{\"schemaVersion\":1,\"source\":{";
    json << "\"format\":\"" << formatTok << '"';
    if (!meta.upAxis.empty()) {
        json << ",\"upAxis\":\"" << meta.upAxis << '"';
    }
    if (std::isfinite(meta.metersPerUnit)) {
        json << ",\"metersPerUnit\":" << meta.metersPerUnit;
    }
    if (stage.metas().kilogramsPerUnit.has_value()) {
        json << ",\"kilogramsPerUnit\":" << stage.metas().kilogramsPerUnit.value();
    }
    if (std::isfinite(meta.timeCodesPerSecond)) {
        json << ",\"timeCodesPerSecond\":" << meta.timeCodesPerSecond;
    }
    if (std::isfinite(meta.framesPerSecond)) {
        json << ",\"framesPerSecond\":" << meta.framesPerSecond;
    }
    if (meta.startTimeCode) {
        json << ",\"startTimeCode\":" << *meta.startTimeCode;
    }
    if (meta.endTimeCode && std::isfinite(*meta.endTimeCode)) {
        json << ",\"endTimeCode\":" << *meta.endTimeCode;
    }
    json << '}';

    // S4 cameras / lights (typed RenderScene + stage walk)
    json << ",\"cameras\":[";
    bool first = true;
    for (const auto &c : rs.cameras) {
        if (!first) {
            json << ',';
        }
        first = false;
        json << "{\"name\":\"" << JsonEscape(c.name) << "\",\"primPath\":\"" << JsonEscape(c.abs_path)
             << "\",\"focalLength\":" << c.focalLength << ",\"znear\":" << c.znear << ",\"zfar\":" << c.zfar << '}';
    }
    if (!acc.firstCam) {
        if (!first) {
            json << ',';
        }
        json << acc.cameras.str();
    }
    json << "],\"lights\":[";
    first = true;
    for (const auto &l : rs.lights) {
        if (!first) {
            json << ',';
        }
        first = false;
        json << "{\"name\":\"" << JsonEscape(l.name) << "\",\"primPath\":\"" << JsonEscape(l.abs_path) << "\"}";
    }
    if (!acc.firstLight) {
        if (!first) {
            json << ',';
        }
        json << acc.lights.str();
    }
    json << ']';

    // S7 volumes + point instancers
    json << ",\"volumes\":[" << acc.volumes.str() << ']';
    json << ",\"pointInstancers\":[" << acc.instancers.str() << ']';

    // S8 composition snapshot
    json << ",\"composition\":{";
    const auto &sm = stage.metas();
    if (sm.defaultPrim.valid()) {
        json << "\"defaultPrim\":\"" << JsonEscape(sm.defaultPrim.str()) << '"';
    }
    json << ",\"doc\":\"" << JsonEscape(sm.doc.value) << "\"";
    json << '}';

    // S8 skipped
    json << ",\"skipped\":[";
    for (size_t i = 0; i < acc.skipped.size(); ++i) {
        if (i) {
            json << ',';
        }
        json << '"' << JsonEscape(acc.skipped[i]) << '"';
    }
    json << ",\"MaterialX_networks\",\"full_UsdShade_NodeGraph\",\"Hydra_composition_evaluator\"";
    json << "]}";
    return json.str();
}

} // namespace

void EmitUsdCategorySidecars(
        aiScene *pScene,
        const std::string &pFile,
        const lightusd::Stage &stage,
        const lightusd::tydra::RenderScene &render_scene,
        const lightusd::tydra::SceneMetadata &sceneMeta,
        const std::vector<std::string> *extraSkipped) {
    if (pScene == nullptr) {
        return;
    }

    const char *formatTok = "usd";
    if (isUsdz(pFile)) {
        formatTok = "usdz";
    } else if (isUsdc(pFile)) {
        formatTok = "usdc";
    } else if (isUsda(pFile)) {
        formatTok = "usda";
    }

    StageWalkAccum acc;
    for (const auto &root : stage.root_prims()) {
        WalkPrim(root, acc);
    }
    if (extraSkipped != nullptr) {
        for (const auto &note : *extraSkipped) {
            acc.skipped.push_back(note);
        }
    }

    EmitMeshSidecars(pScene, render_scene);
    EmitMaterialSidecars(pScene, render_scene);
    EmitAnimSidecars(pScene, render_scene);
    EmitPhysicsSidecar(pScene, acc, formatTok);

    EnsureMeta(pScene);
    const std::string sceneJson = BuildSceneCategoryJson(formatTok, sceneMeta, stage, acc, render_scene);
    // Replace S0 timing-only bag with full S0-S8 scene category JSON.
    pScene->mMetaData->Set(AI_METADATA_SCENE_SIDECAR_JSON, aiString(sceneJson));
    pScene->mMetaData->Set(AI_METADATA_SOURCE_FORMAT, aiString(formatTok));
    FlagSidecarMeta(pScene);
}

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_IMPORTER
