#include <rfb/keysym.h>
#include "hostkey_vnc.h"

static HOST_KEY_SCANCODE_t hostkey_vnc_translate_printable(rfbKeySym keysym)
{
    switch (keysym) {
    case 'a': case 'A': return HOST_KEY_SCAN_A;
    case 'b': case 'B': return HOST_KEY_SCAN_B;
    case 'c': case 'C': return HOST_KEY_SCAN_C;
    case 'd': case 'D': return HOST_KEY_SCAN_D;
    case 'e': case 'E': return HOST_KEY_SCAN_E;
    case 'f': case 'F': return HOST_KEY_SCAN_F;
    case 'g': case 'G': return HOST_KEY_SCAN_G;
    case 'h': case 'H': return HOST_KEY_SCAN_H;
    case 'i': case 'I': return HOST_KEY_SCAN_I;
    case 'j': case 'J': return HOST_KEY_SCAN_J;
    case 'k': case 'K': return HOST_KEY_SCAN_K;
    case 'l': case 'L': return HOST_KEY_SCAN_L;
    case 'm': case 'M': return HOST_KEY_SCAN_M;
    case 'n': case 'N': return HOST_KEY_SCAN_N;
    case 'o': case 'O': return HOST_KEY_SCAN_O;
    case 'p': case 'P': return HOST_KEY_SCAN_P;
    case 'q': case 'Q': return HOST_KEY_SCAN_Q;
    case 'r': case 'R': return HOST_KEY_SCAN_R;
    case 's': case 'S': return HOST_KEY_SCAN_S;
    case 't': case 'T': return HOST_KEY_SCAN_T;
    case 'u': case 'U': return HOST_KEY_SCAN_U;
    case 'v': case 'V': return HOST_KEY_SCAN_V;
    case 'w': case 'W': return HOST_KEY_SCAN_W;
    case 'x': case 'X': return HOST_KEY_SCAN_X;
    case 'y': case 'Y': return HOST_KEY_SCAN_Y;
    case 'z': case 'Z': return HOST_KEY_SCAN_Z;
    case '1': case '!': return HOST_KEY_SCAN_1;
    case '2': case '@': return HOST_KEY_SCAN_2;
    case '3': case '#': return HOST_KEY_SCAN_3;
    case '4': case '$': return HOST_KEY_SCAN_4;
    case '5': case '%': return HOST_KEY_SCAN_5;
    case '6': case '^': return HOST_KEY_SCAN_6;
    case '7': case '&': return HOST_KEY_SCAN_7;
    case '8': case '*': return HOST_KEY_SCAN_8;
    case '9': case '(': return HOST_KEY_SCAN_9;
    case '0': case ')': return HOST_KEY_SCAN_0;
    case '-': case '_': return HOST_KEY_SCAN_MINUS;
    case '=': case '+': return HOST_KEY_SCAN_EQUALS;
    case '[': case '{': return HOST_KEY_SCAN_LEFTBRACKET;
    case ']': case '}': return HOST_KEY_SCAN_RIGHTBRACKET;
    case ';': case ':': return HOST_KEY_SCAN_SEMICOLON;
    case '\'': case '"': return HOST_KEY_SCAN_APOSTROPHE;
    case '`': case '~': return HOST_KEY_SCAN_GRAVE;
    case '\\': case '|': return HOST_KEY_SCAN_BACKSLASH;
    case ',': case '<': return HOST_KEY_SCAN_COMMA;
    case '.': case '>': return HOST_KEY_SCAN_PERIOD;
    case '/': case '?': return HOST_KEY_SCAN_SLASH;
    case ' ': return HOST_KEY_SCAN_SPACE;
    default:
        return 0;
    }
}

