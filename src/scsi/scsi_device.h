#ifndef _SCSI_DEVICE_H_
#define _SCSI_DEVICE_H_

#include <stddef.h>
#include <stdint.h>
#include "scsi.h"

#define SCSI_LUN_USE_CDB 0xff

#define GPCMD_TEST_UNIT_READY               0x00
#define GPCMD_REQUEST_SENSE                 0x03
#define GPCMD_FORMAT_UNIT                   0x04
#define GPCMD_READ_6                        0x08
#define GPCMD_SEEK_6                        0x0b
#define GPCMD_WRITE_6                       0x0a
#define GPCMD_INQUIRY                       0x12
#define GPCMD_MODE_SELECT_6                 0x15
#define GPCMD_MODE_SENSE_6                  0x1a
#define GPCMD_START_STOP_UNIT               0x1b
#define GPCMD_PREVENT_REMOVAL               0x1e
#define GPCMD_READ_CDROM_CAPACITY           0x25
#define GPCMD_READ_10                       0x28
#define GPCMD_WRITE_10                      0x2a
#define GPCMD_SEEK_10                       0x2b
#define GPCMD_VERIFY_10                     0x2f
#define GPCMD_SYNCHRONIZE_CACHE             0x35
#define GPCMD_READ_SUBCHANNEL               0x42
#define GPCMD_READ_TOC_PMA_ATIP             0x43
#define GPCMD_READ_HEADER                   0x44
#define GPCMD_GET_CONFIGURATION             0x46
#define GPCMD_GET_EVENT_STATUS              0x4a
#define GPCMD_READ_DISC_INFO                0x51
#define GPCMD_READ_TRACK_INFO               0x52
#define GPCMD_MODE_SELECT_10                0x55
#define GPCMD_MODE_SENSE_10                 0x5a
#define GPCMD_READ_12                       0xa8
#define GPCMD_WRITE_12                      0xaa
#define GPCMD_MECHANISM_STATUS              0xbd

#define SCSI_STATUS_OK              0
#define SCSI_STATUS_CHECK_CONDITION 2

#define SENSE_NONE            0
#define SENSE_NOT_READY       2
#define SENSE_MEDIUM_ERROR    3
#define SENSE_ILLEGAL_REQUEST 5
#define SENSE_UNIT_ATTENTION  6
#define SENSE_DATA_PROTECT    7

#define ASC_NONE                        0x00
#define ASC_NOT_READY                   0x04
#define ASC_WRITE_ERROR                 0x0c
#define ASC_UNRECOVERED_READ_ERROR      0x11
#define ASC_ILLEGAL_OPCODE              0x20
#define ASC_LBA_OUT_OF_RANGE            0x21
#define ASC_INV_FIELD_IN_CMD_PACKET     0x24
#define ASC_INV_FIELD_IN_PARAMETER_LIST 0x26
#define ASC_WRITE_PROTECTED             0x27
#define ASC_MEDIUM_MAY_HAVE_CHANGED     0x28
#define ASC_CAPACITY_DATA_CHANGED       0x2a
#define ASC_INCOMPATIBLE_FORMAT         0x30
#define ASC_MEDIUM_NOT_PRESENT          0x3a

#define ASCQ_NONE                       0x00
#define ASCQ_UNIT_IN_PROCESS_OF_BECOMING_READY 0x01

#define SCSI_PHASE_DATA_OUT 0
#define SCSI_PHASE_DATA_IN  1
#define SCSI_PHASE_COMMAND  2
#define SCSI_PHASE_STATUS   3

#define SCSI_NONE            0x0060
#define SCSI_FIXED_DISK      0x0000
#define SCSI_REMOVABLE_CDROM 0x8005

typedef struct scsi_device_t scsi_device_t;

typedef struct scsi_common_s {
    void *priv;
    scsi_device_t *owner;
    uint8_t *temp_buffer;
    size_t temp_buffer_sz;
    uint8_t current_cdb[16];
    uint8_t sense[256];
    uint8_t id;
    uint8_t cur_lun;
    uint16_t max_transfer_len;
    int buffer_pos;
    int total_length;
    int unit_attention;
    uint32_t block_len;
    double callback;
    uint8_t status_byte;
} scsi_common_t;

struct scsi_device_t {
    int32_t buffer_length;
    uint8_t status;
    uint8_t phase;
    uint16_t type;
    scsi_common_t *sc;
    void (*command)(scsi_common_t *sc, const uint8_t *cdb);
    void (*request_sense)(scsi_common_t *sc, uint8_t *buffer, uint8_t alloc_length);
    void (*reset)(scsi_common_t *sc);
    uint8_t (*phase_data_out)(scsi_common_t *sc);
    void (*command_stop)(scsi_common_t *sc);
};

extern scsi_device_t scsi_devices[SCSI_BUS_MAX][SCSI_ID_MAX];

double scsi_device_get_callback(scsi_device_t *dev);
uint8_t *scsi_device_sense(scsi_device_t *dev);
void scsi_device_request_sense(scsi_device_t *dev, uint8_t *buffer, uint8_t alloc_length);
void scsi_device_reset(scsi_device_t *dev);
int scsi_device_present(scsi_device_t *dev);
int scsi_device_valid(scsi_device_t *dev);
int scsi_device_cdb_length(scsi_device_t *dev);
void scsi_device_command_phase0(scsi_device_t *dev, uint8_t *cdb);
void scsi_device_command_stop(scsi_device_t *dev);
void scsi_device_command_phase1(scsi_device_t *dev);
void scsi_device_identify(scsi_device_t *dev, uint8_t lun);
void scsi_device_close_all(void);
void scsi_device_init(void);

void scsi_common_set_sense(scsi_common_t *sc, uint8_t key, uint8_t asc, uint8_t ascq);
void scsi_common_clear_sense(scsi_common_t *sc);
int scsi_common_ensure_buffer(scsi_common_t *sc, size_t len);

#endif
