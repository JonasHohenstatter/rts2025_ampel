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
#include "tm1637.h"

// Channel has to be set to a fixed value

ampel_state_t state = STATE_INIT;
ampel_config_t my_lights[] = {
    { .red = GPIO_NUM_18, .yellow = GPIO_NUM_22, .green = GPIO_NUM_23, 0 },
    { .red = GPIO_NUM_13, .yellow = GPIO_NUM_12, .green = GPIO_NUM_14 ,0 },
};

#define SETUP_ID 1
#define SETUP_SIZE 4

#define CLK_PIN 18
#define DIO_PIN 19

#define Seconds * 1000000

role_t role = ROLE_IDLE;

peer_t peers[SETUP_SIZE - 1];
size_t peer_count = 0;
size_t PEERS_NEEDED = 1;
uint8_t master_mac[6];
int64_t master_keepalive = 5;

int64_t next_event = 0;

tm1637_led_t *display;

// TODO: this isn't really accounting for delay, redo
int64_t master_offset = 0;

#define LIGHT_COUNT (sizeof(my_lights) / sizeof(ampel_config_t))

typedef struct {
    char command;
    uint8_t group;
    int64_t timestamp;
} switchParams_t;

void show_time_count() {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if(next_event == 0) {
            continue;
        }
        
        int64_t delta = next_event - esp_timer_get_time();


        int64_t remaining_seconds = (delta / 1000000);

        printf("%lli", remaining_seconds);

        if(remaining_seconds < 0) {
            remaining_seconds = 0;
        }
        tm1637_set_number(display, remaining_seconds, false, 0);
    }
}

void switch_lights(void *pvParameters) {
    switchParams_t *params = (switchParams_t *) pvParameters;
    char command = params->command;
    uint8_t group = params->group;
    int64_t timestamp = params->timestamp;

    next_event = timestamp;

    vTaskDelay(pdMS_TO_TICKS(10)); // small delay to ensure task setup

    int64_t now_master = esp_timer_get_time() - master_offset;
    while (now_master < timestamp) {
        int64_t diff_us = timestamp - now_master;
        int64_t diff_ms = diff_us / 1000;
        if (diff_ms < 1) diff_ms = 1;
        if (diff_ms > 200) diff_ms = 200;

        vTaskDelay(pdMS_TO_TICKS((uint32_t)diff_ms));
        now_master = esp_timer_get_time() - master_offset;
    }
    
    // Case switch to Green
    if (command == 'G') {
        printf("Switching to Green.\n");
        for (int i = 0; i < LIGHT_COUNT; i++) {
            if (my_lights[i].group != group) continue;
            
            gpio_set_level(my_lights[i].red, 1);
            gpio_set_level(my_lights[i].yellow, 1);
            gpio_set_level(my_lights[i].green, 0);
            }
        vTaskDelay(pdMS_TO_TICKS(1000));
        for (int i = 0; i < LIGHT_COUNT; i++) {
            if (my_lights[i].group != group) continue;
            
            gpio_set_level(my_lights[i].red, 0);
            gpio_set_level(my_lights[i].yellow, 0);
            gpio_set_level(my_lights[i].green, 1);
            }
        vTaskDelay(pdMS_TO_TICKS(10));
        }
    // Case switch to Red
    else {
        printf("Switching to Red.\n");
        for (int i = 0; i < LIGHT_COUNT; i++) {
            if (my_lights[i].group != group) continue;
            gpio_set_level(my_lights[i].red, 0);
            gpio_set_level(my_lights[i].yellow, 1);
            gpio_set_level(my_lights[i].green, 0);
            }
        vTaskDelay(pdMS_TO_TICKS(1000));
        for (int i = 0; i < LIGHT_COUNT; i++) {
            if (my_lights[i].group != group) continue;
            
            gpio_set_level(my_lights[i].red, 1);
            gpio_set_level(my_lights[i].yellow, 0);
            gpio_set_level(my_lights[i].green, 0);
            }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    free(params);
    vTaskDelete(NULL);
}

void send_command(uint8_t mac[6], char command, uint8_t group, int64_t timestamp) {
    char buffer[64];


    snprintf(buffer, sizeof(buffer), "COMMAND;%c;GROUP%d;%lli;%d", command, group, timestamp, SETUP_ID);

    for(int i = 0; i < peer_count; i++) {
         esp_now_send(peers[i].peer_mac, (uint8_t *) buffer, 64);
    }

    switchParams_t *p = malloc(sizeof(switchParams_t));
    if (p != NULL) {
        p->command = command;
        p->group = group;
        p->timestamp = timestamp;

        xTaskCreate(
            switch_lights,      
            "LightSwitchTask",  
            4096,    
            p,          // Pass the heap pointer
            1,   
            NULL
        );
    }
}

void keepalive() {
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if(state == STATE_RUN) {
            if(role == ROLE_MASTER) {
                for(int i = 0; i < peer_count; i++) {
                    char buffer[64];
                    snprintf(buffer, sizeof(buffer), "KEEPALIVE");
                    printf("Master: Sending keepalives.\n");
                    if(esp_now_send(peers[i].peer_mac, (uint8_t *) buffer, 64) != ESP_OK) {
                        for(int i = 0; i < peer_count; i++) {
                            esp_now_del_peer(peers[i].peer_mac);
                        }
                        peer_count = 0;
                        printf("Master: Keepalive failed, resetting.\n");
                        state = STATE_INIT;
                        break;
                    };
                }
            }
            else if(role == ROLE_SLAVE) {
                if(master_keepalive-- <= 0) {
                    printf("Slave: Haven't received keepalive for a while, resetting.\n");
                    state = STATE_INIT;
                    esp_now_del_peer(master_mac);
                }
            }
        }
    }
}


