#include "../../config.h"
#include <rfb/rfb.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "frontend_vnc.h"
#include "hostkey_vnc.h"
#include "../frontend.h"
#include "../../input/mouse.h"
#include "../../debuglog.h"

#define FRONTEND_VNC_DEFAULT_WIDTH 640
#define FRONTEND_VNC_DEFAULT_HEIGHT 400
#define FRONTEND_VNC_DEFAULT_PORT 5900
#define FRONTEND_VNC_TITLE_LEN 256
#define FRONTEND_VNC_BIND_LEN 64
#define FRONTEND_VNC_PASSWORD_LEN 128
#define FRONTEND_VNC_HELD_KEY_LEN 0x200
#define FRONTEND_VNC_BUTTON_LEFT 0x01
#define FRONTEND_VNC_BUTTON_RIGHT 0x04

extern volatile uint8_t running;

static rfbScreenInfoPtr frontend_vnc_screen = NULL;
static rfbClientPtr frontend_vnc_active_client = NULL;
static rfbCursorPtr frontend_vnc_invisible_cursor = NULL;
static uint32_t* frontend_vnc_frontbuffer = NULL;
static uint32_t* frontend_vnc_backbuffer = NULL;
static uint32_t* frontend_vnc_previousbuffer = NULL; //keep a copy of the framebuffer drawn at the last pump to compare against for dirty rects
static int frontend_vnc_width = 0;
static int frontend_vnc_height = 0;
static int frontend_vnc_previous_width = 0;
static int frontend_vnc_previous_height = 0;
static int frontend_vnc_last_pointer_x = 0;
static int frontend_vnc_last_pointer_y = 0;
static char frontend_vnc_title[FRONTEND_VNC_TITLE_LEN] = "";
static char frontend_vnc_bind_address[FRONTEND_VNC_BIND_LEN] = "0.0.0.0";
static char frontend_vnc_password[FRONTEND_VNC_PASSWORD_LEN] = "";
static char* frontend_vnc_password_list[2] = { frontend_vnc_password, NULL };
static uint16_t frontend_vnc_port = FRONTEND_VNC_DEFAULT_PORT;
static uint8_t frontend_vnc_wait_for_client = 0;
static uint8_t frontend_vnc_pointer_valid = 0;
static uint8_t frontend_vnc_last_button_mask = 0;
static volatile uint8_t frontend_vnc_frame_dirty = 0;
static volatile uint8_t frontend_vnc_do_full_frame_refresh = 0;
static volatile int frontend_vnc_is_pumping = 0;
static uint8_t frontend_vnc_held_keys[FRONTEND_VNC_HELD_KEY_LEN];

static void frontend_vnc_release_input_state(void);

static void frontend_vnc_configure_pixel_format(void)
{
    uint32_t endian_test = 1;

    if (frontend_vnc_screen == NULL) {
        return;
    }

    frontend_vnc_screen->serverFormat.bitsPerPixel = 32;
    frontend_vnc_screen->serverFormat.depth = 24;
    frontend_vnc_screen->serverFormat.trueColour = 1;
    frontend_vnc_screen->serverFormat.bigEndian = (*(uint8_t*)&endian_test == 0) ? 1 : 0;
    frontend_vnc_screen->serverFormat.redMax = 255;
    frontend_vnc_screen->serverFormat.greenMax = 255;
    frontend_vnc_screen->serverFormat.blueMax = 255;
    frontend_vnc_screen->serverFormat.redShift = 0;
    frontend_vnc_screen->serverFormat.greenShift = 8;
    frontend_vnc_screen->serverFormat.blueShift = 16;
    frontend_vnc_screen->blackPixel = 0x00000000u;
    frontend_vnc_screen->whitePixel = 0x00FFFFFFu;
}

static void frontend_vnc_destroy_server(void)
{
    frontend_vnc_release_input_state();
    frontend_vnc_active_client = NULL;

    if (frontend_vnc_screen != NULL) {
        rfbShutdownServer(frontend_vnc_screen, 1);
        rfbScreenCleanup(frontend_vnc_screen);
        frontend_vnc_screen = NULL;
    }

    free(frontend_vnc_frontbuffer);
    free(frontend_vnc_backbuffer);
    free(frontend_vnc_previousbuffer);
    frontend_vnc_frontbuffer = NULL;
    frontend_vnc_backbuffer = NULL;
    frontend_vnc_previousbuffer = NULL;
    frontend_vnc_width = 0;
    frontend_vnc_height = 0;
}

