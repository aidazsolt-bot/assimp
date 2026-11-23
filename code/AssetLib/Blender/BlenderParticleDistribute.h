/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
----------------------------------------------------------------------
*/

/** @file BlenderParticleDistribute.h
 *  Assimp FACE-particle distribution for BLEND PointInstancer arrays when
 *  DNA ParticleSystem.particles is empty (typical hair/collection .blend).
 *  Deterministic from ParticleSettings DNA + emitter Mesh - not a Blender
 *  depsgraph / GPL port; Host must not treat coords as bit-identical to Blender.
 */
#ifndef INCLUDED_AI_BLENDER_PARTICLE_DISTRIBUTE_H
#define INCLUDED_AI_BLENDER_PARTICLE_DISTRIBUTE_H

#include "BlenderScene.h"
#include <vector>

namespace Assimp {
namespace Blender {

struct ParticleFaceDistributeParams {
    int totpart{ 0 };
    int seed{ 0 };
    short from{ 0 }; // PART_FROM_FACE == 1
    short distr{ 0 }; // PART_DISTR_JIT == 0, RAND == 1
    int flag{ 0 }; // PART_TRAND (1<<7), PART_EDISTR (1<<8)
    float jitfac{ 1.f };
    int userjit{ 0 };
    float size{ 1.f };
    float randsize{ 0.f };
    /** RNA hair_length (= DNA normfac*4). 0 => scale uses size only. */
    float hairLength{ 0.f };
    std::vector<int> prototypeCounts;
};

/** Returns true and fills arrays when Mesh has faces and totpart > 0 and from==FACE. */
bool BlendDistributeParticlesOnMeshFaces(
        const Mesh &mesh,
        const ParticleFaceDistributeParams &params,
        std::vector<float> &positions,
        std::vector<float> &orients,
        std::vector<float> &scales,
        std::vector<int> &protoIndices);

} // namespace Blender
} // namespace Assimp

#endif
