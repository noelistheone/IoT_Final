#!/usr/bin/env python3
"""Create local credentials and a software OTA signing key without exposing secrets."""
import argparse
import json
import os
from pathlib import Path
import re
import secrets
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def prepare_mqtt(directory):
    path = directory / "mqtt.json"
    if not path.exists():
        return
    path.chmod(0o600)
    try:
        broker = json.loads(path.read_text())
    except (ValueError, UnicodeError):
        raise SystemExit("Invalid JSON in private/mqtt.json")
    if not isinstance(broker, dict) or any(
        not isinstance(broker.get(key), str) or not broker[key] or not broker[key].isprintable()
        for key in ("host", "username", "password")
    ):
        raise SystemExit("Set host, username and password in private/mqtt.json")
    if not re.fullmatch(r"[A-Za-z0-9.-]+", broker["host"]) or broker.get("port") != 8883:
        raise SystemExit("Use a broker hostname and TLS port 8883 in private/mqtt.json")
    header = ["#pragma once", "// Generated from the local MQTT configuration."]
    for macro, field in (("HOST", "host"), ("USER", "username"), ("PASSWORD", "password")):
        header.append(f"#define AIRCUE_MQTT_{macro} " + json.dumps(broker[field], ensure_ascii=False))
    destination = directory / "mqtt-secrets.hpp"
    destination.write_text("\n".join(header) + "\n")
    destination.chmod(0o600)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cloud-origin")
    parser.add_argument("--device-id")
    trust=parser.add_mutually_exclusive_group()
    trust.add_argument("--cloud-ca",type=Path,help="Private CA certificate for a local HTTPS backend")
    trust.add_argument("--system-ca",action="store_true",help="Use the firmware's public CA bundle")
    args = parser.parse_args()
    origin = args.cloud_origin or "https://aircue.example.invalid"
    device_id = args.device_id or "aircue-" + secrets.token_hex(4)
    if not origin.startswith("https://") or "/" in origin[8:] or "@" in origin or any(c.isspace() for c in origin):
        parser.error("Use an HTTPS origin without a trailing slash or path")
    if not device_id.isascii() or not all(c.isalnum() or c in "-_" for c in device_id) or len(device_id) > 40:
        parser.error("Invalid device ID")
    directory = ROOT / "private"
    directory.mkdir(mode=0o700, exist_ok=True)
    os.chmod(directory, 0o700)
    prepare_mqtt(directory)
    path = directory / "provision.json"
    if path.exists():
        data = json.loads(path.read_text())
        if args.device_id and args.device_id != data["device_id"]:
            parser.error("An existing device ID is immutable; use a separate provisioning directory for another device")
        if args.cloud_origin:
            data["cloud_origin"] = origin
    else:
        data = dict(device_id=device_id, ap_password=secrets.token_urlsafe(18),
                    admin_password=secrets.token_urlsafe(24), device_token=secrets.token_urlsafe(32),
                    cloud_origin=origin, cloud_username="admin", cloud_password=secrets.token_urlsafe(24))
    if args.cloud_ca:
        certificate=args.cloud_ca.read_text()
        if "PRIVATE KEY" in certificate or certificate.count("-----BEGIN CERTIFICATE-----")!=1:
            parser.error("Supply exactly one CA certificate, never a private key")
        subprocess.run(["openssl","x509","-in",str(args.cloud_ca),"-noout"],check=True,capture_output=True)
        data["cloud_ca_pem"]=certificate
    elif args.system_ca:
        data.pop("cloud_ca_pem",None)
    path.write_text(json.dumps(data, indent=2) + "\n")
    os.chmod(path, 0o600)
    cloud = {name: data[name] for name in ("device_id", "device_token", "cloud_username", "cloud_password")}
    (directory / "cloud-provision.json").write_text(json.dumps(cloud, indent=2) + "\n")
    os.chmod(directory / "cloud-provision.json", 0o600)
    mapping = {"AIRCUE_DEVICE_ID": "device_id", "AIRCUE_AP_PASSWORD": "ap_password",
               "AIRCUE_ADMIN_PASSWORD": "admin_password", "AIRCUE_DEVICE_TOKEN": "device_token",
               "AIRCUE_CLOUD_ORIGIN": "cloud_origin"}
    header = "#pragma once\n// Generated locally; never commit this file.\n"
    header += "\n".join("#define %s %s" % (key, json.dumps(data[value])) for key, value in mapping.items()) + "\n"
    if data.get("cloud_ca_pem"):
        header+="#define AIRCUE_CLOUD_CA_PEM "+json.dumps(data["cloud_ca_pem"])+"\n"
    (directory / "secrets.hpp").write_text(header)
    os.chmod(directory / "secrets.hpp", 0o600)
    key = directory / "ota-signing-key.pem"
    if not key.exists():
        subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key)], check=True)
        os.chmod(key, 0o600)
    print("Local provisioning files are ready in private/. Existing credentials were preserved.")

if __name__ == "__main__":
    main()
