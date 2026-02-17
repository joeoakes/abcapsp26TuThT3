# redis_https
C HTTPS + mTLS JSON -> Redis mission storage.

This folder is a Redis variant of the HTTPS server so the original `https/` Mongo flow stays available.

## Endpoints
- `POST /mission` (primary)
- `POST /move` (compatibility alias)
- `GET /health`

## Expected JSON payload
```json
{
  "mission_id": "TEST_MISSION",
  "robot_id": "TEST_ROBOT",
  "mission_type": "patrol",
  "start_time": 1770056813,
  "end_time": 1770056848,
  "moves_left_turn": 46,
  "moves_right_turn": 46,
  "moves_straight": 52,
  "moves_reverse": 8,
  "moves_total": 152,
  "distance_traveled": 24.41,
  "duration_seconds": 35,
  "mission_result": "success",
  "abort_reason": "user exited"
}
```

Stored Redis key format:
- `team3ttmission:<mission_id>`

## Env vars
- `REDIS_HOST` (default `127.0.0.1`)
- `REDIS_PORT` (default `6379`)
- `REDIS_KEY_PREFIX` (default `team3ttmission`)
- `CERT_FILE`, `KEY_FILE`, `CA_CERT_FILE` (optional cert path overrides)

## Build (Linux/WSL)
Install deps:
```bash
sudo apt update
sudo apt install -y build-essential pkg-config libmicrohttpd-dev libgnutls28-dev libbson-dev libhiredis-dev
```

Compile:
```bash
gcc -O2 -Wall -Wextra -std=c11 maze_https_mongo.c -o maze_https_redis \
  $(pkg-config --cflags --libs libmicrohttpd gnutls libbson-1.0 hiredis)
```

## Run
```bash
./maze_https_redis
```

## Test
```bash
curl -sS --cacert certs/ca.crt --cert certs/client.crt --key certs/client.key \
  -X POST https://localhost:8445/mission \
  -H "Content-Type: application/json" \
  -d '{"mission_id":"TEST_MISSION","robot_id":"TEST_ROBOT","mission_type":"patrol","start_time":1770056813,"end_time":1770056848,"moves_left_turn":46,"moves_right_turn":46,"moves_straight":52,"moves_reverse":8,"moves_total":152,"distance_traveled":24.41,"duration_seconds":35,"mission_result":"success","abort_reason":"user exited"}'
```

Verify in Redis:
```bash
redis-cli HGETALL team3ttmission:TEST_MISSION
```
