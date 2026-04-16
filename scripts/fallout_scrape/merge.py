"""Merge scraped wiki data into curated rpg_data/ JSONs.

Rules:
 - Curated entries (in rpg_data/) always win — they're hand-written and higher quality.
 - Dedup by name (case-insensitive).  Scraped entries whose names exist in curated are dropped.
 - For flavor: append scraped arrays to existing dict arrays, dedup by exact text.
 - Strip _wiki_title debug fields from final output.
 - Write back to rpg_data/*.json.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

REPO_ROOT   = Path(__file__).parent.parent.parent
SCRAPE_DIR  = Path(__file__).parent
DATA_DIR    = REPO_ROOT / "scripts" / "rpg_data"


def load_json(path: Path):
    if not path.exists():
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save_json(path: Path, data):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    print(f"  Wrote {path.name}")


def strip_debug(entry: dict) -> dict:
    """Remove scraper-only debug fields."""
    return {k: v for k, v in entry.items() if not k.startswith("_")}


def merge_list(curated: list[dict], scraped: list[dict]) -> list[dict]:
    """Merge two lists of dicts (enemies, items, or locations).

    Curated entries always win.  Adds new scraped entries only if their
    (case-insensitive) name doesn't already appear in curated.
    """
    curated_names = {e["name"].lower() for e in curated}
    new_entries   = [strip_debug(e) for e in scraped if e["name"].lower() not in curated_names]
    merged        = curated + new_entries
    added         = len(new_entries)
    return merged, added


def merge_flavor(curated: dict, scraped: dict) -> tuple[dict, int]:
    """Append scraped flavor into curated dict arrays, dedup by text."""
    result: dict = {}
    total_added = 0
    for key in ("room_descriptions", "find_descriptions", "ambient_descriptions", "npc_lines"):
        existing_texts = {e["text"] for e in curated.get(key, [])}
        new_items: list[dict] = []
        for item in scraped.get(key, []):
            if item.get("text") and item["text"] not in existing_texts:
                existing_texts.add(item["text"])
                new_items.append(strip_debug(item))
        result[key] = curated.get(key, []) + new_items
        total_added += len(new_items)
    return result, total_added


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Merge scraped Fallout wiki data into curated rpg_data/ JSONs.")
    ap.add_argument("--dry-run", action="store_true", help="Print what would change, don't write")
    args = ap.parse_args()

    changes: list[str] = []

    # ── Enemies ──────────────────────────────────────────────────────────────
    curated_enemies = load_json(DATA_DIR / "enemies.json") or []
    scraped_enemies = load_json(SCRAPE_DIR / "scraped_enemies.json") or []
    if scraped_enemies:
        merged_enemies, added = merge_list(curated_enemies, scraped_enemies)
        merged_enemies.sort(key=lambda e: (e["difficulty"], e["name"]))
        changes.append(f"enemies: +{added} new (total {len(merged_enemies)})")
        if not args.dry_run:
            save_json(DATA_DIR / "enemies.json", merged_enemies)
    else:
        changes.append("enemies: no scraped data")

    # ── Items ─────────────────────────────────────────────────────────────────
    curated_items = load_json(DATA_DIR / "items.json") or []
    scraped_items = load_json(SCRAPE_DIR / "scraped_items.json") or []
    if scraped_items:
        merged_items, added = merge_list(curated_items, scraped_items)
        merged_items.sort(key=lambda e: (e["type"], e.get("rarity", 1), e["name"]))
        changes.append(f"items: +{added} new (total {len(merged_items)})")
        if not args.dry_run:
            save_json(DATA_DIR / "items.json", merged_items)
    else:
        changes.append("items: no scraped data")

    # ── Locations ─────────────────────────────────────────────────────────────
    curated_locs = load_json(DATA_DIR / "locations.json") or []
    scraped_locs = load_json(SCRAPE_DIR / "scraped_locations.json") or []
    if scraped_locs:
        merged_locs, added = merge_list(curated_locs, scraped_locs)
        merged_locs.sort(key=lambda e: (e["type"], e.get("danger_level", 1), e["name"]))
        changes.append(f"locations: +{added} new (total {len(merged_locs)})")
        if not args.dry_run:
            save_json(DATA_DIR / "locations.json", merged_locs)
    else:
        changes.append("locations: no scraped data")

    # ── Flavor ────────────────────────────────────────────────────────────────
    curated_flavor = load_json(DATA_DIR / "flavor.json") or {}
    scraped_flavor = load_json(SCRAPE_DIR / "scraped_flavor.json") or {}
    if scraped_flavor:
        merged_flavor, added = merge_flavor(curated_flavor, scraped_flavor)
        changes.append(f"flavor: +{added} new entries (total {sum(len(v) for v in merged_flavor.values())})")
        if not args.dry_run:
            save_json(DATA_DIR / "flavor.json", merged_flavor)
    else:
        changes.append("flavor: no scraped data")

    print("\nMerge summary:")
    for c in changes:
        print(f"  {c}")

    if args.dry_run:
        print("\n[dry-run] No files written.")


if __name__ == "__main__":
    main()
