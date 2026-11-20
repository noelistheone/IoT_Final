"""Prepare the pinned ESP-IDF build and sign the application image."""
import os
from pathlib import Path
import subprocess
import sys

Import("env")
root = Path(env.subst("$PROJECT_DIR"))
if not (root / "private/secrets.hpp").is_file():
    raise RuntimeError("Run python3 tools/provision.py before building")

sys.path.insert(0, str(root / "tools"))
from esp_idf_patches import generate

framework = Path(env.PioPlatform().get_package_dir("framework-espidf"))
env.PrependUnique(CPPPATH=[str(generate(root, framework))])

build = Path(env.subst("$BUILD_DIR"))
build.mkdir(parents=True, exist_ok=True)
signing_key = root / "private/ota-signing-key.pem"
espsecure = Path(env.PioPlatform().get_package_dir("tool-esptoolpy")) / "espsecure.py"
python = os.environ.get("AIRCUE_SIGN_PYTHON", env.subst("$PYTHONEXE"))
command = [python, str(espsecure)]
if os.environ.get("AIRCUE_SIGN_ARCH") == "arm64":
    command = ["/usr/bin/arch", "-arm64", *command]

public = build / "signature_verification_key.bin"
subprocess.run([*command, "extract_public_key", "--keyfile", str(signing_key), str(public)], check=True)
data = public.read_bytes()
assembly = f"""\
.section .rodata.embedded
.global signature_verification_key_bin
.global _binary_signature_verification_key_bin_start
signature_verification_key_bin:
_binary_signature_verification_key_bin_start:
.byte {",".join(map(str, data))}
.global _binary_signature_verification_key_bin_end
_binary_signature_verification_key_bin_end:
.global signature_verification_key_bin_length
signature_verification_key_bin_length:
.long {len(data)}
"""

# Both application and bootloader link the same verification key.
bootloader = build / "bootloader"
bootloader.mkdir(exist_ok=True)
for directory in (build, bootloader):
    source = directory / "signature_verification_key.bin.S"
    if not source.exists() or source.read_text() != assembly:
        source.write_text(assembly)
(bootloader / public.name).write_bytes(data)


def sign_firmware(target, source, env):
    image = Path(str(target[0]))
    signed = image.with_suffix(".signed.bin")
    subprocess.run([*command, "sign_data", "--version", "1", "--keyfile", str(signing_key),
                    "--output", str(signed), str(image)], check=True)
    subprocess.run([*command, "verify_signature", "--version", "1", "--keyfile", str(public),
                    str(signed)], check=True)
    if signed.stat().st_size > 0x160000:
        raise RuntimeError("Signed firmware exceeds the OTA slot")
    signed.replace(image)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", sign_firmware)
