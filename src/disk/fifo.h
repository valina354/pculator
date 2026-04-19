/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          FIFO infrastructure header.
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>
 *
 *          Copyright 2023-2025 Miran Grca.
 *
 * Adapted for PCulator.
 */
#ifndef _FIFO_H_
#define _FIFO_H_

#include <stdint.h>

typedef struct fifo_t {
	int start;
	int end;
	int trigger_len;
	int len;
	int empty;
	int overrun;
	int full;
	int ready;
	uint8_t* buf;
} fifo_t;

int fifo_get_count(void* priv);
void fifo_write(uint8_t val, void* priv);
uint8_t fifo_read(void* priv);
int fifo_get_full(void* priv);
int fifo_get_empty(void* priv);
int fifo_get_overrun(void* priv);
int fifo_get_ready(void* priv);
int fifo_get_trigger_len(void* priv);
void fifo_set_trigger_len(void* priv, int trigger_len);
void fifo_set_len(void* priv, int len);
void fifo_reset(void* priv);
void fifo_close(void* priv);
void* fifo_init(int len);

#endif
