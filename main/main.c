#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "udp_audio.h"
#include "tcp_audio.h"

void bt_main(void);
void ap_main(void);

void app_main(void)
{

    /* initialize NVS — it is used to store PHY calibration data */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Start the SoftAP */
    ap_main();

    /* Start the Bluetooth A2DP Sink */
    bt_main();

    /* Initialize UDP audio */
    udp_audio_init("192.168.4.2", 12345);

    /* Initialize TCP audio */
    // tcp_audio_init("192.168.4.2", 12345);
}