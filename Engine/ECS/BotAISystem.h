#pragma once
// IA dei bot della modalita' Vs Bot (5 contro 5). Tre comportamenti, scelti dal ruolo dell'unita' ("ai_role"):
//   Ranged    (tiratore): tiene le distanze; se il bersaglio e' facile spara, se si sente minacciato si
//                         riposiziona (copertura o distanza) e spara quando puo'.
//   Versatile           : valuta dalla sua prospettiva se puo' vincere il corpo a corpo (forze, salute, alleati e
//                         nemici vicini); se si', carica, altrimenti combatte come un tiratore piu' aggressivo.
//   Melee               : carica il bersaglio costi quel che costi, anche prendendo colpi, e mena le mani.
// Ogni bot vede solo cio' che ha davanti (raggio e campo visivo del blueprint, linea di vista libera) e ricorda
// per qualche secondo l'ultima posizione del bersaglio. Spara tramite WeaponTriggerComponent e attacca in
// mischia con lo stesso MeleeStateComponent del giocatore.
#include "Components.h"
#include "../Scene/Scene.h"
#include "../Navigation/NavMeshQuery.h"
#include <glm/glm.hpp>

namespace SunEngine {

    class BotAISystem {
    public:
        // Basi delle due squadre (centro dei punti di partenza): servono per avanzare quando non si vede nessuno.
        void SetTeamBases(const glm::vec3& base0, const glm::vec3& base1) { m_Base[0] = base0; m_Base[1] = base1; }
        void Update(Scene& scene, NavMeshQuery& navQuery, float deltaTime);

    private:
        glm::vec3 m_Base[2]{ glm::vec3(0.0f, 0.0f, 50.0f), glm::vec3(0.0f, 0.0f, -50.0f) };

        void Think(Scene& scene, NavMeshQuery& navQuery, entt::entity self, BotBrainComponent& brain);
        void Move(Scene& scene, NavMeshQuery& navQuery, entt::entity self, BotBrainComponent& brain, float dt);
        void AimAndFire(Scene& scene, entt::entity self, BotBrainComponent& brain, float dt);
        void DoMelee(Scene& scene, entt::entity self, BotBrainComponent& brain, float dt);
        void TryParry(Scene& scene, entt::entity self, BotBrainComponent& brain);
        void Resupply(Scene& scene, entt::entity self, BotBrainComponent& brain, float dt);   // scatola di munizioni
        void TryGrenade(Scene& scene, entt::entity self, BotBrainComponent& brain, float dt); // granate (es. stikkbomb)
        bool ShouldHeal(Scene& scene, entt::entity self, BotBrainComponent& brain, const WeaponBlueprint& item);   // curare o attaccare?
        void TryDeploy(Scene& scene, entt::entity self, BotBrainComponent& brain, float dt);  // mine, barricate, muri con feritoia
    };

}
