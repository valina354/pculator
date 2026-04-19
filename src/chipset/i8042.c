#include <string.h>
#include "../debuglog.h"
#include "../ports.h"
#include "../timing.h"
#include "../machine.h"
#include "i8042.h"

enum {
    I8042_MISC_AMI_BLOCK_P23 = 0x04
};

static const uint8_t i8042_nont_to_t[256] = {
    0xff, 0x43, 0x41, 0x3f, 0x3d, 0x3b, 0x3c, 0x58,
    0x64, 0x44, 0x42, 0x40, 0x3e, 0x0f, 0x29, 0x59,
    0x65, 0x38, 0x2a, 0x70, 0x1d, 0x10, 0x02, 0x5a,
    0x66, 0x71, 0x2c, 0x1f, 0x1e, 0x11, 0x03, 0x5b,
    0x67, 0x2e, 0x2d, 0x20, 0x12, 0x05, 0x04, 0x5c,
    0x68, 0x39, 0x2f, 0x21, 0x14, 0x13, 0x06, 0x5d,
    0x69, 0x31, 0x30, 0x23, 0x22, 0x15, 0x07, 0x5e,
    0x6a, 0x72, 0x32, 0x24, 0x16, 0x08, 0x09, 0x5f,
    0x6b, 0x33, 0x25, 0x17, 0x18, 0x0b, 0x0a, 0x60,
    0x6c, 0x34, 0x35, 0x26, 0x27, 0x19, 0x0c, 0x61,
    0x6d, 0x73, 0x28, 0x74, 0x1a, 0x0d, 0x62, 0x6e,
    0x3a, 0x36, 0x1c, 0x1b, 0x75, 0x2b, 0x63, 0x76,
    0x55, 0x56, 0x77, 0x78, 0x79, 0x7a, 0x0e, 0x7b,
    0x7c, 0x4f, 0x7d, 0x4b, 0x47, 0x7e, 0x7f, 0x6f,
    0x52, 0x53, 0x50, 0x4c, 0x4d, 0x48, 0x01, 0x45,
    0x57, 0x4e, 0x51, 0x4a, 0x37, 0x49, 0x46, 0x54,
    0x80, 0x81, 0x82, 0x41, 0x54, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
};

i8042_t kbc;

static uint8_t i8042_in_range(uint8_t val, uint8_t low, uint8_t high)
{
    return (uint8_t)((val >= low) && (val <= high));
}

static uint64_t i8042_usec_to_interval(uint64_t usec)
{
    return (timing_getFreq() * usec) / 1000000ULL;
}

static uint8_t i8042_is_ps2_default(const i8042_t* dev)
{
    return (uint8_t)((dev != NULL) && (dev->profile.flags & KBC_PROFILE_FLAG_PS2_DEFAULT));
}

static uint8_t i8042_get_ob_status_hi(const i8042_t* dev, uint8_t stat_hi)
{
    if ((dev->profile.vendor == KBC_VENDOR_AMI) || (dev->misc_flags & KBC_FLAG_PS2_MODE)) {
        if (dev->p1 & 0x80) {
            stat_hi |= KBC_STAT_UNLOCKED;
        }
    } else {
        stat_hi |= KBC_STAT_UNLOCKED;
    }

    return stat_hi;
}

static uint8_t i8042_read_p1(i8042_t* dev)
{
    uint8_t ret;

    ret = 0xB0;
    ret |= (uint8_t)(dev->p1 & 0x03);
    dev->p1 = (uint8_t)(((dev->p1 + 1) & 0x03) | (dev->p1 & 0xFC));

    return ret;
}

static int i8042_translate(i8042_t* dev, uint8_t val, uint8_t* out);
static void i8042_ibf_process(i8042_t* dev);

static void i8042_ctrl_queue_reset(i8042_t* dev)
{
    dev->key_ctrl_queue_start = 0;
    dev->key_ctrl_queue_end = 0;
    memset(dev->key_ctrl_queue, 0, sizeof(dev->key_ctrl_queue));
}

static void i8042_ctrl_queue_add(i8042_t* dev, uint8_t val)
{
    dev->key_ctrl_queue[dev->key_ctrl_queue_end] = val;
    dev->key_ctrl_queue_end = (uint8_t)((dev->key_ctrl_queue_end + 1) & 0x3F);
    dev->state = KBC_STATE_CTRL_OUT;
}

static void i8042_set_enable_kbd(i8042_t* dev, uint8_t enable)
{
    dev->mem[0x20] &= (uint8_t)~KBC_CCB_ENABLEKBD;
    dev->mem[0x20] |= enable ? 0x00 : KBC_CCB_ENABLEKBD;
}

static void i8042_set_enable_aux(i8042_t* dev, uint8_t enable)
{
    dev->mem[0x20] &= (uint8_t)~KBC_CCB_PCMODE;
    dev->mem[0x20] |= enable ? 0x00 : KBC_CCB_PCMODE;
}

static void i8042_raise_irq(i8042_t* dev, uint8_t channel)
{
    if (dev == NULL) {
        return;
    }

    if ((channel < 2) && (dev->pic != NULL) && (dev->mem[0x20] & KBC_CCB_ENABLEKINT)) {
        if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
            i8259_doirq(dev->pic, 1);
        } else {
            i8259_setlevelirq(dev->pic, 1, 1);
        }
    } else if ((channel == 2) && (dev->pic_aux != NULL) && (dev->mem[0x20] & KBC_CCB_ENABLEMINT)) {
        i8259_doirq(dev->pic_aux, 4);
    }
}

