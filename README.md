# mod-playerbots-auctions

An add-on module for [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) on AzerothCore:
the random bots use the auction house in their own name. They sell their loot, buy and bid on what they can
use, and travel to a city when their bags fill up.

It adds to mod-playerbots and does not change or replace it. Nothing is added to the databases.

**Status: test version.** Built and run on the Conquest of Azeroth core (`jealous-sound/azerothcore-wotlk-coa`)
with the CoA Playerbots fork (`Zyth45/mod-playerbots`, branch `coa`). Selling has been seen working on a
server; buying and the trips to the city are new and have only been compiled.

## What the bots do

**Selling.** The bots already decide for every item whether they need it. What they do not need they would
sell to a vendor. A bot that is in a capital city or near an auctioneer puts those items up for auction instead.

- Only what the bot does not need, what is not soulbound and what a vendor would pay for
- Weapons and armor from uncommon (green) quality, everything else from normal (white) quality - configurable
- Prices start from the vendor value and the quality. Most auctions are near the usual price, some are
  bargains, some are clearly overpriced; bots undercut the cheapest offer a little, down to a limit

**Buying.** At the auction house a bot also looks through some of the offers of players and other bots.

- It pays most for an upgrade for itself, less for things it uses up, and now and then buys something to sell on
- It buys outright when the price fits, otherwise it may place a bid - also against a player
- It always pays more than a vendor would, and now and then far more than the usual price
- It pays with its own money. For items a vendor sells it never pays the vendor's price, so buying from a
  vendor and selling to the bots does not pay off

**Learning prices.** What auctions really sell for moves what the bots consider the usual price - within
limits, and only sales a bot took part in count.

**Trips to the city.** A bot whose bags fill up far from an auction house takes its hearth to a capital of its
faction on the same continent, sells there, stays a while and returns. This uses the city visits of the CoA
Playerbots fork; with other versions of mod-playerbots the bots are simply not sent to town.

**Mail.** A bot empties its auction mail when it is at the auction house: money for sold items, items nobody
bought (offered again later) and what it bought.

Bots that travel with a real player neither sell nor buy.

## Requirements

- AzerothCore with **mod-playerbots** installed (this module does not build without it)

## Installation

1. Put this folder into `modules` next to `mod-playerbots`, then run CMake and build the server.
2. Copy `mod_playerbots_auctions.conf.dist` to `mod_playerbots_auctions.conf` and set
   `PlayerbotsAuctions.Enable = 1`.

For a first test set `PlayerbotsAuctions.Place = 0` and `PlayerbotsAuctions.Debug = 1`: bots then sell
wherever they are, and every auction, bid and purchase is written to the server log.

## License

GNU AGPL v3, like AzerothCore.
