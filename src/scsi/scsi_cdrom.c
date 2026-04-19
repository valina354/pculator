#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../debuglog.h"
#include "../host/host.h"
#include "scsi_cdrom.h"

#define SCSI_CDROM_MAX_TRANSFER_BYTES (16U * 1024U * 1024U)

#define SCSI_TOC_FORMAT_NORMAL   0x00
#define SCSI_TOC_FORMAT_SESSION  0x01
#define SCSI_TOC_FORMAT_RAW      0x02

#define SCSI_TRACK_CONTROL_DATA  0x14
#define SCSI_TRACK_POINT_LEADOUT 0xaa

#define SCSI_GESN_MEDIA        4
#define SCSI_GESN_NO_EVENT     0x80
#define SCSI_MEC_NO_CHANGE     0
#define SCSI_MEC_MEDIA_REMOVAL 3

#define SCSI_MMC_PROFILE_CD_ROM 0x0008

#define SCSI_SUBCHANNEL_STATUS_DATA_ONLY 0x15

typedef struct {
    scsi_common_t common;
    FILE *image;
    uint64_t total_blocks;
    uint32_t block_size;
    char path[512];
} SCSI_CDROM_t;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static void cdrom_store_be16(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t) (value >> 8);
    buffer[1] = (uint8_t) value;
}

static void cdrom_store_be32(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t) (value >> 24);
    buffer[1] = (uint8_t) (value >> 16);
    buffer[2] = (uint8_t) (value >> 8);
    buffer[3] = (uint8_t) value;
}

static int cdrom_has_media(const SCSI_CDROM_t *cdrom)
{
    return (cdrom->image != NULL) && (cdrom->total_blocks != 0);
}

static uint32_t cdrom_read_lba(const uint8_t *cdb)
{
    switch (cdb[0]) {
    case GPCMD_READ_6:
        return ((uint32_t) (cdb[1] & 0x1f) << 16) | ((uint32_t) cdb[2] << 8) | (uint32_t) cdb[3];
    case GPCMD_READ_10:
    case GPCMD_READ_12:
        return be32(&cdb[2]);
    default:
        return 0;
    }
}

static uint32_t cdrom_read_blocks(const uint8_t *cdb)
{
    switch (cdb[0]) {
    case GPCMD_READ_6:
        return cdb[4] ? cdb[4] : 256U;
    case GPCMD_READ_10:
        return ((uint32_t) cdb[7] << 8) | (uint32_t) cdb[8];
    case GPCMD_READ_12:
        return be32(&cdb[6]);
    default:
        return 0;
    }
}

static uint32_t cdrom_seek_lba(const uint8_t *cdb)
{
    switch (cdb[0]) {
    case GPCMD_SEEK_6:
        return ((uint32_t) cdb[2] << 8) | (uint32_t) cdb[3];
    case GPCMD_SEEK_10:
        return be32(&cdb[2]);
    default:
        return 0;
    }
}

static int64_t cdrom_seek_block(FILE *image, uint64_t lba, uint32_t block_size)
{
    return host_fseek64(image, (uint64_t) lba * block_size, SEEK_SET);
}

static uint64_t cdrom_file_blocks(FILE *image, uint32_t block_size)
{
    uint64_t size;

    if (host_fseek64(image, 0, SEEK_END) != 0) {
        return 0;
    }
    size = host_ftell64(image);
    if (host_fseek64(image, 0, SEEK_SET) != 0) {
        return 0;
    }
    if ((size == UINT64_MAX) || (size == 0)) {
        return 0;
    }

    return size / block_size;
}

static void cdrom_lba_to_msf(uint32_t lba, uint8_t *minute, uint8_t *second, uint8_t *frame)
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

static int cdrom_copy_response(scsi_common_t *sc, const uint8_t *response, size_t response_len, size_t alloc_length)
{
    size_t length = response_len;

    if (alloc_length < length) {
        length = alloc_length;
    }
    if (length != 0) {
        if (scsi_common_ensure_buffer(sc, length) != 0) {
            return -1;
        }
        memcpy(sc->temp_buffer, response, length);
        sc->owner->phase = SCSI_PHASE_DATA_IN;
    } else {
        sc->owner->phase = SCSI_PHASE_STATUS;
    }

    sc->owner->buffer_length = (int32_t) length;
    return 0;
}

