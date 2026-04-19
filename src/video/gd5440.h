#ifndef _GD5440_H_
#define _GD5440_H_

#include <stdint.h>
#include "../pci.h"

int gd5440_init(PCI_t* pci, uint8_t slot);
void gd5440_reset(void);
void gd5440_close(void);

#endif
