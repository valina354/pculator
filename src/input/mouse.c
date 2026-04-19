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
	Host mouse routed either as a Microsoft-compatible serial mouse
	or as a basic PS/2 auxiliary mouse.
*/

#include <string.h>
#include <stdint.h>
#include "../config.h"
#include "../debuglog.h"
#include "mouse.h"

enum {
	MOUSE_MODE_NONE = 0,
	MOUSE_MODE_SERIAL,
	MOUSE_MODE_PS2
};

enum {
	MOUSE_PS2_FLAG_REPORTING = 0x01,
	MOUSE_PS2_FLAG_REMOTE = 0x02,
	MOUSE_PS2_FLAG_SCALING21 = 0x04,
	MOUSE_PS2_FLAG_WANT_PARAM = 0x08
};

typedef struct {
	uint8_t left;
	uint8_t right;
} MOUSE_BUTTONS_t;

typedef struct {
	kbc_at_device_t dev;
	uint8_t scan_enabled;
} MOUSE_PS2_t;

static MOUSE_BUTTONS_t mouse_buttons;
static uint8_t mouse_mode = MOUSE_MODE_NONE;
static UART_t* mouse_uart = NULL;
static uint8_t mouse_lasttoggle = 0;
static int32_t mouse_pendingx = 0;
static int32_t mouse_pendingy = 0;
static uint8_t mouse_buttonsDirty = 0;
static uint8_t mouse_txbuf[MOUSE_TX_BUFFER_LEN];
static uint8_t mouse_txlen = 0;
static uint8_t mouse_txpos = 0;
static MOUSE_PS2_t mouse_ps2;

static void mouse_clear_pending_state(void);
static uint8_t mouse_has_pending_packet(void);
static uint8_t mouse_serial_is_transmitting(void);
static void mouse_serial_prepare_pending_packet(void);
static void mouse_ps2_queue_stream_packets(MOUSE_PS2_t* ps2);

static int32_t mouse_clamp_serial_delta(int32_t value)
{
	if (value < -128) return -128;
	if (value > 127) return 127;
	return value;
}

static int32_t mouse_clamp_ps2_delta(int32_t value)
{
	if (value < -255) return -255;
	if (value > 255) return 255;
	return value;
}

static void mouse_clear_pending_state(void)
{
	mouse_txlen = 0;
	mouse_txpos = 0;
	mouse_pendingx = 0;
	mouse_pendingy = 0;
	mouse_buttonsDirty = 0;

	if (mouse_uart != NULL) {
		mouse_uart->rxnew = 0;
		mouse_uart->pendirq &= (uint8_t)~UART_PENDING_RX;
	}
}

static uint8_t mouse_has_pending_packet(void)
{
	return (uint8_t)((mouse_pendingx != 0) || (mouse_pendingy != 0) || mouse_buttonsDirty);
}

static uint8_t mouse_serial_is_transmitting(void)
{
	return (uint8_t)((mouse_txlen > 0) && (mouse_txpos < mouse_txlen));
}

static void mouse_serial_begin_transmit(uint8_t byte_count)
{
	mouse_txlen = byte_count;
	mouse_txpos = 0;
}

static void mouse_serial_prepare_reset_id(void)
{
	mouse_txbuf[0] = 'M';
	mouse_serial_begin_transmit(1);
}

static void mouse_serial_prepare_packet(int32_t xrel, int32_t yrel)
{
	uint8_t b0;

	b0 = 0x40;
	b0 |= (uint8_t)(((yrel & 0xC0) >> 4) & 0x0C);
	b0 |= (uint8_t)(((xrel & 0xC0) >> 6) & 0x03);
	if (mouse_buttons.left) b0 |= 0x20;
	if (mouse_buttons.right) b0 |= 0x10;

	mouse_txbuf[0] = b0;
	mouse_txbuf[1] = (uint8_t)(xrel & 0x3F);
	mouse_txbuf[2] = (uint8_t)(yrel & 0x3F);
	mouse_serial_begin_transmit(MOUSE_PACKET_LEN);
}

static void mouse_serial_prepare_pending_packet(void)
{
	int32_t xrel;
	int32_t yrel;

	if (mouse_serial_is_transmitting()) return;
	if (!mouse_has_pending_packet()) return;

	xrel = mouse_clamp_serial_delta(mouse_pendingx);
	yrel = mouse_clamp_serial_delta(mouse_pendingy);
	mouse_pendingx -= xrel;
	mouse_pendingy -= yrel;
	mouse_buttonsDirty = 0;

	mouse_serial_prepare_packet(xrel, yrel);
}

