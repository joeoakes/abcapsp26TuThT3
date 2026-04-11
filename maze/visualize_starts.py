import argparse
import os

import numpy as np
import matplotlib.pyplot as plt
import seaborn as sns
import torch

from dagger.maze_env import MazeEnv
from dagger.agent import DAggerAgent

def parse_args():
    p = argparse.ArgumentParser("Visualize DAgger success rate by starting cell")
    p.add_argument("--model", type=str, default="checkpoints/dagger_best.pt")
    p.add_argument("--num-mazes", type=int, default=100)
    p.add_argument("--width", type=int, default=21)
    p.add_argument("--height", type=int, default=15)
    p.add_argument("--max-steps", type=int, default=500)
    p.add_argument("--observation-mode", type=str, default="global", choices=["local", "global"])
    p.add_argument("--hidden", type=int, default=256)
    p.add_argument("--seed-offset", type=int, default=1_000_000)
    p.add_argument("--out", type=str, default="success_heatmap.png")
    return p.parse_args()

def main():
    args = parse_args()
    device = "cuda" if torch.cuda.is_available() else "cpu"
    
    env = MazeEnv(
        width=args.width,
        height=args.height,
        max_steps=args.max_steps,
        observation_mode=args.observation_mode
    )
    
    agent = DAggerAgent(
        obs_shape=env.observation_space_shape,
        n_actions=env.action_space_n,
        hidden=args.hidden,
        device=device
    )
    
    if not os.path.exists(args.model):
        print(f"Error: Model not found at '{args.model}'")
        return
        
    print(f"Loading {args.model} on {device}...")
    agent.load(args.model)
    agent.epsilon = 0.0 # fully greedy

    success_counts = np.zeros((args.height, args.width))
    total_counts = np.zeros((args.height, args.width))

    print(f"Evaluating {args.num_mazes} held-out mazes...")
    print(f"For each maze, testing the remaining {(args.width * args.height) - 1} start cells...")
    
    for i in range(args.num_mazes):
        seed = args.seed_offset + i
        env.reset(seed=seed)
        
        for sy in range(args.height):
            for sx in range(args.width):
                if sx == env.goal_x and sy == env.goal_y:
                    continue
                
                # Manual start position override
                env.agent_x = sx
                env.agent_y = sy
                env.steps = 0
                env._visit_count.fill(0)
                env._visit_count[sy, sx] = 1
                
                obs = env._get_obs()
                done = False
                
                while not done:
                    # Select greedy action
                    action = agent.select_action(obs, greedy=True)
                    obs, done, info = env.step(action)
                    
                success_counts[sy, sx] += int(info["success"])
                total_counts[sy, sx] += 1
                
        if (i + 1) % 10 == 0 or (i + 1) == args.num_mazes:
            print(f"  Processed {i+1:3d}/{args.num_mazes} mazes")

    # Format data for heatmap
    success_rate = np.zeros((args.height, args.width))
    success_rate[:] = np.nan
    mask = total_counts > 0
    success_rate[mask] = success_counts[mask] / total_counts[mask]
    
    # Render heatmap
    plt.figure(figsize=(12, 8))
    sns.set_theme(style="white")
    
    ax = sns.heatmap(success_rate, annot=False, cmap="viridis", vmin=0, vmax=1, cbar_kws={'label': 'Success Rate'})
    
    # Mark the goal cell
    ax.add_patch(plt.Rectangle((env.goal_x, env.goal_y), 1, 1, fill=False, edgecolor='red', lw=3))
    plt.text(env.goal_x + 0.5, env.goal_y + 0.5, 'G', color='red', ha='center', va='center', fontweight='bold', fontsize=14)
    
    plt.title(f"DAgger Success Rate by Starting Cell\n(Evaluated over {args.num_mazes} Held-Out Mazes)", pad=20, fontsize=14)
    plt.xlabel("X Coordinate", labelpad=10)
    plt.ylabel("Y Coordinate", labelpad=10)
    plt.tight_layout()
    
    plt.savefig(args.out, dpi=150)
    print(f"\nSaved visualization to {os.path.abspath(args.out)}")

if __name__ == "__main__":
    main()
