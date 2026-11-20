#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace aircue {
enum class Quality : uint8_t { Commissioning, Warmup, Valid, Fault };
enum class Level : uint8_t { Unassessed, Clear, Watch, High };
enum class Trend : uint8_t { Unknown, Rising, Stable, Falling };
enum class Event : uint8_t { None, Boot, EpisodeStart, Falling, Recovered, ConfigChanged,
                            SensorFault, SensorRestored, TimeAnchor, Gap, Maintenance, Config };
const char* name(Quality v);
const char* name(Level v);
const char* name(Trend v);

struct Rules {
    int32_t watch_on = 1000, watch_off = 800, high_on = 1500, high_off = 1300;
    uint32_t watch_s = 30, high_s = 60, high_clear_s = 120, clear_s = 120;
    uint32_t initial_s = 30, recovery_s = 30, trend_ppm = 100;
    bool valid() const;
    bool operator==(const Rules& o) const;
    bool operator!=(const Rules& o) const { return !(*this == o); }
};
struct RecordId {
    std::array<uint8_t, 16> boot{};
    uint32_t sequence = 0;
    bool empty() const { return sequence == 0; }
    bool operator==(const RecordId& o) const { return boot == o.boot && sequence == o.sequence; }
    std::string str() const;
};
struct Episode {
    RecordId id{};
    bool active = false, interrupted = false;
    int32_t peak = 0;
    uint64_t candidate_ms = 0, confirmed_ms = 0, falling_ms = 0, ended_ms = 0;
    uint64_t valid_ms = 0, high_ms = 0, red_ms = 0, missing_ms = 0;
};
struct Sample {
    uint64_t ms = 0;
    int32_t ppm = 0;
    Quality quality = Quality::Warmup;
    bool available = false;
};
struct View {
    Quality quality = Quality::Warmup;
    Level level = Level::Unassessed;
    Trend trend = Trend::Unknown;
    int32_t raw = 0;
    double filtered = 0;
    uint64_t last_valid_ms = 0;
    bool range_warn = false, recovering = false;
    Episode episode{};
};
struct Step {
    bool valid = false;
    uint32_t valid_ms = 0, high_ms = 0, red_ms = 0;
    Event event = Event::None;
};

class AlarmEngine {
public:
    explicit AlarmEngine(const Rules& rules = {}): rules_(rules) {}
    Step update(const Sample& sample);
    Event configure(const Rules& rules);
    void identify_episode(const RecordId& id) { view_.episode.id = id; }
    void restore(const Episode& episode, Level level);
    const View& view() const { return view_; }
private:
    struct Dwell {
        uint64_t elapsed = 0, since = 0;
        bool active = false;
        void update(bool condition, uint64_t now, uint32_t dt);
        void reset() { *this = {}; }
    };
    struct Point { uint64_t ms = 0; double ppm = 0; };
    Rules rules_;
    View view_{};
    std::array<int32_t, 5> median_{};
    size_t median_size_ = 0, median_pos_ = 0;
    std::array<Point, 64> trend_{};
    size_t trend_size_ = 0, trend_pos_ = 0;
    uint64_t last_ms_ = 0;
    bool previous_valid_ = false, initial_low_ = true, fault_latched_ = false;
    bool previous_high_ = false;
    int32_t candidate_peak_ = 0;
    Dwell initial_, recovery_, watch_, high_, leave_high_, clear_;
    void reset_candidates();
    Trend calculate_trend(uint64_t now) const;
};

struct Minute {
    int32_t mean = 0, minimum = 0, maximum = 0;
    uint16_t valid_count = 0, expected_count = 60;
    uint32_t config_revision = 0, valid_ms = 0, observed_high_ms = 0, red_valid_ms = 0, window_ms = 60000;
    Level level = Level::Unassessed;
    Trend trend = Trend::Unknown;
    uint16_t warmup_count = 0, error_count = 0, missed_count = 0;
    uint32_t window_index = UINT32_MAX;
};
class MinuteAccumulator {
public:
    void begin(uint64_t start, uint64_t end, uint32_t revision, uint32_t window_index);
    void add(const Sample& sample, const Step& step, const View& view);
    Minute finish() const;
    void truncate(uint64_t stop);
    uint64_t end() const { return end_; }
    uint64_t start() const { return start_; }
private:
    Minute minute_{};
    int64_t sum_ = 0;
    uint64_t start_ = 0, end_ = 0;
    uint64_t slots_ = 0;
    uint64_t interval_end_ = 0;
};
}
