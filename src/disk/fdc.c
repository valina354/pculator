/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of the NEC uPD-765 and compatible floppy disk
 *          controller.
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          Miran Grca, <mgrca8@gmail.com>
 *          Fred N. van Kempen, <decwiz@yahoo.com>
 *          Toni Riikonen, <riikonen.toni@gmail.com>
 *
 *          Copyright 2008-2020 Sarah Walker.
 *          Copyright 2016-2020 Miran Grca.
 *          Copyright 2018-2020 Fred N. van Kempen.
 *          Copyright 2025 Toni Riikonen.
 *
 * Adapted for PCulator as an AT-style wrapper-backed controller using a
 * lean raw-image backend.
 */
#include <stdlib.h>
#include <string.h>
#include "../config.h"
#include "../debuglog.h"
#include "../ports.h"
#include "../timing.h"
#include "../chipset/i8259.h"
#include "../chipset/dma.h"
#include "fdc.h"
#include "fdd.h"
#include "fifo.h"

#define FDC_PRIMARY_BASE 0x3F0
#define FDC_IRQ 6
#define FDC_DMA 2
#define FDC_RESULT_FIFO_LEN 16
#define FDC_TRANSFER_BUFFER_LEN 8192
#define FDC_SEEK_HZ 100.0
#define FDC_RW_HZ 500.0

#define FDC_CMD_SPECIFY 0x03
#define FDC_CMD_SENSE_DRIVE_STATUS 0x04
#define FDC_CMD_WRITE_DATA 0x05
#define FDC_CMD_READ_DATA 0x06
#define FDC_CMD_RECALIBRATE 0x07
#define FDC_CMD_SENSE_INTERRUPT 0x08
#define FDC_CMD_READ_ID 0x0A
#define FDC_CMD_FORMAT_TRACK 0x0D
#define FDC_CMD_SEEK 0x0F
#define FDC_CMD_WRITE_DELETED_DATA 0x09
#define FDC_CMD_READ_DELETED_DATA 0x0C

#ifdef DEBUG_FDC
#define fdc_log_detail(...) debug_log(DEBUG_DETAIL, __VA_ARGS__)
#else
#define fdc_log_detail(...) do { } while (0)
#endif
#define fdc_log_info(...) debug_log(DEBUG_INFO, __VA_ARGS__)

enum {
	FDC_PHASE_COMMAND = 0,
	FDC_PHASE_PARAMS,
	FDC_PHASE_RESULT,
	FDC_PHASE_PIO_READ,
	FDC_PHASE_PIO_WRITE,
	FDC_PHASE_BUSY
};

enum {
	FDC_OP_NONE = 0,
	FDC_OP_SEEK,
	FDC_OP_RECALIBRATE,
	FDC_OP_READ,
	FDC_OP_WRITE,
	FDC_OP_READ_ID,
	FDC_OP_FORMAT
};

enum {
	FDC_SEEK_REASON_NONE = 0,
	FDC_SEEK_REASON_COMMAND,
	FDC_SEEK_REASON_RECALIBRATE,
	FDC_SEEK_REASON_RW
};

typedef struct FDC_IMPL_s {
	CPU_t* cpu;
	I8259_t* i8259;
	DMA_t* dma;
	uint8_t irq;
	uint8_t dma_ch;
	uint8_t dor;
	uint8_t dsr;
	uint8_t ccr;
	uint8_t msr;
	uint8_t phase;
	uint8_t command;
	uint8_t params[16];
	uint8_t param_len;
	uint8_t params_needed;
	uint8_t selected_drive;
	uint8_t rw_drive;
	uint8_t current_head;
	uint8_t current_track;
	uint8_t current_sector;
	uint8_t current_size;
	uint8_t current_eot;
	uint8_t current_gap;
	uint8_t current_dtl;
	uint8_t current_mt;
	uint8_t use_dma;
	uint8_t op;
	uint8_t fintr;
	uint8_t st0;
	uint8_t reset_sense_count;
	uint8_t seek_pending;
	uint8_t seek_drive;
	uint8_t seek_target;
	uint8_t seek_reason;
	uint8_t read_id_index[FDD_NUM];
	uint8_t format_fill;
	uint8_t format_sector_count;
	uint16_t pcn[FDD_NUM];
	uint32_t timer_seek;
	uint32_t timer_rw;
	uint8_t transfer_buffer[FDC_TRANSFER_BUFFER_LEN];
	int transfer_len;
	int transfer_pos;
	sector_id_t last_id;
	void* result_fifo;
} FDC_IMPL_t;

static FDC_IMPL_t* fdc_impl(FDC_t* fdc)
{
	if (fdc == NULL) {
		return NULL;
	}

	return (FDC_IMPL_t*)fdc->impl;
}

static int fdc_command_params_needed(uint8_t command)
{
	switch (command & 0x1F) {
	case FDC_CMD_SPECIFY:
		return 2;
	case FDC_CMD_SENSE_DRIVE_STATUS:
		return 1;
	case FDC_CMD_WRITE_DATA:
	case FDC_CMD_READ_DATA:
	case FDC_CMD_WRITE_DELETED_DATA:
	case FDC_CMD_READ_DELETED_DATA:
		return 8;
	case FDC_CMD_RECALIBRATE:
		return 1;
	case FDC_CMD_SENSE_INTERRUPT:
		return 0;
	case FDC_CMD_READ_ID:
		return 1;
	case FDC_CMD_FORMAT_TRACK:
		return 5;
	case FDC_CMD_SEEK:
		return 2;
	default:
		break;
	}

	return -1;
}

