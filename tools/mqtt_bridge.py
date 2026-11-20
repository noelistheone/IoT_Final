#!/usr/bin/env python3
"""Restore retained Lab 5 history into the local backend and relay owned commands."""
import argparse
import json
from pathlib import Path
import signal
import sqlite3
import ssl
import sys
import time

import httpx
import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from backend.mqtt_protocol import decode, encode

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", default="http://127.0.0.1:8000")
    parser.add_argument("--seconds", type=float, default=0)
    args = parser.parse_args()
    credentials = json.loads((ROOT / "private/mqtt.json").read_text())
    provision = json.loads((ROOT / "private/provision.json").read_text())
    device, key = provision["device_id"], provision["device_token"]
    prefix = "aircue/" + device + "/v1/"
    inbox = ROOT / "private/mqtt-inbox.db"
    with sqlite3.connect(inbox) as db:
        db.execute("PRAGMA journal_mode=WAL")
        db.execute("CREATE TABLE IF NOT EXISTS inbox (topic TEXT PRIMARY KEY, body TEXT NOT NULL, generation INTEGER NOT NULL)")
    inbox.chmod(0o600)
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=device + "-computer", protocol=mqtt.MQTTv311)
    client.username_pw_set(credentials["username"], credentials["password"])
    client.tls_set_context(ssl.create_default_context()); client.reconnect_delay_set(1, 60)
    metrics = dict(connected=False, received=0, imported=0, rejected=0, errors=0)
    active = True
    def stop(*unused):
        nonlocal active
        active = False
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    def on_connect(client, userdata, flags, reason, properties):
        metrics["connected"] = not reason.is_failure
        if not reason.is_failure:
            # A new subscription replays every retained block, regardless of session expiry.
            client.subscribe([(prefix + "state", 1), (prefix + "history/#", 1), (prefix + "result", 1)])
            print("MQTT connected; requesting retained AirCue history.", flush=True)
        else:
            print("MQTT authentication or connection was rejected.", flush=True)
    def on_disconnect(client, userdata, flags, reason, properties):
        metrics["connected"] = False
    def on_message(client, userdata, message):
        try:
            suffix = message.topic.removeprefix(prefix)
            if message.topic == prefix + "state" or message.topic == prefix + "result" or suffix.startswith(("history/minutes/", "history/events/")):
                body = decode(message.topic, message.payload, key)
                topic = message.topic + ("/" + body["request_id"] if suffix == "result" else "")
                with sqlite3.connect(inbox, timeout=5) as db:
                    db.execute("PRAGMA synchronous=FULL")
                    db.execute("INSERT INTO inbox VALUES(?,?,?) ON CONFLICT(topic) DO UPDATE SET body=excluded.body,generation=excluded.generation", (topic, json.dumps(body, separators=(",", ":")), time.time_ns()))
                metrics["received"] += 1
        except (ValueError, KeyError, TypeError, sqlite3.Error):
            metrics["rejected"] += 1
    client.on_connect = on_connect; client.on_disconnect = on_disconnect; client.on_message = on_message
    def publish(suffix, body):
        topic = prefix + suffix
        return client.publish(topic, encode(topic, body, key), qos=1, retain=False)
    command_sent = {}
    def commands(reply):
        if metrics["connected"] and reply.get("commands"):
            command = reply["commands"][0]
            now = time.monotonic()
            if now - command_sent.get(command["request_id"], 0) >= 5:
                publish("command", command)
                command_sent[command["request_id"]] = now
                if len(command_sent) > 128:
                    command_sent.pop(next(iter(command_sent)))
    client.connect_async(credentials["host"], credentials["port"], keepalive=60); client.loop_start()
    started = time.monotonic(); next_poll = next_report = 0
    path = "/v1/devices/" + device
    try:
        with httpx.Client(base_url=args.backend.rstrip("/"), timeout=5, headers={"Authorization": "Bearer " + key}) as http:
            while active and (not args.seconds or time.monotonic() - started < args.seconds):
                now = time.monotonic()
                try:
                    with sqlite3.connect(inbox) as db:
                        rows = db.execute("SELECT topic,body,generation FROM inbox ORDER BY generation LIMIT 8").fetchall()
                    for topic, text, generation in rows:
                        body = json.loads(text)
                        result = topic.startswith(prefix + "result/")
                        if not result and "status" in body:
                            body["status_sent_at"] = body["status"].get("received_at") or None
                        response = http.post(path + ("/command-results" if result else "/ingest"), json=body)
                        response.raise_for_status()
                        if result:
                            publish("result-ack", {"request_id": body["request_id"]})
                        else:
                            commands(response.json())
                        with sqlite3.connect(inbox) as db:
                            db.execute("DELETE FROM inbox WHERE topic=? AND generation=?", (topic, generation))
                        metrics["imported"] += 1
                    if now >= next_poll:
                        next_poll = now + 10
                        response = http.post(path + "/ingest", json={"records": []}); response.raise_for_status(); commands(response.json())
                except (httpx.HTTPError, sqlite3.Error, ValueError):
                    metrics["errors"] += 1
                    time.sleep(1)
                if now >= next_report:
                    next_report = now + 5
                    destination = ROOT / "build/mqtt-bridge-status.json"
                    destination.write_text(json.dumps({**metrics, "updated_at": int(time.time()*1000)}, indent=2) + "\n")
                time.sleep(.25)
    finally:
        client.disconnect(); client.loop_stop()

if __name__ == "__main__":
    main()