static void i8042_send_to_ob(i8042_t* dev, uint8_t val, uint8_t channel, uint8_t stat_hi)
{
    uint8_t out = val;

    if ((channel == 1) && !i8042_translate(dev, val, &out)) {
        return;
    }

    stat_hi = i8042_get_ob_status_hi(dev, stat_hi);
    dev->ob = out;
    dev->status = (uint8_t)((dev->status & 0x0F) | KBC_STAT_OFULL | stat_hi);
    if (channel == 2) {
        dev->status |= KBC_STAT_MFULL;
    }
    i8042_raise_irq(dev, channel);
}

static void i8042_delay_to_ob(i8042_t* dev, uint8_t val, uint8_t channel, uint8_t stat_hi)
{
    dev->val = val;
    dev->channel = channel;
    dev->stat_hi = stat_hi;
    dev->pending = 1;
    dev->state = KBC_STATE_DELAY_OUT;
}

static int i8042_translate(i8042_t* dev, uint8_t val, uint8_t* out)
{
    uint8_t translate = (uint8_t)((dev->mem[0x20] & KBC_CCB_TRANSLATE) != 0);

    if (!(dev->misc_flags & KBC_FLAG_PS2_MODE) && (dev->mem[0x20] & KBC_CCB_PCMODE)) {
        translate = 0;
    }

    if (translate && (val == 0xF0)) {
        dev->sc_or = 0x80;
        return 0;
    }

    if (translate && (dev->sc_or == 0x80) && (i8042_nont_to_t[val] & 0x80)) {
        dev->sc_or = 0;
        return 0;
    }

    if (translate) {
        *out = (uint8_t)(i8042_nont_to_t[val] | dev->sc_or);
        dev->sc_or = 0;
        return 1;
    }

    *out = val;
    return 1;
}

static void i8042_write_p2(i8042_t* dev, uint8_t val)
{
    uint8_t old = dev->p2;

    dev->p2 = val;
    if (dev->cpu != NULL) {
        dev->cpu->a20_gate = (uint8_t)((val & 0x02) ? 1 : 0);
    }

    if (((old ^ val) & 0x01) && ((val & 0x01) == 0)) {
        machine_platform_reset();
    }
}

static void i8042_write_p2_fast_a20(i8042_t* dev, uint8_t val)
{
    dev->p2 = val;
    if (dev->cpu != NULL) {
        dev->cpu->a20_gate = (uint8_t)((val & 0x02) ? 1 : 0);
    }
}

static void i8042_pulse_restore(void* priv)
{
    i8042_t* dev = (i8042_t*)priv;

    if ((dev == NULL) || !dev->pulse_pending) {
        return;
    }

    dev->pulse_pending = 0;
    i8042_write_p2(dev, dev->old_p2);
    timing_timerDisable(dev->pulse_timer);
}

static void i8042_pulse_output(i8042_t* dev, uint8_t mask)
{
    if (mask == 0x0F) {
        return;
    }

    dev->old_p2 = dev->p2;
    dev->pulse_pending = 1;
    i8042_write_p2(dev, (uint8_t)(dev->p2 & (0xF0 | mask)));
    timing_updateInterval(dev->pulse_timer, i8042_usec_to_interval(6));
    timing_timerEnable(dev->pulse_timer);
}

static void i8042_write_cmdbyte(i8042_t* dev, uint8_t val)
{
    dev->mem[0x20] = val;
    if (!(dev->misc_flags & KBC_FLAG_PS2_MODE)) {
        i8042_write_p2(dev, (uint8_t)((dev->p2 & 0x0F) | ((val & 0x03) << 4) |
            ((val & KBC_CCB_PCMODE) ? 0xC0 : 0x00)));
    }
    dev->status = (uint8_t)((dev->status & (uint8_t)~KBC_STAT_SYSFLAG) | (val & KBC_STAT_SYSFLAG));
}

static void i8042_scan_keyboard_at(i8042_t* dev)
{
    if ((dev->mem[0x20] & KBC_CCB_ENABLEKBD) || (dev->status & KBC_STAT_OFULL)) {
        return;
    }

    if (dev->mem[0x20] & KBC_CCB_PCMODE) {
        if (dev->ports[0].out_new != -1) {
            i8042_send_to_ob(dev, (uint8_t)dev->ports[0].out_new, 1, 0x00);
            dev->ports[0].out_new = -1;
            dev->state = KBC_STATE_MAIN_IBF;
        } else if (dev->status & KBC_STAT_IFULL) {
            i8042_ibf_process(dev);
        }
        return;
    }

    if (dev->mem[0x2E] != 0x00) {
        dev->mem[0x2E] = 0x00;
    }
    dev->p2 &= 0xBF;

    if (dev->ports[0].out_new == -1) {
        return;
    }

    if (dev->mem[0x20] & KBC_CCB_TRANSLATE) {
        if ((dev->mem[0x20] & KBC_CCB_IGNORELOCK) || (dev->p1 & 0x80)) {
            i8042_send_to_ob(dev, (uint8_t)dev->ports[0].out_new, 1, 0x00);
        }
        dev->mem[0x2D] = (uint8_t)((dev->ports[0].out_new == 0xF0) ? 0x80 : 0x00);
    } else {
        i8042_send_to_ob(dev, (uint8_t)dev->ports[0].out_new, 1, 0x00);
    }
    dev->ports[0].out_new = -1;
    dev->state = KBC_STATE_MAIN_IBF;
}

