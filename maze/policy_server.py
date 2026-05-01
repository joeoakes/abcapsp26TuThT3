"""
HTTPS inference server for the ML maze agent. Run this on the AI server.

Request JSON (POST /policy):
    {
      "width":   <int>,
      "height":  <int>,
      "walls":   [H*W uint8 bitmasks, row-major],
      "visited": [H*W 0/1 flags, row-major],
      "agent":   [x, y],
      "goal":    [x, y]
    }

Response JSON:
    { "action": 0 | 1 | 2 | 3 }   # 0=N, 1=E, 2=S, 3=W

Environment variables:
    POLICY_PORT         HTTPS listen port             (default: 8445)
    POLICY_CHECKPOINT   Path to .pt checkpoint        (default: checkpoints/dagger_best.pt)
    POLICY_DEVICE       'cpu' or 'cuda'               (default: cuda if available)
    POLICY_HIDDEN       Hidden layer width            (default: 256)
    CERT_FILE           TLS server cert               (default: certs/spark_server.crt)
    KEY_FILE            TLS server key                (default: certs/spark_server.key)
    CA_CERT_FILE        CA used to verify clients     (default: certs/ca.crt)
    POLICY_VERIFY_PEER  1 to require client cert      (default: 1 - full mTLS)
"""

import json
import os
import ssl
import sys
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path

import numpy as np
import torch

# Make sibling `dagger` package importable regardless of CWD
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from dagger.agent import DAggerAgent  # noqa: E402


POLICY_PORT = int(os.environ.get("POLICY_PORT", "8445"))
POLICY_CHECKPOINT = os.environ.get("POLICY_CHECKPOINT", "checkpoints/dagger_best.pt")
POLICY_DEVICE = os.environ.get("POLICY_DEVICE") or ("cuda" if torch.cuda.is_available() else "cpu")
POLICY_HIDDEN = int(os.environ.get("POLICY_HIDDEN", "256"))
CERT_FILE = os.environ.get("CERT_FILE", "certs/spark_server.crt")
KEY_FILE = os.environ.get("KEY_FILE", "certs/spark_server.key")
CA_CERT_FILE = os.environ.get("CA_CERT_FILE", "certs/ca.crt")
POLICY_VERIFY_PEER = os.environ.get("POLICY_VERIFY_PEER", "1").strip().lower() in ("1", "true", "yes", "on")

WALL_N, WALL_E, WALL_S, WALL_W = 1, 2, 4, 8
N_ACTIONS = 4

_agent: DAggerAgent | None = None
_infer_lock = threading.Lock()


def build_global_obs(
    width: int,
    height: int,
    walls_flat,
    visited_flat,
    agent_xy,
    goal_xy,
) -> np.ndarray:
    """Reproduce MazeEnv._get_global_obs from raw maze state."""
    walls = np.asarray(walls_flat, dtype=np.uint8).reshape(height, width)
    visited = np.asarray(visited_flat, dtype=np.uint8).reshape(height, width)

    wall_features = np.zeros((height, width, 4), dtype=np.float32)
    wall_features[..., 0] = (walls & WALL_N) != 0
    wall_features[..., 1] = (walls & WALL_E) != 0
    wall_features[..., 2] = (walls & WALL_S) != 0
    wall_features[..., 3] = (walls & WALL_W) != 0

    visited_ch = (visited > 0).astype(np.float32)[..., None]
    agent_ch = np.zeros((height, width, 1), dtype=np.float32)
    ax, ay = int(agent_xy[0]), int(agent_xy[1])
    agent_ch[ay, ax, 0] = 1.0
    goal_ch = np.zeros((height, width, 1), dtype=np.float32)
    gx, gy = int(goal_xy[0]), int(goal_xy[1])
    goal_ch[gy, gx, 0] = 1.0

    obs = np.concatenate([wall_features, visited_ch, agent_ch, goal_ch], axis=-1)
    return np.transpose(obs, (2, 0, 1)).astype(np.float32)


def build_tls_context() -> ssl.SSLContext:
    """Build the server-side TLS context. Peer verification is opt-in."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(certfile=CERT_FILE, keyfile=KEY_FILE)

    if POLICY_VERIFY_PEER:
        ctx.load_verify_locations(cafile=CA_CERT_FILE)
        ctx.verify_mode = ssl.CERT_REQUIRED
        print(f"[policy] mTLS peer verification ENABLED (trusted CA={CA_CERT_FILE})")
    else:
        ctx.verify_mode = ssl.CERT_NONE
        print("[policy] mTLS peer verification DISABLED (set POLICY_VERIFY_PEER=1 to require client cert)")
    return ctx


class PolicyHandler(BaseHTTPRequestHandler):
    """HTTP handler for POST /policy inference requests."""

    def do_POST(self):
        if self.path != "/policy":
            self.send_error(404, "Not Found")
            return

        length = int(self.headers.get("Content-Length", 0))
        if length == 0:
            self.send_error(400, "Empty body")
            return

        raw = self.rfile.read(length)
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            self.send_error(400, "Invalid JSON")
            return

        try:
            width = int(data["width"])
            height = int(data["height"])
            walls = data["walls"]
            visited = data["visited"]
            agent = data["agent"]
            goal = data["goal"]
            if len(walls) != width * height or len(visited) != width * height:
                raise ValueError("walls/visited length mismatch")
            obs = build_global_obs(width, height, walls, visited, agent, goal)
        except (KeyError, ValueError, TypeError) as exc:
            self.send_error(400, f"Bad observation: {exc}")
            return

        with _infer_lock, torch.no_grad():
            state = torch.tensor(
                obs, dtype=torch.float32, device=_agent.device
            ).unsqueeze(0)
            logits = _agent.policy_net(state)
            action = int(logits.argmax(dim=1).item())

        body = json.dumps({"action": action}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):  # noqa: A003 - overriding stdlib
        print(f"[policy] {fmt % args}" if args else f"[policy] {fmt}")


def load_agent() -> DAggerAgent:
    """Load the checkpoint and return an inference-ready DAgger agent."""
    print(f"[policy] device={POLICY_DEVICE}")
    print(f"[policy] checkpoint={POLICY_CHECKPOINT}")

    ckpt = torch.load(
        POLICY_CHECKPOINT, map_location=POLICY_DEVICE, weights_only=True
    )
    obs_shape = tuple(ckpt.get("obs_shape", (7, 15, 21)))

    agent = DAggerAgent(
        obs_shape=obs_shape,
        n_actions=N_ACTIONS,
        hidden=POLICY_HIDDEN,
        device=POLICY_DEVICE,
    )
    agent.load(POLICY_CHECKPOINT)
    agent.policy_net.eval()
    print(
        f"[policy] loaded checkpoint (obs_shape={obs_shape}, "
        f"episodes={agent.episodes_done}, train_steps={agent.train_steps})"
    )
    return agent


def main():
    global _agent
    _agent = load_agent()

    server = HTTPServer(("0.0.0.0", POLICY_PORT), PolicyHandler)
    ctx = build_tls_context()
    server.socket = ctx.wrap_socket(server.socket, server_side=True)

    print(f"[policy] HTTPS server listening on https://0.0.0.0:{POLICY_PORT}/policy")
    print(f"[policy] TLS server cert={CERT_FILE} key={KEY_FILE}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[policy] Shutting down...")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
