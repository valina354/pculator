#ifndef _BLASTER_H_
#define _BLASTER_H_

#include <stdint.h>
#include "../chipset/dma.h"
#include "../chipset/i8259.h"

typedef struct {
	DMA_t* dma;
	I8259_t* i8259;
	uint16_t base;
	uint8_t dspenable;
	int16_t sample[2];
	uint8_t readbuf[64];
	uint8_t readlen;
	uint8_t readready;
	uint8_t writebuf;
	uint8_t timeconst;
	double samplerate;
	double inputrate;
	double outputrate;
	uint32_t timer;
	uint32_t dmalen;
	uint32_t blocksize;
	uint32_t blockbytes;
	uint32_t autolen8;
	uint32_t autolen16;
	uint8_t dma8chan;
	uint8_t dma16chan;
	uint8_t irq;
	uint8_t lastcmd;
	uint8_t writehilo;
	uint32_t dmacount;
	uint8_t autoinit;
	uint8_t autoinitstop;
	uint8_t testreg;
	uint8_t dorecord;
	uint8_t activedma;
	uint8_t dma16;
	uint8_t dmastereo;
	uint8_t dmasigned;
	uint8_t pausedsp;
	int dmaLastTransferred;
	uint8_t dmaBlockDone;
	uint8_t mixeraddr;
	uint8_t mixerregs[256];
	uint8_t irqpending8;
	uint8_t irqpending16;
	uint32_t pauseduration;
	uint32_t pausecount;
	uint8_t cmdparampos;
	uint8_t cmdparamneeded;
	uint8_t cmdparams[4];
} BLASTER_t;

void blaster_write(BLASTER_t* blaster, uint16_t addr, uint8_t value);
uint8_t blaster_read(BLASTER_t* blaster, uint16_t addr);
void blaster_getSample(BLASTER_t* blaster, int16_t* left, int16_t* right);
void blaster_init(BLASTER_t* blaster, DMA_t* dma, I8259_t* i8259, uint16_t base, uint8_t dma8_channel, uint8_t dma16_channel, uint8_t irq);

#endif
