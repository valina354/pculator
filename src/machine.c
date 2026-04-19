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
	Machine definitions.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "config.h"
#include "debuglog.h"
#include "cpu/cpu.h"
#include "chipset/i8042.h"
#include "chipset/i8259.h"
#include "chipset/i8253.h"
#include "chipset/dma.h"
#include "chipset/i8255.h"
#include "chipset/uart.h"
#include "audio/pcspeaker.h"
#include "audio/opl2.h"
#include "audio/blaster.h"
#include "disk/fdc.h"
#include "disk/ata.h"
#include "input/mouse.h"
#include "io/ne2000.h"
#include "io/pcap.h"
#include "io/rtl8139.h"
#include "video/gd5440.h"
#include "video/vga.h"
#include "chipset/cmosrtc.h"
#include "memory.h"
#include "utility.h"
#include "timing.h"
#include "frontends/frontend.h"
#include "machine.h"

extern MACHINE_t machine;
/*
	ID string, full description, init function, default video, default hardware flags
*/
const MACHINEDEF_t machine_defs[] = {
	{ "p55tp4xe", "ASUS P/I-P55TP4XE", machine_init_p55tp4xe, "cmos/p55tp4xe.bin", VIDEO_CARD_GD5440, MACHINE_HW_BLASTER, { KBC_VENDOR_AMI, 'H', KBC_PROFILE_FLAG_PS2_CAPABLE | KBC_PROFILE_FLAG_AMIKEY2 | KBC_PROFILE_FLAG_PS2_DEFAULT | KBC_PROFILE_FLAG_PCI } },
	{ "p55t2p4", "ASUS P/I-P55T2P4", machine_init_p55t2p4, "cmos/p55t2p4.bin", VIDEO_CARD_GD5440, MACHINE_HW_BLASTER, { KBC_VENDOR_AMI, 'H', KBC_PROFILE_FLAG_PS2_CAPABLE | KBC_PROFILE_FLAG_AMIKEY2 | KBC_PROFILE_FLAG_PS2_DEFAULT | KBC_PROFILE_FLAG_PCI } },
	{ NULL }
};

const MACHINEMEM_t machine_mem[][10] = {
	//ASUS P/I-P55TP4XE
	{
		{ MACHINE_MEM_RAM, 0x00000, 0xA0000, MACHINE_ROM_ISNOTROM, NULL },
		{ MACHINE_MEM_RAM, 0x100000, 0x7F00000, MACHINE_ROM_ISNOTROM, NULL }, // 128 MB RAM
		{ MACHINE_MEM_ENDLIST, 0, 0, 0, NULL }
	},

	//ASUS P/I-P55T2P4
	{
		{ MACHINE_MEM_RAM, 0x00000, 0xA0000, MACHINE_ROM_ISNOTROM, NULL },
		{ MACHINE_MEM_RAM, 0x100000, 0x1FF00000, MACHINE_ROM_ISNOTROM, NULL }, // 512 MB RAM
		{ MACHINE_MEM_ENDLIST, 0, 0, 0, NULL }
	},
};

#define MACHINE_CONVENTIONAL_RAM_SIZE 0xA0000U
#define MACHINE_EXTENDED_RAM_BASE     0x100000U
#define MACHINE_MB_SIZE               (1024U * 1024U)

typedef struct {
	void (*tx)(void*, uint8_t);
	void* udata;
	void (*mcr)(void*, uint8_t);
	void* udata2;
} MACHINE_UART_PROFILE_t;

static uint8_t machine_has_ps2_mouse(const MACHINE_t* machine)
{
	return (uint8_t)((machine != NULL) && (machine->kbc_profile.flags & KBC_PROFILE_FLAG_PS2_CAPABLE));
}

static uint8_t machine_mouse_uses_uart(const MACHINE_t* machine, uint8_t index)
{
	if (machine == NULL) {
		return 0;
	}

	if (index == 0) {
		return (uint8_t)(machine->mouse_connection == MACHINE_MOUSE_UART0);
	}

	return (uint8_t)(machine->mouse_connection == MACHINE_MOUSE_UART1);
}

