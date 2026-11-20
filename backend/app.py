"""AirCue's device ingestion API and authenticated, same-origin dashboard."""
import csv
import io
import json
import os
from pathlib import Path
import secrets
import re
import sqlite3
import time
from typing import Optional, Union
from urllib.parse import urlsplit

from fastapi import Depends, FastAPI, HTTPException, Query, Request, Response
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.openapi.utils import get_openapi
from pydantic import BaseModel, ConfigDict, Field, StrictBool, StrictInt, StrictStr, field_validator
from starlette.middleware.gzip import GZipMiddleware

from .codec import decode
from .db import connect, initialize
from .security import decode_cursor, encode_cursor, password_hash, token_hash, verify_password

ROOT = Path(__file__).resolve().parents[1]
RULE_FIELDS = {"watch_on", "watch_off", "high_on", "high_off", "watch_s", "high_s", "high_clear_s", "clear_s", "trend_ppm"}
DEFAULT_RULES = dict(watch_on=1000, watch_off=800, high_on=1500, high_off=1300, watch_s=30, high_s=60, high_clear_s=120, clear_s=120, trend_ppm=100)

def now_ms():
    return int(time.time() * 1000)

def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)

class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)

class Login(StrictModel):
    username: str = Field(min_length=1, max_length=80)
    password: str = Field(min_length=1, max_length=256)

class WireRecord(StrictModel):
    wire: str = Field(pattern=r"^(?:[0-9a-f]{192}|[0-9a-f]{224})$")
    run_id: str = Field(default="", pattern=r"^[a-zA-Z0-9_-]{0,40}$")
    run_t0: int = Field(default=0, ge=0, le=2**53-1)
    order: Optional[str] = Field(default=None, pattern=r"^[0-9]{1,20}$")

class Gap(StrictModel):
    gap_id: str = Field(min_length=1, max_length=100)
    reason: str = Field(min_length=1, max_length=80)
    first_order: str = Field(pattern=r"^[0-9]{1,20}$")
    last_order: str = Field(pattern=r"^[0-9]{1,20}$")
    count: int = Field(ge=0, le=1000000)
    segment: str = Field(max_length=100)
    record_ranges: list[dict] = Field(default_factory=list, max_length=64)

class Ingest(StrictModel):
    records: list[WireRecord] = Field(default_factory=list, max_length=32)
    gaps: list[Gap] = Field(default_factory=list, max_length=48)
    status: Optional[dict] = None
    status_sent_at: Optional[int] = Field(default=None, ge=0, le=2**53-1)
    config_snapshots: list[dict] = Field(default_factory=list, max_length=32)

class Command(StrictModel):
    request_id: str = Field(pattern=r"^[a-zA-Z0-9_-]{1,64}$")
    base_revision: int = Field(ge=1, le=2**32-1)
    operation: str = Field(pattern=r"^(config|wifi|uplink|mute|commission|run_start|run_stop|ota)$")
    expires_at: int = Field(gt=0, le=2**53-1)
    patch: Optional[dict[str, Union[StrictInt, StrictBool, StrictStr]]] = None
    action: Optional[str] = Field(default=None, pattern=r"^(upsert|delete|prefer)$")
    ssid: Optional[str] = None
    wifi_password: Optional[str] = None
    enabled: Optional[bool] = None
    fresh_air_confirmed: Optional[bool] = None
    episode_id: Optional[str] = Field(default=None, pattern=r"^[0-9a-f]{32}:[0-9]{1,10}$")
    duration_s: Optional[int] = Field(default=None, ge=1, le=300)

class CommandResult(StrictModel):
    request_id: str = Field(pattern=r"^[a-zA-Z0-9_-]{1,64}$")
    status: str = Field(pattern=r"^(applied|accepted|rejected|conflict|expired)$")
    applied_revision: int = Field(ge=1, le=2**32-1)
    message: str = Field(default="", max_length=200)
    http_status: int = Field(default=200, ge=200, le=599)

