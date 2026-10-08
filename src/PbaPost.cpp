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

#include <cstring>
#include <ctime>

namespace pba
{
    namespace
    {
        constexpr uint32 PostSender = 0x50424100;       // marks a gossip option as one of the post's
        constexpr uint32 TextBase   = 9113300;          // the texts of the gossip window; a game remembers a text by its number
        // "Auctions" of the post: this bit, 6 bits of count, 24 bits of item. Real auctions count up from 1 and
        // never get there. The top bit stays clear: the game takes such a number for no auction at all and
        // will not let the row be selected.
        constexpr uint32 IdFlag     = 0x40000000;
        constexpr uint32 IdMask     = 0xC0000000;
        constexpr uint32 MaxLot     = 60;
        constexpr uint32 SellerLow  = 0xFFFFFF00;       // + the house: the "player" the goods are listed under and the flyer comes from; no character has these numbers
        constexpr time_t Shortest   = 30 * MINUTE;      // a bid always waits at least this long

        ObjectGuid Seller(uint32 house)
        {
            return ObjectGuid::Create<HighGuid::Player>(SellerLow + house);
        }

        enum Action : uint32
        {
            ACT_BROWSE = 1,
            ACT_POST,
            ACT_INFO,
            ACT_BACK,
            ACT_FLYERS_OFF,
            ACT_FLYERS_ON,
            ACT_SAMPLE
        };

        // ---------------------------------------------------------------------------------- what is said

        enum : uint32
        {
            TEXT_MENU,
            TEXT_INFO,
            TEXT_SAMPLE,
            TEXT_COUNT
        };

        char const* const Texts[TEXT_COUNT] =
        {
            "Welcome, $N. The auction hall is open, as always.$B$BAnd if the hall cannot help you, the Trading Post might: "
            "we keep what the auctions have run out of. At a price.",

            "The Trading Post sells materials and crafted supplies - ore, herbs, cloth, leather, gems, potions, food, scrolls "
            "and the like - but only what the auction house is short of right now. What the hall has plenty of, we do not carry."
            "$B$BWhat we have is all there is until tomorrow, for everybody together, and the emptier a shelf, the dearer the rest. "
            "Each customer gets one lot of a thing a day.$B$BBuy it outright and it is in your mailbox at once. Or bid the "
            "lower price: then it goes with the caravan and reaches you when the auction ends - within a day at the latest. "
            "Either way it costs well above what things usually go for.",

            "Ah, $N! The free sample! Of course, of course. One moment...$B$B...$B$BThere. With the compliments of the Trading "
            "Post. Do come again - and do bring money next time."
        };

        /// The letters that come with the goods, by the clerks of each post.
        struct Letter
        {
            char const* house;      // horde, alliance, goblin
            char const* kind;       // express: sent at once; caravan: came with the caravan
            char const* text;       // {count}, {item}, {player}
        };

        Letter const Letters[] =
        {
        { "horde", "express", "{player},\nYou asked. Grukka delivers. Your {count} x {item} are in this mail. No waiting, no excuses, no weakness.\nCount them if you like. They are all there. I counted twice, and I do not count for fun.\nUse them well. Make the Horde stronger.\nLok'tar.\n- Grukka Ironjaw, Quartermaster of the Horde Trading Post" },
        { "horde", "express", "Warrior,\nThe crates were in my storehouse, now they are in your mailbox. {count} pieces of {item}, packed tight, ready for use.\nA peon tried to carry them too slowly. I fixed that. He runs now.\nDo not thank me. Thanks do not sharpen axes. Go do something worth singing about.\n- Grukka Ironjaw, Quartermaster of the Horde Trading Post" },
        { "horde", "express", "{player},\nStrength. Honor. Supplies. The third one is my job.\nHere: {count} x {item}, sent the moment your order hit my table. I do not make warriors wait. Waiting is for Alliance clerks and their paperwork.\nIf something is missing, you counted wrong. Count again.\n- Grukka Ironjaw, Quartermaster of the Horde Trading Post" },
        { "horde", "caravan", "{player},\nThe caravan is back. So are your {count} x {item}.\nRoad was bad. Centaur raiders came over a hill. My guards went over the hill after them. The centaur did not come back over the hill.\nYour goods were never touched. Not one scratch.\nHorde caravans arrive. That is the rule.\n- Grukka Ironjaw, Quartermaster of the Horde Trading Post" },
        { "horde", "caravan", "Warrior,\nYour order rode with the caravan through the Barrens. Hot. Dusty. A kodo sat down in the road and refused to move for an hour. I respect that kodo. It has will.\nNow it moved, and you have {count} pieces of {item} in your mail. Complete. Dry. Ready.\nGo. Use them. Win.\n- Grukka Ironjaw, Quartermaster of the Horde Trading Post" },
        { "horde", "caravan", "{player},\nThe wagons came in at dusk. Your crates came with them: {count} x {item}.\nA wheel broke near the crossroads. The drivers carried the wagon the last mile. On their backs. I gave them double rations. They earned it.\nYou waited for the caravan. Now you carry its sweat with you. Honor that.\n- Grukka Ironjaw, Quartermaster of the Horde Trading Post" },
        { "horde", "express", "Hey hey, {player}!\nZul'jabi here, mon. You wanted it now, so you got it now! Your {count} x {item} be sittin' in your mailbox already, smilin' at you.\nNo caravan, no waitin', no stubborn kodo. Just Zul'jabi runnin' fast like a raptor with a hot tail.\nStay cool, mon!\n- Zul'jabi, Caravan Master of the Horde Trading Post" },
        { "horde", "express", "Ey, friend!\nDe spirits say you be in a hurry. Zul'jabi listen to de spirits! Here be {count} pieces of {item}, packed wit' love and a little bit of sand.\nDon't worry about de sand, it be lucky sand. Probably.\nYou come back anytime, mon. De trading post always open, even when Zul'jabi be nappin'.\n- Zul'jabi, Caravan Master of the Horde Trading Post" },
        { "horde", "express", "{player}, my friend!\nYou order, I deliver, we both happy. Dat be de whole philosophy, mon.\nYour {count} x {item} be in de mail right now. I wrapped dem in banana leaves for freshness. Okay, maybe dat only matters for de food. Still smells nice though!\nWalk tall, keep your tusks shiny.\n- Zul'jabi, Caravan Master of the Horde Trading Post" },
        { "horde", "caravan", "Ey, {player}!\nDe caravan made it, mon! Took de long way past de coast 'cause a storm come rollin' in, big thunder, very dramatic. De kodos loved it. De drivers, not so much.\nBut look: {count} x {item}, all dry, all there, all yours.\nZul'jabi keep his promises, even in de rain!\n- Zul'jabi, Caravan Master of the Horde Trading Post" },
        { "horde", "caravan", "Hey hey, friend!\nGood news and funny news. Good news: your {count} pieces of {item} arrived wit' de caravan, safe and sound.\nFunny news: a monkey jumped on de lead wagon near de jungle and stole Zul'jabi's hat. Your crates? He didn't care. Only wanted de hat.\nI miss dat hat, mon.\n- Zul'jabi, Caravan Master of the Horde Trading Post" },
        { "horde", "caravan", "{player}, mon!\nDe wagons rolled in at sunset, all dusty and singin'. We lost a wheel in a gully, so we danced around de fire till it got fixed. Troll engineering, very relaxing.\nYour {count} x {item} been ridin' on top like a little king de whole way. Nothin' missin'.\nEnjoy, and tell your friends!\n- Zul'jabi, Caravan Master of the Horde Trading Post" },
        { "horde", "express", "Greetings, {player},\nThe wind carried your request to me, and I did not let it wait. Your {count} x {item} rest now in your mailbox, as a stone rests in the river.\nI have written your name in my ledger with care. Every name in it is a promise kept.\nWalk with the Earth Mother.\n- Thalsorn Mistwalker, Ledger-Keeper of the Horde Trading Post" },
        { "horde", "express", "Young one,\nSome things are worth the long road. Others are needed today. Yours was needed today, so I sent it at once: {count} pieces of {item}.\nI checked each bundle twice, slowly, as my grandfather taught me. Haste in the hand, patience in the heart.\nMay your path be green.\n- Thalsorn Mistwalker, Ledger-Keeper of the Horde Trading Post" },
        { "horde", "express", "Friend,\nThe ink in my ledger is still wet, and already your goods have gone: {count} x {item}, sent with no delay.\nThe young bulls laughed that an old ledger-keeper could move so fast. I reminded them that the plainstrider is old and swift as well. They stopped laughing.\nPeace upon your hooves, or feet.\n- Thalsorn Mistwalker, Ledger-Keeper of the Horde Trading Post" },
        { "horde", "caravan", "Greetings, {player},\nThe caravan has returned across the plains, and with it your {count} x {item}.\nWe walked beneath many stars. One night a great herd of thunder lizards crossed our path, and we waited until dawn for them to pass. Some things should not be rushed.\nAll is accounted for in my ledger.\n- Thalsorn Mistwalker, Ledger-Keeper of the Horde Trading Post" },
        { "horde", "caravan", "Friend,\nLong roads teach patience, and you have shown it. Your {count} pieces of {item} arrived with the caravan this evening, whole and unharmed.\nA young kodo named Mudfoot decided the river was a fine place to rest. We sang to him until he agreed to continue. He has a lovely voice, for a kodo.\nBe well.\n- Thalsorn Mistwalker, Ledger-Keeper of the Horde Trading Post" },
        { "horde", "caravan", "{player},\nThe dust of the road is still on my hooves as I write this. Hail fell on the second day, large as gnoll teeth, and we sheltered under the wagons. The goods sheltered under us.\nAnd so your {count} x {item} come to you complete, as the ledger says they must.\nThe Earth Mother watches over honest trade.\n- Thalsorn Mistwalker, Ledger-Keeper of the Horde Trading Post" },
        { "horde", "express", "Dear {player},\nHow refreshing, a customer who still has a pulse. Probably.\nYour {count} x {item} have been shipped at once. I do not sleep, I do not eat, and I do not take breaks, so there was simply nothing to delay them.\nDo try not to die before you use them. It is terribly inconvenient. Trust me.\n- Ambrose Hollowmere, Shipping Clerk of the Horde Trading Post" },
        { "horde", "express", "Greetings,\nEnclosed: {count} pieces of {item}, packed by hand. My hand, specifically, the left one. It stays attached most days.\nThey were shipped the moment your order arrived. Efficiency is one of the few joys left to me, along with filing and the quiet moaning of the wind.\nWith cold regards,\n- Ambrose Hollowmere, Shipping Clerk of the Horde Trading Post" },
        { "horde", "express", "{player},\nYour order has been processed, stamped, sealed and sent. Your {count} x {item} now await you in the mailbox, fresher than anything else in my office.\nIf you notice a faint smell of grave dirt on the wrapping, that is not the goods. That is me. I apologize. Somewhat.\nUntil our next transaction,\n- Ambrose Hollowmere, Shipping Clerk of the Horde Trading Post" },
        { "horde", "caravan", "Dear {player},\nThe caravan has arrived, and I am pleased to report your {count} x {item} survived the journey in better shape than our drivers.\nA pack of hungry wolves followed us through the woods for two nights. Our bat riders chased them off. Lovely creatures, bats. So few people appreciate them.\nRegards from the crypt,\n- Ambrose Hollowmere, Shipping Clerk of the Horde Trading Post" },
        { "horde", "caravan", "Greetings,\nIt rained the entire way through the marshes. I did not mind. Rot is merely a matter of perspective. Your goods, however, were wrapped in oilcloth and are perfectly dry.\n{count} pieces of {item}, delivered by caravan, every last one present and accounted for.\nI signed for you. My handwriting is dreadful, but so is everyone's.\n- Ambrose Hollowmere, Shipping Clerk of the Horde Trading Post" },
        { "horde", "caravan", "{player},\nThe caravan crawled in this morning, missing a wheel and gaining a curse. The curse is minor. Mostly it makes the wagon hum.\nYour {count} x {item} arrived complete and entirely uncursed. I checked with a priest. She was very confused to be asked.\nPatience rewarded, as the dead say. We have a great deal of it.\n- Ambrose Hollowmere, Shipping Clerk of the Horde Trading Post" },
        { "alliance", "express", "Most esteemed {player},\nIt is with the utmost satisfaction that I, on behalf of the noble house which so graciously sponsors this establishment, present to you {count} x {item}, dispatched forthwith.\nKindly note the wax seal. It bears the family crest. Do try not to get mud on it.\nWith refined regards,\n- Percival Ashcombe, Steward of the Alliance Trading Post" },
        { "alliance", "express", "Honored customer,\nOne does not keep a person of quality waiting. Accordingly, your {count} pieces of {item} were dispatched at once, by a footman instructed to walk briskly yet with dignity.\nHe managed the briskness. The dignity remains a work in progress.\nYour humble and impeccably dressed servant,\n- Percival Ashcombe, Steward of the Alliance Trading Post" },
        { "alliance", "express", "My dear {player},\nIt pleases me to inform you that your {count} x {item} await you in the mailbox, delivered without the slightest delay.\nI personally inspected each piece through my monocle. Twice. The monocle has since been polished, as it was dreadfully disappointed by the dust.\nLong live the King, and good taste.\n- Percival Ashcombe, Steward of the Alliance Trading Post" },
        { "alliance", "caravan", "Most esteemed {player},\nThe caravan has at last arrived, and your {count} x {item} have survived the indignities of the open road.\nBrigands accosted the wagons near the woods. Our guards dealt with them swiftly. One brigand complimented my embroidered banners before fleeing. A man of taste, wasted on crime.\nYours most properly,\n- Percival Ashcombe, Steward of the Alliance Trading Post" },
        { "alliance", "caravan", "Honored customer,\nI regret to report that the caravan was delayed by a most vulgar puddle, roughly the size of a small lake, which the drivers insisted upon calling a road.\nNevertheless, your {count} pieces of {item} have arrived in perfect order, which is more than I can say for my boots.\nWith dignity intact, barely,\n- Percival Ashcombe, Steward of the Alliance Trading Post" },
        { "alliance", "caravan", "My dear {player},\nThe wagons rolled in this afternoon, shedding mud upon the courtyard like peasants at a festival. Among them, safe and complete, your {count} x {item}.\nA wheel came loose near the farmlands. A farmer repaired it and asked for nothing but a handshake. I gave him two. Generosity becomes a gentleman.\nWith noble regards,\n- Percival Ashcombe, Steward of the Alliance Trading Post" },
        { "alliance", "express", "Dear {player},\nOrder logged at fourteen past three precisely! Goods dispatched at fourteen and a half past three! Your {count} x {item} are now in your mailbox, inventoried, labeled, cross-referenced and triple-checked.\nPlease do not put them back in a different order. I have a system.\nMeticulously yours,\n- Tibbly Gearwhistle, Bookkeeper of the Alliance Trading Post" },
        { "alliance", "express", "Hello hello!\nOrder received, order filed, order fulfilled! {count} pieces of {item}, sent at once via my patented Sorting-and-Dispatching Apparatus, Mark Seven.\nMark Six exploded. We do not talk about Mark Six. The new ledgers are fireproof now.\nWith great precision and only minor soot,\n- Tibbly Gearwhistle, Bookkeeper of the Alliance Trading Post" },
        { "alliance", "express", "Dear valued {player},\nI have entered your order in column B, row 2317 of the Great Ledger, and underlined it twice in blue, which means URGENT and COMPLETED.\nYour {count} x {item} have already been sent. If you find a smudge on the label, that was my thumb. I have since washed it. Twice.\nOrderly regards,\n- Tibbly Gearwhistle, Bookkeeper of the Alliance Trading Post" },
        { "alliance", "caravan", "Dear {player},\nThe caravan arrived exactly four hours and eleven minutes behind my projected schedule! I have written a very stern note to the weather.\nYour {count} x {item} are complete. I counted them, and then counted the counting. Everything balances.\nThe weather has not replied to my note. Rude.\n- Tibbly Gearwhistle, Bookkeeper of the Alliance Trading Post" },
        { "alliance", "caravan", "Hello hello!\nYour {count} pieces of {item} have arrived with the caravan, and my ledger is happy again! An unbalanced ledger gives me hives.\nThe drivers report a lost wheel, a grumpy ram and a short detour through a field of very tall grass. I have added three new columns to track such things in the future.\nTidily yours,\n- Tibbly Gearwhistle, Bookkeeper of the Alliance Trading Post" },
        { "alliance", "caravan", "Dear valued customer,\nGreat news, according to my calculations! The caravan made it, the crates made it, and your {count} x {item} made it, all present and correct.\nBandits did attempt to stop us. Our dwarf guards objected. I recorded the objection as Item 47: Brief Noisy Disagreement, Resolved.\nPrecisely yours,\n- Tibbly Gearwhistle, Bookkeeper of the Alliance Trading Post" },
        { "alliance", "express", "Oi, {player}!\nGood choice not waitin' on the caravan, I tell ye. The roads out there are a disgrace. Your {count} x {item} went straight to your mailbox, no ruts, no potholes, no mud up to the beard.\nIf only I could send meself the same way. Me knees would thank ye.\nCheers,\n- Durgan Stoutkeg, Caravan Master of the Alliance Trading Post" },
        { "alliance", "express", "Well met, lad or lass!\nYer {count} pieces of {item} are sent and sittin' in the mail already. Didn't even need me wagons for it.\nWhich is just as well, since the road to the pass is half washed away again, and nobody's fixed it since me grandfather complained about it. And he complained a LOT.\nBottoms up,\n- Durgan Stoutkeg, Caravan Master of the Alliance Trading Post" },
        { "alliance", "express", "{player}!\nQuick as a gryphon with its tail on fire, your {count} x {item} are in yer mailbox.\nNo road travel, ye lucky so-and-so. Ye'll never know the pain of a wagon seat after twelve hours on cobblestones laid by a blind kobold.\nGo on, enjoy 'em. I'll be here, rubbin' me backside.\n- Durgan Stoutkeg, Caravan Master of the Alliance Trading Post" },
        { "alliance", "caravan", "Oi, {player}!\nWe made it. Barely. The road through the hills has more holes than a gnome's excuse. Lost a wheel, found a wheel, lost it again.\nBut I'm a dwarf of me word: your {count} x {item} arrived whole, counted, and not a scratch on 'em. Can't say the same for me wagon.\nBring an ale next time ye visit.\n- Durgan Stoutkeg, Caravan Master of the Alliance Trading Post" },
        { "alliance", "caravan", "Well met!\nThe caravan's back and so are your {count} pieces of {item}, safe and sound.\nIt rained, then it snowed, then it rained on the snow. The road turned into porridge. Bad porridge. Me ram sat down in it and refused to budge till I sang to her. Don't tell anyone about the singin'.\nCheers,\n- Durgan Stoutkeg, Caravan Master of the Alliance Trading Post" },
        { "alliance", "caravan", "{player}!\nWhoever built the road between here and the coast should be made to walk it barefoot. Twice.\nRaptors in the bushes, rocks in the road, rain in me boots. And yet, by stubbornness and good dwarven axles, your {count} x {item} came through complete.\nYe're welcome. Now I need a long soak.\n- Durgan Stoutkeg, Caravan Master of the Alliance Trading Post" },
        { "alliance", "express", "Blessings, {player},\nThe Light guided my hands swiftly today. Your {count} x {item} have been sent at once and await you in your mailbox.\nI said a small prayer over the package. It cannot hurt, and it may help. That is true of most kindness.\nMay the Light walk beside you.\n- Ishaali, Shipping Clerk of the Alliance Trading Post" },
        { "alliance", "express", "Dear friend,\nIn the long years of our wandering, my people learned that help delayed is help denied. So I did not delay: {count} pieces of {item}, sent the moment I read your request.\nUse them well, and share your good fortune where you can.\nIn the Light,\n- Ishaali, Shipping Clerk of the Alliance Trading Post" },
        { "alliance", "express", "Blessings upon you, {player},\nYour {count} x {item} have been wrapped, sealed and sent without delay. I hummed a hymn while I packed them. The dwarf at the next desk hummed along, badly but sincerely.\nSincerity counts for much in the eyes of the Light.\nWalk in peace,\n- Ishaali, Shipping Clerk of the Alliance Trading Post" },
        { "alliance", "caravan", "Blessings, {player},\nThe caravan has arrived, and the Light be thanked, so have your {count} x {item}.\nWe were caught in a fierce storm on the road. I prayed, the drivers cursed, and the wagons held. I believe both helped.\nEverything is here, whole and undamaged. Patience has its reward.\nIn the Light,\n- Ishaali, Shipping Clerk of the Alliance Trading Post" },
        { "alliance", "caravan", "Dear friend,\nYour {count} pieces of {item} came to us with the caravan this evening, complete in every way.\nOn the road we met a lost elekk calf. We fed it, and it followed us the rest of the way. The quartermaster says we cannot keep it. I am praying on the matter.\nMay the Light keep you,\n- Ishaali, Shipping Clerk of the Alliance Trading Post" },
        { "alliance", "caravan", "Blessings upon you, {player},\nThe journey was long, but every road ends somewhere, and this one ended at your mailbox. Your {count} x {item} have arrived safely with the caravan.\nBandits watched us from a ridge for a whole day. In the end they only waved. Perhaps the Light softened their hearts. Perhaps it was our guards.\nGo in peace,\n- Ishaali, Shipping Clerk of the Alliance Trading Post" },
        { "goblin", "express", "Hey there, {player}, valued customer!\nYour {count} x {item} are in your mailbox right now. Fast service, best service, cartel-quality service!\nWhile I got you: ever considered buying MORE? Twice as many, twice the fun! Also we got rocket boots. Unrelated. Very tempting.\nTime is money, friend!\n- Gizzik Sharpcoin, Sales Manager of the Goblin Trading Post" },
        { "goblin", "express", "Greetings, top-tier customer!\nYou got taste, I can tell. That's why your {count} pieces of {item} went out the door the second you ordered. Zoom!\nNow, a customer of your caliber might also enjoy our deluxe gift wrapping, our premium crate polish, or a lovely commemorative mug. Just say the word.\nBuy big, live big,\n- Gizzik Sharpcoin, Sales Manager of the Goblin Trading Post" },
        { "goblin", "express", "{player}! My favorite person this hour!\nDelivered at once: {count} x {item}. You're welcome, you're welcome.\nHey, ever thought about a subscription? Every week, fresh goods, straight to your box, no thinking required. I'm drafting the contract now. Only nine pages. Ten with the cover.\nKeep those coins jingling,\n- Gizzik Sharpcoin, Sales Manager of the Goblin Trading Post" },
        { "goblin", "caravan", "Hey hey, {player}!\nThe caravan rolled in, and your {count} x {item} came with it, safe and sound!\nSmall note: some bandits tried to rob us at the canyon. I sold them insurance. Then they left. Best sales day of the month, honestly.\nNext time, maybe order double? The caravan's going anyway!\n- Gizzik Sharpcoin, Sales Manager of the Goblin Trading Post" },
        { "goblin", "caravan", "Valued customer!\nGreat news: your {count} pieces of {item} arrived with the caravan, every last one.\nEven greater news: the trip took so long that I designed a whole new product line! Rain-proof crates, kodo-proof crates, and crates that are also chairs. Interested? Of course you are.\nAlways selling,\n- Gizzik Sharpcoin, Sales Manager of the Goblin Trading Post" },
        { "goblin", "caravan", "{player}, my friend, my pal!\nThe wheel fell off. Then the spare wheel fell off. Then I sold both wheels to a passing tinker and bought better wheels. Profit even in disaster, that's the goblin way!\nYour {count} x {item} arrived complete and on the nicer wheels. You're welcome.\nCome back soon and bring friends!\n- Gizzik Sharpcoin, Sales Manager of the Goblin Trading Post" },
        { "goblin", "express", "Dear {player},\nYour {count} x {item} have been delivered at once. Lovely. Now, the fees.\nThere is the handling fee, the packing fee, the fee for writing this letter, and the fee for calculating the fees. All already included, don't panic! I just like listing them. It calms me.\nFiscally yours,\n- Fenny Tollwhistle, Fee Accountant of the Goblin Trading Post" },
        { "goblin", "express", "Greetings, customer!\nEnclosed: {count} pieces of {item}, sent the very moment you asked. Speed is a premium service, and I love premium services.\nThe envelope you are holding? Paid for. The ink? Paid for. The breath I took while writing? Honestly, I looked into it. Legal said no.\nWith careful accounting,\n- Fenny Tollwhistle, Fee Accountant of the Goblin Trading Post" },
        { "goblin", "express", "{player},\nDelivered! Your {count} x {item} are in the mailbox, and every tax, toll and tariff was settled before they left the warehouse.\nYou may notice a small stamp on each crate. That is the Stamp Fee stamp. It proves the stamp fee was paid. I am very proud of it.\nTariffs and best wishes,\n- Fenny Tollwhistle, Fee Accountant of the Goblin Trading Post" },
        { "goblin", "caravan", "Dear {player},\nThe caravan has arrived with your {count} x {item}, complete and accounted for.\nAlong the way we paid a bridge toll, a road toll, a toll to an ogre who said he owned a rock, and a toll to a second ogre who said he owned the first ogre. All covered. I kept the receipts. I always keep the receipts.\n- Fenny Tollwhistle, Fee Accountant of the Goblin Trading Post" },
        { "goblin", "caravan", "Greetings, customer!\nYour {count} pieces of {item} rode in with the caravan today. Delivered in full, no surprises, no hidden charges. The charges were all very visible. I made sure.\nA sandstorm delayed us a day. I tried to bill the sandstorm. It did not cooperate. Paperwork is pending.\nWith tidy books,\n- Fenny Tollwhistle, Fee Accountant of the Goblin Trading Post" },
        { "goblin", "caravan", "{player},\nWonderful news! The caravan made it, and so did your {count} x {item}.\nWe lost a wheel near the river. Replacing it involved a wheel fee, a labor fee and a moral-support fee for the driver, who cried a little. All absorbed by the post. You are welcome.\nMay your ledgers always balance,\n- Fenny Tollwhistle, Fee Accountant of the Goblin Trading Post" },
        { "goblin", "express", "Yo, {player}!\nKrazzle here! Your {count} x {item} got launched straight into your mailbox by my new Instant Delivery Cannon! Okay, a mailman carried them. But the cannon was VERY close.\nNo explosions this time. I am slightly disappointed. You should be thrilled.\nKeep it loud,\n- Krazzle Boomgear, Dispatch Boss of the Goblin Trading Post" },
        { "goblin", "express", "Hey hey!\nYou wanted it fast, you got it FASTER. {count} pieces of {item}, out the door before the ink on your order dried. I even singed my eyebrows a little in the rush. Worth it.\nIf the box smells a tiny bit like gunpowder, that is just the smell of quality.\nBoom and goodbye,\n- Krazzle Boomgear, Dispatch Boss of the Goblin Trading Post" },
        { "goblin", "express", "{player}!\nDone! Shipped! Delivered! Your {count} x {item} are in the mail already, faster than a rocket-powered gnome. And I have SEEN a rocket-powered gnome. Long story. Lots of screaming.\nAnyway, everything arrived in one piece. Unlike the gnome's hat.\nStay explosive,\n- Krazzle Boomgear, Dispatch Boss of the Goblin Trading Post" },
        { "goblin", "caravan", "Yo, {player}!\nThe caravan is back! It is on fire a little, but it is back! The fire is unrelated to your goods. Probably a fuse thing.\nYour {count} x {item} came through perfect, complete, not even warm. I packed them far away from the dynamite, as instructed by everyone, repeatedly.\nKeep it loud,\n- Krazzle Boomgear, Dispatch Boss of the Goblin Trading Post" },
        { "goblin", "caravan", "Hey hey!\nWe had to cross a canyon. The bridge was out. So I built a ramp. You can guess the rest. Wheee!\nThe wagons landed, mostly. And your {count} pieces of {item}? Every single one arrived with the caravan safe and sound. My hat did not. Goodbye, hat.\nBoom and goodbye,\n- Krazzle Boomgear, Dispatch Boss of the Goblin Trading Post" },
        { "goblin", "caravan", "{player}!\nCaravan report: one stubborn kodo, two bandits, three small explosions. The explosions solved everything, actually. The bandits ran, and the kodo started walking, mostly from surprise.\nYour {count} x {item} arrived complete and unblown-up, which I consider a professional triumph.\nNext time I bring a bigger fuse.\n- Krazzle Boomgear, Dispatch Boss of the Goblin Trading Post" },
        { "goblin", "express", "Dear {player},\nPer your order, please find enclosed {count} x {item}, delivered at once. This letter constitutes proof of delivery, receipt of goods and acceptance of the terms nobody reads.\nYou did not read them. Nobody does. Do not worry, they are mostly harmless.\nContractually yours,\n- Nixa Fastfingers, Contracts Clerk of the Goblin Trading Post" },
        { "goblin", "express", "Greetings, customer!\nYour {count} pieces of {item} are now in your possession, effective immediately, hereinafter referred to as The Goods.\nThe Goods may not be returned, refunded, or used to bribe Trading Post staff. Except me. Small bribes welcome.\nSee fine print. There is a lot of fine print.\n- Nixa Fastfingers, Contracts Clerk of the Goblin Trading Post" },
        { "goblin", "express", "{player},\nSigned, sealed, delivered at once: {count} x {item}.\nClause seven states that goods arriving faster than expected count as a gift of excellent service. Clause eight states that gifts must be remembered fondly. Clause nine states you should tell your friends.\nBinding regards,\n- Nixa Fastfingers, Contracts Clerk of the Goblin Trading Post" },
        { "goblin", "caravan", "Dear {player},\nAs per the caravan clause of your agreement, your {count} x {item} have arrived with the caravan, complete and undamaged.\nThe agreement also covered delays due to weather, bandits, and acts of kodo. All three occurred. All three were contractually anticipated. I am very good at my job.\nContractually yours,\n- Nixa Fastfingers, Contracts Clerk of the Goblin Trading Post" },
        { "goblin", "caravan", "Greetings, customer!\nThe caravan has arrived. Your {count} pieces of {item} came with it, all present and correct, as guaranteed in paragraph twelve.\nParagraph thirteen covers wheel loss on mountain roads. We used paragraph thirteen. Paragraph fourteen covers singing drivers. We used that too. Loudly.\nBinding regards,\n- Nixa Fastfingers, Contracts Clerk of the Goblin Trading Post" },
        { "goblin", "caravan", "{player},\nNotice of delivery: {count} x {item}, transported by caravan, received complete.\nBandits attempted to seize the shipment. I presented them with a cease and desist notice. They did not read it either, but the guards behind me were very persuasive.\nAll terms fulfilled. Please sign nowhere. I did it already.\n- Nixa Fastfingers, Contracts Clerk of the Goblin Trading Post" },
        };

