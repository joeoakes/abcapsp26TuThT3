# Robot Bridge — Maze → ROS2 Mini-Pupper

HTTPS server that receives movement commands from the maze game and publishes `Twist` messages to the ROS2 `/cmd_vel` topic on the Mini-Pupper.

## Startup

```bash
# On the Mini-Pupper
# (First time) create the certs directory
mkdir -p certs

# Run the bridge
python3 robot_bridge.py
```

## mTLS Certificate Setup (Required)

`robot_bridge.py` now enforces mTLS. The client (maze app) must present a cert
signed by the CA that the robot bridge trusts.

Run these commands once (from `robot/`):

```bash
mkdir -p certs

# 1) Create a local CA
openssl genrsa -out certs/ca.key 4096
openssl req -x509 -new -nodes -key certs/ca.key -sha256 -days 3650 -subj "/CN=maze-robot-ca" -out certs/ca.crt

# 2) Create robot server cert/key
ROBOT_IP="10.170.9.185"
openssl genrsa -out certs/server.key 4096
openssl req -new -key certs/server.key -subj "/CN=mini-pupper" -out certs/server.csr
openssl x509 -req -in certs/server.csr -CA certs/ca.crt -CAkey certs/ca.key -CAcreateserial -out certs/server.crt -days 825 -sha256 -extfile <(printf "subjectAltName=IP:%s" "$ROBOT_IP")

# 3) Create maze client cert/key
openssl genrsa -out certs/client.key 4096
openssl req -new -key certs/client.key -subj "/CN=maze-client" -out certs/client.csr
openssl x509 -req -in certs/client.csr -CA certs/ca.crt -CAkey certs/ca.key -CAcreateserial -out certs/client.crt -days 825 -sha256
```

Distribute certs securely:

- Keep private keys private:
  - Robot needs `certs/server.key`
  - Maze machine needs `client.key`
- Copy trust/material to maze machine for robot link:
  - `ca.crt`
  - `client.crt`
  - `client.key`

## Environment Variables

| Variable          | Default             | Description                          |
|-------------------|---------------------|--------------------------------------|
| `ROBOT_PORT`      | `8445`              | HTTPS listen port                    |
| `SPEED`           | `1.0`               | Linear velocity (m/s)                |
| `TURN_SPEED`      | `1.0`               | Angular velocity (rad/s)             |
| `TURN_ANGLE_MULT` | `2.0`               | Turning duration multiplier          |
| `TURN_MOVE_DELAY` | `0.5`               | Delay after turning before moving    |
| `MOVE_DURATION`   | `2.0`               | Seconds per movement burst           |
| `CERT_FILE`       | `certs/server.crt`  | TLS certificate path                 |
| `KEY_FILE`        | `certs/server.key`  | TLS private key path                 |
| `CA_CERT_FILE`    | `certs/ca.crt`      | Trusted CA for client-cert verify    |

## Testing

```bash
# POST a test command
curl --cacert certs/ca.crt --cert certs/client.crt --key certs/client.key -X POST https://mini-pupper:8445/robot -H "Content-Type: application/json" -d '{"action":"forward"}'

# Can also watch for cmd_vel in another terminal
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

On the machine running `maze_sdl2`, you can optionally override the robot URL + credentials:

```bash
export ROBOT_URL="https://10.170.9.185:8445/robot"
export ROBOT_CA_CERT="../robot/certs/ca.crt"
export ROBOT_CLIENT_CERT="../robot/certs/client.crt"
export ROBOT_CLIENT_KEY="../robot/certs/client.key"
./maze_sdl2
```

`ROBOT_CA_CERT`, `ROBOT_CLIENT_CERT`, and `ROBOT_CLIENT_KEY` will by default to point to `../robot/certs/`.

Each valid maze move will send a command to the Mini-Pupper over verified mTLS.

