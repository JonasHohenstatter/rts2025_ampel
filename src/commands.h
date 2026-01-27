#include <stdint.h>
#include "driver/gpio.h"

typedef struct {
    gpio_num_t red;
    gpio_num_t yellow;
    gpio_num_t green;
    uint8_t group;
} ampel_config_t;

typedef struct {
    gpio_num_t button;
    uint8_t group;
} button_config_t;

typedef struct {
    uint8_t peer_mac[6];
} peer_t;

typedef enum {
    STATE_INIT,
    STATE_RUN,
    STATE_NEGOTIATE,
    STATE_MASTER,
    STATE_SLAVE,
} ampel_state_t;

typedef enum {
    ROLE_IDLE,
    ROLE_MASTER,
    ROLE_SLAVE
} role_t;

