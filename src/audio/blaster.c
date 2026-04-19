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

/*
	Emulation of the Sound Blaster 16 DSP and mixer
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../config.h"
#include "../debuglog.h"
#include "../ports.h"
#include "../timing.h"
#include "../chipset/dma.h"
#include "../chipset/i8259.h"
#include "blaster.h"

const int16_t cmd_E2_table[9] = { 0x01, -0x02, -0x04,  0x08, -0x10,  0x20,  0x40, -0x80, -106 };
static const char blaster_sb16Copyright[] = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992.";

void blaster_putreadbuf(BLASTER_t* blaster, uint8_t value) {
	if (blaster->readlen >= sizeof(blaster->readbuf)) return;

	blaster->readbuf[blaster->readlen++] = value;
}

uint8_t blaster_getreadbuf(BLASTER_t* blaster) {
	uint8_t ret = 0xFF;

	if (blaster->readlen == 0) {
		return ret;
	}

	ret = blaster->readbuf[0];
	blaster->readlen--;
	memmove(blaster->readbuf, blaster->readbuf + 1, sizeof(blaster->readbuf) - 1);

	return ret;
}

static void blaster_syncCompatibilityMixer(BLASTER_t* blaster) {
	blaster->mixerregs[0x02] = (blaster->mixerregs[0x30] >> 4) & 0x0F;
	blaster->mixerregs[0x04] = (blaster->mixerregs[0x32] & 0xF0) | ((blaster->mixerregs[0x33] >> 4) & 0x0F);
	blaster->mixerregs[0x06] = (blaster->mixerregs[0x34] >> 4) & 0x0F;
	blaster->mixerregs[0x08] = (blaster->mixerregs[0x36] >> 4) & 0x0F;
	blaster->mixerregs[0x0A] = (blaster->mixerregs[0x3A] >> 5) & 0x07;
	blaster->mixerregs[0x0E] = 0x02;
	blaster->mixerregs[0x22] = (blaster->mixerregs[0x30] & 0xF0) | ((blaster->mixerregs[0x31] >> 4) & 0x0F);
	blaster->mixerregs[0x26] = (blaster->mixerregs[0x34] & 0xF0) | ((blaster->mixerregs[0x35] >> 4) & 0x0F);
	blaster->mixerregs[0x28] = (blaster->mixerregs[0x36] & 0xF0) | ((blaster->mixerregs[0x37] >> 4) & 0x0F);
	blaster->mixerregs[0x2E] = (blaster->mixerregs[0x38] & 0xF0) | ((blaster->mixerregs[0x39] >> 4) & 0x0F);
}

static uint8_t blaster_getMixerIRQSetup(BLASTER_t* blaster) {
	switch (blaster->irq) {
	case 2:
		return 0x01;
	case 7:
		return 0x04;
	case 10:
		return 0x08;
	default:
		blaster->irq = 5;
		return 0x02;
	}
}

static uint8_t blaster_getMixerDMASetup(BLASTER_t* blaster) {
	uint8_t ret = 0;

	switch (blaster->dma8chan) {
	case 0:
		ret |= 0x01;
		break;
	case 3:
		ret |= 0x08;
		break;
	default:
		blaster->dma8chan = 1;
		ret |= 0x02;
		break;
	}

	switch (blaster->dma16chan) {
	case 6:
		ret |= 0x40;
		break;
	case 7:
		ret |= 0x80;
		break;
	default:
		blaster->dma16chan = 5;
		ret |= 0x20;
		break;
	}

	return ret;
}

static uint8_t blaster_getMixerIRQStatus(BLASTER_t* blaster) {
	uint8_t ret = 0x40;

	if (blaster->irqpending8 != 0) {
		ret |= 0x01;
	}
	if (blaster->irqpending16 != 0) {
		ret |= 0x02;
	}

	return ret;
}

static void blaster_raiseIrq(BLASTER_t* blaster, uint8_t irq16) {
	if (irq16 != 0) {
		blaster->irqpending16 = 1;
	} else {
		blaster->irqpending8 = 1;
	}
	i8259_doirq(blaster->i8259, blaster->irq);
}

static void blaster_clearIrq(BLASTER_t* blaster, uint8_t irq16) {
	if (irq16 != 0) {
		blaster->irqpending16 = 0;
	} else {
		blaster->irqpending8 = 0;
	}

	if ((blaster->irqpending8 == 0) && (blaster->irqpending16 == 0)) {
		i8259_clearirq(blaster->i8259, blaster->irq);
	}
}

static void blaster_resetMixer(BLASTER_t* blaster) {
	memset(blaster->mixerregs, 0, sizeof(blaster->mixerregs));

	blaster->mixerregs[0x30] = 0xF8;
	blaster->mixerregs[0x31] = 0xF8;
	blaster->mixerregs[0x32] = 0xF8;
	blaster->mixerregs[0x33] = 0xF8;
	blaster->mixerregs[0x34] = 0xF8;
	blaster->mixerregs[0x35] = 0xF8;
	blaster->mixerregs[0x36] = 0x00;
	blaster->mixerregs[0x37] = 0x00;
	blaster->mixerregs[0x38] = 0x00;
	blaster->mixerregs[0x39] = 0x00;
	blaster->mixerregs[0x3A] = 0x00;
	blaster->mixerregs[0x3B] = 0x80;
	blaster->mixerregs[0x3C] = 0x1F;
	blaster->mixerregs[0x3D] = 0x15;
	blaster->mixerregs[0x3E] = 0x0B;
	blaster->mixerregs[0x3F] = 0x00;
	blaster->mixerregs[0x40] = 0x00;
	blaster->mixerregs[0x41] = 0x00;
	blaster->mixerregs[0x42] = 0x00;
	blaster->mixerregs[0x43] = 0x00;
	blaster->mixerregs[0x44] = 0x80;
	blaster->mixerregs[0x45] = 0x80;
	blaster->mixerregs[0x46] = 0x80;
	blaster->mixerregs[0x47] = 0x80;
	blaster->mixerregs[0x48] = 0x00;
	blaster->mixerregs[0x49] = 0x80;
	blaster->mixerregs[0x4A] = 0x80;
	blaster->mixerregs[0x83] = 0xFF;
	blaster->mixerregs[0x84] = 0x02;
	blaster->mixerregs[0x8C] = 0x00;
	blaster->mixerregs[0x8E] = 0x00;
	blaster->mixerregs[0x90] = 0x00;
	blaster->mixerregs[0xFD] = 0x10;
	blaster->mixerregs[0xFE] = 0x06;
	blaster->mixerregs[0xFF] = 0x05;
	blaster_syncCompatibilityMixer(blaster);
	blaster->mixerregs[0x80] = blaster_getMixerIRQSetup(blaster);
	blaster->mixerregs[0x81] = blaster_getMixerDMASetup(blaster);
	blaster->mixerregs[0x82] = blaster_getMixerIRQStatus(blaster);
}

void blaster_reset(BLASTER_t* blaster) {
	blaster->dspenable = 0;
	blaster->sample[0] = 0;
	blaster->sample[1] = 0;
	blaster->readlen = 0;
	blaster->dmaLastTransferred = 0;
	blaster->dmaBlockDone = 0;
	blaster->lastcmd = 0;
	blaster->writehilo = 0;
	blaster->dmalen = 0;
	blaster->blocksize = 1;
	blaster->blockbytes = 1;
	blaster->dmacount = 0;
	blaster->autoinit = 0;
	blaster->autoinitstop = 0;
	blaster->dorecord = 0;
	blaster->activedma = 0;
	blaster->dma16 = 0;
	blaster->dmastereo = 0;
	blaster->dmasigned = 0;
	blaster->pausedsp = 0;
	blaster->pauseduration = 0;
	blaster->pausecount = 0;
	blaster->cmdparampos = 0;
	blaster->cmdparamneeded = 0;
	blaster->irqpending8 = 0;
	blaster->irqpending16 = 0;
	blaster->autolen8 = 1;
	blaster->autolen16 = 1;
	blaster->readready = 0;
	blaster->writebuf = 0;
	i8259_clearirq(blaster->i8259, blaster->irq);
	blaster_putreadbuf(blaster, 0xAA);
	timing_timerDisable(blaster->timer);
}

void blaster_writecmd(BLASTER_t* blaster, uint8_t value) {
	if (blaster->cmdparamneeded != 0) {
		blaster->cmdparams[blaster->cmdparampos++] = value;
		if (blaster->cmdparampos < blaster->cmdparamneeded) {
			return;
		}

		if (((blaster->lastcmd >= 0xB0) && (blaster->lastcmd <= 0xBF)) ||
			((blaster->lastcmd >= 0xC0) && (blaster->lastcmd <= 0xCF))) {
			blaster->blocksize = (uint32_t)blaster->cmdparams[1] | ((uint32_t)blaster->cmdparams[2] << 8);
			blaster->blocksize++;
			blaster->dma16 = ((blaster->lastcmd & 0xF0) == 0xB0) ? 1 : 0;
			blaster->dorecord = ((blaster->lastcmd & 0x08) != 0) ? 1 : 0;
			blaster->autoinit = ((blaster->lastcmd & 0x04) != 0) ? 1 : 0;
			blaster->autoinitstop = 0;
			blaster->dmastereo = ((blaster->cmdparams[0] & 0x20) != 0) ? 1 : 0;
			blaster->dmasigned = ((blaster->cmdparams[0] & 0x10) != 0) ? 1 : 0;
			blaster->blockbytes = blaster->blocksize;
			if (blaster->dma16 != 0) {
				blaster->blockbytes <<= 1;
				blaster->autolen16 = blaster->blocksize;
			} else {
				blaster->autolen8 = blaster->blocksize;
			}
			blaster->dmalen = blaster->blocksize;
			blaster->dmacount = 0;
			blaster->activedma = 1;
			blaster->pausedsp = 0;
			blaster->samplerate = blaster->dorecord ? blaster->inputrate : blaster->outputrate;
			if (blaster->samplerate < 1.0) {
				blaster->samplerate = 1.0;
			}
			timing_updateIntervalFreq(blaster->timer, blaster->samplerate);
			timing_timerEnable(blaster->timer);
			blaster->lastcmd = 0;
			blaster->cmdparampos = 0;
			blaster->cmdparamneeded = 0;
			return;
		}

		switch (blaster->lastcmd) {
		case 0x09:
		case 0x10:
			blaster->sample[0] = ((int16_t)((int)value - 128)) * 256;
			blaster->sample[1] = blaster->sample[0];
			break;
		case 0x14:
		case 0x24:
			blaster->blocksize = (uint32_t)blaster->cmdparams[0] | ((uint32_t)blaster->cmdparams[1] << 8);
			blaster->blocksize++;
			blaster->blockbytes = blaster->blocksize;
			blaster->dmalen = blaster->blocksize;
			blaster->dmacount = 0;
			blaster->autoinit = 0;
			blaster->autoinitstop = 0;
			blaster->dorecord = (blaster->lastcmd == 0x24) ? 1 : 0;
			blaster->activedma = 1;
			blaster->dma16 = 0;
			blaster->dmastereo = 0;
			blaster->dmasigned = 0;
			blaster->pausedsp = 0;
			blaster->samplerate = blaster->dorecord ? blaster->inputrate : blaster->outputrate;
			if (blaster->samplerate < 1.0) {
				blaster->samplerate = 1.0;
			}
			timing_updateIntervalFreq(blaster->timer, blaster->samplerate);
			timing_timerEnable(blaster->timer);
#ifdef DEBUG_BLASTER
			debug_log(DEBUG_DETAIL, "[BLASTER] Begin DMA transfer mode with %lu byte blocks\r\n", blaster->blockbytes);
#endif
			break;
		case 0x40:
			blaster->timeconst = value;
			blaster->samplerate = 1000000.0 / (256.0 - (double)value);
			blaster->outputrate = blaster->samplerate;
			blaster->inputrate = blaster->samplerate;
			if (blaster->activedma != 0) {
				timing_updateIntervalFreq(blaster->timer, blaster->samplerate);
			}
#ifdef DEBUG_BLASTER
			debug_log(DEBUG_DETAIL, "[BLASTER] Set time constant: %u (Sample rate: %f Hz)\r\n", value, blaster->samplerate);
#endif
			break;
		case 0x41:
		case 0x42:
		{
			uint16_t rate = ((uint16_t)blaster->cmdparams[0] << 8) | blaster->cmdparams[1];

			if (rate == 0) {
				rate = 1;
			}
			if (blaster->lastcmd == 0x41) {
				blaster->outputrate = (double)rate;
				if ((blaster->activedma != 0) && (blaster->dorecord == 0)) {
					blaster->samplerate = blaster->outputrate;
					timing_updateIntervalFreq(blaster->timer, blaster->samplerate);
				}
			} else {
				blaster->inputrate = (double)rate;
				if ((blaster->activedma != 0) && (blaster->dorecord != 0)) {
					blaster->samplerate = blaster->inputrate;
					timing_updateIntervalFreq(blaster->timer, blaster->samplerate);
				}
			}
			break;
		}
		case 0x48:
			blaster->autolen8 = (uint32_t)blaster->cmdparams[0] | ((uint32_t)blaster->cmdparams[1] << 8);
			blaster->autolen8++;
			blaster->blocksize = blaster->autolen8;
			blaster->blockbytes = blaster->autolen8;
			break;
		case 0x80:
			blaster->pauseduration = (uint32_t)blaster->cmdparams[0] | ((uint32_t)blaster->cmdparams[1] << 8);
			blaster->pauseduration++;
			blaster->pausecount = 0;
			blaster->samplerate = blaster->outputrate;
			if (blaster->samplerate < 1.0) {
				blaster->samplerate = 1.0;
			}
			timing_updateIntervalFreq(blaster->timer, blaster->samplerate);
			timing_timerEnable(blaster->timer);
			break;
		case 0xE0:
			blaster_putreadbuf(blaster, (uint8_t)(~value));
			break;
		case 0xE2:
		{
			int16_t val = 0xAA;
			int i;

			for (i = 0; i < 8; i++) {
				if ((value >> i) & 0x01) {
					val += cmd_E2_table[i];
				}
			}
			val += cmd_E2_table[8];
			dma_channel_write(blaster->dma, blaster->dma8chan, (uint8_t)val);
			break;
		}
		case 0xE4:
			blaster->testreg = value;
			break;
		}

		blaster->lastcmd = 0;
		blaster->cmdparampos = 0;
		blaster->cmdparamneeded = 0;
		return;
	}

	if (((value >= 0xB0) && (value <= 0xBF)) || ((value >= 0xC0) && (value <= 0xCF))) {
		blaster->lastcmd = value;
		blaster->cmdparamneeded = 3;
		blaster->cmdparampos = 0;
		return;
	}

	switch (value) {
	case 0x09:
	case 0x10:
		blaster->lastcmd = value;
		blaster->cmdparamneeded = 1;
		blaster->cmdparampos = 0;
		return;
	case 0x14:
	case 0x24:
		blaster->lastcmd = value;
		blaster->cmdparamneeded = 2;
		blaster->cmdparampos = 0;
		return;
	case 0x1C:
	case 0x2C:
		blaster->blocksize = blaster->autolen8;
		if (blaster->blocksize == 0) {
			blaster->blocksize = 1;
		}
		blaster->blockbytes = blaster->blocksize;
		blaster->dmalen = blaster->blocksize;
		blaster->dmacount = 0;
		blaster->autoinit = 1;
		blaster->autoinitstop = 0;
		blaster->dorecord = (value == 0x2C) ? 1 : 0;
		blaster->activedma = 1;
		blaster->dma16 = 0;
		blaster->dmastereo = 0;
		blaster->dmasigned = 0;
		blaster->pausedsp = 0;
		blaster->samplerate = blaster->dorecord ? blaster->inputrate : blaster->outputrate;
		if (blaster->samplerate < 1.0) {
			blaster->samplerate = 1.0;
		}
		timing_updateIntervalFreq(blaster->timer, blaster->samplerate);
		timing_timerEnable(blaster->timer);
#ifdef DEBUG_BLASTER
		debug_log(DEBUG_DETAIL, "[BLASTER] Begin auto-init DMA transfer mode with %lu byte blocks\r\n", blaster->blockbytes);
#endif
		return;
	case 0x20:
		blaster_putreadbuf(blaster, 128);
		return;
	case 0x40:
	case 0xE0:
	case 0xE2:
	case 0xE4:
		blaster->lastcmd = value;
		blaster->cmdparamneeded = 1;
		blaster->cmdparampos = 0;
		return;
	case 0x41:
	case 0x42:
	case 0x48:
	case 0x80:
		blaster->lastcmd = value;
		blaster->cmdparamneeded = 2;
		blaster->cmdparampos = 0;
		return;
	case 0x45:
		if (blaster->dma16 == 0) {
			blaster->autoinit = 1;
			blaster->autoinitstop = 0;
			blaster->pausedsp = 0;
			if (blaster->activedma == 0) {
				blaster->activedma = 1;
				blaster->dmalen = blaster->autolen8;
				if (blaster->dmalen == 0) {
					blaster->dmalen = 1;
				}
				blaster->blocksize = blaster->dmalen;
				blaster->blockbytes = blaster->blocksize;
				blaster->dmacount = 0;
			}
			blaster->samplerate = blaster->dorecord ? blaster->inputrate : blaster->outputrate;
			if (blaster->samplerate < 1.0) {
				blaster->samplerate = 1.0;
			}
			timing_updateIntervalFreq(blaster->timer, blaster->samplerate);
			timing_timerEnable(blaster->timer);
		}
		return;
	case 0x47:
		if (blaster->dma16 != 0) {
			blaster->autoinit = 1;
			blaster->autoinitstop = 0;
			blaster->pausedsp = 0;
			if (blaster->activedma == 0) {
				blaster->activedma = 1;
				blaster->dmalen = blaster->autolen16;
				if (blaster->dmalen == 0) {
					blaster->dmalen = 1;
				}
				blaster->blocksize = blaster->dmalen;
				blaster->blockbytes = blaster->blocksize << 1;
				blaster->dmacount = 0;
			}
			blaster->samplerate = blaster->dorecord ? blaster->inputrate : blaster->outputrate;
			if (blaster->samplerate < 1.0) {
				blaster->samplerate = 1.0;
			}
			timing_updateIntervalFreq(blaster->timer, blaster->samplerate);
			timing_timerEnable(blaster->timer);
		}
		return;
	case 0x90:
	case 0x91:
	case 0x98:
	case 0x99:
		blaster->blocksize = blaster->autolen8;
		if (blaster->blocksize == 0) {
			blaster->blocksize = 1;
		}
		blaster->blockbytes = blaster->blocksize;
		blaster->dmalen = blaster->blocksize;
		blaster->dmacount = 0;
		blaster->autoinit = ((value == 0x90) || (value == 0x98)) ? 1 : 0;
		blaster->autoinitstop = 0;
		blaster->dorecord = ((value == 0x98) || (value == 0x99)) ? 1 : 0;
		blaster->activedma = 1;
		blaster->dma16 = 0;
		blaster->dmastereo = 0;
		blaster->dmasigned = 0;
		blaster->pausedsp = 0;
		blaster->samplerate = blaster->dorecord ? blaster->inputrate : blaster->outputrate;
		if (blaster->samplerate < 1.0) {
			blaster->samplerate = 1.0;
		}
		timing_updateIntervalFreq(blaster->timer, blaster->samplerate);
		timing_timerEnable(blaster->timer);
		return;
	case 0xD0:
		if (blaster->dma16 == 0) {
			blaster->pausedsp = 1;
			timing_timerDisable(blaster->timer);
		}
		return;
	case 0xD1:
		blaster->dspenable = 1;
		return;
	case 0xD3:
		blaster->dspenable = 0;
		return;
	case 0xD4:
		if (blaster->dma16 == 0) {
			blaster->pausedsp = 0;
			if ((blaster->activedma != 0) || (blaster->pauseduration != 0)) {
				timing_timerEnable(blaster->timer);
			}
		}
		return;
	case 0xD5:
		if (blaster->dma16 != 0) {
			blaster->pausedsp = 1;
			timing_timerDisable(blaster->timer);
		}
		return;
	case 0xD6:
		if (blaster->dma16 != 0) {
			blaster->pausedsp = 0;
			if ((blaster->activedma != 0) || (blaster->pauseduration != 0)) {
				timing_timerEnable(blaster->timer);
			}
		}
		return;
	case 0xD8:
		blaster_putreadbuf(blaster, (blaster->dspenable != 0) ? 0xFF : 0x00);
		return;
	case 0xD9:
		if ((blaster->activedma != 0) && (blaster->dma16 != 0) && (blaster->autoinit != 0)) {
			blaster->autoinitstop = 1;
		}
		return;
	case 0xDA:
		if ((blaster->activedma != 0) && (blaster->dma16 == 0) && (blaster->autoinit != 0)) {
			blaster->autoinitstop = 1;
		}
		return;
	case 0xE1:
		blaster_putreadbuf(blaster, 4);
		blaster_putreadbuf(blaster, 5);
		return;
	case 0xE3:
	{
		const char* p = blaster_sb16Copyright;

		while (*p != 0) {
			blaster_putreadbuf(blaster, (uint8_t)*p++);
		}
		blaster_putreadbuf(blaster, 0);
		return;
	}
	case 0xE8:
		blaster_putreadbuf(blaster, blaster->testreg);
		return;
	case 0xF2:
		blaster_raiseIrq(blaster, 0);
		return;
	case 0xF3:
		blaster_raiseIrq(blaster, 1);
		return;
	case 0xF8:
		blaster_putreadbuf(blaster, 0);
		return;
	default:
		debug_log(DEBUG_DETAIL, "[BLASTER] Unrecognized command: 0x%02X\r\n", value);
		return;
	}
}

void blaster_write(BLASTER_t* blaster, uint16_t addr, uint8_t value) {
#ifdef DEBUG_BLASTER
	debug_log(DEBUG_DETAIL, "[BLASTER] Write %03X: %02X\r\n", addr, value);
#endif
	addr &= 0x0F;

	switch (addr) {
	case 0x04:
		blaster->mixeraddr = value;
		break;
	case 0x05:
		switch (blaster->mixeraddr) {
		case 0x00:
			blaster_resetMixer(blaster);
			break;
		case 0x02:
			blaster->mixerregs[0x30] = ((value & 0x0F) << 4) | 0x08;
			blaster->mixerregs[0x31] = ((value & 0x0F) << 4) | 0x08;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x04:
			blaster->mixerregs[0x32] = (value & 0xF0) | 0x08;
			blaster->mixerregs[0x33] = ((value & 0x0F) << 4) | 0x08;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x06:
			blaster->mixerregs[0x34] = ((value & 0x0F) << 4) | 0x08;
			blaster->mixerregs[0x35] = ((value & 0x0F) << 4) | 0x08;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x08:
			blaster->mixerregs[0x36] = ((value & 0x0F) << 4) | 0x08;
			blaster->mixerregs[0x37] = ((value & 0x0F) << 4) | 0x08;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x0A:
			blaster->mixerregs[0x3A] = ((value & 0x07) << 5) | 0x18;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x0E:
			blaster->mixerregs[0x0E] = 0x02;
			break;
		case 0x22:
			blaster->mixerregs[0x30] = (value & 0xF0) | 0x08;
			blaster->mixerregs[0x31] = ((value & 0x0F) << 4) | 0x08;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x26:
			blaster->mixerregs[0x34] = (value & 0xF0) | 0x08;
			blaster->mixerregs[0x35] = ((value & 0x0F) << 4) | 0x08;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x28:
			blaster->mixerregs[0x36] = (value & 0xF0) | 0x08;
			blaster->mixerregs[0x37] = ((value & 0x0F) << 4) | 0x08;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x2E:
			blaster->mixerregs[0x38] = (value & 0xF0) | 0x08;
			blaster->mixerregs[0x39] = ((value & 0x0F) << 4) | 0x08;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x30:
		case 0x31:
		case 0x32:
		case 0x33:
		case 0x34:
		case 0x35:
		case 0x36:
		case 0x37:
		case 0x38:
		case 0x39:
			blaster->mixerregs[blaster->mixeraddr] = value & 0xF8;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x3A:
			blaster->mixerregs[0x3A] = value & 0xE8;
			blaster_syncCompatibilityMixer(blaster);
			break;
		case 0x3B:
			blaster->mixerregs[0x3B] = value & 0xC0;
			break;
		case 0x3C:
			blaster->mixerregs[0x3C] = value & 0x1F;
			break;
		case 0x3D:
		case 0x3E:
			blaster->mixerregs[blaster->mixeraddr] = value & 0x7F;
			break;
		case 0x3F:
		case 0x40:
		case 0x41:
		case 0x42:
			blaster->mixerregs[blaster->mixeraddr] = value & 0xC0;
			break;
		case 0x43:
			blaster->mixerregs[0x43] = value & 0x01;
			break;
		case 0x44:
		case 0x45:
		case 0x46:
		case 0x47:
			blaster->mixerregs[blaster->mixeraddr] = value & 0xF0;
			break;
		case 0x48:
		case 0x49:
		case 0x4A:
		case 0x83:
		case 0x8C:
		case 0x8E:
		case 0x90:
		case 0xFD:
		case 0xFE:
		case 0xFF:
			blaster->mixerregs[blaster->mixeraddr] = value;
			break;
		case 0x80:
			if (value & 0x08) {
				blaster->irq = 10;
			} else if (value & 0x04) {
				blaster->irq = 7;
			} else if (value & 0x02) {
				blaster->irq = 5;
			} else if (value & 0x01) {
				blaster->irq = 2;
			}
			blaster->mixerregs[0x80] = blaster_getMixerIRQSetup(blaster);
			break;
		case 0x81:
			if (value & 0x80) {
				blaster->dma16chan = 7;
			} else if (value & 0x40) {
				blaster->dma16chan = 6;
			} else if (value & 0x20) {
				blaster->dma16chan = 5;
			}

			if (value & 0x08) {
				blaster->dma8chan = 3;
			} else if (value & 0x02) {
				blaster->dma8chan = 1;
			} else if (value & 0x01) {
				blaster->dma8chan = 0;
			}
			blaster->mixerregs[0x81] = blaster_getMixerDMASetup(blaster);
			break;
		case 0x82:
			break;
		case 0x84:
			blaster->mixerregs[0x84] = value & 0x07;
			break;
		default:
			blaster->mixerregs[blaster->mixeraddr] = value;
			break;
		}
		break;
	case 0x06:
		if (value == 0) {
			blaster_reset(blaster);
		}
		break;
	case 0x0C:
		blaster_writecmd(blaster, value);
		break;
	}
}

uint8_t blaster_read(BLASTER_t* blaster, uint16_t addr) {
	uint8_t ret = 0xFF;

#ifdef DEBUG_BLASTER
	debug_log(DEBUG_DETAIL, "[BLASTER] Read %03X\r\n", addr);
#endif
	addr &= 0x0F;

	switch (addr) {
	case 0x05:
		switch (blaster->mixeraddr) {
		case 0x80:
			return blaster_getMixerIRQSetup(blaster);
		case 0x81:
			return blaster_getMixerDMASetup(blaster);
		case 0x82:
			return blaster_getMixerIRQStatus(blaster);
		case 0x0E:
			return 0x02;
		case 0x84:
			return (blaster->mixerregs[0x84] & 0x01) | 0x02;
		default:
			return blaster->mixerregs[blaster->mixeraddr];
		}
	case 0x0A:
		return blaster_getreadbuf(blaster);
	case 0x0C:
		return 0x7F;
	case 0x0E:
		blaster_clearIrq(blaster, 0);
		return (blaster->readlen > 0) ? 0xFF : 0x7F;
	case 0x0F:
		blaster_clearIrq(blaster, 1);
		return ret;
	}

	return ret;
}

void blaster_generateSample(BLASTER_t* blaster) {
	uint8_t channel;
	uint8_t frameunits;
	uint8_t framebytes;

	if (blaster->pauseduration != 0) {
		blaster->sample[0] = 0;
		blaster->sample[1] = 0;
		if (++blaster->pausecount >= blaster->pauseduration) {
			blaster->pauseduration = 0;
			blaster->pausecount = 0;
			blaster_raiseIrq(blaster, 0);
			if ((blaster->activedma == 0) && (blaster->pausedsp == 0)) {
				timing_timerDisable(blaster->timer);
			}
		}
		return;
	}

	if (blaster->activedma == 0) {
		blaster->sample[0] = 0;
		blaster->sample[1] = 0;
		timing_timerDisable(blaster->timer);
		return;
	}

	if (blaster->pausedsp != 0) {
		blaster->sample[0] = 0;
		blaster->sample[1] = 0;
		return;
	}

	channel = (blaster->dma16 != 0) ? blaster->dma16chan : blaster->dma8chan;
	frameunits = (blaster->dmastereo != 0) ? 2 : 1;
	framebytes = frameunits;
	if (blaster->dma16 != 0) {
		framebytes <<= 1;
	}

	if ((blaster->dmalen < frameunits) || (dma_channel_remaining(blaster->dma, channel) < framebytes)) {
		blaster->sample[0] = 0;
		blaster->sample[1] = 0;
		return;
	}

	if (blaster->dorecord == 0) {
		if (blaster->dma16 == 0) {
			int16_t left, right;

			if (blaster->dmasigned != 0) {
				left = (int16_t)(((int8_t)dma_channel_read(blaster->dma, channel)) << 8);
			} else {
				left = (int16_t)(((int)dma_channel_read(blaster->dma, channel) - 128) * 256);
			}

			if (blaster->dmastereo != 0) {
				if (blaster->dmasigned != 0) {
					right = (int16_t)(((int8_t)dma_channel_read(blaster->dma, channel)) << 8);
				} else {
					right = (int16_t)(((int)dma_channel_read(blaster->dma, channel) - 128) * 256);
				}
				blaster->sample[0] = left;
				blaster->sample[1] = right;
			} else {
				blaster->sample[0] = left;
				blaster->sample[1] = left;
			}
		} else {
			uint16_t raw;
			int16_t left, right;

			raw = (uint16_t)dma_channel_read(blaster->dma, channel);
			raw |= (uint16_t)dma_channel_read(blaster->dma, channel) << 8;
			if (blaster->dmasigned != 0) {
				left = (int16_t)raw;
			} else {
				left = (int16_t)((int32_t)raw - 32768);
			}

			if (blaster->dmastereo != 0) {
				raw = (uint16_t)dma_channel_read(blaster->dma, channel);
				raw |= (uint16_t)dma_channel_read(blaster->dma, channel) << 8;
				if (blaster->dmasigned != 0) {
					right = (int16_t)raw;
				} else {
					right = (int16_t)((int32_t)raw - 32768);
				}
				blaster->sample[0] = left;
				blaster->sample[1] = right;
			} else {
				blaster->sample[0] = left;
				blaster->sample[1] = left;
			}
		}
	} else {
		uint8_t value8 = (blaster->dmasigned != 0) ? 0x00 : 0x80;
		uint16_t value16 = (blaster->dmasigned != 0) ? 0x0000 : 0x8000;

		blaster->sample[0] = 0;
		blaster->sample[1] = 0;
		if (blaster->dma16 == 0) {
			dma_channel_write(blaster->dma, channel, value8);
			if (blaster->dmastereo != 0) {
				dma_channel_write(blaster->dma, channel, value8);
			}
		} else {
			dma_channel_write(blaster->dma, channel, (uint8_t)(value16 & 0xFF));
			dma_channel_write(blaster->dma, channel, (uint8_t)(value16 >> 8));
			if (blaster->dmastereo != 0) {
				dma_channel_write(blaster->dma, channel, (uint8_t)(value16 & 0xFF));
				dma_channel_write(blaster->dma, channel, (uint8_t)(value16 >> 8));
			}
		}
	}

	blaster->dmacount += framebytes;
	blaster->dmalen -= frameunits;
	if (blaster->dmalen == 0) {
		blaster_raiseIrq(blaster, blaster->dma16);
		if ((blaster->autoinit != 0) && (blaster->autoinitstop == 0)) {
			blaster->dmalen = blaster->blocksize;
			blaster->dmacount = 0;
		} else {
			blaster->activedma = 0;
			blaster->autoinit = 0;
			blaster->autoinitstop = 0;
			timing_timerDisable(blaster->timer);
		}
	}
}

void blaster_getSample(BLASTER_t* blaster, int16_t* left, int16_t* right) {
	int32_t sampleleft;
	int32_t sampleright;
	uint8_t voiceleft;
	uint8_t voiceright;
	uint8_t masterleft;
	uint8_t masterright;
	uint8_t gainleft;
	uint8_t gainright;

	sampleleft = blaster->sample[0];
	sampleright = blaster->sample[1];
	voiceleft = (uint8_t)(blaster->mixerregs[0x32] >> 3);
	voiceright = (uint8_t)(blaster->mixerregs[0x33] >> 3);
	masterleft = (uint8_t)(blaster->mixerregs[0x30] >> 3);
	masterright = (uint8_t)(blaster->mixerregs[0x31] >> 3);
	gainleft = (uint8_t)(blaster->mixerregs[0x41] >> 6);
	gainright = (uint8_t)(blaster->mixerregs[0x42] >> 6);

	sampleleft = (sampleleft * voiceleft) / 31;
	sampleleft = (sampleleft * masterleft) / 31;
	sampleleft *= (1 << gainleft);
	sampleright = (sampleright * voiceright) / 31;
	sampleright = (sampleright * masterright) / 31;
	sampleright *= (1 << gainright);

	if (sampleleft > 32767) {
		sampleleft = 32767;
	} else if (sampleleft < -32768) {
		sampleleft = -32768;
	}
	if (sampleright > 32767) {
		sampleright = 32767;
	} else if (sampleright < -32768) {
		sampleright = -32768;
	}

	*left = (int16_t)sampleleft;
	*right = (int16_t)sampleright;
}

void blaster_init(BLASTER_t* blaster, DMA_t* dma, I8259_t* i8259, uint16_t base, uint8_t dma8_channel, uint8_t dma16_channel, uint8_t irq) {
	debug_log(DEBUG_DETAIL, "[BLASTER] Initializing Sound Blaster 16 at base port 0x%03X, IRQ %u, DMA %u/%u\r\n", base, irq, dma8_channel, dma16_channel);
	memset(blaster, 0, sizeof(BLASTER_t));
	blaster->dma = dma;
	blaster->i8259 = i8259;
	blaster->base = base;
	blaster->dma8chan = dma8_channel;
	blaster->dma16chan = dma16_channel;
	blaster->irq = irq;
	blaster->outputrate = 22050.0;
	blaster->inputrate = 22050.0;
	blaster->samplerate = 22050.0;
	blaster_resetMixer(blaster);
	ports_cbRegister(base, 16, (void*)blaster_read, NULL, (void*)blaster_write, NULL, blaster);

	blaster->timer = timing_addTimer(blaster_generateSample, blaster, 22050, TIMING_DISABLED);
	blaster_reset(blaster);
}
