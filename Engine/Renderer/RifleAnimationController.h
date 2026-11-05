#pragma once
#include <filesystem>
#include "Animation.h"
#include "Animator.h"
#include "../ECS/RifleLocomotionState.h"
#include <unordered_map>

namespace SunEngine {

    // Stato dei comandi del giocatore che pilota le animazioni.
    struct RifleAnimInput {
        bool isMoving = false;
        bool forward = false, backward = false, left = false, right = false;
        bool sprinting = false;
        bool aiming = false;
        bool firing = false;     // sta sparando (grilletto / cooldown colpo attivo)
        bool vaulting = false;   // scavalcamento ostacolo in corso
        bool crouching = false;  // accovacciato (CTRL)
        bool climbing = false;   // arrampicata lenta (gretchin): base ferma, il movimento lo fa la posa procedurale
        bool shotFired = false;  // un colpo e' partito in QUESTO frame (fronte di salita del cooldown)
        float groundSpeed = 0.0f;     // velocita' orizzontale reale del personaggio (m/s); <=0 = ignora
        float vaultDuration = 0.0f;   // durata dello scavalcamento in corso (s); adatta la clip di salto
    };

    class RifleAnimationController {
    public:
        bool LoadClips(const std::string& animsDir, const std::string& modelPath, SkinnedModel* model) {
            bool allOk = true;

            // Idle: "rifle aiming idle.fbx" (fucile alzato, stesso set delle altre clip). La idle integrata
            // nel file master ha il fucile abbassato: usata come idle faceva "abbassare e rialzare" il
            // fucile a ogni sparo. Resta come ripiego se il file non c'e'.
            {
                const std::string aimIdlePath = animsDir + "rifle aiming idle.fbx";
                std::error_code ec;
                if (std::filesystem::exists(aimIdlePath, ec)) {
                    allOk &= LoadClip(RifleAnimState::AimIdle, aimIdlePath, model);
                }
                else {
                    SUN_CORE_WARN("RifleAnimationController: manca '{0}', uso la idle integrata nel modello (fucile abbassato)", aimIdlePath);
                    allOk &= LoadClip(RifleAnimState::AimIdle, modelPath, model);
                }
            }

            allOk &= LoadClip(RifleAnimState::WalkStart, animsDir + "start walking.fbx", model);
            allOk &= LoadClip(RifleAnimState::Walk, animsDir + "walking.fbx", model);
            allOk &= LoadClip(RifleAnimState::WalkStop, animsDir + "stop walking.fbx", model);
            allOk &= LoadClip(RifleAnimState::WalkBackStart, animsDir + "start walking backwards.fbx", model);
            allOk &= LoadClip(RifleAnimState::WalkBack, animsDir + "walking backwards.fbx", model);
            allOk &= LoadClip(RifleAnimState::WalkBackStop, animsDir + "walk backwards stop.fbx", model);
            allOk &= LoadClip(RifleAnimState::StrafeLeft, animsDir + "strafe (2).fbx", model);
            allOk &= LoadClip(RifleAnimState::StrafeRight, animsDir + "strafe.fbx", model);
            allOk &= LoadClip(RifleAnimState::Sprint, animsDir + "rifle run.fbx", model);
            allOk &= LoadClip(RifleAnimState::JumpForward, animsDir + "jump forward.fbx", model);
            allOk &= LoadClip(RifleAnimState::JumpBackward, animsDir + "jump backward.fbx", model);
            allOk &= LoadClip(RifleAnimState::Firing, animsDir + "firing rifle.fbx", model);
            // Accovacciato: clip facoltative (se mancano il modello animato resta in piedi, le hitbox si abbassano comunque)
            {
                std::error_code ec;
                if (std::filesystem::exists(animsDir + "crouch idle.fbx", ec)) LoadClip(RifleAnimState::CrouchIdle, animsDir + "crouch idle.fbx", model);
                if (std::filesystem::exists(animsDir + "crouch walking.fbx", ec)) LoadClip(RifleAnimState::CrouchWalk, animsDir + "crouch walking.fbx", model);
            }

            auto it = Clips().find(m_CurrentState);
            if (it != Clips().end()) {
                m_Animator.PlayAnimation(&it->second);
            }

            return allOk;
        }

