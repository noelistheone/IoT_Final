#pragma once
#include <cstdint>

namespace aircue {
struct SensorSampleCheck {
    bool frame_ok;
    bool status_clear;
    bool statistics_ok() const {return frame_ok && status_clear;}
};
inline SensorSampleCheck check_sensor_sample(int16_t error,uint16_t status,int16_t ppm) {
    // A CRC-checked frame and the measurement status are separate facts.
    // Undocumented status bits do not prove a transport failure. Keep raw
    // previews available, but exclude flagged readings from formal statistics.
    return {error==0 && ppm>=380 && ppm<=32000,status==0};
}
}
