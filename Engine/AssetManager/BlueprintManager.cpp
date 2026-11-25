#include "BlueprintManager.h"
#include "GarmentJson.h"
#include "../Core/Logger/Log.h"
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace SunEngine {

    static HitboxLocation ParseLocationString(const std::string& locStr) {
        if (locStr == "Head")     return HitboxLocation::Head;
        if (locStr == "Torso")    return HitboxLocation::Torso;
        if (locStr == "LeftArm")  return HitboxLocation::LeftArm;
        if (locStr == "RightArm") return HitboxLocation::RightArm;
        if (locStr == "LeftLeg")  return HitboxLocation::LeftLeg;
        if (locStr == "RightLeg") return HitboxLocation::RightLeg;
        return HitboxLocation::Torso;
    }

    // Skin sul campo: "skin_blocks" {"Blocco": [r,g,b]} e "skin_parts" [{block, offset, bone, bone_offset, size, color, ...}]
    // Tipo di danno di un'arma senza "damage_types" nel JSON: dedotto da cos'e'
    static DamageSplit DefaultDamageSplit(const WeaponBlueprint& bp) {
        auto has = [&](const char* w) { return bp.id.find(w) != std::string::npos; };
        using T = DamageType;
        if (bp.is_melee) {
            if (has("pike") || has("spear") || has("lance") || has("lancia")) return DamageSplit::Pure(T::Pierce);
            if (has("knife") || has("dagger")) return DamageSplit::Two(T::Pierce, 0.7f, T::Slash);
            if (has("wrench") || has("hammer") || has("mace") || has("maul") || has("club") || has("bash") || has("fist")) return DamageSplit::Pure(T::Blunt);
            if (has("shovel")) return DamageSplit::Two(T::Blunt, 0.6f, T::Slash);
            if (has("cleaver") || has("axe")) return DamageSplit::Two(T::Slash, 0.8f, T::Blunt);
            return DamageSplit::Pure(T::Slash);
        }
        if (bp.is_equipment) {
            if (bp.explosion_damage > 0.0f || bp.equipment_type == "grenade" || bp.equipment_type == "landmine") return DamageSplit::Two(T::Explosive, 0.7f, T::Pierce);
            return DamageSplit::Pure(T::Pierce);
        }
        switch (bp.projectile_type) {
        case ProjectileType::Flamer: return DamageSplit::Pure(T::Fire);
        case ProjectileType::Plasma: return DamageSplit::Two(T::Energy, 0.6f, T::Fire);
        case ProjectileType::Explosive: return DamageSplit::Pure(T::Explosive);
        case ProjectileType::Gravity: return DamageSplit::Pure(T::Blunt);
        case ProjectileType::Psychic: return DamageSplit::Pure(T::Arcane);
        case ProjectileType::HeavyRound: return DamageSplit::Two(T::Pierce, 0.6f, T::Explosive);
        case ProjectileType::Laser: case ProjectileType::ContinuousBeam: return DamageSplit::Pure(T::Energy);
        default: return DamageSplit::Pure(T::Pierce);
        }
    }

    static void ParseSkin(const nlohmann::json& j, std::vector<std::pair<std::string, glm::vec4>>& blockColors, std::vector<ArmorSkinPart>& parts,
                          std::vector<GarmentDef>* garments) {
        if (garments) ParseGarments(j, *garments);   // strati sul modello animato
        auto readVec3 = [](const nlohmann::json& a, glm::vec3 def) {
            if (a.is_array() && a.size() >= 3) return glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
            return def;
        };
        auto readColor = [](const nlohmann::json& a) {
            glm::vec4 c(0.5f, 0.5f, 0.5f, 1.0f);
            if (a.is_array() && a.size() >= 3) c = glm::vec4(a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), a.size() >= 4 ? a[3].get<float>() : 1.0f);
            return c;
        };
        if (j.contains("skin_blocks") && j["skin_blocks"].is_object()) {
            for (auto& [blockName, col] : j["skin_blocks"].items()) blockColors.push_back({ blockName, readColor(col) });
        }
        if (j.contains("skin_parts") && j["skin_parts"].is_array()) {
            for (const auto& jp : j["skin_parts"]) {
                ArmorSkinPart part;
                part.block = jp.value("block", std::string());
                part.offset = readVec3(jp.value("offset", nlohmann::json()), glm::vec3(0.0f));
                part.bone = jp.value("bone", std::string());
                part.boneOffset = readVec3(jp.value("bone_offset", nlohmann::json()), glm::vec3(0.0f));
                part.size = readVec3(jp.value("size", nlohmann::json()), glm::vec3(0.1f));
                part.color = readColor(jp.value("color", nlohmann::json()));
                part.roughness = jp.value("roughness", 0.7f);
                part.metallic = jp.value("metallic", 0.0f);
                parts.push_back(part);
            }
        }
    }

    BlueprintManager& BlueprintManager::Get() {
        static BlueprintManager instance;
        return instance;
    }

    void BlueprintManager::LoadArmorsFromDirectory(const std::string& folderPath) {
        namespace fs = std::filesystem;

        if (!fs::exists(folderPath) || !fs::is_directory(folderPath)) {
            SUN_CORE_WARN("Cartella armature non trovata: {0}", folderPath);
            return;
        }

        for (const auto& entry : fs::recursive_directory_iterator(folderPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                std::ifstream file(entry.path());
                if (!file.is_open()) continue;

                nlohmann::json j;
                try {
                    file >> j;
                }
                catch (...) {
                    SUN_CORE_ERROR("Errore durante il parsing del file armatura: {0}", entry.path().string());
                    continue;
                }

                ArmorBlueprint bp;
                bp.id = j.value("id", entry.path().stem().string());
                bp.displayName = j.value("displayName", "Pezzo Ignoto");
                bp.location = ParseLocationString(j.value("location", "Torso"));
                bp.armorRating = j.value("armor_rating", 0);
                bp.coverage = j.value("coverage", 0.85f);
                bp.maxDurability = j.value("max_durability", 100.0f);
                bp.weight = j.value("weight", 0.0f);
                bp.pointCost = j.value("point_cost", 0);
                bp.faction = j.value("faction", "Neutral");
                bp.unitRestriction = j.value("unit_restriction", "");
                if (j.contains("allowed_units") && j["allowed_units"].is_array()) {
                    for (const auto& u : j["allowed_units"]) if (u.is_string()) bp.allowed_units.push_back(u.get<std::string>());
                }
                bp.blocks_headshot_bonus = j.value("blocks_headshot_bonus", false);
                bp.aim_bonus = j.value("aim_bonus", 0.0f);
                bp.morale_cap_bonus = j.value("morale_cap_bonus", 0.0f);
                bp.morale_start_bonus = j.value("morale_start_bonus", 0.0f);
                if (j.contains("resistances")) ParseResist(j["resistances"], bp.resist);
                bp.resist_pure = j.value("resist_pure", false);

                ParseSkin(j, bp.skinBlockColors, bp.skinParts, &bp.garments);

                m_Armors[bp.id] = bp;
                SUN_CORE_INFO("Armatura caricata: [{0}] {1} (ID: {2})", bp.faction, bp.displayName, bp.id);
            }
        }
    }

    void BlueprintManager::LoadWeaponsFromDirectory(const std::string& folderPath) {
        namespace fs = std::filesystem;

        if (!fs::exists(folderPath) || !fs::is_directory(folderPath)) {
            SUN_CORE_WARN("Cartella armi non trovata: {0}", folderPath);
            return;
        }

        for (const auto& entry : fs::recursive_directory_iterator(folderPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                std::ifstream f(entry.path());
                if (!f.is_open()) continue;

                nlohmann::json j;
                try {
                    f >> j;
                }
                catch (...) {
                    SUN_CORE_ERROR("Errore durante il parsing del file arma: {0}", entry.path().string());
                    continue;
                }

                WeaponBlueprint bp;
                bp.id = j.value("id", entry.path().stem().string());
                bp.displayName = j.value("displayName", j.value("name", "Arma Ignota"));
                bp.faction = j.value("faction", "Neutral");
                bp.unitRestriction = j.value("unit_restriction", "");

                bp.has_scope = j.value("has_scope", false);
                bp.scope_zoom_fov = j.value("scope_zoom_fov", 25.0f);
                bp.has_underbarrel = j.value("has_underbarrel", false);
                bp.underbarrel_type = j.value("underbarrel_type", "shotgun");

                bp.is_melee = j.value("is_melee", false);
                bp.default_melee_type = j.value("default_melee_type", "Normal");
                bp.default_melee_damage = j.value("default_melee_damage", j.value("base_damage", 20.0f));
                bp.special_ability = j.value("special_ability", "none");
                bp.parry_blocks_ranged = j.value("parry_blocks_ranged", false);
                bp.melee_reach = j.value("melee_reach", 1.85f);
                bp.heavy_lunge_distance = j.value("heavy_lunge_distance", 0.0f);

                bp.weight_kg = j.value("weight_kg", 0.0f);
                bp.point_cost = j.value("point_cost", 5);
                bp.magazine_size = j.value("magazine_size", 0);
                bp.reload_time_sec = j.value("reload_time_sec", 0.0f);
                bp.rate_of_fire = j.value("rate_of_fire", 0.0f);
                bp.base_damage = j.value("base_damage", 0.0f);
                bp.armor_penetration = j.value("armor_penetration", 0);
                bp.impact_force = j.value("impact_force", 0.0f);
                bp.toughness_bypass = j.value("toughness_bypass", 0.0f);
                bp.range_effective = j.value("range_effective", 0.0f);
                bp.heat_capacity = j.value("heat_capacity", 100.0f);
                bp.burst_count = j.value("burst_count", 3);   // <-- AGGIUNGI QUESTA RIGA
                bp.spare_magazines = std::max(0, j.value("spare_magazines", 4));
                bp.backstab_multiplier = j.value("backstab_multiplier", 1.0f);
                bp.wielder_speed_bonus = j.value("wielder_speed_bonus", 0.0f);
                bp.equipment_type = j.value("equipment_type", std::string());
                bp.equipment_uses = j.value("equipment_uses", 0);
                bp.equipment_range = j.value("equipment_range", 5.0f);
                bp.equipment_use_time = j.value("equipment_use_time", 1.0f);
                if (j.contains("repair_tags") && j["repair_tags"].is_array()) {
                    for (const auto& t : j["repair_tags"]) if (t.is_string()) bp.repair_tags.push_back(t.get<std::string>());
                }
                bp.repair_rate = j.value("repair_rate", 0.0f);
                bp.repair_range = j.value("repair_range", 2.2f);
                bp.companion_speed = j.value("companion_speed", 7.0f);
                bp.companion_attack_interval = j.value("companion_attack_interval", 1.0f);
                bp.explosion_radius = j.value("explosion_radius", 0.0f);
                bp.explosion_damage = j.value("explosion_damage", 0.0f);
                bp.companion_health = j.value("companion_health", 30.0f);
                if (j.contains("barricade_size") && j["barricade_size"].is_array() && j["barricade_size"].size() >= 3)
                    bp.barricade_size = glm::vec3(j["barricade_size"][0].get<float>(), j["barricade_size"][1].get<float>(), j["barricade_size"][2].get<float>());
                bp.heal_per_second = j.value("heal_per_second", 0.0f);
                bp.heal_amount = j.value("heal_amount", 0.0f);
                bp.charge_time = j.value("charge_time", 1.5f);
                bp.overcharge_time = j.value("overcharge_time", 3.0f);
                bp.overcharge_damage = j.value("overcharge_damage", 20.0f);
                bp.structure_armor = j.value("structure_armor", 0.0f);
                bp.fuse_time = j.value("fuse_time", 3.0f);
                bp.throw_speed = j.value("throw_speed", 14.0f);
                bp.trigger_radius = j.value("trigger_radius", 0.9f);
                bp.launches_grenade = j.value("launches_grenade", false);
                bp.fusion_shot = j.value("fusion_shot", false);
                bp.summon_unit = j.value("summon_unit", std::string());
                bp.summon_delay = j.value("summon_delay", 5.0f);
                bp.ammo_per_shot = std::max(1, j.value("ammo_per_shot", 1));
                bp.crate_charges = std::max(1, j.value("crate_charges", 3));
                bp.equipment_cooldown = j.value("equipment_cooldown", 0.0f);
                bp.move_speed_bonus = j.value("move_speed_bonus", 0.0f);
                bp.morale_bonus = j.value("morale_bonus", 0.0f);
                bp.morale_cap_bonus = j.value("morale_cap_bonus", 0.0f);
                bp.morale_start_bonus = j.value("morale_start_bonus", 0.0f);
                bp.explode_on_impact = j.value("explode_on_impact", false);
                bp.structure_damage_multiplier = j.value("structure_damage_multiplier", 1.0f);
                bp.structure_health = j.value("structure_health", 250.0f);
                bp.ammo_capacity_bonus = j.value("ammo_capacity_bonus", 0.0f);
                {
                    std::vector<std::pair<std::string, glm::vec4>> unusedColors;   // la dotazione aggiunge solo pezzi
                    ParseSkin(j, unusedColors, bp.skinParts, &bp.garments);
                }
                bp.effect_duration = j.value("effect_duration", 0.0f);
                bp.companion_evasion = j.value("companion_evasion", 0.30f);
                if (j.contains("allowed_units") && j["allowed_units"].is_array()) {
                    for (const auto& u : j["allowed_units"]) if (u.is_string()) bp.allowed_units.push_back(u.get<std::string>());
                }

                // fire_modes: array di stringhe nel JSON, es. ["semi"] o ["auto"] o ["burst"]
                if (j.contains("fire_modes") && j["fire_modes"].is_array()) {
                    for (const auto& m : j["fire_modes"]) {
                        if (m.is_string()) bp.fire_modes.push_back(m.get<std::string>());
                    }
                }
                bp.pellet_count = j.value("pellet_count", 1);
                bp.spread_base = j.value("spread_base", 0.0f);
                bp.spread_auto_multiplier = j.value("spread_auto_multiplier", 1.0f);
                bp.recoil_force = j.value("recoil_force", 0.0f);
                bp.shake_intensity = j.value("shake_intensity", 0.0f);
                bp.fov_kick = j.value("fov_kick", 0.0f);
                bp.suppression_value = j.value("suppression_value", 0);
                bp.suppression_slow = std::clamp(j.value("suppression_slow", 0.0f), 0.0f, 0.8f);
                bp.handling_speed = j.value("handling_speed", 0.0f);
                bp.projectile_speed = j.value("projectile_speed", 100.0f);
                bp.projectile_length = j.value("projectile_length", 0.5f);
                bp.projectile_width = j.value("projectile_width", 0.05f);
                bp.flamer_cone_angle = j.value("flamer_cone_angle", 30.0f);
                bp.fusion_range = j.value("fusion_range", 3.0f);
                bp.fusion_multiplier = j.value("fusion_multiplier", 2.0f);
                bp.damage_falloff_start = j.value("damage_falloff_start", 0.0f);
                bp.damage_falloff_end = j.value("damage_falloff_end", 0.0f);

                std::string slotStr = j.value("slot_type", j.value("weapon_slot", "primary"));
                if (slotStr == "equipment" || slotStr == "Equipment") {
                    bp.slot_type = WeaponSlotType::Equipment;
                    bp.is_equipment = true;
                }
                else if (slotStr == "secondary" || slotStr == "Secondary") {
                    bp.slot_type = WeaponSlotType::Secondary;
                }
                else if (slotStr == "melee" || slotStr == "Melee" || bp.is_melee) {
                    bp.slot_type = WeaponSlotType::Melee;
                }
                else {
                    bp.slot_type = WeaponSlotType::Primary;
                }

                std::string handStr = j.value("hand_type", "");
                if (handStr == "OneHanded" || j.value("hands_required", 2) == 1) {
                    bp.hand_type = HandType::OneHanded;
                    bp.hand_type_str = "OneHanded";
                }
                else {
                    bp.hand_type = HandType::TwoHanded;
                    bp.hand_type_str = "TwoHanded";
                }

                std::string typeStr = j.value("projectile_type", "laser");
                if (typeStr == "ballistic") bp.projectile_type = ProjectileType::Ballistic;
                else if (typeStr == "plasma") bp.projectile_type = ProjectileType::Plasma;
                else if (typeStr == "explosive") bp.projectile_type = ProjectileType::Explosive;
                else if (typeStr == "flamer") bp.projectile_type = ProjectileType::Flamer;
                else if (typeStr == "continuous_beam" || typeStr == "fusion") bp.projectile_type = ProjectileType::ContinuousBeam;
                else if (typeStr == "gravity") bp.projectile_type = ProjectileType::Gravity;
                else if (typeStr == "psychic") bp.projectile_type = ProjectileType::Psychic;
                else if (typeStr == "heavy_round") bp.projectile_type = ProjectileType::HeavyRound;
                else bp.projectile_type = ProjectileType::Laser;
                // Tipo di munizione: dal JSON, altrimenti dalla famiglia del proiettile
                bp.ammo_type = j.value("ammo_type", std::string());
                if (bp.ammo_type.empty()) {
                    if (typeStr == "ballistic") bp.ammo_type = "proiettili";
                    else if (typeStr == "heavy_round") bp.ammo_type = "proiettili_pesanti";
                    else if (typeStr == "plasma") bp.ammo_type = "celle_scarica";
                    else if (typeStr == "explosive") bp.ammo_type = "esplosivi";
                    else if (typeStr == "flamer") bp.ammo_type = "carburante";
                    else if (typeStr == "continuous_beam" || typeStr == "fusion") bp.ammo_type = "cariche_fusione";
                    else if (typeStr == "gravity") bp.ammo_type = "celle_gravita";
                    else if (typeStr == "psychic") bp.ammo_type = "cristalli_psi";
                    else bp.ammo_type = "celle_impulsi";
                }

                if (j.contains("primary_color") && j["primary_color"].is_array() && j["primary_color"].size() >= 3) {
                    bp.primary_color.r = j["primary_color"][0].get<float>();
                    bp.primary_color.g = j["primary_color"][1].get<float>();
                    bp.primary_color.b = j["primary_color"][2].get<float>();
                    bp.primary_color.a = (j["primary_color"].size() == 4) ? j["primary_color"][3].get<float>() : 1.0f;
                }

                if (j.contains("weapon_scale") && j["weapon_scale"].is_array() && j["weapon_scale"].size() == 3) {
                    bp.weapon_scale = glm::vec3(j["weapon_scale"][0].get<float>(), j["weapon_scale"][1].get<float>(), j["weapon_scale"][2].get<float>());
                }
                if (j.contains("hold_offset") && j["hold_offset"].is_array() && j["hold_offset"].size() == 3) {
                    bp.hold_offset = glm::vec3(j["hold_offset"][0].get<float>(), j["hold_offset"][1].get<float>(), j["hold_offset"][2].get<float>());
                }

                if (j.contains("moveset") && j["moveset"].is_object()) {
                    const auto& mj = j["moveset"];
                    bp.hasMoveset = true;
                    bp.moveset.entryNode = mj.value("entry_node", "");

                    if (mj.contains("nodes") && mj["nodes"].is_object()) {
                        for (auto& [nodeKey, nodeJson] : mj["nodes"].items()) {
                            MeleeNode node;
                            node.damageMultiplier = nodeJson.value("damage_multiplier", 1.0f);
                            node.staminaCost = nodeJson.value("stamina_cost", 10.0f);

                            if (nodeJson.contains("timings") && nodeJson["timings"].is_object()) {
                                const auto& tj = nodeJson["timings"];
                                node.timings.windup = tj.value("windup", 0.10f);
                                node.timings.active = tj.value("active", 0.15f);
                                node.timings.recovery = tj.value("recovery", 0.20f);
                            }

                            if (nodeJson.contains("combo_window") && nodeJson["combo_window"].is_object()) {
                                const auto& cwj = nodeJson["combo_window"];
                                node.comboWindow.start = cwj.value("start", 0.10f);
                                node.comboWindow.end = cwj.value("end", 0.35f);
                            }

                            if (nodeJson.contains("pose") && nodeJson["pose"].is_object()) {
                                for (auto& [boneName, rotArray] : nodeJson["pose"].items()) {
                                    if (rotArray.is_array() && rotArray.size() == 3) {
                                        node.pose[boneName] = glm::vec3(
                                            rotArray[0].get<float>(),
                                            rotArray[1].get<float>(),
                                            rotArray[2].get<float>()
                                        );
                                    }
                                }
                            }

                            if (nodeJson.contains("transitions") && nodeJson["transitions"].is_object()) {
                                for (auto& [inputKey, nextNodeKey] : nodeJson["transitions"].items()) {
                                    node.transitions[inputKey] = nextNodeKey.get<std::string>();
                                }
                            }

                            bp.moveset.nodes[nodeKey] = node;
                        }
                    }
                }

                // Tipi di danno: dal JSON, o dedotti dal tipo di arma
                if (j.contains("damage_types") && ParseDamageSplit(j["damage_types"], bp.damage)) bp.damage_types_set = true;
                else bp.damage = DefaultDamageSplit(bp);
                bp.damage_pure = j.value("damage_pure", false);
                if (j.contains("damage_fx") && j["damage_fx"].is_array()) {
                    bp.damage_fx_set = true;
                    for (const auto& f : j["damage_fx"]) {
                        if (!f.is_object()) continue;
                        DamageFxDef d;
                        d.type = DamageTypeFromCode(f.value("type", std::string()));
                        if (d.type < 0) continue;
                        const std::string at = f.value("at", std::string(bp.is_melee ? "blade" : "muzzle"));
                        d.at = at == "blade" ? DamageFxAt::Blade : (at == "core" ? DamageFxAt::Core : DamageFxAt::Muzzle);
                        d.intensity = f.value("intensity", 1.0f);
                        bp.damage_fx.push_back(d);
                    }
                }

                m_Weapons[bp.id] = bp;
                SUN_CORE_INFO("Arma caricata: {0} (ID: {1}, Moveset: {2})", bp.displayName, bp.id, bp.hasMoveset ? "SI" : "NO");
            }
        }
    }

    void BlueprintManager::LoadUnitsFromDirectory(const std::string& folderPath) {
        namespace fs = std::filesystem;

        if (!fs::exists(folderPath) || !fs::is_directory(folderPath)) {
            SUN_CORE_WARN("Cartella unita non trovata: {0}", folderPath);
            return;
        }
        // Razze: GameData/Races/races.json, accanto alla cartella delle unita'
        {
            const fs::path racesFile = fs::path(folderPath).lexically_normal().parent_path().parent_path() / "Races" / "races.json";
            const fs::path racesFile2 = fs::path(folderPath).parent_path() / "Races" / "races.json";
            std::ifstream rf(fs::exists(racesFile) ? racesFile : racesFile2);
            if (rf.is_open()) {
                try {
                    nlohmann::json rj; rf >> rj;
                    if (rj.contains("races") && rj["races"].is_object()) {
                        for (const auto& kv : rj["races"].items()) {
                            RaceInfo r; r.id = kv.key(); r.name = kv.value().value("name", kv.key());
                            if (kv.value().contains("resistances")) ParseResist(kv.value()["resistances"], r.resist);
                            m_Races[r.id] = r;
                        }
                    }
                }
                catch (...) { SUN_CORE_ERROR("Errore nel file delle razze (Races/races.json)"); }
            }
            else SUN_CORE_WARN("Razze non trovate (Races/races.json)");
        }

        for (const auto& entry : fs::recursive_directory_iterator(folderPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                std::ifstream f(entry.path());
                if (!f.is_open()) continue;

                nlohmann::json j;
                try {
                    f >> j;
                }
                catch (...) {
                    SUN_CORE_ERROR("Errore durante il parsing del file unita: {0}", entry.path().string());
                    continue;
                }

                UnitBlueprint bp;
                bp.id = j.value("id", entry.path().stem().string());
                bp.displayName = j.value("displayName", j.value("id", "Unita Ignota"));
                bp.skeleton_id = j.value("skeleton", std::string());
                bp.skeleton_scale = j.value("skeleton_scale", 1.0f);
                bp.animated_body = j.value("animated_body", std::string());
                bp.model_scale = j.value("model_scale", 1.0f);
                bp.ai_role = j.value("ai_role", std::string());
                bp.point_cost = j.value("point_cost", 15);
                bp.max_per_squad = j.value("max_per_squad", 5);
                bp.base_speed = j.value("base_speed", 5.0f);
                bp.stamina = j.value("stamina", 100.0f);
                bp.stamina_regen = j.value("stamina_regen", 15.0f);
                bp.health = j.value("health", 100.0f);
                bp.toughness = j.value("toughness", 15.0f);
                bp.armor_rating = j.value("armor_rating", 0);
                bp.strength = j.value("strength", 50.0f);
                bp.climb_height = j.value("climb_height", 0.0f);
                bp.momentum_bonus = j.value("momentum_bonus", 0.0f);
                bp.momentum_time = j.value("momentum_time", 2.5f);
                bp.charge_threshold = j.value("charge_threshold", 0.7f);
                bp.charge_damage = j.value("charge_damage", 0.0f);
                bp.quadruped_run = j.value("quadruped_run", false);
                bp.climb_speed = j.value("climb_speed", 0.6f);
                bp.accuracy_skill = j.value("accuracy_skill", 1.0f);
                bp.melee_skill = j.value("melee_skill", 1.0f);
                bp.morale = j.value("morale", 100.0f);
                bp.suppression_threshold = j.value("suppression_threshold", 50);
                bp.vision_range = j.value("vision_range", 30.0f);
                bp.fov_degrees = j.value("fov_degrees", 90.0f);
                bp.weight_capacity = j.value("weight_capacity", 60.0f);
                bp.weightCapacity = bp.weight_capacity;
                bp.ammo_multiplier = j.value("ammo_multiplier", 1.0f);
                bp.loadout_version = j.value("loadout_version", 0);
                bp.no_melee = j.value("no_melee", false);
                bp.melee_fallback = j.value("melee_fallback", std::string());
                bp.strict_loadout = j.value("strict_loadout", false);
                bp.race = j.value("race", std::string());
                if (const RaceInfo* r = GetRace(bp.race)) bp.resist = r->resist;
                if (j.contains("resistances")) {
                    DamageResist own{}; ParseResist(j["resistances"], own);
                    for (int t = 0; t < kDamageTypeCount; ++t) bp.resist[t] = std::clamp(bp.resist[t] + own[t], -1.0f, 0.9f);
                }
                if (j.contains("base_skin") && j["base_skin"].is_object()) ParseSkin(j["base_skin"], bp.baseSkinBlockColors, bp.baseSkinParts, &bp.baseGarments);
                if (j.contains("default_loadout") && j["default_loadout"].is_object()) {
                    const auto& lj = j["default_loadout"];
                    bp.has_default_loadout = true;
                    bp.default_primary = lj.value("primary", std::string());
                    bp.default_secondary = lj.value("secondary", std::string());
                    bp.default_melee = lj.value("melee", std::string());
                    if (lj.contains("equipment") && lj["equipment"].is_array()) {
                        for (size_t i = 0; i < 3 && i < lj["equipment"].size(); ++i)
                            if (lj["equipment"][i].is_string()) bp.default_equipment[i] = lj["equipment"][i].get<std::string>();
                    }
                    // Armatura/vesti base: "armor": {"head": "", "torso": "...", "arm_l", "arm_r", "leg_l", "leg_r"}
                    if (lj.contains("armor") && lj["armor"].is_object()) {
                        const auto& aj = lj["armor"];
                        bp.has_default_armor = true;
                        const char* keys[6] = { "head", "torso", "arm_l", "arm_r", "leg_l", "leg_r" };
                        for (int i = 0; i < 6; ++i) bp.default_armor[i] = aj.value(keys[i], std::string());
                    }
                }

                m_Units[bp.id] = bp;
                SUN_CORE_INFO("Unita caricata: {0} (ID: {1})", bp.displayName, bp.id);
            }
        }
    }

    bool BlueprintManager::LoadAll(const std::string& unitsPath, const std::string& weaponsPath, const std::string& armorsPath) {
        if (!unitsPath.empty()) LoadUnitsFromDirectory(unitsPath);
        if (!weaponsPath.empty()) LoadWeaponsFromDirectory(weaponsPath);
        if (!armorsPath.empty()) LoadArmorsFromDirectory(armorsPath);

        return true;
    }

    const UnitBlueprint* BlueprintManager::GetUnit(const std::string& id) const {
        auto it = m_Units.find(id);
        return (it != m_Units.end()) ? &it->second : nullptr;
    }

    const WeaponBlueprint* BlueprintManager::GetWeapon(const std::string& id) const {
        auto it = m_Weapons.find(id);
        if (it != m_Weapons.end()) return &it->second;
        const size_t hash = id.find('#');
        if (hash == std::string::npos) return nullptr;
        // esemplare: copia del blueprint base con le sue modifiche (rarita', tratti, condizione)
        auto v = m_WeaponVariants.find(id);
        if (v != m_WeaponVariants.end()) return v->second.get();
        auto base = m_Weapons.find(id.substr(0, hash));
        if (base == m_Weapons.end()) return nullptr;
        auto bp = std::make_unique<WeaponBlueprint>(base->second);
        bp->id = id;
        if (m_WeaponVariantFn) m_WeaponVariantFn(*bp, id.substr(hash));
        return (m_WeaponVariants[id] = std::move(bp)).get();
    }

    const ArmorBlueprint* BlueprintManager::GetArmorPiece(const std::string& id) const {
        auto it = m_Armors.find(id);
        if (it != m_Armors.end()) return &it->second;
        const size_t hash = id.find('#');
        if (hash == std::string::npos) return nullptr;
        auto v = m_ArmorVariants.find(id);
        if (v != m_ArmorVariants.end()) return v->second.get();
        auto base = m_Armors.find(id.substr(0, hash));
        if (base == m_Armors.end()) return nullptr;
        auto bp = std::make_unique<ArmorBlueprint>(base->second);
        bp->id = id;
        if (m_ArmorVariantFn) m_ArmorVariantFn(*bp, id.substr(hash));
        return (m_ArmorVariants[id] = std::move(bp)).get();
    }

}