static uint8_t mouse_ps2_can_queue(const kbc_at_device_t* dev, uint8_t main, uint8_t bytes)
{
	uint8_t used;
	uint8_t limit;

	if (dev == NULL) {
		return 0;
	}

	used = kbc_at_dev_queue_pos((kbc_at_device_t*)dev, main);
	limit = main ? 63 : 15;
	return (uint8_t)(used <= (uint8_t)(limit - bytes));
}

static void mouse_ps2_queue_byte(MOUSE_PS2_t* ps2, uint8_t val, uint8_t main)
{
	if ((ps2 == NULL) || !mouse_ps2_can_queue(&ps2->dev, main, 1)) {
		return;
	}

	kbc_at_dev_queue_add(&ps2->dev, val, main);
}

static uint8_t mouse_ps2_status_byte(const MOUSE_PS2_t* ps2)
{
	uint8_t status = 0;

	if (ps2 == NULL) {
		return 0;
	}

	if (ps2->dev.flags & MOUSE_PS2_FLAG_REMOTE) status |= 0x40;
	if (ps2->dev.flags & MOUSE_PS2_FLAG_REPORTING) status |= 0x20;
	if (ps2->dev.flags & MOUSE_PS2_FLAG_SCALING21) status |= 0x10;
	if (mouse_buttons.left) status |= 0x01;
	if (mouse_buttons.right) status |= 0x02;

	return status;
}

static void mouse_ps2_prepare_packet(int32_t raw_x, int32_t raw_y, int32_t xrel, int32_t yrel, uint8_t packet[3])
{
	packet[0] = 0x08;
	if (mouse_buttons.left) packet[0] |= 0x01;
	if (mouse_buttons.right) packet[0] |= 0x02;
	if (xrel < 0) packet[0] |= 0x10;
	if (yrel < 0) packet[0] |= 0x20;
	if ((raw_x < -255) || (raw_x > 255)) packet[0] |= 0x40;
	if ((raw_y < -255) || (raw_y > 255)) packet[0] |= 0x80;

	packet[1] = (uint8_t)(xrel & 0xFF);
	packet[2] = (uint8_t)(yrel & 0xFF);
}

static int mouse_ps2_queue_pending_packet(MOUSE_PS2_t* ps2, uint8_t main, uint8_t force)
{
	int32_t raw_x;
	int32_t raw_y;
	int32_t xrel;
	int32_t yrel;
	uint8_t packet[3];
	uint8_t i;

	if ((ps2 == NULL) || !mouse_ps2_can_queue(&ps2->dev, main, 3)) {
		return 0;
	}

	if (!force && !mouse_has_pending_packet()) {
		return 0;
	}

	raw_x = mouse_pendingx;
	raw_y = -mouse_pendingy;
	xrel = mouse_clamp_ps2_delta(raw_x);
	yrel = mouse_clamp_ps2_delta(raw_y);

	mouse_pendingx -= xrel;
	mouse_pendingy += yrel;
	mouse_buttonsDirty = 0;

	mouse_ps2_prepare_packet(raw_x, raw_y, xrel, yrel, packet);
	for (i = 0; i < 3; i++) {
		mouse_ps2_queue_byte(ps2, packet[i], main);
	}

	return 1;
}

static void mouse_ps2_set_defaults(MOUSE_PS2_t* ps2)
{
	if (ps2 == NULL) {
		return;
	}

	ps2->dev.flags &= (uint8_t)~(MOUSE_PS2_FLAG_REPORTING | MOUSE_PS2_FLAG_REMOTE |
		MOUSE_PS2_FLAG_SCALING21 | MOUSE_PS2_FLAG_WANT_PARAM);
	ps2->dev.resolution = 2;
	ps2->dev.rate = 100;
}

static void mouse_ps2_reset(MOUSE_PS2_t* ps2, int do_fa)
{
	if (ps2 == NULL) {
		return;
	}

	mouse_clear_pending_state();
	kbc_at_dev_reset(&ps2->dev, do_fa);
	ps2->scan_enabled = 1;
	mouse_ps2_set_defaults(ps2);
}

static void mouse_ps2_bat(void* priv)
{
	MOUSE_PS2_t* ps2 = (MOUSE_PS2_t*)priv;

	if (ps2 == NULL) {
		return;
	}

	mouse_ps2_queue_byte(ps2, 0xAA, 0);
	mouse_ps2_queue_byte(ps2, 0x00, 0);
}

