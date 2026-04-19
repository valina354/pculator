#ifndef _W83877_H_
#define _W83877_H_

#include <stdint.h>
#include "i8259.h"
#include "uart.h"
#include "../disk/fdc.h"

typedef struct {
	uint8_t regs[256];
	uint8_t locked;
	uint8_t rw_locked;
	uint8_t unlock_step;
	uint8_t key;
	uint8_t key_times;
	uint8_t cur_reg;
	uint16_t config_base;
	FDC_t* fdc;
	UART_t* uart[2];
	I8259_t* pic;
	void (*tx_cb[2])(void*, uint8_t);
	void* tx_udata[2];
	void (*mcr_cb[2])(void*, uint8_t);
	void* mcr_udata[2];
} W83877_t;

void w83877_init(W83877_t* dev, FDC_t* fdc, UART_t* uart0, UART_t* uart1, I8259_t* pic,
	void (*uart0_tx)(void*, uint8_t), void* uart0_udata, void (*uart0_mcr)(void*, uint8_t), void* uart0_udata2,
	void (*uart1_tx)(void*, uint8_t), void* uart1_udata, void (*uart1_mcr)(void*, uint8_t), void* uart1_udata2);

#endif
