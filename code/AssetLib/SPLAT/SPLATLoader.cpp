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

/** @file SPLATLoader.cpp
 *  @brief antimatter15 .splat importer -> aiGaussianSplat (KHR-native).
 */

#ifndef ASSIMP_BUILD_NO_SPLAT_IMPORTER

#include "SPLATLoader.h"

#include <assimp/ByteSwapper.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/gaussian.h>
#include <assimp/importerdesc.h>
#include <assimp/scene.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace Assimp;

namespace {

/** antimatter15 / convert.py record size (headerless). */
static constexpr size_t kSplatRecordBytes = 32u;

/** Y_0,0 used when convert.py bakes display RGB = 0.5 + SH_C0 * f_dc. */
static constexpr float kShC0 = 0.28209479177387814f;

static constexpr aiImporterDesc desc = {
        "SPLAT 3D Gaussian Splat Importer",
        "",
        "",
        "antimatter15 .splat (32-byte LE records, no header)",
        aiImporterFlags_SupportBinaryFlavour,
        0,
        0,
        0,
        0,
        "splat"
};

inline float ReadFloat32LE(const uint8_t *p) {
    float v = 0.f;
    std::memcpy(&v, p, sizeof(float));
    AI_SWAP4(v);
    return v;
}

inline float DecodeQuatComponent(uint8_t b) {
    return (static_cast<float>(b) - 128.f) / 128.f;
}

} // namespace

// ------------------------------------------------------------------------------------------------
bool SPLATImporter::CanRead(const std::string &pFile, IOSystem *pIOHandler, bool checkSig) const {
    const std::string extension = GetExtension(pFile);
    if (extension == "splat") {
        return true;
    }
    if (!checkSig || !pIOHandler) {
        return false;
    }
    // Headerless: only size sanity (multiple of 32, non-empty).
    std::unique_ptr<IOStream> file(pIOHandler->Open(pFile, "rb"));
    if (!file) {
        return false;
    }
    const size_t fileSize = file->FileSize();
    return fileSize >= kSplatRecordBytes && (fileSize % kSplatRecordBytes) == 0;
}

// ------------------------------------------------------------------------------------------------
const aiImporterDesc *SPLATImporter::GetInfo() const {
    return &desc;
}

