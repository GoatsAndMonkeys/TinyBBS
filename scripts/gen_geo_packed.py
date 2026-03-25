#!/usr/bin/env python3
"""
gen_geo_packed.py — Generate a single packed binary for offline reverse geocoding.

Reads cities1000.txt (GeoNames, US entries) and produces a single binary file
that can be uploaded to the device's external flash.

Binary format:
  Header (8 bytes):
    magic       uint32  0x47454F31 ("GEO1")
    cell_count  uint32  number of cells in index

  Index (12 bytes per cell, sorted by key for binary search):
    floor_lat   int16   latitude floor (e.g. 40 for 40.xxx)
    lon_offset  uint16  longitude + 180 (e.g. 107 for -73.xxx = -73+180)
    data_offset uint32  byte offset from start of file to cell data
    entry_count uint16  number of cities in this cell
    _pad        uint16  padding for alignment

  Cell data (22 bytes per entry):
    lat100      int16   latitude * 100
    lon100      int16   longitude * 100
    name        char[18] null-terminated city name (max 17 chars)

Usage:
    python3 scripts/gen_geo_packed.py [input_file] [output_file]

    Defaults:
        input_file  → scripts/cities1000.txt
        output_file → data/geo_us.bin
"""

import os
import sys
import struct
import math
from collections import defaultdict


MAX_ENTRIES_PER_CELL = 200  # keep top 200 cities per cell by population


def cell_key(lat, lon):
    return int(math.floor(lat)), int(math.floor(lon)) + 180


def pack_name(name):
    encoded = name.encode("ascii", errors="replace")[:17]
    return encoded.ljust(18, b"\x00")


def pack_entry(lat, lon, name):
    lat100 = max(-32768, min(32767, int(round(lat * 100))))
    lon100 = max(-32768, min(32767, int(round(lon * 100))))
    return struct.pack("<hh", lat100, lon100) + pack_name(name)


def load_geonames_us(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 15:
                continue
            if parts[8] != "US":
                continue
            try:
                name = parts[2] or parts[1]
                lat = float(parts[4])
                lon = float(parts[5])
                pop = int(parts[14]) if parts[14] else 0
            except (ValueError, IndexError):
                continue
            ascii_name = name.encode("ascii", errors="ignore").decode("ascii").strip()
            if not ascii_name:
                continue
            rows.append((pop, lat, lon, ascii_name))
    return rows


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))

    input_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(script_dir, "cities1000.txt")
    output_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(script_dir, "..", "data", "geo_us.bin")

    if not os.path.exists(input_path):
        print(f"ERROR: {input_path} not found")
        print("Download: curl -L -o scripts/cities1000.zip https://download.geonames.org/export/dump/cities1000.zip")
        sys.exit(1)

    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    print(f"Reading {input_path}...")
    rows = load_geonames_us(input_path)
    print(f"  {len(rows)} US cities loaded")

    # Group by cell
    cells = defaultdict(list)
    for row in rows:
        key = cell_key(row[1], row[2])
        cells[key].append(row)

    # Sort cells by key for binary search, trim per cell
    sorted_keys = sorted(cells.keys())
    cell_count = len(sorted_keys)

    print(f"  {cell_count} grid cells")

    # Calculate offsets
    HEADER_SIZE = 8
    INDEX_ENTRY_SIZE = 12
    CITY_ENTRY_SIZE = 22
    index_size = cell_count * INDEX_ENTRY_SIZE
    data_start = HEADER_SIZE + index_size

    # Build index and data
    index_buf = bytearray()
    data_buf = bytearray()
    total_cities = 0

    for floor_lat, lon_off in sorted_keys:
        entries = cells[(floor_lat, lon_off)]
        entries.sort(key=lambda e: e[0], reverse=True)  # biggest cities first
        entries = entries[:MAX_ENTRIES_PER_CELL]

        offset = data_start + len(data_buf)
        count = len(entries)

        # Index entry: lat(i16) + lon_off(u16) + offset(u32) + count(u16) + pad(u16)
        index_buf += struct.pack("<hHIHH", floor_lat, lon_off, offset, count, 0)

        # City entries
        for _, lat, lon, name in entries:
            data_buf += pack_entry(lat, lon, name)

        total_cities += count

    # Write output
    with open(output_path, "wb") as f:
        # Header
        f.write(struct.pack("<II", 0x47454F31, cell_count))
        # Index
        f.write(index_buf)
        # Data
        f.write(data_buf)

    file_size = os.path.getsize(output_path)
    print(f"\nOutput: {output_path}")
    print(f"  {total_cities} cities in {cell_count} cells")
    print(f"  {file_size} bytes ({file_size/1024:.1f} KB)")
    print(f"  Header: {HEADER_SIZE} bytes")
    print(f"  Index:  {index_size} bytes ({cell_count} cells x {INDEX_ENTRY_SIZE})")
    print(f"  Data:   {len(data_buf)} bytes ({total_cities} cities x {CITY_ENTRY_SIZE})")


if __name__ == "__main__":
    main()
