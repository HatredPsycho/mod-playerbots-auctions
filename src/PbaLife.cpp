/*
 * mod-playerbots-auctions - what the bots do: go to the auction house, sell, buy, craft.
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Pba.h"

namespace pba
{
namespace
{
    /// One auction as a buyer sees it.
    struct Offer
    {
        uint32 id = 0;
        uint32 count = 0;
        uint32 buyout = 0;      // 0 = bids only
        uint32 each = 0;        // buyout per piece; huge if there is no buyout
        uint32 nextBid = 0;     // what the next bid has to be
        bool mine = false;      // the bot is the highest bidder already
        bool buyable = false;   // not from the own account, and from someone the bots may buy from
    };

    /// The offers of every item in one auction house, cheapest first, without the bot's own auctions.
    using Index = std::unordered_map<uint32, std::vector<Offer>>;

    enum Task
    {
        TASK_VISIT,     // on its way to the auctioneers for the full business
        TASK_SELL,      // a second look only to sell what it has made
        TASK_CRAFT      // on its way to an anvil, or busy crafting
    };

    struct Watch
    {
        time_t from = 0;        // not looked at before this
        time_t until = 0;       // given up after this
        Task task = TASK_VISIT;
        bool renewed = false;
    };

    struct Plan
    {
        Recipe recipe;
        uint32 left = 0;        // how many more it is going to make
        uint32 fails = 0;
        time_t until = 0;       // its materials are not for sale until then
        std::vector<uint32> keep;   // what it makes is a material for its own profession: kept, not sold
        time_t keepUntil = 0;
        uint32 steps = 0;       // how many steps of a chain lie behind it: ore - bars - sword
        bool next = false;      // back at the auctioneers it decides what to do with what it made
    };

    class Life
    {
    public:
        void Update(uint32 diff)
        {
            if (!cfg.enabled)
                return;

            _saveTimer += diff;
            if (_saveTimer >= 5 * MINUTE * IN_MILLISECONDS)
            {
                _saveTimer = 0;
                market.SaveMemory();
            }

            // Bots on an errand are watched closely: they act the moment they stand where they wanted to go.
            _watchTimer += diff;
            if (_watchTimer >= 2 * IN_MILLISECONDS)
            {
                _watchTimer = 0;
                time_t const now = GameTime::GetGameTime().count();
                for (auto watch = _watch.begin(); watch != _watch.end();)
                {
                    Player* bot = ObjectAccessor::FindConnectedPlayer(watch->first);
                    bool done = !bot || watch->second.until < now;
                    if (done && watch->second.task == TASK_CRAFT)
                        _plans.erase(watch->first.GetCounter());        // did not get there: the materials are free again
                    if (!done && watch->second.from <= now && bot->IsInWorld() && !bot->IsBeingTeleported())
                    {
                        Task const task = watch->second.task;
                        watch->second.renewed = false;
                        bool finished;
                        if (task == TASK_CRAFT)
                            finished = Work(bot, now);
                        else
                        {
                            _nextVisit.erase(watch->first.GetCounter());
                            finished = Visit(bot, now, task == TASK_SELL);
                        }
                        done = finished && !watch->second.renewed;      // unless it set out on the next errand
                    }
                    watch = done ? _watch.erase(watch) : std::next(watch);
                }
            }

            // With Debug on: every five minutes a line on what the bots were up to.
            _statTimer += diff;
            if (_statTimer >= 5 * MINUTE * IN_MILLISECONDS)
            {
                _statTimer = 0;
                if (cfg.debug)
                    LOG_INFO("module", "PlayerbotsAuctions: last 5 minutes - {} bot(s) online, {} asked themselves whether to go ({} had something to sell, "
                        "the most was {} thing(s)), {} set out, {} wanted to but found no way, {} did business at an auctioneer, {} on an errand now, "
                        "{} time(s) something was kept from a vendor. Of {} thing(s) in their bags {} were not of a kind that is sold, "
                        "{} soulbound or not tradable, {} needed by the bot itself, {} not worth an auction. {} thing(s) went to a vendor as junk.",
                        sRandomPlayerbotMgr.GetAllBots().size(), _stat.asked, _stat.withGoods, _stat.mostGoods, _stat.trips, _stat.noWay, _stat.visits,
                        _watch.size(), _kept.exchange(0), _stat.seen, _stat.wrongKind, _stat.bound, _stat.needed, _stat.cheap, _stat.vendored);
                if (cfg.debug)
                {
                    LOG_INFO("module", "PlayerbotsAuctions: most common of a kind that is not sold: {}", MostCommon(_stat.wrongKinds));
                    LOG_INFO("module", "PlayerbotsAuctions: most common the bots need themselves: {}", MostCommon(_stat.neededOnes));
                    std::string crafts;
                    for (auto const& skill : _stat.recipes)
                        crafts += Acore::StringFormat("{}{}: {} recipe(s) thought through, {} lack a material, {} do not pay, {} work out",
                            crafts.empty() ? "" : "; ", SkillName(skill.first), skill.second[0], skill.second[1], skill.second[2], skill.second[3]);
                    LOG_INFO("module", "PlayerbotsAuctions: professions at the auctioneers: {}", crafts.empty() ? "-" : crafts);
                    std::string misses;
                    for (std::string const& miss : _stat.noPay)
                        misses += (misses.empty() ? "" : "; ") + miss;
                    LOG_INFO("module", "PlayerbotsAuctions: some that do not pay: {}", misses.empty() ? "-" : misses);
                    LOG_INFO("module", "PlayerbotsAuctions: gathered in the last 5 minutes: {}", GatheredText());
                    LOG_INFO("module", "PlayerbotsAuctions: professions of the bots online: {}", ProfessionCensus());
                    LOG_INFO("module", "PlayerbotsAuctions: gathering right now: {}", NodeCensus());
                }
                else
                {
                    std::lock_guard<std::mutex> guard(_gatherLock);
                    _gathered.clear();
                }
                _stat = Stat();
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

                if (Visit(bot, now, false))
                    ++visits;
            }
        }

        /// For the summary: what a bot took out of the world - ore from a vein, herbs, a hide off a corpse, cloth.
        /// Called from the map threads, hence the lock.
        void Looted(Player* bot, Item* item, uint32 count, ObjectGuid source)
        {
            if (!cfg.enabled || !cfg.debug || !bot || !item || !item->GetTemplate() || !bot->GetSession() ||
                !sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()))
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (proto->Class != ITEM_CLASS_TRADE_GOODS && proto->Class != ITEM_CLASS_GEM)
                return;

            char const* what = nullptr;
            if (source.IsGameObject())
            {
                if (proto->Class == ITEM_CLASS_GEM)
                    what = "gems from veins";
                else if (proto->SubClass == ITEM_SUBCLASS_METAL_STONE)
                    what = "ore and stone mined";
                else if (proto->SubClass == ITEM_SUBCLASS_HERB)
                    what = "herbs picked";
                else
                    what = "other materials from chests and nodes";
            }
            else if (source.IsCreatureOrVehicle())
            {
                Creature* corpse = ObjectAccessor::GetCreature(*bot, source);
                if (corpse && corpse->loot.loot_type == LOOT_SKINNING)
                    what = proto->SubClass == ITEM_SUBCLASS_LEATHER ? "leather and hides skinned" :
                           proto->SubClass == ITEM_SUBCLASS_HERB ? "herbs from corpses" :
                           proto->SubClass == ITEM_SUBCLASS_METAL_STONE ? "ore from corpses" : "other things skinned";
                else if (proto->SubClass == ITEM_SUBCLASS_CLOTH && proto->Class == ITEM_CLASS_TRADE_GOODS)
                    what = "cloth looted";
                else if (proto->SubClass == ITEM_SUBCLASS_MEAT && proto->Class == ITEM_CLASS_TRADE_GOODS)
                    what = "meat looted";
                else
                    what = "other materials looted";
            }
            if (!what)
                return;

            std::lock_guard<std::mutex> guard(_gatherLock);
            Gathered& entry = _gathered[what];
            entry.pieces += count;
            entry.bots.insert(bot->GetGUID().GetCounter());
        }

        /// Is this something the bot does not give to a vendor? mod-playerbots sells whatever a bot does not
        /// need to the next vendor it passes; what is meant for the auction house, and the materials the bot
        /// has plans for, stay in its bags.
        bool KeepsFromVendor(Player* bot, Item* item)
        {
            if (!cfg.enabled || !cfg.keepFromVendor || !cfg.cityTrips || !bot || !item || !bot->GetSession() ||
                !sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()))
                return false;
            if (bot->GetLevel() < cfg.minLevel || AvoidsAuctionHouse(bot))
                return false;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || !CanTravel(botAI) || (botAI->GetMaster() && IsRealPlayer(botAI->GetMaster())))
                return false;

            time_t const now = GameTime::GetGameTime().count();
            auto plan = _plans.find(bot->GetGUID().GetCounter());
            if (plan != _plans.end())
            {
                if (plan->second.keepUntil > now && Keeps(plan->second, item->GetEntry()))
                    return Kept();
                if (plan->second.left || plan->second.until > now)
                    for (auto const& reagent : plan->second.recipe.reagents)
                        if (reagent.first == item->GetEntry())
                            return Kept();
            }

            if (!IsSellable(bot, item) || market.Tries(bot->GetGUID().GetCounter(), item->GetEntry()) >= TryLimit(bot))
                return false;
            // What would not be worth an auction may go: all it carries of the kind counts, a stack grows.
            ItemTemplate const* proto = item->GetTemplate();
            if (proto->Quality >= MAX_ITEM_QUALITY || !Market::Base(proto) ||
                market.Value(proto) * bot->GetItemCount(proto->ItemId) < double(cfg.minListValue))
                return false;
            return Kept();
        }

    private:
        bool Kept()
        {
            ++_kept;       // asked from the map threads, hence counted apart
            return true;
        }

        // =========================================================================================== errands

        void SetWatch(Player* bot, time_t from, time_t until, Task task)
        {
            Watch& watch = _watch[bot->GetGUID()];
            watch.from = from;
            watch.until = until;
            watch.task = task;
            watch.renewed = true;
        }

        /// One look at a bot. Returns true if it did its business at an auctioneer.
        bool Visit(Player* bot, time_t now, bool sellOnly)
        {
            _now = now;
            if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld() || !bot->GetSession())
                return false;

            ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
            auto next = _nextVisit.find(guid);
            if (next != _nextVisit.end() && next->second > now)
                return false;

            // Some players never set foot in an auction house.
            if (AvoidsAuctionHouse(bot))
            {
                _nextVisit[guid] = now + 6 * HOUR;
                return false;
            }

            if (bot->GetLevel() < cfg.minLevel || !bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() ||
                bot->IsInFlight() || bot->GetTradeData() || bot->InBattleground() || bot->GetMap()->Instanceable())
                return false;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || !botAI->GetAiObjectContext())
                return false;

            // Bots that travel with a real player keep their loot; that player decides what happens to it.
            if (botAI->GetMaster() && IsRealPlayer(botAI->GetMaster()))
                return false;

            // Arrived from another continent: told again what it came for.
            ResumeJourney(bot, botAI);

            // Next to a vendor: rid of the junk.
            SellJunk(bot, botAI);

            AuctionHouseId houseId;
            if (!FindHouse(bot, botAI, houseId))
            {
                // Not at an auctioneer: look again in a while, not at every pass.
                _nextVisit[guid] = now + urand(2 * MINUTE, 4 * MINUTE);
                // A bot that is not on an errand already decides whether it is time to go.
                if (_watch.find(bot->GetGUID()) == _watch.end() && _decisions < 20 && DecideToGo(bot, botAI, now))
                    SetWatch(bot, now, now + 8 * MINUTE, TASK_VISIT);
                return false;
            }

            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
            AuctionHouseEntry const* houseEntry = AuctionHouseMgr::GetAuctionHouseEntryFromHouse(houseId);
            if (!house || !houseEntry)
                return false;

            ++_stat.visits;
            // From here on the bot "was at the auction house". Those who like to trade come back sooner.
            float const trading = TraitOf(bot, TRAIT_TRADING);
            _nextVisit[guid] = now + time_t(float(urand(cfg.visitCooldownMin, cfg.visitCooldownMax)) * (1.6f - 1.2f * trading));

            if (cfg.collectMail)
                CollectMail(bot, now);

            Index index;
            uint32 own = 0;
            BuildIndex(bot, house, index, own);

            if (!sellOnly)
            {
                // What the bot could make, and the materials that go into it.
                std::unordered_set<uint32> materials;
                if (cfg.craftEnabled)
                {
                    std::vector<Recipe> recipes = Recipes(bot, CanTravel(botAI));
                    AddRefining(bot, botAI, recipes);
                    for (Recipe const& recipe : recipes)
                        for (auto const& reagent : recipe.reagents)
                            materials.insert(reagent.first);

                    if (PlanCraft(bot, botAI, house, index, recipes) && cfg.collectMail)
                        CollectMail(bot, now);          // the materials it bought arrive by mail at once
                    StartWork(bot, botAI, now);
                }

                if (Buy(bot, botAI, house, index, materials) && cfg.collectMail)
                    CollectMail(bot, now);              // what it bought outright arrives by mail at once
            }

            if (house->Getcount() < cfg.maxAuctionsPerHouse)
                Sell(bot, botAI, house, houseEntry, houseId, index, own);
            return true;
        }

        /// Does this bot want to go to the auction house now? Like a player, it goes when it feels it has
        /// enough to sell or its bags are getting full - and every bot draws that line somewhere else.
        /// A bot that is in town anyway goes for less, but not every time.
        bool DecideToGo(Player* bot, PlayerbotAI* botAI, time_t now)
        {
            if (!cfg.cityTrips || bot->GetGroup())
                return false;

            ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
            auto next = _nextTrip.find(guid);
            if (next != _nextTrip.end() && next->second > now)
                return false;

            // At night fewer people are up for a trip to town.
            if (frand(0.0f, 1.0f) > ActivityNow(now))
            {
                _nextTrip[guid] = now + 10 * MINUTE;
                return false;
            }

            ++_decisions;       // looking through the bags of many bots at once would cost a noticeable moment
            std::vector<Item*> items;
            Collect(bot, botAI, items);
            uint32 const goods = uint32(items.size());
            bool const inTown = IsInTown(bot);
            ++_stat.asked;
            if (goods)
                ++_stat.withGoods;
            _stat.mostGoods = std::max(_stat.mostGoods, goods);

            // This bot's own lines. The orderly go early, the others collect until nothing fits.
            float const order = TraitOf(bot, TRAIT_ORDER);
            float const trading = TraitOf(bot, TRAIT_TRADING);
            // mod-playerbots starts throwing things away at 90 percent: every bot is on its way before that.
            uint32 const full = uint32(85.0f - order * 35.0f);
            uint32 const enough = cfg.cityItemsMax - uint32(order * float(cfg.cityItemsMax - cfg.cityItemsMin) + 0.5f);
            bool const bagsFull = botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get() >= full;

            bool go;
            char const* why;
            if (goods >= 3 && bagsFull)
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
                go = inTown && goods >= 1 && frand(0.0f, 100.0f) < 20.0f + trading * 60.0f;
                why = "it is in town anyway";
            }

            if (!go)
            {
                _nextTrip[guid] = now + (inTown ? 5 * MINUTE : 10 * MINUTE);
                return false;
            }

            Journey const journey = SendToAuctionHouse(bot, botAI);
            if (journey == JOURNEY_NONE)
            {
                ++_stat.noWay;
                _nextTrip[guid] = now + 10 * MINUTE;
                return false;
            }
            ++_stat.trips;

            _nextTrip[guid] = now + time_t(float(urand(cfg.cityCooldownMin, cfg.cityCooldownMax)) * (1.5f - trading));
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} {} to the auction house with {} item(s) to sell: {}.",
                    bot->GetName(), journey == JOURNEY_WALK ? "walks" : "takes its hearth to a city and walks", goods, why);
            return true;
        }

        // =========================================================================================== the offers

        bool CanBuyFrom(Player* bot, AuctionEntry const* auction)
        {
            if (auction->owner == bot->GetGUID() || auction->bidder == bot->GetGUID())
                return false;
            // The server does not let anyone bid on the auctions of the own account.
            uint32 const owner = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
            if (!owner || owner == bot->GetSession()->GetAccountId())
                return false;
            return sPlayerbotAIConfig.IsInRandomAccountList(owner) ? cfg.buyFromBots : cfg.buyFromPlayers;
        }

        void BuildIndex(Player* bot, AuctionHouseObject* house, Index& index, uint32& own)
        {
            // The server does not let anyone bid on the auctions of the own account.
            uint32 const account = bot->GetSession()->GetAccountId();
            std::unordered_map<ObjectGuid, bool> sellers;
            for (auto const& entry : house->GetAuctions())
            {
                AuctionEntry const* auction = entry.second;
                if (!auction || !auction->itemCount)
                    continue;
                if (auction->owner == bot->GetGUID())
                {
                    ++own;
                    continue;
                }

                Offer offer;
                offer.id = auction->Id;
                offer.count = auction->itemCount;
                offer.buyout = auction->buyout;
                offer.each = auction->buyout ? std::max<uint32>(1, auction->buyout / auction->itemCount) : std::numeric_limits<uint32>::max();
                offer.nextBid = std::max(auction->startbid, auction->bid ? auction->bid + auction->GetAuctionOutBid() : auction->startbid);
                offer.mine = auction->bidder == bot->GetGUID();
                auto seller = sellers.find(auction->owner);
                if (seller == sellers.end())
                {
                    uint32 const owner = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
                    bool const allowed = owner && owner != account &&
                        (sPlayerbotAIConfig.IsInRandomAccountList(owner) ? cfg.buyFromBots : cfg.buyFromPlayers);
                    seller = sellers.emplace(auction->owner, allowed).first;
                }
                offer.buyable = seller->second;
                index[auction->item_template].push_back(offer);
            }
            for (auto& list : index)
                std::sort(list.second.begin(), list.second.end(), [](Offer const& a, Offer const& b) { return a.each < b.each; });
        }

        /// What the bot may spend. The thrifty keep a reserve for repairs and a rainy day.
        double Spendable(Player* bot)
        {
            if (!cfg.buyUseBotMoney)
                return double(MAX_MONEY_AMOUNT);
            return double(bot->GetMoney()) * (0.85 - 0.5 * TraitOf(bot, TRAIT_THRIFT));
        }

        // =========================================================================================== selling

        void Sell(Player* bot, PlayerbotAI* botAI, AuctionHouseObject* house, AuctionHouseEntry const* houseEntry,
            AuctionHouseId houseId, Index const& index, uint32 own)
        {
            std::vector<Item*> items;
            Collect(bot, botAI, items);
            if (items.empty())
                return;

            // Those who like to trade put up more at once and keep more auctions running.
            float const trading = TraitOf(bot, TRAIT_TRADING);
            uint32 const perVisit = 2 + uint32(trading * 6.0f);
            uint32 const maxOwn = 4 + uint32(trading * 20.0f);

            Acore::Containers::RandomShuffle(items);
            uint32 posted = 0;
            for (Item* item : items)
            {
                // One stack can go up as several packs, like a player does it.
                for (uint32 packs = 0; packs < 3; ++packs)
                {
                    if (posted >= perVisit || own + posted >= maxOwn)
                        return;
                    bool wholeStack = false;
                    if (!OfferPack(bot, item, house, houseEntry, houseId, index, wholeStack))
                        break;
                    ++posted;
                    if (wholeStack)
                        break;      // the item is in the auction house now
                }
            }
        }

        /// How a bot packs a stack: singles, fives, tens, twenties or all of it. A habit it keeps per kind of item.
        uint32 PackSize(Player* bot, ItemTemplate const* proto, uint32 count)
        {
            if (count <= 1 || proto->GetMaxStackSize() <= 1)
                return count;
            static uint32 const sizes[] = { 1, 5, 10, 20 };
            std::vector<uint32> fitting;
            for (uint32 const size : sizes)
                if (size < count)
                    fitting.push_back(size);
            fitting.push_back(count);
            // Mostly the whole stack or a round pack; singles only for things worth something.
            uint32 const pick = uint32(TraitOf(bot, TRAIT_HABIT) * 1000.0f + float(proto->ItemId % 7)) + urand(0, 1);
            uint32 size = fitting[pick % fitting.size()];
            if (size == 1 && Market::Regular(proto) < double(20 * SILVER))
                size = count;
            return size;
        }

        /// The price of one piece, as this bot would set it today.
        double PriceFor(Player* bot, ItemTemplate const* proto, Index const& index, uint32 tries, bool& exact)
        {
            float const knowledge = TraitOf(bot, TRAIT_KNOWLEDGE);
            float const patience = TraitOf(bot, TRAIT_PATIENCE);
            float const greed = TraitOf(bot, TRAIT_GREED);
            exact = false;

            // The cheapest offer of someone else.
            uint32 cheapest = 0;
            auto found = index.find(proto->ItemId);
            if (found != index.end())
                for (Offer const& offer : found->second)
                    if (offer.buyout)
                    {
                        cheapest = offer.each;
                        break;
                    }

            double each;
            if (knowledge >= 0.5f)
            {
                // Knows what the item goes for and looks at the competition.
                double const value = market.Value(proto);
                each = value * (0.95 + 0.2 * greed);
                if (!cheapest)
                    each *= 1.15 + 0.45 * greed;                // nobody else offers it: asks for more
                else if (double(cheapest) >= value * 0.8)
                {
                    // Goes just below the cheapest. The better it knows the game, the smaller the step.
                    double const below = double(cheapest) * (1.0 - (1.0 - knowledge) * 0.08) - 1.0;
                    if (below < each)
                    {
                        each = below;
                        exact = true;
                    }
                }
                // An offer well below the usual price is somebody's mistake or a bait; it does not follow that.
            }
            else
            {
                // Prices by gut feeling: the less it knows, the further off it can be - in both directions.
                double const spread = 1.0 - knowledge * 2.0;
                each = Market::Regular(proto) * std::pow(3.0, double(frand(-1.0f, 1.0f)) * spread) * (0.95 + 0.2 * greed);
                if (!cheapest)
                    each *= 1.1 + 0.2 * greed;
            }

            if (patience < 0.3f)
                each *= 0.85;       // wants it gone
            else if (patience > 0.7f)
                each *= 1.1;        // can wait
            // What came back unsold goes up again for less.
            each *= std::max(0.5, 1.0 - 0.15 * tries);

            // Never close to the vendor value: nobody should buy from a bot to sell to a vendor.
            double const lowest = double(proto->SellPrice) * cfg.minVendorFactor;
            if (each < lowest)
            {
                each = lowest;
                exact = false;
            }
            return each;
        }

        /// Puts one pack of an item up for auction. Returns false if the bot keeps the item.
        bool OfferPack(Player* bot, Item* item, AuctionHouseObject* house, AuctionHouseEntry const* houseEntry,
            AuctionHouseId houseId, Index const& index, bool& wholeStack)
        {
            ItemTemplate const* proto = item->GetTemplate();
            uint32 const count = item->GetCount();
            if (!count || sAuctionMgr->GetAItem(item->GetGUID()))
                return false;

            uint32 const pack = PackSize(bot, proto, count);
            wholeStack = pack >= count;
            uint32 const tries = market.Tries(bot->GetGUID().GetCounter(), proto->ItemId);

            bool exact = false;
            double const each = PriceFor(bot, proto, index, tries, exact);
            double const total = each * pack;
            double const limit = cfg.maxBuyout ? double(cfg.maxBuyout) : double(MAX_MONEY_AMOUNT);
            if (total > limit || total < double(cfg.minListValue))
                return false;       // too valuable to give away at the highest allowed price, or not worth the walk
            // Rounding must not take the price below the line under which buying to sell to a vendor would pay.
            uint32 const vendorLine = uint32(std::ceil(double(proto->SellPrice) * pack * cfg.minVendorFactor));
            uint32 const buyout = std::max<uint32>({ exact ? uint32(total) : HumanPrice(total), vendorLine, 2u });

            // The bid: some start low to attract bidders, some want their price and nothing less.
            float const patience = TraitOf(bot, TRAIT_PATIENCE);
            float const habit = TraitOf(bot, TRAIT_HABIT);
            double const share = habit < 0.2f ? 1.0 : (patience < 0.3f ? 0.6 : 0.7 + 0.25 * habit);
            uint32 const bid = std::clamp<uint32>(uint32(double(buyout) * share), std::min(vendorLine, buyout), buyout);

            // The impatient list for half a day, the patient for two.
            uint32 const etime = (patience < 0.3f ? 12 : patience < 0.7f ? 24 : 48) * HOUR;
            uint32 const auctionTime = uint32(etime * sWorld->getRate(RATE_AUCTION_TIME));

            // The deposit is lost if nobody buys. For things that bring little more than a vendor pays,
            // the auction house is not worth it - those go to the vendor.
            uint32 deposit = 0;
            if (cfg.chargeDeposit)
            {
                deposit = AuctionHouseMgr::GetAuctionDeposit(houseEntry, etime, item, pack);
                double const gain = double(buyout) * 0.95 - double(proto->SellPrice) * pack;
                if (gain < double(deposit) * 2.0 || !bot->HasEnoughMoney(deposit))
                    return false;
            }

            // From here on this is what the server does when a player creates an auction.
            Item* sold = item;
            if (!wholeStack)
            {
                sold = item->CloneItem(pack, bot);
                if (!sold)
                    return false;
            }
            if (deposit)
                bot->ModifyMoney(-int32(deposit));

            AuctionEntry* auction = new AuctionEntry;
            auction->Id = sObjectMgr->GenerateAuctionID();
            auction->houseId = houseId;
            auction->item_guid = sold->GetGUID();
            auction->item_template = sold->GetEntry();
            auction->itemCount = pack;
            auction->owner = bot->GetGUID();
            auction->startbid = bid;
            auction->bidder = ObjectGuid::Empty;
            auction->bid = 0;
            auction->buyout = buyout;
            auction->expire_time = GameTime::GetGameTime().count() + auctionTime;
            auction->deposit = deposit;
            auction->auctionHouseEntry = houseEntry;

            sAuctionMgr->AddAItem(sold);
            house->AddAuction(auction);

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            if (wholeStack)
            {
                bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
                item->DeleteFromInventoryDB(trans);
                item->SaveToDB(trans);
            }
            else
            {
                // The pack is a new item; the stack in the bag gets smaller.
                item->SetCount(count - pack);
                item->SetState(ITEM_CHANGED, bot);
                bot->ItemRemovedQuestCheck(item->GetEntry(), pack);
                item->SendUpdateToPlayer(bot);
                sold->SaveToDB(trans);
            }
            auction->SaveToDB(trans);
            bot->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} offers {} x{} (item {}) for {} copper, bid {} copper, {} h, {} auction house{}.",
                    bot->GetName(), proto->Name1, pack, proto->ItemId, buyout, bid, etime / HOUR,
                    houseId == AuctionHouseId::Alliance ? "Alliance" : houseId == AuctionHouseId::Horde ? "Horde" : "neutral",
                    tries ? ", not for the first time" : "");
            return true;
        }

        /// How often a bot tries before it gives an item to the vendor instead.
        uint32 TryLimit(Player* bot)
        {
            float const patience = TraitOf(bot, TRAIT_PATIENCE);
            return patience < 0.3f ? 1 : patience < 0.7f ? 2 : 3;
        }

        /// For the summary: the fifteen most common items of a group, with quality and item class.
        static std::string MostCommon(std::unordered_map<uint32, uint32> const& counts)
        {
            std::vector<std::pair<uint32, uint32>> list(counts.begin(), counts.end());
            std::sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            std::string text;
            for (std::size_t i = 0; i < list.size() && i < 15; ++i)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(list[i].first))
                    text += Acore::StringFormat("{}{} x{} (q{} c{})", text.empty() ? "" : ", ", proto->Name1, list[i].second, proto->Quality, proto->Class);
            return text.empty() ? "-" : text;
        }

        std::string GatheredText()
        {
            std::lock_guard<std::mutex> guard(_gatherLock);
            std::string text;
            for (auto const& entry : _gathered)
                text += Acore::StringFormat("{}{}: {} piece(s) by {} bot(s)", text.empty() ? "" : "; ", entry.first, entry.second.pieces, entry.second.bots.size());
            _gathered.clear();
            return text.empty() ? "nothing" : text;
        }

        /// How many of the bots online have which profession - so one can tell "nobody mines" from "no miner is online".
        static std::string ProfessionCensus()
        {
            static uint32 const skills[] = { SKILL_MINING, SKILL_HERBALISM, SKILL_SKINNING, SKILL_BLACKSMITHING, SKILL_ENGINEERING,
                SKILL_JEWELCRAFTING, SKILL_ALCHEMY, SKILL_INSCRIPTION, SKILL_TAILORING, SKILL_LEATHERWORKING, SKILL_ENCHANTING,
                SKILL_COOKING, SKILL_FIRST_AID };
            uint32 counts[std::size(skills)] = { };
            PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
            for (auto const& entry : bots)
                if (Player* bot = entry.second)
                    for (std::size_t i = 0; i < std::size(skills); ++i)
                        if (bot->HasSkill(skills[i]))
                            ++counts[i];
            std::string text;
            for (std::size_t i = 0; i < std::size(skills); ++i)
                text += Acore::StringFormat("{}{} {}", i ? ", " : "", SkillName(skills[i]), counts[i]);
            return text;
        }

        /// Why the gatherers gather or do not: do they carry their tool, are there veins and herbs in sight, and
        /// would mod-playerbots go for them? It only does within its loot distance, at about the bot's own height
        /// (3.5 yards up or down) and with enough skill.
        static std::string NodeCensus()
        {
            static uint32 const picks[] = { 756, 778, 1819, 1893, 1959, 2901, 9465, 20723, 40772, 40892, 40893 };
            static uint32 const knives[] = { 7005, 40772, 40893, 12709, 19901 };
            struct Count
            {
                uint32 bots = 0, withTool = 0, seeing = 0, nodes = 0, close = 0, level = 0, skilled = 0, fine = 0;
            };
            Count mining, herbs, skinning;
            float const reach = sPlayerbotAIConfig.lootDistance;

            PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
            for (auto const& entry : bots)
            {
                Player* bot = entry.second;
                if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->GetSession())
                    continue;
                if (bot->HasSkill(SKILL_SKINNING))
                {
                    ++skinning.bots;
                    for (uint32 const knife : knives)
                        if (bot->HasItemCount(knife, 1)) { ++skinning.withTool; break; }
                }
                bool const miner = bot->HasSkill(SKILL_MINING), herbalist = bot->HasSkill(SKILL_HERBALISM);
                if (!miner && !herbalist)
                    continue;
                if (miner)
                {
                    ++mining.bots;
                    for (uint32 const pick : picks)
                        if (bot->HasItemCount(pick, 1)) { ++mining.withTool; break; }
                }
                if (herbalist)
                {
                    ++herbs.bots;
                    ++herbs.withTool;       // needs none
                }

                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                if (!botAI || !botAI->GetAiObjectContext())
                    continue;
                bool sawVein = false, sawHerb = false;
                GuidVector const objects = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest game objects")->Get();
                for (ObjectGuid const guid : objects)
                {
                    GameObject* go = ObjectAccessor::GetGameObject(*bot, guid);
                    if (!go || !go->isSpawned() || !go->GetGOInfo())
                        continue;
                    LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
                    if (!lock)
                        continue;
                    for (uint8 i = 0; i < 8; ++i)
                    {
                        if (lock->Type[i] != LOCK_KEY_SKILL)
                            continue;
                        bool const vein = lock->Index[i] == LOCKTYPE_MINING, herb = lock->Index[i] == LOCKTYPE_HERBALISM;
                        if (!(vein && miner) && !(herb && herbalist))
                            continue;
                        Count& count = vein ? mining : herbs;
                        (vein ? sawVein : sawHerb) = true;
                        ++count.nodes;
                        bool const close = bot->GetDistance(go) <= reach;
                        // The rule of patches/mod-playerbots/gather-on-slopes.patch.
                        bool const level = std::fabs(go->GetPositionZ() - bot->GetPositionZ()) <=
                            std::min(25.0f, INTERACTION_DISTANCE - 2.0f + 0.5f * bot->GetExactDist2d(go));
                        bool const skilled = bot->GetSkillValue(vein ? SKILL_MINING : SKILL_HERBALISM) >= std::max<uint32>(1, lock->Skill[i]);
                        if (close) ++count.close;
                        if (level) ++count.level;
                        if (skilled) ++count.skilled;
                        if (close && level && skilled) ++count.fine;
                        break;
                    }
                }
                if (sawVein) ++mining.seeing;
                if (sawHerb) ++herbs.seeing;
            }

            auto text = [reach](char const* name, char const* node, Count const& count)
            {
                return Acore::StringFormat("{} {} ({} with the tool), {} of them see {} {} - {} within {} yards, {} on a slope they take, {} with enough skill, {} all three",
                    count.bots, name, count.withTool, count.seeing, count.nodes, node, count.close, uint32(reach), count.level, count.skilled, count.fine);
            };
            return text("miners", "vein(s)", mining) + "; " + text("herbalists", "herb(s)", herbs) +
                Acore::StringFormat("; {} skinners ({} with a knife)", skinning.bots, skinning.withTool);
        }

        static std::string SkillName(uint32 skill)
        {
            switch (skill)
            {
                case 0: return "taking apart";
                case SKILL_ALCHEMY: return "alchemy";
                case SKILL_BLACKSMITHING: return "blacksmithing";
                case SKILL_COOKING: return "cooking";
                case SKILL_ENCHANTING: return "enchanting";
                case SKILL_ENGINEERING: return "engineering";
                case SKILL_FIRST_AID: return "first aid";
                case SKILL_INSCRIPTION: return "inscription";
                case SKILL_JEWELCRAFTING: return "jewelcrafting";
                case SKILL_LEATHERWORKING: return "leatherworking";
                case SKILL_MINING: return "mining";
                case SKILL_HERBALISM: return "herbalism";
                case SKILL_SKINNING: return "skinning";
                case SKILL_TAILORING: return "tailoring";
                default: return "skill " + std::to_string(skill);
            }
        }

        static char const* KindName(Kind kind)
        {
            return kind == KIND_PROSPECT ? "prospect" : kind == KIND_MILL ? "mill" : kind == KIND_DISENCHANT ? "disenchant" :
                kind == KIND_ENCHANT ? "scroll" : "craft";
        }

        /// What the bot has no use for and what is not worth an auction goes to the vendor it stands next to,
        /// for the vendor's price - as a player empties the bags on the way.
        void SellJunk(Player* bot, PlayerbotAI* botAI)
        {
            if (!cfg.sellJunk || !FindVendor(bot, botAI))
                return;

            std::vector<Item*> junk;
            auto look = [&](Item* item)
            {
                if (!item || !item->GetTemplate() || !item->GetTemplate()->SellPrice || item->GetOwnerGUID() != bot->GetGUID() ||
                    item->IsNotEmptyBag() || sAuctionMgr->GetAItem(item->GetGUID()) || IsHandout(bot, item->GetTemplate()) ||
                    KeepsFromVendor(bot, item))
                    return;
                // The bot's own judgement: nothing it wears, uses, or needs for a quest or a profession.
                ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
                if (usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH)
                    junk.push_back(item);
            };
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                look(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        look(bag->GetItemByPos(slot));
            if (junk.empty())
                return;

            uint64 money = 0;
            std::string names;
            uint32 named = 0;
            for (Item* item : junk)
            {
                uint64 const price = uint64(item->GetTemplate()->SellPrice) * item->GetCount();
                money += price;
                if (cfg.debug && named < 6)
                {
                    names += Acore::StringFormat("{}{} x{} ({} copper)", named ? ", " : "", item->GetTemplate()->Name1, item->GetCount(), price);
                    ++named;
                }
                bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            }
            bot->ModifyMoney(int32(std::min<uint64>(money, MAX_MONEY_AMOUNT / 2)));
            _stat.vendored += uint32(junk.size());
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} sells {} thing(s) it has no use for to a vendor for {} copper: {}{}.",
                    bot->GetName(), junk.size(), money, names, junk.size() > named ? ", ..." : "");
        }

        static bool Keeps(Plan const& plan, uint32 itemId)
        {
            return std::find(plan.keep.begin(), plan.keep.end(), itemId) != plan.keep.end();
        }

        void ConsiderItem(Player* bot, PlayerbotAI* botAI, Item* item, std::vector<Item*>& items)
        {
            if (!item)
                return;
            ++_stat.seen;
            if (!IsSellable(bot, item))
            {
                if (item->GetTemplate() && !IsAllowedKind(item->GetTemplate()))
                {
                    ++_stat.wrongKind;
                    ++_stat.wrongKinds[item->GetEntry()];
                }
                else
                    ++_stat.bound;
                return;
            }

            // Materials for what the bot is about to craft are not for sale.
            auto plan = _plans.find(bot->GetGUID().GetCounter());
            if (plan != _plans.end() && plan->second.keepUntil > _now && Keeps(plan->second, item->GetEntry()))
                return;
            if (plan != _plans.end() && (plan->second.left || plan->second.until > _now))
                for (auto const& reagent : plan->second.recipe.reagents)
                    if (reagent.first == item->GetEntry())
                        return;

            // What nobody wanted goes to the vendor: mod-playerbots sells it there by itself.
            if (market.Tries(bot->GetGUID().GetCounter(), item->GetEntry()) >= TryLimit(bot))
                return;

            ItemTemplate const* proto = item->GetTemplate();
            // Its tools - the one rod, the one spanner - are not for sale.
            if (proto->TotemCategory && bot->GetItemCount(proto->ItemId) <= 1)
            {
                ++_stat.needed;
                return;
            }
            if (IsHandout(bot, proto))
            {
                ++_stat.needed;
                return;
            }
            // What would not be worth an auction is no reason for a trip: it goes to a vendor on the way.
            if (market.Value(proto) * bot->GetItemCount(proto->ItemId) < double(cfg.minListValue))
            {
                ++_stat.cheap;
                return;
            }

            // The bot's own judgement: only what it neither uses, wears, needs for a quest or a profession.
            ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
            // mod-playerbots has no opinion on what a vendor gives nothing for: dusts, essences, some gems.
            if (usage == ITEM_USAGE_AH || usage == ITEM_USAGE_VENDOR || (usage == ITEM_USAGE_NONE && !item->GetTemplate()->SellPrice))
                items.push_back(item);
            // To mod-playerbots a miner "needs" all ore and bars, a skinner all leather, a herbalist all herbs and
            // everybody all cloth and meat - two stacks of each before anything is for sale. A material is only
            // kept by a bot whose own craft works it. What it smelts, cooks or makes into bandages on this visit
            // is reserved by its plan; the rest is sold.
            else if ((usage == ITEM_USAGE_SKILL || usage == ITEM_USAGE_KEEP) &&
                (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_GEM) && !UsesInCraft(bot, proto->ItemId))
                items.push_back(item);
            else
            {
                ++_stat.needed;
                ++_stat.neededOnes[item->GetEntry()];
            }
        }

        void Collect(Player* bot, PlayerbotAI* botAI, std::vector<Item*>& items)
        {
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                ConsiderItem(bot, botAI, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), items);

            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        ConsiderItem(bot, botAI, bag->GetItemByPos(slot), items);
        }

        // =========================================================================================== buying

        bool HasEmptyBagSlot(Player* bot)
        {
            for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
                if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    return true;
            return false;
        }

        /// A bag the bot just bought goes into a free bag slot.
        void EquipNewBags(Player* bot)
        {
            // Wherever the new bag ended up: in the backpack or inside another bag.
            std::vector<Item*> bags;
            auto look = [&bags](Item* item)
            {
                if (item && item->GetTemplate() && item->GetTemplate()->InventoryType == INVTYPE_BAG &&
                    item->GetTemplate()->Class == ITEM_CLASS_CONTAINER && !item->IsNotEmptyBag())
                    bags.push_back(item);
            };
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                look(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        look(bag->GetItemByPos(slot));

            for (Item* item : bags)
            {
                if (!HasEmptyBagSlot(bot))
                    return;
                uint16 dest = 0;
                if (bot->CanEquipItem(NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                    continue;
                bot->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
                bot->EquipItem(dest, item, true);
            }
        }

        /// The bot looks through the offers for things it has a use for and buys or bids, within its means.
        /// Returns true if it bought something outright.
        bool Buy(Player* bot, PlayerbotAI* botAI, AuctionHouseObject* house, Index const& index,
            std::unordered_set<uint32> const& materials)
        {
            if (!cfg.buyEnabled || index.empty())
                return false;

            float const trading = TraitOf(bot, TRAIT_TRADING);
            float const patience = TraitOf(bot, TRAIT_PATIENCE);
            float const thrift = TraitOf(bot, TRAIT_THRIFT);
            float const knowledge = TraitOf(bot, TRAIT_KNOWLEDGE);

            // Not everyone browses every time.
            if (frand(0.0f, 1.0f) > 0.3f + 0.6f * trading)
                return false;
            // With bags this full the bot has to sell first; a purchase would only wait in the mail.
            if (botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get() >= 90)
                return false;

            double spendable = Spendable(bot);
            if (spendable < 1.0)
                return false;

            // A random handful of the items on offer, like a player who browses a few pages.
            std::vector<uint32> looked;
            looked.reserve(index.size());
            for (auto const& entry : index)
                looked.push_back(entry.first);
            Acore::Containers::RandomShuffle(looked);
            if (looked.size() > cfg.buyCandidates)
                looked.resize(cfg.buyCandidates);

            bool const goblin = trading > 0.75f && knowledge > 0.5f;
            // Now and then a bot that is easy with its money simply has to have something.
            bool impulse = thrift < 0.3f && urand(0, 99) < 10;
            std::unordered_map<uint32, time_t>& had = _bought[bot->GetGUID().GetCounter()];
            uint32 const maxDeals = 1 + uint32(trading * 3.0f);
            uint32 deals = 0;
            bool bought = false;

            for (uint32 const itemId : looked)
            {
                if (deals >= maxDeals)
                    break;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
                if (!proto || proto->Quality >= MAX_ITEM_QUALITY || !Market::Base(proto))
                    continue;

                // How much the bot wants it.
                bool const isMaterial = materials.find(itemId) != materials.end();
                bool resale = false;
                double interest = 0.0;
                switch (botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", itemId)->Get())
                {
                    case ITEM_USAGE_EQUIP:
                    case ITEM_USAGE_REPLACE:
                        interest = 1.5;         // an upgrade for itself
                        break;
                    case ITEM_USAGE_USE:
                    case ITEM_USAGE_SKILL:
                    case ITEM_USAGE_AMMO:
                        interest = 1.2;         // something it uses up
                        break;
                    case ITEM_USAGE_AH:
                    case ITEM_USAGE_VENDOR:
                    case ITEM_USAGE_NONE:
                        if (proto->Class == ITEM_CLASS_CONTAINER && proto->InventoryType == INVTYPE_BAG && HasEmptyBagSlot(bot))
                            interest = 1.4;     // a bag for a free bag slot
                        else if (isMaterial && bot->GetItemCount(itemId) < proto->GetMaxStackSize())
                            interest = 1.0;     // material for its profession: worth a stack when the price is right
                        else if (goblin && IsAllowedKind(proto))
                        {
                            interest = 0.6;     // a trader buys what is clearly too cheap, to sell it on
                            resale = true;
                        }
                        else if (impulse && IsAllowedKind(proto))
                        {
                            interest = frand(1.2f, 2.5f);       // the bot that simply has to have it
                            impulse = false;
                        }
                        break;
                    default:
                        break;
                }
                if (interest <= 0.0)
                    continue;
                // One of a kind is enough for now; otherwise a bot would buy the same "upgrade" again before
                // it has put the first one on.
                auto before = had.find(itemId);
                if (!isMaterial && !resale && before != had.end() && before->second + 4 * HOUR > _now)
                    continue;

                // What one piece is worth to this bot today. The better off it is, the less it haggles.
                std::vector<Offer> const& offers = index.find(itemId)->second;
                double const value = market.Value(proto);
                double each = value * interest * frand(0.8f, 1.15f);
                if (!resale)
                {
                    double const dear = value * double(offers.front().count);
                    each *= std::clamp(1.0 + 0.25 * std::log10(std::max(1.0, spendable / std::max(1.0, dear))), 0.85, 1.4);
                    // Always more than a vendor pays, or nobody would bother with the auction house ...
                    each = std::max(each, double(proto->SellPrice) * cfg.buyMinVendorFactor);
                }
                // ... but less than a vendor asks, or buying from a vendor and selling to the bots would print money.
                if (proto->BuyPrice > 0 && market.IsVendorItem(itemId))
                    each = std::min(each, double(proto->BuyPrice) / std::max<uint32>(1, proto->BuyCount) * cfg.buyVendorItemPercent / 100.0);

                double const purse = std::min(spendable * 0.6, cfg.maxBuyout ? double(cfg.maxBuyout) : double(MAX_MONEY_AMOUNT));

                // The cheapest offer to buy outright, and the lowest bid that would do.
                Offer const* buy = nullptr;
                Offer const* bid = nullptr;
                for (Offer const& offer : offers)
                {
                    if (offer.mine || !offer.buyable)
                        continue;
                    if (!buy && offer.buyout && double(offer.buyout) <= each * offer.count && double(offer.buyout) <= purse)
                        buy = &offer;
                    if (cfg.buyBids && !resale && offer.nextBid && (!offer.buyout || offer.nextBid < offer.buyout) &&
                        double(offer.nextBid) <= each * offer.count * 0.8 && double(offer.nextBid) <= purse &&
                        (!bid || double(offer.nextBid) / offer.count < double(bid->nextBid) / bid->count))
                        bid = &offer;
                }

                // The patient bid low and wait; the impatient pay the buyout price.
                if (!buy && !bid)
                    continue;
                bool const buyNow = patience > 0.6f ? !bid : buy != nullptr;
                Offer const* chosen = buyNow ? buy : bid;
                AuctionEntry* auction = house->GetAuction(chosen->id);
                if (!auction || !CanBuyFrom(bot, auction))
                    continue;

                if (buyNow)
                {
                    uint32 const price = auction->buyout;
                    if (!price || double(price) > purse || !Buyout(bot, house, auction, proto))
                        continue;
                    spendable -= price;
                    bought = true;
                }
                else
                {
                    uint32 const price = std::max(auction->startbid, auction->bid ? auction->bid + auction->GetAuctionOutBid() : auction->startbid);
                    if (!price || (auction->buyout && price >= auction->buyout) || double(price) > each * auction->itemCount * 0.8 ||
                        double(price) > purse || !Bid(bot, auction, proto, price))
                        continue;
                    spendable -= price;
                }
                had[itemId] = _now;
                ++deals;
            }
            return bought;
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

        // =========================================================================================== crafting

        /// What making a recipe a number of times would bring and cost.
        struct Estimate
        {
            bool ok = false;
            bool missing = false;       // a material is not to be had
            double worth = 0.0, cost = 0.0, toPay = 0.0;
            std::vector<uint32> toBuy;      // auctions
        };

        /// The bot works out a recipe: what would the products bring, what do the materials cost - those it
        /// carries (it could sell them instead), those on offer here, those from a vendor?
        Estimate WorkOut(Player* bot, PlayerbotAI* botAI, Recipe const& recipe, uint32 times, Index const& index,
            double purse, bool mayBuy, double skillBonus, bool intermediate)
        {
            Estimate estimate;
            if (TakesApart(recipe.kind))
            {
                // Taking apart: what comes out on average, at its usual price. Gear is disenchanted piece by piece.
                if (!recipe.yield || (recipe.kind == KIND_DISENCHANT && times > 1))
                    return estimate;
                double value = 0.0;
                for (Yield const& yield : *recipe.yield)
                    if (ItemTemplate const* got = sObjectMgr->GetItemTemplate(yield.item))
                        if (got->Quality < MAX_ITEM_QUALITY && Market::Base(got))
                            value += market.Value(got) * yield.count;
                estimate.worth = value * (intermediate ? 1.2 : 0.95) * times;
                return Materials(bot, recipe, times, index, purse, mayBuy, estimate);
            }

            ItemTemplate const* product = sObjectMgr->GetItemTemplate(recipe.product);
            if (!product || product->Quality >= MAX_ITEM_QUALITY)
                return estimate;
            // A tool for its own work it makes once, whatever it would bring.
            bool const ownTool = recipe.ownTool && !bot->HasItemCount(product->ItemId, 1);
            if (ownTool ? times > 1 : !Market::Base(product))
                return estimate;

            // What the product is worth to the bot: its price minus the auction house's cut if it sells it,
            // a little more if it can use it itself - but it only needs one.
            ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", product->ItemId)->Get();
            // Something it wears or uses - or works on: the bars a smith smelts from its ore are for its own anvil.
            bool const forItself = intermediate || ownTool || usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_USE;
            bool const forSale = IsAllowedKind(product);
            if (!forItself && !forSale)
                return estimate;
            if (!forSale && !intermediate && times > 1)
                return estimate;
            double const value = (Market::Base(product) ? market.Value(product) : 0.0) * recipe.made;
            estimate.worth = value * (forItself ? 1.2 : 0.95) + value * 0.95 * (times - 1);
            // A recipe that still raises its skill is worth more to it - once; the next one may not.
            bool const skillUp = recipe.kind != KIND_ENCHANT && ItemUsageValue::SpellGivesSkillUp(recipe.spell, bot);
            if (skillUp)
                estimate.worth += value * skillBonus;
            // A lot that would not be worth an auction is not made as a batch - a single one at most, and only
            // if the bot can use it itself or learns from it. A step towards another recipe is a different matter.
            // What it already carries of it counts: one more cheap potion fills up a pack worth offering.
            double const lot = value * times + market.Value(product) * bot->GetItemCount(product->ItemId);
            if (!intermediate && lot < double(cfg.minListValue) && (times > 1 || (!forItself && !skillUp)))
                return estimate;

            estimate = Materials(bot, recipe, times, index, purse, mayBuy, estimate);
            if (ownTool && !estimate.missing)
            {
                estimate.worth = std::max(estimate.worth, estimate.cost * 1.5 + 1.0);
                estimate.ok = estimate.toPay <= purse;
            }
            return estimate;
        }

        /// The other side of the sum: what the materials cost - those it carries, those on offer, those from a vendor.
        Estimate Materials(Player* bot, Recipe const& recipe, uint32 times, Index const& index, double purse, bool mayBuy, Estimate estimate)
        {
            for (auto const& reagent : recipe.reagents)
            {
                ItemTemplate const* material = sObjectMgr->GetItemTemplate(reagent.first);
                if (!material)
                    return estimate;
                bool const priced = material->Quality < MAX_ITEM_QUALITY && Market::Base(material);
                double const usual = recipe.sourceValue > 0.0 ? recipe.sourceValue : priced ? market.Value(material) : 0.0;

                uint32 const wanted = reagent.second * times;
                uint32 const have = std::min(bot->GetItemCount(reagent.first), wanted);
                estimate.cost += usual * have;
                uint32 need = wanted - have;
                if (!need)
                    continue;

                if (cfg.matsVendor && material->BuyPrice > 0 && market.IsVendorSupply(reagent.first))
                {
                    double const price = double(material->BuyPrice) / std::max<uint32>(1, material->BuyCount) * need;
                    estimate.cost += price;
                    estimate.toPay += price;
                    continue;
                }
                if (!mayBuy)
                {
                    estimate.missing = true;
                    return estimate;
                }

                // The bot does not pay any price because the product is valuable: a material that costs far
                // more than usual is left alone, however well the recipe would pay.
                auto found = index.find(reagent.first);
                if (found != index.end())
                    for (Offer const& offer : found->second)
                    {
                        if (!need || !offer.buyout || double(offer.each) > usual * cfg.matsMaxPrice)
                            break;
                        if (!offer.buyable || offer.mine || (cfg.maxBuyout && offer.buyout > cfg.maxBuyout))
                            continue;
                        uint32 const used = std::min(need, offer.count);
                        // It pays for the whole stack; what is left over it can sell again, for a little less.
                        estimate.cost += double(offer.buyout) - double(offer.each) * (offer.count - used) * 0.9;
                        estimate.toPay += offer.buyout;
                        estimate.toBuy.push_back(offer.id);
                        need -= used;
                    }
                if (need)
                {
                    estimate.missing = true;
                    return estimate;
                }
            }

            // A tool it lacks is bought once and stays: it has to be able to afford it, no more.
            for (uint32 const tool : recipe.tools)
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tool))
                    estimate.toPay += double(proto->BuyPrice);
            estimate.ok = estimate.toPay <= purse && estimate.worth >= estimate.cost * (1.0 + cfg.matsMinProfit / 100.0);
            return estimate;
        }

        /// What kind of crafter the bot is decides how it chooses:
        ///  - for the casual one the profession is a sideline: it makes the first thing that works out with
        ///    what it carries;
        ///  - the one in between provides for itself and works on its skill, and buys what is missing;
        ///  - the producer thinks through all its recipes, takes the one that pays best and makes several.
        /// Returns true if the bot bought materials.
        bool PlanCraft(Player* bot, PlayerbotAI* botAI, AuctionHouseObject* house, Index const& index, std::vector<Recipe> recipes)
        {
            ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
            auto existing = _plans.find(guid);
            // Back from the forge with bars of its own: now it decides whether to work them or to sell them.
            bool const followUp = existing != _plans.end() && existing->second.next && !existing->second.left;
            uint32 const steps = followUp ? existing->second.steps + 1 : 0;
            if (followUp)
            {
                // Whatever it decides now: what it kept back is free again. A new plan reserves what it needs.
                existing->second.next = false;
                existing->second.keep.clear();
            }
            else
            {
                if (existing != _plans.end() && (existing->second.left || existing->second.until > _now))
                    return false;       // still busy with the last one
                if (frand(0.0f, 1.0f) > ActivityNow(_now))
                    return false;
            }

            float const crafting = TraitOf(bot, TRAIT_CRAFTING);
            bool const casual = crafting < 0.25f;
            bool const producer = crafting > 0.6f;
            uint32 const most = producer ? cfg.craftBatch : casual ? 1 : 2;
            double const skillBonus = cfg.matsSkillBonus / 100.0 * (casual || producer ? 1.0 : 1.5);
            double const purse = Spendable(bot) * 0.6;

            // What goes into its own recipes. Making one of those - smelting ore into bars - is a step on the way.
            std::unordered_set<uint32> ownMaterials;
            std::unordered_map<uint32, std::unordered_set<uint32>> madeFrom;        // material -> what it is made into
            for (Recipe const& recipe : recipes)
                if (!TakesApart(recipe.kind))
                    for (auto const& reagent : recipe.reagents)
                    {
                        ownMaterials.insert(reagent.first);
                        madeFrom[reagent.first].insert(recipe.product);
                    }

            Acore::Containers::RandomShuffle(recipes);
            uint32 const look = casual ? std::min<uint32>(10, cfg.matsRecipes) : cfg.matsRecipes;
            if (recipes.size() > look)
                recipes.resize(look);

            Recipe const* best = nullptr;
            Estimate bestEstimate;
            uint32 bestTimes = 0;
            for (Recipe const& recipe : recipes)
            {
                // Does what comes of it go into another of its recipes?
                bool intermediate = ownMaterials.find(recipe.product) != ownMaterials.end();
                // ... but not in a circle: what can be turned back into what it was made of is no step forward.
                if (intermediate)
                    for (auto const& reagent : recipe.reagents)
                        if (madeFrom[recipe.product].count(reagent.first))
                            intermediate = false;
                if (TakesApart(recipe.kind) && recipe.yield)
                    for (Yield const& yield : *recipe.yield)
                        if (ownMaterials.find(yield.item) != ownMaterials.end())
                            intermediate = true;

                for (uint32 times = most; times >= 1; --times)
                {
                    Estimate estimate = WorkOut(bot, botAI, recipe, times, index, purse,
                        cfg.matsEnabled && !casual && !intermediate,       // for a step on the way it buys nothing
                        skillBonus, intermediate);
                    if (times == 1)
                    {
                        // For the summary: which professions the bots have, and what comes of their recipes.
                        uint32 skill = 0;
                        if (!TakesApart(recipe.kind))
                        {
                            SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(recipe.spell);
                            if (bounds.first != bounds.second)
                                skill = bounds.first->second->SkillLine;
                        }
                        uint32* counts = _stat.recipes[skill];
                        ++counts[0];
                        if (estimate.ok)
                            ++counts[3];
                        else if (estimate.missing)
                            ++counts[1];
                        else
                        {
                            ++counts[2];
                            if (cfg.debug && _stat.noPay.size() < 12)
                                if (ItemTemplate const* made = sObjectMgr->GetItemTemplate(TakesApart(recipe.kind) ? recipe.reagents.front().first : recipe.product))
                                    _stat.noPay.push_back(Acore::StringFormat("{} {} (q{} c{}): worth {}, materials {}", KindName(recipe.kind),
                                        made->Name1, made->Quality, made->Class, uint64(estimate.worth), uint64(estimate.cost)));
                        }
                    }
                    if (!estimate.ok)
                        continue;
                    if (!best || estimate.worth - estimate.cost > bestEstimate.worth - bestEstimate.cost)
                    {
                        best = &recipe;
                        bestEstimate = std::move(estimate);
                        bestTimes = times;
                    }
                    break;      // the largest number that works out is the one that counts
                }
                if (best && casual)
                    break;      // good enough
            }
            if (!best)
                return false;       // after a step: nothing worth making of what it kept, it is sold as it is

            // It pays. Buy what is missing; if someone was quicker, the plan is dropped.
            bool bought = false;
            for (uint32 const id : bestEstimate.toBuy)
            {
                AuctionEntry* auction = house->GetAuction(id);
                ItemTemplate const* material = auction ? sObjectMgr->GetItemTemplate(auction->item_template) : nullptr;
                if (!auction || !material || !auction->buyout || !CanBuyFrom(bot, auction) || !Buyout(bot, house, auction, material))
                    return bought;
                bought = true;
            }

            Plan& plan = _plans[guid];
            plan.recipe = *best;
            plan.left = bestTimes;
            plan.fails = 0;
            plan.until = _now + 10 * MINUTE;
            plan.keep.clear();
            if (!TakesApart(best->kind))
            {
                if (ownMaterials.find(best->product) != ownMaterials.end())
                    plan.keep.push_back(best->product);
            }
            else
                for (Yield const& yield : *best->yield)
                    if (ownMaterials.find(yield.item) != ownMaterials.end())
                        plan.keep.push_back(yield.item);
            plan.keepUntil = _now + 30 * MINUTE;
            plan.steps = steps;
            plan.next = false;
            if (cfg.debug && TakesApart(best->kind))
                if (ItemTemplate const* source = sObjectMgr->GetItemTemplate(best->reagents.front().first))
                    LOG_INFO("module", "PlayerbotsAuctions: {} plans to {} {} x{} (item {}): worth {} copper as it is, {} copper taken apart{}.",
                        bot->GetName(), KindName(best->kind), source->Name1, bestTimes * best->reagents.front().second, source->ItemId,
                        uint64(bestEstimate.cost), uint64(bestEstimate.worth), plan.keep.empty() ? "" : ", for its own recipes");
            if (cfg.debug)
                if (ItemTemplate const* product = sObjectMgr->GetItemTemplate(best->product))
                    LOG_INFO("module", "PlayerbotsAuctions: {} plans to craft {} x{} (item {}): materials worth {} copper, product worth {} copper to it{}.",
                        bot->GetName(), product->Name1, bestTimes, product->ItemId, uint64(bestEstimate.cost), uint64(bestEstimate.worth),
                        best->focus ? ", and it has to walk to its work place" : "");
            return bought;
        }

        /// Sets the bot to work on its plan: on the spot, or after a walk to the anvil, forge or fire.
        void StartWork(Player* bot, PlayerbotAI* botAI, time_t now)
        {
            auto plan = _plans.find(bot->GetGUID().GetCounter());
            if (plan == _plans.end() || !plan->second.left)
                return;

            if (plan->second.recipe.focus)
            {
                Focus const* focus = NearestFocus(bot, plan->second.recipe.focus);
                if (!focus || !WalkInTown(bot, botAI, focus->x, focus->y, focus->z))
                {
                    _plans.erase(plan);
                    return;
                }
                SetWatch(bot, now + 2, now + 8 * MINUTE, TASK_CRAFT);
            }
            else
                SetWatch(bot, now + 2, now + 4 * MINUTE, TASK_CRAFT);
        }

        /// Thread, vials, flux: what a trade vendor sells for money in any quantity the bot gets from the
        /// vendor and pays the vendor's price.
        bool BuyFromVendor(Player* bot, Recipe const& recipe)
        {
            for (uint32 const tool : recipe.tools)
            {
                if (bot->HasItemCount(tool, 1))
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(tool);
                ItemPosCountVec dest;
                if (!proto || !market.IsVendorSupply(tool) || !bot->HasEnoughMoney(uint32(proto->BuyPrice)) ||
                    bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, tool, 1) != EQUIP_ERR_OK)
                    return false;
                bot->ModifyMoney(-int32(proto->BuyPrice));
                bot->StoreNewItem(dest, tool, true);
            }
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

        /// The bot is done crafting, one way or the other: back to the auctioneers to sell what it has.
        bool EndWork(Player* bot, PlayerbotAI* botAI, time_t now, time_t free, bool walked)
        {
            auto plan = _plans.find(bot->GetGUID().GetCounter());
            if (plan != _plans.end())
            {
                plan->second.left = 0;
                plan->second.until = free;      // the materials of a cast still running stay reserved
            }
            // What it made goes into its own recipes: at the auctioneers it thinks about the next step.
            // (The last cast may still be running, so it is not asked whether the bars are in the bags yet.)
            bool const chain = plan != _plans.end() && !plan->second.keep.empty() && plan->second.steps < 3;
            if (chain)
            {
                plan->second.next = true;
                plan->second.keepUntil = now + 15 * MINUTE;
            }
            else if (plan != _plans.end())
                plan->second.keep.clear();
            if (walked && botAI)
                SendToAuctionHouse(bot, botAI);
            SetWatch(bot, free + 1, now + 8 * MINUTE, chain ? TASK_VISIT : TASK_SELL);
            return true;
        }

        /// One look at a bot that has a plan. Returns true when the look is over (the watch may have been
        /// set anew for the next step).
        bool Work(Player* bot, time_t now)
        {
            _now = now;
            ObjectGuid::LowType const guid = bot->GetGUID().GetCounter();
            auto found = _plans.find(guid);
            if (found == _plans.end() || !found->second.left)
                return true;
            Plan& plan = found->second;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI || !bot->IsAlive())
                return EndWork(bot, botAI, now, now, false);
            if (bot->IsInCombat())
                return false;       // in a moment
            if (bot->IsNonMeleeSpellCast(false))
            {
                StayPut(bot, botAI, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
                return false;
            }

            // Recipes for the anvil, the forge or the fire are made there.
            if (plan.recipe.focus)
            {
                Focus const* focus = NearestFocus(bot, plan.recipe.focus);
                if (!focus)
                    return EndWork(bot, botAI, now, now, true);
                float const distance = bot->GetExactDist(focus->x, focus->y, focus->z);
                float const reach = std::max(3.0f, focus->range - 2.0f);
                if (distance > reach)
                {
                    if (distance < 30.0f)
                    {
                        // The last steps it takes itself, to a spot a little short of the anvil - and
                        // mod-playerbots is kept from walking it off to the next townsman meanwhile.
                        float const part = std::min(3.0f, reach * 0.5f) / distance;
                        float const x = focus->x + (bot->GetPositionX() - focus->x) * part;
                        float const y = focus->y + (bot->GetPositionY() - focus->y) * part;
                        StayPut(bot, botAI, x, y, focus->z);
                        if (!bot->isMoving())
                            bot->GetMotionMaster()->MovePoint(0, x, y, focus->z);
                    }
                    else if (!bot->isMoving())
                        WalkInTown(bot, botAI, focus->x, focus->y, focus->z);       // mod-playerbots finds the way
                    return false;
                }
            }

            // It stays where it works until it is done.
            StayPut(bot, botAI, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

            // Crafting is done standing, on foot and not on the move.
            bot->StopMoving();
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            if (!bot->IsStandState())
                bot->SetStandState(UNIT_STAND_STATE_STAND);

            bool ready = !cfg.matsVendor || BuyFromVendor(bot, plan.recipe);
            for (auto const& reagent : plan.recipe.reagents)
                if (bot->GetItemCount(reagent.first) < reagent.second)
                    ready = false;      // the materials have run out, or did not arrive
            if (!ready)
                return EndWork(bot, botAI, now, now, plan.recipe.focus != 0);

            if (plan.recipe.kind != KIND_CRAFT)
            {
                if (!Refine(bot, plan.recipe))
                    return EndWork(bot, botAI, now, now, false);
                if (cfg.debug && plan.recipe.kind == KIND_ENCHANT)
                {
                    if (ItemTemplate const* scroll = sObjectMgr->GetItemTemplate(plan.recipe.product))
                        LOG_INFO("module", "PlayerbotsAuctions: {} puts an enchantment on vellum: {} (item {}).", bot->GetName(),
                            scroll->Name1, scroll->ItemId);
                }
                else if (cfg.debug)
                    if (ItemTemplate const* source = sObjectMgr->GetItemTemplate(plan.recipe.reagents.front().first))
                        LOG_INFO("module", "PlayerbotsAuctions: {} {}s {} (item {}).", bot->GetName(), KindName(plan.recipe.kind),
                            source->Name1, source->ItemId);
                time_t const done = now + 3;
                if (--plan.left)
                {
                    plan.until = now + 10 * MINUTE;
                    SetWatch(bot, done, done + 4 * MINUTE, TASK_CRAFT);
                    return true;
                }
                return EndWork(bot, botAI, now, done, false);
            }

            if (!botAI->CastSpell(plan.recipe.spell, bot))
            {
                if (++plan.fails >= 3)
                    return EndWork(bot, botAI, now, now, plan.recipe.focus != 0);
                return false;
            }

            SpellInfo const* info = sSpellMgr->GetSpellInfo(plan.recipe.spell);
            time_t const done = now + (info ? info->CalcCastTime(bot) / IN_MILLISECONDS : 3) + 1;
            if (cfg.debug)
                if (ItemTemplate const* product = sObjectMgr->GetItemTemplate(plan.recipe.product))
                    LOG_INFO("module", "PlayerbotsAuctions: {} crafts {} (item {}){}.", bot->GetName(), product->Name1, product->ItemId,
                        plan.recipe.focus ? " at its work place" : "");

            plan.fails = 0;
            if (--plan.left)
            {
                plan.until = now + 10 * MINUTE;
                SetWatch(bot, done, done + 4 * MINUTE, TASK_CRAFT);      // the next one, when this one is done
                return true;
            }
            return EndWork(bot, botAI, now, done, plan.recipe.focus != 0);
        }

        // =========================================================================================== mail

        /// Did this mail bring back an item nobody bought?
        static bool IsExpiredAuction(Mail const* mail)
        {
            // The subject of an auction mail reads "item:0:answer:auction:count".
            std::vector<std::string_view> const parts = Acore::Tokenize(mail->subject, ':', true);
            if (parts.size() < 3)
                return false;
            Optional<uint32> const answer = Acore::StringTo<uint32>(parts[2]);
            return answer && *answer == AUCTION_EXPIRED;
        }

        /// The bot empties its auction mail: the money of sold items, the items nobody bought and what it
        /// bought itself. Without this the mail would pile up, because the bots never open a mailbox for it.
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

                bool const expired = !mail->items.empty() && IsExpiredAuction(mail);
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
                    if (expired)
                        market.AddTry(bot->GetGUID().GetCounter(), info.item_template);      // remembered: nobody wanted it at that price
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

            if (returned)
                EquipNewBags(bot);      // a bag it bought goes into a free bag slot

            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} collected {} copper and {} item(s) from its auction mail.",
                    bot->GetName(), money, returned);
        }

        uint32 _timer = 0;
        uint32 _watchTimer = 0;
        struct Stat
        {
            uint32 asked = 0, withGoods = 0, mostGoods = 0, trips = 0, noWay = 0, visits = 0;
            uint32 seen = 0, wrongKind = 0, bound = 0, needed = 0, vendored = 0, cheap = 0;
            std::unordered_map<uint32, uint32> wrongKinds, neededOnes;
            std::map<uint32, uint32[4]> recipes;        // skill -> thought through, material missing, does not pay, works out
            std::vector<std::string> noPay;             // a few of those that do not pay, with the sum
        };
        Stat _stat;
        struct Gathered
        {
            uint64 pieces = 0;
            std::set<ObjectGuid::LowType> bots;
        };
        std::mutex _gatherLock;
        std::map<std::string, Gathered> _gathered;
        std::atomic<uint32> _kept{0};
        uint32 _statTimer = 0;
        uint32 _saveTimer = 0;
        uint32 _decisions = 0;
        time_t _now = 0;
        ObjectGuid _last;
        std::unordered_map<ObjectGuid::LowType, time_t> _nextVisit;
        std::unordered_map<ObjectGuid::LowType, time_t> _nextTrip;
        std::unordered_map<ObjectGuid::LowType, Plan> _plans;
        std::unordered_map<ObjectGuid::LowType, std::unordered_map<uint32, time_t>> _bought;       // what it bought, and when
        std::map<ObjectGuid, Watch> _watch;                 // bots on an errand
    };

    Life life;
}
}

class PlayerbotsAuctionsWorld : public WorldScript
{
public:
    PlayerbotsAuctionsWorld() : WorldScript("PlayerbotsAuctionsWorld") { }

    void OnAfterConfigLoad(bool reload) override
    {
        pba::LoadSettings();
        if (reload)
            pba::LoadMadeValues();     // priced with the settings
    }

    void OnStartup() override
    {
        // Loaded even if the module is switched off, so it can be switched on while the server runs.
        pba::market.LoadVendorItems();
        pba::LoadFocusObjects();
        pba::LoadYields();
        pba::LoadPotionRecipes();
        pba::LoadMadeValues();
        pba::market.LoadMemory();
        if (pba::cfg.enabled)
            LOG_INFO("server.loading", ">> PlayerbotsAuctions: the bots use the auction house.");
    }

    void OnUpdate(uint32 diff) override
    {
        pba::life.Update(diff);
    }

    void OnShutdown() override
    {
        pba::market.SaveMemory();
    }
};

class PlayerbotsAuctionsPlayer : public PlayerScript
{
public:
    PlayerbotsAuctionsPlayer() : PlayerScript("PlayerbotsAuctionsPlayer", { PLAYERHOOK_CAN_SELL_ITEM, PLAYERHOOK_ON_LOOT_ITEM }) { }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid source) override
    {
        pba::life.Looted(player, item, count, source);
    }

    bool OnPlayerCanSellItem(Player* player, Item* item, Creature* /*vendor*/) override
    {
        return !pba::life.KeepsFromVendor(player, item);
    }
};

