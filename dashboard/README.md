# 🐾 Team 3 – Mini-Pupper Mission Dashboard

## Overview

This folder contains the live mission dashboard for the Team 3 Mini-Pupper system.

The dashboard provides real-time visualization of:

- Mission status
- Telemetry data
- Robot movement information
- AI and logging system health

---

## How to Run

## Setup
 
### Step 1 — Create a virtual environment (first time only)

```bash
cd dashboard
python3 -m venv venv
source venv/bin/activate
pip install fastapi uvicorn pymongo python-dotenv requests websockets redis
```

### Step 2 — Open SSH tunnels to logging server

You need three tunnels: MongoDB, Redis, and the HTTPS telemetry receiver. Open each in its own terminal and keep them all running:

```bash
# MongoDB
ssh -L 27017:localhost:27017 USERID@10.170.8.130

# Redis (in a separate terminal)
ssh -L 6379:localhost:6379 USERID@10.170.8.130

# HTTPS telemetry receiver (in a separate terminal)
ssh -L 8445:localhost:8445 USERID@10.170.8.130
```

The 8445 tunnel is required when the receiver's TLS certificate has `CN=localhost` only — connecting directly to the server's IP would fail mTLS hostname verification. Tunneling lets your local maze client talk to `https://localhost:8445/...`, which matches the cert.

**Skip the 8445 tunnel if a receiver with an IP-SAN cert is already running.** From the repo root, run:

```bash
curl -s --cacert https_final/certs/ca.crt --cert https_final/certs/client.crt --key https_final/certs/client.key https://10.170.8.130:8445/health
```

If you get `{"ok":true}`, you're good — no tunnel needed. If you get a `subjectAltName does not match` error or a connection refused, set up the 8445 tunnel as shown above.

### Step 3 — Start the dashboard backend

In another terminal:

```bash
cd dashboard
source venv/bin/activate
MONGO_URI="mongodb://localhost:27017" MONGO_DB="maze" MONGO_COL="team3ttmoves" python3 main.py
```

You should see:

```
[db] Connected to MongoDB: mongodb://localhost:27017  db=maze  col=team3ttmoves
[redis] Connected to Redis: localhost:6379
[server] Dashboard API running at http://localhost:8000
[poller] Starting MongoDB poll loop (2s interval)
```

> **Note on collection name:** the HTTPS receiver (`https_final/maze_https_final`) writes telemetry to the `team3ttmoves` collection by default. If you point the dashboard at the older `moves` collection you will see no live data.

### Step 4 — Serve the dashboard UI

`main.py` is API-only and does not serve `index.html`. In yet another terminal:

```bash
cd dashboard
python3 -m http.server 8080
```

Then open <http://localhost:8080/index.html> in your browser. The page talks to the backend on `localhost:8000` for data and to `localhost:8000/ws` for live updates.

---

## System Architecture Context

Telemetry flow in our system:

```
GameHat Maze App
↓ HTTPS (Port 8445)
Mini-Pupper Telemetry Receiver
↓
Logging Server (MongoDB) — 10.170.8.130
↓
main.py (reads MongoDB via SSH tunnel, pushes via WebSocket)
↓
Dashboard (index.html)
↓
AI Server — 10.170.8.109
```

---

## Dashboard Sections

### 1. Status Overview

- Mission Status
- Mission Timer (Session Runtime)
- Total Telemetry Events
- Last Event Timestamp
- Secure Connection Indicator (HTTPS / mTLS)

### 2. Telemetry Summary

- Total Commands
- Successful Moves
- Failed Commands
- Success Rate

### 3. Movement & Position

- Current (X, Y) Maze Coordinates
- Direction
- Goal Reached Indicator
- Live Maze Map Visualization

### 4. AI & Logging

- MongoDB Status
- AI Server Status
- Secure Channel Status (HTTPS)
- mTLS Status
- WebSocket Status
- Robot Bridge Status
- Last Model Output

### 5. Charts

- Action Distribution (pie chart)
- Event Timeline (bar chart)
- Live Telemetry Feed

---

## Security Representation

The dashboard reflects the system's secure telemetry design:

- HTTPS is used for telemetry transmission
- mTLS support is enforced on port 8445
- MongoDB is accessed via SSH tunnel only — never exposed publicly
- Secure connection status is displayed in the UI

---

## Current Status

This is a live integrated dashboard.

- ✅ Live MongoDB connection via SSH tunnel
- ✅ Real-time telemetry feed via WebSocket
- ✅ Live maze map from player position data
- ✅ AI diagnostics endpoint (local fallback if AI server offline)

---

## Team 3 – CMPSC Project

Mini-Pupper + Maze App + Telemetry + AI Integration

---
## Current Status

![Mission Dashboard Preview](dashboard.png)
![Mission Dashboard Preview](dashboard.png.2.png)
