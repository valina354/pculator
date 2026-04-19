#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../debuglog.h"
#include "../host_endian.h"
#include "../memory.h"
#include "../ports.h"
#include "../timing.h"
#include "buslogic.h"
#include "scsi.h"
#include "scsi_cdrom.h"
#include "scsi_device.h"
#include "scsi_disk.h"

#define MIN_VAL(a, b) (((a) < (b)) ? (a) : (b))
#define ROM_SIZE 0x4000

#define BUSLOGIC_FLAG_MBX_24BIT         0x01
#define BUSLOGIC_FLAG_CDROM_BOOT        0x02
#define BUSLOGIC_FLAG_INT_GEOM_WRITABLE 0x04

#define BUSLOGIC_RESET_DELAY_US 50000.0

#define CTRL_HRST  0x80
#define CTRL_SRST  0x40
#define CTRL_IRST  0x20
#define CTRL_SCRST 0x10

#define STAT_STST   0x80
#define STAT_DFAIL  0x40
#define STAT_INIT   0x20
#define STAT_IDLE   0x10
#define STAT_CDFULL 0x08
#define STAT_DFULL  0x04
#define STAT_INVCMD 0x01

#define CMD_NOP         0x00
#define CMD_MBINIT      0x01
#define CMD_START_SCSI  0x02
#define CMD_BIOSCMD     0x03
#define CMD_INQUIRY     0x04
#define CMD_EMBOI       0x05
#define CMD_SELTIMEOUT  0x06
#define CMD_BUSON_TIME  0x07
#define CMD_BUSOFF_TIME 0x08
#define CMD_DMASPEED    0x09
#define CMD_RETDEVS     0x0A
#define CMD_RETCONF     0x0B
#define CMD_TARGET      0x0C
#define CMD_RETSETUP    0x0D
#define CMD_WRITE_CH2   0x1A
#define CMD_READ_CH2    0x1B
#define CMD_ECHO        0x1F
#define CMD_OPTIONS     0x21

#define INTR_ANY  0x80
#define INTR_HACC 0x04
#define INTR_MBOA 0x02
#define INTR_MBIF 0x01

#define MBO_FREE  0x00
#define MBO_START 0x01
#define MBO_ABORT 0x02

#define MBI_SUCCESS   0x01
#define MBI_NOT_FOUND 0x03
#define MBI_ERROR     0x04

#define SCSI_INITIATOR_COMMAND     0x00
#define TARGET_MODE_COMMAND        0x01
#define SCATTER_GATHER_COMMAND     0x02
#define SCSI_INITIATOR_COMMAND_RES 0x03
#define SCATTER_GATHER_COMMAND_RES 0x04
#define BUS_RESET_OPCODE           0x81

#define CCB_DATA_XFER_IN  0x01
#define CCB_DATA_XFER_OUT 0x02

#define CCB_COMPLETE          0x00
#define CCB_SELECTION_TIMEOUT 0x11
#define CCB_INVALID_OP_CODE   0x16
#define CCB_INVALID_CCB       0x1A
#define CCB_ABORTED           0x26

typedef struct BUSLOGIC_s BUSLOGIC_t;

#pragma pack(push, 1)
typedef struct {
    uint8_t hi;
    uint8_t mid;
    uint8_t lo;
} addr24_t;

typedef struct {
    uint8_t fSynchronousInitiationEnabled;
    uint8_t uBusTransferRate;
    uint8_t uPreemptTimeOnBus;
    uint8_t uTimeOffBus;
    uint8_t cMailbox;
    addr24_t MailboxAddress;
    uint8_t SynchronousValuesId0To7[8];
    uint8_t uDisconnectPermittedId0To7;
    uint8_t VendorSpecificData[28];
} ReplyInquireSetupInformation;

typedef struct {
    uint8_t Count;
    addr24_t Address;
} MailboxInit_t;

typedef struct {
    uint8_t CmdStatus;
    addr24_t CCBPointer;
} Mailbox_t;

typedef struct {
    uint8_t CCBPointer[4];
    uint8_t HostStatus;
    uint8_t TargetStatus;
    uint8_t Reserved;
    uint8_t CompletionCode;
} Mailbox32Raw_t;

typedef struct {
    uint32_t CCBPointer;
    uint8_t HostStatus;
    uint8_t TargetStatus;
    uint8_t Reserved;
    uint8_t CompletionCode;
} Mailbox32_t;

typedef struct {
    uint8_t Opcode;
    uint8_t AddrControl;
    uint8_t CdbLength;
    uint8_t RequestSenseLength;
    addr24_t DataLength;
    addr24_t DataPointer;
    addr24_t LinkPointer;
    uint8_t LinkId;
    uint8_t HostStatus;
    uint8_t TargetStatus;
    uint8_t Reserved[2];
    uint8_t Cdb[12];
} CCB24_t;

typedef struct {
    uint8_t Opcode;
    uint8_t ControlRaw;
    uint8_t CdbLength;
    uint8_t RequestSenseLength;
    uint8_t DataLength[4];
    uint8_t DataPointer[4];
    uint8_t Reserved2[2];
    uint8_t HostStatus;
    uint8_t TargetStatus;
    uint8_t Id;
    uint8_t LunRaw;
    uint8_t Cdb[12];
    uint8_t Reserved3[6];
    uint8_t SensePointer[4];
} CCB32Raw_t;

typedef struct {
    uint8_t Opcode;
    uint8_t ControlRaw;
    uint8_t CdbLength;
    uint8_t RequestSenseLength;
    uint32_t DataLength;
    uint32_t DataPointer;
    uint8_t Reserved2[2];
    uint8_t HostStatus;
    uint8_t TargetStatus;
    uint8_t Id;
    uint8_t LunRaw;
    uint8_t Cdb[12];
    uint8_t Reserved3[6];
    uint32_t SensePointer;
} CCB32_t;

typedef union {
    CCB24_t old_ccb;
    CCB32_t new_ccb;
} CCBU_t;

typedef struct {
    CCBU_t CmdBlock;
    uint32_t CCBPointer;
    int Is24bit;
    uint8_t TargetID;
    uint8_t LUN;
    uint8_t HostStatus;
    uint8_t TargetStatus;
    uint8_t MailboxCompletionCode;
} Req_t;

typedef struct {
    uint8_t command;
    uint8_t lun;
    uint8_t id;
    uint8_t address[4];
    uint8_t secount;
    addr24_t dma_address;
} BIOSCMD_t;

typedef struct {
    uint8_t command;
    uint8_t id_lun;
    uint8_t address[4];
    uint8_t secount;
    addr24_t dma_address;
} BIOSCMDRaw_t;

typedef struct {
    uint32_t DataLength;
    uint32_t DataPointer;
    uint8_t TargetId;
    uint8_t LogicalUnit;
    uint8_t ControlRaw;
    uint8_t CDBLength;
    uint8_t CDB[12];
} ESCMD_t;

typedef struct {
    uint8_t DataLength[4];
    uint8_t DataPointer[4];
    uint8_t TargetId;
    uint8_t LogicalUnit;
    uint8_t ControlRaw;
    uint8_t CDBLength;
    uint8_t CDB[12];
} ESCMDRaw_t;

typedef struct {
    uint32_t Segment;
    uint32_t SegmentPointer;
} SGE32_t;

typedef struct {
    uint8_t Segment[4];
    uint8_t SegmentPointer[4];
} SGE32Raw_t;

typedef struct {
    addr24_t Segment;
    addr24_t SegmentPointer;
} SGE_t;

typedef struct {
    uint8_t Count;
    uint8_t Address[4];
} MailboxInitExtended_t;

typedef struct {
    uint8_t uSignature;
    uint8_t uCharacterD;
    uint8_t uHostBusType;
    uint8_t uWideTransferPermittedId0To7;
    uint8_t uWideTransfersActiveId0To7;
    uint8_t SynchronousValuesId8To15[8];
    uint8_t uDisconnectPermittedId8To15;
    uint8_t uReserved2;
    uint8_t uWideTransferPermittedId8To15;
    uint8_t uWideTransfersActiveId8To15;
} buslogic_setup_t;

typedef struct {
    uint8_t uBusType;
    uint8_t uBiosAddress;
    uint8_t u16ScatterGatherLimit[2];
    uint8_t cMailbox;
    uint8_t uMailboxAddressBase[4];
    uint8_t Flags;
    uint8_t aFirmwareRevision[3];
    uint8_t Features;
} ReplyInquireExtendedSetupInformation;

typedef struct {
    uint8_t u8View[256];
} HALocalRAM;
#pragma pack(pop)

struct BUSLOGIC_s {
    CPU_t *cpu;
    DMA_t *dma;
    I8259_t *pic_master;
    I8259_t *pic_slave;
    uint8_t bus;
    uint16_t Base;
    uint8_t Irq;
    uint8_t DmaChannel;
    uint8_t HostID;
    uint8_t flags;
    uint8_t Status;
    uint8_t Interrupt;
    uint8_t IrqEnabled;
    uint8_t Geometry;
    uint8_t Command;
    uint8_t CmdParam;
    uint32_t CmdParamLeft;
    uint16_t DataReply;
    uint16_t DataReplyLeft;
    uint8_t DataBuf[65536];
    uint8_t CmdBuf[128];
    uint8_t dma_buffer[64];
    uint8_t temp_cdb[12];
    uint8_t fw_rev[8];
    uint8_t BusOnTime;
    uint8_t BusOffTime;
    uint8_t ATBusSpeed;
    uint32_t target_data_len;
    uint8_t scsi_cmd_phase;
    uint8_t aggressive_round_robin;
    uint8_t ExtendedLUNCCBFormat;
    uint32_t transfer_size;
    uint32_t MailboxInit;
    uint32_t MailboxCount;
    uint32_t MailboxOutAddr;
    uint32_t MailboxOutPosCur;
    uint32_t MailboxInAddr;
    uint32_t MailboxInPosCur;
    uint32_t MailboxReq;
    uint8_t MailboxOutInterrupts;
    uint8_t PendingInterrupt;
    uint8_t ToRaise;
    uint8_t callback_sub_phase;
    uint32_t Outgoing;
    uint32_t mail_timer_id;
    uint32_t reset_timer_id;
    uint8_t rom_loaded;
    uint8_t rom_data[ROM_SIZE];
    uint32_t rom_addr;
    char rom_path[512];
    char nvr_path[512];
    HALocalRAM LocalRAM;
    Req_t Req;
};

