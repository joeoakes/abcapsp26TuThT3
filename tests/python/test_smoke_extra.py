"""Smoke tests for Build 3 (B3-11 → B3-13).

These use the real C library (libmaze.so) and torch.
"""

import importlib

import numpy as np
import pytest


def _import_dagger(name):
    try:
        return importlib.import_module(name)
    except FileNotFoundError as exc:
        pytest.skip(str(exc))


# B3-11
def test_different_seeds_produce_different_wall_grids():
    """Two mazes with different seeds have at least one wall byte difference."""
    bridge = _import_dagger("dagger.maze_bridge")

    h1, w1, _ = bridge.generate_training_instance(21, 15, seed=100)
    h2, w2, _ = bridge.generate_training_instance(21, 15, seed=200)

    try:
        walls1 = h1.get_wall_grid()
        walls2 = h2.get_wall_grid()
        assert not np.array_equal(walls1, walls2), \
            "Two different seeds produced identical wall grids"
    finally:
        h1.close()
        h2.close()


# B3-12
def test_evaluate_greedy_runs_without_error():
    """evaluate_greedy runs 5 held-out mazes and returns valid metrics."""
    torch = pytest.importorskip("torch")
    maze_env = _import_dagger("dagger.maze_env")
    agent_mod = importlib.import_module("dagger.agent")
    train_mod = importlib.import_module("dagger.train")

    env = maze_env.MazeEnv(width=7, height=5, max_steps=50, seed=1)

    try:
        agent = agent_mod.DAggerAgent(
            obs_shape=env.observation_space_shape,
            n_actions=4,
            hidden=16,
        )

        # Build a tiny args namespace matching evaluate_greedy signature
        args = type("Args", (), {
            "width": 7, "height": 5, "patch_radius": 3,
            "max_steps": 50, "observation_mode": "global",
            "eval_mazes": 5, "eval_seed_offset": 900_000,
        })()

        success_rate, avg_eff = train_mod.evaluate_greedy(agent, args)

        assert isinstance(success_rate, float)
        assert 0.0 <= success_rate <= 100.0
        assert isinstance(avg_eff, float)
    finally:
        env.close()


# B3-13
def test_prefill_expert_buffer_fills_correct_count():
    """prefill_expert_buffer populates the agent's buffer with labels."""
    torch = pytest.importorskip("torch")
    maze_env = _import_dagger("dagger.maze_env")
    agent_mod = importlib.import_module("dagger.agent")
    train_mod = importlib.import_module("dagger.train")

    env = maze_env.MazeEnv(width=7, height=5, max_steps=50, seed=1)

    try:
        agent = agent_mod.DAggerAgent(
            obs_shape=env.observation_space_shape,
            n_actions=4,
            hidden=16,
        )

        args = type("Args", (), {
            "width": 7, "height": 5, "patch_radius": 3,
            "max_steps": 50, "observation_mode": "global",
            "expert_prefill_episodes": 3,
        })()

        count = train_mod.prefill_expert_buffer(agent, args)
        assert count > 0
        assert len(agent.expert_buffer) == count
    finally:
        env.close()
