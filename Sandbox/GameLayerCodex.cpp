// Codex: tutto quello che c'e' nel gioco, scheda per scheda, letto dai file di GameData.
//
//   Panoramica      quante unita', armi, attrezzi e armature ci sono e cosa manca ancora (modelli, foto, chi le usa)
//   Unita'          i personaggi dell'albero di ricerca (grado, costi, dotazione base, armi che possono usare)
//   Nemici e storia le unita' d'elite e quelle che non si ricercano (boss e personaggi delle storie)
//   Armi da fuoco, Armi bianche, Equipaggiamento, Armature
//
// Ogni scheda mostra la foto (GameData/Codex/img/<id>.jpg), tutte le statistiche del file con le etichette di
// GameData/Codex/labels.json (le stesse della pagina web, Tools/Codex) e chi puo' portare l'oggetto.
//
// Foto delle unita' per il Codex: avviare il gioco con SUN_CODEX_SHOTS=<cartella>. Ogni unita' passa in vetrina,
// ferma, di tre quarti; si salva un'immagine a testa (<id>.ppm, senza interfaccia) e alla fine il gioco si chiude.
// Tools/Codex/build.py le ritaglia e le mette in GameData/Codex/img.
#include "GameLayer.h"
#include "UiTheme.h"
#include "DamageUi.h"
#include "Engine/Renderer/Core/Texture.h"
#include "Engine/Core/Logger/Log.h"
#include <nlohmann/json.hpp>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <array>
#include <tuple>

namespace SunEngine {

    namespace fs = std::filesystem;

    enum CodexCategory { kCodexOverview = 0, kCodexUnits, kCodexOthers, kCodexRanged, kCodexMelee, kCodexEquipment, kCodexArmor, kCodexIdeas, kCodexCount };
    static const char* kCodexCategoryNames[kCodexCount] = { "Panoramica", "Unita'", "Nemici, elite e storia", "Armi da fuoco", "Armi bianche", "Equipaggiamento", "Armature", "Idee e progetti" };

    struct GameLayer::CodexEntry {
        std::string id, name, faction, comment, kind;   // kind: "unit", "weapon", "armor", "idea" (unita' non ancora giocabile), "hierarchy"
        int category = kCodexUnits;
        nlohmann::json j;
        bool hasModel = false;                          // armi: c'e' la mesh in Models/Weapons
    };

    struct GameLayer::CodexData {
        nlohmann::json labels;
        std::vector<CodexEntry> entries;
        std::unordered_map<std::string, size_t> byId;
        std::unordered_map<std::string, std::unique_ptr<GpuTexture>> images;
        std::set<std::string> noImage;
    };

    namespace {
        const char* kDataDir = "GameData/";

        std::string FactionFromUnitId(const std::string& id) {
            if (id.rfind("kingship_", 0) == 0) return "Kingship";
            if (id.rfind("vanguard_", 0) == 0) return "Vanguard";
            if (id.rfind("vrepighs_", 0) == 0) return "Vrepighs";
            return "";
        }

        ImVec4 FactionColor(const std::string& f) {
            if (f == "Kingship") return ImVec4(0.62f, 0.66f, 0.38f, 1.0f);
            if (f == "Vanguard") return ImVec4(0.80f, 0.28f, 0.22f, 1.0f);
            if (f == "Vrepighs") return ImVec4(0.80f, 0.47f, 0.24f, 1.0f);
            return Ui::Col::Ash;
        }

        std::string Trim(double v) {
            char b[32];
            if (std::fabs(v - std::round(v)) < 1e-6) std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(std::llround(v)));
            else {
                std::snprintf(b, sizeof(b), "%.2f", v);
                std::string s(b);
                while (!s.empty() && s.back() == '0') s.pop_back();
                if (!s.empty() && s.back() == '.') s.pop_back();
                return s;
            }
            return b;
        }

