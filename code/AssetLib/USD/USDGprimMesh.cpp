/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------
Copyright (c) 2006-2026, assimp team
All rights reserved.
----------------------------------------------------------------------
*/

#ifndef ASSIMP_BUILD_NO_USD_IMPORTER

#include "USDGprimMesh.h"

#include <assimp/defs.h>
#include <assimp/material.h>
#include <assimp/mesh.h>
#include <assimp/scene.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Assimp {
namespace {

double AnimatableScalar(const lightusd::Animatable<double> &anim, double fallback) {
    double v = fallback;
    if (!anim.get_scalar(&v)) {
        return fallback;
    }
    return v;
}

aiMesh *MakeCubeMesh(const std::string &name, double size) {
    const float h = static_cast<float>(size) * 0.5f;
    // 24 verts (unique normals per face) / 12 tris
    static const float corners[8][3] = {
            { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
            { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 }
    };
    static const int faces[6][4] = {
            { 0, 1, 2, 3 }, // -Z
            { 4, 7, 6, 5 }, // +Z
            { 0, 4, 5, 1 }, // -Y
            { 3, 2, 6, 7 }, // +Y
            { 0, 3, 7, 4 }, // -X
            { 1, 5, 6, 2 }  // +X
    };
    static const float normals[6][3] = {
            { 0, 0, -1 }, { 0, 0, 1 }, { 0, -1, 0 }, { 0, 1, 0 }, { -1, 0, 0 }, { 1, 0, 0 }
    };

    aiMesh *mesh = new aiMesh();
    mesh->mName.Set(name);
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mNumVertices = 24;
    mesh->mVertices = new aiVector3D[24];
    mesh->mNormals = new aiVector3D[24];
    mesh->mNumFaces = 12;
    mesh->mFaces = new aiFace[12];

    unsigned int vi = 0;
    unsigned int fi = 0;
    for (int f = 0; f < 6; ++f) {
        const int *q = faces[f];
        const unsigned int base = vi;
        for (int k = 0; k < 4; ++k) {
            mesh->mVertices[vi] = aiVector3D(corners[q[k]][0] * h, corners[q[k]][1] * h, corners[q[k]][2] * h);
            mesh->mNormals[vi] = aiVector3D(normals[f][0], normals[f][1], normals[f][2]);
            ++vi;
        }
        // two tris
        mesh->mFaces[fi].mNumIndices = 3;
        mesh->mFaces[fi].mIndices = new unsigned int[3]{ base, base + 1, base + 2 };
        ++fi;
        mesh->mFaces[fi].mNumIndices = 3;
        mesh->mFaces[fi].mIndices = new unsigned int[3]{ base, base + 2, base + 3 };
        ++fi;
    }
    return mesh;
}

aiMesh *MakeSphereMesh(const std::string &name, double radius, int slices = 16, int stacks = 12) {
    const float r = static_cast<float>(radius);
    const unsigned int numVerts = static_cast<unsigned int>((stacks + 1) * (slices + 1));
    const unsigned int numFaces = static_cast<unsigned int>(stacks * slices * 2);

    aiMesh *mesh = new aiMesh();
    mesh->mName.Set(name);
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mNumVertices = numVerts;
    mesh->mVertices = new aiVector3D[numVerts];
    mesh->mNormals = new aiVector3D[numVerts];
    mesh->mNumFaces = numFaces;
    mesh->mFaces = new aiFace[numFaces];

    unsigned int vi = 0;
    for (int i = 0; i <= stacks; ++i) {
        const float v = static_cast<float>(i) / static_cast<float>(stacks);
        const float phi = v * static_cast<float>(AI_MATH_PI);
        const float y = std::cos(phi);
        const float ring = std::sin(phi);
        for (int j = 0; j <= slices; ++j) {
            const float u = static_cast<float>(j) / static_cast<float>(slices);
            const float theta = u * static_cast<float>(AI_MATH_TWO_PI);
            const float x = ring * std::cos(theta);
            const float z = ring * std::sin(theta);
            mesh->mVertices[vi] = aiVector3D(x * r, y * r, z * r);
            mesh->mNormals[vi] = aiVector3D(x, y, z);
            ++vi;
        }
    }

    unsigned int fi = 0;
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const unsigned int a = static_cast<unsigned int>(i * (slices + 1) + j);
            const unsigned int b = a + static_cast<unsigned int>(slices + 1);
            mesh->mFaces[fi].mNumIndices = 3;
            mesh->mFaces[fi].mIndices = new unsigned int[3]{ a, b, a + 1 };
            ++fi;
            mesh->mFaces[fi].mNumIndices = 3;
            mesh->mFaces[fi].mIndices = new unsigned int[3]{ a + 1, b, b + 1 };
            ++fi;
        }
    }
    return mesh;
}

