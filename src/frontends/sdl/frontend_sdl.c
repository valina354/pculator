/*
  PCulator: A portable, open-source x86 PC emulator.
  Copyright (C)2025 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "../../config.h"

#ifdef _WIN32
#include <SDL/SDL.h>
#else
#include <SDL.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "frontend_sdl.h"
#include "frontend_sdl_ui.h"
#include "hostkey_sdl.h"
#include "../frontend.h"
#include "../../input/mouse.h"
#include "../../timing.h"

static SDL_Window* frontend_sdl_window = NULL;
static SDL_Renderer* frontend_sdl_renderer = NULL;
static SDL_Texture* frontend_sdl_texture = NULL;
static uint64_t frontend_sdl_frameTime[30];
static uint32_t frontend_sdl_keyTimer = TIMING_ERROR;
static HOST_KEY_SCANCODE_t frontend_sdl_lastRepeatScan = 0;
static uint8_t frontend_sdl_frameIdx = 0;
static uint8_t frontend_sdl_grabbed = 0;
static uint8_t frontend_sdl_doRepeat = 0;
static uint8_t frontend_sdl_swallowF10Break = 0;
static uint8_t frontend_sdl_swallowF11Break = 0;
static uint8_t frontend_sdl_swallowF12Break = 0;
static int frontend_sdl_fixedw = 0;
static int frontend_sdl_fixedh = 0;
static int frontend_sdl_curw = 0;
static int frontend_sdl_curh = 0;
static double frontend_sdl_fps = 0.0;
static char frontend_sdl_base_title[256] = "";
static FRONTEND_AUDIO_PULL_t frontend_sdl_audio_pull = NULL;
static void* frontend_sdl_audio_udata = NULL;
static uint8_t frontend_sdl_audio_active = 0;

static void frontend_sdl_keyRepeat(void* dummy)
{
    (void)dummy;
    frontend_sdl_doRepeat = 1;
    timing_updateIntervalFreq(frontend_sdl_keyTimer, 15);
}

static int frontend_sdl_get_window_id(void)
{
    if (frontend_sdl_window == NULL) {
        return 0;
    }

    return (int)SDL_GetWindowID(frontend_sdl_window);
}

static void frontend_sdl_refresh_window_title(void)
{
    char title[320];

    if (frontend_sdl_window == NULL) {
        return;
    }

    /*if (frontend_sdl_fps > 0.0) {
        if (frontend_sdl_grabbed) {
            snprintf(title, sizeof(title), "%s - %.2f FPS [Right Ctrl-F11 to release mouse]",
                frontend_sdl_base_title,
                frontend_sdl_fps);
        }
        else {
            snprintf(title, sizeof(title), "%s - %.2f FPS",
                frontend_sdl_base_title,
                frontend_sdl_fps);
        }
    }
    else {*/
        if (frontend_sdl_grabbed) {
            snprintf(title, sizeof(title), "%s - [Right Ctrl + F11 to release mouse]",
                frontend_sdl_base_title,
                frontend_sdl_fps);
        }
        else {
            snprintf(title, sizeof(title), "%s",
                frontend_sdl_base_title,
                frontend_sdl_fps);
        }
    //}

    SDL_SetWindowTitle(frontend_sdl_window, title);
}

static void frontend_sdl_set_mouse_grab_internal(int grabbed)
{
    if (frontend_sdl_window == NULL) {
        frontend_sdl_grabbed = 0;
        return;
    }

    if (grabbed) {
        SDL_SetRelativeMouseMode(SDL_TRUE);
        frontend_sdl_grabbed = 1;
    }
    else {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        frontend_sdl_grabbed = 0;
    }

    frontend_sdl_refresh_window_title();
}

static void frontend_sdl_audio_callback(void* udata, Uint8* stream, int len)
{
    (void)udata;

    if ((frontend_sdl_audio_pull == NULL) || (stream == NULL) || (len <= 0)) {
        if ((stream != NULL) && (len > 0)) {
            memset(stream, 0, (size_t)len);
        }
        return;
    }

    frontend_sdl_audio_pull((int16_t*)stream, len / (int)sizeof(int16_t), frontend_sdl_audio_udata);
}

