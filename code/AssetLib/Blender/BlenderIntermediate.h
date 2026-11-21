/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

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

----------------------------------------------------------------------
*/

/** @file  BlenderIntermediate.h
 *  @brief Internal utility structures for the BlenderLoader. It also serves
 *    as master include file for the whole (internal) Blender subsystem.
 */
#ifndef INCLUDED_AI_BLEND_INTERMEDIATE_H
#define INCLUDED_AI_BLEND_INTERMEDIATE_H

#include "BlenderLoader.h"
#include "BlenderDNA.h"
#include "BlenderScene.h"
#include <deque>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <assimp/material.h>

struct aiTexture;

namespace Assimp {
namespace Blender {

    // --------------------------------------------------------------------
    /** Mini smart-array to avoid pulling in even more boost stuff. usable with vector and deque */
    // --------------------------------------------------------------------
    template <template <typename,typename> class TCLASS, typename T>
    struct TempArray    {
        typedef TCLASS< T*,std::allocator<T*> > mywrap;

        TempArray() = default;

        ~TempArray () {
            for(T* elem : arr) {
                delete elem;
            }
        }

        void dismiss() {
            arr.clear();
        }

        mywrap* operator -> () {
            return &arr;
        }

        operator mywrap& () {
            return arr;
        }

        operator const mywrap& () const {
            return arr;
        }

        mywrap& get () {
            return arr;
        }

        const mywrap& get () const {
            return arr;
        }

        T* operator[] (size_t idx) const {
            return arr[idx];
        }

        T*& operator[] (size_t idx) {
            return arr[idx];
        }

    private:
        // no copy semantics
        void operator= (const TempArray&)  {
        }

        TempArray(const TempArray& /*arr*/) {
        }

    private:
        mywrap arr;
    };

#if defined(_MSC_VER) && _MSC_VER < 1900
#   pragma warning(disable:4351)
#endif

    // As counter-intuitive as it may seem, a comparator must return false for equal values.
    // The C++ standard defines and expects this behavior: true if lhs < rhs, false otherwise.
    struct ObjectCompare {
        bool operator() (const Object* left, const Object* right) const {
            return ::strncmp(left->id.name, right->id.name, strlen( left->id.name ) ) < 0;
        }
    };

    // When keeping objects in sets, sort them by their name.
    typedef std::set<const Object*, ObjectCompare> ObjectSet;

    // --------------------------------------------------------------------
    /** ConversionData acts as intermediate storage location for
     *  the various ConvertXXX routines in BlenderImporter.*/
    // --------------------------------------------------------------------
    struct ConversionData
    {
        ConversionData(const FileDatabase& db)
            : sentinel_cnt()
            , next_texture()
            , db(db)
        {}

        // As counter-intuitive as it may seem, a comparator must return false for equal values.
        // The C++ standard defines and expects this behavior: true if lhs < rhs, false otherwise.
        struct ObjectCompare {
            bool operator() (const Object* left, const Object* right) const {
                return ::strncmp( left->id.name, right->id.name, strlen( left->id.name ) ) < 0;
            }
        };

        ObjectSet objects;

        TempArray <std::vector, aiMesh> meshes;
        TempArray <std::vector, aiCamera> cameras;
        TempArray <std::vector, aiLight> lights;
        TempArray <std::vector, aiMaterial> materials;
        TempArray <std::vector, aiTexture> textures;

        // set of all materials referenced by at least one mesh in the scene
        std::deque< std::shared_ptr< Material > > materials_raw;

        // counter to name sentinel textures inserted as substitutes for procedural textures.
        unsigned int sentinel_cnt;

        // next texture ID for each texture type, respectively
        unsigned int next_texture[aiTextureType_UNKNOWN+1];

        // Dedupe embedded aiTexture by Blender Image* (packedfiles / packedfile).
        std::unordered_map<const Image *, unsigned int> embeddedImageTexIndex;

        // Camera objects (for Action -> aiNodeAnim), filled during ConvertNode.
        std::vector<const Object *> camera_objects;

        // original file data
        const FileDatabase& db;

        // Parallel to meshes[]: CustomData/primvar JSON for aiMeshSidecar (S1).
        std::vector<std::string> meshSidecarJson;

