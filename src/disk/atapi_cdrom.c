#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../debuglog.h"
#include "../host/host.h"
#include "atapi_cdrom.h"

#define ATAPI_GPCMD_TEST_UNIT_READY      0x00
#define ATAPI_GPCMD_REQUEST_SENSE        0x03
#define ATAPI_GPCMD_READ_6               0x08
#define ATAPI_GPCMD_SEEK_6               0x0B
#define ATAPI_GPCMD_INQUIRY              0x12
#define ATAPI_GPCMD_MODE_SENSE_6         0x1A
#define ATAPI_GPCMD_START_STOP_UNIT      0x1B
#define ATAPI_GPCMD_PREVENT_REMOVAL      0x1E
#define ATAPI_GPCMD_READ_CAPACITY        0x25
#define ATAPI_GPCMD_READ_10              0x28
#define ATAPI_GPCMD_SEEK_10              0x2B
#define ATAPI_GPCMD_READ_SUBCHANNEL      0x42
#define ATAPI_GPCMD_READ_TOC_PMA_ATIP    0x43
#define ATAPI_GPCMD_READ_HEADER          0x44
#define ATAPI_GPCMD_GET_CONFIGURATION    0x46
#define ATAPI_GPCMD_GET_EVENT_STATUS     0x4A
#define ATAPI_GPCMD_READ_DISC_INFO       0x51
#define ATAPI_GPCMD_READ_TRACK_INFO      0x52
#define ATAPI_GPCMD_MODE_SENSE_10        0x5A
#define ATAPI_GPCMD_READ_12              0xA8
#define ATAPI_GPCMD_MECHANISM_STATUS     0xBD

#define ATAPI_SENSE_NONE             0x00
#define ATAPI_SENSE_NOT_READY        0x02
#define ATAPI_SENSE_MEDIUM_ERROR     0x03
#define ATAPI_SENSE_ILLEGAL_REQUEST  0x05
#define ATAPI_SENSE_UNIT_ATTENTION   0x06

#define ATAPI_ASC_NONE                    0x00
#define ATAPI_ASC_UNRECOVERED_READ_ERROR  0x11
#define ATAPI_ASC_ILLEGAL_OPCODE          0x20
#define ATAPI_ASC_LBA_OUT_OF_RANGE        0x21
#define ATAPI_ASC_INV_FIELD_IN_CMD_PACKET 0x24
#define ATAPI_ASC_MEDIUM_MAY_HAVE_CHANGED 0x28
#define ATAPI_ASC_MEDIUM_NOT_PRESENT      0x3A

#define ATAPI_ASCQ_NONE 0x00

#define ATAPI_MAX_TRANSFER_BYTES (16U * 1024U * 1024U)

#define ATAPI_TOC_FORMAT_NORMAL  0x00
#define ATAPI_TOC_FORMAT_SESSION 0x01
#define ATAPI_TOC_FORMAT_RAW     0x02

#define ATAPI_TRACK_CONTROL_DATA 0x14
#define ATAPI_TRACK_POINT_LEADOUT 0xAA

#define ATAPI_GESN_MEDIA        4
#define ATAPI_GESN_NO_EVENT     0x80
#define ATAPI_MEC_NO_CHANGE     0
#define ATAPI_MEC_NEW_MEDIA     2
#define ATAPI_MEC_MEDIA_REMOVAL 3

#define ATAPI_MMC_PROFILE_CD_ROM 0x0008

#define ATAPI_SUBCHANNEL_STATUS_DATA_ONLY 0x15

static uint32_t atapi_cdrom_be32(const uint8_t* buffer);

static const char* atapi_cdrom_opcode_name(uint8_t opcode)
{
	switch (opcode) {
	case ATAPI_GPCMD_TEST_UNIT_READY:
		return "TEST UNIT READY";
	case ATAPI_GPCMD_REQUEST_SENSE:
		return "REQUEST SENSE";
	case ATAPI_GPCMD_READ_6:
		return "READ(6)";
	case ATAPI_GPCMD_SEEK_6:
		return "SEEK(6)";
	case ATAPI_GPCMD_INQUIRY:
		return "INQUIRY";
	case ATAPI_GPCMD_MODE_SENSE_6:
		return "MODE SENSE(6)";
	case ATAPI_GPCMD_START_STOP_UNIT:
		return "START STOP UNIT";
	case ATAPI_GPCMD_PREVENT_REMOVAL:
		return "PREVENT/ALLOW";
	case ATAPI_GPCMD_READ_CAPACITY:
		return "READ CAPACITY";
	case ATAPI_GPCMD_READ_10:
		return "READ(10)";
	case ATAPI_GPCMD_SEEK_10:
		return "SEEK(10)";
	case ATAPI_GPCMD_READ_SUBCHANNEL:
		return "READ SUBCHANNEL";
	case ATAPI_GPCMD_READ_TOC_PMA_ATIP:
		return "READ TOC/PMA/ATIP";
	case ATAPI_GPCMD_READ_HEADER:
		return "READ HEADER";
	case ATAPI_GPCMD_GET_CONFIGURATION:
		return "GET CONFIGURATION";
	case ATAPI_GPCMD_GET_EVENT_STATUS:
		return "GET EVENT STATUS";
	case ATAPI_GPCMD_READ_DISC_INFO:
		return "READ DISC INFO";
	case ATAPI_GPCMD_READ_TRACK_INFO:
		return "READ TRACK INFO";
	case ATAPI_GPCMD_MODE_SENSE_10:
		return "MODE SENSE(10)";
	case ATAPI_GPCMD_READ_12:
		return "READ(12)";
	case ATAPI_GPCMD_MECHANISM_STATUS:
		return "MECHANISM STATUS";
	default:
		return "UNKNOWN";
	}
}

