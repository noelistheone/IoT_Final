#pragma once
#include "record.hpp"
#include "wifi.hpp"
#include <atomic>
#include <vector>
#include <cstring>
#include <string_view>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "cJSON.h"

namespace aircue {
uint64_t monotonic_ms();
uint64_t utc_ms();
std::string random_hex(size_t bytes);
struct Lock {
    SemaphoreHandle_t mutex;
    explicit Lock(SemaphoreHandle_t m): mutex(m) { xSemaphoreTake(m, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex); }
};
struct Json {
    cJSON* p;
    explicit Json(cJSON* value = nullptr): p(value) {}
    ~Json() { cJSON_Delete(p); }
    cJSON* release() {auto* value=p;p=nullptr;return value;}
    Json(const Json&) = delete;
    Json& operator=(const Json&) = delete;
};
std::string json_text(const cJSON* obj);
class JsonText {
    char* text_ = nullptr;
public:
    explicit JsonText(char* text = nullptr): text_(text) {}
    ~JsonText() { cJSON_free(text_); }
    JsonText(const JsonText&) = delete;
    JsonText& operator=(const JsonText&) = delete;
    void reset(char* text = nullptr) { cJSON_free(text_); text_ = text; }
    const char* c_str() const { return text_ ? text_ : ""; }
    bool empty() const { return !text_ || !*text_; }
    std::string_view view() const { return c_str(); }
};
JsonText json_take(cJSON* obj);
std::string string_field(const cJSON* obj, const char* key, const std::string& fallback = "");
double number_field(const cJSON* obj, const char* key, double fallback = 0);
bool bool_field(const cJSON* obj, const char* key, bool fallback = false);
bool equal_secret(const std::string& a, const std::string& b);
std::string digest(const std::string& value);

struct Config {
    Rules rules{};
    uint32_t revision = 1;
    uint32_t rules_revision = 1;
    bool uplink = false, commissioned = false, demo = false, buzzer_enabled = false;
    std::string ssid, wifi_password, sensor_serial;
    std::vector<WifiProfile> wifi_profiles;
    std::string run_id;
    uint64_t run_t0 = 0;
    uint64_t mute_until = 0;
    RecordId muted_episode{};
};
class ConfigManager {
public:
    bool begin();
    Config get();
    cJSON* public_json();
    cJSON* apply(const cJSON* request, const std::string& operation, bool remote);
    cJSON* command_results();
    bool acknowledge_result(const std::string& id);
    uint32_t boot_ordinal = 0;
private:
    SemaphoreHandle_t mutex_ = xSemaphoreCreateMutex();
    Config config_{};
    std::string state_;
    uint32_t generation_ = 0;
    bool save(cJSON* state);
    void unpack(const cJSON* state);
};
extern ConfigManager config;

struct StoredRecord {
    Record record;
    uint64_t order = 0, run_t0 = 0;
    std::string run_id;
};
struct StorageWork {
    Record record;
    Episode episode;
    Level level;
    uint32_t revision = 0;
    uint32_t config_revision = 0;
    Rules rules{};
    bool demo = false, uplink = false;
    char run_id[41]{};
    uint64_t run_t0 = 0;
};
class StorageService {
public:
    bool begin(const std::array<uint8_t,16>& boot, uint32_t ordinal);
    RecordId enqueue(Record record, const Config& cfg, const Episode& ep = {}, Level level = Level::Unassessed);
    void run();
    std::vector<StoredRecord> page(uint64_t cursor, size_t limit, uint8_t type = 0);
    bool acknowledge(uint64_t order);
    bool flush(uint32_t timeout_ms);
    bool restore(Episode& ep, Level& level, uint32_t& revision);
    cJSON* gaps();
    bool acknowledge_gaps(const cJSON* ids);
    cJSON* diagnostics();
    cJSON* config_snapshot(uint32_t revision);
    std::atomic<bool> healthy{false};
    std::atomic<uint32_t> dropped_queue{0}, write_errors{0};
    std::atomic<uint64_t> ack{0}, remote_minute_ack{0}, remote_event_ack{0};
private:
    struct Segment {
        std::string path, run_id;
        uint64_t run_t0 = 0, first = 0, last = 0;
        uint64_t last_utc = 0;
        bool unknown_utc = false;
        uint32_t generation = 0, ordinal = 0, count = 0;
        uint8_t type = 1;
        uint16_t record_bytes = record_size;
    };
    SemaphoreHandle_t mutex_ = xSemaphoreCreateMutex();
    SemaphoreHandle_t queue_mutex_ = xSemaphoreCreateMutex();
    QueueHandle_t queue_ = xQueueCreate(32, sizeof(StorageWork));
    std::vector<Segment> segments_;
    std::array<uint8_t,16> boot_{};
    uint32_t ordinal_ = 0, sequence_ = 0, generation_ = 0;
    uint32_t checkpoint_generation_ = 0;
    uint32_t last_written_sequence_ = 0;
    uint64_t last_ack_save_ = 0, last_committed_ = 0;
    std::atomic<uint32_t> pending_{0};
    bool append(const StorageWork& work);
    bool checkpoint(const StorageWork& work);
    bool save_config_snapshot(const StorageWork& work);
    bool record_gap(const Segment& segment, const char* reason);
    bool reclaim(uint8_t type);
};
extern StorageService storage;
struct Runtime {
    View view{};
    uint64_t sensor_started = 0, commission_elapsed = 0;
    std::string sensor_serial;
    bool self_test_ok = false, sta_connected = false, tls_ok = false;
    std::string network = "MANUAL_OFFLINE", cloud = "OFFLINE", ota = "IDLE";
    std::string mqtt = "STARTING";
    uint64_t mqtt_last_ack = 0;
    uint64_t last_ack_utc = 0;
    uint32_t sample_count = 0, invalid_count = 0, bypass_count = 0, wifi_retries = 0, sync_retries = 0;
    uint32_t time_jumps = 0;
    int wifi_disconnect_reason = 0;
    std::string sta_ip,ap_ssid,connected_ssid,wifi_target_ssid;
    uint16_t sensor_status = 0, self_test = 0xffff;
    int16_t diagnostic_raw_ppm = 0, sensor_error = 0;
    uint64_t diagnostic_sample_ms = 0;
    bool diagnostic_sample_ok = false, buzzer_requested = false;
    unsigned buzzer_duty = 0, buzzer_tests = 0;
    float temperature = 0, humidity = 0;
    int reset_reason = 0;
    bool boot_validated = false;
    int boot_validation_error = 0;
};
extern SemaphoreHandle_t runtime_mutex;
extern SemaphoreHandle_t uplink_memory;
struct UplinkMemory {
    bool held = false;
    explicit UplinkMemory(bool wait = true);
    explicit operator bool() const {return held;}
    ~UplinkMemory() {if(held) xSemaphoreGive(uplink_memory);}
};
extern Runtime runtime;
extern std::array<uint8_t,16> boot_id;
extern std::atomic<bool> ota_requested;
extern std::atomic<bool> mqtt_pause_requested,mqtt_paused;
extern std::atomic<uint64_t> led_test_until;
extern std::atomic<unsigned> buzzer_test_request;
extern std::atomic<bool> sensor_initializing;
Runtime snapshot();
Climate climate_sample(const Runtime& state);
cJSON* climate_json(const Climate& climate);
cJSON* status_json(bool full = true);
cJSON* stored_json(const StoredRecord& item, bool wire = false);
bool network_start();
void sensor_task(void*);
void alarm_task(void*);
void output_task(void*);
void network_task(void*);
void sync_task(void*);
void mqtt_task(void*);
void inspect_i2c_hardware();
void hardware_task(void*);
cJSON* hardware_json(bool full = true);
bool local_peer_allowed(int socket);
}
