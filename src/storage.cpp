#include "aircue/runtime.hpp"
#include "aircue/retention.hpp"
#include "secrets.hpp"
#include "esp_littlefs.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <set>
#include <cerrno>

namespace aircue {
StorageService storage;
static bool durable(FILE* f) { return fflush(f)==0 && fsync(fileno(f))==0; }
static std::string load_nvs(const char* key, const char* fallback="[]") {
    nvs_handle_t n; size_t len=0;
    if(nvs_open("journal",NVS_READONLY,&n)!=ESP_OK) return fallback;
    std::string value;
    if(nvs_get_str(n,key,nullptr,&len)==ESP_OK && len<16000) { value.resize(len); if(nvs_get_str(n,key,value.data(),&len)!=ESP_OK) value.clear(); }
    nvs_close(n); if(!value.empty()) value.resize(value.size()-1); return value.empty()?fallback:value;
}
static bool save_nvs(const char* key, const std::string& value) {
    nvs_handle_t n; if(nvs_open("journal",NVS_READWRITE,&n)!=ESP_OK) return false;
    auto err=nvs_set_str(n,key,value.c_str()); if(err==ESP_OK) err=nvs_commit(n); nvs_close(n); return err==ESP_OK;
}
static cJSON* journal_metadata() {
    return cJSON_Parse(load_nvs("state","{\"gaps\":[],\"lost\":0,\"retired\":[]}").c_str());
}
bool StorageService::record_gap(const Segment& seg,const char* reason) {
    Json metadata(journal_metadata());
    auto* manifest=cJSON_GetObjectItemCaseSensitive(metadata.p,"gaps");
    if(!cJSON_IsArray(manifest)) return false;
    // Keep logging indefinitely when both receivers are offline. Older gap details
    // are summarized, while the cumulative loss counter remains monotonic.
    if(cJSON_GetArraySize(manifest)>=48) {
        cJSON_DeleteItemFromArray(manifest,0);
        double summarized=number_field(metadata.p,"summarized_gap_reports")+1;
        cJSON_DeleteItemFromObjectCaseSensitive(metadata.p,"summarized_gap_reports");
        cJSON_AddNumberToObject(metadata.p,"summarized_gap_reports",summarized);
    }
    auto* gap=cJSON_CreateObject();
    const std::string id=std::to_string(seg.ordinal)+"-"+std::to_string(seg.generation)+"-"+std::to_string(seg.first)+"-"+std::to_string(seg.last)+"-"+reason;
    cJSON* existing;
    cJSON_ArrayForEach(existing,manifest) if(string_field(existing,"gap_id")==id) {cJSON_Delete(gap);return true;}
    cJSON_AddStringToObject(gap,"gap_id",id.c_str()); cJSON_AddStringToObject(gap,"reason",reason);
    cJSON_AddStringToObject(gap,"first_order",std::to_string(seg.first).c_str()); cJSON_AddStringToObject(gap,"last_order",std::to_string(seg.last).c_str());
    uint32_t affected=seg.count;
    auto* ranges=cJSON_AddArrayToObject(gap,"record_ranges");
    const bool retiring=strcmp(reason,"CAPACITY_UNACKNOWLEDGED")==0 || strcmp(reason,"RETENTION_UNACKNOWLEDGED")==0;
    uint64_t acknowledged=std::min(ack.load(),seg.type==1?remote_minute_ack.load():remote_event_ack.load());
    if(retiring) {
        FILE* source=fopen(seg.path.c_str(),"rb"); if(!source) {cJSON_Delete(gap);return false;}
        fseek(source,128,SEEK_SET); affected=0; cJSON* range=nullptr; RecordId previous;
        for(uint32_t i=0;i<seg.count;++i) {
            uint8_t bytes[climate_record_size];Record record;
            if(fread(bytes,1,seg.record_bytes,source)!=seg.record_bytes||!decode(bytes,seg.record_bytes,record)){fclose(source);cJSON_Delete(gap);return false;}
            if(((uint64_t(seg.ordinal)<<32)|record.id.sequence)<=acknowledged) continue;
            ++affected;
            if(range && previous.boot==record.id.boot && previous.sequence+1==record.id.sequence) cJSON_ReplaceItemInObject(range,"last",cJSON_CreateNumber(record.id.sequence));
            else {range=cJSON_CreateObject();cJSON_AddStringToObject(range,"boot_id",hex(record.id.boot.data(),16).c_str());cJSON_AddNumberToObject(range,"first",record.id.sequence);cJSON_AddNumberToObject(range,"last",record.id.sequence);cJSON_AddItemToArray(ranges,range);}
            previous=record.id;
        }
        fclose(source);
    } else if(strcmp(reason,"QUEUE_OVERFLOW")==0) {
        auto* range=cJSON_CreateObject();cJSON_AddStringToObject(range,"boot_id",hex(boot_.data(),16).c_str());
        cJSON_AddNumberToObject(range,"first",static_cast<uint32_t>(seg.first));cJSON_AddNumberToObject(range,"last",static_cast<uint32_t>(seg.last));cJSON_AddItemToArray(ranges,range);
    }
    cJSON_AddNumberToObject(gap,"count",affected); cJSON_AddStringToObject(gap,"segment",seg.path.c_str());
    cJSON_AddItemToArray(manifest,gap);
    cJSON_ReplaceItemInObject(metadata.p,"lost",cJSON_CreateNumber(number_field(metadata.p,"lost")+affected));
    if(retiring) cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(metadata.p,"retired"),cJSON_CreateString(seg.path.c_str()));
    auto encoded=json_text(metadata.p);
    while(encoded.size()>10000 && cJSON_GetArraySize(manifest)>1) {
        cJSON_DeleteItemFromArray(manifest,0);
        double summarized=number_field(metadata.p,"summarized_gap_reports")+1;
        cJSON_DeleteItemFromObjectCaseSensitive(metadata.p,"summarized_gap_reports");
        cJSON_AddNumberToObject(metadata.p,"summarized_gap_reports",summarized);
        encoded=json_text(metadata.p);
    }
    // Persist the loss counter and deletion intent in the same NVS transaction.
    return encoded.size()<=10000 && save_nvs("state",encoded);
}
bool StorageService::begin(const std::array<uint8_t,16>& boot,uint32_t ordinal) {
    boot_=boot; ordinal_=ordinal;
    esp_vfs_littlefs_conf_t conf{}; conf.base_path="/journal"; conf.partition_label="journal"; conf.format_if_mount_failed=false;
    if(esp_vfs_littlefs_register(&conf)!=ESP_OK) return false;
    Json metadata(journal_metadata());
    auto* retired=cJSON_GetObjectItemCaseSensitive(metadata.p,"retired");
    if(!cJSON_IsArray(retired)) return false;
    if(cJSON_GetArraySize(retired)) {
        cJSON* path;
        cJSON_ArrayForEach(path,retired) {
            if(!cJSON_IsString(path) || strncmp(path->valuestring,"/journal/",9)!=0) return false;
            if(unlink(path->valuestring)!=0 && errno!=ENOENT) return false;
        }
        cJSON_ReplaceItemInObject(metadata.p,"retired",cJSON_CreateArray());
        if(!save_nvs("state",json_text(metadata.p))) return false;
    }
    ack=strtoull(load_nvs("ack","0").c_str(),nullptr,10);
    DIR* directory=opendir("/journal"); if(!directory) return false;
    bool ok=true; dirent* ent;
    while((ent=readdir(directory))) {
        std::string file=ent->d_name;
        if(file.size()!=13 || (file[0]!='m' && file[0]!='e') || file.substr(9)!=".bin") continue;
        Segment seg; seg.path="/journal/"+file;
        FILE* f=fopen(seg.path.c_str(),"r+b"); if(!f) { ok=false; continue; }
        uint8_t header[128]{};
        if(fread(header,1,128,f)!=128 || (memcmp(header,"AQCSEG01",8) && memcmp(header,"AQCSEG02",8)) || get32(header+124)!=crc32(header,124) || std::string(reinterpret_cast<char*>(header+20),strnlen(reinterpret_cast<char*>(header+20),40))!=AIRCUE_DEVICE_ID) {
            fclose(f); ok=false; continue;
        }
        seg.generation=get32(header+8); seg.ordinal=get32(header+12); seg.type=header[16];
        seg.record_bytes=memcmp(header,"AQCSEG02",8)==0?get16(header+18):record_size;
        if(seg.record_bytes!=record_size && seg.record_bytes!=climate_record_size) {fclose(f);ok=false;continue;}
        seg.run_id=std::string(reinterpret_cast<char*>(header+64),strnlen(reinterpret_cast<char*>(header+64),40)); seg.run_t0=get64(header+104);
        generation_=std::max(generation_,seg.generation);
        uint8_t bytes[climate_record_size]; size_t read;
        while((read=fread(bytes,1,seg.record_bytes,f))!=0) {
            Record r;
            if(read!=seg.record_bytes || !decode(bytes,read,r)) {
                int extra=fgetc(f);
                if(extra!=EOF) { ok=false; break; }
                Segment tail=seg; tail.count=1;
                if(!record_gap(tail,"TORN_TAIL_UNKNOWN_ID") || ftruncate(fileno(f),128+seg.count*seg.record_bytes)!=0 || fsync(fileno(f))!=0) ok=false;
                break;
            }
            uint64_t order=(uint64_t(seg.ordinal)<<32)|r.id.sequence;
            if(!seg.count) seg.first=order;
            if(r.type!=seg.type || (seg.count && order<=seg.last)) { ok=false; break; }
            seg.last=order; ++seg.count; last_committed_=std::max(last_committed_,order);
            seg.last_utc=std::max(seg.last_utc,r.utc_ms);seg.unknown_utc=seg.unknown_utc||!r.utc_ms;
        }
        fclose(f); segments_.push_back(seg);
    }
    closedir(directory);
    std::sort(segments_.begin(),segments_.end(),[](const Segment& a,const Segment& b){return a.generation<b.generation;});
    healthy=ok; return ok;
}
RecordId StorageService::enqueue(Record r,const Config& cfg,const Episode& ep,Level level) {
    Lock lock(queue_mutex_);
    if(sequence_==UINT32_MAX) { ++dropped_queue; healthy=false; return {}; }
    r.id.boot=boot_; r.id.sequence=++sequence_;
    r.monotonic_ms=r.monotonic_ms?r.monotonic_ms:monotonic_ms();
    if(!r.utc_ms) r.flags|=UtcUnknown;
    if(cfg.demo) r.flags|=Demo;
    StorageWork work{}; work.record=r; work.episode=ep; work.level=level; work.revision=cfg.rules_revision;
    work.config_revision=cfg.revision; work.rules=cfg.rules; work.demo=cfg.demo; work.uplink=cfg.uplink;
    if(r.type==2 && r.payload[0]==static_cast<uint8_t>(Event::EpisodeStart)) {
        work.episode.id=r.id;
        std::copy(r.id.boot.begin(),r.id.boot.end(),work.record.payload.begin()+4); put32(work.record.payload.data()+20,r.id.sequence);
    }
    strncpy(work.run_id,cfg.run_id.c_str(),40); work.run_t0=cfg.run_t0;
    ++pending_;
    if(xQueueSend(queue_,&work,0)!=pdTRUE) { --pending_; ++dropped_queue; return r.id; }
    return r.id;
}
bool StorageService::reclaim(uint8_t type) {
    while(true) {
    esp_task_wdt_reset();
    size_t count=0; for(auto& s:segments_) if(s.type==type) count+=s.count;
    uint64_t now=utc_ms();
    auto expired=[&](const Segment& s){return history_expired(now,s.last_utc,s.unknown_utc);};
    auto it=std::find_if(segments_.begin(),segments_.end(),[&](const Segment& s){return s.type==type && (count>=(type==1?retained_minutes:retained_events) || expired(s));});
    if(it==segments_.end()) return true;
    if(!save_nvs("ack",std::to_string(ack.load()))) return false;
    last_ack_save_=monotonic_ms();
    uint64_t acknowledged=std::min(ack.load(),type==1?remote_minute_ack.load():remote_event_ack.load());
    if(it->last>acknowledged && !record_gap(*it,expired(*it)?"RETENTION_UNACKNOWLEDGED":"CAPACITY_UNACKNOWLEDGED")) return false;
    if(unlink(it->path.c_str())!=0) return false;
    Json metadata(journal_metadata());
    auto* retired=cJSON_GetObjectItemCaseSensitive(metadata.p,"retired");
    for(int i=cJSON_GetArraySize(retired)-1;i>=0;--i) {
        auto* path=cJSON_GetArrayItem(retired,i);
        if(cJSON_IsString(path)&&it->path==path->valuestring) cJSON_DeleteItemFromArray(retired,i);
    }
    if(!save_nvs("state",json_text(metadata.p))) return false;
    segments_.erase(it);
    }
}
bool StorageService::checkpoint(const StorageWork& w) {
    uint8_t b[160]{}; memcpy(b,"AQCEP001",8);
    put64(b+8,(uint64_t(ordinal_)<<32)|w.record.id.sequence); put32(b+16,w.revision); b[20]=static_cast<uint8_t>(w.level);
    const auto& e=w.episode; b[21]=e.active; b[22]=e.interrupted;
    std::copy(e.id.boot.begin(),e.id.boot.end(),b+24); put32(b+40,e.id.sequence); put32(b+44,e.peak);
    put64(b+48,e.candidate_ms); put64(b+56,e.confirmed_ms); put64(b+64,e.falling_ms); put64(b+72,e.ended_ms);
    put64(b+80,e.valid_ms); put64(b+88,e.high_ms); put64(b+96,e.red_ms); put64(b+104,e.missing_ms);
    put32(b+112,++checkpoint_generation_); put32(b+156,crc32(b,156));
    // A prepared checkpoint becomes valid only when its exact journal record exists.
    const char* path=checkpoint_generation_%2?"/journal/episode_a":"/journal/episode_b";
    FILE* f=fopen(path,"wb"); if(!f) return false;
    bool ok=fwrite(b,1,sizeof(b),f)==sizeof(b) && durable(f); if(fclose(f)!=0) ok=false; return ok;
}
bool StorageService::append(const StorageWork& w) {
    if(w.record.id.sequence>last_written_sequence_+1) {
        Segment gap;gap.generation=generation_;gap.path="ram-queue";gap.ordinal=ordinal_;
        gap.first=(uint64_t(ordinal_)<<32)|(last_written_sequence_+1);gap.last=(uint64_t(ordinal_)<<32)|(w.record.id.sequence-1);gap.count=w.record.id.sequence-last_written_sequence_-1;
        if(!record_gap(gap,"QUEUE_OVERFLOW")) return false;
    }
    if(!save_config_snapshot(w)) return false;
    if(!reclaim(w.record.type)) return false;
    Segment* target=nullptr;
    for(auto it=segments_.rbegin();it!=segments_.rend();++it) if(it->type==w.record.type) {
        if(it->ordinal==ordinal_ && it->count<64 && it->run_id==w.run_id && it->record_bytes==encoded_size(w.record)) target=&*it;
        break;
    }
    if(!target) {
        if(!save_nvs("ack",std::to_string(ack.load()))) return false;
        last_ack_save_=monotonic_ms();
        Segment seg; seg.type=w.record.type; seg.ordinal=ordinal_; seg.generation=++generation_; seg.run_id=w.run_id; seg.run_t0=w.run_t0;seg.record_bytes=encoded_size(w.record);
        char path[40]; snprintf(path,sizeof(path),"/journal/%c%08x.bin",seg.type==1?'m':'e',static_cast<unsigned>(seg.generation)); seg.path=path;
        uint8_t header[128]{}; memcpy(header,w.record.version==2?"AQCSEG02":"AQCSEG01",8); put32(header+8,seg.generation); put32(header+12,ordinal_); header[16]=seg.type;
        header[17]=w.record.version;put16(header+18,seg.record_bytes);
        strncpy(reinterpret_cast<char*>(header+20),AIRCUE_DEVICE_ID,40); strncpy(reinterpret_cast<char*>(header+64),seg.run_id.c_str(),40); put64(header+104,seg.run_t0);
        put32(header+124,crc32(header,124)); FILE* f=fopen(seg.path.c_str(),"wb"); if(!f) return false;
        bool ok=fwrite(header,1,128,f)==128 && durable(f); if(fclose(f)!=0) ok=false; if(!ok) return false;
        segments_.push_back(seg); target=&segments_.back();
    }
    bool episode_record=w.record.type==1 || (w.record.type==2 && w.record.payload[0]>=2 && w.record.payload[0]<=5);
    if(episode_record && !checkpoint(w)) return false;
    FILE* f=fopen(target->path.c_str(),"ab"); if(!f) return false;
    auto bytes=encode(w.record); bool ok=fwrite(bytes.data(),1,bytes.size(),f)==bytes.size() && durable(f);
    if(fclose(f)!=0) ok=false;
    if(!ok) return false;
    uint64_t order=(uint64_t(ordinal_)<<32)|w.record.id.sequence;
    if(!target->count) target->first=order;
    target->last=order; ++target->count; last_committed_=order;
    target->last_utc=std::max(target->last_utc,w.record.utc_ms);target->unknown_utc=target->unknown_utc||!w.record.utc_ms;
    last_written_sequence_=w.record.id.sequence;
    return true;
}
void StorageService::run() {
    esp_task_wdt_add(nullptr); StorageWork work;uint64_t maintenance=0;
    while(true) {
        esp_task_wdt_reset();
        if(xQueueReceive(queue_,&work,pdMS_TO_TICKS(500))==pdTRUE) {
            Lock lock(mutex_);
            // Stop appending after an ambiguous write; recovery must inspect its tail first.
            if(!healthy || !append(work)) { ++write_errors; healthy=false; }
            --pending_;
        }
        if(monotonic_ms()>=maintenance) {
            maintenance=monotonic_ms()+60000;Lock lock(mutex_);
            if(healthy && (!reclaim(1)||!reclaim(2))) {++write_errors;healthy=false;}
        }
    }
}
std::vector<StoredRecord> StorageService::page(uint64_t cursor,size_t limit,uint8_t type) {
    Lock lock(mutex_); limit=std::min<size_t>(limit,256); std::vector<StoredRecord> out;
    for(const auto& seg:segments_) {
        if(type && seg.type!=type) continue;
        if(seg.last<=cursor || !seg.count) continue;
        if(out.size()==limit && seg.first>out.back().order) continue;
        FILE* f=fopen(seg.path.c_str(),"rb");
        if(!f) {healthy=false;++write_errors;return {};}
        fseek(f,128,SEEK_SET);
        for(uint32_t i=0;i<seg.count;++i) {
            uint8_t b[climate_record_size]; Record r;
            if(fread(b,1,seg.record_bytes,f)!=seg.record_bytes || !decode(b,seg.record_bytes,r)) {fclose(f);healthy=false;++write_errors;return {};}
            uint64_t order=(uint64_t(seg.ordinal)<<32)|r.id.sequence; if(order<=cursor) continue;
            StoredRecord item{r,order,seg.run_t0,seg.run_id};
            auto at=std::lower_bound(out.begin(),out.end(),order,[](const StoredRecord& a,uint64_t b){return a.order<b;});
            if(out.size()<limit || at!=out.end()) { out.insert(at,std::move(item)); if(out.size()>limit) out.pop_back(); }
        }
        fclose(f);
    }
    return out;
}
bool StorageService::acknowledge(uint64_t order) {
    Lock lock(mutex_); if(order<ack || order>last_committed_) return false;
    ack=order;
    if(monotonic_ms()-last_ack_save_>=300000) { if(!save_nvs("ack",std::to_string(order))) return false; last_ack_save_=monotonic_ms(); }
    return true;
}
bool StorageService::flush(uint32_t timeout_ms) {
    uint64_t deadline=monotonic_ms()+timeout_ms;
    while(pending_ && monotonic_ms()<deadline) vTaskDelay(pdMS_TO_TICKS(10));
    Lock lock(mutex_); return !pending_ && healthy && save_nvs("ack",std::to_string(ack.load()));
}
cJSON* StorageService::gaps() { Lock lock(mutex_); Json metadata(journal_metadata()); return cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(metadata.p,"gaps"),true); }
bool StorageService::acknowledge_gaps(const cJSON* ids) {
    Lock lock(mutex_); Json metadata(journal_metadata()); auto* gaps=cJSON_GetObjectItemCaseSensitive(metadata.p,"gaps");
    if(!cJSON_IsArray(gaps) || !cJSON_IsArray(ids)) return false;
    for(int i=cJSON_GetArraySize(gaps)-1;i>=0;--i) {
        auto id=string_field(cJSON_GetArrayItem(gaps,i),"gap_id"); cJSON* x;
        cJSON_ArrayForEach(x,ids) if(cJSON_IsString(x)&&id==x->valuestring) { cJSON_DeleteItemFromArray(gaps,i); break; }
    }
    return save_nvs("state",json_text(metadata.p));
}
bool StorageService::restore(Episode& ep,Level& level,uint32_t& revision) {
    uint64_t best=0; bool found=false;
    for(auto path:{"/journal/episode_a","/journal/episode_b"}) {
        uint8_t b[160]; FILE* f=fopen(path,"rb"); if(!f) continue;
        bool ok=fread(b,1,sizeof(b),f)==sizeof(b); fclose(f);
        if(!ok || memcmp(b,"AQCEP001",8) || crc32(b,156)!=get32(b+156)) continue;
        checkpoint_generation_=std::max(checkpoint_generation_,get32(b+112));
        uint64_t order=get64(b+8); if(order<=best) continue;
        auto committed=page(order-1,1); if(committed.empty() || committed.front().order!=order) continue;
        best=order; found=true; revision=get32(b+16); level=static_cast<Level>(b[20]); ep.active=b[21]; ep.interrupted=b[22];
        std::copy(b+24,b+40,ep.id.boot.begin()); ep.id.sequence=get32(b+40); ep.peak=static_cast<int32_t>(get32(b+44));
        ep.candidate_ms=get64(b+48); ep.confirmed_ms=get64(b+56); ep.falling_ms=get64(b+64); ep.ended_ms=get64(b+72);
        ep.valid_ms=get64(b+80); ep.high_ms=get64(b+88); ep.red_ms=get64(b+96); ep.missing_ms=get64(b+104);
    }
    if(found && ep.active) ep.interrupted=true;
    return found;
}
cJSON* StorageService::diagnostics() {
    Lock lock(mutex_); auto* out=cJSON_CreateObject(); size_t total=0,used=0; esp_littlefs_info("journal",&total,&used);
    uint32_t records=0,backlog=0;
    for(auto& seg:segments_) { records+=seg.count; if(seg.last>ack) backlog+=seg.count; }
    cJSON_AddBoolToObject(out,"healthy",healthy); cJSON_AddNumberToObject(out,"used_bytes",used); cJSON_AddNumberToObject(out,"total_bytes",total);
    cJSON_AddNumberToObject(out,"retained_records",records); cJSON_AddNumberToObject(out,"backlog_upper_bound",backlog);
    cJSON_AddStringToObject(out,"ack_cursor",std::to_string(ack.load()).c_str()); cJSON_AddNumberToObject(out,"queue_drops",dropped_queue);
    cJSON_AddStringToObject(out,"remote_minute_ack",std::to_string(remote_minute_ack.load()).c_str());
    cJSON_AddStringToObject(out,"remote_event_ack",std::to_string(remote_event_ack.load()).c_str());
    cJSON_AddNumberToObject(out,"retention_hours",72);cJSON_AddNumberToObject(out,"minute_capacity",4384);
    cJSON_AddNumberToObject(out,"event_capacity",1024);
    Json metadata(journal_metadata());
    cJSON_AddNumberToObject(out,"summarized_gap_reports",number_field(metadata.p,"summarized_gap_reports"));
    cJSON_AddNumberToObject(out,"write_errors",write_errors); cJSON_AddNumberToObject(out,"discarded_records",number_field(metadata.p,"lost")); return out;
}
static cJSON* snapshot_json(const uint8_t* b) {
    auto* out=cJSON_CreateObject();
    const char* keys[]={"revision","rules_revision","watch_on","watch_off","high_on","high_off","watch_s","high_s","high_clear_s","clear_s","trend_ppm"};
    for(size_t i=0;i<11;++i) cJSON_AddNumberToObject(out,keys[i],get32(b+4+4*i));
    cJSON_AddBoolToObject(out,"demo",b[1]&1); cJSON_AddBoolToObject(out,"uplink_enabled",b[1]&2); return out;
}
cJSON* StorageService::config_snapshot(uint32_t revision) {
    Lock lock(mutex_); FILE* f=fopen("/journal/configs.bin","rb"); if(!f) return cJSON_CreateNull();
    uint8_t b[64]; cJSON* out=nullptr;
    while(fread(b,1,64,f)==64) if(b[0]==1 && get32(b+60)==crc32(b,60) && get32(b+4)==revision) {out=snapshot_json(b);break;}
    fclose(f); return out?out:cJSON_CreateNull();
}
bool StorageService::save_config_snapshot(const StorageWork& w) {
    FILE* f=fopen("/journal/configs.bin","r+b");
    if(!f) f=fopen("/journal/configs.bin","w+b");
    if(!f) return false;
    uint8_t b[64]; size_t count=0,read=0;
    while((read=fread(b,1,64,f))==64) {
        if(b[0]!=1 || get32(b+60)!=crc32(b,60)) {fclose(f);return false;}
        if(get32(b+4)==w.config_revision) {fclose(f);return true;}
        ++count;
    }
    if(read && (ftruncate(fileno(f),count*64)!=0 || fsync(fileno(f))!=0)) {fclose(f);return false;}
    if(count>=1024) {
        fclose(f);
        std::set<uint32_t> referenced;
        for(const auto& seg:segments_) {
            FILE* input=fopen(seg.path.c_str(),"rb"); if(!input) return false; fseek(input,128,SEEK_SET);
            for(uint32_t i=0;i<seg.count;++i) {uint8_t raw[climate_record_size];Record record;if(fread(raw,1,seg.record_bytes,input)!=seg.record_bytes||!decode(raw,seg.record_bytes,record)){fclose(input);return false;}referenced.insert(record.type==1?read_minute(record).config_revision:get32(record.payload.data()+40));}
            fclose(input);
        }
        FILE* previous=fopen("/journal/configs.bin","rb"); if(!previous) return false;
        f=fopen("/journal/configs.new","wb"); if(!f) {fclose(previous);return false;}
        count=0; bool ok=true;
        while(fread(b,1,64,previous)==64) if(referenced.count(get32(b+4))) {ok=ok&&fwrite(b,1,64,f)==64;++count;}
        fclose(previous);
        ok=ok&&durable(f); if(fclose(f)!=0) ok=false;
        if(!ok || count>=1024 || rename("/journal/configs.new","/journal/configs.bin")!=0) return false;
        f=fopen("/journal/configs.bin","ab"); if(!f) return false;
    } else fseek(f,count*64,SEEK_SET);
    memset(b,0,64); b[0]=1; b[1]=(w.demo?1:0)|(w.uplink?2:0);
    const uint32_t fields[]={w.config_revision,w.revision,static_cast<uint32_t>(w.rules.watch_on),static_cast<uint32_t>(w.rules.watch_off),static_cast<uint32_t>(w.rules.high_on),static_cast<uint32_t>(w.rules.high_off),w.rules.watch_s,w.rules.high_s,w.rules.high_clear_s,w.rules.clear_s,w.rules.trend_ppm};
    for(size_t i=0;i<11;++i) put32(b+4+4*i,fields[i]);
    put32(b+60,crc32(b,60));
    bool ok=fwrite(b,1,64,f)==64 && durable(f); if(fclose(f)!=0) ok=false; return ok;
}
}
