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

/** @file gaussian.h
 *  @brief ABI-safe side channel for 3D Gaussian Splatting attributes.
 *
 *  Does **not** extend aiMesh / aiScene layout. Splat arrays live in
 *  scene-private storage and are reached via getters. Positions stay in
 *  aiMesh::mVertices (point cloud; 1-index faces per vertex).
 *
 *  Canonical value space matches ratified KHR_gaussian_splatting (ellipse
 *  kernel): linear opacity in [0,1], non-negative linear scale, unit
 *  quaternion in glTF order (x,y,z,w), SH coefficients as in the KHR
 *  attribute layout. PLY graphdeco logits / wxyz quats are converted on
 *  import. Assimp does not invent COLOR_0, SH bias, or missing meta.
 *
 *  Does **not** bake splat attrs into aiMaterial. See doc/PLY.md and
 *  doc/GaussianSplatting.md.
 */
#pragma once
#ifndef AI_GAUSSIAN_H_INC
#define AI_GAUSSIAN_H_INC

#ifdef __GNUC__
#   pragma GCC system_header
#endif

#include <assimp/defs.h>
#include <assimp/types.h>
#include <assimp/vector3.h>
#include <assimp/color4.h>

#ifdef __cplusplus
extern "C" {
#endif

struct aiScene;

/** Scene / node metadata: present and true when at least one mesh has splat data. */
#define AI_METADATA_GAUSSIAN_SPLAT "IsGaussianSplat"

/** Scene metadata: number of higher-order SH scalars per point (0 or 9/24/45...). */
#define AI_METADATA_GAUSSIAN_SH_REST_COUNT "GaussianSHRestCount"

/**
 * Per-mesh 3DGS attributes in KHR_gaussian_splatting-compatible value space.
 *
 * Positions: use the corresponding aiMesh::mVertices (mNumPoints == mNumVertices).
 */
struct aiGaussianSplat {
    /** Number of Gaussians (== owning mesh vertex count). */
    unsigned int mNumPoints;

    /** Number of higher-order SH scalars per point (0 if only degree 0).
     *  Degree 1 => 9, degree 2 => 24, degree 3 => 45 (3 * ((d+1)^2 - 1)). */
    unsigned int mNumRestCoeffs;

    /** SH degree-0 RGB coefficients (KHR SH_DEGREE_0_COEF_0 / PLY f_dc_*). Size: mNumPoints. */
    C_STRUCT aiVector3D *mDC;

    /** Higher-order SH scalars in KHR coefficient order (degree 1..N, each RGB).
     *  Size: mNumPoints * mNumRestCoeffs. nullptr if mNumRestCoeffs == 0. */
    ai_real *mRest;

    /** Linear scale (>= 0). Size: mNumPoints. */
    C_STRUCT aiVector3D *mScale;

    /** Unit quaternion as (r,g,b,a) == (x,y,z,w) glTF order. Size: mNumPoints. */
    C_STRUCT aiColor4D *mRotation;

    /** Linear opacity in [0, 1]. Size: mNumPoints. */
    ai_real *mOpacity;

    /** Authored KHR kernel id (e.g. "ellipse"), or empty if not present in source. */
    C_STRUCT aiString mKernel;

    /** Authored KHR colorSpace, or empty if not present in source. */
    C_STRUCT aiString mColorSpace;

    /** Authored KHR projection, or empty (KHR default is "perspective"). */
    C_STRUCT aiString mProjection;

    /** Authored KHR sortingMethod, or empty (KHR default is "cameraDistance"). */
    C_STRUCT aiString mSortingMethod;

#ifdef __cplusplus
    aiGaussianSplat() AI_NO_EXCEPT
            : mNumPoints(0),
              mNumRestCoeffs(0),
              mDC(nullptr),
              mRest(nullptr),
              mScale(nullptr),
              mRotation(nullptr),
              mOpacity(nullptr),
              mKernel(),
              mColorSpace(),
              mProjection(),
              mSortingMethod() {}

    ~aiGaussianSplat() {
        delete[] mDC;
        delete[] mRest;
        delete[] mScale;
        delete[] mRotation;
        delete[] mOpacity;
    }

private:
    aiGaussianSplat(const aiGaussianSplat &) = delete;
    aiGaussianSplat &operator=(const aiGaussianSplat &) = delete;
#endif
};

/**
 * Morph-target patch for Gaussian attributes (parallel to #aiAnimMesh).
 *
 * Indexing matches #aiMesh::mAnimMeshes / glTF `targets[i]`. POSITION morph
 * stays on #aiAnimMesh; this channel carries splat-only attrs (SCALE, …).
 *
 * Arrays are **glTF morph deltas** (relative), not absolute replacements.
 * nullptr array = that attribute is absent on this target.
 * Weights over time: #aiAnimation::mMorphMeshChannels (#aiMeshMorphAnim).
 */
struct aiGaussianAnimSplat {
    /** Must equal owning #aiGaussianSplat::mNumPoints / mesh vertex count. */
    unsigned int mNumPoints;

    /** Optional name (glTF targetNames / extras). */
    C_STRUCT aiString mName;

    /** Default weight when no morph animation samples (glTF mesh.weights[i]). */
    ai_real mWeight;

    /** Linear scale deltas. Size: mNumPoints. */
    C_STRUCT aiVector3D *mScale;

    /** Quaternion deltas as (r,g,b,a)==(x,y,z,w). Size: mNumPoints. */
    C_STRUCT aiColor4D *mRotation;

    /** Opacity deltas. Size: mNumPoints. */
    ai_real *mOpacity;

    /** SH degree-0 RGB deltas. Size: mNumPoints. */
    C_STRUCT aiVector3D *mDC;

    /** Higher-order SH deltas. Size: mNumPoints * mNumRestCoeffs. */
    ai_real *mRest;

    /** Rest coeff count per point (0 if mRest is nullptr). */
    unsigned int mNumRestCoeffs;

#ifdef __cplusplus
    aiGaussianAnimSplat() AI_NO_EXCEPT
            : mNumPoints(0),
              mName(),
              mWeight(0.f),
              mScale(nullptr),
              mRotation(nullptr),
              mOpacity(nullptr),
              mDC(nullptr),
              mRest(nullptr),
              mNumRestCoeffs(0) {}

    ~aiGaussianAnimSplat() {
        delete[] mScale;
        delete[] mRotation;
        delete[] mOpacity;
        delete[] mDC;
        delete[] mRest;
    }

private:
    aiGaussianAnimSplat(const aiGaussianAnimSplat &) = delete;
    aiGaussianAnimSplat &operator=(const aiGaussianAnimSplat &) = delete;
#endif
};

/**
 * @brief Non-zero if the scene carries any Gaussian splat side data.
 */
ASSIMP_API int aiSceneHasGaussianSplat(const C_STRUCT aiScene *pScene);

/**
 * @brief Get Gaussian attributes for a mesh, or nullptr if none.
 *
 * Lifetime is tied to the aiScene (valid until aiReleaseImport / delete).
 * Not preserved by aiCopyScene / SceneCombiner copies.
 */
ASSIMP_API const C_STRUCT aiGaussianSplat *aiGetGaussianSplat(
        const C_STRUCT aiScene *pScene,
        unsigned int meshIndex);

/**
 * @brief Number of Gaussian morph targets for a mesh (0 if none).
 *
 * Aligned with #aiMesh::mNumAnimMeshes when both are present.
 */
ASSIMP_API unsigned int aiGetGaussianAnimSplatCount(
        const C_STRUCT aiScene *pScene,
        unsigned int meshIndex);

/**
 * @brief Get one Gaussian morph target, or nullptr if absent / OOB.
 */
ASSIMP_API const C_STRUCT aiGaussianAnimSplat *aiGetGaussianAnimSplat(
        const C_STRUCT aiScene *pScene,
        unsigned int meshIndex,
        unsigned int targetIndex);

#ifdef __cplusplus
} // extern "C"

namespace Assimp {

/** Attach ownership of @p splat for @p meshIndex (importer use). Takes ownership. */
void AttachGaussianSplat(aiScene *scene, unsigned int meshIndex, aiGaussianSplat *splat);

/**
 * Attach ownership of @p count morph targets for @p meshIndex.
 * Takes ownership of each non-null pointer in @p splats[0..count).
 */
void AttachGaussianAnimSplats(aiScene *scene, unsigned int meshIndex,
        aiGaussianAnimSplat **splats, unsigned int count);

} // namespace Assimp
#endif

#endif // AI_GAUSSIAN_H_INC
