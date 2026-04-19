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
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include "host/host.h"
#include "config.h"
#include "timing.h"
#include "machine.h"
#include "cpu/cpu.h"
#include "chipset/i8259.h"
#include "chipset/i8253.h"
#include "chipset/i8255.h"
#include "chipset/uart.h"
#include "io/pcap.h"
#include "input/mouse.h"
#include "audio/pcspeaker.h"
#include "audio/opl2.h"
#include "audio/blaster.h"
#include "video/vga.h"
#include "disk/ata.h"
#include "frontends/frontend.h"
#include "debuglog.h"

volatile int render_priority = HOST_THREAD_PRIORITY_NORMAL;
volatile int no_affinity = 0;

int args_isMatch(char* s1, char* s2) {
	int i = 0, match = 1;

	while (1) {
		char c1, c2;
		c1 = s1[i];
		c2 = s2[i++];
		if ((c1 >= 'A') && (c1 <= 'Z')) {
			c1 -= 'A' - 'a';
		}
		if ((c2 >= 'A') && (c2 <= 'Z')) {
			c2 -= 'A' - 'a';
		}
		if (c1 != c2) {
			match = 0;
			break;
		}
		if (!c1 || !c2) {
			break;
		}
	}

	return match;
}

void args_showOption(int offset, int termwidth, const char* option, const char* help, ...) {
	char text[2048];
	size_t text_len;
	size_t option_len;
	int current_column;
	const char* ptr;
	va_list argptr;
	int count;

	if (offset < 2) {
		offset = 2;
	}
	if (termwidth <= offset) {
		termwidth = offset + 20;
	}
	if (option == NULL) {
		option = "";
	}
	if (help == NULL) {
		help = "";
	}

	va_start(argptr, help);
	vsnprintf(text, sizeof(text), help, argptr);
	va_end(argptr);

	text[sizeof(text) - 1] = 0;
	text_len = strlen(text);
	while ((text_len > 0) && ((text[text_len - 1] == '\r') || (text[text_len - 1] == '\n'))) {
		text[--text_len] = 0;
	}

	option_len = strlen(option);
	if (option_len > 0) {
		printf("  %s", option);
		current_column = 2 + (int)option_len;
	}
	else {
		current_column = 0;
	}

	if (text_len == 0) {
		printf("\r\n");
		return;
	}

	if (current_column > 0) {
		if (current_column < offset) {
			count = offset - current_column;
			while (count-- > 0) {
				putchar(' ');
			}
			current_column = offset;
		}
		else {
			printf("\r\n");
			count = offset;
			while (count-- > 0) {
				putchar(' ');
			}
			current_column = offset;
		}
	}
	else {
		count = offset;
		while (count-- > 0) {
			putchar(' ');
		}
		current_column = offset;
	}

	ptr = text;
	while (*ptr != 0) {
		const char* word_start;
		size_t word_len;

		while ((*ptr == ' ') || (*ptr == '\t')) {
			ptr++;
		}

		if (*ptr == 0) {
			break;
		}

		if ((*ptr == '\r') || (*ptr == '\n')) {
			if ((*ptr == '\r') && (ptr[1] == '\n')) {
				ptr++;
			}
			ptr++;
			printf("\r\n");
			count = offset;
			while (count-- > 0) {
				putchar(' ');
			}
			current_column = offset;
			continue;
		}

		word_start = ptr;
		while ((*ptr != 0) && (*ptr != ' ') && (*ptr != '\t') && (*ptr != '\r') && (*ptr != '\n')) {
			ptr++;
		}
		word_len = (size_t)(ptr - word_start);
		if (word_len == 0) {
			continue;
		}

		if ((current_column > offset) && ((current_column + 1 + (int)word_len) > termwidth)) {
			printf("\r\n");
			count = offset;
			while (count-- > 0) {
				putchar(' ');
			}
			current_column = offset;
		}
		else if (current_column > offset) {
			putchar(' ');
			current_column++;
		}

		fwrite(word_start, 1, word_len, stdout);
		current_column += (int)word_len;
	}

	printf("\r\n");
}

