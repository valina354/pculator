#ifndef _FDC37C665_H_
#define _FDC37C665_H_

#include <stdint.h>
#include "i8259.h"
#include "uart.h"
#include "../disk/fdc.h"

typedef struct {
	uint8_t regs[16];
	uint8_t tries;
	uint8_t max_reg;
	uint8_t cur_reg;
	uint16_t com3_addr;
	uint16_t com4_addr;
	FDC_t* fdc;
	UART_t* uart[2];
	I8259_t* pic;
	void (*tx_cb[2])(void*, uint8_t);
	void* tx_udata[2];
	void (*mcr_cb[2])(void*, uint8_t);
	void* mcr_udata[2];
} FDC37C665_t;

void fdc37c665_init(FDC37C665_t* dev, FDC_t* fdc, UART_t* uart0, UART_t* uart1, I8259_t* pic,
	void (*uart0_tx)(void*, uint8_t), void* uart0_udata, void (*uart0_mcr)(void*, uint8_t), void* uart0_udata2,
	void (*uart1_tx)(void*, uint8_t), void* uart1_udata, void (*uart1_mcr)(void*, uint8_t), void* uart1_udata2);

#endif
