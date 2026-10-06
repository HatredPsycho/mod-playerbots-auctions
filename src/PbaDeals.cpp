/*
 * mod-playerbots-auctions - deals by chat.
 *
 * A player writes "WTB copper ore" or "WTS 20 linen cloth" into a channel. Bots that have the item to spare, or
 * a use for it, whisper an offer after a moment, each at a price of its own. The player can agree, decline or
 * name another price; a bot gives way a little, depending on its character, and no further. The goods travel
 * by mail, cash on delivery: a bot that sells sends them itself, a bot that buys pays for the parcel the
 * player sends it - if it holds what was agreed, at the price that was agreed.
 */

#include "Pba.h"

#include <cstdlib>

namespace pba
{
namespace
{
    char const* const BuyWords[] = { "wtb", "ltb", "buying", "want to buy", "looking to buy", "looking for", "lf" };
    char const* const SellWords[] = { "wts", "lts", "selling", "want to sell", "looking to sell" };
    char const* const YesWords[] = { "yes", "y", "ya", "yep", "yeah", "yea", "ok", "okay", "k", "kk", "deal", "sure", "fine", "send", "send it",
        "sounds good", "do it", "go", "alright", "ja", "jo", "passt", "gerne" };
    char const* const NoWords[] = { "no", "n", "nah", "nope", "nvm", "nevermind", "never mind", "no thanks", "no thx", "too much", "too expensive",
        "too low", "forget it", "cancel", "nein", "ne" };
    // Words around the name of an item that are not part of it.
    char const* const Filler[] = { "of", "my", "some", "a", "an", "the", "pls", "plz", "please", "anyone", "any", "pst", "cheap", "x", "pm", "me", "whisper", "w",
        "all", "your", "ur", "more", "few", "couple", "bunch", "lot", "lots", "stack", "stacks", "about", "around" };

    bool IsWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80; }

    /// A name as it is compared: lower case, letters and digits only - "Copper Ore", "copperore" and "[Copper Ore]" are the same.
    std::string Squeeze(std::string const& text)
    {
        std::string out;
        out.reserve(text.size());
        for (char const c : text)
            if (IsWordChar(c))
                out += static_cast<unsigned char>(c) < 0x80 ? char(std::tolower(static_cast<unsigned char>(c))) : c;
        return out;
    }

    /// Takes the colour and link codes of the client out of a message; an item that was linked is returned by its id.
    std::string Plain(std::string const& msg, uint32& linked)
    {
        std::string out;
        for (size_t i = 0; i < msg.size(); ++i)
        {
            if (msg[i] != '|' || i + 1 >= msg.size())
            {
                out += msg[i];
                continue;
            }
            char const code = msg[i + 1];
            if (code == 'c' && i + 9 < msg.size())
                i += 9;                                         // |cAARRGGBB
            else if (code == 'H')
            {
                if (!linked && msg.compare(i, 7, "|Hitem:") == 0)
                    linked = uint32(std::strtoul(msg.c_str() + i + 7, nullptr, 10));
                size_t const end = msg.find("|h", i + 2);
                if (end == std::string::npos)
                    break;
                i = end + 1;
            }
            else if (code == 'h' || code == 'r')
                ++i;
            else
                out += msg[i];
        }
        return out;
    }