static int cdrom_command_requires_media(uint8_t opcode)
{
    switch (opcode) {
    case GPCMD_TEST_UNIT_READY:
    case GPCMD_READ_CDROM_CAPACITY:
    case GPCMD_READ_6:
    case GPCMD_READ_10:
    case GPCMD_READ_12:
    case GPCMD_SEEK_6:
    case GPCMD_SEEK_10:
    case GPCMD_READ_SUBCHANNEL:
    case GPCMD_READ_TOC_PMA_ATIP:
    case GPCMD_READ_HEADER:
    case GPCMD_READ_DISC_INFO:
    case GPCMD_READ_TRACK_INFO:
        return 1;
    default:
        return 0;
    }
}

static void cdrom_request_sense(scsi_common_t *sc, uint8_t *buffer, uint8_t alloc_length)
{
    uint8_t copy_len = alloc_length;

    if (copy_len > 18) {
        copy_len = 18;
    }
    memcpy(buffer, sc->sense, copy_len);
    scsi_common_clear_sense(sc);
}

static void cdrom_reset(scsi_common_t *sc)
{
    scsi_common_clear_sense(sc);
}

static int cdrom_build_inquiry(scsi_common_t *sc, uint8_t alloc_length)
{
    uint8_t response[36];

    memset(response, 0, sizeof(response));
    response[0] = 0x05;
    response[1] = 0x80;
    response[2] = 0x05;
    response[3] = 0x02;
    response[4] = 31;
    memcpy(&response[8], "PCULATOR", 8);
    memcpy(&response[16], "SCSI CD-ROM    ", 16);
    memcpy(&response[32], "0001", 4);

    return cdrom_copy_response(sc, response, sizeof(response), alloc_length);
}

static int cdrom_build_capacity(const SCSI_CDROM_t *cdrom, scsi_common_t *sc)
{
    uint8_t response[8];

    memset(response, 0, sizeof(response));
    cdrom_store_be32(response, (uint32_t) (cdrom->total_blocks - 1));
    cdrom_store_be32(response + 4, cdrom->block_size);
    return cdrom_copy_response(sc, response, sizeof(response), sizeof(response));
}

static int cdrom_build_mode_sense(scsi_common_t *sc, const uint8_t *cdb)
{
    uint8_t response[32];
    uint8_t is_ten = (cdb[0] == GPCMD_MODE_SENSE_10);
    uint8_t page_code = cdb[2] & 0x3F;
    uint16_t alloc_length = is_ten ? (((uint16_t) cdb[7] << 8) | cdb[8]) : cdb[4];
    size_t response_len = is_ten ? 32 : 28;

    if ((page_code != 0x00) && (page_code != 0x2A) && (page_code != 0x3F)) {
        return -1;
    }

    memset(response, 0, sizeof(response));
    if (is_ten) {
        cdrom_store_be16(response, (uint16_t) (response_len - 2));
        response[8] = 0x2A;
        response[9] = 0x12;
        response[10] = 0x03;
        response[12] = 0x71;
        response[13] = 0x00;
        response[14] = 0x02;
    } else {
        response[0] = (uint8_t) (response_len - 1);
        response[4] = 0x2A;
        response[5] = 0x12;
        response[6] = 0x03;
        response[8] = 0x71;
        response[9] = 0x00;
        response[10] = 0x02;
    }

    return cdrom_copy_response(sc, response, response_len, alloc_length);
}

static void cdrom_build_toc_descriptor(uint8_t *descriptor, uint8_t track_number, uint32_t lba, uint8_t msf)
{
    uint8_t minute;
    uint8_t second;
    uint8_t frame;

    memset(descriptor, 0, 8);
    descriptor[1] = SCSI_TRACK_CONTROL_DATA;
    descriptor[2] = track_number;
    if (msf) {
        cdrom_lba_to_msf(lba, &minute, &second, &frame);
        descriptor[4] = 0;
        descriptor[5] = minute;
        descriptor[6] = second;
        descriptor[7] = frame;
    } else {
        cdrom_store_be32(&descriptor[4], lba);
    }
}

