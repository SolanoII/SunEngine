#include "../Engine/Core/Application/Application.h"
#include "../Engine/AssetManager/BlueprintManager.h"
#include "../Engine/Core/Logger/Log.h"
#include "GameLayer.h"
#include <filesystem>
#include <unordered_map>   // <-- AGGIUNGI se manca
namespace fs = std::filesystem;
std::string FindValidDir(const std::string& targetDirName) {
    // Cache dei risultati: il filesystem non cambia durante l'esecuzione, quindi
    // una volta trovato il path di una cartella, non serve cercarla di nuovo.
    // Senza questa cache, ogni SpawnOrkBoy faceva un walk ricorsivo completo.
    static std::unordered_map<std::string, std::string> s_Cache;

    // Fast path: già risolto
    auto it = s_Cache.find(targetDirName);
    if (it != s_Cache.end()) {
        return it->second;
    }

    // Slow path: prima ricerca (una sola volta per cartella)
    fs::path searchRoot = fs::current_path();

    for (int depth = 0; depth < 4; ++depth) {
        if (fs::exists(searchRoot)) {
            try {
                for (auto iter = fs::recursive_directory_iterator(
                    searchRoot, fs::directory_options::skip_permission_denied);
                    iter != fs::recursive_directory_iterator(); ++iter)
                {
                    if (iter->is_directory()) {
                        std::string dirName = iter->path().filename().string();

                        // Skip cartelle build/IDE: evita di entrare in .vs/out/.git
                        if (dirName == ".vs" || dirName == "out" || dirName == ".git") {
                            iter.disable_recursion_pending();
                            continue;
                        }

                        if (dirName == targetDirName) {
                            std::string result = iter->path().string() + "/";
                            s_Cache[targetDirName] = result;   // <-- memorizza
                            SUN_CORE_INFO("[FindValidDir] Cache miss per '{0}' -> {1}",
                                targetDirName, result);
                            return result;
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

    // Cache anche i fallimenti: evita di ripetere walk inutili se una cartella non esiste
    s_Cache[targetDirName] = "";
    SUN_CORE_WARN("[FindValidDir] Cartella '{0}' non trovata (risultato cachato)", targetDirName);
    return "";
}

int main()
{
    SunEngine::Application app;

    SUN_CORE_INFO("Directory corrente di esecuzione: {0}", fs::current_path().string());

    std::string unitsPath = FindValidDir("Units");
    std::string weaponsPath = FindValidDir("Weapons");
    std::string armorsPath = FindValidDir("Armors");
    // Il walk ricorsivo di FindValidDir puo' trovare GameData/Models/Weapons (modelli 3D) prima di
// GameData/Weapons (blueprint JSON). Le armi stanno accanto a Units: uso quella cartella se esiste.
    if (!unitsPath.empty()) {
        fs::path sibling = (fs::path(unitsPath) / ".." / "Weapons").lexically_normal();
        if (fs::is_directory(sibling)) {
            weaponsPath = sibling.string() + "/";
        }
    }
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