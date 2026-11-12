// Squadra, lobby e collegamento peer to peer: interfaccia nel garage e compagni visibili accanto alla propria unita'.
// La rete vera sta in Engine/Net/NetParty (questo file la usa soltanto).
#define NOMINMAX
#include "GameLayer.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>

namespace SunEngine {

    namespace {
        const char* PrivacyLabel(Net::Privacy p) {
            return p == Net::Privacy::Open ? "APERTA" : (p == Net::Privacy::Closed ? "CHIUSA" : "PRIVATA");
        }
        const char* PrivacyHelp(Net::Privacy p) {
            return p == Net::Privacy::Open ? "Visibile in rete: entra chiunque."
                 : (p == Net::Privacy::Closed ? "Visibile in rete: entra chi ha il codice, gli altri chiedono a te."
                                              : "Invisibile: entra solo chi ha il codice.");
        }
        // qualita' del collegamento in parole e colore
        ImVec4 PingColor(float rtt, float loss) {
            if (loss > 5.0f || rtt > 160.0f) return Ui::Col::Ember;
            if (loss > 1.0f || rtt > 80.0f) return Ui::Col::Rust;
            return Ui::Col::Verdigris;
        }
    }

    void GameLayer::InitParty() {
        if (m_Profile.netId == 0) m_Profile.netId = Net::RandomPeerId();
        if (m_Profile.netName.empty()) m_Profile.netName = "Giocatore";
        std::snprintf(m_PartyNameBuf, sizeof(m_PartyNameBuf), "%s", m_Profile.netName.c_str());
        if (!m_Party.Init(m_Profile.netId, m_Profile.netName)) SUN_CORE_WARN("Squadra: rete non disponibile ({0})", m_Party.StatusText());
        else SUN_CORE_INFO("Squadra: rete pronta sulla porta UDP {0}", m_Party.Port());
        m_Party.SetServer(m_Profile.netServer);   // server d'incontro (schermata SERVER); vuoto = solo rete locale
        std::snprintf(m_ServerAddrBuf, sizeof(m_ServerAddrBuf), "%s", m_Profile.netServer.c_str());
        m_Party.onHostChanged = [](Net::PeerId host, bool iAmHost, bool migrated) {
            if (host) SUN_CORE_INFO("Squadra: host della partita {0}{1}", iAmHost ? "(io)" : "(un compagno)", migrated ? " dopo una migrazione" : "");
        };
        InitNetMatch();   // messaggi della partita vera (GameLayerNetMatch.cpp)
    }

    void GameLayer::ClearPartyActors() {
        auto& reg = m_Scene.GetRegistry();
        for (auto& pa : m_PartyActors) {
            if (reg.valid(pa.actor.visual)) m_Scene.DestroyEntity(pa.actor.visual);
            if (reg.valid(pa.actor.weapon)) m_Scene.DestroyEntity(pa.actor.weapon);
        }
        m_PartyActors.clear();
    }

