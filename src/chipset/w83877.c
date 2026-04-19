#include <stdint.h>
#include <string.h>
#include "../debuglog.h"
#include "../ports.h"
#include "../disk/fdc.h"
#include "w83877.h"

#define W83877_CONFIG_ADDR_250 0x250
#define W83877_CONFIG_ADDR_3F0 0x3F0
#define W83877_CONFIG_DATA_250 0x252
#define W83877_CONFIG_DATA_3F0 0x3F1

#define W83877_HEFERE(dev) (((dev)->regs[0x0C] >> 5) & 0x01)
#define W83877_HEFRAS(dev) ((dev)->regs[0x16] & 0x01)

static uint8_t w83877_irq_map(uint8_t index)
{
	switch (index & 0x0F) {
	case 0x01:
		return 5;
	case 0x02:
		return 2;
	case 0x03:
		return 3;
	case 0x04:
		return 4;
	case 0x05:
		return 7;
	case 0x06:
		return 6;
	case 0x07:
		return 9;
	case 0x08:
		return 10;
	default:
		break;
	}

	return 0xFF;
}

static void w83877_update_config_base(W83877_t* dev)
{
	uint8_t hefras;

	hefras = W83877_HEFRAS(dev);
	dev->config_base = hefras ? W83877_CONFIG_ADDR_3F0 : W83877_CONFIG_ADDR_250;
	dev->key_times = hefras ? 2 : 1;
	dev->key = (uint8_t)((hefras ? 0x86 : 0x88) | W83877_HEFERE(dev));
	dev->unlock_step = 0;
}

static uint16_t w83877_make_port(const W83877_t* dev, uint8_t reg)
{
	uint16_t port;

	port = 0;
	switch (reg) {
	case 0x20:
		port = (uint16_t)(((uint16_t)(dev->regs[reg] & 0xFCU)) << 2);
		port &= 0x0FF0;
		if ((port < 0x0100) || (port > 0x03F0)) {
			port = 0x03F0;
		}
		break;
	case 0x24:
	case 0x25:
		port = (uint16_t)(((uint16_t)(dev->regs[reg] & 0xFEU)) << 2);
		port &= 0x0FF8;
		if ((port < 0x0100) || (port > 0x03F8)) {
			port = (reg == 0x24) ? 0x03F8 : 0x02F8;
		}
		break;
	default:
		break;
	}

	return port;
}

static uint8_t w83877_fdc_enabled(const W83877_t* dev)
{
	return (uint8_t)(((dev->regs[0x06] & 0x08) == 0) &&
		((dev->regs[0x20] & 0xC0) != 0) &&
		(w83877_make_port(dev, 0x20) == W83877_CONFIG_ADDR_3F0));
}

static void w83877_apply_serial(W83877_t* dev, int port)
{
	uint8_t disable_mask;
	uint8_t irq_bits;
	uint8_t irq;
	uint16_t base;
	uint8_t reg_id;

	disable_mask = port ? 0x10 : 0x20;
	reg_id = port ? 0x25 : 0x24;
	base = 0;
	irq = 0xFF;

	uart_remove(dev->uart[port]);
	if ((dev->regs[0x04] & disable_mask) != 0) {
		return;
	}
	if ((dev->regs[reg_id] & 0xC0) == 0) {
		return;
	}

	base = w83877_make_port(dev, reg_id);
	irq_bits = port ? (uint8_t)(dev->regs[0x28] & 0x0F) : (uint8_t)(dev->regs[0x28] >> 4);
	irq = w83877_irq_map(irq_bits);
	if ((base == 0) || (irq == 0xFF)) {
		return;
	}

	uart_init(dev->uart[port], dev->pic, base, irq,
		dev->tx_cb[port], dev->tx_udata[port],
		dev->mcr_cb[port], dev->mcr_udata[port]);
}

static void w83877_reset_defaults(W83877_t* dev, uint8_t enabled)
{
	dev->regs[0x1E] = enabled ? 0x81 : 0x00;
	dev->regs[0x20] = enabled ? 0xFC : 0x00;
	dev->regs[0x21] = enabled ? 0x7C : 0x00;
	dev->regs[0x22] = enabled ? 0xFD : 0x00;
	dev->regs[0x23] = enabled ? 0xDE : 0x00;
	dev->regs[0x24] = enabled ? 0xFE : 0x00;
	dev->regs[0x25] = enabled ? 0xBE : 0x00;
	dev->regs[0x26] = enabled ? 0x23 : 0x00;
	dev->regs[0x27] = enabled ? 0x65 : 0x00;
	dev->regs[0x28] = enabled ? 0x43 : 0x00;
	dev->regs[0x29] = enabled ? 0x62 : 0x00;
}

static void w83877_write_reg(W83877_t* dev, uint8_t value)
{
	uint8_t old;
	uint8_t changed;

	if ((dev->rw_locked != 0) && (dev->cur_reg < 0x18)) {
		return;
	}

	old = dev->regs[dev->cur_reg];
	changed = (uint8_t)(old ^ value);
	dev->regs[dev->cur_reg] = value;

	switch (dev->cur_reg) {
	case 0x04:
		if ((changed & 0x30) != 0) {
			w83877_apply_serial(dev, 0);
			w83877_apply_serial(dev, 1);
		}
		break;
	case 0x06:
	case 0x20:
	case 0x29:
		break;
	case 0x09:
		if ((changed & 0x40) != 0) {
			dev->rw_locked = (uint8_t)((value & 0x40) != 0);
		}
		break;
	case 0x0C:
		if ((changed & 0x20) != 0) {
			w83877_update_config_base(dev);
		}
		break;
	case 0x16:
		if ((changed & 0x02) != 0) {
			w83877_reset_defaults(dev, (uint8_t)((value & 0x02) != 0));
			w83877_apply_serial(dev, 0);
			w83877_apply_serial(dev, 1);
		}
		if ((changed & 0x01) != 0) {
			w83877_update_config_base(dev);
		}
		break;
	case 0x24:
	case 0x25:
	case 0x28:
		w83877_apply_serial(dev, 0);
		w83877_apply_serial(dev, 1);
		break;
	default:
		break;
	}
}

