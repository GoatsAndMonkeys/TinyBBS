#!/usr/bin/env python3
"""
ble_upload.py — Upload files to TinyBBS external flash via Meshtastic BLE.

Uses the meshtastic Python library to send file data as private messages
to the BBS node. The BBS intercepts messages starting with "!KB" as
knowledge base upload commands.

Protocol:
  DM "!KB OPEN /bbs/kb/wordle.bin 64868"  → device opens file
  DM "!KB DATA <base64_chunk>"             → device writes chunk
  DM "!KB CLOSE"                           → device closes file
  DM "!KB LIST"                            → device lists /bbs/kb/

Usage:
    python3 scripts/ble_upload.py upload data/wordle.bin /bbs/kb/wordle.bin
    python3 scripts/ble_upload.py list
"""

import os
import sys
import time
import base64

def main():
    try:
        import meshtastic
        import meshtastic.ble_interface
    except ImportError:
        print("ERROR: meshtastic not installed. Run: pip install meshtastic")
        sys.exit(1)

    if len(sys.argv) < 2:
        print("Usage:")
        print("  ble_upload.py upload <local_file> <device_path>")
        print("  ble_upload.py list")
        sys.exit(0)

    cmd = sys.argv[1].lower()

    if cmd == "upload":
        if len(sys.argv) < 4:
            print("Usage: ble_upload.py upload <local_file> <device_path>")
            sys.exit(1)
        do_upload(sys.argv[2], sys.argv[3])
    elif cmd == "list":
        do_list()
    else:
        print(f"Unknown command: {cmd}")


def connect_ble():
    """Connect to the Meshtastic device via BLE."""
    import meshtastic.ble_interface
    print("Connecting via BLE...")
    try:
        iface = meshtastic.ble_interface.BLEInterface(None)  # auto-scan
        print(f"Connected!")
        return iface
    except Exception as e:
        print(f"ERROR: {e}")
        sys.exit(1)


def send_dm(iface, text):
    """Send a DM to ourselves (the local node)."""
    my_node = iface.myInfo.my_node_num
    iface.sendText(text, destinationId=my_node, wantAck=False)


def do_upload(local_path, device_path):
    if not os.path.exists(local_path):
        print(f"ERROR: {local_path} not found")
        sys.exit(1)

    with open(local_path, "rb") as f:
        data = f.read()

    file_size = len(data)
    print(f"File: {local_path} ({file_size} bytes, {file_size/1024:.1f} KB)")
    print(f"Dest: {device_path}")

    iface = connect_ble()
    time.sleep(1)

    # OPEN
    print("Opening file on device...")
    send_dm(iface, f"!KB OPEN {device_path} {file_size}")
    time.sleep(0.5)

    # DATA — send in base64 chunks that fit in 200-byte messages
    # Base64 expands by 4/3, plus "!KB DATA " prefix = 9 bytes
    # Max payload: 200 - 9 = 191 bytes of base64 = 143 raw bytes per chunk
    CHUNK_RAW = 120  # conservative: 120 raw bytes → 160 base64 chars → 169 total
    sent = 0
    start_time = time.time()

    print("Uploading...")
    while sent < file_size:
        chunk = data[sent:sent + CHUNK_RAW]
        b64 = base64.b64encode(chunk).decode("ascii")
        send_dm(iface, f"!KB DATA {b64}")
        sent += len(chunk)

        pct = sent * 100 // file_size
        elapsed = time.time() - start_time
        rate = sent / elapsed if elapsed > 0 else 0
        print(f"\r  {sent}/{file_size} ({pct}%) {rate/1024:.1f} KB/s", end="", flush=True)

        # Pace to avoid overwhelming the BLE/mesh stack
        time.sleep(0.1)

    print()

    # CLOSE
    print("Closing file...")
    send_dm(iface, "!KB CLOSE")
    time.sleep(0.5)

    elapsed = time.time() - start_time
    print(f"Done! {sent} bytes in {elapsed:.1f}s")

    iface.close()


def do_list():
    iface = connect_ble()
    time.sleep(1)
    send_dm(iface, "!KB LIST")
    time.sleep(2)
    iface.close()
    print("Check your Meshtastic app for the response.")


if __name__ == "__main__":
    main()
