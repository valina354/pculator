#ifndef KEYBOARD_AT_H
#define KEYBOARD_AT_H

#include <stdint.h>
#include "kbc_at_device.h"

typedef struct keyboard_at_s {
    kbc_at_device_t dev;
    uint8_t scan_enabled;
    uint8_t mode;
    uint8_t rate;
    uint8_t led_state;
    uint8_t shift_state;
    uint8_t set3_flags[256];
    uint8_t set3_all_repeat;
    uint8_t set3_all_break;
    uint8_t inv_cmd_response;
    uint8_t in_reset;
    uint8_t pressed[512];
    uint16_t bat_counter;
} keyboard_at_t;

void keyboard_at_init(keyboard_at_t* keyboard, kbc_at_port_t* port);
void keyboard_at_reset(keyboard_at_t* keyboard, int do_fa);
void keyboard_at_handle_host_key(keyboard_at_t* keyboard, uint16_t scan, uint8_t down);

#endif
