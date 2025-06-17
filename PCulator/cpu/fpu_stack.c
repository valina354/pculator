#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fpu.h"

FPU_t fpu;

void fpu_reset() {
    memset(&fpu, 0, sizeof(fpu));
    fpu.control = 0x037F;  // Default control word
    fpu.status = 0x0000;   // Status word cleared
    fpu.tag = 0xFFFF;      // All stack entries empty (11b)
    fpu.top = 0;
}

static int fpu_stack_index(int i) {
    return (fpu.top + i) & 7;
}

void fpu_stack_push(double value) {
    fpu.top = (fpu.top - 1) & 7;
    fpu.tag &= ~(3 << (fpu.top * 2));  // Set tag for full
    fpu.tag |= (0 << (fpu.top * 2));
    fpu.st[fpu.top] = value;
}

void fpu_stack_pop() {
    fpu.tag &= ~(3 << (fpu.top * 2));  // Set tag for empty
    fpu.tag |= (3 << (fpu.top * 2));
    fpu.top = (fpu.top + 1) & 7;
}

double fpu_get_st(int i) {
    int idx = fpu_stack_index(i);
    return fpu.st[idx];
}

void fpu_set_st(int i, double value) {
    int idx = fpu_stack_index(i);
    fpu.st[idx] = value;
    fpu.tag &= ~(3 << (idx * 2));
    fpu.tag |= (0 << (idx * 2));  // Full
}

int fpu_stack_empty(int i) {
    int idx = fpu_stack_index(i);
    return ((fpu.tag >> (idx * 2)) & 3) == 3;
}

void fpu_set_status_c0(int v) { fpu.status = (fpu.status & ~0x0100) | (v ? 0x0100 : 0); }
void fpu_set_status_c1(int v) { fpu.status = (fpu.status & ~0x0200) | (v ? 0x0200 : 0); }
void fpu_set_status_c2(int v) { fpu.status = (fpu.status & ~0x0400) | (v ? 0x0400 : 0); }
void fpu_set_status_c3(int v) { fpu.status = (fpu.status & ~0x4000) | (v ? 0x4000 : 0); }

void fpu_set_status_top() {
    fpu.status &= 0xC7FF; // Clear top bits
    fpu.status |= (fpu.top & 7) << 11;
}
