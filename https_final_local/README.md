# HTTPS Server (Local Setup)

Local version of the HTTPS mTLS server for development and testing. Connects to MongoDB via SSH tunnel instead of directly to the remote server.

## Prerequisites

Install dependencies (one time):

```bash
sudo apt install -y build-essential libmicrohttpd-dev libgnutls28-dev libmongoc-dev libbson-dev libhiredis-dev
```

## Build

```bash
cd https_final_local
make
```

## Full Setup (5 Terminals)

### Terminal 1 — SSH Tunnel to MongoDB

```bash
ssh -L 27017:localhost:27017 YOUR_USERID@10.170.8.130
```

Keep this open. It forwards your local port 27017 to the remote MongoDB server.

### Terminal 2 — Local HTTPS Server

```bash
cd https_final_local
./maze_https_local
```

Listens on `https://localhost:8445`. Receives telemetry from the maze game and writes to MongoDB via the SSH tunnel.

### Terminal 3 — Dashboard Backend

```bash
cd dashboard
source venv/bin/activate
MONGO_URI="mongodb://localhost:27017" MONGO_DB="maze" MONGO_COL="team3ttmoves" python3 main.py
```

Starts the FastAPI server on `http://localhost:8000`.

### Terminal 4 — Open Dashboard UI

```bash
explorer.exe "$(wslpath -w dashboard/index.html)"
```

Opens the dashboard in your Windows browser (WSL only).

### Terminal 5 — Run Maze Game

```bash
cd maze
TELEMETRY_URL="https://localhost:8445/move" MISSION_URL="https://localhost:8445/mission" ./maze_sdl2
```

Use arrow keys or WASD to move. Press M to toggle A* auto-solve.

## Data Flow

```
Maze Game (localhost)
  |  HTTPS POST (mTLS)
  v
Local HTTPS Server (localhost:8445)
  |  MongoDB write
  v
SSH Tunnel (localhost:27017 -> 10.170.8.130:27017)
  |
  v
Remote MongoDB
  |  MongoDB read
  v
Dashboard Backend (localhost:8000)
  |  WebSocket
  v
Dashboard UI (index.html)
```

## Useful Commands

Clear all telemetry data:

```bash
mongosh mongodb://localhost:27017/maze --eval 'db.team3ttmoves.deleteMany({})'
```

Health check:

```bash
curl --cacert certs/ca.crt --cert certs/client.crt --key certs/client.key https://localhost:8445/health
```

## Notes

- The robot timeout errors are expected if you're not connected to the physical Mini-Pupper
- Redis connection is optional — mission stats won't show if Redis isn't running, but everything else works
- Make sure the SSH tunnel is running before starting the HTTPS server or dashboard
