#!/usr/bin/env python3
"""
gen_geo.py — Generate 1°×1° binary grid cell files for city geo-lookup.

Input (auto-detected):
  A) GeoNames cities1000.txt  (tab-separated, worldwide cities pop>1000)
     Download: https://download.geonames.org/export/dump/cities1000.zip
     US entries are filtered automatically (country code == "US").
  B) SimpleMaps "Basic" US Cities CSV (uscities.csv)
     Download: https://simplemaps.com/data/us-cities
     Required columns: city (or city_ascii), lat, lng

Output: data/geo/c_<lat>_<lon_off>.bin  (one file per 1°×1° cell)
        Binary format per file:
          uint16_t count            (2 bytes, little-endian)
          entry[count]:
            int16_t lat100          (lat × 100, little-endian)
            int16_t lon100          (lon × 100, little-endian)
            char    name[18]        (null-terminated, truncated to 17 chars)
          = 22 bytes per entry

Usage:
    python3 scripts/gen_geo.py [input_file] [output_dir]

    Defaults:
        input_file  → scripts/cities1000.txt  (then scripts/uscities.csv)
        output_dir  → data/geo/

After running, upload data/geo/ to the device via:
    pio run -e t-echo -t uploadfs
"""

import csv
import os
import sys
import struct
import math
from collections import defaultdict

# ── Configuration ─────────────────────────────────────────────────────────────
# Limit entries per cell to avoid very large files in NYC/LA dense areas.
# 300 entries × 22 bytes = 6,600 bytes ≈ 6.5KB max file size.
MAX_ENTRIES_PER_CELL = 300

# Only include cities with population above this threshold when a cell exceeds
# MAX_ENTRIES_PER_CELL. Set to 0 to include everything (smaller cities first trimmed).
POP_THRESHOLD_FOR_TRIM = 0


def cell_key(lat: float, lon: float) -> tuple[int, int]:
    """Return (floor_lat, floor_lon + 180) for the cell containing (lat, lon)."""
    return int(math.floor(lat)), int(math.floor(lon)) + 180


def pack_name(name: str) -> bytes:
    """Encode city name to 18 bytes, null-padded, truncated to 17 printable chars."""
    encoded = name.encode("ascii", errors="replace")[:17]
    return encoded.ljust(18, b"\x00")


def pack_entry(lat: float, lon: float, name: str) -> bytes:
    """Pack one 22-byte record."""
    lat100 = int(round(lat * 100))
    lon100 = int(round(lon * 100))
    # Clamp to int16 range (should never be needed for valid US coords)
    lat100 = max(-32768, min(32767, lat100))
    lon100 = max(-32768, min(32767, lon100))
    return struct.pack("<hh", lat100, lon100) + pack_name(name)


def load_geonames(path):
    """Load GeoNames tab-separated file, return list of (pop, lat, lon, name) for US entries."""
    rows = []
    skipped = 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            if len(parts) < 15:
                skipped += 1
                continue
            try:
                country = parts[8]
                if country != "US":
                    continue
                name    = parts[2] or parts[1]  # asciiname, fallback to name
                lat     = float(parts[4])
                lon     = float(parts[5])
                pop     = int(parts[14]) if parts[14] else 0
            except (ValueError, IndexError):
                skipped += 1
                continue
            ascii_name = name.encode("ascii", errors="ignore").decode("ascii").strip()
            if not ascii_name:
                skipped += 1
                continue
            rows.append((pop, lat, lon, ascii_name))
    return rows, skipped


