#pragma once
#include "HitboxBlock.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <optional>

namespace SunEngine {

    class SkeletonLoader {
    public:
        static std::optional<SkeletonData> LoadFromFile(const std::string& filePath) {
            std::ifstream file(filePath);
            if (!file.is_open()) {
                std::cerr << "[SkeletonLoader] Errore: Impossibile aprire: " << filePath << std::endl;
                return std::nullopt;
            }

            try {
                nlohmann::json j;
                file >> j;

                SkeletonData skeleton;
                skeleton.id = j.value("id", "unknown");
                skeleton.race = j.value("race", "unknown");

                for (const auto& item : j["blocks"]) {
                    HitboxBlock block;
                    block.name = item["name"].get<std::string>();
                    block.parentIndex = item.value("parentIndex", -1);

                    auto offset = item["offset"];
                    block.localOffset = glm::vec3(offset[0].get<float>(), offset[1].get<float>(), offset[2].get<float>());

                    if (item.contains("pivotOffset")) {
                        auto pivot = item["pivotOffset"];
                        block.pivotOffset = glm::vec3(pivot[0].get<float>(), pivot[1].get<float>(), pivot[2].get<float>());
                    }

                    auto size = item["size"];
                    block.size = glm::vec3(size[0].get<float>(), size[1].get<float>(), size[2].get<float>());

                    skeleton.blocks.push_back(block);
                }

                return skeleton;
            }
            catch (const std::exception& e) {
                std::cerr << "[SkeletonLoader] Eccezione parsing JSON: " << e.what() << std::endl;
                return std::nullopt;
            }
        }
    };
}