import base64
import hashlib
import hmac
import secrets

def token_hash(value):
    return hashlib.sha256(value.encode()).hexdigest()

def password_hash(password):
    salt = secrets.token_bytes(16)
    value = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 310000)
    return "pbkdf2_sha256$310000$%s$%s" % (salt.hex(), value.hex())

def verify_password(password, encoded):
    try:
        scheme, iterations, salt, expected = encoded.split("$")
        if scheme != "pbkdf2_sha256":
            return False
        value = hashlib.pbkdf2_hmac("sha256", password.encode(), bytes.fromhex(salt), int(iterations)).hex()
        return hmac.compare_digest(value, expected)
    except (ValueError, TypeError):
        return False

def encode_cursor(row):
    return base64.urlsafe_b64encode(str(row).encode()).decode().rstrip("=")

def decode_cursor(cursor):
    if not cursor:
        return 0
    try:
        raw = base64.urlsafe_b64decode(cursor + "=" * (-len(cursor) % 4)).decode()
        if not raw.isdecimal() or len(raw) > 19:
            raise ValueError()
        value = int(raw)
        if value > 2**63 - 1:
            raise ValueError()
        return value
    except (ValueError, UnicodeError) as exc:
        raise ValueError("Invalid history cursor") from exc
