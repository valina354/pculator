#include <string.h>
#include "keyboard_at.h"

enum {
    KEYBOARD_SHIFT_LCTRL = 0x01,
    KEYBOARD_SHIFT_LSHIFT = 0x02,
    KEYBOARD_SHIFT_LALT = 0x04,
    KEYBOARD_SHIFT_LWIN = 0x08,
    KEYBOARD_SHIFT_RCTRL = 0x10,
    KEYBOARD_SHIFT_RSHIFT = 0x20,
    KEYBOARD_SHIFT_RALT = 0x40,
    KEYBOARD_SHIFT_RWIN = 0x80,
    KEYBOARD_SHIFT_MASK = KEYBOARD_SHIFT_LSHIFT | KEYBOARD_SHIFT_RSHIFT
};

typedef struct {
    uint16_t scan;
    uint8_t set1;
    uint8_t set2;
    uint8_t set3;
    uint8_t flags;
} keyboard_at_ext_map_t;

#define KEYBOARD_AT_EXT_FLAG_SET1_E0 0x01
#define KEYBOARD_AT_EXT_FLAG_SET2_E0 0x02
#define KEYBOARD_AT_EXT_FLAG_SET3_E0 0x04

static const uint8_t keyboard_at_id_bytes[] = { 0xAB, 0x83 };

static const uint8_t keyboard_at_set2_base[0x59] = {
    0x00, 0x76, 0x16, 0x1E, 0x26, 0x25, 0x2E, 0x36,
    0x3D, 0x3E, 0x46, 0x45, 0x4E, 0x55, 0x66, 0x0D,
    0x15, 0x1D, 0x24, 0x2D, 0x2C, 0x35, 0x3C, 0x43,
    0x44, 0x4D, 0x54, 0x5B, 0x5A, 0x14, 0x1C, 0x1B,
    0x23, 0x2B, 0x34, 0x33, 0x3B, 0x42, 0x4B, 0x4C,
    0x52, 0x0E, 0x12, 0x5D, 0x1A, 0x22, 0x21, 0x2A,
    0x32, 0x31, 0x3A, 0x41, 0x49, 0x4A, 0x59, 0x7C,
    0x11, 0x29, 0x58, 0x05, 0x06, 0x04, 0x0C, 0x03,
    0x0B, 0x83, 0x0A, 0x01, 0x09, 0x77, 0x7E, 0x6C,
    0x75, 0x7D, 0x7B, 0x6B, 0x73, 0x74, 0x79, 0x69,
    0x72, 0x7A, 0x70, 0x71, 0x84, 0x00, 0x61, 0x78,
    0x07
};

static const uint8_t keyboard_at_set3_base[0x59] = {
    0x00, 0x08, 0x16, 0x1E, 0x26, 0x25, 0x2E, 0x36,
    0x3D, 0x3E, 0x46, 0x45, 0x4E, 0x55, 0x66, 0x0D,
    0x15, 0x1D, 0x24, 0x2D, 0x2C, 0x35, 0x3C, 0x43,
    0x44, 0x4D, 0x54, 0x5B, 0x5A, 0x11, 0x1C, 0x1B,
    0x23, 0x2B, 0x34, 0x33, 0x3B, 0x42, 0x4B, 0x4C,
    0x52, 0x0E, 0x12, 0x5C, 0x1A, 0x22, 0x21, 0x2A,
    0x32, 0x31, 0x3A, 0x41, 0x49, 0x4A, 0x59, 0x7E,
    0x19, 0x29, 0x14, 0x07, 0x0F, 0x17, 0x1F, 0x27,
    0x2F, 0x37, 0x3F, 0x47, 0x4F, 0x76, 0x5F, 0x6C,
    0x75, 0x7D, 0x84, 0x6B, 0x73, 0x74, 0x7C, 0x69,
    0x72, 0x7A, 0x70, 0x71, 0x57, 0x00, 0x00, 0x56,
    0x5E
};