def create_app(db_path=None, origin=None):
    path = Path(db_path or os.getenv("AIRCUE_DB", str(ROOT / "private" / "aircue.db")))
    path.parent.mkdir(parents=True, exist_ok=True)
    initialize(str(path))
    public_origin = (origin or os.getenv("AIRCUE_ORIGIN", "http://127.0.0.1:8000")).rstrip("/")
    parsed = urlsplit(public_origin)
    if parsed.scheme not in ("http", "https") or parsed.path or not parsed.netloc:
        raise ValueError("AIRCUE_ORIGIN must be an HTTP(S) origin")
    secure_cookie = parsed.scheme == "https"
    app = FastAPI(title="AirCue", version="1.2.0", docs_url=None, redoc_url=None)
    app.add_middleware(GZipMiddleware, minimum_size=1024, compresslevel=5)
    app.state.db_path = str(path)
    dummy_hash = password_hash(secrets.token_urlsafe(32))

    @app.middleware("http")
    async def boundaries(request, call_next):
        if request.headers.get("host", "").lower() != parsed.netloc.lower():
            return JSONResponse({"detail": "Invalid Host"}, 403)
        if request.method in ("POST", "PUT", "PATCH", "DELETE"):
            device_api = request.url.path.endswith(("/ingest", "/command-results"))
            if not device_api and request.headers.get("origin") != public_origin:
                return JSONResponse({"detail": "Invalid Origin"}, 403)
        try:
            length = int(request.headers.get("content-length", "0"))
        except ValueError:
            return JSONResponse({"detail": "Invalid Content-Length"}, 400)
        if length > 16384:
            return JSONResponse({"detail": "Request body exceeds 16 KiB"}, 413)
        # Bound chunked requests too; do not rely solely on Content-Length.
        if request.method in ("POST", "PUT", "PATCH"):
            chunks = bytearray()
            async for chunk in request.stream():
                chunks.extend(chunk)
                if len(chunks) > 16384:
                    return JSONResponse({"detail": "Request body exceeds 16 KiB"}, 413)
            request._body = bytes(chunks)
        response = await call_next(request)
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["Content-Security-Policy"] = "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"
        if request.url.path not in ("/app.js", "/style.css"):
            response.headers["Cache-Control"] = "no-store"
        return response

    @app.exception_handler(sqlite3.OperationalError)
    async def database_busy(request, exc):
        return JSONResponse({"detail": "Database temporarily unavailable"}, 503)

    def session(request: Request):
        cookie = request.cookies.get("aircue_session", "")
        with connect(str(path)) as db:
            row = db.execute("SELECT * FROM sessions WHERE token_hash=? AND expires_at>?", (token_hash(cookie), now_ms())).fetchone()
        if not row:
            raise HTTPException(401, "Login required")
        if request.method != "GET" and not secrets.compare_digest(request.headers.get("x-csrf-token", ""), row["csrf"]):
            raise HTTPException(403, "Invalid CSRF token")
        return dict(row)

    def owned(device_id: str, user=Depends(session)):
        with connect(str(path)) as db:
            row = db.execute("SELECT * FROM devices WHERE id=? AND owner_id=?", (device_id, user["user_id"])).fetchone()
        if not row:
            raise HTTPException(404, "Device not found")
        return dict(row)

    def device(device_id: str, request: Request):
        auth = request.headers.get("authorization", "")
        if not auth.startswith("Bearer "):
            raise HTTPException(401, "Device credential required")
        with connect(str(path)) as db:
            row = db.execute("SELECT * FROM devices WHERE id=? AND token_hash=?", (device_id, token_hash(auth[7:]))).fetchone()
        if not row:
            raise HTTPException(403, "Device credential does not match")
        return dict(row)

    @app.get("/api/v1/info")
    def info():
        return {"mode": "cloud", "version": "1.2.0"}

    @app.head("/firmware/aircue-esp32.bin",include_in_schema=False)
    @app.get("/firmware/aircue-esp32.bin",include_in_schema=False)
    def firmware(request: Request):
        directory=os.getenv("AIRCUE_FIRMWARE_DIR")
        if not directory:
            raise HTTPException(404,"No firmware release is configured")
        auth=request.headers.get("authorization","")
        if not auth.startswith("Bearer "):
            raise HTTPException(401,"Device credential required")
        with connect(str(path)) as db:
            row=db.execute("SELECT id FROM devices WHERE token_hash=?",(token_hash(auth[7:]),)).fetchone()
        if not row or not re.fullmatch(r"[A-Za-z0-9_-]{1,40}",row["id"]):
            raise HTTPException(403,"Invalid device credential")
        root=Path(directory).resolve()
        image=(root/(row["id"]+".bin")).resolve()
        if image.parent!=root or not image.is_file():
            raise HTTPException(404,"No firmware release for this device")
        # Byte ranges always refer to the exact signed image on disk.
        return FileResponse(image,media_type="application/octet-stream",filename="aircue-esp32.bin",headers={"Content-Encoding":"identity"})

    @app.post("/api/v1/session")
    def login(body: Login, request: Request, response: Response):
        peer = request.client.host if request.client else "unknown"
        now = now_ms()
        with connect(str(path), True) as db:
            row = db.execute("SELECT * FROM login_limits WHERE peer=?", (peer,)).fetchone()
            attempts = row["attempts"] if row and now - row["since_ms"] < 60000 else 0
            if attempts >= 10:
                raise HTTPException(429, "Wait before retrying login")
            since = row["since_ms"] if attempts else now
            db.execute("INSERT OR REPLACE INTO login_limits VALUES(?,?,?)", (peer, since, attempts + 1))
            user = db.execute("SELECT * FROM users WHERE username=?", (body.username,)).fetchone()
        valid = verify_password(body.password, user["password_hash"] if user else dummy_hash)
        if not user or not valid:
            raise HTTPException(401, "Invalid credentials")
        token, csrf = secrets.token_urlsafe(32), secrets.token_urlsafe(24)
        with connect(str(path), True) as db:
            db.execute("DELETE FROM sessions WHERE expires_at<=?", (now,))
            db.execute("INSERT INTO sessions VALUES(?,?,?,?)", (token_hash(token), user["id"], csrf, now + 3600000))
        response.set_cookie("aircue_session", token, max_age=3600, httponly=True, samesite="strict", secure=secure_cookie, path="/")
        return {"csrf_token": csrf, "expires_in": 3600}

    @app.get("/api/v1/session")
    def current_session(user=Depends(session)):
        return {"csrf_token": user["csrf"]}

    @app.delete("/api/v1/session")
    def logout(response: Response, user=Depends(session)):
        with connect(str(path), True) as db:
            db.execute("DELETE FROM sessions WHERE token_hash=?", (user["token_hash"],))
        response.delete_cookie("aircue_session", path="/")
        return {"status": "signed_out"}

    @app.get("/v1/devices")
    def list_devices(user=Depends(session)):
        with connect(str(path)) as db:
            return {"devices": [row[0] for row in db.execute("SELECT id FROM devices WHERE owner_id=? ORDER BY id", (user["user_id"],))]}

    @app.post("/v1/devices/{device_id}/ingest")
    def ingest(device_id: str, body: Ingest, auth=Depends(device)):
        now = now_ms()
        decoded = []
        try:
            for item in body.records:
                record = decode(item.wire)
                if bool(item.run_id) != bool(item.run_t0):
                    raise ValueError("Run metadata is incomplete")
                if record.get("window_index") is not None and (not item.run_id or record["demo"]):
                    raise ValueError("An aligned window requires a formal run")
                decoded.append((item, record))
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
        status_text = None
        if body.status is not None:
            status = body.status
            if status.get("device_id", device_id) != device_id:
                raise HTTPException(403, "Status device identity does not match credential")
            try:
                encoded_status = canonical(status)
            except (ValueError, RecursionError):
                raise HTTPException(422, "Invalid status JSON")
            if len(encoded_status) > 6000 or type(status.get("uptime_ms")) is not int or type(status.get("boot_ordinal")) is not int:
                raise HTTPException(422, "Invalid status identity")
            if not 0 <= status["uptime_ms"] <= 2**53-1 or not 1 <= status["boot_ordinal"] <= 2**32-1:
                raise HTTPException(422, "Invalid status timestamp")
            def has_secret(value):
                if isinstance(value, dict):
                    return any("password" in key.lower() or "token" in key.lower() or has_secret(item) for key, item in value.items())
                return isinstance(value, list) and any(has_secret(item) for item in value)
            if has_secret(status):
                raise HTTPException(422, "Status must not contain credentials")
            for key in ("sample_age_ms", "measured_at", "received_at"):
                value = status.get(key)
                if value is not None and (type(value) is not int or not 0 <= value <= 2**53-1):
                    raise HTTPException(422, "Invalid status time field")
            if status.get("ppm") is not None and (type(status["ppm"]) is not int or not 380 <= status["ppm"] <= 32000):
                raise HTTPException(422, "Invalid status concentration")
            status_text = encoded_status
        for snapshot in body.config_snapshots:
            if set(snapshot) - (RULE_FIELDS | {"revision", "rules_revision", "demo", "uplink_enabled"}):
                raise HTTPException(422, "Unexpected configuration snapshot field")
            if type(snapshot.get("revision")) is not int or not 1 <= snapshot["revision"] <= 2**32-1:
                raise HTTPException(422, "Invalid snapshot revision")
            if any(type(snapshot.get(key)) is not int for key in RULE_FIELDS):
                raise HTTPException(422, "Incomplete measurement rule snapshot")
        ack, gap_ack = [], []
        with connect(str(path), True) as db:
            for snapshot in body.config_snapshots:
                existing = db.execute("SELECT body FROM config_snapshots WHERE device_id=? AND revision=?", (device_id, snapshot["revision"])).fetchone()
                previous = json.loads(existing[0]) if existing else {}
                if any(previous.get(key, snapshot[key]) != snapshot[key] for key in RULE_FIELDS):
                    raise HTTPException(409, "Configuration snapshot content conflict")
                db.execute("INSERT OR REPLACE INTO config_snapshots VALUES(?,?,?)", (device_id, snapshot["revision"], canonical({**previous, **snapshot})))
            for item, record in decoded:
                old = db.execute("SELECT wire,run_id,run_t0 FROM records WHERE device_id=? AND boot_id=? AND sequence=?", (device_id, record["boot_id"], record["sequence"])).fetchone()
                if old and (old["wire"] != item.wire or old["run_id"] != item.run_id or old["run_t0"] != item.run_t0):
                    raise HTTPException(409, "Record identity has conflicting content")
                if not old:
                    db.execute("INSERT INTO records(device_id,boot_id,sequence,wire,run_id,run_t0,decoded,received_at) VALUES(?,?,?,?,?,?,?,?)",
                               (device_id, record["boot_id"], record["sequence"], item.wire, item.run_id, item.run_t0, canonical(record), now))
                    if record.get("event") == "TIME_ANCHOR" and record["measured_at"]:
                        db.execute("INSERT INTO time_anchors VALUES(?,?,?,?,?)", (device_id, record["boot_id"], record["monotonic_ms"], record["measured_at"], record["record_id"]))
                ack.append(record["record_id"])
            for gap in body.gaps:
                value = canonical(gap.model_dump())
                old = db.execute("SELECT body FROM gaps WHERE device_id=? AND gap_id=?", (device_id, gap.gap_id)).fetchone()
                if old and old[0] != value:
                    raise HTTPException(409, "Gap identity has conflicting content")
                db.execute("INSERT OR IGNORE INTO gaps VALUES(?,?,?,?)", (device_id, gap.gap_id, value, now))
                gap_ack.append(gap.gap_id)
            if status_text:
                previous = db.execute("SELECT boot_ordinal,status_mono FROM devices WHERE id=?", (device_id,)).fetchone()
                incoming = (body.status["boot_ordinal"], body.status["uptime_ms"])
                if incoming > (previous[0], previous[1]):
                    # A retained MQTT heartbeat must keep its original age when replayed.
                    received = min(now, body.status_sent_at) if body.status_sent_at else now
                    db.execute("UPDATE devices SET status=?,status_received=?,boot_ordinal=?,status_mono=? WHERE id=?", (status_text, received, *incoming, device_id))
                    cfg = body.status.get("config", {})
                    if type(cfg.get("revision")) is int:
                        db.execute("INSERT OR IGNORE INTO config_snapshots VALUES(?,?,?)", (device_id, cfg["revision"], canonical(cfg)))
            db.execute("UPDATE commands SET status='expired' WHERE device_id=? AND status='pending' AND expires_at<=?", (device_id, now))
            commands = [json.loads(row[0]) for row in db.execute("SELECT body FROM commands WHERE device_id=? AND status='pending' ORDER BY created_at LIMIT 4", (device_id,))]
        # Leaving the transaction context commits before the ACK is constructed.
        return {"ack": ack, "gap_ack": gap_ack, "commands": commands, "received_at": now}

    @app.get("/v1/devices/{device_id}/status")
    def status(device_id: str, auth=Depends(owned)):
        now = now_ms()
        data = json.loads(auth["status"]) if auth["status"] else {}
        received = auth["status_received"]
        age = now - received if received else None
        heartbeat_stale = age is None or age > 90000
        # A fresh heartbeat does not make a stale sensor reading fresh.
        data.update(source="cloud", received_at=received, heartbeat_age_ms=age,
                    stale=bool(data.get("stale")) or heartbeat_stale,
                    offline_reason="Offline; reason pending synchronization" if heartbeat_stale else None)
        if data.get("sample_age_ms") is not None and age is not None:
            data["sample_age_ms"] += max(0, age)
        return data

    def read_records(device_id, after, limit, recent=False):
        with connect(str(path)) as db:
            if recent:
                rows = db.execute("SELECT * FROM records WHERE device_id=? ORDER BY row_id DESC LIMIT ?", (device_id, limit)).fetchall()
                rows.reverse()
            else:
                rows = db.execute("SELECT * FROM records WHERE device_id=? AND row_id>? ORDER BY row_id LIMIT ?", (device_id, after, limit)).fetchall()
            result = []
            for row in rows:
                item = json.loads(row["decoded"])
                item.update(received_at=row["received_at"], received_order=str(row["row_id"]), run_id=row["run_id"], run_t0=row["run_t0"])
                config = db.execute("SELECT body FROM config_snapshots WHERE device_id=? AND revision=?", (device_id, item.get("config_revision"))).fetchone()
                item["config_snapshot"] = json.loads(config[0]) if config else None
                if item["measured_at"] is None:
                    anchor = db.execute("SELECT * FROM time_anchors WHERE device_id=? AND boot_id=? ORDER BY ABS(monotonic_ms-?) LIMIT 1", (device_id, row["boot_id"], item["monotonic_ms"])).fetchone()
                    if anchor:
                        item["estimated_measured_at"] = anchor["utc_ms"] + item["monotonic_ms"] - anchor["monotonic_ms"]
                        item["time_quality"] = "ESTIMATED_FROM_ANCHOR"
                result.append(item)
            return result, rows[-1]["row_id"] if rows else after

    @app.get("/v1/devices/{device_id}/history")
    def history(device_id: str, cursor: str = "", limit: int = Query(128, ge=1, le=256), recent: bool = False, auth=Depends(owned)):
        if recent and cursor:
            raise HTTPException(422, "Recent history starts without a cursor")
        try:
            after = decode_cursor(cursor)
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
        rows, end = read_records(device_id, after, limit, recent=recent)
        return {"records": rows, "next_cursor": encode_cursor(end), "has_more": not recent and len(rows) == limit}

    @app.get("/v1/devices/{device_id}/export")
    def export(device_id: str, from_ms: Optional[int] = Query(default=None, ge=0, le=2**53-1),
               to_ms: Optional[int] = Query(default=None, ge=0, le=2**53-1),
               run_id: str = Query(default="", pattern=r"^[a-zA-Z0-9_-]{0,40}$"), auth=Depends(owned)):
        if from_ms is not None and to_ms is not None and from_ms >= to_ms:
            raise HTTPException(422, "from_ms must be earlier than to_ms")
        columns = ["record_id", "record_type", "run_id", "run_t0", "window_index", "measured_at", "estimated_measured_at", "received_at", "time_quality", "monotonic_ms", "mean", "minimum", "maximum", "valid_count", "expected_count", "valid_ms", "observed_high_ms", "red_valid_ms", "warmup_count", "error_count", "missed_count", "window_ms", "level", "trend", "config_revision", "demo", "partial", "interrupted", "event", "episode_id", "peak", "high_ms", "red_ms", "co2_raw_ppm", "temperature_c", "humidity_rh", "pressure_hpa", "barometer_temperature_c", "co2_raw_count", "barometer_count"]
        def matches(row):
            if run_id and row["run_id"] != run_id:
                return False
            # Filter on original UTC only, consistently with the device export.
            measured = row.get("measured_at")
            return (from_ms is None and to_ms is None or measured is not None
                    and (from_ms is None or measured >= from_ms)
                    and (to_ms is None or measured < to_ms))
        def stream():
            buffer = io.StringIO()
            writer = csv.DictWriter(buffer, columns, extrasaction="ignore")
            writer.writeheader()
            yield buffer.getvalue()
            cursor = 0
            while True:
                rows, end = read_records(device_id, cursor, 128)
                if not rows:
                    break
                buffer.seek(0); buffer.truncate(0)
                writer.writerows(row for row in rows if matches(row))
                yield buffer.getvalue()
                cursor = end
        return StreamingResponse(stream(), media_type="text/csv", headers={"Content-Disposition": f'attachment; filename="{device_id}.csv"'})

    @app.get("/v1/devices/{device_id}/episodes")
    def episodes(device_id: str, auth=Depends(owned)):
        with connect(str(path)) as db:
            rows = db.execute("SELECT decoded,received_at FROM records WHERE device_id=? AND json_extract(decoded,'$.record_type')='EVENT' ORDER BY row_id", (device_id,))
            found = {}
            for row in rows:
                item = json.loads(row[0])
                if not item.get("episode_id") or item.get("event") not in ("EPISODE_START", "FALLING", "RECOVERED", "CONFIG_CHANGED"):
                    continue
                eid = item["episode_id"]
                ep = found.setdefault(eid, dict(episode_id=eid, events=[]))
                ep["events"].append(item)
                ep.update(peak=max(ep.get("peak",0),item["peak"]), high_ms=max(ep.get("high_ms",0),item["high_ms"]), red_ms=max(ep.get("red_ms",0),item["red_ms"]), valid_ms=max(ep.get("valid_ms",0),item["valid_ms"]), interrupted=ep.get("interrupted", False) or item["interrupted"])
                if item["event"] == "EPISODE_START":
                    ep["confirmed_at"] = item["measured_at"]
                    ep["candidate_at"] = item["measured_at"] - (item["monotonic_ms"]-item["extra"]) if item["measured_at"] else None
                    ep["start_boot_id"] = item["boot_id"]
                elif item["event"] == "FALLING":
                    ep["first_falling_at"] = item["measured_at"]
                elif item["event"] in ("RECOVERED", "CONFIG_CHANGED"):
                    ep.update(ended_at=item["measured_at"],end_reason=item["event"],end_boot_id=item["boot_id"],known_missing_ms=item["known_missing_ms"])
            for ep in found.values():
                ep["active"] = "end_reason" not in ep
                ep["unknown_power_interval"] = bool(ep.get("start_boot_id") and ep.get("end_boot_id") and ep["start_boot_id"] != ep["end_boot_id"])
                ep["elapsed_wall_ms"] = ep["ended_at"]-ep["candidate_at"] if ep.get("ended_at") and ep.get("candidate_at") and ep["ended_at"]>=ep["candidate_at"] else None
                ep["duration_note"] = "Observed elapsed time; gaps are separate and ventilation causality is not inferred."
            return {"episodes": list(found.values())}

    @app.get("/v1/devices/{device_id}/gaps")
    def gaps(device_id: str, auth=Depends(owned)):
        with connect(str(path)) as db:
            return {"gaps": [json.loads(row[0]) for row in db.execute("SELECT body FROM gaps WHERE device_id=? ORDER BY received_at", (device_id,))]}

    @app.post("/v1/devices/{device_id}/commands")
    def command(device_id: str, body: Command, auth=Depends(owned)):
        value = body.model_dump(exclude_none=True)
        text = canonical(value)
        with connect(str(path), True) as db:
            old = db.execute("SELECT body,status,result FROM commands WHERE device_id=? AND request_id=?", (device_id, body.request_id)).fetchone()
            if old:
                if old["body"] != text:
                    raise HTTPException(409, "request_id was used with different content")
                return {"request_id": body.request_id, "status": old["status"], "result": json.loads(old["result"]) if old["result"] else None}
            now = now_ms()
            if not now < body.expires_at <= now + 3600000:
                raise HTTPException(422, "Expiry must be within one hour")
            operation_fields = {"config": {"patch"}, "wifi": {"action", "ssid", "wifi_password"},
                                "uplink": {"enabled"}, "mute": {"episode_id", "duration_s"},
                                "commission": {"fresh_air_confirmed"}, "run_start": set(), "run_stop": set(), "ota": set()}
            if set(value) - {"request_id", "base_revision", "operation", "expires_at"} - operation_fields[body.operation]:
                raise HTTPException(422, "Field is not allowed for this operation")
            def valid_ssid(ssid):
                return isinstance(ssid, str) and "\0" not in ssid and 1 <= len(ssid.encode()) <= 32
            def valid_password(password):
                return isinstance(password, str) and "\0" not in password and (password == "" or 8 <= len(password.encode()) <= 63)
            if body.operation == "config":
                if not body.patch or set(body.patch) - RULE_FIELDS - {"demo", "buzzer_enabled", "ssid", "wifi_password"}:
                    raise HTTPException(403, "Unknown configuration field")
                for key, item in body.patch.items():
                    valid = (type(item) is int if key in RULE_FIELDS else type(item) is bool if key in {"demo", "buzzer_enabled"}
                             else valid_ssid(item) if key == "ssid" else valid_password(item))
                    if not valid:
                        raise HTTPException(422, "Invalid configuration field type or length")
                rules = {**DEFAULT_RULES, **json.loads(auth["status"] or "{}").get("config", {}), **body.patch}
                if not 380 <= rules["watch_off"] < rules["watch_on"] < rules["high_off"] < rules["high_on"] <= 5000:
                    raise HTTPException(422, "Invalid hysteresis thresholds")
                if any(not 1 <= rules[k] <= 3600 for k in ("watch_s", "high_s", "high_clear_s", "clear_s")) or not 50 <= rules["trend_ppm"] <= 1000:
                    raise HTTPException(422, "Invalid dwell time or trend threshold")
            elif body.operation == "wifi":
                if not body.action or not valid_ssid(body.ssid) or (body.wifi_password is not None and
                        (body.action != "upsert" or not valid_password(body.wifi_password))):
                    raise HTTPException(422, "Invalid saved-network operation")
            elif body.operation == "uplink" and body.enabled is None:
                raise HTTPException(422, "enabled is required")
            elif body.operation == "commission" and body.fresh_air_confirmed is not True:
                raise HTTPException(422, "Fresh-air confirmation is required")
            elif body.operation == "mute" and (not body.episode_id or not body.duration_s):
                raise HTTPException(422, "Mute requires an episode and duration")
            db.execute("UPDATE commands SET status='expired' WHERE device_id=? AND status='pending' AND expires_at<=?", (device_id, now))
            count = db.execute("SELECT count(*) FROM commands WHERE device_id=? AND status='pending'", (device_id,)).fetchone()[0]
            if count >= 64:
                raise HTTPException(429, "Too many pending commands")
            db.execute("INSERT INTO commands VALUES(?,?,?,?,?,?,?)", (device_id, body.request_id, text, "pending", None, now, body.expires_at))
        return {"request_id": body.request_id, "status": "pending"}

    @app.get("/v1/devices/{device_id}/commands")
    def commands(device_id: str, auth=Depends(owned)):
        with connect(str(path), True) as db:
            db.execute("UPDATE commands SET status='expired' WHERE device_id=? AND status='pending' AND expires_at<=?", (device_id, now_ms()))
            return {"commands": [dict(row) for row in db.execute("SELECT request_id,status,result,created_at,expires_at FROM commands WHERE device_id=? ORDER BY created_at DESC LIMIT 64", (device_id,))]}

    @app.post("/v1/devices/{device_id}/command-results")
    def command_result(device_id: str, body: CommandResult, auth=Depends(device)):
        text = canonical(body.model_dump())
        with connect(str(path), True) as db:
            row = db.execute("SELECT result FROM commands WHERE device_id=? AND request_id=?", (device_id, body.request_id)).fetchone()
            if not row:
                raise HTTPException(404, "Unknown command")
            if row[0] and row[0] != text:
                raise HTTPException(409, "Conflicting command result")
            db.execute("UPDATE commands SET status=?,result=? WHERE device_id=? AND request_id=?", (body.status, text, device_id, body.request_id))
        return {"status": "acknowledged", "request_id": body.request_id}

    @app.get("/")
    def index():
        return FileResponse(ROOT / "web" / "index.html")

    @app.get("/app.js")
    def javascript():
        return FileResponse(ROOT / "web" / "app.js", media_type="text/javascript")

    @app.get("/style.css")
    def stylesheet():
        return FileResponse(ROOT / "web" / "style.css", media_type="text/css")

    @app.get("/ble.js")
    def bluetooth_javascript():
        return FileResponse(ROOT / "web" / "ble.js", media_type="text/javascript")

    @app.get("/sw.js")
    def service_worker():
        return FileResponse(ROOT / "web" / "sw.js", media_type="text/javascript", headers={"Cache-Control": "no-cache"})

    @app.get("/manifest.webmanifest")
    def manifest():
        return FileResponse(ROOT / "web" / "manifest.webmanifest", media_type="application/manifest+json")

    def openapi():
        if app.openapi_schema:
            return app.openapi_schema
        schema = get_openapi(title=app.title, version=app.version, routes=app.routes)
        schema.setdefault("components", {})["securitySchemes"] = {
            "DeviceToken": {"type": "http", "scheme": "bearer"},
            "SessionCookie": {"type": "apiKey", "in": "cookie", "name": "aircue_session"},
            "CsrfToken": {"type": "apiKey", "in": "header", "name": "X-CSRF-Token"},
        }
        for route, methods in schema["paths"].items():
            for method, spec in methods.items():
                if not isinstance(spec, dict):
                    continue
                if route.endswith(("/ingest", "/command-results")):
                    spec["security"] = [{"DeviceToken": []}]
                elif route.startswith("/v1/") or (route == "/api/v1/session" and method != "post"):
                    spec["security"] = [{"SessionCookie": [], **({"CsrfToken": []} if method != "get" else {})}]
        app.openapi_schema = schema
        return schema
    app.openapi = openapi

    return app
