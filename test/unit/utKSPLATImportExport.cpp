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

#include "UnitTestPCH.h"

#include <assimp/Importer.hpp>
#include <assimp/gaussian.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

using namespace Assimp;

#ifndef ASSIMP_BUILD_NO_KSPLAT_IMPORTER

class utKSPLATImportExport : public ::testing::Test {};

// File stores mkkellogg wxyz identity (1,0,0,0); Assimp exposes KHR glTF xyzw.
TEST_F(utKSPLATImportExport, importTinyDcKsplat) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/KSPLAT/tiny_dc.ksplat", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    ASSERT_NE(nullptr, scene->mMeshes[0]);
    EXPECT_EQ(1u, scene->mMeshes[0]->mNumVertices);
    EXPECT_EQ(aiPrimitiveType_POINT, scene->mMeshes[0]->mPrimitiveTypes);

    EXPECT_TRUE(aiSceneHasGaussianSplat(scene));
    const aiGaussianSplat *gs = aiGetGaussianSplat(scene, 0);
    ASSERT_NE(nullptr, gs);
    EXPECT_EQ(1u, gs->mNumPoints);
    EXPECT_EQ(0u, gs->mNumRestCoeffs);
    ASSERT_NE(nullptr, gs->mDC);
    ASSERT_NE(nullptr, gs->mScale);
    ASSERT_NE(nullptr, gs->mRotation);
    ASSERT_NE(nullptr, gs->mOpacity);

    EXPECT_NEAR(1.0f, scene->mMeshes[0]->mVertices[0].x, 1e-5f);
    EXPECT_NEAR(2.0f, scene->mMeshes[0]->mVertices[0].y, 1e-5f);
    EXPECT_NEAR(3.0f, scene->mMeshes[0]->mVertices[0].z, 1e-5f);

    EXPECT_NEAR(0.1f, gs->mScale[0].x, 1e-5f);
    EXPECT_NEAR(0.2f, gs->mScale[0].y, 1e-5f);
    EXPECT_NEAR(0.3f, gs->mScale[0].z, 1e-5f);
    EXPECT_NEAR(1.0f, gs->mOpacity[0], 1e-5f);

    constexpr float kShC0 = 0.28209479177387814f;
    EXPECT_NEAR((1.0f - 0.5f) / kShC0, gs->mDC[0].x, 1e-4f);
    EXPECT_NEAR((128.0f / 255.0f - 0.5f) / kShC0, gs->mDC[0].y, 1e-4f);
    EXPECT_NEAR((0.0f - 0.5f) / kShC0, gs->mDC[0].z, 1e-4f);

    EXPECT_NEAR(0.0f, gs->mRotation[0].r, 1e-5f);
    EXPECT_NEAR(0.0f, gs->mRotation[0].g, 1e-5f);
    EXPECT_NEAR(0.0f, gs->mRotation[0].b, 1e-5f);
    EXPECT_NEAR(1.0f, gs->mRotation[0].a, 1e-5f);

    ASSERT_NE(nullptr, scene->mMetaData);
    bool isGs = false;
    EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_GAUSSIAN_SPLAT, isGs));
    EXPECT_TRUE(isGs);
}

#endif // ASSIMP_BUILD_NO_KSPLAT_IMPORTER
