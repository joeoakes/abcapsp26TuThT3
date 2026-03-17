#!/usr/bin/env python3
"""
robot_bridge.py — HTTPS command bridge for ROS2 Mini-Pupper.

Receives movement commands from the maze game over HTTPS and publishes
Twist messages to the ROS2 /cmd_vel topic.

Usage:
    source /opt/ros/<distro>/setup.bash
    python3 robot_bridge.py

Environment variables:
    ROBOT_PORT      — HTTPS listen port          (default: 8445)
    SPEED           — linear.x magnitude m/s     (default: 0.5)
    TURN_SPEED      — angular.z magnitude rad/s  (default: 1.0)
    MOVE_DURATION   — seconds per movement burst  (default: 0.5)
    CERT_FILE       — path to server TLS cert     (default: certs/server.crt)
    KEY_FILE        — path to server TLS key      (default: certs/server.key)
    CA_CERT_FILE    — trusted client CA cert      (default: certs/ca.crt)

Accepted POST /robot JSON body:
    {"action": "forward" | "backward" | "turn_left" | "turn_right" | "stop"}
"""

import json
import os
import ssl
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler

import rclpy
from rclpy.node import Node
from geometry_msgs.msg import Twist

# Configuration (override via environment variables)
ROBOT_PORT    = int(os.environ.get("ROBOT_PORT", "8445"))
SPEED         = float(os.environ.get("SPEED", "0.5"))
TURN_SPEED    = float(os.environ.get("TURN_SPEED", "1.0"))
MOVE_DURATION = float(os.environ.get("MOVE_DURATION", "0.5"))
CERT_FILE     = os.environ.get("CERT_FILE", "certs/server.crt")
KEY_FILE      = os.environ.get("KEY_FILE", "certs/server.key")
CA_CERT_FILE  = os.environ.get("CA_CERT_FILE", "certs/ca.crt")

# ROS2 publisher (singleton, created in main)
_ros_node = None       # type: Node | None
_cmd_pub  = None       # type: rclpy.publisher.Publisher | None
_move_lock = threading.Lock()


def build_tls_context() -> ssl.SSLContext:
    """Create a hardened TLS context that enforces mTLS client auth."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(certfile=CERT_FILE, keyfile=KEY_FILE)
    ctx.load_verify_locations(cafile=CA_CERT_FILE)
    ctx.verify_mode = ssl.CERT_REQUIRED
    return ctx


def _publish_twist(linear_x: float, angular_z: float) -> None:
    """Publish a single Twist message."""
    if _cmd_pub is None:
        return
    twist = Twist()
    twist.linear.x = linear_x
    twist.angular.z = angular_z
    _cmd_pub.publish(twist)


def _stop() -> None:
    """Publish a zero-velocity Twist (stop)."""
    _publish_twist(0.0, 0.0)


def execute_action(action: str) -> None:
    """
    Execute a movement action: publish velocity for MOVE_DURATION seconds,
    then stop.  Runs behind _move_lock so commands don't overlap.
    """
    mapping = {
        "forward":    (SPEED,  0.0),
        "backward":   (-SPEED, 0.0),
        "turn_left":  (0.0,    TURN_SPEED),
        "turn_right": (0.0,   -TURN_SPEED),
        "stop":       (0.0,    0.0),
    }

    velocities = mapping.get(action)
    if velocities is None:
        print(f"[bridge] unknown action: {action}")
        return

    with _move_lock:
        linear_x, angular_z = velocities
        print(f"[bridge] action={action}  lin_x={linear_x}  ang_z={angular_z}  dur={MOVE_DURATION}s")
        if action == "stop":
            _publish_twist(0.0, 0.0)
        else:
            # Test (publish repeatedly at ~20 Hz so the motor controller sees a continuous velocity stream for the full duration)
            rate_hz = 20.0
            interval = 1.0 / rate_hz
            elapsed = 0.0
            while elapsed < MOVE_DURATION:
                _publish_twist(linear_x, angular_z)
                time.sleep(interval)
                elapsed += interval
            _stop()


# HTTPS request handler
class RobotHandler(BaseHTTPRequestHandler):
    """Handle POST /robot commands from the maze client."""

    def do_POST(self):
        if self.path != "/robot":
            self.send_error(404, "Not Found")
            return

        content_length = int(self.headers.get("Content-Length", 0))
        if content_length == 0:
            self.send_error(400, "Empty body")
            return

        body = self.rfile.read(content_length)
        try:
            data = json.loads(body)
        except json.JSONDecodeError:
            self.send_error(400, "Invalid JSON")
            return

        action = data.get("action")
        if not action:
            self.send_error(400, "Missing 'action' field")
            return

        # Execute in a thread so the HTTP response returns immediately
        threading.Thread(target=execute_action, args=(action,), daemon=True).start()

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"status":"ok"}\n')

    def log_message(self, format, *args):
        """Override to use [bridge] prefix."""
        print(f"[bridge] {args[0]} {args[1]} {args[2]}")


def main():
    global _ros_node, _cmd_pub

    # ROS2 init
    rclpy.init()
    _ros_node = Node("maze_robot_bridge")
    _cmd_pub = _ros_node.create_publisher(Twist, "cmd_vel", 10)
    print(f"[bridge] ROS2 node 'maze_robot_bridge' ready, publishing on /cmd_vel")

    # Spin ROS2 in a background thread so callbacks work
    ros_thread = threading.Thread(target=rclpy.spin, args=(_ros_node,), daemon=True)
    ros_thread.start()

    # HTTPS server
    server = HTTPServer(("0.0.0.0", ROBOT_PORT), RobotHandler)

    # Wrap socket with TLS + required client certificate verification (mTLS)
    ctx = build_tls_context()
    server.socket = ctx.wrap_socket(server.socket, server_side=True)

    print(f"[bridge] HTTPS server listening on https://0.0.0.0:{ROBOT_PORT}/robot")
    print(f"[bridge] speed={SPEED}  turn={TURN_SPEED}  move_duration={MOVE_DURATION}s")
    print(f"[bridge] TLS server cert={CERT_FILE} key={KEY_FILE}")
    print(f"[bridge] mTLS trusted client CA={CA_CERT_FILE}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] Shutting down...")
    finally:
        _stop()
        server.server_close()
        _ros_node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
