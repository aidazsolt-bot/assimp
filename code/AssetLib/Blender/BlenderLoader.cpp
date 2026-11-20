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

//#define ASSIMP_BUILD_NO_COMPRESSED_BLEND
// Uncomment this to disable support for (gzip/zstd)compressed .BLEND files

#ifndef ASSIMP_BUILD_NO_BLEND_IMPORTER

#include "BlenderBMesh.h"
#include "BlenderCustomData.h"
#include "BlenderIntermediate.h"
#include "BlenderMeshAttributes.h"
#include "BlenderModifier.h"
#include <assimp/StringUtils.h>
#include <assimp/commonMetaData.h>
#include <assimp/importerdesc.h>
#include <assimp/metadata.h>
#include <assimp/scene.h>
#include <assimp/sidecar.h>

#include <assimp/MemoryIOWrapper.h>
#include <assimp/StreamReader.h>
#include <assimp/StringComparison.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <sstream>
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
    kSH_NODE_MIX_SHADER = 128,
    kSH_NODE_BSDF_GLOSSY = 131,
    kSH_NODE_BSDF_DIFFUSE = 132,
    kSH_NODE_BSDF_GLOSSY_LEGACY = 133,
    kSH_NODE_BSDF_GLASS = 134,
    kSH_NODE_EMISSION = 140,
    kSH_NODE_TEX_IMAGE = 143,
    kSH_NODE_BSDF_PRINCIPLED = 193,
    kSH_NODE_NORMAL_MAP = 175,
    kSH_NODE_BUMP = 170,
    kSH_NODE_INVERT = 119,
    kSH_NODE_REROUTE = 6,
    kSH_NODE_RGB = 101,
    kSH_NODE_VALUE = 102
};

