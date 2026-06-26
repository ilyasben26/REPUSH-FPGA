"""
Collect CHOICE PUF reliability (BER) and majority-voting data.

For each selected valid challenge:
  1. Sets the PUF with 'ph tc,tt,bc,bt'
  2. Collects N individual (non-voted) raw 64-bit samples with 'pm N'

All collected raw samples are saved to a CSV.  The analysis script
(analyze_puf.py) then computes BER per challenge, derives the majority-vote
reference from the samples themselves, and simulates voting at different counts.

NOTE: firmware arguments are always parsed as hex.
  'pm 64' = 100 samples,  'pm c8' = 200 samples,  'pm a' = 10 samples.
  The --samples argument to this script is in decimal; the script converts it.

Usage:
    python collect_ber.py --port /dev/tty.usbserial-XXXX
    python collect_ber.py --port /dev/tty.usbserial-XXXX --samples 200 --max-challenges 30
    python collect_ber.py --port /dev/tty.usbserial-XXXX --no-load --output ber.csv
"""

import argparse
import csv
import random
import sys
import time

import serial


BAUD = 115200
LINE_TIMEOUT = 2.0


def open_port(port: str) -> serial.Serial:
    ser = serial.Serial(port, BAUD, timeout=LINE_TIMEOUT)
    time.sleep(0.2)
    ser.reset_input_buffer()
    return ser


def _readline(ser: serial.Serial, deadline: float) -> str | None:
    """Return the next non-empty stripped line, or None if deadline passed."""
    while time.time() < deadline:
        raw = ser.readline()
        if raw:
            return raw.decode('ascii', errors='replace').strip()
    return None


def send_cmd(ser: serial.Serial, cmd: str, sentinel: str,
             cmd_timeout: float = 30.0) -> list[str]:
    """Send command, return lines between the echo and the sentinel."""
    ser.reset_input_buffer()
    ser.write((cmd + '\r').encode('ascii'))
    ser.flush()

    lines: list[str] = []
    deadline = time.time() + cmd_timeout

    while True:
        line = _readline(ser, deadline)
        if line is None:
            raise TimeoutError(
                f"Timed out after {cmd_timeout:.0f}s waiting for '{sentinel}' "
                f"(command: '{cmd}')"
            )
        if sentinel in line:
            return lines
        lines.append(line)


def get_valid_challenges(ser: serial.Serial) -> list[tuple[int, int, int, int]]:
    """Run 'pv' and return a list of (tc, tt, bc, bt) for all valid challenges."""
    lines = send_cmd(ser, "pv", "PV_END", cmd_timeout=60.0)
    challenges = []
    for line in lines:
        parts = line.split()
        if len(parts) == 5 and ':' in parts[4]:
            try:
                challenges.append((
                    int(parts[0], 16),
                    int(parts[1], 16),
                    int(parts[2], 16),
                    int(parts[3], 16),
                ))
            except ValueError:
                pass
    return challenges


def set_challenge(ser: serial.Serial, tc: int, tt: int, bc: int, bt: int) -> None:
    """Send 'ph' to configure the PUF and consume its single-measurement output."""
    ser.reset_input_buffer()
    cmd = f"ph {tc:x},{tt:x},{bc:x},{bt:x}\r"
    ser.write(cmd.encode('ascii'))
    ser.flush()

    # Read until we see the 'PUF:' response line from ph (or timeout)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        raw = ser.readline()
        if raw:
            line = raw.decode('ascii', errors='replace').strip()
            if line.startswith("PUF:"):
                return
    raise TimeoutError(f"ph command timed out for tc={tc} tt={tt} bc={bc} bt={bt}")


