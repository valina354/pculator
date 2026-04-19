/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of the floppy drive emulation.
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
#include <stdlib.h>
#include <string.h>
#include "../config.h"
#include "../debuglog.h"
#include "fdd.h"
#include "fdd_img.h"

#define FDD_FILENAME_LEN 512

#ifdef DEBUG_FDC
#define fdd_log_detail(...) debug_log(DEBUG_DETAIL, __VA_ARGS__)
#else
#define fdd_log_detail(...) do { } while (0)
#endif

typedef struct FDD_DRIVE_s {
	int type;
	int track;
	int head;
	int motor_enable;
	int check_bpb;
	int write_protected;
	int inserted;
	int changed;
	char filename[FDD_FILENAME_LEN];
	FDD_IMAGE_t image;
} FDD_DRIVE_t;

static FDD_DRIVE_t fdd_drives[FDD_NUM];

static const struct {
	int max_track;
	int flags;
} fdd_drive_types[] = {
	{ 0, 0 },
	{ 43, FDD_FLAG_RPM_300 | FDD_FLAG_525 | FDD_FLAG_HOLE0 },
	{ 43, FDD_FLAG_RPM_300 | FDD_FLAG_525 | FDD_FLAG_DS | FDD_FLAG_HOLE0 },
	{ 86, FDD_FLAG_RPM_300 | FDD_FLAG_525 | FDD_FLAG_DS | FDD_FLAG_HOLE0 | FDD_FLAG_DOUBLE_STEP },
	{ 86, FDD_FLAG_RPM_360 | FDD_FLAG_525 | FDD_FLAG_DS | FDD_FLAG_HOLE0 | FDD_FLAG_HOLE1 | FDD_FLAG_DOUBLE_STEP | FDD_FLAG_PS2 },
	{ 86, FDD_FLAG_RPM_300 | FDD_FLAG_RPM_360 | FDD_FLAG_525 | FDD_FLAG_DS | FDD_FLAG_HOLE0 | FDD_FLAG_HOLE1 | FDD_FLAG_DOUBLE_STEP },
	{ 86, FDD_FLAG_RPM_300 | FDD_FLAG_HOLE0 | FDD_FLAG_DOUBLE_STEP },
	{ 86, FDD_FLAG_RPM_300 | FDD_FLAG_DS | FDD_FLAG_HOLE0 | FDD_FLAG_DOUBLE_STEP },
	{ 86, FDD_FLAG_RPM_300 | FDD_FLAG_DS | FDD_FLAG_HOLE0 | FDD_FLAG_HOLE1 | FDD_FLAG_DOUBLE_STEP | FDD_FLAG_PS2 },
	{ 86, FDD_FLAG_RPM_300 | FDD_FLAG_RPM_360 | FDD_FLAG_DS | FDD_FLAG_HOLE0 | FDD_FLAG_HOLE1 | FDD_FLAG_DOUBLE_STEP | FDD_FLAG_INVERT_DENSEL },
	{ 86, FDD_FLAG_RPM_300 | FDD_FLAG_RPM_360 | FDD_FLAG_DS | FDD_FLAG_HOLE0 | FDD_FLAG_HOLE1 | FDD_FLAG_DOUBLE_STEP },
	{ 86, FDD_FLAG_RPM_300 | FDD_FLAG_DS | FDD_FLAG_HOLE0 | FDD_FLAG_HOLE1 | FDD_FLAG_HOLE2 | FDD_FLAG_DOUBLE_STEP },
	{ 86, FDD_FLAG_RPM_300 | FDD_FLAG_RPM_360 | FDD_FLAG_DS | FDD_FLAG_HOLE0 | FDD_FLAG_HOLE1 | FDD_FLAG_HOLE2 | FDD_FLAG_DOUBLE_STEP }
};

static FDD_DRIVE_t* fdd_get_drive(int drive)
{
	if ((drive < 0) || (drive >= FDD_NUM)) {
		return NULL;
	}

	return &fdd_drives[drive];
}

void fdd_init(void)
{
	int drive;

	for (drive = 0; drive < FDD_NUM; drive++) {
		fdd_img_close(&fdd_drives[drive].image);
	}
	memset(fdd_drives, 0, sizeof(fdd_drives));
	for (drive = 0; drive < FDD_NUM; drive++) {
		fdd_drives[drive].check_bpb = 1;
	}
}

