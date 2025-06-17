#ifndef _TEXT_H_
#define _TEXT_H_

#include <stdint.h>
#include "../../cpu/cpu.h"

int text_init();
void text_writeport(void* dummy, uint16_t port, uint8_t value);
uint8_t text_readport(void* dummy, uint16_t port);
void text_writememory(void* dummy, uint32_t addr, uint8_t value);
uint8_t text_readmemory(void* dummy, uint32_t addr);
void text_scanlineCallback(void* dummy);

#endif
#pragma once
