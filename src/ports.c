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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h> //for rand()
#include "config.h"
#include "debuglog.h"
#include "cpu/cpu.h"
#include "machine.h"
#include "chipset/i8259.h"
#include "chipset/i8253.h"
#include "chipset/i8255.h"
#include "ports.h"
#include "video/vga.h"

struct ports_s ports[64];

int lastportmap = -1;

extern MACHINE_t machine;

extern int showops;

static FUNC_INLINE int getportmap(uint32_t addr32) {
	int i;
	for (i = lastportmap; i >= 0; i--) {
		if (ports[i].used) {
			if ((addr32 >= ports[i].start) && (addr32 < (ports[i].start + ports[i].size))) {
				return i;
			}
		}
	}
	return -1;
}

void port_write(CPU_t* cpu, uint16_t portnum, uint8_t value) {
	int map;
#ifdef DEBUG_PORTS
	debug_log(DEBUG_DETAIL, "port_write @ %03X <- %02X\r\n", portnum, value);
#endif
	//printf("port 8-bit write @ %Xh <- %02X\r\n", portnum, value);
	//portnum &= 0x0FFF;
	map = getportmap(portnum);

	if (map != -1) {
		if (ports[map].writecb != NULL) {
			(*ports[map].writecb)(ports[map].udata, portnum, value);
			return;
		}
	}

	if (portnum == 0x92) {
		cpu->a20_gate = (value & 2) ? 1 : 0;
		return;
	}
}

void port_writew(CPU_t* cpu, uint16_t portnum, uint16_t value) {
	int map;
	//printf("port 16-bit write @ %Xh <- %04X\r\n", portnum, value);
	//portnum &= 0x0FFF;
	map = getportmap(portnum);

	/*if (portnum == 0x80) {
		debug_log(DEBUG_DETAIL, "Diagnostic port out: %04X\r\n", value);
	}*/
	if (map != -1) {
		if (ports[map].writecbW != NULL) {
			(*ports[map].writecbW)(ports[map].udata, portnum, value);
			return;
		}
	}
	port_write(cpu, portnum, (uint8_t)value);
	port_write(cpu, portnum + 1, (uint8_t)(value >> 8));
}

void port_writel(CPU_t* cpu, uint16_t portnum, uint32_t value) {
	int map;
	//printf("port 32-bit write @ %Xh <- %08X\r\n", portnum, value);
	//portnum &= 0x0FFF;
	map = getportmap(portnum);

	if (portnum == 0x80) {
		debug_log(DEBUG_DETAIL, "Diagnostic port out: %08X\r\n", value);
	}

	if (map != -1) {
		if (ports[map].writecbL != NULL) {
			(*ports[map].writecbL)(ports[map].udata, portnum, value);
			return;
		}
	}
	port_write(cpu, portnum, (uint8_t)value);
	port_write(cpu, portnum + 1, (uint8_t)(value >> 8));
	port_write(cpu, portnum + 2, (uint8_t)(value >> 16));
	port_write(cpu, portnum + 3, (uint8_t)(value >> 24));
}

uint8_t port_read(CPU_t* cpu, uint16_t portnum) {
	int map;
	uint8_t ret;
#ifdef DEBUG_PORTS
	if ((portnum != 0x3BA) && (portnum != 0x3DA)) debug_log(DEBUG_DETAIL, "port_read @ %03X\r\n", portnum);
#endif
	if (showops) {
		if ((portnum != 0x3BA) && (portnum != 0x3DA)) debug_log(DEBUG_DETAIL, "port_read @ %03X\r\n", portnum);
	}
	//printf("port 8-bit read @ %Xh\r\n", portnum);
	//portnum &= 0x0FFF;
	map = getportmap(portnum);

	if (map != -1) {
		if (ports[map].readcb != NULL) {
			ret = (*ports[map].readcb)(ports[map].udata, portnum);
			return ret;
		}
	}

	if (portnum == 0x92) {
		return (uint8_t)(cpu->a20_gate ? 0x02 : 0x00);
	}

	return 0xFF;
}

