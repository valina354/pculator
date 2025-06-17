#ifndef __IDE__
#define __IDE__

#include "../../config.h"

#ifdef USE_PCEM_IDE

#include <stdint.h>
#include "../../chipset/i8259.h"
#include "pcem-hdd_file.h"
//#include "device.h"
//#include "timer.h"

struct IDE;

typedef struct IDE {
    int type;
    int board;
    uint8_t atastat;
    uint8_t error;
    int secount, sector, cylinder, head, drive, cylprecomp;
    uint8_t command;
    uint8_t fdisk;
    int pos;
    int reset;
    uint16_t buffer[65536];
    int irqstat;
    int service;
    int lba;
    uint32_t lba_addr;
    int skip512;
    int blocksize, blockcount;
    uint8_t sector_buffer[256 * 512];
    int do_initial_read;
    int sector_pos;
    hdd_file_t hdd_file;
    //atapi_device_t atapi;
} IDE;

extern void writeide(int ide_board, uint16_t addr, uint8_t val);
extern void writeidew(int ide_board, uint16_t val);
extern uint8_t readide(int ide_board, uint16_t addr);
extern uint16_t readidew(int ide_board);
extern void callbackide(int ide_board);
extern void resetide(void);
extern void ide_pri_enable();
extern void ide_sec_enable();
extern void ide_pri_disable();
extern void ide_sec_disable();
extern void ide_set_bus_master(int (*read_data)(int channel, uint8_t* data, int size, void* p),
    int (*write_data)(int channel, uint8_t* data, int size, void* p),
    void (*set_irq)(int channel, void* p), void* p);
void ide_irq_raise(struct IDE* ide);
void ide_reset_devices();
void* ide_init(I8259_t* pic);
void loadhd(struct IDE* ide, int d, const char* fn);

extern int (*ide_bus_master_read_data)(int channel, uint8_t* data, int size, void* p);
extern int (*ide_bus_master_write_data)(int channel, uint8_t* data, int size, void* p);
extern void (*ide_bus_master_set_irq)(int channel, void* p);
extern void* ide_bus_master_p;

//#include "pcem-ide_atapi.h"

enum { IDE_NONE = 0, IDE_HDD, IDE_CDROM };

extern int ideboard;

extern uint32_t ide_timer[2];

extern char ide_fn[7][512];

extern int cdrom_channel, zip_channel;

uint32_t atapi_get_cd_channel(int channel);
uint32_t atapi_get_cd_volume(int channel);

#define CDROM_IMAGE 200

//extern device_t ide_device;

/* Bits of 'atastat' */
#define ERR_STAT 0x01
#define DRQ_STAT 0x08 /* Data request */
#define DSC_STAT 0x10
#define SERVICE_STAT 0x10
#define READY_STAT 0x40
#define BUSY_STAT 0x80

/* Bits of 'error' */
#define ABRT_ERR 0x04 /* Command aborted */
#define MCR_ERR 0x08  /* Media change request */

#define FEATURE_SET_TRANSFER_MODE 0x03
#define FEATURE_ENABLE_IRQ_OVERLAPPED 0x5d
#define FEATURE_ENABLE_IRQ_SERVICE 0x5e
#define FEATURE_DISABLE_REVERT 0x66
#define FEATURE_ENABLE_REVERT 0xcc
#define FEATURE_DISABLE_IRQ_OVERLAPPED 0xdd
#define FEATURE_DISABLE_IRQ_SERVICE 0xde

#endif

#endif //__IDE__
