#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
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
        bool climbing = false;   // arrampicata lenta (pighcib): base ferma, il movimento lo fa la posa procedurale
        bool shotFired = false;  // un colpo e' partito in QUESTO frame (fronte di salita del cooldown)
        float groundSpeed = 0.0f;     // velocita' orizzontale reale del personaggio (m/s); <=0 = ignora
        float vaultDuration = 0.0f;   // durata dello scavalcamento in corso (s); adatta la clip di salto
        // Azione con una sua clip (mischia, parata, urlo...): -1 = nessuna. actionTime 0..1 = punto dell'azione
        // (la clip segue il gioco fotogramma per fotogramma); < 0 = la clip va per conto suo (stordito, carica).
        int action = -1;              // RifleAnimState come intero
        float actionTime = -1.0f;
    };

    class RifleAnimationController {
    public:
        // fallbackDir: dove cercare le clip che mancano in animsDir (es. un artista che ha rifatto solo la camminata).
        // Ogni clip puo' essere .fbx, .glb o .gltf.
        //
        // Insieme di animazioni di una razza: Animations/<cartella>/animset.json (facoltativo) dice quale file usare
        // per ogni movimento ("clips": {"walk": "walking", "melee_heavy": "colpo pesante"...}), da quale cartella
        // prendere quelli che mancano ("fallback": "Human") e lo stile procedurale della razza ("style", RaceMotion.h).
        // Le azioni (melee_light, melee_normal, melee_heavy, parry, stunned, roar, charge) sono facoltative: se il
        // file c'e' la clip prende il posto della posa procedurale.
        bool LoadClips(const std::string& animsDir, const std::string& modelPath, SkinnedModel* model, std::string fallbackDir = std::string()) {
            bool allOk = true;
            m_Set = AnimSet::Load(animsDir);
            if (fallbackDir.empty() && !m_Set.fallback.empty()) {
                std::filesystem::path parent = std::filesystem::path(animsDir).parent_path().parent_path();
                fallbackDir = (parent / m_Set.fallback).string() + "/";
            }
            auto clip = [&](const char* key) {
                const std::string name = m_Set.Name(key);
                std::error_code ec;
                for (const std::string& dir : { animsDir, fallbackDir }) {
                    if (dir.empty()) continue;
                    for (const char* ext : { ".fbx", ".glb", ".gltf" }) {
                        const std::string p = dir + name + ext;
                        if (std::filesystem::exists(p, ec)) return p;
                    }
                }
                return animsDir + name + ".fbx";   // non c'e': il messaggio d'errore indica il nome atteso
            };
            auto optional = [&](RifleAnimState st, const char* key) {
                std::error_code ec;
                const std::string p = clip(key);
                if (std::filesystem::exists(p, ec)) LoadClip(st, p, model);
            };

            // Idle: "rifle aiming idle.fbx" (fucile alzato, stesso set delle altre clip). La idle integrata
            // nel file master ha il fucile abbassato: usata come idle faceva "abbassare e rialzare" il
            // fucile a ogni sparo. Resta come ripiego se il file non c'e'.
            {
                const std::string aimIdlePath = clip("aim_idle");
                std::error_code ec;
                if (std::filesystem::exists(aimIdlePath, ec)) {
                    allOk &= LoadClip(RifleAnimState::AimIdle, aimIdlePath, model);
                }
                else {
                    SUN_CORE_WARN("RifleAnimationController: manca '{0}', uso la idle integrata nel modello (fucile abbassato)", aimIdlePath);
                    allOk &= LoadClip(RifleAnimState::AimIdle, modelPath, model);
                }
            }

            allOk &= LoadClip(RifleAnimState::WalkStart, clip("walk_start"), model);
            allOk &= LoadClip(RifleAnimState::Walk, clip("walk"), model);
            allOk &= LoadClip(RifleAnimState::WalkStop, clip("walk_stop"), model);
            allOk &= LoadClip(RifleAnimState::WalkBackStart, clip("walk_back_start"), model);
            allOk &= LoadClip(RifleAnimState::WalkBack, clip("walk_back"), model);
            allOk &= LoadClip(RifleAnimState::WalkBackStop, clip("walk_back_stop"), model);
            allOk &= LoadClip(RifleAnimState::StrafeLeft, clip("strafe_left"), model);
            allOk &= LoadClip(RifleAnimState::StrafeRight, clip("strafe_right"), model);
            allOk &= LoadClip(RifleAnimState::Sprint, clip("sprint"), model);
            allOk &= LoadClip(RifleAnimState::JumpForward, clip("jump_forward"), model);
            allOk &= LoadClip(RifleAnimState::JumpBackward, clip("jump_backward"), model);
            allOk &= LoadClip(RifleAnimState::Firing, clip("firing"), model);
            // Accovacciato: clip facoltative (se mancano il modello animato resta in piedi, le hitbox si abbassano comunque)
            {
                std::error_code ec;
                if (std::filesystem::exists(clip("crouch_idle"), ec)) LoadClip(RifleAnimState::CrouchIdle, clip("crouch_idle"), model);
                if (std::filesystem::exists(clip("crouch_walk"), ec)) LoadClip(RifleAnimState::CrouchWalk, clip("crouch_walk"), model);
            }

            optional(RifleAnimState::MeleeLight, "melee_light");
            optional(RifleAnimState::MeleeNormal, "melee_normal");
            optional(RifleAnimState::MeleeHeavy, "melee_heavy");
            optional(RifleAnimState::Parry, "parry");
            optional(RifleAnimState::Stunned, "stunned");
            optional(RifleAnimState::Roar, "roar");
            optional(RifleAnimState::Charge, "charge");

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
            // Azione con la sua clip: ha la precedenza su tutto (mischia, parata, urlo, stordito, carica)
            if (in.action >= 0 && Clips().count(static_cast<RifleAnimState>(in.action))) {
                const RifleAnimState st = static_cast<RifleAnimState>(in.action);
                const bool looped = st == RifleAnimState::Stunned || st == RifleAnimState::Charge;
                // si riparte quando cambia l'azione o quando il punto dell'azione torna indietro (colpo nuovo)
                const bool restart = st != m_CurrentState || (in.actionTime >= 0.0f && in.actionTime + 0.2f < m_LastActionTime);
                if (restart) { m_Animator.PlayAnimation(&Clips().at(st), 0.08f, looped); m_CurrentState = st; m_StateAge = 0.0f; }
                m_LastActionTime = in.actionTime;
                if (in.actionTime >= 0.0f) { m_Animator.SetNormalizedTime(in.actionTime); m_Animator.SetPlaybackRate(0.0f); }
                else m_Animator.SetPlaybackRate(m_StyleRate);
                m_Animator.UpdateAnimation(deltaTime);
                return;
            }
            m_LastActionTime = -1.0f;
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

            // Una clip appena partita resta almeno un quarto di secondo: un comando che balla (avanti / di lato /
            // avanti) non fa piu' sfarfallare il corpo fra tre pose. Fuoco e salti passano subito.
            m_StateAge += deltaTime;
            const bool urgent = restartFiring || desired == RifleAnimState::Firing
                || desired == RifleAnimState::JumpForward || desired == RifleAnimState::JumpBackward
                || m_CurrentState == RifleAnimState::Firing;
            if (desired != m_CurrentState && !urgent && m_StateAge < 0.25f) desired = m_CurrentState;

            if (desired != m_CurrentState || restartFiring) {
                auto it = Clips().find(desired);
                if (it != Clips().end()) {
                    // Dissolvenza: corta per il rinculo, morbida fra un'andatura e l'altra, un po' piu' lunga per
                    // partire e fermarsi (il peso del corpo non cambia verso in un decimo di secondo)
                    const bool toIdle = desired == RifleAnimState::AimIdle || desired == RifleAnimState::CrouchIdle;
                    const bool fromIdle = m_CurrentState == RifleAnimState::AimIdle || m_CurrentState == RifleAnimState::CrouchIdle;
                    float blend = (desired == RifleAnimState::Firing) ? 0.04f : ((toIdle || fromIdle) ? 0.30f : 0.26f);
                    if (m_CurrentState == RifleAnimState::Firing) blend = 0.16f;
                    m_StateAge = 0.0f;
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
            m_Set = src.m_Set;
            m_ModelScale = src.m_ModelScale; m_MinRate = src.m_MinRate; m_MaxRate = src.m_MaxRate;
            auto it = Clips().find(m_CurrentState);
            if (it != Clips().end()) m_Animator.PlayAnimation(&it->second);
        }

        // Scala usata per convertire la velocita' incisa nelle clip (cm) in m/s
        void SetModelScale(float scale) { m_ModelScale = scale; }
        void SetPlaybackRateLimits(float minRate, float maxRate) { m_MinRate = minRate; m_MaxRate = maxRate; }
        // Ritmo della razza (RaceMotion.h): il pighcib fa passi piu' corti e rapidi, l'ogre piu' lenti
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
        // Clip di un movimento caricata? (le azioni con la clip non usano la posa procedurale)
        bool HasClip(RifleAnimState s) const { return Clips().count(s) > 0; }
        // Stile della razza scritto nell'animset.json ("style"): vuoto = quello del codice
        const nlohmann::json& StyleOverrides() const { return m_Set.style; }
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

        // animset.json della cartella delle animazioni
        struct AnimSet {
            std::unordered_map<std::string, std::string> clips;   // movimento -> nome del file (senza estensione)
            std::string fallback;                                // cartella di ripiego (accanto a questa)
            nlohmann::json style;                                // ritocchi allo stile della razza
            std::string Name(const char* key) const {
                auto it = clips.find(key);
                if (it != clips.end() && !it->second.empty()) return it->second;
                static const std::unordered_map<std::string, std::string> def = {
                    { "aim_idle", "rifle aiming idle" }, { "walk_start", "start walking" }, { "walk", "walking" }, { "walk_stop", "stop walking" },
                    { "walk_back_start", "start walking backwards" }, { "walk_back", "walking backwards" }, { "walk_back_stop", "walk backwards stop" },
                    { "strafe_left", "strafe (2)" }, { "strafe_right", "strafe" }, { "sprint", "rifle run" }, { "jump_forward", "jump forward" },
                    { "jump_backward", "jump backward" }, { "firing", "firing rifle" }, { "crouch_idle", "crouch idle" }, { "crouch_walk", "crouch walking" },
                    { "melee_light", "melee light" }, { "melee_normal", "melee normal" }, { "melee_heavy", "melee heavy" }, { "parry", "parry" },
                    { "stunned", "stunned" }, { "roar", "roar" }, { "charge", "charge" } };
                auto d = def.find(key);
                return d != def.end() ? d->second : std::string(key);
            }
            static AnimSet Load(const std::string& dir) {
                AnimSet s;
                std::ifstream f(dir + "animset.json");
                if (!f) return s;
                try {
                    nlohmann::json j; f >> j;
                    if (j.contains("clips") && j["clips"].is_object())
                        for (auto it = j["clips"].begin(); it != j["clips"].end(); ++it) if (it.value().is_string()) s.clips[it.key()] = it.value().get<std::string>();
                    s.fallback = j.value("fallback", std::string());
                    if (j.contains("style") && j["style"].is_object()) s.style = j["style"];
                }
                catch (const std::exception& e) { SUN_CORE_WARN("animset.json in {0} non valido: {1}", dir, e.what()); }
                return s;
            }
        };
        AnimSet m_Set;
        float m_LastActionTime = -1.0f;

        std::unordered_map<RifleAnimState, Animation> m_OwnClips;
        std::unordered_map<RifleAnimState, Animation>* m_SharedClips = nullptr;   // clip di un altro controller (stesso corpo)
        Animator m_Animator;
        RifleAnimState m_CurrentState = RifleAnimState::AimIdle;
        float m_ModelScale = 0.01f;   // modello in centimetri
        float m_MinRate = 0.7f;
        float m_MaxRate = 1.8f;
        float m_StyleRate = 1.0f;
        float m_StateAge = 1.0f;      // secondi dalla partenza della clip in corso
    };
}
