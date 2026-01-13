#include <stdint.h>
#include "driver/gpio.h"

typedef enum {
    CMD_TIME,
    CMD_GREEN
} cmd_t;

typedef enum {
    ROLE_IDLE = 0,
    ROLE_MASTER,
    ROLE_SLAVE
} esp_role_t;

typedef struct __attribute__((packed)) {
    cmd_t type;
    int64_t timestamp;
    char* group;
} protocol_msg_t;

typedef struct {
    gpio_num_t red;
    gpio_num_t yellow;
    gpio_num_t green;
} ampel_config_t;


