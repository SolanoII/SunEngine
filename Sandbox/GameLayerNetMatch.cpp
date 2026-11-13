// Partita in rete: la squadra gioca la stessa partita (Difesa del Settore e Sandbox).
//
// Chi fa cosa:
//   - OGNUNO muove la sua unita' e decide i danni dei suoi colpi (come in solitario, sulle copie dei nemici che ha
//     qui). I danni fatti alle copie vanno all'host, che li toglie ai nemici veri.
//   - L'HOST (chi ha la connessione migliore fra quelli in partita, vedi Net::Party) muove i nemici: ondate, IA,
//     colpi dell'orda. Manda a tutti dove sono e come stanno; chi viene colpito dall'orda riceve il danno.
//   - Se l'host esce dalla partita, cade o passa il ruolo, il successivo trasforma le sue copie in nemici veri e
//     riparte dall'ultima istantanea (ondata, tempi): la partita continua.
// Le unita' degli altri giocatori qui sono copie (NetGhostComponent): si muovono, sparano e colpiscono per finta,
// e muoiono quando lo dice il loro proprietario.
#define NOMINMAX
#include "GameLayer.h"
#include "Engine/Net/ServerProtocol.h"
#include "Engine/ECS/VfxEvents.h"
#include "Engine/ECS/HitUtils.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace SunEngine {

    namespace {
        using Net::Wire::Writer;
        using Net::Wire::Reader;

        // messaggi di gioco (primo byte)
        enum NetMsg : std::uint8_t {
            NM_Start = 'M',       // il capo avvia: mappa, modalita', numero della partita
            NM_Leave = 'L',       // sono uscito dalla partita
            NM_State = 'P',       // la mia unita' (20 al secondo)
            NM_Unit = 'U',        // com'e' fatta la mia unita' (quando cambia, e ogni tanto)
            NM_Enemies = 'E',     // host: dove sono e come stanno i nemici
            NM_Damage = 'D',      // all'host: danni che ho fatto alle copie dei nemici
            NM_HitPlayer = 'H',   // host: l'orda ha colpito questo giocatore
            NM_Killed = 'K',      // host: questo nemico e' morto (e chi l'ha ucciso)
            NM_Shots = 'S',       // i miei colpi (solo l'effetto)
            NM_BotShots = 'B',    // host: colpi dei bot (solo l'effetto)
            NM_Squad = 'Q',       // Vs Bot: la mia squadra (unita', chi e' caduto, chi e' in campo)
            NM_Roster = 'R',      // Vs Bot, host: squadre di tutti, bot, tempo, riscaldamento, fine partita
            NM_TeamSwitch = 'T',  // Vs Bot, all'host: voglio passare all'altra squadra
            NM_Despawn = 'X',     // host: questo nemico sparisce senza morire
            NM_VsJoin = 'J',      // Vs Bot gia' avviato: posso entrare? (all'host, nel riscaldamento)
            NM_VsLineup = 'F',    // host: roster aggiornato (qualcuno ha preso il posto di un bot), per chi l'ha chiesto e per tutti
            NM_VsJoinNo = 'N',    // host: troppo tardi o niente posti, aspetta la prossima partita
        };
        constexpr std::uint8_t kSnapMagic = 0xA7;   // istantanea della partita (Party::PublishSnapshot)
        constexpr float kStateRate = 1.0f / 20.0f, kEnemyRate = 1.0f / 15.0f, kSnapRate = 1.5f, kUnitResend = 3.0f;
        constexpr int kEnemiesPerPacket = 40;       // sotto la dimensione di un pacchetto UDP

        void WF(Writer& w, float f) { std::uint32_t u; std::memcpy(&u, &f, 4); w.U32(u); }
        float RF(Reader& r) { const std::uint32_t u = r.U32(); float f; std::memcpy(&f, &u, 4); return std::isfinite(f) ? f : 0.0f; }
        void WV(Writer& w, const glm::vec3& v) { WF(w, v.x); WF(w, v.y); WF(w, v.z); }
        glm::vec3 RV(Reader& r) { const float x = RF(r), y = RF(r); return glm::vec3(x, y, RF(r)); }
        // numeri piccoli in 16 bit (posizioni a 2 cm, angoli a 0,01 gradi)
        void WQ(Writer& w, float v, float scale) { w.U16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(std::lround(v * scale), -32767L, 32767L)))); }
        float RQ(Reader& r, float scale) { return static_cast<float>(static_cast<std::int16_t>(r.U16())) / scale; }
        float WrapDeg(float a) { a = std::fmod(a + 180.0f, 360.0f); if (a < 0.0f) a += 360.0f; return a - 180.0f; }
        float LerpDeg(float a, float b, float t) { return a + WrapDeg(b - a) * t; }

        struct SnapInfo { std::uint32_t matchId = 0; std::string map; int eventIdx = 0; bool waves = false; float waveTimer = 0.0f; int waveCount = 1; };
        bool ParseSnapshot(const std::vector<std::uint8_t>& b, SnapInfo& out) {
            if (b.size() < 2 || b[0] != kSnapMagic) return false;
            Reader r(b.data() + 1, b.size() - 1);
            out.matchId = r.U32(); out.map = r.Str(); out.eventIdx = r.U8();
            out.waves = r.U8() != 0; out.waveTimer = RF(r); out.waveCount = r.U16();
            return r.ok && out.matchId != 0;
        }

        // Unita' in campo: com'e' fatta (per la copia degli altri)
        void WriteUnit(Writer& w, const TrainedUnit& u) {
            w.Str(u.blueprintId); w.Str(u.customName);
            w.Str(u.primaryWeaponId); w.Str(u.secondaryWeaponId); w.Str(u.meleeWeaponId);
            for (int i = 0; i < 3; ++i) w.Str(u.equipmentSlots[i]);
            w.Str(u.armorHeadId); w.Str(u.armorTorsoId); w.Str(u.armorLeftArmId); w.Str(u.armorRightArmId); w.Str(u.armorLeftLegId); w.Str(u.armorRightLegId);
            WF(w, u.armorColor.r); WF(w, u.armorColor.g); WF(w, u.armorColor.b); WF(w, u.armorColor.a);
        }
        bool ReadUnit(Reader& r, TrainedUnit& u) {
            u.blueprintId = r.Str(); u.customName = r.Str();
            u.primaryWeaponId = r.Str(); u.secondaryWeaponId = r.Str(); u.meleeWeaponId = r.Str();
            for (int i = 0; i < 3; ++i) u.equipmentSlots[i] = r.Str();
            u.armorHeadId = r.Str(); u.armorTorsoId = r.Str(); u.armorLeftArmId = r.Str(); u.armorRightArmId = r.Str(); u.armorLeftLegId = r.Str(); u.armorRightLegId = r.Str();
            const float cr = RF(r), cg = RF(r), cb = RF(r);
            u.armorColor = glm::vec4(cr, cg, cb, RF(r));
            return r.ok && !u.blueprintId.empty();
        }
        std::string UnitKey(const TrainedUnit& u) {
            Writer w; WriteUnit(w, u);
            return std::string(w.b.begin(), w.b.end());
        }

        // Roster del Vs Bot deciso dal capo: chi gioca in quale squadra, i bot con le loro squadre (i giocatori veri
        // portano la loro: arriva con NM_Squad)
        void WriteRoster(Writer& w, int tier, const std::vector<VsBotPlayer>& players) {
            w.U8(static_cast<std::uint8_t>(tier));
            w.U8(static_cast<std::uint8_t>(std::min<size_t>(players.size(), 20)));
            for (size_t i = 0; i < players.size() && i < 20; ++i) {
                const auto& p = players[i];
                w.U8(static_cast<std::uint8_t>(p.team)); w.U64(p.owner); w.U8(p.human ? 1 : 0); w.Str(p.name); w.U8(static_cast<std::uint8_t>(p.style));
                const size_t n = p.human ? 0 : std::min<size_t>(p.squad.size(), 40);
                w.U8(static_cast<std::uint8_t>(n));
                for (size_t k = 0; k < n; ++k) { WriteUnit(w, p.squad[k].unit); w.U16(static_cast<std::uint16_t>(p.squad[k].points)); }
            }
        }
        bool ReadRoster(const std::vector<std::uint8_t>& b, int& tier, std::vector<VsBotPlayer>& out) {
            if (b.empty()) return false;
            Reader r(b.data(), b.size());
            tier = r.U8();
            const int n = r.U8();
            out.clear();
            for (int i = 0; i < n && r.ok; ++i) {
                VsBotPlayer p;
                p.team = r.U8() == 1 ? 1 : 0; p.owner = r.U64(); p.human = r.U8() != 0; p.name = r.Str(); p.style = r.U8();
                const int k = r.U8();
                for (int j = 0; j < k && r.ok; ++j) {
                    VsBotSlot s;
                    if (!ReadUnit(r, s.unit)) return false;
                    s.points = r.U16();
                    p.squad.push_back(s);
                }
                out.push_back(p);
            }
            return r.ok && !out.empty() && tier >= 0 && tier <= 2;
        }
        void WriteSquad(Writer& w, const VsBotPlayer& p) {
            const size_t n = std::min<size_t>(p.squad.size(), 40);
            w.U8(static_cast<std::uint8_t>(p.active + 1)); w.U8(static_cast<std::uint8_t>(n));
            for (size_t k = 0; k < n; ++k) { WriteUnit(w, p.squad[k].unit); w.U16(static_cast<std::uint16_t>(p.squad[k].points)); w.U8(static_cast<std::uint8_t>(p.squad[k].state)); }
        }
    }

    // ------------------------------------------------------------------------------------------------
    void GameLayer::InitNetMatch() {
        m_Party.onGameData = [this](Net::PeerId from, const std::uint8_t* data, std::size_t size) { NetOnData(from, data, size); };
    }

    bool GameLayer::NetSquadMatchAvailable() {
        if (m_Party.GetState() != Net::PartyState::InParty) return false;
        for (const auto& mb : m_Party.Members()) if (!mb.self && mb.linked) return true;
        return false;
    }

    bool GameLayer::NetIsAuthority() const { return !m_Net.active || m_Net.authority; }

    bool GameLayer::NetDeathBlocked(entt::entity e) const {
        if (!m_Net.active) return false;
        const auto* g = m_Scene.GetRegistry().try_get<NetGhostComponent>(e);
        return g && !g->killConfirmed;
    }

    // Il capo avvia la partita per tutti: mappa e modalita' le sue, ognuno con la sua squadra
    void GameLayer::NetLeaderStartMatch() {
        if (!m_Party.IsLeader()) return;
        if (!m_Party.Playing()) m_Party.StartSession();
        std::uint32_t id = 0;
        { std::random_device rd; while (id == 0) id = rd(); }
        m_Net.matchId = id;
        const int ev = std::clamp(m_SelectedEventIndex, 0, 3);
        Writer w; w.U8(NM_Start); w.U32(id); w.Str(m_SelectedMapId); w.U8(static_cast<std::uint8_t>(ev));
        m_Party.SendToAll(w.b.data(), w.b.size(), true);
        NetEnterMatch(m_SelectedMapId, ev);
    }

    // Il capo avvia il Vs Bot per tutti: i compagni (collegati) prendono il posto di altrettanti bot nella sua squadra
    void GameLayer::NetLeaderStartVsBot(int tierIdx) {
        if (!m_Party.IsLeader() || tierIdx < 0 || tierIdx > 2) return;
        m_VsBotMessage.clear();
        std::vector<std::pair<std::uint64_t, std::string>> humans{ { m_Party.SelfId(), m_Profile.netName } };
        for (const auto& mb : m_Party.Members()) if (!mb.self && mb.linked) humans.push_back({ mb.id, mb.name });
        auto players = VsBotMakeRoster(tierIdx, humans);
        if (players.empty()) { m_VsBotMessage = "Impossibile formare le squadre per questa coda."; return; }
        if (!m_Party.Playing()) m_Party.StartSession();
        std::uint32_t id = 0;
        { std::random_device rd; while (id == 0) id = rd(); }
        m_Net.matchId = id;
        Writer rw; WriteRoster(rw, tierIdx, players);
        m_Net.vsRoster = rw.b; m_Net.vsRosterMatch = id; m_Net.vsTier = tierIdx;
        Writer w; w.U8(NM_Start); w.U32(id); w.Str("vsbot_trincee"); w.U8(4);
        w.b.insert(w.b.end(), rw.b.begin(), rw.b.end());
        m_Party.SendToAll(w.b.data(), w.b.size(), true);
        NetEnterMatch("vsbot_trincee", 4);
    }

    // Partita gia' iniziata: mappa, modalita' e numero dall'ultima istantanea dell'host (o dall'avvio del capo)
    void GameLayer::NetJoinRunningMatch() {
        SnapInfo si;
        if (ParseSnapshot(m_Party.Snapshot(), si)) {
            if (si.eventIdx == 4) {   // Vs Bot: si entra solo se nel roster c'e' un posto mio, altrimenti lo si chiede all'host
                int tier = 0; std::vector<VsBotPlayer> pl;
                bool inRoster = false;
                if (m_Net.vsRosterMatch == si.matchId && ReadRoster(m_Net.vsRoster, tier, pl))
                    for (const auto& p : pl) inRoster = inRoster || (p.human && p.owner == m_Party.SelfId());
                if (!inRoster) { NetRequestVsBotJoin(si.matchId); return; }
            }
            m_Net.matchId = si.matchId; NetEnterMatch(si.map, si.eventIdx); return;
        }
        if (m_Net.startEvent >= 0 && m_Net.matchId != 0) { NetEnterMatch(m_Net.startMap, m_Net.startEvent); return; }
        m_PartyToasts.push_back({ "La partita della squadra non e' ancora pronta: riprova fra un attimo", 4.0f });
    }

    void GameLayer::NetEnterMatch(const std::string& mapId, int eventIdx) {
        if (m_GameState == GameState::Gameplay) {
            m_PartyToasts.push_back({ "La squadra e' in partita: esci da questa e premi ENTRA IN PARTITA", 6.0f });
            return;
        }
        if (m_GameState != GameState::MainMenu || m_PlayerRoster.empty()) return;
        // squadra: quella scelta nello schieramento, altrimenti l'unita' selezionata nel garage
        m_SelectedSquadIndices.erase(std::remove_if(m_SelectedSquadIndices.begin(), m_SelectedSquadIndices.end(),
            [&](int i) { return i < 0 || i >= static_cast<int>(m_PlayerRoster.size()); }), m_SelectedSquadIndices.end());
        if (m_SelectedSquadIndices.empty())
            m_SelectedSquadIndices.push_back(std::clamp(m_SelectedVaultIndex, 0, static_cast<int>(m_PlayerRoster.size()) - 1));
        m_SelectedMapId = mapId;
        m_SelectedEventIndex = std::clamp(eventIdx, 0, 4);
        m_ShowCustomizeModal = false;
        m_PartyPanelOpen = false;
        int slot = 0;
        { int k = 0; for (const auto& mb : m_Party.Members()) { if (mb.self) slot = k; ++k; } }

        // stato della partita da capo (i compagni restano: le loro copie compaiono col loro stato)
        const std::uint32_t matchId = m_Net.matchId;
        std::unordered_map<Net::PeerId, NetRemote> keep;
        for (auto& [id, r] : m_Net.remotes) { NetRemote k; k.id = r.id; k.name = r.name; keep[id] = k; }
        std::vector<std::uint8_t> roster = std::move(m_Net.vsRoster);
        const std::uint32_t rosterMatch = m_Net.vsRosterMatch;
        m_Net = NetMatchState();
        m_Net.remotes = std::move(keep);
        m_Net.vsRoster = std::move(roster);
        m_Net.vsRosterMatch = rosterMatch;
        m_Net.matchId = matchId;
        m_Net.active = true;
        m_Net.mapId = mapId;
        m_Net.eventIdx = m_SelectedEventIndex;
        NetShotQueue().clear();

        if (m_SelectedEventIndex == 4) {   // Vs Bot: roster del capo, squadra mia
            int tier = 0; std::vector<VsBotPlayer> players;
            bool inRoster = false;
            const bool ok = m_Net.vsRosterMatch == matchId && ReadRoster(m_Net.vsRoster, tier, players);
            if (ok) for (const auto& p : players) inRoster = inRoster || (p.human && p.owner == m_Party.SelfId());
            if (!ok || !inRoster) { m_Net.active = false; NetRequestVsBotJoin(matchId); return; }   // avviato senza di me: chiedo un posto
            if (!VsBotLaunch(tier, std::move(players))) {
                m_Net.active = false;
                m_PartyToasts.push_back({ "Non riesco a entrare nel Vs Bot della squadra", 5.0f });
                return;
            }
            m_Net.vsTier = tier;
        }
        else {
            StartDeployment(slot);
            if (m_GameState != GameState::Gameplay) { m_Net.active = false; return; }
            m_Scene.GetRegistry().emplace_or_replace<TeamComponent>(m_PlayerEntity, TeamComponent{ 0 });   // niente fuoco amico fra compagni
        }
        SUN_CORE_INFO("Partita in rete: mappa '{0}', modalita' {1}, posto {2}", mapId, m_SelectedEventIndex, slot);
    }

    void GameLayer::NetLeaveMatch() {
        if (!m_Net.active) return;
        if (m_Party.GetState() == Net::PartyState::InParty) {
            Writer w; w.U8(NM_Leave); w.U32(m_Net.matchId);
            m_Party.SendToAll(w.b.data(), w.b.size(), true);
        }
        auto& reg = m_Scene.GetRegistry();
        for (auto& [id, r] : m_Net.remotes) NetDestroyPuppet(r);
        for (auto& [id, en] : m_Net.enemies) {
            if (!reg.valid(en.e) || reg.all_of<PlayerTag>(en.e)) continue;
            m_PhysicsSystem.DestroyJoltBody(m_Scene, en.e);
            m_Scene.DestroyEntity(en.e);
        }
        m_Net.enemies.clear();
        // se muovevo io i nemici, il ruolo passa subito a uno di quelli rimasti in partita
        std::vector<Net::PeerId> cand;
        for (const auto& [id, r] : m_Net.remotes) if (r.hasState && r.sinceHeard < 3.0f) cand.push_back(id);
        m_Party.SetHostCandidates(cand);
        for (const auto& id : cand) m_Net.heard[id] = 0.0f;
        m_Net.active = false;
        m_Net.authority = false;
        m_Net.outDamage.clear();
        NetShotQueue().clear();
    }

    // ------------------------------------------------------------------------------------------------
    void GameLayer::UpdateNetMatch(float realDt) {
        const bool inParty = m_Party.GetState() == Net::PartyState::InParty;
        if (!m_Net.active) NetShotQueue().clear();

        // avvio ricevuto dal capo: dal garage si entra subito
        if (m_Net.startPending) {
            m_Net.startPending = false;
            if (inParty && !m_Net.active) {
                if (m_GameState == GameState::MainMenu) NetEnterMatch(m_Net.startMap, m_Net.startEvent);
                else m_PartyToasts.push_back({ "La squadra e' entrata in partita: esci e premi ENTRA IN PARTITA", 6.0f });
            }
        }
        for (auto it = m_Net.heard.begin(); it != m_Net.heard.end();) { it->second += realDt; if (it->second > 5.0f) it = m_Net.heard.erase(it); else ++it; }
        if (!m_Net.active) {
            // fuori dalla partita: se faccio da host, il ruolo passa a uno di quelli che giocano (se ce ne sono)
            m_Net.candTimer -= realDt;
            if (m_Net.candTimer <= 0.0f && inParty) {
                m_Net.candTimer = 0.5f;
                std::vector<Net::PeerId> cand;
                if (m_Party.Playing()) for (const auto& [id, age] : m_Net.heard) if (age < 3.0f) cand.push_back(id);
                m_Party.SetHostCandidates(cand);
            }
            return;
        }
        if (m_GameState != GameState::Gameplay) return;   // uscita in corso (la chiude il cambio di stato)

        // fuori dalla squadra (uscito, espulso, rete persa): si continua da soli, coi nemici veri
        if (!inParty || !m_Party.InSession()) {
            if (!m_Net.authority) NetPromote();
            for (auto& [id, r] : m_Net.remotes) NetDestroyPuppet(r);
            m_Net.remotes.clear();
            m_Net.active = false;
            m_Net.authority = false;
            m_PartyToasts.push_back({ "Collegamento con la squadra perso: continui da solo", 6.0f });
            return;
        }
        // il capo ha chiuso la partita: tutti al garage
        if (!m_Party.Playing()) {
            m_PartyToasts.push_back({ "Il capo squadra ha chiuso la partita", 5.0f });
            if (m_VsBotActive) { LeaveVsBotMatch(); return; }
            auto& reg = m_Scene.GetRegistry();
            std::vector<entt::entity> gone;
            for (auto e : reg.view<HealthComponent>(entt::exclude<PlayerTag>)) gone.push_back(e);
            for (auto e : gone) { if (!reg.valid(e)) continue; m_PhysicsSystem.DestroyJoltBody(m_Scene, e); m_Scene.DestroyEntity(e); }
            ClearPlayerDeathState();
            if (auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity)) hp->currentHealth = hp->maxHealth;
            m_IsPaused = false;
            m_GameState = GameState::MainMenu;
            if (GLFWwindow* window = glfwGetCurrentContext()) glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            return;
        }

        auto& reg = m_Scene.GetRegistry();
        if (reg.valid(m_PlayerEntity) && !reg.all_of<TeamComponent>(m_PlayerEntity)) reg.emplace<TeamComponent>(m_PlayerEntity, TeamComponent{ m_VsBotActive ? m_VsBotMyTeam : 0 });

        // ruolo: host = simulo i nemici
        const bool host = m_Party.IsHost();
        if (host && !m_Net.authority) NetPromote();
        else if (!host && m_Net.authority) NetDemote();

        // compagni: chi non si sente piu' (uscito, caduto) sparisce
        for (auto it = m_Net.remotes.begin(); it != m_Net.remotes.end();) {
            NetRemote& r = it->second;
            r.sinceHeard += realDt;
            bool member = false;
            for (const auto& mb : m_Party.Members()) if (mb.id == r.id) member = true;
            if (!member || r.sinceHeard > 6.0f) { NetDestroyPuppet(r); it = m_Net.remotes.erase(it); }
            else ++it;
        }
        // chi e' davvero in partita: fra loro si sceglie l'host
        m_Net.candTimer -= realDt;
        if (m_Net.candTimer <= 0.0f) {
            m_Net.candTimer = 0.5f;
            std::vector<Net::PeerId> cand{ m_Party.SelfId() };
            for (const auto& [id, r] : m_Net.remotes) if (r.hasState && r.sinceHeard < 3.0f) cand.push_back(id);
            m_Party.SetHostCandidates(cand);
        }

        NetSendState(realDt);
        if (m_Net.authority) NetHostTick(realDt); else { NetGhostTick(realDt); PlayerHitLog().clear(); }
        NetUpdatePuppets(realDt);
        if (m_VsBotActive) NetVsBotTick(realDt);

        // i miei colpi e quelli dei bot che simulo: agli altri solo l'effetto (scia, lampo, suono)
        auto& shots = NetShotQueue();
        if (!shots.empty()) {
            auto writeTrail = [](Writer& w, const NetShotEvent& s) {
                const auto& t = s.trail;
                WV(w, t.start); WV(w, t.end);
                w.U8(static_cast<std::uint8_t>(std::clamp(t.color.r, 0.0f, 1.0f) * 255.0f)); w.U8(static_cast<std::uint8_t>(std::clamp(t.color.g, 0.0f, 1.0f) * 255.0f));
                w.U8(static_cast<std::uint8_t>(std::clamp(t.color.b, 0.0f, 1.0f) * 255.0f)); w.U8(static_cast<std::uint8_t>(std::clamp(t.color.a, 0.0f, 1.0f) * 255.0f));
                WF(w, t.travelTime); WF(w, t.totalLife); WF(w, t.projectileWidth); WF(w, t.impactScale);
                w.U8(static_cast<std::uint8_t>(t.style)); w.U8(static_cast<std::uint8_t>(t.fx + 1));
                w.U8(static_cast<std::uint8_t>((t.isBeam ? 1 : 0) | (t.hitSomething ? 2 : 0) | (t.hitUnit ? 4 : 0) | (s.first ? 8 : 0)));
            };
            std::vector<const NetShotEvent*> mine;
            std::vector<std::pair<std::uint32_t, const NetShotEvent*>> bots;
            for (const auto& sh : shots) {
                const entt::entity e = static_cast<entt::entity>(sh.shooter);
                if (e == m_PlayerEntity) { mine.push_back(&sh); continue; }
                if (!m_Net.authority || !reg.valid(e)) continue;
                const auto* ne = reg.try_get<NetEntityComponent>(e);
                if (ne && ne->netId != 0) bots.push_back({ ne->netId, &sh });
            }
            for (size_t i = 0; i < mine.size();) {
                Writer w; w.U8(NM_Shots); w.U32(m_Net.matchId);
                const size_t n = std::min<size_t>(mine.size() - i, 24);
                w.U8(static_cast<std::uint8_t>(n));
                for (size_t k = 0; k < n; ++k) writeTrail(w, *mine[i + k]);
                m_Party.SendToAll(w.b.data(), w.b.size(), false);
                i += n;
            }
            for (size_t i = 0; i < bots.size();) {
                Writer w; w.U8(NM_BotShots); w.U32(m_Net.matchId);
                const size_t n = std::min<size_t>(bots.size() - i, 24);
                w.U8(static_cast<std::uint8_t>(n));
                for (size_t k = 0; k < n; ++k) { w.U32(bots[i + k].first); writeTrail(w, *bots[i + k].second); }
                m_Party.SendToAll(w.b.data(), w.b.size(), false);
                i += n;
            }
            shots.clear();
        }
    }

    // La mia unita': posizione, mira, salute, gesti (20 volte al secondo); com'e' fatta quando cambia
    void GameLayer::NetSendState(float realDt) {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(m_PlayerEntity)) return;
        const auto* t = reg.try_get<TransformComponent>(m_PlayerEntity);
        const auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity);
        if (!t || !hp) return;

        // com'e' fatta la mia unita' in campo
        TrainedUnit unit;
        bool hasUnit = false;
        if (m_CurrentSquadMemberIdx >= 0 && m_CurrentSquadMemberIdx < static_cast<int>(m_ActiveSquad.size())) {
            const int ri = m_ActiveSquad[m_CurrentSquadMemberIdx].rosterIndex;
            if (ri >= 0 && ri < static_cast<int>(m_PlayerRoster.size())) { unit = m_PlayerRoster[ri]; hasUnit = true; }
        }
        if (hasUnit) {
            const std::string key = UnitKey(unit);
            m_Net.unitTimer -= realDt;
            if (key != m_Net.unitKey) { m_Net.unitKey = key; ++m_Net.unitVer; m_Net.unitTimer = 0.0f; }
            if (m_Net.unitTimer <= 0.0f) {
                m_Net.unitTimer = kUnitResend;
                Writer w; w.U8(NM_Unit); w.U32(m_Net.matchId); w.U32(m_Net.unitVer); w.Str(m_Profile.netName); WriteUnit(w, unit);
                m_Party.SendToAll(w.b.data(), w.b.size(), true);
            }
        }

        // gesti di mischia: un numero che cresce a ogni attacco (o parata) iniziato
        if (const auto* melee = reg.try_get<MeleeStateComponent>(m_PlayerEntity)) {
            const int ph = static_cast<int>(melee->currentPhase);
            if (melee->currentPhase == MeleePhase::Windup && (m_Net.lastMeleePhase != ph || melee->currentAttackNode != m_Net.lastMeleeNode)) ++m_Net.meleeSeq;
            m_Net.lastMeleePhase = ph;
            m_Net.lastMeleeNode = melee->currentAttackNode;
        }

        m_Net.stateTimer -= realDt;
        if (m_Net.stateTimer > 0.0f) return;
        m_Net.stateTimer = kStateRate;
        const bool alive = hp->currentHealth > 0.0f && !reg.all_of<CorpseComponent>(m_PlayerEntity);
        std::uint8_t flags = alive ? 1 : 0;
        if (m_PlayerAimingNow) flags |= 2;
        if (const auto* cr = reg.try_get<CrouchComponent>(m_PlayerEntity); cr && cr->amount > 0.5f) flags |= 4;
        if (const auto* st = reg.try_get<UnitStatsComponent>(m_PlayerEntity); st && st->sprinting) flags |= 8;
        int slot = 0;
        if (const auto* inv = reg.try_get<InventoryComponent>(m_PlayerEntity)) slot = inv->currentSlot;
        const auto* melee = reg.try_get<MeleeStateComponent>(m_PlayerEntity);

        Writer w; w.U8(NM_State); w.U32(m_Net.matchId); w.U16(++m_Net.seq);
        WV(w, t->Position); WF(w, t->Rotation.y); WF(w, t->AimYaw); WF(w, t->AimPitch);
        WF(w, hp->currentHealth); WF(w, hp->maxHealth);
        w.U8(flags); w.U8(static_cast<std::uint8_t>(std::clamp(slot, 0, 6)));
        w.U8(m_Net.meleeSeq);
        w.U8(melee ? static_cast<std::uint8_t>(melee->currentAttack) : 0);
        w.Str(melee ? melee->currentAttackNode : std::string());
        w.U32(hasUnit ? m_Net.unitVer : 0);
        int team = 0;
        if (const auto* tc = reg.try_get<TeamComponent>(m_PlayerEntity)) team = tc->team == 1 ? 1 : 0;
        w.U8(static_cast<std::uint8_t>(team));
        m_Party.SendToAll(w.b.data(), w.b.size(), false);
    }

    // Host: numeri ai nemici nuovi, chi li colpisce, dove sono (a tutti), colpi dell'orda sui compagni, istantanea
    void GameLayer::NetHostTick(float realDt) {
        auto& reg = m_Scene.GetRegistry();
        const Net::PeerId self = m_Party.SelfId();
        // nemici nuovi (ondate): un numero uguale per tutti; il prefisso dell'epoca evita doppioni dopo un cambio di host
        {
            std::vector<entt::entity> fresh;
            for (auto e : reg.view<HordeAgentComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent, NetEntityComponent, NetGhostComponent>)) fresh.push_back(e);
            for (auto e : fresh) {
                const std::uint32_t id = ((m_Party.Epoch() & 0xFFFu) << 20) | (m_Net.nextNetId++ & 0xFFFFFu);
                reg.emplace<NetEntityComponent>(e, NetEntityComponent{ id });
                NetEnemyInfo info; info.e = e; info.prevHp = reg.get<HealthComponent>(e).currentHealth;
                m_Net.enemies[id] = info;
            }
            // Vs Bot: i bot del roster (riga e unita', per ricrearli uguali). Gli altri (rinforzi del vox) restano solo qui.
            if (m_VsBotActive) {
                fresh.clear();
                for (auto e : reg.view<BotBrainComponent, TransformComponent, HealthComponent>(entt::exclude<CorpseComponent, NetEntityComponent, NetGhostComponent, NetRemotePlayerComponent>)) fresh.push_back(e);
                for (auto e : fresh) {
                    int pi = -1, si = -1;
                    for (int i = 0; i < static_cast<int>(m_VsBotPlayers.size()) && pi < 0; ++i)
                        for (int k = 0; k < static_cast<int>(m_VsBotPlayers[i].squad.size()); ++k) if (m_VsBotPlayers[i].squad[k].entity == e) { pi = i; si = k; break; }
                    if (pi < 0) { reg.emplace<NetEntityComponent>(e, NetEntityComponent{ 0 }); continue; }   // non e' del roster
                    const std::uint32_t id = ((m_Party.Epoch() & 0xFFFu) << 20) | (m_Net.nextNetId++ & 0xFFFFFu);
                    reg.emplace<NetEntityComponent>(e, NetEntityComponent{ id });
                    NetEnemyInfo info; info.e = e; info.prevHp = reg.get<HealthComponent>(e).currentHealth;
                    info.kind = 1; info.player = static_cast<std::uint8_t>(pi); info.slot = static_cast<std::uint8_t>(si);
                    m_Net.enemies[id] = info;
                }
            }
        }
        // chi ha colpito i nemici dall'ultimo fotogramma: io (i miei colpi sono nel registro) o un bot (nessun merito);
        // i colpi dei compagni arrivano coi loro messaggi. Poi gesti di mischia e mira dei bot.
        const auto& myHits = PlayerHitLog();
        for (auto it = m_Net.enemies.begin(); it != m_Net.enemies.end();) {
            NetEnemyInfo& en = it->second;
            if (!reg.valid(en.e) || reg.all_of<CorpseComponent>(en.e)) { it = m_Net.enemies.erase(it); continue; }
            const float hp = reg.get<HealthComponent>(en.e).currentHealth;
            if (hp < en.prevHp - 0.01f) en.lastHitBy = std::find(myHits.begin(), myHits.end(), en.e) != myHits.end() ? self : 0;
            en.prevHp = hp;
            if (en.kind == 1) {
                if (const auto* brain = reg.try_get<BotBrainComponent>(en.e)) en.aiming = brain->isAiming;
                if (const auto* inv = reg.try_get<InventoryComponent>(en.e)) en.weaponSlot = inv->currentSlot;
            }
            if (const auto* melee = reg.try_get<MeleeStateComponent>(en.e)) {
                const int ph = static_cast<int>(melee->currentPhase);
                if (melee->currentPhase == MeleePhase::Windup && (en.lastPhase != ph || melee->currentAttackNode != en.lastNode)) ++en.meleeSeq;
                en.lastPhase = ph; en.lastNode = melee->currentAttackNode;
            }
            ++it;
        }
        // l'orda ha colpito un compagno (la sua copia qui): il danno va a lui
        for (auto& [id, r] : m_Net.remotes) NetFlushPuppetDamage(r);
        // dove sono i nemici
        m_Net.enemyTimer -= realDt;
        if (m_Net.enemyTimer <= 0.0f) {
            m_Net.enemyTimer = kEnemyRate;
            std::vector<std::pair<std::uint32_t, const NetEnemyInfo*>> list;
            for (const auto& [id, en] : m_Net.enemies) list.push_back({ id, &en });
            size_t i = 0;
            do {
                const size_t n = std::min<size_t>(list.size() - i, kEnemiesPerPacket);
                Writer w; w.U8(NM_Enemies); w.U32(m_Net.matchId); w.U8(static_cast<std::uint8_t>(n));
                for (size_t k = 0; k < n; ++k) {
                    const NetEnemyInfo& en = *list[i + k].second;
                    const auto& t = reg.get<TransformComponent>(en.e);
                    const auto& h = reg.get<HealthComponent>(en.e);
                    const auto* melee = reg.try_get<MeleeStateComponent>(en.e);
                    w.U32(list[i + k].first);
                    WQ(w, t.Position.x, 50.0f); WQ(w, t.Position.y, 50.0f); WQ(w, t.Position.z, 50.0f);
                    WQ(w, WrapDeg(t.Rotation.y), 100.0f);
                    w.U16(static_cast<std::uint16_t>(std::clamp(h.currentHealth, 0.0f, 6000.0f) * 10.0f));
                    w.U8(en.meleeSeq);
                    w.U8(melee ? static_cast<std::uint8_t>(melee->currentPhase) : 0);
                    w.U8(melee ? static_cast<std::uint8_t>(melee->currentAttack) : 0);
                    w.Str(melee && melee->currentPhase != MeleePhase::None ? melee->currentAttackNode : std::string());
                    w.U8(en.kind);
                    if (en.kind == 1) {
                        int team = 0;
                        if (const auto* tc = reg.try_get<TeamComponent>(en.e)) team = tc->team == 1 ? 1 : 0;
                        w.U8(en.player); w.U8(en.slot); w.U8(static_cast<std::uint8_t>((en.aiming ? 1 : 0) | (team ? 2 : 0))); w.U8(static_cast<std::uint8_t>(std::clamp(en.weaponSlot, 0, 6)));
                    }
                }
                m_Party.SendToAll(w.b.data(), w.b.size(), false);
                i += n;
            } while (i < list.size());
        }
        // istantanea: chi subentra riparte da qui (ondata e tempi)
        m_Net.snapTimer -= realDt;
        if (m_Net.snapTimer <= 0.0f && m_Net.matchId != 0) {
            m_Net.snapTimer = kSnapRate;
            bool waves = false; float timer = 0.0f; int count = 1;
            GetWaveState(waves, timer, count);
            Writer w; w.U8(kSnapMagic); w.U32(m_Net.matchId); w.Str(m_Net.mapId); w.U8(static_cast<std::uint8_t>(m_Net.eventIdx));
            w.U8(waves ? 1 : 0); WF(w, timer); w.U16(static_cast<std::uint16_t>(count));
            m_Party.PublishSnapshot(w.b);
        }
        PlayerHitLog().clear();
    }

    // Copie dei nemici: i miei danni all'host, movimento morbido verso l'ultima posizione ricevuta
    void GameLayer::NetGhostTick(float realDt) {
        auto& reg = m_Scene.GetRegistry();
        const float k = 1.0f - std::exp(-12.0f * realDt);
        for (auto it = m_Net.enemies.begin(); it != m_Net.enemies.end();) {
            NetEnemyInfo& en = it->second;
            if (!reg.valid(en.e) || reg.all_of<CorpseComponent>(en.e)) { it = m_Net.enemies.erase(it); continue; }
            en.sinceSeen += realDt;
            if (en.sinceSeen > 5.0f) {   // l'host non lo manda piu': sparito (5 s: il tempo di un cambio di host)
                m_PhysicsSystem.DestroyJoltBody(m_Scene, en.e);
                m_Scene.DestroyEntity(en.e);
                it = m_Net.enemies.erase(it);
                continue;
            }
            const float hp = reg.get<HealthComponent>(en.e).currentHealth;
            if (hp < en.baseHp - 0.01f) { m_Net.outDamage.push_back({ it->first, en.baseHp - hp }); en.baseHp = hp; }
            else if (hp > en.baseHp) en.baseHp = hp;   // rigenerazione qui: non e' un danno
            auto& t = reg.get<TransformComponent>(en.e);
            if (glm::distance(t.Position, en.pos) > 5.0f) t.Position = en.pos;
            else t.Position += (en.pos - t.Position) * k;
            t.Rotation.y = LerpDeg(t.Rotation.y, en.yaw, k);
            t.AimYaw = t.Rotation.y;
            ++it;
        }
        for (auto& [id, r] : m_Net.remotes) NetFlushPuppetDamage(r);   // PvP: i miei colpi sulle copie degli avversari
        if (!m_Net.outDamage.empty()) {
            Writer w; w.U8(NM_Damage); w.U32(m_Net.matchId);
            const size_t n = std::min<size_t>(m_Net.outDamage.size(), 200);
            w.U8(static_cast<std::uint8_t>(n));
            for (size_t i = 0; i < n; ++i) { w.U32(m_Net.outDamage[i].first); WF(w, m_Net.outDamage[i].second); }
            m_Party.SendToHost(w.b.data(), w.b.size(), true);
            m_Net.outDamage.clear();
        }
    }

    // Copie dei compagni: create con la loro unita', mosse col loro stato, a terra quando cadono
    void GameLayer::NetUpdatePuppets(float realDt) {
        auto& reg = m_Scene.GetRegistry();
        const float k = 1.0f - std::exp(-14.0f * realDt);
        for (auto& [id, r] : m_Net.remotes) {
            if (reg.valid(r.puppet) && reg.all_of<CorpseComponent>(r.puppet)) r.puppet = entt::null;   // il cadavere resta per conto suo
            if (!r.hasState) continue;
            if (!r.alive) {
                if (reg.valid(r.puppet)) {   // caduto: la sua copia cade qui (ragdoll)
                    if (auto* g = reg.try_get<NetGhostComponent>(r.puppet)) g->killConfirmed = true;
                    reg.get<HealthComponent>(r.puppet).currentHealth = 0.0f;
                }
                continue;
            }
            if (reg.valid(r.puppet) && r.puppetVer != r.unitVer) NetDestroyPuppet(r);   // ha cambiato unita' (rientro)
            if (!reg.valid(r.puppet)) {
                if (!r.hasUnit || r.stateUnitVer != r.unitVer) continue;   // aspetto com'e' fatta
                NetSpawnPuppet(r);
                if (!reg.valid(r.puppet)) continue;
            }
            auto& t = reg.get<TransformComponent>(r.puppet);
            if (glm::distance(t.Position, r.pos) > 6.0f) t.Position = r.pos;
            else t.Position += (r.pos - t.Position) * k;
            t.Rotation.y = LerpDeg(t.Rotation.y, r.yaw, k);
            t.AimYaw = LerpDeg(t.AimYaw, r.aimYaw, k);
            t.AimPitch = r.aimPitch;
            if (auto* brain = reg.try_get<BotBrainComponent>(r.puppet)) { brain->isAiming = r.aiming; brain->displayName = r.name; }
            if (auto* st = reg.try_get<UnitStatsComponent>(r.puppet)) st->sprinting = r.sprint;
            if (auto* tc = reg.try_get<TeamComponent>(r.puppet); tc && tc->team != r.team) tc->team = r.team;   // ha cambiato squadra (Vs Bot)
            reg.get_or_emplace<CrouchComponent>(r.puppet).amount = r.crouch ? 1.0f : 0.0f;
            if (auto* inv = reg.try_get<InventoryComponent>(r.puppet); inv && inv->currentSlot != r.slot) {
                inv->currentSlot = r.slot;
                reg.get_or_emplace<WeaponBlueprintId>(r.puppet).id = inv->GetActiveWeaponId();
            }
            if (auto* ws = reg.try_get<WeaponStateComponent>(r.puppet); ws && ws->currentAmmo < 50) ws->currentAmmo = 999;
        }
    }

    // Host: quanto la copia di un compagno ha perso dall'ultima volta (colpi dell'orda) va a lui
    void GameLayer::NetFlushPuppetDamage(NetRemote& r) {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(r.puppet) || reg.all_of<CorpseComponent>(r.puppet) || !r.alive) return;
        const float hp = reg.get<HealthComponent>(r.puppet).currentHealth;
        if (r.hostSetHealth >= 0.0f && hp < r.hostSetHealth - 0.05f) {
            Writer w; w.U8(NM_HitPlayer); w.U32(m_Net.matchId); w.U64(r.id); WF(w, r.hostSetHealth - hp);
            m_Party.SendToAll(w.b.data(), w.b.size(), true);
        }
        r.hostSetHealth = hp;
    }

    entt::entity GameLayer::NetSpawnPuppet(NetRemote& r) {
        auto& reg = m_Scene.GetRegistry();
        const entt::entity e = SpawnBot(r.unit, r.team, r.pos, r.yaw, -1);
        if (e == entt::null) return e;
        m_VsBotUnits.erase(e);   // non e' un bot del Vs Bot
        reg.emplace_or_replace<NetGhostComponent>(e);
        reg.emplace_or_replace<NetRemotePlayerComponent>(e, NetRemotePlayerComponent{ r.id });
        auto& t = reg.get<TransformComponent>(e);
        t.Position = r.pos;   // stessa origine del giocatore che rappresenta
        t.AimYaw = r.aimYaw; t.AimPitch = r.aimPitch;
        auto& hp = reg.get<HealthComponent>(e);
        hp.currentHealth = std::min(r.health, hp.maxHealth > 0.0f ? hp.maxHealth : r.health);
        if (auto* brain = reg.try_get<BotBrainComponent>(e)) brain->displayName = r.name;
        if (auto* ws = reg.try_get<WeaponStateComponent>(e)) ws->currentAmmo = 999;
        r.puppet = e;
        r.puppetVer = r.unitVer;
        r.hostSetHealth = hp.currentHealth;
        return e;
    }

    void GameLayer::NetDestroyPuppet(NetRemote& r) {
        auto& reg = m_Scene.GetRegistry();
        if (reg.valid(r.puppet) && !reg.all_of<CorpseComponent>(r.puppet)) {
            m_PhysicsSystem.DestroyJoltBody(m_Scene, r.puppet);
            m_Scene.DestroyEntity(r.puppet);
        }
        r.puppet = entt::null;
    }

    // Divento host: le copie dei nemici diventano nemici veri, le ondate ripartono dall'ultima istantanea
    void GameLayer::NetPromote() {
        auto& reg = m_Scene.GetRegistry();
        m_Net.authority = true;
        const Net::PeerId self = m_Party.SelfId();
        for (auto& [id, en] : m_Net.enemies) {
            if (!reg.valid(en.e)) continue;
            if (reg.all_of<NetGhostComponent>(en.e)) reg.remove<NetGhostComponent>(en.e);
            if (auto* horde = reg.try_get<HordeAgentComponent>(en.e)) {
                horde->state = HordeState::Waiting; horde->assignedSlotIndex = -1; horde->currentPath.clear();
                horde->target = entt::null; horde->hasEverSeenPlayer = false;
            }
            en.prevHp = reg.get<HealthComponent>(en.e).currentHealth;
            en.lastHitBy = 0;
        }
        (void)self;
        m_Net.nextNetId = 1;
        SnapInfo si;
        if (ParseSnapshot(m_Party.Snapshot(), si) && si.matchId == m_Net.matchId && m_Party.SnapshotAge() >= 0.0f && m_Party.SnapshotAge() < 30.0f)
            SetWaveState(si.waves, si.waveTimer, si.waveCount);
        m_Net.enemyTimer = 0.0f; m_Net.snapTimer = 0.0f;
        SUN_CORE_INFO("Partita in rete: ora muovo io i nemici ({0})", m_Net.enemies.size());
    }

    // Non sono piu' host: i nemici li muove un altro, qui restano le copie
    void GameLayer::NetDemote() {
        auto& reg = m_Scene.GetRegistry();
        m_Net.authority = false;
        for (auto& [id, en] : m_Net.enemies) {
            if (!reg.valid(en.e) || reg.all_of<CorpseComponent>(en.e)) continue;
            reg.emplace_or_replace<NetGhostComponent>(en.e);
            const auto& t = reg.get<TransformComponent>(en.e);
            en.pos = t.Position; en.yaw = t.Rotation.y; en.sinceSeen = 0.0f;
            en.baseHp = reg.get<HealthComponent>(en.e).currentHealth;
        }
        SUN_CORE_INFO("Partita in rete: i nemici li muove un compagno");
    }

    // Morte di un'unita' (CheckUnitDeaths). true = l'uccisione conta per me su questo computer.
    bool GameLayer::NetOnUnitKilled(entt::entity e) {
        if (!m_Net.active) return true;
        auto& reg = m_Scene.GetRegistry();
        if (reg.all_of<NetRemotePlayerComponent>(e)) {
            for (auto& [id, r] : m_Net.remotes) if (r.puppet == e) r.puppet = entt::null;
            return false;
        }
        const auto* ne = reg.try_get<NetEntityComponent>(e);
        if (!ne) return true;
        const std::uint32_t netId = ne->netId;
        auto it = m_Net.enemies.find(netId);
        bool mine = false;
        if (m_Net.authority && !reg.all_of<NetGhostComponent>(e)) {
            Net::PeerId killer = m_Party.SelfId();
            if (it != m_Net.enemies.end()) {
                const float hp = reg.get<HealthComponent>(e).currentHealth;
                const bool hitNow = hp < it->second.prevHp - 0.01f;   // colpito qui in questo fotogramma: da me o da un bot
                const auto& myHits = PlayerHitLog();
                if (hitNow) killer = std::find(myHits.begin(), myHits.end(), e) != myHits.end() ? m_Party.SelfId() : 0;
                else killer = it->second.lastHitBy;
            }
            Writer w; w.U8(NM_Killed); w.U32(m_Net.matchId); w.U32(netId); w.U64(killer);
            m_Party.SendToAll(w.b.data(), w.b.size(), true);
            mine = killer == m_Party.SelfId();
        }
        if (it != m_Net.enemies.end()) m_Net.enemies.erase(it);
        return mine;
    }

    // ------------------------------------------------------------------------------------------------
    void GameLayer::NetOnData(Net::PeerId from, const std::uint8_t* data, std::size_t size) {
        if (size < 1 || from == m_Party.SelfId()) return;
        Reader r(data + 1, size - 1);
        const std::uint8_t type = data[0];
        auto& reg = m_Scene.GetRegistry();

        if (type == NM_Start) {
            const std::uint32_t id = r.U32(); const std::string map = r.Str(); const int ev = r.U8();
            if (!r.ok || from != m_Party.LeaderId() || id == 0) return;
            if (ev == 4) { m_Net.vsRoster.assign(r.p, r.p + r.n); m_Net.vsRosterMatch = id; }   // Vs Bot: il roster deciso dal capo
            m_Net.matchId = id; m_Net.startMap = map; m_Net.startEvent = ev; m_Net.startPending = true;
            return;
        }
        if ((type == NM_VsLineup || type == NM_VsJoinNo) && !m_Net.active) {
            // risposta dell'host alla mia richiesta di entrare nel Vs Bot gia' avviato
            const std::uint32_t id = r.U32(); const std::uint64_t target = r.U64();
            if (!r.ok || id == 0 || id != m_Net.joinRequestMatch || target != m_Party.SelfId()) return;
            m_Net.joinRequestMatch = 0;
            if (type == NM_VsJoinNo) {
                m_PartyToasts.push_back({ "Il Vs Bot sta gia' iniziando e i posti sono presi: aspetta la prossima partita", 5.0f });
                return;
            }
            m_Net.vsRoster.assign(r.p, r.p + r.n);
            m_Net.vsRosterMatch = id;
            m_Net.matchId = id;
            if (m_GameState == GameState::MainMenu) NetEnterMatch("vsbot_trincee", 4);
            else { m_Net.startMap = "vsbot_trincee"; m_Net.startEvent = 4; m_PartyToasts.push_back({ "Hai un posto nel Vs Bot: esci e premi ENTRA IN PARTITA", 6.0f }); }
            return;
        }
        const std::uint32_t matchId = r.U32();
        if (type == NM_State && r.ok) m_Net.heard[from] = 0.0f;   // e' in partita (anche se io no)
        if (!r.ok || !m_Net.active || matchId != m_Net.matchId || m_GameState != GameState::Gameplay) return;

        switch (type) {
        case NM_Leave: {
            auto it = m_Net.remotes.find(from);
            if (it != m_Net.remotes.end()) { NetDestroyPuppet(it->second); m_Net.remotes.erase(it); }
            break;
        }
        case NM_Unit: {
            const std::uint32_t ver = r.U32(); const std::string name = r.Str();
            TrainedUnit u;
            if (!ReadUnit(r, u) || !BlueprintManager::Get().GetUnit(u.blueprintId)) break;   // unita' che questa versione non conosce
            NetRemote& rm = m_Net.remotes[from];
            rm.id = from; rm.name = name.empty() ? "Compagno" : name;
            if (!rm.hasUnit || ver != rm.unitVer) { rm.unit = u; rm.unitVer = ver; rm.hasUnit = true; }
            break;
        }
        case NM_State: {
            const std::uint16_t seq = r.U16();
            const glm::vec3 pos = RV(r); const float yaw = RF(r), aimYaw = RF(r), aimPitch = RF(r);
            const float health = RF(r), maxHealth = RF(r);
            const std::uint8_t flags = r.U8(), slot = r.U8(), mseq = r.U8(), mattack = r.U8();
            const std::string node = r.Str();
            const std::uint32_t uver = r.U32();
            const int team = r.U8() == 1 ? 1 : 0;
            if (!r.ok) break;
            NetRemote& rm = m_Net.remotes[from];
            rm.team = team;
            rm.id = from;
            {   // vecchio (arrivato in ritardo)? Un salto molto indietro invece e' un rientro in partita: si riparte
                const int diff = static_cast<std::int16_t>(static_cast<std::uint16_t>(seq - rm.seq));
                if (rm.hasState && diff <= 0 && diff > -300) break;
            }
            rm.seq = seq; rm.hasState = true; rm.sinceHeard = 0.0f;
            rm.pos = pos; rm.yaw = yaw; rm.aimYaw = aimYaw; rm.aimPitch = aimPitch;
            rm.health = health; rm.alive = (flags & 1) != 0; rm.aiming = (flags & 2) != 0; rm.crouch = (flags & 4) != 0; rm.sprint = (flags & 8) != 0;
            rm.slot = slot; rm.stateUnitVer = uver;
            (void)maxHealth;
            if (rm.name.empty()) for (const auto& mb : m_Party.Members()) if (mb.id == from) rm.name = mb.name;
            if (reg.valid(rm.puppet) && !reg.all_of<CorpseComponent>(rm.puppet)) {
                NetFlushPuppetDamage(rm);   // colpi (dell'orda, dei bot o miei in PvP) non ancora mandati
                if (rm.alive) { reg.get<HealthComponent>(rm.puppet).currentHealth = health; rm.hostSetHealth = health; }
                // gesto di mischia iniziato: lo rifa' la copia (solo il gesto)
                if (mseq != rm.meleeSeq) {
                    rm.meleeSeq = mseq;
                    if (auto* melee = reg.try_get<MeleeStateComponent>(rm.puppet); melee && !node.empty()) {
                        melee->currentPhase = MeleePhase::Windup;
                        melee->currentAttack = static_cast<MeleeAttackType>(std::min<int>(mattack, static_cast<int>(MeleeAttackType::Guard)));
                        melee->currentAttackNode = node;
                        melee->stateTimer = 0.0f;
                        melee->hasHitInThisAttack = true;
                    }
                }
            }
            else rm.meleeSeq = mseq;
            break;
        }
        case NM_Enemies: {
            if (m_Net.authority) break;   // due host per un attimo (cambio in corso): vale il mio
            const int n = r.U8();
            for (int i = 0; i < n; ++i) {
                const std::uint32_t id = r.U32();
                glm::vec3 pos; pos.x = RQ(r, 50.0f); pos.y = RQ(r, 50.0f); pos.z = RQ(r, 50.0f);
                const float yaw = RQ(r, 100.0f);
                const float hp = static_cast<float>(r.U16()) / 10.0f;
                const std::uint8_t mseq = r.U8(), mphase = r.U8(), mattack = r.U8();
                const std::string node = r.Str();
                const std::uint8_t kind = r.U8();
                std::uint8_t pi = 0, si = 0, bflags = 0, wslot = 0;
                if (kind == 1) { pi = r.U8(); si = r.U8(); bflags = r.U8(); wslot = r.U8(); }
                if (!r.ok) break;
                auto it = m_Net.enemies.find(id);
                if (it == m_Net.enemies.end()) {
                    if (hp <= 0.0f) continue;
                    entt::entity e = entt::null;
                    if (kind == 1) {   // bot del Vs Bot: la stessa unita' del roster
                        if (!m_VsBotActive || pi >= m_VsBotPlayers.size() || si >= m_VsBotPlayers[pi].squad.size()) continue;
                        auto& bp = m_VsBotPlayers[pi];
                        e = SpawnBot(bp.squad[si].unit, (bflags & 2) ? 1 : 0, pos, yaw, -1);
                        if (e == entt::null) continue;
                        if (auto* brain = reg.try_get<BotBrainComponent>(e)) brain->displayName = bp.name + " (" + bp.squad[si].unit.customName + ")";
                        bp.squad[si].entity = e;
                    }
                    else e = SpawnNetOrk(pos);
                    if (e == entt::null) continue;
                    reg.emplace_or_replace<NetEntityComponent>(e, NetEntityComponent{ id });
                    reg.emplace_or_replace<NetGhostComponent>(e);
                    auto& t = reg.get<TransformComponent>(e);
                    t.Position = pos; t.Rotation.y = yaw; t.AimYaw = yaw;
                    reg.get<HealthComponent>(e).currentHealth = hp;
                    NetEnemyInfo info; info.e = e; info.baseHp = hp; info.pos = pos; info.yaw = yaw; info.meleeSeq = mseq;
                    info.kind = kind; info.player = pi; info.slot = si;
                    it = m_Net.enemies.emplace(id, info).first;
                }
                NetEnemyInfo& en = it->second;
                if (!reg.valid(en.e) || reg.all_of<CorpseComponent>(en.e)) continue;
                if (kind == 1) {   // mira, squadra e arma in mano del bot
                    if (auto* brain = reg.try_get<BotBrainComponent>(en.e)) brain->isAiming = (bflags & 1) != 0;
                    if (auto* tc = reg.try_get<TeamComponent>(en.e)) tc->team = (bflags & 2) ? 1 : 0;
                    if (auto* inv = reg.try_get<InventoryComponent>(en.e); inv && inv->currentSlot != wslot) {
                        inv->currentSlot = wslot;
                        reg.get_or_emplace<WeaponBlueprintId>(en.e).id = inv->GetActiveWeaponId();
                    }
                    if (auto* ws = reg.try_get<WeaponStateComponent>(en.e); ws && ws->currentAmmo < 50) ws->currentAmmo = 999;
                }
                en.pos = pos; en.yaw = yaw; en.sinceSeen = 0.0f;
                auto& h = reg.get<HealthComponent>(en.e);
                // prima i miei colpi non ancora mandati (altrimenti la salute dell'host li cancellerebbe)
                if (h.currentHealth < en.baseHp - 0.01f) { m_Net.outDamage.push_back({ id, en.baseHp - h.currentHealth }); en.baseHp = h.currentHealth; }
                if (hp < h.currentHealth) { h.currentHealth = hp; en.baseHp = hp; }   // colpi degli altri
                if (auto* melee = reg.try_get<MeleeStateComponent>(en.e)) {
                    if (mseq != en.meleeSeq) {
                        en.meleeSeq = mseq;
                        if (!node.empty() && mphase != static_cast<std::uint8_t>(MeleePhase::None)) {
                            melee->currentPhase = MeleePhase::Windup;
                            melee->currentAttack = static_cast<MeleeAttackType>(std::min<int>(mattack, static_cast<int>(MeleeAttackType::Guard)));
                            melee->currentAttackNode = node;
                            melee->stateTimer = 0.0f;
                            melee->hasHitInThisAttack = true;
                        }
                    }
                    if (mphase == static_cast<std::uint8_t>(MeleePhase::Stunned) && melee->currentPhase != MeleePhase::Stunned) {
                        melee->currentPhase = MeleePhase::Stunned; melee->stunTimer = 0.8f; melee->currentAttackNode.clear();
                    }
                }
            }
            break;
        }
        case NM_Damage: {
            if (!m_Net.authority) break;
            const int n = r.U8();
            for (int i = 0; i < n; ++i) {
                const std::uint32_t id = r.U32(); const float dmg = RF(r);
                if (!r.ok) break;
                auto it = m_Net.enemies.find(id);
                if (it == m_Net.enemies.end() || dmg <= 0.0f) continue;
                NetEnemyInfo& en = it->second;
                if (!reg.valid(en.e) || reg.all_of<CorpseComponent>(en.e)) continue;
                auto& h = reg.get<HealthComponent>(en.e);
                if (h.currentHealth <= 0.0f) continue;
                h.currentHealth = std::max(0.0f, h.currentHealth - std::min(dmg, 5000.0f));
                en.prevHp = h.currentHealth;
                en.lastHitBy = from;
            }
            break;
        }
        case NM_HitPlayer: {
            const Net::PeerId target = r.U64(); const float dmg = RF(r);
            if (!r.ok || target != m_Party.SelfId() || dmg <= 0.0f) break;
            if (auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity); hp && hp->currentHealth > 0.0f && !reg.all_of<CorpseComponent>(m_PlayerEntity))
                hp->currentHealth = std::max(0.0f, hp->currentHealth - std::min(dmg, 5000.0f));
            break;
        }
        case NM_Killed: {
            const std::uint32_t id = r.U32(); const Net::PeerId killer = r.U64();
            if (!r.ok) break;
            auto it = m_Net.enemies.find(id);
            if (it != m_Net.enemies.end() && reg.valid(it->second.e)) {
                if (auto* g = reg.try_get<NetGhostComponent>(it->second.e)) {
                    g->killConfirmed = true;
                    reg.get<HealthComponent>(it->second.e).currentHealth = 0.0f;   // cade al prossimo controllo delle morti
                }
            }
            if (killer == m_Party.SelfId()) AwardKill();
            break;
        }
        case NM_Shots:
        case NM_BotShots: {
            // colpi di un compagno (la sua copia) o di un bot simulato dall'host: scia, lampo e suono, niente danno
            entt::entity shooter = entt::null;
            if (type == NM_Shots) { auto it = m_Net.remotes.find(from); if (it != m_Net.remotes.end() && reg.valid(it->second.puppet)) shooter = it->second.puppet; }
            const int n = r.U8();
            for (int i = 0; i < n; ++i) {
                if (type == NM_BotShots) {
                    const std::uint32_t bid = r.U32();
                    auto it = m_Net.enemies.find(bid);
                    shooter = (it != m_Net.enemies.end() && reg.valid(it->second.e)) ? it->second.e : entt::entity(entt::null);
                }
                TrailRequestComponent t;
                t.start = RV(r); t.end = RV(r);
                const float cr = r.U8() / 255.0f, cg = r.U8() / 255.0f, cb = r.U8() / 255.0f;
                t.color = glm::vec4(cr, cg, cb, r.U8() / 255.0f);
                t.travelTime = std::clamp(RF(r), 0.0f, 2.0f); t.totalLife = std::clamp(RF(r), 0.0f, 3.0f);
                t.projectileWidth = std::clamp(RF(r), 0.0f, 1.0f); t.impactScale = std::clamp(RF(r), 0.0f, 20.0f);
                t.style = r.U8(); t.fx = static_cast<int>(r.U8()) - 1;
                const std::uint8_t fl = r.U8();
                if (!r.ok) break;
                if (glm::distance(t.start, t.end) > 600.0f) continue;
                t.isBeam = (fl & 1) != 0; t.hitSomething = (fl & 2) != 0; t.hitUnit = (fl & 4) != 0;
                const entt::entity te = m_Scene.CreateEntity();
                reg.emplace<TrailRequestComponent>(te, t);
                if (fl & 8) {   // primo proiettile del colpo: lampo alla bocca e suono
                    glm::vec3 dir = t.end - t.start;
                    dir = glm::length(dir) > 1e-4f ? glm::normalize(dir) : glm::vec3(0.0f, 0.0f, -1.0f);
                    const WeaponFx fx = t.fx >= 0 ? static_cast<WeaponFx>(t.fx) : WeaponFx::Ballistic;
                    VfxEvent mz; mz.kind = VfxKind::Muzzle; mz.fx = fx; mz.pos = t.start; mz.dir = dir; mz.color = t.color; EmitVfx(mz);
                    FeelEvent fe; fe.kind = FeelKind::Shot; fe.fx = fx; fe.pos = t.start; fe.dir = dir; fe.color = t.color; fe.scale = 10.0f;
                    if (shooter != entt::null) fe.source = static_cast<std::uint32_t>(shooter);
                    EmitFeel(fe);
                    if (shooter != entt::null) if (auto* ws = reg.try_get<WeaponStateComponent>(shooter)) ws->currentAmmo = std::max(1, ws->currentAmmo - 1);   // gesto dello sparo
                }
            }
            break;
        }
        case NM_Squad: {   // Vs Bot: la squadra di un altro giocatore vero
            if (!m_VsBotActive) break;
            const int active = static_cast<int>(r.U8()) - 1; const int n = r.U8();
            std::vector<VsBotSlot> squad;
            for (int i = 0; i < n && r.ok; ++i) {
                VsBotSlot sl;
                if (!ReadUnit(r, sl.unit)) break;
                sl.points = r.U16();
                const int st = r.U8();
                sl.state = st == 1 ? VsBotSlotState::InField : (st == 2 ? VsBotSlotState::Fallen : VsBotSlotState::Reserve);
                squad.push_back(sl);
            }
            if (!r.ok) break;
            for (auto& p : m_VsBotPlayers) if (p.human && !p.local && p.owner == from) { p.squad = std::move(squad); p.active = active < static_cast<int>(p.squad.size()) ? active : -1; break; }
            break;
        }
        case NM_Roster: {   // Vs Bot, dall'host: squadre, bot, tempi, fine partita
            if (!m_VsBotActive || m_Net.authority) break;
            const float warm = RF(r), time = RF(r);
            const int winner = static_cast<int>(r.U8()) - 2; const int why = r.U8();
            const int n = r.U8();
            for (int i = 0; i < n && r.ok; ++i) {
                const int team = r.U8() == 1 ? 1 : 0; const int active = static_cast<int>(r.U8()) - 1; const float resp = RF(r);
                const int k = r.U8();
                std::vector<std::uint8_t> st;
                for (int j = 0; j < k; ++j) st.push_back(r.U8());
                if (!r.ok || i >= static_cast<int>(m_VsBotPlayers.size())) continue;
                auto& p = m_VsBotPlayers[i];
                p.team = team;
                if (p.human) continue;   // giocatori veri: la squadra la mandano loro
                p.active = active < static_cast<int>(p.squad.size()) ? active : -1;
                p.respawnTimer = resp;
                for (int j = 0; j < k && j < static_cast<int>(p.squad.size()); ++j)
                    p.squad[j].state = st[j] == 1 ? VsBotSlotState::InField : (st[j] == 2 ? VsBotSlotState::Fallen : VsBotSlotState::Reserve);
            }
            if (!r.ok) break;
            m_VsBotWarmup = warm;
            m_VsBotTime = time;
            if (winner >= -1 && m_VsBotResult == 0) VsBotFinish(winner, why);
            break;
        }
        case NM_VsJoin: {   // all'host: un compagno arrivato tardi chiede un posto nel Vs Bot
            if (!m_VsBotActive || !m_Net.authority) break;
            const std::string name = r.Str();
            if (!r.ok) break;
            if (!VsBotAdmitHuman(from, name.empty() ? std::string("Compagno") : name)) {
                Writer w; w.U8(NM_VsJoinNo); w.U32(m_Net.matchId); w.U64(from);
                m_Party.SendToAll(w.b.data(), w.b.size(), true);
                break;
            }
            Writer w; w.U8(NM_VsLineup); w.U32(m_Net.matchId); w.U64(from);
            WriteRoster(w, m_VsBotTier, m_VsBotPlayers);
            m_Party.SendToAll(w.b.data(), w.b.size(), true);
            m_Net.rosterTimer = 0.0f;
            m_PartyToasts.push_back({ name + " entra nel Vs Bot al posto di un bot", 3.0f });
            break;
        }
        case NM_VsLineup: {   // dall'host, gia' in partita: un bot e' diventato un giocatore vero
            if (!m_VsBotActive || m_Net.authority) break;
            (void)r.U64();
            int tier = 0; std::vector<VsBotPlayer> pl;
            if (!r.ok || !ReadRoster(std::vector<std::uint8_t>(r.p, r.p + r.n), tier, pl) || pl.size() != m_VsBotPlayers.size()) break;
            for (size_t i = 0; i < pl.size(); ++i) {
                auto& p = m_VsBotPlayers[i];
                if (!pl[i].human || p.human) continue;
                // il suo bot non era ancora in campo: ora e' un compagno, la sua squadra arriva da lui
                p.human = true; p.local = false; p.owner = pl[i].owner; p.name = pl[i].name;
                p.squad.clear(); p.active = -1;
            }
            break;
        }
        case NM_TeamSwitch: {   // all'host: un giocatore vero vuole passare all'altra squadra
            if (!m_VsBotActive || !m_Net.authority) break;
            for (int i = 0; i < static_cast<int>(m_VsBotPlayers.size()); ++i) if (m_VsBotPlayers[i].human && m_VsBotPlayers[i].owner == from) { VsBotSwitchTeam(i); break; }
            break;
        }
        case NM_Despawn: {   // un nemico sparisce senza morire
            if (m_Net.authority) break;
            const std::uint32_t id = r.U32();
            if (!r.ok) break;
            auto it = m_Net.enemies.find(id);
            if (it == m_Net.enemies.end()) break;
            const entt::entity e = it->second.e;
            for (auto& p : m_VsBotPlayers) for (auto& sl : p.squad) if (sl.entity == e) sl.entity = entt::null;
            if (reg.valid(e) && !reg.all_of<CorpseComponent>(e)) { m_PhysicsSystem.DestroyJoltBody(m_Scene, e); m_Scene.DestroyEntity(e); }
            m_Net.enemies.erase(it);
            break;
        }
        default: break;
        }
    }

    // Vs Bot in squadra: ognuno manda la sua squadra; l'host manda a tutti lo stato del roster (2 volte al secondo)
    void GameLayer::NetVsBotTick(float realDt) {
        const int me = VsBotMeIndex();
        if (me >= 0) {
            Writer w; w.U8(NM_Squad); w.U32(m_Net.matchId); WriteSquad(w, m_VsBotPlayers[me]);
            const std::string key(w.b.begin(), w.b.end());
            m_Net.squadTimer -= realDt;
            if (key != m_Net.squadKey || m_Net.squadTimer <= 0.0f) {
                m_Net.squadKey = key; m_Net.squadTimer = 3.0f;
                m_Party.SendToAll(w.b.data(), w.b.size(), true);
            }
        }
        if (!m_Net.authority) return;
        m_Net.rosterTimer -= realDt;
        if (m_Net.rosterTimer > 0.0f) return;
        m_Net.rosterTimer = 0.5f;
        Writer w; w.U8(NM_Roster); w.U32(m_Net.matchId);
        WF(w, m_VsBotWarmup); WF(w, m_VsBotTime);
        w.U8(static_cast<std::uint8_t>(m_VsBotResult != 0 ? m_VsBotWinner + 2 : 0)); w.U8(static_cast<std::uint8_t>(m_VsBotWhy));
        w.U8(static_cast<std::uint8_t>(std::min<size_t>(m_VsBotPlayers.size(), 20)));
        for (size_t i = 0; i < m_VsBotPlayers.size() && i < 20; ++i) {
            const auto& p = m_VsBotPlayers[i];
            w.U8(static_cast<std::uint8_t>(p.team)); w.U8(static_cast<std::uint8_t>(p.active + 1)); WF(w, p.respawnTimer);
            const size_t n = p.human ? 0 : std::min<size_t>(p.squad.size(), 40);
            w.U8(static_cast<std::uint8_t>(n));
            for (size_t k = 0; k < n; ++k) w.U8(static_cast<std::uint8_t>(p.squad[k].state));
        }
        m_Party.SendToAll(w.b.data(), w.b.size(), true);
    }

    // Vs Bot gia' avviato senza di me: chiedo all'host un posto (lo prendo al posto di un bot finche' i bot non scendono
    // in campo, negli ultimi 3 secondi del riscaldamento)
    void GameLayer::NetRequestVsBotJoin(std::uint32_t matchId) {
        if (matchId == 0) return;
        m_Net.joinRequestMatch = matchId;
        Writer w; w.U8(NM_VsJoin); w.U32(matchId); w.Str(m_Profile.netName);
        m_Party.SendToAll(w.b.data(), w.b.size(), true);
        m_PartyToasts.push_back({ "Chiedo un posto nel Vs Bot della squadra...", 3.0f });
    }

    // [T] nel riscaldamento: chiede all'host di passare all'altra squadra (da soli lo si fa subito)
    void GameLayer::NetRequestTeamSwitch() {
        if (m_Net.active && !m_Net.authority) {
            Writer w; w.U8(NM_TeamSwitch); w.U32(m_Net.matchId);
            m_Party.SendToHost(w.b.data(), w.b.size(), true);
            m_PartyToasts.push_back({ "Richiesta di cambio squadra inviata", 2.0f });
        }
        else VsBotSwitchTeam(VsBotMeIndex());
    }

    // Host: un nemico sparisce senza morire (le copie degli altri spariscono subito invece che dopo qualche secondo)
    void GameLayer::NetDespawnEnemy(entt::entity e) {
        if (!m_Net.active || !m_Net.authority) return;
        const auto* ne = m_Scene.GetRegistry().try_get<NetEntityComponent>(e);
        if (!ne || ne->netId == 0) return;
        Writer w; w.U8(NM_Despawn); w.U32(m_Net.matchId); w.U32(ne->netId);
        m_Party.SendToAll(w.b.data(), w.b.size(), true);
        m_Net.enemies.erase(ne->netId);
    }

    // ------------------------------------------------------------------------------------------------
    // A terra o in pausa durante una partita in rete: niente comandi, ma il mondo va avanti (l'host deve continuare
    // a muovere i nemici per tutti, gli altri a vedere le copie)
    void GameLayer::UpdateNetWorld(float deltaTime) {
        auto& reg = m_Scene.GetRegistry();
        if (!m_IsPaused) m_WeaponSystem.Update(m_Scene, m_PhysicsSystem, deltaTime);   // in pausa no: i clic sul menu sparerebbero
        m_ExplosiveSystem.Update(m_Scene, deltaTime);
        m_PhysicsSystem.Update(m_Scene, deltaTime);
        CorpseSystem::Update(m_Scene, m_PhysicsSystem, deltaTime);
        UpdateHitFeedback(deltaTime);
        UpdateTrainingDummies(deltaTime);
        m_HordeAI.Update(m_Scene, m_NavQuery, deltaTime);
        m_MeleeCombat.Update(m_Scene, deltaTime);
        // ondate attorno a chi e' ancora in piedi
        glm::vec3 center(0.0f); bool found = false;
        if (const auto* hp = reg.try_get<HealthComponent>(m_PlayerEntity); hp && hp->currentHealth > 0.0f && !reg.all_of<CorpseComponent>(m_PlayerEntity)) {
            center = reg.get<TransformComponent>(m_PlayerEntity).Position; found = true;
        }
        for (const auto& [id, r] : m_Net.remotes) if (!found && r.alive && r.hasState) { center = r.pos; found = true; }
        if (found) UpdateWaaaghWaves(deltaTime, center);
        CheckUnitDeaths();
        glm::vec3 playerPos(0.0f);
        if (const auto* pt = reg.try_get<TransformComponent>(m_PlayerEntity)) playerPos = pt->Position;
        AnimateUnits(deltaTime, playerPos, false);
        ActionPose::ApplyCubes(m_Scene, m_PlayerEntity);
    }

    // Nomi dei compagni sopra le loro unita' e, in alto, chi muove i nemici
    void GameLayer::RenderNetMatchOverlay() {
        if (!m_Net.active) return;
        auto& reg = m_Scene.GetRegistry();
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const glm::mat4 vp = m_Camera.GetViewProjection();
        int inField = 1;
        for (const auto& [id, r] : m_Net.remotes) {
            if (!reg.valid(r.puppet) || reg.all_of<CorpseComponent>(r.puppet)) continue;
            ++inField;
            if (m_VsBotActive) continue;   // nel Vs Bot i nomi li mostra il suo HUD (alleati e avversari)
            float h = 2.2f;
            if (const auto* col = reg.try_get<ColliderComponent>(r.puppet)) h = std::max(1.0f, (col->localCenterOffset.y + col->halfExtents.y) * 1.08f + 0.25f);
            const glm::vec3 p3 = reg.get<TransformComponent>(r.puppet).Position + glm::vec3(0.0f, h, 0.0f);
            const glm::vec4 clip = vp * glm::vec4(p3, 1.0f);
            if (clip.w <= 0.01f) continue;
            const ImVec2 p((clip.x / clip.w * 0.5f + 0.5f) * io.DisplaySize.x, (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * io.DisplaySize.y);
            if (p.x < -200.0f || p.x > io.DisplaySize.x + 200.0f || p.y < -100.0f || p.y > io.DisplaySize.y + 100.0f) continue;
            ImFont* f = Ui::FontHeading();
            const float fs = f->FontSize * 0.8f;
            const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, r.name.c_str());
            const ImVec2 a(p.x - ts.x * 0.5f - 8.0f, p.y - ts.y - 6.0f), b(p.x + ts.x * 0.5f + 8.0f, p.y);
            Ui::ChamferRect(dl, a, b, 5.0f, IM_COL32(10, 10, 12, 170), Ui::U32(Ui::Col::Verdigris, 0.85f), 1.0f);
            dl->AddText(f, fs, ImVec2(a.x + 8.0f, a.y + 3.0f), Ui::U32(Ui::Col::Bone), r.name.c_str());
            // salute
            const float frac = std::clamp(r.health / std::max(1.0f, reg.get<HealthComponent>(r.puppet).maxHealth), 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(a.x, b.y + 2.0f), ImVec2(b.x, b.y + 5.0f), IM_COL32(20, 20, 20, 170));
            dl->AddRectFilled(ImVec2(a.x, b.y + 2.0f), ImVec2(a.x + (b.x - a.x) * frac, b.y + 5.0f), Ui::U32(frac > 0.35f ? Ui::Col::Verdigris : Ui::Col::Ember, 0.9f));
        }
        // riga in alto al centro
        std::string hostName = m_Party.IsHost() ? std::string("tu") : std::string("?");
        for (const auto& mb : m_Party.Members()) if (mb.id == m_Party.HostId() && !mb.self) hostName = mb.name;
        char line[160];
        std::snprintf(line, sizeof(line), m_VsBotActive ? "GIOCATORI VERI IN CAMPO %d   |   BOT MOSSI DA: %s" : "SQUADRA IN CAMPO %d   |   NEMICI MOSSI DA: %s", inField, hostName.c_str());
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(line);
        const float y0 = m_VsBotActive ? 46.0f : 10.0f;   // nel Vs Bot sotto la sua riga in alto
        const ImVec2 a(io.DisplaySize.x * 0.5f - ts.x * 0.5f - 12.0f, y0), b(io.DisplaySize.x * 0.5f + ts.x * 0.5f + 12.0f, y0 + 4.0f + ts.y + 6.0f);
        Ui::ChamferRect(fg, a, b, 5.0f, IM_COL32(10, 10, 12, 170), Ui::U32(Ui::Col::Brass, 0.6f), 1.0f);
        fg->AddText(ImVec2(a.x + 12.0f, a.y + 5.0f), Ui::U32(Ui::Col::Bone, 0.9f), line);
    }
}
