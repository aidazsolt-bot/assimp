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

/** @file SPZLoader.cpp
 *  @brief Niantic Labs .spz importer -> aiGaussianSplat (KHR-native).
 */

#ifndef ASSIMP_BUILD_NO_SPZ_IMPORTER

#include "SPZLoader.h"

#include <assimp/DefaultLogger.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/gaussian.h>
#include <assimp/importerdesc.h>
#include <assimp/scene.h>

#include <load-spz.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace Assimp;

namespace {

static constexpr aiImporterDesc desc = {
        "SPZ 3D Gaussian Splat Importer",
        "",
        "",
        "Niantic Labs .spz (gzip v1-3 / ZSTD v4 NGSP)",
        aiImporterFlags_SupportBinaryFlavour,
        0,
        0,
        0,
        0,
        "spz"
};

inline float sigmoid(float x) {
    return static_cast<float>(1.0 / (1.0 + std::exp(static_cast<double>(-x))));
}

/** Map SPZ GaussianCloud into KHR-native aiGaussianSplat + point-cloud aiMesh. */
void BuildSceneFromCloud(const spz::GaussianCloud &cloud, aiScene *pScene) {
    if (cloud.numPoints <= 0) {
        throw DeadlyImportError("SPZ: decoded cloud has no points.");
    }
    const unsigned numPoints = static_cast<unsigned>(cloud.numPoints);
    if (cloud.positions.size() < size_t(numPoints) * 3u ||
            cloud.scales.size() < size_t(numPoints) * 3u ||
            cloud.rotations.size() < size_t(numPoints) * 4u ||
            cloud.alphas.size() < size_t(numPoints) ||
            cloud.colors.size() < size_t(numPoints) * 3u) {
        throw DeadlyImportError("SPZ: attribute buffer size mismatch.");
    }

    unsigned restCoeffs = 0;
    if (cloud.shDegree >= 1) {
        // 3 * ((d+1)^2 - 1) = 9, 24, 45, 72 for degrees 1..4
        const int bands = (cloud.shDegree + 1) * (cloud.shDegree + 1) - 1;
        restCoeffs = static_cast<unsigned>(3 * bands);
    }
    if (restCoeffs > 0 && cloud.sh.size() < size_t(numPoints) * restCoeffs) {
        throw DeadlyImportError("SPZ: SH rest buffer smaller than shDegree implies.");
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
    }
    // Do not invent mKernel / mColorSpace - SPZ has no authored KHR meta strings.

    // Own mesh until transferred to the scene (exception mid-build must not leak).
    std::unique_ptr<aiMesh> meshOwned(new aiMesh());
    aiMesh *mesh = meshOwned.get();
    mesh->mName.Set("SPZ_Gaussians");
    mesh->mPrimitiveTypes = aiPrimitiveType_POINT;
    mesh->mNumVertices = numPoints;
    mesh->mVertices = new aiVector3D[numPoints];
    mesh->mNumFaces = numPoints;
    mesh->mFaces = new aiFace[numPoints];
    mesh->mMaterialIndex = 0;

    for (unsigned i = 0; i < numPoints; ++i) {
        const size_t p3 = size_t(i) * 3u;
        const size_t p4 = size_t(i) * 4u;

        mesh->mVertices[i].x = cloud.positions[p3 + 0];
        mesh->mVertices[i].y = cloud.positions[p3 + 1];
        mesh->mVertices[i].z = cloud.positions[p3 + 2];

        mesh->mFaces[i].mNumIndices = 1;
        mesh->mFaces[i].mIndices = new unsigned int[1];
        mesh->mFaces[i].mIndices[0] = i;

        splat->mDC[i].x = cloud.colors[p3 + 0];
        splat->mDC[i].y = cloud.colors[p3 + 1];
        splat->mDC[i].z = cloud.colors[p3 + 2];

        // SPZ scales are log-encoded; KHR wants linear >= 0.
        splat->mScale[i].x = static_cast<ai_real>(std::exp(static_cast<double>(cloud.scales[p3 + 0])));
        splat->mScale[i].y = static_cast<ai_real>(std::exp(static_cast<double>(cloud.scales[p3 + 1])));
        splat->mScale[i].z = static_cast<ai_real>(std::exp(static_cast<double>(cloud.scales[p3 + 2])));

        // SPZ rotations are xyzw (matches KHR / glTF).
        float qx = cloud.rotations[p4 + 0];
        float qy = cloud.rotations[p4 + 1];
        float qz = cloud.rotations[p4 + 2];
        float qw = cloud.rotations[p4 + 3];
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

        // SPZ alpha is logit; KHR wants linear opacity.
        float op = sigmoid(cloud.alphas[i]);
        if (op < 0.f) {
            op = 0.f;
        } else if (op > 1.f) {
            op = 1.f;
        }
        splat->mOpacity[i] = op;

        if (restCoeffs > 0 && splat->mRest) {
            const ai_real *src = cloud.sh.data() + size_t(i) * restCoeffs;
            ai_real *dst = splat->mRest + size_t(i) * restCoeffs;
            std::memcpy(dst, src, size_t(restCoeffs) * sizeof(ai_real));
        }
    }

    std::unique_ptr<aiMaterial> matOwned(new aiMaterial());
    aiString matName("SPZ_Default");
    matOwned->AddProperty(&matName, AI_MATKEY_NAME);

    pScene->mNumMaterials = 1;
    pScene->mMaterials = new aiMaterial *[1];
    pScene->mMaterials[0] = matOwned.release();

    pScene->mNumMeshes = 1;
    pScene->mMeshes = new aiMesh *[1];
    pScene->mMeshes[0] = meshOwned.release();

    pScene->mRootNode = new aiNode();
    pScene->mRootNode->mName.Set("SPZ_Root");
    pScene->mRootNode->mNumMeshes = 1;
    pScene->mRootNode->mMeshes = new unsigned int[1];
    pScene->mRootNode->mMeshes[0] = 0;

    const int32_t restCount = static_cast<int32_t>(restCoeffs);
    AttachGaussianSplat(pScene, 0, splat.release());

    if (pScene->mMetaData == nullptr) {
        pScene->mMetaData = new aiMetadata();
    }
    pScene->mMetaData->Add(AI_METADATA_GAUSSIAN_SPLAT, true);
    pScene->mMetaData->Add(AI_METADATA_GAUSSIAN_SH_REST_COUNT, restCount);

    if (pScene->mRootNode->mMetaData == nullptr) {
        pScene->mRootNode->mMetaData = new aiMetadata();
    }
    pScene->mRootNode->mMetaData->Add(AI_METADATA_GAUSSIAN_SPLAT, true);

    ASSIMP_LOG_INFO("SPZ: loaded ", numPoints, " gaussians (shDegree=", cloud.shDegree, ").");
}

} // namespace