static int frontend_sdl_set_window(int w, int h)
{
    static int isFirstSet = 1;

    if (frontend_sdl_window == NULL) {
        return -1;
    }
    
    if (frontend_sdl_renderer != NULL) {
        SDL_DestroyRenderer(frontend_sdl_renderer);
    }
    if (frontend_sdl_texture != NULL) {
        SDL_DestroyTexture(frontend_sdl_texture);
    }
    frontend_sdl_renderer = NULL;
    frontend_sdl_texture = NULL;

    if (frontend_sdl_fixedw && frontend_sdl_fixedh && isFirstSet) {
        SDL_SetWindowSize(frontend_sdl_window, frontend_sdl_fixedw, frontend_sdl_fixedh);
    }
    else if (frontend_sdl_fixedw == 0 || frontend_sdl_fixedh == 0) {
        SDL_SetWindowSize(frontend_sdl_window, w, h);
    }

    frontend_sdl_renderer = SDL_CreateRenderer(frontend_sdl_window, -1, 0);
    if (frontend_sdl_renderer == NULL) {
        return -1;
    }

    frontend_sdl_texture = SDL_CreateTexture(frontend_sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        w, h);
    if (frontend_sdl_texture == NULL) {
        return -1;
    }

    if (frontend_sdl_fixedw && frontend_sdl_fixedh) { //use bilinear scaling if we're using a fixed side window
        SDL_SetTextureScaleMode(frontend_sdl_texture, SDL_ScaleModeLinear);
    }

    frontend_sdl_curw = w;
    frontend_sdl_curh = h;

    isFirstSet = 0;

    return 0;
}

static int frontend_sdl_init(const char* title)
{
    int usew = 640, useh = 400;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return -1;
    }

#ifdef _WIN32
    SDL_SetHint("SDL_WINDOWS_NO_CLOSE_ON_ALT_F4", "1");
#endif

    if (title == NULL) {
        frontend_sdl_base_title[0] = 0;
    }
    else {
        strncpy(frontend_sdl_base_title, title, sizeof(frontend_sdl_base_title));
        frontend_sdl_base_title[sizeof(frontend_sdl_base_title) - 1] = 0;
    }

    if (frontend_sdl_fixedw && frontend_sdl_fixedh) {
        usew = frontend_sdl_fixedw;
        useh = frontend_sdl_fixedh;
    }

    frontend_sdl_window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        usew, useh,
        SDL_WINDOW_OPENGL);
    if (frontend_sdl_window == NULL) {
        //Attempt to open using OpenGL failed, so try again without GL as a fallback
        frontend_sdl_window = SDL_CreateWindow(title,
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            usew, useh, 0);
    }
    if (frontend_sdl_window == NULL) {
        SDL_Quit();
        return -1;
    }

    if (frontend_sdl_set_window(usew, useh) != 0) {
        SDL_DestroyWindow(frontend_sdl_window);
        frontend_sdl_window = NULL;
        SDL_Quit();
        return -1;
    }

    memset(frontend_sdl_frameTime, 0, sizeof(frontend_sdl_frameTime));
    frontend_sdl_lastRepeatScan = 0;
    frontend_sdl_frameIdx = 0;
    frontend_sdl_grabbed = 0;
    frontend_sdl_doRepeat = 0;
    frontend_sdl_swallowF10Break = 0;
    frontend_sdl_swallowF11Break = 0;
    frontend_sdl_swallowF12Break = 0;
    frontend_sdl_fps = 0.0;
    frontend_sdl_keyTimer = timing_addTimer(frontend_sdl_keyRepeat, NULL, 2, TIMING_DISABLED);
    frontend_sdl_refresh_window_title();
    return 0;
}

static void frontend_sdl_shutdown(void)
{
    frontend_sdl_ui_shutdown();

    if (frontend_sdl_texture != NULL) {
        SDL_DestroyTexture(frontend_sdl_texture);
        frontend_sdl_texture = NULL;
    }
    if (frontend_sdl_renderer != NULL) {
        SDL_DestroyRenderer(frontend_sdl_renderer);
        frontend_sdl_renderer = NULL;
    }
    if (frontend_sdl_window != NULL) {
        SDL_DestroyWindow(frontend_sdl_window);
        frontend_sdl_window = NULL;
    }

    frontend_sdl_audio_pull = NULL;
    frontend_sdl_audio_udata = NULL;
    frontend_sdl_audio_active = 0;
    frontend_sdl_grabbed = 0;
    frontend_sdl_base_title[0] = 0;
    SDL_Quit();
}

