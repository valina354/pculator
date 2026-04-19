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
 * Adapted for PCulator as a lean raw-image backend.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../config.h"
#include "../debuglog.h"
#include "../host_endian.h"
#include "fdd_img.h"

#ifdef DEBUG_FDC
#define fdd_img_log_detail(...) debug_log(DEBUG_DETAIL, __VA_ARGS__)
#else
#define fdd_img_log_detail(...) do { } while (0)
#endif

static int fdd_img_sector_size_code(int sector_size)
{
	switch (sector_size) {
	case 128:
		return 0;
	case 256:
		return 1;
	default:
	case 512:
		return 2;
	case 1024:
		return 3;
	case 2048:
		return 4;
	case 4096:
		return 5;
	case 8192:
		return 6;
	case 16384:
		return 7;
	}
}

static int fdd_img_bps_is_valid(uint16_t bps)
{
	int i;

	for (i = 0; i <= 8; i++) {
		if (bps == (uint16_t)(128U << i)) {
			return 1;
		}
	}

	return 0;
}

static int fdd_img_first_byte_is_valid(uint8_t first_byte)
{
	switch (first_byte) {
	case 0x60:
	case 0xE9:
	case 0xEB:
		return 1;
	default:
		break;
	}

	return 0;
}

static int fdd_img_is_divisible(uint32_t total, uint8_t what)
{
	if ((total == 0) || (what == 0)) {
		return 0;
	}

	return ((total % what) == 0) ? 1 : 0;
}

static int fdd_img_guess_geometry(FDD_IMAGE_t* image, uint32_t size, uint16_t bpb_bps, uint32_t bpb_total, uint8_t bpb_sectors, uint8_t bpb_sides, uint8_t first_byte, int check_bpb)
{
	int guess;

	image->sides = 2;
	image->sector_size_code = 2;

	guess = (bpb_sides < 1);
	guess = guess || (bpb_sides > 2);
	guess = guess || !fdd_img_bps_is_valid(bpb_bps);
	guess = guess || !fdd_img_first_byte_is_valid(first_byte);
	guess = guess || !fdd_img_is_divisible(bpb_total, bpb_sectors);
	guess = guess || !fdd_img_is_divisible(bpb_total, bpb_sides);
	guess = guess || !check_bpb;

	if (guess) {
		if (size <= (160U * 1024U)) {
			image->sectors = 8;
			image->tracks = 40;
			image->sides = 1;
		}
		else if (size <= (180U * 1024U)) {
			image->sectors = 9;
			image->tracks = 40;
			image->sides = 1;
		}
		else if (size <= (315U * 1024U)) {
			image->sectors = 9;
			image->tracks = 70;
			image->sides = 1;
		}
		else if (size <= (320U * 1024U)) {
			image->sectors = 8;
			image->tracks = 40;
		}
		else if (size <= (360U * 1024U)) {
			image->sectors = 9;
			image->tracks = 40;
		}
		else if (size <= (400U * 1024U)) {
			image->sectors = 10;
			image->tracks = 80;
			image->sides = 1;
		}
		else if (size <= (640U * 1024U)) {
			image->sectors = 8;
			image->tracks = 80;
		}
		else if (size <= (720U * 1024U)) {
			image->sectors = 9;
			image->tracks = 80;
		}
		else if (size <= (800U * 1024U)) {
			image->sectors = 10;
			image->tracks = 80;
		}
		else if (size <= (880U * 1024U)) {
			image->sectors = 11;
			image->tracks = 80;
		}
		else if (size <= (960U * 1024U)) {
			image->sectors = 12;
			image->tracks = 80;
		}
		else if (size <= (1040U * 1024U)) {
			image->sectors = 13;
			image->tracks = 80;
		}
		else if (size <= (1120U * 1024U)) {
			image->sectors = 14;
			image->tracks = 80;
		}
		else if (size <= 1228800U) {
			image->sectors = 15;
			image->tracks = 80;
		}
		else if (size <= 1261568U) {
			image->sectors = 8;
			image->tracks = 77;
			image->sector_size_code = 3;
		}
		else if (size <= 1474560U) {
			image->sectors = 18;
			image->tracks = 80;
		}
		else if (size <= 1556480U) {
			image->sectors = 19;
			image->tracks = 80;
		}
		else if (size <= 1638400U) {
			image->sectors = 20;
			image->tracks = 80;
		}
		else if (size <= 1720320U) {
			image->sectors = 21;
			image->tracks = 80;
		}
		else if (size <= 1741824U) {
			image->sectors = 21;
			image->tracks = 81;
		}
		else if (size <= 1763328U) {
			image->sectors = 21;
			image->tracks = 82;
		}
		else if (size <= 1802240U) {
			image->sectors = 22;
			image->tracks = 80;
		}
		else if (size == 1884160U) {
			image->sectors = 23;
			image->tracks = 80;
		}
		else if (size <= 2949120U) {
			image->sectors = 36;
			image->tracks = 80;
		}
		else if (size <= 3194880U) {
			image->sectors = 39;
			image->tracks = 80;
		}
		else if (size <= 3276800U) {
			image->sectors = 40;
			image->tracks = 80;
		}
		else if (size <= 3358720U) {
			image->sectors = 41;
			image->tracks = 80;
		}
		else if (size <= 3440640U) {
			image->sectors = 42;
			image->tracks = 80;
		}
		else if (size <= 3604480U) {
			image->sectors = 22;
			image->tracks = 80;
			image->sector_size_code = 3;
		}
		else if (size <= 3610624U) {
			image->sectors = 41;
			image->tracks = 86;
		}
		else if (size <= 3698688U) {
			image->sectors = 42;
			image->tracks = 86;
		}
		else {
			return -1;
		}
	}
	else {
		image->tracks = (uint16_t)(bpb_total / ((uint32_t)bpb_sides * (uint32_t)bpb_sectors));
		image->sectors = bpb_sectors;
		image->sides = bpb_sides;
		image->sector_size_code = (uint8_t)fdd_img_sector_size_code(bpb_bps);
	}

	if ((image->tracks == 0) || (image->sectors == 0) || (image->sides == 0)) {
		return -1;
	}

	fdd_img_log_detail("[FDD_IMG] Geometry tracks=%u sides=%u sectors=%u ssize=%u guessed=%u\r\n",
		(unsigned)image->tracks,
		(unsigned)image->sides,
		(unsigned)image->sectors,
		(unsigned)(128U << image->sector_size_code),
		(unsigned)guess);

	return 0;
}

