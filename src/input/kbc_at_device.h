#ifndef KBC_AT_DEVICE_H
#define KBC_AT_DEVICE_H

#include <stdint.h>

enum {
    DEV_STATE_MAIN_1 = 0,
    DEV_STATE_MAIN_OUT,
    DEV_STATE_MAIN_2,
    DEV_STATE_MAIN_CMD,
    DEV_STATE_MAIN_WANT_IN,
    DEV_STATE_MAIN_IN,
    DEV_STATE_EXECUTE_BAT,
    DEV_STATE_MAIN_WANT_EXECUTE_BAT
};

typedef struct kbc_at_port_s {
    uint8_t wantcmd;
    uint8_t dat;
    int16_t out_new;
    void* priv;
    void (*poll)(void* priv);
} kbc_at_port_t;

typedef struct kbc_at_device_s {
    const char* name;
    uint8_t command;
    uint8_t last_scan_code;
    uint8_t state;
    uint8_t resolution;
    uint8_t rate;
    uint8_t cmd_queue_start;
    uint8_t cmd_queue_end;
    uint8_t queue_start;
    uint8_t queue_end;
    uint8_t flags;
    uint8_t cmd_queue[16];
    uint8_t queue[64];
    int fifo_mask;
    int ignore;
    uint8_t* scan;
    void (*process_cmd)(void* priv);
    void (*execute_bat)(void* priv);
    kbc_at_port_t* port;
} kbc_at_device_t;

void kbc_at_dev_queue_reset(kbc_at_device_t* dev, uint8_t reset_main);
uint8_t kbc_at_dev_queue_pos(kbc_at_device_t* dev, uint8_t main);
void kbc_at_dev_queue_add(kbc_at_device_t* dev, uint8_t val, uint8_t main);
void kbc_at_dev_poll(void* priv);
void kbc_at_dev_reset(kbc_at_device_t* dev, int do_fa);
void kbc_at_dev_init(kbc_at_device_t* dev, kbc_at_port_t* port);

#endif
