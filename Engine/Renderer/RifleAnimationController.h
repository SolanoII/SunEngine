#pragma once
#include "Animation.h"
#include "Animator.h"
#include "../ECS/RifleLocomotionState.h"
#include <unordered_map>

namespace SunEngine {

    class RifleAnimationController {
    public:
        bool LoadClips(const std::string& animsDir, const std::string& modelPath, SkinnedModel* model) {
            bool allOk = true;

            // Carica la Idle integrata nel file master
            allOk &= LoadClip(RifleAnimState::AimIdle, modelPath, model);

            allOk &= LoadClip(RifleAnimState::WalkStart, animsDir + "start walking.fbx", model);
            allOk &= LoadClip(RifleAnimState::Walk, animsDir + "walking.fbx", model);
            allOk &= LoadClip(RifleAnimState::WalkStop, animsDir + "stop walking.fbx", model);
            allOk &= LoadClip(RifleAnimState::WalkBackStart, animsDir + "start walking backwards.fbx", model);
            allOk &= LoadClip(RifleAnimState::WalkBack, animsDir + "walking backwards.fbx", model);
            allOk &= LoadClip(RifleAnimState::WalkBackStop, animsDir + "walk backwards stop.fbx", model);
            allOk &= LoadClip(RifleAnimState::StrafeLeft, animsDir + "strafe.fbx", model);
            allOk &= LoadClip(RifleAnimState::StrafeRight, animsDir + "strafe (2).fbx", model);
            allOk &= LoadClip(RifleAnimState::Sprint, animsDir + "rifle run.fbx", model);
            allOk &= LoadClip(RifleAnimState::JumpForward, animsDir + "jump forward.fbx", model);
            allOk &= LoadClip(RifleAnimState::JumpBackward, animsDir + "jump backward.fbx", model);
            allOk &= LoadClip(RifleAnimState::Firing, animsDir + "firing rifle.fbx", model);

            auto it = m_Clips.find(m_CurrentState);
            if (it != m_Clips.end()) {
                m_Animator.PlayAnimation(&it->second);
            }

            return allOk;
        }

        void Update(float deltaTime, bool isMoving, bool movingForward, bool movingBackward,
            bool strafingLeft, bool strafingRight, bool isSprinting, bool isAiming) {

            RifleAnimState desired = DetermineState(isMoving, movingForward, movingBackward,
                strafingLeft, strafingRight, isSprinting, isAiming);

            if (desired != m_CurrentState) {
                auto it = m_Clips.find(desired);
                if (it != m_Clips.end()) {
                    m_Animator.PlayAnimation(&it->second);
                    m_CurrentState = desired;
                }
            }

            m_Animator.UpdateAnimation(deltaTime);
        }

        const std::vector<glm::mat4>& GetFinalBoneMatrices() const { return m_Animator.GetFinalBoneMatrices(); }
        RifleAnimState GetCurrentState() const { return m_CurrentState; }

    private:
        bool LoadClip(RifleAnimState state, const std::string& path, SkinnedModel* model) {
            Animation anim;
            if (!anim.LoadFromFile(path, model)) {
                SUN_CORE_ERROR("RifleAnimationController: impossibile caricare {0}", path);
                return false;
            }
            m_Clips[state] = std::move(anim);
            return true;
        }

        RifleAnimState DetermineState(bool isMoving, bool fwd, bool back, bool left, bool right,
            bool sprint, bool aiming) const {

            if (!isMoving) return RifleAnimState::AimIdle;

            if (sprint && fwd) return RifleAnimState::Sprint;

            if (back) return RifleAnimState::WalkBack;
            if (left) return RifleAnimState::StrafeLeft;
            if (right) return RifleAnimState::StrafeRight;
            if (fwd) return RifleAnimState::Walk;

            return RifleAnimState::AimIdle;
        }

        std::unordered_map<RifleAnimState, Animation> m_Clips;
        Animator m_Animator;
        RifleAnimState m_CurrentState = RifleAnimState::AimIdle;
    };
}