static void cdrom_build_raw_toc_entry(uint8_t *entry, uint8_t point, uint8_t pm, uint8_t ps, uint8_t pf)
{
    memset(entry, 0, 11);
    entry[0] = 1;
    entry[1] = SCSI_TRACK_CONTROL_DATA;
    entry[3] = point;
    entry[8] = pm;
    entry[9] = ps;
    entry[10] = pf;
}

static int cdrom_build_toc(const SCSI_CDROM_t *cdrom, scsi_common_t *sc, const uint8_t *cdb)
{
    uint8_t response[64];
    uint8_t format = cdb[2] & 0x0F;
    uint8_t msf = cdb[1] & 0x02;
    uint8_t start_track = cdb[6];
    uint16_t alloc_length = ((uint16_t) cdb[7] << 8) | cdb[8];
    uint32_t leadout = (uint32_t) cdrom->total_blocks;
    size_t offset = 4;
    size_t response_len;
    uint8_t minute;
    uint8_t second;
    uint8_t frame;

    if (format == SCSI_TOC_FORMAT_NORMAL) {
        format = (cdb[9] >> 6) & 0x03;
    }

    memset(response, 0, sizeof(response));
    response[2] = 1;
    response[3] = 1;

    switch (format) {
    case SCSI_TOC_FORMAT_NORMAL:
        if ((start_track == 0) || (start_track <= 1)) {
            cdrom_build_toc_descriptor(&response[offset], 1, 0, msf);
            offset += 8;
        }
        if ((start_track == 0) || (start_track <= 1) || (start_track == SCSI_TRACK_POINT_LEADOUT)) {
            cdrom_build_toc_descriptor(&response[offset], SCSI_TRACK_POINT_LEADOUT, leadout, msf);
            offset += 8;
        }
        break;

    case SCSI_TOC_FORMAT_SESSION:
        cdrom_build_toc_descriptor(&response[offset], 1, 0, msf);
        offset += 8;
        break;

    case SCSI_TOC_FORMAT_RAW:
        if (start_track <= 1) {
            cdrom_lba_to_msf(0, &minute, &second, &frame);
            cdrom_build_raw_toc_entry(&response[offset], 0xA0, 1, 0, 0);
            offset += 11;
            cdrom_build_raw_toc_entry(&response[offset], 0xA1, 1, 0, 0);
            offset += 11;
            cdrom_build_raw_toc_entry(&response[offset], 1, minute, second, frame);
            offset += 11;
            cdrom_lba_to_msf(leadout, &minute, &second, &frame);
            cdrom_build_raw_toc_entry(&response[offset], 0xA2, minute, second, frame);
            offset += 11;
        }
        break;

    default:
        return -1;
    }

    response_len = offset;
    cdrom_store_be16(response, (uint16_t) (response_len - 2));
    return cdrom_copy_response(sc, response, response_len, alloc_length);
}

static int cdrom_prepare_read(SCSI_CDROM_t *cdrom, scsi_common_t *sc, const uint8_t *cdb)
{
    uint32_t lba = cdrom_read_lba(cdb);
    uint32_t blocks = cdrom_read_blocks(cdb);
    size_t data_len;

    if (blocks == 0) {
        sc->owner->phase = SCSI_PHASE_STATUS;
        sc->owner->buffer_length = 0;
        return 0;
    }
    if ((lba >= cdrom->total_blocks) || (((uint64_t) lba + blocks) > cdrom->total_blocks)) {
        scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, ASCQ_NONE);
        return -1;
    }

    data_len = (size_t) blocks * (size_t) cdrom->block_size;
    if (data_len > SCSI_CDROM_MAX_TRANSFER_BYTES) {
        scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        return -1;
    }
    if (scsi_common_ensure_buffer(sc, data_len) != 0) {
        scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        return -1;
    }
    if (cdrom_seek_block(cdrom->image, lba, cdrom->block_size) != 0) {
        scsi_common_set_sense(sc, SENSE_MEDIUM_ERROR, ASC_UNRECOVERED_READ_ERROR, ASCQ_NONE);
        return -1;
    }
    if (fread(sc->temp_buffer, 1, data_len, cdrom->image) != data_len) {
        scsi_common_set_sense(sc, SENSE_MEDIUM_ERROR, ASC_UNRECOVERED_READ_ERROR, ASCQ_NONE);
        return -1;
    }

    sc->owner->phase = SCSI_PHASE_DATA_IN;
    sc->owner->buffer_length = (int32_t) data_len;
    return 0;
}