static void machine_ne2000_rx(void* opaque, const uint8_t* data, int len)
{
	ne2000_rx_frame((NE2000_t*)opaque, data, len);
}

static int machine_attach_ne2000(MACHINE_t* machine, uint32_t base_port, uint8_t irq)
{
	ne2000_init(&machine->ne2000, &machine->i8259, &machine->i8259_slave, base_port, irq, machine->nic_mac);
	if (machine->pcap_if > -1) {
		if (hostnet_attach(machine->pcap_if, machine_ne2000_rx, &machine->ne2000, NULL, NULL) != 0) {
			return -1;
		}
	}

	return 0;
}

static void machine_handle_host_key(void* udata, HOST_KEY_SCANCODE_t scan, uint8_t down)
{
	i8042_handle_host_key((i8042_t*)udata, scan, down);
}

static uint32_t machine_get_ram_region_size(const MACHINEMEM_t* region)
{
	if ((region == NULL) || (region->memtype != MACHINE_MEM_RAM)) {
		return 0;
	}

	if (region->start == 0) {
		return MACHINE_CONVENTIONAL_RAM_SIZE;
	}

	if ((ramsize != 0) && (region->start == MACHINE_EXTENDED_RAM_BASE)) {
		uint32_t size = (ramsize - 1U) * MACHINE_MB_SIZE;
		if ((region->size != 0) && (size > region->size)) {
			size = region->size;
		}
		return size;
	}

	return region->size;
}

static int machine_init_buslogic_controller(MACHINE_t* machine, I8259_t* pic_slave)
{
	BUSLOGIC_CONFIG_t config;

	if (!machine->buslogic_enabled) {
		return 0;
	}

	if ((machine->buslogic_irq >= 8) && (pic_slave == NULL)) {
		debug_log(DEBUG_ERROR, "[SCSI] BusLogic IRQ %u requires a slave PIC on this machine\r\n", machine->buslogic_irq);
		return -1;
	}

	memset(&config, 0, sizeof(config));
	config.cpu = &machine->CPU;
	config.dma = &machine->dma;
	config.pic_master = &machine->i8259;
	config.pic_slave = pic_slave;
	config.base = machine->buslogic_base;
	config.irq = machine->buslogic_irq;
	config.dma_channel = machine->buslogic_dma;
	config.bios_addr = machine->buslogic_bios_addr;
	config.rom_path = machine->buslogic_rom_path;
	config.nvr_path = machine->buslogic_nvr_path;

	if (buslogic_init(&machine->buslogic, &config, machine->scsi_targets) != 0) {
		debug_log(DEBUG_ERROR, "[SCSI] Failed to initialize BusLogic BT-545S\r\n");
		return -1;
	}

	return 0;
}

static int machine_init_floppy_storage(MACHINE_t* machine)
{
	uint8_t drive;

	if (fdc_init(&machine->fdc, &machine->CPU, &machine->i8259, &machine->dma) != 0) {
		return -1;
	}

	for (drive = 0; drive < 2; drive++) {
		if ((machine->floppy_path[drive][0] != 0) &&
			(fdc_insert(&machine->fdc, drive, machine->floppy_path[drive]) != 0)) {
			debug_log(DEBUG_ERROR, "[FDC] Failed to open floppy image for drive %u: %s\r\n",
				(unsigned)drive,
				machine->floppy_path[drive]);
			return -1;
		}
	}

	return 0;
}

static void machine_attach_serial_mouse(UART_t* uart)
{
	if (uart == NULL) {
		return;
	}

	mouse_init(uart);
	timing_addTimer(mouse_rxpoll, NULL, MOUSE_SERIAL_TIMER_HZ, TIMING_ENABLED);
}

