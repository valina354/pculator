#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ports.h"
#include "../chipset/i8259.h"
#include "../debuglog.h"
#include "../host/host.h"
#include "../timing.h"
#include "ata.h"

#define ATA_ERROR_ABORT 0x04
#define ATA_PRIMARY_IRQ 6
#define ATA_SECONDARY_IRQ 7

ATA_t ata;
static ATA_t ata_secondary;

static ATA_t* ata_controller_from_public_select(int select, uint8_t* drive_out);
static ATA_SLOT_t* ata_slot_from_public_select(int select, ATA_t** controller_out, uint8_t* drive_out);
static void ata_reset_drive(ATA_t* controller, uint8_t select);
static void ata_delayed_irq(void* dummy);
static void ata_reset_cb(void* dummy);
static void ata_command_cb(void* dummy);
static void ata_abort_command(ATA_t* controller, ATA_SLOT_t* slot, uint8_t sense_key, uint8_t raise_irq);
static ATA_SLOT_t* ata_get_cdrom_slot(int select, ATA_t** controller_out, uint8_t* drive_out);
static void ata_prepare_cdrom_media_change(ATA_t* controller, ATA_SLOT_t* slot);
static void ata_irq(ATA_t* controller);
static void ata_command_process(ATA_t* controller);

static uint8_t ata_controller_is_secondary(const ATA_t* controller)
{
	return (uint8_t)(controller == &ata_secondary);
}

static const char* ata_channel_name(const ATA_t* controller)
{
	return ata_controller_is_secondary(controller) ? "secondary" : "primary";
}

static const char* ata_location_name(const ATA_t* controller, uint8_t drive)
{
	if (ata_controller_is_secondary(controller)) {
		return drive ? "secondary slave" : "secondary master";
	}

	return drive ? "primary slave" : "primary master";
}

static ATA_t* ata_controller_from_public_select(int select, uint8_t* drive_out)
{
	if ((select < 0) || (select >= ATA_TOTAL_DEVICE_COUNT)) {
		return NULL;
	}

	if (drive_out != NULL) {
		*drive_out = (uint8_t)(select & 1);
	}

	return (select & 2) ? &ata_secondary : &ata;
}

static ATA_SLOT_t* ata_slot_from_public_select(int select, ATA_t** controller_out, uint8_t* drive_out)
{
	ATA_t* controller;
	uint8_t drive;

	controller = ata_controller_from_public_select(select, &drive);
	if (controller == NULL) {
		return NULL;
	}

	if (controller_out != NULL) {
		*controller_out = controller;
	}
	if (drive_out != NULL) {
		*drive_out = drive;
	}

	return &controller->disk[drive];
}

static ATA_SLOT_t* ata_current_slot(ATA_t* controller)
{
	return &controller->disk[controller->select & 1];
}

static uint8_t ata_slot_has_device(const ATA_SLOT_t* slot)
{
	return (slot->type != ATA_DEVICE_NONE);
}

static uint8_t ata_slot_lba_low(const ATA_SLOT_t* slot)
{
	return (uint8_t) (slot->regs.lba & 0xFF);
}

static uint8_t ata_slot_lba_mid(const ATA_SLOT_t* slot)
{
	return (uint8_t) ((slot->regs.lba >> 8) & 0xFF);
}

static uint8_t ata_slot_lba_high(const ATA_SLOT_t* slot)
{
	return (uint8_t) ((slot->regs.lba >> 16) & 0xFF);
}

static void ata_slot_set_lba_low(ATA_SLOT_t* slot, uint8_t value)
{
	slot->regs.lba = (slot->regs.lba & 0xFFFFFF00U) | value;
}

static void ata_slot_set_lba_mid(ATA_SLOT_t* slot, uint8_t value)
{
	slot->regs.lba = (slot->regs.lba & 0xFFFF00FFU) | ((uint32_t) value << 8);
}

static void ata_slot_set_lba_high(ATA_SLOT_t* slot, uint8_t value)
{
	slot->regs.lba = (slot->regs.lba & 0xFF00FFFFU) | ((uint32_t) value << 16);
}

static uint32_t ata_slot_full_lba(const ATA_SLOT_t* slot)
{
	return slot->regs.lba & 0x0FFFFFFFU;
}

static void ata_lower_irq(ATA_t* controller)
{
	if ((controller->i8259 != NULL) && controller->irq_pending) {
		i8259_clearirq(controller->i8259, controller->irq);
	}
	controller->irq_pending = 0;
}

static void ata_set_interrupt_enable(ATA_t* controller, uint8_t enabled)
{
	controller->disk[0].interrupt = enabled;
	controller->disk[1].interrupt = enabled;
	if (!enabled) {
		ata_lower_irq(controller);
	}
}

static void ata_slot_free_buffer(ATA_SLOT_t* slot)
{
	free(slot->buffer);
	slot->buffer = NULL;
	slot->buffer_size = 0;
	slot->buffer_len = 0;
	slot->buffer_pos = 0;
	slot->transfer_chunk = 0;
	slot->transfer_chunk_pos = 0;
}

static int ata_slot_ensure_buffer(ATA_SLOT_t* slot, uint32_t length)
{
	uint8_t* new_buffer;

	if (length == 0) {
		return 0;
	}
	if (slot->buffer_size >= length) {
		return 0;
	}

	new_buffer = (uint8_t*) realloc(slot->buffer, length);
	if (new_buffer == NULL) {
		return -1;
	}

	slot->buffer = new_buffer;
	slot->buffer_size = length;
	return 0;
}

static void ata_slot_close_media(ATA_SLOT_t* slot)
{
	if (slot->diskfile != NULL) {
		fclose(slot->diskfile);
		slot->diskfile = NULL;
	}
	atapi_cdrom_close(&slot->cdrom);
	slot->path[0] = 0;
	slot->sectors = 0;
	slot->heads = 0;
	slot->cylinders = 0;
	slot->spt = 0;
}

static void ata_slot_clear_transfer(ATA_SLOT_t* slot)
{
	slot->transfer_state = ATA_TRANSFER_NONE;
	slot->buffer_len = 0;
	slot->buffer_pos = 0;
	slot->transfer_chunk = 0;
	slot->transfer_chunk_pos = 0;
	slot->packet_limit = 0;
}

static void ata_slot_set_signature(ATA_SLOT_t* slot)
{
	slot->regs.sectors = 1;
	ata_slot_set_lba_low(slot, 1);

	if (slot->type == ATA_DEVICE_ATAPI_CDROM) {
		ata_slot_set_lba_mid(slot, 0x14);
		ata_slot_set_lba_high(slot, 0xEB);
	} else {
		ata_slot_set_lba_mid(slot, 0x00);
		ata_slot_set_lba_high(slot, 0x00);
	}
}

