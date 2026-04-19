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

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "cpu/cpu.h"
#include "debuglog.h"
#include "memory.h"
#include "machine.h"

extern MACHINE_t machine;

struct mem_s {
	uint32_t start;
	uint32_t size;
	MEMORY_MAP_DESC_t desc;
	int used;
} mem[64];

//We use a memory write cache during instruction execution and only commit/flush to
//actual RAM after an instruction completes with no faults
struct writecache_s {
	uint32_t addr;
	uint32_t value;
	uint8_t size;
} wrcache[512]; //512 should be more than enough

uint16_t wrcache_count = 0;

uint8_t mem_map_lookup_external[1 << 20]; //4 KB page lookup for external owners
uint8_t mem_map_lookup_internal[1 << 20]; //4 KB page lookup for internal owners
uint8_t mem_decode_lookup[1 << 20]; //4 KB page decode state

static void cpu_write_linear(CPU_t* cpu, uint32_t addr32, uint8_t value);

//extern int showops;

#define getpage(addr) ((addr) >> 12)

static FUNC_FORCE_INLINE void memory_desc_clear(MEMORY_MAP_DESC_t* desc) {
	desc->read = NULL;
	desc->write = NULL;
	desc->readcb = NULL;
	desc->readwcb = NULL;
	desc->readlcb = NULL;
	desc->writecb = NULL;
	desc->writewcb = NULL;
	desc->writelcb = NULL;
	desc->udata = NULL;
}

static FUNC_FORCE_INLINE void memory_desc_copy(MEMORY_MAP_DESC_t* dst, const MEMORY_MAP_DESC_t* src) {
	dst->read = src->read;
	dst->write = src->write;
	dst->readcb = src->readcb;
	dst->readwcb = src->readwcb;
	dst->readlcb = src->readlcb;
	dst->writecb = src->writecb;
	dst->writewcb = src->writewcb;
	dst->writelcb = src->writelcb;
	dst->udata = src->udata;
}

static FUNC_FORCE_INLINE int memory_access_stays_on_page(uint32_t addr32, uint8_t size) {
	return ((addr32 & 0xFFFu) + (uint32_t)size) <= 0x1000u;
}

static FUNC_FORCE_INLINE int memory_map_contains_range(const struct mem_s* map, uint32_t addr32, uint8_t size) {
	if ((map == NULL) || (addr32 < map->start)) {
		return 0;
	}

	return ((uint64_t)(addr32 - map->start) + (uint64_t)size) <= (uint64_t)map->size;
}

static FUNC_FORCE_INLINE uint16_t memory_load_u16(const uint8_t* ptr) {
	return (uint16_t)ptr[0] | (uint16_t)((uint16_t)ptr[1] << 8);
}

static FUNC_FORCE_INLINE uint32_t memory_load_u32(const uint8_t* ptr) {
	return (uint32_t)ptr[0] |
		((uint32_t)ptr[1] << 8) |
		((uint32_t)ptr[2] << 16) |
		((uint32_t)ptr[3] << 24);
}

static FUNC_FORCE_INLINE void memory_store_u16(uint8_t* ptr, uint16_t value) {
	ptr[0] = (uint8_t)value;
	ptr[1] = (uint8_t)(value >> 8);
}

static FUNC_FORCE_INLINE void memory_store_u32(uint8_t* ptr, uint32_t value) {
	ptr[0] = (uint8_t)value;
	ptr[1] = (uint8_t)(value >> 8);
	ptr[2] = (uint8_t)(value >> 16);
	ptr[3] = (uint8_t)(value >> 24);
}

static FUNC_FORCE_INLINE uint8_t memory_desc_read_u8(const struct mem_s* map, const MEMORY_MAP_DESC_t* desc, uint32_t addr32) {
	if (desc->read != NULL) {
		return desc->read[addr32 - map->start];
	}

	if (desc->readcb != NULL) {
		return (*desc->readcb)(desc->udata, addr32);
	}

	return 0xFF;
}

static FUNC_FORCE_INLINE int memory_desc_read_u16_direct(const struct mem_s* map, const MEMORY_MAP_DESC_t* desc, uint32_t addr32, uint16_t* value) {
	if (desc->read != NULL) {
		*value = memory_load_u16(&desc->read[addr32 - map->start]);
		return 1;
	}

	if (desc->readwcb != NULL) {
		*value = (*desc->readwcb)(desc->udata, addr32);
		return 1;
	}

	return 0;
}

static FUNC_FORCE_INLINE int memory_desc_read_u32_direct(const struct mem_s* map, const MEMORY_MAP_DESC_t* desc, uint32_t addr32, uint32_t* value) {
	if (desc->read != NULL) {
		*value = memory_load_u32(&desc->read[addr32 - map->start]);
		return 1;
	}

	if (desc->readlcb != NULL) {
		*value = (*desc->readlcb)(desc->udata, addr32);
		return 1;
	}

	if (desc->readwcb != NULL) {
		*value = (uint32_t)(*desc->readwcb)(desc->udata, addr32) |
			((uint32_t)(*desc->readwcb)(desc->udata, addr32 + 2) << 16);
		return 1;
	}

	return 0;
}

