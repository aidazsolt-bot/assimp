/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
All rights reserved.
----------------------------------------------------------------------
*/

/** @file USDSidecarEmit.h
 *  @brief Emit S1-S8 USD category sidecars into aiScene (no new public API).
 */
#pragma once

#ifndef AI_USD_SIDECAR_EMIT_H_INCLUDED
#define AI_USD_SIDECAR_EMIT_H_INCLUDED

#ifndef ASSIMP_BUILD_NO_USD_IMPORTER

#if defined(ASSIMP_BUILD_USD_BETA)
#include "lightusd.hh"
#else
#include "tinyusdz.hh"
#endif
#include "tydra/render-data.hh"

struct aiScene;

#if !defined(ASSIMP_BUILD_USD_BETA)
namespace lightusd = tinyusdz;
#endif

namespace Assimp {

/// Emit Mesh/Material/Anim/Physics sidecars + extend SceneSidecarJson (S1-S8).
void EmitUsdCategorySidecars(
        aiScene *pScene,
        const std::string &pFile,
        const lightusd::Stage &stage,
        const lightusd::tydra::RenderScene &render_scene,
        const lightusd::tydra::SceneMetadata &sceneMeta);

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_IMPORTER
#endif // AI_USD_SIDECAR_EMIT_H_INCLUDED