static int cdrom_prepare_seek(const SCSI_CDROM_t *cdrom, scsi_common_t *sc, const uint8_t *cdb)
{
    uint32_t lba = cdrom_seek_lba(cdb);

    if (lba >= cdrom->total_blocks) {
        scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, ASCQ_NONE);
        return -1;
    }

    sc->owner->phase = SCSI_PHASE_STATUS;
    sc->owner->buffer_length = 0;
    return 0;
}

static int cdrom_prepare_read_header(scsi_common_t *sc, const uint8_t *cdb)
{
    uint8_t response[8];
    uint16_t alloc_length = ((uint16_t) cdb[7] << 8) | cdb[8];
    uint32_t lba = be32(&cdb[2]);
    uint8_t minute;
    uint8_t second;
    uint8_t frame;

    memset(response, 0, sizeof(response));
    response[0] = 0x01;
    if (cdb[1] & 0x02) {
        cdrom_lba_to_msf(lba, &minute, &second, &frame);
        response[5] = minute;
        response[6] = second;
        response[7] = frame;
    } else {
        cdrom_store_be32(&response[4], lba);
    }

    return cdrom_copy_response(sc, response, sizeof(response), alloc_length);
}

static int cdrom_prepare_mechanism_status(scsi_common_t *sc, const uint8_t *cdb)
{
    uint8_t response[8];
    size_t alloc_length = ((size_t) cdb[7] << 16) | ((size_t) cdb[8] << 8) | cdb[9];

    memset(response, 0, sizeof(response));
    response[5] = 1;
    return cdrom_copy_response(sc, response, sizeof(response), alloc_length);
}

static size_t cdrom_append_feature(uint8_t *response,
                                   size_t offset,
                                   uint16_t feature_code,
                                   uint8_t flags,
                                   uint8_t additional_length,
                                   const uint8_t *payload,
                                   size_t payload_len)
{
    cdrom_store_be16(&response[offset], feature_code);
    response[offset + 2] = flags;
    response[offset + 3] = additional_length;
    if ((payload != NULL) && (payload_len != 0)) {
        memcpy(&response[offset + 4], payload, payload_len);
    }

    return offset + 4 + additional_length;
}

static int cdrom_prepare_get_configuration(const SCSI_CDROM_t *cdrom, scsi_common_t *sc, const uint8_t *cdb)
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
    if (cdrom_has_media(cdrom)) {
        cdrom_store_be16(&response[6], SCSI_MMC_PROFILE_CD_ROM);
    }

    memset(current_profile_payload, 0, sizeof(current_profile_payload));
    memset(core_payload, 0, sizeof(core_payload));
    memset(morphing_payload, 0, sizeof(morphing_payload));
    memset(removable_payload, 0, sizeof(removable_payload));
    memset(random_read_payload, 0, sizeof(random_read_payload));
    memset(multi_read_payload, 0, sizeof(multi_read_payload));
    memset(cd_read_payload, 0, sizeof(cd_read_payload));
    memset(status_payload, 0, sizeof(status_payload));

    current_profile_payload[0] = (uint8_t) (SCSI_MMC_PROFILE_CD_ROM >> 8);
    current_profile_payload[1] = (uint8_t) SCSI_MMC_PROFILE_CD_ROM;
    if (cdrom_has_media(cdrom)) {
        current_profile_payload[2] = 1;
    }

    core_payload[3] = 2;
    core_payload[4] = 1;

    morphing_payload[0] = 2;
    removable_payload[0] = 0x0D | 0x20;
    random_read_payload[2] = 8;
    random_read_payload[5] = 0x10;
    status_payload[0] = 7;
    status_payload[2] = 1;

    if ((feature == 0) || (request_type < 2)) {
        offset = cdrom_append_feature(response, offset, 0x0000, 0x03, 4, current_profile_payload, sizeof(current_profile_payload));
    }
    if ((feature == 1) || (request_type < 2)) {
        offset = cdrom_append_feature(response, offset, 0x0001, 0x0B, 8, core_payload, sizeof(core_payload));
    }
    if ((feature == 2) || (request_type < 2)) {
        offset = cdrom_append_feature(response, offset, 0x0002, 0x07, 4, morphing_payload, sizeof(morphing_payload));
    }
    if ((feature == 3) || (request_type < 2)) {
        offset = cdrom_append_feature(response, offset, 0x0003, 0x03, 4, removable_payload, sizeof(removable_payload));
    }
    if ((feature == 0x0010) || (request_type < 2)) {
        offset = cdrom_append_feature(response, offset, 0x0010, 0x03, 8, random_read_payload, sizeof(random_read_payload));
    }
    if ((feature == 0x001D) || (request_type < 2)) {
        offset = cdrom_append_feature(response, offset, 0x001D, 0x03, 0, multi_read_payload, 0);
    }
    if ((feature == 0x001E) || (request_type < 2)) {
        offset = cdrom_append_feature(response, offset, 0x001E, 0x0B, 4, cd_read_payload, sizeof(cd_read_payload));
    }
    if ((feature == 0x0103) || (request_type < 2)) {
        offset = cdrom_append_feature(response, offset, 0x0103, 0x03, 4, status_payload, sizeof(status_payload));
    }

    cdrom_store_be32(response, (uint32_t) (offset - 4));
    return cdrom_copy_response(sc, response, offset, alloc_length);
}

