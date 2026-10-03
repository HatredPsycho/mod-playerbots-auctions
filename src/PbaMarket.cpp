/*
 * mod-playerbots-auctions - what things are worth, and what the bots remember.
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Pba.h"

namespace pba
{
    Market market;

    uint32 Market::Base(ItemTemplate const* proto)
    {
        if (proto->SellPrice)
            return proto->SellPrice;
        if (proto->Class != ITEM_CLASS_TRADE_GOODS && proto->Class != ITEM_CLASS_GEM && proto->Class != ITEM_CLASS_REAGENT)
            return 0;
        if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM || proto->HasFlag(ITEM_FLAG_CONJURED))
            return 0;
        uint32 const level = std::max<uint32>(5, proto->ItemLevel);
        return std::max<uint32>(20, level * level * (proto->Quality + 1) / 2);
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
        if (found == _sold.end() || found->second.sales < 3)
            return regular;         // a price is only "known" after a few sales
        return (regular + found->second.each) / 2.0;
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
        _dirty.insert(itemId);
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
            "PRIMARY KEY (`item`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-playerbots-auctions: what items sold for'");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_unsold` ("
            "`owner` INT UNSIGNED NOT NULL, `item` INT UNSIGNED NOT NULL, `tries` TINYINT UNSIGNED NOT NULL, "
            "PRIMARY KEY (`owner`, `item`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-playerbots-auctions: what came back unsold'");
        // Sellers that no longer exist are forgotten.
        CharacterDatabase.DirectExecute(
            "DELETE u FROM `mod_playerbots_auctions_unsold` u LEFT JOIN `characters` c ON c.`guid` = u.`owner` WHERE c.`guid` IS NULL");
        _tables = true;

        if (QueryResult result = CharacterDatabase.Query("SELECT `item`, `price`, `sales` FROM `mod_playerbots_auctions_market`"))
            do
            {
                Field* fields = result->Fetch();
                Learned& learned = _sold[fields[0].Get<uint32>()];
                learned.each = fields[1].Get<double>();
                learned.sales = fields[2].Get<uint32>();
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

    void Market::SaveMemory()
    {
        if (!_tables || _dirty.empty())
            return;
        for (uint32 const itemId : _dirty)
        {
            auto found = _sold.find(itemId);
            if (found != _sold.end())
                CharacterDatabase.Execute("REPLACE INTO `mod_playerbots_auctions_market` (`item`, `price`, `sales`) VALUES ({}, {}, {})",
                    itemId, found->second.each, found->second.sales);
        }
        _dirty.clear();
    }
}