static uint8_t buslogic_port_read_cb(void *opaque, uint16_t port);
static void buslogic_port_write_cb(void *opaque, uint16_t port, uint8_t value);
static void buslogic_process_mail(BUSLOGIC_t *dev);
static void buslogic_process_mail_cb(void *opaque);
static void buslogic_schedule_mail(BUSLOGIC_t *dev);
static void buslogic_cmd_phase1(BUSLOGIC_t *dev);
static void buslogic_req_setup(BUSLOGIC_t *dev, uint32_t ccb_pointer);
static void buslogic_scsi_cmd(BUSLOGIC_t *dev);
static void buslogic_scsi_cmd_phase1(BUSLOGIC_t *dev);
static void buslogic_request_sense_phase(BUSLOGIC_t *dev);
static void buslogic_notify(BUSLOGIC_t *dev);

static uint32_t addr24_to_u32(addr24_t value)
{
    return ((uint32_t) value.hi << 16) | ((uint32_t) value.mid << 8) | (uint32_t) value.lo;
}

static uint32_t buslogic_read_addr24(const uint8_t *bytes)
{
    return ((uint32_t) bytes[0] << 16) | ((uint32_t) bytes[1] << 8) | (uint32_t) bytes[2];
}

static addr24_t u32_to_addr24(uint32_t value)
{
    addr24_t ret;
    ret.hi = (uint8_t) (value >> 16);
    ret.mid = (uint8_t) (value >> 8);
    ret.lo = (uint8_t) value;
    return ret;
}

enum {
    BUSLOGIC_AUTOSCSI_BASE = 64,
    BUSLOGIC_AUTOSCSI_SIGNATURE_0 = BUSLOGIC_AUTOSCSI_BASE + 0,
    BUSLOGIC_AUTOSCSI_SIGNATURE_1 = BUSLOGIC_AUTOSCSI_BASE + 1,
    BUSLOGIC_AUTOSCSI_INFO_LENGTH = BUSLOGIC_AUTOSCSI_BASE + 2,
    BUSLOGIC_AUTOSCSI_HOST_ADAPTER_TYPE = BUSLOGIC_AUTOSCSI_BASE + 3,
    BUSLOGIC_AUTOSCSI_DMA_CHANNEL = BUSLOGIC_AUTOSCSI_BASE + 11,
    BUSLOGIC_AUTOSCSI_IRQ_CHANNEL = BUSLOGIC_AUTOSCSI_BASE + 12,
    BUSLOGIC_AUTOSCSI_DMA_TRANSFER_RATE = BUSLOGIC_AUTOSCSI_BASE + 13,
    BUSLOGIC_AUTOSCSI_SCSI_ID = BUSLOGIC_AUTOSCSI_BASE + 14,
    BUSLOGIC_AUTOSCSI_SCSI_CONFIGURATION = BUSLOGIC_AUTOSCSI_BASE + 15,
    BUSLOGIC_AUTOSCSI_BUS_ON_DELAY = BUSLOGIC_AUTOSCSI_BASE + 16,
    BUSLOGIC_AUTOSCSI_BUS_OFF_DELAY = BUSLOGIC_AUTOSCSI_BASE + 17,
    BUSLOGIC_AUTOSCSI_BIOS_CONFIGURATION = BUSLOGIC_AUTOSCSI_BASE + 18,
    BUSLOGIC_AUTOSCSI_DEVICE_ENABLED_MASK = BUSLOGIC_AUTOSCSI_BASE + 19,
    BUSLOGIC_AUTOSCSI_FAST_PERMITTED_MASK = BUSLOGIC_AUTOSCSI_BASE + 23,
    BUSLOGIC_AUTOSCSI_SYNC_PERMITTED_MASK = BUSLOGIC_AUTOSCSI_BASE + 25,
    BUSLOGIC_AUTOSCSI_DISCONNECT_PERMITTED_MASK = BUSLOGIC_AUTOSCSI_BASE + 27,
    BUSLOGIC_AUTOSCSI_ROUND_ROBIN_FLAGS = BUSLOGIC_AUTOSCSI_BASE + 33,
    BUSLOGIC_AUTOSCSI_MAXIMUM_LUN = BUSLOGIC_AUTOSCSI_BASE + 41,
    BUSLOGIC_AUTOSCSI_BIOS_FEATURES = BUSLOGIC_AUTOSCSI_BASE + 43,
    BUSLOGIC_AUTOSCSI_BOOT_ORDER_FLAGS = BUSLOGIC_AUTOSCSI_BASE + 45
};

#define BUSLOGIC_AUTOSCSI_FLAG_ROUND_ROBIN 0x10
#define BUSLOGIC_AUTOSCSI_FLAG_INT13_EXTENSION 0x01
#define BUSLOGIC_AUTOSCSI_FLAG_CDROM_BOOT 0x04
#define BUSLOGIC_AUTOSCSI_FLAG_MULTI_BOOT 0x20
#define BUSLOGIC_AUTOSCSI_FLAG_FORCE_BUS_SCAN_ORDER 0x01

static uint16_t buslogic_localram_read_le16(const BUSLOGIC_t *dev, uint32_t offset)
{
    return host_read_le16_unaligned(&dev->LocalRAM.u8View[offset]);
}

static void buslogic_localram_write_le16(BUSLOGIC_t *dev, uint32_t offset, uint16_t value)
{
    host_write_le16_unaligned(&dev->LocalRAM.u8View[offset], value);
}

static void buslogic_parse_mailbox32(Mailbox32_t *dest, const Mailbox32Raw_t *src)
{
    dest->CCBPointer = host_read_le32_unaligned(src->CCBPointer);
    dest->HostStatus = src->HostStatus;
    dest->TargetStatus = src->TargetStatus;
    dest->Reserved = src->Reserved;
    dest->CompletionCode = src->CompletionCode;
}

static void buslogic_build_mailbox32(Mailbox32Raw_t *dest, const Mailbox32_t *src)
{
    memset(dest, 0, sizeof(*dest));
    host_write_le32_unaligned(dest->CCBPointer, src->CCBPointer);
    dest->HostStatus = src->HostStatus;
    dest->TargetStatus = src->TargetStatus;
    dest->Reserved = src->Reserved;
    dest->CompletionCode = src->CompletionCode;
}

static void buslogic_parse_ccb32(CCB32_t *dest, const CCB32Raw_t *src)
{
    memset(dest, 0, sizeof(*dest));
    dest->Opcode = src->Opcode;
    dest->ControlRaw = src->ControlRaw;
    dest->CdbLength = src->CdbLength;
    dest->RequestSenseLength = src->RequestSenseLength;
    dest->DataLength = host_read_le32_unaligned(src->DataLength);
    dest->DataPointer = host_read_le32_unaligned(src->DataPointer);
    memcpy(dest->Reserved2, src->Reserved2, sizeof(dest->Reserved2));
    dest->HostStatus = src->HostStatus;
    dest->TargetStatus = src->TargetStatus;
    dest->Id = src->Id;
    dest->LunRaw = src->LunRaw;
    memcpy(dest->Cdb, src->Cdb, sizeof(dest->Cdb));
    memcpy(dest->Reserved3, src->Reserved3, sizeof(dest->Reserved3));
    dest->SensePointer = host_read_le32_unaligned(src->SensePointer);
}

static void buslogic_parse_escmd(ESCMD_t *dest, const ESCMDRaw_t *src)
{
    memset(dest, 0, sizeof(*dest));
    dest->DataLength = host_read_le32_unaligned(src->DataLength);
    dest->DataPointer = host_read_le32_unaligned(src->DataPointer);
    dest->TargetId = src->TargetId;
    dest->LogicalUnit = src->LogicalUnit;
    dest->ControlRaw = src->ControlRaw;
    dest->CDBLength = src->CDBLength;
    memcpy(dest->CDB, src->CDB, sizeof(dest->CDB));
}

static void buslogic_parse_bioscmd(BIOSCMD_t *dest, const BIOSCMDRaw_t *src)
{
    memset(dest, 0, sizeof(*dest));
    dest->command = src->command;
    dest->lun = (uint8_t) (src->id_lun & 0x07);
    dest->id = (uint8_t) ((src->id_lun >> 5) & 0x07);
    memcpy(dest->address, src->address, sizeof(dest->address));
    dest->secount = src->secount;
    dest->dma_address = src->dma_address;
}

static uint8_t req_control_byte(const Req_t *req)
{
    return req->Is24bit ? (uint8_t) ((req->CmdBlock.old_ccb.AddrControl >> 3) & 0x03) : (uint8_t) ((req->CmdBlock.new_ccb.ControlRaw >> 3) & 0x03);
}

static uint8_t req_opcode(const Req_t *req)
{
    return req->Is24bit ? req->CmdBlock.old_ccb.Opcode : req->CmdBlock.new_ccb.Opcode;
}

static uint8_t req_cdb_length(const Req_t *req)
{
    return req->Is24bit ? req->CmdBlock.old_ccb.CdbLength : req->CmdBlock.new_ccb.CdbLength;
}

static uint8_t req_request_sense_length(const Req_t *req)
{
    return req->Is24bit ? req->CmdBlock.old_ccb.RequestSenseLength : req->CmdBlock.new_ccb.RequestSenseLength;
}

static uint8_t *req_cdb_ptr(Req_t *req)
{
    return req->Is24bit ? req->CmdBlock.old_ccb.Cdb : req->CmdBlock.new_ccb.Cdb;
}

static uint32_t req_data_length(const Req_t *req)
{
    return req->Is24bit ? addr24_to_u32(req->CmdBlock.old_ccb.DataLength) : req->CmdBlock.new_ccb.DataLength;
}

static uint32_t req_data_pointer(const Req_t *req)
{
    return req->Is24bit ? addr24_to_u32(req->CmdBlock.old_ccb.DataPointer) : req->CmdBlock.new_ccb.DataPointer;
}

static uint32_t req_sense_pointer(const Req_t *req)
{
    if (req->Is24bit) {
        return req->CCBPointer + 0x1e;
    }
    return req->CmdBlock.new_ccb.SensePointer;
}

static void buslogic_log(const char *fmt, ...)
{
    char buffer[512];
    va_list ap;

    va_start(ap, fmt);
#if defined(_MSC_VER) && (_MSC_VER < 1900)
    _vsnprintf(buffer, sizeof(buffer), fmt, ap);
#else
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
#endif
    va_end(ap);
    buffer[sizeof(buffer) - 1] = 0;
    debug_log(DEBUG_DETAIL, "%s", buffer);
}

static uint64_t buslogic_interval_from_us(double usec)
{
    double ticks = (usec / 1000000.0) * (double) timing_getFreq();
    if (ticks < 1.0) {
        ticks = 1.0;
    }
    return (uint64_t) ticks;
}

