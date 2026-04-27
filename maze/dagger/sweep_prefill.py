"""
Sweep `--expert-prefill-episodes` to measure its effect on greedy success rate.

For each prefill size, this script:
    1. Seeds RNGs deterministically so every run sees the same stochastic stream.
    2. Runs full DAgger training into an isolated checkpoint dir.
    3. Loads the best checkpoint produced during training.
    4. Evaluates greedily (epsilon=0) on a fixed held-out seed range.
    5. Appends a row to a results table + JSON file.

Usage:
    # Full sweep at the current training budget
    python -m dagger.sweep_prefill --prefills 25 100 200 500 1000

    # Quick iterate (smaller training budget, smaller final eval)
    python -m dagger.sweep_prefill --prefills 50 200 800 \
        --episodes 1500 --eval-mazes-final 500

    # Custom output dir
    python -m dagger.sweep_prefill --prefills 100 200 400 --out sweeps/prefill
"""

import argparse
import json
import random
import time
from pathlib import Path

import numpy as np
import torch

from . import train as train_mod
from .agent import DAggerAgent
from .maze_env import MazeEnv


def parse_args():
    p = argparse.ArgumentParser(description="Sweep prefill sizes for DAgger")
    p.add_argument(
        "--prefills",
        type=int,
        nargs="+",
        default=[25, 100, 200, 500, 1000],
        help="List of --expert-prefill-episodes values to sweep",
    )
    p.add_argument("--out", type=str, default="sweeps/prefill", help="Output directory for per-run checkpoints + results.json")
    p.add_argument("--seed", type=int, default=0, help="Base RNG seed; reapplied before each run for a fair comparison")

    # Training budget (forwarded to dagger.train). Defaults match train.py but
    # are exposed here so users can shrink for a fast sweep.
    p.add_argument("--episodes", type=int, default=5000)
    p.add_argument("--width", type=int, default=21)
    p.add_argument("--height", type=int, default=15)
    p.add_argument("--patch-radius", type=int, default=3)
    p.add_argument("--max-steps", type=int, default=500)
    p.add_argument("--observation-mode", type=str, default="global", choices=["local", "global"])
    p.add_argument("--lr", type=float, default=1e-3)
    p.add_argument("--epsilon-start", type=float, default=1.0)
    p.add_argument("--epsilon-end", type=float, default=0.05)
    p.add_argument("--epsilon-decay", type=float, default=0.999)
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--expert-buffer-size", type=int, default=200_000)
    p.add_argument("--hidden", type=int, default=256)
    p.add_argument("--train-freq", type=int, default=4)
    p.add_argument("--log-interval", type=int, default=250)
    p.add_argument("--save-interval", type=int, default=500, help="Save a dagger_ep<N>.pt checkpoint every N episodes (sweep re-scores all of them on the large held-out set)")
    p.add_argument("--eval-interval", type=int, default=250)
    p.add_argument("--eval-mazes", type=int, default=500, help="Mazes for the per-eval validation inside training. Default raised vs. train.py so in-training 'best' selection is less noisy.")
    p.add_argument("--eval-seed-offset", type=int, default=1_000_000)
    p.add_argument("--bc-pretrain-steps", type=int, default=5000)
    p.add_argument("--post-bc-epsilon", type=float, default=0.10)

    # Final held-out evaluation (bigger than the per-eval validation)
    p.add_argument("--eval-mazes-final", type=int, default=1000, help="Held-out mazes used to score each run after training")
    p.add_argument("--eval-epsilon-final", type=float, default=0.0)
    p.add_argument("--rescore-last-k", type=int, default=6, help="Re-score the last K periodic checkpoints (dagger_ep*.pt) on the large held-out set. The true best of these is reported; the mean/std quantifies late-training noise. Set to 0 to skip and only score dagger_best.pt.")
    p.add_argument("--rescore-only", action="store_true", help="Skip training. Re-score checkpoints already present in <out>/prefill_<N>/ for every value in --prefills. Useful after a noisy sweep to get a clean ranking.")

    return p.parse_args()


def set_global_seed(seed: int):
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(seed)