        /// The pieces of the daily flyer, by writer - every flyer is written by one clerk from start to end - and
        /// what the auctioneer says when it hands over the free sample of the week.
        struct Piece
        {
            char const* house;      // horde, alliance, goblin
            char const* writer;     // a clerk; "auctioneer" for the handover
            char const* part;       // subject, headline, intro, spent, nothing, sample, listhead, closing, signature, ps, handover
            char const* text;       // {player}, {gold}, {item}
        };

        Piece const Flyer[] =
        {
        { "horde", "grukka", "subject", "Supplies. Expensive ones." },
        { "horde", "grukka", "subject", "Quartermaster's orders: buy" },
        { "horde", "grukka", "subject", "Grukka has goods. You have gold." },
        { "horde", "grukka", "subject", "Strong goods for strong coin" },
        { "horde", "grukka", "headline", "THE AUCTION HOUSE IS EMPTY. GRUKKA IS NOT." },
        { "horde", "grukka", "headline", "STRENGTH NEEDS STEEL. STEEL NEEDS GOLD. LOTS OF GOLD." },
        { "horde", "grukka", "headline", "NO HAGGLING. NO WHINING. ONLY BUYING." },
        { "horde", "grukka", "headline", "FRESH SUPPLIES FOR THE HORDE - PRICED FOR WARCHIEFS" },
        { "horde", "grukka", "intro", "{player}. Auction house ran dry again. Weak. My stores are full. My prices are high. Higher than a proto-drake's nest. You will pay them. A warrior does not count coins in battle." },
        { "horde", "grukka", "intro", "Listen, grunt. The auctioneers have nothing. I have ore, herbs, cloth, hide. Everything you need to fight. Nothing comes cheap at my counter. That is honest. Orcs are honest." },
        { "horde", "grukka", "intro", "{player}, you want to raid? You need supplies. You want supplies? You come to Grukka. You want cheap supplies? Go cry to the Alliance. Here we pay in gold and pride." },
        { "horde", "grukka", "intro", "The Horde marches on its stomach and its armor. Both run on my supplies. My prices bite like a worg. Accept the bite. It makes you stronger. Or poorer. Same thing." },
        { "horde", "grukka", "intro", "Grukka speaks plain. Goods are good. Prices are bad. Very bad. Proudly bad. But the auction house is empty, and my shelves are not. Choose wisely, {player}. Choose Grukka." },
        { "horde", "grukka", "spent", "Yesterday the warriors of the Horde left {gold} on my counter. Lok'tar! Grukka bought a bigger axe. And a second axe for the first axe." },
        { "horde", "grukka", "spent", "{gold}. That is what you fools left here yesterday. Grukka respects you. Grukka also laughed. Loudly." },
        { "horde", "grukka", "spent", "Yesterday's tribute to the Quartermaster: {gold}. Brave spending. Reckless spending. Grukka approves of both." },
        { "horde", "grukka", "spent", "The Horde gave {gold} to this post yesterday. Your purses are lighter. Your hearts should be proud. Mine is." },
        { "horde", "grukka", "nothing", "Yesterday nobody bought. Nothing. Grukka sat at the counter and sharpened his axe. All day. Think about that." },
        { "horde", "grukka", "nothing", "No coin crossed my counter yesterday. The Horde got stingy? Shameful. Even kobolds pay for candles." },
        { "horde", "grukka", "listhead", "Today's stores. Look. Then buy." },
        { "horde", "grukka", "listhead", "What Grukka has today:" },
        { "horde", "grukka", "listhead", "Supplies for the strong:" },
        { "horde", "grukka", "closing", "Stock is limited. When it is gone, it is gone. Talk to any auctioneer and choose the Trading Post. Pay full price, goods are in your mailbox at once. Bid less, wait for the caravan." },
        { "horde", "grukka", "closing", "Grukka does not restock for cowards who wait. While stocks last. Buy outright and the mail brings it now. Bid cheap and the caravan brings it later. Maybe." },
        { "horde", "grukka", "closing", "Go to any auctioneer. Pick the Trading Post. Pay. Buyout means your mailbox fills at once. A bid means the caravan walks it over. Slowly. Like a tauren after dinner." },
        { "horde", "grukka", "closing", "Few goods, many fools. Be the first fool. Any auctioneer will open the Trading Post for you. Buy outright for instant mail, or bid and wait for the caravan." },
        { "horde", "grukka", "closing", "Today's stock is small. Tomorrow's may be smaller. Strike now. Buyout: the goods arrive in your mailbox at once. Bid: the caravan comes when it comes." },
        { "horde", "grukka", "signature", "Strength and honor (and fair-ish prices),\n- Grukka Ironjaw, Quartermaster of the Horde Trading Post" },
        { "horde", "grukka", "signature", "Lok'tar ogar. Now pay.\n- Grukka Ironjaw, Quartermaster of the Horde Trading Post" },
        { "horde", "grukka", "ps", "P.S. Grukka does not do refunds. Grukka does axes." },
        { "horde", "grukka", "ps", "P.S. If you think my prices are too high, you are correct. Buy anyway." },
        { "horde", "grukka", "ps", "P.S. The last grunt who haggled now guards the latrines in Grom'gol." },
        { "horde", "grukka", "ps", "P.S. Gold spent here is gold spent for the Horde. Mostly for Grukka. Grukka is Horde." },
        { "horde", "zuljabi", "subject", "Zul'jabi got de goods, mon!" },
        { "horde", "zuljabi", "subject", "De caravan be here, mon" },
        { "horde", "zuljabi", "subject", "Fresh stock, fresh prices, mon" },
        { "horde", "zuljabi", "subject", "Ya wallet be callin' Zul'jabi" },
        { "horde", "zuljabi", "headline", "DE AUCTION HOUSE BE EMPTY, MON - BUT ZUL'JABI BE FULL!" },
        { "horde", "zuljabi", "headline", "EVERYTING YA NEED, AT PRICES DAT MAKE DE SPIRITS BLUSH!" },
        { "horde", "zuljabi", "headline", "STAY AWHILE AND SPEND, MON!" },
        { "horde", "zuljabi", "headline", "DE CARAVAN BE LOADED AND DE PRICES BE CRAZY, MON!" },
        { "horde", "zuljabi", "intro", "Hey hey, {player}! Zul'jabi here, Caravan Master and ya new best friend. De auction house be all out, but my caravan be packed wit ore and herbs and cloth. Prices? Ohhh, dey be big, mon. Real big." },
        { "horde", "zuljabi", "intro", "Da loa smile on ya today, mon! Zul'jabi got everyting de auctioneers ran out of. Is it expensive? Yes, mon. Very. Is it worth it? Ya gonna find out!" },
        { "horde", "zuljabi", "intro", "{player}, my friend, my favorite customer, my future source of income! Zul'jabi be bringin' de good stuff straight from de jungle and beyond. De prices be steep like de Zul'Drak cliffs, mon." },
        { "horde", "zuljabi", "intro", "Ya ever see a troll smile dis big? Dat be because de auction house be empty and Zul'jabi be de only one wit goods. Supply and demand, mon. Mostly demand." },
        { "horde", "zuljabi", "intro", "Welcome, welcome! De caravan be rollin' in today wit herbs, potions, leather, everyting ya need. Zul'jabi charge a lot, but Zul'jabi smile a lot too. Fair trade, mon." },
        { "horde", "zuljabi", "spent", "Yesterday ya people left {gold} wit Zul'jabi, mon! Dat be enough to buy a new raptor. Zul'jabi named him after ya all." },
        { "horde", "zuljabi", "spent", "Ha! {gold} yesterday, mon! De caravan be heavy wit ya gold now. De kodos be complainin'. Zul'jabi not complainin'." },
        { "horde", "zuljabi", "spent", "De whole Horde hear dis: yesterday ya spent {gold} at de Trading Post! Zul'jabi do a little dance for every coin, mon. Long dance." },
        { "horde", "zuljabi", "spent", "Yesterday de customers drop {gold} on Zul'jabi's counter, mon. De loa be pleased. Zul'jabi be more pleased." },
        { "horde", "zuljabi", "nothing", "Yesterday nobody buy nuttin', mon. Zul'jabi sat dere talkin' to de kodo. De kodo not buy nuttin' either." },
        { "horde", "zuljabi", "nothing", "No gold yesterday, mon. Not one copper. Zul'jabi tink ya all be hidin' ya purses under de bed. Zul'jabi find dem." },
        { "horde", "zuljabi", "listhead", "Look what de caravan brought, mon:" },
        { "horde", "zuljabi", "listhead", "Today's goodies from Zul'jabi:" },
        { "horde", "zuljabi", "listhead", "De stock, mon. Pick ya favorite:" },
        { "horde", "zuljabi", "closing", "Don't wait, mon! Stock be limited and de good stuff go fast. Talk to any auctioneer, choose de Trading Post. Buy outright and it be in ya mailbox right now. Bid and de caravan bring it later." },
        { "horde", "zuljabi", "closing", "While de stock last, mon! Pay de full price and de mail spirits deliver at once. Bid cheap and wait for Zul'jabi's caravan. De kodos be slow but dey get dere." },
        { "horde", "zuljabi", "closing", "Any auctioneer can open de Trading Post for ya, mon. Just ask. Buyout be instant, straight to ya mailbox. A bid mean ya wait for de caravan. Patience be a virtue. Gold be better." },
        { "horde", "zuljabi", "closing", "Only a little stock today, mon, so be quick! Buyout: de goods be in ya mailbox before ya finish ya drink. Bid: de caravan come rollin' later." },
        { "horde", "zuljabi", "closing", "Go see any auctioneer, mon, and pick de Trading Post. Full price get ya de goods at once in de mail. Cheap bid get ya de goods whenever de caravan feel like it. Dat be de deal." },
        { "horde", "zuljabi", "signature", "Stay frosty, mon, and keep ya purse open,\n- Zul'jabi, Caravan Master of the Horde Trading Post" },
        { "horde", "zuljabi", "signature", "De loa watch over ya (and ya gold),\n- Zul'jabi, Caravan Master of the Horde Trading Post" },
        { "horde", "zuljabi", "ps", "P.S. De kodo say hello. De kodo also say de prices be fair. De kodo be lyin', mon." },
        { "horde", "zuljabi", "ps", "P.S. Zul'jabi not responsible for any voodoo on de goods. Dat be extra." },
        { "horde", "zuljabi", "ps", "P.S. If ya bid too low, de caravan might take de scenic route, mon. Through Stranglethorn." },
        { "horde", "zuljabi", "ps", "P.S. Haggle wit Zul'jabi and Zul'jabi haggle back. Upward, mon." },
        { "horde", "thalsorn", "subject", "A gift of the Earth Mother (for gold)" },
        { "horde", "thalsorn", "subject", "The ledger speaks, friend" },
        { "horde", "thalsorn", "subject", "Gathered with care, priced without" },
        { "horde", "thalsorn", "subject", "Fresh from the plains of Mulgore" },
        { "horde", "thalsorn", "headline", "WHERE THE AUCTION HOUSE RUNS DRY, THE TRADING POST FLOWS" },
        { "horde", "thalsorn", "headline", "THE EARTH MOTHER PROVIDES - THE LEDGER-KEEPER CHARGES" },
        { "horde", "thalsorn", "headline", "GOODS AS PLENTIFUL AS THE GRASS, PRICES AS HIGH AS THUNDER BLUFF" },
        { "horde", "thalsorn", "headline", "WALK WITH THE WIND - TOWARD OUR COUNTER" },
        { "horde", "thalsorn", "intro", "Greetings, {player}. The winds tell me the auction house stands empty, like a dry riverbed. Our stores are full. The Earth Mother gave us these goods freely. We do not pass them on freely." },
        { "horde", "thalsorn", "intro", "Peace be upon you, traveler. I keep the ledger of the Horde Trading Post, and the ledger is patient. Like the mountain, our prices do not move. Like the mountain, they are very high." },
        { "horde", "thalsorn", "intro", "{player}, the herds grow fat and the herbs grow tall, yet the auction house has nothing. Here you will find what you seek. The price is steep, as is the path to the high mesa." },
        { "horde", "thalsorn", "intro", "The Earth Mother teaches balance. You have too much gold; we have too many goods. Let us restore the balance together, friend. The balance favors us, but such is nature." },
        { "horde", "thalsorn", "intro", "Sit, rest, listen to the drums. The Trading Post has gathered ore, hide, cloth and herbs while the auction house slept. The ledger records every coin you bring. It records many." },
        { "horde", "thalsorn", "spent", "Yesterday our honored customers left {gold} with the Trading Post. The ledger is heavy with gratitude. So is my coin pouch. The Earth Mother smiles." },
        { "horde", "thalsorn", "spent", "The ledger tells a proud tale: {gold} spent here yesterday. Like rain on dry earth, your gold has nourished us. Please rain again." },
        { "horde", "thalsorn", "spent", "Yesterday, {gold} flowed to our counter like a spring flood. The spirits marvel at your recklessness. So do I, quietly." },
        { "horde", "thalsorn", "spent", "Let it be known across the plains: {gold} was left here yesterday. Many purses walked away lighter, as a leaf walks away on the wind." },
        { "horde", "thalsorn", "nothing", "Yesterday, nothing. The ledger page stays empty, like a plain without buffalo. I sat and listened to the wind. The wind did not buy anything either." },
        { "horde", "thalsorn", "nothing", "No coin came yesterday. Perhaps the Horde is wise and thrifty. Or perhaps the Horde forgot us. The ledger forgives. The ledger also remembers." },
        { "horde", "thalsorn", "listhead", "What the land has given us today:" },
        { "horde", "thalsorn", "listhead", "The ledger lists today's goods:" },
        { "horde", "thalsorn", "listhead", "Gathered for you, priced for us:" },
        { "horde", "thalsorn", "closing", "The stores are not endless, friend. While stocks last, speak to any auctioneer and choose the Trading Post. Buy outright and the goods rest in your mailbox at once. Bid and the caravan brings them later." },
        { "horde", "thalsorn", "closing", "Like the seasons, our stock comes and goes. Buy it outright and it reaches your mailbox swiftly as an eagle. Bid, and it travels with the caravan, slow as a kodo." },
        { "horde", "thalsorn", "closing", "Any auctioneer will guide you to the Trading Post. There, the choice is yours: pay in full and receive your goods at once by mail, or bid less and await the caravan." },
        { "horde", "thalsorn", "closing", "Supplies are few and the hunters many. Do not wait for the next moon. A buyout reaches your mailbox at once; a bid follows the caravan's patient path." },
        { "horde", "thalsorn", "closing", "Walk to any auctioneer and ask for the Trading Post. Only so much stock can be gathered each day. Buy outright for the mailbox now, or bid for the caravan later." },
        { "horde", "thalsorn", "signature", "Walk with the Earth Mother (and with your purse open),\n- Thalsorn Mistwalker, Ledger-Keeper of the Horde Trading Post" },
        { "horde", "thalsorn", "signature", "May the winds guide you to our counter,\n- Thalsorn Mistwalker, Ledger-Keeper of the Horde Trading Post" },
        { "horde", "thalsorn", "ps", "P.S. The Earth Mother does not set our prices. If she did, they would be lower. Do not tell her." },
        { "horde", "thalsorn", "ps", "P.S. The ledger never lies. It only exaggerates our profits a little, out of joy." },
        { "horde", "thalsorn", "ps", "P.S. A wise elder once haggled with me. He is still waiting for a discount, high on the mesa." },
        { "horde", "thalsorn", "ps", "P.S. Tauren do not rush. But our stock does run out. Please rush." },
        { "horde", "ambrose", "subject", "Your doom, gift-wrapped" },
        { "horde", "ambrose", "subject", "Dead stock, living prices" },
        { "horde", "ambrose", "subject", "From the Undercity, with malice" },
        { "horde", "ambrose", "subject", "A letter from the grave (of shopping)" },
        { "horde", "ambrose", "headline", "THE AUCTION HOUSE IS DEAD. LONG LIVE THE TRADING POST." },
        { "horde", "ambrose", "headline", "PRICES TO DIE FOR - WE WOULD KNOW" },
        { "horde", "ambrose", "headline", "FRESH GOODS FROM THE UNDERCITY - SMELL NOT INCLUDED" },
        { "horde", "ambrose", "headline", "YOUR GOLD WILL NOT HELP YOU IN THE GRAVE. SPEND IT HERE." },
        { "horde", "ambrose", "intro", "Dear {player}, I write to you from the Shipping Office, which smells of formaldehyde and despair. The auction house is empty. Ours is not. Our prices are high, but then, so is mortality." },
        { "horde", "ambrose", "intro", "Greetings, living customer. The auction house has run out of everything, much like my pulse. Fortunately, the Trading Post has stock aplenty, priced with the cold indifference of the grave." },
        { "horde", "ambrose", "intro", "{player}, life is short. Mine was shorter. Do not waste yours bidding on empty auctions. The Trading Post has what you need, at prices that would make a ghoul gasp, if ghouls breathed." },
        { "horde", "ambrose", "intro", "I have shipped many things in my afterlife: plague barrels, coffins, my own left arm once. Today I ship supplies. They cost a fortune. Everything does, eventually. Ask the Lich King." },
        { "horde", "ambrose", "intro", "The dead have no use for gold. The living, it seems, also have no use for it, judging by how freely they spend it here. Join them, {player}. Our shelves are full and our prices are grave." },
        { "horde", "ambrose", "spent", "Yesterday our customers buried {gold} in this counter. A lovely funeral. I wept, figuratively. My tear ducts rotted off years ago." },
        { "horde", "ambrose", "spent", "{gold} left at the Trading Post yesterday. The Banshee herself could not have shrieked louder than your purses did." },
        { "horde", "ambrose", "spent", "Yesterday's toll: {gold}, freely surrendered. I have seen the Scourge take less from a village. Well done, all of you." },
        { "horde", "ambrose", "spent", "In memoriam: {gold}, spent here yesterday, never to return. It died doing what it loved, which was leaving your pockets." },
        { "horde", "ambrose", "nothing", "Nobody bought anything yesterday. The counter was as quiet as a crypt. I found it rather homely, but my superiors did not." },
        { "horde", "ambrose", "nothing", "Yesterday not a single copper. I assume you are all dead. If so, welcome. If not, how disappointing." },
        { "horde", "ambrose", "listhead", "Today's shipment, freshly exhumed:" },
        { "horde", "ambrose", "listhead", "Goods awaiting a new owner:" },
        { "horde", "ambrose", "listhead", "The manifest, read aloud by the dead:" },
        { "horde", "ambrose", "closing", "Our stock is as limited as your lifespan. Talk to any auctioneer and choose the Trading Post. Pay in full and your mailbox receives it at once. Bid, and the caravan delivers later, if it survives." },
        { "horde", "ambrose", "closing", "While stocks last, which will not be long. Buy outright and the mail bats bring it at once. Bid, and you wait for the caravan, as I wait for a second death." },
        { "horde", "ambrose", "closing", "Any auctioneer can introduce you to the Trading Post. They are alive, so be patient with them. Buyout: instant mail. Bid: the caravan, eventually." },
        { "horde", "ambrose", "closing", "Like all things, our supplies will end. Buy outright and they are in your mailbox immediately. Place a cheaper bid and they travel with the caravan, at the pace of a shambling ghoul." },
        { "horde", "ambrose", "closing", "Seek out any auctioneer and choose the Trading Post. Stock is scarce. Pay the full price for instant delivery to your mailbox, or bid and await the caravan with grim patience." },
        { "horde", "ambrose", "signature", "Yours in eternal undeath (and commerce),\n- Ambrose Hollowmere, Shipping Clerk of the Horde Trading Post" },
        { "horde", "ambrose", "signature", "Victory for the Forsaken, profit for the Post,\n- Ambrose Hollowmere, Shipping Clerk of the Horde Trading Post" },
        { "horde", "ambrose", "ps", "P.S. All shipments are packed in genuine coffins. The coffins are not for sale. Usually." },
        { "horde", "ambrose", "ps", "P.S. Complaints may be addressed to the Shipping Office. I will read them aloud at my next funeral." },
        { "horde", "ambrose", "ps", "P.S. If a package moans, do not open it. That is the courier." },
        { "horde", "ambrose", "ps", "P.S. You cannot take it with you. I tried. I can, however, take it from you." },
        { "alliance", "percival", "subject", "An invitation from the Steward" },
        { "alliance", "percival", "subject", "Fine goods for discerning patrons" },
        { "alliance", "percival", "subject", "Your presence (and purse) requested" },
        { "alliance", "percival", "subject", "Noble wares, noble prices" },
        { "alliance", "percival", "headline", "THE AUCTION HOUSE IS BARE - THE STEWARD'S PANTRY IS NOT" },
        { "alliance", "percival", "headline", "GOODS BEFITTING A LORD, PRICED BEFITTING A KING" },
        { "alliance", "percival", "headline", "HIS LORDSHIP'S STORES ARE OPEN - BRIEFLY AND EXPENSIVELY" },
        { "alliance", "percival", "headline", "QUALITY HAS A PRICE. OURS IS SPECTACULAR." },
        { "alliance", "percival", "intro", "My dear {player}, it is with the utmost distinction that I, Steward of this most noble Trading Post, inform you that the auction house stands empty, whereas our stores overflow. The prices are, naturally, exalted." },
        { "alliance", "percival", "intro", "Esteemed patron, a gentleman never speaks of money. I, however, am a steward, and must speak of it constantly. Our goods are superb. Our prices are a matter of some pride in the household." },
        { "alliance", "percival", "intro", "Good day, {player}. While the common auction house has exhausted its humble wares, the Trading Post offers ore, cloth, leather and potions of the finest provenance, at prices only the well-bred can admire." },
        { "alliance", "percival", "intro", "It is my solemn duty to announce that supplies have arrived. One does not ask the price of such goods. One simply pays it, with a gracious nod, as befits one's station." },
        { "alliance", "percival", "intro", "Distinguished adventurer, I have served noble houses since before the Third War, and never have I seen prices so magnificently inflated. I am, if I may say so, rather proud." },
        { "alliance", "percival", "spent", "I am delighted to report that yesterday our patrons graciously bestowed {gold} upon the Trading Post. The household has commissioned a portrait in their honor." },
        { "alliance", "percival", "spent", "Yesterday, {gold} was left at this counter by the finest families of the Alliance. Such largesse! Such abandon! Such questionable judgment!" },
        { "alliance", "percival", "spent", "The Steward notes, with considerable satisfaction, that {gold} changed hands here yesterday. The silver has been polished and the gold, of course, counted twice." },
        { "alliance", "percival", "spent", "Let the heralds proclaim it: {gold} spent at the Trading Post yesterday. Truly, the nobility of a patron is measured by the lightness of his purse." },
        { "alliance", "percival", "nothing", "Yesterday not a single patron purchased anything. The Steward is not offended. The Steward is merely wounded, deeply, in his dignity." },
        { "alliance", "percival", "nothing", "One regrets to report that yesterday's ledger is empty. Has the Alliance fallen upon hard times, or merely upon poor manners?" },
        { "alliance", "percival", "listhead", "Today's offerings, for your perusal:" },
        { "alliance", "percival", "listhead", "The household presents:" },
        { "alliance", "percival", "listhead", "Wares of distinction, available today:" },
        { "alliance", "percival", "closing", "Our stores, alas, are not infinite. Kindly speak with any auctioneer and request the Trading Post. A buyout places your goods in your mailbox at once; a modest bid sends them along with the caravan." },
        { "alliance", "percival", "closing", "While stocks last, as the vulgar say. Should you purchase outright, your goods arrive by post forthwith. Should you bid, they shall follow by caravan, at a pace befitting their dignity." },
        { "alliance", "percival", "closing", "Any auctioneer will gladly conduct you to the Trading Post. Pay the full sum and a courier delivers to your mailbox immediately. Bid, and the caravan shall attend you in due course." },
        { "alliance", "percival", "closing", "Supplies are limited, and the best families buy early. Purchase outright for prompt delivery to your mailbox, or place a bid and await our caravan with patrician patience." },
        { "alliance", "percival", "closing", "Pray, address any auctioneer and choose the Trading Post. The stock is small and the demand unseemly. A buyout is delivered at once; a bid travels with the caravan." },
        { "alliance", "percival", "signature", "With the utmost respect (and an invoice),\n- Percival Ashcombe, Steward of the Alliance Trading Post" },
        { "alliance", "percival", "signature", "For the Alliance, and for the household accounts,\n- Percival Ashcombe, Steward of the Alliance Trading Post" },
        { "alliance", "percival", "ps", "P.S. Haggling is considered most unbecoming in polite society. We do not permit it." },
        { "alliance", "percival", "ps", "P.S. Should you find our prices excessive, the Steward suggests marrying into money." },
        { "alliance", "percival", "ps", "P.S. Peasants may also shop here. Their gold, I am told, spends exactly the same." },
        { "alliance", "percival", "ps", "P.S. His Lordship's yacht thanks you for your continued patronage." },
        { "alliance", "tibbly", "subject", "Ledger update: you owe us a visit" },
        { "alliance", "tibbly", "subject", "Fresh stock, freshly tallied" },
        { "alliance", "tibbly", "subject", "Tibbly has counted everything!" },
        { "alliance", "tibbly", "subject", "Please balance my books (buy stuff)" },
        { "alliance", "tibbly", "headline", "EVERY ITEM COUNTED, EVERY PRICE CALCULATED, EVERY MARGIN OUTRAGEOUS!" },
        { "alliance", "tibbly", "headline", "THE AUCTION HOUSE IS EMPTY - I CHECKED IT SEVERAL TIMES!" },
        { "alliance", "tibbly", "headline", "PRECISELY ENGINEERED PRICES FOR IMPRECISELY THRIFTY HEROES" },
        { "alliance", "tibbly", "headline", "THE BOOKS DO NOT BALANCE THEMSELVES - BUY SOMETHING!" },
        { "alliance", "tibbly", "intro", "Hello, {player}! Tibbly here, Bookkeeper. I have counted the auction house stock and arrived at a total of none whatsoever. Meanwhile our stock is a large, lovely, carefully tallied pile. Prices are, ahem, optimized." },
        { "alliance", "tibbly", "intro", "Dear customer, I have recalculated our prices many, many times, and each time they came out higher. That is not an error. That is a feature. The ledgers adore it." },
        { "alliance", "tibbly", "intro", "Greetings, {player}! My columns are neat, my rows are tidy, and my prices are astronomical. The auction house has run dry, so you have exactly one option, and I have already written it down." },
        { "alliance", "tibbly", "intro", "Gnomish precision demands I inform you: our supplies are plentiful, our margins are enormous, and our auction house competitor has precisely nothing. I double-checked. Then I triple-checked. Then I had tea." },
        { "alliance", "tibbly", "intro", "Every ingot, every herb, every bolt of cloth has been entered, indexed and cross-referenced. The only empty column in my ledger is the one marked Your Purchases. Shall we fix that?" },
        { "alliance", "tibbly", "spent", "Oh joy! Yesterday our customers left {gold} at the counter! I have entered it in three separate ledgers, just to enjoy writing it again." },
        { "alliance", "tibbly", "spent", "Yesterday's revenue: {gold}! I counted it twice, then a third time, then hugged it. Very professional." },
        { "alliance", "tibbly", "spent", "The books show {gold} spent here yesterday. A beautiful figure. I may frame it. Thank you, reckless spenders, for this gift to accountancy!" },
        { "alliance", "tibbly", "spent", "Huzzah! {gold} recorded yesterday, neatly carried over into today's ledger. My abacus is overheating with happiness." },
        { "alliance", "tibbly", "nothing", "Yesterday's column: blank. Utterly, completely blank. I stared at it for hours. It did not fill itself." },
        { "alliance", "tibbly", "nothing", "Nobody bought anything yesterday. I have recounted the sales several times hoping for an error. There was no error. Only sadness." },
        { "alliance", "tibbly", "listhead", "Today's inventory, alphabetized-ish:" },
        { "alliance", "tibbly", "listhead", "Freshly tallied stock:" },
        { "alliance", "tibbly", "listhead", "Itemized for your convenience:" },
        { "alliance", "tibbly", "closing", "Stock is limited, and I know exactly how limited, because I counted it! Talk to any auctioneer and choose the Trading Post. Buyout means instant mail; a bid means waiting for the caravan." },
        { "alliance", "tibbly", "closing", "While stocks last, which by my calculations is not long at all. Pay in full and the item lands in your mailbox immediately. Bid less and it rides along with the caravan." },
        { "alliance", "tibbly", "closing", "Procedure is simple: approach any auctioneer, select the Trading Post, make your choice. Buyout: delivered to your mailbox at once. Bid: the caravan brings it later. Forms not required. Sadly." },
        { "alliance", "tibbly", "closing", "Our daily allotment is small and accurately recorded. Buy outright to receive goods by mail at once, or place a bid and wait for the caravan to trundle over." },
        { "alliance", "tibbly", "closing", "Any auctioneer can open the Trading Post for you. Supplies are limited. Instant delivery for buyouts, caravan delivery for bids. I have a chart about it, if anyone asks." },
        { "alliance", "tibbly", "signature", "Accurately and affectionately yours,\n- Tibbly Gearwhistle, Bookkeeper of the Alliance Trading Post" },
        { "alliance", "tibbly", "signature", "Balanced books and bright futures,\n- Tibbly Gearwhistle, Bookkeeper of the Alliance Trading Post" },
        { "alliance", "tibbly", "ps", "P.S. I have miscounted exactly never. Except that one time with the mechanostriders. We do not talk about it." },
        { "alliance", "tibbly", "ps", "P.S. Every purchase gives me a tiny thrill. Please thrill me." },
        { "alliance", "tibbly", "ps", "P.S. If you find a cheaper price anywhere, I will be very surprised and recalculate everything." },
        { "alliance", "tibbly", "ps", "P.S. Gnomeregan was lost to radiation. Our prices were lost to ambition." },
        { "alliance", "durgan", "subject", "The caravan made it. Barely." },
        { "alliance", "durgan", "subject", "Goods, ale, and outrageous prices" },
        { "alliance", "durgan", "subject", "Durgan's wagon is back in town" },
        { "alliance", "durgan", "subject", "Hauled it through snow. Pay up." },
        { "alliance", "durgan", "headline", "HAULED THROUGH SNOW AND MUD - PRICED ACCORDINGLY!" },
        { "alliance", "durgan", "headline", "THE AUCTION HOUSE IS EMPTY - MY WAGON IS FULL!" },
        { "alliance", "durgan", "headline", "FINE GOODS FROM IRONFORGE AND BEYOND - NO CHEAP ALE MONEY HERE" },
        { "alliance", "durgan", "headline", "BY MY BEARD, THESE PRICES ARE HIGH - AND WORTH EVERY BUMP!" },
        { "alliance", "durgan", "intro", "Oi, {player}! Durgan here. Dragged this wagon over every rotten road from Dun Morogh to Dragonblight, and my back is killing me. The auction house is empty. My wagon is not. Prices include my suffering." },
        { "alliance", "durgan", "intro", "Ach, the roads in Northrend! Ice, wolves, ghouls, and a bridge that was there last week. Still, the caravan arrived with ore, herbs and leather. You pay for the goods, the journey and my ale." },
        { "alliance", "durgan", "intro", "Listen here, {player}. Those auctioneers ran out of everything. Typical. Good thing old Durgan keeps a full wagon. Costs a fortune, aye, but so does a decent stout these days." },
        { "alliance", "durgan", "intro", "Fresh supplies, lad and lass! Every crate hauled by hand, every barrel guarded with my life. Prices are steep as the Ironforge stairs, and twice as hard on the knees." },
        { "alliance", "durgan", "intro", "I won't lie to you, friend: the prices are a disgrace. A glorious, well-earned disgrace. The roads were terrible and the tavern bills worse. Buy something and help an old dwarf recover." },
        { "alliance", "durgan", "spent", "Yesterday you lot left {gold} at my wagon! Every coin went straight into a barrel of the finest. Cheers to you, you reckless spendthrifts!" },
        { "alliance", "durgan", "spent", "{gold} spent yesterday! Ha! That buys a lot of ale, and a new wheel, and maybe a few more ales." },
        { "alliance", "durgan", "spent", "Durgan salutes his customers: yesterday you dropped {gold} like hot coals. The wagon thanks you. The tavern thanks you more." },
        { "alliance", "durgan", "spent", "By my beard! {gold} left here yesterday. I have not seen such generosity since the Thandol Span was still standing." },
        { "alliance", "durgan", "nothing", "Yesterday not one sale. Not one! I hauled this wagon through a blizzard for nothing? Durgan is drinking alone and grumbling." },
        { "alliance", "durgan", "nothing", "Nobody bought a thing yesterday. Stingier than a Dark Iron at a tax collector's door, the lot of you." },
        { "alliance", "durgan", "listhead", "What made it through the snow:" },
        { "alliance", "durgan", "listhead", "Unloaded fresh from the wagon:" },
        { "alliance", "durgan", "listhead", "Today's haul, lads and lasses:" },
        { "alliance", "durgan", "closing", "Only so much fits on one wagon, so don't dawdle. Talk to any auctioneer and choose the Trading Post. Buy outright and it is in your mailbox at once. Bid and wait for the next caravan." },
        { "alliance", "durgan", "closing", "While stocks last! Pay full price and the goods go straight to your mailbox. Bid cheap and they ride with me on the caravan. Mind, the roads are bad." },
        { "alliance", "durgan", "closing", "Any auctioneer can point you to the Trading Post. Buyout gets you instant mail. A bid gets you a seat in the caravan queue, behind three barrels of stout." },
        { "alliance", "durgan", "closing", "Limited stock, lads. When the wagon's empty, it's empty. Buy outright for instant mailbox delivery, or bid and wait for old Durgan to haul it over." },
        { "alliance", "durgan", "closing", "Find any auctioneer, ask for the Trading Post, and choose. Full price means your mailbox fills at once. A bid means the caravan brings it when the roads allow." },
        { "alliance", "durgan", "signature", "Bottoms up and purses open,\n- Durgan Stoutkeg, Caravan Master of the Alliance Trading Post" },
        { "alliance", "durgan", "signature", "May your mug never empty (unlike our shelves),\n- Durgan Stoutkeg, Caravan Master of the Alliance Trading Post" },
        { "alliance", "durgan", "ps", "P.S. If your caravan delivery is late, blame the roads. Never the ale." },
        { "alliance", "durgan", "ps", "P.S. Lost a wheel near Wintergarde. Prices went up to pay for it. Prices went up a lot." },
        { "alliance", "durgan", "ps", "P.S. No, I will not trade goods for ale. Unless it is very good ale." },
        { "alliance", "durgan", "ps", "P.S. Durgan does not haggle. Durgan only grumbles, and charges extra for it." },
        { "alliance", "ishaali", "subject", "Blessings from the Shipping Office" },
        { "alliance", "ishaali", "subject", "The Light provides (for a fee)" },
        { "alliance", "ishaali", "subject", "A message carried by the Naaru" },
        { "alliance", "ishaali", "subject", "Holy supplies, unholy prices" },
        { "alliance", "ishaali", "headline", "THE LIGHT HAS GUIDED NEW SUPPLIES TO OUR COUNTER" },
        { "alliance", "ishaali", "headline", "WHERE THE AUCTION HOUSE FALTERS, FAITH AND STOCK ENDURE" },
        { "alliance", "ishaali", "headline", "BLESSED GOODS - PRICED FOR THE TRULY DEVOTED" },
        { "alliance", "ishaali", "headline", "THE NAARU WATCH OVER OUR WARES - AND OUR PROFITS" },
        { "alliance", "ishaali", "intro", "Blessings, {player}. The Light has seen fit to empty the auction house and fill our stores. Surely this is a sign. Our prices are high, but so is the heavenly vault above Shattrath." },
        { "alliance", "ishaali", "intro", "Peace be with you, friend. In this dark age of the Scourge, the Trading Post brings ore, herbs, cloth and potions to the faithful. The cost is great, as all true devotion is." },
        { "alliance", "ishaali", "intro", "Dear {player}, I have prayed over every crate in the Shipping Office. The Naaru answered with one word: markup. Who am I to question the Naaru?" },
        { "alliance", "ishaali", "intro", "The Exodar crossed the stars to reach this world. Our caravans cross far less, but charge far more. Such is the mystery of the Light, which I do not pretend to understand." },
        { "alliance", "ishaali", "intro", "Greetings, seeker. Be at peace: the auction house may be barren, but the Light shines upon our shelves. Our prices test your faith. Pass the test, friend. Pay the price." },
        { "alliance", "ishaali", "spent", "Yesterday the faithful offered {gold} at the Trading Post. The Light rejoices, and the treasury hums with holy radiance." },
        { "alliance", "ishaali", "spent", "Praise be! {gold} was given here yesterday. Such devotion! Such sacrifice! Such breathtaking disregard for savings!" },
        { "alliance", "ishaali", "spent", "The Naaru have counted it and found it good: {gold} spent here yesterday. May your purses be refilled by the Light, so you may empty them again." },
        { "alliance", "ishaali", "spent", "Yesterday, {gold} flowed to our counter like light through a prism. The Shipping Office sings hymns in your honor." },
        { "alliance", "ishaali", "nothing", "Yesterday no one purchased anything. I meditated upon this silence and found only one lesson: the Alliance has become thrifty. A grievous trial." },
        { "alliance", "ishaali", "nothing", "Not a single offering yesterday. The Light is patient. I am patient. The ledger, regrettably, is not." },
        { "alliance", "ishaali", "listhead", "Today's blessed offerings:" },
        { "alliance", "ishaali", "listhead", "The Light has provided these:" },
        { "alliance", "ishaali", "listhead", "Wares from the Shipping Office:" },
        { "alliance", "ishaali", "closing", "Even the Light cannot multiply our crates. Stock is limited. Speak with any auctioneer and choose the Trading Post. Buy outright and it reaches your mailbox at once; bid and the caravan will bring it." },
        { "alliance", "ishaali", "closing", "While stocks last, friend. A buyout sends your goods to your mailbox immediately, swift as a prayer. A bid travels more humbly, with the caravan." },
        { "alliance", "ishaali", "closing", "Any auctioneer will lead you to the Trading Post, as a beacon leads the lost. Pay in full for immediate delivery by mail, or bid and await the caravan in patience." },
        { "alliance", "ishaali", "closing", "Our supply is a small flame in a great darkness. Do not let it go out unused. Buy outright for instant mailbox delivery, or bid and let the caravan bring it in time." },
        { "alliance", "ishaali", "closing", "Seek any auctioneer and ask for the Trading Post. The stock is limited and blessed. A buyout arrives at once in your mailbox; a bid follows with the caravan." },
        { "alliance", "ishaali", "signature", "May the Light embrace you (and your purse),\n- Ishaali, Shipping Clerk of the Alliance Trading Post" },
        { "alliance", "ishaali", "signature", "In the name of the Naaru, and of the ledger,\n- Ishaali, Shipping Clerk of the Alliance Trading Post" },
        { "alliance", "ishaali", "ps", "P.S. Prayers for a discount will be heard. They will not be answered." },
        { "alliance", "ishaali", "ps", "P.S. The Light forgives all sins. The Trading Post forgives no debts." },
        { "alliance", "ishaali", "ps", "P.S. If a crate glows, that is a blessing. If it hums, step back." },
        { "alliance", "ishaali", "ps", "P.S. Our seers once gazed upon our prices. They saw many futures. In none of them were the prices lower." },
        { "goblin", "gizzik", "subject", "Deal of the day! And the day after!" },
        { "goblin", "gizzik", "subject", "Gizzik has a deal for you, pal" },
        { "goblin", "gizzik", "subject", "Buy one, buy another one!" },
        { "goblin", "gizzik", "subject", "Limited offer! (They all are)" },
        { "goblin", "gizzik", "headline", "TIME IS MONEY, FRIEND - AND YOURS IS RUNNING OUT!" },
        { "goblin", "gizzik", "headline", "EVERYTHING MUST GO - INTO YOUR BAGS, AT FULL PRICE!" },
        { "goblin", "gizzik", "headline", "THE AUCTION HOUSE IS EMPTY - GIZZIK'S DEALS ARE NOT!" },
        { "goblin", "gizzik", "headline", "WHY BUY ONE WHEN YOU CAN BUY THEM ALL?" },
        { "goblin", "gizzik", "intro", "Hey hey, {player}! Gizzik Sharpcoin, Sales Manager, at your service! The auction house is dry as a Tanaris sandbar, but have I got deals for you! And if you like those deals, I got bigger deals!" },
        { "goblin", "gizzik", "intro", "Friend! Pal! Best customer! You need ore? Herbs? Cloth? Of course you do. And while you are at it, you need potions. And food. And scrolls. Trust me, you need everything." },
        { "goblin", "gizzik", "intro", "{player}, buddy, listen. The auction house is empty. Tragic. But Gizzik always has stock, and Gizzik always has a reason why you should buy twice as much as you planned." },
        { "goblin", "gizzik", "intro", "Time is money, and you are wasting both reading this slowly! Our goods are premium, our prices are premium premium, and our upgrade options are premium premium premium." },
        { "goblin", "gizzik", "intro", "Let me tell you a secret, pal: nobody ever regretted buying more. Well, some did. They are not customers anymore. Do not be like them. Be like a winner. Buy big, buy now!" },
        { "goblin", "gizzik", "spent", "Yesterday our customers left {gold} at the counter! Beautiful! But I know you can do better. Today, let us double it. Let us triple it!" },
        { "goblin", "gizzik", "spent", "{gold} spent yesterday! Fantastic! But was it everything you had? Because if not, Gizzik has some great news for you." },
        { "goblin", "gizzik", "spent", "Big shout-out to yesterday's heroes who dropped {gold} at the Trading Post! Gizzik bought a new hat. Gizzik wants a second hat." },
        { "goblin", "gizzik", "spent", "Yesterday, {gold} walked right into our vault. Whoever spent the most: you are a legend. Whoever spent the least: we need to talk." },
        { "goblin", "gizzik", "nothing", "Yesterday: zero sales. Zip. Nada. Gizzik had to sell his own lunch to himself. At full price. It was a terrible deal." },
        { "goblin", "gizzik", "nothing", "Nobody bought anything yesterday? Pal, that hurts. Gizzik is crying. Gizzik is also raising prices. Coincidence." },
        { "goblin", "gizzik", "listhead", "Today's hottest deals:" },
        { "goblin", "gizzik", "listhead", "Get 'em while they're overpriced:" },
        { "goblin", "gizzik", "listhead", "Gizzik recommends ALL of these:" },
        { "goblin", "gizzik", "closing", "Stock is limited, pal, and it is flying off the shelves! Talk to any auctioneer and choose the Trading Post. Buyout means instant mail. Bid means caravan. Smart buyers buy out. Smarter buyers buy everything." },
        { "goblin", "gizzik", "closing", "While stocks last! And they will not last, because Gizzik is that good. Buy outright and it is in your mailbox now. Bid and wait for the caravan. Why wait? Waiting is for losers." },
        { "goblin", "gizzik", "closing", "Any auctioneer can open the Trading Post for you. Pick, pay, profit! Buyout: goods in your mailbox at once. Bid: the caravan brings them later. Recommended: buyout. Twice." },
        { "goblin", "gizzik", "closing", "Only a few of each left! Act now! A buyout lands in your mailbox at once; a cheap bid rides the caravan. Gizzik suggests the buyout. Gizzik always suggests the buyout." },
        { "goblin", "gizzik", "closing", "Run to any auctioneer, say Trading Post, open your purse. The stock is tiny, the demand is huge. Pay in full for instant mail, or bid and wait for the caravan." },
        { "goblin", "gizzik", "signature", "Time is money, so spend yours here,\n- Gizzik Sharpcoin, Sales Manager of the Goblin Trading Post" },
        { "goblin", "gizzik", "signature", "Your pal in profit,\n- Gizzik Sharpcoin, Sales Manager of the Goblin Trading Post" },
        { "goblin", "gizzik", "ps", "P.S. Bought something yesterday? Great! Buy it again today. Variety is overrated." },
        { "goblin", "gizzik", "ps", "P.S. Ask about our premium package. It is the same package, but it costs more." },
        { "goblin", "gizzik", "ps", "P.S. Gizzik never takes no for an answer. Gizzik takes gold." },
        { "goblin", "gizzik", "ps", "P.S. Refer a friend and you both get the privilege of paying full price!" },
        { "goblin", "fenny", "subject", "A friendly reminder (fees apply)" },
        { "goblin", "fenny", "subject", "Your statement of possible charges" },
        { "goblin", "fenny", "subject", "Fresh stock! Surcharges included" },
        { "goblin", "fenny", "subject", "Notice of fees, sincerely yours" },
        { "goblin", "fenny", "headline", "NEW STOCK ARRIVED - THE ARRIVAL FEE IS ON YOU" },
        { "goblin", "fenny", "headline", "EVERY PURCHASE INCLUDES A FREE FEE!" },
        { "goblin", "fenny", "headline", "TRANSPARENT PRICING - WE SEE RIGHT THROUGH YOUR PURSE" },
        { "goblin", "fenny", "headline", "THE AUCTION HOUSE IS EMPTY - OUR FEE SCHEDULE IS NOT" },
        { "goblin", "fenny", "intro", "Greetings, {player}. Fenny Tollwhistle, Fee Accountant. I am pleased to report the auction house is empty and our shelves are full. Also, reading this letter incurs a reading fee. Just kidding. Mostly." },
        { "goblin", "fenny", "intro", "Dear valued customer, our prices are high, but please do not forget the handling fee, the counter fee, the smile fee and the fee for calculating fees. Everything is perfectly reasonable." },
        { "goblin", "fenny", "intro", "{player}, every good deal deserves a good surcharge. Our ore comes with an ore surcharge, our herbs with an herb surcharge, and our cloth with a cloth surcharge. Consistency matters." },
        { "goblin", "fenny", "intro", "Hello! As Fee Accountant, I ensure that every price is accompanied by its loyal companions: the fees. Exact amounts are calculated at the counter, by me, in private, with a smile." },
        { "goblin", "fenny", "intro", "The auction house has run out of supplies. The Trading Post has not, and never runs out of fees either. Come in, browse freely. Browsing is free. The door fee is not." },
        { "goblin", "fenny", "spent", "Yesterday our customers paid {gold} at this counter. Of course, I already know how much of that was fees. I will not tell you. That would cost extra." },
        { "goblin", "fenny", "spent", "Splendid! {gold} spent here yesterday. The fee department wept with joy, then charged itself a joy fee." },
        { "goblin", "fenny", "spent", "Yesterday's total: {gold}. A lovely sum, comprised of goods, surcharges, and the mysterious line items no one ever asks about." },
        { "goblin", "fenny", "spent", "We thank everyone who left {gold} with us yesterday. Your receipts are available upon request. The request fee is modest." },
        { "goblin", "fenny", "nothing", "Yesterday no one bought anything. There were no fees to collect. I sat in a dark room and calculated what might have been." },
        { "goblin", "fenny", "nothing", "No sales yesterday. Fenny has therefore introduced a no-sales fee. It applies to everyone who did not buy anything. That means you." },
        { "goblin", "fenny", "listhead", "Today's offers (fees not shown):" },
        { "goblin", "fenny", "listhead", "Base prices, before the fun begins:" },
        { "goblin", "fenny", "listhead", "Today's stock, surcharges pending:" },
        { "goblin", "fenny", "closing", "Stock is limited, fees are not. Talk to any auctioneer and choose the Trading Post. A buyout delivers to your mailbox at once. A bid waits for the caravan, at no extra fee. Probably." },
        { "goblin", "fenny", "closing", "While stocks last! Pay in full and your goods are mailed immediately, express fee included. Bid less and they arrive by caravan, with the waiting fee waived. Generous, I know." },
        { "goblin", "fenny", "closing", "Approach any auctioneer, select the Trading Post, and pay. Buyouts arrive by mail at once. Bids travel with the caravan. All fees are clearly explained in a document no one has ever found." },
        { "goblin", "fenny", "closing", "Limited stock, unlimited paperwork. Buy outright for instant mailbox delivery, or bid and wait for the caravan. Either way, Fenny will be there to tally." },
        { "goblin", "fenny", "closing", "Visit any auctioneer and ask for the Trading Post. Our stock is small and our fee schedule is long. Buy outright for immediate delivery by mail, or bid and let the caravan handle it." },
        { "goblin", "fenny", "signature", "Fees sincerely and sincerely fees,\n- Fenny Tollwhistle, Fee Accountant of the Goblin Trading Post" },
        { "goblin", "fenny", "signature", "Yours, subject to applicable charges,\n- Fenny Tollwhistle, Fee Accountant of the Goblin Trading Post" },
        { "goblin", "fenny", "ps", "P.S. This P.S. is free. The next one is not." },
        { "goblin", "fenny", "ps", "P.S. Complaints about fees may be submitted at the counter. A filing fee applies." },
        { "goblin", "fenny", "ps", "P.S. The fee for asking how much the fees are is, regrettably, a fee." },
        { "goblin", "fenny", "ps", "P.S. If you see a fee you do not understand, that is the understanding fee." },
        { "goblin", "krazzle", "subject", "BOOM! New stock just landed!" },
        { "goblin", "krazzle", "subject", "Explosive deals inside! Literally!" },
        { "goblin", "krazzle", "subject", "Krazzle's dispatch is blowing up" },
        { "goblin", "krazzle", "subject", "Hot goods! Do not shake the box!" },
        { "goblin", "krazzle", "headline", "KA-BOOM! FRESH SUPPLIES, FRESHLY SINGED!" },
        { "goblin", "krazzle", "headline", "PRICES SO HIGH THEY NEED A ROCKET TO REACH THEM!" },
        { "goblin", "krazzle", "headline", "THE AUCTION HOUSE FIZZLED - KRAZZLE DID NOT!" },
        { "goblin", "krazzle", "headline", "DELIVERED BY ROCKET, PRICED LIKE A ROCKET!" },
        { "goblin", "krazzle", "intro", "Hey {player}! Krazzle Boomgear here, Dispatch Boss! The auction house went fizzle, but our stock just went BOOM! Ore, herbs, cloth, potions - all delivered by rocket. Mostly intact. Prices sky-high!" },
        { "goblin", "krazzle", "intro", "Listen up! Dispatch blew up a whole warehouse this morning. On purpose? Let us say yes. Anyway, the surviving goods are now extremely rare, and the prices extremely explosive!" },
        { "goblin", "krazzle", "intro", "{player}, you like fireworks? You will love our prices. They go up, up, up, and then they keep going up! The auction house has nothing. We have everything, slightly smoking." },
        { "goblin", "krazzle", "intro", "Krazzle loves three things: dynamite, rockets, and customers who buy outright. Two out of three explode. The third just makes me rich. Come on down and light up my day!" },
        { "goblin", "krazzle", "intro", "Fresh stock just arrived at dispatch with a satisfying thud and only a small fire. The auction house is empty, so our prices have blasted off. Grab your goods before the next detonation!" },
        { "goblin", "krazzle", "spent", "BOOM! Yesterday our customers blew {gold} at the Trading Post! Krazzle celebrated with a crate of fireworks. The neighbors did not celebrate." },
        { "goblin", "krazzle", "spent", "{gold} spent here yesterday! That is enough for a lot of dynamite. Guess what Krazzle bought!" },
        { "goblin", "krazzle", "spent", "Yesterday, {gold} went off like a sapper charge right on our counter. Spectacular! Reckless! Krazzle salutes you from behind a blast shield!" },
        { "goblin", "krazzle", "spent", "Kaboom-a-rama! Yesterday you lit up the ledger with {gold}. Krazzle's eyebrows grew back just to raise themselves at you." },
        { "goblin", "krazzle", "nothing", "Yesterday nobody bought anything. Krazzle got so bored he blew up a mailbox. Not yours. Probably." },
        { "goblin", "krazzle", "nothing", "Zero sales yesterday? A total dud! Not even a fizzle! Krazzle is sulking next to a pile of unlit fuses." },
        { "goblin", "krazzle", "listhead", "Today's stock, still smoking:" },
        { "goblin", "krazzle", "listhead", "What survived the blast:" },
        { "goblin", "krazzle", "listhead", "Hot off the rocket pad:" },
        { "goblin", "krazzle", "closing", "Stock is limited - some of it exploded! Talk to any auctioneer and choose the Trading Post. Buy outright and a rocket drops it in your mailbox at once. Bid and it rolls in later with the caravan." },
        { "goblin", "krazzle", "closing", "While stocks last, which is until the next warehouse fire. Pay in full for instant mail delivery. Bid cheap and wait for the caravan, the slow and boring way." },
        { "goblin", "krazzle", "closing", "Any auctioneer can light up the Trading Post for you. Buyout: whoosh, straight into your mailbox. Bid: the caravan clip-clops over eventually. Krazzle prefers whoosh." },
        { "goblin", "krazzle", "closing", "Small stock, big bang! Buy outright and the goods hit your mailbox at once. Place a bid and they come with the caravan, nice and slow, nothing on fire." },
        { "goblin", "krazzle", "closing", "Head to any auctioneer and ask for the Trading Post. Only a little stock left, the rest is crater. Pay full price for instant mail, or bid and wait for the caravan." },
        { "goblin", "krazzle", "signature", "Stay explosive (and keep buying),\n- Krazzle Boomgear, Dispatch Boss of the Goblin Trading Post" },
        { "goblin", "krazzle", "signature", "Light it up and pay it out,\n- Krazzle Boomgear, Dispatch Boss of the Goblin Trading Post" },
        { "goblin", "krazzle", "ps", "P.S. If your package ticks, put it in the water. Then call us. From far away." },
        { "goblin", "krazzle", "ps", "P.S. Krazzle is not legally allowed near the auction house anymore. Long story. Big boom." },
        { "goblin", "krazzle", "ps", "P.S. Singed goods are a sign of freshness. Everyone knows that." },
        { "goblin", "krazzle", "ps", "P.S. Our rocket couriers have an excellent safety record. The record is on fire, but it is excellent." },
        { "goblin", "nixa", "subject", "Contract offer (please do not read)" },
        { "goblin", "nixa", "subject", "Terms and conditions apply. Shop!" },
        { "goblin", "nixa", "subject", "An offer, legally speaking" },
        { "goblin", "nixa", "subject", "Notice of fresh stock, binding" },
        { "goblin", "nixa", "headline", "FRESH STOCK AVAILABLE - SUBJECT TO TERMS AND CONDITIONS" },
        { "goblin", "nixa", "headline", "THE AUCTION HOUSE IS EMPTY - WE HAVE IT IN WRITING" },
        { "goblin", "nixa", "headline", "OUTRAGEOUS PRICES, FULLY LEGAL, LEGALLY OUTRAGEOUS" },
        { "goblin", "nixa", "headline", "SIGN HERE, HERE, AND HERE - THEN SHOP!" },
        { "goblin", "nixa", "intro", "Dear {player}, hereinafter referred to as the Customer: the Trading Post, hereinafter the Post, hereby informs you that the auction house is empty and the Post has goods, at prices the Customer has already agreed to." },
        { "goblin", "nixa", "intro", "Pursuant to the laws of the Steamwheedle Cartel, the Trading Post offers ore, herbs, cloth, potions and more. By reading this sentence you have accepted all applicable terms. Thank you." },
        { "goblin", "nixa", "intro", "{player}, I am Nixa Fastfingers, Contracts Clerk. Please note that our prices are high, our stock is limited, and our fine print is very fine. You may need a magnifying glass. Sold separately." },
        { "goblin", "nixa", "intro", "Notice is hereby given that the auction house has no stock and the Trading Post does. All prices are final, non-negotiable, and legally described as reasonable within the Post." },
        { "goblin", "nixa", "intro", "Greetings, valued party of the second part. The Trading Post, party of the first part, offers premium goods at premium prices. Any resemblance to fair pricing is purely coincidental." },
        { "goblin", "nixa", "spent", "For the record: yesterday customers voluntarily surrendered {gold} to the Trading Post. Voluntarily. It says so in the contract they did not read." },
        { "goblin", "nixa", "spent", "Let the minutes show that {gold} was spent here yesterday, irrevocably, as per clause something-or-other. Thank you, signatories!" },
        { "goblin", "nixa", "spent", "Yesterday, {gold} transferred legally into the possession of the Post. The Post accepts your gratitude in advance and waives nothing." },
        { "goblin", "nixa", "spent", "Public notice: customers left {gold} at this counter yesterday. Refund requests will be filed in the drawer we never open." },
        { "goblin", "nixa", "nothing", "Yesterday no sales were recorded. The Post reserves the right to be deeply offended and to note this in your permanent file." },
        { "goblin", "nixa", "nothing", "No purchases were made yesterday, in breach of the unwritten agreement between the Post and the Customer. Our lawyers are writing it now." },
        { "goblin", "nixa", "listhead", "Schedule of goods, annex one:" },
        { "goblin", "nixa", "listhead", "Today's offers, as contractually listed:" },
        { "goblin", "nixa", "listhead", "Goods available, terms attached:" },
        { "goblin", "nixa", "closing", "Stock is limited as defined in the stock limitation clause. Talk to any auctioneer and choose the Trading Post. A buyout is delivered to your mailbox at once; a bid shall be delivered by caravan." },
        { "goblin", "nixa", "closing", "Offer valid while stocks last. Outright purchase entitles the Customer to immediate mailbox delivery. A bid entitles the Customer to wait for the caravan, patiently and without complaint." },
        { "goblin", "nixa", "closing", "To proceed, the Customer shall address any auctioneer and select the Trading Post. Buyout results in instant mail delivery. Bids are fulfilled by caravan at a later date." },
        { "goblin", "nixa", "closing", "Quantities are limited and non-renewable for the day. Buy outright for prompt mailbox delivery, or place a bid and accept caravan delivery, timing at the Post's discretion." },
        { "goblin", "nixa", "closing", "Visit any auctioneer and request the Trading Post. Stock is limited. Buyouts are mailed at once. Bids travel with the caravan. Signing is optional. Paying is not." },
        { "goblin", "nixa", "signature", "Sincerely, without prejudice,\n- Nixa Fastfingers, Contracts Clerk of the Goblin Trading Post" },
        { "goblin", "nixa", "signature", "Signed, sealed, and nonrefundable,\n- Nixa Fastfingers, Contracts Clerk of the Goblin Trading Post" },
        { "goblin", "nixa", "ps", "P.S. This letter is legally binding. On you. Not on us." },
        { "goblin", "nixa", "ps", "P.S. All complaints must be submitted in triplicate, in Goblin, written backwards." },
        { "goblin", "nixa", "ps", "P.S. The Post may change prices at any time, in any direction, but mostly up." },
        { "goblin", "nixa", "ps", "P.S. By reading this P.S. you agreed to read the next P.S. Sorry, there is none." },
        { "horde", "auctioneer", "handover", "By the authority of the Horde Trading Post, I present to you, {player}, this {item}. Hold it high. Then remember: free samples are worth what they cost. Real goods cost real gold." },
        { "horde", "auctioneer", "handover", "Lok'tar! Behold your free sample: {item}. It is exactly as valuable as you paid for it. For true supplies, come back and pay full price like a proper warrior." },
        { "horde", "auctioneer", "handover", "The Quartermaster sends his regards and this {item}. Treasure it, champion. It is the only cheap thing the Trading Post will ever give you." },
        { "horde", "auctioneer", "handover", "In the name of the Horde, accept this {item}. Free samples are worthless, of course. Our real goods are priceless. Well, not priceless. Very much priced." },
        { "horde", "auctioneer", "handover", "Here, {player}. One {item}, freshly dug from the back of the stockroom. Free, as promised. Now go and earn some gold, the real wares are waiting at full price." },
        { "horde", "auctioneer", "handover", "Strength and honor! Your free sample: {item}. Do not weep, warrior. Nothing good is free, and the Trading Post is the most expensive shop in all of Azeroth." },
        { "horde", "auctioneer", "handover", "I have been told to give this {item} to you with great ceremony. Ceremony done. If you want something useful, pay full price at the Trading Post like everyone else." },
        { "horde", "auctioneer", "handover", "Your free sample awaits: {item}. The ledger values it at nothing at all. Come back with gold and the Trading Post will show you what real goods look like." },
        { "horde", "auctioneer", "handover", "Take this {item}, {player}. The spirits say it is worthless. The Trading Post agrees. That is why it is free. Our good stuff is very, very much not free." },
        { "horde", "auctioneer", "handover", "From the Horde Trading Post, with all due solemnity: one {item}. You asked for free. You got free. Next time, ask for quality and bring a heavy purse." },
        { "alliance", "auctioneer", "handover", "By gracious decree of the Alliance Trading Post, I bestow upon you, {player}, this exquisite {item}. Its value is precisely what you paid. For true quality, kindly return with gold." },
        { "alliance", "auctioneer", "handover", "For the Alliance! Behold your free sample: {item}. Do treasure it. It is the only bargain the Trading Post will ever offer, and it was never much of one." },
        { "alliance", "auctioneer", "handover", "The Steward himself selected this {item} for you. With tongs. Free samples are, naturally, worthless. Our genuine wares cost a small fortune, as is proper." },
        { "alliance", "auctioneer", "handover", "May the Light shine upon this {item}, your free sample. It needs all the help it can get. For goods of actual worth, the Trading Post charges full price, gladly." },
        { "alliance", "auctioneer", "handover", "Here you are, {player}: one {item}, entered in the ledger at a value of nothing. Should you desire something useful, the most overpriced shop in the kingdom awaits." },
        { "alliance", "auctioneer", "handover", "With great ceremony I present your free sample: {item}. Do not look so disappointed. Quality costs gold, and the Trading Post costs more gold than anyone." },
        { "alliance", "auctioneer", "handover", "Hauled all the way from Ironforge, your free sample: {item}. Worth every copper you paid for it, which is none. Come back and buy the real thing at full price." },
        { "alliance", "auctioneer", "handover", "Accept this {item} with the gratitude it deserves, which is very little. Free samples at the Trading Post are worthless by design. Our prices are outrageous by tradition." },
        { "alliance", "auctioneer", "handover", "In the name of the Alliance Trading Post, this {item} is yours. A gift as noble as it is useless. Return soon with a full purse and see what proper supplies look like." },
        { "alliance", "auctioneer", "handover", "The household presents you, {player}, with one {item}. It is entirely free and entirely pointless. Real goods await at full price - the finest prices in the land." },
        { "goblin", "auctioneer", "handover", "Congratulations, {player}! Your free sample: one genuine {item}! Worth exactly nothing, just like every free thing. Real goods? Full price, pal. Time is money!" },
        { "goblin", "auctioneer", "handover", "Here it is, fresh from the warehouse: {item}! Free, as promised. Not useful, not valuable, but free. Now come back and buy something that actually costs gold." },
        { "goblin", "auctioneer", "handover", "By the terms of the free sample offer, I hereby hand over one {item}. No refunds, no returns, no value. The good stuff is at the Trading Post, priced to perfection." },
        { "goblin", "auctioneer", "handover", "Ta-da! One {item}, absolutely free! Do not ask what it is worth. The Goblin Trading Post never gives away anything valuable. That is what makes us the priciest shop around." },
        { "goblin", "auctioneer", "handover", "Your free sample, {player}: {item}! Gizzik wanted me to say it is a premium item. It is not. Premium items cost premium gold. Come back with lots of it." },
        { "goblin", "auctioneer", "handover", "Here, take this {item}. It survived a warehouse explosion, which is more than can be said for its value. Free samples are junk, friend. Real goods cost real gold." },
        { "goblin", "auctioneer", "handover", "Enjoy your free {item}! There are no fees on it, which is a miracle. There is also no value in it, which is not. Buy the real stuff at full price, pal." },
        { "goblin", "auctioneer", "handover", "With the full pomp of the Steamwheedle Cartel, I present: {item}! Free! Worthless! Exactly as advertised in the fine print. Shopping at full price begins now." },
        { "goblin", "auctioneer", "handover", "One {item}, handed over with tremendous ceremony. Is it junk? Legally, I cannot say. Practically, yes. The Trading Post sells the good stuff, at the worst prices in Azeroth." },
        { "goblin", "auctioneer", "handover", "There you go, {player}: {item}, gratis! Most overpriced shop in the land, and this is what free gets you. Bring gold next time and we will show you what money can buy." },
        { "horde", "grukka", "sample", "Listen, {player}. This week the Trading Post gives something away. Free. Grukka does not say that word lightly. Go to any auctioneer. Say \"I am here for my free sample!\" Take it with honor. Do not waste it." },
        { "horde", "grukka", "sample", "Grukka has seen many things. Battle. Blood. Bargains. Nothing like this week's free sample. Strong warriors weep when they hear of it. Find an auctioneer, ask for your free sample, and stand taller after." },
        { "horde", "grukka", "sample", "No speeches. You want it. Every orc in Orgrimmar wants it. Only those who walk to an auctioneer and claim their free sample will have it. The rest will talk about you around the fire. Let them." },
        { "horde", "grukka", "sample", "Honor demands Grukka speak plainly: this week's free sample is beyond words. So Grukka uses none. Go. Any auctioneer. Ask for the free sample. Return a different warrior." },
        { "horde", "grukka", "sample", "The Warchief does not have one. Grukka checked. You can. Go to an auctioneer, say you are here for your free sample, and hold what even chieftains cannot. Lok'tar." },
        { "horde", "grukka", "sample", "Grukka counts the stock. Small. Very small. Weak ones hesitate and lose. Strong ones march to the nearest auctioneer today and demand their free sample. Which are you, {player}?" },
        { "horde", "grukka", "sample", "Some gifts are earned in battle. This one is earned by walking. Visit any auctioneer, speak the words \"I am here for my free sample!\" and accept what fate has set aside for you. Grukka approves." },
        { "horde", "grukka", "sample", "Grukka does not smile. Grukka smiled once this week. Because of the free sample. Think on that. Then go to an auctioneer and ask for yours before the rest of the Horde does." },
        { "horde", "zuljabi", "sample", "Ey mon, {player}! Zul'jabi got news dat make de spirits dance! Dis week de Trading Post be givin' away a free sample, and it be somethin' else, mon. Go to any auctioneer and say \"I am here for my free sample!\"" },
        { "horde", "zuljabi", "sample", "De loa whisper to Zul'jabi in de night, mon. Dey say de free sample dis week be too good for words. So go see an auctioneer, ask for it nice, and let de mystery wrap around ya like a warm jungle breeze." },
        { "horde", "zuljabi", "sample", "Ya know what be better den a bargain, mon? Somethin' for free! De auctioneer be waitin' wit your free sample right now. Just walk up, grin big, and ask for it. Everybody gonna be askin' where ya got dat!" },
        { "horde", "zuljabi", "sample", "Zul'jabi been runnin' caravans all over Kalimdor and never seen nothin' like dis week's free sample, mon. De whole Echo Isles be jealous. Hurry to an auctioneer and claim it before de others snatch it up!" },
        { "horde", "zuljabi", "sample", "Stay away from de hype? No, mon, run TOWARD de hype! Dis free sample be de talk of every tavern from Booty Bay to Undercity. Ask any auctioneer for it and feel de good times roll, ya hear?" },
        { "horde", "zuljabi", "sample", "Zul'jabi don't make promises lightly, mon. But dis one be sure: ya life gonna split into before de free sample and after de free sample. Go find an auctioneer and pick de after, mon!" },
        { "horde", "zuljabi", "sample", "Psst, {player}. Come close, mon. Dis week de free sample be so exclusive, even Zul'jabi only got to peek at de crate. Ya want it? Visit an auctioneer and say \"I am here for my free sample!\" Easy, mon." },
        { "horde", "zuljabi", "sample", "De wise troll say: never turn down a gift from de Trading Post. De smart troll say: run to de auctioneer before anybody else. Be both, mon! Ask for ya free sample today and let de good times roll." },
        { "horde", "thalsorn", "sample", "Peace be upon you, {player}. As the rain finds the thirsty plain, so this week's free sample has found its way to you. Walk calmly to any auctioneer and ask for it. The Earth Mother smiles on those who answer her call." },
        { "horde", "thalsorn", "sample", "The winds over Mulgore carry rumors of this week's free sample. Even the kodo pause their grazing to listen. Do not let the season pass you by. Visit an auctioneer and ask for what has been set aside for you." },
        { "horde", "thalsorn", "sample", "Some gifts arrive like the dawn - quietly, and then all at once. This week's free sample is such a gift. Go to any auctioneer, speak the words \"I am here for my free sample!\" and let your path change its course." },
        { "horde", "thalsorn", "sample", "Thalsorn has recorded many entries in the ledger, yet none like this. The free sample is rare as a white stag in the deep woods. Seek out an auctioneer, ask humbly, and receive it before it fades into memory." },
        { "horde", "thalsorn", "sample", "The Earth Mother gives freely, and this week, so does the Trading Post. A free sample awaits you at every auctioneer. Ask for it with an open heart, and others will look upon you as the hawk looks upon the high peak." },
        { "horde", "thalsorn", "sample", "Be patient as the mountain, but not too patient. The free sample is here only while the moon turns once. Go to an auctioneer and ask for it, {player}. Your ancestors would not wish you to miss this." },
        { "horde", "thalsorn", "sample", "A river does not ask why it flows. You need not ask what the free sample is. Only know that it is meant for you. Walk to any auctioneer and say you are here for your free sample. The rest will unfold like spring." },
        { "horde", "thalsorn", "sample", "Thalsorn sat beneath the great tree and pondered this week's free sample for a long time. He found no words worthy of it. Perhaps you will. Visit an auctioneer, ask for it, and listen to what your spirit says." },
        { "horde", "ambrose", "sample", "Greetings from beyond, {player}. Very little excites the dead. This week's free sample managed it. Briefly. Shamble over to any auctioneer and say \"I am here for my free sample!\" You will not regret it. Probably." },
        { "horde", "ambrose", "sample", "I have been dead for quite some time and have seen everything worth seeing. Then I saw this week's free sample. Ask any auctioneer for yours. It is to die for, and I would know." },
        { "horde", "ambrose", "sample", "Life is short. Unlife is long and tedious. Break the monotony: visit an auctioneer and claim your free sample. Some say it is the most exciting thing to happen since the Plague. I am one of those people." },
        { "horde", "ambrose", "sample", "The Dark Lady has not received one. Do not tell her I said that. Go quietly to any auctioneer, ask for your free sample, and enjoy the envy of the living and the dead alike." },
        { "horde", "ambrose", "sample", "Shipping notice: one free sample, reserved in your name, awaiting collection at your nearest auctioneer. Unclaimed items are disposed of in the usual manner. You do not want to know the usual manner." },
        { "horde", "ambrose", "sample", "Envy is a sin, they tell me. Then prepare to make sinners of everyone around you. This week's free sample is that remarkable. Ask an auctioneer for it, {player}, and watch them seethe. It warms what is left of my heart." },
        { "horde", "ambrose", "sample", "I rarely recommend anything. Recommending things implies hope. Yet here I am, recommending this week's free sample. Visit any auctioneer and ask for it. Mark this day. It may never happen again." },
        { "horde", "ambrose", "sample", "Someday they will carve it on your tombstone: here lies one who had the free sample. Not soon, I hope, for your sake. Go to an auctioneer and say \"I am here for my free sample!\" Secure your legacy." },
        { "alliance", "percival", "sample", "Esteemed {player}, it is with considerable condescension that I inform you of this week's free sample. Such refinement is seldom offered to commoners. Present yourself at any auctioneer and request it. Do try to look presentable." },
        { "alliance", "percival", "sample", "In all my years as Steward, I have rarely beheld anything so exquisite as this week's free sample. Lords of Stormwind would duel for it. You need merely visit an auctioneer and ask. How fortunate you are." },
        { "alliance", "percival", "sample", "One does not simply receive a free sample. One is graced with it. Kindly proceed to the nearest auctioneer and announce, \"I am here for my free sample!\" Enunciate, please. We have standards." },
        { "alliance", "percival", "sample", "The House of Ashcombe has endorsed very few things in its long and illustrious history. This week's free sample is now among them. Call upon any auctioneer and claim yours. You may thank me in writing." },
        { "alliance", "percival", "sample", "Rumor has it that a certain duchess fainted upon hearing of this week's free sample. I cannot confirm it, but I can confirm it is worth fainting over. Visit an auctioneer and request your own, with dignity." },
        { "alliance", "percival", "sample", "Elegance. Distinction. Exclusivity. These words are insufficient to describe this week's free sample, and I have a very large vocabulary. Go to any auctioneer and ask for it. Your social standing will thank you." },
        { "alliance", "percival", "sample", "My dear {player}, the court is abuzz. Everyone who matters is speaking of the free sample. Everyone who does not matter is asking what it is. Be among the former. See an auctioneer and claim it at once." },
        { "alliance", "percival", "sample", "The King himself has not been offered one. I mention this not to boast, but rather to boast. Present yourself at an auctioneer, ask for your free sample, and join a most exclusive circle indeed." },
        { "alliance", "tibbly", "sample", "Attention, {player}! Tibbly has checked, double-checked and triple-checked the inventory, and yes, this week's free sample is real and reserved for you! Proceed in an orderly fashion to any auctioneer and say \"I am here for my free sample!\"" },
        { "alliance", "tibbly", "sample", "Oh my gears, oh my sprockets! Tibbly has never filed an entry this exciting! The free sample is catalogued, cross-referenced and ready. Please visit an auctioneer and request it. Neatly. Do not smudge the ledger." },
        { "alliance", "tibbly", "sample", "Tibbly must insist that you collect this week's free sample promptly! Uncollected samples create discrepancies, and discrepancies give Tibbly hives. Visit any auctioneer and ask for it. For Tibbly's sake!" },
        { "alliance", "tibbly", "sample", "According to Tibbly's careful projections, owning this week's free sample increases general happiness by a frankly unreasonable margin. See an auctioneer, ask for your free sample, and help Tibbly's numbers look good!" },
        { "alliance", "tibbly", "sample", "The engineers of Gnomeregan would trade their best wrenches for what you can get for nothing. Do not tell them! Simply slip over to an auctioneer and ask for your free sample. Tibbly will log it quietly." },
        { "alliance", "tibbly", "sample", "Tibbly wrote 'remarkable' in the ledger, crossed it out and wrote 'extraordinary', then crossed that out and wrote 'indescribable'. The ledger is now a mess. Ask any auctioneer for your free sample and see why!" },
        { "alliance", "tibbly", "sample", "Item: one free sample. Status: reserved. Recipient: {player}. Action required: visit an auctioneer and ask for it. Tibbly has highlighted this entry in three colors. That has never happened before!" },
        { "alliance", "tibbly", "sample", "Rare? Rarer than a tidy workshop! Exclusive? More exclusive than the inner council of Tinker Town! Tibbly could go on, but the auctioneer is waiting with your free sample. Hurry along now, and mind the paperwork!" },
        { "alliance", "durgan", "sample", "Oi, {player}! Hauled this week's free sample over every pothole between Ironforge and Menethil, I did. Worth every bruise. Get yerself to an auctioneer and say \"I am here for my free sample!\" Then buy me an ale." },
        { "alliance", "durgan", "sample", "Roads were muddy, wagon was stuck, me beard got soaked. And still I'd do it all again for this week's free sample. That's how grand it is. Stomp over to any auctioneer and ask for yours, lad or lass." },
        { "alliance", "durgan", "sample", "Better than a fresh barrel of Thunderbrew? Aye, I said it, and I don't take those words lightly. Pop in at an auctioneer, ask for yer free sample, and see if ye don't agree with old Durgan." },
        { "alliance", "durgan", "sample", "Every dwarf in the tavern's been grumblin' about this week's free sample. Not 'cause it's bad - 'cause they havnae got one yet! Beat 'em to it. Visit an auctioneer and claim it, {player}." },
        { "alliance", "durgan", "sample", "Took the long road through the Wetlands for this one. Never again. Well, maybe again, if it's for another free sample like this. Off with ye to any auctioneer and ask for it before the others grab all the luck." },
        { "alliance", "durgan", "sample", "Durgan doesnae get excited easy. Seen avalanches. Seen bandits. Seen a ram chase a gryphon. But this week's free sample? Had to sit down, I did. Go see an auctioneer and ask for yours. Sit down after." },
        { "alliance", "durgan", "sample", "Me cousin in Thelsamar asked what the free sample is. Told him to go find out himself, the lazy lump. Same goes for you! March to an auctioneer, ask for yer free sample, and raise a mug to Durgan after." },
        { "alliance", "durgan", "sample", "Aye, the roads are rotten, the weather's worse, and the prices at the Trading Post are criminal. But this week's free sample costs ye nothin' at all! Find any auctioneer, ask nice, and count yerself lucky." },
        { "alliance", "ishaali", "sample", "Blessings of the Light upon you, {player}. Ishaali believes this week's free sample was meant to find you. Go to any auctioneer and say \"I am here for my free sample!\" Let your heart be open to what comes." },
        { "alliance", "ishaali", "sample", "The Naaru teach that every gift has purpose. Ishaali has meditated long on this week's free sample, and her faith has never been so stirred. Seek out an auctioneer, ask for it, and walk in wonder." },
        { "alliance", "ishaali", "sample", "Across the Twisting Nether, our people saw many marvels. Ishaali dares to say this week's free sample stands among them. Visit an auctioneer, request it with grace, and may the Light guide your steps." },
        { "alliance", "ishaali", "sample", "Some blessings are loud, some are quiet. This week's free sample is both, in a way Ishaali cannot explain. Only experience can teach it. Ask any auctioneer for your free sample, friend, and be uplifted." },
        { "alliance", "ishaali", "sample", "The vindicators of the Exodar speak of it in hushed voices. The anchorites pray for patience. You need only visit an auctioneer and ask for your free sample. The Light favors the prompt, {player}." },
        { "alliance", "ishaali", "sample", "Ishaali has shipped many parcels, but never one that filled her with such hope. The free sample awaits you at any auctioneer. Ask for it, and may it bring you the peace the Naaru have promised." },
        { "alliance", "ishaali", "sample", "Do not let doubt cloud your path. Faith says the free sample is extraordinary, and Ishaali has faith in abundance. Go to an auctioneer and say you are here for your free sample. The Light will do the rest." },
        { "alliance", "ishaali", "sample", "In the song of the Naaru there is a note that only the fortunate hear. This week's free sample is that note. Hurry to any auctioneer, ask for it, and let others wonder why you look so blessed." },
        { "goblin", "gizzik", "sample", "Hey hey, {player}! Gizzik here with the deal of the CENTURY! This week's free sample is so good, you'll want ten more - which, coincidentally, we sell. Run to any auctioneer and say \"I am here for my free sample!\"" },
        { "goblin", "gizzik", "sample", "Free! The most beautiful word in the goblin tongue, right after 'profit'. This week's free sample is the gateway to a whole new you. Grab it from an auctioneer, then ask about our premium edition. Trust me." },
        { "goblin", "gizzik", "sample", "Listen, friend. Gizzik doesn't hand out free stuff. Gizzik invests in future customers. That's you! Pop by any auctioneer, claim your free sample, and get ready to wonder how you ever lived without it." },
        { "goblin", "gizzik", "sample", "Limited time! Limited stock! Unlimited satisfaction! This week's free sample flies off the shelves faster than a rocket with a loose fuse. Hit up an auctioneer, ask for it, and tell your friends. Especially rich ones." },
        { "goblin", "gizzik", "sample", "You know what's better than a free sample? Two free samples! You know what we're giving you? One. But it's a really, really good one. Visit any auctioneer and claim it before Gizzik changes his mind." },
        { "goblin", "gizzik", "sample", "{player}, Gizzik ran the numbers and you NEED this week's free sample. Your rivals want it. Your guild will talk. Head to an auctioneer, ask for it, and upgrade your entire existence. You're welcome." },
        { "goblin", "gizzik", "sample", "Free sample today, loyal customer tomorrow, VIP Premier Elite member next week! It all starts at the nearest auctioneer with seven simple words: \"I am here for my free sample!\" Say 'em loud, say 'em proud." },
        { "goblin", "gizzik", "sample", "Gizzik guarantees satisfaction or your free sample back! That's how confident we are. Go see any auctioneer, ask for your free sample, and while you're there, browse our exciting new overpriced selection." },
        { "goblin", "fenny", "sample", "Dear {player}, Fenny is pleased to report that this week's free sample carries no collection fee, no handling fee and no sample surcharge. Fenny checked twice and is still in shock. Visit any auctioneer and ask for it!" },
        { "goblin", "fenny", "sample", "It is free. Truly free. Fenny had to lie down after learning that. No tariffs, no levies, no convenience charges. Simply visit an auctioneer and say \"I am here for my free sample!\" Enjoy it before accounting notices." },
        { "goblin", "fenny", "sample", "Fenny has calculated the value of this week's free sample and the result broke the abacus. That repair fee, kindly, is not your concern. Hurry to any auctioneer and ask for your free sample." },
        { "goblin", "fenny", "sample", "Ordinarily, Fenny would apply an exclusivity surcharge to something this exclusive. This week, management has waived it. Do not make Fenny regret this. Visit an auctioneer and collect your free sample promptly." },
        { "goblin", "fenny", "sample", "Late pickup fees? Not this week. Excitement tax? Waived. Envy duty? Your neighbors will pay that one. Stroll over to any auctioneer, request your free sample, and enjoy a rare fee-free moment, {player}." },
        { "goblin", "fenny", "sample", "Fenny would like to remind you that walking to an auctioneer remains entirely free of charge, for now. Use this window wisely: go ask for your free sample. Its worth exceeds every fee schedule Fenny has ever written." },
        { "goblin", "fenny", "sample", "Notice of waived charges: the free sample, the joy of owning it, and the admiration of others are all exempt from fees this week. Fenny is as surprised as you are. Visit an auctioneer and claim it." },
        { "goblin", "fenny", "sample", "Fenny dreams in surcharges, yet even Fenny cannot find a fee to attach to this week's free sample. That alone should tell you how special it is. Go to any auctioneer and ask for yours before policy changes." },
        { "goblin", "krazzle", "sample", "KABOOM, {player}! That's the sound of this week's free sample blowing your mind! Krazzle's dispatch crew is still picking their jaws off the floor. Blast over to any auctioneer and say \"I am here for my free sample!\"" },
        { "goblin", "krazzle", "sample", "Krazzle has dispatched rockets, bombs and one very angry sapper, but nothing ever hit like this week's free sample. Detonate your expectations! Go see an auctioneer and ask for it. Mind the hype." },
        { "goblin", "krazzle", "sample", "This free sample makes more noise in the gossip mill than anything Krazzle ever blew up, and that is saying something! Rush to any auctioneer, ask for your free sample, and watch your boredom go up in smoke." },
        { "goblin", "krazzle", "sample", "Light the fuse on your weekend! This week's free sample is pure dynamite - figuratively, Krazzle's lawyer insists. Zip to an auctioneer and ask for it before the whole Horde and Alliance stampede the counter." },
        { "goblin", "krazzle", "sample", "Krazzle once blew up a warehouse just to feel something. Now Krazzle has the free sample to look forward to instead. Get to any auctioneer, {player}, ask for it, and feel EVERYTHING." },
        { "goblin", "krazzle", "sample", "Boom! Bang! Whoosh! Those are the noises your friends will make when they see you've got this week's free sample. Fire yourself toward an auctioneer and ask for it. Krazzle recommends a running start." },
        { "goblin", "krazzle", "sample", "Dispatch report: free samples are flying out faster than a goblin rocket car with no brakes. Do not get left in the crater! Hit any auctioneer and say you are here for your free sample. Go go go!" },
        { "goblin", "krazzle", "sample", "Krazzle has been told to stop using the word explosive in ads. So Krazzle will just say this week's free sample is extremely, unbelievably, jaw-droppingly good. Ask any auctioneer for it now!" },
        { "goblin", "nixa", "sample", "Notice to {player}, hereinafter the Lucky Recipient: you are entitled to one free sample, as described in no detail whatsoever below. To claim, attend any auctioneer and declare \"I am here for my free sample!\" Terms apply." },
        { "goblin", "nixa", "sample", "Whereas this week's free sample is rare, and whereas you are deserving, the Trading Post hereby invites you to collect it at any auctioneer. Nixa assures you the fine print is very, very fine. Do not squint." },
        { "goblin", "nixa", "sample", "By reading this flyer, you acknowledge that this week's free sample is extraordinary, exclusive and the envy of all. Visit an auctioneer to execute your claim. Clause seven, subsection B: no takebacks." },
        { "goblin", "nixa", "sample", "Nixa has drafted many contracts, but none with an enthusiasm clause until now. This week's free sample triggered it. Present yourself at any auctioneer and request your free sample. Signature not required. Probably." },
        { "goblin", "nixa", "sample", "The party of the first part, the Trading Post, grants the party of the second part, you, the privilege of this week's free sample. Collection at any auctioneer. Envy of third parties is expressly anticipated." },
        { "goblin", "nixa", "sample", "Section one: the free sample is amazing. Section two: see section one. Section three: go to an auctioneer and ask for it. Nixa has never written a contract this short, because nothing more needs saying." },
        { "goblin", "nixa", "sample", "Disclaimer: the Trading Post is not liable for overwhelming joy, sudden popularity or uncontrollable bragging resulting from this week's free sample. Claim yours at any auctioneer, {player}. You have been warned." },
        { "goblin", "nixa", "sample", "All rights reserved. All samples reserved too - including yours! Nixa has stamped, sealed and notarized your claim. Simply visit an auctioneer and say you are here for your free sample. Offer void where boring." },
        { "horde", "grukka", "welcome_subject", "Horde Trading Post: NOW OPEN" },
        { "horde", "grukka", "welcome_subject", "Grukka speaks: the Trading Post is open" },
        { "horde", "grukka", "welcome_subject", "Read this, warrior. We are open." },
        { "horde", "grukka", "welcome_opening", "{player}. Grukka Ironjaw writes. Quartermaster of the {post}. Hear me: the {post} is OPEN. Drums were beaten. Banners were raised. A goblin tried to make a speech. I stopped him. Now you know." },
        { "horde", "grukka", "welcome_opening", "Strength and honor, {player}. I am Grukka Ironjaw, Quartermaster. I do not write letters. Today I write one. The {post} has opened its doors, and I was told to announce it with fanfare. So: FANFARE. There. Done." },
        { "horde", "grukka", "welcome_opening", "Hear me, {player}! Grukka Ironjaw, Quartermaster, bids you stand tall. The {post} opens today. The crates are stacked, the ledgers are sharp, the prices are sharper. A great day for the Horde. Possibly for my purse also." },
        { "horde", "grukka", "welcome_opening", "{player}. I am Grukka Ironjaw. I keep the stores of the {post}. Today the gates swing wide and the Trading Post opens for every true child of the Horde. There was a ribbon. I cut it with my axe. It was a good ribbon." },
        { "horde", "grukka", "welcome_what", "Why does the post exist? The auction house runs dry. Ore gone. Herbs gone. Potions gone. A warrior waits for no one. So the Trading Post sells what the auction house has run out of. Materials, crafted supplies. Nothing else. No trinkets, no hats." },
        { "horde", "grukka", "welcome_what", "Find us at any auctioneer in any auction house. Speak to them. Choose the Trading Post. The auction window opens with our goods. Ore, herbs, cloth, leather, gems. Potions, food, scrolls, cut gems. Only what the auction house lacks. We do not compete. We fill gaps." },
        { "horde", "grukka", "welcome_what", "Our prices are high. I will not lie. A warrior who lies about prices has no honor. They are shamelessly high, and we are proud of it. You pay for the goods, and you pay for having them when nobody else does. That is the trade. Take it or leave it." },
        { "horde", "grukka", "welcome_what", "Every auctioneer, every auction house, carries the post now. Ask for the Trading Post. You will see materials and crafted supplies the market has run out of. Expensive? Yes. Very. Grukka does not haggle. Grukka does not apologize. Grukka sells." },
        { "horde", "grukka", "welcome_how", "The rules. Stock is limited each day, the same stock for everybody. When it is gone, it is gone. Each warrior may buy one lot of a thing per day. No hoarding. Buy outright and the goods wait in your mailbox at once. Bid instead, pay less, and the caravan brings them when the auction ends." },
        { "horde", "grukka", "welcome_how", "Know this. While stocks last, we sell. One lot of each thing per day per customer. Pay the full price and your mailbox has it at once. Place the cheaper bid and wait: when the auction ends, the caravan delivers, within a day. Patience is also strength." },
        { "horde", "grukka", "welcome_how", "From now on I send you a flyer every day with the day's offers. Read it. Or do not. If you want silence, tell any auctioneer to stop sending flyers. I will not weep. Remember: limited stock, one lot each per day, buy now for the mailbox, or bid and wait for the caravan." },
        { "horde", "grukka", "welcome_how", "Daily stock is shared by all. Be quick. One lot of each thing per day, per customer. Buyout means the goods are in your mailbox at once. A bid costs less, and the caravan carries the goods to you when the auction ends. Each day a flyer will come with the offers. Stop it at any auctioneer if you must." },
        { "horde", "grukka", "unsubscribe", "You will throw this in the fire. I know. Fine. To stop the flyers, tell your auctioneer: Stop sending me your flyers. No hard feelings. Some." },
        { "horde", "grukka", "unsubscribe", "Grukka knows this letter annoys you. Grukka does not care. But if you care, tell any auctioneer to stop sending you flyers. Done." },
        { "horde", "grukka", "unsubscribe", "Into the bin with this, yes? Honorable. Say \"Stop sending me your flyers\" at any auctioneer and they stop. Grukka keeps his word." },
        { "horde", "grukka", "unsubscribe", "A warrior hates paperwork. So do I. Burn this flyer, or end them all: tell your local auctioneer to stop sending you flyers." },
        { "horde", "zuljabi", "welcome_subject", "Big news, mon! Trading Post be open!" },
        { "horde", "zuljabi", "welcome_subject", "De Trading Post be OPEN, mon!" },
        { "horde", "zuljabi", "welcome_subject", "Zul'jabi say: come shop, mon!" },
        { "horde", "zuljabi", "welcome_opening", "Hey dere, {player}! Zul'jabi here, Caravan Master of de {post}! Ya hear dem drums? Dat be for YOU, mon, because de {post} be open for business! Grand opening, big party, de raptors even got ribbons on. Ya ain't seen nothin' like it." },
        { "horde", "zuljabi", "welcome_opening", "Greetings, {player}, ya lucky soul! Dis be Zul'jabi writin', de Caravan Master. De {post} just open its doors, mon, and Zul'jabi be dancin' on de crates. Dat be a grand opening, de best one dis side of de jungle." },
        { "horde", "zuljabi", "welcome_opening", "{player}, mon! Stop what ya doin'! Zul'jabi, Caravan Master, got de big news: de {post} be OPEN! De wagons be loaded, de kodos be fed, and de spirits say business gonna be GOOD. Come see, come see!" },
        { "horde", "zuljabi", "welcome_opening", "Ah, {player}, Zul'jabi be so happy to write ya. Me de Caravan Master of de {post}, and today we open de gates wide, mon. Fireworks, drums, a goblin cryin' about de cost of de fireworks. Dat be a real grand opening!" },
        { "horde", "zuljabi", "welcome_what", "So what dis Trading Post be, ya ask? When de auction house run out of somethin', de Trading Post got it, mon. Ore, herbs, cloth, leather, gems, and de crafted stuff too: potions, food, scrolls, cut gems. Only what de auction house be missin'. Clever, eh?" },
        { "horde", "zuljabi", "welcome_what", "Where ya find us? Easy, mon! Every auctioneer in every auction house now run a Trading Post. Just talk to dem and pick de Trading Post. De auction window open up with all de goods de market done run out of. No long walk, no secret password." },
        { "horde", "zuljabi", "welcome_what", "Now Zul'jabi be honest wit ya, mon: de prices be HIGH. Shamelessly high! And we proud of it, ya know? When nobody else got de ore, de herbs, de potions, dat be worth somethin'. De spirits agree. Mostly. One spirit said it be robbery, but he be quiet now." },
        { "horde", "zuljabi", "welcome_what", "De Trading Post carry materials and crafted supplies, mon, but only what de auction house be out of. Ya go to any auctioneer, ya choose de Trading Post, ya see de goods. Expensive? Oh yes. Very expensive. Dat be part of de charm, mon!" },
        { "horde", "zuljabi", "welcome_how", "Here be how it work, mon. Every day we got limited stock, de same for everybody. When it gone, it gone! Each customer get one lot of a thing per day. Buy it outright and it be in ya mailbox at once. Or place a bid for less, and Zul'jabi caravan bring it when de auction end." },
        { "horde", "zuljabi", "welcome_how", "Listen close, mon: stock be limited, while it last. One lot of each thing per day for each customer, no greedy hands. Pay full price and de goods jump into ya mailbox right away. Bid cheaper and my caravan come rollin' when de auction end, within a day." },
        { "horde", "zuljabi", "welcome_how", "And now de best part, mon: every day Zul'jabi send ya a flyer with de day's offers! Every single day! If ya get tired of dat, just tell any auctioneer to stop sendin' de flyers. Zul'jabi only cry a little. Remember: limited stock, one lot per day, buy now or bid and wait for de caravan." },
        { "horde", "zuljabi", "welcome_how", "De rules be simple, mon. Limited stock each day for de whole world. One lot of a thing per customer per day. Buyout? Mailbox, at once. Bid? Cheaper, and de caravan bring it when de auction close. And a flyer come every day now - ya can stop it at any auctioneer, but why would ya?" },
        { "horde", "zuljabi", "unsubscribe", "Zul'jabi know dis flyer probably go straight in de bin, mon. Dat be okay! Tell ya auctioneer \"Stop sending me your flyers\" if ya want de peace and quiet." },
        { "horde", "zuljabi", "unsubscribe", "Ya rollin' ya eyes at dis flyer, mon? Zul'jabi can feel it from here. Just tell any auctioneer to stop sendin' ya de flyers. No hard feelin's, mon." },
        { "horde", "zuljabi", "unsubscribe", "If dis flyer be lining de raptor cage, dat be fine, mon. Or tell ya local auctioneer to stop sendin' ya flyers. De raptor gonna miss it though." },
        { "horde", "zuljabi", "unsubscribe", "Another flyer, mon! Zul'jabi know, Zul'jabi know. Ask any auctioneer to stop sendin' ya de flyers and de mailbox be quiet again." },
        { "horde", "thalsorn", "welcome_subject", "The Trading Post opens its doors" },
        { "horde", "thalsorn", "welcome_subject", "A new spring: the Trading Post opens" },
        { "horde", "thalsorn", "welcome_subject", "By the Earth Mother: we are open" },
        { "horde", "thalsorn", "welcome_opening", "Peace upon you, {player}. I am Thalsorn Mistwalker, Ledger-Keeper of the {post}. As the first rain wakes the plains, so today the {post} wakes and opens its doors. The drums are gentle, but they are drums. Let this be a joyful day." },
        { "horde", "thalsorn", "welcome_opening", "Greetings, {player}. Thalsorn Mistwalker writes to you, keeper of the ledgers of the {post}. The Earth Mother has seen the grass grow and the rivers rise, and now she sees the {post} open its doors to all. A grand opening, slow and proud as a mountain." },
        { "horde", "thalsorn", "welcome_opening", "{player}, may the wind be at your back. I am Thalsorn Mistwalker, who keeps the ledgers of the {post}. Today our gates open like a flower to the morning sun. Come, the long wait is over, and the trade begins." },
        { "horde", "thalsorn", "welcome_opening", "Hear the drums of the plains, {player}. Thalsorn Mistwalker, Ledger-Keeper, bids you welcome. The {post} has opened, a new watering hole for all who travel. The kodos are dressed in their finest blankets. Even they know this is a great day." },
        { "horde", "thalsorn", "welcome_what", "Even the richest field has its dry season. When the auction house runs out of ore, herbs, cloth, leather or gems, or of potions, food, scrolls and cut gems, the Trading Post offers what is missing. We do not crowd the market. We grow only in the empty spaces." },
        { "horde", "thalsorn", "welcome_what", "You will find us wherever there is an auctioneer, in every auction house of the world. Speak to the auctioneer and choose the Trading Post. The auction window will open and show you our goods, only the things the auction house has run out of, as the river fills only the empty bed." },
        { "horde", "thalsorn", "welcome_what", "I will speak plainly, as the Earth Mother would wish. Our prices are high. Shamelessly high, and we are proud of it. What is rare costs much, as water costs much in the desert. You pay for what nobody else can give you today." },
        { "horde", "thalsorn", "welcome_what", "Materials and crafted supplies, carried from far places: that is what the Trading Post offers. Visit any auctioneer of any auction house and choose the Trading Post. You will see only what the market lacks, and you will see it at a price that would make a kodo blush." },
        { "horde", "thalsorn", "welcome_how", "The ways of the post are simple. Each day the stock is limited, shared by all who come, like berries on a bush. Each traveler may take one lot of a thing per day. Buy outright and the goods rest in your mailbox at once. Bid for less, and our caravan brings them when the auction ends, within a day." },
        { "horde", "thalsorn", "welcome_how", "While the stocks last, we trade. One lot of each thing, per customer, per day, so that all may eat from the same harvest. If you pay the full price, your mailbox receives the goods at once. If you place the cheaper bid, be patient as the seasons: the caravan comes when the auction ends." },
        { "horde", "thalsorn", "welcome_how", "From this day on, a flyer with the day's offers will find you each morning, like the dew. If you wish it to stop, ask any auctioneer to stop sending you flyers, and the dew will dry. Remember: limited stock, one lot per day, buy now for the mailbox, or bid and wait for the caravan." },
        { "horde", "thalsorn", "welcome_how", "Know the rhythm of the post. Limited stock each day for all. One lot of a thing per customer and day. Buy outright, and it is in your mailbox at once. Bid, and the caravan brings it when the auction ends. And each day a flyer will come, unless you ask an auctioneer to stop it." },
        { "horde", "thalsorn", "unsubscribe", "Perhaps this flyer will feed the campfire tonight. The Earth Mother forgives. To stop them, tell your auctioneer: Stop sending me your flyers." },
        { "horde", "thalsorn", "unsubscribe", "Like leaves in autumn, these flyers keep falling. If they weary you, ask any auctioneer to stop sending you flyers. The tree will understand." },
        { "horde", "thalsorn", "unsubscribe", "I sense this letter brings you little joy. Be at peace: your local auctioneer can stop the flyers if you ask. The wind carries no grudge." },
        { "horde", "thalsorn", "unsubscribe", "Even the patient kodo tires of the same grass each day. If our flyers tire you, tell any auctioneer to stop sending you flyers." },
        { "horde", "ambrose", "welcome_subject", "The Trading Post is open. Alas." },
        { "horde", "ambrose", "welcome_subject", "Grand opening. Try to contain yourself." },
        { "horde", "ambrose", "welcome_subject", "We are open. I am still dead." },
        { "horde", "ambrose", "welcome_opening", "Dear {player}. Ambrose Hollowmere, Shipping Clerk of the {post}, writing to you from a desk that has outlived three previous clerks. I am instructed to announce, with fanfare, that the {post} has opened. Fanfare. There, I have done it. Do try to look excited." },
        { "horde", "ambrose", "welcome_opening", "Greetings, {player}, from the land of the living, which is to say not from me. I am Ambrose Hollowmere, Shipping Clerk. The {post} is now open, and they have hung bunting everywhere. Bunting. In this economy. I would weep if my ducts still worked." },
        { "horde", "ambrose", "welcome_opening", "{player}. Ambrose Hollowmere here, Shipping Clerk, deceased, punctual. It is my grim duty and rare pleasure to announce the grand opening of the {post}. There was a ribbon. Someone cut it. Nobody died, which I found a little disappointing." },
        { "horde", "ambrose", "welcome_opening", "Esteemed {player}, I am Ambrose Hollowmere, Shipping Clerk of the {post}. I have been asked to write something festive about our grand opening. I have looked inside myself and found mostly dust. Nonetheless: the {post} is open. Hurrah." },
        { "horde", "ambrose", "welcome_what", "What is the Trading Post, you ask? It is where you go when the auction house has nothing left. No ore, no herbs, no potions, no hope. We sell materials and crafted supplies the market has run out of. Think of us as the undertaker of empty shelves." },
        { "horde", "ambrose", "welcome_what", "You will find us at every auctioneer of every auction house. Speak to one, choose the Trading Post, and the auction window opens with our goods. Ore, herbs, cloth, leather, gems, potions, food, scrolls, cut gems. Only what the auction house has run dry of. We are thorough, like a plague." },
        { "horde", "ambrose", "welcome_what", "Our prices are shamelessly high, and we are proud of it. I am told pride is a sin. So is gluttony, and you are hungry for rare goods. We merely provide. Death and taxes are certain; I can vouch for the first, and our prices handle the second." },
        { "horde", "ambrose", "welcome_what", "Simply put: when the auction house has run out of materials or crafted supplies, the Trading Post has not. Ask any auctioneer, anywhere, for the Trading Post. Prepare your purse. It will feel lighter afterwards, the way I felt after my unfortunate demise." },
        { "horde", "ambrose", "welcome_how", "The procedure, which I recite with the enthusiasm of a gravedigger: stock is limited each day, shared by everyone. Each customer may buy one lot of a thing per day. Buy outright and the goods are in your mailbox at once. Bid for less, and the caravan delivers when the auction ends, within a day." },
        { "horde", "ambrose", "welcome_how", "Stock lasts until it does not, like most things. One lot of each thing per customer and day. Pay the buyout and your mailbox receives it at once, fresher than I am. Place the cheaper bid and wait; the caravan rattles over when the auction ends, within a day." },
        { "horde", "ambrose", "welcome_how", "Henceforth I shall send you a flyer every day with the day's offers. Every day. Relentlessly. Like the grave. Should you wish this to end, tell any auctioneer to stop sending you flyers. Meanwhile: limited stock, one lot per day, buy outright or bid and wait for the caravan." },
        { "horde", "ambrose", "welcome_how", "Limited daily stock, for everybody. One lot of a thing per customer per day. Buyout: mailbox, at once. Bid: cheaper, delivered by caravan when the auction ends. Also, a daily flyer will now haunt your mailbox. Any auctioneer can exorcise it if you ask." },
        { "horde", "ambrose", "unsubscribe", "This flyer will likely end its brief life in your bin. I know the feeling. To stop them, tell your auctioneer: Stop sending me your flyers." },
        { "horde", "ambrose", "unsubscribe", "Yes, another flyer. Bury it if you like. Or tell any auctioneer to stop sending you flyers, and let it rest in peace." },
        { "horde", "ambrose", "unsubscribe", "I imagine you sigh when you see my name. I sighed too, once, when I still had lungs. Any auctioneer can stop these flyers for you." },
        { "horde", "ambrose", "unsubscribe", "Feel free to burn this flyer. Fire is cleansing, they tell me. For a permanent solution, ask your local auctioneer to stop sending flyers." },
        { "alliance", "percival", "welcome_subject", "Hear ye! The Trading Post is open!" },
        { "alliance", "percival", "welcome_subject", "A Grand Opening of Distinction" },
        { "alliance", "percival", "welcome_subject", "By noble decree: the Trading Post opens" },
        { "alliance", "percival", "welcome_opening", "My dear {player}, it is I, Percival Ashcombe, Steward of the {post}, of the Ashcombe Ashcombes, naturally. It falls to me, as the most distinguished member of our staff, to proclaim the grand opening of the {post}. Trumpets, please. Louder. Thank you." },
        { "alliance", "percival", "welcome_opening", "Hear ye, hear ye, {player}! Percival Ashcombe, Steward, writes to inform you that the {post} has opened its gilded doors. There were trumpets, there were banners, there was a very fine luncheon to which you were, regrettably, not invited." },
        { "alliance", "percival", "welcome_opening", "Esteemed {player}, greetings from Percival Ashcombe, Steward of the {post}. It is my immense privilege, and frankly your immense privilege, to announce that the {post} is now open. A historic day. Future generations will paint it, I am sure." },
        { "alliance", "percival", "welcome_opening", "{player}, I trust this letter finds you well-groomed. I am Percival Ashcombe, Steward of the {post}, and I bring news of the highest order: the {post} has opened. I personally cut the ribbon with a silver letter knife. It was magnificent." },
        { "alliance", "percival", "welcome_what", "Allow me to explain, in simple words. When the common auction house has run out of something, ore, herbs, cloth, leather or gems, or potions, food, scrolls and cut gems, the Trading Post supplies it. We fill the gaps that the rabble have left behind." },
        { "alliance", "percival", "welcome_what", "One finds the Trading Post at every auctioneer in every auction house of the realm. Simply address the auctioneer and choose the Trading Post. The auction window will present our goods, and only those the auction house has run out of. We do not stoop to competition." },
        { "alliance", "percival", "welcome_what", "Our prices are, I am pleased to say, shamelessly expensive. We are proud of it. Quality has its cost, and exclusivity has an even greater one. A noble does not ask what a thing costs. A noble asks why it does not cost more. We have answered that question." },
        { "alliance", "percival", "welcome_what", "Materials and crafted supplies of the finest provenance, available whenever the auction house has run out. Visit any auctioneer, choose the Trading Post, and behold. The prices are lofty, as befits the goods, the service, and, of course, myself." },
        { "alliance", "percival", "welcome_how", "The arrangements are as follows. Stock is limited each day and shared among all customers, high and low alike, regrettably. Each customer may acquire one lot of a thing per day. Purchase outright and the goods await you in your mailbox at once. Bid for less, and our caravan delivers when the auction ends." },
        { "alliance", "percival", "welcome_how", "Kindly observe the rules of the house. While stocks last, one lot of each thing per customer per day. The buyout price delivers to your mailbox at once, as is proper. The more modest bid will be honored by caravan when the auction concludes, within a day. Patience is a virtue of the well-bred." },
        { "alliance", "percival", "welcome_how", "Furthermore, you shall henceforth receive a daily flyer with the day's offers, composed with exquisite taste. Should you, inexplicably, wish them to cease, inform any auctioneer to stop sending you flyers. Remember: limited stock, one lot per day, buy outright or bid and await the caravan." },
        { "alliance", "percival", "welcome_how", "In brief: a limited daily stock for everybody. One lot of a thing per customer and day. Buy outright and it is in your mailbox at once; bid for less and the caravan brings it when the auction ends. A daily flyer will grace your mailbox, unless you instruct an auctioneer to stop it. Unthinkable, but possible." },
        { "alliance", "percival", "unsubscribe", "One suspects this flyer shall be used to light a commoner's hearth. Very well. Tell your auctioneer \"Stop sending me your flyers\" if you must." },
        { "alliance", "percival", "unsubscribe", "Should our correspondence vex you, which is frankly incomprehensible, any auctioneer may stop sending you these flyers upon request." },
        { "alliance", "percival", "unsubscribe", "I am aware that lesser minds find daily letters tiresome. If yours is one, tell your local auctioneer to stop sending you flyers. I shan't judge. Much." },
        { "alliance", "percival", "unsubscribe", "Bin this flyer if you will; it is still finer paper than your others. To end the subscription, inform any auctioneer to stop sending flyers." },
        { "alliance", "tibbly", "welcome_subject", "Opening Day! (Please file accordingly)" },
        { "alliance", "tibbly", "welcome_subject", "GRAND OPENING: Alliance Trading Post!" },
        { "alliance", "tibbly", "welcome_subject", "Notice of Opening, Form A, Copy B" },
        { "alliance", "tibbly", "welcome_opening", "Dear {player}, Tibbly Gearwhistle here, Bookkeeper of the {post}! I am thrilled, delighted and properly documented to announce that the {post} is now open! The grand opening went exactly to schedule. Well, nearly. Well, there was a small fire. It is filed." },
        { "alliance", "tibbly", "welcome_opening", "Hello, {player}! This is Tibbly Gearwhistle, Bookkeeper, writing to you on officially stamped paper. The {post} is OPEN! I have triple-checked the doors, the shelves and the ledgers, and everything balances. Mostly. Please hold your applause until the end of the letter." },
        { "alliance", "tibbly", "welcome_opening", "{player}, attention please! Tibbly Gearwhistle, Bookkeeper of the {post}, hereby announces the grand opening of the {post}. Confetti was budgeted, confetti was thrown, and confetti is now being swept up. By me. With a very small broom." },
        { "alliance", "tibbly", "welcome_opening", "Greetings, {player}! I am Tibbly Gearwhistle, keeper of the books, ledgers, receipts, and spare receipts of the {post}. It gives me tremendous pleasure, which I have noted in the margin, to announce that the {post} has opened its doors!" },
        { "alliance", "tibbly", "welcome_what", "What is the Trading Post for? Excellent question, I have prepared a list! When the auction house runs out of materials, like ore, herbs, cloth, leather and gems, or crafted supplies, like potions, food, scrolls and cut gems, the Trading Post steps in. Only for what is missing. Nothing else. I checked." },
        { "alliance", "tibbly", "welcome_what", "Where to find us: at every auctioneer of every auction house! Talk to the auctioneer, choose the Trading Post, and the auction window opens with our goods. It only shows what the auction house has run out of. No duplicates. I cannot abide duplicates." },
        { "alliance", "tibbly", "welcome_what", "About the prices. Ahem. They are high. Shamelessly high, really, and we are proud of it, according to the memo. I have run the numbers several times and they keep coming out large. Rarity, transport, and filing all cost money. Mostly filing." },
        { "alliance", "tibbly", "welcome_what", "In summary: materials and crafted supplies the auction house has run out of, available at any auctioneer through the Trading Post option. Prices: magnificent. Selection: precise. Paperwork: impeccable. Please keep this letter for your records." },
        { "alliance", "tibbly", "welcome_how", "Now, the procedures, in order! Stock is limited each day, the same for everybody. Each customer may buy one lot of a thing per day, per regulation. Buy outright, and the goods are in your mailbox at once. Place a bid, which costs less, and the caravan delivers when the auction ends, within a day." },
        { "alliance", "tibbly", "welcome_how", "Please note: our stock is limited and lasts only while it lasts, which is a very precise definition. One lot of each thing per customer per day. Buyout puts the goods in your mailbox at once. A bid is cheaper, and your goods ride with the caravan when the auction ends." },
        { "alliance", "tibbly", "welcome_how", "From now on, you will receive a daily flyer with the day's offers! Printed, folded and sorted by me personally. If you would like them to stop, tell any auctioneer to stop sending you flyers, and I will update the mailing list. With a sigh. Rules recap: limited stock, one lot per day, buy outright or bid." },
        { "alliance", "tibbly", "welcome_how", "Checklist! Limited daily stock for everybody: yes. One lot of a thing per customer per day: yes. Buyout goes straight to your mailbox: yes. Cheaper bid arrives by caravan when the auction ends: yes. Daily flyer with offers: yes, unless you ask any auctioneer to stop it." },
        { "alliance", "tibbly", "unsubscribe", "I suspect this flyer will be filed under \"bin\". To be removed from the mailing list properly, tell any auctioneer: Stop sending me your flyers." },
        { "alliance", "tibbly", "unsubscribe", "Is this flyer cluttering your bags? I understand, clutter is the worst. Your local auctioneer can stop the flyers. I will file the paperwork." },
        { "alliance", "tibbly", "unsubscribe", "If reading this annoys you, please do not crumple it, the paper is expensive. Instead, ask any auctioneer to stop sending you flyers." },
        { "alliance", "tibbly", "unsubscribe", "Statistically, most flyers go straight into the bin. If you are part of that statistic, tell your auctioneer to stop sending you flyers." },
        { "alliance", "durgan", "welcome_subject", "Open fer business, lads an' lasses!" },
        { "alliance", "durgan", "welcome_subject", "The Trading Post is open! Pour an ale!" },
        { "alliance", "durgan", "welcome_subject", "Grand openin', bumpy roads an' all" },
        { "alliance", "durgan", "welcome_opening", "Oi, {player}! Durgan Stoutkeg here, Caravan Master o' the {post}. Hold onto yer beard: the {post} is open! We had a grand openin' wi' drums an' a keg the size o' a ram. The keg didnae survive. The drums barely did." },
        { "alliance", "durgan", "welcome_opening", "Well met, {player}! This is Durgan Stoutkeg writin', Caravan Master o' the {post}. After weeks o' haulin' crates over roads that'd shake the teeth out o' a troll, the {post} is finally open! Raise a mug wi' me, will ye?" },
        { "alliance", "durgan", "welcome_opening", "{player}, ye lucky soul! Durgan Stoutkeg, Caravan Master, bringin' ye the grand news: the {post} has opened its doors! I'd write more fancy words, but me hands are still shakin' from the road. Or the ale. Hard to tell." },
        { "alliance", "durgan", "welcome_opening", "Hail, {player}! Durgan Stoutkeg o' the {post} here, him what drives the wagons. Today the {post} opened, an' I've been told to make it sound grand. So: GRAND. Now, where's me ale?" },
        { "alliance", "durgan", "welcome_what", "So what's this Trading Post for, ye ask? When the auction house runs dry, an' I mean dry like a tavern after a wedding, we've got what's missin'. Ore, herbs, cloth, leather, gems. Potions, food, scrolls, cut gems. Only what the auction house has run out of." },
        { "alliance", "durgan", "welcome_what", "Ye'll find us at every auctioneer in every auction house. Walk up, have a word, choose the Trading Post, an' the auction window opens wi' our goods. Only stuff the auction house is out of, mind. We're no' here to step on toes. Just to empty purses." },
        { "alliance", "durgan", "welcome_what", "Now, the prices. Aye, they're steep. Steeper than the road through the mountain pass, an' that road's a menace. Shamelessly expensive, an' proud of it! Ye try haulin' ore through mud an' bandits an' see what ye charge." },
        { "alliance", "durgan", "welcome_what", "Materials an' crafted supplies, whatever the auction house has run out of, ready at any auctioneer under the Trading Post. Expensive? Aye, as a round for the whole tavern. Worth it? Ask yerself when yer flask is empty an' the market is too." },
        { "alliance", "durgan", "welcome_how", "Here's how it goes. Stock's limited every day, an' it's the same pile for everybody. One lot o' each thing per customer per day, no greedy dwarves. Buy it outright an' it's in yer mailbox at once. Bid for less, an' me caravan brings it when the auction ends. Roads permittin'." },
        { "alliance", "durgan", "welcome_how", "While stocks last, lad or lass! One lot of a thing per customer an' day. Pay the full price an' yer mailbox has it at once. Place the cheaper bid an' wait for me wagons; they come when the auction ends, within a day. Unless a wheel breaks. Wheels always break." },
        { "alliance", "durgan", "welcome_how", "An' from now on I'll send ye a flyer every day wi' the day's offers. If ye'd rather I didnae, tell any auctioneer to stop sendin' ye flyers. Saves me a walk. Remember: limited stock, one lot per day, buy outright for the mailbox, or bid an' wait for the caravan." },
        { "alliance", "durgan", "welcome_how", "The rules, short as a dwarf's patience: limited daily stock for all. One lot o' a thing per customer per day. Buyout lands in yer mailbox at once. A bid's cheaper, an' the caravan hauls it over when the auction ends. Daily flyer included, stop it at any auctioneer if ye must." },
        { "alliance", "durgan", "unsubscribe", "Aye, this flyer's probably goin' straight in the fire. Fair enough. Tell yer auctioneer \"Stop sending me your flyers\" an' I'll stop haulin' them." },
        { "alliance", "durgan", "unsubscribe", "If this flyer's annoyin' ye, ye're no' alone, I hate deliverin' them. Ask any auctioneer to stop sendin' ye flyers an' we're both happy." },
        { "alliance", "durgan", "unsubscribe", "Use this flyer to wipe yer mug if ye like. Or tell yer local auctioneer to stop sendin' ye flyers. Less weight on me wagon either way." },
        { "alliance", "durgan", "unsubscribe", "Another flyer, another muddy road. If ye'd rather skip 'em, any auctioneer can stop the flyers. I'll drink to that." },
        { "alliance", "ishaali", "welcome_subject", "By the Light, we are now open!" },
        { "alliance", "ishaali", "welcome_subject", "A blessed opening: the Trading Post" },
        { "alliance", "ishaali", "welcome_subject", "Rejoice! The Trading Post is open" },
        { "alliance", "ishaali", "welcome_opening", "Blessings of the Light upon you, {player}. I am Ishaali, Shipping Clerk of the {post}. With a joyful heart I announce that the {post} has opened its doors. The Naaru surely smile upon this day, as do I, and as do our very full storerooms." },
        { "alliance", "ishaali", "welcome_opening", "Greetings, {player}, child of the Light. Ishaali writes to you, humble Shipping Clerk of the {post}. Today, after many prayers and much sweeping, the {post} is open! May its doors bring you comfort, supplies and, the Light willing, reasonable joy." },
        { "alliance", "ishaali", "welcome_opening", "{player}, may the Light guide your steps. I am Ishaali, and I keep the shipments of the {post}. It is my honor to proclaim its grand opening. We sang hymns, we lit candles, and only a small number of crates caught fire. The Light provides." },
        { "alliance", "ishaali", "welcome_opening", "Peace and Light to you, {player}. Ishaali of the {post} writes, Shipping Clerk and servant of the Naaru. Rejoice, for the {post} is now open! Our journey across the stars was long, but nothing compared to the joy of this grand opening." },
        { "alliance", "ishaali", "welcome_what", "The Trading Post exists to serve, as the Light commands. When the auction house runs out of materials, ore, herbs, cloth, leather and gems, or crafted supplies, potions, food, scrolls and cut gems, we provide what is missing. Only what is missing. We do not wish to take bread from another's table." },
        { "alliance", "ishaali", "welcome_what", "You will find us at every auctioneer, in every auction house. Speak to them and choose the Trading Post. The auction window will open and show our goods, only those the auction house has run out of. The Light is everywhere, and now so are we." },
        { "alliance", "ishaali", "welcome_what", "I must confess, for the Light loves honesty: our prices are shamelessly high, and we are proud of it. Rare goods carried across great distances carry a great price. Consider it a small sacrifice, and sacrifice is good for the soul." },
        { "alliance", "ishaali", "welcome_what", "Materials and crafted supplies, whatever the auction house lacks, offered at any auctioneer through the Trading Post. The prices are high, I will not pretend otherwise. But what is rare is precious, and what is precious is a blessing. An expensive blessing." },
        { "alliance", "ishaali", "welcome_how", "Hear the ways of the post. Our stock is limited each day, shared among all. Each customer may buy one lot of a thing per day, that all may receive. Buy outright and the goods are in your mailbox at once. Bid for less, and our caravan brings them when the auction ends, within a day." },
        { "alliance", "ishaali", "welcome_how", "While stocks last, the Light provides. One lot of each thing per customer per day, no more, for greed dims the soul. Pay the buyout and your mailbox receives the goods at once. Place the humbler bid, and the caravan delivers when the auction ends." },
        { "alliance", "ishaali", "welcome_how", "From this day forward, a flyer with the day's offers will come to you each day, like a morning prayer. If you wish for silence, ask any auctioneer to stop sending you flyers, and I will pray for you instead. Remember: limited stock, one lot per day, buy outright or bid and await the caravan." },
        { "alliance", "ishaali", "welcome_how", "In short, as the Naaru would say it, if the Naaru said things: limited stock each day for all. One lot of a thing per customer and day. Buyout to your mailbox at once, or a cheaper bid delivered by caravan when the auction ends. And a daily flyer, which any auctioneer can stop." },
        { "alliance", "ishaali", "unsubscribe", "I suspect this flyer will find its way to the bin. The Light forgives you. To stop them, tell your auctioneer: Stop sending me your flyers." },
        { "alliance", "ishaali", "unsubscribe", "If these flyers try your patience, know that patience is a virtue. Or ask any auctioneer to stop sending you flyers. Also a virtue." },
        { "alliance", "ishaali", "unsubscribe", "May the Light grant you peace, even from this flyer. Your local auctioneer can stop the flyers for you. I will not be offended. Truly." },
        { "alliance", "ishaali", "unsubscribe", "Every flyer is sent with a blessing, even the ones you throw away. If you would rather not receive them, any auctioneer can stop them." },
        { "goblin", "gizzik", "welcome_subject", "GRAND OPENING! Buy now, ask later!" },
        { "goblin", "gizzik", "welcome_subject", "Trading Post OPEN! Wallets ready?" },
        { "goblin", "gizzik", "welcome_subject", "Biggest opening EVER! Read this!" },
        { "goblin", "gizzik", "welcome_opening", "{player}, pal, friend, valued customer! Gizzik Sharpcoin here, Sales Manager of the {post}! Guess what's open? The {post}! Grand opening, fireworks, the works! And between you and me, the best deals go to folks who read this letter all the way to the end. Keep reading!" },
        { "goblin", "gizzik", "welcome_opening", "Hey there, {player}! Gizzik Sharpcoin, Sales Manager, at your service and at your wallet! The {post} is officially OPEN, and let me tell you, I have never been this excited about anything. Except money. This is about money. So I'm VERY excited." },
        { "goblin", "gizzik", "welcome_opening", "Listen up, {player}! This is Gizzik Sharpcoin, Sales Manager of the {post}, with the deal of a lifetime: the {post} is open! Did I say deal of a lifetime? I meant deals. Plural. Lots of them. All for you. Well, all for sale to you." },
        { "goblin", "gizzik", "welcome_opening", "{player}! My favorite customer! Don't tell the others. Gizzik Sharpcoin here, Sales Manager. The {post} just opened, and you're one of the very first to know! Act fast, buy big, and remember: Gizzik never forgets a good customer. Or a bad one." },
        { "goblin", "gizzik", "welcome_what", "Here's the pitch! The auction house ran out of ore? Herbs? Potions? Cut gems? Tragic! But the Trading Post has 'em! Materials and crafted supplies, everything the auction house can't give you, right when you need it most. And you need it, trust me. You need more than you think." },
        { "goblin", "gizzik", "welcome_what", "Where do you find this marvel? EVERY auctioneer in EVERY auction house! Just walk up, choose the Trading Post, and boom, the auction window opens with all the goods the auction house ran out of. Ore, herbs, cloth, leather, gems, potions, food, scrolls! Buy 'em all!" },
        { "goblin", "gizzik", "welcome_what", "Are we expensive? Pal, we are SHAMELESSLY expensive, and proud of it! You think quality comes cheap? You think convenience comes cheap? Nothing comes cheap! That's the beauty of it! The more you pay, the better you feel. Science." },
        { "goblin", "gizzik", "welcome_what", "Materials, crafted supplies, all the stuff the auction house is fresh out of, available at any auctioneer through the Trading Post option. Premium prices for premium desperation! And while you're there, buy something extra. You'll thank me later. Everybody does." },
        { "goblin", "gizzik", "welcome_how", "Now here's the deal! Limited stock every day, same stock for everybody, so hurry hurry hurry! One lot of a thing per customer per day, which means you should buy one of EVERYTHING. Buy outright and it's in your mailbox at once. Or bid for less and the caravan brings it when the auction ends." },
        { "goblin", "gizzik", "welcome_how", "While stocks last, friend! And they don't last! One lot of each thing per customer per day. Buyout? Mailbox, instantly, like magic but more expensive. Bid? A little cheaper, and the caravan delivers when the auction ends. Me, I'd buy out. Why wait? Life is short. Wallets are long." },
        { "goblin", "gizzik", "welcome_how", "And the best part: starting now, I'll send you a flyer EVERY DAY with the day's offers! Every day a new chance to spend! If you're somehow not into that, any auctioneer can stop the flyers. But why would you? Remember: limited stock, one lot each per day, buy outright or bid." },
        { "goblin", "gizzik", "welcome_how", "Quick rundown, because time is money: limited daily stock for all. One lot of a thing per customer and day. Buy outright, mailbox at once. Bid lower, caravan delivers when the auction ends. Plus a daily flyer full of offers! You can stop it at any auctioneer. You won't, though." },
        { "goblin", "gizzik", "unsubscribe", "This flyer's going straight in the bin, huh? Fine! But read the offers first! If you really want out, tell any auctioneer: Stop sending me your flyers." },
        { "goblin", "gizzik", "unsubscribe", "Annoyed by daily flyers? I get it! Tell your auctioneer to stop sending you flyers. But you'll miss the deals, pal. The DEALS." },
        { "goblin", "gizzik", "unsubscribe", "Sure, toss this flyer. But first, glance at the offers. Just a peek. Okay, fine, any auctioneer can stop the flyers for you. Your loss!" },
        { "goblin", "gizzik", "unsubscribe", "If this flyer bugs you, your local auctioneer can stop them. But think of all the deals you'll miss! Gizzik weeps. Gizzik weeps in gold." },
        { "goblin", "fenny", "welcome_subject", "Grand opening! (Fees may apply)" },
        { "goblin", "fenny", "welcome_subject", "The Trading Post is open. Fees too." },
        { "goblin", "fenny", "welcome_subject", "Notice of opening and related fees" },
        { "goblin", "fenny", "welcome_opening", "Dear {player}, Fenny Tollwhistle here, Fee Accountant of the {post}. I am delighted to announce that the {post} is now open! The grand opening itself was free of charge. Reading this letter is also free of charge. For now. I am looking into it." },
        { "goblin", "fenny", "welcome_opening", "Greetings, {player}! This is Fenny Tollwhistle, Fee Accountant. The {post} has opened its doors, and I have opened a ledger of fees to celebrate. Door opening fee, ribbon cutting fee, celebration fee. All perfectly reasonable. All perfectly mandatory." },
        { "goblin", "fenny", "welcome_opening", "{player}, wonderful news from the {post}! I'm Fenny Tollwhistle, and I count the fees. The {post} is officially open! There was a band, which had a fee, and fireworks, which had a fee, and a speech, which had a surcharge. What a party!" },
        { "goblin", "fenny", "welcome_opening", "Hello, {player}! Fenny Tollwhistle, Fee Accountant of the {post}, writing to proclaim our grand opening. The {post} is open for business, and business means fees, and fees mean joy. My joy, specifically. I hope you share it. There may be a sharing fee." },
        { "goblin", "fenny", "welcome_what", "What does the Trading Post do? It sells what the auction house has run out of: ore, herbs, cloth, leather, gems, potions, food, scrolls, cut gems. Materials and crafted supplies, delivered with care, handling, and the appropriate fees. Which fees? The appropriate ones." },
        { "goblin", "fenny", "welcome_what", "You will find the Trading Post at every auctioneer in every auction house. Choose the Trading Post when you talk to them and the auction window opens with our goods, only the ones the auction house lacks. There is no fee for looking. I checked. Twice. Sadly." },
        { "goblin", "fenny", "welcome_what", "Our prices are shamelessly high, and we are proud of it. They include the base price, the rarity fee, the convenience fee, the caravan fee, and a small fee for calculating the fees. I won't name the amounts. Numbers upset people. Fees, on the other hand, are lovely." },
        { "goblin", "fenny", "welcome_what", "Materials and crafted supplies the auction house has run out of, available through the Trading Post at any auctioneer. Everything is very expensive, and every fee is fully explained in the fee schedule, which is available upon request. For a fee." },
        { "goblin", "fenny", "welcome_how", "The rules are simple, and mostly fee-free. Stock is limited each day, shared by everybody. One lot of a thing per customer per day. Buy outright, including all fees, and the goods are in your mailbox at once. Or bid for less, fewer fees, and the caravan brings them when the auction ends." },
        { "goblin", "fenny", "welcome_how", "While stocks last! Each customer may buy one lot of each thing per day. The buyout includes the instant mailbox delivery fee, so your goods arrive at once. The cheaper bid includes the slower caravan fee, and the goods come when the auction ends, within a day. Everyone pays something. Lovely." },
        { "goblin", "fenny", "welcome_how", "From now on, you will receive a daily flyer with the day's offers. The flyer itself is free. I argued against that, but I lost. If you wish to stop the flyers, simply ask any auctioneer. No cancellation fee. Yet. Remember: limited stock, one lot per day, buy outright or bid and wait for the caravan." },
        { "goblin", "fenny", "welcome_how", "Summary of terms: limited stock each day for all. One lot of a thing per customer and day. Buyout means mailbox at once, with the express fee. Bid means cheaper, with the caravan fee, delivered when the auction ends. Daily flyers included, and any auctioneer can stop them, free of charge, regrettably." },
        { "goblin", "fenny", "unsubscribe", "I know, this flyer is going in the bin. There is no bin fee. I checked. Tell your auctioneer \"Stop sending me your flyers\" to cancel, also free. Sadly." },
        { "goblin", "fenny", "unsubscribe", "If this flyer annoys you, any auctioneer can stop them, no cancellation fee. I am working on that, but the lawyers say no." },
        { "goblin", "fenny", "unsubscribe", "Feel free to throw this flyer away. Disposal is free. Unsubscribing at your local auctioneer is free too. I hate that, honestly." },
        { "goblin", "fenny", "unsubscribe", "Tired of flyers? Your auctioneer can stop them at no charge. I proposed an unsubscription fee, but apparently that is \"evil\"." },
        { "goblin", "krazzle", "welcome_subject", "KABOOM! The Trading Post is OPEN!" },
        { "goblin", "krazzle", "welcome_subject", "Grand opening with a BANG!" },
        { "goblin", "krazzle", "welcome_subject", "We're open! (Mind the crater)" },
        { "goblin", "krazzle", "welcome_opening", "{player}! Krazzle Boomgear here, Dispatch Boss of the {post}! We opened the {post} today with a BANG! Literally. We blew the doors off. We're getting new doors. Until then, the {post} is very, very open! Grand opening! KABOOM!" },
        { "goblin", "krazzle", "welcome_opening", "Hey, {player}! It's Krazzle Boomgear, Dispatch Boss! Big news: the {post} is open for business! I set off the celebration rockets myself. Most of them went up. The rest went sideways. Nobody's hurt, mostly. What a party!" },
        { "goblin", "krazzle", "welcome_opening", "BOOM, {player}! That's the sound of the {post} opening! Krazzle Boomgear writing, Dispatch Boss, master of fast delivery and faster fuses. The grand opening was a blast. Ha! Get it? A blast! Seriously, we lost a wall." },
        { "goblin", "krazzle", "welcome_opening", "{player}, Krazzle Boomgear, Dispatch Boss of the {post}, here with explosive news! The {post} has opened! We celebrated with fireworks, rocket wagons, and a small accidental detonation of the snack table. The snacks were delicious. Briefly." },
        { "goblin", "krazzle", "welcome_what", "What's the Trading Post? It's the place with the stuff the auction house doesn't have! Ore, herbs, cloth, leather, gems, potions, food, scrolls, cut gems! When the market runs dry, we fire the goods straight at you. Figuratively. Mostly. Only what the auction house has run out of!" },
        { "goblin", "krazzle", "welcome_what", "Where to find us? EVERY auctioneer in EVERY auction house! Walk up, choose the Trading Post, and BOOM, the auction window pops open with our goods. Only stuff the auction house ran out of. No searching, no blasting required. Though blasting is always an option." },
        { "goblin", "krazzle", "welcome_what", "Prices? Sky high! Rocket high! Shamelessly expensive and proud of it! You know what rocket fuel costs these days? And dispatch? And replacement wagons, after the dispatch? It all adds up, pal. Explosively." },
        { "goblin", "krazzle", "welcome_what", "Materials and crafted supplies, everything the auction house is out of, at any auctioneer through the Trading Post. Expensive? Like a crate of blasting powder! Worth it? Like a crate of blasting powder! Wait, that came out wrong." },
        { "goblin", "krazzle", "welcome_how", "Here's how it works! Limited stock each day, same for everybody, so move fast! One lot of a thing per customer per day. Buy outright and BOOM, it's in your mailbox at once! Bid for less and my caravan rolls it over when the auction ends, within a day. Usually in one piece." },
        { "goblin", "krazzle", "welcome_how", "While stocks last, folks! One lot of each thing per customer per day. Pay the buyout and the goods land in your mailbox at once, like a perfectly aimed rocket. Place a cheaper bid and the caravan delivers when the auction ends. Slower, but fewer craters." },
        { "goblin", "krazzle", "welcome_how", "And from now on, a flyer with the day's offers will land in your mailbox every day! Don't worry, they don't explode. Usually. If you'd rather not, any auctioneer can stop the flyers. Remember: limited stock, one lot per day, buy outright for instant delivery, or bid and wait for the caravan." },
        { "goblin", "krazzle", "welcome_how", "Quick and loud: limited daily stock for everybody. One lot of a thing per customer and day. Buyout lands in your mailbox at once. Bid is cheaper, caravan delivers when the auction ends. Daily flyer included, and if it bugs you, any auctioneer can stop it. Boom. Done." },
        { "goblin", "krazzle", "unsubscribe", "This flyer's headed for the bin? Fine, but stand back first. Kidding! To stop them, tell your auctioneer: Stop sending me your flyers." },
        { "goblin", "krazzle", "unsubscribe", "Annoyed by flyers? Me too. I prefer rockets. Any auctioneer can stop these flyers if you ask nicely. Or loudly. Loudly works." },
        { "goblin", "krazzle", "unsubscribe", "Toss this flyer, burn it, blow it up, I won't judge. Or tell your local auctioneer to stop sending you flyers. Less paper, more boom." },
        { "goblin", "krazzle", "unsubscribe", "If this flyer makes you want to explode, don't! That's my job. Just ask any auctioneer to stop sending you flyers." },
        { "goblin", "nixa", "welcome_subject", "Notice: Grand Opening (Terms Apply)" },
        { "goblin", "nixa", "welcome_subject", "Official Notice of Commencement" },
        { "goblin", "nixa", "welcome_subject", "Trading Post open. See fine print." },
        { "goblin", "nixa", "welcome_opening", "Dear {player}, hereinafter \"the Customer\". I, Nixa Fastfingers, Contracts Clerk of the {post}, hereinafter \"the Post\", hereby formally announce that the {post} is open for business, effective immediately, subject to terms, conditions and festivities." },
        { "goblin", "nixa", "welcome_opening", "To {player}, greetings. This is Nixa Fastfingers, Contracts Clerk. Be advised that the {post} has opened its doors. This announcement constitutes fanfare within the meaning of the Grand Opening Clause. Hooray, as defined in the appendix." },
        { "goblin", "nixa", "welcome_opening", "{player}, this letter serves as official notice: the {post} is now open. I am Nixa Fastfingers, Contracts Clerk, and I drafted this announcement myself. The celebratory tone of this letter is non-binding and may be withdrawn at any time." },
        { "goblin", "nixa", "welcome_opening", "Attention, {player}. Nixa Fastfingers, Contracts Clerk of the {post}, writing on behalf of all parties. The {post} has commenced operations, and the Customer is hereby invited to rejoice. Rejoicing is voluntary but strongly recommended." },
        { "goblin", "nixa", "welcome_what", "Purpose of the Trading Post, for the record: the Post supplies goods that the auction house has run out of, namely materials, such as ore, herbs, cloth, leather and gems, and crafted supplies, such as potions, food, scrolls and cut gems. Nothing more, nothing less, nothing refundable." },
        { "goblin", "nixa", "welcome_what", "Location clause: the Trading Post is available at every auctioneer of every auction house. The Customer shall speak to the auctioneer and choose the Trading Post, whereupon the auction window opens with the goods of the Post, being only those the auction house has run out of." },
        { "goblin", "nixa", "welcome_what", "Pricing clause: the Customer acknowledges that prices are shamelessly high, and that the Post is proud of it. Such pride is not grounds for complaint. Complaints are accepted in writing, then filed, then forgotten, in that order." },
        { "goblin", "nixa", "welcome_what", "In plain words, which I use reluctantly: materials and crafted supplies the auction house lacks, available at any auctioneer through the Trading Post option. Prices are high by design. By reading this sentence, the Customer agrees that this was explained clearly." },
        { "goblin", "nixa", "welcome_how", "Terms of sale. The stock is limited each day and shared by all customers. Each Customer may purchase one lot of a thing per day. Upon buyout, goods are delivered to the mailbox at once. Upon a bid, at a lower price, goods are delivered by caravan when the auction ends, within a day." },
        { "goblin", "nixa", "welcome_how", "Article on availability: goods are sold while stocks last. Limit of one lot of each thing per Customer per day. Buyout delivery: immediate, to the mailbox. Bid delivery: by caravan, upon the end of the auction, within a day. All deliveries are final. All finality is also final." },
        { "goblin", "nixa", "welcome_how", "Subscription clause: the Customer shall henceforth receive a daily flyer with the offers of the day. The Customer may terminate this subscription at any auctioneer by requesting that the flyers stop. Recap: limited stock, one lot per day, buyout to mailbox, or bid and await the caravan." },
        { "goblin", "nixa", "welcome_how", "Summary of terms: limited daily stock for all; one lot of a thing per Customer and day; buyout delivered to the mailbox at once; bid, at a lower price, delivered by caravan when the auction ends. A daily flyer is included, cancellable at any auctioneer. See fine print. There is always fine print." },
        { "goblin", "nixa", "unsubscribe", "The Customer is likely to dispose of this flyer unread. The Post accepts this. To terminate, tell any auctioneer: Stop sending me your flyers." },
        { "goblin", "nixa", "unsubscribe", "Per the cancellation clause, the Customer may stop these flyers at any auctioneer. Binning this flyer does not count as cancellation." },
        { "goblin", "nixa", "unsubscribe", "Notice: annoyance caused by this flyer is not covered by any guarantee. Remedy: ask your local auctioneer to stop sending you flyers." },
        { "goblin", "nixa", "unsubscribe", "By throwing this flyer away, the Customer waives nothing. To actually stop the flyers, inform any auctioneer. Fine print ends here." },
        { "horde", "any", "lastseen", "- {item}: {seller} sold it next door {when} for {price}. A fool's price. Ours is {ours} now, or {caravan} if you trust the caravan." },
        { "horde", "any", "lastseen", "- {item}: the auction house had it {when}, {price} from {seller}. We charge {ours} on the spot, {caravan} by caravan. Quality costs, grunt." },
        { "horde", "any", "lastseen", "- {item}: last spotted {when} at {price}, posted by {seller}. Gone now. Ours waits for you at {ours}, or {caravan} with the next caravan." },
        { "horde", "any", "lastseen", "- {item}: {seller} let it go for {price} {when}. Such generosity is not our way. {ours} here, {caravan} if you can wait for the wolves." },
        { "horde", "any", "lastseen", "- {item}: seen next door {when} - {price} each, seller {seller}. Our price: {ours} today, {caravan} when the kodos arrive. Patience is cheaper." },
        { "horde", "any", "lastseen", "- {item}: {when}, {seller} asked {price} for it at the auction house. We ask {ours}. Or {caravan}, delivered by caravan. Honor has a price tag." },
        { "horde", "any", "lastseen", "- {item}: the clerks next door saw it {when} at {price} from {seller}. Here it costs {ours} at once, {caravan} by caravan. Do not haggle." },
        { "horde", "any", "lastseen", "- {item}: {price} a piece from {seller}, {when}. That was then. This is the Trading Post: {ours} now, {caravan} on the caravan." },
        { "horde", "any", "lastseen", "- {item}: last on the auction board {when}, {seller} wanted {price}. Our counter says {ours}, the caravan says {caravan}. Both are final." },
        { "horde", "any", "lastseen", "- {item}: {seller} sold it for {price} {when} and probably regrets nothing. We charge {ours}, or {caravan} by caravan, and regret nothing either." },
        { "horde", "any", "neverseen", "- {item}: never seen at the auction house in living memory. Here: {ours} at once, or {caravan} with the caravan." },
        { "horde", "any", "neverseen", "- {item}: the auction house has no record of it. None. We have it for {ours}, or {caravan} by caravan." },
        { "horde", "any", "neverseen", "- {item}: not one sighting next door since the clerks started counting. Ours: {ours}, or {caravan} if you can wait." },
        { "horde", "any", "neverseen", "- {item}: the auctioneer swears it never passed his counter. Ours costs {ours} today, {caravan} with the next caravan." },
        { "horde", "any", "neverseen", "- {item}: nobody has ever posted this next door. Rare things cost. {ours} now, {caravan} when the kodos arrive." },
        { "horde", "any", "neverseen", "- {item}: unseen at the auction house since the founding of the city. Here: {ours} on the spot, {caravan} by caravan." },
        { "horde", "any", "neverseen", "- {item}: the elders cannot remember it on the auction board. We can sell it for {ours}, or {caravan} with the caravan." },
        { "horde", "any", "neverseen", "- {item}: never listed next door. Not once. Ours waits at {ours}, or {caravan} if the caravan survives the road." },
        { "horde", "any", "neverseen", "- {item}: even the oldest auction ledgers are silent about it. Here: {ours}, or {caravan} delivered later." },
        { "horde", "any", "neverseen", "- {item}: the auction house has never seen one. We have. Pay {ours} today, or {caravan} and wait for the caravan." },
        { "horde", "grukka", "misers", "For the misers among you. The Quartermaster writes this with clenched teeth. Cheapest goods at the auction house next door:" },
        { "horde", "grukka", "misers", "Grukka does not like this list. Honor demands it anyway. The cheapest bargains next door, for the misers:" },
        { "horde", "grukka", "misers", "Misers. You know who you are. These are the cheapest offers at the auction house right now. Do not thank me." },
        { "horde", "grukka", "misers", "It pains me to write this. The auction house next door sells these cheap. Take them, misers, and be gone." },
        { "horde", "grukka", "hurry", "Move fast. First come, first served. The bots buy cheap things themselves." },
        { "horde", "grukka", "hurry", "These bargains will not wait. Neither will the bots. Go now, or lose them." },
        { "horde", "grukka", "hurry", "First come, first served. Hesitate, and a bot takes it. That is the way of war." },
        { "horde", "grukka", "hurry", "Cheap goods do not last. The bots are faster than you. Run." },
        { "horde", "zuljabi", "misers", "For de misers among you, mon. Zul'jabi be cryin' while he write dis: de cheapest bargains next door." },
        { "horde", "zuljabi", "misers", "Ah, de misers! Dis list hurt me heart, mon. Here be de cheapest things at de auction house right now." },
        { "horde", "zuljabi", "misers", "It pains me to write dis, mon. But de spirits say share, so here be de cheapest stuff next door:" },
        { "horde", "zuljabi", "misers", "You too cheap for de Trading Post? No worries, mon. Dat auction house got dese bargains for ya:" },
        { "horde", "zuljabi", "hurry", "Hurry, mon! First come, first served. De bots be snatchin' de cheap stuff demselves." },
        { "horde", "zuljabi", "hurry", "Dese bargains no gonna wait for ya, mon. Run fast before de bots grab dem!" },
        { "horde", "zuljabi", "hurry", "First come, first served, mon. De bots love a cheap deal more dan anybody." },
        { "horde", "zuljabi", "hurry", "Go go go, mon! Dat cheap stuff vanish faster dan a troll at tax time." },
        { "horde", "thalsorn", "misers", "For the misers among you. Even the river sometimes runs shallow. These are the cheapest bargains at the auction house next door:" },
        { "horde", "thalsorn", "misers", "It pains me to write this, as a drought pains the plains. The cheapest offers next door, for those who count each copper:" },
        { "horde", "thalsorn", "misers", "The Ledger-Keeper bows to the thrifty. Like seeds scattered by wind, these cheap offers lie next door:" },
        { "horde", "thalsorn", "misers", "Not every traveler can afford the Trading Post. For the misers, the earth offers these bargains at the auction house:" },
        { "horde", "thalsorn", "hurry", "Hurry. First come, first served. The bots graze on cheap goods like hungry kodos." },
        { "horde", "thalsorn", "hurry", "Bargains melt like spring snow. The bots will gather them if you do not." },
        { "horde", "thalsorn", "hurry", "First come, first served. The wind does not wait, and neither do the bots." },
        { "horde", "thalsorn", "hurry", "Move swiftly, friend. Cheap things are taken before the sun sets." },
        { "horde", "ambrose", "misers", "For the misers among you. Writing this list is more painful than my own death was. The cheapest bargains next door:" },
        { "horde", "ambrose", "misers", "It pains me to write this, and I no longer feel pain. The auction house next door offers these, for the thrifty:" },
        { "horde", "ambrose", "misers", "Misers, rejoice. Briefly. Here are the cheapest offers next door, listed with all the enthusiasm of a gravedigger." },
        { "horde", "ambrose", "misers", "The Shipping Clerk grudgingly exhumes the cheapest bargains at the auction house next door, for the stingy among you:" },
        { "horde", "ambrose", "hurry", "Hurry. First come, first served. The bots buy cheap things themselves, and they never sleep either." },
        { "horde", "ambrose", "hurry", "These bargains will not wait. They will die young, like most things." },
        { "horde", "ambrose", "hurry", "First come, first served. The bots descend on cheap goods like crows on a battlefield." },
        { "horde", "ambrose", "hurry", "Move along quickly. Unlike me, the bargains do not last forever." },
        { "alliance", "any", "lastseen", "- {item}: last seen at the auction house {when}, {price} each from {seller}. Our humble price: {ours} at once, {caravan} by caravan." },
        { "alliance", "any", "lastseen", "- {item}: {seller} parted with it {when} for a mere {price}. A tragic lack of ambition. Ours: {ours} now, {caravan} with the caravan." },
        { "alliance", "any", "lastseen", "- {item}: the auctioneer recorded it {when} at {price}, sold by {seller}. Kindly compare with our {ours}, or {caravan} by wagon." },
        { "alliance", "any", "lastseen", "- {item}: {when}, {price} a piece from {seller}. Alas, sold out. The Trading Post offers {ours} today, or {caravan} with the caravan." },
        { "alliance", "any", "lastseen", "- {item}: listed next door {when} by {seller} at {price}. Here it is {ours} immediately, {caravan} when the caravan rolls in. Splendid value." },
        { "alliance", "any", "lastseen", "- {item}: {seller} asked only {price} for it {when}. We ask {ours}. Or {caravan}, should you prefer to wait for the caravan." },
        { "alliance", "any", "lastseen", "- {item}: last seen {when} at the auction house - {price} each, from {seller}. Ours: {ours} at once, {caravan} by caravan. Service has a price." },
        { "alliance", "any", "lastseen", "- {item}: {price} from {seller}, {when}. Lucky buyer. Unlucky you. Our offer: {ours} today, {caravan} via the caravan." },
        { "alliance", "any", "lastseen", "- {item}: according to the auction ledger, {seller} sold it {when} for {price}. Our ledger says {ours}, or {caravan} with the caravan." },
        { "alliance", "any", "lastseen", "- {item}: seen next door {when} for {price}, courtesy of {seller}. Here: {ours} on the spot, {caravan} by caravan. Worth every copper." },
        { "alliance", "any", "neverseen", "- {item}: not seen at the auction house in living memory. Here: {ours}, or {caravan} with the caravan." },
        { "alliance", "any", "neverseen", "- {item}: the auction house has never listed it. We, naturally, have. {ours} at once, {caravan} by caravan." },
        { "alliance", "any", "neverseen", "- {item}: no record of it next door, not in any ledger. Ours: {ours} today, or {caravan} with the caravan." },
        { "alliance", "any", "neverseen", "- {item}: never once on the auction board. A true rarity at {ours}, or {caravan} if you can wait for the caravan." },
        { "alliance", "any", "neverseen", "- {item}: the auctioneer has never laid eyes on it. You may, for {ours} now or {caravan} via the caravan." },
        { "alliance", "any", "neverseen", "- {item}: unseen at the auction house since anyone started keeping books. Here: {ours}, or {caravan} by caravan." },
        { "alliance", "any", "neverseen", "- {item}: no seller has ever posted it next door. Our price: {ours} at once, {caravan} when the caravan arrives." },
        { "alliance", "any", "neverseen", "- {item}: absent from the auction house as long as anyone remembers. Ours: {ours} today, {caravan} delivered later." },
        { "alliance", "any", "neverseen", "- {item}: the auction clerks claim it is a myth. We sell the myth for {ours}, or {caravan} with the caravan." },
        { "alliance", "any", "neverseen", "- {item}: never seen next door, not even rumored. Here it costs {ours}, or {caravan} by the slow caravan." },
        { "alliance", "percival", "misers", "For the misers among you, and it pains me deeply to write this: the cheapest bargains at the auction house next door." },
        { "alliance", "percival", "misers", "The Steward must, under protest, acknowledge the existence of cheap goods. They are next door. For the misers:" },
        { "alliance", "percival", "misers", "Should you lack the refinement to shop with us, the auction house next door offers these regrettable bargains:" },
        { "alliance", "percival", "misers", "It is with the heaviest of hearts that I list the cheapest offers next door. Misers, please lower your eyes as you read:" },
        { "alliance", "percival", "hurry", "Do hurry. First come, first served, and the bots have no manners whatsoever." },
        { "alliance", "percival", "hurry", "These bargains will not wait for your leisurely stroll. The bots certainly will not." },
        { "alliance", "percival", "hurry", "First come, first served. One does not dawdle where cheap goods and bots are concerned." },
        { "alliance", "percival", "hurry", "Make haste, good people. The bots snap up cheap things with appalling speed." },
        { "alliance", "tibbly", "misers", "For the misers among you. Tibbly has triple-checked this list and still hates it. Cheapest bargains next door:" },
        { "alliance", "tibbly", "misers", "It pains me to write this, and it ruins my lovely margins. The cheapest offers at the auction house next door:" },
        { "alliance", "tibbly", "misers", "Entry under protest: cheapest bargains at the auction house next door, for misers. Filed, sorted, regretted." },
        { "alliance", "tibbly", "misers", "Against all accounting principles, the Bookkeeper lists the cheapest auction house offers. For the misers only:" },
        { "alliance", "tibbly", "hurry", "Hurry! First come, first served. The bots buy cheap things themselves, alphabetically and fast." },
        { "alliance", "tibbly", "hurry", "These bargains will not wait. By my calculations, the bots get there first." },
        { "alliance", "tibbly", "hurry", "First come, first served. Late arrivals will be noted in the ledger. In red ink." },
        { "alliance", "tibbly", "hurry", "Quickly now! Cheap goods vanish faster than a rounding error." },
        { "alliance", "durgan", "misers", "For the misers among you. Durgan needs a stiff ale after writing this. The cheapest bargains next door:" },
        { "alliance", "durgan", "misers", "Bah. It pains me to write this. The auction house next door sells these cheap. Go on then, misers." },
        { "alliance", "durgan", "misers", "Misers, listen up. These cheap offers next door are the only reason I'm not sober today." },
        { "alliance", "durgan", "misers", "Grumble, grumble. Here be the cheapest bargains at the auction house, for those who drink water to save coin:" },
        { "alliance", "durgan", "hurry", "Hurry up! First come, first served. The bots grab cheap things faster than I grab a tankard." },
        { "alliance", "durgan", "hurry", "These bargains won't wait, and neither will the bots. Off with ye." },
        { "alliance", "durgan", "hurry", "First come, first served. Dawdle and a bot drinks your ale. So to speak." },
        { "alliance", "durgan", "hurry", "Move yer feet! Cheap goods go quick, like the first keg of the night." },
        { "alliance", "ishaali", "misers", "For the misers among you. The Light teaches charity, so I write this, though it pains me. The cheapest bargains next door:" },
        { "alliance", "ishaali", "misers", "It pains me to write this, yet the Light asks for honesty. The auction house next door offers these at humble prices:" },
        { "alliance", "ishaali", "misers", "May the Light forgive me for this list. For the thrifty souls, the cheapest offers at the auction house next door:" },
        { "alliance", "ishaali", "misers", "Even the frugal deserve guidance. With a heavy heart, I present the cheapest bargains next door:" },
        { "alliance", "ishaali", "hurry", "Hurry, friends. First come, first served. The bots buy cheap things themselves, without prayer." },
        { "alliance", "ishaali", "hurry", "These bargains will not wait. The Light guides the swift, and so do the bots." },
        { "alliance", "ishaali", "hurry", "First come, first served. Do not tarry, for patience is not rewarded here." },
        { "alliance", "ishaali", "hurry", "Move with purpose. Cheap blessings are claimed quickly, mostly by bots." },
        { "goblin", "any", "lastseen", "- {item}: last seen next door {when}, {price} per piece from {seller}. Here: {ours} instantly, {caravan} with the caravan. Plus fees. Kidding. Mostly." },
        { "goblin", "any", "lastseen", "- {item}: {seller} dumped it {when} for {price}. Bad business! We sell at {ours} now, or {caravan} on the caravan. Good business!" },
        { "goblin", "any", "lastseen", "- {item}: the auction house had it {when} at {price}, seller {seller}. Our price: {ours} express, {caravan} economy caravan." },
        { "goblin", "any", "lastseen", "- {item}: {when}, {seller} let it go for {price}. Rookie. Ours: {ours} right now, {caravan} by caravan. Time is money, friend." },
        { "goblin", "any", "lastseen", "- {item}: last listing next door: {price} from {seller}, {when}. Our listing: {ours} at once, {caravan} by caravan. Spot the professional." },
        { "goblin", "any", "lastseen", "- {item}: {seller} sold it for {price} {when}. We call that a missed opportunity. We call {ours} an opportunity. Caravan price: {caravan}." },
        { "goblin", "any", "lastseen", "- {item}: seen at the auction house {when} at {price} via {seller}. Here: {ours} today, {caravan} with the caravan. Markup is an art." },
        { "goblin", "any", "lastseen", "- {item}: {price} each from {seller}, {when}. Sold out, sucker. Ours: {ours} immediate, {caravan} when the caravan shows up." },
        { "goblin", "any", "lastseen", "- {item}: our market spies saw it {when} for {price}, from {seller}. Our sticker says {ours}, caravan sticker says {caravan}. No refunds." },
        { "goblin", "any", "lastseen", "- {item}: {seller} sold it {when} at {price} and walked away happy. We charge {ours}, or {caravan} by caravan, and walk away happier." },
        { "goblin", "any", "neverseen", "- {item}: never seen at the auction house in living memory. Here: {ours} express, or {caravan} by caravan." },
        { "goblin", "any", "neverseen", "- {item}: zero listings next door, ever. Scarcity is beautiful. {ours} now, {caravan} with the caravan." },
        { "goblin", "any", "neverseen", "- {item}: the auction house never had one. We do. Monopoly price: {ours}, or {caravan} via caravan." },
        { "goblin", "any", "neverseen", "- {item}: not a single sighting next door. Exclusive deal: {ours} at once, {caravan} when the caravan rolls in." },
        { "goblin", "any", "neverseen", "- {item}: our spies watched the auction house for ages. Nothing. Ours: {ours}, or {caravan} by caravan." },
        { "goblin", "any", "neverseen", "- {item}: unseen next door since before the first coin was minted. Here: {ours} today, {caravan} with the caravan." },
        { "goblin", "any", "neverseen", "- {item}: no auction record, no competition, no mercy. {ours} right now, or {caravan} on the caravan." },
        { "goblin", "any", "neverseen", "- {item}: the auction house does not even know it exists. Yours for {ours}, or {caravan} with the caravan." },
        { "goblin", "any", "neverseen", "- {item}: never listed next door. Supply and demand says {ours}. The caravan says {caravan}." },
        { "goblin", "any", "neverseen", "- {item}: absent from the auction house in living memory. Rare goods, rare prices: {ours}, or {caravan} delivered later." },
        { "goblin", "gizzik", "misers", "For the misers among you. Gizzik writes this through tears. Cheapest bargains next door - but have you seen our premium line?" },
        { "goblin", "gizzik", "misers", "It pains me to write this. These are the cheapest offers at the auction house. Upgrade to ours anytime, friend!" },
        { "goblin", "gizzik", "misers", "Sure, sure, you want cheap. Here are the cheapest bargains next door. Consider buying two of ours instead." },
        { "goblin", "gizzik", "misers", "Misers, welcome! Here is the bargain bin of the auction house. Ask me about our deluxe caravan package." },
        { "goblin", "gizzik", "hurry", "Hurry! First come, first served. The bots buy cheap stuff themselves. Ours never runs out!" },
        { "goblin", "gizzik", "hurry", "These bargains won't wait. Ours will. That's the premium experience." },
        { "goblin", "gizzik", "hurry", "First come, first served. Lose out to a bot? Our counter is always open, friend." },
        { "goblin", "gizzik", "hurry", "Quick, quick! Cheap goods vanish. Expensive goods wait patiently for you. Think about it." },
        { "goblin", "fenny", "misers", "For the misers among you. It pains me to write this list, since none of it carries a fee. The cheapest bargains next door:" },
        { "goblin", "fenny", "misers", "No fees, no surcharges, no joy. Here are the cheapest offers at the auction house next door, misers:" },
        { "goblin", "fenny", "misers", "The Fee Accountant reluctantly lists the cheapest bargains next door. Reading this list is free. For now." },
        { "goblin", "fenny", "misers", "Cheapest auction house offers, for misers. Note: the auction house deposit fee is not our fault. Ours is." },
        { "goblin", "fenny", "hurry", "Hurry. First come, first served. The bots buy cheap things themselves and dodge every fee." },
        { "goblin", "fenny", "hurry", "These bargains will not wait. Waiting fees may apply. Kidding. Probably." },
        { "goblin", "fenny", "hurry", "First come, first served. Late arrival surcharge: your dignity, lost to a bot." },
        { "goblin", "fenny", "hurry", "Move fast! Cheap goods vanish before I can even invent a fee for them." },
        { "goblin", "krazzle", "misers", "For the misers among you. Krazzle almost blew up the press writing this. The cheapest bargains next door:" },
        { "goblin", "krazzle", "misers", "It pains me to write this. More than that time with the gunpowder barrel. Cheapest offers at the auction house:" },
        { "goblin", "krazzle", "misers", "Misers! Here's the cheapest stuff next door. Light the fuse and run over there, I guess." },
        { "goblin", "krazzle", "misers", "Kaboom goes my profit. These are the cheapest bargains at the auction house next door, for the stingy:" },
        { "goblin", "krazzle", "hurry", "Hurry! First come, first served. The bots grab cheap stuff faster than a short fuse burns." },
        { "goblin", "krazzle", "hurry", "These bargains won't wait. They're gone quicker than a sapper charge. Boom." },
        { "goblin", "krazzle", "hurry", "First come, first served. Hesitate and a bot blasts right past you." },
        { "goblin", "krazzle", "hurry", "Go, go, go! The cheap stuff explodes off the shelves, mostly into bot bags." },
        { "goblin", "nixa", "misers", "For the misers among you. It pains me to write this. Cheapest auction house bargains next door, see fine print:" },
        { "goblin", "nixa", "misers", "Clause zero: the Trading Post does not endorse thrift. The cheapest offers at the auction house next door:" },
        { "goblin", "nixa", "misers", "Per this notice, misers are hereby informed of the cheapest bargains next door. Signed under duress:" },
        { "goblin", "nixa", "misers", "The Contracts Clerk lists these cheap offers next door against her will. Terms and conditions apply, somewhere:" },
        { "goblin", "nixa", "hurry", "Hurry. First come, first served. Fine print: the bots buy cheap things themselves." },
        { "goblin", "nixa", "hurry", "These bargains will not wait. Nothing in this contract obliges them to." },
        { "goblin", "nixa", "hurry", "First come, first served. Bots are not bound by any agreement and act accordingly." },
        { "goblin", "nixa", "hurry", "Act fast. Offer valid while supplies last, which is not long at all." },
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
            double value = 0.0;         // what one piece usually goes for among the bots
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
            uint32 house = 0;
            bool atPost = false;        // the auction window on its screen shows the post
        };