static int cdrom_prepare_get_event_status(const SCSI_CDROM_t *cdrom, scsi_common_t *sc, const uint8_t *cdb)
{
    uint8_t response[8];
    size_t response_length = 4;

    if (!(cdb[1] & 0x01)) {
        return -1;
    }

    memset(response, 0, sizeof(response));
    response[2] = SCSI_GESN_NO_EVENT;
    response[3] = (uint8_t) (1U << SCSI_GESN_MEDIA);
    if (cdb[4] & (1U << SCSI_GESN_MEDIA)) {
        response_length = 8;
        response[2] = SCSI_GESN_MEDIA;
        response[4] = cdrom_has_media(cdrom) ? SCSI_MEC_NO_CHANGE : SCSI_MEC_MEDIA_REMOVAL;
        response[5] = 1;
    }
    cdrom_store_be16(response, (uint16_t) (response_length - 4));
    return cdrom_copy_response(sc, response, response_length, ((uint16_t) cdb[7] << 8) | cdb[8]);
}

static int cdrom_prepare_disc_info(const SCSI_CDROM_t *cdrom, scsi_common_t *sc, const uint8_t *cdb)
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
    cdrom_lba_to_msf((uint32_t) cdrom->total_blocks, &minute, &second, &frame);
    response[20] = minute;
    response[21] = second;
    response[22] = frame;
    return cdrom_copy_response(sc, response, sizeof(response), ((uint16_t) cdb[7] << 8) | cdb[8]);
}

static int cdrom_prepare_track_info(const SCSI_CDROM_t *cdrom, scsi_common_t *sc, const uint8_t *cdb)
{
    uint8_t response[36];
    uint32_t pos = be32(&cdb[2]);
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
        } else if (pos == SCSI_TRACK_POINT_LEADOUT) {
            track_number = SCSI_TRACK_POINT_LEADOUT;
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
    cdrom_store_be32(&response[8], track_start);
    cdrom_store_be32(&response[24], track_length);
    return cdrom_copy_response(sc, response, sizeof(response), ((uint16_t) cdb[7] << 8) | cdb[8]);
}

