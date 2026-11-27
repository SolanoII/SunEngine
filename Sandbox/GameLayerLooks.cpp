// Trasmogrificazione: l'aspetto di un oggetto sbloccato su un altro dello stesso posto.
//
// - Un aspetto si sblocca avendo l'oggetto: in dotazione a un'unita' del garage oppure comprato/ottenuto (CreateItem).
//   Resta per sempre (PlayerProfile::looksOwned) e si usa gratis su quanti oggetti si vuole.
// - Armature: stesso posto (ArmorSlot) e pezzo che l'unita' potrebbe indossare (i corpi sono diversi).
//   Armi ed equipaggiamento: stesso tipo preciso ("look_type" nel JSON, es. fucile automatico su fucile automatico).
// - Le statistiche sono sempre quelle dell'oggetto vero: l'aspetto cambia solo cosa si disegna.
// - Opzione (m_Profile.lookMode): 0 tutti gli aspetti, 1 tutti tranne quelli degli avversari, 2 solo i miei, 3 nessuno
//   (chi preferisce vedere l'equipaggiamento com'e' davvero).
#include "GameLayer.h"
#include "ItemRarity.h"
#include <imgui.h>

namespace SunEngine {

    namespace {
        // Slot 0..5 = armi ed equipaggiamento, 6.. = posti dell'armatura (ArmorSlot)
        bool IsArmorSlot(int slot) { return slot >= 6; }
    }

    void GameLayer::CollectLooks() {
        auto& owned = m_Profile.looksOwned;
        auto add = [&](const std::string& id) {
            if (id.empty() || id == "none") return;
            owned.insert(BlueprintManager::BaseId(id));
        };
        for (const auto& it : m_Inventory) add(it.itemId);
        for (auto& u : m_PlayerRoster) {
            for (auto* slot : u.Slots()) add(*slot);
            if (const auto* bp = BlueprintManager::Get().GetUnit(u.blueprintId)) {
                add(bp->default_primary); add(bp->default_secondary); add(bp->default_melee);
                for (const auto& e : bp->default_equipment) add(e);
                for (const auto& a : bp->default_armor) add(a);
            }
        }
    }

    bool GameLayer::LooksVisible(int owner) const {
        switch (m_Profile.lookMode) {
        case 1: return owner != kLookEnemy;   // nascondi quelli degli avversari
        case 2: return owner == kLookMine;    // solo i miei
        case 3: return false;                 // nessuno: equipaggiamento com'e'
        default: return true;
        }
    }

