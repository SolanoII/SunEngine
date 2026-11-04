#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <nlohmann/json.hpp>
#include "../Renderer/GarmentShell.h"
#include <glm/glm.hpp>

namespace SunEngine {

    enum class HitboxLocation {
        Head,
        Torso,
        LeftArm,
        RightArm,
        LeftLeg,
        RightLeg
    };

    enum class ProjectileType {
        Laser,
        Ballistic,
        Plasma,
        Explosive,
        Flamer,
        ContinuousBeam,
        Grav,
        Psychic
    };

    enum class WeaponSlotType {
        Primary,
        Secondary,
        Melee,
        Equipment   // dotazione tattica (scatola di munizioni, ...): slot equipaggiamento, tasti 4-5-6
    };

    enum class HandType {
        OneHanded,
        TwoHanded
    };

    struct MeleeTiming {
        float windup = 0.10f;
        float active = 0.15f;
        float recovery = 0.20f;
    };

    struct ComboWindow {
        float start = 0.10f;
        float end = 0.35f;
    };

    struct MeleeNode {
        float damageMultiplier = 1.0f;
        float staminaCost = 10.0f;
        MeleeTiming timings;
        ComboWindow comboWindow;
        std::unordered_map<std::string, glm::vec3> pose;
        std::unordered_map<std::string, std::string> transitions;
    };

    struct MeleeMoveset {
        std::string entryNode = "";
        std::unordered_map<std::string, MeleeNode> nodes;
    };

    struct PowerMode {
        int ammo_cost = 1;
        float damage_mult = 1.0f;
        int ap_bonus = 0;
        int heat_generation = 0;
    };

    // Pezzo visivo di un'armatura/veste ("skin"): un cubo agganciato a un blocco dello scheletro a cubi
    // (offset dal centro del blocco, nei suoi assi) e, per il modello animato, a un osso (offset negli assi
    // del corpo: +X destra, +Y su, -Z avanti). Solo estetica: non e' una hitbox.
    struct ArmorSkinPart {
        std::string block;             // es. "Head"
        glm::vec3 offset{ 0.0f };
        std::string bone;              // es. "mixamorig1:Head" (vuoto = niente sul modello animato)
        glm::vec3 boneOffset{ 0.0f };
        glm::vec3 size{ 0.1f };
        glm::vec4 color{ 0.5f, 0.5f, 0.5f, 1.0f };
        float roughness = 0.7f;
        float metallic = 0.0f;
    };

    struct WeaponBlueprint {
        std::string id;
        std::string displayName;
        std::string faction;
        std::string unitRestriction;

        WeaponSlotType slot_type = WeaponSlotType::Primary;
        HandType hand_type = HandType::TwoHanded;
        std::string hand_type_str = "TwoHanded";

        float weight_kg = 0.0f;
        int point_cost = 5;

        bool has_scope = false;
        float scope_zoom_fov = 25.0f;
        bool has_underbarrel = false;
        std::string underbarrel_type = "shotgun";

        bool is_melee = false;
        std::string default_melee_type = "Normal";
        float default_melee_damage = 20.0f;
        std::string special_ability = "none";
        bool parry_blocks_ranged = false;   // la parata con quest'arma ferma anche i colpi a distanza (perfect deflect)
        float melee_reach = 1.85f;          // portata del colpo in mischia (metri, dal centro dell'attaccante)
        float heavy_lunge_distance = 0.0f;  // attacco pesante con scatto in avanti (metri), es. lancia dello yoof
        bool hasMoveset = false;
        MeleeMoveset moveset;

        int magazine_size = 0;
        float reload_time_sec = 0.0f;
        std::vector<std::string> fire_modes;
        int burst_count = 3;   // <-- NUOVO: colpi per raffica se fire_modes == ["burst"]
        float rate_of_fire = 0.0f;
        float base_damage = 0.0f;
        int armor_penetration = 0;
        float impact_force = 0.0f;
        float toughness_bypass = 0.0f;
        float range_effective = 0.0f;

        int pellet_count = 1;
        float flamer_cone_angle = 30.0f;
        float melta_range = 4.5f;
        float melta_multiplier = 2.5f;
        float damage_falloff_start = 0.0f;
        float damage_falloff_end = 0.0f;