static int frontend_sdl_bind_machine(MACHINE_t* machine)
{
    return frontend_sdl_ui_init(machine);
}

static void frontend_sdl_update_fps(uint64_t curtime)
{
    static uint64_t lasttime = 0;

    if (lasttime != 0) {
        int i;
        int avgcount = 0;
        uint64_t curavg = 0;

        frontend_sdl_frameTime[frontend_sdl_frameIdx++] = curtime - lasttime;
        if (frontend_sdl_frameIdx == 30) {
            frontend_sdl_frameIdx = 0;
            for (i = 0; i < 30; i++) {
                if (frontend_sdl_frameTime[i] != 0) {
                    curavg += frontend_sdl_frameTime[i];
                    avgcount++;
                }
            }
            if (avgcount != 0) {
                curavg /= (uint64_t)avgcount;
                if (curavg != 0) {
                    frontend_sdl_fps = (double)((timing_getFreq() * 10) / curavg) / 10.0;
                    frontend_sdl_refresh_window_title();
                }
            }
        }
    }

    lasttime = curtime;
}

static void frontend_sdl_video_present(uint32_t* framebuffer, int width, int height, int stride_bytes)
{
    if ((frontend_sdl_window == NULL) || (framebuffer == NULL)) {
        return;
    }

    if ((width != frontend_sdl_curw) || (height != frontend_sdl_curh)) {
        if (frontend_sdl_set_window(width, height) != 0) {
            return;
        }
    }
    if ((frontend_sdl_renderer == NULL) || (frontend_sdl_texture == NULL)) {
        return;
    }

    SDL_UpdateTexture(frontend_sdl_texture, NULL, framebuffer, stride_bytes);
    SDL_RenderClear(frontend_sdl_renderer);
    SDL_RenderCopy(frontend_sdl_renderer, frontend_sdl_texture, NULL, NULL);
    SDL_RenderPresent(frontend_sdl_renderer);
    frontend_sdl_update_fps(timing_getCur());
}

static int frontend_sdl_video_can_accept_present(void)
{
    return 1;
}

static void frontend_sdl_video_resize(int width, int height)
{
    frontend_sdl_set_window(width, height);
}

static void frontend_sdl_set_window_title(const char* title)
{
    if (title == NULL) {
        frontend_sdl_base_title[0] = 0;
    }
    else {
        strncpy(frontend_sdl_base_title, title, sizeof(frontend_sdl_base_title));
        frontend_sdl_base_title[sizeof(frontend_sdl_base_title) - 1] = 0;
    }

    frontend_sdl_refresh_window_title();
}

static double frontend_sdl_get_video_fps(void)
{
    return frontend_sdl_fps;
}

static int frontend_sdl_is_mouse_grabbed(void)
{
    return frontend_sdl_grabbed ? 1 : 0;
}

static void frontend_sdl_set_mouse_grab(int grabbed)
{
    frontend_sdl_set_mouse_grab_internal(grabbed);
}

