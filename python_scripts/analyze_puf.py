"""
Analyze CHOICE PUF evaluation data from CSV files produced by
collect_uniformity.py and collect_ber.py.

Computes and prints:
  - Uniformity score (fraction of 1s per bit, deviation from 50%)
  - Bit independence (Pearson correlation between hi and lo halves)
  - Reliability / BER (bit flip rate vs. majority-vote reference)
  - Majority-voting sweep (simulated BER at vote counts 1, 3, 5, 7, 10)

With --plot, saves figures to PNG files alongside the CSVs.

Usage:
    python analyze_puf.py --uniformity uniformity.csv
    python analyze_puf.py --ber ber.csv
    python analyze_puf.py --uniformity uniformity.csv --ber ber.csv --plot
"""

import argparse
import csv
import math
import sys
import random
from collections import defaultdict
from pathlib import Path


# ---------------------------------------------------------------------------
# CSV loading
# ---------------------------------------------------------------------------

def load_uniformity(path: str) -> list[int]:
    """Return list of 64-bit PUF responses from uniformity CSV."""
    responses = []
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            responses.append(int(row['response']))
    return responses


def load_ber(path: str) -> dict[int, list[int]]:
    """Return {challenge_idx: [response, ...]} from BER CSV."""
    per_challenge: dict[int, list[int]] = defaultdict(list)
    with open(path, newline='') as f:
        reader = csv.DictReader(f)
        for row in reader:
            per_challenge[int(row['challenge_idx'])].append(int(row['response']))
    return dict(per_challenge)


# ---------------------------------------------------------------------------
# Core math helpers
# ---------------------------------------------------------------------------

def bit_at(value: int, b: int) -> int:
    return (value >> b) & 1



def pearson_corr(xs: list[float], ys: list[float]) -> float:
    n = len(xs)
    if n < 2:
        return 0.0
    mx = sum(xs) / n
    my = sum(ys) / n
    cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sx  = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy  = math.sqrt(sum((y - my) ** 2 for y in ys))
    if sx == 0 or sy == 0:
        return 0.0
    return cov / (sx * sy)


# ---------------------------------------------------------------------------
# Uniformity analysis
# ---------------------------------------------------------------------------