static void buslogic_schedule_mail(BUSLOGIC_t *dev)
{
    timing_updateInterval(dev->mail_timer_id, buslogic_interval_from_us(5.0));
    timing_timerEnable(dev->mail_timer_id);
}

static void buslogic_cmd_phase1(BUSLOGIC_t *dev)
{
    if ((dev->Command == 0x90) && (dev->CmdParam == 2)) {
        dev->CmdParamLeft = dev->CmdBuf[1];
    }
}

static void buslogic_process_mail_cb(void *opaque)
{
    BUSLOGIC_t *dev = (BUSLOGIC_t *) opaque;

    timing_timerDisable(dev->mail_timer_id);
    switch (dev->callback_sub_phase) {
    case 0:
        buslogic_process_mail(dev);
        break;
    case 1:
        buslogic_scsi_cmd(dev);
        break;
    case 2:
        buslogic_scsi_cmd_phase1(dev);
        break;
    case 3:
        buslogic_request_sense_phase(dev);
        break;
    case 4:
        buslogic_notify(dev);
        break;
    default:
        dev->callback_sub_phase = 0;
        break;
    }

    if ((dev->callback_sub_phase != 0) || (dev->MailboxReq != 0)) {
        buslogic_schedule_mail(dev);
    }
}

static void buslogic_reset_timer_cb(void *opaque)
{
    BUSLOGIC_t *dev = (BUSLOGIC_t *) opaque;

    timing_timerDisable(dev->reset_timer_id);
    dev->Status = STAT_INIT | STAT_IDLE;
}

static void buslogic_set_irq_line(BUSLOGIC_t *dev, int set)
{
    if (dev->Irq >= 8) {
        if (dev->pic_slave == NULL) {
            return;
        }
        if (set) {
            i8259_doirq(dev->pic_slave, (uint8_t) (dev->Irq - 8));
        } else {
            i8259_clearirq(dev->pic_slave, (uint8_t) (dev->Irq - 8));
        }
    } else if (set) {
        i8259_doirq(dev->pic_master, dev->Irq);
    } else {
        i8259_clearirq(dev->pic_master, dev->Irq);
    }
}

static void buslogic_raise_irq(BUSLOGIC_t *dev, int suppress, uint8_t interrupt)
{
    //buslogic_log("[BusLogic] raise_irq int=%02X suppress=%d irq_en=%u current=%02X pending=%02X\r\n",
    //    interrupt, suppress, dev->IrqEnabled, dev->Interrupt, dev->PendingInterrupt);

    if ((interrupt & (INTR_MBIF | INTR_MBOA)) != 0) {
        if ((dev->Interrupt & INTR_HACC) == 0) {
            dev->Interrupt |= interrupt;
        } else {
            dev->PendingInterrupt |= interrupt;
        }
    } else if ((interrupt & INTR_HACC) != 0) {
        dev->Interrupt |= interrupt;
    }

    dev->Interrupt |= INTR_ANY;
    if (dev->IrqEnabled && !suppress) {
        buslogic_set_irq_line(dev, 1);
    }
}

static void buslogic_clear_irq(BUSLOGIC_t *dev)
{
    //buslogic_log("[BusLogic] clear_irq current=%02X pending=%02X\r\n", dev->Interrupt, dev->PendingInterrupt);
    dev->Interrupt = 0;
    buslogic_set_irq_line(dev, 0);
    if (dev->PendingInterrupt != 0) {
        uint8_t pending = dev->PendingInterrupt;

        dev->PendingInterrupt = 0;
        buslogic_raise_irq(dev, 0, pending);
    }
}

static uint8_t completion_code(uint8_t *sense)
{
    switch (sense[12]) {
    case ASC_NONE:
        return 0x00;
    case ASC_ILLEGAL_OPCODE:
    case ASC_INV_FIELD_IN_CMD_PACKET:
    case ASC_INV_FIELD_IN_PARAMETER_LIST:
        return 0x01;
    case ASC_LBA_OUT_OF_RANGE:
        return 0x02;
    case ASC_WRITE_PROTECTED:
        return 0x03;
    case ASC_INCOMPATIBLE_FORMAT:
        return 0x0c;
    case ASC_NOT_READY:
    case ASC_MEDIUM_MAY_HAVE_CHANGED:
    case ASC_CAPACITY_DATA_CHANGED:
    case ASC_MEDIUM_NOT_PRESENT:
        return 0xaa;
    default:
        return 0xff;
    }
}

static void buslogic_save_nvr(BUSLOGIC_t *dev)
{
    FILE *fp;

    if (dev->nvr_path[0] == 0) {
        return;
    }

    fp = fopen(dev->nvr_path, "wb");
    if (fp == NULL) {
        return;
    }

    (void) fwrite(dev->LocalRAM.u8View, 1, sizeof(dev->LocalRAM.u8View), fp);
    fclose(fp);
}

static void buslogic_autoscsi_defaults(BUSLOGIC_t *dev, uint8_t safe)
{
    HALocalRAM *ram = &dev->LocalRAM;
    uint8_t bios_features = 0;
    uint8_t round_robin_flags = safe ? BUSLOGIC_AUTOSCSI_FLAG_ROUND_ROBIN : 0;

    memset(ram, 0, sizeof(*ram));
    ram->u8View[BUSLOGIC_AUTOSCSI_SIGNATURE_0] = 'F';
    ram->u8View[BUSLOGIC_AUTOSCSI_SIGNATURE_1] = 'A';
    ram->u8View[BUSLOGIC_AUTOSCSI_INFO_LENGTH] = 64;
    memcpy(&ram->u8View[BUSLOGIC_AUTOSCSI_HOST_ADAPTER_TYPE + 1], "545S", 4);

    switch (dev->DmaChannel) {
    case 5: ram->u8View[BUSLOGIC_AUTOSCSI_DMA_CHANNEL] = 1; break;
    case 6: ram->u8View[BUSLOGIC_AUTOSCSI_DMA_CHANNEL] = 2; break;
    case 7: ram->u8View[BUSLOGIC_AUTOSCSI_DMA_CHANNEL] = 3; break;
    default: ram->u8View[BUSLOGIC_AUTOSCSI_DMA_CHANNEL] = 0; break;
    }

    switch (dev->Irq) {
    case 9: ram->u8View[BUSLOGIC_AUTOSCSI_IRQ_CHANNEL] = 1; break;
    case 10: ram->u8View[BUSLOGIC_AUTOSCSI_IRQ_CHANNEL] = 2; break;
    case 11: ram->u8View[BUSLOGIC_AUTOSCSI_IRQ_CHANNEL] = 3; break;
    case 12: ram->u8View[BUSLOGIC_AUTOSCSI_IRQ_CHANNEL] = 4; break;
    case 14: ram->u8View[BUSLOGIC_AUTOSCSI_IRQ_CHANNEL] = 5; break;
    case 15: ram->u8View[BUSLOGIC_AUTOSCSI_IRQ_CHANNEL] = 6; break;
    default: ram->u8View[BUSLOGIC_AUTOSCSI_IRQ_CHANNEL] = 0; break;
    }

    ram->u8View[BUSLOGIC_AUTOSCSI_DMA_TRANSFER_RATE] = 1;
    ram->u8View[BUSLOGIC_AUTOSCSI_SCSI_ID] = 7;
    ram->u8View[BUSLOGIC_AUTOSCSI_SCSI_CONFIGURATION] = 0x3f;
    ram->u8View[BUSLOGIC_AUTOSCSI_BUS_ON_DELAY] = 7;
    ram->u8View[BUSLOGIC_AUTOSCSI_BUS_OFF_DELAY] = 4;
    ram->u8View[BUSLOGIC_AUTOSCSI_BIOS_CONFIGURATION] = (dev->rom_addr != 0) ? 0x33 : 0x32;
    if (!safe) {
        ram->u8View[BUSLOGIC_AUTOSCSI_BIOS_CONFIGURATION] |= 0x04;
        bios_features |= BUSLOGIC_AUTOSCSI_FLAG_INT13_EXTENSION | BUSLOGIC_AUTOSCSI_FLAG_CDROM_BOOT | BUSLOGIC_AUTOSCSI_FLAG_MULTI_BOOT;
    }

    buslogic_localram_write_le16(dev, BUSLOGIC_AUTOSCSI_DEVICE_ENABLED_MASK, 0xffff);
    buslogic_localram_write_le16(dev, BUSLOGIC_AUTOSCSI_FAST_PERMITTED_MASK, 0xffff);
    buslogic_localram_write_le16(dev, BUSLOGIC_AUTOSCSI_DISCONNECT_PERMITTED_MASK, 0xffff);
    ram->u8View[BUSLOGIC_AUTOSCSI_MAXIMUM_LUN] = 7;
    ram->u8View[BUSLOGIC_AUTOSCSI_BOOT_ORDER_FLAGS] = BUSLOGIC_AUTOSCSI_FLAG_FORCE_BUS_SCAN_ORDER;
    ram->u8View[BUSLOGIC_AUTOSCSI_BIOS_FEATURES] = bios_features;
    ram->u8View[BUSLOGIC_AUTOSCSI_ROUND_ROBIN_FLAGS] = round_robin_flags;
}

static void buslogic_load_nvr(BUSLOGIC_t *dev)
{
    FILE *fp;
    size_t read_len;

    buslogic_autoscsi_defaults(dev, 0);
    if (dev->nvr_path[0] == 0) {
        return;
    }

    fp = fopen(dev->nvr_path, "rb");
    if (fp == NULL) {
        buslogic_save_nvr(dev);
        return;
    }

    read_len = fread(dev->LocalRAM.u8View, 1, sizeof(dev->LocalRAM.u8View), fp);
    fclose(fp);

    if (read_len != sizeof(dev->LocalRAM.u8View)) {
        buslogic_autoscsi_defaults(dev, 0);
        buslogic_save_nvr(dev);
    }
}

static void buslogic_register_io(BUSLOGIC_t *dev)
{
    ports_cbRegister(dev->Base, 4, buslogic_port_read_cb, NULL, buslogic_port_write_cb, NULL, dev);
}

static uint8_t buslogic_rom_read(void *opaque, uint32_t addr)
{
    BUSLOGIC_t *dev = (BUSLOGIC_t *) opaque;

    if (!dev->rom_loaded || (addr < dev->rom_addr) || (addr >= (dev->rom_addr + ROM_SIZE))) {
        return 0xff;
    }

    return dev->rom_data[addr - dev->rom_addr];
}

static void buslogic_rom_write(void *opaque, uint32_t addr, uint8_t value)
{
    (void) opaque;
    (void) addr;
    (void) value;
}

