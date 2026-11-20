/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
----------------------------------------------------------------------
*/
/** @file BlenderMeshAttributes.h
 *  Blender 5+ AttributeStorage -> classic mvert/mloop/mpoly hydration.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace Assimp {
namespace Blender {

struct Mesh;
class FileDatabase;

/** AttrType / AttrDomain / AttrStorageType from BKE_attribute_enums.hh */
enum class BlendAttrType : std::int16_t {
    Bool = 0,
    Int8 = 1,
    Int16_2D = 2,
    Int32 = 3,
    Int32_2D = 4,
    Float = 5,
    Float2 = 6,
    Float3 = 7,
    Float4x4 = 8,
    ColorByte = 9,
    ColorFloat = 10,
    Quaternion = 11,
    String = 12,
    Float4 = 13
};

enum class BlendAttrDomain : std::int8_t {
    Point = 0,
    Edge = 1,
    Face = 2,
    Corner = 3
};

enum class BlendAttrStorageType : std::int8_t {
    Array = 0,
    Single = 1
};

/** Byte size of one AttrType element; 0 if unsupported. */
inline std::size_t BlendAttrTypeSize(BlendAttrType t) {
    switch (t) {
    case BlendAttrType::Bool:
    case BlendAttrType::Int8: return 1;
    case BlendAttrType::Int16_2D: return 4;
    case BlendAttrType::Int32: return 4;
    case BlendAttrType::Int32_2D: return 8;
    case BlendAttrType::Float: return 4;
    case BlendAttrType::Float2: return 8;
    case BlendAttrType::Float3: return 12;
    case BlendAttrType::Float4:
    case BlendAttrType::ColorFloat:
    case BlendAttrType::Quaternion: return 16;
    case BlendAttrType::ColorByte: return 4;
    case BlendAttrType::Float4x4: return 64;
    default: return 0;
    }
}

/**
 * If Mesh lacks classic mvert/mloop/mpoly but has AttributeStorage
 * (position / .corner_vert / poly_offset_indices), fill the classic arrays
 * so ConvertMesh can run. Returns true if hydration produced usable topology.
 */
bool HydrateMeshFromAttributeStorage(Mesh &mesh, const FileDatabase &db);

} // namespace Blender
} // namespace Assimp