static const char* fdc_command_name(uint8_t command)
{
	switch (command & 0x1F) {
	case FDC_CMD_SPECIFY:
		return "SPECIFY";
	case FDC_CMD_SENSE_DRIVE_STATUS:
		return "SENSE_DRIVE_STATUS";
	case FDC_CMD_WRITE_DATA:
		return "WRITE_DATA";
	case FDC_CMD_READ_DATA:
		return "READ_DATA";
	case FDC_CMD_RECALIBRATE:
		return "RECALIBRATE";
	case FDC_CMD_SENSE_INTERRUPT:
		return "SENSE_INTERRUPT";
	case FDC_CMD_READ_ID:
		return "READ_ID";
	case FDC_CMD_FORMAT_TRACK:
		return "FORMAT_TRACK";
	case FDC_CMD_SEEK:
		return "SEEK";
	case FDC_CMD_WRITE_DELETED_DATA:
		return "WRITE_DELETED_DATA";
	case FDC_CMD_READ_DELETED_DATA:
		return "READ_DELETED_DATA";
	default:
		break;
	}

	return "UNKNOWN";
}

static int fdc_bytes_for_size(uint8_t size_code, uint8_t dtl)
{
	if ((size_code == 0) && (dtl != 0)) {
		return dtl;
	}
	if (size_code > 7) {
		return 0;
	}

	return 128 << size_code;
}

static void fdc_set_phase(FDC_IMPL_t* fdc, uint8_t phase, uint8_t msr)
{
	fdc->phase = phase;
	fdc->msr = msr;
}

static void fdc_result_clear(FDC_IMPL_t* fdc)
{
	if (fdc->result_fifo != NULL) {
		fifo_reset(fdc->result_fifo);
	}
}

static void fdc_result_push(FDC_IMPL_t* fdc, uint8_t value)
{
	if (fdc->result_fifo != NULL) {
		fifo_write(value, fdc->result_fifo);
	}
}

static void fdc_enter_command_phase(FDC_IMPL_t* fdc)
{
	fdc->command = 0;
	fdc->param_len = 0;
	fdc->params_needed = 0;
	fdc->transfer_len = 0;
	fdc->transfer_pos = 0;
	fdc_result_clear(fdc);
	if ((fdc->dor & 0x04) != 0) {
		fdc_set_phase(fdc, FDC_PHASE_COMMAND, 0x80);
	}
	else {
		fdc_set_phase(fdc, FDC_PHASE_BUSY, 0x00);
	}
}

static void fdc_raise_irq(FDC_IMPL_t* fdc)
{
	i8259_doirq(fdc->i8259, fdc->irq);
}

static void fdc_clear_irq(FDC_IMPL_t* fdc)
{
	i8259_clearirq(fdc->i8259, fdc->irq);
}

static void fdc_begin_result_phase(FDC_IMPL_t* fdc, int raise_irq)
{
	fdc_set_phase(fdc, FDC_PHASE_RESULT, 0xD0);
	if (raise_irq) {
		fdc_raise_irq(fdc);
	}
}

static void fdc_queue_result7(FDC_IMPL_t* fdc, uint8_t st0, uint8_t st1, uint8_t st2, const sector_id_t* id)
{
	fdc_log_detail("[FDC] Result ST0=%02X ST1=%02X ST2=%02X C=%u H=%u R=%u N=%u\r\n",
		st0, st1, st2, id->id.c, id->id.h, id->id.r, id->id.n);
	fdc_result_clear(fdc);
	fdc_result_push(fdc, st0);
	fdc_result_push(fdc, st1);
	fdc_result_push(fdc, st2);
	fdc_result_push(fdc, id->id.c);
	fdc_result_push(fdc, id->id.h);
	fdc_result_push(fdc, id->id.r);
	fdc_result_push(fdc, id->id.n);
	fdc_begin_result_phase(fdc, 1);
}

static void fdc_abort(FDC_IMPL_t* fdc)
{
	fdc_log_detail("[FDC] Abort controller op=%u phase=%u\r\n", fdc->op, fdc->phase);
	timing_timerDisable(fdc->timer_seek);
	timing_timerDisable(fdc->timer_rw);
	fdc->op = FDC_OP_NONE;
	fdc->seek_pending = 0;
	fdc->transfer_len = 0;
	fdc->transfer_pos = 0;
	fdc_enter_command_phase(fdc);
}

static uint8_t fdc_status3(FDC_IMPL_t* fdc, uint8_t drive, uint8_t head)
{
	uint8_t status;

	status = (drive & 0x03) | ((head & 0x01) << 2);
	if (fdd_is_double_sided(drive)) {
		status |= 0x08;
	}
	if (fdd_track0(drive)) {
		status |= 0x10;
	}
	if (fdd_is_inserted(drive)) {
		status |= 0x20;
	}
	if (fdd_is_write_protected(drive)) {
		status |= 0x40;
	}
	return status;
}

static int fdc_drive_motor_enabled(FDC_IMPL_t* fdc, uint8_t drive)
{
	return ((fdc->dor & (uint8_t)(0x10U << drive)) != 0) ? 1 : 0;
}

static void fdc_finish_seek_interrupt(FDC_IMPL_t* fdc, uint8_t drive, uint8_t st0)
{
	fdc_log_detail("[FDC] Seek complete drive=%u track=%u head=%u st0=%02X\r\n",
		drive, (unsigned)fdc->pcn[drive], fdd_get_head(drive), st0);
	fdc->selected_drive = drive;
	fdc->st0 = st0;
	fdc->fintr = 1;
	fdc_raise_irq(fdc);
	fdc_enter_command_phase(fdc);
}

