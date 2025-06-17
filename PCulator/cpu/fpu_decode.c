#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "fpu.h"
#include "cpu.h"

extern FPU_t fpu;

void fpu_execute(CPU_t* cpu, uint8_t opcode) {
    modregrm(cpu);
    uint8_t st0i = fpu.top;
    uint8_t sti = (fpu.top + cpu->rm) & 7;

    switch (opcode) {
    case 0xD8: // FADD/...
        if (cpu->mode == 3) {
            fpu_op_binary(cpu->reg, fpu.st[sti]);
        } else {
            uint32_t raw = readrm32(cpu, cpu->rm);
            float fval; memcpy(&fval, &raw, 4);
            fpu_op_binary(cpu->reg, (double)fval);
        }
        break;

    case 0xD9:
        if (cpu->mode == 3) {
            switch (cpu->reg) {
                case 0: fpu_load_st(fpu.st[sti]); break;
                case 1: { double tmp = fpu.st[st0i]; fpu.st[st0i] = fpu.st[sti]; fpu.st[sti] = tmp; } break;
                case 4: fpu.st[st0i] = -fpu.st[st0i]; break;
                case 5: fpu.st[st0i] = fabs(fpu.st[st0i]); break;
                case 7: putreg16(cpu, regax, fpu.status); break;
                default: printf("Unhandled D9 reg %u\n", cpu->reg); break;
            }
        } else {
            switch (cpu->reg) {
                case 0: {
                    uint32_t raw = readrm32(cpu, cpu->rm);
                    float val; memcpy(&val, &raw, 4);
                    fpu_load_st((double)val); break;
                }
                case 3: {
                    float result = (float)fpu.st[st0i];
                    writerm32(cpu, cpu->rm, *(uint32_t*)&result);
                    fpu_stack_pop(); break;
                }
                case 5: fpu_fldcw(readrm16(cpu, cpu->rm)); break;
                case 7: writerm16(cpu, cpu->rm, fpu.status); break;
                default: printf("Unhandled D9 mem reg %u\n", cpu->reg); break;
            }
        }
        break;

    case 0xDA:
        if (cpu->mode != 3) {
            uint32_t raw = readrm32(cpu, cpu->rm);
            float val; memcpy(&val, &raw, 4);
            fpu_compare(fpu.st[st0i], (double)val, cpu->reg == 3);
        } else {
            printf("DA reg mode unsupported\n");
        }
        break;

    case 0xDB:
        if (cpu->mode == 3) {
            if (cpu->reg == 4) fpu.st[sti] = fpu.st[st0i];
        } else {
            switch (cpu->reg) {
                case 0: {
                    int32_t val = (int32_t)readrm32(cpu, cpu->rm);
                    fpu_load_st((double)val); break;
                }
                case 1:
                case 3: {
                    int32_t val = (int32_t)fpu.st[st0i];
                    writerm32(cpu, cpu->rm, *(uint32_t*)&val);
                    if (cpu->reg == 3) fpu_stack_pop();
                    break;
                }
                default:
                    printf("Unhandled DB mem reg %u\n", cpu->reg); break;
            }
        }
        break;

    case 0xDC: // FADD/FMUL/... mem64 or ST(i)
        if (cpu->mode == 3) {
            switch (cpu->reg) {
                case 0: fpu.st[sti] += fpu.st[st0i]; break;
                case 1: fpu.st[sti] *= fpu.st[st0i]; break;
                case 4: fpu.st[sti] -= fpu.st[st0i]; break;
                case 6: fpu.st[sti] /= fpu.st[st0i]; break;
                default: printf("Unhandled DC reg %u\n", cpu->reg); break;
            }
        } else {
            uint64_t raw = fpu_readrm64(cpu, cpu->rm);
            double val; memcpy(&val, &raw, 8);
            fpu_op_binary(cpu->reg, val);
        }
        break;

    case 0xDD:
        if (cpu->mode != 3) {
            switch (cpu->reg) {
                case 0: {
                    uint64_t raw = fpu_readrm64(cpu, cpu->rm);
                    double val; memcpy(&val, &raw, 8);
                    fpu_load_st(val); break;
                }
                case 2:
                case 3: {
                    double val = fpu.st[st0i];
                    uint64_t raw; memcpy(&raw, &val, 8);
                    fpu_writerm64(cpu, cpu->rm, raw);
                    if (cpu->reg == 3) fpu_stack_pop(); break;
                }
                default:
                    printf("Unhandled DD mem reg %u\n", cpu->reg); break;
            }
        }
        break;

    case 0xDE:
        if (cpu->mode == 3) {
            if (cpu->reg == 0 || cpu->reg == 1)
                fpu_compare(fpu.st[st0i], fpu.st[sti], cpu->reg == 1);
            else if (cpu->reg == 3) {
                fpu.st[sti] = fpu.st[st0i];
                fpu_stack_pop();
            }
        }
        break;

    case 0xDF:
        if (cpu->mode != 3) {
            switch (cpu->reg) {
                case 0: {
                    int16_t val = (int16_t)readrm16(cpu, cpu->rm);
                    fpu_load_st((double)val); break;
                }
                case 2:
                case 3: {
                    int16_t val = (int16_t)fpu.st[st0i];
                    writerm16(cpu, cpu->rm, *(uint16_t*)&val);
                    if (cpu->reg == 3) fpu_stack_pop(); break;
                }
                default:
                    printf("Unhandled DF mem reg %u\n", cpu->reg); break;
            }
        }
        break;

    default:
        printf("Unhandled FPU opcode %02X\n", opcode);
        break;
    }

    fpu_set_status_top();
}