static void mouse_ps2_queue_stream_packets(MOUSE_PS2_t* ps2)
{
	if ((ps2 == NULL) || (mouse_mode != MOUSE_MODE_PS2)) {
		return;
	}

	if (!(ps2->dev.flags & MOUSE_PS2_FLAG_REPORTING) || (ps2->dev.flags & MOUSE_PS2_FLAG_REMOTE)) {
		return;
	}

	while (mouse_has_pending_packet() && mouse_ps2_can_queue(&ps2->dev, 1, 3)) {
		if (!mouse_ps2_queue_pending_packet(ps2, 1, 0)) {
			break;
		}
	}
}

static void mouse_ps2_write(void* priv)
{
	MOUSE_PS2_t* ps2 = (MOUSE_PS2_t*)priv;
	uint8_t val;

	if ((ps2 == NULL) || (ps2->dev.port == NULL)) {
		return;
	}

	val = ps2->dev.port->dat;
	ps2->dev.state = DEV_STATE_MAIN_OUT;

	if (ps2->dev.flags & MOUSE_PS2_FLAG_WANT_PARAM) {
		ps2->dev.flags &= (uint8_t)~MOUSE_PS2_FLAG_WANT_PARAM;
		switch (ps2->dev.command) {
		case 0xE8:
			ps2->dev.resolution = (uint8_t)(val & 0x03);
			mouse_ps2_queue_byte(ps2, 0xFA, 0);
			break;
		case 0xF3:
			ps2->dev.rate = val;
			mouse_ps2_queue_byte(ps2, 0xFA, 0);
			break;
		default:
			mouse_ps2_queue_byte(ps2, 0xFE, 0);
			break;
		}
		return;
	}

	switch (val) {
	case 0xE6:
		ps2->dev.flags &= (uint8_t)~MOUSE_PS2_FLAG_SCALING21;
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		break;
	case 0xE7:
		ps2->dev.flags |= MOUSE_PS2_FLAG_SCALING21;
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		break;
	case 0xE8:
		ps2->dev.command = val;
		ps2->dev.flags |= MOUSE_PS2_FLAG_WANT_PARAM;
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		ps2->dev.state = DEV_STATE_MAIN_WANT_IN;
		break;
	case 0xE9:
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		mouse_ps2_queue_byte(ps2, mouse_ps2_status_byte(ps2), 0);
		mouse_ps2_queue_byte(ps2, ps2->dev.resolution, 0);
		mouse_ps2_queue_byte(ps2, ps2->dev.rate, 0);
		break;
	case 0xEA:
		ps2->dev.flags &= (uint8_t)~MOUSE_PS2_FLAG_REMOTE;
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		mouse_ps2_queue_stream_packets(ps2);
		break;
	case 0xEB:
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		(void)mouse_ps2_queue_pending_packet(ps2, 0, 1);
		break;
	case 0xEE:
		mouse_ps2_queue_byte(ps2, 0xFE, 0);
		break;
	case 0xF0:
		ps2->dev.flags |= MOUSE_PS2_FLAG_REMOTE;
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		break;
	case 0xF2:
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		mouse_ps2_queue_byte(ps2, 0x00, 0);
		break;
	case 0xF3:
		ps2->dev.command = val;
		ps2->dev.flags |= MOUSE_PS2_FLAG_WANT_PARAM;
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		ps2->dev.state = DEV_STATE_MAIN_WANT_IN;
		break;
	case 0xF4:
		ps2->dev.flags |= MOUSE_PS2_FLAG_REPORTING;
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		mouse_ps2_queue_stream_packets(ps2);
		break;
	case 0xF5:
		ps2->dev.flags &= (uint8_t)~MOUSE_PS2_FLAG_REPORTING;
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		break;
	case 0xF6:
		mouse_ps2_queue_byte(ps2, 0xFA, 0);
		mouse_clear_pending_state();
		mouse_ps2_set_defaults(ps2);
		break;
	case 0xFE:
		mouse_ps2_queue_byte(ps2, ps2->dev.last_scan_code, 0);
		break;
	case 0xFF:
		mouse_ps2_reset(ps2, 1);
		break;
	default:
		mouse_ps2_queue_byte(ps2, 0xFE, 0);
		break;
	}
}

