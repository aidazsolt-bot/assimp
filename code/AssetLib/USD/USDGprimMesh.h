/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
All rights reserved.
----------------------------------------------------------------------
*/

/** @file USDGprimMesh.h
 *  @brief Tessellate UsdGeomGprim Cube/Sphere/... into aiMesh (TinyUSDZ Stage).
 */
#pragma once

#ifndef AI_USD_GPRIM_MESH_H_INCLUDED
#define AI_USD_GPRIM_MESH_H_INCLUDED

#ifndef ASSIMP_BUILD_NO_USD_IMPORTER

#if defined(ASSIMP_BUILD_USD_BETA)
#include "lightusd.hh"
#else
#include "tinyusdz.hh"
#endif

#include <string>
#include <unordered_map>
#include <vector>

struct aiMesh;
struct aiNode;
struct aiScene;

#if !defined(ASSIMP_BUILD_USD_BETA)
namespace lightusd = tinyusdz;
#endif

namespace Assimp {

/// Tessellate authored GeomCube/Sphere/Capsule/Cone/Cylinder prims into meshes
/// and attach them to matching aiNodes (by prim leaf name). Returns number of
/// meshes appended. Also records payload/reference skips in `outSkipped`.
unsigned int AppendTessellatedUsdGprimMeshes(
        aiScene *scene,
        const lightusd::Stage &stage,
        std::vector<std::string> *outSkipped);

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_IMPORTER
#endif // AI_USD_GPRIM_MESH_H_INCLUDED
