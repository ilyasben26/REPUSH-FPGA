"""
Collect LR-PUF end-to-end reliability data.

For a given state index and challenge ID, this script optionally enrolls the
BCH fuzzy commitment scheme and then runs N repeated queries, recording whether
each query successfully reconstructed the same key as the enrollment.

The FPGA debug commands used:
  be <state_idx> <challenge_id>  — enroll (10-vote majority, stores helper to SD)
  bq <state_idx> <challenge_id>  — query  (single-shot, BCH-corrects up to T=4)

Both arguments are parsed as hex by the firmware.

Output CSV columns:
  query_idx, state_idx, challenge_id, success, key_hex

Usage:
    python collect_lrpuf.py --port /dev/tty.usbserial-XXXX --state 0 --challenge 0
    python collect_lrpuf.py --port /dev/tty.usbserial-XXXX --state 0 --challenge 0 --no-enroll
    python collect_lrpuf.py --port /dev/tty.usbserial-XXXX --state 0 --challenge 0 --queries 1000
"""

import argparse
import csv
import sys
import time

import serial


BAUD        = 115200
LINE_TIMEOUT = 2.0


def open_port(port: str) -> serial.Serial:
    ser = serial.Serial(port, BAUD, timeout=LINE_TIMEOUT)
    time.sleep(0.2)
    ser.reset_input_buffer()
    return ser


def _readline(ser: serial.Serial, deadline: float) -> str | None:
    while time.time() < deadline:
        raw = ser.readline()
        if raw:
            return raw.decode('ascii', errors='replace').strip()
    return None


def enroll(ser: serial.Serial, state_idx: int, challenge_id: int,
           cmd_timeout: float = 15.0) -> str | None:
    """Run 'be' and return the enrollment key hex string, or None on error."""
    cmd = f"be {state_idx:x} {challenge_id:x}"
    print(f"Enrolling: {cmd} ...")
    ser.reset_input_buffer()
    ser.write((cmd + '\r').encode('ascii'))
    ser.flush()
    deadline = time.time() + cmd_timeout
    while True:
        line = _readline(ser, deadline)
        if line is None:
            print("  ERROR: timed out waiting for enrollment response")
            return None
        if line.startswith("BCH_SEED:"):
            key = line[len("BCH_SEED:"):]
            print(f"  Enrolled key : {key}")
            return key
        if "BCH_ERR" in line:
            print("  ERROR: enrollment failed (BCH_ERR)")
            return None


def query(ser: serial.Serial, state_idx: int, challenge_id: int,
          cmd_timeout: float = 5.0) -> tuple[bool, str]:
    """Run 'bq' and return (success, key_hex). key_hex is '' on failure."""
    cmd = f"bq {state_idx:x} {challenge_id:x}"
    ser.reset_input_buffer()
    ser.write((cmd + '\r').encode('ascii'))
    ser.flush()
    deadline = time.time() + cmd_timeout
    while True:
        line = _readline(ser, deadline)
        if line is None:
            return False, ''
        if line.startswith("BCH_SEED:"):
            return True, line[len("BCH_SEED:"):]
        if "BCH_ERR" in line:
            return False, ''


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Collect LR-PUF end-to-end reliability data via FPGA debug UART"
    )
    parser.add_argument("--port",      required=True,
                        help="Serial port for the FPGA debug UART (USB-C)")
    parser.add_argument("--state",     type=lambda x: int(x, 0), default=0,
                        help="State index (decimal or 0x hex, default: 0)")
    parser.add_argument("--challenge", type=lambda x: int(x, 0), default=0,
                        help="Challenge ID (decimal or 0x hex, default: 0)")
    parser.add_argument("--queries",   type=int, default=500,
                        help="Number of query repetitions (default: 500)")
    parser.add_argument("--no-enroll", action="store_true",
                        help="Skip enrollment; use first successful query as reference")
    parser.add_argument("--output",    default="lrpuf.csv",
                        help="Output CSV file (default: lrpuf.csv)")
    args = parser.parse_args()

    ser = open_port(args.port)
    print(f"Connected to {args.port} at {BAUD} baud")

    state_idx    = args.state
    challenge_id = args.challenge

    # Enrollment
    ref_key: str | None = None
    if not args.no_enroll:
        ref_key = enroll(ser, state_idx, challenge_id)
        if ref_key is None:
            print("Aborting: enrollment failed.", file=sys.stderr)
            sys.exit(1)
    else:
        print("Skipping enrollment — reference key will be taken from first successful query.")

    # Query loop
    print(f"\nRunning {args.queries} queries "
          f"(state={state_idx}, challenge={challenge_id})...")

    rows: list[tuple] = []
    n_success = 0
    n_fail    = 0
    n_mismatch = 0
    t0 = time.time()

    for i in range(args.queries):
        pct = int(100 * i / args.queries)
        bar = '#' * (pct // 5) + '.' * (20 - pct // 5)
        print(f"\r  [{i+1:4d}/{args.queries}] [{bar}] {pct:3d}%  "
              f"ok={n_success} fail={n_fail} mismatch={n_mismatch}  querying...",
              end='', flush=True)

        ok, key = query(ser, state_idx, challenge_id)

        if not ok:
            n_fail += 1
            rows.append((i, state_idx, challenge_id, 0, ''))
        else:
            if ref_key is None:
                ref_key = key
                print(f"\n  Reference key (first success): {ref_key}")
            matched = (key == ref_key)
            if matched:
                n_success += 1
            else:
                n_mismatch += 1
            rows.append((i, state_idx, challenge_id, 1 if matched else 2, key))

        pct = int(100 * (i + 1) / args.queries)
        bar = '#' * (pct // 5) + '.' * (20 - pct // 5)
        print(f"\r  [{i+1:4d}/{args.queries}] [{bar}] {pct:3d}%  "
              f"ok={n_success} fail={n_fail} mismatch={n_mismatch}        ",
              end='', flush=True)

    elapsed = time.time() - t0
    print(f"\n\nDone in {elapsed:.1f}s")

    with open(args.output, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['query_idx', 'state_idx', 'challenge_id', 'success', 'key_hex'])
        writer.writerows(rows)
    print(f"Saved {len(rows)} rows to '{args.output}'")

    total = len(rows)
    print(f"\nSummary:")
    print(f"  Total queries     : {total}")
    print(f"  Key matched       : {n_success} ({100*n_success/total:.2f}%)")
    print(f"  BCH decode failed : {n_fail}    ({100*n_fail/total:.2f}%)")
    print(f"  Key mismatch      : {n_mismatch} ({100*n_mismatch/total:.2f}%)")
    print(f"  End-to-end reliability : {100*n_success/total:.4f}%")

    ser.close()


if __name__ == "__main__":
    main()
