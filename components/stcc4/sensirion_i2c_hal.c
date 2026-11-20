#include "sensirion_i2c_hal.h"
#include "aircue_i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include <stdio.h>

// The bus is configured once by SensorService; every transfer has a deadline.
void sensirion_i2c_hal_init(void) {}
void sensirion_i2c_hal_free(void) {}
int8_t sensirion_i2c_hal_read(uint8_t address, uint8_t* data, uint8_t count) {
    esp_err_t result=aircue_i2c_read(address,data,count);
    if(count==3 && result==ESP_OK)
        printf("AIRCUE_SENSOR_SELF_TEST bytes=%02x%02x%02x\n",data[0],data[1],data[2]);
    return result == ESP_OK ? 0 : -1;
}
int8_t sensirion_i2c_hal_write(uint8_t address, const uint8_t* data, uint8_t count) {
    esp_err_t result=aircue_i2c_write(address,data,count);
    if(count==2 && data[0]!=0xec)
        printf("AIRCUE_SENSOR_COMMAND command=%02x%02x result=%s\n",data[0],data[1],esp_err_to_name(result));
    return result == ESP_OK ? 0 : -1;
}
void sensirion_i2c_hal_sleep_usec(uint32_t useconds) {
    // Round up and allow one tick for phase: vTaskDelay may start just before
    // a tick interrupt. Device command execution times are minimum delays.
    if (useconds >= 1000) {
        const uint32_t tick_us = 1000000 / configTICK_RATE_HZ;
        // Leave margin when reading the self-test result on the physical board.
        uint32_t delay_us=useconds==360000?500000:useconds;
        vTaskDelay((delay_us + tick_us - 1) / tick_us + 1);
    }
    else esp_rom_delay_us(useconds);
}
int16_t sensirion_i2c_hal_select_bus(uint8_t index) { return index == 0 ? 0 : -1; }