static void buslogic_reset(BUSLOGIC_t *dev, int hard_reset)
{
    uint8_t i;

    buslogic_clear_irq(dev);
    dev->Geometry = 0x90;
    dev->Command = 0xff;
    dev->CmdParam = 0;
    dev->CmdParamLeft = 0;
    dev->DataReply = 0;
    dev->DataReplyLeft = 0;
    dev->flags |= BUSLOGIC_FLAG_MBX_24BIT;
    dev->MailboxOutInterrupts = 0;
    dev->PendingInterrupt = 0;
    dev->MailboxInPosCur = 0;
    dev->MailboxOutPosCur = 0;
    dev->MailboxCount = 0;
    dev->MailboxReq = 0;
    dev->IrqEnabled = 1;
    dev->target_data_len = 0;
    dev->ToRaise = 0;
    dev->callback_sub_phase = 0;
    dev->Outgoing = 0;
    timing_timerDisable(dev->mail_timer_id);

    for (i = 0; i < SCSI_ID_MAX; i++) {
        scsi_device_reset(&scsi_devices[dev->bus][i]);
    }

    if (hard_reset) {
        dev->Status = STAT_STST;
        timing_updateInterval(dev->reset_timer_id, buslogic_interval_from_us(BUSLOGIC_RESET_DELAY_US));
        timing_timerEnable(dev->reset_timer_id);
    } else {
        dev->Status = STAT_INIT | STAT_IDLE;
    }
}

static void buslogic_rd_sge(BUSLOGIC_t *dev, int is24bit, uint32_t address, SGE32_t *sg)
{
    if (is24bit) {
        SGE_t sg24;

        memset(&sg24, 0, sizeof(sg24));
        dma_bm_read(dev->dma, address, (uint8_t *) &sg24, sizeof(sg24), (int) dev->transfer_size);
        sg->Segment = addr24_to_u32(sg24.Segment);
        sg->SegmentPointer = addr24_to_u32(sg24.SegmentPointer);
    } else {
        SGE32Raw_t raw;

        memset(&raw, 0, sizeof(raw));
        dma_bm_read(dev->dma, address, (uint8_t *) &raw, sizeof(raw), (int) dev->transfer_size);
        sg->Segment = host_read_le32_unaligned(raw.Segment);
        sg->SegmentPointer = host_read_le32_unaligned(raw.SegmentPointer);
    }
}

static int buslogic_get_length(BUSLOGIC_t *dev, Req_t *req, int is24bit)
{
    uint32_t data_pointer = req_data_pointer(req);
    uint32_t data_length = req_data_length(req);
    uint32_t sg_entry_len = is24bit ? sizeof(SGE_t) : sizeof(SGE32Raw_t);
    uint32_t total = 0;
    SGE32_t sg;
    uint8_t opcode = req_opcode(req);
    uint32_t i;

    (void) dev;

    if ((data_length == 0) || (req_control_byte(req) == 0x03)) {
        return 0;
    }

    if ((opcode == SCATTER_GATHER_COMMAND) || (opcode == SCATTER_GATHER_COMMAND_RES)) {
        for (i = 0; i < data_length; i += sg_entry_len) {
            buslogic_rd_sge(dev, is24bit, data_pointer + i, &sg);
            total += sg.Segment;
        }
        return (int) total;
    }

    if ((opcode == SCSI_INITIATOR_COMMAND) || (opcode == SCSI_INITIATOR_COMMAND_RES)) {
        return (int) data_length;
    }

    return 0;
}

static void buslogic_set_residue(BUSLOGIC_t *dev, Req_t *req, int32_t transfer_length)
{
    uint32_t residue = 0;
    int32_t buf_len = scsi_devices[dev->bus][req->TargetID].buffer_length;
    uint8_t opcode = req_opcode(req);

    if ((opcode != SCSI_INITIATOR_COMMAND_RES) && (opcode != SCATTER_GATHER_COMMAND_RES)) {
        return;
    }

    if ((transfer_length > 0) && (req_control_byte(req) < 0x03)) {
        transfer_length -= buf_len;
        if (transfer_length > 0) {
            residue = (uint32_t) transfer_length;
        }
    }

    if (req->Is24bit) {
        addr24_t res24 = u32_to_addr24(residue);
        uint8_t bytes[4] = { 0, 0, 0, 0 };

        dma_bm_read(dev->dma, req->CCBPointer + 0x04, bytes, sizeof(bytes), (int) dev->transfer_size);
        memcpy(bytes, &res24, 3);
        dma_bm_write(dev->dma, req->CCBPointer + 0x04, bytes, sizeof(bytes), (int) dev->transfer_size);
    } else {
        uint8_t bytes[4];

        host_write_le32_unaligned(bytes, residue);
        dma_bm_write(dev->dma, req->CCBPointer + 0x04, bytes, sizeof(bytes), (int) dev->transfer_size);
    }
}

static void buslogic_buf_dma_transfer(BUSLOGIC_t *dev, Req_t *req, int is24bit, int transfer_length, int dir)
{
    uint32_t data_pointer = req_data_pointer(req);
    uint32_t data_length = req_data_length(req);
    uint32_t sg_entry_len = is24bit ? sizeof(SGE_t) : sizeof(SGE32Raw_t);
    int32_t buf_len = scsi_devices[dev->bus][req->TargetID].buffer_length;
    uint8_t *temp_buffer = scsi_devices[dev->bus][req->TargetID].sc->temp_buffer;
    uint8_t read_from_host = dir && ((req_control_byte(req) == CCB_DATA_XFER_OUT) || (req_control_byte(req) == 0x00));
    uint8_t write_to_host = (!dir) && ((req_control_byte(req) == CCB_DATA_XFER_IN) || (req_control_byte(req) == 0x00));
    uint8_t opcode = req_opcode(req);
    uint32_t i;
    int sg_pos = 0;

    if ((req_control_byte(req) == 0x03) || (transfer_length == 0) || (buf_len <= 0)) {
        return;
    }

    if ((opcode == SCATTER_GATHER_COMMAND) || (opcode == SCATTER_GATHER_COMMAND_RES)) {
        for (i = 0; i < data_length; i += sg_entry_len) {
            SGE32_t sg;
            uint32_t data_to_transfer;

            buslogic_rd_sge(dev, is24bit, data_pointer + i, &sg);
            data_to_transfer = MIN_VAL((uint32_t) buf_len, sg.Segment);
            if (read_from_host && (data_to_transfer != 0)) {
                dma_bm_read(dev->dma, sg.SegmentPointer, &temp_buffer[sg_pos], data_to_transfer, (int) dev->transfer_size);
            } else if (write_to_host && (data_to_transfer != 0)) {
                dma_bm_write(dev->dma, sg.SegmentPointer, &temp_buffer[sg_pos], data_to_transfer, (int) dev->transfer_size);
            }

            sg_pos += (int) data_to_transfer;
            buf_len -= (int) data_to_transfer;
            if (buf_len <= 0) {
                break;
            }
        }
    } else if ((opcode == SCSI_INITIATOR_COMMAND) || (opcode == SCSI_INITIATOR_COMMAND_RES)) {
        uint32_t data_to_transfer = MIN_VAL((uint32_t) buf_len, data_length);

        if (read_from_host) {
            dma_bm_read(dev->dma, data_pointer, temp_buffer, data_to_transfer, (int) dev->transfer_size);
        } else if (write_to_host) {
            dma_bm_write(dev->dma, data_pointer, temp_buffer, data_to_transfer, (int) dev->transfer_size);
        }
    }
}

static uint8_t buslogic_request_sense_length(uint8_t value)
{
    if (value == 0) {
        return 14;
    }
    if (value == 1) {
        return 0;
    }
    return value;
}

static void buslogic_sense_buffer_write(BUSLOGIC_t *dev, Req_t *req, int copy)
{
    uint8_t sense_len = buslogic_request_sense_length(req_request_sense_length(req));
    uint32_t sense_addr;
    uint8_t temp_sense[256];

    if (!copy || (sense_len == 0)) {
        return;
    }

    scsi_device_request_sense(&scsi_devices[dev->bus][req->TargetID], temp_sense, sense_len);
    sense_addr = req_sense_pointer(req);
    dma_bm_write(dev->dma, sense_addr, temp_sense, sense_len, (int) dev->transfer_size);
}

static void buslogic_mbi_setup(BUSLOGIC_t *dev, uint8_t host_status, uint8_t target_status, uint8_t mbcc)
{
    Req_t *req = &dev->Req;

    req->HostStatus = host_status;
    req->TargetStatus = target_status;
    req->MailboxCompletionCode = mbcc;
}

static void buslogic_write_ccb_status(BUSLOGIC_t *dev, Req_t *req)
{
    uint8_t bytes[4] = { 0, 0, 0, 0 };

    if (req->MailboxCompletionCode == MBI_NOT_FOUND) {
        return;
    }

    dma_bm_read(dev->dma, req->CCBPointer + 0x0c, bytes, sizeof(bytes), (int) dev->transfer_size);
    bytes[2] = req->HostStatus;
    bytes[3] = req->TargetStatus;
    dma_bm_write(dev->dma, req->CCBPointer + 0x0c, bytes, sizeof(bytes), (int) dev->transfer_size);
}

static void buslogic_write_mbo_free(BUSLOGIC_t *dev)
{
    uint8_t free_code = MBO_FREE;
    uint32_t outgoing = dev->Outgoing;
    uint32_t offset = (dev->flags & BUSLOGIC_FLAG_MBX_24BIT) ? 0 : 7;

    if (outgoing == 0) {
        outgoing = dev->MailboxOutAddr + (dev->MailboxOutPosCur * ((dev->flags & BUSLOGIC_FLAG_MBX_24BIT) ? sizeof(Mailbox_t) : sizeof(Mailbox32Raw_t)));
    }
    dma_bm_write(dev->dma, outgoing + offset, &free_code, 1, (int) dev->transfer_size);
}