void args_showHelp() {
	int termwidth = host_get_terminal_columns(stdout);
	if (termwidth < 0) termwidth = 80;

	printf(STR_TITLE " command line parameters:\r\n\r\n");

	printf("Machine options:\r\n");
	args_showOption(25, termwidth, "-machine <id>", "Emulate machine definition defined by <id>. (Default is %s)", usemachine);
	args_showOption(25, termwidth, "", "Use -machine list to display <id> options.");
	args_showOption(25, termwidth, "-mem <size>", "Override machine RAM size with <size> MB total system memory from 1 to 512 MB.");
	args_showOption(25, termwidth, "", "Note: Not all machine chipsets/BIOSes support all RAM sizes.");
	args_showOption(25, termwidth, "-cmos <file>", "Override the default CMOS data file for the selected machine.");
	printf("\r\n");

	printf("Disk options:\r\n");
	args_showOption(25, termwidth, "-fd0 <file>", "Insert <file> disk image as floppy 0.");
	args_showOption(25, termwidth, "-fd1 <file>", "Insert <file> disk image as floppy 1.");
	args_showOption(25, termwidth, "-hd0 <file>", "Insert <file> disk image as hard disk 0. (IDE primary master)");
	args_showOption(25, termwidth, "-hd1 <file>", "Insert <file> disk image as hard disk 1. (IDE primary slave)");
	args_showOption(25, termwidth, "-hd2 <file>", "Insert <file> disk image as hard disk 2. (IDE secondary master)");
	args_showOption(25, termwidth, "-hd3 <file>", "Insert <file> disk image as hard disk 3. (IDE secondary slave)");
	args_showOption(25, termwidth, "-cd0 <file>", "Attach <file> as IDE ATAPI CD-ROM 0. (IDE primary master)");
	args_showOption(25, termwidth, "-cd1 <file>", "Attach <file> as IDE ATAPI CD-ROM 1. (IDE primary slave)");
	args_showOption(25, termwidth, "-cd2 <file>", "Attach <file> as IDE ATAPI CD-ROM 2. (IDE secondary master)");
	args_showOption(25, termwidth, "-cd3 <file>", "Attach <file> as IDE ATAPI CD-ROM 3. (IDE secondary slave)");
	args_showOption(25, termwidth, "", "Specify \".\" as <file> to create an empty ATAPI CD-ROM drive.");
	args_showOption(25, termwidth, "-buslogic", "Enable BusLogic BT-545S ISA SCSI controller emulation.");
	args_showOption(25, termwidth, "-buslogic-base <hex>", "Set BusLogic base port. (Default is 334h)");
	args_showOption(25, termwidth, "-buslogic-irq <num>", "Set BusLogic IRQ. (Default is 11)");
	args_showOption(25, termwidth, "-buslogic-dma <num>", "Set BusLogic DMA channel. (Default is 6)");
	args_showOption(25, termwidth, "-buslogic-bios <addr>", "Set BusLogic BIOS address: off, c800, d000 or d800.");
	args_showOption(25, termwidth, "-scsi-hd <id> <file>", "Attach <file> as SCSI hard disk target <id> (0-6).");
	args_showOption(25, termwidth, "-scsi-cd <id> <file>", "Attach <file> as SCSI CD-ROM target <id> (0-6).");
	args_showOption(25, termwidth, "", "Specify \".\" as <file> to create an empty CD-ROM drive at that target.");
	printf("\r\n");

	printf("Video options:\r\n");
	args_showOption(25, termwidth, "-video <type>", "Use <type> video card emulation. Valid values are stdvga or gd5440.");
	args_showOption(25, termwidth, "", "Default is gd5440 (Cirrus Logic GD5440).");
	args_showOption(25, termwidth, "-fpslock <FPS>", "Attempt to lock video refresh to <FPS> frames per second.");
	printf("\r\n");

	printf("Frontend options:\r\n");
	args_showOption(25, termwidth, "-frontend <name>", "Use compiled frontend <name>. (Default is %s)", frontend_get_default_name());
	args_showOption(25, termwidth, "", "Use -frontend list to display compiled frontends.");
	printf("\r\n");

	frontend_show_help();

	printf("Pointer options:\r\n");
	args_showOption(25, termwidth, "-mouse <type>", "Connect the host mouse to <type>: uart0, uart1, ps2 or none.");
	args_showOption(25, termwidth, "", "Valid <type> values for -mouse are: uart0, uart1, ps2, none.");
	printf("\r\n");

	printf("NIC options:\r\n");
	args_showOption(25, termwidth, "-nic <type>", "Select emulated NIC type: ne2000 or rtl8139.");
#ifdef USE_PCAP
	args_showOption(25, termwidth, "-net <id>", "Attach the selected NIC to host interface <id>. Use \"-net list\" to display available pcap interfaces.");
#else
	args_showOption(25, termwidth, "", "NOTE: This build did not include pcap. NIC will not receive or send any packets.");
#endif
	args_showOption(25, termwidth, "-mac <addr>", "Override the default NIC MAC address using aa:bb:cc:dd:ee:ff syntax.");
	args_showOption(25, termwidth, "", "This is required if running more than one instance of the emulator on the same LAN.");
	printf("\r\n");

	printf("Miscellaneous options:\r\n");
	args_showOption(25, termwidth, "-debug <level>", "<level> can be: NONE, ERROR, INFO, DETAIL. (Default is INFO)");
	args_showOption(25, termwidth, "-render-priority <val>", "Specify a different scheduling priority for the video render thread.");
	args_showOption(25, termwidth, "", "This can be useful on single-core hosts to give core emulation more cycles.");
	args_showOption(25, termwidth, "", "Valid values: lowest, lower, low, normal, high, higher, highest.");
#ifdef _WIN32
	args_showOption(25, termwidth, "-no-affinity", "Don't attempt to set process affinity to performance cores.");
#endif
	args_showOption(25, termwidth, "-h", "Show this help screen.");
}

