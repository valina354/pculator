/*
  PCulator: A portable, open-source x86 PC emulator.
  Copyright (C)2025 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
	Intel 8259 interrupt controller
*/

#include <stdint.h>
#include <string.h>
#include "../config.h"
#include "../debuglog.h"
#include "i8259.h"
#include "../ports.h"

static uint8_t i8259_has_pending(I8259_t* i8259) {
	return (uint8_t)(i8259->irr & (~i8259->imr));
}

static void i8259_sync_cascade(I8259_t* slave) {
	I8259_t* master;

	if ((slave == NULL) || (slave->master == NULL)) {
		return;
	}

	master = (I8259_t*)slave->master;
	if (((master->isr & (1 << 2)) == 0) && i8259_has_pending(slave)) {
		master->irr |= (1 << 2);
	}
	else {
		master->irr &= ~(1 << 2);
	}
}

uint8_t i8259_read(I8259_t* i8259, uint16_t portnum) {
#ifdef DEBUG_PIC
	debug_log(DEBUG_DETAIL, "[I8259] Read port 0x%X\n", portnum);
#endif
	switch (portnum & 1) {
	case 0:
		if (i8259->readmode == 0) {
			return i8259->irr;
		}
		else {
			return i8259->isr;
		}
	case 1: //read mask register
		return i8259->imr;
	}
	return 0;
}

void i8259_write(I8259_t* i8259, uint16_t portnum, uint8_t value) {
#ifdef DEBUG_PIC
	debug_log(DEBUG_DETAIL, "[I8259] Write port 0x%X: %X\n", portnum, value);
#endif
	switch (portnum & 1) {
	case 0:
		if (value & 0x10) { //ICW1
#ifdef DEBUG_PIC
			debug_log(DEBUG_DETAIL, "[I8259] ICW1 = %02X\r\n", value);
#endif
			i8259->imr = 0x00;
			i8259->icw[1] = value;
			i8259->icwstep = 2;
			i8259->readmode = 0;
		}
		else if ((value & 0x08) == 0) { //OCW2
#ifdef DEBUG_PIC
			debug_log(DEBUG_DETAIL, "[I8259] OCW2 = %02X\r\n", value);
#endif
			i8259->ocw[2] = value;
			switch (value & 0xE0) {
			case 0x60: //specific EOI
				i8259->irr &= ~(1 << (value & 0x07));
				i8259->isr &= ~(1 << (value & 0x07));
				break;
			case 0x40: //no operation
				break;
			case 0x20: //non-specific EOI
				i8259->irr &= ~i8259->isr;
				i8259->isr = 0x00;
				break;
			default: //other
#ifdef DEBUG_PIC
				debug_log(DEBUG_DETAIL, "[I8259] Unhandled EOI type: %u\r\n", value & 0xE0);
#endif
				break;
			}
			if (i8259->slave != NULL) {
				i8259_sync_cascade((I8259_t*)i8259->slave);
			}
			if (i8259->master != NULL) {
				i8259_sync_cascade(i8259);
			}
		}
		else { //OCW3
#ifdef DEBUG_PIC
			debug_log(DEBUG_DETAIL, "[I8259] OCW3 = %02X\r\n", value);
#endif
			i8259->ocw[3] = value;
			if (value & 0x02) {
				i8259->readmode = value & 1;
			}
		}
		break;
	case 1:
#ifdef DEBUG_PIC
		debug_log(DEBUG_DETAIL, "[I8259] ICW%u = %02X\r\n", i8259->icwstep, value);
#endif
		switch (i8259->icwstep) {
		case 2: //ICW2
			i8259->icw[2] = value;
			i8259->intoffset = value & 0xF8;
#ifdef DEBUG_PIC
			debug_log(DEBUG_DETAIL, "[I8259] Interrupt vector set to %02X\n", i8259->intoffset);
#endif
			if (i8259->icw[1] & 0x02) {
				i8259->icwstep = 4;
			}
			else {
				i8259->icwstep = 3;
			}
			break;
		case 3: //ICW3
			i8259->icw[3] = value;
			if (i8259->icw[1] & 0x01) {
				i8259->icwstep = 4;
			}
			else {
				i8259->icwstep = 5; //done with ICWs
			}
			break;
		case 4: //ICW4
			i8259->icw[4] = value;
			i8259->icwstep = 5; //done with ICWs
			break;
		case 5: //just set IMR value now
			i8259->imr = value;
			if (i8259->slave != NULL) {
				i8259_sync_cascade((I8259_t*)i8259->slave);
			}
			if (i8259->master != NULL) {
				i8259_sync_cascade(i8259);
			}
			break;
		}
		break;
	}
}

uint8_t i8259_nextintr(I8259_t* i8259) {
	uint8_t i, tmpirr;
	tmpirr = i8259->irr & (uint8_t)(~i8259->imr); //AND request register with inverted mask register
	for (i = 0; i < 8; i++) {
		if ((tmpirr >> i) & 1) {
			i8259->irr &= (uint8_t)~(1u << i);
			i8259->isr |= (uint8_t)(1u << i);
			return i;
		}
	}
	return 0xFF;
}

void i8259_doirq(I8259_t* i8259, uint8_t irqnum) {
#ifdef DEBUG_PIC
	debug_log(DEBUG_DETAIL, "[I8259] IRQ %u raised\r\n", irqnum);
#endif
	i8259->irr |= (1 << irqnum);
	if (i8259->master != NULL) { //if this is a slave PIC
		i8259_sync_cascade(i8259);
	}
}

void i8259_clearirq(I8259_t* i8259, uint8_t irqnum) {
#ifdef DEBUG_PIC
	debug_log(DEBUG_DETAIL, "[I8259] IRQ %u cleared\r\n", irqnum);
#endif
	i8259->irr &= ~(1 << irqnum);
	if (i8259->master != NULL) { //if this is a slave PIC
		i8259_sync_cascade(i8259);
	}
}

void i8259_setlevelirq(I8259_t* i8259, uint8_t irqnum, uint8_t state) {
	if (state != 0) {
		i8259_doirq(i8259, irqnum);
	} else {
		i8259_clearirq(i8259, irqnum);
	}
}

void i8259_init(I8259_t* i8259, uint16_t portbase, I8259_t* master) {
	memset(i8259, 0, sizeof(I8259_t));
	i8259->intoffset = 0;
	i8259->imr = 0xFF; //mask all interrupts on init
	i8259->master = (void*)master;
	if (master != NULL) {
		master->slave = (void*)i8259;
	}
	ports_cbRegister(portbase, 2, (void*)i8259_read, NULL, (void*)i8259_write, NULL, i8259);
}