static int i8042_scan_keyboard_ps2(i8042_t* dev)
{
    if ((dev->ports[0].out_new == -1) || (dev->status & KBC_STAT_OFULL)) {
        return 0;
    }

    i8042_send_to_ob(dev, (uint8_t)dev->ports[0].out_new, 1, 0x00);
    dev->ports[0].out_new = -1;
    dev->state = KBC_STATE_MAIN_IBF;
    return 1;
}

static int i8042_scan_aux_ps2(i8042_t* dev)
{
    if ((dev->ports[1].out_new == -1) || (dev->status & KBC_STAT_OFULL)) {
        return 0;
    }

    i8042_send_to_ob(dev, (uint8_t)dev->ports[1].out_new, 2, 0x00);
    dev->ports[1].out_new = -1;
    dev->state = KBC_STATE_MAIN_IBF;
    return 1;
}

static void i8042_handle_data_to_kbd(i8042_t* dev)
{
    i8042_set_enable_kbd(dev, 1);
    dev->ports[0].wantcmd = 1;
    dev->ports[0].dat = dev->ib;
    dev->state = KBC_STATE_SEND_KBD;
}

static uint8_t i8042_handle_ami_data(i8042_t* dev, uint8_t val)
{
    if (i8042_in_range(dev->command, 0x40, 0x5F)) {
        dev->mem[(dev->command & 0x1F) + 0x20] = val;
        if ((dev->command & 0x1F) == 0x00) {
            i8042_write_cmdbyte(dev, val);
        }
        return 0;
    }

    switch (dev->command) {
    case 0xAF:
        if (dev->command_phase == 1) {
            dev->mem_addr = val;
            dev->wantdata = 1;
            dev->state = KBC_STATE_PARAM;
            dev->command_phase = 2;
        } else if (dev->command_phase == 2) {
            dev->mem[dev->mem_addr] = val;
            dev->command_phase = 0;
        }
        return 0;
    case 0xC1:
        dev->p1 = val;
        return 0;
    case 0xCB:
        dev->ami_flags = val;
        if (dev->profile.flags & KBC_PROFILE_FLAG_PS2_CAPABLE) {
            if (val & 0x01) {
                dev->misc_flags |= KBC_FLAG_PS2_MODE;
            } else {
                dev->misc_flags &= (uint8_t)~KBC_FLAG_PS2_MODE;
            }
        }
        return 0;
    default:
        return 1;
    }
}

static uint8_t i8042_handle_ami_cmd(i8042_t* dev, uint8_t val)
{
    if (i8042_in_range(val, 0x00, 0x1F)) {
        i8042_delay_to_ob(dev, dev->mem[val + 0x20], 0, 0x00);
        return 0;
    }

    if (i8042_in_range(val, 0x40, 0x5F)) {
        dev->wantdata = 1;
        dev->state = KBC_STATE_PARAM;
        return 0;
    }

    if (i8042_in_range(val, 0xB0, 0xB3)) {
        if (!(dev->profile.flags & KBC_PROFILE_FLAG_PCI) || (val > 0xB1)) {
            dev->p1 &= (uint8_t)~(1 << (val & 0x03));
        }
        i8042_delay_to_ob(dev, dev->ob, 0, 0x00);
        dev->pending++;
        return 0;
    }

    if (i8042_in_range(val, 0xB8, 0xBB)) {
        if (!(dev->profile.flags & KBC_PROFILE_FLAG_PCI) || (val > 0xB9)) {
            dev->p1 |= (uint8_t)(1 << (val & 0x03));
            i8042_delay_to_ob(dev, dev->ob, 0, 0x00);
            dev->pending++;
        }
        return 0;
    }

    switch (val) {
    case 0xA1:
        i8042_delay_to_ob(dev, dev->profile.revision, 0, 0x00);
        return 0;
    case 0xA2:
        if (!(dev->misc_flags & KBC_FLAG_PS2_MODE)) {
            i8042_write_p2(dev, (uint8_t)(dev->p2 & 0xF3));
            i8042_delay_to_ob(dev, 0x00, 0, 0x00);
            return 0;
        }
        return 1;
    case 0xA3:
        if (!(dev->misc_flags & KBC_FLAG_PS2_MODE)) {
            i8042_write_p2(dev, (uint8_t)(dev->p2 | 0x0C));
            i8042_delay_to_ob(dev, 0x00, 0, 0x00);
            return 0;
        }
        return 1;
    case 0xA4:
        if (!(dev->misc_flags & KBC_FLAG_PS2_MODE)) {
            dev->misc_flags &= (uint8_t)~KBC_FLAG_CLOCK;
            return 0;
        }
        return 1;
    case 0xA5:
        if (!(dev->misc_flags & KBC_FLAG_PS2_MODE)) {
            dev->misc_flags |= KBC_FLAG_CLOCK;
            return 0;
        }
        return 1;
    case 0xA6:
        if (!(dev->misc_flags & KBC_FLAG_PS2_MODE)) {
            i8042_delay_to_ob(dev, (uint8_t)((dev->misc_flags & KBC_FLAG_CLOCK) ? 0xFF : 0x00), 0, 0x00);
            return 0;
        }
        return 1;
    case 0xA7:
        if (!(dev->misc_flags & KBC_FLAG_PS2_MODE)) {
            dev->misc_flags &= (uint8_t)~KBC_FLAG_CACHE;
            return 0;
        }
        return 1;
    case 0xA8:
        if (!(dev->misc_flags & KBC_FLAG_PS2_MODE)) {
            dev->misc_flags |= KBC_FLAG_CACHE;
            return 0;
        }
        return 1;
    case 0xA9:
        if (!(dev->misc_flags & KBC_FLAG_PS2_MODE)) {
            i8042_delay_to_ob(dev, (uint8_t)((dev->misc_flags & KBC_FLAG_CACHE) ? 0xFF : 0x00), 0, 0x00);
            return 0;
        }
        return 1;
    case 0xAF:
        if (dev->ami_is_amikey_2) {
            dev->wantdata = 1;
            dev->state = KBC_STATE_PARAM;
            dev->command_phase = 1;
            return 0;
        }
        return 1;
    case 0xB4:
    case 0xB5:
        if (!(dev->profile.flags & KBC_PROFILE_FLAG_PCI)) {
            i8042_write_p2(dev, (uint8_t)(dev->p2 & (uint8_t)~(4 << (val & 0x01))));
        }
        i8042_delay_to_ob(dev, dev->ob, 0, 0x00);
        dev->pending++;
        return 0;
    case 0xBC:
    case 0xBD:
        if (!(dev->profile.flags & KBC_PROFILE_FLAG_PCI)) {
            i8042_write_p2(dev, (uint8_t)(dev->p2 | (4 << (val & 0x01))));
        }
        i8042_delay_to_ob(dev, dev->ob, 0, 0x00);
        dev->pending++;
        return 0;
    case 0xC1:
        dev->wantdata = 1;
        dev->state = KBC_STATE_PARAM;
        return 0;
    case 0xC8:
        dev->ami_flags &= (uint8_t)~I8042_MISC_AMI_BLOCK_P23;
        return 0;
    case 0xC9:
        dev->ami_flags |= I8042_MISC_AMI_BLOCK_P23;
        return 0;
    case 0xCA:
        i8042_delay_to_ob(dev, dev->ami_flags, 0, 0x00);
        return 0;
    case 0xCB:
        dev->wantdata = 1;
        dev->state = KBC_STATE_PARAM;
        return 0;
    case 0xEF:
        i8042_delay_to_ob(dev, (uint8_t)((dev->profile.revision >= 'H') ? 0x00 : 0xFF), 0, 0x00);
        return 0;
    default:
        return 1;
    }
}

