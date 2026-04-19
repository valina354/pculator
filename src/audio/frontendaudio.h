#ifndef _FRONTENDAUDIO_H_
#define _FRONTENDAUDIO_H_

#include "../machine.h"

int frontendaudio_init(MACHINE_t* machine);
void frontendaudio_shutdown(void);
void frontendaudio_generateSample(void* dummy);
void frontendaudio_updateSampleTiming(void);
void frontendaudio_pull(int16_t* stream, int samples, void* udata);

#endif
