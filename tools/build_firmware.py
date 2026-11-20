#!/usr/bin/env python3
"""Build ESP-IDF from a private, space-free staging directory."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]

def native_python(executable):
    if sys.platform == "darwin" and subprocess.run(["/usr/sbin/sysctl","-n","hw.optional.arm64"],capture_output=True,text=True).stdout.strip() == "1":
        return ["/usr/bin/arch","-arm64",str(executable)]
    return [str(executable)]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-t", "--target", action="append", default=[])
    parser.add_argument("--upload-port")
    args = parser.parse_args()
    private = ROOT / "private"
    if not all((private / name).is_file() for name in ("secrets.hpp", "ota-signing-key.pem")):
        parser.exit(1, "Run python3 tools/provision.py first\n")
    sign_env = ROOT / ".build-venv"
    sign_python = sign_env / "bin/python"
    if not sign_python.exists():
        base_python = "/usr/bin/python3" if sys.platform == "darwin" else sys.executable
        subprocess.run(native_python(base_python)+["-m", "venv", str(sign_env)], check=True)
    stamp = sign_env / "requirements.sha256"
    requirement_hash = hashlib.sha256((ROOT / "tools/requirements-build.txt").read_bytes()).hexdigest()
    if not stamp.exists() or stamp.read_text() != requirement_hash:
        subprocess.run(native_python(sign_python)+["-m", "pip", "install", "-r", str(ROOT / "tools/requirements-build.txt")], check=True)
        stamp.write_text(requirement_hash)
    pio = shutil.which("pio") or str(Path.home() / ".platformio/penv/bin/platformio")
    stage = Path(tempfile.gettempdir()) / ("aircue-build-" + hashlib.sha256(str(ROOT).encode()).hexdigest()[:12])
    stage.mkdir(mode=0o700, exist_ok=True)
    os.chmod(stage, 0o700)
    old_defaults = (stage / "sdkconfig.defaults").read_bytes() if (stage / "sdkconfig.defaults").exists() else None
    for name in ["src", "include", "components", "data"]:
        source = ROOT / name
        # Copy source snapshots, while retaining only PlatformIO's build cache.
        destination = stage / name
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(source, destination, ignore=shutil.ignore_patterns("generated_assets.hpp", "aircue_sdk_patch"))
    # Stage only build inputs. Database credentials and desktop TLS keys stay on the host.
    for directory, files in (
        ("private", ("secrets.hpp", "ota-signing-key.pem", "mqtt-secrets.hpp", "wifi-bootstrap.hpp", "recovery-bootstrap.hpp")),
        ("tools", ("prepare_firmware.py", "esp_idf_patches.py", "native_fs.py")),
    ):
        destination = stage / directory
        if destination.exists():
            shutil.rmtree(destination)
        destination.mkdir(mode=0o700)
        for filename in files:
            source = ROOT / directory / filename
            if source.is_file():
                shutil.copy2(source, destination / filename)
    for name in ["platformio.ini", "CMakeLists.txt", "partitions.csv", "sdkconfig.defaults"]:
        shutil.copy2(ROOT / name, stage / name)
    if old_defaults != (stage / "sdkconfig.defaults").read_bytes():
        (stage / "sdkconfig.esp32dev").unlink(missing_ok=True)
    # PlatformIO only watches the top-level CMake files, so explicitly invalidate
    # its configuration when an external component adds sources or dependencies.
    cmake_files=[ROOT/"CMakeLists.txt",ROOT/"src/CMakeLists.txt",ROOT/"tools/prepare_firmware.py",ROOT/"tools/esp_idf_patches.py",*sorted((ROOT/"components").rglob("CMakeLists.txt"))]
    cmake_hash=hashlib.sha256(b"".join(str(p.relative_to(ROOT)).encode()+p.read_bytes() for p in cmake_files)).hexdigest()
    cmake_stamp=stage/"cmake-inputs.sha256"
    if not cmake_stamp.exists() or cmake_stamp.read_text()!=cmake_hash:
        (stage/".pio/build/esp32dev/CMakeCache.txt").unlink(missing_ok=True)
    command = [pio, "run", "--project-dir", str(stage), "-e", "esp32dev"]
    native_fs = stage / "make_journal"
    fs_sources = [ROOT / "tools/make_journal.c", ROOT / "components/littlefs/src/littlefs/lfs.c", ROOT / "components/littlefs/src/littlefs/lfs_util.c"]
    fs_hash = hashlib.sha256(b"".join(p.read_bytes() for p in fs_sources)).hexdigest()
    fs_stamp = stage / "make_journal.sha256"
    if not native_fs.exists() or not fs_stamp.exists() or fs_stamp.read_text() != fs_hash:
        subprocess.run([shutil.which("cc"), "-std=c11", "-O2", "-DLFS_NO_DEBUG", "-DLFS_NO_WARN", "-I" + str(ROOT / "components/littlefs/src/littlefs"), *map(str, fs_sources), "-o", str(native_fs)], check=True)
        fs_stamp.write_text(fs_hash)
    for target in args.target:
        command += ["-t", target]
    if args.upload_port:
        command += ["--upload-port", args.upload_port]
    print("Building the source snapshot in", stage, flush=True)
    environment = dict(os.environ, AIRCUE_SIGN_PYTHON=str(sign_python), AIRCUE_MKFS_BINARY=str(native_fs))
    if len(native_python(sign_python))>1:
        environment["AIRCUE_SIGN_ARCH"]="arm64"
    subprocess.run(command, check=True, env=environment)
    cmake_stamp.write_text(cmake_hash)
    output = ROOT / ".pio/build/esp32dev"
    output.mkdir(parents=True, exist_ok=True)
    for name in ["firmware.bin", "firmware.elf", "firmware.map", "partitions.bin", "bootloader.bin", "littlefs.bin"]:
        source = stage / ".pio/build/esp32dev" / name
        if source.exists():
            shutil.copy2(source, output / name)
    print("Build artifacts copied to", output)

if __name__ == "__main__":
    main()