void on_sent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
    if(status == ESP_NOW_SEND_FAIL) {
        for(int i = 0; i < peer_count; i++) {
            esp_now_del_peer(peers[i].peer_mac);
        }
        peer_count = 0;
        state = STATE_INIT;
    }
}

void on_receive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    char received_str[64];
    int copy_len = (len < sizeof(received_str) - 1) ? len : sizeof(received_str) - 1;
    memcpy(received_str, data, copy_len);
    received_str[copy_len] = '\0';

    printf("Länge: %d | Nachricht: \"%s\"\n",len, received_str);

    if(role == ROLE_SLAVE){
        char expected_str[64];

        snprintf(expected_str, sizeof(expected_str), "MASTER;JOINME;%d", SETUP_ID);

        if (strcmp(received_str, expected_str) == 0) {
            printf("Received JoinMe\n");
            if(memcmp(master_mac, info->src_addr, 6) != 0) return;
            if(esp_now_is_peer_exist(info->src_addr)) return;

            esp_now_peer_info_t new_peer = { .channel = 1, .encrypt = false };
            memcpy(new_peer.peer_addr, info->src_addr, 6);
            esp_now_add_peer(&new_peer);

            char buffer[64];
            snprintf(buffer, sizeof(buffer), "SLAVE;JOIN;%d", SETUP_ID);
            printf("Slave: Sent Master a Join Request.");
            esp_now_send(master_mac, (uint8_t *) buffer, 64);
            return;
        }

        snprintf(expected_str, sizeof(expected_str), "KEEPALIVE");

        if (strcmp(received_str, expected_str) == 0) {
            master_keepalive = 5;
            return;
        }


        if(memcmp(received_str, "TIME_SYNC;", 10) == 0) {
            printf("Received Time Sync.\n");
            int64_t master_time = 0;
            int setup_id = 0;

            int parsed = sscanf(received_str, "TIME_SYNC;%lli;%d", &master_time, &setup_id);

            if(parsed == 2 && setup_id == SETUP_ID) {
                master_offset = esp_timer_get_time() - master_time;
                state = STATE_RUN;
            }
            else {
                state = STATE_INIT;
                esp_now_del_peer(master_mac);
            }
            return;
        }

        if(memcmp(received_str, "COMMAND;", 8) == 0) {
            printf("COMMANDO!!!\n");
            char command; uint8_t group; int64_t timestamp; int setup_id;

            int parsed = sscanf(received_str, "COMMAND;%c;GROUP%hhd;%lli;%d", &command, &group, &timestamp, &setup_id);

            if(parsed == 4 && setup_id == SETUP_ID) {
                // 1. Speicher dynamisch reservieren (Heap)
                // Jeder Task bekommt so seine EIGENE Kopie der Daten
                switchParams_t *p = malloc(sizeof(switchParams_t));
                
                if (p != NULL) {
                    p->command = command;
                    p->group = group;
                    p->timestamp = timestamp;

                    xTaskCreate(
                        switch_lights,      
                        "LightSwitchTask",  
                        4096,    
                        p,          // Wir übergeben den Pointer auf den reservierten Speicher
                        1,   
                        NULL
                    );
                }
            }
        }

        
    } 
    else {
        char expected_str[64];
        snprintf(expected_str, sizeof(expected_str), "SLAVE;JOIN;%d", SETUP_ID);

        if (strcmp(received_str, expected_str) == 0) {
            printf("SLAVE JOINED\n");
            for(int i = 0; i < peer_count; i++) {
                if(memcmp(peers[i].peer_mac, info->src_addr, 6) == 0) {
                    return;
                }
            }
            memcpy(peers[peer_count].peer_mac, info->src_addr, 6);
            
            esp_now_peer_info_t new_peer = { .channel = 1, .encrypt = false };
            memcpy(new_peer.peer_addr, info->src_addr, 6);
            esp_now_add_peer(&new_peer);

            peer_count++;
            return;
        }
    }
}

