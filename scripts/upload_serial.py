#!/usr/bin/env python3
"""
upload_serial.py — Upload files to TinyBBS external flash over USB serial.

Protocol (matches BBSSerialUpload.h):
  Frame:    0xBB <cmd:1> <len:2 LE> <data:len> <crc8:1>
  Response: 0xBB 0x80 <status:1>  (status 0=OK, 1=ERR)
  Commands: 0x01=OPEN  data=[pathLen:1][path:pathLen][fileSize:4 LE]
            0x02=DATA  data=[chunk bytes]
            0x03=CLOSE data=[]

Usage:
  python3 scripts/upload_serial.py [--port /dev/cu.usbmodem1101] [--baud 115200] FILE DEST

  FILE  — local file to upload (e.g. data/wordle.bin)
  DEST  — path on device (e.g. /bbs/kb/wordle.bin)

  If --port is omitted the script auto-detects the first matching serial port.

Examples:
  python3 scripts/upload_serial.py data/wordle.bin /bbs/kb/wordle.bin
  python3 scripts/upload_serial.py data/geo_us.bin /bbs/kb/geo_us.bin --port /dev/cu.usbserial-0001
"""

import argparse
import os
import struct
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERROR: pyserial not installed.  Run: pip install pyserial")
    sys.exit(1)

# ── Constants ────────────────────────────────────────────────────────────────

FRAME_MAGIC  = 0xBB
CMD_OPEN     = 0x01
CMD_DATA     = 0x02
CMD_CLOSE    = 0x03
RESP_STATUS  = 0x80

CHUNK_SIZE   = 256   # bytes per DATA frame (fits in firmware's 512-byte buf)
TIMEOUT_S    = 5.0   # seconds to wait for response

# ── CRC-8 (same polynomial as firmware) ─────────────────────────────────────

def crc8(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc

# ── Frame encode / decode ────────────────────────────────────────────────────

def build_frame(cmd: int, payload: bytes) -> bytes:
    hdr     = bytes([FRAME_MAGIC, cmd]) + struct.pack('<H', len(payload))
    body    = hdr + payload
    return body + bytes([crc8(body)])

def read_response(port: serial.Serial) -> bool:
    """Read 0xBB 0x80 <status> and return True on OK, False on ERR/timeout."""
    deadline = time.time() + TIMEOUT_S
    buf = bytearray()
    while time.time() < deadline:
        n = port.in_waiting
        if n:
            buf += port.read(n)
        # Scan for 0xBB 0x80
        while len(buf) >= 3:
            if buf[0] == FRAME_MAGIC and buf[1] == RESP_STATUS:
                status = buf[2]
                return status == 0
            buf.pop(0)   # skip non-matching byte
        time.sleep(0.005)
    return False  # timeout

# ── Port detection ───────────────────────────────────────────────────────────

KNOWN_DESCRIPTIONS = [
    "RAK", "rak", "nRF52", "Adafruit", "Nordic", "Meshtastic",
    "WisMesh", "T-Echo", "CP210", "CH340", "FTDI", "USB Serial",
]

def auto_detect_port() -> str | None:
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = (p.description or "") + " " + (p.manufacturer or "")
        for kw in KNOWN_DESCRIPTIONS:
            if kw.lower() in desc.lower():
                return p.device
    # Last resort: first /dev/cu.usb* on macOS
    for p in ports:
        if "usbmodem" in p.device or "usbserial" in p.device:
            return p.device
    return None

# ── Upload ───────────────────────────────────────────────────────────────────

def upload(port_name: str, baud: int, local_path: str, dest_path: str):
    if not os.path.exists(local_path):
        print(f"ERROR: File not found: {local_path}")
        sys.exit(1)

    file_size = os.path.getsize(local_path)
    print(f"  Local:  {local_path}  ({file_size:,} bytes)")
    print(f"  Device: {dest_path}")
    print(f"  Port:   {port_name}  @ {baud} baud")
    print()

    with serial.Serial(port_name, baud, timeout=TIMEOUT_S) as ser:
        time.sleep(0.2)   # let the port settle
        ser.reset_input_buffer()

        # ── OPEN ──────────────────────────────────────────────────────────
        path_bytes  = dest_path.encode('ascii')
        open_payload = bytes([len(path_bytes)]) + path_bytes + struct.pack('<I', file_size)
        ser.write(build_frame(CMD_OPEN, open_payload))
        if not read_response(ser):
            print("ERROR: Device rejected OPEN (check path / filesystem)")
            sys.exit(1)
        print(f"  OPEN accepted")

        # ── DATA (chunked) ────────────────────────────────────────────────
        sent  = 0
        with open(local_path, 'rb') as f:
            while True:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                ser.write(build_frame(CMD_DATA, chunk))
                if not read_response(ser):
                    print(f"\nERROR: Device rejected DATA at offset {sent}")
                    sys.exit(1)
                sent += len(chunk)
                pct   = 100 * sent // file_size
                bar   = '#' * (pct // 5) + '.' * (20 - pct // 5)
                print(f"\r  [{bar}] {pct:3d}%  {sent:,}/{file_size:,} B", end='', flush=True)

        print(f"\r  [{'#'*20}] 100%  {sent:,}/{file_size:,} B")

        # ── CLOSE ─────────────────────────────────────────────────────────
        ser.write(build_frame(CMD_CLOSE, b''))
        if not read_response(ser):
            print("WARNING: CLOSE response was ERR (file may still be written)")
        else:
            print(f"  CLOSE  OK")

    print()
    print(f"Upload complete: {dest_path}")

# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description="Upload a file to TinyBBS external flash over USB serial.",
        epilog=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("file",        help="Local file to upload")
    ap.add_argument("dest",        help="Destination path on device (e.g. /bbs/kb/wordle.bin)")
    ap.add_argument("--port", "-p", default=None, help="Serial port (auto-detected if omitted)")
    ap.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate (default 115200)")
    ap.add_argument("--list-ports", action="store_true", help="List available serial ports and exit")
    args = ap.parse_args()

    if args.list_ports:
        ports = list(serial.tools.list_ports.comports())
        if not ports:
            print("No serial ports found.")
        for p in ports:
            print(f"  {p.device:<25}  {p.description}")
        return

    port = args.port
    if port is None:
        port = auto_detect_port()
        if port is None:
            print("ERROR: Could not auto-detect serial port.")
            print("       Run with --list-ports to see available ports,")
            print("       then pass --port /dev/cu.usbmodemXXXX")
            sys.exit(1)
        print(f"Auto-detected port: {port}")

    upload(port, args.baud, args.file, args.dest)


if __name__ == '__main__':
    main()