    std::vector<std::string> Words(std::string const& text)
    {
        std::vector<std::string> words;
        std::string word;
        for (char const c : text)
        {
            if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == ';' || c == '[' || c == ']' || c == '(' || c == ')' || c == '!' || c == '?')
            {
                if (!word.empty())
                    words.push_back(word);
                word.clear();
            }
            else
                word += c;
        }
        if (!word.empty())
            words.push_back(word);
        return words;
    }

    bool IsNumber(std::string const& word)
    {
        return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    }

    uint32 UnitOf(std::string const& word)
    {
        if (word == "g" || word == "gold" || word == "golds" || word == "gp")
            return GOLD;
        if (word == "s" || word == "silver" || word == "silvers" || word == "silber" || word == "sp")
            return SILVER;
        if (word == "c" || word == "copper" || word == "coppers" || word == "kupfer" || word == "cp")
            return 1;
        return 0;
    }

    /// Reads digits with an optional fraction ("3", "3.92") at text[i]; false if there are none.
    bool ReadNumber(std::string const& text, size_t& i, double& number, bool& fraction)
    {
        size_t const start = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
            ++i;
        if (i == start || i - start > 7)
            return false;
        fraction = false;
        if (i + 1 < text.size() && text[i] == '.' && std::isdigit(static_cast<unsigned char>(text[i + 1])))
        {
            fraction = true;
            ++i;
            while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
                ++i;
        }
        number = std::strtod(text.substr(start, i - start).c_str(), nullptr);
        return true;
    }

    /// Reads an amount of money that starts at words[at]: "5g", "1g20s", "3.5g", "20 s", "3 gold 50 silver". With
    /// bare, a number with a fraction and no unit ("3.92") is gold. Returns how many words it took, 0 if there is
    /// no money here.
    size_t ReadMoney(std::vector<std::string> const& words, size_t at, uint64& copper, bool bare = false)
    {
        double total = 0.0;
        size_t taken = 0;
        while (at + taken < words.size())
        {
            std::string const& word = words[at + taken];
            // One word: numbers and units in turns.
            double sum = 0.0;
            size_t i = 0;
            bool whole = !word.empty(), fraction = false;
            double number = 0.0;
            while (i < word.size() && whole)
            {
                if (!ReadNumber(word, i, number, fraction))
                {
                    whole = false;
                    break;
                }
                size_t const start = i;
                while (i < word.size() && std::isalpha(static_cast<unsigned char>(word[i])))
                    ++i;
                uint32 const unit = UnitOf(word.substr(start, i - start));
                if (!unit)
                {
                    whole = false;
                    break;
                }
                sum += number * unit;
            }
            if (whole && i == word.size())
            {
                total += sum;
                ++taken;
                continue;
            }
            // Two words: a number, then a unit.
            i = 0;
            if (ReadNumber(word, i, number, fraction) && i == word.size())
            {
                if (at + taken + 1 < words.size() && UnitOf(words[at + taken + 1]))
                {
                    total += number * UnitOf(words[at + taken + 1]);
                    taken += 2;
                    continue;
                }
                if (bare && fraction && !taken)
                {
                    total += number * double(GOLD);
                    ++taken;
                }
            }
            break;
        }
        copper = uint64(std::llround(total));
        return copper ? taken : 0;
    }

    bool IsEachWord(std::string const& word)
    {
        return word == "each" || word == "ea" || word == "per" || word == "apiece" || word == "/ea" || word == "/each" || word == "p/u" ||
            word == "je" || word == "pro";
    }

    std::string MoneyText(uint64 copper)
    {
        uint64 const gold = copper / GOLD, silver = (copper % GOLD) / SILVER, rest = copper % SILVER;
        std::string text;
        if (gold)
            text += std::to_string(gold) + "g";
        if (silver)
            text += (text.empty() ? "" : " ") + std::to_string(silver) + "s";
        if (rest || text.empty())
            text += (text.empty() ? "" : " ") + std::to_string(rest) + "c";
        return text;
    }

    template <size_t N>
    bool IsOneOf(std::string const& text, char const* const (&list)[N])
    {
        for (char const* entry : list)
            if (text == entry)
                return true;
        return false;
    }

    // What the bots say. {g} stands for the goods ("14 Copper Ore"), {m} for the money ("9s 50c"). Many ways to say
    // each thing, so that two bots - or the same bot twice - seldom sound alike.
    namespace Lines
    {
        /// A handful of lines for when the catalog has none (it was emptied, or not loaded).
        struct Pool
        {
            char const* occasion;
            std::vector<char const*> lines;
        };

        // A bot sells and the price the player named is fine.
        Pool const SellOk = { "trade.sell_ok", {
            "i have {g}, {m} is fine. want me to mail it COD?",
            "{g} for {m}? sure. say yes and i mail it to you COD",
            "got {g} here, {m} works for me. shall i send it COD?",
            "{m} for {g} sounds fair. i can mail it COD, just say yes",
            "yeah i can do {g} for {m}. want it by mail, COD?",
            "{g}? have that. {m} is ok with me, i'd send it COD",
            "sure, {m} for {g}. say the word and it's in the mail (COD)",
            "i'll sell you {g} for {m}, no problem. COD by mail ok?",
            "deal from my side: {g}, {m}. yes and i send it COD",
            "{m} is good. i have {g} in my bags, can mail it COD right away",
            "works for me, {g} at {m}. should i post it to you COD?",
            "can do. {g} for {m}, sent COD if you say yes",
            "fine by me, {m} for {g}. mail COD?",
            "you can have {g} for {m}. just say yes and i send it COD"
        } };
        // A bot sells and names its own price.
        Pool const SellOffer = { "trade.sell_offer", {
            "i have {g}, {m} for all of it? say yes and i mail it COD",
            "got {g} for you, {m}. want it? i'd send it COD",
            "{g} here, {m} and it's yours. i can mail it COD",
            "i can sell you {g} for {m}, sent COD if you want",
            "have {g} lying around. {m}? i'd mail it COD",
            "{g}, {m}. interested? would send it COD",
            "saw you're looking for that. {g} for {m}, by mail COD?",
            "i could let {g} go for {m}. yes and it's in the mail (COD)",
            "need {g}? i'd take {m} for it. COD by mail",
            "{m} and i send you {g}, COD",
            "got some in my bags: {g} for {m}. say yes and i mail it COD",
            "i've got {g}. how about {m}? i'd post it COD",
            "selling {g} for {m} if you want it. mail, COD",
            "can offer {g}, {m} total. want me to send it COD?",
            "{g} for {m}, that's my price. COD by mail if ok",
            "you can have {g} from me, {m}. i'd mail it COD",
            "just farmed {g}. {m} and i mail it to you COD",
            "{g}? sure. {m}, sent COD. yes or no?"
        } };
        // A bot buys and the price the player named is fine.
        Pool const BuyOk = { "trade.buy_ok", {
            "i'll take {g} for {m}. mail it to me COD",
            "{g} for {m}, ok. send it COD and i'll pay",
            "{m} for {g} is fine, just mail it COD",
            "sure, i'd buy {g} at {m}. send it COD",
            "{m}? fair enough. mail me {g} COD",
            "yes please, {g} for {m}. COD by mail and i pay on arrival",
            "i can use that. {g}, {m}, send COD",
            "works for me: {m} for {g}. just post it COD",
            "ok deal, {g} for {m}. mail it COD to me",
            "{m} is fine with me. send {g} COD and you get your money",
            "i'm in, {g} at {m}. COD mail please",
            "good price. mail {g} to me COD for {m}"
        } };
        // A bot buys and names its own price.
        Pool const BuyOffer = { "trade.buy_offer", {
            "i'd give you {m} for {g}. mail it to me COD if that works",
            "i could use {g}, {m}? send it COD and i'll pay",
            "{m} for {g}? if ok just mail it COD",
            "i'll take {g} for {m}, COD by mail",
            "would buy {g} from you, {m}. send COD if you agree",
            "i need that! {m} for {g}? mail it COD",
            "can pay {m} for {g}. COD mail and it's done",
            "{g}... i'd pay {m}. just send it COD",
            "interested in {g}. {m} ok? then mail it COD to me",
            "{m} is what i can do for {g}. post it COD if that's fine",
            "been looking for that. {m} for {g}, send it COD?",
            "i'd buy {g}, offering {m}. COD by mail",
            "how about {m} for {g}? if yes, mail it COD",
            "{g} for {m} and we're good. send COD",
            "my offer: {m} for {g}. mail it COD and i pay when it lands",
            "could take {g} off your hands for {m}. COD mail",
            "{m}. that's what {g} is worth to me. send COD if ok"
        } };
        // A bot sells; the player wants another number of pieces.
        Pool const SellRecount = { "trade.sell_recount", {
            "sure, {g} for {m} then. yes?",
            "ok, {g} would be {m}. want me to mail it COD?",
            "no problem. {g}, {m}. say yes and i send it COD",
            "can do, {g} for {m}",
            "{g}? that's {m} then. deal?",
            "alright, {g} comes to {m}. shall i send it?",
            "fine, {m} for {g}. yes and it's in the mail",
            "ok {g} it is, {m}. COD by mail?",
            "sure thing. {m} for {g}, sent COD if you say yes",
            "{g} for {m}, works for me. send it?"
        } };
        // A bot sells; the player wants more than it has.
        Pool const SellAllIHave = { "trade.sell_all_i_have", {
            "{g} is all i have. {m} for that?",
            "don't have that many, only {g}. {m}?",
            "i can only do {g}, {m}. want it?",
            "that's more than i've got. {g} for {m}?",
            "sorry, {g} is everything. {m} and it's yours",
            "only {g} in my bags. {m}, COD by mail?"
        } };
        // A bot buys; the player offers another number of pieces.
        Pool const BuyRecount = { "trade.buy_recount", {
            "ok, {g} for {m} then. mail it COD",
            "fine, i'd take {g} for {m}. send it COD if ok",
            "{g}? i'd pay {m} for that. COD by mail",
            "sure, {m} for {g}. just send it COD",
            "alright, {g} then, {m}. mail it COD and i pay",
            "works too. {m} for {g}, COD",
            "ok {g}, that makes {m}. send it COD",
            "{g} is fine, {m}. post it COD whenever"
        } };
        // A bot buys; the player offers more than it wants.
        Pool const BuyAllINeed = { "trade.buy_all_i_need", {
            "i only need {g}. {m} for that?",
            "that's more than i can use. {g} for {m}?",
            "can't take that many, {g} is enough. {m}?",
            "i'd only take {g}, {m}. send that COD if ok",
            "{g} is all i can afford right now. {m}?",
            "too many for me, just {g} please. {m}, COD"
        } };
        // The player said no.
        Pool const Declined = { "trade.declined", {
            "ok, no problem", "alright, maybe next time", "np", "ok, gl", "sure, no worries", "fair enough", "alright", "ok np, good luck",
            "no worries", "k, another time then", "all good", "ok then", "sure thing", "ok, see ya", "np, gl selling", "right, never mind then"
        } };
        // A bot sells; the player went too low once.
        Pool const SellCounter = { "trade.sell_counter", {
            "can't go that low. {m} is the best i can do",
            "hm no. {m} and we have a deal",
            "too low for me, {m}?",
            "that's below what i get elsewhere. {m}, last price",
            "nah, a bit more. {m}?",
            "i can come down to {m}, not lower",
            "meet me at {m} and it's yours",
            "ouch. {m} is as low as i go",
            "not for that, sorry. {m} would work",
            "{m}. can't do better than that",
            "hmm. {m} then, final offer",
            "you're killing me. {m} and i send it"
        } };
        // A bot sells; the player went too low twice.
        Pool const SellRefuse = { "trade.sell_refuse", {
            "sorry, then i'd rather keep it", "no deal then, sorry", "nah, can't do that", "then i'll put it in the ah, sorry",
            "too low, i'll pass", "sorry, not for that price", "no, then i keep it", "can't, sorry. gl", "that's less than a vendor gives me, no",
            "nope, sorry", "then no, maybe someone else has it cheaper", "we're too far apart, sorry"
        } };
        // A bot buys; the player went too high once.
        Pool const BuyCounter = { "trade.buy_counter", {
            "that's too much for me. {m} is as far as i go",
            "hm, {m} and i take it",
            "can't pay that. {m}?",
            "a bit steep. {m} is my limit",
            "i can go up to {m}, no more",
            "{m}, that's all i can spare",
            "not that much, sorry. {m}?",
            "meet me at {m}?",
            "hmm. {m}, last offer",
            "too rich for me. {m} would work",
            "i'm not that wealthy :) {m}?",
            "{m} and you have a buyer"
        } };
        // A bot buys; the player went too high twice.
        Pool const BuyRefuse = { "trade.buy_refuse", {
            "then no thanks", "too much for me, sorry", "i'll pass then", "can't afford that, sorry", "no, then i'll look in the ah",
            "sorry, too expensive", "then not, gl selling", "nah, i'm out", "that's more than it's worth to me, sorry", "pass, sorry",
            "we won't get there, np", "ok then no. good luck with it"
        } };
        // A bot buys and the price is settled.
        Pool const BuyAgreed = { "trade.buy_agreed", {
            "deal. mail me {g} COD for {m} and i pay when it arrives",
            "ok! send {g} COD, {m}",
            "great, {g} for {m}, COD by mail",
            "perfect. {g}, COD {m}, i pay as soon as it's there",
            "done. just mail it COD for {m}",
            "nice, thanks. COD {m} and send it over",
            "deal! waiting for the mail, {m} COD",
            "cool. {g} COD for {m}, i'll check my mailbox",
            "agreed. send it COD ({m}) whenever you're at a mailbox",
            "ok good. mail {g}, {m} COD",
            "sweet, {m} it is. send it COD",
            "sounds good, i'll pay the COD of {m} when it lands"
        } };
        // A bot sells but the goods went elsewhere meanwhile.
        Pool const Gone = { "trade.gone", {
            "ah sorry, i don't have it anymore", "sorry, just sold it", "damn, it's gone already. sorry", "oh, too late, someone else got it",
            "sorry, used it up myself a minute ago", "argh, sold it in the ah just now. sorry", "sorry mate, it's gone", "i was too slow, don't have it now. sorry"
        } };
        Pool const BagTrouble = { "trade.bag_trouble", {
            "sorry, something's wrong with my bags. another time", "hm, can't send it right now, sorry", "the mail won't take it right now, sorry. later maybe"
        } };
        // A bot sent its parcel; when it arrives follows.
        Pool const Shipped = { "trade.shipped", {
            "sent! {m} COD, should be in your mailbox {when}",
            "it's in the mail, {m} COD. arrives {when}",
            "done, mailed it COD for {m}. you'll have it {when}",
            "on its way, {m} COD. should be there {when}",
            "posted it. {m} COD, check your mail {when}",
            "ok sent, COD {m}. it'll show up {when}",
            "mailed. {m} on delivery, you get it {when}",
            "there you go, sent it COD ({m}). arrives {when}",
            "thanks! parcel's out, {m} COD. it lands {when}",
            "shipped :) {m} COD. look in your mailbox {when}"
        } };
        // A parcel that is not what was agreed.
        Pool const ParcelWrong = { "trade.parcel_wrong", {
            "that's not what we said ({m} for {g}). i'll leave the parcel, it comes back to you",
            "hm, we said {g} for {m}. not taking this one, sorry",
            "the parcel doesn't match our deal ({g}, {m}). i'll let it go back",
            "we agreed on {m} for {g}, this is something else. leaving it in the mail",
            "uh, that COD is not what we said ({m} for {g}). not paying that, sorry",
            "wrong parcel? we said {g} for {m}. it'll return to you"
        } };
        // A parcel held more than was agreed: the rest goes back.
        Pool const ParcelExtra = { "trade.parcel_extra", {
            "you sent more than we said, so {g} is in the mail back to you",
            "that was more than i asked for. sent {g} back to you",
            "thanks! there was too much in the parcel though, {g} is on its way back",
            "i only needed what we agreed on. the other {g} is in your mailbox",
            "got more than we said, mailed {g} back. only paying for what i asked for wouldn't be fair either way",
            "you put in too many, {g} coming back to you by mail",
            "careful, you sent extra. {g} returned to you",
            "kept what we agreed, the rest ({g}) is in the mail to you"
        } };
        // A bot paid for the player's parcel.
        Pool const ParcelPaid = { "trade.parcel_paid", {
            "got it, thanks! {m} is in the mail to you",
            "parcel arrived, paid {m}. thx!",
            "thanks, money's on its way ({m})",
            "received, paid the COD of {m}. cheers",
            "there it is. paid {m}, thanks a lot",
            "thx! took the parcel, {m} is yours",
            "arrived :) paid {m}. good doing business",
            "got the mail, {m} sent back to you. thanks",
            "perfect, exactly what i needed. paid {m}",
            "paid {m}. thanks, pleasure",
            "all good, parcel's here and {m} is on the way to you",
            "nice, thank you. {m} paid"
        } };
    }

    /// What a player asked for in a channel.
    struct Ask
    {
        bool buy = false;               // the player wants to buy; a bot sells
        uint32 count = 0;               // 0: did not say
        uint64 price = 0;               // 0: did not say
        bool each = false;
        std::vector<uint32> items;      // every item of that name
    };

    /// Reads a line of chat: which way the trade goes, how many, of what, for how much. False: not about trade.
    bool Parse(std::string const& msg, Ask& ask, std::string& name, uint32& linked)
    {
        linked = 0;
        std::string const text = Lower(Plain(msg, linked));

        // The first word of trade in the line says which way it goes.
        size_t at = std::string::npos, length = 0;
        auto look = [&](char const* word, bool buy)
        {
            size_t const size = std::strlen(word);
            for (size_t pos = text.find(word); pos != std::string::npos; pos = text.find(word, pos + 1))
            {
                bool const before = pos == 0 || !IsWordChar(text[pos - 1]);
                bool const after = pos + size >= text.size() || !IsWordChar(text[pos + size]);
                if (before && after && (pos < at || (pos == at && size > length)))
                {
                    at = pos;
                    length = size;
                    ask.buy = buy;
                }
                if (before && after)
                    break;
            }
        };
        for (char const* word : BuyWords)
            look(word, true);
        for (char const* word : SellWords)
            look(word, false);
        if (at == std::string::npos)
            return false;

        std::vector<std::string> const words = Words(text.substr(at + length));
        name.clear();
        for (size_t i = 0; i < words.size(); ++i)
        {
            std::string const& word = words[i];
            // "20", "20x", "x20": how many. A bare number counts as that only before the name - "20 copper ore" is
            // twenty pieces of ore, not twenty copper.
            {
                std::string digits = word;
                bool marked = false;
                if (digits.size() > 1 && digits.back() == 'x')
                {
                    digits.pop_back();
                    marked = true;
                }
                else if (digits.size() > 1 && digits.front() == 'x')
                {
                    digits.erase(0, 1);
                    marked = true;
                }
                bool const shortUnit = i + 1 < words.size() && words[i + 1].size() <= 2 && UnitOf(words[i + 1]);
                if (!ask.count && IsNumber(digits) && digits.size() <= 4 && (marked || (name.empty() && !shortUnit)))
                {
                    ask.count = uint32(std::strtoul(digits.c_str(), nullptr, 10));
                    continue;
                }
            }
            uint64 money = 0;
            if (size_t const taken = ReadMoney(words, i, money))
            {
                ask.price = money;
                ask.each = i + taken < words.size() && IsEachWord(words[i + taken]);
                break;
            }
            if (word == "for" || word == "at" || word == "@")
            {
                if (ReadMoney(words, i + 1, money))
                    continue;
                if (name.empty())
                    continue;
            }
            if (IsOneOf(word, Filler) && (name.empty() || i + 1 == words.size()))
                continue;
            name += (name.empty() ? "" : " ") + word;
        }
        return true;
    }
    /// What a player answered a bot.
    struct Reply
    {
        bool yes = false, no = false;
        uint64 money = 0;               // a price, 0 if none
        bool each = false;              // ... for one piece
        uint32 count = 0;               // another number of pieces, 0 if none
        bool stacks = false;            // ... counted in stacks
        bool all = false, half = false;
    };

    /// A number written out, as far as people do that in chat; -1 if it is none.
    int32 WordNumber(std::string const& word)
    {
        static std::pair<char const*, int32> const numbers[] = {
            { "one", 1 }, { "two", 2 }, { "three", 3 }, { "four", 4 }, { "five", 5 }, { "six", 6 }, { "seven", 7 }, { "eight", 8 }, { "nine", 9 },
            { "ten", 10 }, { "eleven", 11 }, { "twelve", 12 }, { "fifteen", 15 }, { "twenty", 20 }, { "thirty", 30 }, { "forty", 40 }, { "fifty", 50 },
            { "sixty", 60 }, { "seventy", 70 }, { "eighty", 80 }, { "ninety", 90 }, { "hundred", 100 },
            { "ein", 1 }, { "eins", 1 }, { "eine", 1 }, { "einen", 1 }, { "zwei", 2 }, { "drei", 3 }, { "vier", 4 }, { "f\xc3\xbcnf", 5 }, { "fuenf", 5 },
            { "sechs", 6 }, { "sieben", 7 }, { "acht", 8 }, { "neun", 9 }, { "zehn", 10 }, { "elf", 11 }, { "zw\xc3\xb6lf", 12 }, { "zwoelf", 12 },
            { "zwanzig", 20 }, { "dreissig", 30 }, { "f\xc3\xbcnfzig", 50 }, { "fuenfzig", 50 }, { "hundert", 100 }
        };
        for (auto const& number : numbers)
            if (word == number.first)
                return number.second;
        return -1;
    }

    bool IsCountNoun(std::string const& word)
    {
        return word == "of" || word == "pieces" || word == "piece" || word == "pcs" || word == "pc" || word == "stack" || word == "stacks" ||
            word == "x" || word == "st\xc3\xbc" "ck" || word == "stueck" || word == "stk" || word == "please" || word == "pls" || word == "plz";
    }

    bool IsCountLead(std::string const& word)
    {
        return word == "only" || word == "just" || word == "need" || word == "take" || word == "want" || word == "have" || word == "got" || word == "for" ||
            word == "nur" || word == "brauche" || word == "nehme" || word == "habe" || word == "like" || word == "send";
    }

    /// Reads the whisper of a player: yes or no, a price in whatever way it is written ("3g", "3 gold 2 silver",
    /// "three gold", "3.92", "3,92", "50s each"), another number of pieces ("only need 3", "3 of them", "x5", "2 stacks", "half").
    Reply ReadReply(std::string const& msg)
    {
        Reply reply;
        uint32 linked = 0;
        std::string text = Lower(Plain(msg, linked));
        // "3,92" is 3.92.
        for (size_t i = 1; i + 1 < text.size(); ++i)
            if (text[i] == ',' && std::isdigit(static_cast<unsigned char>(text[i - 1])) && std::isdigit(static_cast<unsigned char>(text[i + 1])))
                text[i] = '.';

        std::vector<std::string> words = Words(text);
        for (std::string& word : words)
            while (word.size() > 1 && (word.back() == '.' || word.back() == ':'))
                word.pop_back();
        // Numbers written out become digits where they stand for a number: before a unit or a word of counting,
        // after a word like "only", or alone.
        for (size_t i = 0; i < words.size(); ++i)
        {
            int32 const number = WordNumber(words[i]);
            if (number < 0)
                continue;
            bool const before = i + 1 < words.size() && (UnitOf(words[i + 1]) || IsCountNoun(words[i + 1]));
            bool const after = i > 0 && IsCountLead(words[i - 1]);
            if (before || after || words.size() == 1)
                words[i] = std::to_string(number);
        }

        std::string joined;
        for (std::string const& word : words)
            joined += (joined.empty() ? "" : " ") + word;
        while (!joined.empty() && (joined.back() == '.' || joined.back() == '!'))
            joined.pop_back();

        size_t moneyAt = words.size(), moneyWords = 0;
        for (size_t i = 0; i < words.size() && !reply.money; ++i)
        {
            uint64 money = 0;
            if (size_t const taken = ReadMoney(words, i, money, true))
            {
                reply.money = money;
                reply.each = i + taken < words.size() && IsEachWord(words[i + taken]);
                moneyAt = i;
                moneyWords = taken;
            }
        }
        for (size_t i = 0; i < words.size() && !reply.count; ++i)
        {
            if (i >= moneyAt && i < moneyAt + moneyWords)
                continue;
            std::string digits = words[i];
            if (digits.size() > 1 && digits.back() == 'x')
                digits.pop_back();
            else if (digits.size() > 1 && digits.front() == 'x')
                digits.erase(0, 1);
            if (IsNumber(digits) && digits.size() <= 4)
            {
                reply.count = uint32(std::strtoul(digits.c_str(), nullptr, 10));
                reply.stacks = i + 1 < words.size() && (words[i + 1] == "stack" || words[i + 1] == "stacks");
            }
            else if (words[i] == "half" || words[i] == "h\xc3\xa4lfte" || words[i] == "haelfte")
                reply.half = true;
            else if (words[i] == "all" || words[i] == "everything" || words[i] == "alles" || words[i] == "alle")
                reply.all = true;
        }

        bool const other = reply.money || reply.count || reply.half;
        reply.yes = IsOneOf(joined, YesWords) || (!words.empty() && IsOneOf(words.front(), YesWords) && !other);
        reply.no = !reply.yes && !other && !reply.all && (IsOneOf(joined, NoWords) || (!words.empty() && IsOneOf(words.front(), NoWords)));
        return reply;
    }

    struct Deal
    {
        ObjectGuid bot, player;
        bool botSells = false;
        uint32 item = 0;
        std::vector<uint32> items;      // before the offer is made: the items the name can mean
        uint32 count = 0;
        uint32 most = 0;                // as many as the bot has to spare, or would take
        uint64 asked = 0;               // what the player named, 0 if nothing
        bool askedEach = false;
        uint64 price = 0;               // what the bot stands at, for all of it
        uint64 limit = 0;               // as far as it would go: the least it sells for, the most it pays
        uint8 stage = 0;                // 0 has not answered yet, 1 made its offer, 2 paid a parcel while away and has to send back what was too much
        uint8 haggled = 0;
        time_t at = 0;                  // when it answers
        time_t until = 0;               // when it stops waiting
        time_t noted = 0;               // when the log last said why a parcel is not paid yet
        uint32 mail = 0;                // stage 2: the paid parcel that holds more than was agreed
    };

    struct Heard
    {
        ObjectGuid player, bot;         // bot: a whisper to it
        std::string text;
    };

    class Deals
    {
    public:
        // Chat is handled wherever the server reads the player's packets; the bots are thought about later, in the
        // world's own update.
        void Hear(Player* player, Player* bot, std::string const& text)
        {
            std::lock_guard<std::mutex> guard(_lock);
            if (_heard.size() < 200)
                _heard.push_back({ player->GetGUID(), bot ? bot->GetGUID() : ObjectGuid::Empty, text });
        }

        void Update(uint32 diff)
        {
            if (!cfg.enabled || !cfg.deals)
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
            {
                Player* player = ObjectAccessor::FindConnectedPlayer(one.player);
                if (!player || !player->IsInWorld())
                    continue;
                if (one.bot)
                    Answered(player, one.bot, one.text, now);
                else
                    Asked(player, one.text, now);
            }

            for (size_t i = 0; i < _deals.size();)
            {
                Deal& deal = _deals[i];
                bool keep = deal.until > now;
                if (keep && deal.stage == 0 && deal.at <= now)
                    keep = Offer(deal, now);
                if (keep)
                    ++i;
                else
                    _deals.erase(_deals.begin() + i);
            }

            // Parcels a player sent to a bot that buys, and the money of parcels a bot sent.
            if (++_mailTicks >= 20)
            {
                _mailTicks = 0;
                for (size_t i = 0; i < _deals.size();)
                {
                    if (_deals[i].stage >= 1 && !_deals[i].botSells && Parcel(_deals[i], now))
                        _deals.erase(_deals.begin() + i);
                    else
                        ++i;
                }
                for (auto waiting = _awaiting.begin(); waiting != _awaiting.end();)
                {
                    if (waiting->second < now)
                    {
                        waiting = _awaiting.erase(waiting);
                        continue;
                    }
                    if (Player* bot = ObjectAccessor::FindConnectedPlayer(waiting->first))
                        if (bot->IsInWorld() && !bot->IsBeingTeleported())
                            CollectBotMail(bot);
                    ++waiting;
                }
                Save(false);
            }
        }

        /// What bots wait for: the parcels of players they agreed to buy from. Kept in a table, so that a bot
        /// still knows after a restart which parcel it has to pay - mail can take an hour to arrive, and the
        /// server may be switched off in between.
        void Load()
        {
            _stored.clear();
            _table = false;
            if (!cfg.saveMemory)
                return;
            CharacterDatabase.DirectExecute(
                "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_deals` ("
                "`bot` INT UNSIGNED NOT NULL, `player` INT UNSIGNED NOT NULL, `item` INT UNSIGNED NOT NULL, `count` INT UNSIGNED NOT NULL, "
                "`price` BIGINT UNSIGNED NOT NULL, `until` BIGINT UNSIGNED NOT NULL, `paid` INT UNSIGNED NOT NULL DEFAULT 0, PRIMARY KEY (`bot`, `player`, `item`)) "
                "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-playerbots-auctions: parcels of players that bots agreed to pay for'");
            _table = true;
            // `paid`: the mail a bot paid while it was away, when that mail holds more than was agreed.
            if (!CharacterDatabase.Query("SHOW COLUMNS FROM `mod_playerbots_auctions_deals` LIKE 'paid'"))
                CharacterDatabase.DirectExecute("ALTER TABLE `mod_playerbots_auctions_deals` ADD COLUMN `paid` INT UNSIGNED NOT NULL DEFAULT 0");
            CharacterDatabase.DirectExecute(
                "DELETE d FROM `mod_playerbots_auctions_deals` d LEFT JOIN `characters` b ON b.`guid` = d.`bot` LEFT JOIN `characters` p ON p.`guid` = d.`player` "
                "WHERE b.`guid` IS NULL OR p.`guid` IS NULL");

            // The time the server was off does not count: nobody could send anything.
            time_t const now = GameTime::GetGameTime().count();
            uint64 const away = AwayFor();
            uint32 kept = 0;
            if (QueryResult result = CharacterDatabase.Query("SELECT `bot`, `player`, `item`, `count`, `price`, `until`, `paid` FROM `mod_playerbots_auctions_deals`"))
                do
                {
                    Field* fields = result->Fetch();
                    Deal deal;
                    deal.bot = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
                    deal.player = ObjectGuid::Create<HighGuid::Player>(fields[1].Get<uint32>());
                    deal.item = fields[2].Get<uint32>();
                    deal.count = deal.most = fields[3].Get<uint32>();
                    deal.price = deal.limit = fields[4].Get<uint64>();
                    deal.until = time_t(fields[5].Get<uint64>() + away);
                    deal.mail = fields[6].Get<uint32>();
                    deal.stage = deal.mail ? 2 : 1;
                    deal.haggled = 1;
                    deal.at = now;
                    if (!deal.count || deal.until <= now || !sObjectMgr->GetItemTemplate(deal.item))
                        continue;
                    _deals.push_back(deal);
                    ++kept;
                } while (result->NextRow());
            if (kept)
                LOG_INFO("server.loading", ">> PlayerbotsAuctions: {} parcel(s) of players are still expected by bots that agreed to buy.", kept);
            Save(true);
        }

        /// A paid parcel out of which the surplus still has to go back: nobody else empties it.
        bool Holds(uint32 mailId) const
        {
            for (Deal const& deal : _deals)
                if (deal.stage == 2 && deal.mail == mailId)
                    return true;
            return false;
        }

        /// Writes the table anew when something about the open purchases has changed.
        void Save(bool wait)
        {
            if (!_table)
                return;
            std::string rows;
            for (Deal const& deal : _deals)
                if (deal.stage >= 1 && !deal.botSells && deal.item && deal.count)
                    rows += Acore::StringFormat("{}({}, {}, {}, {}, {}, {}, {})", rows.empty() ? "" : ", ", deal.bot.GetCounter(), deal.player.GetCounter(),
                        deal.item, deal.count, deal.price, uint64(deal.until), deal.mail);
            if (rows == _stored)
                return;
            _stored = rows;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            trans->Append("DELETE FROM `mod_playerbots_auctions_deals`");
            if (!rows.empty())
                trans->Append("REPLACE INTO `mod_playerbots_auctions_deals` (`bot`, `player`, `item`, `count`, `price`, `until`, `paid`) VALUES " + rows);
            if (wait)
                CharacterDatabase.DirectCommitTransaction(trans);
            else
                CharacterDatabase.CommitTransaction(trans);
        }

        /// The names of everything that can be traded, read once: the first line of chat does not have to wait for it.
        void Names()
        {
            _names.clear();
            LoadNames();
        }

    private:
        // ------------------------------------------------------------------------------------- understanding

        void LoadNames()
        {
            if (!_names.empty())
                return;
            for (auto const& entry : *sObjectMgr->GetItemTemplateStore())
            {
                ItemTemplate const& proto = entry.second;
                if (proto.Bonding == BIND_WHEN_PICKED_UP || proto.Bonding == BIND_QUEST_ITEM || !IsAllowedKind(&proto) || proto.Name1.empty())
                    continue;
                _names[Squeeze(proto.Name1)].push_back(proto.ItemId);
            }
        }

        std::vector<uint32> const* ItemsNamed(std::string const& name)
        {
            LoadNames();
            std::string const key = Squeeze(name);
            if (key.size() < 3)
                return nullptr;
            auto found = _names.find(key);
            if (found == _names.end() && key.back() == 's')
                found = _names.find(key.substr(0, key.size() - 1));       // "copper bars"
            return found != _names.end() ? &found->second : nullptr;
        }

        bool Understand(std::string const& msg, Ask& ask)
        {
            std::string name;
            uint32 linked = 0;
            if (!Parse(msg, ask, name, linked))
                return false;

            // Linked with a shift-click or typed out: both will do. A link says exactly which item is meant; other
            // items of the same name are the same thing to whoever reads it.
            if (linked)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(linked);
                if (!proto || !IsAllowedKind(proto))
                    return false;
                ask.items.push_back(linked);
                name = proto->Name1;
            }
            if (std::vector<uint32> const* items = ItemsNamed(name))
                for (uint32 const id : *items)
                    if (id != linked)
                        ask.items.push_back(id);
            return !ask.items.empty();
        }

        // ------------------------------------------------------------------------------------- the bots' side

        /// Would this bot part with it? What it neither wears nor uses, has no plan for and no craft that works it.
        static bool Spares(Player* bot, PlayerbotAI* botAI, Item* item)
        {
            if (!IsSellable(bot, item))
                return false;
            ItemTemplate const* proto = item->GetTemplate();
            if (ReservedForCraft(bot, proto->ItemId) || IsHandout(bot, proto) || !Market::Base(proto))
                return false;
            if (proto->TotemCategory && bot->GetItemCount(proto->ItemId) <= 1)
                return false;
            ItemUsage const usage = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", proto->ItemId)->Get();
            if (usage == ITEM_USAGE_AH || usage == ITEM_USAGE_VENDOR || (usage == ITEM_USAGE_NONE && !proto->SellPrice))
                return true;
            return (usage == ITEM_USAGE_SKILL || usage == ITEM_USAGE_KEEP) &&
                (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_GEM) && !UsesInCraft(bot, proto->ItemId);
        }

        static void Spare(Player* bot, PlayerbotAI* botAI, uint32 itemId, std::vector<Item*>& stacks)
        {
            auto look = [&](Item* item)
            {
                if (item && item->GetEntry() == itemId && Spares(bot, botAI, item))
                    stacks.push_back(item);
            };
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                look(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
                if (Bag* bag = bot->GetBagByPos(bagSlot))
                    for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                        look(bag->GetItemByPos(slot));
        }

        /// How much a bot wants something a player offers, as in the auction house: 0 not at all.
        double Interest(Player* bot, PlayerbotAI* botAI, ItemTemplate const* proto, bool& resale, time_t now)
        {
            resale = false;
            if (!Market::Base(proto) || IsHandout(bot, proto))
                return 0.0;
            bool const material = UsesInCraft(bot, proto->ItemId);
            auto had = _bought.find(bot->GetGUID().GetCounter());
            if (!material && had != _bought.end())
            {
                auto before = had->second.find(proto->ItemId);
                if (before != had->second.end() && before->second + 4 * HOUR > now)
                    return 0.0;                 // one of a kind is enough for now
            }
            switch (botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", proto->ItemId)->Get())
            {
                case ITEM_USAGE_EQUIP:
                case ITEM_USAGE_REPLACE:
                    return 1.5;
                case ITEM_USAGE_USE:
                case ITEM_USAGE_SKILL:
                case ITEM_USAGE_AMMO:
                    return 1.2;
                case ITEM_USAGE_AH:
                case ITEM_USAGE_VENDOR:
                case ITEM_USAGE_NONE:
                    if (material && bot->GetItemCount(proto->ItemId) < proto->GetMaxStackSize())
                        return 1.0;
                    if (TraitOf(bot, TRAIT_TRADING) > 0.75f && TraitOf(bot, TRAIT_KNOWLEDGE) > 0.5f && IsAllowedKind(proto))
                    {
                        resale = true;          // a trader takes what is clearly cheap, to sell it on
                        return 0.6;
                    }
                    return 0.0;
                default:
                    return 0.0;
            }
        }

        static double Spendable(Player* bot)
        {
            if (!cfg.buyUseBotMoney)
                return double(MAX_MONEY_AMOUNT);
            return double(bot->GetMoney()) * (0.85 - 0.5 * TraitOf(bot, TRAIT_THRIFT));
        }

        static void Tell(Player* bot, Player* player, std::string const& text)
        {
            bot->Whisper(text, LANG_UNIVERSAL, player);
        }

        static std::string Goods(uint32 count, ItemTemplate const* proto)
        {
            return count > 1 ? std::to_string(count) + " " + proto->Name1 : proto->Name1;
        }

        /// One of many ways to say it.
        /// One of many ways to say it: from the catalog, in the voice of the bot's personality where there are
        /// such lines, else one of the handful built in.
        static std::string Pick(Player* bot, Lines::Pool const& pool)
        {
            std::string const line = PhraseFor(bot, pool.occasion);
            if (!line.empty())
                return line;
            return pool.lines[urand(0, uint32(pool.lines.size()) - 1)];
        }

        /// The first whisper of a bot now and then starts with a word of greeting.
        static std::string Hello()
        {
            static char const* const words[] = { "hey, ", "hi, ", "yo, ", "heya, ", "hey there, ", "hi there, ", "psst, ", "hello, ", "sup, ", "oh hey, " };
            return urand(0, 99) < 30 ? words[urand(0, std::size(words) - 1)] : "";
        }

        static std::string Fill(std::string text, std::string const& goods, std::string const& money, std::string const& when = "soon")
        {
            for (size_t pos; (pos = text.find("{when}")) != std::string::npos;)
                text.replace(pos, 6, when);
            for (size_t pos; (pos = text.find("{g}")) != std::string::npos;)
                text.replace(pos, 3, goods);
            for (size_t pos; (pos = text.find("{m}")) != std::string::npos;)
                text.replace(pos, 3, money);
            return text;
        }

        // ------------------------------------------------------------------------------------- a player asks

        void Asked(Player* player, std::string const& text, time_t now)
        {
            Ask ask;
            if (!Understand(text, ask))
                return;
            // Not every few seconds anew.
            time_t& last = _lastAsk[player->GetGUID().GetCounter()];
            if (last + 10 > now)
                return;
            last = now;

            // What the player sells it has to carry.
            if (!ask.buy)
            {
                ask.items.erase(std::remove_if(ask.items.begin(), ask.items.end(), [&](uint32 id) { return !player->GetItemCount(id); }), ask.items.end());
                if (ask.items.empty())
                    return;
            }

            std::vector<Player*> willing;
            PlayerBotMap const bots = sRandomPlayerbotMgr.GetAllBots();
            for (auto const& entry : bots)
            {
                Player* bot = entry.second;
                if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->GetTeamId() != player->GetTeamId() || bot->GetLevel() < cfg.minLevel)
                    continue;
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                if (!botAI || botAI->GetMaster())
                    continue;
                if (std::any_of(_deals.begin(), _deals.end(), [&](Deal const& deal) { return deal.bot == bot->GetGUID() && deal.player == player->GetGUID(); }))
                    continue;
                bool fits = false;
                if (ask.buy)
                {
                    for (uint32 const id : ask.items)
                    {
                        std::vector<Item*> stacks;
                        if (bot->GetItemCount(id))
                            Spare(bot, botAI, id, stacks);
                        if (!stacks.empty())
                        {
                            fits = true;
                            break;
                        }
                    }
                }
                else
                {
                    bool resale;
                    for (uint32 const id : ask.items)
                        if (Interest(bot, botAI, sObjectMgr->GetItemTemplate(id), resale, now) > 0.0)
                        {
                            fits = true;
                            break;
                        }
                }
                // Not everybody reads the channel, and not everybody can be bothered.
                if (fits && frand(0.0f, 1.0f) < 0.4f + 0.5f * TraitOf(bot, TRAIT_TRADING))
                    willing.push_back(bot);
            }
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} wants to {} \"{}\" - {} bot(s) could answer.", player->GetName(), ask.buy ? "buy" : "sell",
                    sObjectMgr->GetItemTemplate(ask.items.front())->Name1, willing.size());
            if (willing.empty())
                return;

            Acore::Containers::RandomShuffle(willing);
            if (willing.size() > cfg.dealAnswers)
                willing.resize(cfg.dealAnswers);
            time_t at = now + urand(4, 12);
            for (Player* bot : willing)
            {
                Deal deal;
                deal.bot = bot->GetGUID();
                deal.player = player->GetGUID();
                deal.botSells = ask.buy;
                deal.items = ask.items;
                deal.count = ask.count;
                deal.asked = ask.price;
                deal.askedEach = ask.each;
                deal.at = at;
                deal.until = at + 10 * MINUTE;
                _deals.push_back(deal);
                at += urand(5, 20);
            }
        }

        /// The bot makes its offer. False: it has thought better of it.
        bool Offer(Deal& deal, time_t now)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(deal.bot);
            Player* player = ObjectAccessor::FindConnectedPlayer(deal.player);
            if (!bot || !player || !bot->IsInWorld() || !player->IsInWorld() || bot->IsBeingTeleported())
                return false;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                return false;
            float const greed = TraitOf(bot, TRAIT_GREED), patience = TraitOf(bot, TRAIT_PATIENCE), thrift = TraitOf(bot, TRAIT_THRIFT);

            if (deal.botSells)
            {
                ItemTemplate const* proto = nullptr;
                uint32 have = 0;
                for (uint32 const id : deal.items)
                {
                    std::vector<Item*> stacks;
                    Spare(bot, botAI, id, stacks);
                    uint32 count = 0;
                    for (Item* stack : stacks)
                        count += stack->GetCount();
                    if (count > have)
                    {
                        have = count;
                        proto = sObjectMgr->GetItemTemplate(id);
                    }
                }
                if (!proto || !have)
                    return false;
                deal.item = proto->ItemId;
                // As many as were asked for; without a number a few stacks at the most.
                uint32 const stack = std::max<uint32>(1, proto->GetMaxStackSize());
                deal.most = std::min<uint32>(have, stack * MAX_MAIL_ITEMS);
                deal.count = std::min(deal.most, deal.count ? deal.count : (stack > 1 ? stack * 2 : 1));

                // Its price as at the auctioneers, with nobody to undercut - and without the cut of the house.
                double const value = std::max(market.Value(proto), Market::Regular(proto) * 0.6);
                double each = value * (0.95 + 0.25 * greed) * frand(0.97f, 1.08f);
                double const least = std::max(each * (0.8 + 0.14 * patience), double(proto->SellPrice) * cfg.minVendorFactor);
                each = std::max(each, least);
                deal.price = std::max<uint64>(HumanPrice(each * deal.count), 2);
                deal.limit = std::min<uint64>(deal.price, uint64(std::ceil(least * deal.count)));

                uint64 const named = deal.asked * (deal.askedEach ? deal.count : 1);
                std::string const goods = Goods(deal.count, proto);
                if (named && named >= deal.limit)
                {
                    deal.price = std::min<uint64>(named, MAX_MONEY_AMOUNT);       // the player's own price will do
                    Tell(bot, player, Hello() + Fill(Pick(bot, Lines::SellOk), goods, MoneyText(deal.price)));
                }
                else
                    Tell(bot, player, Hello() + Fill(Pick(bot, Lines::SellOffer), goods, MoneyText(deal.price)));
            }
            else
            {
                ItemTemplate const* proto = nullptr;
                double interest = 0.0;
                bool resale = false;
                for (uint32 const id : deal.items)
                {
                    bool trader;
                    ItemTemplate const* candidate = sObjectMgr->GetItemTemplate(id);
                    double const wants = player->GetItemCount(id) ? Interest(bot, botAI, candidate, trader, now) : 0.0;
                    if (wants > interest)
                    {
                        interest = wants;
                        resale = trader;
                        proto = candidate;
                    }
                }
                if (!proto)
                    return false;
                deal.item = proto->ItemId;
                uint32 const stack = std::max<uint32>(1, proto->GetMaxStackSize());
                uint32 const offered = player->GetItemCount(proto->ItemId);
                uint32 const wanted = deal.count;
                // One piece of gear is enough; of a material as much as fills its stock.
                deal.count = offered;
                if (stack == 1 && !resale)
                    deal.count = 1;
                else if (!resale)
                    deal.count = std::min<uint32>(deal.count, stack * 2);
                deal.count = std::min<uint32>(deal.count, stack * MAX_MAIL_ITEMS);

                double const value = market.Value(proto);
                double const spendable = Spendable(bot);
                double each = value * interest * frand(0.8f, 1.1f);
                if (!resale)
                {
                    each *= std::clamp(1.0 + 0.25 * std::log10(std::max(1.0, spendable / std::max(1.0, value * deal.count))), 0.85, 1.4);
                    each = std::max(each, double(proto->SellPrice) * cfg.buyMinVendorFactor);
                }
                double most = each * (1.05 + 0.15 * (1.0 - thrift));
                // Less than a vendor asks, or buying from a vendor and selling to the bots would print money.
                if (proto->BuyPrice > 0 && market.IsVendorItem(proto->ItemId))
                {
                    double const cap = double(proto->BuyPrice) / std::max<uint32>(1, proto->BuyCount) * cfg.buyVendorItemPercent / 100.0;
                    each = std::min(each, cap);
                    most = std::min(most, cap);
                }
                double const purse = std::min(spendable * 0.6, cfg.maxBuyout ? double(cfg.maxBuyout) : double(MAX_MONEY_AMOUNT));
                if (most * deal.count > purse)
                    deal.count = uint32(purse / std::max(1.0, most));
                if (!deal.count || each < 1.0)
                    return false;
                // That is as many as it would take; the player may have fewer in mind.
                deal.most = deal.count;
                if (wanted)
                    deal.count = std::min(deal.count, wanted);

                deal.limit = uint64(most * deal.count);
                deal.price = std::min<uint64>(std::max<uint64>(HumanPrice(each * deal.count), 1), deal.limit);
                uint64 const named = deal.asked * (deal.askedEach ? deal.count : 1);
                std::string const goods = Goods(deal.count, proto);
                if (named && named <= deal.limit)
                {
                    deal.price = named;
                    Tell(bot, player, Hello() + Fill(Pick(bot, Lines::BuyOk), goods, MoneyText(deal.price)));
                }
                else
                    Tell(bot, player, Hello() + Fill(Pick(bot, Lines::BuyOffer), goods, MoneyText(deal.price)));
                // The parcel of a player takes its time to arrive.
                deal.until = now + 3 * HOUR + sWorld->getIntConfig(CONFIG_MAIL_DELIVERY_DELAY);
            }

            if (deal.botSells)
                deal.until = now + 10 * MINUTE;
            deal.stage = 1;
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} offers {} to {} {} x{} for {} copper (as far as {}).", bot->GetName(), player->GetName(),
                    deal.botSells ? "sell" : "buy", sObjectMgr->GetItemTemplate(deal.item)->Name1, deal.count, deal.price, deal.limit);
            return true;
        }

        // ------------------------------------------------------------------------------------- the player answers

        void Answered(Player* player, ObjectGuid botGuid, std::string const& msg, time_t now)
        {
            auto found = std::find_if(_deals.begin(), _deals.end(), [&](Deal const& deal) { return deal.bot == botGuid && deal.player == player->GetGUID() && deal.stage == 1; });
            if (found == _deals.end())
            {
                ChatterHeard(player, msg, 5, botGuid);       // no deal between them: small talk
                return;
            }
            Deal& deal = *found;
            Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(deal.item);
            if (!bot || !proto)
                return;

            Reply const reply = ReadReply(msg);
            bool const yes = reply.yes, no = reply.no;

            // Another number of pieces: the price follows, and the bot says what that comes to.
            uint32 wanted = reply.count * (reply.stacks ? std::max<uint32>(1, proto->GetMaxStackSize()) : 1);
            if (reply.all)
                wanted = deal.most;
            else if (reply.half)
                wanted = std::max<uint32>(1, deal.count / 2);
            bool const tooMany = wanted > deal.most;
            wanted = std::min(wanted, deal.most);
            if (wanted && wanted != deal.count)
            {
                double const each = double(deal.price) / deal.count, eachLimit = double(deal.limit) / deal.count;
                deal.count = wanted;
                deal.price = std::max<uint64>(1, HumanPrice(each * wanted));
                deal.limit = deal.botSells ? std::min<uint64>(deal.price, uint64(std::ceil(eachLimit * wanted)))
                                           : std::max<uint64>(deal.price, uint64(eachLimit * wanted));
                deal.haggled = 0;
                if (deal.botSells)
                    deal.until = now + 10 * MINUTE;
                if (!reply.money)
                {
                    Tell(bot, player, Fill(Pick(bot, tooMany ? (deal.botSells ? Lines::SellAllIHave : Lines::BuyAllINeed)
                                                        : (deal.botSells ? Lines::SellRecount : Lines::BuyRecount)), Goods(deal.count, proto), MoneyText(deal.price)));
                    return;
                }
            }
            else if (tooMany && !reply.money)
            {
                Tell(bot, player, Fill(Pick(bot, deal.botSells ? Lines::SellAllIHave : Lines::BuyAllINeed), Goods(deal.count, proto), MoneyText(deal.price)));
                return;
            }

            uint64 const named = reply.money * (reply.each ? deal.count : 1);
            std::string const goods = Goods(deal.count, proto);

            if (no)
            {
                Tell(bot, player, Pick(bot, Lines::Declined));
                _deals.erase(found);
                return;
            }

            if (deal.botSells)
            {
                if (named && named < deal.price)
                {
                    if (named >= deal.limit)
                        deal.price = named;                     // it can live with that
                    else if (!deal.haggled)
                    {
                        deal.haggled = 1;
                        deal.price = std::max<uint64>(deal.limit, std::min<uint64>(deal.price, HumanPrice(double(deal.price + named) / 2.0)));
                        deal.until = now + 10 * MINUTE;
                        Tell(bot, player, Fill(Pick(bot, Lines::SellCounter),
                            goods, MoneyText(deal.price)));
                        return;
                    }
                    else
                    {
                        Tell(bot, player, Pick(bot, Lines::SellRefuse));
                        _deals.erase(found);
                        return;
                    }
                }
                else if (!yes && !named)
                    return;                                     // something else it was told: not about the deal
                Ship(deal, bot, player, proto, now);
                _deals.erase(found);
                return;
            }

            // The bot buys.
            if (named && named > deal.price)
            {
                if (named <= deal.limit)
                    deal.price = named;
                else if (!deal.haggled)
                {
                    deal.haggled = 1;
                    deal.price = std::min<uint64>(deal.limit, std::max<uint64>(deal.price, HumanPrice(double(deal.price + named) / 2.0)));
                    Tell(bot, player, Fill(Pick(bot, Lines::BuyCounter),
                        goods, MoneyText(deal.price)));
                    return;
                }
                else
                {
                    Tell(bot, player, Pick(bot, Lines::BuyRefuse));
                    _deals.erase(found);
                    return;
                }
            }
            else if (named)
                deal.price = named;                             // cheaper than it offered: gladly
            else if (!yes)
                return;
            Tell(bot, player, Fill(Pick(bot, Lines::BuyAgreed),
                goods, MoneyText(deal.price)));
        }

        // ------------------------------------------------------------------------------------- the mail

        /// The bot puts the goods into the mail, cash on delivery.
        void Ship(Deal const& deal, Player* bot, Player* player, ItemTemplate const* proto, time_t now)
        {
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            std::vector<Item*> stacks;
            if (botAI && bot->IsInWorld() && !bot->IsBeingTeleported())
                Spare(bot, botAI, deal.item, stacks);
            uint32 have = 0;
            for (Item* stack : stacks)
                have += stack->GetCount();
            if (have < deal.count)
            {
                Tell(bot, player, Pick(bot, Lines::Gone));
                return;
            }

            // Whole stacks first, so as little as possible has to be split.
            std::sort(stacks.begin(), stacks.end(), [](Item* a, Item* b) { return a->GetCount() > b->GetCount(); });
            MailDraft draft(proto->Name1, "As agreed. Thanks!");
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            uint32 left = deal.count, parcels = 0;
            for (Item* stack : stacks)
            {
                if (!left || parcels >= MAX_MAIL_ITEMS)
                    break;
                Item* sent;
                if (stack->GetCount() <= left)
                {
                    left -= stack->GetCount();
                    sent = stack;
                    sent->SetNotRefundable(bot);
                    bot->MoveItemFromInventory(sent->GetBagSlot(), sent->GetSlot(), true);
                    sent->DeleteFromInventoryDB(trans);
                    if (sent->GetState() == ITEM_UNCHANGED)
                        sent->FSetState(ITEM_CHANGED);
                }
                else
                {
                    sent = Item::CreateItem(deal.item, left, bot);
                    if (!sent)
                        break;
                    uint32 taken = left;
                    bot->DestroyItemCount(stack, taken, true);
                    left = 0;
                }
                sent->SetOwnerGUID(player->GetGUID());
                sent->SaveToDB(trans);
                draft.AddItem(sent);
                ++parcels;
            }

            if (!parcels)
            {
                Tell(bot, player, Pick(bot, Lines::BagTrouble));
                return;
            }

            // Postage, as a player pays it.
            uint32 const postage = 30 * parcels;
            if (cfg.buyUseBotMoney && bot->HasEnoughMoney(postage))
                bot->ModifyMoney(-int32(postage));
            uint64 const due = deal.price * (deal.count - left) / deal.count;
            uint32 const delay = cfg.dealMailDelay < 0 ? sWorld->getIntConfig(CONFIG_MAIL_DELIVERY_DELAY) : uint32(cfg.dealMailDelay);
            draft.AddCOD(uint32(std::min<uint64>(due, MAX_MONEY_AMOUNT)))
                .SendMailTo(trans, MailReceiver(player, player->GetGUID().GetCounter()), MailSender(bot), MAIL_CHECK_MASK_HAS_BODY, delay);
            bot->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            // The money comes back by mail when the player takes the goods; unpaid, the parcel returns after three days.
            _awaiting[bot->GetGUID()] = now + delay + 4 * DAY;
            std::string const when = delay >= 50 * MINUTE ? "in about an hour" : delay >= 2 * MINUTE ? "in a few minutes" : "now";
            Tell(bot, player, Fill(Pick(bot, Lines::Shipped), "", MoneyText(due), when));
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} mailed {} x{} to {}, {} copper on delivery.", bot->GetName(), proto->Name1, deal.count - left, player->GetName(), due);
        }

        /// What the server does when a player presses "Return" on a mail.
        void SendBack(Player* bot, Mail* mail)
        {
            uint32 const id = mail->messageID;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
            stmt->SetData(0, id);
            trans->Append(stmt);
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
            stmt->SetData(0, id);
            trans->Append(stmt);
            bot->RemoveMail(id);

            MailDraft draft(mail->subject, mail->body);
            for (MailItemInfo const& info : mail->items)
            {
                if (Item* item = bot->GetMItem(info.item_guid))
                    draft.AddItem(item);
                bot->RemoveMItem(info.item_guid);
            }
            draft.AddMoney(mail->money).SendReturnToSender(bot->GetSession()->GetAccountId(), mail->receiver, mail->sender, trans);
            CharacterDatabase.CommitTransaction(trans);
            delete mail;
            bot->m_mailsUpdated = true;
            PBA_MAIL_DELETED(bot->GetGUID());
        }

        /// A parcel that holds more than was agreed: what is too much goes back to the player by mail, the
        /// bot keeps what it asked for. Works on the mail before anything is taken out of it. Returns how many
        /// pieces went back.
        uint32 SendSurplus(Player* bot, Player* player, Mail* mail, Deal const& deal, CharacterDatabaseTransaction trans)
        {
            uint32 total = 0;
            for (MailItemInfo const& info : mail->items)
                if (Item* item = bot->GetMItem(info.item_guid))
                    total += item->GetCount();
            if (total <= deal.count)
                return 0;
            uint32 left = total - deal.count;
            uint32 const surplus = left;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(deal.item);
            MailDraft draft(proto ? proto->Name1 : "Too many", "You sent more than we agreed on. Here is the rest back.");
            std::vector<MailItemInfo> const attached = mail->items;
            uint32 parcels = 0;
            for (MailItemInfo const& info : attached)
            {
                if (!left || parcels >= MAX_MAIL_ITEMS)
                    break;
                Item* item = bot->GetMItem(info.item_guid);
                if (!item)
                    continue;
                Item* back;
                if (item->GetCount() <= left)
                {
                    // The whole stack leaves the bot's mail.
                    left -= item->GetCount();
                    back = item;
                    mail->RemoveItem(info.item_guid);
                    bot->RemoveMItem(info.item_guid);
                    // Its place in the old mail is given up here and now: the same statement run after the
                    // new mail is written would take it out of that one as well.
                    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM);
                    stmt->SetData(0, info.item_guid);
                    trans->Append(stmt);
                    if (back->GetState() == ITEM_UNCHANGED)
                        back->FSetState(ITEM_CHANGED);
                }
                else
                {
                    // Part of a stack: a new item for the player, the stack in the mail gets smaller.
                    back = item->CloneItem(left, bot);
                    if (!back)
                        break;
                    item->SetCount(item->GetCount() - left);
                    if (item->GetState() == ITEM_UNCHANGED)
                        item->FSetState(ITEM_CHANGED);
                    item->SaveToDB(trans);
                    left = 0;
                }
                back->SetOwnerGUID(deal.player);
                back->SaveToDB(trans);
                draft.AddItem(back);
                ++parcels;
            }
            if (!parcels)
                return 0;
            draft.SendMailTo(trans, MailReceiver(player, deal.player.GetCounter()), MailSender(bot), MAIL_CHECK_MASK_HAS_BODY, 0);
            mail->state = MAIL_STATE_CHANGED;
            return surplus - left;
        }

        /// Writes down what was taken out of a mail; a mail with nothing left in it is gone.
        void StoreMail(Player* bot, Mail* mail, CharacterDatabaseTransaction trans)
        {
            if (mail->items.empty() && !mail->money)
            {
                mail->state = MAIL_STATE_DELETED;
                PBA_MAIL_DELETED(bot->GetGUID());
                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
                stmt->SetData(0, mail->messageID);
                trans->Append(stmt);
                stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
                stmt->SetData(0, mail->messageID);
                trans->Append(stmt);
                mail->removedItems.clear();
                return;
            }
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_MAIL);
            stmt->SetData(0, uint8(1));
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

        void SaySurplus(Player* bot, Player* player, Deal const& deal, uint32 back)
        {
            if (!back)
                return;
            if (player && player->IsInWorld())
                Tell(bot, player, Fill(Pick(bot, Lines::ParcelExtra), Goods(back, sObjectMgr->GetItemTemplate(deal.item)), ""));
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: {} mailed {} piece(s) of item {} back to a player: the parcel held more than the {} agreed on.",
                    bot->GetName(), back, deal.item, deal.count);
        }

        /// The parcel for a bot that is not online, straight in the database. True: paid, the deal is over.
        template <typename Waits>
        bool PaidWhileAway(Deal& deal, time_t now, Waits& waits)
        {
            ObjectGuid::LowType const botLow = deal.bot.GetCounter(), playerLow = deal.player.GetCounter();
            QueryResult result = CharacterDatabase.Query(
                "SELECT m.`id`, m.`cod`, m.`subject`, COUNT(mi.`item_guid`), COALESCE(SUM(ii.`count`), 0), COALESCE(SUM(ii.`itemEntry` <> {}), 0) "
                "FROM `mail` m LEFT JOIN `mail_items` mi ON mi.`mail_id` = m.`id` LEFT JOIN `item_instance` ii ON ii.`guid` = mi.`item_guid` "
                "WHERE m.`receiver` = {} AND m.`sender` = {} AND m.`messageType` = 0 AND m.`cod` > 0 AND m.`deliver_time` <= {} "
                "GROUP BY m.`id`, m.`cod`, m.`subject` ORDER BY m.`id` LIMIT 1", deal.item, botLow, playerLow, uint64(now));
            if (!result)
            {
                waits("the bot is not online, and no parcel has arrived for it");
                return false;
            }
            Field* fields = result->Fetch();
            uint32 const mailId = fields[0].Get<uint32>();
            uint32 const cost = fields[1].Get<uint32>();
            std::string const subject = fields[2].Get<std::string>();
            uint32 const count = fields[4].Get<uint32>();
            bool const only = fields[3].Get<uint64>() > 0 && fields[5].Get<uint64>() == 0;
            uint64 const fair = deal.price * std::min(count, deal.count) / deal.count;
            if (!only || !count || cost > fair)
            {
                waits("the bot is not online, and the parcel is not what was agreed - it goes back when the bot returns");
                return false;
            }
            if (cfg.buyUseBotMoney)
            {
                uint32 money = 0;
                if (QueryResult purse = CharacterDatabase.Query("SELECT `money` FROM `characters` WHERE `guid` = {} AND `online` = 0", botLow))
                    money = purse->Fetch()[0].Get<uint32>();
                else
                {
                    waits("the bot is logging in");
                    return false;
                }
                if (money < cost)
                {
                    waits(Acore::StringFormat("the bot is not online, has {} copper and needs {}", money, cost));
                    return false;
                }
            }

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            if (cfg.buyUseBotMoney)
                trans->Append("UPDATE `characters` SET `money` = `money` - {} WHERE `guid` = {} AND `money` >= {}", cost, botLow, cost);
            // Paid goods wait like any mail without cash on delivery: thirty days, not the three of an unpaid parcel
            // (after which the server would send them back to the player, who has the money already).
            trans->Append("UPDATE `mail` SET `cod` = 0, `expire_time` = {} WHERE `id` = {}", uint64(now + 30 * DAY), mailId);
            Player* player = ObjectAccessor::FindConnectedPlayer(deal.player);
            MailDraft(subject, "")
                .AddMoney(cost)
                .SendMailTo(trans, MailReceiver(player, playerLow), MailSender(MAIL_NORMAL, botLow), MAIL_CHECK_MASK_COD_PAYMENT);
            CharacterDatabase.CommitTransaction(trans);

            _bought[botLow][deal.item] = now;
            market.RecordSale(deal.item, cost, count);
            if (cfg.debug)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(deal.item);
                LOG_INFO("module", "PlayerbotsAuctions: a bot that is not online paid {} copper for a parcel with {} x{} from a player.",
                    cost, proto ? proto->Name1 : "?", count);
            }
            if (count > deal.count)
            {
                // More than was agreed: what is too much goes back as soon as the bot is here to send it.
                deal.stage = 2;
                deal.mail = mailId;
                deal.until = now + 30 * DAY;
                deal.noted = 0;
                return false;
            }
            return true;
        }

        /// Has the player's parcel arrived at the bot that buys? It pays if it holds what was agreed. True: the deal is over.
        bool Parcel(Deal& deal, time_t now)
        {
            // With Debug on, the log says once a minute why a parcel is still unpaid.
            auto waits = [&](std::string const& why)
            {
                if (!cfg.debug || deal.noted + MINUTE > now)
                    return;
                deal.noted = now;
                ItemTemplate const* wanted = sObjectMgr->GetItemTemplate(deal.item);
                std::string name;
                sCharacterCache->GetCharacterNameByGuid(deal.bot, name);
                LOG_INFO("module", "PlayerbotsAuctions: the parcel for {} ({} x{} for {} copper) is not paid yet: {}.",
                    name.empty() ? "a bot" : name, wanted ? wanted->Name1 : "?", deal.count, deal.price, why);
            };

            Player* bot = ObjectAccessor::FindConnectedPlayer(deal.bot);
            if (deal.stage == 2)
            {
                if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
                    return false;
                Mail* mail = bot->GetMail(deal.mail);
                if (mail && mail->state != MAIL_STATE_DELETED)
                {
                    Player* player = ObjectAccessor::FindConnectedPlayer(deal.player);
                    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                    uint32 const back = SendSurplus(bot, player, mail, deal, trans);
                    StoreMail(bot, mail, trans);
                    CharacterDatabase.CommitTransaction(trans);
                    bot->m_mailsUpdated = true;
                    SaySurplus(bot, player, deal, back);
                }
                return true;
            }
            // Random bots take turns being online, and one that is off can stay off for hours. The player is
            // not kept waiting for that: the parcel is paid from what the bot has, and the goods lie in its
            // mailbox until it is back.
            if (!bot)
                return PaidWhileAway(deal, now, waits);
            if (!bot->IsInWorld() || bot->IsBeingTeleported())
            {
                waits("the bot is on its way into the world");
                return false;
            }
            Player* player = ObjectAccessor::FindConnectedPlayer(deal.player);
            uint32 fromPlayer = 0, onTheWay = 0;

            std::vector<Mail*> const mails(bot->GetMails().begin(), bot->GetMails().end());
            for (Mail* mail : mails)
            {
                if (!mail || mail->state == MAIL_STATE_DELETED || mail->messageType != MAIL_NORMAL || mail->sender != deal.player.GetCounter())
                    continue;
                ++fromPlayer;
                if (mail->deliver_time > now)
                {
                    ++onTheWay;
                    continue;
                }
                if (!mail->COD || mail->items.empty())
                    continue;

                // Only what was agreed, and not for more than was agreed.
                uint32 count = 0;
                bool only = true;
                std::vector<Item*> items;
                for (MailItemInfo const& info : mail->items)
                {
                    Item* item = bot->GetMItem(info.item_guid);
                    if (!item || info.item_template != deal.item)
                    {
                        only = false;
                        break;
                    }
                    count += item->GetCount();
                    items.push_back(item);
                }
                uint64 const fair = deal.price * std::min(count, deal.count) / deal.count;
                if (!only || !count || mail->COD > fair)
                {
                    if (cfg.debug)
                        LOG_INFO("module", "PlayerbotsAuctions: {} sends a parcel back: {}, {} piece(s), {} copper on delivery; agreed were {} piece(s) of item {} for {} copper.",
                            bot->GetName(), only ? "the agreed item" : "something else in it", count, mail->COD, deal.count, deal.item, deal.price);
                    // Back to the player at once, as with "Return" at a mailbox - not after the three days an
                    // unpaid parcel lies around. The deal stands: the right parcel is still welcome.
                    SendBack(bot, mail);
                    if (player && player->IsInWorld())
                        Tell(bot, player, Fill(Pick(bot, Lines::ParcelWrong), Goods(deal.count, sObjectMgr->GetItemTemplate(deal.item)), MoneyText(deal.price)));
                    return false;
                }

                // It pays when it has the money. What does not fit into its bags stays in its mailbox, paid for,
                // and is taken out when there is room - full bags must not keep a player waiting for money.
                uint32 const cost = mail->COD;
                if (cfg.buyUseBotMoney && !bot->HasEnoughMoney(cost))
                {
                    waits(Acore::StringFormat("the bot has {} copper and needs {}", bot->GetMoney(), cost));
                    return false;
                }

                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                // More in it than was agreed: the rest goes back, the bot keeps what it asked for.
                uint32 const back = SendSurplus(bot, player, mail, deal, trans);
                std::vector<MailItemInfo> const attached = mail->items;
                for (MailItemInfo const& info : attached)
                {
                    Item* item = bot->GetMItem(info.item_guid);
                    ItemPosCountVec dest;
                    if (!item || bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                        continue;
                    mail->RemoveItem(info.item_guid);
                    mail->removedItems.push_back(info.item_guid);
                    bot->RemoveMItem(info.item_guid);
                    item->SetState(ITEM_UNCHANGED);
                    bot->MoveItemToInventory(dest, item, true);
                }
                if (cfg.buyUseBotMoney)
                    bot->ModifyMoney(-int32(cost));
                MailDraft(mail->subject, "")
                    .AddMoney(cost)
                    .SendMailTo(trans, MailReceiver(player, mail->sender), MailSender(MAIL_NORMAL, mail->receiver), MAIL_CHECK_MASK_COD_PAYMENT);
                mail->COD = 0;

                bot->SaveInventoryAndGoldToDB(trans);
                StoreMail(bot, mail, trans);
                CharacterDatabase.CommitTransaction(trans);
                bot->m_mailsUpdated = true;

                _bought[bot->GetGUID().GetCounter()][deal.item] = now;
                market.RecordSale(deal.item, cost, count - back);
                if (player && player->IsInWorld())
                    Tell(bot, player, Fill(Pick(bot, Lines::ParcelPaid),
                        "", MoneyText(cost)));
                SaySurplus(bot, player, deal, back);
                if (cfg.debug)
                    LOG_INFO("module", "PlayerbotsAuctions: {} paid {} copper for a parcel with {} x{} from a player.", bot->GetName(), cost,
                        sObjectMgr->GetItemTemplate(deal.item)->Name1, count);
                return true;
            }
            waits(onTheWay ? "it is still on its way" : fromPlayer ? "the mail from this player is not a parcel with cash on delivery" :
                "nothing from this player has arrived");
            return false;
        }

        std::mutex _lock;
        std::vector<Heard> _heard;
        std::vector<Deal> _deals;
        std::unordered_map<std::string, std::vector<uint32>> _names;
        std::unordered_map<ObjectGuid::LowType, time_t> _lastAsk;
        std::unordered_map<ObjectGuid::LowType, std::unordered_map<uint32, time_t>> _bought;
        std::map<ObjectGuid, time_t> _awaiting;       // bots that wait for the money of a parcel
        uint32 _timer = 0;
        uint32 _mailTicks = 0;
        bool _table = false;
        std::string _stored;                          // what the table holds
    };

    Deals deals;
}

    void LoadDealNames()
    {
        deals.Names();
        deals.Load();
    }

    void SaveDeals(bool wait)
    {
        deals.Save(wait);
    }

    bool DealHoldsMail(uint32 mailId)
    {
        return deals.Holds(mailId);
    }

    void DealsUpdate(uint32 diff)
    {
        deals.Update(diff);
    }

    void DealsHeard(Player* player, Player* bot, std::string const& text, std::string const& channel)
    {
        if (cfg.enabled && !cfg.deals && player && bot && !GET_PLAYERBOT_AI(player))
            ChatterHeard(player, text, 5, bot->GetGUID());      // with deals switched off a whisper is small talk
        if (!cfg.enabled || !cfg.deals || !player || text.empty() || text.size() > 255)
            return;
        // A bot does not haggle with a bot.
        if (GET_PLAYERBOT_AI(player) || sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId()))
            return;
        if (!bot && !cfg.dealChannels.empty())
        {
            std::string const name = Lower(channel);
            if (std::none_of(cfg.dealChannels.begin(), cfg.dealChannels.end(), [&](std::string const& part) { return name.find(part) != std::string::npos; }))
                return;
        }
        deals.Hear(player, bot, text);
    }
}