static int frontend_vnc_allocate_buffers(int width, int height, uint32_t** frontbuffer, uint32_t** backbuffer, uint32_t** previousbuffer)
{
    size_t pixel_count;

    if ((frontbuffer == NULL) || (backbuffer == NULL) || (width <= 0) || (height <= 0)) {
        return -1;
    }

    if ((size_t)width > (SIZE_MAX / sizeof(uint32_t)) / (size_t)height) {
        return -1;
    }

    pixel_count = (size_t)width * (size_t)height;
    *frontbuffer = (uint32_t*)calloc(pixel_count, sizeof(uint32_t));
    *backbuffer = (uint32_t*)calloc(pixel_count, sizeof(uint32_t));
    *previousbuffer = (uint32_t*)calloc(pixel_count, sizeof(uint32_t));
    if ((*frontbuffer == NULL) || (*backbuffer == NULL) || (*previousbuffer == NULL)) {
        free(*frontbuffer);
        free(*backbuffer);
        free(*previousbuffer);
        *frontbuffer = NULL;
        *backbuffer = NULL;
        *previousbuffer = NULL;
        return -1;
    }

    frontend_vnc_previous_width = width;
    frontend_vnc_previous_height = height;

    return 0;
}

static void frontend_vnc_reset_pointer_tracking(void)
{
    frontend_vnc_pointer_valid = 0;
    frontend_vnc_last_pointer_x = 0;
    frontend_vnc_last_pointer_y = 0;
    frontend_vnc_last_button_mask = 0;
}

static void frontend_vnc_release_mouse_buttons(void)
{
    if (frontend_vnc_last_button_mask & FRONTEND_VNC_BUTTON_LEFT) {
        frontend_inject_mouse_button(MOUSE_ACTION_LEFT, MOUSE_UNPRESSED);
    }
    if (frontend_vnc_last_button_mask & FRONTEND_VNC_BUTTON_RIGHT) {
        frontend_inject_mouse_button(MOUSE_ACTION_RIGHT, MOUSE_UNPRESSED);
    }

    frontend_vnc_last_button_mask = 0;
}

static void frontend_vnc_release_held_keys(void)
{
    size_t i;

    for (i = 0; i < sizeof(frontend_vnc_held_keys); i++) {
        if (frontend_vnc_held_keys[i]) {
            frontend_inject_key((HOST_KEY_SCANCODE_t)i, 0);
            frontend_vnc_held_keys[i] = 0;
        }
    }
}

static void frontend_vnc_release_input_state(void)
{
    frontend_vnc_release_mouse_buttons();
    frontend_vnc_release_held_keys();
    frontend_vnc_reset_pointer_tracking();
}

static void frontend_vnc_client_gone(rfbClientPtr cl)
{
    if (cl == frontend_vnc_active_client) {
        frontend_vnc_active_client = NULL;
        frontend_vnc_release_input_state();
    }
}

static enum rfbNewClientAction frontend_vnc_new_client(rfbClientPtr cl)
{
    if (cl == NULL) {
        return RFB_CLIENT_REFUSE;
    }

    if (frontend_vnc_active_client != NULL) {
        return RFB_CLIENT_REFUSE;
    }

    frontend_vnc_active_client = cl;
    frontend_vnc_release_input_state();
    frontend_vnc_frame_dirty = 1;
    cl->clientGoneHook = frontend_vnc_client_gone;
    return RFB_CLIENT_ACCEPT;
}

static int8_t frontend_vnc_clamp_mouse_delta(int value)
{
    if (value < -128) {
        return -128;
    }
    if (value > 127) {
        return 127;
    }

    return (int8_t)value;
}

static void frontend_vnc_handle_key_event(rfbBool down, rfbKeySym keysym, rfbClientPtr cl)
{
    HOST_KEY_EVENT_t host_event[HOSTKEY_VNC_MAX_EVENTS];
    uint8_t count;
    size_t i;

    if (cl != frontend_vnc_active_client) {
        return;
    }

    count = hostkey_vnc_translate(keysym, down ? 1 : 0, host_event);
    if (count == 0) {
        return;
    }

    for (i = 0; i < count; i++) {
        if (host_event[i].scan < FRONTEND_VNC_HELD_KEY_LEN) {
            frontend_vnc_held_keys[host_event[i].scan] = host_event[i].down ? 1 : 0;
        }
        frontend_inject_key(host_event[i].scan, host_event[i].down);
    }
}

