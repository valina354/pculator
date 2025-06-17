#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "fpu.h"

extern FPU_t fpu;

void fpu_finit() {
    fpu_reset();  // Reset to default state
}

void fpu_load_st(double val) {
    fpu_stack_push(val);
}

void fpu_compare(double a, double b, int pop) {
    fpu_set_status_c0(a < b);
    fpu_set_status_c2(a != b);
    fpu_set_status_c3(a == b);
    if (pop) fpu_stack_pop();
    fpu_set_status_top();
}

void fpu_op_binary(int op, double val) {
    int st0i = fpu.top;
    switch (op) {
        case 0: fpu.st[st0i] += val; break; // FADD
        case 1: fpu.st[st0i] *= val; break; // FMUL
        case 2: fpu.st[st0i] = val - fpu.st[st0i]; break; // FCOM reversed
        case 3: fpu.st[st0i] = val / fpu.st[st0i]; break; // FDIV reversed
        case 4: fpu.st[st0i] -= val; break; // FSUB
        case 5: fpu.st[st0i] = fpu.st[st0i] / val; break; // FDIV
        default:
            printf("Unhandled FPU binary op: %d\n", op);
            break;
    }
}

void fpu_fstcw(uint16_t* out) {
    *out = fpu.control;
}

void fpu_fldcw(uint16_t val) {
    fpu.control = val;
}

void fpu_fnstsw(uint16_t* out) {
    fpu_set_status_top();
    *out = fpu.status;
}

void fpu_frstor(const uint8_t* mem) {
    fpu.control = *(uint16_t*)&mem[0];
    fpu.status  = *(uint16_t*)&mem[2];
    fpu.tag     = *(uint16_t*)&mem[4];
    fpu.top     = (fpu.status >> 11) & 7;

    for (int i = 0; i < 8; i++) {
        memcpy(&fpu.st[i], &mem[32 + (i * 10)], 10);  // 80-bit load (truncated to double)
    }
}

void fpu_fsave(uint8_t* mem) {
    *(uint16_t*)&mem[0] = fpu.control;
    *(uint16_t*)&mem[2] = fpu.status;
    *(uint16_t*)&mem[4] = fpu.tag;

    for (int i = 0; i < 8; i++) {
        memcpy(&mem[32 + (i * 10)], &fpu.st[i], 10);  // Store as 80-bit (from double)
    }
}