static void args_init_buslogic_defaults(MACHINE_t* machine)
{
	if (machine->buslogic_base == 0) {
		machine->buslogic_base = 0x334;
	}
	if (machine->buslogic_irq == 0) {
		machine->buslogic_irq = 11;
	}
	if (machine->buslogic_dma == 0) {
		machine->buslogic_dma = 6;
	}
	if (machine->buslogic_rom_path[0] == 0) {
		strcpy(machine->buslogic_rom_path, "roms/scsi/buslogic/BT-545S_BIOS.rom");
	}
	if (machine->buslogic_nvr_path[0] == 0) {
		strcpy(machine->buslogic_nvr_path, "nvr/bt545s.nvr");
	}
}

static int args_parse_hex_nibble(char ch)
{
	if ((ch >= '0') && (ch <= '9')) {
		return ch - '0';
	}

	ch = (char)tolower((unsigned char)ch);
	if ((ch >= 'a') && (ch <= 'f')) {
		return 10 + (ch - 'a');
	}

	return -1;
}

static int args_parse_mac_address(const char* text, uint8_t mac[6])
{
	int i;

	if ((text == NULL) || (mac == NULL)) {
		return -1;
	}

	for (i = 0; i < 6; i++) {
		int hi;
		int lo;
		size_t pos = (size_t)i * 3;

		hi = args_parse_hex_nibble(text[pos]);
		lo = args_parse_hex_nibble(text[pos + 1]);
		if ((hi < 0) || (lo < 0)) {
			return -1;
		}

		mac[i] = (uint8_t)((hi << 4) | lo);
		if ((i < 5) && (text[pos + 2] != ':')) {
			return -1;
		}
	}

	return (text[17] == 0) ? 0 : -1;
}

