#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../debuglog.h"
#include "../memory.h"
#include "../ports.h"
#include "dma.h"

#define ADDR 0
#define COUNT 1

static uint8_t dma_port_read(void *opaque, uint16_t port);
static void dma_port_write(void *opaque, uint16_t port, uint8_t value);
static I8257State *dma_state_for_channel(DMA_t *dma, uint8_t ch, int *ichan);
static int dma_page_port_valid(uint16_t port);

static int dma_page_port_valid(uint16_t port)
{
    switch (port & 0x07) {
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x07:
        return 1;
    default:
        return 0;
    }
}

static uint8_t dma_port_read(void *opaque, uint16_t port)
{
    DMA_t *dma = (DMA_t *) opaque;

    if (port < 0x10) {
        if (port < 0x08) {
            return (uint8_t) i8257_read_chan(dma->low, port, 1);
        }

        return (uint8_t) i8257_read_cont(dma->low, port - 0x08, 1);
    }

    if ((port >= 0x80) && (port < 0x90)) {
        return dma->page_latch[port - 0x80];
    }

    if ((port >= 0x480) && (port < 0x490)) {
        return dma->pageh_latch[port - 0x480];
    }

    if ((port >= 0xC0) && (port < 0xE0)) {
        if ((port & 0x10) == 0) {
            return (uint8_t) i8257_read_chan(dma->high, port - 0xC0, 1);
        }

        return (uint8_t) i8257_read_cont(dma->high, port - 0xD0, 1);
    }

    return 0xff;
}

static void dma_port_write(void *opaque, uint16_t port, uint8_t value)
{
    DMA_t *dma = (DMA_t *) opaque;

    if (port < 0x10) {
        if (port < 0x08) {
            i8257_write_chan(dma->low, port, value, 1);
        } else {
            i8257_write_cont(dma->low, port - 0x08, value, 1);
        }
        return;
    }

    if ((port >= 0x80) && (port < 0x90)) {
        dma->page_latch[port - 0x80] = value;
        if (dma_page_port_valid(port)) {
            if ((port & 0x08) != 0) {
                i8257_write_page(dma->high, port, value);
            } else {
                i8257_write_page(dma->low, port, value);
            }
        }
        return;
    }

    if ((port >= 0x480) && (port < 0x490)) {
        dma->pageh_latch[port - 0x480] = value;
        if (dma_page_port_valid(port)) {
            if ((port & 0x08) != 0) {
                i8257_write_pageh(dma->high, port, value);
            } else {
                i8257_write_pageh(dma->low, port, value);
            }
        }
        return;
    }

    if ((port >= 0xC0) && (port < 0xE0)) {
        if ((port & 0x10) == 0) {
            i8257_write_chan(dma->high, port - 0xC0, value, 1);
        } else {
            i8257_write_cont(dma->high, port - 0xD0, value, 1);
        }
    }
}

static I8257State *dma_state_for_channel(DMA_t *dma, uint8_t ch, int *ichan)
{
    if (ch < 4) {
        if (ichan != NULL) {
            *ichan = ch;
        }
        return dma->low;
    }

    if (ichan != NULL) {
        *ichan = ch - 4;
    }
    return dma->high;
}

static uint32_t dma_current_address(I8257State *state, int ichan)
{
    I8257Regs *reg = &state->regs[ichan];
    int dir = ((reg->mode >> 5) & 1) ? -1 : 1;

    return ((uint32_t) (reg->pageh & 0x7f) << 24) |
        ((uint32_t) reg->page << 16) |
        (uint32_t) (reg->now[ADDR] + (reg->now[COUNT] * dir));
}

static uint32_t dma_total_length(I8257State *state, int ichan)
{
    return ((uint32_t) state->regs[ichan].base[COUNT] + 1u) << state->dshift;
}

static void dma_commit_position(I8257State *state, int ichan, uint32_t new_pos)
{
    I8257Regs *reg = &state->regs[ichan];
    uint32_t total = dma_total_length(state, ichan);

    if (new_pos > total) {
        new_pos = total;
    }

    reg->now[COUNT] = (int) new_pos;
    if (new_pos >= total) {
        state->status |= (uint8_t) (1u << ichan);
        if ((reg->mode & 0x10) != 0) {
            reg->now[COUNT] = 0;
        }
    }
}

static void dma_advance_channel(I8257State *state, int ichan, int len)
{
    I8257Regs *reg = &state->regs[ichan];

    dma_commit_position(state, ichan, (uint32_t) reg->now[COUNT] + (uint32_t) len);
}

void dma_init(DMA_t *dma, CPU_t *cpu)
{
    memset(dma, 0, sizeof(*dma));
    dma->cpu = cpu;
    dma->low = i8257_new(NULL, 0, 0x00, 0x80, 0x480, 0);
    dma->high = i8257_new(NULL, 0, 0xC0, 0x88, 0x488, 1);
    i8257_attach_cpu(dma->low, cpu);
    i8257_attach_cpu(dma->high, cpu);

    ports_cbRegister(0x00, 0x10, dma_port_read, NULL, dma_port_write, NULL, dma);
    ports_cbRegister(0x80, 0x10, dma_port_read, NULL, dma_port_write, NULL, dma);
    ports_cbRegister(0x480, 0x10, dma_port_read, NULL, dma_port_write, NULL, dma);
    ports_cbRegister(0xC0, 0x20, dma_port_read, NULL, dma_port_write, NULL, dma);
}

