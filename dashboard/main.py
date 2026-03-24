#!/usr/bin/env python3
import asyncio
import json
import os
import time
from datetime import datetime, timezone
from typing import List, Optional

from dotenv import load_dotenv
load_dotenv()

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Query
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pymongo import MongoClient, DESCENDING
from pymongo.errors import ConnectionFailure
import uvicorn

# ── Config ──────────────────────────────────────────────────────────────────
MONGO_URI      = os.getenv("MONGO_URI",      "mongodb://localhost:27017")
MONGO_DB       = os.getenv("MONGO_DB",       "maze")
MONGO_COL      = os.getenv("MONGO_COL",      "moves")
DASHBOARD_PORT = int(os.getenv("DASHBOARD_PORT", "8000"))
AI_SERVER_URL  = os.getenv("AI_SERVER_URL",  "")

# ── App ──────────────────────────────────────────────────────────────────────
from contextlib import asynccontextmanager

@asynccontextmanager
async def lifespan(app: FastAPI):
    asyncio.create_task(mongo_poller())
    print(f"[server] Dashboard API running at http://localhost:{DASHBOARD_PORT}")
    print(f"[server] Open dashboard: http://localhost:{DASHBOARD_PORT}/  (serve index.html separately)")
    print(f"[server] WebSocket:      ws://localhost:{DASHBOARD_PORT}/ws")
    print(f"[server] Health:         http://localhost:{DASHBOARD_PORT}/health")
    yield

