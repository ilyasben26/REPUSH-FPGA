import argparse
import csv
import sys
import time
from datetime import timedelta

import serial

# PUF configuration
PUF_RESPONSE_BITS = 64  # Number of PUF cells in the FPGA design
PUF_RESPONSE_BYTES = PUF_RESPONSE_BITS // 8
PUF_RESPONSE_HEX_DIGITS = PUF_RESPONSE_BITS // 4
PUF_RESPONSE_MASK = (1 << PUF_RESPONSE_BITS) - 1

# # Set ASR tuning (upper=7, lower=3)
# python puf_uart_client.py --port /dev/ttyUSB0 tune 31 15

# # Set ASR choice (top=3, bottom=0)
# python puf_uart_client.py --port /dev/ttyUSB0 choice 3 0

# # Set top pattern and in-pattern bit
# python puf_uart_client.py --port /dev/ttyUSB0 top 0xAAAAAAAA 0

# # Set bottom pattern and in-pattern bit
# python puf_uart_client.py --port /dev/ttyUSB0 bottom 0x55555555 1

# # Request PUF measurement
# python puf_uart_client.py --port /dev/ttyUSB0 req

def build_payload(command, data_value):
    if command < 0 or command > 0xF:
        raise ValueError("command must be 4-bit (0..15)")
    if data_value < 0 or data_value >= (1 << 60):
        raise ValueError("data_value must fit in lower 60 bits")
    payload_value = ((command & 0xF) << 60) | data_value
    return payload_value.to_bytes(8, byteorder="big")


def send_command(ser, payload):
    if len(payload) != 8:
        raise ValueError("payload must be exactly 8 bytes")
    ser.write(payload)
    ser.flush()


def read_response(ser, timeout=1.0):
    ser.timeout = timeout
    # The FPGA UART interface sends a full PUF response.
    data = ser.read(PUF_RESPONSE_BYTES)
    if len(data) != PUF_RESPONSE_BYTES:
        raise TimeoutError(f"Timed out waiting for {PUF_RESPONSE_BYTES}-byte response")
    return data


