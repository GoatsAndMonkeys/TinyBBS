"""Scrape Fallout locations from fallout.fandom.com into locations.json format.

Strategy:
 - Enumerate `<Game> locations` categories for each game.
 - Parse {{Infobox location}} for type, factions, description.
 - Infer typical_enemies from the Creatures/Inhabitants sections.
 - Map wiki location types to LTYPE_* codes from content_tables.h.
 - Danger level derived from location type + faction signals.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import mwparserfromhell as mwp

from wiki_client import iter_category_members, iter_category_recursive, get_wikitext

SITE = "https://fallout.fandom.com/api.php"

GAME_LOC_CATEGORIES: dict[str, str] = {
    "FO1": "Fallout locations",
    "FO2": "Fallout 2 locations",
    "FO3": "Fallout 3 locations",
    "NV": "Fallout: New Vegas locations",
    "FO4": "Fallout 4 locations",
}

# Map wiki location type strings → our LTYPE_* codes (strings for gen_rpg_packed.py)
LOCATION_TYPE_MAP: list[tuple[str, str]] = [
    # Most specific first
    ("vault", "vault"),
    ("military base", "military"), ("military installation", "military"),
    ("fort", "military"), ("army", "military"), ("bunker", "military"),
    ("camp", "raider_camp"), ("raider base", "raider_camp"), ("raider camp", "raider_camp"),
    ("gang hideout", "raider_camp"), ("hideout", "raider_camp"),
    ("ghoul", "ghoul_ruin"),
    ("cave", "cave"), ("cavern", "cave"), ("mine", "cave"), ("tunnel", "cave"),
    ("town", "town"), ("city", "town"), ("settlement", "town"),
    ("village", "town"), ("outpost", "town"), ("trading", "town"),
    ("station", "town"), ("marketplace", "town"), ("community", "town"),
    ("ruin", "wasteland"), ("factory", "wasteland"), ("plant", "wasteland"),
    ("facility", "wasteland"), ("lab", "wasteland"), ("laboratory", "wasteland"),
    ("hospital", "wasteland"), ("school", "wasteland"), ("hotel", "wasteland"),
    ("office", "wasteland"), ("building", "wasteland"),
]

# Danger level hints from the type/description
DANGER_HINTS: list[tuple[str, int]] = [
    ("raider", 3), ("super mutant", 4), ("ghoul", 3), ("deathclaw", 4),
    ("brotherhood", 2), ("ncr", 2), ("legion", 3), ("institute", 3),
    ("super-mutant", 4), ("quarry", 3), ("military", 3), ("vault", 2),
    ("town", 1), ("settlement", 1), ("city", 1),
]

# Strip wiki markup
RE_REF = re.compile(r"<ref[^/]*?>.*?</ref>|<ref[^/]*?/>", re.S | re.I)
RE_HTML = re.compile(r"<[^>]+>")
RE_LINK = re.compile(r"\[\[([^\]|]+\|)?([^\]]+)\]\]")
RE_TEMPLATE = re.compile(r"\{\{[^{}]*?\}\}")
RE_WS = re.compile(r"\s+")


def strip_wiki(s: str) -> str:
    if not s:
        return ""
    s = RE_REF.sub("", s)
    for _ in range(6):
        new = RE_TEMPLATE.sub("", s)
        if new == s:
            break
        s = new
    s = RE_LINK.sub(lambda m: m.group(2), s)
    s = RE_HTML.sub("", s)
    s = s.replace("'''", "").replace("''", "")
    return RE_WS.sub(" ", s).strip()


def first_sentence(text: str, limit: int = 95) -> str:
    if not text:
        return ""
    cleaned = re.sub(r"\s+", " ", text).strip()
    m = re.search(r"[.!?]\s", cleaned)
    sentence = cleaned[: m.start() + 1].strip() if m else cleaned
    if len(sentence) <= limit:
        return sentence
    cut = sentence[:limit].rsplit(" ", 1)[0]
    return cut.rstrip(",;:") + "..."


def extract_intro(wikitext: str) -> str:
    if not wikitext:
        return ""
    head_m = re.search(r"^==[^=]", wikitext, re.M)
    body = wikitext[: head_m.start()] if head_m else wikitext
    try:
        text = mwp.parse(body).strip_code(normalize=True, collapse=True)
    except Exception:
        text = strip_wiki(body)
    for para in re.split(r"\n\s*\n", text):
        p = para.strip()
        if len(p) < 20:
            continue
        if not re.match(r"[A-Z]", p):
            continue
        if p.startswith('"') or p.startswith("'"):
            continue
        return p
    return ""


def map_location_type(raw: str) -> str:
    if not raw:
        return "wasteland"
    low = raw.lower().strip()
    for keyword, ltype in LOCATION_TYPE_MAP:
        if keyword in low:
            return ltype
    return "wasteland"


def danger_from_context(loc_type: str, text: str) -> int:
    """Heuristic danger 1-4 from type and descriptive text."""
    base = {
        "vault": 2, "wasteland": 2, "raider_camp": 3, "ghoul_ruin": 3,
        "military": 3, "town": 1, "cave": 2,
    }.get(loc_type, 2)
    low = text.lower()
    for kw, d in DANGER_HINTS:
        if kw in low:
            return max(base, d) if d > base else base
    return base


def extract_typical_enemies(wikitext: str, title: str) -> list[str]:
    """Extract creature names from Creatures/Inhabitants sections and Infobox."""
    enemies: list[str] = []

    # Look for ==Creatures== or ==Inhabitants== section
    creature_patterns = [
        r"==\s*(?:Creatures?|Notable inhabitants?|Residents?|Enemies)\s*==\s*(.*?)(?===|\Z)",
    ]
    for pat in creature_patterns:
        m = re.search(pat, wikitext, re.S | re.I)
        if not m:
            continue
        section = m.group(1)
        # Extract linked creature names from bullet lists
        for link_m in re.finditer(r"\[\[([^\]|]+)(?:\|([^\]]+))?\]\]", section):
            name = (link_m.group(2) or link_m.group(1)).strip()
            name = re.sub(r"\s*\([^)]+\)\s*$", "", name).strip()
            if name and len(name) > 2:
                # Filter out obvious non-creature links
                skip_words = ["category", "image", "file", "fallout", "link", "see"]
                if not any(sw in name.lower() for sw in skip_words):
                    enemies.append(name)
        break

    # Also check Infobox location fields: creatures, inhabitants, factions
    try:
        code = mwp.parse(wikitext)
        for t in code.filter_templates():
            tname = str(t.name).strip().lower()
            if not tname.startswith("infobox location"):
                continue
            for field in ("creatures", "inhabitants", "enemies"):
                if not t.has(field):
                    continue
                val = str(t.get(field).value).strip()
                for link_m in re.finditer(r"\[\[([^\]|]+)(?:\|([^\]]+))?\]\]", val):
                    name = (link_m.group(2) or link_m.group(1)).strip()
                    name = re.sub(r"\s*\([^)]+\)\s*$", "", name).strip()
                    if name and len(name) > 2:
                        enemies.append(name)
    except Exception:
        pass

    # Deduplicate, preserve order, max 4
    seen: set[str] = set()
    result: list[str] = []
    for e in enemies:
        low = e.lower()
        if low not in seen:
            seen.add(low)
            result.append(e)
        if len(result) >= 4:
            break
    return result


def parse_location_page(game: str, title: str, wikitext: str) -> dict | None:
    if not wikitext.strip():
        return None

    code = mwp.parse(wikitext)

    # Find infobox location — REQUIRED (filters out overview/list pages)
    ib = None
    for t in code.filter_templates():
        tname = str(t.name).strip().lower()
        if tname.startswith("infobox location") or tname.startswith("location infobox"):
            ib = t
            break

    if ib is None:
        return None  # Not a real location article

    def get_field(*names: str) -> str:
        for n in names:
            if ib.has(n):
                return strip_wiki(str(ib.get(n).value))
        return ""

    raw_type = get_field("type", "loc_type", "location_type", "category")
    loc_type = map_location_type(raw_type)

    name = re.sub(r"\s*\([^)]+\)\s*$", "", title).strip()

    intro = extract_intro(wikitext)
    desc  = first_sentence(intro, limit=95)

    enemies = extract_typical_enemies(wikitext, title)

    # Build danger context: loc_type + name + factions from infobox
    factions_text = get_field("factions", "faction", "leaders", "inhabitants")
    context = f"{raw_type} {name} {factions_text} {intro}"
    danger = danger_from_context(loc_type, context)

    return {
        "name": name,
        "type": loc_type,
        "game_source": [game],
        "typical_enemies": enemies,
        "description": desc,
        "danger_level": danger,
        "_wiki_title": title,
    }


SKIP_LOC_SUBCATS = ("cut ", "images", "sounds", "mentioned-only", "mentioned only",
                    "overview", "maps", "housing", "community", "communities",
                    "by game")


def scrape_game(game: str, limit: int | None = None) -> list[dict]:
    cat = GAME_LOC_CATEGORIES[game]
    # Recurse 1 level into subcategories to get actual location pages.
    # Deeper recursion would hit thousands of pages; 1 level gives us the
    # main game + DLC locations without exhaustive traversal.
    titles = iter_category_recursive(SITE, cat, skip_patterns=SKIP_LOC_SUBCATS, max_depth=1)
    if limit:
        titles = titles[:limit]
    results: list[dict] = []
    for i, title in enumerate(titles, 1):
        try:
            wt = get_wikitext(SITE, title)
        except Exception as exc:
            print(f"  [{game}] skip '{title}': {type(exc).__name__}", file=sys.stderr)
            continue
        try:
            entry = parse_location_page(game, title, wt)
        except Exception as exc:
            print(f"  [{game}] parse error '{title}': {exc}", file=sys.stderr)
            entry = None
        if entry:
            results.append(entry)
        if i % 25 == 0 or i == len(titles):
            print(f"  [{game}] {i}/{len(titles)}  kept {len(results)}", file=sys.stderr)
    return results


def merge_by_name(entries: list[dict]) -> list[dict]:
    by_name: dict[str, dict] = {}
    for e in entries:
        key = e["name"].lower()
        if key not in by_name:
            by_name[key] = {**e, "game_source": list(e["game_source"])}
            continue
        cur = by_name[key]
        for g in e["game_source"]:
            if g not in cur["game_source"]:
                cur["game_source"].append(g)
        cur["danger_level"] = max(cur["danger_level"], e["danger_level"])
        # Union typical_enemies (dedup)
        seen = set(cur["typical_enemies"])
        for en in e.get("typical_enemies", []):
            if en not in seen:
                cur["typical_enemies"].append(en)
                seen.add(en)
        cur["typical_enemies"] = cur["typical_enemies"][:4]
        if not cur.get("description") and e.get("description"):
            cur["description"] = e["description"]
    order = {"FO1": 0, "FO2": 1, "FO3": 2, "NV": 3, "FO4": 4}
    for v in by_name.values():
        v["game_source"].sort(key=lambda g: order.get(g, 99))
    return list(by_name.values())


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", nargs="+", default=["FO3", "NV", "FO4"])
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--out", default=str(Path(__file__).parent / "scraped_locations.json"))
    args = ap.parse_args()

    all_entries: list[dict] = []
    for game in args.games:
        if game not in GAME_LOC_CATEGORIES:
            print(f"Unknown game: {game}", file=sys.stderr)
            sys.exit(1)
        entries = scrape_game(game, limit=args.limit)
        print(f"{game}: {len(entries)} locations parsed", file=sys.stderr)
        all_entries.extend(entries)

    merged = merge_by_name(all_entries)
    merged.sort(key=lambda e: (e["type"], e["danger_level"], e["name"]))
    out = Path(args.out)
    out.write_text(json.dumps(merged, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"\nWrote {len(merged)} unique locations to {out}")


if __name__ == "__main__":
    main()