static const keyboard_at_ext_map_t keyboard_at_ext_map[] = {
    { 0x11C, 0x1C, 0x5A, 0x5A, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x11D, 0x1D, 0x14, 0x14, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x135, 0x35, 0x4A, 0x4A, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x137, 0x37, 0x7C, 0x7C, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x138, 0x38, 0x11, 0x39, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x146, 0x46, 0x7E, 0x7E, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 | KEYBOARD_AT_EXT_FLAG_SET3_E0 },
    { 0x147, 0x47, 0x6C, 0x6C, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x148, 0x48, 0x75, 0x75, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x149, 0x49, 0x7D, 0x7D, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x14B, 0x4B, 0x6B, 0x6B, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x14D, 0x4D, 0x74, 0x74, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x14F, 0x4F, 0x69, 0x69, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x150, 0x50, 0x72, 0x72, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x151, 0x51, 0x7A, 0x7A, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x152, 0x52, 0x70, 0x70, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x153, 0x53, 0x71, 0x71, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x15B, 0x5B, 0x1F, 0x8B, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x15C, 0x5C, 0x27, 0x8C, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 },
    { 0x15D, 0x5D, 0x2F, 0x8D, KEYBOARD_AT_EXT_FLAG_SET1_E0 | KEYBOARD_AT_EXT_FLAG_SET2_E0 }
};

static uint8_t keyboard_at_fake_shift_needed(uint16_t scan)
{
    switch (scan) {
    case 0x137:
    case 0x147:
    case 0x148:
    case 0x149:
    case 0x14A:
    case 0x14B:
    case 0x14D:
    case 0x14F:
    case 0x150:
    case 0x151:
    case 0x152:
    case 0x153:
        return 1;
    default:
        return 0;
    }
}

static void keyboard_at_queue_bytes(keyboard_at_t* keyboard, const uint8_t* data, uint8_t len, uint8_t main)
{
    uint8_t i;

    for (i = 0; i < len; i++) {
        if (data[i] != 0x00) {
            kbc_at_dev_queue_add(&keyboard->dev, data[i], main);
        }
    }
}

static void keyboard_at_update_shift_state(keyboard_at_t* keyboard, uint16_t scan, uint8_t down)
{
    uint8_t mask = 0;

    switch (scan) {
    case 0x01D:
        mask = KEYBOARD_SHIFT_LCTRL;
        break;
    case 0x11D:
        mask = KEYBOARD_SHIFT_RCTRL;
        break;
    case 0x02A:
        mask = KEYBOARD_SHIFT_LSHIFT;
        break;
    case 0x036:
        mask = KEYBOARD_SHIFT_RSHIFT;
        break;
    case 0x038:
        mask = KEYBOARD_SHIFT_LALT;
        break;
    case 0x138:
        mask = KEYBOARD_SHIFT_RALT;
        break;
    case 0x15B:
        mask = KEYBOARD_SHIFT_LWIN;
        break;
    case 0x15C:
        mask = KEYBOARD_SHIFT_RWIN;
        break;
    default:
        break;
    }

    if (mask == 0) {
        return;
    }

    if (down) {
        keyboard->shift_state |= mask;
    } else {
        keyboard->shift_state &= (uint8_t)~mask;
    }
}

static uint8_t keyboard_at_num_lock_on(const keyboard_at_t* keyboard)
{
    return (uint8_t)((keyboard->led_state & 0x02) != 0);
}

