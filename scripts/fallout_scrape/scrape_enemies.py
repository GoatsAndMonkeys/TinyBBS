"""Scrape Fallout creatures/NPCs from fallout.fandom.com into enemies.json format.

Strategy:
 - Enumerate category `<Game> creatures` for each game (FO1/2/3/NV/FO4).
 - Fetch each page's wikitext.
 - Parse main {{Infobox creature}} for type/faction hints + intro paragraph.
 - Parse {{Stats creature <GAME>}} rows — that's where HP/DR/damage/items live.
 - Normalize stats into RPG ranges (hp_min/hp_max, damage_min/damage_max, difficulty 1-4).
 - Map wiki types to our FACTION_* codes.
 - Output JSON entries compatible with scripts/rpg_data/enemies.json.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Iterable

import mwparserfromhell as mwp

from wiki_client import iter_category_members, iter_category_recursive, get_wikitext

SITE = "https://fallout.fandom.com/api.php"

GAME_CATEGORIES = {
    "FO1": "Fallout creatures",
    "FO2": "Fallout 2 creatures",
    "FO3": "Fallout 3 creatures",
    "NV": "Fallout: New Vegas creatures",
    "FO4": "Fallout 4 creatures",
}

# Specific keyword hints in title/intro — checked BEFORE the generic type
# field, because most pages have type=creature even for centaurs/mutants/etc.
# Order matters: most specific first.
FACTION_HINTS = [
    ("super mutant", "mutant"),
    ("feral ghoul", "ghoul"),
    ("ghoul", "ghoul"),
    ("centaur", "mutant"),
    ("floater", "mutant"),
    ("abomination", "mutant"),
    ("synth", "robot"),
    ("robobrain", "robot"),
    ("sentry bot", "robot"),
    ("mr. handy", "robot"),
    ("mister handy", "robot"),
    ("protectron", "robot"),
    ("assaultron", "robot"),
    ("eyebot", "robot"),
    ("mr. gutsy", "robot"),
    ("robot", "robot"),
    ("raider", "raider"),
]

# Wiki type= field values mapped to our FACTION_* taxonomy.
TYPE_TO_FACTION = {
    "animal": "creature",
    "creature": "creature",
    "insect": "creature",
    "mutated animal": "creature",
    "mutated insect": "creature",
    "giant insect": "creature",
    "abomination": "mutant",
    "super mutant": "mutant",
    "centaur": "mutant",
    "floater": "mutant",
    "ghoul": "ghoul",
    "feral ghoul": "ghoul",
    "robot": "robot",
    "robot/computer": "robot",
    "synth": "robot",
    "human": "human",
    "raider": "raider",
}

# Strip wiki markup: icons, links, templates, bold/italic, refs.
RE_REF = re.compile(r"<ref[^/]*?>.*?</ref>|<ref[^/]*?/>", re.S | re.I)
RE_HTML = re.compile(r"<[^>]+>")
RE_LINK = re.compile(r"\[\[([^\]|]+\|)?([^\]]+)\]\]")
RE_TEMPLATE = re.compile(r"\{\{[^{}]*?\}\}")
RE_WS = re.compile(r"\s+")


def strip_wiki(s: str) -> str:
    if not s:
        return ""
    s = RE_REF.sub("", s)
    # Repeatedly strip templates (handles nested simply enough for infobox fields)
    for _ in range(6):
        new = RE_TEMPLATE.sub("", s)
        if new == s:
            break
        s = new
    s = RE_LINK.sub(lambda m: m.group(2), s)
    s = RE_HTML.sub("", s)
    s = s.replace("'''", "").replace("''", "")
    return RE_WS.sub(" ", s).strip()


def clean_int(s: str) -> int | None:
    if not s:
        return None
    s = strip_wiki(s).replace(",", "").strip()
    m = re.match(r"-?\d+", s)
    return int(m.group(0)) if m else None


def extract_damage(attack_field: str) -> int | None:
    """Pull a representative damage number from an attack1 field.

    Examples seen:
      "Melee (100 {{Icon|damage|link=Damage}})"
      "[[Claw]] (25{{Icon|damage|link=Damage}})"
      "Bite (15 damage)"
      "Spit (40 {{Icon|damage}} + 2 {{Icon|rad}})"
    """
    if not attack_field:
        return None
    cleaned = strip_wiki(attack_field)
    # Find first run of digits
    m = re.search(r"(\d+)", cleaned)
    return int(m.group(1)) if m else None


def first_sentence(text: str, limit: int = 100) -> str:
    """Trim intro paragraph to one punchy sentence, <= limit chars."""
    if not text:
        return ""
    # Take up to first period/exclamation, but not decimals like 2.5
    cleaned = re.sub(r"\s+", " ", text).strip()
    # Try sentence boundary first
    m = re.search(r"[.!?]\s", cleaned)
    sentence = cleaned[: m.start() + 1].strip() if m else cleaned
    if len(sentence) <= limit:
        return sentence
    # Hard truncate at word boundary
    cut = sentence[:limit].rsplit(" ", 1)[0]
    return cut.rstrip(",;:") + "..."


def extract_intro(wikitext: str) -> str:
    """Pull the first narrative paragraph from wikitext.

    Pattern: after top-level templates (Infobox, Quotation, For, Games), before
    the first ==Heading==. Then strip_code to drop links/refs/formatting.
    """
    if not wikitext:
        return ""
    # Cut at first section heading
    head_m = re.search(r"^==[^=]", wikitext, re.M)
    body = wikitext[: head_m.start()] if head_m else wikitext
    # Use mwparserfromhell to strip markup cleanly
    try:
        text = mwp.parse(body).strip_code(normalize=True, collapse=True)
    except Exception:
        text = strip_wiki(body)
    # Find the first paragraph that looks like a real sentence (has a verb,
    # starts with a capital letter, length >= 20). Skip leftover crumbs.
    for para in re.split(r"\n\s*\n", text):
        p = para.strip()
        if len(p) < 20:
            continue
        if not re.match(r"[A-Z]", p):
            continue
        # Skip quote-lead-ins that slipped through
        if p.startswith('"') or p.startswith("'"):
            continue
        return p
    return ""


def classify_faction(infobox_type: str, title: str, wikitext: str) -> str:
    # Specific hints beat the generic type field — check title/intro first.
    low = (title + " " + wikitext[:2000]).lower()
    for kw, fac in FACTION_HINTS:
        if kw in low:
            return fac
    t = (infobox_type or "").lower().strip()
    if t in TYPE_TO_FACTION:
        return TYPE_TO_FACTION[t]
    return "creature"


def difficulty_from_stats(hp_max: int, dmg_max: int) -> int:
    """Bucket to 1-4 based on combined threat. Matches existing curated data spread."""
    score = hp_max + dmg_max * 5
    if score >= 900:
        return 4
    if score >= 400:
        return 3
    if score >= 150:
        return 2
    return 1


def normalize_hp_range(hp: int) -> tuple[int, int]:
    """Wiki gives one 'hp' number. Spread it ±25% to match our min/max format."""
    lo = max(1, int(hp * 0.75))
    hi = max(lo + 1, int(hp * 1.25))
    return lo, hi


def normalize_dmg_range(dmg: int) -> tuple[int, int]:
    lo = max(1, int(dmg * 0.7))
    hi = max(lo + 1, int(dmg * 1.3))
    return lo, hi


def xp_for(hp_max: int, dmg_max: int) -> int:
    """Rough XP: scales with hp + dmg. Caps to fit uint16 but we stay well under."""
    xp = int(hp_max * 0.3 + dmg_max * 4)
    return max(10, min(xp, 1000))


def parse_loot(items_field: str) -> list[dict]:
    """Extract loot item names from a stats template `items` field.

    The field looks like:
      * {{Icon|dead}} 100% [[Deathclaw hand (Fallout 3)|deathclaw hand]]
      * {{Icon|dead}} 50% [[Molerat meat]]
    We want item name + a weight (convert 100% → 100, etc.).
    """
    if not items_field:
        return []
    loot: list[dict] = []
    for line in items_field.splitlines():
        line = line.strip()
        if not line or not line.startswith("*"):
            continue
        line = line.lstrip("*").strip()
        # Find percent
        pct_m = re.search(r"(\d+)\s*%", line)
        weight = int(pct_m.group(1)) if pct_m else 50
        # Get linked item name (use display text after |, else link target)
        link_m = re.search(r"\[\[([^\]|]+)(?:\|([^\]]+))?\]\]", line)
        if not link_m:
            continue
        name = (link_m.group(2) or link_m.group(1)).strip()
        # Strip " (Fallout 3)" etc. and leading articles
        name = re.sub(r"\s*\([^)]+\)\s*$", "", name).strip()
        if not name:
            continue
        # Title-case for consistency with curated data
        name = name[0].upper() + name[1:]
        loot.append({"item": name, "weight": max(1, min(weight, 100))})
    # Dedup by item name, keep highest weight
    best: dict[str, int] = {}
    for e in loot:
        if e["item"] not in best or best[e["item"]] < e["weight"]:
            best[e["item"]] = e["weight"]
    return [{"item": k, "weight": v} for k, v in best.items()]


def parse_creature_page(game: str, title: str, wikitext: str) -> dict | None:
    """Return a single normalized enemy dict, or None if unparseable."""
    if not wikitext.strip():
        return None
    code = mwp.parse(wikitext)

    # Main creature infobox
    infobox_type = ""
    for t in code.filter_templates():
        tname = str(t.name).strip().lower()
        if tname.startswith("infobox creature"):
            if t.has("type"):
                infobox_type = strip_wiki(str(t.get("type").value))
            break

    # Collect all stats rows from {{Stats creature <GAME>|1=row|...}} templates.
    # FO2 shares FO1's engine and uses "Stats creature FO1" templates.
    game_tag_map: dict[str, tuple[str, ...]] = {
        "FO1": ("fo1",),
        "FO2": ("fo1", "fo2"),  # FO2 pages use Stats creature FO1 templates
        "FO3": ("fo3",),
        "NV": ("fnv",),
        "FO4": ("fo4",),
    }
    stat_rows: list[dict] = []
    for t in code.filter_templates():
        tname = str(t.name).strip().lower()
        if not tname.startswith("stats creature "):
            continue
        # Only this game's stats
        tname_nospace = tname.replace(" ", "")
        if not any(tag in tname_nospace for tag in game_tag_map[game]):
            continue
        # Distinguish header vs row: first positional param "1"
        first = str(t.get(1).value).strip().lower() if t.has(1) else ""
        if first != "row":
            continue
        row: dict = {}
        for p in t.params:
            pn = str(p.name).strip().lower()
            pv = str(p.value).strip()
            row[pn] = pv
        stat_rows.append(row)

    if not stat_rows:
        return None

    # Pick the "best" row: highest hp (captures the strongest variant, matches our naming).
    best = max(
        stat_rows,
        key=lambda r: clean_int(r.get("hp", "")) or 0,
    )
    hp = clean_int(best.get("hp", ""))
    if not hp or hp < 1:
        return None
    dmg = extract_damage(best.get("attack1", "") or best.get("attack", ""))
    if not dmg or dmg < 1:
        # Many pages lack attack1 — derive conservative default from hp
        dmg = max(2, hp // 20)
    dr = clean_int(best.get("dr", "") or best.get("defense_rating", "")) or 0

    hp_min, hp_max = normalize_hp_range(hp)
    dmg_min, dmg_max = normalize_dmg_range(dmg)
    diff = difficulty_from_stats(hp_max, dmg_max)
    xp = xp_for(hp_max, dmg_max)

    faction = classify_faction(infobox_type, title, wikitext)

    loot = parse_loot(best.get("items", ""))[:3]

    # Name: strip " (Fallout X)" suffix
    name = re.sub(r"\s*\([^)]+\)\s*$", "", title).strip()

    # Description from wikitext intro (no extra API call)
    intro = extract_intro(wikitext)
    desc = first_sentence(intro, limit=95)

    return {
        "name": name,
        "game_source": [game],
        "faction": faction,
        "hp_min": hp_min,
        "hp_max": hp_max,
        "damage_min": dmg_min,
        "damage_max": dmg_max,
        "defense_rating": max(0, min(dr, 100)),
        "xp_reward": xp,
        "loot_table": loot,
        "description": desc,
        "difficulty": diff,
        # Internal: wiki page title for dedup / debugging
        "_wiki_title": title,
    }


# Subcategory name substrings to skip (lowercased)
SKIP_SUBCATS = ("cut ", "images", "sounds", "mentioned-only", "mentioned only")


def scrape_game(game: str, limit: int | None = None) -> list[dict]:
    cat = GAME_CATEGORIES[game]
    # Recurse into DLC subcategories; skip cut/image/sound/mentioned-only subcats
    titles = iter_category_recursive(SITE, cat, skip_patterns=SKIP_SUBCATS)
    if limit:
        titles = titles[:limit]
    results: list[dict] = []
    cat_low = cat.lower()
    for i, title in enumerate(titles, 1):
        # Skip the category overview page itself
        if title.lower() == cat_low:
            continue
        wt = get_wikitext(SITE, title)
        entry = parse_creature_page(game, title, wt)
        if entry:
            results.append(entry)
        if i % 25 == 0 or i == len(titles):
            print(f"  [{game}] {i}/{len(titles)}  kept {len(results)}", file=sys.stderr)
    return results


def merge_by_name(entries: list[dict]) -> list[dict]:
    """Combine duplicate entries across games: union game_source, take best stats."""
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
        # Merge stats by taking widest range + best loot
        cur["hp_min"] = min(cur["hp_min"], e["hp_min"])
        cur["hp_max"] = max(cur["hp_max"], e["hp_max"])
        cur["damage_min"] = min(cur["damage_min"], e["damage_min"])
        cur["damage_max"] = max(cur["damage_max"], e["damage_max"])
        cur["defense_rating"] = max(cur["defense_rating"], e["defense_rating"])
        cur["xp_reward"] = max(cur["xp_reward"], e["xp_reward"])
        cur["difficulty"] = max(cur["difficulty"], e["difficulty"])
        if not cur.get("description") and e.get("description"):
            cur["description"] = e["description"]
        # Merge loot by name (keep highest weight)
        seen_loot = {lt["item"]: lt["weight"] for lt in cur.get("loot_table", [])}
        for lt in e.get("loot_table", []):
            if lt["item"] not in seen_loot or lt["weight"] > seen_loot[lt["item"]]:
                seen_loot[lt["item"]] = lt["weight"]
        cur["loot_table"] = [{"item": k, "weight": v} for k, v in list(seen_loot.items())[:3]]
    # Canonical order
    order = {"FO1": 0, "FO2": 1, "FO3": 2, "NV": 3, "FO4": 4}
    for v in by_name.values():
        v["game_source"].sort(key=lambda g: order.get(g, 99))
    return list(by_name.values())


def main():
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("--games", nargs="+", default=["FO3", "NV", "FO4"])
    ap.add_argument("--limit", type=int, default=None, help="max pages per game (debug)")
    ap.add_argument("--out", default=str(Path(__file__).parent / "scraped_enemies.json"))
    args = ap.parse_args()

    all_entries: list[dict] = []
    for game in args.games:
        if game not in GAME_CATEGORIES:
            print(f"Unknown game: {game}", file=sys.stderr)
            sys.exit(1)
        entries = scrape_game(game, limit=args.limit)
        print(f"{game}: {len(entries)} creatures parsed", file=sys.stderr)
        all_entries.extend(entries)

    merged = merge_by_name(all_entries)
    merged.sort(key=lambda e: (e["difficulty"], e["name"]))
    out = Path(args.out)
    out.write_text(json.dumps(merged, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"\nWrote {len(merged)} unique enemies to {out}")


if __name__ == "__main__":
    main()
