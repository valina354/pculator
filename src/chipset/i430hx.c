#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../debuglog.h"
#include "../memory.h"
#include "../pci.h"
#include "../utility.h"
#include "i430hx.h"

#define I430HX_SLOT 0x00
#define I430HX_LOW_SMRAM_ADDR 0x0A0000U
#define I430HX_LOW_SMRAM_SIZE 0x20000U
#define I430HX_BIOS_SEGMENT_SIZE 0x10000U
#define I430HX_DRB_REG_COUNT  8U
#define I430HX_DRB_UNIT_MB    4U
#define I430HX_DIMM_COUNT     4U
#define I430HX_MAX_RAM_MB     512U
#define I430HX_MIN_RAM_MB     4U
#define I430HX_MIN_MODULE_MB  4U
#define I430HX_MAX_DIMM_MB    128U

typedef struct {
	uint32_t addr;
	uint32_t len;
	uint8_t reg;
	uint8_t shift;
} I430HX_PAM_WINDOW_t;

static const I430HX_PAM_WINDOW_t i430hx_pam_windows[] = {
	{ 0x0C0000, 0x04000, 0x5A, 0 },
	{ 0x0C4000, 0x04000, 0x5A, 4 },
	{ 0x0C8000, 0x04000, 0x5B, 0 },
	{ 0x0CC000, 0x04000, 0x5B, 4 },
	{ 0x0D0000, 0x04000, 0x5C, 0 },
	{ 0x0D4000, 0x04000, 0x5C, 4 },
	{ 0x0D8000, 0x04000, 0x5D, 0 },
	{ 0x0DC000, 0x04000, 0x5D, 4 },
	{ 0x0E0000, 0x04000, 0x5E, 0 },
	{ 0x0E4000, 0x04000, 0x5E, 4 },
	{ 0x0E8000, 0x04000, 0x5F, 0 },
	{ 0x0EC000, 0x04000, 0x5F, 4 },
	{ 0x0F0000, 0x10000, 0x59, 4 }
};

static const uint32_t i430hx_bios_high_aliases[] = {
	0xFFFC0000U,
	0xFFFD0000U,
	0xFFFE0000U,
	0xFFFF0000U
};

static void i430hx_seed_drb(I430HX_t* dev, uint32_t total_ram_mb);
static void i430hx_refresh_smram(I430HX_t* dev);

static uint16_t i430hx_min_u16(uint16_t a, uint16_t b)
{
	return (a < b) ? a : b;
}

static uint16_t i430hx_pow2_floor_u16(uint16_t value)
{
	uint16_t result;

	result = 1;
	while ((uint16_t)(result << 1) <= value) {
		result = (uint16_t)(result << 1);
	}

	return result;
}

static uint8_t i430hx_is_pow2_u16(uint16_t value)
{
	return (uint8_t)((value != 0) && ((value & (uint16_t)(value - 1)) == 0));
}

static void i430hx_sort_rows_desc(uint16_t* rows, uint8_t count)
{
	uint8_t i;

	for (i = 1; i < count; i++) {
		uint16_t value;
		int8_t j;

		value = rows[i];
		j = (int8_t)i - 1;
		while ((j >= 0) && (rows[j] < value)) {
			rows[j + 1] = rows[j];
			j--;
		}
		rows[j + 1] = value;
	}
}

static void i430hx_populate_dimm_rows(uint16_t* rows, uint8_t slot_count, uint16_t total_size_mb, uint16_t min_module_size_mb, uint16_t max_module_size_mb)
{
	uint8_t row;
	uint8_t split;

	memset(rows, 0, (size_t)slot_count * sizeof(rows[0]));

	for (row = 0; (row < slot_count) && total_size_mb; row++) {
		rows[row] = i430hx_pow2_floor_u16(i430hx_min_u16(total_size_mb, max_module_size_mb));
		if (total_size_mb >= rows[row]) {
			total_size_mb = (uint16_t)(total_size_mb - rows[row]);
		} else {
			rows[row] = 0;
			break;
		}
	}

	split = (uint8_t)(total_size_mb == 0);
	while (split) {
		split = 0;

		for (row = 0; row < slot_count; row++) {
			uint8_t next_empty_row;

			if ((rows[row] <= (uint16_t)(min_module_size_mb << 1)) || !i430hx_is_pow2_u16(rows[row])) {
				continue;
			}

			next_empty_row = 0xFF;
			for (uint8_t i = (uint8_t)(row + 1); i < slot_count; i++) {
				if (rows[i] == 0) {
					next_empty_row = i;
					break;
				}
			}
			if (next_empty_row == 0xFF) {
				break;
			}

			rows[row] = (uint16_t)(rows[row] >> 1);
			rows[next_empty_row] = rows[row];
			i430hx_sort_rows_desc(rows, slot_count);
			split = 1;
			break;
		}
	}
}