static int frontend_sdl_pump_events(void)
{
    SDL_Event event;
    HOST_KEY_EVENT_t host_event[HOSTKEY_SDL_MAX_EVENTS];
    int main_window_id = frontend_sdl_get_window_id();

    if (frontend_sdl_doRepeat) {
        frontend_sdl_doRepeat = 0;
        if (frontend_sdl_lastRepeatScan != 0) {
            frontend_inject_key(frontend_sdl_lastRepeatScan, 1);
        }
    }

    if (frontend_sdl_ui_isVisible()) {
        frontend_sdl_ui_beginInput();
    }

    while (SDL_PollEvent(&event)) {
        if ((event.type == SDL_KEYDOWN) && !event.key.repeat) {
            if ((event.key.keysym.sym == SDLK_F10) && (event.key.keysym.mod & KMOD_RCTRL) && frontend_sdl_ui_isAvailable()) {
                frontend_sdl_swallowF10Break = 1;
                frontend_sdl_doRepeat = 0;
                timing_timerDisable(frontend_sdl_keyTimer);
                if (frontend_sdl_grabbed) {
                    frontend_sdl_set_mouse_grab_internal(0);
                }
                frontend_sdl_ui_toggle();
                continue;
            }
            if ((event.key.keysym.sym == SDLK_F11) && (event.key.keysym.mod & KMOD_RCTRL)) {
                frontend_sdl_swallowF11Break = 1;
                frontend_sdl_set_mouse_grab_internal(frontend_sdl_grabbed ? 0 : 1);
                continue;
            }
            if ((event.key.keysym.sym == SDLK_F12) && (event.key.keysym.mod & KMOD_RCTRL)) {
                frontend_sdl_swallowF12Break = 1;
                frontend_inject_ctrl_alt_del();
                continue;
            }
        }
        if (event.type == SDL_KEYUP) {
            if (frontend_sdl_swallowF10Break && (event.key.keysym.sym == SDLK_F10)) {
                frontend_sdl_swallowF10Break = 0;
                continue;
            }
            if (frontend_sdl_swallowF11Break && (event.key.keysym.sym == SDLK_F11)) {
                frontend_sdl_swallowF11Break = 0;
                continue;
            }
            if (frontend_sdl_swallowF12Break && (event.key.keysym.sym == SDLK_F12)) {
                frontend_sdl_swallowF12Break = 0;
                continue;
            }
        }
        if (frontend_sdl_ui_handleEvent(&event)) {
            continue;
        }

        switch (event.type) {
        case SDL_KEYDOWN:
            if (event.key.repeat || ((int)event.key.windowID != main_window_id)) {
                continue;
            }
            if (hostkey_sdl_translate(&event.key, host_event) != 0) {
                frontend_inject_key(host_event[0].scan, host_event[0].down);
                if ((host_event[0].scan != HOST_KEY_SCAN_PRINTSCREEN) &&
                    (host_event[0].scan != HOST_KEY_SCAN_PAUSE)) {
                    frontend_sdl_lastRepeatScan = host_event[0].scan;
                    timing_updateIntervalFreq(frontend_sdl_keyTimer, 2);
                    timing_timerEnable(frontend_sdl_keyTimer);
                }
            }
            continue;
        case SDL_KEYUP:
            if (event.key.repeat || ((int)event.key.windowID != main_window_id)) {
                continue;
            }
            if (hostkey_sdl_translate(&event.key, host_event) != 0) {
                frontend_inject_key(host_event[0].scan, host_event[0].down);
                if (host_event[0].scan == frontend_sdl_lastRepeatScan) {
                    timing_timerDisable(frontend_sdl_keyTimer);
                    frontend_sdl_lastRepeatScan = 0;
                }
            }
            continue;
        case SDL_MOUSEMOTION:
        {
            int xrel;
            int yrel;

            if (((int)event.motion.windowID != main_window_id) || !frontend_sdl_grabbed) {
                continue;
            }
            xrel = event.motion.xrel;
            yrel = event.motion.yrel;
            if (xrel < -128) xrel = -128;
            if (xrel > 127) xrel = 127;
            if (yrel < -128) yrel = -128;
            if (yrel > 127) yrel = 127;
            frontend_inject_mouse_move((int8_t)xrel, (int8_t)yrel);
            continue;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if ((int)event.button.windowID != main_window_id) {
                continue;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                if (!frontend_sdl_grabbed) {
                    frontend_sdl_set_mouse_grab_internal(1);
                    break;
                }
                frontend_inject_mouse_button(MOUSE_ACTION_LEFT,
                    (event.button.state == SDL_PRESSED) ? MOUSE_PRESSED : MOUSE_UNPRESSED);
            }
            else if (event.button.button == SDL_BUTTON_RIGHT) {
                if (frontend_sdl_grabbed) {
                    frontend_inject_mouse_button(MOUSE_ACTION_RIGHT,
                        (event.button.state == SDL_PRESSED) ? MOUSE_PRESSED : MOUSE_UNPRESSED);
                }
            }
            continue;
        case SDL_WINDOWEVENT:
            if ((int)event.window.windowID == main_window_id) {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    return FRONTEND_PUMP_QUIT;
                }
                else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    SDL_SetWindowKeyboardGrab(frontend_sdl_window, SDL_TRUE);
                }
                else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    SDL_SetWindowKeyboardGrab(frontend_sdl_window, SDL_FALSE);
                }
            }
            continue;
        case SDL_QUIT:
            return FRONTEND_PUMP_QUIT;
        default:
            break;
        }
    }

    if (frontend_sdl_ui_isVisible()) {
        frontend_sdl_ui_update();
    }

    return FRONTEND_PUMP_NONE;
}

