#include "../Engine/Core/Application/Application.h"
#include "../Engine/AssetManager/BlueprintManager.h"
#include "../Engine/Core/Logger/Log.h"
#include "GameLayer.h"
#include <filesystem>

namespace fs = std::filesystem;

std::string FindValidDir(const std::string& targetDirName) {
    fs::path searchRoot = fs::current_path();

    for (int depth = 0; depth < 4; ++depth) {
        if (fs::exists(searchRoot)) {
            try {
                for (auto it = fs::recursive_directory_iterator(searchRoot, fs::directory_options::skip_permission_denied);
                    it != fs::recursive_directory_iterator(); ++it)
                {
                    if (it->is_directory()) {
                        std::string dirName = it->path().filename().string();

                        if (dirName == ".vs" || dirName == "out" || dirName == ".git") {
                            it.disable_recursion_pending();
                            continue;
                        }

                        if (dirName == targetDirName) {
                            return it->path().string() + "/";
                        }
                    }
                }
            }
            catch (...) {
                // Ignora eccezioni sui permessi
            }
        }
        if (!searchRoot.has_parent_path()) break;
        searchRoot = searchRoot.parent_path();
    }

    return "";
}

int main()
{
    SunEngine::Application app;

    SUN_CORE_INFO("Directory corrente di esecuzione: {0}", fs::current_path().string());

    std::string unitsPath = FindValidDir("Units");
    std::string weaponsPath = FindValidDir("Weapons");
    std::string armorsPath = FindValidDir("Armors");

    if (armorsPath.empty()) {
        std::string gameDataDir = FindValidDir("GameData");
        if (!gameDataDir.empty()) {
            armorsPath = gameDataDir + "Armors/";
        }
        else {
            armorsPath = "GameData/Armors/";
        }
    }
    SUN_CORE_INFO("Percorso Units individuato: {0}", unitsPath);
    SUN_CORE_INFO("Percorso Weapons individuato: {0}", weaponsPath);
    SUN_CORE_INFO("Percorso Armors individuato: {0}", armorsPath);

    bool success = SunEngine::BlueprintManager::Get().LoadAll(unitsPath, weaponsPath, armorsPath);

    if (!success) {
        SUN_CORE_ERROR("ERRORE CRITICO: Impossibile caricare i Blueprint di gioco!");
    }

    // FIX C2061: Aggiunto namespace esplicito SunEngine::
    app.PushLayer(new SunEngine::GameLayer());
    app.Run();

    return 0;
}