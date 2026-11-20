#include "aircue/runtime.hpp"
#include "aircue/ble.hpp"
#include "aircue/export.hpp"
#include "secrets.hpp"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace aircue {
static const char* events[]={"NONE","BOOT","EPISODE_START","FALLING","RECOVERED","CONFIG_CHANGED","SENSOR_FAULT","SENSOR_RESTORED","TIME_ANCHOR","GAP","MAINTENANCE","CONFIG"};
static void add_ppm(cJSON* object,const char* field,double ppm) {
    if(!std::isfinite(ppm)) { cJSON_AddNullToObject(object,field); return; }
    // cJSON drops the decimal point on whole numbers. Emit a numeric literal.
    char number[32];
    const int length=std::snprintf(number,sizeof(number),"%.2f",ppm);
    if(length>0 && size_t(length)<sizeof(number)) cJSON_AddRawToObject(object,field,number);
    else cJSON_AddNullToObject(object,field);
}
cJSON* climate_json(const Climate& climate) {
    static const char* fields[]={"co2_raw_ppm","temperature_c","humidity_rh","pressure_hpa","barometer_temperature_c"};
    auto* out=cJSON_CreateObject();
    for(unsigned i=0;i<ClimateFields;++i) {
        if(!(climate.mask&(1U<<i))) cJSON_AddNullToObject(out,fields[i]);
        else if(i==RawCo2) add_ppm(out,fields[i],climate.values[i]);
        else cJSON_AddNumberToObject(out,fields[i],climate.values[i]);
    }
    cJSON_AddNumberToObject(out,"co2_raw_count",climate.co2_samples);
    cJSON_AddNumberToObject(out,"barometer_count",climate.barometer_samples);return out;
}
cJSON* status_json(bool full) {
    auto state=snapshot(); auto cfg=config.get(); uint64_t now=monotonic_ms(),utc=utc_ms();
    auto* out=cJSON_CreateObject(); const auto& view=state.view;
    cJSON_AddStringToObject(out,"device_id",AIRCUE_DEVICE_ID); cJSON_AddStringToObject(out,"boot_id",hex(boot_id.data(),16).c_str());
    cJSON_AddNumberToObject(out,"boot_ordinal",config.boot_ordinal); cJSON_AddNumberToObject(out,"uptime_ms",now);
    cJSON_AddStringToObject(out,"source","local"); cJSON_AddStringToObject(out,"firmware",esp_app_get_description()->version);
    cJSON_AddStringToObject(out,"quality",name(view.quality)); cJSON_AddStringToObject(out,"level",name(view.level)); cJSON_AddStringToObject(out,"trend",name(view.trend));
    cJSON_AddItemToObject(out,"climate",climate_json(climate_sample(state)));
    if(view.last_valid_ms) {
        add_ppm(out,"ppm",view.raw); add_ppm(out,"filtered_ppm",view.filtered);
        cJSON_AddNumberToObject(out,"sample_age_ms",now-view.last_valid_ms);
        if(utc) cJSON_AddNumberToObject(out,"measured_at",utc-(now-view.last_valid_ms)); else cJSON_AddNullToObject(out,"measured_at");
    } else { cJSON_AddNullToObject(out,"ppm"); cJSON_AddNullToObject(out,"sample_age_ms"); cJSON_AddNullToObject(out,"measured_at"); }
    cJSON_AddStringToObject(out,"time_quality",utc?"SYNCED":"UTC_UNKNOWN"); cJSON_AddNumberToObject(out,"received_at",utc);
    cJSON_AddBoolToObject(out,"stale",!view.last_valid_ms||now-view.last_valid_ms>5000);
    cJSON_AddBoolToObject(out,"range_warn",view.range_warn); cJSON_AddBoolToObject(out,"recovering",view.recovering);
    auto* ep=cJSON_AddObjectToObject(out,"episode"); cJSON_AddBoolToObject(ep,"active",view.episode.active);
    cJSON_AddStringToObject(ep,"episode_id",view.episode.id.empty()?"":view.episode.id.str().c_str());
    add_ppm(ep,"peak",view.episode.peak); cJSON_AddNumberToObject(ep,"high_ms",view.episode.high_ms);
    cJSON_AddNumberToObject(ep,"red_ms",view.episode.red_ms); cJSON_AddNumberToObject(ep,"valid_ms",view.episode.valid_ms);
    cJSON_AddNumberToObject(ep,"missing_ms",view.episode.missing_ms); cJSON_AddBoolToObject(ep,"interrupted",view.episode.interrupted);
    cJSON_AddNumberToObject(ep,"candidate_ms",view.episode.candidate_ms); cJSON_AddNumberToObject(ep,"confirmed_ms",view.episode.confirmed_ms);
    cJSON_AddNumberToObject(ep,"falling_ms",view.episode.falling_ms); cJSON_AddNumberToObject(ep,"ended_ms",view.episode.ended_ms);
    cJSON_AddNumberToObject(out,"mute_remaining_s",cfg.mute_until>now?(cfg.mute_until-now)/1000:0);
    cJSON_AddItemToObject(out,"config",config.public_json()); cJSON_AddItemToObject(out,"storage",storage.diagnostics());
    cJSON_AddStringToObject(out,"network",state.network.c_str()); cJSON_AddStringToObject(out,"cloud",state.cloud.c_str());
    cJSON_AddBoolToObject(out,"sta_connected",state.sta_connected); cJSON_AddBoolToObject(out,"tls_ok",state.tls_ok);
    cJSON_AddStringToObject(out,"sta_ip",state.sta_connected?state.sta_ip.c_str():"");
    cJSON_AddStringToObject(out,"connected_ssid",state.connected_ssid.c_str());
    cJSON_AddStringToObject(out,"wifi_target_ssid",state.wifi_target_ssid.c_str());
    cJSON_AddStringToObject(out,"ap_ssid",state.ap_ssid.c_str());
    cJSON_AddNumberToObject(out,"last_ack_utc",state.last_ack_utc); cJSON_AddStringToObject(out,"ota",state.ota.c_str());
    auto* diagnostics=cJSON_AddObjectToObject(out,"diagnostics");
    cJSON_AddNumberToObject(diagnostics,"sample_count",state.sample_count); cJSON_AddNumberToObject(diagnostics,"invalid_count",state.invalid_count);
    cJSON_AddNumberToObject(diagnostics,"bypass_count",state.bypass_count); cJSON_AddNumberToObject(diagnostics,"wifi_retries",state.wifi_retries);
    cJSON_AddNumberToObject(diagnostics,"sync_retries",state.sync_retries); cJSON_AddNumberToObject(diagnostics,"reset_reason",state.reset_reason);
    cJSON_AddNumberToObject(diagnostics,"free_heap",esp_get_free_heap_size()); cJSON_AddNumberToObject(diagnostics,"commission_elapsed_ms",state.commission_elapsed);
    cJSON_AddNumberToObject(diagnostics,"minimum_free_heap",esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(diagnostics,"largest_free_block",heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if(full) {
        auto* stacks=cJSON_AddObjectToObject(diagnostics,"task_stack_minimum");
        for(const char* task:{"sensor","outputs","alarm","hardware","journal","network","sync","remote_mqtt","ble_requests"}) {
            auto handle=xTaskGetHandle(task);
            if(handle) cJSON_AddNumberToObject(stacks,task,uxTaskGetStackHighWaterMark(handle));
        }
    }
    cJSON_AddStringToObject(diagnostics,"sensor_serial",state.sensor_serial.c_str()); cJSON_AddNumberToObject(diagnostics,"self_test",state.self_test);
    cJSON_AddNumberToObject(diagnostics,"sensor_status",state.sensor_status); cJSON_AddNumberToObject(diagnostics,"time_jumps",state.time_jumps);
    cJSON_AddNumberToObject(diagnostics,"temperature_c",state.temperature); cJSON_AddNumberToObject(diagnostics,"humidity_rh",state.humidity);
    add_ppm(diagnostics,"raw_ppm",state.diagnostic_raw_ppm);
    cJSON_AddNumberToObject(diagnostics,"raw_sample_age_ms",state.diagnostic_sample_ms?now-state.diagnostic_sample_ms:0);
    cJSON_AddBoolToObject(diagnostics,"raw_sample_ok",state.diagnostic_sample_ok);
    cJSON_AddNumberToObject(diagnostics,"sensor_error",state.sensor_error);
    cJSON_AddNumberToObject(diagnostics,"wifi_disconnect_reason",state.wifi_disconnect_reason);
    cJSON_AddBoolToObject(diagnostics,"buzzer_sound_enabled",cfg.buzzer_enabled);
    cJSON_AddItemToObject(out,"bluetooth",bluetooth_json());
    cJSON_AddBoolToObject(out,"ap_enabled",false);
    cJSON_AddStringToObject(out,"remote_mqtt",state.mqtt.c_str());
    cJSON_AddNumberToObject(out,"remote_last_ack_utc",state.mqtt_last_ack);
    cJSON_AddBoolToObject(diagnostics,"boot_validated",state.boot_validated);
    cJSON_AddNumberToObject(diagnostics,"boot_validation_error",state.boot_validation_error);
    cJSON_AddItemToObject(diagnostics,"hardware",hardware_json(full));
    return out;
}
cJSON* stored_json(const StoredRecord& item,bool wire) {
    const auto& r=item.record; auto* out=cJSON_CreateObject();
    cJSON_AddStringToObject(out,"run_id",item.run_id.c_str()); cJSON_AddNumberToObject(out,"run_t0",item.run_t0);
    cJSON_AddStringToObject(out,"order",std::to_string(item.order).c_str());
    if(wire) { auto bytes=encode(r); cJSON_AddStringToObject(out,"wire",hex(bytes.data(),bytes.size()).c_str()); return out; }
    cJSON_AddStringToObject(out,"record_id",r.id.str().c_str()); cJSON_AddStringToObject(out,"boot_id",hex(r.id.boot.data(),16).c_str());
    cJSON_AddNumberToObject(out,"sequence",r.id.sequence); cJSON_AddNumberToObject(out,"monotonic_ms",r.monotonic_ms);
    if(r.utc_ms) cJSON_AddNumberToObject(out,"measured_at",r.utc_ms); else cJSON_AddNullToObject(out,"measured_at");
    cJSON_AddNullToObject(out,"received_at"); cJSON_AddStringToObject(out,"time_quality",r.flags&UtcUnknown?"UTC_UNKNOWN":"SYNCED");
    cJSON_AddBoolToObject(out,"demo",r.flags&Demo); cJSON_AddBoolToObject(out,"partial",r.flags&Partial);
    cJSON_AddBoolToObject(out,"interrupted",r.flags&Interrupted); cJSON_AddBoolToObject(out,"range_warn",r.flags&RangeWarn);
    if(r.type==1) {
        Json climate(climate_json(read_climate(r)));
        while(climate.p->child) {auto* item=cJSON_DetachItemViaPointer(climate.p,climate.p->child);cJSON_AddItemToObject(out,item->string,item);}
        auto m=read_minute(r); cJSON_AddStringToObject(out,"record_type","MINUTE");
        if(m.valid_count) { add_ppm(out,"mean",m.mean); add_ppm(out,"minimum",m.minimum); add_ppm(out,"maximum",m.maximum); }
        else { cJSON_AddNullToObject(out,"mean"); cJSON_AddNullToObject(out,"minimum"); cJSON_AddNullToObject(out,"maximum"); }
#define FIELD(k) cJSON_AddNumberToObject(out,#k,m.k)
        FIELD(valid_count); FIELD(expected_count); FIELD(config_revision); FIELD(valid_ms); FIELD(observed_high_ms); FIELD(red_valid_ms); FIELD(window_ms); FIELD(warmup_count); FIELD(error_count); FIELD(missed_count);
#undef FIELD
        if(m.window_index==UINT32_MAX) cJSON_AddNullToObject(out,"window_index"); else cJSON_AddNumberToObject(out,"window_index",m.window_index);
        cJSON_AddStringToObject(out,"level",name(m.level)); cJSON_AddStringToObject(out,"trend",name(m.trend));
        cJSON_AddItemToObject(out,"config_snapshot",storage.config_snapshot(m.config_revision));
    } else {
        auto e=read_episode(r); cJSON_AddStringToObject(out,"record_type","EVENT");
        cJSON_AddStringToObject(out,"event",events[std::min<unsigned>(11,r.payload[0])]);
        cJSON_AddStringToObject(out,"episode_id",e.id.empty()?"":e.id.str().c_str()); add_ppm(out,"peak",e.peak);
        cJSON_AddNumberToObject(out,"high_ms",e.high_ms); cJSON_AddNumberToObject(out,"red_ms",e.red_ms); cJSON_AddNumberToObject(out,"valid_ms",e.valid_ms);
        cJSON_AddNumberToObject(out,"known_missing_ms",e.missing_ms);
        cJSON_AddNumberToObject(out,"config_revision",get32(r.payload.data()+40)); cJSON_AddStringToObject(out,"level",name(static_cast<Level>(r.payload[1])));
    }
    return out;
}
}