void fdd_reset(void)
{
	int drive;

	for (drive = 0; drive < FDD_NUM; drive++) {
		fdd_drives[drive].track = 0;
		fdd_drives[drive].head = 0;
		fdd_drives[drive].motor_enable = 0;
	}
}

void fdd_set_type(int drive, int type)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if (fdd == NULL) {
		return;
	}

	if ((type < FDD_TYPE_NONE) || (type > FDD_TYPE_35_2ED_DUALRPM)) {
		type = FDD_TYPE_NONE;
	}

	fdd->type = type;
	if (fdd->track >= fdd_get_type_max_track(type)) {
		fdd->track = 0;
	}
}

int fdd_get_type(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->type : FDD_TYPE_NONE;
}

int fdd_get_type_max_track(int type)
{
	if ((type < FDD_TYPE_NONE) || (type > FDD_TYPE_35_2ED_DUALRPM)) {
		return 0;
	}

	return fdd_drive_types[type].max_track;
}

int fdd_get_flags(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if (fdd == NULL) {
		return 0;
	}

	return fdd_drive_types[fdd->type].flags;
}

void fdd_set_motor_enable(int drive, int motor_enable)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if (fdd == NULL) {
		return;
	}

	fdd->motor_enable = motor_enable ? 1 : 0;
}

int fdd_get_motor_enable(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->motor_enable : 0;
}

void fdd_do_seek(int drive, int track)
{
	FDD_DRIVE_t* fdd;
	int max_track;
	int old_track;

	fdd = fdd_get_drive(drive);
	if (fdd == NULL) {
		return;
	}

	old_track = fdd->track;
	max_track = fdd_get_type_max_track(fdd->type);
	if (track < 0) {
		track = 0;
	}
	if ((max_track > 0) && (track >= max_track)) {
		track = max_track - 1;
	}
	fdd->track = track;
	if (track != old_track) {
		fdd->changed = 0;
		fdd_log_detail("[FDD] Seek drive=%d old=%d new=%d changed=0\r\n", drive, old_track, track);
	}
}

void fdd_seek(int drive, int track_diff)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if (fdd == NULL) {
		return;
	}

	fdd_do_seek(drive, fdd->track + track_diff);
}

int fdd_track0(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? (fdd->track == 0) : 1;
}

int fdd_is_525(int drive)
{
	return (fdd_get_flags(drive) & FDD_FLAG_525) ? 1 : 0;
}

int fdd_is_dd(int drive)
{
	return (fdd_get_flags(drive) & FDD_FLAG_HOLE1) ? 0 : 1;
}

int fdd_is_hd(int drive)
{
	return (fdd_get_flags(drive) & FDD_FLAG_HOLE1) ? 1 : 0;
}

int fdd_is_ed(int drive)
{
	return (fdd_get_flags(drive) & FDD_FLAG_HOLE2) ? 1 : 0;
}

int fdd_is_double_sided(int drive)
{
	return (fdd_get_flags(drive) & FDD_FLAG_DS) ? 1 : 0;
}

void fdd_set_head(int drive, int head)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if (fdd == NULL) {
		return;
	}

	fdd->head = head ? 1 : 0;
}

int fdd_get_head(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->head : 0;
}

int fdd_current_track(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->track : 0;
}

void fdd_set_check_bpb(int drive, int check_bpb)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if (fdd == NULL) {
		return;
	}

	fdd->check_bpb = check_bpb ? 1 : 0;
}

int fdd_get_check_bpb(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->check_bpb : 0;
}

