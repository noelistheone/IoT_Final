#pragma once
#include <cstdint>
namespace aircue {
constexpr unsigned retained_minutes=4384;
constexpr unsigned retained_events=1024;
constexpr uint64_t retention_ms=72ULL*3600000;
inline bool history_expired(uint64_t now,uint64_t last_utc,bool unknown_utc) {
    // Retain unknown-clock records until the count limit; never guess their wall time.
    return !unknown_utc && last_utc && now>last_utc && now-last_utc>retention_ms+60000;
}
}