uint8_t hostkey_vnc_translate(rfbKeySym keysym, uint8_t down, HOST_KEY_EVENT_t out[HOSTKEY_VNC_MAX_EVENTS])
{
    HOST_KEY_SCANCODE_t scan;

    if (out == NULL) {
        return 0;
    }

    scan = hostkey_vnc_translate_printable(keysym);
    if (scan == 0) {
        switch (keysym) {
        case XK_BackSpace: scan = HOST_KEY_SCAN_BACKSPACE; break;
        case XK_Tab:
        case XK_ISO_Left_Tab: scan = HOST_KEY_SCAN_TAB; break;
        case XK_Return: scan = HOST_KEY_SCAN_ENTER; break;
        case XK_Escape: scan = HOST_KEY_SCAN_ESCAPE; break;
        case XK_Caps_Lock: scan = HOST_KEY_SCAN_CAPSLOCK; break;
        case XK_Num_Lock: scan = HOST_KEY_SCAN_NUMLOCK; break;
        case XK_Scroll_Lock: scan = HOST_KEY_SCAN_SCROLLLOCK; break;
        case XK_Shift_L: scan = HOST_KEY_SCAN_LEFTSHIFT; break;
        case XK_Shift_R: scan = HOST_KEY_SCAN_RIGHTSHIFT; break;
        case XK_Control_L: scan = HOST_KEY_SCAN_LEFTCTRL; break;
        case XK_Control_R: scan = HOST_KEY_SCAN_RIGHTCTRL; break;
        case XK_Alt_L: scan = HOST_KEY_SCAN_LEFTALT; break;
        case XK_Alt_R:
        case XK_ISO_Level3_Shift:
        case XK_Mode_switch: scan = HOST_KEY_SCAN_RIGHTALT; break;
        case XK_Super_L: scan = HOST_KEY_SCAN_LEFTGUI; break;
        case XK_Super_R: scan = HOST_KEY_SCAN_RIGHTGUI; break;
        case XK_Menu: scan = HOST_KEY_SCAN_APPLICATION; break;
        case XK_Print: scan = HOST_KEY_SCAN_PRINTSCREEN; break;
        case XK_Pause: scan = HOST_KEY_SCAN_PAUSE; break;
        case XK_F1: scan = HOST_KEY_SCAN_F1; break;
        case XK_F2: scan = HOST_KEY_SCAN_F2; break;
        case XK_F3: scan = HOST_KEY_SCAN_F3; break;
        case XK_F4: scan = HOST_KEY_SCAN_F4; break;
        case XK_F5: scan = HOST_KEY_SCAN_F5; break;
        case XK_F6: scan = HOST_KEY_SCAN_F6; break;
        case XK_F7: scan = HOST_KEY_SCAN_F7; break;
        case XK_F8: scan = HOST_KEY_SCAN_F8; break;
        case XK_F9: scan = HOST_KEY_SCAN_F9; break;
        case XK_F10: scan = HOST_KEY_SCAN_F10; break;
        case XK_F11: scan = HOST_KEY_SCAN_F11; break;
        case XK_F12: scan = HOST_KEY_SCAN_F12; break;
        case XK_Home: scan = HOST_KEY_SCAN_HOME; break;
        case XK_Up: scan = HOST_KEY_SCAN_UP; break;
        case XK_Page_Up: scan = HOST_KEY_SCAN_PAGEUP; break;
        case XK_Left: scan = HOST_KEY_SCAN_LEFT; break;
        case XK_Right: scan = HOST_KEY_SCAN_RIGHT; break;
        case XK_End: scan = HOST_KEY_SCAN_END; break;
        case XK_Down: scan = HOST_KEY_SCAN_DOWN; break;
        case XK_Page_Down: scan = HOST_KEY_SCAN_PAGEDOWN; break;
        case XK_Insert: scan = HOST_KEY_SCAN_INSERT; break;
        case XK_Delete: scan = HOST_KEY_SCAN_DELETE; break;
        case XK_KP_Multiply: scan = HOST_KEY_SCAN_KP_MULTIPLY; break;
        case XK_KP_Subtract: scan = HOST_KEY_SCAN_KP_MINUS; break;
        case XK_KP_Add: scan = HOST_KEY_SCAN_KP_PLUS; break;
        case XK_KP_Divide: scan = HOST_KEY_SCAN_KP_DIVIDE; break;
        case XK_KP_Enter: scan = HOST_KEY_SCAN_KP_ENTER; break;
        case XK_KP_7:
        case XK_KP_Home: scan = HOST_KEY_SCAN_KP_7; break;
        case XK_KP_8:
        case XK_KP_Up: scan = HOST_KEY_SCAN_KP_8; break;
        case XK_KP_9:
        case XK_KP_Page_Up: scan = HOST_KEY_SCAN_KP_9; break;
        case XK_KP_4:
        case XK_KP_Left: scan = HOST_KEY_SCAN_KP_4; break;
        case XK_KP_5:
        case XK_KP_Begin: scan = HOST_KEY_SCAN_KP_5; break;
        case XK_KP_6:
        case XK_KP_Right: scan = HOST_KEY_SCAN_KP_6; break;
        case XK_KP_1:
        case XK_KP_End: scan = HOST_KEY_SCAN_KP_1; break;
        case XK_KP_2:
        case XK_KP_Down: scan = HOST_KEY_SCAN_KP_2; break;
        case XK_KP_3:
        case XK_KP_Page_Down: scan = HOST_KEY_SCAN_KP_3; break;
        case XK_KP_0:
        case XK_KP_Insert: scan = HOST_KEY_SCAN_KP_0; break;
        case XK_KP_Decimal:
        case XK_KP_Delete: scan = HOST_KEY_SCAN_KP_PERIOD; break;
        default:
            return 0;
        }
    }

    out[0].scan = scan;
    out[0].down = down ? 1 : 0;
    return 1;
}
