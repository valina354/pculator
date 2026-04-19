#ifndef ATAPI_CDROM_H
#define ATAPI_CDROM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ATAPI_CDROM_BLOCK_SIZE 2048U
#define ATAPI_CDROM_CDB_SIZE 12U

typedef enum {
	ATAPI_CDROM_CMD_ERROR = -1,
	ATAPI_CDROM_CMD_OK = 0,
	ATAPI_CDROM_CMD_DATA_IN = 1
} ATAPI_CDROM_CMD_RESULT_t;

typedef struct {
	FILE* image;
	uint64_t total_blocks;
	uint32_t block_size;
	char path[512];
	uint8_t sense_key;
	uint8_t sense_asc;
	uint8_t sense_ascq;
	uint8_t unit_attention;
	uint8_t prevent_removal;
	uint8_t has_media;
	uint8_t tray_open;
} ATAPI_CDROM_t;

void atapi_cdrom_init(ATAPI_CDROM_t* cdrom);
void atapi_cdrom_reset(ATAPI_CDROM_t* cdrom);
void atapi_cdrom_close(ATAPI_CDROM_t* cdrom);
int atapi_cdrom_attach(ATAPI_CDROM_t* cdrom, const char* path);
int atapi_cdrom_reload(ATAPI_CDROM_t* cdrom);
int atapi_cdrom_eject(ATAPI_CDROM_t* cdrom);
const char* atapi_cdrom_get_path(const ATAPI_CDROM_t* cdrom);
ATAPI_CDROM_CMD_RESULT_t atapi_cdrom_command(ATAPI_CDROM_t* cdrom,
	const uint8_t cdb[ATAPI_CDROM_CDB_SIZE],
	uint8_t** buffer,
	size_t* buffer_len,
	size_t* buffer_capacity);

#endif
