#!/usr/bin/env python3
"""Builds data/dialogues.txt: short 3-step conversations between auction-house bots.

Each script belongs to one template. Slots are filled with ordinary old-world goods and
places, so the conversations fit a level 60 realm. Run make_dialogues.py afterwards.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "dialogues.txt")

ITEMS = ["Copper Ore", "Linen Cloth", "Boar Meat", "Light Leather", "Silverleaf",
         "Malachite", "Rough Stone", "Fishing Pole", "Lesser Healing Potion",
         "Mageweave Cloth", "Iron Ore", "Heavy Leather", "Silk Cloth", "Truesilver Ore",
         "Small Egg", "Shiny Bauble", "Rusted Sword", "Green Hills of Stranglethorn",
         "Blackmouth Oil", "Soft Banana Bread", "Coarse Stone", "Wooden Buckler",
         "Handmade Leather Bag", "Minor Mana Potion", "Simple Flour", "Mild Spices",
         "Empty Vial", "Bottle of Wine", "Traveler's Backpack", "Fishing Hook"]
PRICES = ["1 silver", "2 silver", "3 silver", "5 silver", "8 silver", "12 silver",
          "20 silver", "35 silver", "50 silver", "75 silver", "90 silver", "1 gold",
          "1 gold 50", "2 gold", "3 gold", "4 gold", "5 gold", "7 gold", "9 gold", "12 gold"]
PLACES = ["the auction house", "the bank", "the inn", "the market", "the forge",
          "the dock", "the plaza", "the mailbox", "the flight master", "the fountain",
          "the bridge", "the north gate", "the south gate", "the tailor", "the alchemist",
          "the weapon rack", "the stable", "the bakery", "the tannery", "the jewelcrafter",
          "the enchanter", "the barracks", "the temple", "the graveyard", "the watchtower",
          "the harbour", "the cart", "the well", "the notice board", "the training yard",
          "the archway", "the cellar", "the roof", "the balcony", "the alley",
          "the square", "the signpost", "the lamp post", "the bench", "the tree",
          "the fence", "the wagon"]
CREATURES = ["a wolf", "a boar", "a murloc", "a kobold", "a defias", "an ogre",
             "a spider", "a bear"]
REASONS = ["for my profession", "for a friend", "for the guild bank", "to resell",
           "for an alt", "for a quest"]

# (personality step 0, 1, 2), three spoken lines. {item} {price} {place} {creature} {reason}
TEMPLATES = [
    ("miser", "braggart", "gossip", [
        "{item} for {price}? I could feed a family for a week.",
        "Then perhaps you should sell the family. I sold {item} for {price} before breakfast.",
        "And that, ladies and gentlemen, is why we all stand at {place}."]),
    ("newbie", "veteran", "newbie", [
        "Is {item} at {price} a good deal? I am new at {place}.",
        "At {price}? Buy it, use it, and stay away from {creature} on the way home.",
        "Good advice. I will spend the rest of my money on {item}, then."]),
    ("trader", "hawker", "trader", [
        "Fresh {item}, {price}, no questions asked.",
        "I have the same {item} for half. Well, mostly the same. Slightly used.",
        "Slightly used by {creature}, I assume. I will keep my {price} offer."]),
    ("collector", "knowitall", "collector", [
        "I finally found {item} for {price}. It took me a week.",
        "A week? The vendor in {place} had three of them this morning.",
        "Do not tell me that. I was happier five seconds before I saw {item} at {place}."]),
    ("grumbler", "optimist", "cynic", [
        "Prices at {place} are robbery. {item} for {price} is robbery.",
        "At least the weather is nice, and nobody forced you to buy {item}.",
        "Someone did force him. It was his profession, and the {item} was {price}."]),
    ("joker", "grumbler", "gossip", [
        "I sold {item} to {creature} for {price}. It paid in copper.",
        "That is not funny. That is a future economic crisis at {place}.",
        "It is a little funny. I watched from {place}."]),
    ("gambler", "miser", "gambler", [
        "I bet {item} reaches {price} by tonight. Easy money.",
        "That is not an investment in {item}. That is a donation with extra steps.",
        "A donation of {price} is still a donation. Double or nothing."]),
    ("snob", "braggart", "gossip", [
        "Who would wear {item}? It looks like it lost a fight with {creature}.",
        "I would. I did. I looked magnificent, and I paid {price} for it.",
        "And now everyone at {place} is talking about it. Congratulations."]),
    ("soldier", "hothead", "veteran", [
        "Discipline wins battles. Even at {place}, discipline wins.",
        "Discipline did not win me {item} for {price}. Luck did.",
        "Luck is what discipline calls a good day. Buy the {item} and move on."]),
    ("pirate", "trader", "pirate", [
        "Arr. {item} for {price}, and no questions about where it came from.",
        "I have questions. The first is why {item} is wet.",
        "The sea has opinions about cargo. Take it at {price} or leave it."]),
    ("roleplayer", "cynic", "poet", [
        "Honour is worth more than {item}, even at {price}.",
        "Honour does not pay the auctioneer at {place}, my friend.",
        "No, but it makes a better story when the {item} is gone."]),
    ("philosopher", "miser", "joker", [
        "Gold is only a promise we all agree to believe, especially at {place}.",
        "I believe in {price} for {item}. That is a very specific promise.",
        "And I believe this conversation has cost me more than the {item}."]),
    ("tinkerer", "newbie", "tinkerer", [
        "This {item} is a marvel. I paid {price} and it only exploded twice.",
        "Twice? Is that normal for {item}?",
        "Normal is a strong word. The last {item} exploded more."]),
    ("competitive", "braggart", "gossip", [
        "I sold more {item} this week than anyone at {place}.",
        "You sold three {item}. I sold four, and I was not even trying.",
        "This is the most exciting thing that has happened at {place} all day."]),
    ("polite", "quiet", "polite", [
        "Good morning. Have you seen {item} at a fair price?",
        "Mm. {price}. Over there, by {place}.",
        "Thank you. I will remember that when {creature} comes for me."]),
    ("lazy", "optimist", "grumbler", [
        "I was going to farm {item}, but {place} is so far away.",
        "It is across the square. You can see {place} from here.",
        "Exactly. Far away. I will buy it for {price} and call it a bargain."]),
    ("socialite", "glutton", "gossip", [
        "After this, drinks at the inn. I found {item} and I am celebrating.",
        "I will come if there is food. Is there food at {place}?",
        "There is always food. That is why we meet at {place}."]),
    ("animalfriend", "collector", "animalfriend", [
        "I tracked {creature} for an hour and it dropped nothing but {item}.",
        "Nothing but {item}? That is worth {price} to the right buyer.",
        "Then I am the right buyer for {item}. Hand it over before I change my mind."]),
    ("weatherwatcher", "farmer", "weatherwatcher", [
        "Rain later. You can feel it in the air around {place}.",
        "Rain is good for {item}. Bad for my boots.",
        "Then buy better boots. I saw a pair for {price} this morning."]),
    ("detective", "conspiracist", "detective", [
        "Someone bought every {item} at {place} this morning.",
        "Every {item}? That is not a buyer. That is a pattern.",
        "A pattern with {price} and no witnesses. I will ask around."]),
    ("bard", "gossip", "bard", [
        "I am writing a song about {item} and the fool who paid {price}.",
        "Is the fool with the {item} in the room?",
        "He is standing at {place}, pretending not to listen."]),
    ("motherhen", "newbie", "motherhen", [
        "You are not going out there with that {item}. It is barely armour.",
        "But it was only {price}, and it matches my boots.",
        "Then we will find you something that keeps {creature} away. Come along."]),
    ("daredevil", "coward", "daredevil", [
        "I once fought {creature} for {item} and won. Barely.",
        "Barely is not a word I like in the same sentence as {creature}.",
        "It is the only word {creature} understands. That and run."]),
    ("critic", "poet", "critic", [
        "The market at {place} has no taste. {item} for {price} proves it.",
        "Perhaps the {item} is a poem nobody has learned to read yet.",
        "Some {item} should stay in the drawer."]),
]

TARGET = 1000
rows = []
script = 0
# Enough slot combinations for every template; the banks are cycled independently.
for template_index, (a, b, c, lines) in enumerate(TEMPLATES):
    for n in range(64):
        if script >= TARGET:
            break
        slots = {
            "item": ITEMS[(template_index * 3 + n) % len(ITEMS)],
            "price": PRICES[(template_index * 5 + n) % len(PRICES)],
            "place": PLACES[(template_index * 2 + n) % len(PLACES)],
            "creature": CREATURES[(template_index + n) % len(CREATURES)],
            "reason": REASONS[(template_index + n) % len(REASONS)],
        }
        script += 1
        for step, (personality, text) in enumerate(zip((a, b, c), lines)):
            text = text.format(**slots)
            rows.append((script, step, personality, text))

with open(OUT, "w", encoding="utf-8", newline="\n") as out:
    out.write("# Auction-house dialogues. One line each: id|step|personality|text\n")
    out.write("# Steps of one id are said in order by two or three nearby bots.\n")
    for row in rows:
        out.write("%d|%d|%s|%s\n" % row)
print("%d dialogues, %d lines" % (script, len(rows)))
