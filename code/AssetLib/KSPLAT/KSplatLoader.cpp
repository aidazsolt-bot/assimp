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

/** @file KSplatLoader.cpp
 *  @brief mkkellogg .ksplat importer -> aiGaussianSplat (KHR-native).
 *
 *  Layout matches GaussianSplats3D SplatBuffer / PlayCanvas splat-transform
 *  read-ksplat (header 4096, section headers 1024, buckets + per-splat records).
 */

#ifndef ASSIMP_BUILD_NO_KSPLAT_IMPORTER

#include "KSplatLoader.h"

#include <assimp/ByteSwapper.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/gaussian.h>
#include <assimp/importerdesc.h>
#include <assimp/scene.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace Assimp;

namespace {

static constexpr size_t kMainHeaderBytes = 4096u;
static constexpr size_t kSectionHeaderBytes = 1024u;

/** Y_0,0 - baked display RGB = 0.5 + SH_C0 * f_dc (same as .splat). */
static constexpr float kShC0 = 0.28209479177387814f;

/** SH rest scalars for degrees 0..3 (mkkellogg / PlayCanvas). */
static constexpr unsigned kHarmonicsComponentCount[4] = { 0u, 9u, 24u, 45u };

struct CompressionConfig {
    unsigned centerBytes;
    unsigned scaleBytes;
    unsigned rotationBytes;
    unsigned colorBytes;
    unsigned harmonicsBytes;
    unsigned scaleStartByte;
    unsigned rotationStartByte;
    unsigned colorStartByte;
    unsigned harmonicsStartByte;
    unsigned scaleQuantRange;
};

static constexpr CompressionConfig kCompressionModes[3] = {
        { 12u, 12u, 16u, 4u, 4u, 12u, 24u, 40u, 44u, 1u },
        { 6u, 6u, 8u, 4u, 2u, 6u, 12u, 20u, 24u, 32767u },
        { 6u, 6u, 8u, 4u, 1u, 6u, 12u, 20u, 24u, 32767u },
};

static constexpr aiImporterDesc desc = {
        "KSPLAT 3D Gaussian Splat Importer",
        "",
        "",
        "mkkellogg GaussianSplats3D .ksplat (sectioned/bucketed)",
        aiImporterFlags_SupportBinaryFlavour,
        0,
        0,
        0,
        0,
        "ksplat"
};

inline uint16_t ReadU16LE(const uint8_t *p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    AI_SWAP2(v);
    return v;
}

inline uint32_t ReadU32LE(const uint8_t *p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    AI_SWAP4(v);
    return v;
}

inline float ReadF32LE(const uint8_t *p) {
    float v = 0.f;
    std::memcpy(&v, p, sizeof(v));
    AI_SWAP4(v);
    return v;
}

/** IEEE-754 binary16 -> float32 (PlayCanvas decodeFloat16). */
inline float DecodeFloat16(uint16_t encoded) {
    const unsigned signBit = (encoded >> 15) & 1u;
    const unsigned exponent = (encoded >> 10) & 0x1fu;
    unsigned mantissa = encoded & 0x3ffu;

    if (exponent == 0) {
        if (mantissa == 0) {
            return signBit ? -0.f : 0.f;
        }
        int exp = -14;
        while ((mantissa & 0x400u) == 0u) {
            mantissa <<= 1;
            --exp;
        }
        mantissa &= 0x3ffu;
        const unsigned finalExp = static_cast<unsigned>(exp + 127);
        const unsigned finalMantissa = mantissa << 13;
        const uint32_t bits = (signBit << 31) | (finalExp << 23) | finalMantissa;
        float out = 0.f;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    if (exponent == 0x1fu) {
        return mantissa == 0 ? (signBit ? -INFINITY : INFINITY) : NAN;
    }

    const unsigned finalExp = exponent - 15u + 127u;
    const unsigned finalMantissa = mantissa << 13;
    const uint32_t bits = (signBit << 31) | (finalExp << 23) | finalMantissa;
    float out = 0.f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

/** Map packed SH component index to PLY-style f_rest / mRest slot (PlayCanvas). */
inline unsigned HarmonicsDestIndex(unsigned i, unsigned harmonicsComponentCount) {
    unsigned channel = 0;
    unsigned coeff = 0;
    if (i < 9u) {
        channel = i / 3u;
        coeff = i % 3u;
    } else if (i < 24u) {
        channel = (i - 9u) / 5u;
        coeff = ((i - 9u) % 5u) + 3u;
    } else {
        channel = (i - 24u) / 7u;
        coeff = ((i - 24u) % 7u) + 8u;
    }
    return channel * (harmonicsComponentCount / 3u) + coeff;
}

} // namespace

// ------------------------------------------------------------------------------------------------
bool KSPLATImporter::CanRead(const std::string &pFile, IOSystem *pIOHandler, bool checkSig) const {
    const std::string extension = GetExtension(pFile);
    if (extension == "ksplat") {
        return true;
    }
    if (!checkSig || !pIOHandler) {
        return false;
    }
    std::unique_ptr<IOStream> file(pIOHandler->Open(pFile, "rb"));
    if (!file || file->FileSize() < kMainHeaderBytes) {
        return false;
    }
    uint8_t hdr[2] = { 0, 0 };
    if (file->Read(hdr, 1, 2) != 2) {
        return false;
    }
    return hdr[0] == 0 && hdr[1] >= 1;
}

// ------------------------------------------------------------------------------------------------
const aiImporterDesc *KSPLATImporter::GetInfo() const {
    return &desc;
}

// ------------------------------------------------------------------------------------------------
void KSPLATImporter::InternReadFile(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
    std::unique_ptr<IOStream> file(pIOHandler->Open(pFile, "rb"));
    if (!file) {
        throw DeadlyImportError("Failed to open KSPLAT file ", pFile, ".");
    }

    const size_t fileSize = file->FileSize();
    if (fileSize < kMainHeaderBytes || fileSize == SIZE_MAX) {
        throw DeadlyImportError("KSPLAT file too small or unreadable: ", pFile);
    }

    std::vector<uint8_t> buffer(fileSize);
    if (file->Read(buffer.data(), 1, fileSize) != fileSize) {
        throw DeadlyImportError("Failed to read KSPLAT file ", pFile, ".");
    }

    const uint8_t *const data = buffer.data();
    const uint8_t majorVersion = data[0];
    const uint8_t minorVersion = data[1];
    if (majorVersion != 0 || minorVersion < 1) {
        throw DeadlyImportError("KSPLAT: unsupported version ", static_cast<unsigned>(majorVersion), ".",
                static_cast<unsigned>(minorVersion));
    }

    const uint32_t maxSections = ReadU32LE(data + 4);
    const uint32_t numSplats = ReadU32LE(data + 16);
    const uint16_t compressionMode = ReadU16LE(data + 20);
    if (compressionMode > 2) {
        throw DeadlyImportError("KSPLAT: invalid compression mode ", compressionMode);
    }
    if (numSplats == 0) {
        throw DeadlyImportError("KSPLAT: file has no gaussians");
    }
    if (maxSections == 0) {
        throw DeadlyImportError("KSPLAT: maxSections is zero");
    }

    const size_t headersEnd = kMainHeaderBytes + size_t(maxSections) * kSectionHeaderBytes;
    if (fileSize < headersEnd) {
        throw DeadlyImportError("KSPLAT: truncated section headers");
    }

    float minHarmonicsValue = ReadF32LE(data + 36);
    float maxHarmonicsValue = ReadF32LE(data + 40);
    if (minHarmonicsValue == 0.f && maxHarmonicsValue == 0.f) {
        minHarmonicsValue = -1.5f;
        maxHarmonicsValue = 1.5f;
    }

    unsigned maxHarmonicsDegree = 0;
    for (uint32_t sectionIdx = 0; sectionIdx < maxSections; ++sectionIdx) {
        const uint8_t *sec = data + kMainHeaderBytes + size_t(sectionIdx) * kSectionHeaderBytes;
        const uint32_t sectionSplatCount = ReadU32LE(sec + 0);
        if (sectionSplatCount == 0) {
            continue;
        }
        const uint16_t harmonicsDegree = ReadU16LE(sec + 40);
        if (harmonicsDegree > 3) {
            throw DeadlyImportError("KSPLAT: harmonicsDegree out of range");
        }
        if (harmonicsDegree > maxHarmonicsDegree) {
            maxHarmonicsDegree = harmonicsDegree;
        }
    }

    const unsigned restCoeffs = kHarmonicsComponentCount[maxHarmonicsDegree];
    const CompressionConfig &cfg = kCompressionModes[compressionMode];

    std::unique_ptr<aiGaussianSplat> splat(new aiGaussianSplat());
    splat->mNumPoints = numSplats;
    splat->mNumRestCoeffs = restCoeffs;
    splat->mDC = new aiVector3D[numSplats];
    splat->mScale = new aiVector3D[numSplats];
    splat->mRotation = new aiColor4D[numSplats];
    splat->mOpacity = new ai_real[numSplats];
    if (restCoeffs > 0) {
        splat->mRest = new ai_real[size_t(numSplats) * restCoeffs];
        std::memset(splat->mRest, 0, size_t(numSplats) * restCoeffs * sizeof(ai_real));
    }

    std::unique_ptr<aiMesh> meshOwned(new aiMesh());
    aiMesh *mesh = meshOwned.get();
    mesh->mName.Set("KSPLAT_Gaussians");
    mesh->mPrimitiveTypes = aiPrimitiveType_POINT;
    mesh->mNumVertices = numSplats;
    mesh->mVertices = new aiVector3D[numSplats];
    mesh->mNumFaces = numSplats;
    mesh->mFaces = new aiFace[numSplats];
    mesh->mMaterialIndex = 0;

    size_t currentSectionDataOffset = headersEnd;
    unsigned splatIndex = 0;

    for (uint32_t sectionIdx = 0; sectionIdx < maxSections; ++sectionIdx) {
        const uint8_t *sec = data + kMainHeaderBytes + size_t(sectionIdx) * kSectionHeaderBytes;
        const uint32_t sectionSplatCount = ReadU32LE(sec + 0);
        const uint32_t maxSectionSplats = ReadU32LE(sec + 4);
        const uint32_t bucketCapacity = ReadU32LE(sec + 8);
        const uint32_t bucketCount = ReadU32LE(sec + 12);
        const float spatialBlockSize = ReadF32LE(sec + 16);
        const uint16_t bucketStorageSize = ReadU16LE(sec + 20);
        uint32_t quantizationRange = ReadU32LE(sec + 24);
        if (quantizationRange == 0) {
            quantizationRange = cfg.scaleQuantRange;
        }
        const uint32_t fullBuckets = ReadU32LE(sec + 32);
        const uint32_t partialBuckets = ReadU32LE(sec + 36);
        const uint16_t harmonicsDegree = ReadU16LE(sec + 40);

        if (sectionSplatCount == 0) {
            // Still advance storage for empty reserved sections if maxSectionSplats set.
            const unsigned harmN = kHarmonicsComponentCount[harmonicsDegree <= 3 ? harmonicsDegree : 0];
            const size_t bytesPerSplat = cfg.centerBytes + cfg.scaleBytes + cfg.rotationBytes + cfg.colorBytes +
                                        size_t(harmN) * cfg.harmonicsBytes;
            const size_t partialBucketMetaSize = size_t(partialBuckets) * 4u;
            const size_t totalBucketStorageSize = size_t(bucketStorageSize) * bucketCount + partialBucketMetaSize;
            currentSectionDataOffset += bytesPerSplat * maxSectionSplats + totalBucketStorageSize;
            continue;
        }

        if (bucketCapacity == 0 && compressionMode != 0) {
            throw DeadlyImportError("KSPLAT: bucketCapacity is zero");
        }
        if (harmonicsDegree > 3) {
            throw DeadlyImportError("KSPLAT: harmonicsDegree out of range");
        }

        const uint32_t fullBucketSplats = fullBuckets * bucketCapacity;
        const size_t partialBucketMetaSize = size_t(partialBuckets) * 4u;
        const size_t totalBucketStorageSize = size_t(bucketStorageSize) * bucketCount + partialBucketMetaSize;
        const unsigned harmonicsComponentCount = kHarmonicsComponentCount[harmonicsDegree];
        const size_t bytesPerSplat = cfg.centerBytes + cfg.scaleBytes + cfg.rotationBytes + cfg.colorBytes +
                                    size_t(harmonicsComponentCount) * cfg.harmonicsBytes;
        const size_t sectionDataSize = bytesPerSplat * maxSectionSplats;

        if (currentSectionDataOffset + totalBucketStorageSize + sectionDataSize > fileSize) {
            throw DeadlyImportError("KSPLAT: truncated section data");
        }
        if (sectionSplatCount > maxSectionSplats) {
            throw DeadlyImportError("KSPLAT: sectionSplatCount exceeds maxSectionSplats");
        }
        if (splatIndex + sectionSplatCount > numSplats) {
            throw DeadlyImportError("KSPLAT: splat count exceeds header numSplats");
        }

        const float positionScale = spatialBlockSize / 2.0f / static_cast<float>(quantizationRange);

        const uint8_t *partialBucketSizesBytes = data + currentSectionDataOffset;
        const float *bucketCenters = reinterpret_cast<const float *>(data + currentSectionDataOffset + partialBucketMetaSize);
        const uint8_t *splatData = data + currentSectionDataOffset + totalBucketStorageSize;

        uint32_t currentPartialBucket = fullBuckets;
        uint32_t currentPartialBase = fullBucketSplats;

        auto decodeHarmonics = [&](size_t splatByteOffset, unsigned component) -> float {
            const size_t base = splatByteOffset + cfg.harmonicsStartByte;
            switch (compressionMode) {
            case 0:
                return ReadF32LE(splatData + base + size_t(component) * 4u);
            case 1:
                return DecodeFloat16(ReadU16LE(splatData + base + size_t(component) * 2u));
            case 2: {
                const float normalized = static_cast<float>(splatData[base + component]) / 255.f;
                return minHarmonicsValue + normalized * (maxHarmonicsValue - minHarmonicsValue);
            }
            default:
                return 0.f;
            }
        };

        for (uint32_t splatIdx = 0; splatIdx < sectionSplatCount; ++splatIdx) {
            const size_t splatByteOffset = size_t(splatIdx) * bytesPerSplat;

            uint32_t bucketIdx = 0;
            if (compressionMode != 0) {
                if (splatIdx < fullBucketSplats) {
                    bucketIdx = splatIdx / bucketCapacity;
                } else {
                    if (partialBuckets == 0) {
                        throw DeadlyImportError("KSPLAT: partial bucket required but missing");
                    }
                    uint32_t currentBucketSize = ReadU32LE(partialBucketSizesBytes + size_t(currentPartialBucket - fullBuckets) * 4u);
                    if (splatIdx >= currentPartialBase + currentBucketSize) {
                        ++currentPartialBucket;
                        currentPartialBase += currentBucketSize;
                        currentBucketSize = ReadU32LE(partialBucketSizesBytes + size_t(currentPartialBucket - fullBuckets) * 4u);
                    }
                    bucketIdx = currentPartialBucket;
                }
                if (bucketIdx >= bucketCount) {
                    throw DeadlyImportError("KSPLAT: bucket index out of range");
                }
            }

            float x = 0.f, y = 0.f, z = 0.f;
            if (compressionMode == 0) {
                x = ReadF32LE(splatData + splatByteOffset);
                y = ReadF32LE(splatData + splatByteOffset + 4);
                z = ReadF32LE(splatData + splatByteOffset + 8);
            } else {
                const uint8_t *bc = reinterpret_cast<const uint8_t *>(bucketCenters) + size_t(bucketIdx) * 12u;
                const float bcx = ReadF32LE(bc);
                const float bcy = ReadF32LE(bc + 4);
                const float bcz = ReadF32LE(bc + 8);
                x = (static_cast<float>(ReadU16LE(splatData + splatByteOffset)) - static_cast<float>(quantizationRange)) * positionScale +
                    bcx;
                y = (static_cast<float>(ReadU16LE(splatData + splatByteOffset + 2)) - static_cast<float>(quantizationRange)) * positionScale +
                    bcy;
                z = (static_cast<float>(ReadU16LE(splatData + splatByteOffset + 4)) - static_cast<float>(quantizationRange)) * positionScale +
                    bcz;
            }

            float scaleX = 0.f, scaleY = 0.f, scaleZ = 0.f;
            if (compressionMode == 0) {
                scaleX = ReadF32LE(splatData + splatByteOffset + cfg.scaleStartByte);
                scaleY = ReadF32LE(splatData + splatByteOffset + cfg.scaleStartByte + 4);
                scaleZ = ReadF32LE(splatData + splatByteOffset + cfg.scaleStartByte + 8);
            } else {
                scaleX = DecodeFloat16(ReadU16LE(splatData + splatByteOffset + cfg.scaleStartByte));
                scaleY = DecodeFloat16(ReadU16LE(splatData + splatByteOffset + cfg.scaleStartByte + 2));
                scaleZ = DecodeFloat16(ReadU16LE(splatData + splatByteOffset + cfg.scaleStartByte + 4));
            }
            if (scaleX < 0.f) {
                scaleX = 0.f;
            }
            if (scaleY < 0.f) {
                scaleY = 0.f;
            }
            if (scaleZ < 0.f) {
                scaleZ = 0.f;
            }

            // File stores wxyz (mkkellogg SplatBuffer read path).
            float qw = 0.f, qx = 0.f, qy = 0.f, qz = 0.f;
            if (compressionMode == 0) {
                qw = ReadF32LE(splatData + splatByteOffset + cfg.rotationStartByte);
                qx = ReadF32LE(splatData + splatByteOffset + cfg.rotationStartByte + 4);
                qy = ReadF32LE(splatData + splatByteOffset + cfg.rotationStartByte + 8);
                qz = ReadF32LE(splatData + splatByteOffset + cfg.rotationStartByte + 12);
            } else {
                qw = DecodeFloat16(ReadU16LE(splatData + splatByteOffset + cfg.rotationStartByte));
                qx = DecodeFloat16(ReadU16LE(splatData + splatByteOffset + cfg.rotationStartByte + 2));
                qy = DecodeFloat16(ReadU16LE(splatData + splatByteOffset + cfg.rotationStartByte + 4));
                qz = DecodeFloat16(ReadU16LE(splatData + splatByteOffset + cfg.rotationStartByte + 6));
            }
            const double qlen = std::sqrt(double(qx) * qx + double(qy) * qy + double(qz) * qz + double(qw) * qw);
            if (qlen > 0.0) {
                const float inv = static_cast<float>(1.0 / qlen);
                qx *= inv;
                qy *= inv;
                qz *= inv;
                qw *= inv;
            } else {
                qx = qy = qz = 0.f;
                qw = 1.f;
            }

            const float red = static_cast<float>(splatData[splatByteOffset + cfg.colorStartByte]) / 255.f;
            const float green = static_cast<float>(splatData[splatByteOffset + cfg.colorStartByte + 1]) / 255.f;
            const float blue = static_cast<float>(splatData[splatByteOffset + cfg.colorStartByte + 2]) / 255.f;
            float op = static_cast<float>(splatData[splatByteOffset + cfg.colorStartByte + 3]) / 255.f;
            if (op < 0.f) {
                op = 0.f;
            } else if (op > 1.f) {
                op = 1.f;
            }

            mesh->mVertices[splatIndex].x = x;
            mesh->mVertices[splatIndex].y = y;
            mesh->mVertices[splatIndex].z = z;
            mesh->mFaces[splatIndex].mNumIndices = 1;
            mesh->mFaces[splatIndex].mIndices = new unsigned int[1];
            mesh->mFaces[splatIndex].mIndices[0] = splatIndex;

            splat->mScale[splatIndex].x = scaleX;
            splat->mScale[splatIndex].y = scaleY;
            splat->mScale[splatIndex].z = scaleZ;
            splat->mOpacity[splatIndex] = op;
            splat->mDC[splatIndex].x = (red - 0.5f) / kShC0;
            splat->mDC[splatIndex].y = (green - 0.5f) / kShC0;
            splat->mDC[splatIndex].z = (blue - 0.5f) / kShC0;
            // KHR / glTF xyzw as (r,g,b,a)
            splat->mRotation[splatIndex].r = qx;
            splat->mRotation[splatIndex].g = qy;
            splat->mRotation[splatIndex].b = qz;
            splat->mRotation[splatIndex].a = qw;

            if (harmonicsComponentCount > 0 && splat->mRest != nullptr) {
                ai_real *dst = splat->mRest + size_t(splatIndex) * restCoeffs;
                for (unsigned i = 0; i < harmonicsComponentCount; ++i) {
                    const unsigned col = HarmonicsDestIndex(i, harmonicsComponentCount);
                    if (col < restCoeffs) {
                        dst[col] = decodeHarmonics(splatByteOffset, i);
                    }
                }
            }

            ++splatIndex;
        }

        currentSectionDataOffset += sectionDataSize + totalBucketStorageSize;
    }

    if (splatIndex != numSplats) {
        throw DeadlyImportError("KSPLAT: splat count mismatch (header ", numSplats, ", decoded ", splatIndex, ")");
    }

    std::unique_ptr<aiMaterial> matOwned(new aiMaterial());
    aiString matName("KSPLAT_Default");
    matOwned->AddProperty(&matName, AI_MATKEY_NAME);

    pScene->mNumMaterials = 1;
    pScene->mMaterials = new aiMaterial *[1];
    pScene->mMaterials[0] = matOwned.release();

    pScene->mNumMeshes = 1;
    pScene->mMeshes = new aiMesh *[1];
    pScene->mMeshes[0] = meshOwned.release();

    pScene->mRootNode = new aiNode();
    pScene->mRootNode->mName.Set("KSPLAT_Root");
    pScene->mRootNode->mNumMeshes = 1;
    pScene->mRootNode->mMeshes = new unsigned int[1];
    pScene->mRootNode->mMeshes[0] = 0;

    AttachGaussianSplat(pScene, 0, splat.release());

    if (pScene->mMetaData == nullptr) {
        pScene->mMetaData = new aiMetadata();
    }
    pScene->mMetaData->Add(AI_METADATA_GAUSSIAN_SPLAT, true);
    pScene->mMetaData->Add(AI_METADATA_GAUSSIAN_SH_REST_COUNT, int32_t(restCoeffs));

    if (pScene->mRootNode->mMetaData == nullptr) {
        pScene->mRootNode->mMetaData = new aiMetadata();
    }
    pScene->mRootNode->mMetaData->Add(AI_METADATA_GAUSSIAN_SPLAT, true);

    ASSIMP_LOG_INFO("KSPLAT: loaded ", numSplats, " gaussians (compression=", compressionMode,
            ", restCoeffs=", restCoeffs, ").");
}

#endif // !! ASSIMP_BUILD_NO_KSPLAT_IMPORTER