static void i8042_process_cmd(i8042_t* dev)
{
    int bad = 1;

    if (dev->status & KBC_STAT_CD) {
        dev->wantdata = 0;
        dev->state = KBC_STATE_MAIN_IBF;
        i8042_ctrl_queue_reset(dev);

        if (dev->profile.vendor == KBC_VENDOR_AMI) {
            bad = i8042_handle_ami_cmd(dev, dev->ib);
        }

        if (bad) {
            if (i8042_in_range(dev->ib, 0x20, 0x3F)) {
                i8042_delay_to_ob(dev, dev->mem[dev->ib], 0, 0x00);
                if (dev->ib == 0x20) {
                    dev->pending++;
                }
            } else if (i8042_in_range(dev->ib, 0x60, 0x7F)) {
                dev->wantdata = 1;
                dev->state = KBC_STATE_PARAM;
            } else if (i8042_in_range(dev->ib, 0xF0, 0xFF)) {
                i8042_pulse_output(dev, (uint8_t)(dev->ib & 0x0F));
            } else {
            switch (dev->ib) {
            default:
                debug_log(DEBUG_DETAIL, "[I8042] Unsupported KBC command %02X\r\n", dev->ib);
                break;
            case 0xA4:
                if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
                    i8042_delay_to_ob(dev, 0xF1, 0, 0x00);
                }
                break;
            case 0xA5:
                dev->wantdata = 1;
                dev->state = KBC_STATE_PARAM;
                break;
            case 0xA7:
                if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
                    i8042_set_enable_aux(dev, 0);
                }
                break;
            case 0xA8:
                if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
                    i8042_set_enable_aux(dev, 1);
                }
                break;
            case 0xA9:
                if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
                    i8042_delay_to_ob(dev, 0x00, 0, 0x00);
                }
                break;
            case 0xAA:
                if (i8042_is_ps2_default(dev)) {
                    if (dev->state != KBC_STATE_RESET) {
                        dev->p1 |= 0xFF;
                        i8042_write_p2(dev, 0x4B);
                        if (dev->pic_aux != NULL) {
                            i8259_clearirq(dev->pic_aux, 4);
                        }
                        if (dev->pic != NULL) {
                            i8259_clearirq(dev->pic, 1);
                        }
                    }

                    dev->status = (uint8_t)((dev->status & 0x0F) | 0x60);
                    dev->mem[0x20] = 0x30;
                    dev->mem[0x22] = 0x0B;
                    dev->mem[0x25] = 0x02;
                    dev->mem[0x27] = 0xF8;
                    dev->mem[0x28] = 0xCE;
                    dev->mem[0x29] = 0x0B;
                    dev->mem[0x30] = 0x0B;
                } else {
                    if (dev->state != KBC_STATE_RESET) {
                        dev->p1 |= 0xFF;
                        i8042_write_p2(dev, 0xCF);
                        if (dev->pic != NULL) {
                            i8259_setlevelirq(dev->pic, 1, 0);
                        }
                    }

                    dev->status = (uint8_t)((dev->status & 0x0F) | 0x60);
                    dev->mem[0x20] = 0x10;
                    dev->mem[0x22] = 0x06;
                    dev->mem[0x25] = 0x01;
                    dev->mem[0x27] = 0xFB;
                    dev->mem[0x28] = 0xE0;
                    dev->mem[0x29] = 0x06;
                }
                dev->mem[0x21] = 0x01;
                dev->mem[0x2A] = 0x10;
                dev->mem[0x2B] = 0x20;
                dev->mem[0x2C] = 0x15;
                dev->ports[0].out_new = -1;
                dev->ports[1].out_new = -1;
                i8042_ctrl_queue_reset(dev);
                i8042_ctrl_queue_add(dev, 0x55);
                break;
            case 0xAB:
                i8042_delay_to_ob(dev, 0x00, 0, 0x00);
                break;
            case 0xAC:
                if (!(dev->misc_flags & KBC_FLAG_PS2_MODE)) {
                    dev->mem[0x30] = (uint8_t)((dev->p1 & 0xF0) | 0x80);
                    dev->mem[0x31] = dev->p2;
                    dev->mem[0x32] = 0x00;
                    dev->mem[0x33] = 0x00;
                    {
                        static const uint8_t hex_conv[16] = { 0x0B, 2, 3, 4, 5, 6, 7, 8, 9, 0x0A, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21 };
                        uint8_t i;
                        for (i = 0; i < 20; i++) {
                            i8042_ctrl_queue_add(dev, hex_conv[dev->mem[i + 0x20] >> 4]);
                            i8042_ctrl_queue_add(dev, hex_conv[dev->mem[i + 0x20] & 0x0F]);
                            i8042_ctrl_queue_add(dev, 0x39);
                        }
                    }
                }
                break;
            case 0xAD:
                i8042_set_enable_kbd(dev, 0);
                break;
            case 0xAE:
                i8042_set_enable_kbd(dev, 1);
                break;
            case 0xC0:
                i8042_delay_to_ob(dev, i8042_read_p1(dev), 0, 0x00);
                break;
            case 0xC1:
                if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
                    dev->status &= 0x0F;
                    dev->status |= (uint8_t)(dev->p1 << 4);
                }
                break;
            case 0xC2:
                if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
                    dev->status &= 0x0F;
                    dev->status |= (uint8_t)(dev->p1 & 0xF0);
                }
                break;
            case 0xD0:
                {
                    uint8_t mask = 0xFF;
                    if (!(dev->misc_flags & KBC_FLAG_PS2_MODE) && (dev->mem[0x20] & KBC_CCB_ENABLEKBD)) {
                        mask &= 0xBF;
                    }
                    i8042_delay_to_ob(dev,
                        (uint8_t)(((dev->p2 & 0xFD) | (dev->cpu != NULL && dev->cpu->a20_gate ? 0x02 : 0x00)) & mask),
                        0,
                        0x00);
                }
                break;
            case 0xD1:
                dev->wantdata = 1;
                dev->state = KBC_STATE_PARAM;
                break;
            case 0xD2:
                dev->wantdata = 1;
                dev->state = KBC_STATE_PARAM;
                break;
            case 0xD3:
                if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
                    dev->wantdata = 1;
                    dev->state = KBC_STATE_PARAM;
                }
                break;
            case 0xD4:
                if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
                    dev->wantdata = 1;
                    dev->state = KBC_STATE_PARAM;
                }
                break;
            case 0xDD:
            case 0xDF:
                i8042_write_p2_fast_a20(dev, (uint8_t)((dev->p2 & 0xFD) | (dev->ib & 0x02)));
                break;
            case 0xE0:
                i8042_delay_to_ob(dev, 0x00, 0, 0x00);
                break;
            }
            }
        }

        if (dev->wantdata) {
            dev->command = dev->ib;
        }
        return;
    }

    if (!dev->wantdata) {
        i8042_handle_data_to_kbd(dev);
        return;
    }

    dev->wantdata = 0;
    dev->state = KBC_STATE_MAIN_IBF;

    if ((dev->profile.vendor == KBC_VENDOR_AMI) && !i8042_handle_ami_data(dev, dev->ib)) {
        return;
    }

    if (i8042_in_range(dev->command, 0x60, 0x7F)) {
        dev->mem[(dev->command & 0x1F) + 0x20] = dev->ib;
        if (dev->command == 0x60) {
            i8042_write_cmdbyte(dev, dev->ib);
        }
        return;
    }

    switch (dev->command) {
    default:
        debug_log(DEBUG_DETAIL, "[I8042] Unsupported KBC command data %02X for %02X\r\n", dev->ib, dev->command);
        break;
    case 0xA5:
        if (dev->ib != 0x00) {
            dev->wantdata = 1;
            dev->state = KBC_STATE_PARAM;
        }
        break;
    case 0xD1:
        if (dev->ami_flags & I8042_MISC_AMI_BLOCK_P23) {
            dev->ib &= (uint8_t)~0x0C;
            dev->ib |= (uint8_t)(dev->p2 & 0x0C);
        }
        i8042_write_p2(dev, (uint8_t)(dev->ib | 0x01));
        break;
    case 0xD2:
        i8042_delay_to_ob(dev, dev->ib, 0, 0x00);
        break;
    case 0xD3:
        i8042_delay_to_ob(dev, dev->ib, 2, 0x00);
        break;
    case 0xD4:
        if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
            i8042_set_enable_aux(dev, 1);
            if ((dev->ports[1].poll != NULL) && (dev->ports[1].priv != NULL)) {
                dev->ports[1].dat = dev->ib;
                dev->ports[1].wantcmd = 1;
                dev->state = KBC_STATE_SEND_AUX;
            } else {
                i8042_delay_to_ob(dev, 0xFE, 2, KBC_STAT_RTIMEOUT);
            }
        }
        break;
    }
}