static void buslogic_write_mbi(BUSLOGIC_t *dev)
{
    Req_t *req = &dev->Req;
    uint32_t incoming = dev->MailboxInAddr + (dev->MailboxInPosCur * ((dev->flags & BUSLOGIC_FLAG_MBX_24BIT) ? sizeof(Mailbox_t) : sizeof(Mailbox32Raw_t)));

    if (req->MailboxCompletionCode != MBI_NOT_FOUND) {
        buslogic_write_ccb_status(dev, req);
    }

    if (dev->flags & BUSLOGIC_FLAG_MBX_24BIT) {
        Mailbox_t mbi;

        mbi.CmdStatus = req->MailboxCompletionCode;
        mbi.CCBPointer = u32_to_addr24(req->CCBPointer);
        dma_bm_write(dev->dma, incoming, (uint8_t *) &mbi, sizeof(mbi), (int) dev->transfer_size);
    } else {
        Mailbox32Raw_t mbi32;
        Mailbox32_t mbi;

        memset(&mbi, 0, sizeof(mbi));
        mbi.CCBPointer = req->CCBPointer;
        mbi.HostStatus = req->HostStatus;
        mbi.TargetStatus = req->TargetStatus;
        mbi.CompletionCode = req->MailboxCompletionCode;
        buslogic_build_mailbox32(&mbi32, &mbi);
        dma_bm_write(dev->dma, incoming, (uint8_t *) &mbi32, sizeof(mbi32), (int) dev->transfer_size);
    }

    dev->MailboxInPosCur++;
    if (dev->MailboxInPosCur >= dev->MailboxCount) {
        dev->MailboxInPosCur = 0;
    }

    dev->ToRaise = INTR_MBIF | INTR_ANY;
    if (dev->MailboxOutInterrupts) {
        dev->ToRaise |= INTR_MBOA;
    }
}

static void buslogic_request_sense_phase(BUSLOGIC_t *dev)
{
    Req_t *req = &dev->Req;
    scsi_device_t *sd = &scsi_devices[dev->bus][req->TargetID];

    if ((dev->scsi_cmd_phase != SCSI_PHASE_STATUS) && (dev->temp_cdb[0] == GPCMD_REQUEST_SENSE) && (req_control_byte(req) == 0x03)) {
        uint8_t sense_len = buslogic_request_sense_length(req_request_sense_length(req));

        if ((sd->status != SCSI_STATUS_OK) && (sense_len > 0)) {
            uint32_t sense_addr = req_sense_pointer(req);
            dma_bm_write(dev->dma, sense_addr, sd->sc->temp_buffer, sense_len, (int) dev->transfer_size);
        }
        scsi_device_command_phase1(sd);
    } else {
        buslogic_sense_buffer_write(dev, req, (sd->status != SCSI_STATUS_OK));
    }

    buslogic_set_residue(dev, req, (int32_t) dev->target_data_len);
    if (sd->status == SCSI_STATUS_OK) {
        buslogic_mbi_setup(dev, CCB_COMPLETE, SCSI_STATUS_OK, MBI_SUCCESS);
    } else {
        buslogic_mbi_setup(dev, CCB_COMPLETE, SCSI_STATUS_CHECK_CONDITION, MBI_ERROR);
    }

    dev->callback_sub_phase = 4;
}

static void buslogic_scsi_cmd(BUSLOGIC_t *dev)
{
    Req_t *req = &dev->Req;
    scsi_device_t *sd = &scsi_devices[dev->bus][req->TargetID];
    uint8_t cdb_len;

    dev->target_data_len = (uint32_t) buslogic_get_length(dev, req, req->Is24bit);
    cdb_len = req_cdb_length(req);
    memset(dev->temp_cdb, 0, sizeof(dev->temp_cdb));
    memcpy(dev->temp_cdb, req_cdb_ptr(req), MIN_VAL(cdb_len, (uint8_t) sizeof(dev->temp_cdb)));

    sd->buffer_length = (int32_t) dev->target_data_len;
    scsi_device_command_phase0(sd, dev->temp_cdb);
    dev->scsi_cmd_phase = sd->phase;
    dev->callback_sub_phase = (dev->scsi_cmd_phase == SCSI_PHASE_STATUS) ? 3 : 2;
}

static void buslogic_scsi_cmd_phase1(BUSLOGIC_t *dev)
{
    Req_t *req = &dev->Req;
    scsi_device_t *sd = &scsi_devices[dev->bus][req->TargetID];

    if (dev->scsi_cmd_phase != SCSI_PHASE_STATUS) {
        if ((dev->temp_cdb[0] != GPCMD_REQUEST_SENSE) || (req_control_byte(req) != 0x03)) {
            buslogic_buf_dma_transfer(dev, req, req->Is24bit, (int) dev->target_data_len, (dev->scsi_cmd_phase == SCSI_PHASE_DATA_OUT));
            scsi_device_command_phase1(sd);
        }
    }

    dev->callback_sub_phase = 3;
}

static void buslogic_read_req(BUSLOGIC_t *dev, uint32_t ccb_pointer)
{
    Req_t *req = &dev->Req;

    memset(req, 0, sizeof(*req));
    req->Is24bit = !!(dev->flags & BUSLOGIC_FLAG_MBX_24BIT);
    req->CCBPointer = ccb_pointer;
    if (req->Is24bit) {
        dma_bm_read(dev->dma, ccb_pointer, (uint8_t *) &req->CmdBlock.old_ccb, (uint32_t) sizeof(req->CmdBlock.old_ccb), (int) dev->transfer_size);
    } else {
        CCB32Raw_t raw;

        memset(&raw, 0, sizeof(raw));
        dma_bm_read(dev->dma, ccb_pointer, (uint8_t *) &raw, (uint32_t) sizeof(raw), (int) dev->transfer_size);
        buslogic_parse_ccb32(&req->CmdBlock.new_ccb, &raw);
    }
    req->TargetID = req->Is24bit ? (uint8_t) ((req->CmdBlock.old_ccb.AddrControl >> 5) & 0x07) : req->CmdBlock.new_ccb.Id;
    req->LUN = req->Is24bit ? (uint8_t) (req->CmdBlock.old_ccb.AddrControl & 0x07) : (uint8_t) (req->CmdBlock.new_ccb.LunRaw & 0x1f);
}

static void buslogic_req_setup(BUSLOGIC_t *dev, uint32_t ccb_pointer)
{
    Req_t *req;
    scsi_device_t *sd;

    buslogic_read_req(dev, ccb_pointer);
    req = &dev->Req;
    sd = &scsi_devices[dev->bus][req->TargetID];
    dev->target_data_len = 0;
    dev->scsi_cmd_phase = SCSI_PHASE_STATUS;

    //buslogic_log("[BusLogic] CCB %08X opcode=%02X target=%u lun=%u\r\n",
    //    req->CCBPointer, req_opcode(req), req->TargetID, req->LUN);

    if ((req->TargetID > 7) || (req->LUN > 7) || !scsi_device_present(sd) || (req->LUN > 0)) {
        //debug_log(DEBUG_INFO, "[BusLogic] selection timeout ccb=%08X target=%u lun=%u\r\n",
        //    req->CCBPointer, req->TargetID, req->LUN);
        buslogic_mbi_setup(dev, CCB_SELECTION_TIMEOUT, SCSI_STATUS_OK, MBI_ERROR);
        dev->callback_sub_phase = 4;
        return;
    }

    scsi_device_identify(sd, req->LUN);
    if (req_opcode(req) == BUS_RESET_OPCODE) {
        scsi_device_reset(sd);
        buslogic_mbi_setup(dev, CCB_COMPLETE, SCSI_STATUS_OK, MBI_SUCCESS);
        dev->callback_sub_phase = 4;
        return;
    }

    if ((req_opcode(req) == TARGET_MODE_COMMAND) || (req_opcode(req) > SCATTER_GATHER_COMMAND_RES)) {
        buslogic_mbi_setup(dev, CCB_INVALID_OP_CODE, SCSI_STATUS_OK, MBI_ERROR);
        dev->callback_sub_phase = 4;
        return;
    }

    dev->callback_sub_phase = 1;
}

static void buslogic_notify(BUSLOGIC_t *dev)
{
    Req_t *req = &dev->Req;
    scsi_device_t *sd = &scsi_devices[dev->bus][req->TargetID];

    /*debug_log(DEBUG_INFO, "[BusLogic] notify ccb=%08X host=%02X target=%02X mbi=%02X irq=%u\r\n",
        req->CCBPointer, req->HostStatus, req->TargetStatus, req->MailboxCompletionCode, dev->Irq);
    buslogic_log("[BusLogic] notify ccb=%08X host=%02X target=%02X mbi=%02X\r\n",
        req->CCBPointer, req->HostStatus, req->TargetStatus, req->MailboxCompletionCode);*/
    buslogic_write_mbo_free(dev);
    buslogic_write_mbi(dev);

    if (scsi_device_present(sd)) {
        scsi_device_identify(sd, SCSI_LUN_USE_CDB);
    }

    dev->Outgoing = 0;
    dev->callback_sub_phase = 0;
    if (dev->ToRaise != 0) {
        buslogic_raise_irq(dev, 0, dev->ToRaise);
    }
}

static int buslogic_process_mbo(BUSLOGIC_t *dev, uint32_t outgoing, Mailbox32_t *mb32)
{
    dev->ToRaise = 0;
    dev->Outgoing = outgoing;

    if (mb32->CompletionCode == MBO_START) {
        //buslogic_log("[BusLogic] Mailbox start at %08X CCB=%08X\r\n", outgoing, mb32->CCBPointer);
        buslogic_req_setup(dev, mb32->CCBPointer);
        return 1;
    }

    if (mb32->CompletionCode == MBO_ABORT) {
        //buslogic_log("[BusLogic] Mailbox abort at %08X CCB=%08X\r\n", outgoing, mb32->CCBPointer);
        buslogic_read_req(dev, mb32->CCBPointer);
        buslogic_mbi_setup(dev, CCB_ABORTED, SCSI_STATUS_OK, MBI_NOT_FOUND);
        dev->callback_sub_phase = 4;
        return 1;
    }

    return 0;
}

