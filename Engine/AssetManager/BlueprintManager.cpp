#include "BlueprintManager.h"
#include "../Core/Logger/Log.h"
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
                bp.handling_speed = j.value("handling_speed", 0.0f);
                bp.projectile_speed = j.value("projectile_speed", 100.0f);
                bp.projectile_length = j.value("projectile_length", 0.5f);
                bp.projectile_width = j.value("projectile_width", 0.05f);
                bp.flamer_cone_angle = j.value("flamer_cone_angle", 30.0f);
                bp.melta_range = j.value("melta_range", 3.0f);
                bp.melta_multiplier = j.value("melta_multiplier", 2.0f);
                bp.damage_falloff_start = j.value("damage_falloff_start", 0.0f);
                bp.damage_falloff_end = j.value("damage_falloff_end", 0.0f);

                std::string slotStr = j.value("slot_type", j.value("weapon_slot", "primary"));
                if (slotStr == "secondary" || slotStr == "Secondary") {
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
                else if (typeStr == "continuous_beam" || typeStr == "melta") bp.projectile_type = ProjectileType::ContinuousBeam;
                else if (typeStr == "grav") bp.projectile_type = ProjectileType::Grav;
                else if (typeStr == "psychic") bp.projectile_type = ProjectileType::Psychic;
                else bp.projectile_type = ProjectileType::Laser;

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
                bp.point_cost = j.value("point_cost", 15);
                bp.base_speed = j.value("base_speed", 5.0f);
                bp.stamina = j.value("stamina", 100.0f);
                bp.stamina_regen = j.value("stamina_regen", 15.0f);
                bp.health = j.value("health", 100.0f);
                bp.toughness = j.value("toughness", 15.0f);
                bp.armor_rating = j.value("armor_rating", 0);
                bp.strength = j.value("strength", 50.0f);
                bp.accuracy_skill = j.value("accuracy_skill", 1.0f);
                bp.melee_skill = j.value("melee_skill", 1.0f);
                bp.morale = j.value("morale", 100.0f);
                bp.suppression_threshold = j.value("suppression_threshold", 50);
                bp.vision_range = j.value("vision_range", 30.0f);
                bp.fov_degrees = j.value("fov_degrees", 90.0f);
                bp.weight_capacity = j.value("weight_capacity", 60.0f);
                bp.weightCapacity = bp.weight_capacity;

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
        return (it != m_Weapons.end()) ? &it->second : nullptr;
    }

    const ArmorBlueprint* BlueprintManager::GetArmorPiece(const std::string& id) const {
        auto it = m_Armors.find(id);
        return (it != m_Armors.end()) ? &it->second : nullptr;
    }

}