static int cdrom_prepare_subchannel(scsi_common_t *sc, const uint8_t *cdb)
{
    uint8_t response[24];
    uint8_t format = cdb[3];
    uint8_t msf = (cdb[1] >> 1) & 1;
    uint16_t alloc_length = ((uint16_t) cdb[7] << 8) | cdb[8];
    size_t response_length = 4;
    uint8_t minute;
    uint8_t second;
    uint8_t frame;

    if ((format > 3) || ((format != 3) && (cdb[6] != 0))) {
        return -1;
    }

    memset(response, 0, sizeof(response));
    response[1] = SCSI_SUBCHANNEL_STATUS_DATA_ONLY;
    if (cdb[2] & 0x40) {
        switch (format) {
        case 0:
            response_length = 4;
            break;

        case 1:
            response_length = 16;
            response[4] = format;
            response[5] = SCSI_TRACK_CONTROL_DATA;
            response[6] = 1;
            response[7] = 1;
            if (msf) {
                cdrom_lba_to_msf(0, &minute, &second, &frame);
                response[8] = 0;
                response[9] = minute;
                response[10] = second;
                response[11] = frame;
                response[12] = 0;
                response[13] = 0;
                response[14] = 0;
                response[15] = 0;
            } else {
                cdrom_store_be32(&response[8], 0);
                cdrom_store_be32(&response[12], 0);
            }
            break;

        default:
            response_length = 24;
            response[4] = format;
            break;
        }
        response[3] = (uint8_t) (response_length - 4);
    }

    return cdrom_copy_response(sc, response, response_length, alloc_length);
}

