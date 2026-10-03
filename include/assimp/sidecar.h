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

/** @file sidecar.h
 *  @brief ABI-safe generic mesh side channel for importer-only data.
 *
 *  Does **not** extend aiMesh / aiScene layout. Named attribute buffers and
 *  JSON extras/extensions live in scene-private storage and are reached via
 *  getters. Use this for data Assimp's public scene graph does not map
 *  (e.g. glTF custom / underscore vertex attributes, mesh extras).
 *
 *  See doc/Sidecar.md.
 */
#pragma once
#ifndef AI_SIDECAR_H_INC
#define AI_SIDECAR_H_INC

#ifdef __GNUC__
#   pragma GCC system_header
#endif

#include <assimp/defs.h>
#include <assimp/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct aiScene;

/** Scene metadata: present and true when at least one mesh has sidecar data. */
#define AI_METADATA_MESH_SIDECAR "HasMeshSidecar"

/** Component type for #aiSidecarBuffer::mComponentType (glTF-aligned). */
enum aiSidecarComponentType {
    aiSidecarComponentType_Byte = 5120,
    aiSidecarComponentType_UnsignedByte = 5121,
    aiSidecarComponentType_Short = 5122,
    aiSidecarComponentType_UnsignedShort = 5123,
    aiSidecarComponentType_UnsignedInt = 5125,
    aiSidecarComponentType_Float = 5126
};

/**
 * One named per-vertex (or per-element) buffer on a mesh sidecar.
 *
 * Layout is tightly packed: mByteLength == mNumElements * mNumComponents * sizeof(component).
 * After import with an index buffer, mNumElements matches the remapped aiMesh vertex count.
 */
struct aiSidecarBuffer {
    /** Attribute semantic / name (e.g. "_SCALE"). */
    C_STRUCT aiString mName;

    /** Number of elements (usually == owning mesh vertex count). */
    unsigned int mNumElements;

    /** Components per element (1=SCALAR, 2=VEC2, 3=VEC3, 4=VEC4, ...). */
    unsigned int mNumComponents;

    /** Element component type (#aiSidecarComponentType). */
    unsigned int mComponentType;

    /** Tightly packed component bytes. Size: mByteLength. */
    uint8_t *mData;

    /** Byte length of mData. */
    size_t mByteLength;

#ifdef __cplusplus
    aiSidecarBuffer() AI_NO_EXCEPT
            : mName(),
              mNumElements(0),
              mNumComponents(0),
              mComponentType(0),
              mData(nullptr),
              mByteLength(0) {}

    ~aiSidecarBuffer() {
        delete[] mData;
        mData = nullptr;
        mByteLength = 0;
    }

private:
    aiSidecarBuffer(const aiSidecarBuffer &) = delete;
    aiSidecarBuffer &operator=(const aiSidecarBuffer &) = delete;
#endif
};

/**
 * Per-mesh sidecar bag: custom attribute buffers + optional JSON.
 *
 * Indexed by mesh index (same as aiScene::mMeshes).
 */
struct aiMeshSidecar {
    /** Number of named buffers. */
    unsigned int mNumBuffers;

    /** Array of mNumBuffers buffers (owned). */
    C_STRUCT aiSidecarBuffer *mBuffers;

    /** UTF-8 JSON object for mesh/primitive extras, or nullptr. */
    char *mExtrasJson;

    /** Byte length of mExtrasJson excluding trailing NUL (0 if nullptr). */
    size_t mExtrasJsonLength;

    /** UTF-8 JSON object for mesh/primitive extensions, or nullptr. */
    char *mExtensionsJson;

    /** Byte length of mExtensionsJson excluding trailing NUL (0 if nullptr). */
    size_t mExtensionsJsonLength;

#ifdef __cplusplus
    aiMeshSidecar() AI_NO_EXCEPT
            : mNumBuffers(0),
              mBuffers(nullptr),
              mExtrasJson(nullptr),
              mExtrasJsonLength(0),
              mExtensionsJson(nullptr),
              mExtensionsJsonLength(0) {}

    ~aiMeshSidecar() {
        delete[] mBuffers;
        mBuffers = nullptr;
        mNumBuffers = 0;
        delete[] mExtrasJson;
        mExtrasJson = nullptr;
        mExtrasJsonLength = 0;
        delete[] mExtensionsJson;
        mExtensionsJson = nullptr;
        mExtensionsJsonLength = 0;
    }

private:
    aiMeshSidecar(const aiMeshSidecar &) = delete;
    aiMeshSidecar &operator=(const aiMeshSidecar &) = delete;
#endif
};

/**
 * @brief Non-zero if the scene carries any mesh sidecar data.
 */
ASSIMP_API int aiSceneHasMeshSidecar(const C_STRUCT aiScene *pScene);

/**
 * @brief Get sidecar for a mesh, or nullptr if none.
 *
 * Lifetime is tied to the aiScene (valid until aiReleaseImport / delete).
 * Not preserved by aiCopyScene / SceneCombiner copies.
 */
ASSIMP_API const C_STRUCT aiMeshSidecar *aiGetMeshSidecar(
        const C_STRUCT aiScene *pScene,
        unsigned int meshIndex);

#ifdef __cplusplus
} // extern "C"

namespace Assimp {

/** Attach ownership of @p sidecar for @p meshIndex (importer use). Takes ownership. */
void AttachMeshSidecar(aiScene *scene, unsigned int meshIndex, aiMeshSidecar *sidecar);

} // namespace Assimp
#endif

#endif // AI_SIDECAR_H_INC
