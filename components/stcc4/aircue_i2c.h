#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t aircue_i2c_init(int sda,int scl,uint32_t frequency);
esp_err_t aircue_i2c_probe(uint8_t address);
esp_err_t aircue_i2c_wake_secure_element(void);
esp_err_t aircue_i2c_read(uint8_t address,uint8_t* data,size_t count);
esp_err_t aircue_i2c_write(uint8_t address,const uint8_t* data,size_t count);
esp_err_t aircue_i2c_write_read(uint8_t address,const uint8_t* command,size_t command_size,uint8_t* data,size_t count);
#ifdef __cplusplus
}
#endif
