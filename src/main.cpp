#include "aircue/runtime.hpp"
#include "aircue/ble.hpp"
#include "esp_system.h"
#include "esp_random.h"
#include "nvs_flash.h"

extern "C" void app_main() {
    using namespace aircue;
    runtime.reset_reason=esp_reset_reason();
    // Start visual feedback before attempting storage or networking initialization.
    configASSERT(xTaskCreate(output_task,"outputs",3584,nullptr,8,nullptr)==pdPASS);
    esp_err_t nvs=nvs_flash_init();
    if(nvs!=ESP_OK || !config.begin()) {
        printf("AIRCUE_BOOT NVS initialization failed: %s\n",esp_err_to_name(nvs));
        Lock lock(runtime_mutex); runtime.view.quality=Quality::Fault;
        // Preserve corrupt NVS for diagnosis instead of silently erasing credentials.
        return;
    }
    if(!network_start()) { Lock lock(runtime_mutex); runtime.network="WIFI_INIT_FAILED"; }
    if(!bluetooth_start()) printf("AIRCUE_BOOT bluetooth initialization failed\n");
    // The BLE controller provides RF entropy even when Wi-Fi is manually disabled.
    esp_fill_random(boot_id.data(),boot_id.size());
    bool storage_ok=storage.begin(boot_id,config.boot_ordinal);
    printf("AIRCUE_BOOT journal=%s\n",storage_ok?"ready":"failed");
    configASSERT(xTaskCreate([](void*){storage.run();},"journal",4096,nullptr,3,nullptr)==pdPASS);
    configASSERT(xTaskCreate(sensor_task,"sensor",4096,nullptr,7,nullptr)==pdPASS);
    configASSERT(xTaskCreate(alarm_task,"alarm",8192,nullptr,6,nullptr)==pdPASS);
    configASSERT(xTaskCreate(hardware_task,"hardware",6144,nullptr,3,nullptr)==pdPASS);
    configASSERT(xTaskCreate(network_task,"network",3072,nullptr,4,nullptr)==pdPASS);
    configASSERT(xTaskCreate(sync_task,"sync",8192,nullptr,2,nullptr)==pdPASS);
    configASSERT(xTaskCreate(mqtt_task,"remote_mqtt",8192,nullptr,2,nullptr)==pdPASS);
}
