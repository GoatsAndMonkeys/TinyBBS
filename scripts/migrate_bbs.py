#!/usr/bin/env python3
"""
BBS Migration Tool: Heltec V3 → T-Echo (MicroBBS)

Reads the LittleFS partition from the Heltec V3 over USB serial,
extracts BBS files, and creates a T-Echo-compatible UF2.

Usage:
  Step 1 — Dump Heltec flash:
    python3 scripts/migrate_bbs.py dump --port /dev/cu.usbserial-0001

  Step 2 — Inspect what was extracted:
    python3 scripts/migrate_bbs.py inspect

  Step 3 — Build T-Echo UF2 with BBS data:
    python3 scripts/migrate_bbs.py build-uf2

  Then flash to T-Echo (double-press reset → drag UF2 to TECHOBOOT drive):
    1. Drag firmware.uf2   (Meshtastic firmware — already done)
    2. Drag bbs_data.uf2   (this script's output)
"""

import argparse
import os
import struct
import subprocess
import sys

# ─── Partition layout ────────────────────────────────────────────────────────
# Heltec V3 (ESP32-S3): LittleFS at 0x670000, size 0x180000 (1.5 MB)
HELTEC_LFS_OFFSET = 0x670000
HELTEC_LFS_SIZE   = 0x180000

# T-Echo (nRF52840): InternalFS LittleFS at 0xED000, size 7*4096 = 28672 bytes
TECHO_LFS_ADDR  = 0xED000
TECHO_LFS_SIZE  = 7 * 4096          # 28,672 bytes
TECHO_BLOCK_SZ  = 4096              # nRF52840 flash erase block size
TECHO_BLOCK_CNT = TECHO_LFS_SIZE // TECHO_BLOCK_SZ  # = 7

# BBS paths to migrate
BBS_PATHS = ["/bbs/meta.bin", "/bbs/bul", "/bbs/mail", "/bbs/qsl"]
WORDLE_PATH = "/bbs/wdl"

DUMP_FILE   = "heltec_littlefs.bin"
EXTRACT_DIR = "bbs_extracted"
UF2_OUT     = "bbs_data.uf2"

# UF2 magic numbers
UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC2 = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
NRF52840_FAMILY = 0xADA52840


def run(cmd, **kwargs):
    print(f"  $ {' '.join(cmd)}")
    return subprocess.run(cmd, check=True, **kwargs)


# ─── Step 1: Dump Heltec LittleFS ────────────────────────────────────────────

def cmd_dump(port: str):
    print(f"\n=== Dumping Heltec V3 LittleFS (port={port}) ===")
    print(f"    Offset: 0x{HELTEC_LFS_OFFSET:06x}  Size: {HELTEC_LFS_SIZE//1024}KB")
    print("    This takes ~15-30 seconds...")

    esptool = ["python3", "-m", "esptool",
               "--chip", "esp32s3",
               "--port", port,
               "--baud", "921600",
               "read_flash",
               hex(HELTEC_LFS_OFFSET),
               hex(HELTEC_LFS_SIZE),
               DUMP_FILE]
    try:
        run(esptool)
        print(f"\n✓ Saved {HELTEC_LFS_SIZE//1024}KB to {DUMP_FILE}")
    except subprocess.CalledProcessError:
        print("\nERROR: esptool failed. Make sure:")
        print("  - Heltec V3 is connected via USB")
        print("  - Port is correct (e.g. /dev/cu.usbserial-0001)")
        print("  - Heltec is not in bootloader mode (just power on normally)")
        sys.exit(1)

    # Now extract
    cmd_extract()


