#pragma once
#include "core.hpp"
#include "climate.hpp"
#include <cstddef>
#include <vector>

namespace aircue {
constexpr size_t record_size = 96;
constexpr size_t climate_record_size = 112;
enum RecordFlags : uint16_t { UtcUnknown = 1, Demo = 2, Partial = 4, Interrupted = 8, RangeWarn = 16, Bypass = 32 };
struct Record {
    uint8_t version = 1;
    uint8_t type = 1;
    uint16_t flags = 0;
    RecordId id{};
    uint64_t monotonic_ms = 0, utc_ms = 0;
    std::array<uint8_t, 68> payload{};
};
uint32_t crc32(const uint8_t* bytes, size_t length);
void put16(uint8_t* p, uint16_t v);
void put32(uint8_t* p, uint32_t v);
void put64(uint8_t* p, uint64_t v);
uint16_t get16(const uint8_t* p);
uint32_t get32(const uint8_t* p);
uint64_t get64(const uint8_t* p);
size_t encoded_size(const Record& record);
std::vector<uint8_t> encode(const Record& record);
bool decode(const uint8_t* bytes, size_t length, Record& record);
Record minute_record(const Minute& minute);
Minute read_minute(const Record& record);
void write_climate(Record& record,const Climate& climate);
Climate read_climate(const Record& record);
Record event_record(Event type, const Episode& episode, uint32_t revision, Level level);
Episode read_episode(const Record& record);
std::string hex(const uint8_t* data, size_t size);
bool unhex(const std::string& text, uint8_t* data, size_t size);
}
