#ifndef _BUSLOGIC_H_
#define _BUSLOGIC_H_

#include <stdint.h>
#include "../chipset/dma.h"
#include "../chipset/i8259.h"

#define BUSLOGIC_MAX_TARGETS 7
#define BUSLOGIC_TARGET_NONE 0
#define BUSLOGIC_TARGET_DISK 1
#define BUSLOGIC_TARGET_CDROM 2

typedef struct {
    uint8_t present;
    uint8_t type;
    char path[512];
} BUSLOGIC_TARGET_t;

typedef struct {
    CPU_t *cpu;
    DMA_t *dma;
    I8259_t *pic_master;
    I8259_t *pic_slave;
    uint16_t base;
    uint8_t irq;
    uint8_t dma_channel;
    uint32_t bios_addr;
    const char *rom_path;
    const char *nvr_path;
} BUSLOGIC_CONFIG_t;

typedef struct BUSLOGIC_s BUSLOGIC_t;

int buslogic_init(BUSLOGIC_t **out, const BUSLOGIC_CONFIG_t *config, const BUSLOGIC_TARGET_t targets[BUSLOGIC_MAX_TARGETS]);

#endif
