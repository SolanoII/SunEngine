// Rarita' degli oggetti e magazzino (regole e statistiche in ItemRarity.h).
//
// Ogni oggetto creato in officina (comprato dopo la ricerca), trovato come bottino o preso agli avversari e' un
// esemplare nel magazzino, con rarita', tratti e condizione. Le unita' lo equipaggiano dal garage; nel loro slot
// l'esemplare sta come "id#u<numero>#r..#c..#t..", e da quel testo il BlueprintManager ricava le statistiche.
// La dotazione base resta sempre disponibile, comune e senza usura.
#include "GameLayer.h"
#include "UiTheme.h"
#include "DamageUi.h"
#include "Engine/Core/Logger/Log.h"
#include <imgui.h>
#include <algorithm>
#include <random>

namespace SunEngine {

    namespace {
        std::mt19937& ItemRng() { static std::mt19937 rng{ std::random_device{}() }; return rng; }
        const char* kStoreFilters[5] = { "Tutto", "Fuoco", "Bianche", "Equip.", "Armature" };
        int FilterOf(ItemKind k) { return k == ItemKind::Ranged ? 1 : k == ItemKind::Melee ? 2 : k == ItemKind::Equipment ? 3 : 4; }
    }

    void GameLayer::InitItemRarity() {
        BlueprintManager::Get().SetVariantBuilders(
            [](WeaponBlueprint& w, const std::string& suffix) { ApplyWeaponRarity(w, suffix); },
            [](ArmorBlueprint& a, const std::string& suffix) { ApplyArmorRarity(a, suffix); });
    }

    // ------------------------------------------------------------------------------------ profilo
    void GameLayer::InventoryFromProfile() {
        m_Inventory.clear();
        m_NextItemUid = std::max<uint64_t>(1, m_Profile.nextItemUid);
        for (const auto& j : m_Profile.inventory) {
            if (!j.is_object()) continue;
            ItemInstance it;
            it.uid = j.value("uid", static_cast<uint64_t>(0));
            it.itemId = j.value("item", std::string());
            it.rarity = std::clamp(j.value("rarity", 0), 0, 4);
            it.condition = std::clamp(j.value("condition", 100.0f), 0.0f, 100.0f);
            if (j.contains("traits") && j["traits"].is_array())
                for (const auto& t : j["traits"]) if (t.is_string() && FindTrait(t.get<std::string>())) it.traits.push_back(t.get<std::string>());
            const bool exists = BlueprintManager::Get().GetWeapon(it.itemId) || BlueprintManager::Get().GetArmorPiece(it.itemId);
            if (it.uid == 0 || !exists) continue;   // oggetto non piu' nel gioco
            m_NextItemUid = std::max(m_NextItemUid, it.uid + 1);
            m_Inventory.push_back(std::move(it));
        }
    }

    void GameLayer::InventoryToProfile() {
        m_Profile.inventory = nlohmann::json::array();
        for (const auto& it : m_Inventory)
            m_Profile.inventory.push_back({ {"uid", it.uid}, {"item", it.itemId}, {"rarity", it.rarity}, {"condition", it.condition}, {"traits", it.traits} });
        m_Profile.nextItemUid = m_NextItemUid;
    }

    // Prima della rarita' gli oggetti ricercati si compravano "per unita'": ognuno diventa un esemplare comune del
    // magazzino, addosso a chi lo aveva equipaggiato. Gli esemplari che le unita' hanno negli slot ma che non sono piu'
    // nel magazzino tornano alla dotazione base.
    void GameLayer::MigrateOwnedItems() {
        int made = 0;
        for (auto& u : m_PlayerRoster) {
            for (const auto& id : u.ownedItems) {
                if (!BlueprintManager::Get().GetWeapon(id) && !BlueprintManager::Get().GetArmorPiece(id)) continue;
                ItemInstance& it = CreateItem(id, 0);
                ++made;
                for (auto* slot : u.Slots()) if (*slot == id) { *slot = it.Ref(); break; }
            }
            u.ownedItems.clear();
            auto slots = u.Slots();
            for (int i = 0; i < 12; ++i) {
                const uint64_t uid = InstanceUidOf(*slots[i]);
                if (uid == 0) continue;
                ItemInstance* it = FindItem(uid);
                if (!it || it->Broken()) *slots[i] = u.DefaultForSlot(i);
                else if (*slots[i] != it->Ref()) *slots[i] = it->Ref();   // rarita' o condizione cambiate
            }
        }
        if (made > 0) SUN_CORE_INFO("Magazzino: {0} oggetti comprati per le unita' diventati esemplari comuni", made);
    }

