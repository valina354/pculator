/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Raw floppy image helpers.
 *
 * Adapted for PCulator.
 */
#ifndef _FDD_IMG_H_
#define _FDD_IMG_H_

#include <stdio.h>
#include <stdint.h>
#include "fdd.h"

typedef struct FDD_IMAGE_s {
	FILE* fp;
	uint32_t size;
	uint16_t tracks;
	uint8_t sectors;
	uint8_t sides;
	uint8_t sector_size_code;
} FDD_IMAGE_t;

int fdd_img_load(FDD_IMAGE_t* image, const char* path, int check_bpb, int* write_protected);
void fdd_img_close(FDD_IMAGE_t* image);
int fdd_img_read_sector(FDD_IMAGE_t* image, int sector, int track, int side, int sector_size, uint8_t* buf, sector_id_t* id);
int fdd_img_write_sector(FDD_IMAGE_t* image, int sector, int track, int side, int sector_size, const uint8_t* buf, sector_id_t* id, int write_protected);
int fdd_img_read_address(FDD_IMAGE_t* image, int track, int side, int index, sector_id_t* id);
int fdd_img_format_track(FDD_IMAGE_t* image, int track, int side, int sector_size, int sectors, uint8_t fill, const uint8_t* ids, sector_id_t* last_id, int write_protected);

#endif