static uint8_t* i430hx_shadow_ptr(I430HX_t* dev, uint32_t addr)
{
	return &dev->shadow_ram[addr - 0x0C0000];
}

static int i430hx_window_uses_bios_callback(const I430HX_PAM_WINDOW_t* window)
{
	return window->addr >= 0x0E0000;
}

static uint8_t i430hx_external_read_u8(I430HX_t* dev, uint32_t addr)
{
	if (dev->bios_flash.array != NULL) {
		return intel_flash_read(&dev->bios_flash, addr);
	}

	if ((dev->bios_rom != NULL) && (dev->bios_flash.size != 0)) {
		return dev->bios_rom[addr & (dev->bios_flash.size - 1U)];
	}

	return 0xFF;
}

static void i430hx_external_write_u8(I430HX_t* dev, uint32_t addr, uint8_t value)
{
	if (dev->bios_flash.array != NULL) {
		intel_flash_write(&dev->bios_flash, addr, value);
	}
}

static uint8_t i430hx_bios_external_read(void* udata, uint32_t addr)
{
	return i430hx_external_read_u8((I430HX_t*)udata, addr);
}

static void i430hx_bios_external_write(void* udata, uint32_t addr, uint8_t value)
{
	i430hx_external_write_u8((I430HX_t*)udata, addr, value);
}

static void i430hx_apply_window(I430HX_t* dev, const I430HX_PAM_WINDOW_t* window)
{
	MEMORY_DECODE_t state;
	uint8_t bits;

	bits = (uint8_t)((dev->config[window->reg] >> window->shift) & 0x03);
	state = (MEMORY_DECODE_t)bits;
	memory_mapSetDecodeState(window->addr, window->len, state);
}

static void i430hx_refresh_subset(I430HX_t* dev, uint8_t reg)
{
	uint8_t i;

	for (i = 0; i < (sizeof(i430hx_pam_windows) / sizeof(i430hx_pam_windows[0])); i++) {
		if (i430hx_pam_windows[i].reg == reg) {
			i430hx_apply_window(dev, &i430hx_pam_windows[i]);
		}
	}
}

static void i430hx_refresh_mappings(I430HX_t* dev)
{
	uint8_t i;

	for (i = 0; i < (sizeof(i430hx_pam_windows) / sizeof(i430hx_pam_windows[0])); i++) {
		i430hx_apply_window(dev, &i430hx_pam_windows[i]);
	}
}

static void i430hx_refresh_smram(I430HX_t* dev)
{
	uint8_t smram = (uint8_t)(dev->config[0x72] & 0x78);
	MEMORY_DECODE_t state = MEMORY_DECODE_EXTERNAL_EXTERNAL;

	switch (smram) {
	case 0x48:
		state = MEMORY_DECODE_EXTERNAL_INTERNAL;
		break;
	case 0x68:
		state = MEMORY_DECODE_INTERNAL_INTERNAL;
		break;
	case 0x38:
		state = MEMORY_DECODE_INTERNAL_EXTERNAL;
		break;
	default:
		if ((smram & 0x40) != 0) {
			state = MEMORY_DECODE_INTERNAL_EXTERNAL;
		}
		break;
	}

	memory_mapSetDecodeState(I430HX_LOW_SMRAM_ADDR, I430HX_LOW_SMRAM_SIZE, state);
}