static void atapi_cdrom_trace_command(const ATAPI_CDROM_t* cdrom, const uint8_t* cdb)
{
	uint8_t opcode = cdb[0];

	return;

	debug_log(DEBUG_DETAIL,
		"[ATAPI] CMD %s (0x%02X) media=%u ua=%u tray=%u prevent=%u sense=%02X/%02X/%02X CDB=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
		atapi_cdrom_opcode_name(opcode),
		opcode,
		(unsigned int) cdrom->has_media,
		(unsigned int) cdrom->unit_attention,
		(unsigned int) cdrom->tray_open,
		(unsigned int) cdrom->prevent_removal,
		(unsigned int) cdrom->sense_key,
		(unsigned int) cdrom->sense_asc,
		(unsigned int) cdrom->sense_ascq,
		(unsigned int) cdb[0],
		(unsigned int) cdb[1],
		(unsigned int) cdb[2],
		(unsigned int) cdb[3],
		(unsigned int) cdb[4],
		(unsigned int) cdb[5],
		(unsigned int) cdb[6],
		(unsigned int) cdb[7],
		(unsigned int) cdb[8],
		(unsigned int) cdb[9],
		(unsigned int) cdb[10],
		(unsigned int) cdb[11]);

	switch (opcode) {
	case ATAPI_GPCMD_MODE_SENSE_6:
	case ATAPI_GPCMD_MODE_SENSE_10:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   MODE SENSE page=0x%02X alloc=%u\n",
			(unsigned int) (cdb[2] & 0x3F),
			(unsigned int) ((opcode == ATAPI_GPCMD_MODE_SENSE_10) ? ((((uint16_t) cdb[7]) << 8) | cdb[8]) : cdb[4]));
		break;

	case ATAPI_GPCMD_READ_TOC_PMA_ATIP:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   READ TOC format=%u start=%u msf=%u alloc=%u\n",
			(unsigned int) (cdb[2] & 0x0F),
			(unsigned int) cdb[6],
			(unsigned int) ((cdb[1] >> 1) & 1),
			(unsigned int) ((((uint16_t) cdb[7]) << 8) | cdb[8]));
		break;

	case ATAPI_GPCMD_READ_6:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   READ(6) lba=%lu blocks=%lu\n",
			(unsigned long) ((((uint32_t) cdb[1] & 0x1FU) << 16) | ((uint32_t) cdb[2] << 8) | cdb[3]),
			(unsigned long) (cdb[4] ? cdb[4] : 256U));
		break;

	case ATAPI_GPCMD_READ_10:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   READ(10) lba=%lu blocks=%lu\n",
			(unsigned long) atapi_cdrom_be32(&cdb[2]),
			(unsigned long) ((((uint32_t) cdb[7]) << 8) | cdb[8]));
		break;

	case ATAPI_GPCMD_SEEK_6:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   SEEK(6) lba=%lu\n",
			(unsigned long) ((((uint32_t) cdb[2]) << 8) | cdb[3]));
		break;

	case ATAPI_GPCMD_SEEK_10:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   SEEK(10) lba=%lu\n",
			(unsigned long) atapi_cdrom_be32(&cdb[2]));
		break;

	case ATAPI_GPCMD_READ_12:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   READ(12) lba=%lu blocks=%lu\n",
			(unsigned long) atapi_cdrom_be32(&cdb[2]),
			(unsigned long) atapi_cdrom_be32(&cdb[6]));
		break;

	case ATAPI_GPCMD_READ_HEADER:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   READ HEADER lba=%lu msf=%u alloc=%u\n",
			(unsigned long) atapi_cdrom_be32(&cdb[2]),
			(unsigned int) ((cdb[1] >> 1) & 1),
			(unsigned int) ((((uint16_t) cdb[7]) << 8) | cdb[8]));
		break;

	case ATAPI_GPCMD_GET_CONFIGURATION:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   GET CONFIGURATION feature=0x%04X rt=%u alloc=%u\n",
			(unsigned int) ((((uint16_t) cdb[2]) << 8) | cdb[3]),
			(unsigned int) (cdb[1] & 0x03),
			(unsigned int) ((((uint16_t) cdb[7]) << 8) | cdb[8]));
		break;

	case ATAPI_GPCMD_GET_EVENT_STATUS:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   GET EVENT STATUS polled=%u class=0x%02X alloc=%u\n",
			(unsigned int) (cdb[1] & 0x01),
			(unsigned int) cdb[4],
			(unsigned int) ((((uint16_t) cdb[7]) << 8) | cdb[8]));
		break;

	case ATAPI_GPCMD_READ_DISC_INFO:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   READ DISC INFO alloc=%u\n",
			(unsigned int) ((((uint16_t) cdb[7]) << 8) | cdb[8]));
		break;

	case ATAPI_GPCMD_READ_TRACK_INFO:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   READ TRACK INFO type=%u addr=%lu alloc=%u\n",
			(unsigned int) (cdb[1] & 0x03),
			(unsigned long) atapi_cdrom_be32(&cdb[2]),
			(unsigned int) ((((uint16_t) cdb[7]) << 8) | cdb[8]));
		break;

	case ATAPI_GPCMD_READ_SUBCHANNEL:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   READ SUBCHANNEL format=%u msf=%u subq=%u track=%u alloc=%u\n",
			(unsigned int) cdb[3],
			(unsigned int) ((cdb[1] >> 1) & 1),
			(unsigned int) ((cdb[2] >> 6) & 1),
			(unsigned int) cdb[6],
			(unsigned int) ((((uint16_t) cdb[7]) << 8) | cdb[8]));
		break;

	case ATAPI_GPCMD_REQUEST_SENSE:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   REQUEST SENSE alloc=%u\n",
			(unsigned int) cdb[4]);
		break;

	case ATAPI_GPCMD_INQUIRY:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   INQUIRY evpd=%u alloc=%u\n",
			(unsigned int) (cdb[1] & 1),
			(unsigned int) cdb[4]);
		break;

	case ATAPI_GPCMD_START_STOP_UNIT:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   START STOP loej=%u start=%u\n",
			(unsigned int) ((cdb[4] >> 1) & 1),
			(unsigned int) (cdb[4] & 1));
		break;

	case ATAPI_GPCMD_PREVENT_REMOVAL:
		debug_log(DEBUG_DETAIL,
			"[ATAPI]   PREVENT/ALLOW prevent=%u\n",
			(unsigned int) (cdb[4] & 1));
		break;

	default:
		break;
	}
}

static void atapi_cdrom_store_be16(uint8_t* buffer, uint16_t value)
{
	buffer[0] = (uint8_t) (value >> 8);
	buffer[1] = (uint8_t) value;
}

static void atapi_cdrom_store_be32(uint8_t* buffer, uint32_t value)
{
	buffer[0] = (uint8_t) (value >> 24);
	buffer[1] = (uint8_t) (value >> 16);
	buffer[2] = (uint8_t) (value >> 8);
	buffer[3] = (uint8_t) value;
}

