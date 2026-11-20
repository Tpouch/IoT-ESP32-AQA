#!/usr/bin/env python3
"""Reference implementation of the AQA HTTP API (docs/api.md), for local tests.

Standard library only, data in SQLite. Not hardened for the internet: use it
on your LAN to test a board, or as a starting point for the real app.

    python3 tools/mock_api.py --token dev-token            # http://0.0.0.0:8000
    # .env on the board:  API_URL=http://<your-pc-ip>:8000   DEVICE_TOKEN=dev-token

Device routes (used by the firmware):
    POST /api/v1/devices/{id}/status
    POST /api/v1/devices/{id}/frames
    POST /api/v1/devices/{id}/commands/{command_id}/ack
App routes (suggested in docs/api.md, not used by the firmware):
    GET  /api/v1/devices
    GET  /api/v1/devices/{id}
    GET  /api/v1/devices/{id}/frames?from=<epoch>&to=<epoch>&limit=<n>
    POST /api/v1/devices/{id}/commands        {"type": "...", "params": {...}}
    GET  /api/v1/devices/{id}/commands
"""

import argparse
import json
import re
import sqlite3
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
    id TEXT PRIMARY KEY,
    last_seen INTEGER,
    status_json TEXT
);
CREATE TABLE IF NOT EXISTS frames (
    device_id TEXT NOT NULL,
    ts INTEGER NOT NULL,             -- epoch seconds (t, or received_at - age_s)
    ts_estimated INTEGER NOT NULL,   -- 1 when t was null on the device
    geohash TEXT,
    snd INTEGER, co2 INTEGER, pm25 INTEGER, hum REAL, temp REAL,
    stale INTEGER NOT NULL DEFAULT 0,
    received_at INTEGER NOT NULL,
    PRIMARY KEY (device_id, ts)      -- a retried batch does not duplicate rows
);
CREATE TABLE IF NOT EXISTS commands (
    id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    type TEXT NOT NULL,
    params_json TEXT NOT NULL,
    state TEXT NOT NULL,             -- pending | sent | in_progress | completed | failed
    progress INTEGER NOT NULL DEFAULT 0,
    detail TEXT,
    created_at INTEGER NOT NULL,
    sent_at INTEGER,
    updated_at INTEGER NOT NULL
);
"""

COMMAND_TYPES = {"status", "config", "geohash", "restart", "ota", "rollback", "wifi"}
RESEND_AFTER_S = 600  # re-deliver a "sent" command never acked after 10 min
OFFLINE_AFTER_S = 180

DEVICE_RE = re.compile(r"^/api/v1/devices/(aqa-[0-9A-F]{12})(/.*)?$")


class Store:
    def __init__(self, path):
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        self.lock = threading.Lock()

    def q(self, sql, args=()):
        with self.lock, self.db:
            return self.db.execute(sql, args).fetchall()

    def touch(self, device_id, status=None):
        now = int(time.time())
        with self.lock, self.db:
            self.db.execute(
                "INSERT INTO devices (id, last_seen, status_json) VALUES (?, ?, ?) "
                "ON CONFLICT(id) DO UPDATE SET last_seen = excluded.last_seen, "
                "status_json = COALESCE(excluded.status_json, devices.status_json)",
                (device_id, now, json.dumps(status) if status is not None else None))

    def deliverable_commands(self, device_id):
        """Pending commands (and stale unacked ones), marked as sent."""
        now = int(time.time())
        with self.lock, self.db:
            rows = self.db.execute(
                "SELECT * FROM commands WHERE device_id = ? AND (state = 'pending' OR "
                "(state = 'sent' AND sent_at < ?)) ORDER BY created_at LIMIT 5",
                (device_id, now - RESEND_AFTER_S)).fetchall()
            for r in rows:
                self.db.execute("UPDATE commands SET state = 'sent', sent_at = ?, updated_at = ? "
                                "WHERE id = ?", (now, now, r["id"]))
        return [{"id": r["id"], "type": r["type"], "params": json.loads(r["params_json"])}
                for r in rows]


class Handler(BaseHTTPRequestHandler):
    store: Store
    tokens: dict  # device_id -> token, or {"*": token} for any device

    # ---- plumbing

    def reply(self, code, body=None):
        data = b"" if body is None else json.dumps(body).encode()
        self.send_response(code)
        if body is not None:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > 256 * 1024:
            raise ValueError("body too large")
        raw = self.rfile.read(length) if length else b"{}"
        value = json.loads(raw)
        if not isinstance(value, dict):
            raise ValueError("body must be a JSON object")
        return value

    def device_authorized(self, device_id):
        expected = self.tokens.get(device_id) or self.tokens.get("*")
        got = self.headers.get("Authorization", "")
        return expected is not None and got == f"Bearer {expected}"

    def log_message(self, fmt, *args):
        print(f"{self.address_string()} {fmt % args}")

    # ---- routing

    def do_POST(self):
        m = DEVICE_RE.match(self.path.split("?")[0])
        if not m:
            return self.reply(404, {"error": "not found"})
        device_id, rest = m.group(1), m.group(2) or ""
        try:
            body = self.body()
        except ValueError as exc:
            return self.reply(400, {"error": str(exc)})

        if rest == "/commands":  # app route: enqueue a command
            return self.enqueue_command(device_id, body)

        if not self.device_authorized(device_id):
            return self.reply(401, {"error": "invalid token"})
        if rest == "/status":
            return self.post_status(device_id, body)
        if rest == "/frames":
            return self.post_frames(device_id, body)
        ack = re.match(r"^/commands/([A-Za-z0-9_-]{1,64})/ack$", rest)
        if ack:
            return self.post_ack(device_id, ack.group(1), body)
        return self.reply(404, {"error": "not found"})

    def do_GET(self):
        path, _, query = self.path.partition("?")
        params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
        if path == "/api/v1/devices":
            now = int(time.time())
            rows = self.store.q("SELECT id, last_seen FROM devices ORDER BY id")
            return self.reply(200, {"devices": [
                {"id": r["id"], "last_seen": r["last_seen"],
                 "online": now - (r["last_seen"] or 0) < OFFLINE_AFTER_S} for r in rows]})
        m = DEVICE_RE.match(path)
        if not m:
            return self.reply(404, {"error": "not found"})
        device_id, rest = m.group(1), m.group(2) or ""
        if rest == "":
            rows = self.store.q("SELECT * FROM devices WHERE id = ?", (device_id,))
            if not rows:
                return self.reply(404, {"error": "unknown device"})
            r = rows[0]
            return self.reply(200, {"id": r["id"], "last_seen": r["last_seen"],
                                    "online": time.time() - (r["last_seen"] or 0) < OFFLINE_AFTER_S,
                                    "status": json.loads(r["status_json"] or "null")})
        if rest == "/frames":
            try:
                t_from = int(params.get("from", 0))
                t_to = int(params.get("to", 2**31))
                limit = min(int(params.get("limit", 5000)), 50000)
            except ValueError:
                return self.reply(400, {"error": "from, to and limit must be integers"})
            rows = self.store.q(
                "SELECT ts, ts_estimated, geohash, snd, co2, pm25, hum, temp, stale FROM frames "
                "WHERE device_id = ? AND ts BETWEEN ? AND ? ORDER BY ts LIMIT ?",
                (device_id, t_from, t_to, limit))
            return self.reply(200, {"frames": [
                {**dict(r), "ts_estimated": bool(r["ts_estimated"]), "stale": bool(r["stale"])}
                for r in rows]})
        if rest == "/commands":
            rows = self.store.q("SELECT * FROM commands WHERE device_id = ? "
                                "ORDER BY created_at DESC LIMIT 50", (device_id,))
            return self.reply(200, {"commands": [
                {"id": r["id"], "type": r["type"], "params": json.loads(r["params_json"]),
                 "state": r["state"], "progress": r["progress"], "detail": r["detail"],
                 "created_at": r["created_at"], "updated_at": r["updated_at"]} for r in rows]})
        return self.reply(404, {"error": "not found"})

    # ---- device endpoints

    def post_status(self, device_id, body):
        self.store.touch(device_id, body)
        print(f"[status] {device_id} fw={body.get('firmware_version')} boot={body.get('boot_count')} "
              f"queue={body.get('queue')} sensors={body.get('sensors')}")
        return self.reply(200, {"commands": self.store.deliverable_commands(device_id)})

    def post_frames(self, device_id, body):
        frames = body.get("frames")
        if not isinstance(frames, list) or not frames or len(frames) > 100:
            return self.reply(422, {"error": "frames must be a list of 1-100 objects"})
        now = int(time.time())
        rows = []
        for f in frames:
            if not isinstance(f, dict) or not isinstance(f.get("age_s"), int):
                return self.reply(422, {"error": "each frame needs an integer age_s"})
            t = f.get("t")
            ts, estimated = (t, 0) if isinstance(t, int) and t > 0 else (now - f["age_s"], 1)
            rows.append((device_id, ts, estimated, body.get("geohash"), f.get("snd"), f.get("co2"),
                         f.get("pm25"), f.get("hum"), f.get("temp"), int(bool(f.get("stale"))), now))
        with self.store.lock, self.store.db:
            cur = self.store.db.executemany(
                "INSERT OR IGNORE INTO frames VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", rows)
        self.store.touch(device_id)
        print(f"[frames] {device_id} {len(rows)} received, {cur.rowcount} new, last={frames[-1]}")
        return self.reply(200, {"accepted": len(rows),
                                "commands": self.store.deliverable_commands(device_id)})

    def post_ack(self, device_id, command_id, body):
        state = body.get("status")
        if state not in ("in_progress", "completed", "failed"):
            return self.reply(422, {"error": "status must be in_progress, completed or failed"})
        now = int(time.time())
        with self.store.lock, self.store.db:
            cur = self.store.db.execute(
                "UPDATE commands SET state = ?, progress = ?, detail = ?, updated_at = ? "
                "WHERE id = ? AND device_id = ?",
                (state, int(body.get("progress") or 0), body.get("detail"), now, command_id, device_id))
        if cur.rowcount == 0:
            return self.reply(404, {"error": "unknown command"})
        self.store.touch(device_id)
        print(f"[ack] {device_id} {command_id} {state} {body.get('progress')}% {body.get('detail') or ''}")
        return self.reply(204)

    # ---- app endpoint

    def enqueue_command(self, device_id, body):
        ctype, params = body.get("type"), body.get("params", {})
        if ctype not in COMMAND_TYPES or not isinstance(params, dict):
            return self.reply(422, {"error": f"type must be one of {sorted(COMMAND_TYPES)}, params an object"})
        cid = uuid.uuid4().hex[:16]
        now = int(time.time())
        self.store.q("INSERT INTO commands VALUES (?, ?, ?, ?, 'pending', 0, NULL, ?, NULL, ?)",
                     (cid, device_id, ctype, json.dumps(params), now, now))
        print(f"[command] {device_id} {cid} {ctype} {params}")
        return self.reply(201, {"id": cid, "state": "pending"})


def main():
    parser = argparse.ArgumentParser(description="Serveur de test de l'API AQA (docs/api.md).")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--db", default="aqa-mock.sqlite3")
    parser.add_argument("--token", required=True,
                        help="jeton accepté pour tous les capteurs, ou id=jeton,id2=jeton2")
    args = parser.parse_args()

    if "=" in args.token:
        tokens = dict(pair.split("=", 1) for pair in args.token.split(","))
    else:
        tokens = {"*": args.token}

    Handler.store = Store(args.db)
    Handler.tokens = tokens
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"API de test sur http://{args.host}:{args.port}  (base : {args.db})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
