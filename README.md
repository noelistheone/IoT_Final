# AirCue

AirCue is an ESP32 indoor air quality monitor. It samples CO₂, temperature, relative humidity and barometric pressure every second. The OLED and three LEDs show the current air quality. The phone and desktop dashboard adds ventilation guidance, historical charts, CSV export and device settings.

The ESP32 runs on standalone power. It sends data over Wi-Fi to two independent destinations, the Mac's HTTPS receiver and the Lab 5 MQTT broker. The Mac stores readings in SQLite and imports retained MQTT history when it comes back online. Bluetooth also provides access to live readings, history and settings while offline.

## Hardware

The firmware targets the 4 MiB ESP32 DevKit using PlatformIO's `esp32dev` environment.

| Component | Connections |
| --- | --- |
| STCC4 CO₂ module with SHT41 temperature and humidity | VIN to 3V3, GND to GND, SDA to GPIO 21, SCL to GPIO 22. The STCC4 uses address `0x64`. Leave the module's 3Vo output unconnected. |
| BMP280 pressure and temperature sensor | Share the 3.3 V supply and I²C bus on GPIO 21 and 22. The firmware probes `0x76` and `0x77`. |
| SPI OLED | SCK 18, MOSI 23, CS 5, DC 16, RESET 17 |
| Red, yellow and green LEDs | GPIO 2, 4 and 15 respectively, each with a 220 Ω series resistor |
| Passive piezo buzzer | GPIO 27 and GND. Automatic sound is off by default. |
| Pairing and mute buttons | GPIO 32 and 33 respectively. Each button connects its pin to GND when pressed. |

The pin definitions are in [board.hpp](include/aircue/board.hpp). The STCC4 supplies the readings from its attached SHT41. The BMP280 reports its own temperature separately, giving the dashboard five climate series.

## Configure a new installation

Install Python 3.9 or later, PlatformIO Core, a C/C++ compiler and OpenSSL. Run these commands from the repository root.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r backend/requirements.txt
mkdir -m 700 -p private
```

For the Lab 5 upload, create `private/mqtt.json` with your broker hostname and credentials. Both the ESP32 and the Mac use this configuration. The MQTT account needs publish and subscribe access under the device's `aircue/<device_id>/v1/` topic prefix, including retained messages.

```json
{
  "host": "broker.example.com",
  "port": 8883,
  "username": "YOUR_MQTT_USERNAME",
  "password": "YOUR_MQTT_PASSWORD"
}
```

Prepare the Mac receiver on the LAN interface reachable by the ESP32. The command detects the local IPv4 address. Pass `--host YOUR_LAN_IPV4` to select a specific interface.

```sh
python tools/local_backend.py prepare
```

This creates the SQLite database, device identity, dashboard login, local TLS certificates and firmware signing key. It also generates the firmware's MQTT header when the broker configuration is present. Sign in with `cloud_username` and `cloud_password` from `private/provision.json`. Keep `private/` backed up outside Git. An existing installation needs its original device token and signing key to keep its identity and accept updates.

## Build and flash

```sh
python tools/build_firmware.py
python tools/verify_firmware.py
```

The build uses the pinned ESP-IDF toolchain and produces a signed image in `.pio/build/esp32dev/firmware.bin`. It embeds the device credentials. Keep compiled images private.

For the **first installation on a blank board**, connect the ESP32 over USB and run both commands. The second command initializes the 1 MiB LittleFS journal **and erases any history already in that partition**.

```sh
python tools/build_firmware.py -t upload
python tools/build_firmware.py -t uploadfs
```

Use `--upload-port PORT` to select a serial device. For subsequent updates, place the signed image at `<firmware_directory>/<device_id>.bin` on the receiver and choose **Settings → Sensor and recording → Maintenance → Check for updates**. The paths are recorded in `private/local-backend.json`. The board verifies the signature before switching OTA slots. Rebuild after changing the embedded broker credentials or receiver address.

## Run the dashboard

```sh
python tools/desktop_services.py
```

Open [AirCue on this computer](http://127.0.0.1:8000). The supervisor starts the local dashboard, the ESP32 HTTPS receiver on port 8443 and the MQTT history bridge when configured.

For access from other networks, install `cloudflared` on the Mac before starting the supervisor. It can also use an OpenSSH tunnel to localhost.run when a dedicated SSH key and verified host keys are supplied in `private/web-tunnel.key` and `private/web-tunnel.known_hosts`. Open the HTTPS `origin` written to `private/online-backend.json` on the phone or another computer. The Mac must be awake, online and running the service. The public address of a temporary tunnel can change.

For automatic startup after macOS login, stop the foreground supervisor and run the installer.

```sh
python tools/install_autostart.py
```

The login service keeps its own copy in `~/Library/Application Support/AirCue`. Its `private/online-backend.json` contains the active public address. The project records the installed runtime and database locations in `private/desktop-install.json`. Run the installer again after updating the source. Use `--remove` to remove the login service.

## Pair the board and add Wi-Fi

1. Open the dashboard in Chrome on Mac or Bluefy on iPhone. On the phone, load the HTTPS page once while online so the browser can cache it for offline use.
2. Hold button 1 (GPIO 32) for two seconds. Choose **Connect via Bluetooth**, select the nearby `AirCue-BLE-…` device and enter the six-digit code shown on its OLED.
3. In **Settings → Wi-Fi**, add a 2.4 GHz network and select **Enable Wi-Fi**. The board remembers up to eight profiles across power cycles and tries them in priority order.
4. Open **Overview** for current readings and ventilation advice. In **History**, choose a metric or sync the board's stored records. Bluetooth supports one client at a time.

## Measurements and storage

- The dashboard distinguishes raw CO₂ during preparation from validated CO₂ used for alerts. Commissioning requires 12 hours of continuous sensing, a passing self-test and user confirmation of fresh-air exposure. A cold start has a one-hour warm-up.
- Green, yellow and red indicate clear, watch and high CO₂ levels. Thresholds and delays are adjustable in **Settings → Alerts**. Button 2 mutes the current alert.
- The board stores one-minute summaries and events. Its rolling journal targets 72 hours of minute data and prunes old segments. The capacity is 4,384 minute records and 1,024 events. MQTT retained blocks hold up to 5,120 minute records and 1,024 events. The Mac deduplicates direct uploads and replayed records by device, boot and sequence.
- The retained MQTT state topic is `aircue/<device_id>/v1/state`. Its JSON envelope contains a `body` string and a `signature`. After verifying the signature, parse `body` as JSON and read `status.climate`. CO₂ fields are JSON numbers with two decimal places on the wire, for example `"co2_raw_ppm": 824.00`. The STCC4's raw output has whole-ppm resolution. Missing measurements use `null`.
- A formal 72-hour recording starts from **Settings → Sensor and recording** once the device has valid commissioned readings, UTC and demo mode turned off. To analyze an exported run, use `python tools/analyze_run.py readings.csv --run-id RUN_ID`.

## Source

| Directory | Contents |
| --- | --- |
| `src/`, `include/aircue/` | Firmware, climate processing, alerts, journal, BLE, Wi-Fi, MQTT and OTA |
| `components/` | Sensirion STCC4 driver and LittleFS sources with their upstream licenses |
| `backend/` | FastAPI endpoints, device authentication, MQTT message verification and SQLite storage |
| `web/` | Responsive dashboard and offline browser cache |
| `tools/` | Provisioning, firmware build and verification, desktop services and recording analysis |