static void ata_slot_clear_error(ATA_SLOT_t* slot)
{
	slot->error = 0;
}

static void ata_slot_clear_service(ATA_SLOT_t* slot)
{
	slot->service = 0;
}

static void ata_slot_clear_status_override(ATA_SLOT_t* slot)
{
	slot->status = 0;
	slot->status_override_valid = 0;
}

static void ata_slot_set_status_override(ATA_SLOT_t* slot, uint8_t status)
{
	slot->status = status;
	slot->status_override_valid = 1;
}

static void ata_slot_set_error(ATA_SLOT_t* slot, uint8_t error)
{
	slot->error = error;
}

static void ata_slot_clear_delayed_commands(ATA_SLOT_t* slot)
{
	slot->delayed_identify_abort = 0;
}

static ATA_SLOT_t* ata_get_cdrom_slot(int select, ATA_t** controller_out, uint8_t* drive_out)
{
	ATA_SLOT_t* slot = ata_slot_from_public_select(select, controller_out, drive_out);

	if ((slot == NULL) || (slot->type != ATA_DEVICE_ATAPI_CDROM)) {
		return NULL;
	}

	return slot;
}

static void ata_prepare_cdrom_media_change(ATA_t* controller, ATA_SLOT_t* slot)
{
	if ((controller == NULL) || (slot == NULL)) {
		return;
	}

	/* Drop any in-flight packet/data phase so the next guest command sees the
	 * newly inserted or ejected media from a clean ready state. */
	ata_slot_clear_transfer(slot);
	ata_slot_clear_error(slot);
	ata_slot_clear_service(slot);
	ata_slot_clear_status_override(slot);
	ata_slot_clear_delayed_commands(slot);
	slot->command = 0;
	slot->lastcmd = 0;
	memset(slot->packet_cdb, 0, sizeof(slot->packet_cdb));
	controller->readssincecommand = 0;
	controller->delay_irq = 0;
	ata_lower_irq(controller);
}

static void ata_reset_drive(ATA_t* controller, uint8_t select)
{
	ATA_SLOT_t* slot = &controller->disk[select];

	ata_slot_clear_status_override(slot);
	ata_slot_clear_service(slot);
	ata_slot_clear_delayed_commands(slot);
	slot->command = 0;
	slot->lbamode = 0;
	slot->lastcmd = 0;
	slot->curreadsect = 0;
	slot->targetsect = 0;
	slot->cursect = 1;
	slot->curhead = 0;
	slot->curcyl = 0;
	slot->regs.features = 0;
	slot->regs.drive = 0;
	slot->regs.lba = 0;
	slot->regs.sectors = 1;
	memset(slot->packet_cdb, 0, sizeof(slot->packet_cdb));
	ata_slot_clear_transfer(slot);

	if (slot->type == ATA_DEVICE_NONE) {
		slot->error = 0;
		ata_slot_set_lba_low(slot, 0);
		ata_slot_set_lba_mid(slot, 0);
		ata_slot_set_lba_high(slot, 0);
	} else {
		slot->error = 1;
		ata_slot_set_signature(slot);
	}

	if (slot->type == ATA_DEVICE_ATAPI_CDROM) {
		atapi_cdrom_reset(&slot->cdrom);
	}
}

static void ata_complete_reset(ATA_t* controller)
{
	uint8_t i;

	controller->delay_irq = 0;
	controller->inreset = 0;
	controller->select = 0;
	controller->dscflag = ATA_STATUS_DSC;
	for (i = 0; i < ATA_CHANNEL_DEVICE_COUNT; i++) {
		ata_reset_drive(controller, i);
		if (controller->disk[i].type == ATA_DEVICE_ATAPI_CDROM) {
			/* Match 86Box's non-early ATAPI reset state: signature is valid,
			 * but status remains cleared until the next command. */
			ata_slot_set_status_override(&controller->disk[i], 0);
		}
	}
	ata_lower_irq(controller);
}

static void ata_begin_controller_reset(ATA_t* controller)
{
	controller->inreset = 1;
	controller->delay_irq = 0;
	ata_slot_clear_transfer(&controller->disk[0]);
	ata_slot_clear_transfer(&controller->disk[1]);
	ata_slot_clear_status_override(&controller->disk[0]);
	ata_slot_clear_status_override(&controller->disk[1]);
	ata_slot_clear_service(&controller->disk[0]);
	ata_slot_clear_service(&controller->disk[1]);
	ata_slot_clear_delayed_commands(&controller->disk[0]);
	ata_slot_clear_delayed_commands(&controller->disk[1]);
	timing_timerDisable(controller->cmdtimer);
	ata_lower_irq(controller);
	timing_timerEnable(controller->resettimer);
}

static void ata_delayed_irq(void* dummy)
{
	ATA_t* controller = (ATA_t*)dummy;
	ATA_SLOT_t* slot;

	if ((controller == NULL) || !controller->delay_irq) {
		return;
	}

	slot = &controller->disk[controller->irq_drive & 1];
	if (!controller->inreset && slot->interrupt && (controller->i8259 != NULL)) {
		i8259_doirq(controller->i8259, controller->irq);
		controller->irq_pending = 1;
	}
	controller->delay_irq = 0;
	controller->dscflag = ATA_STATUS_DSC;
	timing_timerDisable(controller->timernum);
}

static void ata_reset_cb(void* dummy)
{
	ATA_t* controller = (ATA_t*)dummy;

	if (controller == NULL) {
		return;
	}

	timing_timerDisable(controller->resettimer);
	ata_complete_reset(controller);
}

static void ata_command_cb(void* dummy)
{
	ATA_t* controller = (ATA_t*)dummy;
	uint8_t i;

	if (controller == NULL) {
		return;
	}

	timing_timerDisable(controller->cmdtimer);

	for (i = 0; i < ATA_CHANNEL_DEVICE_COUNT; i++) {
		ATA_SLOT_t* slot = &controller->disk[i];

		if (!slot->delayed_identify_abort) {
			continue;
		}

		slot->delayed_identify_abort = 0;
		controller->select = i;
		ata_slot_clear_status_override(slot);
		ata_slot_set_signature(slot);
		ata_abort_command(controller, slot, 0, 1);
	}
}

void ata_warm_reset(void)
{
	ATA_t* controller;
	uint8_t channel;

	/* Warm reboot should not detach media, but it must restore the ATA host
	 * controller and task-file signatures to a freshly reset state. */
	for (channel = 0; channel < ATA_CHANNEL_COUNT; channel++) {
		controller = channel ? &ata_secondary : &ata;
		if (controller->i8259 == NULL) {
			continue;
		}

		timing_timerDisable(controller->timernum);
		timing_timerDisable(controller->resettimer);
		timing_timerDisable(controller->cmdtimer);
		controller->control = 0;
		controller->delay_irq = 0;
		controller->inreset = 0;
		controller->irq_pending = 0;
		controller->irq_drive = 0;
		controller->readssincecommand = 0;
		ata_set_interrupt_enable(controller, 1);
		ata_complete_reset(controller);
	}

	debug_log(DEBUG_DETAIL, "[ATA] Warm reset complete\n");
}