    void GameLayer::UpdateParty(float realDt) {
        m_Party.Update(realDt);
        UpdateNetMatch(realDt);   // partita in rete: stati, nemici, danni, ingresso e uscita
        // la mia unita' del garage: i compagni la vedono accanto alla loro
        if (m_SelectedVaultIndex >= 0 && m_SelectedVaultIndex < static_cast<int>(m_PlayerRoster.size()))
            m_Party.SetUnit(m_PlayerRoster[m_SelectedVaultIndex].blueprintId);
        for (auto& e : m_Party.PopEvents()) { m_PartyToasts.push_back({ e, 5.0f }); SUN_CORE_INFO("Squadra: {0}", e); }
        for (auto& t : m_PartyToasts) t.time -= realDt;
        m_PartyToasts.erase(std::remove_if(m_PartyToasts.begin(), m_PartyToasts.end(), [](const PartyToast& t) { return t.time <= 0.0f; }), m_PartyToasts.end());
        if (m_PartyToasts.size() > 4) m_PartyToasts.erase(m_PartyToasts.begin(), m_PartyToasts.end() - 4);

        // --- compagni nel garage: le loro unita' in piedi accanto alla mia ---
        auto& reg = m_Scene.GetRegistry();
        const bool show = m_GameState == GameState::MainMenu && !m_ShowCustomizeModal &&
            (m_CurrentTab == MenuTab::StasisVault || m_CurrentTab == MenuTab::Deployment) &&
            m_Party.GetState() == Net::PartyState::InParty && reg.valid(m_PlayerEntity) && reg.all_of<TransformComponent>(m_PlayerEntity) &&
            AnimatedBodyReady(AnimatedBodyKind::Human);
        if (!show) { if (!m_PartyActors.empty()) ClearPartyActors(); return; }

        const auto& pt = reg.get<TransformComponent>(m_PlayerEntity);
        const float yr = glm::radians(pt.Rotation.y);
        const glm::vec3 right(std::cos(yr), 0.0f, -std::sin(yr)), fwd(-std::sin(yr), 0.0f, -std::cos(yr));
        // via chi e' uscito o ha cambiato unita'
        for (size_t i = 0; i < m_PartyActors.size();) {
            bool keep = false;
            for (const auto& mb : m_Party.Members()) if (!mb.self && mb.id == m_PartyActors[i].id && mb.unitId == m_PartyActors[i].actor.unitId) keep = true;
            if (!keep) {
                if (reg.valid(m_PartyActors[i].actor.visual)) m_Scene.DestroyEntity(m_PartyActors[i].actor.visual);
                if (reg.valid(m_PartyActors[i].actor.weapon)) m_Scene.DestroyEntity(m_PartyActors[i].actor.weapon);
                m_PartyActors.erase(m_PartyActors.begin() + i);
            }
            else ++i;
        }
        int slot = 0;
        for (const auto& mb : m_Party.Members()) {
            if (mb.self) continue;
            const int k = slot++;
            if (mb.unitId.empty() || !BlueprintManager::Get().GetUnit(mb.unitId)) continue;   // unita' che questa versione non conosce
            // posti: destra, sinistra, destra piu' in la'... un passo indietro rispetto a me
            const float side = (k % 2 == 0 ? 1.0f : -1.0f) * (1.9f + 1.9f * static_cast<float>(k / 2));
            const glm::vec3 pos = pt.Position + right * side - fwd * 0.5f;
            PartyActor* found = nullptr;
            for (auto& pa : m_PartyActors) if (pa.id == mb.id) found = &pa;
            if (!found) {
                m_PartyActors.emplace_back();
                found = &m_PartyActors.back();
                found->id = mb.id;
                SpawnShowcaseActor(found->actor, mb.unitId, pos, pt.Rotation.y, 977u * static_cast<unsigned int>(k + 7));
            }
            found->name = mb.name;
            found->actor.pos = pos;
            found->actor.facing = pt.Rotation.y;
        }
        // stessa animazione della vetrina, ma sempre a riposo
        const int savedAnim = m_ShowcaseAnim;
        m_ShowcaseAnim = static_cast<int>(ShowcaseAnim::Idle);
        for (auto& pa : m_PartyActors) {
            if (auto* vt = reg.try_get<TransformComponent>(pa.actor.visual)) {
                vt->Position = pa.actor.pos;
                vt->Rotation = glm::vec3(0.0f, pa.actor.facing + m_AnimatedYawOffset, 0.0f);
                vt->AimYaw = pa.actor.facing;
            }
            UpdateShowcaseActor(pa.actor, realDt);
        }
        m_ShowcaseAnim = savedAnim;
    }

    // Nomi dei compagni sopra la testa delle loro unita' nel garage, e avvisi della squadra in alto
    void GameLayer::RenderPartyOverlay() {
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const glm::mat4 vp = m_Camera.GetViewProjection();
        for (const auto& pa : m_PartyActors) {
            const float h = pa.actor.kind == AnimatedBodyKind::Ogryn ? 2.9f : (pa.actor.kind == AnimatedBodyKind::Gretchin ? 1.2f : 2.25f);
            const glm::vec4 clip = vp * glm::vec4(pa.actor.pos + glm::vec3(0.0f, h * pa.actor.scale, 0.0f), 1.0f);
            if (clip.w <= 0.01f) continue;
            const ImVec2 p((clip.x / clip.w * 0.5f + 0.5f) * io.DisplaySize.x, (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * io.DisplaySize.y);
            ImFont* f = Ui::FontHeading();
            const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, pa.name.c_str());
            const ImVec2 a(p.x - ts.x * 0.5f - 10.0f, p.y - ts.y - 8.0f), b(p.x + ts.x * 0.5f + 10.0f, p.y);
            Ui::ChamferRect(dl, a, b, 6.0f, IM_COL32(10, 10, 12, 200), Ui::U32(Ui::Col::Brass, 0.9f), 1.0f);
            dl->AddText(f, f->FontSize, ImVec2(a.x + 10.0f, a.y + 4.0f), Ui::U32(Ui::Col::Bone), pa.name.c_str());
        }
        // avvisi ("X e' entrato", "Nuovo host: Y")
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        float y = 70.0f;
        for (const auto& t : m_PartyToasts) {
            const float alpha = std::clamp(t.time / 0.6f, 0.0f, 1.0f);
            const ImVec2 ts = ImGui::CalcTextSize(t.text.c_str());
            const ImVec2 a(io.DisplaySize.x * 0.5f - ts.x * 0.5f - 14.0f, y), b(io.DisplaySize.x * 0.5f + ts.x * 0.5f + 14.0f, y + ts.y + 10.0f);
            Ui::ChamferRect(fg, a, b, 6.0f, IM_COL32(10, 10, 12, static_cast<int>(215 * alpha)), Ui::U32(Ui::Col::Phosphor, 0.8f * alpha), 1.0f);
            fg->AddText(ImVec2(a.x + 14.0f, a.y + 5.0f), Ui::U32(Ui::Col::Bone, alpha), t.text.c_str());
            y += ts.y + 16.0f;
        }
    }