static FUNC_FORCE_INLINE void memory_desc_write_u8(const struct mem_s* map, const MEMORY_MAP_DESC_t* desc, uint32_t addr32, uint8_t value) {
	if (desc->write != NULL) {
		desc->write[addr32 - map->start] = value;
		return;
	}

	if (desc->writecb != NULL) {
		(*desc->writecb)(desc->udata, addr32, value);
	}
}

static FUNC_FORCE_INLINE int memory_desc_write_u16_direct(const struct mem_s* map, const MEMORY_MAP_DESC_t* desc, uint32_t addr32, uint16_t value) {
	if (desc->write != NULL) {
		memory_store_u16(&desc->write[addr32 - map->start], value);
		return 1;
	}

	if (desc->writewcb != NULL) {
		(*desc->writewcb)(desc->udata, addr32, value);
		return 1;
	}

	return 0;
}

static FUNC_FORCE_INLINE int memory_desc_write_u32_direct(const struct mem_s* map, const MEMORY_MAP_DESC_t* desc, uint32_t addr32, uint32_t value) {
	if (desc->write != NULL) {
		memory_store_u32(&desc->write[addr32 - map->start], value);
		return 1;
	}

	if (desc->writelcb != NULL) {
		(*desc->writelcb)(desc->udata, addr32, value);
		return 1;
	}

	if (desc->writewcb != NULL) {
		(*desc->writewcb)(desc->udata, addr32, (uint16_t)value);
		(*desc->writewcb)(desc->udata, addr32 + 2, (uint16_t)(value >> 16));
		return 1;
	}

	return 0;
}

static FUNC_FORCE_INLINE uint8_t memory_get_decode_state(uint32_t addr32) {
	return mem_decode_lookup[getpage(addr32)] & 0x03;
}

static FUNC_FORCE_INLINE int memory_lookup_map(uint32_t addr32, int is_write) {
	uint8_t state = memory_get_decode_state(addr32);
	uint8_t map;

	if ((state & (is_write ? 0x02 : 0x01)) != 0) {
		map = mem_map_lookup_internal[getpage(addr32)];
	}
	else {
		map = mem_map_lookup_external[getpage(addr32)];
	}

	return map;
}

static FUNC_FORCE_INLINE struct mem_s* memory_lookup_map_range(uint32_t addr32, uint8_t size, int is_write) {
	int map = memory_lookup_map(addr32, is_write);

	if (map == 0xFF) {
		return NULL;
	}

	if (!memory_map_contains_range(&mem[map], addr32, size)) {
		return NULL;
	}

	return &mem[map];
}

/*FUNC_INLINE int getmap(uint32_t addr32) {
	int i;
	for (i = 0; i < 32; i++) {
		if (mem[i].used) {
			if ((addr32 >= mem[i].start) && (addr32 < (mem[i].start + mem[i].size))) {
				return i;
			}
		}
	}
	return -1;
}*/

void wrcache_init() {
	wrcache_count = 0;
}

static FUNC_FORCE_INLINE int wrcache_entry_contains(const struct writecache_s* entry, uint32_t addr32, uint8_t* dst) {
	uint32_t offset;

	if ((addr32 < entry->addr) || (addr32 >= (entry->addr + (uint32_t)entry->size))) {
		return 0;
	}

	offset = addr32 - entry->addr;
	*dst = (uint8_t)(entry->value >> (offset << 3));
	return 1;
}

static FUNC_FORCE_INLINE int wrcache_has_overlap(uint32_t addr32, uint8_t size) {
	uint16_t i;
	uint64_t start = addr32;
	uint64_t end = start + (uint64_t)size;

	for (i = 0; i < wrcache_count; i++) {
		uint64_t entry_start = wrcache[i].addr;
		uint64_t entry_end = entry_start + (uint64_t)wrcache[i].size;

		if ((start < entry_end) && (end > entry_start)) {
			return 1;
		}
	}

	return 0;
}

static void wrcache_flush_entry(const struct writecache_s* entry) {
	struct mem_s* map;
	uint8_t i;

	if ((entry->size > 1) && memory_access_stays_on_page(entry->addr, entry->size)) {
		map = memory_lookup_map_range(entry->addr, entry->size, 1);
		if (map != NULL) {
			if ((entry->size == 2) && memory_desc_write_u16_direct(map, &map->desc, entry->addr, (uint16_t)entry->value)) {
				return;
			}

			if ((entry->size == 4) && memory_desc_write_u32_direct(map, &map->desc, entry->addr, entry->value)) {
				return;
			}
		}
	}

	for (i = 0; i < entry->size; i++) {
		int map_index = memory_lookup_map(entry->addr + (uint32_t)i, 1);
		if (map_index == 0xFF) {
			continue;
		}

		memory_desc_write_u8(&mem[map_index], &mem[map_index].desc,
			entry->addr + (uint32_t)i,
			(uint8_t)(entry->value >> (i << 3)));
	}
}

