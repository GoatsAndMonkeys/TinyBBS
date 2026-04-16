"""Scrape Fallout weapons, armor, and consumables from fallout.fandom.com into items.json format.

Strategy:
 - Enumerate category `<Game> weapons`, `<Game> armor and clothing`, `<Game> consumables` for each game.
 - Fetch each page's wikitext.
 - Parse {{Infobox item*}} or {{Infobox weapon*}} / {{Infobox armor*}} templates.
 - Extract: damage, weight, value, type.
 - Normalize rarity 1-3 from value + name signals.
 - Output JSON entries compatible with scripts/rpg_data/items.json.
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

# Categories per game: (category name, default item type)
GAME_ITEM_CATEGORIES: dict[str, list[tuple[str, str]]] = {
    "FO1": [
        ("Fallout weapons", "weapon"),
        ("Fallout armor and clothing", "armor"),
        ("Fallout items", "consumable"),
    ],
    "FO2": [
        ("Fallout 2 weapons", "weapon"),
        ("Fallout 2 armor and clothing", "armor"),
        ("Fallout 2 items", "consumable"),
    ],
    "FO3": [
        ("Fallout 3 weapons", "weapon"),
        ("Fallout 3 armor and clothing", "armor"),
        ("Fallout 3 consumables", "consumable"),
    ],
    "NV": [
        ("Fallout: New Vegas weapons", "weapon"),
        ("Fallout: New Vegas armor and clothing", "armor"),
        ("Fallout: New Vegas consumables", "consumable"),
    ],
    "FO4": [
        ("Fallout 4 weapons", "weapon"),
        ("Fallout 4 armor and clothing", "armor"),
        ("Fallout 4 consumables", "consumable"),
    ],
}

# Unique weapon categories — membership bumps rarity to ≥ 3
UNIQUE_WEAPON_CATEGORIES: dict[str, str] = {
    "FO3": "Fallout 3 unique weapons",
    "NV": "Fallout: New Vegas unique weapons",
    "FO4": "Fallout 4 unique weapons",
}

# Wiki type field values → our taxonomy
WIKI_TYPE_MAP: dict[str, str] = {
    # weapons
    "pistol": "weapon", "revolver": "weapon", "rifle": "weapon",
    "shotgun": "weapon", "smg": "weapon", "submachine gun": "weapon",
    "heavy weapons": "weapon", "energy weapon": "weapon",
    "melee": "weapon", "unarmed": "weapon", "explosive": "weapon",
    "knife": "weapon", "sword": "weapon", "axe": "weapon", "spear": "weapon",
    "bow": "weapon", "crossbow": "weapon", "grenade": "weapon", "mine": "weapon",
    # armor
    "armor": "armor", "clothing": "armor", "helmet": "armor", "hat": "armor",
    "outfit": "armor", "headgear": "armor", "apparel": "armor",
    # consumables
    "food": "consumable", "drink": "consumable", "aid": "consumable",
    "chem": "consumable", "medicine": "consumable", "drug": "consumable",
    "stimpak": "consumable", "rad-x": "consumable", "radaway": "consumable",
    # junk/ammo
    "junk": "junk", "component": "junk", "ammo": "ammo", "ammunition": "ammo",
}

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


def clean_float(s: str) -> float | None:
    if not s:
        return None
    s = strip_wiki(s).replace(",", "").strip()
    m = re.match(r"[\d.]+", s)
    try:
        return float(m.group(0)) if m else None
    except ValueError:
        return None


def clean_int(s: str) -> int | None:
    v = clean_float(s)
    return int(v) if v is not None else None


def extract_damage(field: str) -> tuple[int, int] | None:
    """Return (min, max) damage from a wiki damage field, or None."""
    if not field:
        return None
    s = strip_wiki(field)
    # Range: "10-20" or "10 to 20"
    m = re.search(r"(\d+)\s*(?:[-–]|to)\s*(\d+)", s)
    if m:
        lo, hi = int(m.group(1)), int(m.group(2))
        if lo > hi:
            lo, hi = hi, lo
        return lo, hi
    # Single number
    m = re.search(r"(\d+)", s)
    if m:
        base = int(m.group(1))
        lo = max(1, int(base * 0.85))
        hi = max(lo + 1, int(base * 1.15))
        return lo, hi
    return None


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


def rarity_from_value(value: int, name: str) -> int:
    """Compute rarity 1-3 from caps value and name signals."""
    name_low = name.lower()
    # Named/unique signals in the item name
    unique_words = ["prototype", "legendary", "experimental", "alien", "unique",
                    "special", "gauss", "plasma", "railway", "tesla", "shishkebab",
                    "smuggler", "terrible", "victory", "jack", "ol' painful", "ratslayer",
                    "infiltrator", "medicine stick", "lucky", "gehenna"]
    if any(w in name_low for w in unique_words):
        return 3
    if value is None or value <= 0:
        return 1
    if value >= 500:
        return 3
    if value >= 100:
        return 2
    return 1


def classify_item_type(infobox_type: str, cat_type: str, title: str) -> str:
    """Determine final item type from wiki data and category context."""
    low = infobox_type.lower().strip() if infobox_type else ""
    # Check wiki type field
    for k, v in WIKI_TYPE_MAP.items():
        if k in low:
            return v
    # Fall back to category-based type
    return cat_type


def find_infobox(code: mwp.wikicode.Wikicode) -> mwp.nodes.template.Template | None:
    """Find the first infobox-like template on the page."""
    for t in code.filter_templates():
        tname = str(t.name).strip().lower()
        if tname.startswith("infobox item") or tname.startswith("infobox weapon") \
                or tname.startswith("infobox armor") or tname.startswith("infobox apparel"):
            return t
    return None


# Patterns that indicate a page is a category overview, list, or disambiguation —
# not an individual item entry.
_OVERVIEW_PATTERNS = re.compile(
    r"^(Fallout\s+\d*[:\s]|Fallout:\s+New Vegas\s+)(weapons?|armor|clothing|items?|"
    r"consumables?|ammunition|ammo|apparel|headgear|unique weapons?)$",
    re.I,
)
_OVERVIEW_WORDS = {"list of", "overview", "category:"}


def _is_overview_page(title: str) -> bool:
    low = title.lower()
    if any(w in low for w in _OVERVIEW_WORDS):
        return True
    if _OVERVIEW_PATTERNS.match(title):
        return True
    return False


def parse_item_page(game: str, title: str, wikitext: str, cat_type: str,
                    is_unique: bool = False) -> dict | None:
    """Return a normalized item dict, or None if unparseable."""
    if not wikitext.strip():
        return None
    if _is_overview_page(title):
        return None
    code = mwp.parse(wikitext)
    ib = find_infobox(code)
    # Require an infobox — bare list/overview pages won't have one
    if ib is None:
        return None

    def get_field(*names: str) -> str:
        if ib is None:
            return ""
        for n in names:
            if ib.has(n):
                return str(ib.get(n).value).strip()
        return ""

    # Extract fields
    raw_damage = get_field("damage", "damage1", "dmg", "dps", "attack_dmg", "attack", "damage_per_shot")
    raw_weight = get_field("weight", "wt")
    raw_value  = get_field("value", "caps", "price")
    raw_type   = get_field("type", "item_type", "category")

    # Damage → "min-max" string
    damage_str = "0"
    if cat_type == "weapon":
        pair = extract_damage(raw_damage)
        if pair:
            damage_str = f"{pair[0]}-{pair[1]}"

    weight = clean_float(raw_weight) or 0.0
    value  = clean_int(raw_value) or 0

    item_type = classify_item_type(raw_type, cat_type, title)
    # Skip ammo — not useful as standalone content entries
    if item_type == "ammo":
        return None

    rarity = rarity_from_value(value, title)
    if is_unique:
        rarity = max(rarity, 3)

    # Clean name
    name = re.sub(r"\s*\([^)]+\)\s*$", "", title).strip()

    intro = extract_intro(wikitext)
    desc  = first_sentence(intro, limit=95)

    return {
        "name": name,
        "type": item_type,
        "game_source": [game],
        "damage": damage_str,
        "weight": round(weight, 1),
        "value": value,
        "rarity": rarity,
        "description": desc,
        "_wiki_title": title,
    }


# Subcategory name substrings to skip (lowercased) — no-content structural cats
SKIP_ITEM_SUBCATS = ("cut ", "images", "sounds", "mentioned-only", "mentioned only",
                     "overview", "by game")


def scrape_game_items(game: str, limit: int | None = None) -> list[dict]:
    cats = GAME_ITEM_CATEGORIES.get(game, [])
    # Build unique-weapon set for this game (for rarity bump)
    unique_titles: set[str] = set()
    if game in UNIQUE_WEAPON_CATEGORIES:
        try:
            unique_titles = set(
                iter_category_members(SITE, UNIQUE_WEAPON_CATEGORIES[game])
            )
        except Exception as exc:
            print(f"  [{game}] unique weapons category failed: {exc}", file=sys.stderr)

    results: list[dict] = []
    for cat_name, cat_type in cats:
        try:
            # Recurse into subcategories (e.g. "FO3 handguns", "FO3 rifles")
            titles = iter_category_recursive(SITE, cat_name, skip_patterns=SKIP_ITEM_SUBCATS)
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
            is_unique = title in unique_titles
            try:
                entry = parse_item_page(game, title, wt, cat_type, is_unique)
            except Exception as exc:
                print(f"  [{game}] parse error '{title}': {exc}", file=sys.stderr)
                entry = None
            if entry:
                results.append(entry)
            if i % 25 == 0 or i == len(titles):
                print(f"    [{game}/{cat_type}] {i}/{len(titles)}  kept {len(results)}", file=sys.stderr)
    return results


def merge_by_name(entries: list[dict]) -> list[dict]:
    """Merge duplicate items across games: union game_source, keep best stats."""
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
        cur["value"]  = max(cur["value"], e["value"])
        cur["rarity"] = max(cur["rarity"], e["rarity"])
        cur["weight"] = max(cur["weight"], e["weight"])
        if not cur.get("description") and e.get("description"):
            cur["description"] = e["description"]
        # Keep more informative damage string
        if e["damage"] != "0" and cur["damage"] == "0":
            cur["damage"] = e["damage"]
    order = {"FO1": 0, "FO2": 1, "FO3": 2, "NV": 3, "FO4": 4}
    for v in by_name.values():
        v["game_source"].sort(key=lambda g: order.get(g, 99))
    return list(by_name.values())


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--games", nargs="+", default=["FO3", "NV", "FO4"])
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--out", default=str(Path(__file__).parent / "scraped_items.json"))
    args = ap.parse_args()

    all_entries: list[dict] = []
    for game in args.games:
        if game not in GAME_ITEM_CATEGORIES:
            print(f"Unknown game: {game}", file=sys.stderr)
            sys.exit(1)
        entries = scrape_game_items(game, limit=args.limit)
        print(f"{game}: {len(entries)} items parsed", file=sys.stderr)
        all_entries.extend(entries)

    merged = merge_by_name(all_entries)
    merged.sort(key=lambda e: (e["type"], e["rarity"], e["name"]))
    out = Path(args.out)
    out.write_text(json.dumps(merged, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"\nWrote {len(merged)} unique items to {out}")


if __name__ == "__main__":
    main()
