"""
Collect raw bytes from the Arduino TRNG (analogRead floating pin entropy source).

Sends 'trng_dump N' over the Arduino serial port and saves the raw bytes to a
binary file suitable for analysis with NIST SP 800-22 or similar test suites.

The Arduino outputs:
  TRNG_START <n_bytes>
  <hex bytes, all on one line>
  TRNG_END

NIST SP 800-22 requires at least 1,000,000 bits (125,000 bytes) to run all 15
tests meaningfully. Use --bytes 125000 or higher.

Usage:
    python collect_trng.py --port /dev/tty.usbmodem-XXXX
    python collect_trng.py --port /dev/tty.usbmodem-XXXX --bytes 125000 --output trng.bin
"""

import argparse
import os
import sys
import time

import serial


BAUD         = 115200
LINE_TIMEOUT = 2.0
# generateRandomSeed() takes ~8 bits × 8 samples × 1ms delay × 32 bits = ~256ms per call
# Each call produces 4 bytes, so throughput ≈ 4 / 0.256 ≈ 15 bytes/s
BYTES_PER_SEC = 15


def open_port(port: str) -> serial.Serial:
    ser = serial.Serial(port, BAUD, timeout=LINE_TIMEOUT)
    time.sleep(0.5)
    ser.reset_input_buffer()
    return ser


def _extract_bytes(buf: bytes, up_to_end: bool = True) -> bytes:
    """Extract complete hex bytes from buf, between TRNG_START and optionally TRNG_END."""
    text = buf.decode('ascii', errors='replace')
    start_idx = text.find("TRNG_START")
    if start_idx == -1:
        return b""
    after_start = text[text.find('\n', start_idx) + 1:]
    end_idx = after_start.find("TRNG_END")
    if up_to_end and end_idx == -1:
        return b""
    if end_idx != -1:
        after_start = after_start[:end_idx]
    hex_str = ''.join(c for c in after_start if c in '0123456789abcdefABCDEF')
    hex_str = hex_str[:len(hex_str) & ~1]  # trim to complete bytes
    return bytes.fromhex(hex_str) if hex_str else b""


def collect(ser: serial.Serial, n_bytes: int) -> tuple[bytes, bool]:
    """Return (data, complete) where complete=False if interrupted early."""
    cmd = f"trng_dump {n_bytes}\n"
    ser.reset_input_buffer()
    ser.write(cmd.encode('ascii'))
    ser.flush()

    eta = n_bytes / BYTES_PER_SEC
    print(f"  Requesting {n_bytes} bytes (estimated {eta:.0f}s at ~{BYTES_PER_SEC} B/s)...")

    total_timeout = n_bytes * 0.08 + 30.0
    deadline = time.time() + total_timeout
    ser.timeout = 2.0

    buf = b""
    last_print = time.time()
    t_start = time.time()

    try:
        while time.time() < deadline:
            chunk = ser.read(512)
            if chunk:
                buf += chunk

            text = buf.decode('ascii', errors='replace')
            start_idx = text.find("TRNG_START")
            end_idx   = text.find("TRNG_END")

            if start_idx != -1 and time.time() - last_print >= 1.0:
                data_so_far = text[start_idx:]
                hex_chars = sum(c in '0123456789abcdefABCDEF' for c in data_so_far)
                collected = hex_chars // 2
                pct = min(int(100 * collected / n_bytes), 99)
                bar = '#' * (pct // 5) + '.' * (20 - pct // 5)
                elapsed = time.time() - t_start
                rate = collected / elapsed if elapsed > 2 else BYTES_PER_SEC
                eta_s = (n_bytes - collected) / rate if rate > 0 else 0
                eta_str = (f"{int(eta_s//3600)}h{int((eta_s%3600)//60):02d}m"
                           if eta_s >= 60 else f"{eta_s:.0f}s")
                print(f"\r  [{bar}] {pct:3d}%  {collected}/{n_bytes} bytes  ETA {eta_str}   ",
                      end='', flush=True)
                last_print = time.time()

            if start_idx != -1 and end_idx != -1 and end_idx > start_idx:
                data = _extract_bytes(buf, up_to_end=True)
                got = len(data)
                pct = int(100 * got / n_bytes)
                bar = '#' * (pct // 5) + '.' * (20 - pct // 5)
                print(f"\r  [{bar}] {pct:3d}%  {got}/{n_bytes} bytes", flush=True)
                if got < n_bytes:
                    raise ValueError(
                        f"Arduino returned only {got} bytes instead of {n_bytes} — "
                        f"firmware cap may be too low"
                    )
                return data, True

        raise TimeoutError(f"Timed out after {total_timeout:.0f}s")

    except KeyboardInterrupt:
        print("\nInterrupted — saving collected bytes so far...")
        partial = _extract_bytes(buf, up_to_end=False)
        return partial, False


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Collect Arduino TRNG bytes for NIST SP 800-22 analysis"
    )
    parser.add_argument("--port",   required=True,
                        help="Arduino serial port (e.g. /dev/tty.usbmodem-XXXX)")
    parser.add_argument("--bytes",  type=int, default=125000,
                        help="Number of bytes to collect (default: 125000 = 1Mbit)")
    parser.add_argument("--output", default="trng.bin",
                        help="Output binary file (default: trng.bin)")
    args = parser.parse_args()

    ser = open_port(args.port)
    print(f"Connected to {args.port} at {BAUD} baud")

    # Check how many bytes already exist so we can resume
    already = 0
    if args.output != '-':
        try:
            already = os.path.getsize(args.output)
        except FileNotFoundError:
            already = 0

    remaining = args.bytes - already
    if remaining <= 0:
        print(f"'{args.output}' already has {already} bytes — target reached, nothing to do.")
        ser.close()
        return

    if already > 0:
        print(f"Resuming: {already} bytes already in '{args.output}', "
              f"need {remaining} more.")

    t0 = time.time()
    data = b""
    complete = False
    try:
        data, complete = collect(ser, remaining)
    except (TimeoutError, ValueError) as e:
        print(f"\nERROR: {e}", file=sys.stderr)

    elapsed = time.time() - t0

    if data:
        with open(args.output, 'ab') as f:
            f.write(data)

    total = already + len(data)
    rate = len(data) / elapsed if elapsed > 0 else 0
    print(f"\nCollected {len(data)} bytes in {elapsed:.1f}s ({rate:.1f} B/s)")
    print(f"Total in '{args.output}': {total}/{args.bytes} bytes")
    if not complete and total < args.bytes:
        print(f"Re-run the same command to resume ({args.bytes - total} bytes remaining).")
    print(f"\nTo run NIST SP 800-22:")
    print(f"  pip install nistrng")
    print(f"  python -c \"import nistrng; nistrng.run_all_battery(open('{args.output}','rb').read())\"")

    ser.close()


if __name__ == "__main__":
    main()