        /// Bought on a bid: paid, on its way, in the mailbox when the auction ends.
        struct Order
        {
            ObjectGuid::LowType player = 0;
            uint32 item = 0;
            uint32 count = 0;
            uint32 house = 0;
            uint32 sender = 0;          // the auctioneer, as the sender of the mail
            time_t due = 0;
        };

        std::mutex lock;        // the list is asked for on the thread of the player's map; everything below is behind this
        std::vector<Ware> wares;
        std::map<uint32, Stock> stocks;                                     // auction house -> shelves
        std::unordered_map<ObjectGuid::LowType, Visitor> visitors;
        std::unordered_map<uint64, uint32> bought;                          // player and item -> pieces, today
        std::unordered_map<uint64, uint32> sold;                            // auction house and item -> pieces, today
        uint32 today = 0;                                                   // the day the two above are about
        std::vector<Order> orders;
        struct Reader
        {
            uint32 day = 0;             // the last flyer
            bool off = false;           // does not want any
            uint32 sample = 0;          // the week the free sample was collected
            uint32 intro = 0;           // the mail that introduced the post; 0: not sent yet
            bool introRead = false;     // read (or gone): from then on the daily flyers come
        };
        std::map<uint64, uint64> spent;                                     // auction house and day -> copper players left there
        std::vector<uint32> junk;                                           // what a free sample can be