static void fdc_error_rw(FDC_IMPL_t* fdc, int error_code)
{
	uint8_t st0;
	uint8_t st1;
	uint8_t st2;

	st0 = 0x40 | (fdc->rw_drive & 0x03) | ((fdc->current_head & 0x01) << 2);
	st1 = 0;
	st2 = 0;

	fdc_log_detail("[FDC] RW error drive=%u track=%u head=%u sector=%u size=%u err=%d\r\n",
		fdc->rw_drive,
		fdc->current_track,
		fdc->current_head,
		fdc->current_sector,
		fdc->current_size,
		error_code);
	if (error_code == FDD_RESULT_NO_MEDIA) {
		st0 |= 0x08;
	}
	else if (error_code == FDD_RESULT_WRITE_PROTECTED) {
		st1 = 0x02;
	}
	else {
		st1 = 0x04;
	}

	timing_timerDisable(fdc->timer_rw);
	fdc->op = FDC_OP_NONE;
	fdc->last_id.id.c = fdc->current_track;
	fdc->last_id.id.h = fdc->current_head;
	fdc->last_id.id.r = fdc->current_sector;
	fdc->last_id.id.n = fdc->current_size;
	fdc_queue_result7(fdc, st0, st1, st2, &fdc->last_id);
}

static void fdc_success_rw(FDC_IMPL_t* fdc)
{
	uint8_t st0;

	fdc_log_detail("[FDC] RW success drive=%u C=%u H=%u R=%u N=%u\r\n",
		fdc->rw_drive,
		fdc->last_id.id.c,
		fdc->last_id.id.h,
		fdc->last_id.id.r,
		fdc->last_id.id.n);
	timing_timerDisable(fdc->timer_rw);
	fdc->op = FDC_OP_NONE;
	st0 = (fdc->rw_drive & 0x03) | ((fdc->last_id.id.h & 0x01) << 2);
	fdc_queue_result7(fdc, st0, 0x00, 0x00, &fdc->last_id);
}

static int fdc_advance_rw(FDC_IMPL_t* fdc)
{
	if ((fdc->current_eot == 0) || (fdc->current_sector < fdc->current_eot)) {
		fdc->current_sector++;
		return 1;
	}

	if (fdc->current_mt && fdd_is_double_sided(fdc->rw_drive)) {
		if (fdc->current_head == 0) {
			fdc->current_head = 1;
			fdd_set_head(fdc->rw_drive, 1);
			fdc->current_sector = 1;
			return 1;
		}

		fdc->current_head = 0;
		fdc->current_track++;
		fdc->pcn[fdc->rw_drive] = fdc->current_track;
		fdd_set_head(fdc->rw_drive, 0);
		fdd_do_seek(fdc->rw_drive, fdc->current_track);
		fdc->current_sector = 1;
		return 1;
	}

	return 0;
}

static int fdc_dma_transfer_complete(FDC_IMPL_t* fdc)
{
	int remaining;

	if (!fdc->use_dma) {
		return 0;
	}

	remaining = dma_channel_remaining(fdc->dma, fdc->dma_ch);
	if (remaining > 0) {
		return 0;
	}

	fdc_log_detail("[FDC] DMA terminal count drive=%u C=%u H=%u R=%u N=%u\r\n",
		fdc->rw_drive,
		fdc->last_id.id.c,
		fdc->last_id.id.h,
		fdc->last_id.id.r,
		fdc->last_id.id.n);
	return 1;
}

static void fdc_continue_after_sector(FDC_IMPL_t* fdc)
{
	if (fdc_dma_transfer_complete(fdc)) {
		fdc_success_rw(fdc);
		return;
	}

	if (fdc_advance_rw(fdc)) {
		fdc_set_phase(fdc, FDC_PHASE_BUSY, (fdc->op == FDC_OP_READ) ? 0x50 : 0x10);
		timing_timerEnable(fdc->timer_rw);
	}
	else {
		fdc_success_rw(fdc);
	}
}

static void fdc_finish_pio_write(FDC_IMPL_t* fdc)
{
	int error;

	if (fdc->op == FDC_OP_WRITE) {
		error = fdd_write_sector(fdc->rw_drive,
			fdc->current_sector,
			fdc->current_track,
			fdc->current_head,
			fdc->ccr & 0x03,
			fdc->current_size,
			fdc->transfer_buffer,
			&fdc->last_id);
		if (error != FDD_RESULT_OK) {
			fdc_error_rw(fdc, error);
			return;
		}
		fdc_continue_after_sector(fdc);
		return;
	}

	if (fdc->op == FDC_OP_FORMAT) {
		error = fdd_format_track(fdc->rw_drive,
			fdc->current_track,
			fdc->current_head,
			fdc->current_size,
			fdc->format_sector_count,
			fdc->format_fill,
			fdc->transfer_buffer,
			&fdc->last_id);
		if (error == FDD_RESULT_WRITE_PROTECTED) {
			fdc_error_rw(fdc, error);
			return;
		}
		if (error != FDD_RESULT_OK) {
			fdc_error_rw(fdc, FDD_RESULT_NO_SECTOR);
			return;
		}
		timing_timerDisable(fdc->timer_rw);
		fdc->op = FDC_OP_NONE;
		fdc_queue_result7(fdc, (fdc->rw_drive & 0x03) | ((fdc->last_id.id.h & 0x01) << 2), 0x00, 0x00, &fdc->last_id);
	}
}

