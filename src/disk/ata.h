#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdio.h>
#include "../chipset/i8259.h"
#include "atapi_cdrom.h"

#define ATA_PORT_DATA			0x1F0
#define ATA_PORT_ERROR			0x1F1
#define ATA_PORT_FEATURES		0x1F1
#define ATA_PORT_SECTORS		0x1F2
#define ATA_PORT_LBA_LOW		0x1F3
#define ATA_PORT_LBA_MID		0x1F4
#define ATA_PORT_LBA_HIGH		0x1F5
#define ATA_PORT_DRIVE			0x1F6
#define ATA_PORT_STATUS			0x1F7
#define ATA_PORT_COMMAND		0x1F7
#define ATA_PORT_ALTERNATE		0x3F6
#define ATA_PORT_DATA_SEC		0x170
#define ATA_PORT_ERROR_SEC		0x171
#define ATA_PORT_FEATURES_SEC	0x171
#define ATA_PORT_SECTORS_SEC	0x172
#define ATA_PORT_LBA_LOW_SEC	0x173
#define ATA_PORT_LBA_MID_SEC	0x174
#define ATA_PORT_LBA_HIGH_SEC	0x175
#define ATA_PORT_DRIVE_SEC		0x176
#define ATA_PORT_STATUS_SEC		0x177
#define ATA_PORT_COMMAND_SEC	0x177
#define ATA_PORT_ALTERNATE_SEC	0x376

#define ATA_CMD_DEVICE_RESET		0x08
#define ATA_CMD_RECALIBRATE			0x10
#define ATA_CMD_READ_SECTORS		0x20
#define ATA_CMD_WRITE_SECTORS		0x30
#define ATA_CMD_DIAGNOSTIC			0x90
#define ATA_CMD_INITIALIZE_PARAMS	0x91
#define ATA_CMD_PACKET				0xA0
#define ATA_CMD_IDENTIFY_PACKET		0xA1
#define ATA_CMD_READ_MULTIPLE		0xC4
#define ATA_CMD_IDLE_IMMEDIATE		0xE1
#define ATA_CMD_IDENTIFY			0xEC

#define ATA_STATUS_BUSY			0x80
#define ATA_STATUS_DRDY			0x40
#define ATA_STATUS_DSC			0x10
#define ATA_STATUS_DRQ			0x08
#define ATA_STATUS_ERR			0x01

#define ATA_CHANNEL_COUNT			2
#define ATA_CHANNEL_DEVICE_COUNT	2
#define ATA_TOTAL_DEVICE_COUNT		(ATA_CHANNEL_COUNT * ATA_CHANNEL_DEVICE_COUNT)

typedef enum {
	ATA_DEVICE_NONE = 0,
	ATA_DEVICE_HDD,
	ATA_DEVICE_ATAPI_CDROM
} ATA_DEVICE_TYPE_t;

typedef enum {
	ATA_TRANSFER_NONE = 0,
	ATA_TRANSFER_HDD_READ,
	ATA_TRANSFER_HDD_WRITE,
	ATA_TRANSFER_ATAPI_PACKET,
	ATA_TRANSFER_ATAPI_DATA_IN
} ATA_TRANSFER_STATE_t;

typedef struct {
	uint8_t features;
	uint8_t sectors;
	uint32_t lba;
	uint8_t drive;
} ATA_REGS_t;

typedef struct {
	ATA_DEVICE_TYPE_t type;
	ATA_TRANSFER_STATE_t transfer_state;
	FILE* diskfile;
	char path[512];
	uint32_t sectors;
	uint32_t heads;
	uint32_t cylinders;
	uint32_t spt;
	uint32_t cursect;
	uint32_t curhead;
	uint32_t curcyl;
	uint8_t interrupt;
	uint8_t service;
	uint8_t delayed_identify_abort;
	uint8_t error;
	uint8_t status;
	uint8_t status_override_valid;
	uint8_t command;
	uint8_t lbamode;
	uint8_t lastcmd;
	uint16_t curreadsect;
	uint16_t targetsect;
	uint8_t* buffer;
	uint32_t buffer_size;
	uint32_t buffer_len;
	uint32_t buffer_pos;
	uint32_t transfer_chunk;
	uint32_t transfer_chunk_pos;
	uint32_t packet_limit;
	uint8_t packet_cdb[ATAPI_CDROM_CDB_SIZE];
	ATAPI_CDROM_t cdrom;
	ATA_REGS_t regs;
} ATA_SLOT_t;

typedef struct {
	uint8_t select;
	uint8_t delay_irq;
	uint8_t inreset;
	uint8_t control;
	uint8_t irq;
	uint8_t irq_pending;
	uint8_t irq_drive;
	uint32_t timernum;
	uint32_t resettimer;
	uint32_t cmdtimer;
	uint8_t readssincecommand;
	uint32_t savelba;
	uint8_t dscflag;
	I8259_t* i8259;
	ATA_SLOT_t disk[2];
} ATA_t;

int ata_attach_hdd(int select, const char* filename);
int ata_attach_cdrom(int select, const char* filename);
void ata_detach(int select);
ATA_DEVICE_TYPE_t ata_get_device_type(int select);
const char* ata_get_cdrom_media_path(int select);
int ata_change_cdrom(int select, const char* filename);
int ata_eject_cdrom(int select);
int ata_insert_disk(int select, char* filename);
void ata_warm_reset(void);
void ata_init(I8259_t* i8259);
void ata_init_dual(I8259_t* i8259);

#endif