    void GameLayer::RenderPartyPanel() {
        ImGuiIO& io = ImGui::GetIO();
        const Net::PartyState state = m_Party.GetState();
        const auto& members = m_Party.Members();

        // Chiuso: solo una linguetta in basso al centro
        if (!m_PartyPanelOpen) {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 14.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::Begin("##SquadraChiusa", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
            char label[64];
            if (state == Net::PartyState::InParty) std::snprintf(label, sizeof(label), "SQUADRA  %d / %d", static_cast<int>(members.size()), m_Party.MaxMembers());
            else if (state == Net::PartyState::Joining) std::snprintf(label, sizeof(label), "SQUADRA  collegamento...");
            else std::snprintf(label, sizeof(label), "SQUADRA");
            if (Ui::Button(label, ImVec2(240.0f, 36.0f), state == Net::PartyState::InParty ? Ui::Btn::Primary : Ui::Btn::Normal)) m_PartyPanelOpen = true;
            ImGui::End();
            return;
        }

        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 14.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(std::min(620.0f, io.DisplaySize.x - 780.0f < 480.0f ? 480.0f : io.DisplaySize.x - 780.0f), 0.0f));
        bool open = true;
        ImGui::Begin("SQUADRA", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        if (!open) m_PartyPanelOpen = false;
        const float fullW = ImGui::GetContentRegionAvail().x;

        if (state == Net::PartyState::Offline) {
            ImGui::TextColored(Ui::Col::Ember, "Rete non disponibile su questo computer.");
            ImGui::TextColored(Ui::Col::Ash, "%s", m_Party.StatusText().c_str());
            ImGui::End();
            return;
        }

        // ---------- nessuna squadra: nome, crea, entra ----------
        if (state == Net::PartyState::Idle) {
            ImGui::TextColored(Ui::Col::Ash, "Il tuo nome");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputText("##nome", m_PartyNameBuf, sizeof(m_PartyNameBuf))) {
                if (m_PartyNameBuf[0]) { m_Profile.netName = m_PartyNameBuf; m_Party.SetName(m_Profile.netName); }
            }
            ImGui::SameLine(0.0f, 16.0f);
            if (Ui::PlainButton("ELENCO SERVER")) { m_CurrentTab = MenuTab::Servers; m_Party.RefreshServerList(); }
            ImGui::Spacing();
            Ui::Heading("CREA UNA SQUADRA");
            for (int i = 0; i < 3; ++i) {
                const Net::Privacy p = static_cast<Net::Privacy>(i);
                if (ImGui::RadioButton(PrivacyLabel(p), m_PartyNewPrivacy == i)) m_PartyNewPrivacy = i;
                if (i < 2) ImGui::SameLine(0.0f, 18.0f);
            }
            ImGui::TextColored(Ui::Col::Ash, "%s", PrivacyHelp(static_cast<Net::Privacy>(m_PartyNewPrivacy)));
            if (Ui::Button("CREA SQUADRA", ImVec2(220.0f, 36.0f), Ui::Btn::Primary)) m_Party.Create(static_cast<Net::Privacy>(m_PartyNewPrivacy), 5);

            ImGui::Spacing();
            Ui::Heading("ENTRA COL CODICE");
            ImGui::SetNextItemWidth(220.0f);
            const bool enter = ImGui::InputTextWithHint("##codice", "6 cifre, o indirizzo", m_PartyCodeBuf, sizeof(m_PartyCodeBuf), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (Ui::PlainButton("INCOLLA")) {
                if (const char* clip = ImGui::GetClipboardText()) {
                    // dagli appunti: via spazi e a capo
                    size_t n = 0;
                    for (const char* c = clip; *c && n + 1 < sizeof(m_PartyCodeBuf); ++c) if (!std::isspace(static_cast<unsigned char>(*c))) m_PartyCodeBuf[n++] = *c;
                    m_PartyCodeBuf[n] = 0;
                }
            }
            ImGui::SameLine();
            if ((Ui::Button("ENTRA", ImVec2(110.0f, 0.0f), Ui::Btn::Primary, m_PartyCodeBuf[0] != 0) || enter) && m_PartyCodeBuf[0]) m_Party.Join(m_PartyCodeBuf);

            const auto& lobbies = m_Party.Lobbies();
            ImGui::Spacing();
            char head[64]; std::snprintf(head, sizeof(head), "SQUADRE IN RETE   %d", static_cast<int>(lobbies.size()));
            Ui::Heading(head);
            if (lobbies.empty()) ImGui::TextColored(Ui::Col::Ash, "Nessuna squadra aperta o chiusa visibile nella tua rete.");
            for (const auto& lb : lobbies) {
                ImGui::PushID(static_cast<int>(lb.leaderId & 0x7FFFFFFF));
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", lb.leaderName.c_str());
                ImGui::SameLine(fullW * 0.45f);
                ImGui::TextColored(Ui::Col::Ash, "%d / %d", lb.members, lb.maxMembers);
                ImGui::SameLine(fullW * 0.60f);
                ImGui::TextColored(lb.privacy == Net::Privacy::Open ? Ui::Col::Verdigris : Ui::Col::Rust, "%s", PrivacyLabel(lb.privacy));
                ImGui::SameLine(fullW - 120.0f);
                if (Ui::PlainButton(lb.privacy == Net::Privacy::Open ? "ENTRA" : "CHIEDI", ImVec2(120.0f, 0.0f))) m_Party.Join(lb);
                ImGui::PopID();
            }
            if (!m_Party.StatusText().empty()) { ImGui::Spacing(); ImGui::TextColored(Ui::Col::Rust, "%s", m_Party.StatusText().c_str()); }
            ImGui::End();
            return;
        }

        // ---------- sto entrando ----------
        if (state == Net::PartyState::Joining) {
            ImGui::TextColored(Ui::Col::Gold, "%s", m_Party.StatusText().c_str());
            ImGui::Spacing();
            if (Ui::PlainButton("ANNULLA", ImVec2(140.0f, 0.0f))) m_Party.Leave();
            ImGui::End();
            return;
        }

        // ---------- in squadra ----------
        const bool leader = m_Party.IsLeader();
        const std::string code = m_Party.Code6();
        {   // codice a 6 cifre, grande, col pulsante per copiarlo
            ImFont* ft = Ui::FontTitle();
            const std::string shown = code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
            ImGui::TextColored(Ui::Col::Ash, "CODICE DELLA SQUADRA");
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 ts = ft->CalcTextSizeA(ft->FontSize * 1.15f, FLT_MAX, 0.0f, shown.c_str());
            ImGui::GetWindowDrawList()->AddText(ft, ft->FontSize * 1.15f, p, Ui::U32(Ui::Col::Gold), shown.c_str());
            ImGui::Dummy(ImVec2(ts.x + 14.0f, ts.y));
            ImGui::SameLine();
            ImGui::BeginGroup();
            if (Ui::PlainButton("COPIA", ImVec2(90.0f, 0.0f))) { ImGui::SetClipboardText(code.c_str()); m_PartyToasts.push_back({ "Codice copiato: " + code, 3.0f }); }
            ImGui::SameLine();
            if (Ui::PlainButton("COPIA CODICE ESTESO", ImVec2(190.0f, 0.0f))) { ImGui::SetClipboardText(m_Party.InviteCode().c_str()); m_PartyToasts.push_back({ "Codice esteso copiato (per chi entra via internet)", 3.0f }); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Il codice a 6 cifre funziona in rete locale e, col server d'incontro (schermata SERVER),\nanche via internet senza aprire porte. Quello esteso contiene indirizzo e porta:\nserve solo senza server, con la porta UDP %d aperta sul router del capo.", static_cast<int>(m_Party.Port()));
            ImGui::EndGroup();
        }
        // tipo di lobby
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Ui::Col::Ash, "Lobby");
        ImGui::SameLine();
        if (leader) {
            for (int i = 0; i < 3; ++i) {
                const Net::Privacy p = static_cast<Net::Privacy>(i);
                if (ImGui::RadioButton(PrivacyLabel(p), m_Party.GetPrivacy() == p)) m_Party.SetPrivacy(p);
                ImGui::SameLine(0.0f, 14.0f);
            }
            ImGui::NewLine();
        }
        else ImGui::TextColored(Ui::Col::Bone, "%s", PrivacyLabel(m_Party.GetPrivacy()));
        ImGui::TextColored(Ui::Col::Ash, "%s", PrivacyHelp(m_Party.GetPrivacy()));

        // compagni: chi comanda, collegamento, chi sarebbe il miglior host
        ImGui::Spacing();
        char head[64]; std::snprintf(head, sizeof(head), "COMPAGNI   %d / %d", static_cast<int>(members.size()), m_Party.MaxMembers());
        Ui::Heading(head);
        Net::PeerId best = 0; float bestScore = 1e9f;
        if (members.size() > 1) for (const auto& mb : members) if (mb.score < bestScore) { bestScore = mb.score; best = mb.id; }
        const Net::PeerId hostNow = m_Party.InSession() ? m_Party.HostId() : 0;
        bool selfReady = false;
        for (const auto& mb : members) {
            ImGui::PushID(static_cast<int>(mb.id & 0x7FFFFFFF));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(mb.self ? Ui::Col::Gold : Ui::Col::Bone, "%s%s", mb.name.c_str(), mb.self ? "  (tu)" : "");
            if (mb.self) selfReady = mb.ready;
            float x = fullW * 0.34f;
            ImGui::SameLine(x);
            if (mb.leader) ImGui::TextColored(Ui::Col::Gold, "CAPO"); else ImGui::TextColored(Ui::Col::Ash, " ");
            x = fullW * 0.44f;
            ImGui::SameLine(x);
            if (mb.self) ImGui::TextColored(Ui::Col::Ash, "-");
            else if (!mb.linked) ImGui::TextColored(Ui::Col::Rust, "collego...");
            else ImGui::TextColored(PingColor(mb.rttMs, mb.lossPct), "%.0f ms%s", mb.rttMs, mb.relayed ? " ponte" : (mb.lossPct > 1.0f ? " !" : ""));
            if (!mb.self && mb.linked && ImGui::IsItemHovered()) ImGui::SetTooltip(mb.relayed ? "Latenza %.0f ms (stimata), via ponte del server d'incontro:\ni vostri router non si aprono l'uno all'altro" : "Latenza %.0f ms, pacchetti persi %.1f%%", mb.rttMs, mb.lossPct);
            x = fullW * 0.60f;
            ImGui::SameLine(x);
            if (hostNow == mb.id) ImGui::TextColored(Ui::Col::Phosphor, "HOST");
            else if (best == mb.id) ImGui::TextColored(Ui::Col::Ash, "miglior host");
            else ImGui::TextColored(Ui::Col::Ash, " ");
            if (ImGui::IsItemHovered() && members.size() > 1) ImGui::SetTooltip("Qualita' verso tutti gli altri: %.0f (piu' basso = meglio)", mb.score);
            x = fullW * 0.80f;
            ImGui::SameLine(x);
            if (mb.ready) ImGui::TextColored(Ui::Col::Verdigris, "PRONTO"); else ImGui::TextColored(Ui::Col::Ash, "in attesa");
            if (leader && !mb.self) {
                ImGui::SameLine(fullW - 22.0f);
                if (ImGui::SmallButton("X")) m_Party.Kick(mb.id);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Espelli dalla squadra");
            }
            ImGui::PopID();
        }
        // richieste d'ingresso (lobby chiusa)
        if (leader && !m_Party.Requests().empty()) {
            ImGui::Spacing();
            Ui::Heading("CHIEDONO DI ENTRARE", Ui::Col::Rust);
            const std::vector<Net::JoinRequest> reqs = m_Party.Requests();   // copia: rispondendo l'elenco cambia
            for (const auto& rq : reqs) {
                ImGui::PushID(static_cast<int>(rq.id & 0x7FFFFFFF));
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", rq.name.c_str());
                ImGui::SameLine(fullW - 230.0f);
                if (Ui::PlainButton("ACCETTA", ImVec2(110.0f, 0.0f))) m_Party.AnswerRequest(rq.id, true);
                ImGui::SameLine();
                if (Ui::PlainButton("RIFIUTA", ImVec2(110.0f, 0.0f))) m_Party.AnswerRequest(rq.id, false);
                ImGui::PopID();
            }
        }

        // collegamento di partita: host scelto per la connessione migliore, successione se cade
        ImGui::Spacing();
        Ui::Heading(m_Party.Playing() ? "PARTITA IN CORSO" : "PEER PRINCIPALE");
        if (m_Party.InSession()) {
            std::string hostName = "?", order;
            for (const auto& mb : members) if (mb.id == m_Party.HostId()) hostName = mb.name;
            for (Net::PeerId id : m_Party.Succession()) {
                if (id == m_Party.HostId()) continue;
                for (const auto& mb : members) if (mb.id == id) order += (order.empty() ? "" : ", ") + mb.name;
            }
            ImGui::TextColored(Ui::Col::Ash, "Host:"); ImGui::SameLine(); ImGui::TextColored(Ui::Col::Phosphor, "%s%s", hostName.c_str(), m_Party.IsHost() ? "  (tu)" : "");
            if (!order.empty()) { ImGui::TextColored(Ui::Col::Ash, "Se cade subentra, in ordine:"); ImGui::SameLine(); ImGui::TextColored(Ui::Col::Bone, "%s", order.c_str()); }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fullW);
            ImGui::TextColored(Ui::Col::Ash, "L'host muove i nemici: e' chi ha la connessione migliore fra quelli in partita, e se esce o cade "
                               "subentra il successivo. Ognuno porta la sua squadra e i suoi rientri. La partita la avvia il capo "
                               "dalla schermata SCHIERAMENTO (Difesa del Settore o Sandbox).");
            ImGui::PopTextWrapPos();
            if (m_Party.Playing()) {
                if (!m_Net.active && Ui::Button("ENTRA IN PARTITA", ImVec2(200.0f, 34.0f), Ui::Btn::Primary)) NetJoinRunningMatch();
                if (leader) {
                    if (!m_Net.active) ImGui::SameLine();
                    if (Ui::PlainButton("TERMINA LA PARTITA PER TUTTI", ImVec2(260.0f, 0.0f))) m_Party.EndSession();
                }
            }
            else if (leader && members.size() > 1) {
                if (Ui::Button("VAI ALLO SCHIERAMENTO", ImVec2(220.0f, 34.0f), Ui::Btn::Primary)) { m_CurrentTab = MenuTab::Deployment; m_PartyPanelOpen = false; }
            }
        }

        ImGui::Spacing();
        if (Ui::Button(selfReady ? "NON PRONTO" : "PRONTO", ImVec2(160.0f, 36.0f), selfReady ? Ui::Btn::Normal : Ui::Btn::Primary)) m_Party.SetReady(!selfReady);
        ImGui::SameLine();
        if (Ui::Button("LASCIA LA SQUADRA", ImVec2(200.0f, 36.0f), Ui::Btn::Danger)) m_Party.Leave();
        ImGui::End();
    }
}

