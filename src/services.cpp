#include "aircue/runtime.hpp"
#include "aircue/board.hpp"
#include "aircue/ble.hpp"
#include "aircue/sensor_sample.hpp"
#include "driver/gpio.h"
#include "aircue_i2c.h"
#include "driver/ledc.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
extern "C" {
#include "stcc4_i2c.h"
}

namespace aircue {
Runtime runtime;
SemaphoreHandle_t runtime_mutex=xSemaphoreCreateMutex();
std::array<uint8_t,16> boot_id{};
std::atomic<bool> ota_requested{false};
std::atomic<uint64_t> led_test_until{0};
std::atomic<unsigned> buzzer_test_request{0};
std::atomic<bool> sensor_initializing{true};
static std::atomic<bool> maintenance{true}, sensor_fault{false};
static std::atomic<uint64_t> warmup_until{UINT64_MAX};
struct Raw { uint64_t ms; int16_t ppm; uint16_t temperature,humidity,status; bool good; };
static QueueHandle_t sensor_samples=xQueueCreate(1,sizeof(Raw));
Runtime snapshot() { Lock lock(runtime_mutex); return runtime; }
static void record_event(Event event,const Config& cfg,const View& view) {
    auto r=event_record(event,view.episode,cfg.revision,view.level); r.utc_ms=utc_ms();
    storage.enqueue(r,cfg,view.episode,view.level);
}
void sensor_task(void*) {
    esp_task_wdt_add(nullptr);
    bool bus_ok=aircue_i2c_init(board::sda,board::scl,board::i2c_clock_hz)==ESP_OK;
    stcc4_init(STCC4_I2C_ADDR_64); bool first=true,inventory_done=false;
    if(!bus_ok) printf("AIRCUE_SENSOR I2C initialization failed\n");
    while(true) {
        esp_task_wdt_reset(); maintenance=true; sensor_initializing=true;
        bool ok=bus_ok; uint16_t self_test=0xffff; uint32_t product=0; uint64_t serial=0;
        int16_t init_error=-1; const char* stage="bus";
        if(ok) {
            // The application never puts STCC4 to sleep. Stop restores idle
            // after an ESP32 reset without sending an unnecessary wake payload.
            int16_t stop_error=stcc4_stop_continuous_measurement();
            if(stop_error) vTaskDelay(pdMS_TO_TICKS(1200));
            printf("AIRCUE_SENSOR stop_error=%d\n",stop_error);
            stage="identity"; init_error=stcc4_get_product_id(&product,&serial); ok=init_error==0;
            vTaskDelay(pdMS_TO_TICKS(20));
            if(ok) {stage="self_test";init_error=stcc4_perform_self_test(&self_test);ok=init_error==0&&self_test==0;}
            if(ok && first) {
                stage="conditioning";init_error=stcc4_perform_conditioning();ok=init_error==0;
                if(ok) vTaskDelay(pdMS_TO_TICKS(2000)+1);
            }
            if(ok) {stage="start";init_error=stcc4_start_continuous_measurement();ok=init_error==0;}
        }
        uint64_t started=monotonic_ms();
        if(first) warmup_until=started+board::warmup_ms;
        else warmup_until=std::max<uint64_t>(warmup_until,started+20000);
        if(ok) first=false;
        {
            Lock lock(runtime_mutex); char identity[17]; snprintf(identity,sizeof(identity),"%016llx",static_cast<unsigned long long>(serial));
            runtime.sensor_serial=identity; runtime.self_test=self_test; runtime.self_test_ok=ok;
            runtime.sensor_started=started; runtime.commission_elapsed=0;
            runtime.sensor_error=init_error;
        }
        maintenance=false; sensor_fault=!ok;
        printf("AIRCUE_SENSOR initialized=%d stage=%s error=%d product=0x%08lx self_test=0x%04x serial=%016llx\n",ok,stage,init_error,static_cast<unsigned long>(product),self_test,static_cast<unsigned long long>(serial));
        if(bus_ok && !inventory_done) {inspect_i2c_hardware();inventory_done=true;}
        sensor_initializing=false;
        if(!ok) { for(int i=0;i<30;++i) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(1000)); } continue; }
        uint64_t last_good=started;uint16_t previous_status=0;
        TickType_t sample_tick=xTaskGetTickCount();
        while(true) {
            // Use fixed deadlines so I2C work does not accumulate sampling drift.
            // Skip elapsed slots after a stall instead of issuing catch-up reads.
            if(xTaskGetTickCount()-sample_tick>=pdMS_TO_TICKS(1000)) sample_tick=xTaskGetTickCount();
            xTaskDelayUntil(&sample_tick,pdMS_TO_TICKS(1000));
            esp_task_wdt_reset(); Raw raw{};
            int16_t sensor_error=stcc4_read_measurement_raw(&raw.ppm,&raw.temperature,&raw.humidity,&raw.status);
            // A new measurement may lag the nominal one-second period by 150 ms.
            // Retry an I2C failure once; CRC failures remain invalid samples.
            if(sensor_error<0) {
                vTaskDelay(pdMS_TO_TICKS(150)+1);
                sensor_error=stcc4_read_measurement_raw(&raw.ppm,&raw.temperature,&raw.humidity,&raw.status);
            }
            raw.ms=monotonic_ms();raw.good=check_sensor_sample(sensor_error,raw.status,raw.ppm).frame_ok;
            if(!raw.good) printf("AIRCUE_SENSOR sample_invalid error=%d status=0x%04x ppm=%d\n",sensor_error,raw.status,raw.ppm);
            if(raw.good && raw.status!=previous_status) {
                printf("AIRCUE_SENSOR measurement_status=0x%04x raw_preview=available statistics=%s\n",raw.status,raw.status?"excluded":"eligible");
                previous_status=raw.status;
            }
            if(raw.good) last_good=raw.ms;
            // Report missing data promptly, but only restart acquisition after
            // a sustained transport outage. Status flags never stop the sensor.
            if(raw.ms-last_good>=30000) sensor_fault=true;
            {
                Lock lock(runtime_mutex); runtime.sensor_status=raw.status; ++runtime.sample_count;
                runtime.sensor_error=sensor_error; runtime.diagnostic_sample_ms=raw.ms;
                runtime.diagnostic_sample_ok=raw.good; runtime.diagnostic_raw_ppm=raw.ppm;
                if(raw.good) { runtime.temperature=-45+175.0f*raw.temperature/65535; runtime.humidity=-6+125.0f*raw.humidity/65535; }
                else ++runtime.invalid_count;
                runtime.commission_elapsed=raw.ms-started;
            }
            xQueueOverwrite(sensor_samples,&raw);
            if(sensor_fault) break;
        }
    }
}
void alarm_task(void*) {
    esp_task_wdt_add(nullptr);
    Config cfg=config.get(); AlarmEngine engine(cfg.rules);
    Episode recovered; Level recovered_level; uint32_t recovered_revision=0;
    if(storage.restore(recovered,recovered_level,recovered_revision)) {
        engine.restore(recovered,recovered_level);
        if(recovered.active && recovered_revision!=cfg.rules_revision) {
            engine.configure(cfg.rules); record_event(Event::ConfigChanged,cfg,engine.view());
        }
    }
    record_event(Event::Boot,cfg,engine.view());
    MinuteAccumulator minute;
    ClimateAccumulator climate;
    Config window_cfg=cfg;
    bool partial=true;
    auto open_window=[&](uint64_t begin,uint64_t wall,bool interrupted) {
        uint64_t end=begin+60000;
        uint32_t index=UINT32_MAX;
        partial=interrupted;
        if(!cfg.run_id.empty() && !cfg.demo && wall) {
            if(wall<cfg.run_t0) {
                end=std::min(end,begin+cfg.run_t0-wall);
                partial=true;
            } else {
                uint64_t offset=(wall-cfg.run_t0)%60000;
                uint64_t window=(wall-cfg.run_t0)/60000;
                if(window<4320) { index=window; end=begin+60000-offset; }
                partial=partial || offset!=0;
            }
        }
        minute.begin(begin,end,cfg.revision,index);
        climate.reset();
        window_cfg=cfg;
    };
    open_window(monotonic_ms(),utc_ms(),true);
    uint64_t last_utc=0,last_clock_mono=0,last_raw=0,last_good=0,next_barometer_summary=0;
    bool last_fault=false,ota_validated=false;
    uint64_t next_boot_validation=0;
    while(true) {
        esp_task_wdt_reset();
        // Consume new samples directly; an independent polling phase can invent gaps.
        // The bounded wait still advances fault detection if the producer stops.
        Raw raw{};
        uint64_t before_wait=monotonic_ms(),wait_ms=maintenance?1000:1500;
        if(last_good && before_wait<last_good+5000) wait_ms=std::min(wait_ms,last_good+5000-before_wait);
        bool received=xQueueReceive(sensor_samples,&raw,pdMS_TO_TICKS(wait_ms))==pdTRUE;
        uint64_t now=monotonic_ms(),utc=utc_ms();
        Config latest=config.get();
        bool rule_change=latest.rules!=cfg.rules || latest.demo!=cfg.demo;
        bool time_change=utc && (!last_utc || std::llabs(static_cast<int64_t>(utc)-static_cast<int64_t>(last_utc)-static_cast<int64_t>(now-last_clock_mono))>2000);
        bool context_change=rule_change || window_cfg.run_id!=latest.run_id || window_cfg.demo!=latest.demo || (time_change&&!latest.run_id.empty());
        if(rule_change) {
            bool active=engine.view().episode.active;
            engine.configure(latest.rules);
            if(active) record_event(Event::ConfigChanged,cfg,engine.view());
            partial=true;
        }
        cfg=latest;
        if(time_change) {
            if(last_utc) { Lock lock(runtime_mutex); ++runtime.time_jumps; }
            record_event(Event::TimeAnchor,cfg,engine.view());
        }
        last_utc=utc; last_clock_mono=now;
        bool fresh=received && raw.ms!=last_raw && now-raw.ms<=1500;
        if(fresh) { last_raw=raw.ms; if(raw.good) last_good=raw.ms; }
        auto state=snapshot(); Sample sample; sample.ms=fresh?raw.ms:now;
        bool bypass=fresh && raw.ms-state.sensor_started<20000;
        bool missing=!maintenance && now-state.sensor_started>=5000 && now-last_good>=5000;
        if(sensor_fault || missing || (fresh && raw.good && raw.status!=0)) sample.quality=Quality::Fault;
        else if(!cfg.commissioned || cfg.sensor_serial!=state.sensor_serial) sample.quality=Quality::Commissioning;
        else if(maintenance || now<warmup_until || bypass) sample.quality=Quality::Warmup;
        else sample.quality=Quality::Valid;
        sample.available=fresh&&raw.good&&raw.status==0&&!bypass; sample.ppm=raw.ppm;
        if(bypass) { Lock lock(runtime_mutex); ++runtime.bypass_count; }
        auto step=engine.update(sample);
        if(step.event!=Event::None) {
            auto r=event_record(step.event,engine.view().episode,cfg.revision,engine.view().level); r.utc_ms=utc;
            auto id=storage.enqueue(r,cfg,engine.view().episode,engine.view().level);
            if(step.event==Event::EpisodeStart) engine.identify_episode(id);
        }
        if(context_change) { minute.truncate(now); partial=true; }
        // Include the measured interval ending at this sample before closing a window.
        minute.add(sample,step,engine.view());
        while(now>=minute.end()) {
            uint64_t boundary=minute.end();
            auto summary=minute.finish(); auto record=minute_record(summary);
            write_climate(record,climate.finish());
            record.monotonic_ms=boundary;
            record.utc_ms=utc && utc>=now-boundary ? utc-(now-boundary) : 0;
            if(partial || summary.window_ms!=60000) record.flags|=Partial;
            if(engine.view().range_warn) record.flags|=RangeWarn;
            storage.enqueue(record,window_cfg,engine.view().episode,engine.view().level);
            open_window(boundary,record.utc_ms,context_change);
            context_change=false;
            minute.add(sample,step,engine.view());
        }
        // Climate previews remain separate from the validated CO2 alarm statistics.
        // Only new CO2 readings are counted; barometer acquisition also survives CO2 faults.
        auto ambient=climate_sample(state);
        bluetooth_update_live(ambient,engine.view(),now,state.diagnostic_sample_ms?now-state.diagnostic_sample_ms:UINT16_MAX);
        if(!fresh) ambient.mask&=~7U;
        if(now<next_barometer_summary) ambient.mask&=7U;
        else next_barometer_summary=now+900;
        climate.add(ambient);
        bool fault=engine.view().quality==Quality::Fault;
        if(fault!=last_fault) record_event(fault?Event::SensorFault:Event::SensorRestored,cfg,engine.view());
        last_fault=fault;
        { Lock lock(runtime_mutex); runtime.view=engine.view(); }
        // Local sensor communication and storage suffice; cloud reachability is irrelevant.
        if(!ota_validated && now>=next_boot_validation && fresh && raw.good && storage.healthy && state.self_test_ok) {
            esp_err_t result=esp_ota_mark_app_valid_cancel_rollback();
            ota_validated=result==ESP_OK;
            next_boot_validation=now+30000;
            { Lock lock(runtime_mutex); runtime.boot_validated=ota_validated; runtime.boot_validation_error=result; }
            if(result!=ESP_OK) printf("AIRCUE_OTA boot_validation_error=%d retry_in_ms=30000\n",result);
        }
    }
}
void output_task(void*) {
    // The output task owns the buzzer; PWM is attached only when sound is needed.
    gpio_reset_pin(static_cast<gpio_num_t>(board::buzzer));
    gpio_set_level(static_cast<gpio_num_t>(board::buzzer),0);
    gpio_set_direction(static_cast<gpio_num_t>(board::buzzer),GPIO_MODE_INPUT_OUTPUT);
    const int pins[]={board::red,board::yellow,board::green};
    for(auto pin:pins) { gpio_reset_pin(static_cast<gpio_num_t>(pin)); gpio_set_direction(static_cast<gpio_num_t>(pin),GPIO_MODE_INPUT_OUTPUT); }
    bool pwm_ready=false;
    unsigned previous_duty=0;
    uint64_t test_start=0,last_test=0;unsigned test_pattern=0;
    esp_task_wdt_add(nullptr); uint64_t next_beep=0,pattern_start=0; bool was_high=false,was_fault=false; int pattern=0;
    while(true) {
        esp_task_wdt_reset(); uint64_t now=monotonic_ms(); auto state=snapshot(); auto cfg=config.get();
        bool fault=state.view.quality==Quality::Fault || (!storage.healthy && now>3000);
        bool waiting=state.view.quality!=Quality::Valid || state.view.level==Level::Unassessed;
        bool high=!waiting&&state.view.level==Level::High;
        bool muted=cfg.muted_episode==state.view.episode.id && now<cfg.mute_until;
        bool red=false,yellow=false,green=false;
        if(fault) { red=(now/250)%2; yellow=!red; }
        else if(waiting) yellow=(now/1000)%2;
        else if(high) red=state.view.trend!=Trend::Falling || (now/1000)%2;
        else if(state.view.level==Level::Watch) yellow=true;
        else if(state.view.level==Level::Clear) green=true;
        uint64_t test_end=led_test_until.load();
        if(test_end>now && cfg.run_id.empty()) {
            unsigned phase=(now-(test_end-3000))/1000;
            red=phase==0; yellow=phase==1; green=phase==2;
        }
        gpio_set_level(static_cast<gpio_num_t>(board::red),red==board::led_active_high);
        gpio_set_level(static_cast<gpio_num_t>(board::yellow),yellow==board::led_active_high);
        gpio_set_level(static_cast<gpio_num_t>(board::green),green==board::led_active_high);
        if(fault && (!was_fault || now>=next_beep)) { pattern=2; pattern_start=now; next_beep=now+300000; }
        else if(!fault && high && !muted && (!was_high || now>=next_beep)) { pattern=1; pattern_start=now; next_beep=now+300000; }
        if((pattern==1 && (muted || !high)) || (pattern==2 && !fault)) pattern=0;
        uint64_t t=now-pattern_start;
        bool sound=pattern==1?(t<150 || (t>=300&&t<450)):pattern==2?(t<70||(t>=140&&t<210)||(t>=280&&t<350)):false;
        if(t>500) pattern=0;
        unsigned test=buzzer_test_request.exchange(0);
        if(test>=1 && test<=2 && cfg.run_id.empty() && (!last_test || now-last_test>=5000)) {
            test_pattern=test;test_start=last_test=now;
            Lock lock(runtime_mutex);++runtime.buzzer_tests;
            printf("AIRCUE_TEST buzzer pattern=%u started duration_ms=500\n",test);
        }
        if(test_pattern && now-test_start>=500) {test_pattern=0;printf("AIRCUE_TEST buzzer completed output=off\n");}
        uint64_t test_t=now-test_start;
        bool test_sound=test_pattern==1?(test_t<150||(test_t>=300&&test_t<450)):
            test_pattern==2?(test_t<70||(test_t>=140&&test_t<210)||(test_t>=280&&test_t<350)):false;
        bool audible=cfg.buzzer_enabled?sound:false;
        if(test_pattern) audible=test_sound;
        if(audible && !pwm_ready) {
            ledc_timer_config_t timer{};timer.speed_mode=LEDC_LOW_SPEED_MODE;timer.duty_resolution=LEDC_TIMER_10_BIT;
            timer.timer_num=LEDC_TIMER_0;timer.freq_hz=board::buzzer_frequency_hz;timer.clk_cfg=LEDC_AUTO_CLK;
            ledc_channel_config_t channel{};channel.gpio_num=board::buzzer;channel.speed_mode=LEDC_LOW_SPEED_MODE;
            channel.channel=LEDC_CHANNEL_0;channel.timer_sel=LEDC_TIMER_0;
            pwm_ready=ledc_timer_config(&timer)==ESP_OK && ledc_channel_config(&channel)==ESP_OK;
        }
        if(pwm_ready) {ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,audible?512:0);ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);}
        unsigned duty=pwm_ready&&audible?512:0;
        if(duty!=previous_duty && (test_pattern || !cfg.buzzer_enabled)) printf("AIRCUE_TEST buzzer pwm_duty=%u\n",duty);
        previous_duty=duty;
        {Lock lock(runtime_mutex);runtime.buzzer_requested=sound;runtime.buzzer_duty=pwm_ready&&audible?512:0;}
        was_high=high; was_fault=fault; vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}
