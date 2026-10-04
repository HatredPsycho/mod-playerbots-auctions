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

        struct Vellum
        {
            uint32 item = 0;
            uint32 level = 0;
            bool weapon = false;
        };
        std::vector<Vellum> vellums;        // those a scribe can make, the plainest first

        bool IsScrollRecipe(SpellInfo const* info)
        {
            return info && info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL) && info->Effects[EFFECT_0].Effect == SPELL_EFFECT_ENCHANT_ITEM &&
                info->Effects[EFFECT_0].ItemType && info->IsAbilityOfSkillType(SKILL_ENCHANTING) &&
                (info->EquippedItemClass == ITEM_CLASS_WEAPON || info->EquippedItemClass == ITEM_CLASS_ARMOR);
        }

        template <class Fn>
        void ForEachVellum(SpellInfo const* info, Fn&& fn)
        {
            bool const weapon = info->EquippedItemClass == ITEM_CLASS_WEAPON;
            for (Vellum const& vellum : vellums)
                if (vellum.weapon == weapon && vellum.level >= info->BaseLevel)
                    if (fn(vellum))
                        return;
        }

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

    namespace
    {
        constexpr double WorkMargin = 1.25;     // a quarter on top of the materials for the work

        /// Is this something people buy from a crafter? Plain gear made to learn the craft is not.
        bool HasBuyers(ItemTemplate const* proto)
        {
            if (proto->Quality >= MAX_ITEM_QUALITY || proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM)
                return false;
            switch (proto->Class)
            {
                case ITEM_CLASS_CONSUMABLE:
                case ITEM_CLASS_CONTAINER:
                case ITEM_CLASS_GEM:
                case ITEM_CLASS_TRADE_GOODS:
                case ITEM_CLASS_REAGENT:
                    return true;
                case ITEM_CLASS_WEAPON:
                case ITEM_CLASS_ARMOR:
                    return proto->Quality >= ITEM_QUALITY_UNCOMMON;
                case ITEM_CLASS_GLYPH:
                    return cfg.glyphs;
                default:
                    return false;
            }
        }

        double MaterialPrice(ItemTemplate const* proto)
        {
            if (proto->Quality >= MAX_ITEM_QUALITY)
                return 0.0;
            if (proto->BuyPrice && market.IsVendorSupply(proto->ItemId))
                return double(proto->BuyPrice) / std::max<uint32>(1, proto->BuyCount);
            return Market::Regular(proto);
        }

        double Factor(ItemTemplate const* proto)
        {
            return cfg.priceMultiplier[proto->Quality] > 0.0 ? cfg.priceMultiplier[proto->Quality] : 1.0;
        }

        void LoadCraftedValues()
        {
            struct Made
            {
                SpellInfo const* info;
                ItemTemplate const* product;
            };
            std::vector<Made> recipes;
            std::unordered_map<uint32, std::unordered_set<uint32>> from;      // product -> what it is made of
            for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
                if (!info || !info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL) || info->Effects[EFFECT_0].Effect != SPELL_EFFECT_CREATE_ITEM)
                    continue;
                ItemTemplate const* product = sObjectMgr->GetItemTemplate(info->Effects[EFFECT_0].ItemType);
                if (!product)
                    continue;
                bool any = false;
                for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    if (info->Reagent[i] > 0 && info->ReagentCount[i])
                    {
                        from[product->ItemId].insert(uint32(info->Reagent[i]));
                        any = true;
                    }
                if (any && HasBuyers(product))
                    recipes.push_back({ info, product });
            }

            // Bars come of ore, shirts of bolts that come of cloth: a few rounds until it has settled.
            uint32 valued = 0;
            for (uint32 round = 0; round < 3; ++round)
            {
                std::unordered_map<uint32, double> least;       // product -> the cheapest way to make one
                for (Made const& made : recipes)
                {
                    double cost = 0.0;
                    bool circle = false;
                    for (uint32 i = 0; i < MAX_SPELL_REAGENTS && !circle; ++i)
                    {
                        if (made.info->Reagent[i] <= 0 || !made.info->ReagentCount[i])
                            continue;
                        uint32 const reagent = uint32(made.info->Reagent[i]);
                        // What can be turned back into what it was made of - essences - is worth no more for it.
                        auto back = from.find(reagent);
                        if (back != from.end() && back->second.count(made.product->ItemId))
                            circle = true;
                        else if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(reagent))
                            cost += MaterialPrice(proto) * made.info->ReagentCount[i];
                    }
                    if (circle || cost <= 0.0)
                        continue;
                    double const each = cost * WorkMargin / std::max<int32>(1, made.info->Effects[EFFECT_0].BasePoints + 1);
                    auto found = least.find(made.product->ItemId);
                    if (found == least.end() || each < found->second)
                        least[made.product->ItemId] = each;
                }
                valued = 0;
                for (auto const& entry : least)
                {
                    ItemTemplate const* product = sObjectMgr->GetItemTemplate(entry.first);
                    uint32 const base = uint32(std::clamp(entry.second / Factor(product), 1.0, 2000000000.0));
                    if (base > product->SellPrice)
                    {
                        Market::SetMade(product->ItemId, base);
                        ++valued;
                    }
                    else
                        Market::SetMade(product->ItemId, 0);
                }
            }
            LOG_INFO("server.loading", ">> PlayerbotsAuctions: {} crafted thing(s) are worth more by their materials than a vendor gives.", valued);
        }
    }

    void LoadMadeValues()
    {
        Market::ClearMade();
        LoadCraftedValues();
        vellums.clear();
        std::vector<SpellInfo const*> enchants;
        std::unordered_set<uint32> seen;
        for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info || !info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL))
                continue;
            if (IsScrollRecipe(info))
                enchants.push_back(info);
            else if (info->Effects[EFFECT_0].Effect == SPELL_EFFECT_CREATE_ITEM && info->IsAbilityOfSkillType(SKILL_INSCRIPTION))
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(info->Effects[EFFECT_0].ItemType))
                    if ((proto->IsWeaponVellum() || proto->IsArmorVellum()) && seen.insert(proto->ItemId).second)
                        vellums.push_back({ proto->ItemId, proto->RequiredLevel ? proto->RequiredLevel : proto->ItemLevel, proto->IsWeaponVellum() });
        }
        std::sort(vellums.begin(), vellums.end(), [](Vellum const& a, Vellum const& b) { return a.level < b.level; });

        // What a scroll is worth: what goes into it, and a quarter on top for the work.
        uint32 scrolls = 0;
        for (SpellInfo const* info : enchants)
        {
            ItemTemplate const* scroll = sObjectMgr->GetItemTemplate(info->Effects[EFFECT_0].ItemType);
            if (!scroll || scroll->SellPrice || scroll->Quality >= MAX_ITEM_QUALITY)
                continue;
            double cost = 0.0;
            ForEachVellum(info, [&](Vellum const& vellum)
            {
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vellum.item))
                    cost = Market::Regular(proto);
                return true;
            });
            if (cost <= 0.0)
            {
                Market::SetMade(scroll->ItemId, 0);
                continue;       // no vellum for it
            }
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (info->Reagent[i] > 0 && info->ReagentCount[i])
                    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(info->Reagent[i]))
                        if (proto->Quality < MAX_ITEM_QUALITY)
                            cost += Market::Regular(proto) * info->ReagentCount[i];
            Market::SetMade(scroll->ItemId, uint32(std::clamp(cost * WorkMargin / Factor(scroll), 1.0, 2000000000.0)));
            ++scrolls;
        }
        LOG_INFO("server.loading", ">> PlayerbotsAuctions: enchanters know {} enchantment(s) for a scroll, on {} kind(s) of vellum.",
            scrolls, vellums.size());
    }

    uint32 VellumFor(Player* bot, SpellInfo const* info)
    {
        if (!IsScrollRecipe(info))
            return 0;
        uint32 plainest = 0, carried = 0;
        ForEachVellum(info, [&](Vellum const& vellum)
        {
            if (!plainest)
                plainest = vellum.item;
            if (bot->HasItemCount(vellum.item, 1))
                carried = vellum.item;
            return carried != 0;
        });
        return carried ? carried : plainest;
    }

    void AddRefining(Player* bot, PlayerbotAI* botAI, std::vector<Recipe>& recipes)
    {
        if (!cfg.refine)
            return;
        // By the skill, not only by the spell: mod-playerbots gives its bots the profession without always
        // teaching what comes with it.
        bool const prospects = bot->HasSpell(SPELL_PROSPECTING) || bot->GetSkillValue(SKILL_JEWELCRAFTING) >= 20;
        bool const mills = bot->HasSpell(SPELL_MILLING) || bot->GetSkillValue(SKILL_INSCRIPTION) >= 1;
        bool const disenchants = bot->HasSpell(SPELL_DISENCHANT) || bot->GetSkillValue(SKILL_ENCHANTING) >= 1;
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
                // To mod-playerbots, bound gear an enchanter cannot wear is "to disenchant" - which it then never does.
                if (usage != ITEM_USAGE_AH && usage != ITEM_USAGE_VENDOR && usage != ITEM_USAGE_DISENCHANT)
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

        if (recipe.kind == KIND_ENCHANT)
        {
            // As the server does it when an enchantment is cast on vellum: the vellum and the materials go,
            // the scroll comes. No skill is gained from it.
            for (auto const& reagent : recipe.reagents)
                if (bot->GetItemCount(reagent.first) < reagent.second)
                    return false;
            ItemPosCountVec dest;
            if (!sObjectMgr->GetItemTemplate(recipe.product) ||
                bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, recipe.product, 1) != EQUIP_ERR_OK)
                return false;
            for (auto const& reagent : recipe.reagents)
                bot->DestroyItemCount(reagent.first, reagent.second, true);
            bot->StoreNewItem(dest, recipe.product, true);
            return true;
        }
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