static void i8042_ibf_process(i8042_t* dev)
{
    dev->status &= (uint8_t)~KBC_STAT_IFULL;
    dev->state = KBC_STATE_MAIN_IBF;
    if (dev->status & KBC_STAT_CD) {
        i8042_process_cmd(dev);
    } else {
        i8042_handle_data_to_kbd(dev);
    }
}

static void i8042_poll_at(i8042_t* dev)
{
    switch (dev->state) {
    case KBC_STATE_RESET:
        if (dev->status & KBC_STAT_IFULL) {
            dev->status = (uint8_t)(((dev->status & 0x0F) | KBC_STAT_UNLOCKED) & (uint8_t)~KBC_STAT_IFULL);
            if ((dev->status & KBC_STAT_CD) && (dev->ib == 0xAA)) {
                i8042_process_cmd(dev);
            }
        }
        break;
    case KBC_STATE_AMI_OUT:
        if (dev->status & KBC_STAT_OFULL) {
            break;
        }
        //fallthrough
    case KBC_STATE_MAIN_IBF:
    default:
at_main_ibf:
        if (dev->status & KBC_STAT_OFULL) {
            if ((dev->status & KBC_STAT_IFULL) && (dev->status & KBC_STAT_CD)) {
                dev->status &= (uint8_t)~KBC_STAT_IFULL;
                i8042_process_cmd(dev);
            }
        } else if (dev->status & KBC_STAT_IFULL) {
            i8042_ibf_process(dev);
        } else if (!(dev->mem[0x20] & KBC_CCB_ENABLEKBD)) {
            dev->state = KBC_STATE_MAIN_KBD;
        }
        break;
    case KBC_STATE_MAIN_KBD:
    case KBC_STATE_MAIN_BOTH:
        if (dev->status & KBC_STAT_IFULL) {
            i8042_ibf_process(dev);
        } else {
            i8042_scan_keyboard_at(dev);
            dev->state = KBC_STATE_MAIN_IBF;
        }
        break;
    case KBC_STATE_DELAY_OUT:
        i8042_send_to_ob(dev, dev->val, dev->channel, dev->stat_hi);
        dev->state = KBC_STATE_MAIN_IBF;
        dev->pending = 0;
        goto at_main_ibf;
        break;
    case KBC_STATE_CTRL_OUT:
        if (dev->status & KBC_STAT_IFULL) {
            dev->state = KBC_STATE_MAIN_IBF;
            i8042_ibf_process(dev);
        }
        if (!(dev->status & KBC_STAT_OFULL)) {
            i8042_send_to_ob(dev, dev->key_ctrl_queue[dev->key_ctrl_queue_start], 0, 0x00);
            dev->key_ctrl_queue_start = (uint8_t)((dev->key_ctrl_queue_start + 1) & 0x3F);
            if (dev->key_ctrl_queue_start == dev->key_ctrl_queue_end) {
                dev->state = KBC_STATE_MAIN_IBF;
            }
        }
        break;
    case KBC_STATE_PARAM:
        if (dev->status & KBC_STAT_IFULL) {
            if (dev->status & KBC_STAT_CD) {
                dev->state = KBC_STATE_MAIN_IBF;
            }
            dev->status &= (uint8_t)~KBC_STAT_IFULL;
            i8042_process_cmd(dev);
        }
        break;
    case KBC_STATE_SEND_KBD:
        if (!dev->ports[0].wantcmd) {
            dev->state = KBC_STATE_SCAN_KBD;
        }
        break;
    case KBC_STATE_SCAN_KBD:
        i8042_scan_keyboard_at(dev);
        break;
    }
}