        // Compatibilita': vecchia firma (senza fuoco/vault).
        void Update(float deltaTime, bool isMoving, bool movingForward, bool movingBackward,
            bool strafingLeft, bool strafingRight, bool isSprinting, bool isAiming) {
            RifleAnimInput in;
            in.isMoving = isMoving; in.forward = movingForward; in.backward = movingBackward;
            in.left = strafingLeft; in.right = strafingRight; in.sprinting = isSprinting; in.aiming = isAiming;
            Update(deltaTime, in);
        }

        void Update(float deltaTime, const RifleAnimInput& in) {
            RifleAnimState desired = DetermineState(in);

            // Fuoco: la clip e' UN colpo di rinculo che parte e torna all'idle. Va rilanciata a ogni
            // colpo (non messa in loop, altrimenti si vede "abbassa il fucile / rialza" tra due spari).
            const bool restartFiring = in.shotFired && !in.isMoving && !in.vaulting && !in.climbing
                && Clips().find(RifleAnimState::Firing) != Clips().end();
            if (restartFiring) {
                desired = RifleAnimState::Firing;
            }
            else if (m_CurrentState == RifleAnimState::Firing && !in.isMoving && !in.vaulting
                && !m_Animator.IsFinished()) {
                desired = RifleAnimState::Firing;   // lascia finire il rinculo in corso
            }

            if (desired != m_CurrentState || restartFiring) {
                auto it = Clips().find(desired);
                if (it != Clips().end()) {
                    // crossfade breve: niente "scatti" tra una clip e l'altra
                    float blend = (desired == RifleAnimState::Firing) ? 0.04f : 0.18f;
                    const bool loop = !(desired == RifleAnimState::Firing
                        || desired == RifleAnimState::JumpForward || desired == RifleAnimState::JumpBackward);
                    m_Animator.PlayAnimation(&it->second, blend, loop);
                    m_CurrentState = desired;
                }
            }

            m_Animator.SetPlaybackRate(ComputePlaybackRate(in));
            m_Animator.UpdateAnimation(deltaTime);
        }

        // Un controller per ogni unita' animata (bot, manichini): usa le clip gia' caricate da 'src' (stesso corpo),
        // con un suo Animator (tempo, blend e posa propri). src deve restare vivo finche' si usa questo controller.
        void ShareClipsFrom(RifleAnimationController& src) {
            m_SharedClips = &src.Clips();
            m_ModelScale = src.m_ModelScale; m_MinRate = src.m_MinRate; m_MaxRate = src.m_MaxRate;
            auto it = Clips().find(m_CurrentState);
            if (it != Clips().end()) m_Animator.PlayAnimation(&it->second);
        }

        // Scala usata per convertire la velocita' incisa nelle clip (cm) in m/s
        void SetModelScale(float scale) { m_ModelScale = scale; }
        void SetPlaybackRateLimits(float minRate, float maxRate) { m_MinRate = minRate; m_MaxRate = maxRate; }
        // Ritmo della razza (RaceMotion.h): il gretchin fa passi piu' corti e rapidi, l'ogryn piu' lenti
        void SetStyleRate(float rate) { m_StyleRate = std::clamp(rate, 0.5f, 2.0f); }

        bool GetBoneModelTransform(const std::string& boneName, glm::mat4& out) const {
            return m_Animator.GetBoneModelTransform(boneName, out);
        }

        // Ricalcola le ossa con la posa procedurale aggiornata, senza far avanzare il tempo (es. dopo l'IK delle mani)
        void Reevaluate() { m_Animator.UpdateAnimation(0.0f); }

        const std::vector<glm::mat4>& GetFinalBoneMatrices() const { return m_Animator.GetFinalBoneMatrices(); }
        void SetProceduralPose(const ProceduralPose* pose) { m_Animator.SetProceduralPose(pose); }
        // Clip vere da accovacciato caricate? (altrimenti l'accovacciamento lo fa la posa procedurale)
        bool HasCrouchClips() const { return Clips().count(RifleAnimState::CrouchIdle) > 0; }
        RifleAnimState GetCurrentState() const { return m_CurrentState; }

