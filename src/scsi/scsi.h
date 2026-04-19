#ifndef _SCSI_H_
#define _SCSI_H_

#include <stdint.h>

#define SCSI_BUS_MAX 1
#define SCSI_ID_MAX 16
#define SCSI_LUN_MAX 8

void scsi_reset(void);
uint8_t scsi_get_bus(void);
void scsi_bus_set_speed(uint8_t bus, double speed);
double scsi_bus_get_speed(uint8_t bus);

#endif
