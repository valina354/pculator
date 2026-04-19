#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "frontend.h"
#include "../input/mouse.h"

typedef struct {
    const char* name;
    int (*register_fn)(void);
    void (*show_help_fn)(void);
    FRONTEND_ARG_RESULT_t (*parse_arg_fn)(int argc, char* argv[], int* index);
} FRONTEND_DESCRIPTOR_t;

static const FRONTEND_OPS_t* frontend_ops = NULL;
static FRONTEND_KEYBOARD_SINK_t frontend_keyboard_sink;
static const FRONTEND_DESCRIPTOR_t* frontend_requested_descriptor = NULL;
static const FRONTEND_DESCRIPTOR_t* frontend_active_descriptor = NULL;

#if defined(FRONTEND_HAS_SDL)
#include "sdl/frontend_sdl.h"
#endif
#if defined(FRONTEND_HAS_VNC)
#include "vnc/frontend_vnc.h"
#endif
#if defined(FRONTEND_HAS_NULL)
#include "null/frontend_null.h"
#endif

#if defined(FRONTEND_HAS_SDL)
#define FRONTEND_HAS_SDL_COUNT 1
#else
#define FRONTEND_HAS_SDL_COUNT 0
#endif

#if defined(FRONTEND_HAS_VNC)
#define FRONTEND_HAS_VNC_COUNT 1
#else
#define FRONTEND_HAS_VNC_COUNT 0
#endif

#if defined(FRONTEND_HAS_NULL)
#define FRONTEND_HAS_NULL_COUNT 1
#else
#define FRONTEND_HAS_NULL_COUNT 0
#endif

#if defined(FRONTEND_DEFAULT_SDL)
#define FRONTEND_DEFAULT_SDL_COUNT 1
#else
#define FRONTEND_DEFAULT_SDL_COUNT 0
#endif

#if defined(FRONTEND_DEFAULT_VNC)
#define FRONTEND_DEFAULT_VNC_COUNT 1
#else
#define FRONTEND_DEFAULT_VNC_COUNT 0
#endif

#if defined(FRONTEND_DEFAULT_NULL)
#define FRONTEND_DEFAULT_NULL_COUNT 1
#else
#define FRONTEND_DEFAULT_NULL_COUNT 0
#endif

#if (FRONTEND_HAS_SDL_COUNT + FRONTEND_HAS_VNC_COUNT + FRONTEND_HAS_NULL_COUNT) == 0
#error At least one PCulator frontend backend must be compiled into the build.
#endif

#if (FRONTEND_DEFAULT_SDL_COUNT + FRONTEND_DEFAULT_VNC_COUNT + FRONTEND_DEFAULT_NULL_COUNT) != 1
#error Exactly one PCulator default frontend must be selected at build time.
#endif

#if defined(FRONTEND_DEFAULT_SDL) && !defined(FRONTEND_HAS_SDL)
#error The SDL frontend cannot be the default unless it is compiled in.
#endif

#if defined(FRONTEND_DEFAULT_VNC) && !defined(FRONTEND_HAS_VNC)
#error The VNC frontend cannot be the default unless it is compiled in.
#endif

#if defined(FRONTEND_DEFAULT_NULL) && !defined(FRONTEND_HAS_NULL)
#error The null frontend cannot be the default unless it is compiled in.
#endif

static const FRONTEND_DESCRIPTOR_t frontend_descriptors[] = {
#if defined(FRONTEND_HAS_SDL)
    { "sdl", frontend_sdl_register, frontend_sdl_show_help, frontend_sdl_try_parse_arg },
#endif
#if defined(FRONTEND_HAS_VNC)
    { "vnc", frontend_vnc_register, frontend_vnc_show_help, frontend_vnc_try_parse_arg },
#endif
#if defined(FRONTEND_HAS_NULL)
    { "null", frontend_null_register, NULL, NULL },
#endif
};

static const size_t frontend_descriptor_count =
    sizeof(frontend_descriptors) / sizeof(frontend_descriptors[0]);

static int frontend_is_name_match(const char* lhs, const char* rhs)
{
    size_t i = 0;

    if ((lhs == NULL) || (rhs == NULL)) {
        return 0;
    }

    while (1) {
        char c1 = lhs[i];
        char c2 = rhs[i];

        if ((c1 >= 'A') && (c1 <= 'Z')) {
            c1 = (char)(c1 - ('A' - 'a'));
        }
        if ((c2 >= 'A') && (c2 <= 'Z')) {
            c2 = (char)(c2 - ('A' - 'a'));
        }

        if (c1 != c2) {
            return 0;
        }
        if ((c1 == 0) && (c2 == 0)) {
            return 1;
        }

        i++;
    }
}

