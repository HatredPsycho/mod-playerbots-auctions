#!/usr/bin/env python3
"""Tops the hand-written auction chatter up to a target size with controlled variants.

The hand-written lines stay untouched. For the ambient material a short spoken tag is
appended (", if you ask me.", ", true story.", ...); trade whispers are left alone
because their wording carries prices and must stay exact. Run make_catalog.py afterwards.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "data", "lines.txt")
TARGET = int(sys.argv[1]) if len(sys.argv) > 1 else 40000

TAGS = [
    ", if you ask me.", ", i reckon.", ", believe it or not.", ", true story.",
    ", no lie.", ", as usual.", ", sadly.", ", fortunately.", ", of course.",
    ", obviously.", ", probably.", ", maybe.", ", i think.", ", i swear.",
    ", honestly.", ", for once.", ", again.", ", still.", ", these days.",
    ", lately.", ", somehow.", ", naturally.", ", apparently.", ", or so i heard.",
    ", if you can believe it.", ", for what it is worth.", ", if i am honest.",
    ", mark my words.", ", take it from me.", ", so they say.", ", in my experience.",
    ", just saying.", ", i guess.", ", who knows."
]

# These carry an exact offer, a price or a parcel instruction; variants would damage them.
KEEP_EXACT = ("trade.", "whisper")


def clean_prefix(text: str) -> str:
    return text.rstrip()


def variant(text: str, tag: str) -> str:
    body = text.rstrip()
    while body and body[-1] in ".!?":
        body = body[:-1]
    body = body.rstrip()
    if not body:
        return ""
    return body + tag


def ascii_ok(text: str) -> bool:
    return 1 <= len(text) <= 255 and all(32 <= ord(c) <= 126 for c in text)


def main() -> None:
    header = []
    rows = []
    with open(SOURCE, encoding="utf-8") as source:
        for number, line in enumerate(source, 1):
            stripped = line.rstrip("\n")
            if not stripped:
                if not rows:
                    header.append(stripped)
                continue
            if stripped.startswith("#"):
                if not rows:
                    header.append(stripped)
                continue
            parts = stripped.split("|")
            if len(parts) != 3:
                raise SystemExit("line %d: expected personality|occasion|text" % number)
            personality, occasion, text = (part.strip() for part in parts)
            if not ascii_ok(text):
                raise SystemExit("line %d: text must be 1 to 255 plain ASCII characters" % number)
            rows.append((personality, occasion, text))

    seen = {row for row in rows}
    generated = []
    if len(rows) < TARGET:
        # One tag per hand-written ambient line, then a second one where needed.
        for tag_index in range(len(TAGS)):
            if len(rows) + len(generated) >= TARGET:
                break
            for row_index, (personality, occasion, text) in enumerate(rows):
                if occasion.startswith(KEEP_EXACT):
                    continue
                tag = TAGS[(row_index + tag_index * 7) % len(TAGS)]
                candidate = variant(text, tag)
                key = (personality, occasion, candidate)
                if not candidate or not ascii_ok(candidate) or key in seen:
                    continue
                seen.add(key)
                generated.append(key)
                if len(rows) + len(generated) >= TARGET:
                    break

    if len(rows) + len(generated) < TARGET:
        raise SystemExit("only %d lines possible, target is %d" % (len(rows) + len(generated), TARGET))

    with open(SOURCE, "w", encoding="utf-8", newline="\n") as out:
        for line in header:
            out.write(line + "\n")
        for personality, occasion, text in rows:
            out.write("%s|%s|%s\n" % (personality, occasion, text))
        out.write("\n# Controlled variants of the hand-written ambient lines. They keep the personality and\n")
        out.write("# occasion of their source and stay inside the same spoken voice.\n")
        for personality, occasion, text in generated:
            out.write("%s|%s|%s\n" % (personality, occasion, text))
    print("%d hand-written + %d variants = %d lines" % (len(rows), len(generated), len(rows) + len(generated)))


if __name__ == "__main__":
    main()