def build_train_args(sweep_args, prefill: int, ckpt_dir: Path) -> argparse.Namespace:
    """Create the Namespace that `train.train` expects."""
    ns = argparse.Namespace(
        width=sweep_args.width,
        height=sweep_args.height,
        patch_radius=sweep_args.patch_radius,
        max_steps=sweep_args.max_steps,
        observation_mode=sweep_args.observation_mode,
        lr=sweep_args.lr,
        epsilon_start=sweep_args.epsilon_start,
        epsilon_end=sweep_args.epsilon_end,
        epsilon_decay=sweep_args.epsilon_decay,
        batch_size=sweep_args.batch_size,
        expert_buffer_size=sweep_args.expert_buffer_size,
        hidden=sweep_args.hidden,
        episodes=sweep_args.episodes,
        train_freq=sweep_args.train_freq,
        log_interval=sweep_args.log_interval,
        save_interval=sweep_args.save_interval,
        eval_interval=sweep_args.eval_interval,
        eval_mazes=sweep_args.eval_mazes,
        eval_seed_offset=sweep_args.eval_seed_offset,
        expert_prefill_episodes=prefill,
        bc_pretrain_steps=sweep_args.bc_pretrain_steps,
        post_bc_epsilon=sweep_args.post_bc_epsilon,
        checkpoint_dir=str(ckpt_dir),
        resume=None,
    )
    return ns


def _eval_one(ckpt_path: Path, env: MazeEnv, agent: DAggerAgent, sweep_args) -> dict:
    agent.load(str(ckpt_path))
    agent.epsilon = sweep_args.eval_epsilon_final

    successes = []
    efficiencies = []
    for i in range(sweep_args.eval_mazes_final):
        obs = env.reset(seed=sweep_args.eval_seed_offset + i)
        done = False
        while not done:
            action = agent.select_action(
                obs, greedy=(sweep_args.eval_epsilon_final == 0.0)
            )
            obs, done, info = env.step(action)
        optimal = env.optimal_path_length()
        success = bool(info["success"])
        successes.append(float(success))
        if success and optimal > 0:
            efficiencies.append(info["steps"] / optimal)

    return {
        "success_rate": 100.0 * float(np.mean(successes)) if successes else 0.0,
        "avg_efficiency": float(np.mean(efficiencies)) if efficiencies else float("inf"),
        "episodes_trained": agent.episodes_done,
        "train_steps": agent.train_steps,
    }


def final_eval(ckpt_path: Path, sweep_args) -> dict:
    """Evaluate a single checkpoint greedily on the held-out seed range."""
    device = "cuda" if torch.cuda.is_available() else "cpu"
    env = MazeEnv(
        width=sweep_args.width,
        height=sweep_args.height,
        patch_radius=sweep_args.patch_radius,
        max_steps=sweep_args.max_steps,
        observation_mode=sweep_args.observation_mode,
    )
    agent = DAggerAgent(
        obs_shape=env.observation_space_shape,
        n_actions=env.action_space_n,
        hidden=sweep_args.hidden,
        device=device,
    )
    try:
        return _eval_one(ckpt_path, env, agent, sweep_args)
    finally:
        env.close()


def _episode_of(path: Path) -> int:
    """Parse the episode number from a dagger_ep<N>.pt filename, or -1."""
    name = path.stem  # e.g. 'dagger_ep3500'
    if name.startswith("dagger_ep"):
        try:
            return int(name[len("dagger_ep"):])
        except ValueError:
            return -1
    return -1


def rescore_checkpoints(ckpt_dir: Path, sweep_args) -> list[dict]:
    """
    Re-score the last K periodic checkpoints on the large held-out set.

    Returns a list of {checkpoint, episode, success_rate, avg_efficiency,
    episodes_trained, train_steps} sorted by episode ascending.
    """
    k = sweep_args.rescore_last_k
    if k <= 0:
        return []

    periodic = sorted(
        (p for p in ckpt_dir.glob("dagger_ep*.pt")),
        key=_episode_of,
    )
    # Always include dagger_final.pt so we cover the tail of training.
    final_ckpt = ckpt_dir / "dagger_final.pt"
    candidates = periodic[-k:]
    if final_ckpt.exists() and final_ckpt not in candidates:
        candidates.append(final_ckpt)

    if not candidates:
        return []

    device = "cuda" if torch.cuda.is_available() else "cpu"
    env = MazeEnv(
        width=sweep_args.width,
        height=sweep_args.height,
        patch_radius=sweep_args.patch_radius,
        max_steps=sweep_args.max_steps,
        observation_mode=sweep_args.observation_mode,
    )
    agent = DAggerAgent(
        obs_shape=env.observation_space_shape,
        n_actions=env.action_space_n,
        hidden=sweep_args.hidden,
        device=device,
    )

    scored: list[dict] = []
    try:
        for ckpt in candidates:
            stats = _eval_one(ckpt, env, agent, sweep_args)
            scored.append({
                "checkpoint": str(ckpt),
                "episode": _episode_of(ckpt) if ckpt.name != "dagger_final.pt"
                           else stats["episodes_trained"],
                **stats,
            })
            print(
                f"    {ckpt.name:20s} ep={scored[-1]['episode']:>5}  "
                f"success={stats['success_rate']:5.1f}%  "
                f"eff={stats['avg_efficiency']:.2f}x"
            )
    finally:
        env.close()

    scored.sort(key=lambda r: r["episode"])
    return scored