static uint8_t i430hx_reset_bus_speed_bits(void)
{
	return 0x03; //0x01;
}

static uint8_t i430hx_read_config(void* udata, uint8_t function, uint8_t addr)
{
	I430HX_t* dev = (I430HX_t*)udata;

	if (function != 0) {
		return 0xFF;
	}

	return dev->config[addr];
}

static void i430hx_write_config(void* udata, uint8_t function, uint8_t addr, uint8_t value)
{
	I430HX_t* dev = (I430HX_t*)udata;

	if (function != 0) {
		return;
	}

	switch (addr) {
	case 0x0D:
		dev->config[0x0D] = value & 0xF8;
		break;
	case 0x0F:
		dev->config[0x0F] = value & 0x40;
		break;
	case 0x04:
		dev->config[0x04] = (dev->config[0x04] & (uint8_t)~0x02) | (value & 0x02);
		break;
	case 0x07:
		dev->config[0x07] &= (uint8_t)~(value & 0x70);
		break;
	case 0x52:
		dev->config[0x52] = (dev->config[0x52] & 0x04) | (value & 0xFB);
		break;
	case 0x50:
		dev->config[0x50] = value & 0xF7;
		break;
	case 0x57:
		dev->config[0x57] = value & 0xCF;
		break;
	case 0x58:
		dev->config[0x58] = value & 0x7F;
		break;
	case 0x59:
		dev->config[0x59] = value & 0x70;
		i430hx_refresh_subset(dev, 0x59);
		break;
	case 0x5A:
	case 0x5B:
	case 0x5C:
	case 0x5D:
	case 0x5E:
	case 0x5F:
		dev->config[addr] = value & 0x77;
		i430hx_refresh_subset(dev, addr);
		break;
	case 0x60:
	case 0x61:
	case 0x62:
	case 0x63:
	case 0x64:
	case 0x65:
	case 0x66:
	case 0x67:
		i430hx_seed_drb(dev, dev->total_ram_mb);
		break;
	case 0x68:
		dev->config[0x68] = value;
		break;
	case 0x72:
		if (dev->smram_locked) {
			dev->config[0x72] = (dev->config[0x72] & 0xDF) | (value & 0x20);
		} else {
			dev->config[0x72] = (dev->config[0x72] & 0x87) | (value & 0x78);
			dev->smram_locked = (uint8_t)((value & 0x10) != 0);
			if (dev->smram_locked) {
				dev->config[0x72] &= 0xBF;
			}
		}
		i430hx_refresh_smram(dev);
		break;
	default:
		break;
	}
}

static void i430hx_seed_drb(I430HX_t* dev, uint32_t total_ram_mb)
{
	uint16_t dimm_rows[I430HX_DIMM_COUNT];
	uint8_t i;

	if (total_ram_mb < I430HX_MIN_RAM_MB) {
		total_ram_mb = I430HX_MIN_RAM_MB;
	}
	if (total_ram_mb > I430HX_MAX_RAM_MB) {
		total_ram_mb = I430HX_MAX_RAM_MB;
	}

	total_ram_mb &= ~(I430HX_DRB_UNIT_MB - 1U);
	if (total_ram_mb < I430HX_MIN_RAM_MB) {
		total_ram_mb = I430HX_MIN_RAM_MB;
	}

	i430hx_populate_dimm_rows(dimm_rows,
		I430HX_DIMM_COUNT,
		(uint16_t)total_ram_mb,
		I430HX_MIN_MODULE_MB,
		I430HX_MAX_DIMM_MB);

	for (i = 0; i < I430HX_DRB_REG_COUNT; i++) {
		uint8_t dimm;
		uint16_t row_size_mb;
		uint8_t cumulative;

		dimm = (uint8_t)(i >> 1);
		row_size_mb = 0;
		if (dimm < I430HX_DIMM_COUNT) {
			if (dimm_rows[dimm] < (uint16_t)(I430HX_DRB_UNIT_MB << 1)) {
				row_size_mb = (i & 1U) ? 0 : I430HX_DRB_UNIT_MB;
			} else {
				row_size_mb = (uint16_t)(dimm_rows[dimm] >> 1);
			}
		}

		if (i == 0) {
			cumulative = 0;
		} else {
			cumulative = dev->config[0x60 + i - 1];
		}

		if (row_size_mb != 0) {
			cumulative = (uint8_t)(cumulative + (uint8_t)(row_size_mb / I430HX_DRB_UNIT_MB));
		}
		dev->config[0x60 + i] = cumulative;
	}
}