void wrcache_flush() {
	uint16_t i;
	for (i = 0; i < wrcache_count; i++) {
		wrcache_flush_entry(&wrcache[i]);
	}
	wrcache_count = 0;
}

static FUNC_FORCE_INLINE void wrcache_write(uint32_t addr32, uint32_t value, uint8_t size) {
	wrcache[wrcache_count].addr = addr32;
	wrcache[wrcache_count].value = value;
	wrcache[wrcache_count].size = size;
	wrcache_count++;
	if (wrcache_count == 512) {
		printf("FATAL: wrcache_count == 512\n");
		exit(0);
	}
}

static FUNC_FORCE_INLINE int wrcache_read(uint32_t addr32, uint8_t* dst) {
	uint16_t i;
	for (i = 0; i < wrcache_count; i++) {
		if (wrcache_entry_contains(&wrcache[i], addr32, dst)) {
			return 1;
		}
	}
	return 0;
}

static FUNC_FORCE_INLINE uint32_t cpu_apply_a20(CPU_t* cpu, uint32_t addr32) {
	if (!cpu->a20_gate) {
		addr32 &= 0xFFFFF;
	}

	return addr32;
}

static FUNC_FORCE_INLINE int cpu_linear_access_can_direct(CPU_t* cpu, uint32_t addr32, uint8_t size, int is_write, uint32_t* phys_addr, struct mem_s** map) {
	uint32_t phys;

	if ((size <= 1) || (!cpu->a20_gate && (((addr32 & 0xFFFFFu) + (uint32_t)size) > 0x100000u))) {
		return 0;
	}

	phys = cpu_apply_a20(cpu, addr32);
	if (!memory_access_stays_on_page(phys, size)) {
		return 0;
	}

	*map = memory_lookup_map_range(phys, size, is_write);
	if (*map == NULL) {
		return 0;
	}

	*phys_addr = phys;
	return 1;
}

static FUNC_FORCE_INLINE uint8_t cpu_read_phys_u8(CPU_t* cpu, uint32_t addr32) {
	int map;
	uint8_t cacheval;

	addr32 = cpu_apply_a20(cpu, addr32);

	if (wrcache_read(addr32, &cacheval)) {
		return cacheval;
	}

	map = memory_lookup_map(addr32, 0);
	if (map == 0xFF) {
		return 0xFF;
	}

	return memory_desc_read_u8(&mem[map], &mem[map].desc, addr32);
}

static FUNC_FORCE_INLINE uint8_t cpu_read_linear(CPU_t* cpu, uint32_t addr32) {
	return cpu_read_phys_u8(cpu, addr32);
}

static FUNC_FORCE_INLINE uint32_t cpu_read_linear_fallback(CPU_t* cpu, uint32_t addr32, uint8_t size) {
	uint32_t value = 0;
	uint8_t i;

	for (i = 0; i < size; i++) {
		value |= (uint32_t)cpu_read_linear(cpu, addr32 + (uint32_t)i) << (i << 3);
	}

	return value;
}

static FUNC_FORCE_INLINE uint32_t cpu_read_linear_sized(CPU_t* cpu, uint32_t addr32, uint8_t size) {
	struct mem_s* map;
	uint32_t phys;
	uint16_t value16;
	uint32_t value32;

	if (size == 1) {
		return cpu_read_linear(cpu, addr32);
	}

	if (!cpu_linear_access_can_direct(cpu, addr32, size, 0, &phys, &map) || wrcache_has_overlap(phys, size)) {
		return cpu_read_linear_fallback(cpu, addr32, size);
	}

	if ((size == 2) && memory_desc_read_u16_direct(map, &map->desc, phys, &value16)) {
		return value16;
	}

	if ((size == 4) && memory_desc_read_u32_direct(map, &map->desc, phys, &value32)) {
		return value32;
	}

	return cpu_read_linear_fallback(cpu, addr32, size);
}

int memory_paging_enabled(CPU_t* cpu) {
	return cpu->protected_mode && ((cpu->cr[0] & 0x80000000) != 0);
}

static FUNC_FORCE_INLINE int mem_access_is_write(MEM_ACCESS_t access) {
	return (access & 1) != 0;
}

static FUNC_FORCE_INLINE int mem_access_is_user(MEM_ACCESS_t access) {
	return access <= MEM_ACCESS_USER_WRITE;
}

