"""
DAgger training pipeline for maze navigation

Usage:
    python -m dagger.train
    python -m dagger.train --episodes 5000 --width 21 --height 15
    python -m dagger.train --resume checkpoints/dagger_latest.pt
"""

import argparse
import time
from pathlib import Path

import numpy as np

from .maze_env import MazeEnv
from .agent import DAggerAgent


def parse_args():
    p = argparse.ArgumentParser(description="Train DAgger on maze navigation")

    # Environment
    p.add_argument("--width", type=int, default=21)
    p.add_argument("--height", type=int, default=15)
    p.add_argument("--patch-radius", type=int, default=3)
    p.add_argument("--max-steps", type=int, default=500)
    p.add_argument("--observation-mode", type=str, default="global", choices=["local", "global"])

    # Agent hyperparameters
    p.add_argument("--lr", type=float, default=1e-3)
    p.add_argument("--epsilon-start", type=float, default=1.0)
    p.add_argument("--epsilon-end", type=float, default=0.05)
    p.add_argument("--epsilon-decay", type=float, default=0.999)
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--expert-buffer-size", type=int, default=200_000)
    p.add_argument("--hidden", type=int, default=256)

    # Training
    p.add_argument("--episodes", type=int, default=5000)
    p.add_argument("--train-freq", type=int, default=4, help="Train every N steps")
    p.add_argument("--log-interval", type=int, default=50, help="Print stats every N episodes")
    p.add_argument("--save-interval", type=int, default=500, help="Checkpoint every N episodes")
    p.add_argument("--eval-interval", type=int, default=250, help="Run greedy validation every N episodes (0 disables)")
    p.add_argument("--eval-mazes", type=int, default=100, help="Number of held-out mazes for greedy validation")
    p.add_argument("--eval-seed-offset", type=int, default=1_000_000, help="Seed offset for held-out validation mazes")
    p.add_argument("--expert-prefill-episodes", type=int, default=200, help="Number of shortest-path expert episodes to seed expert buffer")
    p.add_argument("--bc-pretrain-steps", type=int, default=5000, help="Number of BC gradient steps before DAgger rollouts")
    p.add_argument("--post-bc-epsilon", type=float, default=0.10, help="Exploration rate after initial BC pretraining")
    p.add_argument("--checkpoint-dir", type=str, default="checkpoints")
    p.add_argument("--resume", type=str, default=None, help="Path to checkpoint to resume from")

    return p.parse_args()


def evaluate_greedy(agent: DAggerAgent, args) -> tuple[float, float]:
    env = MazeEnv(
        width=args.width,
        height=args.height,
        patch_radius=args.patch_radius,
        max_steps=args.max_steps,
        observation_mode=args.observation_mode,
    )

    successes = []
    efficiencies = []

    for i in range(args.eval_mazes):
        obs = env.reset(seed=args.eval_seed_offset + i)
        done = False
        while not done:
            action = agent.select_action(obs, greedy=True)
            obs, done, info = env.step(action)

        optimal = env.optimal_path_length()
        success = bool(info["success"])
        successes.append(float(success))
        if success and optimal > 0:
            efficiencies.append(info["steps"] / optimal)

    env.close()
    success_rate = 100.0 * np.mean(successes) if successes else 0.0
    avg_eff = np.mean(efficiencies) if efficiencies else float("inf")
    return success_rate, avg_eff


def prefill_expert_buffer(agent: DAggerAgent, args) -> int:
    if args.expert_prefill_episodes <= 0:
        return 0

    env = MazeEnv(
        width=args.width,
        height=args.height,
        patch_radius=args.patch_radius,
        max_steps=args.max_steps,
        observation_mode=args.observation_mode,
    )

    total_labels = 0
    for ep in range(args.expert_prefill_episodes):
        obs = env.reset(seed=ep + 1)
        done = False

        while not done:
            action = env.optimal_action()
            if action is None:
                break
            agent.store_expert_label(obs, action)
            obs, done, _ = env.step(action)
            total_labels += 1

    env.close()
    return total_labels


def pretrain_bc(agent: DAggerAgent, args):
    if args.bc_pretrain_steps <= 0 or len(agent.expert_buffer) == 0:
        return

    losses = []
    for _ in range(args.bc_pretrain_steps):
        loss = agent.train_step()
        if loss is not None:
            losses.append(loss)

    if losses:
        print(
            f"BC pretrain: {len(losses)} steps, "
            f"final loss={losses[-1]:.4f}, avg loss={np.mean(losses):.4f}"
        )
        agent.epsilon = min(agent.epsilon, args.post_bc_epsilon)


