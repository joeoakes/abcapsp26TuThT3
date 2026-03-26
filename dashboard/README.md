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
pip install fastapi uvicorn pymongo python-dotenv requests websockets
```
 
### Step 2 — Open SSH tunnel to logging server
 
Open a dedicated terminal and keep it running:
 
```bash
ssh -L 27017:localhost:27017 'USERID'@10.170.8.130
```
 
This tunnels MongoDB (port 27017) from the logging server to your local machine.
 
### Step 3 — Open and Start the dashboard backend
 
In a second terminal:
 
```bash
cd dashboard
source venv/bin/activate
open /Users/alicia/abcapsp26TuThT3-main/dashboard/index.html
MONGO_URI="mongodb://localhost:27017" MONGO_DB="maze" MONGO_COL="moves" python3 main.py
```
 
You should see:
 
```
[db] Connected to MongoDB: mongodb://localhost:27017
[server] Dashboard API running at http://localhost:8000
[poller] Starting MongoDB poll loop (2s interval)
```

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
