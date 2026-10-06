/*
 * mod-playerbots-auctions - what the bots say.
 *
 * Every bot is somebody in chat, too: a joker, a grumbler, one who speaks like a knight of old, one who hardly
 * speaks at all - 65 personalities, and a bot keeps the one it has. Now and then one of them says something in
 * the General channel of a zone a player is in, or aloud next to a player: about the innkeeper it stands next
 * to, the wolf that comes too close, or nothing in particular. They announce a new level and congratulate each
 * other, name what killed them and what they killed, show off what they found, feel for a player who dies
 * next to them, wave back, and answer when a player says hello, asks for a joke or wonders about bots.
 *
 * What they say comes from a catalog of several thousand lines, by personality and occasion (data/lines.txt,
 * built in as PbaCatalog.cpp). On a running server the lines live in a table of the characters database, where
 * they can be switched off or added to.
 */

#include "Pba.h"

#include "ChannelMgr.h"

#include <cstdlib>
#include <ctime>

namespace pba
{
namespace
{
    // Order matters: a bot's personality is a position in this list.
    struct Persona
    {
        char const* name;
        float talk;                         // how much it has to say, 1 = like most
        std::vector<uint32> emotes;         // what it does instead of talking
    };

    std::vector<Persona> const Personas =
    {
        { "joker",        1.5f, { TEXT_EMOTE_JOKE, TEXT_EMOTE_LAUGH, TEXT_EMOTE_GIGGLE, TEXT_EMOTE_GUFFAW, TEXT_EMOTE_DANCE, TEXT_EMOTE_FLOP } },
        { "braggart",     1.5f, { TEXT_EMOTE_FLEX, TEXT_EMOTE_GLOAT, TEXT_EMOTE_SMIRK, TEXT_EMOTE_VICTORY } },
        { "grumbler",     1.2f, { TEXT_EMOTE_SIGH, TEXT_EMOTE_GROAN, TEXT_EMOTE_FACEPALM, TEXT_EMOTE_BORED } },
        { "newbie",       1.3f, { TEXT_EMOTE_CONFUSED, TEXT_EMOTE_LOST, TEXT_EMOTE_SURPRISED, TEXT_EMOTE_WAVE, TEXT_EMOTE_CHEER } },
        { "veteran",      0.8f, { TEXT_EMOTE_NOD, TEXT_EMOTE_SIGH, TEXT_EMOTE_SALUTE, TEXT_EMOTE_CHUCKLE } },
        { "roleplayer",   1.2f, { TEXT_EMOTE_BOW, TEXT_EMOTE_SALUTE, TEXT_EMOTE_CURTSEY, TEXT_EMOTE_PRAY } },
        { "quiet",        0.2f, { TEXT_EMOTE_NOD, TEXT_EMOTE_SHY, TEXT_EMOTE_WAVE, TEXT_EMOTE_SHRUG, TEXT_EMOTE_FIDGET } },
        { "collector",    1.0f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_CHEER, TEXT_EMOTE_SCRATCH } },
        { "miser",        0.9f, { TEXT_EMOTE_SIGH, TEXT_EMOTE_BEG, TEXT_EMOTE_GROAN } },
        { "optimist",     1.3f, { TEXT_EMOTE_CHEER, TEXT_EMOTE_DANCE, TEXT_EMOTE_WAVE, TEXT_EMOTE_WHISTLE, TEXT_EMOTE_CLAP } },
        { "doomsayer",    1.0f, { TEXT_EMOTE_SIGH, TEXT_EMOTE_CRY, TEXT_EMOTE_SHRUG, TEXT_EMOTE_GROAN } },
        { "knowitall",    1.3f, { TEXT_EMOTE_TALK, TEXT_EMOTE_NOD, TEXT_EMOTE_SMIRK, TEXT_EMOTE_PONDER } },
        { "dreamer",      0.9f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_WHISTLE, TEXT_EMOTE_SIGH, TEXT_EMOTE_SING } },
        { "hothead",      1.2f, { TEXT_EMOTE_ANGRY, TEXT_EMOTE_ROAR, TEXT_EMOTE_FACEPALM, TEXT_EMOTE_GROAN } },
        { "polite",       1.0f, { TEXT_EMOTE_BOW, TEXT_EMOTE_CURTSEY, TEXT_EMOTE_NOD, TEXT_EMOTE_WAVE, TEXT_EMOTE_APPLAUD } },
        { "sleepy",       0.7f, { TEXT_EMOTE_YAWN, TEXT_EMOTE_TIRED, TEXT_EMOTE_SLEEP, TEXT_EMOTE_BRB } },
        { "glutton",      1.1f, { TEXT_EMOTE_HUNGRY, TEXT_EMOTE_EAT, TEXT_EMOTE_THIRSTY } },
        { "angler",       1.0f, { TEXT_EMOTE_WHISTLE, TEXT_EMOTE_PONDER, TEXT_EMOTE_CHEER } },
        { "lorenerd",     1.2f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_TALK, TEXT_EMOTE_SURPRISED } },
        { "unlucky",      1.0f, { TEXT_EMOTE_SIGH, TEXT_EMOTE_CRY, TEXT_EMOTE_FACEPALM, TEXT_EMOTE_SHRUG } },
        { "pirate",       1.2f, { TEXT_EMOTE_ROAR, TEXT_EMOTE_LAUGH, TEXT_EMOTE_SALUTE, TEXT_EMOTE_SING } },
        { "conspiracist", 1.1f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_FIDGET, TEXT_EMOTE_SCRATCH } },
        { "animalfriend", 1.0f, { TEXT_EMOTE_PURR, TEXT_EMOTE_BARK, TEXT_EMOTE_MOO, TEXT_EMOTE_CUDDLE, TEXT_EMOTE_WHISTLE } },
        { "coward",       0.9f, { TEXT_EMOTE_COWER, TEXT_EMOTE_SCARED, TEXT_EMOTE_FIDGET, TEXT_EMOTE_SHY } },
        { "hero",         1.3f, { TEXT_EMOTE_SALUTE, TEXT_EMOTE_FLEX, TEXT_EMOTE_VICTORY, TEXT_EMOTE_ROAR } },
        { "poet",         0.9f, { TEXT_EMOTE_BOW, TEXT_EMOTE_SING, TEXT_EMOTE_PONDER, TEXT_EMOTE_SIGH } },
        { "gossip",       1.5f, { TEXT_EMOTE_TALK, TEXT_EMOTE_GIGGLE, TEXT_EMOTE_SNICKER, TEXT_EMOTE_WAVE } },
        { "explorer",     1.0f, { TEXT_EMOTE_WHISTLE, TEXT_EMOTE_CHEER, TEXT_EMOTE_PONDER, TEXT_EMOTE_WAVE } },
        { "trader",        1.3f, { TEXT_EMOTE_NOD, TEXT_EMOTE_PONDER, TEXT_EMOTE_BECKON, TEXT_EMOTE_GRIN } },
        { "grandparent",   1.0f, { TEXT_EMOTE_WAVE, TEXT_EMOTE_PAT, TEXT_EMOTE_HUG, TEXT_EMOTE_CHUCKLE } },
        { "sergeant",      1.2f, { TEXT_EMOTE_SALUTE, TEXT_EMOTE_POINT, TEXT_EMOTE_ROAR, TEXT_EMOTE_GLARE } },
        { "philosopher",   0.9f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_NOD, TEXT_EMOTE_STARE } },
        { "hypochondriac", 1.1f, { TEXT_EMOTE_GROAN, TEXT_EMOTE_SIGH, TEXT_EMOTE_FIDGET, TEXT_EMOTE_CRY } },
        { "fashionista",   1.2f, { TEXT_EMOTE_FLEX, TEXT_EMOTE_CURTSEY, TEXT_EMOTE_SMIRK, TEXT_EMOTE_STARE } },
        { "tavernregular", 1.3f, { TEXT_EMOTE_CHEER, TEXT_EMOTE_LAUGH, TEXT_EMOTE_THIRSTY, TEXT_EMOTE_SING, TEXT_EMOTE_GUFFAW } },
        { "farmer",        0.9f, { TEXT_EMOTE_NOD, TEXT_EMOTE_WHISTLE, TEXT_EMOTE_SCRATCH, TEXT_EMOTE_WAVE } },
        { "herbalist",     1.0f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_KNEEL, TEXT_EMOTE_HAPPY } },
        { "prospector",    1.0f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_CHEER, TEXT_EMOTE_POINT, TEXT_EMOTE_SCRATCH } },
        { "craftsman",     1.0f, { TEXT_EMOTE_NOD, TEXT_EMOTE_FLEX, TEXT_EMOTE_PONDER } },
        { "snob",          1.0f, { TEXT_EMOTE_SMIRK, TEXT_EMOTE_SIGH, TEXT_EMOTE_BORED, TEXT_EMOTE_GLARE } },
        { "bard",          1.4f, { TEXT_EMOTE_SING, TEXT_EMOTE_WHISTLE, TEXT_EMOTE_DANCE, TEXT_EMOTE_BOW } },
        { "detective",     1.0f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_STARE, TEXT_EMOTE_POINT, TEXT_EMOTE_SCRATCH } },
        { "daredevil",     1.2f, { TEXT_EMOTE_CHEER, TEXT_EMOTE_ROAR, TEXT_EMOTE_FLEX, TEXT_EMOTE_VICTORY, TEXT_EMOTE_FLOP } },
        { "lazy",          0.7f, { TEXT_EMOTE_YAWN, TEXT_EMOTE_BORED, TEXT_EMOTE_SHRUG, TEXT_EMOTE_FLOP } },
        { "perfectionist", 1.0f, { TEXT_EMOTE_NOD, TEXT_EMOTE_SIGH, TEXT_EMOTE_FIDGET, TEXT_EMOTE_PONDER } },
        { "competitive",   1.3f, { TEXT_EMOTE_FLEX, TEXT_EMOTE_VICTORY, TEXT_EMOTE_TAUNT, TEXT_EMOTE_CHEER } },
        { "mystic",        0.9f, { TEXT_EMOTE_PRAY, TEXT_EMOTE_PONDER, TEXT_EMOTE_KNEEL, TEXT_EMOTE_STARE } },
        { "cynic",         0.9f, { TEXT_EMOTE_SHRUG, TEXT_EMOTE_SMIRK, TEXT_EMOTE_GOLFCLAP, TEXT_EMOTE_SIGH } },
        { "motherhen",     1.3f, { TEXT_EMOTE_HUG, TEXT_EMOTE_PAT, TEXT_EMOTE_COMFORT, TEXT_EMOTE_WAVE } },
        { "socialite",     1.5f, { TEXT_EMOTE_DANCE, TEXT_EMOTE_WAVE, TEXT_EMOTE_CHEER, TEXT_EMOTE_CLAP, TEXT_EMOTE_HAPPY } },
        { "hermit",        0.4f, { TEXT_EMOTE_SHRUG, TEXT_EMOTE_NOD, TEXT_EMOTE_GROWL, TEXT_EMOTE_SIGH } },
        { "homesick",      0.9f, { TEXT_EMOTE_SIGH, TEXT_EMOTE_CRY, TEXT_EMOTE_PONDER, TEXT_EMOTE_MOURN } },
        { "tourist",       1.2f, { TEXT_EMOTE_POINT, TEXT_EMOTE_SURPRISED, TEXT_EMOTE_WAVE, TEXT_EMOTE_CHEER } },
        { "statistician",  1.0f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_NOD, TEXT_EMOTE_SCRATCH } },
        { "scatterbrain",  1.1f, { TEXT_EMOTE_CONFUSED, TEXT_EMOTE_LOST, TEXT_EMOTE_SCRATCH, TEXT_EMOTE_FACEPALM } },
        { "weatherwatcher",1.0f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_POINT, TEXT_EMOTE_STARE, TEXT_EMOTE_SIGH } },
        { "critic",        1.1f, { TEXT_EMOTE_GOLFCLAP, TEXT_EMOTE_PONDER, TEXT_EMOTE_NOD, TEXT_EMOTE_BORED } },
        { "student",       1.2f, { TEXT_EMOTE_NOD, TEXT_EMOTE_CHEER, TEXT_EMOTE_BOW, TEXT_EMOTE_PONDER } },
        { "soldier",       0.9f, { TEXT_EMOTE_SALUTE, TEXT_EMOTE_NOD, TEXT_EMOTE_STARE } },
        { "zen",           0.7f, { TEXT_EMOTE_BOW, TEXT_EMOTE_KNEEL, TEXT_EMOTE_NOD, TEXT_EMOTE_HAPPY } },
        { "dramaqueen",    1.4f, { TEXT_EMOTE_CRY, TEXT_EMOTE_SURPRISED, TEXT_EMOTE_FACEPALM, TEXT_EMOTE_CHEER, TEXT_EMOTE_FLOP } },
        { "hawker",        1.5f, { TEXT_EMOTE_BECKON, TEXT_EMOTE_WAVE, TEXT_EMOTE_POINT, TEXT_EMOTE_GRIN } },
        { "gambler",       1.2f, { TEXT_EMOTE_GRIN, TEXT_EMOTE_WINK, TEXT_EMOTE_CHEER, TEXT_EMOTE_GROAN } },
        { "tinkerer",      1.1f, { TEXT_EMOTE_PONDER, TEXT_EMOTE_CHEER, TEXT_EMOTE_SCRATCH, TEXT_EMOTE_SURPRISED } },
        { "spooky",        0.9f, { TEXT_EMOTE_STARE, TEXT_EMOTE_CACKLE, TEXT_EMOTE_FIDGET, TEXT_EMOTE_GRIN } }
    };

    // Who a bot is, it stays: players get to know them. Written down once, so that a longer list of
    // personalities in a later version changes nobody.
    std::unordered_map<ObjectGuid::LowType, uint16> assigned;
    bool personaTable = false;

    void LoadPersonas()
    {
        assigned.clear();
        personaTable = false;
        if (!cfg.saveMemory)
            return;
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_personas` ("
            "`guid` INT UNSIGNED NOT NULL, `personality` VARCHAR(24) NOT NULL, PRIMARY KEY (`guid`)) "
            "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-playerbots-auctions: who each bot is in chat'");
        CharacterDatabase.DirectExecute(
            "DELETE p FROM `mod_playerbots_auctions_personas` p LEFT JOIN `characters` c ON c.`guid` = p.`guid` WHERE c.`guid` IS NULL");
        personaTable = true;
        if (QueryResult result = CharacterDatabase.Query("SELECT `guid`, `personality` FROM `mod_playerbots_auctions_personas`"))
            do
            {
                Field* fields = result->Fetch();
                std::string const name = fields[1].Get<std::string>();
                for (size_t i = 0; i < Personas.size(); ++i)
                    if (name == Personas[i].name)
                    {
                        assigned[fields[0].Get<uint32>()] = uint16(i);
                        break;
                    }
            } while (result->NextRow());
    }

    Persona const& PersonaOf(Player* bot)
    {
        ObjectGuid::LowType const low = bot->GetGUID().GetCounter();
        auto found = assigned.find(low);
        if (found != assigned.end() && found->second < Personas.size())
            return Personas[found->second];
        size_t const index = std::min<size_t>(Personas.size() - 1, size_t(TraitOf(bot, TRAIT_PERSONA) * float(Personas.size())));
        assigned[low] = uint16(index);
        if (personaTable)
            CharacterDatabase.Execute("REPLACE INTO `mod_playerbots_auctions_personas` (`guid`, `personality`) VALUES ({}, '{}')", low, Personas[index].name);
        return Personas[index];
    }

    /// How much this one talks: its kind, and a little of its own.
    float TalkOf(Player* bot)
    {
        return PersonaOf(bot).talk * (0.6f + 0.8f * TraitOf(bot, TRAIT_TALK));
    }

    // ------------------------------------------------------------------------------------------ the lines

    class Phrases
    {
    public:
        void Load()
        {
            _lines.clear();
            uint32 loaded = 0;
            if (cfg.saveMemory && Sync())
            {
                if (QueryResult result = CharacterDatabase.Query("SELECT `personality`, `occasion`, `text` FROM `mod_playerbots_auctions_lines` WHERE `enabled` <> 0"))
                    do
                    {
                        Field* fields = result->Fetch();
                        std::string const text = fields[2].Get<std::string>();
                        if (text.empty())
                            continue;
                        _lines[fields[0].Get<std::string>() + "|" + fields[1].Get<std::string>()].push_back(text);
                        ++loaded;
                    } while (result->NextRow());
            }
            if (!loaded)
                for (size_t i = 0; i < CatalogSize; ++i)
                {
                    _lines[std::string(Catalog[i].personality) + "|" + Catalog[i].occasion].push_back(Catalog[i].text);
                    ++loaded;
                }
            LOG_INFO("server.loading", ">> PlayerbotsAuctions: the bots have {} line(s) to say, in {} personalities.", loaded, Personas.size());
        }

        /// A line for this bot and occasion, or nothing. Lines of its own personality come first; "any" lines are
        /// for everybody. What was said a moment ago is not said again.
        std::string Pick(Player* bot, std::string const& occasion)
        {
            std::vector<std::string> const* own = Find(std::string(PersonaOf(bot).name) + "|" + occasion);
            std::vector<std::string> const* any = Find("any|" + occasion);
            if (!own && !any)
                return "";
            for (uint32 tries = 0; tries < 8; ++tries)
            {
                std::vector<std::string> const* pool = own && (!any || urand(0, 99) < 70) ? own : any;
                std::string const& line = (*pool)[urand(0, uint32(pool->size()) - 1)];
                size_t const mark = std::hash<std::string>()(line);
                if (std::find(_recent.begin(), _recent.end(), mark) != _recent.end() && tries < 7)
                    continue;
                _recent.push_back(mark);
                if (_recent.size() > 400)
                    _recent.erase(_recent.begin(), _recent.begin() + 100);
                return line;
            }
            return "";
        }

    private:
        std::vector<std::string> const* Find(std::string const& key) const
        {
            auto found = _lines.find(key);
            return found != _lines.end() && !found->second.empty() ? &found->second : nullptr;
        }

        /// Puts the built-in catalog into the table when it is newer than what is there. Lines somebody switched
        /// off stay switched off, lines somebody added (ids from 1000000) stay.
        bool Sync()
        {
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_lines` ("
                "`id` INT UNSIGNED NOT NULL, `personality` VARCHAR(24) NOT NULL, `occasion` VARCHAR(32) NOT NULL, `text` VARCHAR(255) NOT NULL, "
                "`enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1, PRIMARY KEY (`id`), KEY `kind` (`personality`, `occasion`)) "
                "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-playerbots-auctions: what the bots say'");
            if (StateGet("lines_version") == CatalogVersion)
                return true;

            std::unordered_set<std::string> off;
            if (QueryResult result = CharacterDatabase.Query("SELECT `text` FROM `mod_playerbots_auctions_lines` WHERE `enabled` = 0 AND `id` < 1000000"))
                do
                {
                    off.insert(result->Fetch()[0].Get<std::string>());
                } while (result->NextRow());

            CharacterDatabase.DirectExecute("DELETE FROM `mod_playerbots_auctions_lines` WHERE `id` < 1000000");
            std::string sql;
            uint32 batch = 0;
            for (size_t i = 0; i < CatalogSize; ++i)
            {
                std::string personality = Catalog[i].personality, occasion = Catalog[i].occasion, text = Catalog[i].text;
                bool const enabled = off.find(text) == off.end();
                CharacterDatabase.EscapeString(personality);
                CharacterDatabase.EscapeString(occasion);
                CharacterDatabase.EscapeString(text);
                sql += Acore::StringFormat("{}({}, '{}', '{}', '{}', {})", batch ? ", " : "", i + 1, personality, occasion, text, enabled ? 1 : 0);
                if (++batch == 250 || i + 1 == CatalogSize)
                {
                    CharacterDatabase.DirectExecute("INSERT INTO `mod_playerbots_auctions_lines` (`id`, `personality`, `occasion`, `text`, `enabled`) VALUES " + sql);
                    sql.clear();
                    batch = 0;
                }
            }
            StateSet("lines_version", CatalogVersion);
            return true;
        }

        std::unordered_map<std::string, std::vector<std::string>> _lines;
        std::vector<size_t> _recent;
    };

    Phrases phrases;

    std::string Replace(std::string text, char const* what, std::string const& with)
    {
        size_t const size = std::strlen(what);
        for (size_t pos = 0; (pos = text.find(what, pos)) != std::string::npos; pos += with.size())
            text.replace(pos, size, with);
        return text;
    }

    // ------------------------------------------------------------------------------------------ the talking

    enum Where : uint8 { IN_GENERAL, IN_TRADE, IN_WORLD, ALOUD, SHOUTED, WHISPERED };

    struct Line
    {
        time_t at = 0;
        ObjectGuid bot;
        Where where = IN_GENERAL;
        std::string text;
        uint32 emote = 0;               // instead of text
        ObjectGuid to;                  // whom a whisper or a gesture is for
    };

    bool IsHuman(Player* player)
    {
        return player && player->IsInWorld() && player->GetSession() && !GET_PLAYERBOT_AI(player) &&
            !sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId());
    }

    /// A random bot that is free to chat: not led by a player, not in a fight, not on its way through a portal.
    bool CanChat(Player* bot)
    {
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsInCombat() || bot->IsInFlight())
            return false;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        return botAI && !botAI->GetMaster();
    }

    class Chatter
    {
    public:
        void Load()
        {
            phrases.Load();
        }

        void Update(uint32 diff)
        {
            if (!cfg.enabled || !cfg.chatter)
                return;
            _timer += diff;
            if (_timer < 1 * IN_MILLISECONDS)
                return;
            _timer = 0;
            time_t const now = GameTime::GetGameTime().count();

            std::vector<Heard> heard;
            {
                std::lock_guard<std::mutex> guard(_lock);
                heard.swap(_heard);
            }
            for (Heard const& one : heard)
                React(one, now);

            for (size_t i = 0; i < _queue.size();)
            {
                if (_queue[i].at > now)
                {
                    ++i;
                    continue;
                }
                Speak(_queue[i], now);
                _queue.erase(_queue.begin() + i);
            }

            if (!_nextGeneral)
            {
                // Not the moment the world comes up.
                _nextGeneral = now + urand(60, 180);
                _nextAloud = now + urand(90, 240);
            }
            if (_nextGeneral <= now)
            {
                _nextGeneral = now + time_t(float(cfg.chatterInterval) * frand(0.5f, 1.5f));
                Unprompted(now);
            }
            if (cfg.chatterSay && _nextAloud <= now)
            {
                _nextAloud = now + time_t(float(cfg.chatterInterval) * frand(0.8f, 2.2f));
                Nearby(now);
            }
        }

        // ---- what happens to a bot (called from the map threads: only noted here)

        void Event(Player* who, char const* occasion, std::string const& detail, ObjectGuid target)
        {
            if (!cfg.enabled || !cfg.chatter || !who)
                return;
            std::lock_guard<std::mutex> guard(_lock);
            if (_heard.size() < 300)
                _heard.push_back({ who->GetGUID(), occasion, detail, IN_GENERAL, true, target });
        }

        // ---- what a player wrote

        void Listen(Player* player, std::string const& text, Where where, ObjectGuid target)
        {
            if (!cfg.enabled || !cfg.chatter || !cfg.chatterReplies || !player || text.empty() || text.size() > 255)
                return;
            std::lock_guard<std::mutex> guard(_lock);
            if (_heard.size() < 300)
                _heard.push_back({ player->GetGUID(), "", text, where, false, target });
        }

    private:
        struct Heard
        {
            ObjectGuid who;
            std::string occasion;       // an event of a bot
            std::string text;           // a line of a player, or the detail of an event
            Where where;
            bool event;
            ObjectGuid target;          // the bot a whisper or a gesture was for
        };

        // ------------------------------------------------------------------------------------- choosing who

        /// Bots of a player's faction that could say something where the player would read it.
        std::vector<Player*> Around(Player* player, Where where, Player* notThis = nullptr)
        {
            std::vector<Player*> bots;
            PlayerBotMap const all = sRandomPlayerbotMgr.GetAllBots();
            for (auto const& entry : all)
            {
                Player* bot = entry.second;
                if (!CanChat(bot) || bot == notThis || bot->GetTeamId() != player->GetTeamId())
                    continue;
                if (where == ALOUD || where == SHOUTED)
                {
                    if (bot->GetMapId() != player->GetMapId() || !bot->IsWithinDistInMap(player, 30.0f))
                        continue;
                }
                else if (where != IN_WORLD && bot->GetZoneId() != player->GetZoneId())
                    continue;
                bots.push_back(bot);
            }
            return bots;
        }

        /// One of them, the talkative more likely than the quiet; none that spoke a moment ago.
        Player* Choose(std::vector<Player*> const& bots, time_t now, time_t rest)
        {
            std::vector<Player*> free;
            std::vector<float> weights;
            for (Player* bot : bots)
            {
                auto last = _spoke.find(bot->GetGUID().GetCounter());
                if (last != _spoke.end() && last->second + rest > now)
                    continue;
                free.push_back(bot);
                weights.push_back(TalkOf(bot));
            }
            if (free.empty())
                return nullptr;
            float total = 0.0f;
            for (float const weight : weights)
                total += weight;
            float roll = frand(0.0f, total);
            for (size_t i = 0; i < free.size(); ++i)
            {
                roll -= weights[i];
                if (roll <= 0.0f)
                    return free[i];
            }
            return free.back();
        }

        static std::vector<Player*> RealPlayers()
        {
            std::vector<Player*> players;
            for (auto const& entry : ObjectAccessor::GetPlayers())
                if (IsHuman(entry.second))
                    players.push_back(entry.second);
            return players;
        }

        static std::string ZoneOf(Player* bot)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            AreaTableEntry const* zone = botAI ? botAI->GetCurrentZone() : nullptr;
            return zone ? PlayerbotAI::GetLocalizedAreaName(zone) : "";
        }

        /// The line with what it stands for filled in; empty if something it needs is not known.
        static std::string Filled(std::string line, Player* bot, std::string const& name, std::string const& detail)
        {
            if (line.find("{zone}") != std::string::npos)
            {
                std::string const zone = ZoneOf(bot);
                if (zone.empty())
                    return "";
                line = Replace(line, "{zone}", zone);
            }
            if (line.find("{name}") != std::string::npos)
            {
                if (name.empty())
                    return "";
                line = Replace(line, "{name}", name);
            }
            line = Replace(line, "{level}", std::to_string(bot->GetLevel()));
            // What the line is about: the item found, the creature met, the townsperson stood next to.
            for (char const* what : { "{item}", "{creature}", "{npc}" })
                if (line.find(what) != std::string::npos)
                {
                    if (detail.empty())
                        return "";
                    line = Replace(line, what, detail);
                }
            return line;
        }

        bool Say(Player* bot, char const* occasion, Where where, time_t at, std::string const& name = "", std::string const& detail = "",
            ObjectGuid to = ObjectGuid::Empty)
        {
            for (uint32 tries = 0; tries < 4; ++tries)
            {
                std::string const line = Filled(phrases.Pick(bot, occasion), bot, name, detail);
                if (line.empty())
                    continue;
                Line out;
                out.at = at;
                out.bot = bot->GetGUID();
                out.where = where;
                out.text = line;
                out.to = to;
                _queue.push_back(out);
                _spoke[bot->GetGUID().GetCounter()] = at;
                return true;
            }
            return false;
        }

        void Speak(Line const& line, time_t /*now*/)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(line.bot);
            if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
                return;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                return;
            if (line.emote)
            {
                WorldPacket data(SMSG_TEXT_EMOTE);
                data << line.emote;
                data << uint32(0);
                data << line.to;
                bot->GetSession()->HandleTextEmoteOpcode(data);
                return;
            }
            bool said = true;
            switch (line.where)
            {
                case IN_GENERAL: said = botAI->SayToChannel(line.text, ChatChannelId::GENERAL); break;
                case IN_TRADE:   said = botAI->SayToChannel(line.text, ChatChannelId::TRADE); break;
                case IN_WORLD:   said = botAI->SayToWorld(line.text); break;
                case ALOUD:      botAI->Say(line.text); break;
                case SHOUTED:    botAI->Yell(line.text); break;
                case WHISPERED:
                    if (Player* player = ObjectAccessor::FindConnectedPlayer(line.to))
                        bot->Whisper(line.text, LANG_UNIVERSAL, player);
                    break;
            }
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} ({}) {}: {}", bot->GetName(), PersonaOf(bot).name,
                    !said ? "found no channel to say" : line.where == ALOUD ? "says" : line.where == SHOUTED ? "yells" : line.where == WHISPERED ? "whispers" : "writes", line.text);
        }

        // ------------------------------------------------------------------------------------- out of the blue

        void Unprompted(time_t now)
        {
            std::vector<Player*> players = RealPlayers();
            if (players.empty())
                return;
            Player* player = players[urand(0, uint32(players.size()) - 1)];
            // In the zone the player is in; if nobody is there who could talk, in the channel everybody reads.
            Where where = IN_GENERAL;
            Player* bot = Choose(Around(player, IN_GENERAL), now, 10 * MINUTE);
            if (!bot && cfg.chatterWorld)
            {
                where = IN_WORLD;
                bot = Choose(Around(player, IN_WORLD), now, 10 * MINUTE);
            }
            if (!bot)
                return;
            // Late at night and early in the morning there is talk of that, too.
            std::time_t const wall = std::time(nullptr);
            std::tm const* local = std::localtime(&wall);
            int const hour = local ? local->tm_hour : 12;
            char const* occasion = "ambient.general";
            if ((hour >= 22 || hour < 5) && urand(0, 99) < 30)
                occasion = "ambient.night";
            else if (hour >= 5 && hour < 9 && urand(0, 99) < 30)
                occasion = "ambient.morning";
            if (!Say(bot, occasion, where, now))
                Say(bot, "ambient.general", where, now);
        }

        /// The townsperson the bot stands next to: what kind (an occasion "npc...") and the name, or nullptr.
        static char const* Townsperson(Player* bot, PlayerbotAI* botAI, std::string& name)
        {
            char const* kind = nullptr;
            float nearest = 20.0f;
            for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get())
            {
                Creature* npc = botAI->GetCreature(guid);
                if (!npc || !npc->IsAlive() || npc->IsPet() || npc->GetName().empty())
                    continue;
                float const distance = bot->GetDistance(npc);
                if (distance >= nearest)
                    continue;
                char const* what = npc->IsInnkeeper() ? "npc.innkeeper" : npc->IsTaxi() ? "npc.flightmaster" : npc->IsAuctioner() ? "npc.auctioneer" :
                    npc->IsBanker() ? "npc.banker" : npc->IsTrainer() ? "npc.trainer" : (npc->IsVendor() || npc->IsArmorer()) ? "npc.vendor" :
                    npc->IsGuard() ? "npc.guard" : nullptr;
                if (!what)
                    continue;
                kind = what;
                nearest = distance;
                name = npc->GetName();
            }
            return kind;
        }

        /// A creature not far off that could worry somebody: its name, or empty.
        static std::string Menace(Player* bot, PlayerbotAI* botAI)
        {
            std::string name;
            float nearest = 40.0f;
            for (ObjectGuid const guid : botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest hostile npcs")->Get())
            {
                Creature* creature = botAI->GetCreature(guid);
                if (!creature || !creature->IsAlive() || creature->IsCritter() || creature->IsPet() || creature->GetName().empty())
                    continue;
                float const distance = bot->GetDistance(creature);
                if (distance < nearest)
                {
                    nearest = distance;
                    name = creature->GetName();
                }
            }
            return name;
        }

        void Nearby(time_t now)
        {
            std::vector<Player*> players = RealPlayers();
            if (players.empty())
                return;
            Player* player = players[urand(0, uint32(players.size()) - 1)];
            Player* bot = Choose(Around(player, ALOUD), now, 6 * MINUTE);
            if (!bot)
                return;
            Persona const& persona = PersonaOf(bot);
            // Often about what is right there: the innkeeper, the flight master, a wolf that comes too close.
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                if (urand(0, 99) < 35)
                {
                    std::string name;
                    if (char const* kind = Townsperson(bot, botAI, name))
                        if (Say(bot, kind, ALOUD, now, "", name))
                            return;
                }
                if (urand(0, 99) < 25)
                {
                    std::string const name = Menace(bot, botAI);
                    if (!name.empty() && Say(bot, "creature", ALOUD, now, "", name))
                        return;
                }
            }
            uint32 const roll = urand(0, 99);
            if (roll < 30 && !persona.emotes.empty())
            {
                Line out;
                out.at = now;
                out.bot = bot->GetGUID();
                out.emote = persona.emotes[urand(0, uint32(persona.emotes.size()) - 1)];
                _queue.push_back(out);
                _spoke[bot->GetGUID().GetCounter()] = now;
            }
            else if (roll < 36)
                Say(bot, "ambient.yell", SHOUTED, now);
            else
                Say(bot, "ambient.say", ALOUD, now);
        }

        // ------------------------------------------------------------------------------------- reacting

        static bool Has(std::string const& text, std::initializer_list<char const*> words)
        {
            for (char const* word : words)
            {
                size_t const size = std::strlen(word);
                for (size_t pos = text.find(word); pos != std::string::npos; pos = text.find(word, pos + 1))
                {
                    bool const before = pos == 0 || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
                    bool const after = pos + size >= text.size() || !std::isalnum(static_cast<unsigned char>(text[pos + size]));
                    if (before && after)
                        return true;
                }
            }
            return false;
        }

        void React(Heard const& one, time_t now)
        {
            Player* who = ObjectAccessor::FindConnectedPlayer(one.who);
            if (!who || !who->IsInWorld())
                return;

            if (one.event)
            {
                if (one.occasion == "playerdeath" || one.occasion == "playerding")
                    Witnessed(who, one.occasion, now);
                else if (one.occasion == "emote")
                    Gestured(who, one.target, uint32(std::strtoul(one.text.c_str(), nullptr, 10)), now);
                else
                    Happened(who, one.occasion, one.text, now);
                return;
            }
            if (!IsHuman(who))
                return;
            std::string const text = Lower(one.text);
            // Trade is answered by those who want to trade.
            if (Has(text, { "wtb", "wts", "ltb", "lts", "buying", "selling", "lfg", "lfm", "lf1m", "lf2m", "lf3m" }))
                return;

            char const* occasion = nullptr;
            uint32 chance = 0;
            bool joke = false;
            if (Has(text, { "joke", "jokes", "witz" }))
            {
                occasion = "joke";
                chance = 100;
                joke = true;
            }
            else if (Has(text, { "ding", "dinged", "level up", "leveled", "levelled", "lvl up" }))
            {
                occasion = "gratz";
                chance = 90;
            }
            else if (Has(text, { "bot", "bots", "npc", "npcs" }))
            {
                occasion = "bot";
                chance = 70;
            }
            else if (Has(text, { "thanks", "thank you", "thx", "ty", "tyvm", "danke", "cheers" }))
            {
                occasion = "welcome";
                chance = 45;
            }
            else if (Has(text, { "bye", "cya", "gn", "good night", "goodnight", "night all", "logging off", "gtg", "g2g", "see you", "farewell", "nacht" }))
            {
                occasion = "bye";
                chance = 70;
            }
            else if (Has(text, { "hi", "hello", "hey", "hallo", "greetings", "sup", "yo", "heya", "hiya", "howdy", "good morning", "morning", "good evening", "evening", "hi all", "moin" }))
            {
                occasion = "greet";
                chance = 85;
            }
            else if (Has(text, { "help", "how do i", "where is", "where do i", "where can i", "anyone know", "does anyone", "can someone", "stuck", "lost" }))
            {
                occasion = "help";
                chance = 55;
            }
            // Whispered to one of them: it answers, whatever it was.
            if (one.where == WHISPERED)
            {
                Player* bot = ObjectAccessor::FindConnectedPlayer(one.target);
                if (!bot || !bot->IsInWorld() || !sRandomPlayerbotMgr.IsRandomBot(bot))
                    return;
                if (!occasion)
                {
                    occasion = "whisper";
                    chance = 80;
                }
                time_t& asked = _lastReply[who->GetGUID().GetCounter()];
                if (urand(0, 99) >= chance || asked + 4 > now)
                    return;
                asked = now;
                Say(bot, occasion, WHISPERED, now + urand(2, 6), who->GetName(), "", who->GetGUID());
                return;
            }

            if (!occasion || urand(0, 99) >= chance)
                return;
            time_t& last = _lastReply[who->GetGUID().GetCounter()];
            if (last + 15 > now)
                return;
            last = now;

            std::vector<Player*> bots = Around(who, one.where);
            if (bots.empty())
                return;
            // A joke is for the joker, if one is around.
            if (joke)
            {
                std::vector<Player*> jokers;
                for (Player* bot : bots)
                    if (std::strcmp(PersonaOf(bot).name, "joker") == 0)
                        jokers.push_back(bot);
                if (!jokers.empty())
                    bots = jokers;
            }
            uint32 const answers = urand(0, 99) < 55 ? 1 : 2;
            time_t at = now + urand(3, 9);
            Player* first = nullptr;
            for (uint32 i = 0; i < answers; ++i)
            {
                Player* bot = Choose(bots, now, 45);
                if (!bot || bot == first)
                    break;
                first = bot;
                Where const where = one.where == SHOUTED ? ALOUD : one.where;
                Say(bot, occasion, where, at, who->GetName());
                at += urand(3, 10);
            }
        }

        /// Something happened to a bot: a new level, a death, a good find.
        void Happened(Player* bot, std::string occasion, std::string const& detail, time_t now)
        {
            if (!CanChatSoon(bot))
                return;
            float chance = occasion == "ding" ? 0.45f : occasion == "death" ? 0.15f : occasion == "kill" ? 0.5f : 0.35f;
            if (frand(0.0f, 1.0f) > chance * TalkOf(bot))
                return;
            auto last = _spoke.find(bot->GetGUID().GetCounter());
            if (last != _spoke.end() && last->second + 3 * MINUTE > now)
                return;
            // Killed by something with a name: mostly that is what it talks about.
            if (occasion == "death" && !detail.empty() && urand(0, 99) < 65)
                occasion = "death.by";

            // A fight or a death next to a player is talked about aloud, right there.
            if (occasion != "ding" && occasion != "loot")
                for (Player* player : RealPlayers())
                    if (player->GetMapId() == bot->GetMapId() && player->IsWithinDistInMap(bot, 40.0f))
                    {
                        Say(bot, occasion.c_str(), ALOUD, now + urand(2, 5), "", detail);
                        return;
                    }
            // Elsewhere only now and then: a zone hears enough of fights.
            if (occasion == "kill" && urand(0, 99) >= 40)
                return;

            // Only where a player reads it: a player in the zone, or the channel everybody is in.
            Where where = IN_GENERAL;
            Player* reader = nullptr;
            for (Player* player : RealPlayers())
                if (player->GetTeamId() == bot->GetTeamId())
                {
                    if (player->GetZoneId() == bot->GetZoneId())
                    {
                        reader = player;
                        where = IN_GENERAL;
                        break;
                    }
                    if (!reader && cfg.chatterWorld && occasion == "ding")
                    {
                        reader = player;
                        where = IN_WORLD;
                    }
                }
            if (!reader)
                return;
            // Everybody's every level in the channel of the whole world would be too much.
            if (where == IN_WORLD && urand(0, 99) >= 25)
                return;
            if (!Say(bot, occasion.c_str(), where, now + urand(2, 6), "", detail))
                return;

            // The others are pleased for it.
            if (occasion == "ding")
            {
                std::vector<Player*> const others = Around(reader, where, bot);
                uint32 const cheers = urand(0, 99) < 35 ? 0 : urand(0, 99) < 70 ? 1 : 2;
                time_t at = now + urand(7, 14);
                Player* first = nullptr;
                for (uint32 i = 0; i < cheers; ++i)
                {
                    Player* other = Choose(others, now, 60);
                    if (!other || other == first)
                        break;
                    first = other;
                    Say(other, "gratz", where, at, bot->GetName());
                    at += urand(2, 8);
                }
            }
        }

        /// A player died or gained a level, and bots stood by.
        void Witnessed(Player* player, std::string const& occasion, time_t now)
        {
            if (!IsHuman(player))
                return;
            std::vector<Player*> const bots = Around(player, ALOUD);
            bool const died = occasion == "playerdeath";
            if (bots.empty() || urand(0, 99) >= (died ? 70u : 85u))
                return;
            uint32 const voices = died || urand(0, 99) < 60 ? 1 : 2;
            time_t at = now + urand(2, 5);
            Player* first = nullptr;
            for (uint32 i = 0; i < voices; ++i)
            {
                Player* bot = Choose(bots, now, 45);
                if (!bot || bot == first)
                    break;
                first = bot;
                Say(bot, died ? "playerdeath" : "gratz", ALOUD, at, player->GetName());
                at += urand(2, 6);
            }
        }

        /// A player waved at a bot, bowed to it, was rude to it: it answers in kind, as suits it.
        void Gestured(Player* player, ObjectGuid botGuid, uint32 emote, time_t now)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
            if (!IsHuman(player) || !bot || !bot->IsInWorld() || !sRandomPlayerbotMgr.IsRandomBot(bot) || !bot->IsAlive() || bot->IsInCombat())
                return;
            if (bot->GetMapId() != player->GetMapId() || !bot->IsWithinDistInMap(player, 40.0f))
                return;
            time_t& last = _lastGesture[player->GetGUID().GetCounter()];
            if (last + 3 > now)
                return;
            last = now;

            Persona const& persona = PersonaOf(bot);
            std::string const who = persona.name;
            uint32 answer = 0;
            char const* word = nullptr;
            switch (emote)
            {
                case TEXT_EMOTE_WAVE: case TEXT_EMOTE_HELLO: case TEXT_EMOTE_GREET: case TEXT_EMOTE_HAIL:
                    answer = who == "roleplayer" || who == "polite" ? TEXT_EMOTE_BOW : who == "soldier" || who == "sergeant" ? TEXT_EMOTE_SALUTE : TEXT_EMOTE_WAVE;
                    word = "greet";
                    break;
                case TEXT_EMOTE_BYE:
                    answer = TEXT_EMOTE_WAVE;
                    word = "bye";
                    break;
                case TEXT_EMOTE_BOW: case TEXT_EMOTE_CURTSEY: case TEXT_EMOTE_KNEEL:
                    answer = TEXT_EMOTE_BOW;
                    break;
                case TEXT_EMOTE_SALUTE:
                    answer = TEXT_EMOTE_SALUTE;
                    break;
                case TEXT_EMOTE_DANCE:
                    answer = who == "grumbler" || who == "hermit" || who == "cynic" ? TEXT_EMOTE_SIGH : TEXT_EMOTE_DANCE;
                    break;
                case TEXT_EMOTE_LAUGH: case TEXT_EMOTE_ROFL: case TEXT_EMOTE_GIGGLE: case TEXT_EMOTE_CHUCKLE: case TEXT_EMOTE_GUFFAW:
                    answer = who == "quiet" || who == "coward" ? TEXT_EMOTE_SHY : TEXT_EMOTE_LAUGH;
                    break;
                case TEXT_EMOTE_CHEER: case TEXT_EMOTE_CLAP: case TEXT_EMOTE_APPLAUD: case TEXT_EMOTE_CONGRATULATE:
                    answer = who == "braggart" || who == "hero" ? TEXT_EMOTE_FLEX : TEXT_EMOTE_BOW;
                    break;
                case TEXT_EMOTE_THANK:
                    answer = TEXT_EMOTE_NOD;
                    word = "welcome";
                    break;
                case TEXT_EMOTE_HUG: case TEXT_EMOTE_CUDDLE: case TEXT_EMOTE_COMFORT: case TEXT_EMOTE_PAT:
                    answer = who == "hermit" || who == "grumbler" || who == "snob" ? TEXT_EMOTE_SIGH : TEXT_EMOTE_HUG;
                    break;
                case TEXT_EMOTE_KISS: case TEXT_EMOTE_FLIRT: case TEXT_EMOTE_LOVE: case TEXT_EMOTE_WINK:
                    answer = who == "braggart" ? TEXT_EMOTE_FLEX : who == "cynic" || who == "snob" ? TEXT_EMOTE_SIGH : TEXT_EMOTE_BLUSH;
                    break;
                case TEXT_EMOTE_RUDE: case TEXT_EMOTE_SPIT: case TEXT_EMOTE_SLAP: case TEXT_EMOTE_INSULT: case TEXT_EMOTE_TAUNT:
                case TEXT_EMOTE_BONK: case TEXT_EMOTE_GLARE:
                    answer = who == "hothead" || who == "sergeant" ? TEXT_EMOTE_ANGRY : who == "coward" || who == "quiet" ? TEXT_EMOTE_COWER :
                        who == "zen" || who == "polite" ? TEXT_EMOTE_BOW : who == "dramaqueen" ? TEXT_EMOTE_CRY : urand(0, 1) ? TEXT_EMOTE_SHRUG : TEXT_EMOTE_SIGH;
                    break;
                case TEXT_EMOTE_POKE: case TEXT_EMOTE_TICKLE: case TEXT_EMOTE_TEASE:
                    answer = who == "grumbler" || who == "hermit" ? TEXT_EMOTE_GLARE : TEXT_EMOTE_GIGGLE;
                    break;
                case TEXT_EMOTE_CRY: case TEXT_EMOTE_MOURN:
                    answer = who == "cynic" ? TEXT_EMOTE_SHRUG : TEXT_EMOTE_COMFORT;
                    break;
                case TEXT_EMOTE_JOKE:
                    answer = who == "grumbler" || who == "cynic" || who == "snob" ? TEXT_EMOTE_GROAN : TEXT_EMOTE_LAUGH;
                    break;
                case TEXT_EMOTE_CHICKEN:
                    answer = who == "coward" ? TEXT_EMOTE_COWER : who == "hothead" ? TEXT_EMOTE_ANGRY : TEXT_EMOTE_LAUGH;
                    break;
                case TEXT_EMOTE_POINT: case TEXT_EMOTE_STARE: case TEXT_EMOTE_BECKON:
                    answer = who == "coward" || who == "quiet" ? TEXT_EMOTE_SHY : TEXT_EMOTE_CONFUSED;
                    break;
                default:
                    // Whatever it was: now and then the bot does what it always does.
                    if (urand(0, 99) < 35 && !persona.emotes.empty())
                        answer = persona.emotes[urand(0, uint32(persona.emotes.size()) - 1)];
                    break;
            }
            if (!answer || urand(0, 99) >= 85)
                return;
            Line out;
            out.at = now + urand(1, 3);
            out.bot = bot->GetGUID();
            out.emote = answer;
            out.to = player->GetGUID();
            _queue.push_back(out);
            // ... and sometimes a word with it.
            if (word && urand(0, 99) < 40)
                Say(bot, word, ALOUD, out.at + 1, player->GetName());
        }

        static bool CanChatSoon(Player* bot)
        {
            if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
                return false;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            return botAI && !botAI->GetMaster() && sRandomPlayerbotMgr.IsRandomBot(bot);
        }

        std::mutex _lock;
        std::vector<Heard> _heard;
        std::vector<Line> _queue;
        std::unordered_map<ObjectGuid::LowType, time_t> _spoke;
        std::unordered_map<ObjectGuid::LowType, time_t> _lastReply;
        std::unordered_map<ObjectGuid::LowType, time_t> _lastGesture;
        time_t _nextGeneral = 0, _nextAloud = 0;
        uint32 _timer = 0;
    };

    Chatter chatter;
}

    void LoadChatter()
    {
        LoadPersonas();
        chatter.Load();
    }

    void ChatterUpdate(uint32 diff)
    {
        chatter.Update(diff);
    }

    void ChatterEvent(Player* who, char const* occasion, std::string const& detail, ObjectGuid target)
    {
        chatter.Event(who, occasion, detail, target);
    }

    void ChatterHeard(Player* player, std::string const& text, uint8 where, ObjectGuid target)
    {
        chatter.Listen(player, text, Where(where), target);
    }

    std::string PhraseFor(Player* bot, std::string const& occasion)
    {
        return phrases.Pick(bot, occasion);
    }

    char const* PersonalityOf(Player* bot)
    {
        return PersonaOf(bot).name;
    }
}
