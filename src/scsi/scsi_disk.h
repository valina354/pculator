#ifndef _SCSI_DISK_H_
#define _SCSI_DISK_H_

#include "scsi_device.h"

int scsi_disk_attach(uint8_t bus, uint8_t id, const char *path);

#endif
