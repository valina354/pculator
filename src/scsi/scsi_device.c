#include <stdlib.h>
#include <string.h>
#include "../debuglog.h"
#include "scsi_device.h"

scsi_device_t scsi_devices[SCSI_BUS_MAX][SCSI_ID_MAX];

static uint8_t scsi_null_device_sense[18] = {
    0x70, 0x00, SENSE_ILLEGAL_REQUEST, 0x00, 0x00, 0x00, 0x00, 0x0a,
    0x00, 0x00, 0x00, 0x00, ASC_INV_FIELD_IN_CMD_PACKET, 0x00, 0x00, 0x00,
    0x00, 0x00
};

static uint8_t scsi_device_target_command(scsi_device_t *dev, uint8_t *cdb)
{
    if (dev->command == NULL) {
        return SCSI_STATUS_CHECK_CONDITION;
    }

    dev->sc->status_byte = 0;
    memcpy(dev->sc->current_cdb, cdb, sizeof(dev->sc->current_cdb));
    dev->command(dev->sc, cdb);

    if (dev->sc->status_byte != 0) {
        return SCSI_STATUS_CHECK_CONDITION;
    }

    return SCSI_STATUS_OK;
}

double scsi_device_get_callback(scsi_device_t *dev)
{
    if (dev->sc != NULL) {
        return dev->sc->callback;
    }

    return -1.0;
}

uint8_t *scsi_device_sense(scsi_device_t *dev)
{
    if (dev->sc != NULL) {
        return dev->sc->sense;
    }

    return scsi_null_device_sense;
}

void scsi_device_request_sense(scsi_device_t *dev, uint8_t *buffer, uint8_t alloc_length)
{
    if (dev->request_sense != NULL) {
        dev->request_sense(dev->sc, buffer, alloc_length);
        return;
    }

    memcpy(buffer, scsi_null_device_sense, alloc_length);
}

void scsi_device_reset(scsi_device_t *dev)
{
    if (dev->reset != NULL) {
        dev->reset(dev->sc);
    }
}

int scsi_device_present(scsi_device_t *dev)
{
    return dev->type != SCSI_NONE;
}

int scsi_device_valid(scsi_device_t *dev)
{
    return dev->sc != NULL;
}

int scsi_device_cdb_length(scsi_device_t *dev)
{
    (void) dev;
    return 12;
}

void scsi_device_command_phase0(scsi_device_t *dev, uint8_t *cdb)
{
    if (dev->sc == NULL) {
        dev->phase = SCSI_PHASE_STATUS;
        dev->status = SCSI_STATUS_CHECK_CONDITION;
        return;
    }

    dev->phase = SCSI_PHASE_COMMAND;
    dev->status = scsi_device_target_command(dev, cdb);
}

void scsi_device_command_stop(scsi_device_t *dev)
{
    if (dev->command_stop != NULL) {
        dev->command_stop(dev->sc);
    }

    if ((dev->sc != NULL) && (dev->sc->status_byte != 0)) {
        dev->status = SCSI_STATUS_CHECK_CONDITION;
    } else {
        dev->status = SCSI_STATUS_OK;
    }
}

void scsi_device_command_phase1(scsi_device_t *dev)
{
    if (dev->sc == NULL) {
        return;
    }

    if (dev->phase == SCSI_PHASE_DATA_OUT) {
        if (dev->phase_data_out != NULL) {
            dev->phase_data_out(dev->sc);
        }
    } else {
        scsi_device_command_stop(dev);
    }

    if (dev->sc->status_byte != 0) {
        dev->status = SCSI_STATUS_CHECK_CONDITION;
    } else {
        dev->status = SCSI_STATUS_OK;
    }
}

void scsi_device_identify(scsi_device_t *dev, uint8_t lun)
{
    if ((dev == NULL) || (dev->sc == NULL)) {
        return;
    }

    dev->sc->cur_lun = lun;
}

void scsi_device_close_all(void)
{
    uint8_t bus;
    uint8_t id;

    for (bus = 0; bus < SCSI_BUS_MAX; bus++) {
        for (id = 0; id < SCSI_ID_MAX; id++) {
            if ((scsi_devices[bus][id].command_stop != NULL) && (scsi_devices[bus][id].sc != NULL)) {
                scsi_devices[bus][id].command_stop(scsi_devices[bus][id].sc);
            }
        }
    }
}

void scsi_device_init(void)
{
    uint8_t bus;
    uint8_t id;

    for (bus = 0; bus < SCSI_BUS_MAX; bus++) {
        for (id = 0; id < SCSI_ID_MAX; id++) {
            memset(&scsi_devices[bus][id], 0, sizeof(scsi_device_t));
            scsi_devices[bus][id].type = SCSI_NONE;
        }
    }
}

void scsi_common_set_sense(scsi_common_t *sc, uint8_t key, uint8_t asc, uint8_t ascq)
{
    memset(sc->sense, 0, sizeof(sc->sense));
    sc->sense[0] = 0x70;
    sc->sense[2] = key;
    sc->sense[7] = 0x0a;
    sc->sense[12] = asc;
    sc->sense[13] = ascq;
    sc->status_byte = 1;
}

void scsi_common_clear_sense(scsi_common_t *sc)
{
    memset(sc->sense, 0, sizeof(sc->sense));
    sc->sense[0] = 0x70;
    sc->sense[7] = 0x0a;
    sc->status_byte = 0;
}

int scsi_common_ensure_buffer(scsi_common_t *sc, size_t len)
{
    uint8_t *new_buffer;

    if (sc->temp_buffer_sz >= len) {
        return 0;
    }

    new_buffer = (uint8_t *) realloc(sc->temp_buffer, len);
    if (new_buffer == NULL) {
        debug_log(DEBUG_ERROR, "[SCSI] Unable to allocate %u bytes for target buffer\r\n", (unsigned) len);
        return -1;
    }

    sc->temp_buffer = new_buffer;
    sc->temp_buffer_sz = len;
    return 0;
}
