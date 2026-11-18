// =============================================================================================================
// Ricostruzione dei personaggi speciali
// ---------------------------------------------------------------------------------------------------------
// I personaggi speciali (unita' d'elite dell'albero di ricerca: Konrad, il vecchio zhelvar...) hanno le
// statistiche del grado successivo ma il costo in punti del loro grado. Il prezzo lo pagano qui: quando cadono
// in partita (sopravvivenza, sandbox, Vs Bot) non tornano subito disponibili. Restano "in ricostruzione" per un
// tempo reale che cresce col grado, oppure si rimettono in servizio subito pagando energia (la risorsa con cui
// si compra l'equipaggiamento), tanto meno quanto meno manca. Nelle storie (PVE-STORY) non succede: li' il
// personaggio e' quello del racconto, non quello del garage.
// =============================================================================================================
#include "GameLayer.h"
#include "UiTheme.h"
#include "Engine/Core/Logger/Log.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

namespace SunEngine {

    namespace {
        int64_t NowSeconds() { return static_cast<int64_t>(std::time(nullptr)); }

        // Minuti reali di ricostruzione per grado
        int64_t RebuildMinutes(int tier) {
            switch (tier) {
            case 1: case 2: case 3: return 10;
            case 4: return 20;
            case 5: return 30;
            case 6: return 45;
            default: return 60;
            }
        }

        std::string MinSec(int64_t s) {
            char b[32];
            if (s >= 3600) std::snprintf(b, sizeof(b), "%lld:%02lld:%02lld", static_cast<long long>(s / 3600), static_cast<long long>((s / 60) % 60), static_cast<long long>(s % 60));
            else std::snprintf(b, sizeof(b), "%lld:%02lld", static_cast<long long>(s / 60), static_cast<long long>(s % 60));
            return b;
        }
    }

    int GameLayer::SpecialTierOf(const TrainedUnit& u) const {
        const ResearchNode* n = m_ResearchTree.FindNodeByUnit(u.blueprintId);
        return (n && n->premium) ? std::max(1, n->tier) : 0;
    }

    int64_t GameLayer::RebuildSecondsLeft(const TrainedUnit& u) const {
        if (u.rebuildUntil <= 0) return 0;
        return std::max<int64_t>(0, u.rebuildUntil - NowSeconds());
    }

    // Energia per ricostruirlo subito: 10.000 per grado a ricostruzione appena iniziata, in proporzione al tempo
    // che manca (minimo 500)
    int64_t GameLayer::RebuildEnergyCost(const TrainedUnit& u) const {
        const int64_t left = RebuildSecondsLeft(u);
        if (left <= 0) return 0;
        const int tier = std::max(1, SpecialTierOf(u));
        const int64_t total = RebuildMinutes(tier) * 60;
        const int64_t full = static_cast<int64_t>(tier) * 10000;
        return std::max<int64_t>(500, full * std::min(left, total) / std::max<int64_t>(1, total));
    }

    void GameLayer::MarkSpecialFallen(int rosterIdx) {
        if (rosterIdx < 0 || rosterIdx >= static_cast<int>(m_PlayerRoster.size())) return;
        TrainedUnit& u = m_PlayerRoster[rosterIdx];
        const int tier = SpecialTierOf(u);
        if (tier <= 0 || RebuildSecondsLeft(u) > 0) return;   // unita' normale, o gia' in ricostruzione
        u.rebuildUntil = NowSeconds() + RebuildMinutes(tier) * 60;
        m_PartyToasts.push_back({ u.customName + " e' caduto: in ricostruzione per " + std::to_string(RebuildMinutes(tier)) + " minuti", 5.0f });
        SUN_CORE_INFO("Ricostruzione: {0} (grado {1}) torna disponibile tra {2} minuti", u.customName, tier, RebuildMinutes(tier));
        SaveProfileIfChanged();
    }

    bool GameLayer::RebuildNow(int rosterIdx) {
        if (rosterIdx < 0 || rosterIdx >= static_cast<int>(m_PlayerRoster.size())) return false;
        TrainedUnit& u = m_PlayerRoster[rosterIdx];
        const int64_t cost = RebuildEnergyCost(u);
        if (cost <= 0 || m_Profile.energy < cost) return false;
        m_Profile.energy -= cost;
        u.rebuildUntil = 0;
        SUN_CORE_INFO("Ricostruzione: {0} rimesso in servizio per {1} energia", u.customName, cost);
        SaveProfileIfChanged();
        return true;
    }

    int GameLayer::PruneRebuildingFromSquad() {
        int removed = 0;
        for (auto it = m_SelectedSquadIndices.begin(); it != m_SelectedSquadIndices.end();) {
            const int idx = *it;
            if (idx >= 0 && idx < static_cast<int>(m_PlayerRoster.size()) && RebuildSecondsLeft(m_PlayerRoster[idx]) > 0) {
                it = m_SelectedSquadIndices.erase(it);
                ++removed;
            }
            else ++it;
        }
        if (removed > 0) SUN_CORE_WARN("{0} personaggi speciali in ricostruzione tolti dalla squadra", removed);
        return removed;
    }

    void GameLayer::RenderRebuildPanel(int rosterIdx) {
        if (rosterIdx < 0 || rosterIdx >= static_cast<int>(m_PlayerRoster.size())) return;
        const TrainedUnit& u = m_PlayerRoster[rosterIdx];
        const int tier = SpecialTierOf(u);
        if (tier <= 0) return;
        ImGui::Spacing();
        const int64_t left = RebuildSecondsLeft(u);
        if (left <= 0) {
            ImGui::TextColored(Ui::Col::Gold, "Personaggio speciale: pronto");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Statistiche del grado successivo, costo in punti del suo grado.\nSe cade in partita va ricostruito: %lld minuti reali, oppure subito pagando energia.",
                                  static_cast<long long>(RebuildMinutes(tier)));
            return;
        }
        const int64_t total = RebuildMinutes(tier) * 60;
        ImGui::TextColored(Ui::Col::Rust, "In ricostruzione: pronto tra %s", MinSec(left).c_str());
        Ui::Meter(1.0f - std::clamp(static_cast<float>(left) / static_cast<float>(std::max<int64_t>(1, total)), 0.0f, 1.0f), ImVec2(ImGui::GetContentRegionAvail().x, 6.0f), Ui::Col::Rust);
        const int64_t cost = RebuildEnergyCost(u);
        const bool canPay = m_Profile.energy >= cost;
        char label[96];
        std::snprintf(label, sizeof(label), canPay ? "RICOSTRUISCI ORA   %lld ENERGIA" : "ENERGIA INSUFFICIENTE (%lld)", static_cast<long long>(cost));
        if (Ui::Button(label, ImVec2(-1.0f, 32.0f), Ui::Btn::Primary, canPay)) RebuildNow(rosterIdx);
    }

}
