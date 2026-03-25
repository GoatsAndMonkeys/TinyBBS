#!/usr/bin/env python3
"""
upload_geo.py — Upload geo_us.bin to the BBS node's external flash over serial.

The BBS firmware listens for a special upload protocol on the serial port:
  1. Host sends: "!GEOUPLOAD <size>\n"
  2. Device responds: "READY\n"
  3. Host sends raw binary data in 128-byte chunks
  4. After each chunk, device responds: "OK\n" or "ERR\n"
  5. Host sends: "!GEODONE\n"
  6. Device responds: "DONE <bytes>\n"

Usage:
    python3 scripts/upload_geo.py [port] [bin_file]

    Defaults:
        port     → auto-detect /dev/cu.usbmodem*
        bin_file → data/geo_us.bin
"""

import os
import sys
import glob
import time

def find_port():
    """Auto-detect serial port."""
    patterns = ["/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"]
    for pat in patterns:
        ports = glob.glob(pat)
        if ports:
            return sorted(ports)[0]
    return None

def main():
    try:
        import serial
    except ImportError:
        print("ERROR: pyserial not installed. Run: pip install pyserial")
        sys.exit(1)

    script_dir = os.path.dirname(os.path.abspath(__file__))

    # Parse args
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    bin_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(script_dir, "..", "data", "geo_us.bin")

    if not port:
        print("ERROR: No serial port found. Specify manually: python3 upload_geo.py /dev/cu.usbmodemXXXX")
        sys.exit(1)

    if not os.path.exists(bin_path):
        print(f"ERROR: {bin_path} not found. Run: python3 scripts/gen_geo_packed.py")
        sys.exit(1)

    file_size = os.path.getsize(bin_path)
    print(f"Uploading {bin_path} ({file_size} bytes, {file_size/1024:.1f} KB)")
    print(f"Port: {port}")

    with open(bin_path, "rb") as bf:
        data = bf.read()

    ser = serial.Serial(port, 115200, timeout=5)
    time.sleep(2)  # wait for device to be ready

    # Flush any pending log output
    ser.reset_input_buffer()
    time.sleep(0.5)
    ser.reset_input_buffer()

    # Send upload command (may need to retry — runOnce runs every 60s on nRF52,
    # but we also check more frequently via the Serial.available() peek)
    cmd = f"!GEOUPLOAD {file_size}\n"
    print(f"Sending: {cmd.strip()}")

    # Try up to 3 times, flushing log lines between attempts
    ready = False
    for attempt in range(3):
        ser.write(cmd.encode())
        # Read lines for up to 10 seconds, looking for READY
        deadline = time.time() + 10
        while time.time() < deadline:
            resp = ser.readline().decode("utf-8", errors="replace").strip()
            if resp == "READY":
                ready = True
                break
            elif resp:
                # Skip Meshtastic log lines
                if attempt == 0:
                    print(f"  (skipping log: {resp[:60]})")
        if ready:
            break
        print(f"  Retry {attempt + 2}/3...")
        time.sleep(2)

    if not ready:
        print("ERROR: Device did not respond with 'READY' after 3 attempts.")
        print("Make sure the BBS firmware with geo upload support is running.")
        ser.close()
        sys.exit(1)

    print("Device ready. Uploading...")

    CHUNK_SIZE = 128
    sent = 0
    errors = 0

    while sent < file_size:
        chunk = data[sent:sent + CHUNK_SIZE]
        ser.write(chunk)

        # Read response, skipping any interleaved log lines
        ok = False
        for _ in range(10):
            resp = ser.readline().decode("utf-8", errors="replace").strip()
            if resp == "OK":
                ok = True
                break
            elif resp.startswith("ERR"):
                break
            # else: skip log line
        if ok:
            sent += len(chunk)
            pct = sent * 100 // file_size
            print(f"\r  {sent}/{file_size} bytes ({pct}%)", end="", flush=True)
        else:
            errors += 1
            print(f"\n  ERROR at offset {sent}: '{resp}'")
            if errors > 5:
                print("Too many errors, aborting.")
                break

    print()

    # Send done
    ser.write(b"!GEODONE\n")
    resp = ser.readline().decode("utf-8", errors="replace").strip()
    print(f"Device: {resp}")

    ser.close()

    if errors == 0:
        print(f"\nUpload complete! {file_size} bytes written to external flash.")
        print("The BBS will now use offline reverse geocoding for QSL posts.")
    else:
        print(f"\nUpload finished with {errors} errors.")


if __name__ == "__main__":
    main()
