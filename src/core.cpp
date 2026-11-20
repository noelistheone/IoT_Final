#include "aircue/core.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aircue {
const char* name(Quality v) { static const char* n[] = {"COMMISSIONING","WARMUP","VALID","FAULT"}; return n[static_cast<unsigned>(v) % 4]; }
const char* name(Level v) { static const char* n[] = {"UNASSESSED","CLEAR","WATCH","HIGH"}; return n[static_cast<unsigned>(v) % 4]; }
const char* name(Trend v) { static const char* n[] = {"UNKNOWN","RISING","STABLE","FALLING"}; return n[static_cast<unsigned>(v) % 4]; }
bool Rules::valid() const {
    return 380 <= watch_off && watch_off < watch_on && watch_on < high_off && high_off < high_on && high_on <= 5000
        && watch_s >= 1 && watch_s <= 3600 && high_s >= 1 && high_s <= 3600
        && high_clear_s >= 1 && high_clear_s <= 3600 && clear_s >= 1 && clear_s <= 3600
        && initial_s == 30 && recovery_s == 30 && trend_ppm >= 50 && trend_ppm <= 1000;
}
bool Rules::operator==(const Rules& o) const {
    return watch_on == o.watch_on && watch_off == o.watch_off && high_on == o.high_on && high_off == o.high_off
        && watch_s == o.watch_s && high_s == o.high_s && high_clear_s == o.high_clear_s && clear_s == o.clear_s
        && initial_s == o.initial_s && recovery_s == o.recovery_s && trend_ppm == o.trend_ppm;
}
std::string RecordId::str() const {
    char out[44]{};
    for (size_t i = 0; i < boot.size(); ++i) std::snprintf(out + i * 2, 3, "%02x", boot[i]);
    std::snprintf(out + 32, 12, ":%u", static_cast<unsigned>(sequence));
    return out;
}
void AlarmEngine::Dwell::update(bool condition, uint64_t now, uint32_t dt) {
    if (!condition) { reset(); return; }
    if (active && dt) elapsed += dt;
    else { elapsed = 0; since = now; }
    active = true;
}
void AlarmEngine::reset_candidates() {
    initial_.reset(); recovery_.reset(); watch_.reset(); high_.reset(); leave_high_.reset(); clear_.reset();
    initial_low_ = true;
    candidate_peak_ = 0;
}
static double median(double* values, size_t n) {
    std::sort(values, values + n);
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}
Trend AlarmEngine::calculate_trend(uint64_t now) const {
    if (now < 59000) return Trend::Unknown;
    double first[64]{}, last[64]{};
    size_t a = 0, b = 0, count = 0;
    uint64_t prior = 0, earliest = now;
    for (size_t i = 0; i < trend_size_; ++i) {
        const auto& p = trend_[(trend_pos_ + 64 - trend_size_ + i) % 64];
        if (p.ms > now || now - p.ms >= 60000) continue;
        if (count && p.ms - prior > 5000) return Trend::Unknown;
        earliest = std::min(earliest, p.ms);
        prior = p.ms; ++count;
        if (now - p.ms >= 40000) first[a++] = p.ppm;
        if (now - p.ms < 20000) last[b++] = p.ppm;
    }
    if (count < 54 || a < 18 || b < 18 || now - earliest < 58000) return Trend::Unknown;
    double delta = median(last, b) - median(first, a);
    return delta >= rules_.trend_ppm ? Trend::Rising : delta <= -static_cast<double>(rules_.trend_ppm) ? Trend::Falling : Trend::Stable;
}
Step AlarmEngine::update(const Sample& sample) {
    Step out;
    uint64_t elapsed = sample.ms >= last_ms_ ? sample.ms - last_ms_ : UINT64_MAX;
    bool valid = sample.available && sample.quality == Quality::Valid && sample.ppm >= 380 && sample.ppm <= 32000;
    uint32_t dt = valid && previous_valid_ && elapsed <= 1500 ? static_cast<uint32_t>(elapsed) : 0;
    if (view_.episode.active && last_ms_ && elapsed < 86400000 && (!valid || !previous_valid_ || elapsed > 1500)) {
        view_.episode.missing_ms += elapsed;
    }
    if (!valid || !dt) {
        reset_candidates();
        if (view_.episode.active && (!valid || elapsed > 1500)) view_.episode.interrupted = true;
    }
    last_ms_ = sample.ms;
    previous_valid_ = valid;
    if (!valid) {
        median_size_ = median_pos_ = 0;
        view_.trend = Trend::Unknown;
        view_.quality = sample.quality;
        if (sample.quality == Quality::Fault) fault_latched_ = true;
        view_.recovering = fault_latched_;
        return out;
    }
    out.valid = true; out.valid_ms = dt;
    view_.raw = sample.ppm; view_.last_valid_ms = sample.ms;
    view_.range_warn = sample.ppm < 400 || sample.ppm > 5000;
    median_[median_pos_] = sample.ppm;
    median_pos_ = (median_pos_ + 1) % 5;
    median_size_ = std::min<size_t>(5, median_size_ + 1);
    double values[5];
    for (size_t i = 0; i < median_size_; ++i) values[i] = median_[i];
    double middle = median(values, median_size_);
    view_.filtered = dt ? view_.filtered + (1 - std::exp(-static_cast<double>(dt) / 10000.0)) * (middle - view_.filtered) : middle;
    trend_[trend_pos_] = {sample.ms, view_.filtered};
    trend_pos_ = (trend_pos_ + 1) % 64;
    trend_size_ = std::min<size_t>(64, trend_size_ + 1);
    view_.trend = calculate_trend(sample.ms);
    bool high_now = view_.filtered >= rules_.high_on;
    out.high_ms = high_now && previous_high_ ? dt : 0;
    previous_high_ = high_now;
    out.red_ms = view_.level == Level::High ? dt : 0;
    if (view_.episode.active) {
        auto& ep = view_.episode;
        ep.peak = std::max(ep.peak, sample.ppm);
        ep.valid_ms += dt; ep.high_ms += out.high_ms; ep.red_ms += out.red_ms;
    }
    if (fault_latched_) {
        recovery_.update(true, sample.ms, dt);
        view_.quality = Quality::Fault; view_.recovering = true;
        if (recovery_.elapsed < rules_.recovery_s * 1000ULL) return out;
        fault_latched_ = false; view_.recovering = false;
        reset_candidates();
    }
    view_.quality = Quality::Valid;
    const double c = view_.filtered;
    watch_.update(c >= rules_.watch_on, sample.ms, dt);
    high_.update(high_now, sample.ms, dt);
    candidate_peak_ = high_now ? std::max(high_.elapsed ? candidate_peak_ : 0, sample.ppm) : 0;
    leave_high_.update(c <= rules_.high_off, sample.ms, dt);
    clear_.update(c <= rules_.watch_off, sample.ms, dt);
    Level next = view_.level;
    if (view_.level != Level::High && high_.elapsed >= rules_.high_s * 1000ULL) next = Level::High;
    else if (view_.level == Level::Unassessed) {
        initial_.update(true, sample.ms, dt);
        initial_low_ = initial_low_ && c < rules_.watch_on;
        if (initial_.elapsed >= rules_.initial_s * 1000ULL) next = initial_low_ ? Level::Clear : Level::Watch;
    } else if (view_.level != Level::Clear && clear_.elapsed >= rules_.clear_s * 1000ULL) next = Level::Clear;
    else if (view_.level == Level::High && leave_high_.elapsed >= rules_.high_clear_s * 1000ULL) next = Level::Watch;
    else if (view_.level == Level::Clear && watch_.elapsed >= rules_.watch_s * 1000ULL) next = Level::Watch;
    if (next == Level::High && !view_.episode.active) {
        view_.episode = {};
        view_.episode.active = true;
        view_.episode.peak = candidate_peak_;
        view_.episode.candidate_ms = high_.since;
        view_.episode.confirmed_ms = sample.ms;
        view_.episode.high_ms = high_.elapsed;
        view_.episode.valid_ms = high_.elapsed;
        out.event = Event::EpisodeStart;
    }
    view_.level = next;
    if (next == Level::Clear && view_.episode.active) {
        view_.episode.active = false;
        view_.episode.ended_ms = sample.ms;
        out.event = Event::Recovered;
    } else if (view_.episode.active && view_.trend == Trend::Falling && !view_.episode.falling_ms && out.event == Event::None) {
        view_.episode.falling_ms = sample.ms;
        out.event = Event::Falling;
    }
    return out;
}
Event AlarmEngine::configure(const Rules& rules) {
    Event event = view_.episode.active ? Event::ConfigChanged : Event::None;
    view_.episode.active = false;
    rules_ = rules; view_.level = Level::Unassessed;
    reset_candidates(); previous_valid_ = false;
    median_size_ = trend_size_ = median_pos_ = trend_pos_ = 0;
    return event;
}
void AlarmEngine::restore(const Episode& ep, Level level) {
    view_.episode = ep;
    if (ep.active) { view_.episode.interrupted = true; view_.level = level; }
    reset_candidates(); previous_valid_ = false;
}
void MinuteAccumulator::begin(uint64_t start, uint64_t end, uint32_t revision, uint32_t index) {
    start_ = start; end_ = end; sum_ = 0; slots_ = 0; interval_end_ = 0; minute_ = {};
    minute_.window_ms = static_cast<uint32_t>(end - start);
    minute_.expected_count = static_cast<uint16_t>((end - start + 999) / 1000);
    minute_.config_revision = revision; minute_.window_index = index;
}
void MinuteAccumulator::add(const Sample& sample, const Step& step, const View& view) {
    // Attribute the observed interval to both sides of a minute boundary exactly once.
    if (step.valid && step.valid_ms && sample.ms > interval_end_) {
        uint64_t lo = std::max(start_, sample.ms >= step.valid_ms ? sample.ms - step.valid_ms : 0);
        uint64_t hi = std::min(end_, sample.ms);
        uint32_t overlap = hi > lo ? static_cast<uint32_t>(hi - lo) : 0;
        minute_.valid_ms += overlap;
        minute_.observed_high_ms += step.high_ms ? overlap : 0;
        minute_.red_valid_ms += step.red_ms ? overlap : 0;
        interval_end_ = sample.ms;
    }
    if (sample.ms < start_ || sample.ms >= end_) return;
    uint64_t slot = (sample.ms - start_) / 1000;
    if (slot >= 64 || (slots_ & (1ULL << slot))) return;
    slots_ |= 1ULL << slot;
    minute_.level = view.level; minute_.trend = view.trend;
    if (step.valid) {
        if (!minute_.valid_count) minute_.minimum = minute_.maximum = sample.ppm;
        minute_.minimum = std::min(minute_.minimum, sample.ppm);
        minute_.maximum = std::max(minute_.maximum, sample.ppm);
        ++minute_.valid_count; sum_ += sample.ppm;
    } else if (sample.quality == Quality::Warmup || sample.quality == Quality::Commissioning) ++minute_.warmup_count;
    else ++minute_.error_count;
}
void MinuteAccumulator::truncate(uint64_t stop) {
    end_ = std::max(start_ + 1, std::min(end_, stop));
    minute_.window_ms = static_cast<uint32_t>(end_ - start_);
    minute_.expected_count = static_cast<uint16_t>((end_ - start_ + 999) / 1000);
}
Minute MinuteAccumulator::finish() const {
    Minute out = minute_;
    if (out.valid_count) out.mean = static_cast<int32_t>(std::llround(static_cast<double>(sum_) / out.valid_count));
    out.missed_count = out.expected_count - out.valid_count - out.warmup_count - out.error_count;
    return out;
}
}
