#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "commands.h"

ampel_config_t my_lights[] = {
    { .red = GPIO_NUM_23, .yellow = GPIO_NUM_22, .green = GPIO_NUM_18 },
};

#define LIGHT_COUNT (sizeof(my_lights) / sizeof(ampel_config_t))

void on_sent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
    
}

void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
   
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    for(int i = 0; i < LIGHT_COUNT; i++) {
        gpio_reset_pin(my_lights[i].red);
        gpio_set_direction(my_lights[i].red, GPIO_MODE_OUTPUT);
        gpio_reset_pin(my_lights[i].yellow);
        gpio_set_direction(my_lights[i].yellow, GPIO_MODE_OUTPUT);
        gpio_reset_pin(my_lights[i].green);
        gpio_set_direction(my_lights[i].green, GPIO_MODE_OUTPUT);
    }

    esp_now_init();

    esp_now_register_recv_cb(on_receive);
    esp_now_register_send_cb(on_sent);

    esp_now_peer_info_t broadcast_info = { .channel = 0, .encrypt = false };
    memset(broadcast_info.peer_addr, 0xFF, 6);
    esp_now_add_peer(&broadcast_info);
    
    if (esp_now_add_peer(&broadcast_info) != ESP_OK) {
        printf("Failed to add peer\n");
        return;
    }
}   