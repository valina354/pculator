#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../debuglog.h"
#include "../host/host.h"
#include "scsi_disk.h"

typedef struct {
    scsi_common_t common;
    FILE *image;
    uint64_t total_blocks;
    uint32_t block_size;
    uint32_t pending_lba;
    uint32_t pending_blocks;
    uint8_t read_only;
    char path[512];
} SCSI_DISK_t;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static uint32_t read_lba(const uint8_t *cdb)
{
    switch (cdb[0]) {
    case GPCMD_READ_6:
    case GPCMD_WRITE_6:
        return ((uint32_t) (cdb[1] & 0x1f) << 16) | ((uint32_t) cdb[2] << 8) | (uint32_t) cdb[3];
    case GPCMD_READ_10:
    case GPCMD_WRITE_10:
    case GPCMD_VERIFY_10:
        return be32(&cdb[2]);
    case GPCMD_READ_12:
    case GPCMD_WRITE_12:
        return be32(&cdb[2]);
    default:
        return 0;
    }
}

static uint32_t read_blocks(const uint8_t *cdb)
{
    switch (cdb[0]) {
    case GPCMD_READ_6:
    case GPCMD_WRITE_6:
        return cdb[4] ? cdb[4] : 256;
    case GPCMD_READ_10:
    case GPCMD_WRITE_10:
    case GPCMD_VERIFY_10:
        return ((uint32_t) cdb[7] << 8) | (uint32_t) cdb[8];
    case GPCMD_READ_12:
    case GPCMD_WRITE_12:
        return be32(&cdb[6]);
    default:
        return 0;
    }
}

static int64_t disk_seek_block(FILE *image, uint64_t lba, uint32_t block_size)
{
    return host_fseek64(image, (uint64_t) lba * block_size, SEEK_SET);
}

static uint64_t disk_file_blocks(FILE *image, uint32_t block_size)
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

static void disk_request_sense(scsi_common_t *sc, uint8_t *buffer, uint8_t alloc_length)
{
    uint8_t copy_len = alloc_length;

    if (copy_len > 18) {
        copy_len = 18;
    }
    memcpy(buffer, sc->sense, copy_len);
    scsi_common_clear_sense(sc);
}

static void disk_reset(scsi_common_t *sc)
{
    scsi_common_clear_sense(sc);
}

static void disk_build_inquiry(SCSI_DISK_t *disk)
{
    scsi_common_t *sc = &disk->common;

    if (scsi_common_ensure_buffer(sc, 36) != 0) {
        scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        sc->owner->phase = SCSI_PHASE_STATUS;
        sc->owner->buffer_length = 0;
        return;
    }

    memset(sc->temp_buffer, 0, 36);
    sc->temp_buffer[0] = 0x00;
    sc->temp_buffer[1] = 0x00;
    sc->temp_buffer[2] = 0x05;
    sc->temp_buffer[3] = 0x02;
    sc->temp_buffer[4] = 31;
    memcpy(&sc->temp_buffer[8], "PCULATOR", 8);
    memcpy(&sc->temp_buffer[16], "SCSI DISK      ", 16);
    memcpy(&sc->temp_buffer[32], "0001", 4);

    sc->owner->phase = SCSI_PHASE_DATA_IN;
    sc->owner->buffer_length = 36;
}

static void disk_build_mode_sense(SCSI_DISK_t *disk, const uint8_t *cdb)
{
    scsi_common_t *sc = &disk->common;
    uint8_t page = cdb[2] & 0x3f;
    uint8_t is_ten = (cdb[0] == GPCMD_MODE_SENSE_10);
    uint32_t len = is_ten ? 20 : 16;

    if ((page != 0x3f) && (page != 0x08) && (page != 0x04)) {
        scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        sc->owner->phase = SCSI_PHASE_STATUS;
        sc->owner->buffer_length = 0;
        return;
    }

    if (scsi_common_ensure_buffer(sc, len) != 0) {
        scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
        sc->owner->phase = SCSI_PHASE_STATUS;
        sc->owner->buffer_length = 0;
        return;
    }

    memset(sc->temp_buffer, 0, len);
    if (is_ten) {
        sc->temp_buffer[0] = 0;
        sc->temp_buffer[1] = (uint8_t) (len - 2);
        sc->temp_buffer[7] = 8;
        sc->temp_buffer[8] = 0x08;
        sc->temp_buffer[9] = 0x0a;
        sc->temp_buffer[10] = 0x04;
        sc->temp_buffer[16] = 0x04;
        sc->temp_buffer[17] = 0x16;
        sc->temp_buffer[18] = 0x00;
        sc->temp_buffer[19] = 0x80;
    } else {
        sc->temp_buffer[0] = (uint8_t) (len - 1);
        sc->temp_buffer[3] = 8;
        sc->temp_buffer[4] = 0x08;
        sc->temp_buffer[5] = 0x0a;
        sc->temp_buffer[6] = 0x04;
        sc->temp_buffer[12] = 0x04;
        sc->temp_buffer[13] = 0x16;
        sc->temp_buffer[14] = 0x00;
        sc->temp_buffer[15] = 0x80;
    }

    sc->owner->phase = SCSI_PHASE_DATA_IN;
    sc->owner->buffer_length = (int32_t) len;
}