def main():
    parser = argparse.ArgumentParser(description="PUF UART client")
    parser.add_argument("--port", required=True, help="Serial port (e.g., COM3)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")

    subparsers = parser.add_subparsers(dest="cmd", required=True)

    p_req = subparsers.add_parser("req", help="Request PUF measurement")
    p_req.add_argument("--delay", type=float, default=0.05, help="Delay before read (s)")

    p_tune = subparsers.add_parser("tune", help="Set ASR tuning")
    p_tune.add_argument("upper", type=int, help="Upper tuning (0..7)")
    p_tune.add_argument("lower", type=int, help="Lower tuning (0..7)")

    p_choice = subparsers.add_parser("choice", help="Set ASR choice")
    p_choice.add_argument("top", type=int, help="Top ASR index (0..3)")
    p_choice.add_argument("bottom", type=int, help="Bottom ASR index (0..3)")

    p_top = subparsers.add_parser("top", help="Set top pattern")
    p_top.add_argument("pattern", type=lambda x: int(x, 16), help="32-bit hex pattern")
    p_top.add_argument("in_bit", type=int, choices=[0, 1], help="Top in-pattern bit")

    p_bottom = subparsers.add_parser("bottom", help="Set bottom pattern")
    p_bottom.add_argument("pattern", type=lambda x: int(x, 16), help="32-bit hex pattern")
    p_bottom.add_argument("in_bit", type=int, choices=[0, 1], help="Bottom in-pattern bit")

    p_challenge = subparsers.add_parser("challenge", help="Challenge the PUF")
    p_challenge.add_argument("--top-tune", type=int, help="Upper tuning (0..7)")
    p_challenge.add_argument("--bottom-tune", type=int, help="Lower tuning (0..7)")
    p_challenge.add_argument("--top-choice", type=int, help="Top ASR index (0..3)")
    p_challenge.add_argument("--bottom-choice", type=int, help="Bottom ASR index (0..3)")
    p_challenge.add_argument("--count", type=int, default=1, help="Number of requests to perform")
    p_challenge.add_argument("--resp-delay", type=float, default=0.05, help="Delay (s) before reading response")
    p_challenge.add_argument(
        "--majority-mode",
        choices=["bit", "sequence"],
        default="bit",
        help="Majority voting mode: 'bit' for per-bit voting, 'sequence' for most frequent full response"
    )
    p_challenge.add_argument(
        "--show-intermediate",
        action="store_true",
        help="Print each intermediate PUF response before majority voting"
    )

    p_gather = subparsers.add_parser("gather", help="Gather PUF responses for all configurations")
    p_gather.add_argument("--output", type=str, default="puf_responses.csv", help="Output CSV file")
    p_gather.add_argument("--count", type=int, default=100, help="Number of responses per configuration")
    p_gather.add_argument("--resp-delay", type=float, default=0.005, help="Delay (s) before reading each response (default: 5ms)")

    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1.0)
    time.sleep(0.1)
    
    # Flush any leftover bytes in the OS serial buffer from previous partial reads
    ser.reset_input_buffer()

    try:
        if args.cmd == "req":
            payload = build_payload(0x1, 0)
            send_command(ser, payload)
            time.sleep(args.delay)
            resp = read_response(ser)
            value = int.from_bytes(resp, 'big') & PUF_RESPONSE_MASK
            print(f"PUF response: 0x{value:0{PUF_RESPONSE_HEX_DIGITS}X} ({value:0{PUF_RESPONSE_BITS}b})")

        elif args.cmd == "tune":
            if not (0 <= args.upper <= 7 and 0 <= args.lower <= 7):
                raise ValueError("upper/lower must be 0..7")
            data_value = ((args.upper & 0x7) << 5) | ((args.lower & 0x7) << 2)
            payload = build_payload(0x2, data_value)
            send_command(ser, payload)
            print("ASR tuning set")

        elif args.cmd == "choice":
            if not (0 <= args.top <= 3 and 0 <= args.bottom <= 3):
                raise ValueError("top/bottom must be 0..3")
            data_value = ((args.top & 0x3) << 2) | (args.bottom & 0x3)
            payload = build_payload(0x3, data_value)
            send_command(ser, payload)
            print("ASR choice set")

        elif args.cmd == "top":
            pattern = args.pattern & 0xFFFFFFFF
            in_bit = args.in_bit & 0x1
            data_value = ((in_bit & 0x1) << 32) | pattern
            payload = build_payload(0x4, data_value)
            send_command(ser, payload)
            print("Top pattern set")

        elif args.cmd == "bottom":
            pattern = args.pattern & 0xFFFFFFFF
            in_bit = args.in_bit & 0x1
            data_value = ((in_bit & 0x1) << 32) | pattern
            payload = build_payload(0x5, data_value)
            send_command(ser, payload)
            print("Bottom pattern set")
        
        elif args.cmd == "challenge":
            challenge_start = time.monotonic()
            if args.top_choice <= args.bottom_choice:
                raise ValueError("top_choice must be greater than bottom_choice")

            # Helper to send setup command (no response expected from FPGA)
            def send_setup(payload, delay=0.01):
                send_command(ser, payload)
                time.sleep(delay)  # Allow FPGA to process

            # 1. Tune top and bottom
            if not (0 <= args.top_tune <= 7 and 0 <= args.bottom_tune <= 7):
                raise ValueError("top/bottom tune must be 0..7")
            tune_val = ((args.top_tune & 0x7) << 5) | ((args.bottom_tune & 0x7) << 2)
            send_setup(build_payload(0x2, tune_val))

            # 2. Set choices
            if not (0 <= args.top_choice <= 3 and 0 <= args.bottom_choice <= 3):
                raise ValueError("top/bottom choice must be 0..3")
            choice_val = ((args.top_choice & 0x3) << 2) | (args.bottom_choice & 0x3)
            send_setup(build_payload(0x3, choice_val))

            # 3. Bottom pattern
            if args.bottom_choice % 2 == 0:
                bottom_pattern, bottom_in_bit = 0xAAAAAAAA, 0
            else:
                bottom_pattern, bottom_in_bit = 0x55555555, 1

            # 4. Top pattern
            if args.top_choice % 2 != 0:
                top_pattern, top_in_bit = 0xAAAAAAAA, 0
            else:
                top_pattern, top_in_bit = 0x55555555, 1

            # Set top pattern
            top_val = ((top_in_bit & 0x1) << 32) | (top_pattern & 0xFFFFFFFF)
            send_setup(build_payload(0x4, top_val))

            # Set bottom pattern
            bottom_val = ((bottom_in_bit & 0x1) << 32) | (bottom_pattern & 0xFFFFFFFF)
            send_setup(build_payload(0x5, bottom_val))

            # 5. Query PUF count times - each PUF_REQ gets exactly one full response
            ser.reset_input_buffer()  # Clean buffer before measurement loop
            bit_ones = [0] * PUF_RESPONSE_BITS
            sequence_counts = {}
            for i in range(args.count):
                send_command(ser, build_payload(0x1, 0))
                time.sleep(args.resp_delay)
                resp = read_response(ser)
                value = int.from_bytes(resp, 'big') & PUF_RESPONSE_MASK
                for bit in range(PUF_RESPONSE_BITS):
                    bit_ones[bit] += (value >> bit) & 0x1
                sequence_counts[value] = sequence_counts.get(value, 0) + 1
                if args.show_intermediate:
                    print(f"PUF response {i+1}: 0x{value:0{PUF_RESPONSE_HEX_DIGITS}X} ({value:0{PUF_RESPONSE_BITS}b})")

            if args.majority_mode == "bit":
                # Per-bit majority vote over all collected responses.
                majority_value = 0
                for bit in range(PUF_RESPONSE_BITS):
                    if bit_ones[bit] * 2 >= args.count:
                        majority_value |= (1 << bit)
            else:
                # Per-sequence majority: pick the most frequent full response.
                # In a tie, the first sequence that reached the top count is kept.
                majority_value = max(sequence_counts, key=sequence_counts.get)

            print(f"Majority mode         : {args.majority_mode}")
            print(f"Majority-voted response: {majority_value:0{PUF_RESPONSE_BITS}b}")
            print(f"Majority-voted decimal : {majority_value}")

            # Per-bit summary across collected responses.
            print("Per-bit summary (% of ones/zeros):")
            for bit in range(PUF_RESPONSE_BITS - 1, -1, -1):
                ones_pct = (bit_ones[bit] * 100.0) / args.count
                zeros_pct = 100.0 - ones_pct
                print(f"  bit[{bit:02d}] -> 1: {ones_pct:6.2f}% | 0: {zeros_pct:6.2f}%")

            challenge_elapsed = time.monotonic() - challenge_start
            print(f"Challenge elapsed time : {challenge_elapsed:.3f} s ({timedelta(seconds=int(challenge_elapsed))})")

        elif args.cmd == "gather":
            # Helper to send setup command (no response expected from FPGA)
            def send_setup(payload, delay=0.002):
                send_command(ser, payload)
                time.sleep(delay)

            # Generate all valid choice combinations (top_choice > bottom_choice)
            choice_combinations = [
                (tc, bc) for tc in range(4) for bc in range(4) if tc > bc
            ]
            
            total_configs = len(choice_combinations) * 8 * 8
            total_responses = total_configs * args.count

            # ------------------------------------------------------------------
            # Resume: read existing CSV to find fully-completed configurations.
            # A config is complete if it already has exactly args.count rows.
            # Partial configs (interrupted mid-way) are discarded and re-run.
            # ------------------------------------------------------------------
            import os
            from collections import Counter

            completed_configs = set()   # (top_choice, top_tune, bottom_choice, bottom_tune)
            resume_mode = os.path.exists(args.output)

            if resume_mode:
                row_counts = Counter()
                try:
                    with open(args.output, 'r', newline='') as existing:
                        reader = csv.DictReader(existing)
                        for row in reader:
                            key = (int(row['top_choice']), int(row['top_tune']),
                                   int(row['bottom_choice']), int(row['bottom_tune']))
                            row_counts[key] += 1
                    completed_configs = {k for k, v in row_counts.items() if v >= args.count}
                    partial = {k for k, v in row_counts.items() if 0 < v < args.count}
                    print(f"Resume: found {len(completed_configs)} completed configs, "
                          f"{len(partial)} partial (will re-run), "
                          f"{total_configs - len(completed_configs)} remaining")
                except (KeyError, ValueError) as e:
                    print(f"Warning: could not parse existing CSV ({e}), starting fresh")
                    resume_mode = False
                    completed_configs = set()

            file_mode = 'a' if resume_mode else 'w'
            print(f"Gathering {args.count} responses for {total_configs} configurations "
                  f"({total_responses} total) — {'appending to' if resume_mode else 'creating'} {args.output}")

            with open(args.output, file_mode, newline='') as csvfile:
                writer = csv.writer(csvfile)
                if not resume_mode:
                    writer.writerow(['top_choice', 'top_tune', 'bottom_choice', 'bottom_tune',
                                     'response'])
                
                config_num = 0  # counts only configs processed in this session
                gather_start = time.monotonic()

                # Cache last-sent setup values to skip redundant commands
                last_choice_val = None
                last_top_val = None
                last_bottom_val = None

                for top_choice, bottom_choice in choice_combinations:
                    for top_tune in range(8):
                        for bottom_tune in range(8):
                            # Skip already-completed configurations
                            cfg_key = (top_choice, top_tune, bottom_choice, bottom_tune)
                            if cfg_key in completed_configs:
                                continue

                            config_num += 1
                            
                            # 1. Set tuning (changes every iteration)
                            tune_val = ((top_tune & 0x7) << 5) | ((bottom_tune & 0x7) << 2)
                            send_setup(build_payload(0x2, tune_val))

                            # 2. Set choices (only changes when top/bottom choice changes)
                            choice_val = ((top_choice & 0x3) << 2) | (bottom_choice & 0x3)
                            if choice_val != last_choice_val:
                                send_setup(build_payload(0x3, choice_val))
                                last_choice_val = choice_val

                            # 3. Set patterns (only change with choice, not tune)
                            if bottom_choice % 2 == 0:
                                bottom_pattern, bottom_in_bit = 0xAAAAAAAA, 0
                            else:
                                bottom_pattern, bottom_in_bit = 0x55555555, 1

                            if top_choice % 2 != 0:
                                top_pattern, top_in_bit = 0xAAAAAAAA, 0
                            else:
                                top_pattern, top_in_bit = 0x55555555, 1

                            top_val = ((top_in_bit & 0x1) << 32) | (top_pattern & 0xFFFFFFFF)
                            if top_val != last_top_val:
                                send_setup(build_payload(0x4, top_val))
                                last_top_val = top_val

                            bottom_val = ((bottom_in_bit & 0x1) << 32) | (bottom_pattern & 0xFFFFFFFF)
                            if bottom_val != last_bottom_val:
                                send_setup(build_payload(0x5, bottom_val))
                                last_bottom_val = bottom_val

                            # 4. Query PUF count times
                            ser.reset_input_buffer()
                            for meas_idx in range(args.count):
                                send_command(ser, build_payload(0x1, 0))
                                time.sleep(args.resp_delay)
                                try:
                                    resp = read_response(ser)
                                except TimeoutError:
                                    raise TimeoutError(
                                        f"Timed out on config {config_num}/{total_configs} "
                                        f"(top_choice={top_choice}, top_tune={top_tune}, "
                                        f"bottom_choice={bottom_choice}, bottom_tune={bottom_tune}), "
                                        f"measurement {meas_idx + 1}/{args.count}. "
                                        f"Try increasing --resp-delay (current: {args.resp_delay}s)."
                                    )
                                value = int.from_bytes(resp, 'big') & PUF_RESPONSE_MASK
                                writer.writerow([top_choice, top_tune, bottom_choice, bottom_tune,
                                                 value])
                            
                            # Progress update
                            done_total = len(completed_configs) + config_num
                            elapsed = time.monotonic() - gather_start
                            rate = config_num / elapsed if elapsed > 0 else 0  # configs/sec this session
                            remaining = total_configs - done_total
                            eta_sec = remaining / rate if rate > 0 else 0
                            bar_width = 30
                            filled = int(bar_width * done_total / total_configs)
                            bar = '█' * filled + '░' * (bar_width - filled)
                            pct = 100 * done_total / total_configs
                            print(
                                f"\r[{bar}] {pct:5.1f}%  "
                                f"{done_total}/{total_configs} configs  "
                                f"elapsed {timedelta(seconds=int(elapsed))}  "
                                f"ETA {timedelta(seconds=int(eta_sec))}  "
                                f"{rate:.2f} cfg/s  "
                                f"(tc={top_choice} tt={top_tune:2d} bc={bottom_choice} bt={bottom_tune:2d})",
                                end='', flush=True
                            )
                            if done_total == total_configs:
                                print()  # Final newline
            
            print(f"Done! Results saved to {args.output}")


    finally:
        ser.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