        float spread_base = 0.0f;
        float spread_auto_multiplier = 1.0f;
        float recoil_force = 0.0f;
        float shake_intensity = 0.0f;
        float fov_kick = 0.0f;
        int suppression_value = 0;
        float handling_speed = 0.0f;
        float heat_capacity = 100.0f;
        float heat_recoil_multiplier = 1.0f;

        ProjectileType projectile_type = ProjectileType::Laser;
        bool is_beam = true;
        glm::vec4 primary_color = glm::vec4(1.0f, 0.40f, 0.05f, 0.95f);
        glm::vec4 core_color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        float projectile_speed = 100.0f;
        float projectile_length = 0.5f;
        float projectile_width = 0.38f;
        float explosion_radius = 0.0f;

        std::unordered_map<std::string, PowerMode> power_modes;

        // --- Munizioni limitate ---
        // Riserva iniziale = magazine_size * spare_magazines * ammo_multiplier dell'unita'.
        // Es. Grot Blasta: 6 colpi x 2 caricatori di scorta = 12 di riserva (18 in tutto).
        int spare_magazines = 4;

        // --- Passive delle armi da mischia ---
        float backstab_multiplier = 1.0f;    // danno se colpisci alle spalle (x2 = doppio)
        float wielder_speed_bonus = 0.0f;    // velocita' extra di chi la tiene in mano (0.05 = +5%)

        // --- Dotazione tattica (slot_type "equipment") ---
        bool is_equipment = false;
        std::string equipment_type;          // "ammo_box": click sinistro = rifornisce se', destro = un compagno vicino
        int equipment_uses = 0;              // usi per partita
        float equipment_range = 5.0f;        // portata per rifornire un compagno (metri)
        float equipment_use_time = 1.0f;     // secondi per completare un rifornimento

        // Riparazione (es. chiave inglese): click destro tenuto ripara le entita' con almeno uno di questi tag
        // ("Structure", "Construct", "Mechanical": veicoli, strutture, torrette, semoventi...)
        std::vector<std::string> repair_tags;
        float repair_rate = 0.0f;            // salute riparata al secondo
        float repair_range = 2.2f;           // metri

        // Compagno squig (equipment_type "squig" = morso, "squig_bomb" = carica ed esplode, uno per partita).
        // base_damage / armor_penetration = morso; explosion_radius / explosion_damage = esplosione.
        float companion_speed = 7.0f;
        float companion_attack_interval = 1.0f;
        float explosion_damage = 0.0f;
        // Scrap ("scrap_barricade"): fermo e con il click sinistro tenuto per equipment_use_time secondi costruisce
        // una barricata davanti a se' (copertura che ferma colpi e movimento), grande barricade_size.
        glm::vec3 barricade_size = glm::vec3(2.0f, 1.0f, 0.4f);
        // Zaino di cibo ("food_pack"): click sinistro lo posa a terra; per effect_duration secondi cura
        // heal_per_second ai compagni entro equipment_range metri.
        float heal_per_second = 0.0f;
        // Medikit ("medkit"): cura istantanea di heal_amount punti salute su se stessi dopo equipment_use_time secondi
        float heal_amount = 0.0f;
        float effect_duration = 0.0f;
        // Granata ("grenade") e mina ("landmine"): explosion_radius / explosion_damage / armor_penetration.
        // fuse_time = miccia della granata o tempo per armare la mina; throw_speed = velocita' di lancio;
        // trigger_radius = quanto vicino deve passare un nemico per far scattare la mina.
        float fuse_time = 3.0f;
        bool explode_on_impact = false;    // granata a impatto (es. stikkbomb): scoppia appena tocca qualcosa
        float ammo_capacity_bonus = 0.0f;  // zaino munizioni ("ammo_backpack"): +50% di riserva = 0.5
        // Danno delle esplosioni a strutture e mezzi (barricate, muri, veicoli, torrette): x structure_damage_multiplier.
        // Es. granata anti-struttura del loota = 5.
        float structure_damage_multiplier = 1.0f;
        float structure_health = 250.0f;   // salute di barricate e muri costruiti con questo oggetto
        // Skin della dotazione sul corpo (es. zaino sulla schiena), come "skin_parts" delle armature
        std::vector<ArmorSkinPart> skinParts;
        std::vector<GarmentDef> garments;   // strati sul modello animato ("garments")
        float throw_speed = 14.0f;
        float trigger_radius = 0.9f;
        float companion_health = 30.0f;      // lo squig si puo' uccidere: poca salute...
        float companion_evasion = 0.30f;     // ...ma schiva il 30% dei colpi