static void keyboard_at_queue_fake_shift(keyboard_at_t* keyboard, uint8_t open)
{
    uint8_t seq[3];
    uint8_t shift_states;
    uint8_t num_lock;

    shift_states = keyboard->shift_state & KEYBOARD_SHIFT_MASK;
    num_lock = keyboard_at_num_lock_on(keyboard);
    memset(seq, 0, sizeof(seq));

    if (keyboard->mode == 0x01) {
        if (num_lock) {
            if (shift_states != 0) {
                return;
            }
            seq[0] = 0xE0;
            seq[1] = open ? 0x2A : 0xAA;
            keyboard_at_queue_bytes(keyboard, seq, 2, 1);
            return;
        }

        if (shift_states & KEYBOARD_SHIFT_LSHIFT) {
            seq[0] = 0xE0;
            seq[1] = open ? 0xAA : 0x2A;
            keyboard_at_queue_bytes(keyboard, seq, 2, 1);
        }
        if (shift_states & KEYBOARD_SHIFT_RSHIFT) {
            seq[0] = 0xE0;
            seq[1] = open ? 0xB6 : 0x36;
            keyboard_at_queue_bytes(keyboard, seq, 2, 1);
        }
        return;
    }

    if (num_lock) {
        if (shift_states != 0) {
            return;
        }
        seq[0] = 0xE0;
        if (open) {
            seq[1] = 0x12;
            keyboard_at_queue_bytes(keyboard, seq, 2, 1);
        } else {
            seq[1] = 0xF0;
            seq[2] = 0x12;
            keyboard_at_queue_bytes(keyboard, seq, 3, 1);
        }
        return;
    }

    if (shift_states & KEYBOARD_SHIFT_LSHIFT) {
        seq[0] = 0xE0;
        if (open) {
            seq[1] = 0xF0;
            seq[2] = 0x12;
            keyboard_at_queue_bytes(keyboard, seq, 3, 1);
        } else {
            seq[1] = 0x12;
            keyboard_at_queue_bytes(keyboard, seq, 2, 1);
        }
    }

    if (shift_states & KEYBOARD_SHIFT_RSHIFT) {
        seq[0] = 0xE0;
        if (open) {
            seq[1] = 0xF0;
            seq[2] = 0x59;
            keyboard_at_queue_bytes(keyboard, seq, 3, 1);
        } else {
            seq[1] = 0x59;
            keyboard_at_queue_bytes(keyboard, seq, 2, 1);
        }
    }
}

static uint8_t keyboard_at_fill_ext_sequence(uint8_t code, uint8_t flags, uint8_t mode, uint8_t down, uint8_t out[4])
{
    memset(out, 0, 4);

    if (mode == 0x01) {
        if (flags & KEYBOARD_AT_EXT_FLAG_SET1_E0) {
            out[0] = 0xE0;
            out[1] = down ? code : (uint8_t)(code | 0x80);
            return 2;
        }
        out[0] = down ? code : (uint8_t)(code | 0x80);
        return 1;
    }

    if (mode == 0x02) {
        if (flags & KEYBOARD_AT_EXT_FLAG_SET2_E0) {
            out[0] = 0xE0;
            if (down) {
                out[1] = code;
                return 2;
            }
            out[1] = 0xF0;
            out[2] = code;
            return 3;
        }
        if (down) {
            out[0] = code;
            return 1;
        }
        out[0] = 0xF0;
        out[1] = code;
        return 2;
    }

    if (flags & KEYBOARD_AT_EXT_FLAG_SET3_E0) {
        out[0] = 0xE0;
        if (down) {
            out[1] = code;
            return 2;
        }
        out[1] = 0xF0;
        out[2] = code;
        return 3;
    }

    if (down) {
        out[0] = code;
        return 1;
    }

    out[0] = 0xF0;
    out[1] = code;
    return 2;
}

