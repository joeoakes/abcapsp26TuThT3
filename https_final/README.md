# https_final
Combined HTTPS + mTLS server with both data paths:
- Telemetry -> MongoDB
- Mission summary -> Redis

## Routes
- `POST /move` -> store telemetry JSON in MongoDB
- `GET /moves?limit=100&sort=desc&session_id=<id>` -> read telemetry from MongoDB
- `POST /mission` -> store mission JSON in Redis hash
- `GET /mission?mission_id=<id>` -> read mission JSON from Redis hash
- `GET /health` -> health check

## Defaults / env
- `MONGO_URI` (default `mongodb://localhost:27017`)
- `MONGO_DB` (default `maze`)
- `MONGO_COL` (default `moves`)
- `REDIS_HOST` (default `127.0.0.1`)
- `REDIS_PORT` (default `6379`)
- `REDIS_KEY_PREFIX` (default `team3ttmission`)
- `CERT_FILE`, `KEY_FILE`, `CA_CERT_FILE`

## Build (one line)
```bash
PKG_CONFIG_PATH=$HOME/local/lib/pkgconfig gcc -O2 -Wall -Wextra -std=c11 maze_https_mongo.c -o maze_https_final $(pkg-config --cflags --libs libmicrohttpd gnutls libbson-1.0 libmongoc-1.0) -I$HOME/local/include -L$HOME/local/lib -lhiredis -Wl,-rpath,$HOME/local/lib
```

## Run
```bash
MONGO_URI="mongodb://localhost:27017" REDIS_HOST="127.0.0.1" REDIS_PORT="6379" REDIS_KEY_PREFIX="team3ttmission" ./maze_https_final
```

## One-line test commands
Telemetry insert (Mongo):
```bash
curl --cacert certs/ca.crt --cert certs/client.crt --key certs/client.key -X POST https://localhost:8445/move -H "Content-Type: application/json" -d '{"event_type":"player_move","session_id":"S1","input":{"device":"keyboard","move_sequence":1},"player":{"position":{"x":1,"y":0}},"goal_reached":false,"timestamp":"2026-02-17T12:00:00Z"}'
```

Telemetry read (Mongo):
```bash
curl --cacert certs/ca.crt --cert certs/client.crt --key certs/client.key "https://localhost:8445/moves?limit=10&sort=desc"
```

Mission insert (Redis):
```bash
curl --cacert certs/ca.crt --cert certs/client.crt --key certs/client.key -X POST https://localhost:8445/mission -H "Content-Type: application/json" -d '{"mission_id":"TEST_MISSION","robot_id":"TEST_ROBOT","mission_type":"patrol","start_time":1770056813,"end_time":1770056848,"moves_left_turn":46,"moves_right_turn":46,"moves_straight":52,"moves_reverse":8,"moves_total":152,"distance_traveled":24.41,"duration_seconds":35,"mission_result":"success","abort_reason":"user exited"}'
```

Mission read (Redis via API):
```bash
curl --cacert certs/ca.crt --cert certs/client.crt --key certs/client.key "https://localhost:8445/mission?mission_id=TEST_MISSION"
```

Mission read (direct Redis):
```bash
redis-cli HGETALL team3ttmission:TEST_MISSION
```
