#include "aircue/runtime.hpp"
#include "aircue/ble.hpp"
#include "aircue/board.hpp"
#include "driver/gpio.h"
#include "aircue_i2c.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_task_wdt.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aircue {
namespace {
SemaphoreHandle_t hardware_mutex=xSemaphoreCreateMutex();
struct Hardware {
    bool scanned=false,atecc_wake=false,bmp_ok=false,mpu_ok=false,oled_sent=false;
    std::vector<uint8_t> addresses;
    uint8_t atecc_address=0,bmp_address=0,bmp_id=0,bmp_control=0,bmp_status=0,mpu_address=0,mpu_id=0,lora_version=0;
    int32_t bmp_raw_pressure=0,bmp_raw_temperature=0;
    int pot=-1,pot_min=4096,pot_max=-1,button1=-1,button2=-1;
    unsigned button1_edges=0,button2_edges=0;
    uint8_t calibration[24]{};
    double pressure_hpa=0,barometer_c=0,acceleration[3]{};
    uint64_t auxiliary_ms=0;
} hardware;
spi_device_handle_t oled=nullptr,radio=nullptr;

bool read_register(uint8_t address,uint8_t reg,uint8_t* data,size_t size) {
    return aircue_i2c_write_read(address,&reg,1,data,size)==ESP_OK;
}
bool write_register(uint8_t address,uint8_t reg,uint8_t value) {
    uint8_t bytes[]={reg,value};
    return aircue_i2c_write(address,bytes,sizeof(bytes))==ESP_OK;
}
bool probe_address(uint8_t address) {
    return aircue_i2c_probe(address)==ESP_OK;
}
void digital_output(int pin,int value) {
    gpio_reset_pin(static_cast<gpio_num_t>(pin));
    gpio_set_level(static_cast<gpio_num_t>(pin),value);
    gpio_set_direction(static_cast<gpio_num_t>(pin),GPIO_MODE_OUTPUT);
}
bool oled_write(bool data,const uint8_t* bytes,size_t size) {
    if(!oled) return false;
    gpio_set_level(static_cast<gpio_num_t>(board::oled_dc),data);
    spi_transaction_t transaction{};transaction.length=size*8;transaction.tx_buffer=bytes;
    return spi_device_transmit(oled,&transaction)==ESP_OK;
}
void display_status() {
    if(!oled) return;
    // A compact 5-by-7 alphabet keeps the diagnostic screen readable offline.
    static const struct Glyph {char character;uint8_t columns[5];} font[]={
        {'A',{126,9,9,9,126}},{'B',{127,73,73,73,54}},{'C',{62,65,65,65,34}},
        {'D',{127,65,65,34,28}},{'E',{127,73,73,73,65}},{'F',{127,9,9,9,1}},
        {'G',{62,65,73,73,58}},{'H',{127,8,8,8,127}},{'I',{65,65,127,65,65}},
        {'J',{32,64,65,63,1}},{'K',{127,8,20,34,65}},{'L',{127,64,64,64,64}},
        {'M',{127,2,12,2,127}},{'N',{127,4,8,16,127}},{'O',{62,65,65,65,62}},
        {'P',{127,9,9,9,6}},{'Q',{62,65,81,33,94}},{'R',{127,9,25,41,70}},
        {'S',{38,73,73,73,50}},{'T',{1,1,127,1,1}},{'U',{63,64,64,64,63}},
        {'V',{31,32,64,32,31}},{'W',{63,64,56,64,63}},{'X',{99,20,8,20,99}},
        {'Y',{3,4,120,4,3}},{'Z',{97,81,73,69,67}},
        {'0',{62,81,73,69,62}},{'1',{0,66,127,64,0}},{'2',{98,81,73,73,70}},
        {'3',{34,65,73,73,54}},{'4',{24,20,18,127,16}},{'5',{39,69,69,69,57}},
        {'6',{60,74,73,73,48}},{'7',{1,113,9,5,3}},{'8',{54,73,73,73,54}},
        {'9',{6,73,73,41,30}},{':',{0,54,54,0,0}},{'.',{0,96,96,0,0}},
        {'-',{8,8,8,8,8}},{'%',{99,19,8,100,99}},{'/',{32,16,8,4,2}}
    };
    uint8_t pixels[1024]{};
    auto line=[&](unsigned row,const char* text) {
        for(unsigned x=0;*text&&x+5<128;++text,x+=6)
            for(const auto& glyph:font) if(glyph.character==*text) {memcpy(pixels+row*128+x,glyph.columns,5);break;}
    };
    auto state=snapshot();char text[32];
    line(0,"AIRCUE - AUTONOMOUS");
    line(1,state.diagnostic_sample_ok&&state.sensor_status?"SENSOR STATUS FLAG":state.self_test_ok?"SENSOR READY":"SENSOR CHECKING");
    if(state.diagnostic_sample_ok) {
        snprintf(text,sizeof(text),"CO2 RAW: %d PPM",state.diagnostic_raw_ppm);line(2,text);
        snprintf(text,sizeof(text),"T: %.1f C",state.temperature);line(3,text);
        snprintf(text,sizeof(text),"RH: %.1f %%",state.humidity);line(4,text);
    } else line(2,state.sample_count?"SAMPLE UNAVAILABLE":"SENSOR STARTING");
    line(5,state.sta_connected?"WIFI CONNECTED":"WIFI OFFLINE");
    uint32_t code=0;
    if(bluetooth_pairing_code(code)) {snprintf(text,sizeof(text),"BLE PIN: %06lu",static_cast<unsigned long>(code));line(6,text);}
    else line(6,config.get().buzzer_enabled?"SOUND ENABLED":"SOUND DISABLED");
    Json ble(bluetooth_json());
    line(7,bool_field(ble.p,"secured")?"BLE CONNECTED":number_field(ble.p,"pairing_seconds")>0?"BLE PAIRING OPEN":"PAIR: HOLD BUTTON 1");
    const uint8_t address[]={0x21,0,127,0x22,0,7};
    bool ok=oled_write(false,address,sizeof(address))&&oled_write(true,pixels,sizeof(pixels));
    Lock lock(hardware_mutex);hardware.oled_sent=ok;
}
void initialize_spi() {
    // Both chip selects must be inactive before clocking the shared SPI bus.
    digital_output(board::oled_cs,1); digital_output(board::lora_cs,1);
    digital_output(board::oled_dc,0); digital_output(board::oled_reset,1);
    digital_output(board::lora_reset,1);
    if constexpr(board::lora_probe_enabled) {
        gpio_set_level(static_cast<gpio_num_t>(board::lora_reset),0);vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(static_cast<gpio_num_t>(board::lora_reset),1);vTaskDelay(pdMS_TO_TICKS(10));
    }
    spi_bus_config_t bus{};bus.mosi_io_num=board::spi_mosi;bus.miso_io_num=board::spi_miso;
    bus.sclk_io_num=board::spi_sck;bus.quadwp_io_num=-1;bus.quadhd_io_num=-1;bus.max_transfer_sz=1024;
    // Match Activity 6's VSPI host and native 18/19/23 signal routing.
    if(spi_bus_initialize(SPI3_HOST,&bus,SPI_DMA_CH_AUTO)!=ESP_OK) return;
    spi_device_interface_config_t display{};display.clock_speed_hz=1000000;display.spics_io_num=board::oled_cs;
    display.queue_size=1;display.flags=SPI_DEVICE_HALFDUPLEX;
    if(spi_bus_add_device(SPI3_HOST,&display,&oled)!=ESP_OK) oled=nullptr;
    spi_device_interface_config_t lora{};lora.clock_speed_hz=500000;lora.spics_io_num=-1;lora.queue_size=1;
    if(board::lora_probe_enabled && spi_bus_add_device(SPI3_HOST,&lora,&radio)==ESP_OK) {
        // Match the course library: manual CS remains low across two byte transfers.
        for(int attempt=0;attempt<3;++attempt) {
            uint8_t version=0;esp_err_t result=spi_device_acquire_bus(radio,portMAX_DELAY);
            if(result==ESP_OK) {
                gpio_set_level(static_cast<gpio_num_t>(board::lora_cs),0);
                spi_transaction_t address{},data{};
                address.length=data.length=8;
                address.flags=data.flags=SPI_TRANS_USE_TXDATA|SPI_TRANS_USE_RXDATA;
                address.tx_data[0]=0x42;
                result=spi_device_polling_transmit(radio,&address);
                if(result==ESP_OK) result=spi_device_polling_transmit(radio,&data);
                version=data.rx_data[0];
                gpio_set_level(static_cast<gpio_num_t>(board::lora_cs),1);
                spi_device_release_bus(radio);
            }
            if(result==ESP_OK) {Lock lock(hardware_mutex);hardware.lora_version=version;}
            printf("AIRCUE_RADIO attempt=%d result=%s version=0x%02x tx=disabled\n",attempt+1,esp_err_to_name(result),version);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if(oled) {
        gpio_set_level(static_cast<gpio_num_t>(board::oled_reset),0);vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(static_cast<gpio_num_t>(board::oled_reset),1);vTaskDelay(pdMS_TO_TICKS(20));
        const uint8_t commands[]={0xae,0xd5,0x80,0xa8,0x3f,0xd3,0,0x40,0x8d,0x14,0x20,0,0xa1,0xc8,0xda,0x12,0x81,0x7f,0xd9,0xf1,0xdb,0x40,0xa4,0xa6,0xaf};
        if(oled_write(false,commands,sizeof(commands))) display_status();
    }
}
uint16_t u16(const uint8_t* p) {return uint16_t(p[0])|(uint16_t(p[1])<<8);}
int16_t s16(const uint8_t* p) {return static_cast<int16_t>(u16(p));}
void auxiliary_sample() {
    Hardware copy;{Lock lock(hardware_mutex);copy=hardware;}
    if(!copy.scanned) return;
    bool bmp=false;double temperature=0,pressure=0;
    int32_t raw_pressure=0,raw_temperature=0;uint8_t control=0,status=0;
    uint8_t bytes[14]{};
    if(copy.bmp_address && read_register(copy.bmp_address,0xf7,bytes,6)) {
        int32_t p=(int32_t(bytes[0])<<12)|(int32_t(bytes[1])<<4)|(bytes[2]>>4);
        int32_t t=(int32_t(bytes[3])<<12)|(int32_t(bytes[4])<<4)|(bytes[5]>>4);
        raw_pressure=p;raw_temperature=t;
        read_register(copy.bmp_address,0xf4,&control,1);read_register(copy.bmp_address,0xf3,&status,1);
        const auto* c=copy.calibration;
        double a=(t/16384.0-u16(c)/1024.0)*s16(c+2);
        double b=(t/131072.0-u16(c)/8192.0);b=b*b*s16(c+4);
        double fine=a+b;temperature=fine/5120.0;
        a=fine/2.0-64000.0;
        b=a*a*s16(c+16)/32768.0;
        b+=a*s16(c+14)*2.0;b=b/4.0+s16(c+12)*65536.0;
        a=(s16(c+10)*a*a/524288.0+s16(c+8)*a)/524288.0;
        a=(1.0+a/32768.0)*u16(c+6);
        if(a!=0 && p!=0x80000 && t!=0x80000) {
            pressure=1048576.0-p;pressure=(pressure-b/4096.0)*6250.0/a;
            a=s16(c+22)*pressure*pressure/2147483648.0;b=pressure*s16(c+20)/32768.0;
            pressure=(pressure+(a+b+s16(c+18))/16.0)/100.0;
            bmp=std::isfinite(pressure)&&pressure>=300&&pressure<=1100&&temperature>=-40&&temperature<=85;
        }
    }
    {Lock lock(hardware_mutex);hardware.bmp_ok=bmp;
     hardware.pressure_hpa=pressure;hardware.barometer_c=temperature;
     hardware.bmp_control=control;hardware.bmp_status=status;
     hardware.bmp_raw_pressure=raw_pressure;hardware.bmp_raw_temperature=raw_temperature;
     hardware.auxiliary_ms=monotonic_ms();}
}
void print_status() {
    // Diagnostic output must never delay buttons or the OLED during a TLS transfer.
    UplinkMemory memory(false);if(!memory) return;
    auto value=json_take(status_json());if(!value.empty()) printf("AIRCUE_STATUS %s\n",value.c_str());fflush(stdout);
}
void command(const std::string& line) {
    if(line=="status" || line=="buzzer") print_status();
    else if(line=="hardware") {Json value(hardware_json());printf("AIRCUE_HARDWARE %s\n",json_text(value.p).c_str());}
    else if(line=="leds" && config.get().run_id.empty()) {led_test_until=monotonic_ms()+3000;printf("AIRCUE_TEST LEDs: red, yellow, green; one second each.\n");}
    else if(line=="ble_pair") {bluetooth_pairing_window();printf("AIRCUE_BLE pairing window opened; passkey appears only on OLED\n");}
    else if((line=="buzzer_high"||line=="buzzer_fault") && config.get().run_id.empty()) buzzer_test_request=line=="buzzer_high"?1:2;
    else if(line=="history") {
        for(const auto& item:storage.page(0,16)) {Json value(stored_json(item));printf("AIRCUE_RECORD %s\n",json_text(value.p).c_str());}
        printf("AIRCUE_HISTORY_END\n");
    } else if(!line.empty()) printf("AIRCUE_HELP status | hardware | history | leds | buzzer (read-only) | buzzer_high | buzzer_fault | ble_pair\n");
}
}

void inspect_i2c_hardware() {
    Hardware value;
    for(uint8_t address=8;address<120;++address) if(probe_address(address)) value.addresses.push_back(address);
    // A sleeping ATECC may not ACK a scan. Send only a wake token, read its response,
    // then put it back to sleep. No configuration, key, or lock commands are issued.
    uint8_t wake[4]{};
    for(uint8_t address:{0x60,0x35,0x36}) {
        if(!board::atecc_probe_enabled) break;
        if(aircue_i2c_wake_secure_element()!=ESP_OK) continue;
        if(aircue_i2c_read(address,wake,4)!=ESP_OK) continue;
        const uint8_t expected[]={4,0x11,0x33,0x43};
        if(memcmp(wake,expected,4)==0) {
            value.atecc_wake=true;value.atecc_address=address;
            uint8_t sleep=1;aircue_i2c_write(address,&sleep,1);break;
        }
    }
    for(uint8_t address:{0x76,0x77}) {
        uint8_t id=0;
        if(read_register(address,0xd0,&id,1) && (id==0x58||id==0x60)) {
            value.bmp_address=address;value.bmp_id=id;
            if(!read_register(address,0x88,value.calibration,24)) value.bmp_address=0;
            else {write_register(address,0xf5,0xa0);write_register(address,0xf4,0x27);}
            printf("AIRCUE_BMP calibration=%s\n",hex(value.calibration,24).c_str());
            break;
        }
    }
    {Lock lock(hardware_mutex);hardware.addresses=value.addresses;hardware.atecc_wake=value.atecc_wake;hardware.atecc_address=value.atecc_address;
     hardware.bmp_address=value.bmp_address;hardware.bmp_id=value.bmp_id;hardware.mpu_address=value.mpu_address;hardware.mpu_id=value.mpu_id;
     memcpy(hardware.calibration,value.calibration,24);hardware.scanned=true;}
    Json out(hardware_json());printf("AIRCUE_HARDWARE %s\n",json_text(out.p).c_str());
}
cJSON* hardware_json(bool full) {
    Hardware value;{Lock lock(hardware_mutex);value=hardware;}
    auto* out=cJSON_CreateObject();
    if(full) {
    auto* addresses=cJSON_AddArrayToObject(out,"i2c_addresses");
    for(auto address:value.addresses) {char text[5];snprintf(text,sizeof(text),"0x%02x",address);cJSON_AddItemToArray(addresses,cJSON_CreateString(text));}
    cJSON_AddBoolToObject(out,"i2c_scan_complete",value.scanned);
    cJSON_AddNumberToObject(out,"i2c_clock_hz",board::i2c_clock_hz);
    cJSON_AddStringToObject(out,"i2c_implementation","software_open_drain");
    cJSON_AddBoolToObject(out,"atecc_wake_response",value.atecc_wake);
    cJSON_AddBoolToObject(out,"atecc_probe_enabled",board::atecc_probe_enabled);
    cJSON_AddStringToObject(out,"atecc_status",board::atecc_probe_enabled?(value.atecc_wake?"RESPONDING":"NOT_DETECTED"):"DEFERRED_SOLDER_REPAIR");
    cJSON_AddNumberToObject(out,"atecc_address",value.atecc_address);
    cJSON_AddStringToObject(out,"atecc_access",board::atecc_probe_enabled?"wake/read/sleep only; no provisioning or locking":"Dedicated probe disabled pending solder repair");
    cJSON_AddNumberToObject(out,"bmp_chip_id",value.bmp_id);cJSON_AddBoolToObject(out,"bmp_data_ok",value.bmp_ok);
    cJSON_AddNumberToObject(out,"bmp_control",value.bmp_control);cJSON_AddNumberToObject(out,"bmp_status",value.bmp_status);
    cJSON_AddNumberToObject(out,"bmp_raw_pressure",value.bmp_raw_pressure);cJSON_AddNumberToObject(out,"bmp_raw_temperature",value.bmp_raw_temperature);
    cJSON_AddNumberToObject(out,"bmp_diagnostic_pressure_hpa",value.pressure_hpa);cJSON_AddNumberToObject(out,"bmp_diagnostic_temperature_c",value.barometer_c);
    if(value.bmp_ok) {cJSON_AddNumberToObject(out,"pressure_hpa",value.pressure_hpa);cJSON_AddNumberToObject(out,"barometer_temperature_c",value.barometer_c);}
    cJSON_AddNumberToObject(out,"lora_reg_version",value.lora_version);cJSON_AddBoolToObject(out,"lora_sx127x_detected",value.lora_version==0x12);
    cJSON_AddBoolToObject(out,"lora_probe_enabled",board::lora_probe_enabled);
    cJSON_AddStringToObject(out,"lora_status",board::lora_probe_enabled?(value.lora_version==0x12?"RESPONDING":"NOT_DETECTED"):"DEFERRED_SOLDER_REPAIR");
    cJSON_AddBoolToObject(out,"radio_transmission_enabled",false);
    cJSON_AddBoolToObject(out,"oled_status_sent",value.oled_sent);
    cJSON_AddStringToObject(out,"oled_verification","Visual confirmation required; write-only SPI has no presence ACK");
    cJSON_AddNumberToObject(out,"button1_level",value.button1);cJSON_AddNumberToObject(out,"button2_level",value.button2);
    cJSON_AddNumberToObject(out,"button1_edges",value.button1_edges);cJSON_AddNumberToObject(out,"button2_edges",value.button2_edges);
    } else {
        // Upload all climate values and operational health; verbose bus diagnostics stay on BLE.
        cJSON_AddBoolToObject(out,"bmp_data_ok",value.bmp_ok);
        if(value.bmp_ok) {cJSON_AddNumberToObject(out,"pressure_hpa",value.pressure_hpa);cJSON_AddNumberToObject(out,"barometer_temperature_c",value.barometer_c);}
        cJSON_AddStringToObject(out,"atecc_status",value.atecc_wake?"RESPONDING":"NOT_DETECTED");
        cJSON_AddStringToObject(out,"lora_status",value.lora_version==0x12?"RESPONDING":"NOT_DETECTED");
        cJSON_AddBoolToObject(out,"oled_status_sent",value.oled_sent);
    }
    cJSON_AddBoolToObject(out,"unrelated_sensor_sampling",false);
    auto state=snapshot();
    cJSON_AddBoolToObject(out,"buzzer_sound_enabled",config.get().buzzer_enabled);
    cJSON_AddNumberToObject(out,"buzzer_gpio_level",gpio_get_level(static_cast<gpio_num_t>(board::buzzer)));
    cJSON_AddNumberToObject(out,"buzzer_pwm_duty",state.buzzer_duty);
    cJSON_AddNumberToObject(out,"buzzer_test_count",state.buzzer_tests);
    if(full) {
        cJSON_AddStringToObject(out,"buzzer_test","PWM duty is observable; audible output requires physical confirmation");
        cJSON_AddStringToObject(out,"servo_test","Not driven; a standard SG90 has no position-feedback input");
    }
    auto* leds=cJSON_AddArrayToObject(out,"led_gpio_levels");
    for(int pin:{board::red,board::yellow,board::green}) cJSON_AddItemToArray(leds,cJSON_CreateNumber(gpio_get_level(static_cast<gpio_num_t>(pin))));
    return out;
}
Climate climate_sample(const Runtime& state) {
    Climate value;uint64_t now=monotonic_ms();
    if(state.diagnostic_sample_ok && state.diagnostic_sample_ms && now-state.diagnostic_sample_ms<=1500) {
        value.mask=7;value.values[RawCo2]=state.diagnostic_raw_ppm;
        value.values[AirTemperature]=state.temperature;value.values[Humidity]=state.humidity;
    }
    Lock lock(hardware_mutex);
    if(hardware.bmp_ok && hardware.auxiliary_ms && now-hardware.auxiliary_ms<=2500) {
        value.mask|=(1U<<Pressure)|(1U<<BarometerTemperature);
        value.values[Pressure]=hardware.pressure_hpa;value.values[BarometerTemperature]=hardware.barometer_c;
    }
    return value;
}
void hardware_task(void*) {
    esp_task_wdt_add(nullptr);
    for(int pin:{board::button1,board::button2}) {
        gpio_config_t cfg{};cfg.pin_bit_mask=1ULL<<pin;cfg.mode=GPIO_MODE_INPUT;cfg.pull_up_en=GPIO_PULLUP_ENABLE;gpio_config(&cfg);
    }
    // Keep the unused servo signal inactive rather than moving an unobserved mechanism.
    digital_output(board::servo,0);
    initialize_spi();
    uart_driver_install(UART_NUM_0,2048,0,0,nullptr,0);
    std::string input;uint64_t next_status=0,next_auxiliary=0,button1_down=0,button2_down=0;bool pairing_opened=false;
    printf("AIRCUE_CONSOLE Ready. Autonomous tasks do not require a serial client.\n");
    while(true) {
        esp_task_wdt_reset();uint64_t now=monotonic_ms();
        int b1=gpio_get_level(static_cast<gpio_num_t>(board::button1)),b2=gpio_get_level(static_cast<gpio_num_t>(board::button2));
        if(b1==0) {
            if(!button1_down) button1_down=now;
            if(!pairing_opened && now-button1_down>=2000) {bluetooth_pairing_window();pairing_opened=true;}
        } else {button1_down=0;pairing_opened=false;}
        if(b2==0) {if(!button2_down) button2_down=now;}
        else if(button2_down) {
            if(now-button2_down>=50) {
                auto state=snapshot();auto cfg=config.get();
                if(state.view.episode.active) {
                    Json request(cJSON_CreateObject());cJSON_AddStringToObject(request.p,"request_id",random_hex(16).c_str());
                    cJSON_AddNumberToObject(request.p,"base_revision",cfg.revision);cJSON_AddNumberToObject(request.p,"duration_s",300);
                    cJSON_AddStringToObject(request.p,"episode_id",state.view.episode.id.str().c_str());
                    Json result(config.apply(request.p,"mute",false));
                }
            }
            button2_down=0;
        }
        {Lock lock(hardware_mutex);
         if(hardware.button1>=0&&hardware.button1!=b1) ++hardware.button1_edges;
         if(hardware.button2>=0&&hardware.button2!=b2) ++hardware.button2_edges;
         hardware.button1=b1;hardware.button2=b2;}
        if(now>=next_auxiliary) {
            // Leave the bus quiet while the CO2 sensor runs its startup checks.
            if(!sensor_initializing) auxiliary_sample();
            display_status();next_auxiliary=now+1000;
        }
        if(now>=next_status) {print_status();next_status=now+5000;}
        uint8_t bytes[64];int count=uart_read_bytes(UART_NUM_0,bytes,sizeof(bytes),pdMS_TO_TICKS(25));
        for(int i=0;i<count;++i) {
            if(bytes[i]=='\n'||bytes[i]=='\r') {command(input);input.clear();}
            else if(bytes[i]>=32&&bytes[i]<127&&input.size()<64) input+=static_cast<char>(bytes[i]);
        }
    }
}
}
