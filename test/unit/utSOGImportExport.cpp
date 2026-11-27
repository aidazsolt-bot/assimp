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

#include <cstdlib>
#include <string>
#include <vector>

using namespace Assimp;

#ifndef ASSIMP_BUILD_NO_SOG_IMPORTER

class utSOGImportExport : public ::testing::Test {};

TEST_F(utSOGImportExport, importEnvSog) {
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(
            ASSIMP_TEST_MODELS_DIR "/SOG/env.sog", aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, scene) << importer.GetErrorString();
    ASSERT_EQ(1u, scene->mNumMeshes);
    ASSERT_NE(nullptr, scene->mMeshes[0]);
    EXPECT_EQ(aiPrimitiveType_POINT, scene->mMeshes[0]->mPrimitiveTypes);

    EXPECT_TRUE(aiSceneHasGaussianSplat(scene));
    const aiGaussianSplat *gs = aiGetGaussianSplat(scene, 0);
    ASSERT_NE(nullptr, gs);
    // XGRIDS LCC2 env.sog sample: 13921 gaussians, SH DC only.
    EXPECT_EQ(13921u, gs->mNumPoints);
    EXPECT_EQ(13921u, scene->mMeshes[0]->mNumVertices);
    EXPECT_EQ(0u, gs->mNumRestCoeffs);
    ASSERT_NE(nullptr, gs->mDC);
    ASSERT_NE(nullptr, gs->mScale);
    ASSERT_NE(nullptr, gs->mRotation);
    ASSERT_NE(nullptr, gs->mOpacity);

    // Sanity: opacity in [0,1], scale >= 0, quat roughly unit.
    EXPECT_GE(gs->mOpacity[0], 0.f);
    EXPECT_LE(gs->mOpacity[0], 1.f);
    EXPECT_GE(gs->mScale[0].x, 0.f);
    const float qlen2 = gs->mRotation[0].r * gs->mRotation[0].r +
                        gs->mRotation[0].g * gs->mRotation[0].g +
                        gs->mRotation[0].b * gs->mRotation[0].b +
                        gs->mRotation[0].a * gs->mRotation[0].a;
    EXPECT_NEAR(1.f, qlen2, 5e-2f);

    ASSERT_NE(nullptr, scene->mMetaData);
    bool isGs = false;
    EXPECT_TRUE(scene->mMetaData->Get(AI_METADATA_GAUSSIAN_SPLAT, isGs));
    EXPECT_TRUE(isGs);
}

TEST_F(utSOGImportExport, importHomeSogOptional) {
    Assimp::Importer importer;
    std::vector<std::string> candidates;
    if (const char *home = std::getenv("HOME")) {
        candidates.push_back(std::string(home) + "/3D-Objekte/xgrids-3dgs/sog/0_0.sog");
        candidates.push_back(std::string(home) + "/3D-Objekte/xgrids-3dgs/env.sog");
    }
    const aiScene *scene = nullptr;
    for (const std::string &path : candidates) {
        scene = importer.ReadFile(path, 0);
        if (scene != nullptr) {
            break;
        }
    }
    if (scene == nullptr) {
        GTEST_SKIP() << "optional home SOG samples not present";
    }
    EXPECT_TRUE(aiSceneHasGaussianSplat(scene));
    const aiGaussianSplat *gs = aiGetGaussianSplat(scene, 0);
    ASSERT_NE(nullptr, gs);
    EXPECT_GT(gs->mNumPoints, 0u);
}

#endif // !! ASSIMP_BUILD_NO_SOG_IMPORTER