static const FRONTEND_DESCRIPTOR_t* frontend_find_descriptor(const char* name)
{
    size_t i;

    for (i = 0; i < frontend_descriptor_count; i++) {
        if (frontend_is_name_match(frontend_descriptors[i].name, name)) {
            return &frontend_descriptors[i];
        }
    }

    return NULL;
}

static const FRONTEND_DESCRIPTOR_t* frontend_get_default_descriptor(void)
{
#if defined(FRONTEND_DEFAULT_SDL)
    return frontend_find_descriptor("sdl");
#elif defined(FRONTEND_DEFAULT_VNC)
    return frontend_find_descriptor("vnc");
#elif defined(FRONTEND_DEFAULT_NULL)
    return frontend_find_descriptor("null");
#else
    return NULL;
#endif
}

int frontend_is_arg_match(const char* actual, const char* expected)
{
    return (actual != NULL) && (expected != NULL) && (_stricmp(actual, expected) == 0);
}

static int frontend_validate_ops(const FRONTEND_OPS_t* ops)
{
    if (ops == NULL) {
        return -1;
    }

    if ((ops->name == NULL) ||
        (ops->init == NULL) ||
        (ops->shutdown == NULL) ||
        (ops->pump_events == NULL) ||
        (ops->video_present == NULL) ||
        (ops->video_resize == NULL) ||
        (ops->set_window_title == NULL)) {
        return -1;
    }

    return 0;
}

const char* frontend_get_default_name(void)
{
    const FRONTEND_DESCRIPTOR_t* descriptor = frontend_get_default_descriptor();

    return (descriptor != NULL) ? descriptor->name : "";
}

int frontend_select_by_name(const char* name)
{
    const FRONTEND_DESCRIPTOR_t* descriptor = frontend_find_descriptor(name);

    if (descriptor == NULL) {
        return -1;
    }

    frontend_requested_descriptor = descriptor;
    return 0;
}

void frontend_print_available(void)
{
    const FRONTEND_DESCRIPTOR_t* default_descriptor = frontend_get_default_descriptor();
    size_t i;

    printf("Compiled frontends:\r\n");
    for (i = 0; i < frontend_descriptor_count; i++) {
        printf("  %s%s\r\n",
            frontend_descriptors[i].name,
            (&frontend_descriptors[i] == default_descriptor) ? " (default)" : "");
    }
}

void frontend_show_help(void)
{
    size_t i;

    for (i = 0; i < frontend_descriptor_count; i++) {
        if (frontend_descriptors[i].show_help_fn != NULL) {
            frontend_descriptors[i].show_help_fn();
        }
    }
}

FRONTEND_ARG_RESULT_t frontend_try_parse_arg(int argc, char* argv[], int* index)
{
    size_t i;

    for (i = 0; i < frontend_descriptor_count; i++) {
        FRONTEND_ARG_RESULT_t result;

        if (frontend_descriptors[i].parse_arg_fn == NULL) {
            continue;
        }

        result = frontend_descriptors[i].parse_arg_fn(argc, argv, index);
        if (result != FRONTEND_ARG_UNHANDLED) {
            return result;
        }
    }

    return FRONTEND_ARG_UNHANDLED;
}

int frontend_register_ops(const FRONTEND_OPS_t* ops)
{
    if (frontend_validate_ops(ops) != 0) {
        return -1;
    }

    frontend_ops = ops;
    return 0;
}

int frontend_init(const char* title)
{
    memset(&frontend_keyboard_sink, 0, sizeof(frontend_keyboard_sink));
    frontend_ops = NULL;
    frontend_active_descriptor = (frontend_requested_descriptor != NULL) ?
        frontend_requested_descriptor :
        frontend_get_default_descriptor();

    if ((frontend_active_descriptor == NULL) || (frontend_active_descriptor->register_fn == NULL)) {
        return -1;
    }
    if (frontend_active_descriptor->register_fn() != 0) {
        frontend_active_descriptor = NULL;
        return -1;
    }
    if (frontend_ops == NULL) {
        frontend_active_descriptor = NULL;
        return -1;
    }

    return frontend_ops->init(title);
}

void frontend_shutdown(void)
{
    if (frontend_ops != NULL) {
        frontend_ops->shutdown();
    }
    frontend_ops = NULL;
    frontend_active_descriptor = NULL;
    memset(&frontend_keyboard_sink, 0, sizeof(frontend_keyboard_sink));
}

