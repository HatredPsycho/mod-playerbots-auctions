/*
 * mod-playerbots-auctions - the trading post: what the auction house has run out of, at a price.
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Bots use up most of what they gather, and some things they hardly come by at all. A player who needs
 * twenty copper ore and finds none in the auction house has nowhere to go. Every auctioneer therefore also
 * runs a trading post. It sells materials and crafted supplies - nothing else - and only those the auction
 * house is short of right now, for a multiple of what they usually go for and in small daily amounts. It is
 * a last resort, not a second market.
 *
 * The post shows its goods in the game's own auction window. The window asks the server for a list and
 * shows what comes back; for a player who chose the post, the module answers that question itself.
 */

#include "Pba.h"

#include "Chat.h"
#include "GossipDef.h"
#include "Language.h"
#include "NPCHandler.h"
#include "Opcodes.h"
#include "QueryPackets.h"
#include "ScriptedGossip.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <ctime>

namespace pba
{
    namespace
    {
        constexpr uint32 PostSender = 0x50424100;       // marks a gossip option as one of the post's
        constexpr uint32 TextBase   = 9113000;          // the texts of the gossip window; a game remembers a text by its number
        constexpr uint32 IdFlag     = 0x80000000;       // "auctions" of the post: this bit, 7 bits of count, 24 bits of item
        constexpr uint32 MaxLot     = 100;
        constexpr uint32 SellerLow  = 0xFFFFFF00;       // the "player" the goods are listed under: no character has this number

        ObjectGuid Seller()
        {
            return ObjectGuid::Create<HighGuid::Player>(SellerLow);
        }

        enum Action : uint32
        {
            ACT_BROWSE = 1,
            ACT_POST,
            ACT_INFO,
            ACT_BACK,
            ACT_OPEN,
            ACT_PICK = 100      // + which approach
        };

        // ---------------------------------------------------------------------------------- what is said

        enum Temper : uint8 { PROUD, GREEDY, HURRIED, KINDLY, WARY, TEMPERS };
        enum Approach : uint8 { FLATTER, PLEAD, WALK, COIN, NUMBERS, APPROACHES };

        char const* const Approaches[APPROACHES] =
        {
            "These are the finest goods I have seen in a long time.",
            "I am only a poor adventurer. Have a heart.",
            "Then I will try my luck at the auction house instead.",
            "I have the coin right here. Name a fair price and we are done.",
            "I know what these go for. Let us talk real numbers."
        };

        // How each kind of trader takes each approach: from -2 (the price goes up) to +2 (it comes down).
        int8 const Taste[TEMPERS][APPROACHES] =
        {
            //  flatter plead walk coin numbers
            {    2,     1,   -2,   0,   -1 },   // proud
            {    0,    -2,    2,   1,    0 },   // greedy
            {   -1,     0,    1,   2,   -2 },   // in a hurry
            {    1,     2,   -2,  -1,    0 },   // kindly
            {   -2,    -1,    0,   1,    2 }    // wary
        };

        enum : uint32
        {
            TEXT_MENU,
            TEXT_INFO,
            TEXT_INFO_PLAIN,
            TEXT_OPENING,                               // two for each temper
            TEXT_REACTION = TEXT_OPENING + TEMPERS * 2, // good, neither, bad for each temper
            TEXT_END = TEXT_REACTION + TEMPERS * 3,     // good, neither, bad
            TEXT_COUNT = TEXT_END + 3
        };

        char const* const Texts[TEXT_COUNT] =
        {
            // the menu
            "Welcome, $N. The auction hall is open, as always.$B$BAnd if the hall cannot help you, the Trading Post might: "
            "we keep what the auctions have run out of. At a price.",

            // what the post is
            "The Trading Post sells materials and crafted supplies - ore, herbs, cloth, leather, gems, potions, food, scrolls "
            "and the like - but only what the auction house is short of right now. What the hall has plenty of, we do not carry."
            "$B$BOur prices are well above what things usually go for, and nobody gets more than a little of one thing a day. "
            "What you buy is sent to your mailbox.$B$BYou may haggle with me once a day. How that goes decides your prices until "
            "tomorrow - so mind who you are talking to before you open your mouth.",

            "The Trading Post sells materials and crafted supplies - ore, herbs, cloth, leather, gems, potions, food, scrolls "
            "and the like - but only what the auction house is short of right now. What the hall has plenty of, we do not carry."
            "$B$BOur prices are well above what things usually go for, and nobody gets more than a little of one thing a day. "
            "What you buy is sent to your mailbox.",

            // how the trader is today: proud
            "You will not find finer goods between here and Booty Bay. Every crate in this post was chosen by my own hand, "
            "and I do not care to hear otherwise.$B$BNow. You wished to discuss my prices?",
            "Careful where you put your fingers. These goods have a reputation, and so do I.$B$BYou have something to say "
            "about what they cost, I take it.",
            // greedy
            "Ah, a customer! Come in, come in. Do not walk off now - I have exactly what you need, and nobody else does today."
            "$B$BLet us talk about what it will cost you.",
            "Gold is gold, friend, and I would hate to see yours end up in somebody else's purse.$B$BSo. What will it take?",
            // in a hurry
            "Make it quick. The caravan leaves within the hour and I have forty crates left to count.$B$BPrices? Fine. Talk.",
            "Yes, yes, what is it? I have three ledgers open and no time for a chat.$B$BYou want to haggle. Of course you do. Go on.",
            // kindly
            "Oh, hello there, $N! You look like you have had a long road. Come, stand out of the draught.$B$BIt has been a slow "
            "day and I do enjoy a bit of company. Now, what can I do for you, dear?",
            "A friendly face at last! Do you know, you remind me of someone I used to travel with.$B$BPrices, yes, yes. "
            "We will get to that. Tell me how you have been keeping.",
            // wary
            "Hm. Another one. The last adventurer who came in here smiling walked out with my scales.$B$BState your business, "
            "and keep it plain.",
            "I have heard every story there is, stranger. Twice.$B$BIf you have something to say about my prices, say it straight.",

            // how the trader takes what was said: proud
            "Ha! At last, somebody with eyes in their head. For a customer of taste I can do a little better.$B$BWas there more?",
            "Hm. That is as may be. The price stands for now.$B$BWas there more?",
            "You come into MY post and say that to my face? The price just went up.$B$BChoose your next words with care.",
            // greedy
            "No, no, no - wait! Let us not be hasty. I am sure we can shave a little something off.$B$BWhat else?",
            "Is that so. Well. It changes very little.$B$BWhat else?",
            "Then you are in the wrong shop, friend. Wasting my time costs extra.$B$BTry again.",
            // in a hurry
            "Finally, somebody who gets to the point. Fine. Less for you, less counting for me.$B$BNext.",
            "Yes, yes. Anything else? I am losing daylight.$B$BNext.",
            "I do not have time for this! Every minute you talk costs you.$B$BNext, and make it short.",
            // kindly
            "Oh, you poor thing. I really should not, but... let me see what I can do for you.$B$BGo on, dear.",
            "That is nice, dear. Now, where were we?$B$BGo on.",
            "Well! There is no call for that tone. And here I thought we were getting along.$B$BGo on, then.",
            // wary
            "Hm. That I can respect. A little off, then.$B$BAnything else?",
            "Words. I have heard them before. We will see.$B$BAnything else?",
            "There it is. Every thief starts just like that. My price goes up when my hackles do.$B$BAnything else?",

            // how it ended
            "Done. You drive a hard bargain, $N. Those are my prices for you today - do not tell the others.",
            "Well, that is that. My prices for you today stand about where they stood. Have a look.",
            "I have had about enough. These are my prices for you today, and you can thank your own tongue for them."
        };

