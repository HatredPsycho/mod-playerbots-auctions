/*
 * mod-playerbots-auctions - lets the random bots of mod-playerbots put their loot up for auction.
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * The bots already decide for every item whether they need it. What they do not need and may trade
 * they sell to a vendor. This module steps in before that: a bot that is in a capital city or near
 * an auctioneer puts those items up for auction in its own name, like a player would. It also looks
 * through the offers and bids on or buys what it can use, and a bot whose bags fill up far from a city
 * takes its hearth to an auction house.
 *
 * Nothing in mod-playerbots is changed; the module only uses what it offers.
 */

#include "AiObjectContext.h"
#include "AuctionHouseMgr.h"
#include "AuctionHouseSearcher.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "Config.h"
#include "Containers.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "TravelMgr.h"
#include "World.h"

// The CoA core counts mail through its own manager; a stock AzerothCore does it in the character cache.
#if __has_include("MailMgr.h")
#include "MailMgr.h"
#define PBA_MAIL_DELETED(guid) sMailMgr->OnMailDeleted((guid).GetCounter())
#else
#define PBA_MAIL_DELETED(guid) sCharacterCache->DecreaseCharacterMailCount(guid)
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    struct Settings
    {
        bool   enabled = false;
        bool   debug = false;
        uint32 intervalMs = 30000;
        uint32 botsPerCycle = 10;
        float  auctioneerRange = 35.0f;
        uint32 visitCooldownMin = 20 * 60;     // seconds
        uint32 visitCooldownMax = 60 * 60;
        uint32 itemsPerVisit = 4;
        uint32 maxAuctionsPerBot = 12;
        uint32 maxAuctionsPerHouse = 20000;
        uint32 minLevel = 1;
        uint32 minQualityEquipment = ITEM_QUALITY_UNCOMMON;
        uint32 minQualityOther = ITEM_QUALITY_NORMAL;
        uint32 maxQuality = ITEM_QUALITY_EPIC;
        std::unordered_set<uint32> itemClasses;
        std::unordered_set<uint32> excludedItems;
        std::vector<std::string> excludedNameParts;
        float  priceMultiplier[MAX_ITEM_QUALITY] = { };
        uint32 priceVariation = 20;            // percent up and down
        uint32 bidPercent = 70;
        uint32 undercutPercent = 5;
        uint32 maxUndercutPercent = 25;
        float  minVendorFactor = 1.5f;
        uint32 maxBuyout = 5000 * GOLD;          // 0 = no limit
        bool   chargeDeposit = false;
        bool   collectMail = true;
        bool   learnPrices = true;
        uint32 bargainChance = 10;             // percent of the auctions that are clearly cheap
        uint32 overpricedChance = 10;          // ... and clearly expensive
        float  overpricedMax = 3.0f;
        float  aloneMin = 1.15f;               // price factor when nobody else offers the item
        float  aloneMax = 1.6f;
        uint32 temperPercent = 15;             // how far a bot's own price level is from the average
        bool   craftEnabled = true;
        bool   matsEnabled = true;
        uint32 matsMinProfit = 10;             // percent the product has to be worth more than its materials
        uint32 matsSkillBonus = 30;            // percent extra a recipe is worth to the bot while it gives skill
        uint32 matsRecipes = 40;               // recipes a bot thinks through per visit
        bool   matsVendor = true;
        float  matsMaxPrice = 1.5f;            // a material costing more than this times its usual price is left alone

        bool   buyEnabled = true;
        uint32 buyChance = 60;                 // percent of the visits in which a bot looks at the offers
        uint32 buyCandidates = 40;
        uint32 buyMaxPerVisit = 2;
        bool   buyFromBots = true;
        bool   buyFromPlayers = true;
        bool   buyBids = true;
        bool   buyUseBotMoney = true;
        uint32 buyMoneyShare = 50;             // percent of its money a bot spends on one auction at most
        uint32 buyImpulseChance = 5;
        float  buyImpulseMax = 3.0f;
        uint32 buySpeculateChance = 15;
        float  buyMinVendorFactor = 1.2f;
        uint32 buyVendorItemPercent = 75;

        bool   cityTrips = true;
        uint32 cityBagMin = 50;                // every bot has its own idea of "full" between these two
        uint32 cityBagMax = 95;
        uint32 cityMinItems = 3;
        uint32 goodsMin = 6;                   // every bot has its own idea of "enough to sell" between these two
        uint32 goodsMax = 20;
        uint32 inTownChance = 50;              // percent: a bot in town with something to sell bothers to go
        uint32 cityCooldownMin = 60 * 60;      // seconds
        uint32 cityCooldownMax = 180 * 60;
    };

    Settings cfg;

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

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

    void LoadSettings()
    {
        cfg.enabled             = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Enable", false);
        cfg.debug               = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Debug", false);
        cfg.intervalMs          = std::max<uint32>(5, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.IntervalSeconds", 30)) * IN_MILLISECONDS;
        cfg.botsPerCycle        = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.BotsPerCycle", 10));
        cfg.auctioneerRange     = sConfigMgr->GetOption<float>("PlayerbotsAuctions.AuctioneerRange", 35.0f);
        cfg.visitCooldownMin    = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.VisitCooldownMinutesMin", 20) * MINUTE;
        cfg.visitCooldownMax    = std::max(cfg.visitCooldownMin, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.VisitCooldownMinutesMax", 60) * MINUTE);
        cfg.itemsPerVisit       = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.ItemsPerVisit", 4));
        cfg.maxAuctionsPerBot   = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.MaxAuctionsPerBot", 12);
        cfg.maxAuctionsPerHouse = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.MaxAuctionsPerHouse", 20000);
        cfg.minLevel            = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.MinBotLevel", 1);
        cfg.minQualityEquipment = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.MinQuality.Equipment", ITEM_QUALITY_UNCOMMON);
        cfg.minQualityOther     = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.MinQuality.Other", ITEM_QUALITY_NORMAL);
        cfg.maxQuality          = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.MaxQuality", ITEM_QUALITY_EPIC);
        LoadNumbers(sConfigMgr->GetOption<std::string>("PlayerbotsAuctions.ItemClasses", "0,1,2,3,4,5,7,9,15,16"), cfg.itemClasses);
        LoadNumbers(sConfigMgr->GetOption<std::string>("PlayerbotsAuctions.ExcludedItemIDs", ""), cfg.excludedItems);

        cfg.excludedNameParts.clear();
        std::string const names = sConfigMgr->GetOption<std::string>("PlayerbotsAuctions.ExcludedNameParts",
            "monster -,deprecated,[ph],(test), test ,qatest,qa ,zzold,npc equip,unused");
        for (std::string_view part : Acore::Tokenize(names, ',', false))
            cfg.excludedNameParts.push_back(Lower(std::string(part)));

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
        cfg.priceVariation      = std::min<uint32>(90, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.VariationPercent", 20));
        cfg.bidPercent          = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.BidPercent", 70), 1, 100);
        cfg.undercutPercent     = std::min<uint32>(50, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.UndercutPercent", 5));
        cfg.minVendorFactor     = std::max(1.0f, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.MinVendorFactor", 1.5f));
        cfg.maxUndercutPercent  = std::min<uint32>(90, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.MaxUndercutPercent", 25));
        cfg.maxBuyout           = uint32(std::min<uint64>(uint64(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.MaxBuyoutGold", 5000)) * GOLD, MAX_MONEY_AMOUNT));
        cfg.chargeDeposit       = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.ChargeDeposit", false);
        cfg.collectMail         = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.CollectAuctionMail", true);
        cfg.learnPrices         = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Price.LearnFromSales", true);
        cfg.bargainChance       = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.BargainChance", 10));
        cfg.overpricedChance    = std::min<uint32>(100 - cfg.bargainChance, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.OverpricedChance", 10));
        cfg.overpricedMax       = std::max(1.5f, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.OverpricedMaxFactor", 3.0f));

        cfg.aloneMin            = std::max(1.0f, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.NoCompetitionFactorMin", 1.15f));
        cfg.aloneMax            = std::max(cfg.aloneMin, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.NoCompetitionFactorMax", 1.6f));
        cfg.temperPercent       = std::min<uint32>(50, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.SellerTemperPercent", 15));
        cfg.craftEnabled        = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Crafting.Enable", true);
        cfg.matsEnabled         = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Crafting.BuyMaterials", true);
        cfg.matsMinProfit       = std::min<uint32>(500, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Crafting.MinProfitPercent", 10));
        cfg.matsSkillBonus      = std::min<uint32>(500, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Crafting.SkillUpBonusPercent", 30));
        cfg.matsRecipes         = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Crafting.RecipesPerVisit", 40), 1, 500);
        cfg.matsMaxPrice        = std::max(0.5f, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Crafting.MaxMaterialPriceFactor", 1.5f));
        cfg.matsVendor          = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Crafting.BuyVendorMaterials", true);

        cfg.buyEnabled          = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Buy.Enable", true);
        cfg.buyChance           = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Buy.ChancePerVisit", 60));
        cfg.buyCandidates       = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Buy.OffersPerVisit", 40), 1, 500);
        cfg.buyMaxPerVisit      = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Buy.MaxPerVisit", 2));
        cfg.buyFromBots         = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Buy.FromBots", true);
        cfg.buyFromPlayers      = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Buy.FromPlayers", true);
        cfg.buyBids             = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Buy.PlaceBids", true);
        cfg.buyUseBotMoney      = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.Buy.UseBotMoney", true);
        cfg.buyMoneyShare       = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Buy.MaxMoneySharePercent", 50), 1, 100);
        cfg.buyImpulseChance    = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Buy.ImpulseChance", 5));
        cfg.buyImpulseMax       = std::max(1.5f, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Buy.ImpulseMaxFactor", 3.0f));
        cfg.buySpeculateChance  = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Buy.SpeculateChance", 15));
        cfg.buyMinVendorFactor  = std::max(1.0f, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Buy.MinVendorFactor", 1.2f));
        cfg.buyVendorItemPercent = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Buy.VendorItemMaxPercent", 75), 1, 100);

        cfg.cityTrips           = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.CityTrip.Enable", true);
        cfg.cityBagMin          = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.BagsFullPercentMin", 50), 10, 100);
        cfg.cityBagMax          = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.BagsFullPercentMax", 95), cfg.cityBagMin, 100);
        cfg.cityMinItems        = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.MinItemsToSell", 3));
        cfg.goodsMin            = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.EnoughItemsMin", 6));
        cfg.goodsMax            = std::max(cfg.goodsMin, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.EnoughItemsMax", 20));
        cfg.inTownChance        = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.InTownChance", 50));
        cfg.cityCooldownMin     = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.CooldownMinutesMin", 60) * MINUTE;
        cfg.cityCooldownMax     = std::max(cfg.cityCooldownMin, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.CooldownMinutesMax", 180) * MINUTE);
    }

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
        if (proto->HasFlag(ITEM_FLAG_CONJURED) || !proto->SellPrice)
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

    Creature* FindAuctioneer(Player* bot, PlayerbotAI* botAI)
    {
        GuidVector const npcs = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const guid : npcs)
        {
            Creature* creature = ObjectAccessor::GetCreature(*bot, guid);
            if (creature && creature->IsAlive() && creature->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER) &&
                bot->GetDistance(creature) <= cfg.auctioneerRange && !creature->IsHostileTo(bot))
                return creature;
        }
        return nullptr;
    }

    /// The auction house the bot does its business in, or false if it is not at an auctioneer.
    bool FindHouse(Player* bot, PlayerbotAI* botAI, AuctionHouseId& houseId)
    {
        Creature* auctioneer = FindAuctioneer(bot, botAI);
        if (!auctioneer)
            return false;

        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
        {
            houseId = AuctionHouseId::Neutral;
            return true;
        }

        if (AuctionHouseEntry const* entry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction()))
            {
                houseId = AuctionHouseId(entry->houseId);
                return true;
            }

        houseId = bot->GetTeamId() == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde;
        return true;
    }

    /// A number from 0 to 1 that belongs to one bot and never changes: its character. One bot finds its
    /// bags full when they are half full, another only when nothing fits any more; one always asks a
    /// little more than the others, another a little less.
    float Trait(Player* bot, uint32 which)
    {
        uint32 hash = bot->GetGUID().GetCounter() * 2654435761u + which * 40503u;
        hash ^= hash >> 15;
        hash *= 2246822519u;
        hash ^= hash >> 13;
        return float(hash & 0xFFFF) / 65535.0f;
    }

    enum Traits { TRAIT_BAGS = 1, TRAIT_PRICE = 2, TRAIT_GOODS = 3 };

    /// What an item is worth. The starting point is the vendor value times the factor of its quality;
    /// what auctions really sold for moves the value, within limits, so one odd sale cannot bend the market.
    class Market
    {
    public:
        static double Regular(ItemTemplate const* proto)
        {
            return double(proto->SellPrice) * cfg.priceMultiplier[proto->Quality];
        }

        double Value(ItemTemplate const* proto) const
        {
            double const regular = Regular(proto);
            if (!cfg.learnPrices)
                return regular;
            auto found = _sold.find(proto->ItemId);
            if (found == _sold.end() || found->second.sales < 3)
                return regular;         // a price is only "known" after a few sales
            return (regular + found->second.each) / 2.0;
        }

        void RecordSale(uint32 itemId, uint32 price, uint32 count)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto || !price || !count || !proto->SellPrice || proto->Quality >= MAX_ITEM_QUALITY)
                return;
            // Every sale is cut down to between half and three times the calculated price before it counts,
            // so a single absurd sale cannot move the market.
            double const regular = Regular(proto);
            double const each = std::clamp(double(price) / count, regular * 0.5, regular * 3.0);
            Learned& learned = _sold[itemId];
            learned.each = learned.sales ? learned.each * 0.8 + each * 0.2 : each;      // newer sales count more
            ++learned.sales;
        }

        void LoadVendorItems()
        {
            _vendorItems.clear();
            if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor WHERE item > 0"))
                do
                    _vendorItems.insert(result->Fetch()[0].Get<uint32>());
                while (result->NextRow());

            _vendorSupplies.clear();
            if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT item FROM npc_vendor WHERE item > 0 AND maxcount = 0 AND ExtendedCost = 0"))
                do
                    _vendorSupplies.insert(result->Fetch()[0].Get<uint32>());
                while (result->NextRow());
        }

        bool IsVendorItem(uint32 itemId) const { return _vendorItems.find(itemId) != _vendorItems.end(); }

        /// Sold by a vendor for money in any quantity: thread, vials, flux and the like.
        bool IsVendorSupply(uint32 itemId) const { return _vendorSupplies.find(itemId) != _vendorSupplies.end(); }

    private:
        struct Learned
        {
            double each = 0.0;      // price of one piece
            uint32 sales = 0;
        };
        std::unordered_map<uint32, Learned> _sold;      // item -> what it really sold for
        std::unordered_set<uint32> _vendorItems;
        std::unordered_set<uint32> _vendorSupplies;
    };

    Market market;

    struct City
    {
        TeamId team;
        uint32 mapId;
        float x, y, z;              // the auctioneers
        float ax, ay, az;           // where a bot arrives by hearth: bank or inn, a walk away
    };

    // The eight old capitals.
    City const Cities[] =
    {
        { TEAM_ALLIANCE, 0,   -8820.1f,    662.0f,   97.2f,  -8935.3f,    613.2f,   99.6f },    // Stormwind
        { TEAM_ALLIANCE, 0,   -4959.7f,   -907.9f,  505.2f,  -4840.7f,   -857.1f,  502.0f },    // Ironforge
        { TEAM_ALLIANCE, 1,    9864.5f,   2341.7f, 1326.8f,   9942.0f,   2519.7f, 1317.7f },    // Darnassus
        { TEAM_ALLIANCE, 530, -4025.5f, -11736.0f, -151.8f,  -3919.0f, -11544.7f, -150.1f },    // Exodar
        { TEAM_HORDE,    1,    1683.6f,  -4461.3f,   20.4f,   1627.5f,  -4375.7f,   12.1f },    // Orgrimmar
        { TEAM_HORDE,    1,   -1204.7f,    102.8f,  134.7f,  -1300.3f,     38.5f,  129.3f },    // Thunder Bluff
        { TEAM_HORDE,    0,    1612.3f,    199.8f,  -56.8f,   1635.4f,    223.3f,  -43.0f },    // Undercity
        { TEAM_HORDE,    530,  9648.4f,  -7135.7f,   16.9f,   9565.7f,  -7222.8f,   16.4f },    // Silvermoon
    };

    constexpr float CityRadius = 1500.0f;       // closer than this to its auction house a bot is "in town"

    // The CoA Playerbots fork lets bots visit a city and return afterwards. Other versions of
    // mod-playerbots do not have that; there the bots are not sent anywhere and only do their
    // business when they happen to stand at an auctioneer.
    template <typename AI>
    concept HasCityLife = requires(AI& ai, WorldPosition pos) { ai.rpgInfo.ChangeToGoCity(pos); ai.rpgInfo.cityReturnPos = pos; ai.rpgInfo.cityStayMs = 0u; };

    enum Journey { JOURNEY_NONE, JOURNEY_WALK, JOURNEY_HEARTH };

    /// Sends a bot to the auctioneers. A bot that is in town walks there. A bot out in the world takes its
    /// hearth to a capital of its faction, arrives at the bank or inn and walks the rest; when its visit is
    /// over, mod-playerbots brings it back to where it was.
    template <typename AI>
    Journey SendToAuctionHouse(Player* bot, AI* botAI)
    {
        if constexpr (HasCityLife<AI>)
        {
            if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
                return JOURNEY_NONE;
            auto& info = botAI->rpgInfo;

            // Capitals of the bot's faction on the same map. A trip to another map would make mod-playerbots
            // forget the visit and the way back, so bots in Northrend stay where they are.
            std::vector<City const*> reachable;
            City const* here = nullptr;
            for (City const& city : Cities)
            {
                if (city.team != bot->GetTeamId() || city.mapId != bot->GetMapId())
                    continue;
                reachable.push_back(&city);
                if (bot->GetExactDist2d(city.x, city.y) < CityRadius)
                    here = &city;
            }
            if (reachable.empty())
                return JOURNEY_NONE;

            City const* city = here ? here : reachable[urand(0, uint32(reachable.size()) - 1)];
            float const angle = frand(0.0f, 6.2831853f);
            float const radius = frand(2.0f, 6.0f);
            WorldPosition const auctioneers(city->mapId, city->x + radius * std::cos(angle), city->y + radius * std::sin(angle), city->z);

            if (!here)
            {
                if (info.cityStayMs || info.cityReturnPos != WorldPosition())
                    return JOURNEY_NONE;        // mod-playerbots is already taking it to a city
                info.cityReturnPos = WorldPosition(bot);
                if (!bot->TeleportTo(city->mapId, city->ax + frand(-3.0f, 3.0f), city->ay + frand(-3.0f, 3.0f), city->az, bot->GetOrientation()))
                {
                    info.cityReturnPos = WorldPosition();
                    return JOURNEY_NONE;
                }
            }
            info.ChangeToGoCity(auctioneers);
            return here ? JOURNEY_WALK : JOURNEY_HEARTH;
        }
        else
        {
            return JOURNEY_NONE;
        }
    }

    bool IsInTown(Player* bot)
    {
        for (City const& city : Cities)
            if (city.team == bot->GetTeamId() && city.mapId == bot->GetMapId() && bot->GetExactDist2d(city.x, city.y) < CityRadius)
                return true;
        return false;
    }

    /// One thing a bot can make with a profession.
    struct Recipe
    {
        uint32 spell = 0;
        uint32 product = 0;
        uint32 made = 1;                                        // pieces per cast
        std::vector<std::pair<uint32, uint32>> reagents;        // item, count
    };

    /// What this bot can craft where it stands: recipes it knows that turn materials into an item and need
    /// neither a forge, an anvil or a fire nor a tool the bot does not carry.
    std::vector<Recipe> Recipes(Player* bot)
    {
        std::vector<Recipe> recipes;
        for (auto const& known : bot->GetSpellMap())
        {
            if (!known.second || known.second->State == PLAYERSPELL_REMOVED || !known.second->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(known.first);
            if (!info || !info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL) || info->Effects[EFFECT_0].Effect != SPELL_EFFECT_CREATE_ITEM ||
                !info->Effects[EFFECT_0].ItemType || info->RequiresSpellFocus || bot->HasSpellCooldown(info->Id))
                continue;

            bool tools = true;
            for (uint32 i = 0; i < 2; ++i)
            {
                if (info->Totem[i] && !bot->HasItemCount(info->Totem[i], 1))
                    tools = false;
                if (info->TotemCategory[i] && !bot->HasItemTotemCategory(info->TotemCategory[i]))
                    tools = false;
            }
            if (!tools)
                continue;

            Recipe recipe;
            recipe.spell = info->Id;
            recipe.product = info->Effects[EFFECT_0].ItemType;
            recipe.made = uint32(std::max<int32>(1, info->Effects[EFFECT_0].BasePoints + 1));
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (info->Reagent[i] > 0 && info->ReagentCount[i])
                    recipe.reagents.emplace_back(uint32(info->Reagent[i]), info->ReagentCount[i]);
            if (!recipe.reagents.empty())
                recipes.push_back(std::move(recipe));
        }
        return recipes;
    }

    class AuctionSeller
    {
    public:
        void Update(uint32 diff)
        {
            if (!cfg.enabled)
                return;

            // Bots on their way to the auctioneers are watched closely: they do their business the moment
            // they stand there, before they wander off again or sell their load to a vendor.
            _arrivalTimer += diff;
            if (_arrivalTimer >= 2 * IN_MILLISECONDS)
            {
                _arrivalTimer = 0;
                time_t const now = GameTime::GetGameTime().count();
                for (auto arrival = _arrivals.begin(); arrival != _arrivals.end();)
                {
                    Player* bot = ObjectAccessor::FindConnectedPlayer(arrival->first);
                    bool done = !bot || arrival->second.until < now;
                    if (!done && arrival->second.from <= now && bot->IsInWorld() && !bot->IsBeingTeleported())
                    {
                        bool const sellOnly = arrival->second.sellOnly;
                        arrival->second.renewed = false;
                        _nextVisit.erase(arrival->first.GetCounter());
                        done = Visit(bot, now, sellOnly) && !arrival->second.renewed;     // unless the visit asked for another look
                    }
                    arrival = done ? _arrivals.erase(arrival) : std::next(arrival);
                }
            }

            _timer += diff;
            if (_timer < cfg.intervalMs)
                return;
            _timer = 0;

            // The list of bots is walked a part at a time, so a large bot population costs nothing noticeable.
            PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
            if (bots.empty())
                return;

            time_t const now = GameTime::GetGameTime().count();

            _decisions = 0;
            uint32 visits = 0, looked = 0;
            auto itr = bots.upper_bound(_last);
            uint32 const lookLimit = std::min<uint32>(uint32(bots.size()), cfg.botsPerCycle * 25);
            while (looked < lookLimit && visits < cfg.botsPerCycle)
            {
                if (itr == bots.end())
                    itr = bots.begin();
                _last = itr->first;
                Player* bot = itr->second;
                ++itr;
                ++looked;

                if (Visit(bot, now))
                    ++visits;
            }
        }

    private:
        bool Visit(Player* bot, time_t now, bool sellOnly = false)
        {
            _now = now;
            if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld() || !bot->GetSession())
                return false;

            auto next = _nextVisit.find(bot->GetGUID().GetCounter());
            if (next != _nextVisit.end() && next->second > now)
                return false;

            if (bot->GetLevel() < cfg.minLevel || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() ||
                bot->IsInFlight() || bot->GetTradeData() || bot->InBattleground() || bot->GetMap()->Instanceable())
                return false;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || !botAI->GetAiObjectContext())
                return false;

            // Bots that travel with a real player keep their loot; that player decides what happens to it.
            if (botAI->GetMaster() && IsRealPlayer(botAI->GetMaster()))
                return false;

            AuctionHouseId houseId;
            if (!FindHouse(bot, botAI, houseId))
            {
                // Not at an auctioneer: look again in a while, not at every pass.
                _nextVisit[bot->GetGUID().GetCounter()] = now + urand(2 * MINUTE, 4 * MINUTE);
                // A bot that is not already on its way decides whether it is time to go.
                if (_arrivals.find(bot->GetGUID()) == _arrivals.end() && _decisions < 20 && DecideToGo(bot, botAI, now))
                    _arrivals[bot->GetGUID()] = { now, now + 8 * MINUTE, false, true };      // watched until it stands at the auctioneer
                return false;
            }

            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
            AuctionHouseEntry const* houseEntry = AuctionHouseMgr::GetAuctionHouseEntryFromHouse(houseId);
            if (!house || !houseEntry)
                return false;

            // From here on the bot "was at the auction house", whether it had something to sell or not.
            _nextVisit[bot->GetGUID().GetCounter()] = now + urand(cfg.visitCooldownMin, cfg.visitCooldownMax);

            if (cfg.collectMail)
                CollectMail(bot, now);

            if (!sellOnly)
            {
                // What the bot could make, and the materials that go into it.
                std::vector<Recipe> recipes;
                std::unordered_set<uint32> materials;
                if (cfg.craftEnabled)
                {
                    recipes = Recipes(bot);
                    for (Recipe const& recipe : recipes)
                        for (auto const& reagent : recipe.reagents)
                            materials.insert(reagent.first);
                }

                // A crafter works out which of its recipes pays, buys what is missing and makes it. The result
                // is sold at a second look, once the crafting is done.
                if (PlanCraft(bot, botAI, house, recipes) && cfg.collectMail)
                    CollectMail(bot, now);          // the materials it bought arrive by mail at once
                if (Craft(bot, botAI, now))
                    _arrivals[bot->GetGUID()] = { now + 20, now + 3 * MINUTE, true, true };

                if (Buy(bot, botAI, house, materials) && cfg.collectMail)
                    CollectMail(bot, now);          // what it bought outright arrives by mail at once
            }

            if (house->Getcount() >= cfg.maxAuctionsPerHouse)
                return true;

            std::vector<Item*> items;
            Collect(bot, botAI, items);
            if (items.empty())
                return true;

            // One look at the auction house: how much the bot already offers, and the cheapest offer of every item.
            uint32 own = 0;
            std::unordered_map<uint32, uint32> cheapest;
            for (auto const& entry : house->GetAuctions())
            {
                AuctionEntry const* auction = entry.second;
                if (!auction)
                    continue;
                if (auction->owner == bot->GetGUID())
                    ++own;
                if (auction->buyout && auction->itemCount)
                {
                    uint32 const each = auction->buyout / auction->itemCount;
                    auto found = cheapest.find(auction->item_template);
                    if (found == cheapest.end() || each < found->second)
                        cheapest[auction->item_template] = each;
                }
            }

            Acore::Containers::RandomShuffle(items);
            uint32 posted = 0;
            for (Item* item : items)
            {
                if (posted >= cfg.itemsPerVisit || own + posted >= cfg.maxAuctionsPerBot)
                    break;
                if (Post(bot, item, house, houseEntry, houseId, cheapest))
                    ++posted;
            }

            if (cfg.debug && posted)
                LOG_INFO("module", "PlayerbotsAuctions: {} put {} item(s) up for auction.", bot->GetName(), posted);
            return true;
        }

        /// Thread, vials, flux: what a trade vendor sells the bot gets from the vendor - there is one near
        /// every auction house - and pays the vendor's price.
        bool BuyFromVendor(Player* bot, Recipe const& recipe)
        {
            for (auto const& reagent : recipe.reagents)
            {
                uint32 const have = bot->GetItemCount(reagent.first);
                if (have >= reagent.second)
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(reagent.first);
                if (!proto || proto->BuyPrice <= 0 || !market.IsVendorSupply(reagent.first))
                    return false;

                uint32 const need = reagent.second - have;
                uint32 const price = uint32(std::ceil(double(proto->BuyPrice) / std::max<uint32>(1, proto->BuyCount) * need));
                ItemPosCountVec dest;
                if (!bot->HasEnoughMoney(price) || bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, reagent.first, need) != EQUIP_ERR_OK)
                    return false;
                bot->ModifyMoney(-int32(price));
                bot->StoreNewItem(dest, reagent.first, true);
            }
            return true;
        }

        /// The bot thinks through some of its recipes: what would the product bring, what do the materials
        /// cost - those it carries (it could sell them instead), those on offer here, those from a vendor?
        /// For the first recipe that pays it buys what is missing. A recipe that still raises its skill is
        /// worth a little more to it. Returns true if it bought something.
        bool PlanCraft(Player* bot, PlayerbotAI* botAI, AuctionHouseObject* house, std::vector<Recipe> recipes)
        {
            if (!cfg.matsEnabled || recipes.empty())
                return false;
            Plan& plan = _plans[bot->GetGUID().GetCounter()];
            if (plan.until > _now)
                return false;       // still busy with the last one
            plan = Plan();

            // The offers of every material, cheapest first.
            struct Offer
            {
                uint32 id, each, count, buyout;
            };
            std::unordered_set<uint32> wanted;
            for (Recipe const& recipe : recipes)
                for (auto const& reagent : recipe.reagents)
                    wanted.insert(reagent.first);
            std::unordered_map<uint32, std::vector<Offer>> offers;
            uint32 const account = bot->GetSession()->GetAccountId();
            for (auto const& entry : house->GetAuctions())
            {
                AuctionEntry const* auction = entry.second;
                if (!auction || !auction->buyout || !auction->itemCount || auction->owner == bot->GetGUID() ||
                    wanted.find(auction->item_template) == wanted.end())
                    continue;
                uint32 const owner = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
                if (!owner || owner == account)
                    continue;
                if (sPlayerbotAIConfig.IsInRandomAccountList(owner) ? !cfg.buyFromBots : !cfg.buyFromPlayers)
                    continue;
                offers[auction->item_template].push_back({ auction->Id, auction->buyout / auction->itemCount, auction->itemCount, auction->buyout });
            }
            for (auto& list : offers)
                std::sort(list.second.begin(), list.second.end(), [](Offer const& a, Offer const& b) { return a.each < b.each; });

            double const purse = cfg.buyUseBotMoney ? double(bot->GetMoney()) * cfg.buyMoneyShare / 100.0 : double(MAX_MONEY_AMOUNT);
            Acore::Containers::RandomShuffle(recipes);
            if (recipes.size() > cfg.matsRecipes)
                recipes.resize(cfg.matsRecipes);

            for (Recipe const& recipe : recipes)
            {
                ItemTemplate const* product = sObjectMgr->GetItemTemplate(recipe.product);
                if (!product || !product->SellPrice || product->Quality >= MAX_ITEM_QUALITY)
                    continue;

                // What the product is worth to the bot: its price minus the auction house's cut if it sells it,
                // a little more if it can use it itself.
                ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", product->ItemId)->Get();
                bool const forItself = usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_USE;
                if (!forItself && !IsAllowedKind(product))
                    continue;
                double worth = market.Value(product) * recipe.made * (forItself ? 1.2 : 0.95);
                if (ItemUsageValue::SpellGivesSkillUp(recipe.spell, bot))
                    worth *= 1.0 + cfg.matsSkillBonus / 100.0;

                double cost = 0.0, toPay = 0.0;
                std::vector<uint32> toBuy;
                bool possible = true;
                for (auto const& reagent : recipe.reagents)
                {
                    ItemTemplate const* material = sObjectMgr->GetItemTemplate(reagent.first);
                    if (!material)
                    {
                        possible = false;
                        break;
                    }
                    uint32 const have = std::min(bot->GetItemCount(reagent.first), reagent.second);
                    if (have && material->SellPrice && material->Quality < MAX_ITEM_QUALITY)
                        cost += market.Value(material) * have;
                    uint32 need = reagent.second - have;
                    if (!need)
                        continue;

                    if (cfg.matsVendor && material->BuyPrice > 0 && market.IsVendorSupply(reagent.first))
                    {
                        double const price = double(material->BuyPrice) / std::max<uint32>(1, material->BuyCount) * need;
                        cost += price;
                        toPay += price;
                        continue;
                    }

                    // The bot does not pay any price because the product is valuable: a material that costs far
                    // more than usual is left alone, however well the recipe would pay.
                    double const usual = material->SellPrice && material->Quality < MAX_ITEM_QUALITY ? market.Value(material) : 0.0;
                    auto found = offers.find(reagent.first);
                    if (found != offers.end())
                        for (Offer const& offer : found->second)
                        {
                            if (!need || double(offer.each) > usual * cfg.matsMaxPrice)
                                break;
                            if (cfg.maxBuyout && offer.buyout > cfg.maxBuyout)
                                continue;
                            uint32 const used = std::min(need, offer.count);
                            // It pays for the whole stack; what is left over it can sell again, for a little less.
                            cost += double(offer.buyout) - double(offer.each) * (offer.count - used) * 0.9;
                            toPay += offer.buyout;
                            toBuy.push_back(offer.id);
                            need -= used;
                        }
                    if (need)
                    {
                        possible = false;
                        break;
                    }
                }
                if (!possible || toPay > purse || worth < cost * (1.0 + cfg.matsMinProfit / 100.0))
                    continue;

                // It pays. Buy what is missing; if someone was quicker, the plan is dropped.
                bool bought = false;
                for (uint32 const id : toBuy)
                {
                    AuctionEntry* auction = house->GetAuction(id);
                    ItemTemplate const* material = auction ? sObjectMgr->GetItemTemplate(auction->item_template) : nullptr;
                    if (!auction || !material || !auction->buyout || auction->owner == bot->GetGUID() || !Buyout(bot, house, auction, material))
                        return bought;
                    bought = true;
                }

                plan.recipe = recipe;
                plan.until = _now + 2 * MINUTE;
                plan.started = false;
                if (cfg.debug)
                    LOG_INFO("module", "PlayerbotsAuctions: {} plans to craft {} (item {}): materials worth {} copper, product worth {} copper to it.",
                        bot->GetName(), product->Name1, product->ItemId, uint64(cost), uint64(worth));
                return bought;
            }
            return false;
        }

        /// mod-playerbots gives the bots professions and recipes, but nothing makes the bots craft. Here they
        /// carry out what PlanCraft decided.
        bool Craft(Player* bot, PlayerbotAI* botAI, time_t now)
        {
            if (!cfg.craftEnabled || bot->IsNonMeleeSpellCast(false))
                return false;

            auto plan = _plans.find(bot->GetGUID().GetCounter());
            if (plan == _plans.end() || plan->second.started || plan->second.until <= now)
                return false;
            plan->second.started = true;        // one try; the materials stay reserved until the cast is over

            // Crafting is done standing, on foot and not on the move.
            bot->StopMoving();
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            if (!bot->IsStandState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);

            if (cfg.matsVendor && !BuyFromVendor(bot, plan->second.recipe))
                return false;
            if (!botAI->CastSpell(plan->second.recipe.spell, bot))
                return false;
            if (cfg.debug)
                if (ItemTemplate const* product = sObjectMgr->GetItemTemplate(plan->second.recipe.product))
                    LOG_INFO("module", "PlayerbotsAuctions: {} crafts {} (item {}).", bot->GetName(), product->Name1, product->ItemId);
            return true;
        }

        /// Does this bot want to go to the auction house now? Like a player, it goes when it feels it has
        /// enough to sell or its bags are getting full - and every bot draws that line somewhere else.
        /// A bot that is in town anyway goes for less, but not every time.
        bool DecideToGo(Player* bot, PlayerbotAI* botAI, time_t now)
        {
            if (!cfg.cityTrips || bot->GetGroup())
                return false;
            ++_decisions;       // looking through the bags of many bots at once would cost a noticeable moment

            auto next = _nextTrip.find(bot->GetGUID().GetCounter());
            if (next != _nextTrip.end() && next->second > now)
                return false;

            std::vector<Item*> items;
            Collect(bot, botAI, items);
            uint32 const goods = uint32(items.size());
            bool const inTown = IsInTown(bot);

            // This bot's own lines: how full is full, and how much is enough.
            uint32 const full = cfg.cityBagMin + uint32(Trait(bot, TRAIT_BAGS) * float(cfg.cityBagMax - cfg.cityBagMin));
            uint32 const enough = cfg.goodsMin + uint32(Trait(bot, TRAIT_GOODS) * float(cfg.goodsMax - cfg.goodsMin));
            bool const bagsFull = botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get() >= full;

            bool go;
            char const* why;
            if (goods >= cfg.cityMinItems && bagsFull)
            {
                go = true;
                why = "its bags are full";
            }
            else if (goods >= enough)
            {
                go = true;
                why = "it has enough to sell";
            }
            else
            {
                // In town with a few things to sell: worth the walk, if it feels like it.
                go = inTown && goods >= std::max<uint32>(1, enough / 3) && urand(0, 99) < cfg.inTownChance;
                why = "it is in town anyway";
            }

            if (!go)
            {
                _nextTrip[bot->GetGUID().GetCounter()] = now + (inTown ? 5 * MINUTE : 10 * MINUTE);
                return false;
            }

            Journey const journey = SendToAuctionHouse(bot, botAI);
            if (journey == JOURNEY_NONE)
            {
                _nextTrip[bot->GetGUID().GetCounter()] = now + 10 * MINUTE;
                return false;
            }

            _nextTrip[bot->GetGUID().GetCounter()] = now + urand(cfg.cityCooldownMin, cfg.cityCooldownMax);
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} {} to the auction house with {} item(s) to sell: {}.",
                    bot->GetName(), journey == JOURNEY_WALK ? "walks" : "takes its hearth to a city and walks", goods, why);
            return true;
        }

        /// The bot looks through some of the offers and buys or bids on what is worth it to this bot.
        /// Returns true if it bought something outright.
        bool Buy(Player* bot, PlayerbotAI* botAI, AuctionHouseObject* house, std::unordered_set<uint32> const& materials)
        {
            if (!cfg.buyEnabled || urand(0, 99) >= cfg.buyChance)
                return false;
            // With bags this full the bot has to sell first; a purchase would only wait in the mail.
            if (botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get() >= 90)
                return false;

            // A random handful of the offers, like a player who browses a few pages.
            std::vector<uint32> offers;
            uint32 seen = 0;
            for (auto const& entry : house->GetAuctions())
            {
                AuctionEntry const* auction = entry.second;
                if (!auction || auction->owner == bot->GetGUID() || auction->bidder == bot->GetGUID())
                    continue;
                ++seen;
                if (offers.size() < cfg.buyCandidates)
                    offers.push_back(auction->Id);
                else
                {
                    uint32 const slot = urand(0, seen - 1);
                    if (slot < cfg.buyCandidates)
                        offers[slot] = auction->Id;
                }
            }
            Acore::Containers::RandomShuffle(offers);

            uint32 deals = 0;
            bool bought = false;
            for (uint32 const id : offers)
            {
                if (deals >= cfg.buyMaxPerVisit)
                    break;
                // Looked up again by its number: an auction bought a moment ago no longer exists.
                AuctionEntry* auction = house->GetAuction(id);
                if (!auction)
                    continue;
                switch (Consider(bot, botAI, house, auction, materials))
                {
                    case DEAL_BOUGHT: bought = true; ++deals; break;
                    case DEAL_BID: ++deals; break;
                    default: break;
                }
            }
            return bought;
        }

        enum Deal { DEAL_NONE, DEAL_BID, DEAL_BOUGHT };

        Deal Consider(Player* bot, PlayerbotAI* botAI, AuctionHouseObject* house, AuctionEntry* auction,
            std::unordered_set<uint32> const& materials)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
            if (!proto || !auction->itemCount || !proto->SellPrice || proto->Quality >= MAX_ITEM_QUALITY)
                return DEAL_NONE;

            // The server does not let anyone bid on the auctions of the own account.
            uint32 const ownerAccount = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
            if (!ownerAccount || ownerAccount == bot->GetSession()->GetAccountId())
                return DEAL_NONE;
            if (sPlayerbotAIConfig.IsInRandomAccountList(ownerAccount) ? !cfg.buyFromBots : !cfg.buyFromPlayers)
                return DEAL_NONE;

            // One of a kind is enough until the server restarts; otherwise a bot would buy the same
            // "upgrade" again before it has put the first one on.
            std::unordered_set<uint32>& had = _bought[bot->GetGUID().GetCounter()];
            bool const isMaterial = materials.find(proto->ItemId) != materials.end();
            if (!isMaterial && had.find(proto->ItemId) != had.end())
                return DEAL_NONE;

            // How much the bot wants it: an upgrade for itself most, things it uses up next,
            // and now and then something it has no use for - to sell it on.
            double interest = 0.0;
            switch (botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", proto->ItemId)->Get())
            {
                case ITEM_USAGE_EQUIP:
                case ITEM_USAGE_REPLACE:
                    interest = 1.5;
                    break;
                case ITEM_USAGE_USE:
                case ITEM_USAGE_SKILL:
                case ITEM_USAGE_AMMO:
                    interest = 1.2;
                    break;
                case ITEM_USAGE_AH:
                case ITEM_USAGE_VENDOR:
                case ITEM_USAGE_NONE:
                    if (isMaterial && bot->GetItemCount(proto->ItemId) < proto->GetMaxStackSize())
                    {
                        interest = 1.0;     // material for its profession: worth a stack when the price is right
                        break;
                    }
                    if (urand(0, 99) >= cfg.buySpeculateChance)
                        return DEAL_NONE;
                    interest = 0.85;
                    break;
                default:
                    return DEAL_NONE;
            }

            // What one piece is worth to this bot today. Mostly around the going price, sometimes far above:
            // the bot that simply has to have it.
            double const mood = urand(0, 99) < cfg.buyImpulseChance ? frand(1.5f, cfg.buyImpulseMax) : frand(0.7f, 1.2f);
            double each = market.Value(proto) * interest * mood;
            // Always more than a vendor pays, or nobody would bother with the auction house ...
            each = std::max(each, double(proto->SellPrice) * cfg.buyMinVendorFactor);
            // ... but less than a vendor asks, or buying from a vendor and selling to the bots would print money.
            if (proto->BuyPrice > 0 && market.IsVendorItem(proto->ItemId))
                each = std::min(each, double(proto->BuyPrice) / std::max<uint32>(1, proto->BuyCount) * cfg.buyVendorItemPercent / 100.0);

            double limit = std::min(each * auction->itemCount, cfg.maxBuyout ? double(cfg.maxBuyout) : double(MAX_MONEY_AMOUNT));
            if (cfg.buyUseBotMoney)
                limit = std::min(limit, double(bot->GetMoney()) * cfg.buyMoneyShare / 100.0);
            if (limit < 1.0)
                return DEAL_NONE;

            if (auction->buyout && double(auction->buyout) <= limit)
            {
                if (!Buyout(bot, house, auction, proto))
                    return DEAL_NONE;
                had.insert(proto->ItemId);
                return DEAL_BOUGHT;
            }

            if (!cfg.buyBids)
                return DEAL_NONE;
            // A bid is a bet on getting it cheaper, so the bot stays a little below what it would pay outright.
            uint32 const nextBid = std::max(auction->startbid, auction->bid ? auction->bid + auction->GetAuctionOutBid() : auction->startbid);
            if (!nextBid || (auction->buyout && nextBid >= auction->buyout) || double(nextBid) > limit * 0.85)
                return DEAL_NONE;
            if (!Bid(bot, auction, proto, nextBid))
                return DEAL_NONE;
            had.insert(proto->ItemId);
            return DEAL_BID;
        }

        /// With "use bot money" off the purchase costs the bot nothing: it is handed the price first.
        bool Afford(Player* bot, uint32 price)
        {
            if (price > MAX_MONEY_AMOUNT)
                return false;
            if (cfg.buyUseBotMoney)
                return bot->HasEnoughMoney(price);
            if (bot->GetMoney() > MAX_MONEY_AMOUNT - price)
                return false;
            bot->ModifyMoney(int32(price));
            return true;
        }

        // The two functions below do what the server does when a player bids or buys out.
        bool Bid(Player* bot, AuctionEntry* auction, ItemTemplate const* proto, uint32 price)
        {
            if (price <= auction->bid || price < auction->startbid || !Afford(bot, price))
                return false;

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            if (auction->bidder)
                sAuctionMgr->SendAuctionOutbiddedMail(auction, price, bot, trans);
            bot->ModifyMoney(-int32(price));
            auction->bidder = bot->GetGUID();
            auction->bid = price;
            sAuctionMgr->GetAuctionHouseSearcher()->UpdateBid(auction);

            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_AUCTION_BID);
            stmt->SetData(0, auction->bidder.GetCounter());
            stmt->SetData(1, auction->bid);
            stmt->SetData(2, auction->Id);
            trans->Append(stmt);
            bot->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} bids {} copper on {} x{} (item {}).",
                    bot->GetName(), price, proto->Name1, auction->itemCount, proto->ItemId);
            return true;
        }

        bool Buyout(Player* bot, AuctionHouseObject* house, AuctionEntry* auction, ItemTemplate const* proto)
        {
            uint32 const price = auction->buyout;
            if (!Afford(bot, price))
                return false;
            uint32 const count = auction->itemCount;

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            bot->ModifyMoney(-int32(price));
            if (auction->bidder)
                sAuctionMgr->SendAuctionOutbiddedMail(auction, price, bot, trans);
            auction->bidder = bot->GetGUID();
            auction->bid = price;

            sAuctionMgr->SendAuctionSalePendingMail(auction, trans);
            sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
            sAuctionMgr->SendAuctionWonMail(auction, trans);
            sScriptMgr->OnAuctionSuccessful(house, auction);

            auction->DeleteFromDB(trans);
            sAuctionMgr->RemoveAItem(auction->item_guid);
            house->RemoveAuction(auction);          // the auction no longer exists after this line

            bot->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} buys {} x{} (item {}) for {} copper.",
                    bot->GetName(), proto->Name1, count, proto->ItemId, price);
            return true;
        }

        /// The bot empties its auction mail: the money of sold items and the items nobody bought.
        /// Without this the mail would pile up, because the bots never open a mailbox for it.
        void CollectMail(Player* bot, time_t now)
        {
            uint32 money = 0, returned = 0;
            std::vector<Mail*> touched;

            // A copy: taking money or items can make the server send the bot new mail, which changes the list.
            std::vector<Mail*> const mails(bot->GetMails().begin(), bot->GetMails().end());
            for (Mail* mail : mails)
            {
                bool changed = false;
                if (!mail || mail->state == MAIL_STATE_DELETED || mail->messageType != MAIL_AUCTION ||
                    mail->COD || mail->deliver_time > now)
                    continue;

                if (mail->money && bot->ModifyMoney(int32(mail->money), false))
                {
                    money += mail->money;
                    mail->money = 0;
                    mail->state = MAIL_STATE_CHANGED;
                    changed = true;
                }

                std::vector<MailItemInfo> const attached = mail->items;
                for (MailItemInfo const& info : attached)
                {
                    Item* item = bot->GetMItem(info.item_guid);
                    if (!item)
                        continue;

                    ItemPosCountVec dest;
                    if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                        continue;       // bags are full: the item stays in the mail for the next visit

                    mail->RemoveItem(info.item_guid);
                    mail->removedItems.push_back(info.item_guid);
                    mail->state = MAIL_STATE_CHANGED;
                    bot->RemoveMItem(info.item_guid);
                    item->SetState(ITEM_UNCHANGED);
                    bot->MoveItemToInventory(dest, item, true);
                    ++returned;
                    changed = true;
                }

                if (!mail->money && mail->items.empty())
                {
                    mail->state = MAIL_STATE_DELETED;
                    PBA_MAIL_DELETED(bot->GetGUID());
                    changed = true;
                }

                if (changed)
                    touched.push_back(mail);
            }

            if (touched.empty())
                return;

            // Money, bags and mail are written together, as the server does when a player empties a mail.
            // Otherwise a crash in between would hand out the money or the item a second time.
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            bot->SaveInventoryAndGoldToDB(trans);
            for (Mail* mail : touched)
            {
                if (mail->state == MAIL_STATE_DELETED)
                {
                    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
                    stmt->SetData(0, mail->messageID);
                    trans->Append(stmt);
                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
                    stmt->SetData(0, mail->messageID);
                    trans->Append(stmt);
                    mail->removedItems.clear();
                    continue;       // the server frees the mail at the bot's next save
                }

                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_MAIL);
                stmt->SetData(0, uint8(mail->HasItems() ? 1 : 0));
                stmt->SetData(1, uint32(mail->expire_time));
                stmt->SetData(2, uint32(mail->deliver_time));
                stmt->SetData(3, mail->money);
                stmt->SetData(4, mail->COD);
                stmt->SetData(5, uint8(mail->checked));
                stmt->SetData(6, mail->messageID);
                trans->Append(stmt);
                for (uint32 const itemGuid : mail->removedItems)
                {
                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM);
                    stmt->SetData(0, itemGuid);
                    trans->Append(stmt);
                }
                mail->removedItems.clear();
                mail->state = MAIL_STATE_UNCHANGED;
            }
            CharacterDatabase.CommitTransaction(trans);
            bot->m_mailsUpdated = true;

            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} collected {} copper and {} unsold item(s) from its auction mail.",
                    bot->GetName(), money, returned);
        }

        void Consider(Player* bot, PlayerbotAI* botAI, Item* item, std::vector<Item*>& items)
        {
            if (!IsSellable(bot, item))
                return;

            // Materials for what the bot is about to craft are not for sale.
            auto plan = _plans.find(bot->GetGUID().GetCounter());
            if (plan != _plans.end() && plan->second.until > _now)
                for (auto const& reagent : plan->second.recipe.reagents)
                    if (reagent.first == item->GetEntry())
                        return;

            // The bot's own judgement: only what it neither uses, wears, needs for a quest or a profession.
            ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
            if (usage == ITEM_USAGE_AH || usage == ITEM_USAGE_VENDOR)
                items.push_back(item);
        }

        void Collect(Player* bot, PlayerbotAI* botAI, std::vector<Item*>& items)
        {
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                Consider(bot, botAI, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), items);

            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        Consider(bot, botAI, bag->GetItemByPos(slot), items);
        }

        bool Post(Player* bot, Item* item, AuctionHouseObject* house, AuctionHouseEntry const* houseEntry,
            AuctionHouseId houseId, std::unordered_map<uint32, uint32>& cheapest)
        {
            ItemTemplate const* proto = item->GetTemplate();
            uint32 const count = item->GetCount();
            if (!count)
                return false;

            // Price of one piece: the vendor value times a factor per quality, a little up or down.
            // Price of one piece: what the item is worth, a little up or down. Now and then a seller wants to be
            // rid of it quickly, or dreams of getting rich - those are the bargains and the overpriced offers.
            // Each bot has its own price level, a little above or below the others.
            double const temper = 1.0 + (double(Trait(bot, TRAIT_PRICE)) * 2.0 - 1.0) * cfg.temperPercent / 100.0;
            double const regular = market.Value(proto) * temper;
            double each;
            uint32 const roll = urand(0, 99);
            if (roll < cfg.bargainChance)
                each = regular * frand(0.55f, 0.85f);
            else if (roll < cfg.bargainChance + cfg.overpricedChance)
                each = regular * frand(1.5f, cfg.overpricedMax);
            else
            {
                each = regular * frand(1.0f - cfg.priceVariation / 100.0f, 1.0f + cfg.priceVariation / 100.0f);

                // Like a player, the bot goes a little below the cheapest offer. A limit keeps the bots from
                // underbidding each other down to nothing.
                auto found = cheapest.find(proto->ItemId);
                if (found == cheapest.end())
                    each *= frand(cfg.aloneMin, cfg.aloneMax);      // nobody else offers it: the seller asks for more
                else if (cfg.undercutPercent && double(found->second) <= each)
                    each = double(found->second) * (100 - cfg.undercutPercent) / 100.0;
                each = std::max(each, regular * (100 - cfg.maxUndercutPercent) / 100.0);
            }
            // Never close to the vendor value: nobody should buy from a bot to sell to a vendor.
            each = std::max(each, double(proto->SellPrice) * cfg.minVendorFactor);

            // An item worth more than the highest allowed price is kept, not given away.
            double const total = each * count;
            double const limit = cfg.maxBuyout ? double(cfg.maxBuyout) : double(MAX_MONEY_AMOUNT);
            if (total > limit)
                return false;
            uint32 const buyout = std::max<uint32>(uint32(total), 2);
            uint32 const bid = std::max<uint32>(uint32(uint64(buyout) * cfg.bidPercent / 100), 1);

            static uint32 const hours[] = { 12, 24, 48 };
            uint32 const etime = hours[urand(0, 2)] * HOUR;
            uint32 const auctionTime = uint32(etime * sWorld->getRate(RATE_AUCTION_TIME));

            uint32 deposit = 0;
            if (cfg.chargeDeposit)
            {
                deposit = AuctionHouseMgr::GetAuctionDeposit(houseEntry, etime, item, count);
                if (!bot->HasEnoughMoney(deposit))
                    return false;
                bot->ModifyMoney(-int32(deposit));
            }

            AuctionEntry* auction = new AuctionEntry;
            auction->Id = sObjectMgr->GenerateAuctionID();
            auction->houseId = houseId;
            auction->item_guid = item->GetGUID();
            auction->item_template = item->GetEntry();
            auction->itemCount = count;
            auction->owner = bot->GetGUID();
            auction->startbid = bid;
            auction->bidder = ObjectGuid::Empty;
            auction->bid = 0;
            auction->buyout = buyout;
            auction->expire_time = GameTime::GetGameTime().count() + auctionTime;
            auction->deposit = deposit;
            auction->auctionHouseEntry = houseEntry;

            sAuctionMgr->AddAItem(item);
            house->AddAuction(auction);

            bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            item->DeleteFromInventoryDB(trans);
            item->SaveToDB(trans);
            auction->SaveToDB(trans);
            bot->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            uint32 const perPiece = buyout / count;
            auto known = cheapest.find(proto->ItemId);
            if (known == cheapest.end() || perPiece < known->second)
                cheapest[proto->ItemId] = perPiece;

            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} offers {} x{} (item {}) for {} copper, bid {} copper, {} h.",
                    bot->GetName(), proto->Name1, count, proto->ItemId, buyout, bid, etime / HOUR);
            return true;
        }

        uint32 _timer = 0;
        uint32 _arrivalTimer = 0;
        ObjectGuid _last;
        std::unordered_map<ObjectGuid::LowType, time_t> _nextVisit;
        std::unordered_map<ObjectGuid::LowType, time_t> _nextTrip;
        struct Plan
        {
            Recipe recipe;
            time_t until = 0;       // its materials are reserved until then
            bool started = false;
        };
        std::unordered_map<ObjectGuid::LowType, Plan> _plans;
        time_t _now = 0;
        uint32 _decisions = 0;

        struct Watch
        {
            time_t from = 0;        // not looked at before this
            time_t until = 0;       // given up after this
            bool sellOnly = false;  // a second look only to sell what was just made
            bool renewed = false;
        };
        std::map<ObjectGuid, Watch> _arrivals;                         // bots on their way to the auctioneers
        std::unordered_map<ObjectGuid::LowType, std::unordered_set<uint32>> _bought;
    };

    AuctionSeller seller;
}