// ------------------------------------------------------------------------------------------------
bool SPZImporter::CanRead(const std::string &pFile, IOSystem *pIOHandler, bool checkSig) const {
    const std::string extension = GetExtension(pFile);
    if (extension == "spz") {
        return true;
    }
    if (!checkSig || !pIOHandler) {
        return false;
    }
    // NGSP magic (v4) or gzip magic (legacy v1-3).
    static const char *tokens[] = { "NGSP" };
    if (CheckMagicToken(pIOHandler, pFile, tokens, 1)) {
        return true;
    }
    static const char gzipTok[] = { '\x1f', '\x8b' };
    return CheckMagicToken(pIOHandler, pFile, gzipTok, 1, 0, 2);
}

// ------------------------------------------------------------------------------------------------
const aiImporterDesc *SPZImporter::GetInfo() const {
    return &desc;
}

// ------------------------------------------------------------------------------------------------
void SPZImporter::InternReadFile(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
    std::unique_ptr<IOStream> file(pIOHandler->Open(pFile, "rb"));
    if (!file) {
        throw DeadlyImportError("Failed to open SPZ file ", pFile, ".");
    }

    const size_t fileSize = file->FileSize();
    if (fileSize == 0 || fileSize == SIZE_MAX) {
        throw DeadlyImportError("SPZ file is empty or unreadable: ", pFile);
    }

    std::vector<uint8_t> buffer(fileSize);
    if (file->Read(buffer.data(), 1, fileSize) != fileSize) {
        throw DeadlyImportError("Failed to read SPZ file ", pFile, ".");
    }

    spz::UnpackOptions opt;
    // Keep authored coordinates (no invent); host may convert RUB->LUF if needed.
    opt.to = spz::CoordinateSystem::UNSPECIFIED;

    const spz::GaussianCloud cloud = spz::loadSpz(buffer.data(), buffer.size(), opt);
    BuildSceneFromCloud(cloud, pScene);
}

#endif // !! ASSIMP_BUILD_NO_SPZ_IMPORTER
