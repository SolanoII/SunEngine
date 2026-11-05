#pragma once

#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace SunEngine {

    // Richieste di effetti visivi dai sistemi di gioco (armi, mischia, esplosioni) al sistema particellare.
    // Il gioco le consuma ogni frame (ParticleSystem); senza grafica (simulazioni) la coda resta limitata.

    // Aspetto del colpo di un'arma: decide lampo alla bocca, proiettile in volo e impatto
    enum class WeaponFx : int {
        Laser = 0,      // lasgun/laspistol: dardo di luce del colore dell'arma
        HotShot,        // laser sovraccarico: piu' spesso e brillante
        LongLas,        // laser da cecchino: raggio lungo che resta un attimo
        Ballistic,      // proiettile: tracciante giallo, scintille e polvere
        Shotgun,        // pallettoni: lampo largo, tanti traccianti corti
        Sniper,         // proiettile pesante: scia d'aria che resta
        Bolt,           // cartuccia a razzo che esplode nel bersaglio
        Plasma,         // globo azzurro-bianco, lento, scoppio di plasma
        Melta,          // raggio di calore arancione con aria tremolante
        Flamer,         // getto di fiamme
        Explosive,      // razzo/granata: scia di fumo, esplosione
        Grav            // onda gravitazionale viola
    };

    enum class VfxKind : int {
        Muzzle = 0,     // lampo alla bocca dell'arma (pos, dir, fx)
        FlamerJet,      // getto del lanciafiamme (pos, dir, scale = gittata)
        Explosion,      // esplosione (pos, scale = raggio)
        MeleeSlash,     // fendente (pos = inizio, pos2 = fine, dir = punto di controllo dell'arco, variant = 0 basso / 1 normale / 2 pesante, scale = durata del colpo, flag = spada a catena)
        MeleeHit,       // colpo in mischia a segno (pos, dir = direzione del colpo, pos2 = piedi del colpito per il sangue a terra, variant come sopra, flag = spada a catena)
        Parry,          // parata riuscita (pos)
        GuardBreak,     // guardia rotta da un pesante (pos)
        Interrupt,      // attacco interrotto (pos)
        Text            // scritta che sale (pos, text, color)
    };

    struct VfxEvent {
        VfxKind kind = VfxKind::Muzzle;
        glm::vec3 pos{ 0.0f }, pos2{ 0.0f, -10000.0f, 0.0f }, dir{ 0.0f, 0.0f, -1.0f };   // pos2.y = -10000: non usato
        glm::vec4 color{ 1.0f };
        float scale = 1.0f;
        int variant = 0;
        bool flag = false;
        WeaponFx fx = WeaponFx::Ballistic;
        std::string text;
    };

    inline std::vector<VfxEvent>& VfxEventQueue() { static std::vector<VfxEvent> q; return q; }
    inline void EmitVfx(const VfxEvent& e) {
        auto& q = VfxEventQueue();
        if (q.size() >= 1024) q.erase(q.begin(), q.begin() + 256);   // nessuno le consuma (simulazioni): butto le piu' vecchie
        q.push_back(e);
    }
    inline void EmitVfxText(const glm::vec3& pos, const std::string& text, const glm::vec4& color) {
        VfxEvent e; e.kind = VfxKind::Text; e.pos = pos; e.text = text; e.color = color; EmitVfx(e);
    }
}