static FUNC_FORCE_INLINE MEM_ACCESS_t cpu_default_access(CPU_t* cpu, int iswrite) {
	if (cpu->startcpl == 3) {
		return iswrite ? MEM_ACCESS_USER_WRITE : MEM_ACCESS_USER_READ;
	}

	return iswrite ? MEM_ACCESS_SUPERVISOR_WRITE : MEM_ACCESS_SUPERVISOR_READ;
}

static FUNC_FORCE_INLINE void cpu_write_phys_u8_direct(CPU_t* cpu, uint32_t addr32, uint8_t value) {
	struct mem_s* map;

	addr32 = cpu_apply_a20(cpu, addr32);

	map = memory_lookup_map_range(addr32, 1, 1);
	if (map == NULL) {
		return;
	}

	memory_desc_write_u8(map, &map->desc, addr32, value);
}

static FUNC_FORCE_INLINE uint32_t cpu_read_phys_u32(CPU_t* cpu, uint32_t addr32) {
	return cpu_read_linear_sized(cpu, addr32, 4);
}

static FUNC_FORCE_INLINE void cpu_or_phys_u8_direct(CPU_t* cpu, uint32_t addr32, uint8_t mask) {
	uint8_t value = cpu_read_phys_u8(cpu, addr32);

	if ((value & mask) == mask) {
		return;
	}

	cpu_write_phys_u8_direct(cpu, addr32, value | mask);
}

typedef struct {
	uint32_t phys_base;
	uint32_t pte_addr;
	uint8_t entry_flags;
} PAGE_WALK_RESULT_t;

static FUNC_FORCE_INLINE int page_access_is_allowed(CPU_t* cpu, MEM_ACCESS_t access, int effective_user, int effective_write) {
	if (mem_access_is_user(access) && !effective_user) {
		return 0;
	}

	if (mem_access_is_write(access) && !effective_write) {
		if (mem_access_is_user(access) || (cpu->cr[0] & 0x00010000)) {
			return 0;
		}
	}

	return 1;
}

static FUNC_FORCE_INLINE uint32_t cpu_raise_page_fault(CPU_t* cpu, uint32_t addr32, MEM_ACCESS_t access, int present) {
	uint32_t err = (present ? 1u : 0u) |
		(mem_access_is_write(access) ? 2u : 0u) |
		(mem_access_is_user(access) ? 4u : 0u);

	if (!cpu->doexception) {
		cpu->cr[2] = addr32;
	}

	exception(cpu, 14, err);
	return 0xFFFFFFFF;
}

void memory_tlb_flush(CPU_t* cpu) {
	memset(cpu->tlb, 0, sizeof(cpu->tlb));
}

void memory_tlb_invalidate_page(CPU_t* cpu, uint32_t linear_addr) {
	CPU_TLB_SET_t* set;
	uint32_t linear_page = linear_addr >> 12;
	uint8_t i;

	set = &cpu->tlb[linear_page & 0xFF];
	for (i = 0; i < 2; i++) {
		if ((set->way[i].flags & CPU_TLB_ENTRY_VALID) && (set->way[i].tag == linear_page)) {
			set->way[i].flags = 0;
		}
	}
}

// The TLB intentionally caches only successful 4 KiB translations. It does not
// snoop guest page-table writes or DMA, does not negative-cache faults, and does
// not model split I/D TLBs, PSE, PAE, or global-page behavior.
static FUNC_FORCE_INLINE int page_walk_translate(CPU_t* cpu, uint32_t addr32, MEM_ACCESS_t access, PAGE_WALK_RESULT_t* out) {
	uint32_t dir, table, dentry_addr, dentry, tentry_addr, tentry;
	int effective_user, effective_write;
	uint8_t entry_flags;

	dir = (addr32 >> 22) & 0x3FF;
	table = (addr32 >> 12) & 0x3FF;

	dentry_addr = (cpu->cr[3] & 0xFFFFF000) + (dir << 2);
	dentry = cpu_read_phys_u32(cpu, dentry_addr);
	if ((dentry & 1) == 0) {
		cpu_raise_page_fault(cpu, addr32, access, 0);
		return 0;
	}

	cpu_or_phys_u8_direct(cpu, dentry_addr, 0x20);

	tentry_addr = (dentry & 0xFFFFF000) + (table << 2);
	tentry = cpu_read_phys_u32(cpu, tentry_addr);
	if ((tentry & 1) == 0) {
		cpu_raise_page_fault(cpu, addr32, access, 0);
		return 0;
	}

	cpu_or_phys_u8_direct(cpu, tentry_addr, 0x20);

	effective_user = ((dentry & 0x4) != 0) && ((tentry & 0x4) != 0);
	effective_write = ((dentry & 0x2) != 0) && ((tentry & 0x2) != 0);
	if (!page_access_is_allowed(cpu, access, effective_user, effective_write)) {
		cpu_raise_page_fault(cpu, addr32, access, 1);
		return 0;
	}

	entry_flags = (effective_user ? CPU_TLB_ENTRY_USER_OK : 0) |
		(effective_write ? CPU_TLB_ENTRY_WRITE_OK : 0) |
		((tentry & 0x40) ? CPU_TLB_ENTRY_DIRTY : 0);

	if (mem_access_is_write(access) && ((entry_flags & CPU_TLB_ENTRY_DIRTY) == 0)) {
		cpu_or_phys_u8_direct(cpu, tentry_addr, 0x40);
		entry_flags |= CPU_TLB_ENTRY_DIRTY;
	}

	out->phys_base = tentry & 0xFFFFF000;
	out->pte_addr = tentry_addr;
	out->entry_flags = entry_flags;
	return 1;
}