static void fdc_rw_callback(void* opaque)
{
	FDC_IMPL_t* fdc;
	int bytes;
	int i;
	int error;
	int read_id_index;

	fdc = (FDC_IMPL_t*)opaque;
	if (fdc == NULL) {
		return;
	}

	timing_timerDisable(fdc->timer_rw);

	switch (fdc->op) {
	case FDC_OP_READ:
		fdc_log_detail("[FDC] Read sector drive=%u track=%u head=%u sector=%u size=%u dma=%u\r\n",
			fdc->rw_drive, fdc->current_track, fdc->current_head, fdc->current_sector, fdc->current_size, fdc->use_dma);
		error = fdd_read_sector(fdc->rw_drive,
			fdc->current_sector,
			fdc->current_track,
			fdc->current_head,
			fdc->ccr & 0x03,
			fdc->current_size,
			fdc->transfer_buffer,
			&fdc->last_id);
		if (error != FDD_RESULT_OK) {
			fdc_error_rw(fdc, error);
			return;
		}

		bytes = fdc_bytes_for_size(fdc->current_size, fdc->current_dtl);
		if ((bytes <= 0) || (bytes > FDC_TRANSFER_BUFFER_LEN)) {
			fdc_error_rw(fdc, FDD_RESULT_NO_SECTOR);
			return;
		}

		if (fdc->use_dma) {
			for (i = 0; i < bytes; i++) {
				dma_channel_write(fdc->dma, fdc->dma_ch, fdc->transfer_buffer[i]);
			}
			fdc_continue_after_sector(fdc);
		}
		else {
			fdc->transfer_len = bytes;
			fdc->transfer_pos = 0;
			fdc_set_phase(fdc, FDC_PHASE_PIO_READ, 0xF0);
			fdc_raise_irq(fdc);
		}
		break;

	case FDC_OP_WRITE:
		fdc_log_detail("[FDC] Write sector drive=%u track=%u head=%u sector=%u size=%u dma=%u\r\n",
			fdc->rw_drive, fdc->current_track, fdc->current_head, fdc->current_sector, fdc->current_size, fdc->use_dma);
		bytes = fdc_bytes_for_size(fdc->current_size, fdc->current_dtl);
		if ((bytes <= 0) || (bytes > FDC_TRANSFER_BUFFER_LEN)) {
			fdc_error_rw(fdc, FDD_RESULT_NO_SECTOR);
			return;
		}

		if (fdc->use_dma) {
			for (i = 0; i < bytes; i++) {
				fdc->transfer_buffer[i] = dma_channel_read(fdc->dma, fdc->dma_ch);
			}

			error = fdd_write_sector(fdc->rw_drive,
				fdc->current_sector,
				fdc->current_track,
				fdc->current_head,
				fdc->ccr & 0x03,
				fdc->current_size,
				fdc->transfer_buffer,
				&fdc->last_id);
			if (error != FDD_RESULT_OK) {
				fdc_error_rw(fdc, error);
				return;
			}

			fdc_continue_after_sector(fdc);
		}
		else {
			fdc->transfer_len = bytes;
			fdc->transfer_pos = 0;
			fdc_set_phase(fdc, FDC_PHASE_PIO_WRITE, 0xB0);
		}
		break;

	case FDC_OP_READ_ID:
		fdc_log_detail("[FDC] Read ID drive=%u track=%u head=%u index=%u\r\n",
			fdc->rw_drive, (unsigned)fdc->pcn[fdc->rw_drive], fdc->current_head, fdc->read_id_index[fdc->rw_drive]);
		read_id_index = fdc->read_id_index[fdc->rw_drive];
		error = fdd_read_address(fdc->rw_drive, fdc->pcn[fdc->rw_drive], fdc->current_head, read_id_index, &fdc->last_id);
		if (error != FDD_RESULT_OK) {
			fdc_error_rw(fdc, error);
			return;
		}

		fdc->read_id_index[fdc->rw_drive] = (uint8_t)(read_id_index + 1);
		fdc->op = FDC_OP_NONE;
		fdc_queue_result7(fdc, (fdc->rw_drive & 0x03) | ((fdc->last_id.id.h & 0x01) << 2), 0x00, 0x00, &fdc->last_id);
		break;

	case FDC_OP_FORMAT:
		fdc_log_detail("[FDC] Format track drive=%u track=%u head=%u sectors=%u size=%u dma=%u fill=%02X\r\n",
			fdc->rw_drive, fdc->current_track, fdc->current_head, fdc->format_sector_count, fdc->current_size, fdc->use_dma, fdc->format_fill);
		bytes = fdc->format_sector_count * 4;
		if ((bytes <= 0) || (bytes > FDC_TRANSFER_BUFFER_LEN)) {
			fdc_error_rw(fdc, FDD_RESULT_NO_SECTOR);
			return;
		}

		if (fdc->use_dma) {
			for (i = 0; i < bytes; i++) {
				fdc->transfer_buffer[i] = dma_channel_read(fdc->dma, fdc->dma_ch);
			}

			error = fdd_format_track(fdc->rw_drive,
				fdc->current_track,
				fdc->current_head,
				fdc->current_size,
				fdc->format_sector_count,
				fdc->format_fill,
				fdc->transfer_buffer,
				&fdc->last_id);
			if (error != FDD_RESULT_OK) {
				fdc_error_rw(fdc, error);
				return;
			}

			fdc->op = FDC_OP_NONE;
			fdc_queue_result7(fdc, (fdc->rw_drive & 0x03) | ((fdc->last_id.id.h & 0x01) << 2), 0x00, 0x00, &fdc->last_id);
		}
		else {
			fdc->transfer_len = bytes;
			fdc->transfer_pos = 0;
			fdc_set_phase(fdc, FDC_PHASE_PIO_WRITE, 0xB0);
		}
		break;

	default:
		break;
	}
}