static int fdd_img_sector_offset(FDD_IMAGE_t* image, int sector, int track, int side, int sector_size, uint32_t* offset, uint32_t* sector_bytes)
{
	uint32_t bytes_per_sector;
	uint32_t track_bytes;
	uint32_t pos;

	if ((image == NULL) || (image->fp == NULL)) {
		return -1;
	}

	if ((track < 0) || (track >= image->tracks) || (side < 0) || (side >= image->sides)) {
		fdd_img_log_detail("[FDD_IMG] Invalid CH track=%d side=%d max_track=%u sides=%u\r\n",
			track, side, (unsigned)image->tracks, (unsigned)image->sides);
		return -1;
	}
	if ((sector < 1) || (sector > image->sectors)) {
		fdd_img_log_detail("[FDD_IMG] Invalid sector=%d max=%u\r\n", sector, (unsigned)image->sectors);
		return -1;
	}
	if (sector_size != image->sector_size_code) {
		fdd_img_log_detail("[FDD_IMG] Sector size mismatch req=%d image=%u\r\n",
			sector_size, (unsigned)image->sector_size_code);
		return -1;
	}

	bytes_per_sector = 128U << image->sector_size_code;
	track_bytes = (uint32_t)image->sectors * bytes_per_sector;
	pos = (((uint32_t)track * (uint32_t)image->sides) + (uint32_t)side) * track_bytes;
	pos += (uint32_t)(sector - 1) * bytes_per_sector;

	if ((pos + bytes_per_sector) > image->size) {
		fdd_img_log_detail("[FDD_IMG] Offset beyond image pos=%lu bytes=%lu size=%lu\r\n",
			(unsigned long)pos, (unsigned long)bytes_per_sector, (unsigned long)image->size);
		return -1;
	}

	*offset = pos;
	*sector_bytes = bytes_per_sector;
	return 0;
}