static FUNC_FORCE_INLINE uint32_t translate_page(CPU_t* cpu, uint32_t addr32, MEM_ACCESS_t access) {
	CPU_TLB_SET_t* set;
	CPU_TLB_ENTRY_t* entry;
	PAGE_WALK_RESULT_t walk;
	uint32_t linear_page = addr32 >> 12;
	uint8_t index, other;

	set = &cpu->tlb[linear_page & 0xFF];
	index = set->mru & 1;
	entry = &set->way[index];
	if ((entry->flags & CPU_TLB_ENTRY_VALID) && (entry->tag == linear_page)) {
		goto tlb_hit;
	}

	other = index ^ 1;
	entry = &set->way[other];
	if ((entry->flags & CPU_TLB_ENTRY_VALID) && (entry->tag == linear_page)) {
		index = other;
		set->mru = index;
		goto tlb_hit;
	}

	if (!page_walk_translate(cpu, addr32, access, &walk)) {
		return 0xFFFFFFFF;
	}

	if ((set->way[0].flags & CPU_TLB_ENTRY_VALID) == 0) {
		index = 0;
	}
	else if ((set->way[1].flags & CPU_TLB_ENTRY_VALID) == 0) {
		index = 1;
	}
	else {
		index = (set->mru ^ 1) & 1;
	}

	entry = &set->way[index];
	entry->tag = linear_page;
	entry->phys_base = walk.phys_base;
	entry->pte_addr = walk.pte_addr;
	entry->flags = walk.entry_flags | CPU_TLB_ENTRY_VALID;
	set->mru = index;
	return entry->phys_base | (addr32 & 0xFFF);

tlb_hit:
	if (!page_access_is_allowed(cpu, access,
		(entry->flags & CPU_TLB_ENTRY_USER_OK) != 0,
		(entry->flags & CPU_TLB_ENTRY_WRITE_OK) != 0)) {
		return cpu_raise_page_fault(cpu, addr32, access, 1);
	}

	if (mem_access_is_write(access) && ((entry->flags & CPU_TLB_ENTRY_DIRTY) == 0)) {
		cpu_or_phys_u8_direct(cpu, entry->pte_addr, 0x40);
		entry->flags |= CPU_TLB_ENTRY_DIRTY;
	}

	set->mru = index;
	return entry->phys_base | (addr32 & 0xFFF);
}

static FUNC_FORCE_INLINE uint8_t cpu_read_access_u8(CPU_t* cpu, uint32_t addr32, MEM_ACCESS_t access) {
	if (cpu->doexception && cpu->nowrite) {
		return 0xFF;
	}

	if (memory_paging_enabled(cpu)) {
		addr32 = translate_page(cpu, addr32, access);
		if (addr32 == 0xFFFFFFFF) {
			return 0xFF;
		}
	}

	return cpu_read_linear(cpu, addr32);
}

static FUNC_FORCE_INLINE void cpu_write_access_u8(CPU_t* cpu, uint32_t addr32, uint8_t value, MEM_ACCESS_t access) {
	if (cpu->nowrite) {
		return;
	}

	if (memory_paging_enabled(cpu)) {
		addr32 = translate_page(cpu, addr32, access);
		if (addr32 == 0xFFFFFFFF) {
			return;
		}
	}

	cpu_write_linear(cpu, addr32, value);
}

static FUNC_FORCE_INLINE void cpu_write_linear(CPU_t* cpu, uint32_t addr32, uint8_t value) {
	if (cpu->nowrite) {
		return;
	}

	addr32 = cpu_apply_a20(cpu, addr32);
	wrcache_write(addr32, value, 1);
}

static FUNC_FORCE_INLINE void cpu_write_linear_fallback(CPU_t* cpu, uint32_t addr32, uint32_t value, uint8_t size) {
	uint8_t i;

	for (i = 0; i < size; i++) {
		cpu_write_linear(cpu, addr32 + (uint32_t)i, (uint8_t)(value >> (i << 3)));
	}
}

