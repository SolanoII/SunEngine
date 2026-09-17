#pragma once
#ifndef SUN_PHYSICS_SYSTEM_H
#define SUN_PHYSICS_SYSTEM_H

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>

#include "../Scene/Scene.h"
#include "Components.h"
#include "../Physics/JoltUtils.h"

#include <vector>
#include <memory>
#include <thread>
#include <algorithm>
#include <cmath>

namespace SunEngine {

    inline JPH::RVec3 ToJoltRVec3(const glm::vec3& v) {
        return JPH::RVec3(v.x, v.y, v.z);
    }

    inline JPH::Quat Mat4ToJoltQuat(const glm::mat4& m) {
        glm::vec3 c0 = glm::vec3(m[0]);
        glm::vec3 c1 = glm::vec3(m[1]);
        glm::vec3 c2 = glm::vec3(m[2]);

        float l0 = glm::length(c0);
        float l1 = glm::length(c1);
        float l2 = glm::length(c2);

        if (l0 > 1e-6f) c0 /= l0; else c0 = glm::vec3(1.0f, 0.0f, 0.0f);
        if (l1 > 1e-6f) c1 /= l1; else c1 = glm::vec3(0.0f, 1.0f, 0.0f);
        if (l2 > 1e-6f) c2 /= l2; else c2 = glm::vec3(0.0f, 0.0f, 1.0f);

        JPH::Mat44 joltMat(
            JPH::Vec4(c0.x, c0.y, c0.z, 0.0f),
            JPH::Vec4(c1.x, c1.y, c1.z, 0.0f),
            JPH::Vec4(c2.x, c2.y, c2.z, 0.0f),
            JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)
        );

        JPH::Quat q = joltMat.GetRotation().GetQuaternion();
        if (std::isnan(q.GetX()) || std::isnan(q.GetY()) || std::isnan(q.GetZ()) || std::isnan(q.GetW()) || q.LengthSq() < 1e-6f) {
            return JPH::Quat::sIdentity();
        }
        return q.Normalized();
    }

    // Filtro custom per ragdoll: impedisce la self-collision tra gli arti
// dello stesso ragdoll. Jolt chiama CanCollide() solo quando entrambi
// i body condividono lo stesso GroupFilter; se il GroupID coincide,
// sono dello stesso ragdoll → niente collisione interna.
    class RagdollSelfCollisionFilter : public JPH::GroupFilter {
    public:
        virtual bool CanCollide(const JPH::CollisionGroup& inGroup1,
            const JPH::CollisionGroup& inGroup2) const override {
            // Gruppi diversi (ragdoll diversi, o ragdoll vs ambiente) → collidono.
            // Stesso gruppo (stesso ragdoll) → niente self-collision.
            return inGroup1.GetGroupID() != inGroup2.GetGroupID();
        }
    };

    namespace Layers {
        static constexpr JPH::ObjectLayer NON_MOVING = 0;
        static constexpr JPH::ObjectLayer MOVING = 1;
        static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
    }

    namespace BroadPhaseLayers {
        static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
        static constexpr JPH::BroadPhaseLayer MOVING(1);
        static constexpr JPH::uint NUM_LAYERS = 2;
    }

    class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
    public:
        BPLayerInterfaceImpl() {
            mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
            mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
        }

        virtual JPH::uint GetNumBroadPhaseLayers() const override {
            return BroadPhaseLayers::NUM_LAYERS;
        }

        virtual JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
            return mObjectToBroadPhase[inLayer];
        }

#if defined(JPH_PROFILE_ENABLED) || defined(JPH_EXTERNAL_PROFILE)
        virtual const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
            switch ((JPH::BroadPhaseLayer::Type)inLayer) {
            case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
            case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING: return "MOVING";
            default: JPH_ASSERT(false); return "INVALID";
            }
        }
