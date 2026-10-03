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
    enum SellPlace
    {
        PLACE_ANYWHERE   = 0,   // wherever the bot is (not realistic, fills the auction house fastest)
        PLACE_CITY       = 1,   // in a capital city, or near an auctioneer elsewhere
        PLACE_AUCTIONEER = 2    // only near an auctioneer
    };

    struct Settings
    {
        bool   enabled = false;
        bool   debug = false;
        uint32 intervalMs = 30000;
        uint32 botsPerCycle = 10;
        uint32 place = PLACE_CITY;
        float  auctioneerRange = 40.0f;
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
        uint32 cityBagPercent = 80;
        uint32 cityMinItems = 3;
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
        cfg.place               = sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Place", PLACE_CITY);
        cfg.auctioneerRange     = sConfigMgr->GetOption<float>("PlayerbotsAuctions.AuctioneerRange", 40.0f);
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
        cfg.cityBagPercent      = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.BagsFullPercent", 80), 10, 100);
        cfg.cityMinItems        = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.CityTrip.MinItemsToSell", 3));
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

    bool IsInCapital(Player* bot)
    {
        // Shattrath and Dalaran count as capitals but have no auction house for everyone.
        if (bot->GetZoneId() == 3703 || bot->GetZoneId() == 4395)
            return false;

        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId()))
            if (area->flags & AREA_FLAG_CAPITAL)
                return true;
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
            if (zone->flags & AREA_FLAG_CAPITAL)
                return true;
        return false;
    }

    /// The auction house the bot sells in, or false if the bot is in no place to sell.
    bool FindHouse(Player* bot, PlayerbotAI* botAI, AuctionHouseId& houseId)
    {
        // The cheap test first: only outside a capital the surroundings are searched for an auctioneer.
        Creature* auctioneer = nullptr;
        if (cfg.place == PLACE_AUCTIONEER || (cfg.place == PLACE_CITY && !IsInCapital(bot)))
        {
            auctioneer = FindAuctioneer(bot, botAI);
            if (!auctioneer)
                return false;
        }

        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
        {
            houseId = AuctionHouseId::Neutral;
            return true;
        }

        if (auctioneer)
            if (AuctionHouseEntry const* entry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction()))
            {
                houseId = AuctionHouseId(entry->houseId);
                return true;
            }

        houseId = bot->GetTeamId() == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde;
        return true;
    }

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
        }

        bool IsVendorItem(uint32 itemId) const { return _vendorItems.find(itemId) != _vendorItems.end(); }

    private:
        struct Learned
        {
            double each = 0.0;      // price of one piece
            uint32 sales = 0;
        };
        std::unordered_map<uint32, Learned> _sold;      // item -> what it really sold for
        std::unordered_set<uint32> _vendorItems;
    };

    Market market;

    struct CityPoint
    {
        TeamId team;
        uint32 mapId;
        float x, y, z;
    };

    // The auction houses of the eight old capitals.
    CityPoint const AuctionHouses[] =
    {
        { TEAM_ALLIANCE, 0,   -8820.1f,    662.0f,   97.2f },    // Stormwind
        { TEAM_ALLIANCE, 0,   -4959.7f,   -907.9f,  505.2f },    // Ironforge
        { TEAM_ALLIANCE, 1,    9864.5f,   2341.7f, 1326.8f },    // Darnassus
        { TEAM_ALLIANCE, 530, -4025.5f, -11736.0f, -151.8f },    // Exodar
        { TEAM_HORDE,    1,    1683.6f,  -4461.3f,   20.4f },    // Orgrimmar
        { TEAM_HORDE,    1,   -1204.7f,    102.8f,  134.7f },    // Thunder Bluff
        { TEAM_HORDE,    0,    1612.3f,    199.8f,  -56.8f },    // Undercity
        { TEAM_HORDE,    530,  9648.4f,  -7135.7f,   16.9f },    // Silvermoon
    };

    // The CoA Playerbots fork lets bots visit a city and return afterwards. Other versions of
    // mod-playerbots do not have that; there the bots simply are not sent to town.
    template <typename Info>
    concept HasCityLife = requires(Info& info, WorldPosition pos) { info.ChangeToGoCity(pos); info.cityReturnPos = pos; info.cityStayMs = 0u; };

    template <typename AI>
    bool SendToAuctionHouse(Player* bot, AI* botAI)
    {
        if constexpr (HasCityLife<decltype(botAI->rpgInfo)>)
        {
            if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
                return false;
            auto& info = botAI->rpgInfo;
            if (info.cityStayMs || info.cityReturnPos != WorldPosition())
                return false;       // already on a city visit

            // The nearest auction house of the bot's faction on the same map. A trip to another map would
            // make mod-playerbots forget the visit and the way back, so bots in Northrend stay where they are.
            CityPoint const* best = nullptr;
            float bestDistance = 0.0f;
            for (CityPoint const& point : AuctionHouses)
            {
                if (point.team != bot->GetTeamId() || point.mapId != bot->GetMapId())
                    continue;
                float const distance = bot->GetExactDist2d(point.x, point.y);
                if (!best || distance < bestDistance)
                {
                    best = &point;
                    bestDistance = distance;
                }
            }
            if (!best)
                return false;

            float const angle = frand(0.0f, 6.2831853f);
            float const radius = frand(2.0f, 6.0f);
            WorldPosition const target(best->mapId, best->x + radius * std::cos(angle), best->y + radius * std::sin(angle), best->z);

            info.cityReturnPos = WorldPosition(bot);
            if (!bot->TeleportTo(best->mapId, target.GetPositionX(), target.GetPositionY(), target.GetPositionZ(), bot->GetOrientation()))
            {
                info.cityReturnPos = WorldPosition();
                return false;
            }
            info.ChangeToGoCity(target);
            return true;
        }
        else
        {
            return false;
        }
    }

    class AuctionSeller
    {
    public:
        void Update(uint32 diff)
        {
            if (!cfg.enabled)
                return;

            _timer += diff;
            if (_timer < cfg.intervalMs)
                return;
            _timer = 0;

            // The list of bots is walked a part at a time, so a large bot population costs nothing noticeable.
            PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
            if (bots.empty())
                return;

            time_t const now = GameTime::GetGameTime().count();

            // Bots that just travelled to an auction house come first, before they sell their load to a vendor.
            for (auto arrival = _arrivals.begin(); arrival != _arrivals.end();)
            {
                auto found = bots.find(arrival->first);
                bool done = found == bots.end() || arrival->second < now;
                if (!done && found->second && found->second->IsInWorld() && !found->second->IsBeingTeleported())
                {
                    _nextVisit.erase(arrival->first.GetCounter());
                    done = Visit(found->second, now);
                }
                arrival = done ? _arrivals.erase(arrival) : std::next(arrival);
            }

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
        bool Visit(Player* bot, time_t now)
        {
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
                // Not in a place to sell: look again in a while, not at every pass.
                _nextVisit[bot->GetGUID().GetCounter()] = now + urand(2 * MINUTE, 4 * MINUTE);
                if (TryCityTrip(bot, botAI, now))
                    _arrivals[bot->GetGUID()] = now + 3 * MINUTE;      // watched until it has arrived and sold
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

            if (Buy(bot, botAI, house) && cfg.collectMail)
                CollectMail(bot, now);          // what it bought outright arrives by mail at once

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

        /// Bags nearly full and far from an auction house: the bot takes its hearth to one, sells there and
        /// comes back when its city visit is over. The travelling itself is done by mod-playerbots.
        bool TryCityTrip(Player* bot, PlayerbotAI* botAI, time_t now)
        {
            if (!cfg.cityTrips || cfg.place == PLACE_ANYWHERE || bot->GetGroup())
                return false;

            auto next = _nextTrip.find(bot->GetGUID().GetCounter());
            if (next != _nextTrip.end() && next->second > now)
                return false;

            if (botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get() < cfg.cityBagPercent)
                return false;

            std::vector<Item*> items;
            Collect(bot, botAI, items);
            if (items.size() < cfg.cityMinItems)
            {
                _nextTrip[bot->GetGUID().GetCounter()] = now + 10 * MINUTE;      // full of things it keeps
                return false;
            }

            if (!SendToAuctionHouse(bot, botAI))
            {
                _nextTrip[bot->GetGUID().GetCounter()] = now + 10 * MINUTE;
                return false;
            }

            _nextTrip[bot->GetGUID().GetCounter()] = now + urand(cfg.cityCooldownMin, cfg.cityCooldownMax);
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} has full bags ({} items to sell) and travels to an auction house.",
                    bot->GetName(), items.size());
            return true;
        }

        /// The bot looks through some of the offers and buys or bids on what is worth it to this bot.
        /// Returns true if it bought something outright.
        bool Buy(Player* bot, PlayerbotAI* botAI, AuctionHouseObject* house)
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
                switch (Consider(bot, botAI, house, auction))
                {
                    case DEAL_BOUGHT: bought = true; ++deals; break;
                    case DEAL_BID: ++deals; break;
                    default: break;
                }
            }
            return bought;
        }

        enum Deal { DEAL_NONE, DEAL_BID, DEAL_BOUGHT };

        Deal Consider(Player* bot, PlayerbotAI* botAI, AuctionHouseObject* house, AuctionEntry* auction)
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
            if (had.find(proto->ItemId) != had.end())
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
            double const regular = market.Value(proto);
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
                if (found != cheapest.end() && cfg.undercutPercent && double(found->second) <= each)
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
        ObjectGuid _last;
        std::unordered_map<ObjectGuid::LowType, time_t> _nextVisit;
        std::unordered_map<ObjectGuid::LowType, time_t> _nextTrip;
        std::map<ObjectGuid, time_t> _arrivals;                        // bot on its way -> give up after
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
            LOG_INFO("server.loading", ">> PlayerbotsAuctions: the bots put their loot up for auction (place {}, every {} s).",
                cfg.place, cfg.intervalMs / IN_MILLISECONDS);
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
