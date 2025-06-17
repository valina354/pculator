#ifndef FPU_H
#define FPU_H

#include "cpu.h"

// FPU Exceptions (matches Intel spec)
#define FPU_EX_INVALID    0x01
#define FPU_EX_DENORMAL   0x02
#define FPU_EX_DIV0       0x04
#define FPU_EX_OVERFLOW   0x08
#define FPU_EX_UNDERFLOW  0x10
#define FPU_EX_PRECISION  0x20
#define FPU_EX_STACK      0x40

void fpu_init();
void fpu_exec(CPU_t* cpu);  // Returns cycles used

#endif

/*
#ifndef FPU_H
#define FPU_H

#include <stdint.h>

typedef struct {
    double st[8];
    uint8_t top;
    uint16_t control;
    uint16_t status;
    uint16_t tag;
} FPU_t;

extern FPU_t fpu;

void fpu_reset();
void fpu_load_st(double val);
void fpu_stack_pop();
void fpu_stack_push(double val);
void fpu_set_status_top();

void fpu_op_binary(uint8_t op, double val);
void fpu_compare(double a, double b, int unordered_equal);
void fpu_fldcw(uint16_t cw);

// For memory operations
uint64_t fpu_readrm64(void* cpu, uint8_t rm);
void fpu_writerm64(void* cpu, uint8_t rm, uint64_t val);

#endif
*/