static FUNC_FORCE_INLINE void cpu_write_linear_sized(CPU_t* cpu, uint32_t addr32, uint32_t value, uint8_t size) {
	struct mem_s* map;
	uint32_t phys;

	if (size == 1) {
		cpu_write_linear(cpu, addr32, (uint8_t)value);
		return;
	}

	if (cpu->nowrite) {
		return;
	}

	if (cpu_linear_access_can_direct(cpu, addr32, size, 1, &phys, &map)) {
		wrcache_write(phys, value, size);
		return;
	}

	cpu_write_linear_fallback(cpu, addr32, value, size);
}

static FUNC_FORCE_INLINE uint32_t cpu_read_access_split(CPU_t* cpu, uint32_t addr32, MEM_ACCESS_t access, uint8_t size) {
	uint32_t value;
	uint8_t i;

	value = cpu_read_access_u8(cpu, addr32, access);
	if (cpu->doexception && cpu->nowrite) {
		return value;
	}

	for (i = 1; i < size; i++) {
		value |= (uint32_t)cpu_read_access_u8(cpu, addr32 + (uint32_t)i, access) << (i << 3);
		if (cpu->doexception && cpu->nowrite) {
			return value;
		}
	}

	return value;
}

static FUNC_FORCE_INLINE uint32_t cpu_read_access_sized(CPU_t* cpu, uint32_t addr32, MEM_ACCESS_t access, uint8_t size) {
	uint32_t phys;

	if (cpu->doexception && cpu->nowrite) {
		return 0xFF;
	}

	if (!memory_paging_enabled(cpu)) {
		return cpu_read_linear_sized(cpu, addr32, size);
	}

	if (!memory_access_stays_on_page(addr32, size)) {
		return cpu_read_access_split(cpu, addr32, access, size);
	}

	phys = translate_page(cpu, addr32, access);
	if (phys == 0xFFFFFFFF) {
		return 0xFF;
	}

	return cpu_read_linear_sized(cpu, phys, size);
}

static FUNC_FORCE_INLINE void cpu_write_access_split(CPU_t* cpu, uint32_t addr32, uint32_t value, MEM_ACCESS_t access, uint8_t size) {
	uint8_t i;

	for (i = 0; i < size; i++) {
		cpu_write_access_u8(cpu, addr32 + (uint32_t)i, (uint8_t)(value >> (i << 3)), access);
		if (cpu->nowrite) {
			return;
		}
	}
}

static FUNC_FORCE_INLINE void cpu_write_access_sized(CPU_t* cpu, uint32_t addr32, uint32_t value, MEM_ACCESS_t access, uint8_t size) {
	uint32_t phys;

	if (cpu->nowrite) {
		return;
	}

	if (!memory_paging_enabled(cpu)) {
		cpu_write_linear_sized(cpu, addr32, value, size);
		return;
	}

	if (!memory_access_stays_on_page(addr32, size)) {
		cpu_write_access_split(cpu, addr32, value, access, size);
		return;
	}

	phys = translate_page(cpu, addr32, access);
	if (phys == 0xFFFFFFFF) {
		return;
	}

	cpu_write_linear_sized(cpu, phys, value, size);
}

void cpu_write(CPU_t* cpu, uint32_t addr32, uint8_t value) {
	cpu_write_access_u8(cpu, addr32, value, cpu_default_access(cpu, 1));
}

uint8_t cpu_read(CPU_t* cpu, uint32_t addr32) {
	return cpu_read_access_u8(cpu, addr32, cpu_default_access(cpu, 0));
}

void cpu_write_sys(CPU_t* cpu, uint32_t addr32, uint8_t value) {
	cpu_write_access_u8(cpu, addr32, value, MEM_ACCESS_SUPERVISOR_WRITE);
}

uint8_t cpu_read_sys(CPU_t* cpu, uint32_t addr32) {
	return cpu_read_access_u8(cpu, addr32, MEM_ACCESS_SUPERVISOR_READ);
}

uint8_t memory_read_phys_u8(CPU_t* cpu, uint32_t addr32) {
	return cpu_read_phys_u8(cpu, addr32);
}

void memory_write_phys_u8(CPU_t* cpu, uint32_t addr32, uint8_t value) {
	cpu_write_phys_u8_direct(cpu, addr32, value);
}

void cpu_writew(CPU_t* cpu, uint32_t addr32, uint16_t value) {
	cpu_write_access_sized(cpu, addr32, value, cpu_default_access(cpu, 1), 2);
}

void cpu_writel(CPU_t* cpu, uint32_t addr32, uint32_t value) {
	cpu_write_access_sized(cpu, addr32, value, cpu_default_access(cpu, 1), 4);
}

void cpu_writew_sys(CPU_t* cpu, uint32_t addr32, uint16_t value) {
	cpu_write_access_sized(cpu, addr32, value, MEM_ACCESS_SUPERVISOR_WRITE, 2);
}

void cpu_writel_sys(CPU_t* cpu, uint32_t addr32, uint32_t value) {
	cpu_write_access_sized(cpu, addr32, value, MEM_ACCESS_SUPERVISOR_WRITE, 4);
}