static void frontend_vnc_handle_pointer_event(int button_mask, int x, int y, rfbClientPtr cl)
{
    int delta_x;
    int delta_y;
    int changed_mask;

    if (cl != frontend_vnc_active_client) {
        return;
    }

    if (!frontend_vnc_pointer_valid) {
        frontend_vnc_last_pointer_x = x;
        frontend_vnc_last_pointer_y = y;
        frontend_vnc_pointer_valid = 1;
    }
    else {
        delta_x = x - frontend_vnc_last_pointer_x;
        delta_y = y - frontend_vnc_last_pointer_y;
        frontend_vnc_last_pointer_x = x;
        frontend_vnc_last_pointer_y = y;
        if ((delta_x != 0) || (delta_y != 0)) {
            frontend_inject_mouse_move(frontend_vnc_clamp_mouse_delta(delta_x),
                frontend_vnc_clamp_mouse_delta(delta_y));
        }
    }

    changed_mask = button_mask ^ frontend_vnc_last_button_mask;
    if (changed_mask & FRONTEND_VNC_BUTTON_LEFT) {
        frontend_inject_mouse_button(MOUSE_ACTION_LEFT,
            (button_mask & FRONTEND_VNC_BUTTON_LEFT) ? MOUSE_PRESSED : MOUSE_UNPRESSED);
    }
    if (changed_mask & FRONTEND_VNC_BUTTON_RIGHT) {
        frontend_inject_mouse_button(MOUSE_ACTION_RIGHT,
            (button_mask & FRONTEND_VNC_BUTTON_RIGHT) ? MOUSE_PRESSED : MOUSE_UNPRESSED);
    }

    frontend_vnc_last_button_mask = (uint8_t)(button_mask & (FRONTEND_VNC_BUTTON_LEFT | FRONTEND_VNC_BUTTON_RIGHT));
    //rfbDefaultPtrAddEvent(button_mask, x, y, cl);
}

static void frontend_vnc_set_title_internal(const char* title)
{
    if (title == NULL) {
        frontend_vnc_title[0] = 0;
    }
    else {
        strncpy(frontend_vnc_title, title, sizeof(frontend_vnc_title) - 1);
        frontend_vnc_title[sizeof(frontend_vnc_title) - 1] = 0;
    }

    if (frontend_vnc_screen != NULL) {
        frontend_vnc_screen->desktopName = frontend_vnc_title;
    }
}

static int frontend_vnc_replace_framebuffer(int width, int height)
{
    uint32_t* old_frontbuffer = frontend_vnc_frontbuffer;
    uint32_t* old_backbuffer = frontend_vnc_backbuffer;
    uint32_t* old_prevbuffer = frontend_vnc_previousbuffer;
    uint32_t* new_frontbuffer = NULL;
    uint32_t* new_backbuffer = NULL;
    uint32_t* new_prevbuffer = NULL;

    if (frontend_vnc_allocate_buffers(width, height, &new_frontbuffer, &new_backbuffer, &new_prevbuffer) != 0) {
        return -1;
    }

    frontend_vnc_frontbuffer = new_frontbuffer;
    frontend_vnc_backbuffer = new_backbuffer;
    frontend_vnc_previousbuffer = new_prevbuffer;
    frontend_vnc_width = width;
    frontend_vnc_height = height;
    frontend_vnc_reset_pointer_tracking();

    if (frontend_vnc_screen != NULL) {
        rfbNewFramebuffer(frontend_vnc_screen, (char*)frontend_vnc_frontbuffer, width, height, 8, 3, 4);
        frontend_vnc_configure_pixel_format();
    }

    frontend_vnc_frame_dirty = 1;
    frontend_vnc_do_full_frame_refresh = 1;
    free(old_frontbuffer);
    free(old_backbuffer);
    free(old_prevbuffer);
    return 0;
}

