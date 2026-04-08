"""Quick smoke test for the C bridge + environment + agent"""

# Test 1: Bridge
print("=== Test 1: C Bridge ===")
from dagger.maze_bridge import generate_training_instance
handle, walls, dist = generate_training_instance(21, 15, seed=42)
print(f"  Wall grid: {walls.shape} {walls.dtype}")
print(f"  Dist map:  {dist.shape} {dist.dtype}")
print(f"  Optimal distance (0,0)->goal: {dist[0, 0]}")
path = handle.astar(0, 0, 20, 14)
print(f"  A* path length: {len(path)}")
assert walls.shape == (15, 21)
assert dist.shape == (15, 21)
assert dist[0, 0] == len(path)
print("  PASS")

# Test 2: Environment
print("\n=== Test 2: MazeEnv ===")
from dagger.maze_env import MazeEnv
env = MazeEnv(width=21, height=15, seed=42)
obs = env.reset()
print(f"  Obs dim: {env.obs_dim}")
print(f"  Obs shape: {obs.shape}")
print(f"  Optimal path: {env.optimal_path_length()} steps")

# Take a few random actions
import numpy as np
for i in range(10):
    action = np.random.randint(4)
    obs, done, info = env.step(action)
    if done:
        break
print(f"  Agent at: ({env.agent_x}, {env.agent_y})")
print("  PASS")

# Test 3: Agent
print("\n=== Test 3: DAgger Agent ===")
from dagger.agent import DAggerAgent
agent = DAggerAgent(obs_shape=env.observation_space_shape, n_actions=4)
obs = env.reset()
action = agent.select_action(obs)
print(f"  Selected action: {action}")

# Store some expert labels and train
for _ in range(100):
    expert_action = env.optimal_action()
    if expert_action is not None:
        agent.store_expert_label(obs, expert_action)
    action = agent.select_action(obs)
    obs, done, _ = env.step(action)
    if done:
        obs = env.reset()

loss = agent.train_step()
print(f"  Train step loss: {loss:.4f}" if loss else "  Buffer too small")
print("  PASS")

env.close()
print("\nAll tests passed!")