static void i8042_poll_ps2(i8042_t* dev)
{
    switch (dev->state) {
    case KBC_STATE_RESET:
        if (dev->status & KBC_STAT_IFULL) {
            dev->status = (uint8_t)(((dev->status & 0x0F) | KBC_STAT_UNLOCKED) & (uint8_t)~KBC_STAT_IFULL);
            if ((dev->status & KBC_STAT_CD) && (dev->ib == 0xAA)) {
                i8042_process_cmd(dev);
            }
        }
        break;
    case KBC_STATE_AMI_OUT:
        if (dev->status & KBC_STAT_OFULL) {
            break;
        }
        //fallthrough
    case KBC_STATE_MAIN_IBF:
    default:
        if (dev->status & KBC_STAT_IFULL) {
            i8042_ibf_process(dev);
        } else if (!(dev->status & KBC_STAT_OFULL)) {
            if (dev->mem[0x20] & KBC_CCB_PCMODE) {
                if (!(dev->mem[0x20] & KBC_CCB_ENABLEKBD)) {
                    dev->p2 &= 0xBF;
                    dev->state = KBC_STATE_MAIN_KBD;
                }
            } else {
                dev->p2 &= 0xF7;
                if (dev->mem[0x20] & KBC_CCB_ENABLEKBD) {
                    dev->state = KBC_STATE_MAIN_AUX;
                } else {
                    dev->p2 &= 0xBF;
                    dev->state = KBC_STATE_MAIN_BOTH;
                }
            }
        }
        break;
    case KBC_STATE_MAIN_KBD:
        if (dev->status & KBC_STAT_IFULL) {
            i8042_ibf_process(dev);
        } else {
            (void)i8042_scan_keyboard_ps2(dev);
            dev->state = KBC_STATE_MAIN_IBF;
        }
        break;
    case KBC_STATE_MAIN_AUX:
        if (dev->status & KBC_STAT_IFULL) {
            i8042_ibf_process(dev);
        } else {
            (void)i8042_scan_aux_ps2(dev);
            dev->state = KBC_STATE_MAIN_IBF;
        }
        break;
    case KBC_STATE_MAIN_BOTH:
        if (dev->status & KBC_STAT_IFULL) {
            i8042_ibf_process(dev);
        } else if (i8042_scan_keyboard_ps2(dev)) {
            dev->state = KBC_STATE_MAIN_IBF;
        } else {
            dev->state = KBC_STATE_MAIN_AUX;
        }
        break;
    case KBC_STATE_DELAY_OUT:
        i8042_send_to_ob(dev, dev->val, dev->channel, dev->stat_hi);
        dev->state = KBC_STATE_MAIN_IBF;
        dev->pending = 0;
        break;
    case KBC_STATE_CTRL_OUT:
        if (dev->status & KBC_STAT_IFULL) {
            dev->state = KBC_STATE_MAIN_IBF;
            i8042_ibf_process(dev);
        }
        if (!(dev->status & KBC_STAT_OFULL)) {
            i8042_send_to_ob(dev, dev->key_ctrl_queue[dev->key_ctrl_queue_start], 0, 0x00);
            dev->key_ctrl_queue_start = (uint8_t)((dev->key_ctrl_queue_start + 1) & 0x3F);
            if (dev->key_ctrl_queue_start == dev->key_ctrl_queue_end) {
                dev->state = KBC_STATE_MAIN_IBF;
            }
        }
        break;
    case KBC_STATE_PARAM:
        if (dev->status & KBC_STAT_IFULL) {
            if (dev->status & KBC_STAT_CD) {
                dev->state = KBC_STATE_MAIN_IBF;
            }
            dev->status &= (uint8_t)~KBC_STAT_IFULL;
            i8042_process_cmd(dev);
        }
        break;
    case KBC_STATE_SEND_KBD:
        if (!dev->ports[0].wantcmd) {
            dev->state = KBC_STATE_SCAN_KBD;
        }
        break;
    case KBC_STATE_SCAN_KBD:
        (void)i8042_scan_keyboard_ps2(dev);
        break;
    case KBC_STATE_SEND_AUX:
        if (!dev->ports[1].wantcmd) {
            dev->state = KBC_STATE_SCAN_AUX;
        }
        break;
    case KBC_STATE_SCAN_AUX:
        (void)i8042_scan_aux_ps2(dev);
        break;
    }
}

