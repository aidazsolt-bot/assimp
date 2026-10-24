/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the following
conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the materials provided with the
  distribution.

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
---------------------------------------------------------------------------
*/

/** @file SOGLoader.cpp
 *  @brief PlayCanvas SOG v2 importer -> aiGaussianSplat (KHR-native).
 */

#ifndef ASSIMP_BUILD_NO_SOG_IMPORTER

#include "SOGLoader.h"

#include <assimp/DefaultLogger.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/ZipArchiveIOSystem.h>
#include <assimp/gaussian.h>
#include <assimp/importerdesc.h>
#include <assimp/scene.h>

#include <rapidjson/document.h>
#include <webp/decode.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Assimp;

namespace {

static constexpr aiImporterDesc desc = {
        "SOG 3D Gaussian Splat Importer",
        "",
        "",
        "PlayCanvas SOG v2 (ZIP: meta.json + WebP); .sog / .ssog",
        aiImporterFlags_SupportBinaryFlavour,
        0,
        0,
        0,
        0,
        "sog ssog"
};

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // w*h*4
};

inline float Unlog(float n) {
    const float a = std::fabs(n);
    const float e = std::exp(a) - 1.f;
    return n < 0.f ? -e : e;
}

inline float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

/** Smallest-three quat unpack -> [w,x,y,z]. mode = A-252 in 0..3. */
void UnpackQuat(uint8_t pr, uint8_t pg, uint8_t pb, uint8_t pa, float outWxyz[4]) {
    const int mode = int(pa) - 252;
    if (mode < 0 || mode > 3) {
        outWxyz[0] = 1.f;
        outWxyz[1] = 0.f;
        outWxyz[2] = 0.f;
        outWxyz[3] = 0.f;
        return;
    }
    const float invSqrt2 = 1.f / std::sqrt(2.f);
    const float a = ((float(pr) / 255.f) * 2.f - 1.f) * invSqrt2;
    const float b = ((float(pg) / 255.f) * 2.f - 1.f) * invSqrt2;
    const float c = ((float(pb) / 255.f) * 2.f - 1.f) * invSqrt2;
    const float t = a * a + b * b + c * c;
    const float d = std::sqrt(std::max(0.f, 1.f - t));
    // Place omitted (largest) component at mode index in [w,x,y,z].
    float q[4] = { 0.f, 0.f, 0.f, 0.f };
    if (mode == 0) {
        q[0] = d;
        q[1] = a;
        q[2] = b;
        q[3] = c;
    } else if (mode == 1) {
        q[0] = a;
        q[1] = d;
        q[2] = b;
        q[3] = c;
    } else if (mode == 2) {
        q[0] = a;
        q[1] = b;
        q[2] = d;
        q[3] = c;
    } else {
        q[0] = a;
        q[1] = b;
        q[2] = c;
        q[3] = d;
    }
    outWxyz[0] = q[0];
    outWxyz[1] = q[1];
    outWxyz[2] = q[2];
    outWxyz[3] = q[3];
}

std::vector<uint8_t> ReadZipEntry(ZipArchiveIOSystem &archive, const std::string &name) {
    if (!archive.Exists(name.c_str())) {
        throw DeadlyImportError("SOG: missing archive entry '", name, "'.");
    }
    std::unique_ptr<IOStream> stream(archive.Open(name.c_str()));
    if (!stream) {
        throw DeadlyImportError("SOG: failed to open archive entry '", name, "'.");
    }
    const size_t sz = stream->FileSize();
    if (sz == 0 || sz == SIZE_MAX) {
        throw DeadlyImportError("SOG: empty archive entry '", name, "'.");
    }
    std::vector<uint8_t> buf(sz);
    if (stream->Read(buf.data(), 1, sz) != sz) {
        throw DeadlyImportError("SOG: short read on archive entry '", name, "'.");
    }
    return buf;
}