static int frontend_vnc_init(const char* title)
{
    int argc = 1;
    char* argv[2] = { "PCulator", NULL };
    static char cursor_bits[] = " ";
    static char mask_bits[] = " ";
    in_addr_t listen_interface;

    if (!rfbStringToAddr(frontend_vnc_bind_address, &listen_interface)) {
        debug_log(DEBUG_ERROR, "[VNC] Invalid VNC bind address: %s\r\n", frontend_vnc_bind_address);
        return -1;
    }

    frontend_vnc_set_title_internal(title);
    frontend_vnc_active_client = NULL;
    frontend_vnc_release_input_state();
    memset(frontend_vnc_held_keys, 0, sizeof(frontend_vnc_held_keys));

    frontend_vnc_screen = rfbGetScreen(&argc, argv,
        FRONTEND_VNC_DEFAULT_WIDTH, FRONTEND_VNC_DEFAULT_HEIGHT,
        8, 3, 4);
    if (frontend_vnc_screen == NULL) {
        return -1;
    }

    if (frontend_vnc_allocate_buffers(FRONTEND_VNC_DEFAULT_WIDTH, FRONTEND_VNC_DEFAULT_HEIGHT,
        &frontend_vnc_frontbuffer, &frontend_vnc_backbuffer, &frontend_vnc_previousbuffer) != 0) {
        rfbScreenCleanup(frontend_vnc_screen);
        frontend_vnc_screen = NULL;
        return -1;
    }

    frontend_vnc_width = FRONTEND_VNC_DEFAULT_WIDTH;
    frontend_vnc_height = FRONTEND_VNC_DEFAULT_HEIGHT;
    frontend_vnc_screen->desktopName = frontend_vnc_title;
    frontend_vnc_screen->frameBuffer = (char*)frontend_vnc_frontbuffer;
    frontend_vnc_screen->autoPort = 0;
    frontend_vnc_screen->port = frontend_vnc_port;
    frontend_vnc_screen->listenInterface = listen_interface;
    frontend_vnc_screen->alwaysShared = 1;
    frontend_vnc_screen->dontDisconnect = 1;
    frontend_vnc_screen->kbdAddEvent = frontend_vnc_handle_key_event;
    frontend_vnc_screen->ptrAddEvent = frontend_vnc_handle_pointer_event;
    frontend_vnc_screen->newClientHook = frontend_vnc_new_client;
    if (frontend_vnc_password[0] != 0) {
        frontend_vnc_screen->passwordCheck = rfbCheckPasswordByList;
        frontend_vnc_screen->authPasswdData = (void*)frontend_vnc_password_list;
    }
    else {
        frontend_vnc_screen->passwordCheck = NULL;
        frontend_vnc_screen->authPasswdData = NULL;
    }
    frontend_vnc_configure_pixel_format();

    //we don't libvncserver to draw any visible extra cursor on top of the framebuffer
    frontend_vnc_invisible_cursor = rfbMakeXCursor(1, 1, cursor_bits, mask_bits);
    frontend_vnc_screen->cursor = frontend_vnc_invisible_cursor;

    rfbInitServer(frontend_vnc_screen);
    if (!rfbIsActive(frontend_vnc_screen)) {
        frontend_vnc_destroy_server();
        return -1;
    }

    if (frontend_vnc_wait_for_client) {
        debug_log(DEBUG_INFO, "[VNC] Waiting for client connection to begin emulation...\r\n");
        while (running && (frontend_vnc_active_client == NULL) && rfbIsActive(frontend_vnc_screen)) {
            rfbProcessEvents(frontend_vnc_screen, 10000);
        }
        if (frontend_vnc_active_client == NULL) {
            frontend_vnc_destroy_server();
            return -1;
        }
    }

    return 0;
}

static void frontend_vnc_shutdown(void)
{
    frontend_vnc_destroy_server();
}

static int frontend_vnc_bind_machine(MACHINE_t* machine)
{
    (void)machine;
    return 0;
}

