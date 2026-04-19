#include <stdint.h>
#include <string.h>
#include "debuglog.h"
#include "ports.h"
#include "pci.h"

static void pci_update_irq_line(PCI_t* pci, uint8_t route_pin)
{
	uint8_t irq;
	uint8_t active;

	if ((pci == NULL) || (route_pin >= 4)) {
		return;
	}

	irq = pci->route_irq[route_pin];
	if (irq == PCI_IRQ_DISABLED) {
		return;
	}

	active = (pci->route_refcount[route_pin] != 0);
	if (irq >= 8) {
		if (pci->pic_slave != NULL) {
			i8259_setlevelirq(pci->pic_slave, (uint8_t)(irq - 8), active);
		}
	} else if (pci->pic_master != NULL) {
		i8259_setlevelirq(pci->pic_master, irq, active);
	}
}

static uint8_t pci_slot_route_pin(PCI_t* pci, uint8_t slot, uint8_t intpin)
{
	uint8_t route_pin;

	if ((pci == NULL) || (slot >= PCI_MAX_SLOTS) || (intpin < 1) || (intpin > 4)) {
		return 0;
	}

	route_pin = pci->slot[slot].swizzle[intpin - 1];
	if ((route_pin >= 1) && (route_pin <= 4)) {
		return route_pin;
	}

	return intpin;
}

static uint8_t pci_config_enabled(PCI_t* pci)
{
	if (pci == NULL) {
		return 0;
	}

	if (pci->config_type != PCI_CONFIG_TYPE_1) {
		return 0;
	}

	return (uint8_t)((pci->config_address & 0x80000000UL) != 0);
}

static PCI_FUNCTION_t* pci_current_function(PCI_t* pci, uint8_t reg_offset)
{
	uint8_t bus;
	uint8_t slot;
	uint8_t function;

	(void) reg_offset;

	if (!pci_config_enabled(pci)) {
		return NULL;
	}

	bus = (uint8_t)((pci->config_address >> 16) & 0xFF);
	slot = (uint8_t)((pci->config_address >> 11) & 0x1F);
	function = (uint8_t)((pci->config_address >> 8) & 0x07);
	if ((bus != 0) || (slot >= PCI_MAX_SLOTS) || (function >= PCI_MAX_FUNCTIONS)) {
		return NULL;
	}
	if (!pci->slot[slot].present) {
		return NULL;
	}
	if (!pci->slot[slot].function[function].present) {
		return NULL;
	}

	return &pci->slot[slot].function[function];
}

static uint8_t pci_read_config_byte(PCI_t* pci, uint8_t reg_offset)
{
	PCI_FUNCTION_t* function;
	uint8_t addr;

	function = pci_current_function(pci, reg_offset);
	if ((function == NULL) || (function->read == NULL)) {
		return 0xFF;
	}

	addr = (uint8_t)((pci->config_address & 0xFC) + reg_offset);
	return function->read(function->udata, (uint8_t)((pci->config_address >> 8) & 0x07), addr);
}

static void pci_write_config_byte(PCI_t* pci, uint8_t reg_offset, uint8_t value)
{
	PCI_FUNCTION_t* function;
	uint8_t addr;

	function = pci_current_function(pci, reg_offset);
	if ((function == NULL) || (function->write == NULL)) {
		return;
	}

	addr = (uint8_t)((pci->config_address & 0xFC) + reg_offset);
	function->write(function->udata, (uint8_t)((pci->config_address >> 8) & 0x07), addr, value);
}

static uint8_t pci_port_readb(void* udata, uint16_t port)
{
	PCI_t* pci = (PCI_t*)udata;
	uint8_t offset = (uint8_t)(port - 0x0CF8);

	if (offset < 4) {
		return (uint8_t)((pci->config_address >> (offset << 3)) & 0xFF);
	}

	if (offset < 8) {
		return pci_read_config_byte(pci, (uint8_t)(offset - 4));
	}

	return 0xFF;
}

