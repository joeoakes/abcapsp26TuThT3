"""Integration tests against the remote AI / policy server (B3-20 → B3-22).

The policy server runs on 10.170.8.109:8445 (policy_server.py).
All tests are marked @pytest.mark.remote and skipped when the server is unreachable.
"""

import json
import ssl
import urllib.error
import urllib.request

import pytest

from conftest import AI_SERVER, https_post, https_get

pytestmark = pytest.mark.remote


def _server_reachable(ssl_ctx):
    try:
        # Send a valid 21x15 observation so the server doesn't crash on
        # shape mismatches — the trained model expects obs_shape=(7,15,21).
        payload = _build_simple_observation()
        body = json.dumps(payload).encode()
        req = urllib.request.Request(
            f"{AI_SERVER}/policy", data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        urllib.request.urlopen(req, context=ssl_ctx, timeout=5)
        return True
    except urllib.error.HTTPError:
        # A 400 still means the server is reachable
        return True
    except Exception:
        return False


@pytest.fixture(autouse=True)
def _require_ai_server(mtls_ssl_ctx):
    if not _server_reachable(mtls_ssl_ctx):
        pytest.skip("AI server unreachable at " + AI_SERVER)


def _build_simple_observation():
    """Build a valid observation payload matching the trained model (21x15)."""
    w, h = 21, 15
    return {
        "width": w,
        "height": h,
        "walls": [15] * (w * h),       # all walls
        "visited": [1] + [0] * (w * h - 1),  # only start visited
        "agent": [0, 0],
        "goal": [w - 1, h - 1],
    }


# B3-20
def test_policy_returns_valid_action(mtls_ssl_ctx):
    """POST /policy with valid observation returns {"action": 0-3}."""
    payload = _build_simple_observation()
    status, body = https_post(f"{AI_SERVER}/policy", payload, mtls_ssl_ctx)

    assert status == 200
    assert "action" in body
    assert body["action"] in (0, 1, 2, 3)


# B3-21
def test_policy_rejects_invalid_json(mtls_ssl_ctx):
    """POST /policy with malformed JSON returns 400."""
    raw = b"{bad json"
    req = urllib.request.Request(
        f"{AI_SERVER}/policy", data=raw,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with pytest.raises(urllib.error.HTTPError) as exc_info:
        urllib.request.urlopen(req, context=mtls_ssl_ctx, timeout=10)
    assert exc_info.value.code == 400


# B3-22
def test_policy_wrong_path_returns_404(mtls_ssl_ctx):
    """POST /notfound returns 404."""
    payload = _build_simple_observation()
    raw = json.dumps(payload).encode()
    req = urllib.request.Request(
        f"{AI_SERVER}/notfound", data=raw,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with pytest.raises(urllib.error.HTTPError) as exc_info:
        urllib.request.urlopen(req, context=mtls_ssl_ctx, timeout=10)
    assert exc_info.value.code == 404
