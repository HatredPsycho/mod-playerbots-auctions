/*
 * mod-playerbots-auctions - where things are: auctioneers, capitals, anvils, forges and fires; and what
 * a bot can craft there.
 * Released under GNU AGPL v3: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "Pba.h"

namespace pba
{
    namespace
    {
        struct City
        {
            TeamId team;
            uint32 mapId;
            float x, y, z;              // the auctioneers
            float ax, ay, az;           // where a bot arrives by hearth: bank or inn, a walk away
        };

        // The eight old capitals.
        City const Cities[] =
        {
            { TEAM_ALLIANCE, 0,   -8820.1f,    662.0f,   97.2f,  -8935.3f,    613.2f,   99.6f },    // Stormwind
            { TEAM_ALLIANCE, 0,   -4959.7f,   -907.9f,  505.2f,  -4840.7f,   -857.1f,  502.0f },    // Ironforge
            { TEAM_ALLIANCE, 1,    9864.5f,   2341.7f, 1326.8f,   9942.0f,   2519.7f, 1317.7f },    // Darnassus
            { TEAM_ALLIANCE, 530, -4025.5f, -11736.0f, -151.8f,  -3919.0f, -11544.7f, -150.1f },    // Exodar
            { TEAM_HORDE,    1,    1683.6f,  -4461.3f,   20.4f,   1627.5f,  -4375.7f,   12.1f },    // Orgrimmar
            { TEAM_HORDE,    1,   -1204.7f,    102.8f,  134.7f,  -1300.3f,     38.5f,  129.3f },    // Thunder Bluff
            { TEAM_HORDE,    0,    1612.3f,    199.8f,  -56.8f,   1635.4f,    223.3f,  -43.0f },    // Undercity
            { TEAM_HORDE,    530,  9648.4f,  -7135.7f,   16.9f,   9565.7f,  -7222.8f,   16.4f },    // Silvermoon
        };

        constexpr uint32 CityCount = sizeof(Cities) / sizeof(Cities[0]);
        constexpr float CityRadius = 1500.0f;       // closer than this to its auction house a bot is "in town"
        constexpr float FocusRadius = 700.0f;       // an anvil counts as the town's if it is this close to the auction house

        std::vector<Focus> cityFocus[CityCount];

        /// A trip to a capital on another map, under way.
        struct Pending
        {
            City const* city = nullptr;
            uint32 mapId = 0;           // where the bot set out from
            float x = 0.0f, y = 0.0f, z = 0.0f;
            time_t until = 0;
        };
        std::unordered_map<ObjectGuid::LowType, Pending> pending;

        City const* TownOf(Player* bot)
        {
            for (City const& city : Cities)
                if (city.team == bot->GetTeamId() && city.mapId == bot->GetMapId() && bot->GetExactDist2d(city.x, city.y) < CityRadius)
                    return &city;
            return nullptr;
        }

        // The CoA Playerbots fork lets bots visit a city and return afterwards. Other versions of
        // mod-playerbots do not have that; there the bots are not sent anywhere and only do their
        // business when they happen to stand at an auctioneer.
        template <typename AI>
        concept HasCityLife = requires(AI& ai, WorldPosition pos) { ai.rpgInfo.ChangeToGoCity(pos); ai.rpgInfo.cityReturnPos = pos; ai.rpgInfo.cityStayMs = 0u; };

        template <typename AI>
        Journey Send(Player* bot, AI* botAI)
        {
            if constexpr (HasCityLife<AI>)
            {
                if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
                    return JOURNEY_NONE;
                auto& info = botAI->rpgInfo;

                // Any capital of the bot's faction will do.
                std::vector<City const*> reachable;
                for (City const& city : Cities)
                    if (city.team == bot->GetTeamId())
                        reachable.push_back(&city);
                if (reachable.empty())
                    return JOURNEY_NONE;

                City const* here = TownOf(bot);
                City const* city = here ? here : reachable[urand(0, uint32(reachable.size()) - 1)];
                float const angle = frand(0.0f, 6.2831853f);
                float const radius = frand(2.0f, 6.0f);
                WorldPosition const auctioneers(city->mapId, city->x + radius * std::cos(angle), city->y + radius * std::sin(angle), city->z);

                if (!here)
                {
                    if (info.cityStayMs || info.cityReturnPos != WorldPosition())
                        return JOURNEY_NONE;        // mod-playerbots is already taking it to a city

                    if (city->mapId != bot->GetMapId())
                    {
                        // To another continent. mod-playerbots clears the bot's head when it changes maps, so
                        // the errand and the way back are kept here and given to it again on arrival.
                        Pending trip;
                        trip.city = city;
                        trip.mapId = bot->GetMapId();
                        trip.x = bot->GetPositionX();
                        trip.y = bot->GetPositionY();
                        trip.z = bot->GetPositionZ();
                        trip.until = GameTime::GetGameTime().count() + 3 * MINUTE;
                        if (!bot->TeleportTo(city->mapId, city->ax + frand(-3.0f, 3.0f), city->ay + frand(-3.0f, 3.0f), city->az, bot->GetOrientation()))
                            return JOURNEY_NONE;
                        pending[bot->GetGUID().GetCounter()] = trip;
                        return JOURNEY_HEARTH;
                    }

                    info.cityReturnPos = WorldPosition(bot);
                    if (!bot->TeleportTo(city->mapId, city->ax + frand(-3.0f, 3.0f), city->ay + frand(-3.0f, 3.0f), city->az, bot->GetOrientation()))
                    {
                        info.cityReturnPos = WorldPosition();
                        return JOURNEY_NONE;
                    }
                }
                info.ChangeToGoCity(auctioneers);
                return here ? JOURNEY_WALK : JOURNEY_HEARTH;
            }
            else
            {
                return JOURNEY_NONE;
            }
        }

        template <typename AI>
        void Resume(Player* bot, AI* botAI)
        {
            if constexpr (HasCityLife<AI>)
            {
                auto found = pending.find(bot->GetGUID().GetCounter());
                if (found == pending.end())
                    return;
                Pending const trip = found->second;
                if (trip.until < GameTime::GetGameTime().count())
                {
                    pending.erase(found);       // never arrived
                    return;
                }
                if (bot->GetMapId() != trip.city->mapId || bot->IsBeingTeleported() || !bot->IsInWorld())
                    return;                     // still on its way
                pending.erase(found);

                float const angle = frand(0.0f, 6.2831853f);
                float const radius = frand(2.0f, 6.0f);
                auto& info = botAI->rpgInfo;
                info.cityReturnPos = WorldPosition(trip.mapId, trip.x, trip.y, trip.z);
                info.ChangeToGoCity(WorldPosition(trip.city->mapId, trip.city->x + radius * std::cos(angle),
                    trip.city->y + radius * std::sin(angle), trip.city->z));
            }
        }

        template <typename AI>
        bool Walk(Player* bot, AI* botAI, float x, float y, float z)
        {
            if constexpr (HasCityLife<AI>)
            {
                if (!botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
                    return false;
                botAI->rpgInfo.ChangeToGoCity(WorldPosition(bot->GetMapId(), x, y, z));
                return true;
            }
            else
            {
                return false;
            }
        }
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

    Creature* FindVendor(Player* bot, PlayerbotAI* botAI)
    {
        GuidVector const npcs = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const guid : npcs)
        {
            Creature* creature = ObjectAccessor::GetCreature(*bot, guid);
            if (creature && creature->IsAlive() && creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) &&
                bot->GetDistance(creature) <= cfg.auctioneerRange && !creature->IsHostileTo(bot))
                return creature;
        }
        return nullptr;
    }

    bool FindHouse(Player* bot, PlayerbotAI* botAI, AuctionHouseId& houseId)
    {
        Creature* auctioneer = FindAuctioneer(bot, botAI);
        if (!auctioneer)
            return false;

        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
        {
            houseId = AuctionHouseId::Neutral;
            return true;
        }

        if (AuctionHouseEntry const* entry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction()))
        {
            houseId = AuctionHouseId(entry->houseId);
            return true;
        }

        houseId = bot->GetTeamId() == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde;
        return true;
    }

    bool IsInTown(Player* bot)
    {
        return TownOf(bot) != nullptr;
    }

    Journey SendToAuctionHouse(Player* bot, PlayerbotAI* botAI)
    {
        return Send(bot, botAI);
    }

    void ResumeJourney(Player* bot, PlayerbotAI* botAI)
    {
        if (!pending.empty())
            Resume(bot, botAI);
    }

    bool WalkInTown(Player* bot, PlayerbotAI* botAI, float x, float y, float z)
    {
        return Walk(bot, botAI, x, y, z);
    }

    bool CanTravel(PlayerbotAI* botAI)
    {
        return botAI && botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT);
    }

    void StayPut(Player* bot, PlayerbotAI* botAI, float x, float y, float z)
    {
        // mod-playerbots does not start a new move while a forced one is under way.
        LastMovement& last = botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
        last.Set(bot->GetMapId(), x, y, z, bot->GetOrientation(), 4000.0f, MovementPriority::MOVEMENT_FORCED);
    }

    void LoadFocusObjects()
    {
        for (std::vector<Focus>& list : cityFocus)
            list.clear();

        uint32 count = 0;
        // Spell focus objects: anvils, forges, cooking fires, moonwells and the like.
        if (QueryResult result = WorldDatabase.Query(
            "SELECT g.map, g.position_x, g.position_y, g.position_z, t.Data0, t.Data1 FROM gameobject g "
            "JOIN gameobject_template t ON t.entry = g.id "
            "LEFT JOIN game_event_gameobject e ON e.guid = g.guid LEFT JOIN pool_gameobject p ON p.guid = g.guid "
            "WHERE t.type = 8 AND t.Data0 > 0 AND g.map IN (0, 1, 530) AND e.guid IS NULL AND p.guid IS NULL"))     // only what is always there
            do
            {
                Field* fields = result->Fetch();
                Focus focus;
                focus.mapId = fields[0].Get<uint16>();
                focus.x = fields[1].Get<float>();
                focus.y = fields[2].Get<float>();
                focus.z = fields[3].Get<float>();
                focus.id = fields[4].Get<uint32>();
                int32 const range = fields[5].Get<int32>();
                focus.range = range > 0 ? float(range) : 10.0f;

                for (uint32 i = 0; i < CityCount; ++i)
                {
                    float const dx = Cities[i].x - focus.x, dy = Cities[i].y - focus.y;
                    if (Cities[i].mapId == focus.mapId && dx * dx + dy * dy < FocusRadius * FocusRadius)
                    {
                        cityFocus[i].push_back(focus);
                        ++count;
                    }
                }
            } while (result->NextRow());

        LOG_INFO("server.loading", ">> PlayerbotsAuctions: {} anvils, forges, fires and other work places in the capitals.", count);
    }

    Focus const* NearestFocus(Player* bot, uint32 focusId)
    {
        City const* town = TownOf(bot);
        if (!town)
            return nullptr;

        Focus const* best = nullptr;
        float bestDistance = 0.0f;
        for (Focus const& focus : cityFocus[town - Cities])
        {
            if (focus.id != focusId)
                continue;
            float const distance = bot->GetExactDist(focus.x, focus.y, focus.z);
            if (!best || distance < bestDistance)
            {
                best = &focus;
                bestDistance = distance;
            }
        }
        return best;
    }

    std::vector<Recipe> Recipes(Player* bot, bool canWalk)
    {
        std::vector<Recipe> recipes;
        for (auto const& known : bot->GetSpellMap())
        {
            if (!known.second || known.second->State == PLAYERSPELL_REMOVED || !known.second->Active)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(known.first);
            if (!info || !info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL) || info->Effects[EFFECT_0].Effect != SPELL_EFFECT_CREATE_ITEM ||
                !info->Effects[EFFECT_0].ItemType || bot->HasSpellCooldown(info->Id))
                continue;
            if (!cfg.glyphs)
                if (ItemTemplate const* made = sObjectMgr->GetItemTemplate(info->Effects[EFFECT_0].ItemType))
                    if (made->Class == ITEM_CLASS_GLYPH)
                        continue;
            if (info->RequiresSpellFocus && (!cfg.craftFocus || !canWalk || !NearestFocus(bot, info->RequiresSpellFocus)))
                continue;

            // The tools: what it does not carry it buys, if a vendor has it - a hammer, a pick.
            bool tools = true;
            std::vector<uint32> toBuy;
            for (uint32 i = 0; i < 2; ++i)
            {
                if (info->Totem[i] && !bot->HasItemCount(info->Totem[i], 1))
                {
                    if (market.IsVendorSupply(info->Totem[i]))
                        toBuy.push_back(info->Totem[i]);
                    else
                        tools = false;
                }
                if (info->TotemCategory[i] && !bot->HasItemTotemCategory(info->TotemCategory[i]))
                {
                    if (uint32 const tool = market.VendorTool(info->TotemCategory[i]))
                        toBuy.push_back(tool);
                    else
                        tools = false;
                }
            }
            if (!tools || (!toBuy.empty() && !cfg.matsVendor))
                continue;

            Recipe recipe;
            recipe.tools = std::move(toBuy);
            recipe.spell = info->Id;
            recipe.product = info->Effects[EFFECT_0].ItemType;
            recipe.made = uint32(std::max<int32>(1, info->Effects[EFFECT_0].BasePoints + 1));
            recipe.focus = info->RequiresSpellFocus;
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (info->Reagent[i] > 0 && info->ReagentCount[i])
                    recipe.reagents.emplace_back(uint32(info->Reagent[i]), info->ReagentCount[i]);
            if (!recipe.reagents.empty())
                recipes.push_back(std::move(recipe));
        }
        return recipes;
    }
}
