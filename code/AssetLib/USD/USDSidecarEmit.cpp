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

#include "tydra/scene-access.hh"

#include <climits>
#include <cmath>
#include <cstring>
#include <map>
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
    if (aiSceneHasSceneSidecar(scene)) {
        scene->mMetaData->Add(AI_METADATA_SCENE_SIDECAR, t);
    }
}

struct StageWalkAccum {
    std::ostringstream cameras;
    std::ostringstream lights;
    std::ostringstream volumes;
    std::ostringstream instancers;
    std::ostringstream physicsNodes;
    std::ostringstream physicsJoints;
    std::ostringstream physicsMaterials;
    std::ostringstream physicsFilters;
    std::ostringstream composition;
    std::vector<std::string> skipped;
    bool firstCam{ true };
    bool firstLight{ true };
    bool firstVol{ true };
    bool firstInst{ true };
    bool firstPhys{ true };
    bool firstJoint{ true };
    bool firstPhysMat{ true };
    bool firstFilter{ true };
};

bool TryFloatProp(const std::map<std::string, lightusd::Property> &props, const char *key, double &out) {
    auto it = props.find(key);
    if (it == props.end() || !it->second.is_attribute()) {
        return false;
    }
    const auto &attr = it->second.get_attribute();
    if (auto f = attr.get_value<float>()) {
        out = double(*f);
        return true;
    }
    if (auto d = attr.get_value<double>()) {
        out = *d;
        return true;
    }
    return false;
}

const lightusd::GPrim *AsGPrim(const lightusd::Prim &prim) {
    if (const auto *c = prim.as<lightusd::GeomCube>()) {
        return c;
    }
    if (const auto *s = prim.as<lightusd::GeomSphere>()) {
        return s;
    }
    if (const auto *y = prim.as<lightusd::GeomCylinder>()) {
        return y;
    }
    if (const auto *o = prim.as<lightusd::GeomCone>()) {
        return o;
    }
    if (const auto *a = prim.as<lightusd::GeomCapsule>()) {
        return a;
    }
    if (const auto *m = prim.as<lightusd::GeomMesh>()) {
        return m;
    }
    if (const auto *p = prim.as<lightusd::GeomPoints>()) {
        return p;
    }
    if (const auto *pi = prim.as<lightusd::PointInstancer>()) {
        return pi;
    }
    return nullptr;
}

bool IsLightType(const std::string &ty) {
    return ty.find("Light") != std::string::npos;
}

bool IsPhysicsJointType(const std::string &ty) {
    return ty.find("Joint") != std::string::npos;
}