static void mouse_ps2_poll(void* priv)
{
	MOUSE_PS2_t* ps2 = (MOUSE_PS2_t*)priv;

	if (ps2 == NULL) {
		return;
	}

	mouse_ps2_queue_stream_packets(ps2);
	kbc_at_dev_poll(&ps2->dev);
}

void mouse_init_none(void)
{
	kbc_at_port_t* port = mouse_ps2.dev.port;

	if (port != NULL) {
		port->poll = NULL;
		port->priv = NULL;
		port->wantcmd = 0;
		port->out_new = -1;
	}

	mouse_clear_pending_state();
	memset(&mouse_buttons, 0, sizeof(mouse_buttons));
	memset(&mouse_ps2, 0, sizeof(mouse_ps2));
	mouse_mode = MOUSE_MODE_NONE;
	mouse_uart = NULL;
	mouse_lasttoggle = 0;
}

void mouse_togglereset(void* dummy, uint8_t value)
{
	uint8_t control;

	(void)dummy;
	if (mouse_mode != MOUSE_MODE_SERIAL) {
		return;
	}

	control = value & (UART_MCR_DTR | UART_MCR_RTS);
	if ((control & UART_MCR_RTS) && !(mouse_lasttoggle & UART_MCR_RTS)) {
		mouse_clear_pending_state();
		mouse_serial_prepare_reset_id();
	}

	mouse_lasttoggle = control;
	if (mouse_uart != NULL) {
		mouse_uart->msr = (uint8_t)((mouse_uart->msr & 0x0F) | 0xB0);
		mouse_uart->lastmsr = mouse_uart->msr;
	}
}

void mouse_tx(void* dummy, uint8_t value)
{
	(void)dummy;
	(void)value;
}

void mouse_action(uint8_t action, uint8_t state, int32_t xrel, int32_t yrel)
{
	if (mouse_mode == MOUSE_MODE_NONE) {
		return;
	}

	switch (action) {
	case MOUSE_ACTION_MOVE:
		mouse_pendingx += xrel;
		mouse_pendingy += yrel;
		break;
	case MOUSE_ACTION_LEFT:
		mouse_buttons.left = (state == MOUSE_PRESSED) ? 1 : 0;
		mouse_buttonsDirty = 1;
		break;
	case MOUSE_ACTION_RIGHT:
		mouse_buttons.right = (state == MOUSE_PRESSED) ? 1 : 0;
		mouse_buttonsDirty = 1;
		break;
	default:
		break;
	}

	if (mouse_mode == MOUSE_MODE_PS2) {
		mouse_ps2_queue_stream_packets(&mouse_ps2);
	}
}

void mouse_rxpoll(void* dummy)
{
	(void)dummy;
	if ((mouse_mode != MOUSE_MODE_SERIAL) || (mouse_uart == NULL)) {
		return;
	}

	if (!mouse_serial_is_transmitting()) {
		mouse_serial_prepare_pending_packet();
		return;
	}

	if (mouse_uart->rxnew) return;

	uart_rxdata(mouse_uart, mouse_txbuf[mouse_txpos]);
	mouse_txpos++;

	if (mouse_txpos >= mouse_txlen) {
		mouse_txpos = 0;
		mouse_txlen = 0;
		mouse_serial_prepare_pending_packet();
	}
}

void mouse_init(UART_t* uart)
{
	debug_log(DEBUG_DETAIL, "[MOUSE] Initializing Microsoft-compatible serial mouse\r\n");
	mouse_init_none();
	mouse_mode = MOUSE_MODE_SERIAL;
	mouse_uart = uart;
	if (mouse_uart != NULL) {
		mouse_uart->msr = (uint8_t)((mouse_uart->msr & 0x0F) | 0xB0);
		mouse_uart->lastmsr = mouse_uart->msr;
	}
}

void mouse_init_ps2(kbc_at_port_t* port)
{
	debug_log(DEBUG_DETAIL, "[MOUSE] Initializing PS/2 mouse\r\n");
	mouse_init_none();
	mouse_mode = MOUSE_MODE_PS2;
	kbc_at_dev_init(&mouse_ps2.dev, port);
	mouse_ps2.dev.name = "PS/2 mouse";
	mouse_ps2.dev.process_cmd = mouse_ps2_write;
	mouse_ps2.dev.execute_bat = mouse_ps2_bat;
	mouse_ps2.dev.scan = &mouse_ps2.scan_enabled;
	if (port != NULL) {
		port->priv = &mouse_ps2;
		port->poll = mouse_ps2_poll;
	}
	mouse_ps2_reset(&mouse_ps2, 0);
}