    // ------------------------------------------------------------------------------------ esemplari
    ItemInstance* GameLayer::FindItem(uint64_t uid) {
        for (auto& it : m_Inventory) if (it.uid == uid) return &it;
        return nullptr;
    }

    int GameLayer::ItemHolder(uint64_t uid) const {
        if (uid == 0) return -1;
        for (int i = 0; i < static_cast<int>(m_PlayerRoster.size()); ++i) {
            auto& u = const_cast<TrainedUnit&>(m_PlayerRoster[i]);
            for (auto* slot : u.Slots()) if (InstanceUidOf(*slot) == uid) return i;
        }
        return -1;
    }

    int64_t GameLayer::ItemValue(const std::string& itemId) const {
        const ResearchNode* n = m_ResearchTree.FindNodeByItem(BlueprintManager::BaseId(itemId));
        return (n && n->purchaseCost > 0) ? n->purchaseCost : 4000;
    }

    ItemInstance& GameLayer::CreateItem(const std::string& itemId, int rarity) {
        ItemInstance it;
        it.uid = m_NextItemUid++;
        it.itemId = BlueprintManager::BaseId(itemId);
        SetRarity(it, rarity, ItemRng());
        m_Inventory.push_back(std::move(it));
        return m_Inventory.back();
    }

    // Dopo un potenziamento, una riparazione o l'usura: le unita' che lo hanno addosso prendono il testo nuovo
    void GameLayer::RefreshItemRefs(uint64_t uid) {
        ItemInstance* it = FindItem(uid);
        for (auto& u : m_PlayerRoster) {
            auto slots = u.Slots();
            for (int i = 0; i < 12; ++i) {
                if (InstanceUidOf(*slots[i]) != uid) continue;
                *slots[i] = (it && !it->Broken()) ? it->Ref() : u.DefaultForSlot(i);
            }
        }
    }

    void GameLayer::RemoveItem(uint64_t uid) {
        m_Inventory.erase(std::remove_if(m_Inventory.begin(), m_Inventory.end(), [&](const ItemInstance& i) { return i.uid == uid; }), m_Inventory.end());
        RefreshItemRefs(uid);   // chi lo aveva torna alla dotazione base
        if (m_SelectedItemUid == uid) m_SelectedItemUid = 0;
    }

    // Morte con gli oggetti addosso: Wear = -34% di condizione (a zero si rompe), Break = si rompe, Lose = si perde
    void GameLayer::ApplyDeathToItems(int rosterIndex, DeathRule rule) {
        if (rule == DeathRule::None || rosterIndex < 0 || rosterIndex >= static_cast<int>(m_PlayerRoster.size())) return;
        std::vector<uint64_t> uids;
        for (auto* slot : m_PlayerRoster[rosterIndex].Slots()) if (const uint64_t u = InstanceUidOf(*slot)) uids.push_back(u);
        for (uint64_t uid : uids) {
            ItemInstance* it = FindItem(uid);
            if (!it) continue;
            if (rule == DeathRule::Lose) { SUN_CORE_INFO("Oggetto perso: {0}", it->itemId); RemoveItem(uid); continue; }
            it->condition = rule == DeathRule::Break ? 0.0f : std::max(0.0f, it->condition - kWearPerDeath);
            RefreshItemRefs(uid);
        }
    }

