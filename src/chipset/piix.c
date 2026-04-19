#include <stdint.h>
#include <string.h>
#include "../debuglog.h"
#include "../machine.h"
#include "../pci.h"
#include "../ports.h"
#include "piix.h"

#define PIIX_SLOT 0x07

extern MACHINE_t machine;

static const char* piix_variant_name(const PIIX_t* dev)
{
	if ((dev != NULL) && (dev->variant == PIIX_VARIANT_PIIX3)) {
		return "PIIX3";
	}

	return "PIIX";
}

static uint8_t piix_func_present(PIIX_t* dev, uint8_t function)
{
	if ((dev == NULL) || (function >= PIIX_FUNCTION_COUNT)) {
		return 0;
	}

	return dev->function_present[function];
}

static void piix_update_function_visibility(PIIX_t* dev)
{
	memset(dev->function_present, 0, sizeof(dev->function_present));
	dev->function_present[0] = 1;

	if (dev->variant == PIIX_VARIANT_PIIX3) {
		dev->function_present[1] = 1;
		dev->function_present[2] = (uint8_t)((dev->regs[0][0x6A] & 0x10) != 0);
		dev->regs[0][0x0E] = 0x80;
		return;
	}

	dev->function_present[1] = (uint8_t)((dev->regs[0][0x6A] & 0x04) != 0);
	dev->regs[0][0x0E] = dev->function_present[1] ? 0x80 : 0x00;
}

static void piix_seed_defaults_piix(PIIX_t* dev)
{
	memset(dev->regs, 0, sizeof(dev->regs));

	dev->regs[0][0x00] = 0x86;
	dev->regs[0][0x01] = 0x80;
	dev->regs[0][0x02] = 0x2E;
	dev->regs[0][0x03] = 0x12;
	dev->regs[0][0x04] = 0x07;
	dev->regs[0][0x06] = 0x80;
	dev->regs[0][0x07] = 0x02;
	dev->regs[0][0x08] = 0x01;
	dev->regs[0][0x09] = 0x00;
	dev->regs[0][0x0A] = 0x01;
	dev->regs[0][0x0B] = 0x06;
	dev->regs[0][0x4C] = 0x4D;
	dev->regs[0][0x4E] = 0x03;
	dev->regs[0][0x60] = 0x80;
	dev->regs[0][0x61] = 0x80;
	dev->regs[0][0x62] = 0x80;
	dev->regs[0][0x63] = 0x80;
	dev->regs[0][0x69] = 0x02;
	dev->regs[0][0x6A] = 0x04;
	dev->regs[0][0x70] = 0x80;
	dev->regs[0][0x71] = 0x80;
	dev->regs[0][0x76] = 0x0C;
	dev->regs[0][0x77] = 0x0C;
	dev->regs[0][0x78] = 0x02;
	dev->regs[0][0xA0] = 0x08;
	dev->regs[0][0xA8] = 0x0F;

	dev->regs[1][0x00] = 0x86;
	dev->regs[1][0x01] = 0x80;
	dev->regs[1][0x02] = 0x2F;
	dev->regs[1][0x03] = 0x12;
	dev->regs[1][0x04] = 0x00;
	dev->regs[1][0x06] = 0x80;
	dev->regs[1][0x07] = 0x02;
	dev->regs[1][0x08] = 0x01;
	dev->regs[1][0x09] = 0x80;
	dev->regs[1][0x0A] = 0x01;
	dev->regs[1][0x0B] = 0x01;
	dev->regs[1][0x20] = 0x01;
	dev->regs[1][0x21] = 0x00;
	dev->regs[1][0x41] = 0x00;
	dev->regs[1][0x43] = 0x00;
	dev->regs[1][0x3D] = 0x01;
}

