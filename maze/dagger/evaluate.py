"""
Evaluate a trained DAgger agent on held-out mazes

Usage:
    python -m dagger.evaluate --model checkpoints/dagger_best.pt
    python -m dagger.evaluate --model checkpoints/dagger_best.pt --num-mazes 200 --render
"""

import argparse
import time

import numpy as np
import torch

from .maze_env import MazeEnv
from .agent import DAggerAgent


def parse_args():
    p = argparse.ArgumentParser(description="Evaluate DAgger maze agent")
    p.add_argument("--model", type=str, required=True, help="Path to .pt checkpoint")

    # Environment (must match training)
    p.add_argument("--width", type=int, default=21)
    p.add_argument("--height", type=int, default=15)
    p.add_argument("--patch-radius", type=int, default=3)
    p.add_argument("--max-steps", type=int, default=500)
    p.add_argument("--observation-mode", type=str, default="global", choices=["local", "global"])
    p.add_argument("--hidden", type=int, default=256)

    # Evaluation
    p.add_argument("--num-mazes", type=int, default=1000, help="Number of test mazes")
    p.add_argument("--seed-offset", type=int, default=1_000_000, help="Seed offset so test mazes don't overlap with training")
    p.add_argument("--render", action="store_true", help="Print ASCII maze for first few episodes")
    p.add_argument("--render-count", type=int, default=3)
    p.add_argument("--epsilon", type=float, default=0.0, help="Exploration rate for epsilon-greedy eval (0.0 = fully greedy)")
    return p.parse_args()


def evaluate(args):
    device = "cuda" if torch.cuda.is_available() else "cpu"

    env = MazeEnv(
        width=args.width,
        height=args.height,
        patch_radius=args.patch_radius,
        max_steps=args.max_steps,
        observation_mode=args.observation_mode,
    )

    agent = DAggerAgent(
        obs_shape=env.observation_space_shape,
        n_actions=env.action_space_n,
        hidden=args.hidden,
        device=device,
    )
    agent.load(args.model)
    agent.epsilon = args.epsilon
    print(f"Loaded model from {args.model}")
    print(f"  Device: {device}")
    print(f"  Episodes trained: {agent.episodes_done}")
    print(f"  Train steps: {agent.train_steps}")
    print(f"  Eval epsilon: {agent.epsilon:.4f}")
    print()

    start_time = time.perf_counter()
    successes = []
    path_lengths = []
    optimal_lengths = []
    efficiencies = []
    failure_reasons = []

    for i in range(args.num_mazes):
        seed = args.seed_offset + i
        obs = env.reset(seed=seed)
        done = False
        trajectory = [(env.agent_x, env.agent_y)]

        while not done:
            action = agent.select_action(obs, greedy=(agent.epsilon == 0.0))
            obs, done, info = env.step(action)
            trajectory.append((env.agent_x, env.agent_y))

        optimal = env.optimal_path_length()
        success = info["success"]
        steps = info["steps"]

        successes.append(success)
        path_lengths.append(steps)
        optimal_lengths.append(optimal)

        if success and optimal > 0:
            efficiencies.append(steps / optimal)
        else:
            efficiencies.append(float("inf"))

        if not success:
            # Classify failure
            unique_cells = len(set(trajectory))
            total_cells = len(trajectory)
            if total_cells > 0 and unique_cells / total_cells < 0.3:
                reason = "looping"
            elif steps >= args.max_steps:
                reason = "timeout"
            else:
                reason = "stuck"
            failure_reasons.append(reason)

        # Render a few
        if args.render and i < args.render_count:
            status = "SUCCESS" if success else "FAIL"
            print(f"--- Maze {i+1} (seed={seed}) [{status}] ---")
            print(f"  Steps: {steps}, Optimal: {optimal}, "
                  f"Efficiency: {steps/optimal:.2f}x" if optimal > 0 else "")
            print(env.render_ascii())
            print()

    # Summary stats
    succ_rate = np.mean(successes) * 100
    avg_len = np.mean(path_lengths)
    finite_eff = [e for e in efficiencies if e != float("inf")]
    avg_eff = np.mean(finite_eff) if finite_eff else float("inf")

    print("=" * 60)
    print(f"EVALUATION RESULTS ({args.num_mazes} test mazes)")
    print("=" * 60)
    print(f"  Epsilon:             {args.epsilon}")
    print(f"  Success rate:        {succ_rate:.1f}%")
    print(f"  Avg path length:     {avg_len:.1f}")
    print(f"  Avg path efficiency (successes only): {avg_eff:.2f}x optimal")
    print()

    if failure_reasons:
        from collections import Counter
        counts = Counter(failure_reasons)
        total_fail = len(failure_reasons)
        print("  Failure analysis:")
        for reason, count in counts.most_common():
            print(f"    {reason:10s}: {count:4d} ({count/total_fail*100:.1f}%)")
    else:
        print("  No failures!")

    elapsed = time.perf_counter() - start_time
    rate = args.num_mazes / elapsed if elapsed > 0 else float("inf")
    print()
    print(f"  Total eval time:     {elapsed:.2f}s")
    print(f"  Throughput:          {rate:.2f} mazes/sec")

    env.close()


def main():
    args = parse_args()
    evaluate(args)


if __name__ == "__main__":
    main()