int frontend_bind_machine(MACHINE_t* machine)
{
    if ((frontend_ops == NULL) || (frontend_ops->bind_machine == NULL)) {
        return 0;
    }

    return frontend_ops->bind_machine(machine);
}

int frontend_pump_events(void)
{
    if ((frontend_ops == NULL) || (frontend_ops->pump_events == NULL)) {
        return FRONTEND_PUMP_NONE;
    }

    return frontend_ops->pump_events();
}

void frontend_video_present(uint32_t* framebuffer, int width, int height, int stride_bytes)
{
    if ((frontend_ops == NULL) || (frontend_ops->video_present == NULL)) {
        return;
    }

    frontend_ops->video_present(framebuffer, width, height, stride_bytes);
}

int frontend_video_can_accept_present(void)
{
    if ((frontend_ops == NULL) || (frontend_ops->video_can_accept_present == NULL)) {
        return 0;
    }

    return frontend_ops->video_can_accept_present();
}

void frontend_video_resize(int width, int height)
{
    if ((frontend_ops == NULL) || (frontend_ops->video_resize == NULL)) {
        return;
    }

    frontend_ops->video_resize(width, height);
}

void frontend_set_window_title(const char* title)
{
    if ((frontend_ops == NULL) || (frontend_ops->set_window_title == NULL)) {
        return;
    }

    frontend_ops->set_window_title(title);
}

double frontend_get_video_fps(void)
{
    if ((frontend_ops == NULL) || (frontend_ops->get_video_fps == NULL)) {
        return 0.0;
    }

    return frontend_ops->get_video_fps();
}

int frontend_is_mouse_grabbed(void)
{
    if ((frontend_ops == NULL) || (frontend_ops->is_mouse_grabbed == NULL)) {
        return 0;
    }

    return frontend_ops->is_mouse_grabbed();
}

void frontend_set_mouse_grab(int grabbed)
{
    if ((frontend_ops == NULL) || (frontend_ops->set_mouse_grab == NULL)) {
        return;
    }

    frontend_ops->set_mouse_grab(grabbed ? 1 : 0);
}

void frontend_register_keyboard_sink(const FRONTEND_KEYBOARD_SINK_t* sink)
{
    if (sink == NULL) {
        memset(&frontend_keyboard_sink, 0, sizeof(frontend_keyboard_sink));
        return;
    }

    frontend_keyboard_sink = *sink;
}

void frontend_unregister_keyboard_sink(void)
{
    memset(&frontend_keyboard_sink, 0, sizeof(frontend_keyboard_sink));
}

void frontend_inject_key(HOST_KEY_SCANCODE_t scan, uint8_t down)
{
    if (frontend_keyboard_sink.handle_key != NULL) {
        frontend_keyboard_sink.handle_key(frontend_keyboard_sink.udata, scan, down ? 1 : 0);
    }
}

void frontend_inject_mouse_move(int32_t xrel, int32_t yrel)
{
    mouse_action(MOUSE_ACTION_MOVE, MOUSE_NEITHER, xrel, yrel);
}

void frontend_inject_mouse_button(uint8_t action, uint8_t state)
{
    mouse_action(action, state, 0, 0);
}

void frontend_inject_ctrl_alt_del(void)
{
    frontend_inject_key(HOST_KEY_SCAN_LEFTCTRL, 1);
    frontend_inject_key(HOST_KEY_SCAN_LEFTALT, 1);
    frontend_inject_key(HOST_KEY_SCAN_DELETE, 1);
    frontend_inject_key(HOST_KEY_SCAN_DELETE, 0);
    frontend_inject_key(HOST_KEY_SCAN_LEFTALT, 0);
    frontend_inject_key(HOST_KEY_SCAN_LEFTCTRL, 0);
}

int frontend_audio_init_sink(const FRONTEND_AUDIO_FORMAT_t* format, FRONTEND_AUDIO_PULL_t pull, void* udata)
{
    if ((frontend_ops == NULL) || (frontend_ops->audio_init == NULL)) {
        return -1;
    }

    return frontend_ops->audio_init(format, pull, udata);
}

void frontend_audio_shutdown_sink(void)
{
    if ((frontend_ops != NULL) && (frontend_ops->audio_shutdown != NULL)) {
        frontend_ops->audio_shutdown();
    }
}

void frontend_audio_set_paused(int paused)
{
    if ((frontend_ops != NULL) && (frontend_ops->audio_set_paused != NULL)) {
        frontend_ops->audio_set_paused(paused ? 1 : 0);
    }
}