bool IsPhysicsCollisionGroupType(const std::string &ty) {
    return ty.find("CollisionGroup") != std::string::npos;
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
        // TinyUSDZ Volume/OpenVDBAsset are not full Prim type-traits; emit path + type.
        // filePath may appear as custom prop on some authored stages via GPrim fallback below.
        acc.volumes << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path)
                    << "\",\"type\":\"" << JsonEscape(ty) << "\"";
        if (const lightusd::GPrim *gp = AsGPrim(prim)) {
            auto it = gp->props.find("filePath");
            if (it == gp->props.end()) {
                it = gp->props.find("field:filePath");
            }
            if (it != gp->props.end() && it->second.is_attribute()) {
                if (auto s = it->second.get_attribute().get_value<std::string>()) {
                    acc.volumes << ",\"filePath\":\"" << JsonEscape(*s) << '"';
                }
            }
        }
        acc.volumes << '}';
    } else if (ty == "PointInstancer" || ty == "Points") {
        if (!acc.firstInst) {
            acc.instancers << ',';
        }
        acc.firstInst = false;
        acc.instancers << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path)
                       << "\",\"type\":\"" << JsonEscape(ty) << '"';
        if (prim.as<lightusd::PointInstancer>() != nullptr) {
            acc.instancers << ",\"kind\":\"PointInstancer\"";
        } else if (prim.as<lightusd::GeomPoints>() != nullptr) {
            acc.instancers << ",\"kind\":\"GeomPoints\"";
        }
        acc.instancers << '}';
    } else if (IsPhysicsJointType(ty) || IsPhysicsJointType(name) || path.find("Joint") != std::string::npos) {
        // TinyUSDZ may keep unknown UsdPhysics joint schemas as generic Prim; still inventory by name/path.
        if (!acc.firstJoint) {
            acc.physicsJoints << ',';
        }
        acc.firstJoint = false;
        std::string jointTy = ty;
        // TinyUSDZ often maps unknown UsdPhysics joint schemas to Model.
        if (jointTy.empty() || jointTy == "Model" || jointTy == "Scope" || jointTy == "Xform") {
            if (name.find("Distance") != std::string::npos || path.find("Distance") != std::string::npos) {
                jointTy = "DistancePhysicsJoint";
            } else {
                jointTy = "PhysicsJoint";
            }
        }
        acc.physicsJoints << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path)
                          << "\",\"type\":\"" << JsonEscape(jointTy) << "\"}";
    } else if (IsPhysicsCollisionGroupType(ty) || IsPhysicsCollisionGroupType(name) ||
               path.find("CollisionGroup") != std::string::npos) {
        if (!acc.firstFilter) {
            acc.physicsFilters << ',';
        }
        acc.firstFilter = false;
        acc.physicsFilters << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path)
                           << "\",\"type\":\"" << JsonEscape(ty) << "\"}";
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
        bool isMaterialApiOnly = false;
        bool isFilteredPairs = false;
        if (prim.metas().apiSchemas) {
            for (const auto &pair : prim.metas().apiSchemas->names) {
                const char *pn = PhysicsApiName(pair.first);
                // FilteredPairs / CollisionGroup may arrive as unknown API name strings in metas;
                // also detect via type name above. Unknown enum values are skipped by PhysicsApiName.
                if (pn == nullptr && !IsPhysicsApi(pair.first)) {
                    continue;
                }
                anyPhys = true;
                if (pn && (std::strcmp(pn, "PhysicsMaterialAPI") == 0 ||
                           std::strcmp(pn, "Preliminary_PhysicsMaterialAPI") == 0)) {
                    isMaterialApiOnly = true;
                } else {
                    isMaterialApiOnly = false;
                }
                if (!firstApi) {
                    apis << ',';
                }
                firstApi = false;
                apis << '"' << (pn ? pn : "PhysicsAPI") << '"';
            }
        }
        const lightusd::GPrim *gprim = AsGPrim(prim);
        // Fallback: authored physics:* props without apiSchemas recognition.
        if (!anyPhys && gprim != nullptr) {
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
        // apiSchemas token string may include FilteredPairs even when enum lacks it - scan raw?
        (void)isFilteredPairs;

        if (anyPhys && isMaterialApiOnly && !IsPhysicsJointType(ty)) {
            if (!acc.firstPhysMat) {
                acc.physicsMaterials << ',';
            }
            acc.firstPhysMat = false;
            acc.physicsMaterials << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path)
                                 << "\",\"apiSchemas\":[" << apis.str() << "]";
            if (gprim) {
                double fr = 0, rst = 0;
                if (TryFloatProp(gprim->props, "physics:staticFriction", fr) ||
                        TryFloatProp(gprim->props, "physics:dynamicFriction", fr)) {
                    acc.physicsMaterials << ",\"friction\":" << fr;
                }
                if (TryFloatProp(gprim->props, "physics:restitution", rst)) {
                    acc.physicsMaterials << ",\"restitution\":" << rst;
                }
            }
            acc.physicsMaterials << '}';
        } else if (anyPhys) {
            if (!acc.firstPhys) {
                acc.physicsNodes << ',';
            }
            acc.firstPhys = false;
            acc.physicsNodes << "{\"name\":\"" << JsonEscape(name) << "\",\"primPath\":\"" << JsonEscape(path)
                             << "\",\"apiSchemas\":[" << apis.str() << "]";
            if (gprim) {
                double mass = 0, fr = 0, rst = 0;
                if (TryFloatProp(gprim->props, "physics:mass", mass) || TryFloatProp(gprim->props, "mass", mass)) {
                    acc.physicsNodes << ",\"mass\":" << mass;
                }
                if (TryFloatProp(gprim->props, "physics:staticFriction", fr) ||
                        TryFloatProp(gprim->props, "physics:dynamicFriction", fr)) {
                    acc.physicsNodes << ",\"friction\":" << fr;
                }
                if (TryFloatProp(gprim->props, "physics:restitution", rst)) {
                    acc.physicsNodes << ",\"restitution\":" << rst;
                }
            }
            acc.physicsNodes << '}';
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

// Match RenderMaterial.abs_path (Material prim) to a child/self UsdPreviewSurface shader.
const lightusd::UsdPreviewSurface *FindPreviewSurfaceForMaterial(
        const lightusd::tydra::PathShaderMap<lightusd::UsdPreviewSurface> &shaders,
        const std::string &matAbsPath) {
    const lightusd::UsdPreviewSurface *best = nullptr;
    size_t bestLen = 0;
    for (const auto &kv : shaders) {
        const std::string &spath = kv.first;
        if (spath.compare(0, matAbsPath.size(), matAbsPath) != 0) {
            continue;
        }
        if (spath.size() != matAbsPath.size() && spath[matAbsPath.size()] != '/') {
            continue;
        }
        if (spath.size() >= bestLen) {
            bestLen = spath.size();
            best = kv.second.second;
        }
    }
    return best;
}

template <typename T>
bool TryAuthoredAnimValue(const lightusd::TypedAttributeWithFallback<lightusd::Animatable<T>> &attr, T &out) {
    if (!attr.authored() || attr.is_blocked() || attr.is_connection()) {
        return false;
    }
    return attr.get_value().get(lightusd::value::TimeCode::Default(), &out);
}

void AppendAuthoredPreviewSurface(
        std::ostringstream &json,
        const lightusd::UsdPreviewSurface &ps) {
    bool first = true;
    auto comma = [&]() {
        if (!first) {
            json << ',';
        }
        first = false;
    };

    lightusd::value::color3f col;
    if (TryAuthoredAnimValue(ps.diffuseColor, col)) {
        comma();
        json << "\"diffuseColor\":[" << col[0] << ',' << col[1] << ',' << col[2] << ']';
    }
    float f = 0.f;
    if (TryAuthoredAnimValue(ps.metallic, f)) {
        comma();
        json << "\"metallic\":" << f;
    }
    if (TryAuthoredAnimValue(ps.roughness, f)) {
        comma();
        json << "\"roughness\":" << f;
    }
    if (TryAuthoredAnimValue(ps.opacity, f)) {
        comma();
        json << "\"opacity\":" << f;
    }
    if (TryAuthoredAnimValue(ps.ior, f)) {
        comma();
        json << "\"ior\":" << f;
    }
    if (TryAuthoredAnimValue(ps.clearcoat, f)) {
        comma();
        json << "\"clearcoat\":" << f;
    }
    if (TryAuthoredAnimValue(ps.clearcoatRoughness, f)) {
        comma();
        json << "\"clearcoatRoughness\":" << f;
    }
}

void EmitMaterialSidecars(
        aiScene *scene,
        const lightusd::Stage &stage,
        const lightusd::tydra::RenderScene &rs) {
    lightusd::tydra::PathShaderMap<lightusd::UsdPreviewSurface> previewShaders;
    (void)lightusd::tydra::ListShaders(stage, previewShaders);

    for (size_t i = 0; i < rs.materials.size() && i < scene->mNumMaterials; ++i) {
        const auto &mat = rs.materials[i];
        std::ostringstream json;
        json.setf(std::ios::fixed);
        json.precision(6);
        json << "{\"schemaVersion\":1,\"source\":{\"format\":\"usd\"},\"primPath\":\"" << JsonEscape(mat.abs_path)
             << "\",\"name\":\"" << JsonEscape(mat.name) << "\",\"UsdPreviewSurface\":{";
        if (const lightusd::UsdPreviewSurface *ps = FindPreviewSurfaceForMaterial(previewShaders, mat.abs_path)) {
            AppendAuthoredPreviewSurface(json, *ps);
        }
        // Honesty: omit schema-default factors when inputs were not authored (no TinyUSDZ fallback emit).
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
    if (acc.firstPhys && acc.firstJoint && acc.firstPhysMat && acc.firstFilter) {
        return;
    }
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"source\":{\"format\":\"" << formatTok << "\"";
    json << "},\"USD_physics_rigid\":{";
    bool needComma = false;
    if (!acc.firstPhys) {
        json << "\"nodes\":[" << acc.physicsNodes.str() << ']';
        needComma = true;
    }
    if (!acc.firstJoint) {
        if (needComma) {
            json << ',';
        }
        json << "\"joints\":[" << acc.physicsJoints.str() << ']';
        needComma = true;
    }
    if (!acc.firstPhysMat) {
        if (needComma) {
            json << ',';
        }
        json << "\"materials\":[" << acc.physicsMaterials.str() << ']';
        needComma = true;
    }
    if (!acc.firstFilter) {
        if (needComma) {
            json << ',';
        }
        json << "\"collisionGroups\":[" << acc.physicsFilters.str() << ']';
    }
    json << "}}";
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
    EmitMaterialSidecars(pScene, stage, render_scene);
    EmitAnimSidecars(pScene, render_scene);
    EmitPhysicsSidecar(pScene, acc, formatTok);

    EnsureMeta(pScene);
    const std::string sceneJson = BuildSceneCategoryJson(formatTok, sceneMeta, stage, acc, render_scene);
    // Full S0-S8 scene JSON on heap (not aiString / AI_MAXLEN).
    {
        auto *ssc = new aiSceneSidecar();
        ssc->mExtensionsJson = DupJson(sceneJson, ssc->mExtensionsJsonLength);
        AttachSceneSidecar(pScene, ssc);
    }
    pScene->mMetaData->Set(AI_METADATA_SOURCE_FORMAT, aiString(formatTok));
    FlagSidecarMeta(pScene);
}

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_IMPORTER
