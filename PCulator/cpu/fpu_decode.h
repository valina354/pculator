#ifndef FPU_DECODE_H
#define FPU_DECODE_H

#include <stdint.h>
struct CPU_t;
void fpu_execute(struct CPU_t* cpu, uint8_t opcode);

#endif
