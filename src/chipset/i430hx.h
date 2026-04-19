#ifndef _I430HX_H_
#define _I430HX_H_

#include <stdint.h>
#include "../pci.h"
#include "intel_flash.h"

typedef struct {
	PCI_t* pci;
	uint8_t config[256];
	uint32_t total_ram_mb;
	uint8_t* bios_rom;
	INTEL_FLASH_t bios_flash;
	uint8_t low_smram[0x20000];
	uint8_t shadow_ram[0x40000];
	uint8_t open_bus[0x20000];
	uint8_t smram_locked;
} I430HX_t;

int i430hx_init(I430HX_t* dev, PCI_t* pci, uint32_t total_ram_mb);
int i430hx_load_bios(I430HX_t* dev, char* bios_path);

#endif
