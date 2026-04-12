import importlib.util
import io
import math
import sys
import types
from pathlib import Path

import pytest


@pytest.fixture
def robot_bridge_module(monkeypatch):
    fake_rclpy = types.ModuleType("rclpy")
    fake_rclpy.init = lambda: None
    fake_rclpy.shutdown = lambda: None
    fake_rclpy.spin = lambda node: None

    fake_rclpy_node = types.ModuleType("rclpy.node")

    class FakeNode:
        def __init__(self, name):
            self.name = name

        def create_publisher(self, msg_type, topic, queue_size):
            return types.SimpleNamespace(msg_type=msg_type, topic=topic, queue_size=queue_size)

        def destroy_node(self):
            return None

    fake_rclpy_node.Node = FakeNode
    fake_rclpy.node = fake_rclpy_node

    fake_geometry_msgs = types.ModuleType("geometry_msgs")
    fake_geometry_msgs_msg = types.ModuleType("geometry_msgs.msg")

    class FakeTwist:
        def __init__(self):
            self.linear = types.SimpleNamespace(x=0.0)
            self.angular = types.SimpleNamespace(z=0.0)

    fake_geometry_msgs_msg.Twist = FakeTwist
    fake_geometry_msgs.msg = fake_geometry_msgs_msg

    monkeypatch.setitem(sys.modules, "rclpy", fake_rclpy)
    monkeypatch.setitem(sys.modules, "rclpy.node", fake_rclpy_node)
    monkeypatch.setitem(sys.modules, "geometry_msgs", fake_geometry_msgs)
    monkeypatch.setitem(sys.modules, "geometry_msgs.msg", fake_geometry_msgs_msg)

    module_path = Path(__file__).resolve().parents[3] / "robot" / "robot_bridge.py"
    module_name = "robot_bridge_under_test"
    spec = importlib.util.spec_from_file_location(module_name, module_path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
        yield module
    finally:
        sys.modules.pop(module_name, None)


class RecordingPublisher:
    def __init__(self):
        self.messages = []

    def publish(self, twist):
        self.messages.append((twist.linear.x, twist.angular.z))


class FakeSSLContext:
    def __init__(self, protocol):
        self.protocol = protocol
        self.minimum_version = None
        self.verify_mode = None
        self.cert_chain = None
        self.verify_locations = None

    def load_cert_chain(self, certfile, keyfile):
        self.cert_chain = (certfile, keyfile)

    def load_verify_locations(self, cafile):
        self.verify_locations = cafile


def make_handler(module, path, body, monkeypatch):
    handler = module.RobotHandler.__new__(module.RobotHandler)
    handler.path = path
    handler.headers = {"Content-Length": str(len(body))}
    handler.rfile = io.BytesIO(body)
    handler.wfile = io.BytesIO()
    handler.errors = []
    handler.responses = []
    handler.response_headers = []
    handler.ended = False
    monkeypatch.setattr(handler, "send_error", lambda code, message: handler.errors.append((code, message)))
    monkeypatch.setattr(handler, "send_response", lambda code: handler.responses.append(code))
    monkeypatch.setattr(handler, "send_header", lambda key, value: handler.response_headers.append((key, value)))
    monkeypatch.setattr(handler, "end_headers", lambda: setattr(handler, "ended", True))
    return handler


# B2-49
def test_build_tls_context_configures_mtls(robot_bridge_module, monkeypatch):
    fake_ctx = FakeSSLContext(robot_bridge_module.ssl.PROTOCOL_TLS_SERVER)
    monkeypatch.setattr(robot_bridge_module.ssl, "SSLContext", lambda protocol: fake_ctx)
    monkeypatch.setattr(robot_bridge_module, "CERT_FILE", "server.crt")
    monkeypatch.setattr(robot_bridge_module, "KEY_FILE", "server.key")
    monkeypatch.setattr(robot_bridge_module, "CA_CERT_FILE", "ca.crt")

    ctx = robot_bridge_module.build_tls_context()

    assert ctx is fake_ctx
    assert fake_ctx.protocol == robot_bridge_module.ssl.PROTOCOL_TLS_SERVER
    assert fake_ctx.minimum_version == robot_bridge_module.ssl.TLSVersion.TLSv1_2
    assert fake_ctx.cert_chain == ("server.crt", "server.key")
    assert fake_ctx.verify_locations == "ca.crt"
    assert fake_ctx.verify_mode == robot_bridge_module.ssl.CERT_REQUIRED


# B2-50
def test_publish_twist_noops_without_publisher(robot_bridge_module):
    robot_bridge_module._cmd_pub = None

    robot_bridge_module._publish_twist(1.0, -0.5)

    assert robot_bridge_module._cmd_pub is None


# B2-51
def test_publish_twist_sends_expected_message(robot_bridge_module):
    publisher = RecordingPublisher()
    robot_bridge_module._cmd_pub = publisher

    robot_bridge_module._publish_twist(1.25, -0.75)

    assert publisher.messages == [(1.25, -0.75)]


# B2-52
def test_stop_publishes_zero_velocity(robot_bridge_module, monkeypatch):
    recorded = []
    monkeypatch.setattr(robot_bridge_module, "_publish_twist", lambda linear_x, angular_z: recorded.append((linear_x, angular_z)))

    robot_bridge_module._stop()

    assert recorded == [(0.0, 0.0)]


# B2-53
def test_execute_action_ignores_unknown_action(robot_bridge_module, monkeypatch):
    recorded = []
    monkeypatch.setattr(robot_bridge_module, "_publish_twist", lambda linear_x, angular_z: recorded.append((linear_x, angular_z)))
    robot_bridge_module._last_action = "stop"

    robot_bridge_module.execute_action("jump")

    assert recorded == []
    assert robot_bridge_module._last_action == "stop"


# B2-54
def test_execute_action_forward_publishes_motion_then_stop(robot_bridge_module, monkeypatch):
    recorded = []
    sleep_calls = []
    monkeypatch.setattr(robot_bridge_module, "_publish_twist", lambda linear_x, angular_z: recorded.append((linear_x, angular_z)))
    monkeypatch.setattr(robot_bridge_module.time, "sleep", lambda duration: sleep_calls.append(duration))
    monkeypatch.setattr(robot_bridge_module, "MOVE_DURATION", 0.11)
    monkeypatch.setattr(robot_bridge_module, "SPEED", 1.5)
    robot_bridge_module._last_action = "stop"

    robot_bridge_module.execute_action("forward")

    assert recorded[:-1] == [(1.5, 0.0), (1.5, 0.0), (1.5, 0.0)]
    assert recorded[-1] == (0.0, 0.0)
    assert sleep_calls == [0.05, 0.05, 0.05]
    assert robot_bridge_module._last_action == "forward"


# B2-55
def test_execute_action_turn_uses_turn_duration_formula(robot_bridge_module, monkeypatch):
    recorded = []
    sleep_calls = []
    monkeypatch.setattr(robot_bridge_module, "_publish_twist", lambda linear_x, angular_z: recorded.append((linear_x, angular_z)))
    monkeypatch.setattr(robot_bridge_module.time, "sleep", lambda duration: sleep_calls.append(duration))
    monkeypatch.setattr(robot_bridge_module, "TURN_SPEED", 2.0)
    monkeypatch.setattr(robot_bridge_module, "TURN_ANGLE_MULT", 1.0)
    robot_bridge_module._last_action = "stop"

    robot_bridge_module.execute_action("turn_left")

    expected_duration = math.radians(90.0) / 2.0
    expected_publish_count = math.ceil(expected_duration / 0.05)
    assert recorded[:-1] == [(0.0, 2.0)] * expected_publish_count
    assert recorded[-1] == (0.0, 0.0)
    assert sleep_calls == [0.05] * expected_publish_count
    assert robot_bridge_module._last_action == "turn_left"


# B2-56
def test_execute_action_waits_before_forward_after_turn(robot_bridge_module, monkeypatch):
    recorded = []
    sleep_calls = []
    monkeypatch.setattr(robot_bridge_module, "_publish_twist", lambda linear_x, angular_z: recorded.append((linear_x, angular_z)))
    monkeypatch.setattr(robot_bridge_module.time, "sleep", lambda duration: sleep_calls.append(duration))
    monkeypatch.setattr(robot_bridge_module, "TURN_MOVE_DELAY", 0.3)
    monkeypatch.setattr(robot_bridge_module, "MOVE_DURATION", 0.1)
    monkeypatch.setattr(robot_bridge_module, "SPEED", 0.8)
    robot_bridge_module._last_action = "turn_right"

    robot_bridge_module.execute_action("forward")

    assert sleep_calls[0] == 0.3
    assert sleep_calls[1:] == [0.05, 0.05]
    assert recorded[:-1] == [(0.8, 0.0), (0.8, 0.0)]
    assert recorded[-1] == (0.0, 0.0)
    assert robot_bridge_module._last_action == "forward"


# B2-57
def test_execute_action_stop_publishes_zero_once(robot_bridge_module, monkeypatch):
    recorded = []
    monkeypatch.setattr(robot_bridge_module, "_publish_twist", lambda linear_x, angular_z: recorded.append((linear_x, angular_z)))
    robot_bridge_module._last_action = "forward"

    robot_bridge_module.execute_action("stop")

    assert recorded == [(0.0, 0.0)]
    assert robot_bridge_module._last_action == "stop"


# B2-58
def test_do_post_rejects_non_robot_path(robot_bridge_module, monkeypatch):
    handler = make_handler(robot_bridge_module, "/bad", b"{}", monkeypatch)

    robot_bridge_module.RobotHandler.do_POST(handler)

    assert handler.errors == [(404, "Not Found")]


# B2-59
def test_do_post_rejects_empty_body(robot_bridge_module, monkeypatch):
    handler = make_handler(robot_bridge_module, "/robot", b"", monkeypatch)

    robot_bridge_module.RobotHandler.do_POST(handler)

    assert handler.errors == [(400, "Empty body")]


# B2-60
def test_do_post_rejects_invalid_json(robot_bridge_module, monkeypatch):
    handler = make_handler(robot_bridge_module, "/robot", b"{", monkeypatch)

    robot_bridge_module.RobotHandler.do_POST(handler)

    assert handler.errors == [(400, "Invalid JSON")]


# B2-61
def test_do_post_rejects_missing_action_field(robot_bridge_module, monkeypatch):
    handler = make_handler(robot_bridge_module, "/robot", b'{"foo":"bar"}', monkeypatch)

    robot_bridge_module.RobotHandler.do_POST(handler)

    assert handler.errors == [(400, "Missing 'action' field")]


# B2-62
def test_do_post_starts_background_action_and_returns_json(robot_bridge_module, monkeypatch):
    handler = make_handler(robot_bridge_module, "/robot", b'{"action":"forward"}', monkeypatch)
    thread_calls = []

    class FakeThread:
        def __init__(self, target, args, daemon):
            self.target = target
            self.args = args
            self.daemon = daemon
            thread_calls.append((target, args, daemon, False))

        def start(self):
            target, args, daemon, _ = thread_calls[-1]
            thread_calls[-1] = (target, args, daemon, True)

    monkeypatch.setattr(robot_bridge_module.threading, "Thread", FakeThread)

    robot_bridge_module.RobotHandler.do_POST(handler)

    assert thread_calls == [(robot_bridge_module.execute_action, ("forward",), True, True)]
    assert handler.responses == [200]
    assert handler.response_headers == [("Content-Type", "application/json")]
    assert handler.ended is True
    assert handler.wfile.getvalue() == b'{"status":"ok"}\n'