static void machine_init_at_core(MACHINE_t* machine)
{
	FRONTEND_KEYBOARD_SINK_t keyboard_sink;

	showops = 0;
	i8259_init(&machine->i8259, 0x20, NULL);
	i8259_init(&machine->i8259_slave, 0xA0, &machine->i8259);
	i8253_init(&machine->i8253, &machine->i8259, &machine->pcspeaker);
	dma_init(&machine->dma, &machine->CPU);
	i8255_init(&machine->i8255, &machine->pcspeaker);
	pcspeaker_init(&machine->pcspeaker);
	i8042_init(&machine->CPU, &machine->i8259, &machine->i8259_slave, machine->kbc_profile);
	mouse_init_none();
	if (machine->mouse_connection == MACHINE_MOUSE_PS2) {
		mouse_init_ps2(&kbc.ports[1]);
	}
	keyboard_sink.handle_key = machine_handle_host_key;
	keyboard_sink.udata = &kbc;
	frontend_register_keyboard_sink(&keyboard_sink);
}

void machine_platform_reset(void)
{
	debug_log(DEBUG_DETAIL, "[MACHINE] Platform reset requested\n");

	pci_reset(&machine.pci);

	if (videocard == VIDEO_CARD_GD5440) {
		gd5440_reset();
	}

	if (machine.rtl8139 != NULL) {
		rtl8139_reset_device(machine.rtl8139);
	}

	if (machine.piix.pci != NULL) {
		piix_reset(&machine.piix);
	}

	ata_warm_reset();

	i8042_reset(&kbc);
	cpu_reset(&machine.CPU);
}

static int machine_init_video_device(MACHINE_t* machine)
{
	if (machine == NULL) {
		return -1;
	}

	switch (videocard) {
	case VIDEO_CARD_STDVGA:
		return vga_init();
	case VIDEO_CARD_GD5440:
		return gd5440_init(&machine->pci, 0x0C);
	default:
		debug_log(DEBUG_ERROR, "[VIDEO] Unsupported video card selection: %u\r\n", (unsigned)videocard);
		return -1;
	}
}

static int machine_resolve_mouse_connection(MACHINE_t* machine)
{
	MACHINE_MOUSE_CONNECTION_t requested;

	if (machine == NULL) {
		return -1;
	}

	requested = machine->mouse_connection;
	if (requested == MACHINE_MOUSE_DEFAULT) {
		requested = machine_has_ps2_mouse(machine) ? MACHINE_MOUSE_PS2 : MACHINE_MOUSE_UART0;
	}

	if ((requested == MACHINE_MOUSE_PS2) && !machine_has_ps2_mouse(machine)) {
		printf("The selected machine does not support a PS/2 mouse. Use -mouse uart0, -mouse uart1 or -mouse none.\r\n");
		return -1;
	}

	machine->mouse_connection = requested;
	return 0;
}

static void machine_build_uart_profile(MACHINE_t* machine, uint8_t index, MACHINE_UART_PROFILE_t* profile)
{
	memset(profile, 0, sizeof(*profile));

	if (!machine_mouse_uses_uart(machine, index)) {
		return;
	}

	profile->tx = mouse_tx;
	profile->mcr = mouse_togglereset;
	machine_attach_serial_mouse(&machine->UART[index]);
}

