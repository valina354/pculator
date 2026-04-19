#ifndef _RTL8139_H_
#define _RTL8139_H_

#include <stdint.h>
#include "../pci.h"
#include "../chipset/dma.h"

struct RTL8139State;
typedef struct RTL8139State RTL8139State;

int rtl8139_init(RTL8139State** out_dev, PCI_t* pci, DMA_t* dma, uint8_t pci_slot, const uint8_t* mac, int pcap_if);
void rtl8139_reset_device(RTL8139State* dev);

#endif
