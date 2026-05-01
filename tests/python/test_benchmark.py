"""Benchmarking tests (B3-31 → B3-35).

These measure performance and print timing results.
They are NOT marked remote; they all run locally.
"""

import importlib
import time

import numpy as np
import pytest


def _import_dagger(name):
    try:
        return importlib.import_module(name)
    except FileNotFoundError as exc:
        pytest.skip(str(exc))


# B3-31
def test_benchmark_maze_create_destroy():
    """Benchmark: maze create+destroy for 21x15 (avg over 500 iterations)."""
    bridge = _import_dagger("dagger.maze_bridge")
    iterations = 500

    start = time.perf_counter()
    for seed in range(iterations):
        h = bridge.MazeHandle(21, 15, seed)
        h.close()
    elapsed = time.perf_counter() - start

    avg_us = (elapsed / iterations) * 1e6
    print(f"\n  maze create+destroy: {avg_us:.1f} µs/iter ({iterations} iters, {elapsed:.3f}s total)")
    assert elapsed < 30.0  # sanity: should finish well under 30s


# B3-32
def test_benchmark_astar_solve():
    """Benchmark: A* solve for 21x15 maze (avg over 500 iterations)."""
    bridge = _import_dagger("dagger.maze_bridge")
    iterations = 500

    # Pre-create mazes
    handles = [bridge.MazeHandle(21, 15, s) for s in range(iterations)]

    start = time.perf_counter()
    for h in handles:
        path = h.astar(0, 0, 20, 14)
        assert len(path) > 0
    elapsed = time.perf_counter() - start

    for h in handles:
        h.close()

    avg_us = (elapsed / iterations) * 1e6
    print(f"\n  A* solve 21x15: {avg_us:.1f} µs/iter ({iterations} iters, {elapsed:.3f}s total)")
    assert elapsed < 30.0


# B3-33
def test_benchmark_distance_map():
    """Benchmark: BFS distance map compute (avg over 500 iterations)."""
    bridge = _import_dagger("dagger.maze_bridge")
    iterations = 500

    handles = [bridge.MazeHandle(21, 15, s) for s in range(iterations)]

    start = time.perf_counter()
    for h in handles:
        dist = h.compute_distance_map(20, 14)
        assert dist[0, 0] >= 0
    elapsed = time.perf_counter() - start

    for h in handles:
        h.close()

    avg_us = (elapsed / iterations) * 1e6
    print(f"\n  BFS distance map 21x15: {avg_us:.1f} µs/iter ({iterations} iters, {elapsed:.3f}s total)")
    assert elapsed < 30.0


# B3-34
def test_benchmark_train_step_latency():
    """Benchmark: DAggerAgent train_step latency (avg over 100 gradient steps)."""
    torch = pytest.importorskip("torch")
    agent_mod = importlib.import_module("dagger.agent")

    torch.manual_seed(0)
    agent = agent_mod.DAggerAgent(obs_shape=(7, 15, 21), n_actions=4, hidden=64, batch_size=32)

    # Fill expert buffer
    for _ in range(200):
        state = np.random.randn(7, 15, 21).astype(np.float32)
        agent.store_expert_label(state, np.random.randint(4))

    iterations = 100

    # Warm up
    agent.train_step()

    start = time.perf_counter()
    for _ in range(iterations):
        loss = agent.train_step()
        assert loss is not None
    elapsed = time.perf_counter() - start

    avg_ms = (elapsed / iterations) * 1e3
    print(f"\n  train_step: {avg_ms:.2f} ms/step ({iterations} steps, {elapsed:.3f}s total)")
    assert elapsed < 60.0


# B3-35
def test_benchmark_select_action_latency():
    """Benchmark: DAggerAgent select_action inference (avg over 1000 calls)."""
    torch = pytest.importorskip("torch")
    agent_mod = importlib.import_module("dagger.agent")

    torch.manual_seed(0)
    agent = agent_mod.DAggerAgent(obs_shape=(7, 15, 21), n_actions=4, hidden=64)
    agent.policy_net.eval()

    state = np.random.randn(7, 15, 21).astype(np.float32)
    iterations = 1000

    # Warm up
    agent.select_action(state, greedy=True)

    start = time.perf_counter()
    for _ in range(iterations):
        action = agent.select_action(state, greedy=True)
        assert 0 <= action < 4
    elapsed = time.perf_counter() - start

    avg_us = (elapsed / iterations) * 1e6
    print(f"\n  select_action: {avg_us:.1f} µs/call ({iterations} calls, {elapsed:.3f}s total)")
    assert elapsed < 30.0