static void disk_command(scsi_common_t *sc, const uint8_t *cdb)
{
    SCSI_DISK_t *disk = (SCSI_DISK_t *) sc->priv;
    uint32_t lba;
    uint32_t blocks;
    uint32_t data_len;

    scsi_common_clear_sense(sc);
    sc->owner->buffer_length = 0;
    sc->owner->phase = SCSI_PHASE_STATUS;

    switch (cdb[0]) {
    case GPCMD_TEST_UNIT_READY:
        return;
    case GPCMD_INQUIRY:
        if ((cdb[1] & 1) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
            return;
        }
        disk_build_inquiry(disk);
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
        disk_build_mode_sense(disk, cdb);
        return;
    case GPCMD_MODE_SELECT_6:
    case GPCMD_MODE_SELECT_10:
    case GPCMD_START_STOP_UNIT:
    case GPCMD_PREVENT_REMOVAL:
    case GPCMD_SYNCHRONIZE_CACHE:
        return;
    case GPCMD_READ_CDROM_CAPACITY:
        if (scsi_common_ensure_buffer(sc, 8) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
            return;
        }
        memset(sc->temp_buffer, 0, 8);
        if (disk->total_blocks == 0) {
            scsi_common_set_sense(sc, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, ASCQ_NONE);
            return;
        }
        sc->temp_buffer[0] = (uint8_t) ((disk->total_blocks - 1) >> 24);
        sc->temp_buffer[1] = (uint8_t) ((disk->total_blocks - 1) >> 16);
        sc->temp_buffer[2] = (uint8_t) ((disk->total_blocks - 1) >> 8);
        sc->temp_buffer[3] = (uint8_t) (disk->total_blocks - 1);
        sc->temp_buffer[4] = (uint8_t) (disk->block_size >> 24);
        sc->temp_buffer[5] = (uint8_t) (disk->block_size >> 16);
        sc->temp_buffer[6] = (uint8_t) (disk->block_size >> 8);
        sc->temp_buffer[7] = (uint8_t) disk->block_size;
        sc->owner->phase = SCSI_PHASE_DATA_IN;
        sc->owner->buffer_length = 8;
        return;
    case GPCMD_VERIFY_10:
        return;
    case GPCMD_READ_6:
    case GPCMD_READ_10:
    case GPCMD_READ_12:
        lba = read_lba(cdb);
        blocks = read_blocks(cdb);
        if ((lba >= disk->total_blocks) || ((uint64_t) lba + blocks > disk->total_blocks)) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, ASCQ_NONE);
            return;
        }
        data_len = blocks * disk->block_size;
        if (scsi_common_ensure_buffer(sc, data_len) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
            return;
        }
        //debug_log(DEBUG_INFO, "[SCSI] disk target %u read lba=%lu blocks=%lu bytes=%lu\r\n",
        //    sc->id, (unsigned long) lba, (unsigned long) blocks, (unsigned long) data_len);
        if (disk_seek_block(disk->image, lba, disk->block_size) != 0) {
            scsi_common_set_sense(sc, SENSE_MEDIUM_ERROR, ASC_UNRECOVERED_READ_ERROR, ASCQ_NONE);
            return;
        }
        if (fread(sc->temp_buffer, 1, data_len, disk->image) != data_len) {
            scsi_common_set_sense(sc, SENSE_MEDIUM_ERROR, ASC_UNRECOVERED_READ_ERROR, ASCQ_NONE);
            return;
        }
        sc->owner->phase = SCSI_PHASE_DATA_IN;
        sc->owner->buffer_length = (int32_t) data_len;
        return;
    case GPCMD_WRITE_6:
    case GPCMD_WRITE_10:
    case GPCMD_WRITE_12:
        if (disk->read_only) {
            //debug_log(DEBUG_INFO, "[SCSI] disk target %u write rejected (read-only) lba=%lu blocks=%lu\r\n",
            //    sc->id, (unsigned long) read_lba(cdb), (unsigned long) read_blocks(cdb));
            scsi_common_set_sense(sc, SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, ASCQ_NONE);
            return;
        }
        lba = read_lba(cdb);
        blocks = read_blocks(cdb);
        if ((lba >= disk->total_blocks) || ((uint64_t) lba + blocks > disk->total_blocks)) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, ASCQ_NONE);
            return;
        }
        data_len = blocks * disk->block_size;
        if (scsi_common_ensure_buffer(sc, data_len) != 0) {
            scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, ASCQ_NONE);
            return;
        }
        disk->pending_lba = lba;
        disk->pending_blocks = blocks;
        //debug_log(DEBUG_INFO, "[SCSI] disk target %u write lba=%lu blocks=%lu bytes=%lu\r\n",
        //    sc->id, (unsigned long) lba, (unsigned long) blocks, (unsigned long) data_len);
        memset(sc->temp_buffer, 0, data_len);
        sc->owner->phase = SCSI_PHASE_DATA_OUT;
        sc->owner->buffer_length = (int32_t) data_len;
        return;
    default:
        scsi_common_set_sense(sc, SENSE_ILLEGAL_REQUEST, ASC_ILLEGAL_OPCODE, ASCQ_NONE);
        return;
    }
}

