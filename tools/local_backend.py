#!/usr/bin/env python3
"""Prepare or serve a private-CA HTTPS backend on the local network."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import socket
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
MANIFEST=ROOT/"private/local-backend.json"

def write_private(path,content):
    path.write_text(content)
    path.chmod(0o600)

def openssl(*args):
    subprocess.run(["openssl",*map(str,args)],check=True,capture_output=True)

def local_ip():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
        sock.connect(("1.1.1.1",80))
        return sock.getsockname()[0]

def prepare(host,port):
    host=str(ipaddress.IPv4Address(host or local_ip()))
    directory=ROOT/"private/local-tls"
    directory.mkdir(parents=True,mode=0o700,exist_ok=True)
    directory.chmod(0o700)
    ca_key=directory/"ca-key.pem";ca=directory/"ca.pem"
    key=directory/"server-key.pem";cert=directory/"server.pem"
    if ca_key.exists()!=ca.exists():
        raise SystemExit("Incomplete local CA; restore its matching key and certificate before continuing")
    if not ca.exists():
        config=directory/"ca.cnf"
        write_private(config,"[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ca\n[dn]\nCN=AirCue Local Development CA\n[ca]\nbasicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\n")
        openssl("genrsa","-out",ca_key,"2048")
        openssl("req","-new","-x509","-sha256","-days","365","-key",ca_key,"-out",ca,"-config",config)
    previous=json.loads(MANIFEST.read_text()) if MANIFEST.exists() else {}
    valid=cert.exists() and subprocess.run(["openssl","x509","-in",str(cert),"-checkend","86400","-noout"],capture_output=True).returncode==0
    if not valid or previous.get("host")!=host or not key.exists():
        request=directory/"server.csr";config=directory/"server.cnf"
        # The DNS entry also supports embedded TLS implementations that match
        # numeric host strings as DNS names; desktop clients use the IP SAN.
        write_private(config,f"[req]\nprompt=no\ndistinguished_name=dn\nreq_extensions=server\n[dn]\nCN={host}\n[server]\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=IP:{host},DNS:{host}\n")
        openssl("genrsa","-out",key,"2048")
        openssl("req","-new","-sha256","-key",key,"-out",request,"-config",config)
        openssl("x509","-req","-in",request,"-CA",ca,"-CAkey",ca_key,"-CAcreateserial","-out",cert,"-days","30","-sha256","-extfile",config,"-extensions","server")
    for item in directory.iterdir():
        if item.is_file(): item.chmod(0o600)
    origin=f"https://{host}:{port}"
    subprocess.run([sys.executable,str(ROOT/"tools/provision.py"),"--cloud-origin",origin,"--cloud-ca",str(ca)],check=True)
    database=ROOT/"private/aircue-local.db"
    subprocess.run([sys.executable,"-m","backend.cli","init","--db",str(database)],cwd=ROOT,check=True)
    ota=ROOT/"private/ota";ota.mkdir(mode=0o700,exist_ok=True)
    write_private(MANIFEST,json.dumps(dict(host=host,port=port,origin=origin,ca=str(ca),cert=str(cert),key=str(key),database=str(database),firmware_directory=str(ota)),indent=2)+"\n")
    print(f"Local HTTPS prepared: {origin}. Rebuild firmware to embed this origin and CA.")
    print("No system trust settings were changed. Keep this host address stable during testing.")

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("action",choices=["prepare","serve","serve-ui"])
    parser.add_argument("--host",help="LAN IPv4 address reachable by the ESP32")
    parser.add_argument("--port",type=int,default=8443)
    args=parser.parse_args()
    if not 1024<=args.port<=65535: parser.error("Use a port between 1024 and 65535")
    if args.action=="prepare":
        prepare(args.host,args.port)
    else:
        cfg=json.loads(MANIFEST.read_text())
        ui=args.action=="serve-ui"
        os.environ.update(AIRCUE_ORIGIN="http://127.0.0.1:8000" if ui else cfg["origin"],AIRCUE_DB=cfg["database"],AIRCUE_FIRMWARE_DIR=cfg["firmware_directory"])
        sys.path.insert(0,str(ROOT))
        import uvicorn
        from backend.app import create_app
        uvicorn.run(create_app(),host="127.0.0.1" if ui else "0.0.0.0",port=8000 if ui else cfg["port"],ssl_keyfile=None if ui else cfg["key"],ssl_certfile=None if ui else cfg["cert"],access_log=False)

if __name__=="__main__":
    main()