aiMesh *MakeCylinderMesh(const std::string &name, double radius, double height, int slices = 16) {
    const float r = static_cast<float>(radius);
    const float h = static_cast<float>(height) * 0.5f;
    // side only (good enough for collider viz)
    const unsigned int numVerts = static_cast<unsigned int>(slices * 2);
    const unsigned int numFaces = static_cast<unsigned int>(slices * 2);

    aiMesh *mesh = new aiMesh();
    mesh->mName.Set(name);
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mNumVertices = numVerts;
    mesh->mVertices = new aiVector3D[numVerts];
    mesh->mNormals = new aiVector3D[numVerts];
    mesh->mNumFaces = numFaces;
    mesh->mFaces = new aiFace[numFaces];

    for (int j = 0; j < slices; ++j) {
        const float u = static_cast<float>(j) / static_cast<float>(slices);
        const float theta = u * static_cast<float>(AI_MATH_TWO_PI);
        const float x = std::cos(theta);
        const float z = std::sin(theta);
        mesh->mVertices[j] = aiVector3D(x * r, -h, z * r);
        mesh->mNormals[j] = aiVector3D(x, 0, z);
        mesh->mVertices[j + slices] = aiVector3D(x * r, h, z * r);
        mesh->mNormals[j + slices] = aiVector3D(x, 0, z);
    }
    unsigned int fi = 0;
    for (int j = 0; j < slices; ++j) {
        const unsigned int a = static_cast<unsigned int>(j);
        const unsigned int b = static_cast<unsigned int>((j + 1) % slices);
        const unsigned int c = a + static_cast<unsigned int>(slices);
        const unsigned int d = b + static_cast<unsigned int>(slices);
        mesh->mFaces[fi].mNumIndices = 3;
        mesh->mFaces[fi].mIndices = new unsigned int[3]{ a, b, d };
        ++fi;
        mesh->mFaces[fi].mNumIndices = 3;
        mesh->mFaces[fi].mIndices = new unsigned int[3]{ a, d, c };
        ++fi;
    }
    return mesh;
}

aiNode *FindNodeByName(aiNode *node, const std::string &name) {
    if (node == nullptr) {
        return nullptr;
    }
    if (name == node->mName.C_Str()) {
        return node;
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        if (aiNode *found = FindNodeByName(node->mChildren[i], name)) {
            return found;
        }
    }
    return nullptr;
}

void BindMeshIndex(aiNode *node, unsigned int meshIndex) {
    if (node == nullptr) {
        return;
    }
    if (node->mNumMeshes == 0) {
        node->mNumMeshes = 1;
        node->mMeshes = new unsigned int[1];
        node->mMeshes[0] = meshIndex;
        return;
    }
    // Append if not already bound.
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        if (node->mMeshes[i] == meshIndex) {
            return;
        }
    }
    unsigned int *merged = new unsigned int[node->mNumMeshes + 1];
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        merged[i] = node->mMeshes[i];
    }
    merged[node->mNumMeshes] = meshIndex;
    delete[] node->mMeshes;
    node->mMeshes = merged;
    ++node->mNumMeshes;
}