static void ata_irq(ATA_t* controller)
{
	ATA_SLOT_t* slot = ata_current_slot(controller);

	if (slot->type == ATA_DEVICE_ATAPI_CDROM) {
		slot->service = 1;
	}
	controller->delay_irq = 1;
	controller->irq_drive = controller->select;
	ata_delayed_irq(controller);
}

static void ata_identify_store_word(uint8_t* buffer, uint16_t word, uint16_t value)
{
	uint32_t offset = (uint32_t) word << 1;
	buffer[offset] = (uint8_t) value;
	buffer[offset + 1] = (uint8_t) (value >> 8);
}

static void ata_identify_store_string(uint8_t* buffer, uint16_t word, uint16_t bytes, const char* value)
{
	uint32_t offset = (uint32_t) word << 1;
	uint16_t i;
	size_t len = strlen(value);

	for (i = 0; i < bytes; i++) {
		buffer[offset + i] = ' ';
	}
	for (i = 0; (i + 1) < bytes; i += 2) {
		char a = (i < len) ? value[i] : ' ';
		char b = ((i + 1) < len) ? value[i + 1] : ' ';
		buffer[offset + i] = (uint8_t) b;
		buffer[offset + i + 1] = (uint8_t) a;
	}
	if ((bytes & 1) && ((size_t) bytes <= len)) {
		buffer[offset + bytes - 1] = (uint8_t) value[bytes - 1];
	}
}

static uint8_t ata_gen_status(ATA_t* controller)
{
	ATA_SLOT_t* slot = ata_current_slot(controller);
	uint8_t status = 0;

	if (controller->inreset) {
		return ATA_STATUS_BUSY;
	}
	if (slot->status_override_valid) {
		return slot->status;
	}
	if (!ata_slot_has_device(slot) &&
		(slot->transfer_state == ATA_TRANSFER_NONE) &&
		(slot->lastcmd == 0) &&
		(slot->error == 0)) {
		return 0;
	}

	if (slot->transfer_state != ATA_TRANSFER_NONE) {
		status |= ATA_STATUS_DRQ;
	}
	if (ata_slot_has_device(slot)) {
		status |= ATA_STATUS_DRDY;
		if (slot->type != ATA_DEVICE_ATAPI_CDROM) {
			status |= ATA_STATUS_DSC;
		}
	}
	if (slot->error != 0) {
		status |= ATA_STATUS_ERR;
	}
	if (slot->type == ATA_DEVICE_ATAPI_CDROM) {
		/* 86Box exposes ATAPI bit 4 from a latched service flag, not from a
		 * synthesized DSC state. That leaves post-IRQ states at 0x50/0x58
		 * instead of dropping back to 0x40/0x48. */
		status &= (uint8_t) ~ATA_STATUS_DSC;
		if (slot->service) {
			status |= ATA_STATUS_DSC;
		}
	}

	return status;
}

static uint8_t ata_slot_gate_status(const ATA_t* controller, const ATA_SLOT_t* slot)
{
	if (controller->inreset) {
		return ATA_STATUS_BUSY;
	}
	if (slot->status_override_valid) {
		return slot->status;
	}
	if (slot->transfer_state != ATA_TRANSFER_NONE) {
		return ATA_STATUS_DRQ;
	}

	return 0;
}

static uint8_t ata_slot_accepts_taskfile_write(const ATA_t* controller, const ATA_SLOT_t* slot)
{
	return (uint8_t)((ata_slot_gate_status(controller, slot) & (ATA_STATUS_BUSY | ATA_STATUS_DRQ)) == 0);
}

static void ata_abort_command(ATA_t* controller, ATA_SLOT_t* slot, uint8_t sense_key, uint8_t raise_irq)
{
	ATA_TRANSFER_STATE_t transfer_state = slot->transfer_state;

	ata_slot_clear_transfer(slot);
	slot->buffer_len = 0;
	slot->buffer_pos = 0;
	ata_slot_set_error(slot, ATA_ERROR_ABORT | (sense_key << 4));
	if ((slot->type == ATA_DEVICE_ATAPI_CDROM) &&
		((slot->command == ATA_CMD_PACKET) ||
		 (transfer_state == ATA_TRANSFER_ATAPI_PACKET) ||
		 (transfer_state == ATA_TRANSFER_ATAPI_DATA_IN))) {
		slot->regs.sectors = 0x03;
	}
	if (raise_irq && slot->interrupt) {
		ata_irq(controller);
	}
}

static int ata_prepare_pio_in(ATA_SLOT_t* slot, uint32_t length, ATA_TRANSFER_STATE_t state)
{
	if (ata_slot_ensure_buffer(slot, length) != 0) {
		return -1;
	}

	slot->transfer_state = state;
	slot->buffer_len = length;
	slot->buffer_pos = 0;
	slot->transfer_chunk = length;
	slot->transfer_chunk_pos = 0;
	return 0;
}

static int ata_prepare_hdd_identify(ATA_SLOT_t* slot)
{
	uint32_t total_chs;

	if (ata_prepare_pio_in(slot, 512, ATA_TRANSFER_HDD_READ) != 0) {
		return -1;
	}

	memset(slot->buffer, 0, 512);
	total_chs = slot->cylinders * slot->heads * slot->spt;
	slot->curreadsect = 1;
	slot->targetsect = 1;
	ata_slot_clear_error(slot);
	ata_identify_store_word(slot->buffer, 0, 0x0040);
	ata_identify_store_word(slot->buffer, 1, (uint16_t) slot->cylinders);
	ata_identify_store_word(slot->buffer, 3, (uint16_t) slot->heads);
	ata_identify_store_word(slot->buffer, 4, (uint16_t) (slot->spt * 512U));
	ata_identify_store_word(slot->buffer, 5, 512);
	ata_identify_store_word(slot->buffer, 6, (uint16_t) slot->spt);
	ata_identify_store_string(slot->buffer, 10, 20, "PCULATOR0001");
	ata_identify_store_word(slot->buffer, 20, 3);
	ata_identify_store_word(slot->buffer, 21, 16);
	ata_identify_store_word(slot->buffer, 22, 4);
	ata_identify_store_string(slot->buffer, 23, 8, "1.0");
	ata_identify_store_string(slot->buffer, 27, 40, "PCulator virtual IDE disk");
	ata_identify_store_word(slot->buffer, 47, 1);
	ata_identify_store_word(slot->buffer, 48, 1);
	ata_identify_store_word(slot->buffer, 49, 1 << 9);
	ata_identify_store_word(slot->buffer, 51, 0x0200);
	ata_identify_store_word(slot->buffer, 52, 0x0200);
	ata_identify_store_word(slot->buffer, 53, 0x0007);
	ata_identify_store_word(slot->buffer, 54, (uint16_t) slot->cylinders);
	ata_identify_store_word(slot->buffer, 55, (uint16_t) slot->heads);
	ata_identify_store_word(slot->buffer, 56, (uint16_t) slot->spt);
	ata_identify_store_word(slot->buffer, 57, (uint16_t) (total_chs & 0xFFFF));
	ata_identify_store_word(slot->buffer, 58, (uint16_t) (total_chs >> 16));
	ata_identify_store_word(slot->buffer, 60, (uint16_t) (slot->sectors & 0xFFFF));
	ata_identify_store_word(slot->buffer, 61, (uint16_t) (slot->sectors >> 16));
	ata_identify_store_word(slot->buffer, 80, 0x001E);
	return 0;
}

