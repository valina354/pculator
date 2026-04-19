#include "../config.h"
#include <stdint.h>
#include <string.h>
#include "frontendaudio.h"
#include "pcspeaker.h"
#include "opl2.h"
#include "blaster.h"
#include "../frontends/frontend.h"
#include "../timing.h"

#define FRONTENDAUDIO_TIMING_FAST   1
#define FRONTENDAUDIO_TIMING_NORMAL 2

#define PCSPK_GAIN      64
#define OPL_GAIN        96
#define SB_GAIN         128
#define MASTER_GAIN     192

static int16_t frontendaudio_buffer[SAMPLE_BUFFER];
static int frontendaudio_bufferpos = 0;
static double frontendaudio_rateFast = 0.0;
static uint32_t frontendaudio_timer = TIMING_ERROR;
static volatile uint8_t frontendaudio_updateTiming = 0;
static MACHINE_t* frontendaudio_machine = NULL;
static uint8_t frontendaudio_enabled = 0;

static void frontendaudio_bufferSample(int16_t left, int16_t right)
{
    if ((frontendaudio_enabled == 0) || (frontendaudio_bufferpos >= SAMPLE_BUFFER - 2)) {
        return;
    }

    frontendaudio_buffer[frontendaudio_bufferpos++] = left;
    frontendaudio_buffer[frontendaudio_bufferpos++] = right;

    if (frontendaudio_bufferpos < (int)((double)SAMPLE_BUFFER * 0.5)) {
        frontendaudio_updateTiming = FRONTENDAUDIO_TIMING_FAST;
    }
    else if (frontendaudio_bufferpos >= (int)((double)SAMPLE_BUFFER * 0.75)) {
        frontendaudio_updateTiming = FRONTENDAUDIO_TIMING_NORMAL;
    }

    if (frontendaudio_bufferpos == SAMPLE_BUFFER) {
        timing_timerDisable(frontendaudio_timer);
    }
}

int frontendaudio_init(MACHINE_t* machine)
{
    FRONTEND_AUDIO_FORMAT_t format;

    if (machine == NULL) {
        return -1;
    }

    frontendaudio_machine = machine;
    frontendaudio_bufferpos = 0;
    frontendaudio_updateTiming = 0;
    frontendaudio_rateFast = (double)SAMPLE_RATE * 1.01;
    frontendaudio_timer = timing_addTimer(frontendaudio_generateSample, NULL, SAMPLE_RATE, TIMING_ENABLED);

    format.sample_rate = SAMPLE_RATE;
    format.channels = 2;
    format.buffer_samples = (uint16_t)(SAMPLE_BUFFER >> 3);

    if (frontend_audio_init_sink(&format, frontendaudio_pull, NULL) != 0) {
        frontendaudio_enabled = 0;
        timing_timerDisable(frontendaudio_timer);
        return -1;
    }

    frontendaudio_enabled = 1;
    frontend_audio_set_paused(1);
    return 0;
}

void frontendaudio_shutdown(void)
{
    frontendaudio_enabled = 0;
    frontendaudio_bufferpos = 0;
    frontendaudio_updateTiming = 0;
    frontend_audio_shutdown_sink();
    if (frontendaudio_timer != TIMING_ERROR) {
        timing_timerDisable(frontendaudio_timer);
    }
}

void frontendaudio_updateSampleTiming(void)
{
    if (frontendaudio_enabled == 0) {
        return;
    }

    if (frontendaudio_updateTiming == FRONTENDAUDIO_TIMING_FAST) {
        timing_updateIntervalFreq(frontendaudio_timer, frontendaudio_rateFast);
    }
    else if (frontendaudio_updateTiming == FRONTENDAUDIO_TIMING_NORMAL) {
        timing_updateIntervalFreq(frontendaudio_timer, SAMPLE_RATE);
        frontend_audio_set_paused(0);
    }
    frontendaudio_updateTiming = 0;
}

void frontendaudio_pull(int16_t* stream, int samples, void* udata)
{
    int i;
    int bytes;

    (void)udata;

    if ((stream == NULL) || (samples <= 0)) {
        return;
    }

    bytes = samples * (int)sizeof(int16_t);
    memset(stream, 0, (size_t)bytes);

    if (frontendaudio_enabled == 0) {
        return;
    }

    if (frontendaudio_bufferpos < (int)((double)SAMPLE_BUFFER * 0.75)) {
        timing_timerEnable(frontendaudio_timer);
    }

    if (frontendaudio_bufferpos < samples) {
        frontend_audio_set_paused(1);
        return;
    }

    for (i = 0; i < samples; i++) {
        stream[i] = frontendaudio_buffer[i];
    }
    for (; i < frontendaudio_bufferpos; i++) {
        frontendaudio_buffer[i - samples] = frontendaudio_buffer[i];
    }
    frontendaudio_bufferpos -= samples;
}

void frontendaudio_generateSample(void* dummy)
{
    int16_t blasterLeft;
    int16_t blasterRight;
    int16_t outLeft;
    int16_t outRight;

    (void)dummy;

    if ((frontendaudio_enabled == 0) || (frontendaudio_machine == NULL)) {
        return;
    }

    outLeft = outRight = ((int32_t)pcspeaker_getSample(&frontendaudio_machine->pcspeaker) * PCSPK_GAIN) >> 8;
    if (frontendaudio_machine->mixOPL) {
        int16_t oplSample[2];
        OPL3_GenerateStream(&frontendaudio_machine->OPL3, oplSample, 1);
        outLeft += ((int32_t)oplSample[0] * OPL_GAIN) >> 8;
        outRight += ((int32_t)oplSample[1] * OPL_GAIN) >> 8;
    }
    if (frontendaudio_machine->mixBlaster) {
        blaster_getSample(&frontendaudio_machine->blaster, &blasterLeft, &blasterRight);
        outLeft += ((int32_t)blasterLeft * SB_GAIN) >> 8;
        outRight += ((int32_t)blasterRight * SB_GAIN) >> 8;
    }

    outLeft = (outLeft * MASTER_GAIN) >> 8;
    outRight = (outRight * MASTER_GAIN) >> 8;

    frontendaudio_bufferSample((int16_t)outLeft, (int16_t)outRight);
}
