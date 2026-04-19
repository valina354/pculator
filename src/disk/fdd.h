/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for the floppy drive emulation.
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          Miran Grca, <mgrca8@gmail.com>
 *          Fred N. van Kempen, <decwiz@yahoo.com>
 *          Toni Riikonen, <riikonen.toni@gmail.com>
 *
 *          Copyright 2008-2025 Sarah Walker.
 *          Copyright 2016-2025 Miran Grca.
 *          Copyright 2018-2025 Fred N. van Kempen.
 *          Copyright 2025 Toni Riikonen.
 *
 * Adapted for PCulator as a lean raw-image floppy backend.
 */
#ifndef _FDD_H_
#define _FDD_H_

#include <stdint.h>

#define FDD_NUM 4

#define FDD_FLAG_RPM_300       1
#define FDD_FLAG_RPM_360       2
#define FDD_FLAG_525           4
#define FDD_FLAG_DS            8
#define FDD_FLAG_HOLE0         16
#define FDD_FLAG_HOLE1         32
#define FDD_FLAG_HOLE2         64
#define FDD_FLAG_DOUBLE_STEP   128
#define FDD_FLAG_INVERT_DENSEL 256
#define FDD_FLAG_IGNORE_DENSEL 512
#define FDD_FLAG_PS2           1024

#define FDD_RESULT_OK               0
#define FDD_RESULT_NO_MEDIA        -1
#define FDD_RESULT_NO_SECTOR       -2
#define FDD_RESULT_WRITE_PROTECTED -3
#define FDD_RESULT_IOERR           -4

typedef enum {
	FDD_TYPE_NONE = 0,
	FDD_TYPE_525_1DD,
	FDD_TYPE_525_2DD,
	FDD_TYPE_525_2QD,
	FDD_TYPE_525_2HD,
	FDD_TYPE_525_2HD_DUALRPM,
	FDD_TYPE_35_1DD,
	FDD_TYPE_35_2DD,
	FDD_TYPE_35_2HD,
	FDD_TYPE_35_2HD_NEC,
	FDD_TYPE_35_2HD_3MODE,
	FDD_TYPE_35_2ED,
	FDD_TYPE_35_2ED_DUALRPM
} FDD_TYPE;

typedef struct sector_id_fields_t {
	uint8_t c;
	uint8_t h;
	uint8_t r;
	uint8_t n;
} sector_id_fields_t;

typedef union sector_id_t {
	uint32_t dword;
	uint8_t byte_array[4];
	sector_id_fields_t id;
} sector_id_t;

void fdd_init(void);
void fdd_reset(void);
void fdd_set_type(int drive, int type);
int fdd_get_type(int drive);
int fdd_get_type_max_track(int type);
int fdd_get_flags(int drive);
void fdd_set_motor_enable(int drive, int motor_enable);
int fdd_get_motor_enable(int drive);
void fdd_do_seek(int drive, int track);
void fdd_seek(int drive, int track_diff);
int fdd_track0(int drive);
int fdd_is_525(int drive);
int fdd_is_dd(int drive);
int fdd_is_hd(int drive);
int fdd_is_ed(int drive);
int fdd_is_double_sided(int drive);
void fdd_set_head(int drive, int head);
int fdd_get_head(int drive);
int fdd_current_track(int drive);
void fdd_set_check_bpb(int drive, int check_bpb);
int fdd_get_check_bpb(int drive);
int fdd_load(int drive, const char* fn);
void fdd_close(int drive);
const char* fdd_get_filename(int drive);
int fdd_is_inserted(int drive);
int fdd_is_write_protected(int drive);
int fdd_get_disk_changed(int drive);
void fdd_set_disk_changed(int drive, int changed);
int fdd_get_sectors(int drive);
int fdd_get_tracks(int drive);
int fdd_get_sides(int drive);
int fdd_get_sector_size_code(int drive);
int fdd_read_sector(int drive, int sector, int track, int side, int density, int sector_size, uint8_t* buf, sector_id_t* id);
int fdd_write_sector(int drive, int sector, int track, int side, int density, int sector_size, const uint8_t* buf, sector_id_t* id);
int fdd_read_address(int drive, int track, int side, int index, sector_id_t* id);
int fdd_format_track(int drive, int track, int side, int sector_size, int sectors, uint8_t fill, const uint8_t* ids, sector_id_t* last_id);

#endif