    // ------------------------------------------------------------------------------------ consumo con l'uso
    // Ogni fotogramma in partita (non nella storia): colpi sparati dall'arma in mano (il caricatore che scende senza
    // ricaricare) e piastre delle armature consumate dai colpi presi. I colpi in mischia si contano in CollectMatchXp.
    void GameLayer::TrackItemWear() {
        auto& reg = m_Scene.GetRegistry();
        if (m_Story.active || !reg.valid(m_PlayerEntity)) return;
        if (const auto* wid = reg.try_get<WeaponBlueprintId>(m_PlayerEntity)) {
            const auto* ws = reg.try_get<WeaponStateComponent>(m_PlayerEntity);
            const uint64_t uid = InstanceUidOf(wid->id);
            if (ws && wid->id == m_WearLastWeapon && m_WearLastAmmo >= 0 && ws->currentAmmo < m_WearLastAmmo && !ws->isReloading && uid)
                m_WearPending[uid] += kWearPerShot * static_cast<float>(m_WearLastAmmo - ws->currentAmmo);
            m_WearLastWeapon = wid->id;
            m_WearLastAmmo = ws ? ws->currentAmmo : -1;
        }
        if (const auto* st = reg.try_get<UnitStatsComponent>(m_PlayerEntity)) {
            const ArmorPiece* pieces[6] = { &st->armorSet.head, &st->armorSet.torso, &st->armorSet.leftArm, &st->armorSet.rightArm, &st->armorSet.leftLeg, &st->armorSet.rightLeg };
            for (const ArmorPiece* p : pieces) {
                const uint64_t uid = InstanceUidOf(p->id);
                if (!uid || p->maxDurability <= 0.0f) continue;
                const float lost = std::clamp(1.0f - p->currentDurability / p->maxDurability, 0.0f, 1.0f);
                float& w = m_ArmorWorn[uid];
                w = std::max(w, lost);
            }
        }
    }

    void GameLayer::FlushItemWear() {
        for (const auto& [uid, frac] : m_ArmorWorn) m_WearPending[uid] += frac * kArmorWearPerLife;
        for (const auto& [uid, loss] : m_WearPending) {
            ItemInstance* it = FindItem(uid);
            if (!it || loss <= 0.0f) continue;
            it->condition = std::max(0.0f, it->condition - loss);
            RefreshItemRefs(uid);
        }
        m_WearPending.clear();
        m_ArmorWorn.clear();
        m_WearLastWeapon.clear();
        m_WearLastAmmo = -1;
    }

