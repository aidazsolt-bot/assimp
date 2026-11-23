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
        /** DOF focus empties / objects (TRS Action -> aiNodeAnim), filled from Camera DNA. */
        std::vector<const Object *> camera_focus_objects;
        /** Per aiCamera name: extra JSON fragment for SceneSidecarJson.cameras[] (DOF bind). */
        std::unordered_map<std::string, std::string> cameraSidecarExtras;
        /** Armature objects (bone hierarchy + pose Action -> aiNodeAnim). */
        std::vector<const Object *> armature_objects;
        /**
         * Map Armature Object name -> bone rest name used as aiNode::mName
         * (unique: "ArmatureObj|BoneName") for pose channel targeting.
         */
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> armatureBoneNodeNames;

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
        std::vector<PhysicsInvNode> physicsOceanNodes;
        std::vector<PhysicsInvNode> physicsDynamicPaintNodes;
        std::vector<PhysicsInvNode> physicsSolidifyNodes;
        /** Modifier-prop / pointer anim channels for aiAnimationSidecar (C5). */
        struct AnimPointerChannel {
            std::string actionName;
            std::string nodeName;
            std::string rnaPath;
            int arrayIndex{ 0 };
            std::vector<float> times;
            std::vector<float> values;
        };
        std::vector<AnimPointerChannel> animPointerChannels;
        /** DNA Drivers (AnimData.drivers / ChannelDriver) for Animation sidecar. */
        struct DriverTargetEmit {
            std::string idCode;
            std::string idName;
            std::string rnaPath;
            std::string bone;
            short transChan{ 0 };
            short flag{ 0 };
            short options{ 0 };
            int idtype{ 0 };
            int contextProperty{ 0 };
            float fallbackValue{ 0.f };
            bool hasFallback{ false };
            bool hasOptions{ false };
            bool hasContextProperty{ false };
            bool hasRotationMode{ false };
            char rotationMode{ 0 };
        };
        struct DriverVarEmit {
            std::string name;
            short type{ 0 };
            short flag{ 0 };
            std::vector<DriverTargetEmit> targets;
        };
        struct DriverChannelEmit {
            std::string ownerName;
            std::string ownerIdCode; // "OB" when from Object.adt
            std::string rnaPath;
            int arrayIndex{ 0 };
            int type{ 0 };
            int flag{ 0 };
            float influence{ 0.f };
            bool hasInfluence{ false };
            std::string expression;
            std::vector<DriverVarEmit> variables;
            std::vector<float> keyTimes; // optional generator FCurve keys
            std::vector<float> keyValues;
        };
        std::vector<DriverChannelEmit> driverChannels;
        /** Scene volumes[] entries (Principled Volume bind). */
        std::vector<std::string> volumeJsonEntries;
        /** C13 PointInstancer pending (particle collection/object instances). */
        struct PointInstancerPending {
            std::string emitterName;
            std::string systemName;
            std::string settingsName;
            std::string collectionName;
            std::string instanceObjectName;
            short ren_as{ 0 };
            short from{ 0 };
            short distr{ 0 };
            int flag{ 0 };
            int totpart{ 0 };
            int seed{ 0 };
            float size{ 0.f };
            float randsize{ 0.f };
            float jitfac{ 0.f };
            short phystype{ 0 };
            short draw_as{ 0 };
            int draw{ 0 };
            int userjit{ 0 };
            int grid_res{ 10 };
            float grid_rand{ 0.f };
            float randlife{ 0.f };
            float randlength{ 0.f };
            short hair_step{ 5 };
            /** DNA normfac; emit hair_length = normfac*4 when hasHairLength. */
            float normfac{ 1.f };
            bool hasHairLength{ false };
            bool hasPhystype{ false };
            bool hasDrawAs{ false };
            bool hasDraw{ false };
            bool hasUserjit{ false };
            bool hasGridRes{ false };
            bool hasGridRand{ false };
            bool hasRandlife{ false };
            bool hasRandlength{ false };
            bool hasHairStep{ false };
            std::vector<std::string> prototypeNames;
            std::vector<int> prototypeCounts;
            std::vector<int> prototypeIndices;
            std::vector<float> positions; // xyz * N when DNA ParticleData or mesh-face eval
            std::vector<float> orients; // wxyz * N
            std::vector<float> scales; // uniform * N
            std::vector<int> protoIndices; // per-instance prototype slot
            /** mesh_face_eval Presence: object-space face area / normalized emit weight per instance. */
            std::vector<float> faceArea;
            std::vector<float> emitWeight;
            bool arraysResolved{ false };
            /** "dna_particles" | "mesh_face_eval" when arraysResolved. */
            std::string arraysSource;
            /**
             * Host 0ai contract (Presence when arraysResolved):
             * positionsSpace / scalesSpace = "emitterLocal".
             * positions[] = emitter mesh-local (Host: parent under emitter node for T).
             * scales[] = isotropic instance scale in the same emitter-local frame as
             * Blender size*path_len before pamat*obmat - NOT world, NOT Host skinScale.
             * mesh_face_eval: scales[i] = size*(1-randsize*r)*hair_length (path_len proxy).
             * dna_particles: scales[i] = ParticleData.size (or settings size).
             * size / hair_length / normfac stay separate Presence keys.
             * Host must not apply emitter Object.scale a second time onto scales[] when
             * parenting (Blender already folds Object.scale into pamat*obmat / eval path).
             */
            std::string positionsSpace;
            std::string scalesSpace;
            /** Optional Presence: how scales[] were formed (mesh_face_eval only). */
            std::string scalesMeaning;
        };
        std::vector<PointInstancerPending> pointInstancers;
        /** Material indices with resolved Principled Volume (parallel to materials_raw). */
        std::vector<char> materialHasPrincipledVolume;
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
            /** Scene mesh index for mesh/convex/trimesh colliders (-1 = implicit shape). */
            int meshIdx{ -1 };
            bool convexHull{ false };
            /** RBO_FLAG_DISABLED -> KHR trigger volume (not a simulated body). */
            bool isTrigger{ false };
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
            float spring_stiffness_x{ 0.f }, spring_stiffness_y{ 0.f }, spring_stiffness_z{ 0.f };
            float spring_stiffness_ang_x{ 0.f }, spring_stiffness_ang_y{ 0.f }, spring_stiffness_ang_z{ 0.f };
            float spring_damping_x{ 0.f }, spring_damping_y{ 0.f }, spring_damping_z{ 0.f };
            float spring_damping_ang_x{ 0.f }, spring_damping_ang_y{ 0.f }, spring_damping_ang_z{ 0.f };
            float motor_lin_target_velocity{ 0.f };
            float motor_ang_target_velocity{ 0.f };
            float motor_lin_max_impulse{ 0.f };
            float motor_ang_max_impulse{ 0.f };
        };
        std::vector<KhrJointPending> khrJoints;

        std::vector<std::string> skippedCategories;

        /** Compact BLEND_scene.worldEnv JSON object (Presence - omit when empty). */
        std::string worldEnvJson;
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
