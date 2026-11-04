#pragma once
// Lettura degli strati "garments" dai JSON di armature, unita' (base_skin) e dotazioni.
// Esempio:
//   "garments": [ { "id": "grot_rags_torso", "bones": ["Spine", "Spine1", "Spine2", "Hips"],
//                   "height_range": [0.44, 0.73], "inflate": 0.010, "noise": 0.6,
//                   "texture": "Textures/Garments/grot_rags.png", "tex_tiles": 4, "alpha_cut": 0.5 } ]
// Campi: bones, min_weight, height_range [min,max] (frazione dell'altezza in posa T), side (+1 sinistra, -1 destra),
// max_abs_x, facing [x,y,z] + facing_min (es. solo il davanti), inflate, noise, noise_scale, plate_cells,
// plate_step, texture, tex_tiles, color, roughness, metallic, alpha_cut, model (FBX skinnato opzionale).
#include "../Renderer/GarmentShell.h"
#include <nlohmann/json.hpp>

namespace SunEngine {

    inline GarmentDef ParseGarmentDef(const nlohmann::json& j) {
        GarmentDef d;
        auto vec2 = [&](const char* key, glm::vec2 def) {
            if (j.contains(key) && j[key].is_array() && j[key].size() >= 2) return glm::vec2(j[key][0].get<float>(), j[key][1].get<float>());
            return def;
        };
        auto vec3 = [&](const char* key, glm::vec3 def) {
            if (j.contains(key) && j[key].is_array() && j[key].size() >= 3) return glm::vec3(j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>());
            return def;
        };
        d.id = j.value("id", std::string());
        if (j.contains("bones") && j["bones"].is_array()) for (const auto& b : j["bones"]) if (b.is_string()) d.bones.push_back(b.get<std::string>());
        d.minWeight = j.value("min_weight", d.minWeight);
        d.heightRange = vec2("height_range", d.heightRange);
        d.maxAbsX = j.value("max_abs_x", d.maxAbsX);
        d.side = j.value("side", d.side);
        d.facing = vec3("facing", d.facing);
        d.facingMin = j.value("facing_min", d.facingMin);
        d.inflate = j.value("inflate", d.inflate);
        d.noise = j.value("noise", d.noise);
        d.noiseScale = j.value("noise_scale", d.noiseScale);
        d.plateCells = j.value("plate_cells", d.plateCells);
        d.plateStep = j.value("plate_step", d.plateStep);
        d.texture = j.value("texture", std::string());
        d.texTiles = j.value("tex_tiles", d.texTiles);
        if (j.contains("color") && j["color"].is_array() && j["color"].size() >= 3) {
            const auto& c = j["color"];
            d.color = glm::vec4(c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c.size() >= 4 ? c[3].get<float>() : 1.0f);
        }
        d.roughness = j.value("roughness", d.roughness);
        d.metallic = j.value("metallic", d.metallic);
        d.alphaCut = j.value("alpha_cut", d.alphaCut);
        d.model = j.value("model", std::string());
        return d;
    }

    inline void ParseGarments(const nlohmann::json& j, std::vector<GarmentDef>& out) {
        if (!j.contains("garments") || !j["garments"].is_array()) return;
        for (const auto& g : j["garments"]) if (g.is_object()) out.push_back(ParseGarmentDef(g));
    }
}