    private:
        std::unordered_map<RifleAnimState, Animation>& Clips() { return m_SharedClips ? *m_SharedClips : m_OwnClips; }
        const std::unordered_map<RifleAnimState, Animation>& Clips() const { return m_SharedClips ? *m_SharedClips : m_OwnClips; }
        bool LoadClip(RifleAnimState state, const std::string& path, SkinnedModel* model) {
            Animation anim;
            if (!anim.LoadFromFile(path, model)) {
                SUN_CORE_ERROR("RifleAnimationController: impossibile caricare {0}", path);
                return false;
            }
            Clips()[state] = std::move(anim);
            return true;
        }

        // Velocita' di riproduzione: locomozione agganciata alla velocita' reale (meno "pattinamento" dei
        // piedi); scavalcamento adattato alla durata del vault; il resto a velocita' normale.
        float ComputePlaybackRate(const RifleAnimInput& in) const {
            auto it = Clips().find(m_CurrentState);
            if (it == Clips().end()) return 1.0f;
            const Animation& clip = it->second;

            if (m_CurrentState == RifleAnimState::JumpForward || m_CurrentState == RifleAnimState::JumpBackward) {
                if (in.vaultDuration > 0.05f && clip.GetTicksPerSecond() > 0.0f) {
                    float clipSeconds = clip.GetDuration() / clip.GetTicksPerSecond();
                    return std::clamp(clipSeconds / in.vaultDuration, 0.5f, 3.0f);
                }
                return 1.0f;
            }

            const bool locomotion = (m_CurrentState == RifleAnimState::Walk || m_CurrentState == RifleAnimState::WalkBack
                || m_CurrentState == RifleAnimState::CrouchWalk
                || m_CurrentState == RifleAnimState::StrafeLeft || m_CurrentState == RifleAnimState::StrafeRight
                || m_CurrentState == RifleAnimState::Sprint);
            if (!locomotion || in.groundSpeed < 0.2f) return m_StyleRate;

            float clipSpeed = clip.GetRootTravelSpeed() * m_ModelScale;   // m/s inciti nella clip
            if (clipSpeed < 0.2f) return m_StyleRate;
            return std::clamp(in.groundSpeed / clipSpeed * m_StyleRate, m_MinRate, m_MaxRate * std::max(1.0f, m_StyleRate));
        }

        RifleAnimState DetermineState(const RifleAnimInput& in) const {
            // Scavalcamento: clip di salto in avanti (o indietro se si arretra)
            if (in.climbing) return RifleAnimState::AimIdle;
            if (in.vaulting) {
                return (in.backward && !in.forward) ? RifleAnimState::JumpBackward : RifleAnimState::JumpForward;
            }
            if (in.crouching) {
                const RifleAnimState c = in.isMoving ? RifleAnimState::CrouchWalk : RifleAnimState::CrouchIdle;
                if (Clips().count(c)) return c;
                if (Clips().count(RifleAnimState::CrouchIdle)) return RifleAnimState::CrouchIdle;
            }

            // Fermo: idle. Il rinculo di fuoco e' un one-shot gestito in Update (vedi restartFiring); se si
            // muove e spara continua la locomozione (nessun layer solo-busto per ora: servirebbe una
            // maschera sulle ossa).
            if (!in.isMoving) return RifleAnimState::AimIdle;

            if (in.sprinting && in.forward) return RifleAnimState::Sprint;

            if (in.backward) return RifleAnimState::WalkBack;
            if (in.left) return RifleAnimState::StrafeLeft;
            if (in.right) return RifleAnimState::StrafeRight;
            if (in.forward) return RifleAnimState::Walk;

            return RifleAnimState::AimIdle;
        }

        std::unordered_map<RifleAnimState, Animation> m_OwnClips;
        std::unordered_map<RifleAnimState, Animation>* m_SharedClips = nullptr;   // clip di un altro controller (stesso corpo)
        Animator m_Animator;
        RifleAnimState m_CurrentState = RifleAnimState::AimIdle;
        float m_ModelScale = 0.01f;   // modello in centimetri
        float m_MinRate = 0.7f;
        float m_MaxRate = 1.8f;
        float m_StyleRate = 1.0f;
    };
}
