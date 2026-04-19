#include "scsi.h"

static uint8_t scsi_next_bus = 0;
static double scsi_bus_speed[SCSI_BUS_MAX];

void scsi_reset(void)
{
    uint8_t i;

    scsi_next_bus = 0;
    for (i = 0; i < SCSI_BUS_MAX; i++) {
        scsi_bus_speed[i] = 0.0;
    }
}

uint8_t scsi_get_bus(void)
{
    uint8_t ret = scsi_next_bus;

    if (scsi_next_bus >= SCSI_BUS_MAX) {
        return 0xff;
    }

    scsi_next_bus++;
    return ret;
}

void scsi_bus_set_speed(uint8_t bus, double speed)
{
    if (bus < SCSI_BUS_MAX) {
        scsi_bus_speed[bus] = speed;
    }
}

double scsi_bus_get_speed(uint8_t bus)
{
    if (bus >= SCSI_BUS_MAX) {
        return 0.0;
    }

    return scsi_bus_speed[bus];
}