struct BlendShaderFlat {
    bool ok{ false };
    bool mixedGraph{ false };
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
    const Assimp::Blender::Image *mapBaseColor{ nullptr };
    const Assimp::Blender::Image *mapRoughness{ nullptr };
    const Assimp::Blender::Image *mapNormal{ nullptr };
    const Assimp::Blender::Image *mapEmission{ nullptr };
    const Assimp::Blender::Image *mapMetallic{ nullptr };

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
    if (std::strcmp(sock.default_value->dna_type, "bNodeSocketValueFloat") == 0) {
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
    if (std::strcmp(sock.default_value->dna_type, "bNodeSocketValueRGBA") == 0) {
        const auto *rgba = static_cast<const bNodeSocketValueRGBA *>(sock.default_value.get());
        out[0] = rgba->value[0];
        out[1] = rgba->value[1];
        out[2] = rgba->value[2];
        out[3] = rgba->value[3];
        return true;
    }
    return false;
}

const Assimp::Blender::bNode *BlendCastNode(const Assimp::Blender::ElemBase *e) {
    using namespace Assimp::Blender;
    if (e == nullptr) {
        return nullptr;
    }
    if (e->dna_type != nullptr && std::strcmp(e->dna_type, "bNode") != 0) {
        return nullptr;
    }
    return static_cast<const bNode *>(e);
}

const Assimp::Blender::bNodeSocket *BlendCastSocket(const Assimp::Blender::ElemBase *e) {
    using namespace Assimp::Blender;
    if (e == nullptr) {
        return nullptr;
    }
    if (e->dna_type != nullptr && std::strcmp(e->dna_type, "bNodeSocket") != 0) {
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

bool BlendNodeIsMixShader(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0' && std::strcmp(n.idname, "ShaderNodeMixShader") == 0) {
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

bool BlendNodeIsPassthrough(const Assimp::Blender::bNode &n) {
    if (n.idname[0] != '\0') {
        if (std::strcmp(n.idname, "NodeReroute") == 0 ||
                std::strcmp(n.idname, "ShaderNodeInvert") == 0 ||
                std::strcmp(n.idname, "ShaderNodeNormalMap") == 0 ||
                std::strcmp(n.idname, "ShaderNodeBump") == 0 ||
                std::strcmp(n.idname, "ShaderNodeHueSaturation") == 0 ||
                std::strcmp(n.idname, "ShaderNodeRGBToBW") == 0) {
            return true;
        }
    }
    return n.type == kSH_NODE_REROUTE || n.type == kSH_NODE_INVERT ||
           n.type == kSH_NODE_NORMAL_MAP || n.type == kSH_NODE_BUMP;
}

bool BlendNodeIsFlattenable(const Assimp::Blender::bNode &n) {
    return BlendNodeIsPrincipled(n) || BlendNodeIsGlass(n) || BlendNodeIsGlossy(n) ||
           BlendNodeIsDiffuse(n) || BlendNodeIsEmission(n);
}

const Assimp::Blender::bNodeLink *BlendCastLink(const Assimp::Blender::ElemBase *e) {
    using namespace Assimp::Blender;
    if (e == nullptr) {
        return nullptr;
    }
    if (e->dna_type != nullptr && std::strcmp(e->dna_type, "bNodeLink") != 0) {
        return nullptr;
    }
    return static_cast<const bNodeLink *>(e);
}

const Assimp::Blender::Image *BlendNodeImage(const Assimp::Blender::bNode &n) {
    using namespace Assimp::Blender;
    if (!n.id) {
        return nullptr;
    }
    if (n.id->dna_type != nullptr && std::strcmp(n.id->dna_type, "Image") != 0) {
        return nullptr;
    }
    return static_cast<const Image *>(n.id.get());
}

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

    const Assimp::Blender::bNode *FollowToTexImage(const Assimp::Blender::bNode *n, int depth = 0) const {
        using namespace Assimp::Blender;
        if (n == nullptr || depth > 8) {
            return nullptr;
        }
        if (BlendNodeIsTexImage(*n)) {
            return n;
        }
        if (!BlendNodeIsPassthrough(*n)) {
            return nullptr;
        }
        // Normal Map: Color is the tangent-space map. Bump: prefer Normal chain, then Height.
        auto trySock = [&](const char *want) -> const bNode * {
            for (std::shared_ptr<ElemBase> cur = n->inputs.first; cur;) {
                const bNodeSocket *sock = BlendCastSocket(cur.get());
                if (sock == nullptr) {
                    break;
                }
                if (BlendSockNameIs(*sock, want)) {
                    const bNodeLink *l = FindLinkToSocket(*sock);
                    if (l) {
                        return FollowToTexImage(BlendCastNode(l->fromnode.get()), depth + 1);
                    }
                }
                cur = sock->next;
            }
            return nullptr;
        };
        if (BlendNodeIsNormalMap(*n)) {
            if (const bNode *hit = trySock("Color")) {
                return hit;
            }
        }
        if (BlendNodeIsBump(*n)) {
            if (const bNode *hit = trySock("Normal")) {
                return hit;
            }
            if (const bNode *hit = trySock("Height")) {
                return hit;
            }
        }
        for (std::shared_ptr<ElemBase> cur = n->inputs.first; cur;) {
            const bNodeSocket *sock = BlendCastSocket(cur.get());
            if (sock == nullptr) {
                break;
            }
            if (BlendSockNameIsAny(*sock, { "Color", "Height", "Image", "Normal" })) {
                const bNodeLink *l = FindLinkToSocket(*sock);
                if (l) {
                    const bNode *hit = FollowToTexImage(BlendCastNode(l->fromnode.get()), depth + 1);
                    if (hit) {
                        return hit;
                    }
                }
            }
            cur = sock->next;
        }
        return nullptr;
    }

    bool ReadConstantColor(const Assimp::Blender::bNode *n, float out[4], int depth = 0) const {
        using namespace Assimp::Blender;
        if (n == nullptr || depth > 6) {
            return false;
        }
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
        if (BlendNodeIsValue(*n)) {
            float v = 0.f;
            for (std::shared_ptr<ElemBase> cur = n->outputs.first; cur;) {
                const bNodeSocket *sock = BlendCastSocket(cur.get());
                if (sock == nullptr) {
                    break;
                }
                BlendReadSocketFloat(*sock, v);
                out[0] = out[1] = out[2] = v;
                out[3] = 1.f;
                return true;
            }
        }
        if (BlendNodeIsPassthrough(*n) && !BlendNodeIsNormalMap(*n) && !BlendNodeIsBump(*n)) {
            for (std::shared_ptr<ElemBase> cur = n->inputs.first; cur;) {
                const bNodeSocket *sock = BlendCastSocket(cur.get());
                if (sock == nullptr) {
                    break;
                }
                if (BlendSockNameIsAny(*sock, { "Color", "Fac", "Value" })) {
                    const bNodeLink *l = FindLinkToSocket(*sock);
                    if (l && ReadConstantColor(BlendCastNode(l->fromnode.get()), out, depth + 1)) {
                        return true;
                    }
                }
                cur = sock->next;
            }
        }
        return false;
    }

    bool ReadConstantFloat(const Assimp::Blender::bNode *n, float &out, int depth = 0) const {
        float tmp[4];
        if (!ReadConstantColor(n, tmp, depth)) {
            return false;
        }
        out = tmp[0];
        return true;
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
                float c[4] = { 0.8f, 0.8f, 0.8f, 1.f };
                BlendReadSocketColor(*sock, c);
                if (std::fabs(c[0] - 0.8f) > 0.02f || std::fabs(c[1] - 0.8f) > 0.02f || std::fabs(c[2] - 0.8f) > 0.02f) {
                    score += 5;
                }
            }
            if (BlendSockNameIs(*sock, "Roughness")) {
                float r = 0.5f;
                BlendReadSocketFloat(*sock, r);
                if (std::fabs(r - 0.5f) > 0.02f) {
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

    const Assimp::Blender::bNode *ResolveSurface(const Assimp::Blender::bNode *n, bool &sawMix, int depth = 0) const {
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
        if (BlendNodeIsMixShader(*n)) {
            sawMix = true;
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
                        best = PickBest(best, ResolveSurface(BlendCastNode(l->fromnode.get()), sawMix, depth + 1));
                    }
                }
                // Mix Shader inputs are often just named "Shader" twice - take every linked input.
                const bNodeLink *l = FindLinkToSocket(*sock);
                if (l && !BlendSockNameIs(*sock, "Fac")) {
                    best = PickBest(best, ResolveSurface(BlendCastNode(l->fromnode.get()), sawMix, depth + 1));
                }
                cur = sock->next;
            }
            return best;
        }
        return nullptr;
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

// Returns true if a numeric/color factor was resolved (typed default_value or linked constant).
// Texture maps alone do not count as a factor (mapOut may still be set).
bool BlendReadLinkedOrDefaultFloat(const BlendNodeGraph &g, const Assimp::Blender::bNodeSocket &sock,
        float &out, const Assimp::Blender::Image **mapOut) {
    using namespace Assimp::Blender;
    bool got = BlendReadSocketFloat(sock, out);
    const bNodeLink *l = g.FindLinkToSocket(sock);
    if (!l) {
        return got;
    }
    const bNode *src = BlendCastNode(l->fromnode.get());
    const bNode *tex = g.FollowToTexImage(src);
    if (tex && mapOut) {
        *mapOut = BlendNodeImage(*tex);
    }
    float linked = out;
    if (g.ReadConstantFloat(src, linked)) {
        out = linked;
        return true;
    }
    return got;
}

bool BlendReadLinkedOrDefaultColor(const BlendNodeGraph &g, const Assimp::Blender::bNodeSocket &sock,
        float out[4], const Assimp::Blender::Image **mapOut) {
    using namespace Assimp::Blender;
    bool got = BlendReadSocketColor(sock, out);
    const bNodeLink *l = g.FindLinkToSocket(sock);
    if (!l) {
        return got;
    }
    const bNode *src = BlendCastNode(l->fromnode.get());
    const bNode *tex = g.FollowToTexImage(src);
    if (tex && mapOut) {
        *mapOut = BlendNodeImage(*tex);
    }
    float linked[4] = { out[0], out[1], out[2], out[3] };
    if (g.ReadConstantColor(src, linked)) {
        out[0] = linked[0];
        out[1] = linked[1];
        out[2] = linked[2];
        out[3] = linked[3];
        return true;
    }
    return got;
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
                    const bNode *tex = g.FollowToTexImage(BlendCastNode(l->fromnode.get()));
                    if (tex) {
                        flat.mapNormal = BlendNodeImage(*tex);
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
                if (BlendReadLinkedOrDefaultColor(g, sock, tmp, &flat.mapEmission)) {
                    flat.baseColor[0] = flat.emissionColor[0] = tmp[0];
                    flat.baseColor[1] = flat.emissionColor[1] = tmp[1];
                    flat.baseColor[2] = flat.emissionColor[2] = tmp[2];
                    flat.baseColor[3] = tmp[3];
                    flat.hasBaseColor = true;
                    flat.hasEmissionColor = true;
                }
            } else if (BlendSockNameIs(sock, "Strength")) {
                if (BlendReadSocketFloat(sock, flat.emissionStrength)) {
                    flat.hasEmissionStrength = true;
                }
            }
        });
        return finish();
    }

    return flat;
}

BlendShaderFlat BlendFlattenMaterialNodes(const Assimp::Blender::Material &mat) {
    using namespace Assimp::Blender;
    BlendShaderFlat flat;
    if (!mat.nodetree) {
        return flat;
    }
    const BlendNodeGraph g = BlendBuildGraph(*mat.nodetree);

    const bNode *outputNode = nullptr;
    for (const bNode *n : g.nodes) {
        if (BlendNodeIsOutput(*n)) {
            outputNode = n;
            break;
        }
    }

    bool sawMix = false;
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
                        chosen = g.ResolveSurface(BlendCastNode(l->fromnode.get()), sawMix);
                    }
                }
            }
            cur = sock->next;
        }
        // Fallback: any link into the output node.
        if (chosen == nullptr) {
            for (const bNodeLink *l : g.links) {
                if (BlendCastNode(l->tonode.get()) == outputNode) {
                    chosen = g.ResolveSurface(BlendCastNode(l->fromnode.get()), sawMix);
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
    if (chosen == nullptr) {
        return flat;
    }

    flat = BlendFlattenFromNode(g, *chosen);
    flat.mixedGraph = sawMix;
    return flat;
}

void BlendAttachNodeImage(aiMaterial *mout, const Assimp::Blender::Image *img, aiTextureType type,
        Assimp::Blender::ConversionData &conv_data) {
    using namespace Assimp::Blender;
    if (mout == nullptr || img == nullptr) {
        return;
    }
    aiString name;
    if (img->packedfile) {
        name.data[0] = '*';
        name.length = 1 + Assimp::ASSIMP_itoa10(name.data + 1, static_cast<unsigned int>(AI_MAXLEN - 1),
                static_cast<int32_t>(conv_data.textures->size()));
        conv_data.textures->push_back(new aiTexture());
        aiTexture *curTex = conv_data.textures->back();
        const size_t nlen = strlen(img->name);
        const char *s = img->name + nlen, *e = s;
        while (s >= img->name && *s != '.') {
            --s;
        }
        curTex->achFormatHint[0] = s + 1 > e ? '\0' : static_cast<char>(::tolower(static_cast<unsigned char>(s[1])));
        curTex->achFormatHint[1] = s + 2 > e ? '\0' : static_cast<char>(::tolower(static_cast<unsigned char>(s[2])));
        curTex->achFormatHint[2] = s + 3 > e ? '\0' : static_cast<char>(::tolower(static_cast<unsigned char>(s[3])));
        curTex->achFormatHint[3] = '\0';
        curTex->mWidth = img->packedfile->size;
        uint8_t *ch = new uint8_t[curTex->mWidth];
        conv_data.db.reader->SetCurrentPos(static_cast<size_t>(img->packedfile->data->val));
        conv_data.db.reader->CopyAndAdvance(ch, curTex->mWidth);
        curTex->pcData = reinterpret_cast<aiTexel *>(ch);
    } else {
        name = aiString(img->name);
    }
    mout->AddProperty(&name, AI_MATKEY_TEXTURE(type, conv_data.next_texture[type]++));
}

void BlendApplyFlatToMaterial(aiMaterial *mout, const BlendShaderFlat &flat,
        Assimp::Blender::ConversionData &conv_data) {
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
    const bool hasEmit = flat.mapEmission != nullptr ||
                         (flat.hasEmissionColor &&
                                 (flat.emissionColor[0] + flat.emissionColor[1] + flat.emissionColor[2]) > 1e-4f &&
                                 (!flat.hasEmissionStrength || flat.emissionStrength > 0.f));
    if (hasEmit) {
        const float s = (flat.hasEmissionStrength && flat.emissionStrength > 0.f) ? flat.emissionStrength : 1.f;
        aiColor3D emit(flat.emissionColor[0] * s, flat.emissionColor[1] * s, flat.emissionColor[2] * s);
        mout->AddProperty(&emit, 1, AI_MATKEY_COLOR_EMISSIVE);
    }
    BlendAttachNodeImage(mout, flat.mapBaseColor, aiTextureType_BASE_COLOR, conv_data);
    if (flat.mapBaseColor) {
        BlendAttachNodeImage(mout, flat.mapBaseColor, aiTextureType_DIFFUSE, conv_data);
    }
    BlendAttachNodeImage(mout, flat.mapRoughness, aiTextureType_DIFFUSE_ROUGHNESS, conv_data);
    BlendAttachNodeImage(mout, flat.mapMetallic, aiTextureType_METALNESS, conv_data);
    BlendAttachNodeImage(mout, flat.mapNormal, aiTextureType_NORMALS, conv_data);
    BlendAttachNodeImage(mout, flat.mapEmission, aiTextureType_EMISSIVE, conv_data);
}

static std::string BlendMapJson(const Assimp::Blender::Image *img) {
    if (img == nullptr || img->name[0] == '\0') {
        return "null";
    }
    return std::string("\"") + BlendJsonEscape(img->name) + '"';
}

std::string BlendBuildMaterialSidecarJson(const Assimp::Blender::Material &mat, const BlendShaderFlat &flat) {
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
    } else {
        mjson << ",\"use_nodes\":false,\"engine\":\"INTERNAL\"";
    }
    mjson << '}';
    if (flat.ok) {
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
    case R::Shape_Trimesh: return "hull";
    case R::Shape_Compound: return "compound";
    default: return "box";
    }
}

void CollectBlendPhysicsModifiers(const Assimp::Blender::Object *obj, Assimp::Blender::ConversionData &conv) {
    using namespace Assimp::Blender;
    if (obj == nullptr) {
        return;
    }
    const char *objName = obj->id.name + 2;

    // Soft Body is commonly Object.soft (not Softbody modifier).
    if (obj->soft) {
        if (!conv.physicsSoftFirst) {
            conv.physicsSoftNodes << ',';
        }
        conv.physicsSoftFirst = false;
        const SoftBody &sb = *obj->soft;
        conv.physicsSoftNodes << "{\"name\":\"" << BlendJsonEscape(objName)
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
    }

    // Rigid Body DNA -> BLEND bag + KHR-shaped nodes for Host Bullet/PhysX.
    if (obj->rigidbody_object) {
        const RigidBodyOb &rb = *obj->rigidbody_object;
        if (!conv.physicsRigidFirst) {
            conv.physicsRigidNodes << ',';
        }
        conv.physicsRigidFirst = false;
        const char *motionType = "dynamic";
        if (rb.type == RigidBodyOb::Type_Passive) {
            motionType = "static";
        }
        if (rb.flag & RigidBodyOb::Flag_Kinematic) {
            motionType = "kinematic";
        }
        const char *shapeName = BlendRigidShapeName(rb.shape);
        conv.physicsRigidNodes << "{\"name\":\"" << BlendJsonEscape(objName)
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
                               << "}}";

        // KHR_implicit_shapes entry
        if (!conv.khrShapeFirst) {
            conv.khrShapes << ',';
        }
        conv.khrShapeFirst = false;
        const int shapeIdx = conv.khrShapeCount++;
        conv.khrShapes << "{\"type\":\"" << shapeName << "\"}";

        // physicsMaterials entry (1:1 with body for now)
        if (!conv.khrMatFirst) {
            conv.khrMaterials << ',';
        }
        conv.khrMatFirst = false;
        const int matIdx = conv.khrMatCount++;
        conv.khrMaterials << "{\"staticFriction\":" << rb.friction
                          << ",\"dynamicFriction\":" << rb.friction
                          << ",\"restitution\":" << rb.restitution << '}';

        if (!conv.khrNodeFirst) {
            conv.khrNodes << ',';
        }
        conv.khrNodeFirst = false;
        conv.khrNodes << "{\"name\":\"" << BlendJsonEscape(objName)
                      << "\",\"KHR_physics_rigid_bodies\":{"
                      << "\"motion\":{\"type\":\"" << motionType << "\",\"mass\":" << rb.mass << '}'
                      << ",\"collider\":{\"geometry\":{\"shape\":" << shapeIdx
                      << "},\"physicsMaterial\":" << matIdx << "}}}";
    }

    if (obj->rigidbody_constraint) {
        if (!conv.physicsJointFirst) {
            conv.physicsJointNodes << ',';
        }
        conv.physicsJointFirst = false;
        const RigidBodyCon &jc = *obj->rigidbody_constraint;
        conv.physicsJointNodes << "{\"name\":\"" << BlendJsonEscape(objName)
                               << "\",\"type\":\"rigidbody_constraint\",\"settings\":{"
                               << "\"source\":\"Object.rigidbody_constraint\""
                               << ",\"constraint_type\":" << jc.type
                               << ",\"breaking_threshold\":" << jc.breaking_threshold
                               << "}}";
    }

    // Particle systems (Object.particlesystem ListBase)
    for (const ParticleSystem *ps = static_cast<const ParticleSystem *>(obj->particlesystem.first.get());
            ps != nullptr;
            ps = static_cast<const ParticleSystem *>(ps->next.get())) {
        if (!conv.physicsParticleFirst) {
            conv.physicsParticleNodes << ',';
        }
        conv.physicsParticleFirst = false;
        const char *psName = (ps->name[0] != '\0') ? ps->name : objName;
        conv.physicsParticleNodes << "{\"name\":\"" << BlendJsonEscape(objName)
                                  << "\",\"system\":\"" << BlendJsonEscape(psName)
                                  << "\",\"type\":\"particles\",\"settings\":{";
        if (ps->part) {
            const ParticleSettings &p = *ps->part;
            conv.physicsParticleNodes << "\"part_type\":" << p.type
                                      << ",\"totpart\":" << p.totpart
                                      << ",\"mass\":" << p.mass
                                      << ",\"size\":" << p.size
                                      << ",\"lifetime\":" << p.lifetime
                                      << ",\"frame_start\":" << p.sta
                                      << ",\"frame_end\":" << p.end;
            if (p.id.name[0] != '\0') {
                conv.physicsParticleNodes << ",\"settings_name\":\"" << BlendJsonEscape(p.id.name + 2) << '"';
            }
        } else {
            conv.physicsParticleNodes << "\"source\":\"ParticleSystem\"";
        }
        conv.physicsParticleNodes << "}}";
    }

    const SharedModifierData *cur = static_cast<const SharedModifierData *>(obj->modifiers.first.get());
    for (; cur; cur = static_cast<const SharedModifierData *>(cur->modifier.next.get())) {
        const ModifierData &dat = cur->modifier;
        std::ostringstream *stream = nullptr;
        bool *firstFlag = nullptr;
        const char *bagKey = nullptr;
        switch (dat.type) {
        case ModifierData::eModifierType_Softbody:
            stream = &conv.physicsSoftNodes;
            firstFlag = &conv.physicsSoftFirst;
            bagKey = "softbody";
            break;
        case ModifierData::eModifierType_Cloth:
            stream = &conv.physicsClothNodes;
            firstFlag = &conv.physicsClothFirst;
            bagKey = "cloth";
            break;
        case ModifierData::eModifierType_Fluidsim:
        case ModifierData::eModifierType_Fluid:
            stream = &conv.physicsFluidNodes;
            firstFlag = &conv.physicsFluidFirst;
            bagKey = "fluid";
            break;
        case ModifierData::eModifierType_Smoke:
            stream = &conv.physicsSmokeNodes;
            firstFlag = &conv.physicsSmokeFirst;
            bagKey = "smoke";
            break;
        case ModifierData::eModifierType_ParticleSystem:
            stream = &conv.physicsParticleNodes;
            firstFlag = &conv.physicsParticleFirst;
            bagKey = "particles";
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
            if (hitCloth) {
                stream = &conv.physicsClothNodes;
                firstFlag = &conv.physicsClothFirst;
                bagKey = "cloth";
            } else if (hitSoft) {
                stream = &conv.physicsSoftNodes;
                firstFlag = &conv.physicsSoftFirst;
                bagKey = "softbody";
            } else if (hitFluid) {
                stream = &conv.physicsFluidNodes;
                firstFlag = &conv.physicsFluidFirst;
                bagKey = "fluid";
            } else if (hitPart) {
                stream = &conv.physicsParticleNodes;
                firstFlag = &conv.physicsParticleFirst;
                bagKey = "particles";
            }
            break;
        }
        default:
            break;
        }
        if (stream == nullptr) {
            continue;
        }
        if (!*firstFlag) {
            *stream << ',';
        }
        *firstFlag = false;

        std::ostringstream settings;
        settings << "{\"mode\":" << dat.mode;
        if (dat.type == ModifierData::eModifierType_Nodes) {
            settings << ",\"source\":\"NodesModifierData\"";
        }
        if (dat.type == ModifierData::eModifierType_Cloth &&
                cur->dna_type != nullptr &&
                std::strcmp(cur->dna_type, "ClothModifierData") == 0) {
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
        if (dat.type == ModifierData::eModifierType_Fluid &&
                cur->dna_type != nullptr &&
                std::strcmp(cur->dna_type, "FluidModifierData") == 0) {
            const auto *fluid = static_cast<const FluidModifierData *>(cur);
            const char *role = "unknown";
            if (fluid->type & FluidModifierData::MOD_FLUID_TYPE_DOMAIN) {
                role = "domain";
            } else if (fluid->type & FluidModifierData::MOD_FLUID_TYPE_FLOW) {
                role = "flow";
            } else if (fluid->type & FluidModifierData::MOD_FLUID_TYPE_EFFEC) {
                role = "effector";
            }
            settings << ",\"fluid_role\":\"" << role << "\"";
            if (fluid->domain) {
                settings << ",\"domain_type\":" << fluid->domain->type;
                settings << ",\"cache\":{\"directory\":\"" << BlendJsonEscape(fluid->domain->cache_directory)
                         << "\",\"frame_start\":" << fluid->domain->cache_frame_start
                         << ",\"frame_end\":" << fluid->domain->cache_frame_end
                         << ",\"type\":" << fluid->domain->cache_type << '}';
            }
        }
        settings << '}';

        *stream << "{\"name\":\"" << BlendJsonEscape(objName)
                << "\",\"modifier\":\"" << BlendJsonEscape(dat.name)
                << "\",\"type\":\"" << bagKey
                << "\",\"settings\":" << settings.str() << '}';
    }
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
        //if (bl.id == "SC") {

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
void BlenderImporter::ParseSubCollection(const Blender::Scene &in, aiNode *root, const std::shared_ptr<Collection>& collection, ConversionData &conv_data) {

    std::deque<Object *> root_objects;
    // Count number of objects
    for (std::shared_ptr<CollectionObject> cur = std::static_pointer_cast<CollectionObject>(collection->gobject.first); cur; cur = cur->next) {
        if (cur->ob) {
            root_objects.push_back(cur->ob);
        }
    }
    std::deque<Collection *> root_children;
    // Count number of child nodes
    for (std::shared_ptr<CollectionChild> cur = std::static_pointer_cast<CollectionChild>(collection->children.first); cur; cur = cur->next) {
        if (cur->collection) {
            root_children.push_back(cur->collection.get());
        }
    }
    root->mNumChildren = static_cast<unsigned int>(root_objects.size() + root_children.size());
    // ValidateDataStructure requires nullptr when empty (not new aiNode*[0]).
    if (root->mNumChildren == 0) {
        root->mChildren = nullptr;
        return;
    }
    root->mChildren = new aiNode *[root->mNumChildren]();

    for (unsigned int i = 0; i < static_cast<unsigned int>(root_objects.size()); ++i) {
        root->mChildren[i] = ConvertNode(in, root_objects[i], conv_data, aiMatrix4x4());
        root->mChildren[i]->mParent = root;
    }

    // For each subcollection create a new node to represent it
    unsigned int iterator = static_cast<unsigned int>(root_objects.size());
    for (std::shared_ptr<CollectionChild> cur = std::static_pointer_cast<CollectionChild>(collection->children.first); cur; cur = cur->next) {
        if (!cur->collection) {
            continue;
        }
        root->mChildren[iterator] = new aiNode(cur->collection->id.name + 2); // skip over the name prefix 'OB'
        root->mChildren[iterator]->mParent = root;
        ParseSubCollection(in, root->mChildren[iterator], cur->collection, conv_data);
        iterator += 1;
    }
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::ConvertBlendFile(aiScene *out, const Scene &in, const FileDatabase &file,
        const char *blenderVersion) {
    ConversionData conv(file);

    aiNode *root = out->mRootNode = new aiNode("<BlenderRoot>");
    // Iterate over all objects directly under master_collection,
    // If in.master_collection == null, then we're parsing something older.
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

    // acknowledge that the scene might come out incomplete
    // by Assimp's definition of `complete`: blender scenes
    // can consist of thousands of cameras or lights with
    // not a single mesh between them.
    if (!out->mNumMeshes) {
        out->mFlags |= AI_SCENE_FLAGS_INCOMPLETE;
    }

    // C0-S8 sidecars (doc/Sidecar.md BLEND). Always emit scene bag when Scene DNA loaded.
    if (out->mMetaData == nullptr) {
        out->mMetaData = new aiMetadata();
    }
    const float fpsDenom = (in.r.frs_sec_base > 0.f) ? in.r.frs_sec_base : 1.f;
    const float fps = (fpsDenom != 0.f) ? (in.r.frs_sec / fpsDenom) : in.r.frs_sec;
    const float metersPerUnit = (in.unit.scale_length > 0.f) ? in.unit.scale_length : 1.f;

    // S1 mesh CustomData layers
    for (unsigned int mi = 0; mi < out->mNumMeshes; ++mi) {
        if (mi >= conv.meshSidecarJson.size() || conv.meshSidecarJson[mi].empty()) {
            continue;
        }
        auto *sc = new aiMeshSidecar();
        sc->mExtensionsJson = BlendDupJson(conv.meshSidecarJson[mi], sc->mExtensionsJsonLength);
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
    if (conv.physicsRigidFirst) {
        conv.skippedCategories.push_back("BLEND_physics_rigidbody_object_pointer");
    }
    conv.skippedCategories.push_back("BLEND_action_nla_animation_sidecar");
    conv.skippedCategories.push_back("BLEND_shader_network_full_nodegraph");
    const bool hasPhys = !conv.physicsSoftFirst || !conv.physicsClothFirst ||
                         !conv.physicsFluidFirst || !conv.physicsSmokeFirst ||
                         !conv.physicsRigidFirst || !conv.physicsParticleFirst ||
                         !conv.physicsJointFirst;
    if (hasPhys) {
        std::ostringstream pjson;
        pjson << "{\"schemaVersion\":1,\"source\":{\"format\":\"blend\"";
        if (blenderVersion != nullptr && blenderVersion[0] != '\0') {
            pjson << ",\"blender_version\":\"" << blenderVersion << '"';
        }
        pjson << '}';
        if (!conv.physicsSoftFirst) {
            pjson << ",\"BLEND_physics_softbody\":{\"nodes\":[" << conv.physicsSoftNodes.str() << "]}";
        }
        if (!conv.physicsClothFirst) {
            pjson << ",\"BLEND_physics_cloth\":{\"nodes\":[" << conv.physicsClothNodes.str() << "]}";
        }
        if (!conv.physicsFluidFirst) {
            pjson << ",\"BLEND_physics_fluid\":{\"nodes\":[" << conv.physicsFluidNodes.str() << "]}";
        }
        if (!conv.physicsSmokeFirst) {
            pjson << ",\"BLEND_physics_smoke\":{\"nodes\":[" << conv.physicsSmokeNodes.str() << "]}";
        }
        if (!conv.physicsRigidFirst) {
            pjson << ",\"BLEND_physics_rigidbody\":{\"nodes\":[" << conv.physicsRigidNodes.str() << "]}";
        }
        if (!conv.physicsParticleFirst) {
            pjson << ",\"BLEND_physics_particles\":{\"nodes\":[" << conv.physicsParticleNodes.str() << "]}";
        }
        if (!conv.physicsJointFirst) {
            pjson << ",\"BLEND_physics_constraints\":{\"nodes\":[" << conv.physicsJointNodes.str() << "]}";
        }
        // Host Bullet/PhysX path: same KHR keys as glTF when RigidBodyOb fields were converted.
        if (!conv.khrShapeFirst) {
            pjson << ",\"KHR_implicit_shapes\":{\"shapes\":[" << conv.khrShapes.str() << "]}";
            pjson << ",\"KHR_physics_rigid_bodies\":{\"physicsMaterials\":[" << conv.khrMaterials.str()
                  << "],\"collisionFilters\":[],\"physicsJoints\":[]}";
            pjson << ",\"nodes\":[" << conv.khrNodes.str() << ']';
        }
        pjson << '}';
        auto *psc = new aiPhysicsSidecar();
        psc->mExtensionsJson = BlendDupJson(pjson.str(), psc->mExtensionsJsonLength);
        AttachPhysicsSidecar(out, psc);
    }

    std::ostringstream json;
    json.setf(std::ios::fixed);
    json.precision(9);
    json << "{\"schemaVersion\":1,\"source\":{"
         << "\"format\":\"blend\"";
    if (blenderVersion != nullptr && blenderVersion[0] != '\0') {
        json << ",\"generator\":\"Blender " << blenderVersion << '"';
        out->mMetaData->Add(AI_METADATA_SOURCE_FORMAT_VERSION, aiString(blenderVersion));
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

    // S4 cameras / lights (typed already; bag for host inject inventory)
    json << ",\"cameras\":[";
    for (unsigned int ci = 0; ci < out->mNumCameras; ++ci) {
        if (ci) {
            json << ',';
        }
        json << "{\"index\":" << ci << ",\"name\":\"" << BlendJsonEscape(out->mCameras[ci]->mName.C_Str()) << "\"}";
    }
    json << "],\"lights\":[";
    for (unsigned int li = 0; li < out->mNumLights; ++li) {
        if (li) {
            json << ',';
        }
        json << "{\"index\":" << li << ",\"name\":\"" << BlendJsonEscape(out->mLights[li]->mName.C_Str()) << "\"}";
    }
    json << "],\"volumes\":[],\"pointInstancers\":[],\"composition\":{},\"skipped\":[";
    for (size_t si = 0; si < conv.skippedCategories.size(); ++si) {
        if (si) {
            json << ',';
        }
        json << '"' << BlendJsonEscape(conv.skippedCategories[si]) << '"';
    }
    json << "]}";

    out->mMetaData->Add(AI_METADATA_SCENE_SIDECAR_JSON, aiString(json.str()));
    out->mMetaData->Add(AI_METADATA_SOURCE_FORMAT, aiString("blend"));
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
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::ResolveImage(aiMaterial *out, const Material *mat, const MTex *tex, const Image *img, ConversionData &conv_data) {
    (void)mat;
    (void)tex;
    (void)conv_data;
    aiString name;

    // check if the file contents are bundled with the BLEND file
    if (img->packedfile) {
        name.data[0] = '*';
        name.length = 1 + ASSIMP_itoa10(name.data + 1, static_cast<unsigned int>(AI_MAXLEN - 1), static_cast<int32_t>(conv_data.textures->size()));

        conv_data.textures->push_back(new aiTexture());
        aiTexture *curTex = conv_data.textures->back();

        // usually 'img->name' will be the original file name of the embedded textures,
        // so we can extract the file extension from it.
        const size_t nlen = strlen(img->name);
        const char *s = img->name + nlen, *e = s;
        while (s >= img->name && *s != '.') {
            --s;
        }

        curTex->achFormatHint[0] = s + 1 > e ? '\0' : (char)::tolower((unsigned char)s[1]);
        curTex->achFormatHint[1] = s + 2 > e ? '\0' : (char)::tolower((unsigned char)s[2]);
        curTex->achFormatHint[2] = s + 3 > e ? '\0' : (char)::tolower((unsigned char)s[3]);
        curTex->achFormatHint[3] = '\0';

        // tex->mHeight = 0;
        curTex->mWidth = img->packedfile->size;
        uint8_t *ch = new uint8_t[curTex->mWidth];

        conv_data.db.reader->SetCurrentPos(static_cast<size_t>(img->packedfile->data->val));
        conv_data.db.reader->CopyAndAdvance(ch, curTex->mWidth);

        curTex->pcData = reinterpret_cast<aiTexel *>(ch);

        LogInfo("Reading embedded texture, original file was ", img->name);
    } else {
        name = aiString(img->name);
    }

    aiTextureType texture_type = aiTextureType_UNKNOWN;
    MTex::MapType map_type = tex->mapto;

    if (map_type & MTex::MapType_COL)
        texture_type = aiTextureType_DIFFUSE;
    else if (map_type & MTex::MapType_NORM) {
        if (tex->tex->imaflag & Tex::ImageFlags_NORMALMAP) {
            texture_type = aiTextureType_NORMALS;
        } else {
            texture_type = aiTextureType_HEIGHT;
        }
        out->AddProperty(&tex->norfac, 1, AI_MATKEY_BUMPSCALING);
    } else if (map_type & MTex::MapType_COLSPEC)
        texture_type = aiTextureType_SPECULAR;
    else if (map_type & MTex::MapType_COLMIR)
        texture_type = aiTextureType_REFLECTION;
    //else if (map_type & MTex::MapType_REF)
    else if (map_type & MTex::MapType_SPEC)
        texture_type = aiTextureType_SHININESS;
    else if (map_type & MTex::MapType_EMIT)
        texture_type = aiTextureType_EMISSIVE;
    //else if (map_type & MTex::MapType_ALPHA)
    //else if (map_type & MTex::MapType_HAR)
    //else if (map_type & MTex::MapType_RAYMIRR)
    //else if (map_type & MTex::MapType_TRANSLU)
    else if (map_type & MTex::MapType_AMB)
        texture_type = aiTextureType_AMBIENT;
    else if (map_type & MTex::MapType_DISPLACE)
        texture_type = aiTextureType_DISPLACEMENT;
    //else if (map_type & MTex::MapType_WARP)

    out->AddProperty(&name, AI_MATKEY_TEXTURE(texture_type,
                                    conv_data.next_texture[texture_type]++));
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::AddSentinelTexture(aiMaterial *out, const Material *mat, const MTex *tex, ConversionData &conv_data) {
    (void)mat;
    (void)tex;
    (void)conv_data;

    aiString name;
    name.length = ai_snprintf(name.data, AI_MAXLEN, "Procedural,num=%i,type=%s", conv_data.sentinel_cnt++,
            GetTextureTypeDisplayString(tex->tex->type));
    out->AddProperty(&name, AI_MATKEY_TEXTURE_DIFFUSE(
                                    conv_data.next_texture[aiTextureType_DIFFUSE]++));
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::ResolveTexture(aiMaterial *out, const Material *mat, const MTex *tex, ConversionData &conv_data) {
    const Tex *rtex = tex->tex.get();
    if (!rtex || !rtex->type) {
        return;
    }

    // We can't support most of the texture types because they're mostly procedural.
    // These are substituted by a dummy texture.
    const char *dispnam = "";
    switch (rtex->type) {
        // these are listed in blender's UI
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

        // these do no appear in the UI, why?
    case Tex::Type_POINTDENSITY:
    case Tex::Type_VOXELDATA:

        LogWarn("Encountered a texture with an unsupported type: ", dispnam);
        AddSentinelTexture(out, mat, tex, conv_data);
        break;

    case Tex::Type_IMAGE:
        if (!rtex->ima) {
            LogError("A texture claims to be an Image, but no image reference is given");
            break;
        }
        ResolveImage(out, mat, tex, rtex->ima.get(), conv_data);
        break;

    default:
        ai_assert(false);
    };
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::BuildDefaultMaterial(Blender::ConversionData &conv_data) {
    // add a default material if necessary
    unsigned int index = static_cast<unsigned int>(-1);
    for (aiMesh *mesh : conv_data.meshes.get()) {
        if (mesh->mMaterialIndex == static_cast<unsigned int>(-1)) {

            if (index == static_cast<unsigned int>(-1)) {
                // Setup a default material.
                std::shared_ptr<Material> p(new Material());
                const size_t len = ::strlen(AI_DEFAULT_MATERIAL_NAME);
                ai_assert(len < sizeof(p->id.name) - 2);
                memcpy(p->id.name + 2, AI_DEFAULT_MATERIAL_NAME, len);

                // Note: MSVC11 does not zero-initialize Material here, although it should.
                // Thus all relevant fields should be explicitly initialized. We cannot add
                // a default constructor to Material since the DNA codegen does not support
                // parsing it.
                p->r = p->g = p->b = 0.6f;
                p->specr = p->specg = p->specb = 0.6f;
                p->ambr = p->ambg = p->ambb = 0.0f;
                p->mirr = p->mirg = p->mirb = 0.0f;
                p->emit = 0.f;
                p->alpha = 0.f;
                p->har = 0;

                index = static_cast<unsigned int>(conv_data.materials_raw.size());
                conv_data.materials_raw.push_back(p);
                LogInfo("Adding default material");
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

    int diffuseRamp = 0;
    result->AddProperty(&diffuseRamp, 1, "$mat.blend.diffuse.ramp", 0, 0);

    aiColor3D specularColor(source->specr, source->specg, source->specb);
    result->AddProperty(&specularColor, 1, "$mat.blend.specular.color", 0, 0);

    float specularIntensity = source->spec;
    result->AddProperty(&specularIntensity, 1, "$mat.blend.specular.intensity", 0, 0);

    int specularShader = source->spec_shader;
    result->AddProperty(&specularShader, 1, "$mat.blend.specular.shader", 0, 0);

    int specularRamp = 0;
    result->AddProperty(&specularRamp, 1, "$mat.blend.specular.ramp", 0, 0);

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

    for (const std::shared_ptr<Material> &mat : conv_data.materials_raw) {
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
        // For any new material field handled here, the default material above must be updated with an appropriate default value.

        // set material name
        aiString name = aiString(mat->id.name + 2); // skip over the name prefix 'MA'
        mout->AddProperty(&name, AI_MATKEY_NAME);

        // C6: flatten active Principled/Glass/Glossy/Diffuse/Emission + follow Image Tex links.
        const BlendShaderFlat flat = BlendFlattenMaterialNodes(*mat);
        if (flat.ok) {
            BlendApplyFlatToMaterial(mout, flat, conv_data);
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

            ResolveTexture(mout, mat.get(), mat->mtex[i].get(), conv_data);
        }

        AddBlendParams(mout, mat.get());
        conv_data.materialSidecarJson.push_back(BlendBuildMaterialSidecarJson(*mat, flat));
        if (mat->nodetree && !flat.ok) {
            conv_data.skippedCategories.push_back("BLEND_shader_nodegraph");
        }
    }
}

// ------------------------------------------------------------------------------------------------
void BlenderImporter::CheckActualType(const ElemBase *dt, const char *check) {
    ai_assert(dt);
    if (strcmp(dt->dna_type, check)) {
        ThrowException("Expected object at ", std::hex, dt, " to be of type `", check,
                "`, but it claims to be a `", dt->dna_type, "`instead");
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

    // Blender 5+: classic *mvert/*mloop/*mpoly may be empty; hydrate from AttributeStorage.
    if (static_cast<size_t>(mesh->totvert) > mesh->mvert.size() ||
            (mesh->totloop > 0 && static_cast<size_t>(mesh->totloop) > mesh->mloop.size()) ||
            (mesh->totpoly > 0 && static_cast<size_t>(mesh->totpoly) > mesh->mpoly.size())) {
        Mesh *mutableMesh = const_cast<Mesh *>(mesh);
        if (!HydrateMeshFromAttributeStorage(*mutableMesh, conv_data.db)) {
            LogWarn("mesh '", mesh->id.name + 2,
                    "' has no classic arrays and AttributeStorage hydrate failed "
                    "(totvert=", mesh->totvert, " mvert=", mesh->mvert.size(),
                    " totloop=", mesh->totloop, " mloop=", mesh->mloop.size(), ")");
            return;
        }
        LogInfo("hydrated mesh '", mesh->id.name + 2,
                "' from AttributeStorage (verts=", mesh->mvert.size(),
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
    for (MyPair &it : per_mat) {

        mat_num_to_mesh_idx[it.first] = temp->size();
        temp->push_back(new aiMesh());

        aiMesh *out = temp->back();
        out->mVertices = new aiVector3D[per_mat_verts[it.first]];
        out->mNormals = new aiVector3D[per_mat_verts[it.first]];

        //out->mNumFaces = 0
        //out->mNumVertices = 0
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

    // TODO should we create the TextureUVMapping map in Convert<Material> to prevent redundant processing?

    // create texture <-> uvname mapping for all materials
    // key is texture number, value is data *
    typedef std::map<uint32_t, const MLoopUV *> TextureUVMapping;
    // key is material number, value is the TextureUVMapping for the material
    typedef std::map<uint32_t, TextureUVMapping> MaterialTextureUVMappings;
    MaterialTextureUVMappings matTexUvMappings;
    const uint32_t maxMat = static_cast<uint32_t>(mesh->mat.size());
    for (uint32_t m = 0; m < maxMat; ++m) {
        // get material by index
        const std::shared_ptr<Material> pMat = mesh->mat[m];
        // DNA may leave empty mat slots; dereferencing an empty shared_ptr crashes
        // (seen on large fluid demos such as lava_fluid-viscosity-demo.blend).
        if (!pMat) {
            continue;
        }
        TextureUVMapping texuv;
        const uint32_t maxTex = sizeof(pMat->mtex) / sizeof(pMat->mtex[0]);
        for (uint32_t t = 0; t < maxTex; ++t) {
            if (pMat->mtex[t] && pMat->mtex[t]->uvname[0]) {
                // get the CustomData layer for given uvname and correct type
                const ElemBase *pLoop = getCustomDataLayerData(mesh->ldata, CD_MLOOPUV, pMat->mtex[t]->uvname);
                if (pLoop) {
                    texuv.insert(std::make_pair(t, dynamic_cast<const MLoopUV *>(pLoop)));
                }
            }
        }
        if (texuv.size()) {
            matTexUvMappings.insert(std::make_pair(m, texuv));
        }
    }

    // collect texture coordinates, they're stored in a separate per-face buffer
    if (mesh->mtface || mesh->mloopuv) {
        if (mesh->totface > static_cast<int>(mesh->mtface.size())) {
            ThrowException("Number of UV faces is larger than the corresponding UV face array (#1)");
        }
        for (std::vector<aiMesh *>::iterator it = temp->begin() + old; it != temp->end(); ++it) {
            ai_assert(0 != (*it)->mNumVertices);
            ai_assert(0 != (*it)->mNumFaces);
            const auto itMatTexUvMapping = matTexUvMappings.find((*it)->mMaterialIndex);
            if (itMatTexUvMapping == matTexUvMappings.end()) {
                // default behaviour like before
                (*it)->mTextureCoords[0] = new aiVector3D[(*it)->mNumVertices];
            } else {
                // create texture coords for every mapped tex
                for (uint32_t i = 0; i < itMatTexUvMapping->second.size(); ++i) {
                    (*it)->mTextureCoords[i] = new aiVector3D[(*it)->mNumVertices];
                }
            }
            (*it)->mNumFaces = (*it)->mNumVertices = 0;
        }

        for (int i = 0; i < mesh->totface; ++i) {
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

            const auto itMatTexUvMapping = matTexUvMappings.find(v.mat_nr);
            if (itMatTexUvMapping == matTexUvMappings.end()) {
                // old behavior
                aiVector3D *vo = &out->mTextureCoords[0][out->mNumVertices];
                for (unsigned int j = 0; j < f.mNumIndices; ++j, ++vo, ++out->mNumVertices) {
                    const MLoopUV &uv = mesh->mloopuv[v.loopstart + j];
                    vo->x = uv.uv[0];
                    vo->y = uv.uv[1];
                }
            } else {
                // create textureCoords for every mapped tex
                for (uint32_t m = 0; m < itMatTexUvMapping->second.size(); ++m) {
                    const MLoopUV *tm = itMatTexUvMapping->second[m];
                    aiVector3D *vo = &out->mTextureCoords[m][out->mNumVertices];
                    uint32_t j = 0;
                    for (; j < f.mNumIndices; ++j, ++vo) {
                        const MLoopUV &uv = tm[v.loopstart + j];
                        vo->x = uv.uv[0];
                        vo->y = uv.uv[1];
                    }
                    // only update written mNumVertices in last loop
                    // TODO why must the numVertices be incremented here?
                    if (m == itMatTexUvMapping->second.size() - 1) {
                        out->mNumVertices += j;
                    }
                }
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

    // S1: stash CustomData layer inventory for mesh sidecars (attached after scene owns meshes).
    {
        const std::string layerJson = BuildBlendCustomDataLayersJson(mesh);
        if (conv_data.meshSidecarJson.size() < temp->size()) {
            conv_data.meshSidecarJson.resize(temp->size());
        }
        for (size_t mi = old; mi < temp->size(); ++mi) {
            conv_data.meshSidecarJson[mi] = layerJson;
        }
    }

    return;
}

// ------------------------------------------------------------------------------------------------
aiCamera *BlenderImporter::ConvertCamera(const Scene & /*in*/, const Object *obj, const Camera *cam, ConversionData & /*conv_data*/) {
    std::unique_ptr<aiCamera> out(new aiCamera());
    out->mName = obj->id.name + 2;
    out->mPosition = aiVector3D(0.f, 0.f, 0.f);
    out->mUp = aiVector3D(0.f, 1.f, 0.f);
    out->mLookAt = aiVector3D(0.f, 0.f, -1.f);
    if (cam->sensor_x && cam->lens) {
        out->mHorizontalFOV = 2.f * std::atan2(cam->sensor_x, 2.f * cam->lens);
    }
    out->mClipPlaneNear = cam->clipsta;
    out->mClipPlaneFar = cam->clipend;

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
        case Object ::Type_EMPTY:
            break; // do nothing

            // supported object types
        case Object ::Type_MESH: {
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
        case Object ::Type_LAMP: {
            CheckActualType(obj->data.get(), "Lamp");
            aiLight *mesh = ConvertLight(in, obj, static_cast<const Lamp *>(obj->data.get()), conv_data);

            if (mesh) {
                conv_data.lights->push_back(mesh);
            }
        } break;
        case Object ::Type_CAMERA: {
            CheckActualType(obj->data.get(), "Camera");
            aiCamera *mesh = ConvertCamera(in, obj, static_cast<const Camera *>(obj->data.get()), conv_data);

            if (mesh) {
                conv_data.cameras->push_back(mesh);
            }
        } break;

            // unsupported object types / log, but do not break
        case Object ::Type_CURVE:
            NotSupportedObjectType(obj, "Curve");
            break;
        case Object ::Type_SURF:
            NotSupportedObjectType(obj, "Surface");
            break;
        case Object ::Type_FONT:
            NotSupportedObjectType(obj, "Font");
            break;
        case Object ::Type_MBALL:
            NotSupportedObjectType(obj, "MetaBall");
            break;
        case Object ::Type_WAVE:
            NotSupportedObjectType(obj, "Wave");
            break;
        case Object ::Type_LATTICE:
            NotSupportedObjectType(obj, "Lattice");
            break;

            // invalid or unknown type
        default:
            break;
        }
    }

    for (unsigned int x = 0; x < 4; ++x) {
        for (unsigned int y = 0; y < 4; ++y) {
            node->mTransformation[y][x] = obj->obmat[x][y];
        }
    }

    aiMatrix4x4 m = parentTransform;
    m = m.Inverse();

    node->mTransformation = m * node->mTransformation;

    if (children.size()) {
        node->mNumChildren = static_cast<unsigned int>(children.size());
        aiNode **nd = node->mChildren = new aiNode *[node->mNumChildren]();
        for (const Object *nobj : children) {
            *nd = ConvertNode(in, nobj, conv_data, node->mTransformation * parentTransform);
            (*nd++)->mParent = node.get();
        }
    }

    // apply modifiers
    modifier_cache->ApplyModifiers(*node, conv_data, in, *obj);
    CollectBlendPhysicsModifiers(obj, conv_data);

    return node.release();
}

BlenderImporter::StreamOrError BlenderImporter::ParseMagicToken(const std::string &pFile, IOSystem *pIOHandler) const {
    std::shared_ptr<IOStream> stream(pIOHandler->Open(pFile, "rb"));
    if (stream == nullptr) {
        return {{}, {}, "Could not open file for reading"};
    }

    char magic[8] = { 0 };
    stream->Read(magic, 7, 1);
    if (strcmp(magic, Token) == 0) {
        return {stream, {}, {}};
    }

    // Check for gzip (legacy) or zstd (Blender 3+ default) compressed blend files.
#ifdef ASSIMP_BUILD_NO_COMPRESSED_BLEND
    return {{}, {}, "BLENDER magic bytes are missing, is this file compressed (Assimp was built without decompression support)?"};
#else
    const bool isGzip = (magic[0] == 0x1f && static_cast<uint8_t>(magic[1]) == 0x8b);
    // Zstandard magic: 0x28 0xB5 0x2F 0xFD (little-endian frame header)
    const bool isZstd = (static_cast<uint8_t>(magic[0]) == 0x28 &&
            static_cast<uint8_t>(magic[1]) == 0xb5 &&
            static_cast<uint8_t>(magic[2]) == 0x2f &&
            static_cast<uint8_t>(magic[3]) == 0xfd);

    if (!isGzip && !isZstd) {
        return {{}, {}, "BLENDER magic bytes are missing, couldn't find GZIP or Zstandard header either"};
    }

    stream->Seek(0L, aiOrigin_SET);
    auto uncompressed = std::make_shared<std::vector<char>>();
    size_t total = 0;

    if (isGzip) {
        LogDebug("Found no BLENDER magic word but a GZIP header, might be a compressed file");
        if (magic[2] != 8) {
            return {{}, {}, "Unsupported GZIP compression method"};
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
        const void *src = reader->GetPtr();
        const size_t srcSize = reader->GetRemainingSize();
        ZSTD_DCtx *dctx = ZSTD_createDCtx();
        if (dctx == nullptr) {
            return {{}, {}, "Failed to allocate Zstandard decompression context"};
        }
        uncompressed->resize(srcSize * 8 + 256 * 1024);
        ZSTD_inBuffer input{ src, srcSize, 0 };
        ZSTD_outBuffer output{ uncompressed->data(), uncompressed->size(), 0 };
        while (input.pos < input.size) {
            if (output.pos == output.size) {
                const size_t newSize = uncompressed->size() * 2;
                if (newSize > (1ull << 31)) {
                    ZSTD_freeDCtx(dctx);
                    return {{}, {}, "Zstandard BLEND payload is unreasonably large"};
                }
                uncompressed->resize(newSize);
                output.dst = uncompressed->data();
                output.size = uncompressed->size();
            }
            const size_t ret = ZSTD_decompressStream(dctx, &output, &input);
            if (ZSTD_isError(ret)) {
                ZSTD_freeDCtx(dctx);
                return {{}, {}, std::string("Zstandard decompress failed: ") + ZSTD_getErrorName(ret)};
            }
            // ret==0 means a frame finished; continue for further frames / skippable frames.
        }
        ZSTD_freeDCtx(dctx);
        total = output.pos;
        uncompressed->resize(total);
#else
        return {{}, {}, "BLENDER file appears Zstandard-compressed, but Assimp was built without ZSTD support"};
#endif
    }

    // replace the input stream with a memory stream
    stream = std::make_shared<MemoryIOStream>(reinterpret_cast<uint8_t *>(uncompressed->data()), total);

    // .. and retry
    stream->Read(magic, 7, 1);
    if (strcmp(magic, Token) == 0) {
        return {stream, uncompressed, {}};
    }
    return {{}, {}, isGzip ? "Found no BLENDER magic word in decompressed GZIP file" : "Found no BLENDER magic word in decompressed Zstandard file"};
#endif
}

#endif // ASSIMP_BUILD_NO_BLEND_IMPORTER
