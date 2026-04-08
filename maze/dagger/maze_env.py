"""
Maze environment wrapping the C maze library

Observation: fixed-size numpy vector containing:
    - Local wall patch (flattened binary features around agent)
    - Normalized goal direction vector (dx, dy)
    - Normalized Euclidean distance to goal

Actions: 0=North, 1=East, 2=South, 3=West
"""

import numpy as np
from .maze_bridge import MazeHandle, WALL_N, WALL_E, WALL_S, WALL_W


# Action definitions
ACTION_N = 0
ACTION_E = 1
ACTION_S = 2
ACTION_W = 3
NUM_ACTIONS = 4

# Movement deltas indexed by action
DX = [0, 1, 0, -1]
DY = [-1, 0, 1, 0]


class MazeEnv:
    """
    Gym-style maze environment.

    Parameters:
        width, height: maze dimensions (cells)
        patch_radius: half-size of local observation window (default 3 → 7x7 patch)
        max_steps: episode step limit
        observation_mode: 'local' or 'global' (default 'global')
        seed: fixed seed for deterministic mazes (0 = random)
    """

    def __init__(
        self,
        width: int = 21,
        height: int = 15,
        patch_radius: int = 3,
        max_steps: int = 500,
        observation_mode: str = "global",
        seed: int = 0,
        **_kwargs,
    ):
        self.width = width
        self.height = height
        self.patch_radius = patch_radius
        self.max_steps = max_steps
        self.observation_mode = observation_mode

        self._fixed_seed = seed  # 0 = use random seeds each reset

        # Goal always at bottom-right (matches maze_sdl2.c)
        self.goal_x = width - 1
        self.goal_y = height - 1

        # Observation size: 4 wall bits per patch cell + 3 goal features
        self.patch_size = 2 * patch_radius + 1
        if self.observation_mode not in {"local", "global"}:
            raise ValueError("observation_mode must be 'local' or 'global'")
        if self.observation_mode == "local":
            self.obs_dim = self.patch_size * self.patch_size * 5 + 3
        else:
            self.obs_dim = self.height * self.width * 7

        # State (set by reset)
        self._handle = None
        self._wall_grid = None
        self._wall_features = None
        self._goal_channel = None
        self._dist_map = None
        self._visit_count = None
        self.agent_x = 0
        self.agent_y = 0
        self.steps = 0
        self._rng = np.random.default_rng()

    @property
    def observation_space_shape(self):
        if self.observation_mode == "local":
            return (self.obs_dim,)
        return (7, self.height, self.width)

    @property
    def action_space_n(self):
        return NUM_ACTIONS

    # Core API

    def reset(self, seed: int | None = None) -> np.ndarray:
        """Generate a new maze and return the initial observation."""
        if self._handle:
            self._handle.close()

        if seed is not None:
            maze_seed = seed
        elif self._fixed_seed:
            maze_seed = self._fixed_seed
        else:
            maze_seed = int(self._rng.integers(1, 2**31))

        self._handle = MazeHandle(self.width, self.height, maze_seed)
        self._wall_grid = self._handle.get_wall_grid()
        self._dist_map = self._handle.compute_distance_map(self.goal_x, self.goal_y)
        self._wall_features = self._build_wall_features()
        self._goal_channel = np.zeros((self.height, self.width, 1), dtype=np.float32)
        self._goal_channel[self.goal_y, self.goal_x, 0] = 1.0

        # Start at top-left
        self.agent_x = 0
        self.agent_y = 0
        self.steps = 0

        self._visit_count = np.zeros((self.height, self.width), dtype=np.int32)
        self._visit_count[self.agent_y, self.agent_x] = 1

        return self._get_obs()

    def step(self, action: int):
        """Take action, return (obs, done, info)."""
        assert 0 <= action < NUM_ACTIONS
        self.steps += 1

        dx, dy = DX[action], DY[action]

        if self._handle.can_move(self.agent_x, self.agent_y, dx, dy):
            self.agent_x += dx
            self.agent_y += dy
            self._visit_count[self.agent_y, self.agent_x] += 1

        done = False
        info = {"success": False, "steps": self.steps}
        if self.agent_x == self.goal_x and self.agent_y == self.goal_y:
            done = True
            info["success"] = True
        elif self.steps >= self.max_steps:
            done = True

        return self._get_obs(), done, info

    def close(self):
        if self._handle:
            self._handle.close()
            self._handle = None

    # Observation helpers

    def _get_obs(self) -> np.ndarray:
        if self.observation_mode == "local":
            return self._get_local_obs()
        return self._get_global_obs()

    def _get_local_obs(self) -> np.ndarray:
        patch = self._get_wall_patch()
        visit_patch = self._get_visit_patch()
        goal_dx = (self.goal_x - self.agent_x) / max(self.width, 1)
        goal_dy = (self.goal_y - self.agent_y) / max(self.height, 1)
        goal_dist = np.sqrt(goal_dx**2 + goal_dy**2)
        obs = np.concatenate([patch, visit_patch, [goal_dx, goal_dy, goal_dist]])
        return obs.astype(np.float32)

    def _get_global_obs(self) -> np.ndarray:
        visited_channel = (self._visit_count > 0).astype(np.float32)[..., None]
        agent_channel = np.zeros((self.height, self.width, 1), dtype=np.float32)
        agent_channel[self.agent_y, self.agent_x, 0] = 1.0
        obs = np.concatenate(
            [self._wall_features, visited_channel, agent_channel, self._goal_channel],
            axis=-1,
        )
        return np.transpose(obs, (2, 0, 1)).astype(np.float32)

    def _get_wall_patch(self) -> np.ndarray:
        """
        Extract a (2r+1)x(2r+1) local patch of wall features around the agent.
        Each cell contributes 4 binary values (N, E, S, W wall present).
        Out-of-bounds cells are treated as fully walled.
        """
        r = self.patch_radius
        patch = np.ones((self.patch_size, self.patch_size, 4), dtype=np.float32)

        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                cx = self.agent_x + dx
                cy = self.agent_y + dy
                py = dy + r
                px = dx + r
                if 0 <= cx < self.width and 0 <= cy < self.height:
                    w = int(self._wall_grid[cy, cx])
                    patch[py, px, 0] = float(bool(w & WALL_N))
                    patch[py, px, 1] = float(bool(w & WALL_E))
                    patch[py, px, 2] = float(bool(w & WALL_S))
                    patch[py, px, 3] = float(bool(w & WALL_W))

        return patch.flatten()

    def _get_visit_patch(self) -> np.ndarray:
        r = self.patch_radius
        patch = np.zeros((self.patch_size, self.patch_size), dtype=np.float32)

        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                cx = self.agent_x + dx
                cy = self.agent_y + dy
                py = dy + r
                px = dx + r
                if 0 <= cx < self.width and 0 <= cy < self.height:
                    patch[py, px] = float(self._visit_count[cy, cx] > 0)

        return patch.flatten()

    def _build_wall_features(self) -> np.ndarray:
        wall_features = np.zeros((self.height, self.width, 4), dtype=np.float32)
        wall_features[..., 0] = (self._wall_grid & WALL_N) != 0
        wall_features[..., 1] = (self._wall_grid & WALL_E) != 0
        wall_features[..., 2] = (self._wall_grid & WALL_S) != 0
        wall_features[..., 3] = (self._wall_grid & WALL_W) != 0
        return wall_features

    # Info helpers

    def optimal_path_length(self) -> int:
        """Return the A* optimal path length from start to goal."""
        d = int(self._dist_map[0, 0])
        return d if d >= 0 else -1

    def optimal_action(self) -> int | None:
        """Return the action that follows the BFS shortest path from the current state."""
        if self.agent_x == self.goal_x and self.agent_y == self.goal_y:
            return None

        best_action = None
        best_dist = int(self._dist_map[self.agent_y, self.agent_x])

        for action, (dx, dy) in enumerate(zip(DX, DY)):
            if not self._handle.can_move(self.agent_x, self.agent_y, dx, dy):
                continue
            nx = self.agent_x + dx
            ny = self.agent_y + dy
            d = int(self._dist_map[ny, nx])
            if d >= 0 and d < best_dist:
                best_dist = d
                best_action = action

        return best_action

    def render_ascii(self) -> str:
        """Simple ASCII rendering for debugging."""
        lines = []
        for y in range(self.height):
            top = ""
            mid = ""
            for x in range(self.width):
                w = int(self._wall_grid[y, x])
                top += "+" + ("---" if w & WALL_N else "   ")
                if x == self.agent_x and y == self.agent_y:
                    cell = " A "
                elif x == self.goal_x and y == self.goal_y:
                    cell = " G "
                else:
                    cell = "   "
                mid += ("|" if w & WALL_W else " ") + cell
            top += "+"
            mid += "|" if int(self._wall_grid[y, self.width - 1]) & WALL_E else " "
            lines.append(top)
            lines.append(mid)
        # Bottom border
        bot = ""
        for x in range(self.width):
            w = int(self._wall_grid[self.height - 1, x])
            bot += "+" + ("---" if w & WALL_S else "   ")
        bot += "+"
        lines.append(bot)
        return "\n".join(lines)