static void fdc_seek_callback(void* opaque)
{
	FDC_IMPL_t* fdc;
	int track;

	fdc = (FDC_IMPL_t*)opaque;
	if ((fdc == NULL) || !fdc->seek_pending) {
		return;
	}

	track = fdd_current_track(fdc->seek_drive);
	fdc_log_detail("[FDC] Seek step drive=%u current=%d target=%u reason=%u\r\n",
		fdc->seek_drive, track, fdc->seek_target, fdc->seek_reason);
	if (track < fdc->seek_target) {
		track++;
	}
	else if (track > fdc->seek_target) {
		track--;
	}

	fdd_do_seek(fdc->seek_drive, track);
	fdc->pcn[fdc->seek_drive] = (uint16_t)track;

	if (track != fdc->seek_target) {
		return;
	}

	fdc->seek_pending = 0;
	timing_timerDisable(fdc->timer_seek);

	if (fdc->seek_reason == FDC_SEEK_REASON_RW) {
		fdc_set_phase(fdc, FDC_PHASE_BUSY, (fdc->op == FDC_OP_READ) ? 0x50 : 0x10);
		timing_timerEnable(fdc->timer_rw);
	}
	else if (fdc->seek_reason == FDC_SEEK_REASON_RECALIBRATE) {
		fdc_finish_seek_interrupt(fdc, fdc->seek_drive, 0x20 | (fdc->seek_drive & 0x03));
	}
	else {
		fdc_finish_seek_interrupt(fdc, fdc->seek_drive, 0x20 | (fdc->seek_drive & 0x03) | ((fdd_get_head(fdc->seek_drive) & 0x01) << 2));
	}

	fdc->seek_reason = FDC_SEEK_REASON_NONE;
}

static void fdc_soft_reset(FDC_IMPL_t* fdc)
{
	int drive;

	fdc_log_detail("[FDC] Controller reset\r\n");
	timing_timerDisable(fdc->timer_seek);
	timing_timerDisable(fdc->timer_rw);
	fdc_result_clear(fdc);
	fdc->command = 0;
	fdc->param_len = 0;
	fdc->params_needed = 0;
	fdc->op = FDC_OP_NONE;
	fdc->seek_pending = 0;
	fdc->seek_reason = FDC_SEEK_REASON_NONE;
	fdc->transfer_len = 0;
	fdc->transfer_pos = 0;
	fdc->fintr = 0;
	fdc->st0 = 0;
	fdc->reset_sense_count = 4;
	fdc->selected_drive = fdc->dor & 0x03;
	fdc->rw_drive = fdc->selected_drive;
	fdc->use_dma = 1;
	fdd_reset();
	fdd_set_motor_enable(0, (fdc->dor & 0x10) ? 1 : 0);
	fdd_set_motor_enable(1, (fdc->dor & 0x20) ? 1 : 0);
	fdd_set_motor_enable(2, (fdc->dor & 0x40) ? 1 : 0);
	fdd_set_motor_enable(3, (fdc->dor & 0x80) ? 1 : 0);
	for (drive = 0; drive < FDD_NUM; drive++) {
		fdc->read_id_index[drive] = 0;
		fdc->pcn[drive] = 0;
	}
	fdc_enter_command_phase(fdc);
	fdc_raise_irq(fdc);
}

static void fdc_start_rw(FDC_IMPL_t* fdc, uint8_t op)
{
	int drive;
	int bytes;

	drive = fdc->params[0] & 0x03;
	fdc->rw_drive = (uint8_t)drive;
	fdc->current_track = fdc->params[1];
	fdc->current_head = fdc->params[2] & 0x01;
	fdc->current_sector = fdc->params[3];
	fdc->current_size = fdc->params[4];
	fdc->current_eot = fdc->params[5];
	fdc->current_gap = fdc->params[6];
	fdc->current_dtl = fdc->params[7];
	fdc->current_mt = (fdc->command & 0x80) ? 1 : 0;
	fdc->op = op;
	fdc_log_detail("[FDC] Start %s drive=%u C=%u H=%u R=%u N=%u EOT=%u MT=%u DMA=%u\r\n",
		(op == FDC_OP_READ) ? "READ" : "WRITE",
		drive,
		fdc->current_track,
		fdc->current_head,
		fdc->current_sector,
		fdc->current_size,
		fdc->current_eot,
		fdc->current_mt,
		fdc->use_dma);
	fdc->last_id.id.c = fdc->current_track;
	fdc->last_id.id.h = fdc->current_head;
	fdc->last_id.id.r = fdc->current_sector;
	fdc->last_id.id.n = fdc->current_size;

	bytes = fdc_bytes_for_size(fdc->current_size, fdc->current_dtl);
	if ((bytes <= 0) || (bytes > FDC_TRANSFER_BUFFER_LEN)) {
		fdc_error_rw(fdc, FDD_RESULT_NO_SECTOR);
		return;
	}

	if (!fdd_get_flags(drive) || !fdd_get_motor_enable(drive)) {
		fdc_error_rw(fdc, FDD_RESULT_NO_MEDIA);
		return;
	}

	fdd_set_head(drive, fdc->current_head);
	if (fdd_current_track(drive) != fdc->current_track) {
		fdc->seek_pending = 1;
		fdc->seek_drive = (uint8_t)drive;
		fdc->seek_target = fdc->current_track;
		fdc->seek_reason = FDC_SEEK_REASON_RW;
		fdc_set_phase(fdc, FDC_PHASE_BUSY, 0x10);
		timing_timerEnable(fdc->timer_seek);
		return;
	}

	fdc_set_phase(fdc, FDC_PHASE_BUSY, (op == FDC_OP_READ) ? 0x50 : 0x10);
	timing_timerEnable(fdc->timer_rw);
}

