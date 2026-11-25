#pragma once
// Rarita' degli oggetti (armi, armi bianche, equipaggiamento, armature).
//
//   0 Comune      grigio   appena creato in officina
//   1 Non comune  verde
//   2 Raro        blu      + 1 tratto
//   3 Epico       viola    + 2 tratti
//   4 Leggendario giallo   + 3 tratti
//
// Ogni oggetto del magazzino e' un esemplare (ItemInstance) con la sua rarita', i suoi tratti e la sua condizione.
// Si sale di rarita' potenziandolo nel magazzino (energia), trovandolo come bottino alla fine delle missioni o, in
// futuro, prendendolo agli avversari. Morire con l'oggetto addosso lo consuma, lo rompe o lo fa perdere, secondo la
// modalita' (DeathRule).
//
// In partita l'esemplare viaggia come id del blueprint con un suffisso, "fucile#u12#r3#c80#tdmg.mag": u = numero
// dell'esemplare, r = rarita', c = condizione (0-100), t = tratti. BlueprintManager costruisce la copia modificata
// del blueprint da quel testo (ApplyWeaponRarity / ApplyArmorRarity), cosi' tutto il gioco (danno, ricarica, armatura,
// peso, rete) usa le statistiche giuste senza sapere niente della rarita'.
#include "Engine/AssetManager/BlueprintManager.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace SunEngine {

    enum class ItemRarity : int { Common = 0, Uncommon, Rare, Epic, Legendary, Count };

    struct RarityInfo { const char* name; ImVec4 color; float statMult; int traits; };
    inline const RarityInfo& Rarity(int r) {
        static const RarityInfo k[5] = {
            { "Comune",      ImVec4(0.66f, 0.66f, 0.64f, 1.0f), 1.00f, 0 },
            { "Non comune",  ImVec4(0.38f, 0.80f, 0.36f, 1.0f), 1.05f, 0 },
            { "Raro",        ImVec4(0.32f, 0.58f, 1.00f, 1.0f), 1.10f, 1 },
            { "Epico",       ImVec4(0.70f, 0.40f, 0.98f, 1.0f), 1.16f, 2 },
            { "Leggendario", ImVec4(1.00f, 0.80f, 0.20f, 1.0f), 1.24f, 3 },
        };
        return k[std::clamp(r, 0, 4)];
    }

    // Che tipo di oggetto e' (decide i tratti possibili e cosa migliora la rarita')
    enum class ItemKind { Ranged, Melee, Equipment, Armor };

    struct ItemTrait { const char* code; const char* name; const char* effect; ItemKind kind; };
    inline const std::vector<ItemTrait>& ItemTraits() {
        static const std::vector<ItemTrait> k = {
            { "dmg",   "Canna rinforzata",    "+8% danno",                 ItemKind::Ranged },
            { "mag",   "Caricatore maggiorato","+25% caricatore",          ItemKind::Ranged },
            { "rld",   "Mani svelte",         "-15% tempo di ricarica",    ItemKind::Ranged },
            { "acc",   "Mira salda",          "-15% dispersione",          ItemKind::Ranged },
            { "ap",    "Perforante",          "+1 perforazione",           ItemKind::Ranged },
            { "rng",   "Lunga gittata",       "+20% portata",              ItemKind::Ranged },
            { "sup",   "Terrore",             "+30% soppressione",         ItemKind::Ranged },
            { "rec",   "Rinculo domato",      "-25% rinculo",              ItemKind::Ranged },
            { "edge",  "Filo affilato",       "+10% danno",                ItemKind::Melee },
            { "map",   "Punta temprata",      "+1 perforazione",           ItemKind::Melee },
            { "reach", "Lunga",               "+0,25 m di portata",        ItemKind::Melee },
            { "lunge", "Slancio",             "+0,5 m di affondo pesante", ItemKind::Melee },
            { "boom",  "Carica potenziata",   "+15% danno",                ItemKind::Equipment },
            { "wide",  "Raggio ampio",        "+20% raggio",               ItemKind::Equipment },
            { "spare", "Scorta",              "+1 uso",                    ItemKind::Equipment },
            { "fast",  "Rapido",              "-20% tempo d'uso",          ItemKind::Equipment },
            { "thick", "Piastre spesse",      "+2 armatura",               ItemKind::Armor },
            { "pad",   "Imbottitura",         "+15% resistenza",           ItemKind::Armor },
            { "light", "Leggera",             "-25% peso",                 ItemKind::Armor },
            { "cover", "Copertura ampia",     "+5% copertura",             ItemKind::Armor },
        };
        return k;
    }
    inline const ItemTrait* FindTrait(const std::string& code) {
        for (const auto& t : ItemTraits()) if (code == t.code) return &t;
        return nullptr;
    }

    inline ItemKind KindOfItem(const std::string& itemId) {
        auto& bpm = BlueprintManager::Get();
        if (bpm.GetArmorPiece(itemId)) return ItemKind::Armor;
        if (const auto* w = bpm.GetWeapon(itemId)) {
            if (w->is_equipment) return ItemKind::Equipment;
            if (w->is_melee) return ItemKind::Melee;
        }
        return ItemKind::Ranged;
    }

    // ------------------------------------------------------------------------------------ esemplare
    struct ItemInstance {
        uint64_t uid = 0;
        std::string itemId;               // id del blueprint (arma o armatura)
        int rarity = 0;
        float condition = 100.0f;         // 0 = rotto (non si equipaggia finche' non si ripara)
        std::vector<std::string> traits;
        bool Broken() const { return condition <= 0.0f; }
        // id da mettere nello slot dell'unita' (e nei componenti in partita)
        std::string Ref() const {
            std::string s = itemId + "#u" + std::to_string(uid) + "#r" + std::to_string(rarity) + "#c" + std::to_string(static_cast<int>(std::lround(condition)));
            if (!traits.empty()) {
                s += "#t";
                for (size_t i = 0; i < traits.size(); ++i) s += (i ? "." : "") + traits[i];
            }
            return s;
        }
    };

    // Legge "#u12#r3#c80#tdmg.mag" (i pezzi mancanti restano ai valori base)
    struct ItemRefInfo { uint64_t uid = 0; int rarity = 0; float condition = 100.0f; std::vector<std::string> traits; bool isInstance = false; };
    inline ItemRefInfo ParseItemRef(const std::string& id) {
        ItemRefInfo r;
        size_t p = id.find('#');
        while (p != std::string::npos) {
            const size_t q = id.find('#', p + 1);
            const std::string part = id.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1);
            if (!part.empty()) {
                const std::string v = part.substr(1);
                switch (part[0]) {
                case 'u': r.uid = std::strtoull(v.c_str(), nullptr, 10); r.isInstance = true; break;
                case 'r': r.rarity = std::clamp(std::atoi(v.c_str()), 0, 4); break;
                case 'c': r.condition = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.0f, 100.0f); break;
                case 't': { size_t a = 0; while (a <= v.size()) { size_t b = v.find('.', a); if (b == std::string::npos) b = v.size(); if (b > a) r.traits.push_back(v.substr(a, b - a)); a = b + 1; } break; }
                default: break;
                }
            }
            p = q;
        }
        return r;
    }
    inline uint64_t InstanceUidOf(const std::string& slotId) { return slotId.find("#u") == std::string::npos ? 0 : ParseItemRef(slotId).uid; }

    // ------------------------------------------------------------------------------------ statistiche
    // Rarita': danno x1,05 / 1,10 / 1,16 / 1,24; ricarica -5% per livello, dispersione -7%, portata +4%; caricatore
    // +10% dal raro, +15% epico, +25% leggendario. Condizione: sotto il 100% il danno cala fino a -20% e la
    // dispersione sale fino a +30%.
    inline void ApplyWeaponRarity(WeaponBlueprint& w, const std::string& suffix) {
        const ItemRefInfo r = ParseItemRef(suffix);
        const float m = Rarity(r.rarity).statMult;
        const float lv = static_cast<float>(r.rarity);
        const float cond = r.condition / 100.0f;
        auto has = [&](const char* c) { return std::find(r.traits.begin(), r.traits.end(), std::string(c)) != r.traits.end(); };
        const float wear = 0.8f + 0.2f * cond;
        if (w.is_equipment) {
            w.explosion_damage *= m * (has("boom") ? 1.15f : 1.0f) * wear;
            w.base_damage *= m * (has("boom") ? 1.15f : 1.0f) * wear;
            w.explosion_radius *= (1.0f + 0.05f * lv) * (has("wide") ? 1.2f : 1.0f);
            w.heal_amount *= m;
            w.equipment_uses += (r.rarity >= 3 ? 1 : 0) + (has("spare") ? 1 : 0);
            w.equipment_use_time *= has("fast") ? 0.8f : 1.0f;
            return;
        }
        w.base_damage *= m * wear;
        if (w.is_melee) {
            if (has("edge")) w.base_damage *= 1.10f;
            if (has("map")) w.armor_penetration += 1;
            if (has("reach")) w.melee_reach += 0.25f;
            if (has("lunge")) w.heavy_lunge_distance += 0.5f;
            return;
        }
        if (has("dmg")) w.base_damage *= 1.08f;
        w.reload_time_sec *= (1.0f - 0.05f * lv) * (has("rld") ? 0.85f : 1.0f);
        w.spread_base *= (1.0f - 0.07f * lv) * (has("acc") ? 0.85f : 1.0f) * (1.0f + 0.3f * (1.0f - cond));
        w.range_effective *= (1.0f + 0.04f * lv) * (has("rng") ? 1.2f : 1.0f);
        w.damage_falloff_start *= (1.0f + 0.04f * lv) * (has("rng") ? 1.2f : 1.0f);
        w.damage_falloff_end *= (1.0f + 0.04f * lv) * (has("rng") ? 1.2f : 1.0f);
        const float magBonus = r.rarity >= 4 ? 0.25f : r.rarity >= 3 ? 0.15f : r.rarity >= 2 ? 0.10f : 0.0f;
        if (w.magazine_size > 1) w.magazine_size = static_cast<int>(std::lround(static_cast<float>(w.magazine_size) * (1.0f + magBonus) * (has("mag") ? 1.25f : 1.0f)));
        if (has("ap")) w.armor_penetration += 1;
        if (has("sup")) w.suppression_value = static_cast<int>(std::lround(static_cast<float>(w.suppression_value) * 1.3f));
        if (has("rec")) w.recoil_force *= 0.75f;
        if (has("dmg") && w.explosion_damage > 0.0f) w.explosion_damage *= 1.08f;
        w.explosion_damage *= m;
    }

    // Armature: armatura +6% per livello (+1 dal raro), resistenza +10% per livello; condizione: resistenza fino a -40%
    inline void ApplyArmorRarity(ArmorBlueprint& a, const std::string& suffix) {
        const ItemRefInfo r = ParseItemRef(suffix);
        const float lv = static_cast<float>(r.rarity);
        const float cond = r.condition / 100.0f;
        auto has = [&](const char* c) { return std::find(r.traits.begin(), r.traits.end(), std::string(c)) != r.traits.end(); };
        a.armorRating = static_cast<int>(std::lround(static_cast<float>(a.armorRating) * (1.0f + 0.06f * lv))) + (r.rarity >= 2 ? 1 : 0) + (has("thick") ? 2 : 0);
        a.maxDurability *= (1.0f + 0.10f * lv) * (has("pad") ? 1.15f : 1.0f) * (0.6f + 0.4f * cond);
        if (has("light")) a.weight *= 0.75f;
        if (has("cover")) a.coverage = std::min(1.0f, a.coverage + 0.05f);
    }

    // ------------------------------------------------------------------------------------ costi
    // Valore dell'oggetto: il suo prezzo d'acquisto nell'albero di ricerca, o 4000 per la dotazione base
    inline int64_t UpgradeCost(int64_t value, int rarity) {
        static const double k[4] = { 0.5, 1.0, 2.0, 4.0 };
        return rarity >= 4 ? 0 : static_cast<int64_t>(std::llround(static_cast<double>(value) * k[std::clamp(rarity, 0, 3)]));
    }
    inline int64_t RepairCost(int64_t value, const ItemInstance& it) {
        return static_cast<int64_t>(std::llround(static_cast<double>(value) * 0.3 * (1.0 - it.condition / 100.0) * (1.0 + 0.5 * it.rarity)));
    }
    inline int64_t DismantleValue(int64_t value, int rarity) { return static_cast<int64_t>(std::llround(static_cast<double>(value) * 0.2 * (1.0 + rarity))); }

    // Un tratto nuovo a caso, fra quelli del tipo di oggetto non ancora presi
    inline void RollTrait(ItemInstance& it, std::mt19937& rng) {
        const ItemKind kind = KindOfItem(it.itemId);
        std::vector<const ItemTrait*> pool;
        for (const auto& t : ItemTraits())
            if (t.kind == kind && std::find(it.traits.begin(), it.traits.end(), std::string(t.code)) == it.traits.end()) pool.push_back(&t);
        if (pool.empty()) return;
        it.traits.push_back(pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng)]->code);
    }
    // Porta l'esemplare alla rarita' r, aggiungendo i tratti che spettano
    inline void SetRarity(ItemInstance& it, int r, std::mt19937& rng) {
        it.rarity = std::clamp(r, 0, 4);
        while (static_cast<int>(it.traits.size()) < Rarity(it.rarity).traits) RollTrait(it, rng);
    }
    // Bottino: rarita' tirata a sorte (comune 45%, non comune 30%, raro 16%, epico 7%, leggendario 2%), almeno minRarity
    inline int RollLootRarity(std::mt19937& rng, int minRarity = 0) {
        const int roll = std::uniform_int_distribution<int>(0, 99)(rng);
        const int r = roll < 45 ? 0 : roll < 75 ? 1 : roll < 91 ? 2 : roll < 98 ? 3 : 4;
        return std::max(r, minRarity);
    }

    // Cosa succede agli oggetti addosso quando l'unita' muore, secondo la modalita'
    enum class DeathRule { None, Wear, Break, Lose };
    constexpr float kWearPerDeath = 34.0f;   // Wear: -34% di condizione a ogni morte (tre morti e si rompe)
    // Consumo con l'uso (5v5 e sopravvivenza, non la storia):
    constexpr float kWearPerShot = 0.03f;       // armi da fuoco: -0,03% a colpo (300 colpi = -9%)
    constexpr float kWearPerMeleeHit = 0.15f;   // armi bianche: -0,15% a colpo a segno (40 colpi = -6%)
    constexpr float kArmorWearPerLife = 25.0f;  // armature: -25% se in una vita la piastra si consuma tutta, in proporzione
}
