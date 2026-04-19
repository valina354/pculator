#ifndef _PCI_H_
#define _PCI_H_

#include <stdint.h>
#include "chipset/i8259.h"

#define PCI_CONFIG_TYPE_1 1
#define PCI_MAX_SLOTS 32
#define PCI_MAX_FUNCTIONS 8
#define PCI_IRQ_DISABLED 0xFF

typedef enum {
	PCI_CARD_NORTHBRIDGE = 0,
	PCI_CARD_NORMAL,
	PCI_CARD_SOUTHBRIDGE
} PCI_SLOT_TYPE_t;

typedef uint8_t (*PCI_CONFIG_READ_CB)(void* udata, uint8_t function, uint8_t addr);
typedef void (*PCI_CONFIG_WRITE_CB)(void* udata, uint8_t function, uint8_t addr, uint8_t value);

typedef struct {
	uint8_t present;
	PCI_CONFIG_READ_CB read;
	PCI_CONFIG_WRITE_CB write;
	void* udata;
} PCI_FUNCTION_t;

typedef struct {
	uint8_t present;
	uint8_t type;
	uint8_t swizzle[4];
	uint8_t irq_state[4];
	PCI_FUNCTION_t function[PCI_MAX_FUNCTIONS];
} PCI_SLOT_t;

typedef struct {
	uint8_t config_type;
	uint32_t config_address;
	I8259_t* pic_master;
	I8259_t* pic_slave;
	uint8_t route_irq[4];
	uint8_t route_refcount[4];
	PCI_SLOT_t slot[PCI_MAX_SLOTS];
} PCI_t;

void pci_init(PCI_t* pci, uint8_t config_type, I8259_t* pic_master, I8259_t* pic_slave);
void pci_register_slot(PCI_t* pci, uint8_t slot, PCI_SLOT_TYPE_t type, uint8_t inta, uint8_t intb, uint8_t intc, uint8_t intd);
void pci_register_device(PCI_t* pci, uint8_t slot, uint8_t function, PCI_CONFIG_READ_CB readcb, PCI_CONFIG_WRITE_CB writecb, void* udata);
void pci_set_irq_routing(PCI_t* pci, uint8_t pin, uint8_t irq);
void pci_raise_irq(PCI_t* pci, uint8_t slot, uint8_t intpin);
void pci_lower_irq(PCI_t* pci, uint8_t slot, uint8_t intpin);
void pci_reset(PCI_t* pci);

#endif
