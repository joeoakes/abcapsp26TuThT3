"""Integration tests against the remote logging server (B3-14 → B3-19).

The logging server runs on 10.170.8.130:8445 (maze_https_final).
All tests are marked @pytest.mark.remote and skipped when the server is unreachable.
"""

import json
import ssl
import time
import urllib.error
import urllib.request

import pytest

from conftest import LOGGING_SERVER, https_post, https_get

pytestmark = pytest.mark.remote


def _server_reachable(ssl_ctx):
    """Quick connectivity check."""
    try:
        https_get(f"{LOGGING_SERVER}/health", ssl_ctx, timeout=5)
        return True
    except Exception:
        return False


@pytest.fixture(autouse=True)
def _require_logging_server(mtls_ssl_ctx):
    if not _server_reachable(mtls_ssl_ctx):
        pytest.skip("Logging server unreachable at " + LOGGING_SERVER)


# B3-14
def test_health_endpoint_returns_ok(mtls_ssl_ctx):
    status, body = https_get(f"{LOGGING_SERVER}/health", mtls_ssl_ctx)
    assert status == 200
    assert body.get("ok") is True


# B3-15
def test_post_move_returns_ok(mtls_ssl_ctx, test_session_id):
    payload = {
        "session_id": test_session_id,
        "event_type": "player_move",
        "input": {"device": "test", "move_sequence": 1},
        "player": {"position": {"x": 0, "y": 0}},
        "goal_reached": False,
        "timestamp": "2026-05-01T00:00:00Z",
    }
    status, body = https_post(f"{LOGGING_SERVER}/move", payload, mtls_ssl_ctx)
    assert status == 200
    assert body.get("ok") is True


# B3-16
def test_get_moves_returns_expected_fields(mtls_ssl_ctx):
    status, body = https_get(f"{LOGGING_SERVER}/moves?limit=1", mtls_ssl_ctx)
    assert status == 200
    assert "ok" in body
    assert "moves" in body
    assert "count" in body
    assert isinstance(body["moves"], list)


# B3-17
def test_post_mission_returns_ok_with_key(mtls_ssl_ctx, test_mission_id):
    payload = {
        "mission_id": test_mission_id,
        "robot_id": "test_robot",
        "mission_type": "maze",
        "start_time": int(time.time()),
        "end_time": int(time.time()),
        "moves_left_turn": 0,
        "moves_right_turn": 0,
        "moves_straight": 1,
        "moves_reverse": 0,
        "moves_total": 1,
        "distance_traveled": 1.0,
        "duration_seconds": 1,
        "mission_result": "success",
        "abort_reason": "",
    }
    status, body = https_post(f"{LOGGING_SERVER}/mission", payload, mtls_ssl_ctx)
    assert status == 200
    assert body.get("ok") is True
    assert "key" in body


# B3-18
def test_get_mission_retrieves_posted_mission(mtls_ssl_ctx, test_mission_id):
    # POST a mission first
    payload = {
        "mission_id": test_mission_id,
        "robot_id": "test_robot",
        "mission_type": "maze",
        "start_time": 1000000,
        "end_time": 1000010,
        "moves_total": 5,
        "distance_traveled": 5.0,
        "duration_seconds": 10,
        "mission_result": "success",
        "abort_reason": "",
    }
    https_post(f"{LOGGING_SERVER}/mission", payload, mtls_ssl_ctx)

    # GET it back
    status, body = https_get(
        f"{LOGGING_SERVER}/mission?mission_id={test_mission_id}",
        mtls_ssl_ctx,
    )
    assert status == 200
    assert body.get("ok") is True
    assert "mission" in body
    assert body["mission"]["mission_id"] == test_mission_id


# B3-19
def test_connection_without_client_cert_is_rejected(no_client_cert_ssl_ctx):
    """Server should reject connections without a valid client certificate.

    Note: /health does NOT verify client certs, so we test against /moves
    which calls verify_client_cert and returns 403 when no cert is presented.
    """
    try:
        status, body = https_get(f"{LOGGING_SERVER}/moves?limit=1", no_client_cert_ssl_ctx)
        # If we got a response, it must be a 403 (no client cert → forbidden)
        assert status == 403
    except urllib.error.HTTPError as exc:
        # urllib raises HTTPError for 4xx/5xx — 403 is the expected code
        assert exc.code == 403
    except (ssl.SSLError, urllib.error.URLError, ConnectionResetError, OSError):
        # TLS handshake failure or connection reset — also acceptable
        pass