// ------------------------------------------------------------------------------------------------
void SPLATImporter::InternReadFile(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
    std::unique_ptr<IOStream> file(pIOHandler->Open(pFile, "rb"));
    if (!file) {
        throw DeadlyImportError("Failed to open SPLAT file ", pFile, ".");
    }

    const size_t fileSize = file->FileSize();
    if (fileSize == 0 || fileSize == SIZE_MAX) {
        throw DeadlyImportError("SPLAT file is empty or unreadable: ", pFile);
    }
    if ((fileSize % kSplatRecordBytes) != 0) {
        throw DeadlyImportError("SPLAT file size is not a multiple of 32: ", pFile);
    }

    const unsigned numPoints = static_cast<unsigned>(fileSize / kSplatRecordBytes);
    if (numPoints == 0) {
        throw DeadlyImportError("SPLAT file has no gaussians: ", pFile);
    }

    std::vector<uint8_t> buffer(fileSize);
    if (file->Read(buffer.data(), 1, fileSize) != fileSize) {
        throw DeadlyImportError("Failed to read SPLAT file ", pFile, ".");
    }

    std::unique_ptr<aiGaussianSplat> splat(new aiGaussianSplat());
    splat->mNumPoints = numPoints;
    splat->mNumRestCoeffs = 0; // .splat has no higher-order SH
    splat->mDC = new aiVector3D[numPoints];
    splat->mScale = new aiVector3D[numPoints];
    splat->mRotation = new aiColor4D[numPoints];
    splat->mOpacity = new ai_real[numPoints];
    // Do not invent mKernel / mColorSpace.

    aiMesh *mesh = new aiMesh();
    mesh->mName.Set("SPLAT_Gaussians");
    mesh->mPrimitiveTypes = aiPrimitiveType_POINT;
    mesh->mNumVertices = numPoints;
    mesh->mVertices = new aiVector3D[numPoints];
    mesh->mNumFaces = numPoints;
    mesh->mFaces = new aiFace[numPoints];
    mesh->mMaterialIndex = 0;

    for (unsigned i = 0; i < numPoints; ++i) {
        const uint8_t *rec = buffer.data() + size_t(i) * kSplatRecordBytes;

        const float px = ReadFloat32LE(rec + 0);
        const float py = ReadFloat32LE(rec + 4);
        const float pz = ReadFloat32LE(rec + 8);
        mesh->mVertices[i].x = px;
        mesh->mVertices[i].y = py;
        mesh->mVertices[i].z = pz;

        mesh->mFaces[i].mNumIndices = 1;
        mesh->mFaces[i].mIndices = new unsigned int[1];
        mesh->mFaces[i].mIndices[0] = i;

        // Linear scale (already exp'd by convert.py).
        float sx = ReadFloat32LE(rec + 12);
        float sy = ReadFloat32LE(rec + 16);
        float sz = ReadFloat32LE(rec + 20);
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

        // Baked display RGB + linear opacity (uint8 / 255).
        // Recover SH DC: rgb = 0.5 + SH_C0 * f_dc  =>  f_dc = (rgb - 0.5) / SH_C0
        const float r = static_cast<float>(rec[24]) / 255.f;
        const float g = static_cast<float>(rec[25]) / 255.f;
        const float b = static_cast<float>(rec[26]) / 255.f;
        float op = static_cast<float>(rec[27]) / 255.f;
        if (op < 0.f) {
            op = 0.f;
        } else if (op > 1.f) {
            op = 1.f;
        }
        splat->mOpacity[i] = op;
        splat->mDC[i].x = (r - 0.5f) / kShC0;
        splat->mDC[i].y = (g - 0.5f) / kShC0;
        splat->mDC[i].z = (b - 0.5f) / kShC0;

        // File order matches PLY rot_0..3 (graphdeco wxyz) from convert.py.
        // Store KHR/glTF xyzw as (r,g,b,a).
        float qw = DecodeQuatComponent(rec[28]);
        float qx = DecodeQuatComponent(rec[29]);
        float qy = DecodeQuatComponent(rec[30]);
        float qz = DecodeQuatComponent(rec[31]);
        const double qlen = std::sqrt(double(qx) * qx + double(qy) * qy + double(qz) * qz + double(qw) * qw);
        if (qlen > 0.0) {
            const float inv = static_cast<float>(1.0 / qlen);
            qx *= inv;
            qy *= inv;
            qz *= inv;
            qw *= inv;
        } else {
            qx = qy = qz = 0.f;
            qw = 1.f;
        }
        splat->mRotation[i].r = qx;
        splat->mRotation[i].g = qy;
        splat->mRotation[i].b = qz;
        splat->mRotation[i].a = qw;
    }

    aiMaterial *mat = new aiMaterial();
    aiString matName("SPLAT_Default");
    mat->AddProperty(&matName, AI_MATKEY_NAME);

    pScene->mNumMaterials = 1;
    pScene->mMaterials = new aiMaterial *[1];
    pScene->mMaterials[0] = mat;

    pScene->mNumMeshes = 1;
    pScene->mMeshes = new aiMesh *[1];
    pScene->mMeshes[0] = mesh;

    pScene->mRootNode = new aiNode();
    pScene->mRootNode->mName.Set("SPLAT_Root");
    pScene->mRootNode->mNumMeshes = 1;
    pScene->mRootNode->mMeshes = new unsigned int[1];
    pScene->mRootNode->mMeshes[0] = 0;

    AttachGaussianSplat(pScene, 0, splat.release());

    if (pScene->mMetaData == nullptr) {
        pScene->mMetaData = new aiMetadata();
    }
    pScene->mMetaData->Add(AI_METADATA_GAUSSIAN_SPLAT, true);
    pScene->mMetaData->Add(AI_METADATA_GAUSSIAN_SH_REST_COUNT, int32_t(0));

    if (pScene->mRootNode->mMetaData == nullptr) {
        pScene->mRootNode->mMetaData = new aiMetadata();
    }
    pScene->mRootNode->mMetaData->Add(AI_METADATA_GAUSSIAN_SPLAT, true);

    ASSIMP_LOG_INFO("SPLAT: loaded ", numPoints, " gaussians.");
}

#endif // !! ASSIMP_BUILD_NO_SPLAT_IMPORTER
