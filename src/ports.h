#ifndef _PORTS_H_
#define _PORTS_H_

#include <stdint.h>

#define PORTS_COUNT 0x10000

struct ports_s {
	uint32_t start;
	uint32_t size;
	uint8_t(*readcb)(void* udata, uint16_t addr);
	uint16_t(*readcbW)(void* udata, uint16_t addr);
	uint32_t(*readcbL)(void* udata, uint16_t addr);
	void (*writecb)(void* udata, uint16_t addr, uint8_t value);
	void (*writecbW)(void* udata, uint16_t addr, uint16_t value);
	void (*writecbL)(void* udata, uint16_t addr, uint32_t value);
	void* udata;
	int used;
};

extern uint8_t(*ports_cbReadB[PORTS_COUNT])(void* udata, uint32_t portnum);
extern uint16_t(*ports_cbReadW[PORTS_COUNT])(void* udata, uint32_t portnum);
extern uint32_t(*ports_cbReadL[PORTS_COUNT])(void* udata, uint32_t portnum);
extern void (*ports_cbWriteB[PORTS_COUNT])(void* udata, uint32_t portnum, uint8_t value);
extern void (*ports_cbWriteW[PORTS_COUNT])(void* udata, uint32_t portnum, uint16_t value);
extern void (*ports_cbWriteL[PORTS_COUNT])(void* udata, uint32_t portnum, uint32_t value);
extern void* ports_udata[PORTS_COUNT];

void ports_cbRegister(uint32_t start, uint32_t count, uint8_t(*readb)(void*, uint16_t), uint16_t(*readw)(void*, uint16_t), void (*writeb)(void*, uint16_t, uint8_t), void (*writew)(void*, uint16_t, uint16_t), void* udata);
void ports_cbRegisterEx(uint32_t start, uint32_t count,
	uint8_t(*readb)(void*, uint16_t), uint16_t(*readw)(void*, uint16_t), uint32_t(*readl)(void*, uint16_t),
	void (*writeb)(void*, uint16_t, uint8_t), void (*writew)(void*, uint16_t, uint16_t), void (*writel)(void*, uint16_t, uint32_t),
	void* udata);
void ports_cbUnregister(uint32_t start, uint32_t count);
void ports_init();

#endif