static uint32_t atapi_cdrom_be32(const uint8_t* buffer)
{
	return ((uint32_t) buffer[0] << 24) |
		((uint32_t) buffer[1] << 16) |
		((uint32_t) buffer[2] << 8) |
		(uint32_t) buffer[3];
}

static void atapi_cdrom_lba_to_msf(uint32_t lba, uint8_t* minute, uint8_t* second, uint8_t* frame)
{
	uint32_t msf_lba = lba + 150U;

	if (minute != NULL) {
		*minute = (uint8_t) ((msf_lba / (75U * 60U)) & 0xFF);
	}
	if (second != NULL) {
		*second = (uint8_t) ((msf_lba / 75U) % 60U);
	}
	if (frame != NULL) {
		*frame = (uint8_t) (msf_lba % 75U);
	}
}

static void atapi_cdrom_clear_sense(ATAPI_CDROM_t* cdrom)
{
	cdrom->sense_key = ATAPI_SENSE_NONE;
	cdrom->sense_asc = ATAPI_ASC_NONE;
	cdrom->sense_ascq = ATAPI_ASCQ_NONE;
}

static void atapi_cdrom_set_sense(ATAPI_CDROM_t* cdrom, uint8_t key, uint8_t asc, uint8_t ascq)
{
	cdrom->sense_key = key;
	cdrom->sense_asc = asc;
	cdrom->sense_ascq = ascq;
}

static void atapi_cdrom_mark_media_change(ATAPI_CDROM_t* cdrom)
{
	cdrom->unit_attention = 1;
}

static void atapi_cdrom_release_media(ATAPI_CDROM_t* cdrom, uint8_t clear_path)
{
	if (cdrom->image != NULL) {
		fclose(cdrom->image);
		cdrom->image = NULL;
	}

	cdrom->total_blocks = 0;
	cdrom->has_media = 0;
	if (clear_path) {
		cdrom->path[0] = 0;
	}
}

static int atapi_cdrom_ensure_buffer(uint8_t** buffer, size_t* capacity, size_t length)
{
	uint8_t* new_buffer;

	if (length == 0) {
		return 0;
	}
	if (*capacity >= length) {
		return 0;
	}

	new_buffer = (uint8_t*) realloc(*buffer, length);
	if (new_buffer == NULL) {
		return -1;
	}

	*buffer = new_buffer;
	*capacity = length;
	return 0;
}

static int atapi_cdrom_copy_response(uint8_t** buffer,
	size_t* buffer_len,
	size_t* buffer_capacity,
	const uint8_t* response,
	size_t response_len,
	size_t alloc_length)
{
	size_t length = response_len;

	if (alloc_length < length) {
		length = alloc_length;
	}
	if (length == 0) {
		*buffer_len = 0;
		return 0;
	}
	if (atapi_cdrom_ensure_buffer(buffer, buffer_capacity, length) != 0) {
		return -1;
	}

	memcpy(*buffer, response, length);
	*buffer_len = length;
	return 0;
}

static size_t atapi_cdrom_append_feature(uint8_t* response,
	size_t offset,
	uint16_t feature_code,
	uint8_t flags,
	uint8_t additional_length,
	const uint8_t* payload,
	size_t payload_len)
{
	atapi_cdrom_store_be16(&response[offset], feature_code);
	response[offset + 2] = flags;
	response[offset + 3] = additional_length;
	if ((payload != NULL) && (payload_len != 0)) {
		memcpy(&response[offset + 4], payload, payload_len);
	}

	return offset + 4 + additional_length;
}

static int atapi_cdrom_command_requires_media(uint8_t opcode)
{
	switch (opcode) {
	case ATAPI_GPCMD_TEST_UNIT_READY:
	case ATAPI_GPCMD_READ_CAPACITY:
	case ATAPI_GPCMD_READ_6:
	case ATAPI_GPCMD_READ_10:
	case ATAPI_GPCMD_READ_12:
	case ATAPI_GPCMD_SEEK_6:
	case ATAPI_GPCMD_SEEK_10:
	case ATAPI_GPCMD_READ_SUBCHANNEL:
	case ATAPI_GPCMD_READ_TOC_PMA_ATIP:
	case ATAPI_GPCMD_READ_HEADER:
	case ATAPI_GPCMD_READ_DISC_INFO:
	case ATAPI_GPCMD_READ_TRACK_INFO:
		return 1;
	default:
		return 0;
	}
}

static int atapi_cdrom_command_allows_unit_attention(uint8_t opcode)
{
	switch (opcode) {
	case ATAPI_GPCMD_REQUEST_SENSE:
	case ATAPI_GPCMD_INQUIRY:
	case ATAPI_GPCMD_GET_CONFIGURATION:
	case ATAPI_GPCMD_GET_EVENT_STATUS:
		return 1;
	default:
		return 0;
	}
}

static int atapi_cdrom_prepare_request_sense(ATAPI_CDROM_t* cdrom, uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, uint8_t alloc_length)
{
	uint8_t key = cdrom->sense_key;
	uint8_t asc = cdrom->sense_asc;
	uint8_t ascq = cdrom->sense_ascq;
	size_t length = 18;

	if ((key == ATAPI_SENSE_NONE) && cdrom->unit_attention) {
		key = ATAPI_SENSE_UNIT_ATTENTION;
		asc = ATAPI_ASC_MEDIUM_MAY_HAVE_CHANGED;
		ascq = ATAPI_ASCQ_NONE;
	}

	if (alloc_length < length) {
		length = alloc_length;
	}
	if (atapi_cdrom_ensure_buffer(buffer, buffer_capacity, length) != 0) {
		return -1;
	}

	if (length != 0) {
		memset(*buffer, 0, length);
		(*buffer)[0] = 0x70;
		if (length > 2) {
			(*buffer)[2] = key;
		}
		if (length > 7) {
			(*buffer)[7] = 10;
		}
		if (length > 12) {
			(*buffer)[12] = asc;
		}
		if (length > 13) {
			(*buffer)[13] = ascq;
		}
	}

	*buffer_len = length;
	if (key == ATAPI_SENSE_UNIT_ATTENTION) {
		cdrom->unit_attention = 0;
	}
	atapi_cdrom_clear_sense(cdrom);
	return 0;
}