static uint8_t w83877_read(void* udata, uint16_t port)
{
	W83877_t* dev = (W83877_t*)udata;

	if (dev->config_base == W83877_CONFIG_ADDR_250) {
		if ((dev->locked != 0) && (port == 0x251)) {
			return dev->cur_reg;
		}
		if ((dev->locked != 0) && (port == W83877_CONFIG_DATA_250)) {
			if ((dev->rw_locked == 0) || (dev->cur_reg >= 0x18)) {
				return dev->regs[dev->cur_reg];
			}
			return 0xFF;
		}
		if ((port == W83877_CONFIG_ADDR_3F0) || (port == W83877_CONFIG_DATA_3F0)) {
			if (w83877_fdc_enabled(dev)) {
				return fdc_io_read(dev->fdc, port);
			}
		}
		return 0xFF;
	}

	if ((port == W83877_CONFIG_ADDR_3F0) || (port == W83877_CONFIG_DATA_3F0)) {
		if (dev->locked != 0) {
			if (port == W83877_CONFIG_ADDR_3F0) {
				return dev->cur_reg;
			}
			if ((dev->rw_locked == 0) || (dev->cur_reg >= 0x18)) {
				return dev->regs[dev->cur_reg];
			}
			return 0xFF;
		}
		if (w83877_fdc_enabled(dev)) {
			return fdc_io_read(dev->fdc, port);
		}
		return 0xFF;
	}

	return 0xFF;
}

static void w83877_try_unlock(W83877_t* dev, uint8_t value)
{
	if (value != dev->key) {
		dev->unlock_step = 0;
		dev->locked = 0;
		return;
	}

	if (dev->key_times == 1) {
		dev->locked = 1;
		dev->unlock_step = 0;
		return;
	}

	if (dev->unlock_step != 0) {
		dev->locked = 1;
		dev->unlock_step = 0;
	} else {
		dev->unlock_step = 1;
	}
}

static void w83877_write(void* udata, uint16_t port, uint8_t value)
{
	W83877_t* dev = (W83877_t*)udata;

	if (dev->config_base == W83877_CONFIG_ADDR_250) {
		if (port == W83877_CONFIG_ADDR_250) {
			w83877_try_unlock(dev, value);
			return;
		}
		if (port == 0x251) {
			if (dev->locked != 0) {
				dev->cur_reg = value;
				if (value == 0xAA) {
					dev->locked = 0;
				}
			}
			return;
		}
		if (port == W83877_CONFIG_DATA_250) {
			if (dev->locked != 0) {
				w83877_write_reg(dev, value);
			}
			return;
		}
		if ((port == W83877_CONFIG_ADDR_3F0) || (port == W83877_CONFIG_DATA_3F0)) {
			if (w83877_fdc_enabled(dev)) {
				fdc_io_write(dev->fdc, port, value);
			}
			return;
		}
		return;
	}

	if (port == W83877_CONFIG_ADDR_3F0) {
		if (dev->locked != 0) {
			dev->cur_reg = value;
			if (value == 0xAA) {
				dev->locked = 0;
			}
			return;
		}

		if (value == dev->key) {
			w83877_try_unlock(dev, value);
			return;
		}

		dev->unlock_step = 0;
		if (w83877_fdc_enabled(dev)) {
			fdc_io_write(dev->fdc, port, value);
		}
		return;
	}

	if (port == W83877_CONFIG_DATA_3F0) {
		if (dev->locked != 0) {
			w83877_write_reg(dev, value);
			return;
		}
		if (w83877_fdc_enabled(dev)) {
			fdc_io_write(dev->fdc, port, value);
		}
	}
}

static void w83877_reset(W83877_t* dev)
{
	memset(dev->regs, 0, sizeof(dev->regs));
	dev->regs[0x03] = 0x30;
	dev->regs[0x07] = 0xF5;
	dev->regs[0x09] = 0x0A;
	dev->regs[0x0A] = 0x1F;
	dev->regs[0x0C] = 0x28;
	dev->regs[0x0D] = 0xA3;
	dev->regs[0x16] = 0x07;
	w83877_reset_defaults(dev, 1);
	dev->locked = 0;
	dev->rw_locked = 0;
	dev->unlock_step = 0;
	dev->cur_reg = 0;
	w83877_update_config_base(dev);
	w83877_apply_serial(dev, 0);
	w83877_apply_serial(dev, 1);
}

void w83877_init(W83877_t* dev, FDC_t* fdc, UART_t* uart0, UART_t* uart1, I8259_t* pic,
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
	w83877_reset(dev);
	ports_cbRegister(W83877_CONFIG_ADDR_250, 3, w83877_read, NULL, w83877_write, NULL, dev);
	ports_cbRegister(W83877_CONFIG_ADDR_3F0, 2, w83877_read, NULL, w83877_write, NULL, dev);
	debug_log(DEBUG_DETAIL, "[SIO] Winbond W83877F initialized (config base 0x%03X)\n", dev->config_base);
}