static void i430hx_register_static_maps(I430HX_t* dev)
{
	uint8_t i;

	memory_mapRegisterInternal(I430HX_LOW_SMRAM_ADDR,
		I430HX_LOW_SMRAM_SIZE,
		dev->low_smram,
		dev->low_smram);

	for (i = 0; i < (sizeof(i430hx_pam_windows) / sizeof(i430hx_pam_windows[0])); i++) {
		memory_mapRegisterInternal(i430hx_pam_windows[i].addr,
			i430hx_pam_windows[i].len,
			i430hx_shadow_ptr(dev, i430hx_pam_windows[i].addr),
			i430hx_shadow_ptr(dev, i430hx_pam_windows[i].addr));

		if (i430hx_window_uses_bios_callback(&i430hx_pam_windows[i])) {
			memory_mapCallbackRegister(i430hx_pam_windows[i].addr,
				i430hx_pam_windows[i].len,
				i430hx_bios_external_read, NULL, NULL,
				i430hx_bios_external_write, NULL, NULL,
				dev);
			continue;
		}
		if (i430hx_pam_windows[i].addr < 0x0C8000) {
			continue;
		}
		memory_mapRegister(i430hx_pam_windows[i].addr,
			i430hx_pam_windows[i].len,
			&dev->open_bus[i430hx_pam_windows[i].addr - 0x0C0000],
			NULL);
	}

	for (i = 0; i < (sizeof(i430hx_bios_high_aliases) / sizeof(i430hx_bios_high_aliases[0])); i++) {
		memory_mapCallbackRegister(i430hx_bios_high_aliases[i],
			I430HX_BIOS_SEGMENT_SIZE,
			i430hx_bios_external_read, NULL, NULL,
			i430hx_bios_external_write, NULL, NULL,
			dev);
	}
}

int i430hx_init(I430HX_t* dev, PCI_t* pci, uint32_t total_ram_mb)
{
	memset(dev, 0, sizeof(*dev));
	dev->pci = pci;
	dev->total_ram_mb = total_ram_mb;
	memset(dev->open_bus, 0xFF, sizeof(dev->open_bus));

	dev->config[0x00] = 0x86;
	dev->config[0x01] = 0x80;
	dev->config[0x02] = 0x50;
	dev->config[0x03] = 0x12;
	dev->config[0x08] = 0x00;
	dev->config[0x09] = 0x00;
	dev->config[0x0A] = 0x00;
	dev->config[0x0B] = 0x06;
	dev->config[0x52] = 0xB2;
	dev->config[0x57] = i430hx_reset_bus_speed_bits();
	dev->config[0x72] = 0x02;
	i430hx_seed_drb(dev, total_ram_mb);
	i430hx_register_static_maps(dev);
	i430hx_refresh_smram(dev);
	i430hx_refresh_mappings(dev);
	pci_register_device(pci, I430HX_SLOT, 0, i430hx_read_config, i430hx_write_config, dev);
	debug_log(DEBUG_DETAIL, "[i430HX] Intel 82439HX host bridge initialized\n");
	return 0;
}

int i430hx_load_bios(I430HX_t* dev, char* bios_path)
{
	intel_flash_destroy(&dev->bios_flash);
	dev->bios_rom = NULL;

	if (intel_flash_init_bxt(&dev->bios_flash, INTEL_FLASH_SIZE_128K) != 0) {
		return -1;
	}

	if (intel_flash_load_file(&dev->bios_flash, bios_path) != 0) {
		intel_flash_destroy(&dev->bios_flash);
		dev->bios_rom = NULL;
		return -1;
	}

	dev->bios_rom = dev->bios_flash.array;
	return 0;
}