    void GameLayer::AttachLooks(entt::entity e, const TrainedUnit& unit, int owner) {
        auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(e)) return;
        LookComponent lc;
        if (LooksVisible(owner)) {
            auto& u = const_cast<TrainedUnit&>(unit);
            auto slots = u.Slots();
            for (int i = 0; i < TrainedUnit::kSlotCount; ++i) {
                const std::string& item = *slots[i];
                if (item.empty() || item == "none" || unit.looks[i].empty()) continue;
                // aspetto che questa versione conosce e dello stesso posto/tipo (puo' arrivare dalla rete)
                const auto& bpm = BlueprintManager::Get();
                if (IsArmorSlot(i)) {
                    const auto* a = bpm.GetArmorPiece(unit.looks[i]);
                    if (!a || a->slot != i - 6) continue;
                }
                else {
                    const auto* w = bpm.GetWeapon(unit.looks[i]);
                    const auto* real = bpm.GetWeapon(BlueprintManager::BaseId(item));
                    if (!w || !real || w->look_type.empty() || w->look_type != real->look_type) continue;
                }
                lc.map[item] = unit.looks[i];
                lc.map[BlueprintManager::BaseId(item)] = unit.looks[i];
            }
        }
        if (lc.map.empty()) { if (reg.all_of<LookComponent>(e)) reg.remove<LookComponent>(e); }
        else reg.emplace_or_replace<LookComponent>(e, std::move(lc));
    }

    void GameLayer::RefreshLooksInField() {
        auto& reg = m_Scene.GetRegistry();
        if (reg.valid(m_PlayerEntity) && !m_PlayerLookUnit.blueprintId.empty()) {
            AttachLooks(m_PlayerEntity, m_PlayerLookUnit, kLookMine);
            ApplyArmorSkin(m_PlayerEntity, m_PlayerLookUnit);
        }
        const auto* mine = reg.valid(m_PlayerEntity) ? reg.try_get<TeamComponent>(m_PlayerEntity) : nullptr;
        for (const auto& [e, unit] : m_VsBotUnits) {
            if (!reg.valid(e)) continue;
            const auto* t = reg.try_get<TeamComponent>(e);
            AttachLooks(e, unit, (t ? t->team : 0) == (mine ? mine->team : 0) ? kLookAlly : kLookEnemy);
            ApplyArmorSkin(e, unit);
        }
    }

    std::string GameLayer::LookOf(entt::entity e, const std::string& itemId) const {
        if (itemId.empty()) return itemId;
        const auto& reg = m_Scene.GetRegistry();
        if (!reg.valid(e)) return itemId;
        const auto* lc = reg.try_get<LookComponent>(e);
        if (!lc) return itemId;
        auto it = lc->map.find(itemId);
        if (it == lc->map.end()) it = lc->map.find(BlueprintManager::BaseId(itemId));
        return it == lc->map.end() ? itemId : it->second;
    }

    std::vector<std::string> GameLayer::LookChoices(const TrainedUnit& unit, int slot) const {
        std::vector<std::string> out;
        if (slot < 0 || slot >= TrainedUnit::kSlotCount) return out;
        const auto& bpm = BlueprintManager::Get();
        auto& u = const_cast<TrainedUnit&>(unit);
        const std::string cur = BlueprintManager::BaseId(*u.Slots()[slot]);
        if (cur.empty() || cur == "none") return out;
        const auto& owned = m_Profile.looksOwned;
        if (IsArmorSlot(slot)) {
            const int as = slot - 6;
            for (const auto& [id, a] : bpm.GetArmorPieces()) {
                if (id == cur || a.slot != as || !owned.count(id) || !bpm.CanEquip(a, unit.blueprintId)) continue;
                out.push_back(id);
            }
        }
        else {
            const auto* w = bpm.GetWeapon(cur);
            if (!w || w->look_type.empty()) return out;
            for (const auto& [id, o] : bpm.GetWeapons()) {
                if (id == cur || o.look_type != w->look_type || !owned.count(id)) continue;
                out.push_back(id);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    bool GameLayer::SanitizeLooks(TrainedUnit& unit) const {
        bool changed = false;
        for (int i = 0; i < TrainedUnit::kSlotCount; ++i) {
            if (unit.looks[i].empty()) continue;
            const auto choices = LookChoices(unit, i);
            if (std::find(choices.begin(), choices.end(), unit.looks[i]) == choices.end()) { unit.looks[i].clear(); changed = true; }
        }
        return changed;
    }

    bool GameLayer::DrawLookCombo(TrainedUnit& unit, int slot) {
        if (slot < 0 || slot >= TrainedUnit::kSlotCount) return false;
        const auto& bpm = BlueprintManager::Get();
        const auto choices = LookChoices(unit, slot);
        std::string& look = unit.looks[slot];
        if (!look.empty() && std::find(choices.begin(), choices.end(), look) == choices.end()) look.clear();
        auto nameOf = [&](const std::string& id) -> std::string {
            if (IsArmorSlot(slot)) { const auto* a = bpm.GetArmorPiece(id); return a ? a->displayName : id; }
            const auto* w = bpm.GetWeapon(id); return w ? w->displayName : id;
        };
        bool changed = false;
        ImGui::PushID(slot + 1000);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.62f, 0.55f, 0.75f, 1.0f), "  Aspetto");
        ImGui::SameLine();
        if (choices.empty()) {
            ImGui::TextDisabled("%s", look.empty() ? "il suo (nessun altro aspetto sbloccato per questo posto)" : nameOf(look).c_str());
        }
        else {
            ImGui::SetNextItemWidth(-1.0f);
            const std::string preview = look.empty() ? std::string("[Il suo aspetto]") : nameOf(look);
            if (ImGui::BeginCombo("##look", preview.c_str())) {
                if (ImGui::Selectable("[Il suo aspetto]", look.empty())) { look.clear(); changed = true; }
                for (const auto& id : choices) {
                    if (ImGui::Selectable(nameOf(id).c_str(), look == id)) { look = id; changed = true; }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Solo l'aspetto: statistiche, peso e costo restano quelli dell'oggetto equipaggiato.\nGratis: l'aspetto e' tuo da quando hai l'oggetto.");
                }
                ImGui::EndCombo();
            }
        }
        ImGui::PopID();
        return changed;
    }

}
