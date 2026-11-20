#include "aircue/runtime.hpp"
#include "aircue/update.hpp"
#include "secrets.hpp"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_netif.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include <algorithm>
#include <cstring>

namespace aircue {
SemaphoreHandle_t uplink_memory=xSemaphoreCreateCounting(1,1);
UplinkMemory::UplinkMemory(bool wait) {
    // TLS handshakes and upload batches share a bounded transient RAM budget.
    if(!wait) {held=xSemaphoreTake(uplink_memory,0)==pdTRUE;return;}
    while(xSemaphoreTake(uplink_memory,pdMS_TO_TICKS(1000))!=pdTRUE) esp_task_wdt_reset();
    held=true;
}
static esp_netif_t* ap_netif=nullptr;
struct NetworkEvent { int type=0,reason=0; char ssid[33]{},ip[16]{}; };
static QueueHandle_t network_events=xQueueCreate(16,sizeof(NetworkEvent));
static void wifi_event(void*,esp_event_base_t base,int32_t event,void* data) {
    NetworkEvent message;
    if(base==IP_EVENT&&event==IP_EVENT_STA_GOT_IP) {
        message.type=1;auto* ip=static_cast<ip_event_got_ip_t*>(data);
        snprintf(message.ip,sizeof(message.ip),IPSTR,IP2STR(&ip->ip_info.ip));
    } else if(base==WIFI_EVENT&&event==WIFI_EVENT_STA_DISCONNECTED) {
        message.type=2;auto* disconnected=static_cast<wifi_event_sta_disconnected_t*>(data);
        message.reason=disconnected->reason;
        memcpy(message.ssid,disconnected->ssid,std::min<size_t>(32,disconnected->ssid_len));
    }
    if(message.type) xQueueSend(network_events,&message,0);
}
bool network_start() {
    if(esp_netif_init()!=ESP_OK || esp_event_loop_create_default()!=ESP_OK) return false;
    // Management is provided by BLE and HTTPS; the device never creates an AP.
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT(); if(esp_wifi_init(&init)!=ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,nullptr); esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,nullptr);
    return esp_wifi_set_mode(WIFI_MODE_STA)==ESP_OK;
}
bool local_peer_allowed(int fd) {
    sockaddr_in local{},peer{}; socklen_t len=sizeof(local);
    if(getsockname(fd,reinterpret_cast<sockaddr*>(&local),&len)!=0 || getpeername(fd,reinterpret_cast<sockaddr*>(&peer),&len)!=0) return false;
    esp_netif_ip_info_t info; if(!ap_netif || esp_netif_get_ip_info(ap_netif,&info)!=ESP_OK || local.sin_addr.s_addr!=info.ip.addr) return false;
    wifi_sta_list_t wifi{}; wifi_sta_mac_ip_list_t list{};
    if(esp_wifi_ap_get_sta_list(&wifi)!=ESP_OK || esp_wifi_ap_get_sta_list_with_ip(&wifi,&list)!=ESP_OK) return false;
    for(int i=0;i<list.num;++i) if(list.sta[i].ip.addr==peer.sin_addr.s_addr) return true;
    return false;
}
void network_task(void*) {
    esp_task_wdt_add(nullptr); bool active=false,connected=false,sntp=false,connecting=false;
    std::vector<WifiProfile> profiles; WifiRotation rotation;
    uint64_t next=0,connect_deadline=0; int attempted=-1;
    while(true) {
        esp_task_wdt_reset(); auto cfg=config.get(); uint64_t now=monotonic_ms(); NetworkEvent event;
        if(!cfg.uplink || cfg.wifi_profiles.empty()) {
            if(sntp) { esp_sntp_stop(); sntp=false; }
            if(active) { esp_wifi_disconnect(); esp_wifi_stop(); active=false; }
            connected=connecting=false;attempted=-1;
        } else if(!active || cfg.wifi_profiles!=profiles) {
            // Preserve a working link when another profile or its priority changes.
            int keep=active && attempted>=0?wifi_profile_index(cfg.wifi_profiles,profiles[attempted]):-1;
            if(keep>=0 && (connected||connecting)) {
                profiles=cfg.wifi_profiles;attempted=keep;rotation.reset();rotation.index=keep;
            } else {
                if(sntp) { esp_sntp_stop(); sntp=false; }
                attempted=-1;connected=connecting=false;
                if(active) esp_wifi_disconnect();
                else if(esp_wifi_start()!=ESP_OK) {vTaskDelay(pdMS_TO_TICKS(1000));continue;}
                profiles=cfg.wifi_profiles;rotation.reset();active=true;next=now+1000;
            }
        }
        auto failed=[&]() {
            if(sntp) {esp_sntp_stop();sntp=false;}
            connected=connecting=false;attempted=-1;
            next=now+rotation.failure(profiles.size())+(esp_random()%250);
        };
        while(xQueueReceive(network_events,&event,0)==pdTRUE) {
            if(!active || attempted<0) continue;
            if(event.type==1) {
                wifi_ap_record_t ap{};
                if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK && profiles[attempted].ssid==std::string(reinterpret_cast<char*>(ap.ssid),strnlen(reinterpret_cast<char*>(ap.ssid),32))) {
                    connected=true;connecting=false;rotation.connected();
                    Lock lock(runtime_mutex);runtime.sta_ip=event.ip;runtime.wifi_disconnect_reason=0;
                }
            } else if(event.type==2 && profiles[attempted].ssid==event.ssid && (connected||connecting)) {
                {Lock lock(runtime_mutex);runtime.wifi_disconnect_reason=event.reason;}
                failed();
            }
        }
        if(active) {
            if(connecting && now>=connect_deadline) {failed();esp_wifi_disconnect();}
            if(!connected && !connecting && now>=next) {
                attempted=static_cast<int>(rotation.index);
                const auto& profile=profiles[rotation.index]; wifi_config_t sta{};
                strncpy(reinterpret_cast<char*>(sta.sta.ssid),profile.ssid.c_str(),sizeof(sta.sta.ssid));
                strncpy(reinterpret_cast<char*>(sta.sta.password),profile.password.c_str(),sizeof(sta.sta.password));
                sta.sta.pmf_cfg.capable=true;
                // esp_wifi_connect scans for this saved SSID; no scan is run while manually offline.
                connecting=esp_wifi_set_config(WIFI_IF_STA,&sta)==ESP_OK && esp_wifi_connect()==ESP_OK;
                connect_deadline=now+15000;
                if(!connecting) failed();
                Lock lock(runtime_mutex); ++runtime.wifi_retries;
            }
            if(connected&&!sntp) { esp_sntp_setoperatingmode(SNTP_OPMODE_POLL); esp_sntp_setservername(0,const_cast<char*>("pool.ntp.org")); esp_sntp_init(); sntp=true; }
        }
        {
            Lock lock(runtime_mutex);runtime.sta_connected=connected;
            runtime.connected_ssid=connected&&attempted>=0?profiles[attempted].ssid:"";
            runtime.wifi_target_ssid=attempted>=0?profiles[attempted].ssid:"";
            runtime.network=!cfg.uplink?"MANUAL_OFFLINE":cfg.wifi_profiles.empty()?"NO_SAVED_NETWORK":connected?"WIFI_CONNECTED":connecting?"CONNECTING":"RETRY_BACKOFF";
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
struct HttpBuffer {
    char* text=nullptr;size_t size=0;bool overflow=false;
    ~HttpBuffer() {free(text);}
    char* release() {auto* value=text;text=nullptr;return value;}
};
static esp_err_t receive_http(esp_http_client_event_t* event) {
    auto* buffer=static_cast<HttpBuffer*>(event->user_data);
    if(event->event_id==HTTP_EVENT_ON_DATA && event->data_len>0) {
        size_t size=buffer->size+event->data_len;
        if(size>16384) {buffer->overflow=true;return ESP_FAIL;}
        // A failed receive allocation is a transport retry, never a C++ abort.
        auto* text=static_cast<char*>(realloc(buffer->text,size+1));
        if(!text) {buffer->overflow=true;return ESP_FAIL;}
        buffer->text=text;memcpy(text+buffer->size,event->data,event->data_len);
        buffer->size=size;text[size]='\0';
    }
    return ESP_OK;
}
static void configure_trust(esp_http_client_config_t& cfg) {
#ifdef AIRCUE_CLOUD_CA_PEM
    cfg.cert_pem=AIRCUE_CLOUD_CA_PEM;
#else
    cfg.crt_bundle_attach=esp_crt_bundle_attach;
#endif
}
static esp_err_t ota_credentials(esp_http_client_handle_t client) {
    std::string bearer=std::string("Bearer ")+AIRCUE_DEVICE_TOKEN;
    return esp_http_client_set_header(client,"Authorization",bearer.c_str());
}
static int post(const std::string& path,std::string_view data,JsonText& response) {
    if(!config.get().uplink || !utc_ms()) return 0;
    std::string url=std::string(AIRCUE_CLOUD_ORIGIN)+path;
    if(data.size()>16384) return 413;
    HttpBuffer buffer; esp_http_client_config_t cfg{}; cfg.url=url.c_str(); cfg.timeout_ms=5000;
    configure_trust(cfg); cfg.event_handler=receive_http; cfg.user_data=&buffer;
    cfg.disable_auto_redirect=true; cfg.buffer_size=1024;
    auto client=esp_http_client_init(&cfg); if(!client) return 0;
    std::string bearer=std::string("Bearer ")+AIRCUE_DEVICE_TOKEN;
    esp_http_client_set_method(client,HTTP_METHOD_POST); esp_http_client_set_header(client,"Content-Type","application/json");
    esp_http_client_set_header(client,"Authorization",bearer.c_str()); esp_http_client_set_post_field(client,data.data(),data.size());
    auto err=esp_http_client_perform(client); int status=err==ESP_OK&&!buffer.overflow?esp_http_client_get_status_code(client):0;
    esp_http_client_cleanup(client); response.reset(buffer.release()); return status;
}
static void perform_ota() {
    auto cfg=config.get(); auto view=snapshot().view;
    auto set_status=[](const char* value){Lock lock(runtime_mutex); runtime.ota=value;printf("AIRCUE_OTA state=%s free_heap=%lu\n",value,static_cast<unsigned long>(esp_get_free_heap_size()));};
    if(!cfg.uplink || !cfg.run_id.empty() || !utc_ms()) {set_status("REJECTED");return;}
    // MQTT's persistent TLS connection is resumed after maintenance. Sampling,
    // flash recording, BLE advertising and physical outputs remain independent.
    mqtt_pause_requested=true;
    struct ResumeMirror {~ResumeMirror(){mqtt_pause_requested=false;}} resume;
    set_status("PREPARING");
    uint64_t pause_deadline=monotonic_ms()+15000;
    while(!mqtt_paused && monotonic_ms()<pause_deadline) {esp_task_wdt_reset();vTaskDelay(pdMS_TO_TICKS(100));}
    if(!mqtt_paused) {set_status("TRANSPORT_BUSY");return;}
    UplinkMemory memory;
    auto record=event_record(Event::Maintenance,view.episode,cfg.revision,view.level); record.utc_ms=utc_ms(); storage.enqueue(record,cfg,view.episode,view.level);
    if(!storage.flush(5000)) {set_status("STORAGE_FAILED");return;}
    std::string url=std::string(AIRCUE_CLOUD_ORIGIN)+"/firmware/aircue-esp32.bin";
    esp_http_client_config_t http{}; http.url=url.c_str(); http.timeout_ms=5000; http.buffer_size=1024; configure_trust(http); http.disable_auto_redirect=true;
    esp_https_ota_config_t ota{}; ota.http_config=&http; ota.http_client_init_cb=ota_credentials;
    // Range requests bound incoming TLS records while autonomous sampling continues.
    ota.partial_http_download=true;ota.max_http_request_size=8192;
    esp_https_ota_handle_t handle=nullptr;
    set_status("DOWNLOADING");
    if(esp_https_ota_begin(&ota,&handle)!=ESP_OK) {set_status("DOWNLOAD_FAILED");return;}
    esp_app_desc_t description{};
    if(esp_https_ota_get_img_desc(handle,&description)!=ESP_OK || strncmp(description.project_name,esp_app_get_description()->project_name,sizeof(description.project_name))!=0 || !newer_release_version(description.version,esp_app_get_description()->version)) {
        esp_https_ota_abort(handle);set_status("WRONG_TARGET_OR_VERSION");return;
    }
    esp_err_t err;uint64_t deadline=monotonic_ms()+600000;int reported=0;
    do {
        esp_task_wdt_reset();
        if(!config.get().uplink || monotonic_ms()>deadline) { esp_https_ota_abort(handle);set_status("CANCELLED");return; }
        err=esp_https_ota_perform(handle);
        int downloaded=esp_https_ota_get_image_len_read(handle);
        if(downloaded-reported>=65536) {reported=downloaded;printf("AIRCUE_OTA downloaded=%d free_heap=%lu\n",downloaded,static_cast<unsigned long>(esp_get_free_heap_size()));}
    } while(err==ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    if(err!=ESP_OK || !esp_https_ota_is_complete_data_received(handle)) { esp_https_ota_abort(handle);set_status("INCOMPLETE");return; }
    if(esp_https_ota_finish(handle)!=ESP_OK) {set_status("SIGNATURE_OR_IMAGE_REJECTED");return;}
    set_status("VERIFIED_RESTARTING"); storage.flush(5000); esp_restart();
}
void sync_task(void*) {
    esp_task_wdt_add(nullptr); uint64_t next=0; uint32_t backoff=1000;
    const std::string path=std::string("/v1/devices/")+AIRCUE_DEVICE_ID;
    while(true) {
        esp_task_wdt_reset(); uint64_t now=monotonic_ms(); auto cfg=config.get(); auto state=snapshot();
        if(ota_requested.exchange(false)) perform_ota();
        if(!cfg.uplink || !state.sta_connected || !utc_ms() || now<next) {
            if(!cfg.uplink || !state.sta_connected || !utc_ms()) {
                Lock lock(runtime_mutex);
                runtime.cloud=!cfg.uplink?"OFFLINE":!state.sta_connected?"WAITING_FOR_WIFI":"WAITING_FOR_TIME";
                runtime.tls_ok=false;
            }
            vTaskDelay(pdMS_TO_TICKS(200)); continue;
        }
        UplinkMemory memory;
        Json body(cJSON_CreateObject()),gaps(storage.gaps()); bool has_gaps=cJSON_GetArraySize(gaps.p)>0;
        cJSON_AddItemToObject(body.p,"gaps",gaps.release());
        constexpr size_t batch_limit=2;
        auto batch=has_gaps?std::vector<StoredRecord>{}:storage.page(storage.ack,batch_limit);
        auto* records=cJSON_AddArrayToObject(body.p,"records");
        for(auto& record:batch) cJSON_AddItemToArray(records,stored_json(record,true));
        auto* snapshots=cJSON_AddArrayToObject(body.p,"config_snapshots");
        std::vector<uint32_t> revisions;
        for(auto& item:batch) {
            uint32_t revision=item.record.type==1?read_minute(item.record).config_revision:get32(item.record.payload.data()+40);
            if(std::find(revisions.begin(),revisions.end(),revision)!=revisions.end()) continue;
            revisions.push_back(revision); auto* snapshot=storage.config_snapshot(revision);
            if(cJSON_IsObject(snapshot)) cJSON_AddItemToArray(snapshots,snapshot); else cJSON_Delete(snapshot);
        }
        cJSON_AddItemToObject(body.p,"status",status_json(false));
        auto payload=json_take(body.release());
        if(payload.empty()) {next=monotonic_ms()+5000;Lock lock(runtime_mutex);runtime.cloud="MEMORY_BUSY";continue;}
        // Release the JSON tree before allocating the TLS handshake buffers.
        JsonText response; int code=post(path+"/ingest",payload.view(),response);
        payload.reset();
        if(code==200) {
            Json reply(cJSON_Parse(response.c_str())); auto* ack_ids=cJSON_GetObjectItemCaseSensitive(reply.p,"ack");
            if(!cJSON_IsArray(ack_ids)) { next=now+30000; continue; }
            uint64_t acknowledged=storage.ack;
            for(auto& item:batch) {
                bool found=false; cJSON* id;
                cJSON_ArrayForEach(id,ack_ids) if(cJSON_IsString(id)&&item.record.id.str()==id->valuestring) found=true;
                if(!found) break;
                acknowledged=item.order;
            }
            storage.acknowledge(acknowledged);
            if(has_gaps) storage.acknowledge_gaps(cJSON_GetObjectItemCaseSensitive(reply.p,"gap_ack"));
            cJSON* command; unsigned processed=0;bool command_received=false;
            cJSON_ArrayForEach(command,cJSON_GetObjectItemCaseSensitive(reply.p,"commands")) {
                if(processed++>=1) break;
                if(!config.get().uplink) break;
                command_received=true;
                Json result(config.apply(command,string_field(command,"operation"),true));
                if(!cJSON_HasObjectItem(result.p,"request_id")) cJSON_AddStringToObject(result.p,"request_id",string_field(command,"request_id").c_str());
                if(string_field(result.p,"status")!="applied") {JsonText ignored;post(path+"/command-results",json_text(result.p),ignored);}
                esp_task_wdt_reset();
            }
            Json pending(config.command_results()); cJSON* result; processed=0;
            cJSON_ArrayForEach(result,pending.p) {
                if(processed++>=1) break;
                JsonText ignored;if(post(path+"/command-results",json_text(result),ignored)==200) config.acknowledge_result(string_field(result,"request_id"));
                esp_task_wdt_reset();
            }
            bool fully_acked=batch.empty()||acknowledged==batch.back().order;
            backoff=1000; next=now+((command_received||batch.size()==batch_limit||has_gaps||!fully_acked)?1000+esp_random()%250:30000);
            Lock lock(runtime_mutex); runtime.tls_ok=true; runtime.last_ack_utc=utc_ms(); runtime.cloud=batch.size()==batch_limit||!fully_acked?"CATCHING_UP":"SYNCED";
        } else {
            uint32_t delay=(code==401||code==403||code==409)?300000:backoff;
            next=now+delay+esp_random()%1000; backoff=std::min<uint32_t>(60000,backoff*2);
            Lock lock(runtime_mutex); ++runtime.sync_retries; runtime.tls_ok=code!=0;
            runtime.cloud=code==401||code==403?"AUTH_FAILED":code==409?"CONTENT_CONFLICT":code==0?"TLS_OR_TRANSPORT_FAILED":"SERVER_UNAVAILABLE";
        }
    }
}
}
