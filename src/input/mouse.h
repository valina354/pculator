#ifndef _MOUSE_H_
#define _MOUSE_H_

#include <stdint.h>
#include "kbc_at_device.h"
#include "../chipset/uart.h"

#define MOUSE_ACTION_MOVE		0
#define MOUSE_ACTION_LEFT		1
#define MOUSE_ACTION_RIGHT		2

#define MOUSE_PRESSED			0
#define MOUSE_UNPRESSED			1
#define MOUSE_NEITHER			2

#define MOUSE_DEFAULT_BAUD		1200
#define MOUSE_BITS_PER_BYTE		10.0
#define MOUSE_SERIAL_TIMER_HZ	(MOUSE_DEFAULT_BAUD / MOUSE_BITS_PER_BYTE)
#define MOUSE_PACKET_LEN		3
#define MOUSE_TX_BUFFER_LEN		4

void mouse_init_none(void);
void mouse_togglereset(void* dummy, uint8_t value);
void mouse_tx(void* dummy, uint8_t value);
void mouse_action(uint8_t action, uint8_t state, int32_t xrel, int32_t yrel);
void mouse_rxpoll(void* dummy);
void mouse_init(UART_t* uart);
void mouse_init_ps2(kbc_at_port_t* port);

#endif