static int frontend_vnc_pump_events(void)
{
    if (frontend_vnc_screen == NULL) {
        return FRONTEND_PUMP_NONE;
    }

    if (!rfbIsActive(frontend_vnc_screen)) {
        return FRONTEND_PUMP_QUIT;
    }

    frontend_vnc_is_pumping = 1;

    if (frontend_vnc_frame_dirty && (frontend_vnc_width > 0) && (frontend_vnc_height > 0)) {
        frontend_vnc_frame_dirty = 0;
        if (frontend_vnc_do_full_frame_refresh) {
            frontend_vnc_do_full_frame_refresh = 0;
            rfbMarkRectAsModified(frontend_vnc_screen, 0, 0, frontend_vnc_width, frontend_vnc_height);
        }
        else {
            uint32_t startx, starty, rectw, recth;
            for (starty = 0; starty < frontend_vnc_height; starty += 16) {
                for (startx = 0; startx < frontend_vnc_width; startx += 16) {
                    uint32_t row;
                    rectw = frontend_vnc_width - startx;
                    rectw = (rectw >= 16) ? 16 : rectw;
                    recth = frontend_vnc_height - starty;
                    recth = (recth >= 16) ? 16 : recth;
                    for (row = 0; row < recth; row++) {
                        uint32_t rowoffset = (starty + row) * frontend_vnc_width + startx;
                        if (memcmp((void*)(frontend_vnc_frontbuffer + rowoffset), (void*)(frontend_vnc_previousbuffer + rowoffset), (size_t)(rectw * sizeof(uint32_t)))) {
                            rfbMarkRectAsModified(frontend_vnc_screen, (int)startx, (int)starty, (int)(startx + rectw), (int)(starty + recth));
                            break;
                        }
                    }
                }
            }
        }

        if ((frontend_vnc_width == frontend_vnc_previous_width) && (frontend_vnc_height == frontend_vnc_previous_height)) {
            memcpy((void*)frontend_vnc_previousbuffer, (void*)frontend_vnc_frontbuffer, (size_t)(frontend_vnc_width * frontend_vnc_height * sizeof(uint32_t)));
        }
    }

    rfbProcessEvents(frontend_vnc_screen, 0);
    frontend_vnc_is_pumping = 0;
    return rfbIsActive(frontend_vnc_screen) ? FRONTEND_PUMP_NONE : FRONTEND_PUMP_QUIT;
}

static void frontend_vnc_video_present(uint32_t* framebuffer, int width, int height, int stride_bytes)
{
    uint8_t* src;
    uint32_t* src_row;
    int y;
    int x;
    size_t frame_bytes;

    if ((frontend_vnc_screen == NULL) || (framebuffer == NULL) || (width <= 0) || (height <= 0)) {
        return;
    }

    if ((width != frontend_vnc_width) || (height != frontend_vnc_height)) {
        if (frontend_vnc_replace_framebuffer(width, height) != 0) {
            return;
        }
    }
    if ((frontend_vnc_frontbuffer == NULL) || (frontend_vnc_backbuffer == NULL)) {
        return;
    }

    src = (uint8_t*)framebuffer;

    for (y = 0; y < height; y++) {
        uint32_t* dst_row = frontend_vnc_backbuffer + ((size_t)y * (size_t)width);
        src_row = (uint32_t*)(src + ((size_t)y * (size_t)stride_bytes));

        for (x = 0; x < width; x++) {
            uint32_t pixel = src_row[x];
            dst_row[x] = ((pixel & 0x00FF0000u) >> 16) |
                (pixel & 0x0000FF00u) |
                ((pixel & 0x000000FFu) << 16);
        }
    }

    frame_bytes = (size_t)width * (size_t)height * sizeof(uint32_t);
    memcpy(frontend_vnc_frontbuffer, frontend_vnc_backbuffer, frame_bytes);
    frontend_vnc_frame_dirty = 1;
}

static int frontend_vnc_video_can_accept_present(void)
{
    int can_present = 1;
    if (frontend_vnc_active_client == NULL) can_present = 0;
    if (frontend_vnc_is_pumping) can_present = 0;
    return can_present;
}

static void frontend_vnc_video_resize(int width, int height)
{
    if ((width <= 0) || (height <= 0) ||
        ((width == frontend_vnc_width) && (height == frontend_vnc_height))) {
        return;
    }

    frontend_vnc_replace_framebuffer(width, height);
}

static void frontend_vnc_set_window_title(const char* title)
{
    frontend_vnc_set_title_internal(title);
}

static double frontend_vnc_get_video_fps(void)
{
    return 0.0;
}

static int frontend_vnc_is_mouse_grabbed(void)
{
    return (frontend_vnc_active_client != NULL) ? 1 : 0;
}

static void frontend_vnc_set_mouse_grab(int grabbed)
{
    (void)grabbed;
}

static int frontend_vnc_audio_init(const FRONTEND_AUDIO_FORMAT_t* format, FRONTEND_AUDIO_PULL_t pull, void* udata)
{
    (void)format;
    (void)pull;
    (void)udata;
    return -1;
}

static void frontend_vnc_audio_shutdown(void)
{
}

static void frontend_vnc_audio_set_paused(int paused)
{
    (void)paused;
}

