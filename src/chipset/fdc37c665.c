#include <stdint.h>
#include <string.h>
#include "../debuglog.h"
#include "../ports.h"
#include "../disk/fdc.h"
#include "fdc37c665.h"

#define FDC37C665_CONFIG_PORT 0x3F0
#define FDC37C665_DATA_PORT   0x3F1

static void fdc37c665_set_com34_addr(FDC37C665_t* dev)
{
	switch (dev->regs[0x01] & 0x60) {
	case 0x00:
		dev->com3_addr = 0x338;
		dev->com4_addr = 0x238;
		break;
	case 0x20:
		dev->com3_addr = 0x3E8;
		dev->com4_addr = 0x2E8;
		break;
	case 0x40:
		dev->com3_addr = 0x3E8;
		dev->com4_addr = 0x2E0;
		break;
	case 0x60:
		dev->com3_addr = 0x220;
		dev->com4_addr = 0x228;
		break;
	}
}

static void fdc37c665_apply_serial(FDC37C665_t* dev, int port)
{
	uint8_t shift;
	uint8_t mode;
	uint16_t base = 0;
	uint8_t irq = 0;

	shift = (uint8_t)(port << 2);
	mode = (uint8_t)((dev->regs[0x02] >> shift) & 0x07);
	uart_remove(dev->uart[port]);
	if ((mode & 0x04) == 0) {
		return;
	}

	switch (mode & 0x03) {
	case 0:
		base = 0x3F8;
		irq = 4;
		break;
	case 1:
		base = 0x2F8;
		irq = 3;
		break;
	case 2:
		base = dev->com3_addr;
		irq = 4;
		break;
	case 3:
		base = dev->com4_addr;
		irq = 3;
		break;
	}

	if (base != 0) {
		uart_init(dev->uart[port], dev->pic, base, irq,
			dev->tx_cb[port], dev->tx_udata[port],
			dev->mcr_cb[port], dev->mcr_udata[port]);
	}
}

static void fdc37c665_reset(FDC37C665_t* dev)
{
	memset(dev->regs, 0, sizeof(dev->regs));
	dev->tries = 0;
	dev->max_reg = 0x0F;
	dev->cur_reg = 0;
	dev->regs[0x00] = 0x3B;
	dev->regs[0x01] = 0x9F;
	dev->regs[0x02] = 0xDC;
	dev->regs[0x03] = 0x78;
	dev->regs[0x06] = 0xFF;
	dev->regs[0x0D] = 0x65;
	dev->regs[0x0E] = 0x02;
	fdc37c665_set_com34_addr(dev);
	fdc37c665_apply_serial(dev, 0);
	fdc37c665_apply_serial(dev, 1);
}

static uint8_t fdc37c665_read(void* udata, uint16_t port)
{
	FDC37C665_t* dev = (FDC37C665_t*)udata;

	if ((dev->tries == 2) && (port == FDC37C665_DATA_PORT) && (dev->cur_reg <= dev->max_reg)) {
		return dev->regs[dev->cur_reg];
	}

	return fdc_io_read(dev->fdc, port);
}

static void fdc37c665_write_reg(FDC37C665_t* dev, uint8_t value)
{
	uint8_t valxor;

	if (dev->cur_reg > dev->max_reg) {
		return;
	}

	valxor = (uint8_t)(value ^ dev->regs[dev->cur_reg]);
	dev->regs[dev->cur_reg] = value;
	switch (dev->cur_reg) {
	case 0x01:
		if (valxor & 0x60) {
			fdc37c665_set_com34_addr(dev);
			fdc37c665_apply_serial(dev, 0);
			fdc37c665_apply_serial(dev, 1);
		}
		break;
	case 0x02:
		if (valxor & 0x07) {
			fdc37c665_apply_serial(dev, 0);
		}
		if (valxor & 0x70) {
			fdc37c665_apply_serial(dev, 1);
		}
		break;
	case 0x04:
		if (valxor & 0x30) {
			fdc37c665_apply_serial(dev, 0);
			fdc37c665_apply_serial(dev, 1);
		}
		break;
	default:
		break;
	}
}

static void fdc37c665_write(void* udata, uint16_t port, uint8_t value)
{
	FDC37C665_t* dev = (FDC37C665_t*)udata;

	if (dev->tries == 2) {
		if (port == FDC37C665_CONFIG_PORT) {
			if (value == 0xAA) {
				dev->tries = 0;
			} else {
				dev->cur_reg = value;
			}
		} else if (port == FDC37C665_DATA_PORT) {
			fdc37c665_write_reg(dev, value);
		}
		return;
	}

	if ((port == FDC37C665_CONFIG_PORT) && (value == 0x55)) {
		if (dev->tries < 2) {
			dev->tries++;
		}
		return;
	}

	fdc_io_write(dev->fdc, port, value);
}

void fdc37c665_init(FDC37C665_t* dev, FDC_t* fdc, UART_t* uart0, UART_t* uart1, I8259_t* pic,
	void (*uart0_tx)(void*, uint8_t), void* uart0_udata, void (*uart0_mcr)(void*, uint8_t), void* uart0_udata2,
	void (*uart1_tx)(void*, uint8_t), void* uart1_udata, void (*uart1_mcr)(void*, uint8_t), void* uart1_udata2)
{
	memset(dev, 0, sizeof(*dev));
	dev->fdc = fdc;
	dev->uart[0] = uart0;
	dev->uart[1] = uart1;
	dev->pic = pic;
	dev->tx_cb[0] = uart0_tx;
	dev->tx_udata[0] = uart0_udata;
	dev->mcr_cb[0] = uart0_mcr;
	dev->mcr_udata[0] = uart0_udata2;
	dev->tx_cb[1] = uart1_tx;
	dev->tx_udata[1] = uart1_udata;
	dev->mcr_cb[1] = uart1_mcr;
	dev->mcr_udata[1] = uart1_udata2;
	fdc37c665_reset(dev);
	ports_cbRegister(FDC37C665_CONFIG_PORT, 2, fdc37c665_read, NULL, fdc37c665_write, NULL, dev);
	debug_log(DEBUG_DETAIL, "[SIO] SMC FDC37C665 initialized at 0x3F0\n");
}
