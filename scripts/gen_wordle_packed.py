#!/usr/bin/env python3
"""
gen_wordle_packed.py — Generate packed Wordle dictionary for external flash.

Extracts the full 12,972-word list from BBSWordle.h and writes a sorted
binary file for binary search on the device.

Binary format:
  Header (8 bytes):
    magic   uint32  0x57444C31 ("WDL1")
    count   uint32  number of words

  Words (5 bytes each, sorted, lowercase, no null terminator):
    "aahed"
    "aalii"
    ...

Usage:
    python3 scripts/gen_wordle_packed.py
"""

import os
import re
import struct
import sys

MAGIC = 0x57444C31  # "WDL1"


def extract_words(header_path):
    with open(header_path) as f:
        content = f.read()

    # Find the #else section (full word list for ESP32)
    else_pos = content.find('#else')
    if else_pos == -1:
        print("ERROR: No #else section found in BBSWordle.h")
        sys.exit(1)

    # Find the end marker
    end_pos = content.find('static const uint32_t WORDLE_WORD_COUNT = 12972;', else_pos)
    if end_pos == -1:
        print("ERROR: Could not find WORDLE_WORD_COUNT = 12972")
        sys.exit(1)

    section = content[else_pos:end_pos]
    words = re.findall(r'"([a-z]{5})"', section)
    return sorted(set(words))


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    header_path = os.path.join(script_dir, "..", "module-src", "BBSWordle.h")
    output_path = os.path.join(script_dir, "..", "data", "wordle.bin")

    if not os.path.exists(header_path):
        print(f"ERROR: {header_path} not found")
        sys.exit(1)

    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    print(f"Extracting words from {header_path}...")
    words = extract_words(header_path)
    print(f"  {len(words)} unique words extracted")

    # Validate
    for w in words:
        assert len(w) == 5 and w.isalpha() and w.islower(), f"Bad word: {w}"

    # Write binary
    with open(output_path, "wb") as f:
        f.write(struct.pack("<II", MAGIC, len(words)))
        for w in words:
            f.write(w.encode("ascii"))

    size = os.path.getsize(output_path)
    print(f"\nOutput: {output_path}")
    print(f"  {len(words)} words, {size} bytes ({size/1024:.1f} KB)")
    print(f"  Header: 8 bytes")
    print(f"  Words:  {len(words) * 5} bytes")


if __name__ == "__main__":
    main()
