#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include "../../chipset/i8259.h"

#define ATA_PORT_DATA			0x1F0
#define ATA_PORT_ERROR			0x1F1
#define ATA_PORT_FEATURES		0x1F1
#define ATA_PORT_SECTORS		0x1F2
#define ATA_PORT_LBA_LOW		0x1F3
#define ATA_PORT_LBA_MID		0x1F4
#define ATA_PORT_LBA_HIGH		0x1F5
#define ATA_PORT_DRIVE			0x1F6 //also head or top LBA bits in bits 0-4
#define ATA_PORT_STATUS			0x1F7
#define ATA_PORT_COMMAND		0x1F7
#define ATA_PORT_ALTERNATE		0x3F6

#define ATA_CMD_IDENTIFY			0xEC
#define ATA_CMD_DIAGNOSTIC			0x90
#define ATA_CMD_INITIALIZE_PARAMS	0x91
#define ATA_CMD_RECALIBRATE			0x10
#define ATA_CMD_IDLE_IMMEDIATE		0xE1
#define ATA_CMD_READ_SECTORS		0x20
#define ATA_CMD_WRITE_SECTORS		0x30
#define ATA_CMD_READ_MULTIPLE		0xC4
#define ATA_CMD_DEVICE_RESET		0x08

#define ATA_STATUS_BUSY			0x80
#define ATA_STATUS_DRDY			0x40
#define ATA_STATUS_DSC			0x10
#define ATA_STATUS_DRQ			0x08
#define ATA_STATUS_ERR			0x01

typedef struct {
	uint8_t select;
	uint8_t delay_irq;
	uint8_t inreset;
	uint32_t timernum;
	uint32_t resettimer;
	uint8_t readssincecommand;
	uint32_t savelba;
	uint8_t dscflag;
	I8259_t* i8259;
	struct disk_s {
		FILE* diskfile;
		uint32_t sectors;
		uint32_t heads;
		uint32_t cylinders;
		uint32_t spt;
		uint32_t cursect;
		uint32_t curhead;
		uint32_t curcyl;
		uint8_t iswriting;
		uint8_t isreading;
		uint8_t interrupt;
		char model[20];
		uint8_t error;
		uint8_t status;
		uint8_t command;
		uint8_t lbamode;
		uint8_t lastcmd;
		uint16_t curreadsect;
		uint16_t targetsect;
		uint8_t buffer[512];
		int buffer_pos;
		struct regs_s {
			uint8_t features;
			uint8_t sectors;
			uint32_t lba;
			uint8_t drive;
		} regs;
	} disk[2];
} ATA_t;

int ata_insert_disk(int select, char* filename);
void ata_init(I8259_t* i8259);

#endif