RgbaImage DecodeWebP(const std::vector<uint8_t> &bytes, const char *label) {
    int w = 0, h = 0;
    uint8_t *rgba = WebPDecodeRGBA(bytes.data(), bytes.size(), &w, &h);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) {
            WebPFree(rgba);
        }
        throw DeadlyImportError("SOG: WebP decode failed for ", label, ".");
    }
    RgbaImage img;
    img.width = w;
    img.height = h;
    const size_t n = size_t(w) * size_t(h) * 4u;
    img.rgba.assign(rgba, rgba + n);
    WebPFree(rgba);
    return img;
}

void ReadCodebook256(const rapidjson::Value &arr, float out[256], const char *label) {
    if (!arr.IsArray() || arr.Size() < 256) {
        throw DeadlyImportError("SOG: ", label, " codebook must have 256 floats.");
    }
    for (unsigned i = 0; i < 256; ++i) {
        if (!arr[i].IsNumber()) {
            throw DeadlyImportError("SOG: ", label, " codebook entry is not a number.");
        }
        out[i] = static_cast<float>(arr[i].GetDouble());
    }
}

std::string RequireFileName(const rapidjson::Value &files, unsigned index, const char *label) {
    if (!files.IsArray() || files.Size() <= index || !files[index].IsString()) {
        throw DeadlyImportError("SOG: ", label, " files[", index, "] missing.");
    }
    return files[index].GetString();
}

} // namespace

// ------------------------------------------------------------------------------------------------
bool SOGImporter::CanRead(const std::string &pFile, IOSystem *pIOHandler, bool checkSig) const {
    const std::string extension = GetExtension(pFile);
    if (extension == "sog" || extension == "ssog") {
        return true;
    }
    if (!checkSig || !pIOHandler) {
        return false;
    }
    // Bundled SOG is a ZIP containing meta.json.
    if (!ZipArchiveIOSystem::isZipArchive(pIOHandler, pFile)) {
        return false;
    }
    ZipArchiveIOSystem archive(pIOHandler, pFile);
    return archive.Exists("meta.json"); // const char*
}

// ------------------------------------------------------------------------------------------------
const aiImporterDesc *SOGImporter::GetInfo() const {
    return &desc;
}