static void fdc_handle_command(FDC_IMPL_t* fdc)
{
	uint8_t drive;
	uint8_t result;

	fdc_log_detail("[FDC] Execute %s (%02X) params=%u\r\n",
		fdc_command_name(fdc->command), fdc->command, fdc->params_needed);
	switch (fdc->command & 0x1F) {
	case FDC_CMD_SPECIFY:
		fdc->use_dma = ((fdc->params[1] & 0x01) == 0) ? 1 : 0;
		fdc_enter_command_phase(fdc);
		break;

	case FDC_CMD_SENSE_DRIVE_STATUS:
		drive = fdc->params[0] & 0x03;
		fdd_set_head(drive, (fdc->params[0] & 0x04) ? 1 : 0);
		result = fdc_status3(fdc, drive, fdd_get_head(drive));
		fdc_result_clear(fdc);
		fdc_result_push(fdc, result);
		fdc_begin_result_phase(fdc, 0);
		break;

	case FDC_CMD_RECALIBRATE:
		drive = fdc->params[0] & 0x03;
		fdc->rw_drive = drive;
		if (!fdd_get_flags(drive) || !fdd_get_motor_enable(drive)) {
			fdc_finish_seek_interrupt(fdc, drive, 0x70 | drive);
			return;
		}
		fdc->seek_pending = 1;
		fdc->seek_drive = drive;
		fdc->seek_target = 0;
		fdc->seek_reason = FDC_SEEK_REASON_RECALIBRATE;
		fdc_set_phase(fdc, FDC_PHASE_BUSY, 0x10 | (1 << drive));
		timing_timerEnable(fdc->timer_seek);
		break;

	case FDC_CMD_SENSE_INTERRUPT:
		fdc_result_clear(fdc);
		if (fdc->reset_sense_count != 0) {
			drive = (uint8_t)(4 - fdc->reset_sense_count);
			fdc_log_detail("[FDC] Sense interrupt reset pending drive=%u pcn=%u\r\n", drive, (unsigned)fdc->pcn[drive]);
			fdc_result_push(fdc, 0xC0 | drive);
			fdc_result_push(fdc, (uint8_t)fdc->pcn[drive]);
			fdc->reset_sense_count--;
		}
		else if (fdc->fintr) {
			fdc_log_detail("[FDC] Sense interrupt fintr st0=%02X pcn=%u\r\n",
				fdc->st0, (unsigned)fdc->pcn[fdc->selected_drive & 0x03]);
			fdc_result_push(fdc, fdc->st0);
			fdc_result_push(fdc, (uint8_t)fdc->pcn[fdc->selected_drive & 0x03]);
			fdc->fintr = 0;
		}
		else {
			fdc_result_push(fdc, 0x80);
		}
		fdc_begin_result_phase(fdc, 0);
		break;

	case FDC_CMD_SEEK:
		drive = fdc->params[0] & 0x03;
		fdc->rw_drive = drive;
		fdd_set_head(drive, (fdc->params[0] & 0x04) ? 1 : 0);
		if (!fdd_get_flags(drive) || !fdd_get_motor_enable(drive)) {
			fdc_finish_seek_interrupt(fdc, drive, 0x20 | drive);
			return;
		}
		fdc->seek_pending = 1;
		fdc->seek_drive = drive;
		fdc->seek_target = fdc->params[1];
		fdc->seek_reason = FDC_SEEK_REASON_COMMAND;
		fdc_set_phase(fdc, FDC_PHASE_BUSY, 0x10 | (1 << drive));
		timing_timerEnable(fdc->timer_seek);
		break;

	case FDC_CMD_READ_DATA:
	case FDC_CMD_READ_DELETED_DATA:
		fdc_start_rw(fdc, FDC_OP_READ);
		break;

	case FDC_CMD_WRITE_DATA:
	case FDC_CMD_WRITE_DELETED_DATA:
		fdc_start_rw(fdc, FDC_OP_WRITE);
		break;

	case FDC_CMD_READ_ID:
		drive = fdc->params[0] & 0x03;
		fdc->rw_drive = drive;
		fdc->current_head = (fdc->params[0] & 0x04) ? 1 : 0;
		fdd_set_head(drive, fdc->current_head);
		if (!fdd_get_flags(drive) || !fdd_get_motor_enable(drive)) {
			fdc_error_rw(fdc, FDD_RESULT_NO_MEDIA);
			return;
		}
		fdc->op = FDC_OP_READ_ID;
		fdc_set_phase(fdc, FDC_PHASE_BUSY, 0x50);
		timing_timerEnable(fdc->timer_rw);
		break;

	case FDC_CMD_FORMAT_TRACK:
		drive = fdc->params[0] & 0x03;
		fdc->rw_drive = drive;
		fdc->current_head = (fdc->params[0] & 0x04) ? 1 : 0;
		fdc->current_track = (uint8_t)fdc->pcn[drive];
		fdc->current_size = fdc->params[1];
		fdc->format_sector_count = fdc->params[2];
		fdc->format_fill = fdc->params[4];
		fdd_set_head(drive, fdc->current_head);
		if (!fdd_get_flags(drive) || !fdd_get_motor_enable(drive)) {
			fdc_error_rw(fdc, FDD_RESULT_NO_MEDIA);
			return;
		}
		fdc->op = FDC_OP_FORMAT;
		fdc_set_phase(fdc, FDC_PHASE_BUSY, 0x10);
		timing_timerEnable(fdc->timer_rw);
		break;

	default:
		fdc_result_clear(fdc);
		fdc_result_push(fdc, 0x80);
		fdc_begin_result_phase(fdc, 0);
		break;
	}
}