    // Bottino di fine missione: un oggetto a caso fra quelli che le unita' della fazione possono usare
    uint64_t GameLayer::GrantMissionLoot(const std::string& faction, int minRarity) {
        auto& bpm = BlueprintManager::Get();
        std::string prefix = faction == "Kingship" ? "kingship_" : faction == "Vanguard" ? "vanguard_" : faction == "Vrepighs" ? "vrepighs_" : "";
        auto forFaction = [&](const std::vector<std::string>& allowed) {
            for (const auto& a : allowed) if (prefix.empty() || a.rfind(prefix, 0) == 0) return true;
            return false;
        };
        std::vector<std::string> pool;
        for (const auto& [id, w] : bpm.GetWeapons()) {
            if (w.allowed_units.empty() || !forFaction(w.allowed_units)) continue;
            if (id.find("claws") != std::string::npos || id.find("bash") != std::string::npos) continue;   // parti del corpo
            pool.push_back(id);
        }
        for (const auto& [id, a] : bpm.GetArmorPieces()) if (!a.allowed_units.empty() && forFaction(a.allowed_units)) pool.push_back(id);
        if (pool.empty()) return 0;
        std::sort(pool.begin(), pool.end());
        const std::string pick = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(ItemRng())];
        ItemInstance& it = CreateItem(pick, RollLootRarity(ItemRng(), minRarity));
        m_LastLootUid = it.uid;
        SUN_CORE_INFO("Bottino: {0} ({1})", it.itemId, Rarity(it.rarity).name);
        return it.uid;
    }

    std::string GameLayer::ItemLabel(const std::string& slotId, const std::string& baseName) const {
        if (slotId.find("#u") == std::string::npos) return baseName;
        const ItemRefInfo r = ParseItemRef(slotId);
        std::string s = baseName + "  [" + Rarity(r.rarity).name;
        if (r.condition < 99.5f) s += ", " + std::to_string(static_cast<int>(std::lround(r.condition))) + "%";
        return s + "]";
    }

    // ------------------------------------------------------------------------------------ garage: scelta per slot
    // Per un oggetto: la dotazione base (se e' della sua dotazione), gli esemplari del magazzino liberi o gia' suoi,
    // e "crea" (energia) se e' ricercato. Ritorna true se lo slot e' cambiato.
    bool GameLayer::DrawSlotChoices(TrainedUnit& unit, std::string& slotRef, const std::string& itemId, const std::string& name, bool isArmor) {
        const ResearchNode* node = m_ResearchTree.FindNodeByItem(itemId);
        const bool researched = !node || m_ResearchTree.IsResearched(m_Profile, *node);
        bool changed = false;
        ImGui::PushID(itemId.c_str());
        // dotazione base: sempre disponibile, comune, non si consuma
        bool isBase = false;
        if (const auto* ubp = BlueprintManager::Get().GetUnit(unit.blueprintId)) {
            isBase = ubp->default_primary == itemId || ubp->default_secondary == itemId || ubp->default_melee == itemId;
            for (const auto& e : ubp->default_equipment) isBase = isBase || e == itemId;
            for (const auto& a : ubp->default_armor) isBase = isBase || a == itemId;
        }
        if (isBase) {
            ImGui::PushStyleColor(ImGuiCol_Text, Rarity(0).color);
            if (ImGui::Selectable((name + "   [dotazione base]").c_str(), slotRef == itemId) && slotRef != itemId) { slotRef = itemId; changed = true; }
            ImGui::PopStyleColor();
        }
        // esemplari del magazzino
        const int me = static_cast<int>(&unit - m_PlayerRoster.data());
        for (const auto& it : m_Inventory) {
            if (it.itemId != itemId) continue;
            const int holder = ItemHolder(it.uid);
            const bool mine = InstanceUidOf(slotRef) == it.uid;
            if (holder >= 0 && holder != me) continue;               // addosso a un'altra unita'
            ImGui::PushID(static_cast<int>(it.uid));
            std::string label = name + "   [" + Rarity(it.rarity).name + "]";
            if (it.condition < 99.5f) label += "  " + std::to_string(static_cast<int>(std::lround(it.condition))) + "%";
            if (it.Broken()) label += "  ROTTO";
            ImGui::PushStyleColor(ImGuiCol_Text, it.Broken() ? Ui::Col::Ash : Rarity(it.rarity).color);
            if (ImGui::Selectable(label.c_str(), mine, it.Broken() ? ImGuiSelectableFlags_Disabled : 0) && !mine) { slotRef = it.Ref(); changed = true; }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && !it.traits.empty()) {
                std::string tip;
                for (const auto& t : it.traits) if (const ItemTrait* tr = FindTrait(t)) tip += std::string(tr->name) + ": " + tr->effect + "\n";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            ImGui::PopID();
        }
        // officina: crea un esemplare nuovo (comune)
        const int64_t cost = node ? node->purchaseCost : 0;
        if (!researched) {
            ImGui::Selectable((name + "   [DA RICERCARE]").c_str(), false, ImGuiSelectableFlags_Disabled);
        }
        else if (!isBase || node) {
            const std::string lab = "   + Crea " + name + " (" + std::to_string(static_cast<long long>(cost > 0 ? cost : ItemValue(itemId))) + " energia)";
            ImGui::PushStyleColor(ImGuiCol_Text, Ui::Col::Ash);
            if (ImGui::Selectable(lab.c_str(), false)) {
                const int64_t price = cost > 0 ? cost : ItemValue(itemId);
                if (m_Profile.energy >= price) {
                    m_Profile.energy -= price;
                    ItemInstance& it = CreateItem(itemId, 0);
                    slotRef = it.Ref();
                    changed = true;
                    m_ResearchMessage = "Creato e equipaggiato su " + unit.customName + ": " + name + " (comune)";
                }
                else m_ResearchMessage = "Energia insufficiente per creare " + name;
                m_ResearchMessageTimer = 4.0f;
            }
            ImGui::PopStyleColor();
        }
        (void)isArmor;
        ImGui::PopID();
        return changed;
    }

    // ------------------------------------------------------------------------------------ magazzino
    void GameLayer::RenderStorehouseUI() {
        ImGuiIO& io = ImGui::GetIO();
        auto& bpm = BlueprintManager::Get();
        ImGui::SetNextWindowPos(ImVec2(20.0f, 80.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - 40.0f, io.DisplaySize.y - 100.0f));
        ImGui::Begin("MAGAZZINO", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

        auto nameOf = [&](const std::string& id) {
            if (const auto* w = bpm.GetWeapon(id)) return w->displayName;
            if (const auto* a = bpm.GetArmorPiece(id)) return a->displayName;
            return id;
        };

        // --- elenco
        ImGui::BeginChild("##store_list", ImVec2(420.0f, 0.0f), true);
        Ui::Heading("MAGAZZINO");
        for (int f = 0; f < 5; ++f) {
            if (f) ImGui::SameLine();
            if (ImGui::RadioButton(kStoreFilters[f], m_StoreFilter == f)) m_StoreFilter = f;
        }
        ImGui::Spacing();
        std::vector<ItemInstance*> shown;
        for (auto& it : m_Inventory) if (m_StoreFilter == 0 || FilterOf(KindOfItem(it.itemId)) == m_StoreFilter) shown.push_back(&it);
        std::sort(shown.begin(), shown.end(), [](const ItemInstance* a, const ItemInstance* b) { return a->rarity != b->rarity ? a->rarity > b->rarity : a->uid < b->uid; });
        if (shown.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Ui::Col::Ash, "Vuoto. Gli oggetti si creano dal garage (Modifica ed equipaggia: \"+ Crea\" dopo la ricerca), "
                                             "si trovano come bottino alla fine delle missioni della PVE-STORY e, piu' avanti, si prendono agli avversari.");
            ImGui::PopTextWrapPos();
        }
        for (ItemInstance* it : shown) {
            ImGui::PushID(static_cast<int>(it->uid));
            const int holder = ItemHolder(it->uid);
            std::string label = nameOf(it->itemId);
            if (it->Broken()) label += "  (rotto)";
            ImGui::PushStyleColor(ImGuiCol_Text, it->Broken() ? Ui::Col::Ash : Rarity(it->rarity).color);
            if (ImGui::Selectable(label.c_str(), m_SelectedItemUid == it->uid)) m_SelectedItemUid = it->uid;
            ImGui::PopStyleColor();
            ImGui::SameLine(300.0f);
            ImGui::TextColored(Ui::Col::Ash, "%s", holder >= 0 ? m_PlayerRoster[holder].customName.c_str() : "-");
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // --- scheda
        ImGui::BeginChild("##store_card", ImVec2(0.0f, 0.0f), false);
        ItemInstance* sel = FindItem(m_SelectedItemUid);
        if (!sel) {
            Ui::Title("Rarita'");
            ImGui::Spacing();
            for (int r = 0; r < 5; ++r) {
                const RarityInfo& ri = Rarity(r);
                ImGui::TextColored(ri.color, "%-12s", ri.name);
                ImGui::SameLine(160.0f);
                ImGui::TextColored(Ui::Col::Bone, "statistiche x%.2f%s", ri.statMult, ri.traits ? (std::string("   + ") + std::to_string(ri.traits) + (ri.traits == 1 ? " tratto" : " tratti")).c_str() : "");
            }
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(Ui::Col::Ash, "Gli oggetti appena creati sono comuni. Si potenziano qui con l'energia, si trovano come bottino alla fine delle "
                                             "missioni e, piu' avanti, si prendono agli avversari. Nella 5v5 e nella sopravvivenza si consumano: ogni colpo sparato -0,03%%, "
                                             "ogni colpo in mischia a segno -0,15%%, le armature fino a -25%% secondo i colpi presi, e morire con l'oggetto addosso -34%%. "
                                             "A zero si rompe e va riparato. La dotazione base non si consuma mai.");
            ImGui::PopTextWrapPos();
            ImGui::EndChild(); ImGui::End();
            return;
        }
        const std::string name = nameOf(sel->itemId);
        const RarityInfo& ri = Rarity(sel->rarity);
        Ui::Title(name.c_str(), ri.color);
        ImGui::TextColored(ri.color, "%s", ri.name);
        ImGui::SameLine(); ImGui::TextColored(Ui::Col::Ash, "  esemplare n. %llu   (%s)", static_cast<unsigned long long>(sel->uid), sel->itemId.c_str());
        ImGui::Spacing();
        ImGui::TextColored(Ui::Col::Ash, "Condizione");
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, sel->condition > 60.0f ? Ui::Col::Verdigris : (sel->condition > 25.0f ? Ui::Col::Rust : Ui::Col::Ember));
        ImGui::ProgressBar(sel->condition / 100.0f, ImVec2(360.0f, 16.0f), sel->Broken() ? "ROTTO" : (std::to_string(static_cast<int>(std::lround(sel->condition))) + "%").c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // statistiche: base -> questo esemplare
        Ui::Heading("STATISTICHE");
        const std::string ref = sel->Ref();
        if (ImGui::BeginTable("##st", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(""); ImGui::TableSetupColumn("Base"); ImGui::TableSetupColumn("Questo");
            ImGui::TableHeadersRow();
            auto row = [](const char* lab, float a, float b, bool lowerIsBetter = false) {
                if (a == 0.0f && b == 0.0f) return;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextColored(Ui::Col::Ash, "%s", lab);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.3g", a);
                ImGui::TableSetColumnIndex(2);
                const bool better = lowerIsBetter ? b < a - 1e-4f : b > a + 1e-4f, worse = lowerIsBetter ? b > a + 1e-4f : b < a - 1e-4f;
                ImGui::TextColored(better ? Ui::Col::Verdigris : (worse ? Ui::Col::Ember : Ui::Col::Bone), "%.3g", b);
            };
            if (const auto* b = bpm.GetWeapon(sel->itemId)) {
                const auto* v = bpm.GetWeapon(ref);
                row("Danno", b->base_damage, v->base_damage);
                row("Danno d'esplosione", b->explosion_damage, v->explosion_damage);
                row("Raggio d'esplosione", b->explosion_radius, v->explosion_radius);
                row("Caricatore", static_cast<float>(b->magazine_size), static_cast<float>(v->magazine_size));
                row("Ricarica (s)", b->reload_time_sec, v->reload_time_sec, true);
                row("Dispersione", b->spread_base, v->spread_base, true);
                row("Portata (m)", b->range_effective, v->range_effective);
                row("Perforazione", static_cast<float>(b->armor_penetration), static_cast<float>(v->armor_penetration));
                row("Rinculo", b->recoil_force, v->recoil_force, true);
                row("Portata in mischia (m)", b->is_melee ? b->melee_reach : 0.0f, v->is_melee ? v->melee_reach : 0.0f);
                row("Usi", static_cast<float>(b->equipment_uses), static_cast<float>(v->equipment_uses));
                row("Cura", b->heal_amount, v->heal_amount);
            }
            else if (const auto* b2 = bpm.GetArmorPiece(sel->itemId)) {
                const auto* v = bpm.GetArmorPiece(ref);
                row("Armatura", static_cast<float>(b2->armorRating), static_cast<float>(v->armorRating));
                row("Resistenza", b2->maxDurability, v->maxDurability);
                row("Copertura", b2->coverage, v->coverage);
                row("Peso (kg)", b2->weight, v->weight, true);
            }
            ImGui::EndTable();
        }
        // tipi di danno / difese (DamageTypes.h)
        if (const auto* b = bpm.GetWeapon(sel->itemId); b && (b->base_damage > 0.0f || b->explosion_damage > 0.0f)) {
            const auto* v = bpm.GetWeapon(ref);
            ImGui::Spacing(); Ui::Heading("TIPO DI DANNO");
            DamageUi::SplitBar(v->damage, 360.0f, v->damage_pure ? "(puro: i tratti lo possono solo rinforzare)" : nullptr);
            if (v->damage.Text() != b->damage.Text()) ImGui::TextColored(Ui::Col::Ash, "Base: %s", b->damage.Text().c_str());
        }
        else if (const auto* b2 = bpm.GetArmorPiece(sel->itemId)) {
            const auto* v = bpm.GetArmorPiece(ref);
            ImGui::Spacing(); Ui::Heading("DIFESE");
            DamageUi::ResistRows(v->resist, 160.0f, &b2->resist);
            if (b2->resist_pure) ImGui::TextColored(Ui::Col::Ash, "Difese pure: i tratti possono solo rinforzare quelle che ci sono.");
        }
        if (!sel->traits.empty()) {
            ImGui::Spacing(); Ui::Heading("TRATTI");
            for (const auto& t : sel->traits) if (const ItemTrait* tr = FindTrait(t)) {
                ImGui::TextColored(tr->dmgType >= 0 ? DamageUi::Col(tr->dmgType) : ri.color, "%s", tr->name); ImGui::SameLine(200.0f); ImGui::Text("%s", tr->effect);
            }
        }

        // chi puo' usarlo
        ImGui::Spacing(); Ui::Heading("CHI PUO' USARLO");
        {
            std::string who;
            const auto* w = bpm.GetWeapon(sel->itemId);
            const auto* a = bpm.GetArmorPiece(sel->itemId);
            for (const auto& u : m_PlayerRoster) {
                const bool can = w ? bpm.CanEquip(*w, u.blueprintId) : (a && bpm.CanEquip(*a, u.blueprintId));
                if (can) who += (who.empty() ? "" : ", ") + u.customName;
            }
            const int holder = ItemHolder(sel->uid);
            ImGui::PushTextWrapPos(0.0f);
            if (holder >= 0) ImGui::TextColored(Ui::Col::Gold, "Addosso a: %s", m_PlayerRoster[holder].customName.c_str());
            ImGui::TextColored(Ui::Col::Bone, "Nel tuo garage: %s", who.empty() ? "nessuna unita' (serve un'unita' che lo possa equipaggiare)" : who.c_str());
            ImGui::PopTextWrapPos();
        }

        // azioni
        ImGui::Spacing(); Ui::Divider(); ImGui::Spacing();
        const int64_t value = ItemValue(sel->itemId);
        auto say = [&](const std::string& m) { m_StoreMessage = m; m_StoreMessageTimer = 4.0f; };
        if (sel->rarity < 4) {
            const int64_t up = UpgradeCost(value, sel->rarity);
            const std::string lab = std::string("POTENZIA A ") + Rarity(sel->rarity + 1).name + "  (" + std::to_string(static_cast<long long>(up)) + " energia)";
            if (Ui::Button(lab.c_str(), ImVec2(380.0f, 36.0f), Ui::Btn::Primary, m_Profile.energy >= up && !sel->Broken())) {
                m_Profile.energy -= up;
                SetRarity(*sel, sel->rarity + 1, ItemRng());
                RefreshItemRefs(sel->uid);
                say(name + " ora e' " + Rarity(sel->rarity).name);
            }
        }
        else ImGui::TextColored(ri.color, "Rarita' massima.");
        if (sel->condition < 100.0f) {
            const int64_t rc = RepairCost(value, *sel);
            if (Ui::Button(("RIPARA  (" + std::to_string(static_cast<long long>(rc)) + " energia)").c_str(), ImVec2(380.0f, 32.0f), Ui::Btn::Normal, m_Profile.energy >= rc)) {
                m_Profile.energy -= rc;
                sel->condition = 100.0f;
                RefreshItemRefs(sel->uid);
                say(name + " riparato");
            }
        }
        const int64_t dv = DismantleValue(value, sel->rarity);
        if (Ui::Button(("SMONTA  (+" + std::to_string(static_cast<long long>(dv)) + " energia)").c_str(), ImVec2(380.0f, 28.0f), Ui::Btn::Ghost)) {
            m_Profile.energy += dv;
            say(name + " smontato");
            RemoveItem(sel->uid);
        }
        if (m_StoreMessageTimer > 0.0f) { m_StoreMessageTimer -= io.DeltaTime; ImGui::Spacing(); ImGui::TextColored(Ui::Col::Verdigris, "%s", m_StoreMessage.c_str()); }
        ImGui::EndChild();
        ImGui::End();
    }
}