static uint8_t keyboard_at_fill_sequence(keyboard_at_t* keyboard, uint16_t scan, uint8_t down, uint8_t out[4])
{
    size_t i;
    uint8_t code;

    memset(out, 0, 4);

    if (scan < 0x80) {
        if (keyboard->mode == 0x01) {
            if (scan == 0x055) {
                return 0;
            }
            if ((scan > 0x058) && (scan != 0x05B) && (scan != 0x05C)) {
                return 0;
            }
            code = (uint8_t)scan;
            if (scan == 0x056) {
                code = 0x56;
            }
            out[0] = down ? code : (uint8_t)(code | 0x80);
            return 1;
        }

        if (keyboard->mode == 0x02) {
            if ((scan >= sizeof(keyboard_at_set2_base)) || (keyboard_at_set2_base[scan] == 0x00)) {
                return 0;
            }
            code = keyboard_at_set2_base[scan];
            if (down) {
                out[0] = code;
                return 1;
            }
            out[0] = 0xF0;
            out[1] = code;
            return 2;
        }

        if ((scan >= sizeof(keyboard_at_set3_base)) || (keyboard_at_set3_base[scan] == 0x00)) {
            return 0;
        }
        code = keyboard_at_set3_base[scan];
        if (down) {
            out[0] = code;
            return 1;
        }
        out[0] = 0xF0;
        out[1] = code;
        return 2;
    }

    for (i = 0; i < (sizeof(keyboard_at_ext_map) / sizeof(keyboard_at_ext_map[0])); i++) {
        if (keyboard_at_ext_map[i].scan == scan) {
            if (keyboard->mode == 0x01) {
                return keyboard_at_fill_ext_sequence(keyboard_at_ext_map[i].set1, keyboard_at_ext_map[i].flags, keyboard->mode, down, out);
            }
            if (keyboard->mode == 0x02) {
                return keyboard_at_fill_ext_sequence(keyboard_at_ext_map[i].set2, keyboard_at_ext_map[i].flags, keyboard->mode, down, out);
            }
            return keyboard_at_fill_ext_sequence(keyboard_at_ext_map[i].set3, keyboard_at_ext_map[i].flags, keyboard->mode, down, out);
        }
    }

    return 0;
}

static void keyboard_at_process_key(keyboard_at_t* keyboard, uint16_t scan, uint8_t down)
{
    uint8_t seq[4];
    uint8_t len;

    if ((scan == 0x145) || (scan == 0x12A)) {
        return;
    }

    if ((keyboard->mode == 0x03) && !down) {
        uint8_t make_seq[4];
        uint8_t make_len = keyboard_at_fill_sequence(keyboard, scan, 1, make_seq);
        if ((make_len != 0) && !keyboard->set3_all_break &&
            ((make_seq[0] >= sizeof(keyboard->set3_flags)) || ((keyboard->set3_flags[make_seq[0]] & 0x02) == 0))) {
            return;
        }
    }

    len = keyboard_at_fill_sequence(keyboard, scan, down, seq);
    if (len == 0) {
        return;
    }

    if (down && keyboard_at_fake_shift_needed(scan)) {
        keyboard_at_queue_fake_shift(keyboard, 1);
    }

    keyboard_at_queue_bytes(keyboard, seq, len, 1);

    if (!down && keyboard_at_fake_shift_needed(scan)) {
        keyboard_at_queue_fake_shift(keyboard, 0);
    }
}