static uint16_t pci_port_readw(void* udata, uint16_t port)
{
	uint16_t value;

	value = pci_port_readb(udata, port);
	value |= (uint16_t)pci_port_readb(udata, (uint16_t)(port + 1)) << 8;
	return value;
}

static uint32_t pci_port_readl(void* udata, uint16_t port)
{
	PCI_t* pci = (PCI_t*)udata;

	if (port == 0x0CF8) {
		return pci->config_address;
	}

	if (port == 0x0CFC) {
		return (uint32_t)pci_read_config_byte(pci, 0) |
			((uint32_t)pci_read_config_byte(pci, 1) << 8) |
			((uint32_t)pci_read_config_byte(pci, 2) << 16) |
			((uint32_t)pci_read_config_byte(pci, 3) << 24);
	}

	return (uint32_t)pci_port_readw(udata, port) |
		((uint32_t)pci_port_readw(udata, (uint16_t)(port + 2)) << 16);
}

static void pci_port_writeb(void* udata, uint16_t port, uint8_t value)
{
	PCI_t* pci = (PCI_t*)udata;
	uint8_t offset = (uint8_t)(port - 0x0CF8);
	uint32_t shift;
	uint32_t mask;

	if (offset < 4) {
		shift = (uint32_t)offset << 3;
		mask = 0xFFUL << shift;
		pci->config_address = (pci->config_address & ~mask) | ((uint32_t)value << shift);
		return;
	}

	if (offset < 8) {
		pci_write_config_byte(pci, (uint8_t)(offset - 4), value);
	}
}

static void pci_port_writew(void* udata, uint16_t port, uint16_t value)
{
	pci_port_writeb(udata, port, (uint8_t)value);
	pci_port_writeb(udata, (uint16_t)(port + 1), (uint8_t)(value >> 8));
}

static void pci_port_writel(void* udata, uint16_t port, uint32_t value)
{
	PCI_t* pci = (PCI_t*)udata;

	if (port == 0x0CF8) {
		pci->config_address = value;
		return;
	}

	pci_port_writeb(udata, port, (uint8_t)value);
	pci_port_writeb(udata, (uint16_t)(port + 1), (uint8_t)(value >> 8));
	pci_port_writeb(udata, (uint16_t)(port + 2), (uint8_t)(value >> 16));
	pci_port_writeb(udata, (uint16_t)(port + 3), (uint8_t)(value >> 24));
}

void pci_init(PCI_t* pci, uint8_t config_type, I8259_t* pic_master, I8259_t* pic_slave)
{
	uint8_t i;

	memset(pci, 0, sizeof(*pci));
	pci->config_type = config_type;
	pci->pic_master = pic_master;
	pci->pic_slave = pic_slave;
	pci->config_address = 0;
	for (i = 0; i < 4; i++) {
		pci->route_irq[i] = PCI_IRQ_DISABLED;
	}

	ports_cbRegisterEx(0x0CF8, 8,
		pci_port_readb, pci_port_readw, pci_port_readl,
		pci_port_writeb, pci_port_writew, pci_port_writel,
		pci);
	debug_log(DEBUG_DETAIL, "[PCI] Initialized PCI configuration mechanism #%u\n", (unsigned)config_type);
}

void pci_register_slot(PCI_t* pci, uint8_t slot, PCI_SLOT_TYPE_t type, uint8_t inta, uint8_t intb, uint8_t intc, uint8_t intd)
{
	if ((pci == NULL) || (slot >= PCI_MAX_SLOTS)) {
		return;
	}

	memset(&pci->slot[slot], 0, sizeof(pci->slot[slot]));
	pci->slot[slot].present = 1;
	pci->slot[slot].type = (uint8_t)type;
	pci->slot[slot].swizzle[0] = inta;
	pci->slot[slot].swizzle[1] = intb;
	pci->slot[slot].swizzle[2] = intc;
	pci->slot[slot].swizzle[3] = intd;
}