static int ata_prepare_atapi_identify(ATA_SLOT_t* slot)
{
	if (ata_prepare_pio_in(slot, 512, ATA_TRANSFER_HDD_READ) != 0) {
		return -1;
	}

	memset(slot->buffer, 0, 512);
	ata_slot_clear_error(slot);
	ata_identify_store_word(slot->buffer, 0, 0x85C0);
	ata_identify_store_string(slot->buffer, 10, 20, "");
	ata_identify_store_string(slot->buffer, 23, 8, "1.0");
	ata_identify_store_string(slot->buffer, 27, 40, "PCulator virtual ATAPI CD-ROM");
	ata_identify_store_word(slot->buffer, 49, 1 << 9);
	ata_identify_store_word(slot->buffer, 71, 30);
	ata_identify_store_word(slot->buffer, 72, 30);
	ata_identify_store_word(slot->buffer, 80, 0x007E);
	ata_identify_store_word(slot->buffer, 81, 0x0019);
	ata_identify_store_word(slot->buffer, 126, 0xFFFE);
	return 0;
}

static uint32_t ata_hdd_chs_to_lba(const ATA_SLOT_t* slot)
{
	return ((slot->curcyl * slot->heads + slot->curhead) * slot->spt + (slot->cursect - 1));
}

static void ata_hdd_update_chs_position(ATA_SLOT_t* slot)
{
	slot->regs.lba = (slot->regs.lba & ~63U) | slot->cursect;
	slot->regs.lba = (slot->regs.lba & 0xFF0000FFU) | (slot->curcyl << 8);
	slot->regs.lba = (slot->regs.lba & 0xF0FFFFFFU) | (slot->curhead << 24);
	slot->cursect++;
	if (slot->cursect > slot->spt) {
		slot->cursect = 1;
		slot->curhead++;
		if (slot->curhead == slot->heads) {
			slot->curhead = 0;
			slot->curcyl++;
		}
	}
}

static void ata_hdd_read_sector(ATA_t* controller)
{
	ATA_SLOT_t* slot = ata_current_slot(controller);
	uint32_t lba;

	if (slot->diskfile == NULL) {
		ata_abort_command(controller, slot, 0, 1);
		return;
	}
	if (ata_prepare_pio_in(slot, 512, ATA_TRANSFER_HDD_READ) != 0) {
		ata_abort_command(controller, slot, 0, 1);
		return;
	}

	if (slot->lbamode) {
		lba = ata_slot_full_lba(slot);
		if (host_fseek64(slot->diskfile, (uint64_t) lba * 512ULL, SEEK_SET) != 0 ||
			(fread(slot->buffer, 1, 512, slot->diskfile) != 512)) {
			ata_abort_command(controller, slot, 0, 1);
			return;
		}
		slot->regs.lba = (slot->regs.lba & 0xF0000000U) | ((lba + 1) & 0x0FFFFFFFU);
	} else {
		lba = ata_hdd_chs_to_lba(slot);
		if (host_fseek64(slot->diskfile, (uint64_t) lba * 512ULL, SEEK_SET) != 0 ||
			(fread(slot->buffer, 1, 512, slot->diskfile) != 512)) {
			ata_abort_command(controller, slot, 0, 1);
			return;
		}
		ata_hdd_update_chs_position(slot);
	}

	slot->curreadsect++;
	ata_slot_clear_error(slot);
	if (slot->interrupt) {
		ata_irq(controller);
	}
}

static void ata_hdd_write_sector(ATA_t* controller)
{
	ATA_SLOT_t* slot = ata_current_slot(controller);
	uint32_t lba;

	if (slot->diskfile == NULL) {
		ata_abort_command(controller, slot, 0, 1);
		return;
	}

	if (slot->lbamode) {
		lba = ata_slot_full_lba(slot);
		if (host_fseek64(slot->diskfile, (uint64_t) lba * 512ULL, SEEK_SET) != 0 ||
			(fwrite(slot->buffer, 1, 512, slot->diskfile) != 512)) {
			ata_abort_command(controller, slot, 0, 1);
			return;
		}
		slot->regs.lba = (slot->regs.lba & 0xF0000000U) | ((lba + 1) & 0x0FFFFFFFU);
	} else {
		lba = ata_hdd_chs_to_lba(slot);
		if (host_fseek64(slot->diskfile, (uint64_t) lba * 512ULL, SEEK_SET) != 0 ||
			(fwrite(slot->buffer, 1, 512, slot->diskfile) != 512)) {
			ata_abort_command(controller, slot, 0, 1);
			return;
		}
		ata_hdd_update_chs_position(slot);
	}

	slot->buffer_pos = 0;
	slot->transfer_chunk = 512;
	slot->transfer_chunk_pos = 0;
	slot->curreadsect++;
	ata_slot_clear_error(slot);
	if (slot->interrupt) {
		ata_irq(controller);
	}
}

static void ata_hdd_begin_read(ATA_t* controller, ATA_SLOT_t* slot)
{
	uint32_t sector;
	uint32_t cyl;
	uint32_t head;

	ata_slot_clear_error(slot);
	slot->curreadsect = 0;
	slot->targetsect = (slot->regs.sectors == 0) ? 256 : slot->regs.sectors;
	if (slot->lbamode) {
		controller->savelba = ata_slot_full_lba(slot);
	} else {
		sector = slot->regs.lba & 63;
		cyl = (slot->regs.lba >> 8) & 0xFFFF;
		head = (slot->regs.lba >> 24) & 0x0F;
		slot->curcyl = cyl;
		slot->curhead = head;
		slot->cursect = sector;
		controller->savelba = (cyl * slot->heads + head) * slot->spt + (sector - 1);
	}
	ata_hdd_read_sector(controller);
}