static void fdc_port_write(void* opaque, uint16_t addr, uint8_t value)
{
	FDC_IMPL_t* fdc;
	uint8_t old_dor;
	int params_needed;

	fdc = (FDC_IMPL_t*)opaque;
	if (fdc == NULL) {
		return;
	}

	addr &= 7;

	switch (addr) {
	case 2:
		fdc_log_detail("[FDC] Write DOR=%02X\r\n", value);
		old_dor = fdc->dor;
		fdc->dor = value;
		fdc->selected_drive = value & 0x03;
		fdd_set_motor_enable(0, (value & 0x10) ? 1 : 0);
		fdd_set_motor_enable(1, (value & 0x20) ? 1 : 0);
		fdd_set_motor_enable(2, (value & 0x40) ? 1 : 0);
		fdd_set_motor_enable(3, (value & 0x80) ? 1 : 0);

		if ((value & 0x04) == 0) {
			timing_timerDisable(fdc->timer_seek);
			timing_timerDisable(fdc->timer_rw);
			fdc_set_phase(fdc, FDC_PHASE_BUSY, 0x00);
			return;
		}

		if (((old_dor & 0x04) == 0) && ((value & 0x04) != 0)) {
			fdc_soft_reset(fdc);
		}
		break;

	case 4:
		fdc_log_detail("[FDC] Write DSR=%02X\r\n", value);
		fdc->dsr = value;
		if ((value & 0x80) == 0) {
			fdc_soft_reset(fdc);
		}
		break;

	case 5:
		if ((fdc->dor & 0x04) == 0) {
			return;
		}

		if (fdc->phase == FDC_PHASE_PIO_WRITE) {
			if ((fdc->transfer_pos >= 0) && (fdc->transfer_pos < FDC_TRANSFER_BUFFER_LEN) && (fdc->transfer_pos < fdc->transfer_len)) {
				fdc->transfer_buffer[fdc->transfer_pos++] = value;
			}

			if (fdc->transfer_pos >= fdc->transfer_len) {
				fdc_finish_pio_write(fdc);
			}
			return;
		}

		if (fdc->phase == FDC_PHASE_RESULT) {
			return;
		}

		if (fdc->phase == FDC_PHASE_COMMAND) {
			fdc_log_detail("[FDC] Command byte %02X (%s)\r\n", value, fdc_command_name(value));
			fdc->command = value;
			fdc->param_len = 0;
			params_needed = fdc_command_params_needed(value);
			if (params_needed < 0) {
				fdc_result_clear(fdc);
				fdc_result_push(fdc, 0x80);
				fdc_begin_result_phase(fdc, 0);
				return;
			}

			fdc->params_needed = (uint8_t)params_needed;
			if (params_needed == 0) {
				fdc_handle_command(fdc);
			}
			else {
				fdc_set_phase(fdc, FDC_PHASE_PARAMS, 0x90);
			}
			return;
		}

		if (fdc->phase == FDC_PHASE_PARAMS) {
			fdc_log_detail("[FDC] Param[%u]=%02X for %s\r\n",
				(unsigned)fdc->param_len, value, fdc_command_name(fdc->command));
			if (fdc->param_len < (int)sizeof(fdc->params)) {
				fdc->params[fdc->param_len++] = value;
			}

			if (fdc->param_len >= fdc->params_needed) {
				fdc_handle_command(fdc);
			}
		}
		break;

	case 7:
		fdc_log_detail("[FDC] Write CCR=%02X\r\n", value);
		fdc->ccr = value & 0x03;
		break;

	default:
		break;
	}
}

static uint8_t fdc_port_read(void* opaque, uint16_t addr)
{
	FDC_IMPL_t* fdc;
	uint8_t value;
	uint8_t drive;

	fdc = (FDC_IMPL_t*)opaque;
	if (fdc == NULL) {
		return 0xFF;
	}

	addr &= 7;
	drive = fdc->selected_drive & 0x03;

	switch (addr) {
	case 0:
		value = 0xFF;
		fdc_log_detail("[FDC] Read STA=%02X\r\n", value);
		return value;

	case 1:
		value = 0xFF;
		fdc_log_detail("[FDC] Read STB=%02X\r\n", value);
		return value;

	case 2:
		value = fdc->dor;
		fdc_log_detail("[FDC] Read DOR=%02X\r\n", value);
		return value;

	case 3:
		value = 0x20;
		fdc_log_detail("[FDC] Read TDR=%02X\r\n", value);
		return value;

	case 4:
		value = fdc->msr;
		if (fdc->seek_pending) {
			value |= (uint8_t)(1U << (fdc->seek_drive & 0x03));
		}
		fdc_log_detail("[FDC] Read MSR=%02X phase=%u seek_pending=%u\r\n", value, fdc->phase, fdc->seek_pending);
		return value;

	case 5:
		fdc_clear_irq(fdc);

		if (fdc->phase == FDC_PHASE_RESULT) {
			if (fifo_get_empty(fdc->result_fifo)) {
				fdc_log_detail("[FDC] Read DATA with empty result FIFO\r\n");
				fdc_enter_command_phase(fdc);
				return 0x00;
			}

			value = fifo_read(fdc->result_fifo);
			fdc_log_detail("[FDC] Read DATA result=%02X\r\n", value);
			if (fifo_get_empty(fdc->result_fifo)) {
				fdc_enter_command_phase(fdc);
			}
			return value;
		}

		if (fdc->phase == FDC_PHASE_PIO_READ) {
			if ((fdc->transfer_pos < 0) || (fdc->transfer_pos >= fdc->transfer_len)) {
				fdc_log_detail("[FDC] Read DATA outside PIO buffer\r\n");
				return 0x00;
			}

			value = fdc->transfer_buffer[fdc->transfer_pos++];
			fdc_log_detail("[FDC] Read DATA pio=%02X pos=%d/%d\r\n", value, fdc->transfer_pos, fdc->transfer_len);
			if (fdc->transfer_pos >= fdc->transfer_len) {
				fdc_continue_after_sector(fdc);
			}
			return value;
		}

		fdc_log_detail("[FDC] Read DATA default=00 phase=%u\r\n", fdc->phase);
		return 0x00;

	case 7:
		if (fdc_drive_motor_enabled(fdc, drive)) {
			value = (fdd_get_disk_changed(drive) || !fdd_is_inserted(drive)) ? 0x80 : 0x00;
		}
		else {
			value = 0x00;
		}
		fdc_log_detail("[FDC] Read DIR=%02X drive=%u motor=%u changed=%u inserted=%u\r\n",
			value,
			drive,
			fdc_drive_motor_enabled(fdc, drive),
			fdd_get_disk_changed(drive),
			fdd_is_inserted(drive));
		return value;

	default:
		break;
	}

	return 0xFF;
}

