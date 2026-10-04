# mod-playerbots-auctions

An add-on module for [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) on AzerothCore:
the random bots take part in the economy like players. They go to the auction house when they feel it is
time, sell what they do not need, buy what they can use, and work with their professions.

It adds to mod-playerbots and does not change or replace it.

**Status: test version.** Built against the Conquest of Azeroth core (`jealous-sound/azerothcore-wotlk-coa`)
with the CoA Playerbots fork (`Zyth45/mod-playerbots`, branch `coa`). An early version has been seen selling
on a server; the behaviour described here is new and has only been compiled.

## Every bot is somebody

Each bot has a character of its own that never changes: how orderly it is, how patient, how well it knows the
market, how thrifty, how keen on trading, how serious about its profession. Everything else follows from that.

- **Going there.** A bot decides for itself when it is time: when its bags are getting full, or when it has
  enough to sell - and every bot draws those lines somewhere else. A bot in town walks to the auctioneers. A
  bot out in the world takes its hearth to a capital of its faction, arrives at the bank or inn, walks to the auctioneers,
  stays a while and returns. Some bots never use the auction house at all. In the evening and at the weekend
  more is going on than at night.
- **Selling.** Only what the bot does not need. One that knows the market goes just below the cheapest offer;
  one that does not guesses - too low or far too high. Stacks go up in packs. The impatient list for half a day
  and take what they get, the patient for two days. The deposit is real, so what brings little more than a
  vendor pays goes to the vendor. What comes back unsold goes up again for less, then to the vendor.
- **Buying.** Upgrades for its equipment, things it uses up, a bag for a free slot, materials for its
  professions - the cheapest offer of each. What it pays depends on the usual price, how much it wants the
  thing and how well off it is. The patient bid, the impatient buy outright. Traders buy what is clearly too
  cheap and sell it on. Bots pay with their own money, and the thrifty keep a reserve.
- **Crafting.** mod-playerbots gives the bots professions and recipes, but nothing makes them craft. Here a
  bot compares what a product would bring with what its materials cost - carried, on offer, from a vendor -
  and makes what pays. For some the profession is a sideline, some provide for themselves and work on their
  skill, some produce in batches for the market. For an anvil, a forge or a fire the bot walks there.
  A miner smelts its ore; a smith then decides at once whether to work the bars or to sell them.
  Jewelcrafters prospect, scribes mill and enchanters disenchant when that pays or feeds their own recipes.
  Enchanters put their enchantments on vellum - made by scribes - and sell the scrolls.
- **Keeping.** What is meant for the auction house and the materials a bot has plans for are not sold to a
  vendor on the way. Things a vendor gives nothing for - dusts, essences - get a price from their item level.
- **Memory.** The bots learn what things really sell for and remember what did not sell. That survives a
  restart in two small tables of the characters database, which the module creates itself.

All business is done standing at an auctioneer. Bots that travel with a real player neither sell nor buy.

The travelling uses the city visits of the CoA Playerbots fork. With other versions of mod-playerbots the bots
are not sent anywhere and only do their business when they happen to pass an auctioneer.

## Requirements

- AzerothCore with **mod-playerbots** installed (this module does not build without it)
- Three small changes to mod-playerbots, in `patches/mod-playerbots`:
  - **keep-bags-on-refresh.patch** - mod-playerbots empties the bags of every random bot every 10 to 40
    minutes and after every death. With that the bots never have anything to sell. The patch keeps the bags
    and only renews food, drink and potions.
  - **scaled-item-lookup.patch** - since CoA core #6403 a bot that loots a level-scaled item ends the
    server. Not needed any more once mod-playerbots has the fix.
  - **gather-on-slopes.patch** - mod-playerbots ignores every vein and herb more than 3.5 yards above or
    below the bot. Veins are in hillsides, so miners never mined. With the patch the height may grow with
    the distance (half a yard per yard, 25 at most).

## Installation

**With [AFK Realm](https://github.com/aspollon/AFK-Realm):** tick the module under "Modules by AFK Realm" in
the module manager. AFK Realm downloads it, applies the patches with every build (and leaves one out when
it no longer fits or is no longer needed), sets `AiPlayerbot.LootDistance` to 60 so the bots gather more ore
and herbs, and rebuilds the server. `afk-realm.json` tells it what to do.

**By hand:**

1. Put this folder into `modules` next to `mod-playerbots`.
2. In `modules/mod-playerbots` run `git apply ../mod-playerbots-auctions/patches/mod-playerbots/*.patch`
   (again after every update of mod-playerbots).
3. Run CMake and build the server.
4. Copy `mod_playerbots_auctions.conf.dist` to `mod_playerbots_auctions.conf`. The module is switched on by
   default. Consider a higher `AiPlayerbot.LootDistance` in `playerbots.conf`.

With `PlayerbotsAuctions.Debug = 1` every trip, auction, bid, purchase and crafted item is written to the
server log, and every five minutes a summary of what the bots carry and what comes of their professions.

## Removing it

Take the folder out and rebuild. The two tables `mod_playerbots_auctions_market` and
`mod_playerbots_auctions_unsold` in the characters database can be dropped; nothing else was changed.

## License

GNU AGPL v3, like AzerothCore.
