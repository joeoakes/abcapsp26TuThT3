import numpy as np
import pytest

from dagger import maze_env


class FakeMazeHandle:
    def __init__(self, width: int, height: int, seed: int):
        self.width = width
        self.height = height
        self.seed = seed
        self.closed = False
        self._walls = np.array(
            [
                [maze_env.WALL_N | maze_env.WALL_W, maze_env.WALL_N, maze_env.WALL_N | maze_env.WALL_E],
                [maze_env.WALL_W, 0, maze_env.WALL_E],
                [maze_env.WALL_S | maze_env.WALL_W, maze_env.WALL_S, maze_env.WALL_S | maze_env.WALL_E],
            ],
            dtype=np.uint8,
        )
        self._dist = np.array(
            [
                [4, 3, 2],
                [3, 2, 1],
                [2, 1, 0],
            ],
            dtype=np.int32,
        )

    def get_wall_grid(self) -> np.ndarray:
        return self._walls.copy()

    def compute_distance_map(self, gx: int, gy: int) -> np.ndarray:
        assert (gx, gy) == (self.width - 1, self.height - 1)
        return self._dist.copy()

    def can_move(self, x: int, y: int, dx: int, dy: int) -> bool:
        nx = x + dx
        ny = y + dy
        if not (0 <= nx < self.width and 0 <= ny < self.height):
            return False
        wall = int(self._walls[y, x])
        if dx == 0 and dy == -1:
            return not bool(wall & maze_env.WALL_N)
        if dx == 1 and dy == 0:
            return not bool(wall & maze_env.WALL_E)
        if dx == 0 and dy == 1:
            return not bool(wall & maze_env.WALL_S)
        if dx == -1 and dy == 0:
            return not bool(wall & maze_env.WALL_W)
        return False

    def close(self):
        self.closed = True


@pytest.fixture
def fake_env(monkeypatch):
    monkeypatch.setattr(maze_env, "MazeHandle", FakeMazeHandle)
    env = maze_env.MazeEnv(width=3, height=3, patch_radius=1, max_steps=3, observation_mode="global", seed=7)
    env.reset()
    try:
        yield env
    finally:
        env.close()


@pytest.fixture
def fake_local_env(monkeypatch):
    monkeypatch.setattr(maze_env, "MazeHandle", FakeMazeHandle)
    env = maze_env.MazeEnv(width=3, height=3, patch_radius=1, max_steps=4, observation_mode="local", seed=11)
    env.reset()
    try:
        yield env
    finally:
        env.close()


# B2-23
def test_constructor_rejects_invalid_observation_mode():
    with pytest.raises(ValueError):
        maze_env.MazeEnv(observation_mode="invalid")


# B2-24
def test_global_reset_initializes_expected_state(fake_env):
    assert fake_env.observation_space_shape == (7, 3, 3)
    assert fake_env.action_space_n == 4
    assert fake_env.agent_x == 0
    assert fake_env.agent_y == 0
    assert fake_env.goal_x == 2
    assert fake_env.goal_y == 2
    assert fake_env.steps == 0
    assert fake_env._visit_count[0, 0] == 1
    assert fake_env.optimal_path_length() == 4


# B2-25
def test_global_observation_contains_agent_visited_and_goal_channels(fake_env):
    obs = fake_env.reset()

    assert obs.shape == (7, 3, 3)
    assert obs.dtype == np.float32
    assert obs[4, 0, 0] == 1.0
    assert obs[5, 0, 0] == 1.0
    assert obs[5].sum() == 1.0
    assert obs[6, 2, 2] == 1.0
    assert obs[6].sum() == 1.0


# B2-26
def test_step_moves_agent_and_updates_visit_count(fake_env):
    obs, done, info = fake_env.step(maze_env.ACTION_E)

    assert obs.shape == fake_env.observation_space_shape
    assert fake_env.agent_x == 1
    assert fake_env.agent_y == 0
    assert fake_env.steps == 1
    assert fake_env._visit_count[0, 1] == 1
    assert done is False
    assert info == {"success": False, "steps": 1}


# B2-27
def test_blocked_step_keeps_position_but_increments_steps(fake_env):
    obs, done, info = fake_env.step(maze_env.ACTION_N)

    assert obs.shape == fake_env.observation_space_shape
    assert fake_env.agent_x == 0
    assert fake_env.agent_y == 0
    assert fake_env.steps == 1
    assert fake_env._visit_count[0, 0] == 1
    assert done is False
    assert info["steps"] == 1


# B2-28
def test_max_steps_ends_episode(fake_env):
    fake_env.step(maze_env.ACTION_N)
    fake_env.step(maze_env.ACTION_N)
    _, done, info = fake_env.step(maze_env.ACTION_N)

    assert done is True
    assert info == {"success": False, "steps": 3}


# B2-29
def test_optimal_action_follows_shortest_path(fake_env):
    assert fake_env.optimal_action() == maze_env.ACTION_E

    fake_env.step(maze_env.ACTION_E)
    assert fake_env.optimal_action() == maze_env.ACTION_E

    fake_env.step(maze_env.ACTION_E)
    assert fake_env.optimal_action() == maze_env.ACTION_S


# B2-30
def test_optimal_action_is_none_at_goal(fake_env):
    fake_env.agent_x = fake_env.goal_x
    fake_env.agent_y = fake_env.goal_y

    assert fake_env.optimal_action() is None


# B2-31
def test_local_observation_shape_and_boundary_walls(fake_local_env):
    obs = fake_local_env.reset()

    assert obs.shape == (48,)
    assert obs.dtype == np.float32

    patch = obs[: fake_local_env.patch_size * fake_local_env.patch_size * 4].reshape(fake_local_env.patch_size, fake_local_env.patch_size, 4)
    visit_patch = obs[fake_local_env.patch_size * fake_local_env.patch_size * 4 : -3].reshape(fake_local_env.patch_size, fake_local_env.patch_size)

    assert np.all(patch[0, 0] == 1.0)
    assert visit_patch[1, 1] == 1.0
    assert visit_patch.sum() == 1.0


# B2-32
def test_local_goal_features_are_normalized(fake_local_env):
    obs = fake_local_env.reset()
    goal_dx, goal_dy, goal_dist = obs[-3:]

    assert goal_dx == pytest.approx(2 / 3)
    assert goal_dy == pytest.approx(2 / 3)
    assert goal_dist == pytest.approx(np.sqrt((2 / 3) ** 2 + (2 / 3) ** 2))


# B2-33
def test_render_ascii_includes_agent_goal_and_borders(fake_env):
    rendered = fake_env.render_ascii()

    assert " A " in rendered
    assert " G " in rendered
    assert "+" in rendered


# B2-34
def test_close_releases_handle(fake_env):
    handle = fake_env._handle

    fake_env.close()

    assert handle.closed is True
    assert fake_env._handle is None