        // Unita' che possono equipaggiarla ("allowed_units"); vuoto = tutte (armi esistenti, da rivedere)
        std::vector<std::string> allowed_units;
        bool AllowedFor(const std::string& unitId) const {
            if (allowed_units.empty()) return true;
            for (const auto& u : allowed_units) if (u == unitId) return true;
            return false;
        }

        glm::vec3 weapon_scale = glm::vec3(0.2f, 0.2f, 0.8f);
        glm::vec3 hold_offset = glm::vec3(0.6f, 0.0f, -0.2f);
        glm::vec3 barrel_offset = glm::vec3(0.0f, 0.0f, -0.4f);
    };

    struct UnitBlueprint {
        std::string id;
        std::string displayName;
        // Corpo: scheletro a cubi (Skeletons/<skeleton_id>.json, vuoto = id dell'unita'), sua scala, modello
        // animato ("human", "ork", "ogryn", "gretchin", "neophyte", "astartes"; vuoto = dedotto dall'id) e scala
        // del modello. Es. lo yoof usa lo scheletro e il modello del boy, piu' piccoli (0.85).
        std::string skeleton_id;
        float skeleton_scale = 1.0f;
        std::string animated_body;
        float model_scale = 1.0f;
        // Comportamento dei bot: "ranged" (tiratore), "versatile", "melee" (vuoto = versatile)
        std::string ai_role;
        int point_cost = 10;
        int max_per_squad = 5;   // quante unita' di questo tipo puo' avere una squadra ("max_per_squad")
        float base_speed = 0.0f;
        float stamina = 0.0f;
        float stamina_regen = 0.0f;
        float health = 0.0f;
        float toughness = 15.0f;
        int armor_rating = 0;
        float strength = 0.0f;
        // Arrampicata lenta (es. gretchin): muri fino a "climb_height" metri, piu' alti di quanto scavalcherebbe,
        // a "climb_speed" m/s. 0 = solo lo scavalcamento normale.
        float climb_height = 0.0f;
        float climb_speed = 0.6f;
        float accuracy_skill = 0.0f;
        float melee_skill = 0.0f;
        float morale = 0.0f;
        int suppression_threshold = 0;
        float vision_range = 0.0f;
        float fov_degrees = 0.0f;
        float weight_capacity = 60.0f;
        float weightCapacity = 60.0f; // Alias di sicurezza per evitare errori di denominazione
        // Vestiti sempre visibili sotto l'armatura ("base_skin": "skin_blocks" + "skin_parts"), es. calzoni degli orki
        std::vector<std::pair<std::string, glm::vec4>> baseSkinBlockColors;
        std::vector<ArmorSkinPart> baseSkinParts;
        std::vector<GarmentDef> baseGarments;   // vestiti base sul modello animato ("base_skin": {"garments": [...]})
        // Niente armi da mischia ("no_melee", es. loota): lo slot resta vuoto e i tasti della mischia usano
        // l'arma di riserva "melee_fallback" (es. botta col deffgun), che non si puo' equipaggiare.
        bool no_melee = false;
        std::string melee_fallback;
        // Solo l'equipaggiamento definito per questa unita' ("strict_loadout", es. tutti gli orki): niente armi e
        // armature generiche (quelle senza "allowed_units")
        bool strict_loadout = false;
        // Munizioni di scorta del personaggio: moltiplica la riserva di ogni arma (1 = standard)
        float ammo_multiplier = 1.0f;
        // Dotazione base ("default_loadout"): armi ed equipaggiamento con cui l'unita' arriva nel garage
        bool has_default_loadout = false;
        std::string default_primary;
        std::string default_secondary;
        std::string default_melee;
        std::string default_equipment[3];
        // Versione della dotazione base: se un'unita' salvata nel profilo ne ha una piu' vecchia, la riceve di nuovo
        // al caricamento (es. yoof = 3: gli yoof gia' nel garage passano dal fucile laser al kit da yoof)
        int loadout_version = 0;
        bool has_default_armor = false;
        std::string default_armor[6];      // testa, torso, braccio sx, braccio dx, gamba sx, gamba dx ("" = scoperto)
    };

