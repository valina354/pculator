#ifndef _FDC_H_
#define _FDC_H_

#include <stdint.h>
#include "../cpu/cpu.h"
#include "../chipset/i8259.h"
#include "../chipset/dma.h"

typedef struct FDC_s {
	void* impl;
} FDC_t;

int fdc_init(FDC_t* fdc, CPU_t* cpu, I8259_t* i8259, DMA_t* dma_controller);
int fdc_insert(FDC_t* fdc, uint8_t drive, char* path);
void fdc_eject(FDC_t* fdc, uint8_t drive);
const char* fdc_get_filename(FDC_t* fdc, uint8_t drive);
int fdc_is_inserted(FDC_t* fdc, uint8_t drive);
uint8_t fdc_io_read(FDC_t* fdc, uint16_t addr);
void fdc_io_write(FDC_t* fdc, uint16_t addr, uint8_t value);

#endif