        /// When a thing was last seen in an auction house, for how much a piece and from whom.
        struct Sighting
        {
            time_t when = 0;
            uint32 each = 0;
            ObjectGuid::LowType seller = 0;
        };
        std::unordered_map<uint64, Sighting> seen;                          // auction house and item
        std::unordered_map<ObjectGuid::LowType, Reader> readers;
        bool tables = false;
        thread_local bool passing = false;      // the real auction window is being opened; the menu stays out of it

        uint32 DayOf(time_t when)
        {
            std::tm const time = Acore::Time::TimeBreakdown(when);
            return uint32(time.tm_year + 1900) * 1000 + uint32(time.tm_yday);
        }

        uint32 DayNow()
        {
            return DayOf(GameTime::GetGameTime().count());
        }

        /// The week of the year, as a number that grows: a new free sample every week.
        uint32 WeekNow()
        {
            std::tm const time = Acore::Time::TimeBreakdown(GameTime::GetGameTime().count());
            return uint32(time.tm_year + 1900) * 100 + uint32(time.tm_yday) / 7;
        }

        void SaveReader(ObjectGuid::LowType low, Reader const& reader);

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

        uint64 Key(uint32 a, uint32 b)
        {
            return (uint64(a) << 32) | b;
        }

        /// Midnight: the shelves are full again and everybody may buy again.
        void NewDay()
        {
            uint32 const day = DayNow();
            if (day == today)
                return;
            today = day;
            bought.clear();
            sold.clear();
        }