static void buslogic_process_mail(BUSLOGIC_t *dev)
{
    uint32_t idx;

    if ((dev->Status & STAT_INIT) || !dev->MailboxInit || (dev->MailboxCount == 0) || (dev->MailboxReq == 0)) {
        return;
    }

    if (dev->aggressive_round_robin) {
        for (idx = 0; idx < dev->MailboxCount; idx++) {
            Mailbox32_t mb32;
            uint32_t outgoing;

            memset(&mb32, 0, sizeof(mb32));
            if (dev->flags & BUSLOGIC_FLAG_MBX_24BIT) {
                Mailbox_t mb24;

                outgoing = dev->MailboxOutAddr + (idx * sizeof(Mailbox_t));
                dma_bm_read(dev->dma, outgoing, (uint8_t *) &mb24, sizeof(mb24), (int) dev->transfer_size);
                mb32.CCBPointer = addr24_to_u32(mb24.CCBPointer);
                mb32.CompletionCode = mb24.CmdStatus;
            } else {
                Mailbox32Raw_t raw32;

                outgoing = dev->MailboxOutAddr + (idx * sizeof(Mailbox32Raw_t));
                memset(&raw32, 0, sizeof(raw32));
                dma_bm_read(dev->dma, outgoing, (uint8_t *) &raw32, sizeof(raw32), (int) dev->transfer_size);
                buslogic_parse_mailbox32(&mb32, &raw32);
            }

            dev->MailboxOutPosCur = idx;
            if (buslogic_process_mbo(dev, outgoing, &mb32)) {
                if (dev->MailboxReq > 0) {
                    dev->MailboxReq--;
                }
                break;
            }
        }
    } else {
        Mailbox32_t mb32;
        uint32_t outgoing;

        memset(&mb32, 0, sizeof(mb32));
        if (dev->flags & BUSLOGIC_FLAG_MBX_24BIT) {
            Mailbox_t mb24;

            outgoing = dev->MailboxOutAddr + (dev->MailboxOutPosCur * sizeof(Mailbox_t));
            dma_bm_read(dev->dma, outgoing, (uint8_t *) &mb24, sizeof(mb24), (int) dev->transfer_size);
            mb32.CCBPointer = addr24_to_u32(mb24.CCBPointer);
            mb32.CompletionCode = mb24.CmdStatus;
        } else {
            Mailbox32Raw_t raw32;

            outgoing = dev->MailboxOutAddr + (dev->MailboxOutPosCur * sizeof(Mailbox32Raw_t));
            memset(&raw32, 0, sizeof(raw32));
            dma_bm_read(dev->dma, outgoing, (uint8_t *) &raw32, sizeof(raw32), (int) dev->transfer_size);
            buslogic_parse_mailbox32(&mb32, &raw32);
        }

        if (buslogic_process_mbo(dev, outgoing, &mb32)) {
            if (dev->MailboxReq > 0) {
                dev->MailboxReq--;
            }
            dev->MailboxOutPosCur++;
            dev->MailboxOutPosCur %= dev->MailboxCount;
        }
    }
}

static void buslogic_cmd_done(BUSLOGIC_t *dev, int suppress)
{
    dev->DataReply = 0;
    dev->Status |= STAT_IDLE;
    if (dev->Command != CMD_START_SCSI) {
        dev->Status &= (uint8_t) ~STAT_DFULL;
        buslogic_raise_irq(dev, suppress, INTR_HACC);
    }
    dev->Command = 0xff;
    dev->CmdParam = 0;
    dev->CmdParamLeft = 0;
}

static uint8_t buslogic_get_param_len(uint8_t command)
{
    switch (command) {
    case 0x25:
    case 0x8B:
    case 0x8C:
    case 0x8D:
    case 0x8F:
    case 0x96:
        return 1;
    case 0x81:
        return (uint8_t) sizeof(MailboxInitExtended_t);
    case 0x83:
        return 12;
    case 0x90:
    case 0x91:
        return 2;
    case 0xFB:
        return 3;
    default:
        return 0;
    }
}

static void buslogic_setup_data(BUSLOGIC_t *dev)
{
    ReplyInquireSetupInformation *reply = (ReplyInquireSetupInformation *) dev->DataBuf;
    buslogic_setup_t *setup = (buslogic_setup_t *) reply->VendorSpecificData;

    reply->fSynchronousInitiationEnabled = buslogic_localram_read_le16(dev, BUSLOGIC_AUTOSCSI_SYNC_PERMITTED_MASK) ? 1 : 0;
    reply->uBusTransferRate = dev->ATBusSpeed;
    reply->uPreemptTimeOnBus = dev->BusOnTime;
    reply->uTimeOffBus = dev->BusOffTime;
    reply->cMailbox = (uint8_t) dev->MailboxCount;
    reply->MailboxAddress = u32_to_addr24(dev->MailboxOutAddr);

    memset(setup, 0, sizeof(*setup));
    setup->uSignature = 'B';
    setup->uCharacterD = 'D';
    setup->uHostBusType = 'A';
}

static uint8_t buslogic_bios_scsi_command(BUSLOGIC_t *dev, scsi_device_t *sd, uint8_t *cdb, uint8_t *buf, int len, uint32_t addr)
{
    sd->buffer_length = -1;
    scsi_device_command_phase0(sd, cdb);
    if (sd->phase == SCSI_PHASE_STATUS) {
        return completion_code(scsi_device_sense(sd));
    }

    if (len > 0) {
        if (sd->phase == SCSI_PHASE_DATA_IN) {
            if (buf != NULL) {
                memcpy(buf, sd->sc->temp_buffer, (size_t) sd->buffer_length);
            } else {
                dma_bm_write(dev->dma, addr, sd->sc->temp_buffer, (uint32_t) sd->buffer_length, (int) dev->transfer_size);
            }
        } else if (sd->phase == SCSI_PHASE_DATA_OUT) {
            if (buf != NULL) {
                memcpy(sd->sc->temp_buffer, buf, (size_t) sd->buffer_length);
            } else {
                dma_bm_read(dev->dma, addr, sd->sc->temp_buffer, (uint32_t) sd->buffer_length, (int) dev->transfer_size);
            }
        }
    }

    scsi_device_command_phase1(sd);
    return completion_code(scsi_device_sense(sd));
}

static uint8_t buslogic_bios_read_capacity(BUSLOGIC_t *dev, scsi_device_t *sd, uint8_t *buf)
{
    uint8_t cdb[12];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = GPCMD_READ_CDROM_CAPACITY;
    memset(buf, 0, 8);
    return buslogic_bios_scsi_command(dev, sd, cdb, buf, 8, 0);
}

static uint8_t buslogic_bios_inquiry(BUSLOGIC_t *dev, scsi_device_t *sd, uint8_t *buf)
{
    uint8_t cdb[12];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = GPCMD_INQUIRY;
    cdb[4] = 36;
    memset(buf, 0, 36);
    return buslogic_bios_scsi_command(dev, sd, cdb, buf, 36, 0);
}

static uint8_t buslogic_bios_command(BUSLOGIC_t *dev, BIOSCMD_t *cmd, int islba)
{
    static const int bios_cmd_to_scsi[18] = {
        0, 0, GPCMD_READ_10, GPCMD_WRITE_10, GPCMD_VERIFY_10, 0, 0, GPCMD_FORMAT_UNIT,
        0, 0, 0, 0, 0x2b, 0, 0, 0, GPCMD_TEST_UNIT_READY, 0x01
    };
    uint8_t cdb[12];
    uint8_t buf[36];
    uint32_t lba;
    uint32_t dma_address;
    uint32_t transfer_len = 0;
    uint8_t ret = 0;
    scsi_device_t *sd;

    if ((cmd->id > 7) || (cmd->lun != 0)) {
        return 0x80;
    }

    sd = &scsi_devices[dev->bus][cmd->id];
    if (!scsi_device_present(sd)) {
        return 0x80;
    }

    scsi_device_identify(sd, 0xff);
    if ((sd->type == SCSI_REMOVABLE_CDROM) && ((dev->flags & BUSLOGIC_FLAG_CDROM_BOOT) == 0)) {
        return 0x80;
    }

    memset(cdb, 0, sizeof(cdb));
    dma_address = addr24_to_u32(cmd->dma_address);
    if (islba) {
        lba = ((uint32_t) cmd->address[0] << 24) | ((uint32_t) cmd->address[1] << 16) |
            ((uint32_t) cmd->address[2] << 8) | (uint32_t) cmd->address[3];
    } else {
        uint16_t cyl = host_read_be16_unaligned(cmd->address);
        uint8_t head = (uint8_t) (cmd->address[2] & 0x0f);
        uint8_t sec = (uint8_t) (cmd->address[3] & 0x1f);

        lba = ((uint32_t) cyl << 9) + ((uint32_t) head << 5) + (uint32_t) sec;
    }

    switch (cmd->command) {
    case 0x00:
    case 0x06:
    case 0x09:
    case 0x0d:
    case 0x0e:
    case 0x0f:
    case 0x14:
        return 0x00;
    case 0x01:
        dma_bm_write(dev->dma, dma_address, scsi_device_sense(sd), 14, (int) dev->transfer_size);
        return 0x00;
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x0c:
        cdb[0] = (uint8_t) bios_cmd_to_scsi[cmd->command];
        cdb[1] = (cmd->lun & 7) << 5;
        cdb[2] = (uint8_t) (lba >> 24);
        cdb[3] = (uint8_t) (lba >> 16);
        cdb[4] = (uint8_t) (lba >> 8);
        cdb[5] = (uint8_t) lba;
        if (cmd->command != 0x0c) {
            cdb[8] = cmd->secount;
            if ((cmd->command == 0x02) || (cmd->command == 0x03)) {
                transfer_len = (uint32_t) cmd->secount * sd->sc->block_len;
            }
        }
        ret = buslogic_bios_scsi_command(dev, sd, cdb, NULL, (int) transfer_len, dma_address);
        return (cmd->command == 0x0c) ? (uint8_t) !!ret : ret;
    case 0x07:
    case 0x10:
    case 0x11:
        cdb[0] = (uint8_t) bios_cmd_to_scsi[cmd->command];
        cdb[1] = (cmd->lun & 7) << 5;
        return buslogic_bios_scsi_command(dev, sd, cdb, NULL, cmd->secount, dma_address);
    case 0x08:
    case 0x15:
        if (cmd->command == 0x08) {
            uint8_t rcbuf[8];
            int i;

            ret = buslogic_bios_read_capacity(dev, sd, rcbuf);
            if (ret) {
                return ret;
            }
            memset(buf, 0, 6);
            for (i = 0; i < 4; i++) {
                buf[i] = rcbuf[i];
            }
            for (i = 4; i < 6; i++) {
                buf[i] = rcbuf[(i + 2) ^ 1];
            }
            dma_bm_write(dev->dma, dma_address, buf, 4, (int) dev->transfer_size);
            return 0;
        } else {
            uint8_t rcbuf[8];

            ret = buslogic_bios_inquiry(dev, sd, buf);
            if (ret) {
                return ret;
            }
            ret = buslogic_bios_read_capacity(dev, sd, rcbuf);
            if (ret) {
                return ret;
            }
            buf[4] = buf[0];
            buf[5] = buf[1];
            memcpy(buf, rcbuf, 4);
            dma_bm_write(dev->dma, dma_address, buf, 4, (int) dev->transfer_size);
            return 0;
        }
    default:
        return 0x01;
    }
}

