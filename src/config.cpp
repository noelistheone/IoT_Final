#include "aircue/runtime.hpp"
#include "nvs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#if __has_include("wifi-bootstrap.hpp")
#include "wifi-bootstrap.hpp"
#endif
#if __has_include("recovery-bootstrap.hpp")
#include "recovery-bootstrap.hpp"
#endif

namespace aircue {
ConfigManager config;
uint64_t monotonic_ms() { return esp_timer_get_time() / 1000; }
uint64_t utc_ms() { struct timeval tv; gettimeofday(&tv, nullptr); return tv.tv_sec > 1735689600 ? uint64_t(tv.tv_sec)*1000 + tv.tv_usec/1000 : 0; }
std::string random_hex(size_t bytes) { std::vector<uint8_t> data(bytes); esp_fill_random(data.data(), bytes); return hex(data.data(), bytes); }
std::string json_text(const cJSON* obj) { char* text = cJSON_PrintUnformatted(obj); std::string out = text ? text : "{}"; cJSON_free(text); return out; }
JsonText json_take(cJSON* obj) {
    // Transfer the printed buffer directly; do not allocate a second large string.
    char* text=cJSON_PrintUnformatted(obj);cJSON_Delete(obj);
    return JsonText(text);
}
std::string string_field(const cJSON* obj, const char* key, const std::string& fallback) { auto* x = cJSON_GetObjectItemCaseSensitive(obj, key); return cJSON_IsString(x) ? x->valuestring : fallback; }
double number_field(const cJSON* obj, const char* key, double fallback) { auto* x = cJSON_GetObjectItemCaseSensitive(obj, key); return cJSON_IsNumber(x) ? x->valuedouble : fallback; }
bool bool_field(const cJSON* obj, const char* key, bool fallback) { auto* x = cJSON_GetObjectItemCaseSensitive(obj, key); return cJSON_IsBool(x) ? cJSON_IsTrue(x) : fallback; }
bool equal_secret(const std::string& a, const std::string& b) { size_t diff = a.size() ^ b.size(); for(size_t i=0;i<std::max(a.size(),b.size());++i) diff |= (i<a.size()?a[i]:0) ^ (i<b.size()?b[i]:0); return diff == 0; }
std::string digest(const std::string& value) { uint8_t out[32]; mbedtls_sha256(reinterpret_cast<const uint8_t*>(value.data()), value.size(), out, 0); return hex(out, sizeof(out)); }
static cJSON* result(const char* status, int code, uint32_t revision, const char* message) {
    auto* out = cJSON_CreateObject(); cJSON_AddStringToObject(out,"status",status); cJSON_AddNumberToObject(out,"http_status",code);
    cJSON_AddNumberToObject(out,"applied_revision",revision); cJSON_AddStringToObject(out,"message",message); return out;
}
static void assign(cJSON* obj, const char* key, cJSON* value) { if(cJSON_HasObjectItem(obj,key)) cJSON_ReplaceItemInObjectCaseSensitive(obj,key,value); else cJSON_AddItemToObject(obj,key,value); }
static cJSON* cached_response(const cJSON* entry) {
    auto* legacy=cJSON_GetObjectItemCaseSensitive(entry,"result");
    if(legacy) return cJSON_Duplicate(legacy,true);
    auto* out=result(bool_field(entry,"ota")?"accepted":"applied",200,number_field(entry,"revision"),bool_field(entry,"intent")?"Intent saved; observe actual state in status":"Configuration committed");
    cJSON_AddStringToObject(out,"request_id",string_field(entry,"id").c_str());return out;
}
static std::string canonical(const cJSON* value) {
    if(cJSON_IsObject(value)) {
        std::vector<std::string> keys; cJSON* child;
        cJSON_ArrayForEach(child,value) keys.emplace_back(child->string);
        std::sort(keys.begin(),keys.end()); std::string out="{";
        for(const auto& key:keys) {
            if(out.size()>1) out+=",";
            Json name(cJSON_CreateString(key.c_str())); out+=json_text(name.p)+":"+canonical(cJSON_GetObjectItemCaseSensitive(value,key.c_str()));
        }
        return out+"}";
    }
    if(cJSON_IsArray(value)) {
        std::string out="["; cJSON* child;
        cJSON_ArrayForEach(child,value) { if(out.size()>1) out+=","; out+=canonical(child); }
        return out+"]";
    }
    return json_text(value);
}
static cJSON* rules_json(const Rules& r) {
    auto* o=cJSON_CreateObject();
#define RULE(k) cJSON_AddNumberToObject(o,#k,r.k)
    RULE(watch_on); RULE(watch_off); RULE(high_on); RULE(high_off); RULE(watch_s); RULE(high_s); RULE(high_clear_s); RULE(clear_s); RULE(trend_ppm);
#undef RULE
    return o;
}
static bool read_wifi_profiles(const cJSON* list,std::vector<WifiProfile>& profiles) {
    if(!cJSON_IsArray(list) || cJSON_GetArraySize(list)>int(max_wifi_profiles)) return false;
    std::vector<WifiProfile> result;
    cJSON* item;
    cJSON_ArrayForEach(item,list) {
        if(!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item,"ssid")) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item,"wifi_password"))) return false;
        WifiProfile profile{string_field(item,"ssid"),string_field(item,"wifi_password")};
        if(!remember_wifi(result,profile)) return false;
    }
    if(result.size()!=size_t(cJSON_GetArraySize(list))) return false;
    profiles=std::move(result);return true;
}
static void save_wifi_profiles(cJSON* state,const std::vector<WifiProfile>& profiles) {
    auto* list=cJSON_CreateArray();
    for(const auto& p:profiles) {
        auto* item=cJSON_CreateObject();cJSON_AddStringToObject(item,"ssid",p.ssid.c_str());cJSON_AddStringToObject(item,"wifi_password",p.password.c_str());cJSON_AddItemToArray(list,item);
    }
    assign(state,"wifi_profiles",list);
    assign(state,"ssid",cJSON_CreateString(profiles.empty()?"":profiles.front().ssid.c_str()));
    assign(state,"wifi_password",cJSON_CreateString(profiles.empty()?"":profiles.front().password.c_str()));
}
void ConfigManager::unpack(const cJSON* s) {
    config_.revision=number_field(s,"revision",1); config_.uplink=bool_field(s,"uplink_enabled");
    config_.rules_revision=number_field(s,"rules_revision",1);
    config_.commissioned=bool_field(s,"commissioned"); config_.demo=bool_field(s,"demo");
    config_.buzzer_enabled=bool_field(s,"buzzer_enabled");
    config_.ssid=string_field(s,"ssid"); config_.wifi_password=string_field(s,"wifi_password"); config_.sensor_serial=string_field(s,"sensor_serial");
    config_.wifi_profiles.clear();
    auto* profiles=cJSON_GetObjectItemCaseSensitive(s,"wifi_profiles");
    if(profiles) read_wifi_profiles(profiles,config_.wifi_profiles);
    else if(!config_.ssid.empty()) remember_wifi(config_.wifi_profiles,{config_.ssid,config_.wifi_password});
    config_.run_id=string_field(s,"run_id"); config_.run_t0=number_field(s,"run_t0");
    auto* r=cJSON_GetObjectItemCaseSensitive(s,"rules");
#define READ(k) config_.rules.k=number_field(r,#k,config_.rules.k)
    READ(watch_on); READ(watch_off); READ(high_on); READ(high_off); READ(watch_s); READ(high_s); READ(high_clear_s); READ(clear_s); READ(trend_ppm);
#undef READ
}
bool ConfigManager::save(cJSON* s) {
    auto text=json_text(s); std::vector<uint8_t> blob(text.size()+8);
    put32(blob.data(),generation_+1); memcpy(blob.data()+4,text.data(),text.size()); put32(blob.data()+blob.size()-4,crc32(blob.data(),blob.size()-4));
    nvs_handle_t nvs;
    if(nvs_open("aircue",NVS_READWRITE,&nvs)!=ESP_OK) return false;
    auto err=nvs_set_blob(nvs,(generation_+1)%2?"config_a":"config_b",blob.data(),blob.size());
    if(err==ESP_OK) err=nvs_commit(nvs);
    nvs_close(nvs);
    if(err!=ESP_OK) return false;
    ++generation_; state_=text; unpack(s); return true;
}
bool ConfigManager::begin() {
    Lock lock(mutex_); nvs_handle_t nvs;
    if(nvs_open("aircue",NVS_READWRITE,&nvs)!=ESP_OK) return false;
    bool found_blob=false;
    for(auto key:{"config_a","config_b"}) {
        size_t len=0;
        if(nvs_get_blob(nvs,key,nullptr,&len)!=ESP_OK) continue;
        found_blob=true;
        if(len<8 || len>40000) continue;
        std::vector<uint8_t> blob(len);
        if(nvs_get_blob(nvs,key,blob.data(),&len)!=ESP_OK || crc32(blob.data(),len-4)!=get32(blob.data()+len-4)) continue;
        uint32_t gen=get32(blob.data());
        Json obj(cJSON_ParseWithLength(reinterpret_cast<char*>(blob.data()+4),len-8));
        if(obj.p && gen>generation_) { generation_=gen; state_=json_text(obj.p); unpack(obj.p); }
    }
    if(found_blob && !generation_) { nvs_close(nvs); return false; }
    uint32_t boot=0; nvs_get_u32(nvs,"boot_ordinal",&boot);
#ifdef AIRCUE_RECOVERY_BOOT_FLOOR
    // A deliberate NVS migration must not reuse server-visible ordering counters.
    static_assert(AIRCUE_RECOVERY_BOOT_FLOOR>0,"Recovery boot floor must be positive");
    boot=std::max<uint32_t>(boot,AIRCUE_RECOVERY_BOOT_FLOOR-1);
#endif
    auto err=boot==UINT32_MAX?ESP_FAIL:nvs_set_u32(nvs,"boot_ordinal",boot+1);
    if(err==ESP_OK) err=nvs_commit(nvs);
    nvs_close(nvs);
    if(err!=ESP_OK) return false;
    boot_ordinal=boot+1;
    if(state_.empty()) {
        Json s(cJSON_CreateObject()); cJSON_AddNumberToObject(s.p,"revision",1); cJSON_AddBoolToObject(s.p,"uplink_enabled",false);
        cJSON_AddItemToObject(s.p,"rules",rules_json(config_.rules)); cJSON_AddArrayToObject(s.p,"requests");
        if(!save(s.p)) return false;
    }
    {
        // Compact old receipt objects before enabling the radio stacks. The
        // durable request identity, content hash, revision and result are retained.
        Json compact(cJSON_Parse(state_.c_str()));bool changed=false;cJSON* entry;
        auto* requests=cJSON_GetObjectItemCaseSensitive(compact.p,"requests");
        cJSON_ArrayForEach(entry,requests) {
            auto* legacy=cJSON_GetObjectItemCaseSensitive(entry,"result");
            if(!cJSON_IsObject(legacy)) continue;
            auto status=string_field(legacy,"status");
            if(status!="applied" && status!="accepted") continue;
            assign(entry,"revision",cJSON_CreateNumber(number_field(legacy,"applied_revision")));
            assign(entry,"ota",cJSON_CreateBool(status=="accepted"));
            assign(entry,"intent",cJSON_CreateBool(string_field(legacy,"message").find("Intent saved")==0));
            cJSON_DeleteItemFromObjectCaseSensitive(entry,"result");changed=true;
        }
        if(changed && !save(compact.p)) return false;
    }
#ifdef AIRCUE_RECOVERY_CONFIG_FLOOR
    if(config_.revision<AIRCUE_RECOVERY_CONFIG_FLOOR) {
        if(!config_.run_id.empty()) return false;
        Json recovered(cJSON_Parse(state_.c_str()));
        assign(recovered.p,"revision",cJSON_CreateNumber(AIRCUE_RECOVERY_CONFIG_FLOOR));
        assign(recovered.p,"rules_revision",cJSON_CreateNumber(AIRCUE_RECOVERY_CONFIG_FLOOR));
        if(!save(recovered.p)) return false;
        printf("AIRCUE_RECOVERY monotonic_counters_restored\n");
    }
#endif
#ifdef AIRCUE_BOOTSTRAP_WIFI_ID
    // Apply an owner-supplied profile once; later changes in the local UI persist.
    Json profile(cJSON_Parse(state_.c_str()));
    if(string_field(profile.p,"wifi_bootstrap_id")!=AIRCUE_BOOTSTRAP_WIFI_ID) {
#ifdef AIRCUE_BOOTSTRAP_WIFI_PROFILES_JSON
        Json imported(cJSON_Parse(AIRCUE_BOOTSTRAP_WIFI_PROFILES_JSON));
        std::vector<WifiProfile> networks;
        if(!read_wifi_profiles(imported.p,networks) || networks.empty()) return false;
        for(const auto& saved:config_.wifi_profiles) {
            auto existing=std::find_if(networks.begin(),networks.end(),[&](const WifiProfile& p){return p.ssid==saved.ssid;});
            if(existing==networks.end() && !remember_wifi(networks,saved)) return false;
        }
#else
        const std::string ssid=AIRCUE_BOOTSTRAP_WIFI_SSID,password=AIRCUE_BOOTSTRAP_WIFI_PASSWORD;
        std::vector<WifiProfile> networks=config_.wifi_profiles;
        if(!remember_wifi(networks,{ssid,password},true)) return false;
#endif
        if(config_.revision==UINT32_MAX) return false;
        save_wifi_profiles(profile.p,networks);
        assign(profile.p,"uplink_enabled",cJSON_CreateBool(true));
        assign(profile.p,"wifi_bootstrap_id",cJSON_CreateString(AIRCUE_BOOTSTRAP_WIFI_ID));
        assign(profile.p,"revision",cJSON_CreateNumber(config_.revision+1));
        if(!save(profile.p)) return false;
        printf("AIRCUE_WIFI private_hotspot_profile_saved\n");
    }
#endif
    config_.mute_until=0; config_.muted_episode={};
    return config_.rules.valid() && (config_.ssid.empty() || !config_.wifi_profiles.empty());
}
Config ConfigManager::get() { Lock lock(mutex_); return config_; }
cJSON* ConfigManager::public_json() {
    auto c=get(); auto* out=rules_json(c.rules); cJSON_AddNumberToObject(out,"revision",c.revision);
    cJSON_AddNumberToObject(out,"rules_revision",c.rules_revision);
    cJSON_AddBoolToObject(out,"uplink_enabled",c.uplink); cJSON_AddStringToObject(out,"ssid",c.ssid.c_str());
    cJSON_AddBoolToObject(out,"buzzer_enabled",c.buzzer_enabled);
    cJSON_AddNumberToObject(out,"wifi_limit",max_wifi_profiles);
    auto* networks=cJSON_AddArrayToObject(out,"wifi_networks");
    for(const auto& p:c.wifi_profiles) cJSON_AddItemToArray(networks,cJSON_CreateString(p.ssid.c_str()));
    cJSON_AddBoolToObject(out,"commissioned",c.commissioned); cJSON_AddBoolToObject(out,"demo",c.demo);
    cJSON_AddStringToObject(out,"run_id",c.run_id.c_str()); cJSON_AddNumberToObject(out,"run_t0",c.run_t0); return out;
}
cJSON* ConfigManager::apply(const cJSON* req, const std::string& op, bool remote) {
    Lock lock(mutex_);
    auto bad=[&](int code,const char* message) { return result(code==409?"conflict":"rejected",code,config_.revision,message); };
    const std::string id=string_field(req,"request_id");
    if(!cJSON_IsObject(req) || id.empty() || id.size()>64 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos) return bad(422,"Invalid request_id");
    if(esp_get_free_heap_size()<20000+state_.size()*2) return bad(503,"Memory is busy; retry after synchronization");
    const std::string hash=digest((remote?"remote:":"local:")+op+":"+canonical(req));
    Json state(cJSON_Parse(state_.c_str())); auto* requests=cJSON_GetObjectItemCaseSensitive(state.p,"requests");
    cJSON* entry;
    std::vector<std::string> keys;
    cJSON_ArrayForEach(entry,req) {
        std::string key=entry->string;
        if(std::find(keys.begin(),keys.end(),key)!=keys.end()) return bad(422,"Duplicate JSON field");
        keys.push_back(key);
        bool allowed=key=="request_id" || key=="base_revision" || (remote&&(key=="operation"||key=="expires_at"))
            || (op=="config"&&key=="patch") || (op=="uplink"&&key=="enabled")
            || (op=="wifi"&&(key=="action"||key=="ssid"||key=="wifi_password"))
            || (op=="mute"&&(key=="episode_id"||key=="duration_s")) || (op=="commission"&&key=="fresh_air_confirmed");
        if(!allowed) return bad(403,"Field is not allowed for this operation");
    }
    cJSON_ArrayForEach(entry,requests) if(string_field(entry,"id")==id) {
        if(string_field(entry,"hash")!=hash) return bad(409,"request_id was used with different content");
        return cached_response(entry);
    }
    double base=number_field(req,"base_revision",-1);
    if(base!=config_.revision) return bad(409,"Configuration revision changed");
    if(config_.revision==UINT32_MAX) return bad(503,"Revision space exhausted");
    if(remote) {
        uint64_t now=utc_ms(); double expiry=number_field(req,"expires_at",0);
        if(!now) return bad(409,"Device time is unavailable");
        if(expiry<=now) return result("expired",409,config_.revision,"Command expired");
        if(expiry>now+3600000) return bad(422,"Command expiry exceeds one hour");
    }
    auto snapshot_value=snapshot();
    uint64_t new_mute=config_.mute_until; RecordId muted=config_.muted_episode;
    if(op=="config") {
        auto* patch=cJSON_GetObjectItemCaseSensitive(req,"patch");
        if(!cJSON_IsObject(patch) || !patch->child) return bad(422,"An object patch is required");
        Rules rules=config_.rules; auto* saved_rules=cJSON_GetObjectItemCaseSensitive(state.p,"rules");
        bool rule_changed=false;
        std::vector<std::string> patch_keys;
        cJSON_ArrayForEach(entry,patch) {
            std::string key=entry->string; bool recognized=false;
            if(std::find(patch_keys.begin(),patch_keys.end(),key)!=patch_keys.end()) return bad(422,"Duplicate patch field");
            patch_keys.push_back(key);
#define PATCH(k) if(key==#k) { if(!cJSON_IsNumber(entry)||!std::isfinite(entry->valuedouble)||entry->valuedouble<0||entry->valuedouble>32000||std::floor(entry->valuedouble)!=entry->valuedouble) return bad(422,"Rules require bounded integers"); rules.k=entry->valuedouble; recognized=true; rule_changed=true; assign(saved_rules,#k,cJSON_CreateNumber(rules.k)); }
            PATCH(watch_on) PATCH(watch_off) PATCH(high_on) PATCH(high_off) PATCH(watch_s) PATCH(high_s) PATCH(high_clear_s) PATCH(clear_s) PATCH(trend_ppm)
#undef PATCH
            if(recognized) continue;
            if(key=="demo" && cJSON_IsBool(entry)) { assign(state.p,"demo",cJSON_CreateBool(cJSON_IsTrue(entry))); rule_changed=true; }
            else if(key=="buzzer_enabled" && cJSON_IsBool(entry)) assign(state.p,"buzzer_enabled",cJSON_Duplicate(entry,true));
            else if(key=="ssid" && cJSON_IsString(entry) && strlen(entry->valuestring)>=1 && strlen(entry->valuestring)<=32) {}
            else if(key=="wifi_password" && cJSON_IsString(entry) && (strlen(entry->valuestring)==0 || (strlen(entry->valuestring)>=8 && strlen(entry->valuestring)<=63))) {}
            else return bad(422,"Unknown field or invalid field type");
        }
        if(!rules.valid()) return bad(422,"Invalid hysteresis, dwell time, or trend threshold");
        if(cJSON_HasObjectItem(patch,"ssid") || cJSON_HasObjectItem(patch,"wifi_password")) {
            auto networks=config_.wifi_profiles;
            std::string ssid=string_field(patch,"ssid",config_.ssid),password;
            auto saved=std::find_if(networks.begin(),networks.end(),[&](const WifiProfile& p){return p.ssid==ssid;});
            if(cJSON_HasObjectItem(patch,"wifi_password")) password=string_field(patch,"wifi_password");
            else if(saved!=networks.end()) password=saved->password;
            else return bad(422,"Supply a password for a new network, or an empty string for an open network");
            if(!remember_wifi(networks,{ssid,password},true)) return bad(422,"Invalid Wi-Fi profile or saved-network limit reached (8)");
            save_wifi_profiles(state.p,networks);
        }
        if(rule_changed && !config_.run_id.empty()) return bad(409,"Stop the formal run before changing measurement rules");
        if(rule_changed) assign(state.p,"rules_revision",cJSON_CreateNumber(config_.revision+1));
    } else if(op=="wifi") {
        auto networks=config_.wifi_profiles;
        auto action=string_field(req,"action"),ssid=string_field(req,"ssid");
        if(ssid.empty() || ssid.size()>32) return bad(422,"Invalid SSID");
        if(action=="upsert") {
            auto it=std::find_if(networks.begin(),networks.end(),[&](const WifiProfile& p){return p.ssid==ssid;});
            auto* password=cJSON_GetObjectItemCaseSensitive(req,"wifi_password");
            if(password && !cJSON_IsString(password)) return bad(422,"Password must be a string");
            if(!password && it==networks.end()) return bad(422,"A new network requires a password or an explicit empty string");
            if(!remember_wifi(networks,{ssid,password?password->valuestring:it->password})) return bad(422,"Invalid profile or eight-network limit reached");
        } else {
            if(cJSON_HasObjectItem(req,"wifi_password")) return bad(422,"Password is only allowed when saving a network");
            if(action=="delete") {if(!remove_wifi(networks,ssid)) return bad(409,"Network no longer exists");}
            else if(action=="prefer") {if(!prefer_wifi(networks,ssid)) return bad(409,"Network no longer exists");}
            else return bad(422,"Unknown Wi-Fi operation");
        }
        save_wifi_profiles(state.p,networks);
    } else if(op=="uplink") {
        auto* enabled=cJSON_GetObjectItemCaseSensitive(req,"enabled");
        if(!cJSON_IsBool(enabled)) return bad(422,"enabled must be boolean");
        if(cJSON_IsTrue(enabled)&&config_.ssid.empty()) return bad(422,"Configure Wi-Fi first");
        assign(state.p,"uplink_enabled",cJSON_Duplicate(enabled,true));
    } else if(op=="mute") {
        double duration=number_field(req,"duration_s",0);
        if(duration<1 || duration>300 || floor(duration)!=duration) return bad(422,"Mute duration must be 1..300 seconds");
        if(!snapshot_value.view.episode.active || string_field(req,"episode_id")!=snapshot_value.view.episode.id.str()) return bad(409,"Episode is no longer active");
        new_mute=monotonic_ms()+uint64_t(duration*1000); muted=snapshot_value.view.episode.id;
    } else if(op=="commission") {
        if(!bool_field(req,"fresh_air_confirmed") || snapshot_value.commission_elapsed<43200000 || !snapshot_value.self_test_ok) return bad(409,"Complete 12 hours of continuous sensing and confirm fresh-air preparation");
        assign(state.p,"commissioned",cJSON_CreateBool(true)); assign(state.p,"sensor_serial",cJSON_CreateString(snapshot_value.sensor_serial.c_str()));
    } else if(op=="run_start") {
        uint64_t now=utc_ms();
        if(!now || snapshot_value.view.quality!=Quality::Valid || !config_.commissioned || config_.demo || !config_.run_id.empty()) return bad(409,"A run requires valid commissioned sensing, UTC, and normal mode");
        assign(state.p,"run_id",cJSON_CreateString(random_hex(16).c_str())); assign(state.p,"run_t0",cJSON_CreateNumber((now/60000+1)*60000));
    } else if(op=="run_stop") {
        assign(state.p,"run_id",cJSON_CreateString("")); assign(state.p,"run_t0",cJSON_CreateNumber(0));
    } else if(op=="ota") {
        if(!config_.run_id.empty() || !config_.uplink || !utc_ms() || !snapshot_value.sta_connected) return bad(409,"OTA requires online UTC and no active formal run");
    } else return bad(422,"Unknown operation");
    uint32_t revision=config_.revision+1;
    assign(state.p,"revision",cJSON_CreateNumber(revision));
    Json response(result(op=="ota"?"accepted":"applied",200,revision,op=="uplink"?"Intent saved; observe actual state in status":"Configuration committed"));
    cJSON_AddStringToObject(response.p,"request_id",id.c_str());
    auto* cached=cJSON_CreateObject(); cJSON_AddStringToObject(cached,"id",id.c_str()); cJSON_AddStringToObject(cached,"hash",hash.c_str());
    cJSON_AddBoolToObject(cached,"remote",remote); cJSON_AddBoolToObject(cached,"reported",false);
    cJSON_AddNumberToObject(cached,"revision",revision);cJSON_AddBoolToObject(cached,"ota",op=="ota");cJSON_AddBoolToObject(cached,"intent",op=="uplink");cJSON_AddItemToArray(requests,cached);
    // Revision checks still prevent reapplying an evicted request. Bound the
    // receipt cache so parsing persisted settings fits alongside BLE and TLS.
    while(cJSON_GetArraySize(requests)>16) {
        int evict=-1;
        for(int i=0;i<cJSON_GetArraySize(requests);++i) {
            auto* old=cJSON_GetArrayItem(requests,i);
            if(!bool_field(old,"remote")||bool_field(old,"reported")) {evict=i;break;}
        }
        if(evict<0) return bad(503,"Unreported command result cache is full");
        cJSON_DeleteItemFromArray(requests,evict);
    }
    if(!save(state.p)) return bad(503,"NVS commit failed");
    config_.mute_until=new_mute; config_.muted_episode=muted;
    auto audit=event_record(Event::Config,snapshot_value.view.episode,revision,snapshot_value.view.level);
    audit.utc_ms=utc_ms();
    storage.enqueue(audit,config_,snapshot_value.view.episode,snapshot_value.view.level);
    if(op=="ota") ota_requested=true;
    return cJSON_Duplicate(response.p,true);
}
cJSON* ConfigManager::command_results() {
    Lock lock(mutex_); Json state(cJSON_Parse(state_.c_str())); auto* out=cJSON_CreateArray(); cJSON* entry;
    cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(state.p,"requests"))
        if(bool_field(entry,"remote")&&!bool_field(entry,"reported")) {cJSON_AddItemToArray(out,cached_response(entry));break;}
    return out;
}
bool ConfigManager::acknowledge_result(const std::string& id) {
    Lock lock(mutex_); Json state(cJSON_Parse(state_.c_str())); cJSON* entry;
    cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(state.p,"requests")) if(string_field(entry,"id")==id) {
        assign(entry,"reported",cJSON_CreateBool(true)); return save(state.p);
    }
    return false;
}
}
