#pragma once
// Tipi di danno.
//
// Ogni arma divide il suo danno fra uno o piu' tipi ("damage_types": {"pierce": 60, "blast": 40}, in percentuale).
// Ogni armatura ha una difesa per tipo ("resistances": {"pierce": 10, "slash": 15}, in percentuale): la parte del
// colpo di quel tipo che arriva sul pezzo di armatura (coperto e non distrutto) viene ridotta di tanto. La difesa
// generica dell'armatura (armor_rating, contro la perforazione dell'arma) resta e vale per tutti i tipi.
//
// "damage_pure": true = arma di un tipo solo (lanciafiamme, lanciarazzi, armi elementali): i tratti possono solo
// rinforzare il suo tipo, non aggiungerne altri. "resist_pure": true = lo stesso per le difese di un'armatura.
//
// La tabella qui sotto e' letta anche da Tools/Codex/build.py (per la pagina web): una riga per tipo, stesso formato.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace SunEngine {

    enum class DamageType : int {
        Pierce = 0, Slash, Blunt, Energy, Radiation, Electric, Explosive,
        Fire, Acid, Poison, Water, Nature, Light, Dark, Arcane, Count
    };
    constexpr int kDamageTypeCount = static_cast<int>(DamageType::Count);

    struct DamageTypeInfo {
        const char* code;        // chiave nei JSON
        const char* name;        // nome in gioco
        const char* group;       // Fisico / Energia / Elementale / Arcano
        glm::vec3 color;
        const char* splitTrait;  // tratto d'arma che AGGIUNGE questo tipo (+20% del danno come questo tipo)
        const char* boostTrait;  // tratto d'arma che RINFORZA questo tipo (+15% della sua parte)
        const char* armorNew;    // tratto d'armatura che aggiunge la difesa (+15%)
        const char* armorBoost;  // tratto d'armatura che rinforza la difesa che c'e' gia' (+10%)
        const char* desc;        // cosa lo fa
        const char* fx;          // come si riconosce sul modello dell'arma
    };

    inline const DamageTypeInfo& DamageInfo(int t) {
        static const DamageTypeInfo k[kDamageTypeCount] = {
            { "pierce", "Perforazione",   "Fisico",     glm::vec3(0.86f, 0.84f, 0.78f), "Chiodata",    "Penetrante",          "Piastre balistiche",       "Piastre temprate",    "Proiettili, chiodi, punte e lance", "" },
            { "slash",  "Taglio",         "Fisico",     glm::vec3(0.94f, 0.66f, 0.60f), "Seghettata",  "Filo rasoio",         "Cotta di maglia",          "Maglia fitta",        "Lame, spade a catena, artigli e lanciaseghe", "" },
            { "blunt",  "Contundente",    "Fisico",     glm::vec3(0.78f, 0.66f, 0.50f), "Appesantita", "Massa d'urto",        "Imbottitura d'urto",       "Imbottitura spessa",  "Mazze, martelli, calci del fucile e armi gauss", "" },
            { "energy", "Energetico",     "Energia",    glm::vec3(0.36f, 0.86f, 1.00f), "Fotonica",    "Celle sovralimentate","Rivestimento riflettente", "Specchi rinforzati",  "Laser, plasma e fusione", "bagliore del colore del colpo alla bocca" },
            { "rad",    "Radioattivo",    "Energia",    glm::vec3(0.58f, 1.00f, 0.22f), "Irradiata",   "Nucleo arricchito",   "Schermatura al piombo",    "Piombo spesso",       "Armi radioattive", "alone verde malato e granelli che salgono" },
            { "shock",  "Elettrico",      "Energia",    glm::vec3(0.55f, 0.72f, 1.00f), "Voltaica",    "Bobine potenziate",   "Isolamento",               "Isolamento doppio",   "Armi voltaiche e magia elettrica", "archi e scintille azzurre" },
            { "blast",  "Esplosivo",      "Fisico",     glm::vec3(1.00f, 0.62f, 0.26f), "Detonante",   "Carica maggiorata",   "Strati antischegge",       "Antischegge spessi",  "Lanciamissili, lanciagranate, granate e mine", "fumo e braci" },
            { "fire",   "Fuoco",          "Elementale", glm::vec3(1.00f, 0.46f, 0.10f), "Incendiaria", "Combustione viva",    "Ignifuga",                 "Ignifuga rinforzata", "Lanciafiamme e magia del fuoco", "fiammelle e braci che salgono" },
            { "acid",   "Acido",          "Elementale", glm::vec3(0.78f, 0.96f, 0.16f), "Corrosiva",   "Acido concentrato",   "Smalto antiacido",         "Smalto spesso",       "Magia dell'acido", "gocce gialloverdi che colano" },
            { "poison", "Veleno",         "Elementale", glm::vec3(0.64f, 0.36f, 0.86f), "Avvelenata",  "Tossina distillata",  "Filtri sigillati",         "Filtri doppi",        "Magia del veleno", "vapori violacei" },
            { "frost",  "Acqua/Ghiaccio", "Elementale", glm::vec3(0.62f, 0.90f, 1.00f), "Gelida",      "Gelo profondo",       "Fodera termica",           "Fodera spessa",       "Magia dell'acqua e del ghiaccio", "brina e nebbia fredda che scende" },
            { "nature", "Natura",         "Elementale", glm::vec3(0.34f, 0.78f, 0.30f), "Spinosa",     "Linfa vorace",        "Unguenti protettivi",      "Unguenti densi",      "Magia della natura", "spore e foglie verdi" },
            { "holy",   "Luce",           "Elementale", glm::vec3(1.00f, 0.88f, 0.52f), "Benedetta",   "Consacrata",          "Velo d'ombra",             "Velo profondo",       "Armi sacre e magia della luce", "luce dorata che esce dalla canna o dalla lama" },
            { "dark",   "Oscurita'",      "Elementale", glm::vec3(0.60f, 0.32f, 0.78f), "Maledetta",   "Profanata",           "Sigilli sacri",            "Sigilli rinnovati",   "Armi maledette e magia oscura", "fumo nero e viola" },
            { "arcane", "Magico",         "Arcano",     glm::vec3(0.86f, 0.42f, 1.00f), "Arcana",      "Rune incise",         "Glifi di guardia",         "Glifi profondi",      "Armi arcane e psioniche", "rune e luci che girano attorno" },
        };
        return k[(t >= 0 && t < kDamageTypeCount) ? t : 0];
    }
    inline const DamageTypeInfo& DamageInfo(DamageType t) { return DamageInfo(static_cast<int>(t)); }
    inline int DamageTypeFromCode(const std::string& code) {
        for (int i = 0; i < kDamageTypeCount; ++i) if (code == DamageInfo(i).code) return i;
        // sinonimi comodi nei JSON
        if (code == "light") return static_cast<int>(DamageType::Light);
        if (code == "water" || code == "ice") return static_cast<int>(DamageType::Water);
        if (code == "radiation") return static_cast<int>(DamageType::Radiation);
        if (code == "electric") return static_cast<int>(DamageType::Electric);
        if (code == "explosive") return static_cast<int>(DamageType::Explosive);
        if (code == "magic") return static_cast<int>(DamageType::Arcane);
        return -1;
    }
    // I tipi fisici non hanno un effetto sul modello (si riconoscono dall'arma stessa)
    inline bool DamageTypeHasFx(int t) {
        return t != static_cast<int>(DamageType::Pierce) && t != static_cast<int>(DamageType::Slash) && t != static_cast<int>(DamageType::Blunt);
    }

    // Come si divide il danno di un'arma (quote che sommano a 1)
    struct DamageSplit {
        std::array<float, kDamageTypeCount> share{};
        static DamageSplit Pure(DamageType t) { DamageSplit s; s.share[static_cast<int>(t)] = 1.0f; return s; }
        static DamageSplit Two(DamageType a, float fa, DamageType b) { DamageSplit s; s.share[static_cast<int>(a)] = fa; s.share[static_cast<int>(b)] = 1.0f - fa; return s; }
        float Sum() const { float x = 0.0f; for (float v : share) x += std::max(0.0f, v); return x; }
        void Normalize() {
            const float s = Sum();
            if (s <= 1e-6f) { share = {}; share[0] = 1.0f; return; }
            for (float& v : share) v = std::max(0.0f, v) / s;
        }
        bool Has(int t) const { return share[t] > 1e-4f; }
        int Count() const { int n = 0; for (float v : share) n += v > 1e-4f ? 1 : 0; return n; }
        int Dominant() const { int b = 0; for (int i = 1; i < kDamageTypeCount; ++i) if (share[i] > share[b]) b = i; return b; }
        // Quanto passa di un colpo contro queste difese (0..1+)
        float Through(const std::array<float, kDamageTypeCount>& resist) const {
            float x = 0.0f;
            for (int i = 0; i < kDamageTypeCount; ++i) if (share[i] > 0.0f) x += share[i] * (1.0f - std::min(0.9f, resist[i]));
            return x;
        }
        // "Perforazione 60% + Esplosivo 40%"
        std::string Text() const {
            std::string s;
            std::vector<int> order;
            for (int i = 0; i < kDamageTypeCount; ++i) if (Has(i)) order.push_back(i);
            std::sort(order.begin(), order.end(), [&](int a, int b) { return share[a] > share[b]; });
            for (int i : order) {
                if (!s.empty()) s += " + ";
                s += DamageInfo(i).name;
                if (order.size() > 1) s += " " + std::to_string(static_cast<int>(std::lround(share[i] * 100.0f))) + "%";
            }
            return s;
        }
    };

    using DamageResist = std::array<float, kDamageTypeCount>;   // 0.15 = -15% di quel tipo di danno
    inline std::string ResistText(const DamageResist& r, const char* none = "nessuna") {
        std::string s;
        for (int i = 0; i < kDamageTypeCount; ++i) {
            if (std::abs(r[i]) < 0.005f) continue;
            if (!s.empty()) s += ", ";
            s += std::string(DamageInfo(i).name) + " " + (r[i] > 0 ? "" : "+") + std::to_string(static_cast<int>(std::lround(std::abs(r[i]) * 100.0f))) + "%";
        }
        return s.empty() ? std::string(none) : s;
    }

    // {"pierce": 60, "blast": 40} (percentuali o frazioni, si normalizza)
    inline bool ParseDamageSplit(const nlohmann::json& j, DamageSplit& out) {
        if (!j.is_object()) return false;
        DamageSplit s;
        for (const auto& kv : j.items()) {
            const int t = DamageTypeFromCode(kv.key());
            if (t >= 0 && kv.value().is_number()) s.share[t] += kv.value().get<float>();
        }
        if (s.Sum() <= 1e-6f) return false;
        s.Normalize();
        out = s;
        return true;
    }
    // {"pierce": 10, "slash": 15} in percentuale (anche negative: debolezza)
    inline void ParseResist(const nlohmann::json& j, DamageResist& out) {
        out = {};
        if (!j.is_object()) return;
        for (const auto& kv : j.items()) {
            const int t = DamageTypeFromCode(kv.key());
            if (t >= 0 && kv.value().is_number()) out[t] = std::clamp(kv.value().get<float>() / 100.0f, -1.0f, 0.9f);
        }
    }

    // Dove si vede l'effetto del tipo sul modello dell'arma (punti del modello a blocchi, "anchors")
    enum class DamageFxAt : int { Muzzle = 0, Blade, Core };
    struct DamageFxDef {
        int type = 0;
        DamageFxAt at = DamageFxAt::Muzzle;
        float intensity = 1.0f;
    };

    // Sull'entity che disegna l'arma in mano: id dell'arma (anche esemplare con '#'), per gli effetti del tipo di danno
    struct WeaponFxComponent {
        std::string weaponId;
        std::array<float, kDamageTypeCount> emitAcc{};   // particelle da far uscire per tipo (accumulo fra i fotogrammi)
    };
}