    struct ArmorBlueprint {
        std::string id;
        std::string displayName;
        HitboxLocation location = HitboxLocation::Torso;
        int armorRating = 0;
        float coverage = 0.85f;
        float maxDurability = 100.0f;
        float weight = 0.0f;
        int pointCost = 0;
        std::string faction;
        std::string unitRestriction;

        // Unita' che possono indossarla ("allowed_units"); vuoto = regola di "unit_restriction"
        std::vector<std::string> allowed_units;
        bool AllowedFor(const std::string& unitId) const {
            if (!allowed_units.empty()) {
                for (const auto& u : allowed_units) if (u == unitId) return true;
                return false;
            }
            return unitRestriction.empty() || unitRestriction == unitId;
        }
        // Elmetto: finche' regge, annulla il moltiplicatore del colpo in testa (es. elmetto di ferraglia del
        // gretchin: un colpo di lasgun in testa non lo uccide piu' al primo colpo)
        bool blocks_headshot_bonus = false;

        // Skin sul campo: colore di alcuni blocchi dello scheletro ("skin_blocks": {"Torso": [r,g,b]}) e
        // pezzi aggiunti ("skin_parts"), es. vesti e elmetto del gretchin
        std::vector<std::pair<std::string, glm::vec4>> skinBlockColors;
        std::vector<ArmorSkinPart> skinParts;
        // Strati sul modello animato (texture/mesh sopra il corpo, vedi GarmentShell.h). Se ci sono, i pezzi
        // a cubi agganciati alle ossa ("bone" negli skin_parts) non vengono usati sul modello animato.
        std::vector<GarmentDef> garments;
    };

    class BlueprintManager {
    public:
        static BlueprintManager& Get();

        bool LoadAll(const std::string& unitsPath, const std::string& weaponsPath, const std::string& armorsPath = "");
        void LoadArmorsFromDirectory(const std::string& folderPath);
        void LoadWeaponsFromDirectory(const std::string& folderPath);
        void LoadUnitsFromDirectory(const std::string& folderPath);

        const UnitBlueprint* GetUnit(const std::string& id) const;
        const WeaponBlueprint* GetWeapon(const std::string& id) const;
        const ArmorBlueprint* GetArmorPiece(const std::string& id) const;

        // L'unita' puo' equipaggiare l'oggetto? Con "strict_loadout" solo se l'oggetto la elenca in "allowed_units".
        bool CanEquip(const WeaponBlueprint& w, const std::string& unitId) const {
            const UnitBlueprint* u = GetUnit(unitId);
            if (u && u->strict_loadout) {
                for (const auto& a : w.allowed_units) if (a == unitId) return true;
                return false;
            }
            return w.AllowedFor(unitId);
        }
        bool CanEquip(const ArmorBlueprint& a, const std::string& unitId) const {
            const UnitBlueprint* u = GetUnit(unitId);
            if (u && u->strict_loadout) {
                for (const auto& x : a.allowed_units) if (x == unitId) return true;
                return false;
            }
            return a.AllowedFor(unitId);
        }

        const std::unordered_map<std::string, UnitBlueprint>& GetUnits() const { return m_Units; }
        const std::unordered_map<std::string, WeaponBlueprint>& GetWeapons() const { return m_Weapons; }
        const std::unordered_map<std::string, ArmorBlueprint>& GetArmorPieces() const { return m_Armors; }

    private:
        BlueprintManager() = default;
        std::unordered_map<std::string, UnitBlueprint> m_Units;
        std::unordered_map<std::string, WeaponBlueprint> m_Weapons;
        std::unordered_map<std::string, ArmorBlueprint> m_Armors;
    };

}