/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
----------------------------------------------------------------------
*/
/** @file BlenderMeshAttributes.cpp */

#include "BlenderMeshAttributes.h"
#include "BlenderDNA.h"
#include "BlenderScene.h"

#include <assimp/ByteSwapper.h>
#include <cstring>
#include <string>
#include <vector>

namespace Assimp {
namespace Blender {
namespace {

const FileBlockHead *FindBlockExactAddress(const Pointer &ptr, const FileDatabase &db, size_t expectedSize) {
    if (!ptr.val) {
        return nullptr;
    }
    // Collect every block whose base address equals ptr (Blender 5 generated
    // pointers collide across IDs for AttributeStorage / AttributeArray).
    std::vector<const FileBlockHead *> exact;
    for (const FileBlockHead &e : db.entries) {
        if (e.address.val == ptr.val) {
            exact.push_back(&e);
        }
    }
    if (exact.empty()) {
        return nullptr;
    }
    if (expectedSize > 0) {
        for (const FileBlockHead *b : exact) {
            if (b->size == expectedSize) {
                return b;
            }
        }
        for (const FileBlockHead *b : exact) {
            if (b->num > 0 && static_cast<size_t>(b->num) * (b->size / b->num) == expectedSize) {
                return b;
            }
        }
        // Do not guess "largest" on size mismatch - generated addresses collide.
        return nullptr;
    }
    // No size hint: prefer largest block.
    const FileBlockHead *best = exact.front();
    for (const FileBlockHead *b : exact) {
        if (b->size > best->size) {
            best = b;
        }
    }
    return best;
}

bool ReadBlockBytes(const FileBlockHead &block, const FileDatabase &db, std::vector<uint8_t> &out) {
    out.resize(block.size);
    if (block.size == 0) {
        return true;
    }
    const StreamReaderAny::pos pold = db.reader->GetCurrentPos();
    db.reader->SetCurrentPos(block.start);
    db.reader->CopyAndAdvance(out.data(), block.size);
    db.reader->SetCurrentPos(pold);
    return true;
}

bool ReadIntArrayAtPointer(const Pointer &ptr, size_t count, const FileDatabase &db, std::vector<int> &out) {
    const size_t bytes = count * sizeof(int);
    const FileBlockHead *block = FindBlockExactAddress(ptr, db, bytes);
    if (block == nullptr) {
        // Fall back to containing-block search used by classic Locate.
        try {
            // linear scan for containing block of exact size
            for (const FileBlockHead &e : db.entries) {
                if (e.address.val == ptr.val && e.size == bytes) {
                    block = &e;
                    break;
                }
            }
        } catch (...) {
            return false;
        }
    }
    if (block == nullptr || block->size < bytes) {
        return false;
    }
    std::vector<uint8_t> raw;
    if (!ReadBlockBytes(*block, db, raw)) {
        return false;
    }
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        int v = 0;
        std::memcpy(&v, raw.data() + i * 4, 4);
        if (!db.little) {
            ByteSwap::Swap4(&v);
        }
        out[i] = v;
    }
    return true;
}

struct RawAttr {
    std::string name;
    BlendAttrType data_type = BlendAttrType::Bool;
    BlendAttrDomain domain = BlendAttrDomain::Point;
    BlendAttrStorageType storage = BlendAttrStorageType::Array;
    int64_t size = 0;
    std::vector<uint8_t> data;
};

bool LoadAttributeArrayPayload(const Pointer &arrayPtr, BlendAttrType type, const FileDatabase &db, RawAttr &out,
        int64_t preferredSize = -1, uint64_t localityHint = 0) {
    // AttributeArray DNA is 24 bytes (some writers pad allocations to 32).
    // Generated pointers collide across IDs - collect every candidate.
    static const size_t kAttrArrayDnaSize = 24;
    std::vector<const FileBlockHead *> aaBlocks;
    for (const FileBlockHead &e : db.entries) {
        if (e.address.val == arrayPtr.val && e.size >= kAttrArrayDnaSize) {
            aaBlocks.push_back(&e);
        }
    }
    if (aaBlocks.empty()) {
        return false;
    }

    const size_t elem = BlendAttrTypeSize(type);
    if (elem == 0) {
        return false;
    }

    const FileBlockHead *bestData = nullptr;
    int64_t bestSize = 0;
    int bestScore = -1;
    uint64_t bestDist = ~uint64_t(0);

    for (const FileBlockHead *aaBlock : aaBlocks) {
        std::vector<uint8_t> aaRaw;
        if (!ReadBlockBytes(*aaBlock, db, aaRaw) || aaRaw.size() < kAttrArrayDnaSize) {
            continue;
        }
        Pointer dataPtr;
        int64_t size = 0;
        std::memcpy(&dataPtr.val, aaRaw.data(), 8);
        std::memcpy(&size, aaRaw.data() + 16, 8);
        if (!db.little) {
            ByteSwap::Swap8(&dataPtr.val);
            ByteSwap::Swap8(&size);
        }
        if (size <= 0 || !dataPtr.val) {
            continue;
        }
        // Strict size match when we know the domain count - avoids wrong twin arrays.
        if (preferredSize >= 0 && size != preferredSize) {
            continue;
        }
        const size_t want = static_cast<size_t>(size) * elem;
        const FileBlockHead *dataBlock = nullptr;
        for (const FileBlockHead &e : db.entries) {
            if (e.address.val == dataPtr.val && e.size == want) {
                dataBlock = &e;
                break;
            }
        }
        if (dataBlock == nullptr) {
            continue;
        }
        int score = 2;
        if (preferredSize >= 0 && size == preferredSize) {
            score = 3;
        }
        uint64_t dist = 0;
        if (localityHint) {
            dist = dataPtr.val > localityHint ? dataPtr.val - localityHint : localityHint - dataPtr.val;
        }
        if (score > bestScore || (score == bestScore && dist < bestDist)) {
            bestScore = score;
            bestDist = dist;
            bestData = dataBlock;
            bestSize = size;
        }
    }
    if (bestData == nullptr) {
        return false;
    }
    if (!ReadBlockBytes(*bestData, db, out.data)) {
        return false;
    }
    out.size = bestSize;
    return true;
}

bool LoadAttributesFromStorage(const Mesh &mesh, const FileDatabase &db, std::vector<RawAttr> &out) {
    // attribute_storage is embedded; re-read from the Mesh file block via DNA offsets
    // already converted into mesh.attribute_* fields filled by Convert<Mesh>.
    if (mesh.attribute_dna_num <= 0 || !mesh.attribute_dna_ptr.val) {
        return false;
    }
    const size_t attrStride = 24; // blender::Attribute DNA
    const size_t want = static_cast<size_t>(mesh.attribute_dna_num) * attrStride;
    const FileBlockHead *attrsBlock = FindBlockExactAddress(mesh.attribute_dna_ptr, db, want);
    if (attrsBlock == nullptr) {
        // also try matching by num in block header
        for (const FileBlockHead &e : db.entries) {
            if (e.address.val == mesh.attribute_dna_ptr.val &&
                    e.num == static_cast<size_t>(mesh.attribute_dna_num)) {
                attrsBlock = &e;
                break;
            }
        }
    }
    if (attrsBlock == nullptr) {
        return false;
    }
    std::vector<uint8_t> raw;
    if (!ReadBlockBytes(*attrsBlock, db, raw)) {
        return false;
    }
    const size_t n = std::min(static_cast<size_t>(mesh.attribute_dna_num), raw.size() / attrStride);
    out.clear();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *base = raw.data() + i * attrStride;
        Pointer namePtr, dataPtr;
        int16_t dataType = 0;
        std::memcpy(&namePtr.val, base + 0, 8);
        std::memcpy(&dataType, base + 8, 2);
        const int8_t domain = static_cast<int8_t>(base[10]);
        const int8_t storage = static_cast<int8_t>(base[11]);
        std::memcpy(&dataPtr.val, base + 16, 8);
        if (!db.little) {
            ByteSwap::Swap8(&namePtr.val);
            ByteSwap::Swap2(&dataType);
            ByteSwap::Swap8(&dataPtr.val);
        }

        RawAttr attr;
        attr.data_type = static_cast<BlendAttrType>(dataType);
        attr.domain = static_cast<BlendAttrDomain>(domain);
        attr.storage = static_cast<BlendAttrStorageType>(storage);

        // name is a DATA string block
        for (const FileBlockHead &e : db.entries) {
            if (e.address.val == namePtr.val && e.size > 0) {
                std::vector<uint8_t> nb;
                if (ReadBlockBytes(e, db, nb)) {
                    size_t len = 0;
                    while (len < nb.size() && nb[len] != 0) {
                        ++len;
                    }
                    attr.name.assign(reinterpret_cast<const char *>(nb.data()), len);
                }
                break;
            }
        }

        if (attr.storage == BlendAttrStorageType::Array && dataPtr.val) {
            int64_t preferred = -1;
            if (attr.domain == BlendAttrDomain::Point) {
                preferred = mesh.totvert;
            } else if (attr.domain == BlendAttrDomain::Corner) {
                preferred = mesh.totloop;
            } else if (attr.domain == BlendAttrDomain::Face) {
                preferred = mesh.totpoly;
            } else if (attr.domain == BlendAttrDomain::Edge) {
                preferred = mesh.totedge;
            }
            const uint64_t hint = mesh.attribute_dna_ptr.val ? mesh.attribute_dna_ptr.val : mesh.poly_offset_ptr.val;
            if (!LoadAttributeArrayPayload(dataPtr, attr.data_type, db, attr, preferred, hint)) {
                continue;
            }
        }
        out.push_back(std::move(attr));
    }
    return !out.empty();
}

const RawAttr *FindAttr(const std::vector<RawAttr> &attrs, const char *name, BlendAttrDomain domain, BlendAttrType type) {
    for (const RawAttr &a : attrs) {
        if (a.domain == domain && a.data_type == type && a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

} // namespace

bool HydrateMeshFromAttributeStorage(Mesh &mesh, const FileDatabase &db) {
    if (mesh.totvert <= 0) {
        return false;
    }
    const bool needVerts = mesh.mvert.size() < static_cast<size_t>(mesh.totvert);
    const bool needLoops = mesh.totloop > 0 && mesh.mloop.size() < static_cast<size_t>(mesh.totloop);
    const bool needPolys = mesh.totpoly > 0 && mesh.mpoly.size() < static_cast<size_t>(mesh.totpoly);
    if (!needVerts && !needLoops && !needPolys) {
        return false;
    }

    // Face offsets: Mesh.poly_offset_indices (totpoly+1 ints)
    if (needPolys && mesh.poly_offset_indices.size() < static_cast<size_t>(mesh.totpoly) + 1) {
        if (mesh.poly_offset_ptr.val) {
            std::vector<int> offsets;
            if (ReadIntArrayAtPointer(mesh.poly_offset_ptr, static_cast<size_t>(mesh.totpoly) + 1, db, offsets)) {
                mesh.poly_offset_indices.assign(offsets.begin(), offsets.end());
            }
        }
    }

    std::vector<RawAttr> attrs;
    if (!LoadAttributesFromStorage(mesh, db, attrs)) {
        ASSIMP_LOG_WARN("BLEND: AttributeStorage load failed for mesh (dna_num=",
                mesh.attribute_dna_num, " dna_ptr=", mesh.attribute_dna_ptr.val, ")");
        return false;
    }

    auto clearPartial = [&mesh]() {
        mesh.mvert.clear();
        mesh.mloop.clear();
        mesh.mpoly.clear();
        mesh.poly_offset_indices.clear();
    };

    if (needVerts) {
        const RawAttr *pos = FindAttr(attrs, "position", BlendAttrDomain::Point, BlendAttrType::Float3);
        if (pos == nullptr || pos->size < mesh.totvert || pos->data.size() < static_cast<size_t>(mesh.totvert) * 12) {
            ASSIMP_LOG_WARN("BLEND: missing position attr for hydrate (attrs=", attrs.size(),
                    " pos=", (pos ? pos->size : -1), " totvert=", mesh.totvert, ")");
            clearPartial();
            return false;
        }
        mesh.mvert.resize(static_cast<size_t>(mesh.totvert));
        for (int i = 0; i < mesh.totvert; ++i) {
            float co[3];
            std::memcpy(co, pos->data.data() + static_cast<size_t>(i) * 12, 12);
            if (!db.little) {
                ByteSwap::Swap4(&co[0]);
                ByteSwap::Swap4(&co[1]);
                ByteSwap::Swap4(&co[2]);
            }
            mesh.mvert[i].co[0] = co[0];
            mesh.mvert[i].co[1] = co[1];
            mesh.mvert[i].co[2] = co[2];
            mesh.mvert[i].no[0] = 0.f;
            mesh.mvert[i].no[1] = 0.f;
            mesh.mvert[i].no[2] = 1.f;
        }
    }

    if (needLoops && mesh.totloop > 0) {
        const RawAttr *cv = FindAttr(attrs, ".corner_vert", BlendAttrDomain::Corner, BlendAttrType::Int32);
        if (cv == nullptr || cv->size < mesh.totloop || cv->data.size() < static_cast<size_t>(mesh.totloop) * 4) {
            ASSIMP_LOG_WARN("BLEND: missing .corner_vert attr for hydrate (attrs=", attrs.size(),
                    " cv=", (cv ? cv->size : -1), " totloop=", mesh.totloop, ")");
            clearPartial();
            return false;
        }
        mesh.mloop.resize(static_cast<size_t>(mesh.totloop));
        for (int i = 0; i < mesh.totloop; ++i) {
            int v = 0;
            std::memcpy(&v, cv->data.data() + static_cast<size_t>(i) * 4, 4);
            if (!db.little) {
                ByteSwap::Swap4(&v);
            }
            mesh.mloop[i].v = v;
            mesh.mloop[i].e = 0;
        }
    }

    if (needPolys && mesh.totpoly > 0 &&
            mesh.poly_offset_indices.size() >= static_cast<size_t>(mesh.totpoly) + 1) {
        mesh.mpoly.resize(static_cast<size_t>(mesh.totpoly));
        // Optional face material_index (Int32 on Face domain)
        const RawAttr *mat = FindAttr(attrs, "material_index", BlendAttrDomain::Face, BlendAttrType::Int32);
        for (int i = 0; i < mesh.totpoly; ++i) {
            const int start = mesh.poly_offset_indices[static_cast<size_t>(i)];
            const int end = mesh.poly_offset_indices[static_cast<size_t>(i) + 1];
            mesh.mpoly[i].loopstart = start;
            mesh.mpoly[i].totloop = end - start;
            mesh.mpoly[i].mat_nr = 0;
            if (mat != nullptr && mat->size > i && mat->data.size() >= static_cast<size_t>(i + 1) * 4) {
                int mi = 0;
                std::memcpy(&mi, mat->data.data() + static_cast<size_t>(i) * 4, 4);
                if (!db.little) {
                    ByteSwap::Swap4(&mi);
                }
                if (mi >= 0 && (mesh.mat.empty() || static_cast<size_t>(mi) < mesh.mat.size())) {
                    mesh.mpoly[i].mat_nr = static_cast<short>(mi);
                }
            }
        }
    }

    if (mesh.mvert.size() < static_cast<size_t>(mesh.totvert) ||
            (mesh.totloop > 0 && mesh.mloop.size() < static_cast<size_t>(mesh.totloop)) ||
            (mesh.totpoly > 0 && mesh.mpoly.size() < static_cast<size_t>(mesh.totpoly))) {
        clearPartial();
        return false;
    }
    // Reject hydrates whose corner indices are clearly from a twin mesh.
    for (int i = 0; i < mesh.totloop; ++i) {
        if (mesh.mloop[static_cast<size_t>(i)].v < 0 ||
                mesh.mloop[static_cast<size_t>(i)].v >= mesh.totvert) {
            ASSIMP_LOG_WARN("BLEND: hydrated loop index out of range; discarding");
            clearPartial();
            return false;
        }
    }
    return true;
}

} // namespace Blender
} // namespace Assimp