def cmd_extract():
    """Extract files from the LittleFS dump."""
    try:
        import littlefs
    except ImportError:
        print("ERROR: littlefs-python not installed. Run: pip install littlefs-python")
        sys.exit(1)

    if not os.path.exists(DUMP_FILE):
        print(f"ERROR: {DUMP_FILE} not found. Run 'dump' step first.")
        sys.exit(1)

    print(f"\n=== Extracting files from {DUMP_FILE} ===")
    with open(DUMP_FILE, "rb") as f:
        data = f.read()

    # Heltec ESP32 LittleFS params (from Arduino LittleFS defaults)
    try:
        ctx = littlefs.UserContext(buffer=bytearray(data))
        fs = littlefs.LittleFS(context=ctx, block_size=4096, block_count=HELTEC_LFS_SIZE // 4096, mount=True)
    except Exception as e:
        print(f"ERROR mounting LittleFS: {e}")
        print("The dump may be corrupt or the Heltec BBS may have never been booted.")
        sys.exit(1)

    os.makedirs(EXTRACT_DIR, exist_ok=True)
    total_bytes = 0
    file_count = 0

    def extract_dir(path):
        nonlocal total_bytes, file_count
        try:
            entries = fs.listdir(path)
        except Exception:
            return
        for name in entries:
            fpath = f"{path}/{name}" if path != "/" else f"/{name}"
            try:
                stat = fs.stat(fpath)
                if stat.type == 2:  # directory
                    extract_dir(fpath)
                else:
                    local_path = EXTRACT_DIR + fpath
                    os.makedirs(os.path.dirname(local_path), exist_ok=True)
                    with fs.open(fpath, "rb") as src, open(local_path, "wb") as dst:
                        content = src.read()
                        dst.write(content)
                        total_bytes += len(content)
                        file_count += 1
                        print(f"  {fpath}  ({len(content)} bytes)")
            except Exception as e:
                print(f"  SKIP {fpath}: {e}")

    extract_dir("/bbs")
    print(f"\n✓ Extracted {file_count} files, {total_bytes} bytes total → {EXTRACT_DIR}/")
    return file_count, total_bytes


# ─── Step 2: Inspect extracted data ──────────────────────────────────────────

def cmd_inspect():
    print(f"\n=== BBS Data Inspection ===")
    if not os.path.exists(EXTRACT_DIR):
        print(f"ERROR: {EXTRACT_DIR} not found. Run 'dump' step first.")
        sys.exit(1)

    total = 0
    files = []
    for root, dirs, filenames in os.walk(EXTRACT_DIR):
        for fname in filenames:
            fpath = os.path.join(root, fname)
            size = os.path.getsize(fpath)
            rel = fpath[len(EXTRACT_DIR):]
            files.append((rel, size))
            total += size

    files.sort()
    print(f"  {'Path':<40} {'Bytes':>8}")
    print(f"  {'-'*40} {'-'*8}")
    for path, size in files:
        print(f"  {path:<40} {size:>8}")
    print(f"  {'-'*40} {'-'*8}")
    print(f"  {'TOTAL':<40} {total:>8}")

    # T-Echo storage budget
    print(f"\n  T-Echo storage budget:")
    print(f"    LittleFS total:    {TECHO_LFS_SIZE:>8} bytes")
    print(f"    Meshtastic config: {'~6144':>8} bytes (estimated)")
    avail = TECHO_LFS_SIZE - 6144
    print(f"    Available for BBS: {avail:>8} bytes")
    if total <= avail:
        print(f"    BBS data total:    {total:>8} bytes  ✓ FITS")
    else:
        print(f"    BBS data total:    {total:>8} bytes  ✗ OVER by {total-avail}")
        print(f"    → Oldest bulletins/QSL will be trimmed to fit")


# ─── Step 3: Build T-Echo UF2 ────────────────────────────────────────────────

def cmd_build_uf2():
    """
    Build a UF2 file that writes a fresh nRF52840-compatible LittleFS image
    containing the extracted BBS files, starting at 0xED000.

    Flash order for T-Echo:
      1. firmware.uf2   (Meshtastic + MicroBBS code)
      2. bbs_data.uf2   (this file - BBS content)

    On first boot after step 2, Meshtastic formats the FS only if begin() fails.
    Since our BBS data FS is valid LittleFS, Meshtastic will mount it and add
    its own config files (/prefs/, /channels.proto, etc.) alongside /bbs/.
    """
    try:
        import littlefs
    except ImportError:
        print("ERROR: littlefs-python not installed. Run: pip install littlefs-python")
        sys.exit(1)

    if not os.path.exists(EXTRACT_DIR):
        print(f"ERROR: {EXTRACT_DIR} not found. Run 'dump' step first.")
        sys.exit(1)

    print(f"\n=== Building T-Echo LittleFS image ===")
    print(f"    Block size:  {TECHO_BLOCK_SZ} bytes")
    print(f"    Block count: {TECHO_BLOCK_CNT}")
    print(f"    Total size:  {TECHO_LFS_SIZE} bytes")

    # Create LittleFS with nRF52840 InternalFileSystem parameters
    ctx = littlefs.UserContext(buffsize=TECHO_LFS_SIZE)
    fs = littlefs.LittleFS(
        context=ctx,
        block_size=TECHO_BLOCK_SZ,
        block_count=TECHO_BLOCK_CNT,
        read_size=256,
        prog_size=256,
        cache_size=512,
        lookahead_size=16,
        mount=False,
    )
    fs.format()
    fs.mount()

    # Walk extracted BBS files and add to new FS
    added = 0
    skipped = 0
    total_bytes = 0

    # Sort files: meta first, then bulletins, qsl, mail, wordle
    all_files = []
    for root, dirs, filenames in os.walk(EXTRACT_DIR):
        for fname in sorted(filenames):
            fpath = os.path.join(root, fname)
            rel = fpath[len(EXTRACT_DIR):]
            all_files.append((rel, fpath))
    all_files.sort(key=lambda x: (0 if 'meta' in x[0] else 1, x[0]))

    for rel_path, local_path in all_files:
        size = os.path.getsize(local_path)
        if total_bytes + size > TECHO_LFS_SIZE - 8192:  # keep 8KB headroom for Meshtastic
            print(f"  SKIP {rel_path} ({size}B) — would exceed budget")
            skipped += 1
            continue

        # Ensure directory exists
        parts = rel_path.rsplit("/", 1)
        if len(parts) > 1 and parts[0]:
            try:
                fs.makedirs(parts[0])
            except Exception:
                pass

        try:
            with open(local_path, "rb") as src:
                content = src.read()
            with fs.open(rel_path, "wb") as dst:
                dst.write(content)
            print(f"  + {rel_path}  ({size}B)")
            added += 1
            total_bytes += size
        except Exception as e:
            print(f"  ERROR writing {rel_path}: {e}")
            skipped += 1

    # Export filesystem image
    img = bytes(ctx.buffer)
    fs.unmount()

    print(f"\n  Added: {added} files ({total_bytes} bytes)")
    if skipped:
        print(f"  Skipped: {skipped} files (over budget)")

    # Build UF2
    print(f"\n=== Generating {UF2_OUT} ===")
    uf2_data = bytearray()
    block_num = 0
    # Write 256 bytes per UF2 block (standard page payload)
    PAGE = 256
    addr = TECHO_LFS_ADDR
    total_blocks = (len(img) + PAGE - 1) // PAGE

    for offset in range(0, len(img), PAGE):
        chunk = img[offset:offset + PAGE]
        chunk = chunk.ljust(PAGE, b'\xff')  # pad to PAGE bytes

        block = struct.pack("<IIIIIIII",
            UF2_MAGIC0, UF2_MAGIC1,
            UF2_FLAG_FAMILY,
            addr + offset,
            PAGE,
            block_num,
            total_blocks,
            NRF52840_FAMILY,
        )  # 32 bytes header
        block += chunk                      # 256 bytes payload
        block += b'\x00' * (476 - PAGE)    # 220 bytes padding
        block += struct.pack("<I", UF2_MAGIC2)  # 4 bytes
        assert len(block) == 512
        uf2_data += block
        block_num += 1

    with open(UF2_OUT, "wb") as f:
        f.write(uf2_data)

    print(f"✓ Wrote {len(uf2_data)//512} UF2 blocks → {UF2_OUT}")
    print(f"\nFlash to T-Echo:")
    print(f"  1. Double-press T-Echo reset button → TECHOBOOT drive appears")
    print(f"  2. Drag firmware.uf2 first (if not already flashed)")
    print(f"  3. Drag {UF2_OUT} → T-Echo reboots with BBS data pre-loaded")
    print(f"  4. Configure Meshtastic via app — BBS data survives!")


# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="BBS Migration Tool: Heltec V3 → T-Echo")
    sub = ap.add_subparsers(dest="cmd")

    p_dump = sub.add_parser("dump", help="Dump Heltec V3 LittleFS and extract files")
    p_dump.add_argument("--port", required=True, help="Serial port, e.g. /dev/cu.usbserial-0001")

    sub.add_parser("inspect", help="Inspect extracted BBS files and check T-Echo budget")
    sub.add_parser("build-uf2", help="Build T-Echo UF2 from extracted files")

    args = ap.parse_args()
    if not args.cmd:
        ap.print_help()
        sys.exit(0)

    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    if args.cmd == "dump":
        cmd_dump(args.port)
        cmd_inspect()
    elif args.cmd == "inspect":
        cmd_inspect()
    elif args.cmd == "build-uf2":
        cmd_build_uf2()


if __name__ == "__main__":
    main()