static int atapi_cdrom_prepare_inquiry(uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, uint8_t alloc_length)
{
	size_t length = 36;

	if (alloc_length < length) {
		length = alloc_length;
	}
	if (length == 0) {
		*buffer_len = 0;
		return 0;
	}
	if (atapi_cdrom_ensure_buffer(buffer, buffer_capacity, length) != 0) {
		return -1;
	}

	memset(*buffer, 0, length);
	if (length > 0) {
		(*buffer)[0] = 0x05;
	}
	if (length > 1) {
		(*buffer)[1] = 0x80;
	}
	if (length > 2) {
		(*buffer)[2] = 0x05;
	}
	if (length > 3) {
		(*buffer)[3] = 0x02;
	}
	if (length > 4) {
		(*buffer)[4] = 31;
	}
	if (length > 8) {
		memcpy(&(*buffer)[8], "PCULATOR", (length - 8 < 8) ? length - 8 : 8);
	}
	if (length > 16) {
		memcpy(&(*buffer)[16], "ATAPI CD-ROM    ", (length - 16 < 16) ? length - 16 : 16);
	}
	if (length > 32) {
		memcpy(&(*buffer)[32], "0001", (length - 32 < 4) ? length - 32 : 4);
	}

	*buffer_len = length;
	return 0;
}

static int atapi_cdrom_prepare_mode_sense(uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, const uint8_t* cdb)
{
	uint8_t is_ten = (cdb[0] == ATAPI_GPCMD_MODE_SENSE_10);
	uint8_t page_code = cdb[2] & 0x3F;
	uint16_t alloc_length = is_ten ? (((uint16_t) cdb[7] << 8) | cdb[8]) : cdb[4];
	size_t length = is_ten ? 32 : 28;

	if ((page_code != 0x00) && (page_code != 0x2A) && (page_code != 0x3F)) {
		return -1;
	}

	if (alloc_length < length) {
		length = alloc_length;
	}
	if (length == 0) {
		*buffer_len = 0;
		return 0;
	}
	if (atapi_cdrom_ensure_buffer(buffer, buffer_capacity, length) != 0) {
		return -1;
	}

	memset(*buffer, 0, length);
	if (is_ten) {
		if (length > 1) {
			atapi_cdrom_store_be16(*buffer, (uint16_t) (32 - 2));
		}
		if (length > 8) {
			(*buffer)[8] = 0x2A;
		}
		if (length > 9) {
			(*buffer)[9] = 0x12;
		}
		if (length > 10) {
			(*buffer)[10] = 0x03;
		}
		if (length > 12) {
			(*buffer)[12] = 0x71;
		}
		if (length > 13) {
			(*buffer)[13] = 0x00;
		}
		if (length > 14) {
			(*buffer)[14] = 0x02;
		}
	} else {
		if (length > 0) {
			(*buffer)[0] = (uint8_t) (28 - 1);
		}
		if (length > 4) {
			(*buffer)[4] = 0x2A;
		}
		if (length > 5) {
			(*buffer)[5] = 0x12;
		}
		if (length > 6) {
			(*buffer)[6] = 0x03;
		}
		if (length > 8) {
			(*buffer)[8] = 0x71;
		}
		if (length > 9) {
			(*buffer)[9] = 0x00;
		}
		if (length > 10) {
			(*buffer)[10] = 0x02;
		}
	}

	*buffer_len = length;
	return 0;
}

static int atapi_cdrom_prepare_capacity(const ATAPI_CDROM_t* cdrom, uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity)
{
	if (atapi_cdrom_ensure_buffer(buffer, buffer_capacity, 8) != 0) {
		return -1;
	}

	memset(*buffer, 0, 8);
	atapi_cdrom_store_be32(*buffer, (uint32_t) (cdrom->total_blocks - 1));
	atapi_cdrom_store_be32(*buffer + 4, cdrom->block_size);
	*buffer_len = 8;
	return 0;
}

static void atapi_cdrom_build_toc_descriptor(uint8_t* descriptor, uint8_t track_number, uint32_t lba, uint8_t msf)
{
	uint8_t minute;
	uint8_t second;
	uint8_t frame;

	memset(descriptor, 0, 8);
	descriptor[1] = ATAPI_TRACK_CONTROL_DATA;
	descriptor[2] = track_number;
	if (msf) {
		atapi_cdrom_lba_to_msf(lba, &minute, &second, &frame);
		descriptor[4] = 0;
		descriptor[5] = minute;
		descriptor[6] = second;
		descriptor[7] = frame;
	} else {
		atapi_cdrom_store_be32(&descriptor[4], lba);
	}
}

static void atapi_cdrom_build_raw_toc_entry(uint8_t* entry, uint8_t point, uint8_t pm, uint8_t ps, uint8_t pf)
{
	memset(entry, 0, 11);
	entry[0] = 1;
	entry[1] = ATAPI_TRACK_CONTROL_DATA;
	entry[3] = point;
	entry[8] = pm;
	entry[9] = ps;
	entry[10] = pf;
}

static int atapi_cdrom_prepare_toc(const ATAPI_CDROM_t* cdrom, uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, const uint8_t* cdb)
{
	uint8_t response[64];
	uint8_t format = cdb[2] & 0x0F;
	uint8_t msf = cdb[1] & 0x02;
	uint8_t start_track = cdb[6];
	uint16_t alloc_length = ((uint16_t) cdb[7] << 8) | cdb[8];
	uint32_t leadout = (uint32_t) cdrom->total_blocks;
	size_t offset = 4;
	size_t total_length;
	uint8_t minute;
	uint8_t second;
	uint8_t frame;

	if (format == ATAPI_TOC_FORMAT_NORMAL) {
		format = (cdb[9] >> 6) & 0x03;
	}

	memset(response, 0, sizeof(response));
	response[2] = 1;
	response[3] = 1;

	switch (format) {
	case ATAPI_TOC_FORMAT_NORMAL:
		if ((start_track == 0) || (start_track <= 1)) {
			atapi_cdrom_build_toc_descriptor(&response[offset], 1, 0, msf);
			offset += 8;
		}
		if ((start_track == 0) || (start_track <= 1) || (start_track == ATAPI_TRACK_POINT_LEADOUT)) {
			atapi_cdrom_build_toc_descriptor(&response[offset], ATAPI_TRACK_POINT_LEADOUT, leadout, msf);
			offset += 8;
		}
		break;

	case ATAPI_TOC_FORMAT_SESSION:
		atapi_cdrom_build_toc_descriptor(&response[offset], 1, 0, msf);
		offset += 8;
		break;

	case ATAPI_TOC_FORMAT_RAW:
		if (start_track <= 1) {
			atapi_cdrom_lba_to_msf(0, &minute, &second, &frame);
			atapi_cdrom_build_raw_toc_entry(&response[offset], 0xA0, 1, 0, 0);
			offset += 11;
			atapi_cdrom_build_raw_toc_entry(&response[offset], 0xA1, 1, 0, 0);
			offset += 11;
			atapi_cdrom_build_raw_toc_entry(&response[offset], 1, minute, second, frame);
			offset += 11;
			atapi_cdrom_lba_to_msf(leadout, &minute, &second, &frame);
			atapi_cdrom_build_raw_toc_entry(&response[offset], 0xA2, minute, second, frame);
			offset += 11;
		}
		break;

	default:
		return -1;
	}

	total_length = offset;
	atapi_cdrom_store_be16(response, (uint16_t) (total_length - 2));
	return atapi_cdrom_copy_response(buffer, buffer_len, buffer_capacity, response, total_length, alloc_length);
}