static void ata_hdd_begin_write(ATA_t* controller, ATA_SLOT_t* slot)
{
	uint32_t sector;
	uint32_t cyl;
	uint32_t head;

	if (ata_slot_ensure_buffer(slot, 512) != 0) {
		ata_abort_command(controller, slot, 0, 1);
		return;
	}

	ata_slot_clear_error(slot);
	slot->transfer_state = ATA_TRANSFER_HDD_WRITE;
	slot->buffer_len = 512;
	slot->buffer_pos = 0;
	slot->transfer_chunk = 512;
	slot->transfer_chunk_pos = 0;
	slot->curreadsect = 0;
	slot->targetsect = (slot->regs.sectors == 0) ? 256 : slot->regs.sectors;
	if (slot->lbamode) {
		controller->savelba = ata_slot_full_lba(slot);
	} else {
		sector = slot->regs.lba & 63;
		cyl = (slot->regs.lba >> 8) & 0xFFFF;
		head = (slot->regs.lba >> 24) & 0x0F;
		slot->curcyl = cyl;
		slot->curhead = head;
		slot->cursect = sector;
		controller->savelba = (cyl * slot->heads + head) * slot->spt + (sector - 1);
	}
}

static uint32_t ata_atapi_byte_count_limit(const ATA_SLOT_t* slot)
{
	uint32_t limit = ((uint32_t) ata_slot_lba_high(slot) << 8) | ata_slot_lba_mid(slot);

	if (limit == 0) {
		return 0x10000U;
	}
	return limit;
}

static void ata_atapi_set_reason(ATA_SLOT_t* slot, uint8_t reason)
{
	slot->regs.sectors = reason;
}

static void ata_atapi_complete(ATA_t* controller, ATA_SLOT_t* slot, uint8_t raise_irq)
{
	ata_slot_clear_transfer(slot);
	ata_atapi_set_reason(slot, 0x03);
	/* Match 86Box by leaving the ATAPI request-length byte pair alone on
	 * completion; BIOSes can poll those task-file bytes after a successful
	 * command rather than expecting them to be forced to 0. */
	ata_slot_clear_error(slot);
	if (raise_irq && slot->interrupt) {
		ata_irq(controller);
	}
}

static void ata_atapi_begin_data_in(ATA_t* controller, ATA_SLOT_t* slot, uint8_t raise_irq)
{
	uint32_t remaining;
	uint32_t chunk;

	remaining = slot->buffer_len - slot->buffer_pos;
	if (remaining == 0) {
		ata_atapi_complete(controller, slot, raise_irq);
		return;
	}

	chunk = slot->packet_limit;
	if ((chunk == 0) || (chunk > remaining)) {
		chunk = remaining;
	}

	slot->transfer_state = ATA_TRANSFER_ATAPI_DATA_IN;
	slot->transfer_chunk = chunk;
	slot->transfer_chunk_pos = 0;
	ata_atapi_set_reason(slot, 0x02);
	ata_slot_set_lba_mid(slot, (uint8_t) (chunk & 0xFF));
	ata_slot_set_lba_high(slot, (uint8_t) (chunk >> 8));
	ata_slot_clear_error(slot);
	if (raise_irq && slot->interrupt) {
		ata_irq(controller);
	}
}

static void ata_atapi_execute_packet(ATA_t* controller, ATA_SLOT_t* slot)
{
	ATAPI_CDROM_CMD_RESULT_t result;
	size_t buffer_len = slot->buffer_len;
	size_t buffer_capacity = slot->buffer_size;

	slot->buffer_len = 0;
	slot->buffer_pos = 0;
	slot->transfer_chunk = 0;
	slot->transfer_chunk_pos = 0;

	result = atapi_cdrom_command(&slot->cdrom, slot->packet_cdb, &slot->buffer, &buffer_len, &buffer_capacity);
	slot->buffer_size = (uint32_t) buffer_capacity;

	if (result == ATAPI_CDROM_CMD_ERROR) {
		ata_abort_command(controller, slot, slot->cdrom.sense_key, 1);
		return;
	}

	slot->buffer_len = (uint32_t) buffer_len;
	slot->buffer_pos = 0;

	if (result == ATAPI_CDROM_CMD_DATA_IN) {
		ata_atapi_begin_data_in(controller, slot, 1);
	} else {
		ata_atapi_complete(controller, slot, 1);
	}
}

