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

#define RED GPIO_NUM_23
#define YELLOW GPIO_NUM_22
#define GREEN GPIO_NUM_18

typedef struct {
    int64_t master_time;
    uint8_t command;
} sync_msg_t;

void blink_yellow() {
    gpio_set_level(YELLOW, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(YELLOW, 0);  
    vTaskDelay(pdMS_TO_TICKS(1000));
}


uint8_t peer_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t peer = 0;
int cycle_counter = 4;

void ampel_cycle() {
    gpio_set_level(GREEN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(GREEN, 0);    
    if(cycle_counter-- <= 0) {
        peer = 0;
        return;
    }   
    gpio_set_level(YELLOW, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(YELLOW, 0);
    if(cycle_counter-- <= 0) {
        peer = 0;
        return;
    }
    gpio_set_level(RED, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(RED, 0);
    if(cycle_counter-- <= 0) {
        peer = 0;
        return;
    }
    esp_now_send(peer_mac, (uint8_t *) "KEEPALIVE", 9);
}

void on_sent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
    
}

void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    printf("Empfangen: %.*s\n", len, data);
    if (peer == 0 && len == 5 && memcmp(data, "Slave", 5) == 0) {
        if (!esp_now_is_peer_exist(info->src_addr)) {
            esp_now_peer_info_t slave_info = {};
            memcpy(slave_info.peer_addr, info->src_addr, 6);
            slave_info.channel = 0;
            slave_info.encrypt = false;
            esp_now_add_peer(&slave_info);
        }
        memcpy(peer_mac, info->src_addr, 6);

        char msg_buffer[32];
        int64_t now = esp_timer_get_time();
        int len = snprintf(msg_buffer, sizeof(msg_buffer), "Master,%lli", now);

        cycle_counter = 4;

        esp_now_send(peer_mac, (uint8_t *)msg_buffer, len);
    }
    if(peer == 0 && len == 3 && memcmp(data, "ACK", 3) == 0){
        peer = 1;
    }
    if(peer == 1 && len == 9 && memcmp(data, "KEEPALIVE", 9) == 0){
        cycle_counter = 4;
    }
}

void app_main(void) {
    gpio_reset_pin(GREEN);
    gpio_reset_pin(YELLOW);
    gpio_reset_pin(RED);
    gpio_set_direction(GREEN, GPIO_MODE_OUTPUT);
    gpio_set_direction(YELLOW, GPIO_MODE_OUTPUT);
    gpio_set_direction(RED, GPIO_MODE_OUTPUT);

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

    uint8_t my_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, my_mac);
    printf("\n------------------------------\n");
    printf("MY MAC: " MACSTR "\n", MAC2STR(my_mac));
    printf("------------------------------\n\n");
    esp_now_init();

    esp_now_register_recv_cb(on_receive);
    esp_now_register_send_cb(on_sent);

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, peer_mac, 6);
    peer_info.channel = 0;
    peer_info.encrypt = false;
    
    if (esp_now_add_peer(&peer_info) != ESP_OK) {
        printf("Failed to add peer\n");
        return;
    }

    while(1) {
        if(peer == 0) {
            blink_yellow();
        }
        else {
            char msg_buffer[32];
            int64_t offset = 2000000;
            int64_t now = esp_timer_get_time();
            int len = snprintf(msg_buffer, sizeof(msg_buffer), "START,%lli", now + offset);

            esp_now_send(peer_mac, (uint8_t *)msg_buffer, len);

            while(esp_timer_get_time() < (now + offset)) {
                continue;
            }

            while(peer == 1) {
                ampel_cycle();
            }
        }
    }
}