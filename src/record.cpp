#include "aircue/record.hpp"
#include <algorithm>
#include <cstring>

namespace aircue {
void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = v >> (8*i); }
void put64(uint8_t* p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = v >> (8*i); }
uint16_t get16(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }
uint32_t get32(const uint8_t* p) { uint32_t n = 0; for (unsigned i = 0; i < 4; ++i) n |= uint32_t(p[i]) << (8*i); return n; }
uint64_t get64(const uint8_t* p) { uint64_t n = 0; for (unsigned i = 0; i < 8; ++i) n |= uint64_t(p[i]) << (8*i); return n; }
uint32_t crc32(const uint8_t* bytes, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}
size_t encoded_size(const Record& r) {return r.version==2?climate_record_size:record_size;}
std::vector<uint8_t> encode(const Record& r) {
    std::vector<uint8_t> out(encoded_size(r));
    out[0] = r.version; out[1] = r.type; put16(out.data() + 2, r.flags);
    std::copy(r.id.boot.begin(), r.id.boot.end(), out.begin() + 4);
    put32(out.data() + 20, r.id.sequence); put64(out.data() + 24, r.monotonic_ms); put64(out.data() + 32, r.utc_ms);
    std::copy_n(r.payload.begin(),out.size()-44,out.begin()+40);
    put32(out.data()+out.size()-4,crc32(out.data(),out.size()-4));
    return out;
}
bool decode(const uint8_t* p, size_t size, Record& r) {
    if (!((size==record_size && p[0]==1) || (size==climate_record_size && p[0]==2 && p[1]==1)) || (p[1]!=1 && p[1]!=2) || crc32(p,size-4)!=get32(p+size-4)) return false;
    r=Record{};r.version=p[0];r.type = p[1]; r.flags = get16(p+2); std::copy(p+4, p+20, r.id.boot.begin());
    r.id.sequence = get32(p+20); r.monotonic_ms = get64(p+24); r.utc_ms = get64(p+32);
    std::copy(p+40,p+size-4,r.payload.begin()); return r.id.sequence != 0;
}
Record minute_record(const Minute& m) {
    Record r;r.version=2; auto* p = r.payload.data();
    put32(p, m.mean); put32(p+4, m.minimum); put32(p+8, m.maximum);
    put16(p+12, m.valid_count); put16(p+14, m.expected_count); put32(p+16, m.config_revision);
    put32(p+20, m.valid_ms); put32(p+24, m.observed_high_ms); put32(p+28, m.red_valid_ms); put32(p+32, m.window_ms);
    p[36] = static_cast<uint8_t>(m.level); p[37] = static_cast<uint8_t>(m.trend);
    put16(p+38, m.warmup_count); put16(p+40, m.error_count); put16(p+42, m.missed_count); put32(p+44, m.window_index);
    return r;
}
Minute read_minute(const Record& r) {
    const auto* p = r.payload.data(); Minute m;
    m.mean = static_cast<int32_t>(get32(p)); m.minimum = static_cast<int32_t>(get32(p+4)); m.maximum = static_cast<int32_t>(get32(p+8));
    m.valid_count = get16(p+12); m.expected_count = get16(p+14); m.config_revision = get32(p+16);
    m.valid_ms = get32(p+20); m.observed_high_ms = get32(p+24); m.red_valid_ms = get32(p+28); m.window_ms = get32(p+32);
    m.level = static_cast<Level>(p[36]); m.trend = static_cast<Trend>(p[37]);
    m.warmup_count = get16(p+38); m.error_count = get16(p+40); m.missed_count = get16(p+42); m.window_index = get32(p+44);
    return m;
}
static constexpr double climate_scale[]={1,100,100,10,100};
void write_climate(Record& r,const Climate& climate) {
    if(r.type!=1) return;
    r.version=2;auto* p=r.payload.data()+48;uint16_t mask=0;
    for(unsigned i=0;i<ClimateFields;++i) if(climate.mask&(1U<<i)) {
        double value=std::round(climate.values[i]*climate_scale[i]);
        if(std::isfinite(value) && value>=INT16_MIN && value<=INT16_MAX) {put16(p+i*2,static_cast<int16_t>(value));mask|=1U<<i;}
    }
    put16(p+10,mask);put16(p+12,climate.co2_samples);put16(p+14,climate.barometer_samples);
}
Climate read_climate(const Record& r) {
    Climate result;if(r.version!=2 || r.type!=1) return result;
    const auto* p=r.payload.data()+48;result.mask=get16(p+10);
    for(unsigned i=0;i<ClimateFields;++i) result.values[i]=static_cast<int16_t>(get16(p+i*2))/climate_scale[i];
    result.co2_samples=get16(p+12);result.barometer_samples=get16(p+14);return result;
}
Record event_record(Event e, const Episode& ep, uint32_t revision, Level level) {
    Record r; r.type = 2; auto* p = r.payload.data(); p[0] = static_cast<uint8_t>(e); p[1] = static_cast<uint8_t>(level);
    std::copy(ep.id.boot.begin(), ep.id.boot.end(), p+4); put32(p+20, ep.id.sequence); put32(p+24, ep.peak);
    put32(p+28, std::min<uint64_t>(UINT32_MAX, ep.high_ms)); put32(p+32, std::min<uint64_t>(UINT32_MAX, ep.red_ms));
    put32(p+36, std::min<uint64_t>(UINT32_MAX, ep.valid_ms)); put32(p+40, revision);
    put64(p+44, e == Event::EpisodeStart ? ep.candidate_ms : e == Event::Falling ? ep.falling_ms : ep.missing_ms);
    if (ep.interrupted) r.flags |= Interrupted;
    return r;
}
Episode read_episode(const Record& r) {
    Episode e; const auto* p = r.payload.data();
    std::copy(p+4, p+20, e.id.boot.begin()); e.id.sequence = get32(p+20);
    if (p[0] == static_cast<uint8_t>(Event::EpisodeStart)) { e.id = r.id; e.candidate_ms = get64(p+44); e.confirmed_ms = r.monotonic_ms; }
    e.peak = static_cast<int32_t>(get32(p+24)); e.high_ms = get32(p+28); e.red_ms = get32(p+32); e.valid_ms = get32(p+36);
    e.interrupted = r.flags & Interrupted;
    if(p[0]==static_cast<uint8_t>(Event::Recovered)||p[0]==static_cast<uint8_t>(Event::ConfigChanged)) e.missing_ms=get64(p+44);
    e.active = p[0] == static_cast<uint8_t>(Event::EpisodeStart) || p[0] == static_cast<uint8_t>(Event::Falling);
    return e;
}
std::string hex(const uint8_t* data, size_t size) {
    static const char digits[] = "0123456789abcdef"; std::string s(size*2, '0');
    for (size_t i=0; i<size; ++i) { s[2*i]=digits[data[i]>>4]; s[2*i+1]=digits[data[i]&15]; } return s;
}
bool unhex(const std::string& s, uint8_t* data, size_t size) {
    if (s.size()!=size*2) return false;
    auto nibble=[](char c)->int { if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; return -1; };
    for(size_t i=0;i<size;++i) { int a=nibble(s[2*i]),b=nibble(s[2*i+1]); if(a<0||b<0)return false; data[i]=(a<<4)|b; } return true;
}
}