        bool LoadJson(const fs::path& p, nlohmann::json& out) {
            std::ifstream f(p);
            if (!f) return false;
            try { out = nlohmann::json::parse(f, nullptr, true, true); return true; }
            catch (...) { return false; }
        }
    }

    // ------------------------------------------------------------------------------------------ dati
    void GameLayer::LoadCodex() {
        auto data = std::make_shared<CodexData>();
        LoadJson(fs::path(kDataDir) / "Codex" / "labels.json", data->labels);
        auto add = [&](CodexEntry&& e) { data->byId[e.id] = data->entries.size(); data->entries.push_back(std::move(e)); };

        // unita'
        std::error_code ec;
        for (const auto& it : fs::directory_iterator(fs::path(kDataDir) / "Units", ec)) {
            if (it.path().extension() != ".json") continue;
            CodexEntry e; e.kind = "unit";
            if (!LoadJson(it.path(), e.j) || !e.j.is_object()) continue;
            e.id = e.j.value("id", it.path().stem().string());
            e.name = e.j.value("displayName", e.id);
            e.comment = e.j.value("comment", std::string());
            e.faction = FactionFromUnitId(e.id);
            const ResearchNode* n = m_ResearchTree.FindNodeByUnit(e.id);
            e.category = (n && !n->premium) ? kCodexUnits : kCodexOthers;
            add(std::move(e));
        }
        // armi ed equipaggiamento (la cartella dice il tipo)
        for (const auto& dir : fs::directory_iterator(fs::path(kDataDir) / "Weapons", ec)) {
            if (!dir.is_directory()) continue;
            const std::string folder = dir.path().filename().string();
            for (const auto& it : fs::directory_iterator(dir.path(), ec)) {
                if (it.path().extension() != ".json") continue;
                CodexEntry e; e.kind = "weapon";
                if (!LoadJson(it.path(), e.j) || !e.j.is_object()) continue;
                e.id = e.j.value("id", it.path().stem().string());
                e.name = e.j.value("displayName", e.id);
                e.comment = e.j.value("comment", std::string());
                e.faction = e.j.value("faction", std::string());
                e.category = folder == "Equipment" ? kCodexEquipment : (folder.rfind("Melee", 0) == 0 ? kCodexMelee : kCodexRanged);
                e.j["_folder"] = folder;
                e.hasModel = fs::exists(fs::path(kDataDir) / "Models" / "Weapons" / (e.id + ".glb"), ec);
                add(std::move(e));
            }
        }
        // armature
        for (const auto& it : fs::recursive_directory_iterator(fs::path(kDataDir) / "Armors", ec)) {
            if (it.path().extension() != ".json") continue;
            CodexEntry e; e.kind = "armor"; e.category = kCodexArmor;
            if (!LoadJson(it.path(), e.j) || !e.j.is_object()) continue;
            e.id = e.j.value("id", it.path().stem().string());
            e.name = e.j.value("displayName", e.id);
            e.comment = e.j.value("comment", std::string());
            e.faction = e.j.value("faction", std::string());
            add(std::move(e));
        }
        // idee: le unita' dell'albero di ricerca che non esistono ancora (anche oltre il grado mostrato) e le gerarchie
        nlohmann::json tree;
        if (LoadJson(fs::path(kDataDir) / "Research" / "research_tree.json", tree) && tree.contains("factions")) {
            const int maxTier = tree.value("max_tier", 0);
            for (const auto& jf : tree["factions"]) {
                const std::string fid = jf.value("id", std::string());
                const std::string fac = fid == "kingship" ? "Kingship" : fid == "vanguard" ? "Vanguard" : fid == "vrepighs" ? "Vrepighs" : fid;
                std::unordered_map<std::string, std::string> names;
                for (const auto& jn : jf.value("nodes", nlohmann::json::array())) names[jn.value("id", std::string())] = jn.value("name", std::string());
                for (const auto& jn : jf.value("nodes", nlohmann::json::array())) {
                    const std::string uid = jn.value("unit", std::string());
                    if (uid.empty() || BlueprintManager::Get().GetUnit(uid)) continue;
                    CodexEntry e; e.kind = "idea"; e.category = kCodexIdeas; e.faction = fac;
                    e.id = uid; e.name = jn.value("name", uid); e.comment = jn.value("description", std::string());
                    e.j = jn;
                    e.j["_parent_name"] = names[jn.value("parent", std::string())];
                    e.j["_hidden"] = maxTier > 0 && jn.value("tier", 1) > maxTier;
                    add(std::move(e));
                }
                for (const auto& js : jf.value("sections", nlohmann::json::array())) {
                    CodexEntry e; e.kind = "hierarchy"; e.category = kCodexIdeas; e.faction = fac;
                    e.id = "gerarchia_" + js.value("id", std::string());
                    std::string nm = js.value("name", e.id);
                    e.name = "Gerarchia: " + nm;
                    e.comment = js.value("intro", std::string());
                    e.j = js;
                    add(std::move(e));
                }
            }
        }
        // in ordine di nome; le idee: prima le gerarchie, poi per grado
        auto key = [](const CodexEntry& e) { return std::make_tuple(e.kind == "hierarchy" ? 0 : 1, e.kind == "idea" ? e.j.value("tier", 0) : 0, e.name); };
        std::sort(data->entries.begin(), data->entries.end(), [&](const CodexEntry& a, const CodexEntry& b) { return key(a) < key(b); });
        data->byId.clear();
        for (size_t i = 0; i < data->entries.size(); ++i) data->byId[data->entries[i].id] = i;
        m_Codex = data;
        SUN_CORE_INFO("[codex] {} schede", m_Codex->entries.size());
    }

    unsigned int GameLayer::CodexImage(const std::string& id) {
        if (!m_Codex || m_Codex->noImage.count(id)) return 0;
        auto it = m_Codex->images.find(id);
        if (it != m_Codex->images.end()) return it->second->GetID();
        const fs::path p = fs::path(kDataDir) / "Codex" / "img" / (id + ".jpg");
        std::error_code ec;
        auto tex = std::make_unique<GpuTexture>();
        if (!fs::exists(p, ec) || !tex->LoadFromFile(p.string(), false, false)) { m_Codex->noImage.insert(id); return 0; }
        const unsigned int gl = tex->GetID();
        m_Codex->images[id] = std::move(tex);
        return gl;
    }

    // ------------------------------------------------------------------------------------------ foto delle unita'
    void GameLayer::UpdateCodexShots() {
        static bool s_Init = false;
        if (!s_Init) {
            s_Init = true;
            if (const char* d = std::getenv("SUN_CODEX_SHOTS")) {
                m_CodexShotDir = d;
                std::error_code ec; fs::create_directories(m_CodexShotDir, ec);
                for (const auto& kv : BlueprintManager::Get().GetUnits()) m_CodexShotQueue.push_back(kv.first);
                std::sort(m_CodexShotQueue.begin(), m_CodexShotQueue.end());
                SUN_CORE_INFO("[codex] foto di {} unita' in {}", m_CodexShotQueue.size(), m_CodexShotDir);
            }
        }
        if (m_CodexShotDir.empty()) return;
        if (m_CodexShotQueue.empty()) {
            if (GLFWwindow* w = glfwGetCurrentContext()) glfwSetWindowShouldClose(w, 1);
            return;
        }
        // in vetrina, fermo, di tre quarti
        m_GameState = GameState::MainMenu;
        m_CurrentTab = MenuTab::Showcase;
        m_ShowcaseFocusUnit = m_CodexShotQueue.front();
        m_ShowcaseFromResearch = false;
        m_ShowcaseAnim = 0;
        m_ShowcaseCycle = false;
        m_ShowcaseAutoRotate = false;
        m_ShowcaseYaw = -38.0f;
        m_ShowcasePitch = -7.0f;
        m_ShowcaseZoom = 0.92f;
        if (++m_CodexShotFrame >= 45) m_CodexShotNow = true;   // il tempo di caricare corpo, vestiti e arma
    }

    void GameLayer::CaptureCodexShot() {
        GLFWwindow* win = glfwGetCurrentContext();
        int w = 0, h = 0;
        if (win) glfwGetFramebufferSize(win, &w, &h);
        if (w > 0 && h > 0 && !m_CodexShotQueue.empty()) {
            std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            const std::string path = (fs::path(m_CodexShotDir) / (m_CodexShotQueue.front() + ".ppm")).string();
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int y = h - 1; y >= 0; --y) std::fwrite(px.data() + static_cast<size_t>(y) * w * 3, 1, static_cast<size_t>(w) * 3, f);   // dal basso all'alto
                std::fclose(f);
                SUN_CORE_INFO("[codex] foto {}", path);
            }
        }
        if (!m_CodexShotQueue.empty()) m_CodexShotQueue.erase(m_CodexShotQueue.begin());
        m_CodexShotFrame = 0;
        m_CodexShotNow = false;
    }

    // ------------------------------------------------------------------------------------------ interfaccia
    namespace {
        struct CodexFmt {
            const nlohmann::json& L;
            std::string Label(const std::string& k) const {
                if (L.contains("labels") && L["labels"].contains(k)) return L["labels"][k][0].get<std::string>();
                return k;
            }
            std::string Unit(const std::string& k) const {
                if (L.contains("labels") && L["labels"].contains(k)) return L["labels"][k][1].get<std::string>();
                return "";
            }
            bool Percent(const std::string& k) const {
                if (!L.contains("percent")) return false;
                for (const auto& p : L["percent"]) if (p == k) return true;
                return false;
            }
            std::string Word(const std::string& k, const std::string& v) const {
                if (L.contains("values") && L["values"].contains(k) && L["values"][k].contains(v)) return L["values"][k][v].get<std::string>();
                return v;
            }
            std::string Faction(const std::string& f) const {
                if (L.contains("factions") && L["factions"].contains(f)) return L["factions"][f].get<std::string>();
                return f.empty() ? "-" : f;
            }
            std::string Value(const std::string& k, const nlohmann::json& v) const {
                if (v.is_boolean()) return v.get<bool>() ? "si'" : "no";
                if (v.is_number()) {
                    const double d = v.get<double>();
                    if (Percent(k)) return Trim(std::round(d * 1000.0) / 10.0) + "%";
                    const std::string u = Unit(k);
                    return Trim(d) + (u.empty() ? "" : " " + u);
                }
                if (v.is_string()) return Word(k, v.get<std::string>());
                if (v.is_array()) {
                    std::string s;
                    for (const auto& x : v) {
                        if (!s.empty()) s += ", ";
                        s += x.is_string() ? Word(k, x.get<std::string>()) : (x.is_number() ? Trim(x.get<double>()) : x.dump());
                    }
                    return s;
                }
                return "";
            }
        };

        void Row(const char* a, const std::string& b, const ImVec4& col = Ui::Col::Bone) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextColored(Ui::Col::Ash, "%s", a);
            ImGui::TableSetColumnIndex(1); ImGui::TextColored(col, "%s", b.c_str());
        }
        bool BeginStats(const char* id) {
            return ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp);
        }

        const std::set<std::string> kSkip = { "id", "displayName", "comment", "faction", "allowed_units", "unit_restriction", "is_melee",
            "moveset", "primary_color", "core_color", "weapon_scale", "hold_offset", "barrel_offset", "skin_parts", "skin_blocks", "garments",
            "default_loadout", "base_skin", "skeleton", "loadout_version", "point_cost", "weight_kg", "slot_type", "hand_type", "_folder",
            "projectile_length", "projectile_width", "repair_tags", "weightCapacity",
            "damage_types", "damage_pure", "damage_fx", "resistances", "resist_pure" };
    }

    void GameLayer::RenderCodexUI() {
        if (!m_Codex) LoadCodex();
        CodexData& cd = *m_Codex;
        CodexFmt F{ cd.labels };
        auto& bpm = BlueprintManager::Get();
        ImGuiIO& io = ImGui::GetIO();

        ImGui::SetNextWindowPos(ImVec2(20.0f, 80.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - 40.0f, io.DisplaySize.y - 100.0f));
        ImGui::Begin("CODEX", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

        int counts[kCodexCount] = {};
        for (const auto& e : cd.entries) counts[e.category]++;
        counts[kCodexOverview] = static_cast<int>(cd.entries.size());

        // --- categorie
        ImGui::BeginChild("##codex_cat", ImVec2(210.0f, 0.0f), true);
        Ui::Heading("CODEX");
        for (int c = 0; c < kCodexCount; ++c) {
            char lab[96];
            std::snprintf(lab, sizeof(lab), "%s  (%d)", kCodexCategoryNames[c], counts[c]);
            if (ImGui::Selectable(lab, m_CodexCategory == c)) { m_CodexCategory = c; m_CodexSelected.clear(); }
        }
        ImGui::Spacing(); Ui::Divider(); ImGui::Spacing();
        ImGui::TextWrapped("Tutto quello che c'e' nel gioco, letto dai file di dati: si aggiorna da solo quando cambiano.");
        if (Ui::PlainButton("Ricarica i dati", ImVec2(-1, 28))) { m_Codex.reset(); ImGui::EndChild(); ImGui::End(); return; }
        ImGui::EndChild();
        ImGui::SameLine();

        // --- panoramica
        if (m_CodexCategory == kCodexOverview) {
            ImGui::BeginChild("##codex_over", ImVec2(0.0f, 0.0f), false);
            Ui::Title("A che punto siamo");
            ImGui::Spacing();
            const float tileW = 128.0f;
            for (int c = 1; c < kCodexCount; ++c) {
                if (c > 1) ImGui::SameLine();
                Ui::StatTile(kCodexCategoryNames[c], std::to_string(counts[c]).c_str(), tileW, Ui::Col::Gold);
            }
            ImGui::Spacing(); Ui::Divider(); ImGui::Spacing();
            // per fazione
            std::map<std::string, std::array<int, kCodexCount>> perFaction;
            for (const auto& e : cd.entries) perFaction[e.faction.empty() ? "-" : e.faction][e.category]++;
            if (ImGui::BeginTable("##fac", kCodexCount, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn("Fazione");
                for (int c = 1; c < kCodexCount; ++c) ImGui::TableSetupColumn(kCodexCategoryNames[c]);
                ImGui::TableHeadersRow();
                for (const auto& kv : perFaction) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextColored(FactionColor(kv.first), "%s", F.Faction(kv.first).c_str());
                    for (int c = 1; c < kCodexCount; ++c) { ImGui::TableSetColumnIndex(c); ImGui::Text("%d", kv.second[c]); }
                }
                ImGui::EndTable();
            }
            ImGui::Spacing(); Ui::Heading("DA FARE");
            std::vector<std::string> noModel, nobodyW, nobodyA, noPhoto, ideas;
            for (const auto& e : cd.entries) {
                if (e.kind == "weapon") {
                    if (!e.hasModel && e.id.find("claws") == std::string::npos) noModel.push_back(e.name);   // gli artigli sono il corpo
                    const auto* w = bpm.GetWeapon(e.id);
                    bool any = false;
                    if (w) for (const auto& u : bpm.GetUnits()) if (bpm.CanEquip(*w, u.first)) { any = true; break; }
                    if (!any) nobodyW.push_back(e.name);
                }
                else if (e.kind == "armor") {
                    const auto* a = bpm.GetArmorPiece(e.id);
                    bool any = false;
                    if (a) for (const auto& u : bpm.GetUnits()) if (bpm.CanEquip(*a, u.first)) { any = true; break; }
                    if (!any) nobodyA.push_back(e.name);
                }
                else if (e.kind == "unit" && !CodexImage(e.id)) noPhoto.push_back(e.name);
                else if (e.kind == "idea") ideas.push_back(e.name);
            }
            auto list = [](const char* title, const std::vector<std::string>& v) {
                ImGui::TextColored(v.empty() ? Ui::Col::Verdigris : Ui::Col::Rust, "%s: %d", title, static_cast<int>(v.size()));
                if (v.empty()) return;
                std::string s;
                for (const auto& x : v) { if (!s.empty()) s += ", "; s += x; }
                ImGui::Indent(14.0f); ImGui::PushStyleColor(ImGuiCol_Text, Ui::Col::Ash); ImGui::TextWrapped("%s", s.c_str()); ImGui::PopStyleColor(); ImGui::Unindent(14.0f);
            };
            list("Armi e attrezzi senza modello 3D (si vedono a cubi)", noModel);
            list("Armi e attrezzi che nessuna unita' puo' usare", nobodyW);
            list("Armature che nessuna unita' puo' indossare", nobodyA);
            list("Unita' senza foto nel Codex", noPhoto);
            ImGui::TextColored(Ui::Col::Phosphor, "Unita' in progetto (nell'albero, non ancora giocabili): %d", static_cast<int>(ideas.size()));
            // rarita' degli oggetti (le regole sono in ItemRarity.h; qui le stesse cose che mostra il magazzino)
            ImGui::Spacing(); Ui::Heading("RARITA' DEGLI OGGETTI");
            for (int r = 0; r < 5; ++r) {
                const RarityInfo& ri = Rarity(r);
                ImGui::TextColored(ri.color, "%s", ri.name); ImGui::SameLine(150.0f);
                ImGui::Text("statistiche x%.2f%s", ri.statMult, ri.traits ? ("   + " + std::to_string(ri.traits) + (ri.traits == 1 ? " tratto" : " tratti")).c_str() : "");
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Ui::Col::Ash, "Nuovi = comuni. Si potenziano nel MAGAZZINO, si trovano come bottino nella PVE-STORY e, piu' avanti, si prendono agli avversari. "
                                             "Si consumano con l'uso (colpi sparati e in mischia, danni presi dalle armature) e morendo con l'oggetto addosso (5v5, sopravvivenza); a zero si rompono. %d tratti possibili.", [] {
                                                 std::set<std::string> u; for (const auto& t : ItemTraits()) u.insert(t.code); return static_cast<int>(u.size()); }());
            ImGui::PopTextWrapPos();
            // tipi di danno (DamageTypes.h)
            ImGui::Spacing(); Ui::Heading("TIPI DI DANNO");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Ui::Col::Ash, "Ogni arma divide il danno fra uno o piu' tipi; ogni armatura ha una difesa per tipo, che riduce quella parte "
                                             "del colpo (se il pezzo copre e regge). I tratti delle armi possono aggiungere un tipo (+20%% del danno) o "
                                             "rinforzarne uno (+15%%); quelli delle armature aggiungere una difesa (+15%%) o rinforzarla (+10%%). "
                                             "Le armi e le armature \"pure\" possono solo rinforzare quello che hanno. I tipi non fisici si vedono sul modello dell'arma.");
            ImGui::PopTextWrapPos();
            if (ImGui::BeginTable("##dmgtypes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Tipo"); ImGui::TableSetupColumn("Gruppo"); ImGui::TableSetupColumn("Chi lo fa"); ImGui::TableSetupColumn("Sull'arma");
                ImGui::TableHeadersRow();
                for (int t = 0; t < kDamageTypeCount; ++t) {
                    const DamageTypeInfo& d = DamageInfo(t);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextColored(DamageUi::Col(t), "%s", d.name);
                    ImGui::TableSetColumnIndex(1); ImGui::TextColored(Ui::Col::Ash, "%s", d.group);
                    ImGui::TableSetColumnIndex(2); ImGui::TextWrapped("%s", d.desc);
                    ImGui::TableSetColumnIndex(3); ImGui::TextColored(Ui::Col::Ash, "%s", d.fx[0] ? d.fx : "-");
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
            ImGui::End();
            return;
        }

        // --- elenco
        ImGui::BeginChild("##codex_list", ImVec2(300.0f, 0.0f), true);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##codex_q", "Cerca per nome o id", m_CodexSearch, sizeof(m_CodexSearch));
        std::string q = m_CodexSearch;
        std::transform(q.begin(), q.end(), q.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const CodexEntry* sel = nullptr;
        std::string lastFaction = "#";
        std::vector<const CodexEntry*> shown;
        for (const auto& e : cd.entries) {
            if (e.category != m_CodexCategory) continue;
            std::string hay = e.name + " " + e.id;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (!q.empty() && hay.find(q) == std::string::npos) continue;
            shown.push_back(&e);
        }
        std::stable_sort(shown.begin(), shown.end(), [](const CodexEntry* a, const CodexEntry* b) { return a->faction < b->faction; });
        for (const CodexEntry* e : shown) {
            if (e->faction != lastFaction) { lastFaction = e->faction; ImGui::Spacing(); ImGui::TextColored(FactionColor(e->faction), "%s", F.Faction(e->faction).c_str()); }
            ImGui::PushID(e->id.c_str());
            if (ImGui::Selectable(e->name.c_str(), m_CodexSelected == e->id)) m_CodexSelected = e->id;
            ImGui::PopID();
        }
        if (shown.empty()) ImGui::TextColored(Ui::Col::Ash, "Niente con questo nome.");
        if (m_CodexSelected.empty() && !shown.empty()) m_CodexSelected = shown.front()->id;
        if (auto it = cd.byId.find(m_CodexSelected); it != cd.byId.end()) sel = &cd.entries[it->second];
        ImGui::EndChild();
        ImGui::SameLine();

        // --- scheda
        ImGui::BeginChild("##codex_card", ImVec2(0.0f, 0.0f), false);
        if (!sel) { ImGui::TextColored(Ui::Col::Ash, "Scegli una voce a sinistra."); ImGui::EndChild(); ImGui::End(); return; }
        const CodexEntry& e = *sel;
        const nlohmann::json& j = e.j;

        Ui::Title(e.name.c_str());
        ImGui::TextColored(Ui::Col::Ash, "%s", e.id.c_str());
        ImGui::SameLine(); Ui::Chip(F.Faction(e.faction).c_str(), FactionColor(e.faction));
        ImGui::Spacing();

        if (e.kind == "hierarchy") {   // gerarchia: caste in colonne, gradi dall'alto
            if (!e.comment.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::TextColored(Ui::Col::Ash, "%s", e.comment.c_str()); ImGui::PopTextWrapPos(); }
            const auto divs = j.value("divisions", nlohmann::json::array());
            if (!divs.empty() && ImGui::BeginTable("##gerarchia", static_cast<int>(divs.size()), ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
                for (const auto& d : divs) ImGui::TableSetupColumn(d.value("name", std::string()).c_str());
                ImGui::TableNextRow();
                int c = 0;
                for (const auto& d : divs) {
                    ImGui::TableSetColumnIndex(c++);
                    const auto col = d.value("color", std::vector<float>{ 0.8f, 0.8f, 0.8f });
                    const ImVec4 dc(col[0], col[1], col[2], 1.0f);
                    ImGui::TextColored(dc, "%s", d.value("name", std::string()).c_str());
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextColored(Ui::Col::Ash, "%s", d.value("role", std::string()).c_str());
                    for (const auto& r : d.value("ranks", nlohmann::json::array())) {
                        ImGui::Spacing();
                        ImGui::TextColored(dc, "%s  -  grado %s", r.value("title", std::string()).c_str(), Ui::Roman(r.value("tier", 1)));
                        if (!r.value("text", std::string()).empty()) ImGui::TextColored(Ui::Col::Bone, "%s", r.value("text", std::string()).c_str());
                        if (!r.value("member", std::string()).empty()) ImGui::TextColored(Ui::Col::Gold, "%s", r.value("member", std::string()).c_str());
                    }
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndTable();
            }
            if (!j.value("footer", std::string()).empty()) {
                ImGui::Spacing(); Ui::Heading(j.value("footer_title", std::string("NOTA")).c_str());
                ImGui::PushTextWrapPos(0.0f); ImGui::TextColored(Ui::Col::Ash, "%s", j.value("footer", std::string()).c_str()); ImGui::PopTextWrapPos();
            }
            ImGui::EndChild(); ImGui::End();
            return;
        }
        if (e.kind == "idea") {   // unita' in progetto: cosa sara', dove sta nell'albero
            ImGui::TextColored(Ui::Col::Rust, "IN PROGETTO - non ancora giocabile");
            ImGui::Spacing();
            if (!e.comment.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::TextColored(Ui::Col::Bone, "%s", e.comment.c_str()); ImGui::PopTextWrapPos(); }
            ImGui::Spacing(); Ui::Heading("NELL'ALBERO DI RICERCA");
            if (BeginStats("##idea")) {
                Row("Fazione", F.Faction(e.faction), FactionColor(e.faction));
                Row("Grado", Ui::Roman(j.value("tier", 1)));
                if (!j.value("caste", std::string()).empty()) Row("Casta", j.value("caste", std::string()));
                if (!j.value("_parent_name", std::string()).empty()) Row("Si ricerca da", j.value("_parent_name", std::string()));
                if (j.value("research_cost", 0) > 0) Row("Ricerca", Trim(j.value("research_cost", 0.0)) + " punti studio");
                if (j.value("premium", false)) Row("Unita' d'elite", "si'", Ui::Col::Amethyst);
                Row("Nell'albero", j.value("_hidden", false) ? "nascosta: oltre il grado mostrato" : "visibile, \"in arrivo\"", j.value("_hidden", false) ? Ui::Col::Ash : Ui::Col::Gold);
                Row("Nome del primo", j.value("default_name", std::string("-")));
                ImGui::EndTable();
            }
            ImGui::EndChild(); ImGui::End();
            return;
        }

        const float avail = ImGui::GetContentRegionAvail().x;
        const float imgW = std::min(460.0f, avail * 0.48f), imgH = imgW * 416.0f / 720.0f;
        ImGui::BeginGroup();
        if (const unsigned int tex = CodexImage(e.id)) ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(imgW, imgH));
        else {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + imgW, p.y + imgH), Ui::U32(Ui::Col::Iron));
            const char* why = e.kind == "armor" ? "Le armature si vedono addosso alle unita'" : (e.kind == "unit" ? "Foto non ancora fatta" : "Nessun modello 3D");
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 14.0f, p.y + imgH * 0.5f - 8.0f), Ui::U32(Ui::Col::Ash), why);
            ImGui::Dummy(ImVec2(imgW, imgH));
        }
        if (e.kind == "unit" && Ui::Button("VEDI IN 3D", ImVec2(imgW, 34.0f))) {
            OpenShowcasePreview(e.id);
            m_ShowcaseFromResearch = false;
            m_ShowcaseFromCodex = true;
        }
        if (!e.comment.empty()) { ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + imgW); ImGui::TextColored(Ui::Col::Ash, "%s", e.comment.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::EndGroup();
        ImGui::SameLine();

        ImGui::BeginChild("##codex_stats", ImVec2(0.0f, 0.0f), false);
        std::set<std::string> used;
        // scheda generale
        Ui::Heading("SCHEDA");
        if (BeginStats("##s")) {
            Row("Fazione", F.Faction(e.faction), FactionColor(e.faction));
            Row("Tipo", kCodexCategoryNames[e.category]);
            if (j.contains("point_cost")) Row("Costo in punti", Trim(j["point_cost"].get<double>()));
            if (j.contains("weight_kg")) Row("Peso", Trim(j["weight_kg"].get<double>()) + " kg");
            if (j.contains("hand_type")) Row("Mani", j["hand_type"] == "TwoHanded" ? "due" : "una");
            if (e.kind == "weapon" && !j.value("is_melee", false) && j.contains("rate_of_fire") && j.contains("base_damage")) {
                const double dps = j["base_damage"].get<double>() * j.value("pellet_count", 1) * j["rate_of_fire"].get<double>() / 60.0;
                if (dps > 0.0) Row("Danno al secondo (calcolato)", Trim(std::round(dps)), Ui::Col::Gold);
                if (j.contains("magazine_size") && j.contains("spare_magazines"))
                    Row("Colpi in tutto", Trim(j["magazine_size"].get<double>() * (1.0 + j["spare_magazines"].get<double>())));
            }
            if (e.kind == "weapon" && j.contains("moveset") && j["moveset"].contains("nodes")) {
                double mx = 1.0; int n = 0;
                for (const auto& kv : j["moveset"]["nodes"].items()) { ++n; mx = std::max(mx, kv.value().value("damage_multiplier", 1.0)); }
                Row("Attacchi nella combo", std::to_string(n));
                Row("Colpo piu' forte", "x" + Trim(mx) + " (" + Trim(std::round(j.value("base_damage", 0.0) * mx)) + ")", Ui::Col::Gold);
            }
            // ricerca
            const ResearchNode* n = e.kind == "unit" ? m_ResearchTree.FindNodeByUnit(e.id) : m_ResearchTree.FindNodeByItem(e.id);
            if (n) {
                Row("Grado", Ui::Roman(n->tier));
                if (n->premium) Row("Unita' d'elite", Trim(static_cast<double>(n->premiumCost)) + " Vanguardio", Ui::Col::Amethyst);
                else {
                    if (n->researchCost > 0) Row("Ricerca", Trim(static_cast<double>(n->researchCost)) + " punti studio");
                    if (n->purchaseCost > 0) Row(e.kind == "unit" ? "Reclutamento" : "Acquisto", Trim(static_cast<double>(n->purchaseCost)) + " energia");
                    if (const ResearchNode* p = m_ResearchTree.FindParent(*n)) Row("Si sblocca da", p->name);
                }
            }
            else if (e.kind == "unit") Row("Ricerca", "non si ricerca (nemico o personaggio delle storie)", Ui::Col::Rust);
            ImGui::EndTable();
        }
        // tipi di danno / difese (dal blueprint: anche quelli dedotti)
        if (e.kind == "weapon") {
            if (const auto* w = bpm.GetWeapon(e.id); w && (w->base_damage > 0.0f || w->explosion_damage > 0.0f)) {
                ImGui::Spacing(); Ui::Heading("TIPO DI DANNO");
                DamageUi::SplitBar(w->damage, std::min(360.0f, ImGui::GetContentRegionAvail().x), w->damage_pure ? "(puro)" : nullptr);
                ImGui::TextColored(Ui::Col::Ash, w->damage_pure ? "Arma pura: i tratti possono solo rinforzare il suo tipo." : "I tratti possono aggiungere altri tipi o rinforzare questi.");
            }
        }
        else if (e.kind == "armor") {
            if (const auto* a = bpm.GetArmorPiece(e.id)) {
                ImGui::Spacing(); Ui::Heading("DIFESE");
                DamageUi::ResistRows(a->resist, 140.0f);
                if (a->resist_pure) ImGui::TextColored(Ui::Col::Ash, "Difese pure: i tratti possono solo rinforzare quelle che ci sono.");
            }
        }
        // gruppi di statistiche
        const std::string gk = e.kind;
        if (cd.labels.contains("groups") && cd.labels["groups"].contains(gk)) {
            for (const auto& g : cd.labels["groups"][gk]) {
                std::vector<std::pair<std::string, std::string>> rows;
                for (const auto& kk : g[1]) {
                    const std::string k = kk.get<std::string>();
                    if (!j.contains(k)) continue;
                    used.insert(k);
                    const std::string v = F.Value(k, j[k]);
                    if (!v.empty()) rows.emplace_back(F.Label(k), v);
                }
                if (rows.empty()) continue;
                ImGui::Spacing(); Ui::Heading(g[0].get<std::string>().c_str());
                if (BeginStats(g[0].get<std::string>().c_str())) { for (const auto& r : rows) Row(r.first.c_str(), r.second); ImGui::EndTable(); }
            }
        }
        {   // il resto del file
            std::vector<std::pair<std::string, std::string>> rows;
            for (const auto& kv : j.items()) {
                if (used.count(kv.key()) || kSkip.count(kv.key()) || kv.value().is_object()) continue;
                const std::string v = F.Value(kv.key(), kv.value());
                if (!v.empty()) rows.emplace_back(F.Label(kv.key()), v);
            }
            if (!rows.empty()) {
                ImGui::Spacing(); Ui::Heading("ALTRO");
                if (BeginStats("##altro")) { for (const auto& r : rows) Row(r.first.c_str(), r.second); ImGui::EndTable(); }
            }
        }

        // chi la usa / cosa usa
        auto names = [](const std::vector<std::string>& v) { std::string s; for (const auto& x : v) { if (!s.empty()) s += ", "; s += x; } return s; };
        if (e.kind == "weapon" || e.kind == "armor") {
            std::vector<std::string> base, other;
            for (const auto& kv : bpm.GetUnits()) {
                const UnitBlueprint& u = kv.second;
                bool can = false, isBase = false;
                if (e.kind == "weapon") {
                    if (const auto* w = bpm.GetWeapon(e.id)) can = bpm.CanEquip(*w, u.id);
                    isBase = u.default_primary == e.id || u.default_secondary == e.id || u.default_melee == e.id ||
                             u.default_equipment[0] == e.id || u.default_equipment[1] == e.id || u.default_equipment[2] == e.id;
                }
                else {
                    if (const auto* a = bpm.GetArmorPiece(e.id)) can = bpm.CanEquip(*a, u.id);
                    for (const auto& d : u.default_armor) if (d == e.id) isBase = true;
                }
                if (isBase) base.push_back(u.displayName);
                else if (can) other.push_back(u.displayName);
            }
            std::sort(base.begin(), base.end()); std::sort(other.begin(), other.end());
            ImGui::Spacing(); Ui::Heading("CHI LA USA");
            ImGui::TextColored(Ui::Col::Gold, "Dotazione base:"); ImGui::SameLine(); ImGui::TextWrapped("%s", base.empty() ? "nessuno" : names(base).c_str());
            ImGui::TextColored(Ui::Col::Brass, "Puo' sceglierla:"); ImGui::SameLine(); ImGui::TextWrapped("%s", other.empty() ? "nessun altro" : names(other).c_str());
        }
        if (e.kind == "unit") {
            if (const UnitBlueprint* u = bpm.GetUnit(e.id)) {
                auto wname = [&](const std::string& id) { const auto* w = bpm.GetWeapon(id); return w ? w->displayName : id; };
                auto aname = [&](const std::string& id) { const auto* a = bpm.GetArmorPiece(id); return a ? a->displayName : id; };
                ImGui::Spacing(); Ui::Heading("DOTAZIONE BASE");
                if (BeginStats("##dot")) {
                    if (!u->default_primary.empty()) Row("Primaria", wname(u->default_primary));
                    if (!u->default_secondary.empty()) Row("Secondaria", wname(u->default_secondary));
                    if (!u->default_melee.empty()) Row("Mischia", wname(u->default_melee));
                    for (const auto& eq : u->default_equipment) if (!eq.empty()) Row("Equipaggiamento", wname(eq));
                    static const char* kParts[6] = { "Testa", "Torso", "Braccio sinistro", "Braccio destro", "Gamba sinistra", "Gamba destra" };
                    for (int i = 0; i < 6; ++i) if (!u->default_armor[i].empty()) Row(kParts[i], aname(u->default_armor[i]));
                    ImGui::EndTable();
                }
                std::vector<std::string> fire, melee, equip, armor;
                for (const auto& kv : bpm.GetWeapons()) {
                    if (!bpm.CanEquip(kv.second, u->id)) continue;
                    auto ce = cd.byId.find(kv.first);
                    const int cat = ce != cd.byId.end() ? cd.entries[ce->second].category : kCodexRanged;
                    (cat == kCodexEquipment ? equip : (cat == kCodexMelee ? melee : fire)).push_back(kv.second.displayName);
                }
                for (const auto& kv : bpm.GetArmorPieces()) if (bpm.CanEquip(kv.second, u->id)) armor.push_back(kv.second.displayName);
                for (auto* v : { &fire, &melee, &equip, &armor }) std::sort(v->begin(), v->end());
                ImGui::Spacing(); Ui::Heading("PUO' USARE");
                ImGui::TextColored(Ui::Col::Brass, "Armi da fuoco (%d):", static_cast<int>(fire.size())); ImGui::TextWrapped("%s", fire.empty() ? "-" : names(fire).c_str());
                ImGui::TextColored(Ui::Col::Brass, "Armi bianche (%d):", static_cast<int>(melee.size())); ImGui::TextWrapped("%s", melee.empty() ? "-" : names(melee).c_str());
                ImGui::TextColored(Ui::Col::Brass, "Equipaggiamento (%d):", static_cast<int>(equip.size())); ImGui::TextWrapped("%s", equip.empty() ? "-" : names(equip).c_str());
                ImGui::TextColored(Ui::Col::Brass, "Armature (%d):", static_cast<int>(armor.size())); ImGui::TextWrapped("%s", armor.empty() ? "-" : names(armor).c_str());
            }
        }
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::End();
    }
}