        // ---------------------------------------------------------------------------------- what it sells

        struct Ware
        {
            ItemTemplate const* proto = nullptr;
            bool made = false;          // a potion, a meal, a scroll, a cut gem: the living of a crafter
            std::wstring name;          // in small letters, to search in
        };

        struct Offer
        {
            uint32 ware = 0;
            uint32 each = 0;            // the asking price of one piece, before the haggling
        };

        /// What the post next to one auction house has on its shelves: what that house is short of.
        struct Stock
        {
            time_t built = 0;
            time_t wanted = 0;                              // when somebody last looked
            std::vector<Offer> offers;
            std::unordered_map<uint32, uint32> byItem;      // item -> place in offers
        };

        struct Visitor
        {
            ObjectGuid npc;
            AuctionHouseObject* house = nullptr;
            bool atPost = false;        // the auction window on its screen shows the post
        };

        /// What a player did at the post today.
        struct Today
        {
            uint32 day = 0;
            uint8 round = 0;
            int8 score = 0;
            uint8 used = 0;             // approaches tried, as bits
            bool done = false;
            float factor = 1.0f;        // what the haggling made of the asking price
            std::unordered_map<uint32, uint32> bought;
        };

        std::mutex lock;        // the list is asked for on the thread of the player's map; everything below is behind this
        std::vector<Ware> wares;
        std::map<AuctionHouseObject*, Stock> stocks;
        std::unordered_map<ObjectGuid::LowType, Visitor> visitors;
        std::unordered_map<ObjectGuid::LowType, Today> todays;
        bool table = false;
        thread_local bool passing = false;      // the real auction window is being opened; the menu stays out of it

        uint32 DayNow()
        {
            std::tm const time = Acore::Time::TimeBreakdown(GameTime::GetGameTime().count());
            return uint32(time.tm_year + 1900) * 1000 + uint32(time.tm_yday);
        }

        uint32 Mix(uint32 a, uint32 b, uint32 c)
        {
            uint32 hash = a * 2654435761u + b * 40503u + c * 2246822519u;
            hash ^= hash >> 15;
            hash *= 2246822519u;
            hash ^= hash >> 13;
            hash *= 3266489917u;
            hash ^= hash >> 16;
            return hash;
        }

        Today& TodayOf(ObjectGuid::LowType player)
        {
            Today& today = todays[player];
            uint32 const day = DayNow();
            if (today.day != day)
            {
                today = Today();
                today.day = day;
            }
            return today;
        }

        void Remember(ObjectGuid::LowType player, uint32 item, uint32 day, uint32 value)
        {
            if (table)
                CharacterDatabase.Execute("REPLACE INTO `mod_playerbots_auctions_post` (`player`, `item`, `day`, `bought`) VALUES ({}, {}, {}, {})",
                    player, item, day, value);
        }

        uint32 DailyLimit(Ware const& ware)
        {
            uint32 const limit = ware.made ? cfg.postDailyMade : cfg.postDailyRaw;
            return std::max<uint32>(1, limit);
        }

        bool OfAPerson(Player* player)
        {
            return player && player->GetSession() && !GET_PLAYERBOT_AI(player) &&
                !sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId());
        }