void cpu_writew_linear(CPU_t* cpu, uint32_t addr32, uint16_t value) {
	cpu_write_linear_sized(cpu, addr32, value, 2);
}

void cpu_writel_linear(CPU_t* cpu, uint32_t addr32, uint32_t value) {
	cpu_write_linear_sized(cpu, addr32, value, 4);
}

uint16_t cpu_readw(CPU_t* cpu, uint32_t addr32) {
	return (uint16_t)cpu_read_access_sized(cpu, addr32, cpu_default_access(cpu, 0), 2);
}

uint32_t cpu_readl(CPU_t* cpu, uint32_t addr32) {
	return cpu_read_access_sized(cpu, addr32, cpu_default_access(cpu, 0), 4);
}

uint16_t cpu_readw_sys(CPU_t* cpu, uint32_t addr32) {
	return (uint16_t)cpu_read_access_sized(cpu, addr32, MEM_ACCESS_SUPERVISOR_READ, 2);
}

uint32_t cpu_readl_sys(CPU_t* cpu, uint32_t addr32) {
	return cpu_read_access_sized(cpu, addr32, MEM_ACCESS_SUPERVISOR_READ, 4);
}

uint16_t cpu_readw_linear(CPU_t* cpu, uint32_t addr32) {
	return (uint16_t)cpu_read_linear_sized(cpu, addr32, 2);
}

uint32_t cpu_readl_linear(CPU_t* cpu, uint32_t addr32) {
	return cpu_read_linear_sized(cpu, addr32, 4);
}

static struct mem_s* memory_alloc_map(uint32_t start, uint32_t len) {
	uint8_t i;

	for (i = 0; i < 64; i++) {
		if (mem[i].used == 0) {
			mem[i].start = start;
			mem[i].size = len;
			mem[i].used = 1;
			memory_desc_clear(&mem[i].desc);
			return &mem[i];
		}
	}

	debug_log(DEBUG_ERROR, "[MEMORY] Out of memory map structs!\n");
	exit(0);
}

static void memory_update_lookup(struct mem_s* map, uint8_t internal) {
	uint32_t page_start;
	uint32_t page_count;
	uint32_t j;
	uint8_t index;
	uint8_t* lookup;

	page_start = map->start >> 12;
	page_count = (map->size + 0xFFF) >> 12;
	index = (uint8_t)(map - mem);
	lookup = internal ? mem_map_lookup_internal : mem_map_lookup_external;
	for (j = page_start; j < (page_start + page_count); j++) {
		lookup[j] = index;
	}
}

static void memory_clear_lookup(struct mem_s* map, uint8_t internal) {
	uint32_t page_start;
	uint32_t page_count;
	uint32_t j;
	uint8_t index;
	uint8_t* lookup;

	page_start = map->start >> 12;
	page_count = (map->size + 0xFFF) >> 12;
	index = (uint8_t)(map - mem);
	lookup = internal ? mem_map_lookup_internal : mem_map_lookup_external;
	for (j = page_start; j < (page_start + page_count); j++) {
		if (lookup[j] == index) {
			lookup[j] = 0xFF;
		}
	}
}

static void memory_release_map(struct mem_s* map, uint8_t internal) {
	if ((map == NULL) || !map->used) {
		return;
	}

	memory_clear_lookup(map, internal);
	memory_desc_clear(&map->desc);
	map->start = 0;
	map->size = 0;
	map->used = 0;
}

static void memory_register_desc(uint32_t start, uint32_t len, const MEMORY_MAP_DESC_t* desc, uint8_t internal) {
	struct mem_s* map;

	map = memory_alloc_map(start, len);
	memory_desc_copy(&map->desc, desc);
	memory_update_lookup(map, internal);
}

void memory_mapRegister(uint32_t start, uint32_t len, uint8_t* readb, uint8_t* writeb) {
	MEMORY_MAP_DESC_t desc;

	memory_desc_clear(&desc);
	desc.read = readb;
	desc.write = writeb;
	memory_register_desc(start, len, &desc, 0);
}

void memory_mapRegisterInternal(uint32_t start, uint32_t len, uint8_t* readb, uint8_t* writeb) {
	MEMORY_MAP_DESC_t desc;

	memory_desc_clear(&desc);
	desc.read = readb;
	desc.write = writeb;
	memory_register_desc(start, len, &desc, 1);
}

void memory_mapCallbackRegister(uint32_t start, uint32_t count,
	uint8_t(*readb)(void*, uint32_t), uint16_t(*readw)(void*, uint32_t), uint32_t(*readl)(void*, uint32_t),
	void (*writeb)(void*, uint32_t, uint8_t), void (*writew)(void*, uint32_t, uint16_t), void (*writel)(void*, uint32_t, uint32_t),
	void* udata) {
	MEMORY_MAP_DESC_t desc;

	memory_desc_clear(&desc);
	desc.read = NULL;
	desc.write = NULL;
	desc.readcb = readb;
	desc.readwcb = readw;
	desc.readlcb = readl;
	desc.writecb = writeb;
	desc.writewcb = writew;
	desc.writelcb = writel;
	desc.udata = udata;
	memory_register_desc(start, count, &desc, 0);
}

