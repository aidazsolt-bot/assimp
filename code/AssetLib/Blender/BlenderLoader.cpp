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
#include <cstring>
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
        conv.physicsSoftNodes << "{\"name\":\"" << BlendJsonEscape(objName)
                              << "\",\"type\":\"softbody\",\"settings\":{\"source\":\"Object.soft\"}}";
    }
    // Rigid Body DNA presence (KHR-shaped field map still open).
    if (obj->rigidbody_object) {
        if (!conv.physicsRigidFirst) {
            conv.physicsRigidNodes << ',';
        }
        conv.physicsRigidFirst = false;
        conv.physicsRigidNodes << "{\"name\":\"" << BlendJsonEscape(objName)
                               << "\",\"type\":\"rigidbody\",\"settings\":{\"source\":\"Object.rigidbody_object\"}}";
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

    char version[4] = { 0 };
    file.i64bit = (stream->Read(version, 1, 1), version[0] == '-');
    file.little = (stream->Read(version, 1, 1), version[0] == 'v');

    stream->Read(version, 3, 1);
    version[3] = '\0';

    LogInfo("Blender version is ", version[0], ".", version + 1,
            " (64bit: ", file.i64bit ? "true" : "false",
            ", little endian: ", file.little ? "true" : "false", ")");

    char blenderVersionStr[8] = { 0 };
    blenderVersionStr[0] = version[0];
    blenderVersionStr[1] = '.';
    blenderVersionStr[2] = version[1];
    blenderVersionStr[3] = version[2];
    blenderVersionStr[4] = '\0';

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
        SectionParser parser(*out.reader, out.i64bit);

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
    ss.Convert(out, file);

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
    root->mChildren = new aiNode *[root->mNumChildren]();

    for (unsigned int i = 0; i < static_cast<unsigned int>(root_objects.size()); ++i) {
        root->mChildren[i] = ConvertNode(in, root_objects[i], conv_data, aiMatrix4x4());
        root->mChildren[i]->mParent = root;
    }

    // For each subcollection create a new node to represent it
    unsigned int iterator = static_cast<unsigned int>(root_objects.size());
    for (std::shared_ptr<CollectionChild> cur = std::static_pointer_cast<CollectionChild>(collection->children.first); cur; cur = cur->next) {
        if (cur->collection) {
            root->mChildren[iterator] = new aiNode(cur->collection->id.name + 2); // skip over the name prefix 'OB'
            root->mChildren[iterator]->mParent = root;
            ParseSubCollection(in, root->mChildren[iterator], cur->collection, conv_data);
        }
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

    // S3 material remainder bag (Principled factors live on aiMaterial; sidecar carries source tag)
    for (unsigned int mati = 0; mati < out->mNumMaterials; ++mati) {
        std::ostringstream mjson;
        mjson << "{\"schemaVersion\":1,\"source\":{\"format\":\"blend\"},\"BLEND_shader\":{";
        if (mati < conv.materials_raw.size() && conv.materials_raw[mati]) {
            mjson << "\"name\":\"" << BlendJsonEscape(conv.materials_raw[mati]->id.name + 2) << '"';
        }
        mjson << "}}";
        auto *msc = new aiMaterialSidecar();
        msc->mExtensionsJson = BlendDupJson(mjson.str(), msc->mExtensionsJsonLength);
        AttachMaterialSidecar(out, mati, msc);
    }

    // S5/S6 physics from modifiers + Object.soft / rigidbody_object presence
    if (conv.physicsRigidFirst) {
        conv.skippedCategories.push_back("BLEND_physics_rigidbody_object_pointer");
    }
    conv.skippedCategories.push_back("BLEND_action_nla_animation_sidecar");
    conv.skippedCategories.push_back("BLEND_shader_network_full_nodegraph");
    const bool hasPhys = !conv.physicsSoftFirst || !conv.physicsClothFirst ||
                         !conv.physicsFluidFirst || !conv.physicsSmokeFirst ||
                         !conv.physicsRigidFirst;
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
            // Presence-only until RigidBodyOb fields map to KHR_physics_rigid_bodies.
            pjson << ",\"BLEND_physics_rigidbody\":{\"nodes\":[" << conv.physicsRigidNodes.str() << "]}";
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
    json << ",\"metersPerUnit\":" << metersPerUnit
         << ",\"timeCodesPerSecond\":" << fps
         << ",\"framesPerSecond\":" << fps
         << ",\"startTimeCode\":" << static_cast<double>(in.r.sfra)
         << ",\"endTimeCode\":" << static_cast<double>(in.r.efra)
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

        // basic material colors
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

        col = aiColor3D(mat->ambr, mat->ambg, mat->ambb);
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

    if (static_cast<size_t>(mesh->totvert) > mesh->mvert.size()) {
        ThrowException("Number of vertices is larger than the corresponding array");
    }

    if (static_cast<size_t>(mesh->totloop) > mesh->mloop.size()) {
        ThrowException("Number of vertices is larger than the corresponding array");
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

            if (static_cast<size_t>(it.first) >= mesh->mat.size()) {
                ThrowException("Material index is out of range");
            }

            std::shared_ptr<Material> mat = mesh->mat[it.first];
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

            if (loop.v >= mesh->totvert) {
                ThrowException("Vertex index out of range");
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