        char const* PostName(Creature* creature)
        {
            FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creature->GetFaction());
            if (faction && (faction->ourMask & FACTION_MASK_ALLIANCE))
                return "Alliance Trading Post";
            if (faction && (faction->ourMask & FACTION_MASK_HORDE))
                return "Horde Trading Post";
            return "Goblin Trading Post";
        }

        std::wstring SmallLetters(std::string const& text)
        {
            std::wstring wide;
            if (!Utf8toWStr(text, wide))
                return std::wstring();
            wstrToLower(wide);
            return wide;
        }

        void AddLoot(char const* tableName, std::unordered_set<uint32>& into)
        {
            if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT `Item` FROM `{}` WHERE `Reference` = 0", tableName))
                do
                    into.insert((*result)[0].Get<uint32>());
                while (result->NextRow());
        }

        /// What the world really gives: what drops, is gathered, fished, skinned or comes of taking things apart,
        /// and what a recipe turns that into. The item table of a server holds far more than that - things that
        /// were never finished, things of other expansions - and a post that sold those would hand out what
        /// nobody is meant to have.
        std::unordered_set<uint32> Obtainable(std::unordered_set<uint32>& made)
        {
            std::unordered_set<uint32> found;
            for (char const* tableName : { "creature_loot_template", "gameobject_loot_template", "reference_loot_template",
                "item_loot_template", "fishing_loot_template", "skinning_loot_template", "disenchant_loot_template",
                "prospecting_loot_template", "milling_loot_template", "pickpocketing_loot_template" })
                AddLoot(tableName, found);

            struct Recipe
            {
                uint32 product;
                std::vector<uint32> reagents;
            };
            std::vector<Recipe> recipes;
            for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
                if (!info || !info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL))
                    continue;
                bool const crafts = info->Effects[EFFECT_0].Effect == SPELL_EFFECT_CREATE_ITEM;
                bool const scroll = info->Effects[EFFECT_0].Effect == SPELL_EFFECT_ENCHANT_ITEM && info->IsAbilityOfSkillType(SKILL_ENCHANTING) &&
                    (info->EquippedItemClass == ITEM_CLASS_WEAPON || info->EquippedItemClass == ITEM_CLASS_ARMOR);
                if ((!crafts && !scroll) || !info->Effects[EFFECT_0].ItemType)
                    continue;
                Recipe recipe;
                recipe.product = info->Effects[EFFECT_0].ItemType;
                for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                    if (info->Reagent[i] > 0 && info->ReagentCount[i])
                        recipe.reagents.push_back(uint32(info->Reagent[i]));
                if (!recipe.reagents.empty())
                    recipes.push_back(std::move(recipe));
            }

            // Bars come of ore and bolts come of bars: a few rounds until nothing new turns up.
            for (uint32 round = 0; round < 8; ++round)
            {
                bool more = false;
                for (Recipe const& recipe : recipes)
                {
                    if (made.count(recipe.product))
                        continue;
                    bool all = true;
                    for (uint32 const reagent : recipe.reagents)
                        if (!found.count(reagent) && !market.IsVendorItem(reagent))
                        {
                            all = false;
                            break;
                        }
                    if (all)
                    {
                        made.insert(recipe.product);
                        found.insert(recipe.product);
                        more = true;
                    }
                }
                if (!more)
                    break;
            }
            return found;
        }

        bool IsWare(ItemTemplate const& proto, std::unordered_set<uint32> const& found, std::unordered_set<uint32> const& made, bool& crafted)
        {
            if (proto.ItemId >= (1u << 24) || proto.Quality < ITEM_QUALITY_NORMAL || proto.Quality > cfg.postMaxQuality || proto.Quality >= MAX_ITEM_QUALITY)
                return false;
            if (proto.Bonding != NO_BIND || proto.Duration || proto.HasFlag(ITEM_FLAG_CONJURED) || proto.HasFlag(ITEM_FLAG_HAS_LOOT))
                return false;
            if (proto.RequiredLevel > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) || proto.Name1.empty())
                return false;
            if (cfg.postExcluded.count(proto.ItemId) || cfg.excludedItems.count(proto.ItemId))
                return false;
            // What a vendor sells to anybody at any time is not missing anywhere.
            if (market.IsVendorSupply(proto.ItemId) || !found.count(proto.ItemId))
                return false;

            switch (proto.Class)
            {
                case ITEM_CLASS_TRADE_GOODS:
                    // Ore asks for jewelcrafting and herbs for inscription in the item table - to prospect and
                    // to mill them. Everybody can carry and use them, so that is not asked about here.
                    crafted = false;
                    break;
                case ITEM_CLASS_GEM:
                    if (proto.RequiredSkill)
                        return false;       // a cut only a jeweler can wear
                    crafted = proto.GemProperties != 0;
                    break;
                case ITEM_CLASS_CONSUMABLE:
                    // Only what a crafter makes: potions, elixirs, flasks, meals, scrolls, armor kits and the
                    // like. Nothing that needs a profession to be used - bandages, an engineer's toys.
                    if (!made.count(proto.ItemId) || proto.RequiredSkill || proto.SubClass == ITEM_SUBCLASS_BANDAGE)
                        return false;
                    crafted = true;
                    break;
                default:
                    return false;
            }

            if (!Market::Base(&proto))
                return false;
            std::string const name = Lower(proto.Name1);
            for (std::string const& part : cfg.excludedNameParts)
                if (!part.empty() && name.find(part) != std::string::npos)
                    return false;
            return true;
        }

        /// What one auction house is short of right now, and what the post asks for it. On the world's thread.
        void BuildStock(AuctionHouseObject* house, Stock& stock)
        {
            std::unordered_map<uint32, uint32> held;
            for (auto const& entry : house->GetAuctions())
                if (AuctionEntry const* auction = entry.second)
                    held[auction->item_template] += std::max<uint32>(1, auction->itemCount);

            stock.offers.clear();
            stock.byItem.clear();
            for (uint32 i = 0; i < wares.size(); ++i)
            {
                Ware const& ware = wares[i];
                if (cfg.postScarce)
                {
                    // Materials are needed by the handful: a single piece in the hall helps nobody. What a
                    // crafter makes is bought one at a time, and one is enough to send the player to the hall.
                    uint32 const enough = ware.made ? 1 : std::min<uint32>(cfg.postScarce, std::max<uint32>(1, ware.proto->GetMaxStackSize()));
                    auto found = held.find(ware.proto->ItemId);
                    if (found != held.end() && found->second >= enough)
                        continue;
                }
                double const asked = market.Value(ware.proto) * (ware.made ? cfg.postPriceMade : cfg.postPriceRaw);
                Offer offer;
                offer.ware = i;
                offer.each = std::max<uint32>(HumanPrice(asked), 1);
                stock.byItem[ware.proto->ItemId] = uint32(stock.offers.size());
                stock.offers.push_back(offer);
            }
            stock.built = GameTime::GetGameTime().count();
        }

        uint32 PriceOf(Offer const& offer, Ware const& ware, float factor)
        {
            // Never so cheap that a vendor would pay more for it.
            return std::max<uint32>(HumanPrice(double(offer.each) * factor), ware.proto->SellPrice + 1);
        }

        uint32 Total(uint32 each, uint32 count)
        {
            return uint32(std::min<uint64>(uint64(each) * count, MAX_MONEY_AMOUNT));
        }

        float FactorOf(Today const& today)
        {
            return cfg.postHaggle && today.done ? today.factor : 1.0f;
        }

        // ---------------------------------------------------------------------------------- the gossip window

        void SendText(WorldSession* session, uint32 id)
        {
            WorldPacket data(SMSG_NPC_TEXT_UPDATE, 600);
            data << uint32(TextBase + id);
            for (uint8 i = 0; i < MAX_GOSSIP_TEXT_OPTIONS; ++i)
            {
                data << float(i ? 0.0f : 1.0f);
                data << (i ? "" : Texts[id]);
                data << (i ? "" : Texts[id]);
                data << uint32(0);
                for (uint8 j = 0; j < MAX_GOSSIP_TEXT_EMOTES; ++j)
                {
                    data << uint32(0);
                    data << uint32(0);
                }
            }
            session->SendPacket(&data);
        }

        void ShowMenu(Player* player, Creature* creature)
        {
            // The menu was not opened the usual way; the server checks this when an option is chosen.
            creature->LastUsedScriptID = creature->GetScriptId();
            ClearGossipMenuFor(player);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Browse the auction house.", PostSender, ACT_BROWSE);
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, std::string("Buy at the ") + PostName(creature) + ".", PostSender, ACT_POST);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "What is the Trading Post?", PostSender, ACT_INFO);
            SendGossipMenuFor(player, TextBase + TEXT_MENU, creature->GetGUID());
        }

        /// Three of the five things one can say, different ones each round: the best one is not always on offer.
        void Offered(ObjectGuid::LowType player, Today const& today, uint8 out[3])
        {
            uint8 order[APPROACHES] = { FLATTER, PLEAD, WALK, COIN, NUMBERS };
            for (uint32 i = APPROACHES - 1; i > 0; --i)
                std::swap(order[i], order[Mix(player, today.day, 300 + today.round * 10 + i) % (i + 1)]);
            for (uint32 i = 0; i < 3; ++i)
                out[i] = order[i];
        }

        Temper TemperOf(ObjectGuid::LowType player, Today const& today)
        {
            return Temper(Mix(player, today.day, 77) % TEMPERS);
        }

        void ShowRound(Player* player, Creature* creature, Today const& today, uint32 text)
        {
            uint8 offered[3];
            Offered(player->GetGUID().GetCounter(), today, offered);
            ClearGossipMenuFor(player);
            for (uint8 const approach : offered)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Approaches[approach], PostSender, uint32(ACT_PICK) + approach);
            SendGossipMenuFor(player, TextBase + text, creature->GetGUID());
        }

        void OpenPost(Player* player, Creature* creature)
        {
            WorldSession* session = player->GetSession();
            if (player->GetLevel() < sWorld->getIntConfig(CONFIG_AUCTION_LEVEL_REQ))
            {
                CloseGossipMenuFor(player);
                ChatHandler(session).SendNotification(LANG_AUCTION_REQ, sWorld->getIntConfig(CONFIG_AUCTION_LEVEL_REQ));
                return;
            }
            AuctionHouseEntry const* entry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(creature->GetFaction());
            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(creature->GetFaction());
            if (!entry || !house)
            {
                CloseGossipMenuFor(player);
                return;
            }

            {
                std::lock_guard<std::mutex> guard(lock);
                Visitor& visitor = visitors[player->GetGUID().GetCounter()];
                visitor.npc = creature->GetGUID();
                visitor.house = house;
                visitor.atPost = true;
                Stock& stock = stocks[house];
                time_t const now = GameTime::GetGameTime().count();
                stock.wanted = now;
                if (!stock.built || now - stock.built >= 5)
                    BuildStock(house, stock);
            }

            if (player->HasUnitState(UNIT_STATE_DIED))
                player->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);
            CloseGossipMenuFor(player);
            WorldPacket data(MSG_AUCTION_HELLO, 12);
            data << creature->GetGUID();
            data << uint32(entry->houseId);
            data << uint8(1);
            session->SendPacket(&data);
        }

        void Haggle(Player* player, Creature* creature, uint8 approach)
        {
            ObjectGuid::LowType const low = player->GetGUID().GetCounter();
            uint32 text;
            bool over = false;
            float factor = 1.0f;
            {
                std::lock_guard<std::mutex> guard(lock);
                Today& today = TodayOf(low);
                if (today.done)
                {
                    over = true;
                    text = 0;
                }
                else
                {
                    uint8 offered[3];
                    Offered(low, today, offered);
                    if (std::find(offered, offered + 3, approach) == offered + 3)
                        return;     // not one of the three on the screen

                    Temper const temper = TemperOf(low, today);
                    int32 effect = Taste[temper][approach];
                    if (today.used & (1 << approach))
                        --effect;   // the same line twice: it has heard that one
                    uint32 const luck = Mix(low, today.day, 500 + today.round) % 100;
                    if (luck < 12)
                        ++effect;   // a good moment
                    else if (luck >= 88)
                        --effect;   // a bad one
                    effect = std::clamp(effect, -2, 2);
                    today.used |= uint8(1 << approach);
                    today.score = int8(today.score + effect);
                    ++today.round;

                    if (today.round >= 3)
                    {
                        today.done = true;
                        today.factor = std::clamp(1.0f - 0.055f * float(today.score), 0.67f, 1.33f);
                        factor = today.factor;
                        Remember(low, 0, today.day, uint32(std::lround(today.factor * 1000.0f)));
                        text = TEXT_END + (today.score >= 2 ? 0 : today.score <= -2 ? 2 : 1);
                        over = true;
                    }
                    else
                    {
                        text = TEXT_REACTION + temper * 3 + (effect > 0 ? 0 : effect < 0 ? 2 : 1);
                        ShowRound(player, creature, today, text);
                    }
                }
            }
            if (!over)
                return;
            if (!text)
            {
                OpenPost(player, creature);     // settled earlier today
                return;
            }

            int32 const percent = int32(std::lround((factor - 1.0f) * 100.0f));
            std::string result = percent < 0 ? Acore::StringFormat("{}% below", -percent) : percent > 0 ? Acore::StringFormat("{}% above", percent) : std::string("exactly at");
            ChatHandler(player->GetSession()).PSendSysMessage("{}: your prices until tomorrow are {} the asking price.", PostName(creature), result);
            ClearGossipMenuFor(player);
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, "Show me what you have.", PostSender, ACT_OPEN);
            SendGossipMenuFor(player, TextBase + text, creature->GetGUID());
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} haggled at the trading post - prices at {}% of what is asked, until tomorrow.",
                    player->GetName(), int32(std::lround(factor * 100.0f)));
        }

        // ---------------------------------------------------------------------------------- the auction window

        struct Row
        {
            Ware const* ware;
            uint32 count;
            uint32 total;
        };

        int Compare(uint32 column, Row const& a, Row const& b)
        {
            // The same way round as the auction house sorts, so the arrows of the window point the usual way.
            switch (column)
            {
                case AUCTION_SORT_MINLEVEL:
                    return a.ware->proto->RequiredLevel > b.ware->proto->RequiredLevel ? -1 : a.ware->proto->RequiredLevel < b.ware->proto->RequiredLevel ? 1 : 0;
                case AUCTION_SORT_RARITY:
                    return a.ware->proto->Quality < b.ware->proto->Quality ? -1 : a.ware->proto->Quality > b.ware->proto->Quality ? 1 : 0;
                case AUCTION_SORT_ITEM:
                {
                    int const names = a.ware->name.compare(b.ware->name);
                    return names > 0 ? -1 : names < 0 ? 1 : 0;
                }
                case AUCTION_SORT_MINBIDBUY:
                case AUCTION_SORT_BID:
                    return a.total > b.total ? -1 : a.total < b.total ? 1 : 0;
                case AUCTION_SORT_BUYOUT:
                case AUCTION_SORT_BUYOUT_2:
                    return a.total < b.total ? -1 : a.total > b.total ? 1 : 0;
                case AUCTION_SORT_STACK:
                    return a.count < b.count ? -1 : a.count > b.count ? 1 : 0;
                default:
                    return 0;
            }
        }

        void WriteRow(WorldPacket& data, Row const& row)
        {
            ItemTemplate const* proto = row.ware->proto;
            data << uint32(IdFlag | (row.count << 24) | proto->ItemId);
            data << uint32(proto->ItemId);
            for (uint8 i = 0; i < MAX_INSPECTED_ENCHANTMENT_SLOT; ++i)
            {
                data << uint32(0);
                data << uint32(0);
                data << uint32(0);
            }
            data << int32(0);                                       // no random property
            data << uint32(0);
            data << uint32(row.count);
            data << uint32(proto->Spells[0].SpellCharges);
            data << uint32(0);
            // As close to an ordinary auction as it gets - a seller with a name, a lowest bid just under the
            // buyout - so that the window offers the buyout the way it always does.
            data << Seller();
            data << uint32(row.total > 1 ? row.total - 1 : row.total);
            data << uint32(0);
            data << uint32(row.total);
            data << uint32(24 * HOUR * IN_MILLISECONDS);
            data << uint64(0);
            data << uint32(0);
        }

        /// The window asks what there is. Runs on the thread of the player's map.
        void ListItems(WorldSession* session, WorldPacket& recvData)
        {
            Player* player = session->GetPlayer();
            std::string searched;
            uint8 levelMin, levelMax, usable, getAll, sortCount;
            uint32 listFrom, slot, itemClass, itemSubClass, quality;
            ObjectGuid guid;
            recvData >> guid >> listFrom >> searched >> levelMin >> levelMax >> slot >> itemClass >> itemSubClass >> quality >> usable >> getAll >> sortCount;
            if (sortCount > AUCTION_SORT_MAX)
                return;
            std::vector<std::pair<uint8, bool>> sorting;
            for (uint8 i = 0; i < sortCount; ++i)
            {
                uint8 mode, descending;
                recvData >> mode >> descending;
                sorting.emplace_back(mode, descending == 1);
            }
            std::wstring const wanted = SmallLetters(searched);
            if (!searched.empty() && wanted.empty())
                return;
            int const locale = session->GetSessionDbLocaleIndex();

            // Held to the end: the rows point into the list of wares, which a reload of the settings rebuilds.
            std::lock_guard<std::mutex> guard(lock);
            std::vector<Row> rows;
            {
                ObjectGuid::LowType const low = player->GetGUID().GetCounter();
                auto visitor = visitors.find(low);
                if (visitor == visitors.end() || !visitor->second.house)
                    return;
                Stock& stock = stocks[visitor->second.house];
                stock.wanted = GameTime::GetGameTime().count();
                Today const& today = TodayOf(low);
                float const factor = FactorOf(today);

                for (Offer const& offer : stock.offers)
                {
                    Ware const& ware = wares[offer.ware];
                    ItemTemplate const* proto = ware.proto;
                    if (itemClass != 0xffffffff && proto->Class != itemClass)
                        continue;
                    if (itemSubClass != 0xffffffff && proto->SubClass != itemSubClass)
                        continue;
                    if (slot != 0xffffffff && proto->InventoryType != slot)
                        continue;
                    if (quality != 0xffffffff && proto->Quality < quality)
                        continue;
                    if (levelMin && (proto->RequiredLevel < levelMin || (levelMax && proto->RequiredLevel > levelMax)))
                        continue;
                    if (usable && player->CanUseItem(proto) != EQUIP_ERR_OK)
                        continue;
                    if (!wanted.empty())
                    {
                        bool match = ware.name.find(wanted) != std::wstring::npos;
                        if (!match && locale > LOCALE_enUS)
                            if (ItemLocale const* names = sObjectMgr->GetItemLocale(proto->ItemId))
                            {
                                std::string name = proto->Name1;
                                ObjectMgr::GetLocaleString(names->Name, locale, name);
                                match = SmallLetters(name).find(wanted) != std::wstring::npos;
                            }
                        if (!match)
                            continue;
                    }

                    auto had = today.bought.find(proto->ItemId);
                    uint32 const bought = had != today.bought.end() ? had->second : 0;
                    uint32 const limit = DailyLimit(ware);
                    if (bought >= limit)
                        continue;
                    // One piece, a handful, and all that is left for today.
                    uint32 const most = std::min({ limit - bought, proto->GetMaxStackSize(), MaxLot });
                    uint32 const each = PriceOf(offer, ware, factor);
                    uint32 last = 0;
                    for (uint32 const size : { uint32(1), uint32(5), most })
                        if (size <= most && size > last)
                        {
                            rows.push_back({ &ware, size, Total(each, size) });
                            last = size;
                        }
                }
            }

            if (!sorting.empty())
                std::stable_sort(rows.begin(), rows.end(), [&sorting](Row const& a, Row const& b)
                {
                    for (auto const& order : sorting)
                        if (int const result = Compare(order.first, a, b))
                            return (result < 0) == order.second;
                    return false;
                });

            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: trading post - {} asks for a list (\"{}\", class {}, kind {}, from {}): {} offer(s), {} copper in the purse.",
                    player->GetName(), searched, int32(itemClass), int32(itemSubClass), listFrom, rows.size(), player->GetMoney());
            WorldPacket data(SMSG_AUCTION_LIST_RESULT, 12 + 140 * MAX_AUCTIONS_PER_PAGE);
            data << uint32(0);
            uint32 count = 0;
            uint32 const most = getAll ? uint32(MAX_GETALL_RETURN) : uint32(MAX_AUCTIONS_PER_PAGE);
            for (size_t i = getAll ? 0 : listFrom; i < rows.size() && count < most; ++i, ++count)
                WriteRow(data, rows[i]);
            data.put<uint32>(0, count);
            data << uint32(rows.size());
            data << uint32(AUCTION_SEARCH_DELAY);
            session->SendPacket(&data);
        }

        void Refuse(WorldSession* session, uint32 id, AuctionError error)
        {
            session->SendAuctionCommandResult(id, AUCTION_PLACE_BID, error);
        }

        /// A price was clicked. Runs on the world's thread.
        void Buy(WorldSession* session, ObjectGuid npcGuid, uint32 id, uint32 paid)
        {
            Player* player = session->GetPlayer();
            uint32 const itemId = id & 0x00FFFFFF;
            uint32 const count = (id >> 24) & 0x7F;
            ObjectGuid::LowType const low = player->GetGUID().GetCounter();
            Creature* creature = player->GetNPCIfCanInteractWith(npcGuid, UNIT_NPC_FLAG_AUCTIONEER);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!creature || !proto || !count || !cfg.enabled || !cfg.post || !OfAPerson(player))
            {
                Refuse(session, id, ERR_AUCTION_ITEM_NOT_FOUND);
                return;
            }

            uint32 cost = 0;
            uint32 day = 0, boughtNow = 0;
            {
                std::lock_guard<std::mutex> guard(lock);
                auto visitor = visitors.find(low);
                if (visitor == visitors.end() || !visitor->second.atPost || visitor->second.npc != npcGuid || !visitor->second.house)
                {
                    Refuse(session, id, ERR_AUCTION_ITEM_NOT_FOUND);
                    return;
                }
                Stock& stock = stocks[visitor->second.house];
                auto place = stock.byItem.find(itemId);
                if (place == stock.byItem.end())
                {
                    Refuse(session, id, ERR_AUCTION_ITEM_NOT_FOUND);    // the hall has it again
                    return;
                }
                Offer const& offer = stock.offers[place->second];
                Ware const& ware = wares[offer.ware];
                Today& today = TodayOf(low);
                uint32 const bought = today.bought[itemId];
                if (bought + count > DailyLimit(ware) || count > proto->GetMaxStackSize())
                {
                    Refuse(session, id, ERR_AUCTION_ITEM_NOT_FOUND);
                    return;
                }
                cost = Total(PriceOf(offer, ware, FactorOf(today)), count);
                // The price on the screen is the price: if things moved since the list was sent, nothing is sold.
                // A bid of the lowest amount buys as well; that is one copper less.
                if (paid != cost && paid + 1 != cost)
                {
                    if (cfg.debug)
                        LOG_INFO("module", "PlayerbotsAuctions: trading post - {} offered {} copper for {} x {} (item {}), the price is {}: not sold.",
                            player->GetName(), paid, count, proto->Name1, itemId, cost);
                    Refuse(session, id, ERR_AUCTION_ITEM_NOT_FOUND);
                    return;
                }
                cost = paid;
                if (!player->HasEnoughMoney(cost))
                {
                    Refuse(session, id, ERR_AUCTION_NOT_ENOUGHT_MONEY);
                    return;
                }
                today.bought[itemId] = bought + count;
                day = today.day;
                boughtNow = bought + count;
            }

            Item* item = Item::CreateItem(itemId, count, player);
            if (!item)
            {
                std::lock_guard<std::mutex> guard(lock);
                TodayOf(low).bought[itemId] = boughtNow - count;
                Refuse(session, id, ERR_AUCTION_DATABASE_ERROR);
                return;
            }

            char const* post = PostName(creature);
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            player->ModifyMoney(-int32(cost));
            item->SaveToDB(trans);
            std::string subject = count > 1 ? Acore::StringFormat("{} ({})", proto->Name1, count) : proto->Name1;
            MailDraft(subject, Acore::StringFormat("From the {}, as ordered. A pleasure doing business.", post))
                .AddItem(item)
                .SendMailTo(trans, MailReceiver(player, low), MailSender(MAIL_CREATURE, creature->GetEntry()), MAIL_CHECK_MASK_HAS_BODY, 0);
            player->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);
            Remember(low, itemId, day, boughtNow);

            // What the auction house tells the game after a buyout, so the window behaves as it always does.
            if (AuctionHouseEntry const* entry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(creature->GetFaction()))
                session->SendAuctionBidderNotification(entry->houseId, id, player->GetGUID(), 0, 0, itemId);
            session->SendAuctionCommandResult(id, AUCTION_PLACE_BID, ERR_AUCTION_OK);
            LOG_INFO("module", "PlayerbotsAuctions: {} bought {} x {} (item {}) at the trading post for {} copper.",
                player->GetName(), count, proto->Name1, itemId, cost);
        }
    }

    // ---------------------------------------------------------------------------------------- from outside

    void LoadPost()
    {
        std::lock_guard<std::mutex> guard(lock);
        wares.clear();
        stocks.clear();
        for (auto& visitor : visitors)
            visitor.second.atPost = false;

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_post` ("
            "`player` INT UNSIGNED NOT NULL, `item` INT UNSIGNED NOT NULL, `day` INT UNSIGNED NOT NULL, `bought` INT UNSIGNED NOT NULL, "
            "PRIMARY KEY (`player`, `item`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
            "COMMENT='mod-playerbots-auctions: what players bought at the trading post today (item 0: how the haggling went)'");
        table = true;
        uint32 const day = DayNow();
        CharacterDatabase.DirectExecute("DELETE FROM `mod_playerbots_auctions_post` WHERE `day` <> {}", day);
        todays.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT `player`, `item`, `bought` FROM `mod_playerbots_auctions_post` WHERE `day` = {}", day))
            do
            {
                Field* fields = result->Fetch();
                Today& today = todays[fields[0].Get<uint32>()];
                today.day = day;
                uint32 const item = fields[1].Get<uint32>();
                if (item)
                    today.bought[item] = fields[2].Get<uint32>();
                else
                {
                    today.done = true;
                    today.round = 3;
                    today.factor = std::clamp(float(fields[2].Get<uint32>()) / 1000.0f, 0.67f, 1.33f);
                }
            } while (result->NextRow());

        if (!cfg.post)
            return;

        std::unordered_set<uint32> made;
        std::unordered_set<uint32> const found = Obtainable(made);
        uint32 crafted = 0;
        for (auto const& entry : *sObjectMgr->GetItemTemplateStore())
        {
            ItemTemplate const& proto = entry.second;
            if (proto.Class != ITEM_CLASS_TRADE_GOODS && proto.Class != ITEM_CLASS_GEM && proto.Class != ITEM_CLASS_CONSUMABLE)
                continue;
            bool isMade = false;
            if (!IsWare(proto, found, made, isMade))
                continue;
            Ware ware;
            ware.proto = &proto;
            ware.made = isMade;
            ware.name = SmallLetters(proto.Name1);
            wares.push_back(std::move(ware));
            if (isMade)
                ++crafted;
        }
        std::sort(wares.begin(), wares.end(), [](Ware const& a, Ware const& b)
        {
            if (a.proto->Class != b.proto->Class)
                return a.proto->Class > b.proto->Class;     // materials first
            if (a.proto->SubClass != b.proto->SubClass)
                return a.proto->SubClass < b.proto->SubClass;
            return a.name < b.name;
        });
        LOG_INFO("server.loading", ">> PlayerbotsAuctions: the trading posts know {} material(s) and {} crafted supply(ies) to sell when the auction house is short of them.",
            wares.size() - crafted, crafted);
    }

    void PostUpdate(uint32 diff)
    {
        static uint32 timer = 0;
        timer += diff;
        if (timer < 15 * IN_MILLISECONDS)
            return;
        timer = 0;
        if (!cfg.enabled || !cfg.post)
            return;

        // The shelves follow the auction house while somebody is looking at them.
        std::lock_guard<std::mutex> guard(lock);
        time_t const now = GameTime::GetGameTime().count();
        for (auto& entry : stocks)
            if (entry.second.wanted && now - entry.second.wanted < 10 * MINUTE)
                BuildStock(entry.first, entry.second);
    }

    bool PostHello(WorldSession const* session, ObjectGuid /*guid*/, Creature* creature)
    {
        if (passing || !cfg.enabled || !cfg.post || !creature || !session)
            return true;
        Player* player = session->GetPlayer();
        if (!OfAPerson(player))
            return true;

        {
            std::lock_guard<std::mutex> guard(lock);
            Visitor& visitor = visitors[player->GetGUID().GetCounter()];
            visitor.npc = creature->GetGUID();
            visitor.house = sAuctionMgr->GetAuctionsMap(creature->GetFaction());
            visitor.atPost = false;
        }
        ShowMenu(player, creature);
        return false;
    }

    bool PostGossip(Player* player, Creature* creature, uint32 sender, uint32 action)
    {
        if (sender != PostSender || !player || !creature)
            return false;
        if (!cfg.enabled || !cfg.post || !creature->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER) || !OfAPerson(player))
        {
            CloseGossipMenuFor(player);
            return true;
        }

        ObjectGuid::LowType const low = player->GetGUID().GetCounter();
        switch (action)
        {
            case ACT_BROWSE:
            {
                {
                    std::lock_guard<std::mutex> guard(lock);
                    visitors[low].atPost = false;
                }
                CloseGossipMenuFor(player);
                passing = true;
                player->GetSession()->SendAuctionHello(creature->GetGUID(), creature);
                passing = false;
                break;
            }
            case ACT_INFO:
                ClearGossipMenuFor(player);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "I see.", PostSender, ACT_BACK);
                SendGossipMenuFor(player, TextBase + (cfg.postHaggle ? TEXT_INFO : TEXT_INFO_PLAIN), creature->GetGUID());
                break;
            case ACT_BACK:
                ShowMenu(player, creature);
                break;
            case ACT_POST:
            {
                bool open = !cfg.postHaggle;
                if (!open)
                {
                    std::lock_guard<std::mutex> guard(lock);
                    Today const& today = TodayOf(low);
                    if (today.done)
                        open = true;
                    else if (today.round)
                        // Walked off in the middle of it: the trader has not forgotten.
                        ShowRound(player, creature, today, TEXT_REACTION + TemperOf(low, today) * 3 + 1);
                    else
                        ShowRound(player, creature, today, TEXT_OPENING + TemperOf(low, today) * 2 + Mix(low, today.day, 78) % 2);
                }
                if (open)
                    OpenPost(player, creature);
                break;
            }
            case ACT_OPEN:
                OpenPost(player, creature);
                break;
            default:
                if (action >= ACT_PICK && action < uint32(ACT_PICK) + uint32(APPROACHES) && cfg.postHaggle)
                    Haggle(player, creature, uint8(action - ACT_PICK));
                else
                    CloseGossipMenuFor(player);
                break;
        }
        return true;
    }

    bool PostPacket(WorldSession* session, WorldPacket const& packet)
    {
        switch (packet.GetOpcode())
        {
            case CMSG_NPC_TEXT_QUERY:
            {
                if (packet.size() < 4)
                    return true;
                uint32 const id = packet.read<uint32>(0);
                if (id < TextBase || id >= TextBase + TEXT_COUNT)
                    return true;
                SendText(session, id - TextBase);
                return false;
            }
            case CMSG_NAME_QUERY:
            {
                if (packet.size() < 8 || ObjectGuid(packet.read<uint64>(0)) != Seller())
                    return true;
                WorldPackets::Query::NameQueryResponse response;
                response.Guid = Seller().WriteAsPacked();
                response.NameUnknown = false;
                response.Name = "Trading Post";
                response.Race = RACE_HUMAN;
                response.Sex = GENDER_MALE;
                response.Class = CLASS_ROGUE;
                response.Declined = false;
                session->SendPacket(response.Write());
                return false;
            }
            case CMSG_AUCTION_LIST_ITEMS:
            {
                Player* player = session->GetPlayer();
                if (!player || !cfg.enabled || !cfg.post)
                    return true;
                {
                    std::lock_guard<std::mutex> guard(lock);
                    auto visitor = visitors.find(player->GetGUID().GetCounter());
                    if (visitor == visitors.end() || !visitor->second.atPost)
                        return true;
                }
                WorldPacket copy(packet);
                try
                {
                    ListItems(session, copy);
                }
                catch (ByteBufferException const&)
                {
                    LOG_ERROR("module", "PlayerbotsAuctions: trading post - the list request of {} could not be read.", player->GetName());
                }
                return false;
            }
            case CMSG_AUCTION_PLACE_BID:
            {
                Player* player = session->GetPlayer();
                if (!player || packet.size() < 16)
                    return true;
                uint32 const id = packet.read<uint32>(8);
                if (!(id & IdFlag))
                    return true;        // a real auction
                if (cfg.debug)
                    LOG_INFO("module", "PlayerbotsAuctions: trading post - {} clicks a price: {} x item {} for {} copper.",
                        player->GetName(), (id >> 24) & 0x7F, id & 0x00FFFFFF, packet.read<uint32>(12));
                Buy(session, ObjectGuid(packet.read<uint64>(0)), id, packet.read<uint32>(12));
                return false;
            }
            default:
                return true;
        }
    }

    void PostLeft(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(lock);
        visitors.erase(player->GetGUID().GetCounter());
    }
}
