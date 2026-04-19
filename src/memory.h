#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <stdint.h>
#include "config.h"
#include "cpu/cpu.h"

//#define MEMORY_RANGE		0x100000
//#define MEMORY_MASK			0x0FFFFF
#define MEMORY_RANGE		0x4000000
#define MEMORY_MASK			0x3FFFFFF

typedef enum {
	MEM_ACCESS_USER_READ = 0,
	MEM_ACCESS_USER_WRITE = 1,
	MEM_ACCESS_SUPERVISOR_READ = 2,
	MEM_ACCESS_SUPERVISOR_WRITE = 3,
} MEM_ACCESS_t;

typedef enum {
	MEMORY_DECODE_EXTERNAL_EXTERNAL = 0,
	MEMORY_DECODE_INTERNAL_EXTERNAL = 1,
	MEMORY_DECODE_EXTERNAL_INTERNAL = 2,
	MEMORY_DECODE_INTERNAL_INTERNAL = 3,
} MEMORY_DECODE_t;

typedef struct {
	uint8_t* read;
	uint8_t* write;
	uint8_t(*readcb)(void*, uint32_t);
	uint16_t(*readwcb)(void*, uint32_t);
	uint32_t(*readlcb)(void*, uint32_t);
	void (*writecb)(void*, uint32_t, uint8_t);
	void (*writewcb)(void*, uint32_t, uint16_t);
	void (*writelcb)(void*, uint32_t, uint32_t);
	void* udata;
} MEMORY_MAP_DESC_t;

typedef struct {
	uint32_t start;
	uint32_t len;
	uint8_t enabled;
	uint8_t internal;
	int map_index;
	MEMORY_MAP_DESC_t desc;
} MEMORY_MAPPING_t;

int memory_paging_enabled(CPU_t* cpu);

uint8_t cpu_read(CPU_t* cpu, uint32_t addr);
uint16_t cpu_readw(CPU_t* cpu, uint32_t addr);
uint32_t cpu_readl(CPU_t* cpu, uint32_t addr32);
void cpu_write(CPU_t* cpu, uint32_t addr32, uint8_t value);
void cpu_writew(CPU_t* cpu, uint32_t addr32, uint16_t value);
void cpu_writel(CPU_t* cpu, uint32_t addr32, uint32_t value);
uint8_t cpu_read_sys(CPU_t* cpu, uint32_t addr32);
uint16_t cpu_readw_sys(CPU_t* cpu, uint32_t addr32);
uint32_t cpu_readl_sys(CPU_t* cpu, uint32_t addr32);
void cpu_write_sys(CPU_t* cpu, uint32_t addr32, uint8_t value);
void cpu_writew_sys(CPU_t* cpu, uint32_t addr32, uint16_t value);
void cpu_writel_sys(CPU_t* cpu, uint32_t addr32, uint32_t value);
uint8_t memory_read_phys_u8(CPU_t* cpu, uint32_t addr32);
void memory_write_phys_u8(CPU_t* cpu, uint32_t addr32, uint8_t value);
void cpu_writew_linear(CPU_t* cpu, uint32_t addr32, uint16_t value);
void cpu_writel_linear(CPU_t* cpu, uint32_t addr32, uint32_t value);
uint16_t cpu_readw_linear(CPU_t* cpu, uint32_t addr32);
uint32_t cpu_readl_linear(CPU_t* cpu, uint32_t addr32);
void memory_mapRegister(uint32_t start, uint32_t len, uint8_t* readb, uint8_t* writeb);
void memory_mapRegisterInternal(uint32_t start, uint32_t len, uint8_t* readb, uint8_t* writeb);
void memory_mapCallbackRegister(uint32_t start, uint32_t count,
	uint8_t(*readb)(void*, uint32_t), uint16_t(*readw)(void*, uint32_t), uint32_t(*readl)(void*, uint32_t),
	void (*writeb)(void*, uint32_t, uint8_t), void (*writew)(void*, uint32_t, uint16_t), void (*writel)(void*, uint32_t, uint32_t),
	void* udata);
void memory_mapCallbackRegisterInternal(uint32_t start, uint32_t count,
	uint8_t(*readb)(void*, uint32_t), uint16_t(*readw)(void*, uint32_t), uint32_t(*readl)(void*, uint32_t),
	void (*writeb)(void*, uint32_t, uint8_t), void (*writew)(void*, uint32_t, uint16_t), void (*writel)(void*, uint32_t, uint32_t),
	void* udata);
void memory_mapping_add(MEMORY_MAPPING_t* mapping, uint32_t start, uint32_t len,
	uint8_t(*readb)(void*, uint32_t), uint16_t(*readw)(void*, uint32_t), uint32_t(*readl)(void*, uint32_t),
	void (*writeb)(void*, uint32_t, uint8_t), void (*writew)(void*, uint32_t, uint16_t), void (*writel)(void*, uint32_t, uint32_t),
	void* udata, uint8_t internal);
void memory_mapping_set_addr(MEMORY_MAPPING_t* mapping, uint32_t start, uint32_t len);
void memory_mapping_enable(MEMORY_MAPPING_t* mapping);
void memory_mapping_disable(MEMORY_MAPPING_t* mapping);
void memory_mapSetDecodeState(uint32_t start, uint32_t len, MEMORY_DECODE_t state);
int memory_init();
void memory_tlb_flush(CPU_t* cpu);
void memory_tlb_invalidate_page(CPU_t* cpu, uint32_t linear_addr);

void wrcache_init();
void wrcache_flush();

#endif
