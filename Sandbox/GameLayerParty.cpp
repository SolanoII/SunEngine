// Squadra, lobby e collegamento peer to peer: interfaccia nel garage e compagni visibili accanto alla propria unita'.
// La rete vera sta in Engine/Net/NetParty (questo file la usa soltanto).
#define NOMINMAX
#include "GameLayer.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
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
        m_Party.onHostChanged = [](Net::PeerId host, bool iAmHost, bool migrated) {
            if (host) SUN_CORE_INFO("Squadra: host della partita {0}{1}", iAmHost ? "(io)" : "(un compagno)", migrated ? " dopo una migrazione" : "");
        };
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
        // Collegamento di prova: l'host conta i secondi e li pubblica come "istantanea della partita"; chi subentra
        // dopo una caduta riparte dall'ultimo valore ricevuto (la partita vera usera' lo stesso meccanismo)
        if (m_Party.IsHost()) {
            m_PartyLinkClock += realDt;
            if (m_PartyLinkClock >= 1.0f) {
                m_PartyLinkClock = 0.0f;
                std::uint32_t seconds = 0;
                const auto& snap = m_Party.Snapshot();
                if (snap.size() == 4) seconds = static_cast<std::uint32_t>(snap[0]) | (static_cast<std::uint32_t>(snap[1]) << 8) | (static_cast<std::uint32_t>(snap[2]) << 16) | (static_cast<std::uint32_t>(snap[3]) << 24);
                ++seconds;
                m_Party.PublishSnapshot({ static_cast<std::uint8_t>(seconds), static_cast<std::uint8_t>(seconds >> 8), static_cast<std::uint8_t>(seconds >> 16), static_cast<std::uint8_t>(seconds >> 24) });
            }
        }
        else m_PartyLinkClock = 0.0f;
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
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Il codice a 6 cifre funziona fra computer della stessa rete (anche virtuale).\nQuello esteso contiene indirizzo e porta: serve a chi entra via internet,\ncon la porta UDP %d aperta sul router del capo.", static_cast<int>(m_Party.Port()));
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
            else ImGui::TextColored(PingColor(mb.rttMs, mb.lossPct), "%.0f ms%s", mb.rttMs, mb.lossPct > 1.0f ? " !" : "");
            if (!mb.self && mb.linked && ImGui::IsItemHovered()) ImGui::SetTooltip("Latenza %.0f ms, pacchetti persi %.1f%%", mb.rttMs, mb.lossPct);
            x = fullW * 0.60f;
            ImGui::SameLine(x);
            if (hostNow == mb.id) ImGui::TextColored(Ui::Col::Phosphor, "HOST");
            else if (!hostNow && best == mb.id) ImGui::TextColored(Ui::Col::Phosphor, "miglior host");
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
        Ui::Heading("COLLEGAMENTO DI PARTITA");
        if (m_Party.InSession()) {
            std::string hostName = "?", order;
            for (const auto& mb : members) if (mb.id == m_Party.HostId()) hostName = mb.name;
            for (Net::PeerId id : m_Party.Succession()) {
                if (id == m_Party.HostId()) continue;
                for (const auto& mb : members) if (mb.id == id) order += (order.empty() ? "" : ", ") + mb.name;
            }
            ImGui::TextColored(Ui::Col::Ash, "Host:"); ImGui::SameLine(); ImGui::TextColored(Ui::Col::Phosphor, "%s%s", hostName.c_str(), m_Party.IsHost() ? "  (tu)" : "");
            {   // tempo di collegamento contato dall'host: continua anche dopo un cambio di host
                const auto& snap = m_Party.Snapshot();
                if (snap.size() == 4) {
                    const unsigned sec = static_cast<unsigned>(snap[0]) | (static_cast<unsigned>(snap[1]) << 8) | (static_cast<unsigned>(snap[2]) << 16) | (static_cast<unsigned>(snap[3]) << 24);
                    ImGui::SameLine(0.0f, 24.0f);
                    ImGui::TextColored(Ui::Col::Ash, "Collegati da"); ImGui::SameLine();
                    ImGui::TextColored(Ui::Col::Bone, "%u:%02u", sec / 60u, sec % 60u);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lo conta l'host. Se l'host esce o cade, il successivo riparte da qui:\nprova a chiudere il gioco dell'host durante il collegamento.");
                }
            }
            if (!order.empty()) { ImGui::TextColored(Ui::Col::Ash, "Se cade subentra, in ordine:"); ImGui::SameLine(); ImGui::TextColored(Ui::Col::Bone, "%s", order.c_str()); }
            if (leader && Ui::PlainButton("CHIUDI IL COLLEGAMENTO", ImVec2(240.0f, 0.0f))) m_Party.EndSession();
        }
        else {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fullW);
            ImGui::TextColored(Ui::Col::Ash, "All'avvio fa da host chi ha la connessione migliore verso tutti; se esce o cade subentra il successivo. "
                               "La partita giocata insieme arriva col prossimo aggiornamento: per ora si prova il collegamento.");
            ImGui::PopTextWrapPos();
            if (leader) {
                if (Ui::Button("PROVA IL COLLEGAMENTO", ImVec2(240.0f, 34.0f), Ui::Btn::Normal, members.size() > 1)) m_Party.StartSession();
            }
        }

        ImGui::Spacing();
        if (Ui::Button(selfReady ? "NON PRONTO" : "PRONTO", ImVec2(160.0f, 36.0f), selfReady ? Ui::Btn::Normal : Ui::Btn::Primary)) m_Party.SetReady(!selfReady);
        ImGui::SameLine();
        if (Ui::Button("LASCIA LA SQUADRA", ImVec2(200.0f, 36.0f), Ui::Btn::Danger)) m_Party.Leave();
        ImGui::End();
    }
}
