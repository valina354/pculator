#ifndef _INTEL_FLASH_H_
#define _INTEL_FLASH_H_

#include <stdint.h>

#define INTEL_FLASH_BLOCK_COUNT 7
#define INTEL_FLASH_SIZE_128K   0x20000U

typedef struct {
	uint8_t command;
	uint8_t status;
	uint8_t* array;
	uint16_t flash_id;
	uint32_t program_addr;
	uint32_t size;
	uint32_t block_start[INTEL_FLASH_BLOCK_COUNT];
	uint32_t block_end[INTEL_FLASH_BLOCK_COUNT];
	uint32_t block_len[INTEL_FLASH_BLOCK_COUNT];
} INTEL_FLASH_t;

int intel_flash_init_bxt(INTEL_FLASH_t* flash, uint32_t size);
void intel_flash_destroy(INTEL_FLASH_t* flash);
void intel_flash_reset(INTEL_FLASH_t* flash);
int intel_flash_load_file(INTEL_FLASH_t* flash, char* bios_path);
uint8_t intel_flash_read(INTEL_FLASH_t* flash, uint32_t addr);
void intel_flash_write(INTEL_FLASH_t* flash, uint32_t addr, uint8_t value);

#endif
