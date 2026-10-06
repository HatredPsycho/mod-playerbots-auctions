/*
 * mod-playerbots-auctions - settings, character traits and item rules.
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Pba.h"

namespace pba
{
    Settings cfg;

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    namespace
    {
        void LoadNumbers(std::string const& text, std::unordered_set<uint32>& into)
        {
            into.clear();
            for (std::string_view part : Acore::Tokenize(text, ',', false))
            {
                while (!part.empty() && part.front() == ' ')
                    part.remove_prefix(1);
                while (!part.empty() && part.back() == ' ')
                    part.remove_suffix(1);
                if (Optional<uint32> value = Acore::StringTo<uint32>(part))
                    into.insert(*value);
            }
        }
    }

    void LoadSettings()
    {
        cfg.enabled             = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Enable", true);
        cfg.debug               = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Debug", false);
        cfg.intervalMs          = std::max<uint32>(5, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.IntervalSeconds", 30)) * IN_MILLISECONDS;
        cfg.botsPerCycle        = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.BotsPerCycle", 10));
        cfg.auctioneerRange     = sConfigMgr->GetOption<float>("PlayerbotsAuctions.AuctioneerRange", 35.0f);
        cfg.minLevel            = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.MinBotLevel", 1);
        cfg.visitCooldownMin    = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.VisitCooldownMinutesMin", 20) * MINUTE;
        cfg.visitCooldownMax    = std::max(cfg.visitCooldownMin, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.VisitCooldownMinutesMax", 60) * MINUTE);
        cfg.collectMail         = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.CollectAuctionMail", true);
        cfg.avoidersPercent     = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Character.AvoidersPercent", 15));
        cfg.rhythm              = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.DailyRhythm", true);
        cfg.saveMemory          = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.SaveMemory", true);
        cfg.pauseOffline        = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.PauseAuctionsWhileOffline", true);

        cfg.maxAuctionsPerHouse = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Sell.MaxAuctionsPerHouse", 20000);
        cfg.minQualityEquipment = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Sell.MinQuality.Equipment", ITEM_QUALITY_UNCOMMON);
        cfg.minQualityOther     = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Sell.MinQuality.Other", ITEM_QUALITY_NORMAL);
        cfg.maxQuality          = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Sell.MaxQuality", ITEM_QUALITY_LEGENDARY);
        LoadNumbers(sConfigMgr->GetOption<std::string>("PlayerbotsAuctions.Sell.ItemClasses", "0,1,2,3,4,5,7,9,11,12,13,15,16"), cfg.itemClasses);
        LoadNumbers(sConfigMgr->GetOption<std::string>("PlayerbotsAuctions.Sell.ExcludedItemIDs", ""), cfg.excludedItems);
        cfg.excludedNameParts.clear();
        std::string const names = sConfigMgr->GetOption<std::string>("PlayerbotsAuctions.Sell.ExcludedNameParts",
            "monster -,deprecated,[ph],(test), test ,qatest,qa ,zzold,npc equip,unused");
        for (std::string_view part : Acore::Tokenize(names, ',', false))
            cfg.excludedNameParts.push_back(Lower(std::string(part)));
        cfg.chargeDeposit       = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Sell.ChargeDeposit", true);
        cfg.minListValue        = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Sell.MinAuctionValueSilver", 1) * SILVER;
        cfg.keepFromVendor      = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Sell.KeepFromVendor", true);
        cfg.sellJunk            = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Sell.JunkToVendor", true);

        cfg.priceMultiplier[ITEM_QUALITY_POOR]      = sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.Poor", 1.5f);
        cfg.priceMultiplier[ITEM_QUALITY_NORMAL]    = sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.Normal", 3.0f);
        cfg.priceMultiplier[ITEM_QUALITY_UNCOMMON]  = sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.Uncommon", 6.0f);
        cfg.priceMultiplier[ITEM_QUALITY_RARE]      = sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.Rare", 12.0f);
        cfg.priceMultiplier[ITEM_QUALITY_EPIC]      = sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.Epic", 25.0f);
        cfg.priceMultiplier[ITEM_QUALITY_LEGENDARY] = sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.Legendary", 50.0f);
        cfg.priceMultiplier[ITEM_QUALITY_ARTIFACT]  = cfg.priceMultiplier[ITEM_QUALITY_LEGENDARY];
        cfg.priceMultiplier[ITEM_QUALITY_HEIRLOOM]  = cfg.priceMultiplier[ITEM_QUALITY_EPIC];
        for (float& factor : cfg.priceMultiplier)
            factor = std::max(factor, 0.1f);
        cfg.minVendorFactor     = std::max(1.0f, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.MinVendorFactor", 1.5f));
        cfg.maxBuyout           = uint32(std::min<uint64>(uint64(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.MaxBuyoutGold", 5000)) * GOLD, MAX_MONEY_AMOUNT));
        cfg.learnPrices         = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Price.LearnFromSales", true);

        cfg.buyEnabled          = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Buy.Enable", true);
        cfg.buyCandidates       = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Buy.ItemsLookedAt", 60), 1, 500);
        cfg.buyFromBots         = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Buy.FromBots", true);
        cfg.buyFromPlayers      = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Buy.FromPlayers", true);
        cfg.buyBids             = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Buy.PlaceBids", true);
        cfg.buyUseBotMoney      = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Buy.UseBotMoney", true);
        cfg.buyMinVendorFactor  = std::max(1.0f, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Buy.MinVendorFactor", 1.2f));
        cfg.buyVendorItemPercent = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Buy.VendorItemMaxPercent", 75), 1, 100);

        cfg.cityTrips           = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.CityTrip.Enable", true);
        cfg.cityCooldownMin     = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.CooldownMinutesMin", 60) * MINUTE;
        cfg.cityCooldownMax     = std::max(cfg.cityCooldownMin, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.CooldownMinutesMax", 180) * MINUTE);
        cfg.cityItemsMin        = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.ItemsMin", 2));
        cfg.cityItemsMax        = std::max(cfg.cityItemsMin, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.ItemsMax", 8));

        cfg.craftEnabled        = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Crafting.Enable", true);
        cfg.matsEnabled         = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Crafting.BuyMaterials", true);
        cfg.matsMinProfit       = std::min<uint32>(500, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Crafting.MinProfitPercent", 10));
        cfg.matsSkillBonus      = std::min<uint32>(500, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Crafting.SkillUpBonusPercent", 100));
        cfg.matsRecipes         = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Crafting.RecipesPerVisit", 40), 1, 500);
        cfg.matsVendor          = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Crafting.BuyVendorMaterials", true);
        cfg.matsMaxPrice        = std::max(0.5f, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Crafting.MaxMaterialPriceFactor", 1.5f));
        cfg.craftFocus          = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Crafting.WalkToAnvilForgeFire", true);
        cfg.craftBatch          = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Crafting.MaxInARow", 5), 1, 20);
        cfg.refine              = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Crafting.TakeApart", true);
        cfg.scrolls             = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Crafting.Scrolls", true);
        cfg.glyphs              = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Crafting.Glyphs", false);

        cfg.deals               = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Chat.Enable", true);
        cfg.dealAnswers         = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Chat.Answers", 3), 1, 10);
        cfg.dealMailDelay       = sConfigMgr->GetOption<int32>("PlayerbotsAuctions.Chat.MailDelay", -1);
        cfg.chatter             = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Chatter.Enable", true);
        cfg.chatterInterval     = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Chatter.Interval", 150), 20, 3600);
        cfg.chatterReplies      = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Chatter.Replies", true);
        cfg.chatterSay          = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Chatter.Nearby", true);
        cfg.chatterWorld        = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Chatter.WorldChannel", true);
        cfg.dealChannels.clear();
        std::string const channels = sConfigMgr->GetOption<std::string>("PlayerbotsAuctions.Chat.Channels", "");
        for (std::string_view part : Acore::Tokenize(channels, ',', false))
        {
            std::string name = Lower(std::string(part));
            name.erase(0, name.find_first_not_of(' '));
            name.erase(name.find_last_not_of(' ') + 1);
            if (!name.empty())
                cfg.dealChannels.push_back(name);
        }
    }

    // ------------------------------------------------------------------------------------------ character

    float TraitOf(Player* bot, Trait which)
    {
        uint32 hash = bot->GetGUID().GetCounter() * 2654435761u + uint32(which) * 40503u;
        hash ^= hash >> 15;
        hash *= 2246822519u;
        hash ^= hash >> 13;
        hash *= 3266489917u;
        hash ^= hash >> 16;
        return float(hash & 0xFFFF) / 65535.0f;
    }

    bool AvoidsAuctionHouse(Player* bot)
    {
        return TraitOf(bot, TRAIT_TRADING) * 100.0f < float(cfg.avoidersPercent);
    }

    float ActivityNow(time_t now)
    {
        if (!cfg.rhythm)
            return 1.0f;

        std::tm const time = Acore::Time::TimeBreakdown(now);
        float activity;
        if (time.tm_hour < 6)
            activity = 0.25f;
        else if (time.tm_hour < 12)
            activity = 0.55f;
        else if (time.tm_hour < 17)
            activity = 0.75f;
        else if (time.tm_hour < 23)
            activity = 1.0f;
        else
            activity = 0.6f;

        if (time.tm_wday == 0 || time.tm_wday == 6)     // the weekend
            activity += 0.2f;
        return std::min(activity, 1.0f);
    }

    // ------------------------------------------------------------------------------------------ items

    bool IsEquipment(ItemTemplate const* proto)
    {
        return proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR;
    }

    /// What the item is and whether this kind of item may be sold at all.
    bool IsAllowedKind(ItemTemplate const* proto)
    {
        if (cfg.itemClasses.find(proto->Class) == cfg.itemClasses.end())
            return false;
        if (proto->Quality > cfg.maxQuality || proto->Quality >= MAX_ITEM_QUALITY)
            return false;
        if (proto->Quality < (IsEquipment(proto) ? cfg.minQualityEquipment : cfg.minQualityOther))
            return false;
        if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1)
            return false;
        if (proto->HasFlag(ITEM_FLAG_CONJURED) || !Market::Base(proto))
            return false;
        if (cfg.excludedItems.find(proto->ItemId) != cfg.excludedItems.end())
            return false;

        std::string const name = Lower(proto->Name1);
        for (std::string const& part : cfg.excludedNameParts)
            if (!part.empty() && name.find(part) != std::string::npos)
                return false;

        return true;
    }

    /// The same checks the server makes when a player puts an item up for auction.
    bool IsSellable(Player* bot, Item* item)
    {
        if (!item || !item->GetTemplate() || !IsAllowedKind(item->GetTemplate()))
            return false;
        if (item->IsSoulBound() || !item->CanBeTraded() || item->IsNotEmptyBag())
            return false;
        if (item->GetUInt32Value(ITEM_FIELD_DURATION) || sAuctionMgr->GetAItem(item->GetGUID()))
            return false;
        return item->GetOwnerGUID() == bot->GetGUID();
    }

    namespace
    {
        // Potion -> the recipes that make it.
        std::unordered_map<uint32, std::vector<uint32>> potionRecipes;
        // Material -> the recipes of a craft, and the spells of a class, it goes into. Not among them are
        // smelting - ore is there to be smelted or sold - and the sidelines everybody has: nobody keeps
        // all the cloth and meat it finds for bandages and cooking.
        std::unordered_map<uint32, std::vector<uint32>> craftUses;
    }

    void LoadPotionRecipes()
    {
        potionRecipes.clear();
        craftUses.clear();
        for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info)
                continue;
            if (!info->IsAbilityOfSkillType(SKILL_MINING) && !info->IsAbilityOfSkillType(SKILL_FIRST_AID) &&
                !info->IsAbilityOfSkillType(SKILL_COOKING) && !info->IsAbilityOfSkillType(SKILL_FISHING))
                for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    if (info->Reagent[i] > 0 && info->ReagentCount[i])
                        craftUses[uint32(info->Reagent[i])].push_back(id);
            if (!info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL) || info->Effects[EFFECT_0].Effect != SPELL_EFFECT_CREATE_ITEM)
                continue;
            ItemTemplate const* product = sObjectMgr->GetItemTemplate(info->Effects[EFFECT_0].ItemType);
            if (product && product->Class == ITEM_CLASS_CONSUMABLE && product->SubClass == ITEM_SUBCLASS_POTION)
                potionRecipes[product->ItemId].push_back(id);
        }
    }

    bool IsHandout(Player* bot, ItemTemplate const* proto)
    {
        if (proto->Class == ITEM_CLASS_PROJECTILE)
            return true;
        if (proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_POTION)
            return false;
        // A potion is the bot's own to sell only if it can make it; any other was handed to it.
        auto found = potionRecipes.find(proto->ItemId);
        if (found != potionRecipes.end())
            for (uint32 const spell : found->second)
                if (bot->HasSpell(spell))
                    return false;
        return true;
    }

    bool UsesInCraft(Player* bot, uint32 itemId)
    {
        auto found = craftUses.find(itemId);
        if (found != craftUses.end())
            for (uint32 const spell : found->second)
                if (bot->HasSpell(spell))
                    return true;
        return false;
    }

    uint32 HumanPrice(double copper)
    {
        if (copper < 100.0)
            return uint32(std::max(1.0, std::round(copper)));
        // Two digits that matter, the second one a 0 or a 5: 1g 45s, 23g 50s, 370g.
        double const step = std::pow(10.0, std::floor(std::log10(copper)) - 1.0) * 5.0;
        double const rounded = std::round(copper / step) * step;
        return uint32(std::min(std::max(rounded, 1.0), double(MAX_MONEY_AMOUNT)));
    }
}