static int atapi_cdrom_prepare_read(ATAPI_CDROM_t* cdrom, uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, const uint8_t* cdb)
{
	uint32_t lba;
	uint32_t blocks;
	size_t data_len;

	switch (cdb[0]) {
	case ATAPI_GPCMD_READ_6:
		lba = (((uint32_t) cdb[1] & 0x1FU) << 16) | ((uint32_t) cdb[2] << 8) | cdb[3];
		blocks = cdb[4];
		if (blocks == 0) {
			blocks = 256;
		}
		break;

	case ATAPI_GPCMD_READ_10:
		lba = atapi_cdrom_be32(&cdb[2]);
		blocks = ((uint32_t) cdb[7] << 8) | cdb[8];
		break;

	default:
		lba = atapi_cdrom_be32(&cdb[2]);
		blocks = atapi_cdrom_be32(&cdb[6]);
		break;
	}

	if (blocks == 0) {
		*buffer_len = 0;
		return 0;
	}
	if ((lba >= cdrom->total_blocks) || (((uint64_t) lba + blocks) > cdrom->total_blocks)) {
		atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_LBA_OUT_OF_RANGE, ATAPI_ASCQ_NONE);
		return -1;
	}

	data_len = (size_t) blocks * (size_t) cdrom->block_size;
	if (data_len > ATAPI_MAX_TRANSFER_BYTES) {
		atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
		return -1;
	}
	if (atapi_cdrom_ensure_buffer(buffer, buffer_capacity, data_len) != 0) {
		return -1;
	}
	if (host_fseek64(cdrom->image, (uint64_t) lba * cdrom->block_size, SEEK_SET) != 0) {
		atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_MEDIUM_ERROR, ATAPI_ASC_UNRECOVERED_READ_ERROR, ATAPI_ASCQ_NONE);
		return -1;
	}
	if (fread(*buffer, 1, data_len, cdrom->image) != data_len) {
		atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_MEDIUM_ERROR, ATAPI_ASC_UNRECOVERED_READ_ERROR, ATAPI_ASCQ_NONE);
		return -1;
	}

	*buffer_len = data_len;
	return 0;
}

static int atapi_cdrom_prepare_seek(const ATAPI_CDROM_t* cdrom, const uint8_t* cdb)
{
	uint32_t lba;

	switch (cdb[0]) {
	case ATAPI_GPCMD_SEEK_6:
		lba = ((uint32_t) cdb[2] << 8) | cdb[3];
		break;

	default:
		lba = atapi_cdrom_be32(&cdb[2]);
		break;
	}

	if (lba >= cdrom->total_blocks) {
		return -1;
	}

	return 0;
}

static int atapi_cdrom_prepare_read_header(const ATAPI_CDROM_t* cdrom, uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, const uint8_t* cdb)
{
	uint8_t response[8];
	uint16_t alloc_length = ((uint16_t) cdb[7] << 8) | cdb[8];
	uint32_t lba = atapi_cdrom_be32(&cdb[2]);
	uint8_t minute;
	uint8_t second;
	uint8_t frame;

	(void) cdrom;

	memset(response, 0, sizeof(response));
	response[0] = 0x01;
	if (cdb[1] & 0x02) {
		atapi_cdrom_lba_to_msf(lba, &minute, &second, &frame);
		response[5] = minute;
		response[6] = second;
		response[7] = frame;
	} else {
		atapi_cdrom_store_be32(&response[4], lba);
	}

	return atapi_cdrom_copy_response(buffer, buffer_len, buffer_capacity, response, sizeof(response), alloc_length);
}

static int atapi_cdrom_prepare_mechanism_status(uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, const uint8_t* cdb)
{
	uint8_t response[8];
	size_t alloc_length = ((size_t) cdb[7] << 16) | ((size_t) cdb[8] << 8) | cdb[9];

	memset(response, 0, sizeof(response));
	response[5] = 1;
	return atapi_cdrom_copy_response(buffer, buffer_len, buffer_capacity, response, sizeof(response), alloc_length);
}

