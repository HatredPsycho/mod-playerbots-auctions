/*
 * mod-playerbots-auctions - prospecting, milling and disenchanting: taking things apart.
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * These are not recipes with a fixed product: what comes out is a matter of luck. A bot knows what comes
 * out on average - from the loot tables of the server - and decides with that, as a player does who has
 * prospected a few hundred stacks of ore.
 */

#include "Pba.h"
#include "LootMgr.h"

namespace pba
{
    namespace
    {
        enum : uint32
        {
            SPELL_DISENCHANT = 13262,
            SPELL_PROSPECTING = 31252,
            SPELL_MILLING = 51005
        };

        struct Row
        {
            uint32 item = 0;
            uint32 reference = 0;
            float chance = 0.0f;
            uint32 group = 0;
            uint32 min = 1, max = 1;
        };

        using Table = std::unordered_map<uint32, std::vector<Row>>;
        using Yields = std::unordered_map<uint32, std::vector<Yield>>;

        Yields prospecting, milling, disenchanting;

        Table LoadTable(char const* name)
        {
            Table table;
            if (QueryResult result = WorldDatabase.Query(
                "SELECT Entry, Item, Reference, Chance, GroupId, MinCount, MaxCount FROM {} WHERE QuestRequired = 0", name))
                do
                {
                    Field* fields = result->Fetch();
                    Row row;
                    row.item = fields[1].Get<uint32>();
                    int32 const reference = fields[2].Get<int32>();
                    row.reference = uint32(reference < 0 ? -reference : reference);
                    row.chance = std::clamp(fields[3].Get<float>(), 0.0f, 100.0f);
                    row.group = fields[4].Get<uint8>();
                    row.min = fields[5].Get<uint8>();
                    row.max = std::max<uint32>(row.min, fields[6].Get<uint8>());
                    table[fields[0].Get<uint32>()].push_back(row);
                } while (result->NextRow());
            return table;
        }

        /// What a loot table gives on average. Rows without a group roll each for themselves; of the rows
        /// of a group exactly one is taken, and those without a chance share what the others leave.
        void Expect(std::vector<Row> const& rows, Table const& references, double factor, uint32 depth, std::map<uint32, double>& out)
        {
            if (depth > 4 || factor <= 0.0)
                return;

            std::map<uint32, std::pair<double, uint32>> groups;     // group -> chance given away, rows without one
            for (Row const& row : rows)
                if (row.group)
                {
                    auto& group = groups[row.group];
                    if (row.chance > 0.0f)
                        group.first += row.chance;
                    else
                        ++group.second;
                }

            for (Row const& row : rows)
            {
                double chance = row.chance;
                if (row.group)
                {
                    auto const& group = groups[row.group];
                    if (row.chance <= 0.0f)
                        chance = std::max(0.0, 100.0 - group.first) / std::max<uint32>(1, group.second);
                    else if (group.first > 100.0)
                        chance = row.chance * 100.0 / group.first;
                }
                double const share = factor * chance / 100.0;
                if (share <= 0.0)
                    continue;

                if (row.reference)
                {
                    auto found = references.find(row.reference);
                    if (found != references.end())
                        Expect(found->second, references, share * row.max, depth + 1, out);      // MaxCount: how often it is rolled
                }
                else
                    out[row.item] += share * (row.min + row.max) / 2.0;
            }
        }

        void Build(char const* name, Table const& references, Yields& yields)
        {
            yields.clear();
            Table const table = LoadTable(name);
            for (auto const& entry : table)
            {
                std::map<uint32, double> out;
                Expect(entry.second, references, 1.0, 0, out);
                std::vector<Yield>& list = yields[entry.first];
                for (auto const& item : out)
                    if (item.second > 0.0 && sObjectMgr->GetItemTemplate(item.first))
                        list.push_back({ item.first, float(item.second) });
                if (list.empty())
                    yields.erase(entry.first);
            }
        }

        std::vector<Yield> const* YieldOf(Yields const& yields, uint32 id)
        {
            auto found = yields.find(id);
            return found != yields.end() ? &found->second : nullptr;
        }

        template <class Fn>
        void ForEachBagItem(Player* bot, Fn&& fn)
        {
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    fn(item);
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        if (Item* item = bag->GetItemByPos(slot))
                            fn(item);
        }

        bool CanDisenchant(Player* bot, Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || !proto->DisenchantID || proto->RequiredDisenchantSkill == uint32(-1) ||
                proto->RequiredDisenchantSkill > bot->GetSkillValue(SKILL_ENCHANTING))
                return false;
            if (proto->Quality < ITEM_QUALITY_UNCOMMON || proto->Quality > ITEM_QUALITY_EPIC ||
                (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
                return false;
            return item->GetOwnerGUID() == bot->GetGUID() && !item->IsNotEmptyBag() && !sAuctionMgr->GetAItem(item->GetGUID());
        }
    }

    void LoadYields()
    {
        Table const references = LoadTable("reference_loot_template");
        Build("prospecting_loot_template", references, prospecting);
        Build("milling_loot_template", references, milling);
        Build("disenchant_loot_template", references, disenchanting);
        LOG_INFO("server.loading", ">> PlayerbotsAuctions: the bots know what comes of prospecting {} ore(s), milling {} herb(s) and disenchanting {} kind(s) of item.",
            prospecting.size(), milling.size(), disenchanting.size());
    }