static void buslogic_bios_dma_transfer(BUSLOGIC_t *dev, ESCMD_t *cmd, uint8_t target_id, int dir)
{
    scsi_device_t *sd = &scsi_devices[dev->bus][target_id];
    uint8_t data_direction = (cmd->ControlRaw >> 3) & 0x03;
    uint32_t transfer_length;

    if ((data_direction == 0x03) || (cmd->DataLength == 0) || (sd->buffer_length <= 0)) {
        return;
    }

    transfer_length = MIN_VAL(cmd->DataLength, (uint32_t) sd->buffer_length);
    if (dir && ((data_direction == CCB_DATA_XFER_OUT) || (data_direction == 0x00))) {
        dma_bm_read(dev->dma, cmd->DataPointer, sd->sc->temp_buffer, transfer_length, (int) dev->transfer_size);
    } else if (!dir && ((data_direction == CCB_DATA_XFER_IN) || (data_direction == 0x00))) {
        dma_bm_write(dev->dma, cmd->DataPointer, sd->sc->temp_buffer, transfer_length, (int) dev->transfer_size);
    }
}

static void buslogic_bios_request_setup(BUSLOGIC_t *dev, uint8_t *cmd_buf, uint8_t *data_in_buf, uint8_t reply_len)
{
    ESCMDRaw_t raw_cmd;
    ESCMD_t cmd;
    scsi_device_t *sd;
    uint8_t temp_cdb[12];
    int phase;

    memset(&raw_cmd, 0, sizeof(raw_cmd));
    memcpy(&raw_cmd, cmd_buf, sizeof(raw_cmd));
    buslogic_parse_escmd(&cmd, &raw_cmd);
    sd = &scsi_devices[dev->bus][cmd.TargetId];

    memset(data_in_buf, 0, 4);
    if ((cmd.TargetId > 15) || (cmd.LogicalUnit > 7)) {
        data_in_buf[2] = CCB_INVALID_CCB;
        data_in_buf[3] = SCSI_STATUS_OK;
        dev->DataReplyLeft = reply_len;
        return;
    }
    if (!scsi_device_present(sd) || (cmd.LogicalUnit > 0)) {
        data_in_buf[2] = CCB_SELECTION_TIMEOUT;
        data_in_buf[3] = SCSI_STATUS_OK;
        dev->DataReplyLeft = reply_len;
        return;
    }

    scsi_device_identify(sd, cmd.LogicalUnit);
    memset(temp_cdb, 0, sizeof(temp_cdb));
    memcpy(temp_cdb, cmd.CDB, MIN_VAL(cmd.CDBLength, (uint8_t) sizeof(temp_cdb)));
    sd->buffer_length = (int32_t) cmd.DataLength;
    scsi_device_command_phase0(sd, temp_cdb);
    phase = sd->phase;
    if (phase != SCSI_PHASE_STATUS) {
        buslogic_bios_dma_transfer(dev, &cmd, cmd.TargetId, (phase == SCSI_PHASE_DATA_OUT));
        scsi_device_command_phase1(sd);
    }
    scsi_device_identify(sd, SCSI_LUN_USE_CDB);
    data_in_buf[2] = CCB_COMPLETE;
    data_in_buf[3] = sd->status;
    dev->DataReplyLeft = reply_len;
}

static int buslogic_vendor_cmd(BUSLOGIC_t *dev)
{
    uint16_t targets_present_mask = 0;
    uint32_t offset;
    uint32_t count;
    int i;

    switch (dev->Command) {
    case 0x20:
        buslogic_reset(dev, 1);
        dev->DataReplyLeft = 0;
        break;
    case 0x23:
        memset(dev->DataBuf, 0, 8);
        dev->DataReplyLeft = 8;
        break;
    case 0x24:
        for (i = 0; i < 15; i++) {
            if ((i != dev->HostID) && scsi_device_present(&scsi_devices[dev->bus][i])) {
                targets_present_mask |= (uint16_t) (1u << i);
            }
        }
        dev->DataBuf[0] = (uint8_t) targets_present_mask;
        dev->DataBuf[1] = (uint8_t) (targets_present_mask >> 8);
        dev->DataReplyLeft = 2;
        break;
    case 0x25:
        dev->IrqEnabled = dev->CmdBuf[0] ? 1 : 0;
        dev->DataReplyLeft = 0;
        return 1;
    case 0x81: {
        MailboxInitExtended_t *mb = (MailboxInitExtended_t *) dev->CmdBuf;

        dev->flags &= (uint8_t) ~BUSLOGIC_FLAG_MBX_24BIT;
        dev->MailboxInit = 1;
        dev->MailboxCount = mb->Count;
        dev->MailboxOutAddr = host_read_le32_unaligned(mb->Address);
        dev->MailboxInAddr = dev->MailboxOutAddr + (dev->MailboxCount * sizeof(Mailbox32Raw_t));
        dev->Status &= (uint8_t) ~STAT_INIT;
        dev->DataReplyLeft = 0;
        break;
    }
    case 0x83:
        if (dev->CmdParam == 12) {
            dev->CmdParamLeft = dev->CmdBuf[11];
            return 0;
        }
        buslogic_bios_request_setup(dev, dev->CmdBuf, dev->DataBuf, 4);
        break;
    case 0x84:
        dev->DataBuf[0] = dev->fw_rev[4];
        dev->DataReplyLeft = 1;
        break;
    case 0x85:
        dev->DataBuf[0] = dev->fw_rev[5] ? dev->fw_rev[5] : ' ';
        dev->DataReplyLeft = 1;
        break;
    case 0x8B:
        dev->DataReplyLeft = dev->CmdBuf[0];
        memset(dev->DataBuf, 0, dev->DataReplyLeft);
        memcpy(dev->DataBuf, "545S", MIN_VAL(dev->DataReplyLeft, 4));
        break;
    case 0x8C:
        dev->DataReplyLeft = dev->CmdBuf[0];
        memset(dev->DataBuf, 0, dev->DataReplyLeft);
        break;
    case 0x8D: {
        ReplyInquireExtendedSetupInformation *reply = (ReplyInquireExtendedSetupInformation *) dev->DataBuf;

        dev->DataReplyLeft = dev->CmdBuf[0];
        memset(dev->DataBuf, 0, dev->DataReplyLeft);
        reply->uBusType = 'A';
        reply->uBiosAddress = (uint8_t) (dev->rom_addr >> 12);
        host_write_le16_unaligned(reply->u16ScatterGatherLimit, 8192);
        reply->cMailbox = (uint8_t) dev->MailboxCount;
        host_write_le32_unaligned(reply->uMailboxAddressBase, dev->MailboxOutAddr);
        memcpy(reply->aFirmwareRevision, &dev->fw_rev[2], 3);
        break;
    }
    case 0x8F:
        dev->aggressive_round_robin = dev->CmdBuf[0] & 1;
        dev->DataReplyLeft = 0;
        break;
    case 0x90:
        offset = dev->CmdBuf[0];
        count = MIN_VAL((uint32_t) dev->CmdBuf[1], (uint32_t) (sizeof(dev->LocalRAM.u8View) - MIN_VAL(offset, (uint32_t) sizeof(dev->LocalRAM.u8View))));
        if (count != 0) {
            memcpy(&dev->LocalRAM.u8View[offset], &dev->CmdBuf[2], count);
            buslogic_save_nvr(dev);
        }
        dev->DataReplyLeft = 0;
        break;
    case 0x91:
        offset = dev->CmdBuf[0];
        count = MIN_VAL((uint32_t) dev->CmdBuf[1], (uint32_t) (sizeof(dev->LocalRAM.u8View) - MIN_VAL(offset, (uint32_t) sizeof(dev->LocalRAM.u8View))));
        dev->DataReplyLeft = (uint16_t) count;
        if (count != 0) {
            memcpy(dev->DataBuf, &dev->LocalRAM.u8View[offset], count);
        }
        break;
    case 0x96:
        dev->ExtendedLUNCCBFormat = dev->CmdBuf[0] ? 1 : 0;
        dev->DataReplyLeft = 0;
        break;
    case 0xFB:
        dev->DataReplyLeft = dev->CmdBuf[2];
        break;
    default:
        dev->Status |= STAT_INVCMD;
        dev->DataReplyLeft = 0;
        break;
    }

    return 0;
}

