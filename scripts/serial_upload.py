#!/usr/bin/env python3
"""
serial_upload.py — Upload files to TinyBBS external flash via raw serial.

Uses 0xBB-framed packets that Meshtastic's serial parser ignores
(it only looks for 0x94C3). No protobuf, no meshtastic library needed.

Frame: 0xBB <cmd:1> <len:2 LE> <data:len> <crc8:1>
Response: 0xBB 0x80 <status:1>  (0=OK, 1=ERR)

Usage:
    python3 scripts/serial_upload.py upload data/wordle.bin /bbs/kb/wordle.bin [port]
    python3 scripts/serial_upload.py upload data/geo_us.bin /bbs/kb/geo_us.bin [port]
"""

import os
import sys
import time
import struct
import glob

CHUNK_SIZE = 128  # smaller chunks for reliability


def find_port():
    for pat in ["/dev/cu.usbmodem*", "/dev/ttyACM*"]:
        ports = glob.glob(pat)
        if ports:
            return sorted(ports)[0]
    return None


def crc8(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def send_frame(ser, cmd, payload=b""):
    frame = bytes([0xBB, cmd]) + struct.pack("<H", len(payload)) + payload
    frame += bytes([crc8(frame)])
    ser.write(frame)
    ser.flush()


def read_response(ser, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if ser.in_waiting >= 3:
            b = ser.read(1)
            if b and b[0] == 0xBB:
                rest = ser.read(2)
                if len(rest) == 2:
                    return rest[0], rest[1]  # cmd, status
        # Drain any non-0xBB bytes (meshtastic log output)
        elif ser.in_waiting > 0:
            ser.read(ser.in_waiting)
        time.sleep(0.005)
    return None, None


def main():
    try:
        import serial
    except ImportError:
        print("ERROR: pyserial not installed. Run: pip install pyserial")
        sys.exit(1)

    if len(sys.argv) < 4 or sys.argv[1] != "upload":
        print("Usage: serial_upload.py upload <local_file> <device_path> [port]")
        sys.exit(0)

    local_path = sys.argv[2]
    device_path = sys.argv[3]
    port = sys.argv[4] if len(sys.argv) > 4 else find_port()

    if not port:
        print("ERROR: No serial port found. Specify: serial_upload.py upload file path /dev/cu.usbmodemXXXX")
        sys.exit(1)

    if not os.path.exists(local_path):
        print(f"ERROR: {local_path} not found")
        sys.exit(1)

    with open(local_path, "rb") as f:
        data = f.read()

    file_size = len(data)
    print(f"File: {local_path} ({file_size} bytes, {file_size/1024:.1f} KB)")
    print(f"Dest: {device_path}")
    print(f"Port: {port}")

    ser = serial.Serial(port, 115200, timeout=2)
    time.sleep(1)
    ser.reset_input_buffer()

    # OPEN
    print("Opening file on device...")
    path_bytes = device_path.encode("utf-8")
    open_payload = bytes([len(path_bytes)]) + path_bytes + struct.pack("<I", file_size)
    send_frame(ser, 0x01, open_payload)

    cmd, status = read_response(ser)
    if status != 0:
        print(f"ERROR: OPEN failed (status={status})")
        ser.close()
        sys.exit(1)
    print("Device ready.")

    # DATA
    sent = 0
    errors = 0
    start_time = time.time()

    print("Uploading...")
    while sent < file_size:
        chunk = data[sent:sent + CHUNK_SIZE]
        send_frame(ser, 0x02, chunk)

        # Wait for response with retry
        ok = False
        for attempt in range(3):
            cmd, status = read_response(ser, 10)
            if status == 0:
                ok = True
                break
            if status is None:
                # Timeout — small delay and retry
                time.sleep(0.1)
                ser.reset_input_buffer()
                send_frame(ser, 0x02, chunk)  # resend

        if ok:
            sent += len(chunk)
            pct = sent * 100 // file_size
            elapsed = time.time() - start_time
            rate = sent / elapsed if elapsed > 0 else 0
            print(f"\r  {sent}/{file_size} ({pct}%) {rate/1024:.1f} KB/s", end="", flush=True)
        else:
            errors += 1
            print(f"\n  Error at {sent}")
            if errors > 20:
                print("Too many errors.")
                break

    print()

    # CLOSE
    send_frame(ser, 0x03)
    cmd, status = read_response(ser)

    elapsed = time.time() - start_time
    ser.close()

    if errors == 0:
        print(f"Upload complete! {sent} bytes in {elapsed:.1f}s ({sent/elapsed/1024:.1f} KB/s)")
    else:
        print(f"Upload finished with {errors} errors.")


if __name__ == "__main__":
    main()