void pci_register_device(PCI_t* pci, uint8_t slot, uint8_t function, PCI_CONFIG_READ_CB readcb, PCI_CONFIG_WRITE_CB writecb, void* udata)
{
	if ((pci == NULL) || (slot >= PCI_MAX_SLOTS) || (function >= PCI_MAX_FUNCTIONS)) {
		return;
	}

	pci->slot[slot].present = 1;
	pci->slot[slot].function[function].present = 1;
	pci->slot[slot].function[function].read = readcb;
	pci->slot[slot].function[function].write = writecb;
	pci->slot[slot].function[function].udata = udata;
}

void pci_set_irq_routing(PCI_t* pci, uint8_t pin, uint8_t irq)
{
	uint8_t old_irq;

	if ((pci == NULL) || (pin < 1) || (pin > 4)) {
		return;
	}

	old_irq = pci->route_irq[pin - 1];
	if ((old_irq != PCI_IRQ_DISABLED) && (pci->route_refcount[pin - 1] != 0)) {
		if (old_irq >= 8) {
			if (pci->pic_slave != NULL) {
				i8259_setlevelirq(pci->pic_slave, (uint8_t)(old_irq - 8), 0);
			}
		} else if (pci->pic_master != NULL) {
			i8259_setlevelirq(pci->pic_master, old_irq, 0);
		}
	}

	pci->route_irq[pin - 1] = irq;
	pci_update_irq_line(pci, (uint8_t)(pin - 1));
}

void pci_raise_irq(PCI_t* pci, uint8_t slot, uint8_t intpin)
{
	uint8_t route_pin;

	if ((pci == NULL) || (slot >= PCI_MAX_SLOTS) || (intpin < 1) || (intpin > 4)) {
		return;
	}

	if (pci->slot[slot].irq_state[intpin - 1]) {
		return;
	}

	route_pin = pci_slot_route_pin(pci, slot, intpin);
	if ((route_pin < 1) || (route_pin > 4)) {
		return;
	}

	pci->slot[slot].irq_state[intpin - 1] = 1;
	pci->route_refcount[route_pin - 1]++;
	pci_update_irq_line(pci, (uint8_t)(route_pin - 1));
}

void pci_lower_irq(PCI_t* pci, uint8_t slot, uint8_t intpin)
{
	uint8_t route_pin;

	if ((pci == NULL) || (slot >= PCI_MAX_SLOTS) || (intpin < 1) || (intpin > 4)) {
		return;
	}

	if (!pci->slot[slot].irq_state[intpin - 1]) {
		return;
	}

	route_pin = pci_slot_route_pin(pci, slot, intpin);
	pci->slot[slot].irq_state[intpin - 1] = 0;
	if ((route_pin >= 1) && (route_pin <= 4) && (pci->route_refcount[route_pin - 1] != 0)) {
		pci->route_refcount[route_pin - 1]--;
		pci_update_irq_line(pci, (uint8_t)(route_pin - 1));
	}
}

void pci_reset(PCI_t* pci)
{
	uint8_t pin;
	uint8_t slot;

	if (pci == NULL) {
		return;
	}

	for (pin = 0; pin < 4; pin++) {
		uint8_t irq = pci->route_irq[pin];

		if ((irq != PCI_IRQ_DISABLED) && (pci->route_refcount[pin] != 0)) {
			if (irq >= 8) {
				if (pci->pic_slave != NULL) {
					i8259_setlevelirq(pci->pic_slave, (uint8_t)(irq - 8), 0);
				}
			} else if (pci->pic_master != NULL) {
				i8259_setlevelirq(pci->pic_master, irq, 0);
			}
		}

		pci->route_refcount[pin] = 0;
	}

	for (slot = 0; slot < PCI_MAX_SLOTS; slot++) {
		memset(pci->slot[slot].irq_state, 0, sizeof(pci->slot[slot].irq_state));
	}

	pci->config_address = 0;
	debug_log(DEBUG_DETAIL, "[PCI] Reset configuration state\n");
}
