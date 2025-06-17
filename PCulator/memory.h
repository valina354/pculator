#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <stdint.h>
#include "config.h"
#include "cpu/cpu.h"

//#define MEMORY_RANGE		0x100000
//#define MEMORY_MASK			0x0FFFFF
#define MEMORY_RANGE		0x4000000
#define MEMORY_MASK			0x3FFFFFF

extern FUNC_INLINE uint8_t cpu_read(CPU_t* cpu, uint32_t addr);
uint16_t cpu_readw(CPU_t* cpu, uint32_t addr);
extern FUNC_INLINE void cpu_write(CPU_t* cpu, uint32_t addr32, uint8_t value);
extern FUNC_INLINE void cpu_writew(CPU_t* cpu, uint32_t addr32, uint16_t value);
extern FUNC_INLINE void cpu_writel(CPU_t* cpu, uint32_t addr32, uint32_t value);
void cpu_writew_linear(CPU_t* cpu, uint32_t addr32, uint16_t value);
void cpu_writel_linear(CPU_t* cpu, uint32_t addr32, uint32_t value);
extern FUNC_INLINE uint16_t cpu_readw(CPU_t* cpu, uint32_t addr32);
extern FUNC_INLINE uint32_t cpu_readl(CPU_t* cpu, uint32_t addr32);
uint16_t cpu_readw_linear(CPU_t* cpu, uint32_t addr32);
uint32_t cpu_readl_linear(CPU_t* cpu, uint32_t addr32);
void memory_mapRegister(uint32_t start, uint32_t len, uint8_t* readb, uint8_t* writeb);
void memory_mapCallbackRegister(uint32_t start, uint32_t count, uint8_t(*readb)(void*, uint32_t), void (*writeb)(void*, uint32_t, uint8_t), void* udata);
int memory_init();

void wrcache_init();
void wrcache_flush();

#endif
