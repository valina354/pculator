#ifndef _MACHINE_H_
#define _MACHINE_H_

#include "config.h"
#include <stdint.h>
#include "cpu/cpu.h"
#include "chipset/i8042.h"
#include "chipset/i8259.h"
#include "chipset/i8253.h"
#include "chipset/dma.h"
#include "chipset/i8255.h"
#include "chipset/i430fx.h"
#include "chipset/i430hx.h"
#include "chipset/piix.h"
#include "chipset/fdc37c665.h"
#include "chipset/w83877.h"
#include "chipset/uart.h"
#include "pci.h"
#include "io/ne2000.h"
#include "audio/opl2.h"
#include "audio/nukedopl.h"
#include "audio/blaster.h"
#include "audio/pcspeaker.h"
#include "disk/fdc.h"
#include "scsi/buslogic.h"

#define MACHINE_MEM_RAM						0
#define MACHINE_MEM_ROM						1
#define MACHINE_MEM_ROM_INTERLEAVED_LOW		2
#define MACHINE_MEM_ROM_INTERLEAVED_HIGH	3
#define MACHINE_MEM_ENDLIST					4

#define MACHINE_ROM_OPTIONAL	0
#define MACHINE_ROM_REQUIRED	1
#define MACHINE_ROM_ISNOTROM	2

#define MACHINE_HW_OPL					0x0000000000000001ULL
#define MACHINE_HW_BLASTER				0x0000000000000002ULL
#define MACHINE_HW_NE2000				0x0000000000000400ULL
#define MACHINE_HW_RTL8139				0x0000000000000800ULL

//the "skip" HW flags are set in args.c to make sure machine init functions don't override explicit settings from the command line
#define MACHINE_HW_SKIP_OPL				0x8000000000000000ULL
#define MACHINE_HW_SKIP_BLASTER			0x4000000000000000ULL
#define MACHINE_HW_SKIP_DISK			0x0800000000000000ULL

typedef enum {
	MACHINE_NIC_NONE = 0,
	MACHINE_NIC_NE2000,
	MACHINE_NIC_RTL8139
} MACHINE_NIC_TYPE_t;

typedef enum {
	MACHINE_MOUSE_DEFAULT = 0,
	MACHINE_MOUSE_NONE,
	MACHINE_MOUSE_UART0,
	MACHINE_MOUSE_UART1,
	MACHINE_MOUSE_PS2
} MACHINE_MOUSE_CONNECTION_t;

struct RTL8139State;
typedef struct RTL8139State RTL8139State;

typedef struct {
	CPU_t CPU;
	I8259_t i8259, i8259_slave;
	I8253_t i8253;
	DMA_t dma;
	I8255_t i8255;
	PCI_t pci;
	I430FX_t i430fx;
	I430HX_t i430hx;
	PIIX_t piix;
	FDC37C665_t fdc37c665;
	W83877_t w83877;
	UART_t UART[2];
	OPL2_t OPL2;
	opl3_chip OPL3;
	uint8_t mixOPL;
	BLASTER_t blaster;
	uint8_t mixBlaster;
	PCSPEAKER_t pcspeaker;
	NE2000_t ne2000;
	RTL8139State* rtl8139;
	MACHINE_NIC_TYPE_t nic_type;
	uint8_t nic_mac[6];
	FDC_t fdc;
	char floppy_path[2][512];
	char cmos_path[512];
	BUSLOGIC_t* buslogic;
	uint8_t buslogic_enabled;
	uint16_t buslogic_base;
	uint8_t buslogic_irq;
	uint8_t buslogic_dma;
	uint32_t buslogic_bios_addr;
	char buslogic_rom_path[512];
	char buslogic_nvr_path[512];
	BUSLOGIC_TARGET_t scsi_targets[BUSLOGIC_MAX_TARGETS];
	uint64_t hwflags;
	uint32_t extmem;
	KBC_PROFILE_t kbc_profile;
	MACHINE_MOUSE_CONNECTION_t mouse_connection;
	int pcap_if;
} MACHINE_t;

typedef struct {
	uint8_t memtype;
	uint32_t start;
	uint32_t size;
	uint8_t required;
	char* filename;
} MACHINEMEM_t;

typedef struct {
	char* id;
	char* description;
	int (*init)(MACHINE_t* machine);
	char* cmosfile;
	uint8_t video;
	uint64_t hwflags;
	KBC_PROFILE_t kbc_profile;
} MACHINEDEF_t;

int machine_init_asus_386(MACHINE_t* machine);
int machine_init_opti5x7(MACHINE_t* machine);
int machine_init_p55tp4xe(MACHINE_t* machine);
int machine_init_p55t2p4(MACHINE_t* machine);
int machine_init_linux_386(MACHINE_t* machine);
int machine_init(MACHINE_t* machine, char* id);
void machine_list();
void machine_platform_reset(void);

#endif
