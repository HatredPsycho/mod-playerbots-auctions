/*
 * mod-playerbots-auctions - what things are worth, and what the bots remember.
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Pba.h"

#include <ctime>

namespace pba
{
    Market market;

    namespace
    {
        std::unordered_map<uint32, uint32> made;        // item -> value, see SetMade
    }

    void Market::ClearMade() { made.clear(); }

    void Market::SetMade(uint32 itemId, uint32 base)
    {
        if (base)
            made[itemId] = base;
        else
            made.erase(itemId);
    }

    uint32 Market::Base(ItemTemplate const* proto)
    {
        if (!made.empty())
        {
            auto found = made.find(proto->ItemId);
            if (found != made.end())
                return std::max(found->second, proto->SellPrice);
        }
        if (proto->SellPrice)
            return proto->SellPrice;
        if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM || proto->HasFlag(ITEM_FLAG_CONJURED))
            return 0;
        // A companion or a mount a vendor gives nothing for is still worth having: by its rarity.
        if (proto->Class == ITEM_CLASS_MISC && (proto->SubClass == ITEM_SUBCLASS_JUNK_PET || proto->SubClass == ITEM_SUBCLASS_JUNK_MOUNT))
            return 5000 * (proto->Quality + 1);
        // Anything else that can be traded has a value too, by its item level - gear apart: a weapon or a
        // piece of armor a vendor gives nothing for is not a normal item.
        if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
            return 0;
        uint32 const level = std::max<uint32>(5, proto->ItemLevel);
        uint32 const byLevel = std::max<uint32>(20, level * level * (proto->Quality + 1) / 2);
        // What a vendor would ask for it, were there one - dusts, essences and shards have such a price, and
        // a vendor buys at a fifth of what it sells for. Never less than the item level says, and not
        // absurdly more: some of these prices are placeholders.
        if (proto->BuyPrice)
        {
            uint32 const byPrice = proto->BuyPrice / std::max<uint32>(1, proto->BuyCount) / 5;
            return std::max(byLevel, std::min(byPrice, byLevel * 10));
        }
        return byLevel;
    }

    double Market::Regular(ItemTemplate const* proto)
    {
        return double(Base(proto)) * cfg.priceMultiplier[proto->Quality];
    }

    double Market::Value(ItemTemplate const* proto) const
    {
        double const regular = Regular(proto);
        if (!cfg.learnPrices)
            return regular;
        auto found = _sold.find(proto->ItemId);
        if (found == _sold.end() || !found->second.sales)
            return regular;
        // What was learned counts the more the more often it was seen, and fades within days: a market that
        // was full last week may be empty today.
        Learned const& learned = found->second;
        double const weight = std::min(1.0, learned.sales / 5.0);
        double const age = learned.last ? double(std::max<time_t>(0, GameTime::GetGameTime().count() - learned.last)) : 0.0;
        double const fade = std::pow(0.5, age / double(3 * DAY));
        return regular + (learned.each - regular) * weight * fade;
    }

    void Market::RecordReturn(uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!cfg.learnPrices || !proto || proto->Quality >= MAX_ITEM_QUALITY || !Base(proto))
            return;
        double const regular = Regular(proto);
        // Not below what a vendor gives: from there on the vendor is the buyer.
        double const lowest = proto->SellPrice ? double(proto->SellPrice) : regular * 0.2;
        Learned& learned = _sold[itemId];
        double const before = learned.sales ? learned.each : regular;
        learned.each = std::max(lowest, before * 0.7 + before * 0.6 * 0.3);
        ++learned.sales;
        learned.last = GameTime::GetGameTime().count();
        _dirty.insert(itemId);
        if (cfg.debug)
            LOG_INFO("module", "PlayerbotsAuctions: {} (item {}) came back unsold - the bots now take one to be worth {} copper, usually {}.",
                proto->Name1, itemId, uint64(Value(proto)), uint64(regular));
    }

    void Market::RecordSale(uint32 itemId, uint32 price, uint32 count)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto || !price || !count || proto->Quality >= MAX_ITEM_QUALITY || !Base(proto) || proto->Quality >= MAX_ITEM_QUALITY)
            return;
        // Every sale is cut down to between half and three times the calculated price before it counts,
        // so a single absurd sale cannot move the market.
        double const regular = Regular(proto);
        double const each = std::clamp(double(price) / count, regular * 0.5, regular * 3.0);
        Learned& learned = _sold[itemId];
        learned.each = learned.sales ? learned.each * 0.8 + each * 0.2 : each;      // newer sales count more
        ++learned.sales;
        learned.last = GameTime::GetGameTime().count();
        _dirty.insert(itemId);
        if (cfg.debug)
            LOG_INFO("module", "PlayerbotsAuctions: {} x{} (item {}) sold for {} copper - the bots now take one to be worth {} copper, usually {}.",
                proto->Name1, count, itemId, price, uint64(Value(proto)), uint64(regular));
    }

    void Market::LoadVendorItems()
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

        // The cheapest tool of each kind a vendor sells.
        _vendorTools.clear();
        for (uint32 const itemId : _vendorSupplies)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
                if (proto->TotemCategory)
                {
                    uint32& tool = _vendorTools[proto->TotemCategory];
                    ItemTemplate const* other = tool ? sObjectMgr->GetItemTemplate(tool) : nullptr;
                    if (!other || proto->BuyPrice < other->BuyPrice)
                        tool = itemId;
                }
    }

    namespace
    {
        uint64 TryKey(ObjectGuid::LowType owner, uint32 itemId) { return (uint64(owner) << 32) | itemId; }
    }

    uint32 Market::Tries(ObjectGuid::LowType owner, uint32 itemId) const
    {
        auto found = _tries.find(TryKey(owner, itemId));
        return found != _tries.end() ? found->second : 0;
    }

    void Market::AddTry(ObjectGuid::LowType owner, uint32 itemId)
    {
        uint32& tries = _tries[TryKey(owner, itemId)];
        tries = std::min<uint32>(tries + 1, 250);
        if (_tables)
            CharacterDatabase.Execute("REPLACE INTO `mod_playerbots_auctions_unsold` (`owner`, `item`, `tries`) VALUES ({}, {}, {})", owner, itemId, tries);
    }

    void Market::Forget(ObjectGuid::LowType owner, uint32 itemId)
    {
        if (!_tries.erase(TryKey(owner, itemId)))
            return;
        if (_tables)
            CharacterDatabase.Execute("DELETE FROM `mod_playerbots_auctions_unsold` WHERE `owner` = {} AND `item` = {}", owner, itemId);
    }

    void Market::LoadMemory()
    {
        _sold.clear();
        _dirty.clear();
        _tries.clear();
        _tables = false;
        if (!cfg.saveMemory)
            return;

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_market` ("
            "`item` INT UNSIGNED NOT NULL, `price` DOUBLE NOT NULL, `sales` INT UNSIGNED NOT NULL, "
            "`seen` BIGINT UNSIGNED NOT NULL DEFAULT 0, PRIMARY KEY (`item`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-playerbots-auctions: what items sold for'");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_unsold` ("
            "`owner` INT UNSIGNED NOT NULL, `item` INT UNSIGNED NOT NULL, `tries` TINYINT UNSIGNED NOT NULL, "
            "PRIMARY KEY (`owner`, `item`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-playerbots-auctions: what came back unsold'");
        // Sellers that no longer exist are forgotten.
        CharacterDatabase.DirectExecute(
            "DELETE u FROM `mod_playerbots_auctions_unsold` u LEFT JOIN `characters` c ON c.`guid` = u.`owner` WHERE c.`guid` IS NULL");
        // Tables of an older version do not have the time yet.
        if (QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
            "AND TABLE_NAME = 'mod_playerbots_auctions_market' AND COLUMN_NAME = 'seen'"))
            if (!result->Fetch()[0].Get<uint64>())
                CharacterDatabase.DirectExecute("ALTER TABLE `mod_playerbots_auctions_market` ADD COLUMN `seen` BIGINT UNSIGNED NOT NULL DEFAULT 0");
        _tables = true;

        if (QueryResult result = CharacterDatabase.Query("SELECT `item`, `price`, `sales`, `seen` FROM `mod_playerbots_auctions_market`"))
            do
            {
                Field* fields = result->Fetch();
                Learned& learned = _sold[fields[0].Get<uint32>()];
                learned.each = fields[1].Get<double>();
                learned.sales = fields[2].Get<uint32>();
                learned.last = time_t(fields[3].Get<uint64>());
            } while (result->NextRow());

        if (QueryResult result = CharacterDatabase.Query("SELECT `owner`, `item`, `tries` FROM `mod_playerbots_auctions_unsold`"))
            do
            {
                Field* fields = result->Fetch();
                _tries[TryKey(fields[0].Get<uint32>(), fields[1].Get<uint32>())] = fields[2].Get<uint8>();
            } while (result->NextRow());

        LOG_INFO("server.loading", ">> PlayerbotsAuctions: the bots remember the prices of {} item(s) and {} thing(s) that did not sell.",
            _sold.size(), _tries.size());
    }

    namespace
    {
        char const* const StateTable =
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_state` ("
            "`name` VARCHAR(32) NOT NULL, `value` BIGINT UNSIGNED NOT NULL, PRIMARY KEY (`name`)) "
            "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-playerbots-auctions: when the server last ran'";
        bool stateTable = false;
    }

    uint64 StateGet(std::string const& name)
    {
        if (!stateTable)
        {
            CharacterDatabase.DirectExecute(StateTable);
            stateTable = true;
        }
        std::string key = name;
        CharacterDatabase.EscapeString(key);
        if (QueryResult result = CharacterDatabase.Query("SELECT `value` FROM `mod_playerbots_auctions_state` WHERE `name` = '{}'", key))
            return result->Fetch()[0].Get<uint64>();
        return 0;
    }

    void StateSet(std::string const& name, uint64 value)
    {
        if (!stateTable)
        {
            CharacterDatabase.DirectExecute(StateTable);
            stateTable = true;
        }
        std::string key = name;
        CharacterDatabase.EscapeString(key);
        CharacterDatabase.DirectExecute("REPLACE INTO `mod_playerbots_auctions_state` (`name`, `value`) VALUES ('{}', {})", key, value);
    }

    uint64 awayFor = 0;

    uint64 AwayFor()
    {
        return awayFor;
    }

    void ResumeAuctions()
    {
        CharacterDatabase.DirectExecute(StateTable);
        stateTable = true;

        time_t const now = std::time(nullptr);
        time_t last = 0;
        if (QueryResult result = CharacterDatabase.Query("SELECT `value` FROM `mod_playerbots_auctions_state` WHERE `name` = 'last_seen'"))
            last = time_t(result->Fetch()[0].Get<uint64>());
        NoteRunning(true);
        if (last && now > last && uint64(now - last) <= 60 * DAY)
            awayFor = uint64(now - last);

        // Auctions whose item is gone: the core never loads them and never removes them either. A reset of
        // the random bots leaves hundreds behind, because it deletes the bots' items but not their auctions.
        if (cfg.enabled)
        {
            uint64 lost = 0;
            if (QueryResult result = CharacterDatabase.Query(
                    "SELECT COUNT(*) FROM `auctionhouse` ah LEFT JOIN `item_instance` ii ON ii.`guid` = ah.`itemguid` WHERE ii.`guid` IS NULL"))
                lost = result->Fetch()[0].Get<uint64>();
            if (lost)
            {
                CharacterDatabase.DirectExecute(
                    "DELETE ah FROM `auctionhouse` ah LEFT JOIN `item_instance` ii ON ii.`guid` = ah.`itemguid` WHERE ii.`guid` IS NULL");
                LOG_INFO("server.loading", ">> PlayerbotsAuctions: removed {} auction(s) whose item no longer exists (left over from deleted characters).", lost);
            }
        }

        // A restart of a minute or two is not worth it; a gap of months means the note was not kept (the module
        // was not installed), and nothing sensible can be said about it.
        if (!cfg.enabled || !cfg.pauseOffline || !last || now <= last)
            return;
        uint64 const away = uint64(now - last);
        if (away < 2 * MINUTE || away > 60 * DAY)
            return;

        uint64 auctions = 0;
        if (QueryResult result = CharacterDatabase.Query("SELECT COUNT(*) FROM `auctionhouse`"))
            auctions = result->Fetch()[0].Get<uint64>();
        CharacterDatabase.DirectExecute("UPDATE `auctionhouse` SET `time` = `time` + {}", away);
        LOG_INFO("server.loading", ">> PlayerbotsAuctions: the server was off for {} h {} min. {} auction(s) were given that time back.",
            away / HOUR, (away % HOUR) / MINUTE, auctions);
    }

    void NoteRunning(bool wait)
    {
        if (!stateTable)
            return;
        std::string const sql = Acore::StringFormat("REPLACE INTO `mod_playerbots_auctions_state` (`name`, `value`) VALUES ('last_seen', {})",
            uint64(std::time(nullptr)));
        if (wait)
            CharacterDatabase.DirectExecute(sql);
        else
            CharacterDatabase.Execute(sql);
    }

    void Market::SaveMemory()
    {
        if (!_tables || _dirty.empty())
            return;
        for (uint32 const itemId : _dirty)
        {
            auto found = _sold.find(itemId);
            if (found != _sold.end())
                CharacterDatabase.Execute("REPLACE INTO `mod_playerbots_auctions_market` (`item`, `price`, `sales`, `seen`) VALUES ({}, {}, {}, {})",
                    itemId, found->second.each, found->second.sales, uint64(found->second.last));
        }
        _dirty.clear();
    }
}
