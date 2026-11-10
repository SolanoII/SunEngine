#pragma once
// Sensazione di gioco: tutto quello che fa "sentire" uno sparo o un colpo.
//   - suoni a strati per tipo di arma (vicino + coda lontana), proiettili che fischiano accanto, impatti per
//     superficie (carne, armatura, muro, terra, energia), esplosioni, mischia, conferma di colpo e di uccisione
//   - scossoni della telecamera (trauma), pugno di FOV, fermo-immagine sui colpi di mischia
//   - lampi di luce di spari ed esplosioni, bossoli che cadono, segni dei colpi sui muri
//   - sussulto di chi viene colpito
// Ogni corpo ha il suo peso (FeelProfile): un gretchin trema per tutto e fa passetti leggeri, un Astartes e'
// una roccia con passi che si sentono nel petto e armi piu' gravi.
// Consuma la coda FeelEventQueue() riempita dai sistemi del motore (armi, mischia, esplosivi, movimento).
#include "Engine/ECS/VfxEvents.h"
#include "Engine/Audio/SfxBank.h"
#include "Engine/Renderer/ProceduralPose.h"
#include "Engine/Scene/Scene.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SunEngine {

    class DeferredRenderer;

    // Peso di un corpo. L'indice e' quello di GameLayer::AnimatedBodyKind (Human, Ork, Ogryn, Neophyte, Astartes, Gretchin).
    struct FeelProfile {
        float shakeTaken = 1.0f;   // quanto ti scuote quello che ti succede attorno (colpi presi, esplosioni, fischi)
        float shakeGiven = 1.0f;   // quanto ti scuote la tua stessa arma
        float stepPitch = 1.0f;    // tono dei passi
        float stepVolume = 1.0f;
        float stepHeavy = 0.0f;    // strato di tonfo pesante sotto ogni passo (stivali corazzati)
        float stepShake = 0.0f;    // i tuoi passi muovono la telecamera
        float gunPitch = 1.0f;     // tono della tua arma (piu' basso = piu' grossa)
        float hitStop = 1.0f;      // durata del fermo-immagine quando colpisci in mischia
        float flinch = 1.0f;       // quanto il corpo sussulta quando viene colpito
        float power = 1.0f;        // volume/pugno di FOV delle uccisioni
    };

    class GameFeel {
    public:
        using BodyFn = std::function<int(entt::entity)>;   // corpo di un'unita' (indice di AnimatedBodyKind)

        void Init(ma_engine* engine, ma_sound_group* sfxGroup, const std::string& audioDir);
        void Shutdown();   // prima di distruggere gruppi e motore audio
        void Reset();      // nuova partita: via bossoli, segni, luci

        // Una volta per fotogramma. realDt = tempo vero (non rallentato dal fermo-immagine).
        void Update(float realDt, bool paused, Scene& scene, entt::entity player, const glm::vec3& camPos, const BodyFn& bodyOf);

        // 1 normale, quasi 0 durante il fermo-immagine
        float TimeScale() const { return m_HitStop > 0.0f ? 0.06f : 1.0f; }

        struct CameraKick { float pitch = 0.0f, yaw = 0.0f, fov = 0.0f; glm::vec3 offset{ 0.0f }; };
        CameraKick GetCameraKick() const { return m_Kick; }

        void SubmitLights(DeferredRenderer& renderer) const;   // dopo ClearLights
        void Draw(DeferredRenderer& renderer) const;           // bossoli e segni, nel passaggio G-Buffer
        void ApplyFlinch(entt::entity e, ProceduralPose& pose) const;

        float shakeScale = 1.0f;     // opzione: scossoni della telecamera (0 = spenti)
        bool hitStopEnabled = true;  // opzione: fermo-immagine

        static const FeelProfile& ProfileOf(int body);

    private:
        struct Pending { FeelEvent e; float t; };
        struct FlashLight { glm::vec3 pos, color; float range, intensity, life, maxLife; };
        struct Casing { glm::vec3 pos, vel; float groundY, spin, spinVel, yaw, life, size; bool rested, sounded; };
        struct Mark { glm::vec3 pos; int axis; float size, age, heat; glm::vec3 glow; };
        struct Flinch { float t = 0.0f, strength = 0.0f; glm::vec2 local{ 0.0f, 1.0f }; };

        void Handle(const FeelEvent& e, Scene& scene, entt::entity player, const BodyFn& bodyOf);
        void OnShot(const FeelEvent& e, Scene& scene, entt::entity player, const BodyFn& bodyOf);
        void OnImpact(const FeelEvent& e, Scene& scene, entt::entity player, const BodyFn& bodyOf);
        void OnExplosion(const glm::vec3& pos, float radius);
        void OnMeleeHit(const FeelEvent& e, Scene& scene, entt::entity player, const BodyFn& bodyOf);
        void OnStep(const FeelEvent& e, Scene& scene, entt::entity player, const BodyFn& bodyOf);
        void AddTrauma(float amount) { m_Trauma = (std::min)(1.0f, m_Trauma + amount * shakeScale); }
        void AddLight(const glm::vec3& pos, const glm::vec3& color, float range, float intensity, float life);
        void AddFlinch(Scene& scene, entt::entity target, const glm::vec3& dir, float strength);
        void HitStop(float seconds) { if (hitStopEnabled) m_HitStop = (std::min)(0.16f, (std::max)(m_HitStop, seconds)); }

        SfxBank m_Bank;
        bool m_AudioReady = false;
        // indici dei suoni
        int m_ShotSnd[12] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        int m_TailLight = -1, m_TailHeavy = -1, m_TailEnergy = -1;
        int m_Whiz = -1, m_WhizSnap = -1, m_WhizEnergy = -1;
        int m_ImpFlesh = -1, m_ImpArmor = -1, m_ImpHard = -1, m_ImpDirt = -1, m_ImpEnergy = -1;
        int m_Expl = -1, m_ExplFar = -1, m_ExplSmall = -1;
        int m_SwingLight = -1, m_SwingMid = -1, m_SwingHeavy = -1, m_MeleeHit = -1, m_MeleeHeavy = -1, m_Chain = -1, m_Parry = -1, m_GuardBreak = -1;
        int m_HitTick = -1, m_HitArmorTick = -1, m_Kill = -1, m_Hurt = -1;
        int m_CasingSnd = -1, m_CasingHeavy = -1;
        int m_StepSnd[4] = { -1, -1, -1, -1 }, m_StepHeavy = -1, m_StepLight = -1, m_Land = -1;

        std::vector<Pending> m_Pending;
        std::vector<FlashLight> m_Lights;
        std::vector<Casing> m_Casings;
        std::vector<Mark> m_Marks;
        std::unordered_map<entt::entity, Flinch> m_Flinch;
        std::unordered_map<std::uint32_t, float> m_LoopCooldown;   // armi a getto continuo: un suono ogni tot

        glm::vec3 m_CamPos{ 0.0f }, m_PlayerPos{ 0.0f };
        int m_PlayerBody = 0;
        float m_Time = 0.0f;
        float m_Trauma = 0.0f, m_FovPunch = 0.0f, m_KickPitch = 0.0f, m_KickYaw = 0.0f, m_Dip = 0.0f;
        float m_HitStop = 0.0f;
        float m_TickCooldown = 0.0f, m_WhizCooldown = 0.0f, m_ImpactBudget = 0.0f, m_CasingSndCooldown = 0.0f;
        float m_PrevPlayerY = 0.0f, m_PrevVy = 0.0f; bool m_HavePrev = false;
        CameraKick m_Kick;
    };
}
