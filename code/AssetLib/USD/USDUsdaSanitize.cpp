/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
All rights reserved.
----------------------------------------------------------------------
*/

#ifndef ASSIMP_BUILD_NO_USD_IMPORTER

#include "USDUsdaSanitize.h"

#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace Assimp {
namespace {

bool IsIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

size_t SkipBalancedBrace(const std::string &s, size_t openBrace) {
    if (openBrace >= s.size() || s[openBrace] != '{') {
        return openBrace;
    }
    int depth = 0;
    for (size_t i = openBrace; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return i + 1;
            }
        }
    }
    return s.size();
}

// Only strip metas TinyUSDZ is known to hard-fail on for Host FX samples.
// Do NOT strip generic Ident={ (that would eat filePath.timeSamples, etc.).
const std::unordered_set<std::string> &HostileDictPrimMetas() {
    static const std::unordered_set<std::string> k = {
            "Usd_DCE_Params",
    };
    return k;
}

} // namespace

bool SanitizeUsdaAscii(std::string &usda, std::vector<std::string> *outNotes) {
    if (usda.empty()) {
        return false;
    }

    bool changed = false;
    std::string out;
    out.reserve(usda.size());

    size_t i = 0;
    while (i < usda.size()) {
        if (!IsIdentStart(usda[i])) {
            out.push_back(usda[i]);
            ++i;
            continue;
        }

        const size_t identStart = i;
        ++i;
        while (i < usda.size() && IsIdentChar(usda[i])) {
            ++i;
        }
        const std::string ident = usda.substr(identStart, i - identStart);

        // Attribute paths like filePath.timeSamples must never be treated as metas.
        const bool dotted = (identStart > 0 && usda[identStart - 1] == '.');

        size_t j = i;
        while (j < usda.size() && (usda[j] == ' ' || usda[j] == '\t')) {
            ++j;
        }

        // Stage meta: kilogramsPerUnit = <number>
        if (!dotted && ident == "kilogramsPerUnit" && j < usda.size() && usda[j] == '=') {
            while (j < usda.size() && usda[j] != '\n' && usda[j] != '\r') {
                ++j;
            }
            if (outNotes) {
                outNotes->push_back("sanitized:stripped_stage_meta:kilogramsPerUnit");
            }
            changed = true;
            i = j;
            continue;
        }

        // Hostile prim dictionary meta: Usd_DCE_Params = { ... }
        if (!dotted && HostileDictPrimMetas().count(ident) != 0 && j < usda.size() && usda[j] == '=') {
            size_t k = j + 1;
            while (k < usda.size() && (usda[k] == ' ' || usda[k] == '\t')) {
                ++k;
            }
            if (k < usda.size() && usda[k] == '{') {
                const size_t end = SkipBalancedBrace(usda, k);
                if (outNotes) {
                    outNotes->push_back(std::string("sanitized:stripped_prim_dict_meta:") + ident);
                }
                changed = true;
                i = end;
                continue;
            }
        }

        out.append(usda, identStart, i - identStart);
    }

    if (changed) {
        usda.swap(out);
    }
    return changed;
}

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_IMPORTER