static void ata_command_process(ATA_t* controller)
{
	ATA_SLOT_t* slot = ata_current_slot(controller);

	ata_slot_clear_transfer(slot);
	ata_slot_clear_error(slot);
	if (slot->type == ATA_DEVICE_ATAPI_CDROM) {
		/* A new host command acknowledges the prior ATAPI service phase. */
		ata_slot_clear_service(slot);
	}
	controller->readssincecommand = 0;
	slot->lastcmd = slot->command;
	ata_slot_clear_status_override(slot);

	switch (slot->command) {
	case ATA_CMD_IDENTIFY:
		if (slot->type == ATA_DEVICE_HDD) {
			if (ata_prepare_hdd_identify(slot) != 0) {
				ata_abort_command(controller, slot, 0, 1);
				break;
			}
			if (slot->interrupt) {
				ata_irq(controller);
			}
		} else if (slot->type == ATA_DEVICE_ATAPI_CDROM) {
			slot->delayed_identify_abort = 1;
			/* Start ATAPI IDENTIFY from a clean BSY phase rather than
			 * carrying the previous packet-service state into the new
			 * command. */
			ata_slot_set_status_override(slot, ATA_STATUS_BUSY);
			timing_timerEnable(controller->cmdtimer);
		} else {
			ata_abort_command(controller, slot, 0, 1);
		}
		break;

	case ATA_CMD_IDENTIFY_PACKET:
		if (slot->type == ATA_DEVICE_ATAPI_CDROM) {
			if (ata_prepare_atapi_identify(slot) != 0) {
				ata_abort_command(controller, slot, 0, 1);
				break;
			}
			if (slot->interrupt) {
				ata_irq(controller);
			}
		} else {
			ata_abort_command(controller, slot, 0, 1);
		}
		break;

	case ATA_CMD_PACKET:
		if (slot->type == ATA_DEVICE_ATAPI_CDROM) {
			if (slot->regs.features & 0x01) {
				ata_abort_command(controller, slot, 0, 1);
				break;
			}
			slot->packet_limit = ata_atapi_byte_count_limit(slot);
			slot->transfer_state = ATA_TRANSFER_ATAPI_PACKET;
			slot->buffer_len = 0;
			slot->buffer_pos = 0;
			slot->transfer_chunk = 0;
			slot->transfer_chunk_pos = 0;
			memset(slot->packet_cdb, 0, sizeof(slot->packet_cdb));
			ata_atapi_set_reason(slot, 0x01);
			ata_slot_clear_error(slot);
		} else {
			ata_abort_command(controller, slot, 0, 1);
		}
		break;

	case ATA_CMD_DIAGNOSTIC:
		/*
		 * Match 86Box by treating EXECUTE DEVICE DIAGNOSTIC as a real
		 * controller reset sequence that re-presents each device's task-file
		 * signature after BSY clears. A simple "clear error and IRQ" leaves
		 * the ATAPI task file carrying whatever DOS's last packet command wrote.
		 */
		ata_begin_controller_reset(controller);
		break;

	case ATA_CMD_DEVICE_RESET:
		if (slot->type == ATA_DEVICE_ATAPI_CDROM) {
			ata_reset_drive(controller, controller->select);
			slot = ata_current_slot(controller);
			ata_slot_set_status_override(slot, 0);
			if (slot->interrupt) {
				ata_irq(controller);
			}
		} else {
			ata_abort_command(controller, slot, 0, 1);
		}
		break;

	case ATA_CMD_INITIALIZE_PARAMS:
		if ((slot->type != ATA_DEVICE_HDD) || (slot->diskfile == NULL)) {
			ata_abort_command(controller, slot, 0, 1);
			break;
		}
		slot->spt = slot->regs.sectors & 63;
		slot->heads = ((slot->regs.lba >> 24) & 0x0F) + 1;
		ata_slot_clear_error(slot);
		if (slot->interrupt) {
			ata_irq(controller);
		}
		break;

	case ATA_CMD_IDLE_IMMEDIATE:
	case 0x40:
	case 0x41:
	case 0x70:
	case 0xEF:
		if (!ata_slot_has_device(slot)) {
			ata_abort_command(controller, slot, 0, 1);
			break;
		}
		ata_slot_clear_error(slot);
		if (slot->interrupt) {
			ata_irq(controller);
		}
		break;

	case ATA_CMD_RECALIBRATE:
		if (slot->type != ATA_DEVICE_HDD) {
			ata_abort_command(controller, slot, 0, 1);
			break;
		}
		ata_slot_clear_error(slot);
		if (slot->interrupt) {
			ata_irq(controller);
		}
		break;

	case ATA_CMD_READ_SECTORS:
	case 0x21:
		if ((slot->type != ATA_DEVICE_HDD) || (slot->diskfile == NULL)) {
			ata_abort_command(controller, slot, 0, 1);
			break;
		}
		ata_hdd_begin_read(controller, slot);
		break;

	case ATA_CMD_WRITE_SECTORS:
	case 0x31:
		if ((slot->type != ATA_DEVICE_HDD) || (slot->diskfile == NULL)) {
			ata_abort_command(controller, slot, 0, 1);
			break;
		}
		ata_hdd_begin_write(controller, slot);
		break;

	default:
		debug_log(DEBUG_DETAIL, "[ATA] Unimplemented command on %s channel: 0x%02X\n",
			ata_channel_name(controller),
			slot->command);
		ata_abort_command(controller, slot, 0, 1);
		break;
	}
}

static uint8_t ata_data_read_byte(ATA_t* controller, ATA_SLOT_t* slot)
{
	uint8_t value;

	if (slot->transfer_state == ATA_TRANSFER_NONE) {
		return 0;
	}
	if (slot->buffer_pos >= slot->buffer_len) {
		return 0;
	}

	value = slot->buffer[slot->buffer_pos++];

	switch (slot->transfer_state) {
	case ATA_TRANSFER_HDD_READ:
		if (slot->buffer_pos >= slot->buffer_len) {
			if (slot->curreadsect >= slot->targetsect) {
				ata_slot_clear_transfer(slot);
			} else {
				ata_hdd_read_sector(controller);
			}
		}
		break;

	case ATA_TRANSFER_ATAPI_DATA_IN:
		slot->transfer_chunk_pos++;
		if (slot->transfer_chunk_pos >= slot->transfer_chunk) {
			if (slot->buffer_pos >= slot->buffer_len) {
				ata_atapi_complete(controller, slot, 1);
			} else {
				ata_atapi_begin_data_in(controller, slot, 1);
			}
		}
		break;

	default:
		break;
	}

	return value;
}

static void ata_data_write_byte(ATA_t* controller, ATA_SLOT_t* slot, uint8_t value)
{
	if (slot->transfer_state == ATA_TRANSFER_HDD_WRITE) {
		if ((slot->buffer == NULL) || (slot->buffer_pos >= slot->buffer_len)) {
			return;
		}

		slot->buffer[slot->buffer_pos++] = value;
		if (slot->buffer_pos >= slot->buffer_len) {
			ata_hdd_write_sector(controller);
			if (slot->curreadsect >= slot->targetsect) {
				ata_slot_clear_transfer(slot);
			}
		}
		return;
	}

	if (slot->transfer_state == ATA_TRANSFER_ATAPI_PACKET) {
		if (slot->buffer_pos >= sizeof(slot->packet_cdb)) {
			return;
		}

		slot->packet_cdb[slot->buffer_pos++] = value;
		if (slot->buffer_pos >= sizeof(slot->packet_cdb)) {
			ata_atapi_execute_packet(controller, slot);
		}
	}
}

static uint8_t ata_read_port(void* dummy, uint16_t portnum)
{
	ATA_t* controller = (ATA_t*)dummy;
	ATA_SLOT_t* slot = ata_current_slot(controller);
	uint8_t ret = 0;

	switch (portnum) {
	case ATA_PORT_DATA:
	case ATA_PORT_DATA_SEC:
		ret = ata_data_read_byte(controller, slot);
		break;

	case ATA_PORT_ERROR:
	case ATA_PORT_ERROR_SEC:
		ret = slot->error;
		break;

	case ATA_PORT_SECTORS:
	case ATA_PORT_SECTORS_SEC:
		ret = slot->regs.sectors;
		break;

	case ATA_PORT_LBA_LOW:
	case ATA_PORT_LBA_LOW_SEC:
		ret = ata_slot_lba_low(slot);
		break;

	case ATA_PORT_LBA_MID:
	case ATA_PORT_LBA_MID_SEC:
		ret = ata_slot_lba_mid(slot);
		break;

	case ATA_PORT_LBA_HIGH:
	case ATA_PORT_LBA_HIGH_SEC:
		ret = ata_slot_lba_high(slot);
		break;

	case ATA_PORT_DRIVE:
	case ATA_PORT_DRIVE_SEC:
		ret = 0xA0 | ((slot->regs.lba >> 24) & 0x0F) | (controller->select << 4) | (slot->lbamode << 6);
		break;

	case ATA_PORT_STATUS:
	case ATA_PORT_STATUS_SEC:
		ret = ata_gen_status(controller);
		ata_lower_irq(controller);
		break;

	case ATA_PORT_ALTERNATE:
	case ATA_PORT_ALTERNATE_SEC:
		ret = ata_gen_status(controller);
		break;
	}

	return ret;
}