def collect_samples(ser: serial.Serial, n_samples: int,
                    prefix: str = "", cmd_timeout: float = 60.0) -> list[int]:
    """Send 'pm N', stream responses line-by-line, and show a live counter."""
    pm_cmd = f"pm {n_samples:x}"   # firmware parses hex: 100 decimal = 0x64
    ser.reset_input_buffer()
    ser.write((pm_cmd + '\r').encode('ascii'))
    ser.flush()

    samples: list[int] = []
    deadline = time.time() + max(cmd_timeout, n_samples * 0.05 + 10)

    while True:
        line = _readline(ser, deadline)
        if line is None:
            raise TimeoutError(
                f"pm timed out after {len(samples)}/{n_samples} samples"
            )
        if "PM_END" in line:
            break
        s = line.strip()
        if len(s) == 17 and s[8] == ':':
            try:
                hi = int(s[:8], 16)
                lo = int(s[9:], 16)
                samples.append((hi << 32) | lo)
            except ValueError:
                pass
        # Update progress on every line (parsed or not) so the user sees motion
        pct = int(100 * len(samples) / n_samples) if n_samples else 100
        bar = '#' * (pct // 5) + '.' * (20 - pct // 5)
        print(f"\r  {prefix}  [{len(samples):3d}/{n_samples}] [{bar}] {pct:3d}%",
              end='', flush=True)

    return samples


def majority_vote(samples: list[int]) -> int:
    """Compute per-bit majority vote across a list of 64-bit integers."""
    n = len(samples)
    if n == 0:
        return 0
    bit_ones = [sum((s >> b) & 1 for s in samples) for b in range(64)]
    return sum(1 << b for b in range(64) if bit_ones[b] * 2 >= n)


def hamming(a: int, b: int) -> int:
    return bin(a ^ b).count('1')


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Collect CHOICE PUF BER data via the FPGA debug UART"
    )
    parser.add_argument("--port", required=True,
                        help="Serial port for the FPGA debug UART (USB-C)")
    parser.add_argument("--samples", type=int, default=100,
                        help="Individual raw samples per challenge (default: 100)")
    parser.add_argument("--max-challenges", type=int, default=None,
                        help="Test at most N challenges (default: all valid)")
    parser.add_argument("--output", default="ber.csv",
                        help="Output CSV file (default: ber.csv)")
    parser.add_argument("--seed", type=int, default=42,
                        help="RNG seed for challenge sub-sampling (default: 42)")
    parser.add_argument("--no-load", action="store_true",
                        help="Skip 'lc' (valid challenges already in FPGA SRAM)")
    args = parser.parse_args()

    if args.samples < 10:
        print("ERROR: --samples must be at least 10 for meaningful BER statistics.",
              file=sys.stderr)
        sys.exit(1)
    if args.samples > 1000:
        print("ERROR: --samples capped at 1000 by the firmware.", file=sys.stderr)
        sys.exit(1)

    ser = open_port(args.port)
    print(f"Connected to {args.port} at {BAUD} baud")

    if not args.no_load:
        print("Loading valid challenges from SD card (lc)...")
        try:
            send_cmd(ser, "lc", "valid challenges from SD", cmd_timeout=15.0)
        except TimeoutError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            sys.exit(1)

    print("Fetching valid challenge list (pv)...")
    challenges = get_valid_challenges(ser)
    print(f"Found {len(challenges)} valid challenges")

    if not challenges:
        print("ERROR: no valid challenges returned.", file=sys.stderr)
        sys.exit(1)

    if args.max_challenges is not None and args.max_challenges < len(challenges):
        random.seed(args.seed)
        challenges = random.sample(challenges, args.max_challenges)
        print(f"Sub-sampled {len(challenges)} challenges (seed={args.seed})")

    n_total = len(challenges) * args.samples
    print(f"Collecting {args.samples} samples x {len(challenges)} challenges "
          f"= {n_total} total PUF queries")
    print(f"Estimated time: {n_total * 0.002:.0f}–{n_total * 0.005:.0f}s")

    rows: list[tuple] = []
    t0 = time.time()

    for idx, (tc, tt, bc, bt) in enumerate(challenges):
        elapsed = time.time() - t0
        done_samples = idx * args.samples
        rate = done_samples / elapsed if elapsed > 1 else 0
        eta_s = ((len(challenges) - idx) * args.samples / rate) if rate > 0 else 0
        eta_str = f"ETA {eta_s:.0f}s" if rate > 0 else "ETA --"
        prefix = f"[{idx+1:3d}/{len(challenges)}] tc={tc} tt={tt} bc={bc} bt={bt} | {eta_str}"

        try:
            set_challenge(ser, tc, tt, bc, bt)
            samples = collect_samples(ser, args.samples, prefix=prefix)
        except TimeoutError as e:
            print(f"\n  Warning: timeout on challenge {idx} "
                  f"(tc={tc} tt={tt} bc={bc} bt={bt}): {e}", file=sys.stderr)
            continue

        got = len(samples)
        elapsed_ch = time.time() - t0 - elapsed
        # End the progress line and print a concise one-line summary
        print(f"\r  [{idx+1:3d}/{len(challenges)}] tc={tc} tt={tt} bc={bc} bt={bt}"
              f"  {got}/{args.samples} samples  ({elapsed_ch:.1f}s)" +
              (" [INCOMPLETE]" if got != args.samples else ""))

        for sample_idx, response in enumerate(samples):
            rows.append((idx, tc, tt, bc, bt, sample_idx, response))

    elapsed = time.time() - t0
    print(f"\nDone in {elapsed:.1f}s")

    with open(args.output, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['challenge_idx', 'tc', 'tt', 'bc', 'bt',
                         'sample_idx', 'response'])
        writer.writerows(rows)

    print(f"Saved {len(rows)} rows to '{args.output}'")

    if not rows:
        sys.exit(1)

    # Quick BER summary computed directly here
    from collections import defaultdict
    per_challenge: dict[int, list[int]] = defaultdict(list)
    for row in rows:
        per_challenge[row[0]].append(row[6])

    all_bers: list[float] = []
    for cidx, samps in per_challenge.items():
        ref = majority_vote(samps)
        for s in samps:
            all_bers.append(hamming(s, ref) / 64.0)

    mean_ber = sum(all_bers) / len(all_bers)
    max_ber  = max(all_bers)
    print(f"\nQuick BER summary (raw, before error correction):")
    print(f"  Samples analysed : {len(all_bers)}")
    print(f"  Mean BER         : {mean_ber * 100:.3f}%  "
          f"({mean_ber * 64:.2f} flips / 64 bits)")
    print(f"  Worst-case BER   : {max_ber * 100:.3f}%  "
          f"({max_ber * 64:.2f} flips / 64 bits)")
    print(f"\nRun 'python analyze_puf.py --ber {args.output}' for full analysis and plots.")

    ser.close()


if __name__ == "__main__":
    main()