void app_main(void) {
    display = tm1637_init(CLK_PIN, DIO_PIN);

    // 2. Helligkeit einstellen (0 = sehr dunkel, 7 = sehr hell)
    tm1637_set_brightness(display, 5);

    // 3. Optional: Kurzer Start-Test (zeigt 0000)
    tm1637_set_number(display, 15, true, 0x00); 
    vTaskDelay(pdMS_TO_TICKS(500));

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

    esp_wifi_set_ps(WIFI_PS_NONE);

    uint8_t target_channel = 1; 
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(target_channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);

    uint8_t my_mac[6];
    ret = esp_read_mac(my_mac, ESP_MAC_WIFI_STA);

    printf("Meine MAC-Adresse: %02X:%02X:%02X:%02X:%02X:%02X\n",
    my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);
    
    uint8_t temp_mac[] = {0xA4, 0xF0, 0x0F, 0x5E, 0xE4, 0xA0};
    memcpy(master_mac, temp_mac, 6);

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

    esp_now_peer_info_t broadcast_info = { .channel = 1, .encrypt = false };
    memset(broadcast_info.peer_addr, 0xFF, 6);
    
    if (esp_now_add_peer(&broadcast_info) != ESP_OK) {
        printf("Failed to add peer\n");
        return;
    }

    // STARTUP IMAGE
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("AMPEL SYSTEM starting up.\n");
    printf("::::::::::::::::::::::::\n");
    printf("::::::::::::::::::::::::\n");
    printf("::::--*#++*=*=*+%%:::::::\n");
    printf(":..+@@@@@@@%%*##*+*#=:::=\n");
    printf("..%%@@@@+==-----=*%%%%#::::\n");
    printf("...%%@@*+==-----==+@+::::\n");
    printf("....:=@%%%%@@@####@**:.::::\n");
    printf("......==-=+=+==+==*-..::\n");
    printf("......==*=#**+==++-....:\n");
    printf("......+**+##%%@##*=......\n");
    printf(".......%%*+**=*%%%%@%%.....\n");
    printf(".:.:::@@+@#%%%%@@*=@%%-.:==\n");
    printf(".::::@@@+++*#*+@%%@@:.:+:\n");
    printf("#@@@@@@@@@+++@@@@@@@@%%--\n");
    printf("@@@@@@@@@@@@@@@@@@@@@@@@\n");
    printf("@@@@@%%@@@*%%@@@@@@@@@@@@@\n");
    printf("@@@@@@@@*===@@@@@@@@@@@@\n");
    printf("@@@@@#@@@%%@@@@@@@@@@@@@@\n");
    printf("@@@@@:@@%%@@%%@@@@@@@@@@@@\n");
    printf("@@@@@-@@@@%%@@@@@@@@@@@@@\n");


    xTaskCreate(
        keepalive,      
        "Task Name",  
        4096,    
        NULL, 
        1,   
        NULL
    );

    if(memcmp(my_mac, master_mac, 6) == 0) {
        role = ROLE_MASTER;
        xTaskCreate(
            show_time_count,
            "Task Name",  
            4096,    
            NULL, 
            1,   
            NULL
        );
    }
    else {
        role = ROLE_SLAVE;
    }

    state = STATE_INIT;

    int killswitch = 7;

    // known bug: restart in init phase lets others progress
    // keepalives are just checked for successful sent, not for state of slave
    while(1) {
        switch(state) {
            case STATE_INIT:
                master_offset = 0;
                master_keepalive = 5;
                
                printf("INIT_STATE\n");
                for (int i = 0; i < LIGHT_COUNT; i++) {
                    gpio_set_level(my_lights[i].red, 0);
                    gpio_set_level(my_lights[i].yellow, 0);
                    gpio_set_level(my_lights[i].green, 0);
                }
                if(role == ROLE_MASTER) {
                    while(peer_count < (SETUP_SIZE - 1)) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        printf("Master: SUCHE PEERS\n");
                        char buffer[64];
                        snprintf(buffer, sizeof(buffer), "MASTER;JOINME;%d", SETUP_ID);
                        esp_now_send(broadcast_info.peer_addr, (uint8_t *) buffer, 64);
                        for(int i = 0; i < LIGHT_COUNT; i++) {
                            gpio_set_level(my_lights[i].yellow, 1);
                        }
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        for(int i = 0; i < LIGHT_COUNT; i++) {
                            gpio_set_level(my_lights[i].yellow, 0);
                        }
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                    printf("HAB ALLE\n");
                }
                else if(role == ROLE_SLAVE) {
                    printf("SUCHE PAPA\n"); 
                    for(int i = 0; i < LIGHT_COUNT; i++) {
                        gpio_set_level(my_lights[i].yellow, 1);
                    }
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    for(int i = 0; i < LIGHT_COUNT; i++) {
                        gpio_set_level(my_lights[i].yellow, 0);
                    }
                }
                
                if(role == ROLE_MASTER) {
                    char msg_buffer[64];
                    int64_t now = esp_timer_get_time();

                    snprintf(msg_buffer, sizeof(msg_buffer), "TIME_SYNC;%lli;%d", now, SETUP_ID);

                    for(int i = 0; i < peer_count; i++) {
                        esp_now_send(peers[i].peer_mac, (uint8_t *)msg_buffer, 64);
                    }
                    
                    state = STATE_RUN;
                }

                killswitch -= 1;
                if(killswitch <= 0) {
                    printf("KABUTT\n");
                    printf("%d Killswitch", killswitch);
                    esp_restart();
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case STATE_RUN:
                if(role == ROLE_MASTER) {
                    printf("Meister\n");
                    int64_t when = 20000000;
                    int64_t ts = esp_timer_get_time() + when;

                    send_command(broadcast_info.peer_addr, 'G', 0, ts);
                    send_command(broadcast_info.peer_addr, 'R', 1, ts);

                    vTaskDelay(pdMS_TO_TICKS(when / 1000));

                    when = 10000000;
                    ts = esp_timer_get_time() + when;

                    send_command(broadcast_info.peer_addr, 'R', 0, ts + 10000000);       
                    send_command(broadcast_info.peer_addr, 'G', 1, ts + 10000000);

                    vTaskDelay(pdMS_TO_TICKS(when / 1000));
                }
                else {
                    printf("SLAVE\n");
                     vTaskDelay(pdMS_TO_TICKS(1000));
                }
               
                break;
            default:
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
        }
        
    }
    
}   