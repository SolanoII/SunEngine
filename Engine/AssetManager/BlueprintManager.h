#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <nlohmann/json.hpp>
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
        Melee
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

        glm::vec3 weapon_scale = glm::vec3(0.2f, 0.2f, 0.8f);
        glm::vec3 hold_offset = glm::vec3(0.6f, 0.0f, -0.2f);
        glm::vec3 barrel_offset = glm::vec3(0.0f, 0.0f, -0.4f);
    };

    struct UnitBlueprint {
        std::string id;
        std::string displayName;
        int point_cost = 10;
        float base_speed = 0.0f;
        float stamina = 0.0f;
        float stamina_regen = 0.0f;
        float health = 0.0f;
        float toughness = 15.0f;
        int armor_rating = 0;
        float strength = 0.0f;
        float accuracy_skill = 0.0f;
        float melee_skill = 0.0f;
        float morale = 0.0f;
        int suppression_threshold = 0;
        float vision_range = 0.0f;
        float fov_degrees = 0.0f;
        float weight_capacity = 60.0f;
        float weightCapacity = 60.0f; // Alias di sicurezza per evitare errori di denominazione
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