static void ata_write_port(void* dummy, uint16_t portnum, uint8_t value)
{
	ATA_t* controller = (ATA_t*)dummy;
	ATA_SLOT_t* slot = ata_current_slot(controller);

	switch (portnum) {
	case ATA_PORT_DATA:
	case ATA_PORT_DATA_SEC:
		ata_data_write_byte(controller, slot, value);
		break;

	case ATA_PORT_FEATURES:
	case ATA_PORT_FEATURES_SEC:
		if (ata_slot_accepts_taskfile_write(controller, slot)) {
			slot->regs.features = value;
		}
		break;

	case ATA_PORT_SECTORS:
	case ATA_PORT_SECTORS_SEC:
		if (ata_slot_accepts_taskfile_write(controller, slot)) {
			slot->regs.sectors = value;
		}
		break;

	case ATA_PORT_LBA_LOW:
	case ATA_PORT_LBA_LOW_SEC:
		if (ata_slot_accepts_taskfile_write(controller, slot)) {
			ata_slot_set_lba_low(slot, value);
		}
		break;

	case ATA_PORT_LBA_MID:
	case ATA_PORT_LBA_MID_SEC:
		if (ata_slot_accepts_taskfile_write(controller, slot)) {
			ata_slot_set_lba_mid(slot, value);
		}
		break;

	case ATA_PORT_LBA_HIGH:
	case ATA_PORT_LBA_HIGH_SEC:
		if (ata_slot_accepts_taskfile_write(controller, slot)) {
			ata_slot_set_lba_high(slot, value);
		}
		break;

	case ATA_PORT_DRIVE:
	case ATA_PORT_DRIVE_SEC:
		controller->select = (value >> 4) & 1;
		slot = ata_current_slot(controller);
		if (ata_slot_accepts_taskfile_write(controller, slot)) {
			slot->lbamode = (value >> 6) & 1;
			slot->regs.drive = (uint8_t) (value & 0xEF);
			slot->regs.lba = (slot->regs.lba & 0x00FFFFFFU) | ((uint32_t) (value & 0x0F) << 24);
		}
		break;

	case ATA_PORT_COMMAND:
	case ATA_PORT_COMMAND_SEC:
		slot->command = value;
		ata_command_process(controller);
		break;

	case ATA_PORT_ALTERNATE:
	case ATA_PORT_ALTERNATE_SEC:
	{
		uint8_t old_control = controller->control;

		controller->control = value;
		ata_set_interrupt_enable(controller, ((value >> 1) & 1) ^ 1);
		if (!(old_control & 0x04) && (value & 0x04)) {
			controller->inreset = 1;
			controller->delay_irq = 0;
			ata_slot_clear_transfer(&controller->disk[0]);
			ata_slot_clear_transfer(&controller->disk[1]);
			ata_lower_irq(controller);
		} else if ((old_control & 0x04) && !(value & 0x04)) {
			timing_timerEnable(controller->resettimer);
		}
		break;
	}
	}
}

static uint16_t ata_read_data(void* dummy, uint16_t portnum)
{
	ATA_t* controller = (ATA_t*)dummy;
	uint16_t lo;
	uint16_t hi;

	(void) portnum;
	lo = ata_data_read_byte(controller, ata_current_slot(controller));
	hi = ata_data_read_byte(controller, ata_current_slot(controller));
	return lo | (hi << 8);
}

