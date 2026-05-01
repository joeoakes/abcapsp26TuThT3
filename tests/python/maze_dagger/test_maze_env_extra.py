"""Additional unit tests for MazeEnv (B3-08, B3-09)."""

import numpy as np
import pytest

from dagger import maze_env


class FakeMazeHandle:
    """Minimal fake with an open corridor to the goal."""

    def __init__(self, width: int, height: int, seed: int):
        self.width = width
        self.height = height
        self.seed = seed
        self.closed = False
        # 2x2 maze: open E from (0,0) and open S from (1,0)
        self._walls = np.array(
            [
                [maze_env.WALL_N | maze_env.WALL_W | maze_env.WALL_S,
                 maze_env.WALL_N | maze_env.WALL_E],
                [maze_env.WALL_S | maze_env.WALL_W,
                 maze_env.WALL_S | maze_env.WALL_E | maze_env.WALL_W],
            ],
            dtype=np.uint8,
        )
        self._dist = np.array(
            [[2, 1],
             [1, 0]],
            dtype=np.int32,
        )

    def get_wall_grid(self):
        return self._walls.copy()

    def compute_distance_map(self, gx, gy):
        return self._dist.copy()

    def can_move(self, x, y, dx, dy):
        nx, ny = x + dx, y + dy
        if not (0 <= nx < self.width and 0 <= ny < self.height):
            return False
        w = int(self._walls[y, x])
        if dx == 0 and dy == -1:
            return not bool(w & maze_env.WALL_N)
        if dx == 1 and dy == 0:
            return not bool(w & maze_env.WALL_E)
        if dx == 0 and dy == 1:
            return not bool(w & maze_env.WALL_S)
        if dx == -1 and dy == 0:
            return not bool(w & maze_env.WALL_W)
        return False

    def close(self):
        self.closed = True


# B3-08
def test_reset_with_explicit_seed_is_deterministic(monkeypatch):
    """Two resets with the same explicit seed produce identical observations."""
    monkeypatch.setattr(maze_env, "MazeHandle", FakeMazeHandle)
    env = maze_env.MazeEnv(width=2, height=2, max_steps=10, observation_mode="global", seed=0)

    try:
        obs1 = env.reset(seed=42)
        obs2 = env.reset(seed=42)
        assert np.array_equal(obs1, obs2)
    finally:
        env.close()


# B3-09
def test_goal_reaching_step_returns_done_and_success(monkeypatch):
    """Stepping onto the goal cell sets done=True and info['success']=True."""
    monkeypatch.setattr(maze_env, "MazeHandle", FakeMazeHandle)
    env = maze_env.MazeEnv(width=2, height=2, max_steps=10, observation_mode="global", seed=7)
    env.reset()

    try:
        # Move E then S to reach goal (1,1)
        env.step(maze_env.ACTION_E)       # → (1,0)
        _, done, info = env.step(maze_env.ACTION_S)  # → (1,1) = goal

        assert done is True
        assert info["success"] is True
        assert info["steps"] == 2
    finally:
        env.close()