void dma_register_channel(DMA_t *dma, uint8_t ch, IsaDmaTransferHandler transfer_handler, void *opaque)
{
    int ichan;
    I8257State *state = dma_state_for_channel(dma, ch, &ichan);

    i8257_dma_register_channel(state, ichan, transfer_handler, opaque);
}

uint8_t dma_channel_read(DMA_t *dma, uint8_t ch)
{
    int ichan;
    I8257State *state = dma_state_for_channel(dma, ch, &ichan);
    I8257Regs *reg = &state->regs[ichan];
    uint32_t total = dma_total_length(state, ichan);
    uint32_t addr;
    uint8_t value;

    if ((uint32_t) reg->now[COUNT] >= total) {
        return 0;
    }

    addr = dma_current_address(state, ichan);
    value = memory_read_phys_u8(dma->cpu, addr);
    dma_advance_channel(state, ichan, 1);
    return value;
}

void dma_channel_write(DMA_t *dma, uint8_t ch, uint8_t value)
{
    int ichan;
    I8257State *state = dma_state_for_channel(dma, ch, &ichan);
    I8257Regs *reg = &state->regs[ichan];
    uint32_t addr = dma_current_address(state, ichan);
    uint32_t total = dma_total_length(state, ichan);

    if ((uint32_t) reg->now[COUNT] >= total) {
        return;
    }

    memory_write_phys_u8(dma->cpu, addr, value);
    dma_advance_channel(state, ichan, 1);
}

int dma_channel_remaining(DMA_t *dma, uint8_t ch)
{
    int ichan;
    I8257State *state = dma_state_for_channel(dma, ch, &ichan);
    I8257Regs *reg = &state->regs[ichan];
    uint32_t total = dma_total_length(state, ichan);
    uint32_t pos = (uint32_t) reg->now[COUNT];

    if (pos >= total) {
        return 0;
    }

    return (int) (total - pos);
}

int dma_channel_read_memory(DMA_t *dma, uint8_t ch, void *buf, int pos, int len)
{
    int ichan;
    I8257State *state = dma_state_for_channel(dma, ch, &ichan);

    return i8257_dma_read_memory(state, ichan, buf, pos, len);
}

int dma_channel_write_memory(DMA_t *dma, uint8_t ch, void *buf, int pos, int len)
{
    int ichan;
    I8257State *state = dma_state_for_channel(dma, ch, &ichan);

    return i8257_dma_write_memory(state, ichan, buf, pos, len);
}

void dma_hold_dreq(DMA_t *dma, uint8_t ch)
{
    int ichan;
    I8257State *state = dma_state_for_channel(dma, ch, &ichan);

    i8257_dma_hold_DREQ(state, ichan);
}

void dma_release_dreq(DMA_t *dma, uint8_t ch)
{
    int ichan;
    I8257State *state = dma_state_for_channel(dma, ch, &ichan);

    i8257_dma_release_DREQ(state, ichan);
}

void dma_bm_read(DMA_t *dma, uint32_t phys_addr, uint8_t *data, uint32_t total_size, int transfer_size)
{
    uint32_t aligned;
    uint32_t tail;

    if (transfer_size <= 0) {
        transfer_size = 1;
    }

    aligned = total_size & ~((uint32_t) transfer_size - 1u);
    tail = total_size - aligned;

    for (uint32_t i = 0; i < aligned; i += (uint32_t) transfer_size) {
        for (int j = 0; j < transfer_size; j++) {
            data[i + (uint32_t) j] = memory_read_phys_u8(dma->cpu, phys_addr + i + (uint32_t) j);
        }
    }

    if (tail != 0) {
        for (uint32_t j = 0; j < tail; j++) {
            data[aligned + j] = memory_read_phys_u8(dma->cpu, phys_addr + aligned + j);
        }
    }
}

void dma_bm_write(DMA_t *dma, uint32_t phys_addr, const uint8_t *data, uint32_t total_size, int transfer_size)
{
    uint32_t aligned;
    uint32_t tail;

    if (transfer_size <= 0) {
        transfer_size = 1;
    }

    aligned = total_size & ~((uint32_t) transfer_size - 1u);
    tail = total_size - aligned;

    for (uint32_t i = 0; i < aligned; i += (uint32_t) transfer_size) {
        for (int j = 0; j < transfer_size; j++) {
            memory_write_phys_u8(dma->cpu, phys_addr + i + (uint32_t) j, data[i + (uint32_t) j]);
        }
    }

    if (tail != 0) {
        for (int j = 0; j < transfer_size; j++) {
            uint32_t offset = aligned + (uint32_t) j;
            uint8_t value = (j < (int)tail) ? data[offset] : memory_read_phys_u8(dma->cpu, phys_addr + offset);
            memory_write_phys_u8(dma->cpu, phys_addr + offset, value);
        }
    }
}