def load_simplemaps(path):
    """Load SimpleMaps uscities.csv, return list of (pop, lat, lon, name)."""
    rows = []
    skipped = 0
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames or []
        city_col = "city_ascii" if "city_ascii" in fieldnames else "city"
        for row in reader:
            try:
                name = row[city_col].strip()
                lat  = float(row["lat"])
                lon  = float(row["lng"])
                pop  = int(row.get("population", 0) or 0)
            except (KeyError, ValueError):
                skipped += 1
                continue
            if not name:
                skipped += 1
                continue
            ascii_name = name.encode("ascii", errors="ignore").decode("ascii").strip()
            if not ascii_name:
                skipped += 1
                continue
            rows.append((pop, lat, lon, ascii_name))
    return rows, skipped


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))

    # Find input file
    if len(sys.argv) > 1:
        input_path = sys.argv[1]
    else:
        # Auto-detect: prefer GeoNames, fall back to SimpleMaps
        geonames_default = os.path.join(script_dir, "cities1000.txt")
        simplemaps_default = os.path.join(script_dir, "uscities.csv")
        if os.path.exists(geonames_default):
            input_path = geonames_default
        elif os.path.exists(simplemaps_default):
            input_path = simplemaps_default
        else:
            print("ERROR: No input file found.")
            print()
            print("Option A — GeoNames (recommended, free, no sign-up):")
            print("  curl -L -o scripts/cities1000.zip https://download.geonames.org/export/dump/cities1000.zip")
            print("  cd scripts && unzip cities1000.zip && cd ..")
            print()
            print("Option B — SimpleMaps (US only):")
            print("  https://simplemaps.com/data/us-cities  → save as scripts/uscities.csv")
            sys.exit(1)

    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(script_dir, "..", "data", "geo")

    if not os.path.exists(input_path):
        print(f"ERROR: Input file not found: {input_path}")
        sys.exit(1)

    os.makedirs(out_dir, exist_ok=True)

    # Auto-detect format by extension / content
    basename = os.path.basename(input_path).lower()
    is_geonames = basename.endswith(".txt") or (not basename.endswith(".csv"))

    print(f"Reading {input_path} ({'GeoNames' if is_geonames else 'SimpleMaps'} format) ...")
    if is_geonames:
        all_rows, skipped = load_geonames(input_path)
    else:
        all_rows, skipped = load_simplemaps(input_path)

    # cells[key] = list of (pop, lat, lon, name)
    cells: dict[tuple[int, int], list] = defaultdict(list)
    for row in all_rows:
        key = cell_key(row[1], row[2])
        cells[key].append(row)

    print(f"  Loaded {len(all_rows)} cities ({skipped} skipped) into {len(cells)} cells")

    # Write binary files
    files_written = 0
    total_entries = 0
    trimmed_cells = 0

    for (floor_lat, lon_off), entries in sorted(cells.items()):
        # Sort by population descending so we keep the largest cities if we trim
        entries.sort(key=lambda e: e[0], reverse=True)

        if len(entries) > MAX_ENTRIES_PER_CELL:
            trimmed_cells += 1
            entries = entries[:MAX_ENTRIES_PER_CELL]

        count = len(entries)
        filename = f"c_{floor_lat}_{lon_off}.bin"
        filepath = os.path.join(out_dir, filename)

        with open(filepath, "wb") as f:
            f.write(struct.pack("<H", count))
            for _, lat, lon, name in entries:
                f.write(pack_entry(lat, lon, name))

        total_entries += count
        files_written += 1

    print(f"  Written {files_written} cell files, {total_entries} total entries")
    if trimmed_cells:
        print(f"  Trimmed {trimmed_cells} dense cells to {MAX_ENTRIES_PER_CELL} entries (kept largest cities)")

    # Summary stats
    all_sizes = [os.path.getsize(os.path.join(out_dir, f)) for f in os.listdir(out_dir) if f.endswith(".bin")]
    if all_sizes:
        total_kb = sum(all_sizes) / 1024
        avg_kb   = total_kb / len(all_sizes)
        max_kb   = max(all_sizes) / 1024
        print()
        print(f"Storage summary:")
        print(f"  Total:   {total_kb:.1f} KB  ({total_kb/1024:.2f} MB)")
        print(f"  Average: {avg_kb:.1f} KB per cell")
        print(f"  Largest: {max_kb:.1f} KB")
        print(f"  Files:   {files_written}")
        print()
        print(f"Output directory: {os.path.abspath(out_dir)}")
        print()
        print("Next steps:")
        print("  1. Run: pio run -e t-echo -t buildfs")
        print("     (or configure data_dir in platformio.ini to point to data/)")
        print("  2. Flash filesystem: pio run -e t-echo -t uploadfs")


if __name__ == "__main__":
    main()
