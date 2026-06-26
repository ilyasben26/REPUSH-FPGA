"""
Run NIST SP 800-22 statistical tests on a binary file of TRNG bytes.

Usage:
    python analyze_trng.py trng.bin
"""

import argparse
import numpy
from nistrng import *


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run NIST SP 800-22 tests on a binary file of random bytes"
    )
    parser.add_argument("input", help="Binary file to test (e.g. trng.bin)")
    args = parser.parse_args()

    raw = numpy.frombuffer(open(args.input, 'rb').read(), dtype=numpy.uint8)
    binary_sequence = pack_sequence(raw.astype(int))

    print(f"Loaded {len(raw)} bytes ({len(raw) * 8:,} bits) from '{args.input}'")
    print("Checking test eligibility...")
    eligible_battery = check_eligibility_all_battery(binary_sequence, SP800_22R1A_BATTERY)
    print(f"{len(eligible_battery)}/{len(SP800_22R1A_BATTERY)} tests eligible\n")

    results = run_all_battery(binary_sequence, eligible_battery, False)

    passed = 0
    failed = 0
    print(f"{'Result':<8} {'Score':<10} {'Time (ms)':<12} Test")
    print("-" * 65)
    for result, elapsed in results:
        status = "PASS" if result.passed else "FAIL"
        marker = "" if result.passed else "  <---"
        print(f"{status:<8} {numpy.round(result.score, 4):<10} {elapsed:<12.1f} {result.name}{marker}")
        if result.passed:
            passed += 1
        else:
            failed += 1

    print("-" * 65)
    print(f"\n{passed}/{passed + failed} tests passed")
    if failed == 0:
        print("All tests passed.")
    else:
        print(f"{failed} test(s) failed — see marked rows above.")


if __name__ == "__main__":
    main()
