"""Quick smoke test for the C bridge + environment + agent"""

import importlib

import numpy as np
import pytest


def _import_dagger_module(module_name: str):
    try:
        return importlib.import_module(module_name)
    except FileNotFoundError as exc:
        pytest.skip(str(exc))


def test_bridge_generates_expected_shapes_and_path_length():
    maze_bridge = _import_dagger_module("dagger.maze_bridge")
    handle, walls, dist = maze_bridge.generate_training_instance(21, 15, seed=42)

    try:
        path = handle.astar(0, 0, 20, 14)
        assert walls.shape == (15, 21)
        assert walls.dtype == np.uint8
        assert dist.shape == (15, 21)
        assert dist.dtype == np.int32
        assert dist[0, 0] == len(path)
    finally:
        handle.close()


def test_maze_env_smoke_run():
    maze_env = _import_dagger_module("dagger.maze_env")
    env = maze_env.MazeEnv(width=21, height=15, seed=42)

    try:
        obs = env.reset()
        assert obs.shape == env.observation_space_shape
        assert env.optimal_path_length() >= 0

        rng = np.random.default_rng(42)
        for _ in range(10):
            previous_steps = env.steps
            obs, done, info = env.step(int(rng.integers(4)))
            assert obs.shape == env.observation_space_shape
            assert env.steps == previous_steps + 1
            assert info["steps"] == env.steps
            assert 0 <= env.agent_x < env.width
            assert 0 <= env.agent_y < env.height
            if done:
                break
    finally:
        env.close()


def test_dagger_agent_can_collect_labels_and_train():
    pytest.importorskip("torch")
    maze_env = _import_dagger_module("dagger.maze_env")
    agent_module = importlib.import_module("dagger.agent")
    env = maze_env.MazeEnv(width=21, height=15, seed=42)

    try:
        obs = env.reset()
        agent = agent_module.DAggerAgent(obs_shape=env.observation_space_shape, n_actions=4)

        action = agent.select_action(obs)
        assert 0 <= action < 4

        for _ in range(100):
            expert_action = env.optimal_action()
            if expert_action is None:
                obs = env.reset()
                expert_action = env.optimal_action()

            agent.store_expert_label(obs, expert_action)
            obs, done, _ = env.step(expert_action)
            if done:
                obs = env.reset()

        loss = agent.train_step()
        assert loss is not None
        assert isinstance(loss, float)
        assert loss >= 0.0
    finally:
        env.close()
