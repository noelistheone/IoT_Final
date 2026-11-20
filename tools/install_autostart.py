#!/usr/bin/env python3
"""Install or remove only AirCue's per-user macOS login service."""
import argparse
import os
from pathlib import Path
import platform
import plistlib
import json
import shutil
import sqlite3
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LABEL = "com.aircue.desktop"
RUNTIME = Path.home() / "Library/Application Support/AirCue"

def native_command(python):
    arm = platform.machine() == "arm64" or subprocess.run(
        ["/usr/sbin/sysctl", "-n", "hw.optional.arm64"], capture_output=True, text=True
    ).stdout.strip() == "1"
    return ["/usr/bin/arch", "-arm64", str(python)] if arm else [str(python)]

def stage_runtime():
    # launchd cannot read Documents without broad privacy access. Install runtime
    # files in Application Support, the standard location for a user service.
    RUNTIME.mkdir(parents=True, mode=0o700, exist_ok=True); RUNTIME.chmod(0o700)
    for name in ("backend", "web"):
        shutil.copytree(ROOT / name, RUNTIME / name, dirs_exist_ok=True, ignore=shutil.ignore_patterns("__pycache__"))
    for name in ("tools", "private", "build", "private/bin", "private/local-tls", "private/ota"):
        (RUNTIME / name).mkdir(parents=True, exist_ok=True, mode=0o700)
    for name in ("desktop_services.py", "local_backend.py", "mqtt_bridge.py", "online_backend.py"):
        shutil.copy2(ROOT / "tools" / name, RUNTIME / "tools" / name)
    for name in ("provision.json", "mqtt.json", "web-tunnel.key", "web-tunnel.known_hosts"):
        source = ROOT / "private" / name
        if source.is_file():
            shutil.copy2(source, RUNTIME / "private" / name)
            (RUNTIME / "private" / name).chmod(0o600)
    tunnel = ROOT / "private/bin/cloudflared"
    tunnel_binary = tunnel if tunnel.is_file() else shutil.which("cloudflared")
    if tunnel_binary:
        shutil.copy2(tunnel_binary, RUNTIME / "private/bin/cloudflared")
    cfg = json.loads((ROOT / "private/local-backend.json").read_text())
    database = RUNTIME / "private/aircue-local.db"
    if not database.exists():
        with sqlite3.connect(cfg["database"]) as source, sqlite3.connect(database) as destination:
            source.backup(destination)
        database.chmod(0o600)
    for key in ("ca", "cert", "key"):
        source = Path(cfg[key]); destination = RUNTIME / "private/local-tls" / source.name
        if source != destination:
            shutil.copy2(source, destination)
        destination.chmod(0o600); cfg[key] = str(destination)
    old_firmware = Path(cfg["firmware_directory"])
    new_firmware = RUNTIME / "private/ota"
    if old_firmware != new_firmware:
        shutil.copytree(old_firmware, new_firmware, dirs_exist_ok=True)
    cfg.update(database=str(database), firmware_directory=str(new_firmware))
    for manifest in (ROOT / "private/local-backend.json", RUNTIME / "private/local-backend.json"):
        manifest.write_text(json.dumps(cfg, indent=2) + "\n"); manifest.chmod(0o600)
    python = RUNTIME / ".venv/bin/python"
    if not python.exists():
        subprocess.run([*native_command("/usr/bin/python3"), "-m", "venv", str(RUNTIME / ".venv")], check=True)
    subprocess.run([*native_command(python), "-m", "pip", "install", "-r", str(RUNTIME / "backend/requirements.txt")], check=True)
    manifest = ROOT / "private/desktop-install.json"
    manifest.write_text(json.dumps(dict(runtime=str(RUNTIME), database=str(database)), indent=2) + "\n"); manifest.chmod(0o600)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--remove", action="store_true")
    args = parser.parse_args()
    directory = Path.home() / "Library/LaunchAgents"
    path = directory / (LABEL + ".plist")
    target = "gui/" + str(os.getuid())
    subprocess.run(["launchctl", "bootout", target + "/" + LABEL], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if args.remove:
        path.unlink(missing_ok=True)
        print("AirCue login service removed. Project data was preserved.")
        return
    directory.mkdir(parents=True, exist_ok=True)
    stage_runtime()
    document = dict(Label=LABEL, ProgramArguments=[*native_command(RUNTIME / ".venv/bin/python"), str(RUNTIME / "tools/desktop_services.py")],
                    WorkingDirectory=str(RUNTIME), RunAtLoad=True, KeepAlive=True, ThrottleInterval=30,
                    StandardOutPath=str(RUNTIME / "build/desktop-services.log"), StandardErrorPath=str(RUNTIME / "build/desktop-services.log"))
    path.write_bytes(plistlib.dumps(document)); path.chmod(0o600)
    subprocess.run(["launchctl", "bootstrap", target, str(path)], check=True)
    print("AirCue will start after this macOS user logs in, and restore remote MQTT history automatically.")

if __name__ == "__main__":
    main()