static uint8_t disk_phase_data_out(scsi_common_t *sc)
{
    SCSI_DISK_t *disk = (SCSI_DISK_t *) sc->priv;
    uint32_t data_len = disk->pending_blocks * disk->block_size;

    //debug_log(DEBUG_INFO, "[SCSI] disk target %u commit write lba=%lu blocks=%lu bytes=%lu\r\n",
    //    sc->id, (unsigned long) disk->pending_lba, (unsigned long) disk->pending_blocks, (unsigned long) data_len);
    if (disk_seek_block(disk->image, disk->pending_lba, disk->block_size) != 0) {
        scsi_common_set_sense(sc, SENSE_MEDIUM_ERROR, ASC_WRITE_ERROR, ASCQ_NONE);
        return 1;
    }
    if (fwrite(sc->temp_buffer, 1, data_len, disk->image) != data_len) {
        scsi_common_set_sense(sc, SENSE_MEDIUM_ERROR, ASC_WRITE_ERROR, ASCQ_NONE);
        return 1;
    }
    fflush(disk->image);
    return 0;
}

int scsi_disk_attach(uint8_t bus, uint8_t id, const char *path)
{
    SCSI_DISK_t *disk;
    scsi_device_t *dev;

    if ((bus >= SCSI_BUS_MAX) || (id >= SCSI_ID_MAX)) {
        return -1;
    }

    disk = (SCSI_DISK_t *) calloc(1, sizeof(SCSI_DISK_t));
    if (disk == NULL) {
        return -1;
    }

    disk->image = fopen(path, "rb+");
    if (disk->image == NULL) {
        disk->image = fopen(path, "rb");
        disk->read_only = 1;
    }
    if (disk->image == NULL) {
        free(disk);
        return -1;
    }

    disk->block_size = 512;
    disk->total_blocks = disk_file_blocks(disk->image, disk->block_size);
    strncpy(disk->path, path, sizeof(disk->path) - 1);
    scsi_common_clear_sense(&disk->common);
    disk->common.block_len = disk->block_size;
    disk->common.max_transfer_len = 0xffff;
    disk->common.priv = disk;
    disk->common.id = id;

    dev = &scsi_devices[bus][id];
    memset(dev, 0, sizeof(*dev));
    dev->type = SCSI_FIXED_DISK;
    dev->sc = &disk->common;
    dev->sc->owner = dev;
    dev->command = disk_command;
    dev->request_sense = disk_request_sense;
    dev->reset = disk_reset;
    dev->phase_data_out = disk_phase_data_out;
    dev->command_stop = NULL;

    debug_log(DEBUG_INFO, "[SCSI] Attached disk target %u to bus %u from %s\r\n", id, bus, path);
    return 0;
}