static void piix_seed_defaults_piix3(PIIX_t* dev)
{
	memset(dev->regs, 0, sizeof(dev->regs));

	dev->regs[0][0x00] = 0x86;
	dev->regs[0][0x01] = 0x80;
	dev->regs[0][0x02] = 0x00;
	dev->regs[0][0x03] = 0x70;
	dev->regs[0][0x04] = 0x07;
	dev->regs[0][0x06] = 0x80;
	dev->regs[0][0x07] = 0x02;
	dev->regs[0][0x08] = 0x00;
	dev->regs[0][0x09] = 0x00;
	dev->regs[0][0x0A] = 0x01;
	dev->regs[0][0x0B] = 0x06;
	dev->regs[0][0x0E] = 0x80;
	dev->regs[0][0x4C] = 0x4D;
	dev->regs[0][0x4E] = 0x03;
	dev->regs[0][0x60] = 0x80;
	dev->regs[0][0x61] = 0x80;
	dev->regs[0][0x62] = 0x80;
	dev->regs[0][0x63] = 0x80;
	dev->regs[0][0x64] = 0x00;
	dev->regs[0][0x69] = 0x02;
	dev->regs[0][0x6A] = 0x10;
	dev->regs[0][0x70] = 0x80;
	dev->regs[0][0x71] = 0x00;
	dev->regs[0][0x76] = 0x04;
	dev->regs[0][0x77] = 0x04;
	dev->regs[0][0x78] = 0x02;
	dev->regs[0][0xA0] = 0x08;
	dev->regs[0][0xA8] = 0x0F;

	dev->regs[1][0x00] = 0x86;
	dev->regs[1][0x01] = 0x80;
	dev->regs[1][0x02] = 0x10;
	dev->regs[1][0x03] = 0x70;
	dev->regs[1][0x04] = 0x00;
	dev->regs[1][0x06] = 0x80;
	dev->regs[1][0x07] = 0x02;
	dev->regs[1][0x08] = 0x00;
	dev->regs[1][0x09] = 0x80;
	dev->regs[1][0x0A] = 0x01;
	dev->regs[1][0x0B] = 0x01;
	dev->regs[1][0x20] = 0x01;
	dev->regs[1][0x21] = 0x00;
	dev->regs[1][0x41] = 0x00;
	dev->regs[1][0x43] = 0x00;

	dev->regs[2][0x00] = 0x86;
	dev->regs[2][0x01] = 0x80;
	dev->regs[2][0x02] = 0x20;
	dev->regs[2][0x03] = 0x70;
	dev->regs[2][0x04] = 0x00;
	dev->regs[2][0x06] = 0x80;
	dev->regs[2][0x07] = 0x02;
	dev->regs[2][0x08] = 0x01;
	dev->regs[2][0x0A] = 0x03;
	dev->regs[2][0x0B] = 0x0C;
	dev->regs[2][0x20] = 0x01;
	dev->regs[2][0x3D] = 0x04;
	dev->regs[2][0x60] = 0x00;
	dev->regs[2][0x6A] = 0x01;
	dev->regs[2][0xC1] = 0x20;
	dev->regs[2][0xFF] = 0x00;
}

static void piix_seed_defaults(PIIX_t* dev)
{
	if (dev->variant == PIIX_VARIANT_PIIX3) {
		piix_seed_defaults_piix3(dev);
	} else {
		piix_seed_defaults_piix(dev);
	}

	piix_update_function_visibility(dev);
}

static uint8_t piix_read_config(void* udata, uint8_t function, uint8_t addr)
{
	PIIX_t* dev = (PIIX_t*)udata;

	if (!piix_func_present(dev, function)) {
		return 0xFF;
	}

	return dev->regs[function][addr];
}

static void piix_update_irq_route(PIIX_t* dev, uint8_t reg)
{
	uint8_t value;

	value = dev->regs[0][reg];
	if (value & 0x80) {
		pci_set_irq_routing(dev->pci, (uint8_t)((reg - 0x60) + 1), PCI_IRQ_DISABLED);
	} else {
		pci_set_irq_routing(dev->pci, (uint8_t)((reg - 0x60) + 1), (uint8_t)(value & 0x0F));
	}
}

static uint8_t piix_port92_read(void* udata, uint16_t port)
{
	PIIX_t* dev = (PIIX_t*)udata;
	uint8_t value;

	(void)port;
	value = (uint8_t)(dev->port92_reg & 0x01);
	if (machine.CPU.a20_gate) {
		value |= 0x02;
	}

	return (uint8_t)(value | 0x24);
}

static void piix_port92_write(void* udata, uint16_t port, uint8_t value)
{
	PIIX_t* dev = (PIIX_t*)udata;
	uint8_t old = dev->port92_reg;

	(void)port;
	dev->port92_reg = (uint8_t)(value & 0x03);
	machine.CPU.a20_gate = (uint8_t)((value & 0x02) != 0);
	debug_log(DEBUG_DETAIL,
		"[PIIX] PORT92 write old=%02X new=%02X a20=%u\n",
		(unsigned int)old,
		(unsigned int)dev->port92_reg,
		(unsigned int)machine.CPU.a20_gate);

	if ((!(old & 0x01)) && (dev->port92_reg & 0x01)) {
		debug_log(DEBUG_INFO, "[PIIX] PORT92 reset pulse\n");
		machine_platform_reset();
	}
}

static void piix_write_func0(PIIX_t* dev, uint8_t addr, uint8_t value)
{
	switch (addr) {
	case 0x04:
		dev->regs[0][0x04] = (value & 0x08) | 0x07;
		break;
	case 0x07:
		if (value & 0x20) {
			dev->regs[0][0x07] &= 0xDF;
		}
		if (value & 0x10) {
			dev->regs[0][0x07] &= 0xEF;
		}
		if (value & 0x08) {
			dev->regs[0][0x07] &= 0xF7;
		}
		if (value & 0x04) {
			dev->regs[0][0x07] &= 0xFB;
		}
		break;
	case 0x4C:
	case 0x4E:
		dev->regs[0][addr] = value;
		break;
	case 0x60:
	case 0x61:
	case 0x62:
	case 0x63:
		dev->regs[0][addr] = value & 0x8F;
		piix_update_irq_route(dev, addr);
		break;
	case 0x69:
		dev->regs[0][0x69] = (dev->variant == PIIX_VARIANT_PIIX3) ? (value & 0xFE) : (value & 0xFA);
		break;
	case 0x6A:
		if (dev->variant == PIIX_VARIANT_PIIX3) {
			dev->regs[0][0x6A] = value & 0xD1;
		} else {
			dev->regs[0][0x6A] = (dev->regs[0][0x6A] & 0xFB) | (value & 0x04);
		}
		piix_update_function_visibility(dev);
		break;
	case 0x70:
		dev->regs[0][0x70] = (dev->variant == PIIX_VARIANT_PIIX3) ? (value & 0xEF) : (value & 0xCF);
		break;
	case 0x71:
		if (dev->variant != PIIX_VARIANT_PIIX3) {
			dev->regs[0][0x71] = value & 0xCF;
		}
		break;
	case 0x76:
	case 0x77:
		dev->regs[0][addr] = (dev->variant == PIIX_VARIANT_PIIX3) ? (value & 0x87) : (value & 0x8F);
		break;
	case 0x78:
	case 0x79:
	case 0xA0:
	case 0xA8:
		dev->regs[0][addr] = value;
		break;
	default:
		break;
	}
}

