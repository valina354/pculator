#ifndef _DMA_H_
#define _DMA_H_

#include <stdint.h>
#include "../cpu/cpu.h"
#include "i8257.h"

typedef struct DMA_s {
    CPU_t *cpu;
    I8257State *low;
    I8257State *high;
    uint8_t page_latch[16];
    uint8_t pageh_latch[16];
} DMA_t;

void dma_init(DMA_t *dma, CPU_t *cpu);
void dma_register_channel(DMA_t *dma, uint8_t ch, IsaDmaTransferHandler transfer_handler, void *opaque);
uint8_t dma_channel_read(DMA_t *dma, uint8_t ch);
void dma_channel_write(DMA_t *dma, uint8_t ch, uint8_t value);
int dma_channel_remaining(DMA_t *dma, uint8_t ch);
int dma_channel_read_memory(DMA_t *dma, uint8_t ch, void *buf, int pos, int len);
int dma_channel_write_memory(DMA_t *dma, uint8_t ch, void *buf, int pos, int len);
void dma_hold_dreq(DMA_t *dma, uint8_t ch);
void dma_release_dreq(DMA_t *dma, uint8_t ch);
void dma_bm_read(DMA_t *dma, uint32_t phys_addr, uint8_t *data, uint32_t total_size, int transfer_size);
void dma_bm_write(DMA_t *dma, uint32_t phys_addr, const uint8_t *data, uint32_t total_size, int transfer_size);

#endif
