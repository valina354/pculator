#include "../frontend.h"

static int frontend_null_init(const char* title)
{
    (void)title;
    return 0;
}

static void frontend_null_shutdown(void)
{
}

static int frontend_null_bind_machine(MACHINE_t* machine)
{
    (void)machine;
    return 0;
}

static int frontend_null_pump_events(void)
{
    return FRONTEND_PUMP_NONE;
}

static void frontend_null_video_present(uint32_t* framebuffer, int width, int height, int stride_bytes)
{
    (void)framebuffer;
    (void)width;
    (void)height;
    (void)stride_bytes;
}

static int frontend_null_video_can_accept_present(void)
{
    return 0;
}

static void frontend_null_video_resize(int width, int height)
{
    (void)width;
    (void)height;
}

static void frontend_null_set_window_title(const char* title)
{
    (void)title;
}

static double frontend_null_get_video_fps(void)
{
    return 0.0;
}

static int frontend_null_is_mouse_grabbed(void)
{
    return 0;
}

static void frontend_null_set_mouse_grab(int grabbed)
{
    (void)grabbed;
}

static int frontend_null_audio_init(const FRONTEND_AUDIO_FORMAT_t* format, FRONTEND_AUDIO_PULL_t pull, void* udata)
{
    (void)format;
    (void)pull;
    (void)udata;
    return -1;
}

static void frontend_null_audio_shutdown(void)
{
}

static void frontend_null_audio_set_paused(int paused)
{
    (void)paused;
}

int frontend_null_register(void)
{
    static const FRONTEND_OPS_t frontend_null_ops = {
        "null",
        frontend_null_init,
        frontend_null_shutdown,
        frontend_null_bind_machine,
        frontend_null_pump_events,
        frontend_null_video_present,
        frontend_null_video_can_accept_present,
        frontend_null_video_resize,
        frontend_null_set_window_title,
        frontend_null_get_video_fps,
        frontend_null_is_mouse_grabbed,
        frontend_null_set_mouse_grab,
        frontend_null_audio_init,
        frontend_null_audio_shutdown,
        frontend_null_audio_set_paused
    };

    return frontend_register_ops(&frontend_null_ops);
}