int fdc_init(FDC_t* fdc, CPU_t* cpu, I8259_t* i8259, DMA_t* dma_controller)
{
	FDC_IMPL_t* impl;

	if ((fdc == NULL) || (cpu == NULL) || (i8259 == NULL) || (dma_controller == NULL)) {
		return -1;
	}

	impl = fdc_impl(fdc);
	if (impl == NULL) {
		impl = (FDC_IMPL_t*)calloc(1, sizeof(FDC_IMPL_t));
		if (impl == NULL) {
			return -1;
		}

		impl->result_fifo = fifo_init(FDC_RESULT_FIFO_LEN);
		if (impl->result_fifo == NULL) {
			free(impl);
			return -1;
		}

		impl->timer_seek = timing_addTimer(fdc_seek_callback, impl, FDC_SEEK_HZ, TIMING_DISABLED);
		impl->timer_rw = timing_addTimer(fdc_rw_callback, impl, FDC_RW_HZ, TIMING_DISABLED);
		ports_cbRegister(FDC_PRIMARY_BASE, 8, fdc_port_read, NULL, fdc_port_write, NULL, impl);
		fdc->impl = impl;
	}

	impl->cpu = cpu;
	impl->i8259 = i8259;
	impl->dma = dma_controller;
	impl->irq = FDC_IRQ;
	impl->dma_ch = FDC_DMA;
	impl->dor = 0x0C;
	impl->dsr = 0x80;
	impl->ccr = 0x00;
	fdd_init();
	fdd_set_type(0, FDD_TYPE_35_2HD);
	fdd_set_type(1, FDD_TYPE_35_2HD);
	fdd_set_type(2, FDD_TYPE_NONE);
	fdd_set_type(3, FDD_TYPE_NONE);
	fdc_log_detail("[FDC] Init complete irq=%u dma=%u\r\n", impl->irq, impl->dma_ch);
	fdc_soft_reset(impl);
	return 0;
}

int fdc_insert(FDC_t* fdc, uint8_t drive, char* path)
{
	FDC_IMPL_t* impl;

	impl = fdc_impl(fdc);
	if ((impl == NULL) || (drive >= 2)) {
		return -1;
	}

	fdc_abort(impl);
	if (fdd_load(drive, path) != FDD_RESULT_OK) {
		fdc_log_info("[FDC] Insert failed drive=%u path=%s\r\n", drive, path ? path : "(null)");
		return -1;
	}

	fdc_log_info("[FDC] Inserted drive=%u path=%s\r\n", drive, path);
	return 0;
}

void fdc_eject(FDC_t* fdc, uint8_t drive)
{
	FDC_IMPL_t* impl;

	impl = fdc_impl(fdc);
	if ((impl == NULL) || (drive >= 2)) {
		return;
	}

	fdc_abort(impl);
	fdd_close(drive);
	fdc_log_info("[FDC] Ejected drive=%u\r\n", drive);
}

const char* fdc_get_filename(FDC_t* fdc, uint8_t drive)
{
	FDC_IMPL_t* impl;

	impl = fdc_impl(fdc);
	if ((impl == NULL) || (drive >= 2)) {
		return NULL;
	}

	return fdd_get_filename(drive);
}

int fdc_is_inserted(FDC_t* fdc, uint8_t drive)
{
	FDC_IMPL_t* impl;

	impl = fdc_impl(fdc);
	if ((impl == NULL) || (drive >= 2)) {
		return 0;
	}

	return fdd_is_inserted(drive);
}

uint8_t fdc_io_read(FDC_t* fdc, uint16_t addr)
{
	FDC_IMPL_t* impl;

	impl = fdc_impl(fdc);
	if (impl == NULL) {
		return 0xFF;
	}

	return fdc_port_read(impl, addr);
}

void fdc_io_write(FDC_t* fdc, uint16_t addr, uint8_t value)
{
	FDC_IMPL_t* impl;

	impl = fdc_impl(fdc);
	if (impl == NULL) {
		return;
	}

	fdc_port_write(impl, addr, value);
}
