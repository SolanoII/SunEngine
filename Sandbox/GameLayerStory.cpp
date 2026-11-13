// PVE-STORY: le storie di The Sun's Darkest Hours da rigiocare, fuori dal sistema delle squadre.
// Ogni capitolo si gioca con un personaggio fisso della storia, sulla sua mappa, con il suo obiettivo.
//
// Primo Incontro (Anno 999 del Calendario Regale, Vespera IV, Terza Serra), mappa "warzone_sector4":
//   0  Martus Kell      fuga dalla serra centrale (A) al magazzino nord (B): arrivarci vivo
//   1  Sergente Ordan   rifugio del livello tre, stanza chiusa: ondate dall'anticamera fino alla fine (record)
//   2  Konrad von Eber  ondate nei campi; dopo 5 minuti arriva il capo con la collana di frammenti: abbatterlo
//
// Il garage del giocatore viene messo da parte all'inizio del capitolo e rimesso a posto all'uscita (EndStory),
// e intanto il salvataggio del profilo usa la copia messa da parte (vedi SaveProfileIfChanged).
#include "GameLayer.h"
#include "UiTheme.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Logger/Log.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Tags.h"
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
        StoryText s_PrimoIncontro;
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
        ChapterVoice s_Voices[3];
        bool s_VoicesLoaded = false;
        struct Subtitle { std::string text; float total = 0.0f, left = 0.0f; };
        std::vector<Subtitle> s_Subtitles;         // battute in coda (in basso, una alla volta)
        std::set<std::string> s_VoiceFired;        // battute gia' dette in questo capitolo
        float s_IntroTime = -1.0f, s_IntroTotal = 0.0f;   // prologo: secondi trascorsi (-1 = nessuno)
        float s_OutroTime = -1.0f, s_OutroTotal = 0.0f;   // ultime righe prima della schermata di fine
        bool s_SkipWasDown = true;

        void PlainPunctuation(std::string& s);
        void LoadVoices() {
            if (s_VoicesLoaded) return;
            s_VoicesLoaded = true;
            std::ifstream in(FindValidDir("Stories") + "primo_incontro_voci.json", std::ios::binary);
            if (!in) { SUN_CORE_WARN("PVE-STORY: voci dei capitoli non trovate (primo_incontro_voci.json)"); return; }
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
                    ChapterVoice& v = s_Voices[i];
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
                if (a > 0.0f) dl->AddText(font, size, ImVec2((io.DisplaySize.x - std::min(ts.x, wrap)) * 0.5f, y), Ui::U32(Ui::Col::Bone, a), line.c_str(), nullptr, wrap);
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

        struct StoryChapter {
            const char* name;
            const char* role;
            const char* quote;
            const char* objective;
            glm::vec3 start;
        };
        const StoryChapter kChapters[3] = {
            { "Martus Kell", "Laborator della Terza Serra, milizia planetaria",
              "\"Kell! Apri gli occhi!\"",
              "La linea e' caduta. Attraversa la serra e raggiungi il magazzino nord, vivo. Non fermarti: i Vrepighs ti inseguono.",
              { 2.0f, 0.5f, -50.0f } },   // dentro la serra centrale, fra due filari
            { "Sergente Ordan", "Sergente della milizia di Vespera IV",
              "\"Qualcuno deve chiudere quella porta.\"",
              "Tieni il rifugio del livello tre. I Vrepighs sfondano dall'anticamera, ondata dopo ondata, fino alla fine.",
              { 2.0f, 0.5f, 94.0f } },
            { "Konrad von Eber", "Ordo Domus Humilitatis",
              "\"Allora non possiamo lasciare che raggiungano i rifugi.\"",
              "Ferma l'assalto alla Terza Serra. Dopo 5 minuti arriva il loro capo, quello con la collana di frammenti viola: abbattilo.",
              { 0.0f, 0.5f, 20.0f } },
        };
        // Martus: il magazzino nord (porta a sud, z = 95)
        const glm::vec3 kMartusGoal{ -40.0f, 0.0f, 89.0f };
        constexpr float kMartusGoalRadius = 4.5f;
        // Ordan: l'anticamera sigillata davanti alla porta del rifugio
        const glm::vec3 kOrdanBreach[3] = { { -1.5f, 0.5f, 82.0f }, { 2.0f, 0.5f, 81.5f }, { 5.5f, 0.5f, 82.0f } };

        // Il personaggio del capitolo, come unita' giocabile (armi e armature della storia)
        TrainedUnit MakeStoryUnit(int chapter) {
            TrainedUnit u;
            if (chapter == 0) {
                u = TrainedUnit{ "Martus Kell", "imperium_conscript", glm::vec4(0.80f, 0.62f, 0.52f, 1.0f) };
                u.ApplyDefaultLoadout();
                u.primaryWeaponId = "autogun_standard";   // il fucile automatico assegnato alle milizie planetarie
            }
            else if (chapter == 1) {
                u = TrainedUnit{ "Sergente Ordan", "imperium_guardsman_recruit", glm::vec4(0.66f, 0.50f, 0.40f, 1.0f) };
                u.ApplyDefaultLoadout();
                u.primaryWeaponId = "guardsman_autogun";
                u.secondaryWeaponId = "guardsman_autopistol";
            }
            else {
                // lo stesso Konrad che si sblocca nei Vestments (Sworn Brother): livrea bianca, nera e avorio
                // dell'Ordo Domus Humilitatis, spada lunga dalla lama scura, equipaggiamento migliore
                u = TrainedUnit{ "Konrad von Eber", "spacemarines_sworn_brother", glm::vec4(0.78f, 0.62f, 0.52f, 1.0f) };
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

        // a sinistra le storie (per ora una), a destra quella scelta
        ImGui::BeginChild("##storie", ImVec2(230.0f, 0.0f), true);
        Ui::Heading("STORIE");
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.88f, 0.52f, 0.24f, 0.55f));
        ImGui::Selectable("I. Primo Incontro", true);
        ImGui::PopStyleColor();
        // la storia si gioca (capitoli) oppure si legge (racconto completo)
        ImGui::Indent(12.0f);
        if (ImGui::Selectable("Gioca i capitoli", !s_Reading)) s_Reading = false;
        if (ImGui::Selectable("Leggi il racconto", s_Reading)) { s_Reading = true; s_ReadScrollTop = true; }
        ImGui::Unindent(12.0f);
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

        ImGui::BeginChild("##storia", ImVec2(0.0f, 0.0f), false);
        Ui::Title("PRIMO INCONTRO");
        ImGui::TextColored(Ui::Col::Brass, "Anno 999 del Calendario Regale  -  Vespera IV, pianeta agricolo  -  Dominio di Aurel");
        ImGui::Spacing();
        ImGui::TextWrapped("\"La prima cosa che capii fu che il cielo stava cadendo.\" Le piattaforme orbitali precipitano sulla "
                           "Terza Serra e i Vrepighs attraversano i campi. La milizia dei Laboratores tiene la strada di servizio, "
                           "i civili scendono nei rifugi, e nessuno sta venendo a salvarli. O almeno cosi' credono.");
        ImGui::TextColored(Ui::Col::Ash, "Mappa: Vespera IV - Terza Serra");
        ImGui::Spacing(); Ui::Divider(); ImGui::Spacing();

        const auto& st = m_Profile.story;
        const float cardW = std::max(220.0f, (ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f);
        int start = -1;
        for (int c = 0; c < 3; ++c) {
            if (c > 0) ImGui::SameLine();
            ImGui::PushID(c);
            ImGui::BeginChild("##capitolo", ImVec2(cardW, 0.0f), true);
            ImGui::TextColored(Ui::Col::Gold, "CAPITOLO %s", Ui::Roman(c + 1));
            Ui::TextIn(Ui::FontHeading(), Ui::Col::Bone, kChapters[c].name);
            ImGui::TextColored(Ui::Col::Ash, "%s", kChapters[c].role);
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, Ui::Col::Brass);
            ImGui::TextWrapped("%s", kChapters[c].quote);
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::TextColored(Ui::Col::Gold, "OBIETTIVO");
            ImGui::TextWrapped("%s", kChapters[c].objective);
            ImGui::Spacing();
            if (c == 1) {
                const int bestK = st.is_object() ? st.value("ordan_best_kills", 0) : 0;
                const float bestT = st.is_object() ? st.value("ordan_best_time", 0.0f) : 0.0f;
                if (bestT > 0.0f) ImGui::TextColored(Ui::Col::Verdigris, "Record: %d Vrepighs, porta tenuta %s", bestK, Clock(bestT).c_str());
                else ImGui::TextColored(Ui::Col::Ash, "Nessun record");
            }
            else {
                const bool done = st.is_object() && st.value(c == 0 ? "martus_done" : "konrad_done", false);
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
    // stretta, corpo grande). L'ultimo capitolo aperto resta nel profilo (story.read_primo_incontro).
    void GameLayer::RenderStoryReader() {
        StoryText& tx = s_PrimoIncontro;
        LoadStoryText(tx, "primo_incontro.txt");
        ImGui::BeginChild("##racconto", ImVec2(0.0f, 0.0f), false);
        if (tx.chapters.empty()) {
            Ui::Title("PRIMO INCONTRO");
            ImGui::TextColored(Ui::Col::Ash, "Il racconto non e' stato trovato (GameData/Stories/primo_incontro.txt).");
            ImGui::EndChild();
            return;
        }
        const int count = static_cast<int>(tx.chapters.size());
        if (s_ReadChapter < 0) s_ReadChapter = m_Profile.story.is_object() ? m_Profile.story.value("read_primo_incontro", 0) : 0;
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
        LoadVoices();
        int play = -1;
        for (int g = 0; g < 3; ++g)
            for (int n : s_Voices[g].playFrom) if (n == s_ReadChapter + 1) play = g;
        int startPlay = -1;
        if (play >= 0) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::SetCursorPosX(left);
            ImGui::TextColored(Ui::Col::Brass, "Questo momento si puo' rivivere: %s.", kChapters[play].name);
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
            m_Profile.story["read_primo_incontro"] = s_ReadChapter;   // il segno resta nel profilo
            SaveProfileIfChanged();
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // Inizio, aggiornamento e fine di un capitolo
    // ---------------------------------------------------------------------------------------------------------
    void GameLayer::StartStoryChapter(int chapter) {
        chapter = std::clamp(chapter, 0, 2);
        if (!m_Story.active) {   // il garage si mette da parte una volta sola (anche quando si rigioca)
            m_StoryBackupRoster = m_PlayerRoster;
            m_StoryBackupSquad = m_SelectedSquadIndices;
            m_StoryBackupMap = m_SelectedMapId;
            m_StoryBackupEvent = m_SelectedEventIndex;
        }
        m_PlayerRoster = { MakeStoryUnit(chapter) };
        m_SelectedSquadIndices = { 0 };
        m_SelectedMapId = kStoryMap;
        m_SelectedEventIndex = 3;   // come il Sandbox: niente ondate della sopravvivenza, le ondate le fa il capitolo

        m_Story = StoryState{};
        m_Story.active = true;
        m_Story.chapter = chapter;
        StartDeployment(0);
        if (m_GameState != GameState::Gameplay) { EndStory(); return; }   // non e' partito

        // il personaggio parte dal punto del suo capitolo
        auto& reg = m_Scene.GetRegistry();
        if (auto* t = reg.try_get<TransformComponent>(m_PlayerEntity)) {
            t->Position = kChapters[chapter].start;
            m_PhysicsSystem.SetBodyPosition(m_Scene, m_PlayerEntity, t->Position);
        }
        m_Story.kills0 = m_KillCount;
        m_Story.spawnTimer = chapter == 1 ? 6.0f : 4.0f;   // un attimo per guardarsi intorno
        m_Story.waveSize = chapter == 0 ? 2 : 1;
        m_PartyToasts.push_back({ std::string(kChapters[chapter].name) + ": " + kChapters[chapter].objective, 6.0f });
        // Voce del personaggio: prologo sullo schermo scuro (la partita aspetta), poi le battute in campo
        LoadVoices();
        s_Subtitles.clear();
        s_VoiceFired.clear();
        s_OutroTime = -1.0f;
        s_SkipWasDown = true;
        if (!s_Voices[chapter].prologue.empty()) {
            s_IntroTime = 0.0f;
            s_IntroTotal = LinesTotal(s_Voices[chapter].prologue);
            m_IsPaused = true;
        }
        else s_IntroTime = -1.0f;
        SUN_CORE_INFO("PVE-STORY: Primo Incontro, capitolo {0} ({1})", chapter + 1, kChapters[chapter].name);
    }

    void GameLayer::UpdateStory(float deltaTime, const glm::vec3& playerPos) {
        if (!m_Story.active || m_Story.result != 0) return;
        auto& reg = m_Scene.GetRegistry();
        m_Story.time += deltaTime;

        int alive = 0;
        for (auto e : reg.view<HealthComponent>(entt::exclude<PlayerTag, CorpseComponent, TrainingDummyComponent, SquigComponent>)) {
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
                SpawnNetOrk(p);
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
                const float total = glm::length(glm::vec2(kChapters[0].start.x - kMartusGoal.x, kChapters[0].start.z - kMartusGoal.z));
                const float left = glm::length(glm::vec2(playerPos.x - kMartusGoal.x, playerPos.z - kMartusGoal.z));
                if (left < 28.0f) StoryVoice("vicino");
                else if (left < total * 0.55f) StoryVoice("meta");
            }
            else if (chapter == 1) {
                if (m_Story.time > 45.0f) StoryVoice("tiene_1");
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
                const entt::entity boss = SpawnNetOrk(p);
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

        if (m_Story.result != 0) {   // fine del capitolo: la simulazione si ferma sotto la schermata
            if (!s_Voices[chapter].win.empty()) { s_OutroTime = 0.0f; s_OutroTotal = LinesTotal(s_Voices[chapter].win); s_SkipWasDown = true; }
            m_IsPaused = true;
            if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }

    void GameLayer::StoryOnPlayerDeath() {
        if (!m_Story.active || m_Story.result != 0) return;
        if (m_Story.chapter == 1) {
            // Ordan: e' la fine che conosciamo. Conta quanto ha tenuto la porta.
            m_Story.result = 3;
            const int kills = m_KillCount - m_Story.kills0;
            auto& st = m_Profile.story;
            const float bestT = st.value("ordan_best_time", 0.0f);
            const int bestK = st.value("ordan_best_kills", 0);
            if (kills > bestK || (kills == bestK && m_Story.time > bestT)) {
                st["ordan_best_kills"] = kills;
                st["ordan_best_time"] = m_Story.time;
                m_Story.newRecord = true;
            }
            ApplyMatchXp(1.0f, "La porta ha retto per " + Clock(m_Story.time));
        }
        else {
            m_Story.result = 2;
            ApplyMatchXp(0.5f, m_Story.chapter == 0 ? "Martus e' caduto prima del magazzino" : "Konrad e' caduto");
        }
        // le ultime righe del personaggio, prima della schermata di fine
        LoadVoices();
        if (!s_Voices[m_Story.chapter].fall.empty()) { s_OutroTime = 0.0f; s_OutroTotal = LinesTotal(s_Voices[m_Story.chapter].fall); s_SkipWasDown = true; }
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
        const auto& lines = s_Voices[std::clamp(m_Story.chapter, 0, 2)].lines;
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
        const std::string header = std::string("PRIMO INCONTRO  -  ") + kChapters[chapter].name;
        DrawLines(s_Voices[chapter].prologue, s_IntroTime, fade, header.c_str());
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
        const int chapter = m_Story.chapter;
        const int kills = m_KillCount - m_Story.kills0;
        const glm::mat4 vp = m_Camera.GetViewProjection();

        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 20.0f, 20.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f));
        ImGui::Begin("StoryHUD", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
        ImGui::TextColored(Ui::Col::Gold, "PRIMO INCONTRO - %s", kChapters[chapter].name);
        ImGui::TextColored(Ui::Col::Bone, "Tempo %s    Vrepighs abbattuti %d", Clock(m_Story.time).c_str(), kills);
        if (chapter == 0) {
            glm::vec3 pos(0.0f);
            if (const auto* t = reg.try_get<TransformComponent>(m_PlayerEntity)) pos = t->Position;
            const float dist = glm::length(glm::vec2(pos.x - kMartusGoal.x, pos.z - kMartusGoal.z));
            ImGui::TextColored(Ui::Col::Rust, "Raggiungi il magazzino nord: %.0f m", dist);
            WorldMarker(vp, kMartusGoal + glm::vec3(0.0f, 2.5f, 0.0f), "MAGAZZINO NORD", Ui::U32(Ui::Col::Gold));
        }
        else if (chapter == 1) {
            ImGui::TextColored(Ui::Col::Rust, "Tieni la porta del rifugio");
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
        ImGui::End();
    }

    void GameLayer::RenderStoryEnd() {
        ImGuiIO& io = ImGui::GetIO();
        const int chapter = m_Story.chapter;
        // prima le ultime righe del personaggio sul campo che si oscura (SPAZIO per passare oltre)
        if (s_OutroTime >= 0.0f) {
            s_OutroTime += std::min(io.DeltaTime, 0.1f);
            const float in = std::clamp(s_OutroTime / 1.0f, 0.0f, 1.0f);
            ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(4, 4, 6, static_cast<int>(215.0f * in)));
            const auto& v = s_Voices[std::clamp(chapter, 0, 2)];
            DrawLines(m_Story.result == 1 ? v.win : v.fall, s_OutroTime, 1.0f, nullptr);
            if (s_OutroTime >= s_OutroTotal || (s_OutroTime > 0.5f && SkipPressed())) s_OutroTime = -1.0f;
            return;
        }
        const int kills = m_KillCount - m_Story.kills0;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f));
        ImGui::Begin("PRIMO INCONTRO", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        const char* title = "";
        const char* text = "";
        const char* quote = "";
        ImVec4 col = Ui::Col::Gold;
        if (chapter == 0) {
            if (m_Story.result == 1) { title = "CE L'HAI FATTA"; text = "Martus Kell ha raggiunto il magazzino nord, vivo."; quote = "\"Siamo del Kingship\", dissi."; col = Ui::Col::Verdigris; }
            else { title = "CADUTO"; text = "Martus Kell e' caduto nella Terza Serra, prima di raggiungere il magazzino."; col = Ui::Col::Ember; }
        }
        else if (chapter == 1) {
            title = "LA FINE DELLA LINEA";
            text = "Il sergente Ordan ha tenuto la porta finche' ha potuto.";
            quote = "\"Siamo noi i rinforzi.\"";
            col = Ui::Col::Rust;
        }
        else {
            if (m_Story.result == 1) { title = "LA LINEA HA RETTO"; text = "Il capo dei Vrepighs e' caduto e la sua collana di frammenti si e' spezzata."; quote = "\"Avete fatto il vostro dovere.\""; col = Ui::Col::Verdigris; }
            else { title = "CADUTO"; text = "Konrad von Eber e' caduto nella Terza Serra."; col = Ui::Col::Ember; }
        }
        Ui::Heading(title, col);
        ImGui::TextWrapped("%s", text);
        if (quote[0]) ImGui::TextColored(Ui::Col::Brass, "%s", quote);
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        ImGui::Text("Tempo: %s", Clock(m_Story.time).c_str());
        ImGui::Text("Vrepighs abbattuti: %d", kills);
        if (chapter == 1 && m_Story.newRecord) ImGui::TextColored(Ui::Col::Gold, "Nuovo record!");
        ImGui::Spacing();
        RenderMatchRecap();
        ImGui::Spacing();
        const int readChapter = s_Voices[std::clamp(chapter, 0, 2)].readAfter;
        const bool read = readChapter > 0 && Ui::Button("Leggi cosa e' successo", ImVec2(-1.0f, 40.0f), Ui::Btn::Primary);
        const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        const bool again = Ui::PlainButton("Rigioca il capitolo", ImVec2(halfW, 40.0f));
        ImGui::SameLine();
        const bool back = Ui::PlainButton("Torna alla PVE-STORY", ImVec2(halfW, 40.0f));
        ImGui::End();

        if (read) {   // dalla partita al racconto: si apre il capitolo di questo momento
            StoryLeaveToMenu();
            s_Reading = true;
            s_ReadChapter = readChapter - 1;
            s_ReadScrollTop = true;
            return;
        }

        if (again) {
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
