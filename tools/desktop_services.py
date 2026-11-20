#!/usr/bin/env python3
"""Supervise the computer's wireless receivers, MQTT replay, and HTTPS dashboard."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]

def main():
    active = True
    def stop(*unused):
        nonlocal active
        active = False
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    definitions = {
        "https": ["tools/local_backend.py", "serve"],
        "loopback": ["tools/local_backend.py", "serve-ui"],
    }
    if (ROOT / "private/mqtt.json").is_file():
        definitions["mqtt"] = ["tools/mqtt_bridge.py"]
    private = ROOT / "private"
    ssh_tunnel = all((private / name).is_file() for name in ("web-tunnel.key", "web-tunnel.known_hosts"))
    if ssh_tunnel or shutil.which("cloudflared") or (private / "bin/cloudflared").is_file():
        definitions["online"] = ["tools/online_backend.py"]
    children, logs, retry = {}, {}, {}
    (ROOT / "build").mkdir(exist_ok=True)
    try:
        for name in definitions:
            logs[name] = (ROOT / "build" / ("service-" + name + ".log")).open("a")
        while active:
            now = time.monotonic()
            for name, arguments in definitions.items():
                child = children.get(name)
                if child is not None and child.poll() is None:
                    continue
                if now < retry.get(name, 0):
                    continue
                children[name] = subprocess.Popen([sys.executable, *arguments], cwd=ROOT, stdout=logs[name], stderr=subprocess.STDOUT)
                retry[name] = now + 30
            manifest = ROOT / "private/services.json"
            temporary = manifest.with_suffix(".tmp")
            temporary.write_text(json.dumps({"pid": os.getpid(), "children": {name: child.pid for name, child in children.items()}}, indent=2) + "\n")
            temporary.chmod(0o600); temporary.replace(manifest)
            time.sleep(2)
    finally:
        for child in children.values():
            if child.poll() is None:
                child.terminate()
        for child in children.values():
            try:
                child.wait(timeout=8)
            except subprocess.TimeoutExpired:
                child.kill(); child.wait()
        for log in logs.values():
            log.close()

if __name__ == "__main__":
    main()
