#include "aircue/ble.hpp"
#include "aircue/ble_live.hpp"
#include "aircue/ble_fragment.hpp"
#include "aircue/runtime.hpp"
#include "secrets.hpp"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include <algorithm>
#include <cstring>
extern "C" {
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
void ble_store_config_init(void);
}

namespace aircue {
namespace {
// UUIDs are encoded least-significant byte first by NimBLE.
const ble_uuid128_t service_uuid=BLE_UUID128_INIT(0x01,0x00,0x42,0x6f,0x5c,0x3a,0x9b,0x9a,0x80,0x4a,0xb2,0x8e,0xe0,0xc4,0x7a,0x6d);
const ble_uuid128_t request_uuid=BLE_UUID128_INIT(0x02,0x00,0x42,0x6f,0x5c,0x3a,0x9b,0x9a,0x80,0x4a,0xb2,0x8e,0xe0,0xc4,0x7a,0x6d);
const ble_uuid128_t response_uuid=BLE_UUID128_INIT(0x03,0x00,0x42,0x6f,0x5c,0x3a,0x9b,0x9a,0x80,0x4a,0xb2,0x8e,0xe0,0xc4,0x7a,0x6d);
const ble_uuid128_t live_uuid=BLE_UUID128_INIT(0x04,0x00,0x42,0x6f,0x5c,0x3a,0x9b,0x9a,0x80,0x4a,0xb2,0x8e,0xe0,0xc4,0x7a,0x6d);
std::atomic<bool> enabled{false},secured{false},subscribed{false};
std::atomic<bool> live_subscribed{false};
std::atomic<uint32_t> live_sent{0},live_dropped{0};
std::atomic<int> last_disconnect_reason{0},last_send_error{0};
std::atomic<uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint32_t> generation{0},request_count{0},error_count{0},passkey{0};
std::atomic<uint64_t> pairing_until{0},passkey_until{0},connected_at{0};
uint8_t address_type=0;
uint16_t response_handle=0;
uint16_t live_handle=0,live_sequence=0;
ble_npl_callout live_timer{};
portMUX_TYPE live_mutex=portMUX_INITIALIZER_UNLOCKED;
std::array<uint8_t,20> live_cache{};
uint64_t live_cached_at=0;
char ble_name[30]{};
struct Request {uint32_t generation;uint16_t connection;char text[1537];};
QueueHandle_t requests=nullptr;
SemaphoreHandle_t indicated=nullptr;
std::atomic<int> indication_status{0};
// Only the NimBLE host accesses this assembly buffer. Queue sends copy it.
Request receiving_request{};
size_t received=0;
uint64_t received_at=0;

void advertise();
bool authenticated(uint16_t handle) {
    ble_gap_conn_desc description{};
    return ble_gap_conn_find(handle,&description)==0 && description.sec_state.encrypted && description.sec_state.authenticated;
}
std::array<uint8_t,20> current_live() {
    // The Bluetooth host only copies a fixed-size mailbox. It never waits for
    // application mutexes or allocates Runtime strings during radio callbacks.
    std::array<uint8_t,20> bytes;uint64_t cached_at;
    portENTER_CRITICAL(&live_mutex);bytes=live_cache;cached_at=live_cached_at;portEXIT_CRITICAL(&live_mutex);
    uint64_t elapsed=monotonic_ms()-cached_at;
    uint64_t age=static_cast<uint64_t>(get16(bytes.data()+16))+elapsed;
    if(!cached_at || elapsed>2500) bytes[1]&=0xc0;
    if(age>1500) bytes[1]&=~7U;
    if(age>5000) bytes[1]&=~32U;
    put16(bytes.data()+16,std::min<uint64_t>(UINT16_MAX,age));
    put16(bytes.data()+18,++live_sequence);return bytes;
}
int access_live(uint16_t handle,uint16_t,ble_gatt_access_ctxt* context,void*) {
    if(!authenticated(handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if(context->op!=BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    auto bytes=current_live();return os_mbuf_append(context->om,bytes.data(),bytes.size())?BLE_ATT_ERR_INSUFFICIENT_RES:0;
}
void push_live(ble_npl_event*) {
    // The host timer is independent of the JSON/history worker and TLS memory gate.
    // Notifications are expendable: a busy radio drops a frame instead of queuing stale data.
    uint16_t handle=connection;
    if(handle!=BLE_HS_CONN_HANDLE_NONE && secured && live_subscribed && authenticated(handle)) {
        auto bytes=current_live();auto* buffer=ble_hs_mbuf_from_flat(bytes.data(),bytes.size());
        if(buffer && ble_gatts_notify_custom(handle,live_handle,buffer)==0) ++live_sent;
        else ++live_dropped;
    }
    ble_npl_callout_reset(&live_timer,ble_npl_time_ms_to_ticks32(1000));
}
int access(uint16_t handle,uint16_t,ble_gatt_access_ctxt* context,void*) {
    if(!authenticated(handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if(context->op==BLE_GATT_ACCESS_OP_READ_CHR) {
        const char* ready="AirCue BLE/1";
        return os_mbuf_append(context->om,ready,strlen(ready))?BLE_ATT_ERR_INSUFFICIENT_RES:0;
    }
    if(context->op!=BLE_GATT_ACCESS_OP_WRITE_CHR || !subscribed) return BLE_ATT_ERR_UNLIKELY;
    uint16_t size=OS_MBUF_PKTLEN(context->om);
    if(!size || size>512) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    char bytes[512];
    if(os_mbuf_copydata(context->om,0,size,bytes)) return BLE_ATT_ERR_UNLIKELY;
    if(monotonic_ms()-received_at>5000) received=0;
    received_at=monotonic_ms();
    for(unsigned i=0;i<size;++i) {
        if(bytes[i]=='\n') {
            if(!received) continue;
            receiving_request.generation=generation;receiving_request.connection=handle;
            receiving_request.text[received]='\0';received=0;
            if(xQueueSend(requests,&receiving_request,0)!=pdTRUE) {++error_count;return BLE_ATT_ERR_INSUFFICIENT_RES;}
        } else if(bytes[i]=='\0' || received>=sizeof(receiving_request.text)-1) {
            received=0;++error_count;return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        } else receiving_request.text[received++]=bytes[i];
    }
    return 0;
}
int event(ble_gap_event* event,void*) {
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status) {advertise();break;}
        connection=event->connect.conn_handle;++generation;secured=false;subscribed=false;live_subscribed=false;received=0;
        connected_at=monotonic_ms();
        printf("AIRCUE_BLE connected handle=%u\n",event->connect.conn_handle);
        // Accessing the protected read characteristic triggers the phone's pairing UI.
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        last_disconnect_reason=event->disconnect.reason;
        printf("AIRCUE_BLE disconnected reason=%d\n",event->disconnect.reason);
        connection=BLE_HS_CONN_HANDLE_NONE;++generation;secured=false;subscribed=false;live_subscribed=false;received=0;passkey_until=0;
        indication_status=BLE_HS_ENOTCONN;xSemaphoreGive(indicated);advertise();break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        secured=event->enc_change.status==0 && authenticated(event->enc_change.conn_handle);
        // Sending Service Changed here invalidates handles that the browser
        // just discovered, even though this connection's database is unchanged.
        printf("AIRCUE_BLE security status=%d authenticated=%d\n",event->enc_change.status,static_cast<int>(secured.load()));
        passkey_until=0;break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if(event->subscribe.attr_handle==response_handle) subscribed=event->subscribe.cur_indicate;
        if(event->subscribe.attr_handle==live_handle) live_subscribed=event->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if(event->notify_tx.indication && event->notify_tx.attr_handle==response_handle && event->notify_tx.status!=0) {
            indication_status=event->notify_tx.status;xSemaphoreGive(indicated);
        }
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if(event->passkey.params.action!=BLE_SM_IOACT_DISP || monotonic_ms()>=pairing_until) {
            ble_gap_terminate(event->passkey.conn_handle,BLE_ERR_REM_USER_CONN_TERM);break;
        }
        ble_sm_io input{};input.action=BLE_SM_IOACT_DISP;input.passkey=esp_random()%1000000;
        passkey=input.passkey;passkey_until=monotonic_ms()+60000;
        ble_sm_inject_io(event->passkey.conn_handle,&input);break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        if(monotonic_ms()>=pairing_until) return BLE_GAP_REPEAT_PAIRING_IGNORE;
        ble_gap_conn_desc description{};
        if(ble_gap_conn_find(event->repeat_pairing.conn_handle,&description)!=0) return BLE_GAP_REPEAT_PAIRING_IGNORE;
        ble_store_util_delete_peer(&description.peer_id_addr);return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:advertise();break;
    default:break;
    }
    return 0;
}
void advertise() {
    if(connection!=BLE_HS_CONN_HANDLE_NONE || !ble_hs_synced()) return;
    ble_hs_adv_fields fields{};fields.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128=const_cast<ble_uuid128_t*>(&service_uuid);fields.num_uuids128=1;fields.uuids128_is_complete=1;
    ble_hs_adv_fields scan{};scan.name=reinterpret_cast<uint8_t*>(ble_name);scan.name_len=strlen(ble_name);scan.name_is_complete=1;
    if(ble_gap_adv_set_fields(&fields) || ble_gap_adv_rsp_set_fields(&scan)) {++error_count;return;}
    ble_gap_adv_params params{};params.conn_mode=BLE_GAP_CONN_MODE_UND;params.disc_mode=BLE_GAP_DISC_MODE_GEN;
    params.itvl_min=320;params.itvl_max=480;
    if(ble_gap_adv_start(address_type,nullptr,BLE_HS_FOREVER,&params,event,nullptr)) ++error_count;
}
void on_sync() {if(ble_hs_id_infer_auto(0,&address_type)==0) advertise();ble_npl_callout_reset(&live_timer,ble_npl_time_ms_to_ticks32(1000));}
cJSON* failure(const char* message,int status=422) {
    auto* value=cJSON_CreateObject();cJSON_AddStringToObject(value,"message",message);cJSON_AddNumberToObject(value,"http_status",status);return value;
}
cJSON* execute(const cJSON* request) {
    auto op=string_field(request,"op");auto* body=cJSON_GetObjectItemCaseSensitive(request,"body");
    if(op=="status") return status_json();
    if(op=="gaps") {auto* out=cJSON_CreateObject();cJSON_AddItemToObject(out,"gaps",storage.gaps());return out;}
    if(op=="history") {
        auto cursor=string_field(body,"cursor","0");
        if(cursor.empty()) cursor="0";
        if(cursor.size()>20 || cursor.find_first_not_of("0123456789")!=std::string::npos) return failure("Invalid history cursor");
        uint64_t after=0;
        for(char digit:cursor) {if(after>(UINT64_MAX-(digit-'0'))/10) return failure("Invalid history cursor");after=after*10+(digit-'0');}
        // Small pages bound RAM and BLE latency independently of the retained journal size.
        bool compact=string_field(body,"format")=="wire";size_t limit=compact?8:4;
        auto page=storage.page(after,limit+1);bool more=page.size()>limit;
        if(more) page.resize(limit);
        auto* out=cJSON_CreateObject();auto* rows=cJSON_AddArrayToObject(out,"records");
        auto* snapshots=cJSON_AddArrayToObject(out,"config_snapshots");std::vector<uint32_t> revisions;
        for(const auto& row:page) {
            // Send compact CRC-protected records; decoding belongs on the phone.
            cJSON_AddItemToArray(rows,stored_json(row,compact));after=row.order;
            uint32_t revision=row.record.type==1?read_minute(row.record).config_revision:get32(row.record.payload.data()+40);
            if(compact && std::find(revisions.begin(),revisions.end(),revision)==revisions.end()) {
                revisions.push_back(revision);auto* value=storage.config_snapshot(revision);
                if(cJSON_IsObject(value)) cJSON_AddItemToArray(snapshots,value);else cJSON_Delete(value);
            }
        }
        cJSON_AddStringToObject(out,"next_cursor",std::to_string(after).c_str());cJSON_AddBoolToObject(out,"has_more",more);return out;
    }
    if(op=="config" || op=="wifi" || op=="uplink" || op=="mute" || op=="commission" || op=="run_start" || op=="run_stop" || op=="ota") return config.apply(body,op,false);
    return failure("Unknown operation");
}
bool send(const Request& request,std::string_view text) {
    uint64_t deadline=monotonic_ms()+45000;
    for(size_t offset=0;offset<=text.size();) {
        esp_task_wdt_reset();
        if(generation!=request.generation || connection!=request.connection || !secured || !subscribed || monotonic_ms()>=deadline) return false;
        char fragment[182];
        size_t capacity=std::min<size_t>(sizeof(fragment),std::max<int>(20,ble_att_mtu(request.connection)-3));
        size_t count=ble_json_fragment(text,offset,fragment,capacity);
        auto* buffer=ble_hs_mbuf_from_flat(fragment,count);
        if(!buffer) {last_send_error=BLE_HS_ENOMEM;return false;}
        while(xSemaphoreTake(indicated,0)==pdTRUE) {}
        indication_status=0;
        int result=ble_gatts_indicate_custom(request.connection,response_handle,buffer);
        if(result!=0) {last_send_error=result;return false;}
        if(xSemaphoreTake(indicated,pdMS_TO_TICKS(5000))!=pdTRUE) {last_send_error=BLE_HS_ETIMEOUT;return false;}
        if(indication_status!=BLE_HS_EDONE) {last_send_error=indication_status.load();return false;}
        offset+=count;
    }
    return true;
}
void worker(void*) {
    esp_task_wdt_add(nullptr);Request request{};
    while(true) {
        esp_task_wdt_reset();
        if(connection!=BLE_HS_CONN_HANDLE_NONE && !secured && monotonic_ms()>connected_at+90000)
            ble_gap_terminate(connection,BLE_ERR_REM_USER_CONN_TERM);
        if(xQueueReceive(requests,&request,pdMS_TO_TICKS(500))!=pdTRUE) continue;
        if(request.generation!=generation || !secured || !authenticated(request.connection)) continue;
        // Share transient JSON/TLS memory without pausing sensor or output tasks.
        UplinkMemory memory;
        Json parsed(cJSON_Parse(request.text));auto id=string_field(parsed.p,"id");
        if(id.empty() || id.size()>64 || id.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_")!=std::string::npos) {++error_count;continue;}
        Json response(cJSON_CreateObject());cJSON_AddStringToObject(response.p,"id",id.c_str());
        cJSON_AddItemToObject(response.p,"result",execute(parsed.p));++request_count;
        cJSON_Delete(parsed.release());auto text=json_take(response.release());
        char fallback[200];auto encoded=text.view();
        if(text.empty()) {
            snprintf(fallback,sizeof(fallback),"{\"id\":\"%s\",\"result\":{\"http_status\":503,\"message\":\"Device memory is busy; retry shortly\"}}",id.c_str());
            encoded=fallback;
        }
        if(!send(request,encoded)) {
            ++error_count;printf("AIRCUE_BLE response_failed error=%d bytes=%u\n",last_send_error.load(),static_cast<unsigned>(encoded.size()));
            if(generation==request.generation) ble_gap_terminate(request.connection,BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}
}
void bluetooth_update_live(const Climate& climate,const View& view,uint64_t now,uint64_t age) {
    auto bytes=live_frame(climate,view,now,age,0);
    portENTER_CRITICAL(&live_mutex);live_cache=bytes;live_cached_at=now;portEXIT_CRITICAL(&live_mutex);
}
bool bluetooth_start() {
    if(nimble_port_init()!=ESP_OK) return false;
    requests=xQueueCreate(1,sizeof(Request));indicated=xSemaphoreCreateBinary();
    if(!requests || !indicated) return false;
    ble_hs_cfg.sync_cb=on_sync;ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap=BLE_SM_IO_CAP_DISP_ONLY;ble_hs_cfg.sm_bonding=1;ble_hs_cfg.sm_mitm=1;ble_hs_cfg.sm_sc=1;
    ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();ble_svc_gatt_init();ble_store_config_init();
    snprintf(ble_name,sizeof(ble_name),"AirCue-BLE-%s",AIRCUE_DEVICE_ID+strlen(AIRCUE_DEVICE_ID)-6);
    ble_svc_gap_device_name_set(ble_name);
    static ble_gatt_chr_def characteristics[4]{};
    characteristics[0].uuid=&request_uuid.u;characteristics[0].access_cb=access;
    characteristics[0].flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_ENC|BLE_GATT_CHR_F_WRITE_AUTHEN;
    characteristics[0].min_key_size=16;
    characteristics[1].uuid=&response_uuid.u;characteristics[1].access_cb=access;
    characteristics[1].flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_READ_ENC|BLE_GATT_CHR_F_READ_AUTHEN|BLE_GATT_CHR_F_INDICATE;
    characteristics[1].min_key_size=16;characteristics[1].val_handle=&response_handle;
    characteristics[2].uuid=&live_uuid.u;characteristics[2].access_cb=access_live;
    characteristics[2].flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_READ_ENC|BLE_GATT_CHR_F_READ_AUTHEN|BLE_GATT_CHR_F_NOTIFY;
    characteristics[2].min_key_size=16;characteristics[2].val_handle=&live_handle;
    static ble_gatt_svc_def services[2]{};
    services[0].type=BLE_GATT_SVC_TYPE_PRIMARY;services[0].uuid=&service_uuid.u;services[0].characteristics=characteristics;
    if(ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) return false;
    if(xTaskCreate(worker,"ble_requests",7168,nullptr,2,nullptr)!=pdPASS) return false;
    ble_npl_callout_init(&live_timer,nimble_port_get_dflt_eventq(),push_live,nullptr);
    enabled=true;
    nimble_port_freertos_init([](void*){nimble_port_run();nimble_port_freertos_deinit();});
    return true;
}
void bluetooth_pairing_window() {pairing_until=monotonic_ms()+120000;}
bool bluetooth_pairing_code(uint32_t& code) {code=passkey;return monotonic_ms()<passkey_until;}
cJSON* bluetooth_json() {
    auto* out=cJSON_CreateObject();uint64_t now=monotonic_ms(),until=pairing_until;
    cJSON_AddBoolToObject(out,"enabled",enabled);cJSON_AddBoolToObject(out,"connected",connection!=BLE_HS_CONN_HANDLE_NONE);
    cJSON_AddBoolToObject(out,"secured",secured);cJSON_AddStringToObject(out,"name",ble_name);
    cJSON_AddNumberToObject(out,"pairing_seconds",until>now?(until-now)/1000:0);
    cJSON_AddNumberToObject(out,"requests",request_count);cJSON_AddNumberToObject(out,"errors",error_count);
    cJSON_AddNumberToObject(out,"last_disconnect_reason",last_disconnect_reason);cJSON_AddNumberToObject(out,"last_send_error",last_send_error);
    cJSON_AddBoolToObject(out,"live_subscribed",live_subscribed);
    cJSON_AddNumberToObject(out,"live_interval_ms",1000);cJSON_AddNumberToObject(out,"live_sent",live_sent);cJSON_AddNumberToObject(out,"live_dropped",live_dropped);
    auto host=xTaskGetHandle("nimble_host");if(host) cJSON_AddNumberToObject(out,"host_stack_minimum",uxTaskGetStackHighWaterMark(host));return out;
}
}