uint16_t port_readw(CPU_t* cpu, uint16_t portnum) {
	int map;
	uint16_t ret;
	//printf("port 16-bit read @ %Xh\r\n", portnum);
	//portnum &= 0x0FFF;
	map = getportmap(portnum);

	if (map != -1) {
		if (ports[map].readcbW != NULL) {
			ret = (*ports[map].readcbW)(ports[map].udata, portnum);
			return ret;
		}
	}

	ret = port_read(cpu, portnum);
	ret |= (uint16_t)port_read(cpu, portnum + 1) << 8;
	return ret;
}

uint32_t port_readl(CPU_t* cpu, uint16_t portnum) {
	int map;
	uint32_t ret;
	//printf("port 32-bit read @ %Xh\r\n", portnum);
	//portnum &= 0x0FFF;
	map = getportmap(portnum);

	if (map != -1) {
		if (ports[map].readcbL != NULL) {
			ret = (*ports[map].readcbL)(ports[map].udata, portnum);
			return ret;
		}
	}

	ret = port_read(cpu, portnum);
	ret |= (uint32_t)port_read(cpu, portnum + 1) << 8;
	ret |= (uint32_t)port_read(cpu, portnum + 2) << 16;
	ret |= (uint32_t)port_read(cpu, portnum + 3) << 24;
	return ret;
}

void ports_cbRegisterEx(uint32_t start, uint32_t count,
	uint8_t (*readb)(void*, uint16_t), uint16_t (*readw)(void*, uint16_t), uint32_t(*readl)(void*, uint16_t),
	void (*writeb)(void*, uint16_t, uint8_t), void (*writew)(void*, uint16_t, uint16_t), void (*writel)(void*, uint16_t, uint32_t),
	void* udata) {
	uint8_t i;
	for (i = 0; i < 64; i++) {
		if (ports[i].used == 0) break;
	}
	if (i == 64) {
		debug_log(DEBUG_ERROR, "[PORTS] Out of port map structs!\n");
		exit(0);
	}
	ports[i].readcb = readb;
	ports[i].writecb = writeb;
	ports[i].readcbW = readw;
	ports[i].writecbW = writew;
	ports[i].readcbL = readl;
	ports[i].writecbL = writel;
	ports[i].start = start;
	ports[i].size = count;
	ports[i].udata = udata;
	ports[i].used = 1;
	if (i > lastportmap) {
		lastportmap = i;
	}
}

void ports_cbRegister(uint32_t start, uint32_t count, uint8_t (*readb)(void*, uint16_t), uint16_t (*readw)(void*, uint16_t), void (*writeb)(void*, uint16_t, uint8_t), void (*writew)(void*, uint16_t, uint16_t), void* udata) {
	ports_cbRegisterEx(start, count, readb, readw, NULL, writeb, writew, NULL, udata);
}

void ports_cbUnregister(uint32_t start, uint32_t count)
{
	uint32_t end;
	int i;
	int highest_used;

	if ((count == 0) || (start >= PORTS_COUNT)) {
		return;
	}

	end = start + count;
	if (end > PORTS_COUNT) {
		end = PORTS_COUNT;
	}
	for (i = 0; i < 64; i++) {
		uint32_t map_start;
		uint32_t map_end;

		if (!ports[i].used) {
			continue;
		}

		map_start = ports[i].start;
		map_end = ports[i].start + ports[i].size;
		if ((start < map_end) && (end > map_start)) {
			ports[i].used = 0;
			ports[i].start = 0;
			ports[i].size = 0;
			ports[i].readcb = NULL;
			ports[i].readcbW = NULL;
			ports[i].readcbL = NULL;
			ports[i].writecb = NULL;
			ports[i].writecbW = NULL;
			ports[i].writecbL = NULL;
			ports[i].udata = NULL;
		}
	}

	highest_used = -1;
	for (i = 0; i < 64; i++) {
		if (ports[i].used) {
			highest_used = i;
		}
	}

	lastportmap = highest_used;
}

void ports_init() {
	uint32_t i;
	for (i = 0; i < 64; i++) {
		ports[i].readcb = NULL;
		ports[i].writecb = NULL;
		ports[i].readcbW = NULL;
		ports[i].writecbW = NULL;
		ports[i].readcbL = NULL;
		ports[i].writecbL = NULL;
		ports[i].used = 0;
	}
}
