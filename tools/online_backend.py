#!/usr/bin/env python3
"""Serve the dashboard from this computer through a public HTTPS tunnel."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
TUNNEL_REMOVED = 'Register tunnel error from server side error="Unauthorized: Tunnel not found"'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8001)
    parser.add_argument("--cloudflared", default=shutil.which("cloudflared") or str(ROOT / "private/bin/cloudflared"))
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("Use a port between 1024 and 65535")
    cfg = json.loads((ROOT / "private/local-backend.json").read_text())
    ssh_key = ROOT / "private/web-tunnel.key"
    known_hosts = ROOT / "private/web-tunnel.known_hosts"
    if ssh_key.is_file() and known_hosts.is_file():
        command = [
            "/usr/bin/ssh", "-F", "/dev/null", "-nT", "-o", "BatchMode=yes",
            "-o", "StrictHostKeyChecking=yes", "-o", f'UserKnownHostsFile="{known_hosts}"',
            "-o", "IdentitiesOnly=yes", "-o", "ConnectTimeout=10", "-o", "ExitOnForwardFailure=yes",
            "-o", "ServerAliveInterval=25", "-o", "ServerAliveCountMax=3", "-i", str(ssh_key),
            "-R", f"80:127.0.0.1:{args.port}", "aircue@localhost.run",
        ]
        address_pattern = r"https://[a-z0-9-]+(?:\.app)?\.(?:lhr\.life|lhr\.rocks)\b"
    else:
        command = [args.cloudflared, "tunnel", "--no-autoupdate", "--protocol", "http2", "--url", f"http://127.0.0.1:{args.port}"]
        address_pattern = r"https://[a-z0-9-]+\.trycloudflare\.com\b"
    log_path = ROOT / "build/online-tunnel.log"
    log_path.parent.mkdir(exist_ok=True)
    active = True
    reload_requested = False
    def stop(*unused):
        nonlocal active
        active = False
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    def reload_server(*unused):
        nonlocal reload_requested
        reload_requested = True
    signal.signal(signal.SIGHUP, reload_server)
    server = None
    with log_path.open("w") as log, log_path.open(errors="replace") as tunnel_events:
        tunnel = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 60
            origin = None
            while active and time.monotonic() < deadline and tunnel.poll() is None:
                match = re.search(address_pattern, log_path.read_text(errors="replace"))
                if match:
                    origin = match.group()
                    break
                time.sleep(.2)
            if not origin:
                raise RuntimeError("The HTTPS tunnel did not provide an address; see build/online-tunnel.log")
            env = dict(os.environ, AIRCUE_ORIGIN=origin, AIRCUE_DB=cfg["database"], AIRCUE_FIRMWARE_DIR=cfg["firmware_directory"])
            server = subprocess.Popen([sys.executable, "-m", "uvicorn", "backend.app:create_app", "--factory", "--host", "127.0.0.1", "--port", str(args.port), "--no-access-log", "--forwarded-allow-ips", "127.0.0.1"], cwd=ROOT, env=env)
            manifest = ROOT / "private/online-backend.json"
            manifest.write_text(json.dumps(dict(origin=origin, port=args.port, launcher_pid=os.getpid(), tunnel_pid=tunnel.pid, server_pid=server.pid, temporary=True), indent=2) + "\n")
            manifest.chmod(0o600)
            print("Online dashboard: " + origin, flush=True)
            print("The computer and this process must stay running. Device and account authentication remain required.", flush=True)
            while active and tunnel.poll() is None:
                if any(TUNNEL_REMOVED in line for line in tunnel_events):
                    manifest.unlink(missing_ok=True)
                    raise RuntimeError("Cloudflare removed this tunnel. The supervisor will request a new address.")
                if reload_requested or server.poll() is not None:
                    if server.poll() is None:
                        server.terminate()
                        try: server.wait(timeout=5)
                        except subprocess.TimeoutExpired: server.kill(); server.wait()
                    server = subprocess.Popen([sys.executable, "-m", "uvicorn", "backend.app:create_app", "--factory", "--host", "127.0.0.1", "--port", str(args.port), "--no-access-log", "--forwarded-allow-ips", "127.0.0.1"], cwd=ROOT, env=env)
                    manifest.write_text(json.dumps(dict(origin=origin, port=args.port, launcher_pid=os.getpid(), tunnel_pid=tunnel.pid, server_pid=server.pid, temporary=True), indent=2) + "\n")
                    manifest.chmod(0o600); reload_requested = False
                    time.sleep(1)
                time.sleep(.5)
            if active:
                raise RuntimeError("An online backend process exited unexpectedly")
        finally:
            for process in (server, tunnel):
                if process is not None and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == "__main__":
    main()
