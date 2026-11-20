#include "aircue/runtime.hpp"
#include "secrets.hpp"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "mbedtls/md.h"
#include "mqtt_client.h"
#include "nvs.h"
#include <algorithm>
#include <cstring>
#if __has_include("mqtt-secrets.hpp")
#include "mqtt-secrets.hpp"
#endif

namespace aircue {
std::atomic<bool> mqtt_pause_requested{false},mqtt_paused{true};
#ifdef AIRCUE_MQTT_HOST
namespace {
std::atomic<bool> connected{false};
std::atomic<bool> handshake_memory{false};
esp_mqtt_client_handle_t active_client=nullptr;
QueueHandle_t acknowledgments=nullptr,commands=nullptr;
struct Incoming {char topic[128];char text[2049];};
const std::string prefix=std::string("aircue/")+AIRCUE_DEVICE_ID+"/v1/";
struct Mirror {uint64_t ack=0,after=0;unsigned slot=0;};
Mirror mirrors[2];
void state(const char* value) {Lock lock(runtime_mutex);runtime.mqtt=value;}
std::string signature(const std::string& topic,std::string_view body) {
    uint8_t bytes[32];mbedtls_md_context_t context;mbedtls_md_init(&context);
    int error=mbedtls_md_setup(&context,mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),1);
    if(!error) error=mbedtls_md_hmac_starts(&context,reinterpret_cast<const uint8_t*>(AIRCUE_DEVICE_TOKEN),strlen(AIRCUE_DEVICE_TOKEN));
    if(!error) error=mbedtls_md_hmac_update(&context,reinterpret_cast<const uint8_t*>(topic.data()),topic.size());
    const uint8_t newline='\n';if(!error) error=mbedtls_md_hmac_update(&context,&newline,1);
    if(!error) error=mbedtls_md_hmac_update(&context,reinterpret_cast<const uint8_t*>(body.data()),body.size());
    if(!error) error=mbedtls_md_hmac_finish(&context,bytes);
    mbedtls_md_free(&context);if(error) return "";
    return hex(bytes,sizeof(bytes));
}
std::string envelope(const std::string& topic,std::string_view body) {
    size_t size=body.size()+128;
    for(unsigned char byte:body) size+=byte<32?5:byte=='"'||byte=='\\'?1:0;
    if(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)<size+1024 || esp_get_free_heap_size()<size+8192) return {};
    std::string out;out.reserve(size);out="{\"body\":\"";
    for(unsigned char byte:body) {
        if(byte=='"' || byte=='\\') out+='\\';
        if(byte<32) {const char* digits="0123456789abcdef";out+="\\u00";out+=digits[byte>>4];out+=digits[byte&15];}
        else out+=static_cast<char>(byte);
    }
    auto mac=signature(topic,body);if(mac.empty()) return {};
    out+="\",\"signature\":\""+mac+"\"}";return out;
}
bool persist(bool load) {
    nvs_handle_t handle;if(nvs_open("aircue_mqtt",NVS_READWRITE,&handle)!=ESP_OK) return false;
    uint8_t bytes[48]{};bool ok=true;
    if(load) {
        size_t size=sizeof(bytes);auto error=nvs_get_blob(handle,"progress",bytes,&size);
        if(error==ESP_OK && size==sizeof(bytes) && get32(bytes+44)==crc32(bytes,44)) {
            for(unsigned i=0;i<2;++i) {mirrors[i].ack=get64(bytes+i*20);mirrors[i].after=get64(bytes+i*20+8);mirrors[i].slot=get32(bytes+i*20+16);}
        } else if(error!=ESP_ERR_NVS_NOT_FOUND) ok=false;
    } else {
        for(unsigned i=0;i<2;++i) {put64(bytes+i*20,mirrors[i].ack);put64(bytes+i*20+8,mirrors[i].after);put32(bytes+i*20+16,mirrors[i].slot);}
        put32(bytes+44,crc32(bytes,44));ok=nvs_set_blob(handle,"progress",bytes,sizeof(bytes))==ESP_OK && nvs_commit(handle)==ESP_OK;
    }
    nvs_close(handle);
    storage.remote_minute_ack=mirrors[0].ack;storage.remote_event_ack=mirrors[1].ack;return ok;
}
void event(void*,esp_event_base_t,int32_t id,void* data) {
    auto* event=static_cast<esp_mqtt_event_handle_t>(data);
    if(id==MQTT_EVENT_BEFORE_CONNECT) {
        xSemaphoreTake(uplink_memory,portMAX_DELAY);handshake_memory=true;
    } else if(id==MQTT_EVENT_CONNECTED) {
        if(handshake_memory.exchange(false)) xSemaphoreGive(uplink_memory);
        connected=true;state("CONNECTED");
        esp_mqtt_client_subscribe(event->client,(prefix+"command").c_str(),1);
        esp_mqtt_client_subscribe(event->client,(prefix+"result-ack").c_str(),1);
    } else if(id==MQTT_EVENT_DISCONNECTED) {if(handshake_memory.exchange(false)) xSemaphoreGive(uplink_memory);connected=false;state("RECONNECTING");}
    else if(id==MQTT_EVENT_ERROR) {if(handshake_memory.exchange(false)) xSemaphoreGive(uplink_memory);state("AUTH_TLS_OR_NETWORK_FAILED");}
    else if(id==MQTT_EVENT_PUBLISHED) xQueueSend(acknowledgments,&event->msg_id,0);
    else if(id==MQTT_EVENT_DATA) {
        static Incoming input{};static int received=0,total=0;
        if(event->current_data_offset==0) {
            input={};received=0;total=event->total_data_len;
            if(event->topic_len<=0 || event->topic_len>=int(sizeof(input.topic)) || total<=0 || total>=int(sizeof(input.text))) {total=0;return;}
            memcpy(input.topic,event->topic,event->topic_len);
        }
        if(!total || event->current_data_offset!=received || received+event->data_len>total) {total=0;return;}
        memcpy(input.text+received,event->data,event->data_len);received+=event->data_len;
        if(received==total) {xQueueSend(commands,&input,0);total=0;}
    }
}
bool publish(esp_mqtt_client_handle_t client,const std::string& suffix,cJSON* body,bool retain=true) {
    Json owned(body);
    if(!connected || !config.get().uplink) return false;
    auto serialized=json_take(owned.release());if(serialized.empty()) return false;
    auto topic=prefix+suffix,text=envelope(topic,serialized.view());
    if(text.empty()) return false;
    if(text.size()>16000) {state("MESSAGE_TOO_LARGE");return false;}
    int sent=esp_mqtt_client_publish(client,topic.c_str(),text.c_str(),text.size(),1,retain);
    if(sent<0) return false;
    uint64_t deadline=monotonic_ms()+10000;int id;
    while(connected && monotonic_ms()<deadline && config.get().uplink) {
        esp_task_wdt_reset();
        if(xQueueReceive(acknowledgments,&id,pdMS_TO_TICKS(100))==pdTRUE && id==sent) {
            Lock lock(runtime_mutex);runtime.mqtt_last_ack=utc_ms();return true;
        }
    }
    return false;
}
bool mirror(esp_mqtt_client_handle_t client,unsigned index) {
    auto& current=mirrors[index];auto page=storage.page(current.after,8,index+1);
    if(page.empty() || page.back().order<=current.ack) return true;
    Json body(cJSON_CreateObject());auto* records=cJSON_AddArrayToObject(body.p,"records");
    auto* snapshots=cJSON_AddArrayToObject(body.p,"config_snapshots");std::vector<uint32_t> revisions;
    for(const auto& item:page) {
        cJSON_AddItemToArray(records,stored_json(item,true));
        uint32_t revision=item.record.type==1?read_minute(item.record).config_revision:get32(item.record.payload.data()+40);
        if(std::find(revisions.begin(),revisions.end(),revision)==revisions.end()) {
            revisions.push_back(revision);auto* value=storage.config_snapshot(revision);
            if(cJSON_IsObject(value)) cJSON_AddItemToArray(snapshots,value);else cJSON_Delete(value);
        }
    }
    unsigned slots=index==0?640:128;
    auto topic=std::string("history/")+(index==0?"minutes/":"events/")+std::to_string(current.slot%slots);
    if(!publish(client,topic,body.release())) return false;
    auto previous=current;current.ack=page.back().order;
    if(page.size()==8) {current.after=current.ack;current.slot=(current.slot+1)%slots;}
    if(!persist(false)) {current=previous;state("PROGRESS_SAVE_FAILED");return false;}
    return true;
}
void process_commands() {
    Incoming input{};
    if(xQueueReceive(commands,&input,0)!=pdTRUE) return;
    std::string topic=input.topic;
    if(topic!=prefix+"command" && topic!=prefix+"result-ack") return;
    Json packet(cJSON_Parse(input.text));auto text=string_field(packet.p,"body");
    if(text.empty() || !equal_secret(signature(topic,text),string_field(packet.p,"signature"))) return;
    Json body(cJSON_Parse(text.c_str()));
    if(topic==prefix+"result-ack") config.acknowledge_result(string_field(body.p,"request_id"));
    else {
        Json result(config.apply(body.p,string_field(body.p,"operation"),true));
        // Successful results are already durable in ConfigManager's request cache.
        if(!cJSON_HasObjectItem(result.p,"request_id")) cJSON_AddStringToObject(result.p,"request_id",string_field(body.p,"request_id").c_str());
        // Rejected results are returned by the caller during the next command retry.
        if(string_field(result.p,"status")!="applied" && string_field(result.p,"status")!="accepted") {
            // Cache only the transport response in RAM; a retried request is revalidated.
            publish(active_client,"result",result.release(),false);
        }
    }
}
}
#endif
void mqtt_task(void*) {
#ifndef AIRCUE_MQTT_HOST
    {Lock lock(runtime_mutex);runtime.mqtt="NOT_CONFIGURED";}
    vTaskDelete(nullptr);
#else
    esp_task_wdt_add(nullptr);acknowledgments=xQueueCreate(8,sizeof(int));commands=xQueueCreate(2,sizeof(Incoming));
    if(!acknowledgments || !commands || !persist(true)) {state("INITIALIZATION_FAILED");esp_task_wdt_delete(nullptr);vTaskDelete(nullptr);return;}
    uint64_t next_status=0,next_records=0,next_result=0;uint32_t last_status_revision=0;
    while(true) {
        esp_task_wdt_reset();auto cfg=config.get();auto view=snapshot();uint64_t now=monotonic_ms();
        if(mqtt_pause_requested || !cfg.uplink || !view.sta_connected || !utc_ms()) {
            if(active_client) {esp_mqtt_client_stop(active_client);esp_mqtt_client_destroy(active_client);if(handshake_memory.exchange(false)) xSemaphoreGive(uplink_memory);active_client=nullptr;connected=false;}
            mqtt_paused=true;
            state(mqtt_pause_requested?"OTA_MAINTENANCE":!cfg.uplink?"OFFLINE":!view.sta_connected?"WAITING_FOR_WIFI":"WAITING_FOR_TIME");
            vTaskDelay(pdMS_TO_TICKS(500));continue;
        }
        if(!active_client) {
            mqtt_paused=false;
            esp_mqtt_client_config_t options{};options.broker.address.uri="mqtts://" AIRCUE_MQTT_HOST ":8883";
            options.broker.verification.crt_bundle_attach=esp_crt_bundle_attach;
            options.credentials.client_id=AIRCUE_DEVICE_ID "-board";options.credentials.username=AIRCUE_MQTT_USER;
            options.credentials.authentication.password=AIRCUE_MQTT_PASSWORD;
            options.session.keepalive=60;options.network.timeout_ms=5000;options.network.reconnect_timeout_ms=10000;
            options.buffer.size=2048;options.buffer.out_size=1024;options.outbox.limit=16000;options.task.stack_size=5120;
            active_client=esp_mqtt_client_init(&options);
            if(!active_client) {state("NO_MEMORY");vTaskDelay(pdMS_TO_TICKS(5000));continue;}
            esp_mqtt_client_register_event(active_client,MQTT_EVENT_ANY,event,nullptr);esp_mqtt_client_start(active_client);
            state("CONNECTING");next_status=next_records=next_result=0;
        }
        if(connected) {
            UplinkMemory memory;
            process_commands();
            if(config.get().revision!=last_status_revision) next_status=0;
            if(now>=next_records) {
                bool ok=mirror(active_client,0) && mirror(active_client,1);
                next_records=now+(ok?1000:10000);if(ok) state("BROKER_ACKNOWLEDGED");
            }
            if(now>=next_status) {
                Json body(cJSON_CreateObject());cJSON_AddItemToObject(body.p,"status",status_json(false));
                bool ok=publish(active_client,"state",body.release());next_status=now+(ok?30000:5000);last_status_revision=config.get().revision;
            }
            if(now>=next_result) {
                Json results(config.command_results());auto* result=cJSON_DetachItemFromArray(results.p,0);
                if(result) publish(active_client,"result",result,false);
                next_result=now+5000;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#endif
}
}
