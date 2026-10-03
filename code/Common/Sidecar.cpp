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
---------------------------------------------------------------------------
*/

/** @file Sidecar.cpp
 *  @brief ABI-safe getters / storage for generic mesh sidecar data.
 */

#include "ScenePrivate.h"

#include <assimp/scene.h>
#include <assimp/sidecar.h>

using namespace Assimp;

// ------------------------------------------------------------------------------------------------
ASSIMP_API int aiSceneHasMeshSidecar(const aiScene *pScene) {
    if (pScene == nullptr || pScene->mPrivate == nullptr) {
        return 0;
    }
    const ScenePrivateData *priv = ScenePriv(pScene);
    for (const aiMeshSidecar *s : priv->mMeshSidecars) {
        if (s != nullptr) {
            return 1;
        }
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------
ASSIMP_API const aiMeshSidecar *aiGetMeshSidecar(const aiScene *pScene, unsigned int meshIndex) {
    if (pScene == nullptr || pScene->mPrivate == nullptr) {
        return nullptr;
    }
    if (meshIndex >= pScene->mNumMeshes) {
        return nullptr;
    }
    const ScenePrivateData *priv = ScenePriv(pScene);
    if (meshIndex >= priv->mMeshSidecars.size()) {
        return nullptr;
    }
    return priv->mMeshSidecars[meshIndex];
}

// ------------------------------------------------------------------------------------------------
void Assimp::AttachMeshSidecar(aiScene *scene, unsigned int meshIndex, aiMeshSidecar *sidecar) {
    if (scene == nullptr || scene->mPrivate == nullptr || sidecar == nullptr) {
        delete sidecar;
        return;
    }
    ScenePrivateData *priv = ScenePriv(scene);
    if (meshIndex >= priv->mMeshSidecars.size()) {
        priv->mMeshSidecars.resize(static_cast<size_t>(meshIndex) + 1u, nullptr);
    }
    delete priv->mMeshSidecars[meshIndex];
    priv->mMeshSidecars[meshIndex] = sidecar;
}

// ------------------------------------------------------------------------------------------------
ASSIMP_API int aiSceneHasMaterialSidecar(const aiScene *pScene) {
    if (pScene == nullptr || pScene->mPrivate == nullptr) {
        return 0;
    }
    const ScenePrivateData *priv = ScenePriv(pScene);
    for (const aiMaterialSidecar *s : priv->mMaterialSidecars) {
        if (s != nullptr) {
            return 1;
        }
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------
ASSIMP_API const aiMaterialSidecar *aiGetMaterialSidecar(const aiScene *pScene, unsigned int materialIndex) {
    if (pScene == nullptr || pScene->mPrivate == nullptr) {
        return nullptr;
    }
    if (materialIndex >= pScene->mNumMaterials) {
        return nullptr;
    }
    const ScenePrivateData *priv = ScenePriv(pScene);
    if (materialIndex >= priv->mMaterialSidecars.size()) {
        return nullptr;
    }
    return priv->mMaterialSidecars[materialIndex];
}

// ------------------------------------------------------------------------------------------------
void Assimp::AttachMaterialSidecar(aiScene *scene, unsigned int materialIndex, aiMaterialSidecar *sidecar) {
    if (scene == nullptr || scene->mPrivate == nullptr || sidecar == nullptr) {
        delete sidecar;
        return;
    }
    ScenePrivateData *priv = ScenePriv(scene);
    if (materialIndex >= priv->mMaterialSidecars.size()) {
        priv->mMaterialSidecars.resize(static_cast<size_t>(materialIndex) + 1u, nullptr);
    }
    delete priv->mMaterialSidecars[materialIndex];
    priv->mMaterialSidecars[materialIndex] = sidecar;
}
