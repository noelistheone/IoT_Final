"""Provision the local prototype database or make a consistent WAL-aware backup."""
import argparse
import json
import os
from pathlib import Path
import sqlite3

from .db import connect, initialize
from .security import password_hash, token_hash

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["init", "backup"])
    parser.add_argument("--db", default=os.getenv("AIRCUE_DB", "private/aircue.db"))
    parser.add_argument("--provision", default="private/cloud-provision.json")
    parser.add_argument("--output", default="private/aircue-backup.db")
    args = parser.parse_args()
    Path(args.db).parent.mkdir(parents=True, exist_ok=True)
    if args.action == "backup":
        if not Path(args.db).is_file():
            parser.error("The source database does not exist")
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        with sqlite3.connect(args.db) as source, sqlite3.connect(args.output) as target:
            source.backup(target)
        print("Database backup completed.")
        return
    cfg = json.loads(Path(args.provision).read_text())
    initialize(args.db)
    with connect(args.db, True) as db:
        db.execute("INSERT OR IGNORE INTO users(username,password_hash) VALUES(?,?)", (cfg["cloud_username"], password_hash(cfg["cloud_password"])))
        uid = db.execute("SELECT id FROM users WHERE username=?", (cfg["cloud_username"],)).fetchone()[0]
        db.execute("INSERT OR IGNORE INTO devices(id,owner_id,token_hash) VALUES(?,?,?)", (cfg["device_id"], uid, token_hash(cfg["device_token"])))
    print("Database provisioned. Credentials remain in the private provisioning file.")

if __name__ == "__main__":
    main()
