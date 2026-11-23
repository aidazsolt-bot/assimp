/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
----------------------------------------------------------------------
*/

/** @file BlenderParticleDistribute.cpp
 *  Mesh-face particle placement for BLEND PointInstancer sidecar arrays.
 */

#ifndef ASSIMP_BUILD_NO_BLEND_IMPORTER

#include "BlenderParticleDistribute.h"
#include <cmath>
#include <cstdint>

namespace Assimp {
namespace Blender {
namespace {

constexpr short kFromFace = 1;
constexpr int kFlagTrand = 1 << 7;
constexpr int kFlagEdistr = 1 << 8;

struct Tri {
    float v0[3], v1[3], v2[3];
    float area;
};

uint32_t HashU32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float HashFloat(uint32_t seed, uint32_t salt) {
    return float(HashU32(seed ^ (salt * 0x9e3779b9u)) & 0x00ffffffu) / float(0x01000000u);
}

float TriArea(const float a[3], const float b[3], const float c[3]) {
    const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
    const float acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
    const float cx = aby * acz - abz * acy;
    const float cy = abz * acx - abx * acz;
    const float cz = abx * acy - aby * acx;
    return 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
}

void FaceNormal(const float a[3], const float b[3], const float c[3], float n[3]) {
    const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
    const float acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
    n[0] = aby * acz - abz * acy;
    n[1] = abz * acx - abx * acz;
    n[2] = abx * acy - aby * acx;
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-20f) {
        n[0] /= len;
        n[1] /= len;
        n[2] /= len;
    } else {
        n[0] = 0.f;
        n[1] = 0.f;
        n[2] = 1.f;
    }
}

/** Quaternion rotating +Z onto normal (xyzw as w,x,y,z). */
void QuatAlignZToNormal(const float n[3], float q[4]) {
    const float z[3] = { 0.f, 0.f, 1.f };
    const float dot = z[0] * n[0] + z[1] * n[1] + z[2] * n[2];
    if (dot > 0.999999f) {
        q[0] = 1.f;
        q[1] = q[2] = q[3] = 0.f;
        return;
    }
    if (dot < -0.999999f) {
        q[0] = 0.f;
        q[1] = 1.f;
        q[2] = 0.f;
        q[3] = 0.f;
        return;
    }
    float ax = z[1] * n[2] - z[2] * n[1];
    float ay = z[2] * n[0] - z[0] * n[2];
    float az = z[0] * n[1] - z[1] * n[0];
    const float alen = std::sqrt(ax * ax + ay * ay + az * az);
    if (alen > 1e-20f) {
        ax /= alen;
        ay /= alen;
        az /= alen;
    }
    const float ang = std::acos(std::max(-1.f, std::min(1.f, dot)));
    const float s = std::sin(ang * 0.5f);
    q[0] = std::cos(ang * 0.5f);
    q[1] = ax * s;
    q[2] = ay * s;
    q[3] = az * s;
}

void PushTri(std::vector<Tri> &tris, const float *a, const float *b, const float *c) {
    Tri t;
    for (int k = 0; k < 3; ++k) {
        t.v0[k] = a[k];
        t.v1[k] = b[k];
        t.v2[k] = c[k];
    }
    t.area = TriArea(t.v0, t.v1, t.v2);
    if (t.area > 1e-12f) {
        tris.push_back(t);
    }
}

void CollectTris(const Mesh &mesh, std::vector<Tri> &tris) {
    tris.clear();
    if (!mesh.mpoly.empty() && !mesh.mloop.empty() && !mesh.mvert.empty()) {
        for (const MPoly &poly : mesh.mpoly) {
            if (poly.totloop < 3) {
                continue;
            }
            const int ls = poly.loopstart;
            if (ls < 0 || ls + poly.totloop > static_cast<int>(mesh.mloop.size())) {
                continue;
            }
            const int i0 = mesh.mloop[static_cast<size_t>(ls)].v;
            if (i0 < 0 || i0 >= static_cast<int>(mesh.mvert.size())) {
                continue;
            }
            for (int li = 1; li + 1 < poly.totloop; ++li) {
                const int i1 = mesh.mloop[static_cast<size_t>(ls + li)].v;
                const int i2 = mesh.mloop[static_cast<size_t>(ls + li + 1)].v;
                if (i1 < 0 || i2 < 0 || i1 >= static_cast<int>(mesh.mvert.size()) ||
                        i2 >= static_cast<int>(mesh.mvert.size())) {
                    continue;
                }
                PushTri(tris, mesh.mvert[static_cast<size_t>(i0)].co,
                        mesh.mvert[static_cast<size_t>(i1)].co,
                        mesh.mvert[static_cast<size_t>(i2)].co);
            }
        }
        return;
    }
    if (!mesh.mface.empty() && !mesh.mvert.empty()) {
        for (const MFace &f : mesh.mface) {
            if (f.v1 < 0 || f.v2 < 0 || f.v3 < 0 ||
                    f.v1 >= static_cast<int>(mesh.mvert.size()) ||
                    f.v2 >= static_cast<int>(mesh.mvert.size()) ||
                    f.v3 >= static_cast<int>(mesh.mvert.size())) {
                continue;
            }
            PushTri(tris, mesh.mvert[static_cast<size_t>(f.v1)].co,
                    mesh.mvert[static_cast<size_t>(f.v2)].co,
                    mesh.mvert[static_cast<size_t>(f.v3)].co);
            if (f.v4 > 0 && f.v4 < static_cast<int>(mesh.mvert.size())) {
                PushTri(tris, mesh.mvert[static_cast<size_t>(f.v1)].co,
                        mesh.mvert[static_cast<size_t>(f.v3)].co,
                        mesh.mvert[static_cast<size_t>(f.v4)].co);
            }
        }
    }
}

int PickTri(const std::vector<float> &cum, float pos) {
    int lo = 0, hi = static_cast<int>(cum.size()) - 1;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (cum[static_cast<size_t>(mid)] >= pos) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo;
}

int PickProto(const std::vector<int> &counts, int particleIndex, uint32_t seed) {
    if (counts.empty()) {
        return 0;
    }
    int total = 0;
    for (int c : counts) {
        total += (c > 0) ? c : 1;
    }
    if (total <= 0) {
        return static_cast<int>(particleIndex % counts.size());
    }
    int slot = static_cast<int>(HashU32(seed + static_cast<uint32_t>(particleIndex) * 17u) %
            static_cast<uint32_t>(total));
    for (size_t i = 0; i < counts.size(); ++i) {
        const int c = (counts[i] > 0) ? counts[i] : 1;
        if (slot < c) {
            return static_cast<int>(i);
        }
        slot -= c;
    }
    return static_cast<int>(counts.size() - 1);
}

} // namespace

bool BlendDistributeParticlesOnMeshFaces(
        const Mesh &mesh,
        const ParticleFaceDistributeParams &params,
        std::vector<float> &positions,
        std::vector<float> &orients,
        std::vector<float> &scales,
        std::vector<int> &protoIndices) {
    positions.clear();
    orients.clear();
    scales.clear();
    protoIndices.clear();

    if (params.totpart <= 0 || params.from != kFromFace) {
        return false;
    }

    std::vector<Tri> tris;
    CollectTris(mesh, tris);
    if (tris.empty()) {
        return false;
    }

    const bool useArea = (params.flag & kFlagEdistr) != 0;
    const bool useTrand = (params.flag & kFlagTrand) != 0;
    std::vector<float> cum(tris.size());
    float tot = 0.f;
    for (size_t i = 0; i < tris.size(); ++i) {
        const float w = useArea ? tris[i].area : 1.f;
        tot += w;
        cum[i] = tot;
    }
    if (tot <= 0.f) {
        return false;
    }
    for (float &c : cum) {
        c /= tot;
    }

    const uint32_t seed = static_cast<uint32_t>(params.seed);
    const int N = params.totpart;
    positions.reserve(static_cast<size_t>(N) * 3u);
    orients.reserve(static_cast<size_t>(N) * 4u);
    scales.reserve(static_cast<size_t>(N));
    protoIndices.reserve(static_cast<size_t>(N));

    const float hairMul = (params.hairLength > 0.f) ? params.hairLength : 1.f;
    const float step = (N < 2) ? 0.5f : (1.f / float(N));

    for (int p = 0; p < N; ++p) {
        float pos;
        if (useTrand) {
            pos = HashFloat(seed, static_cast<uint32_t>(p) + 1u) * cum.back();
        } else {
            pos = (float(p) + 0.5f) * step * cum.back();
        }
        if (pos > cum.back()) {
            pos = cum.back();
        }
        const int ti = PickTri(cum, pos);
        const Tri &t = tris[static_cast<size_t>(ti)];

        float u = HashFloat(seed, static_cast<uint32_t>(p) + 101u);
        float v = HashFloat(seed, static_cast<uint32_t>(p) + 202u);
        // JIT: pull toward face center by jitfac (0 = center, 1 = full random).
        const float jf = std::max(0.f, std::min(1.f, params.jitfac));
        u = 0.33333334f + (u - 0.33333334f) * jf;
        v = 0.33333334f + (v - 0.33333334f) * jf;
        if (u + v > 1.f) {
            u = 1.f - u;
            v = 1.f - v;
        }
        const float w = 1.f - u - v;
        const float x = w * t.v0[0] + u * t.v1[0] + v * t.v2[0];
        const float y = w * t.v0[1] + u * t.v1[1] + v * t.v2[1];
        const float z = w * t.v0[2] + u * t.v1[2] + v * t.v2[2];
        positions.push_back(x);
        positions.push_back(y);
        positions.push_back(z);

        float nrm[3];
        FaceNormal(t.v0, t.v1, t.v2, nrm);
        float q[4];
        QuatAlignZToNormal(nrm, q);
        orients.push_back(q[0]);
        orients.push_back(q[1]);
        orients.push_back(q[2]);
        orients.push_back(q[3]);

        const float r = HashFloat(seed, static_cast<uint32_t>(p) + 303u);
        const float psz = params.size * (1.f - params.randsize * r);
        scales.push_back(psz * hairMul);

        protoIndices.push_back(PickProto(params.prototypeCounts, p, seed));
    }

    return static_cast<int>(scales.size()) == N;
}

} // namespace Blender
} // namespace Assimp

#endif // ASSIMP_BUILD_NO_BLEND_IMPORTER
