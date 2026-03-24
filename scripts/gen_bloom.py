#!/usr/bin/env python3
"""
Generate a Bloom filter from the full Wordle word list for nRF52 word validation.
Outputs a C header snippet with the bloom filter bitset.

Usage: python3 scripts/gen_bloom.py > /tmp/bloom_output.txt
       Then paste the WORDLE_BLOOM array into BBSWordle.h

Filter params:
  Words: ~12,972
  Size: 12,288 bytes = 98,304 bits
  Hash functions: 5
  Expected false positive rate: ~2.7%
"""

import re
import sys
import struct

BLOOM_BYTES = 12288   # 12KB
BLOOM_BITS  = BLOOM_BYTES * 8
K_HASHES    = 5       # number of hash functions

def fnv1a32(data: bytes, seed: int = 0) -> int:
    """FNV-1a 32-bit hash."""
    FNV_PRIME = 0x01000193
    h = (0x811c9dc5 ^ seed) & 0xFFFFFFFF
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFF
    return h

def get_bit_positions(word: str) -> list[int]:
    """Return K bit positions for a word."""
    w = word.lower().encode('ascii')
    positions = []
    for i in range(K_HASHES):
        h = fnv1a32(w, seed=i * 0x9e3779b9 & 0xFFFFFFFF)
        positions.append(h % BLOOM_BITS)
    return positions

def extract_words(header_path: str) -> list[str]:
    """Extract words from BBSWordle.h full list (between #else and WORDLE_WORD_COUNT = 12972)."""
    with open(header_path) as f:
        content = f.read()

    # Find the #else section (full word list)
    else_pos = content.find('#else\n// Full Wordle dictionary')
    if else_pos == -1:
        # Try alternate form
        else_pos = content.find('#else')

    end_pos = content.find('static const uint32_t WORDLE_WORD_COUNT = 12972;', else_pos)
    if end_pos == -1:
        print("ERROR: Could not find full word list section", file=sys.stderr)
        sys.exit(1)

    section = content[else_pos:end_pos]
    words = re.findall(r'"([a-z]{5})"', section)
    return words

def build_bloom(words: list[str]) -> bytearray:
    """Build the Bloom filter bitset."""
    bits = bytearray(BLOOM_BYTES)
    for word in words:
        for pos in get_bit_positions(word):
            bits[pos // 8] |= (1 << (pos % 8))
    return bits

def format_c_array(bits: bytearray, name: str = "WORDLE_BLOOM") -> str:
    """Format bytearray as a C uint8_t array."""
    lines = [f"static const uint8_t {name}[{len(bits)}] = {{"]
    row = []
    for i, b in enumerate(bits):
        row.append(f"0x{b:02x}")
        if len(row) == 16:
            lines.append("    " + ",".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ",".join(row))
    lines.append("};")
    return "\n".join(lines)

def main():
    import os
    script_dir = os.path.dirname(os.path.abspath(__file__))
    header = os.path.join(script_dir, "../module-src/BBSWordle.h")

    print(f"// Extracting words from {header}...", file=sys.stderr)
    words = extract_words(header)
    print(f"// Found {len(words)} words", file=sys.stderr)

    bits = build_bloom(words)

    set_bits = sum(bin(b).count('1') for b in bits)
    fill = set_bits / BLOOM_BITS
    # Theoretical FPR
    import math
    fpr = (1 - math.exp(-K_HASHES * len(words) / BLOOM_BITS)) ** K_HASHES

    print(f"// Bloom filter stats:", file=sys.stderr)
    print(f"//   Words: {len(words)}", file=sys.stderr)
    print(f"//   Filter size: {BLOOM_BYTES} bytes ({BLOOM_BITS} bits)", file=sys.stderr)
    print(f"//   Hash functions: {K_HASHES}", file=sys.stderr)
    print(f"//   Bits set: {set_bits}/{BLOOM_BITS} ({fill*100:.1f}%)", file=sys.stderr)
    print(f"//   Est. false positive rate: {fpr*100:.2f}%", file=sys.stderr)

    print(format_c_array(bits))

if __name__ == "__main__":
    main()
