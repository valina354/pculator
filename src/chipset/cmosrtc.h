#ifndef CMOSRTC_H
#define CMOSRTC_H

#include <stdint.h>
#include "i8259.h"

uint8_t cmosrtc_read(void* dummy, uint16_t addr);
void cmosrtc_write(void* dummy, uint16_t addr, uint8_t value);
void cmosrtc_init(char* cmosfilename, I8259_t* i8259);

#endif
