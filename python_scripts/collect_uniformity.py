"""
Collect CHOICE PUF uniformity and bit-independence data.

Sends 'pv' to the FPGA debug UART (USB-C at 115200 baud), which sweeps every
valid challenge and returns one raw 64-bit response per line.  Saves results
to a CSV for analysis by analyze_puf.py.

Usage:
    python collect_uniformity.py --port /dev/tty.usbserial-XXXX
    python collect_uniformity.py --port /dev/tty.usbserial-XXXX --output uniformity.csv
    python collect_uniformity.py --port /dev/tty.usbserial-XXXX --no-load
"""

import argparse
import csv
import sys
import time

import serial


BAUD = 115200
LINE_TIMEOUT = 2.0   # seconds per readline (should never trigger during normal output)


def open_port(port: str) -> serial.Serial:
    ser = serial.Serial(port, BAUD, timeout=LINE_TIMEOUT)
    time.sleep(0.2)
    ser.reset_input_buffer()
    return ser


def send_cmd(ser: serial.Serial, cmd: str, sentinel: str,
             cmd_timeout: float = 30.0) -> list[str]:
    """
    Send a text command to the debug UART and collect output lines until
    a line containing `sentinel` is found.  The sentinel line itself is not
    included in the returned list.

    The firmware echoes each character as it is typed, then outputs \\r\\n when
    ENTER ('\\r') is received.  The first line returned by readline() after
    sending a command is therefore the echo of the command itself; subsequent
    lines are the command's output.
    """
    ser.reset_input_buffer()
    ser.write((cmd + '\r').encode('ascii'))
    ser.flush()

    lines: list[str] = []
    deadline = time.time() + cmd_timeout

    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode('ascii', errors='replace').strip()
        if not line:
            continue
        if sentinel in line:
            return lines
        lines.append(line)

    raise TimeoutError(
        f"Timed out after {cmd_timeout:.0f}s waiting for '{sentinel}' "
        f"(command: '{cmd}')"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Collect CHOICE PUF uniformity data via the FPGA debug UART"
    )
    parser.add_argument("--port", required=True,
                        help="Serial port for the FPGA debug UART (USB-C)")
    parser.add_argument("--output", default="uniformity.csv",
                        help="Output CSV file (default: uniformity.csv)")
    parser.add_argument("--no-load", action="store_true",
                        help="Skip the 'lc' command (valid challenges already loaded)")
    args = parser.parse_args()

    ser = open_port(args.port)
    print(f"Connected to {args.port} at {BAUD} baud")

    if not args.no_load:
        print("Loading valid challenges from SD card (lc)...")
        try:
            lines = send_cmd(ser, "lc", "valid challenges from SD", cmd_timeout=15.0)
            for ln in lines:
                if ln and not ln.startswith("lc"):   # skip echo
                    print(f"  {ln}")
        except TimeoutError as e:
            print(f"ERROR: {e}", file=sys.stderr)
            print("Make sure the SD card is inserted and run 'si' first if needed.",
                  file=sys.stderr)
            sys.exit(1)

    print("Sweeping all valid challenges (pv)...")
    t0 = time.time()
    try:
        lines = send_cmd(ser, "pv", "PV_END", cmd_timeout=60.0)
    except TimeoutError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
    elapsed = time.time() - t0

    # Parse output: first line after echo is PV_START <count_hex>
    count_expected: int | None = None
    data_lines: list[str] = []
    for line in lines:
        if line.startswith("PV_START"):
            try:
                count_expected = int(line.split()[1], 16)
            except (IndexError, ValueError):
                pass
        elif line.startswith("pv"):   # echo
            pass
        else:
            data_lines.append(line)

    print(f"Received {len(data_lines)} response lines in {elapsed:.2f}s"
          + (f" (expected {count_expected})" if count_expected is not None else ""))

    # Parse data lines: "tc tt bc bt HHHHHHHH:LLLLLLLL"
    rows: list[tuple] = []
    warn_count = 0
    for line in data_lines:
        parts = line.split()
        if len(parts) != 5 or ':' not in parts[4]:
            warn_count += 1
            if warn_count <= 5:
                print(f"  Warning: skipping unexpected line: {line!r}", file=sys.stderr)
            continue
        try:
            tc  = int(parts[0], 16)
            tt  = int(parts[1], 16)
            bc  = int(parts[2], 16)
            bt  = int(parts[3], 16)
            hi_str, lo_str = parts[4].split(':')
            hi  = int(hi_str, 16)
            lo  = int(lo_str, 16)
            response = (hi << 32) | lo
        except ValueError:
            warn_count += 1
            if warn_count <= 5:
                print(f"  Warning: parse error on line: {line!r}", file=sys.stderr)
            continue
        rows.append((tc, tt, bc, bt, hi_str.upper(), lo_str.upper(), response))

    if warn_count > 5:
        print(f"  ... ({warn_count} unexpected lines total)", file=sys.stderr)

    with open(args.output, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['tc', 'tt', 'bc', 'bt', 'hi_hex', 'lo_hex', 'response'])
        writer.writerows(rows)

    print(f"Saved {len(rows)} rows to '{args.output}'")

    if not rows:
        print("No data collected. Exiting.", file=sys.stderr)
        sys.exit(1)

    # Quick uniformity summary
    total_bits = len(rows) * 64
    total_ones = sum(bin(r[6]).count('1') for r in rows)
    pct_ones = 100.0 * total_ones / total_bits
    deviation = abs(pct_ones - 50.0)
    print(f"\nUniformity summary ({len(rows)} valid challenges, {total_bits} bits):")
    print(f"  Overall fraction of 1s : {pct_ones:.2f}%  (ideal 50.00%)")
    print(f"  Deviation from ideal   : {deviation:.2f} percentage points")

    # Per-bit summary (bits 0-63)
    responses = [r[6] for r in rows]
    bit_ones = [sum((r >> b) & 1 for r in responses) for b in range(64)]
    bit_pct  = [100.0 * ones / len(responses) for ones in bit_ones]
    worst_bit = max(range(64), key=lambda b: abs(bit_pct[b] - 50.0))
    print(f"  Worst bit              : bit[{worst_bit}] = {bit_pct[worst_bit]:.1f}% ones")
    print(f"\nRun 'python analyze_puf.py --uniformity {args.output}' for plots.")

    ser.close()


if __name__ == "__main__":
    main()