int machine_init_p55tp4xe(MACHINE_t* machine) {
	MACHINE_UART_PROFILE_t uart_profile[2];
	uint32_t total_ram_mb;

	if (machine == NULL) return -1;

	machine_init_at_core(machine);

	if ((machine->hwflags & MACHINE_HW_BLASTER) && !(machine->hwflags & MACHINE_HW_SKIP_BLASTER)) {
		blaster_init(&machine->blaster, &machine->dma, &machine->i8259, 0x220, 1, 5, 5);
		OPL3_init(&machine->OPL3);
		machine->mixBlaster = 1;
		machine->mixOPL = 1;
	}
	else if ((machine->hwflags & MACHINE_HW_OPL) && !(machine->hwflags & MACHINE_HW_SKIP_OPL)) {
		OPL3_init(&machine->OPL3);
		machine->mixOPL = 1;
	}
	machine_build_uart_profile(machine, 0, &uart_profile[0]);
	machine_build_uart_profile(machine, 1, &uart_profile[1]);

	pci_init(&machine->pci, PCI_CONFIG_TYPE_1, &machine->i8259, &machine->i8259_slave);
	pci_register_slot(&machine->pci, 0x00, PCI_CARD_NORTHBRIDGE, 0, 0, 0, 0);
	pci_register_slot(&machine->pci, 0x0C, PCI_CARD_NORMAL, 1, 2, 3, 4);
	pci_register_slot(&machine->pci, 0x0B, PCI_CARD_NORMAL, 2, 3, 4, 1);
	pci_register_slot(&machine->pci, 0x0A, PCI_CARD_NORMAL, 3, 4, 1, 2);
	pci_register_slot(&machine->pci, 0x09, PCI_CARD_NORMAL, 4, 1, 2, 3);
	pci_register_slot(&machine->pci, 0x07, PCI_CARD_SOUTHBRIDGE, 0, 0, 0, 0);

	total_ram_mb = 1U + (machine->extmem / MACHINE_MB_SIZE);
	if (i430fx_init(&machine->i430fx, &machine->pci, total_ram_mb) != 0) {
		return -1;
	}
	if (i430fx_load_bios(&machine->i430fx, "roms/machine/p55tp4xe/p55tp4xe.bin") != 0) {
		debug_log(DEBUG_ERROR, "[MACHINE] Could not load required BIOS ROM: roms/machine/p55tp4xe/p55tp4xe.bin\r\n");
		return -1;
	}
	piix_init(&machine->piix, &machine->pci);

	if (machine->nic_type == MACHINE_NIC_NE2000) {
		if (machine_attach_ne2000(machine, 0x300, 10) != 0) {
			return -1;
		}
	}
	else if (machine->nic_type == MACHINE_NIC_RTL8139) {
		if (rtl8139_init(&machine->rtl8139, &machine->pci, &machine->dma, 0x09, machine->nic_mac, machine->pcap_if) != 0) {
			return -1;
		}
	}

	cpu_reset(&machine->CPU);

	if (machine_init_floppy_storage(machine) != 0) {
		return -1;
	}
	ata_init_dual(&machine->i8259_slave);
	fdc37c665_init(&machine->fdc37c665, &machine->fdc, &machine->UART[0], &machine->UART[1], &machine->i8259,
		uart_profile[0].tx, uart_profile[0].udata, uart_profile[0].mcr, uart_profile[0].udata2,
		uart_profile[1].tx, uart_profile[1].udata, uart_profile[1].mcr, uart_profile[1].udata2);

	if (machine_init_buslogic_controller(machine, &machine->i8259_slave)) {
		return -1;
	}

	if (machine_init_video_device(machine)) {
		return -1;
	}
	return 0;
}

