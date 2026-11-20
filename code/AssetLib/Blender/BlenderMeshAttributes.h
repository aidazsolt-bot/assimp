/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
----------------------------------------------------------------------
*/
/** @file BlenderMeshAttributes.h
 *  Blender 5+ AttributeStorage -> classic mvert/mloop/mpoly hydration.
 *
 *  DNA layouts and enums are taken from blender.git:
 *  - source/blender/makesdna/DNA_attribute_types.h
 *  - source/blender/blenkernel/BKE_attribute_enums.hh
 *  - source/blender/makesdna/DNA_mesh_types.h (face_offset_indices / attribute_storage)
 *  - source/blender/makesdna/intern/dna_rename_defs.h (poly_offset_indices -> face_offset_indices, etc.)
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace Assimp {
namespace Blender {

struct Mesh;
class FileDatabase;

/** blender::bke::AttrType (BKE_attribute_enums.hh) */
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

/** blender::bke::AttrDomain (BKE_attribute_enums.hh) */
enum class BlendAttrDomain : std::int8_t {
    Point = 0,
    Edge = 1,
    Face = 2,
    Corner = 3,
    Curve = 4,
    Instance = 5,
    Layer = 6
};

/** blender::bke::AttrStorageType (BKE_attribute_enums.hh) */
enum class BlendAttrStorageType : std::int8_t {
    Array = 0,
    Single = 1
};

/**
 * blender::Attribute DNA (DNA_attribute_types.h), 64-bit:
 *   +0  char *name
 *   +8  int16_t data_type
 *  +10  int8_t domain
 *  +11  int8_t storage_type
 *  +12  char _pad[4]
 *  +16  void *data   (AttributeArray* or AttributeSingle*)
 *  size 24
 */
static constexpr std::size_t kBlendAttrDnaSize = 24;

/**
 * blender::AttributeArray DNA (DNA_attribute_types.h), 64-bit:
 *   +0  void *data
 *   +8  ImplicitSharingInfoHandle *sharing_info
 *  +16  int64_t size
 *  +24  int8_t is_single   (absent in early 5.0 DNA -> struct size 24)
 *  +25  char _pad[7]
 *  size 32 (current blender.git)
 */
static constexpr std::size_t kBlendAttrArrayDnaSizeMin = 24;
static constexpr std::size_t kBlendAttrArrayDnaSize = 32;

/**
 * blender::AttributeSingle DNA (DNA_attribute_types.h), 64-bit:
 *   +0  void *data
 *   +8  ImplicitSharingInfoHandle *sharing_info
 *  size 16
 */
static constexpr std::size_t kBlendAttrSingleDnaSize = 16;

/** Byte size of one AttrType element; 0 if unsupported. Matches CPPType sizes used by Blender. */
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
    case BlendAttrType::String: return 0; // MStringProperty - not needed for mesh hydrate
    default: return 0;
    }
}

/**
 * If Mesh lacks classic mvert/mloop/mpoly but has AttributeStorage
 * (position / .corner_vert / face_offset_indices), fill the classic arrays
 * so ConvertMesh can run. Returns true if hydration produced usable topology.
 */
bool HydrateMeshFromAttributeStorage(Mesh &mesh, const FileDatabase &db);

} // namespace Blender
} // namespace Assimp