int fdd_img_load(FDD_IMAGE_t* image, const char* path, int check_bpb, int* write_protected)
{
	FILE* fp;
	uint8_t first_byte;
	uint16_t bpb_bps;
	uint16_t bpb_total16;
	uint32_t bpb_total32;
	uint8_t bpb_sectors;
	uint8_t bpb_sides;
	long size;

	if ((image == NULL) || (path == NULL) || (*path == 0) || (write_protected == NULL)) {
		return -1;
	}

	memset(image, 0, sizeof(*image));
	*write_protected = 0;

	fp = fopen(path, "r+b");
	if (fp == NULL) {
		fp = fopen(path, "rb");
		if (fp == NULL) {
			return -1;
		}
		*write_protected = 1;
	}
	fdd_img_log_detail("[FDD_IMG] Open path=%s writeprot=%u\r\n", path, (unsigned)*write_protected);

	if (fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}

	first_byte = (uint8_t)fgetc(fp);

	if (fseek(fp, 0x0B, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}
	{
		uint8_t raw[2];
		if (fread(raw, sizeof(raw), 1, fp) != 1) {
			fclose(fp);
			return -1;
		}
		bpb_bps = host_read_le16_unaligned(raw);
	}

	if (fseek(fp, 0x13, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}
	{
		uint8_t raw[2];
		if (fread(raw, sizeof(raw), 1, fp) != 1) {
			fclose(fp);
			return -1;
		}
		bpb_total16 = host_read_le16_unaligned(raw);
	}

	if (fseek(fp, 0x20, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}
	{
		uint8_t raw[4];
		if (fread(raw, sizeof(raw), 1, fp) != 1) {
			fclose(fp);
			return -1;
		}
		bpb_total32 = host_read_le32_unaligned(raw);
	}

	if (fseek(fp, 0x18, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}
	bpb_sectors = (uint8_t)fgetc(fp);

	if (fseek(fp, 0x1A, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}
	bpb_sides = (uint8_t)fgetc(fp);

	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	size = ftell(fp);
	if (size < 0) {
		fclose(fp);
		return -1;
	}

	image->fp = fp;
	image->size = (uint32_t)size;

	if (fdd_img_guess_geometry(image,
		image->size,
		bpb_bps,
		(bpb_total16 != 0) ? (uint32_t)bpb_total16 : bpb_total32,
		bpb_sectors,
		bpb_sides,
		first_byte,
		check_bpb) != 0) {
		fdd_img_close(image);
		return -1;
	}

	fdd_img_log_detail("[FDD_IMG] Loaded path=%s size=%lu tracks=%u sides=%u sectors=%u ssize=%u\r\n",
		path,
		(unsigned long)image->size,
		(unsigned)image->tracks,
		(unsigned)image->sides,
		(unsigned)image->sectors,
		(unsigned)(128U << image->sector_size_code));

	if (fseek(image->fp, 0, SEEK_SET) != 0) {
		fdd_img_close(image);
		return -1;
	}

	return 0;
}

void fdd_img_close(FDD_IMAGE_t* image)
{
	if (image == NULL) {
		return;
	}

	fdd_img_log_detail("[FDD_IMG] Close image\r\n");

	if (image->fp != NULL) {
		fclose(image->fp);
		image->fp = NULL;
	}

	memset(image, 0, sizeof(*image));
}

int fdd_img_read_sector(FDD_IMAGE_t* image, int sector, int track, int side, int sector_size, uint8_t* buf, sector_id_t* id)
{
	uint32_t offset;
	uint32_t sector_bytes;

	if ((buf == NULL) || (id == NULL)) {
		return -1;
	}

	if (fdd_img_sector_offset(image, sector, track, side, sector_size, &offset, &sector_bytes) != 0) {
		return -1;
	}

	if (fseek(image->fp, (long)offset, SEEK_SET) != 0) {
		return -1;
	}
	if (fread(buf, 1, sector_bytes, image->fp) != sector_bytes) {
		return -1;
	}

	id->id.c = (uint8_t)track;
	id->id.h = (uint8_t)side;
	id->id.r = (uint8_t)sector;
	id->id.n = (uint8_t)sector_size;
	return 0;
}

int fdd_img_write_sector(FDD_IMAGE_t* image, int sector, int track, int side, int sector_size, const uint8_t* buf, sector_id_t* id, int write_protected)
{
	uint32_t offset;
	uint32_t sector_bytes;

	if ((buf == NULL) || (id == NULL) || write_protected) {
		return -1;
	}

	if (fdd_img_sector_offset(image, sector, track, side, sector_size, &offset, &sector_bytes) != 0) {
		return -1;
	}

	if (fseek(image->fp, (long)offset, SEEK_SET) != 0) {
		return -1;
	}
	if (fwrite(buf, 1, sector_bytes, image->fp) != sector_bytes) {
		return -1;
	}
	fflush(image->fp);

	id->id.c = (uint8_t)track;
	id->id.h = (uint8_t)side;
	id->id.r = (uint8_t)sector;
	id->id.n = (uint8_t)sector_size;
	return 0;
}

int fdd_img_read_address(FDD_IMAGE_t* image, int track, int side, int index, sector_id_t* id)
{
	int sector;

	if ((image == NULL) || (id == NULL)) {
		return -1;
	}

	if ((track < 0) || (track >= image->tracks) || (side < 0) || (side >= image->sides)) {
		return -1;
	}

	if (image->sectors <= 0) {
		return -1;
	}

	sector = (index % image->sectors) + 1;
	id->id.c = (uint8_t)track;
	id->id.h = (uint8_t)side;
	id->id.r = (uint8_t)sector;
	id->id.n = image->sector_size_code;
	return 0;
}

int fdd_img_format_track(FDD_IMAGE_t* image, int track, int side, int sector_size, int sectors, uint8_t fill, const uint8_t* ids, sector_id_t* last_id, int write_protected)
{
	uint8_t sector_buf[8192];
	uint32_t sector_bytes;
	int index;
	sector_id_t id;

	if ((image == NULL) || (ids == NULL) || (last_id == NULL) || write_protected) {
		return -1;
	}

	if ((sector_size < 0) || (sector_size > 7)) {
		return -1;
	}

	sector_bytes = 128U << sector_size;
	if (sector_bytes > sizeof(sector_buf)) {
		return -1;
	}

	memset(sector_buf, fill, sector_bytes);

	for (index = 0; index < sectors; index++) {
		id.id.c = ids[(index * 4) + 0];
		id.id.h = ids[(index * 4) + 1];
		id.id.r = ids[(index * 4) + 2];
		id.id.n = ids[(index * 4) + 3];

		if ((id.id.n != sector_size) || (id.id.c != track) || (id.id.h != side)) {
			return -1;
		}

		if (fdd_img_write_sector(image, id.id.r, id.id.c, id.id.h, id.id.n, sector_buf, &id, write_protected) != 0) {
			return -1;
		}
	}

	*last_id = id;
	return 0;
}
