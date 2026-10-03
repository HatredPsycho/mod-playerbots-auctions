/*
 * mod-playerbots-auctions - the random bots of mod-playerbots take part in the economy like players.
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 *
 * Every bot has a character of its own. From it follows when the bot goes to the auction house, what it
 * sells there and for how much, what it buys and what it is willing to pay, and what it makes with its
 * professions. Nothing in mod-playerbots is changed; the module only uses what it offers.
 */

#ifndef MOD_PLAYERBOTS_AUCTIONS_H
#define MOD_PLAYERBOTS_AUCTIONS_H

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
#include "LastMovementValue.h"
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
#include "Timer.h"
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
#include <atomic>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pba
{
    // ------------------------------------------------------------------------------------------ settings

    struct Settings
    {
        bool   enabled = false;
        bool   debug = false;
        uint32 intervalMs = 30000;
        uint32 botsPerCycle = 10;
        float  auctioneerRange = 35.0f;
        uint32 minLevel = 1;
        uint32 visitCooldownMin = 20 * 60;     // seconds
        uint32 visitCooldownMax = 60 * 60;
        bool   collectMail = true;
        uint32 avoidersPercent = 15;           // bots that never use the auction house
        bool   rhythm = true;                  // more going on in the evening and at the weekend
        bool   saveMemory = true;

        // selling
        uint32 maxAuctionsPerHouse = 20000;
        uint32 minQualityEquipment = ITEM_QUALITY_UNCOMMON;
        uint32 minQualityOther = ITEM_QUALITY_NORMAL;
        uint32 maxQuality = ITEM_QUALITY_EPIC;
        std::unordered_set<uint32> itemClasses;
        std::unordered_set<uint32> excludedItems;
        std::vector<std::string> excludedNameParts;
        bool   chargeDeposit = true;
        uint32 minListValue = 1 * SILVER;      // an auction worth less than this is not worth the walk
        bool   keepFromVendor = true;          // what is meant for the auction house is not sold to a vendor on the way

        // prices
        float  priceMultiplier[MAX_ITEM_QUALITY] = { };
        float  minVendorFactor = 1.5f;
        uint32 maxBuyout = 5000 * GOLD;        // 0 = no limit
        bool   learnPrices = true;

        // buying
        bool   buyEnabled = true;
        uint32 buyCandidates = 60;
        bool   buyFromBots = true;
        bool   buyFromPlayers = true;
        bool   buyBids = true;
        bool   buyUseBotMoney = true;
        float  buyMinVendorFactor = 1.2f;
        uint32 buyVendorItemPercent = 75;

        // trips
        bool   cityTrips = true;
        uint32 cityCooldownMin = 60 * 60;      // seconds
        uint32 cityCooldownMax = 180 * 60;
        uint32 cityItemsMin = 2;               // things to sell the tidiest bot sets out for
        uint32 cityItemsMax = 8;               // ... and the one that collects the longest

        // crafting
        bool   craftEnabled = true;
        bool   matsEnabled = true;
        uint32 matsMinProfit = 10;             // percent the product has to be worth more than its materials
        uint32 matsSkillBonus = 30;            // percent extra a recipe is worth while it gives skill
        uint32 matsRecipes = 40;               // recipes a bot thinks through per visit
        bool   matsVendor = true;
        float  matsMaxPrice = 1.5f;            // a material costing more than this times its usual price is left alone
        bool   craftFocus = true;              // walk to an anvil, a forge or a fire when a recipe needs one
        uint32 craftBatch = 5;                 // the most a producer makes of one thing in a row
        bool   refine = true;                  // prospecting, milling, disenchanting
    };

    extern Settings cfg;
    void LoadSettings();
    std::string Lower(std::string text);

    // ------------------------------------------------------------------------------------------ character

    /// What makes one bot different from the next. Every trait is a number from 0 to 1 that belongs to
    /// the bot and never changes.
    enum Trait : uint32
    {
        TRAIT_ORDER = 1,    // low: collects until nothing fits   high: tidies up early
        TRAIT_PATIENCE,     // low: wants to be rid of it now     high: waits for a good price
        TRAIT_KNOWLEDGE,    // low: prices by gut feeling         high: knows the market
        TRAIT_THRIFT,       // low: spends what it has            high: keeps a reserve
        TRAIT_TRADING,      // low: hardly uses the auction house high: lives there
        TRAIT_CRAFTING,     // low: profession is a sideline      high: produces on purpose
        TRAIT_GREED,        // its own price level, a little below or above the others
        TRAIT_HABIT         // small habits: how it sets bids, how it packs stacks
    };

    float TraitOf(Player* bot, Trait which);
    bool AvoidsAuctionHouse(Player* bot);

    /// How much is going on at this time of the day and week, from 0.25 (dead of night) to 1 (evening).
    float ActivityNow(time_t now);

    // ------------------------------------------------------------------------------------------ items

    bool IsEquipment(ItemTemplate const* proto);
    bool IsAllowedKind(ItemTemplate const* proto);
    bool IsSellable(Player* bot, Item* item);

    /// A price as a person would type it: two digits that matter, the rest zeros.
    uint32 HumanPrice(double copper);

    // ------------------------------------------------------------------------------------------ market

    /// What an item is worth, and what the bots remember. The starting point of every price is the vendor
    /// value times the factor of its quality; what auctions really sold for moves it, within limits.
    class Market
    {
    public:
        /// What the calculation starts from: the vendor value. Things a vendor gives nothing for - dusts,
        /// essences, some gems - get a value from their item level instead, or they could never be traded.
        static uint32 Base(ItemTemplate const* proto);
        static double Regular(ItemTemplate const* proto);
        double Value(ItemTemplate const* proto) const;
        void RecordSale(uint32 itemId, uint32 price, uint32 count);

        void LoadVendorItems();
        bool IsVendorItem(uint32 itemId) const { return _vendorItems.find(itemId) != _vendorItems.end(); }
        /// Sold by a vendor for money in any quantity: thread, vials, flux and the like.
        bool IsVendorSupply(uint32 itemId) const { return _vendorSupplies.find(itemId) != _vendorSupplies.end(); }

        // How often a seller's item came back unsold. Remembered per seller and kind of item, because a
        // pack that comes back melts into the stack it was taken from.
        uint32 Tries(ObjectGuid::LowType owner, uint32 itemId) const;
        void AddTry(ObjectGuid::LowType owner, uint32 itemId);
        void Forget(ObjectGuid::LowType owner, uint32 itemId);

        // The memory survives a restart in two small tables of the characters database.
        void LoadMemory();
        void SaveMemory();

    private:
        struct Learned
        {
            double each = 0.0;      // price of one piece
            uint32 sales = 0;
        };
        std::unordered_map<uint32, Learned> _sold;
        std::unordered_set<uint32> _dirty;
        std::unordered_map<uint64, uint32> _tries;        // seller and item -> times
        std::unordered_set<uint32> _vendorItems;
        std::unordered_set<uint32> _vendorSupplies;
        bool _tables = false;
    };

    extern Market market;

    // ------------------------------------------------------------------------------------------ places

    struct Focus
    {
        uint32 id = 0;          // which kind: anvil, forge, cooking fire, ...
        uint32 mapId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float range = 10.0f;
    };

    enum Journey { JOURNEY_NONE, JOURNEY_WALK, JOURNEY_HEARTH };

    Creature* FindAuctioneer(Player* bot, PlayerbotAI* botAI);
    /// The auction house the bot does its business in, or false if it is not at an auctioneer.
    bool FindHouse(Player* bot, PlayerbotAI* botAI, AuctionHouseId& houseId);
    bool IsInTown(Player* bot);
    /// Sends a bot to the auctioneers: on foot if it is in town, by hearth and then on foot if not.
    Journey SendToAuctionHouse(Player* bot, PlayerbotAI* botAI);
    /// A bot that took its hearth to another continent arrives without knowing why it came: mod-playerbots
    /// clears its head on the way. Here it is told again - where the auctioneers are and where to return to.
    void ResumeJourney(Player* bot, PlayerbotAI* botAI);
    /// Lets a bot that is in town walk to a place in it.
    bool WalkInTown(Player* bot, PlayerbotAI* botAI, float x, float y, float z);
    /// Can this bot be sent anywhere at all?
    bool CanTravel(PlayerbotAI* botAI);
    /// Keeps mod-playerbots from walking the bot off for the next few seconds.
    void StayPut(Player* bot, PlayerbotAI* botAI, float x, float y, float z);

    void LoadFocusObjects();
    /// The nearest anvil, forge or fire of that kind in the town the bot is in.
    Focus const* NearestFocus(Player* bot, uint32 focusId);

    // ------------------------------------------------------------------------------------------ recipes

    enum Kind : uint8 { KIND_CRAFT, KIND_PROSPECT, KIND_MILL, KIND_DISENCHANT };

    /// What comes of taking something apart, on average.
    struct Yield
    {
        uint32 item = 0;
        float count = 0.0f;
    };

    /// One thing a bot can make with a profession.
    struct Recipe
    {
        Kind kind = KIND_CRAFT;
        std::vector<Yield> const* yield = nullptr;             // prospecting, milling, disenchanting: what comes out
        double sourceValue = 0.0;                               // ... and what the thing is worth to it as it is, if not its usual price
        uint32 spell = 0;
        uint32 product = 0;
        uint32 made = 1;                                        // pieces per cast
        uint32 focus = 0;                                       // needs an anvil, a forge, a fire
        std::vector<std::pair<uint32, uint32>> reagents;        // item, count
    };

    /// What this bot can craft here: recipes of its professions that turn materials into an item, for which it
    /// carries the tools, and - if they need an anvil, a forge or a fire - for which the town has one.
    std::vector<Recipe> Recipes(Player* bot, bool canWalk);

    void LoadYields();
    /// Adds what the bot could take apart: ore to prospect, herbs to mill, gear to disenchant.
    void AddRefining(Player* bot, PlayerbotAI* botAI, std::vector<Recipe>& recipes);
    /// Takes it apart: the source is gone, what comes out is in the bags.
    bool Refine(Player* bot, Recipe const& recipe);
}

#endif