static int args_parse_scsi_target_id(const char* text)
{
	char* end;
	long value;

	if (text == NULL) {
		return -1;
	}

	value = strtol(text, &end, 0);
	if ((end == text) || (*end != 0) || (value < 0) || (value >= BUSLOGIC_MAX_TARGETS)) {
		return -1;
	}

	return (int) value;
}

static void args_set_floppy_path(MACHINE_t* machine, uint8_t drive, const char* path)
{
	if ((machine == NULL) || (drive > 1) || (path == NULL)) {
		return;
	}

	strncpy(machine->floppy_path[drive], path, sizeof(machine->floppy_path[drive]) - 1);
	machine->floppy_path[drive][sizeof(machine->floppy_path[drive]) - 1] = 0;
}

static void args_set_cmos_path(MACHINE_t* machine, const char* path)
{
	if ((machine == NULL) || (path == NULL)) {
		return;
	}

	strncpy(machine->cmos_path, path, sizeof(machine->cmos_path) - 1);
	machine->cmos_path[sizeof(machine->cmos_path) - 1] = 0;
}

static int args_reserve_ide_slot(uint8_t slot_types[ATA_TOTAL_DEVICE_COUNT], uint8_t slot, uint8_t type, const char* option)
{
	if (slot >= ATA_TOTAL_DEVICE_COUNT) {
		return -1;
	}
	if ((slot_types[slot] != 0) && (slot_types[slot] != type)) {
		printf("Cannot assign both a hard disk and a CD-ROM to IDE slot %u.\r\n", (unsigned)slot);
		printf("Conflicting option: %s\r\n", option);
		return -1;
	}

	slot_types[slot] = type;
	return 0;
}

static int args_set_mouse_connection(MACHINE_t* machine, MACHINE_MOUSE_CONNECTION_t connection, const char* option)
{
	if (machine == NULL) {
		return -1;
	}

	if ((machine->mouse_connection != MACHINE_MOUSE_DEFAULT) && (machine->mouse_connection != connection)) {
		printf("Conflicting mouse connection options: %s\r\n", option);
		return -1;
	}

	machine->mouse_connection = connection;
	return 0;
}

