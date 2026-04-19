#ifndef _SCSI_CDROM_H_
#define _SCSI_CDROM_H_

#include "scsi_device.h"

int scsi_cdrom_attach(uint8_t bus, uint8_t id, const char *path);
int scsi_cdrom_detach(uint8_t bus, uint8_t id);
const char* scsi_cdrom_get_path(uint8_t bus, uint8_t id);

#endif
