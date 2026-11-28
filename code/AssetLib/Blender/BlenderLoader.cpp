/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

/** @file  BlenderLoader.cpp
 *  @brief Implementation of the Blender3D importer class.
 */

// #define ASSIMP_BUILD_NO_COMPRESSED_BLEND
//  Uncomment this to disable support for (gzip/zstd)compressed .BLEND files

#ifndef ASSIMP_BUILD_NO_BLEND_IMPORTER

#include "BlenderBMesh.h"
#include "BlenderCustomData.h"
#include "BlenderIntermediate.h"
#include "BlenderMeshAttributes.h"
#include "BlenderModifier.h"
#include "BlenderParticleDistribute.h"
#include <assimp/StringUtils.h>
#include <assimp/commonMetaData.h>
#include <assimp/importerdesc.h>
#include <assimp/metadata.h>
#include <assimp/scene.h>
#include <assimp/sidecar.h>

#include <assimp/DefaultIOSystem.h>
#include <assimp/MemoryIOWrapper.h>
#include <assimp/StreamReader.h>
#include <assimp/StringComparison.h>
#include <assimp/anim.h>
#include <assimp/camera.h>
#include <assimp/commonMetaData.h>
#include <assimp/metadata.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(ASSIMP_BUILD_NO_COMPRESSED_BLEND) && defined(ASSIMP_BLEND_SUPPORT_ZSTD)
#include <zstd.h>
#endif

namespace {
char *BlendDupJson(const std::string &s, size_t &outLen) {
    outLen = s.size();
    char *p = new char[outLen + 1];
    std::memcpy(p, s.c_str(), outLen + 1);
    return p;
}

std::string BlendJsonEscape(const std::string &in) {
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

// Legacy DNA node type ints (BKE_node_legacy_types.hh) - used when idname is absent.
enum BlendShaderNodeType : short {
    kSH_NODE_OUTPUT_MATERIAL = 124,
    kSH_NODE_OUTPUT_WORLD = 125,
    kSH_NODE_MIX_SHADER = 128,
    kSH_NODE_BACKGROUND = 130,
    kSH_NODE_BSDF_GLOSSY = 131,
    kSH_NODE_BSDF_DIFFUSE = 132,
    kSH_NODE_BSDF_GLOSSY_LEGACY = 133,
    kSH_NODE_BSDF_GLASS = 134,
    kSH_NODE_EMISSION = 140,
    kSH_NODE_TEX_IMAGE = 143,
    kSH_NODE_ADD_SHADER = 156,
    kSH_NODE_TEX_ENVIRONMENT = 157,
    kSH_NODE_TEX_BRICK = 169,
    kSH_NODE_VALTORGB = 104,
    kSH_NODE_BSDF_PRINCIPLED = 193,
    kSH_NODE_VOLUME_PRINCIPLED = 200,
    kSH_NODE_NORMAL_MAP = 175,
    kSH_NODE_BUMP = 170,
    kSH_NODE_INVERT = 119,
    kSH_NODE_REROUTE = 6,
    kSH_NODE_RGB = 101,
    kSH_NODE_VALUE = 102
};

/** Image map slot + optional UV layer name from ShaderNodeUVMap / MTex.uvname. */
struct BlendTexMap {
    const Assimp::Blender::Image *img{ nullptr };
    char uvname[64]{};
    /** Presence: UV scale/offset/rotate from ShaderNodeMapping or MTex ofs/size/rot. */
    bool hasUvTrafo{ false };
    float uvOfs[2]{ 0.f, 0.f };
    float uvScale[2]{ 1.f, 1.f };
    float uvRot{ 0.f };

    explicit operator bool() const {
        return img != nullptr;
    }
};

struct BlendShaderFlat {
    bool ok{ false };
    bool mixedGraph{ false };
    bool addShader{ false };
    const char *output{ nullptr };
    // Presence: only emit typed keys that were resolved from DNA default_value
    // or a linked constant / node-type-fixed factor (never invent ns zeros).
    bool hasBaseColor{ false };
    bool hasMetallic{ false };
    bool hasRoughness{ false };
    bool hasIor{ false };
    bool hasTransmission{ false };
    bool hasAlpha{ false };
    bool hasEmissionColor{ false };
    bool hasEmissionStrength{ false };
    bool hasCoatWeight{ false };
    bool hasCoatRoughness{ false };
    bool hasVolumeColor{ false };
    bool hasVolumeDensity{ false };
    bool hasVolumeDensityDefault{ false };
    bool hasVolumeDensityLinked{ false };
    bool hasVolumeDensityAttribute{ false };
    bool hasVolumeAnisotropy{ false };
    bool hasVolumeAbsorptionColor{ false };
    bool hasVolumeEmissionColor{ false };
    bool hasVolumeEmissionStrength{ false };
    float baseColor[4]{};
    float metallic{ 0.f };
    float roughness{ 0.f };
    float ior{ 0.f };
    float transmission{ 0.f };
    float alpha{ 0.f };
    float emissionColor[3]{};
    float emissionStrength{ 0.f };
    float coatWeight{ 0.f };
    float coatRoughness{ 0.f };
    float volumeColor[4]{};
    float volumeDensity{ 0.f };
    float volumeDensityDefault{ 0.f };
    float volumeAnisotropy{ 0.f };
    float volumeAbsorptionColor[4]{};
    float volumeEmissionColor[4]{};
    float volumeEmissionStrength{ 0.f };
    std::string volumeDensityAttribute;
    BlendTexMap mapBaseColor;
    BlendTexMap mapRoughness;
    BlendTexMap mapNormal;
    BlendTexMap mapEmission;
    BlendTexMap mapMetallic;

    bool HasAnyFactor() const {
        return hasBaseColor || hasMetallic || hasRoughness || hasIor || hasTransmission ||
               hasAlpha || hasEmissionColor || hasEmissionStrength || hasCoatWeight ||
               hasCoatRoughness || mapBaseColor || mapRoughness || mapNormal || mapEmission ||
               mapMetallic;
    }
};

bool BlendSockNameIs(const Assimp::Blender::bNodeSocket &sock, const char *want) {
    if (want == nullptr) {
        return false;
    }
    if (sock.identifier[0] != '\0' && std::strcmp(sock.identifier, want) == 0) {
        return true;
    }
    return std::strcmp(sock.name, want) == 0;
}

bool BlendSockNameIsAny(const Assimp::Blender::bNodeSocket &sock, std::initializer_list<const char *> names) {
    for (const char *n : names) {
        if (BlendSockNameIs(sock, n)) {
            return true;
        }
    }
    return false;
}

bool BlendReadSocketFloat(const Assimp::Blender::bNodeSocket &sock, float &out) {
    using namespace Assimp::Blender;
    // Only bNodeSocket.default_value (blender DNA). Deprecated ns is often zeroed and
    // must not be treated as an authored factor.
    if (!sock.default_value || sock.default_value->dna_type == nullptr) {
        return false;
    }
    if (BlendElemConvertedHullIs(*(sock.default_value), "bNodeSocketValueFloat")) {
        out = static_cast<const bNodeSocketValueFloat *>(sock.default_value.get())->value;
        return true;
    }
    return false;
}

bool BlendReadSocketColor(const Assimp::Blender::bNodeSocket &sock, float out[4]) {
    using namespace Assimp::Blender;
    if (!sock.default_value || sock.default_value->dna_type == nullptr) {
        return false;
    }
    if (BlendElemConvertedHullIs(*(sock.default_value), "bNodeSocketValueRGBA")) {
        const auto *rgba = static_cast<const bNodeSocketValueRGBA *>(sock.default_value.get());
        out[0] = rgba->value[0];
        out[1] = rgba->value[1];
        out[2] = rgba->value[2];
        out[3] = rgba->value[3];
        return true;
    }
    return false;
}

/** Presence: Vector socket default_value (SOCK_VECTOR / bNodeSocketValueVector). */
bool BlendReadSocketVector(const Assimp::Blender::bNodeSocket &sock, float out[3]) {
    using namespace Assimp::Blender;
    if (!sock.default_value || sock.default_value->dna_type == nullptr) {
        return false;
    }
    if (BlendElemConvertedHullIs(*(sock.default_value), "bNodeSocketValueVector")) {
        const auto *vec = static_cast<const bNodeSocketValueVector *>(sock.default_value.get());
        out[0] = vec->value[0];
        out[1] = vec->value[1];
        out[2] = vec->value[2];
        return true;
    }
    return false;
}

bool BlendNodeIsMapping(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeMapping") == 0) {
        return true;
    }
    // SH_NODE_MAPPING legacy type id (BKE_node_legacy_types.hh).
    return n.type == 109;
}

const Assimp::Blender::bNode *BlendCastNode(const Assimp::Blender::ElemBase *e) {
    using namespace Assimp::Blender;
    if (e == nullptr) {
        return nullptr;
    }
    if (e->dna_type != nullptr && !BlendElemConvertedHullIs(*(e), "bNode")) {
        return nullptr;
    }
    return static_cast<const bNode *>(e);
}

const Assimp::Blender::bNodeSocket *BlendCastSocket(const Assimp::Blender::ElemBase *e) {
    using namespace Assimp::Blender;
    if (e == nullptr) {
        return nullptr;
    }
    if (e->dna_type != nullptr && !BlendElemConvertedHullIs(*(e), "bNodeSocket")) {
        return nullptr;
    }
    return static_cast<const bNodeSocket *>(e);
}

bool BlendNodeIsPrincipled(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeBsdfPrincipled") == 0) {
        return true;
    }
    return n.type == kSH_NODE_BSDF_PRINCIPLED;
}

bool BlendNodeIsVolumePrincipled(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeVolumePrincipled") == 0) {
        return true;
    }
    return n.type == kSH_NODE_VOLUME_PRINCIPLED;
}

bool BlendNodeIsGlass(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeBsdfGlass") == 0) {
        return true;
    }
    return n.type == kSH_NODE_BSDF_GLASS;
}

bool BlendNodeIsGlossy(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeBsdfGlossy") == 0) {
        return true;
    }
    return n.type == kSH_NODE_BSDF_GLOSSY || n.type == kSH_NODE_BSDF_GLOSSY_LEGACY;
}

bool BlendNodeIsDiffuse(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeBsdfDiffuse") == 0) {
        return true;
    }
    return n.type == kSH_NODE_BSDF_DIFFUSE;
}

bool BlendNodeIsOutput(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeOutputMaterial") == 0) {
        return true;
    }
    return n.type == kSH_NODE_OUTPUT_MATERIAL;
}

bool BlendNodeIsOutputWorld(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeOutputWorld") == 0) {
        return true;
    }
    return n.type == kSH_NODE_OUTPUT_WORLD;
}

bool BlendNodeIsBackground(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeBackground") == 0) {
        return true;
    }
    return n.type == kSH_NODE_BACKGROUND;
}

bool BlendNodeIsTexEnvironment(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeTexEnvironment") == 0) {
        return true;
    }
    return n.type == kSH_NODE_TEX_ENVIRONMENT;
}

bool BlendNodeIsAddShader(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeAddShader") == 0) {
        return true;
    }
    return n.type == kSH_NODE_ADD_SHADER;
}

bool BlendNodeIsEmission(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeEmission") == 0) {
        return true;
    }
    return n.type == kSH_NODE_EMISSION;
}

bool BlendNodeIsTexImage(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeTexImage") == 0) {
        return true;
    }
    return n.type == kSH_NODE_TEX_IMAGE;
}

bool BlendNodeIsTexBrick(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeTexBrick") == 0) {
        return true;
    }
    return n.type == kSH_NODE_TEX_BRICK;
}

bool BlendNodeIsTexNoise(const Assimp::Blender::bNode &n) {
    return n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeTexNoise") == 0;
}

bool BlendNodeIsTexMusgrave(const Assimp::Blender::bNode &n) {
    return n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeTexMusgrave") == 0;
}

/** SHD_MUSGRAVE_* / SHD_NOISE_* alias; nullptr if unknown (do not invent). */
const char *BlendMusgraveTypeAlias(int type) {
    switch (type) {
    case 0:
        return "MULTIFRACTAL";
    case 1:
        return "FBM";
    case 2:
        return "HYBRID_MULTIFRACTAL";
    case 3:
        return "RIDGED_MULTIFRACTAL";
    case 4:
        return "HETERO_TERRAIN";
    default:
        return nullptr;
    }
}

bool BlendNodeIsColorRamp(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' &&
            (std::strcmp(n.idname, "ShaderNodeValToRGB") == 0 ||
                    std::strcmp(n.idname, "ShaderNodeRGBCurve") == 0)) {
        return true;
    }
    return n.type == kSH_NODE_VALTORGB;
}

bool BlendNodeIsUVMap(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeUVMap") == 0) {
        return true;
    }
    // SH_NODE_UVMAP historically 311 in DNA_node_types.h
    return n.type == 311;
}

bool BlendNodeIsTexCoord(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeTexCoord") == 0) {
        return true;
    }
    // SH_NODE_TEX_COORD historically 311+; idname is authoritative on 2.8+.
    return false;
}

bool BlendNodeIsMixShader(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeMixShader") == 0) {
        return true;
    }
    // Unified ShaderNodeMix with data_type SOCK_SHADER (eNodeSocketDatatype=3).
    // custom1 is data_type only for ShaderNodeMix - not for MixRGB (blend mode).
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeMix") == 0 && n.custom1 == 3) {
        return true;
    }
    return n.type == kSH_NODE_MIX_SHADER;
}

bool BlendNodeIsRGB(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && (std::strcmp(n.idname, "ShaderNodeRGB") == 0 ||
                                       std::strcmp(n.idname, "ShaderNodeCombineRGB") == 0 ||
                                       std::strcmp(n.idname, "ShaderNodeCombineColor") == 0)) {
        return true;
    }
    return n.type == kSH_NODE_RGB;
}

bool BlendNodeIsValue(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeValue") == 0) {
        return true;
    }
    return n.type == kSH_NODE_VALUE;
}

bool BlendNodeIsNormalMap(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeNormalMap") == 0) {
        return true;
    }
    return n.type == kSH_NODE_NORMAL_MAP;
}

bool BlendNodeIsBump(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeBump") == 0) {
        return true;
    }
    return n.type == kSH_NODE_BUMP;
}

bool BlendNodeIsDisplacement(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0') {
        return std::strcmp(n.idname, "ShaderNodeDisplacement") == 0 ||
               std::strcmp(n.idname, "ShaderNodeVectorDisplacement") == 0;
    }
    return false;
}

bool BlendNodeIsGroup(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0') {
        return std::strcmp(n.idname, "ShaderNodeGroup") == 0 ||
               std::strcmp(n.idname, "NodeGroup") == 0;
    }
    // Legacy NODE_GROUP
    return n.type == 2;
}

bool BlendNodeIsMixColor(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0') {
        if (std::strcmp(n.idname, "ShaderNodeMixRGB") == 0) {
            return true;
        }
        // ShaderNodeMix: custom1 = eNodeSocketDatatype; SOCK_SHADER=3 is Mix Shader, not color.
        if (std::strcmp(n.idname, "ShaderNodeMix") == 0) {
            return n.custom1 != 3;
        }
        return false;
    }
    return n.type == 75; // SH_NODE_MIX_RGB_LEGACY
}

/** MixRGB / ShaderNodeMixRGB: custom1 is MA_RAMP_* blend type. ShaderNodeMix uses custom1 as data_type. */
bool BlendNodeIsMixRgbBlendType(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0') {
        return std::strcmp(n.idname, "ShaderNodeMixRGB") == 0;
    }
    return n.type == 75; // SH_NODE_MIX_RGB_LEGACY
}

bool BlendNodeIsGroupOutput(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0') {
        return std::strcmp(n.idname, "NodeGroupOutput") == 0;
    }
    // NODE_GROUP_OUTPUT
    return n.type == 3;
}

bool BlendNodeIsGroupInput(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0') {
        return std::strcmp(n.idname, "NodeGroupInput") == 0;
    }
    // NODE_GROUP_INPUT
    return n.type == 7;
}

const Assimp::Blender::bNodeTree *BlendNodeGroupTree(const Assimp::Blender::bNode &n) {
    using namespace Assimp::Blender;
    if (!BlendNodeIsGroup(n) || !n.id) {
        return nullptr;
    }
    if (n.id->dna_type != nullptr && !BlendElemConvertedHullIs(*(n.id), "bNodeTree")) {
        return nullptr;
    }
    return static_cast<const bNodeTree *>(n.id.get());
}

bool BlendNodeIsFlattenable(const Assimp::Blender::bNode &n) {
    return BlendNodeIsPrincipled(n) || BlendNodeIsGlass(n) || BlendNodeIsGlossy(n) ||
           BlendNodeIsDiffuse(n) || BlendNodeIsEmission(n);
}

/** Nodes that end a Tex-Image upstream walk (shader roots / mix), not image filters. */
bool BlendNodeIsTexWalkBarrier(const Assimp::Blender::bNode &n) {
    return BlendNodeIsFlattenable(n) || BlendNodeIsOutput(n) || BlendNodeIsMixShader(n) ||
           BlendNodeIsAddShader(n) || BlendNodeIsOutputWorld(n) || BlendNodeIsBackground(n);
}

/** Higher = try first when several inputs are linked (Fac/Scale last). */
int BlendSockTexWalkPriority(const Assimp::Blender::bNodeSocket &sock) {
    if (BlendSockNameIsAny(sock, { "Fac", "Factor", "Scale", "Strength", "Distance",
                                         "Epsilon", "Weight", "Roughness", "Detail", "Dimension", "Lacunarity",
                                         "Offset", "Gain", "Gamma", "From Max", "From Min", "To Max", "To Min",
                                         "Steps", "Interpolation Type" })) {
        return 0;
    }
    if (BlendSockNameIsAny(sock, { "Color", "Image", "Height", "Normal" })) {
        return 100;
    }
    if (BlendSockNameIsAny(sock, { "B", "Color2", "ColorB" })) {
        return 90;
    }
    if (BlendSockNameIsAny(sock, { "A", "Color1", "ColorA" })) {
        return 80;
    }
    if (BlendSockNameIsAny(sock, { "Value", "Vector" })) {
        return 70;
    }
    // Unknown socket name: still follow if linked (Blender versions rename sockets).
    return 40;
}

const Assimp::Blender::bNodeLink *BlendCastLink(const Assimp::Blender::ElemBase *e) {
    using namespace Assimp::Blender;
    if (e == nullptr) {
        return nullptr;
    }
    if (e->dna_type != nullptr && !BlendElemConvertedHullIs(*(e), "bNodeLink")) {
        return nullptr;
    }
    return static_cast<const bNodeLink *>(e);
}

const Assimp::Blender::Image *BlendNodeImage(const Assimp::Blender::bNode &n) {
    using namespace Assimp::Blender;
    if (!n.id) {
        return nullptr;
    }
    if (n.id->dna_type != nullptr && !BlendElemConvertedHullIs(*(n.id), "Image")) {
        return nullptr;
    }
    return static_cast<const Image *>(n.id.get());
}

void BlendCopyUvName(char out[64], const char *src) {
    if (out == nullptr) {
        return;
    }
    out[0] = '\0';
    if (src == nullptr || src[0] == '\0') {
        return;
    }
    std::strncpy(out, src, 63);
    out[63] = '\0';
}

/** Match UV layer to aiMesh texcoord index (Blender: name, or default/render when empty).
 *  CollectMeshUVLayers promotes CustomData active_rnd (render/default UV) to Assimp set 0.
 *  Returns -1 when unresolved - callers must not invent UVWSRC 0. */
int BlendFindUvIndex(const Assimp::Blender::ConversionData &conv_data, unsigned matIndex, const char *uvname) {
    const bool wantDefault = (uvname == nullptr || uvname[0] == '\0');

    auto matchNamed = [&](const aiMesh *mesh) -> int {
        if (mesh == nullptr) {
            return -1;
        }
        for (unsigned u = 0; u < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++u) {
            if (!mesh->HasTextureCoords(u)) {
                break;
            }
            const aiString *n = mesh->GetTextureCoordsName(u);
            if (n != nullptr && n->length > 0 && std::strcmp(n->C_Str(), uvname) == 0) {
                return static_cast<int>(u);
            }
        }
        return -1;
    };
    auto matchDefault = [&](const aiMesh *mesh) -> int {
        // After active_rnd promote, Assimp set 0 is Blender default/render UV.
        if (mesh != nullptr && mesh->HasTextureCoords(0)) {
            return 0;
        }
        return -1;
    };

    auto scan = [&](bool requireMat) -> int {
        for (const aiMesh *mesh : conv_data.meshes.get()) {
            if (mesh == nullptr) {
                continue;
            }
            if (requireMat && mesh->mMaterialIndex != matIndex) {
                continue;
            }
            const int hit = wantDefault ? matchDefault(mesh) : matchNamed(mesh);
            if (hit >= 0) {
                return hit;
            }
        }
        return -1;
    };

    const int onMat = scan(true);
    if (onMat >= 0) {
        return onMat;
    }
    return scan(false);
}

unsigned BlendMaterialRawIndex(const Assimp::Blender::ConversionData &conv_data, const Assimp::Blender::Material *mat) {
    if (mat == nullptr) {
        return 0;
    }
    for (size_t i = 0; i < conv_data.materials_raw.size(); ++i) {
        if (conv_data.materials_raw[i].get() == mat) {
            return static_cast<unsigned>(i);
        }
    }
    return 0;
}

void BlendEmitUvWsrc(aiMaterial *mout, aiTextureType type, unsigned slot, int uvIndex) {
    if (mout == nullptr || uvIndex < 0) {
        return;
    }
    mout->AddProperty(&uvIndex, 1, AI_MATKEY_UVWSRC(type, slot));
}

/** Assimp-usual $tex.uvtrafo when DNA/node scale-offset-rotate was resolved (Presence). */
void BlendEmitUvTransform(aiMaterial *mout, aiTextureType type, unsigned slot,
        float ofsU, float ofsV, float scaleU, float scaleV, float rotRad) {
    if (mout == nullptr) {
        return;
    }
    aiUVTransform trafo;
    trafo.mTranslation.x = ofsU;
    trafo.mTranslation.y = ofsV;
    trafo.mScaling.x = scaleU;
    trafo.mScaling.y = scaleV;
    trafo.mRotation = rotRad;
    mout->AddProperty(&trafo, 1, AI_MATKEY_UVTRANSFORM(type, slot));
}

void BlendEmitUvTransformFromMap(aiMaterial *mout, aiTextureType type, unsigned slot, const BlendTexMap &map) {
    if (!map.hasUvTrafo) {
        return;
    }
    BlendEmitUvTransform(mout, type, slot, map.uvOfs[0], map.uvOfs[1], map.uvScale[0], map.uvScale[1], map.uvRot);
}

void BlendEmitUvTransformFromMTex(aiMaterial *mout, aiTextureType type, unsigned slot,
        const Assimp::Blender::MTex &tex) {
    // DNA MTex.ofs / size / rot always present on BI slots that ResolveImage sees.
    BlendEmitUvTransform(mout, type, slot, tex.ofs[0], tex.ofs[1], tex.size[0], tex.size[1], tex.rot);
    // Extra meta for hosts that do not read aiUVTransform (Z scale / full ofs).
    mout->AddProperty(&tex.ofs[0], 3, "$tex.blend.ofs", type, slot);
    mout->AddProperty(&tex.size[0], 3, "$tex.blend.size", type, slot);
    mout->AddProperty(&tex.rot, 1, "$tex.blend.rot", type, slot);
}

/** usage + mapto (+ optional uv name) so UNKNOWN (and typed) slots stay Host-parseable. */
void BlendEmitTexUsageMeta(aiMaterial *mout, aiTextureType type, unsigned slot, const char *usage,
        int maptoBits = 0, bool haveMapto = false, const char *uvname = nullptr) {
    if (mout == nullptr) {
        return;
    }
    if (usage != nullptr && usage[0] != '\0') {
        aiString u(usage);
        mout->AddProperty(&u, "$tex.blend.usage", type, slot);
    }
    if (haveMapto) {
        mout->AddProperty(&maptoBits, 1, "$tex.blend.mapto", type, slot);
    }
    if (uvname != nullptr && uvname[0] != '\0') {
        aiString uv(uvname);
        mout->AddProperty(&uv, "$tex.blend.uvName", type, slot);
    }
}

/** Sample FCurve at keyframe points (BezTriple.vec[1] = time, value). Linear between keys. */
float BlendEvalFCurve(const Assimp::Blender::FCurve &fc, float time) {
    if (fc.bezt.empty()) {
        return 0.f;
    }
    const auto &keys = fc.bezt;
    if (time <= keys.front().vec[1][0]) {
        return keys.front().vec[1][1];
    }
    if (time >= keys.back().vec[1][0]) {
        return keys.back().vec[1][1];
    }
    for (size_t i = 1; i < keys.size(); ++i) {
        const float t0 = keys[i - 1].vec[1][0];
        const float t1 = keys[i].vec[1][0];
        if (time <= t1) {
            const float v0 = keys[i - 1].vec[1][1];
            const float v1 = keys[i].vec[1][1];
            const float u = (t1 > t0) ? ((time - t0) / (t1 - t0)) : 0.f;
            return v0 + (v1 - v0) * u;
        }
    }
    return keys.back().vec[1][1];
}

const Assimp::Blender::FCurve *BlendCastFCurve(const Assimp::Blender::ElemBase *e) {
    using namespace Assimp::Blender;
    if (e == nullptr) {
        return nullptr;
    }
    if (e->dna_type != nullptr && !BlendElemConvertedHullIs(*(e), "FCurve")) {
        return nullptr;
    }
    return static_cast<const FCurve *>(e);
}

const Assimp::Blender::DriverVar *BlendCastDriverVar(const Assimp::Blender::ElemBase *e) {
    using namespace Assimp::Blender;
    if (e == nullptr) {
        return nullptr;
    }
    if (e->dna_type != nullptr && !BlendElemConvertedHullIs(*(e), "DriverVar")) {
        return nullptr;
    }
    return static_cast<const DriverVar *>(e);
}

/** Map DNA eDriver_Types -> stable name (label only; always also emit numeric type). */
const char *BlendDriverTypeName(int type) {
    switch (type) {
    case 0:
        return "AVERAGE";
    case 1:
        return "PYTHON";
    case 2:
        return "SUM";
    case 3:
        return "MIN";
    case 4:
        return "MAX";
    default:
        return nullptr;
    }
}

/** Map DNA eDriverVar_Types. */
const char *BlendDriverVarTypeName(short type) {
    switch (type) {
    case 0:
        return "SINGLE_PROP";
    case 1:
        return "ROT_DIFF";
    case 2:
        return "LOC_DIFF";
    case 3:
        return "TRANSFORM_CHAN";
    case 4:
        return "CONTEXT_PROP";
    default:
        return nullptr;
    }
}

/** Map DNA eDriverTarget_TransformChannels. */
const char *BlendDriverTransChanName(short ch) {
    switch (ch) {
    case 0:
        return "LOC_X";
    case 1:
        return "LOC_Y";
    case 2:
        return "LOC_Z";
    case 3:
        return "ROT_X";
    case 4:
        return "ROT_Y";
    case 5:
        return "ROT_Z";
    case 6:
        return "SCALE_X";
    case 7:
        return "SCALE_Y";
    case 8:
        return "SCALE_Z";
    case 9:
        return "SCALE_AVG";
    case 10:
        return "ROT_W";
    default:
        return nullptr;
    }
}

void BlendCollectObjectDrivers(const Assimp::Blender::Object *obj, Assimp::Blender::ConversionData &conv) {
    using namespace Assimp::Blender;
    if (obj == nullptr || !obj->adt) {
        return;
    }
    const char *objName = (obj->id.name[0] != '\0') ? (obj->id.name + 2) : "";
    for (std::shared_ptr<ElemBase> cur = obj->adt->drivers.first; cur;) {
        const FCurve *fc = BlendCastFCurve(cur.get());
        if (fc == nullptr) {
            break;
        }
        cur = fc->next;
        if (!fc->driver) {
            continue;
        }
        const ChannelDriver &drv = *fc->driver;
        ConversionData::DriverChannelEmit ch;
        ch.ownerName = objName ? objName : "";
        ch.ownerIdCode = "OB";
        ch.rnaPath = fc->rna_path;
        ch.arrayIndex = fc->array_index;
        ch.type = drv.type;
        ch.flag = drv.flag;
        // influence defaults to 0 in DNA zero-init; Blender treats unset as 1.0 at runtime,
        // but we only emit when the field is non-zero (Presence - no invent of 1.0).
        if (drv.influence != 0.f) {
            ch.influence = drv.influence;
            ch.hasInfluence = true;
        }
        if (drv.expression[0] != '\0') {
            ch.expression.assign(drv.expression);
        }
        for (std::shared_ptr<ElemBase> vcur = drv.variables.first; vcur;) {
            const DriverVar *dv = BlendCastDriverVar(vcur.get());
            if (dv == nullptr) {
                break;
            }
            vcur = dv->next;
            ConversionData::DriverVarEmit ve;
            if (dv->name[0] != '\0') {
                ve.name.assign(dv->name);
            }
            ve.type = dv->type;
            ve.flag = dv->flag;
            const int nTargets = (dv->num_targets > 0) ?
                                         std::min(static_cast<int>(dv->num_targets), 8) :
                                         0;
            for (int ti = 0; ti < nTargets; ++ti) {
                const DriverTarget &dt = dv->targets[ti];
                ConversionData::DriverTargetEmit te;
                te.idCode = dt.id_code;
                te.idName = dt.id_name;
                te.rnaPath = dt.rna_path;
                if (dt.pchan_name[0] != '\0') {
                    te.bone.assign(dt.pchan_name);
                }
                te.transChan = dt.transChan;
                te.flag = dt.flag;
                te.idtype = dt.idtype;
                if (dt.options != 0) {
                    te.options = dt.options;
                    te.hasOptions = true;
                }
                if (dt.context_property != 0) {
                    te.contextProperty = dt.context_property;
                    te.hasContextProperty = true;
                }
                // DTAR_OPTION_USE_FALLBACK = 1 << 0
                if ((dt.options & 1) != 0) {
                    te.fallbackValue = dt.fallback_value;
                    te.hasFallback = true;
                }
                if (dt.rotation_mode != 0) {
                    te.rotationMode = dt.rotation_mode;
                    te.hasRotationMode = true;
                }
                ve.targets.push_back(std::move(te));
            }
            ch.variables.push_back(std::move(ve));
        }
        if (!fc->bezt.empty()) {
            ch.keyTimes.reserve(fc->bezt.size());
            ch.keyValues.reserve(fc->bezt.size());
            for (const BezTriple &bz : fc->bezt) {
                ch.keyTimes.push_back(bz.vec[1][0]);
                ch.keyValues.push_back(bz.vec[1][1]);
            }
        }
        // Emit only when there is resolvable driver payload (expression and/or vars and/or keys).
        if (ch.expression.empty() && ch.variables.empty() && ch.keyTimes.empty()) {
            continue;
        }
        conv.driverChannels.push_back(std::move(ch));
    }
}

/** Build aiNodeAnim TRS from Action FCurves (location / rotation_euler), Assimp-usual like MD5 camera. */
aiNodeAnim *BlendBuildCameraNodeAnim(const Assimp::Blender::Object &obj, const Assimp::Blender::bAction &act) {
    using namespace Assimp::Blender;
    const FCurve *loc[3] = { nullptr, nullptr, nullptr };
    const FCurve *rot[3] = { nullptr, nullptr, nullptr };
    const FCurve *scl[3] = { nullptr, nullptr, nullptr };

    for (std::shared_ptr<ElemBase> cur = act.curves.first; cur;) {
        const FCurve *fc = BlendCastFCurve(cur.get());
        if (fc == nullptr) {
            break;
        }
        if (fc->array_index >= 0 && fc->array_index < 3) {
            if (fc->rna_path == "location") {
                loc[fc->array_index] = fc;
            } else if (fc->rna_path == "rotation_euler") {
                rot[fc->array_index] = fc;
            } else if (fc->rna_path == "scale") {
                scl[fc->array_index] = fc;
            }
        }
        cur = fc->next;
    }

    if (!loc[0] && !loc[1] && !loc[2] && !rot[0] && !rot[1] && !rot[2]) {
        return nullptr;
    }

    std::set<float> times;
    auto collect = [&](const FCurve *fc) {
        if (!fc) {
            return;
        }
        for (const BezTriple &b : fc->bezt) {
            times.insert(b.vec[1][0]);
        }
    };
    for (int i = 0; i < 3; ++i) {
        collect(loc[i]);
        collect(rot[i]);
        collect(scl[i]);
    }
    if (times.empty()) {
        return nullptr;
    }

    std::unique_ptr<aiNodeAnim> nd(new aiNodeAnim());
    nd->mNodeName.Set(obj.id.name + 2);
    nd->mNumPositionKeys = nd->mNumRotationKeys = nd->mNumScalingKeys = static_cast<unsigned int>(times.size());
    nd->mPositionKeys = new aiVectorKey[nd->mNumPositionKeys];
    nd->mRotationKeys = new aiQuatKey[nd->mNumRotationKeys];
    nd->mScalingKeys = new aiVectorKey[nd->mNumScalingKeys];

    unsigned int i = 0;
    for (float t : times) {
        aiVector3D p(obj.loc[0], obj.loc[1], obj.loc[2]);
        aiVector3D r(obj.rot[0], obj.rot[1], obj.rot[2]);
        aiVector3D s(obj.scale[0], obj.scale[1], obj.scale[2]);
        for (int a = 0; a < 3; ++a) {
            if (loc[a]) {
                (&p.x)[a] = BlendEvalFCurve(*loc[a], t);
            }
            if (rot[a]) {
                (&r.x)[a] = BlendEvalFCurve(*rot[a], t);
            }
            if (scl[a]) {
                (&s.x)[a] = BlendEvalFCurve(*scl[a], t);
            }
        }
        // Blender EULER_XYZ (radians) -> aiQuaternion via eul_to_mat3 XYZ.
        const float cx = std::cos(r.x), sx = std::sin(r.x);
        const float cy = std::cos(r.y), sy = std::sin(r.y);
        const float cz = std::cos(r.z), sz = std::sin(r.z);
        aiMatrix3x3 mat;
        mat.a1 = cy * cz;
        mat.a2 = -cy * sz;
        mat.a3 = sy;
        mat.b1 = cz * sx * sy + cx * sz;
        mat.b2 = cx * cz - sx * sy * sz;
        mat.b3 = -cy * sx;
        mat.c1 = -cx * cz * sy + sx * sz;
        mat.c2 = cz * sx + cx * sy * sz;
        mat.c3 = cx * cy;

        nd->mPositionKeys[i].mTime = t;
        nd->mPositionKeys[i].mValue = p;
        nd->mRotationKeys[i].mTime = t;
        nd->mRotationKeys[i].mValue = aiQuaternion(mat);
        nd->mScalingKeys[i].mTime = t;
        nd->mScalingKeys[i].mValue = s;
        ++i;
    }
    return nd.release();
}

void BlendPushCameraActionAnim(std::vector<aiAnimation *> &anims, const Assimp::Blender::Object &obj,
        const Assimp::Blender::bAction &act) {
    aiNodeAnim *channel = BlendBuildCameraNodeAnim(obj, act);
    if (channel == nullptr) {
        return;
    }
    aiAnimation *anim = new aiAnimation();
    anim->mName.Set(act.id.name[0] ? act.id.name + 2 : (obj.id.name + 2));
    anim->mTicksPerSecond = 24.0; // Blender default FPS when Scene.r not wired
    anim->mNumChannels = 1;
    anim->mChannels = new aiNodeAnim *[1];
    anim->mChannels[0] = channel;
    double tmax = 0.0;
    for (unsigned int k = 0; k < channel->mNumPositionKeys; ++k) {
        tmax = std::max(tmax, channel->mPositionKeys[k].mTime);
    }
    anim->mDuration = tmax;
    anims.push_back(anim);
}

void BlendConvertCameraAnimations(std::vector<aiAnimation *> &anims, Assimp::Blender::ConversionData &conv_data) {
    using namespace Assimp::Blender;
    std::set<std::pair<const Object *, const bAction *>> seen;
    std::vector<const Object *> objs = conv_data.camera_objects;
    objs.insert(objs.end(), conv_data.camera_focus_objects.begin(), conv_data.camera_focus_objects.end());
    if (objs.empty()) {
        return;
    }
    for (const Object *obj : objs) {
        if (obj == nullptr || !obj->adt) {
            continue;
        }
        auto tryAct = [&](const std::shared_ptr<bAction> &act) {
            if (!act || !seen.insert({ obj, act.get() }).second) {
                return;
            }
            BlendPushCameraActionAnim(anims, *obj, *act);
        };
        // Active Action (when assigned).
        tryAct(obj->adt->action);
        // NLA strips (tiger cameras stash clips here; adt.action is often null).
        for (std::shared_ptr<ElemBase> tcur = obj->adt->nla_tracks.first; tcur;) {
            const NlaTrack *track = nullptr;
            if (tcur->dna_type == nullptr || BlendElemConvertedHullIs(*(tcur), "NlaTrack")) {
                track = static_cast<const NlaTrack *>(tcur.get());
            }
            if (track == nullptr) {
                break;
            }
            for (std::shared_ptr<ElemBase> scur = track->strips.first; scur;) {
                const NlaStrip *strip = nullptr;
                if (scur->dna_type == nullptr || BlendElemConvertedHullIs(*(scur), "NlaStrip")) {
                    strip = static_cast<const NlaStrip *>(scur.get());
                }
                if (strip == nullptr) {
                    break;
                }
                tryAct(strip->act);
                scur = strip->next;
            }
            tcur = track->next;
        }
    }
}

// Matrix helpers are defined later in this TU (same anonymous namespace).
void BlendIdentityM4(float m[4][4]);
void BlendMulM3(float r[3][3], const float a[3][3], const float b[3][3]);
void BlendMulM4(float r[4][4], const float a[4][4], const float b[4][4]);
void BlendSizeToMat3(float m[3][3], const float size[3]);
void BlendQuatToMat3(float m[3][3], const float q[4]);
void BlendEulXYZToMat3(float m[3][3], const float eul[3]);
void BlendCopyMat4ToAi(aiMatrix4x4 &out, const float m[4][4]);

const Assimp::Blender::Bone *BlendCastBone(const Assimp::Blender::ElemBase *e) {
    using namespace Assimp::Blender;
    if (e == nullptr) {
        return nullptr;
    }
    if (e->dna_type != nullptr && !BlendElemConvertedHullIs(*(e), "Bone")) {
        return nullptr;
    }
    return static_cast<const Bone *>(e);
}

/** Parent-relative rest matrix from Bone.arm_mat (armature space). */
void BlendBoneRestLocalM4(const Assimp::Blender::Bone *bone, const Assimp::Blender::Bone *parent,
        float out[4][4]) {
    if (bone == nullptr) {
        BlendIdentityM4(out);
        return;
    }
    if (parent == nullptr) {
        for (unsigned i = 0; i < 4; ++i) {
            for (unsigned j = 0; j < 4; ++j) {
                out[i][j] = bone->arm_mat[i][j];
            }
        }
        return;
    }
    // local = inv(parent.arm_mat) * bone.arm_mat
    aiMatrix4x4 parentAi, boneAi;
    BlendCopyMat4ToAi(parentAi, parent->arm_mat);
    BlendCopyMat4ToAi(boneAi, bone->arm_mat);
    const aiMatrix4x4 localAi = parentAi.Inverse() * boneAi;
    for (unsigned x = 0; x < 4; ++x) {
        for (unsigned y = 0; y < 4; ++y) {
            out[x][y] = localAi[y][x];
        }
    }
}

/** rna_path "pose.bones[\"Name\"].prop" -> bone name + property. */
bool BlendParsePoseBoneRnaPath(const std::string &path, std::string &boneName, std::string &prop) {
    static const char kPrefix[] = "pose.bones[\"";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (path.size() <= kPrefixLen || path.compare(0, kPrefixLen, kPrefix) != 0) {
        return false;
    }
    const size_t endName = path.find("\"].", kPrefixLen);
    if (endName == std::string::npos) {
        return false;
    }
    boneName.assign(path, kPrefixLen, endName - kPrefixLen);
    prop.assign(path, endName + 3, std::string::npos);
    return !boneName.empty() && !prop.empty();
}

void BlendCollectActionFCurves(const Assimp::Blender::bAction &act, std::vector<const Assimp::Blender::FCurve *> &out) {
    using namespace Assimp::Blender;
    for (std::shared_ptr<ElemBase> cur = act.curves.first; cur;) {
        const FCurve *fc = BlendCastFCurve(cur.get());
        if (fc == nullptr) {
            break;
        }
        out.push_back(fc);
        cur = fc->next;
    }
    for (const std::shared_ptr<ActionStripKeyframeData> &strip : act.strip_keyframe_data_array) {
        if (!strip) {
            continue;
        }
        for (const std::shared_ptr<ActionChannelBag> &bag : strip->channelbag_array) {
            if (!bag) {
                continue;
            }
            for (const std::shared_ptr<FCurve> &fc : bag->fcurve_array) {
                if (fc) {
                    out.push_back(fc.get());
                }
            }
        }
    }
}

struct BlendPoseBoneCurves {
    const Assimp::Blender::FCurve *loc[3];
    const Assimp::Blender::FCurve *eul[3];
    const Assimp::Blender::FCurve *quat[4];
    const Assimp::Blender::FCurve *scl[3];
    BlendPoseBoneCurves() {
        for (int i = 0; i < 3; ++i) {
            loc[i] = eul[i] = scl[i] = nullptr;
        }
        for (int i = 0; i < 4; ++i) {
            quat[i] = nullptr;
        }
    }
    bool HasAny() const {
        for (int i = 0; i < 3; ++i) {
            if (loc[i] || eul[i] || scl[i]) {
                return true;
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (quat[i]) {
                return true;
            }
        }
        return false;
    }
    bool PreferQuat() const {
        for (int i = 0; i < 4; ++i) {
            if (quat[i]) {
                return true;
            }
        }
        return false;
    }
};

void BlendPoseBasisToMat4(const BlendPoseBoneCurves &c, float t, float r_mat[4][4]) {
    // Blender pose-channel DNA defaults: loc 0, eul 0, quat identity (w=1), scale 1.
    float loc[3] = { 0.f, 0.f, 0.f };
    float eul[3] = { 0.f, 0.f, 0.f };
    float quat[4] = { 1.f, 0.f, 0.f, 0.f };
    float scl[3] = { 1.f, 1.f, 1.f };
    for (int a = 0; a < 3; ++a) {
        if (c.loc[a]) {
            loc[a] = BlendEvalFCurve(*c.loc[a], t);
        }
        if (c.eul[a]) {
            eul[a] = BlendEvalFCurve(*c.eul[a], t);
        }
        if (c.scl[a]) {
            scl[a] = BlendEvalFCurve(*c.scl[a], t);
        }
    }
    for (int a = 0; a < 4; ++a) {
        if (c.quat[a]) {
            quat[a] = BlendEvalFCurve(*c.quat[a], t);
        }
    }

    float smat[3][3], rmat[3][3], tmat[3][3];
    BlendSizeToMat3(smat, scl);
    if (c.PreferQuat()) {
        BlendQuatToMat3(rmat, quat);
    } else {
        BlendEulXYZToMat3(rmat, eul);
    }
    BlendMulM3(tmat, rmat, smat);
    BlendIdentityM4(r_mat);
    for (unsigned int i = 0; i < 3; ++i) {
        for (unsigned int j = 0; j < 3; ++j) {
            r_mat[i][j] = tmat[i][j];
        }
    }
    r_mat[3][0] = loc[0];
    r_mat[3][1] = loc[1];
    r_mat[3][2] = loc[2];
}

aiNodeAnim *BlendBuildPoseBoneNodeAnim(const std::string &nodeName, const float restLocal[4][4],
        const BlendPoseBoneCurves &curves) {
    using namespace Assimp::Blender;
    if (!curves.HasAny()) {
        return nullptr;
    }
    std::set<float> times;
    auto collect = [&](const FCurve *fc) {
        if (!fc) {
            return;
        }
        for (const BezTriple &b : fc->bezt) {
            times.insert(b.vec[1][0]);
        }
    };
    for (int i = 0; i < 3; ++i) {
        collect(curves.loc[i]);
        collect(curves.eul[i]);
        collect(curves.scl[i]);
    }
    for (int i = 0; i < 4; ++i) {
        collect(curves.quat[i]);
    }
    if (times.empty()) {
        return nullptr;
    }

    std::unique_ptr<aiNodeAnim> nd(new aiNodeAnim());
    nd->mNodeName.Set(nodeName.c_str());
    nd->mNumPositionKeys = nd->mNumRotationKeys = nd->mNumScalingKeys = static_cast<unsigned int>(times.size());
    nd->mPositionKeys = new aiVectorKey[nd->mNumPositionKeys];
    nd->mRotationKeys = new aiQuatKey[nd->mNumRotationKeys];
    nd->mScalingKeys = new aiVectorKey[nd->mNumScalingKeys];

    unsigned int i = 0;
    for (float t : times) {
        float basis[4][4], localBl[4][4];
        BlendPoseBasisToMat4(curves, t, basis);
        BlendMulM4(localBl, restLocal, basis);
        aiMatrix4x4 localAi;
        BlendCopyMat4ToAi(localAi, localBl);
        aiVector3D scaling, position;
        aiQuaternion rotation;
        localAi.Decompose(scaling, rotation, position);
        nd->mPositionKeys[i].mTime = t;
        nd->mPositionKeys[i].mValue = position;
        nd->mRotationKeys[i].mTime = t;
        nd->mRotationKeys[i].mValue = rotation;
        nd->mScalingKeys[i].mTime = t;
        nd->mScalingKeys[i].mValue = scaling;
        ++i;
    }
    return nd.release();
}

void BlendFindBoneRestLocal(const Assimp::Blender::Bone *bone, const Assimp::Blender::Bone *parent,
        const std::string &wantName, float out[4][4], bool &found) {
    using namespace Assimp::Blender;
    if (bone == nullptr || found) {
        return;
    }
    if (wantName == bone->name) {
        BlendBoneRestLocalM4(bone, parent, out);
        found = true;
        return;
    }
    for (std::shared_ptr<ElemBase> cur = bone->childbase.first; cur;) {
        const Bone *child = BlendCastBone(cur.get());
        if (child == nullptr) {
            break;
        }
        BlendFindBoneRestLocal(child, bone, wantName, out, found);
        if (found) {
            return;
        }
        cur = child->next;
    }
}

bool BlendArmatureBoneRestLocal(const Assimp::Blender::bArmature &arm, const std::string &boneName, float out[4][4]) {
    using namespace Assimp::Blender;
    bool found = false;
    for (std::shared_ptr<ElemBase> cur = arm.bonebase.first; cur;) {
        const Bone *bone = BlendCastBone(cur.get());
        if (bone == nullptr) {
            break;
        }
        BlendFindBoneRestLocal(bone, nullptr, boneName, out, found);
        if (found) {
            return true;
        }
        cur = bone->next;
    }
    return false;
}

void BlendPushArmatureActionAnim(std::vector<aiAnimation *> &anims, const Assimp::Blender::Object &obj,
        const Assimp::Blender::bAction &act, Assimp::Blender::ConversionData &conv_data) {
    using namespace Assimp::Blender;
    if (!obj.data || (obj.data->dna_type != nullptr && !BlendElemConvertedHullIs(*(obj.data), "bArmature"))) {
        return;
    }
    const bArmature *arm = static_cast<const bArmature *>(obj.data.get());
    const std::string armName = obj.id.name + 2;
    const auto nameIt = conv_data.armatureBoneNodeNames.find(armName);
    if (nameIt == conv_data.armatureBoneNodeNames.end() || nameIt->second.empty()) {
        return;
    }

    std::vector<const FCurve *> fcurves;
    BlendCollectActionFCurves(act, fcurves);
    if (fcurves.empty()) {
        return;
    }

    std::unordered_map<std::string, BlendPoseBoneCurves> byBone;
    for (const FCurve *fc : fcurves) {
        if (fc == nullptr) {
            continue;
        }
        std::string boneName, prop;
        if (!BlendParsePoseBoneRnaPath(fc->rna_path, boneName, prop)) {
            continue;
        }
        if (nameIt->second.find(boneName) == nameIt->second.end()) {
            continue;
        }
        BlendPoseBoneCurves &bag = byBone[boneName];
        if (prop == "location" && fc->array_index >= 0 && fc->array_index < 3) {
            bag.loc[fc->array_index] = fc;
        } else if (prop == "rotation_euler" && fc->array_index >= 0 && fc->array_index < 3) {
            bag.eul[fc->array_index] = fc;
        } else if (prop == "rotation_quaternion" && fc->array_index >= 0 && fc->array_index < 4) {
            bag.quat[fc->array_index] = fc;
        } else if (prop == "scale" && fc->array_index >= 0 && fc->array_index < 3) {
            bag.scl[fc->array_index] = fc;
        }
    }
    if (byBone.empty()) {
        return;
    }

    std::vector<aiNodeAnim *> channels;
    double tmax = 0.0;
    for (const auto &kv : byBone) {
        float restLocal[4][4];
        if (!BlendArmatureBoneRestLocal(*arm, kv.first, restLocal)) {
            continue;
        }
        const auto nodeNameIt = nameIt->second.find(kv.first);
        if (nodeNameIt == nameIt->second.end()) {
            continue;
        }
        aiNodeAnim *ch = BlendBuildPoseBoneNodeAnim(nodeNameIt->second, restLocal, kv.second);
        if (ch == nullptr) {
            continue;
        }
        for (unsigned int k = 0; k < ch->mNumPositionKeys; ++k) {
            tmax = std::max(tmax, ch->mPositionKeys[k].mTime);
        }
        channels.push_back(ch);
    }
    if (channels.empty()) {
        return;
    }

    aiAnimation *anim = new aiAnimation();
    anim->mName.Set(act.id.name[0] ? act.id.name + 2 : armName.c_str());
    anim->mTicksPerSecond = 24.0;
    anim->mDuration = tmax;
    anim->mNumChannels = static_cast<unsigned int>(channels.size());
    anim->mChannels = new aiNodeAnim *[anim->mNumChannels];
    std::copy(channels.begin(), channels.end(), anim->mChannels);
    anims.push_back(anim);
}

void BlendConvertArmatureAnimations(std::vector<aiAnimation *> &anims, Assimp::Blender::ConversionData &conv_data) {
    using namespace Assimp::Blender;
    if (conv_data.armature_objects.empty()) {
        return;
    }
    std::set<std::pair<const Object *, const bAction *>> seen;
    for (const Object *obj : conv_data.armature_objects) {
        if (obj == nullptr || !obj->adt) {
            continue;
        }
        auto tryAct = [&](const std::shared_ptr<bAction> &act) {
            if (!act || !seen.insert({ obj, act.get() }).second) {
                return;
            }
            BlendPushArmatureActionAnim(anims, *obj, *act, conv_data);
        };
        tryAct(obj->adt->action);
        for (std::shared_ptr<ElemBase> tcur = obj->adt->nla_tracks.first; tcur;) {
            const NlaTrack *track = nullptr;
            if (tcur->dna_type == nullptr || BlendElemConvertedHullIs(*(tcur), "NlaTrack")) {
                track = static_cast<const NlaTrack *>(tcur.get());
            }
            if (track == nullptr) {
                break;
            }
            for (std::shared_ptr<ElemBase> scur = track->strips.first; scur;) {
                const NlaStrip *strip = nullptr;
                if (scur->dna_type == nullptr || BlendElemConvertedHullIs(*(scur), "NlaStrip")) {
                    strip = static_cast<const NlaStrip *>(scur.get());
                }
                if (strip == nullptr) {
                    break;
                }
                tryAct(strip->act);
                scur = strip->next;
            }
            tcur = track->next;
        }
    }
}

/** Recursively emit bone aiNodes under an Armature object node. */
void BlendEmitBoneNodeTree(aiNode *parentNode, const Assimp::Blender::Bone *bone,
        const Assimp::Blender::Bone *parentBone, const Assimp::Blender::Object &armObj,
        Assimp::Blender::ConversionData &conv_data, std::vector<aiNode *> &outChildren) {
    using namespace Assimp::Blender;
    if (bone == nullptr || parentNode == nullptr) {
        return;
    }
    const std::string armName = armObj.id.name + 2;
    const std::string boneName = bone->name;
    // Unique Assimp node name: ArmatureObject|BoneName (pose channels target this).
    const std::string nodeName = armName + "|" + boneName;
    conv_data.armatureBoneNodeNames[armName][boneName] = nodeName;

    std::unique_ptr<aiNode> node(new aiNode(nodeName));
    float restLocal[4][4];
    BlendBoneRestLocalM4(bone, parentBone, restLocal);
    BlendCopyMat4ToAi(node->mTransformation, restLocal);

    std::vector<aiNode *> childNodes;
    for (std::shared_ptr<ElemBase> cur = bone->childbase.first; cur;) {
        const Bone *child = BlendCastBone(cur.get());
        if (child == nullptr) {
            break;
        }
        BlendEmitBoneNodeTree(node.get(), child, bone, armObj, conv_data, childNodes);
        cur = child->next;
    }
    if (!childNodes.empty()) {
        node->mNumChildren = static_cast<unsigned int>(childNodes.size());
        node->mChildren = new aiNode *[node->mNumChildren];
        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
            node->mChildren[i] = childNodes[i];
            node->mChildren[i]->mParent = node.get();
        }
    }
    outChildren.push_back(node.release());
}

void BlendAttachArmatureBones(aiNode *armNode, const Assimp::Blender::Object &obj,
        Assimp::Blender::ConversionData &conv_data, std::vector<aiNode *> &boneRoots) {
    using namespace Assimp::Blender;
    if (armNode == nullptr || !obj.data) {
        return;
    }
    if (obj.data->dna_type != nullptr && !BlendElemConvertedHullIs(*(obj.data), "bArmature")) {
        return;
    }
    const bArmature *arm = static_cast<const bArmature *>(obj.data.get());
    for (std::shared_ptr<ElemBase> cur = arm->bonebase.first; cur;) {
        const Bone *bone = BlendCastBone(cur.get());
        if (bone == nullptr) {
            break;
        }
        BlendEmitBoneNodeTree(armNode, bone, nullptr, obj, conv_data, boneRoots);
        cur = bone->next;
    }
}

/** Prefer legacy Image.packedfile; else first ImagePackedFile in packedfiles. */
const Assimp::Blender::PackedFile *BlendImageGetPacked(const Assimp::Blender::Image *img) {
    using namespace Assimp::Blender;
    if (img == nullptr) {
        return nullptr;
    }
    if (img->packedfile) {
        return img->packedfile.get();
    }
    for (std::shared_ptr<ElemBase> cur = img->packedfiles.first; cur;) {
        const ImagePackedFile *ipf = nullptr;
        if (cur->dna_type == nullptr || BlendElemConvertedHullIs(*(cur), "ImagePackedFile")) {
            ipf = static_cast<const ImagePackedFile *>(cur.get());
        }
        if (ipf == nullptr) {
            break;
        }
        if (ipf->packedfile) {
            return ipf->packedfile.get();
        }
        cur = ipf->next;
    }
    return nullptr;
}

/** Blender relative file paths start with "//". Drop that prefix and map remaining '/' to the OS separator. */
std::string BlendNormalizeRelativePath(const char *path) {
    if (path == nullptr) {
        return {};
    }
    std::string out(path);
    if (out.size() >= 2 && out[0] == '/' && out[1] == '/') {
        out.erase(0, 2);
        const char sep = Assimp::DefaultIOSystem().getOsSeparator();
        if (sep != '/') {
            std::replace(out.begin(), out.end(), '/', sep);
        }
    }
    return out;
}

/** Path for format hint / logging: Image.filepath|name, else ImagePackedFile.filepath, else ID name. */
const char *BlendImageDisplayPath(const Assimp::Blender::Image *img) {
    using namespace Assimp::Blender;
    if (img == nullptr) {
        return "";
    }
    if (img->name[0] != '\0') {
        return img->name;
    }
    for (std::shared_ptr<ElemBase> cur = img->packedfiles.first; cur;) {
        const ImagePackedFile *ipf = nullptr;
        if (cur->dna_type == nullptr || BlendElemConvertedHullIs(*(cur), "ImagePackedFile")) {
            ipf = static_cast<const ImagePackedFile *>(cur.get());
        }
        if (ipf == nullptr) {
            break;
        }
        if (ipf->filepath[0] != '\0') {
            return ipf->filepath;
        }
        cur = ipf->next;
    }
    // Presence: DNA ID name (skip "IM" prefix) when filepath was empty (e.g. generated "gradient").
    if (img->id.name[2] != '\0') {
        return img->id.name + 2;
    }
    return img->name;
}

/**
 * Ensure packed Image bytes are in conv_data.textures (dedupe by Image*).
 * Returns true and writes *outIndex when packed payload was resolved.
 */
bool BlendEnsureEmbeddedPackedTexture(const Assimp::Blender::Image *img,
        Assimp::Blender::ConversionData &conv_data, unsigned int *outIndex) {
    using namespace Assimp::Blender;
    if (img == nullptr) {
        return false;
    }
    const PackedFile *packed = BlendImageGetPacked(img);
    if (packed == nullptr || !packed->data || packed->size <= 0) {
        return false;
    }
    const auto found = conv_data.embeddedImageTexIndex.find(img);
    if (found != conv_data.embeddedImageTexIndex.end()) {
        if (outIndex) {
            *outIndex = found->second;
        }
        return true;
    }
    const unsigned int texIndex = static_cast<unsigned int>(conv_data.textures->size());
    conv_data.embeddedImageTexIndex.emplace(img, texIndex);
    conv_data.textures->push_back(new aiTexture());
    aiTexture *curTex = conv_data.textures->back();
    const char *dispPath = BlendImageDisplayPath(img);
    const size_t nlen = std::strlen(dispPath);
    const char *s = dispPath + nlen, *e = s;
    while (s >= dispPath && *s != '.') {
        --s;
    }
    curTex->achFormatHint[0] = s + 1 > e ? '\0' : static_cast<char>(::tolower(static_cast<unsigned char>(s[1])));
    curTex->achFormatHint[1] = s + 2 > e ? '\0' : static_cast<char>(::tolower(static_cast<unsigned char>(s[2])));
    curTex->achFormatHint[2] = s + 3 > e ? '\0' : static_cast<char>(::tolower(static_cast<unsigned char>(s[3])));
    curTex->achFormatHint[3] = '\0';
    curTex->mWidth = static_cast<unsigned int>(packed->size);
    uint8_t *ch = new uint8_t[curTex->mWidth];
    conv_data.db.reader->SetCurrentPos(static_cast<size_t>(packed->data->val));
    conv_data.db.reader->CopyAndAdvance(ch, curTex->mWidth);
    curTex->pcData = reinterpret_cast<aiTexel *>(ch);
    if (dispPath[0] != '\0') {
        const std::string filePath = BlendNormalizeRelativePath(dispPath);
        if (!filePath.empty()) {
            curTex->mFilename = aiString(filePath);
        }
    }
    ASSIMP_LOG_INFO("Reading embedded texture, original file was ", curTex->mFilename.C_Str());
    if (outIndex) {
        *outIndex = texIndex;
    }
    return true;
}

/** Emit every DNA Image ID block that has authored packed bytes (Presence). */
void BlendEmitAllPackedImages(Assimp::Blender::ConversionData &conv_data,
        std::vector<std::shared_ptr<Assimp::Blender::Image>> &owned) {
    using namespace Assimp::Blender;
    const auto it = conv_data.db.dna.indices.find("Image");
    if (it == conv_data.db.dna.indices.end()) {
        return;
    }
    const Structure &ss = conv_data.db.dna.structures[it->second];
    DNA::FactoryPair builders = conv_data.db.dna.GetBlobToStructureConverter(ss, conv_data.db);
    if (!builders.first || !builders.second) {
        return;
    }
    for (const FileBlockHead &bl : conv_data.db.entries) {
        if (bl.id != "IM" || bl.dna_index != it->second) {
            continue;
        }
        // Reuse ObjectCache when this Image was already converted via materials.
        std::shared_ptr<Image> img;
        conv_data.db.cache(img).get(ss, img, bl.address, 0);
        if (!img) {
            const auto pold = conv_data.db.reader->GetCurrentPos();
            const Pointer prevScope = conv_data.db.active_id_scope;
            conv_data.db.reader->SetCurrentPos(bl.start);
            conv_data.db.active_id_scope = bl.address;
            std::shared_ptr<ElemBase> base = (ss.*builders.first)();
            img = std::static_pointer_cast<Image>(base);
            // Cache before Convert to match ResolvePointer cyclic-safe order.
            conv_data.db.cache(img).set(ss, img, bl.address, 0);
            (ss.*builders.second)(base, conv_data.db);
            BlendElemBindConvertedHull(*base, ss.name.c_str(), ss.name.c_str());
            conv_data.db.active_id_scope = prevScope;
            conv_data.db.reader->SetCurrentPos(pold);
        }
        if (!img) {
            continue;
        }
        owned.push_back(img);
        BlendEnsureEmbeddedPackedTexture(img.get(), conv_data, nullptr);
    }
}

struct BlendNodeGraph;
BlendNodeGraph BlendBuildGraph(const Assimp::Blender::bNodeTree &tree);

struct BlendNodeGraph {
    std::vector<const Assimp::Blender::bNode *> nodes;
    std::vector<const Assimp::Blender::bNodeLink *> links;

    const Assimp::Blender::bNodeLink *FindLinkToSocket(const Assimp::Blender::bNodeSocket &sock) const {
        for (const Assimp::Blender::bNodeLink *l : links) {
            if (l->tosock.get() == static_cast<const Assimp::Blender::ElemBase *>(&sock)) {
                return l;
            }
            const Assimp::Blender::bNodeSocket *ts = BlendCastSocket(l->tosock.get());
            if (ts == &sock) {
                return l;
            }
        }
        return nullptr;
    }

    /** Stack frame while inside a ShaderNodeGroup: host graph owns the instance node. */
    struct GroupWalkFrame {
        const BlendNodeGraph *host = nullptr;
        const Assimp::Blender::bNode *instance = nullptr;
    };

    /** Walk bNodeLink upstream until ShaderNodeTexImage. No filename name-score.
     *  Barriers: BSDF / Material Output / Mix Shader. Group: enter nodetree via
     *  NodeGroupOutput matching fromsock; NodeGroupInput continues on the outer
     *  group instance inputs (host graph).
     *  Mix Color/RGB (Blender ramp_blend): result starts as Color1; Color2 blended by Fac.
     *  Fac~0 -> Color1 only; Fac~1 -> Color2 only; else Color1 first (accumulator base).
     *  Mix never treats Fac as the color/normal source (Fac is a mask only). */
    const Assimp::Blender::bNode *FollowToTexImage(const Assimp::Blender::bNode *n, int depth = 0,
            const Assimp::Blender::bNodeSocket *fromOutputSock = nullptr,
            std::vector<GroupWalkFrame> *groupStack = nullptr) const {
        using namespace Assimp::Blender;
        if (n == nullptr || depth > 24) {
            return nullptr;
        }
        if (BlendNodeIsTexImage(*n)) {
            return n;
        }
        if (BlendNodeIsTexWalkBarrier(*n)) {
            return nullptr;
        }

        auto followLink = [&](const bNodeLink *l) -> const bNode * {
            if (!l) {
                return nullptr;
            }
            const bNodeSocket *outSock = BlendCastSocket(l->fromsock.get());
            return FollowToTexImage(BlendCastNode(l->fromnode.get()), depth + 1, outSock, groupStack);
        };
        auto tryFromSock = [&](const bNodeSocket &sock) -> const bNode * {
            return followLink(FindLinkToSocket(sock));
        };
        auto sockNameHit = [](const bNodeSocket &a, const bNodeSocket &b) -> bool {
            if (BlendSockNameIs(a, b.name) || BlendSockNameIs(b, a.name)) {
                return true;
            }
            if (a.identifier[0] != '\0' && b.identifier[0] != '\0' &&
                    std::strcmp(a.identifier, b.identifier) == 0) {
                return true;
            }
            if (a.identifier[0] != '\0' &&
                    (BlendSockNameIs(b, a.identifier) || std::strcmp(b.name, a.identifier) == 0)) {
                return true;
            }
            if (b.identifier[0] != '\0' &&
                    (BlendSockNameIs(a, b.identifier) || std::strcmp(a.name, b.identifier) == 0)) {
                return true;
            }
            return false;
        };

        // NodeGroupInput: continue on the enclosing ShaderNodeGroup's matching input.
        if (BlendNodeIsGroupInput(*n)) {
            if (groupStack == nullptr || groupStack->empty() || fromOutputSock == nullptr) {
                return nullptr;
            }
            const GroupWalkFrame frame = groupStack->back();
            if (frame.host == nullptr || frame.instance == nullptr) {
                return nullptr;
            }
            for (std::shared_ptr<ElemBase> cur = frame.instance->inputs.first; cur;) {
                const bNodeSocket *is = BlendCastSocket(cur.get());
                if (is == nullptr) {
                    break;
                }
                if (!sockNameHit(*is, *fromOutputSock)) {
                    cur = is->next;
                    continue;
                }
                const bNodeLink *il = frame.host->FindLinkToSocket(*is);
                if (!il) {
                    return nullptr;
                }
                groupStack->pop_back();
                const bNode *hit = frame.host->FollowToTexImage(BlendCastNode(il->fromnode.get()),
                        depth + 1, BlendCastSocket(il->fromsock.get()), groupStack);
                groupStack->push_back(frame);
                return hit;
            }
            return nullptr;
        }

        // ShaderNodeGroup: resolve inside the group tree from the matching Group Output input.
        if (BlendNodeIsGroup(*n)) {
            const bNodeTree *gt = BlendNodeGroupTree(*n);
            if (gt == nullptr || fromOutputSock == nullptr) {
                return nullptr;
            }
            const BlendNodeGraph gg = BlendBuildGraph(*gt);
            std::vector<GroupWalkFrame> localStack;
            std::vector<GroupWalkFrame> *st = groupStack ? groupStack : &localStack;
            st->push_back({ this, n });
            auto followInGroup = [&](const bNodeLink *il) -> const bNode * {
                if (!il) {
                    return nullptr;
                }
                return gg.FollowToTexImage(BlendCastNode(il->fromnode.get()), depth + 1,
                        BlendCastSocket(il->fromsock.get()), st);
            };
            for (const bNode *gn : gg.nodes) {
                if (gn == nullptr || !BlendNodeIsGroupOutput(*gn)) {
                    continue;
                }
                for (std::shared_ptr<ElemBase> cur = gn->inputs.first; cur;) {
                    const bNodeSocket *is = BlendCastSocket(cur.get());
                    if (is == nullptr) {
                        break;
                    }
                    if (!sockNameHit(*is, *fromOutputSock)) {
                        cur = is->next;
                        continue;
                    }
                    if (const bNode *hit = followInGroup(gg.FindLinkToSocket(*is))) {
                        st->pop_back();
                        return hit;
                    }
                    cur = is->next;
                }
            }
            // Fallback: linked Group Output inputs, Color/Image before Fac (socket rename safe).
            struct GCand {
                int prio;
                const bNodeLink *link;
            };
            std::vector<GCand> gcands;
            for (const bNode *gn : gg.nodes) {
                if (gn == nullptr || !BlendNodeIsGroupOutput(*gn)) {
                    continue;
                }
                for (std::shared_ptr<ElemBase> cur = gn->inputs.first; cur;) {
                    const bNodeSocket *is = BlendCastSocket(cur.get());
                    if (is == nullptr) {
                        break;
                    }
                    if (const bNodeLink *il = gg.FindLinkToSocket(*is)) {
                        gcands.push_back({ BlendSockTexWalkPriority(*is), il });
                    }
                    cur = is->next;
                }
            }
            std::stable_sort(gcands.begin(), gcands.end(),
                    [](const GCand &a, const GCand &b) { return a.prio > b.prio; });
            for (const GCand &c : gcands) {
                if (const bNode *hit = followInGroup(c.link)) {
                    st->pop_back();
                    return hit;
                }
            }
            st->pop_back();
            return nullptr;
        }

        // MixRGB / ShaderNodeMix: Color1 is ramp_blend accumulator; Color2 is blend-in (Fac).
        // ShaderNodeMix (type 713) exposes parallel A/B/Factor sockets per datatype
        // (Float/Vector/Color/Rotation). Prefer Color (and non-Vector Factor); never keep
        // the last homonym (A_Rotation) or Fac walks miss Image Texture links.
        if (BlendNodeIsMixColor(*n)) {
            const bNodeSocket *sockA = nullptr;
            const bNodeSocket *sockB = nullptr;
            const bNodeSocket *sockFac = nullptr;
            auto mixIdentOk = [](const bNodeSocket &sock, const char *colorTag) -> bool {
                if (sock.identifier[0] == '\0') {
                    return true; // legacy MixRGB: identifier often empty
                }
                if (std::strcmp(sock.identifier, sock.name) == 0) {
                    return true;
                }
                return std::strstr(sock.identifier, colorTag) != nullptr;
            };
            for (std::shared_ptr<ElemBase> cur = n->inputs.first; cur;) {
                const bNodeSocket *sock = BlendCastSocket(cur.get());
                if (sock == nullptr) {
                    break;
                }
                if (BlendSockNameIsAny(*sock, { "Color1", "ColorA" }) ||
                        (BlendSockNameIs(*sock, "A") && mixIdentOk(*sock, "Color"))) {
                    sockA = sock;
                } else if (BlendSockNameIsAny(*sock, { "Color2", "ColorB" }) ||
                           (BlendSockNameIs(*sock, "B") && mixIdentOk(*sock, "Color"))) {
                    sockB = sock;
                } else if (BlendSockNameIsAny(*sock, { "Fac", "Factor" }) &&
                           (sock->identifier[0] == '\0' || std::strstr(sock->identifier, "Vector") == nullptr)) {
                    sockFac = sock;
                }
                cur = sock->next;
            }
            // Fac from DNA only - never invent 0.5 when default_value is unread.
            // Linked non-constant Fac is a mask: do not use socket default for extremes.
            bool haveFac = false;
            float fac = 0.f;
            bool facLinkedNonConst = false;
            if (sockFac) {
                const bNodeLink *fl = FindLinkToSocket(*sockFac);
                if (fl) {
                    float linked = 0.f;
                    if (ReadConstantFloat(BlendCastNode(fl->fromnode.get()), linked)) {
                        fac = linked;
                        haveFac = true;
                    } else {
                        facLinkedNonConst = true;
                    }
                } else if (BlendReadSocketFloat(*sockFac, fac)) {
                    haveFac = true;
                }
            }
            if (facLinkedNonConst) {
                haveFac = false; // mid path: Color1 accumulator first
            }
            // MixRGB MA_RAMP_BLEND extremes only (custom1 = blend type). ShaderNodeMix
            // custom1 is data_type - never treat as blend; Color1 accumulator first.
            const bool blendMode = BlendNodeIsMixRgbBlendType(*n) && (n->custom1 == 0);
            if (haveFac && blendMode && fac <= 1e-4f) {
                if (sockA) {
                    if (const bNode *hit = tryFromSock(*sockA)) {
                        return hit;
                    }
                }
            } else if (haveFac && blendMode && fac >= 1.f - 1e-4f) {
                if (sockB) {
                    if (const bNode *hit = tryFromSock(*sockB)) {
                        return hit;
                    }
                }
            } else {
                // Mid Fac, unknown Fac, or Multiply/Overlay/...: Color1 first, then Color2.
                if (sockA) {
                    if (const bNode *hit = tryFromSock(*sockA)) {
                        return hit;
                    }
                }
                if (sockB) {
                    if (const bNode *hit = tryFromSock(*sockB)) {
                        return hit;
                    }
                }
            }
            // Do not fall through to Fac / other sockets: Fac is a mask, not the map.
            return nullptr;
        }

        struct Cand {
            int prio;
            const bNodeLink *link;
        };
        std::vector<Cand> cands;
        for (std::shared_ptr<ElemBase> cur = n->inputs.first; cur;) {
            const bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            if (const bNodeLink *l = FindLinkToSocket(*sock)) {
                cands.push_back({ BlendSockTexWalkPriority(*sock), l });
            }
            cur = sock->next;
        }
        std::stable_sort(cands.begin(), cands.end(),
                [](const Cand &a, const Cand &b) { return a.prio > b.prio; });

        for (const Cand &c : cands) {
            if (const bNode *hit = followLink(c.link)) {
                return hit;
            }
        }
        return nullptr;
    }

    bool ReadConstantColor(const Assimp::Blender::bNode *n, float out[4], int depth = 0) const {
        using namespace Assimp::Blender;
        if (n == nullptr || depth > 6) {
            return false;
        }
        // Presence: only RGB sockets/nodes are colors. ShaderNodeValue is a float
        // factor - never promote v -> (v,v,v) into an albedo/emission constant
        // (tiger_body Value=-20 became baseColor [-20,-20,-20]).
        if (BlendNodeIsRGB(*n)) {
            // RGB node: first output socket carries the color default.
            for (std::shared_ptr<ElemBase> cur = n->outputs.first; cur;) {
                const bNodeSocket *sock = BlendCastSocket(cur.get());
                if (sock == nullptr) {
                    break;
                }
                BlendReadSocketColor(*sock, out);
                return true;
            }
        }
        // Follow linked inputs (same priority as Tex walk) until an RGB node.
        if (!BlendNodeIsTexWalkBarrier(*n) && !BlendNodeIsNormalMap(*n) && !BlendNodeIsBump(*n) &&
                !BlendNodeIsTexImage(*n) && !BlendNodeIsValue(*n)) {
            struct Cand {
                int prio;
                const bNodeLink *link;
            };
            std::vector<Cand> cands;
            for (std::shared_ptr<ElemBase> cur = n->inputs.first; cur;) {
                const bNodeSocket *sock = BlendCastSocket(cur.get());
                if (sock == nullptr) {
                    break;
                }
                if (const bNodeLink *l = FindLinkToSocket(*sock)) {
                    cands.push_back({ BlendSockTexWalkPriority(*sock), l });
                }
                cur = sock->next;
            }
            std::stable_sort(cands.begin(), cands.end(),
                    [](const Cand &a, const Cand &b) { return a.prio > b.prio; });
            for (const Cand &c : cands) {
                if (ReadConstantColor(BlendCastNode(c.link->fromnode.get()), out, depth + 1)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool ReadConstantFloat(const Assimp::Blender::bNode *n, float &out, int depth = 0) const {
        using namespace Assimp::Blender;
        if (n == nullptr || depth > 6) {
            return false;
        }
        if (BlendNodeIsValue(*n)) {
            for (std::shared_ptr<ElemBase> cur = n->outputs.first; cur;) {
                const bNodeSocket *sock = BlendCastSocket(cur.get());
                if (sock == nullptr) {
                    break;
                }
                if (BlendReadSocketFloat(*sock, out)) {
                    return true;
                }
                cur = sock->next;
            }
            return false;
        }
        if (BlendNodeIsRGB(*n)) {
            float tmp[4]{};
            for (std::shared_ptr<ElemBase> cur = n->outputs.first; cur;) {
                const bNodeSocket *sock = BlendCastSocket(cur.get());
                if (sock == nullptr) {
                    break;
                }
                if (BlendReadSocketColor(*sock, tmp)) {
                    out = tmp[0];
                    return true;
                }
                cur = sock->next;
            }
            return false;
        }
        if (!BlendNodeIsTexWalkBarrier(*n) && !BlendNodeIsNormalMap(*n) && !BlendNodeIsBump(*n) &&
                !BlendNodeIsTexImage(*n)) {
            struct Cand {
                int prio;
                const bNodeLink *link;
            };
            std::vector<Cand> cands;
            for (std::shared_ptr<ElemBase> cur = n->inputs.first; cur;) {
                const bNodeSocket *sock = BlendCastSocket(cur.get());
                if (sock == nullptr) {
                    break;
                }
                if (const bNodeLink *l = FindLinkToSocket(*sock)) {
                    cands.push_back({ BlendSockTexWalkPriority(*sock), l });
                }
                cur = sock->next;
            }
            std::stable_sort(cands.begin(), cands.end(),
                    [](const Cand &a, const Cand &b) { return a.prio > b.prio; });
            for (const Cand &c : cands) {
                if (ReadConstantFloat(BlendCastNode(c.link->fromnode.get()), out, depth + 1)) {
                    return true;
                }
            }
        }
        return false;
    }

    int Rank(const Assimp::Blender::bNode &n) const {
        if (BlendNodeIsPrincipled(n)) return 5;
        if (BlendNodeIsGlass(n)) return 4;
        if (BlendNodeIsGlossy(n)) return 3;
        if (BlendNodeIsDiffuse(n)) return 2;
        if (BlendNodeIsEmission(n)) return 1;
        return 0;
    }

    int DetailScore(const Assimp::Blender::bNode &n) const {
        // Prefer the Principled that actually carries authored defaults / maps.
        int score = 0;
        if (!BlendNodeIsPrincipled(n) && !BlendNodeIsGlass(n) && !BlendNodeIsGlossy(n) && !BlendNodeIsDiffuse(n)) {
            return score;
        }
        for (std::shared_ptr<Assimp::Blender::ElemBase> cur = n.inputs.first; cur;) {
            const Assimp::Blender::bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            if (FindLinkToSocket(*sock)) {
                score += 3;
            }
            if (BlendSockNameIsAny(*sock, { "Base Color", "BaseColor", "Color" })) {
                float c[4]{};
                if (BlendReadSocketColor(*sock, c)) {
                    score += 5;
                }
            }
            if (BlendSockNameIs(*sock, "Roughness")) {
                float r = 0.f;
                if (BlendReadSocketFloat(*sock, r)) {
                    score += 4;
                }
            }
            cur = sock->next;
        }
        return score;
    }

    const Assimp::Blender::bNode *PickBest(const Assimp::Blender::bNode *a, const Assimp::Blender::bNode *b) const {
        if (a == nullptr) return b;
        if (b == nullptr) return a;
        const int ra = Rank(*a), rb = Rank(*b);
        if (ra != rb) {
            return ra > rb ? a : b;
        }
        const int sa = DetailScore(*a), sb = DetailScore(*b);
        return sa >= sb ? a : b;
    }

    const Assimp::Blender::bNode *ResolveSurface(const Assimp::Blender::bNode *n, bool &sawMix,
            bool *sawAdd = nullptr, int depth = 0) const {
        using namespace Assimp::Blender;
        if (n == nullptr || depth > 10) {
            return nullptr;
        }
        if (BlendNodeIsFlattenable(*n) && !BlendNodeIsEmission(*n)) {
            return n;
        }
        if (BlendNodeIsEmission(*n)) {
            return n;
        }
        if (BlendNodeIsMixShader(*n) || BlendNodeIsAddShader(*n)) {
            if (BlendNodeIsMixShader(*n)) {
                sawMix = true;
            }
            if (BlendNodeIsAddShader(*n) && sawAdd != nullptr) {
                *sawAdd = true;
            }
            const bNode *best = nullptr;
            for (std::shared_ptr<ElemBase> cur = n->inputs.first; cur;) {
                const bNodeSocket *sock = BlendCastSocket(cur.get());
                if (sock == nullptr) {
                    break;
                }
                if (BlendSockNameIsAny(*sock, { "Shader", "Shader_001", "Closure" }) ||
                        BlendSockNameIs(*sock, "Shader")) {
                    const bNodeLink *l = FindLinkToSocket(*sock);
                    if (l) {
                        best = PickBest(best,
                                ResolveSurface(BlendCastNode(l->fromnode.get()), sawMix, sawAdd, depth + 1));
                    }
                }
                // Mix/Add Shader inputs are often just named "Shader" twice - take every linked input.
                const bNodeLink *l = FindLinkToSocket(*sock);
                if (l && !BlendSockNameIs(*sock, "Fac")) {
                    best = PickBest(best,
                            ResolveSurface(BlendCastNode(l->fromnode.get()), sawMix, sawAdd, depth + 1));
                }
                cur = sock->next;
            }
            return best;
        }
        return nullptr;
    }

    /** Walk Mix/Add chains and collect every Emission leaf (Presence merge later). */
    void CollectEmissionLeaves(const Assimp::Blender::bNode *n,
            std::vector<const Assimp::Blender::bNode *> &out, int depth = 0) const {
        using namespace Assimp::Blender;
        if (n == nullptr || depth > 10) {
            return;
        }
        if (BlendNodeIsEmission(*n)) {
            out.push_back(n);
            return;
        }
        if (!BlendNodeIsMixShader(*n) && !BlendNodeIsAddShader(*n)) {
            return;
        }
        for (std::shared_ptr<ElemBase> cur = n->inputs.first; cur;) {
            const bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            const bNodeLink *l = FindLinkToSocket(*sock);
            if (l && !BlendSockNameIs(*sock, "Fac")) {
                CollectEmissionLeaves(BlendCastNode(l->fromnode.get()), out, depth + 1);
            }
            cur = sock->next;
        }
    }
};

BlendNodeGraph BlendBuildGraph(const Assimp::Blender::bNodeTree &tree) {
    using namespace Assimp::Blender;
    BlendNodeGraph g;
    for (std::shared_ptr<ElemBase> cur = tree.nodes.first; cur;) {
        const bNode *n = BlendCastNode(cur.get());
        if (n == nullptr) {
            break;
        }
        g.nodes.push_back(n);
        cur = n->next;
    }
    for (std::shared_ptr<ElemBase> cur = tree.links.first; cur;) {
        const bNodeLink *l = BlendCastLink(cur.get());
        if (l == nullptr) {
            break;
        }
        g.links.push_back(l);
        cur = l->next;
    }
    return g;
}

/** Follow Image Texture Vector input to ShaderNodeUVMap.uv_map (empty = active/TexCoord).
 *  TexImage often lives inside a ShaderNodeGroup - resolve against the graph that owns
 *  the node's Vector link (outer material graph alone misses inner links). */
void BlendResolveTexImageUvName(const BlendNodeGraph &g, const Assimp::Blender::bNode &texNode, char uvname[64]) {
    using namespace Assimp::Blender;
    BlendCopyUvName(uvname, nullptr);
    if (!BlendNodeIsTexImage(texNode)) {
        return;
    }

    auto graphHasNode = [](const BlendNodeGraph &gg, const bNode &node) -> bool {
        for (const bNode *n : gg.nodes) {
            if (n == &node) {
                return true;
            }
        }
        return false;
    };

    auto resolveInGraph = [&](const BlendNodeGraph &gg) -> bool {
        if (!graphHasNode(gg, texNode)) {
            return false;
        }
        for (std::shared_ptr<ElemBase> cur = texNode.inputs.first; cur;) {
            const bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            if (!BlendSockNameIsAny(*sock, { "Vector", "Vector2" })) {
                cur = sock->next;
                continue;
            }
            const bNodeLink *l = gg.FindLinkToSocket(*sock);
            if (l == nullptr) {
                // Unlinked Vector = Blender active/render UV (empty name).
                return true;
            }
            const bNode *src = BlendCastNode(l->fromnode.get());
            for (int depth = 0; src != nullptr && depth < 12; ++depth) {
                if (BlendNodeIsUVMap(*src)) {
                    if (src->storage && src->storage->dna_type != nullptr &&
                            BlendElemConvertedHullIs(*(src->storage), "NodeShaderUVMap")) {
                        const auto *st = static_cast<const NodeShaderUVMap *>(src->storage.get());
                        BlendCopyUvName(uvname, st->uv_map);
                    }
                    return true;
                }
                if (BlendNodeIsTexCoord(*src)) {
                    return true; // empty name -> active/render
                }
                const bool passthroughVec =
                        (src->idname[0] != '\0' &&
                                (std::strcmp(src->idname, "NodeReroute") == 0 ||
                                        std::strcmp(src->idname, "ShaderNodeMapping") == 0 ||
                                        std::strcmp(src->idname, "ShaderNodeVectorMath") == 0 ||
                                        std::strcmp(src->idname, "ShaderNodeVectorTransform") == 0));
                if (!passthroughVec) {
                    break;
                }
                const bNodeLink *rl = nullptr;
                for (std::shared_ptr<ElemBase> ic = src->inputs.first; ic;) {
                    const bNodeSocket *is = BlendCastSocket(ic.get());
                    if (is == nullptr) {
                        break;
                    }
                    if (std::strcmp(src->idname, "ShaderNodeMapping") == 0 &&
                            !BlendSockNameIsAny(*is, { "Vector", "Vector2" })) {
                        ic = is->next;
                        continue;
                    }
                    if (std::strcmp(src->idname, "ShaderNodeVectorMath") == 0 &&
                            !BlendSockNameIsAny(*is, { "Vector", "Vector2", "A" })) {
                        ic = is->next;
                        continue;
                    }
                    rl = gg.FindLinkToSocket(*is);
                    if (rl) {
                        break;
                    }
                    ic = is->next;
                }
                src = rl ? BlendCastNode(rl->fromnode.get()) : nullptr;
            }
            return true;
        }
        return true; // tex in graph, no Vector socket
    };

    if (resolveInGraph(g)) {
        return;
    }

    // Nested ShaderNodeGroup trees (Image Tex inside group).
    std::function<bool(const BlendNodeGraph &, int)> walkGroups;
    walkGroups = [&](const BlendNodeGraph &gg, int depth) -> bool {
        if (depth > 8) {
            return false;
        }
        for (const bNode *n : gg.nodes) {
            if (n == nullptr || !BlendNodeIsGroup(*n)) {
                continue;
            }
            const bNodeTree *gt = BlendNodeGroupTree(*n);
            if (gt == nullptr) {
                continue;
            }
            const BlendNodeGraph sub = BlendBuildGraph(*gt);
            if (resolveInGraph(sub)) {
                return true;
            }
            if (walkGroups(sub, depth + 1)) {
                return true;
            }
        }
        return false;
    };
    walkGroups(g, 0);
}

/** Follow TexImage Vector through Mapping -> Presence ofs/scale/rot (no invent). */
void BlendResolveTexImageUvTrafo(const BlendNodeGraph &g, const Assimp::Blender::bNode &texNode, BlendTexMap &out) {
    using namespace Assimp::Blender;
    out.hasUvTrafo = false;
    if (!BlendNodeIsTexImage(texNode)) {
        return;
    }
    auto graphHasNode = [](const BlendNodeGraph &gg, const bNode &node) -> bool {
        for (const bNode *n : gg.nodes) {
            if (n == &node) {
                return true;
            }
        }
        return false;
    };
    auto readMapping = [&](const BlendNodeGraph &gg, const bNode &mapNode) -> bool {
        if (!BlendNodeIsMapping(mapNode)) {
            return false;
        }
        float loc[3] = { 0, 0, 0 };
        float rot[3] = { 0, 0, 0 };
        float scl[3] = { 1, 1, 1 };
        bool gotLoc = false, gotRot = false, gotScl = false;
        for (std::shared_ptr<ElemBase> cur = mapNode.inputs.first; cur;) {
            const bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            if (gg.FindLinkToSocket(*sock) == nullptr) {
                float vec[3] = { 0, 0, 0 };
                if (BlendReadSocketVector(*sock, vec)) {
                    if (BlendSockNameIsAny(*sock, { "Location" })) {
                        loc[0] = vec[0];
                        loc[1] = vec[1];
                        loc[2] = vec[2];
                        gotLoc = true;
                    } else if (BlendSockNameIsAny(*sock, { "Rotation" })) {
                        rot[0] = vec[0];
                        rot[1] = vec[1];
                        rot[2] = vec[2];
                        gotRot = true;
                    } else if (BlendSockNameIsAny(*sock, { "Scale" })) {
                        scl[0] = vec[0];
                        scl[1] = vec[1];
                        scl[2] = vec[2];
                        gotScl = true;
                    }
                }
            }
            cur = sock->next;
        }
        // Pre-2.81 TexMapping storage when sockets had no readable defaults.
        if ((!gotLoc || !gotRot || !gotScl) && mapNode.storage && mapNode.storage->dna_type &&
                BlendElemConvertedHullIs(*(mapNode.storage), "TexMapping")) {
            const auto *tm = static_cast<const TexMapping *>(mapNode.storage.get());
            if (!gotLoc) {
                loc[0] = tm->loc[0];
                loc[1] = tm->loc[1];
                loc[2] = tm->loc[2];
                gotLoc = true;
            }
            if (!gotRot) {
                rot[0] = tm->rot[0];
                rot[1] = tm->rot[1];
                rot[2] = tm->rot[2];
                gotRot = true;
            }
            if (!gotScl) {
                scl[0] = tm->size[0];
                scl[1] = tm->size[1];
                scl[2] = tm->size[2];
                gotScl = true;
            }
        }
        if (!gotLoc && !gotRot && !gotScl) {
            return false;
        }
        out.uvOfs[0] = loc[0];
        out.uvOfs[1] = loc[1];
        out.uvScale[0] = scl[0];
        out.uvScale[1] = scl[1];
        // UV plane: Blender Mapping Rotation Z drives 2D UV rotate (radians).
        out.uvRot = rot[2];
        out.hasUvTrafo = true;
        return true;
    };
    auto resolveInGraph = [&](const BlendNodeGraph &gg) -> bool {
        if (!graphHasNode(gg, texNode)) {
            return false;
        }
        for (std::shared_ptr<ElemBase> cur = texNode.inputs.first; cur;) {
            const bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            if (!BlendSockNameIsAny(*sock, { "Vector", "Vector2" })) {
                cur = sock->next;
                continue;
            }
            const bNodeLink *l = gg.FindLinkToSocket(*sock);
            if (l == nullptr) {
                return false;
            }
            const bNode *src = BlendCastNode(l->fromnode.get());
            for (int depth = 0; src != nullptr && depth < 12; ++depth) {
                if (BlendNodeIsMapping(*src)) {
                    return readMapping(gg, *src);
                }
                const bool passthrough =
                        (src->idname[0] != '\0' &&
                                (std::strcmp(src->idname, "NodeReroute") == 0 ||
                                        std::strcmp(src->idname, "ShaderNodeVectorMath") == 0 ||
                                        std::strcmp(src->idname, "ShaderNodeVectorTransform") == 0));
                if (!passthrough) {
                    break;
                }
                const bNodeLink *rl = nullptr;
                for (std::shared_ptr<ElemBase> ic = src->inputs.first; ic;) {
                    const bNodeSocket *is = BlendCastSocket(ic.get());
                    if (is == nullptr) {
                        break;
                    }
                    rl = gg.FindLinkToSocket(*is);
                    if (rl) {
                        break;
                    }
                    ic = is->next;
                }
                src = rl ? BlendCastNode(rl->fromnode.get()) : nullptr;
            }
            return false;
        }
        return false;
    };
    if (resolveInGraph(g)) {
        return;
    }
    std::function<bool(const BlendNodeGraph &, int)> walkGroups;
    walkGroups = [&](const BlendNodeGraph &gg, int depth) -> bool {
        if (depth > 8) {
            return false;
        }
        for (const bNode *n : gg.nodes) {
            if (n == nullptr || !BlendNodeIsGroup(*n)) {
                continue;
            }
            const bNodeTree *gt = BlendNodeGroupTree(*n);
            if (gt == nullptr) {
                continue;
            }
            const BlendNodeGraph sub = BlendBuildGraph(*gt);
            if (resolveInGraph(sub)) {
                return true;
            }
            if (walkGroups(sub, depth + 1)) {
                return true;
            }
        }
        return false;
    };
    walkGroups(g, 0);
}

void BlendFillTexMap(const BlendNodeGraph &g, const Assimp::Blender::bNode *texNode, BlendTexMap &out) {
    out.img = nullptr;
    out.hasUvTrafo = false;
    BlendCopyUvName(out.uvname, nullptr);
    if (texNode == nullptr) {
        return;
    }
    out.img = BlendNodeImage(*texNode);
    if (out.img) {
        BlendResolveTexImageUvName(g, *texNode, out.uvname);
        BlendResolveTexImageUvTrafo(g, *texNode, out);
    }
}

// Returns true if a numeric/color factor was resolved (typed default_value or linked constant).
// Texture maps alone do not count as a factor (mapOut may still be set).
bool BlendReadLinkedOrDefaultFloat(const BlendNodeGraph &g, const Assimp::Blender::bNodeSocket &sock,
        float &out, BlendTexMap *mapOut) {
    using namespace Assimp::Blender;
    bool got = BlendReadSocketFloat(sock, out);
    const bNodeLink *l = g.FindLinkToSocket(sock);
    if (!l) {
        return got;
    }
    const bNode *src = BlendCastNode(l->fromnode.get());
    const bNode *tex = g.FollowToTexImage(src, 0, BlendCastSocket(l->fromsock.get()));
    if (tex && mapOut) {
        BlendFillTexMap(g, tex, *mapOut);
    }
    float linked = out;
    if (g.ReadConstantFloat(src, linked)) {
        out = linked;
        return true;
    }
    // Linked: unused socket default_value is not authored for this input.
    return false;
}

bool BlendReadLinkedOrDefaultColor(const BlendNodeGraph &g, const Assimp::Blender::bNodeSocket &sock,
        float out[4], BlendTexMap *mapOut) {
    using namespace Assimp::Blender;
    bool got = BlendReadSocketColor(sock, out);
    const bNodeLink *l = g.FindLinkToSocket(sock);
    if (!l) {
        return got;
    }
    const bNode *src = BlendCastNode(l->fromnode.get());
    const bNode *tex = g.FollowToTexImage(src, 0, BlendCastSocket(l->fromsock.get()));
    if (tex && mapOut) {
        BlendFillTexMap(g, tex, *mapOut);
    }
    float linked[4] = { out[0], out[1], out[2], out[3] };
    if (g.ReadConstantColor(src, linked)) {
        out[0] = linked[0];
        out[1] = linked[1];
        out[2] = linked[2];
        out[3] = linked[3];
        return true;
    }
    // Linked: do not treat the dormant Base Color default as albedo (map may
    // still be set). Never invent a constant; never promote ShaderNodeValue.
    return false;
}

BlendShaderFlat BlendFlattenFromNode(const BlendNodeGraph &g, const Assimp::Blender::bNode &node) {
    BlendShaderFlat flat;
    using namespace Assimp::Blender;

    auto foreachInput = [&](const auto &fn) {
        for (std::shared_ptr<ElemBase> cur = node.inputs.first; cur;) {
            const bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            fn(*sock);
            cur = sock->next;
        }
    };

    auto finish = [&]() {
        // Node recognized only counts as flatten success if we resolved something real.
        flat.ok = flat.HasAnyFactor();
        return flat;
    };

    if (BlendNodeIsPrincipled(node)) {
        flat.output = "Principled BSDF";
        foreachInput([&](const bNodeSocket &sock) {
            if (BlendSockNameIsAny(sock, { "Base Color", "BaseColor" })) {
                if (BlendReadLinkedOrDefaultColor(g, sock, flat.baseColor, &flat.mapBaseColor)) {
                    flat.hasBaseColor = true;
                }
            } else if (BlendSockNameIs(sock, "Metallic")) {
                if (BlendReadLinkedOrDefaultFloat(g, sock, flat.metallic, &flat.mapMetallic)) {
                    flat.hasMetallic = true;
                }
            } else if (BlendSockNameIs(sock, "Roughness")) {
                if (BlendReadLinkedOrDefaultFloat(g, sock, flat.roughness, &flat.mapRoughness)) {
                    flat.hasRoughness = true;
                }
            } else if (BlendSockNameIs(sock, "IOR")) {
                if (BlendReadSocketFloat(sock, flat.ior)) {
                    flat.hasIor = true;
                }
            } else if (BlendSockNameIs(sock, "Alpha")) {
                if (BlendReadSocketFloat(sock, flat.alpha)) {
                    flat.hasAlpha = true;
                }
            } else if (BlendSockNameIsAny(sock, { "Transmission", "Transmission Weight" })) {
                if (BlendReadSocketFloat(sock, flat.transmission)) {
                    flat.hasTransmission = true;
                }
            } else if (BlendSockNameIsAny(sock, { "Coat Weight", "Clearcoat" })) {
                if (BlendReadSocketFloat(sock, flat.coatWeight)) {
                    flat.hasCoatWeight = true;
                }
            } else if (BlendSockNameIsAny(sock, { "Coat Roughness", "Clearcoat Roughness" })) {
                if (BlendReadSocketFloat(sock, flat.coatRoughness)) {
                    flat.hasCoatRoughness = true;
                }
            } else if (BlendSockNameIsAny(sock, { "Emission Color", "Emission" })) {
                float tmp[4]{};
                if (BlendReadLinkedOrDefaultColor(g, sock, tmp, &flat.mapEmission)) {
                    flat.emissionColor[0] = tmp[0];
                    flat.emissionColor[1] = tmp[1];
                    flat.emissionColor[2] = tmp[2];
                    flat.hasEmissionColor = true;
                }
            } else if (BlendSockNameIs(sock, "Emission Strength")) {
                if (BlendReadSocketFloat(sock, flat.emissionStrength)) {
                    flat.hasEmissionStrength = true;
                }
            } else if (BlendSockNameIs(sock, "Normal")) {
                const bNodeLink *l = g.FindLinkToSocket(sock);
                if (l) {
                    const bNode *src = BlendCastNode(l->fromnode.get());
                    const bNode *tex = g.FollowToTexImage(src, 0, BlendCastSocket(l->fromsock.get()));
                    if (tex) {
                        BlendFillTexMap(g, tex, flat.mapNormal);
                    }
                }
            }
        });
        return finish();
    }

    if (BlendNodeIsGlass(node)) {
        flat.output = "Glass BSDF";
        // Node-type fixed: Glass BSDF is refractive (blender ShaderNodeBsdfGlass).
        flat.transmission = 1.f;
        flat.hasTransmission = true;
        flat.metallic = 0.f;
        flat.hasMetallic = true;
        foreachInput([&](const bNodeSocket &sock) {
            if (BlendSockNameIs(sock, "Color")) {
                if (BlendReadLinkedOrDefaultColor(g, sock, flat.baseColor, &flat.mapBaseColor)) {
                    flat.hasBaseColor = true;
                }
            } else if (BlendSockNameIs(sock, "Roughness")) {
                if (BlendReadLinkedOrDefaultFloat(g, sock, flat.roughness, &flat.mapRoughness)) {
                    flat.hasRoughness = true;
                }
            } else if (BlendSockNameIs(sock, "IOR")) {
                if (BlendReadSocketFloat(sock, flat.ior)) {
                    flat.hasIor = true;
                }
            }
        });
        return finish();
    }

    if (BlendNodeIsGlossy(node)) {
        flat.output = "Glossy BSDF";
        // Node-type fixed: Glossy BSDF is a metallic-like specular lobe.
        flat.metallic = 1.f;
        flat.hasMetallic = true;
        flat.transmission = 0.f;
        flat.hasTransmission = true;
        foreachInput([&](const bNodeSocket &sock) {
            if (BlendSockNameIs(sock, "Color")) {
                if (BlendReadLinkedOrDefaultColor(g, sock, flat.baseColor, &flat.mapBaseColor)) {
                    flat.hasBaseColor = true;
                }
            } else if (BlendSockNameIs(sock, "Roughness")) {
                if (BlendReadLinkedOrDefaultFloat(g, sock, flat.roughness, &flat.mapRoughness)) {
                    flat.hasRoughness = true;
                }
            }
        });
        return finish();
    }

    if (BlendNodeIsDiffuse(node)) {
        flat.output = "Diffuse BSDF";
        flat.metallic = 0.f;
        flat.hasMetallic = true;
        flat.transmission = 0.f;
        flat.hasTransmission = true;
        foreachInput([&](const bNodeSocket &sock) {
            if (BlendSockNameIs(sock, "Color")) {
                if (BlendReadLinkedOrDefaultColor(g, sock, flat.baseColor, &flat.mapBaseColor)) {
                    flat.hasBaseColor = true;
                }
            } else if (BlendSockNameIs(sock, "Roughness")) {
                if (BlendReadLinkedOrDefaultFloat(g, sock, flat.roughness, &flat.mapRoughness)) {
                    flat.hasRoughness = true;
                }
            }
        });
        return finish();
    }

    if (BlendNodeIsEmission(node)) {
        flat.output = "Emission";
        foreachInput([&](const bNodeSocket &sock) {
            if (BlendSockNameIs(sock, "Color")) {
                float tmp[4]{};
                const bNodeLink *l = g.FindLinkToSocket(sock);
                if (l) {
                    // Linked: only Image Tex or constant nodes count (no invent from unread Brick/Ramp).
                    const bNode *src = BlendCastNode(l->fromnode.get());
                    const bNode *tex = g.FollowToTexImage(src, 0, BlendCastSocket(l->fromsock.get()));
                    if (tex) {
                        BlendFillTexMap(g, tex, flat.mapEmission);
                    }
                    if (g.ReadConstantColor(src, tmp)) {
                        flat.baseColor[0] = flat.emissionColor[0] = tmp[0];
                        flat.baseColor[1] = flat.emissionColor[1] = tmp[1];
                        flat.baseColor[2] = flat.emissionColor[2] = tmp[2];
                        flat.baseColor[3] = tmp[3];
                        flat.hasBaseColor = true;
                        flat.hasEmissionColor = true;
                    } else if (flat.mapEmission) {
                        // Image-backed emission: no constant color key.
                        flat.mapBaseColor = flat.mapEmission;
                    }
                } else if (BlendReadSocketColor(sock, tmp)) {
                    flat.baseColor[0] = flat.emissionColor[0] = tmp[0];
                    flat.baseColor[1] = flat.emissionColor[1] = tmp[1];
                    flat.baseColor[2] = flat.emissionColor[2] = tmp[2];
                    flat.baseColor[3] = tmp[3];
                    flat.hasBaseColor = true;
                    flat.hasEmissionColor = true;
                }
                if (flat.mapEmission && !flat.hasBaseColor) {
                    // Splash / toon Emission surfaces: same Image is the visible albedo.
                    flat.mapBaseColor = flat.mapEmission;
                }
            } else if (BlendSockNameIs(sock, "Strength")) {
                // Strength default is authored when unlinked; linked unread procedural => omit.
                if (!g.FindLinkToSocket(sock)) {
                    if (BlendReadSocketFloat(sock, flat.emissionStrength)) {
                        flat.hasEmissionStrength = true;
                    }
                } else {
                    float linked = 0.f;
                    const bNodeLink *l = g.FindLinkToSocket(sock);
                    const bNode *src = l ? BlendCastNode(l->fromnode.get()) : nullptr;
                    if (src && g.ReadConstantFloat(src, linked)) {
                        flat.emissionStrength = linked;
                        flat.hasEmissionStrength = true;
                    }
                }
            }
        });
        return finish();
    }

    return flat;
}

BlendShaderFlat BlendFlattenMaterialNodes(const Assimp::Blender::Material &mat, const BlendNodeGraph &g) {
    using namespace Assimp::Blender;
    BlendShaderFlat flat;
    if (!mat.nodetree) {
        return flat;
    }

    const bNode *outputNode = nullptr;
    for (const bNode *n : g.nodes) {
        if (BlendNodeIsOutput(*n)) {
            outputNode = n;
            break;
        }
    }

    bool sawMix = false;
    bool sawAdd = false;
    const bNode *surfaceRoot = nullptr;
    const bNode *chosen = nullptr;
    if (outputNode != nullptr) {
        for (std::shared_ptr<ElemBase> cur = outputNode->inputs.first; cur;) {
            const bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            if (BlendSockNameIsAny(*sock, { "Surface", "Displacement", "Volume" })) {
                if (BlendSockNameIs(*sock, "Surface") || BlendSockNameIs(*sock, "surface")) {
                    const bNodeLink *l = g.FindLinkToSocket(*sock);
                    if (l) {
                        surfaceRoot = BlendCastNode(l->fromnode.get());
                        chosen = g.ResolveSurface(surfaceRoot, sawMix, &sawAdd);
                    }
                }
            }
            cur = sock->next;
        }
        // Fallback: any link into the output node.
        if (chosen == nullptr) {
            for (const bNodeLink *l : g.links) {
                if (BlendCastNode(l->tonode.get()) == outputNode) {
                    surfaceRoot = BlendCastNode(l->fromnode.get());
                    chosen = g.ResolveSurface(surfaceRoot, sawMix, &sawAdd);
                    if (chosen) {
                        break;
                    }
                }
            }
        }
    }

    if (chosen == nullptr) {
        for (const bNode *n : g.nodes) {
            chosen = g.PickBest(chosen, BlendNodeIsFlattenable(*n) ? n : nullptr);
        }
    }

    if (chosen != nullptr) {
        flat = BlendFlattenFromNode(g, *chosen);
        flat.mixedGraph = sawMix;
        flat.addShader = sawAdd;
    }

    // C7 Phase 1: Presence-merge Emission siblings under Add/Mix (constants / Image only).
    // Emission leaf wins over Principled socket defaults (do not keep invent-black Emission Color).
    if (surfaceRoot != nullptr) {
        std::vector<const bNode *> emitLeaves;
        g.CollectEmissionLeaves(surfaceRoot, emitLeaves);
        for (const bNode *en : emitLeaves) {
            if (en == nullptr || en == chosen) {
                continue;
            }
            const BlendShaderFlat emitFlat = BlendFlattenFromNode(g, *en);
            bool merged = false;
            if (emitFlat.hasEmissionColor) {
                flat.emissionColor[0] = emitFlat.emissionColor[0];
                flat.emissionColor[1] = emitFlat.emissionColor[1];
                flat.emissionColor[2] = emitFlat.emissionColor[2];
                flat.hasEmissionColor = true;
                merged = true;
            } else if (emitFlat.mapEmission || emitFlat.hasEmissionStrength) {
                // Procedural Color link unread: drop Principled default emissionColor key.
                flat.hasEmissionColor = false;
                flat.emissionColor[0] = flat.emissionColor[1] = flat.emissionColor[2] = 0.f;
            }
            if (emitFlat.hasEmissionStrength) {
                flat.emissionStrength = emitFlat.emissionStrength;
                flat.hasEmissionStrength = true;
                merged = true;
            }
            if (emitFlat.mapEmission) {
                flat.mapEmission = emitFlat.mapEmission;
                merged = true;
            }
            if (merged || emitFlat.hasEmissionStrength || emitFlat.mapEmission) {
                flat.ok = true;
                flat.addShader = true;
            }
        }
    }

    // Principled Volume on Material Output Volume socket (Water Caustics Material.002).
    if (outputNode != nullptr) {
        for (std::shared_ptr<ElemBase> cur = outputNode->inputs.first; cur;) {
            const bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            if (BlendSockNameIs(*sock, "Volume") || BlendSockNameIs(*sock, "volume")) {
                const bNodeLink *l = g.FindLinkToSocket(*sock);
                const bNode *vn = l ? BlendCastNode(l->fromnode.get()) : nullptr;
                if (vn != nullptr &&
                        ((vn->idname[0] != '\0' && std::strcmp(vn->idname, "NodeReroute") == 0) ||
                                vn->type == kSH_NODE_REROUTE)) {
                    for (const bNodeLink *rl : g.links) {
                        if (BlendCastNode(rl->tonode.get()) == vn) {
                            vn = BlendCastNode(rl->fromnode.get());
                            break;
                        }
                    }
                }
                if (vn != nullptr && BlendNodeIsVolumePrincipled(*vn)) {
                    flat.ok = true;
                    if (flat.output == nullptr) {
                        flat.output = "Principled Volume";
                    }
                    for (std::shared_ptr<ElemBase> sc = vn->inputs.first; sc;) {
                        const bNodeSocket *vs = BlendCastSocket(sc.get());
                        if (vs == nullptr) {
                            break;
                        }
                        const bool linked = (g.FindLinkToSocket(*vs) != nullptr);
                        if (BlendSockNameIs(*vs, "Color") || BlendSockNameIs(*vs, "color")) {
                            if (!linked) {
                                float rgba[4] = { 0, 0, 0, 0 };
                                if (BlendReadSocketColor(*vs, rgba)) {
                                    flat.hasVolumeColor = true;
                                    flat.volumeColor[0] = rgba[0];
                                    flat.volumeColor[1] = rgba[1];
                                    flat.volumeColor[2] = rgba[2];
                                    flat.volumeColor[3] = rgba[3];
                                }
                            }
                        } else if (BlendSockNameIs(*vs, "Density") || BlendSockNameIs(*vs, "density")) {
                            // Presence C12: unlinked -> density; linked -> densityLinked (+
                            // densityDefault only when socket default_value is readable).
                            float dens = 0.f;
                            const bool gotDefault = BlendReadSocketFloat(*vs, dens);
                            if (linked) {
                                flat.hasVolumeDensityLinked = true;
                                if (gotDefault) {
                                    flat.hasVolumeDensityDefault = true;
                                    flat.volumeDensityDefault = dens;
                                }
                            } else if (gotDefault) {
                                flat.hasVolumeDensity = true;
                                flat.volumeDensity = dens;
                            }
                        } else if (BlendSockNameIs(*vs, "Density Attribute") ||
                                   BlendSockNameIs(*vs, "density_attribute")) {
                            if (vs->default_value && vs->default_value->dna_type &&
                                    BlendElemConvertedHullIs(*(vs->default_value), "bNodeSocketValueString")) {
                                const auto *sv = static_cast<const bNodeSocketValueString *>(
                                        vs->default_value.get());
                                if (sv->value[0] != '\0') {
                                    flat.hasVolumeDensityAttribute = true;
                                    flat.volumeDensityAttribute = sv->value;
                                }
                            }
                        } else if ((BlendSockNameIs(*vs, "Anisotropy") ||
                                           BlendSockNameIs(*vs, "anisotropy")) &&
                                   !linked) {
                            float a = 0.f;
                            if (BlendReadSocketFloat(*vs, a)) {
                                flat.hasVolumeAnisotropy = true;
                                flat.volumeAnisotropy = a;
                            }
                        } else if ((BlendSockNameIs(*vs, "Absorption Color") ||
                                           BlendSockNameIs(*vs, "absorption_color")) &&
                                   !linked) {
                            float rgba[4] = { 0, 0, 0, 0 };
                            if (BlendReadSocketColor(*vs, rgba)) {
                                flat.hasVolumeAbsorptionColor = true;
                                flat.volumeAbsorptionColor[0] = rgba[0];
                                flat.volumeAbsorptionColor[1] = rgba[1];
                                flat.volumeAbsorptionColor[2] = rgba[2];
                                flat.volumeAbsorptionColor[3] = rgba[3];
                            }
                        } else if ((BlendSockNameIs(*vs, "Emission Strength") ||
                                           BlendSockNameIs(*vs, "emission_strength")) &&
                                   !linked) {
                            float s = 0.f;
                            if (BlendReadSocketFloat(*vs, s)) {
                                flat.hasVolumeEmissionStrength = true;
                                flat.volumeEmissionStrength = s;
                            }
                        } else if ((BlendSockNameIs(*vs, "Emission Color") ||
                                           BlendSockNameIs(*vs, "emission_color")) &&
                                   !linked) {
                            float rgba[4] = { 0, 0, 0, 0 };
                            if (BlendReadSocketColor(*vs, rgba)) {
                                flat.hasVolumeEmissionColor = true;
                                flat.volumeEmissionColor[0] = rgba[0];
                                flat.volumeEmissionColor[1] = rgba[1];
                                flat.volumeEmissionColor[2] = rgba[2];
                                flat.volumeEmissionColor[3] = rgba[3];
                            }
                        }
                        sc = vs->next;
                    }
                }
            }
            cur = sock->next;
        }
    }

    // Splash / toon setups: Principled often inside ShaderNodeGroup; flatten there with that graph.
    if (!flat.mapBaseColor) {
        for (const bNode *n : g.nodes) {
            const bNodeTree *gt = BlendNodeGroupTree(*n);
            if (gt == nullptr) {
                continue;
            }
            const BlendNodeGraph gg = BlendBuildGraph(*gt);
            const bNode *inner = nullptr;
            for (const bNode *gn : gg.nodes) {
                inner = gg.PickBest(inner, BlendNodeIsFlattenable(*gn) ? gn : nullptr);
            }
            if (inner == nullptr) {
                continue;
            }
            BlendShaderFlat innerFlat = BlendFlattenFromNode(gg, *inner);
            if (!flat.ok || (innerFlat.mapBaseColor && !flat.mapBaseColor) ||
                    (innerFlat.HasAnyFactor() && !flat.HasAnyFactor())) {
                flat = innerFlat;
                flat.mixedGraph = true;
            }
            if (flat.mapBaseColor) {
                break;
            }
        }
    }

    // Presence: only Image Tex maps reached via bNodeLink (same as Blender). No name-guess.
    return flat;
}

BlendShaderFlat BlendFlattenMaterialNodes(const Assimp::Blender::Material &mat) {
    using namespace Assimp::Blender;
    if (!mat.nodetree) {
        return BlendShaderFlat();
    }
    return BlendFlattenMaterialNodes(mat, BlendBuildGraph(*mat.nodetree));
}

/** Embed packed Image into conv.textures (dedupe by Image*). Returns true when *N or filepath usable. */
bool BlendEnsureEmbeddedImage(const Assimp::Blender::Image *img, Assimp::Blender::ConversionData &conv_data,
        aiString &outName, int *outEmbedIndex = nullptr) {
    using namespace Assimp::Blender;
    if (img == nullptr) {
        return false;
    }
    unsigned int texIndex = 0;
    if (BlendEnsureEmbeddedPackedTexture(img, conv_data, &texIndex)) {
        outName.data[0] = '*';
        outName.length = 1 + Assimp::ASSIMP_itoa10(outName.data + 1, static_cast<unsigned int>(AI_MAXLEN - 1),
                                     static_cast<int32_t>(texIndex));
        if (outEmbedIndex != nullptr) {
            *outEmbedIndex = static_cast<int>(texIndex);
        }
        return true;
    }
    const char *dispPath = BlendImageDisplayPath(img);
    if (dispPath == nullptr || dispPath[0] == '\0') {
        return false;
    }
    outName = aiString(BlendNormalizeRelativePath(dispPath));
    if (outEmbedIndex != nullptr) {
        *outEmbedIndex = -1;
    }
    return true;
}

void BlendAttachNodeImage(aiMaterial *mout, const BlendTexMap &map, aiTextureType type,
        Assimp::Blender::ConversionData &conv_data, unsigned matIndex, const char *usage = nullptr) {
    using namespace Assimp::Blender;
    if (mout == nullptr || map.img == nullptr) {
        return;
    }
    aiString name;
    if (!BlendEnsureEmbeddedImage(map.img, conv_data, name)) {
        return;
    }
    const unsigned slot = conv_data.next_texture[type]++;
    mout->AddProperty(&name, AI_MATKEY_TEXTURE(type, slot));
    const int uvIndex = BlendFindUvIndex(conv_data, matIndex, map.uvname);
    BlendEmitUvWsrc(mout, type, slot, uvIndex);
    BlendEmitUvTransformFromMap(mout, type, slot, map);
    // Fully typed = $tex.file + usage meta (inventory decommission only then).
    // UNKNOWN slots still get usage/uvName so Host can parse later.
    const char *u = (usage != nullptr && usage[0] != '\0') ? usage : (type == aiTextureType_UNKNOWN ? "unknown" : nullptr);
    BlendEmitTexUsageMeta(mout, type, slot, u, 0, false, map.uvname);
}

/** Inventory: every ShaderNodeTexImage (incl. nested groups), linked or not. */
struct BlendMapInventoryEntry {
    const Assimp::Blender::Image *img{ nullptr };
    const Assimp::Blender::bNode *node{ nullptr };
    bool linked{ false };
    /** Set when emitted as typed $tex.* (+ usage meta) - drop from Sidecar inventory. */
    bool typed{ false };
};

struct BlendTexUsageHit {
    aiTextureType type{ aiTextureType_UNKNOWN };
    const char *usage{ "unknown" }; // key=value meta ($tex.blend.usage)
    /** Destination socket on the consuming node (e.g. Mix Fac). */
    char socket[64]{};
    /**
     * Source socket on the TexImage (or walk start) that feeds this hit
     * (e.g. Color / Alpha into Mix Fac). Presence only - omit when unread.
     */
    char fromSocket[64]{};
};

/** Map Principled/BSDF/Bump/Displacement/Mix socket name -> typed type + usage label. */
bool BlendClassifySocketUsage(const char *sockName, const Assimp::Blender::bNode &toNode,
        BlendTexUsageHit &out) {
    using namespace Assimp::Blender;
    if (sockName == nullptr || sockName[0] == '\0') {
        return false;
    }
    out.socket[0] = '\0';
    std::strncpy(out.socket, sockName, sizeof(out.socket) - 1);
    out.socket[sizeof(out.socket) - 1] = '\0';

    // Fac into ColorRamp selects the ramp - not a terminal usage (keep walking).
    // Mix Shader Fac ≠ Mix RGB Fac (Enterprise Decals / Aztec_Base_Material).
    if (std::strcmp(sockName, "Fac") == 0 || std::strcmp(sockName, "Factor") == 0) {
        if (BlendNodeIsColorRamp(toNode)) {
            return false;
        }
        if (BlendNodeIsMixShader(toNode)) {
            out.type = aiTextureType_UNKNOWN;
            out.usage = "shaderMixFac";
            return true;
        }
        if (BlendNodeIsMixColor(toNode)) {
            out.type = aiTextureType_UNKNOWN;
            out.usage = "mixFac";
            return true;
        }
        // Fac into other nodes: not a typed albedo mix mask - leave unresolved.
        return false;
    }
    // Mix Color1/Color2/A/B are pass-through operands - not a terminal usage.
    if (BlendNodeIsMixColor(toNode) &&
            (std::strcmp(sockName, "Color1") == 0 || std::strcmp(sockName, "Color2") == 0 ||
                    std::strcmp(sockName, "Color") == 0 || std::strcmp(sockName, "A") == 0 ||
                    std::strcmp(sockName, "B") == 0 || std::strcmp(sockName, "Result") == 0)) {
        return false;
    }
    // Glossy BSDF Color = specular/gloss lobe (not albedo baseColor / mixColor).
    if (BlendNodeIsGlossy(toNode) &&
            (std::strcmp(sockName, "Color") == 0 || std::strcmp(sockName, "color") == 0)) {
        out.type = aiTextureType_SPECULAR;
        out.usage = "glossyColor";
        return true;
    }
    if (BlendNodeIsBump(toNode) || BlendNodeIsDisplacement(toNode)) {
        if (std::strcmp(sockName, "Height") == 0 || std::strcmp(sockName, "height") == 0) {
            out.type = aiTextureType_HEIGHT;
            out.usage = "height";
            return true;
        }
        if (std::strcmp(sockName, "Displacement") == 0) {
            out.type = aiTextureType_DISPLACEMENT;
            out.usage = "displacement";
            return true;
        }
    }
    if (BlendNodeIsNormalMap(toNode)) {
        if (std::strcmp(sockName, "Color") == 0 || std::strcmp(sockName, "color") == 0) {
            out.type = aiTextureType_NORMALS;
            out.usage = "normal";
            return true;
        }
    }
    if (std::strcmp(sockName, "Base Color") == 0 || std::strcmp(sockName, "BaseColor") == 0) {
        out.type = aiTextureType_BASE_COLOR;
        out.usage = "baseColor";
        return true;
    }
    // Color on Principled/Diffuse/Emission/Glass = base (not Mix - handled above).
    if (std::strcmp(sockName, "Color") == 0 && BlendNodeIsFlattenable(toNode)) {
        out.type = aiTextureType_BASE_COLOR;
        out.usage = "baseColor";
        return true;
    }
    if (std::strcmp(sockName, "Normal") == 0) {
        out.type = aiTextureType_NORMALS;
        out.usage = "normal";
        return true;
    }
    if (std::strcmp(sockName, "Roughness") == 0) {
        out.type = aiTextureType_DIFFUSE_ROUGHNESS;
        out.usage = "roughness";
        return true;
    }
    if (std::strcmp(sockName, "Metallic") == 0 || std::strcmp(sockName, "Metalness") == 0) {
        out.type = aiTextureType_METALNESS;
        out.usage = "metallic";
        return true;
    }
    if (std::strcmp(sockName, "Emission Color") == 0 || std::strcmp(sockName, "Emission") == 0) {
        out.type = aiTextureType_EMISSIVE;
        out.usage = "emission";
        return true;
    }
    if (std::strcmp(sockName, "Alpha") == 0) {
        out.type = aiTextureType_OPACITY;
        out.usage = "opacity";
        return true;
    }
    if (std::strcmp(sockName, "Specular") == 0 || std::strcmp(sockName, "Specular IOR Level") == 0 ||
            std::strcmp(sockName, "Specular Tint") == 0) {
        out.type = aiTextureType_SPECULAR;
        out.usage = "specular";
        return true;
    }
    if (std::strcmp(sockName, "Sheen") == 0 || std::strcmp(sockName, "Sheen Weight") == 0 ||
            std::strcmp(sockName, "Sheen Color") == 0 || std::strcmp(sockName, "Sheen Roughness") == 0) {
        out.type = aiTextureType_SHEEN;
        out.usage = "sheen";
        return true;
    }
    if (std::strcmp(sockName, "Coat Weight") == 0 || std::strcmp(sockName, "Clearcoat") == 0 ||
            std::strcmp(sockName, "Coat Roughness") == 0 || std::strcmp(sockName, "Clearcoat Roughness") == 0 ||
            std::strcmp(sockName, "Coat Normal") == 0 || std::strcmp(sockName, "Clearcoat Normal") == 0) {
        out.type = aiTextureType_CLEARCOAT;
        out.usage = "clearcoat";
        return true;
    }
    if (std::strcmp(sockName, "Transmission") == 0 || std::strcmp(sockName, "Transmission Weight") == 0) {
        out.type = aiTextureType_TRANSMISSION;
        out.usage = "transmission";
        return true;
    }
    if (std::strcmp(sockName, "Anisotropic") == 0 || std::strcmp(sockName, "Anisotropy") == 0) {
        out.type = aiTextureType_ANISOTROPY;
        out.usage = "anisotropy";
        return true;
    }
    if (std::strcmp(sockName, "Ambient Occlusion") == 0 || std::strcmp(sockName, "AO") == 0) {
        out.type = aiTextureType_AMBIENT_OCCLUSION;
        out.usage = "ambientOcclusion";
        return true;
    }
    // Volume / principled density maps - no dedicated Assimp type; UNKNOWN + usage for Host.
    if (std::strcmp(sockName, "Density") == 0 || std::strcmp(sockName, "density") == 0) {
        out.type = aiTextureType_UNKNOWN;
        out.usage = "density";
        return true;
    }
    if (std::strcmp(sockName, "Height") == 0 || std::strcmp(sockName, "height") == 0) {
        out.type = aiTextureType_HEIGHT;
        out.usage = "height";
        return true;
    }
    if (BlendNodeIsOutput(toNode) &&
            (std::strcmp(sockName, "Displacement") == 0 || std::strcmp(sockName, "displacement") == 0)) {
        out.type = aiTextureType_DISPLACEMENT;
        out.usage = "displacement";
        return true;
    }
    return false;
}

void BlendCollectTexImageInventory(const BlendNodeGraph &g, const BlendNodeGraph &rootLinks,
        std::vector<BlendMapInventoryEntry> &out, int depth = 0) {
    using namespace Assimp::Blender;
    if (depth > 8) {
        return;
    }
    auto nodeHasOutboundLink = [&](const bNode *n) -> bool {
        for (const bNodeLink *l : rootLinks.links) {
            if (BlendCastNode(l->fromnode.get()) == n) {
                return true;
            }
        }
        for (const bNodeLink *l : g.links) {
            if (BlendCastNode(l->fromnode.get()) == n) {
                return true;
            }
        }
        return false;
    };
    for (const bNode *n : g.nodes) {
        if (n == nullptr) {
            continue;
        }
        if (BlendNodeIsTexImage(*n)) {
            const Image *img = BlendNodeImage(*n);
            if (img != nullptr) {
                BlendMapInventoryEntry e;
                e.img = img;
                e.node = n;
                e.linked = nodeHasOutboundLink(n);
                out.push_back(e);
            }
        }
        if (BlendNodeIsGroup(*n)) {
            const bNodeTree *gt = BlendNodeGroupTree(*n);
            if (gt != nullptr) {
                const BlendNodeGraph sub = BlendBuildGraph(*gt);
                BlendCollectTexImageInventory(sub, rootLinks, out, depth + 1);
            }
        }
    }
}

/**
 * Walk outbound DNA links from a TexImage (through Mix/Reroute/NormalMap/Bump/
 * Group Output) and collect usage hits. Presence only - no filename invent.
 * ColorRamp is procedural HostEval: do not attribute ramp-output sockets to the
 * source Image (keeps mixFac on MixRGB Fac; shaderMixFac on Mix Shader Fac).
 */
void BlendCollectTexImageUsages(const BlendNodeGraph &g, const Assimp::Blender::bNode *texNode,
        std::vector<BlendTexUsageHit> &out, int depth = 0,
        const BlendNodeGraph *hostGraph = nullptr, const Assimp::Blender::bNode *hostGroup = nullptr,
        bool throughMixColor = false) {
    using namespace Assimp::Blender;
    if (texNode == nullptr || depth > 20) {
        return;
    }
    auto sockLabel = [](const bNodeSocket *s) -> const char * {
        if (s == nullptr) {
            return "";
        }
        return s->identifier[0] != '\0' ? s->identifier : s->name;
    };
    auto already = [&](const BlendTexUsageHit &h) -> bool {
        for (const BlendTexUsageHit &e : out) {
            if (e.type == h.type && std::strcmp(e.usage, h.usage) == 0 &&
                    std::strcmp(e.socket, h.socket) == 0 &&
                    std::strcmp(e.fromSocket, h.fromSocket) == 0) {
                return true;
            }
        }
        return false;
    };
    auto markMixLayer = [&](BlendTexUsageHit &h) {
        // Mix Color1/Color2 layers that reach Principled Base Color are not the folded primary.
        if (throughMixColor && std::strcmp(h.usage, "baseColor") == 0) {
            h.usage = "mixColor";
        }
    };
    auto isMixColorOperand = [](const char *sockName) -> bool {
        return sockName != nullptr &&
               (std::strcmp(sockName, "Color1") == 0 || std::strcmp(sockName, "Color2") == 0 ||
                       std::strcmp(sockName, "Color") == 0 || std::strcmp(sockName, "A") == 0 ||
                       std::strcmp(sockName, "B") == 0 || std::strcmp(sockName, "Result") == 0);
    };
    auto fillFromSock = [&](BlendTexUsageHit &hit, const bNodeLink *link) {
        if (link == nullptr) {
            return;
        }
        const char *fs = sockLabel(BlendCastSocket(link->fromsock.get()));
        if (fs == nullptr || fs[0] == '\0') {
            return;
        }
        std::strncpy(hit.fromSocket, fs, sizeof(hit.fromSocket) - 1);
        hit.fromSocket[sizeof(hit.fromSocket) - 1] = '\0';
    };
    for (const bNodeLink *l : g.links) {
        if (l == nullptr || BlendCastNode(l->fromnode.get()) != texNode) {
            continue;
        }
        const bNode *to = BlendCastNode(l->tonode.get());
        const bNodeSocket *ts = BlendCastSocket(l->tosock.get());
        if (to == nullptr) {
            continue;
        }
        const char *toSock = sockLabel(ts);
        BlendTexUsageHit hit;
        const bool classified = BlendClassifySocketUsage(toSock, *to, hit);
        if (classified) {
            markMixLayer(hit);
            fillFromSock(hit, l);
            if (!already(hit)) {
                out.push_back(hit);
            }
            // MixRGB.Fac / MixShader.Fac: do not attribute Mix Color/Shader operands to this map.
            if (std::strcmp(hit.usage, "mixFac") == 0 || std::strcmp(hit.usage, "shaderMixFac") == 0) {
                continue;
            }
        }
        // ColorRamp: Fac/Color select ramp output - HostEval via BLEND_shader_network.
        if (BlendNodeIsColorRamp(*to)) {
            continue;
        }
        const bool nextThroughMix =
                throughMixColor || (BlendNodeIsMixColor(*to) && isMixColorOperand(toSock));
        if (BlendNodeIsGroupOutput(*to)) {
            // Cross into host ShaderNodeGroup outputs.
            if (hostGraph != nullptr && hostGroup != nullptr) {
                for (const bNodeLink *hl : hostGraph->links) {
                    if (hl == nullptr || BlendCastNode(hl->fromnode.get()) != hostGroup) {
                        continue;
                    }
                    const bNodeSocket *hos = BlendCastSocket(hl->fromsock.get());
                    if (hos == nullptr || ts == nullptr) {
                        continue;
                    }
                    const bool nameHit =
                            (hos->identifier[0] && ts->identifier[0] &&
                                    std::strcmp(hos->identifier, ts->identifier) == 0) ||
                            BlendSockNameIs(*hos, ts->name) || BlendSockNameIs(*ts, hos->name);
                    if (!nameHit) {
                        continue;
                    }
                    const bNode *hnt = BlendCastNode(hl->tonode.get());
                    if (hnt == nullptr) {
                        continue;
                    }
                    const bNodeSocket *hts = BlendCastSocket(hl->tosock.get());
                    BlendTexUsageHit hh;
                    if (BlendClassifySocketUsage(sockLabel(hts), *hnt, hh)) {
                        markMixLayer(hh);
                        fillFromSock(hh, hl);
                        if (!already(hh)) {
                            out.push_back(hh);
                        }
                    }
                    if (!BlendNodeIsTexWalkBarrier(*hnt) || BlendNodeIsNormalMap(*hnt) ||
                            BlendNodeIsBump(*hnt) || BlendNodeIsDisplacement(*hnt)) {
                        BlendCollectTexImageUsages(*hostGraph, hnt, out, depth + 1, nullptr, nullptr,
                                nextThroughMix);
                    }
                }
            }
            continue;
        }
        if (BlendNodeIsGroup(*to)) {
            const bNodeTree *gt = BlendNodeGroupTree(*to);
            if (gt == nullptr || ts == nullptr) {
                continue;
            }
            const BlendNodeGraph sub = BlendBuildGraph(*gt);
            // Enter only via Group Input outputs that match the fed group input socket.
            for (const bNode *gin : sub.nodes) {
                if (gin == nullptr || !BlendNodeIsGroupInput(*gin)) {
                    continue;
                }
                for (const bNodeLink *il : sub.links) {
                    if (il == nullptr || BlendCastNode(il->fromnode.get()) != gin) {
                        continue;
                    }
                    const bNodeSocket *fos = BlendCastSocket(il->fromsock.get());
                    if (fos == nullptr) {
                        continue;
                    }
                    const bool nameHit =
                            (fos->identifier[0] && ts->identifier[0] &&
                                    std::strcmp(fos->identifier, ts->identifier) == 0) ||
                            BlendSockNameIs(*fos, ts->name) || BlendSockNameIs(*ts, fos->name) ||
                            (ts->identifier[0] && BlendSockNameIs(*fos, ts->identifier));
                    if (!nameHit) {
                        continue;
                    }
                    const bNode *inner = BlendCastNode(il->tonode.get());
                    if (inner == nullptr) {
                        continue;
                    }
                    BlendTexUsageHit ih;
                    const bNodeSocket *its = BlendCastSocket(il->tosock.get());
                    if (BlendClassifySocketUsage(sockLabel(its), *inner, ih)) {
                        markMixLayer(ih);
                        fillFromSock(ih, il);
                        if (!already(ih)) {
                            out.push_back(ih);
                        }
                        if (std::strcmp(ih.usage, "mixFac") == 0 ||
                                std::strcmp(ih.usage, "shaderMixFac") == 0) {
                            continue;
                        }
                    }
                    if (BlendNodeIsColorRamp(*inner)) {
                        continue;
                    }
                    if (!BlendNodeIsTexWalkBarrier(*inner) || BlendNodeIsNormalMap(*inner) ||
                            BlendNodeIsBump(*inner) || BlendNodeIsDisplacement(*inner)) {
                        BlendCollectTexImageUsages(sub, inner, out, depth + 1, &g, to, nextThroughMix);
                    }
                }
            }
            continue;
        }
        if (BlendNodeIsTexWalkBarrier(*to)) {
            continue;
        }
        // Continue through filters / mix / reroute / normal / bump (not ColorRamp).
        BlendCollectTexImageUsages(g, to, out, depth + 1, hostGraph, hostGroup, nextThroughMix);
    }
}

bool BlendMatHasTextureFile(const aiMaterial *mout, aiTextureType type, const aiString &file) {
    if (mout == nullptr || file.length == 0) {
        return false;
    }
    const unsigned n = mout->GetTextureCount(type);
    for (unsigned i = 0; i < n; ++i) {
        aiString path;
        if (mout->Get(AI_MATKEY_TEXTURE(type, i), path) == AI_SUCCESS &&
                std::strcmp(path.C_Str(), file.C_Str()) == 0) {
            return true;
        }
    }
    return false;
}

/** Fully typed = same $tex.file slot also has $tex.blend.usage (no invent). */
bool BlendMatHasFullyTypedFile(const aiMaterial *mout, const aiString &file) {
    if (mout == nullptr || file.length == 0) {
        return false;
    }
    for (int ti = 1; ti <= static_cast<int>(AI_TEXTURE_TYPE_MAX); ++ti) {
        const auto type = static_cast<aiTextureType>(ti);
        const unsigned n = mout->GetTextureCount(type);
        for (unsigned i = 0; i < n; ++i) {
            aiString path;
            if (mout->Get(AI_MATKEY_TEXTURE(type, i), path) != AI_SUCCESS ||
                    std::strcmp(path.C_Str(), file.C_Str()) != 0) {
                continue;
            }
            aiString usage;
            if (mout->Get("$tex.blend.usage", type, i, usage) == AI_SUCCESS && usage.length > 0) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Linked maps.inventory TexImages -> typed $tex.file + usage meta.
 * typed=true only when $tex.file AND $tex.blend.usage Present (fully typed).
 * Unlinked / usage-unresolved stay in inventory (honest leftover).
 */
void BlendEmitInventoryTypedMaps(aiMaterial *mout, const BlendShaderFlat &flat,
        std::vector<BlendMapInventoryEntry> &inventory, Assimp::Blender::ConversionData &conv_data,
        unsigned matIndex, const BlendNodeGraph &rootG) {
    using namespace Assimp::Blender;
    if (mout == nullptr) {
        return;
    }
    (void)flat;

    for (BlendMapInventoryEntry &e : inventory) {
        // Unlinked Previz stay inventory-only (no usage invent).
        if (e.img == nullptr || e.node == nullptr || !e.linked) {
            continue;
        }
        // Resolve UV once for this TexImage node.
        BlendTexMap map;
        BlendFillTexMap(rootG, e.node, map);
        if (!map.img) {
            map.img = e.img;
        }
        // Usages: search from node in root; if node lives in a nested group, walk each group.
        std::vector<BlendTexUsageHit> hits;
        BlendCollectTexImageUsages(rootG, e.node, hits);
        if (hits.empty()) {
            for (const bNode *gn : rootG.nodes) {
                if (gn == nullptr || !BlendNodeIsGroup(*gn)) {
                    continue;
                }
                const bNodeTree *gt = BlendNodeGroupTree(*gn);
                if (gt == nullptr) {
                    continue;
                }
                const BlendNodeGraph sub = BlendBuildGraph(*gt);
                bool inSub = false;
                for (const bNode *sn : sub.nodes) {
                    if (sn == e.node) {
                        inSub = true;
                        break;
                    }
                }
                if (!inSub) {
                    // deeper nesting: one more level
                    for (const bNode *sn : sub.nodes) {
                        if (sn == nullptr || !BlendNodeIsGroup(*sn)) {
                            continue;
                        }
                        const bNodeTree *gt2 = BlendNodeGroupTree(*sn);
                        if (gt2 == nullptr) {
                            continue;
                        }
                        const BlendNodeGraph sub2 = BlendBuildGraph(*gt2);
                        for (const bNode *sn2 : sub2.nodes) {
                            if (sn2 == e.node) {
                                BlendCollectTexImageUsages(sub2, e.node, hits, 0, &sub, sn);
                                break;
                            }
                        }
                    }
                    continue;
                }
                BlendCollectTexImageUsages(sub, e.node, hits, 0, &rootG, gn);
            }
        }
        // UV for nested nodes: BlendFillTexMap on root may miss; try again with empty uv ok.
        if (map.uvname[0] == '\0') {
            BlendFillTexMap(rootG, e.node, map);
            if (!map.img) {
                map.img = e.img;
            }
        }

        if (hits.empty()) {
            // Linked but usage unresolved - keep inventory (teilresolved / honest).
            continue;
        }
        aiString embedName;
        if (!BlendEnsureEmbeddedImage(e.img, conv_data, embedName)) {
            // No file/embed Presence - keep inventory, do not invent typed slot.
            continue;
        }
        for (const BlendTexUsageHit &h : hits) {
            if (h.usage == nullptr || h.usage[0] == '\0') {
                continue;
            }
            // Already fully typed for this file - skip duplicate slot.
            if (BlendMatHasFullyTypedFile(mout, embedName) &&
                    BlendMatHasTextureFile(mout, h.type, embedName)) {
                continue;
            }
            if (BlendMatHasTextureFile(mout, h.type, embedName)) {
                // File present without usage (legacy flatten) - attach usage on existing slot.
                const unsigned n = mout->GetTextureCount(h.type);
                for (unsigned si = 0; si < n; ++si) {
                    aiString path;
                    if (mout->Get(AI_MATKEY_TEXTURE(h.type, si), path) != AI_SUCCESS ||
                            std::strcmp(path.C_Str(), embedName.C_Str()) != 0) {
                        continue;
                    }
                    aiString existing;
                    if (mout->Get("$tex.blend.usage", h.type, si, existing) == AI_SUCCESS &&
                            existing.length > 0) {
                        break;
                    }
                    aiString usage(h.usage);
                    mout->AddProperty(&usage, "$tex.blend.usage", h.type, si);
                    if (h.socket[0] != '\0') {
                        aiString sock(h.socket);
                        mout->AddProperty(&sock, "$tex.blend.socket", h.type, si);
                    }
                    if (h.fromSocket[0] != '\0') {
                        aiString fromSock(h.fromSocket);
                        mout->AddProperty(&fromSock, "$tex.blend.fromSocket", h.type, si);
                    }
                    if (e.node->name[0] != '\0') {
                        aiString nname(e.node->name);
                        mout->AddProperty(&nname, "$tex.blend.node", h.type, si);
                    }
                    break;
                }
                continue;
            }
            const unsigned slot = conv_data.next_texture[h.type]++;
            mout->AddProperty(&embedName, AI_MATKEY_TEXTURE(h.type, slot));
            const int uvIndex = BlendFindUvIndex(conv_data, matIndex, map.uvname);
            BlendEmitUvWsrc(mout, h.type, slot, uvIndex);
            BlendEmitUvTransformFromMap(mout, h.type, slot, map);
            BlendEmitTexUsageMeta(mout, h.type, slot, h.usage, 0, false, map.uvname);
            if (h.socket[0] != '\0') {
                aiString sock(h.socket);
                mout->AddProperty(&sock, "$tex.blend.socket", h.type, slot);
            }
            if (h.fromSocket[0] != '\0') {
                aiString fromSock(h.fromSocket);
                mout->AddProperty(&fromSock, "$tex.blend.fromSocket", h.type, slot);
            }
            if (e.node->name[0] != '\0') {
                aiString nname(e.node->name);
                mout->AddProperty(&nname, "$tex.blend.node", h.type, slot);
            }
            // Mirror BASE onto DIFFUSE (Looks compat) with same usage meta.
            if (h.type == aiTextureType_BASE_COLOR &&
                    !BlendMatHasTextureFile(mout, aiTextureType_DIFFUSE, embedName)) {
                const unsigned dslot = conv_data.next_texture[aiTextureType_DIFFUSE]++;
                mout->AddProperty(&embedName, AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, dslot));
                BlendEmitUvWsrc(mout, aiTextureType_DIFFUSE, dslot, uvIndex);
                BlendEmitUvTransformFromMap(mout, aiTextureType_DIFFUSE, dslot, map);
                BlendEmitTexUsageMeta(mout, aiTextureType_DIFFUSE, dslot, h.usage, 0, false, map.uvname);
            }
        }
    }
    // Reconcile: fully typed only when $tex.file + $tex.blend.usage Present.
    for (BlendMapInventoryEntry &e : inventory) {
        if (e.img == nullptr) {
            e.typed = false;
            continue;
        }
        aiString embedName;
        if (!BlendEnsureEmbeddedImage(e.img, conv_data, embedName)) {
            e.typed = false;
            continue;
        }
        e.typed = BlendMatHasFullyTypedFile(mout, embedName);
    }
}

struct BlendWorldEnvFlat {
    const Assimp::Blender::Image *img{ nullptr };
    bool hasStrength{ false };
    float strength{ 0.f };
};

BlendWorldEnvFlat BlendFlattenWorldNodes(const Assimp::Blender::World &world) {
    using namespace Assimp::Blender;
    BlendWorldEnvFlat out;
    if (!world.nodetree) {
        return out;
    }
    const BlendNodeGraph g = BlendBuildGraph(*world.nodetree);
    const bNode *outputNode = nullptr;
    for (const bNode *n : g.nodes) {
        if (n != nullptr && BlendNodeIsOutputWorld(*n)) {
            outputNode = n;
            break;
        }
    }
    if (outputNode == nullptr) {
        return out;
    }
    const bNode *bg = nullptr;
    for (std::shared_ptr<ElemBase> cur = outputNode->inputs.first; cur;) {
        const bNodeSocket *sock = BlendCastSocket(cur.get());
        if (sock == nullptr) {
            break;
        }
        if (BlendSockNameIs(*sock, "Surface") || BlendSockNameIs(*sock, "surface")) {
            const bNodeLink *l = g.FindLinkToSocket(*sock);
            if (l) {
                bg = BlendCastNode(l->fromnode.get());
            }
        }
        cur = sock->next;
    }
    if (bg == nullptr) {
        return out;
    }
    // Peel single Reroute.
    if ((bg->idname[0] != '\0' && std::strcmp(bg->idname, "NodeReroute") == 0) || bg->type == kSH_NODE_REROUTE) {
        for (const bNodeLink *rl : g.links) {
            if (BlendCastNode(rl->tonode.get()) == bg) {
                bg = BlendCastNode(rl->fromnode.get());
                break;
            }
        }
    }
    if (bg == nullptr || !BlendNodeIsBackground(*bg)) {
        return out;
    }
    for (std::shared_ptr<ElemBase> cur = bg->inputs.first; cur;) {
        const bNodeSocket *sock = BlendCastSocket(cur.get());
        if (sock == nullptr) {
            break;
        }
        if (BlendSockNameIs(*sock, "Color") || BlendSockNameIs(*sock, "color")) {
            const bNodeLink *l = g.FindLinkToSocket(*sock);
            if (l) {
                const bNode *src = BlendCastNode(l->fromnode.get());
                if (src != nullptr && (BlendNodeIsTexEnvironment(*src) || BlendNodeIsTexImage(*src))) {
                    out.img = BlendNodeImage(*src);
                }
            }
        } else if (BlendSockNameIs(*sock, "Strength") || BlendSockNameIs(*sock, "strength")) {
            if (!g.FindLinkToSocket(*sock)) {
                float s = 0.f;
                if (BlendReadSocketFloat(*sock, s)) {
                    out.hasStrength = true;
                    out.strength = s;
                }
            }
        }
        cur = sock->next;
    }
    return out;
}

void BlendEmitWorldEnv(const Assimp::Blender::Scene &in, Assimp::Blender::ConversionData &conv) {
    using namespace Assimp::Blender;
    conv.worldEnvJson.clear();
    if (!in.world) {
        return;
    }
    const BlendWorldEnvFlat flat = BlendFlattenWorldNodes(*in.world);
    if (flat.img == nullptr) {
        return;
    }
    aiString embedName;
    int embedIndex = -1;
    if (!BlendEnsureEmbeddedImage(flat.img, conv, embedName, &embedIndex)) {
        return;
    }
    // Presence: only emit worldEnv when an Environment/Image Texture Image was resolved.
    std::ostringstream wj;
    wj << '{';
    const char *disp = BlendImageDisplayPath(flat.img);
    bool first = true;
    auto sep = [&]() {
        if (!first) {
            wj << ',';
        }
        first = false;
    };
    if (flat.img->name[0] != '\0') {
        sep();
        wj << "\"image\":\"" << BlendJsonEscape(flat.img->name + 2) << '"';
    }
    if (disp != nullptr && disp[0] != '\0') {
        sep();
        wj << "\"path\":\"" << BlendJsonEscape(disp) << '"';
    }
    if (embedIndex >= 0) {
        sep();
        wj << "\"embed\":\"*" << embedIndex << '"';
    }
    sep();
    wj << "\"projection\":\"equirectangular\"";
    if (flat.hasStrength) {
        sep();
        wj << "\"strength\":" << flat.strength;
    }
    wj << '}';
    conv.worldEnvJson = wj.str();
}

/** DNA Presence: HostEval procedural kinds on a nodetree (no invent). */
struct BlendHostProceduralKinds {
    bool brick = false;
    bool ramp = false;
    bool noise = false;
    bool musgrave = false;

    bool any() const {
        return brick || ramp || noise || musgrave;
    }

    void Collect(const BlendNodeGraph &g) {
        using namespace Assimp::Blender;
        for (const bNode *n : g.nodes) {
            if (n == nullptr) {
                continue;
            }
            if (BlendNodeIsTexBrick(*n)) {
                brick = true;
            } else if (BlendNodeIsColorRamp(*n)) {
                ramp = true;
            } else if (BlendNodeIsTexNoise(*n)) {
                noise = true;
            } else if (BlendNodeIsTexMusgrave(*n)) {
                musgrave = true;
            }
        }
    }

    std::string MatKeyCsv() const {
        std::string s;
        auto add = [&](const char *tok) {
            if (!s.empty()) {
                s += ',';
            }
            s += tok;
        };
        if (brick) {
            add("TexBrick");
        }
        if (ramp) {
            add("ColorRamp");
        }
        if (noise) {
            add("TexNoise");
        }
        if (musgrave) {
            add("TexMusgrave");
        }
        return s;
    }

    void AppendJsonArray(std::ostringstream &nj) const {
        if (!any()) {
            return;
        }
        nj << ",\"hostProcedural\":[";
        bool first = true;
        auto add = [&](const char *tok) {
            if (!first) {
                nj << ',';
            }
            first = false;
            nj << '"' << tok << '"';
        };
        if (brick) {
            add("TexBrick");
        }
        if (ramp) {
            add("ColorRamp");
        }
        if (noise) {
            add("TexNoise");
        }
        if (musgrave) {
            add("TexMusgrave");
        }
        nj << ']';
    }
};

/**
 * Presence MATKEYs mirroring bag eval / hostProcedural so Host can skip JSON
 * open when HostEval is not needed. Emit only when network bag is attached.
 */
void BlendEmitShaderNetworkHostEvalMatKeys(aiMaterial *mout, const BlendNodeGraph &g) {
    if (mout == nullptr) {
        return;
    }
    aiString evalOwner("host");
    mout->AddProperty(&evalOwner, "$mat.blend.shaderNetworkEval", 0, 0);
    BlendHostProceduralKinds hp;
    hp.Collect(g);
    if (!hp.any()) {
        return;
    }
    const std::string csv = hp.MatKeyCsv();
    aiString hostProc(csv.c_str());
    mout->AddProperty(&hostProc, "$mat.blend.hostProcedural", 0, 0);
}

std::string BlendBuildShaderNetworkJson(const Assimp::Blender::Material &mat,
        Assimp::Blender::ConversionData &conv, const BlendNodeGraph &g) {
    using namespace Assimp::Blender;
    (void)conv;
    (void)mat;

    // Link DAG structure (Presence): roots = Material Output; parents = upstream
    // (fromnode), children = downstream (tonode). Socket-level refs stay in links[].
    // Typed Principled flatten is unchanged - this bag is HostEval only.
    auto pushUnique = [](std::vector<std::string> &dst, const std::string &name) {
        if (name.empty()) {
            return;
        }
        if (std::find(dst.begin(), dst.end(), name) == dst.end()) {
            dst.push_back(name);
        }
    };
    std::map<std::string, std::vector<std::string>> parentsOf;
    std::map<std::string, std::vector<std::string>> childrenOf;
    std::vector<std::string> roots;
    for (const bNode *n : g.nodes) {
        if (n != nullptr && BlendNodeIsOutput(*n) && n->name[0] != '\0') {
            pushUnique(roots, n->name);
        }
    }
    for (const bNodeLink *l : g.links) {
        if (l == nullptr) {
            continue;
        }
        const bNode *fn = BlendCastNode(l->fromnode.get());
        const bNode *tn = BlendCastNode(l->tonode.get());
        if (fn == nullptr || tn == nullptr || fn->name[0] == '\0' || tn->name[0] == '\0') {
            continue;
        }
        pushUnique(childrenOf[fn->name], tn->name);
        pushUnique(parentsOf[tn->name], fn->name);
    }

    BlendHostProceduralKinds hostProc;
    hostProc.Collect(g);

    std::ostringstream nj;
    // Host-eval policy: emit authored socket defaults + ColorBand + links so the
    // renderer Look can implement Brick/Ramp (no Assimp bake; no invent).
    nj << "{\"schemaVersion\":1,\"eval\":\"host\",\"assimpEval\":false";
    if (!roots.empty()) {
        nj << ",\"roots\":[";
        for (size_t ri = 0; ri < roots.size(); ++ri) {
            if (ri) {
                nj << ',';
            }
            nj << '"' << BlendJsonEscape(roots[ri]) << '"';
        }
        nj << ']';
    }
    nj << ",\"nodes\":[";
    bool nfirst = true;
    for (const bNode *n : g.nodes) {
        if (n == nullptr) {
            continue;
        }
        if (!nfirst) {
            nj << ',';
        }
        nfirst = false;
        nj << '{';
        if (n->idname[0] != '\0') {
            nj << "\"idname\":\"" << BlendJsonEscape(n->idname) << '"';
        } else {
            nj << "\"type\":" << n->type;
        }
        if (n->name[0] != '\0') {
            nj << ",\"name\":\"" << BlendJsonEscape(n->name) << '"';
        }
        if (n->name[0] != '\0') {
            const auto pit = parentsOf.find(n->name);
            if (pit != parentsOf.end() && !pit->second.empty()) {
                nj << ",\"parents\":[";
                for (size_t pi = 0; pi < pit->second.size(); ++pi) {
                    if (pi) {
                        nj << ',';
                    }
                    nj << '"' << BlendJsonEscape(pit->second[pi]) << '"';
                }
                nj << ']';
            }
            const auto cit = childrenOf.find(n->name);
            if (cit != childrenOf.end() && !cit->second.empty()) {
                nj << ",\"children\":[";
                for (size_t ci = 0; ci < cit->second.size(); ++ci) {
                    if (ci) {
                        nj << ',';
                    }
                    nj << '"' << BlendJsonEscape(cit->second[ci]) << '"';
                }
                nj << ']';
            }
        }
        if (BlendNodeIsTexBrick(*n)) {
            nj << ",\"procedural\":\"TexBrick\"";
        } else if (BlendNodeIsColorRamp(*n)) {
            nj << ",\"procedural\":\"ColorRamp\"";
        } else if (BlendNodeIsTexNoise(*n)) {
            nj << ",\"procedural\":\"TexNoise\"";
        } else if (BlendNodeIsTexMusgrave(*n)) {
            nj << ",\"procedural\":\"TexMusgrave\"";
        } else if (BlendNodeIsMapping(*n)) {
            // Host 0aj: Mapping Location/Rotation/Scale defaults for Image + procedural paths.
            nj << ",\"kind\":\"Mapping\"";
        } else if (BlendNodeIsAddShader(*n)) {
            nj << ",\"kind\":\"AddShader\"";
        } else if (BlendNodeIsEmission(*n)) {
            nj << ",\"kind\":\"Emission\"";
        } else if (BlendNodeIsMixColor(*n)) {
            nj << ",\"kind\":\"Mix\"";
        }

        // Unlinked input socket defaults (Presence - omit linked / unread).
        // Vector (SOCK_VECTOR): Mapping Location/Rotation/Scale and Tex Image Vector, etc.
        nj << ",\"inputs\":[";
        bool ifirst = true;
        bool haveLoc = false, haveRot = false, haveScl = false;
        for (std::shared_ptr<ElemBase> cur = n->inputs.first; cur;) {
            const bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            const bool linked = g.FindLinkToSocket(*sock) != nullptr;
            float f = 0.f;
            float rgba[4] = { 0, 0, 0, 0 };
            float vec[3] = { 0, 0, 0 };
            const bool gotF = !linked && BlendReadSocketFloat(*sock, f);
            const bool gotC = !linked && BlendReadSocketColor(*sock, rgba);
            const bool gotV = !linked && BlendReadSocketVector(*sock, vec);
            if (gotF || gotC || gotV || linked) {
                if (!ifirst) {
                    nj << ',';
                }
                ifirst = false;
                const char *sname = sock->identifier[0] != '\0' ? sock->identifier : sock->name;
                nj << "{\"name\":\"" << BlendJsonEscape(sname) << '"';
                if (linked) {
                    nj << ",\"linked\":true";
                } else if (gotC) {
                    nj << ",\"rgba\":[" << rgba[0] << ',' << rgba[1] << ',' << rgba[2] << ',' << rgba[3] << ']';
                } else if (gotV) {
                    nj << ",\"vector\":[" << vec[0] << ',' << vec[1] << ',' << vec[2] << ']';
                    if (BlendSockNameIsAny(*sock, { "Location" })) {
                        haveLoc = true;
                    } else if (BlendSockNameIsAny(*sock, { "Rotation" })) {
                        haveRot = true;
                    } else if (BlendSockNameIsAny(*sock, { "Scale" })) {
                        haveScl = true;
                    }
                } else if (gotF) {
                    nj << ",\"value\":" << f;
                }
                nj << '}';
            }
            cur = sock->next;
        }
        // Pre-2.81 Mapping: Location/Rotation/Scale lived in TexMapping storage, not sockets.
        if (BlendNodeIsMapping(*n) && n->storage && n->storage->dna_type &&
                BlendElemConvertedHullIs(*(n->storage), "TexMapping")) {
            const auto *tm = static_cast<const TexMapping *>(n->storage.get());
            auto emitVec = [&](const char *name, const float v[3], bool &have) {
                if (have) {
                    return;
                }
                if (!ifirst) {
                    nj << ',';
                }
                ifirst = false;
                nj << "{\"name\":\"" << name << "\",\"vector\":[" << v[0] << ',' << v[1] << ',' << v[2]
                   << "],\"source\":\"TexMapping\"}";
                have = true;
            };
            emitVec("Location", tm->loc, haveLoc);
            emitVec("Rotation", tm->rot, haveRot);
            emitVec("Scale", tm->size, haveScl);
        }
        nj << ']';

        // ColorRamp storage (ColorBand stops).
        if (n->storage && n->storage->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(n->storage), "ColorBand")) {
            const auto *band = static_cast<const ColorBand *>(n->storage.get());
            const int tot = std::max(0, std::min(static_cast<int>(band->tot), 32));
            nj << ",\"colorBand\":{\"tot\":" << tot << ",\"ipotype\":" << static_cast<int>(band->ipotype)
               << ",\"elements\":[";
            for (int ei = 0; ei < tot; ++ei) {
                if (ei) {
                    nj << ',';
                }
                const CBData &e = band->data[ei];
                nj << "{\"pos\":" << e.pos << ",\"rgba\":[" << e.r << ',' << e.g << ',' << e.b << ',' << e.a << "]}";
            }
            nj << "]}";
        }
        // Musgrave / Noise type Presence (SHD_MUSGRAVE_* == SHD_NOISE_*; omit if unread).
        if (n->storage && n->storage->dna_type != nullptr) {
            int musgraveType = -1;
            if (BlendElemConvertedHullIs(*(n->storage), "NodeTexNoise")) {
                const auto *noise = static_cast<const NodeTexNoise *>(n->storage.get());
                musgraveType = static_cast<int>(noise->type);
            } else if (BlendElemConvertedHullIs(*(n->storage), "NodeTexMusgrave")) {
                const auto *mus = static_cast<const NodeTexMusgrave *>(n->storage.get());
                musgraveType = mus->musgrave_type;
            }
            if (musgraveType >= 0) {
                nj << ",\"musgraveType\":" << musgraveType;
                if (const char *alias = BlendMusgraveTypeAlias(musgraveType)) {
                    nj << ",\"musgraveTypeName\":\"" << alias << '"';
                }
            }
        }
        nj << '}';
    }
    nj << "],\"links\":[";
    bool lfirst = true;
    for (const bNodeLink *l : g.links) {
        if (l == nullptr) {
            continue;
        }
        const bNode *fn = BlendCastNode(l->fromnode.get());
        const bNode *tn = BlendCastNode(l->tonode.get());
        const bNodeSocket *fs = BlendCastSocket(l->fromsock.get());
        const bNodeSocket *ts = BlendCastSocket(l->tosock.get());
        if (fn == nullptr || tn == nullptr) {
            continue;
        }
        if (!lfirst) {
            nj << ',';
        }
        lfirst = false;
        nj << "{\"from\":\"" << BlendJsonEscape(fn->name) << "\",\"to\":\"" << BlendJsonEscape(tn->name) << '"';
        if (fs != nullptr) {
            const char *sn = fs->identifier[0] != '\0' ? fs->identifier : fs->name;
            nj << ",\"fromSock\":\"" << BlendJsonEscape(sn) << '"';
        }
        if (ts != nullptr) {
            const char *sn = ts->identifier[0] != '\0' ? ts->identifier : ts->name;
            nj << ",\"toSock\":\"" << BlendJsonEscape(sn) << '"';
        }
        nj << '}';
    }
    nj << ']';
    hostProc.AppendJsonArray(nj);
    nj << '}';
    // Honesty: Assimp does not CPU-eval the graph; Host Look may. Do not list
    // TexBrick/ColorRamp under skipped[] when params were emitted for hostEval.
    return nj.str();
}

std::string BlendBuildShaderNetworkJson(const Assimp::Blender::Material &mat,
        Assimp::Blender::ConversionData &conv) {
    using namespace Assimp::Blender;
    if (!mat.nodetree) {
        return {};
    }
    return BlendBuildShaderNetworkJson(mat, conv, BlendBuildGraph(*mat.nodetree));
}

void BlendApplyFlatToMaterial(aiMaterial *mout, const BlendShaderFlat &flat,
        Assimp::Blender::ConversionData &conv_data, unsigned matIndex) {
    if (mout == nullptr || !flat.ok) {
        return;
    }
    if (flat.hasBaseColor) {
        aiColor4D base(flat.baseColor[0], flat.baseColor[1], flat.baseColor[2], flat.baseColor[3]);
        mout->AddProperty(&base, 1, AI_MATKEY_BASE_COLOR);
        aiColor3D diff(flat.baseColor[0], flat.baseColor[1], flat.baseColor[2]);
        mout->AddProperty(&diff, 1, AI_MATKEY_COLOR_DIFFUSE);
    }
    if (flat.hasMetallic) {
        mout->AddProperty(&flat.metallic, 1, AI_MATKEY_METALLIC_FACTOR);
    }
    if (flat.hasRoughness) {
        mout->AddProperty(&flat.roughness, 1, AI_MATKEY_ROUGHNESS_FACTOR);
    }
    if (flat.hasIor) {
        mout->AddProperty(&flat.ior, 1, AI_MATKEY_REFRACTI);
    }
    if (flat.hasTransmission) {
        mout->AddProperty(&flat.transmission, 1, AI_MATKEY_TRANSMISSION_FACTOR);
    }
    if (flat.hasAlpha) {
        mout->AddProperty(&flat.alpha, 1, AI_MATKEY_OPACITY);
    }
    const bool hasEmit = flat.mapEmission ||
                         (flat.hasEmissionColor &&
                                 (flat.emissionColor[0] + flat.emissionColor[1] + flat.emissionColor[2]) > 1e-4f &&
                                 (!flat.hasEmissionStrength || flat.emissionStrength > 0.f));
    if (hasEmit) {
        const float s = (flat.hasEmissionStrength && flat.emissionStrength > 0.f) ? flat.emissionStrength : 1.f;
        aiColor3D emit(flat.emissionColor[0] * s, flat.emissionColor[1] * s, flat.emissionColor[2] * s);
        mout->AddProperty(&emit, 1, AI_MATKEY_COLOR_EMISSIVE);
    }
    BlendAttachNodeImage(mout, flat.mapBaseColor, aiTextureType_BASE_COLOR, conv_data, matIndex, "baseColor");
    if (flat.mapBaseColor) {
        BlendAttachNodeImage(mout, flat.mapBaseColor, aiTextureType_DIFFUSE, conv_data, matIndex, "baseColor");
    }
    BlendAttachNodeImage(mout, flat.mapRoughness, aiTextureType_DIFFUSE_ROUGHNESS, conv_data, matIndex, "roughness");
    BlendAttachNodeImage(mout, flat.mapMetallic, aiTextureType_METALNESS, conv_data, matIndex, "metallic");
    BlendAttachNodeImage(mout, flat.mapNormal, aiTextureType_NORMALS, conv_data, matIndex, "normal");
    BlendAttachNodeImage(mout, flat.mapEmission, aiTextureType_EMISSIVE, conv_data, matIndex, "emission");
}

static std::string BlendMapJson(const BlendTexMap &map) {
    if (!map.img || map.img->name[0] == '\0') {
        return "null";
    }
    // Prefer image datablock name; append uv layer when C6 UV Map resolved it.
    if (map.uvname[0] != '\0') {
        return std::string("{\"image\":\"") + BlendJsonEscape(map.img->name) + "\",\"uv\":\"" +
               BlendJsonEscape(map.uvname) + "\"}";
    }
    return std::string("\"") + BlendJsonEscape(map.img->name) + '"';
}

std::string BlendBuildMaterialSidecarJson(const Assimp::Blender::Material &mat, const BlendShaderFlat &flat,
        Assimp::Blender::ConversionData &conv_data,
        const std::vector<BlendMapInventoryEntry> &inventory,
        const BlendNodeGraph *prebuiltGraph = nullptr) {
    std::ostringstream mjson;
    mjson << "{\"schemaVersion\":1,\"source\":{\"format\":\"blend\"},\"BLEND_shader\":{";
    mjson << "\"name\":\"" << BlendJsonEscape(mat.id.name + 2) << '"';
    if (mat.nodetree) {
        mjson << ",\"use_nodes\":true,\"engine\":\"CYCLES|EEVEE\"";
        if (flat.ok && flat.output != nullptr) {
            mjson << ",\"output\":\"" << BlendJsonEscape(flat.output) << '"';
        }
        if (flat.mixedGraph) {
            mjson << ",\"mixedGraph\":true";
        }
        if (flat.addShader) {
            mjson << ",\"addShader\":true";
        }
    } else {
        mjson << ",\"use_nodes\":false,\"engine\":\"INTERNAL\"";
    }
    mjson << '}';
    if (flat.HasAnyFactor()) {
        mjson << ",\"BLEND_shader_principled\":{";
        bool first = true;
        auto sep = [&]() {
            if (!first) {
                mjson << ',';
            }
            first = false;
        };
        if (flat.hasBaseColor) {
            sep();
            mjson << "\"baseColor\":[" << flat.baseColor[0] << ',' << flat.baseColor[1] << ','
                  << flat.baseColor[2] << ',' << flat.baseColor[3] << ']';
        }
        if (flat.hasMetallic) {
            sep();
            mjson << "\"metallic\":" << flat.metallic;
        }
        if (flat.hasRoughness) {
            sep();
            mjson << "\"roughness\":" << flat.roughness;
        }
        if (flat.hasIor) {
            sep();
            mjson << "\"ior\":" << flat.ior;
        }
        if (flat.hasTransmission) {
            sep();
            mjson << "\"transmission\":" << flat.transmission;
        }
        if (flat.hasAlpha) {
            sep();
            mjson << "\"alpha\":" << flat.alpha;
        }
        if (flat.hasEmissionColor) {
            sep();
            mjson << "\"emissionColor\":[" << flat.emissionColor[0] << ',' << flat.emissionColor[1] << ','
                  << flat.emissionColor[2] << ']';
        }
        if (flat.hasEmissionStrength) {
            sep();
            mjson << "\"emissionStrength\":" << flat.emissionStrength;
        }
        if (flat.hasCoatWeight) {
            sep();
            mjson << "\"coatWeight\":" << flat.coatWeight;
        }
        if (flat.hasCoatRoughness) {
            sep();
            mjson << "\"coatRoughness\":" << flat.coatRoughness;
        }
        if (flat.mapBaseColor || flat.mapRoughness || flat.mapMetallic || flat.mapNormal || flat.mapEmission) {
            sep();
            mjson << "\"maps\":{\"baseColor\":" << BlendMapJson(flat.mapBaseColor)
                  << ",\"roughness\":" << BlendMapJson(flat.mapRoughness)
                  << ",\"metallic\":" << BlendMapJson(flat.mapMetallic)
                  << ",\"normal\":" << BlendMapJson(flat.mapNormal)
                  << ",\"emission\":" << BlendMapJson(flat.mapEmission) << '}';
        }
        mjson << '}';
    }
    // Leftover only: unlinked / usage-unresolved / no file+usage. Omit key if empty.
    bool anyInv = false;
    for (const BlendMapInventoryEntry &e : inventory) {
        if (e.img != nullptr && !e.typed) {
            anyInv = true;
            break;
        }
    }
    if (anyInv) {
        mjson << ",\"maps\":{\"inventory\":[";
        bool ifirst = true;
        for (const BlendMapInventoryEntry &e : inventory) {
            if (e.img == nullptr || e.typed) {
                continue;
            }
            aiString embedName;
            int embedIndex = -1;
            const bool hasFile = BlendEnsureEmbeddedImage(e.img, conv_data, embedName, &embedIndex);
            if (!ifirst) {
                mjson << ',';
            }
            ifirst = false;
            mjson << '{';
            if (e.img->name[0] != '\0') {
                mjson << "\"image\":\"" << BlendJsonEscape(e.img->name + 2) << '"';
            }
            if (e.node != nullptr && e.node->name[0] != '\0') {
                mjson << ",\"node\":\"" << BlendJsonEscape(e.node->name) << '"';
            }
            mjson << ",\"linked\":" << (e.linked ? "true" : "false");
            mjson << ",\"hasEmbed\":" << ((hasFile && embedIndex >= 0) ? "true" : "false");
            mjson << ",\"usageResolved\":false";
            if (hasFile && embedIndex >= 0) {
                mjson << ",\"embed\":\"*" << embedIndex << '"';
            } else if (hasFile && embedName.length > 0) {
                mjson << ",\"path\":\"" << BlendJsonEscape(embedName.C_Str()) << '"';
            }
            mjson << '}';
        }
        mjson << "]}";
    }
    if (flat.hasVolumeColor || flat.hasVolumeDensity || flat.hasVolumeDensityDefault ||
            flat.hasVolumeDensityLinked || flat.hasVolumeDensityAttribute || flat.hasVolumeAnisotropy ||
            flat.hasVolumeAbsorptionColor || flat.hasVolumeEmissionColor || flat.hasVolumeEmissionStrength) {
        mjson << ",\"BLEND_shader_volume\":{";
        bool vfirst = true;
        auto vsep = [&]() {
            if (!vfirst) {
                mjson << ',';
            }
            vfirst = false;
        };
        vsep();
        mjson << "\"output\":\"Principled Volume\"";
        if (flat.hasVolumeColor) {
            vsep();
            mjson << "\"color\":[" << flat.volumeColor[0] << ',' << flat.volumeColor[1] << ','
                  << flat.volumeColor[2] << ',' << flat.volumeColor[3] << ']';
        }
        if (flat.hasVolumeDensity) {
            vsep();
            mjson << "\"density\":" << flat.volumeDensity;
        }
        if (flat.hasVolumeDensityLinked) {
            vsep();
            mjson << "\"densityLinked\":true";
        }
        if (flat.hasVolumeDensityDefault) {
            vsep();
            mjson << "\"densityDefault\":" << flat.volumeDensityDefault;
        }
        if (flat.hasVolumeDensityAttribute) {
            vsep();
            mjson << "\"densityAttribute\":\"" << BlendJsonEscape(flat.volumeDensityAttribute) << '"';
        }
        if (flat.hasVolumeAnisotropy) {
            vsep();
            mjson << "\"anisotropy\":" << flat.volumeAnisotropy;
        }
        if (flat.hasVolumeAbsorptionColor) {
            vsep();
            mjson << "\"absorptionColor\":[" << flat.volumeAbsorptionColor[0] << ','
                  << flat.volumeAbsorptionColor[1] << ',' << flat.volumeAbsorptionColor[2] << ','
                  << flat.volumeAbsorptionColor[3] << ']';
        }
        if (flat.hasVolumeEmissionStrength) {
            vsep();
            mjson << "\"emissionStrength\":" << flat.volumeEmissionStrength;
        }
        if (flat.hasVolumeEmissionColor) {
            vsep();
            mjson << "\"emissionColor\":[" << flat.volumeEmissionColor[0] << ','
                  << flat.volumeEmissionColor[1] << ',' << flat.volumeEmissionColor[2] << ','
                  << flat.volumeEmissionColor[3] << ']';
        }
        mjson << '}';
    }
    {
        const std::string net = prebuiltGraph != nullptr ?
                                        BlendBuildShaderNetworkJson(mat, conv_data, *prebuiltGraph) :
                                        BlendBuildShaderNetworkJson(mat, conv_data);
        if (!net.empty()) {
            mjson << ",\"BLEND_shader_network\":" << net;
        }
    }
    mjson << '}';
    return mjson.str();
}

std::string BuildBlendCustomDataLayersJson(const Assimp::Blender::Mesh *mesh) {
    using namespace Assimp::Blender;
    std::ostringstream json;
    json << "{\"schemaVersion\":1,\"source\":{\"format\":\"blend\"},\"meshName\":\""
         << BlendJsonEscape(mesh->id.name + 2) << "\",\"customDataLayers\":[";
    bool first = true;
    auto emitLayers = [&](const CustomData &cd, const char *domain) {
        for (const auto &layer : cd.layers) {
            if (!layer) {
                continue;
            }
            if (!first) {
                json << ',';
            }
            first = false;
            json << "{\"domain\":\"" << domain << "\",\"type\":" << layer->type
                 << ",\"name\":\"" << BlendJsonEscape(layer->name) << "\"}";
        }
    };
    emitLayers(mesh->vdata, "vertex");
    emitLayers(mesh->ldata, "loop");
    json << "]}";
    return json.str();
}

static const char *BlendRigidShapeName(short shape) {
    using R = Assimp::Blender::RigidBodyOb;
    switch (shape) {
    case R::Shape_Box: return "box";
    case R::Shape_Sphere: return "sphere";
    case R::Shape_Capsule: return "capsule";
    case R::Shape_Cylinder: return "cylinder";
    case R::Shape_Cone: return "cone";
    case R::Shape_ConvexH: return "convex";
    case R::Shape_Trimesh: return "trimesh";
    case R::Shape_Compound: return "compound";
    default: return "box";
    }
}

/** Emit one mesh triangle soup into KHR_physics_collider_meshes (Presence only). */
static bool BlendAppendColliderMeshSoup(std::ostringstream &meshesJson, bool &first,
        int meshIdx, const char *name, const aiMesh *mesh) {
    if (mesh == nullptr || mesh->mVertices == nullptr || mesh->mNumVertices < 3 ||
            mesh->mFaces == nullptr || mesh->mNumFaces == 0) {
        return false;
    }
    if (!first) {
        meshesJson << ',';
    }
    first = false;
    meshesJson << "{\"index\":" << meshIdx << ",\"name\":\"" << BlendJsonEscape(name ? name : "")
               << "\",\"positions\":[";
    for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
        if (v) {
            meshesJson << ',';
        }
        const aiVector3D &p = mesh->mVertices[v];
        meshesJson << p.x << ',' << p.y << ',' << p.z;
    }
    meshesJson << "],\"indices\":[";
    bool idxFirst = true;
    for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
        const aiFace &face = mesh->mFaces[f];
        if (face.mNumIndices < 3 || face.mIndices == nullptr) {
            continue;
        }
        for (unsigned k = 1; k + 1 < face.mNumIndices; ++k) {
            if (!idxFirst) {
                meshesJson << ',';
            }
            idxFirst = false;
            meshesJson << face.mIndices[0] << ',' << face.mIndices[k] << ',' << face.mIndices[k + 1];
        }
    }
    meshesJson << "]}";
    return !idxFirst;
}

// Append mesh indices + material bind for fluid/smoke domain/flow visual meshes.
static void BlendAppendNodeMeshMaterialBind(std::ostringstream &os, const aiNode *node,
        Assimp::Blender::ConversionData &conv) {
    if (node == nullptr || node->mNumMeshes == 0) {
        return;
    }
    os << ",\"meshes\":[";
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        if (i) {
            os << ',';
        }
        os << node->mMeshes[i];
    }
    os << ']';

    std::vector<unsigned int> mats;
    mats.reserve(node->mNumMeshes);
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        const unsigned int mi = node->mMeshes[i];
        if (mi >= conv.meshes->size()) {
            continue;
        }
        const aiMesh *mesh = conv.meshes[mi];
        if (mesh == nullptr || mesh->mMaterialIndex == static_cast<unsigned int>(-1)) {
            continue;
        }
        if (std::find(mats.begin(), mats.end(), mesh->mMaterialIndex) == mats.end()) {
            mats.push_back(mesh->mMaterialIndex);
        }
    }
    if (mats.empty()) {
        return;
    }
    os << ",\"materialIndex\":" << mats[0];
    if (mats[0] < conv.materials_raw.size() && conv.materials_raw[mats[0]]) {
        os << ",\"materialName\":\"" << BlendJsonEscape(conv.materials_raw[mats[0]]->id.name + 2) << '"';
    }
    os << ",\"materials\":[";
    for (size_t i = 0; i < mats.size(); ++i) {
        if (i) {
            os << ',';
        }
        os << "{\"index\":" << mats[i];
        if (mats[i] < conv.materials_raw.size() && conv.materials_raw[mats[i]]) {
            os << ",\"name\":\"" << BlendJsonEscape(conv.materials_raw[mats[i]]->id.name + 2) << '"';
        }
        os << '}';
    }
    os << ']';
}

static void BlendPushPhysicsInv(std::vector<Assimp::Blender::ConversionData::PhysicsInvNode> &bag,
        const char *name, const std::string &json) {
    Assimp::Blender::ConversionData::PhysicsInvNode entry;
    entry.name = name ? name : "";
    entry.json = json;
    bag.push_back(std::move(entry));
}

/** Collect sculpt Cloth brushes (Brush.sculpt_tool) into BLEND_physics_cloth Presence. */
static void BlendCollectClothBrushPresence(Assimp::Blender::ConversionData &conv,
        const Assimp::Blender::Scene *scene) {
    using namespace Assimp::Blender;
    const auto it = conv.db.dna.indices.find("Brush");
    if (it == conv.db.dna.indices.end()) {
        return;
    }
    const Structure &ss = conv.db.dna.structures[it->second];
    DNA::FactoryPair builders = conv.db.dna.GetBlobToStructureConverter(ss, conv.db);
    if (!builders.first || !builders.second) {
        return;
    }

    auto deformKind = [](int t) -> const char * {
        switch (t) {
        case Brush::ClothDeform_Drag: return "drag";
        case Brush::ClothDeform_Push: return "push";
        case Brush::ClothDeform_Grab: return "grab";
        case Brush::ClothDeform_PinchPoint: return "pinch_point";
        case Brush::ClothDeform_PinchPerpendicular: return "pinch_perpendicular";
        case Brush::ClothDeform_Inflate: return "inflate";
        case Brush::ClothDeform_Expand: return "expand";
        case Brush::ClothDeform_SnakeHook: return "snake_hook";
        default: return nullptr;
        }
    };
    auto falloffKind = [](int t) -> const char * {
        switch (t) {
        case Brush::ClothFalloff_Radial: return "radial";
        case Brush::ClothFalloff_Plane: return "plane";
        default: return nullptr;
        }
    };
    auto areaKind = [](int t) -> const char * {
        switch (t) {
        case Brush::ClothArea_Local: return "local";
        case Brush::ClothArea_Global: return "global";
        case Brush::ClothArea_Dynamic: return "dynamic";
        default: return nullptr;
        }
    };
    auto boundaryDeformKind = [](int t) -> const char * {
        switch (t) {
        case Brush::BoundaryDeform_Bend: return "bend";
        case Brush::BoundaryDeform_Expand: return "expand";
        case Brush::BoundaryDeform_Inflate: return "inflate";
        case Brush::BoundaryDeform_Grab: return "grab";
        case Brush::BoundaryDeform_Twist: return "twist";
        case Brush::BoundaryDeform_Smooth: return "smooth";
        default: return nullptr;
        }
    };
    auto boundaryFalloffKind = [](int t) -> const char * {
        switch (t) {
        case Brush::BoundaryFalloff_Constant: return "constant";
        case Brush::BoundaryFalloff_Radius: return "radius";
        case Brush::BoundaryFalloff_Loop: return "loop";
        case Brush::BoundaryFalloff_LoopInvert: return "loop_invert";
        default: return nullptr;
        }
    };

    for (const FileBlockHead &bl : conv.db.entries) {
        if (bl.dna_index != it->second) {
            continue;
        }
        std::shared_ptr<Brush> brush;
        conv.db.cache(brush).get(ss, brush, bl.address, 0);
        if (!brush) {
            const auto pold = conv.db.reader->GetCurrentPos();
            const Pointer prevScope = conv.db.active_id_scope;
            conv.db.reader->SetCurrentPos(bl.start);
            conv.db.active_id_scope = bl.address;
            std::shared_ptr<ElemBase> base = (ss.*builders.first)();
            brush = std::static_pointer_cast<Brush>(base);
            conv.db.cache(brush).set(ss, brush, bl.address, 0);
            (ss.*builders.second)(base, conv.db);
            BlendElemBindConvertedHull(*base, ss.name.c_str(), ss.name.c_str());
            conv.db.active_id_scope = prevScope;
            conv.db.reader->SetCurrentPos(pold);
        }
        if (!brush || !brush->hasSculptTool) {
            continue;
        }
        const int tool = static_cast<unsigned char>(brush->sculpt_tool);
        // DNA eBrushSculptType: SCULPT_BRUSH_TYPE_CLOTH=26, BOUNDARY=30.
        const bool isClothTool = (tool == Brush::Sculpt_Cloth);
        const bool isBoundaryCloth = (tool == Brush::Sculpt_Boundary);
        if (!isClothTool && !isBoundaryCloth) {
            continue;
        }

        const char *bname = brush->id.name[2] != '\0' ? brush->id.name + 2 : brush->id.name;
        std::ostringstream cj;
        cj << "{\"name\":\"" << BlendJsonEscape(bname)
           << "\",\"type\":\"cloth\",\"settings\":{"
           << "\"source\":\"Brush\""
           << ",\"sculpt_tool\":" << tool
           << ",\"sculpt_tool_kind\":\"" << (isClothTool ? "cloth" : "boundary") << '"';
        if (brush->size != 0) {
            cj << ",\"size\":" << brush->size;
        }
        cj << ",\"alpha\":" << brush->alpha;
        if (brush->hasFlag2) {
            cj << ",\"flag2\":" << brush->flag2
               << ",\"cloth_pin_simulation_boundary\":"
               << ((brush->flag2 & Brush::Flag2_ClothPinSimulationBoundary) ? "true" : "false")
               << ",\"cloth_use_collision\":"
               << ((brush->flag2 & Brush::Flag2_ClothUseCollision) ? "true" : "false");
        }
        if (brush->hasBoundaryDeform) {
            cj << ",\"boundary_deform_type\":" << brush->boundary_deform_type;
            if (const char *bk = boundaryDeformKind(brush->boundary_deform_type)) {
                cj << ",\"boundary_deform_kind\":\"" << bk << '"';
            }
            cj << ",\"boundary_falloff_type\":" << brush->boundary_falloff_type;
            if (const char *bf = boundaryFalloffKind(brush->boundary_falloff_type)) {
                cj << ",\"boundary_falloff_kind\":\"" << bf << '"';
            }
            cj << ",\"boundary_offset\":" << brush->boundary_offset;
        }
        if (brush->hasClothParms) {
            cj << ",\"cloth_deform_type\":" << brush->cloth_deform_type;
            if (const char *dk = deformKind(brush->cloth_deform_type)) {
                cj << ",\"cloth_deform_kind\":\"" << dk << '"';
            }
            cj << ",\"cloth_force_falloff_type\":" << brush->cloth_force_falloff_type;
            if (const char *fk = falloffKind(brush->cloth_force_falloff_type)) {
                cj << ",\"cloth_force_falloff_kind\":\"" << fk << '"';
            }
            cj << ",\"cloth_simulation_area_type\":" << brush->cloth_simulation_area_type;
            if (const char *ak = areaKind(brush->cloth_simulation_area_type)) {
                cj << ",\"cloth_simulation_area_kind\":\"" << ak << '"';
            }
            cj << ",\"cloth_mass\":" << brush->cloth_mass
               << ",\"cloth_damping\":" << brush->cloth_damping
               << ",\"cloth_sim_limit\":" << brush->cloth_sim_limit
               << ",\"cloth_sim_falloff\":" << brush->cloth_sim_falloff
               << ",\"cloth_constraint_softbody_strength\":" << brush->cloth_constraint_softbody_strength;
        }
        cj << "}}";
        BlendPushPhysicsInv(conv.physicsClothNodes, bname, cj.str());
    }

    // Scene sculpt gravity Presence (DNA_scene_types.h Sculpt).
    if (scene && scene->toolsettings && scene->toolsettings->sculpt) {
        const Sculpt &sc = *scene->toolsettings->sculpt;
        if (!sc.hasGravityFactor && !sc.gravity_object) {
            return;
        }
        std::ostringstream gj;
        gj << "{\"name\":\"Sculpt\",\"type\":\"cloth\",\"settings\":{"
           << "\"source\":\"Sculpt\"";
        if (sc.hasGravityFactor) {
            gj << ",\"gravity_factor\":" << sc.gravity_factor;
        }
        if (sc.gravity_object) {
            const char *gon = sc.gravity_object->id.name[2] != '\0' ?
                                      sc.gravity_object->id.name + 2 :
                                      sc.gravity_object->id.name;
            gj << ",\"gravity_object\":\"" << BlendJsonEscape(gon) << '"';
        }
        gj << "}}";
        BlendPushPhysicsInv(conv.physicsClothNodes, "Sculpt", gj.str());
    }
}

// Pre-order DFS indices - same order as Host flattenSceneNodes / skinNodes.
static void BlendBuildHierarchyIndexMap(const aiNode *node, std::unordered_map<std::string, int> &out, int &next) {
    if (node == nullptr) {
        return;
    }
    const int self = next++;
    const char *nm = node->mName.C_Str();
    if (out.find(nm) == out.end()) {
        out.emplace(nm, self);
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        BlendBuildHierarchyIndexMap(node->mChildren[i], out, next);
    }
}

static void BlendEmitPhysicsInvBag(std::ostringstream &pjson, const char *key,
        const std::vector<Assimp::Blender::ConversionData::PhysicsInvNode> &nodes,
        const std::unordered_map<std::string, int> &nameToIndex) {
    if (nodes.empty()) {
        return;
    }
    pjson << ",\"" << key << "\":{\"nodes\":[";
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (i) {
            pjson << ',';
        }
        int idx = -1;
        const auto it = nameToIndex.find(nodes[i].name);
        if (it != nameToIndex.end()) {
            idx = it->second;
        }
        const std::string &j = nodes[i].json;
        if (!j.empty() && j[0] == '{') {
            pjson << "{\"index\":" << idx << ',' << j.substr(1);
        } else {
            pjson << j;
        }
    }
    pjson << "]}";
}

static void BlendAppendLimit(std::ostringstream &limits, bool &first, const char *axesKey, int axis,
        float mn, float mx) {
    if (!first) {
        limits << ',';
    }
    first = false;
    limits << "{\"" << axesKey << "\":[" << axis << "],\"min\":" << mn << ",\"max\":" << mx << '}';
}

static void BlendEmitKhrPhysics(std::ostringstream &pjson, Assimp::Blender::ConversionData &conv,
        const std::unordered_map<std::string, int> &nameToIndex, const aiScene *scene) {
    using namespace Assimp::Blender;
    if (conv.khrBodies.empty() && conv.khrJoints.empty()) {
        return;
    }

    // Unique col_groups masks -> collisionFilters (System_N = bit N).
    std::vector<int> filterMasks;
    for (const auto &b : conv.khrBodies) {
        if (std::find(filterMasks.begin(), filterMasks.end(), b.colGroups) == filterMasks.end()) {
            filterMasks.push_back(b.colGroups);
        }
    }
    std::ostringstream filters;
    for (size_t fi = 0; fi < filterMasks.size(); ++fi) {
        if (fi) {
            filters << ',';
        }
        filters << "{\"collisionSystems\":[";
        bool firstSys = true;
        for (int bit = 0; bit < 20; ++bit) {
            if ((filterMasks[fi] & (1 << bit)) == 0) {
                continue;
            }
            if (!firstSys) {
                filters << ',';
            }
            firstSys = false;
            filters << "\"System_" << bit << '"';
        }
        filters << "],\"collideWithSystems\":[";
        firstSys = true;
        for (int bit = 0; bit < 20; ++bit) {
            if ((filterMasks[fi] & (1 << bit)) == 0) {
                continue;
            }
            if (!firstSys) {
                filters << ',';
            }
            firstSys = false;
            filters << "\"System_" << bit << '"';
        }
        filters << "]}";
    }

    std::ostringstream joints;
    std::ostringstream jointNodes;
    bool jointNodeFirst = true;
    int emittedJointDefs = 0;
    for (const ConversionData::KhrJointPending &jp : conv.khrJoints) {
        if ((jp.flag & RigidBodyCon::Flag_Enabled) == 0) {
            continue;
        }
        std::ostringstream limits;
        bool limFirst = true;
        if (jp.flag & RigidBodyCon::Flag_UseLimitLinX) {
            BlendAppendLimit(limits, limFirst, "linearAxes", 0, jp.limit_lin_x_lower, jp.limit_lin_x_upper);
        }
        if (jp.flag & RigidBodyCon::Flag_UseLimitLinY) {
            BlendAppendLimit(limits, limFirst, "linearAxes", 1, jp.limit_lin_y_lower, jp.limit_lin_y_upper);
        }
        if (jp.flag & RigidBodyCon::Flag_UseLimitLinZ) {
            BlendAppendLimit(limits, limFirst, "linearAxes", 2, jp.limit_lin_z_lower, jp.limit_lin_z_upper);
        }
        if (jp.flag & RigidBodyCon::Flag_UseLimitAngX) {
            BlendAppendLimit(limits, limFirst, "angularAxes", 0, jp.limit_ang_x_lower, jp.limit_ang_x_upper);
        }
        if (jp.flag & RigidBodyCon::Flag_UseLimitAngY) {
            BlendAppendLimit(limits, limFirst, "angularAxes", 1, jp.limit_ang_y_lower, jp.limit_ang_y_upper);
        }
        if (jp.flag & RigidBodyCon::Flag_UseLimitAngZ) {
            BlendAppendLimit(limits, limFirst, "angularAxes", 2, jp.limit_ang_z_lower, jp.limit_ang_z_upper);
        }

        std::ostringstream drives;
        bool drvFirst = true;
        auto appendSpringDrive = [&](const char *type, int axis, float stiffness, float damping) {
            // Presence: only when spring flag is set (values may be 0).
            if (!drvFirst) {
                drives << ',';
            }
            drvFirst = false;
            drives << "{\"type\":\"" << type << "\",\"mode\":\"force\",\"axis\":" << axis
                   << ",\"positionTarget\":0,\"velocityTarget\":0"
                   << ",\"stiffness\":" << stiffness
                   << ",\"damping\":" << damping << '}';
        };
        if (jp.flag & RigidBodyCon::Flag_UseSpringX) {
            appendSpringDrive("linear", 0, jp.spring_stiffness_x, jp.spring_damping_x);
        }
        if (jp.flag & RigidBodyCon::Flag_UseSpringY) {
            appendSpringDrive("linear", 1, jp.spring_stiffness_y, jp.spring_damping_y);
        }
        if (jp.flag & RigidBodyCon::Flag_UseSpringZ) {
            appendSpringDrive("linear", 2, jp.spring_stiffness_z, jp.spring_damping_z);
        }
        if (jp.flag & RigidBodyCon::Flag_UseSpringAngX) {
            appendSpringDrive("angular", 0, jp.spring_stiffness_ang_x, jp.spring_damping_ang_x);
        }
        if (jp.flag & RigidBodyCon::Flag_UseSpringAngY) {
            appendSpringDrive("angular", 1, jp.spring_stiffness_ang_y, jp.spring_damping_ang_y);
        }
        if (jp.flag & RigidBodyCon::Flag_UseSpringAngZ) {
            appendSpringDrive("angular", 2, jp.spring_stiffness_ang_z, jp.spring_damping_ang_z);
        }
        if (jp.flag & RigidBodyCon::Flag_UseMotorLin) {
            if (!drvFirst) {
                drives << ',';
            }
            drvFirst = false;
            drives << "{\"type\":\"linear\",\"mode\":\"force\",\"axis\":0"
                   << ",\"velocityTarget\":" << jp.motor_lin_target_velocity
                   << ",\"maxForce\":" << jp.motor_lin_max_impulse << '}';
        }
        if (jp.flag & RigidBodyCon::Flag_UseMotorAng) {
            if (!drvFirst) {
                drives << ',';
            }
            drvFirst = false;
            drives << "{\"type\":\"angular\",\"mode\":\"force\",\"axis\":0"
                   << ",\"velocityTarget\":" << jp.motor_ang_target_velocity
                   << ",\"maxForce\":" << jp.motor_ang_max_impulse << '}';
        }

        if (emittedJointDefs > 0) {
            joints << ',';
        }
        const int defIdx = emittedJointDefs++;
        const char *jointTypeName = "generic";
        switch (jp.type) {
        case RigidBodyCon::Type_Point: jointTypeName = "point"; break;
        case RigidBodyCon::Type_Hinge: jointTypeName = "hinge"; break;
        case RigidBodyCon::Type_Slider: jointTypeName = "slider"; break;
        case RigidBodyCon::Type_6Dof: jointTypeName = "generic"; break; // limits encode free axes
        case RigidBodyCon::Type_6DofSpring: jointTypeName = "generic"; break;
        case RigidBodyCon::Type_Fixed: jointTypeName = "fixed"; break;
        case RigidBodyCon::Type_Piston: jointTypeName = "slider"; break;
        case RigidBodyCon::Type_Motor: jointTypeName = "generic"; break;
        default: break;
        }
        // Hinge without explicit ang-limit flags: Blender hinge frees X rotation by default.
        if (jp.type == RigidBodyCon::Type_Hinge && limFirst) {
            BlendAppendLimit(limits, limFirst, "linearAxes", 0, 0.f, 0.f);
            BlendAppendLimit(limits, limFirst, "linearAxes", 1, 0.f, 0.f);
            BlendAppendLimit(limits, limFirst, "linearAxes", 2, 0.f, 0.f);
            BlendAppendLimit(limits, limFirst, "angularAxes", 1, 0.f, 0.f);
            BlendAppendLimit(limits, limFirst, "angularAxes", 2, 0.f, 0.f);
        }
        joints << "{\"limits\":[" << limits.str() << "],\"drives\":[" << drives.str() << ']'
               << ",\"constraintType\":" << jp.type
               << ",\"jointType\":\"" << jointTypeName << '"'
               << ",\"breakingThreshold\":" << jp.breaking_threshold << '}';

        // Attach joint on constraint Empty; connectedNode = ob2 (DFS index).
        int selfIdx = -1;
        int connectedIdx = -1;
        const auto itSelf = nameToIndex.find(jp.constraintName);
        if (itSelf != nameToIndex.end()) {
            selfIdx = itSelf->second;
        }
        const auto itB = nameToIndex.find(jp.ob2Name);
        if (itB != nameToIndex.end()) {
            connectedIdx = itB->second;
        }
        if (selfIdx < 0 || connectedIdx < 0 || connectedIdx == selfIdx) {
            continue;
        }
        const bool enableCol = (jp.flag & RigidBodyCon::Flag_DisableCollisions) == 0;
        if (!jointNodeFirst) {
            jointNodes << ',';
        }
        jointNodeFirst = false;
        jointNodes << "{\"index\":" << selfIdx
                   << ",\"name\":\"" << BlendJsonEscape(jp.constraintName)
                   << "\",\"KHR_physics_rigid_bodies\":{\"joint\":{"
                   << "\"connectedNode\":" << connectedIdx
                   << ",\"joint\":" << defIdx
                   << ",\"enableCollision\":" << (enableCol ? "true" : "false")
                   << "}}}";
    }

    std::ostringstream nodes;
    std::ostringstream colliderMeshes;
    bool nodeFirst = true;
    bool colliderMeshFirst = true;
    std::set<int> emittedColliderMeshes;
    for (const auto &b : conv.khrBodies) {
        int idx = -1;
        const auto it = nameToIndex.find(b.name);
        if (it != nameToIndex.end()) {
            idx = it->second;
        }
        int filterIdx = -1;
        for (size_t fi = 0; fi < filterMasks.size(); ++fi) {
            if (filterMasks[fi] == b.colGroups) {
                filterIdx = static_cast<int>(fi);
                break;
            }
        }
        if (!nodeFirst) {
            nodes << ',';
        }
        nodeFirst = false;
        nodes << "{\"index\":" << idx << ",\"name\":\"" << BlendJsonEscape(b.name)
              << "\",\"KHR_physics_rigid_bodies\":{";
        if (b.isTrigger) {
            // Disabled RBO -> trigger volume (KHR parity with Gravity* sensors).
            nodes << "\"trigger\":{\"geometry\":{";
            if (b.meshIdx >= 0) {
                nodes << "\"mesh\":" << b.meshIdx;
            } else {
                nodes << "\"shape\":" << b.shapeIdx;
            }
            nodes << '}';
            if (filterIdx >= 0) {
                nodes << ",\"collisionFilter\":" << filterIdx;
            }
            nodes << '}';
        } else {
            if (!b.motionType.empty()) {
                nodes << "\"motion\":{\"type\":\"" << b.motionType << "\",\"mass\":" << b.mass << "},";
            }
            nodes << "\"collider\":{\"geometry\":{";
            if (b.meshIdx >= 0) {
                nodes << "\"convexHull\":" << (b.convexHull ? "true" : "false")
                      << ",\"mesh\":" << b.meshIdx;
            } else {
                nodes << "\"shape\":" << b.shapeIdx;
            }
            nodes << "},\"physicsMaterial\":" << b.matIdx;
            if (filterIdx >= 0) {
                nodes << ",\"collisionFilter\":" << filterIdx;
            }
            nodes << '}';
        }
        nodes << "}}";

        // Meshes already moved to aiScene (conv.meshes dismissed) before emit.
        if (b.meshIdx >= 0 && emittedColliderMeshes.insert(b.meshIdx).second && scene != nullptr &&
                b.meshIdx < static_cast<int>(scene->mNumMeshes) && scene->mMeshes != nullptr) {
            const aiMesh *am = scene->mMeshes[static_cast<unsigned int>(b.meshIdx)];
            BlendAppendColliderMeshSoup(colliderMeshes, colliderMeshFirst, b.meshIdx, b.name.c_str(), am);
        }
    }
    if (!jointNodeFirst) {
        if (!nodeFirst) {
            nodes << ',';
        }
        nodes << jointNodes.str();
    }

    if (!conv.khrShapeFirst) {
        pjson << ",\"KHR_implicit_shapes\":{\"shapes\":[" << conv.khrShapes.str() << "]}";
    }
    pjson << ",\"KHR_physics_rigid_bodies\":{\"physicsMaterials\":[" << conv.khrMaterials.str()
          << "],\"collisionFilters\":[" << filters.str()
          << "],\"physicsJoints\":[" << joints.str() << "]}";
    pjson << ",\"nodes\":[" << nodes.str() << ']';
    if (!colliderMeshFirst) {
        pjson << ",\"KHR_physics_collider_meshes\":{\"meshes\":[" << colliderMeshes.str() << "]}";
    }
}

/** DNA Particle MTex mapto bits (eParticleTextureInfluence) - not material MAP_*. */
enum BlendParticleMapto : int {
    BlendPamap_Time = 1 << 0,
    BlendPamap_Life = 1 << 1,
    BlendPamap_Dens = 1 << 2,
    BlendPamap_Size = 1 << 3,
    BlendPamap_Length = 1 << 4,
    BlendPamap_IVel = 1 << 5,
    BlendPamap_Field = 1 << 6,
    BlendPamap_Clump = 1 << 7,
    BlendPamap_KinkFreq = 1 << 8,
    BlendPamap_Rough = 1 << 9,
    BlendPamap_Gravity = 1 << 10,
    BlendPamap_Damp = 1 << 11,
    BlendPamap_KinkAmp = 1 << 12,
    BlendPamap_Twist = 1 << 13,
};

/** DNA eMTex_TexCo - TEXCO_UV bit (makesdna DNA_material_types.h). */
enum BlendMTexTexCo : int {
    BlendTexco_UV = 1 << 4,
};

/** Build comma-separated PAMAP labels from DNA mapto bits (key=value meta; no JSON). */
static std::string BlendParticleInfluenceLabels(int mapBits) {
    std::string out;
    auto add = [&](int bit, const char *name) {
        if ((mapBits & bit) == 0) {
            return;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += name;
    };
    add(BlendPamap_Time, "time");
    add(BlendPamap_Life, "life");
    add(BlendPamap_Dens, "density");
    add(BlendPamap_Size, "size");
    add(BlendPamap_Length, "length");
    add(BlendPamap_IVel, "ivel");
    add(BlendPamap_Field, "field");
    add(BlendPamap_Clump, "clump");
    add(BlendPamap_KinkFreq, "kinkFreq");
    add(BlendPamap_Rough, "rough");
    add(BlendPamap_Gravity, "gravity");
    add(BlendPamap_Damp, "damp");
    add(BlendPamap_KinkAmp, "kinkAmp");
    add(BlendPamap_Twist, "twist");
    return out;
}

/**
 * 0al4: DNA ParticleSettings.mtex[] -> typed aiMaterial ($tex.file + key=value meta).
 * Sidecar textures[] decommissioned - Host binds via typedMaterial / meta.
 */
static void BlendEmitParticleTypedMTexMaterials(Assimp::Blender::ConversionData &conv) {
    using namespace Assimp::Blender;
    for (const ParticleSettings *part : conv.pendingParticleMtex) {
        if (part == nullptr || part->id.name[2] == '\0') {
            continue;
        }
        const std::string settingsName = part->id.name + 2;
        // Dedupe by material name (shared ParticleSettings across systems).
        bool already = false;
        for (const aiMaterial *existing : conv.materials.get()) {
            if (existing == nullptr) {
                continue;
            }
            aiString en;
            if (existing->Get(AI_MATKEY_NAME, en) == AI_SUCCESS &&
                    std::strcmp(en.C_Str(), settingsName.c_str()) == 0) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }

        for (size_t i = 0; i < sizeof(conv.next_texture) / sizeof(conv.next_texture[0]); ++i) {
            conv.next_texture[i] = 0;
        }

        aiMaterial *mout = new aiMaterial();
        aiString matName(settingsName);
        mout->AddProperty(&matName, AI_MATKEY_NAME);
        {
            // Presence marker: this mat carries Particle MTex maps (not Looks BASE/NORMAL).
            const int isPs = 1;
            mout->AddProperty(&isPs, 1, "$mat.blend.particleSettings", 0, 0);
            aiString psName(settingsName);
            mout->AddProperty(&psName, "$mat.blend.particleSettingsName", 0, 0);
        }

        const aiTextureType texType = aiTextureType_UNKNOWN;
        unsigned emitted = 0;
        for (size_t ti = 0; ti < sizeof(part->mtex) / sizeof(part->mtex[0]); ++ti) {
            if (!part->mtex[ti] || !part->mtex[ti]->tex || !part->mtex[ti]->tex->ima) {
                continue;
            }
            const MTex &mtx = *part->mtex[ti];
            const Image *img = mtx.tex->ima.get();
            aiString embedName;
            if (!BlendEnsureEmbeddedImage(img, conv, embedName)) {
                continue;
            }
            const unsigned slot = conv.next_texture[texType]++;
            mout->AddProperty(&embedName, AI_MATKEY_TEXTURE(texType, slot));

            const int mapBits = static_cast<int>(mtx.mapto);
            const std::string influences = BlendParticleInfluenceLabels(mapBits);
            // UNKNOWN typed slot: usage + mapto so Host can parse density/length/... later.
            const char *usageLabel = influences.empty() ? "particle" : influences.c_str();
            BlendEmitTexUsageMeta(mout, texType, slot, usageLabel, mapBits, true, mtx.uvname);
            BlendEmitUvTransformFromMTex(mout, texType, slot, mtx);
            {
                const int dnaSlot = static_cast<int>(ti);
                mout->AddProperty(&dnaSlot, 1, "$tex.blend.mtexSlot", texType, slot);
            }
            if (!influences.empty()) {
                aiString inf(influences);
                mout->AddProperty(&inf, "$tex.blend.particleInfluence", texType, slot);
            }
            if (mtx.hasPadensfac) {
                mout->AddProperty(&mtx.padensfac, 1, "$tex.blend.densityFac", texType, slot);
            }
            if (mtx.hasLengthfac) {
                mout->AddProperty(&mtx.lengthfac, 1, "$tex.blend.lengthFac", texType, slot);
            }
            if (mtx.hasTimefac) {
                mout->AddProperty(&mtx.timefac, 1, "$tex.blend.timeFac", texType, slot);
            }
            if (mtx.hasLifefac) {
                mout->AddProperty(&mtx.lifefac, 1, "$tex.blend.lifeFac", texType, slot);
            }
            if (mtx.hasSizefac) {
                mout->AddProperty(&mtx.sizefac, 1, "$tex.blend.sizeFac", texType, slot);
            }
            // UVWSRC when DNA TEXCO_UV (named uvname or default/render UV after active_rnd).
            // BlendFindUvIndex returns -1 if unresolved - never invent UV0 when missing.
            {
                bool resolveUv = false;
                if (mtx.hasTexco) {
                    resolveUv = (mtx.texco & BlendTexco_UV) != 0;
                } else if (mtx.uvname[0] != '\0') {
                    // Older DNA without texco: named UV still means UV mapping.
                    resolveUv = true;
                }
                if (resolveUv) {
                    const int uvIndex = BlendFindUvIndex(conv, 0u, mtx.uvname);
                    BlendEmitUvWsrc(mout, texType, slot, uvIndex);
                }
            }
            ++emitted;
        }
        if (emitted == 0) {
            delete mout;
            continue;
        }
        conv.materials->push_back(mout);
        conv.materialSidecarJson.emplace_back();
    }
    conv.pendingParticleMtex.clear();
}

static const char *BlendParticleVgSlotName(int slot) {
    using Assimp::Blender::ParticleSystem;
    switch (slot) {
    case ParticleSystem::VG_Density: return "density";
    case ParticleSystem::VG_Vel: return "velocity";
    case ParticleSystem::VG_Length: return "length";
    case ParticleSystem::VG_Clump: return "clump";
    case ParticleSystem::VG_Kink: return "kink";
    case ParticleSystem::VG_Rough1: return "rough1";
    case ParticleSystem::VG_Rough2: return "rough2";
    case ParticleSystem::VG_RoughE: return "rough_end";
    case ParticleSystem::VG_Size: return "size";
    case ParticleSystem::VG_Tan: return "tangent";
    case ParticleSystem::VG_Rot: return "rotation";
    case ParticleSystem::VG_Effector: return "effector";
    case ParticleSystem::VG_Twist: return "twist";
    default: return nullptr;
    }
}

/** Resolve deform-group names: Mesh.vertex_group_names preferred, else Object.defbase. */
static void BlendCollectDeformGroupNames(const Assimp::Blender::Object *obj,
        const Assimp::Blender::Mesh *mesh, std::vector<std::string> &out) {
    out.clear();
    auto walk = [&out](const Assimp::Blender::ListBase &lb) {
        for (const Assimp::Blender::bDeformGroup *dg =
                        static_cast<const Assimp::Blender::bDeformGroup *>(lb.first.get());
                dg != nullptr;
                dg = static_cast<const Assimp::Blender::bDeformGroup *>(dg->next.get())) {
            out.emplace_back(dg->name[0] ? dg->name : "");
        }
    };
    if (mesh != nullptr && mesh->vertex_group_names.first) {
        walk(mesh->vertex_group_names);
    }
    if (out.empty() && obj != nullptr && obj->defbase.first) {
        walk(obj->defbase);
    }
}

static float BlendVertDeformWeight(const Assimp::Blender::Mesh &mesh, int vertIdx, int defNr) {
    if (vertIdx < 0 || static_cast<size_t>(vertIdx) >= mesh.dvert.size() || defNr < 0) {
        return 0.f;
    }
    const Assimp::Blender::MDeformVert &dv = mesh.dvert[static_cast<size_t>(vertIdx)];
    for (size_t i = 0; i < dv.dw.size(); ++i) {
        if (dv.dw[i].def_nr == defNr) {
            return dv.dw[i].weight;
        }
    }
    return 0.f;
}

/** Area-average poly/face density weight from DNA dvert (Presence; no invent). */
static bool BlendFaceDensityFromVgroup(const Assimp::Blender::Mesh &mesh, int defNr, bool invert,
        std::vector<int> &faceMask, std::vector<float> &faceDensity) {
    faceMask.clear();
    faceDensity.clear();
    if (defNr < 0 || mesh.dvert.empty()) {
        return false;
    }
    auto pushFace = [&](int faceIdx, const int *vids, int nvid) {
        if (nvid <= 0) {
            return;
        }
        float sum = 0.f;
        for (int i = 0; i < nvid; ++i) {
            float w = BlendVertDeformWeight(mesh, vids[i], defNr);
            if (invert) {
                w = 1.f - w;
            }
            sum += w;
        }
        const float avg = sum / static_cast<float>(nvid);
        if (avg > 0.f) {
            faceMask.push_back(faceIdx);
            faceDensity.push_back(avg);
        }
    };
    if (!mesh.mpoly.empty() && !mesh.mloop.empty()) {
        for (size_t pi = 0; pi < mesh.mpoly.size(); ++pi) {
            const Assimp::Blender::MPoly &p = mesh.mpoly[pi];
            if (p.totloop < 3 || p.loopstart < 0) {
                continue;
            }
            std::vector<int> vids;
            vids.reserve(static_cast<size_t>(p.totloop));
            for (int li = 0; li < p.totloop; ++li) {
                const size_t lidx = static_cast<size_t>(p.loopstart + li);
                if (lidx >= mesh.mloop.size()) {
                    vids.clear();
                    break;
                }
                vids.push_back(mesh.mloop[lidx].v);
            }
            if (!vids.empty()) {
                pushFace(static_cast<int>(pi), vids.data(), static_cast<int>(vids.size()));
            }
        }
    } else if (!mesh.mface.empty()) {
        for (size_t fi = 0; fi < mesh.mface.size(); ++fi) {
            const Assimp::Blender::MFace &f = mesh.mface[fi];
            int vids[4] = { f.v1, f.v2, f.v3, f.v4 };
            const int n = (f.v4 != 0) ? 4 : 3;
            pushFace(static_cast<int>(fi), vids, n);
        }
    } else {
        return false;
    }
    return !faceMask.empty();
}

/** DNA mesh-local average face normal (Presence for 0ak eye shells; no invent flip). */
static bool BlendMeshRestAvgNormal(const Assimp::Blender::Mesh &mesh, float out[3]) {
    out[0] = out[1] = out[2] = 0.f;
    size_t count = 0;
    auto accum = [&](float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz) {
        const float abx = bx - ax, aby = by - ay, abz = bz - az;
        const float acx = cx - ax, acy = cy - ay, acz = cz - az;
        float nx = aby * acz - abz * acy;
        float ny = abz * acx - abx * acz;
        float nz = abx * acy - aby * acx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len <= 1e-20f) {
            return;
        }
        out[0] += nx / len;
        out[1] += ny / len;
        out[2] += nz / len;
        ++count;
    };
    auto vert = [&](int i, float &x, float &y, float &z) -> bool {
        if (i < 0 || static_cast<size_t>(i) >= mesh.mvert.size()) {
            return false;
        }
        x = mesh.mvert[static_cast<size_t>(i)].co[0];
        y = mesh.mvert[static_cast<size_t>(i)].co[1];
        z = mesh.mvert[static_cast<size_t>(i)].co[2];
        return true;
    };
    if (!mesh.mpoly.empty() && !mesh.mloop.empty()) {
        for (const Assimp::Blender::MPoly &p : mesh.mpoly) {
            if (p.totloop < 3 || p.loopstart < 0) {
                continue;
            }
            float x0, y0, z0, x1, y1, z1, x2, y2, z2;
            const size_t l0 = static_cast<size_t>(p.loopstart);
            if (l0 + 2 >= mesh.mloop.size()) {
                continue;
            }
            if (!vert(mesh.mloop[l0].v, x0, y0, z0) || !vert(mesh.mloop[l0 + 1].v, x1, y1, z1) ||
                    !vert(mesh.mloop[l0 + 2].v, x2, y2, z2)) {
                continue;
            }
            accum(x0, y0, z0, x1, y1, z1, x2, y2, z2);
        }
    } else {
        for (const Assimp::Blender::MFace &f : mesh.mface) {
            float x0, y0, z0, x1, y1, z1, x2, y2, z2;
            if (!vert(f.v1, x0, y0, z0) || !vert(f.v2, x1, y1, z1) || !vert(f.v3, x2, y2, z2)) {
                continue;
            }
            accum(x0, y0, z0, x1, y1, z1, x2, y2, z2);
        }
    }
    if (count == 0) {
        return false;
    }
    out[0] /= static_cast<float>(count);
    out[1] /= static_cast<float>(count);
    out[2] /= static_cast<float>(count);
    const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (len <= 1e-20f) {
        return false;
    }
    out[0] /= len;
    out[1] /= len;
    out[2] /= len;
    return true;
}

/** Map NodesModifier Input_* IDProperty values to group-socket labels (Presence). */
static void BlendAppendNodesModifierParticlePresence(std::ostringstream &settings,
        const Assimp::Blender::NodesModifierData &nmd) {
    using Assimp::Blender::bNodeSocket;
    using Assimp::Blender::bNodeTree;
    using Assimp::Blender::ElemBase;
    using Assimp::Blender::IDProperty;

    const IDProperty *root = nmd.settings.properties.get();
    if (root == nullptr) {
        return;
    }
    const ElemBase *childFirst = nullptr;
    if (root->type == IDProperty::Type_Group) {
        childFirst = root->data.group.first.get();
    } else if (std::strncmp(root->name, "Input_", 6) == 0) {
        childFirst = root;
    }
    if (childFirst == nullptr) {
        return;
    }

    // identifier (Input_N) -> socket display name from node_group.inputs
    std::unordered_map<std::string, std::string> idToLabel;
    if (nmd.node_group) {
        const bNodeTree &tree = *nmd.node_group;
        for (const ElemBase *e = tree.inputs.first.get(); e != nullptr;) {
            const bNodeSocket *sock = static_cast<const bNodeSocket *>(e);
            if (sock->identifier[0] != '\0' && sock->name[0] != '\0') {
                idToLabel[sock->identifier] = sock->name;
            }
            e = sock->next.get();
        }
    }

    struct NamedVal {
        std::string label;
        const IDProperty *prop;
    };
    std::vector<NamedVal> vals;
    for (const ElemBase *e = childFirst; e != nullptr;) {
        const IDProperty *prop = static_cast<const IDProperty *>(e);
        const char *ident = prop->name;
        if (std::strstr(ident, "_use_attribute") != nullptr ||
                std::strstr(ident, "_attribute_name") != nullptr) {
            e = prop->next.get();
            continue;
        }
        if (std::strncmp(ident, "Input_", 6) != 0) {
            e = prop->next.get();
            continue;
        }
        NamedVal nv;
        auto it = idToLabel.find(ident);
        nv.label = (it != idToLabel.end()) ? it->second : std::string(ident);
        nv.prop = prop;
        vals.push_back(std::move(nv));
        e = prop->next.get();
    }
    if (vals.empty()) {
        return;
    }

    bool haveAttract = false;
    float attractStrength = 0.f;
    bool haveGravity = false;
    float gravity[3] = { 0.f, 0.f, 0.f };
    bool havePhysNewtonian = false;
    bool haveTotpart = false;
    int totpart = 0;
    bool haveEmissionDensity = false;
    float emissionDensity = 0.f;
    bool haveLifetime = false;
    int lifetime = 0;
    std::ostringstream hostKeys;

    settings << ",\"inputs\":{";
    bool firstIn = true;
    for (const NamedVal &nv : vals) {
        const IDProperty &p = *nv.prop;
        if (!firstIn) {
            settings << ',';
        }
        firstIn = false;
        const std::string key = BlendJsonEscape(nv.label.c_str());
        settings << '"' << key << "\":";
        if (p.type == IDProperty::Type_Int) {
            settings << p.data.val;
            // Partikelzahl only from sockets that literally mean count (no invent from Lifetime).
            if (key == "Count" || key == "Amount" || key == "Number") {
                haveTotpart = true;
                totpart = p.data.val;
            }
            if (key == "Lifetime") {
                haveLifetime = true;
                lifetime = p.data.val;
            }
        } else if (p.type == IDProperty::Type_Float) {
            float f = 0.f;
            std::memcpy(&f, &p.data.val, sizeof(float));
            settings << f;
            if (key == "Attraction Strength") {
                haveAttract = true;
                attractStrength = f;
            }
            if (key == "Emission Density") {
                haveEmissionDensity = true;
                emissionDensity = f;
            }
            if (key == "Count" || key == "Amount" || key == "Number") {
                haveTotpart = true;
                totpart = static_cast<int>(f);
            }
            if (key == "Lifetime") {
                haveLifetime = true;
                lifetime = static_cast<int>(f);
            }
        } else if (p.type == IDProperty::Type_Array) {
            settings << '[';
            if (!p.floatArray.empty()) {
                for (size_t i = 0; i < p.floatArray.size(); ++i) {
                    if (i) {
                        settings << ',';
                    }
                    settings << p.floatArray[i];
                }
                if (key == "Gravity" && p.floatArray.size() >= 3) {
                    haveGravity = true;
                    gravity[0] = p.floatArray[0];
                    gravity[1] = p.floatArray[1];
                    gravity[2] = p.floatArray[2];
                    // Authored Gravity on particle GN => Newtonian Presence for Host Force-Step.
                    havePhysNewtonian = true;
                }
            } else if (!p.intArray.empty()) {
                for (size_t i = 0; i < p.intArray.size(); ++i) {
                    if (i) {
                        settings << ',';
                    }
                    settings << p.intArray[i];
                }
            }
            settings << ']';
        } else if (p.type == IDProperty::Type_String) {
            settings << '"' << BlendJsonEscape(p.str.c_str()) << '"';
        } else {
            settings << "null";
        }
    }
    settings << '}';
    if (haveTotpart) {
        hostKeys << ",\"totpart\":" << totpart;
    }
    if (haveEmissionDensity) {
        hostKeys << ",\"emissionDensity\":" << emissionDensity;
    }
    if (haveLifetime) {
        hostKeys << ",\"lifetime\":" << lifetime;
    }
    settings << hostKeys.str();
    if (haveAttract) {
        settings << ",\"attractionStrength\":" << attractStrength;
        settings << ",\"forcefield_kind\":\"attraction\"";
    }
    if (haveGravity) {
        settings << ",\"gravity\":[" << gravity[0] << ',' << gravity[1] << ',' << gravity[2] << ']';
    }
    if (havePhysNewtonian) {
        settings << ",\"phys_kind\":\"newtonian\"";
    }
}

/** DNA ModifierData.type -> short ASCII label (Presence; unknown -> typeN). */
static void BlendModifierTypeLabel(int type, char out[32]) {
    using namespace Assimp::Blender;
    const char *lab = nullptr;
    switch (type) {
    case ModifierData::eModifierType_None: lab = "None"; break;
    case ModifierData::eModifierType_Subsurf: lab = "Subsurf"; break;
    case ModifierData::eModifierType_Lattice: lab = "Lattice"; break;
    case ModifierData::eModifierType_Curve: lab = "Curve"; break;
    case ModifierData::eModifierType_Build: lab = "Build"; break;
    case ModifierData::eModifierType_Mirror: lab = "Mirror"; break;
    case ModifierData::eModifierType_Decimate: lab = "Decimate"; break;
    case ModifierData::eModifierType_Wave: lab = "Wave"; break;
    case ModifierData::eModifierType_Armature: lab = "Armature"; break;
    case ModifierData::eModifierType_Hook: lab = "Hook"; break;
    case ModifierData::eModifierType_Softbody: lab = "Softbody"; break;
    case ModifierData::eModifierType_Boolean: lab = "Boolean"; break;
    case ModifierData::eModifierType_Array: lab = "Array"; break;
    case ModifierData::eModifierType_EdgeSplit: lab = "EdgeSplit"; break;
    case ModifierData::eModifierType_Displace: lab = "Displace"; break;
    case ModifierData::eModifierType_UVProject: lab = "UVProject"; break;
    case ModifierData::eModifierType_Smooth: lab = "Smooth"; break;
    case ModifierData::eModifierType_Cast: lab = "Cast"; break;
    case ModifierData::eModifierType_MeshDeform: lab = "MeshDeform"; break;
    case ModifierData::eModifierType_ParticleSystem: lab = "ParticleSystem"; break;
    case ModifierData::eModifierType_ParticleInstance: lab = "ParticleInstance"; break;
    case ModifierData::eModifierType_Explode: lab = "Explode"; break;
    case ModifierData::eModifierType_Cloth: lab = "Cloth"; break;
    case ModifierData::eModifierType_Collision: lab = "Collision"; break;
    case ModifierData::eModifierType_Bevel: lab = "Bevel"; break;
    case ModifierData::eModifierType_Shrinkwrap: lab = "Shrinkwrap"; break;
    case ModifierData::eModifierType_Fluidsim: lab = "Fluidsim"; break;
    case ModifierData::eModifierType_Mask: lab = "Mask"; break;
    case ModifierData::eModifierType_SimpleDeform: lab = "SimpleDeform"; break;
    case ModifierData::eModifierType_Multires: lab = "Multires"; break;
    case ModifierData::eModifierType_Smoke: lab = "Smoke"; break;
    case ModifierData::eModifierType_Solidify: lab = "Solidify"; break;
    case ModifierData::eModifierType_Ocean: lab = "Ocean"; break;
    case ModifierData::eModifierType_DynamicPaint: lab = "DynamicPaint"; break;
    case ModifierData::eModifierType_MeshCache: lab = "MeshCache"; break;
    case ModifierData::eModifierType_MeshSequenceCache: lab = "MeshSequenceCache"; break;
    case ModifierData::eModifierType_CorrectiveSmooth: lab = "CorrectiveSmooth"; break;
    case ModifierData::eModifierType_SurfaceDeform: lab = "SurfaceDeform"; break;
    case ModifierData::eModifierType_WeightedNormal: lab = "WeightedNormal"; break;
    case ModifierData::eModifierType_LaplacianDeform: lab = "LaplacianDeform"; break;
    case ModifierData::eModifierType_DataTransfer: lab = "DataTransfer"; break;
    case ModifierData::eModifierType_NormalEdit: lab = "NormalEdit"; break;
    case ModifierData::eModifierType_Fluid: lab = "Fluid"; break;
    case ModifierData::eModifierType_Nodes: lab = "Nodes"; break;
    default: break;
    }
    if (lab != nullptr) {
        std::strncpy(out, lab, 31);
        out[31] = '\0';
        return;
    }
    std::snprintf(out, 32, "type%d", type);
}

/**
 * Classify DNA modifier for Host: applied (baked into aiMesh), deferred (Physics bag),
 * or unreadable (skipped[] only - no invent).
 */
static void BlendClassifyModifierEmit(int type, const char *modName, const char *&emitClass,
        const char *&bagKey, bool &gnBlastUnreadable) {
    using namespace Assimp::Blender;
    emitClass = "unreadable";
    bagKey = nullptr;
    gnBlastUnreadable = false;
    if (type == ModifierData::eModifierType_Mirror || type == ModifierData::eModifierType_Subsurf) {
        emitClass = "applied";
        return;
    }
    switch (type) {
    case ModifierData::eModifierType_Softbody:
        emitClass = "deferred";
        bagKey = "softbody";
        return;
    case ModifierData::eModifierType_Cloth:
        emitClass = "deferred";
        bagKey = "cloth";
        return;
    case ModifierData::eModifierType_Fluidsim:
    case ModifierData::eModifierType_Fluid:
        emitClass = "deferred";
        bagKey = "fluid";
        return;
    case ModifierData::eModifierType_Smoke:
        emitClass = "deferred";
        bagKey = "smoke";
        return;
    case ModifierData::eModifierType_ParticleSystem:
        emitClass = "deferred";
        bagKey = "particles";
        return;
    case ModifierData::eModifierType_Explode:
        emitClass = "deferred";
        bagKey = "blast";
        return;
    case ModifierData::eModifierType_Ocean:
        emitClass = "deferred";
        bagKey = "ocean";
        return;
    case ModifierData::eModifierType_DynamicPaint:
        emitClass = "deferred";
        bagKey = "dynamic_paint";
        return;
    case ModifierData::eModifierType_Solidify:
        emitClass = "deferred";
        bagKey = "solidify";
        return;
    case ModifierData::eModifierType_MeshCache:
        emitClass = "deferred";
        bagKey = "mesh_cache";
        return;
    case ModifierData::eModifierType_MeshSequenceCache:
        emitClass = "deferred";
        bagKey = "mesh_sequence_cache";
        return;
    case ModifierData::eModifierType_Collision:
        emitClass = "deferred";
        bagKey = "collision";
        return;
    case ModifierData::eModifierType_Curve:
        emitClass = "deferred";
        bagKey = "curve";
        return;
    case ModifierData::eModifierType_Nodes: {
        // Name heuristic only (same spirit as CollectBlendPhysicsModifiers) - no Depsgraph.
        std::string hay = modName ? modName : "";
        for (char &c : hay) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (hay.find("fracture") != std::string::npos || hay.find("blast") != std::string::npos ||
                hay.find("break") != std::string::npos) {
            emitClass = "unreadable";
            gnBlastUnreadable = true;
            return;
        }
        if (hay.find("cloth") != std::string::npos) {
            emitClass = "deferred";
            bagKey = "cloth";
            return;
        }
        if (hay.find("soft") != std::string::npos) {
            emitClass = "deferred";
            bagKey = "softbody";
            return;
        }
        if (hay.find("fluid") != std::string::npos || hay.find("liquid") != std::string::npos ||
                hay.find("smoke") != std::string::npos) {
            emitClass = "deferred";
            bagKey = "fluid";
            return;
        }
        if (hay.find("particle") != std::string::npos || hay.find("hair") != std::string::npos) {
            emitClass = "deferred";
            bagKey = "particles";
            return;
        }
        emitClass = "unreadable";
        return;
    }
    default:
        return;
    }
}

/**
 * Owner Presence on typed aiNode: which DNA modifiers exist and how they bind.
 * No Modifier:* children (Host 0af). Deferred detail stays in Physics bags.
 */
static void BlendEmitModifierStackOnNode(aiNode *node, const Assimp::Blender::Object *obj,
        Assimp::Blender::ConversionData &conv) {
    using namespace Assimp::Blender;
    if (node == nullptr || obj == nullptr) {
        return;
    }
    const char *objName = obj->id.name + 2;
    std::ostringstream stack;
    int count = 0;
    const SharedModifierData *cur = static_cast<const SharedModifierData *>(obj->modifiers.first.get());
    for (; cur; cur = static_cast<const SharedModifierData *>(cur->modifier.next.get()), ++count) {
        const ModifierData &dat = cur->modifier;
        const char *emitClass = "unreadable";
        const char *bagKey = nullptr;
        bool gnBlast = false;
        BlendClassifyModifierEmit(dat.type, dat.name, emitClass, bagKey, gnBlast);
        char typeLab[32];
        BlendModifierTypeLabel(dat.type, typeLab);
        if (count) {
            stack << ';';
        }
        // slot:name:typeLabel:class[:bag]
        stack << count << ':' << dat.name << ':' << typeLab << ':' << emitClass;
        if (bagKey != nullptr) {
            stack << ':' << bagKey;
        }
        if (std::strcmp(emitClass, "unreadable") == 0 && !gnBlast) {
            // Specific blast GN token is emitted in CollectBlendPhysicsModifiers.
            std::string sk = "BLEND_modifier_unreadable:";
            sk += objName ? objName : "?";
            sk += '/';
            sk += dat.name[0] ? dat.name : typeLab;
            sk += ':';
            sk += typeLab;
            conv.skippedCategories.push_back(sk);
        }
    }
    if (count == 0) {
        return;
    }
    if (node->mMetaData == nullptr) {
        node->mMetaData = new aiMetadata();
    }
    node->mMetaData->Add("$mod.blend.count", count);
    node->mMetaData->Add("$mod.blend.stack", aiString(stack.str()));
    // Assimp-usual mesh bind: node owns mesh indices (same as sidecar meshes[]).
    if (node->mNumMeshes > 0) {
        std::ostringstream meshes;
        for (unsigned i = 0; i < node->mNumMeshes; ++i) {
            if (i) {
                meshes << ',';
            }
            meshes << node->mMeshes[i];
        }
        node->mMetaData->Add("$mod.blend.meshes", aiString(meshes.str()));
    }
}

void CollectBlendPhysicsModifiers(const Assimp::Blender::Object *obj, Assimp::Blender::ConversionData &conv,
        const aiNode *node) {
    using namespace Assimp::Blender;
    if (obj == nullptr) {
        return;
    }
    const char *objName = obj->id.name + 2;

    // Soft Body is commonly Object.soft (not Softbody modifier).
    if (obj->soft) {
        const SoftBody &sb = *obj->soft;
        std::ostringstream softJson;
        softJson << "{\"name\":\"" << BlendJsonEscape(objName)
                 << "\",\"type\":\"softbody\",\"settings\":{"
                 << "\"source\":\"Object.soft\""
                 << ",\"nodemass\":" << sb.nodemass
                 << ",\"grav\":" << sb.grav
                 << ",\"mediafrict\":" << sb.mediafrict
                 << ",\"goalspring\":" << sb.goalspring
                 << ",\"goalfrict\":" << sb.goalfrict
                 << ",\"mingoal\":" << sb.mingoal
                 << ",\"maxgoal\":" << sb.maxgoal
                 << ",\"defgoal\":" << sb.defgoal
                 << ",\"inspring\":" << sb.inspring
                 << ",\"infrict\":" << sb.infrict
                 << "}}";
        BlendPushPhysicsInv(conv.physicsSoftNodes, objName, softJson.str());
    }

    // Collision / Force Field authored on Object.pd (PartDeflect) - Presence only.
    if (obj->pd && (obj->pd->deflect != 0 || obj->pd->forcefield != 0)) {
        const PartDeflect &pd = *obj->pd;
        std::ostringstream colJson;
        colJson << "{\"name\":\"" << BlendJsonEscape(objName)
                << "\",\"type\":\"collision\",\"settings\":{"
                << "\"source\":\"Object.pd\""
                << ",\"deflect\":" << static_cast<int>(pd.deflect)
                << ",\"forcefield\":" << static_cast<int>(pd.forcefield)
                << ",\"flag\":" << pd.flag
                << ",\"falloff\":" << static_cast<int>(pd.falloff)
                << ",\"shape\":" << static_cast<int>(pd.shape)
                << ",\"f_strength\":" << pd.f_strength;
        {
            const char *ffKind = nullptr;
            switch (pd.forcefield) {
            case PartDeflect::Field_Force: ffKind = "force"; break;
            case PartDeflect::Field_Vortex: ffKind = "vortex"; break;
            case PartDeflect::Field_Magnetic: ffKind = "magnetic"; break;
            case PartDeflect::Field_Wind: ffKind = "wind"; break;
            case PartDeflect::Field_Guide: ffKind = "guide"; break;
            case PartDeflect::Field_Texture: ffKind = "texture"; break;
            case PartDeflect::Field_Harmonic: ffKind = "harmonic"; break;
            case PartDeflect::Field_Charge: ffKind = "charge"; break;
            case PartDeflect::Field_LennardJones: ffKind = "lennardjones"; break;
            case PartDeflect::Field_Boid: ffKind = "boid"; break;
            case PartDeflect::Field_Turbulence: ffKind = "turbulence"; break;
            case PartDeflect::Field_Drag: ffKind = "drag"; break;
            case PartDeflect::Field_FluidFlow: ffKind = "fluidflow"; break;
            default: break;
            }
            if (ffKind != nullptr) {
                colJson << ",\"forcefield_kind\":\"" << ffKind << '"';
            }
        }
        colJson << ",\"pdef_damp\":" << pd.pdef_damp
                << ",\"pdef_rdamp\":" << pd.pdef_rdamp
                << ",\"pdef_perm\":" << pd.pdef_perm
                << ",\"pdef_frict\":" << pd.pdef_frict
                << ",\"pdef_rfrict\":" << pd.pdef_rfrict
                << ",\"pdef_stickness\":" << pd.pdef_stickness
                << ",\"pdef_sbdamp\":" << pd.pdef_sbdamp
                << ",\"pdef_sbift\":" << pd.pdef_sbift
                << ",\"pdef_sboft\":" << pd.pdef_sboft
                << ",\"pdef_cfrict\":" << pd.pdef_cfrict
                << "}}";
        BlendPushPhysicsInv(conv.physicsCollisionNodes, objName, colJson.str());
    }

    // Rigid Body DNA -> BLEND bag + KHR-shaped nodes for Host Bullet/PhysX.
    if (obj->rigidbody_object) {
        const RigidBodyOb &rb = *obj->rigidbody_object;
        const char *motionType = "dynamic";
        if (rb.type == RigidBodyOb::Type_Passive) {
            motionType = "static";
        }
        if (rb.flag & RigidBodyOb::Flag_Kinematic) {
            motionType = "kinematic";
        }
        // Passive children under a parent motion root: collider-only so Host compounds.
        const bool colliderOnly = (rb.type == RigidBodyOb::Type_Passive) && (obj->parent != nullptr) &&
                                  ((rb.flag & RigidBodyOb::Flag_Kinematic) == 0);
        const char *shapeName = BlendRigidShapeName(rb.shape);
        std::ostringstream rigidJson;
        rigidJson << "{\"name\":\"" << BlendJsonEscape(objName)
                  << "\",\"type\":\"rigidbody\",\"settings\":{"
                  << "\"source\":\"Object.rigidbody_object\""
                  << ",\"rbo_type\":" << rb.type
                  << ",\"shape\":\"" << shapeName << '"'
                  << ",\"shape_id\":" << rb.shape
                  << ",\"flag\":" << rb.flag
                  << ",\"col_groups\":" << rb.col_groups
                  << ",\"mass\":" << rb.mass
                  << ",\"friction\":" << rb.friction
                  << ",\"restitution\":" << rb.restitution
                  << ",\"margin\":" << rb.margin
                  << ",\"lin_damping\":" << rb.lin_damping
                  << ",\"ang_damping\":" << rb.ang_damping
                  << ",\"motion\":\"" << motionType << '"'
                  << ",\"collider_only\":" << (colliderOnly ? "true" : "false")
                  << "}}";
        BlendPushPhysicsInv(conv.physicsRigidNodes, objName, rigidJson.str());

        // Mesh-based shapes: Host needs geometry.mesh (+ soup), not an empty implicit hull.
        const bool meshShape = (rb.shape == RigidBodyOb::Shape_Trimesh) ||
                               (rb.shape == RigidBodyOb::Shape_ConvexH);
        int meshIdx = -1;
        if (meshShape && node != nullptr && node->mNumMeshes > 0) {
            meshIdx = static_cast<int>(node->mMeshes[0]);
        }

        int shapeIdx = -1;
        if (!meshShape) {
            if (!conv.khrShapeFirst) {
                conv.khrShapes << ',';
            }
            conv.khrShapeFirst = false;
            shapeIdx = conv.khrShapeCount++;
            conv.khrShapes << "{\"type\":\"" << shapeName << "\"}";
        }

        if (!conv.khrMatFirst) {
            conv.khrMaterials << ',';
        }
        conv.khrMatFirst = false;
        const int matIdx = conv.khrMatCount++;
        conv.khrMaterials << "{\"staticFriction\":" << rb.friction
                          << ",\"dynamicFriction\":" << rb.friction
                          << ",\"restitution\":" << rb.restitution << '}';

        ConversionData::KhrBodyPending body;
        body.name = objName ? objName : "";
        // RBO_FLAG_DISABLED: not dynamically simulated -> KHR trigger volume.
        body.isTrigger = (rb.flag & RigidBodyOb::Flag_Disabled) != 0;
        body.motionType = (body.isTrigger || colliderOnly) ? std::string() : std::string(motionType);
        body.mass = rb.mass;
        body.shapeIdx = shapeIdx;
        body.matIdx = matIdx;
        body.colGroups = rb.col_groups;
        body.meshIdx = meshIdx;
        body.convexHull = (rb.shape == RigidBodyOb::Shape_ConvexH);
        // Disabled + no shape/mesh: still need a shape for Host trigger geometry.
        if (body.isTrigger && body.meshIdx < 0 && body.shapeIdx < 0) {
            if (!conv.khrShapeFirst) {
                conv.khrShapes << ',';
            }
            conv.khrShapeFirst = false;
            body.shapeIdx = conv.khrShapeCount++;
            conv.khrShapes << "{\"type\":\"" << shapeName << "\"}";
        }
        conv.khrBodies.push_back(std::move(body));
    }

    if (obj->rigidbody_constraint) {
        const RigidBodyCon &jc = *obj->rigidbody_constraint;
        std::ostringstream jointJson;
        jointJson << "{\"name\":\"" << BlendJsonEscape(objName)
                  << "\",\"type\":\"rigidbody_constraint\",\"settings\":{"
                  << "\"source\":\"Object.rigidbody_constraint\""
                  << ",\"constraint_type\":" << jc.type
                  << ",\"flag\":" << jc.flag
                  << ",\"breaking_threshold\":" << jc.breaking_threshold;
        if (jc.ob1 && jc.ob1->id.name[0] != '\0') {
            jointJson << ",\"object1\":\"" << BlendJsonEscape(jc.ob1->id.name + 2) << '"';
        }
        if (jc.ob2 && jc.ob2->id.name[0] != '\0') {
            jointJson << ",\"object2\":\"" << BlendJsonEscape(jc.ob2->id.name + 2) << '"';
        }
        jointJson << "}}";
        BlendPushPhysicsInv(conv.physicsJointNodes, objName, jointJson.str());

        ConversionData::KhrJointPending jp;
        jp.constraintName = objName ? objName : "";
        if (jc.ob1 && jc.ob1->id.name[0] != '\0') {
            jp.ob1Name = jc.ob1->id.name + 2;
        }
        if (jc.ob2 && jc.ob2->id.name[0] != '\0') {
            jp.ob2Name = jc.ob2->id.name + 2;
        }
        jp.type = jc.type;
        jp.flag = jc.flag;
        jp.breaking_threshold = jc.breaking_threshold;
        jp.limit_lin_x_lower = jc.limit_lin_x_lower;
        jp.limit_lin_x_upper = jc.limit_lin_x_upper;
        jp.limit_lin_y_lower = jc.limit_lin_y_lower;
        jp.limit_lin_y_upper = jc.limit_lin_y_upper;
        jp.limit_lin_z_lower = jc.limit_lin_z_lower;
        jp.limit_lin_z_upper = jc.limit_lin_z_upper;
        jp.limit_ang_x_lower = jc.limit_ang_x_lower;
        jp.limit_ang_x_upper = jc.limit_ang_x_upper;
        jp.limit_ang_y_lower = jc.limit_ang_y_lower;
        jp.limit_ang_y_upper = jc.limit_ang_y_upper;
        jp.limit_ang_z_lower = jc.limit_ang_z_lower;
        jp.limit_ang_z_upper = jc.limit_ang_z_upper;
        jp.spring_stiffness_x = jc.spring_stiffness_x;
        jp.spring_stiffness_y = jc.spring_stiffness_y;
        jp.spring_stiffness_z = jc.spring_stiffness_z;
        jp.spring_stiffness_ang_x = jc.spring_stiffness_ang_x;
        jp.spring_stiffness_ang_y = jc.spring_stiffness_ang_y;
        jp.spring_stiffness_ang_z = jc.spring_stiffness_ang_z;
        jp.spring_damping_x = jc.spring_damping_x;
        jp.spring_damping_y = jc.spring_damping_y;
        jp.spring_damping_z = jc.spring_damping_z;
        jp.spring_damping_ang_x = jc.spring_damping_ang_x;
        jp.spring_damping_ang_y = jc.spring_damping_ang_y;
        jp.spring_damping_ang_z = jc.spring_damping_ang_z;
        jp.motor_lin_target_velocity = jc.motor_lin_target_velocity;
        jp.motor_ang_target_velocity = jc.motor_ang_target_velocity;
        jp.motor_lin_max_impulse = jc.motor_lin_max_impulse;
        jp.motor_ang_max_impulse = jc.motor_ang_max_impulse;
        conv.khrJoints.push_back(std::move(jp));
    }

    // Particle systems (Object.particlesystem ListBase)
    for (const ParticleSystem *ps = static_cast<const ParticleSystem *>(obj->particlesystem.first.get());
            ps != nullptr;
            ps = static_cast<const ParticleSystem *>(ps->next.get())) {
        const char *psName = (ps->name[0] != '\0') ? ps->name : objName;
        std::ostringstream partJson;
        partJson << "{\"name\":\"" << BlendJsonEscape(objName)
                 << "\",\"system\":\"" << BlendJsonEscape(psName)
                 << "\",\"type\":\"particles\",\"settings\":{";
        if (ps->part) {
            const ParticleSettings &p = *ps->part;
            partJson << "\"part_type\":" << p.type
                     << ",\"totpart\":" << p.totpart
                     << ",\"mass\":" << p.mass
                     << ",\"size\":" << p.size
                     << ",\"randsize\":" << p.randsize
                     << ",\"ren_as\":" << p.ren_as
                     << ",\"from\":" << p.from
                     << ",\"distr\":" << p.distr
                     << ",\"lifetime\":" << p.lifetime
                     << ",\"frame_start\":" << p.sta
                     << ",\"frame_end\":" << p.end
                     << ",\"seed\":" << ps->seed
                     << ",\"jitfac\":" << p.jitfac
                     << ",\"flag\":" << p.flag;
            // Presence: DNA eParticleType -> Host label (hair / fluid secondary / emitter).
            {
                const char *partKind = nullptr;
                switch (p.type) {
                case ParticleSettings::Type_Emitter: partKind = "emitter"; break;
                case ParticleSettings::Type_Hair: partKind = "hair"; break;
                case ParticleSettings::Type_Fluid: partKind = "fluid"; break;
                case ParticleSettings::Type_FluidFlip: partKind = "fluid_flip"; break;
                case ParticleSettings::Type_FluidSpray: partKind = "fluid_spray"; break;
                case ParticleSettings::Type_FluidBubble: partKind = "fluid_bubble"; break;
                case ParticleSettings::Type_FluidFoam: partKind = "fluid_foam"; break;
                case ParticleSettings::Type_FluidTracer: partKind = "fluid_tracer"; break;
                case ParticleSettings::Type_FluidSprayFoam: partKind = "fluid_sprayfoam"; break;
                case ParticleSettings::Type_FluidSprayBubble: partKind = "fluid_spraybubble"; break;
                case ParticleSettings::Type_FluidFoamBubble: partKind = "fluid_foambubble"; break;
                case ParticleSettings::Type_FluidSprayFoamBubble: partKind = "fluid_sprayfoambubble"; break;
                default: break;
                }
                if (partKind != nullptr) {
                    partJson << ",\"part_kind\":\"" << partKind << '"';
                }
            }
            if (p.ren_as == ParticleSettings::Draw_Path) {
                partJson << ",\"ren_as_kind\":\"path\"";
            } else if (p.ren_as == ParticleSettings::Draw_Object) {
                partJson << ",\"ren_as_kind\":\"object\"";
            } else if (p.ren_as == ParticleSettings::Draw_Collection) {
                partJson << ",\"ren_as_kind\":\"collection\"";
            } else if (p.ren_as == ParticleSettings::Draw_Halo) {
                partJson << ",\"ren_as_kind\":\"halo\"";
            } else if (p.ren_as == ParticleSettings::Draw_Line) {
                partJson << ",\"ren_as_kind\":\"line\"";
            } else if (p.ren_as == ParticleSettings::Draw_None) {
                partJson << ",\"ren_as_kind\":\"none\"";
            }
            if (p.hasPhystype) {
                partJson << ",\"phystype\":" << p.phystype;
                const char *physKind = nullptr;
                switch (p.phystype) {
                case ParticleSettings::Phys_No: physKind = "no"; break;
                case ParticleSettings::Phys_Newtonian: physKind = "newtonian"; break;
                case ParticleSettings::Phys_Keyed: physKind = "keyed"; break;
                case ParticleSettings::Phys_Boids: physKind = "boids"; break;
                case ParticleSettings::Phys_Fluid: physKind = "fluid"; break;
                default: break;
                }
                if (physKind != nullptr) {
                    partJson << ",\"phys_kind\":\"" << physKind << '"';
                }
            }
            if (p.hasDrawAs) {
                partJson << ",\"draw_as\":" << p.draw_as;
            }
            if (p.hasDraw) {
                partJson << ",\"draw\":" << p.draw;
            }
            if (p.hasUserjit) {
                partJson << ",\"userjit\":" << p.userjit;
            }
            if (p.hasGridRes) {
                partJson << ",\"grid_res\":" << p.grid_res;
            }
            if (p.hasGridRand) {
                partJson << ",\"grid_rand\":" << p.grid_rand;
            }
            if (p.hasRandlife) {
                partJson << ",\"randlife\":" << p.randlife;
            }
            if (p.hasRandlength) {
                partJson << ",\"randlength\":" << p.randlength;
            }
            if (p.hasHairStep) {
                partJson << ",\"hair_step\":" << p.hair_step;
            }
            // Presence: RNA hair_length = DNA normfac * 4 (rna_PartSetting_hairlength_get).
            if (p.hasNormfac) {
                partJson << ",\"normfac\":" << p.normfac
                         << ",\"hair_length\":" << (p.normfac * 4.f);
            }
            if (p.hasPathStart) {
                partJson << ",\"path_start\":" << p.path_start;
            }
            if (p.hasPathEnd) {
                partJson << ",\"path_end\":" << p.path_end;
            }
            // Hair thickness DNA (Cycles: r=rad_scale*0.5, root/tip=r*rad_*). Not size.
            if (p.hasShape) {
                partJson << ",\"shape\":" << p.shape;
            }
            if (p.hasRadRoot) {
                partJson << ",\"rad_root\":" << p.rad_root;
            }
            if (p.hasRadTip) {
                partJson << ",\"rad_tip\":" << p.rad_tip;
            }
            if (p.hasRadScale) {
                partJson << ",\"rad_scale\":" << p.rad_scale;
            }
            if (p.hasShapeFlag) {
                partJson << ",\"shape_flag\":" << static_cast<int>(p.shape_flag);
                if (p.shape_flag & ParticleSettings::Shape_CloseTip) {
                    partJson << ",\"use_close_tip\":true";
                }
            }
            // 0al5: Child-Distribution DNA factors (Host expands after parents; no invent).
            if (p.hasChildtype) {
                partJson << ",\"childtype\":" << static_cast<int>(p.childtype);
                const char *childKind = nullptr;
                switch (p.childtype) {
                case ParticleSettings::Child_None: childKind = "none"; break;
                case ParticleSettings::Child_Particles: childKind = "particles"; break;
                case ParticleSettings::Child_Faces: childKind = "faces"; break;
                default: break;
                }
                if (childKind != nullptr) {
                    partJson << ",\"child_type\":\"" << childKind << '"';
                }
            }
            if (p.hasChildFlag) {
                partJson << ",\"child_flag\":" << p.child_flag;
                if (p.child_flag & ParticleSettings::Child_UseClumpNoise) {
                    partJson << ",\"use_clump_noise\":true";
                }
                if (p.child_flag & ParticleSettings::Child_UseClumpCurve) {
                    partJson << ",\"use_clump_curve\":true";
                }
                if (p.child_flag & ParticleSettings::Child_UseRoughCurve) {
                    partJson << ",\"use_rough_curve\":true";
                }
                if (p.child_flag & ParticleSettings::Child_UseTwistCurve) {
                    partJson << ",\"use_twist_curve\":true";
                }
            }
            // SDNA name honesty: emit only the count field that resolved.
            if (p.hasChildNbr) {
                partJson << ",\"child_nbr\":" << p.child_nbr;
            }
            if (p.hasRenChildNbr) {
                partJson << ",\"ren_child_nbr\":" << p.ren_child_nbr;
            }
            if (p.hasChildPercent) {
                partJson << ",\"child_percent\":" << p.child_percent;
            }
            if (p.hasChildRenderPercent) {
                partJson << ",\"child_render_percent\":" << p.child_render_percent;
            }
            if (p.hasChildSeed) {
                partJson << ",\"child_seed\":" << p.child_seed;
            }
            if (p.hasParents) {
                partJson << ",\"parents\":" << p.parents;
            }
            if (p.hasChildsize) {
                partJson << ",\"childsize\":" << p.childsize;
            }
            if (p.hasChildrandsize) {
                partJson << ",\"childrandsize\":" << p.childrandsize;
            }
            if (p.hasChildrad) {
                partJson << ",\"childrad\":" << p.childrad;
            }
            if (p.hasChildflat) {
                partJson << ",\"childflat\":" << p.childflat;
            }
            if (p.hasClumpfac) {
                partJson << ",\"clumpfac\":" << p.clumpfac;
            }
            if (p.hasClumppow) {
                partJson << ",\"clumppow\":" << p.clumppow;
            }
            if (p.hasKink) {
                partJson << ",\"kink\":" << static_cast<int>(p.kink);
                const char *kinkKind = nullptr;
                switch (p.kink) {
                case ParticleSettings::Kink_No: kinkKind = "none"; break;
                case ParticleSettings::Kink_Curl: kinkKind = "curl"; break;
                case ParticleSettings::Kink_Radial: kinkKind = "radial"; break;
                case ParticleSettings::Kink_Wave: kinkKind = "wave"; break;
                case ParticleSettings::Kink_Braid: kinkKind = "braid"; break;
                case ParticleSettings::Kink_Spiral: kinkKind = "spiral"; break;
                default: break;
                }
                if (kinkKind != nullptr) {
                    partJson << ",\"kink_kind\":\"" << kinkKind << '"';
                }
            }
            if (p.hasKinkAmp) {
                partJson << ",\"kink_amp\":" << p.kink_amp;
            }
            if (p.hasKinkFreq) {
                partJson << ",\"kink_freq\":" << p.kink_freq;
            }
            if (p.hasKinkShape) {
                partJson << ",\"kink_shape\":" << p.kink_shape;
            }
            if (p.hasKinkFlat) {
                partJson << ",\"kink_flat\":" << p.kink_flat;
            }
            if (p.hasKinkAmpClump) {
                partJson << ",\"kink_amp_clump\":" << p.kink_amp_clump;
            }
            if (p.hasKinkExtraSteps) {
                partJson << ",\"kink_extra_steps\":" << p.kink_extra_steps;
            }
            if (p.hasKinkAxisRandom) {
                partJson << ",\"kink_axis_random\":" << p.kink_axis_random;
            }
            if (p.hasKinkAmpRandom) {
                partJson << ",\"kink_amp_random\":" << p.kink_amp_random;
            }
            if (p.hasPartingFac) {
                partJson << ",\"parting_fac\":" << p.parting_fac;
            }
            if (p.hasPartingMin) {
                partJson << ",\"parting_min\":" << p.parting_min;
            }
            if (p.hasPartingMax) {
                partJson << ",\"parting_max\":" << p.parting_max;
            }
            if (p.hasBranchThres) {
                partJson << ",\"branch_thres\":" << p.branch_thres;
            }
            if (p.hasClength) {
                partJson << ",\"clength\":" << p.clength;
            }
            if (p.hasClengthThres) {
                partJson << ",\"clength_thres\":" << p.clength_thres;
            }
            // ParticleSettings.flag child bits - emit true only when bit set (no false invent).
            if ((p.flag & ParticleSettings::Flag_ChildEffect) != 0) {
                partJson << ",\"child_effect\":true";
            }
            if ((p.flag & ParticleSettings::Flag_ChildLongHair) != 0) {
                partJson << ",\"child_long_hair\":true";
            }
            if ((p.flag & ParticleSettings::Flag_ChildGuide) != 0) {
                partJson << ",\"child_guide\":true";
            }
            // C14: DNA HairKey paths when ParticleData.hair[] was written (no invent / no Depsgraph).
            {
                size_t strandWithKeys = 0;
                size_t totalKeys = 0;
                for (size_t pi = 0; pi < ps->particles.size(); ++pi) {
                    const size_t nk = ps->particles[pi].hair.size();
                    if (nk == 0) {
                        continue;
                    }
                    ++strandWithKeys;
                    totalKeys += nk;
                }
                if (strandWithKeys > 0 && totalKeys > 0) {
                    partJson << ",\"hairPaths\":{\"source\":\"ParticleData.hair\""
                             << ",\"arraysResolved\":true"
                             << ",\"positionsSpace\":\"emitterLocal\""
                             << ",\"strandCount\":" << strandWithKeys
                             << ",\"keyCount\":" << totalKeys
                             << ",\"keyCounts\":[";
                    bool firstKc = true;
                    for (size_t pi = 0; pi < ps->particles.size(); ++pi) {
                        const size_t nk = ps->particles[pi].hair.size();
                        if (nk == 0) {
                            continue;
                        }
                        if (!firstKc) {
                            partJson << ',';
                        }
                        firstKc = false;
                        partJson << nk;
                    }
                    partJson << "],\"positions\":[";
                    bool firstPos = true;
                    bool anyTime = false;
                    bool anyWorld = false;
                    for (size_t pi = 0; pi < ps->particles.size(); ++pi) {
                        const ParticleData &pd = ps->particles[pi];
                        for (size_t ki = 0; ki < pd.hair.size(); ++ki) {
                            if (!firstPos) {
                                partJson << ',';
                            }
                            firstPos = false;
                            const HairKey &hk = pd.hair[ki];
                            partJson << hk.co[0] << ',' << hk.co[1] << ',' << hk.co[2];
                            anyTime = true;
                            if (hk.hasWorldCo) {
                                anyWorld = true;
                            }
                        }
                    }
                    partJson << ']';
                    // times[] always parallel to positions when any HairKey was converted.
                    if (anyTime) {
                        partJson << ",\"times\":[";
                        bool firstT = true;
                        for (size_t pi = 0; pi < ps->particles.size(); ++pi) {
                            const ParticleData &pd = ps->particles[pi];
                            for (size_t ki = 0; ki < pd.hair.size(); ++ki) {
                                if (!firstT) {
                                    partJson << ',';
                                }
                                firstT = false;
                                partJson << pd.hair[ki].time;
                            }
                        }
                        partJson << ']';
                    }
                    // world_co only when every emitted key has SDNA world_co (parallel array).
                    if (anyWorld) {
                        bool allWorld = true;
                        for (size_t pi = 0; pi < ps->particles.size() && allWorld; ++pi) {
                            const ParticleData &pd = ps->particles[pi];
                            for (size_t ki = 0; ki < pd.hair.size(); ++ki) {
                                if (!pd.hair[ki].hasWorldCo) {
                                    allWorld = false;
                                    break;
                                }
                            }
                        }
                        if (allWorld) {
                            partJson << ",\"worldPositions\":[";
                            bool firstW = true;
                            for (size_t pi = 0; pi < ps->particles.size(); ++pi) {
                                const ParticleData &pd = ps->particles[pi];
                                for (size_t ki = 0; ki < pd.hair.size(); ++ki) {
                                    if (!firstW) {
                                        partJson << ',';
                                    }
                                    firstW = false;
                                    const HairKey &hk = pd.hair[ki];
                                    partJson << hk.world_co[0] << ',' << hk.world_co[1] << ','
                                             << hk.world_co[2];
                                }
                            }
                            partJson << "],\"worldPositionsSpace\":\"world\"";
                        }
                    }
                    // 0al4: birth face index per strand (DNA ParticleData.num) when resolved.
                    {
                        bool anyBirth = false;
                        for (size_t pi = 0; pi < ps->particles.size(); ++pi) {
                            if (!ps->particles[pi].hair.empty() && ps->particles[pi].hasNum &&
                                    ps->particles[pi].num >= 0) {
                                anyBirth = true;
                                break;
                            }
                        }
                        if (anyBirth) {
                            partJson << ",\"birthFace\":[";
                            bool firstB = true;
                            for (size_t pi = 0; pi < ps->particles.size(); ++pi) {
                                const ParticleData &pd = ps->particles[pi];
                                if (pd.hair.empty()) {
                                    continue;
                                }
                                if (!firstB) {
                                    partJson << ',';
                                }
                                firstB = false;
                                partJson << (pd.hasNum ? pd.num : -1);
                            }
                            partJson << ']';
                        }
                    }
                    partJson << '}';
                } else if (p.type == ParticleSettings::Type_Hair) {
                    // Honesty: hair typed but no HairKey DATA in file (runtime/pathcache only).
                    partJson << ",\"hairPathsResolved\":false";
                    conv.skippedCategories.push_back(
                            std::string("BLEND_C14_hair_paths_unresolved:") +
                            (objName ? objName : "?") + "/" + (psName ? psName : "?"));
                }
            }
            if (p.id.name[0] != '\0') {
                partJson << ",\"settings_name\":\"" << BlendJsonEscape(p.id.name + 2) << '"';
            }
            // 0al4: PS vertex groups + density face mask from DNA dvert (no invent).
            {
                const Mesh *emesh = (obj->type == Object::Type_MESH && obj->data) ?
                                            static_cast<const Mesh *>(obj->data.get()) :
                                            nullptr;
                std::vector<std::string> vgNames;
                BlendCollectDeformGroupNames(obj, emesh, vgNames);
                if (ps->hasVgroup) {
                    bool vgFirst = true;
                    for (int si = 0; si < ParticleSystem::VG_Tot; ++si) {
                        const short idx1 = ps->vgroup[si];
                        if (idx1 <= 0) {
                            continue;
                        }
                        const int defNr = static_cast<int>(idx1) - 1;
                        if (defNr < 0 || static_cast<size_t>(defNr) >= vgNames.size()) {
                            continue;
                        }
                        if (vgNames[static_cast<size_t>(defNr)].empty()) {
                            continue;
                        }
                        const char *slot = BlendParticleVgSlotName(si);
                        if (slot == nullptr) {
                            continue;
                        }
                        if (vgFirst) {
                            partJson << ",\"vertexGroups\":{";
                            vgFirst = false;
                        } else {
                            partJson << ',';
                        }
                        partJson << '"' << slot << "\":\""
                                 << BlendJsonEscape(vgNames[static_cast<size_t>(defNr)]) << '"';
                        if (ps->hasVgNeg && (ps->vg_neg & (1 << si)) != 0) {
                            partJson << ",\"" << slot << "Neg\":true";
                        }
                    }
                    if (!vgFirst) {
                        partJson << '}';
                    }
                    // Density VG -> faceMask / faceDensity (Host skip cavity strands).
                    if (emesh != nullptr && ps->vgroup[ParticleSystem::VG_Density] > 0) {
                        const int defNr = static_cast<int>(ps->vgroup[ParticleSystem::VG_Density]) - 1;
                        const bool invert = ps->hasVgNeg &&
                                            (ps->vg_neg & (1 << ParticleSystem::VG_Density)) != 0;
                        std::vector<int> faceMask;
                        std::vector<float> faceDensity;
                        if (BlendFaceDensityFromVgroup(*emesh, defNr, invert, faceMask, faceDensity)) {
                            partJson << ",\"faceMask\":[";
                            for (size_t i = 0; i < faceMask.size(); ++i) {
                                if (i) {
                                    partJson << ',';
                                }
                                partJson << faceMask[i];
                            }
                            partJson << "],\"faceDensity\":[";
                            for (size_t i = 0; i < faceDensity.size(); ++i) {
                                if (i) {
                                    partJson << ',';
                                }
                                partJson << faceDensity[i];
                            }
                            partJson << "],\"faceMaskSource\":\"vertex_group_density\"";
                        }
                    }
                }
            }
            // 0al4: Particle MTex maps -> typed material (after BuildMaterials).
            // Sidecar textures[] decommissioned - Presence typedMaterial = settings DNA name.
            if (p.id.name[2] != '\0') {
                bool anyMtex = false;
                for (size_t ti = 0; ti < sizeof(p.mtex) / sizeof(p.mtex[0]); ++ti) {
                    if (!p.mtex[ti] || !p.mtex[ti]->tex || !p.mtex[ti]->tex->ima) {
                        continue;
                    }
                    aiString embedName;
                    if (!BlendEnsureEmbeddedImage(p.mtex[ti]->tex->ima.get(), conv, embedName)) {
                        continue;
                    }
                    anyMtex = true;
                    break;
                }
                if (anyMtex) {
                    const std::string settingsName = p.id.name + 2;
                    partJson << ",\"typedMaterial\":\"" << BlendJsonEscape(settingsName) << '"';
                    bool queued = false;
                    for (const ParticleSettings *queuedPart : conv.pendingParticleMtex) {
                        if (queuedPart == &p) {
                            queued = true;
                            break;
                        }
                    }
                    if (!queued) {
                        conv.pendingParticleMtex.push_back(&p);
                    }
                }
            }
        } else {
            partJson << "\"source\":\"ParticleSystem\"";
        }
        partJson << "}}";
        BlendPushPhysicsInv(conv.physicsParticleNodes, objName, partJson.str());

        // C13 PointInstancer inventory for collection/object render (Presence: TRS only if DNA particles[]).
        if (ps->part) {
            const ParticleSettings &p = *ps->part;
            if (p.ren_as == ParticleSettings::Draw_Collection || p.ren_as == ParticleSettings::Draw_Object) {
                ConversionData::PointInstancerPending pi;
                pi.emitterName = objName;
                pi.systemName = psName;
                if (p.id.name[0] != '\0') {
                    pi.settingsName = p.id.name + 2;
                }
                pi.ren_as = p.ren_as;
                pi.from = p.from;
                pi.distr = p.distr;
                pi.flag = p.flag;
                pi.totpart = p.totpart;
                pi.seed = ps->seed;
                pi.size = p.size;
                pi.randsize = p.randsize;
                pi.jitfac = p.jitfac;
                if (p.hasPhystype) {
                    pi.phystype = p.phystype;
                    pi.hasPhystype = true;
                }
                if (p.hasDrawAs) {
                    pi.draw_as = p.draw_as;
                    pi.hasDrawAs = true;
                }
                if (p.hasDraw) {
                    pi.draw = p.draw;
                    pi.hasDraw = true;
                }
                if (p.hasUserjit) {
                    pi.userjit = p.userjit;
                    pi.hasUserjit = true;
                }
                if (p.hasGridRes) {
                    pi.grid_res = p.grid_res;
                    pi.hasGridRes = true;
                }
                if (p.hasGridRand) {
                    pi.grid_rand = p.grid_rand;
                    pi.hasGridRand = true;
                }
                if (p.hasRandlife) {
                    pi.randlife = p.randlife;
                    pi.hasRandlife = true;
                }
                if (p.hasRandlength) {
                    pi.randlength = p.randlength;
                    pi.hasRandlength = true;
                }
                if (p.hasHairStep) {
                    pi.hair_step = p.hair_step;
                    pi.hasHairStep = true;
                }
                if (p.hasNormfac) {
                    pi.normfac = p.normfac;
                    pi.hasHairLength = true;
                }
                if (p.dup_group && p.dup_group->id.name[0] != '\0') {
                    pi.collectionName = p.dup_group->id.name + 2;
                }
                if (p.dup_ob && p.dup_ob->id.name[0] != '\0') {
                    pi.instanceObjectName = p.dup_ob->id.name + 2;
                }
                for (std::shared_ptr<ParticleDupliWeight> dw =
                                std::static_pointer_cast<ParticleDupliWeight>(p.dupliweights.first);
                        dw;
                        dw = std::static_pointer_cast<ParticleDupliWeight>(dw->next)) {
                    if (dw->ob && dw->ob->id.name[0] != '\0') {
                        pi.prototypeNames.emplace_back(dw->ob->id.name + 2);
                        pi.prototypeCounts.push_back(static_cast<int>(dw->count));
                        pi.prototypeIndices.push_back(static_cast<int>(dw->index));
                    }
                }
                if (pi.prototypeNames.empty() && !pi.instanceObjectName.empty()) {
                    pi.prototypeNames.push_back(pi.instanceObjectName);
                    pi.prototypeCounts.push_back(1);
                    pi.prototypeIndices.push_back(0);
                }
                // Instance arrays: DNA ParticleData when present; else mesh-face eval for FACE emit.
                if (!ps->particles.empty()) {
                    const size_t n = ps->particles.size();
                    pi.positions.reserve(n * 3);
                    pi.orients.reserve(n * 4);
                    pi.scales.reserve(n);
                    pi.protoIndices.reserve(n);
                    const size_t nproto = pi.prototypeNames.size();
                    for (size_t i = 0; i < n; ++i) {
                        const ParticleData &pd = ps->particles[i];
                        pi.positions.push_back(pd.state.co[0]);
                        pi.positions.push_back(pd.state.co[1]);
                        pi.positions.push_back(pd.state.co[2]);
                        pi.orients.push_back(pd.state.rot[0]);
                        pi.orients.push_back(pd.state.rot[1]);
                        pi.orients.push_back(pd.state.rot[2]);
                        pi.orients.push_back(pd.state.rot[3]);
                        const float sc = (pd.size > 0.f) ? pd.size : p.size;
                        pi.scales.push_back(sc);
                        if (nproto > 0) {
                            pi.protoIndices.push_back(static_cast<int>(i % nproto));
                        }
                    }
                    pi.arraysResolved = true;
                    pi.arraysSource = "dna_particles";
                    // Host 0ai: DNA ParticleData.co / size are emitter-local (pre parent scale).
                    pi.positionsSpace = "emitterLocal";
                    pi.scalesSpace = "emitterLocal";
                } else if (obj->type == Object::Type_MESH && obj->data) {
                    const Mesh *emesh = static_cast<const Mesh *>(obj->data.get());
                    ParticleFaceDistributeParams dp;
                    dp.totpart = p.totpart;
                    dp.seed = ps->seed;
                    dp.from = p.from;
                    dp.distr = p.distr;
                    dp.flag = p.flag;
                    dp.jitfac = p.jitfac;
                    dp.userjit = p.hasUserjit ? p.userjit : 0;
                    dp.size = p.size;
                    dp.randsize = p.randsize;
                    if (p.hasNormfac) {
                        dp.hairLength = p.normfac * 4.f;
                    }
                    dp.objectScale[0] = obj->scale[0];
                    dp.objectScale[1] = obj->scale[1];
                    dp.objectScale[2] = obj->scale[2];
                    dp.prototypeCounts = pi.prototypeCounts;
                    ParticleFaceDistributeResult dist;
                    if (BlendDistributeParticlesOnMeshFaces(*emesh, dp, dist)) {
                        pi.positions = std::move(dist.positions);
                        pi.orients = std::move(dist.orients);
                        pi.scales = std::move(dist.scales);
                        pi.protoIndices = std::move(dist.protoIndices);
                        pi.faceArea = std::move(dist.faceArea);
                        pi.emitWeight = std::move(dist.emitWeight);
                        pi.arraysResolved = true;
                        pi.arraysSource = "mesh_face_eval";
                        pi.totpart = static_cast<int>(pi.scales.size());
                        // Host 0ai: mesh-local positions; scales = size*(1-rand)*hair_length
                        // in emitter-local (Blender size*path_len), not worldUniform.
                        pi.positionsSpace = "emitterLocal";
                        pi.scalesSpace = "emitterLocal";
                        if (p.hasNormfac) {
                            pi.scalesMeaning = "size_times_hair_length";
                        } else {
                            pi.scalesMeaning = "size";
                        }
                    }
                }
                conv.pointInstancers.push_back(std::move(pi));
            }
        }
    }

    int stackSlot = 0;
    const SharedModifierData *cur = static_cast<const SharedModifierData *>(obj->modifiers.first.get());
    for (; cur; cur = static_cast<const SharedModifierData *>(cur->modifier.next.get()), ++stackSlot) {
        const ModifierData &dat = cur->modifier;
        std::vector<ConversionData::PhysicsInvNode> *bag = nullptr;
        const char *bagKey = nullptr;
        bool wantMeshMatBind = false;
        switch (dat.type) {
        case ModifierData::eModifierType_Softbody:
            bag = &conv.physicsSoftNodes;
            bagKey = "softbody";
            break;
        case ModifierData::eModifierType_Cloth:
            bag = &conv.physicsClothNodes;
            bagKey = "cloth";
            break;
        case ModifierData::eModifierType_Fluidsim:
        case ModifierData::eModifierType_Fluid:
            bag = &conv.physicsFluidNodes;
            bagKey = "fluid";
            wantMeshMatBind = true;
            break;
        case ModifierData::eModifierType_Smoke:
            bag = &conv.physicsSmokeNodes;
            bagKey = "smoke";
            wantMeshMatBind = true;
            break;
        case ModifierData::eModifierType_ParticleSystem:
            bag = &conv.physicsParticleNodes;
            bagKey = "particles";
            break;
        case ModifierData::eModifierType_Explode:
            bag = &conv.physicsBlastNodes;
            bagKey = "blast";
            wantMeshMatBind = true;
            break;
        case ModifierData::eModifierType_Ocean:
            bag = &conv.physicsOceanNodes;
            bagKey = "ocean";
            wantMeshMatBind = true;
            break;
        case ModifierData::eModifierType_DynamicPaint:
            bag = &conv.physicsDynamicPaintNodes;
            bagKey = "dynamic_paint";
            break;
        case ModifierData::eModifierType_Solidify:
            bag = &conv.physicsSolidifyNodes;
            bagKey = "solidify";
            break;
        case ModifierData::eModifierType_MeshCache:
            bag = &conv.meshCacheNodes;
            bagKey = "mesh_cache";
            wantMeshMatBind = true;
            break;
        case ModifierData::eModifierType_MeshSequenceCache:
            bag = &conv.meshCacheNodes;
            bagKey = "mesh_sequence_cache";
            wantMeshMatBind = true;
            break;
        case ModifierData::eModifierType_Collision:
            bag = &conv.physicsCollisionNodes;
            bagKey = "collision";
            wantMeshMatBind = true;
            break;
        case ModifierData::eModifierType_Curve:
            bag = &conv.physicsCurveNodes;
            bagKey = "curve";
            wantMeshMatBind = true;
            break;
        case ModifierData::eModifierType_Nodes: {

            // Blender 3+ often drives cloth/soft via Geometry Nodes - map by name heuristic.
            auto lowerContains = [](const char *a, const char *b) -> bool {
                if (a == nullptr || b == nullptr) {
                    return false;
                }
                std::string hay(a), needle(b);
                for (char &c : hay) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                for (char &c : needle) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                return hay.find(needle) != std::string::npos;
            };
            const bool hitCloth = lowerContains(dat.name, "cloth") || lowerContains(objName, "cloth");
            const bool hitSoft = lowerContains(dat.name, "soft") || lowerContains(objName, "soft") ||
                                 lowerContains(objName, "pudding") || lowerContains(objName, "jiggle");
            const bool hitFluid = lowerContains(dat.name, "fluid") || lowerContains(dat.name, "smoke") ||
                                  lowerContains(objName, "smoke") || lowerContains(objName, "fluid");
            const bool hitPart = lowerContains(dat.name, "particle") || lowerContains(objName, "particle") ||
                                 lowerContains(dat.name, "emitter") || lowerContains(objName, "emitter");
            // Do not specialize-convert NodesModifierData->bNodeTree here (heavy/unsafe on some files).
            // Honest skip when modifier/object names indicate fracture/blast (e.g. CubeCuts / Cut Fracturing).
            const bool hitBlast = lowerContains(dat.name, "fractur") || lowerContains(dat.name, "blast") ||
                                  lowerContains(dat.name, "shatter") || lowerContains(dat.name, "cut") ||
                                  lowerContains(objName, "fractur") || lowerContains(objName, "blast") ||
                                  lowerContains(objName, "shatter");
            if (hitCloth) {
                bag = &conv.physicsClothNodes;
                bagKey = "cloth";
            } else if (hitSoft) {
                bag = &conv.physicsSoftNodes;
                bagKey = "softbody";
            } else if (hitFluid) {
                bag = &conv.physicsFluidNodes;
                bagKey = "fluid";
                wantMeshMatBind = true;
            } else if (hitPart) {
                bag = &conv.physicsParticleNodes;
                bagKey = "particles";
            } else if (hitBlast) {
                // Geometry Nodes fracture is unevaluated - honest skip, no fake chunks.
                conv.skippedCategories.push_back(
                        std::string("BLEND_physics_blast_geometry_nodes_unevaluated:") +
                        (objName ? objName : "?") + "/" + dat.name);
            }
            break;
        }
        default:
            break;
        }
        if (bag == nullptr) {
            continue;
        }

        std::ostringstream settings;
        settings << "{\"mode\":" << dat.mode;
        if (dat.type == ModifierData::eModifierType_Nodes) {
            settings << ",\"source\":\"NodesModifierData\"";
            if (cur->dna_type != nullptr &&
                    BlendElemConvertedHullIs(*(cur), "NodesModifierData") &&
                    bagKey != nullptr && std::strcmp(bagKey, "particles") == 0) {
                BlendAppendNodesModifierParticlePresence(
                        settings, *static_cast<const NodesModifierData *>(cur));
            }
        }
        if (dat.type == ModifierData::eModifierType_Cloth &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "ClothModifierData")) {
            const auto *cloth = static_cast<const ClothModifierData *>(cur);
            if (cloth->sim_parms) {
                const ClothSimSettings &cs = *cloth->sim_parms;
                settings << ",\"mass\":" << cs.mass
                         << ",\"shear\":" << cs.shear
                         << ",\"bending\":" << cs.bending
                         << ",\"tension\":" << cs.tension
                         << ",\"goalspring\":" << cs.goalspring
                         << ",\"goalfrict\":" << cs.goalfrict
                         << ",\"stepsPerFrame\":" << cs.stepsPerFrame
                         << ",\"gravity\":[" << cs.gravity[0] << ',' << cs.gravity[1] << ',' << cs.gravity[2] << ']';
            }
        }
        if (dat.type == ModifierData::eModifierType_Explode) {
            settings << ",\"source\":\"ExplodeModifierData\"";
        }
        if (dat.type == ModifierData::eModifierType_Fluid &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "FluidModifierData")) {
            const auto *fluid = static_cast<const FluidModifierData *>(cur);
            const char *role = "unknown";
            if (fluid->type & FluidModifierData::MOD_FLUID_TYPE_DOMAIN) {
                role = "domain";
            } else if (fluid->type & FluidModifierData::MOD_FLUID_TYPE_FLOW) {
                role = "flow";
                if (fluid->flow) {
                    if (fluid->flow->behavior == FluidFlowSettings::Behavior_Inflow) {
                        role = "inflow";
                    } else if (fluid->flow->behavior == FluidFlowSettings::Behavior_Outflow) {
                        role = "outflow";
                    }
                }
            } else if (fluid->type & FluidModifierData::MOD_FLUID_TYPE_EFFEC) {
                role = "obstacle";
                if (fluid->effector) {
                    if (fluid->effector->type == FluidEffectorSettings::Type_Guide) {
                        role = "guide";
                    } else if (fluid->effector->type == FluidEffectorSettings::Type_Collision) {
                        role = "collision";
                    }
                }
            }
            settings << ",\"fluid_role\":\"" << role << "\"";
            if (fluid->domain) {
                if (fluid->domain->type == FluidDomainSettings::Domain_Gas) {
                    settings << ",\"domain_type\":\"gas\"";
                } else if (fluid->domain->type == FluidDomainSettings::Domain_Liquid) {
                    settings << ",\"domain_type\":\"liquid\"";
                }
                settings << ",\"cache\":{\"directory\":\"" << BlendJsonEscape(fluid->domain->cache_directory)
                         << "\",\"frame_start\":" << fluid->domain->cache_frame_start
                         << ",\"frame_end\":" << fluid->domain->cache_frame_end
                         << ",\"type\":" << fluid->domain->cache_type << '}';
            }
            if (fluid->flow) {
                const char *flowType = nullptr;
                switch (fluid->flow->type) {
                case FluidFlowSettings::Flow_Smoke: flowType = "smoke"; break;
                case FluidFlowSettings::Flow_Fire: flowType = "fire"; break;
                case FluidFlowSettings::Flow_SmokeFire: flowType = "smoke_fire"; break;
                case FluidFlowSettings::Flow_Liquid: flowType = "liquid"; break;
                default: break;
                }
                settings << ",\"flow\":{\"type\":" << fluid->flow->type;
                if (flowType != nullptr) {
                    settings << ",\"flow_type\":\"" << flowType << '"';
                }
                settings << ",\"behavior\":" << fluid->flow->behavior
                         << ",\"density\":" << fluid->flow->density
                         << ",\"fuel_amount\":" << fluid->flow->fuel_amount
                         << ",\"temperature\":" << fluid->flow->temperature << '}';
            }
            if (fluid->effector) {
                settings << ",\"effector\":{\"type\":" << fluid->effector->type
                         << ",\"surface_distance\":" << fluid->effector->surface_distance
                         << ",\"flags\":" << fluid->effector->flags
                         << ",\"subframes\":" << fluid->effector->subframes
                         << ",\"vel_multi\":" << fluid->effector->vel_multi
                         << ",\"guide_mode\":" << fluid->effector->guide_mode << '}';
            }
        }
        if (dat.type == ModifierData::eModifierType_Smoke &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "SmokeModifierData")) {
            const auto *smoke = static_cast<const SmokeModifierData *>(cur);
            const char *role = "unknown";
            if (smoke->type & FluidModifierData::MOD_FLUID_TYPE_DOMAIN) {
                role = "domain";
            } else if (smoke->type & FluidModifierData::MOD_FLUID_TYPE_FLOW) {
                role = "flow";
            } else if (smoke->type & FluidModifierData::MOD_FLUID_TYPE_EFFEC) {
                role = "obstacle";
            }
            // No domain_type on legacy Smoke DNA - Host: smoke bag => gas solver.
            settings << ",\"source\":\"SmokeModifierData\",\"fluid_role\":\"" << role << '"';
        }
        // Legacy Elbeem Fluidsim (Blender 2.x) - FluidsimSettings.type bits.
        if (dat.type == ModifierData::eModifierType_Fluidsim &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "FluidsimModifierData")) {
            const auto *fs = static_cast<const FluidsimModifierData *>(cur);
            settings << ",\"source\":\"FluidsimModifierData\"";
            if (fs->fss) {
                const FluidsimSettings &fss = *fs->fss;
                const char *role = "unknown";
                // Prefer specific roles when multiple bits (DNA can OR ENABLE).
                if (fss.type & FluidsimSettings::Type_Domain) {
                    role = "domain";
                } else if (fss.type & FluidsimSettings::Type_Inflow) {
                    role = "inflow";
                } else if (fss.type & FluidsimSettings::Type_Outflow) {
                    role = "outflow";
                } else if (fss.type & FluidsimSettings::Type_Obstacle) {
                    role = "obstacle";
                } else if (fss.type & FluidsimSettings::Type_Fluid) {
                    role = "flow"; // Elbeem fluid body
                } else if (fss.type & FluidsimSettings::Type_Particle) {
                    role = "particle";
                } else if (fss.type & FluidsimSettings::Type_Control) {
                    role = "control";
                }
                settings << ",\"fluid_role\":\"" << role << '"';
                settings << ",\"fluidsim_type\":" << static_cast<int>(fss.type);
                // Elbeem is liquid surface sim - domain_type only when Domain bit set.
                if (fss.type & FluidsimSettings::Type_Domain) {
                    settings << ",\"domain_type\":\"liquid\"";
                }
                settings << ",\"resolutionxyz\":" << fss.resolutionxyz
                         << ",\"previewresxyz\":" << fss.previewresxyz
                         << ",\"realsize\":" << fss.realsize
                         << ",\"viscosityValue\":" << fss.viscosityValue
                         << ",\"animStart\":" << fss.animStart
                         << ",\"animEnd\":" << fss.animEnd
                         << ",\"bakeStart\":" << fss.bakeStart
                         << ",\"bakeEnd\":" << fss.bakeEnd
                         << ",\"frameOffset\":" << fss.frameOffset
                         << ",\"flag\":" << fss.flag
                         << ",\"typeFlags\":" << fss.typeFlags;
                if (fss.iniVelx != 0.f || fss.iniVely != 0.f || fss.iniVelz != 0.f) {
                    settings << ",\"iniVel\":[" << fss.iniVelx << ',' << fss.iniVely << ','
                             << fss.iniVelz << ']';
                }
                if (fss.surfdataPath[0] != '\0') {
                    settings << ",\"cache\":{\"directory\":\"" << BlendJsonEscape(fss.surfdataPath)
                             << "\",\"frame_start\":" << fss.bakeStart
                             << ",\"frame_end\":" << fss.bakeEnd << '}';
                    if (fss.surfdataPath[0] == '/' && fss.surfdataPath[1] == '/') {
                        settings << ",\"cachePathBlendRelative\":true";
                    }
                }
            } else {
                conv.skippedCategories.push_back(
                        std::string("BLEND_fluidsim_no_settings:") + (objName ? objName : "?") + "/" +
                        dat.name);
            }
        }
        if (dat.type == ModifierData::eModifierType_Collision &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "CollisionModifierData")) {
            settings << ",\"source\":\"CollisionModifierData\"";
            if (obj->pd) {
                settings << ",\"deflect\":" << static_cast<int>(obj->pd->deflect)
                         << ",\"pdef_cfrict\":" << obj->pd->pdef_cfrict
                         << ",\"pdef_damp\":" << obj->pd->pdef_damp
                         << ",\"pdef_frict\":" << obj->pd->pdef_frict;
            }
        }
        if (dat.type == ModifierData::eModifierType_Curve &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "CurveModifierData")) {
            const auto *cm = static_cast<const CurveModifierData *>(cur);
            settings << ",\"source\":\"CurveModifierData\""
                     << ",\"defaxis\":" << cm->defaxis
                     << ",\"flag\":" << cm->flag;
            if (cm->name[0] != '\0') {
                settings << ",\"vgroup\":\"" << BlendJsonEscape(cm->name) << '"';
            }
            if (std::shared_ptr<Object> curveOb = cm->object.lock()) {
                if (curveOb->id.name[0] != '\0') {
                    settings << ",\"curveObject\":\"" << BlendJsonEscape(curveOb->id.name + 2) << '"';
                }
            }
        }

        if (dat.type == ModifierData::eModifierType_Ocean &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "OceanModifierData")) {
            const auto *ocean = static_cast<const OceanModifierData *>(cur);
            settings << ",\"source\":\"OceanModifierData\"";
            if (ocean->hasResolution) {
                settings << ",\"resolution\":" << ocean->resolution;
            }
            if (ocean->hasSpatialSize) {
                settings << ",\"spatial_size\":" << ocean->spatial_size;
            }
            if (ocean->hasDepth) {
                settings << ",\"depth\":" << ocean->depth;
            }
            if (ocean->hasWaveScale) {
                settings << ",\"wave_scale\":" << ocean->wave_scale;
            }
            if (ocean->hasTime) {
                settings << ",\"time\":" << ocean->time;
            }
            if (ocean->hasChopAmount) {
                settings << ",\"chop_amount\":" << ocean->chop_amount;
            }
            if (ocean->hasWindVelocity) {
                settings << ",\"wind_velocity\":" << ocean->wind_velocity;
            }
            if (ocean->hasCached) {
                settings << ",\"cached\":" << (ocean->cached ? "true" : "false");
            }
            // Host animates Ocean.time from scene frame when DNA time is present.
            if (ocean->hasTime) {
                settings << ",\"timeDrivenByFrame\":true";
            }
            if (ocean->hasCachepath && ocean->cachepath[0] != '\0') {
                settings << ",\"cachepath\":\"" << BlendJsonEscape(ocean->cachepath) << '"';
            }
        }
        if (dat.type == ModifierData::eModifierType_DynamicPaint &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "DynamicPaintModifierData")) {
            const auto *dp = static_cast<const DynamicPaintModifierData *>(cur);
            settings << ",\"source\":\"DynamicPaintModifierData\"";
            if (dp->hasType) {
                const char *ui = "unknown";
                if (dp->type & DynamicPaintModifierData::Type_Brush) {
                    ui = "brush";
                } else if (dp->type & DynamicPaintModifierData::Type_Canvas) {
                    ui = "canvas";
                }
                settings << ",\"ui_type\":\"" << ui << '"';
            }
        }
        if (dat.type == ModifierData::eModifierType_Solidify &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "SolidifyModifierData")) {
            const auto *sol = static_cast<const SolidifyModifierData *>(cur);
            settings << ",\"source\":\"SolidifyModifierData\"";
            if (sol->hasOffset) {
                settings << ",\"thickness\":" << sol->offset;
            }
        }
        // 0al: Mesh Cache (MDD/PC2) / Mesh Sequence Cache (Alembic/USD) - paths only.
        if (dat.type == ModifierData::eModifierType_MeshCache &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "MeshCacheModifierData")) {
            const auto *mc = static_cast<const MeshCacheModifierData *>(cur);
            settings << ",\"source\":\"MeshCacheModifierData\"";
            const char *kind = "mesh_cache";
            if (mc->type == MeshCacheModifierData::Type_MDD) {
                kind = "mdd";
            } else if (mc->type == MeshCacheModifierData::Type_PC2) {
                kind = "pc2";
            }
            settings << ",\"cacheKind\":\"" << kind << '"';
            if (mc->filepath[0] != '\0') {
                settings << ",\"cachePath\":\"" << BlendJsonEscape(mc->filepath) << '"';
                if (mc->filepath[0] == '/' && mc->filepath[1] == '/') {
                    settings << ",\"cachePathBlendRelative\":true";
                }
            } else {
                conv.skippedCategories.push_back(
                        std::string("BLEND_mesh_cache_no_path:") + (objName ? objName : "?") + "/" +
                        dat.name);
            }
            settings << ",\"time_mode\":" << static_cast<int>(mc->time_mode)
                     << ",\"play_mode\":" << static_cast<int>(mc->play_mode)
                     << ",\"factor\":" << mc->factor
                     << ",\"frame_start\":" << mc->frame_start
                     << ",\"frame_scale\":" << mc->frame_scale
                     << ",\"eval_frame\":" << mc->eval_frame
                     << ",\"eval_time\":" << mc->eval_time
                     << ",\"eval_factor\":" << mc->eval_factor;
        }
        if (dat.type == ModifierData::eModifierType_MeshSequenceCache &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "MeshSeqCacheModifierData")) {
            const auto *ms = static_cast<const MeshSeqCacheModifierData *>(cur);
            settings << ",\"source\":\"MeshSeqCacheModifierData\"";
            // cacheKind: DNA CacheFile.type when set; else Presence from authored filepath suffix.
            auto kindFromPath = [](const char *path) -> const char * {
                if (path == nullptr || path[0] == '\0') {
                    return nullptr;
                }
                const char *dot = std::strrchr(path, '.');
                if (dot == nullptr) {
                    return nullptr;
                }
                if (std::strcmp(dot, ".abc") == 0) {
                    return "alembic";
                }
                if (std::strcmp(dot, ".usd") == 0 || std::strcmp(dot, ".usda") == 0 ||
                        std::strcmp(dot, ".usdc") == 0 || std::strcmp(dot, ".usdz") == 0) {
                    return "usd";
                }
                return nullptr;
            };
            const char *kind = "mesh_sequence_cache";
            if (ms->cache_file) {
                if (ms->cache_file->type == CacheFile::Type_Alembic) {
                    kind = "alembic";
                } else if (ms->cache_file->type == CacheFile::Type_USD) {
                    kind = "usd";
                } else if (const char *fromPath = kindFromPath(ms->cache_file->filepath)) {
                    kind = fromPath;
                }
                settings << ",\"cacheKind\":\"" << kind << '"';
                settings << ",\"cacheFileType\":" << static_cast<int>(ms->cache_file->type);
                if (ms->cache_file->filepath[0] != '\0') {
                    settings << ",\"cachePath\":\"" << BlendJsonEscape(ms->cache_file->filepath)
                             << '"';
                    if (ms->cache_file->filepath[0] == '/' && ms->cache_file->filepath[1] == '/') {
                        settings << ",\"cachePathBlendRelative\":true";
                    }
                } else {
                    conv.skippedCategories.push_back(
                            std::string("BLEND_mesh_cache_no_path:") + (objName ? objName : "?") +
                            "/" + dat.name);
                }
                if (ms->cache_file->id.name[0] != '\0') {
                    settings << ",\"cacheFile\":\""
                             << BlendJsonEscape(ms->cache_file->id.name + 2) << '"';
                }
                // Presence: CacheFile.frame / frame_offset when DNA resolved.
                settings << ",\"cacheFileFrame\":" << ms->cache_file->frame
                         << ",\"cacheFileFrameOffset\":" << ms->cache_file->frame_offset;
                if (ms->cache_file->override_frame) {
                    settings << ",\"cacheFileOverrideFrame\":true";
                }
            } else {
                settings << ",\"cacheKind\":\"" << kind << '"';
                conv.skippedCategories.push_back(
                        std::string("BLEND_mesh_cache_no_cachefile:") + (objName ? objName : "?") +
                        "/" + dat.name);
            }
            if (ms->object_path[0] != '\0') {
                settings << ",\"objectPath\":\"" << BlendJsonEscape(ms->object_path) << '"';
            }
            settings << ",\"read_flag\":" << static_cast<int>(ms->read_flag)
                     << ",\"velocity_scale\":" << ms->velocity_scale;
            // 0ak: DNA rest avg normal (mesh-local). Assimp does not evaluate Alembic;
            // Host uses ABC sample - this Presence documents DNA rest orientation.
            if (obj->type == Object::Type_MESH && obj->data) {
                const Mesh *emesh = static_cast<const Mesh *>(obj->data.get());
                float avgN[3];
                if (emesh != nullptr && BlendMeshRestAvgNormal(*emesh, avgN)) {
                    settings << ",\"restAvgNormal\":[" << avgN[0] << ',' << avgN[1] << ',' << avgN[2]
                             << "],\"restAvgNormalSpace\":\"meshLocal\"";
                }
            }
        }
        // 0ak also on MeshCache (MDD/PC2) when DNA rest resolves.
        if (dat.type == ModifierData::eModifierType_MeshCache &&
                cur->dna_type != nullptr &&
                BlendElemConvertedHullIs(*(cur), "MeshCacheModifierData") &&
                obj->type == Object::Type_MESH && obj->data) {
            const Mesh *emesh = static_cast<const Mesh *>(obj->data.get());
            float avgN[3];
            if (emesh != nullptr && BlendMeshRestAvgNormal(*emesh, avgN)) {
                settings << ",\"restAvgNormal\":[" << avgN[0] << ',' << avgN[1] << ',' << avgN[2]
                         << "],\"restAvgNormalSpace\":\"meshLocal\"";
            }
        }
        settings << '}';

        std::ostringstream nodeJson;
        nodeJson << "{\"name\":\"" << BlendJsonEscape(objName)
                 << "\",\"modifier\":\"" << BlendJsonEscape(dat.name)
                 << "\",\"type\":\"" << bagKey << '"'
                 << ",\"stackSlot\":" << stackSlot
                 << ",\"emitClass\":\"deferred\"";
        if (wantMeshMatBind) {
            BlendAppendNodeMeshMaterialBind(nodeJson, node, conv);
        }
        nodeJson << ",\"settings\":" << settings.str() << '}';
        BlendPushPhysicsInv(*bag, objName, nodeJson.str());
    }

    // C5: non-TRS Action FCurves (modifier props etc.) -> animPointerChannels.
    auto collectAct = [&](const std::shared_ptr<bAction> &act) {
        if (!act) {
            return;
        }
        const char *actName = (act->id.name[0] != '\0') ? (act->id.name + 2) : "";
        for (std::shared_ptr<ElemBase> cur = act->curves.first; cur;) {
            const FCurve *fc = BlendCastFCurve(cur.get());
            if (fc == nullptr) {
                break;
            }
            cur = fc->next;
            if (fc->rna_path.empty() || fc->bezt.empty()) {
                continue;
            }
            if (fc->rna_path == "location" || fc->rna_path == "rotation_euler" ||
                    fc->rna_path == "rotation_quaternion" || fc->rna_path == "scale" ||
                    fc->rna_path == "delta_location" || fc->rna_path == "delta_scale") {
                continue;
            }
            ConversionData::AnimPointerChannel ch;
            ch.actionName = actName ? actName : "";
            ch.nodeName = objName ? objName : "";
            ch.rnaPath = fc->rna_path;
            ch.arrayIndex = fc->array_index;
            ch.times.reserve(fc->bezt.size());
            ch.values.reserve(fc->bezt.size());
            for (const BezTriple &bz : fc->bezt) {
                ch.times.push_back(bz.vec[1][0]);
                ch.values.push_back(bz.vec[1][1]);
            }
            conv.animPointerChannels.push_back(std::move(ch));
        }
    };
    if (obj->adt) {
        collectAct(obj->adt->action);
        for (std::shared_ptr<ElemBase> tcur = obj->adt->nla_tracks.first; tcur;) {
            const NlaTrack *track = nullptr;
            if (tcur->dna_type == nullptr || BlendElemConvertedHullIs(*(tcur), "NlaTrack")) {
                track = static_cast<const NlaTrack *>(tcur.get());
            }
            if (track == nullptr) {
                break;
            }
            for (std::shared_ptr<ElemBase> scur = track->strips.first; scur;) {
                const NlaStrip *strip = nullptr;
                if (scur->dna_type == nullptr || BlendElemConvertedHullIs(*(scur), "NlaStrip")) {
                    strip = static_cast<const NlaStrip *>(scur.get());
                }
                if (strip == nullptr) {
                    break;
                }
                collectAct(strip->act);
                scur = strip->next;
            }
            tcur = track->next;
        }
    }
    // C5 Drivers: AnimData.drivers -> ConversionData.driverChannels (Animation sidecar).
    BlendCollectObjectDrivers(obj, conv);
}
} // namespace

// zlib is needed for compressed blend files
#ifndef ASSIMP_BUILD_NO_COMPRESSED_BLEND
#include "Common/Compression.h"
#endif

namespace Assimp {

template <>
const char *LogFunctions<BlenderImporter>::Prefix() {
    return "BLEND: ";
}

} // namespace Assimp

using namespace Assimp;
using namespace Assimp::Blender;
using namespace Assimp::Formatter;

static constexpr aiImporterDesc blenderDesc = {
    "Blender 3D Importer (http://www.blender3d.org)",
    "",
    "",
    "No animation support yet",
    aiImporterFlags_SupportBinaryFlavour,
    0,
    0,
    2,
    50,
    "blend"
};

// ------------------------------------------------------------------------------------------------
// Constructor to be privately used by Importer
BlenderImporter::BlenderImporter() :
        modifier_cache(new BlenderModifierShowcase()) {
    // empty
}

// ------------------------------------------------------------------------------------------------
// Destructor, private as well
BlenderImporter::~BlenderImporter() {
    delete modifier_cache;
}

static constexpr char Token[] = "BLENDER";

// ------------------------------------------------------------------------------------------------
// Returns whether the class can handle the format of the given file.
bool BlenderImporter::CanRead(const std::string &pFile, IOSystem *pIOHandler, bool /*checkSig*/) const {
    return ParseMagicToken(pFile, pIOHandler).error.empty();
}

// ------------------------------------------------------------------------------------------------
// Loader registry entry
const aiImporterDesc *BlenderImporter::GetInfo() const {
    return &blenderDesc;
}

// ------------------------------------------------------------------------------------------------
// Setup configuration properties for the loader
void BlenderImporter::SetupProperties(const Importer * /*pImp*/) {
    // nothing to be done for the moment
}

// ------------------------------------------------------------------------------------------------
// Imports the given file into the given scene structure.
void BlenderImporter::InternReadFile(const std::string &pFile,
        aiScene *pScene, IOSystem *pIOHandler) {
    FileDatabase file;
    StreamOrError streamOrError = ParseMagicToken(pFile, pIOHandler);
    if (!streamOrError.error.empty()) {
        ThrowException(streamOrError.error);
    }
    std::shared_ptr<IOStream> stream = std::move(streamOrError.stream);

    // After "BLENDER" magic: format v0 is 5 more bytes (12 total), v1 is 10 more (17 total).
    // Spec: Blender BLO_core_blend_header.hh (BLEND_FILE_FORMAT_VERSION_0 / _1).
    char hdr[16] = { 0 };
    if (stream->Read(hdr, 1, 5) != 5) {
        ThrowException("BLEND: truncated file header");
    }

    char blenderVersionStr[16] = { 0 };
    if (hdr[0] == '_' || hdr[0] == '-') {
        // Format v0: ptr, endian, 3-digit BLENDER_FILE_VERSION
        file.file_format_version = 0;
        file.i64bit = (hdr[0] == '-');
        if (hdr[1] != 'v' && hdr[1] != 'V') {
            ThrowException("BLEND: invalid endian marker in v0 header");
        }
        file.little = (hdr[1] == 'v');
        if (!std::isdigit(static_cast<unsigned char>(hdr[2])) ||
                !std::isdigit(static_cast<unsigned char>(hdr[3])) ||
                !std::isdigit(static_cast<unsigned char>(hdr[4]))) {
            ThrowException("BLEND: invalid version digits in v0 header");
        }
        file.blender_file_version = (hdr[2] - '0') * 100 + (hdr[3] - '0') * 10 + (hdr[4] - '0');
        blenderVersionStr[0] = hdr[2];
        blenderVersionStr[1] = '.';
        blenderVersionStr[2] = hdr[3];
        blenderVersionStr[3] = hdr[4];
        blenderVersionStr[4] = '\0';
    } else if (std::isdigit(static_cast<unsigned char>(hdr[0])) &&
               std::isdigit(static_cast<unsigned char>(hdr[1]))) {
        // Format v1: ASCII header size ("17"), then 5 more bytes already partially read...
        // Bytes read so far after magic: [0]=size tens, [1]=size ones, need rest of 17-byte header.
        const int header_size = (hdr[0] - '0') * 10 + (hdr[1] - '0');
        if (header_size != 17) {
            ThrowException("BLEND: unsupported header size ", header_size,
                    " (expected 12 for v0 or 17 for v1)");
        }
        // Already have 5 bytes after magic (positions 7..11 of full header). Need bytes 12..16.
        char hdr_rest[8] = { 0 };
        if (stream->Read(hdr_rest, 1, 5) != 5) {
            ThrowException("BLEND: truncated format-v1 file header");
        }
        // Full after-magic: hdr[0..4] + hdr_rest[0..4] == bytes 7..16
        // 7-8 size, 9 '-', 10-11 format ver, 12 'v', 13-16 file version
        if (hdr[2] != '-') {
            ThrowException("BLEND: format-v1 header missing '-' separator");
        }
        if (!std::isdigit(static_cast<unsigned char>(hdr[3])) ||
                !std::isdigit(static_cast<unsigned char>(hdr[4]))) {
            ThrowException("BLEND: invalid format-version digits in v1 header");
        }
        file.file_format_version = (hdr[3] - '0') * 10 + (hdr[4] - '0');
        if (file.file_format_version != 1) {
            char dump[24];
            std::snprintf(dump, sizeof(dump), "BLENDER%c%c%c%c%c%c%c%c%c%c",
                    hdr[0], hdr[1], hdr[2], hdr[3], hdr[4],
                    hdr_rest[0], hdr_rest[1], hdr_rest[2], hdr_rest[3], hdr_rest[4]);
            ThrowException("BLEND: unsupported file format version ", file.file_format_version,
                    " (header ", dump, ")");
        }
        if (hdr_rest[0] != 'v') {
            ThrowException("BLEND: format-v1 requires little-endian marker 'v'");
        }
        file.little = true;
        file.i64bit = true; // LargeBHead8 always 8-byte pointers
        if (!std::isdigit(static_cast<unsigned char>(hdr_rest[1])) ||
                !std::isdigit(static_cast<unsigned char>(hdr_rest[2])) ||
                !std::isdigit(static_cast<unsigned char>(hdr_rest[3])) ||
                !std::isdigit(static_cast<unsigned char>(hdr_rest[4]))) {
            ThrowException("BLEND: invalid BLENDER_FILE_VERSION digits in v1 header");
        }
        file.blender_file_version =
                (hdr_rest[1] - '0') * 1000 + (hdr_rest[2] - '0') * 100 +
                (hdr_rest[3] - '0') * 10 + (hdr_rest[4] - '0');
        // "0500" -> "5.00" for generator string
        std::snprintf(blenderVersionStr, sizeof(blenderVersionStr), "%d.%02d",
                file.blender_file_version / 100, file.blender_file_version % 100);
    } else {
        char dump[8];
        std::snprintf(dump, sizeof(dump), "%c%c%c%c%c", hdr[0], hdr[1], hdr[2], hdr[3], hdr[4]);
        ThrowException("BLEND: unrecognized header after BLENDER magic: ", dump);
    }

    LogInfo("Blender version is ", blenderVersionStr,
            " (format: ", file.file_format_version,
            ", file_version: ", file.blender_file_version,
            ", 64bit: ", file.i64bit ? "true" : "false",
            ", little endian: ", file.little ? "true" : "false", ")");

    ParseBlendFile(file, std::move(stream));

    Scene scene;
    ExtractScene(scene, file);

    ConvertBlendFile(pScene, scene, file, blenderVersionStr);
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::ParseBlendFile(FileDatabase &out, std::shared_ptr<IOStream> stream) {
    out.reader = std::make_shared<StreamReaderAny>(stream, out.little);

    DNAParser dna_reader(out);
    const DNA *dna = nullptr;

    out.entries.reserve(128);
    { // even small BLEND files tend to consist of many file blocks
        const bool largeBHead = (out.file_format_version >= 1);
        SectionParser parser(*out.reader, out.i64bit, largeBHead);

        // first parse the file in search for the DNA and insert all other sections into the database
        while ((parser.Next(), 1)) {
            const FileBlockHead &head = parser.GetCurrent();

            if (head.id == "ENDB") {
                break; // only valid end of the file
            } else if (head.id == "DNA1") {
                dna_reader.Parse();
                dna = &dna_reader.GetDNA();
                continue;
            }

            out.entries.push_back(head);
        }
    }
    if (!dna) {
        ThrowException("SDNA not found");
    }

    // Tag DATA blocks with their owning ID (file order), matching Blender's
    // read_data_into_datamap: DATA following an ID belongs to that ID until the
    // next non-DATA block. Required because `old` addresses can collide across IDs.
    {
        Pointer owner;
        owner.val = 0;
        for (FileBlockHead &e : out.entries) {
            if (e.id != "DATA") {
                owner = e.address;
                e.owner_id.val = 0;
            } else {
                e.owner_id = owner;
            }
        }
    }

    std::sort(out.entries.begin(), out.entries.end());
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::ExtractScene(Scene &out, const FileDatabase &file) {
    const FileBlockHead *block = nullptr;
    std::map<std::string, size_t>::const_iterator it = file.dna.indices.find("Scene");
    if (it == file.dna.indices.end()) {
        ThrowException("There is no `Scene` structure record");
    }

    const Structure &ss = file.dna.structures[(*it).second];

    // we need a scene somewhere to start with.
    for (const FileBlockHead &bl : file.entries) {

        // Fix: using the DNA index is more reliable to locate scenes
        // if (bl.id == "SC") {

        if (bl.dna_index == (*it).second) {
            block = &bl;
            break;
        }
    }

    if (!block) {
        ThrowException("There is not a single `Scene` record to load");
    }

    file.reader->SetCurrentPos(block->start);
    // Scene Convert is not entered via ResolvePointer - set ID datamap scope.
    file.active_id_scope = block->address;
    ss.Convert(out, file);
    file.active_id_scope.val = 0;

#ifndef ASSIMP_BUILD_BLENDER_NO_STATS
    ASSIMP_LOG_INFO(
            "(Stats) Fields read: ", file.stats().fields_read,
            ", pointers resolved: ", file.stats().pointers_resolved,
            ", cache hits: ", file.stats().cache_hits,
            ", cached objects: ", file.stats().cached_objects);
#endif
}

// ------------------------------------------------------------------------------------------------
/** Gather Object* from a Collection and nested children (Blender 2.8+ view layer). */
static void BlendCollectCollectionObjects(const std::shared_ptr<Collection> &collection,
        std::vector<Object *> &out) {
    if (!collection) {
        return;
    }
    for (std::shared_ptr<CollectionObject> cur =
                    std::static_pointer_cast<CollectionObject>(collection->gobject.first);
            cur;
            cur = cur->next) {
        if (cur->ob) {
            out.push_back(cur->ob);
        }
    }
    for (std::shared_ptr<CollectionChild> cur =
                    std::static_pointer_cast<CollectionChild>(collection->children.first);
            cur;
            cur = cur->next) {
        if (cur->collection) {
            BlendCollectCollectionObjects(cur->collection, out);
        }
    }
}

/** Collect (collectionName -> object names) from a collection tree for C13 bind. */
static void BlendIndexCollections(
        const std::shared_ptr<Collection> &collection,
        std::vector<std::pair<std::string, std::vector<std::string>>> &out) {
    if (!collection) {
        return;
    }
    std::vector<std::string> names;
    for (std::shared_ptr<CollectionObject> cur =
                    std::static_pointer_cast<CollectionObject>(collection->gobject.first);
            cur;
            cur = cur->next) {
        if (cur->ob && cur->ob->id.name[0] != '\0') {
            names.emplace_back(cur->ob->id.name + 2);
        }
    }
    if (collection->id.name[0] != '\0') {
        out.emplace_back(collection->id.name + 2, std::move(names));
    }
    for (std::shared_ptr<CollectionChild> cur =
                    std::static_pointer_cast<CollectionChild>(collection->children.first);
            cur;
            cur = cur->next) {
        if (cur->collection) {
            BlendIndexCollections(cur->collection, out);
        }
    }
}

/** When DNA dup_group/instance_collection is null, bind prototypes to a owning Collection. */
static std::string BlendResolveCollectionForPrototypes(
        const Scene &in,
        const std::vector<std::string> &protoNames) {
    if (protoNames.empty() || !in.master_collection) {
        return {};
    }
    std::vector<std::pair<std::string, std::vector<std::string>>> cols;
    BlendIndexCollections(in.master_collection, cols);
    std::string best;
    size_t bestExtra = static_cast<size_t>(-1);
    for (const auto &col : cols) {
        size_t hits = 0;
        for (const std::string &pn : protoNames) {
            if (std::find(col.second.begin(), col.second.end(), pn) != col.second.end()) {
                ++hits;
            }
        }
        if (hits != protoNames.size()) {
            continue;
        }
        const size_t extra = col.second.size() - hits;
        if (best.empty() || extra < bestExtra) {
            best = col.first;
            bestExtra = extra;
        }
    }
    return best;
}

static void BlendEmitFloatArray(std::ostringstream &json, const char *key, const std::vector<float> &v) {
    json << ",\"" << key << "\":[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) {
            json << ',';
        }
        json << v[i];
    }
    json << ']';
}

static void BlendEmitIntArray(std::ostringstream &json, const char *key, const std::vector<int> &v) {
    json << ",\"" << key << "\":[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) {
            json << ',';
        }
        json << v[i];
    }
    json << ']';
}

static const aiNode *BlendFindNodeByName(const aiNode *n, const char *name) {
    if (!n || name == nullptr) {
        return nullptr;
    }
    if (n->mName == aiString(name)) {
        return n;
    }
    for (unsigned int ci = 0; ci < n->mNumChildren; ++ci) {
        if (const aiNode *f = BlendFindNodeByName(n->mChildren[ci], name)) {
            return f;
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::ParseSubCollection(const Blender::Scene &in, aiNode *root,
        const std::shared_ptr<Collection> &collection, ConversionData &conv_data) {
    // Kept for API stability: build Object.parent hierarchy from all objects in this
    // collection tree (same as ConvertBlendFile master_collection path).
    std::vector<Object *> all;
    BlendCollectCollectionObjects(collection, all);
    std::set<const Object *> allSet(all.begin(), all.end());
    std::deque<const Object *> no_parents;
    for (Object *ob : all) {
        if (ob == nullptr) {
            continue;
        }
        if (ob->parent != nullptr && allSet.count(ob->parent) != 0) {
            conv_data.objects.insert(ob);
        } else {
            no_parents.push_back(ob);
        }
    }
    root->mNumChildren = static_cast<unsigned int>(no_parents.size());
    if (root->mNumChildren == 0) {
        root->mChildren = nullptr;
        return;
    }
    root->mChildren = new aiNode *[root->mNumChildren]();
    for (unsigned int i = 0; i < root->mNumChildren; ++i) {
        root->mChildren[i] = ConvertNode(in, no_parents[i], conv_data, aiMatrix4x4());
        root->mChildren[i]->mParent = root;
    }
}

// ------------------------------------------------------------------------------------------------

/** Emit Asset Browser strings onto aiMetadata (Presence; aiString caps at AI_MAXLEN). */
void BlendEmitAssetBrowserMetadata(aiMetadata *&meta, const Assimp::Blender::AssetMetaData *asset,
        bool alsoSourceCopyright) {
    if (asset == nullptr) {
        return;
    }
    auto addStr = [&](const char *key, const std::string &val) {
        if (val.empty()) {
            return;
        }
        if (meta == nullptr) {
            meta = new aiMetadata();
        }
        // aiString(std::string) truncates to AI_MAXLEN-1.
        meta->Add(key, aiString(val));
    };
    addStr("Asset.Author", asset->author);
    addStr("Asset.Description", asset->description);
    addStr("Asset.Copyright", asset->copyright);
    addStr("Asset.License", asset->license);
    if (alsoSourceCopyright && !asset->copyright.empty()) {
        addStr(AI_METADATA_SOURCE_COPYRIGHT, asset->copyright);
    }
}

void BlenderImporter::ConvertBlendFile(aiScene *out, const Scene &in, const FileDatabase &file,
        const char *blenderVersion) {
    ConversionData conv(file);

    aiNode *root = out->mRootNode = new aiNode("<BlenderRoot>");
    // Blender 2.8+: objects live in master_collection. Hierarchy must follow Object.parent
    // (glTF/exporter style) - not flat Collection membership.
    if (in.master_collection) {
        ParseSubCollection(in, root, in.master_collection, conv);
    } else {
        std::deque<const Object *> no_parents;
        for (std::shared_ptr<Base> cur = std::static_pointer_cast<Base>(in.base.first); cur; cur = cur->next) {
            if (cur->object) {
                if (!cur->object->parent) {
                    no_parents.push_back(cur->object.get());
                } else {
                    conv.objects.insert(cur->object.get());
                }
            }
        }
        for (std::shared_ptr<Base> cur = in.basact; cur; cur = cur->next) {
            if (cur->object) {
                if (cur->object->parent) {
                    conv.objects.insert(cur->object.get());
                }
            }
        }

        if (no_parents.empty()) {
            ThrowException("Expected at least one object with no parent");
        }

        root->mNumChildren = static_cast<unsigned int>(no_parents.size());
        root->mChildren = new aiNode *[root->mNumChildren]();
        for (unsigned int i = 0; i < root->mNumChildren; ++i) {
            root->mChildren[i] = ConvertNode(in, no_parents[i], conv, aiMatrix4x4());
            root->mChildren[i]->mParent = root;
        }
    }

    BuildMaterials(conv);
    // 0al4: ParticleSettings.mtex[] -> typed $tex.file + meta (after mesh mats exist for UV resolve).
    BlendEmitParticleTypedMTexMaterials(conv);
    BlendEmitWorldEnv(in, conv);

    // Presence: emit every packed Image ID block, not only those reached via materials.
    std::vector<std::shared_ptr<Image>> ownedPackedImages;
    BlendEmitAllPackedImages(conv, ownedPackedImages);

    if (conv.meshes->size()) {
        out->mMeshes = new aiMesh *[out->mNumMeshes = static_cast<unsigned int>(conv.meshes->size())];
        std::copy(conv.meshes->begin(), conv.meshes->end(), out->mMeshes);
        conv.meshes.dismiss();
    }

    if (conv.lights->size()) {
        out->mLights = new aiLight *[out->mNumLights = static_cast<unsigned int>(conv.lights->size())];
        std::copy(conv.lights->begin(), conv.lights->end(), out->mLights);
        conv.lights.dismiss();
    }

    if (conv.cameras->size()) {
        out->mCameras = new aiCamera *[out->mNumCameras = static_cast<unsigned int>(conv.cameras->size())];
        std::copy(conv.cameras->begin(), conv.cameras->end(), out->mCameras);
        conv.cameras.dismiss();
    }

    if (conv.materials->size()) {
        out->mMaterials = new aiMaterial *[out->mNumMaterials = static_cast<unsigned int>(conv.materials->size())];
        std::copy(conv.materials->begin(), conv.materials->end(), out->mMaterials);
        conv.materials.dismiss();
    }

    if (conv.textures->size()) {
        out->mTextures = new aiTexture *[out->mNumTextures = static_cast<unsigned int>(conv.textures->size())];
        std::copy(conv.textures->begin(), conv.textures->end(), out->mTextures);
        conv.textures.dismiss();
    }

    // Camera Actions + Armature pose.bones Actions -> typed aiNodeAnim.
    {
        std::vector<aiAnimation *> anims;
        BlendConvertCameraAnimations(anims, conv);
        BlendConvertArmatureAnimations(anims, conv);
        if (!anims.empty()) {
            out->mNumAnimations = static_cast<unsigned int>(anims.size());
            out->mAnimations = new aiAnimation *[out->mNumAnimations];
            std::copy(anims.begin(), anims.end(), out->mAnimations);
        }
    }

    // acknowledge that the scene might come out incomplete
    // by Assimp's definition of `complete`: blender scenes
    // can consist of thousands of cameras or lights with
    // not a single mesh between them.
    if (!out->mNumMeshes) {
        out->mFlags |= AI_SCENE_FLAGS_INCOMPLETE;
    }

    // C0-S8 sidecars (doc/Sidecar.md BLEND). Always emit scene bag when Scene DNA loaded.
    // Typed: header SourceAsset_* + optional Scene Asset Browser fields (AssetMetaData).
    if (out->mMetaData == nullptr) {
        out->mMetaData = new aiMetadata();
    }
    const float fpsDenom = (in.r.frs_sec_base > 0.f) ? in.r.frs_sec_base : 1.f;
    const float fps = (fpsDenom != 0.f) ? (in.r.frs_sec / fpsDenom) : in.r.frs_sec;
    const float metersPerUnit = (in.unit.scale_length > 0.f) ? in.unit.scale_length : 1.f;

    // S1 mesh CustomData layers + DNA srcVert buffer (MDD / Mesh Cache bind).
    for (unsigned int mi = 0; mi < out->mNumMeshes; ++mi) {
        const bool hasJson = mi < conv.meshSidecarJson.size() && !conv.meshSidecarJson[mi].empty();
        const bool hasSrcVert = mi < conv.meshSrcVertDna.size() && !conv.meshSrcVertDna[mi].empty();
        if (!hasJson && !hasSrcVert) {
            continue;
        }
        auto *sc = new aiMeshSidecar();
        if (hasJson) {
            sc->mExtensionsJson = BlendDupJson(conv.meshSidecarJson[mi], sc->mExtensionsJsonLength);
        }
        if (hasSrcVert && out->mMeshes[mi] != nullptr &&
                conv.meshSrcVertDna[mi].size() == static_cast<size_t>(out->mMeshes[mi]->mNumVertices)) {
            const std::vector<int32_t> &sv = conv.meshSrcVertDna[mi];
            sc->mNumBuffers = 1;
            sc->mBuffers = new aiSidecarBuffer[1];
            aiSidecarBuffer &buf = sc->mBuffers[0];
            buf.mName.Set("srcVert");
            buf.mNumElements = static_cast<unsigned>(sv.size());
            buf.mNumComponents = 1;
            buf.mComponentType = static_cast<unsigned>(aiSidecarComponentType_UnsignedInt);
            buf.mByteLength = sv.size() * sizeof(uint32_t);
            buf.mData = new uint8_t[buf.mByteLength];
            uint32_t *dst = reinterpret_cast<uint32_t *>(buf.mData);
            for (size_t i = 0; i < sv.size(); ++i) {
                dst[i] = static_cast<uint32_t>(sv[i]);
            }
        }
        AttachMeshSidecar(out, mi, sc);
    }

    // S3 material bag: BLEND_shader (+ BLEND_shader_principled when C6 flatten succeeded)
    for (unsigned int mati = 0; mati < out->mNumMaterials; ++mati) {
        std::string body;
        if (mati < conv.materialSidecarJson.size() && !conv.materialSidecarJson[mati].empty()) {
            body = conv.materialSidecarJson[mati];
        } else {
            std::ostringstream mjson;
            mjson << "{\"schemaVersion\":1,\"source\":{\"format\":\"blend\"},\"BLEND_shader\":{";
            if (mati < conv.materials_raw.size() && conv.materials_raw[mati]) {
                mjson << "\"name\":\"" << BlendJsonEscape(conv.materials_raw[mati]->id.name + 2) << '"';
            }
            mjson << "}}";
            body = mjson.str();
        }
        auto *msc = new aiMaterialSidecar();
        msc->mExtensionsJson = BlendDupJson(body, msc->mExtensionsJsonLength);
        AttachMaterialSidecar(out, mati, msc);
    }

    // S5/S6 physics: modifiers + soft/rigid DNA + particles + KHR-shaped rigid when fields exist
    if (conv.physicsRigidNodes.empty()) {
        conv.skippedCategories.push_back("BLEND_physics_rigidbody_object_pointer");
    }
    conv.skippedCategories.push_back("BLEND_shader_network_full_nodegraph");
    // C5: only blanket-skip Action/NLA when nothing was collected into anim sidecar
    // and no typed aiAnimation exists for those channels.
    if (conv.animPointerChannels.empty() && conv.driverChannels.empty() && out->mNumAnimations == 0) {
        // Still avoid silent drop: specific skip only when we truly have no Action DNA walk result.
        // Water Caustics has PlaneAction -> channels; this branch then does not fire.
        conv.skippedCategories.push_back("BLEND_action_nla_animation_sidecar");
    }
    // Sculpt cloth brushes (cloth_brushes_demo) - Brush DNA inventory into BLEND_physics_cloth.
    BlendCollectClothBrushPresence(conv, &in);
    const bool hasPhys = !conv.physicsSoftNodes.empty() || !conv.physicsClothNodes.empty() ||
                         !conv.physicsFluidNodes.empty() || !conv.physicsSmokeNodes.empty() ||
                         !conv.physicsRigidNodes.empty() || !conv.physicsParticleNodes.empty() ||
                         !conv.physicsJointNodes.empty() || !conv.physicsBlastNodes.empty() ||
                         !conv.physicsOceanNodes.empty() || !conv.physicsDynamicPaintNodes.empty() ||
                         !conv.meshCacheNodes.empty() ||
                         !conv.physicsCollisionNodes.empty() || !conv.physicsCurveNodes.empty() ||
                         !conv.physicsSolidifyNodes.empty() ||
                         !conv.khrBodies.empty() || !conv.khrJoints.empty() ||
                         !conv.pointInstancers.empty();
    if (hasPhys) {
        std::unordered_map<std::string, int> nameToIndex;
        int nextIdx = 0;
        BlendBuildHierarchyIndexMap(out->mRootNode, nameToIndex, nextIdx);

        std::ostringstream pjson;
        pjson << "{\"schemaVersion\":1,\"source\":{\"format\":\"blend\"";
        if (blenderVersion != nullptr && blenderVersion[0] != '\0') {
            pjson << ",\"blender_version\":\"" << blenderVersion << '"';
        }
        pjson << '}';
        BlendEmitPhysicsInvBag(pjson, "BLEND_physics_softbody", conv.physicsSoftNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_physics_cloth", conv.physicsClothNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_physics_fluid", conv.physicsFluidNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_physics_smoke", conv.physicsSmokeNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_physics_rigidbody", conv.physicsRigidNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_physics_particles", conv.physicsParticleNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_physics_constraints", conv.physicsJointNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_physics_blast", conv.physicsBlastNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_modifier_ocean", conv.physicsOceanNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_modifier_dynamic_paint", conv.physicsDynamicPaintNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_modifier_solidify", conv.physicsSolidifyNodes, nameToIndex);
        // 0al: Mesh Cache / Mesh Sequence Cache / MDD / Alembic - paths + bind only.
        BlendEmitPhysicsInvBag(pjson, "BLEND_mesh_cache", conv.meshCacheNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_physics_collision", conv.physicsCollisionNodes, nameToIndex);
        BlendEmitPhysicsInvBag(pjson, "BLEND_modifier_curve", conv.physicsCurveNodes, nameToIndex);
        // C13 PointInstancer (particle collection/object) - full detail in Physics heap bag.
        if (!conv.pointInstancers.empty()) {
            bool anyUnresolvedArrays = false;
            pjson << ",\"BLEND_point_instancers\":{\"nodes\":[";
            for (size_t pi = 0; pi < conv.pointInstancers.size(); ++pi) {
                ConversionData::PointInstancerPending &inst = conv.pointInstancers[pi];
                if (inst.collectionName.empty()) {
                    inst.collectionName = BlendResolveCollectionForPrototypes(in, inst.prototypeNames);
                }
                if (pi) {
                    pjson << ',';
                }
                pjson << "{\"name\":\"" << BlendJsonEscape(inst.systemName.empty() ? inst.emitterName : inst.systemName)
                      << "\",\"kind\":\"PointInstancer\""
                      << ",\"source\":\"BLEND_particle\""
                      << ",\"emitter\":\"" << BlendJsonEscape(inst.emitterName) << '"'
                      << ",\"system\":\"" << BlendJsonEscape(inst.systemName) << '"';
                {
                    const auto it = nameToIndex.find(inst.emitterName);
                    if (it != nameToIndex.end()) {
                        pjson << ",\"emitterIndex\":" << it->second;
                    }
                }
                if (!inst.settingsName.empty()) {
                    pjson << ",\"settings\":\"" << BlendJsonEscape(inst.settingsName) << '"';
                }
                if (!inst.collectionName.empty()) {
                    pjson << ",\"collection\":\"" << BlendJsonEscape(inst.collectionName) << '"';
                }
                if (!inst.instanceObjectName.empty()) {
                    pjson << ",\"instanceObject\":\"" << BlendJsonEscape(inst.instanceObjectName) << '"';
                }
                pjson << ",\"ren_as\":" << inst.ren_as
                      << ",\"from\":" << inst.from
                      << ",\"distr\":" << inst.distr
                      << ",\"flag\":" << inst.flag
                      << ",\"totpart\":" << inst.totpart
                      << ",\"seed\":" << inst.seed
                      << ",\"size\":" << inst.size
                      << ",\"randsize\":" << inst.randsize
                      << ",\"jitfac\":" << inst.jitfac;
                if (inst.hasPhystype) {
                    pjson << ",\"phystype\":" << inst.phystype;
                }
                if (inst.hasDrawAs) {
                    pjson << ",\"draw_as\":" << inst.draw_as;
                }
                if (inst.hasDraw) {
                    pjson << ",\"draw\":" << inst.draw;
                }
                if (inst.hasUserjit) {
                    pjson << ",\"userjit\":" << inst.userjit;
                }
                if (inst.hasGridRes) {
                    pjson << ",\"grid_res\":" << inst.grid_res;
                }
                if (inst.hasGridRand) {
                    pjson << ",\"grid_rand\":" << inst.grid_rand;
                }
                if (inst.hasRandlife) {
                    pjson << ",\"randlife\":" << inst.randlife;
                }
                if (inst.hasRandlength) {
                    pjson << ",\"randlength\":" << inst.randlength;
                }
                if (inst.hasHairStep) {
                    pjson << ",\"hair_step\":" << inst.hair_step;
                }
                // Presence: hair_length only when DNA normfac was in the file SDNA.
                if (inst.hasHairLength) {
                    pjson << ",\"normfac\":" << inst.normfac
                          << ",\"hair_length\":" << (inst.normfac * 4.f);
                }
                pjson << ",\"prototypes\":[";
                for (size_t pr = 0; pr < inst.prototypeNames.size(); ++pr) {
                    if (pr) {
                        pjson << ',';
                    }
                    const std::string &pn = inst.prototypeNames[pr];
                    pjson << "{\"name\":\"" << BlendJsonEscape(pn) << '"';
                    if (pr < inst.prototypeIndices.size()) {
                        pjson << ",\"dupliIndex\":" << inst.prototypeIndices[pr];
                    }
                    if (pr < inst.prototypeCounts.size()) {
                        pjson << ",\"count\":" << inst.prototypeCounts[pr];
                    }
                    const auto nit = nameToIndex.find(pn);
                    if (nit != nameToIndex.end()) {
                        pjson << ",\"nodeIndex\":" << nit->second;
                        // Prototype object mesh slot (mesh datablock name may differ from object name).
                        if (const aiNode *pnode = BlendFindNodeByName(out->mRootNode, pn.c_str())) {
                            if (pnode->mNumMeshes > 0) {
                                pjson << ",\"meshIndex\":" << pnode->mMeshes[0];
                            }
                        }
                    }
                    pjson << '}';
                }
                pjson << ']';
                if (inst.arraysResolved) {
                    pjson << ",\"arraysResolved\":true";
                    if (!inst.arraysSource.empty()) {
                        pjson << ",\"arraysSource\":\"" << BlendJsonEscape(inst.arraysSource) << '"';
                    }
                    // Host 0ai Presence: explicit transform spaces (no invent if unset).
                    if (!inst.positionsSpace.empty()) {
                        pjson << ",\"positionsSpace\":\"" << BlendJsonEscape(inst.positionsSpace) << '"';
                    }
                    if (!inst.scalesSpace.empty()) {
                        pjson << ",\"scalesSpace\":\"" << BlendJsonEscape(inst.scalesSpace) << '"';
                    }
                    if (!inst.scalesMeaning.empty()) {
                        pjson << ",\"scalesMeaning\":\"" << BlendJsonEscape(inst.scalesMeaning) << '"';
                    }
                    BlendEmitFloatArray(pjson, "positions", inst.positions);
                    BlendEmitFloatArray(pjson, "orients", inst.orients);
                    BlendEmitFloatArray(pjson, "scales", inst.scales);
                    if (!inst.protoIndices.empty()) {
                        BlendEmitIntArray(pjson, "protoIndices", inst.protoIndices);
                    }
                    // Presence: only when mesh-face eval produced real per-instance weights.
                    if (!inst.faceArea.empty()) {
                        BlendEmitFloatArray(pjson, "faceArea", inst.faceArea);
                    }
                    if (!inst.emitWeight.empty()) {
                        BlendEmitFloatArray(pjson, "emitWeight", inst.emitWeight);
                    }
                } else {
                    pjson << ",\"arraysResolved\":false";
                    anyUnresolvedArrays = true;
                }
                pjson << '}';
            }
            pjson << "]}";
            if (anyUnresolvedArrays) {
                conv.skippedCategories.push_back("BLEND_particle_instance_arrays");
            }
        }
        // Host Bullet/PhysX: KHR joints/filters/nodes (glTF-shaped).
        if (!conv.khrBodies.empty() || !conv.khrJoints.empty()) {
            BlendEmitKhrPhysics(pjson, conv, nameToIndex, out);
        }
        pjson << '}';
        auto *psc = new aiPhysicsSidecar();
        psc->mExtensionsJson = BlendDupJson(pjson.str(), psc->mExtensionsJsonLength);
        AttachPhysicsSidecar(out, psc);
    }

    // C5 Animation sidecar: modifier/pointer FCurves + DNA Drivers (ChannelDriver).
    auto fillFloatBuf = [](aiSidecarBuffer &buf, const std::string &name, const std::vector<float> &data) {
        buf.mName.Set(name);
        buf.mNumElements = static_cast<unsigned>(data.size());
        buf.mNumComponents = 1;
        buf.mComponentType = static_cast<unsigned>(aiSidecarComponentType_Float);
        buf.mByteLength = data.size() * sizeof(float);
        buf.mData = new uint8_t[buf.mByteLength];
        if (!data.empty()) {
            std::memcpy(buf.mData, data.data(), buf.mByteLength);
        }
    };
    if (!conv.animPointerChannels.empty()) {
        std::map<std::string, std::vector<size_t>> byAction;
        for (size_t ci = 0; ci < conv.animPointerChannels.size(); ++ci) {
            const std::string &an = conv.animPointerChannels[ci].actionName;
            byAction[an.empty() ? std::string("Action") : an].push_back(ci);
        }
        const unsigned baseAnim = out->mNumAnimations;
        const unsigned addCount = static_cast<unsigned>(byAction.size());
        aiAnimation **newAnims = new aiAnimation *[baseAnim + addCount];
        for (unsigned i = 0; i < baseAnim; ++i) {
            newAnims[i] = out->mAnimations[i];
        }
        if (out->mAnimations) {
            delete[] out->mAnimations;
        }
        out->mAnimations = newAnims;
        out->mNumAnimations = baseAnim + addCount;
        unsigned aidx = baseAnim;
        for (const auto &kv : byAction) {
            const std::vector<size_t> &chs = kv.second;
            aiAnimation *anim = new aiAnimation();
            anim->mName.Set(kv.first);
            anim->mTicksPerSecond = fps > 0.f ? fps : 24.0;
            anim->mNumChannels = 0;
            anim->mChannels = nullptr;
            double tmax = 0.0;
            for (size_t ci : chs) {
                for (float tt : conv.animPointerChannels[ci].times) {
                    tmax = std::max(tmax, static_cast<double>(tt));
                }
            }
            anim->mDuration = tmax;
            out->mAnimations[aidx] = anim;

            const unsigned nBuf = static_cast<unsigned>(chs.size() * 2u);
            auto *asc = new aiAnimationSidecar();
            asc->mNumBuffers = nBuf;
            asc->mBuffers = new aiSidecarBuffer[nBuf];
            std::ostringstream ajson;
            ajson.setf(std::ios::fixed);
            ajson.precision(9);
            ajson << "{\"schemaVersion\":1,\"source\":{\"format\":\"blend\"},"
                  << "\"BLEND_animation_pointer\":{\"action\":\"" << BlendJsonEscape(kv.first)
                  << "\",\"channels\":[";
            for (size_t i = 0; i < chs.size(); ++i) {
                const auto &ch = conv.animPointerChannels[chs[i]];
                const unsigned biTimes = static_cast<unsigned>(i * 2u);
                const unsigned biVals = biTimes + 1u;
                const std::string tName = "ch" + std::to_string(i) + "_times";
                const std::string vName = "ch" + std::to_string(i) + "_values";
                fillFloatBuf(asc->mBuffers[biTimes], tName, ch.times);
                fillFloatBuf(asc->mBuffers[biVals], vName, ch.values);
                if (i) {
                    ajson << ',';
                }
                ajson << "{\"node\":\"" << BlendJsonEscape(ch.nodeName)
                      << "\",\"rnaPath\":\"" << BlendJsonEscape(ch.rnaPath)
                      << "\",\"arrayIndex\":" << ch.arrayIndex
                      << ",\"pointer\":\"/nodes/" << BlendJsonEscape(ch.nodeName) << "/"
                      << BlendJsonEscape(ch.rnaPath) << "\""
                      << ",\"inputBuffer\":\"" << tName << "\""
                      << ",\"outputBuffer\":\"" << vName << "\""
                      << ",\"outputComponents\":1,\"interpolation\":\"LINEAR\"}";
            }
            ajson << "]}}";
            asc->mExtensionsJson = BlendDupJson(ajson.str(), asc->mExtensionsJsonLength);
            AttachAnimationSidecar(out, aidx, asc);
            ++aidx;
        }
    }
    if (!conv.driverChannels.empty()) {
        // Group by owner Object name (Presence).
        std::map<std::string, std::vector<size_t>> byOwner;
        for (size_t ci = 0; ci < conv.driverChannels.size(); ++ci) {
            const std::string &on = conv.driverChannels[ci].ownerName;
            byOwner[on.empty() ? std::string("_") : on].push_back(ci);
        }
        const unsigned baseAnim = out->mNumAnimations;
        aiAnimation **newAnims = new aiAnimation *[baseAnim + 1u];
        for (unsigned i = 0; i < baseAnim; ++i) {
            newAnims[i] = out->mAnimations[i];
        }
        if (out->mAnimations) {
            delete[] out->mAnimations;
        }
        out->mAnimations = newAnims;
        out->mNumAnimations = baseAnim + 1u;
        aiAnimation *anim = new aiAnimation();
        anim->mName.Set("BLEND_drivers");
        anim->mTicksPerSecond = fps > 0.f ? fps : 24.0;
        anim->mNumChannels = 0;
        anim->mChannels = nullptr;
        anim->mDuration = 0.0;
        out->mAnimations[baseAnim] = anim;

        // Optional key buffers for driver generator FCurves.
        size_t nKeyPairs = 0;
        for (const auto &ch : conv.driverChannels) {
            if (!ch.keyTimes.empty()) {
                ++nKeyPairs;
            }
        }
        auto *asc = new aiAnimationSidecar();
        asc->mNumBuffers = static_cast<unsigned>(nKeyPairs * 2u);
        asc->mBuffers = (asc->mNumBuffers > 0) ? new aiSidecarBuffer[asc->mNumBuffers] : nullptr;
        std::ostringstream ajson;
        ajson.setf(std::ios::fixed);
        ajson.precision(9);
        ajson << "{\"schemaVersion\":1,\"source\":{\"format\":\"blend\"},"
              << "\"BLEND_drivers\":{\"owners\":[";
        bool ownerFirst = true;
        unsigned bufIdx = 0;
        size_t keyPairIdx = 0;
        for (const auto &kv : byOwner) {
            if (!ownerFirst) {
                ajson << ',';
            }
            ownerFirst = false;
            const auto &chs = kv.second;
            const auto &first = conv.driverChannels[chs[0]];
            ajson << "{\"idCode\":\"" << BlendJsonEscape(first.ownerIdCode)
                  << "\",\"idName\":\"" << BlendJsonEscape(first.ownerName) << "\",\"drivers\":[";
            for (size_t i = 0; i < chs.size(); ++i) {
                const auto &ch = conv.driverChannels[chs[i]];
                if (i) {
                    ajson << ',';
                }
                ajson << '{';
                if (!ch.rnaPath.empty()) {
                    ajson << "\"rnaPath\":\"" << BlendJsonEscape(ch.rnaPath) << "\",";
                }
                ajson << "\"arrayIndex\":" << ch.arrayIndex << ",\"type\":" << ch.type;
                if (const char *tn = BlendDriverTypeName(ch.type)) {
                    ajson << ",\"typeName\":\"" << tn << '"';
                }
                ajson << ",\"flag\":" << ch.flag;
                if (ch.hasInfluence) {
                    ajson << ",\"influence\":" << ch.influence;
                }
                if (!ch.expression.empty()) {
                    ajson << ",\"expression\":\"" << BlendJsonEscape(ch.expression) << '"';
                }
                if (!ch.variables.empty()) {
                    ajson << ",\"variables\":[";
                    for (size_t vi = 0; vi < ch.variables.size(); ++vi) {
                        const auto &ve = ch.variables[vi];
                        if (vi) {
                            ajson << ',';
                        }
                        ajson << '{';
                        if (!ve.name.empty()) {
                            ajson << "\"name\":\"" << BlendJsonEscape(ve.name) << "\",";
                        }
                        ajson << "\"type\":" << ve.type;
                        if (const char *vn = BlendDriverVarTypeName(ve.type)) {
                            ajson << ",\"typeName\":\"" << vn << '"';
                        }
                        if (ve.flag != 0) {
                            ajson << ",\"flag\":" << ve.flag;
                        }
                        if (!ve.targets.empty()) {
                            ajson << ",\"targets\":[";
                            for (size_t ti = 0; ti < ve.targets.size(); ++ti) {
                                const auto &te = ve.targets[ti];
                                if (ti) {
                                    ajson << ',';
                                }
                                ajson << '{';
                                bool tfirst = true;
                                auto tsep = [&]() {
                                    if (!tfirst) {
                                        ajson << ',';
                                    }
                                    tfirst = false;
                                };
                                if (!te.idCode.empty()) {
                                    tsep();
                                    ajson << "\"idCode\":\"" << BlendJsonEscape(te.idCode) << '"';
                                }
                                if (!te.idName.empty()) {
                                    tsep();
                                    ajson << "\"idName\":\"" << BlendJsonEscape(te.idName) << '"';
                                }
                                if (te.idtype != 0) {
                                    tsep();
                                    ajson << "\"idtype\":" << te.idtype;
                                }
                                if (!te.rnaPath.empty()) {
                                    tsep();
                                    ajson << "\"rnaPath\":\"" << BlendJsonEscape(te.rnaPath) << '"';
                                }
                                if (!te.bone.empty()) {
                                    tsep();
                                    ajson << "\"bone\":\"" << BlendJsonEscape(te.bone) << '"';
                                }
                                tsep();
                                ajson << "\"transformChannel\":" << te.transChan;
                                if (const char *cn = BlendDriverTransChanName(te.transChan)) {
                                    ajson << ",\"transformChannelName\":\"" << cn << '"';
                                }
                                if (te.flag != 0) {
                                    ajson << ",\"flag\":" << te.flag;
                                }
                                if (te.hasOptions) {
                                    ajson << ",\"options\":" << te.options;
                                }
                                if (te.hasFallback) {
                                    ajson << ",\"fallbackValue\":" << te.fallbackValue;
                                }
                                if (te.hasContextProperty) {
                                    ajson << ",\"contextProperty\":" << te.contextProperty;
                                }
                                if (te.hasRotationMode) {
                                    ajson << ",\"rotationMode\":" << static_cast<int>(te.rotationMode);
                                }
                                ajson << '}';
                            }
                            ajson << ']';
                        }
                        ajson << '}';
                    }
                    ajson << ']';
                }
                if (!ch.keyTimes.empty()) {
                    const std::string tName = "drv" + std::to_string(keyPairIdx) + "_times";
                    const std::string vName = "drv" + std::to_string(keyPairIdx) + "_values";
                    fillFloatBuf(asc->mBuffers[bufIdx], tName, ch.keyTimes);
                    fillFloatBuf(asc->mBuffers[bufIdx + 1u], vName, ch.keyValues);
                    bufIdx += 2u;
                    ++keyPairIdx;
                    ajson << ",\"keyInputBuffer\":\"" << tName << "\",\"keyOutputBuffer\":\"" << vName << '"';
                }
                ajson << '}';
            }
            ajson << "]}";
        }
        ajson << "]}}";
        asc->mExtensionsJson = BlendDupJson(ajson.str(), asc->mExtensionsJsonLength);
        AttachAnimationSidecar(out, baseAnim, asc);
    }

    std::ostringstream json;
    json.setf(std::ios::fixed);
    json.precision(9);
    json << "{\"schemaVersion\":1,\"source\":{"
         << "\"format\":\"blend\"";
    if (blenderVersion != nullptr && blenderVersion[0] != '\0') {
        json << ",\"generator\":\"Blender " << blenderVersion << '"';
        out->mMetaData->Add(AI_METADATA_SOURCE_FORMAT_VERSION, aiString(blenderVersion));
        std::string generator = std::string("Blender ") + blenderVersion;
        out->mMetaData->Add(AI_METADATA_SOURCE_GENERATOR, aiString(generator));
    }
    json << ",\"formatVersion\":" << file.file_format_version
         << ",\"blenderFileVersion\":" << file.blender_file_version
         << ",\"metersPerUnit\":" << metersPerUnit
         << ",\"timeCodesPerSecond\":" << fps
         << ",\"framesPerSecond\":" << fps
         << ",\"startTimeCode\":" << static_cast<double>(in.r.sfra)
         << ",\"endTimeCode\":" << static_cast<double>(in.r.efra)
         << '}';
    // Optional BLEND_file bag for host logs (container honesty)
    json << ",\"BLEND_file\":{"
         << "\"formatVersion\":" << file.file_format_version
         << ",\"blenderFileVersion\":" << file.blender_file_version
         << '}';

    // Scene DNA already loaded: active camera / world (Presence - no invent).
    const char *sceneCameraName = nullptr;
    int sceneCameraIndex = -1;
    if (in.camera && in.camera->id.name[0] != '\0') {
        sceneCameraName = in.camera->id.name + 2;
        for (unsigned int ci = 0; ci < out->mNumCameras; ++ci) {
            if (std::strcmp(out->mCameras[ci]->mName.C_Str(), sceneCameraName) == 0) {
                sceneCameraIndex = static_cast<int>(ci);
                break;
            }
        }
    }
    json << ",\"BLEND_scene\":{";
    {
        bool sfirst = true;
        auto ssep = [&]() {
            if (!sfirst) {
                json << ',';
            }
            sfirst = false;
        };
        if (in.id.name[0] != '\0') {
            ssep();
            json << "\"name\":\"" << BlendJsonEscape(in.id.name + 2) << '"';
        }
        if (sceneCameraName != nullptr) {
            ssep();
            json << "\"camera\":\"" << BlendJsonEscape(sceneCameraName) << '"';
            if (sceneCameraIndex >= 0) {
                ssep();
                json << "\"cameraIndex\":" << sceneCameraIndex;
            }
        }
        if (in.world && in.world->id.name[0] != '\0') {
            ssep();
            json << "\"world\":\"" << BlendJsonEscape(in.world->id.name + 2) << '"';
        }
        if (!conv.worldEnvJson.empty()) {
            ssep();
            json << "\"worldEnv\":" << conv.worldEnvJson;
        }
    }
    json << '}';

    // S4 cameras / lights (typed already; bag for host inject inventory)
    json << ",\"cameras\":[";
    for (unsigned int ci = 0; ci < out->mNumCameras; ++ci) {
        if (ci) {
            json << ',';
        }
        const char *cname = out->mCameras[ci]->mName.C_Str();
        json << "{\"index\":" << ci << ",\"name\":\"" << BlendJsonEscape(cname) << '"';
        if (sceneCameraName != nullptr && std::strcmp(cname, sceneCameraName) == 0) {
            json << ",\"sceneCamera\":true";
        }
        const auto itEx = conv.cameraSidecarExtras.find(cname);
        if (itEx != conv.cameraSidecarExtras.end() && !itEx->second.empty()) {
            json << ',' << itEx->second;
        }
        json << '}';
    }
    json << "],\"lights\":[";
    for (unsigned int li = 0; li < out->mNumLights; ++li) {
        if (li) {
            json << ',';
        }
        json << "{\"index\":" << li << ",\"name\":\"" << BlendJsonEscape(out->mLights[li]->mName.C_Str()) << "\"}";
    }
    json << "],\"volumes\":[";
    for (size_t vi = 0; vi < conv.volumeJsonEntries.size(); ++vi) {
        if (vi) {
            json << ',';
        }
        json << conv.volumeJsonEntries[vi];
    }
    // C13 detail lives in Physics bag (heap). Scene JSON is heap via aiSceneSidecar
    // (not aiString / AI_MAXLEN - Cityscapes volumes[] + worldEnv exceed 1023).
    json << "],\"pointInstancers\":[";
    if (!conv.pointInstancers.empty()) {
        json << "{\"count\":" << conv.pointInstancers.size()
             << ",\"bag\":\"BLEND_point_instancers\"}";
    }
    json << "],\"composition\":{},\"skipped\":[";
    for (size_t si = 0; si < conv.skippedCategories.size(); ++si) {
        if (si) {
            json << ',';
        }
        json << '"' << BlendJsonEscape(conv.skippedCategories[si]) << '"';
    }
    json << "]}";

    {
        auto *ssc = new aiSceneSidecar();
        ssc->mExtensionsJson = BlendDupJson(json.str(), ssc->mExtensionsJsonLength);
        AttachSceneSidecar(out, ssc);
    }
    out->mMetaData->Add(AI_METADATA_SOURCE_FORMAT, aiString("blend"));
    // Presence: DNA AssetMetaData on Scene ID (not Text datablocks).
    BlendEmitAssetBrowserMetadata(out->mMetaData, in.id.asset_data.get(), true);
    const bool flag = true;
    if (aiSceneHasMeshSidecar(out)) {
        out->mMetaData->Add(AI_METADATA_MESH_SIDECAR, flag);
    }
    if (aiSceneHasMaterialSidecar(out)) {
        out->mMetaData->Add(AI_METADATA_MATERIAL_SIDECAR, flag);
    }
    if (aiSceneHasPhysicsSidecar(out)) {
        out->mMetaData->Add(AI_METADATA_PHYSICS_SIDECAR, flag);
    }
    if (aiSceneHasSceneSidecar(out)) {
        out->mMetaData->Add(AI_METADATA_SCENE_SIDECAR, flag);
    }
}

// ------------------------------------------------------------------------------------------------
/** BI DNA eMTex_MapTo bit -> Assimp type + usage (multi-bit: one emit per influence). */
struct BlendMTexMapHit {
    aiTextureType type{ aiTextureType_UNKNOWN };
    const char *usage{ "unmapped" };
    bool bumpScale{ false };
};

static void BlendCollectMTexMapHits(const MTex *tex, std::vector<BlendMTexMapHit> &out) {
    out.clear();
    if (tex == nullptr) {
        return;
    }
    const int map = static_cast<int>(tex->mapto);
    auto add = [&](int bit, aiTextureType type, const char *usage, bool bump = false) {
        if ((map & bit) == 0) {
            return;
        }
        BlendMTexMapHit h;
        h.type = type;
        h.usage = usage;
        h.bumpScale = bump;
        out.push_back(h);
    };
    // Blender 2.79 RNA labels (DNA_material_types.h MAP_*).
    add(MTex::MapType_COL, aiTextureType_DIFFUSE, "diffuse");
    if ((map & MTex::MapType_NORM) != 0) {
        BlendMTexMapHit h;
        const bool normalmap = (tex->tex && (tex->tex->imaflag & Tex::ImageFlags_NORMALMAP) != 0);
        h.type = normalmap ? aiTextureType_NORMALS : aiTextureType_HEIGHT;
        h.usage = normalmap ? "normal" : "height";
        h.bumpScale = true;
        out.push_back(h);
    }
    add(MTex::MapType_COLSPEC, aiTextureType_SPECULAR, "specularColor");
    add(MTex::MapType_COLMIR, aiTextureType_REFLECTION, "mirrorColor");
    // Diffuse reflectivity intensity - no dedicated Assimp type.
    add(MTex::MapType_REF, aiTextureType_UNKNOWN, "diffuseReflectivity");
    // Specular intensity (not hardness).
    add(MTex::MapType_SPEC, aiTextureType_SPECULAR, "specular");
    add(MTex::MapType_EMIT, aiTextureType_EMISSIVE, "emission");
    add(MTex::MapType_ALPHA, aiTextureType_OPACITY, "opacity");
    // Hardness = shininess.
    add(MTex::MapType_HAR, aiTextureType_SHININESS, "hardness");
    add(MTex::MapType_RAYMIRR, aiTextureType_REFLECTION, "rayMirror");
    add(MTex::MapType_TRANSLU, aiTextureType_TRANSMISSION, "translucency");
    add(MTex::MapType_AMB, aiTextureType_AMBIENT, "ambient");
    add(MTex::MapType_DISPLACE, aiTextureType_DISPLACEMENT, "displacement");
    add(MTex::MapType_WARP, aiTextureType_UNKNOWN, "warp");
    if (out.empty()) {
        BlendMTexMapHit h;
        h.type = aiTextureType_UNKNOWN;
        h.usage = "unmapped";
        out.push_back(h);
    }
}

void BlenderImporter::ResolveImage(aiMaterial *out, const Material *mat, const MTex *tex, const Image *img,
        ConversionData &conv_data, int mtexSlot) {
    aiString name;
    if (!BlendEnsureEmbeddedImage(img, conv_data, name)) {
        return;
    }

    std::vector<BlendMTexMapHit> hits;
    BlendCollectMTexMapHits(tex, hits);

    const unsigned matIndex = BlendMaterialRawIndex(conv_data, mat);
    const int uvIndex = BlendFindUvIndex(conv_data, matIndex, tex->uvname);
    const int mapBits = static_cast<int>(tex->mapto);
    bool bumpEmitted = false;

    auto emitOne = [&](aiTextureType texture_type, const char *usage) {
        const unsigned slot = conv_data.next_texture[texture_type]++;
        out->AddProperty(&name, AI_MATKEY_TEXTURE(texture_type, slot));
        BlendEmitUvWsrc(out, texture_type, slot, uvIndex);
        BlendEmitUvTransformFromMTex(out, texture_type, slot, *tex);
        BlendEmitTexUsageMeta(out, texture_type, slot, usage, mapBits, true, tex->uvname);
        if (mtexSlot >= 0) {
            out->AddProperty(&mtexSlot, 1, "$tex.blend.mtexSlot", texture_type, slot);
        }
        // Mesh binding hint: material raw index used by aiMesh::mMaterialIndex.
        const int mi = static_cast<int>(matIndex);
        out->AddProperty(&mi, 1, "$tex.blend.matIndex", texture_type, slot);
    };

    for (const BlendMTexMapHit &h : hits) {
        emitOne(h.type, h.usage);
        if (h.bumpScale && !bumpEmitted) {
            out->AddProperty(&tex->norfac, 1, AI_MATKEY_BUMPSCALING);
            bumpEmitted = true;
        }
        // Typed PBR slot: BI COL maps are also BASE_COLOR for Host Looks.
        if (h.type == aiTextureType_DIFFUSE && (mapBits & MTex::MapType_COL) != 0) {
            emitOne(aiTextureType_BASE_COLOR, "baseColor");
        }
    }
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::ResolveTexture(aiMaterial *out, const Material *mat, const MTex *tex, ConversionData &conv_data,
        int mtexSlot) {
    const Tex *rtex = tex->tex.get();
    if (!rtex || !rtex->type) {
        return;
    }

    // Only Type_IMAGE becomes a real $tex.file / embedded aiTexture.
    // Procedural BI types (EnvMap, Clouds, ...) are not inventable paths (presence):
    // skip the slot and record honesty in skipped[].
    switch (rtex->type) {
    case Tex::Type_CLOUDS:
    case Tex::Type_WOOD:
    case Tex::Type_MARBLE:
    case Tex::Type_MAGIC:
    case Tex::Type_BLEND:
    case Tex::Type_STUCCI:
    case Tex::Type_NOISE:
    case Tex::Type_PLUGIN:
    case Tex::Type_MUSGRAVE:
    case Tex::Type_VORONOI:
    case Tex::Type_DISTNOISE:
    case Tex::Type_ENVMAP:
    case Tex::Type_POINTDENSITY:
    case Tex::Type_VOXELDATA: {
        const char *disp = GetTextureTypeDisplayString(rtex->type);
        LogWarn("BLEND: skipping procedural texture type ", disp,
                " (no Image datablock; not inventing $tex.file)");
        std::string sk = "BLEND_texture_procedural_";
        sk += disp;
        conv_data.skippedCategories.push_back(sk);
        break;
    }

    case Tex::Type_IMAGE:
        if (!rtex->ima) {
            LogError("A texture claims to be an Image, but no image reference is given");
            break;
        }
        ResolveImage(out, mat, tex, rtex->ima.get(), conv_data, mtexSlot);
        break;

    default:
        ai_assert(false);
    };
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::BuildDefaultMaterial(Blender::ConversionData &conv_data) {
    // Assimp DefaultMaterial policy (BaseImporter / ScenePreprocessor):
    // name AI_DEFAULT_MATERIAL_NAME + diffuse (0.6,0.6,0.6). Needed here because
    // ScenePreprocessor only injects when mNumMaterials==0; Blender often has other
    // mats while some meshes still lack a slot.
    unsigned int index = static_cast<unsigned int>(-1);
    for (aiMesh *mesh : conv_data.meshes.get()) {
        if (mesh->mMaterialIndex == static_cast<unsigned int>(-1)) {

            if (index == static_cast<unsigned int>(-1)) {
                std::shared_ptr<Material> p(new Material());
                const size_t len = ::strlen(AI_DEFAULT_MATERIAL_NAME);
                ai_assert(len < sizeof(p->id.name) - 2);
                memcpy(p->id.name + 2, AI_DEFAULT_MATERIAL_NAME, len);

                // Note: MSVC11 does not zero-initialize Material here, although it should.
                // Marker stub only - BuildMaterials emits Assimp-minimal keys (no blend.* invent).
                p->r = p->g = p->b = 0.6f;

                index = static_cast<unsigned int>(conv_data.materials_raw.size());
                conv_data.materials_raw.push_back(p);
                LogInfo("Adding Assimp default material '", AI_DEFAULT_MATERIAL_NAME, "'");
            }
            mesh->mMaterialIndex = index;
        }
    }
}

void BlenderImporter::AddBlendParams(aiMaterial *result, const Material *source) {
    aiColor3D diffuseColor(source->r, source->g, source->b);
    result->AddProperty(&diffuseColor, 1, "$mat.blend.diffuse.color", 0, 0);

    float diffuseIntensity = source->ref;
    result->AddProperty(&diffuseIntensity, 1, "$mat.blend.diffuse.intensity", 0, 0);

    int diffuseShader = source->diff_shader;
    result->AddProperty(&diffuseShader, 1, "$mat.blend.diffuse.shader", 0, 0);

    // No invent for missing DNA ramp fields (presence).

    aiColor3D specularColor(source->specr, source->specg, source->specb);
    result->AddProperty(&specularColor, 1, "$mat.blend.specular.color", 0, 0);

    float specularIntensity = source->spec;
    result->AddProperty(&specularIntensity, 1, "$mat.blend.specular.intensity", 0, 0);

    int specularShader = source->spec_shader;
    result->AddProperty(&specularShader, 1, "$mat.blend.specular.shader", 0, 0);

    int specularHardness = source->har;
    result->AddProperty(&specularHardness, 1, "$mat.blend.specular.hardness", 0, 0);

    int transparencyUse = source->mode & MA_TRANSPARENCY ? 1 : 0;
    result->AddProperty(&transparencyUse, 1, "$mat.blend.transparency.use", 0, 0);

    int transparencyMethod = source->mode & MA_RAYTRANSP ? 2 : (source->mode & MA_ZTRANSP ? 1 : 0);
    result->AddProperty(&transparencyMethod, 1, "$mat.blend.transparency.method", 0, 0);

    float transparencyAlpha = source->alpha;
    result->AddProperty(&transparencyAlpha, 1, "$mat.blend.transparency.alpha", 0, 0);

    float transparencySpecular = source->spectra;
    result->AddProperty(&transparencySpecular, 1, "$mat.blend.transparency.specular", 0, 0);

    float transparencyFresnel = source->fresnel_tra;
    result->AddProperty(&transparencyFresnel, 1, "$mat.blend.transparency.fresnel", 0, 0);

    float transparencyBlend = source->fresnel_tra_i;
    result->AddProperty(&transparencyBlend, 1, "$mat.blend.transparency.blend", 0, 0);

    float transparencyIor = source->ang;
    result->AddProperty(&transparencyIor, 1, "$mat.blend.transparency.ior", 0, 0);

    float transparencyFilter = source->filter;
    result->AddProperty(&transparencyFilter, 1, "$mat.blend.transparency.filter", 0, 0);

    float transparencyFalloff = source->tx_falloff;
    result->AddProperty(&transparencyFalloff, 1, "$mat.blend.transparency.falloff", 0, 0);

    float transparencyLimit = source->tx_limit;
    result->AddProperty(&transparencyLimit, 1, "$mat.blend.transparency.limit", 0, 0);

    int transparencyDepth = source->ray_depth_tra;
    result->AddProperty(&transparencyDepth, 1, "$mat.blend.transparency.depth", 0, 0);

    float transparencyGlossAmount = source->gloss_tra;
    result->AddProperty(&transparencyGlossAmount, 1, "$mat.blend.transparency.glossAmount", 0, 0);

    float transparencyGlossThreshold = source->adapt_thresh_tra;
    result->AddProperty(&transparencyGlossThreshold, 1, "$mat.blend.transparency.glossThreshold", 0, 0);

    int transparencyGlossSamples = source->samp_gloss_tra;
    result->AddProperty(&transparencyGlossSamples, 1, "$mat.blend.transparency.glossSamples", 0, 0);

    int mirrorUse = source->mode & MA_RAYMIRROR ? 1 : 0;
    result->AddProperty(&mirrorUse, 1, "$mat.blend.mirror.use", 0, 0);

    float mirrorReflectivity = source->ray_mirror;
    result->AddProperty(&mirrorReflectivity, 1, "$mat.blend.mirror.reflectivity", 0, 0);

    aiColor3D mirrorColor(source->mirr, source->mirg, source->mirb);
    result->AddProperty(&mirrorColor, 1, "$mat.blend.mirror.color", 0, 0);

    float mirrorFresnel = source->fresnel_mir;
    result->AddProperty(&mirrorFresnel, 1, "$mat.blend.mirror.fresnel", 0, 0);

    float mirrorBlend = source->fresnel_mir_i;
    result->AddProperty(&mirrorBlend, 1, "$mat.blend.mirror.blend", 0, 0);

    int mirrorDepth = source->ray_depth;
    result->AddProperty(&mirrorDepth, 1, "$mat.blend.mirror.depth", 0, 0);

    float mirrorMaxDist = source->dist_mir;
    result->AddProperty(&mirrorMaxDist, 1, "$mat.blend.mirror.maxDist", 0, 0);

    int mirrorFadeTo = source->fadeto_mir;
    result->AddProperty(&mirrorFadeTo, 1, "$mat.blend.mirror.fadeTo", 0, 0);

    float mirrorGlossAmount = source->gloss_mir;
    result->AddProperty(&mirrorGlossAmount, 1, "$mat.blend.mirror.glossAmount", 0, 0);

    float mirrorGlossThreshold = source->adapt_thresh_mir;
    result->AddProperty(&mirrorGlossThreshold, 1, "$mat.blend.mirror.glossThreshold", 0, 0);

    int mirrorGlossSamples = source->samp_gloss_mir;
    result->AddProperty(&mirrorGlossSamples, 1, "$mat.blend.mirror.glossSamples", 0, 0);

    float mirrorGlossAnisotropic = source->aniso_gloss_mir;
    result->AddProperty(&mirrorGlossAnisotropic, 1, "$mat.blend.mirror.glossAnisotropic", 0, 0);
}

void BlenderImporter::BuildMaterials(ConversionData &conv_data) {
    conv_data.materials->reserve(conv_data.materials_raw.size());
    conv_data.materialSidecarJson.reserve(conv_data.materials_raw.size());

    BuildDefaultMaterial(conv_data);

    for (size_t matRawIndex = 0; matRawIndex < conv_data.materials_raw.size(); ++matRawIndex) {
        const std::shared_ptr<Material> &mat = conv_data.materials_raw[matRawIndex];
        // Sparse mesh->mat slots can push empty shared_ptrs; skip them.
        if (!mat) {
            continue;
        }

        // reset per material global counters
        for (size_t i = 0; i < sizeof(conv_data.next_texture) / sizeof(conv_data.next_texture[0]); ++i) {
            conv_data.next_texture[i] = 0;
        }

        aiMaterial *mout = new aiMaterial();
        conv_data.materials->push_back(mout);

        // set material name
        aiString name = aiString(mat->id.name + 2); // skip over the name prefix 'MA'
        mout->AddProperty(&name, AI_MATKEY_NAME);

        // Assimp DefaultMaterial policy (ScenePreprocessor): name + diffuse 0.6 only.
        // Do not invent $mat.blend.* / Ka / Ks / procedural $tex.file for this placeholder.
        if (std::strcmp(mat->id.name + 2, AI_DEFAULT_MATERIAL_NAME) == 0) {
            aiColor3D clr(0.6f, 0.6f, 0.6f);
            mout->AddProperty(&clr, 1, AI_MATKEY_COLOR_DIFFUSE);
            conv_data.materialSidecarJson.emplace_back();
            continue;
        }

        // C6: flatten + inventory + shader network share one BlendNodeGraph (identical emit).
        BlendNodeGraph materialGraph;
        const BlendNodeGraph *materialGraphPtr = nullptr;
        if (mat->nodetree) {
            materialGraph = BlendBuildGraph(*mat->nodetree);
            materialGraphPtr = &materialGraph;
        }
        const BlendShaderFlat flat = materialGraphPtr != nullptr ?
                                             BlendFlattenMaterialNodes(*mat, *materialGraphPtr) :
                                             BlendFlattenMaterialNodes(*mat);
        if (flat.ok) {
            BlendApplyFlatToMaterial(mout, flat, conv_data, static_cast<unsigned>(matRawIndex));
        } else {
            // basic material colors (Blender Internal / no flattenable nodes)
            aiColor3D col(mat->r, mat->g, mat->b);
            if (mat->r || mat->g || mat->b) {

                // Usually, zero diffuse color means no diffuse color at all in the equation.
                // So we omit this member to express this intent.
                mout->AddProperty(&col, 1, AI_MATKEY_COLOR_DIFFUSE);

                if (mat->emit) {
                    aiColor3D emit_col(mat->emit * mat->r, mat->emit * mat->g, mat->emit * mat->b);
                    mout->AddProperty(&emit_col, 1, AI_MATKEY_COLOR_EMISSIVE);
                }
            }

            col = aiColor3D(mat->specr, mat->specg, mat->specb);
            mout->AddProperty(&col, 1, AI_MATKEY_COLOR_SPECULAR);

            // is hardness/shininess set?
            if (mat->har) {
                const float har = mat->har;
                mout->AddProperty(&har, 1, AI_MATKEY_SHININESS);
            }

            // Modern DNA metallic/roughness on Material itself (EEVEE) when no nodes.
            if (mat->metallic > 0.f) {
                mout->AddProperty(&mat->metallic, 1, AI_MATKEY_METALLIC_FACTOR);
            }
            if (mat->roughness > 0.f) {
                mout->AddProperty(&mat->roughness, 1, AI_MATKEY_ROUGHNESS_FACTOR);
            }
        }

        aiColor3D col = aiColor3D(mat->ambr, mat->ambg, mat->ambb);
        mout->AddProperty(&col, 1, AI_MATKEY_COLOR_AMBIENT);

        // is mirror enabled?
        if (mat->mode & MA_RAYMIRROR) {
            const float ray_mirror = mat->ray_mirror;
            mout->AddProperty(&ray_mirror, 1, AI_MATKEY_REFLECTIVITY);
        }

        col = aiColor3D(mat->mirr, mat->mirg, mat->mirb);
        mout->AddProperty(&col, 1, AI_MATKEY_COLOR_REFLECTIVE);

        for (size_t i = 0; i < sizeof(mat->mtex) / sizeof(mat->mtex[0]); ++i) {
            if (!mat->mtex[i]) {
                continue;
            }

            ResolveTexture(mout, mat.get(), mat->mtex[i].get(), conv_data, static_cast<int>(i));
        }

        // Grease Pencil materials: DNA Material.gp_style -> fill/stroke Images (often packed).
        if (mat->gp_style) {
            const unsigned matIndex = static_cast<unsigned>(matRawIndex);
            BlendTexMap gpMap;
            if (mat->gp_style->ima) {
                gpMap.img = mat->gp_style->ima.get();
                BlendAttachNodeImage(mout, gpMap, aiTextureType_BASE_COLOR, conv_data, matIndex, "baseColor");
                BlendAttachNodeImage(mout, gpMap, aiTextureType_DIFFUSE, conv_data, matIndex, "baseColor");
            }
            if (mat->gp_style->sima) {
                gpMap.img = mat->gp_style->sima.get();
                BlendAttachNodeImage(mout, gpMap, aiTextureType_DIFFUSE, conv_data, matIndex);
            }
        }

        AddBlendParams(mout, mat.get());
        std::vector<BlendMapInventoryEntry> inventory;
        if (materialGraphPtr != nullptr) {
            BlendCollectTexImageInventory(*materialGraphPtr, *materialGraphPtr, inventory);
            // Linked inventory maps -> typed $tex.* + usage meta; drop when fully typed.
            BlendEmitInventoryTypedMaps(mout, flat, inventory, conv_data,
                    static_cast<unsigned>(matRawIndex), *materialGraphPtr);
        }
        conv_data.materialSidecarJson.push_back(
                BlendBuildMaterialSidecarJson(*mat, flat, conv_data, inventory, materialGraphPtr));
        // Typed Presence gate for HostEval (mirrors bag eval / hostProcedural).
        if (materialGraphPtr != nullptr) {
            BlendEmitShaderNetworkHostEvalMatKeys(mout, *materialGraphPtr);
        }
        if (flat.hasVolumeColor || flat.hasVolumeDensity || flat.hasVolumeDensityDefault ||
                flat.hasVolumeDensityLinked || flat.hasVolumeDensityAttribute || flat.hasVolumeAnisotropy ||
                flat.hasVolumeAbsorptionColor || flat.hasVolumeEmissionColor ||
                flat.hasVolumeEmissionStrength) {
            if (conv_data.materialHasPrincipledVolume.size() < conv_data.materials_raw.size()) {
                conv_data.materialHasPrincipledVolume.resize(conv_data.materials_raw.size(), 0);
            }
            const unsigned mi = static_cast<unsigned>(conv_data.materials->size() - 1u);
            if (mi < conv_data.materialHasPrincipledVolume.size()) {
                conv_data.materialHasPrincipledVolume[mi] = 1;
            }
            std::ostringstream vj;
            vj << "{\"materialIndex\":" << mi
               << ",\"name\":\"" << BlendJsonEscape(mat->id.name + 2)
               << "\",\"source\":\"Principled Volume\"";
            if (flat.hasVolumeDensity) {
                vj << ",\"density\":" << flat.volumeDensity;
            }
            if (flat.hasVolumeDensityLinked) {
                vj << ",\"densityLinked\":true";
            }
            if (flat.hasVolumeDensityDefault) {
                vj << ",\"densityDefault\":" << flat.volumeDensityDefault;
            }
            if (flat.hasVolumeColor) {
                vj << ",\"color\":[" << flat.volumeColor[0] << ',' << flat.volumeColor[1] << ','
                   << flat.volumeColor[2] << ',' << flat.volumeColor[3] << ']';
            }
            if (flat.hasVolumeDensityAttribute) {
                vj << ",\"densityAttribute\":\"" << BlendJsonEscape(flat.volumeDensityAttribute) << '"';
            }
            if (flat.hasVolumeAnisotropy) {
                vj << ",\"anisotropy\":" << flat.volumeAnisotropy;
            }
            if (flat.hasVolumeAbsorptionColor) {
                vj << ",\"absorptionColor\":[" << flat.volumeAbsorptionColor[0] << ','
                   << flat.volumeAbsorptionColor[1] << ',' << flat.volumeAbsorptionColor[2] << ','
                   << flat.volumeAbsorptionColor[3] << ']';
            }
            if (flat.hasVolumeEmissionStrength) {
                vj << ",\"emissionStrength\":" << flat.volumeEmissionStrength;
            }
            if (flat.hasVolumeEmissionColor) {
                vj << ",\"emissionColor\":[" << flat.volumeEmissionColor[0] << ','
                   << flat.volumeEmissionColor[1] << ',' << flat.volumeEmissionColor[2] << ','
                   << flat.volumeEmissionColor[3] << ']';
            }
            vj << '}';
            conv_data.volumeJsonEntries.push_back(vj.str());
        }
        if (mat->nodetree && !flat.ok) {
            conv_data.skippedCategories.push_back("BLEND_shader_nodegraph");
        }
    }
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::CheckActualType(const ElemBase *dt, const char *check) {
    ai_assert(dt);
    if (!BlendElemConvertedHullIs(*dt, check)) {
        const char *have = dt->converted_hull != nullptr ? dt->converted_hull : dt->dna_type;
        ThrowException("Expected object at ", std::hex, dt, " converted hull `", check,
                "`, but have `", have ? have : "?", "` instead");
    }
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::NotSupportedObjectType(const Object *obj, const char *type) {
    LogWarn("Object `", obj->id.name, "` - type is unsupported: `", type, "`, skipping");
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::ConvertMesh(const Scene & /*in*/, const Object * /*obj*/, const Mesh *mesh,
        ConversionData &conv_data, TempArray<std::vector, aiMesh> &temp) {
    // TODO: Resolve various problems with BMesh triangulation before re-enabling.
    //       See issues #400, #373, #318  #315 and #132.
#if defined(TODO_FIX_BMESH_CONVERSION)
    BlenderBMeshConverter BMeshConverter(mesh);
    if (BMeshConverter.ContainsBMesh()) {
        mesh = BMeshConverter.TriangulateBMesh();
    }
#endif

    typedef std::pair<const int, size_t> MyPair;
    if ((!mesh->totface && !mesh->totloop) || !mesh->totvert) {
        return;
    }

    // some sanity checks
    if (static_cast<size_t>(mesh->totface) > mesh->mface.size()) {
        ThrowException("Number of faces is larger than the corresponding array");
    }

    // Classic *mvert/*mloop may be empty depending on Blender file version:
    // CustomData generic attrs (~2.90-4.x) or AttributeStorage (~4.5+/5.x).
    if (static_cast<size_t>(mesh->totvert) > mesh->mvert.size() ||
            (mesh->totloop > 0 && static_cast<size_t>(mesh->totloop) > mesh->mloop.size()) ||
            (mesh->totpoly > 0 && static_cast<size_t>(mesh->totpoly) > mesh->mpoly.size())) {
        Mesh *mutableMesh = const_cast<Mesh *>(mesh);
        if (!HydrateMeshGeometry(*mutableMesh, conv_data.db)) {
            LogWarn("mesh '", mesh->id.name + 2,
                    "' has no classic arrays and geometry hydrate failed "
                    "(file_version=",
                    conv_data.db.blender_file_version,
                    " totvert=", mesh->totvert, " mvert=", mesh->mvert.size(),
                    " totloop=", mesh->totloop, " mloop=", mesh->mloop.size(), ")");
            return;
        }
        LogInfo("hydrated mesh '", mesh->id.name + 2,
                "' (file_version=", conv_data.db.blender_file_version,
                " verts=", mesh->mvert.size(),
                " loops=", mesh->mloop.size(), " polys=", mesh->mpoly.size(), ")");
    }
    if (static_cast<size_t>(mesh->totvert) > mesh->mvert.size() ||
            (mesh->totloop > 0 && static_cast<size_t>(mesh->totloop) > mesh->mloop.size())) {
        return;
    }

    // collect per-submesh numbers
    std::map<int, size_t> per_mat;
    std::map<int, size_t> per_mat_verts;
    for (int i = 0; i < mesh->totface; ++i) {

        const MFace &mf = mesh->mface[i];
        per_mat[mf.mat_nr]++;
        per_mat_verts[mf.mat_nr] += mf.v4 ? 4 : 3;
    }

    for (int i = 0; i < mesh->totpoly; ++i) {
        const MPoly &mp = mesh->mpoly[i];
        per_mat[mp.mat_nr]++;
        per_mat_verts[mp.mat_nr] += mp.totloop;
    }

    // ... and allocate the corresponding meshes
    const size_t old = temp->size();
    temp->reserve(temp->size() + per_mat.size());

    std::map<size_t, size_t> mat_num_to_mesh_idx;
    // Per sub-mesh: Assimp vertex order -> DNA totvert index (UV-split duplicates share iDna).
    std::vector<std::vector<int32_t>> srcVertLocal;
    srcVertLocal.resize(temp->size()); // preserve prior meshes' slots; new ones push below
    for (MyPair &it : per_mat) {

        mat_num_to_mesh_idx[it.first] = temp->size();
        temp->push_back(new aiMesh());
        srcVertLocal.resize(temp->size());
        srcVertLocal.back().reserve(per_mat_verts[it.first]);

        aiMesh *out = temp->back();
        out->mVertices = new aiVector3D[per_mat_verts[it.first]];
        out->mNormals = new aiVector3D[per_mat_verts[it.first]];

        // out->mNumFaces = 0
        // out->mNumVertices = 0
        out->mFaces = new aiFace[it.second]();

        // all sub-meshes created from this mesh are named equally. this allows
        // curious users to recover the original adjacency.
        out->mName = aiString(mesh->id.name + 2);
        // skip over the name prefix 'ME'

        // resolve the material reference and add this material to the set of
        // output materials. The (temporary) material index is the index
        // of the material entry within the list of resolved materials.
        if (mesh->mat) {

            std::shared_ptr<Material> mat;
            if (it.first >= 0 && static_cast<size_t>(it.first) < mesh->mat.size()) {
                mat = mesh->mat[it.first];
            } else {
                LogWarn("mesh '", mesh->id.name + 2,
                        "' material index ", it.first, " out of range (mat.size=",
                        mesh->mat.size(), "); using default");
            }
            if (!mat) {
                out->mMaterialIndex = static_cast<unsigned int>(-1);
            } else {
                const std::deque<std::shared_ptr<Material>>::iterator has = std::find(
                        conv_data.materials_raw.begin(),
                        conv_data.materials_raw.end(), mat);

                if (has != conv_data.materials_raw.end()) {
                    out->mMaterialIndex = static_cast<unsigned int>(std::distance(conv_data.materials_raw.begin(), has));
                } else {
                    out->mMaterialIndex = static_cast<unsigned int>(conv_data.materials_raw.size());
                    conv_data.materials_raw.push_back(mat);
                }
            }
        } else
            out->mMaterialIndex = static_cast<unsigned int>(-1);
    }

    for (int i = 0; i < mesh->totface; ++i) {

        const MFace &mf = mesh->mface[i];

        aiMesh *const out = temp[mat_num_to_mesh_idx[mf.mat_nr]];
        aiFace &f = out->mFaces[out->mNumFaces++];

        f.mIndices = new unsigned int[f.mNumIndices = mf.v4 ? 4 : 3];
        aiVector3D *vo = out->mVertices + out->mNumVertices;
        aiVector3D *vn = out->mNormals + out->mNumVertices;

        // XXX we can't fold this easily, because we are restricted
        // to the member names from the BLEND file (v1,v2,v3,v4)
        // which are assigned by the genblenddna.py script and
        // cannot be changed without breaking the entire
        // import process.

        std::vector<int32_t> &srcVert = srcVertLocal[mat_num_to_mesh_idx[mf.mat_nr]];
        if (mf.v1 >= mesh->totvert) {
            ThrowException("Vertex index v1 out of range");
        }
        const MVert *v = &mesh->mvert[mf.v1];
        vo->x = v->co[0];
        vo->y = v->co[1];
        vo->z = v->co[2];
        vn->x = v->no[0];
        vn->y = v->no[1];
        vn->z = v->no[2];
        srcVert.push_back(mf.v1);
        f.mIndices[0] = out->mNumVertices++;
        ++vo;
        ++vn;

        //  if (f.mNumIndices >= 2) {
        if (mf.v2 >= mesh->totvert) {
            ThrowException("Vertex index v2 out of range");
        }
        v = &mesh->mvert[mf.v2];
        vo->x = v->co[0];
        vo->y = v->co[1];
        vo->z = v->co[2];
        vn->x = v->no[0];
        vn->y = v->no[1];
        vn->z = v->no[2];
        srcVert.push_back(mf.v2);
        f.mIndices[1] = out->mNumVertices++;
        ++vo;
        ++vn;

        if (mf.v3 >= mesh->totvert) {
            ThrowException("Vertex index v3 out of range");
        }
        //  if (f.mNumIndices >= 3) {
        v = &mesh->mvert[mf.v3];
        vo->x = v->co[0];
        vo->y = v->co[1];
        vo->z = v->co[2];
        vn->x = v->no[0];
        vn->y = v->no[1];
        vn->z = v->no[2];
        srcVert.push_back(mf.v3);
        f.mIndices[2] = out->mNumVertices++;
        ++vo;
        ++vn;

        if (mf.v4 >= mesh->totvert) {
            ThrowException("Vertex index v4 out of range");
        }
        //  if (f.mNumIndices >= 4) {
        if (mf.v4) {
            v = &mesh->mvert[mf.v4];
            vo->x = v->co[0];
            vo->y = v->co[1];
            vo->z = v->co[2];
            vn->x = v->no[0];
            vn->y = v->no[1];
            vn->z = v->no[2];
            srcVert.push_back(static_cast<int32_t>(mf.v4));
            f.mIndices[3] = out->mNumVertices++;
            ++vo;
            ++vn;

            out->mPrimitiveTypes |= aiPrimitiveType_POLYGON;
        } else
            out->mPrimitiveTypes |= aiPrimitiveType_TRIANGLE;

        //  }
        //  }
        //  }
    }

    for (int i = 0; i < mesh->totpoly; ++i) {

        const MPoly &mf = mesh->mpoly[i];

        aiMesh *const out = temp[mat_num_to_mesh_idx[mf.mat_nr]];
        aiFace &f = out->mFaces[out->mNumFaces++];

        f.mIndices = new unsigned int[f.mNumIndices = mf.totloop];
        aiVector3D *vo = out->mVertices + out->mNumVertices;
        aiVector3D *vn = out->mNormals + out->mNumVertices;

        // XXX we can't fold this easily, because we are restricted
        // to the member names from the BLEND file (v1,v2,v3,v4)
        // which are assigned by the genblenddna.py script and
        // cannot be changed without breaking the entire
        // import process.
        for (int j = 0; j < mf.totloop; ++j) {
            const MLoop &loop = mesh->mloop[mf.loopstart + j];

            if (loop.v < 0 || loop.v >= mesh->totvert) {
                LogWarn("mesh '", mesh->id.name + 2,
                        "' has out-of-range corner vertex index; skipping mesh");
                while (temp->size() > old) {
                    delete temp->back();
                    temp->pop_back();
                }
                return;
            }

            const MVert &v = mesh->mvert[loop.v];

            vo->x = v.co[0];
            vo->y = v.co[1];
            vo->z = v.co[2];
            vn->x = v.no[0];
            vn->y = v.no[1];
            vn->z = v.no[2];
            srcVertLocal[mat_num_to_mesh_idx[mf.mat_nr]].push_back(loop.v);
            f.mIndices[j] = out->mNumVertices++;

            ++vo;
            ++vn;
        }
        if (mf.totloop == 3) {
            out->mPrimitiveTypes |= aiPrimitiveType_TRIANGLE;
        } else {
            out->mPrimitiveTypes |= aiPrimitiveType_POLYGON;
        }
    }

    // Named UV maps: CustomData CD_MLOOPUV / CD_PROP_FLOAT2 + AttributeStorage.
    std::vector<BlendUVLayer> uvLayers;
    CollectMeshUVLayers(*mesh, conv_data.db, uvLayers, AI_MAX_NUMBER_OF_TEXTURECOORDS);

    // collect texture coordinates, they're stored in a separate per-face buffer
    if (mesh->mtface || !uvLayers.empty() || !mesh->mloopuv.empty()) {
        if (mesh->totface > 0 && mesh->totface > static_cast<int>(mesh->mtface.size()) && mesh->mtface.size() > 0) {
            ThrowException("Number of UV faces is larger than the corresponding UV face array (#1)");
        }
        // If only mtface (no loop UVs), allocate a single channel.
        const unsigned int numUV = !uvLayers.empty() ? static_cast<unsigned int>(uvLayers.size()) : 1u;
        for (std::vector<aiMesh *>::iterator it = temp->begin() + old; it != temp->end(); ++it) {
            ai_assert(0 != (*it)->mNumVertices);
            ai_assert(0 != (*it)->mNumFaces);
            for (unsigned int u = 0; u < numUV; ++u) {
                (*it)->mTextureCoords[u] = new aiVector3D[(*it)->mNumVertices];
                (*it)->mNumUVComponents[u] = 2;
                if (u < uvLayers.size() && !uvLayers[u].name.empty()) {
                    (*it)->SetTextureCoordsName(u, aiString(uvLayers[u].name));
                }
            }
            (*it)->mNumFaces = (*it)->mNumVertices = 0;
        }

        for (int i = 0; i < mesh->totface; ++i) {
            if (mesh->mtface.empty()) {
                break;
            }
            const MTFace *v = &mesh->mtface[i];

            aiMesh *const out = temp[mat_num_to_mesh_idx[mesh->mface[i].mat_nr]];
            const aiFace &f = out->mFaces[out->mNumFaces++];

            aiVector3D *vo = &out->mTextureCoords[0][out->mNumVertices];
            for (unsigned int j = 0; j < f.mNumIndices; ++j, ++vo, ++out->mNumVertices) {
                vo->x = v->uv[j][0];
                vo->y = v->uv[j][1];
            }
        }

        for (int i = 0; i < mesh->totpoly; ++i) {
            const MPoly &v = mesh->mpoly[i];
            aiMesh *const out = temp[mat_num_to_mesh_idx[v.mat_nr]];
            const aiFace &f = out->mFaces[out->mNumFaces++];

            if (!uvLayers.empty()) {
                for (unsigned int u = 0; u < numUV; ++u) {
                    aiVector3D *vo = &out->mTextureCoords[u][out->mNumVertices];
                    const BlendUVLayer &layer = uvLayers[u];
                    for (unsigned int j = 0; j < f.mNumIndices; ++j, ++vo) {
                        const size_t li = static_cast<size_t>(v.loopstart + static_cast<int>(j));
                        vo->x = layer.uv[li * 2 + 0];
                        vo->y = layer.uv[li * 2 + 1];
                        vo->z = 0.f;
                    }
                }
                out->mNumVertices += f.mNumIndices;
            } else if (!mesh->mloopuv.empty()) {
                aiVector3D *vo = &out->mTextureCoords[0][out->mNumVertices];
                for (unsigned int j = 0; j < f.mNumIndices; ++j, ++vo, ++out->mNumVertices) {
                    const MLoopUV &uv = mesh->mloopuv[v.loopstart + j];
                    vo->x = uv.uv[0];
                    vo->y = uv.uv[1];
                }
            } else {
                // mtface-only path already advanced counters in the totface loop;
                // poly without UV data: keep vertex index alignment with positions.
                out->mNumVertices += f.mNumIndices;
            }
        }
    }

    // collect texture coordinates, old-style (marked as deprecated in current blender sources)
    if (mesh->tface) {
        if (mesh->totface > static_cast<int>(mesh->tface.size())) {
            ThrowException("Number of faces is larger than the corresponding UV face array (#2)");
        }
        for (std::vector<aiMesh *>::iterator it = temp->begin() + old; it != temp->end(); ++it) {
            ai_assert(0 != (*it)->mNumVertices);
            ai_assert(0 != (*it)->mNumFaces);

            (*it)->mTextureCoords[0] = new aiVector3D[(*it)->mNumVertices];
            (*it)->mNumFaces = (*it)->mNumVertices = 0;
        }

        for (int i = 0; i < mesh->totface; ++i) {
            const TFace *v = &mesh->tface[i];

            aiMesh *const out = temp[mat_num_to_mesh_idx[mesh->mface[i].mat_nr]];
            const aiFace &f = out->mFaces[out->mNumFaces++];

            aiVector3D *vo = &out->mTextureCoords[0][out->mNumVertices];
            for (unsigned int j = 0; j < f.mNumIndices; ++j, ++vo, ++out->mNumVertices) {
                vo->x = v->uv[j][0];
                vo->y = v->uv[j][1];
            }
        }
    }

    // collect vertex colors, stored separately as well
    if (mesh->mcol || mesh->mloopcol) {
        if (mesh->totface > static_cast<int>((mesh->mcol.size() / 4))) {
            ThrowException("Number of faces is larger than the corresponding color face array");
        }
        for (std::vector<aiMesh *>::iterator it = temp->begin() + old; it != temp->end(); ++it) {
            ai_assert(0 != (*it)->mNumVertices);
            ai_assert(0 != (*it)->mNumFaces);

            (*it)->mColors[0] = new aiColor4D[(*it)->mNumVertices];
            (*it)->mNumFaces = (*it)->mNumVertices = 0;
        }

        for (int i = 0; i < mesh->totface; ++i) {

            aiMesh *const out = temp[mat_num_to_mesh_idx[mesh->mface[i].mat_nr]];
            const aiFace &f = out->mFaces[out->mNumFaces++];

            aiColor4D *vo = &out->mColors[0][out->mNumVertices];
            for (unsigned int n = 0; n < f.mNumIndices; ++n, ++vo, ++out->mNumVertices) {
                const MCol *col = &mesh->mcol[(i << 2) + n];

                vo->r = col->r;
                vo->g = col->g;
                vo->b = col->b;
                vo->a = col->a;
            }
            for (unsigned int n = f.mNumIndices; n < 4; ++n)
                ;
        }

        for (int i = 0; i < mesh->totpoly; ++i) {
            const MPoly &v = mesh->mpoly[i];
            aiMesh *const out = temp[mat_num_to_mesh_idx[v.mat_nr]];
            const aiFace &f = out->mFaces[out->mNumFaces++];

            aiColor4D *vo = &out->mColors[0][out->mNumVertices];
            const ai_real scaleZeroToOne = 1.f / 255.f;
            for (unsigned int j = 0; j < f.mNumIndices; ++j, ++vo, ++out->mNumVertices) {
                const MLoopCol &col = mesh->mloopcol[v.loopstart + j];
                vo->r = ai_real(col.r) * scaleZeroToOne;
                vo->g = ai_real(col.g) * scaleZeroToOne;
                vo->b = ai_real(col.b) * scaleZeroToOne;
                vo->a = ai_real(col.a) * scaleZeroToOne;
            }
        }
    }

    // S1: stash CustomData layer inventory + DNA srcVert map for mesh sidecars.
    {
        const std::string layerJson = BuildBlendCustomDataLayersJson(mesh);
        if (conv_data.meshSidecarJson.size() < temp->size()) {
            conv_data.meshSidecarJson.resize(temp->size());
        }
        if (conv_data.meshSrcVertDna.size() < temp->size()) {
            conv_data.meshSrcVertDna.resize(temp->size());
        }
        for (size_t mi = old; mi < temp->size(); ++mi) {
            const aiMesh *am = temp[mi];
            const std::vector<int32_t> &sv = srcVertLocal[mi];
            std::ostringstream j;
            // Merge BLEND_srcVert Presence into CustomData JSON object.
            const std::string &base = layerJson;
            if (base.size() >= 2 && base.back() == '}') {
                j << base.substr(0, base.size() - 1);
            } else {
                j << "{\"schemaVersion\":1,\"source\":{\"format\":\"blend\"";
            }
            if (!sv.empty() && am != nullptr && sv.size() == static_cast<size_t>(am->mNumVertices)) {
                j << ",\"BLEND_srcVert\":{"
                  << "\"buffer\":\"srcVert\""
                  << ",\"count\":" << sv.size()
                  << ",\"totvert\":" << mesh->totvert
                  << ",\"componentType\":\"unsignedInt\""
                  << ",\"positionsSpace\":\"meshLocal\""
                  << '}';
                conv_data.meshSrcVertDna[mi] = sv;
            } else if (am != nullptr && am->mNumVertices > 0) {
                // Honesty: expanded verts without a full DNA map (should not happen).
                j << ",\"BLEND_srcVert\":{\"arraysResolved\":false}";
                conv_data.skippedCategories.push_back(
                        std::string("BLEND_srcVert_unresolved:") + (mesh->id.name + 2));
            }
            j << '}';
            conv_data.meshSidecarJson[mi] = j.str();
        }
    }

    return;
}

// ------------------------------------------------------------------------------------------------
aiCamera *BlenderImporter::ConvertCamera(const Scene & /*in*/, const Object *obj, const Camera *cam, ConversionData &conv_data) {
    std::unique_ptr<aiCamera> out(new aiCamera());
    out->mName = obj->id.name + 2;
    out->mPosition = aiVector3D(0.f, 0.f, 0.f);
    out->mUp = aiVector3D(0.f, 1.f, 0.f);
    out->mLookAt = aiVector3D(0.f, 0.f, -1.f);
    // Presence: only emit FOV/aspect/ortho from DNA fields that resolved (no invent).
    if (cam->sensor_x && cam->lens) {
        out->mHorizontalFOV = 2.f * std::atan2(cam->sensor_x, 2.f * cam->lens);
    }
    if (cam->sensor_x > 0.f && cam->sensor_y > 0.f) {
        out->mAspect = cam->sensor_x / cam->sensor_y;
    }
    if (cam->type == Camera::Type_ORTHO && cam->ortho_scale > 0.f) {
        // Blender ortho_scale is full fit size; aiCamera wants half horizontal width.
        out->mOrthographicWidth = cam->ortho_scale * 0.5f;
        out->mHorizontalFOV = 0.f;
    }
    out->mClipPlaneNear = cam->clipsta;
    out->mClipPlaneFar = cam->clipend;

    // C8 sidecar: DOF / focus-object bind (not an aiCamera field) — Presence only.
    const Object *focusObj = nullptr;
    bool dofEnabled = false;
    float focusDistance = 0.f;
    bool hasFocusDistance = false;
    float apertureFstop = 0.f;
    bool hasFstop = false;
    int apertureBlades = 0;
    bool hasBlades = false;
    const char *dofSource = nullptr;

    if (cam->dof.focus_object || (cam->dof.flag & CameraDOFSettings::Flag_Enabled) ||
            cam->dof.focus_distance > 0.f || cam->dof.aperture_fstop > 0.f) {
        dofSource = "Camera.dof";
        if (cam->dof.flag & CameraDOFSettings::Flag_Enabled) {
            dofEnabled = true;
        }
        if (cam->dof.focus_object) {
            focusObj = cam->dof.focus_object.get();
            dofEnabled = true;
        }
        if (cam->dof.focus_distance > 0.f) {
            focusDistance = cam->dof.focus_distance;
            hasFocusDistance = true;
        }
        if (cam->dof.aperture_fstop > 0.f) {
            apertureFstop = cam->dof.aperture_fstop;
            hasFstop = true;
        }
        if (cam->dof.aperture_blades > 0) {
            apertureBlades = cam->dof.aperture_blades;
            hasBlades = true;
        }
    }
    if (focusObj == nullptr && cam->dof_ob) {
        // Legacy *dof_ob (tiger.blend / pre-2.8).
        dofSource = "Camera.dof_ob";
        focusObj = cam->dof_ob.get();
        dofEnabled = true;
    }
    // gpu_dof only supplements when DOF is actually active (dof_ob / dof.flag / focus_object).
    // Do not emit from unused gpu_dof defaults alone (e.g. fstop=128 on non-DOF cams).
    if (dofEnabled || focusObj != nullptr) {
        if (cam->gpu_dof.focus_distance > 0.f || cam->gpu_dof.fstop > 0.f || cam->gpu_dof.num_blades > 0) {
            if (dofSource == nullptr) {
                dofSource = "Camera.gpu_dof";
            } else if (std::strcmp(dofSource, "Camera.dof_ob") == 0) {
                // keep dof_ob as primary source; gpu_dof supplies numeric params
            }
            if (!hasFocusDistance && cam->gpu_dof.focus_distance > 0.f) {
                focusDistance = cam->gpu_dof.focus_distance;
                hasFocusDistance = true;
            }
            if (!hasFstop && cam->gpu_dof.fstop > 0.f) {
                apertureFstop = cam->gpu_dof.fstop;
                hasFstop = true;
            }
            if (!hasBlades && cam->gpu_dof.num_blades > 0) {
                apertureBlades = cam->gpu_dof.num_blades;
                hasBlades = true;
            }
        }
    }

    if (dofEnabled || focusObj != nullptr) {
        std::ostringstream dj;
        dj << "\"BLEND_camera_dof\":{";
        bool first = true;
        auto sep = [&]() {
            if (!first) {
                dj << ',';
            }
            first = false;
        };
        if (dofSource != nullptr) {
            sep();
            dj << "\"source\":\"" << dofSource << '"';
        }
        sep();
        dj << "\"enabled\":" << (dofEnabled ? "true" : "false");
        if (focusObj != nullptr && focusObj->id.name[0] != '\0') {
            const char *fname = focusObj->id.name + 2;
            sep();
            dj << "\"focusObject\":\"" << BlendJsonEscape(fname) << '"';
            sep();
            dj << "\"focusObjectType\":";
            if (focusObj->type == Object::Type_CAMERA) {
                dj << "\"CAMERA\"";
            } else if (focusObj->type == Object::Type_EMPTY) {
                dj << "\"EMPTY\"";
            } else {
                dj << "\"OTHER\"";
            }
            // Empty display (how Host knows focus is a DOF target empty, not a cam).
            if (focusObj->type == Object::Type_EMPTY) {
                const char *disp = nullptr;
                switch (static_cast<unsigned char>(focusObj->empty_drawtype)) {
                case 1: disp = "ARROWS"; break;
                case 2: disp = "PLAIN_AXES"; break;
                case 3: disp = "CIRCLE"; break;
                case 4: disp = "SINGLE_ARROW"; break;
                case 5: disp = "CUBE"; break;
                case 6: disp = "SPHERE"; break;
                case 7: disp = "CONE"; break;
                case 8: disp = "IMAGE"; break;
                default: break;
                }
                if (disp != nullptr) {
                    sep();
                    dj << "\"emptyDisplayType\":\"" << disp << '"';
                }
                if (focusObj->empty_drawsize > 0.f) {
                    sep();
                    dj << "\"emptyDisplaySize\":" << focusObj->empty_drawsize;
                }
            }
            // Collect focus object for TRS Action -> aiNodeAnim (e.g. focusAction).
            bool seen = false;
            for (const Object *fo : conv_data.camera_focus_objects) {
                if (fo == focusObj) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                conv_data.camera_focus_objects.push_back(focusObj);
            }
        }
        if (hasFocusDistance) {
            sep();
            dj << "\"focusDistance\":" << focusDistance;
        }
        if (hasFstop) {
            sep();
            dj << "\"apertureFstop\":" << apertureFstop;
        }
        if (hasBlades) {
            sep();
            dj << "\"apertureBlades\":" << apertureBlades;
        }
        dj << '}';
        conv_data.cameraSidecarExtras[out->mName.C_Str()] = dj.str();
    }

    return out.release();
}

// ------------------------------------------------------------------------------------------------
aiLight *BlenderImporter::ConvertLight(const Scene & /*in*/, const Object *obj, const Lamp *lamp, ConversionData & /*conv_data*/) {
    std::unique_ptr<aiLight> out(new aiLight());
    out->mName = obj->id.name + 2;

    switch (lamp->type) {
    case Lamp::Type_Local:
        out->mType = aiLightSource_POINT;
        break;
    case Lamp::Type_Spot:
        out->mType = aiLightSource_SPOT;

        // blender orients directional lights as facing toward -z
        out->mDirection = aiVector3D(0.f, 0.f, -1.f);
        out->mUp = aiVector3D(0.f, 1.f, 0.f);

        out->mAngleInnerCone = lamp->spotsize * (1.0f - lamp->spotblend);
        out->mAngleOuterCone = lamp->spotsize;
        break;
    case Lamp::Type_Sun:
        out->mType = aiLightSource_DIRECTIONAL;

        // blender orients directional lights as facing toward -z
        out->mDirection = aiVector3D(0.f, 0.f, -1.f);
        out->mUp = aiVector3D(0.f, 1.f, 0.f);
        break;

    case Lamp::Type_Area:
        out->mType = aiLightSource_AREA;

        if (lamp->area_shape == 0) {
            out->mSize = aiVector2D(lamp->area_size, lamp->area_size);
        } else {
            out->mSize = aiVector2D(lamp->area_size, lamp->area_sizey);
        }

        // blender orients directional lights as facing toward -z
        out->mDirection = aiVector3D(0.f, 0.f, -1.f);
        out->mUp = aiVector3D(0.f, 1.f, 0.f);
        break;

    default:
        break;
    }

    out->mColorAmbient = aiColor3D(lamp->r, lamp->g, lamp->b) * lamp->energy;
    out->mColorSpecular = aiColor3D(lamp->r, lamp->g, lamp->b) * lamp->energy;
    out->mColorDiffuse = aiColor3D(lamp->r, lamp->g, lamp->b) * lamp->energy;

    // If default values are supplied, compute the coefficients from light's max distance
    // Read this: https://imdoingitwrong.wordpress.com/2011/01/31/light-attenuation/
    //
    if (lamp->constant_coefficient == 1.0f && lamp->linear_coefficient == 0.0f && lamp->quadratic_coefficient == 0.0f && lamp->dist > 0.0f) {
        out->mAttenuationConstant = 1.0f;
        out->mAttenuationLinear = 2.0f / lamp->dist;
        out->mAttenuationQuadratic = 1.0f / (lamp->dist * lamp->dist);
    } else {
        out->mAttenuationConstant = lamp->constant_coefficient;
        out->mAttenuationLinear = lamp->linear_coefficient;
        out->mAttenuationQuadratic = lamp->quadratic_coefficient;
    }

    return out.release();
}

namespace {

// Minimal Blender-layout mat3/mat4 helpers (column-major, translation in mat[3]).
void BlendZeroM3(float m[3][3]) {
    for (unsigned int i = 0; i < 3; ++i) {
        for (unsigned int j = 0; j < 3; ++j) {
            m[i][j] = 0.f;
        }
    }
}

void BlendIdentityM4(float m[4][4]) {
    for (unsigned int i = 0; i < 4; ++i) {
        for (unsigned int j = 0; j < 4; ++j) {
            m[i][j] = (i == j) ? 1.f : 0.f;
        }
    }
}

void BlendMulM3(float r[3][3], const float a[3][3], const float b[3][3]) {
    float t[3][3];
    for (unsigned int i = 0; i < 3; ++i) {
        for (unsigned int j = 0; j < 3; ++j) {
            t[i][j] = a[0][j] * b[i][0] + a[1][j] * b[i][1] + a[2][j] * b[i][2];
        }
    }
    for (unsigned int i = 0; i < 3; ++i) {
        for (unsigned int j = 0; j < 3; ++j) {
            r[i][j] = t[i][j];
        }
    }
}

void BlendMulM4(float r[4][4], const float a[4][4], const float b[4][4]) {
    float t[4][4];
    for (unsigned int i = 0; i < 4; ++i) {
        for (unsigned int j = 0; j < 4; ++j) {
            t[i][j] = a[0][j] * b[i][0] + a[1][j] * b[i][1] + a[2][j] * b[i][2] + a[3][j] * b[i][3];
        }
    }
    for (unsigned int i = 0; i < 4; ++i) {
        for (unsigned int j = 0; j < 4; ++j) {
            r[i][j] = t[i][j];
        }
    }
}

void BlendSizeToMat3(float m[3][3], const float size[3]) {
    BlendZeroM3(m);
    m[0][0] = size[0];
    m[1][1] = size[1];
    m[2][2] = size[2];
}

void BlendQuatToMat3(float m[3][3], const float q[4]) {
    // Blender quat = (w, x, y, z). Write Blender column-major (m[col][row]) so
    // BlendCopyMat4ToAi's transpose yields Assimp row-major R (P1b: no Rx sign flip).
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    const float n = std::sqrt(w * w + x * x + y * y + z * z);
    const float s = (n > 1e-20f) ? (2.f / (n * n)) : 0.f;
    const float xx = x * x * s, yy = y * y * s, zz = z * z * s;
    const float xy = x * y * s, xz = x * z * s, yz = y * z * s;
    const float wx = w * x * s, wy = w * y * s, wz = w * z * s;
    // Math R[row][col] then store as m[col][row].
    m[0][0] = 1.f - (yy + zz);
    m[1][0] = xy - wz;
    m[2][0] = xz + wy;
    m[0][1] = xy + wz;
    m[1][1] = 1.f - (xx + zz);
    m[2][1] = yz - wx;
    m[0][2] = xz - wy;
    m[1][2] = yz + wx;
    m[2][2] = 1.f - (xx + yy);
}

void BlendAxisAngleToMat3(float m[3][3], const float axis[3], float angle) {
    const float len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    float ax = axis[0], ay = axis[1], az = axis[2];
    if (len > 1e-20f) {
        ax /= len;
        ay /= len;
        az /= len;
    }
    const float c = std::cos(angle), s = std::sin(angle), t = 1.f - c;
    // Math R then store Blender column-major m[col][row].
    m[0][0] = t * ax * ax + c;
    m[1][0] = t * ax * ay - s * az;
    m[2][0] = t * ax * az + s * ay;
    m[0][1] = t * ax * ay + s * az;
    m[1][1] = t * ay * ay + c;
    m[2][1] = t * ay * az - s * ax;
    m[0][2] = t * ax * az - s * ay;
    m[1][2] = t * ay * az + s * ax;
    m[2][2] = t * az * az + c;
}

void BlendEulXYZToMat3(float m[3][3], const float eul[3]) {
    // XYZ intrinsic (Blender ROT_MODE_XYZ / default EUL). Other eul orders approximated as XYZ.
    // Matrices in Blender column-major (m[col][row]) for BlendMulM3 / BlendCopyMat4ToAi.
    float Rx[3][3], Ry[3][3], Rz[3][3], t[3][3];
    const float cx = std::cos(eul[0]), sx = std::sin(eul[0]);
    const float cy = std::cos(eul[1]), sy = std::sin(eul[1]);
    const float cz = std::cos(eul[2]), sz = std::sin(eul[2]);
    BlendZeroM3(Rx);
    Rx[0][0] = 1.f;
    Rx[1][1] = cx;
    Rx[2][1] = -sx;
    Rx[1][2] = sx;
    Rx[2][2] = cx;
    BlendZeroM3(Ry);
    Ry[0][0] = cy;
    Ry[2][0] = sy;
    Ry[1][1] = 1.f;
    Ry[0][2] = -sy;
    Ry[2][2] = cy;
    BlendZeroM3(Rz);
    Rz[0][0] = cz;
    Rz[1][0] = -sz;
    Rz[0][1] = sz;
    Rz[1][1] = cz;
    Rz[2][2] = 1.f;
    BlendMulM3(t, Ry, Rx);
    BlendMulM3(m, Rz, t);
}

/** BKE_object_to_mat4 subset: local object matrix from loc/rot/scale (+ deltas). */
void BlendObjectToMat4(const Object &ob, float r_mat[4][4]) {
    float smat[3][3], rmat[3][3], dmat[3][3], tmat[3][3];
    float size[3] = {
        ob.scale[0] * ob.dscale[0],
        ob.scale[1] * ob.dscale[1],
        ob.scale[2] * ob.dscale[2]
    };
    BlendSizeToMat3(smat, size);

    // rotmode: >0 euler, -1 axis-angle, 0 quaternion (DNA_action_types.h).
    if (ob.rotmode > 0) {
        BlendEulXYZToMat3(rmat, ob.rot);
        BlendEulXYZToMat3(dmat, ob.drot);
        BlendMulM3(rmat, dmat, rmat);
    } else if (ob.rotmode == -1) {
        BlendAxisAngleToMat3(rmat, ob.rotAxis, ob.rotAngle);
        BlendAxisAngleToMat3(dmat, ob.drotAxis, ob.drotAngle);
        BlendMulM3(rmat, dmat, rmat);
    } else {
        BlendQuatToMat3(rmat, ob.quat);
        BlendQuatToMat3(dmat, ob.dquat);
        BlendMulM3(rmat, dmat, rmat);
    }

    BlendMulM3(tmat, rmat, smat);
    BlendIdentityM4(r_mat);
    for (unsigned int i = 0; i < 3; ++i) {
        for (unsigned int j = 0; j < 3; ++j) {
            r_mat[i][j] = tmat[i][j];
        }
    }
    r_mat[3][0] = ob.loc[0] + ob.dloc[0];
    r_mat[3][1] = ob.loc[1] + ob.dloc[1];
    r_mat[3][2] = ob.loc[2] + ob.dloc[2];
}

void BlendCopyMat4ToAi(aiMatrix4x4 &out, const float m[4][4]) {
    // Same transpose as historic obmat → aiNode path.
    for (unsigned int x = 0; x < 4; ++x) {
        for (unsigned int y = 0; y < 4; ++y) {
            out[y][x] = m[x][y];
        }
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
aiNode *BlenderImporter::ConvertNode(const Scene &in, const Object *obj, ConversionData &conv_data, const aiMatrix4x4 &parentTransform) {
    std::deque<const Object *> children;
    for (ObjectSet::iterator it = conv_data.objects.begin(); it != conv_data.objects.end();) {
        const Object *object = *it;
        if (object->parent == obj) {
            children.push_back(object);

            conv_data.objects.erase(it++);
            continue;
        }
        ++it;
    }

    std::unique_ptr<aiNode> node(new aiNode(obj->id.name + 2)); // skip over the name prefix 'OB'
    if (obj->data) {
        switch (obj->type) {
        case Object::Type_EMPTY:
            break; // do nothing

            // supported object types
        case Object::Type_MESH: {
            const size_t old = conv_data.meshes->size();

            CheckActualType(obj->data.get(), "Mesh");
            ConvertMesh(in, obj, static_cast<const Mesh *>(obj->data.get()), conv_data, conv_data.meshes);

            if (conv_data.meshes->size() > old) {
                node->mMeshes = new unsigned int[node->mNumMeshes = static_cast<unsigned int>(conv_data.meshes->size() - old)];
                for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
                    node->mMeshes[i] = static_cast<unsigned int>(i + old);
                }
            }
        } break;
        case Object::Type_LAMP: {
            CheckActualType(obj->data.get(), "Lamp");
            aiLight *mesh = ConvertLight(in, obj, static_cast<const Lamp *>(obj->data.get()), conv_data);

            if (mesh) {
                conv_data.lights->push_back(mesh);
            }
        } break;
        case Object::Type_CAMERA: {
            CheckActualType(obj->data.get(), "Camera");
            aiCamera *mesh = ConvertCamera(in, obj, static_cast<const Camera *>(obj->data.get()), conv_data);

            if (mesh) {
                conv_data.cameras->push_back(mesh);
                conv_data.camera_objects.push_back(obj);
            }
        } break;

        case Object::Type_ARMATURE: {
            CheckActualType(obj->data.get(), "bArmature");
            conv_data.armature_objects.push_back(obj);
        } break;

            // Grease Pencil 3 (bpy 'GREASEPENCIL' / OB_GREASE_PENCIL): no stroke mesh yet,
            // but register DNA materials so packed fill/stroke / node images embed+assign.
        case Object::Type_GREASE_PENCIL: {
            CheckActualType(obj->data.get(), "GreasePencil");
            const GreasePencil *gp = static_cast<const GreasePencil *>(obj->data.get());
            for (const std::shared_ptr<Material> &mat : gp->material_array) {
                if (!mat) {
                    continue;
                }
                const auto has = std::find(conv_data.materials_raw.begin(), conv_data.materials_raw.end(), mat);
                if (has == conv_data.materials_raw.end()) {
                    conv_data.materials_raw.push_back(mat);
                }
            }
        } break;

            // Legacy GP (bpy 'GPENCIL' / OB_GPENCIL_LEGACY): data is bGPdata (unsupported).
        case Object::Type_GPENCIL_LEGACY:
            NotSupportedObjectType(obj, "GPencil");
            break;

            // unsupported object types / log, but do not break
        case Object::Type_CURVE:
            NotSupportedObjectType(obj, "Curve");
            break;
        case Object::Type_SURF:
            NotSupportedObjectType(obj, "Surface");
            break;
        case Object::Type_FONT:
            NotSupportedObjectType(obj, "Font");
            break;
        case Object::Type_MBALL:
            NotSupportedObjectType(obj, "MetaBall");
            break;
        case Object::Type_WAVE:
            NotSupportedObjectType(obj, "Wave");
            break;
        case Object::Type_LATTICE:
            NotSupportedObjectType(obj, "Lattice");
            break;

            // invalid or unknown type
        default:
            break;
        }
    }

    if (obj->has_world_matrix) {
        // DNA world matrix (`obmat` / `object_to_world`): convert to parent-local.
        BlendCopyMat4ToAi(node->mTransformation, obj->obmat);
        aiMatrix4x4 m = parentTransform;
        m = m.Inverse();
        node->mTransformation = m * node->mTransformation;
    } else {
        // Blender >=4.1: world matrix is runtime-only. Compose local from loc/rot/scale
        // and apply parentinv (BKE_object_to_mat4 + parentinv), already parent-local.
        static bool sWarnedMissingObmat = false;
        if (!sWarnedMissingObmat) {
            LogWarn("Object DNA has no obmat/object_to_world (Blender >=4.1); "
                    "composing node transforms from loc/rot/scale + parentinv");
            sWarnedMissingObmat = true;
        }
        float local[4][4], out[4][4];
        BlendObjectToMat4(*obj, local);
        BlendMulM4(out, obj->parentinv, local);
        BlendCopyMat4ToAi(node->mTransformation, out);
    }

    std::vector<aiNode *> boneRoots;
    if (obj->type == Object::Type_ARMATURE) {
        BlendAttachArmatureBones(node.get(), *obj, conv_data, boneRoots);
    }

    const size_t nChildObj = children.size();
    const size_t nChildAll = nChildObj + boneRoots.size();
    if (nChildAll) {
        node->mNumChildren = static_cast<unsigned int>(nChildAll);
        aiNode **nd = node->mChildren = new aiNode *[node->mNumChildren]();
        for (aiNode *bn : boneRoots) {
            *nd = bn;
            (*nd++)->mParent = node.get();
        }
        for (const Object *nobj : children) {
            *nd = ConvertNode(in, nobj, conv_data, node->mTransformation * parentTransform);
            (*nd++)->mParent = node.get();
        }
    }

    // apply modifiers
    modifier_cache->ApplyModifiers(*node, conv_data, in, *obj);
    CollectBlendPhysicsModifiers(obj, conv_data, node.get());
    // Owner Presence: $mod.blend.* on typed aiNode (no Modifier:* children - Host 0af).
    BlendEmitModifierStackOnNode(node.get(), obj, conv_data);

    // Asset Browser fields on Object ID (Presence).
    BlendEmitAssetBrowserMetadata(node->mMetaData, obj->id.asset_data.get(), false);

    return node.release();
}

BlenderImporter::StreamOrError BlenderImporter::ParseMagicToken(const std::string &pFile, IOSystem *pIOHandler) const {
    std::shared_ptr<IOStream> stream(pIOHandler->Open(pFile, "rb"));
    if (stream == nullptr) {
        return { {}, {}, "Could not open file for reading" };
    }

    char magic[8] = { 0 };
    stream->Read(magic, 7, 1);
    if (strcmp(magic, Token) == 0) {
        return { stream, {}, {} };
    }

    // Check for gzip (legacy) or zstd (Blender 3+ default) compressed blend files.
#ifdef ASSIMP_BUILD_NO_COMPRESSED_BLEND
    return { {}, {}, "BLENDER magic bytes are missing, is this file compressed (Assimp was built without decompression support)?" };
#else
    const bool isGzip = (magic[0] == 0x1f && static_cast<uint8_t>(magic[1]) == 0x8b);
    // Zstandard magic: 0x28 0xB5 0x2F 0xFD (little-endian frame header)
    const bool isZstd = (static_cast<uint8_t>(magic[0]) == 0x28 &&
                         static_cast<uint8_t>(magic[1]) == 0xb5 &&
                         static_cast<uint8_t>(magic[2]) == 0x2f &&
                         static_cast<uint8_t>(magic[3]) == 0xfd);

    if (!isGzip && !isZstd) {
        return { {}, {}, "BLENDER magic bytes are missing, couldn't find GZIP or Zstandard header either" };
    }

    stream->Seek(0L, aiOrigin_SET);
    auto uncompressed = std::make_shared<std::vector<char>>();
    size_t total = 0;

    if (isGzip) {
        LogDebug("Found no BLENDER magic word but a GZIP header, might be a compressed file");
        if (magic[2] != 8) {
            return { {}, {}, "Unsupported GZIP compression method" };
        }

        // http://www.gzip.org/zlib/rfc-gzip.html#header-trailer
        std::shared_ptr<StreamReaderLE> reader = std::shared_ptr<StreamReaderLE>(new StreamReaderLE(stream));
        Compression compression;
        if (compression.open(Compression::Format::Binary, Compression::FlushMode::NoFlush, 16 + Compression::MaxWBits)) {
            total = compression.decompress((unsigned char *)reader->GetPtr(), reader->GetRemainingSize(), *uncompressed);
            compression.close();
        }
    } else {
#if defined(ASSIMP_BLEND_SUPPORT_ZSTD)
        // Blender often writes multi-frame ZSTD (not a single frame). Always stream
        // the whole input; ZSTD_getFrameContentSize only describes the first frame.
        LogDebug("Found no BLENDER magic word but a Zstandard header, might be a compressed file");
        std::shared_ptr<StreamReaderLE> reader = std::shared_ptr<StreamReaderLE>(new StreamReaderLE(stream));
        const int8_t *src = reader->GetPtr();
        const size_t srcSize = reader->GetRemainingSize();
        ZSTD_DCtx *dctx = ZSTD_createDCtx();
        if (dctx == nullptr) {
            return { {}, {}, "Failed to allocate Zstandard decompression context" };
        }
        uncompressed->resize(srcSize * 8 + 256 * 1024);
        ZSTD_inBuffer input{ src, srcSize, 0 };
        ZSTD_outBuffer output{ uncompressed->data(), uncompressed->size(), 0 };
        while (input.pos < input.size) {
            if (output.pos == output.size) {
                const size_t newSize = uncompressed->size() * 2;
                if (newSize > (1ull << 31)) {
                    ZSTD_freeDCtx(dctx);
                    return { {}, {}, "Zstandard BLEND payload is unreasonably large" };
                }
                uncompressed->resize(newSize);
                output.dst = uncompressed->data();
                output.size = uncompressed->size();
            }
            const size_t ret = ZSTD_decompressStream(dctx, &output, &input);
            if (ZSTD_isError(ret)) {
                ZSTD_freeDCtx(dctx);
                return { {}, {}, std::string("Zstandard decompress failed: ") + ZSTD_getErrorName(ret) };
            }
            // ret==0 means a frame finished; continue for further frames / skippable frames.
        }
        ZSTD_freeDCtx(dctx);
        total = output.pos;
        uncompressed->resize(total);
#else
        return { {}, {}, "BLENDER file appears Zstandard-compressed, but Assimp was built without ZSTD support" };
#endif
    }

    // replace the input stream with a memory stream
    stream = std::make_shared<MemoryIOStream>(reinterpret_cast<uint8_t *>(uncompressed->data()), total);

    // .. and retry
    stream->Read(magic, 7, 1);
    if (strcmp(magic, Token) == 0) {
        return { stream, uncompressed, {} };
    }
    return { {}, {}, isGzip ? "Found no BLENDER magic word in decompressed GZIP file" : "Found no BLENDER magic word in decompressed Zstandard file" };
#endif
}

#endif // ASSIMP_BUILD_NO_BLEND_IMPORTER
