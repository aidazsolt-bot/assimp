/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
All rights reserved.
----------------------------------------------------------------------
*/

/** @file USDUsdaSanitize.h
 *  @brief Soften USDA ASCII for TinyUSDZ (unknown stage/prim metas).
 */
#pragma once

#ifndef AI_USD_USDA_SANITIZE_H_INCLUDED
#define AI_USD_USDA_SANITIZE_H_INCLUDED

#ifndef ASSIMP_BUILD_NO_USD_IMPORTER

#include <string>
#include <vector>

namespace Assimp {

/// Strip TinyUSDZ-hostile USDA constructs that Pixar samples commonly author:
/// - stage meta `kilogramsPerUnit` (not in TinyUSDZ stage-meta table)
/// - unknown prim dictionary metas (e.g. `Usd_DCE_Params = { ... }`)
/// Returns true if the buffer was modified. Notes go into `outNotes` when non-null.
bool SanitizeUsdaAscii(
        std::string &usda,
        std::vector<std::string> *outNotes);

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_IMPORTER
#endif // AI_USD_USDA_SANITIZE_H_INCLUDED
