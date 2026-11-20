#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace aircue {
enum ClimateField { RawCo2, AirTemperature, Humidity, Pressure, BarometerTemperature, ClimateFields };
struct Climate {
    std::array<double,ClimateFields> values{};
    uint16_t mask=0,co2_samples=0,barometer_samples=0;
};
class ClimateAccumulator {
    std::array<double,ClimateFields> sums_{};
    std::array<uint16_t,ClimateFields> counts_{};
public:
    void add(const Climate& sample) {
        for(unsigned i=0;i<ClimateFields;++i)
            if((sample.mask&(1U<<i)) && std::isfinite(sample.values[i]) && counts_[i]<UINT16_MAX) {sums_[i]+=sample.values[i];++counts_[i];}
    }
    Climate finish() const {
        Climate result;
        for(unsigned i=0;i<ClimateFields;++i) if(counts_[i]) {result.mask|=1U<<i;result.values[i]=sums_[i]/counts_[i];}
        result.co2_samples=counts_[RawCo2];result.barometer_samples=counts_[Pressure];return result;
    }
    void reset() {*this=ClimateAccumulator{};}
};
}
