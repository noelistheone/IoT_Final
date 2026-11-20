#include "aircue_i2c.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Every transfer owns the bus and has a shared 150 ms deadline. All high levels
// release an open-drain output; no device is driven against a push-pull high.
static SemaphoreHandle_t mutex;
static gpio_num_t sda_pin, scl_pin;
static unsigned half_period_us;
static int64_t deadline_us;
static bool initialized;

static void pause_half(void) { esp_rom_delay_us(half_period_us); }
static void clock_low(void) { gpio_set_level(scl_pin, 0); pause_half(); }

static esp_err_t clock_high(void) {
    if (esp_timer_get_time() >= deadline_us) return ESP_ERR_TIMEOUT;
    gpio_set_level(scl_pin, 1);
    int64_t stretch_deadline = esp_timer_get_time() + 5000;
    if (stretch_deadline > deadline_us) stretch_deadline = deadline_us;
    while (!gpio_get_level(scl_pin)) {
        if (esp_timer_get_time() >= stretch_deadline) return ESP_ERR_TIMEOUT;
        esp_rom_delay_us(2);
    }
    pause_half();
    return ESP_OK;
}

static esp_err_t start(void) {
    gpio_set_level(sda_pin, 1); pause_half();
    esp_err_t result = clock_high();
    if (result != ESP_OK) return result;
    if (!gpio_get_level(sda_pin)) return ESP_ERR_INVALID_STATE;
    gpio_set_level(sda_pin, 0); pause_half(); clock_low();
    return ESP_OK;
}

static esp_err_t finish(esp_err_t result) {
    // Always release both lines after success, NACK, or a bounded timeout.
    clock_low(); gpio_set_level(sda_pin, 0); pause_half();
    // A failed transaction still gets at most one millisecond to emit STOP.
    deadline_us = esp_timer_get_time() + 1000;
    esp_err_t stop_result = clock_high();
    gpio_set_level(sda_pin, 1); gpio_set_level(scl_pin, 1); pause_half();
    xSemaphoreGive(mutex);
    return result == ESP_OK ? stop_result : result;
}

static esp_err_t write_byte(uint8_t value) {
    for (int bit = 7; bit >= 0; --bit) {
        gpio_set_level(sda_pin, (value >> bit) & 1); pause_half();
        esp_err_t result = clock_high();
        if (result != ESP_OK) return result;
        clock_low();
    }
    gpio_set_level(sda_pin, 1); pause_half();
    esp_err_t result = clock_high();
    if (result != ESP_OK) return result;
    bool acknowledged = gpio_get_level(sda_pin) == 0;
    clock_low();
    return acknowledged ? ESP_OK : ESP_FAIL;
}

static esp_err_t read_byte(uint8_t* value, bool final) {
    *value = 0; gpio_set_level(sda_pin, 1);
    for (int bit = 7; bit >= 0; --bit) {
        pause_half();
        esp_err_t result = clock_high();
        if (result != ESP_OK) return result;
        *value |= gpio_get_level(sda_pin) << bit;
        clock_low();
    }
    gpio_set_level(sda_pin, final ? 1 : 0); pause_half();
    esp_err_t result = clock_high();
    clock_low(); gpio_set_level(sda_pin, 1);
    return result;
}

static esp_err_t acquire(uint8_t address) {
    if (!initialized || address >= 128) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(200)) != pdTRUE) return ESP_ERR_TIMEOUT;
    deadline_us = esp_timer_get_time() + 150000;
    return ESP_OK;
}

esp_err_t aircue_i2c_init(int sda, int scl, uint32_t frequency) {
    if (initialized || sda == scl || !GPIO_IS_VALID_OUTPUT_GPIO(sda) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(scl) || frequency < 1000 || frequency > 100000)
        return ESP_ERR_INVALID_ARG;
    mutex = xSemaphoreCreateMutex();
    if (!mutex) return ESP_ERR_NO_MEM;
    sda_pin = sda; scl_pin = scl;
    // One bit includes low hold, data setup, and a high sample interval.
    half_period_us = (333334 + frequency - 1) / frequency;
    gpio_config_t cfg = {0};
    cfg.pin_bit_mask = (1ULL << sda) | (1ULL << scl);
    cfg.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_set_level(sda_pin, 1); gpio_set_level(scl_pin, 1);
    esp_err_t result = gpio_config(&cfg);
    initialized = result == ESP_OK;
    if (!initialized) { vSemaphoreDelete(mutex); mutex = NULL; }
    return result;
}

esp_err_t aircue_i2c_probe(uint8_t address) {
    esp_err_t result = acquire(address);
    if (result != ESP_OK) return result;
    result = start();
    if (result == ESP_OK) result = write_byte(address << 1);
    return finish(result);
}

esp_err_t aircue_i2c_wake_secure_element(void) {
    esp_err_t result = acquire(0);
    if (result != ESP_OK) return result;
    gpio_set_level(sda_pin, 1);
    result = clock_high();
    if (result == ESP_OK && !gpio_get_level(sda_pin)) result = ESP_ERR_INVALID_STATE;
    if (result == ESP_OK) {
        // Microchip specifies at least 60 us low, then 1.5 ms continuously high.
        // Keep the bus locked for 25 ms to also cover power-on self-test variants.
        gpio_set_level(sda_pin, 0); esp_rom_delay_us(80);
        gpio_set_level(sda_pin, 1); vTaskDelay(pdMS_TO_TICKS(25) + 1);
    }
    gpio_set_level(sda_pin, 1); gpio_set_level(scl_pin, 1);
    xSemaphoreGive(mutex);
    return result;
}

esp_err_t aircue_i2c_write(uint8_t address, const uint8_t* data, size_t count) {
    if (!data || !count) return ESP_ERR_INVALID_ARG;
    esp_err_t result = acquire(address);
    if (result != ESP_OK) return result;
    result = start();
    if (result == ESP_OK) result = write_byte(address << 1);
    for (size_t i = 0; result == ESP_OK && i < count; ++i) result = write_byte(data[i]);
    return finish(result);
}

esp_err_t aircue_i2c_read(uint8_t address, uint8_t* data, size_t count) {
    if (!data || !count) return ESP_ERR_INVALID_ARG;
    esp_err_t result = acquire(address);
    if (result != ESP_OK) return result;
    result = start();
    if (result == ESP_OK) result = write_byte((address << 1) | 1);
    for (size_t i = 0; result == ESP_OK && i < count; ++i)
        result = read_byte(data + i, i + 1 == count);
    return finish(result);
}

esp_err_t aircue_i2c_write_read(uint8_t address, const uint8_t* command,
                              size_t command_size, uint8_t* data, size_t count) {
    if (!command || !command_size || !data || !count) return ESP_ERR_INVALID_ARG;
    esp_err_t result = acquire(address);
    if (result != ESP_OK) return result;
    result = start();
    if (result == ESP_OK) result = write_byte(address << 1);
    for (size_t i = 0; result == ESP_OK && i < command_size; ++i) result = write_byte(command[i]);
    if (result == ESP_OK) result = start();
    if (result == ESP_OK) result = write_byte((address << 1) | 1);
    for (size_t i = 0; result == ESP_OK && i < count; ++i)
        result = read_byte(data + i, i + 1 == count);
    return finish(result);
}
