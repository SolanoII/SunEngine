#pragma once
// Aiuti fra compagni da far arrivare anche in rete. I sistemi di gioco registrano qui quello che danno agli altri
// (zaino di cibo posato, cassa di munizioni posata o usata, munizioni date a un compagno); GameLayer ogni frame
// raccoglie gli eventi e, in una partita in rete, li manda a tutti.
//
// Regola della rete: ogni computer applica gli aiuti solo a quello che simula lui (il suo giocatore e, se e'
// l'host, i bot e i nemici). Le copie degli altri (NetGhostComponent) non ricevono niente qui: l'aiuto arriva al
// computer del loro proprietario, che lo applica all'unita' vera. Cosi' nessun aiuto vale due volte.
#include <cstdint>
#include <string>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace SunEngine {

    enum class AidKind : std::uint8_t {
        HealZone = 1,     // zaino di cibo posato: cerchio che cura (object = la zona)
        CratePlaced = 2,  // cassa di munizioni posata (object = la cassa)
        CrateTaken = 3,   // dalla cassa e' stato preso un caricatore (netKey, charges = cariche rimaste)
        AmmoRefill = 4,   // scatola di munizioni data a un compagno simulato altrove (target = la sua copia)
    };

    struct AidEvent {
        AidKind kind = AidKind::HealZone;
        entt::entity source = entt::null;   // chi ha dato l'aiuto
        entt::entity target = entt::null;   // chi lo riceve (munizioni)
        entt::entity object = entt::null;   // zona o cassa creata
        std::uint64_t netKey = 0;           // cassa: identificativo in rete (0 = non e' in rete)
        int charges = 0;
        std::string itemId;                 // zaino o cassa: l'oggetto (modello e valori sugli altri computer)
    };

    // Coda unica (static in una funzione inline: una sola istanza in tutto il programma)
    inline std::vector<AidEvent>& AidEventLog() {
        static std::vector<AidEvent> log;
        return log;
    }
    inline void EmitAid(const AidEvent& ev) { AidEventLog().push_back(ev); }

}