class PlayerbotsAuctionsWorld : public WorldScript
{
public:
    PlayerbotsAuctionsWorld() : WorldScript("PlayerbotsAuctionsWorld") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadSettings();
    }

    void OnStartup() override
    {
        market.LoadVendorItems();
        if (cfg.enabled)
            LOG_INFO("server.loading", ">> PlayerbotsAuctions: the bots use the auction house.");
    }

    void OnUpdate(uint32 diff) override
    {
        seller.Update(diff);
    }
};

/// Every sale in the auction house, between whomever, teaches the bots what things go for.
class PlayerbotsAuctionsHouse : public AuctionHouseScript
{
public:
    PlayerbotsAuctionsHouse() : AuctionHouseScript("PlayerbotsAuctionsHouse") { }

    void OnAuctionSuccessful(AuctionHouseObject* /*house*/, AuctionEntry* auction) override
    {
        if (!auction)
            return;
        // Only sales a bot took part in count. Two players (or one player with two accounts) trading an
        // item back and forth at a fantasy price must not teach the bots anything.
        uint32 const seller = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
        uint32 const buyer = sCharacterCache->GetCharacterAccountIdByGuid(auction->bidder);
        if ((seller && sPlayerbotAIConfig.IsInRandomAccountList(seller)) || (buyer && sPlayerbotAIConfig.IsInRandomAccountList(buyer)))
            market.RecordSale(auction->item_template, auction->bid, auction->itemCount);
    }
};

void AddSC_playerbots_auctions()
{
    new PlayerbotsAuctionsWorld();
    new PlayerbotsAuctionsHouse();
}