int args_parse(MACHINE_t* machine, int argc, char* argv[]) {
	int i;
	uint8_t ide_slot_types[ATA_TOTAL_DEVICE_COUNT] = { 0, 0, 0, 0 };

//#ifndef _WIN32
	if (argc < 2) {
		printf("Specify command line parameters. Use -h for help.\r\n");
		return -1;
	}
//#endif

	args_init_buslogic_defaults(machine);

	for (i = 1; i < argc; i++) {
		if (args_isMatch(argv[i], "-h")) {
			args_showHelp();
			return -1;
		}
		else if (args_isMatch(argv[i], "-frontend")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -frontend. Use -h for help.\r\n");
				return -1;
			}

			i++;
			if (args_isMatch(argv[i], "list")) {
				frontend_print_available();
				return -1;
			}
			if (frontend_select_by_name(argv[i]) != 0) {
				printf("%s is not a compiled frontend.\r\n", argv[i]);
				frontend_print_available();
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-nic")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -nic. Use -h for help.\r\n");
				return -1;
			}

			i++;
			machine->hwflags &= ~(MACHINE_HW_NE2000 | MACHINE_HW_RTL8139);
			machine->nic_type = MACHINE_NIC_NONE;

			if (args_isMatch(argv[i], "ne2000")) {
				machine->nic_type = MACHINE_NIC_NE2000;
				machine->hwflags |= MACHINE_HW_NE2000;
			}
			else if (args_isMatch(argv[i], "rtl8139")) {
				machine->nic_type = MACHINE_NIC_RTL8139;
				machine->hwflags |= MACHINE_HW_RTL8139;
			}
			else {
				printf("%s is an invalid NIC type. Use ne2000 or rtl8139.\r\n", argv[i]);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-mac")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -mac. Use -h for help.\r\n");
				return -1;
			}

			i++;
			if (args_parse_mac_address(argv[i], machine->nic_mac) != 0) {
				printf("%s is not a valid MAC address. Use aa:bb:cc:dd:ee:ff syntax.\r\n", argv[i]);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-machine")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -machine. Use -h for help.\r\n");
				return -1;
			}
			i++;
			if (args_isMatch(argv[i], "list")) {
				machine_list();
				return -1;
			}
			usemachine = argv[i];
		}
		else if (args_isMatch(argv[i], "-cmos")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -cmos. Use -h for help.\r\n");
				return -1;
			}
			args_set_cmos_path(machine, argv[++i]);
		}
		else if (args_isMatch(argv[i], "-fd0")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -fd0. Use -h for help.\r\n");
				return -1;
			}
			args_set_floppy_path(machine, 0, argv[++i]);
		}
		else if (args_isMatch(argv[i], "-fd1")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -fd1. Use -h for help.\r\n");
				return -1;
			}
			args_set_floppy_path(machine, 1, argv[++i]);
		}
		else if (args_isMatch(argv[i], "-hd0")) {
			char* path;

			if ((i + 1) == argc) {
				printf("Parameter required for -hd0. Use -h for help.\r\n");
				return -1;
			}
			if (args_reserve_ide_slot(ide_slot_types, 0, 1, "-hd0") != 0) {
				return -1;
			}
			path = argv[++i];
			if (!ata_insert_disk(0, path)) {
				printf("Unable to open hard disk image: %s\r\n", path);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-hd1")) {
			char* path;

			if ((i + 1) == argc) {
				printf("Parameter required for -hd1. Use -h for help.\r\n");
				return -1;
			}
			if (args_reserve_ide_slot(ide_slot_types, 1, 1, "-hd1") != 0) {
				return -1;
			}
			path = argv[++i];
			if (!ata_insert_disk(1, path)) {
				printf("Unable to open hard disk image: %s\r\n", path);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-hd2")) {
			char* path;

			if ((i + 1) == argc) {
				printf("Parameter required for -hd2. Use -h for help.\r\n");
				return -1;
			}
			if (args_reserve_ide_slot(ide_slot_types, 2, 1, "-hd2") != 0) {
				return -1;
			}
			path = argv[++i];
			if (!ata_insert_disk(2, path)) {
				printf("Unable to open hard disk image: %s\r\n", path);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-hd3")) {
			char* path;

			if ((i + 1) == argc) {
				printf("Parameter required for -hd3. Use -h for help.\r\n");
				return -1;
			}
			if (args_reserve_ide_slot(ide_slot_types, 3, 1, "-hd3") != 0) {
				return -1;
			}
			path = argv[++i];
			if (!ata_insert_disk(3, path)) {
				printf("Unable to open hard disk image: %s\r\n", path);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-cd0")) {
			char* path;

			if ((i + 1) == argc) {
				printf("Parameter required for -cd0. Use -h for help.\r\n");
				return -1;
			}
			if (args_reserve_ide_slot(ide_slot_types, 0, 2, "-cd0") != 0) {
				return -1;
			}
			path = argv[++i];
			if (!ata_attach_cdrom(0, path)) {
				printf("Unable to open CD-ROM image: %s\r\n", path);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-cd1")) {
			char* path;

			if ((i + 1) == argc) {
				printf("Parameter required for -cd1. Use -h for help.\r\n");
				return -1;
			}
			if (args_reserve_ide_slot(ide_slot_types, 1, 2, "-cd1") != 0) {
				return -1;
			}
			path = argv[++i];
			if (!ata_attach_cdrom(1, path)) {
				printf("Unable to open CD-ROM image: %s\r\n", path);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-cd2")) {
			char* path;

			if ((i + 1) == argc) {
				printf("Parameter required for -cd2. Use -h for help.\r\n");
				return -1;
			}
			if (args_reserve_ide_slot(ide_slot_types, 2, 2, "-cd2") != 0) {
				return -1;
			}
			path = argv[++i];
			if (!ata_attach_cdrom(2, path)) {
				printf("Unable to open CD-ROM image: %s\r\n", path);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-cd3")) {
			char* path;

			if ((i + 1) == argc) {
				printf("Parameter required for -cd3. Use -h for help.\r\n");
				return -1;
			}
			if (args_reserve_ide_slot(ide_slot_types, 3, 2, "-cd3") != 0) {
				return -1;
			}
			path = argv[++i];
			if (!ata_attach_cdrom(3, path)) {
				printf("Unable to open CD-ROM image: %s\r\n", path);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-buslogic")) {
			machine->buslogic_enabled = 1;
		}
		else if (args_isMatch(argv[i], "-buslogic-base")) {
			unsigned long base;

			if ((i + 1) == argc) {
				printf("Parameter required for -buslogic-base. Use -h for help.\r\n");
				return -1;
			}

			base = strtoul(argv[++i], NULL, 0);
			if ((base == 0) || (base > 0xffff)) {
				printf("%s is an invalid BusLogic base port\r\n", argv[i]);
				return -1;
			}

			machine->buslogic_base = (uint16_t) base;
			machine->buslogic_enabled = 1;
		}
		else if (args_isMatch(argv[i], "-buslogic-irq")) {
			long irq;

			if ((i + 1) == argc) {
				printf("Parameter required for -buslogic-irq. Use -h for help.\r\n");
				return -1;
			}

			irq = strtol(argv[++i], NULL, 0);
			if ((irq < 3) || (irq > 15)) {
				printf("%s is an invalid BusLogic IRQ\r\n", argv[i]);
				return -1;
			}

			machine->buslogic_irq = (uint8_t) irq;
			machine->buslogic_enabled = 1;
		}
		else if (args_isMatch(argv[i], "-buslogic-dma")) {
			long dma;

			if ((i + 1) == argc) {
				printf("Parameter required for -buslogic-dma. Use -h for help.\r\n");
				return -1;
			}

			dma = strtol(argv[++i], NULL, 0);
			if ((dma < 5) || (dma > 7)) {
				printf("%s is an invalid BusLogic DMA channel\r\n", argv[i]);
				return -1;
			}

			machine->buslogic_dma = (uint8_t) dma;
			machine->buslogic_enabled = 1;
		}
		else if (args_isMatch(argv[i], "-buslogic-bios")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -buslogic-bios. Use -h for help.\r\n");
				return -1;
			}

			i++;
			if (args_isMatch(argv[i], "off")) {
				machine->buslogic_bios_addr = 0;
			}
			else if (args_isMatch(argv[i], "c800")) {
				machine->buslogic_bios_addr = 0xC8000;
			}
			else if (args_isMatch(argv[i], "d000")) {
				machine->buslogic_bios_addr = 0xD0000;
			}
			else if (args_isMatch(argv[i], "d800")) {
				machine->buslogic_bios_addr = 0xD8000;
			}
			else {
				printf("%s is an invalid BusLogic BIOS address\r\n", argv[i]);
				return -1;
			}

			machine->buslogic_enabled = 1;
		}
		else if (args_isMatch(argv[i], "-scsi-hd") || args_isMatch(argv[i], "-scsi-cd")) {
			uint8_t is_cdrom = args_isMatch(argv[i], "-scsi-cd");
			int target_id;
			BUSLOGIC_TARGET_t* target;

			if ((i + 2) >= argc) {
				printf("Parameters required for %s. Use -h for help.\r\n", argv[i]);
				return -1;
			}

			target_id = args_parse_scsi_target_id(argv[++i]);
			if (target_id < 0) {
				printf("%s is an invalid SCSI target ID\r\n", argv[i]);
				return -1;
			}

			target = &machine->scsi_targets[target_id];
			memset(target, 0, sizeof(*target));
			target->present = 1;
			target->type = is_cdrom ? BUSLOGIC_TARGET_CDROM : BUSLOGIC_TARGET_DISK;
			i++;
			if (!is_cdrom || !args_isMatch(argv[i], ".")) {
				strncpy(target->path, argv[i], sizeof(target->path) - 1);
				target->path[sizeof(target->path) - 1] = 0;
			}
			machine->buslogic_enabled = 1;
		}
		else if (args_isMatch(argv[i], "-video")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -video. Use -h for help.\r\n");
				return -1;
			}
			if (args_isMatch(argv[i + 1], "stdvga") || args_isMatch(argv[i + 1], "vga")) videocard = VIDEO_CARD_STDVGA;
			else if (args_isMatch(argv[i + 1], "gd5440")) videocard = VIDEO_CARD_GD5440;
			else {
				printf("%s is an invalid video card option\r\n", argv[i + 1]);
				return -1;
			}
			i++;
		}
		else if (args_isMatch(argv[i], "-fpslock")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -fpslock. Use -h for help.\r\n");
				return -1;
			}
			vga_lockFPS = atof(argv[++i]);
			if ((vga_lockFPS < 1) || (vga_lockFPS > 144)) {
				printf("%f is an invalid FPS option, valid range is 1 to 144\r\n", vga_lockFPS);
				return -1;
			}
		}
		else if (args_isMatch(argv[i], "-mem")) {
			char* end;
			unsigned long memsize_mb;

			if ((i + 1) == argc) {
				printf("Parameter required for -mem. Use -h for help.\r\n");
				return -1;
			}

			memsize_mb = strtoul(argv[++i], &end, 0);
			if ((end == argv[i]) || (*end != 0) || (memsize_mb < 1) || (memsize_mb > 512)) {
				printf("%s is an invalid memory size. Specify total RAM in MB from 1 to 512.\r\n", argv[i]);
				return -1;
			}

			ramsize = (uint32_t) memsize_mb;
		}
		else if (args_isMatch(argv[i], "-debug")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -debug. Use -h for help.\r\n");
				return -1;
			}
			if (args_isMatch(argv[i + 1], "none")) debug_setLevel(DEBUG_NONE);
			else if (args_isMatch(argv[i + 1], "error")) debug_setLevel(DEBUG_ERROR);
			else if (args_isMatch(argv[i + 1], "info")) debug_setLevel(DEBUG_INFO);
			else if (args_isMatch(argv[i + 1], "detail")) debug_setLevel(DEBUG_DETAIL);
			else {
				printf("%s is an invalid debug option\r\n", argv[i + 1]);
				return -1;
			}
			i++;
		}
		else if (args_isMatch(argv[i], "-mips")) {
			showMIPS = 1;
		}
		else if (args_isMatch(argv[i], "-mouse")) {
			MACHINE_MOUSE_CONNECTION_t connection;

			if ((i + 1) == argc) {
				printf("Parameter required for -mouse. Use -h for help.\r\n");
				return -1;
			}

			if (args_isMatch(argv[i + 1], "uart0")) {
				connection = MACHINE_MOUSE_UART0;
			}
			else if (args_isMatch(argv[i + 1], "uart1")) {
				connection = MACHINE_MOUSE_UART1;
			}
			else if (args_isMatch(argv[i + 1], "ps2")) {
				connection = MACHINE_MOUSE_PS2;
			}
			else if (args_isMatch(argv[i + 1], "none")) {
				connection = MACHINE_MOUSE_NONE;
			}
			else {
				printf("%s is not a valid parameter for -mouse. Use -h for help.\r\n", argv[i + 1]);
				return -1;
			}

			if (args_set_mouse_connection(machine, connection, "-mouse") != 0) {
				return -1;
			}

			i++;
		}
		else if (args_isMatch(argv[i], "-hw")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -hw. Use -h for help.\r\n");
				return -1;
			}
			if (args_isMatch(argv[i + 1], "opl")) {
				machine->hwflags |= MACHINE_HW_OPL;
			}
			else if (args_isMatch(argv[i + 1], "noopl")) {
				machine->hwflags |= MACHINE_HW_SKIP_OPL;
			}
			else if (args_isMatch(argv[i + 1], "blaster")) {
				machine->hwflags |= MACHINE_HW_BLASTER;
			}
			else if (args_isMatch(argv[i + 1], "noblaster")) {
				machine->hwflags |= MACHINE_HW_SKIP_BLASTER;
			}
			else {
				printf("%s is an invalid hardware option\r\n", argv[i + 1]);
				return -1;
			}
			i++;
		}
		else if (args_isMatch(argv[i], "-net")) {
#ifdef USE_PCAP
			if ((i + 1) == argc) {
				printf("Parameter required for -net. Use -h for help.\r\n");
				return -1;
			}
			i++;
			if (args_isMatch(argv[i], "list")) {
				pcap_listdevs();
				return -1;
			}
			machine->pcap_if = atoi(argv[i]);
#else
			printf("This PCulator build did not include pcap, so -net is not available.\r\n");
			printf("You can still attach a NIC, but it will not be able to send or receive any packets.\r\n");
			return -1;
#endif
		}
#ifdef _WIN32
		else if (args_isMatch(argv[i], "-no-affinity")) {
			no_affinity = 1;
		}
#endif
		else if (args_isMatch(argv[i], "-render-priority")) {
			if ((i + 1) == argc) {
				printf("Parameter required for -render-priority. Use -h for help.\r\n");
				return -1;
			}
			i++;
			if (args_isMatch(argv[i], "lowest")) {
				render_priority = HOST_THREAD_PRIORITY_LOWEST;
			}
			else if (args_isMatch(argv[i], "lower")) {
				render_priority = HOST_THREAD_PRIORITY_LOWER;
			}
			else if (args_isMatch(argv[i], "low")) {
				render_priority = HOST_THREAD_PRIORITY_LOW;
			}
			else if (args_isMatch(argv[i], "normal")) {
				render_priority = HOST_THREAD_PRIORITY_NORMAL;
			}
			else if (args_isMatch(argv[i], "high")) {
				render_priority = HOST_THREAD_PRIORITY_HIGH;
			}
			else if (args_isMatch(argv[i], "higher")) {
				render_priority = HOST_THREAD_PRIORITY_HIGHER;
			}
			else if (args_isMatch(argv[i], "highest")) {
				render_priority = HOST_THREAD_PRIORITY_HIGHEST;
			}
			else {
				printf("Invalid value -render-priority. Use -h for help.\r\n");
				return -1;
			}
		}
		else {
			FRONTEND_ARG_RESULT_t frontend_arg_result = frontend_try_parse_arg(argc, argv, &i);

			if (frontend_arg_result == FRONTEND_ARG_UNHANDLED) {
				printf("%s is not a valid parameter. Use -h for help.\r\n", argv[i]);
				return -1;
			}
			if (frontend_arg_result == FRONTEND_ARG_EXIT) {
				return -1;
			}
			if (frontend_arg_result == FRONTEND_ARG_ERROR) {
				return -1;
			}
		}
	}

	if ((machine->pcap_if > -1) && (machine->nic_type == MACHINE_NIC_NONE)) {
		printf("-net requires -nic to select an emulated network card.\r\n");
		return -1;
	}

	if ((machine->pcap_if < 0) && (machine->nic_type != MACHINE_NIC_NONE)) {
		printf("WARNING: A NIC is attached to the guest machine, but you haven't specified a pcap interface.\r\n");
		printf("The NIC will be seen by the guest, but will not be able to send or receive any packets.\r\n\r\n");
	}

	return 0;
}
