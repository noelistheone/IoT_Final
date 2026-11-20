idf_component_register(SRCS "stcc4_i2c.c" "sensirion_common.c" "sensirion_i2c.c" "sensirion_i2c_hal.c" "aircue_i2c.c"
                       INCLUDE_DIRS "." "../../include" REQUIRES driver esp_timer)
