# mod-playerbots-auctions

An add-on module for [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) on AzerothCore:
the random bots put their loot up for auction, in their own name.

**Status: first test version.** It compiles against the Conquest of Azeroth core
(`jealous-sound/azerothcore-wotlk-coa`) with the CoA Playerbots fork (`Zyth45/mod-playerbots`, branch `coa`).
It has not been run on a server yet.

## What it does

The bots already decide for every item whether they need it. What they do not need they sell to a vendor.
This module steps in before that: a bot that is in a capital city or near an auctioneer puts those items up
for auction, like a player would. So the auction house fills with what the bots really found or made, offered
by the bots you meet in the world.

- Only items the bot does not need (its own judgement), that are not soulbound and that a vendor would pay for
- Weapons and armor from uncommon (green) quality, everything else from normal (white) quality - configurable
- Prices from the vendor value and the quality; bots undercut the cheapest existing offer a little
- A bot also empties its auction mail: the money of sold items, and unsold items, which it offers again later
- Bots that travel with a real player do not sell
- Nothing in mod-playerbots is changed and nothing is added to the databases

Bots do not buy from the auction house (yet), and they do not walk to the auctioneer on purpose: the module
uses the moments a bot is in a city anyway.

## Requirements

- AzerothCore with **mod-playerbots** installed (this module does not build without it)

## Installation

1. Put this folder into `modules` next to `mod-playerbots`, then run CMake and build the server.
2. Copy `mod_playerbots_auctions.conf.dist` to `mod_playerbots_auctions.conf` and set
   `PlayerbotsAuctions.Enable = 1`.

For a first test set `PlayerbotsAuctions.Place = 0` and `PlayerbotsAuctions.Debug = 1`: bots then sell wherever
they are, and every auction is written to the server log.

## License

GNU AGPL v3, like AzerothCore.
