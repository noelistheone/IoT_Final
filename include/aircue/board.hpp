#pragma once

// These assignments follow Lab 1's Fall 2026 kit pin map.
// Keep the kit's LED resistors and passive piezo wiring in place.
namespace aircue::board {
constexpr int sda = 21;
constexpr int scl = 22;
// A conservative open-drain software bus passed physical STCC4 self-tests.
constexpr unsigned i2c_clock_hz = 5000;
constexpr int green = 15;
constexpr int yellow = 4;
constexpr int red = 2;
constexpr int buzzer = 27;
constexpr bool passive_buzzer = true;
constexpr int button1 = 32, button2 = 33, potentiometer = 34, servo = 13;
constexpr int spi_sck = 18, spi_mosi = 23, spi_miso = 19;
constexpr int oled_cs = 5, oled_dc = 16, oled_reset = 17;
constexpr int lora_cs = 14, lora_reset = 26, lora_irq = 25;
// Enable identity-only checks after the owner repaired the optional modules.
constexpr bool lora_probe_enabled = true;
constexpr bool atecc_probe_enabled = true;
constexpr bool led_active_high = true;
constexpr int buzzer_frequency_hz = 2400;
constexpr unsigned warmup_ms = 3600000;
constexpr unsigned commissioning_ms = 12 * 3600000;
constexpr unsigned i2c_timeout_ms = 150;
}