static void i8042_poll_core(i8042_t* dev)
{
    if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
        i8042_poll_ps2(dev);
    } else {
        i8042_poll_at(dev);
    }
}

static void i8042_poll(void* priv)
{
    i8042_poll_core((i8042_t*)priv);
}

static void i8042_dev_poll(void* priv)
{
    i8042_t* dev = (i8042_t*)priv;

    if ((dev->ports[0].poll != NULL) && (dev->ports[0].priv != NULL)) {
        dev->ports[0].poll(dev->ports[0].priv);
    }
    if ((dev->ports[1].poll != NULL) && (dev->ports[1].priv != NULL)) {
        dev->ports[1].poll(dev->ports[1].priv);
    }
}

static uint8_t i8042_read_data(void)
{
    uint8_t ret = kbc.ob;

    kbc.status &= (uint8_t)~KBC_STAT_OFULL;
    if (!(kbc.misc_flags & KBC_FLAG_PS2_MODE) && (kbc.pic != NULL)) {
        i8259_setlevelirq(kbc.pic, 1, 0);
    }
    return ret;
}

static uint8_t i8042_read_status(void)
{
    return kbc.status;
}

static void i8042_write_data(uint8_t value)
{
    kbc.status &= (uint8_t)~KBC_STAT_CD;
    if (kbc.wantdata && (kbc.command == 0xD1)) {
        i8042_write_p2_fast_a20(&kbc, (uint8_t)((kbc.p2 & 0xFD) | (value & 0x02)));
        kbc.wantdata = 0;
        kbc.state = KBC_STATE_MAIN_IBF;
        kbc.status &= (uint8_t)~KBC_STAT_IFULL;
        return;
    }
    kbc.ib = value;
    kbc.status |= KBC_STAT_IFULL;
}