static void piix_write_func1(PIIX_t* dev, uint8_t addr, uint8_t value)
{
	switch (addr) {
	case 0x04:
		dev->regs[1][0x04] = (value & 0x05) | 0x02;
		break;
	case 0x07:
		dev->regs[1][0x07] &= (uint8_t)~(value & 0x38);
		break;
	case 0x20:
		dev->regs[1][0x20] = (value & 0xF0) | 0x01;
		break;
	case 0x21:
		dev->regs[1][0x21] = value;
		break;
	case 0x40:
	case 0x42:
		dev->regs[1][addr] = value;
		break;
	case 0x41:
	case 0x43:
		dev->regs[1][addr] = value & 0xB3;
		break;
	default:
		break;
	}
}

static void piix_write_func2(PIIX_t* dev, uint8_t addr, uint8_t value)
{
	if (dev->variant != PIIX_VARIANT_PIIX3) {
		return;
	}

	switch (addr) {
	case 0x04:
		dev->regs[2][0x04] = value & 0x05;
		break;
	case 0x07:
		dev->regs[2][0x07] &= (uint8_t)~(value & 0x38);
		break;
	case 0x20:
		dev->regs[2][0x20] = (value & 0xF0) | 0x01;
		break;
	case 0x21:
	case 0x22:
	case 0x23:
	case 0x3C:
		dev->regs[2][addr] = value;
		break;
	default:
		break;
	}
}

static void piix_write_config(void* udata, uint8_t function, uint8_t addr, uint8_t value)
{
	PIIX_t* dev = (PIIX_t*)udata;

	if (function == 0) {
		piix_write_func0(dev, addr, value);
	} else if (function == 1) {
		piix_write_func1(dev, addr, value);
	} else if (function == 2) {
		piix_write_func2(dev, addr, value);
	}
}

void piix_reset(PIIX_t* dev)
{
	uint8_t reg;

	if (dev == NULL) {
		return;
	}

	piix_seed_defaults(dev);
	dev->port92_reg = 0;
	piix_update_function_visibility(dev);

	for (reg = 0x60; reg <= 0x63; reg++) {
		piix_update_irq_route(dev, reg);
	}

	debug_log(DEBUG_DETAIL,
		"[%s] Reset complete fn1=%u fn2=%u port92=%02X\n",
		piix_variant_name(dev),
		(unsigned int)dev->function_present[1],
		(unsigned int)dev->function_present[2],
		(unsigned int)dev->port92_reg);
}

static void piix_init_common(PIIX_t* dev, PCI_t* pci, PIIX_VARIANT_t variant)
{
	memset(dev, 0, sizeof(*dev));
	dev->pci = pci;
	dev->variant = (uint8_t)variant;

	pci_register_device(pci, PIIX_SLOT, 0, piix_read_config, piix_write_config, dev);
	pci_register_device(pci, PIIX_SLOT, 1, piix_read_config, piix_write_config, dev);
	if (variant == PIIX_VARIANT_PIIX3) {
		pci_register_device(pci, PIIX_SLOT, 2, piix_read_config, piix_write_config, dev);
	}
	ports_cbRegister(0x0092, 1, piix_port92_read, NULL, piix_port92_write, NULL, dev);
	piix_reset(dev);
	pci_set_irq_routing(pci, 1, PCI_IRQ_DISABLED);
	pci_set_irq_routing(pci, 2, PCI_IRQ_DISABLED);
	pci_set_irq_routing(pci, 3, PCI_IRQ_DISABLED);
	pci_set_irq_routing(pci, 4, PCI_IRQ_DISABLED);
	debug_log(DEBUG_DETAIL,
		(dev->variant == PIIX_VARIANT_PIIX3) ?
			"[PIIX3] Intel 82371SB southbridge initialized\n" :
			"[PIIX] Intel 82371FB southbridge initialized\n");
}

void piix_init(PIIX_t* dev, PCI_t* pci)
{
	piix_init_common(dev, pci, PIIX_VARIANT_PIIX);
}

void piix3_init(PIIX_t* dev, PCI_t* pci)
{
	piix_init_common(dev, pci, PIIX_VARIANT_PIIX3);
}
