"""Authenticated MQTT envelopes bind each payload to its exact topic."""
import hashlib
import hmac
import json

def encode(topic, value, key):
    body = json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    signature = hmac.new(key.encode(), (topic + "\n" + body).encode(), hashlib.sha256).hexdigest()
    return json.dumps(dict(body=body, signature=signature), separators=(",", ":")).encode()

def decode(topic, payload, key):
    if len(payload) > 20000:
        raise ValueError("MQTT envelope is too large")
    value = json.loads(payload)
    if set(value) != {"body", "signature"} or not isinstance(value["body"], str) or not isinstance(value["signature"], str):
        raise ValueError("Invalid MQTT envelope")
    expected = hmac.new(key.encode(), (topic + "\n" + value["body"]).encode(), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expected, value["signature"]):
        raise ValueError("Invalid MQTT signature")
    return json.loads(value["body"])
