"""
Measure BCH enroll and query latency from the host side.

Sends 'be' and 'bq' commands over the FPGA debug UART and times how long
it takes to receive the final response line.  No firmware changes required.

Usage:
    python collect_perf.py --port /dev/tty.usbserial-XXXX
    python collect_perf.py --port /dev/tty.usbserial-XXXX --enroll-n 10 --query-n 50
"""

import argparse
import csv
import statistics
import time

import serial


BAUD = 115200


def open_port(port: str) -> serial.Serial:
    ser = serial.Serial(port, BAUD, timeout=5.0)
    time.sleep(0.3)
    ser.reset_input_buffer()
    return ser


def send_and_wait(ser: serial.Serial, cmd: str, sentinel: str,
                  timeout: float = 60.0) -> float:
    """
    Send cmd, return elapsed seconds until a line containing sentinel arrives.
    """
    ser.reset_input_buffer()
    t0 = time.perf_counter()
    ser.write((cmd + "\r").encode("ascii"))
    ser.flush()

    deadline = time.time() + timeout
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").strip()
        if not line:
            continue
        if sentinel in line:
            return time.perf_counter() - t0

    raise TimeoutError(f"Timed out waiting for '{sentinel}' (cmd='{cmd}')")


def setup(ser: serial.Serial, state: str, seed: str, chal: str) -> None:
    print("lc  — loading valid challenges from SD...")
    send_and_wait(ser, "lc", "valid challenges from SD")

    print("ls  — loading LR-PUF states from SD...")
    send_and_wait(ser, "ls", "States loaded from SD")

    print(f"rs {state} {seed}  — initialising bench state...")
    send_and_wait(ser, f"rs {state} {seed}", "LR-PUF: State")

    print(f"be {state} {chal}  — enrolling once to seed BCH helpers for query bench...")
    send_and_wait(ser, f"be {state} {chal}", "BCH_SEED:")
    print("Setup done.\n")


def run_bench(ser: serial.Serial, cmd: str, sentinel: str,
              n: int, label: str) -> list[float]:
    samples = []
    for i in range(n):
        elapsed = send_and_wait(ser, cmd, sentinel)
        samples.append(elapsed * 1000.0)   # convert to ms
        print(f"  {label} [{i+1:2d}/{n}]  {elapsed*1000:.1f} ms")
    return samples


def print_stats(label: str, samples_ms: list[float]) -> None:
    n = len(samples_ms)
    mean   = statistics.mean(samples_ms)
    median = statistics.median(samples_ms)
    stdev  = statistics.stdev(samples_ms) if n > 1 else 0.0
    print(f"\n{label} (n={n})")
    print(f"  min    {min(samples_ms):.1f} ms")
    print(f"  max    {max(samples_ms):.1f} ms")
    print(f"  mean   {mean:.1f} ms")
    print(f"  median {median:.1f} ms")
    print(f"  stdev  {stdev:.1f} ms")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Measure BCH enroll/query latency from the host side"
    )
    parser.add_argument("--port", required=True,
                        help="Debug UART serial port (USB-C on FPGA)")
    parser.add_argument("--state",      default="a",        help="Hex state index (default: a = 10)")
    parser.add_argument("--chal",       default="1",        help="Hex challenge ID (default: 1)")
    parser.add_argument("--seed",       default="42424242", help="Hex seed for rs command")
    parser.add_argument("--enroll-count", type=int, default=10, help="Number of enroll samples")
    parser.add_argument("--query-count",  type=int, default=50, help="Number of query samples")
    parser.add_argument("--output",     default="perf.csv", help="Output CSV file")
    parser.add_argument("--no-setup",   action="store_true",
                        help="Skip lc/ls/rs/be setup (state already ready)")
    args = parser.parse_args()

    ser = open_port(args.port)
    print(f"Connected to {args.port} at {BAUD} baud\n")

    if not args.no_setup:
        setup(ser, args.state, args.seed, args.chal)

    # --- BCH query bench (run BEFORE enroll bench so helpers are valid) ---
    print(f"Timing 'bq {args.state} {args.chal}'  ({args.query_count} samples)...")
    query_ms = run_bench(ser, f"bq {args.state} {args.chal}",
                         sentinel="BCH_SEED:", n=args.query_count, label="query")

    # --- BCH enroll bench ---
    print(f"\nTiming 'be {args.state} {args.chal}'  ({args.enroll_count} samples)...")
    enroll_ms = run_bench(ser, f"be {args.state} {args.chal}",
                          sentinel="BCH_SEED:", n=args.enroll_count, label="enroll")

    ser.close()

    # --- statistics ---
    print_stats("BCH query  (bq)", query_ms)
    print_stats("BCH enroll (be)", enroll_ms)

    # --- CSV ---
    with open(args.output, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["benchmark", "sample", "ms"])
        for i, v in enumerate(query_ms):
            w.writerow(["bch_query", i, f"{v:.3f}"])
        for i, v in enumerate(enroll_ms):
            w.writerow(["bch_enroll", i, f"{v:.3f}"])
    print(f"\nSaved {len(query_ms)+len(enroll_ms)} samples to '{args.output}'")


if __name__ == "__main__":
    main()