static void cdrom_command(scsi_common_t *sc, const uint8_t *cdb)
{
    SCSI_CDROM_t *cdrom = (SCSI_CDROM_t *) sc->priv;

    sc->owner->phase = SCSI_PHASE_STATUS;
    sc->owner->buffer_length = 0;

    if (cdb[0] != GPCMD_REQUEST_SENSE) {
        scsi_common_clear_sense(sc);
    }
    if (!cdrom_has_media(cdrom) && cdrom_command_requires_media(cdb[0])) {
        scsi_common_set_sense(sc, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, ASCQ_NONE);
        return;
    }

    switch (cdb[0]) {
    case GPCMD_TEST_UNIT_READY:
    case GPCMD_START_STOP_UNIT:
    case GPCMD_PREVENT_REMOVAL:
        return;

    case GPCMD_INQUIRY:
        if ((cdb[1] & 1) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
            return;
        }
        if (cdrom_build_inquiry(sc, cdb[4]) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    case GPCMD_REQUEST_SENSE:
        if (scsi_common_ensure_buffer(sc, 18) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
            return;
        }
        memcpy(sc->temp_buffer, sc->sense, 18);
        sc->owner->phase = SCSI_PHASE_DATA_IN;
        sc->owner->buffer_length = 18;
        scsi_common_clear_sense(sc);
        return;

    case GPCMD_MODE_SENSE_6:
    case GPCMD_MODE_SENSE_10:
        if (cdrom_build_mode_sense(sc, cdb) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    case GPCMD_READ_CDROM_CAPACITY:
        if (cdrom_build_capacity(cdrom, sc) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    case GPCMD_READ_TOC_PMA_ATIP:
        if (cdrom_build_toc(cdrom, sc, cdb) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    case GPCMD_READ_6:
    case GPCMD_READ_10:
    case GPCMD_READ_12:
        (void) cdrom_prepare_read(cdrom, sc, cdb);
        return;

    case GPCMD_SEEK_6:
    case GPCMD_SEEK_10:
        (void) cdrom_prepare_seek(cdrom, sc, cdb);
        return;

    case GPCMD_READ_SUBCHANNEL:
        if (cdrom_prepare_subchannel(sc, cdb) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    case GPCMD_READ_HEADER:
        if (cdrom_prepare_read_header(sc, cdb) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    case GPCMD_GET_CONFIGURATION:
        if (cdrom_prepare_get_configuration(cdrom, sc, cdb) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    case GPCMD_GET_EVENT_STATUS:
        if (cdrom_prepare_get_event_status(cdrom, sc, cdb) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    case GPCMD_READ_DISC_INFO:
        if (cdrom_prepare_disc_info(cdrom, sc, cdb) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    case GPCMD_READ_TRACK_INFO:
        if (cdrom_prepare_track_info(cdrom, sc, cdb) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    case GPCMD_MECHANISM_STATUS:
        if (cdrom_prepare_mechanism_status(sc, cdb) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        }
        return;

    default:
        scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_ILLEGAL_OPCODE, ASCQ_NONE);
        return;
    }
}

int scsi_cdrom_detach(uint8_t bus, uint8_t id)
{
    scsi_device_t *dev;
    SCSI_CDROM_t *cdrom;

    if ((bus >= SCSI_BUS_MAX) || (id >= SCSI_ID_MAX)) {
        return -1;
    }

    dev = &scsi_devices[bus][id];
    if (dev->type == SCSI_NONE) {
        memset(dev, 0, sizeof(*dev));
        dev->type = SCSI_NONE;
        return 0;
    }
    if ((dev->type != SCSI_REMOVABLE_CDROM) || (dev->sc == NULL) || (dev->sc->priv == NULL)) {
        debug_log(DEBUG_ERROR, "[SCSI] Refusing to detach non-CD-ROM target %u from bus %u\r\n", id, bus);
        return -1;
    }
    if (dev->sc->owner != dev) {
        memset(dev, 0, sizeof(*dev));
        dev->type = SCSI_NONE;
        return 0;
    }

    cdrom = (SCSI_CDROM_t *) dev->sc->priv;
    if (cdrom->image != NULL) {
        fclose(cdrom->image);
    }
    free(cdrom->common.temp_buffer);
    free(cdrom);

    memset(dev, 0, sizeof(*dev));
    dev->type = SCSI_NONE;

    debug_log(DEBUG_INFO, "[SCSI] Detached CD-ROM target %u from bus %u\r\n", id, bus);
    return 0;
}

const char* scsi_cdrom_get_path(uint8_t bus, uint8_t id)
{
    scsi_device_t *dev;
    SCSI_CDROM_t *cdrom;

    if ((bus >= SCSI_BUS_MAX) || (id >= SCSI_ID_MAX)) {
        return NULL;
    }

    dev = &scsi_devices[bus][id];
    if ((dev->type != SCSI_REMOVABLE_CDROM) || (dev->sc == NULL) || (dev->sc->priv == NULL)) {
        return NULL;
    }

    cdrom = (SCSI_CDROM_t *) dev->sc->priv;
    return cdrom->path;
}

int scsi_cdrom_attach(uint8_t bus, uint8_t id, const char *path)
{
    SCSI_CDROM_t *cdrom;
    scsi_device_t *dev;
    const char *path_label = ((path != NULL) && (*path != 0)) ? path : "(empty)";

    if ((bus >= SCSI_BUS_MAX) || (id >= SCSI_ID_MAX)) {
        return -1;
    }

    dev = &scsi_devices[bus][id];
    if ((dev->type != SCSI_NONE) && (dev->type != SCSI_REMOVABLE_CDROM)) {
        debug_log(DEBUG_ERROR, "[SCSI] Refusing to replace non-CD-ROM target %u on bus %u\r\n", id, bus);
        return -1;
    }
    if (scsi_cdrom_detach(bus, id) != 0) {
        return -1;
    }

    cdrom = (SCSI_CDROM_t *) calloc(1, sizeof(SCSI_CDROM_t));
    if (cdrom == NULL) {
        return -1;
    }

    if ((path != NULL) && (*path != 0)) {
        cdrom->image = fopen(path, "rb");
        if (cdrom->image == NULL) {
            free(cdrom);
            return -1;
        }
    }

    cdrom->block_size = 2048;
    cdrom->total_blocks = (cdrom->image != NULL) ? cdrom_file_blocks(cdrom->image, cdrom->block_size) : 0;
    if ((path != NULL) && (*path != 0)) {
        strncpy(cdrom->path, path, sizeof(cdrom->path) - 1);
    }
    scsi_common_clear_sense(&cdrom->common);
    cdrom->common.block_len = cdrom->block_size;
    cdrom->common.max_transfer_len = 0xffff;
    cdrom->common.priv = cdrom;
    cdrom->common.id = id;

    memset(dev, 0, sizeof(*dev));
    dev->type = SCSI_REMOVABLE_CDROM;
    dev->sc = &cdrom->common;
    dev->sc->owner = dev;
    dev->command = cdrom_command;
    dev->request_sense = cdrom_request_sense;
    dev->reset = cdrom_reset;
    dev->phase_data_out = NULL;
    dev->command_stop = NULL;

    debug_log(DEBUG_INFO, "[SCSI] Attached CD-ROM target %u to bus %u from %s\r\n", id, bus, path_label);
    return 0;
}