static uint32_t ata_read_data32(void* dummy, uint16_t portnum)
{
	ATA_t* controller = (ATA_t*)dummy;
	uint32_t b0;
	uint32_t b1;
	uint32_t b2;
	uint32_t b3;

	(void) portnum;
	b0 = ata_data_read_byte(controller, ata_current_slot(controller));
	b1 = ata_data_read_byte(controller, ata_current_slot(controller));
	b2 = ata_data_read_byte(controller, ata_current_slot(controller));
	b3 = ata_data_read_byte(controller, ata_current_slot(controller));
	return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static void ata_write_data(void* dummy, uint16_t portnum, uint16_t value)
{
	ATA_t* controller = (ATA_t*)dummy;
	ATA_SLOT_t* slot = ata_current_slot(controller);

	(void) portnum;
	ata_data_write_byte(controller, slot, (uint8_t)value);
	ata_data_write_byte(controller, slot, (uint8_t)(value >> 8));
}

static void ata_write_data32(void* dummy, uint16_t portnum, uint32_t value)
{
	ATA_t* controller = (ATA_t*)dummy;
	ATA_SLOT_t* slot = ata_current_slot(controller);

	(void) portnum;
	ata_data_write_byte(controller, slot, (uint8_t)value);
	ata_data_write_byte(controller, slot, (uint8_t)(value >> 8));
	ata_data_write_byte(controller, slot, (uint8_t)(value >> 16));
	ata_data_write_byte(controller, slot, (uint8_t)(value >> 24));
}

void ata_detach(int select)
{
	ATA_t* controller;
	ATA_SLOT_t* slot;
	uint8_t drive;

	slot = ata_slot_from_public_select(select, &controller, &drive);
	if (slot == NULL) {
		return;
	}

	ata_slot_close_media(slot);
	ata_slot_free_buffer(slot);
	slot->type = ATA_DEVICE_NONE;
	ata_reset_drive(controller, drive);
}

ATA_DEVICE_TYPE_t ata_get_device_type(int select)
{
	ATA_SLOT_t* slot = ata_slot_from_public_select(select, NULL, NULL);

	return (slot == NULL) ? ATA_DEVICE_NONE : slot->type;
}

const char* ata_get_cdrom_media_path(int select)
{
	ATA_SLOT_t* slot = ata_get_cdrom_slot(select, NULL, NULL);

	if ((slot == NULL) || !slot->cdrom.has_media) {
		return NULL;
	}

	return slot->cdrom.path;
}

int ata_change_cdrom(int select, const char* filename)
{
	ATA_t* controller;
	ATA_SLOT_t* slot;
	uint8_t drive;

	slot = ata_get_cdrom_slot(select, &controller, &drive);
	if (slot == NULL) {
		return 0;
	}

	ata_prepare_cdrom_media_change(controller, slot);
	if (atapi_cdrom_attach(&slot->cdrom, filename) != 0) {
		return 0;
	}
	if ((filename == NULL) || (*filename == 0) || ((filename[0] == '.') && (filename[1] == 0))) {
		slot->path[0] = 0;
	} else {
		strncpy(slot->path, filename, sizeof(slot->path) - 1);
		slot->path[sizeof(slot->path) - 1] = 0;
	}

	debug_log(DEBUG_INFO, "[ATA] Changed ATAPI CD-ROM media on %s%s%s\n",
		ata_location_name(controller, drive),
		(filename != NULL) ? ": " : "",
		(filename != NULL) ? filename : "");
	return 1;
}

int ata_eject_cdrom(int select)
{
	ATA_t* controller;
	ATA_SLOT_t* slot;
	uint8_t drive;

	slot = ata_get_cdrom_slot(select, &controller, &drive);
	if (slot == NULL) {
		return 0;
	}

	ata_prepare_cdrom_media_change(controller, slot);
	if (atapi_cdrom_eject(&slot->cdrom) != 0) {
		return 0;
	}
	slot->path[0] = 0;

	debug_log(DEBUG_INFO, "[ATA] Ejected ATAPI CD-ROM media on %s\n",
		ata_location_name(controller, drive));
	return 1;
}

int ata_attach_hdd(int select, const char* filename)
{
	ATA_t* controller;
	ATA_SLOT_t* slot;
	FILE* diskfile;
	uint32_t chs_total;
	uint64_t disk_bytes;
	uint8_t drive;

	slot = ata_slot_from_public_select(select, &controller, &drive);
	if ((slot == NULL) || (filename == NULL) || (*filename == 0)) {
		return 0;
	}

	diskfile = fopen(filename, "r+b");
	if (diskfile == NULL) {
		return 0;
	}
	if (host_fseek64(diskfile, 0, SEEK_END) != 0) {
		fclose(diskfile);
		return 0;
	}
	disk_bytes = host_ftell64(diskfile);
	if ((disk_bytes == UINT64_MAX) || ((disk_bytes / 512ULL) > UINT32_MAX)) {
		fclose(diskfile);
		return 0;
	}
	if (host_fseek64(diskfile, 0, SEEK_SET) != 0) {
		fclose(diskfile);
		return 0;
	}

	ata_detach(select);
	slot = ata_slot_from_public_select(select, &controller, &drive);
	if (slot == NULL) {
		fclose(diskfile);
		return 0;
	}

	slot->type = ATA_DEVICE_HDD;
	slot->diskfile = diskfile;
	strncpy(slot->path, filename, sizeof(slot->path) - 1);
	slot->path[sizeof(slot->path) - 1] = 0;
	slot->sectors = (uint32_t) (disk_bytes / 512ULL);
	slot->spt = 63;
	slot->heads = 16;
	slot->cylinders = slot->sectors / (16L * 63L);
	chs_total = slot->cylinders * slot->heads * slot->spt;
	if (chs_total > slot->sectors) {
		slot->cylinders--;
	}
	ata_reset_drive(controller, drive);
	debug_log(DEBUG_INFO, "[ATA] Attached HDD on %s: %s\n", ata_location_name(controller, drive), filename);
	return 1;
}

int ata_attach_cdrom(int select, const char* filename)
{
	ATA_t* controller;
	ATA_SLOT_t* slot;
	uint8_t drive;

	slot = ata_slot_from_public_select(select, &controller, &drive);
	if (slot == NULL) {
		return 0;
	}

	ata_detach(select);
	slot = ata_slot_from_public_select(select, &controller, &drive);
	if (slot == NULL) {
		return 0;
	}

	slot->type = ATA_DEVICE_ATAPI_CDROM;
	atapi_cdrom_init(&slot->cdrom);
	if (atapi_cdrom_attach(&slot->cdrom, filename) != 0) {
		ata_detach(select);
		return 0;
	}
	if (filename != NULL) {
		strncpy(slot->path, filename, sizeof(slot->path) - 1);
		slot->path[sizeof(slot->path) - 1] = 0;
	}
	ata_reset_drive(controller, drive);
	debug_log(DEBUG_INFO, "[ATA] Attached ATAPI CD-ROM on %s%s%s\n",
		ata_location_name(controller, drive),
		(filename != NULL) ? ": " : "",
		(filename != NULL) ? filename : "");
	return 1;
}

int ata_insert_disk(int select, char* filename)
{
	return ata_attach_hdd(select, filename);
}

static void ata_init_controller(ATA_t* controller, I8259_t* i8259, uint8_t irq)
{
	uint8_t i;

	controller->select = 0;
	controller->i8259 = i8259;
	controller->control = 0;
	controller->delay_irq = 0;
	controller->inreset = 0;
	controller->irq = irq;
	controller->dscflag = ATA_STATUS_DSC;
	controller->irq_pending = 0;
	controller->irq_drive = 0;
	controller->readssincecommand = 0;
	controller->savelba = 0;

	for (i = 0; i < ATA_CHANNEL_DEVICE_COUNT; i++) {
		ata_reset_drive(controller, i);
	}
	ata_set_interrupt_enable(controller, 1);

	controller->timernum = timing_addTimer(ata_delayed_irq, controller, 50000, 0);
	controller->resettimer = timing_addTimer(ata_reset_cb, controller, 4, 0);
	/* Keep deferred command completions short; long wall-clock delays make
	 * BIOS probes treat ATAPI IDENTIFY very differently from 86Box. */
	controller->cmdtimer = timing_addTimer(ata_command_cb, controller, 50000, 0);
}

void ata_init(I8259_t* i8259)
{
	ata_init_controller(&ata, i8259, ATA_PRIMARY_IRQ);
	ports_cbRegisterEx(ATA_PORT_DATA, 1, ata_read_port, ata_read_data, ata_read_data32, ata_write_port, ata_write_data, ata_write_data32, &ata);
	ports_cbRegister(ATA_PORT_ERROR, 7, ata_read_port, NULL, ata_write_port, NULL, &ata);
	ports_cbRegister(ATA_PORT_ALTERNATE, 1, ata_read_port, NULL, ata_write_port, NULL, &ata);
}

void ata_init_dual(I8259_t* i8259)
{
	ata_init(i8259);

	ata_init_controller(&ata_secondary, i8259, ATA_SECONDARY_IRQ);
	ports_cbRegisterEx(ATA_PORT_DATA_SEC, 1, ata_read_port, ata_read_data, ata_read_data32, ata_write_port, ata_write_data, ata_write_data32, &ata_secondary);
	ports_cbRegister(ATA_PORT_ERROR_SEC, 7, ata_read_port, NULL, ata_write_port, NULL, &ata_secondary);
	ports_cbRegister(ATA_PORT_ALTERNATE_SEC, 1, ata_read_port, NULL, ata_write_port, NULL, &ata_secondary);
}