static void keyboard_at_queue_pause(keyboard_at_t* keyboard)
{
    static const uint8_t pause_set1[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
    static const uint8_t pause_set2[] = { 0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77 };

    if (keyboard->mode == 0x01) {
        keyboard_at_queue_bytes(keyboard, pause_set1, sizeof(pause_set1), 1);
        return;
    }

    keyboard_at_queue_bytes(keyboard, pause_set2, sizeof(pause_set2), 1);
}

static void keyboard_at_set_defaults(keyboard_at_t* keyboard)
{
    keyboard->rate = 1;
    keyboard->mode = 0x02;
    keyboard->led_state = 0x00;
    keyboard->set3_all_break = 0;
    keyboard->set3_all_repeat = 0;
    memset(keyboard->set3_flags, 0, sizeof(keyboard->set3_flags));
}

static void keyboard_at_bat(void* priv)
{
    keyboard_at_t* keyboard = (keyboard_at_t*)priv;

    if (keyboard->bat_counter != 0) {
        keyboard->bat_counter--;
        keyboard->dev.state = DEV_STATE_EXECUTE_BAT;
        return;
    }

    keyboard_at_set_defaults(keyboard);
    keyboard->scan_enabled = 1;
    keyboard->in_reset = 0;
    memset(keyboard->pressed, 0, sizeof(keyboard->pressed));
    keyboard->shift_state = 0;
    kbc_at_dev_queue_add(&keyboard->dev, 0xAA, 0);
}

static void keyboard_at_invalid_cmd(keyboard_at_t* keyboard)
{
    kbc_at_dev_queue_add(&keyboard->dev, keyboard->inv_cmd_response, 0);
}

static void keyboard_at_write(void* priv)
{
    keyboard_at_t* keyboard = (keyboard_at_t*)priv;
    uint8_t val;

    if (keyboard->dev.port == NULL) {
        return;
    }

    val = keyboard->dev.port->dat;
    keyboard->dev.state = DEV_STATE_MAIN_OUT;

    if ((val < 0xED) && (keyboard->dev.flags & 0x08)) {
        keyboard->dev.flags &= (uint8_t)~0x08;

        switch (keyboard->dev.command) {
        case 0xED:
            kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
            keyboard->led_state = (uint8_t)(val & 0x0F);
            break;
        case 0xF0:
            switch (val) {
            case 0x00:
                kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
                kbc_at_dev_queue_add(&keyboard->dev, keyboard->mode, 0);
                break;
            case 0x01:
            case 0x02:
            case 0x03:
                keyboard->mode = val;
                kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
                break;
            default:
                kbc_at_dev_queue_add(&keyboard->dev, 0xFE, 0);
                keyboard->dev.flags |= 0x08;
                keyboard->dev.state = DEV_STATE_MAIN_WANT_IN;
                break;
            }
            break;
        case 0xF3:
            if (val & 0x80) {
                keyboard->dev.flags |= 0x08;
                kbc_at_dev_queue_add(&keyboard->dev, 0xFE, 0);
                keyboard->dev.state = DEV_STATE_MAIN_WANT_IN;
            } else {
                keyboard->rate = val;
                kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
            }
            break;
        default:
            kbc_at_dev_queue_add(&keyboard->dev, 0xFE, 0);
            break;
        }
        return;
    }

    if (keyboard->dev.flags & 0x08) {
        if ((val == 0xED) || (val == 0xEE) || (val == 0xF4) || (val == 0xF5) || (val == 0xF6)) {
            keyboard->dev.flags &= (uint8_t)~0x08;
        } else {
            keyboard->dev.state = DEV_STATE_MAIN_WANT_IN;
        }
    }

    switch (val) {
    case 0xED:
        keyboard->dev.command = val;
        keyboard->dev.flags |= 0x08;
        kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
        keyboard->dev.state = DEV_STATE_MAIN_WANT_IN;
        break;
    case 0xEE:
        kbc_at_dev_queue_add(&keyboard->dev, 0xEE, 0);
        break;
    case 0xEF:
    case 0xF1:
        keyboard_at_invalid_cmd(keyboard);
        break;
    case 0xF0:
        keyboard->dev.command = val;
        keyboard->dev.flags |= 0x08;
        kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
        keyboard->dev.state = DEV_STATE_MAIN_WANT_IN;
        break;
    case 0xF2:
        kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
        keyboard_at_queue_bytes(keyboard, keyboard_at_id_bytes, sizeof(keyboard_at_id_bytes), 0);
        break;
    case 0xF3:
        keyboard->dev.command = val;
        keyboard->dev.flags |= 0x08;
        kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
        keyboard->dev.state = DEV_STATE_MAIN_WANT_IN;
        break;
    case 0xF4:
        keyboard->scan_enabled = 1;
        kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
        break;
    case 0xF5:
    case 0xF6:
        keyboard->dev.port->out_new = -1;
        keyboard->dev.port->wantcmd = 0;
        kbc_at_dev_queue_reset(&keyboard->dev, 1);
        keyboard->dev.last_scan_code = 0x00;
        keyboard->scan_enabled = (uint8_t)((val == 0xF6) ? 1 : 0);
        kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
        keyboard_at_set_defaults(keyboard);
        break;
    case 0xF7:
        kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
        keyboard->set3_all_repeat = 1;
        break;
    case 0xF8:
        kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
        keyboard->set3_all_break = 1;
        break;
    case 0xF9:
        kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
        keyboard->set3_all_break = 0;
        break;
    case 0xFA:
        kbc_at_dev_queue_add(&keyboard->dev, 0xFA, 0);
        keyboard->set3_all_repeat = 1;
        keyboard->set3_all_break = 1;
        break;
    case 0xFB:
    case 0xFC:
    case 0xFD:
        keyboard_at_invalid_cmd(keyboard);
        break;
    case 0xFE:
        kbc_at_dev_queue_add(&keyboard->dev, keyboard->dev.last_scan_code, 0);
        break;
    case 0xFF:
        keyboard_at_reset(keyboard, 1);
        break;
    default:
        kbc_at_dev_queue_add(&keyboard->dev, 0xFE, 0);
        break;
    }
}

static void keyboard_at_input(keyboard_at_t* keyboard, uint8_t down, uint16_t scan)
{
    if (keyboard->in_reset) {
        return;
    }

    if ((scan >> 8) == 0xE1) {
        if ((scan & 0xFF) == 0x1D) {
            scan = 0x0100;
        }
    } else if ((scan >> 8) == 0xE0) {
        scan &= 0x00FF;
        scan |= 0x0100;
    } else if ((scan >> 8) != 0x01) {
        scan &= 0x00FF;
    }

    if ((scan > 0x01FF) || (scan == 0x0100)) {
        if (scan == 0x0100) {
            keyboard_at_queue_pause(keyboard);
        }
        return;
    }

    if ((keyboard->pressed[scan] ^ down) == 0) {
        return;
    }

    keyboard_at_update_shift_state(keyboard, scan, down);
    keyboard->pressed[scan] = down;
    keyboard_at_process_key(keyboard, scan, down);
}

void keyboard_at_init(keyboard_at_t* keyboard, kbc_at_port_t* port)
{
    if (keyboard == NULL) {
        return;
    }

    memset(keyboard, 0, sizeof(*keyboard));
    kbc_at_dev_init(&keyboard->dev, port);
    keyboard->dev.name = "AT keyboard";
    keyboard->dev.process_cmd = keyboard_at_write;
    keyboard->dev.execute_bat = keyboard_at_bat;
    keyboard->dev.scan = &keyboard->scan_enabled;
    keyboard->dev.fifo_mask = 0x0F;
    keyboard->inv_cmd_response = 0xFE;
    keyboard_at_reset(keyboard, 0);
    keyboard->bat_counter = 0;
}

void keyboard_at_reset(keyboard_at_t* keyboard, int do_fa)
{
    if (keyboard == NULL) {
        return;
    }

    keyboard->in_reset = 1;
    keyboard_at_set_defaults(keyboard);
    keyboard->shift_state = 0;
    memset(keyboard->pressed, 0, sizeof(keyboard->pressed));
    kbc_at_dev_reset(&keyboard->dev, do_fa);
    keyboard->bat_counter = 1000;
}

void keyboard_at_handle_host_key(keyboard_at_t* keyboard, uint16_t scan, uint8_t down)
{
    if (keyboard == NULL) {
        return;
    }

    if (scan == 0x137) {
        if (keyboard->pressed[0x038] || keyboard->pressed[0x138]) {
            scan = 0x054;
        } else if (down) {
            keyboard_at_input(keyboard, down, 0x12A);
        } else {
            keyboard_at_input(keyboard, down, scan);
            scan = 0x12A;
        }
    } else if (scan == 0x145) {
        if (keyboard->pressed[0x01D] || keyboard->pressed[0x11D]) {
            scan = 0x146;
        } else if (down) {
            keyboard_at_queue_pause(keyboard);
            return;
        } else {
            return;
        }
    }

    keyboard_at_input(keyboard, down, scan);
}
