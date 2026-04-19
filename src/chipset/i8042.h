#ifndef I8042_H
#define I8042_H

#include <stdint.h>
#include "../cpu/cpu.h"
#include "i8259.h"
#include "../input/kbc_at_device.h"
#include "../input/keyboard_at.h"

enum {
    KBC_STAT_PARITY = 0x80,
    KBC_STAT_RTIMEOUT = 0x40,
    KBC_STAT_TTIMEOUT = 0x20,
    KBC_STAT_MFULL = 0x20,
    KBC_STAT_UNLOCKED = 0x10,
    KBC_STAT_CD = 0x08,
    KBC_STAT_SYSFLAG = 0x04,
    KBC_STAT_IFULL = 0x02,
    KBC_STAT_OFULL = 0x01
};

enum {
    KBC_CCB_TRANSLATE = 0x40,
    KBC_CCB_PCMODE = 0x20,
    KBC_CCB_ENABLEKBD = 0x10,
    KBC_CCB_IGNORELOCK = 0x08,
    KBC_CCB_SYSTEM = 0x04,
    KBC_CCB_ENABLEMINT = 0x02,
    KBC_CCB_ENABLEKINT = 0x01
};

enum {
    KBC_FLAG_CLOCK = 0x01,
    KBC_FLAG_CACHE = 0x02,
    KBC_FLAG_PS2_MODE = 0x04
};

typedef enum {
    KBC_VENDOR_GENERIC = 0,
    KBC_VENDOR_AMI
} KBC_VENDOR_t;

enum {
    KBC_PROFILE_FLAG_PS2_CAPABLE = 0x01,
    KBC_PROFILE_FLAG_AMIKEY2 = 0x02,
    KBC_PROFILE_FLAG_PS2_DEFAULT = 0x04,
    KBC_PROFILE_FLAG_PCI = 0x08
};

typedef struct {
    KBC_VENDOR_t vendor;
    uint8_t revision;
    uint8_t flags;
} KBC_PROFILE_t;

typedef enum {
    KBC_STATE_RESET = 0,
    KBC_STATE_DELAY_OUT,
    KBC_STATE_AMI_OUT,
    KBC_STATE_MAIN_IBF,
    KBC_STATE_MAIN_KBD,
    KBC_STATE_MAIN_AUX,
    KBC_STATE_MAIN_BOTH,
    KBC_STATE_CTRL_OUT,
    KBC_STATE_PARAM,
    KBC_STATE_SEND_KBD,
    KBC_STATE_SCAN_KBD,
    KBC_STATE_SEND_AUX,
    KBC_STATE_SCAN_AUX
} KBC_STATE_t;

typedef struct {
    KBC_STATE_t state;
    KBC_PROFILE_t profile;
    CPU_t* cpu;
    I8259_t* pic;
    I8259_t* pic_aux;
    uint8_t command;
    uint8_t command_phase;
    uint8_t status;
    uint8_t wantdata;
    uint8_t ib;
    uint8_t ob;
    uint8_t sc_or;
    uint8_t mem_addr;
    uint8_t p1;
    uint8_t p2;
    uint8_t old_p2;
    uint8_t misc_flags;
    uint8_t ami_flags;
    uint8_t key_ctrl_queue_start;
    uint8_t key_ctrl_queue_end;
    uint8_t val;
    uint8_t channel;
    uint8_t stat_hi;
    uint8_t pending;
    uint8_t pulse_pending;
    uint8_t pulse_mask;
    uint8_t ami_is_amikey_2;
    uint8_t mem[0x100];
    uint8_t key_ctrl_queue[64];
    uint32_t poll_timer;
    uint32_t dev_poll_timer;
    uint32_t pulse_timer;
    kbc_at_port_t ports[2];
    keyboard_at_t keyboard;
} i8042_t;

extern i8042_t kbc;

void i8042_init(CPU_t* cpu, I8259_t* pic, I8259_t* pic_aux, KBC_PROFILE_t profile);
void i8042_reset(i8042_t* dev);
void i8042_handle_host_key(i8042_t* dev, uint16_t scan, uint8_t down);

#endif