class PlayerbotsAuctionsHouse : public AuctionHouseScript
{
public:
    PlayerbotsAuctionsHouse() : AuctionHouseScript("PlayerbotsAuctionsHouse") { }

    void OnAuctionSuccessful(AuctionHouseObject* /*house*/, AuctionEntry* auction) override
    {
        if (!auction || !pba::cfg.enabled)
            return;
        // It found a buyer: whatever was remembered about it not selling is over.
        pba::market.Forget(auction->owner.GetCounter(), auction->item_template);

        // Only sales a bot took part in teach the bots what things go for. Two players (or one player with
        // two accounts) trading an item back and forth at a fantasy price must not teach them anything.
        uint32 const seller = sCharacterCache->GetCharacterAccountIdByGuid(auction->owner);
        uint32 const buyer = sCharacterCache->GetCharacterAccountIdByGuid(auction->bidder);
        if ((seller && sPlayerbotAIConfig.IsInRandomAccountList(seller)) || (buyer && sPlayerbotAIConfig.IsInRandomAccountList(buyer)))
            pba::market.RecordSale(auction->item_template, auction->bid, auction->itemCount);
    }
};

void AddSC_playerbots_auctions()
{
#if __has_include("AscensionCoAConfig.h")
    // The CoA client works out the deposit of an auction itself and asks the server for the deposit rate.
    // The core tells it its XP rates only, so the client stops with a Lua error ("depositMulti" is nil) as
    // soon as a player puts an item into the auction window. With the rate sent along, players can sell.
    RegisterAscensionClientConfig([](AscensionClientConfig& config)
    {
        for (auto const& rate : config.Rates)
            if (rate.first == "RATE_AUCTION_DEPOSIT")
                return;     // the core sends it itself by now
        config.Rates.emplace_back("RATE_AUCTION_DEPOSIT", sWorld->getRate(RATE_AUCTION_DEPOSIT));
        config.Rates.emplace_back("RATE_AUCTION_CUT", sWorld->getRate(RATE_AUCTION_CUT));
        config.Rates.emplace_back("RATE_AUCTION_TIME", sWorld->getRate(RATE_AUCTION_TIME));
    });
#endif
    new PlayerbotsAuctionsWorld();
    new PlayerbotsAuctionsHouse();
    new PlayerbotsAuctionsPlayer();
}