static int atapi_cdrom_prepare_get_configuration(const ATAPI_CDROM_t* cdrom, uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, const uint8_t* cdb)
{
	uint8_t response[128];
	uint16_t feature = ((uint16_t) cdb[2] << 8) | cdb[3];
	uint16_t alloc_length = ((uint16_t) cdb[7] << 8) | cdb[8];
	uint8_t request_type = cdb[1] & 0x03;
	size_t offset = 8;
	uint8_t current_profile_payload[4];
	uint8_t core_payload[8];
	uint8_t morphing_payload[4];
	uint8_t removable_payload[4];
	uint8_t random_read_payload[8];
	uint8_t multi_read_payload[1];
	uint8_t cd_read_payload[4];
	uint8_t status_payload[4];

	if ((feature > 3) && (feature != 0x0010) && (feature != 0x001D) &&
		(feature != 0x001E) && (feature != 0x0103)) {
		return -1;
	}

	memset(response, 0, sizeof(response));
	if (cdrom->has_media) {
		atapi_cdrom_store_be16(&response[6], ATAPI_MMC_PROFILE_CD_ROM);
	}

	memset(current_profile_payload, 0, sizeof(current_profile_payload));
	memset(core_payload, 0, sizeof(core_payload));
	memset(morphing_payload, 0, sizeof(morphing_payload));
	memset(removable_payload, 0, sizeof(removable_payload));
	memset(random_read_payload, 0, sizeof(random_read_payload));
	memset(multi_read_payload, 0, sizeof(multi_read_payload));
	memset(cd_read_payload, 0, sizeof(cd_read_payload));
	memset(status_payload, 0, sizeof(status_payload));

	current_profile_payload[0] = (uint8_t) (ATAPI_MMC_PROFILE_CD_ROM >> 8);
	current_profile_payload[1] = (uint8_t) ATAPI_MMC_PROFILE_CD_ROM;
	if (cdrom->has_media) {
		current_profile_payload[2] = 1;
	}

	core_payload[3] = 2;
	core_payload[4] = 1;

	morphing_payload[0] = 2;
	removable_payload[0] = 0x0D | 0x20;
	random_read_payload[2] = 8;
	random_read_payload[5] = 0x10;
	cd_read_payload[0] = 0;
	status_payload[0] = 7;
	status_payload[2] = 1;

	if ((feature == 0) || (request_type < 2)) {
		offset = atapi_cdrom_append_feature(response, offset, 0x0000, 0x03, 4, current_profile_payload, sizeof(current_profile_payload));
	}
	if ((feature == 1) || (request_type < 2)) {
		offset = atapi_cdrom_append_feature(response, offset, 0x0001, 0x0B, 8, core_payload, 8);
	}
	if ((feature == 2) || (request_type < 2)) {
		offset = atapi_cdrom_append_feature(response, offset, 0x0002, 0x07, 4, morphing_payload, 4);
	}
	if ((feature == 3) || (request_type < 2)) {
		offset = atapi_cdrom_append_feature(response, offset, 0x0003, 0x03, 4, removable_payload, 4);
	}
	if ((feature == 0x0010) || (request_type < 2)) {
		offset = atapi_cdrom_append_feature(response, offset, 0x0010, 0x03, 8, random_read_payload, 8);
	}
	if ((feature == 0x001D) || (request_type < 2)) {
		offset = atapi_cdrom_append_feature(response, offset, 0x001D, 0x03, 0, multi_read_payload, 0);
	}
	if ((feature == 0x001E) || (request_type < 2)) {
		offset = atapi_cdrom_append_feature(response, offset, 0x001E, 0x0B, 4, cd_read_payload, 4);
	}
	if ((feature == 0x0103) || (request_type < 2)) {
		offset = atapi_cdrom_append_feature(response, offset, 0x0103, 0x03, 4, status_payload, 4);
	}

	atapi_cdrom_store_be32(response, (uint32_t) (offset - 4));
	return atapi_cdrom_copy_response(buffer, buffer_len, buffer_capacity, response, offset, alloc_length);
}

static int atapi_cdrom_prepare_get_event_status(const ATAPI_CDROM_t* cdrom, uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, const uint8_t* cdb)
{
	uint8_t response[8];
	size_t response_length = 4;

	(void) cdrom;

	if (!(cdb[1] & 0x01)) {
		return -1;
	}

	memset(response, 0, sizeof(response));
	response[2] = ATAPI_GESN_NO_EVENT;
	response[3] = (uint8_t) (1U << ATAPI_GESN_MEDIA);
	if (cdb[4] & (1U << ATAPI_GESN_MEDIA)) {
		response_length = 8;
		response[2] = ATAPI_GESN_MEDIA;
		response[4] = cdrom->has_media ? (cdrom->unit_attention ? ATAPI_MEC_NEW_MEDIA : ATAPI_MEC_NO_CHANGE) : ATAPI_MEC_MEDIA_REMOVAL;
		response[5] = 1;
	}
	atapi_cdrom_store_be16(response, (uint16_t) (response_length - 4));
	return atapi_cdrom_copy_response(buffer, buffer_len, buffer_capacity, response, response_length, ((uint16_t) cdb[7] << 8) | cdb[8]);
}

static int atapi_cdrom_prepare_disc_info(const ATAPI_CDROM_t* cdrom, uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, const uint8_t* cdb)
{
	uint8_t response[34];
	uint8_t minute;
	uint8_t second;
	uint8_t frame;

	memset(response, 0, sizeof(response));
	response[1] = 0x20;
	response[2] = 0x0E;
	response[3] = 1;
	response[4] = 1;
	response[5] = 1;
	response[6] = 1;
	response[7] = 0x20;
	atapi_cdrom_lba_to_msf((uint32_t) cdrom->total_blocks, &minute, &second, &frame);
	response[20] = minute;
	response[21] = second;
	response[22] = frame;
	return atapi_cdrom_copy_response(buffer, buffer_len, buffer_capacity, response, sizeof(response), ((uint16_t) cdb[7] << 8) | cdb[8]);
}

static int atapi_cdrom_prepare_track_info(const ATAPI_CDROM_t* cdrom, uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, const uint8_t* cdb)
{
	uint8_t response[36];
	uint32_t pos = atapi_cdrom_be32(&cdb[2]);
	uint8_t address_type = cdb[1] & 0x03;
	uint8_t track_number = 1;
	uint8_t session_number = 1;
	uint32_t track_start = 0;
	uint32_t track_length = (uint32_t) cdrom->total_blocks;

	switch (address_type) {
	case 0x00:
		if (pos >= cdrom->total_blocks) {
			return -1;
		}
		break;

	case 0x01:
		if (pos == 0x00) {
			track_number = 0;
			session_number = 0;
			track_length = 0;
		} else if (pos == ATAPI_TRACK_POINT_LEADOUT) {
			track_number = ATAPI_TRACK_POINT_LEADOUT;
			track_start = (uint32_t) cdrom->total_blocks;
			track_length = 0;
		} else if (pos != 1) {
			return -1;
		}
		break;

	case 0x02:
		if (pos != 1) {
			return -1;
		}
		break;

	default:
		return -1;
	}

	memset(response, 0, sizeof(response));
	response[1] = 0x22;
	response[2] = track_number;
	response[3] = session_number;
	if (track_number == 1) {
		response[5] = 0x04;
		response[6] = 0x01;
	}
	atapi_cdrom_store_be32(&response[8], track_start);
	atapi_cdrom_store_be32(&response[24], track_length);
	return atapi_cdrom_copy_response(buffer, buffer_len, buffer_capacity, response, sizeof(response), ((uint16_t) cdb[7] << 8) | cdb[8]);
}