int machine_init_p55t2p4(MACHINE_t* machine) {
	MACHINE_UART_PROFILE_t uart_profile[2];
	uint32_t total_ram_mb;

	if (machine == NULL) return -1;

	machine_init_at_core(machine);

	if ((machine->hwflags & MACHINE_HW_BLASTER) && !(machine->hwflags & MACHINE_HW_SKIP_BLASTER)) {
		blaster_init(&machine->blaster, &machine->dma, &machine->i8259, 0x220, 1, 5, 5);
		OPL3_init(&machine->OPL3);
		machine->mixBlaster = 1;
		machine->mixOPL = 1;
	}
	else if ((machine->hwflags & MACHINE_HW_OPL) && !(machine->hwflags & MACHINE_HW_SKIP_OPL)) {
		OPL3_init(&machine->OPL3);
		machine->mixOPL = 1;
	}
	machine_build_uart_profile(machine, 0, &uart_profile[0]);
	machine_build_uart_profile(machine, 1, &uart_profile[1]);

	pci_init(&machine->pci, PCI_CONFIG_TYPE_1, &machine->i8259, &machine->i8259_slave);
	pci_register_slot(&machine->pci, 0x00, PCI_CARD_NORTHBRIDGE, 0, 0, 0, 0);
	pci_register_slot(&machine->pci, 0x0C, PCI_CARD_NORMAL, 1, 2, 3, 4);
	pci_register_slot(&machine->pci, 0x0B, PCI_CARD_NORMAL, 2, 3, 4, 1);
	pci_register_slot(&machine->pci, 0x0A, PCI_CARD_NORMAL, 3, 4, 1, 2);
	pci_register_slot(&machine->pci, 0x09, PCI_CARD_NORMAL, 4, 1, 2, 3);
	pci_register_slot(&machine->pci, 0x07, PCI_CARD_SOUTHBRIDGE, 0, 0, 0, 0);

	total_ram_mb = 1U + (machine->extmem / MACHINE_MB_SIZE);
	if (i430hx_init(&machine->i430hx, &machine->pci, total_ram_mb) != 0) {
		return -1;
	}
	if (i430hx_load_bios(&machine->i430hx, "roms/machine/p55t2p4/p55t2p4.bin") != 0) {
		debug_log(DEBUG_ERROR, "[MACHINE] Could not load required BIOS ROM: roms/machine/p55t2p4/p55t2p4.bin\r\n");
		return -1;
	}
	piix3_init(&machine->piix, &machine->pci);

	if (machine->nic_type == MACHINE_NIC_NE2000) {
		if (machine_attach_ne2000(machine, 0x300, 10) != 0) {
			return -1;
		}
	}
	else if (machine->nic_type == MACHINE_NIC_RTL8139) {
		if (rtl8139_init(&machine->rtl8139, &machine->pci, &machine->dma, 0x09, machine->nic_mac, machine->pcap_if) != 0) {
			return -1;
		}
	}

	cpu_reset(&machine->CPU);

	if (machine_init_floppy_storage(machine) != 0) {
		return -1;
	}
	ata_init_dual(&machine->i8259_slave);
	w83877_init(&machine->w83877, &machine->fdc, &machine->UART[0], &machine->UART[1], &machine->i8259,
		uart_profile[0].tx, uart_profile[0].udata, uart_profile[0].mcr, uart_profile[0].udata2,
		uart_profile[1].tx, uart_profile[1].udata, uart_profile[1].mcr, uart_profile[1].udata2);

	if (machine_init_buslogic_controller(machine, &machine->i8259_slave)) {
		return -1;
	}

	if (machine_init_video_device(machine)) {
		return -1;
	}
	return 0;
}

