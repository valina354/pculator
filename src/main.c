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
#include <string.h>
#include "config.h"
#include "host/host.h"
#include "args.h"
#include "timing.h"
#include "memory.h"
#include "ports.h"
#include "machine.h"
#include "utility.h"
#include "debuglog.h"
#include "cpu/cpu.h"
#include "chipset/i8259.h"
#include "frontends/frontend.h"
#include "audio/frontendaudio.h"

extern int showops;

char* usemachine = "p55t2p4"; //default

char title[64]; //assuming 64 isn't safe if somebody starts messing with STR_TITLE and STR_VERSION

uint64_t ops = 0;
uint32_t ramsize = 0, instructionsperloop = 100, cpuLimitTimer;
uint8_t videocard = 0xFF, showMIPS = 0;

volatile double currentMIPS = 0;
volatile uint8_t running = 1;

extern volatile int no_affinity;

MACHINE_t machine;

void optimer(void* dummy) {
	ops /= 10000;
	currentMIPS = (double)ops / 10.0;
	if (showMIPS) {
		debug_log(DEBUG_INFO, "%llu.%llu MIPS          \r", ops / 10, ops % 10);
	}
	ops = 0;
}

int showports = 0;

int main(int argc, char *argv[]) {
	static const uint8_t default_nic_mac[6] = { 0xac, 0xde, 0x48, 0x88, 0xbb, 0xab };

	sprintf(title, "%s %s", STR_TITLE, STR_VERSION);

	debug_log(DEBUG_INFO, "%s (c)2026 Mike Chambers\r\n", title);
	debug_log(DEBUG_INFO, "[A portable, open source x86 PC emulator]\r\n\r\n");

	ports_init();
	timing_init();
	memory_init();

	memcpy(machine.nic_mac, default_nic_mac, sizeof(default_nic_mac));
	machine.nic_type = MACHINE_NIC_NONE;
	machine.rtl8139 = NULL;
	machine.pcap_if = -1;
	if (args_parse(&machine, argc, argv)) {
		return -1;
	}

	if (!no_affinity) {
		if (host_do_startup_tasks()) {
			return -1;
		}
	}

	if (frontend_init(title)) {
		debug_log(DEBUG_ERROR, "[ERROR] Frontend initialization failure\r\n");
		return -1;
	}

	if (machine_init(&machine, usemachine) < 0) {
		debug_log(DEBUG_ERROR, "[ERROR] Machine initialization failure\r\n");
		frontend_shutdown();
		return -1;
	}

	if (frontend_bind_machine(&machine) != 0) {
		debug_log(DEBUG_INFO, "[WARNING] Frontend machine binding failure\r\n");
	}
	if (frontendaudio_init(&machine)) {
		debug_log(DEBUG_INFO, "[WARNING] Frontend audio initialization failure\r\n");
	}

	timing_addTimer(optimer, NULL, 10, TIMING_ENABLED);
	while (running) {
		static int curloop = 0;
		cpu_exec(&machine.CPU, &machine.i8259, &machine.i8259_slave, instructionsperloop);
		ops += instructionsperloop;
		timing_loop();
		frontendaudio_updateSampleTiming();
		if (++curloop == 10) {
			if (frontend_pump_events() == FRONTEND_PUMP_QUIT) {
				running = 0;
			}
			curloop = 0;
		}
	}

	frontendaudio_shutdown();
	frontend_shutdown();

	return 0;
}
