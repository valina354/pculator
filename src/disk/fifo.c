/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          FIFO infrastructure.
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>
 *
 *          Copyright 2023-2025 Miran Grca.
 *
 * Adapted for PCulator.
 */
#include <stdlib.h>
#include <string.h>
#include "fifo.h"

int fifo_get_count(void* priv)
{
	fifo_t* fifo;

	fifo = (fifo_t*)priv;
	if (fifo == NULL) {
		return 0;
	}

	if (fifo->end == fifo->start) {
		return fifo->full ? fifo->len : 0;
	}

	if (fifo->end > fifo->start) {
		return fifo->end - fifo->start;
	}

	return fifo->len - fifo->start + fifo->end;
}

void fifo_write(uint8_t val, void* priv)
{
	fifo_t* fifo;

	fifo = (fifo_t*)priv;
	if ((fifo == NULL) || (fifo->buf == NULL) || (fifo->len <= 0)) {
		return;
	}

	if (fifo->full) {
		fifo->overrun = 1;
		return;
	}

	fifo->buf[fifo->end] = val;
	fifo->end = (fifo->end + 1) % fifo->len;
	fifo->empty = 0;
	if (fifo->end == fifo->start) {
		fifo->full = 1;
	}
	fifo->ready = (fifo_get_count(fifo) >= fifo->trigger_len) ? 1 : 0;
}

uint8_t fifo_read(void* priv)
{
	fifo_t* fifo;
	uint8_t ret;

	fifo = (fifo_t*)priv;
	if ((fifo == NULL) || fifo->empty || (fifo->buf == NULL) || (fifo->len <= 0)) {
		return 0;
	}

	ret = fifo->buf[fifo->start];
	fifo->start = (fifo->start + 1) % fifo->len;
	fifo->full = 0;
	if (fifo->start == fifo->end) {
		fifo->empty = 1;
	}
	fifo->ready = (fifo_get_count(fifo) >= fifo->trigger_len) ? 1 : 0;
	return ret;
}

int fifo_get_full(void* priv)
{
	fifo_t* fifo;

	fifo = (fifo_t*)priv;
	return (fifo != NULL) ? fifo->full : 0;
}

int fifo_get_empty(void* priv)
{
	fifo_t* fifo;

	fifo = (fifo_t*)priv;
	return (fifo != NULL) ? fifo->empty : 1;
}

int fifo_get_overrun(void* priv)
{
	fifo_t* fifo;

	fifo = (fifo_t*)priv;
	return (fifo != NULL) ? fifo->overrun : 0;
}

int fifo_get_ready(void* priv)
{
	fifo_t* fifo;

	fifo = (fifo_t*)priv;
	return (fifo != NULL) ? fifo->ready : 0;
}

int fifo_get_trigger_len(void* priv)
{
	fifo_t* fifo;

	fifo = (fifo_t*)priv;
	return (fifo != NULL) ? fifo->trigger_len : 0;
}

void fifo_set_trigger_len(void* priv, int trigger_len)
{
	fifo_t* fifo;

	fifo = (fifo_t*)priv;
	if (fifo == NULL) {
		return;
	}

	if (trigger_len < 1) {
		trigger_len = 1;
	}
	if (trigger_len > fifo->len) {
		trigger_len = fifo->len;
	}
	fifo->trigger_len = trigger_len;
	fifo->ready = (fifo_get_count(fifo) >= fifo->trigger_len) ? 1 : 0;
}

void fifo_set_len(void* priv, int len)
{
	fifo_t* fifo;
	uint8_t* new_buf;

	fifo = (fifo_t*)priv;
	if ((fifo == NULL) || (len < 1)) {
		return;
	}

	new_buf = (uint8_t*)malloc((size_t)len);
	if (new_buf == NULL) {
		return;
	}

	free(fifo->buf);
	fifo->buf = new_buf;
	fifo->len = len;
	fifo_reset(fifo);
}

void fifo_reset(void* priv)
{
	fifo_t* fifo;

	fifo = (fifo_t*)priv;
	if (fifo == NULL) {
		return;
	}

	fifo->start = 0;
	fifo->end = 0;
	fifo->empty = 1;
	fifo->full = 0;
	fifo->overrun = 0;
	fifo->ready = 0;
	if (fifo->buf != NULL) {
		memset(fifo->buf, 0, (size_t)fifo->len);
	}
}

void fifo_close(void* priv)
{
	fifo_t* fifo;

	fifo = (fifo_t*)priv;
	if (fifo == NULL) {
		return;
	}

	free(fifo->buf);
	fifo->buf = NULL;
	free(fifo);
}

void* fifo_init(int len)
{
	fifo_t* fifo;

	if (len < 1) {
		return NULL;
	}

	fifo = (fifo_t*)calloc(1, sizeof(fifo_t));
	if (fifo == NULL) {
		return NULL;
	}

	fifo->buf = (uint8_t*)calloc((size_t)len, sizeof(uint8_t));
	if (fifo->buf == NULL) {
		free(fifo);
		return NULL;
	}

	fifo->len = len;
	fifo->trigger_len = len;
	fifo_reset(fifo);
	return fifo;
}
