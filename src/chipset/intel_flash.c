#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../debuglog.h"
#include "intel_flash.h"

enum {
	INTEL_FLASH_BLOCK_MAIN1 = 0,
	INTEL_FLASH_BLOCK_MAIN2,
	INTEL_FLASH_BLOCK_MAIN3,
	INTEL_FLASH_BLOCK_MAIN4,
	INTEL_FLASH_BLOCK_DATA1,
	INTEL_FLASH_BLOCK_DATA2,
	INTEL_FLASH_BLOCK_BOOT
};

enum {
	INTEL_FLASH_CMD_READ_ARRAY = 0xFF,
	INTEL_FLASH_CMD_IID = 0x90,
	INTEL_FLASH_CMD_READ_STATUS = 0x70,
	INTEL_FLASH_CMD_CLEAR_STATUS = 0x50,
	INTEL_FLASH_CMD_ERASE_SETUP = 0x20,
	INTEL_FLASH_CMD_ERASE_CONFIRM = 0xD0,
	INTEL_FLASH_CMD_PROGRAM_SETUP = 0x40,
	INTEL_FLASH_CMD_PROGRAM_SETUP_ALT = 0x10
};

static uint32_t intel_flash_mask_addr(const INTEL_FLASH_t* flash, uint32_t addr)
{
	if ((flash == NULL) || (flash->size == 0)) {
		return 0;
	}

	return addr & (flash->size - 1U);
}

static int intel_flash_addr_in_block(const INTEL_FLASH_t* flash, uint8_t index, uint32_t addr)
{
	if ((flash == NULL) || (index >= INTEL_FLASH_BLOCK_COUNT)) {
		return 0;
	}

	if (flash->block_len[index] == 0) {
		return 0;
	}

	return (addr >= flash->block_start[index]) && (addr <= flash->block_end[index]);
}

int intel_flash_init_bxt(INTEL_FLASH_t* flash, uint32_t size)
{
	if ((flash == NULL) || (size != INTEL_FLASH_SIZE_128K)) {
		return -1;
	}

	memset(flash, 0, sizeof(*flash));
	flash->array = (uint8_t*)malloc(size);
	if (flash->array == NULL) {
		return -1;
	}

	memset(flash->array, 0xFF, size);
	flash->size = size;
	flash->flash_id = 0x94;
	flash->block_len[INTEL_FLASH_BLOCK_MAIN1] = 0x1C000;
	flash->block_len[INTEL_FLASH_BLOCK_DATA1] = 0x01000;
	flash->block_len[INTEL_FLASH_BLOCK_DATA2] = 0x01000;
	flash->block_len[INTEL_FLASH_BLOCK_BOOT] = 0x02000;

	flash->block_start[INTEL_FLASH_BLOCK_MAIN1] = 0x00000;
	flash->block_end[INTEL_FLASH_BLOCK_MAIN1] = 0x1BFFF;
	flash->block_start[INTEL_FLASH_BLOCK_DATA1] = 0x1C000;
	flash->block_end[INTEL_FLASH_BLOCK_DATA1] = 0x1CFFF;
	flash->block_start[INTEL_FLASH_BLOCK_DATA2] = 0x1D000;
	flash->block_end[INTEL_FLASH_BLOCK_DATA2] = 0x1DFFF;
	flash->block_start[INTEL_FLASH_BLOCK_BOOT] = 0x1E000;
	flash->block_end[INTEL_FLASH_BLOCK_BOOT] = 0x1FFFF;

	intel_flash_reset(flash);
	return 0;
}

void intel_flash_destroy(INTEL_FLASH_t* flash)
{
	if (flash == NULL) {
		return;
	}

	free(flash->array);
	memset(flash, 0, sizeof(*flash));
}

void intel_flash_reset(INTEL_FLASH_t* flash)
{
	if (flash == NULL) {
		return;
	}

	flash->command = INTEL_FLASH_CMD_READ_ARRAY;
	flash->status = 0;
	flash->program_addr = 0;
}

int intel_flash_load_file(INTEL_FLASH_t* flash, char* bios_path)
{
	FILE* file;

	if ((flash == NULL) || (flash->array == NULL) || (bios_path == NULL)) {
		return -1;
	}

	file = fopen(bios_path, "rb");
	if (file == NULL) {
		return -1;
	}

	if (fread(flash->array, 1, flash->size, file) < flash->size) {
		fclose(file);
		return -1;
	}

	fclose(file);
	return 0;
}

uint8_t intel_flash_read(INTEL_FLASH_t* flash, uint32_t addr)
{
	if ((flash == NULL) || (flash->array == NULL)) {
		return 0xFF;
	}

	addr = intel_flash_mask_addr(flash, addr);

	switch (flash->command) {
	case INTEL_FLASH_CMD_IID:
		return (addr & 1U) ? (uint8_t)(flash->flash_id & 0xFF) : 0x89;
	case INTEL_FLASH_CMD_READ_STATUS:
		return flash->status;
	case INTEL_FLASH_CMD_READ_ARRAY:
	default:
		return flash->array[addr];
	}
}

void intel_flash_write(INTEL_FLASH_t* flash, uint32_t addr, uint8_t value)
{
	uint8_t i;

	if ((flash == NULL) || (flash->array == NULL)) {
		return;
	}

	addr = intel_flash_mask_addr(flash, addr);

	switch (flash->command) {
	case INTEL_FLASH_CMD_ERASE_SETUP:
		if (value == INTEL_FLASH_CMD_ERASE_CONFIRM) {
			for (i = 0; i < (INTEL_FLASH_BLOCK_COUNT - 1); i++) {
				if ((i == flash->program_addr) && intel_flash_addr_in_block(flash, i, addr)) {
					memset(&flash->array[flash->block_start[i]], 0xFF, flash->block_len[i]);
				}
			}
			flash->status = 0x80;
		}
		flash->command = INTEL_FLASH_CMD_READ_STATUS;
		return;

	case INTEL_FLASH_CMD_PROGRAM_SETUP:
	case INTEL_FLASH_CMD_PROGRAM_SETUP_ALT:
		if ((addr == flash->program_addr) && !intel_flash_addr_in_block(flash, INTEL_FLASH_BLOCK_BOOT, addr)) {
			flash->array[addr] = value;
		}
		flash->command = INTEL_FLASH_CMD_READ_STATUS;
		flash->status = 0x80;
		return;

	default:
		flash->command = value;
		switch (value) {
		case INTEL_FLASH_CMD_CLEAR_STATUS:
			flash->status = 0;
			break;
		case INTEL_FLASH_CMD_ERASE_SETUP:
			for (i = 0; i < INTEL_FLASH_BLOCK_COUNT; i++) {
				if (intel_flash_addr_in_block(flash, i, addr)) {
					flash->program_addr = i;
					break;
				}
			}
			break;
		case INTEL_FLASH_CMD_PROGRAM_SETUP:
		case INTEL_FLASH_CMD_PROGRAM_SETUP_ALT:
			flash->program_addr = addr;
			break;
		default:
			break;
		}
		break;
	}
}
