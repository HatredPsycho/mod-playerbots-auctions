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
        constexpr uint32 TextBase   = 9113200;          // the texts of the gossip window; a game remembers a text by its number
        // "Auctions" of the post: this bit, 6 bits of count, 24 bits of item. Real auctions count up from 1 and
        // never get there. The top bit stays clear: the game takes such a number for no auction at all and
        // will not let the row be selected.
        constexpr uint32 IdFlag     = 0x40000000;
        constexpr uint32 IdMask     = 0xC0000000;
        constexpr uint32 MaxLot     = 60;
        constexpr uint32 SellerLow  = 0xFFFFFF00;       // the "player" the goods are listed under: no character has this number
        constexpr time_t Shortest   = 30 * MINUTE;      // a bid always waits at least this long

        ObjectGuid Seller()
        {
            return ObjectGuid::Create<HighGuid::Player>(SellerLow);
        }

        enum Action : uint32
        {
            ACT_BROWSE = 1,
            ACT_POST,
            ACT_INFO,
            ACT_BACK
        };

        // ---------------------------------------------------------------------------------- what is said

        enum : uint32
        {
            TEXT_MENU,
            TEXT_INFO,
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
            "Either way it costs well above what things usually go for."
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
        bool tables = false;
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
            data << Seller();
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

            /// Asked for something in particular - a name or a kind of thing - not just "everything".
            bool Particular() const
            {
                return !wanted.empty() || itemClass != 0xffffffff;
            }
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

        std::unordered_map<ObjectGuid::LowType, Query> lastSearch;      // what a player last asked the real auction house

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
                            rows.push_back({ &ware, size, Total(bid, size), Total(buyout, size), ends });
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
        tables = true;

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

        // Caravans arrive even with the post switched off: they were paid for.
        time_t const now = GameTime::GetGameTime().count();
        DeliverOrders(now);
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
                WorldPacket copy(packet);
                {
                    std::lock_guard<std::mutex> guard(lock);
                    ObjectGuid::LowType const low = player->GetGUID().GetCounter();
                    auto visitor = visitors.find(low);
                    if (visitor == visitors.end() || !visitor->second.atPost)
                    {
                        // A search of the real auction house: kept, in case it finds nothing.
                        Query query;
                        try
                        {
                            if (ReadQuery(session, copy, query))
                                lastSearch[low] = std::move(query);
                        }
                        catch (ByteBufferException const&)
                        {
                        }
                        return true;
                    }
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

    void PostSent(WorldSession* session, WorldPacket const& packet)
    {
        // The real auction house found nothing: if the post has what was asked for, the player hears of it.
        if (packet.GetOpcode() != SMSG_AUCTION_LIST_RESULT || packet.size() < 4 || packet.read<uint32>(0) != 0)
            return;
        Player* player = session->GetPlayer();
        if (!player || !cfg.enabled || !cfg.post || !OfAPerson(player))
            return;

        uint32 offers = 0, house = 0;
        std::string searched;
        {
            std::lock_guard<std::mutex> guard(lock);
            ObjectGuid::LowType const low = player->GetGUID().GetCounter();
            auto asked = lastSearch.find(low);
            auto visitor = visitors.find(low);
            if (asked == lastSearch.end() || visitor == visitors.end() || visitor->second.atPost || !visitor->second.house)
                return;
            Query const query = std::move(asked->second);
            lastSearch.erase(asked);
            if (!query.Particular())
                return;
            NewDay();
            house = visitor->second.house;
            Stock& stock = stocks[house];
            time_t const now = GameTime::GetGameTime().count();
            if (!stock.built || now - stock.built >= 30)
                BuildStock(house, stock);       // the results are sent on the world's thread
            for (Offer const& offer : stock.offers)
            {
                uint32 gone = 0;
                if (Fits(query, wares[offer.ware], player) && OnShelf(low, house, wares[offer.ware], gone))
                    ++offers;
            }
            searched = query.searched;
        }
        if (!offers)
            return;

        char const* post = PostName(house);
        ChatHandler handler(session);
        handler.SendNotification(Acore::StringFormat("Nothing here - but the {} has it. Try your luck there!", post));
        if (searched.empty())
            handler.PSendSysMessage("The auction house has none of that right now - the {} has {} offer{}. Talk to the auctioneer "
                "and choose \"Buy at the {}\".", post, offers, offers == 1 ? "" : "s", post);
        else
            handler.PSendSysMessage("The auction house has no \"{}\" right now - the {} has {} offer{}. Talk to the auctioneer "
                "and choose \"Buy at the {}\".", searched, post, offers, offers == 1 ? "" : "s", post);
    }

    void PostLeft(Player* player)
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> guard(lock);
        visitors.erase(player->GetGUID().GetCounter());
        lastSearch.erase(player->GetGUID().GetCounter());
    }
}
