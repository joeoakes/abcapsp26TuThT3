"""
Plot per-prefill learning curves from a sweep_prefill.py results.json.

For each prefill value, this plots success-rate-on-held-out-mazes vs. training
episode, using the per-checkpoint re-scores. Also overlays the late-training
mean ± std band so it's visually obvious when prefill differences sit inside
training noise.

Usage:
    python -m dagger.plot_sweep
    python -m dagger.plot_sweep --results sweeps/prefill/results.json
    python -m dagger.plot_sweep --out sweeps/prefill/curves.png --show
"""

import argparse
import json
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


def parse_args():
    p = argparse.ArgumentParser(description="Plot DAgger prefill sweep curves")
    p.add_argument("--results", type=str, default="sweeps/prefill/results.json", help="Path to results.json written by dagger.sweep_prefill")
    p.add_argument("--out", type=str, default=None, help="Output PNG path. Defaults to <results_dir>/curves.png")
    p.add_argument("--show", action="store_true", help="Also display the plot window (requires a display)")
    return p.parse_args()


def dedupe_by_episode(points: list[dict]) -> list[dict]:
    """Collapse (episode, success) pairs by episode, keeping the first entry.

    `dagger_final.pt` is a copy of the last `dagger_ep<N>.pt`, so it duplicates
    the last episode when re-scored. We drop the duplicate to avoid a
    zero-length jag at the tail of each curve.
    """
    seen: dict[int, dict] = {}
    for pt in points:
        ep = int(pt["episode"])
        if ep not in seen:
            seen[ep] = pt
    return sorted(seen.values(), key=lambda p: p["episode"])


def main():
    args = parse_args()
    results_path = Path(args.results)
    with open(results_path) as f:
        data = json.load(f)

    rows = data["rows"]
    if not rows:
        raise SystemExit(f"No rows in {results_path}")

    out_path = Path(args.out) if args.out else results_path.parent / "curves.png"

    fig, (ax_curve, ax_bar) = plt.subplots(
        1, 2, figsize=(13, 5), gridspec_kw={"width_ratios": [2.2, 1.0]}
    )

    cmap = plt.get_cmap("viridis")
    n = len(rows)
    colors = [cmap(i / max(n - 1, 1)) for i in range(n)]

    # Left: learning curves
    for row, color in zip(rows, colors):
        prefill = row["prefill"]
        pts = dedupe_by_episode(row["per_checkpoint"])
        xs = [p["episode"] for p in pts]
        ys = [p["success_rate"] for p in pts]
        ax_curve.plot(xs, ys, marker="o", color=color, label=f"prefill={prefill}")

        # Mark the re-scored best with a star.
        best = row["true_best_success"]
        best_ep = row["true_best_episode"]
        ax_curve.plot(best_ep, best, marker="*", color=color, markersize=14, markeredgecolor="black", markeredgewidth=0.6, zorder=5)

    ax_curve.set_xlabel("Training episode")
    ax_curve.set_ylabel(f"Held-out success rate (%) on {data['args']['eval_mazes_final']} mazes")
    ax_curve.set_title("DAgger learning curves by prefill size")
    ax_curve.grid(True, alpha=0.3)
    ax_curve.legend(loc="lower right", fontsize=9)
    ax_curve.set_ylim(top=100)

    # Right: late-training mean ± std per prefill (visualizes the noise floor)
    prefills = [r["prefill"] for r in rows]
    means = [r["late_mean_success"] for r in rows]
    stds = [r["late_std_success"] for r in rows]
    bests = [r["true_best_success"] for r in rows]

    x = np.arange(len(prefills))
    ax_bar.errorbar(x, means, yerr=stds, fmt="o", color="tab:blue", capsize=5, label="late-training mean ± std", markersize=8, linewidth=1.5)
    ax_bar.scatter(x, bests, marker="*", color="tab:orange", s=140, edgecolors="black", linewidths=0.6, zorder=5, label="true best (re-scored)")

    ax_bar.set_xticks(x)
    ax_bar.set_xticklabels([str(p) for p in prefills])
    ax_bar.set_xlabel("prefill episodes")
    ax_bar.set_ylabel("Held-out success rate (%)")
    ax_bar.set_title("Late-training noise vs. best")
    ax_bar.grid(True, alpha=0.3, axis="y")
    ax_bar.legend(loc="lower right", fontsize=9)

    fig.suptitle(
        f"Prefill sweep  |  {data['args']['episodes']} eps, "
        f"{data['args']['width']}x{data['args']['height']} mazes, "
        f"eval on {data['args']['eval_mazes_final']} held-out mazes "
        f"(seed offset {data['args']['eval_seed_offset']})",
        fontsize=10,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.95))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=150)
    print(f"Saved {out_path}")

    if args.show:
        plt.show()


if __name__ == "__main__":
    main()