int frontend_vnc_set_bind_address(const char* addr)
{
    if ((addr == NULL) || (strlen(addr) >= sizeof(frontend_vnc_bind_address))) {
        return -1;
    }

    strcpy(frontend_vnc_bind_address, addr);
    return 0;
}

int frontend_vnc_set_port(uint16_t port)
{
    frontend_vnc_port = port;
    return 0;
}

int frontend_vnc_set_password(const char* password)
{
    if ((password == NULL) || (strlen(password) >= sizeof(frontend_vnc_password))) {
        return -1;
    }

    strcpy(frontend_vnc_password, password);
    return 0;
}

void frontend_vnc_set_wait_for_client(int wait_for_client)
{
    frontend_vnc_wait_for_client = wait_for_client ? 1 : 0;
}

void frontend_vnc_show_help(void)
{
    printf("VNC frontend options:\r\n");
    printf("  -vnc-bind <addr>       Listen for VNC clients on <addr>. (Default is 0.0.0.0)\r\n");
    printf("  -vnc-port <port>       Listen for VNC clients on TCP <port>. (Default is 5900)\r\n");
    printf("  -vnc-password <text>   Require <text> as the VNC password. (Optional)\r\n");
    printf("  -vnc-wait-for-client   Wait for the first VNC client connection before starting emulation.\r\n\r\n");
}

FRONTEND_ARG_RESULT_t frontend_vnc_try_parse_arg(int argc, char* argv[], int* index)
{
    char* current_arg;

    if ((argv == NULL) || (index == NULL) || (*index < 0) || (*index >= argc)) {
        return FRONTEND_ARG_UNHANDLED;
    }

    current_arg = argv[*index];
    if (frontend_is_arg_match(current_arg, "-vnc-bind")) {
        if ((*index + 1) == argc) {
            printf("Parameter required for -vnc-bind. Use -h for help.\r\n");
            return FRONTEND_ARG_ERROR;
        }
        (*index)++;
        if (frontend_vnc_set_bind_address(argv[*index]) != 0) {
            printf("%s is an invalid VNC bind address.\r\n", argv[*index]);
            return FRONTEND_ARG_ERROR;
        }
        return FRONTEND_ARG_HANDLED;
    }

    if (frontend_is_arg_match(current_arg, "-vnc-port")) {
        char* end;
        unsigned long port_value;

        if ((*index + 1) == argc) {
            printf("Parameter required for -vnc-port. Use -h for help.\r\n");
            return FRONTEND_ARG_ERROR;
        }

        (*index)++;
        port_value = strtoul(argv[*index], &end, 0);
        if ((end == argv[*index]) || (*end != 0) || (port_value == 0) || (port_value > 65535)) {
            printf("%s is an invalid VNC port.\r\n", argv[*index]);
            return FRONTEND_ARG_ERROR;
        }

        frontend_vnc_set_port((uint16_t)port_value);
        return FRONTEND_ARG_HANDLED;
    }

    if (frontend_is_arg_match(current_arg, "-vnc-password")) {
        if ((*index + 1) == argc) {
            printf("Parameter required for -vnc-password. Use -h for help.\r\n");
            return FRONTEND_ARG_ERROR;
        }

        (*index)++;
        if (frontend_vnc_set_password(argv[*index]) != 0) {
            printf("Invalid VNC password value.\r\n");
            return FRONTEND_ARG_ERROR;
        }
        return FRONTEND_ARG_HANDLED;
    }

    if (frontend_is_arg_match(current_arg, "-vnc-wait-for-client")) {
        frontend_vnc_set_wait_for_client(1);
        return FRONTEND_ARG_HANDLED;
    }

    return FRONTEND_ARG_UNHANDLED;
}

int frontend_vnc_register(void)
{
    static const FRONTEND_OPS_t frontend_vnc_ops = {
        "vnc",
        frontend_vnc_init,
        frontend_vnc_shutdown,
        frontend_vnc_bind_machine,
        frontend_vnc_pump_events,
        frontend_vnc_video_present,
        frontend_vnc_video_can_accept_present,
        frontend_vnc_video_resize,
        frontend_vnc_set_window_title,
        frontend_vnc_get_video_fps,
        frontend_vnc_is_mouse_grabbed,
        frontend_vnc_set_mouse_grab,
        frontend_vnc_audio_init,
        frontend_vnc_audio_shutdown,
        frontend_vnc_audio_set_paused
    };

    return frontend_register_ops(&frontend_vnc_ops);
}
