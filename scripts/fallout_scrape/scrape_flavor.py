"""Scrape Fallout flavor text from terminal entries, holotapes, and notes.

Strategy:
 - Enumerate `<Game> terminal entries`, `<Game> holotapes`, `<Game> notes` categories.
 - Fetch wikitext, extract individual log/message entries (short paragraphs ≤100 chars).
 - Classify into room_descriptions, find_descriptions, ambient_descriptions, npc_lines.
 - Output a dict matching scripts/rpg_data/flavor.json structure.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import mwparserfromhell as mwp

from wiki_client import iter_category_members, get_wikitext

SITE = "https://fallout.fandom.com/api.php"

# Per game: (category, flavor_class)
# flavor_class: "terminal" | "holotape" | "note" | "location"
GAME_FLAVOR_CATEGORIES: dict[str, list[tuple[str, str]]] = {
    "FO3": [
        ("Fallout 3 terminal entries", "terminal"),
        ("Fallout 3 holotapes", "holotape"),
        ("Fallout 3 notes", "note"),
    ],
    "NV": [
        ("Fallout: New Vegas terminal entries", "terminal"),
        ("Fallout: New Vegas holotapes", "holotape"),
        ("Fallout: New Vegas notes", "note"),
    ],
    "FO4": [
        ("Fallout 4 terminal entries", "terminal"),
        ("Fallout 4 holotapes", "holotape"),
        ("Fallout 4 notes", "note"),
    ],
}

# Tone classification keywords
GRIM_WORDS    = {"dead", "death", "blood", "corpse", "body", "silence", "dark", "bone",
                 "rot", "decay", "ash", "ruin", "skull", "grave", "died", "killed", "murder",
                 "terror", "fear", "screaming", "agony", "despair", "horror", "plague"}
HOPEFUL_WORDS = {"hope", "alive", "survive", "safe", "home", "family", "better", "rebuild",
                 "light", "future", "rescue", "saved", "joy", "love", "peace", "grow",
                 "found", "success", "free", "freedom", "music", "laughter", "children"}
TENSE_WORDS   = {"danger", "warning", "alert", "urgent", "breach", "attack", "hostile",
                 "threat", "enemy", "pursue", "armed", "locked", "secure", "guard",
                 "trap", "mine", "ambush", "patrol", "armed", "intruder"}

MAX_FLAVOR_LEN = 100
MIN_FLAVOR_LEN = 20


def classify_tone(text: str) -> str:
    low = text.lower()
    words = set(re.findall(r"\b\w+\b", low))
    grim    = len(words & GRIM_WORDS)
    hopeful = len(words & HOPEFUL_WORDS)
    tense   = len(words & TENSE_WORDS)
    if grim > hopeful and grim > tense:
        return "grim"
    if hopeful > grim and hopeful > tense:
        return "hopeful"
    if tense > grim and tense > hopeful:
        return "tense"
    return "grim"  # wasteland default


def clean_text(s: str) -> str:
    """Strip wiki markup and normalize whitespace."""
    if not s:
        return ""
    s = re.sub(r"<ref[^/]*?>.*?</ref>|<ref[^/]*?/>", "", s, flags=re.S | re.I)
    s = re.sub(r"<[^>]+>", "", s)
    # Strip templates
    for _ in range(6):
        new = re.sub(r"\{\{[^{}]*?\}\}", "", s)
        if new == s:
            break
        s = new
    # Strip links
    s = re.sub(r"\[\[([^\]|]+\|)?([^\]]+)\]\]", lambda m: m.group(2), s)
    s = s.replace("'''", "").replace("''", "")
    s = re.sub(r"\s+", " ", s).strip()
    return s


def is_good_flavor(text: str) -> bool:
    """Filter out junk: too short, too long, wiki meta-text, table artifacts."""
    if len(text) < MIN_FLAVOR_LEN or len(text) > MAX_FLAVOR_LEN:
        return False
    low = text.lower()

    # Wiki meta/navigation artifacts
    meta_phrases = [
        "category:", "file:", "image:", "see also", "references", "navigation",
        "edit this", "fallout wiki", "this page", "disambig",
        "in fallout 3", "in fallout 4", "in fallout: new vegas", "in fallout 2", "in fallout 1",
        "in fallout ()", "geck id", "form id", "base id", "ref id", "quest id",
        "picking up", "this item", "this weapon", "this armor", "this holotape",
        "holotape will", "terminal entries are", "are transcripts",
        "the player character", "the player can", "can be found in",
        "is a piece of", "is a type of", "is an item in", "is a weapon in",
        "is a location in", "is a creature in", "is a character in",
        "is a paper note", "is a terminal entry", "is a holotape", "is a note in",
        "is found in the", "is found on a", "is an entry found",
        "is the epilogue", "was only included",
        "does not spawn", "can be obtained", "npc dialogue",
        "making way up to", "is two floors", "first time, the note",
    ]
    if any(p in low for p in meta_phrases):
        return False

    # Wiki definition pattern: "The X is a [type]..." at start of article
    if re.match(r"^(the|a|an) \w[\w\s]+ is (a|an) (paper note|terminal|holotape|item|weapon"
                r"|piece of|type of|creature|character|location|quest|episode)", low):
        return False

    # Table header / wiki markup artifacts
    table_junk = ["name related", "g.e.c.k.", "form id", "base_id", "ref_id", "item id",
                  "editor id", "weight value", "|", "{{", "}}"]
    if any(p in low for p in table_junk):
        return False

    # Must start with a capital letter or quotation mark
    if not re.match(r'[A-Z"]', text):
        return False

    # Skip header-like lines (log metadata)
    if re.match(r"(entry|log|date|subject|from|to|re:|note:|memo:)\s*[:—\-–]", low):
        return False

    # Skip lines that are just numbers/dates/IDs
    if re.match(r"^[\d/\-:\s]+$", text):
        return False

    # Require at least a few real words (not just template names/IDs)
    word_count = len(re.findall(r"[a-zA-Z]{3,}", text))
    if word_count < 4:
        return False

    return True


def extract_log_entries(wikitext: str) -> list[str]:
    """Extract short individual log entries from terminal/holotape wikitext.

    Targets text between section headers or bullet points.
    Returns a list of cleaned strings, each ≤ MAX_FLAVOR_LEN chars.
    """
    if not wikitext:
        return []

    # Use mwparserfromhell to strip most markup
    try:
        plain = mwp.parse(wikitext).strip_code(normalize=True, collapse=True)
    except Exception:
        plain = clean_text(wikitext)

    entries: list[str] = []

    # Split on double newlines or == sections ==
    chunks = re.split(r"\n(?:==+[^=\n]+==+\s*)?\n+", plain)
    for chunk in chunks:
        chunk = chunk.strip()
        # Sub-split on newlines to get individual lines/sentences
        lines = [l.strip() for l in re.split(r"[.!?]\n|[\n;]", chunk) if l.strip()]
        for line in lines:
            line = re.sub(r"\s+", " ", line).strip()
            # Trim to MAX_FLAVOR_LEN at word boundary
            if len(line) > MAX_FLAVOR_LEN:
                trimmed = line[:MAX_FLAVOR_LEN].rsplit(" ", 1)[0].rstrip(",;:") + "..."
                entries.append(trimmed)
            elif len(line) >= MIN_FLAVOR_LEN:
                entries.append(line)

    # Deduplicate, filter, return
    seen: set[str] = set()
    result: list[str] = []
    for e in entries:
        if e not in seen and is_good_flavor(e):
            seen.add(e)
            result.append(e)
    return result


def classify_flavor_entry(text: str, source_class: str) -> str:
    """Map source class + text content to flavor category."""
    low = text.lower()
    # NPC lines: first person or direct speech from holotapes
    if source_class == "holotape":
        if re.search(r"\b(i|we|my|our|you|he|she|they)\b", low):
            return "npc_lines"
    # Terminal entries → ambient
    if source_class == "terminal":
        return "ambient_descriptions"
    # Notes: check content
    if any(w in low for w in ["found", "discovered", "noticed", "remember", "note", "dear"]):
        return "find_descriptions"
    if any(w in low for w in ["dust", "empty", "quiet", "silence", "hall", "room", "corridor"]):
        return "room_descriptions"
    return "ambient_descriptions"


def loc_type_from_context(title: str, text: str) -> str | None:
    """Infer loc_type for room_descriptions (None if not room_desc)."""
    combined = (title + " " + text).lower()
    if "vault" in combined:
        return "vault"
    if any(w in combined for w in ["raider", "bandit", "gang"]):
        return "raider_camp"
    if any(w in combined for w in ["ghoul", "ruin", "abandoned"]):
        return "ghoul_ruin"
    if any(w in combined for w in ["military", "fort", "army", "bunker"]):
        return "military"
    if any(w in combined for w in ["town", "city", "village", "settlement"]):
        return "town"
    if any(w in combined for w in ["cave", "mine", "cavern"]):
        return "cave"
    return "wasteland"


def scrape_game(game: str, limit: int | None = None) -> dict[str, list]:
    cats = GAME_FLAVOR_CATEGORIES.get(game, [])
    out: dict[str, list] = {
        "room_descriptions": [],
        "find_descriptions": [],
        "ambient_descriptions": [],
        "npc_lines": [],
    }
    total_pages = 0
    for cat_name, cat_class in cats:
        try:
            titles = iter_category_members(SITE, cat_name)
        except Exception as exc:
            print(f"  [{game}] category '{cat_name}' failed: {exc}", file=sys.stderr)
            continue
        if limit:
            titles = titles[:limit]
        print(f"  [{game}] {cat_name}: {len(titles)} pages", file=sys.stderr)
        for i, title in enumerate(titles, 1):
            try:
                wt = get_wikitext(SITE, title)
            except Exception as exc:
                print(f"  [{game}] skip '{title}': {type(exc).__name__}", file=sys.stderr)
                continue
            entries = extract_log_entries(wt)
            for text in entries:
                cat_key = classify_flavor_entry(text, cat_class)
                tone    = classify_tone(text)
                item: dict = {"text": text, "tone": tone, "source_game": game}
                if cat_key == "room_descriptions":
                    item["location_type"] = loc_type_from_context(title, text)
                out[cat_key].append(item)
            total_pages += 1
            if i % 25 == 0 or i == len(titles):
                counts = {k: len(v) for k, v in out.items()}
                print(f"    [{game}/{cat_class}] {i}/{len(titles)}  flavor={counts}", file=sys.stderr)
    print(f"  {game}: {total_pages} pages → {sum(len(v) for v in out.values())} flavor entries", file=sys.stderr)
    return out


def merge_flavor(a: dict[str, list], b: dict[str, list]) -> dict[str, list]:
    """Merge two flavor dicts, deduplicating by exact text."""
    result: dict[str, list] = {}
    for key in ("room_descriptions", "find_descriptions", "ambient_descriptions", "npc_lines"):
        seen: set[str] = set()
        merged: list[dict] = []
        for item in list(a.get(key, [])) + list(b.get(key, [])):
            txt = item.get("text", "")
            if txt and txt not in seen:
                seen.add(txt)
                merged.append(item)
        result[key] = merged
    return result


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", nargs="+", default=["FO3", "NV", "FO4"])
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--out", default=str(Path(__file__).parent / "scraped_flavor.json"))
    args = ap.parse_args()

    combined: dict[str, list] = {
        "room_descriptions": [],
        "find_descriptions": [],
        "ambient_descriptions": [],
        "npc_lines": [],
    }
    for game in args.games:
        if game not in GAME_FLAVOR_CATEGORIES:
            print(f"Unknown game: {game}", file=sys.stderr)
            sys.exit(1)
        game_flavor = scrape_game(game, limit=args.limit)
        combined = merge_flavor(combined, game_flavor)

    out = Path(args.out)
    out.write_text(json.dumps(combined, indent=2, ensure_ascii=False), encoding="utf-8")
    counts = {k: len(v) for k, v in combined.items()}
    print(f"\nWrote {sum(counts.values())} total flavor entries to {out}")
    print(f"  {counts}")


if __name__ == "__main__":
    main()
