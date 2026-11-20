#pragma once
#include "record.hpp"
#include <algorithm>

namespace aircue {
// One frame fits the default ATT payload, so live values never need JSON paging.
inline std::array<uint8_t,20> live_frame(const Climate& climate,const View& view,uint64_t now,uint64_t age,uint16_t sequence) {
    std::array<uint8_t,20> bytes{};bytes[0]=1;
    constexpr double scales[]={1,100,100,10,100};
    for(unsigned i=0;i<ClimateFields;++i) {
        double value=std::round(climate.values[i]*scales[i]);
        if((climate.mask&(1U<<i)) && std::isfinite(value) && value>=INT16_MIN && value<=INT16_MAX) {
            bytes[1]|=1U<<i;put16(bytes.data()+4+i*2,static_cast<int16_t>(value));
        }
    }
    if(view.quality==Quality::Valid && view.last_valid_ms && now>=view.last_valid_ms && now-view.last_valid_ms<=5000 && std::isfinite(view.filtered) && view.filtered>=0 && view.filtered<=32000) {
        bytes[1]|=32;put16(bytes.data()+14,static_cast<uint16_t>(std::round(view.filtered)));
    }
    if(view.range_warn) bytes[1]|=64;
    if(view.recovering) bytes[1]|=128;
    bytes[2]=static_cast<uint8_t>(view.quality);
    bytes[3]=static_cast<uint8_t>(view.level)|(static_cast<uint8_t>(view.trend)<<2);
    put16(bytes.data()+16,std::min<uint64_t>(UINT16_MAX,age));
    put16(bytes.data()+18,sequence);return bytes;
}
}