static void i8042_write_cmd(uint8_t value)
{
    kbc.status |= KBC_STAT_CD;
    if (value == 0xD1) {
        kbc.wantdata = 1;
        kbc.state = KBC_STATE_PARAM;
        kbc.command = 0xD1;
        kbc.status &= (uint8_t)~KBC_STAT_IFULL;
        return;
    } else if (value == 0xAD) {
        i8042_set_enable_kbd(&kbc, 0);
        kbc.state = KBC_STATE_MAIN_IBF;
        kbc.status &= (uint8_t)~KBC_STAT_IFULL;
        return;
    } else if (value == 0xAE) {
        i8042_set_enable_kbd(&kbc, 1);
        kbc.state = KBC_STATE_MAIN_IBF;
        kbc.status &= (uint8_t)~KBC_STAT_IFULL;
        return;
    }
    kbc.ib = value;
    kbc.status |= KBC_STAT_IFULL;
}

static uint8_t i8042_readport(void* dummy, uint16_t portnum)
{
    (void)dummy;
    switch (portnum) {
    case 0x60:
        return i8042_read_data();
    case 0x64:
        return i8042_read_status();
    default:
        return 0xFF;
    }
}

static void i8042_writeport(void* dummy, uint16_t portnum, uint8_t value)
{
    (void)dummy;
    switch (portnum) {
    case 0x60:
        i8042_write_data(value);
        break;
    case 0x64:
        i8042_write_cmd(value);
        break;
    default:
        break;
    }
}

void i8042_reset(i8042_t* dev)
{
    if (dev == NULL) {
        return;
    }

    dev->state = KBC_STATE_RESET;
    dev->command = 0x00;
    dev->command_phase = 0;
    dev->status = KBC_STAT_UNLOCKED;
    dev->wantdata = 0;
    dev->ib = 0x00;
    dev->ob = 0x00;
    dev->sc_or = 0x00;
    dev->mem_addr = 0x00;
    dev->misc_flags = KBC_FLAG_CACHE;
    dev->ami_flags = i8042_is_ps2_default(dev) ? 0x01 : 0x00;
    dev->pending = 0;
    dev->val = 0x00;
    dev->channel = 1;
    dev->stat_hi = 0x00;
    dev->pulse_pending = 0;
    memset(dev->mem, 0, sizeof(dev->mem));
    i8042_ctrl_queue_reset(dev);

    dev->mem[0x20] = (uint8_t)(KBC_CCB_ENABLEKINT | KBC_CCB_TRANSLATE);
    dev->p1 = 0xF0;
    i8042_set_enable_kbd(dev, 0);
    i8042_set_enable_aux(dev, 0);
    dev->p2 = 0xCD;
    dev->old_p2 = dev->p2;

    if ((dev->profile.flags & KBC_PROFILE_FLAG_PS2_CAPABLE) && (dev->ami_flags & 0x01)) {
        dev->misc_flags |= KBC_FLAG_PS2_MODE;
    } else {
        dev->misc_flags &= (uint8_t)~KBC_FLAG_PS2_MODE;
    }

    dev->ami_is_amikey_2 = (uint8_t)((dev->profile.vendor == KBC_VENDOR_AMI) &&
        (((dev->profile.revision >= 'H') && (dev->profile.revision < 'X')) || (dev->profile.revision == '5')));

    dev->ports[0].wantcmd = 0;
    dev->ports[0].dat = 0;
    dev->ports[0].out_new = -1;
    dev->ports[1].wantcmd = 0;
    dev->ports[1].dat = 0;
    dev->ports[1].out_new = -1;

    if (dev->misc_flags & KBC_FLAG_PS2_MODE) {
        if (dev->pic_aux != NULL) {
            i8259_clearirq(dev->pic_aux, 4);
        }
        if (dev->pic != NULL) {
            i8259_clearirq(dev->pic, 1);
        }
        i8042_write_p2(dev, 0x4B);
    } else {
        if (dev->pic != NULL) {
            i8259_setlevelirq(dev->pic, 1, 0);
        }
        i8042_write_p2(dev, 0x8F);
    }
    dev->old_p2 = dev->p2;
    dev->status = (uint8_t)((dev->status & 0x0F) | (dev->p1 & 0xF0));

    keyboard_at_reset(&dev->keyboard, 0);
}

void i8042_init(CPU_t* cpu, I8259_t* pic, I8259_t* pic_aux, KBC_PROFILE_t profile)
{
    memset(&kbc, 0, sizeof(kbc));
    kbc.cpu = cpu;
    kbc.pic = pic;
    kbc.pic_aux = pic_aux;
    kbc.profile = profile;

    keyboard_at_init(&kbc.keyboard, &kbc.ports[0]);

    kbc.poll_timer = timing_addTimer(i8042_poll, &kbc, 10000, TIMING_ENABLED);
    kbc.dev_poll_timer = timing_addTimer(i8042_dev_poll, &kbc, 10000, TIMING_ENABLED);
    kbc.pulse_timer = timing_addTimer(i8042_pulse_restore, &kbc, 1000, TIMING_DISABLED);

    i8042_reset(&kbc);

    ports_cbRegister(0x60, 1, i8042_readport, NULL, i8042_writeport, NULL, NULL);
    ports_cbRegister(0x64, 1, i8042_readport, NULL, i8042_writeport, NULL, NULL);
}

void i8042_handle_host_key(i8042_t* dev, uint16_t scan, uint8_t down)
{
    if (dev == NULL) {
        return;
    }

    keyboard_at_handle_host_key(&dev->keyboard, scan, down);
}