int machine_init(MACHINE_t* machine, char* id) {
	int num = 0, match = 0, i = 0;

	do {
		if (machine_defs[num].id == NULL) {
			debug_log(DEBUG_ERROR, "[MACHINE] ERROR: Machine definition not found: %s\r\n", id);
			return -1;
		}

		if (_stricmp(id, machine_defs[num].id) == 0) {
			match = 1;
		}
		else {
			num++;
		}
	} while (!match);

	debug_log(DEBUG_INFO, "[MACHINE] Initializing machine: \"%s\" (%s)\r\n", machine_defs[num].description, machine_defs[num].id);
	if (ramsize != 0) {
		debug_log(DEBUG_DETAIL, "[MACHINE] Overriding RAM size to %lu MB total)\r\n", (unsigned long)ramsize);
	}

	//Initialize machine memory map
	machine->extmem = 0;
	while(1) {
		static uint8_t* temp;
		uint32_t entry_size = machine_mem[num][i].size;
		if (machine_mem[num][i].memtype == MACHINE_MEM_ENDLIST) {
			break;
		}
		if (machine_mem[num][i].memtype == MACHINE_MEM_RAM) {
			entry_size = machine_get_ram_region_size(&machine_mem[num][i]);
			if (machine_mem[num][i].start == MACHINE_EXTENDED_RAM_BASE) {
				machine->extmem = entry_size;
			}
			if (entry_size == 0) {
				i++;
				continue;
			}
		}
		if (machine_mem[num][i].memtype != MACHINE_MEM_ROM_INTERLEAVED_HIGH) { // you have to put the low one right before high in the machine list...
			temp = (uint8_t*)malloc((size_t)entry_size);
		}
		if ((temp == NULL) &&
			((machine_mem[num][i].required == MACHINE_ROM_REQUIRED) || (machine_mem[num][i].required == MACHINE_ROM_ISNOTROM))) {
			debug_log(DEBUG_ERROR, "[MACHINE] ERROR: Unable to allocate %lu bytes of memory\r\n", entry_size);
			return -1;
		}
		if (machine_mem[num][i].memtype == MACHINE_MEM_RAM) {
			memory_mapRegister(machine_mem[num][i].start, entry_size, temp, temp);
		} else if (machine_mem[num][i].memtype == MACHINE_MEM_ROM) {
			int ret;
			ret = utility_loadFile(temp, machine_mem[num][i].size, machine_mem[num][i].filename);
			if ((machine_mem[num][i].required == MACHINE_ROM_REQUIRED) && ret) {
				debug_log(DEBUG_ERROR, "[MACHINE] Could not open file, or size is less than expected: %s\r\n", machine_mem[num][i].filename);
				return -1;
			}
			memory_mapRegister(machine_mem[num][i].start, machine_mem[num][i].size, temp, NULL);
		} else if (machine_mem[num][i].memtype == MACHINE_MEM_ROM_INTERLEAVED_LOW) {
			int ret;
			uint32_t j;
			uint8_t* temp2;
			temp2 = (uint8_t*)malloc((size_t)machine_mem[num][i].size >> 1);
			ret = utility_loadFile(temp2, machine_mem[num][i].size >> 1, machine_mem[num][i].filename);
			if ((machine_mem[num][i].required == MACHINE_ROM_REQUIRED) && ret) {
				debug_log(DEBUG_ERROR, "[MACHINE] Could not open file, or size is less than expected: %s\r\n", machine_mem[num][i].filename);
				return -1;
			}
			for (j = 0; j < (machine_mem[num][i].size >> 1); j++) {
				temp[j << 1] = temp2[j];
			}
			free(temp2);
		}
		else if (machine_mem[num][i].memtype == MACHINE_MEM_ROM_INTERLEAVED_HIGH) {
			int ret;
			uint32_t j;
			uint8_t* temp2;
			temp2 = (uint8_t*)malloc((size_t)machine_mem[num][i].size >> 1);
			ret = utility_loadFile(temp2, machine_mem[num][i].size >> 1, machine_mem[num][i].filename);
			if ((machine_mem[num][i].required == MACHINE_ROM_REQUIRED) && ret) {
				debug_log(DEBUG_ERROR, "[MACHINE] Could not open file, or size is less than expected: %s\r\n", machine_mem[num][i].filename);
				return -1;
			}
			for (j = 0; j < (machine_mem[num][i].size >> 1); j++) {
				temp[(j << 1) + 1] = temp2[j];
			}
			free(temp2);
			memory_mapRegister(machine_mem[num][i].start, machine_mem[num][i].size, temp, NULL);
		}
		i++;
	}

	if (machine->cmos_path[0] != 0) {
		cmosrtc_init(machine->cmos_path, &machine->i8259_slave);
	}
	else if (machine_defs[num].cmosfile != NULL) {
		cmosrtc_init(machine_defs[num].cmosfile, &machine->i8259_slave);
	}

	machine->hwflags |= machine_defs[num].hwflags;

	if (videocard == 0xFF) {
		videocard = machine_defs[num].video;
	}

	machine->kbc_profile = machine_defs[num].kbc_profile;

	if (machine_resolve_mouse_connection(machine) != 0) {
		return -1;
	}

	if ((*machine_defs[num].init)(machine)) { //call machine-specific init routine
		return -1;
	}

	return num;
}

void machine_list() {
	int machine = 0;

	printf("Valid " STR_TITLE " machines:\r\n");

	while(machine_defs[machine].id != NULL) {
		printf("%s: \"%s\"\r\n", machine_defs[machine].id, machine_defs[machine].description);
		machine++;
	}
}