def train(args):
    env = MazeEnv(
        width=args.width,
        height=args.height,
        patch_radius=args.patch_radius,
        max_steps=args.max_steps,
        observation_mode=args.observation_mode,
    )

    device = "cuda" if __import__("torch").cuda.is_available() else "cpu"
    print(f"Device: {device}")

    agent = DAggerAgent(
        obs_shape=env.observation_space_shape,
        n_actions=env.action_space_n,
        lr=args.lr,
        epsilon_start=args.epsilon_start,
        epsilon_end=args.epsilon_end,
        epsilon_decay=args.epsilon_decay,
        batch_size=args.batch_size,
        expert_buffer_capacity=args.expert_buffer_size,
        hidden=args.hidden,
        device=device,
    )

    if args.resume:
        agent.load(args.resume)
        print(f"Resumed from {args.resume} (episode {agent.episodes_done}, "
              f"epsilon={agent.epsilon:.4f})")

    prefill_count = prefill_expert_buffer(agent, args)
    if prefill_count > 0:
        print(
            f"Prefilled expert buffer with {prefill_count} labels "
            f"from {args.expert_prefill_episodes} shortest-path episodes"
        )
    if not args.resume:
        pretrain_bc(agent, args)

    ckpt_dir = Path(args.checkpoint_dir)
    ckpt_dir.mkdir(parents=True, exist_ok=True)

    # Tracking
    ep_lengths = []
    ep_successes = []
    ep_efficiencies = []
    total_steps = 0
    best_eval_success = -1.0
    t0 = time.time()

    for ep in range(1, args.episodes + 1):
        obs = env.reset()
        done = False

        while not done:
            expert_action = env.optimal_action()
            if expert_action is not None:
                agent.store_expert_label(obs, expert_action)
            action = agent.select_action(obs)
            obs, done, info = env.step(action)
            total_steps += 1

            if len(agent.expert_buffer) >= agent.batch_size and total_steps % args.train_freq == 0:
                agent.train_step()

        agent.end_episode()

        # Logging
        ep_lengths.append(info["steps"])
        ep_successes.append(float(info["success"]))
        optimal = env.optimal_path_length()
        if info["success"] and optimal > 0:
            ep_efficiencies.append(info["steps"] / optimal)
        else:
            ep_efficiencies.append(float("inf"))

        if ep % args.log_interval == 0:
            window = min(args.log_interval, len(ep_lengths))
            avg_len = np.mean(ep_lengths[-window:])
            avg_succ = np.mean(ep_successes[-window:]) * 100
            finite_eff = [e for e in ep_efficiencies[-window:] if e != float("inf")]
            avg_eff = np.mean(finite_eff) if finite_eff else float("inf")
            elapsed = time.time() - t0

            print(
                f"[Ep {ep:5d}] "
                f"len={avg_len:5.0f}  "
                f"succ={avg_succ:5.1f}%  "
                f"eff_succ={avg_eff:5.2f}x  "
                f"eps={agent.epsilon:.3f}  "
                f"ebuf={len(agent.expert_buffer):6d}  "
                f"time={elapsed:.0f}s"
            )

        if args.eval_interval > 0 and ep % args.eval_interval == 0:
            eval_success, eval_eff = evaluate_greedy(agent, args)
            print(
                f"  → Eval: succ={eval_success:5.1f}%  "
                f"eff_succ={eval_eff:5.2f}x"
            )
            if eval_success > best_eval_success:
                best_eval_success = eval_success
                best_path = ckpt_dir / "dagger_best.pt"
                agent.save(str(best_path))
                print(f"  → Saved best checkpoint: {best_path}")

        if ep % args.save_interval == 0:
            path = ckpt_dir / f"dagger_ep{ep}.pt"
            agent.save(str(path))
            agent.save(str(ckpt_dir / "dagger_latest.pt"))
            print(f"  → Saved checkpoint: {path}")

    # Final save
    agent.save(str(ckpt_dir / "dagger_final.pt"))
    env.close()
    print(f"\nTraining complete. {args.episodes} episodes, {total_steps} total steps.")
    print(f"Final epsilon: {agent.epsilon:.4f}")
    print(f"Model saved to {ckpt_dir / 'dagger_final.pt'}")


def main():
    args = parse_args()
    train(args)


if __name__ == "__main__":
    main()