#endif

    private:
        JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
    };

    class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
    public:
        virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
            if (inLayer1 == Layers::NON_MOVING) return inLayer2 == BroadPhaseLayers::MOVING;
            return true;
        }
    };

    class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
    public:
        virtual bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override {
            if (inLayer1 == Layers::NON_MOVING) return inLayer2 == Layers::MOVING;
            return true;
        }
    };

    class PhysicsSystem {
    public:
        PhysicsSystem() = default;

        void Init(Scene& scene) {
            m_SceneRef = &scene;

            JPH::RegisterDefaultAllocator();
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();

            m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
            m_JobSystem = std::make_unique<JPH::JobSystemThreadPool>(
                JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                std::thread::hardware_concurrency() - 1
            );

            m_PhysicsSystem.Init(
                2048, 0, 2048, 2048,
                m_BPLayerInterface,
                m_ObjectVsBPLayerFilter,
                m_ObjectLayerPairFilter
            );

            JPH::BodyInterface& bodyInterface = m_PhysicsSystem.GetBodyInterface();

            JPH::BoxShapeSettings floorShapeSettings(JPH::Vec3(200.0f, 0.5f, 200.0f), 0.0f);
            JPH::ShapeRefC floorShape = floorShapeSettings.Create().Get();

            JPH::BodyCreationSettings floorSettings(
                floorShape,
                JPH::RVec3(0.0f, -0.5f, 0.0f),
                JPH::Quat::sIdentity(),
                JPH::EMotionType::Static,
                Layers::NON_MOVING
            );
            m_FloorBodyID = bodyInterface.CreateAndAddBody(floorSettings, JPH::EActivation::DontActivate);
            m_RagdollGroupFilter = new RagdollSelfCollisionFilter();
            // ------------------------------------------------------------------
            // Observer di distruzione: quando JoltBodyComponent o RagdollComponent
            // vengono rimossi (esplicitamente o per distruzione dell'entita'),
            // libera automaticamente i body Jolt associati. Elimina la necessita'
            // di chiamare DestroyJoltBody manualmente prima di DestroyEntity.
            // ------------------------------------------------------------------
            scene.GetRegistry().on_destroy<JoltBodyComponent>()
                .connect<&PhysicsSystem::OnJoltBodyDestroyed>(*this);
            scene.GetRegistry().on_destroy<RagdollComponent>()
                .connect<&PhysicsSystem::OnRagdollDestroyed>(*this);
        }

        ~PhysicsSystem() {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }

        void RefreshBodies(Scene& scene) {
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem.GetBodyInterface();

            auto unregisteredView = scene.GetRegistry().view<TransformComponent, RigidBodyComponent, ColliderComponent>(
                entt::exclude<JoltBodyComponent, RagdollComponent>
            );

            for (auto entity : unregisteredView) {
                auto& transform = unregisteredView.get<TransformComponent>(entity);
                auto& rb = unregisteredView.get<RigidBodyComponent>(entity);
                auto& col = unregisteredView.get<ColliderComponent>(entity);

                glm::vec3 safeHalfExtents = glm::max(col.halfExtents * transform.Scale, glm::vec3(0.01f));
                JPH::Vec3 halfExtents = ToJoltVec3(safeHalfExtents);
                JPH::ShapeRefC boxShape = new JPH::BoxShape(halfExtents, 0.0f);

                // Determina il tipo di movimento in base a massa e gravità
                bool isMoving = (rb.mass > 0.0f) || rb.useGravity;
                JPH::EMotionType motionType = isMoving ? (rb.useGravity ? JPH::EMotionType::Dynamic : JPH::EMotionType::Kinematic) : JPH::EMotionType::Static;
                JPH::ObjectLayer layer = isMoving ? Layers::MOVING : Layers::NON_MOVING;

                glm::mat4 rotMat = glm::rotate(glm::mat4(1.0f), glm::radians(transform.Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
                rotMat = glm::rotate(rotMat, glm::radians(transform.Rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
                rotMat = glm::rotate(rotMat, glm::radians(transform.Rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

                JPH::Quat qRot = Mat4ToJoltQuat(rotMat);
                glm::vec3 scaledOffset = col.localCenterOffset * transform.Scale;
                glm::vec3 worldCenterOffset = ToGlmQuat(qRot) * scaledOffset;

                JPH::BodyCreationSettings settings(
                    boxShape,
                    ToJoltRVec3(transform.Position + worldCenterOffset),
                    qRot,
                    motionType,
                    layer
                );

                if (motionType == JPH::EMotionType::Dynamic) {
                    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
                    settings.mFriction = 0.7f;
                    settings.mRestitution = 0.1f;
                }

                JPH::BodyID bodyID = bodyInterface.CreateAndAddBody(settings, JPH::EActivation::Activate);
                scene.GetRegistry().emplace<JoltBodyComponent>(entity, bodyID);
            }
        }

        void Update(Scene& scene, float deltaTime) {
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem.GetBodyInterface();

            // 1. Registra nuovi corpi fisici per entità vive
            RefreshBodies(scene);

            // 2. Avanzamento simulazione fisica
            m_PhysicsSystem.Update(deltaTime, 1, m_TempAllocator.get(), m_JobSystem.get());

            // 3. Sincronizzazione posizioni entità standard
            auto joltView = scene.GetRegistry().view<TransformComponent, JoltBodyComponent, ColliderComponent>(entt::exclude<RagdollComponent>);
            for (auto entity : joltView) {
                auto& transform = joltView.get<TransformComponent>(entity);
                auto& joltComp = joltView.get<JoltBodyComponent>(entity);
                auto& col = joltView.get<ColliderComponent>(entity);

                if (!joltComp.bodyID.IsInvalid() && bodyInterface.IsAdded(joltComp.bodyID) && bodyInterface.IsActive(joltComp.bodyID)) {
                    if (bodyInterface.GetMotionType(joltComp.bodyID) == JPH::EMotionType::Dynamic) {
                        JPH::RVec3 pos = bodyInterface.GetPosition(joltComp.bodyID);
                        JPH::Quat rot = bodyInterface.GetRotation(joltComp.bodyID);

                        glm::quat glmRot = ToGlmQuat(rot);
                        glm::vec3 scaledOffset = col.localCenterOffset * transform.Scale;
                        glm::vec3 worldCenterOffset = glmRot * scaledOffset;

                        transform.Position = ToGlmVec3(pos) - worldCenterOffset;
                        transform.Rotation = glm::degrees(glm::eulerAngles(glmRot));
                    }
                }
            }

            // 4. Sincronizzazione matrici 3D reali per ogni arto del Ragdoll
            auto ragdollView = scene.GetRegistry().view<RagdollComponent, SkeletonComponent>();
            for (auto entity : ragdollView) {
                auto& ragdoll = ragdollView.get<RagdollComponent>(entity);
                auto& skel = ragdollView.get<SkeletonComponent>(entity);

                ragdoll.worldTransforms.resize(ragdoll.bodyIDs.size());

                for (size_t i = 0; i < ragdoll.bodyIDs.size() && i < skel.data.blocks.size(); ++i) {
                    const JPH::BodyID& bID = ragdoll.bodyIDs[i];
                    if (!bID.IsInvalid() && bodyInterface.IsAdded(bID)) {
                        JPH::RVec3 bPos = bodyInterface.GetPosition(bID);
                        JPH::Quat bRot = bodyInterface.GetRotation(bID);

                        glm::vec3 worldPos = ToGlmVec3(bPos);
                        glm::quat worldRot = ToGlmQuat(bRot);
                        glm::vec3 blockSize = skel.data.blocks[i].size * skel.scale;

                        ragdoll.worldTransforms[i] = glm::translate(glm::mat4(1.0f), worldPos) *
                            glm::mat4_cast(worldRot) *
                            glm::scale(glm::mat4(1.0f), blockSize);
                    }
                }
            }
        }

        void SetBodyPosition(Scene& scene, entt::entity entity, const glm::vec3& position) {
            if (scene.GetRegistry().all_of<JoltBodyComponent, ColliderComponent>(entity)) {
                auto& joltComp = scene.GetRegistry().get<JoltBodyComponent>(entity);
                auto& col = scene.GetRegistry().get<ColliderComponent>(entity);
                auto& transform = scene.GetRegistry().get<TransformComponent>(entity);

                if (!joltComp.bodyID.IsInvalid()) {
                    JPH::BodyInterface& bodyInterface = m_PhysicsSystem.GetBodyInterface();
                    if (bodyInterface.IsAdded(joltComp.bodyID)) {
                        glm::mat4 rotMat = glm::rotate(glm::mat4(1.0f), glm::radians(transform.Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
                        rotMat = glm::rotate(rotMat, glm::radians(transform.Rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
                        rotMat = glm::rotate(rotMat, glm::radians(transform.Rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

                        JPH::Quat qRot = Mat4ToJoltQuat(rotMat);

                        glm::vec3 scaledOffset = col.localCenterOffset * transform.Scale;
                        glm::vec3 worldCenterOffset = ToGlmQuat(qRot) * scaledOffset;

                        bodyInterface.SetPosition(joltComp.bodyID, ToJoltRVec3(position + worldCenterOffset), JPH::EActivation::Activate);
                    }
                }
            }
        }

        void CreateRagdollFromSkeleton(Scene& scene, entt::entity entity, const SkeletonData& skeleton, float skeletonScale, const glm::vec3& rootPos, const glm::vec3& initialImpulse, int hitBlockIdx = -1) {
            DestroyJoltBody(scene, entity);

            JPH::BodyInterface& bodyInterface = m_PhysicsSystem.GetBodyInterface();
            std::vector<JPH::Body*> createdBodies;

            uint32_t ragdollGroupID = m_NextRagdollGroupID++;
            RagdollComponent ragdollComp{};

            auto physTransforms = skeleton.ComputePhysicsTransforms(rootPos, glm::vec3(0.0f), skeletonScale);

            for (size_t i = 0; i < skeleton.blocks.size(); ++i) {
                const auto& block = skeleton.blocks[i];

                glm::vec3 safeSize = glm::max(block.size * skeletonScale, glm::vec3(0.02f));
                JPH::Vec3 halfExtents = ToJoltVec3(safeSize * 0.5f);
                JPH::ShapeRefC blockShape = new JPH::BoxShape(halfExtents, 0.005f);

                glm::vec3 pos_i = physTransforms[i].blockCenterPos;
                if (std::isnan(pos_i.x) || std::isnan(pos_i.y) || std::isnan(pos_i.z)) {
                    pos_i = rootPos;
                }

                glm::quat rot_i = physTransforms[i].rotation;
                JPH::Quat joltRot = JPH::Quat(rot_i.x, rot_i.y, rot_i.z, rot_i.w);
                if (std::isnan(joltRot.GetX()) || std::isnan(joltRot.GetY()) || std::isnan(joltRot.GetZ()) || std::isnan(joltRot.GetW()) || joltRot.LengthSq() < 1e-6f) {
                    joltRot = JPH::Quat::sIdentity();
                }
                else {
                    joltRot = joltRot.Normalized();
                }

                JPH::BodyCreationSettings settings(
                    blockShape,
                    ToJoltRVec3(pos_i),
                    joltRot,
                    JPH::EMotionType::Dynamic,
                    Layers::MOVING
                );

                float mass = 5.0f;
                if (block.name == "Torso") mass = 32.0f;
                else if (block.name == "Head") mass = 4.5f;
                else if (block.name.find("Arm") != std::string::npos) mass = 3.5f;
                else if (block.name.find("Leg") != std::string::npos) mass = 7.0f;

                settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                settings.mMassPropertiesOverride.mMass = mass;

                settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
                settings.mFriction = 0.85f;
                settings.mRestitution = 0.05f;
                settings.mLinearDamping = 0.15f;
                settings.mAngularDamping = 0.75f;

                settings.mCollisionGroup = JPH::CollisionGroup(m_RagdollGroupFilter, ragdollGroupID, 0);

                JPH::Body* body = bodyInterface.CreateBody(settings);
                bodyInterface.AddBody(body->GetID(), JPH::EActivation::Activate);

                if (glm::length(initialImpulse) > 0.0f && !std::isnan(initialImpulse.x)) {
                    float impulseMult = (static_cast<int>(i) == hitBlockIdx) ? 1.6f : 0.35f;
                    bodyInterface.AddImpulse(body->GetID(), ToJoltVec3(initialImpulse * impulseMult));
                }

                createdBodies.push_back(body);
                ragdollComp.bodyIDs.push_back(body->GetID());
            }

            for (size_t i = 0; i < skeleton.blocks.size(); ++i) {
                const auto& block = skeleton.blocks[i];

                if (block.parentIndex >= 0 && block.parentIndex < static_cast<int>(i)) {
                    JPH::Body* parentBody = createdBodies[block.parentIndex];
                    JPH::Body* childBody = createdBodies[i];

                    JPH::RVec3 jointWorldPos = ToJoltRVec3(physTransforms[i].jointWorldPos);

                    JPH::SwingTwistConstraintSettings constraintSettings;
                    constraintSettings.mSpace = JPH::EConstraintSpace::WorldSpace;
                    constraintSettings.mPosition1 = jointWorldPos;
                    constraintSettings.mPosition2 = jointWorldPos;

                    constraintSettings.mTwistAxis1 = JPH::Vec3::sAxisZ();
                    constraintSettings.mPlaneAxis1 = JPH::Vec3::sAxisY();
                    constraintSettings.mTwistAxis2 = JPH::Vec3::sAxisZ();
                    constraintSettings.mPlaneAxis2 = JPH::Vec3::sAxisY();

                    constraintSettings.mTwistMinAngle = glm::radians(-35.0f);
                    constraintSettings.mTwistMaxAngle = glm::radians(35.0f);
                    constraintSettings.mSwingType = JPH::ESwingType::Cone;
                    constraintSettings.mNormalHalfConeAngle = glm::radians(45.0f);
                    constraintSettings.mPlaneHalfConeAngle = glm::radians(30.0f);

                    JPH::Constraint* constraint = constraintSettings.Create(*parentBody, *childBody);
                    m_PhysicsSystem.AddConstraint(constraint);
                }
            }

            scene.GetRegistry().emplace_or_replace<RagdollComponent>(entity, ragdollComp);
        }

        void KillUnit(Scene& scene, entt::entity entity, const glm::vec3& killImpulse = glm::vec3(0.0f), int hitBlockIdx = -1) {
            auto& registry = scene.GetRegistry();
            if (registry.all_of<SkeletonComponent, TransformComponent>(entity)) {
                auto& skel = registry.get<SkeletonComponent>(entity);
                auto& trans = registry.get<TransformComponent>(entity);

                CreateRagdollFromSkeleton(scene, entity, skel.data, skel.scale, trans.Position, killImpulse, hitBlockIdx);
                registry.emplace_or_replace<CorpseComponent>(entity);
            }
        }

        void ApplyImpulse(Scene& scene, entt::entity entity, const glm::vec3& impulse, int hitBlockIdx = -1) {
            auto& registry = scene.GetRegistry();
            JPH::BodyInterface& bodyInterface = m_PhysicsSystem.GetBodyInterface();

            if (registry.all_of<RagdollComponent>(entity)) {
                auto& ragdoll = registry.get<RagdollComponent>(entity);
                if (hitBlockIdx >= 0 && hitBlockIdx < static_cast<int>(ragdoll.bodyIDs.size())) {
                    const JPH::BodyID& bID = ragdoll.bodyIDs[hitBlockIdx];
                    if (!bID.IsInvalid() && bodyInterface.IsAdded(bID)) {
                        bodyInterface.ActivateBody(bID);
                        bodyInterface.AddImpulse(bID, ToJoltVec3(impulse));
                        return;
                    }
                }
                if (!ragdoll.bodyIDs.empty() && !ragdoll.bodyIDs[0].IsInvalid() && bodyInterface.IsAdded(ragdoll.bodyIDs[0])) {
                    bodyInterface.ActivateBody(ragdoll.bodyIDs[0]);
                    bodyInterface.AddImpulse(ragdoll.bodyIDs[0], ToJoltVec3(impulse));
                }
            }
        }

        void DestroyJoltBody(Scene& scene, entt::entity entity) {
            // Rimuovere i componenti triggera gli observer OnJoltBodyDestroyed /
            // OnRagdollDestroyed, che si occupano del cleanup reale dei body Jolt.
            // La funzione resta idempotente: chiamarla su un'entita' senza
            // componenti fisici e' un no-op.
            auto& registry = scene.GetRegistry();
            if (registry.all_of<JoltBodyComponent>(entity)) {
                registry.remove<JoltBodyComponent>(entity);
            }
            if (registry.all_of<RagdollComponent>(entity)) {
                registry.remove<RagdollComponent>(entity);
            }
        }

        JPH::PhysicsSystem& GetJoltSystem() { return m_PhysicsSystem; }

        private:
            Scene* m_SceneRef = nullptr;

            void OnJoltBodyDestroyed(entt::registry& reg, entt::entity entity) {
                if (!reg.all_of<JoltBodyComponent>(entity)) return;

                const auto& joltComp = reg.get<JoltBodyComponent>(entity);
                if (joltComp.bodyID.IsInvalid()) return;

                JPH::BodyInterface& bodyInterface = m_PhysicsSystem.GetBodyInterface();
                if (bodyInterface.IsAdded(joltComp.bodyID)) {
                    bodyInterface.RemoveBody(joltComp.bodyID);
                    bodyInterface.DestroyBody(joltComp.bodyID);
                }
            }

            void OnRagdollDestroyed(entt::registry& reg, entt::entity entity) {
                if (!reg.all_of<RagdollComponent>(entity)) return;

                const auto& ragdoll = reg.get<RagdollComponent>(entity);
                JPH::BodyInterface& bodyInterface = m_PhysicsSystem.GetBodyInterface();

                for (const JPH::BodyID& bID : ragdoll.bodyIDs) {
                    if (!bID.IsInvalid() && bodyInterface.IsAdded(bID)) {
                        bodyInterface.RemoveBody(bID);
                        bodyInterface.DestroyBody(bID);
                    }
                }
            }

            std::unique_ptr<JPH::TempAllocatorImpl> m_TempAllocator;
            std::unique_ptr<JPH::JobSystemThreadPool> m_JobSystem;

        BPLayerInterfaceImpl m_BPLayerInterface;
        ObjectVsBroadPhaseLayerFilterImpl m_ObjectVsBPLayerFilter;
        ObjectLayerPairFilterImpl m_ObjectLayerPairFilter;

        JPH::PhysicsSystem m_PhysicsSystem;
        JPH::BodyID m_FloorBodyID;

        JPH::Ref<JPH::GroupFilter> m_RagdollGroupFilter;
        uint32_t m_NextRagdollGroupID = 1;
    };
}

#endif // SUN_PHYSICS_SYSTEM_H