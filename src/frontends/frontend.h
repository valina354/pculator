#ifndef _FRONTEND_H_
#define _FRONTEND_H_

#include <stdint.h>
#include "../machine.h"
#include "../input/hostkey.h"

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint16_t buffer_samples;
} FRONTEND_AUDIO_FORMAT_t;

typedef void (*FRONTEND_AUDIO_PULL_t)(int16_t* stream, int samples, void* udata);

typedef struct {
    void (*handle_key)(void* udata, HOST_KEY_SCANCODE_t scan, uint8_t down);
    void* udata;
} FRONTEND_KEYBOARD_SINK_t;

typedef struct {
    const char* name;
    int (*init)(const char* title);
    void (*shutdown)(void);
    int (*bind_machine)(MACHINE_t* machine);
    int (*pump_events)(void);
    void (*video_present)(uint32_t* framebuffer, int width, int height, int stride_bytes);
    int (*video_can_accept_present)(void);
    void (*video_resize)(int width, int height);
    void (*set_window_title)(const char* title);
    double (*get_video_fps)(void);
    int (*is_mouse_grabbed)(void);
    void (*set_mouse_grab)(int grabbed);
    int (*audio_init)(const FRONTEND_AUDIO_FORMAT_t* format, FRONTEND_AUDIO_PULL_t pull, void* udata);
    void (*audio_shutdown)(void);
    void (*audio_set_paused)(int paused);
} FRONTEND_OPS_t;

typedef enum {
    FRONTEND_ARG_UNHANDLED = 0,
    FRONTEND_ARG_HANDLED = 1,
    FRONTEND_ARG_EXIT = 2,
    FRONTEND_ARG_ERROR = 3
} FRONTEND_ARG_RESULT_t;

enum {
    FRONTEND_PUMP_NONE = 0,
    FRONTEND_PUMP_QUIT = 1
};

int frontend_register_ops(const FRONTEND_OPS_t* ops);
const char* frontend_get_default_name(void);
int frontend_select_by_name(const char* name);
void frontend_print_available(void);
void frontend_show_help(void);
FRONTEND_ARG_RESULT_t frontend_try_parse_arg(int argc, char* argv[], int* index);
int frontend_is_arg_match(const char* actual, const char* expected);

int frontend_init(const char* title);
void frontend_shutdown(void);
int frontend_bind_machine(MACHINE_t* machine);
int frontend_pump_events(void);
void frontend_video_present(uint32_t* framebuffer, int width, int height, int stride_bytes);
int frontend_video_can_accept_present(void);
void frontend_video_resize(int width, int height);
void frontend_set_window_title(const char* title);
double frontend_get_video_fps(void);
int frontend_is_mouse_grabbed(void);
void frontend_set_mouse_grab(int grabbed);

void frontend_register_keyboard_sink(const FRONTEND_KEYBOARD_SINK_t* sink);
void frontend_unregister_keyboard_sink(void);
void frontend_inject_key(HOST_KEY_SCANCODE_t scan, uint8_t down);
void frontend_inject_mouse_move(int32_t xrel, int32_t yrel);
void frontend_inject_mouse_button(uint8_t action, uint8_t state);
void frontend_inject_ctrl_alt_del(void);

int frontend_audio_init_sink(const FRONTEND_AUDIO_FORMAT_t* format, FRONTEND_AUDIO_PULL_t pull, void* udata);
void frontend_audio_shutdown_sink(void);
void frontend_audio_set_paused(int paused);

#endif