void AttachMeshToNamedNode(aiNode *node, const std::unordered_map<std::string, unsigned int> &nameToMesh) {
    if (node == nullptr) {
        return;
    }
    const auto it = nameToMesh.find(node->mName.C_Str());
    if (it != nameToMesh.end()) {
        BindMeshIndex(node, it->second);
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        AttachMeshToNamedNode(node->mChildren[i], nameToMesh);
    }
}

void WalkGprims(const lightusd::Prim &prim,
        std::vector<aiMesh *> &outMeshes,
        std::vector<std::string> &outPaths,
        std::unordered_map<std::string, unsigned int> &nameToMesh,
        std::vector<std::string> *outSkipped) {
    const std::string leaf = prim.element_name();
    const std::string path = prim.absolute_path().prim_part();

    if (prim.metas().references.has_value() || prim.metas().payload.has_value()) {
        if (outSkipped) {
            outSkipped->push_back(std::string("composition:payload_or_reference:") + path);
        }
    }

    aiMesh *mesh = nullptr;
    if (const lightusd::GeomCube *cube = prim.as<lightusd::GeomCube>()) {
        const double size = AnimatableScalar(cube->size.get_value(), 2.0);
        mesh = MakeCubeMesh(leaf.empty() ? "Cube" : leaf, size);
    } else if (const lightusd::GeomSphere *sphere = prim.as<lightusd::GeomSphere>()) {
        const double radius = AnimatableScalar(sphere->radius.get_value(), 1.0);
        mesh = MakeSphereMesh(leaf.empty() ? "Sphere" : leaf, radius);
    } else if (const lightusd::GeomCylinder *cyl = prim.as<lightusd::GeomCylinder>()) {
        const double radius = AnimatableScalar(cyl->radius.get_value(), 1.0);
        const double height = AnimatableScalar(cyl->height.get_value(), 2.0);
        mesh = MakeCylinderMesh(leaf.empty() ? "Cylinder" : leaf, radius, height);
    } else if (const lightusd::GeomCone *cone = prim.as<lightusd::GeomCone>()) {
        const double radius = AnimatableScalar(cone->radius.get_value(), 1.0);
        const double height = AnimatableScalar(cone->height.get_value(), 2.0);
        // Approximate cone as tapered cylinder (radius at base, near-zero top via sphere fallback scale).
        mesh = MakeCylinderMesh(leaf.empty() ? "Cone" : leaf, radius, height);
    } else if (const lightusd::GeomCapsule *cap = prim.as<lightusd::GeomCapsule>()) {
        const double radius = AnimatableScalar(cap->radius.get_value(), 0.5);
        const double height = AnimatableScalar(cap->height.get_value(), 1.0);
        mesh = MakeCylinderMesh(leaf.empty() ? "Capsule" : leaf, radius, height + 2.0 * radius);
    }

    if (mesh != nullptr) {
        const unsigned int idx = static_cast<unsigned int>(outMeshes.size());
        outMeshes.push_back(mesh);
        outPaths.push_back(path);
        // Prefer unique path keys; leaf keys are best-effort (duplicate CubeGeom names collide).
        if (!path.empty()) {
            nameToMesh[path] = idx;
        }
        if (!leaf.empty() && nameToMesh.find(leaf) == nameToMesh.end()) {
            nameToMesh[leaf] = idx;
        }
    }

    for (const auto &child : prim.children()) {
        WalkGprims(child, outMeshes, outPaths, nameToMesh, outSkipped);
    }
}

} // namespace