static int atapi_cdrom_prepare_subchannel(const ATAPI_CDROM_t* cdrom, uint8_t** buffer, size_t* buffer_len, size_t* buffer_capacity, const uint8_t* cdb)
{
	uint8_t response[24];
	uint8_t format = cdb[3];
	uint8_t msf = (cdb[1] >> 1) & 1;
	uint16_t alloc_length = ((uint16_t) cdb[7] << 8) | cdb[8];
	size_t response_length = 4;
	uint8_t minute;
	uint8_t second;
	uint8_t frame;

	(void) cdrom;

	if ((format > 3) || ((format != 3) && (cdb[6] != 0))) {
		return -1;
	}

	memset(response, 0, sizeof(response));
	response[1] = ATAPI_SUBCHANNEL_STATUS_DATA_ONLY;
	if (cdb[2] & 0x40) {
		switch (format) {
		case 0:
			response_length = 4;
			break;

		case 1:
			response_length = 16;
			response[4] = format;
			response[5] = ATAPI_TRACK_CONTROL_DATA;
			response[6] = 1;
			response[7] = 1;
			if (msf) {
				atapi_cdrom_lba_to_msf(0, &minute, &second, &frame);
				response[8] = 0;
				response[9] = minute;
				response[10] = second;
				response[11] = frame;
				response[12] = 0;
				response[13] = 0;
				response[14] = 0;
				response[15] = 0;
			} else {
				atapi_cdrom_store_be32(&response[8], 0);
				atapi_cdrom_store_be32(&response[12], 0);
			}
			break;

		default:
			response_length = 24;
			response[4] = format;
			break;
		}
		response[3] = (uint8_t) (response_length - 4);
	}

	return atapi_cdrom_copy_response(buffer, buffer_len, buffer_capacity, response, response_length, alloc_length);
}

void atapi_cdrom_init(ATAPI_CDROM_t* cdrom)
{
	if (cdrom == NULL) {
		return;
	}

	memset(cdrom, 0, sizeof(*cdrom));
	cdrom->block_size = ATAPI_CDROM_BLOCK_SIZE;
}

void atapi_cdrom_reset(ATAPI_CDROM_t* cdrom)
{
	if (cdrom == NULL) {
		return;
	}

	atapi_cdrom_clear_sense(cdrom);
	cdrom->prevent_removal = 0;
}

void atapi_cdrom_close(ATAPI_CDROM_t* cdrom)
{
	if (cdrom == NULL) {
		return;
	}

	atapi_cdrom_release_media(cdrom, 1);
	cdrom->tray_open = 0;
	cdrom->prevent_removal = 0;
	cdrom->unit_attention = 0;
	atapi_cdrom_clear_sense(cdrom);
}

int atapi_cdrom_attach(ATAPI_CDROM_t* cdrom, const char* path)
{
	FILE* image;
	uint64_t size_bytes;

	if (cdrom == NULL) {
		return -1;
	}

	if ((path == NULL) || (*path == 0) || ((path[0] == '.') && (path[1] == 0))) {
		atapi_cdrom_release_media(cdrom, 1);
		cdrom->tray_open = 0;
		atapi_cdrom_mark_media_change(cdrom);
		atapi_cdrom_clear_sense(cdrom);
		return 0;
	}

	image = fopen(path, "rb");
	if (image == NULL) {
		return -1;
	}
	if (host_fseek64(image, 0, SEEK_END) != 0) {
		fclose(image);
		return -1;
	}
	size_bytes = host_ftell64(image);
	if ((size_bytes == UINT64_MAX) || ((size_bytes % ATAPI_CDROM_BLOCK_SIZE) != 0)) {
		fclose(image);
		return -1;
	}
	if (host_fseek64(image, 0, SEEK_SET) != 0) {
		fclose(image);
		return -1;
	}

	atapi_cdrom_release_media(cdrom, 0);
	cdrom->image = image;
	cdrom->total_blocks = size_bytes / ATAPI_CDROM_BLOCK_SIZE;
	cdrom->has_media = 1;
	cdrom->tray_open = 0;
	strncpy(cdrom->path, path, sizeof(cdrom->path) - 1);
	cdrom->path[sizeof(cdrom->path) - 1] = 0;
	atapi_cdrom_mark_media_change(cdrom);
	atapi_cdrom_clear_sense(cdrom);

	debug_log(DEBUG_INFO, "[ATAPI] Inserted CD-ROM image: %s\n", path);
	return 0;
}

int atapi_cdrom_reload(ATAPI_CDROM_t* cdrom)
{
	if ((cdrom == NULL) || (cdrom->path[0] == 0)) {
		return -1;
	}
	return atapi_cdrom_attach(cdrom, cdrom->path);
}

int atapi_cdrom_eject(ATAPI_CDROM_t* cdrom)
{
	if (cdrom == NULL) {
		return -1;
	}

	atapi_cdrom_release_media(cdrom, 0);
	cdrom->tray_open = 1;
	atapi_cdrom_mark_media_change(cdrom);
	atapi_cdrom_clear_sense(cdrom);
	return 0;
}

const char* atapi_cdrom_get_path(const ATAPI_CDROM_t* cdrom)
{
	if ((cdrom == NULL) || (cdrom->path[0] == 0)) {
		return NULL;
	}

	return cdrom->path;
}

