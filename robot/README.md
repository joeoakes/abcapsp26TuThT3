# Robot Bridge — Maze → ROS2 Mini-Pupper

HTTPS server that receives movement commands from the maze game and publishes `Twist` messages to the ROS2 `/cmd_vel` topic on the Mini-Pupper.

## Prerequisites

- **ROS2** (Humble or later) installed and sourced
- `geometry_msgs` package: `sudo apt install ros-humble-geometry-msgs`
- TLS certificate + key for HTTPS (see [Generating Certs](#generating-self-signed-certs))

## Quick Start

```bash
# On the Mini-Pupper
source /opt/ros/humble/setup.bash

# Generate self-signed certs (first time only)
mkdir -p certs
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout certs/server.key -out certs/server.crt \
  -days 365 -subj "/CN=mini-pupper"

# Run the bridge
python3 robot_bridge.py
```

## Environment Variables

| Variable        | Default             | Description                          |
|-----------------|---------------------|--------------------------------------|
| `ROBOT_PORT`    | `8446`              | HTTPS listen port                    |
| `SPEED`         | `0.5`               | Linear velocity (m/s)                |
| `TURN_SPEED`    | `1.0`               | Angular velocity (rad/s)             |
| `MOVE_DURATION` | `0.5`               | Seconds per movement burst           |
| `CERT_FILE`     | `certs/server.crt`  | TLS certificate path                 |
| `KEY_FILE`      | `certs/server.key`  | TLS private key path                 |

## Testing

```bash
# POST a test command
curl -k -X POST https://localhost:8446/robot \
  -H "Content-Type: application/json" \
  -d '{"action":"forward"}'

# Watch cmd_vel in another terminal
ros2 topic echo /cmd_vel
```

## Actions

| Action        | Effect                                    |
|---------------|-------------------------------------------|
| `forward`     | `linear.x = +SPEED` for `MOVE_DURATION`s  |
| `backward`    | `linear.x = -SPEED` for `MOVE_DURATION`s  |
| `turn_left`   | `angular.z = +TURN_SPEED` for duration    |
| `turn_right`  | `angular.z = -TURN_SPEED` for duration    |
| `stop`        | All velocities to zero immediately        |

## Maze Client Setup

On the machine running `maze_sdl2`, set the robot URL:

```bash
export ROBOT_URL="https://10.170.8.120:8446/robot"
./maze_sdl2
```

Each maze move will send a command to the Mini-Pupper.