// ------------------------------------------------------------------------------------------------
void SOGImporter::InternReadFile(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
    if (!ZipArchiveIOSystem::isZipArchive(pIOHandler, pFile)) {
        throw DeadlyImportError("SOG: file is not a ZIP archive: ", pFile);
    }
    ZipArchiveIOSystem archive(pIOHandler, pFile);

    const std::vector<uint8_t> metaBytes = ReadZipEntry(archive, "meta.json");
    rapidjson::Document meta;
    meta.Parse(reinterpret_cast<const char *>(metaBytes.data()), metaBytes.size());
    if (meta.HasParseError() || !meta.IsObject()) {
        throw DeadlyImportError("SOG: meta.json parse failed.");
    }
    if (!meta.HasMember("version") || !meta["version"].IsNumber() || meta["version"].GetInt() != 2) {
        throw DeadlyImportError("SOG: unsupported meta version (need 2).");
    }
    if (!meta.HasMember("count") || !meta["count"].IsNumber()) {
        throw DeadlyImportError("SOG: meta.count missing.");
    }
    const unsigned numPoints = static_cast<unsigned>(meta["count"].GetUint64());
    if (numPoints == 0) {
        throw DeadlyImportError("SOG: meta.count is zero.");
    }

    if (!meta.HasMember("means") || !meta["means"].IsObject() ||
            !meta.HasMember("scales") || !meta["scales"].IsObject() ||
            !meta.HasMember("quats") || !meta["quats"].IsObject() ||
            !meta.HasMember("sh0") || !meta["sh0"].IsObject()) {
        throw DeadlyImportError("SOG: meta missing means/scales/quats/sh0.");
    }

    const rapidjson::Value &means = meta["means"];
    const rapidjson::Value &scales = meta["scales"];
    const rapidjson::Value &quats = meta["quats"];
    const rapidjson::Value &sh0 = meta["sh0"];

    if (!means.HasMember("mins") || !means["mins"].IsArray() || means["mins"].Size() < 3 ||
            !means.HasMember("maxs") || !means["maxs"].IsArray() || means["maxs"].Size() < 3) {
        throw DeadlyImportError("SOG: means.mins/maxs invalid.");
    }
    float meansMin[3], meansMax[3];
    for (int i = 0; i < 3; ++i) {
        meansMin[i] = static_cast<float>(means["mins"][i].GetDouble());
        meansMax[i] = static_cast<float>(means["maxs"][i].GetDouble());
    }

    float scaleCode[256];
    float sh0Code[256];
    ReadCodebook256(scales["codebook"], scaleCode, "scales");
    ReadCodebook256(sh0["codebook"], sh0Code, "sh0");

    const std::string meansLoName = RequireFileName(means["files"], 0, "means");
    const std::string meansHiName = RequireFileName(means["files"], 1, "means");
    const std::string scalesName = RequireFileName(scales["files"], 0, "scales");
    const std::string quatsName = RequireFileName(quats["files"], 0, "quats");
    const std::string sh0Name = RequireFileName(sh0["files"], 0, "sh0");

    const RgbaImage meansLo = DecodeWebP(ReadZipEntry(archive, meansLoName), meansLoName.c_str());
    const RgbaImage meansHi = DecodeWebP(ReadZipEntry(archive, meansHiName), meansHiName.c_str());
    const RgbaImage scalesImg = DecodeWebP(ReadZipEntry(archive, scalesName), scalesName.c_str());
    const RgbaImage quatsImg = DecodeWebP(ReadZipEntry(archive, quatsName), quatsName.c_str());
    const RgbaImage sh0Img = DecodeWebP(ReadZipEntry(archive, sh0Name), sh0Name.c_str());

    const size_t capacity = size_t(meansLo.width) * size_t(meansLo.height);
    if (capacity < numPoints ||
            size_t(meansHi.width) * size_t(meansHi.height) < numPoints ||
            size_t(scalesImg.width) * size_t(scalesImg.height) < numPoints ||
            size_t(quatsImg.width) * size_t(quatsImg.height) < numPoints ||
            size_t(sh0Img.width) * size_t(sh0Img.height) < numPoints) {
        throw DeadlyImportError("SOG: texture capacity smaller than meta.count.");
    }

    // Optional higher-order SH palette.
    unsigned restCoeffs = 0;
    unsigned shBands = 0;
    unsigned paletteCount = 0;
    float shNCode[256];
    RgbaImage centroids;
    RgbaImage labels;
    bool hasShN = false;
    if (meta.HasMember("shN") && meta["shN"].IsObject()) {
        const rapidjson::Value &shN = meta["shN"];
        if (!shN.HasMember("bands") || !shN["bands"].IsNumber()) {
            throw DeadlyImportError("SOG: shN.bands missing.");
        }
        shBands = static_cast<unsigned>(shN["bands"].GetUint());
        static const unsigned kCoeffsPerBand[4] = { 0, 3, 8, 15 };
        if (shBands < 1 || shBands > 3) {
            throw DeadlyImportError("SOG: shN.bands must be 1..3.");
        }
        const unsigned shCoeffs = kCoeffsPerBand[shBands];
        restCoeffs = shCoeffs * 3u;
        ReadCodebook256(shN["codebook"], shNCode, "shN");
        if (!shN.HasMember("count") || !shN["count"].IsNumber()) {
            throw DeadlyImportError("SOG: shN.count missing.");
        }
        paletteCount = static_cast<unsigned>(shN["count"].GetUint());
        const std::string cenName = RequireFileName(shN["files"], 0, "shN");
        const std::string labName = RequireFileName(shN["files"], 1, "shN");
        centroids = DecodeWebP(ReadZipEntry(archive, cenName), cenName.c_str());
        labels = DecodeWebP(ReadZipEntry(archive, labName), labName.c_str());
        if (size_t(labels.width) * size_t(labels.height) < numPoints) {
            throw DeadlyImportError("SOG: shN labels texture too small.");
        }
        if (centroids.width != int(64u * shCoeffs)) {
            throw DeadlyImportError("SOG: shN centroids width mismatch for bands.");
        }
        hasShN = true;
    }

    std::unique_ptr<aiGaussianSplat> splat(new aiGaussianSplat());
    splat->mNumPoints = numPoints;
    splat->mNumRestCoeffs = restCoeffs;
    splat->mDC = new aiVector3D[numPoints];
    splat->mScale = new aiVector3D[numPoints];
    splat->mRotation = new aiColor4D[numPoints];
    splat->mOpacity = new ai_real[numPoints];
    if (restCoeffs > 0) {
        splat->mRest = new ai_real[size_t(numPoints) * restCoeffs];
        std::memset(splat->mRest, 0, size_t(numPoints) * restCoeffs * sizeof(ai_real));
    }

    aiMesh *mesh = new aiMesh();
    mesh->mName.Set("SOG_Gaussians");
    mesh->mPrimitiveTypes = aiPrimitiveType_POINT;
    mesh->mNumVertices = numPoints;
    mesh->mVertices = new aiVector3D[numPoints];
    mesh->mNumFaces = numPoints;
    mesh->mFaces = new aiFace[numPoints];
    mesh->mMaterialIndex = 0;

    for (unsigned i = 0; i < numPoints; ++i) {
        const size_t o4 = size_t(i) * 4u;

        const unsigned qx = unsigned(meansLo.rgba[o4 + 0]) | (unsigned(meansHi.rgba[o4 + 0]) << 8);
        const unsigned qy = unsigned(meansLo.rgba[o4 + 1]) | (unsigned(meansHi.rgba[o4 + 1]) << 8);
        const unsigned qz = unsigned(meansLo.rgba[o4 + 2]) | (unsigned(meansHi.rgba[o4 + 2]) << 8);
        const float nx = Lerp(meansMin[0], meansMax[0], float(qx) / 65535.f);
        const float ny = Lerp(meansMin[1], meansMax[1], float(qy) / 65535.f);
        const float nz = Lerp(meansMin[2], meansMax[2], float(qz) / 65535.f);
        mesh->mVertices[i].x = Unlog(nx);
        mesh->mVertices[i].y = Unlog(ny);
        mesh->mVertices[i].z = Unlog(nz);

        mesh->mFaces[i].mNumIndices = 1;
        mesh->mFaces[i].mIndices = new unsigned int[1];
        mesh->mFaces[i].mIndices[0] = i;

        // Scales: codebook is log-domain -> linear via exp (SOG format §3.3).
        float sx = std::exp(scaleCode[scalesImg.rgba[o4 + 0]]);
        float sy = std::exp(scaleCode[scalesImg.rgba[o4 + 1]]);
        float sz = std::exp(scaleCode[scalesImg.rgba[o4 + 2]]);
        if (sx < 0.f) {
            sx = 0.f;
        }
        if (sy < 0.f) {
            sy = 0.f;
        }
        if (sz < 0.f) {
            sz = 0.f;
        }
        splat->mScale[i].x = sx;
        splat->mScale[i].y = sy;
        splat->mScale[i].z = sz;

        float wxyz[4];
        UnpackQuat(quatsImg.rgba[o4 + 0], quatsImg.rgba[o4 + 1], quatsImg.rgba[o4 + 2],
                quatsImg.rgba[o4 + 3], wxyz);
        float qw = wxyz[0], qx_ = wxyz[1], qy_ = wxyz[2], qz_ = wxyz[3];
        const double qlen = std::sqrt(double(qx_) * qx_ + double(qy_) * qy_ + double(qz_) * qz_ + double(qw) * qw);
        if (qlen > 0.0) {
            const float inv = static_cast<float>(1.0 / qlen);
            qx_ *= inv;
            qy_ *= inv;
            qz_ *= inv;
            qw *= inv;
        } else {
            qx_ = qy_ = qz_ = 0.f;
            qw = 1.f;
        }
        // KHR / glTF xyzw as (r,g,b,a).
        splat->mRotation[i].r = qx_;
        splat->mRotation[i].g = qy_;
        splat->mRotation[i].b = qz_;
        splat->mRotation[i].a = qw;

        // SH DC from codebook (KHR SH coeffs, not display RGB).
        splat->mDC[i].x = sh0Code[sh0Img.rgba[o4 + 0]];
        splat->mDC[i].y = sh0Code[sh0Img.rgba[o4 + 1]];
        splat->mDC[i].z = sh0Code[sh0Img.rgba[o4 + 2]];

        float op = float(sh0Img.rgba[o4 + 3]) / 255.f;
        if (op < 0.f) {
            op = 0.f;
        } else if (op > 1.f) {
            op = 1.f;
        }
        splat->mOpacity[i] = op;

        if (hasShN && restCoeffs > 0) {
            const unsigned label = unsigned(labels.rgba[o4 + 0]) | (unsigned(labels.rgba[o4 + 1]) << 8);
            const unsigned shCoeffs = restCoeffs / 3u;
            if (label < paletteCount) {
                const unsigned cy = label / 64u;
                const unsigned cxBase = (label % 64u) * shCoeffs;
                ai_real *dst = splat->mRest + size_t(i) * restCoeffs;
                for (unsigned c = 0; c < shCoeffs; ++c) {
                    const unsigned cx = cxBase + c;
                    if (cy >= unsigned(centroids.height) || cx >= unsigned(centroids.width)) {
                        continue;
                    }
                    const size_t idx = (size_t(cy) * size_t(centroids.width) + size_t(cx)) * 4u;
                    dst[c] = shNCode[centroids.rgba[idx + 0]];
                    dst[c + shCoeffs] = shNCode[centroids.rgba[idx + 1]];
                    dst[c + 2u * shCoeffs] = shNCode[centroids.rgba[idx + 2]];
                }
            }
        }
    }

    aiMaterial *mat = new aiMaterial();
    aiString matName("SOG_Default");
    mat->AddProperty(&matName, AI_MATKEY_NAME);

    pScene->mNumMaterials = 1;
    pScene->mMaterials = new aiMaterial *[1];
    pScene->mMaterials[0] = mat;

    pScene->mNumMeshes = 1;
    pScene->mMeshes = new aiMesh *[1];
    pScene->mMeshes[0] = mesh;

    pScene->mRootNode = new aiNode();
    pScene->mRootNode->mName.Set("SOG_Root");
    pScene->mRootNode->mNumMeshes = 1;
    pScene->mRootNode->mMeshes = new unsigned int[1];
    pScene->mRootNode->mMeshes[0] = 0;

    AttachGaussianSplat(pScene, 0, splat.release());

    if (pScene->mMetaData == nullptr) {
        pScene->mMetaData = new aiMetadata();
    }
    pScene->mMetaData->Add(AI_METADATA_GAUSSIAN_SPLAT, true);
    pScene->mMetaData->Add(AI_METADATA_GAUSSIAN_SH_REST_COUNT, int32_t(restCoeffs));

    if (pScene->mRootNode->mMetaData == nullptr) {
        pScene->mRootNode->mMetaData = new aiMetadata();
    }
    pScene->mRootNode->mMetaData->Add(AI_METADATA_GAUSSIAN_SPLAT, true);

    ASSIMP_LOG_INFO("SOG: loaded ", numPoints, " gaussians (restCoeffs=", restCoeffs, ").");
}

#endif // !! ASSIMP_BUILD_NO_SOG_IMPORTER