        uint32 ShelfSize(Ware const& ware)
        {
            return std::max<uint32>(1, ware.made ? cfg.postStockMade : cfg.postStockRaw);
        }

        uint32 LotSize(Ware const& ware)
        {
            return std::max<uint32>(1, ware.made ? cfg.postDailyMade : cfg.postDailyRaw);
        }

        /// When the "auction" of one thing at one post ends: once a day, at a time of its own.
        time_t EndOf(uint32 house, uint32 item, time_t now)
        {
            time_t const phase = time_t(Mix(house, item, 91) % DAY);
            time_t const from = now + Shortest;
            return from + (phase - from % DAY + DAY) % DAY;
        }

        bool OfAPerson(Player* player)
        {
            return player && player->GetSession() && !GET_PLAYERBOT_AI(player) &&
                !sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId());
        }

        char const* PostName(uint32 house)
        {
            switch (AuctionHouseId(house))
            {
                case AuctionHouseId::Alliance:  return "Alliance Trading Post";
                case AuctionHouseId::Horde:     return "Horde Trading Post";
                default:                        return "Goblin Trading Post";
            }
        }

        /// The auction house an auctioneer works for, as its number (2 Alliance, 6 Horde, 7 neutral); 0 if none.
        uint32 HouseOf(Creature* creature)
        {
            AuctionHouseEntry const* entry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(creature->GetFaction());
            return entry ? entry->houseId : 0;
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

        /// What one auction house is short of right now. On the world's thread.
        void BuildStock(uint32 houseId, Stock& stock)
        {
            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(houseId));
            std::unordered_map<uint32, uint32> held;
            if (house)
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
                Offer offer;
                offer.ware = i;
                offer.value = market.Value(ware.proto);
                stock.byItem[ware.proto->ItemId] = uint32(stock.offers.size());
                stock.offers.push_back(offer);
            }
            stock.built = GameTime::GetGameTime().count();
        }

        /// The price of one piece: on a bid and outright. The emptier the shelf, the dearer, by up to a half.
        void PricesOf(Offer const& offer, Ware const& ware, uint32 gone, uint32& bid, uint32& buyout)
        {
            double const scarcity = 1.0 + 0.5 * std::min(1.0, double(gone) / double(ShelfSize(ware)));
            double const value = offer.value * scarcity;
            uint32 const floor = ware.proto->SellPrice + 1;     // never so cheap that a vendor would pay more
            buyout = std::max<uint32>(HumanPrice(value * (ware.made ? cfg.postPriceMade : cfg.postPriceRaw)), floor + 1);
            bid = std::max<uint32>(HumanPrice(value * (ware.made ? cfg.postBidMade : cfg.postBidRaw)), floor);
            if (bid >= buyout)
                bid = std::max<uint32>(floor, buyout * 4 / 5);
        }

        uint32 Total(uint32 each, uint32 count)
        {
            return uint32(std::min<uint64>(uint64(each) * count, MAX_MONEY_AMOUNT));
        }

        void SaveBought(ObjectGuid::LowType player, uint32 item, uint32 count)
        {
            if (tables)
                CharacterDatabase.Execute("REPLACE INTO `mod_playerbots_auctions_post` (`player`, `item`, `day`, `bought`) VALUES ({}, {}, {}, {})",
                    player, item, today, count);
        }

        void SaveSold(uint32 house, uint32 item, uint32 count)
        {
            if (tables)
                CharacterDatabase.Execute("REPLACE INTO `mod_playerbots_auctions_post_stock` (`house`, `item`, `day`, `sold`) VALUES ({}, {}, {}, {})",
                    house, item, today, count);
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
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, std::string("Buy at the ") + PostName(HouseOf(creature)) + ".", PostSender, ACT_POST);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "What is the Trading Post?", PostSender, ACT_INFO);
            bool off, sample;
            {
                std::lock_guard<std::mutex> guard(lock);
                Reader const& reader = readers[player->GetGUID().GetCounter()];
                off = reader.off;
                sample = cfg.postSample && !junk.empty() && reader.sample != WeekNow();
            }
            if (sample)
                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "I am here for my free sample!", PostSender, ACT_SAMPLE);
            if (cfg.postFlyer)
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, off ? "Send me your flyers again." : "Stop sending me your flyers.", PostSender,
                    off ? ACT_FLYERS_ON : ACT_FLYERS_OFF);
            }
            SendGossipMenuFor(player, TextBase + TEXT_MENU, creature->GetGUID());
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
            uint32 const house = HouseOf(creature);
            if (!house)
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
            data << uint32(house);
            data << uint8(1);
            session->SendPacket(&data);
        }

        // ---------------------------------------------------------------------------------- the auction window

        struct Row
        {
            uint32 house;
            Ware const* ware;
            uint32 count;
            uint32 bid;
            uint32 buyout;
            time_t ends;
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
                    return a.buyout > b.buyout ? -1 : a.buyout < b.buyout ? 1 : 0;
                case AUCTION_SORT_BID:
                    return a.bid > b.bid ? -1 : a.bid < b.bid ? 1 : 0;
                case AUCTION_SORT_BUYOUT:
                case AUCTION_SORT_BUYOUT_2:
                    return a.buyout < b.buyout ? -1 : a.buyout > b.buyout ? 1 : 0;
                case AUCTION_SORT_TIMELEFT:
                    return a.ends < b.ends ? -1 : a.ends > b.ends ? 1 : 0;
                case AUCTION_SORT_STACK:
                    return a.count < b.count ? -1 : a.count > b.count ? 1 : 0;
                default:
                    return 0;
            }
        }

        void WriteRow(WorldPacket& data, Row const& row, time_t now)
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
            data << Seller(row.house);
            data << uint32(row.bid);                                // the lowest bid: the price with the caravan
            data << uint32(0);
            data << uint32(row.buyout);                             // the buyout: the price at once
            data << uint32(std::max<time_t>(0, row.ends - now) * IN_MILLISECONDS);
            data << uint64(0);
            data << uint32(0);
        }

        /// What the auction window asked for.
        struct Query
        {
            std::string searched;
            std::wstring wanted;
            uint8 levelMin = 0, levelMax = 0, usable = 0, getAll = 0;
            uint32 listFrom = 0, slot = 0, itemClass = 0, itemSubClass = 0, quality = 0;
            int locale = 0;
            std::vector<std::pair<uint8, bool>> sorting;

        };

        bool ReadQuery(WorldSession* session, WorldPacket& recvData, Query& query)
        {
            uint8 sortCount;
            ObjectGuid guid;
            recvData >> guid >> query.listFrom >> query.searched >> query.levelMin >> query.levelMax >> query.slot >> query.itemClass
                >> query.itemSubClass >> query.quality >> query.usable >> query.getAll >> sortCount;
            if (sortCount > AUCTION_SORT_MAX)
                return false;
            for (uint8 i = 0; i < sortCount; ++i)
            {
                uint8 mode, descending;
                recvData >> mode >> descending;
                query.sorting.emplace_back(mode, descending == 1);
            }
            query.wanted = SmallLetters(query.searched);
            query.locale = session->GetSessionDbLocaleIndex();
            return query.searched.empty() || !query.wanted.empty();
        }

        bool Fits(Query const& query, Ware const& ware, Player* player)
        {
            ItemTemplate const* proto = ware.proto;
            if (query.itemClass != 0xffffffff && proto->Class != query.itemClass)
                return false;
            if (query.itemSubClass != 0xffffffff && proto->SubClass != query.itemSubClass)
                return false;
            if (query.slot != 0xffffffff && proto->InventoryType != query.slot)
                return false;
            if (query.quality != 0xffffffff && proto->Quality < query.quality)
                return false;
            if (query.levelMin && (proto->RequiredLevel < query.levelMin || (query.levelMax && proto->RequiredLevel > query.levelMax)))
                return false;
            if (query.usable && player->CanUseItem(proto) != EQUIP_ERR_OK)
                return false;
            if (query.wanted.empty() || ware.name.find(query.wanted) != std::wstring::npos)
                return true;
            if (query.locale > LOCALE_enUS)
                if (ItemLocale const* names = sObjectMgr->GetItemLocale(proto->ItemId))
                {
                    std::string name = proto->Name1;
                    ObjectMgr::GetLocaleString(names->Name, query.locale, name);
                    return SmallLetters(name).find(query.wanted) != std::wstring::npos;
                }
            return false;
        }

        /// Can this player buy it here today: not bought yet, not sold out. Behind the lock.
        bool OnShelf(ObjectGuid::LowType low, uint32 house, Ware const& ware, uint32& gone)
        {
            if (bought.count(Key(low, ware.proto->ItemId)))
                return false;       // one lot of a thing a day
            auto had = sold.find(Key(house, ware.proto->ItemId));
            gone = had != sold.end() ? had->second : 0;
            return gone < ShelfSize(ware);
        }

        /// The window asks what there is. Runs on the thread of the player's map.
        void ListItems(WorldSession* session, WorldPacket& recvData)
        {
            Player* player = session->GetPlayer();
            Query query;
            if (!ReadQuery(session, recvData, query))
                return;
            std::string const& searched = query.searched;
            uint32 const itemClass = query.itemClass, itemSubClass = query.itemSubClass, listFrom = query.listFrom;
            uint8 const getAll = query.getAll;
            auto const& sorting = query.sorting;

            time_t const now = GameTime::GetGameTime().count();
            // Held to the end: the rows point into the list of wares, which a reload of the settings rebuilds.
            std::lock_guard<std::mutex> guard(lock);
            NewDay();
            std::vector<Row> rows;
            {
                ObjectGuid::LowType const low = player->GetGUID().GetCounter();
                auto visitor = visitors.find(low);
                if (visitor == visitors.end() || !visitor->second.house)
                    return;
                uint32 const house = visitor->second.house;
                Stock& stock = stocks[house];
                stock.wanted = now;

                for (Offer const& offer : stock.offers)
                {
                    Ware const& ware = wares[offer.ware];
                    ItemTemplate const* proto = ware.proto;
                    if (!Fits(query, ware, player))
                        continue;
                    uint32 gone = 0;
                    if (!OnShelf(low, house, ware, gone))
                        continue;
                    uint32 const shelf = ShelfSize(ware);
                    // One piece, a handful, and the most one may take.
                    uint32 const most = std::min({ LotSize(ware), shelf - gone, proto->GetMaxStackSize(), MaxLot });
                    uint32 bid, buyout;
                    PricesOf(offer, ware, gone, bid, buyout);
                    time_t const ends = EndOf(house, proto->ItemId, now);
                    uint32 last = 0;
                    for (uint32 const size : { uint32(1), uint32(5), most })
                        if (size <= most && size > last)
                        {
                            rows.push_back({ house, &ware, size, Total(bid, size), Total(buyout, size), ends });
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
                WriteRow(data, rows[i], now);
            data.put<uint32>(0, count);
            data << uint32(rows.size());
            data << uint32(AUCTION_SEARCH_DELAY);
            session->SendPacket(&data);
        }

        void Refuse(WorldSession* session, uint32 id, AuctionError error)
        {
            session->SendAuctionCommandResult(id, AUCTION_PLACE_BID, error);
        }

        std::string Subject(ItemTemplate const* proto, uint32 count)
        {
            return count > 1 ? Acore::StringFormat("{} ({})", proto->Name1, count) : proto->Name1;
        }

        void Fill(std::string& text, char const* what, std::string const& with)
        {
            for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size()))
                text.replace(at, std::strlen(what), with);
        }

        /// A letter of one of the clerks of that post.
        std::string LetterFor(uint32 house, bool express, std::string const& player, ItemTemplate const* proto, uint32 count)
        {
            char const* const side = AuctionHouseId(house) == AuctionHouseId::Alliance ? "alliance" :
                AuctionHouseId(house) == AuctionHouseId::Horde ? "horde" : "goblin";
            std::vector<char const*> fitting;
            for (Letter const& letter : Letters)
                if (!std::strcmp(letter.house, side) && !std::strcmp(letter.kind, express ? "express" : "caravan"))
                    fitting.push_back(letter.text);
            if (fitting.empty())
                return "Your order, as agreed.";
            std::string text = fitting[urand(0, uint32(fitting.size()) - 1)];
            Fill(text, "{count}", std::to_string(count));
            Fill(text, "{item}", proto->Name1);
            Fill(text, "{player}", player);
            return text;
        }

        /// Puts the goods into a player's mailbox, also of one who is not online.
        bool Deliver(ObjectGuid::LowType low, uint32 itemId, uint32 count, uint32 house, uint32 sender, bool express,
            CharacterDatabaseTransaction trans)
        {
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(low);
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            std::string name;
            if (player)
                name = player->GetName();
            else
                sCharacterCache->GetCharacterNameByGuid(guid, name);
            Item* item = Item::CreateItem(itemId, count, player);
            if (!item)
                return false;
            if (!player)
                item->SetOwnerGUID(guid);
            item->SaveToDB(trans);
            MailDraft(Subject(item->GetTemplate(), count), LetterFor(house, express, name, item->GetTemplate(), count))
                .AddItem(item)
                .SendMailTo(trans, MailReceiver(player, low), MailSender(MAIL_CREATURE, sender), MAIL_CHECK_MASK_HAS_BODY, 0);
            return true;
        }

        /// A price was clicked. Runs on the world's thread.
        void Buy(WorldSession* session, ObjectGuid npcGuid, uint32 id, uint32 paid)
        {
            Player* player = session->GetPlayer();
            uint32 const itemId = id & 0x00FFFFFF;
            uint32 const count = (id >> 24) & 0x3F;
            ObjectGuid::LowType const low = player->GetGUID().GetCounter();
            Creature* creature = player->GetNPCIfCanInteractWith(npcGuid, UNIT_NPC_FLAG_AUCTIONEER);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!creature || !proto || !count || !cfg.enabled || !cfg.post || !OfAPerson(player))
            {
                Refuse(session, id, ERR_AUCTION_ITEM_NOT_FOUND);
                return;
            }

            time_t const now = GameTime::GetGameTime().count();
            uint32 cost = 0, house = 0;
            bool express = false;
            time_t ends = 0;
            {
                std::lock_guard<std::mutex> guard(lock);
                NewDay();
                auto visitor = visitors.find(low);
                if (visitor == visitors.end() || !visitor->second.atPost || visitor->second.npc != npcGuid || !visitor->second.house)
                {
                    Refuse(session, id, ERR_AUCTION_ITEM_NOT_FOUND);
                    return;
                }
                house = visitor->second.house;
                Stock& stock = stocks[house];
                auto place = stock.byItem.find(itemId);
                if (place == stock.byItem.end())
                {
                    Refuse(session, id, ERR_AUCTION_ITEM_NOT_FOUND);    // the hall has it again
                    return;
                }
                Offer const& offer = stock.offers[place->second];
                Ware const& ware = wares[offer.ware];
                uint32& gone = sold[Key(house, itemId)];
                if (bought.count(Key(low, itemId)) || count > LotSize(ware) || gone + count > ShelfSize(ware) || count > proto->GetMaxStackSize())
                {
                    Refuse(session, id, ERR_AUCTION_ITEM_NOT_FOUND);
                    return;
                }
                uint32 bid, buyout;
                PricesOf(offer, ware, gone, bid, buyout);
                // The buyout brings it at once; the lowest bid sends it with the caravan. Anything in between is a
                // bid as well and costs no more than the lowest one. Less than that, and the prices moved since
                // the list was sent: nothing is sold.
                if (paid >= Total(buyout, count))
                {
                    express = true;
                    cost = Total(buyout, count);
                }
                else if (paid >= Total(bid, count))
                    cost = Total(bid, count);
                else
                {
                    if (cfg.debug)
                        LOG_INFO("module", "PlayerbotsAuctions: trading post - {} offered {} copper for {} x {} (item {}), the lowest bid is {}: not sold.",
                            player->GetName(), paid, count, proto->Name1, itemId, Total(bid, count));
                    Refuse(session, id, ERR_AUCTION_ITEM_NOT_FOUND);
                    return;
                }
                if (!player->HasEnoughMoney(cost))
                {
                    Refuse(session, id, ERR_AUCTION_NOT_ENOUGHT_MONEY);
                    return;
                }
                gone += count;
                bought[Key(low, itemId)] = count;
                uint64& left = spent[Key(house, today)];
                left += cost;
                if (tables)
                    CharacterDatabase.Execute("REPLACE INTO `mod_playerbots_auctions_post_spent` (`house`, `day`, `copper`) VALUES ({}, {}, {})", house, today, left);
                SaveBought(low, itemId, count);
                SaveSold(house, itemId, gone);
                ends = EndOf(house, itemId, now);
                if (!express)
                {
                    orders.push_back({ low, itemId, count, house, creature->GetEntry(), ends });
                    if (tables)
                        CharacterDatabase.Execute("REPLACE INTO `mod_playerbots_auctions_post_orders` (`player`, `item`, `count`, `house`, `sender`, `due`) VALUES ({}, {}, {}, {}, {}, {})",
                            low, itemId, count, house, creature->GetEntry(), uint64(ends));
                }
            }

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            player->ModifyMoney(-int32(cost));
            if (express)
                Deliver(low, itemId, count, house, creature->GetEntry(), true, trans);
            player->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            if (express)
                // What the auction house tells the game after a buyout, so the window behaves as it always does.
                session->SendAuctionBidderNotification(house, id, player->GetGUID(), 0, 0, itemId);
            else
            {
                uint32 const hours = uint32((ends - now + HOUR / 2) / HOUR);
                ChatHandler(session).PSendSysMessage("{}: {} x {} goes with the caravan and reaches your mailbox when the auction ends - {}.",
                    PostName(house), count, proto->Name1, hours ? Acore::StringFormat("in about {} hour{}", hours, hours == 1 ? "" : "s") : std::string("within the hour"));
            }
            session->SendAuctionCommandResult(id, AUCTION_PLACE_BID, ERR_AUCTION_OK);
            LOG_INFO("module", "PlayerbotsAuctions: {} bought {} x {} (item {}) at the trading post for {} copper, {}.",
                player->GetName(), count, proto->Name1, itemId, cost, express ? "at once" : "with the caravan");
        }

        /// Caravans that have arrived. On the world's thread.
        void DeliverOrders(time_t now)
        {
            std::vector<Order> due;
            {
                std::lock_guard<std::mutex> guard(lock);
                auto arrived = std::partition(orders.begin(), orders.end(), [now](Order const& order) { return order.due > now; });
                due.assign(arrived, orders.end());
                orders.erase(arrived, orders.end());
            }
            for (Order const& order : due)
            {
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                trans->Append("DELETE FROM `mod_playerbots_auctions_post_orders` WHERE `player` = {} AND `item` = {} AND `due` = {}",
                    order.player, order.item, uint64(order.due));
                // A character deleted in the meantime gets nothing.
                if (sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(order.player)) && sObjectMgr->GetItemTemplate(order.item))
                    Deliver(order.player, order.item, order.count, order.house, order.sender, false, trans);
                CharacterDatabase.CommitTransaction(trans);
            }
        }

        std::string LinkOf(ItemTemplate const* proto)
        {
            return Acore::StringFormat("|c{:08x}|Hitem:{}:0:0:0:0:0:0:0:0|h[{}]|h|r", ItemQualityColors[proto->Quality], proto->ItemId, proto->Name1);
        }

        std::string MoneyText(uint32 copper)
        {
            std::string text;
            if (copper >= GOLD)
                text += Acore::StringFormat("{}g", copper / GOLD);
            if (copper % GOLD >= SILVER)
                text += Acore::StringFormat("{}{}s", text.empty() ? "" : " ", (copper % GOLD) / SILVER);
            if (copper % SILVER || text.empty())
                text += Acore::StringFormat("{}{}c", text.empty() ? "" : " ", copper % SILVER);
            return text;
        }

        std::string PieceOf(char const* side, char const* writer, char const* part)
        {
            std::vector<char const*> fitting;
            for (Piece const& piece : Flyer)
                if (!std::strcmp(piece.house, side) && !std::strcmp(piece.writer, writer) && !std::strcmp(piece.part, part))
                    fitting.push_back(piece.text);
            return fitting.empty() ? std::string() : std::string(fitting[urand(0, uint32(fitting.size()) - 1)]);
        }

        char const* SideOf(uint32 house)
        {
            return AuctionHouseId(house) == AuctionHouseId::Alliance ? "alliance" : AuctionHouseId(house) == AuctionHouseId::Horde ? "horde" : "goblin";
        }

        /// One of the clerks of that post, at random: the flyer is theirs from the first line to the last.
        char const* WriterOf(char const* side)
        {
            std::vector<char const*> writers;
            for (Piece const& piece : Flyer)
                if (!std::strcmp(piece.house, side) && std::strcmp(piece.writer, "auctioneer") &&
                    std::find_if(writers.begin(), writers.end(), [&](char const* known) { return !std::strcmp(known, piece.writer); }) == writers.end())
                    writers.push_back(piece.writer);
            return writers.empty() ? "" : writers[urand(0, uint32(writers.size()) - 1)];
        }

        uint32 HouseOfPlayer(Player* player)
        {
            return uint32(sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION) ? AuctionHouseId::Neutral :
                player->GetTeamId() == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde);
        }

        /// The free sample of the week: always, without fail, junk - handed over with all due ceremony.
        void GiveSample(Player* player, Creature* creature)
        {
            ObjectGuid::LowType const low = player->GetGUID().GetCounter();
            uint32 itemId = 0;
            {
                std::lock_guard<std::mutex> guard(lock);
                Reader& reader = readers[low];
                if (!cfg.postSample || junk.empty() || reader.sample == WeekNow())
                {
                    CloseGossipMenuFor(player);
                    return;
                }
                reader.sample = WeekNow();
                SaveReader(low, reader);
                itemId = junk[urand(0, uint32(junk.size()) - 1)];
            }
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto)
                return;

            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1) == EQUIP_ERR_OK)
            {
                if (Item* item = player->StoreNewItem(dest, itemId, true))
                    player->SendNewItem(item, 1, true, false);
            }
            else
            {
                // Bags full: the treasure follows by mail.
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                if (Item* item = Item::CreateItem(itemId, 1, player))
                {
                    item->SaveToDB(trans);
                    MailDraft(proto->Name1, "Your free sample. You are welcome.").AddItem(item)
                        .SendMailTo(trans, MailReceiver(player, low), MailSender(MAIL_CREATURE, creature->GetEntry()), MAIL_CHECK_MASK_HAS_BODY, 0);
                }
                CharacterDatabase.CommitTransaction(trans);
            }

            std::string line = PieceOf(SideOf(HouseOf(creature)), "auctioneer", "handover");
            Fill(line, "{item}", LinkOf(proto));
            Fill(line, "{player}", player->GetName());
            if (!line.empty())
                creature->Whisper(line, LANG_UNIVERSAL, player);
            ClearGossipMenuFor(player);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "...thank you?", PostSender, ACT_BACK);
            SendGossipMenuFor(player, TextBase + TEXT_SAMPLE, creature->GetGUID());
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: trading post - {} collects the free sample of the week: {}.", player->GetName(), proto->Name1);
        }

        /// What lies in the auction houses right now is written down: the cheapest piece of each thing, with its seller.
        /// On the world's thread.
        void Watch()
        {
            time_t const now = GameTime::GetGameTime().count();
            std::set<AuctionHouseObject*> done;
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            uint32 written = 0;
            for (AuctionHouseId const id : { AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral })
            {
                AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(id);
                if (!house || !done.insert(house).second)
                    continue;
                // With the houses joined, every house number points to the same auctions; they are noted for each.
                std::unordered_map<uint32, Sighting> cheapest;
                for (auto const& entry : house->GetAuctions())
                {
                    AuctionEntry const* auction = entry.second;
                    if (!auction || !auction->itemCount)
                        continue;
                    uint32 const price = auction->buyout ? auction->buyout : std::max(auction->bid, auction->startbid);
                    uint32 const each = std::max<uint32>(1, price / auction->itemCount);
                    Sighting& best = cheapest[auction->item_template];
                    if (!best.when || each < best.each)
                        best = { now, each, auction->owner.GetCounter() };
                }
                for (AuctionHouseId const number : { AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral })
                {
                    if (sAuctionMgr->GetAuctionsMapByHouseId(number) != house)
                        continue;
                    std::lock_guard<std::mutex> guard(lock);
                    for (auto const& entry : cheapest)
                    {
                        seen[Key(uint32(number), entry.first)] = entry.second;
                        if (tables)
                        {
                            trans->Append("REPLACE INTO `mod_playerbots_auctions_post_seen` (`house`, `item`, `seen`, `each`, `seller`) VALUES ({}, {}, {}, {}, {})",
                                uint32(number), entry.first, uint64(now), entry.second.each, entry.second.seller);
                            ++written;
                        }
                    }
                }
            }
            if (written)
                CharacterDatabase.CommitTransaction(trans);
        }

        std::string WhenText(time_t ago)
        {
            static char const* const Numbers[] = { "", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
                "twenty-one", "twenty-two", "twenty-three" };
            if (ago < HOUR)
                return "within the hour";
            if (ago < DAY)
            {
                uint32 const hours = uint32(ago / HOUR);
                return hours == 1 ? std::string("an hour ago") : Acore::StringFormat("{} hours ago", Numbers[hours]);
            }
            uint32 const days = uint32(ago / DAY);
            if (days == 1)
                return "yesterday";
            if (days < 14)
                return Acore::StringFormat("{} days ago", Numbers[days]);
            if (days < 60)
                return Acore::StringFormat("{} weeks ago", days / 7 < 24 ? Numbers[days / 7] : "many");
            return "ages ago";
        }

        std::string NameOf(ObjectGuid::LowType low)
        {
            std::string name;
            if (!low || !sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(low), name) || name.empty())
                return "somebody long gone";
            return name;
        }

        /// The best bargains in the auction house right now for a character of that level: well under what
        /// things usually go for. On the world's thread.
        std::vector<std::string> Bargains(Player* player, uint32 house, size_t most)
        {
            struct Find
            {
                AuctionEntry const* auction;
                ItemTemplate const* proto;
                double usual;
                double ratio;
            };
            std::vector<Find> finds;
            AuctionHouseObject* auctions = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId(house));
            if (!auctions)
                return {};
            uint32 const level = player->GetLevel();
            for (auto const& entry : auctions->GetAuctions())
            {
                AuctionEntry const* auction = entry.second;
                if (!auction || !auction->buyout || !auction->itemCount || auction->owner == player->GetGUID())
                    continue;
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
                if (!proto || proto->Quality < ITEM_QUALITY_NORMAL || proto->Quality >= MAX_ITEM_QUALITY || proto->RequiredLevel > level ||
                    proto->ItemLevel > level + 10 || proto->ItemLevel + 25 < level)
                    continue;
                double const usual = market.Value(proto) * auction->itemCount;
                double const ratio = double(auction->buyout) / std::max(1.0, usual);
                if (ratio < 0.6 && auction->buyout > proto->SellPrice * auction->itemCount)
                    finds.push_back({ auction, proto, usual, ratio });
            }
            std::sort(finds.begin(), finds.end(), [](Find const& a, Find const& b) { return a.ratio < b.ratio; });
            std::vector<std::string> lines;
            std::set<uint32> items;
            for (Find const& find : finds)
            {
                if (lines.size() >= most || !items.insert(find.proto->ItemId).second)
                    continue;
                lines.push_back(Acore::StringFormat("- {}{}: {} from {} - usually about {}", find.proto->Name1,
                    find.auction->itemCount > 1 ? Acore::StringFormat(" x{}", find.auction->itemCount) : std::string(),
                    MoneyText(find.auction->buyout), NameOf(find.auction->owner.GetCounter()),
                    MoneyText(uint32(std::min(find.usual, double(MAX_MONEY_AMOUNT))))));
            }
            return lines;
        }

        bool IsPostMail(Mail const* mail)
        {
            return mail->state != MAIL_STATE_DELETED && mail->messageType == MAIL_NORMAL && mail->sender >= SellerLow &&
                mail->sender <= SellerLow + 7 && mail->items.empty() && !mail->money;
        }

        /// The first letter a character ever gets from the post: the grand opening. Daily flyers follow once it was read.
        void SendIntroduction(Player* player)
        {
            ObjectGuid::LowType const low = player->GetGUID().GetCounter();
            uint32 const house = HouseOfPlayer(player);
            char const* const side = SideOf(house);
            char const* const writer = WriterOf(side);
            bool const sample = cfg.postSample && !junk.empty();
            std::string body = PieceOf(side, writer, "welcome_opening") + "\n\n" + PieceOf(side, writer, "welcome_what") + "\n\n" +
                PieceOf(side, writer, "welcome_how") + "\n\n";
            if (sample)
                body += PieceOf(side, writer, "sample") + "\n\n";
            body += PieceOf(side, writer, "signature");
            std::string subject = PieceOf(side, writer, "welcome_subject");
            for (std::string* text : { &body, &subject })
            {
                Fill(*text, "{player}", player->GetName());
                Fill(*text, "{post}", PostName(house));
            }
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            MailDraft(subject, body).SendMailTo(trans, MailReceiver(player, low), MailSender(MAIL_NORMAL, SellerLow + house), MAIL_CHECK_MASK_NONE, 0);
            CharacterDatabase.CommitTransaction(trans);

            // The newest mail of the player is that letter: remembered, to see later whether it was read.
            uint32 mailId = 0;
            for (Mail* mail : player->GetMails())
                if (IsPostMail(mail) && mail->messageID > mailId)
                    mailId = mail->messageID;
            {
                std::lock_guard<std::mutex> guard(lock);
                Reader& reader = readers[low];
                reader.intro = mailId ? mailId : 1;
                reader.day = DayNow();
                SaveReader(low, reader);
            }
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: trading post - {} gets the letter about the opening, by {}.", player->GetName(), writer);
        }

        /// Has the letter about the opening been read - or is it gone, deleted or run out?
        bool IntroductionRead(Player* player, Reader const& reader)
        {
            if (reader.introRead)
                return true;
            for (Mail* mail : player->GetMails())
                if (mail->messageID == reader.intro)
                    return mail->state != MAIL_STATE_DELETED && (mail->checked & MAIL_CHECK_MASK_READ);
            return true;
        }

        /// Once a day, at the first login: what the post of the player's side has today, as a leaflet.
        void SendFlyer(Player* player)
        {
            ObjectGuid::LowType const low = player->GetGUID().GetCounter();
            uint32 const house = HouseOfPlayer(player);
            uint32 const level = player->GetLevel();
            uint64 yesterday = 0;
            bool sample = false;

            std::vector<std::string> lines;
            {
                std::lock_guard<std::mutex> guard(lock);
                NewDay();
                Reader& reader = readers[low];
                if (reader.off || reader.day == today || !reader.intro)
                    return;
                if (!reader.introRead)
                {
                    if (!IntroductionRead(player, reader))
                        return;     // the daily flyers wait until the first letter was read
                    reader.introRead = true;
                }
                auto left = spent.find(Key(house, DayOf(GameTime::GetGameTime().count() - DAY)));
                yesterday = left != spent.end() ? left->second : 0;
                sample = cfg.postSample && !junk.empty() && reader.sample != WeekNow();
                Stock& stock = stocks[house];
                time_t const now = GameTime::GetGameTime().count();
                if (!stock.built || now - stock.built >= 60)
                    BuildStock(house, stock);

                // What suits a character of that level, a few of each kind: mostly materials, some of what crafters make.
                std::vector<Offer const*> materials, crafted;
                for (Offer const& offer : stock.offers)
                {
                    Ware const& ware = wares[offer.ware];
                    uint32 gone = 0;
                    if (!OnShelf(low, house, ware, gone) || ware.proto->RequiredLevel > level)
                        continue;
                    if (ware.proto->ItemLevel > level + 10 || ware.proto->ItemLevel + 25 < level)
                        continue;
                    (ware.made ? crafted : materials).push_back(&offer);
                }
                Acore::Containers::RandomShuffle(materials);
                Acore::Containers::RandomShuffle(crafted);
                if (materials.size() > 4)
                    materials.resize(4);
                if (crafted.size() > 2)
                    crafted.resize(2);
                materials.insert(materials.end(), crafted.begin(), crafted.end());
                for (Offer const* offer : materials)
                {
                    Ware const& ware = wares[offer->ware];
                    uint32 gone = 0;
                    OnShelf(low, house, ware, gone);
                    uint32 bid, buyout;
                    PricesOf(*offer, ware, gone, bid, buyout);
                    // Next to it, when the auction house last had it, for how much and from whom.
                    auto sighting = seen.find(Key(house, ware.proto->ItemId));
                    std::string line = PieceOf(SideOf(house), "any", sighting != seen.end() ? "lastseen" : "neverseen");
                    if (line.empty())
                        line = "- {item}: {ours} a piece - or {caravan} with the caravan";
                    Fill(line, "{item}", ware.proto->Name1);
                    Fill(line, "{ours}", MoneyText(buyout));
                    Fill(line, "{caravan}", MoneyText(bid));
                    if (sighting != seen.end())
                    {
                        Fill(line, "{when}", WhenText(now - sighting->second.when));
                        Fill(line, "{price}", MoneyText(sighting->second.each));
                        Fill(line, "{seller}", NameOf(sighting->second.seller));
                    }
                    lines.push_back(line);
                }
                if (lines.empty())
                    return;     // nothing to advertise today; maybe tomorrow
                reader.day = today;
                SaveReader(low, reader);
            }

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            // Yesterday's flyer goes out with the new one: there is never more than one in the mailbox.
            uint32 intro;
            {
                std::lock_guard<std::mutex> guard(lock);
                intro = readers[low].intro;
            }
            for (Mail* mail : player->GetMails())
                if (IsPostMail(mail) && mail->messageID != intro)
                {
                    mail->state = MAIL_STATE_DELETED;
                    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
                    stmt->SetData(0, mail->messageID);
                    trans->Append(stmt);
                    PBA_MAIL_DELETED(player->GetGUID());
                }
            player->m_mailsUpdated = true;

            char const* const side = SideOf(house);
            char const* const writer = WriterOf(side);
            std::vector<std::string> const bargains = Bargains(player, house, 3);
            std::string list;
            for (std::string const& line : lines)
                list += line + "\n";
            // The pillory: what the customers left at the counter yesterday.
            std::string shame = PieceOf(side, writer, yesterday ? "spent" : "nothing");
            Fill(shame, "{gold}", MoneyText(uint32(std::min<uint64>(yesterday, MAX_MONEY_AMOUNT))));
            std::string body = PieceOf(side, writer, "headline") + "\n\n" + PieceOf(side, writer, "intro") + "\n\n" + shame + "\n\n";
            if (sample)
                body += PieceOf(side, writer, "sample") + "\n\n";
            body += PieceOf(side, writer, "listhead") + "\n" + list + "\n" + PieceOf(side, writer, "closing") + "\n\n";
            // For the misers: the best bargains next door, written through gritted teeth.
            if (!bargains.empty())
            {
                body += PieceOf(side, writer, "misers") + "\n";
                for (std::string const& line : bargains)
                    body += line + "\n";
                body += PieceOf(side, writer, "hurry") + "\n\n";
            }
            body += PieceOf(side, writer, "signature") + "\n\n" + PieceOf(side, writer, "ps");
            // The post knows where this leaflet ends up, and says so.
            body += "\n\n" + PieceOf(side, writer, "unsubscribe");
            std::string subject = PieceOf(side, writer, "subject");
            for (std::string* text : { &body, &subject })
            {
                Fill(*text, "{player}", player->GetName());
                Fill(*text, "{post}", PostName(house));
            }
            MailDraft(subject, body).SendMailTo(trans, MailReceiver(player, low), MailSender(MAIL_NORMAL, SellerLow + house),
                MAIL_CHECK_MASK_NONE, 0, 1);    // gone after a day
            CharacterDatabase.CommitTransaction(trans);
            if (cfg.debug)
                LOG_INFO("module", "PlayerbotsAuctions: trading post - {} gets today's flyer by {}, with {} offer(s){}.", player->GetName(), writer, lines.size(),
                    sample ? " and the free sample" : "");
        }
    }

    namespace
    {
        void SaveReader(ObjectGuid::LowType low, Reader const& reader)
        {
            if (tables)
                CharacterDatabase.Execute("REPLACE INTO `mod_playerbots_auctions_post_flyer` (`player`, `day`, `off`, `sample`, `intro`, `introread`) "
                    "VALUES ({}, {}, {}, {}, {}, {})", low, reader.day, reader.off ? 1 : 0, reader.sample, reader.intro, reader.introRead ? 1 : 0);
        }
    }

    // ---------------------------------------------------------------------------------------- from outside

    void PostLogin(Player* player)
    {
        if (!cfg.enabled || !cfg.post || !cfg.postFlyer || !OfAPerson(player))
            return;
        bool introduced, off;
        {
            std::lock_guard<std::mutex> guard(lock);
            Reader const& reader = readers[player->GetGUID().GetCounter()];
            introduced = reader.intro != 0;
            off = reader.off;
        }
        if (off)
            return;
        if (!introduced)
            SendIntroduction(player);
        else
            SendFlyer(player);
    }

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
            "COMMENT='mod-playerbots-auctions: what players bought at the trading post today'");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_post_stock` ("
            "`house` INT UNSIGNED NOT NULL, `item` INT UNSIGNED NOT NULL, `day` INT UNSIGNED NOT NULL, `sold` INT UNSIGNED NOT NULL, "
            "PRIMARY KEY (`house`, `item`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
            "COMMENT='mod-playerbots-auctions: what each trading post sold today'");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_post_orders` ("
            "`player` INT UNSIGNED NOT NULL, `item` INT UNSIGNED NOT NULL, `count` INT UNSIGNED NOT NULL, `house` INT UNSIGNED NOT NULL, "
            "`sender` INT UNSIGNED NOT NULL, `due` BIGINT UNSIGNED NOT NULL, PRIMARY KEY (`player`, `item`, `due`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
            "COMMENT='mod-playerbots-auctions: bids at the trading post that are on their way'");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_post_flyer` ("
            "`player` INT UNSIGNED NOT NULL, `day` INT UNSIGNED NOT NULL, `off` TINYINT UNSIGNED NOT NULL DEFAULT 0, "
            "`sample` INT UNSIGNED NOT NULL DEFAULT 0, `intro` INT UNSIGNED NOT NULL DEFAULT 0, `introread` TINYINT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`player`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
            "COMMENT='mod-playerbots-auctions: the daily flyer of the trading post - last one sent, who wants none, the week of the last free sample'");
        if (!CharacterDatabase.Query("SHOW COLUMNS FROM `mod_playerbots_auctions_post_flyer` LIKE 'sample'"))
            CharacterDatabase.DirectExecute("ALTER TABLE `mod_playerbots_auctions_post_flyer` ADD COLUMN `sample` INT UNSIGNED NOT NULL DEFAULT 0");
        if (!CharacterDatabase.Query("SHOW COLUMNS FROM `mod_playerbots_auctions_post_flyer` LIKE 'intro'"))
            CharacterDatabase.DirectExecute("ALTER TABLE `mod_playerbots_auctions_post_flyer` ADD COLUMN `intro` INT UNSIGNED NOT NULL DEFAULT 0, "
                "ADD COLUMN `introread` TINYINT UNSIGNED NOT NULL DEFAULT 0");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_post_seen` ("
            "`house` INT UNSIGNED NOT NULL, `item` INT UNSIGNED NOT NULL, `seen` BIGINT UNSIGNED NOT NULL, `each` INT UNSIGNED NOT NULL, "
            "`seller` INT UNSIGNED NOT NULL, PRIMARY KEY (`house`, `item`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
            "COMMENT='mod-playerbots-auctions: when each thing was last seen in each auction house, the cheapest piece, and its seller'");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `mod_playerbots_auctions_post_spent` ("
            "`house` INT UNSIGNED NOT NULL, `day` INT UNSIGNED NOT NULL, `copper` BIGINT UNSIGNED NOT NULL, "
            "PRIMARY KEY (`house`, `day`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
            "COMMENT='mod-playerbots-auctions: what players left at each trading post, by day (for the flyer)'");
        tables = true;
        readers.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT `player`, `day`, `off`, `sample`, `intro`, `introread` FROM `mod_playerbots_auctions_post_flyer`"))
            do
            {
                Field* fields = result->Fetch();
                Reader& reader = readers[fields[0].Get<uint32>()];
                reader.day = fields[1].Get<uint32>();
                reader.off = fields[2].Get<uint8>() != 0;
                reader.sample = fields[3].Get<uint32>();
                reader.intro = fields[4].Get<uint32>();
                reader.introRead = fields[5].Get<uint8>() != 0;
            } while (result->NextRow());
        seen.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT `house`, `item`, `seen`, `each`, `seller` FROM `mod_playerbots_auctions_post_seen`"))
            do
            {
                Field* fields = result->Fetch();
                seen[Key(fields[0].Get<uint32>(), fields[1].Get<uint32>())] = { time_t(fields[2].Get<uint64>()), fields[3].Get<uint32>(), fields[4].Get<uint32>() };
            } while (result->NextRow());
        spent.clear();
        uint32 const yesterday = DayOf(GameTime::GetGameTime().count() - DAY);
        CharacterDatabase.DirectExecute("DELETE FROM `mod_playerbots_auctions_post_spent` WHERE `day` <> {} AND `day` <> {}", DayNow(), yesterday);
        if (QueryResult result = CharacterDatabase.Query("SELECT `house`, `day`, `copper` FROM `mod_playerbots_auctions_post_spent`"))
            do
            {
                Field* fields = result->Fetch();
                spent[Key(fields[0].Get<uint32>(), fields[1].Get<uint32>())] = fields[2].Get<uint64>();
            } while (result->NextRow());

        today = DayNow();
        bought.clear();
        sold.clear();
        CharacterDatabase.DirectExecute("DELETE FROM `mod_playerbots_auctions_post` WHERE `day` <> {}", today);
        CharacterDatabase.DirectExecute("DELETE FROM `mod_playerbots_auctions_post_stock` WHERE `day` <> {}", today);
        if (QueryResult result = CharacterDatabase.Query("SELECT `player`, `item`, `bought` FROM `mod_playerbots_auctions_post` WHERE `day` = {} AND `item` <> 0", today))
            do
            {
                Field* fields = result->Fetch();
                bought[Key(fields[0].Get<uint32>(), fields[1].Get<uint32>())] = fields[2].Get<uint32>();
            } while (result->NextRow());
        if (QueryResult result = CharacterDatabase.Query("SELECT `house`, `item`, `sold` FROM `mod_playerbots_auctions_post_stock` WHERE `day` = {}", today))
            do
            {
                Field* fields = result->Fetch();
                sold[Key(fields[0].Get<uint32>(), fields[1].Get<uint32>())] = fields[2].Get<uint32>();
            } while (result->NextRow());
        orders.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT `player`, `item`, `count`, `house`, `sender`, `due` FROM `mod_playerbots_auctions_post_orders`"))
            do
            {
                Field* fields = result->Fetch();
                orders.push_back({ fields[0].Get<uint32>(), fields[1].Get<uint32>(), fields[2].Get<uint32>(), fields[3].Get<uint32>(),
                    fields[4].Get<uint32>(), time_t(fields[5].Get<uint64>()) });
            } while (result->NextRow());

        if (!cfg.post)
            return;

        std::unordered_set<uint32> made;
        std::unordered_set<uint32> const found = Obtainable(made);
        uint32 crafted = 0;
        junk.clear();
        for (auto const& entry : *sObjectMgr->GetItemTemplateStore())
        {
            ItemTemplate const& proto = entry.second;
            // Junk for the free sample: grey odds and ends that really drop somewhere.
            if (proto.Quality == ITEM_QUALITY_POOR && proto.Class == ITEM_CLASS_MISC && proto.SubClass == ITEM_SUBCLASS_JUNK &&
                proto.Bonding == NO_BIND && proto.SellPrice && !proto.HasFlag(ITEM_FLAG_HAS_LOOT) && found.count(proto.ItemId) && !proto.Name1.empty())
            {
                std::string const name = Lower(proto.Name1);
                bool odd = false;
                for (std::string const& part : cfg.excludedNameParts)
                    if (!part.empty() && name.find(part) != std::string::npos)
                        odd = true;
                if (!odd)
                    junk.push_back(proto.ItemId);
            }
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

        // Caravans arrive even with the post switched off: they were paid for.
        time_t const now = GameTime::GetGameTime().count();
        DeliverOrders(now);

        // What the auction houses hold is written down now and then, for the "last seen" of the flyers.
        static time_t watched = 0;
        if (cfg.enabled && cfg.post && cfg.postFlyer && now - watched >= 10 * MINUTE)
        {
            watched = now;
            Watch();
        }
        if (!cfg.enabled || !cfg.post)
            return;

        // The shelves follow the auction house while somebody is looking at them.
        std::lock_guard<std::mutex> guard(lock);
        NewDay();
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
            visitor.house = HouseOf(creature);
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

        switch (action)
        {
            case ACT_BROWSE:
            {
                {
                    std::lock_guard<std::mutex> guard(lock);
                    visitors[player->GetGUID().GetCounter()].atPost = false;
                }
                CloseGossipMenuFor(player);
                passing = true;
                player->GetSession()->SendAuctionHello(creature->GetGUID(), creature);
                passing = false;
                break;
            }
            case ACT_POST:
                OpenPost(player, creature);
                break;
            case ACT_INFO:
                ClearGossipMenuFor(player);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "I see.", PostSender, ACT_BACK);
                SendGossipMenuFor(player, TextBase + TEXT_INFO, creature->GetGUID());
                break;
            case ACT_BACK:
                ShowMenu(player, creature);
                break;
            case ACT_SAMPLE:
                GiveSample(player, creature);
                break;
            case ACT_FLYERS_OFF:
            case ACT_FLYERS_ON:
            {
                bool const off = action == ACT_FLYERS_OFF;
                {
                    std::lock_guard<std::mutex> guard(lock);
                    Reader& reader = readers[player->GetGUID().GetCounter()];
                    reader.off = off;
                    SaveReader(player->GetGUID().GetCounter(), reader);
                }
                creature->Whisper(off ? "As you wish. No more flyers - but you know where to find us." :
                    "Splendid! Tomorrow's offers will be in your mailbox.", LANG_UNIVERSAL, player);
                ShowMenu(player, creature);
                break;
            }
            default:
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
                if (packet.size() < 8)
                    return true;
                ObjectGuid const asked(packet.read<uint64>(0));
                if (!asked.IsPlayer() || asked.GetCounter() < SellerLow || asked.GetCounter() > SellerLow + 7)
                    return true;
                WorldPackets::Query::NameQueryResponse response;
                response.Guid = asked.WriteAsPacked();
                response.NameUnknown = false;
                response.Name = PostName(asked.GetCounter() - SellerLow);
                response.Race = RACE_HUMAN;
                response.Sex = GENDER_MALE;
                response.Class = CLASS_ROGUE;
                response.Declined = false;
                session->SendPacket(response.Write());
                return false;
            }
            case CMSG_MAIL_RETURN_TO_SENDER:
            {
                // A flyer sent back: there is nobody to send it to, so it is simply thrown away.
                Player* player = session->GetPlayer();
                if (!player || packet.size() < 12)
                    return true;
                Mail* mail = player->GetMail(packet.read<uint32>(8));
                if (!mail || mail->state == MAIL_STATE_DELETED || mail->messageType != MAIL_NORMAL ||
                    mail->sender < SellerLow || mail->sender > SellerLow + 7 || mail->HasItems() || mail->money)
                    return true;
                mail->state = MAIL_STATE_DELETED;
                player->m_mailsUpdated = true;
                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
                stmt->SetData(0, mail->messageID);
                CharacterDatabase.Execute(stmt);
                PBA_MAIL_DELETED(player->GetGUID());
                player->SendMailResult(mail->messageID, MAIL_RETURNED_TO_SENDER, MAIL_OK);
                return false;
            }
            case CMSG_AUCTION_LIST_ITEMS:
            {
                Player* player = session->GetPlayer();
                if (!player || !cfg.enabled || !cfg.post)
                    return true;
                WorldPacket copy(packet);
                {
                    std::lock_guard<std::mutex> guard(lock);
                    ObjectGuid::LowType const low = player->GetGUID().GetCounter();
                    auto visitor = visitors.find(low);
                    if (visitor == visitors.end() || !visitor->second.atPost)
                        return true;
                }
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
                if ((id & IdMask) != IdFlag)
                    return true;        // a real auction
                if (cfg.debug)
                    LOG_INFO("module", "PlayerbotsAuctions: trading post - {} clicks a price: {} x item {} for {} copper.",
                        player->GetName(), (id >> 24) & 0x3F, id & 0x00FFFFFF, packet.read<uint32>(12));
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
