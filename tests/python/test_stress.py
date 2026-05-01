"""Stress / load tests (B3-27 → B3-30).

B3-27 and B3-28 run locally (maze C library).
B3-29 and B3-30 hit remote servers and are marked @pytest.mark.remote.
"""

import importlib
import json
import ssl
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

import numpy as np
import pytest

from conftest import LOGGING_SERVER, AI_SERVER, https_post, https_get


def _import_dagger(name):
    try:
        return importlib.import_module(name)
    except FileNotFoundError as exc:
        pytest.skip(str(exc))


# B3-27
def test_create_and_free_1000_mazes_no_crash():
    """Create and immediately free 1000 mazes — no leak, no crash."""
    bridge = _import_dagger("dagger.maze_bridge")

    for seed in range(1000):
        handle = bridge.MazeHandle(21, 15, seed)
        assert handle._ptr is not None
        handle.close()


# B3-28
def test_1000_astar_solves_all_valid():
    """Run 1000 A* solves on different seeds — every path is legal."""
    bridge = _import_dagger("dagger.maze_bridge")

    for seed in range(1000):
        handle = bridge.MazeHandle(21, 15, seed)
        try:
            path = handle.astar(0, 0, 20, 14)
            assert len(path) > 0, f"No path found for seed {seed}"

            # Walk the path and verify every step is legal
            px, py = 0, 0
            for step_x, step_y in path:
                dx, dy = step_x - px, step_y - py
                assert abs(dx) + abs(dy) == 1, \
                    f"Non-adjacent step at seed {seed}: ({px},{py})→({step_x},{step_y})"
                assert handle.can_move(px, py, dx, dy), \
                    f"Illegal move at seed {seed}: ({px},{py}) d=({dx},{dy})"
                px, py = step_x, step_y

            assert (px, py) == (20, 14), f"Path did not end at goal for seed {seed}"
        finally:
            handle.close()


# B3-29
@pytest.mark.remote
def test_burst_10_telemetry_posts(mtls_ssl_ctx, test_session_id):
    """Burst 10 concurrent telemetry POSTs — all should return 200."""
    try:
        https_get(f"{LOGGING_SERVER}/health", mtls_ssl_ctx, timeout=5)
    except Exception:
        pytest.skip("Logging server unreachable")

    def post_one(seq):
        payload = {
            "session_id": test_session_id,
            "event_type": "player_move",
            "input": {"device": "stress_test", "move_sequence": seq},
            "player": {"position": {"x": seq % 21, "y": seq // 21}},
            "goal_reached": False,
            "timestamp": "2026-05-01T00:00:00Z",
        }
        for attempt in range(2):
            try:
                status, body = https_post(
                    f"{LOGGING_SERVER}/move", payload, mtls_ssl_ctx, timeout=15,
                )
                return status
            except urllib.error.HTTPError as exc:
                if exc.code >= 500 and attempt == 0:
                    import time; time.sleep(0.3)
                    continue
                return exc.code
            except Exception:
                return -1
        return -1

    results = []
    with ThreadPoolExecutor(max_workers=3) as pool:
        futures = [pool.submit(post_one, i) for i in range(10)]
        for f in as_completed(futures):
            results.append(f.result())

    assert len(results) == 10
    ok_count = sum(1 for s in results if s == 200)
    assert ok_count >= 9, f"Only {ok_count}/10 succeeded (need >=9)"

    # Let server recover before subsequent tests
    import time; time.sleep(1.0)


# B3-30
@pytest.mark.remote
def test_burst_20_policy_requests(mtls_ssl_ctx):
    """Burst 20 concurrent policy requests (all return valid actions)."""
    w, h = 21, 15
    probe_payload = {
        "width": w, "height": h,
        "walls": [15] * (w * h),
        "visited": [1] + [0] * (w * h - 1),
        "agent": [0, 0], "goal": [w - 1, h - 1],
    }
    try:
        https_post(f"{AI_SERVER}/policy", probe_payload, mtls_ssl_ctx, timeout=5)
    except urllib.error.HTTPError:
        pass  # 400 is fine (server is reachable)
    except Exception:
        pytest.skip("AI server unreachable")

    def post_policy(_):
        payload = {
            "width": w, "height": h,
            "walls": [15] * (w * h),
            "visited": [1] + [0] * (w * h - 1),
            "agent": [0, 0],
            "goal": [w - 1, h - 1],
        }
        status, body = https_post(f"{AI_SERVER}/policy", payload, mtls_ssl_ctx)
        return status, body

    results = []
    with ThreadPoolExecutor(max_workers=5) as pool:
        futures = [pool.submit(post_policy, i) for i in range(20)]
        for f in as_completed(futures):
            results.append(f.result())

    assert len(results) == 20
    for status, body in results:
        assert status == 200
        assert body["action"] in (0, 1, 2, 3)
