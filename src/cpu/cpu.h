/*
  PCulator: A portable, open-source x86 PC emulator.
  Copyright (C)2025 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef _CPU_H_
#define _CPU_H_

#include <stdint.h>
#include "../host_endian.h"
#include "../config.h"
#include "../chipset/i8259.h"

typedef struct FPU FPU;

union _bytewordregs_ {
	uint32_t longregs[8];
	uint16_t wordregs[16];
	uint8_t byteregs[16];
};

typedef struct {
	uint32_t tag;
	uint32_t phys_base;
	uint32_t pte_addr;
	uint8_t flags;
} CPU_TLB_ENTRY_t;

typedef struct {
	CPU_TLB_ENTRY_t way[2];
	uint8_t mru;
} CPU_TLB_SET_t;

#define CPU_TLB_ENTRY_VALID		0x01
#define CPU_TLB_ENTRY_USER_OK	0x02
#define CPU_TLB_ENTRY_WRITE_OK	0x04
#define CPU_TLB_ENTRY_DIRTY		0x08

typedef struct {
	union _bytewordregs_ regs;
	uint8_t	opcode, segoverride, reptype, hltstate, isaddr32, isoper32, isCS32, iopl, nt, tr, cpl, startcpl, protected_mode, paging, usegdt, nowrite, currentseg;
	uint8_t sib, sib_scale, sib_index, sib_base;
	uint16_t segregs[6];
	uint32_t segcache[6];
	uint8_t segis32[6];
	uint32_t seglimit[6];
	uint8_t doexception, exceptionval;
	uint32_t exceptionerr, exceptionip;
	uint32_t savecs, saveip, ip, useseg, oldsp;
	uint8_t a20_gate;
	uint32_t cr[8], dr[8];
	uint8_t	tempcf, oldcf, cf, pf, af, zf, sf, tf, ifl, df, of, rf, v86f, acf, idf, mode, reg, rm;
	uint16_t oper1, oper2, res16, disp16, temp16, dummy, stacksize, frametemp;
	uint32_t oper1_32, oper2_32, res32, disp32;
	uint8_t	oper1b, oper2b, res8, disp8, temp8, nestlev, addrbyte;
	uint32_t sib_val, temp1, temp2, temp3, temp4, temp5, temp32, tempaddr32, frametemp32, ea;
	uint32_t gdtr, gdtl;
	uint32_t idtr, idtl;
	uint32_t ldtr, ldtl;
	uint32_t trbase, trlimit;
	uint32_t shadow_esp;
	uint8_t trtype;
	uint8_t have387;
	FPU *fpu;
	uint8_t bypass_paging;
	uint8_t interrupt_inhibit;
	uint16_t ldt_selector, tr_selector;
	int32_t	result;
	uint16_t trap_toggle;
	uint64_t totalexec, temp64, temp64_2, temp64_3;
	CPU_TLB_SET_t tlb[256];
} CPU_t;

#define regeax 0
#define regecx 1
#define regedx 2
#define regebx 3
#define regesp 4
#define regebp 5
#define regesi 6
#define regedi 7

#if HOST_BIG_ENDIAN
#define regax 1
#define regcx 3
#define regdx 5
#define regbx 7
#define regsp 9
#define regbp 11
#define regsi 13
#define regdi 15
#else
#define regax 0
#define regcx 2
#define regdx 4
#define regbx 6
#define regsp 8
#define regbp 10
#define regsi 12
#define regdi 14
#endif

#define reges 0
#define regcs 1
#define regss 2
#define regds 3
#define regfs 4
#define reggs 5

#if HOST_BIG_ENDIAN
#define regal 3
#define regah 2
#define regcl 7
#define regch 6
#define regdl 11
#define regdh 10
#define regbl 15
#define regbh 14
#else
#define regal 0
#define regah 1
#define regcl 4
#define regch 5
#define regdl 8
#define regdh 9
#define regbl 12
#define regbh 13
#endif

#define StepIP(mycpu, x)	mycpu->ip += x
#define getmem8(mycpu, x, y)	cpu_read(mycpu, x + y)
#define getmem16(mycpu, x, y)	cpu_readw(mycpu, x + y)
#define getmem32(mycpu, x, y)	cpu_readl(mycpu, x + y)
#define putmem8(mycpu, x, y, z)	cpu_write(mycpu, x + y, z)
#define putmem16(mycpu, x, y, z)	cpu_writew(mycpu, x + y, z)
#define putmem32(mycpu, x, y, z)	cpu_writel(mycpu, x + y, z)
#define signext(value)	(int16_t)(int8_t)(value)
#define signext8to32(value)	(int32_t)(int16_t)(int8_t)(value)
#define signext32(value)	(int32_t)(int16_t)(value)
#define signext64(value)	(int64_t)(int32_t)(int16_t)(value)
//#define getreg16(mycpu, regid)	mycpu->regs.wordregs[regid]
#define getreg8(mycpu, regid)	mycpu->regs.byteregs[byteregtable[regid]]
//#define putreg16(mycpu, regid, writeval)	mycpu->regs.wordregs[regid] = writeval
#define putreg8(mycpu, regid, writeval)	mycpu->regs.byteregs[byteregtable[regid]] = writeval
//#define getsegreg(mycpu, regid)	mycpu->segregs[regid]
//#define putsegreg(mycpu, regid, writeval)	mycpu->segregs[regid] = writeval
#define segbase(mycpu, x)	(cpu->protected_mode ? segtolinear(mycpu, x) :((uint32_t) x << 4))
#define exception(mycpu, x, y) if (!mycpu->doexception) { mycpu->doexception = 1; mycpu->exceptionval = x; mycpu->exceptionerr = y; if (x == 14) mycpu->nowrite = 1; if (showops) debug_log(DEBUG_DETAIL, "EX: %u (%08X)\n", x, y); }
#define ex_check(mycpu) if (mycpu->doexception) break

#define EFLAGS_CF (1 << 0)
#define EFLAGS_PF (1 << 2)
#define EFLAGS_AF (1 << 4)
#define EFLAGS_ZF (1 << 6)
#define EFLAGS_SF (1 << 7)
#define EFLAGS_TF (1 << 8)
#define EFLAGS_IF (1 << 9)
#define EFLAGS_DF (1 << 10)
#define EFLAGS_OF (1 << 11)
#define EFLAGS_IOPL (3 << 12)
#define EFLAGS_NT (1 << 14)
#define EFLAGS_RF (1 << 16)
#define EFLAGS_VM (1 << 17)

#define CR0_PE (1 << 0)
#define CR0_EM (1 << 2)
#define CR0_TS (1 << 3)
#define CR0_NE (1 << 5)

//Needs the following to detect as 8086/80186: (1 << 15) |
#define makeflagsword(x) \
	( \
	2 | (uint16_t) x->cf | ((uint16_t) x->pf << 2) | ((uint16_t) x->af << 4) | ((uint16_t) x->zf << 6) | ((uint16_t) x->sf << 7) | \
	((uint16_t) x->tf << 8) | ((uint16_t) x->ifl << 9) | ((uint16_t) x->df << 10) | ((uint16_t) x->of << 11) | \
	((uint32_t)x->iopl << 12) | ((uint32_t)x->nt << 14) | ((uint32_t)x->rf << 16) | ((uint32_t)x->v86f << 17) | ((uint32_t)x->acf << 18) | ((uint32_t) x->of << 11) | ((uint32_t) x->idf << 21) \
	)

#define decodeflagsword(x,y) { \
	uint32_t tmp; \
	tmp = y; \
	x->cf = tmp & 1; \
	x->pf = (tmp >> 2) & 1; \
	x->af = (tmp >> 4) & 1; \
	x->zf = (tmp >> 6) & 1; \
	x->sf = (tmp >> 7) & 1; \
	x->tf = (tmp >> 8) & 1; \
	if ((cpu->cpl == 0) || !cpu->protected_mode) x->ifl = (tmp >> 9) & 1; \
	x->df = (tmp >> 10) & 1; \
	x->of = (tmp >> 11) & 1; \
	if ((cpu->cpl == 0) || !cpu->protected_mode) x->iopl = (tmp >> 12) & 3; \
	x->nt = (tmp >> 14) & 1; \
	x->rf = (tmp >> 16) & 1; \
	x->v86f = (tmp >> 17) & 1; \
	if (x->v86f) cpu->cpl = 3; \
	x->acf = (tmp >> 18) & 1; \
	x->idf = (tmp >> 21) & 1; \
}

#define INT_SOURCE_EXCEPTION	0
#define INT_SOURCE_SOFTWARE		1
#define INT_SOURCE_HARDWARE		2
#define INT_SOURCE_INT3			3
#define INT_SOURCE_INTO			4

extern int showops;

void cpu_intcall(CPU_t* cpu, uint8_t intnum, uint8_t source, uint32_t err);
void cpu_reset(CPU_t* cpu);
int cpu_interruptCheck(CPU_t* cpu, I8259_t* i8259, int slave);
void cpu_exec(CPU_t* cpu, I8259_t* i8259, I8259_t* i8259_slave, uint32_t execloops);
void port_write(CPU_t* cpu, uint16_t portnum, uint8_t value);
void port_writew(CPU_t* cpu, uint16_t portnum, uint16_t value);
void port_writel(CPU_t* cpu, uint16_t portnum, uint32_t value);
uint8_t port_read(CPU_t* cpu, uint16_t portnum);
uint16_t port_readw(CPU_t* cpu, uint16_t portnum);
uint32_t port_readl(CPU_t* cpu, uint16_t portnum);
void cpu_registerIntCallback(CPU_t* cpu, uint8_t interrupt, void (*cb)(CPU_t*, uint8_t));
uint16_t getreg16(CPU_t* cpu, uint8_t reg);
void putreg16(CPU_t* cpu, uint8_t reg, uint16_t writeval);
uint32_t getsegreg(CPU_t* cpu, uint8_t reg);
void putsegreg(CPU_t* cpu, uint8_t reg, uint32_t writeval);
void modregrm(CPU_t* cpu);
void getea(CPU_t* cpu, uint8_t rmval);

#endif
