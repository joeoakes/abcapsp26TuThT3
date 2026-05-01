"""Shared fixtures for Build 3 tests."""

import json
import ssl
import uuid
import urllib.request
from pathlib import Path

import pytest


_PROJECT_ROOT = Path(__file__).resolve().parents[2]
_CERT_DIR = _PROJECT_ROOT / "https_final" / "certs"

LOGGING_SERVER = "https://10.170.8.130:8445"
AI_SERVER = "https://10.170.8.109:8445"



@pytest.fixture(scope="session")
def cert_paths():
    """Return dict with client_cert, client_key, ca_cert absolute paths."""
    paths = {
        "client_cert": str(_CERT_DIR / "client.crt"),
        "client_key": str(_CERT_DIR / "client.key"),
        "ca_cert": str(_CERT_DIR / "ca.crt"),
    }
    for label, p in paths.items():
        if not Path(p).exists():
            pytest.skip(f"Certificate not found: {p} ({label})")
    return paths


@pytest.fixture(scope="session")
def mtls_ssl_ctx(cert_paths):
    """SSL context configured for mTLS with client cert.

    Hostname verification is disabled because the server certs use
    localhost / hostname SANs but we connect by IP address.  The peer
    certificate is still verified against the trusted CA.
    """
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.load_cert_chain(
        certfile=cert_paths["client_cert"],
        keyfile=cert_paths["client_key"],
    )
    ctx.load_verify_locations(cafile=cert_paths["ca_cert"])
    return ctx


@pytest.fixture(scope="session")
def no_client_cert_ssl_ctx(cert_paths):
    """SSL context WITHOUT a client cert (for testing mTLS rejection)."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_REQUIRED
    ctx.load_verify_locations(cafile=cert_paths["ca_cert"])
    return ctx



def https_post(url, data, ssl_ctx, timeout=10):
    """POST JSON to *url* with *ssl_ctx*.  Returns (status, parsed_json)."""
    body = json.dumps(data).encode("utf-8")
    req = urllib.request.Request(
        url, data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, context=ssl_ctx, timeout=timeout) as resp:
        return resp.status, json.loads(resp.read())


def https_get(url, ssl_ctx, timeout=10):
    """GET *url* with *ssl_ctx*.  Returns (status, parsed_json)."""
    req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, context=ssl_ctx, timeout=timeout) as resp:
        return resp.status, json.loads(resp.read())



def _try_mongo_delete(session_id):
    """Best-effort: delete test telemetry from MongoDB via SSH tunnel."""
    try:
        import pymongo
        client = pymongo.MongoClient("mongodb://localhost:27017", serverSelectionTimeoutMS=2000)
        col = client["maze"]["team3ttmoves"]
        col.delete_many({"session_id": session_id})
        client.close()
    except Exception:
        pass


def _try_redis_delete(mission_id, prefix="team3ttmission"):
    """Best-effort: delete test mission from Redis via SSH tunnel."""
    try:
        import redis as _redis
        r = _redis.Redis(host="localhost", port=6379, socket_connect_timeout=2)
        r.delete(f"{prefix}:{mission_id}")
        r.close()
    except Exception:
        pass


@pytest.fixture()
def test_session_id():
    """Generate a unique session_id for a test; clean up MongoDB after."""
    sid = f"_TEST_B3_{uuid.uuid4().hex[:12]}"
    yield sid
    _try_mongo_delete(sid)


@pytest.fixture()
def test_mission_id():
    """Generate a unique mission_id for a test; clean up Redis after."""
    mid = f"_TEST_B3_{uuid.uuid4().hex[:12]}"
    yield mid
    _try_redis_delete(mid)