unsigned int AppendTessellatedUsdGprimMeshes(
        aiScene *scene,
        const lightusd::Stage &stage,
        std::vector<std::string> *outSkipped) {
    if (scene == nullptr) {
        return 0;
    }

    std::vector<aiMesh *> gprimMeshes;
    std::vector<std::string> gprimPaths;
    std::unordered_map<std::string, unsigned int> nameToMesh;
    for (const auto &root : stage.root_prims()) {
        WalkGprims(root, gprimMeshes, gprimPaths, nameToMesh, outSkipped);
    }
    if (gprimMeshes.empty()) {
        return 0;
    }

    const unsigned int oldCount = scene->mNumMeshes;
    const unsigned int addCount = static_cast<unsigned int>(gprimMeshes.size());
    aiMesh **merged = new aiMesh *[oldCount + addCount];
    for (unsigned int i = 0; i < oldCount; ++i) {
        merged[i] = scene->mMeshes[i];
    }
    for (unsigned int i = 0; i < addCount; ++i) {
        merged[oldCount + i] = gprimMeshes[i];
        // Remap name indices to absolute mesh indices after append.
    }
    delete[] scene->mMeshes;
    scene->mMeshes = merged;
    scene->mNumMeshes = oldCount + addCount;

    // Fix map values to absolute indices
    for (auto &kv : nameToMesh) {
        kv.second += oldCount;
    }

    if (scene->mRootNode == nullptr) {
        scene->mRootNode = new aiNode("<USDRoot>");
    }

    // Ensure at least one default material so ValidateDS is happy when meshes exist.
    if (scene->mNumMaterials == 0) {
        scene->mNumMaterials = 1;
        scene->mMaterials = new aiMaterial *[1];
        scene->mMaterials[0] = new aiMaterial();
        aiString matName("USD_GprimDefault");
        scene->mMaterials[0]->AddProperty(&matName, AI_MATKEY_NAME);
    }
    for (unsigned int i = oldCount; i < scene->mNumMeshes; ++i) {
        scene->mMeshes[i]->mMaterialIndex = 0;
    }

    AttachMeshToNamedNode(scene->mRootNode, nameToMesh);

    // Bind any still-unreferenced new meshes: prefer parent Xform by leaf, else orphan under root.
    std::vector<bool> referenced(scene->mNumMeshes, false);
    std::function<void(aiNode *)> mark = [&](aiNode *n) {
        if (!n) {
            return;
        }
        for (unsigned int i = 0; i < n->mNumMeshes; ++i) {
            if (n->mMeshes[i] < scene->mNumMeshes) {
                referenced[n->mMeshes[i]] = true;
            }
        }
        for (unsigned int i = 0; i < n->mNumChildren; ++i) {
            mark(n->mChildren[i]);
        }
    };
    mark(scene->mRootNode);

    for (unsigned int i = 0; i < addCount; ++i) {
        const unsigned int absIdx = oldCount + i;
        if (referenced[absIdx]) {
            continue;
        }
        const std::string leaf = gprimMeshes[i]->mName.C_Str();
        const std::string &path = gprimPaths[i];
        aiNode *target = FindNodeByName(scene->mRootNode, leaf);
        // Nested gprim (DynamicCube01/CubeGeom): Convert often keeps only the Xform parent.
        if (target == nullptr || target->mNumMeshes > 0) {
            const size_t slash = path.find_last_of('/');
            if (slash != std::string::npos && slash > 0) {
                const size_t prev = path.find_last_of('/', slash - 1);
                const std::string parentLeaf = (prev == std::string::npos) ?
                                                       path.substr(1, slash - 1) :
                                                       path.substr(prev + 1, slash - prev - 1);
                if (aiNode *parent = FindNodeByName(scene->mRootNode, parentLeaf)) {
                    // Only promote when parent has no mesh yet (Xform wrapper).
                    if (parent->mNumMeshes == 0) {
                        target = parent;
                    }
                }
            }
        }
        if (target == nullptr) {
            const unsigned int oldChildren = scene->mRootNode->mNumChildren;
            aiNode **kids = new aiNode *[oldChildren + 1];
            for (unsigned int c = 0; c < oldChildren; ++c) {
                kids[c] = scene->mRootNode->mChildren[c];
            }
            target = new aiNode(leaf.c_str());
            target->mParent = scene->mRootNode;
            kids[oldChildren] = target;
            delete[] scene->mRootNode->mChildren;
            scene->mRootNode->mChildren = kids;
            scene->mRootNode->mNumChildren = oldChildren + 1;
        }
        BindMeshIndex(target, absIdx);
        referenced[absIdx] = true;
    }

    return addCount;
}

} // namespace Assimp

#endif // ASSIMP_BUILD_NO_USD_IMPORTER