def analyze_uniformity(responses: list[int], plot: bool, out_dir: Path) -> None:
    n = len(responses)
    if n == 0:
        print("Uniformity: no data.")
        return

    # Per-bit uniformity: for each of the 64 bit positions, count how many
    # responses have a 1 there, then express as a percentage.
    bit_ones = [sum(bit_at(r, b) for r in responses) for b in range(64)]
    bit_pct  = [100.0 * ones / n for ones in bit_ones]

    total_ones  = sum(bit_ones)
    overall_pct = 100.0 * total_ones / (n * 64)

    deviations = [abs(p - 50.0) for p in bit_pct]
    mean_dev   = sum(deviations) / len(deviations)
    worst_bit  = max(range(64), key=lambda b: deviations[b])

    # Per-challenge uniformity: for each response, count its 1 bits directly.
    ch_ones  = [bin(r).count('1') for r in responses]
    ch_pct   = [100.0 * ones / 64 for ones in ch_ones]

    ch_mean  = sum(ch_pct) / n
    ch_mad   = sum(abs(p - 50.0) for p in ch_pct) / n
    ch_std   = (sum((p - ch_mean) ** 2 for p in ch_pct) / n) ** 0.5
    ch_min   = min(ch_pct)
    ch_max   = max(ch_pct)
    ch_worst = max(range(n), key=lambda i: abs(ch_pct[i] - 50.0))

    print("=" * 60)
    print("UNIFORMITY ANALYSIS")
    print("=" * 60)
    print(f"  Valid challenges measured : {n}")
    print(f"  Total bits analysed       : {n * 64}")
    print()
    print("  Per-bit uniformity (bit-aliasing across all challenges):")
    print(f"    Overall fraction of 1s  : {overall_pct:.2f}%  (ideal 50.00%)")
    print(f"    Mean per-bit deviation  : {mean_dev:.2f} pp from 50%")
    print(f"    Worst bit               : bit[{worst_bit}] = {bit_pct[worst_bit]:.1f}%")
    print()
    print("  Per-challenge uniformity (fraction of 1s within each response):")
    print(f"    Mean                    : {ch_mean:.2f}%  (ideal 50.00%)")
    print(f"    MAD                     : {ch_mad:.2f} pp  (ideal 0)")
    print(f"    Std dev                 : {ch_std:.2f} pp")
    print(f"    Min / Max               : {ch_min:.2f}% / {ch_max:.2f}%")
    print(f"    Worst challenge         : index {ch_worst} = {ch_pct[ch_worst]:.1f}%  "
          f"({bin(responses[ch_worst]).count('1')}/64 ones)")
    print()

    # Bit independence: Pearson correlation between hi half (bits 32-63) and
    # lo half (bits 0-31) treated as 32-element bit vectors across challenges.
    # A high correlation between bit i (hi) and bit j (lo) would indicate
    # structural dependence in the CHOICE PUF architecture.
    hi_bits = [[bit_at(r, 32 + b) for r in responses] for b in range(32)]
    lo_bits = [[bit_at(r, b)      for r in responses] for b in range(32)]

    corr_matrix = [
        [pearson_corr(hi_bits[i], lo_bits[j]) for j in range(32)]
        for i in range(32)
    ]
    all_corrs = [corr_matrix[i][j] for i in range(32) for j in range(32)]
    mean_abs_corr = sum(abs(c) for c in all_corrs) / len(all_corrs)
    max_corr      = max(abs(c) for c in all_corrs)
    hi_lo_diag    = [corr_matrix[b][b] for b in range(32)]  # bit b_hi vs bit b_lo

    print("BIT INDEPENDENCE (hi half vs lo half)")
    print(f"  Mean |correlation|         : {mean_abs_corr:.4f}  (ideal 0)")
    print(f"  Max  |correlation|         : {max_corr:.4f}")
    print(f"  Mean diagonal correlation  : "
          f"{sum(abs(c) for c in hi_lo_diag)/32:.4f}  (bit b_hi vs bit b_lo)")
    print()

    # Uniqueness: treat hi half (bits 32-63) and lo half (bits 0-31) as two
    # virtual devices.  For each challenge, compute the Hamming distance between
    # the two 32-bit halves, expressed as a fraction of 32 bits.  The ideal is
    # 50%, meaning the two halves agree on exactly half the bits by chance.
    hi_halves = [(r >> 32) & 0xFFFFFFFF for r in responses]
    lo_halves = [r         & 0xFFFFFFFF for r in responses]

    hd_per_challenge = [
        bin(hi ^ lo).count('1') / 32.0
        for hi, lo in zip(hi_halves, lo_halves)
    ]
    uniqueness_mean = 100.0 * sum(hd_per_challenge) / n
    uniqueness_mad  = 100.0 * sum(abs(d - 0.5) for d in hd_per_challenge) / n
    uniqueness_std  = 100.0 * (
        sum((d - uniqueness_mean / 100.0) ** 2 for d in hd_per_challenge) / n
    ) ** 0.5
    uniqueness_min  = 100.0 * min(hd_per_challenge)
    uniqueness_max  = 100.0 * max(hd_per_challenge)

    print("UNIQUENESS (hi half vs lo half as proxy for two devices)")
    print(f"  Mean inter-half HD         : {uniqueness_mean:.2f}%  (ideal 50.00%)")
    print(f"  MAD                        : {uniqueness_mad:.2f} pp  (ideal 0)")
    print(f"  Std dev                    : {uniqueness_std:.2f} pp")
    print(f"  Min / Max                  : {uniqueness_min:.2f}% / {uniqueness_max:.2f}%")
    print(f"  NOTE: hi/lo halves are structurally correlated (mean |r|={mean_abs_corr:.2f});")
    print(f"        result is a lower bound, not a true inter-device uniqueness score.")
    print()

    if not plot:
        print("(Re-run with --plot to save figures.)")
        return

    try:
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("matplotlib / numpy not installed — skipping plots.")
        return

    # Figure 1: per-bit fraction of 1s
    fig, ax = plt.subplots(figsize=(8, 4))
    colors = ['tomato' if abs(p - 50) > 10 else 'steelblue' for p in bit_pct]
    ax.bar(range(64), bit_pct, color=colors, width=0.8)
    ax.axhline(50, color='black', linewidth=1, linestyle='--', label='Ideal 50%')
    ax.axhline(50 + 10, color='gray', linewidth=0.7, linestyle=':')
    ax.axhline(50 - 10, color='gray', linewidth=0.7, linestyle=':')
    ax.set_xlabel('Bit position')
    ax.set_ylabel('Uniformity (percentage of 1s)')
    ax.set_title('CHOICE PUF — Per-bit Uniformity\n'
                 f'(n={n} challenges, overall mean={overall_pct:.2f}%)')
    ax.set_xlim(-0.5, 63.5)
    ax.set_ylim(0, 100)
    ax.legend()
    fig.tight_layout()
    out_path = out_dir / 'plot_uniformity_bits.png'
    fig.savefig(out_path, dpi=150)
    print(f"  Saved: {out_path}")
    plt.close(fig)

    # Figure 2: per-challenge uniformity sorted bar chart
    sorted_pct = sorted(ch_pct)
    colors = ['tomato' if abs(p - 50) > 10 else 'steelblue' for p in sorted_pct]
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.bar(range(n), sorted_pct, color=colors, width=1.0)
    ax.axhline(50, color='black', linewidth=1, linestyle='--', label='Ideal 50%')
    ax.axhline(50 + 10, color='gray', linewidth=0.7, linestyle=':')
    ax.axhline(50 - 10, color='gray', linewidth=0.7, linestyle=':')
    ax.axhline(ch_mean, color='tomato', linewidth=1.2, linestyle='-',
               label=f'Mean = {ch_mean:.1f}%')
    ax.set_xlabel('Challenge (sorted by uniformity)')
    ax.set_ylabel('Uniformity (percentage of 1s)')
    ax.set_title('CHOICE PUF — Per-challenge Uniformity (sorted)\n'
                 f'(n={n} challenges, mean={ch_mean:.1f}%, std={ch_std:.1f} pp)')
    ax.set_xlim(-0.5, n - 0.5)
    ax.set_ylim(0, 100)
    ax.legend()
    fig.tight_layout()
    out_path = out_dir / 'plot_uniformity_per_challenge.png'
    fig.savefig(out_path, dpi=150)
    print(f"  Saved: {out_path}")
    plt.close(fig)

    # Figure 3: uniqueness — inter-half Hamming distance per challenge (sorted)
    sorted_hd = sorted(hd_per_challenge)
    colors = ['steelblue' if abs(d * 100 - 50) <= 10 else 'tomato' for d in sorted_hd]
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.bar(range(n), [d * 100 for d in sorted_hd], color=colors, width=1.0)
    ax.axhline(50, color='black', linewidth=1, linestyle='--', label='Ideal 50%')
    ax.axhline(50 + 10, color='gray', linewidth=0.7, linestyle=':')
    ax.axhline(50 - 10, color='gray', linewidth=0.7, linestyle=':')
    ax.axhline(uniqueness_mean, color='tomato', linewidth=1.2, linestyle='-',
               label=f'Mean = {uniqueness_mean:.1f}%')
    ax.set_xlabel('Challenge (sorted by inter-half Hamming distance)')
    ax.set_ylabel('Inter-device HD (%)')
    ax.set_title('CHOICE PUF — Uniqueness (hi vs lo, sorted)\n'
                 f'(n={n} challenges, mean={uniqueness_mean:.1f}%, std={uniqueness_std:.1f}%)')
    ax.set_xlim(-0.5, n - 0.5)
    ax.set_ylim(0, 100)
    ax.legend()
    fig.tight_layout()
    out_path = out_dir / 'plot_uniqueness.png'
    fig.savefig(out_path, dpi=150)
    print(f"  Saved: {out_path}")
    plt.close(fig)

    # Figure 4: correlation heatmap
    corr_arr = np.array(corr_matrix)
    fig, ax = plt.subplots(figsize=(8, 7))
    im = ax.imshow(corr_arr, vmin=-1, vmax=1, cmap='RdBu_r', aspect='equal')
    fig.colorbar(im, ax=ax, label='Pearson r')
    ax.set_xlabel('lo-half bit index (bits 0–31)')
    ax.set_ylabel('hi-half bit index (bits 32–63)')
    ax.set_title('Bit Independence — Pearson Correlation (hi vs lo half)\n'
                 f'(mean |r|={mean_abs_corr:.4f}, ideal 0)')
    fig.tight_layout()
    out_path = out_dir / 'plot_bit_independence.png'
    fig.savefig(out_path, dpi=150)
    print(f"  Saved: {out_path}")
    plt.close(fig)


