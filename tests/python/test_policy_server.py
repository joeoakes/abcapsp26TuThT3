"""Unit tests for policy_server.py (B3-01 → B3-07)."""

import importlib
import io
import json
import sys
import types
from pathlib import Path

import numpy as np
import pytest

torch = pytest.importorskip("torch")

# Import policy_server (maze/ is on sys.path via root conftest.py)
import policy_server



def make_handler(path, body, monkeypatch):
    """Create a PolicyHandler with mocked HTTP plumbing (like test_robot_bridge)."""
    handler = policy_server.PolicyHandler.__new__(policy_server.PolicyHandler)
    handler.path = path
    handler.headers = {"Content-Length": str(len(body))}
    handler.rfile = io.BytesIO(body)
    handler.wfile = io.BytesIO()
    handler.errors = []
    handler.responses = []
    handler.response_headers = []
    handler.ended = False
    monkeypatch.setattr(
        handler, "send_error",
        lambda code, message="": handler.errors.append((code, message)),
    )
    monkeypatch.setattr(
        handler, "send_response",
        lambda code: handler.responses.append(code),
    )
    monkeypatch.setattr(
        handler, "send_header",
        lambda key, value: handler.response_headers.append((key, value)),
    )
    monkeypatch.setattr(
        handler, "end_headers",
        lambda: setattr(handler, "ended", True),
    )
    return handler


# B3-01
def test_build_global_obs_produces_correct_shape_and_channels():
    """build_global_obs returns (7, H, W) float32 with correct channel layout."""
    width, height = 3, 2
    walls = [0x0F] * 6      # all walls everywhere
    visited = [0, 1, 0, 0, 0, 0]
    agent = [0, 0]
    goal = [2, 1]

    obs = policy_server.build_global_obs(width, height, walls, visited, agent, goal)

    assert obs.shape == (7, height, width)
    assert obs.dtype == np.float32
    # Agent channel (index 5) should have a 1.0 only at agent position
    assert obs[5, 0, 0] == 1.0
    assert obs[5].sum() == 1.0
    # Goal channel (index 6) should have a 1.0 only at goal position
    assert obs[6, 1, 2] == 1.0
    assert obs[6].sum() == 1.0
    # Visited channel (index 4): cell (1,0) visited
    assert obs[4, 0, 1] == 1.0
    assert obs[4].sum() == 1.0


# B3-02
def test_build_global_obs_wall_features_match_bitmask():
    """Wall feature channels correctly decompose bitmask values."""
    width, height = 2, 1
    # Cell 0: WALL_N(1) | WALL_E(2) = 3
    # Cell 1: WALL_S(4) | WALL_W(8) = 12
    walls = [3, 12]
    visited = [0, 0]
    agent = [0, 0]
    goal = [1, 0]

    obs = policy_server.build_global_obs(width, height, walls, visited, agent, goal)

    # Channel 0 = WALL_N
    assert obs[0, 0, 0] == 1.0   # cell 0 has N wall
    assert obs[0, 0, 1] == 0.0   # cell 1 does not
    # Channel 1 = WALL_E
    assert obs[1, 0, 0] == 1.0   # cell 0 has E wall
    assert obs[1, 0, 1] == 0.0
    # Channel 2 = WALL_S
    assert obs[2, 0, 0] == 0.0
    assert obs[2, 0, 1] == 1.0   # cell 1 has S wall
    # Channel 3 = WALL_W
    assert obs[3, 0, 0] == 0.0
    assert obs[3, 0, 1] == 1.0   # cell 1 has W wall


# B3-03
def test_policy_handler_rejects_non_policy_path(monkeypatch):
    handler = make_handler("/bad", b"{}", monkeypatch)
    policy_server.PolicyHandler.do_POST(handler)
    assert handler.errors == [(404, "Not Found")]


# B3-04
def test_policy_handler_rejects_empty_body(monkeypatch):
    handler = make_handler("/policy", b"", monkeypatch)
    policy_server.PolicyHandler.do_POST(handler)
    assert handler.errors == [(400, "Empty body")]


# B3-05
def test_policy_handler_rejects_invalid_json(monkeypatch):
    handler = make_handler("/policy", b"{bad", monkeypatch)
    policy_server.PolicyHandler.do_POST(handler)
    assert handler.errors == [(400, "Invalid JSON")]


# B3-06
def test_policy_handler_rejects_mismatched_walls_length(monkeypatch):
    body = json.dumps({
        "width": 3, "height": 2,
        "walls": [15, 15, 15],  # only 3, should be 6
        "visited": [0] * 6,
        "agent": [0, 0],
        "goal": [2, 1],
    }).encode()
    handler = make_handler("/policy", body, monkeypatch)
    policy_server.PolicyHandler.do_POST(handler)
    assert len(handler.errors) == 1
    assert handler.errors[0][0] == 400


# B3-07
def test_policy_handler_returns_valid_action(monkeypatch):
    """Well-formed request returns 200 with {"action": 0..3}."""
    from dagger.agent import DAggerAgent

    agent = DAggerAgent(obs_shape=(7, 2, 3), n_actions=4, hidden=16)
    agent.policy_net.eval()
    monkeypatch.setattr(policy_server, "_agent", agent)

    body = json.dumps({
        "width": 3, "height": 2,
        "walls": [15] * 6,
        "visited": [0] * 6,
        "agent": [0, 0],
        "goal": [2, 1],
    }).encode()

    handler = make_handler("/policy", body, monkeypatch)
    policy_server.PolicyHandler.do_POST(handler)

    assert handler.errors == []
    assert handler.responses == [200]
    response_body = handler.wfile.getvalue()
    data = json.loads(response_body)
    assert "action" in data
    assert data["action"] in (0, 1, 2, 3)