void memory_mapCallbackRegisterInternal(uint32_t start, uint32_t count,
	uint8_t(*readb)(void*, uint32_t), uint16_t(*readw)(void*, uint32_t), uint32_t(*readl)(void*, uint32_t),
	void (*writeb)(void*, uint32_t, uint8_t), void (*writew)(void*, uint32_t, uint16_t), void (*writel)(void*, uint32_t, uint32_t),
	void* udata) {
	MEMORY_MAP_DESC_t desc;

	memory_desc_clear(&desc);
	desc.read = NULL;
	desc.write = NULL;
	desc.readcb = readb;
	desc.readwcb = readw;
	desc.readlcb = readl;
	desc.writecb = writeb;
	desc.writewcb = writew;
	desc.writelcb = writel;
	desc.udata = udata;
	memory_register_desc(start, count, &desc, 1);
}

void memory_mapping_add(MEMORY_MAPPING_t* mapping, uint32_t start, uint32_t len,
	uint8_t(*readb)(void*, uint32_t), uint16_t(*readw)(void*, uint32_t), uint32_t(*readl)(void*, uint32_t),
	void (*writeb)(void*, uint32_t, uint8_t), void (*writew)(void*, uint32_t, uint16_t), void (*writel)(void*, uint32_t, uint32_t),
	void* udata, uint8_t internal) {
	if (mapping == NULL) {
		return;
	}

	memset(mapping, 0, sizeof(*mapping));
	mapping->start = start;
	mapping->len = len;
	mapping->internal = (uint8_t)(internal ? 1 : 0);
	mapping->map_index = -1;
	mapping->desc.read = NULL;
	mapping->desc.write = NULL;
	mapping->desc.readcb = readb;
	mapping->desc.readwcb = readw;
	mapping->desc.readlcb = readl;
	mapping->desc.writecb = writeb;
	mapping->desc.writewcb = writew;
	mapping->desc.writelcb = writel;
	mapping->desc.udata = udata;
}

void memory_mapping_enable(MEMORY_MAPPING_t* mapping) {
	struct mem_s* map;

	if ((mapping == NULL) || mapping->enabled || (mapping->len == 0)) {
		return;
	}

	map = memory_alloc_map(mapping->start, mapping->len);
	memory_desc_copy(&map->desc, &mapping->desc);
	memory_update_lookup(map, mapping->internal);
	mapping->map_index = (int)(map - mem);
	mapping->enabled = 1;
}

void memory_mapping_disable(MEMORY_MAPPING_t* mapping) {
	if ((mapping == NULL) || !mapping->enabled) {
		return;
	}

	if ((mapping->map_index >= 0) && (mapping->map_index < 64)) {
		memory_release_map(&mem[mapping->map_index], mapping->internal);
	}

	mapping->map_index = -1;
	mapping->enabled = 0;
}

void memory_mapping_set_addr(MEMORY_MAPPING_t* mapping, uint32_t start, uint32_t len) {
	uint8_t was_enabled;

	if (mapping == NULL) {
		return;
	}

	was_enabled = mapping->enabled;
	if (was_enabled) {
		memory_mapping_disable(mapping);
	}

	mapping->start = start;
	mapping->len = len;

	if (was_enabled && (len != 0)) {
		memory_mapping_enable(mapping);
	}
}

void memory_mapSetDecodeState(uint32_t start, uint32_t len, MEMORY_DECODE_t state) {
	uint32_t page_start;
	uint32_t page_count;
	uint32_t i;
	uint8_t changed;

	page_start = start >> 12;
	page_count = (len + 0xFFF) >> 12;
	changed = 0;
	for (i = page_start; i < (page_start + page_count); i++) {
		uint8_t new_state;

		new_state = (uint8_t)(state & 0x03);
		if (mem_decode_lookup[i] != new_state) {
			mem_decode_lookup[i] = new_state;
			changed = 1;
		}
	}

	if (changed) {
		memory_tlb_flush(&machine.CPU);
	}
}

int memory_init() {
	uint32_t i;

	for (i = 0; i < 64; i++) {
		memory_desc_clear(&mem[i].desc);
		mem[i].used = 0;
	}

	memset(mem_map_lookup_external, 0xFF, sizeof(mem_map_lookup_external));
	memset(mem_map_lookup_internal, 0xFF, sizeof(mem_map_lookup_internal));
	memset(mem_decode_lookup, MEMORY_DECODE_EXTERNAL_EXTERNAL, sizeof(mem_decode_lookup));

	return 0;
}
