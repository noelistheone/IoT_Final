#!/usr/bin/env python3
"""Verify a signed release and reject unsigned, modified, and wrong-key images."""
from pathlib import Path
import subprocess
import tempfile
from build_firmware import native_python

ROOT = Path(__file__).resolve().parents[1]

def main():
    python = ROOT / ".build-venv/bin/python"
    python_command = native_python(python)
    image = ROOT / ".pio/build/esp32dev/firmware.bin"
    key = ROOT / "private/ota-signing-key.pem"
    assert image.stat().st_size <= 0x160000
    with tempfile.TemporaryDirectory(prefix="aircue-signature-") as temp:
        directory = Path(temp)
        public = directory / "public.bin"
        subprocess.run(python_command + ["-m", "espsecure", "extract_public_key", "--keyfile", str(key), str(public)], check=True, capture_output=True)
        def check(path, public_key):
            return subprocess.run(python_command + ["-m", "espsecure", "verify_signature", "--version", "1", "--keyfile", str(public_key), str(path)], capture_output=True).returncode == 0
        assert check(image, public), "Valid image was rejected"
        raw = image.read_bytes()
        changed = bytearray(raw); changed[256] ^= 0x80
        tampered = directory / "tampered.bin"; tampered.write_bytes(changed)
        unsigned = directory / "unsigned.bin"; unsigned.write_bytes(raw[:-68])
        wrong_key = directory / "wrong.pem"
        subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(wrong_key)], check=True)
        other_public = directory / "wrong-public.bin"
        subprocess.run(python_command + ["-m", "espsecure", "extract_public_key", "--keyfile", str(wrong_key), str(other_public)], check=True, capture_output=True)
        assert not check(tampered, public), "Tampered image was accepted"
        assert not check(unsigned, public), "Unsigned image was accepted"
        assert not check(image, other_public), "Wrong signing key was accepted"
    print("Signature checks passed: valid accepted; unsigned, tampered, and wrong-key images rejected.")

if __name__ == "__main__":
    main()
