// PVE-STORY: le storie di The Sun's Darkest Hours da rigiocare, fuori dal sistema delle squadre.
// Ogni capitolo si gioca con un personaggio fisso della storia, sulla sua mappa, con il suo obiettivo.
//
// I. Primo Incontro (Anno 999 del Calendario Regale, Vespera IV, Terza Serra), mappa "warzone_sector4":
//   0  Martus Kell      fuga dalla serra centrale (A) al magazzino nord (B): arrivarci vivo
//   1  Sergente Ordan   rifugio del livello tre, stanza chiusa: ondate dall'anticamera fino alla fine (record)
//   2  Konrad von Eber  ondate nei campi; dopo 5 minuti arriva il capo con la collana di frammenti: abbatterlo
// II. Il peso delle pietre (Anno 999, il territorio di Gorvakh e poi Vespera IV):
//   0  Kragh            duello nella sala dell'altare ("caverna_gorvakh"), solo artigli: Gorvakh sotto il 50%
//   1  Kragh            sbarco nella Terza Serra con il vargh: distruggere i tre punti di raccolta dei glatt
//                        (una cinquantina di difensori di grado II, 5-7 di guardia a ogni punto)
//   2  Il Vecchio       con tre guerrieri del vargh contro Konrad e i suoi due compagni: reggere 2 minuti
//
// Il garage del giocatore viene messo da parte all'inizio del capitolo e rimesso a posto all'uscita (EndStory),
// e intanto il salvataggio del profilo usa la copia messa da parte (vedi SaveProfileIfChanged).
#include "GameLayer.h"
#include "UiTheme.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Logger/Log.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Tags.h"
#include "Engine/ECS/VfxEvents.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cfloat>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Engine/Core/Input.h"

extern std::string FindValidDir(const std::string& targetDirName);

namespace SunEngine {

    namespace {
        // ---- Racconto completo da leggere nel gioco: GameData/Stories/<id>.txt (UTF-8) ----
        // Le prime righe fino al primo capitolo sono l'intestazione: nome della serie, una riga "_", poi titolo, anno
        // e luogo (senza "_" la prima riga e' il titolo). Un capitolo comincia con
        // una riga "numero romano, punto, titolo" (es. "XII. La porta"); ogni altra riga e' un paragrafo.
        struct StoryText {
            struct Chapter { std::string title; std::vector<std::string> paragraphs; };
            std::vector<std::string> header;
            size_t seriesLines = 0;          // righe dell'intestazione prima del separatore "_": il nome della serie
            std::vector<Chapter> chapters;
            bool loaded = false;
        };
        constexpr int kStoryCount = 2;
        StoryText s_Texts[kStoryCount];
        int s_Story = 0;                 // storia scelta nella scheda PVE-STORY
        bool s_Reading = false;          // scheda PVE-STORY: false = capitoli giocabili, true = racconto
        int s_ReadChapter = -1;          // -1 = riprendi da dove si era arrivati (profilo)
        bool s_ReadScrollTop = false;

        // ---- Voci dei capitoli giocabili: GameData/Stories/primo_incontro_voci.json ----
        // Il personaggio parla in prima persona: un prologo prima di entrare in campo, battute legate a quello che
        // succede in partita (ognuna una volta), e le ultime righe alla vittoria o alla caduta. Lo stesso file lega
        // racconto e partite: da quali capitoli del racconto si gioca il momento, e quale si apre a fine partita.
        struct ChapterVoice {
            std::vector<std::string> prologue, win, fall;
            std::map<std::string, std::vector<std::string>> lines;
            std::vector<int> playFrom;   // capitoli del racconto (numerati da 1) con "Gioca questo momento"
            int readAfter = 0;           // capitolo del racconto (da 1) per "Leggi cosa e' successo"
        };
        ChapterVoice s_Voices[kStoryCount][3];
        bool s_VoicesLoaded[kStoryCount] = {};
        struct Subtitle { std::string text; float total = 0.0f, left = 0.0f; };
        std::vector<Subtitle> s_Subtitles;         // battute in coda (in basso, una alla volta)
        std::set<std::string> s_VoiceFired;        // battute gia' dette in questo capitolo
        float s_IntroTime = -1.0f, s_IntroTotal = 0.0f;   // prologo: secondi trascorsi (-1 = nessuno)
        float s_OutroTime = -1.0f, s_OutroTotal = 0.0f;   // ultime righe prima della schermata di fine
        bool s_SkipWasDown = true;

        void PlainPunctuation(std::string& s);
        const char* const kVoiceFiles[kStoryCount] = { "primo_incontro_voci.json", "il_peso_delle_pietre_voci.json" };
        void LoadVoices(int story) {
            story = std::clamp(story, 0, kStoryCount - 1);
            if (s_VoicesLoaded[story]) return;
            s_VoicesLoaded[story] = true;
            std::ifstream in(FindValidDir("Stories") + kVoiceFiles[story], std::ios::binary);
            if (!in) { SUN_CORE_WARN("PVE-STORY: voci dei capitoli non trovate ({0})", kVoiceFiles[story]); return; }
            try {
                const nlohmann::json j = nlohmann::json::parse(in);
                auto list = [](const nlohmann::json& a) {
                    std::vector<std::string> out;
                    if (a.is_array()) for (const auto& v : a) if (v.is_string()) { out.push_back(v.get<std::string>()); PlainPunctuation(out.back()); }
                    return out;
                };
                const auto& chapters = j.at("capitoli");
                for (size_t i = 0; i < chapters.size() && i < 3; ++i) {
                    const auto& c = chapters[i];
                    ChapterVoice& v = s_Voices[story][i];
                    v.prologue = list(c.value("prologo", nlohmann::json::array()));
                    v.win = list(c.value("vittoria", nlohmann::json::array()));
                    v.fall = list(c.value("caduta", nlohmann::json::array()));
                    if (c.contains("voci") && c["voci"].is_object())
                        for (auto it = c["voci"].begin(); it != c["voci"].end(); ++it) v.lines[it.key()] = list(it.value());
                    if (c.contains("racconto") && c["racconto"].is_array())
                        for (const auto& n : c["racconto"]) if (n.is_number_integer()) v.playFrom.push_back(n.get<int>());
                    v.readAfter = c.value("leggi", 0);
                }
            }
            catch (const std::exception& ex) { SUN_CORE_WARN("PVE-STORY: voci dei capitoli non valide: {0}", ex.what()); }
        }
        // Tempo di lettura di una riga: un po' di base piu' qualcosa per ogni carattere
        float ReadTime(const std::string& s) { return 2.0f + 0.055f * static_cast<float>(s.size()); }
        // Righe una dopo l'altra, centrate, su fondo scuro: ognuna entra quando la precedente e' stata letta
        void DrawLines(const std::vector<std::string>& lines, float time, float alpha, const char* header) {
            ImGuiIO& io = ImGui::GetIO();
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImFont* font = Ui::FontRead();
            const float size = font->FontSize * 1.25f;
            const float wrap = std::min(820.0f, io.DisplaySize.x - 120.0f);
            float y = io.DisplaySize.y * 0.30f;
            if (header && header[0]) {
                ImFont* hf = Ui::FontHeading();
                const ImVec2 hs = hf->CalcTextSizeA(hf->FontSize * 1.2f, FLT_MAX, 0.0f, header);
                dl->AddText(hf, hf->FontSize * 1.2f, ImVec2((io.DisplaySize.x - hs.x) * 0.5f, y - 54.0f), Ui::U32(Ui::Col::Gold, alpha), header);
            }
            float start = 0.6f;
            for (const auto& line : lines) {
                const float a = std::clamp((time - start) / 0.8f, 0.0f, 1.0f) * alpha;
                const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, wrap, line.c_str());
                const bool signature = line.rfind("- ", 0) == 0;   // firma di chi parla
                if (a > 0.0f) dl->AddText(font, size, ImVec2((io.DisplaySize.x - std::min(ts.x, wrap)) * 0.5f, y), Ui::U32(signature ? Ui::Col::Brass : Ui::Col::Bone, a), line.c_str(), nullptr, wrap);
                y += ts.y + 16.0f;
                start += ReadTime(line) * 0.75f;
            }
        }
        float LinesTotal(const std::vector<std::string>& lines) {
            float t = 0.6f;
            for (const auto& l : lines) t += ReadTime(l) * 0.75f;
            return t + 2.2f;
        }
        bool SkipPressed() {   // SPAZIO o INVIO, sul fronte di salita
            const bool down = Input::IsKeyPressed(GLFW_KEY_SPACE) || Input::IsKeyPressed(GLFW_KEY_ENTER);
            const bool pressed = down && !s_SkipWasDown;
            s_SkipWasDown = down;
            return pressed;
        }

        bool IsChapterHeading(const std::string& line) {
            size_t i = 0;
            while (i < line.size() && std::string("IVXLC").find(line[i]) != std::string::npos) ++i;
            return i > 0 && i + 1 < line.size() && line[i] == '.' && line[i + 1] == ' ';
        }