namespace SunEngine {

    // Schermata SERVER: le lobby aperte (rete locale e server d'incontro), con la possibilita' di entrare anche in una
    // partita gia' iniziata; a destra la propria lobby (renderla visibile, nome, codice) e il server d'incontro.
    void GameLayer::RenderServerBrowserUI() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 60.0f));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - 60.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f, 16.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##SERVER", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(2);
        Ui::Backdrop(ImGui::GetWindowDrawList(), ImVec2(0.0f, 60.0f), io.DisplaySize);
        m_Party.RefreshServerList();   // finche' la schermata e' aperta l'elenco si aggiorna da solo

        const Net::PartyState state = m_Party.GetState();
        Ui::Title("SERVER");
        {   // stato del server d'incontro, a destra del titolo
            const bool on = m_Party.ServerOnline();
            char st[96];
            if (m_Party.ServerAddress().empty()) std::snprintf(st, sizeof(st), "SOLO RETE LOCALE");
            else if (on) std::snprintf(st, sizeof(st), "SERVER D'INCONTRO COLLEGATO  %.0f ms", m_Party.ServerRttMs());
            else std::snprintf(st, sizeof(st), "SERVER D'INCONTRO NON RAGGIUNGIBILE");
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
            Ui::Chip(st, m_Party.ServerAddress().empty() ? Ui::Col::Ash : (on ? Ui::Col::Verdigris : Ui::Col::Ember));
        }
        Ui::Divider();

        const float rightW = std::min(470.0f, io.DisplaySize.x * 0.36f);
        const float leftW = ImGui::GetContentRegionAvail().x - rightW - 18.0f;
        const float listH = ImGui::GetContentRegionAvail().y;

        // ---------------- elenco ----------------
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.40f));
        ImGui::BeginChild("##lista", ImVec2(leftW, listH), true);
        Ui::Heading("LOBBY APERTE");
        ImGui::Checkbox("Mostra anche le piene", &m_ServerShowFull);
        ImGui::Spacing();
        const auto& lobbies = m_Party.Lobbies();
        int shown = 0;
        if (ImGui::BeginTable("##lobby", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("LOBBY", ImGuiTableColumnFlags_WidthStretch, 2.6f);
            ImGui::TableSetupColumn("HOST", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("GIOCATORI", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("STATO", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("PING", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();
            for (const auto& lb : lobbies) {
                const bool full = lb.members >= lb.maxMembers;
                if (full && !m_ServerShowFull) continue;
                ++shown;
                ImGui::TableNextRow(0, 30.0f);
                ImGui::PushID(static_cast<int>(lb.leaderId & 0x7FFFFFFF));
                ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
                ImGui::TextColored(Ui::Col::Bone, "%s", lb.name.empty() ? ("Lobby di " + lb.leaderName).c_str() : lb.name.c_str());
                if (!lb.online) { ImGui::SameLine(); ImGui::TextColored(Ui::Col::Phosphor, " LAN"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nella tua rete locale"); }
                ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextColored(Ui::Col::Ash, "%s", lb.leaderName.c_str());
                ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextColored(full ? Ui::Col::Ember : Ui::Col::Bone, "%d / %d", lb.members, lb.maxMembers);
                ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
                if (lb.inSession) ImGui::TextColored(Ui::Col::Rust, "IN PARTITA"); else ImGui::TextColored(Ui::Col::Verdigris, "IN LOBBY");
                if (lb.privacy == Net::Privacy::Closed) { ImGui::SameLine(); ImGui::TextColored(Ui::Col::Ash, " chiusa"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Chiusa: l'host deve accettarti (o entra col codice)"); }
                ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
                if (lb.pingMs >= 0) ImGui::TextColored(lb.pingMs > 120 ? Ui::Col::Rust : Ui::Col::Ash, "%d", lb.pingMs); else ImGui::TextColored(Ui::Col::Ash, "-");
                if (ImGui::IsItemHovered() && lb.pingMs >= 0) ImGui::SetTooltip("Latenza dell'host verso il server d'incontro (indicativa)");
                ImGui::TableNextColumn();
                const bool can = state == Net::PartyState::Idle && !full;
                if (Ui::Button(lb.privacy == Net::Privacy::Closed ? "CHIEDI" : "ENTRA", ImVec2(104.0f, 26.0f), Ui::Btn::Primary, can)) m_Party.Join(lb);
                if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(full ? "Lobby al completo" : "Esci prima dalla tua lobby o squadra");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (shown == 0) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Ui::Col::Ash, m_Party.ServerAddress().empty()
                ? "Nessuna lobby nella tua rete locale. Per vedere quelle via internet imposta un server d'incontro qui a destra."
                : (m_Party.ServerOnline() ? "Nessuna lobby aperta in questo momento: aprine una tu." : "Il server d'incontro non risponde: si vedono solo le lobby della rete locale."));
            ImGui::PopTextWrapPos();
        }
        if (state == Net::PartyState::Joining) { ImGui::Spacing(); ImGui::TextColored(Ui::Col::Gold, "%s", m_Party.StatusText().c_str()); ImGui::SameLine(); if (ImGui::SmallButton("annulla")) m_Party.Leave(); }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // ---------------- destra: la mia lobby, codice, server ----------------
        ImGui::SameLine(0.0f, 18.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.045f, 0.043f, 0.042f, 0.92f));
        ImGui::BeginChild("##mia", ImVec2(rightW, listH), true);
        if (!m_LobbyNameBuf[0]) std::snprintf(m_LobbyNameBuf, sizeof(m_LobbyNameBuf), "%s", m_Party.LobbyName().empty() ? ("Lobby di " + m_Profile.netName).c_str() : m_Party.LobbyName().c_str());
        if (state == Net::PartyState::InParty) {
            Ui::Heading("LA TUA LOBBY");
            const bool leader = m_Party.IsLeader();
            if (leader) {
                ImGui::TextColored(Ui::Col::Ash, "Nome");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##nomelobby", m_LobbyNameBuf, sizeof(m_LobbyNameBuf))) m_Party.SetLobbyName(m_LobbyNameBuf);
                bool visible = m_Party.GetPrivacy() != Net::Privacy::Private;
                if (ImGui::Checkbox("Visibile nella schermata SERVER", &visible)) m_Party.SetPrivacy(visible ? Net::Privacy::Open : Net::Privacy::Private);
                if (visible) {
                    bool closed = m_Party.GetPrivacy() == Net::Privacy::Closed;
                    ImGui::SameLine();
                    if (ImGui::Checkbox("Chiedi a me chi entra", &closed)) m_Party.SetPrivacy(closed ? Net::Privacy::Closed : Net::Privacy::Open);
                }
            }
            else ImGui::TextColored(Ui::Col::Bone, "%s", m_Party.LobbyName().c_str());
            const std::string code = m_Party.Code6();
            ImGui::Spacing();
            ImGui::TextColored(Ui::Col::Ash, "CODICE");
            ImGui::SameLine();
            Ui::TextIn(Ui::FontStrong(), Ui::Col::Gold, code.size() == 6 ? (code.substr(0, 3) + " " + code.substr(3)).c_str() : code.c_str());
            ImGui::SameLine();
            if (Ui::PlainButton("COPIA")) { ImGui::SetClipboardText(code.c_str()); m_PartyToasts.push_back({ "Codice copiato: " + code, 3.0f }); }
            std::string hostName = "-";
            for (const auto& mb : m_Party.Members()) if (mb.id == m_Party.HostId()) hostName = mb.name;
            ImGui::TextColored(Ui::Col::Ash, "Giocatori"); ImGui::SameLine(); ImGui::TextColored(Ui::Col::Bone, "%d / %d", static_cast<int>(m_Party.Members().size()), m_Party.MaxMembers());
            ImGui::SameLine(0.0f, 20.0f);
            ImGui::TextColored(Ui::Col::Ash, "Host"); ImGui::SameLine(); ImGui::TextColored(Ui::Col::Phosphor, "%s%s", hostName.c_str(), m_Party.IsHost() ? " (tu)" : "");
            ImGui::Spacing();
            if (Ui::Button("APRI IL PANNELLO SQUADRA", ImVec2(-1, 32.0f))) { m_CurrentTab = MenuTab::StasisVault; m_PartyPanelOpen = true; }
            if (Ui::Button("LASCIA LA LOBBY", ImVec2(-1, 32.0f), Ui::Btn::Danger)) m_Party.Leave();
        }
        else {
            Ui::Heading("APRI UNA LOBBY");
            ImGui::TextColored(Ui::Col::Ash, "Nome");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##nomelobby", m_LobbyNameBuf, sizeof(m_LobbyNameBuf));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Ui::Col::Ash, "Compare nell'elenco a chi gioca in rete locale e, col server d'incontro, a tutti. Fai da host finche' "
                               "non entra qualcuno con una connessione nettamente migliore.");
            ImGui::PopTextWrapPos();
            const bool idle = state == Net::PartyState::Idle;
            if (Ui::Button("APRI LOBBY VISIBILE", ImVec2(-1, 38.0f), Ui::Btn::Primary, idle)) {
                m_Party.SetLobbyName(m_LobbyNameBuf);
                if (m_Party.Create(Net::Privacy::Open, 5)) m_Party.SetLobbyName(m_LobbyNameBuf);
            }
            if (Ui::Button("APRI LOBBY PRIVATA (SOLO CODICE)", ImVec2(-1, 32.0f), Ui::Btn::Normal, idle)) {
                if (m_Party.Create(Net::Privacy::Private, 5)) m_Party.SetLobbyName(m_LobbyNameBuf);
            }
            ImGui::Spacing();
            Ui::Heading("ENTRA COL CODICE");
            ImGui::SetNextItemWidth(rightW - 230.0f);
            const bool enter = ImGui::InputTextWithHint("##codice2", "6 cifre", m_PartyCodeBuf, sizeof(m_PartyCodeBuf), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (Ui::PlainButton("INCOLLA")) {
                if (const char* clip = ImGui::GetClipboardText()) {
                    size_t n = 0;
                    for (const char* c = clip; *c && n + 1 < sizeof(m_PartyCodeBuf); ++c) if (!std::isspace(static_cast<unsigned char>(*c))) m_PartyCodeBuf[n++] = *c;
                    m_PartyCodeBuf[n] = 0;
                }
            }
            ImGui::SameLine();
            if ((Ui::Button("ENTRA", ImVec2(90.0f, 0.0f), Ui::Btn::Primary, idle && m_PartyCodeBuf[0] != 0) || enter) && idle && m_PartyCodeBuf[0]) m_Party.Join(m_PartyCodeBuf);
        }

        // server d'incontro
        ImGui::Spacing(); ImGui::Spacing();
        Ui::Heading("SERVER D'INCONTRO", Ui::Col::Phosphor);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(Ui::Col::Ash, "Serve a trovarsi via internet senza aprire porte: elenco delle lobby, codici, apertura dei router "
                           "(e ponte se non si aprono). La partita resta fra i giocatori. Lo fa girare SunCollectorServer su un computer sempre "
                           "acceso: tutti devono usare lo stesso indirizzo.");
        ImGui::PopTextWrapPos();
        ImGui::SetNextItemWidth(rightW - 150.0f);
        ImGui::InputTextWithHint("##srv", "indirizzo:porta (es. 203.0.113.7:27640)", m_ServerAddrBuf, sizeof(m_ServerAddrBuf));
        ImGui::SameLine();
        if (Ui::PlainButton("COLLEGA", ImVec2(110.0f, 0.0f))) {
            m_Profile.netServer = m_ServerAddrBuf;
            m_Party.SetServer(m_Profile.netServer);
            m_Party.RefreshServerList();
        }
        if (!m_Party.ServerAddress().empty() && !m_Party.ServerOnline()) ImGui::TextColored(Ui::Col::Rust, "%s", m_Party.StatusText().c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::End();
    }
}