def print_table(rows: list[dict]):
    header = (
        f"{'prefill':>8}  {'best%':>6}  {'best_ep':>7}  "
        f"{'mean%':>6}  {'std%':>5}  {'n':>3}  "
        f"{'advisory%':>10}  {'time_s':>7}"
    )
    sep = "-" * len(header)
    print("\n" + sep)
    print(header)
    print(sep)
    for r in rows:
        print(
            f"{r['prefill']:>8}  "
            f"{r['true_best_success']:>5.1f}  "
            f"{r['true_best_episode']:>7}  "
            f"{r['late_mean_success']:>5.1f}  "
            f"{r['late_std_success']:>4.1f}  "
            f"{r['late_n']:>3}  "
            f"{r['advisory_best_success']:>9.1f}  "
            f"{r['elapsed_s']:>7.0f}"
        )
    print(sep)
    print("  best%    = best of last-K periodic checkpoints, re-scored on the held-out set")
    print("  mean/std = across those K checkpoints (your real noise floor)")
    print("  advisory = dagger_best.pt picked during training (noisy, for reference only)")


def main():
    args = parse_args()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    results_path = out_dir / "results.json"
    rows: list[dict] = []

    for prefill in args.prefills:
        run_name = f"prefill_{prefill:05d}"
        ckpt_dir = out_dir / run_name
        ckpt_dir.mkdir(parents=True, exist_ok=True)

        print("\n" + "=" * 72)
        print(f"  SWEEP RUN: expert-prefill-episodes = {prefill}")
        print(f"  Checkpoints -> {ckpt_dir}")
        print("=" * 72)

        if args.rescore_only:
            if not any(ckpt_dir.glob("dagger_*.pt")):
                print(f"  [skip] No checkpoints in {ckpt_dir}, skipping.")
                continue
            elapsed = 0.0
        else:
            set_global_seed(args.seed)
            train_args = build_train_args(args, prefill=prefill, ckpt_dir=ckpt_dir)

            t0 = time.time()
            train_mod.train(train_args)
            elapsed = time.time() - t0

        # Advisory: score the 'best' picked by the small in-training eval.
        # This is noisy and retained for reference only.
        advisory_ckpt = ckpt_dir / "dagger_best.pt"
        if not advisory_ckpt.exists():
            advisory_ckpt = ckpt_dir / "dagger_final.pt"
        print(f"\n  Advisory eval on {args.eval_mazes_final} mazes "
              f"({advisory_ckpt.name})...")
        advisory_stats = final_eval(advisory_ckpt, args)
        print(f"    -> advisory success = {advisory_stats['success_rate']:.1f}%")

        # Authoritative: re-score the last-K periodic checkpoints + final.
        print(f"  Re-scoring last {args.rescore_last_k} periodic checkpoints on {args.eval_mazes_final} mazes...")
        scored = rescore_checkpoints(ckpt_dir, args)

        if scored:
            succs = [s["success_rate"] for s in scored]
            best = max(scored, key=lambda s: s["success_rate"])
            true_best_success = best["success_rate"]
            true_best_episode = best["episode"]
            true_best_ckpt = best["checkpoint"]
            late_mean = float(np.mean(succs))
            late_std = float(np.std(succs, ddof=1)) if len(succs) > 1 else 0.0
            late_n = len(succs)
        else:
            # Fallback when rescoring is disabled.
            true_best_success = advisory_stats["success_rate"]
            true_best_episode = advisory_stats["episodes_trained"]
            true_best_ckpt = str(advisory_ckpt)
            late_mean = advisory_stats["success_rate"]
            late_std = 0.0
            late_n = 1

        row = {
            "prefill": prefill,
            "elapsed_s": elapsed,
            "true_best_success": true_best_success,
            "true_best_episode": true_best_episode,
            "true_best_checkpoint": true_best_ckpt,
            "late_mean_success": late_mean,
            "late_std_success": late_std,
            "late_n": late_n,
            "advisory_best_success": advisory_stats["success_rate"],
            "advisory_checkpoint": str(advisory_ckpt),
            "per_checkpoint": scored,
        }
        rows.append(row)

        # Persist after each run so a crash mid-sweep doesn't lose results.
        with open(results_path, "w") as f:
            json.dump({"args": vars(args), "rows": rows}, f, indent=2)

        print(
            f"  -> true best = {true_best_success:.1f}% at ep {true_best_episode}  "
            f"(late mean={late_mean:.1f}% ± {late_std:.1f}%, "
            f"advisory={advisory_stats['success_rate']:.1f}%)"
        )

    print_table(rows)
    print(f"\nResults saved to {results_path}")


if __name__ == "__main__":
    main()
