"""SQLite connections enforce durability before any ingestion acknowledgment."""
from contextlib import contextmanager
import sqlite3

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, username TEXT UNIQUE NOT NULL, password_hash TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS devices (
 id TEXT PRIMARY KEY, owner_id INTEGER NOT NULL REFERENCES users(id), token_hash TEXT UNIQUE NOT NULL,
 status TEXT, status_received INTEGER, boot_ordinal INTEGER NOT NULL DEFAULT 0, status_mono INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS records (
 row_id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT NOT NULL REFERENCES devices(id),
 boot_id TEXT NOT NULL, sequence INTEGER NOT NULL, wire TEXT NOT NULL, run_id TEXT NOT NULL,
 run_t0 INTEGER NOT NULL, decoded TEXT NOT NULL, received_at INTEGER NOT NULL,
 UNIQUE(device_id,boot_id,sequence)
);
CREATE INDEX IF NOT EXISTS record_history ON records(device_id,row_id);
CREATE TABLE IF NOT EXISTS gaps (
 device_id TEXT NOT NULL REFERENCES devices(id), gap_id TEXT NOT NULL, body TEXT NOT NULL,
 received_at INTEGER NOT NULL, PRIMARY KEY(device_id,gap_id)
);
CREATE TABLE IF NOT EXISTS time_anchors (
 device_id TEXT NOT NULL, boot_id TEXT NOT NULL, monotonic_ms INTEGER NOT NULL, utc_ms INTEGER NOT NULL,
 record_id TEXT NOT NULL, PRIMARY KEY(device_id,record_id)
);
CREATE INDEX IF NOT EXISTS anchors_boot ON time_anchors(device_id,boot_id,monotonic_ms);
CREATE TABLE IF NOT EXISTS config_snapshots (
 device_id TEXT NOT NULL, revision INTEGER NOT NULL, body TEXT NOT NULL,
 PRIMARY KEY(device_id,revision)
);
CREATE TABLE IF NOT EXISTS sessions (
 token_hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id), csrf TEXT NOT NULL, expires_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS commands (
 device_id TEXT NOT NULL REFERENCES devices(id), request_id TEXT NOT NULL, body TEXT NOT NULL,
 status TEXT NOT NULL, result TEXT, created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL,
 PRIMARY KEY(device_id,request_id)
);
CREATE TABLE IF NOT EXISTS login_limits (peer TEXT PRIMARY KEY, since_ms INTEGER NOT NULL, attempts INTEGER NOT NULL);
"""

@contextmanager
def connect(path, write=False):
    db = sqlite3.connect(path, timeout=5, isolation_level=None)
    db.row_factory = sqlite3.Row
    try:
        db.execute("PRAGMA foreign_keys=ON")
        db.execute("PRAGMA synchronous=FULL")
        if db.execute("PRAGMA synchronous").fetchone()[0] != 2:
            raise RuntimeError("SQLite FULL durability is unavailable")
        db.execute("BEGIN IMMEDIATE" if write else "BEGIN")
        yield db
        db.commit()
    except BaseException:
        db.rollback()
        raise
    finally:
        db.close()

def initialize(path):
    with sqlite3.connect(path) as db:
        if db.execute("PRAGMA journal_mode=WAL").fetchone()[0].lower() != "wal":
            raise RuntimeError("SQLite WAL is unavailable")
        db.execute("PRAGMA synchronous=FULL")
        db.executescript(SCHEMA)