static int frontend_sdl_audio_init(const FRONTEND_AUDIO_FORMAT_t* format, FRONTEND_AUDIO_PULL_t pull, void* udata)
{
    SDL_AudioSpec wanted;

    if ((format == NULL) || (pull == NULL)) {
        return -1;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        return -1;
    }

    memset(&wanted, 0, sizeof(wanted));
    wanted.freq = (int)format->sample_rate;
    wanted.format = AUDIO_S16;
    wanted.channels = format->channels;
    wanted.samples = format->buffer_samples;
    wanted.callback = frontend_sdl_audio_callback;
    wanted.userdata = NULL;

    frontend_sdl_audio_pull = pull;
    frontend_sdl_audio_udata = udata;
    if (SDL_OpenAudio(&wanted, NULL) < 0) {
        frontend_sdl_audio_pull = NULL;
        frontend_sdl_audio_udata = NULL;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }

    frontend_sdl_audio_active = 1;
    return 0;
}

static void frontend_sdl_audio_shutdown(void)
{
    if (frontend_sdl_audio_active) {
        SDL_CloseAudio();
        frontend_sdl_audio_active = 0;
    }
    if (SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    frontend_sdl_audio_pull = NULL;
    frontend_sdl_audio_udata = NULL;
}

static void frontend_sdl_audio_set_paused(int paused)
{
    if (!frontend_sdl_audio_active) {
        return;
    }

    SDL_PauseAudio(paused ? 1 : 0);
}

void frontend_sdl_show_help(void)
{
    printf("SDL frontend options:\r\n");
    printf("  -sdl-fixed <res>       Use a fixed SDL window resolution (for example 1024x768)\r\n\r\n");
}

FRONTEND_ARG_RESULT_t frontend_sdl_try_parse_arg(int argc, char* argv[], int* index)
{
    char* current_arg;

    if ((argv == NULL) || (index == NULL) || (*index < 0) || (*index >= argc)) {
        return FRONTEND_ARG_UNHANDLED;
    }

    current_arg = argv[*index];
    if (frontend_is_arg_match(current_arg, "-sdl-fixed")) {
        int w = 0, h = 0, i, arglen, xPos = 0;
        if ((*index + 1) == argc) {
            printf("Parameter required for -sdl-fixed. Use -h for help.\r\n");
            return FRONTEND_ARG_ERROR;
        }
        (*index)++;
        arglen = strlen(argv[*index]);
        for (i = 0; i < arglen; i++) {
            if (argv[*index][i] == 'x' || argv[*index][i] == 'X') {
                xPos = i;
                break;
            }
        }
        if (i == arglen || xPos == 0) {
            printf("%s is an invalid resolution for -sdl-fixed. Use WxH format, for example 1024x768.\r\n", argv[*index]);
            return FRONTEND_ARG_ERROR;
        }
        argv[*index][xPos] = 0;
        w = atoi(argv[*index]);
        h = atoi(&argv[*index][xPos + 1]);
        //printf("DEBUG: SDL resolution arg parsed as %dx%d\r\n", w, h);
        if ((w < 320) || (h < 200)) {
            printf("Width must be >= 320 and height must be >= 200 for -sdl-fixed.\r\n");
            return FRONTEND_ARG_ERROR;
        }
        frontend_sdl_fixedw = w;
        frontend_sdl_fixedh = h;
        return FRONTEND_ARG_HANDLED;
    }

    return FRONTEND_ARG_UNHANDLED;
}

int frontend_sdl_register(void)
{
    static const FRONTEND_OPS_t frontend_sdl_ops = {
        "sdl",
        frontend_sdl_init,
        frontend_sdl_shutdown,
        frontend_sdl_bind_machine,
        frontend_sdl_pump_events,
        frontend_sdl_video_present,
        frontend_sdl_video_can_accept_present,
        frontend_sdl_video_resize,
        frontend_sdl_set_window_title,
        frontend_sdl_get_video_fps,
        frontend_sdl_is_mouse_grabbed,
        frontend_sdl_set_mouse_grab,
        frontend_sdl_audio_init,
        frontend_sdl_audio_shutdown,
        frontend_sdl_audio_set_paused
    };

    return frontend_register_ops(&frontend_sdl_ops);
}