app = FastAPI(title="Mini-Pupper Dashboard API", version="1.0.0", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ── MongoDB ──────────────────────────────────────────────────────────────────
mongo_client: Optional[MongoClient] = None
db = None
col = None
mongo_ok = False

def connect_mongo():
    global mongo_client, db, col, mongo_ok
    try:
        mongo_client = MongoClient(MONGO_URI, serverSelectionTimeoutMS=3000)
        mongo_client.admin.command("ping")
        db  = mongo_client[MONGO_DB]
        col = db[MONGO_COL]
        mongo_ok = True
        print(f"[db] Connected to MongoDB: {MONGO_URI}  db={MONGO_DB}  col={MONGO_COL}")
    except ConnectionFailure as e:
        mongo_ok = False
        print(f"[db] MongoDB connection failed: {e}")

connect_mongo()

# ── WebSocket Manager ────────────────────────────────────────────────────────
class ConnectionManager:
    def __init__(self):
        self.active: List[WebSocket] = []

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self.active.append(ws)
        print(f"[ws] Client connected — total: {len(self.active)}")

    def disconnect(self, ws: WebSocket):
        if ws in self.active:
            self.active.remove(ws)
        print(f"[ws] Client disconnected — total: {len(self.active)}")

    async def broadcast(self, data: dict):
        dead = []
        for ws in self.active:
            try:
                await ws.send_json(data)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.disconnect(ws)

manager = ConnectionManager()

# ── Helpers ──────────────────────────────────────────────────────────────────
def doc_to_json(doc: dict) -> dict:
    """Convert a MongoDB document to a JSON-serialisable dict."""
    out = {}
    for k, v in doc.items():
        if k == "_id":
            out["_id"] = str(v)
        elif hasattr(v, "isoformat"):
            out[k] = v.isoformat()
        else:
            out[k] = v
    return out

def compute_stats(docs: list) -> dict:
    """Compute summary stats from a list of move documents."""
    total     = len(docs)
    goal_hits = sum(1 for d in docs if d.get("goal_reached"))
    actions   = {}
    devices   = set()
    last_pos  = {"x": 0, "y": 0}
    last_ts   = None
    sessions  = set()

    for d in docs:
        action = d.get("action", d.get("event_type", "unknown"))
        actions[action] = actions.get(action, 0) + 1
        inp = d.get("input", {})
        if isinstance(inp, dict) and inp.get("device"):
            devices.add(inp["device"])
        pos = (d.get("player") or {}).get("position")
        if isinstance(pos, dict):
            last_pos = pos
        ts = d.get("timestamp") or d.get("received_at")
        if ts:
            last_ts = ts
        sid = d.get("session_id")
        if sid:
            sessions.add(sid)

    success_rate = round((goal_hits / total * 100) if total else 0, 1)

    return {
        "total_events":  total,
        "goal_reached":  goal_hits,
        "success_rate":  success_rate,
        "actions":       actions,
        "devices":       list(devices),
        "last_position": last_pos,
        "last_event_ts": last_ts,
        "sessions":      len(sessions),
        "forward":       actions.get("forward",    0),
        "backward":      actions.get("backward",   0),
        "turn_left":     actions.get("turn_left",  0),
        "turn_right":    actions.get("turn_right", 0),
        "stop":          actions.get("stop",       0),
    }

# ── REST Endpoints ────────────────────────────────────────────────────────────

@app.get("/health")
def health():
    """Health check — returns MongoDB + server status."""
    global mongo_ok
    if mongo_ok:
        try:
            mongo_client.admin.command("ping")
        except Exception:
            mongo_ok = False
    return {
        "status":    "ok",
        "mongo":     "connected" if mongo_ok else "disconnected",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "server":    f"http://localhost:{DASHBOARD_PORT}",
    }

@app.get("/moves")
def get_moves(
    limit: int = Query(100, ge=1, le=1000),
    sort:  str = Query("desc", pattern="^(asc|desc)$"),
    session_id: Optional[str] = Query(None),
):
    """Return recent telemetry events from MongoDB."""
    if not mongo_ok:
        return JSONResponse({"ok": False, "error": "MongoDB not connected"}, 500)
    try:
        filt = {}
        if session_id:
            filt["session_id"] = session_id
        direction = DESCENDING if sort == "desc" else 1
        cursor = col.find(filt).sort("_id", direction).limit(limit)
        docs = [doc_to_json(d) for d in cursor]
        return {"ok": True, "moves": docs, "count": len(docs)}
    except Exception as e:
        return JSONResponse({"ok": False, "error": str(e)}, 500)

@app.get("/stats")
def get_stats(limit: int = Query(500, ge=1, le=5000)):
    """Compute aggregate stats from the most recent telemetry events."""
    if not mongo_ok:
        return JSONResponse({"ok": False, "error": "MongoDB not connected"}, 500)
    try:
        cursor = col.find().sort("_id", DESCENDING).limit(limit)
        docs = list(cursor)
        stats = compute_stats(docs)
        stats["ok"] = True
        stats["mongo_status"] = "connected"
        return stats
    except Exception as e:
        return JSONResponse({"ok": False, "error": str(e)}, 500)

@app.get("/sessions")
def get_sessions():
    """Return distinct session IDs."""
    if not mongo_ok:
        return JSONResponse({"ok": False, "error": "MongoDB not connected"}, 500)
    try:
        sessions = col.distinct("session_id")
        sessions = [s for s in sessions if s]
        return {"ok": True, "sessions": sessions, "count": len(sessions)}
    except Exception as e:
        return JSONResponse({"ok": False, "error": str(e)}, 500)

@app.get("/ai/diagnostics")
def ai_diagnostics(query: str = Query("Summarize recent mission performance")):
    """
    AI diagnostics endpoint.
    If AI_SERVER_URL is set, proxies to it.
    Otherwise returns a local analysis of recent telemetry.
    """
    if not mongo_ok:
        return JSONResponse({"ok": False, "error": "MongoDB not connected"}, 500)
    try:
        cursor = col.find().sort("_id", DESCENDING).limit(200)
        docs = list(cursor)
        stats = compute_stats(docs)

        if AI_SERVER_URL:
            import requests
            resp = requests.post(
                f"{AI_SERVER_URL}/query",
                json={"query": query, "context": stats},
                timeout=10,
            )
            return resp.json()

        # Local diagnostic fallback
        total   = stats["total_events"]
        sr      = stats["success_rate"]
        top_act = max(stats["actions"], key=stats["actions"].get) if stats["actions"] else "none"
        diagnosis = (
            f"Analyzed {total} telemetry events. "
            f"Mission success rate: {sr}%. "
            f"Most frequent action: '{top_act}'. "
            f"Last known position: {stats['last_position']}. "
            f"Active sessions: {stats['sessions']}. "
        )
        if sr < 50:
            diagnosis += "⚠️  Success rate is below 50% — consider reviewing navigation logic."
        elif sr >= 90:
            diagnosis += "✅  Excellent mission performance."
        else:
            diagnosis += "ℹ️  Nominal performance."

        return {
            "ok":        True,
            "query":     query,
            "diagnosis": diagnosis,
            "stats":     stats,
            "source":    "local",
        }
    except Exception as e:
        return JSONResponse({"ok": False, "error": str(e)}, 500)

# ── WebSocket ─────────────────────────────────────────────────────────────────

@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await manager.connect(ws)
    # Send current stats immediately on connect
    try:
        if mongo_ok:
            cursor = col.find().sort("_id", DESCENDING).limit(500)
            docs = list(cursor)
            stats = compute_stats(docs)
            recent = [doc_to_json(d) for d in col.find().sort("_id", DESCENDING).limit(5)]
            await ws.send_json({
                "type":   "init",
                "stats":  stats,
                "recent": recent,
            })
        else:
            await ws.send_json({"type": "error", "msg": "MongoDB not connected"})
    except Exception as e:
        print(f"[ws] init error: {e}")

    try:
        while True:
            await ws.receive_text()   # keep-alive: client can send pings
    except WebSocketDisconnect:
        manager.disconnect(ws)

# ── Background poller: push updates every 2s ─────────────────────────────────

_last_seen_id = None

async def mongo_poller():
    global _last_seen_id, mongo_ok
    print("[poller] Starting MongoDB poll loop (2s interval)")
    while True:
        await asyncio.sleep(2)
        if not manager.active:
            continue
        try:
            if not mongo_ok:
                connect_mongo()
                if not mongo_ok:
                    await manager.broadcast({"type": "status", "mongo": "disconnected"})
                    continue

            # Find new documents since last seen
            filt = {}
            if _last_seen_id:
                from bson import ObjectId
                filt = {"_id": {"$gt": ObjectId(_last_seen_id)}}

            new_docs = list(col.find(filt).sort("_id", 1).limit(20))

            if new_docs:
                _last_seen_id = str(new_docs[-1]["_id"])
                json_docs = [doc_to_json(d) for d in new_docs]

                # Recompute stats
                cursor = col.find().sort("_id", DESCENDING).limit(500)
                all_docs = list(cursor)
                stats = compute_stats(all_docs)

                await manager.broadcast({
                    "type":       "update",
                    "new_events": json_docs,
                    "stats":      stats,
                    "mongo":      "connected",
                })
            else:
                # Heartbeat
                await manager.broadcast({
                    "type":  "heartbeat",
                    "mongo": "connected",
                    "ts":    datetime.now(timezone.utc).isoformat(),
                })
        except Exception as e:
            print(f"[poller] error: {e}")
            mongo_ok = False
            await manager.broadcast({"type": "status", "mongo": "disconnected"})



# ── Main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    uvicorn.run("main:app", host="0.0.0.0", port=DASHBOARD_PORT, reload=False)