        // Parallel to materials[]: S3 BLEND_shader (+ optional principled) JSON bodies.
        std::vector<std::string> materialSidecarJson;

        // Physics modifier inventory (S5/S6) - filled while walking objects.
        // Each entry is a complete JSON object without `index`; ConvertBlendFile
        // injects hierarchy DFS index (same order as Host skinNodes flatten).
        struct PhysicsInvNode {
            std::string name;
            std::string json;
        };
        std::vector<PhysicsInvNode> physicsSoftNodes;
        std::vector<PhysicsInvNode> physicsClothNodes;
        std::vector<PhysicsInvNode> physicsFluidNodes;
        std::vector<PhysicsInvNode> physicsSmokeNodes;
        std::vector<PhysicsInvNode> physicsRigidNodes;
        std::vector<PhysicsInvNode> physicsParticleNodes;
        std::vector<PhysicsInvNode> physicsJointNodes;
        std::vector<PhysicsInvNode> physicsBlastNodes;
        std::ostringstream khrShapes;
        std::ostringstream khrMaterials;
        bool khrShapeFirst{ true };
        bool khrMatFirst{ true };
        int khrShapeCount{ 0 };
        int khrMatCount{ 0 };

        /** Pending KHR rigid body node (index / filters / joints filled at emit). */
        struct KhrBodyPending {
            std::string name;
            std::string motionType; // empty => collider-only (compound child)
            float mass{ 0.f };
            int shapeIdx{ -1 };
            int matIdx{ -1 };
            int colGroups{ 0 };
        };
        std::vector<KhrBodyPending> khrBodies;

        /** Pending RigidBodyCon -> KHR physicsJoints + node joint attachment. */
        struct KhrJointPending {
            std::string constraintName;
            std::string ob1Name;
            std::string ob2Name;
            short type{ 0 };
            int flag{ 0 };
            float breaking_threshold{ 0.f };
            float limit_lin_x_lower{ 0.f }, limit_lin_x_upper{ 0.f };
            float limit_lin_y_lower{ 0.f }, limit_lin_y_upper{ 0.f };
            float limit_lin_z_lower{ 0.f }, limit_lin_z_upper{ 0.f };
            float limit_ang_x_lower{ 0.f }, limit_ang_x_upper{ 0.f };
            float limit_ang_y_lower{ 0.f }, limit_ang_y_upper{ 0.f };
            float limit_ang_z_lower{ 0.f }, limit_ang_z_upper{ 0.f };
            float motor_lin_target_velocity{ 0.f };
            float motor_ang_target_velocity{ 0.f };
            float motor_lin_max_impulse{ 0.f };
            float motor_ang_max_impulse{ 0.f };
        };
        std::vector<KhrJointPending> khrJoints;

        std::vector<std::string> skippedCategories;
    };
#if defined(_MSC_VER) && _MSC_VER < 1900
#   pragma warning(default:4351)
#endif

// ------------------------------------------------------------------------------------------------
inline const char* GetTextureTypeDisplayString(Tex::Type t)
{
    switch (t)  {
    case Tex::Type_CLOUDS       :  return  "Clouds";
    case Tex::Type_WOOD         :  return  "Wood";
    case Tex::Type_MARBLE       :  return  "Marble";
    case Tex::Type_MAGIC        :  return  "Magic";
    case Tex::Type_BLEND        :  return  "Blend";
    case Tex::Type_STUCCI       :  return  "Stucci";
    case Tex::Type_NOISE        :  return  "Noise";
    case Tex::Type_PLUGIN       :  return  "Plugin";
    case Tex::Type_MUSGRAVE     :  return  "Musgrave";
    case Tex::Type_VORONOI      :  return  "Voronoi";
    case Tex::Type_DISTNOISE    :  return  "DistortedNoise";
    case Tex::Type_ENVMAP       :  return  "EnvMap";
    case Tex::Type_IMAGE        :  return  "Image";
    default:
        break;
    }
    return "<Unknown>";
}

} // ! Blender
} // ! Assimp

#endif // ! INCLUDED_AI_BLEND_INTERMEDIATE_H
