/*
 * mod-playerbots-auctions - lets the random bots of mod-playerbots put their loot up for auction.
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * The bots already decide for every item whether they need it. What they do not need and may trade
 * they sell to a vendor. This module steps in before that: a bot that is in a capital city or near
 * an auctioneer puts those items up for auction in its own name, like a player would.
 *
 * Nothing in mod-playerbots is changed; the module only reads from it.
 */

#include "AiObjectContext.h"
#include "AuctionHouseMgr.h"
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
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
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
        cfg.priceVariation      = std::min<uint32>(90, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.VariationPercent", 20));
        cfg.bidPercent          = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.BidPercent", 70), 1, 100);
        cfg.undercutPercent     = std::min<uint32>(50, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.UndercutPercent", 5));
        cfg.minVendorFactor     = std::max(1.0f, sConfigMgr->GetOption<float>("PlayerbotsAuctions.Price.MinVendorFactor", 1.5f));
        cfg.maxUndercutPercent  = std::min<uint32>(90, sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.MaxUndercutPercent", 25));
        cfg.maxBuyout           = uint32(std::min<uint64>(uint64(sConfigMgr->GetOption<uint32>("PlayerbotsAuctions.Price.MaxBuyoutGold", 5000)) * GOLD, MAX_MONEY_AMOUNT));
        cfg.chargeDeposit       = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.ChargeDeposit", false);
        cfg.collectMail         = sConfigMgr->GetOption<bool>("PlayerbotsAuctions.CollectAuctionMail", true);
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
            double const regular = double(proto->SellPrice) * cfg.priceMultiplier[proto->Quality];
            double each = regular * frand(1.0f - cfg.priceVariation / 100.0f, 1.0f + cfg.priceVariation / 100.0f);

            // Like a player, the bot goes a little below the cheapest offer. Two limits keep the bots from
            // underbidding each other down to nothing: a share of the regular price, and the vendor value.
            auto found = cheapest.find(proto->ItemId);
            if (found != cheapest.end() && cfg.undercutPercent && double(found->second) <= each)
                each = double(found->second) * (100 - cfg.undercutPercent) / 100.0;
            double const lowest = std::max(regular * (100 - cfg.maxUndercutPercent) / 100.0,
                double(proto->SellPrice) * cfg.minVendorFactor);
            if (each < lowest)
                each = lowest;

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
        if (cfg.enabled)
            LOG_INFO("server.loading", ">> PlayerbotsAuctions: the bots put their loot up for auction (place {}, every {} s).",
                cfg.place, cfg.intervalMs / IN_MILLISECONDS);
    }

    void OnUpdate(uint32 diff) override
    {
        seller.Update(diff);
    }
};

void AddSC_playerbots_auctions()
{
    new PlayerbotsAuctionsWorld();
}