int fdd_load(int drive, const char* fn)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if (fdd == NULL) {
		return FDD_RESULT_NO_MEDIA;
	}

	fdd_close(drive);

	if ((fn == NULL) || (*fn == 0)) {
		fdd_log_detail("[FDD] Load rejected drive=%d empty path\r\n", drive);
		return FDD_RESULT_NO_MEDIA;
	}

	if (fdd_img_load(&fdd->image, fn, fdd->check_bpb, &fdd->write_protected) != 0) {
		fdd_log_detail("[FDD] Load failed drive=%d path=%s\r\n", drive, fn);
		return FDD_RESULT_IOERR;
	}

	strncpy(fdd->filename, fn, sizeof(fdd->filename) - 1);
	fdd->filename[sizeof(fdd->filename) - 1] = 0;
	fdd->inserted = 1;
	fdd->changed = 1;
	fdd_log_detail("[FDD] Loaded drive=%d path=%s tracks=%u sides=%u sectors=%u ssize=%u writeprot=%u\r\n",
		drive,
		fn,
		(unsigned)fdd->image.tracks,
		(unsigned)fdd->image.sides,
		(unsigned)fdd->image.sectors,
		(unsigned)(128U << fdd->image.sector_size_code),
		(unsigned)fdd->write_protected);
	return 0;
}

void fdd_close(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if (fdd == NULL) {
		return;
	}

	fdd_img_close(&fdd->image);
	fdd->write_protected = 0;
	fdd->inserted = 0;
	fdd->changed = 1;
	fdd->filename[0] = 0;
	fdd_log_detail("[FDD] Closed drive=%d\r\n", drive);
}

const char* fdd_get_filename(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if ((fdd == NULL) || !fdd->inserted || (fdd->filename[0] == 0)) {
		return NULL;
	}

	return fdd->filename;
}

int fdd_is_inserted(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->inserted : 0;
}

int fdd_is_write_protected(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->write_protected : 0;
}

int fdd_get_disk_changed(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->changed : 1;
}

void fdd_set_disk_changed(int drive, int changed)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if (fdd == NULL) {
		return;
	}

	fdd->changed = changed ? 1 : 0;
}

int fdd_get_sectors(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->image.sectors : 0;
}

int fdd_get_tracks(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->image.tracks : 0;
}

int fdd_get_sides(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->image.sides : 0;
}

int fdd_get_sector_size_code(int drive)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	return (fdd != NULL) ? fdd->image.sector_size_code : 2;
}

int fdd_read_sector(int drive, int sector, int track, int side, int density, int sector_size, uint8_t* buf, sector_id_t* id)
{
	FDD_DRIVE_t* fdd;

	(void)density;

	fdd = fdd_get_drive(drive);
	if ((fdd == NULL) || !fdd->inserted) {
		return FDD_RESULT_NO_MEDIA;
	}

	if (fdd_img_read_sector(&fdd->image, sector, track, side, sector_size, buf, id) != 0) {
		return FDD_RESULT_NO_SECTOR;
	}

	fdd->changed = 0;
	return 0;
}

int fdd_write_sector(int drive, int sector, int track, int side, int density, int sector_size, const uint8_t* buf, sector_id_t* id)
{
	FDD_DRIVE_t* fdd;

	(void)density;

	fdd = fdd_get_drive(drive);
	if ((fdd == NULL) || !fdd->inserted) {
		return FDD_RESULT_NO_MEDIA;
	}

	if (fdd->write_protected) {
		return FDD_RESULT_WRITE_PROTECTED;
	}

	if (fdd_img_write_sector(&fdd->image, sector, track, side, sector_size, buf, id, fdd->write_protected) != 0) {
		return FDD_RESULT_NO_SECTOR;
	}

	fdd->changed = 0;
	return 0;
}

int fdd_read_address(int drive, int track, int side, int index, sector_id_t* id)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if ((fdd == NULL) || !fdd->inserted) {
		return FDD_RESULT_NO_MEDIA;
	}

	if (fdd_img_read_address(&fdd->image, track, side, index, id) != 0) {
		return FDD_RESULT_NO_SECTOR;
	}

	fdd->changed = 0;
	return 0;
}

int fdd_format_track(int drive, int track, int side, int sector_size, int sectors, uint8_t fill, const uint8_t* ids, sector_id_t* last_id)
{
	FDD_DRIVE_t* fdd;

	fdd = fdd_get_drive(drive);
	if ((fdd == NULL) || !fdd->inserted) {
		return FDD_RESULT_NO_MEDIA;
	}

	if (fdd->write_protected) {
		return FDD_RESULT_WRITE_PROTECTED;
	}

	if (fdd_img_format_track(&fdd->image, track, side, sector_size, sectors, fill, ids, last_id, fdd->write_protected) != 0) {
		return FDD_RESULT_NO_SECTOR;
	}

	fdd->changed = 0;
	return 0;
}
