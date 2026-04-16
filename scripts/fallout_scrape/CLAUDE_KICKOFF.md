# Kickoff Prompt for Claude (dangerous-permissions session)

Paste everything below the `---` into a fresh Claude Code session started with `--dangerously-skip-permissions` in `/Users/goatsandmonkeys/Documents/mesh_bbs`.

---

You are picking up a Fallout wiki content-scraping pipeline mid-work. Read `scripts/fallout_scrape/HANDOFF.md` in full before doing anything — it has the goal, schemas, gotchas, and file paths. Also read the relevant memories: `project_rpg_content.md`, `project_seeed_tracker_l1_qspi.md`, `feedback_xmodem_upload.md`.

## What you're doing

Expanding the on-device Fallout RPG content (Seeed Wio Tracker L1, 1.4 MB free on external QSPI flash) by mining Fandom's Fallout wiki across FO1/FO2/FO3/NV/FO4. The pipeline is:

1. Scrape each content type into `scripts/fallout_scrape/scraped_*.json`
2. Merge with curated `scripts/rpg_data/*.json` (curated wins on name collision)
3. Regenerate binaries with `scripts/gen_rpg_packed.py`
4. Verify total size ≤ 1.4 MB; device upload is deferred until the user has the Tracker plugged in

## Current state

Already built and validated end-to-end on FO3:
- `scripts/fallout_scrape/wiki_client.py` — cached, rate-limited MediaWiki client
- `scripts/fallout_scrape/scrape_enemies.py` — full enemy scraper, works

Not yet built: `scrape_items.py`, `scrape_locations.py`, `scrape_flavor.py`, `merge.py`.

Cache at `scripts/fallout_scrape/cache/` is persistent — re-runs are free, don't delete it unless you suspect staleness.

## Order of operations

### Step 1 — Full enemy scrape (validate the existing scraper at scale)

```bash
cd /Users/goatsandmonkeys/Documents/mesh_bbs && source .venv/bin/activate
cd scripts/fallout_scrape
python scrape_enemies.py --games FO1 FO2 FO3 NV FO4 --out scraped_enemies.json
```

Cold run is long (hundreds of pages × 1 req/sec). Run it in background with `run_in_background: true` and do other prep (read `content_tables.h`, draft `scrape_items.py`) while it runs. Check back in on it rather than polling.

When it finishes:
- Skim the output JSON: faction distribution, stat ranges, descriptions.
- Spot-check 5 random entries against the wiki — names, HP, damage.
- If something looks systemically wrong (all-empty descriptions, wrong factions, junk names), fix the scraper and re-run. Cache makes re-runs fast.

### Step 2 — Build `scrape_items.py`

Model on `scrape_enemies.py`. Categories: `<Game> weapons`, `<Game> armor`, `<Game> consumables`. Infobox: `{{Infobox item}}` (fields: `weight`, `value`, `damage`, `dps`, `type`). Output schema is in HANDOFF.md — match `items.json` exactly.

Rarity: seed from wiki categories (`<Game> unique weapons` → rarity 3+). Most common items: rarity 1. Default damage for non-weapons: `"0"`.

### Step 3 — Build `scrape_locations.py`

Categories: `<Game> locations`. Infobox: `{{Infobox location}}`.

Location `type` **must** match an enum in `module-src/content_tables.h` — read that file first and build a mapping dict. Unknown wiki types → default "wasteland".

`typical_enemies`: scan the wikitext for a `==Creatures==` or `==Notable loot==` section, grab linked creature names, filter against the scraped enemy list. OK to leave empty when uncertain.

### Step 4 — Build `scrape_flavor.py`

Remember: `flavor.json` is a **dict** with four arrays (`room_descriptions`, `find_descriptions`, `ambient_descriptions`, `npc_lines`), not a list. Mine terminal entries, holotapes, notes, and location background paragraphs. Hard length cap: 100 chars per entry.

### Step 5 — Build `merge.py`

Read all `scripts/rpg_data/*.json` (curated). Read all `scripts/fallout_scrape/scraped_*.json`. For enemies/items/locations: case-insensitive dedup by `name`; **curated entries always win**. Append only new scraped entries. Strip `_wiki_title` from final output. For flavor: append scraped to each array, dedup by exact `text` match. Write back to `rpg_data/*.json`.

### Step 6 — Regen + verify

```bash
python scripts/gen_rpg_packed.py
du -ch scripts/data/*.bin | tail -1
```

Total ≤ 1.4 MB. If any individual string pool exceeds 64 KB, the generator will fail (uint16 offsets) — trim descriptions to 80 chars and dedupe before bumping the header format.

Run the sanity checks listed at the end of HANDOFF.md.

### Step 7 — Commit

Commit scraped JSONs, new scrapers, merged `rpg_data/*.json`, and regenerated `scripts/data/*.bin` in logical chunks (one commit per content type is fine, plus a final "regen bins" commit). Do NOT commit `scripts/fallout_scrape/cache/` — add it to .gitignore if it isn't already.

Do NOT upload to the device — the user does that when the Tracker L1 is plugged in.

## Hard rules

- **Don't parallelize HTTP requests** — rate limit is strict. The existing 1 req/sec throttle in `wiki_client.py` must be respected.
- **Don't use `fopen`/`fwrite` on device paths** or touch firmware code. This is pure data-pipeline work.
- **Don't commit the cache dir.**
- **Curated JSON wins on collisions** in the merge step. Never clobber a hand-written entry.
- **Page title cleanup:** strip trailing ` (Fallout X)` / ` (Mothership Zeta)` etc. via `re.sub(r"\s*\([^)]+\)\s*$", "", title)`.
- **Test at small scale first.** Any new scraper: run with `--limit 10 --games FO3` before a full 5-game run.
- **Length caps matter.** Descriptions ≤ 95 chars, flavor ≤ 100 chars — the on-device display and binary pool budgets depend on it.

## What to do when stuck

- Gotchas are listed in HANDOFF.md §Gotchas. Read those before debugging weird wiki parse output.
- If `mwparserfromhell` chokes on a template, fall back to the regex helpers in `scrape_enemies.py` (`strip_wiki`, `RE_TEMPLATE`, etc.) — don't invent a new parser.
- If the user's Tracker L1 becomes available mid-session and they ask to upload, use `scripts/upload_raw_xmodem.py` from the **mesh_bbs_meshenvy** repo (NOT the other one) — it has the `reconnect_keep_file` fix that avoids truncating the file on serial drops.

When all 6 steps are done, summarize: how many new entries per type, total bin size vs budget, and anything that looked odd and deserves a human review.
