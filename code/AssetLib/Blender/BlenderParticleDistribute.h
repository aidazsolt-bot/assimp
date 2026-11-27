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
    /** Object DNA scale[3] for area weights in object space (Blender PART_EDISTR). */
    float objectScale[3]{ 1.f, 1.f, 1.f };
    std::vector<int> prototypeCounts;
};

struct ParticleFaceDistributeResult {
    std::vector<float> positions; // xyz * N (mesh/object-local)
    std::vector<float> orients; // wxyz * N
    std::vector<float> scales; // uniform * N
    std::vector<int> protoIndices;
    /** Object-space triangle area of the face that received each particle (Presence). */
    std::vector<float> faceArea;
    /** Normalized emit weight faceArea/sumAreas for that face (Presence). */
    std::vector<float> emitWeight;
};

/** Returns true and fills result when Mesh has faces and totpart > 0 and from==FACE.
 *  FACE eval always area-weights in object space (objectScale * mesh verts). */
bool BlendDistributeParticlesOnMeshFaces(
        const Mesh &mesh,
        const ParticleFaceDistributeParams &params,
        ParticleFaceDistributeResult &out);

} // namespace Blender
} // namespace Assimp

#endif
