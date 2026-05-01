"""System / end-to-end tests (B3-23 → B3-26).

These combine multiple components and talk to remote servers.
Marked @pytest.mark.remote.
"""

import importlib
import json
import ssl
import time
import urllib.error
import urllib.request

import numpy as np
import pytest

from conftest import (
    LOGGING_SERVER,
    AI_SERVER,
    https_post,
    https_get,
    _try_mongo_delete,
    _try_redis_delete,
)

pytestmark = pytest.mark.remote


def _logging_reachable(ssl_ctx):
    try:
        https_get(f"{LOGGING_SERVER}/health", ssl_ctx, timeout=5)
        return True
    except Exception:
        return False


def _ai_reachable(ssl_ctx):
    try:
        w, h = 21, 15
        payload = {
            "width": w, "height": h,
            "walls": [15] * (w * h),
            "visited": [1] + [0] * (w * h - 1),
            "agent": [0, 0], "goal": [w - 1, h - 1],
        }
        body = json.dumps(payload).encode()
        req = urllib.request.Request(
            f"{AI_SERVER}/policy", data=body,
            headers={"Content-Type": "application/json"}, method="POST",
        )
        urllib.request.urlopen(req, context=ssl_ctx, timeout=5)
        return True
    except urllib.error.HTTPError:
        return True
    except Exception:
        return False


# B3-23
def test_telemetry_round_trip(mtls_ssl_ctx, test_session_id):
    """POST a move → GET it back → verify fields match."""
    if not _logging_reachable(mtls_ssl_ctx):
        pytest.skip("Logging server unreachable")

    payload = {
        "session_id": test_session_id,
        "event_type": "player_move",
        "input": {"device": "pytest", "move_sequence": 42},
        "player": {"position": {"x": 3, "y": 7}},
        "goal_reached": False,
        "timestamp": "2026-05-01T00:00:00Z",
    }
    status, _ = https_post(f"{LOGGING_SERVER}/move", payload, mtls_ssl_ctx)
    assert status == 200

    # Retrieve (filter by session_id)
    time.sleep(0.5)  # allow server to persist
    status, body = https_get(
        f"{LOGGING_SERVER}/moves?limit=100&session_id={test_session_id}",
        mtls_ssl_ctx,
    )
    assert status == 200
    assert body["count"] >= 1

    matched = [m for m in body["moves"] if m.get("session_id") == test_session_id]
    assert len(matched) >= 1
    m = matched[0]
    assert m["event_type"] == "player_move"
    assert m["player"]["position"]["x"] == 3


# B3-24
def test_mission_round_trip(mtls_ssl_ctx, test_mission_id):
    """POST a mission summary → GET it → verify all fields match."""
    if not _logging_reachable(mtls_ssl_ctx):
        pytest.skip("Logging server unreachable")

    payload = {
        "mission_id": test_mission_id,
        "robot_id": "pytest_robot",
        "mission_type": "maze",
        "start_time": 1000000,
        "end_time": 1000035,
        "moves_left_turn": 4,
        "moves_right_turn": 5,
        "moves_straight": 10,
        "moves_reverse": 1,
        "moves_total": 20,
        "distance_traveled": 20.0,
        "duration_seconds": 35,
        "mission_result": "success",
        "abort_reason": "",
    }
    status, post_body = https_post(f"{LOGGING_SERVER}/mission", payload, mtls_ssl_ctx)
    assert status == 200
    assert post_body["ok"] is True

    # GET back
    status, get_body = https_get(
        f"{LOGGING_SERVER}/mission?mission_id={test_mission_id}",
        mtls_ssl_ctx,
    )
    assert status == 200
    mission = get_body["mission"]
    assert mission["mission_id"] == test_mission_id
    assert mission["robot_id"] == "pytest_robot"
    assert mission["mission_result"] == "success"
    assert mission["duration_seconds"] == "35"  # Redis stores as string


# B3-25
def test_policy_guided_maze_solve(mtls_ssl_ctx):
    """Send maze state to AI server repeatedly and try to solve the maze."""
    if not _ai_reachable(mtls_ssl_ctx):
        pytest.skip("AI server unreachable")

    try:
        maze_env_mod = importlib.import_module("dagger.maze_env")
    except FileNotFoundError:
        pytest.skip("libmaze.so not available")

    env = maze_env_mod.MazeEnv(width=21, height=15, max_steps=500, seed=42)

    try:
        env.reset()
        walls_flat = env._wall_grid.flatten().tolist()
        visited = (env._visit_count > 0).astype(int).flatten().tolist()

        steps = 0
        max_steps = 200

        while steps < max_steps:
            payload = {
                "width": env.width,
                "height": env.height,
                "walls": walls_flat,
                "visited": visited,
                "agent": [env.agent_x, env.agent_y],
                "goal": [env.goal_x, env.goal_y],
            }

            status, body = https_post(f"{AI_SERVER}/policy", payload, mtls_ssl_ctx)
            assert status == 200
            action = body["action"]
            assert action in (0, 1, 2, 3)

            _, done, info = env.step(action)
            visited = (env._visit_count > 0).astype(int).flatten().tolist()
            steps += 1

            if done:
                break

        # We don't require success, just that the loop completed without error
        assert steps > 0
    finally:
        env.close()


# B3-26
def test_multi_move_session_filtered_retrieval(mtls_ssl_ctx, test_session_id):
    """POST 5 moves with same session_id → GET filtered → count == 5."""
    if not _logging_reachable(mtls_ssl_ctx):
        pytest.skip("Logging server unreachable")

    for seq in range(1, 6):
        payload = {
            "session_id": test_session_id,
            "event_type": "player_move",
            "input": {"device": "pytest", "move_sequence": seq},
            "player": {"position": {"x": seq, "y": 0}},
            "goal_reached": (seq == 5),
            "timestamp": "2026-05-01T00:00:00Z",
        }
        status, _ = https_post(f"{LOGGING_SERVER}/move", payload, mtls_ssl_ctx)
        assert status == 200

    time.sleep(1.0)  # allow server to persist all
    status, body = https_get(
        f"{LOGGING_SERVER}/moves?limit=100&session_id={test_session_id}",
        mtls_ssl_ctx,
    )
    assert status == 200
    matched = [m for m in body["moves"] if m.get("session_id") == test_session_id]
    assert len(matched) == 5
