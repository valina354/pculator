#ifndef _PIIX_H_
#define _PIIX_H_

#include <stdint.h>
#include "../pci.h"

#define PIIX_FUNCTION_COUNT 3

typedef enum {
	PIIX_VARIANT_PIIX = 0,
	PIIX_VARIANT_PIIX3
} PIIX_VARIANT_t;

typedef struct {
	PCI_t* pci;
	uint8_t regs[PIIX_FUNCTION_COUNT][256];
	uint8_t function_present[PIIX_FUNCTION_COUNT];
	uint8_t port92_reg;
	uint8_t variant;
} PIIX_t;

void piix_init(PIIX_t* dev, PCI_t* pci);
void piix3_init(PIIX_t* dev, PCI_t* pci);
void piix_reset(PIIX_t* dev);

#endif