# ---------------------------------------------------------------------------
# BER / majority-voting analysis
# ---------------------------------------------------------------------------

def analyze_ber(per_challenge: dict[int, list[int]], plot: bool, out_dir: Path) -> None:
    n_challenges = len(per_challenge)
    if n_challenges == 0:
        print("BER: no data.")
        return

    import numpy as np
    from pypuf.metrics import reliability_data

    # Build response array of shape (N, m, r) in {-1, +1} encoding.
    # Truncate all challenges to the minimum sample count so the array is rectangular.
    challenge_list = sorted(per_challenge.keys())
    r = min(len(per_challenge[c]) for c in challenge_list)
    n = len(challenge_list)

    raw = np.array([
        [[(per_challenge[cidx][ri] >> b) & 1 for ri in range(r)]
         for b in range(64)]
        for cidx in challenge_list
    ], dtype=np.int8)           # shape (N, 64, r)
    bipolar = 1 - 2 * raw       # {0,1} -> {+1,-1}

    rel = reliability_data(bipolar)             # shape (N, 64)
    per_bit_rel   = rel.mean(axis=0)            # (64,) — averaged over challenges
    per_ch_rel    = rel.mean(axis=1)            # (N,)  — averaged over bits
    overall_rel   = float(rel.mean())

    print("=" * 60)
    print("RELIABILITY ANALYSIS  (pypuf, reference-free)")
    print("=" * 60)
    print(f"  Challenges            : {n}")
    print(f"  Samples per challenge : {r}")
    print(f"  Overall reliability   : {overall_rel*100:.4f}%")
    worst_bit = int(per_bit_rel.argmin())
    print(f"  Least reliable bit    : bit[{worst_bit}] = {per_bit_rel[worst_bit]*100:.4f}%")
    worst_ch  = int(per_ch_rel.argmin())
    print(f"  Least reliable challenge : index {challenge_list[worst_ch]} = "
          f"{per_ch_rel[worst_ch]*100:.4f}%")
    print()

    if not plot:
        print("(Re-run with --plot to save figures.)")
        return

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed — skipping plots.")
        return

    # Figure 1: per-bit reliability
    rel_bit_pct = per_bit_rel * 100
    colors = ['tomato' if v < 99.0 else 'steelblue' for v in rel_bit_pct]
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.bar(range(64), rel_bit_pct, color=colors, width=0.8)
    ax.axhline(overall_rel * 100, color='black', linewidth=1, linestyle='--',
               label=f'Mean = {overall_rel*100:.4f}%')
    ax.axhline(99.0, color='green', linewidth=1, linestyle=':',
               label='99% threshold')
    ax.set_xlabel('Bit position')
    ax.set_ylabel('Reliability (%)')
    ax.set_title('CHOICE PUF — Per-bit Reliability\n'
                 f'({n} challenges, {r} samples each)')
    ax.set_xlim(-0.5, 63.5)
    ax.set_ylim(min(95.0, float(rel_bit_pct.min()) - 1), 100)
    ax.legend()
    fig.tight_layout()
    out_path = out_dir / 'plot_reliability_per_bit.png'
    fig.savefig(out_path, dpi=150)
    print(f"  Saved: {out_path}")
    plt.close(fig)

    # Figure 2: per-challenge reliability (sorted)
    sorted_ch_rel = np.sort(per_ch_rel) * 100
    colors = ['tomato' if v < 99.0 else 'steelblue' for v in sorted_ch_rel]
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.bar(range(n), sorted_ch_rel, color=colors, width=1.0)
    ax.axhline(overall_rel * 100, color='black', linewidth=1, linestyle='--',
               label=f'Mean = {overall_rel*100:.4f}%')
    ax.axhline(99.0, color='green', linewidth=1, linestyle=':',
               label='99% threshold')
    ax.set_xlabel('Challenge (sorted by reliability)')
    ax.set_ylabel('Reliability (%)')
    ax.set_title('CHOICE PUF — Per-challenge Reliability (sorted)\n'
                 f'({n} challenges, {r} samples each)')
    ax.set_xlim(-0.5, n - 0.5)
    ax.set_ylim(min(95.0, float(sorted_ch_rel.min()) - 1), 100)
    ax.legend()
    fig.tight_layout()
    out_path = out_dir / 'plot_reliability_per_challenge.png'
    fig.savefig(out_path, dpi=150)
    print(f"  Saved: {out_path}")
    plt.close(fig)




# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Analyze CHOICE PUF evaluation CSV data"
    )
    parser.add_argument("--uniformity", metavar="CSV",
                        help="uniformity.csv from collect_uniformity.py")
    parser.add_argument("--ber", metavar="CSV",
                        help="ber.csv from collect_ber.py")
    parser.add_argument("--plot", action="store_true",
                        help="Generate and save PNG plots (requires matplotlib)")
    parser.add_argument("--seed", type=int, default=0,
                        help="RNG seed for voting Monte Carlo (default: 0)")
    args = parser.parse_args()

    if not args.uniformity and not args.ber:
        parser.print_help()
        sys.exit(1)

    random.seed(args.seed)

    if args.uniformity:
        out_dir = Path(args.uniformity).parent
        print(f"Loading uniformity data from '{args.uniformity}'...")
        responses = load_uniformity(args.uniformity)
        print(f"  {len(responses)} challenge responses loaded")
        analyze_uniformity(responses, args.plot, out_dir)

    if args.ber:
        out_dir = Path(args.ber).parent
        print(f"Loading BER data from '{args.ber}'...")
        per_challenge = load_ber(args.ber)
        total_samples = sum(len(v) for v in per_challenge.values())
        print(f"  {len(per_challenge)} challenges, {total_samples} total samples loaded")
        analyze_ber(per_challenge, args.plot, out_dir)


if __name__ == "__main__":
    main()