    void AddRefining(Player* bot, PlayerbotAI* botAI, std::vector<Recipe>& recipes)
    {
        if (!cfg.refine)
            return;
        bool const prospects = bot->HasSpell(SPELL_PROSPECTING);
        bool const mills = bot->HasSpell(SPELL_MILLING);
        bool const disenchants = bot->HasSpell(SPELL_DISENCHANT);
        if (!prospects && !mills && !disenchants)
            return;

        std::unordered_set<uint32> seen;
        uint32 gear = 0;
        ForEachBagItem(bot, [&](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || !seen.insert(proto->ItemId).second)
                return;

            // Never what it needs for a quest.
            ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", proto->ItemId)->Get();
            if (usage == ITEM_USAGE_QUEST)
                return;

            Recipe recipe;
            if (prospects && proto->HasFlag(ITEM_FLAG_IS_PROSPECTABLE) && proto->RequiredSkillRank <= bot->GetSkillValue(SKILL_JEWELCRAFTING))
            {
                recipe.kind = KIND_PROSPECT;
                recipe.spell = SPELL_PROSPECTING;
                recipe.yield = YieldOf(prospecting, proto->ItemId);
                recipe.reagents.emplace_back(proto->ItemId, 5);
            }
            else if (mills && proto->HasFlag(ITEM_FLAG_IS_MILLABLE) && proto->RequiredSkillRank <= bot->GetSkillValue(SKILL_INSCRIPTION))
            {
                recipe.kind = KIND_MILL;
                recipe.spell = SPELL_MILLING;
                recipe.yield = YieldOf(milling, proto->ItemId);
                recipe.reagents.emplace_back(proto->ItemId, 5);
            }
            else if (disenchants && gear < 6 && CanDisenchant(bot, item))
            {
                // Only what it would get rid of anyway - never something it wears, wants to wear or needs.
                if (usage != ITEM_USAGE_AH && usage != ITEM_USAGE_VENDOR)
                    return;
                ++gear;
                recipe.kind = KIND_DISENCHANT;
                recipe.spell = SPELL_DISENCHANT;
                recipe.yield = YieldOf(disenchanting, proto->DisenchantID);
                recipe.reagents.emplace_back(proto->ItemId, 1);
                // What it cannot put up for auction is worth what the vendor gives for it.
                if (item->IsSoulBound() || !item->CanBeTraded())
                    recipe.sourceValue = double(proto->SellPrice);
            }
            else
                return;

            if (recipe.yield)
                recipes.push_back(std::move(recipe));
        });
    }

    bool Refine(Player* bot, Recipe const& recipe)
    {
        if (recipe.kind == KIND_CRAFT || recipe.reagents.empty())
            return false;
        uint32 const entry = recipe.reagents.front().first;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
            return false;

        uint32 lootId = entry;
        LootStore const* store = recipe.kind == KIND_PROSPECT ? &LootTemplates_Prospecting : &LootTemplates_Milling;
        if (recipe.kind == KIND_DISENCHANT)
        {
            lootId = proto->DisenchantID;
            store = &LootTemplates_Disenchant;
        }
        if (!store->HaveLootFor(lootId))
            return false;

        // The dice of the server, as for a player - rolled first: if what comes out does not fit into the
        // bags, nothing is taken apart.
        Loot loot;
        loot.FillLoot(lootId, *store, bot, true, true);
        if (bot->GetFreeInventorySpace() < loot.items.size() + 1)
            return false;
        for (LootItem const& got : loot.items)
        {
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, got.itemid, got.count) != EQUIP_ERR_OK)
                return false;
        }

        if (recipe.kind == KIND_DISENCHANT)
        {
            // The one in its bags, never the one it wears.
            Item* source = nullptr;
            ForEachBagItem(bot, [&](Item* item)
            {
                if (!source && item->GetEntry() == entry && CanDisenchant(bot, item))
                    source = item;
            });
            if (!source)
                return false;
            bot->DestroyItem(source->GetBagSlot(), source->GetSlot(), true);
            bot->UpdateCraftSkill(SPELL_DISENCHANT);
        }
        else
        {
            uint32 inBags = 0;
            ForEachBagItem(bot, [&](Item* item)
            {
                if (item->GetEntry() == entry)
                    inBags += item->GetCount();
            });
            if (inBags < 5)
                return false;
            bot->DestroyItemCount(entry, 5, true);
            bool const prospect = recipe.kind == KIND_PROSPECT;
            if (sWorld->getBoolConfig(prospect ? CONFIG_SKILL_PROSPECTING : CONFIG_SKILL_MILLING))
            {
                uint32 const skill = prospect ? SKILL_JEWELCRAFTING : SKILL_INSCRIPTION;
                bot->UpdateGatherSkill(skill, bot->GetPureSkillValue(skill), proto->RequiredSkillRank);
            }
        }

        for (LootItem const& got : loot.items)
        {
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, got.itemid, got.count) == EQUIP_ERR_OK)
                bot->StoreNewItem(dest, got.itemid, true, got.randomPropertyId);
        }
        return true;
    }
}