        // Il carattere dell'interfaccia ha le lettere accentate e le virgolette basse, non quelle tipografiche
        void PlainPunctuation(std::string& s) {
            static const char* k[][2] = { { "\xE2\x80\x99", "'" }, { "\xE2\x80\x98", "'" }, { "\xE2\x80\x9C", "\"" }, { "\xE2\x80\x9D", "\"" },
                                          { "\xE2\x80\x94", "-" }, { "\xE2\x80\x93", "-" }, { "\xE2\x80\xA6", "..." }, { "\xC2\xA0", " " } };
            for (const auto& r : k) {
                const std::string from = r[0];
                for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p)) s.replace(p, from.size(), r[1]);
            }
        }

        void LoadStoryText(StoryText& st, const char* file) {
            if (st.loaded) return;
            st.loaded = true;
            std::ifstream in(FindValidDir("Stories") + file, std::ios::binary);
            if (!in) { SUN_CORE_WARN("PVE-STORY: racconto non trovato ({0})", file); return; }
            std::stringstream buf; buf << in.rdbuf();
            std::string text = buf.str();
            if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
            PlainPunctuation(text);
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
                if (line.empty()) continue;
                if (st.chapters.empty() && line.find_first_not_of("_-") == std::string::npos) { st.seriesLines = st.header.size(); continue; }
                if (IsChapterHeading(line)) { st.chapters.push_back({ line, {} }); continue; }
                if (st.chapters.empty()) st.header.push_back(line);
                else st.chapters.back().paragraphs.push_back(line);
            }
        }

        constexpr const char* kStoryMap = "warzone_sector4";   // Vespera IV - Terza Serra
        constexpr float kKonradBossTime = 300.0f;               // 5 minuti
        constexpr float kOrdanHoldTime = 60.0f;                 // Ordan: la porta va tenuta almeno un minuto (i civili passano)
        constexpr float kVecchioHoldTime = 120.0f;              // il Vecchio: 2 minuti, il tempo della ritirata
        constexpr float kGorvakhGoal = 0.5f;                    // duello: Gorvakh sotto meta' della sua vita

        struct StoryChapter {
            const char* name;
            const char* role;
            const char* quote;
            const char* objective;
            glm::vec3 start;
            const char* map;
            const char* doneKey;
        };
        struct StoryDef {
            const char* id;            // file del racconto (<id>.txt) e segno di lettura (read_<id>)
            const char* menuName;
            const char* title;
            const char* when;
            const char* blurb;
            const char* mapLine;
            StoryChapter chapters[3];
        };
        const StoryDef kStories[kStoryCount] = {
            { "primo_incontro", "I. Primo Incontro", "PRIMO INCONTRO",
              "Anno 999 del Calendario Regale  -  Vespera IV, pianeta agricolo  -  Dominio di Aurel",
              "\"La prima cosa che capii fu che il cielo stava cadendo.\" Le piattaforme orbitali precipitano sulla "
              "Terza Serra e i Vrepighs attraversano i campi. La milizia dei Laboratores tiene la strada di servizio, "
              "i civili scendono nei rifugi, e nessuno sta venendo a salvarli. O almeno cosi' credono.",
              "Mappa: Vespera IV - Terza Serra",
              {
                { "Martus Kell", "Laborator della Terza Serra, milizia planetaria",
                  "\"Kell! Apri gli occhi!\"",
                  "La linea e' caduta. Attraversa la serra e raggiungi il magazzino nord, vivo. Non fermarti: i Vrepighs ti inseguono.",
                  { 2.0f, 0.5f, -50.0f }, kStoryMap, "martus_done" },   // dentro la serra centrale, fra due filari
                { "Sergente Ordan", "Sergente della milizia di Vespera IV",
                  "\"Qualcuno deve chiudere quella porta.\"",
                  "Tieni la porta del rifugio almeno 1 minuto: e' il tempo che serve ai civili per fuggire nel condotto. Poi resisti finche' puoi.",
                  { 2.0f, 0.5f, 94.0f }, kStoryMap, "ordan_done" },
                { "Konrad von Eber", "Ordo Domus Humilitatis",
                  "\"Allora non possiamo lasciare che raggiungano i rifugi.\"",
                  "Ferma l'assalto alla Terza Serra. Dopo 5 minuti arriva il loro capo, quello con la collana di frammenti viola: abbattilo.",
                  { 0.0f, 0.5f, 20.0f }, kStoryMap, "konrad_done" },
              } },
            { "il_peso_delle_pietre", "II. Il peso delle pietre", "IL PESO DELLE PIETRE",
              "Anno 999 del Calendario Regale  -  Il territorio di Gorvakh, poi Vespera IV",
              "Kragh ha mangiato pietre che altri non osavano toccare e ha piegato interi branchi. Poi Gorvakh lo convoca "
              "e pretende le sue pietre, i suoi guerrieri e una spedizione su un mondo dei glatt. Dalla parte dei Vrepighs, "
              "la storia della Terza Serra e del vecchio che porta al collo i tash che non puo' piu' mangiare.",
              "Mappe: la caverna di Gorvakh, Vespera IV - Terza Serra",
              {
                { "Kragh", "Capo del vargh - grado V",
                  "\"Se vuoi le mie pietre, vieni a prenderle.\"",
                  "Duello nella sala dell'altare, solo artigli. Porta Gorvakh (grado VI) sotto meta' della sua vita prima che lui spezzi te.",
                  { 0.0f, 0.5f, 11.0f }, "caverna_gorvakh", "kragh_duel_done" },
                { "Kragh", "Lo sbarco su Vespera IV",
                  "\"Non lasciateli respirare!\"",
                  "Con il vargh, sfonda le difese dei glatt e distruggi i loro tre punti di raccolta: abbatti chi li difende e resta accanto al punto finche' non cede.",
                  { -12.0f, 0.5f, -88.0f }, kStoryMap, "kragh_landing_done" },
                { "Il Vecchio", "Zhelvar, padre di Kragh",
                  "\"Da questo momento guido io il fronte.\"",
                  "Le tre corazze bianche bloccano la Terza Serra. Con tre guerrieri del vargh tieni il fronte per 2 minuti: e' il tempo della ritirata. [G] Gruuk.",
                  { -22.0f, 0.5f, -46.0f }, kStoryMap, "vecchio_done" },   // dietro il blocco di cemento: le corazze arrivano da nord
              } },
        };
        // capitolo in corso (o scelto nel menu)
        const StoryChapter& ChapterOf(int story, int chapter) { return kStories[std::clamp(story, 0, kStoryCount - 1)].chapters[std::clamp(chapter, 0, 2)]; }

        // Il peso delle pietre: punti di raccolta dei glatt (sbarco) e oggetti di scena da togliere a fine capitolo
        const glm::vec3 kGatherPoints[3] = { { -46.0f, 0.0f, 70.0f }, { 26.0f, 0.0f, 72.0f }, { 56.0f, 0.0f, 18.0f } };
        constexpr float kGatherRadius = 4.5f;     // dove deve stare il vargh
        constexpr float kGatherClear = 10.0f;     // nessun glatt vivo cosi' vicino
        constexpr float kGatherTime = 4.0f;       // secondi per abbatterlo
        std::vector<entt::entity> s_Props[3];     // casse, tenda e antenna di ogni punto
        // Martus: il magazzino nord (porta a sud, z = 95)
        const glm::vec3 kMartusGoal{ -40.0f, 0.0f, 89.0f };
        constexpr float kMartusGoalRadius = 4.5f;
        // Ordan: l'anticamera sigillata davanti alla porta del rifugio
        const glm::vec3 kOrdanBreach[3] = { { -1.5f, 0.5f, 82.0f }, { 2.0f, 0.5f, 81.5f }, { 5.5f, 0.5f, 82.0f } };

        // Il personaggio del capitolo, come unita' giocabile (armi e armature della storia)
        TrainedUnit MakeStoryUnit(int story, int chapter) {
            TrainedUnit u;
            if (story == 1) {
                if (chapter == 2) { u = TrainedUnit{ "Il Vecchio", "vrepighs_zhelvar", glm::vec4(0.42f, 0.30f, 0.50f, 1.0f) }; u.ApplyDefaultLoadout(); return u; }
                u = TrainedUnit{ "Kragh", "vrepighs_kragh", glm::vec4(0.36f, 0.24f, 0.30f, 1.0f) };
                u.ApplyDefaultLoadout();
                if (chapter == 0) {   // duello: solo artigli
                    u.primaryWeaponId.clear(); u.secondaryWeaponId.clear();
                    for (auto& e : u.equipmentSlots) e.clear();
                }
                return u;
            }
            if (chapter == 0) {
                u = TrainedUnit{ "Martus Kell", "kingship_recruit", glm::vec4(0.80f, 0.62f, 0.52f, 1.0f) };
                u.ApplyDefaultLoadout();
                u.primaryWeaponId = "auto_rifle_standard";   // il fucile automatico assegnato alle milizie planetarie
            }
            else if (chapter == 1) {
                u = TrainedUnit{ "Sergente Ordan", "kingship_footman", glm::vec4(0.66f, 0.50f, 0.40f, 1.0f) };
                u.ApplyDefaultLoadout();
                u.primaryWeaponId = "footman_auto_rifle";
                u.secondaryWeaponId = "footman_auto_pistol";
            }
            else {
                // lo stesso Konrad che si sblocca nei Vestments (Landmeister): livrea bianca, nera e avorio
                // dell'Ordo Domus Humilitatis, spada lunga dalla lama scura, equipaggiamento migliore
                u = TrainedUnit{ "Konrad von Eber", "vanguard_konrad", glm::vec4(0.78f, 0.62f, 0.52f, 1.0f) };
                u.ApplyDefaultLoadout();
            }
            return u;
        }

        std::string Clock(float seconds) {
            const int s = std::max(0, static_cast<int>(seconds));
            char b[16];
            std::snprintf(b, sizeof(b), "%02d:%02d", s / 60, s % 60);
            return b;
        }

        // Punto dentro la mappa, fuori dal rifugio chiuso (i Vrepighs dei capitoli aperti non nascono li' dentro)
        glm::vec3 ClampToField(glm::vec3 p) {
            p.x = std::clamp(p.x, -71.0f, 71.0f);
            p.z = std::clamp(p.z, -100.0f, 108.0f);
            if (p.x > -8.0f && p.x < 12.0f && p.z > 78.0f && p.z < 100.0f) p.z = 74.0f;
            return p;
        }

        bool ScreenPoint(const glm::mat4& vp, const glm::vec3& world, ImVec2& out) {
            const glm::vec4 clip = vp * glm::vec4(world, 1.0f);
            if (clip.w <= 0.05f) return false;
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            const ImVec2 size = ImGui::GetIO().DisplaySize;
            out = ImVec2((ndc.x * 0.5f + 0.5f) * size.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * size.y);
            return true;
        }

        // Segnalino nel mondo: rombo e scritta (obiettivo di Martus, capo dei Vrepighs)
        void WorldMarker(const glm::mat4& vp, const glm::vec3& world, const char* label, ImU32 col) {
            ImVec2 p;
            if (!ScreenPoint(vp, world, p)) return;
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const float r = 9.0f;
            dl->AddQuadFilled(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y), IM_COL32(0, 0, 0, 150));
            dl->AddQuad(ImVec2(p.x, p.y - r), ImVec2(p.x + r, p.y), ImVec2(p.x, p.y + r), ImVec2(p.x - r, p.y), col, 2.0f);
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(p.x - ts.x * 0.5f + 1.0f, p.y - r - ts.y - 3.0f), IM_COL32(0, 0, 0, 200), label);
            dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - r - ts.y - 4.0f), col, label);
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Menu: scheda PVE-STORY
    // ---------------------------------------------------------------------------------------------------------
    void GameLayer::RenderStoryUI() {
        ImGuiIO& io = ImGui::GetIO();
        const float w = std::min(1180.0f, io.DisplaySize.x - 40.0f);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 80.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(w, io.DisplaySize.y - 100.0f));
        ImGui::Begin("PVE-STORY", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

        // a sinistra le storie, a destra quella scelta (capitoli da giocare o racconto da leggere)
        ImGui::BeginChild("##storie", ImVec2(230.0f, 0.0f), true);
        Ui::Heading("STORIE");
        for (int k = 0; k < kStoryCount; ++k) {
            ImGui::PushID(k);
            if (k == s_Story) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.88f, 0.52f, 0.24f, 0.55f));
            if (ImGui::Selectable(kStories[k].menuName, k == s_Story) && k != s_Story) { s_Story = k; s_ReadChapter = -1; s_ReadScrollTop = true; }
            if (k == s_Story) ImGui::PopStyleColor();
            if (k == s_Story) {
                ImGui::Indent(12.0f);
                if (ImGui::Selectable("Gioca i capitoli", !s_Reading)) s_Reading = false;
                if (ImGui::Selectable("Leggi il racconto", s_Reading)) { s_Reading = true; s_ReadScrollTop = true; }
                ImGui::Unindent(12.0f);
            }
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::TextColored(Ui::Col::Ash, "Altre storie: in arrivo.");
        ImGui::Spacing(); Ui::Divider(); ImGui::Spacing();
        ImGui::TextWrapped("Qui si rivivono le storie del mondo di The Sun's Darkest Hours. Ogni capitolo si gioca "
                           "con il personaggio della storia, non con la squadra del garage; il racconto completo si "
                           "legge qui, e il segno resta dove sei arrivato.");
        ImGui::EndChild();
        ImGui::SameLine();

        if (s_Reading) {
            RenderStoryReader();
            ImGui::End();
            return;
        }

        const StoryDef& sd = kStories[s_Story];
        ImGui::BeginChild("##storia", ImVec2(0.0f, 0.0f), false);
        Ui::Title(sd.title);
        ImGui::TextColored(Ui::Col::Brass, "%s", sd.when);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", sd.blurb);
        ImGui::TextColored(Ui::Col::Ash, "%s", sd.mapLine);
        ImGui::Spacing(); Ui::Divider(); ImGui::Spacing();

        const auto& st = m_Profile.story;
        const float cardW = std::max(220.0f, (ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f);
        int start = -1;
        for (int c = 0; c < 3; ++c) {
            const StoryChapter& ch = sd.chapters[c];
            if (c > 0) ImGui::SameLine();
            ImGui::PushID(c);
            ImGui::BeginChild("##capitolo", ImVec2(cardW, 0.0f), true);
            ImGui::TextColored(Ui::Col::Gold, "CAPITOLO %s", Ui::Roman(c + 1));
            Ui::TextIn(Ui::FontHeading(), Ui::Col::Bone, ch.name);
            ImGui::TextColored(Ui::Col::Ash, "%s", ch.role);
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, Ui::Col::Brass);
            ImGui::TextWrapped("%s", ch.quote);
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::TextColored(Ui::Col::Gold, "OBIETTIVO");
            ImGui::TextWrapped("%s", ch.objective);
            ImGui::Spacing();
            if (s_Story == 0 && c == 1) {
                const int bestK = st.is_object() ? st.value("ordan_best_kills", 0) : 0;
                const float bestT = st.is_object() ? st.value("ordan_best_time", 0.0f) : 0.0f;
                const bool held = st.is_object() && st.value("ordan_done", false);
                if (bestT > 0.0f) ImGui::TextColored(held ? Ui::Col::Verdigris : Ui::Col::Ash, "%s  -  record: %d Vrepighs, %s", held ? "Completato" : "Non ancora completato", bestK, Clock(bestT).c_str());
                else ImGui::TextColored(Ui::Col::Ash, "Non ancora completato");
            }
            else {
                const bool done = st.is_object() && st.value(ch.doneKey, false);
                if (done) ImGui::TextColored(Ui::Col::Verdigris, "Completato");
                else ImGui::TextColored(Ui::Col::Ash, "Non ancora completato");
            }
            ImGui::Spacing();
            if (Ui::Button("GIOCA", ImVec2(-1.0f, 40.0f), Ui::Btn::Primary)) start = c;
            ImGui::EndChild();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::End();

        if (start >= 0) StartStoryChapter(start);
    }

    // Galleria: il racconto completo da leggere. A sinistra l'indice dei capitoli, a destra la pagina (colonna
    // stretta, corpo grande). L'ultimo capitolo aperto resta nel profilo (story.read_<id della storia>).
    void GameLayer::RenderStoryReader() {
        const StoryDef& sd = kStories[s_Story];
        StoryText& tx = s_Texts[s_Story];
        LoadStoryText(tx, (std::string(sd.id) + ".txt").c_str());
        const std::string readKey = std::string("read_") + sd.id;
        ImGui::BeginChild("##racconto", ImVec2(0.0f, 0.0f), false);
        if (tx.chapters.empty()) {
            Ui::Title(sd.title);
            ImGui::TextColored(Ui::Col::Ash, "Il racconto non e' stato trovato (GameData/Stories/%s.txt).", sd.id);
            ImGui::EndChild();
            return;
        }
        const int count = static_cast<int>(tx.chapters.size());
        if (s_ReadChapter < 0) s_ReadChapter = m_Profile.story.is_object() ? m_Profile.story.value(readKey, 0) : 0;
        s_ReadChapter = std::clamp(s_ReadChapter, 0, count - 1);
        int go = s_ReadChapter;

        // indice
        ImGui::BeginChild("##indice", ImVec2(250.0f, 0.0f), true);
        Ui::Heading("CAPITOLI");
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(tx.chapters[i].title.c_str(), i == s_ReadChapter)) go = i;
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // pagina
        ImGui::BeginChild("##pagina", ImVec2(0.0f, 0.0f), true);
        if (s_ReadScrollTop) { ImGui::SetScrollY(0.0f); s_ReadScrollTop = false; }
        const float avail = ImGui::GetContentRegionAvail().x;
        const float colW = std::min(680.0f, avail - 24.0f);
        const float left = std::max(12.0f, (avail - colW) * 0.5f);
        const auto& ch = tx.chapters[s_ReadChapter];
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        if (s_ReadChapter == 0) {   // frontespizio: titolo, anno, luogo
            for (size_t i = 0; i < tx.header.size(); ++i) {
                ImGui::SetCursorPosX(left);
                if (i < tx.seriesLines) Ui::TextIn(Ui::FontHeading(), Ui::Col::Brass, tx.header[i].c_str());
                else if (i == tx.seriesLines) Ui::TextIn(Ui::FontTitle(), Ui::Col::Gold, tx.header[i].c_str());
                else ImGui::TextColored(Ui::Col::Brass, "%s", tx.header[i].c_str());
            }
            ImGui::Dummy(ImVec2(0.0f, 14.0f));
        }
        ImGui::SetCursorPosX(left);
        Ui::TextIn(Ui::FontHeading(), Ui::Col::Gold, ch.title.c_str());
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::PushFont(Ui::FontRead());
        ImGui::PushStyleColor(ImGuiCol_Text, Ui::Col::Bone);
        for (const auto& par : ch.paragraphs) {
            ImGui::SetCursorPosX(left);
            ImGui::PushTextWrapPos(left + colW);
            ImGui::TextUnformatted(par.c_str(), par.c_str() + par.size());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
        }
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 14.0f));
        ImGui::SetCursorPosX(left);
        ImGui::BeginDisabled(s_ReadChapter == 0);
        if (Ui::PlainButton("< Capitolo precedente", ImVec2(colW * 0.5f - 6.0f, 38.0f))) go = s_ReadChapter - 1;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (s_ReadChapter + 1 < count) {
            if (Ui::Button("Capitolo successivo >", ImVec2(colW * 0.5f - 6.0f, 38.0f), Ui::Btn::Primary)) go = s_ReadChapter + 1;
        }
        else {
            ImGui::BeginDisabled();
            Ui::PlainButton("Fine del racconto", ImVec2(colW * 0.5f - 6.0f, 38.0f));
            ImGui::EndDisabled();
        }
        ImGui::SetCursorPosX(left);
        ImGui::TextColored(Ui::Col::Ash, "Capitolo %d di %d", s_ReadChapter + 1, count);
        // Racconto e partita legati: questo capitolo si puo' rivivere con il suo personaggio
        LoadVoices(s_Story);
        int play = -1;
        for (int g = 0; g < 3; ++g)
            for (int n : s_Voices[s_Story][g].playFrom) if (n == s_ReadChapter + 1) play = g;
        int startPlay = -1;
        if (play >= 0) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::SetCursorPosX(left);
            ImGui::TextColored(Ui::Col::Brass, "Questo momento si puo' rivivere: %s.", sd.chapters[play].name);
            ImGui::SetCursorPosX(left);
            if (Ui::Button("GIOCA QUESTO MOMENTO", ImVec2(colW, 42.0f), Ui::Btn::Primary)) startPlay = play;
        }
        ImGui::Dummy(ImVec2(0.0f, 16.0f));
        ImGui::EndChild();
        ImGui::EndChild();
        if (startPlay >= 0) { StartStoryChapter(startPlay); return; }

        if (go != s_ReadChapter) {
            s_ReadChapter = std::clamp(go, 0, count - 1);
            s_ReadScrollTop = true;
            if (!m_Profile.story.is_object()) m_Profile.story = nlohmann::json::object();
            m_Profile.story[readKey] = s_ReadChapter;   // il segno resta nel profilo
            SaveProfileIfChanged();
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Il peso delle pietre: personaggi dei capitoli e punti di raccolta
    // ---------------------------------------------------------------------------------------------------------
    namespace {
        // Cubo di scena (senza collisioni): casse, tende, antenne dei punti di raccolta
        entt::entity StoryProp(Scene& scene, const glm::vec3& pos, const glm::vec3& size, const glm::vec4& color, float yaw = 0.0f) {
            auto& reg = scene.GetRegistry();
            const entt::entity e = scene.CreateEntity();
            auto& t = reg.emplace<TransformComponent>(e);
            t.Position = pos + glm::vec3(0.0f, size.y * 0.5f, 0.0f);
            t.Scale = size;
            t.Rotation = glm::vec3(0.0f, yaw, 0.0f);
            reg.emplace<MeshComponent>(e, 0u);
            auto& c = reg.emplace<ColorComponent>(e);
            c.Color = color; c.BaseColor = color;
            return e;
        }
        void ClearStoryProps(Scene& scene) {
            auto& reg = scene.GetRegistry();
            for (auto& v : s_Props) {
                for (auto e : v) if (reg.valid(e)) scene.DestroyEntity(e);
                v.clear();
            }
        }
    }

    // Unita' della storia guidata dall'IA (compagni del vargh, glatt, corazze bianche)
    entt::entity GameLayer::SpawnStoryBot(const std::string& unitId, const std::string& name, int team, const glm::vec3& pos, float yaw,
                                          const glm::vec3* guardPost, float leash) {
        TrainedUnit u{ name, unitId, glm::vec4(1.0f) };
        u.armorColor = unitId.rfind("vrepighs", 0) == 0 ? glm::vec4(0.40f, 0.32f, 0.28f, 1.0f) : glm::vec4(0.62f, 0.60f, 0.52f, 1.0f);
        u.ApplyDefaultLoadout();
        const entt::entity e = SpawnBot(u, team, pos, yaw, -1);
        if (e == entt::null) return e;
        m_VsBotUnits.erase(e);   // non e' un'unita' di una squadra del Vs Bot
        if (guardPost) m_Scene.GetRegistry().emplace_or_replace<BotGuardPostComponent>(e, BotGuardPostComponent{ *guardPost, leash });
        return e;
    }

    void GameLayer::SetupStoryChapter() {
        auto& reg = m_Scene.GetRegistry();
        if (m_Story.story != 1) return;
        const int chapter = m_Story.chapter;
        // il personaggio e' della squadra 0, gli avversari della 1 (le uccisioni contano contro la 1)
        m_VsBotMyTeam = 0;
        if (!reg.all_of<TeamComponent>(m_PlayerEntity)) { reg.emplace<TeamComponent>(m_PlayerEntity, TeamComponent{ 0 }); m_Story.addedTeam = true; }
        else reg.get<TeamComponent>(m_PlayerEntity).team = 0;
        const glm::vec3 me = ChapterOf(1, chapter).start;

        if (chapter == 0) {
            // Gorvakh davanti all'altare: resta nella sala
            const glm::vec3 post(0.0f, 0.5f, -6.0f);
            m_Story.boss = SpawnStoryBot("vrepighs_gorvakh", "Gorvakh", 1, post, 180.0f, &post, 40.0f);   // guarda verso Kragh
            m_BotAI.SetTeamBases(me, post);
        }
        else if (chapter == 1) {
            m_BotAI.SetTeamBases(glm::vec3(0.0f, 0.0f, -90.0f), glm::vec3(0.0f, 0.0f, 66.0f));
            // il vargh: scende dai rottami con Kragh
            const char* vargh[12] = { "vrepighs_tusker", "vrepighs_tusker", "vrepighs_tusker", "vrepighs_tusker", "vrepighs_gunner", "vrepighs_gunner",
                                      "vrepighs_gunner", "vrepighs_tusker", "vrepighs_feral_pig", "vrepighs_feral_pig", "vrepighs_gunner", "vrepighs_tusker" };
            for (int i = 0; i < 12; ++i) {
                const glm::vec3 p = me + glm::vec3(-10.0f + 2.2f * static_cast<float>(i % 6) + 8.0f, 0.0f, -2.0f - 2.5f * static_cast<float>(i / 6));
                SpawnStoryBot(vargh[i], i < 6 ? "Guerriero di Morg" : "Guerriero del vargh", 0, p, 180.0f);
            }
            // i glatt: una cinquantina di grado II. 6 di guardia a ogni punto di raccolta...
            int n = 0;
            for (int k = 0; k < 3; ++k) {
                for (int i = 0; i < 6; ++i) {
                    const float a = 6.2831853f * static_cast<float>(i) / 6.0f + 0.4f * static_cast<float>(k);
                    const glm::vec3 p = kGatherPoints[k] + glm::vec3(std::cos(a) * 6.5f, 0.5f, std::sin(a) * 6.5f);
                    SpawnStoryBot("kingship_recruit", "Guardia del punto", 1, p, 0.0f, &p, 12.0f); ++n;
                }
            }
            // ...e il resto sulle linee di difesa: avamposti nei campi e la linea della strada di servizio
            const glm::vec3 outposts[4] = { { -45.0f, 0.5f, 4.0f }, { -14.0f, 0.5f, 10.0f }, { 16.0f, 0.5f, 4.0f }, { 46.0f, 0.5f, 10.0f } };
            for (const auto& o : outposts)
                for (int i = 0; i < 4; ++i) {
                    const glm::vec3 p = o + glm::vec3(-3.0f + 2.0f * static_cast<float>(i), 0.0f, (i % 2) * 1.5f);
                    SpawnStoryBot("kingship_recruit", "Difensore", 1, p, 0.0f, &p, 18.0f); ++n;
                }
            for (int i = 0; i < 10; ++i) {
                float x = -56.0f + 12.0f * static_cast<float>(i);
                if (x > -10.0f && x < 14.0f) x += 26.0f;   // non dentro il rifugio
                const glm::vec3 p(x, 0.5f, 58.0f);
                SpawnStoryBot("kingship_recruit", "Difensore della linea", 1, p, 0.0f, &p, 16.0f); ++n;
            }
            for (int k = 0; k < 3; ++k)   // strade che portano ai punti
                for (int i = 0; i < 2; ++i) {
                    const glm::vec3 p = kGatherPoints[k] + glm::vec3(i == 0 ? -12.0f : 12.0f, 0.5f, -14.0f);
                    SpawnStoryBot("kingship_recruit", "Sentinella", 1, p, 0.0f, &p, 16.0f); ++n;
                }
            SUN_CORE_INFO("PVE-STORY: sbarco, {0} glatt in campo", n);
            // i punti di raccolta: casse di provviste, tenda, antenna con la luce
            ClearStoryProps(m_Scene);
            for (int k = 0; k < 3; ++k) {
                const glm::vec3 c = kGatherPoints[k];
                s_Props[k].push_back(StoryProp(m_Scene, c + glm::vec3(-1.6f, 0.0f, 0.4f), glm::vec3(2.4f, 1.6f, 3.2f), glm::vec4(0.32f, 0.36f, 0.24f, 1.0f), 8.0f));   // tenda
                s_Props[k].push_back(StoryProp(m_Scene, c + glm::vec3(1.2f, 0.0f, -0.6f), glm::vec3(1.0f, 0.8f, 1.0f), glm::vec4(0.42f, 0.33f, 0.20f, 1.0f), 20.0f));  // casse
                s_Props[k].push_back(StoryProp(m_Scene, c + glm::vec3(1.4f, 0.8f, -0.5f), glm::vec3(0.8f, 0.6f, 0.8f), glm::vec4(0.40f, 0.31f, 0.19f, 1.0f), 35.0f));
                s_Props[k].push_back(StoryProp(m_Scene, c + glm::vec3(2.2f, 0.0f, 1.0f), glm::vec3(1.0f, 0.8f, 1.0f), glm::vec4(0.38f, 0.30f, 0.18f, 1.0f), -10.0f));
                s_Props[k].push_back(StoryProp(m_Scene, c + glm::vec3(0.4f, 0.0f, 1.8f), glm::vec3(0.18f, 4.5f, 0.18f), glm::vec4(0.25f, 0.25f, 0.27f, 1.0f)));       // antenna
                s_Props[k].push_back(StoryProp(m_Scene, c + glm::vec3(0.4f, 4.5f, 1.8f), glm::vec3(0.35f, 0.35f, 0.35f), glm::vec4(0.95f, 0.75f, 0.2f, 1.0f)));
            }
        }
        else {
            m_BotAI.SetTeamBases(glm::vec3(-22.0f, 0.0f, -60.0f), glm::vec3(0.0f, 0.0f, 40.0f));
            // tre guerrieri del vargh restano con il vecchio
            for (int i = 0; i < 3; ++i) {
                const glm::vec3 p = me + glm::vec3(-3.0f + 3.0f * static_cast<float>(i), 0.0f, -2.5f);
                SpawnStoryBot("vrepighs_tusker", "Zhelvar", 0, p, 180.0f, &p, 14.0f);
            }
            // le tre corazze bianche arrivano dal rifugio, a piedi: Konrad avanti con la lama scura (niente armi
            // da fuoco, come nel racconto), Adelheid e Dietrich dietro con i fucili. Tirano nel fumo contro un
            // bersaglio che corre: mira, cadenza e colpi di lama ridotti, altrimenti nessun grado IV reggerebbe due minuti
            m_Story.belokh.clear();
            auto belokh = [&](TrainedUnit u, const glm::vec3& p) {
                const entt::entity e = SpawnBot(u, 1, p, 0.0f, -1);
                if (e == entt::null) return;
                m_VsBotUnits.erase(e);
                auto& b = m_Scene.GetRegistry().get_or_emplace<FactionBuffComponent>(e);
                b.accuracyMult = 0.5f; b.spreadMult = 1.8f; b.fireRateMult = 0.6f; b.meleeDamageMult = 0.6f;
                m_Story.belokh.push_back(e);
            };
            TrainedUnit konrad = MakeStoryUnit(0, 2);
            konrad.primaryWeaponId.clear(); konrad.secondaryWeaponId.clear();
            belokh(konrad, glm::vec3(0.0f, 0.5f, 34.0f));
            TrainedUnit adel{ "Adelheid Voss", "vanguard_defender", glm::vec4(0.86f, 0.84f, 0.78f, 1.0f) };
            adel.ApplyDefaultLoadout();
            belokh(adel, glm::vec3(-6.0f, 0.5f, 40.0f));
            TrainedUnit diet{ "Dietrich Falken", "vanguard_defender", glm::vec4(0.80f, 0.79f, 0.74f, 1.0f) };
            diet.ApplyDefaultLoadout();
            belokh(diet, glm::vec3(6.0f, 0.5f, 40.0f));
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Inizio, aggiornamento e fine di un capitolo
    // ---------------------------------------------------------------------------------------------------------
    void GameLayer::StartStoryChapter(int chapter) {
        chapter = std::clamp(chapter, 0, 2);
        const int story = std::clamp(s_Story, 0, kStoryCount - 1);
        if (!m_Story.active) {   // il garage si mette da parte una volta sola (anche quando si rigioca)
            m_StoryBackupRoster = m_PlayerRoster;
            m_StoryBackupSquad = m_SelectedSquadIndices;
            m_StoryBackupMap = m_SelectedMapId;
            m_StoryBackupEvent = m_SelectedEventIndex;
        }
        ClearStoryProps(m_Scene);
        const StoryChapter& ch = ChapterOf(story, chapter);
        m_PlayerRoster = { MakeStoryUnit(story, chapter) };
        m_SelectedSquadIndices = { 0 };
        m_SelectedMapId = ch.map;
        m_SelectedEventIndex = 3;   // come il Sandbox: niente ondate della sopravvivenza, le ondate le fa il capitolo

        const bool addedTeamBefore = m_Story.addedTeam;
        m_Story = StoryState{};
        m_Story.active = true;
        m_Story.story = story;
        m_Story.chapter = chapter;
        m_Story.addedTeam = addedTeamBefore;
        StartDeployment(0);
        if (m_GameState != GameState::Gameplay) { EndStory(); return; }   // non e' partito

        // il personaggio parte dal punto del suo capitolo
        auto& reg = m_Scene.GetRegistry();
        if (auto* t = reg.try_get<TransformComponent>(m_PlayerEntity)) {
            t->Position = ch.start;
            if (story == 1 && chapter == 1) t->AimYaw = 180.0f;   // sbarco: verso sud, dove sono i glatt
            m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, t->Position);
        }
        m_Story.kills0 = m_KillCount;
        m_Story.spawnTimer = chapter == 1 ? 6.0f : 4.0f;   // un attimo per guardarsi intorno
        m_Story.waveSize = chapter == 0 ? 2 : 1;
        SetupStoryChapter();
        m_PartyToasts.push_back({ std::string(ch.name) + ": " + ch.objective, 6.0f });
        // Voce del personaggio: prologo sullo schermo scuro (la partita aspetta), poi le battute in campo
        LoadVoices(story);
        s_Subtitles.clear();
        s_VoiceFired.clear();
        s_OutroTime = -1.0f;
        s_SkipWasDown = true;
        if (!s_Voices[story][chapter].prologue.empty()) {
            s_IntroTime = 0.0f;
            s_IntroTotal = LinesTotal(s_Voices[story][chapter].prologue);
            m_IsPaused = true;
        }
        else s_IntroTime = -1.0f;
        SUN_CORE_INFO("PVE-STORY: {0}, capitolo {1} ({2})", kStories[story].title, chapter + 1, ch.name);
    }

    bool GameLayer::StoryUsesBots() const { return m_Story.active && m_Story.story == 1; }

    void GameLayer::UpdateStory(float deltaTime, const glm::vec3& playerPos) {
        if (!m_Story.active || m_Story.result != 0) return;
        if (m_Story.story == 1) { UpdateStoryStones(deltaTime, playerPos); return; }
        auto& reg = m_Scene.GetRegistry();
        m_Story.time += deltaTime;

        int alive = 0;
        for (auto e : reg.view<HealthComponent>(entt::exclude<PlayerTag, CorpseComponent, TrainingDummyComponent, GrugnComponent>)) {
            (void)e;
            ++alive;
        }

        // ondate
        const int chapter = m_Story.chapter;
        const float interval = chapter == 0 ? 7.0f : (chapter == 1 ? 9.0f : 10.0f);
        const int maxAlive = chapter == 0 ? 22 : (chapter == 1 ? 30 : 40);
        m_Story.spawnTimer -= deltaTime;
        if (m_Story.spawnTimer <= 0.0f) {
            m_Story.spawnTimer = interval;
            const int n = std::min(m_Story.waveSize, maxAlive - alive);
            for (int i = 0; i < n; ++i) {
                glm::vec3 p;
                if (chapter == 1) {
                    // dall'anticamera: sfondano la porta del rifugio
                    p = kOrdanBreach[Random::RangeInt(0, 2)] + glm::vec3(Random::Range(-0.8f, 0.8f), 0.0f, Random::Range(-0.6f, 0.6f));
                }
                else {
                    const float a = Random::Range(0.0f, 6.28318530718f);
                    const float d = Random::Range(chapter == 0 ? 22.0f : 20.0f, chapter == 0 ? 42.0f : 45.0f);
                    p = ClampToField(playerPos + glm::vec3(std::cos(a) * d, 0.0f, std::sin(a) * d));
                    p.y = 0.5f;
                }
                SpawnNetVrepigh(p);
            }
            if (chapter == 0) m_Story.waveSize = std::min(m_Story.waveSize + 1, 6);
            else if (chapter == 1) m_Story.waveSize = std::min(m_Story.waveSize + 1, 10);
            else m_Story.waveSize = std::min(m_Story.waveSize * 2, 8);
        }

        // battute del personaggio legate a quello che succede (ognuna una volta sola)
        {
            const int kills = m_KillCount - m_Story.kills0;
            StoryVoice("inizio");
            if (kills >= 1) StoryVoice("prima_uccisione");
            if (kills >= 10) StoryVoice("uccisioni_10");
            if (const auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity); hp && hp->maxHealth > 0.0f) {
                const float f = hp->currentHealth / hp->maxHealth;
                if (f < 0.25f) StoryVoice("grave");
                else if (f < 0.55f) StoryVoice("ferito");
            }
            if (chapter == 0) {
                const glm::vec3 st = ChapterOf(0, 0).start;
                const float total = glm::length(glm::vec2(st.x - kMartusGoal.x, st.z - kMartusGoal.z));
                const float left = glm::length(glm::vec2(playerPos.x - kMartusGoal.x, playerPos.z - kMartusGoal.z));
                if (left < 28.0f) StoryVoice("vicino");
                else if (left < total * 0.55f) StoryVoice("meta");
            }
            else if (chapter == 1) {
                if (m_Story.time > 30.0f) StoryVoice("tiene_1");
                if (m_Story.time >= kOrdanHoldTime) StoryVoice("minuto");
                if (m_Story.time > 120.0f) StoryVoice("tiene_2");
            }
            else {
                if (m_Story.time > 60.0f) StoryVoice("minuto_1");
                if (m_Story.time > 180.0f) StoryVoice("minuto_3");
                if (m_Story.bossSpawned) {
                    StoryVoice("capo");
                    const auto* bhp = reg.valid(m_Story.boss) ? reg.try_get<HealthComponent>(m_Story.boss) : nullptr;
                    if (bhp && bhp->currentHealth < bhp->maxHealth * 0.5f) StoryVoice("capo_meta");
                }
            }
        }

        if (chapter == 0) {
            // Martus: arrivare al magazzino nord
            const glm::vec2 d(playerPos.x - kMartusGoal.x, playerPos.z - kMartusGoal.z);
            if (glm::length(d) <= kMartusGoalRadius) {
                m_Story.result = 1;
                m_Profile.story["martus_done"] = true;
                ApplyMatchXp(1.0f, "Martus ha raggiunto il magazzino nord");
            }
        }
        else if (chapter == 2) {
            // Konrad: dopo 5 minuti arriva il capo
            if (!m_Story.bossSpawned && m_Story.time >= kKonradBossTime) {
                m_Story.bossSpawned = true;
                glm::vec3 p = ClampToField(playerPos + glm::vec3(0.0f, 0.0f, -32.0f));
                p.y = 0.5f;
                const entt::entity boss = SpawnNetVrepigh(p);
                if (reg.valid(boss)) {
                    auto& hp = reg.get_or_emplace<HealthComponent>(boss);
                    hp.maxHealth = 1200.0f; hp.currentHealth = 1200.0f;
                    if (auto* stats = reg.try_get<UnitStatsComponent>(boss)) {
                        stats->armorSet.torso.armorRating = 12;
                        stats->armorSet.torso.maxDurability = 600.0f;
                        stats->armorSet.torso.currentDurability = 600.0f;
                    }
                    if (auto* c = reg.try_get<ColorComponent>(boss)) {   // pelle scura, quasi nera
                        c->Color = glm::vec4(0.10f, 0.07f, 0.12f, 1.0f);
                        c->BaseColor = c->Color;
                    }
                }
                m_Story.boss = boss;
                m_PartyToasts.push_back({ "Il capo dei Vrepighs e' arrivato: quello con la collana di frammenti viola", 5.0f });
            }
            if (m_Story.bossSpawned && !m_Story.bossDown) {
                const auto* hp = reg.valid(m_Story.boss) ? reg.try_get<HealthComponent>(m_Story.boss) : nullptr;
                const bool down = !hp || hp->currentHealth <= 0.0f || reg.all_of<CorpseComponent>(m_Story.boss);
                if (down) {
                    m_Story.bossDown = true;
                    m_Story.result = 1;
                    m_Profile.story["konrad_done"] = true;
                    ApplyMatchXp(1.0f, "Il capo dei Vrepighs e' caduto: la linea ha retto");
                }
            }
        }

        if (m_Story.result != 0) StoryFinishWin();
    }

    // Fine vittoriosa: le ultime righe, la simulazione si ferma sotto la schermata
    void GameLayer::StoryFinishWin() {
        const auto& v = s_Voices[m_Story.story][std::clamp(m_Story.chapter, 0, 2)];
        if (!v.win.empty()) { s_OutroTime = 0.0f; s_OutroTotal = LinesTotal(v.win); s_SkipWasDown = true; }
        m_IsPaused = true;
        if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    // Il peso delle pietre: obiettivi dei tre capitoli
    void GameLayer::UpdateStoryStones(float deltaTime, const glm::vec3& playerPos) {
        auto& reg = m_Scene.GetRegistry();
        m_Story.time += deltaTime;
        const int chapter = m_Story.chapter;
        StoryVoice("inizio");
        if (const auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity); hp && hp->maxHealth > 0.0f) {
            const float f = hp->currentHealth / hp->maxHealth;
            if (f < 0.25f) StoryVoice("grave");
            else if (f < 0.55f) StoryVoice("ferito");
        }
        auto alive = [&](entt::entity e) {
            const auto* h = reg.valid(e) ? reg.try_get<HealthComponent>(e) : nullptr;
            return h && h->currentHealth > 0.0f && !reg.all_of<CorpseComponent>(e);
        };

        if (chapter == 0) {
            // Duello: Gorvakh sotto meta' della sua vita. Sotto il 70% chiama a se' la pietra (vene viola, colpi piu' pesanti)
            const auto* hp = reg.valid(m_Story.boss) ? reg.try_get<HealthComponent>(m_Story.boss) : nullptr;
            const float f = hp && hp->maxHealth > 0.0f ? hp->currentHealth / hp->maxHealth : 0.0f;
            if (f < 0.85f) StoryVoice("gorvakh_ferito");
            if (f < 0.70f && !m_Story.bossPhase && alive(m_Story.boss)) {
                m_Story.bossPhase = true;
                StoryVoice("gorvakh_pietra");
                auto& b = reg.get_or_emplace<FactionBuffComponent>(m_Story.boss);
                b.meleeDamageMult = 1.15f; b.meleeSpeedMult = 1.10f;
                if (auto* c = reg.try_get<ColorComponent>(m_Story.boss)) { c->Color = glm::mix(c->BaseColor, glm::vec4(0.45f, 0.18f, 0.6f, 1.0f), 0.35f); c->BaseColor = c->Color; }
                m_PartyToasts.push_back({ "Gorvakh chiama a se' la pietra: colpi piu' pesanti", 3.5f });
            }
            if (!alive(m_Story.boss) || f < kGorvakhGoal) {
                m_Story.result = 1;
                m_Profile.story["kragh_duel_done"] = true;
                ApplyMatchXp(1.0f, "Gorvakh e' stato ferito davanti a tutti");
            }
        }
        else if (chapter == 1) {
            // Sbarco: i punti di raccolta cedono se il vargh ci sta accanto e intorno non c'e' piu' un glatt vivo
            const int kills = m_KillCount - m_Story.kills0;
            if (kills >= 1) StoryVoice("prima_uccisione");
            if (kills >= 10) StoryVoice("uccisioni_10");
            for (int k = 0; k < 3; ++k) {
                if (m_Story.pointDone[k]) continue;
                const glm::vec3 c = kGatherPoints[k];
                bool vargh = false, glatt = false;
                auto units = reg.view<TeamComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent>);
                for (auto u : units) {
                    if (units.get<HealthComponent>(u).currentHealth <= 0.0f) continue;
                    const glm::vec3 up = units.get<TransformComponent>(u).Position;
                    const float d = glm::length(glm::vec2(up.x - c.x, up.z - c.z));
                    const int team = units.get<TeamComponent>(u).team;
                    if (team == 0 && d <= kGatherRadius) vargh = true;
                    if (team == 1 && d <= kGatherClear) glatt = true;
                }
                if (vargh && !glatt) m_Story.pointProgress[k] += deltaTime;
                else m_Story.pointProgress[k] = std::max(0.0f, m_Story.pointProgress[k] - deltaTime * 0.5f);
                if (m_Story.pointProgress[k] >= kGatherTime) {
                    m_Story.pointDone[k] = true;
                    ++m_Story.pointsDone;
                    VfxEvent ev; ev.kind = VfxKind::Explosion; ev.pos = c + glm::vec3(0.0f, 0.6f, 0.0f); ev.scale = 3.5f; EmitVfx(ev);
                    for (auto e : s_Props[k]) if (auto* col = reg.try_get<ColorComponent>(e)) { col->Color = glm::vec4(0.07f, 0.06f, 0.05f, 1.0f); col->BaseColor = col->Color; }
                    m_PartyToasts.push_back({ "Punto di raccolta distrutto (" + std::to_string(m_Story.pointsDone) + " su 3)", 3.5f });
                    if (m_Story.pointsDone == 1) StoryVoice("punto_1");
                    if (m_Story.pointsDone == 2) StoryVoice("punto_2");
                }
            }
            if (m_Story.pointsDone >= 3) {
                m_Story.result = 1;
                m_Profile.story["kragh_landing_done"] = true;
                ApplyMatchXp(1.0f, "I tre punti di raccolta dei glatt sono caduti");
            }
        }
        else {
            // Ultima difesa: 2 minuti contro le corazze bianche
            if (m_Story.time > 3.0f) StoryVoice("belokh");
            if (m_Story.time > 60.0f) StoryVoice("minuto");
            if (m_Story.time > kVecchioHoldTime - 30.0f) StoryVoice("ultimi_30");
            if (reg.all_of<WarCryShoutComponent>(m_PlayerEntity)) StoryVoice("gruuk");
            if (m_Story.time >= kVecchioHoldTime) {
                m_Story.result = 1;
                m_Profile.story["vecchio_done"] = true;
                ApplyMatchXp(1.0f, "Il vecchio ha retto il fronte: la ritirata e' passata");
            }
        }
        (void)playerPos;
        if (m_Story.result != 0) StoryFinishWin();
    }

    void GameLayer::StoryOnPlayerDeath() {
        if (!m_Story.active || m_Story.result != 0) return;
        if (m_Story.story == 0 && m_Story.chapter == 1) {
            // Ordan: e' la fine che conosciamo. Se ha tenuto la porta almeno un minuto i civili sono passati (3),
            // altrimenti e' caduto troppo presto (2). Conta comunque quanto ha tenuto.
            m_Story.result = m_Story.time >= kOrdanHoldTime ? 3 : 2;
            if (m_Story.result == 3) m_Profile.story["ordan_done"] = true;
            const int kills = m_KillCount - m_Story.kills0;
            auto& st = m_Profile.story;
            const float bestT = st.value("ordan_best_time", 0.0f);
            const int bestK = st.value("ordan_best_kills", 0);
            if (kills > bestK || (kills == bestK && m_Story.time > bestT)) {
                st["ordan_best_kills"] = kills;
                st["ordan_best_time"] = m_Story.time;
                m_Story.newRecord = true;
            }
            ApplyMatchXp(m_Story.result == 3 ? 1.0f : 0.5f, (m_Story.result == 3 ? "La porta ha retto per " : "La porta ha ceduto dopo ") + Clock(m_Story.time));
        }
        else {
            m_Story.result = 2;
            const char* why = "Il personaggio e' caduto";
            if (m_Story.story == 0) why = m_Story.chapter == 0 ? "Martus e' caduto prima del magazzino" : "Konrad e' caduto";
            else why = m_Story.chapter == 0 ? "Gorvakh ha spezzato Kragh" : (m_Story.chapter == 1 ? "Kragh e' caduto su Vespera IV" : "Il vecchio e' caduto troppo presto");
            ApplyMatchXp(0.5f, why);
        }
        // le ultime righe del personaggio, prima della schermata di fine
        LoadVoices(m_Story.story);
        {
            const auto& v = s_Voices[m_Story.story][m_Story.chapter];
            const auto& lines = m_Story.result == 3 ? v.win : v.fall;
            if (!lines.empty()) { s_OutroTime = 0.0f; s_OutroTotal = LinesTotal(lines); s_SkipWasDown = true; }
        }
        m_IsPaused = true;
        if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    // ---------------------------------------------------------------------------------------------------------
    // HUD e schermata di fine
    // ---------------------------------------------------------------------------------------------------------
    // Battuta del personaggio: va in coda e compare in basso, una alla volta
    void GameLayer::StoryVoice(const char* key) {
        if (!m_Story.active || s_VoiceFired.count(key)) return;
        s_VoiceFired.insert(key);
        const auto& lines = s_Voices[m_Story.story][std::clamp(m_Story.chapter, 0, 2)].lines;
        const auto it = lines.find(key);
        if (it == lines.end()) return;
        for (const auto& l : it->second) s_Subtitles.push_back({ l, ReadTime(l), ReadTime(l) });
    }

    bool GameLayer::StoryIntroActive() const { return m_Story.active && m_Story.result == 0 && s_IntroTime >= 0.0f; }

    // Prologo: schermo scuro, il personaggio dice chi e' e che cosa ha davanti. SPAZIO per entrare subito.
    void GameLayer::RenderStoryIntro() {
        ImGuiIO& io = ImGui::GetIO();
        const int chapter = std::clamp(m_Story.chapter, 0, 2);
        s_IntroTime += std::min(io.DeltaTime, 0.1f);
        const float fade = std::clamp((s_IntroTotal - s_IntroTime) / 1.2f, 0.0f, 1.0f);   // alla fine si apre sul campo
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(4, 4, 6, static_cast<int>(255.0f * (0.35f + 0.65f * fade))));
        const std::string header = std::string(kStories[m_Story.story].title) + "  -  " + ChapterOf(m_Story.story, chapter).name;
        DrawLines(s_Voices[m_Story.story][chapter].prologue, s_IntroTime, fade, header.c_str());
        const char* hint = "SPAZIO: entra in campo";
        const ImVec2 hs = ImGui::CalcTextSize(hint);
        ImGui::GetForegroundDrawList()->AddText(ImVec2((io.DisplaySize.x - hs.x) * 0.5f, io.DisplaySize.y - 60.0f), Ui::U32(Ui::Col::Ash, fade), hint);
        if (s_IntroTime >= s_IntroTotal || (s_IntroTime > 0.5f && SkipPressed())) {
            s_IntroTime = -1.0f;
            m_IsPaused = false;
            if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
    }

    void GameLayer::RenderStoryHUD() {
        if (!m_Story.active) return;
        // battute del personaggio in basso al centro
        if (!s_Subtitles.empty()) {
            ImGuiIO& io = ImGui::GetIO();
            Subtitle& sub = s_Subtitles.front();
            sub.left -= std::min(io.DeltaTime, 0.1f);
            const float a = std::clamp(std::min((sub.total - sub.left) / 0.35f, sub.left / 0.5f), 0.0f, 1.0f);
            ImFont* font = Ui::FontRead();
            const float size = font->FontSize * 1.15f;
            const float wrap = std::min(760.0f, io.DisplaySize.x - 160.0f);
            const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, wrap, sub.text.c_str());
            const ImVec2 pos((io.DisplaySize.x - std::min(ts.x, wrap)) * 0.5f, io.DisplaySize.y * 0.80f);
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(ImVec2(pos.x - 18.0f, pos.y - 8.0f), ImVec2(pos.x + std::min(ts.x, wrap) + 18.0f, pos.y + ts.y + 8.0f), IM_COL32(6, 6, 8, static_cast<int>(150.0f * a)));
            dl->AddText(font, size, pos, Ui::U32(Ui::Col::Bone, a), sub.text.c_str(), nullptr, wrap);
            if (sub.left <= 0.0f) s_Subtitles.erase(s_Subtitles.begin());
        }
        auto& reg = m_Scene.GetRegistry();
        const int story = m_Story.story;
        const int chapter = m_Story.chapter;
        const int kills = m_KillCount - m_Story.kills0;
        const glm::mat4 vp = m_Camera.GetViewProjection();

        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 20.0f, 20.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f));
        ImGui::Begin("StoryHUD", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
        ImGui::TextColored(Ui::Col::Gold, "%s - %s", kStories[story].title, ChapterOf(story, chapter).name);
        if (story == 1) {
            if (chapter == 0) {
                ImGui::TextColored(Ui::Col::Bone, "Tempo %s", Clock(m_Story.time).c_str());
                if (reg.valid(m_Story.boss)) {
                    if (const auto* hp = reg.try_get<HealthComponent>(m_Story.boss)) {
                        ImGui::TextColored(Ui::Col::Ember, "Gorvakh: portalo sotto meta' della sua vita");
                        const ImVec2 at = ImGui::GetCursorScreenPos();
                        Ui::Meter(std::clamp(hp->currentHealth / std::max(1.0f, hp->maxHealth), 0.0f, 1.0f), ImVec2(300.0f, 8.0f), Ui::Col::Ember);
                        ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x + 300.0f * kGorvakhGoal, at.y - 3.0f), ImVec2(at.x + 300.0f * kGorvakhGoal, at.y + 11.0f), Ui::U32(Ui::Col::Bone), 2.0f);
                    }
                    if (const auto* t = reg.try_get<TransformComponent>(m_Story.boss))
                        WorldMarker(vp, t->Position + glm::vec3(0.0f, 3.4f, 0.0f), "GORVAKH", Ui::U32(Ui::Col::Ember));
                }
                ImGui::TextColored(Ui::Col::Ash, "Solo artigli: SINISTRO normale, DESTRO pesante, Q parata");
            }
            else if (chapter == 1) {
                ImGui::TextColored(Ui::Col::Bone, "Tempo %s    Glatt abbattuti %d", Clock(m_Story.time).c_str(), kills);
                ImGui::TextColored(Ui::Col::Rust, "Punti di raccolta distrutti: %d su 3", m_Story.pointsDone);
                static const char* kNames[3] = { "PUNTO DI RACCOLTA I", "PUNTO DI RACCOLTA II", "PUNTO DI RACCOLTA III" };
                for (int k = 0; k < 3; ++k) {
                    if (m_Story.pointDone[k]) continue;
                    WorldMarker(vp, kGatherPoints[k] + glm::vec3(0.0f, 5.5f, 0.0f), kNames[k], Ui::U32(Ui::Col::Gold));
                    if (m_Story.pointProgress[k] > 0.0f) {
                        ImGui::TextColored(Ui::Col::Ember, "%s: lo state abbattendo", kNames[k]);
                        Ui::Meter(std::clamp(m_Story.pointProgress[k] / kGatherTime, 0.0f, 1.0f), ImVec2(300.0f, 6.0f), Ui::Col::Ember);
                    }
                }
                ImGui::TextColored(Ui::Col::Ash, "Stai accanto al punto, con i glatt intorno abbattuti");
            }
            else {
                const float left = std::max(0.0f, kVecchioHoldTime - m_Story.time);
                ImGui::TextColored(Ui::Col::Rust, "Reggi il fronte: ancora %s", Clock(left).c_str());
                Ui::Meter(std::clamp(m_Story.time / kVecchioHoldTime, 0.0f, 1.0f), ImVec2(300.0f, 6.0f), Ui::Col::Verdigris);
                static const char* kBelokh[3] = { "KONRAD", "ADELHEID", "DIETRICH" };
                for (size_t i = 0; i < m_Story.belokh.size() && i < 3; ++i) {
                    const entt::entity b = m_Story.belokh[i];
                    if (!reg.valid(b) || reg.all_of<CorpseComponent>(b)) continue;
                    if (const auto* t = reg.try_get<TransformComponent>(b))
                        WorldMarker(vp, t->Position + glm::vec3(0.0f, 2.9f, 0.0f), kBelokh[i], IM_COL32(235, 235, 225, 255));
                }
            }
        }
        else {
            ImGui::TextColored(Ui::Col::Bone, "Tempo %s    Vrepighs abbattuti %d", Clock(m_Story.time).c_str(), kills);
            if (chapter == 0) {
                glm::vec3 pos(0.0f);
                if (const auto* t = reg.try_get<TransformComponent>(m_PlayerEntity)) pos = t->Position;
                const float dist = glm::length(glm::vec2(pos.x - kMartusGoal.x, pos.z - kMartusGoal.z));
                ImGui::TextColored(Ui::Col::Rust, "Raggiungi il magazzino nord: %.0f m", dist);
                WorldMarker(vp, kMartusGoal + glm::vec3(0.0f, 2.5f, 0.0f), "MAGAZZINO NORD", Ui::U32(Ui::Col::Gold));
            }
            else if (chapter == 1) {
                if (m_Story.time < kOrdanHoldTime) ImGui::TextColored(Ui::Col::Rust, "Tieni la porta: i civili passano tra %s", Clock(kOrdanHoldTime - m_Story.time).c_str());
                else ImGui::TextColored(Ui::Col::Verdigris, "I civili sono passati. Resisti finche' puoi");
                const float bestT = m_Profile.story.value("ordan_best_time", 0.0f);
                if (bestT > 0.0f) ImGui::TextColored(Ui::Col::Ash, "Record: %d Vrepighs, %s", m_Profile.story.value("ordan_best_kills", 0), Clock(bestT).c_str());
            }
            else {
                if (!m_Story.bossSpawned) {
                    ImGui::TextColored(Ui::Col::Rust, "Il loro capo arriva tra %s", Clock(kKonradBossTime - m_Story.time).c_str());
                }
                else if (reg.valid(m_Story.boss)) {
                    if (const auto* hp = reg.try_get<HealthComponent>(m_Story.boss)) {
                        ImGui::TextColored(Ui::Col::Ember, "Il capo con la collana di frammenti");
                        Ui::Meter(std::clamp(hp->currentHealth / std::max(1.0f, hp->maxHealth), 0.0f, 1.0f), ImVec2(300.0f, 8.0f), Ui::Col::Ember);
                    }
                    if (const auto* t = reg.try_get<TransformComponent>(m_Story.boss))
                        WorldMarker(vp, t->Position + glm::vec3(0.0f, 2.8f, 0.0f), "IL CAPO", Ui::U32(Ui::Col::Ember));
                }
            }
        }
        ImGui::End();
    }

    void GameLayer::RenderStoryEnd() {
        ImGuiIO& io = ImGui::GetIO();
        const int story = m_Story.story;
        const int chapter = std::clamp(m_Story.chapter, 0, 2);
        const auto& voices = s_Voices[story][chapter];
        // prima le ultime righe del personaggio sul campo che si oscura (SPAZIO per passare oltre)
        if (s_OutroTime >= 0.0f) {
            s_OutroTime += std::min(io.DeltaTime, 0.1f);
            const float in = std::clamp(s_OutroTime / 1.0f, 0.0f, 1.0f);
            ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(4, 4, 6, static_cast<int>(215.0f * in)));
            DrawLines((m_Story.result == 1 || m_Story.result == 3) ? voices.win : voices.fall, s_OutroTime, 1.0f, nullptr);
            if (s_OutroTime >= s_OutroTotal || (s_OutroTime > 0.5f && SkipPressed())) s_OutroTime = -1.0f;
            return;
        }
        const int kills = m_KillCount - m_Story.kills0;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f));
        ImGui::Begin(kStories[story].title, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        const char* title = "";
        const char* text = "";
        const char* quote = "";
        ImVec4 col = Ui::Col::Gold;
        const bool won = m_Story.result == 1 || m_Story.result == 3;
        if (story == 1) {
            if (chapter == 0) {
                if (won) { title = "UN PASSO INDIETRO"; text = "Kragh ha fatto sanguinare Gorvakh davanti a tutta la sala."; col = Ui::Col::Verdigris; }
                else { title = "SPEZZATO"; text = "Gorvakh ha tenuto Kragh a terra con un piede sulla schiena."; quote = "\"A chi appartengono le tue pietre?\""; col = Ui::Col::Ember; }
            }
            else if (chapter == 1) {
                if (won) { title = "I PUNTI DI RACCOLTA SONO CADUTI"; text = "Il vargh ha spezzato le difese dei glatt su Vespera IV."; col = Ui::Col::Verdigris; }
                else { title = "CADUTO"; text = "Kragh e' caduto tra le piattaforme di Vespera IV."; col = Ui::Col::Ember; }
            }
            else {
                if (won) { title = "IL FRONTE HA RETTO"; text = "Il vecchio ha tenuto le corazze bianche il tempo della ritirata."; quote = "\"Sono tuo padre.\""; col = Ui::Col::Verdigris; }
                else { title = "TROPPO PRESTO"; text = "Il vecchio e' caduto prima che la ritirata passasse."; col = Ui::Col::Ember; }
            }
        }
        else if (chapter == 0) {
            if (won) { title = "CE L'HAI FATTA"; text = "Martus Kell ha raggiunto il magazzino nord, vivo."; quote = "\"Siamo del Kingship\", dissi."; col = Ui::Col::Verdigris; }
            else { title = "CADUTO"; text = "Martus Kell e' caduto nella Terza Serra, prima di raggiungere il magazzino."; col = Ui::Col::Ember; }
        }
        else if (chapter == 1) {
            if (m_Story.result == 3) { title = "LA FINE DELLA LINEA"; text = "Il sergente Ordan ha tenuto la porta finche' i civili non sono passati."; col = Ui::Col::Verdigris; }
            else { title = "LA PORTA HA CEDUTO TROPPO PRESTO"; text = "Il sergente Ordan e' caduto prima che i civili raggiungessero il condotto."; col = Ui::Col::Ember; }
        }
        else {
            if (won) { title = "LA LINEA HA RETTO"; text = "Il capo dei Vrepighs e' caduto e la sua collana di frammenti si e' spezzata."; quote = "\"Avete fatto il vostro dovere.\""; col = Ui::Col::Verdigris; }
            else { title = "CADUTO"; text = "Konrad von Eber e' caduto nella Terza Serra."; col = Ui::Col::Ember; }
        }
        Ui::Heading(title, col);
        // le parole del personaggio dal file delle voci; senza file restano quelle qui sopra
        const auto& closing = won ? voices.win : voices.fall;
        if (!closing.empty()) {
            for (const auto& l : closing) {
                if (l.rfind("- ", 0) == 0) ImGui::TextColored(Ui::Col::Brass, "%s", l.c_str());
                else ImGui::TextWrapped("%s", l.c_str());
            }
        }
        else {
            ImGui::TextWrapped("%s", text);
            if (quote[0]) ImGui::TextColored(Ui::Col::Brass, "%s", quote);
        }
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        ImGui::Text("Tempo: %s", Clock(m_Story.time).c_str());
        if (story == 1) {
            if (chapter == 1) { ImGui::Text("Glatt abbattuti: %d", kills); ImGui::Text("Punti di raccolta distrutti: %d su 3", m_Story.pointsDone); }
        }
        else ImGui::Text("Vrepighs abbattuti: %d", kills);
        if (story == 0 && chapter == 1 && m_Story.newRecord && m_Story.result == 3) ImGui::TextColored(Ui::Col::Gold, "Nuovo record!");
        ImGui::Spacing();
        RenderMatchRecap();
        ImGui::Spacing();
        const int readChapter = voices.readAfter;
        const bool read = readChapter > 0 && Ui::Button("Leggi cosa e' successo", ImVec2(-1.0f, 40.0f), Ui::Btn::Primary);
        const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const bool again = Ui::PlainButton(won ? "Rigioca il capitolo" : "Riprova", ImVec2(halfW, 40.0f));
        ImGui::SameLine();
        const bool back = Ui::PlainButton("Torna al menu", ImVec2(halfW, 40.0f));
        ImGui::End();

        if (read) {   // dalla partita al racconto: si apre il capitolo di questo momento
            s_Story = story;
            StoryLeaveToMenu();
            s_Reading = true;
            s_ReadChapter = readChapter - 1;
            s_ReadScrollTop = true;
            return;
        }

        if (again) {
            s_Story = story;
            ResetMatchState(true);
            StartStoryChapter(chapter);   // il garage resta da parte: si rimette a posto all'uscita
            if (m_GameState == GameState::Gameplay)
                if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
        else if (back) StoryLeaveToMenu();
    }

    void GameLayer::StoryLeaveToMenu() {
        auto& reg = m_Scene.GetRegistry();
        std::vector<entt::entity> toDestroy;
        for (auto e : reg.view<HealthComponent>(entt::exclude<PlayerTag>)) toDestroy.push_back(e);
        for (auto e : toDestroy) {
            if (!reg.valid(e)) continue;
            m_PhysicsSystem.DestroyJoltBody(m_Scene, e);
            m_Scene.DestroyEntity(e);
        }
        ClearStoryProps(m_Scene);
        ClearPlayerDeathState();
        if (auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity)) hp->currentHealth = hp->maxHealth;
        m_IsPaused = false;
        m_PlayerDeathHandled = false;
        m_GameState = GameState::MainMenu;   // il cambio di stato chiama ResetMatchState ed EndStory
        m_CurrentTab = MenuTab::Story;
        if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }

    void GameLayer::EndStory() {
        if (!m_Story.active) return;
        auto& reg = m_Scene.GetRegistry();
        ClearStoryProps(m_Scene);
        if (m_Story.addedTeam && reg.valid(m_PlayerEntity) && reg.all_of<TeamComponent>(m_PlayerEntity)) reg.remove<TeamComponent>(m_PlayerEntity);
        m_PlayerRoster = m_StoryBackupRoster;
        m_SelectedSquadIndices = m_StoryBackupSquad;
        m_SelectedMapId = m_StoryBackupMap;
        m_SelectedEventIndex = m_StoryBackupEvent;
        m_StoryBackupRoster.clear();
        m_StoryBackupSquad.clear();
        m_Story = StoryState{};
        if (m_PlayerRoster.empty()) return;
        if (m_SelectedSquadIndices.empty() || m_SelectedSquadIndices[0] >= static_cast<int>(m_PlayerRoster.size())) m_SelectedSquadIndices = { 0 };
        if (m_SelectedVaultIndex >= static_cast<int>(m_PlayerRoster.size())) m_SelectedVaultIndex = 0;
        ApplyUnitVisuals(m_PlayerRoster[m_SelectedSquadIndices[0]]);   // nel garage torna la tua unita'
    }

}