ATAPI_CDROM_CMD_RESULT_t atapi_cdrom_command(ATAPI_CDROM_t* cdrom,
	const uint8_t cdb[ATAPI_CDROM_CDB_SIZE],
	uint8_t** buffer,
	size_t* buffer_len,
	size_t* buffer_capacity)
{
	uint8_t opcode;

	if ((cdrom == NULL) || (cdb == NULL) || (buffer == NULL) || (buffer_len == NULL) || (buffer_capacity == NULL)) {
		return ATAPI_CDROM_CMD_ERROR;
	}

	opcode = cdb[0];
	*buffer_len = 0;
	atapi_cdrom_trace_command(cdrom, cdb);

	/*
	 * Mirror 86Box's two-step unit-attention handling so a BIOS that retries
	 * a failed command without issuing REQUEST SENSE can still make progress.
	 */
	if (!cdrom->has_media && cdrom->unit_attention) {
		cdrom->unit_attention = 0;
	}

	if (cdrom->unit_attention == 1) {
		if (!atapi_cdrom_command_allows_unit_attention(opcode)) {
			cdrom->unit_attention = 2;
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_UNIT_ATTENTION, ATAPI_ASC_MEDIUM_MAY_HAVE_CHANGED, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
	}
	else if ((cdrom->unit_attention == 2) && (opcode != ATAPI_GPCMD_REQUEST_SENSE)) {
		cdrom->unit_attention = 0;
	}

	if (opcode != ATAPI_GPCMD_REQUEST_SENSE) {
		atapi_cdrom_clear_sense(cdrom);
	}

	if (!cdrom->has_media && atapi_cdrom_command_requires_media(opcode)) {
		atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_NOT_READY, ATAPI_ASC_MEDIUM_NOT_PRESENT, ATAPI_ASCQ_NONE);
		return ATAPI_CDROM_CMD_ERROR;
	}

	switch (opcode) {
	case ATAPI_GPCMD_TEST_UNIT_READY:
		return ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_REQUEST_SENSE:
		if (atapi_cdrom_prepare_request_sense(cdrom, buffer, buffer_len, buffer_capacity, cdb[4]) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return ATAPI_CDROM_CMD_DATA_IN;

	case ATAPI_GPCMD_INQUIRY:
		if (cdb[1] & 1) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		if (atapi_cdrom_prepare_inquiry(buffer, buffer_len, buffer_capacity, cdb[4]) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return ATAPI_CDROM_CMD_DATA_IN;

	case ATAPI_GPCMD_MODE_SENSE_6:
	case ATAPI_GPCMD_MODE_SENSE_10:
		if (atapi_cdrom_prepare_mode_sense(buffer, buffer_len, buffer_capacity, cdb) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return ATAPI_CDROM_CMD_DATA_IN;

	case ATAPI_GPCMD_READ_CAPACITY:
		if (atapi_cdrom_prepare_capacity(cdrom, buffer, buffer_len, buffer_capacity) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return ATAPI_CDROM_CMD_DATA_IN;

	case ATAPI_GPCMD_MECHANISM_STATUS:
		if (atapi_cdrom_prepare_mechanism_status(buffer, buffer_len, buffer_capacity, cdb) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return (*buffer_len != 0) ? ATAPI_CDROM_CMD_DATA_IN : ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_READ_TOC_PMA_ATIP:
		if (atapi_cdrom_prepare_toc(cdrom, buffer, buffer_len, buffer_capacity, cdb) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return ATAPI_CDROM_CMD_DATA_IN;

	case ATAPI_GPCMD_READ_6:
	case ATAPI_GPCMD_READ_10:
	case ATAPI_GPCMD_READ_12:
		if (atapi_cdrom_prepare_read(cdrom, buffer, buffer_len, buffer_capacity, cdb) != 0) {
			return ATAPI_CDROM_CMD_ERROR;
		}
		return (*buffer_len != 0) ? ATAPI_CDROM_CMD_DATA_IN : ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_SEEK_6:
	case ATAPI_GPCMD_SEEK_10:
		if (atapi_cdrom_prepare_seek(cdrom, cdb) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_LBA_OUT_OF_RANGE, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_READ_SUBCHANNEL:
		if (atapi_cdrom_prepare_subchannel(cdrom, buffer, buffer_len, buffer_capacity, cdb) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return (*buffer_len != 0) ? ATAPI_CDROM_CMD_DATA_IN : ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_READ_HEADER:
		if (atapi_cdrom_prepare_read_header(cdrom, buffer, buffer_len, buffer_capacity, cdb) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return (*buffer_len != 0) ? ATAPI_CDROM_CMD_DATA_IN : ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_GET_CONFIGURATION:
		if (atapi_cdrom_prepare_get_configuration(cdrom, buffer, buffer_len, buffer_capacity, cdb) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return (*buffer_len != 0) ? ATAPI_CDROM_CMD_DATA_IN : ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_GET_EVENT_STATUS:
		if (atapi_cdrom_prepare_get_event_status(cdrom, buffer, buffer_len, buffer_capacity, cdb) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return (*buffer_len != 0) ? ATAPI_CDROM_CMD_DATA_IN : ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_READ_DISC_INFO:
		if (atapi_cdrom_prepare_disc_info(cdrom, buffer, buffer_len, buffer_capacity, cdb) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return (*buffer_len != 0) ? ATAPI_CDROM_CMD_DATA_IN : ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_READ_TRACK_INFO:
		if (atapi_cdrom_prepare_track_info(cdrom, buffer, buffer_len, buffer_capacity, cdb) != 0) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return (*buffer_len != 0) ? ATAPI_CDROM_CMD_DATA_IN : ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_START_STOP_UNIT:
		if (cdb[4] & 0x02) {
			if (cdb[4] & 0x01) {
				if (atapi_cdrom_reload(cdrom) != 0) {
					atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_NOT_READY, ATAPI_ASC_MEDIUM_NOT_PRESENT, ATAPI_ASCQ_NONE);
					return ATAPI_CDROM_CMD_ERROR;
				}
			} else {
				if (cdrom->prevent_removal) {
					atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
					return ATAPI_CDROM_CMD_ERROR;
				}
				if (atapi_cdrom_eject(cdrom) != 0) {
					atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_INV_FIELD_IN_CMD_PACKET, ATAPI_ASCQ_NONE);
					return ATAPI_CDROM_CMD_ERROR;
				}
			}
		} else if ((cdb[4] & 0x01) && cdrom->tray_open && (atapi_cdrom_reload(cdrom) != 0)) {
			atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_NOT_READY, ATAPI_ASC_MEDIUM_NOT_PRESENT, ATAPI_ASCQ_NONE);
			return ATAPI_CDROM_CMD_ERROR;
		}
		return ATAPI_CDROM_CMD_OK;

	case ATAPI_GPCMD_PREVENT_REMOVAL:
		cdrom->prevent_removal = cdb[4] & 1;
		return ATAPI_CDROM_CMD_OK;

	default:
		debug_log(DEBUG_DETAIL, "[ATAPI] Unsupported packet command 0x%02X\n", opcode);
		atapi_cdrom_set_sense(cdrom, ATAPI_SENSE_ILLEGAL_REQUEST, ATAPI_ASC_ILLEGAL_OPCODE, ATAPI_ASCQ_NONE);
		return ATAPI_CDROM_CMD_ERROR;
	}
}