static void buslogic_handle_command(BUSLOGIC_t *dev)
{
    ReplyInquireSetupInformation *reply_setup;
    uint32_t fifo_buf;
    uint8_t id;
    int suppress = 0;

    switch (dev->Command) {
    case CMD_NOP:
        dev->DataReplyLeft = 0;
        break;
    case CMD_MBINIT:
        dev->flags |= BUSLOGIC_FLAG_MBX_24BIT;
        dev->MailboxInit = 1;
        dev->MailboxCount = dev->CmdBuf[0];
        dev->MailboxOutAddr = buslogic_read_addr24(&dev->CmdBuf[1]);
        dev->MailboxInAddr = dev->MailboxOutAddr + (dev->MailboxCount * sizeof(Mailbox_t));
        dev->Status &= (uint8_t) ~STAT_INIT;
        dev->DataReplyLeft = 0;
        break;
    case CMD_BIOSCMD:
        {
            BIOSCMDRaw_t raw_bios_cmd;
            BIOSCMD_t parsed_bios_cmd;

            memset(&raw_bios_cmd, 0, sizeof(raw_bios_cmd));
            memcpy(&raw_bios_cmd, dev->CmdBuf, sizeof(raw_bios_cmd));
            buslogic_parse_bioscmd(&parsed_bios_cmd, &raw_bios_cmd);
            parsed_bios_cmd.address[2] &= 0x0f;
            parsed_bios_cmd.address[3] &= 0x1f;
            dev->DataBuf[0] = buslogic_bios_command(dev, &parsed_bios_cmd, 0);
            dev->DataReplyLeft = 1;
        }
        break;
    case CMD_INQUIRY:
        memcpy(dev->DataBuf, dev->fw_rev, 4);
        dev->DataReplyLeft = 4;
        break;
    case CMD_EMBOI:
        if (dev->CmdBuf[0] <= 1) {
            dev->MailboxOutInterrupts = dev->CmdBuf[0];
            suppress = 1;
        } else {
            dev->Status |= STAT_INVCMD;
        }
        dev->DataReplyLeft = 0;
        break;
    case CMD_SELTIMEOUT:
        dev->DataReplyLeft = 0;
        break;
    case CMD_BUSON_TIME:
        dev->BusOnTime = dev->CmdBuf[0];
        dev->DataReplyLeft = 0;
        break;
    case CMD_BUSOFF_TIME:
        dev->BusOffTime = dev->CmdBuf[0];
        dev->DataReplyLeft = 0;
        break;
    case CMD_DMASPEED:
        dev->ATBusSpeed = dev->CmdBuf[0];
        dev->DataReplyLeft = 0;
        break;
    case CMD_RETDEVS:
        memset(dev->DataBuf, 0, 8);
        for (id = 0; id < 8; id++) {
            if ((id != dev->HostID) && scsi_device_present(&scsi_devices[dev->bus][id])) {
                dev->DataBuf[id] = 1;
            }
        }
        dev->DataReplyLeft = 8;
        break;
    case CMD_RETCONF:
        dev->DataBuf[0] = (uint8_t) (1u << dev->DmaChannel);
        dev->DataBuf[1] = (dev->Irq >= 9) ? (uint8_t) (1u << (dev->Irq - 9)) : 0;
        dev->DataBuf[2] = dev->HostID;
        dev->DataReplyLeft = 3;
        break;
    case CMD_RETSETUP:
        reply_setup = (ReplyInquireSetupInformation *) dev->DataBuf;
        memset(dev->DataBuf, 0, dev->CmdBuf[0]);
        reply_setup->uBusTransferRate = dev->ATBusSpeed;
        reply_setup->uPreemptTimeOnBus = dev->BusOnTime;
        reply_setup->uTimeOffBus = dev->BusOffTime;
        reply_setup->cMailbox = (uint8_t) dev->MailboxCount;
        reply_setup->MailboxAddress = u32_to_addr24(dev->MailboxOutAddr);
        buslogic_setup_data(dev);
        dev->DataReplyLeft = dev->CmdBuf[0];
        break;
    case CMD_ECHO:
        dev->DataBuf[0] = dev->CmdBuf[0];
        dev->DataReplyLeft = 1;
        break;
    case CMD_WRITE_CH2:
        fifo_buf = buslogic_read_addr24(dev->CmdBuf);
        dma_bm_read(dev->dma, fifo_buf, dev->dma_buffer, sizeof(dev->dma_buffer), (int) dev->transfer_size);
        dev->DataReplyLeft = 0;
        break;
    case CMD_READ_CH2:
        fifo_buf = buslogic_read_addr24(dev->CmdBuf);
        dma_bm_write(dev->dma, fifo_buf, dev->dma_buffer, sizeof(dev->dma_buffer), (int) dev->transfer_size);
        dev->DataReplyLeft = 0;
        break;
    case CMD_OPTIONS:
        if (dev->CmdParam == 1) {
            dev->CmdParamLeft = dev->CmdBuf[0];
        }
        dev->DataReplyLeft = 0;
        break;
    default:
        suppress = buslogic_vendor_cmd(dev);
        break;
    }

    if (dev->DataReplyLeft != 0) {
        dev->Status |= STAT_DFULL;
    } else if (dev->CmdParamLeft == 0) {
        buslogic_cmd_done(dev, suppress);
    }
}

static uint8_t buslogic_port_read(BUSLOGIC_t *dev, uint16_t port)
{
    uint8_t ret = 0xff;

    switch (port & 3) {
    case 0:
        ret = dev->Status;
        break;
    case 1:
        if (dev->DataReplyLeft != 0) {
            ret = dev->DataBuf[dev->DataReply];
            dev->DataReply++;
            dev->DataReplyLeft--;
            if (dev->DataReplyLeft == 0) {
                buslogic_cmd_done(dev, 0);
            }
        } else {
            ret = 0;
        }
        break;
    case 2:
        ret = dev->Interrupt;
        break;
    case 3:
        ret = dev->Geometry;
        break;
    default:
        break;
    }

    return ret;
}

static void buslogic_port_write(BUSLOGIC_t *dev, uint16_t port, uint8_t value)
{
    switch (port & 3) {
    case 0:
        if ((value & CTRL_HRST) || (value & CTRL_SRST)) {
            buslogic_reset(dev, 1);
            return;
        }
        if ((value & CTRL_SCRST) != 0) {
            uint8_t i;

            for (i = 0; i < SCSI_ID_MAX; i++) {
                scsi_device_reset(&scsi_devices[dev->bus][i]);
            }
        }
        if ((value & CTRL_IRST) != 0) {
            buslogic_clear_irq(dev);
        }
        return;
    case 1:
        if ((value == CMD_START_SCSI) && (dev->Command == 0xff)) {
            dev->MailboxReq++;
            buslogic_schedule_mail(dev);
            return;
        }

        if (dev->Command == 0xff) {
            dev->Command = value;
            dev->CmdParam = 0;
            dev->CmdParamLeft = 0;
            dev->Status &= (uint8_t) ~(STAT_INVCMD | STAT_IDLE);
            switch (dev->Command) {
            case CMD_MBINIT:
                dev->CmdParamLeft = sizeof(MailboxInit_t);
                break;
            case CMD_BIOSCMD:
                dev->CmdParamLeft = 10;
                break;
            case CMD_EMBOI:
            case CMD_BUSON_TIME:
            case CMD_BUSOFF_TIME:
            case CMD_DMASPEED:
            case CMD_RETSETUP:
            case CMD_ECHO:
            case CMD_OPTIONS:
                dev->CmdParamLeft = 1;
                break;
            case CMD_SELTIMEOUT:
                dev->CmdParamLeft = 4;
                break;
            case CMD_WRITE_CH2:
            case CMD_READ_CH2:
                dev->CmdParamLeft = 3;
                break;
            default:
                dev->CmdParamLeft = buslogic_get_param_len(dev->Command);
                break;
            }
        } else {
            dev->CmdBuf[dev->CmdParam] = value;
            dev->CmdParam++;
            if (dev->CmdParamLeft > 0) {
                dev->CmdParamLeft--;
            }
            buslogic_cmd_phase1(dev);
        }

        if (dev->CmdParamLeft == 0) {
            buslogic_handle_command(dev);
        }
        return;
    case 2:
        if (dev->flags & BUSLOGIC_FLAG_INT_GEOM_WRITABLE) {
            dev->Interrupt = value;
        }
        return;
    case 3:
        if (dev->flags & BUSLOGIC_FLAG_INT_GEOM_WRITABLE) {
            dev->Geometry = value;
        }
        return;
    default:
        return;
    }
}

static uint8_t buslogic_port_read_cb(void *opaque, uint16_t port)
{
    return buslogic_port_read((BUSLOGIC_t *) opaque, port);
}

static void buslogic_port_write_cb(void *opaque, uint16_t port, uint8_t value)
{
    buslogic_port_write((BUSLOGIC_t *) opaque, port, value);
}

int buslogic_init(BUSLOGIC_t **out, const BUSLOGIC_CONFIG_t *config, const BUSLOGIC_TARGET_t targets[BUSLOGIC_MAX_TARGETS])
{
    BUSLOGIC_t *dev;
    FILE *fp;
    uint8_t i;

    if ((out == NULL) || (config == NULL) || (config->cpu == NULL) || (config->dma == NULL) || (config->pic_master == NULL)) {
        return -1;
    }
    if ((config->irq >= 8) && (config->pic_slave == NULL)) {
        return -1;
    }

    dev = (BUSLOGIC_t *) calloc(1, sizeof(BUSLOGIC_t));
    if (dev == NULL) {
        return -1;
    }

    scsi_reset();
    scsi_device_init();

    dev->cpu = config->cpu;
    dev->dma = config->dma;
    dev->pic_master = config->pic_master;
    dev->pic_slave = config->pic_slave;
    dev->Base = config->base;
    dev->Irq = config->irq;
    dev->DmaChannel = config->dma_channel;
    dev->HostID = 7;
    dev->transfer_size = 2;
    dev->aggressive_round_robin = 1;
    dev->flags = BUSLOGIC_FLAG_INT_GEOM_WRITABLE | BUSLOGIC_FLAG_MBX_24BIT | BUSLOGIC_FLAG_CDROM_BOOT;
    memcpy(dev->fw_rev, "AA331", 6);
    dev->fw_rev[6] = 0;
    dev->BusOnTime = 7;
    dev->BusOffTime = 4;
    dev->ATBusSpeed = 1;
    dev->bus = scsi_get_bus();
    dev->rom_addr = config->bios_addr;

    if (config->rom_path != NULL) {
        strncpy(dev->rom_path, config->rom_path, sizeof(dev->rom_path) - 1);
    }
    if (config->nvr_path != NULL) {
        strncpy(dev->nvr_path, config->nvr_path, sizeof(dev->nvr_path) - 1);
    }

    if (dev->bus == 0xff) {
        free(dev);
        return -1;
    }

    scsi_bus_set_speed(dev->bus, 5000000.0);
    buslogic_load_nvr(dev);

    if (dev->rom_addr != 0) {
        fp = fopen(dev->rom_path, "rb");
        if (fp == NULL) {
            debug_log(DEBUG_ERROR, "[SCSI] BusLogic BIOS ROM not found: %s\r\n", dev->rom_path);
            free(dev);
            return -1;
        }
        if (fread(dev->rom_data, 1, ROM_SIZE, fp) != ROM_SIZE) {
            fclose(fp);
            debug_log(DEBUG_ERROR, "[SCSI] BusLogic BIOS ROM is not %u bytes: %s\r\n", ROM_SIZE, dev->rom_path);
            free(dev);
            return -1;
        }
        fclose(fp);
        dev->rom_loaded = 1;
        memory_mapCallbackRegister(dev->rom_addr, ROM_SIZE,
                                   buslogic_rom_read, NULL, NULL,
                                   buslogic_rom_write, NULL, NULL,
                                   dev);
    }

    for (i = 0; i < BUSLOGIC_MAX_TARGETS; i++) {
        if (!targets[i].present) {
            continue;
        }
        if (targets[i].type == BUSLOGIC_TARGET_DISK) {
            if (scsi_disk_attach(dev->bus, i, targets[i].path) != 0) {
                free(dev);
                return -1;
            }
        } else if (targets[i].type == BUSLOGIC_TARGET_CDROM) {
            if (scsi_cdrom_attach(dev->bus, i, targets[i].path) != 0) {
                free(dev);
                return -1;
            }
        }
    }

    dev->mail_timer_id = timing_addTimer(buslogic_process_mail_cb, dev, 1000, TIMING_DISABLED);
    dev->reset_timer_id = timing_addTimer(buslogic_reset_timer_cb, dev, 1000, TIMING_DISABLED);
    buslogic_register_io(dev);
    buslogic_reset(dev, 1);
    *out = dev;

    debug_log(DEBUG_DETAIL, "[SCSI] BusLogic BT-545S initialized at 0x%03X IRQ %u DMA %u BIOS %s\r\n",
        dev->Base, dev->Irq, dev->DmaChannel, dev->rom_addr ? "enabled" : "disabled");
    return 0;
}
