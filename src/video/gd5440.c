/*
  PCulator: A portable, open-source x86 PC emulator.
  Copyright (C)2025 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This file contains a self-contained port of the 86Box Cirrus Logic GD5440 PCI
  video card implementation together with the SVGA core it depends on.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdbool.h>
#include <wchar.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "gd5440.h"
#include "../config.h"
#include "../debuglog.h"
#include "../host/host.h"
#include "../host_endian.h"
#include "../memory.h"
#include "../pci.h"
#include "../ports.h"
#include "../timing.h"
#include "../utility.h"
#include "../chipset/dma.h"
#include "../frontends/frontend.h"

extern int render_priority;

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef CLAMP_RANGE
#define CLAMP_RANGE(v, lo, hi) (MIN(MAX((v), (lo)), (hi)))
#endif

#ifndef fallthrough
#define fallthrough ((void) 0)
#endif

#define UNUSED(x) x
#ifndef FUNC_INLINE
#define FUNC_INLINE inline
#endif

static FUNC_INLINE uint16_t
gd5440_read_le16(const uint8_t *src)
{
    return host_read_le16_unaligned(src);
}

static FUNC_INLINE uint32_t
gd5440_read_le32(const uint8_t *src)
{
    return host_read_le32_unaligned(src);
}

static FUNC_INLINE void
gd5440_write_le16(uint8_t *dst, uint16_t value)
{
    host_write_le16_unaligned(dst, value);
}

static FUNC_INLINE void
gd5440_write_le32(uint8_t *dst, uint32_t value)
{
    host_write_le32_unaligned(dst, value);
}

#define MEM_MAPPING_EXTERNAL 0
#define MEM_MAPPING_INTERNAL 1

#define PCI_REG_COMMAND 0x04
#define PCI_COMMAND_IO 0x01
#define PCI_COMMAND_MEM 0x02
#define PCI_COMMAND_MASTER 0x04
#define PCI_INTA 1
#define PCI_ADD_NORMAL 0
#define PCI_ADD_VIDEO 1
#define PCI_ADD_AGP 2

#define ATOMIC_INT volatile int
#define ATOMIC_UINT volatile unsigned int

typedef MEMORY_MAPPING_t gd5440_mem_mapping_t;

typedef struct device_t {
    const char *name;
    const char *internal_name;
    uint32_t flags;
    uint32_t local;
} device_t;

#define DEVICE_ISA 0x0004
#define DEVICE_CBUS 0x0010
#define DEVICE_ISA16 0x0020
#define DEVICE_MCA 0x0080
#define DEVICE_MCA32 0x0100
#define DEVICE_EISA 0x1000
#define DEVICE_AT32 0x2000
#define DEVICE_OLB 0x4000
#define DEVICE_VLB 0x8000
#define DEVICE_PCI 0x10000
#define DEVICE_AGP 0x80000

#define IBM_8514A 0
#define ATI_8514A_ULTRA 0

enum {
    VIDEO_ISA = 0,
    VIDEO_MCA,
    VIDEO_BUS,
    VIDEO_PCI = 3,
    VIDEO_AGP = 4
};

#define VIDEO_FLAG_TYPE_CGA 0
#define VIDEO_FLAG_TYPE_MDA 1
#define VIDEO_FLAG_TYPE_SPECIAL 2
#define VIDEO_FLAG_TYPE_8514 3
#define VIDEO_FLAG_TYPE_XGA 4
#define VIDEO_FLAG_TYPE_NONE 5

enum {
    STRING_MONITOR_SLEEP = 0
};

typedef struct gd5440_video_timings_t {
    int type;
    int write_b;
    int write_w;
    int write_l;
    int read_b;
    int read_w;
    int read_l;
} gd5440_video_timings_t;

typedef struct bitmap_t {
    int w;
    int h;
    uint32_t *dat;
    uint32_t *line[2112];
} bitmap_t;

typedef struct rgb_t {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef rgb_t PALETTE[256];

typedef struct dbcs_font_t {
    uint8_t chr[32];
} dbcs_font_t;

typedef struct monitor_t {
    char name[512];
    int mon_xsize;
    int mon_ysize;
    int mon_scrnsz_x;
    int mon_scrnsz_y;
    int mon_efscrnsz_y;
    int mon_unscaled_size_x;
    int mon_unscaled_size_y;
    double mon_res_x;
    double mon_res_y;
    int mon_bpp;
    bitmap_t *target_buffer;
    int mon_video_timing_read_b;
    int mon_video_timing_read_w;
    int mon_video_timing_read_l;
    int mon_video_timing_write_b;
    int mon_video_timing_write_w;
    int mon_video_timing_write_l;
    int mon_overscan_x;
    int mon_overscan_y;
    int mon_force_resize;
    int mon_fullchange;
    int mon_changeframecount;
    int mon_renderedframes;
    int mon_actualrenderedframes;
    int mon_screenshots;
    int mon_screenshots_clipboard;
    int mon_screenshots_raw;
    int mon_screenshots_raw_clipboard;
    uint32_t *mon_pal_lookup;
    int *mon_cga_palette;
    int mon_pal_lookup_static;
    int mon_cga_palette_static;
    const gd5440_video_timings_t *mon_vid_timings;
    int mon_vid_type;
    int mon_interlace;
    int mon_composite;
    void *mon_blit_data_ptr;
} monitor_t;

#define MONITORS_NUM 2
monitor_t gd5440_monitors[MONITORS_NUM];
int gd5440_monitor_index_global = 0;
int gd5440_show_second_monitors = 0;
int gd5440_video_fullscreen_scale_maximized = 0;
int gd5440_screenshots = 0;
int gd5440_enable_overscan = 0;
int gd5440_force_43 = 0;
int gd5440_vid_resize = 0;
int gd5440_herc_blend = 0;
int gd5440_vid_cga_contrast = 0;
int gd5440_video_grayscale = 0;
int gd5440_video_graytype = 0;
double gd5440_cpuclock = 0.0;
int gd5440_emu_fps = 0;
int gd5440_frames = 0;
int gd5440_readflash = 0;
int gd5440_cycles = 0;
int gd5440_ibm8514_active = 0;
int gd5440_xga_active = 0;
int gd5440_suppress_overscan = 0;
void (*gd5440_video_recalctimings)(void) = NULL;
uint8_t gd5440_doresize_monitors[MONITORS_NUM] = { 0 };

static void gd5440_request_present(int monitor_index, int x, int y, int w, int h);

static uint64_t VGACONST1 = 1;
static uint64_t VGACONST2 = 1;
static uint64_t compat_host_timer_freq = 0;
static wchar_t compat_monitor_sleep_text[] = L"Monitor in sleep mode";

dbcs_font_t *gd5440_fontdatksc5601 = NULL;
dbcs_font_t *gd5440_fontdatksc5601_user = NULL;

uint32_t *gd5440_video_6to8 = NULL;
uint32_t *gd5440_video_8togs = NULL;
uint32_t *gd5440_video_8to32 = NULL;
uint32_t *gd5440_video_15to32 = NULL;
uint32_t *gd5440_video_16to32 = NULL;

uint8_t gd5440_edatlookup[4][4];
uint8_t gd5440_egaremap2bpp[256];

static uint8_t compat_force_resize[MONITORS_NUM] = { 0 };

#define makecol(r, g, b)   ((uint32_t) (b) | ((uint32_t) (g) << 8) | ((uint32_t) (r) << 16) | 0xff000000u)
#define makecol32(r, g, b) makecol((r), (g), (b))
#define getcolr(color) (((color) >> 16) & 0xff)
#define getcolg(color) (((color) >> 8) & 0xff)
#define getcolb(color) ((color) & 0xff)

#define buffer32             (gd5440_monitors[gd5440_monitor_index_global].target_buffer)
#define pal_lookup           (gd5440_monitors[gd5440_monitor_index_global].mon_pal_lookup)
#define overscan_x           (gd5440_monitors[gd5440_monitor_index_global].mon_overscan_x)
#define overscan_y           (gd5440_monitors[gd5440_monitor_index_global].mon_overscan_y)
#define gd5440_video_timing_read_b  (gd5440_monitors[gd5440_monitor_index_global].mon_video_timing_read_b)
#define gd5440_video_timing_read_l  (gd5440_monitors[gd5440_monitor_index_global].mon_video_timing_read_l)
#define gd5440_video_timing_read_w  (gd5440_monitors[gd5440_monitor_index_global].mon_video_timing_read_w)
#define gd5440_video_timing_write_b (gd5440_monitors[gd5440_monitor_index_global].mon_video_timing_write_b)
#define gd5440_video_timing_write_l (gd5440_monitors[gd5440_monitor_index_global].mon_video_timing_write_l)
#define gd5440_video_timing_write_w (gd5440_monitors[gd5440_monitor_index_global].mon_video_timing_write_w)
#define gd5440_video_res_x          (gd5440_monitors[gd5440_monitor_index_global].mon_res_x)
#define gd5440_video_res_y          (gd5440_monitors[gd5440_monitor_index_global].mon_res_y)
#define gd5440_video_bpp            (gd5440_monitors[gd5440_monitor_index_global].mon_bpp)
#define xsize                (gd5440_monitors[gd5440_monitor_index_global].mon_xsize)
#define ysize                (gd5440_monitors[gd5440_monitor_index_global].mon_ysize)
#define changeframecount     (gd5440_monitors[gd5440_monitor_index_global].mon_changeframecount)
#define scrnsz_x             (gd5440_monitors[gd5440_monitor_index_global].mon_scrnsz_x)
#define scrnsz_y             (gd5440_monitors[gd5440_monitor_index_global].mon_scrnsz_y)
#define efscrnsz_y           (gd5440_monitors[gd5440_monitor_index_global].mon_efscrnsz_y)
#define unscaled_size_x      (gd5440_monitors[gd5440_monitor_index_global].mon_unscaled_size_x)
#define unscaled_size_y      (gd5440_monitors[gd5440_monitor_index_global].mon_unscaled_size_y)

static int compat_calc_6to8(int c) {
    int ic = c & 0x3f;
    if (c == 64) ic = 63;
    return (int) (((double) ic / 63.0) * 255.0) & 0xff;
}

static uint32_t compat_calc_8to32(int c) {
    int b = c & 0x03;
    int g = (c >> 2) & 0x07;
    int r = (c >> 5) & 0x07;
    return makecol32((int) ((((double) r) / 7.0) * 255.0),
                     (int) ((((double) g) / 7.0) * 255.0),
                     (int) ((((double) b) / 3.0) * 255.0));
}

static uint32_t compat_calc_15to32(int c) {
    int b = c & 31;
    int g = (c >> 5) & 31;
    int r = (c >> 10) & 31;
    return makecol32((int) ((((double) r) / 31.0) * 255.0),
                     (int) ((((double) g) / 31.0) * 255.0),
                     (int) ((((double) b) / 31.0) * 255.0));
}

static uint32_t compat_calc_16to32(int c) {
    int b = c & 31;
    int g = (c >> 5) & 63;
    int r = (c >> 11) & 31;
    return makecol32((int) ((((double) r) / 31.0) * 255.0),
                     (int) ((((double) g) / 63.0) * 255.0),
                     (int) ((((double) b) / 31.0) * 255.0));
}

bitmap_t *gd5440_create_bitmap(int w, int h) {
    bitmap_t *b = (bitmap_t *) calloc(1, sizeof(bitmap_t));
    int y;
    if (b == NULL) return NULL;
    b->w = w;
    b->h = h;
    b->dat = (uint32_t *) calloc((size_t) w * (size_t) h, sizeof(uint32_t));
    if (b->dat == NULL) { free(b); return NULL; }
    for (y = 0; y < h && y < (int) (sizeof(b->line) / sizeof(b->line[0])); y++)
        b->line[y] = &b->dat[(size_t) y * (size_t) w];
    return b;
}

void gd5440_destroy_bitmap(bitmap_t *b) {
    if (b == NULL) return;
    free(b->dat);
    free(b);
}

void gd5440_hline(bitmap_t *b, int x1, int y, int x2, uint32_t col) {
    int x;
    if (b == NULL || y < 0 || y >= b->h) return;
    if (x1 < 0) x1 = 0;
    if (x2 > b->w) x2 = b->w;
    for (x = x1; x < x2; x++) b->line[y][x] = col;
}

void gd5440_updatewindowsize(int x, int y) { (void) x; (void) y; }

static void compat_video_init_tables(void) {
    uint32_t c;
    uint8_t c0;
    uint8_t d0;

    if (gd5440_video_6to8 != NULL) return;

    for (c0 = 0; c0 < 4; c0++) {
        for (d0 = 0; d0 < 4; d0++) {
            gd5440_edatlookup[c0][d0] = 0;
            if (c0 & 1) gd5440_edatlookup[c0][d0] |= 1;
            if (d0 & 1) gd5440_edatlookup[c0][d0] |= 2;
            if (c0 & 2) gd5440_edatlookup[c0][d0] |= 0x10;
            if (d0 & 2) gd5440_edatlookup[c0][d0] |= 0x20;
        }
    }

    for (c = 0; c < 256; c++) {
        gd5440_egaremap2bpp[c] = 0;
        if (c & 0x01) gd5440_egaremap2bpp[c] |= 0x01;
        if (c & 0x04) gd5440_egaremap2bpp[c] |= 0x02;
        if (c & 0x10) gd5440_egaremap2bpp[c] |= 0x04;
        if (c & 0x40) gd5440_egaremap2bpp[c] |= 0x08;
    }

    gd5440_video_6to8 = (uint32_t *) malloc(sizeof(uint32_t) * 256);
    gd5440_video_8togs = (uint32_t *) malloc(sizeof(uint32_t) * 256);
    gd5440_video_8to32 = (uint32_t *) malloc(sizeof(uint32_t) * 256);
    gd5440_video_15to32 = (uint32_t *) malloc(sizeof(uint32_t) * 65536u);
    gd5440_video_16to32 = (uint32_t *) malloc(sizeof(uint32_t) * 65536u);

    if (gd5440_video_6to8 == NULL || gd5440_video_8togs == NULL || gd5440_video_8to32 == NULL ||
        gd5440_video_15to32 == NULL || gd5440_video_16to32 == NULL) {
        debug_log(DEBUG_ERROR, "[GD5440] Failed to allocate video conversion tables\r\n");
        exit(1);
    }

    for (c = 0; c < 256; c++) {
        gd5440_video_6to8[c] = (uint32_t) compat_calc_6to8(c);
        gd5440_video_8togs[c] = (uint32_t) c | ((uint32_t) c << 16) | ((uint32_t) c << 24);
        gd5440_video_8to32[c] = compat_calc_8to32(c);
    }

    for (c = 0; c < 65535u + 1u; c++) {
        gd5440_video_15to32[c] = compat_calc_15to32(c & 0x7fff);
        gd5440_video_16to32[c] = compat_calc_16to32(c);
    }
}

static void compat_video_init_monitor(void) {
    monitor_t *monitor = &gd5440_monitors[0];
    if (monitor->target_buffer != NULL) return;
    memset(monitor, 0, sizeof(*monitor));
    strcpy(monitor->name, "PCulator primary monitor");
    monitor->target_buffer = gd5440_create_bitmap(2048, 2048);
    monitor->mon_xsize = 640;
    monitor->mon_ysize = 480;
    monitor->mon_unscaled_size_x = 640;
    monitor->mon_unscaled_size_y = 480;
    monitor->mon_scrnsz_x = 640;
    monitor->mon_scrnsz_y = 480;
    monitor->mon_efscrnsz_y = 480;
    monitor->mon_overscan_x = 16;
    monitor->mon_overscan_y = 32;
    monitor->mon_bpp = 32;
    monitor->mon_changeframecount = 2;
    monitor->mon_force_resize = 1;
}

static uint64_t compat_get_host_timer_freq(void) {
    if (compat_host_timer_freq == 0)
        compat_host_timer_freq = timing_getFreq();
    return compat_host_timer_freq;
}

static double compat_video_clock_base(void) {
    double clock_base = (double) compat_get_host_timer_freq();

    return clock_base;
}

static void compat_refresh_vga_consts(void) {
    double clock_base = compat_video_clock_base();

    gd5440_cpuclock = clock_base;
    VGACONST1 = (uint64_t) ((clock_base / 25175000.0) * (double) (1ULL << 32));
    VGACONST2 = (uint64_t) ((clock_base / 28322000.0) * (double) (1ULL << 32));
    if (VGACONST1 == 0) VGACONST1 = 1;
    if (VGACONST2 == 0) VGACONST2 = 1;
}

static void compat_video_init(void) {
    compat_video_init_tables();
    compat_video_init_monitor();
    compat_refresh_vga_consts();
}

void gd5440_video_inform_monitor(int type, const gd5440_video_timings_t *ptr, int monitor_index) {
    if (monitor_index < 0 || monitor_index >= MONITORS_NUM) return;
    gd5440_monitors[monitor_index].mon_vid_type = type;
    gd5440_monitors[monitor_index].mon_vid_timings = ptr;
}

int gd5440_video_get_type_monitor(int monitor_index) {
    if (monitor_index < 0 || monitor_index >= MONITORS_NUM) return 0;
    return gd5440_monitors[monitor_index].mon_vid_type;
}

uint8_t gd5440_video_force_resize_get_monitor(int monitor_index) {
    if (monitor_index < 0 || monitor_index >= MONITORS_NUM) return 0;
    return compat_force_resize[monitor_index];
}

void gd5440_video_force_resize_set_monitor(uint8_t res, int monitor_index) {
    if (monitor_index < 0 || monitor_index >= MONITORS_NUM) return;
    compat_force_resize[monitor_index] = res;
}

void gd5440_video_wait_for_buffer_monitor(int monitor_index) { (void) monitor_index; }
void gd5440_video_blend_monitor(int x, int y, int monitor_index) { (void) x; (void) y; (void) monitor_index; }
void gd5440_video_process_8_monitor(int x, int y, int monitor_index) { (void) x; (void) y; (void) monitor_index; }

void gd5440_video_blit_memtoscreen_monitor(int x, int y, int w, int h, int monitor_index) {
    if (monitor_index < 0 || monitor_index >= MONITORS_NUM) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    gd5440_request_present(monitor_index, x, y, w, h);
}

static void gd5440_present_pending_frame(int monitor_index, int x, int y, int w, int h) {
    monitor_t *monitor;
    uint32_t *base;
    if (monitor_index < 0 || monitor_index >= MONITORS_NUM) return;
    monitor = &gd5440_monitors[monitor_index];
    if (monitor->target_buffer == NULL || monitor->target_buffer->dat == NULL) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > monitor->target_buffer->w) w = monitor->target_buffer->w - x;
    if (y + h > monitor->target_buffer->h) h = monitor->target_buffer->h - y;
    if (w <= 0 || h <= 0) return;
    base = &monitor->target_buffer->line[y][x];
    frontend_video_present(base, w, h, monitor->target_buffer->w * (int) sizeof(uint32_t));
}

wchar_t *gd5440_plat_get_string(int id) {
    if (id == STRING_MONITOR_SLEEP)
        return compat_monitor_sleep_text;
    return compat_monitor_sleep_text;
}
void gd5440_ui_sb_set_text_w(const wchar_t *text) { (void) text; }
void gd5440_fatal(const char *fmt, ...) {
    char buffer[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    buffer[sizeof(buffer) - 1] = 0;
    debug_log(DEBUG_ERROR, "%s", buffer);
    running = 0;
}
static void set_screen_size_monitor(int w, int h, int monitor_index) {
    if (monitor_index < 0 || monitor_index >= MONITORS_NUM) return;
    gd5440_monitors[monitor_index].mon_scrnsz_x = w;
    gd5440_monitors[monitor_index].mon_scrnsz_y = h;
    gd5440_monitors[monitor_index].mon_unscaled_size_x = w;
    gd5440_monitors[monitor_index].mon_unscaled_size_y = h;
    frontend_video_resize(w, h);
}

#define gd5440_video_inform(type, gd5440_video_timings_ptr) gd5440_video_inform_monitor((type), (gd5440_video_timings_ptr), gd5440_monitor_index_global)

uint64_t gd5440_tsc = 0;
uint64_t gd5440_timer_target = 0;
uint64_t GD5440_TIMER_USEC = 0;

#define TIMER_PROCESS 4
#define TIMER_SPLIT 2
#define TIMER_ENABLED 1

typedef struct pc_timer_t {
    uint64_t ts_integer;
    uint32_t ts_frac;
    int flags;
    int in_callback;
    double period;
    void (*callback)(void *priv);
    void *priv;
    uint32_t host_timer_id;
} pc_timer_t;

static void compat_timer_dispatch(void *opaque) {
    pc_timer_t *timer = (pc_timer_t *) opaque;
    if (timer->host_timer_id != TIMING_ERROR) timing_timerDisable(timer->host_timer_id);
    timer->flags &= ~TIMER_ENABLED;
    gd5440_tsc = timing_getCur();
    if (timer->callback != NULL) { timer->in_callback = 1; timer->callback(timer->priv); timer->in_callback = 0; }
}

static uint64_t compat_timer_remaining_ticks(pc_timer_t *timer) {
    uint64_t now = timing_getCur();
    uint64_t ticks = 0;
    if (timer->ts_integer > now) ticks = timer->ts_integer - now;
    if (timer->ts_frac != 0) ticks++;
    if (ticks == 0) ticks = 1;
    return ticks;
}

void gd5440_timer_enable(pc_timer_t *timer) {
    gd5440_tsc = timing_getCur();
    if (timer->host_timer_id == TIMING_ERROR) return;
    timing_updateInterval(timer->host_timer_id, compat_timer_remaining_ticks(timer));
    timing_timerEnable(timer->host_timer_id);
    timer->flags |= TIMER_ENABLED;
}

void gd5440_timer_disable(pc_timer_t *timer) {
    if (timer->host_timer_id != TIMING_ERROR) timing_timerDisable(timer->host_timer_id);
    timer->flags &= ~TIMER_ENABLED;
}

void gd5440_timer_stop(pc_timer_t *timer) { gd5440_timer_disable(timer); }

void gd5440_timer_add(pc_timer_t *timer, void (*callback)(void *priv), void *priv, int start_timer) {
    memset(timer, 0, sizeof(*timer));
    timer->callback = callback;
    timer->priv = priv;
    timer->host_timer_id = timing_addTimer(compat_timer_dispatch, timer, (double) compat_get_host_timer_freq(), TIMING_DISABLED);
    if (start_timer) {
        timer->ts_integer = timing_getCur() + 1;
        timer->ts_frac = 0;
        gd5440_timer_enable(timer);
    }
}

static inline void gd5440_timer_set_callback(pc_timer_t *timer, void (*callback)(void *priv)) { timer->callback = callback; }
static inline void gd5440_timer_set_p(pc_timer_t *timer, void *priv) { timer->priv = priv; }
static inline int gd5440_timer_is_enabled(pc_timer_t *timer) { return !!(timer->flags & TIMER_ENABLED); }

static inline void gd5440_timer_advance_u64(pc_timer_t *timer, uint64_t delay) {
    uint64_t int_delay = delay >> 32;
    uint32_t frac_delay = (uint32_t) (delay & 0xffffffffu);
    if ((uint32_t) (timer->ts_frac + frac_delay) < timer->ts_frac) timer->ts_integer++;
    timer->ts_frac += frac_delay;
    timer->ts_integer += int_delay;
    gd5440_timer_enable(timer);
}

static inline void gd5440_timer_set_delay_u64(pc_timer_t *timer, uint64_t delay) {
    gd5440_tsc = timing_getCur();
    timer->ts_integer = timing_getCur() + (delay >> 32);
    timer->ts_frac = (uint32_t) (delay & 0xffffffffu);
    gd5440_timer_enable(timer);
}

void gd5440_timer_on_auto(pc_timer_t *timer, double period) {
    uint64_t ticks;
    timer->period = period;
    gd5440_tsc = timing_getCur();
    ticks = (uint64_t) ((period * (double) compat_get_host_timer_freq() / 1000000.0) + 0.5);
    if (ticks == 0) ticks = 1;
    timer->ts_integer = timing_getCur() + ticks;
    timer->ts_frac = 0;
    gd5440_timer_enable(timer);
}

static void update_tsc(void) { gd5440_tsc = timing_getCur(); }
typedef struct compat_io_handler_t {
    uint8_t (*readb)(uint16_t, void *);
    uint16_t (*readw)(uint16_t, void *);
    uint32_t (*readl)(uint16_t, void *);
    void (*writeb)(uint16_t, uint8_t, void *);
    void (*writew)(uint16_t, uint16_t, void *);
    void (*writel)(uint16_t, uint32_t, void *);
    void *priv;
} compat_io_handler_t;

static uint8_t compat_io_readb(void *opaque, uint16_t port) {
    compat_io_handler_t *ctx = (compat_io_handler_t *) opaque;
    return (ctx->readb != NULL) ? ctx->readb(port, ctx->priv) : 0xff;
}
static uint16_t compat_io_readw(void *opaque, uint16_t port) {
    compat_io_handler_t *ctx = (compat_io_handler_t *) opaque;
    if (ctx->readw != NULL) return ctx->readw(port, ctx->priv);
    return (uint16_t) compat_io_readb(opaque, port) | ((uint16_t) compat_io_readb(opaque, port + 1) << 8);
}
static uint32_t compat_io_readl(void *opaque, uint16_t port) {
    compat_io_handler_t *ctx = (compat_io_handler_t *) opaque;
    if (ctx->readl != NULL) return ctx->readl(port, ctx->priv);
    return (uint32_t) compat_io_readw(opaque, port) | ((uint32_t) compat_io_readw(opaque, port + 2) << 16);
}
static void compat_io_writeb(void *opaque, uint16_t port, uint8_t value) {
    compat_io_handler_t *ctx = (compat_io_handler_t *) opaque;
    if (ctx->writeb != NULL) ctx->writeb(port, value, ctx->priv);
}
static void compat_io_writew(void *opaque, uint16_t port, uint16_t value) {
    compat_io_handler_t *ctx = (compat_io_handler_t *) opaque;
    if (ctx->writew != NULL) { ctx->writew(port, value, ctx->priv); return; }
    compat_io_writeb(opaque, port, (uint8_t) value);
    compat_io_writeb(opaque, port + 1, (uint8_t) (value >> 8));
}
static void compat_io_writel(void *opaque, uint16_t port, uint32_t value) {
    compat_io_handler_t *ctx = (compat_io_handler_t *) opaque;
    if (ctx->writel != NULL) { ctx->writel(port, value, ctx->priv); return; }
    compat_io_writew(opaque, port, (uint16_t) value);
    compat_io_writew(opaque, port + 2, (uint16_t) (value >> 16));
}

void gd5440_io_sethandler(uint16_t start, uint16_t count,
                   uint8_t (*readb)(uint16_t, void *),
                   uint16_t (*readw)(uint16_t, void *),
                   uint32_t (*readl)(uint16_t, void *),
                   void (*writeb)(uint16_t, uint8_t, void *),
                   void (*writew)(uint16_t, uint16_t, void *),
                   void (*writel)(uint16_t, uint32_t, void *),
                   void *priv) {
    compat_io_handler_t *ctx = (compat_io_handler_t *) calloc(1, sizeof(*ctx));
    if (ctx == NULL) return;
    ctx->readb = readb;
    ctx->readw = readw;
    ctx->readl = readl;
    ctx->writeb = writeb;
    ctx->writew = writew;
    ctx->writel = writel;
    ctx->priv = priv;
    ports_cbRegisterEx(start, count, compat_io_readb, compat_io_readw, compat_io_readl,
                       compat_io_writeb, compat_io_writew, compat_io_writel, ctx);
}

void gd5440_io_removehandler(uint16_t start, uint16_t count,
                      uint8_t (*readb)(uint16_t, void *),
                      uint16_t (*readw)(uint16_t, void *),
                      uint32_t (*readl)(uint16_t, void *),
                      void (*writeb)(uint16_t, uint8_t, void *),
                      void (*writew)(uint16_t, uint16_t, void *),
                      void (*writel)(uint16_t, uint32_t, void *),
                      void *priv) {
    (void) readb; (void) readw; (void) readl; (void) writeb; (void) writew; (void) writel; (void) priv;
    ports_cbUnregister(start, count);
}

typedef struct compat_mem_handler_t {
    uint8_t (*readb)(uint32_t, void *);
    uint16_t (*readw)(uint32_t, void *);
    uint32_t (*readl)(uint32_t, void *);
    void (*writeb)(uint32_t, uint8_t, void *);
    void (*writew)(uint32_t, uint16_t, void *);
    void (*writel)(uint32_t, uint32_t, void *);
    void *priv;
} compat_mem_handler_t;

static uint8_t compat_mem_readb(void* opaque, uint32_t addr) {
    compat_mem_handler_t* ctx = (compat_mem_handler_t*)opaque;
    return (ctx->readb != NULL) ? ctx->readb(addr, ctx->priv) : 0xff;
}
static uint16_t compat_mem_readw(void* opaque, uint32_t addr) {
    compat_mem_handler_t* ctx = (compat_mem_handler_t*)opaque;
    if (ctx->readw != NULL) return ctx->readw(addr, ctx->priv);
    return (uint16_t)compat_mem_readb(opaque, addr) | ((uint16_t)compat_mem_readb(opaque, addr + 1) << 8);
}
static uint32_t compat_mem_readl(void* opaque, uint32_t addr) {
    compat_mem_handler_t* ctx = (compat_mem_handler_t*)opaque;
    if (ctx->readl != NULL) return ctx->readl(addr, ctx->priv);
    return (uint32_t)compat_mem_readw(opaque, addr) | ((uint32_t)compat_mem_readw(opaque, addr + 2) << 16);
}
static void compat_mem_writeb(void* opaque, uint32_t addr, uint8_t value) {
    compat_mem_handler_t* ctx = (compat_mem_handler_t*)opaque;
    if (ctx->writeb != NULL) ctx->writeb(addr, value, ctx->priv);
}
static void compat_mem_writew(void* opaque, uint32_t addr, uint16_t value) {
    compat_mem_handler_t* ctx = (compat_mem_handler_t*)opaque;
    if (ctx->writew != NULL) { ctx->writew(addr, value, ctx->priv); return; }
    compat_mem_writeb(opaque, addr, (uint8_t)value);
    compat_mem_writeb(opaque, addr + 1, (uint8_t)(value >> 8));
}
static void compat_mem_writel(void* opaque, uint32_t addr, uint32_t value) {
    compat_mem_handler_t* ctx = (compat_mem_handler_t*)opaque;
    if (ctx->writel != NULL) { ctx->writel(addr, value, ctx->priv); return; }
    compat_mem_writew(opaque, addr, (uint16_t)value);
    compat_mem_writew(opaque, addr + 2, (uint16_t)(value >> 16));
}


void gd5440_mem_mapping_add(gd5440_mem_mapping_t *mapping, uint32_t start, uint32_t len,
                     uint8_t (*readb)(uint32_t, void *),
                     uint16_t (*readw)(uint32_t, void *),
                     uint32_t (*readl)(uint32_t, void *),
                     void (*writeb)(uint32_t, uint8_t, void *),
                     void (*writew)(uint32_t, uint16_t, void *),
                     void (*writel)(uint32_t, uint32_t, void *),
                     uint8_t *exec,
                     uint32_t flags,
                     void *priv) {
    compat_mem_handler_t *ctx = (compat_mem_handler_t *) calloc(1, sizeof(*ctx));
    (void) exec;
    if (ctx == NULL) return;
    ctx->readb = readb;
    ctx->readw = readw;
    ctx->readl = readl;
    ctx->writeb = writeb;
    ctx->writew = writew;
    ctx->writel = writel;
    ctx->priv = priv;

    memory_mapping_add(mapping, start, len,
        compat_mem_readb, compat_mem_readw, compat_mem_readl,
        compat_mem_writeb, compat_mem_writew, compat_mem_writel,
        ctx,
        (flags == MEM_MAPPING_INTERNAL) ? 1 : 0);

    if (len != 0) {
        memory_mapping_enable(mapping);
    }
}

void gd5440_mem_mapping_set_handler(gd5440_mem_mapping_t *mapping,
                             uint8_t (*readb)(uint32_t, void *),
                             uint16_t (*readw)(uint32_t, void *),
                             uint32_t (*readl)(uint32_t, void *),
                             void (*writeb)(uint32_t, uint8_t, void *),
                             void (*writew)(uint32_t, uint16_t, void *),
                             void (*writel)(uint32_t, uint32_t, void *)) {
    compat_mem_handler_t *ctx;

    if (mapping == NULL) {
        return;
    }

    ctx = (compat_mem_handler_t *) mapping->desc.udata;
    if (ctx == NULL) {
        return;
    }

    ctx->readb = readb;
    ctx->readw = readw;
    ctx->readl = readl;
    ctx->writeb = writeb;
    ctx->writew = writew;
    ctx->writel = writel;
}

#define gd5440_mem_mapping_enable memory_mapping_enable
#define gd5440_mem_mapping_disable memory_mapping_disable

static void compat_mem_mapping_set_addr(gd5440_mem_mapping_t *mapping, uint32_t start, uint32_t len) {
    if (mapping == NULL) {
        return;
    }

    if (mapping->enabled) {
        memory_mapping_disable(mapping);
    }

    mapping->start = start;
    mapping->len = len;

    if (len != 0) {
        memory_mapping_enable(mapping);
    }
}

#define gd5440_mem_mapping_set_addr compat_mem_mapping_set_addr

typedef struct gd5440_rom_t {
    uint8_t *rom;
    int sz;
    uint32_t mask;
    gd5440_mem_mapping_t mapping;
} gd5440_rom_t;

static uint8_t gd5440_rom_read(uint32_t addr, void *priv) {
    gd5440_rom_t *rom = (gd5440_rom_t *) priv;
    if (rom->rom == NULL || rom->sz <= 0) return 0xff;
    return rom->rom[addr & rom->mask];
}
static void gd5440_rom_write(uint32_t addr, uint8_t val, void *priv) { (void) addr; (void) val; (void) priv; }

int gd5440_rom_init(gd5440_rom_t *rom, const char *fn, uint32_t address, int size, int mask, int file_offset, uint32_t flags) {
    FILE *fp;
    memset(rom, 0, sizeof(*rom));
    rom->rom = (uint8_t *) calloc(1, (size_t) size);
    if (rom->rom == NULL) return -1;
    fp = fopen(fn, "rb");
    if (fp == NULL) { debug_log(DEBUG_ERROR, "[GD5440] Missing ROM image: %s\r\n", fn); return -1; }
    if (file_offset > 0) fseek(fp, file_offset, SEEK_SET);
    fread(rom->rom, 1, (size_t) size, fp);
    fclose(fp);
    rom->sz = size;
    rom->mask = (uint32_t) mask;
    gd5440_mem_mapping_add(&rom->mapping, address, (uint32_t) size, gd5440_rom_read, NULL, NULL, gd5440_rom_write, NULL, NULL, NULL, flags, rom);
    return 0;
}

static int gd5440_rom_present(const char *fn) {
    FILE *fp = fopen(fn, "rb");
    if (fp == NULL) return 0;
    fclose(fp);
    return 1;
}
typedef struct compat_i2c_bus_t {
    uint8_t scl;
    uint8_t sda_master;
    uint8_t sda_device;
    uint8_t state;
    uint8_t shift;
    uint8_t bitcount;
    uint8_t selected;
    uint8_t read_mode;
    uint8_t ptr;
    uint8_t edid[256];
    uint16_t edid_size;
} compat_i2c_bus_t;

enum {
    COMPAT_I2C_IDLE = 0,
    COMPAT_I2C_RECV_ADDR,
    COMPAT_I2C_ACK_ADDR,
    COMPAT_I2C_RECV_OFFSET,
    COMPAT_I2C_ACK_OFFSET,
    COMPAT_I2C_RECV_DATA,
    COMPAT_I2C_ACK_DATA,
    COMPAT_I2C_SEND_DATA_SETUP,
    COMPAT_I2C_SEND_DATA,
    COMPAT_I2C_RECV_MASTER_ACK
};

static uint8_t compat_i2c_line_sda(compat_i2c_bus_t *bus) {
    if (!bus->sda_master) return 0;
    return bus->sda_device ? 1 : 0;
}

static void compat_i2c_load_default_edid(uint8_t *edid, uint16_t *size_out) {
    static const uint8_t default_edid[128] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x09, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x30, 0x1e, 0x01, 0x04, 0x0e, 0x21, 0x18, 0x78, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x81, 0x80, 0x81, 0x40, 0x81, 0xc0, 0x95, 0x00,
        0xa9, 0x40, 0xb3, 0x00, 0xd1, 0xc0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x30, 0x2a, 0x00, 0x98,
        0x51, 0x00, 0x2a, 0x40, 0x30, 0x70, 0x13, 0x00, 0x58, 0xc1, 0x10, 0x00, 0x00, 0x1e, 0x00, 0x00,
        0x00, 0xfd, 0x00, 0x2d, 0x7d, 0x1e, 0x73, 0x1e, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x00, 0x00, 0x00, 0xfc, 0x00, 0x38, 0x36, 0x42, 0x6f, 0x78, 0x20, 0x4d, 0x6f, 0x6e, 0x69, 0x74,
        0x6f, 0x72, 0x00, 0x00, 0x00, 0xff, 0x00, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30
    };
    uint8_t checksum = 0;
    int i;
    memcpy(edid, default_edid, sizeof(default_edid));
    for (i = 0; i < 127; i++) checksum = (uint8_t) (checksum + edid[i]);
    edid[127] = (uint8_t) (0x100 - checksum);
    *size_out = 128;
}

static void compat_i2c_start(compat_i2c_bus_t *bus) {
    bus->state = COMPAT_I2C_RECV_ADDR;
    bus->shift = 0;
    bus->bitcount = 0;
    bus->selected = 0;
    bus->read_mode = 0;
    bus->sda_device = 1;
}
static void compat_i2c_stop(compat_i2c_bus_t *bus) { bus->state = COMPAT_I2C_IDLE; bus->sda_device = 1; }

static void compat_i2c_on_rising_scl(compat_i2c_bus_t *bus) {
    uint8_t bit = bus->sda_master ? 1 : 0;
    switch (bus->state) {
        case COMPAT_I2C_RECV_ADDR:
        case COMPAT_I2C_RECV_OFFSET:
        case COMPAT_I2C_RECV_DATA:
            bus->shift = (uint8_t) ((bus->shift << 1) | bit);
            bus->bitcount++;
            if (bus->bitcount == 8) {
                bus->bitcount = 0;
                if (bus->state == COMPAT_I2C_RECV_ADDR) {
                    bus->selected = ((bus->shift >> 1) == 0x50) ? 1 : 0;
                    bus->read_mode = bus->shift & 1;
                    bus->state = COMPAT_I2C_ACK_ADDR;
                    bus->sda_device = bus->selected ? 0 : 1;
                } else if (bus->state == COMPAT_I2C_RECV_OFFSET) {
                    bus->ptr = bus->shift;
                    bus->state = COMPAT_I2C_ACK_OFFSET;
                    bus->sda_device = 0;
                } else {
                    bus->ptr = (uint8_t) (bus->ptr + 1);
                    bus->state = COMPAT_I2C_ACK_DATA;
                    bus->sda_device = 0;
                }
            }
            break;
        case COMPAT_I2C_SEND_DATA:
            bus->bitcount++;
            bus->shift <<= 1;
            if (bus->bitcount == 8) {
                bus->bitcount = 0;
                bus->state = COMPAT_I2C_RECV_MASTER_ACK;
                bus->sda_device = 1;
            }
            break;
        case COMPAT_I2C_RECV_MASTER_ACK:
            if (!bit) {
                bus->ptr = (uint8_t) (bus->ptr + 1);
                bus->state = COMPAT_I2C_SEND_DATA_SETUP;
            } else {
                bus->state = COMPAT_I2C_IDLE;
            }
            break;
        default:
            break;
    }
}

static void compat_i2c_on_falling_scl(compat_i2c_bus_t *bus) {
    switch (bus->state) {
        case COMPAT_I2C_ACK_ADDR:
            bus->sda_device = 1;
            if (!bus->selected) {
                bus->state = COMPAT_I2C_IDLE;
            } else if (bus->read_mode) {
                bus->shift = bus->edid[bus->ptr % bus->edid_size];
                bus->state = COMPAT_I2C_SEND_DATA;
                bus->sda_device = (bus->shift & 0x80) ? 1 : 0;
            } else {
                bus->state = COMPAT_I2C_RECV_OFFSET;
                bus->shift = 0;
            }
            break;
        case COMPAT_I2C_ACK_OFFSET:
            bus->sda_device = 1;
            bus->state = COMPAT_I2C_RECV_DATA;
            bus->shift = 0;
            break;
        case COMPAT_I2C_ACK_DATA:
            bus->sda_device = 1;
            bus->state = COMPAT_I2C_RECV_DATA;
            bus->shift = 0;
            break;
        case COMPAT_I2C_SEND_DATA_SETUP:
            bus->shift = bus->edid[bus->ptr % bus->edid_size];
            bus->state = COMPAT_I2C_SEND_DATA;
            bus->sda_device = (bus->shift & 0x80) ? 1 : 0;
            break;
        case COMPAT_I2C_SEND_DATA:
            if (bus->bitcount < 8) bus->sda_device = (bus->shift & 0x80) ? 1 : 0;
            break;
        default:
            break;
    }
}

void *gd5440_i2c_gpio_init(char *bus_name) {
    compat_i2c_bus_t *bus = (compat_i2c_bus_t *) calloc(1, sizeof(*bus));
    (void) bus_name;
    if (bus == NULL) return NULL;
    bus->scl = 1;
    bus->sda_master = 1;
    bus->sda_device = 1;
    compat_i2c_load_default_edid(bus->edid, &bus->edid_size);
    return bus;
}
void gd5440_i2c_gpio_close(void *dev_handle) { free(dev_handle); }
void gd5440_i2c_gpio_set(void *dev_handle, uint8_t scl, uint8_t sda) {
    compat_i2c_bus_t *bus = (compat_i2c_bus_t *) dev_handle;
    uint8_t old_scl = bus->scl;
    uint8_t old_sda = bus->sda_master;
    bus->scl = scl ? 1 : 0;
    bus->sda_master = sda ? 1 : 0;
    if (old_sda && !bus->sda_master && bus->scl) compat_i2c_start(bus);
    else if (!old_sda && bus->sda_master && bus->scl) compat_i2c_stop(bus);
    if (!old_scl && bus->scl) compat_i2c_on_rising_scl(bus);
    else if (old_scl && !bus->scl) compat_i2c_on_falling_scl(bus);
}
uint8_t gd5440_i2c_gpio_get_scl(void *dev_handle) { compat_i2c_bus_t *bus = (compat_i2c_bus_t *) dev_handle; return bus->scl ? 1 : 0; }
uint8_t gd5440_i2c_gpio_get_sda(void *dev_handle) { compat_i2c_bus_t *bus = (compat_i2c_bus_t *) dev_handle; return compat_i2c_line_sda(bus); }
void *gd5440_i2c_gpio_get_bus(void *dev_handle) { return dev_handle; }
size_t gd5440_ddc_create_default_edid(uint8_t **out) {
    uint8_t *edid = (uint8_t *) malloc(128); uint16_t size = 0; if (edid == NULL) return 0;
    compat_i2c_load_default_edid(edid, &size); if (out != NULL) *out = edid; else free(edid); return size;
}
size_t gd5440_ddc_load_edid(char *path, uint8_t *buf, size_t size) {
    FILE *fp; size_t read; if (path == NULL || buf == NULL || size == 0) return 0;
    fp = fopen(path, "rb"); if (fp == NULL) return 0; read = fread(buf, 1, size, fp); fclose(fp); return read;
}
void *gd5440_ddc_init(void *i2c) { return i2c; }
void gd5440_ddc_close(void *eeprom) { (void) eeprom; }

static PCI_t *compat_pci_bus = NULL;
static DMA_t *compat_dma_bus = NULL;
static uint8_t compat_pci_slot = 0;

typedef struct compat_pci_handler_t {
    uint8_t (*read)(int, int, int, void *);
    void (*write)(int, int, int, uint8_t, void *);
    void *priv;
} compat_pci_handler_t;

static uint8_t compat_pci_read_byte(void *opaque, uint8_t function, uint8_t addr) {
    compat_pci_handler_t *ctx = (compat_pci_handler_t *) opaque;
    return ctx->read((int) function, (int) addr, 1, ctx->priv);
}
static void compat_pci_write_byte(void *opaque, uint8_t function, uint8_t addr, uint8_t value) {
    compat_pci_handler_t *ctx = (compat_pci_handler_t *) opaque;
    ctx->write((int) function, (int) addr, 1, value, ctx->priv);
}

void gd5440_pci_add_card(int type, uint8_t (*read)(int, int, int, void *), void (*write)(int, int, int, uint8_t, void *), void *priv, uint8_t *slot_out) {
    compat_pci_handler_t *ctx = (compat_pci_handler_t *) calloc(1, sizeof(*ctx));
    (void) type;
    if (ctx == NULL) return;
    ctx->read = read;
    ctx->write = write;
    ctx->priv = priv;
    if (slot_out != NULL) *slot_out = compat_pci_slot;
    pci_register_device(compat_pci_bus, compat_pci_slot, 0, compat_pci_read_byte, compat_pci_write_byte, ctx);
}

static void pci_set_irq(uint8_t slot, uint8_t intpin, uint8_t *irq_state) {
    if (*irq_state) return;
    pci_raise_irq(compat_pci_bus, slot, intpin);
    *irq_state = 1;
}
static void pci_clear_irq(uint8_t slot, uint8_t intpin, uint8_t *irq_state) {
    if (!*irq_state) return;
    pci_lower_irq(compat_pci_bus, slot, intpin);
    *irq_state = 0;
}

#define dma_bm_read(addr, data, total_size, transfer_size) \
    dma_bm_read(compat_dma_bus, (addr), (data), (total_size), (transfer_size))
#define dma_bm_write(addr, data, total_size, transfer_size) \
    dma_bm_write(compat_dma_bus, (addr), (data), (total_size), (transfer_size))

static uint64_t plat_timer_read(void) { return timing_getCur(); }

static int gd5440_device_get_config_int(const char *name) {
    if (_stricmp(name, "memory") == 0) return 2;
    if (_stricmp(name, "lfb_base") == 0) return 0;
    return 0;
}

typedef struct gd5440_ibm8514_t {
    int on; uint8_t dac_mask; uint8_t dac_status; uint8_t dac_addr; uint8_t dac_pos; uint8_t dac_r; uint8_t dac_g; uint8_t dac_b;
    PALETTE _8514pal; uint32_t pallook[512]; uint32_t h_sync_start; uint32_t h_blank_end_val; uint32_t h_blank_end_mask; uint32_t h_blank_sub;
    int hdisp; int dispend; int h_total; int interlace; int h_disp_time; uint64_t dispontime; uint64_t dispofftime;
} gd5440_ibm8514_t;

typedef struct gd5440_xga_t {
    int on; PALETTE xgapal; uint32_t pallook[256]; int h_total; int h_disp_time; int interlace; uint64_t dispontime; uint64_t dispofftime;
} gd5440_xga_t;

typedef struct gd5440_svga_t gd5440_svga_t;

void gd5440_ibm8514_set_poll(struct gd5440_svga_t *svga) { (void) svga; }
void gd5440_ibm8514_poll(void *priv) { (void) priv; }
void gd5440_ibm8514_recalctimings(struct gd5440_svga_t *svga) { (void) svga; }
uint8_t gd5440_ibm8514_ramdac_in(uint16_t port, void *priv) { (void) port; (void) priv; return 0xff; }
void gd5440_ibm8514_ramdac_out(uint16_t port, uint8_t val, void *priv) { (void) port; (void) val; (void) priv; }
void gd5440_ibm8514_accel_out_fifo(struct gd5440_svga_t *svga, uint16_t port, uint32_t val, int len) { (void) svga; (void) port; (void) val; (void) len; }
void gd5440_ibm8514_accel_out(uint16_t port, uint32_t val, struct gd5440_svga_t *svga, int len) { (void) port; (void) val; (void) svga; (void) len; }
uint16_t gd5440_ibm8514_accel_in_fifo(struct gd5440_svga_t *svga, uint16_t port, int len) { (void) svga; (void) port; (void) len; return 0xffff; }
uint8_t gd5440_ibm8514_accel_in(uint16_t port, struct gd5440_svga_t *svga) { (void) port; (void) svga; return 0xff; }
int gd5440_ibm8514_cpu_src(struct gd5440_svga_t *svga) { (void) svga; return 0; }
int gd5440_ibm8514_cpu_dest(struct gd5440_svga_t *svga) { (void) svga; return 0; }
void gd5440_ibm8514_accel_out_pixtrans(struct gd5440_svga_t *svga, uint16_t port, uint32_t val, int len) { (void) svga; (void) port; (void) val; (void) len; }
void gd5440_ibm8514_short_stroke_start(int count, int cpu_input, uint32_t mix_dat, uint32_t cpu_dat, struct gd5440_svga_t *svga, uint8_t ssv, int len) { (void) count; (void) cpu_input; (void) mix_dat; (void) cpu_dat; (void) svga; (void) ssv; (void) len; }
void gd5440_ibm8514_accel_start(int count, int cpu_input, uint32_t mix_dat, uint32_t cpu_dat, struct gd5440_svga_t *svga, int len) { (void) count; (void) cpu_input; (void) mix_dat; (void) cpu_dat; (void) svga; (void) len; }
void gd5440_xga_set_poll(struct gd5440_svga_t *svga) { (void) svga; }
void gd5440_xga_poll(void *priv) { (void) priv; }
void gd5440_xga_recalctimings(struct gd5440_svga_t *svga) { (void) svga; }
void gd5440_xga_write_test(uint32_t addr, uint8_t val, void *priv) { (void) addr; (void) val; (void) priv; }
uint8_t gd5440_xga_read_test(uint32_t addr, void *priv) { (void) addr; (void) priv; return 0xff; }
static void pclog_ex(const char *fmt, va_list ap) { (void) fmt; (void) ap; }

#    define FLAG_EXTRA_BANKS  1
#    define FLAG_ADDR_BY8     2
#    define FLAG_EXT_WRITE    4
#    define FLAG_LATCH8       8
#    define FLAG_NOSKEW       16
#    define FLAG_ADDR_BY16    32
#    define FLAG_RAMDAC_SHIFT 64
#    define FLAG_ATI          128
#    define FLAG_S3_911_16BIT 256
#    define FLAG_512K_MASK    512
#    define FLAG_NO_SHIFT3    1024 /* Needed for Bochs VBE. */
#    define FLAG_PRECISETIME  2048 /* Needed for Copper demo if on dynarec. */
#    define FLAG_PANNING_ATI  4096
struct monitor_t;

typedef struct hwcursor_t {
    int      ena;
    int      x;
    int      y;
    int      xoff;
    int      yoff;
    int      cur_xsize;
    int      cur_ysize;
    int      v_acc;
    int      h_acc;
    uint32_t addr;
    uint32_t pitch;
} hwcursor_t;

typedef union {
    uint64_t q;
    uint32_t d[2];
    uint16_t w[4];
    uint8_t  b[8];
} latch_t;

typedef struct gd5440_svga_t {
    gd5440_mem_mapping_t mapping;

    uint8_t fast;
    uint8_t chain4;
    uint8_t chain2_write;
    uint8_t chain2_read;
    uint8_t ext_overscan;
    uint8_t bus_size;
    uint8_t lowres;
    uint8_t interlace;
    uint8_t linedbl;
    uint8_t rowcount;
    uint8_t set_reset_disabled;
    uint8_t bpp;
    uint8_t ramdac_type;
    uint8_t fb_only;
    uint8_t readmode;
    uint8_t writemode;
    uint8_t readplane;
    uint8_t hwcursor_oddeven;
    uint8_t dac_hwcursor_oddeven;
    uint8_t overlay_oddeven;
    uint8_t fcr;
    uint8_t hblank_overscan;
    uint8_t vidsys_ena;
    uint8_t sleep;

    int dac_addr;
    int dac_pos;
    int dac_r;
    int dac_g;
    int dac_b;
    int vtotal;
    int dispend;
    int vdisp;
    int vsyncstart;
    int split;
    int vblankstart;
    int hdisp;
    int hdisp_old;
    int htotal;
    int hdisp_time;
    int rowoffset;
    int dispon;
    int hdisp_on;
    int vc;
    int scanline;
    int linepos;
    int vslines;
    int linecountff;
    int oddeven;
    int cursorvisible;
    int cursoron;
    int blink;
    int scrollcache;
    int char_width;
    int firstline;
    int lastline;
    int firstline_draw;
    int lastline_draw;
    int displine;
    int fullchange;
    int left_overscan;
    int x_add;
    int y_add;
    int pan;
    int vram_display_mask;
    int vidclock;
    int dots_per_clock;
    int hwcursor_on;
    int dac_hwcursor_on;
    int overlay_on;
    int set_override;
    int hblankstart;
    int hblankend;
    int hblank_end_val;
    int hblank_end_len;
    int hblank_end_mask;
    int hblank_sub;
    int packed_4bpp;
    int ps_bit_bug;
    int ati_4color;
    int vblankend;
    int panning_blank;
    int render_line_offset;
    int start_retrace_latch;
    int vga_mode;
    int half_pixel;
    int clock_multiplier;
    int true_color_bypass;
    int multiplexing_rate;

    /*The three variables below allow us to implement memory maps like that seen on a 1MB Trio64 :
      0MB-1MB - VRAM
      1MB-2MB - VRAM mirror
      2MB-4MB - open bus
      4MB-xMB - mirror of above

      For the example memory map, decode_mask would be 4MB-1 (4MB address space), vram_max would be 2MB
      (present video memory only responds to first 2MB), vram_mask would be 1MB-1 (video memory wraps at 1MB)
    */
    uint32_t  decode_mask;
    uint32_t  vram_max;
    uint32_t  vram_mask;
    uint32_t  charseta;
    uint32_t  charsetb;
    uint32_t  adv_flags;
    uint32_t  memaddr_latch;
    uint32_t  ca_adj;
    uint32_t  memaddr;
    uint32_t  memaddr_backup;
    uint32_t  write_bank;
    uint32_t  read_bank;
    uint32_t  extra_banks[2];
    uint32_t  banked_mask;
    uint32_t  cursoraddr;
    uint32_t  overscan_color;
    uint32_t *map8;
    uint32_t  pallook[512];

    PALETTE vgapal;

    uint64_t dispontime;
    uint64_t dispofftime;
    latch_t  latch;

    pc_timer_t timer;
    pc_timer_t gd5440_timer_8514;
    pc_timer_t gd5440_timer_xga;

    double clock;
    double clock_8514;
    double clock_xga;

    double multiplier;

    hwcursor_t hwcursor;
    hwcursor_t hwcursor_latch;
    hwcursor_t dac_hwcursor;
    hwcursor_t dac_hwcursor_latch;
    hwcursor_t overlay;
    hwcursor_t overlay_latch;

    void (*render)(struct gd5440_svga_t *svga);
    void (*render8514)(struct gd5440_svga_t *svga);
    void (*render_xga)(struct gd5440_svga_t *svga);
    void (*recalctimings_ex)(struct gd5440_svga_t *svga);

    void (*gd5440_video_out)(uint16_t addr, uint8_t val, void *priv);
    uint8_t (*gd5440_video_in)(uint16_t addr, void *priv);

    void (*hwcursor_draw)(struct gd5440_svga_t *svga, int displine);

    void (*dac_hwcursor_draw)(struct gd5440_svga_t *svga, int displine);

    void (*overlay_draw)(struct gd5440_svga_t *svga, int displine);

    void (*vblank_start)(struct gd5440_svga_t *svga);

    void (*write)(uint32_t addr, uint8_t val, void *priv);
    void (*writew)(uint32_t addr, uint16_t val, void *priv);
    void (*writel)(uint32_t addr, uint32_t val, void *priv);

    uint8_t (*read)(uint32_t addr, void *priv);
    uint16_t (*readw)(uint32_t addr, void *priv);
    uint32_t (*readl)(uint32_t addr, void *priv);

    void (*ven_write)(struct gd5440_svga_t *svga, uint8_t val, uint32_t addr);
    float (*getclock)(int clock, void *priv);
    float (*getclock8514)(int clock, void *priv);

    /* Called when VC=R18 and friends. If this returns zero then MA resetting
       is skipped. Matrox Mystique in Power mode reuses this counter for
       vertical line interrupt*/
    int (*line_compare)(struct gd5440_svga_t *svga);

    /*Called at the start of vertical sync*/
    void (*vsync_callback)(struct gd5440_svga_t *svga);

    uint32_t (*translate_address)(uint32_t addr, void *priv);
    /*If set then another device is driving the monitor output and the SVGA
      card should not attempt to display anything */
    int   override;
    void *priv;

    int vga_enabled;
    /* The PS/55 POST BIOS has a special monitor detection for its internal VGA
       when the monitor is connected to the Display Adapter. */
    int cable_connected;

    uint8_t  crtc[256];
    uint8_t  gdcreg[256];
    uint8_t  attrregs[32];
    uint8_t  seqregs[256];
    uint8_t  egapal[16];
    uint8_t *vram;
    uint8_t *changedvram;

    uint8_t crtcreg;
    uint8_t gdcaddr;
    uint8_t attrff;
    uint8_t attr_palette_enable;
    uint8_t attraddr;
    uint8_t seqaddr;
    uint8_t miscout;
    uint8_t cgastat;
    uint8_t scrblank;
    uint8_t plane_mask;
    uint8_t writemask;
    uint8_t colourcompare;
    uint8_t colournocare;
    uint8_t dac_mask;
    uint8_t dac_status;
    uint8_t dpms;
    uint8_t dpms_ui;
    uint8_t color_2bpp;
    uint8_t ksc5601_sbyte_mask;
    uint8_t ksc5601_udc_area_msb[2];

    int      ksc5601_swap_mode;
    uint16_t ksc5601_english_font_type;

    int vertical_linedbl;

    /*Used to implement CRTC[0x17] bit 2 hsync divisor*/
    int hsync_divisor;

    /*Tseng-style chain4 mode - CRTC dword mode is the same as byte mode, chain4
      addresses are shifted to match*/
    int packed_chain4;

    /*Disable 8bpp blink mode - some cards support it, some don't, it's a weird mode
      If mode 13h appears in a reddish-brown background (0x88) with dark green text (0x8F),
      you should set this flag when entering that mode*/
    int disable_blink;

    /*Force special shifter bypass logic for 8-bpp lowres modes.
      Needed if the screen is squished on certain S3 cards.*/
    int force_shifter_bypass;

    /*Force CRTC to dword mode, regardless of CR14/CR17. Required for S3 enhanced mode*/
    int force_dword_mode;

    int force_old_addr;

    int remap_required;
    uint32_t (*remap_func)(struct gd5440_svga_t *svga, uint32_t in_addr);

    void *ramdac;
    void *clock_gen;

    /* Monitor Index */
    uint8_t monitor_index;

    /* Pointer to monitor */
    monitor_t *monitor;

    /* Enable LUT mapping of >= 24 bpp modes. */
    int lut_map;

    /* Override the horizontal blanking stuff. */
    int hoverride;

    /* Return a 32 bpp color from a 15/16 bpp color. */
    uint32_t (*conv_16to32)(struct gd5440_svga_t *svga, uint16_t color, uint8_t bpp);

    void *  dev8514;
    void *  ext8514;
    void *  clock_gen8514;
    void *  xga;

    /* If set then another device is driving the monitor output and the EGA
      card should not attempt to display anything. */
    void       (*render_override)(void *priv);
    void *     priv_parent;

    void *     local;
} gd5440_svga_t;

extern void     gd5440_ibm8514_set_poll(gd5440_svga_t *svga);
extern void     gd5440_ibm8514_poll(void *priv);
extern void     gd5440_ibm8514_recalctimings(gd5440_svga_t *svga);
extern uint8_t  gd5440_ibm8514_ramdac_in(uint16_t port, void *priv);
extern void     gd5440_ibm8514_ramdac_out(uint16_t port, uint8_t val, void *priv);
extern void     gd5440_ibm8514_accel_out_fifo(gd5440_svga_t *svga, uint16_t port, uint32_t val, int len);
extern void     gd5440_ibm8514_accel_out(uint16_t port, uint32_t val, gd5440_svga_t *svga, int len);
extern uint16_t gd5440_ibm8514_accel_in_fifo(gd5440_svga_t *svga, uint16_t port, int len);
extern uint8_t  gd5440_ibm8514_accel_in(uint16_t port, gd5440_svga_t *svga);
extern int      gd5440_ibm8514_cpu_src(gd5440_svga_t *svga);
extern int      gd5440_ibm8514_cpu_dest(gd5440_svga_t *svga);
extern void     gd5440_ibm8514_accel_out_pixtrans(gd5440_svga_t *svga, uint16_t port, uint32_t val, int len);
extern void     gd5440_ibm8514_short_stroke_start(int count, int cpu_input, uint32_t mix_dat, uint32_t cpu_dat, gd5440_svga_t *svga, uint8_t ssv, int len);
extern void     gd5440_ibm8514_accel_start(int count, int cpu_input, uint32_t mix_dat, uint32_t cpu_dat, gd5440_svga_t *svga, int len);

extern void     gd5440_ati8514_out(uint16_t addr, uint8_t val, void *priv);
extern uint8_t  gd5440_ati8514_in(uint16_t addr, void *priv);
extern void     gd5440_ati8514_recalctimings(gd5440_svga_t *svga);
extern uint8_t  gd5440_ati8514_mca_read(int port, void *priv);
extern uint8_t  gd5440_ati8514_bios_rom_readb(uint32_t addr, void *priv);
extern uint16_t gd5440_ati8514_bios_rom_readw(uint32_t addr, void *priv);
extern uint32_t gd5440_ati8514_bios_rom_readl(uint32_t addr, void *priv);
extern void     gd5440_ati8514_mca_write(int port, uint8_t val, void *priv);
extern void     gd5440_ati8514_pos_write(uint16_t port, uint8_t val, void *priv);
extern void     gd5440_ati8514_init(gd5440_svga_t *svga, void *ext8514, void *dev8514);

extern void     gd5440_xga_write_test(uint32_t addr, uint8_t val, void *priv);
extern uint8_t  gd5440_xga_read_test(uint32_t addr, void *priv);
extern void     gd5440_xga_set_poll(gd5440_svga_t *svga);
extern void     gd5440_xga_poll(void *priv);
extern void     gd5440_xga_recalctimings(gd5440_svga_t *svga);

extern uint32_t gd5440_svga_decode_addr(gd5440_svga_t *svga, uint32_t addr, int write);

extern int  gd5440_svga_init(const device_t *info, gd5440_svga_t *svga, void *priv, int memsize,
                      void (*recalctimings_ex)(struct gd5440_svga_t *svga),
                      uint8_t (*gd5440_video_in)(uint16_t addr, void *priv),
                      void (*gd5440_video_out)(uint16_t addr, uint8_t val, void *priv),
                      void (*hwcursor_draw)(struct gd5440_svga_t *svga, int displine),
                      void (*overlay_draw)(struct gd5440_svga_t *svga, int displine));
extern void gd5440_svga_recalctimings(gd5440_svga_t *svga);
extern void gd5440_svga_close(gd5440_svga_t *svga);

uint8_t  gd5440_svga_read(uint32_t addr, void *priv);
uint16_t gd5440_svga_readw(uint32_t addr, void *priv);
uint32_t gd5440_svga_readl(uint32_t addr, void *priv);
void     gd5440_svga_write(uint32_t addr, uint8_t val, void *priv);
void     gd5440_svga_writew(uint32_t addr, uint16_t val, void *priv);
void     gd5440_svga_writel(uint32_t addr, uint32_t val, void *priv);
uint8_t  gd5440_svga_read_linear(uint32_t addr, void *priv);
uint8_t  gd5440_svga_readb_linear(uint32_t addr, void *priv);
uint16_t gd5440_svga_readw_linear(uint32_t addr, void *priv);
uint32_t gd5440_svga_readl_linear(uint32_t addr, void *priv);
void     gd5440_svga_write_linear(uint32_t addr, uint8_t val, void *priv);
void     gd5440_svga_writeb_linear(uint32_t addr, uint8_t val, void *priv);
void     gd5440_svga_writew_linear(uint32_t addr, uint16_t val, void *priv);
void     gd5440_svga_writel_linear(uint32_t addr, uint32_t val, void *priv);

void gd5440_svga_add_status_info(char *s, int max_len, void *priv);

extern uint8_t gd5440_svga_rotate[8][256];

void    gd5440_svga_out(uint16_t addr, uint8_t val, void *priv);
uint8_t gd5440_svga_in(uint16_t addr, void *priv);

gd5440_svga_t *gd5440_svga_get_pri(void);
void    gd5440_svga_set_override(gd5440_svga_t *svga, int val);

void gd5440_svga_set_ramdac_type(gd5440_svga_t *svga, int type);
void gd5440_svga_close(gd5440_svga_t *svga);

uint32_t gd5440_svga_mask_addr(uint32_t addr, gd5440_svga_t *svga);
uint32_t gd5440_svga_mask_changedaddr(uint32_t addr, gd5440_svga_t *svga);

void gd5440_svga_doblit(int wx, int wy, gd5440_svga_t *svga);
void gd5440_svga_set_poll(gd5440_svga_t *svga);
void gd5440_svga_poll(void *priv);

enum {
    RAMDAC_6BIT = 0,
    RAMDAC_8BIT
};

uint32_t gd5440_svga_lookup_lut_ram(gd5440_svga_t* svga, uint32_t val);

/* We need a way to add a device with a pointer to a parent device so it can attach itself to it, and
   possibly also a second ATi 68860 RAM DAC type that auto-sets SVGA render on RAM DAC render change. */
extern void    ati68860_ramdac_out(uint16_t addr, uint8_t val, int is_8514, void *priv, gd5440_svga_t *svga);
extern uint8_t ati68860_ramdac_in(uint16_t addr, int is_8514, void *priv, gd5440_svga_t *svga);
extern void    ati68860_set_ramdac_type(void *priv, int type);
extern void    ati68860_ramdac_set_render(void *priv, gd5440_svga_t *svga);
extern void    ati68860_ramdac_set_pallook(void *priv, int i, uint32_t col);
extern void    ati68860_hwcursor_draw(gd5440_svga_t *svga, int displine);

extern void    ati68875_ramdac_out(uint16_t addr, int rs2, int rs3, uint8_t val, int is_8514, void *priv, gd5440_svga_t *svga);
extern uint8_t ati68875_ramdac_in(uint16_t addr, int rs2, int rs3, int is_8514, void *priv, gd5440_svga_t *svga);

extern void    att49x_ramdac_out(uint16_t addr, int rs2, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t att49x_ramdac_in(uint16_t addr, int rs2, void *priv, gd5440_svga_t *svga);

extern void    att498_ramdac_out(uint16_t addr, int rs2, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t att498_ramdac_in(uint16_t addr, int rs2, void *priv, gd5440_svga_t *svga);
extern float   av9194_getclock(int clock, void *priv);

extern void    bt481_ramdac_out(uint16_t addr, int rs2, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t bt481_ramdac_in(uint16_t addr, int rs2, void *priv, gd5440_svga_t *svga);

extern void    bt48x_ramdac_out(uint16_t addr, int rs2, int rs3, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t bt48x_ramdac_in(uint16_t addr, int rs2, int rs3, void *priv, gd5440_svga_t *svga);
extern void    bt48x_recalctimings(void *priv, gd5440_svga_t *svga);
extern void    bt48x_hwcursor_draw(gd5440_svga_t *svga, int displine);

extern void    ibm_rgb528_ramdac_out(uint16_t addr, int rs2, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t ibm_rgb528_ramdac_in(uint16_t addr, int rs2, void *priv, gd5440_svga_t *svga);
extern void    ibm_rgb528_recalctimings(void *priv, gd5440_svga_t *svga);
extern void    ibm_rgb528_hwcursor_draw(gd5440_svga_t *svga, int displine);
extern float   ibm_rgb528_getclock(int clock, void *priv);
extern void    ibm_rgb528_ramdac_set_ref_clock(void *priv, gd5440_svga_t *svga, float ref_clock);

extern float icd2047_getclock(int clock, void *priv);

extern void  icd2061_write(void *priv, int val);
extern float icd2061_getclock(int clock, void *priv);
extern void  icd2061_set_ref_clock(void *priv, float ref_clock);

/* The code is the same, the #define's are so that the correct name can be used. */
#    define ics9161_write    icd2061_write
#    define ics9161_getclock icd2061_getclock

extern float ics1494_getclock(int clock, void *priv);

extern float ics2494_getclock(int clock, void *priv);

extern float ics90c64a_vclk_getclock(int clock, void *priv);
extern float ics90c64a_mclk_getclock(int clock, void *priv);

extern void   ics2595_write(void *priv, int strobe, int dat);
extern double ics2595_getclock(void *priv);
extern void   ics2595_setclock(void *priv, double clock);

extern void    sc1148x_ramdac_out(uint16_t addr, int rs2, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t sc1148x_ramdac_in(uint16_t addr, int rs2, void *priv, gd5440_svga_t *svga);

extern void    sc1502x_ramdac_out(uint16_t addr, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t sc1502x_ramdac_in(uint16_t addr, void *priv, gd5440_svga_t *svga);
extern void    sc1502x_rs2_ramdac_out(uint16_t addr, int rs2, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t sc1502x_rs2_ramdac_in(uint16_t addr, int rs2, void *priv, gd5440_svga_t *svga);

extern void    sdac_ramdac_out(uint16_t addr, int rs2, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t sdac_ramdac_in(uint16_t addr, int rs2, void *priv, gd5440_svga_t *svga);
extern float   sdac_getclock(int clock, void *priv);
extern void    sdac_set_ref_clock(void *priv, float ref_clock);

extern void    stg_ramdac_out(uint16_t addr, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t stg_ramdac_in(uint16_t addr, void *priv, gd5440_svga_t *svga);
extern float   stg_getclock(int clock, void *priv);

extern void    tkd8001_ramdac_out(uint16_t addr, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t tkd8001_ramdac_in(uint16_t addr, void *priv, gd5440_svga_t *svga);

extern void     tvp3026_ramdac_out(uint16_t addr, int rs2, int rs3, uint8_t val, void *priv, gd5440_svga_t *svga);
extern uint8_t  tvp3026_ramdac_in(uint16_t addr, int rs2, int rs3, void *priv, gd5440_svga_t *svga);
static uint32_t tvp3026_conv_16to32(gd5440_svga_t* svga, uint16_t color, uint8_t bpp);
extern void     tvp3026_recalctimings(void *priv, gd5440_svga_t *svga);
extern void     tvp3026_hwcursor_draw(gd5440_svga_t *svga, int displine);
extern float    tvp3026_getclock(int clock, void *priv);
extern void     tvp3026_gpio(uint8_t (*read)(uint8_t cntl, void *priv), void (*write)(uint8_t cntl, uint8_t data, void *priv), void *cb_priv, void *priv);


#define VAR_BYTE_MODE      (0 << 0)
#define VAR_WORD_MODE_MA13 (1 << 0)
#define VAR_WORD_MODE_MA15 (2 << 0)
#define VAR_DWORD_MODE     (3 << 0)
#define VAR_MODE_MASK      (3 << 0)
#define VAR_ROW0_MA13      (1 << 2)
#define VAR_ROW1_MA14      (1 << 3)

#define ADDRESS_REMAP_FUNC(nr)                                                                          \
    static uint32_t address_remap_func_##nr(gd5440_svga_t *svga, uint32_t in_addr)                             \
    {                                                                                                   \
        uint32_t out_addr;                                                                              \
                                                                                                        \
        switch (nr & VAR_MODE_MASK) {                                                                   \
            case VAR_BYTE_MODE:                                                                         \
                out_addr = in_addr;                                                                     \
                break;                                                                                  \
                                                                                                        \
            case VAR_WORD_MODE_MA13:                                                                    \
                out_addr = ((in_addr << 1) & 0x1fff8) | ((in_addr >> 13) & 0x4) | (in_addr & ~0x1ffff); \
                break;                                                                                  \
                                                                                                        \
            case VAR_WORD_MODE_MA15:                                                                    \
                out_addr = ((in_addr << 1) & 0x1fff8) | ((in_addr >> 15) & 0x4) | (in_addr & ~0x1ffff); \
                break;                                                                                  \
                                                                                                        \
            case VAR_DWORD_MODE:                                                                        \
                out_addr = ((in_addr << 2) & 0x3fff0) | ((in_addr >> 14) & 0xc) | (in_addr & ~0x3ffff); \
                break;                                                                                  \
        }                                                                                               \
                                                                                                        \
        if (nr & VAR_ROW0_MA13)                                                                         \
            out_addr = (out_addr & ~0x8000) | ((svga->scanline & 1) ? 0x8000 : 0);                            \
        if (nr & VAR_ROW1_MA14)                                                                         \
            out_addr = (out_addr & ~0x10000) | ((svga->scanline & 2) ? 0x10000 : 0);                          \
                                                                                                        \
        return out_addr;                                                                                \
    }

ADDRESS_REMAP_FUNC(0)
ADDRESS_REMAP_FUNC(1)
ADDRESS_REMAP_FUNC(2)
ADDRESS_REMAP_FUNC(3)
ADDRESS_REMAP_FUNC(4)
ADDRESS_REMAP_FUNC(5)
ADDRESS_REMAP_FUNC(6)
ADDRESS_REMAP_FUNC(7)
ADDRESS_REMAP_FUNC(8)
ADDRESS_REMAP_FUNC(9)
ADDRESS_REMAP_FUNC(10)
ADDRESS_REMAP_FUNC(11)
ADDRESS_REMAP_FUNC(12)
ADDRESS_REMAP_FUNC(13)
ADDRESS_REMAP_FUNC(14)
ADDRESS_REMAP_FUNC(15)

static uint32_t (*address_remap_funcs[16])(gd5440_svga_t *svga, uint32_t in_addr) = {
    address_remap_func_0,
    address_remap_func_1,
    address_remap_func_2,
    address_remap_func_3,
    address_remap_func_4,
    address_remap_func_5,
    address_remap_func_6,
    address_remap_func_7,
    address_remap_func_8,
    address_remap_func_9,
    address_remap_func_10,
    address_remap_func_11,
    address_remap_func_12,
    address_remap_func_13,
    address_remap_func_14,
    address_remap_func_15
};

void
gd5440_svga_recalc_remap_func(gd5440_svga_t *svga)
{
    int func_nr;

    if (svga->fb_only)
        func_nr = 0;
    else {
        if (svga->force_dword_mode)
            func_nr = VAR_DWORD_MODE;
        else if (svga->crtc[0x14] & 0x40)
            func_nr = svga->packed_chain4 ? VAR_BYTE_MODE : VAR_DWORD_MODE;
        else if (svga->crtc[0x17] & 0x40)
            func_nr = VAR_BYTE_MODE;
        else if (svga->crtc[0x17] & 0x20)
            func_nr = VAR_WORD_MODE_MA15;
        else
            func_nr = VAR_WORD_MODE_MA13;

        if (!(svga->crtc[0x17] & 0x01))
            func_nr |= VAR_ROW0_MA13;
        if (!(svga->crtc[0x17] & 0x02))
            func_nr |= VAR_ROW1_MA14;
    }

    svga->remap_required = (func_nr != 0);
    svga->remap_func     = address_remap_funcs[func_nr];
}


uint32_t
gd5440_svga_lookup_lut_ram(gd5440_svga_t* svga, uint32_t val)
{
    if (!svga->lut_map)
        return val;

    uint8_t r = getcolr(svga->pallook[getcolr(val)]);
    uint8_t g = getcolg(svga->pallook[getcolg(val)]);
    uint8_t b = getcolb(svga->pallook[getcolb(val)]);
    return makecol32(r, g, b) | (val & 0xFF000000);
}

#define lookup_lut(val) gd5440_svga_lookup_lut_ram(svga, val)

void
gd5440_svga_render_null(gd5440_svga_t *svga)
{
    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->firstline_draw == 2000)
        svga->firstline_draw = svga->displine;
    svga->lastline_draw = svga->displine;
}

void
gd5440_svga_render_blank(gd5440_svga_t *svga)
{
    if ((svga->displine + svga->y_add) < 0 || (svga->displine + svga->y_add) >= 2048)
        return;

    if (svga->firstline_draw == 2000)
        svga->firstline_draw = svga->displine;
    svga->lastline_draw = svga->displine;

    uint32_t char_width = 0;

    switch (svga->seqregs[1] & 9) {
        case 0:
            char_width = 9;
            break;
        case 1:
            char_width = 8;
            break;
        case 8:
            char_width = 18;
            break;
        case 9:
            char_width = 16;
            break;

        default:
            break;
    }

    uint32_t *line_ptr   = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];
    int32_t   line_width = (uint32_t) (svga->hdisp + svga->scrollcache) * char_width * sizeof(uint32_t);

    if (svga->x_add < 0) {
        line_ptr = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][0];
        line_width -= svga->x_add;
    }

    if ((line_ptr != NULL) && ((svga->hdisp + svga->scrollcache) > 0) && (line_width >= 0))
        memset(line_ptr, 0, line_width);
}

void
gd5440_svga_render_overscan_left(gd5440_svga_t *svga)
{
    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->scrblank || (svga->hdisp <= 0))
        return;

    uint32_t *line_ptr = svga->monitor->target_buffer->line[svga->displine + svga->y_add];

    if ((line_ptr != NULL) && (svga->x_add >= 0))  for (int i = 0; i < svga->x_add; i++)
        *line_ptr++ = svga->overscan_color;
}

void
gd5440_svga_render_overscan_right(gd5440_svga_t *svga)
{
    int right;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->scrblank || (svga->hdisp <= 0))
        return;

    uint32_t *line_ptr = svga->monitor->target_buffer->line[svga->displine + svga->y_add];
    right              = overscan_x  - svga->left_overscan;

    if (line_ptr != NULL) {
        line_ptr += svga->x_add + svga->hdisp;

        for (int i = 0; i < right; i++)
            *line_ptr++ = svga->overscan_color;
    }
}

void
gd5440_svga_render_text_40(gd5440_svga_t *svga)
{
    uint32_t *p;
    int       xx;
    int       drawcursor;
    int       xinc;
    uint8_t   chr;
    uint8_t   attr;
    uint8_t   dat;
    uint32_t  charaddr;
    int       fg;
    int       bg;
    uint32_t  addr = 0;

    if (svga->render_override) {
        svga->render_override(svga->priv_parent);
        return;
    }

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->firstline_draw == 2000)
        svga->firstline_draw = svga->displine;
    svga->lastline_draw = svga->displine;

    if (svga->fullchange) {
        p    = &svga->monitor->target_buffer->line[(svga->displine + svga->y_add) & 2047][(svga->x_add) & 2047];
        xinc = (svga->seqregs[1] & 1) ? 16 : 18;

        for (int x = 0; x < (svga->hdisp + svga->scrollcache); x += xinc) {
            if (!svga->force_old_addr)
                addr = svga->remap_func(svga, svga->memaddr) & svga->vram_display_mask;

            drawcursor = ((svga->memaddr == svga->cursoraddr) && svga->cursorvisible && svga->cursoron);

            if (svga->force_old_addr) {
                chr  = svga->vram[(svga->memaddr << 1) & svga->vram_display_mask];
                attr = svga->vram[((svga->memaddr << 1) + 1) & svga->vram_display_mask];
            } else {
                chr  = svga->vram[addr];
                attr = svga->vram[addr + 1];
            }

            if (attr & 8)
                charaddr = svga->charsetb + (chr * 128);
            else
                charaddr = svga->charseta + (chr * 128);

            if (drawcursor) {
                bg = svga->pallook[svga->egapal[attr & 15] & svga->dac_mask];
                fg = svga->pallook[svga->egapal[attr >> 4] & svga->dac_mask];
            } else {
                fg = svga->pallook[svga->egapal[attr & 15] & svga->dac_mask];
                bg = svga->pallook[svga->egapal[attr >> 4] & svga->dac_mask];

                if (attr & 0x80 && svga->attrregs[0x10] & 8) {
                    bg = svga->pallook[svga->egapal[(attr >> 4) & 7] & svga->dac_mask];
                    if (svga->blink & 16)
                        fg = bg;
                }
            }

            dat = svga->vram[charaddr + (svga->scanline << 2)];
            if (svga->seqregs[1] & 1) {
                for (xx = 0; xx < 16; xx += 2)
                    p[xx] = p[xx + 1] = (dat & (0x80 >> (xx >> 1))) ? fg : bg;
            } else {
                for (xx = 0; xx < 16; xx += 2)
                    p[xx] = p[xx + 1] = (dat & (0x80 >> (xx >> 1))) ? fg : bg;
                if ((chr & ~0x1f) != 0xc0 || !(svga->attrregs[0x10] & 4))
                    p[16] = p[17] = bg;
                else
                    p[16] = p[17] = (dat & 1) ? fg : bg;
            }
            svga->memaddr += 4;
            p += xinc;
        }
        svga->memaddr &= svga->vram_display_mask;
    }
}

void
gd5440_svga_render_text_80(gd5440_svga_t *svga)
{
    uint32_t *p;
    int       xx;
    int       drawcursor;
    int       xinc;
    uint8_t   chr;
    uint8_t   attr;
    uint8_t   dat;
    uint32_t  charaddr;
    int       fg;
    int       bg;
    uint32_t  addr = 0;

    if (svga->render_override) {
        svga->render_override(svga->priv_parent);
        return;
    }

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->firstline_draw == 2000)
        svga->firstline_draw = svga->displine;
    svga->lastline_draw = svga->displine;

    if (svga->fullchange) {
        p    = &svga->monitor->target_buffer->line[(svga->displine + svga->y_add) & 2047][(svga->x_add) & 2047];
        xinc = (svga->seqregs[1] & 1) ? 8 : 9;

        static uint32_t col = 0x00000000;

        for (int x = 0; x < (svga->hdisp + svga->scrollcache); x += xinc) {
            if (!svga->force_old_addr)
                addr = svga->remap_func(svga, svga->memaddr) & svga->vram_display_mask;

            drawcursor = ((svga->memaddr == svga->cursoraddr) && svga->cursorvisible && svga->cursoron);

            if (svga->force_old_addr) {
                chr  = svga->vram[(svga->memaddr << 1) & svga->vram_display_mask];
                attr = svga->vram[((svga->memaddr << 1) + 1) & svga->vram_display_mask];
            } else {
                chr  = svga->vram[addr];
                attr = svga->vram[addr + 1];
            }

            if (attr & 8)
                charaddr = svga->charsetb + (chr * 128);
            else
                charaddr = svga->charseta + (chr * 128);

            if (drawcursor) {
                bg = attr & 15;
                fg = attr >> 4;
            } else {
                fg = attr & 15;
                bg = attr >> 4;
                if (attr & 0x80 && svga->attrregs[0x10] & 8) {
                    bg = (attr >> 4) & 7;
                    if (svga->blink & 16)
                        fg = bg;
                }
            }

            dat = svga->vram[charaddr + (svga->scanline << 2)];

            if (svga->attrregs[0x10] & 0x40) {
                if (svga->seqregs[1] & 1) {
                    for (xx = 0; xx < 8; xx++) {
                        uint32_t col16 = (dat & (0x80 >> xx)) ? fg : bg;
                        if ((x + xx - svga->scrollcache) & 1) {
                            col |= col16;
                            if ((x + xx - 1) >= 0)
                                p[xx - 1] = svga->pallook[col & svga->dac_mask];
                            if ((x + xx) >= 0)
                                p[xx] = svga->pallook[col & svga->dac_mask];
                        } else
                           col = col16 << 4;
                    }
                } else {
                    for (xx = 0; xx < 9; xx++) {
                        uint32_t col16;
                        if (xx < 8)
                            col16 = (dat & (0x80 >> xx)) ? fg : bg;
                        else if ((chr & ~0x1F) != 0xC0 || !(svga->attrregs[0x10] & 4))
                            col16 = bg;
                        else
                            col16 = (dat & 1) ? fg : bg;
                        if ((x + xx - svga->scrollcache) & 1) {
                            col |= col16;
                            if ((x + xx - 1) >= 0)
                                p[xx - 1] = svga->pallook[col & svga->dac_mask];
                            if ((x + xx) >= 0)
                                p[xx] = svga->pallook[col & svga->dac_mask];
                        } else
                           col = col16 << 4;
                    }
                }
            } else {
                if (svga->seqregs[1] & 1) {
                    for (xx = 0; xx < 8; xx++) {
                        col = (col << 4) | ((dat & (0x80 >> xx)) ? fg : bg);
                        p[xx] = svga->pallook[svga->egapal[col & 0x0f] & svga->dac_mask];
                    }
                } else {
                    for (xx = 0; xx < 8; xx++) {
                        col = (col << 4) | ((dat & (0x80 >> xx)) ? fg : bg);
                        p[xx] = svga->pallook[svga->egapal[col & 0x0f] & svga->dac_mask];
                    }
                    if ((chr & ~0x1F) != 0xC0 || !(svga->attrregs[0x10] & 4))
                        col = (col << 4) | bg;
                    else
                        col = (col << 4) | ((dat & 1) ? fg : bg);
                    p[8] = svga->pallook[svga->egapal[col & 0x0f] & svga->dac_mask];
                }
            }

            svga->memaddr += 4;
            p += xinc;
        }
        svga->memaddr &= svga->vram_display_mask;
    }
}

/*Not available on most generic cards.*/
void
gd5440_svga_render_text_80_ksc5601(gd5440_svga_t *svga)
{
    uint32_t *p;
    int       xx;
    int       drawcursor;
    int       xinc;
    uint8_t   chr;
    uint8_t   attr;
    uint8_t   dat;
    uint8_t   nextchr;
    uint32_t  charaddr;
    int       fg;
    int       bg;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->firstline_draw == 2000)
        svga->firstline_draw = svga->displine;
    svga->lastline_draw = svga->displine;

    if (svga->fullchange) {
        p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

        xinc = (svga->seqregs[1] & 1) ? 8 : 9;

        for (int x = 0; x < (svga->hdisp + svga->scrollcache); x += xinc) {
            uint32_t addr = svga->remap_func(svga, svga->memaddr) & svga->vram_display_mask;
            drawcursor    = ((svga->memaddr == svga->cursoraddr) && svga->cursorvisible && svga->cursoron);
            chr           = svga->vram[addr];
            nextchr       = svga->vram[addr + 8];
            attr          = svga->vram[addr + 1];

            if (drawcursor) {
                bg = svga->pallook[svga->egapal[attr & 15] & svga->dac_mask];
                fg = svga->pallook[svga->egapal[attr >> 4] & svga->dac_mask];
            } else {
                fg = svga->pallook[svga->egapal[attr & 15] & svga->dac_mask];
                bg = svga->pallook[svga->egapal[attr >> 4] & svga->dac_mask];
                if (attr & 0x80 && svga->attrregs[0x10] & 8) {
                    bg = svga->pallook[svga->egapal[(attr >> 4) & 7] & svga->dac_mask];
                    if (svga->blink & 16)
                        fg = bg;
                }
            }

            if ((x + xinc) < svga->hdisp && (chr & (nextchr | svga->ksc5601_sbyte_mask) & 0x80)) {
                if ((chr == svga->ksc5601_udc_area_msb[0] || chr == svga->ksc5601_udc_area_msb[1]) && (nextchr > 0xa0 && nextchr < 0xff))
                    dat = gd5440_fontdatksc5601_user[(chr == svga->ksc5601_udc_area_msb[1] ? 96 : 0) + (nextchr & 0x7F) - 0x20].chr[svga->scanline];
                else if (nextchr & 0x80) {
                    if (svga->ksc5601_swap_mode == 1 && (nextchr > 0xa0 && nextchr < 0xff)) {
                        if (chr >= 0x80 && chr < 0x99)
                            chr += 0x30;
                        else if (chr >= 0xB0 && chr < 0xC9)
                            chr -= 0x30;
                    }
                    dat = gd5440_fontdatksc5601[((chr & 0x7F) << 7) | (nextchr & 0x7F)].chr[svga->scanline];
                } else
                    dat = 0xff;
            } else {
                if (attr & 8)
                    charaddr = svga->charsetb + (chr * 128);
                else
                    charaddr = svga->charseta + (chr * 128);

                if ((svga->ksc5601_english_font_type >> 8) == 1)
                    dat = gd5440_fontdatksc5601[((svga->ksc5601_english_font_type & 0x7F) << 7) | (chr >> 1)].chr[((chr & 1) << 4) | svga->scanline];
                else
                    dat = svga->vram[charaddr + (svga->scanline << 2)];
            }

            if (svga->seqregs[1] & 1) {
                for (xx = 0; xx < 8; xx++)
                    p[xx] = (dat & (0x80 >> xx)) ? fg : bg;
            } else {
                for (xx = 0; xx < 8; xx++)
                    p[xx] = (dat & (0x80 >> xx)) ? fg : bg;
                if (((chr & ~0x1f) != 0xc0) || !(svga->attrregs[0x10] & 4))
                    p[8] = bg;
                else
                    p[8] = (dat & 1) ? fg : bg;
            }
            svga->memaddr += 4;
            p += xinc;

            if ((x + xinc) < svga->hdisp && (chr & (nextchr | svga->ksc5601_sbyte_mask) & 0x80)) {
                attr = svga->vram[((svga->memaddr << 1) + 1) & svga->vram_display_mask];

                if (drawcursor) {
                    bg = svga->pallook[svga->egapal[attr & 15] & svga->dac_mask];
                    fg = svga->pallook[svga->egapal[attr >> 4] & svga->dac_mask];
                } else {
                    fg = svga->pallook[svga->egapal[attr & 15] & svga->dac_mask];
                    bg = svga->pallook[svga->egapal[attr >> 4] & svga->dac_mask];
                    if (attr & 0x80 && svga->attrregs[0x10] & 8) {
                        bg = svga->pallook[svga->egapal[(attr >> 4) & 7] & svga->dac_mask];
                        if (svga->blink & 16)
                            fg = bg;
                    }
                }

                if ((chr == svga->ksc5601_udc_area_msb[0] || chr == svga->ksc5601_udc_area_msb[1]) && (nextchr > 0xa0 && nextchr < 0xff))
                    dat = gd5440_fontdatksc5601_user[(chr == svga->ksc5601_udc_area_msb[1] ? 96 : 0) + (nextchr & 0x7F) - 0x20].chr[svga->scanline + 16];
                else if (nextchr & 0x80)
                    dat = gd5440_fontdatksc5601[((chr & 0x7f) << 7) | (nextchr & 0x7F)].chr[svga->scanline + 16];
                else
                    dat = 0xff;

                if (svga->seqregs[1] & 1) {
                    for (xx = 0; xx < 8; xx++)
                        p[xx] = (dat & (0x80 >> xx)) ? fg : bg;
                } else {
                    for (xx = 0; xx < 8; xx++)
                        p[xx] = (dat & (0x80 >> xx)) ? fg : bg;
                    if (((chr & ~0x1f) != 0xc0) || !(svga->attrregs[0x10] & 4))
                        p[8] = bg;
                    else
                        p[8] = (dat & 1) ? fg : bg;
                }

                svga->memaddr += 4;
                p += xinc;
                x += xinc;
            }
        }
        svga->memaddr &= svga->vram_display_mask;
    }
}

void
gd5440_svga_render_2bpp_s3_lowres(gd5440_svga_t *svga)
{
    int       changed_offset;
    int       x;
    uint8_t   dat[2];
    uint32_t  addr;
    uint32_t *p;
    uint32_t  changed_addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        changed_offset = ((svga->memaddr << 1) + (svga->scanline & ~svga->crtc[0x17] & 3) * 0x8000) >> 12;

        if (svga->changedvram[changed_offset] || svga->changedvram[changed_offset + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 16) {
                addr = svga->memaddr;

                if (!(svga->crtc[0x17] & 0x40)) {
                    addr = (addr << 1) & svga->vram_mask;
                    addr &= ~7;

                    if ((svga->crtc[0x17] & 0x20) && (svga->memaddr & 0x20000))
                        addr |= 4;

                    if (!(svga->crtc[0x17] & 0x20) && (svga->memaddr & 0x8000))
                        addr |= 4;
                }

                if (!(svga->crtc[0x17] & 0x01))
                    addr = (addr & ~0x8000) | ((svga->scanline & 1) ? 0x8000 : 0);

                if (!(svga->crtc[0x17] & 0x02))
                    addr = (addr & ~0x10000) | ((svga->scanline & 2) ? 0x10000 : 0);

                dat[0] = svga->vram[addr];
                dat[1] = svga->vram[addr | 0x1];
                if (svga->seqregs[1] & 4)
                    svga->memaddr += 2;
                else
                    svga->memaddr += 4;
                svga->memaddr &= svga->vram_mask;
                p[0] = p[1] = svga->pallook[svga->egapal[(dat[0] >> 6) & 3] & svga->dac_mask];
                p[2] = p[3] = svga->pallook[svga->egapal[(dat[0] >> 4) & 3] & svga->dac_mask];
                p[4] = p[5] = svga->pallook[svga->egapal[(dat[0] >> 2) & 3] & svga->dac_mask];
                p[6] = p[7] = svga->pallook[svga->egapal[dat[0] & 3] & svga->dac_mask];
                p[8] = p[9] = svga->pallook[svga->egapal[(dat[1] >> 6) & 3] & svga->dac_mask];
                p[10] = p[11] = svga->pallook[svga->egapal[(dat[1] >> 4) & 3] & svga->dac_mask];
                p[12] = p[13] = svga->pallook[svga->egapal[(dat[1] >> 2) & 3] & svga->dac_mask];
                p[14] = p[15] = svga->pallook[svga->egapal[dat[1] & 3] & svga->dac_mask];
                p += 16;
            }
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 16) {
                addr = svga->remap_func(svga, svga->memaddr);

                dat[0] = svga->vram[addr];
                dat[1] = svga->vram[addr | 0x1];
                if (svga->seqregs[1] & 4)
                    svga->memaddr += 2;
                else
                    svga->memaddr += 4;

                svga->memaddr &= svga->vram_mask;

                p[0] = p[1] = svga->pallook[svga->egapal[(dat[0] >> 6) & 3] & svga->dac_mask];
                p[2] = p[3] = svga->pallook[svga->egapal[(dat[0] >> 4) & 3] & svga->dac_mask];
                p[4] = p[5] = svga->pallook[svga->egapal[(dat[0] >> 2) & 3] & svga->dac_mask];
                p[6] = p[7] = svga->pallook[svga->egapal[dat[0] & 3] & svga->dac_mask];
                p[8] = p[9] = svga->pallook[svga->egapal[(dat[1] >> 6) & 3] & svga->dac_mask];
                p[10] = p[11] = svga->pallook[svga->egapal[(dat[1] >> 4) & 3] & svga->dac_mask];
                p[12] = p[13] = svga->pallook[svga->egapal[(dat[1] >> 2) & 3] & svga->dac_mask];
                p[14] = p[15] = svga->pallook[svga->egapal[dat[1] & 3] & svga->dac_mask];

                p += 16;
            }
        }
    }
}

void
gd5440_svga_render_2bpp_s3_highres(gd5440_svga_t *svga)
{
    int       changed_offset;
    int       x;
    uint8_t   dat[2];
    uint32_t  addr;
    uint32_t *p;
    uint32_t  changed_addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        changed_offset = ((svga->memaddr << 1) + (svga->scanline & ~svga->crtc[0x17] & 3) * 0x8000) >> 12;

        if (svga->changedvram[changed_offset] || svga->changedvram[changed_offset + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                addr = svga->memaddr;

                if (!(svga->crtc[0x17] & 0x40)) {
                    addr = (addr << 1) & svga->vram_mask;
                    addr &= ~7;

                    if ((svga->crtc[0x17] & 0x20) && (svga->memaddr & 0x20000))
                        addr |= 4;

                    if (!(svga->crtc[0x17] & 0x20) && (svga->memaddr & 0x8000))
                        addr |= 4;
                }

                if (!(svga->crtc[0x17] & 0x01))
                    addr = (addr & ~0x8000) | ((svga->scanline & 1) ? 0x8000 : 0);

                if (!(svga->crtc[0x17] & 0x02))
                    addr = (addr & ~0x10000) | ((svga->scanline & 2) ? 0x10000 : 0);

                dat[0] = svga->vram[addr];
                dat[1] = svga->vram[addr | 0x1];
                if (svga->seqregs[1] & 4)
                    svga->memaddr += 2;
                else
                    svga->memaddr += 4;
                svga->memaddr &= svga->vram_mask;
                p[0] = svga->pallook[svga->egapal[(dat[0] >> 6) & 3] & svga->dac_mask];
                p[1] = svga->pallook[svga->egapal[(dat[0] >> 4) & 3] & svga->dac_mask];
                p[2] = svga->pallook[svga->egapal[(dat[0] >> 2) & 3] & svga->dac_mask];
                p[3] = svga->pallook[svga->egapal[dat[0] & 3] & svga->dac_mask];
                p[4] = svga->pallook[svga->egapal[(dat[1] >> 6) & 3] & svga->dac_mask];
                p[5] = svga->pallook[svga->egapal[(dat[1] >> 4) & 3] & svga->dac_mask];
                p[6] = svga->pallook[svga->egapal[(dat[1] >> 2) & 3] & svga->dac_mask];
                p[7] = svga->pallook[svga->egapal[dat[1] & 3] & svga->dac_mask];
                p += 8;
            }
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                addr = svga->remap_func(svga, svga->memaddr);

                dat[0] = svga->vram[addr];
                dat[1] = svga->vram[addr | 0x1];
                if (svga->seqregs[1] & 4)
                    svga->memaddr += 2;
                else
                    svga->memaddr += 4;

                svga->memaddr &= svga->vram_mask;

                p[0] = svga->pallook[svga->egapal[(dat[0] >> 6) & 3] & svga->dac_mask];
                p[1] = svga->pallook[svga->egapal[(dat[0] >> 4) & 3] & svga->dac_mask];
                p[2] = svga->pallook[svga->egapal[(dat[0] >> 2) & 3] & svga->dac_mask];
                p[3] = svga->pallook[svga->egapal[dat[0] & 3] & svga->dac_mask];
                p[4] = svga->pallook[svga->egapal[(dat[1] >> 6) & 3] & svga->dac_mask];
                p[5] = svga->pallook[svga->egapal[(dat[1] >> 4) & 3] & svga->dac_mask];
                p[6] = svga->pallook[svga->egapal[(dat[1] >> 2) & 3] & svga->dac_mask];
                p[7] = svga->pallook[svga->egapal[dat[1] & 3] & svga->dac_mask];

                p += 8;
            }
        }
    }
}

void
gd5440_svga_render_2bpp_headland_highres(gd5440_svga_t *svga)
{
    int       oddeven;
    uint32_t  addr;
    uint32_t *p;
    uint8_t   edat[4];
    uint8_t   dat;
    uint32_t  changed_addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    changed_addr = svga->remap_func(svga, svga->memaddr);

    if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
        p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

        if (svga->firstline_draw == 2000)
            svga->firstline_draw = svga->displine;
        svga->lastline_draw = svga->displine;

        for (int x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
            addr    = svga->remap_func(svga, svga->memaddr);
            oddeven = 0;

            if (svga->seqregs[1] & 4) {
                oddeven = (addr & 4) ? 1 : 0;
                edat[0] = svga->vram[addr | oddeven];
                edat[2] = svga->vram[addr | oddeven | 0x2];
                edat[1] = edat[3] = 0;
            } else {
                gd5440_write_le32(&edat[0], gd5440_read_le32(&svga->vram[addr]));
            }
            svga->memaddr += 4;
            svga->memaddr &= svga->vram_mask;

            dat  = gd5440_edatlookup[edat[0] >> 6][edat[1] >> 6] | (gd5440_edatlookup[edat[2] >> 6][edat[3] >> 6] << 2);
            p[0] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask] & svga->dac_mask];
            p[1] = svga->pallook[svga->egapal[dat & svga->plane_mask] & svga->dac_mask];
            dat  = gd5440_edatlookup[(edat[0] >> 4) & 3][(edat[1] >> 4) & 3] | (gd5440_edatlookup[(edat[2] >> 4) & 3][(edat[3] >> 4) & 3] << 2);
            p[2] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask] & svga->dac_mask];
            p[3] = svga->pallook[svga->egapal[dat & svga->plane_mask] & svga->dac_mask];
            dat  = gd5440_edatlookup[(edat[0] >> 2) & 3][(edat[1] >> 2) & 3] | (gd5440_edatlookup[(edat[2] >> 2) & 3][(edat[3] >> 2) & 3] << 2);
            p[4] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask] & svga->dac_mask];
            p[5] = svga->pallook[svga->egapal[dat & svga->plane_mask] & svga->dac_mask];
            dat  = gd5440_edatlookup[edat[0] & 3][edat[1] & 3] | (gd5440_edatlookup[edat[2] & 3][edat[3] & 3] << 2);
            p[6] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask] & svga->dac_mask];
            p[7] = svga->pallook[svga->egapal[dat & svga->plane_mask] & svga->dac_mask];

            p += 8;
        }
    }
}

static void
gd5440_svga_render_indexed_gfx(gd5440_svga_t *svga, bool highres, bool combine8bits)
{
    int       x;
    uint32_t  addr;
    uint32_t *p;
    uint32_t  changed_offset;

    const bool blinked   = !!(svga->blink & 0x10);
    const bool attrblink = (!svga->disable_blink) && ((svga->attrregs[0x10] & 0x08) != 0);

    /*
       The following is likely how it works on an IBM VGA - that is, it works with its BIOS.
       But on some cards, certain modes are broken.
       - S3 Trio: mode 13h (320x200x8), incbypow2 given as 2 treated as 0
       - ET4000/W32i: mode 2Eh (640x480x8), incevery given as 2 treated as 1
     */
    const bool forcepacked = combine8bits && (svga->force_old_addr || svga->packed_chain4);

    /*
       SVGA cards with a high-resolution 8bpp mode may actually bypass the VGA shifter logic.
       - HT-216 (+ other Video7 chipsets?) has 0x3C4.0xC8 bit 4 which, when set to 1, loads
         bytes directly, bypassing the shifters.
     */
    const bool highres8bpp = (combine8bits && highres) || svga->force_shifter_bypass;

    const bool     dwordload  = ((svga->seqregs[0x01] & 0x10) != 0);
    const bool     wordload   = ((svga->seqregs[0x01] & 0x04) != 0) && !dwordload;
    const bool     wordincr   = ((svga->crtc[0x17] & 0x08) != 0);
    const bool     dwordincr  = ((svga->crtc[0x14] & 0x20) != 0) && !wordincr;
    const bool     dwordshift = ((svga->crtc[0x14] & 0x40) != 0);
    const bool     wordshift  = ((svga->crtc[0x17] & 0x40) == 0) && !dwordshift;
    const uint32_t incbypow2  = forcepacked ? 0 : (dwordshift ? 2 : wordshift ? 1 : 0);
    const uint32_t incevery   = forcepacked ? 1 : (dwordincr ? 4 : wordincr ? 2 : 1);
    const uint32_t loadevery  = forcepacked ? 1 : (dwordload ? 4 : wordload ? 2 : 1);

    const bool shift4bit = ((svga->gdcreg[0x05] & 0x40) == 0x40) || highres8bpp;
    const bool shift2bit = (((svga->gdcreg[0x05] & 0x60) == 0x20) && !shift4bit);

    const int      dwshift   = highres ? 0 : 1;
    const int      dotwidth  = 1 << dwshift;
    const int      charwidth = dotwidth * ((combine8bits && !svga->packed_4bpp) ? 4 : 8);
    const uint32_t planemask = 0x11111111 * (uint32_t) (svga->plane_mask);
    const uint32_t blinkmask = (attrblink ? 0x88888888 : 0x0);
    const uint32_t blinkval  = (attrblink && blinked ? 0x88888888 : 0x0);

    /*
       This is actually a 8x 3-bit lookup table,
       preshifted by 2 bits to allow shifting by multiples of 4 bits.

       Anyway, when we perform a planar-to-chunky conversion,
       we keep the pixel values in a scrambled order.
       This lookup table unscrambles them.

       WARNING: Octal values are used here!
     */
    const uint32_t shift_values = (shift4bit
                                       ? ((067452301) << 2)
                                       : shift2bit
                                       ? ((026370415) << 2)
                                       : ((002461357) << 2));

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr)
        changed_offset = (svga->memaddr + (svga->scanline & ~svga->crtc[0x17] & 3) * 0x8000) >> 12;
    else
        changed_offset = svga->remap_func(svga, svga->memaddr) >> 12;

    if (!(svga->changedvram[changed_offset] || svga->changedvram[changed_offset + 1] || svga->fullchange))
        return;
    p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

    if (svga->render_line_offset) {
        if (svga->render_line_offset > 0) {
            memset(p, svga->overscan_color, (size_t) charwidth * svga->render_line_offset * sizeof(uint32_t));
            p += charwidth * svga->render_line_offset;
        }
    }

    if (svga->firstline_draw == 2000)
        svga->firstline_draw = svga->displine;
    svga->lastline_draw = svga->displine;

    uint32_t incr_counter = 0;
    uint32_t load_counter = 0;
    uint32_t edat         = 0;
    static uint32_t col          = 0;
    static uint32_t col2         = 0;
    for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += charwidth) {
        if (load_counter == 0) {
            /* Find our address */
            if (svga->force_old_addr) {
                addr = ((svga->memaddr & ~0x3) << incbypow2);

                if (incbypow2 == 2) {
                    if (svga->memaddr & (4 << 15))
                        addr |= 0x8;
                    if (svga->memaddr & (4 << 14))
                        addr |= 0x4;
                } else if (incbypow2 == 1) {
                    if ((svga->crtc[0x17] & 0x20)) {
                        if (svga->memaddr & (4 << 15))
                            addr |= 0x4;
                    } else {
                        if (svga->memaddr & (4 << 13))
                            addr |= 0x4;
                    }
                } else {
                    /* Nothing */
                }

                if (!(svga->crtc[0x17] & 0x01))
                    addr = (addr & ~0x8000) | ((svga->scanline & 1) ? 0x8000 : 0);
                if (!(svga->crtc[0x17] & 0x02))
                    addr = (addr & ~0x10000) | ((svga->scanline & 2) ? 0x10000 : 0);
            } else if (svga->remap_required)
                addr = svga->remap_func(svga, svga->memaddr);
            else
                addr = svga->memaddr;

            addr &= svga->vram_display_mask;

            /* Load VRAM */
            edat = gd5440_read_le32(&svga->vram[addr]);

            /*
               EGA and VGA actually use 4bpp planar as its native format.
               But 4bpp chunky is generally easier to deal with on a modern CPU.
               shift4bit is the native format for this renderer (4bpp chunky).
             */
            if (svga->ati_4color || !shift4bit) {
                if (shift2bit && !svga->ati_4color) {
                    /* Group 2x 2bpp values into 4bpp values */
                    edat = (edat & 0xCCCC3333) | ((edat << 14) & 0x33330000) | ((edat >> 14) & 0x0000CCCC);
                } else {
                    /* Group 4x 1bpp values into 4bpp values */
                    edat = (edat & 0xAA55AA55) | ((edat << 7) & 0x55005500) | ((edat >> 7) & 0x00AA00AA);
                    edat = (edat & 0xCCCC3333) | ((edat << 14) & 0x33330000) | ((edat >> 14) & 0x0000CCCC);
                }
            }
        } else {
            /*
               According to the 82C451 VGA clone chipset datasheet, all 4 planes chain in a ring.
               So, rotate them all around.
               Planar version: edat = (edat >> 8) | (edat << 24);
               Here's the chunky version...
             */
            edat = ((edat >> 1) & 0x77777777) | ((edat << 3) & 0x88888888);
        }
        load_counter += 1;
        if (load_counter >= loadevery)
            load_counter = 0;

        incr_counter += 1;
        if (incr_counter >= incevery) {
            incr_counter = 0;
            svga->memaddr += 4;
            /* DISCREPANCY TODO FIXME 2/4bpp used vram_mask, 8bpp used vram_display_mask --GM */
            svga->memaddr &= svga->vram_display_mask;
        }

        uint32_t current_shift = shift_values;
        uint32_t out_edat      = edat;
        /*
           Apply blink
           FIXME: Confirm blink behaviour on real hardware

           The VGA 4bpp graphics blink logic was a pain to work out.

           If plane 3 is enabled in the attribute controller, then:
           - if bit 3 is 0, then we force the output of it to be 1.
           - if bit 3 is 1, then the output blinks.
           This can be tested with Lotus 1-2-3 release 2.3 with the WYSIWYG addon.

           If plane 3 is disabled in the attribute controller, then the output blinks.
           This can be tested with QBASIC SCREEN 10 - anything using color #2 should
           blink and nothing else.

           If you can simplify the following and have it still work, give yourself a medal.
         */
        out_edat = ((out_edat & planemask & ~blinkmask) | ((out_edat | ~planemask) & blinkmask & blinkval)) ^ blinkmask;

        for (int i = 0; i < (8 + (svga->ati_4color ? 8 : 0)); i += (svga->ati_4color ? 4 : 2)) {
            /*
               c0 denotes the first 4bpp pixel shifted, while c1 denotes the second.
               For 8bpp modes, the first 4bpp pixel is the upper 4 bits.
             */
            uint32_t c0 = (out_edat >> (current_shift & 0x1C)) & 0xF;
            current_shift >>= 3;
            uint32_t c1 = (out_edat >> (current_shift & 0x1C)) & 0xF;
            current_shift >>= 3;

            if (svga->ati_4color) {
                uint32_t  q[4];
                q[0]      = svga->pallook[svga->egapal[(c0 & 0x0c) >> 2]];
                q[1]      = svga->pallook[svga->egapal[c0 & 0x03]];
                q[2]      = svga->pallook[svga->egapal[(c1 & 0x0c) >> 2]];
                q[3]      = svga->pallook[svga->egapal[c1 & 0x03]];

                const int outoffs = i << dwshift;
                for (int ch = 0; ch < 4; ch++) {
                    for (int subx = 0; subx < dotwidth; subx++)
                        p[outoffs + subx + (dotwidth * ch)] = q[ch];
                }
            } else if (combine8bits) {
                if (svga->packed_4bpp) {
                    uint32_t  p0;
                    uint32_t  p1;
                    if (svga->half_pixel) {
                        col                 &= 0xf0;
                        col                 |= (c0 >> 4) & 0xff;
                        col2                 = (c0 << 4) & 0xff;
                        col2                |= (c1 >> 4) & 0xff;
                        p0                  = svga->map8[col & svga->dac_mask];
                        p1                  = svga->map8[col2 & svga->dac_mask];
                        col                 = (c1 << 4) & 0xff;
                    } else {
                        p0                = svga->map8[c0 & svga->dac_mask];
                        p1                = svga->map8[c1 & svga->dac_mask];
                        col                 = p1;
                    }
                    const int outoffs = i << dwshift;
                    for (int subx = 0; subx < dotwidth; subx++)
                        p[outoffs + subx] = p0;
                    for (int subx = 0; subx < dotwidth; subx++)
                        p[outoffs + subx + dotwidth] = p1;
                } else {
                    uint32_t  ccombined = (c0 << 4) | c1;
                    uint32_t  p0;
                    if (svga->half_pixel) {
                        col                 &= 0xf0;
                        col                 |= (ccombined >> 4) & 0xff;
                        p0                  = svga->map8[col & svga->dac_mask];
                        col                 = (ccombined << 4) & 0xff;
                    } else {
                        p0                  = svga->map8[ccombined & svga->dac_mask];
                        col                 = p0;
                    }
                    const int outoffs   = (i >> 1) << dwshift;
                    for (int subx = 0; subx < dotwidth; subx++)
                        p[outoffs + subx] = p0;
                }
            } else {
                uint32_t  p0      = svga->pallook[svga->egapal[c0] & svga->dac_mask];
                uint32_t  p1      = svga->pallook[svga->egapal[c1] & svga->dac_mask];
                const int outoffs = i << dwshift;
                for (int subx = 0; subx < dotwidth; subx++)
                    p[outoffs + subx] = p0;
                for (int subx = 0; subx < dotwidth; subx++)
                    p[outoffs + subx + dotwidth] = p1;
                if ((x + i - svga->scrollcache) & 0x01)
                    /* The lower 4 bits are undefined at this point. */
                    col = c1 << 4;
                else
                    col = (c0 << 4) | c1;
            }
        }

        if (svga->ati_4color)
            p += (charwidth << 1);
            // p += charwidth;
        else
            p += charwidth;
    }

    if (svga->render_line_offset < 0) {
        uint32_t *orig_line = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];
        memmove(orig_line, orig_line + (charwidth * -svga->render_line_offset), (svga->hdisp) * 4);
        memset((orig_line + svga->hdisp) - (charwidth * -svga->render_line_offset), svga->overscan_color, (size_t) charwidth * -svga->render_line_offset * 4);
    }
}

/*
   Remap these to the paletted renderer
   (*, highres, combine8bits)
 */
void gd5440_svga_render_2bpp_lowres(gd5440_svga_t *svga) { gd5440_svga_render_indexed_gfx(svga, false, false); }
void gd5440_svga_render_2bpp_highres(gd5440_svga_t *svga) { gd5440_svga_render_indexed_gfx(svga, true, false); }
void gd5440_svga_render_4bpp_lowres(gd5440_svga_t *svga) { gd5440_svga_render_indexed_gfx(svga, false, false); }
void gd5440_svga_render_4bpp_highres(gd5440_svga_t *svga) { gd5440_svga_render_indexed_gfx(svga, true, false); }
void gd5440_svga_render_8bpp_lowres(gd5440_svga_t *svga) { gd5440_svga_render_indexed_gfx(svga, false, true); }
void gd5440_svga_render_8bpp_highres(gd5440_svga_t *svga) { gd5440_svga_render_indexed_gfx(svga, true, true); }

void
gd5440_svga_render_4bpp_tseng_highres(gd5440_svga_t *svga)
{
    int       changed_offset;
    int       x;
    int       oddeven;
    uint32_t  addr;
    uint32_t *p;
    uint8_t   edat[4];
    uint8_t   dat;
    uint32_t  changed_addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        changed_offset = (svga->memaddr + (svga->scanline & ~svga->crtc[0x17] & 3) * 0x8000) >> 12;

        if (svga->changedvram[changed_offset] || svga->changedvram[changed_offset + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                addr    = svga->memaddr;
                oddeven = 0;

                if (!(svga->crtc[0x17] & 0x40)) {
                    addr = (addr << 1) & svga->vram_mask;

                    if (svga->seqregs[1] & 4)
                        oddeven = (addr & 4) ? 1 : 0;

                    addr &= ~7;

                    if ((svga->crtc[0x17] & 0x20) && (svga->memaddr & 0x20000))
                        addr |= 4;
                    if (!(svga->crtc[0x17] & 0x20) && (svga->memaddr & 0x8000))
                        addr |= 4;
                }

                if (!(svga->crtc[0x17] & 0x01))
                    addr = (addr & ~0x8000) | ((svga->scanline & 1) ? 0x8000 : 0);
                if (!(svga->crtc[0x17] & 0x02))
                    addr = (addr & ~0x10000) | ((svga->scanline & 2) ? 0x10000 : 0);

                if (svga->seqregs[1] & 4) {
                    edat[0] = svga->vram[addr | oddeven];
                    edat[2] = svga->vram[addr | oddeven | 0x2];
                    edat[1] = edat[3] = 0;
                    svga->memaddr += 2;
                } else {
                    gd5440_write_le32(&edat[0], gd5440_read_le32(&svga->vram[addr]));
                    svga->memaddr += 4;
                }
                svga->memaddr &= svga->vram_mask;

                dat  = gd5440_edatlookup[edat[0] >> 6][edat[1] >> 6] | (gd5440_edatlookup[edat[2] >> 6][edat[3] >> 6] << 2);
                p[0] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask]];
                p[1] = svga->pallook[svga->egapal[dat & svga->plane_mask]];
                dat  = gd5440_edatlookup[(edat[0] >> 4) & 3][(edat[1] >> 4) & 3] | (gd5440_edatlookup[(edat[2] >> 4) & 3][(edat[3] >> 4) & 3] << 2);
                p[2] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask]];
                p[3] = svga->pallook[svga->egapal[dat & svga->plane_mask]];
                dat  = gd5440_edatlookup[(edat[0] >> 2) & 3][(edat[1] >> 2) & 3] | (gd5440_edatlookup[(edat[2] >> 2) & 3][(edat[3] >> 2) & 3] << 2);
                p[4] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask]];
                p[5] = svga->pallook[svga->egapal[dat & svga->plane_mask]];
                dat  = gd5440_edatlookup[edat[0] & 3][edat[1] & 3] | (gd5440_edatlookup[edat[2] & 3][edat[3] & 3] << 2);
                p[6] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask]];
                p[7] = svga->pallook[svga->egapal[dat & svga->plane_mask]];

                p += 8;
            }
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                addr    = svga->remap_func(svga, svga->memaddr);
                oddeven = 0;

                if (svga->seqregs[1] & 4) {
                    oddeven = (addr & 4) ? 1 : 0;
                    edat[0] = svga->vram[addr | oddeven];
                    edat[2] = svga->vram[addr | oddeven | 0x2];
                    edat[1] = edat[3] = 0;
                    svga->memaddr += 2;
                } else {
                    gd5440_write_le32(&edat[0], gd5440_read_le32(&svga->vram[addr]));
                    svga->memaddr += 4;
                }
                svga->memaddr &= svga->vram_mask;

                dat  = gd5440_edatlookup[edat[0] >> 6][edat[1] >> 6] | (gd5440_edatlookup[edat[2] >> 6][edat[3] >> 6] << 2);
                p[0] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask]];
                p[1] = svga->pallook[svga->egapal[dat & svga->plane_mask]];
                dat  = gd5440_edatlookup[(edat[0] >> 4) & 3][(edat[1] >> 4) & 3] | (gd5440_edatlookup[(edat[2] >> 4) & 3][(edat[3] >> 4) & 3] << 2);
                p[2] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask]];
                p[3] = svga->pallook[svga->egapal[dat & svga->plane_mask]];
                dat  = gd5440_edatlookup[(edat[0] >> 2) & 3][(edat[1] >> 2) & 3] | (gd5440_edatlookup[(edat[2] >> 2) & 3][(edat[3] >> 2) & 3] << 2);
                p[4] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask]];
                p[5] = svga->pallook[svga->egapal[dat & svga->plane_mask]];
                dat  = gd5440_edatlookup[edat[0] & 3][edat[1] & 3] | (gd5440_edatlookup[edat[2] & 3][edat[3] & 3] << 2);
                p[6] = svga->pallook[svga->egapal[(dat >> 4) & svga->plane_mask]];
                p[7] = svga->pallook[svga->egapal[dat & svga->plane_mask]];

                p += 8;
            }
        }
    }
}

void
gd5440_svga_render_8bpp_clone_highres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp /* + svga->scrollcache*/); x += 8) {
                dat  = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);
                p[0] = svga->map8[dat & svga->dac_mask & 0xff];
                p[1] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                p[2] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                p[3] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + 4) & svga->vram_display_mask]);
                p[4] = svga->map8[dat & svga->dac_mask & 0xff];
                p[5] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                p[6] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                p[7] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                svga->memaddr += 8;
                p += 8;
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp /* + svga->scrollcache*/); x += 8) {
                    dat  = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);
                    p[0] = svga->map8[dat & svga->dac_mask & 0xff];
                    p[1] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                    p[2] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                    p[3] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + 4) & svga->vram_display_mask]);
                    p[4] = svga->map8[dat & svga->dac_mask & 0xff];
                    p[5] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                    p[6] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                    p[7] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                    svga->memaddr += 8;
                    p += 8;
                }
            } else {
                for (x = 0; x <= (svga->hdisp /* + svga->scrollcache*/); x += 4) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                    p[0] = svga->map8[dat & svga->dac_mask & 0xff];
                    p[1] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                    p[2] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                    p[3] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                    svga->memaddr += 4;
                    p += 4;
                }
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    }
}

// TODO: Integrate more of this into the generic paletted renderer --GM
#if 0
void
gd5440_svga_render_8bpp_lowres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                dat = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);

                p[0] = p[1] = svga->map8[dat & 0xff];
                p[2] = p[3] = svga->map8[(dat >> 8) & 0xff];
                p[4] = p[5] = svga->map8[(dat >> 16) & 0xff];
                p[6] = p[7] = svga->map8[(dat >> 24) & 0xff];

                svga->memaddr += 4;
                p += 8;
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                    dat  = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);
                    p[0] = p[1] = svga->map8[dat & svga->dac_mask & 0xff];
                    p[2] = p[3] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                    p[4] = p[5] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                    p[6] = p[7] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                    svga->memaddr += 4;
                    p += 8;
                }
            } else {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                    p[0] = p[1] = svga->map8[dat & svga->dac_mask & 0xff];
                    p[2] = p[3] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                    p[4] = p[5] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                    p[6] = p[7] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                    svga->memaddr += 4;
                    p += 8;
                }
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    }
}

void
gd5440_svga_render_8bpp_highres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp /* + svga->scrollcache*/); x += 8) {
                dat  = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);
                p[0] = svga->map8[dat & svga->dac_mask & 0xff];
                p[1] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                p[2] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                p[3] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + 4) & svga->vram_display_mask]);
                p[4] = svga->map8[dat & svga->dac_mask & 0xff];
                p[5] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                p[6] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                p[7] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                svga->memaddr += 8;
                p += 8;
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp /* + svga->scrollcache*/); x += 8) {
                    dat  = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);
                    p[0] = svga->map8[dat & svga->dac_mask & 0xff];
                    p[1] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                    p[2] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                    p[3] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + 4) & svga->vram_display_mask]);
                    p[4] = svga->map8[dat & svga->dac_mask & 0xff];
                    p[5] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                    p[6] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                    p[7] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                    svga->memaddr += 8;
                    p += 8;
                }
            } else {
                for (x = 0; x <= (svga->hdisp /* + svga->scrollcache*/); x += 4) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                    p[0] = svga->map8[dat & svga->dac_mask & 0xff];
                    p[1] = svga->map8[(dat >> 8) & svga->dac_mask & 0xff];
                    p[2] = svga->map8[(dat >> 16) & svga->dac_mask & 0xff];
                    p[3] = svga->map8[(dat >> 24) & svga->dac_mask & 0xff];

                    svga->memaddr += 4;
                    p += 4;
                }
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    }
}
#endif

void
gd5440_svga_render_8bpp_tseng_lowres(gd5440_svga_t *svga)
{
    uint32_t *p;
    uint32_t  dat;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange || svga->render_line_offset) {
        p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

        if (svga->firstline_draw == 2000)
            svga->firstline_draw = svga->displine;
        svga->lastline_draw = svga->displine;

        if (svga->render_line_offset) {
            if (svga->render_line_offset > 0) {
                memset(p, svga->overscan_color, 8 * svga->render_line_offset * sizeof(uint32_t));
                p += 8 * svga->render_line_offset;
            }
        }

        for (int x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
            dat = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[0] = p[1] = svga->map8[dat & svga->dac_mask & 0xff];
            dat >>= 8;
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[2] = p[3] = svga->map8[dat & svga->dac_mask & 0xff];
            dat >>= 8;
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[4] = p[5] = svga->map8[dat & svga->dac_mask & 0xff];
            dat >>= 8;
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[6] = p[7] = svga->map8[dat & svga->dac_mask & 0xff];

            svga->memaddr += 4;
            p += 8;
        }
        if (svga->render_line_offset < 0) {
            uint32_t *orig_line = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];
            memmove(orig_line, orig_line + (8 * -svga->render_line_offset), (svga->hdisp) * 4);
            memset((orig_line + svga->hdisp) - (8 * -svga->render_line_offset), svga->overscan_color, 8 * -svga->render_line_offset * 4);
        }
        svga->memaddr &= svga->vram_display_mask;
    }
}

void
gd5440_svga_render_8bpp_tseng_highres(gd5440_svga_t *svga)
{
    uint32_t *p;
    uint32_t  dat;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange || svga->render_line_offset) {
        p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

        if (svga->firstline_draw == 2000)
            svga->firstline_draw = svga->displine;
        svga->lastline_draw = svga->displine;

        if (svga->render_line_offset) {
            if (svga->render_line_offset > 0) {
                memset(p, svga->overscan_color, 8 * svga->render_line_offset * sizeof(uint32_t));
                p += 8 * svga->render_line_offset;
            }
        }

        for (int x = 0; x <= (svga->hdisp /* + svga->scrollcache*/); x += 8) {
            dat = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[0] = svga->map8[dat & svga->dac_mask & 0xff];
            dat >>= 8;
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[1] = svga->map8[dat & svga->dac_mask & 0xff];
            dat >>= 8;
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[2] = svga->map8[dat & svga->dac_mask & 0xff];
            dat >>= 8;
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[3] = svga->map8[dat & svga->dac_mask & 0xff];

            dat = gd5440_read_le32(&svga->vram[(svga->memaddr + 4) & svga->vram_display_mask]);
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[4] = svga->map8[dat & svga->dac_mask & 0xff];
            dat >>= 8;
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[5] = svga->map8[dat & svga->dac_mask & 0xff];
            dat >>= 8;
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[6] = svga->map8[dat & svga->dac_mask & 0xff];
            dat >>= 8;
            if (svga->attrregs[0x10] & 0x80)
                dat = (dat & ~0xf0) | ((svga->attrregs[0x14] & 0x0f) << 4);
            p[7] = svga->map8[dat & svga->dac_mask & 0xff];

            svga->memaddr += 8;
            p += 8;
        }

        if (svga->render_line_offset < 0) {
            uint32_t *orig_line = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];
            memmove(orig_line, orig_line + (8 * -svga->render_line_offset), (svga->hdisp) * 4);
            memset((orig_line + svga->hdisp) - (8 * -svga->render_line_offset), svga->overscan_color, 8 * -svga->render_line_offset * 4);
        }

        svga->memaddr &= svga->vram_display_mask;
    }
}

void
gd5440_svga_render_15bpp_lowres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 4) {
                dat = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1)) & svga->vram_display_mask]);

                p[x << 1] = p[(x << 1) + 1] = svga->conv_16to32(svga, dat & 0xffff, 15);
                p[(x << 1) + 2] = p[(x << 1) + 3] = svga->conv_16to32(svga, dat >> 16, 15);

                dat = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 4) & svga->vram_display_mask]);

                p[(x << 1) + 4] = p[(x << 1) + 5] = svga->conv_16to32(svga, dat & 0xffff, 15);
                p[(x << 1) + 6] = p[(x << 1) + 7] = svga->conv_16to32(svga, dat >> 16, 15);
            }
            svga->memaddr += x << 1;
            svga->memaddr &= svga->vram_display_mask;
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 4) {
                    dat = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1)) & svga->vram_display_mask]);

                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 15);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 15);

                    dat = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 4) & svga->vram_display_mask]);

                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 15);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 15);
                }
                svga->memaddr += x << 1;
            } else {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 2) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);

                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 15);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 15);
                    svga->memaddr += 4;
                }
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    }
}

void
gd5440_svga_render_15bpp_highres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1)) & svga->vram_display_mask]);
                p[x]     = svga->conv_16to32(svga, dat & 0xffff, 15);
                p[x + 1] = svga->conv_16to32(svga, dat >> 16, 15);

                dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 4) & svga->vram_display_mask]);
                p[x + 2] = svga->conv_16to32(svga, dat & 0xffff, 15);
                p[x + 3] = svga->conv_16to32(svga, dat >> 16, 15);

                dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 8) & svga->vram_display_mask]);
                p[x + 4] = svga->conv_16to32(svga, dat & 0xffff, 15);
                p[x + 5] = svga->conv_16to32(svga, dat >> 16, 15);

                dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 12) & svga->vram_display_mask]);
                p[x + 6] = svga->conv_16to32(svga, dat & 0xffff, 15);
                p[x + 7] = svga->conv_16to32(svga, dat >> 16, 15);
            }
            svga->memaddr += x << 1;
            svga->memaddr &= svga->vram_display_mask;
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1)) & svga->vram_display_mask]);
                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 15);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 15);

                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 4) & svga->vram_display_mask]);
                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 15);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 15);

                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 8) & svga->vram_display_mask]);
                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 15);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 15);

                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 12) & svga->vram_display_mask]);
                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 15);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 15);
                }
                svga->memaddr += x << 1;
            } else {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 2) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);

                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 15);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 15);
                    svga->memaddr += 4;
                }
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    }
}

void
gd5440_svga_render_15bpp_mix_lowres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
        p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

        if (svga->firstline_draw == 2000)
            svga->firstline_draw = svga->displine;
        svga->lastline_draw = svga->displine;

        for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 4) {
            dat       = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1)) & svga->vram_display_mask]);
            p[x << 1] = p[(x << 1) + 1] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);

            dat >>= 16;
            p[(x << 1) + 2] = p[(x << 1) + 3] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);

            dat             = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 4) & svga->vram_display_mask]);
            p[(x << 1) + 4] = p[(x << 1) + 5] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);

            dat >>= 16;
            p[(x << 1) + 6] = p[(x << 1) + 7] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);
        }
        svga->memaddr += x << 1;
        svga->memaddr &= svga->vram_display_mask;
    }
}

void
gd5440_svga_render_15bpp_mix_highres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
        p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

        if (svga->firstline_draw == 2000)
            svga->firstline_draw = svga->displine;
        svga->lastline_draw = svga->displine;

        for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
            dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1)) & svga->vram_display_mask]);
            p[x] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);
            dat >>= 16;
            p[x + 1] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);

            dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 4) & svga->vram_display_mask]);
            p[x + 2] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);
            dat >>= 16;
            p[x + 3] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);

            dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 8) & svga->vram_display_mask]);
            p[x + 4] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);
            dat >>= 16;
            p[x + 5] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);

            dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 12) & svga->vram_display_mask]);
            p[x + 6] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);
            dat >>= 16;
            p[x + 7] = (dat & 0x00008000) ? svga->pallook[dat & 0xff] : svga->conv_16to32(svga, dat & 0xffff, 15);
        }
        svga->memaddr += x << 1;
        svga->memaddr &= svga->vram_display_mask;
    }
}

void
gd5440_svga_render_16bpp_lowres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 4) {
                dat       = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1)) & svga->vram_display_mask]);
                p[x << 1] = p[(x << 1) + 1] = svga->conv_16to32(svga, dat & 0xffff, 16);
                p[(x << 1) + 2] = p[(x << 1) + 3] = svga->conv_16to32(svga, dat >> 16, 16);

                dat             = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 4) & svga->vram_display_mask]);
                p[(x << 1) + 4] = p[(x << 1) + 5] = svga->conv_16to32(svga, dat & 0xffff, 16);
                p[(x << 1) + 6] = p[(x << 1) + 7] = svga->conv_16to32(svga, dat >> 16, 16);
            }
            svga->memaddr += x << 1;
            svga->memaddr &= svga->vram_display_mask;
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 4) {
                    dat = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1)) & svga->vram_display_mask]);

                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 16);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 16);

                    dat = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 4) & svga->vram_display_mask]);

                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 16);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 16);
                }
                svga->memaddr += x << 1;
            } else {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 2) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);

                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 16);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 16);
                }
                svga->memaddr += 4;
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    }
}

void
gd5440_svga_render_16bpp_highres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                uint32_t dat = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1)) & svga->vram_display_mask]);
                p[x]         = svga->conv_16to32(svga, dat & 0xffff, 16);
                p[x + 1]     = svga->conv_16to32(svga, dat >> 16, 16);

                dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 4) & svga->vram_display_mask]);
                p[x + 2] = svga->conv_16to32(svga, dat & 0xffff, 16);
                p[x + 3] = svga->conv_16to32(svga, dat >> 16, 16);

                dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 8) & svga->vram_display_mask]);
                p[x + 4] = svga->conv_16to32(svga, dat & 0xffff, 16);
                p[x + 5] = svga->conv_16to32(svga, dat >> 16, 16);

                dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 12) & svga->vram_display_mask]);
                p[x + 6] = svga->conv_16to32(svga, dat & 0xffff, 16);
                p[x + 7] = svga->conv_16to32(svga, dat >> 16, 16);
            }
            svga->memaddr += x << 1;
            svga->memaddr &= svga->vram_display_mask;
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 8) {
                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1)) & svga->vram_display_mask]);
                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 16);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 16);

                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 4) & svga->vram_display_mask]);
                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 16);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 16);

                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 8) & svga->vram_display_mask]);
                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 16);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 16);

                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 1) + 12) & svga->vram_display_mask]);
                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 16);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 16);
                }
                svga->memaddr += x << 1;
            } else {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 2) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);

                    *p++ = svga->conv_16to32(svga, dat & 0xffff, 16);
                    *p++ = svga->conv_16to32(svga, dat >> 16, 16);

                    svga->memaddr += 4;
                }
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    }
}

void
gd5440_svga_render_24bpp_lowres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  changed_addr;
    uint32_t  addr;
    uint32_t  dat0;
    uint32_t  dat1;
    uint32_t  dat2;
    uint32_t  fg;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if ((svga->displine + svga->y_add) < 0)
            return;

        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                fg = svga->vram[svga->memaddr] | (svga->vram[svga->memaddr + 1] << 8) | (svga->vram[svga->memaddr + 2] << 16);
                svga->memaddr += 3;
                svga->memaddr &= svga->vram_display_mask;
                svga->monitor->target_buffer->line[svga->displine + svga->y_add][(x << 1) + svga->x_add] = svga->monitor->target_buffer->line[svga->displine + svga->y_add][(x << 1) + 1 + svga->x_add] = lookup_lut(fg);
            }
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                    dat0 = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);
                    dat1 = gd5440_read_le32(&svga->vram[(svga->memaddr + 4) & svga->vram_display_mask]);
                    dat2 = gd5440_read_le32(&svga->vram[(svga->memaddr + 8) & svga->vram_display_mask]);

                    p[0] = p[1] = lookup_lut(dat0 & 0xffffff);
                    p[2] = p[3] = lookup_lut((dat0 >> 24) | ((dat1 & 0xffff) << 8));
                    p[4] = p[5] = lookup_lut((dat1 >> 16) | ((dat2 & 0xff) << 16));
                    p[6] = p[7] = lookup_lut(dat2 >> 8);

                    svga->memaddr += 12;
                }
            } else {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 4) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat0 = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                    addr = svga->remap_func(svga, svga->memaddr + 4);
                    dat1 = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                    addr = svga->remap_func(svga, svga->memaddr + 8);
                    dat2 = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);

                    p[0] = p[1] = lookup_lut(dat0 & 0xffffff);
                    p[2] = p[3] = lookup_lut((dat0 >> 24) | ((dat1 & 0xffff) << 8));
                    p[4] = p[5] = lookup_lut((dat1 >> 16) | ((dat2 & 0xff) << 16));
                    p[6] = p[7] = lookup_lut(dat2 >> 8);

                    svga->memaddr += 12;
                }
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    }
}

void
gd5440_svga_render_24bpp_highres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  changed_addr;
    uint8_t   addr;
    uint32_t  dat0;
    uint32_t  dat1;
    uint32_t  dat2;
    uint32_t  dat;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 4) {
                dat  = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);
                p[x] = lookup_lut(dat & 0xffffff);

                dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + 3) & svga->vram_display_mask]);
                p[x + 1] = lookup_lut(dat & 0xffffff);

                dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + 6) & svga->vram_display_mask]);
                p[x + 2] = lookup_lut(dat & 0xffffff);

                dat      = gd5440_read_le32(&svga->vram[(svga->memaddr + 9) & svga->vram_display_mask]);
                p[x + 3] = lookup_lut(dat & 0xffffff);

                svga->memaddr += 12;
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 4) {
                    dat0 = gd5440_read_le32(&svga->vram[svga->memaddr & svga->vram_display_mask]);
                    dat1 = gd5440_read_le32(&svga->vram[(svga->memaddr + 4) & svga->vram_display_mask]);
                    dat2 = gd5440_read_le32(&svga->vram[(svga->memaddr + 8) & svga->vram_display_mask]);

                    *p++ = lookup_lut(dat0 & 0xffffff);
                    *p++ = lookup_lut((dat0 >> 24) | ((dat1 & 0xffff) << 8));
                    *p++ = lookup_lut((dat1 >> 16) | ((dat2 & 0xff) << 16));
                    *p++ = lookup_lut(dat2 >> 8);

                    svga->memaddr += 12;
                }
            } else {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x += 4) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat0 = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                    addr = svga->remap_func(svga, svga->memaddr + 4);
                    dat1 = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                    addr = svga->remap_func(svga, svga->memaddr + 8);
                    dat2 = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);

                    *p++ = lookup_lut(dat0 & 0xffffff);
                    *p++ = lookup_lut((dat0 >> 24) | ((dat1 & 0xffff) << 8));
                    *p++ = lookup_lut((dat1 >> 16) | ((dat2 & 0xff) << 16));
                    *p++ = lookup_lut(dat2 >> 8);

                    svga->memaddr += 12;
                }
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    }
}

void
gd5440_svga_render_32bpp_lowres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->fullchange) {
            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                dat = svga->vram[svga->memaddr] | (svga->vram[svga->memaddr + 1] << 8) | (svga->vram[svga->memaddr + 2] << 16);
                svga->memaddr += 4;
                svga->memaddr &= svga->vram_display_mask;
                svga->monitor->target_buffer->line[svga->displine + svga->y_add][(x << 1) + svga->x_add] = svga->monitor->target_buffer->line[svga->displine + svga->y_add][(x << 1) + 1 + svga->x_add] = lookup_lut(dat);
            }
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 2)) & svga->vram_display_mask]);
                    *p++ = lookup_lut(dat & 0xffffff);
                    *p++ = lookup_lut(dat & 0xffffff);
                }
                svga->memaddr += (x * 4);
            } else {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                    *p++ = lookup_lut(dat & 0xffffff);
                    *p++ = lookup_lut(dat & 0xffffff);
                    svga->memaddr += 4;
                }
                svga->memaddr &= svga->vram_display_mask;
            }
        }
    }
}

void
gd5440_svga_render_32bpp_highres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    if (svga->force_old_addr) {
        if (svga->changedvram[svga->memaddr >> 12] || svga->changedvram[(svga->memaddr >> 12) + 1] || svga->changedvram[(svga->memaddr >> 12) + 2] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 2)) & svga->vram_display_mask]);
                p[x] = lookup_lut(dat & 0xffffff);
            }
            svga->memaddr += 4;
            svga->memaddr &= svga->vram_display_mask;
        }
    } else {
        changed_addr = svga->remap_func(svga, svga->memaddr);

        if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
            p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

            if (svga->firstline_draw == 2000)
                svga->firstline_draw = svga->displine;
            svga->lastline_draw = svga->displine;

            if (!svga->remap_required) {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                    dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 2)) & svga->vram_display_mask]);
                    *p++ = lookup_lut(dat & 0xffffff);
                }
                svga->memaddr += (x * 4);
            } else {
                for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                    addr = svga->remap_func(svga, svga->memaddr);
                    dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                    *p++ = lookup_lut(dat & 0xffffff);

                    svga->memaddr += 4;
                }
            }
            svga->memaddr &= svga->vram_display_mask;
        }
    }
}

void
gd5440_svga_render_ABGR8888_highres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    changed_addr = svga->remap_func(svga, svga->memaddr);

    if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
        p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

        if (svga->firstline_draw == 2000)
            svga->firstline_draw = svga->displine;
        svga->lastline_draw = svga->displine;

        if (!svga->remap_required) {
            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 2)) & svga->vram_display_mask]);
                *p++ = lookup_lut(((dat & 0xff0000) >> 16) | (dat & 0x00ff00) | ((dat & 0x0000ff) << 16));
            }
            svga->memaddr += x * 4;
        } else {
            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                addr = svga->remap_func(svga, svga->memaddr);
                dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                *p++ = lookup_lut(((dat & 0xff0000) >> 16) | (dat & 0x00ff00) | ((dat & 0x0000ff) << 16));

                svga->memaddr += 4;
            }
        }
        svga->memaddr &= svga->vram_display_mask;
    }
}

void
gd5440_svga_render_RGBA8888_highres(gd5440_svga_t *svga)
{
    int       x;
    uint32_t *p;
    uint32_t  dat;
    uint32_t  changed_addr;
    uint32_t  addr;

    if ((svga->displine + svga->y_add) < 0)
        return;

    changed_addr = svga->remap_func(svga, svga->memaddr);

    if (svga->changedvram[changed_addr >> 12] || svga->changedvram[(changed_addr >> 12) + 1] || svga->fullchange) {
        p = &svga->monitor->target_buffer->line[svga->displine + svga->y_add][svga->x_add];

        if (svga->firstline_draw == 2000)
            svga->firstline_draw = svga->displine;
        svga->lastline_draw = svga->displine;

        if (!svga->remap_required) {
            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                dat  = gd5440_read_le32(&svga->vram[(svga->memaddr + (x << 2)) & svga->vram_display_mask]);
                *p++ = lookup_lut(dat >> 8);
            }
            svga->memaddr += (x * 4);
        } else {
            for (x = 0; x <= (svga->hdisp + svga->scrollcache); x++) {
                addr = svga->remap_func(svga, svga->memaddr);
                dat  = gd5440_read_le32(&svga->vram[addr & svga->vram_display_mask]);
                *p++ = lookup_lut(dat >> 8);

                svga->memaddr += 4;
            }
        }
        svga->memaddr &= svga->vram_display_mask;
    }
}

void gd5440_svga_doblit(int wx, int wy, gd5440_svga_t *svga);
void gd5440_svga_poll(void *priv);

gd5440_svga_t *gd5440_svga_8514;

extern int     cyc_total;
extern uint8_t gd5440_edatlookup[4][4];

uint8_t gd5440_svga_rotate[8][256];

/*Primary SVGA device. As multiple video cards are not yet supported this is the
  only SVGA device.*/
static gd5440_svga_t *gd5440_svga_pri;

#ifdef ENABLE_SVGA_LOG
int gd5440_svga_do_log = ENABLE_SVGA_LOG;

static void
gd5440_svga_log(const char *fmt, ...)
{
    va_list ap;

    if (gd5440_svga_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define gd5440_svga_log(fmt, ...)
#endif

gd5440_svga_t *
gd5440_svga_get_pri(void)
{
    return gd5440_svga_pri;
}

void
gd5440_svga_set_poll(gd5440_svga_t *svga)
{
    gd5440_svga_log("SVGA Timer activated, enabled?=%x.\n", gd5440_timer_is_enabled(&svga->timer));
    gd5440_timer_set_callback(&svga->timer, gd5440_svga_poll);
    if (!gd5440_timer_is_enabled(&svga->timer))
        gd5440_timer_enable(&svga->timer);
}

void
gd5440_svga_set_override(gd5440_svga_t *svga, int val)
{
    gd5440_ibm8514_t *dev = (gd5440_ibm8514_t *) svga->dev8514;
    gd5440_xga_t     *xga = (gd5440_xga_t *) svga->xga;
    uint8_t    ret_poll = 0;

    if (svga->override && !val)
        svga->fullchange = svga->monitor->mon_changeframecount;

    svga->override = val;

    gd5440_svga_log("Override=%x.\n", val);
    if (gd5440_ibm8514_active && (svga->dev8514 != NULL))
        ret_poll |= 1;

    if (gd5440_xga_active && (svga->xga != NULL))
        ret_poll |= 2;

    if (svga->override)
        gd5440_svga_set_poll(svga);
    else {
        switch (ret_poll) {
            case 0:
            default:
                gd5440_svga_set_poll(svga);
                break;

            case 1:
                if (gd5440_ibm8514_active && (svga->dev8514 != NULL)) {
                    if (dev->on)
                        gd5440_ibm8514_set_poll(svga);
                    else
                        gd5440_svga_set_poll(svga);
                } else
                    gd5440_svga_set_poll(svga);
                break;

            case 2:
                if (gd5440_xga_active && (svga->xga != NULL)) {
                    if (xga->on)
                        gd5440_xga_set_poll(svga);
                    else
                        gd5440_svga_set_poll(svga);
                } else
                    gd5440_svga_set_poll(svga);
                break;

            case 3:
                if (gd5440_ibm8514_active && (svga->dev8514 != NULL) && gd5440_xga_active && (svga->xga != NULL))  {
                    if (dev->on)
                        gd5440_ibm8514_set_poll(svga);
                    else if (xga->on)
                        gd5440_xga_set_poll(svga);
                    else
                        gd5440_svga_set_poll(svga);
                } else
                    gd5440_svga_set_poll(svga);
                break;
        }
    }

#ifdef OVERRIDE_OVERSCAN
    if (!val) {
        /* Override turned off, restore overscan X and Y per the CRTC. */
        svga->monitor->mon_overscan_y = (svga->rowcount + 1) << 1;

        if (svga->monitor->mon_overscan_y < 16)
            svga->monitor->mon_overscan_y = 16;

        svga->monitor->mon_overscan_x = (svga->seqregs[1] & 1) ? 16 : 18;

        if (svga->seqregs[1] & 8)
            svga->monitor->mon_overscan_x <<= 1;
    } else
        svga->monitor->mon_overscan_x = svga->monitor->mon_overscan_y = 16;
    /* Override turned off, fix overcan X and Y to 16. */
#endif
}

void
gd5440_svga_out(uint16_t addr, uint8_t val, void *priv)
{
    gd5440_svga_t    *svga = (gd5440_svga_t *) priv;
    gd5440_ibm8514_t *dev = (gd5440_ibm8514_t *) svga->dev8514;
    gd5440_xga_t     *xga = (gd5440_xga_t *) svga->xga;
    uint8_t    o;
    uint8_t    index;
    uint8_t    pal4to16[16] = { 0, 7, 0x38, 0x3f, 0, 3, 4, 0x3f, 0, 2, 4, 0x3e, 0, 3, 5, 0x3f };

    if ((addr >= 0x2ea) && (addr <= 0x2ed)) {
        if (!dev)
            return;
    }

    switch (addr) {
        case 0x2ea:
            dev->dac_mask = val;
            break;
        case 0x2eb:
        case 0x2ec:
            dev->dac_pos    = 0;
            dev->dac_status = addr & 0x03;
            dev->dac_addr   = (val + (addr & 0x01)) & 0xff;
            break;
        case 0x2ed:
            svga->fullchange = svga->monitor->mon_changeframecount;
            switch (dev->dac_pos) {
                case 0:
                    dev->dac_r = val;
                    dev->dac_pos++;
                    break;
                case 1:
                    dev->dac_g = val;
                    dev->dac_pos++;
                    break;
                case 2:
                    index                 = dev->dac_addr & 0xff;
                    dev->dac_b = val;
                    dev->_8514pal[index].r = dev->dac_r;
                    dev->_8514pal[index].g = dev->dac_g;
                    dev->_8514pal[index].b = dev->dac_b;
                    if (svga->ramdac_type == RAMDAC_8BIT)
                        dev->pallook[index] = makecol32(dev->_8514pal[index].r, dev->_8514pal[index].g, dev->_8514pal[index].b);
                    else
                        dev->pallook[index] = makecol32(gd5440_video_6to8[dev->_8514pal[index].r & 0x3f], gd5440_video_6to8[dev->_8514pal[index].g & 0x3f], gd5440_video_6to8[dev->_8514pal[index].b & 0x3f]);
                    dev->dac_pos  = 0;
                    dev->dac_addr = (dev->dac_addr + 1) & 0xff;
                    break;

                default:
                    break;
            }
            break;

        case 0x3c0:
        case 0x3c1:
            if (!svga->attrff) {
                svga->attraddr = val & 0x1f;
                if ((val & 0x20) != svga->attr_palette_enable) {
                    svga->fullchange          = 3;
                    svga->attr_palette_enable = val & 0x20;
                    gd5440_svga_log("Write Port %03x palette enable=%02x.\n", addr, svga->attr_palette_enable);
                    gd5440_svga_recalctimings(svga);
                }
            } else {
                if ((svga->attraddr == 0x13) && (svga->attrregs[0x13] != val))
                    svga->fullchange = svga->monitor->mon_changeframecount;
                o                                   = svga->attrregs[svga->attraddr & 0x1f];
                svga->attrregs[svga->attraddr & 0x1f] = val;
                if (svga->attraddr < 0x10)
                    svga->fullchange = svga->monitor->mon_changeframecount;

                if ((svga->attraddr == 0x10) || (svga->attraddr == 0x14) || (svga->attraddr < 0x10)) {
                    for (int c = 0; c < 0x10; c++) {
                        if (svga->attrregs[0x10] & 0x80)
                            svga->egapal[c] = (svga->attrregs[c] & 0xf) | ((svga->attrregs[0x14] & 0xf) << 4);
                        else if (svga->ati_4color)
                            svga->egapal[c] = pal4to16[(c & 0x03) | ((val >> 2) & 0xc)];
                        else
                            svga->egapal[c] = (svga->attrregs[c] & 0x3f) | ((svga->attrregs[0x14] & 0xc) << 4);
                    }
                    svga->fullchange = svga->monitor->mon_changeframecount;
                }
                /* Recalculate timings on change of attribute register 0x11
                   (overscan border color) too. */
                if (svga->attraddr == 0x10) {
                    if (o != val) {
                        gd5440_svga_log("ATTR10.\n");
                        gd5440_svga_recalctimings(svga);
                    }
                } else if (svga->attraddr == 0x11) {
                    svga->overscan_color = svga->pallook[svga->attrregs[0x11]];
                    if (o != val) {
                        gd5440_svga_log("ATTR11.\n");
                        gd5440_svga_recalctimings(svga);
                    }
                } else if (svga->attraddr == 0x12) {
                    if ((val & 0xf) != svga->plane_mask)
                        svga->fullchange = svga->monitor->mon_changeframecount;
                    svga->plane_mask = val & 0xf;
                }
            }
            svga->attrff ^= 1;
            break;
        case 0x3c2:
            svga->miscout  = val;
            svga->vidclock = val & 4;
            if (svga->priv_parent == NULL) {
                gd5440_io_removehandler(0x03a0, 0x0020, svga->gd5440_video_in, NULL, NULL, svga->gd5440_video_out, NULL, NULL, svga->priv);
                if (!(val & 1))
                    gd5440_io_sethandler(0x03a0, 0x0020, svga->gd5440_video_in, NULL, NULL, svga->gd5440_video_out, NULL, NULL, svga->priv);
            }
            gd5440_svga_recalctimings(svga);
            break;
        case 0x3c3:
            if (gd5440_xga_active && xga)
                xga->on = (val & 0x01) ? 0 : 1;
            if (gd5440_ibm8514_active && dev)
                dev->on = (val & 0x01) ? 0 : 1;

            gd5440_svga_log("Write Port 3C3=%x.\n", val & 0x01);
            gd5440_svga_recalctimings(svga);
            break;
        case 0x3c4:
            svga->seqaddr = val;
            break;
        case 0x3c5:
            if (svga->seqaddr > 0xf)
                return;
            o                                  = svga->seqregs[svga->seqaddr & 0xf];
            svga->seqregs[svga->seqaddr & 0xf] = val;
            if (o != val && (svga->seqaddr & 0xf) == 1) {
                gd5440_svga_log("SEQADDR1 write1.\n");
                gd5440_svga_recalctimings(svga);
            }
            switch (svga->seqaddr & 0xf) {
                case 1:
                    if (svga->scrblank && !(val & 0x20))
                        svga->fullchange = 3;
                    svga->scrblank = (svga->scrblank & ~0x20) | (val & 0x20);
                    gd5440_svga_log("SEQADDR1 write2.\n");
                    gd5440_svga_recalctimings(svga);
                    break;
                case 2:
                    svga->writemask = val & 0xf;
                    break;
                case 3:
                    svga->charsetb = (((val >> 2) & 3) * 0x10000) + 2;
                    svga->charseta = ((val & 3) * 0x10000) + 2;
                    if (val & 0x10)
                        svga->charseta += 0x8000;
                    if (val & 0x20)
                        svga->charsetb += 0x8000;
                    break;
                case 4:
                    svga->chain2_write = !(val & 4);
                    svga->chain4       = (svga->chain4 & ~8) | (val & 8);
                    svga->fast         = (svga->gdcreg[8] == 0xff && !(svga->gdcreg[3] & 0x18) && !svga->gdcreg[1]) &&
                                        ((svga->chain4 && (svga->packed_chain4 || svga->force_old_addr)) || svga->fb_only) &&
                                        !(svga->adv_flags & FLAG_ADDR_BY8);
                    break;

                default:
                    break;
            }
            break;
        case 0x3c6:
            svga->dac_mask = val;
            break;
        case 0x3c7:
        case 0x3c8:
            svga->dac_pos    = 0;
            svga->dac_status = addr & 0x03;
            svga->dac_addr   = (val + (addr & 0x01)) & 0xff;
            break;
        case 0x3c9:
            if (svga->adv_flags & FLAG_RAMDAC_SHIFT)
                val <<= 2;
            svga->fullchange = svga->monitor->mon_changeframecount;
            switch (svga->dac_pos) {
                case 0:
                    svga->dac_r = val;
                    svga->dac_pos++;
                    break;
                case 1:
                    svga->dac_g = val;
                    svga->dac_pos++;
                    break;
                case 2:
                    index                 = svga->dac_addr & 0xff;
                    svga->dac_b           = val;
                    svga->vgapal[index].r = svga->dac_r;
                    svga->vgapal[index].g = svga->dac_g;
                    svga->vgapal[index].b = svga->dac_b;
                    if (svga->ramdac_type == RAMDAC_8BIT)
                        svga->pallook[index] = makecol32(svga->vgapal[index].r, svga->vgapal[index].g, svga->vgapal[index].b);
                    else
                        svga->pallook[index] = makecol32(gd5440_video_6to8[svga->vgapal[index].r & 0x3f], gd5440_video_6to8[svga->vgapal[index].g & 0x3f], gd5440_video_6to8[svga->vgapal[index].b & 0x3f]);
                    svga->dac_pos  = 0;
                    svga->dac_addr = (svga->dac_addr + 1) & 0xff;
                    break;

                default:
                    break;
            }
            break;
        case 0x3ce:
            svga->gdcaddr = val;
            break;
        case 0x3cf:
            o = svga->gdcreg[svga->gdcaddr & 15];
            switch (svga->gdcaddr & 15) {
                case 2:
                    svga->colourcompare = val;
                    break;
                case 4:
                    svga->readplane = val & 3;
                    break;
                case 5:
                    svga->writemode   = val & 3;
                    svga->readmode    = val & 8;
                    svga->chain2_read = val & 0x10;
                    break;
                case 6:
                    if ((svga->gdcreg[6] & 0xc) != (val & 0xc)) {
                        switch (val & 0xc) {
                            case 0x0: /*128k at A0000*/
                                gd5440_mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x20000);
                                svga->banked_mask = 0xffff;
                                break;
                            case 0x4: /*64k at A0000*/
                                gd5440_mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x10000);
                                svga->banked_mask = 0xffff;
                                if (gd5440_xga_active && (svga->xga != NULL)) {
                                    xga->on = 0;
                                    gd5440_mem_mapping_set_handler(&svga->mapping, svga->read, svga->readw, svga->readl, svga->write, svga->writew, svga->writel);
                                }
                                break;
                            case 0x8: /*32k at B0000*/
                                gd5440_mem_mapping_set_addr(&svga->mapping, 0xb0000, 0x08000);
                                svga->banked_mask = 0x7fff;
                                break;
                            case 0xC: /*32k at B8000*/
                                gd5440_mem_mapping_set_addr(&svga->mapping, 0xb8000, 0x08000);
                                svga->banked_mask = 0x7fff;
                                break;

                            default:
                                break;
                        }
                    }
                    break;
                case 7:
                    svga->colournocare = val;
                    break;

                default:
                    break;
            }
            svga->gdcreg[svga->gdcaddr & 15] = val;
            svga->fast                       = (svga->gdcreg[8] == 0xff && !(svga->gdcreg[3] & 0x18) && !svga->gdcreg[1]) &&
                                               ((svga->chain4 && (svga->packed_chain4 || svga->force_old_addr)) || svga->fb_only) &&
                                                !(svga->adv_flags & FLAG_ADDR_BY8);;
            if (((svga->gdcaddr & 15) == 5 && (val ^ o) & 0x70) || ((svga->gdcaddr & 15) == 6 && (val ^ o) & 1)) {
                gd5440_svga_log("GDCADDR%02x recalc.\n", svga->gdcaddr & 0x0f);
                gd5440_svga_recalctimings(svga);
            }
            break;
        case 0x3da:
            svga->fcr = val;
            break;

        default:
            break;
    }
}

uint8_t
gd5440_svga_in(uint16_t addr, void *priv)
{
    gd5440_svga_t    *svga = (gd5440_svga_t *) priv;
    gd5440_ibm8514_t *dev = (gd5440_ibm8514_t *) svga->dev8514;
    gd5440_xga_t     *xga = (gd5440_xga_t *) svga->xga;
    uint8_t    index;
    uint8_t    ret = 0xff;

    if ((addr >= 0x2ea) && (addr <= 0x2ed)) {
        if (!dev)
            return ret;
    }

    switch (addr) {
        case 0x2ea:
            ret = dev->dac_mask;
            break;
        case 0x2eb:
            ret = dev->dac_status;
            break;
        case 0x2ec:
            ret = dev->dac_addr;
            break;
        case 0x2ed:
            index = (dev->dac_addr - 1) & 0xff;
            switch (dev->dac_pos) {
                case 0:
                    dev->dac_pos++;
                    if (svga->ramdac_type == RAMDAC_8BIT)
                        ret = dev->_8514pal[index].r;
                    else
                        ret = dev->_8514pal[index].r & 0x3f;
                    break;
                case 1:
                    dev->dac_pos++;
                    if (svga->ramdac_type == RAMDAC_8BIT)
                        ret = dev->_8514pal[index].g;
                    else
                        ret = dev->_8514pal[index].g & 0x3f;
                    break;
                case 2:
                    dev->dac_pos  = 0;
                    dev->dac_addr = (dev->dac_addr + 1) & 0xff;
                    if (svga->ramdac_type == RAMDAC_8BIT)
                        ret = dev->_8514pal[index].b;
                    else
                        ret = dev->_8514pal[index].b & 0x3f;
                    break;

                default:
                    break;
            }
            break;

        case 0x3c0:
            ret = svga->attraddr | svga->attr_palette_enable;
            break;
        case 0x3c1:
            ret = svga->attrregs[svga->attraddr];
            break;
        case 0x3c2:
            if (svga->cable_connected) {
                if ((svga->vgapal[0].r + svga->vgapal[0].g + svga->vgapal[0].b) >= 0x4e)
                    ret = 0;
                else
                    ret = 0x10;
            /* Monitor is not connected to the planar VGA if the PS/55 Display Adapter is installed. */
            } else {
                /*
                   The IBM PS/55 Display Adapter has own Monitor Type Detection bit in the different I/O port (I/O 3E0h, 3E1h).
                   When the monitor cable is connected to the Display Adapter, the port 3C2h returns the value as 'no cable connection'.
                   The POST of PS/55 has an extra code. If the monitor is not detected on the planar VGA,
                   it reads the POS data in NVRAM set by the reference diskette, and writes the BIOS Data Area (Mem 487h, 489h).
                   MONCHK.EXE in the reference diskette uses both I/O ports to determine the monitor type, updates the NVRAM and BDA.
                */
                if (svga->vgapal[0].r >= 10 || svga->vgapal[0].g >= 10 || svga->vgapal[0].b >= 10)
                    ret = 0;
                else
                    ret = 0x10;
            }
            break;
        case 0x3c3:
            ret = 0x01;
            if (gd5440_xga_active && xga) {
                if (xga->on)
                    ret = 0x00;
            }
            if (gd5440_ibm8514_active && dev) {
                if (dev->on)
                    ret = 0x00;
            }
            gd5440_svga_log("VGA read: (0x%04x) ret=%02x.\n", addr, ret);
            break;
        case 0x3c4:
            ret = svga->seqaddr;
            break;
        case 0x3c5:
            ret = svga->seqregs[svga->seqaddr & 0x0f];
            break;
        case 0x3c6:
            ret = svga->dac_mask;
            break;
        case 0x3c7:
            ret = svga->dac_status;
            break;
        case 0x3c8:
            ret = svga->dac_addr;
            break;
        case 0x3c9:
            index = (svga->dac_addr - 1) & 0xff;
            switch (svga->dac_pos) {
                case 0:
                    svga->dac_pos++;
                    if (svga->ramdac_type == RAMDAC_8BIT)
                        ret = svga->vgapal[index].r;
                    else
                        ret = svga->vgapal[index].r & 0x3f;
                    break;
                case 1:
                    svga->dac_pos++;
                    if (svga->ramdac_type == RAMDAC_8BIT)
                        ret = svga->vgapal[index].g;
                    else
                        ret = svga->vgapal[index].g & 0x3f;
                    break;
                case 2:
                    svga->dac_pos  = 0;
                    svga->dac_addr = (svga->dac_addr + 1) & 0xff;
                    if (svga->ramdac_type == RAMDAC_8BIT)
                        ret = svga->vgapal[index].b;
                    else
                        ret = svga->vgapal[index].b & 0x3f;
                    break;

                default:
                    break;
            }
            if (svga->adv_flags & FLAG_RAMDAC_SHIFT)
                ret >>= 2;
            break;
        case 0x3ca:
            ret = svga->fcr;
            break;
        case 0x3cc:
            ret = svga->miscout;
            break;
        case 0x3ce:
            ret = svga->gdcaddr;
            break;
        case 0x3cf:
            /* The spec says GDC addresses 0xF8 to 0xFB return the latch. */
            switch (svga->gdcaddr) {
                case 0xf8:
                    ret = svga->latch.b[0];
                    break;
                case 0xf9:
                    ret = svga->latch.b[1];
                    break;
                case 0xfa:
                    ret = svga->latch.b[2];
                    break;
                case 0xfb:
                    ret = svga->latch.b[3];
                    break;
                default:
                    ret = svga->gdcreg[svga->gdcaddr & 0xf];
                    break;
            }
            break;
        case 0x3da:
            svga->attrff = 0;

            if (svga->cgastat & 0x01)
                svga->cgastat &= ~0x30;
            else
                svga->cgastat ^= 0x30;

            ret = svga->cgastat;

            if ((svga->fcr & 0x08) && svga->dispon)
                ret |= 0x08;
            break;

        default:
            break;
    }

    if ((addr >= 0x3c6) && (addr <= 0x3c9))
        gd5440_svga_log("VGA IN addr=%03x, temp=%02x.\n", addr, ret);
    else if ((addr >= 0x2ea) && (addr <= 0x2ed))
        gd5440_svga_log("8514/A IN addr=%03x, temp=%02x.\n", addr, ret);

    return ret;
}

void
gd5440_svga_set_ramdac_type(gd5440_svga_t *svga, int type)
{
    gd5440_ibm8514_t *dev = (gd5440_ibm8514_t *) svga->dev8514;
    gd5440_xga_t *xga = (gd5440_xga_t *) svga->xga;

    if (svga->ramdac_type != type) {
        svga->ramdac_type = type;

        for (int c = 0; c < 256; c++) {
            if (gd5440_ibm8514_active && dev) {
                if (svga->ramdac_type == RAMDAC_8BIT)
                    dev->pallook[c] = makecol32(dev->_8514pal[c].r, dev->_8514pal[c].g, dev->_8514pal[c].b);
                else
                    dev->pallook[c] = makecol32((dev->_8514pal[c].r & 0x3f) * 4,
                                                 (dev->_8514pal[c].g & 0x3f) * 4,
                                                 (dev->_8514pal[c].b & 0x3f) * 4);
            }
            if (gd5440_xga_active && xga) {
                if (svga->ramdac_type == RAMDAC_8BIT)
                    xga->pallook[c] = makecol32(xga->xgapal[c].r, xga->xgapal[c].g, xga->xgapal[c].b);
                else {
                    xga->pallook[c] = makecol32((xga->xgapal[c].r & 0x3f) * 4,
                                                 (xga->xgapal[c].g & 0x3f) * 4,
                                                 (xga->xgapal[c].b & 0x3f) * 4);
                }
            }
            if (svga->ramdac_type == RAMDAC_8BIT)
                svga->pallook[c] = makecol32(svga->vgapal[c].r, svga->vgapal[c].g, svga->vgapal[c].b);
            else
                svga->pallook[c] = makecol32((svga->vgapal[c].r & 0x3f) * 4,
                                             (svga->vgapal[c].g & 0x3f) * 4,
                                             (svga->vgapal[c].b & 0x3f) * 4);
        }
    }
}

void
gd5440_svga_recalctimings(gd5440_svga_t *svga)
{
    gd5440_ibm8514_t       *dev = (gd5440_ibm8514_t *) svga->dev8514;
    gd5440_xga_t           *xga = (gd5440_xga_t *) svga->xga;
    uint8_t          set_timer = 0;
    double           crtcconst;
    double           _dispontime;
    double           _dispofftime;
    double           disptime;
    double           crtcconst8514 = 0.0;
    double           _dispontime8514 = 0.0;
    double           _dispofftime8514 = 0.0;
    double           disptime8514 = 0.0;
    double           crtcconst_xga = 0.0;
    double           _dispontime_xga = 0.0;
    double           _dispofftime_xga = 0.0;
    double           disptime_xga = 0.0;
#ifdef ENABLE_SVGA_LOG
    int              vsyncend;
    int              hdispend;
    int              hdispstart;
    int              hsyncstart;
    int              hsyncend;
#endif
    int              old_monitor_overscan_x = svga->monitor->mon_overscan_x;
    int              old_monitor_overscan_y = svga->monitor->mon_overscan_y;

    if (svga->adv_flags & FLAG_PRECISETIME) {
#ifdef USE_DYNAREC
        if (cpu_use_dynarec)
            update_tsc();
#endif
    }

    svga->vtotal      = svga->crtc[6];
    svga->dispend     = svga->crtc[0x12];
    svga->vsyncstart  = svga->crtc[0x10];
    svga->split       = svga->crtc[0x18];
    svga->vblankstart = svga->crtc[0x15];

    if (svga->crtc[7] & 1)
        svga->vtotal |= 0x100;
    if (svga->crtc[7] & 32)
        svga->vtotal |= 0x200;
    svga->vtotal += 2;

    if (svga->crtc[7] & 2)
        svga->dispend |= 0x100;
    if (svga->crtc[7] & 64)
        svga->dispend |= 0x200;
    svga->dispend++;

    if (svga->crtc[7] & 4)
        svga->vsyncstart |= 0x100;
    if (svga->crtc[7] & 128)
        svga->vsyncstart |= 0x200;
    svga->vsyncstart++;

    if (svga->crtc[7] & 0x10)
        svga->split |= 0x100;
    if (svga->crtc[9] & 0x40)
        svga->split |= 0x200;
    svga->split++;

    if (svga->crtc[7] & 0x08)
        svga->vblankstart |= 0x100;
    if (svga->crtc[9] & 0x20)
        svga->vblankstart |= 0x200;
    svga->vblankstart++;

    svga->hdisp = svga->crtc[1];
    if (svga->crtc[1] & 1)
        svga->hdisp++;

    svga->htotal = svga->crtc[0];
    /* +5 has been verified by Sergi to be correct - +6 must have been an off by one error. */
    svga->htotal += 5; /*+5 is required for Tyrian*/

    svga->rowoffset = svga->crtc[0x13];

    svga->clock = (double) ((svga->vidclock) ? VGACONST2 : VGACONST1);

    svga->lowres = !!(svga->attrregs[0x10] & 0x40);

    svga->interlace = 0;

    svga->memaddr_latch = ((svga->crtc[0xc] << 8) | svga->crtc[0xd]) + ((svga->crtc[8] & 0x60) >> 5);
    svga->ca_adj   = 0;

    svga->rowcount = svga->crtc[9] & 0x1f;

    svga->hdisp_time = svga->hdisp;
    svga->render     = gd5440_svga_render_blank;
    if (!svga->scrblank && (svga->crtc[0x17] & 0x80) && svga->attr_palette_enable) {
        /* TODO: In case of bug reports, disable 9-dots-wide character clocks in graphics modes. */
        if (!(svga->gdcreg[6] & 1) && !(svga->attrregs[0x10] & 1)) {
            if (svga->seqregs[1] & 8)
                svga->hdisp *= (svga->seqregs[1] & 1) ? 16 : 18;
            else
                svga->hdisp *= (svga->seqregs[1] & 1) ? 8 : 9;
        } else {
            if (svga->seqregs[1] & 8)
                svga->hdisp *= 16;
            else
                svga->hdisp *= 8;
        }

        if (!(svga->gdcreg[6] & 1) && !(svga->attrregs[0x10] & 1)) { /*Text mode*/
            if (svga->seqregs[1] & 8)                               /*40 column*/
                svga->render = gd5440_svga_render_text_40;
            else
                svga->render = gd5440_svga_render_text_80;

            svga->hdisp_old = svga->hdisp;
        } else {
            svga->hdisp_old = svga->hdisp;

            if ((svga->bpp <= 8) || ((svga->gdcreg[5] & 0x60) <= 0x20)) {
                if ((svga->gdcreg[5] & 0x60) == 0x00) {
                    if (svga->seqregs[1] & 8) { /*Low res (320)*/
                        svga->render = gd5440_svga_render_4bpp_lowres;
                        gd5440_svga_log("4 bpp low res.\n");
                    } else
                        svga->render = gd5440_svga_render_4bpp_highres;
                } else if ((svga->gdcreg[5] & 0x60) == 0x20) {
                    if (svga->seqregs[1] & 8) { /*Low res (320)*/
                        svga->render = gd5440_svga_render_2bpp_lowres;
                        gd5440_svga_log("2 bpp low res.\n");
                    } else
                        svga->render = gd5440_svga_render_2bpp_highres;
                } else {
                    svga->map8 = svga->pallook;
                    gd5440_svga_log("Map8.\n");
                    if (svga->lowres) { /*Low res (320)*/
                        svga->render = gd5440_svga_render_8bpp_lowres;
                        gd5440_svga_log("8 bpp low res.\n");
                    } else
                        svga->render = gd5440_svga_render_8bpp_highres;
                }
            } else {
                switch (svga->gdcreg[5] & 0x60) {
                    case 0x40:
                    case 0x60: /*256+ colours*/
                        switch (svga->bpp) {
                            case 15:
                                if (svga->lowres)
                                    svga->render = gd5440_svga_render_15bpp_lowres;
                                else
                                    svga->render = gd5440_svga_render_15bpp_highres;
                                break;
                            case 16:
                                if (svga->lowres)
                                    svga->render = gd5440_svga_render_16bpp_lowres;
                                else
                                    svga->render = gd5440_svga_render_16bpp_highres;
                                break;
                            case 17:
                                if (svga->lowres)
                                    svga->render = gd5440_svga_render_15bpp_mix_lowres;
                                else
                                    svga->render = gd5440_svga_render_15bpp_mix_highres;
                                break;
                            case 24:
                                if (svga->lowres)
                                    svga->render = gd5440_svga_render_24bpp_lowres;
                                else
                                    svga->render = gd5440_svga_render_24bpp_highres;
                                break;
                            case 32:
                                if (svga->lowres)
                                    svga->render = gd5440_svga_render_32bpp_lowres;
                                else
                                    svga->render = gd5440_svga_render_32bpp_highres;
                                break;

                            default:
                                break;
                        }
                        break;

                    default:
                        break;
                }
            }
        }
    }

    svga->linedbl    = svga->crtc[9] & 0x80;
    svga->char_width = (svga->seqregs[1] & 1) ? 8 : 9;

    svga->monitor->mon_overscan_y = (svga->rowcount + 1) << 1;

    if (svga->monitor->mon_overscan_y < 16)
        svga->monitor->mon_overscan_y = 16;

    if (!(svga->gdcreg[6] & 1) && !(svga->attrregs[0x10] & 1)) {
        svga->monitor->mon_overscan_x = (svga->seqregs[1] & 1) ? 16 : 18;

        if (svga->seqregs[1] & 8)
            svga->monitor->mon_overscan_x <<= 1;
    } else
        svga->monitor->mon_overscan_x = 16;

    svga->hblankstart    = svga->crtc[2];
    svga->hblank_end_val = (svga->crtc[3] & 0x1f) | ((svga->crtc[5] & 0x80) ? 0x20 : 0x00);
    svga->hblank_end_mask = 0x0000003f;

    gd5440_svga_log("htotal = %i, hblankstart = %i, hblank_end_val = %02X\n",
             svga->htotal, svga->hblankstart, svga->hblank_end_val);

    if (!svga->scrblank && svga->attr_palette_enable) {
        /* TODO: In case of bug reports, disable 9-dots-wide character clocks in graphics modes. */
        if (!(svga->gdcreg[6] & 1) && !(svga->attrregs[0x10] & 1)) {
            if (svga->seqregs[1] & 8)
                svga->dots_per_clock = ((svga->seqregs[1] & 1) ? 16 : 18);
            else
                svga->dots_per_clock = ((svga->seqregs[1] & 1) ? 8 : 9);
        } else {
            if (svga->seqregs[1] & 8)
                svga->dots_per_clock = 16;
            else
                svga->dots_per_clock = 8;
        }
    } else
        svga->dots_per_clock = 1;

    svga->multiplier = 1.0;

    if (svga->recalctimings_ex)
        svga->recalctimings_ex(svga);

    if (gd5440_ibm8514_active && (svga->dev8514 != NULL)) {
        if (IBM_8514A || ATI_8514A_ULTRA)
            gd5440_ibm8514_recalctimings(svga);
    }

    if (gd5440_xga_active && (svga->xga != NULL))
        gd5440_xga_recalctimings(svga);

    svga->vblankend = (svga->vblankstart & 0xffffff80) | (svga->crtc[0x16] & 0x7f);
    if (svga->vblankend <= svga->vblankstart)
        svga->vblankend += 0x00000080;

    if (svga->hoverride || svga->override) {
        if (svga->hdisp >= 2048)
            svga->monitor->mon_overscan_x = 0;

        svga->y_add = (svga->monitor->mon_overscan_y >> 1);
        svga->left_overscan = svga->x_add = (svga->monitor->mon_overscan_x >> 1);
    } else {
        uint32_t dot = svga->hblankstart;
        uint32_t adj_dot = svga->hblankstart;
        uint32_t htotal = (uint32_t) svga->htotal;
        /* Verified with both the Voodoo 3 and the S3 cards: compare 7 bits if bit 7 is set,
           otherwise compare 6 bits. */
        uint32_t eff_mask = (svga->hblank_end_val & ~0x0000003f) ? svga->hblank_end_mask : 0x0000003f;
        svga->hblank_sub = 0;

        gd5440_svga_log("HDISP=%d, CRTC1+1=%d, Blank: %04i-%04i, Total: %04i, "
                 "Mask: %02X, ADJ_DOT=%04i.\n", svga->hdisp, svga->crtc[1] + 1,
                 svga->hblankstart, svga->hblank_end_val,
                 svga->htotal, eff_mask, adj_dot);

        while (adj_dot < (htotal << 1)) {
            if (dot == htotal)
                dot = 0;

            if (adj_dot >= htotal)
                svga->hblank_sub++;

            gd5440_svga_log("Loop: adjdot=%d, htotal=%d, dotmask=%02x, "
                     "hblankendvalmask=%02x, blankendval=%02x.\n", adj_dot,
                     svga->htotal, dot & eff_mask, svga->hblank_end_val & eff_mask,
                     svga->hblank_end_val);
            if ((dot & eff_mask) == (svga->hblank_end_val & eff_mask))
                break;

            dot++;
            adj_dot++;
        }

        uint32_t hd = svga->hdisp;
        svga->hdisp -= (svga->hblank_sub * svga->dots_per_clock);

        svga->left_overscan = svga->x_add = (svga->htotal - adj_dot - 1) * svga->dots_per_clock;
        svga->monitor->mon_overscan_x = svga->x_add + (svga->hblankstart * svga->dots_per_clock) - hd + svga->dots_per_clock;
        /* Compensate for the HDISP code above. */
        if (svga->crtc[1] & 1)
            svga->monitor->mon_overscan_x++;

        if ((svga->hdisp >= 2048) || (svga->left_overscan < 0)) {
            svga->left_overscan = svga->x_add = 0;
            svga->monitor->mon_overscan_x = 0;
        }

        /* - 1 because + 1 but also - 2 to compensate for the + 2 added to vtotal above. */
        svga->y_add = svga->vtotal - svga->vblankend - 1;
        svga->monitor->mon_overscan_y = svga->y_add + abs(svga->vblankstart - svga->dispend);

        if ((svga->dispend >= 2048) || (svga->y_add < 0)) {
            svga->y_add = 0;
            svga->monitor->mon_overscan_y = 0;
        }
    }

#if TBD
    if (gd5440_ibm8514_active && (svga->dev8514 != NULL)) {
        if (dev->on) {
            uint32_t _8514_dot = dev->h_sync_start;
            uint32_t _8514_adj_dot = dev->h_sync_start;
            uint32_t _8514_eff_mask = (dev->h_blank_end_val & ~0x0000001f) ? dev->h_blank_end_mask : 0x0000001f;
            dev->h_blank_sub = 0;

            mach_log("8514/A: HDISP=%d, HDISPED=%d, Blank: %04i-%04i, Total: %04i, "
                     "Mask: %02X, ADJ_DOT=%04i.\n", dev->hdisp, (dev->hdisped + 1) << 3,
                     dev->h_sync_start, dev->h_blank_end_val,
                     dev->h_total, _8514_eff_mask, _8514_adj_dot);

            while (_8514_adj_dot < (dev->h_total << 1)) {
                if (_8514_dot == dev->h_total)
                    _8514_dot = 0;

                if (_8514_adj_dot >= dev->h_total)
                    dev->h_blank_sub++;

                mach_log("8514/A: Loop: adjdot=%d, htotal=%d, dotmask=%02x, "
                         "hblankendvalmask=%02x, blankendval=%02x.\n", adj_dot,
                         dev->h_total, _8514_dot & _8514_eff_mask, dev->h_blank_end_val & _8514_eff_mask,
                         dev->h_blank_end_val);
                if ((_8514_dot & _8514_eff_mask) == (dev->h_blank_end_val & _8514_eff_mask))
                    break;

                _8514_dot++;
                _8514_adj_dot++;
            }

            uint32_t _8514_hd = dev->hdisp;
            dev->hdisp -= dev->h_blank_sub;

            svga->left_overscan = svga->x_add = (dev->h_total - _8514_adj_dot - 1) << 3;
            svga->monitor->mon_overscan_x = svga->x_add + (dev->h_sync_start << 3) - _8514_hd + 8;
            svga->monitor->mon_overscan_x++;

            if ((dev->hdisp >= 2048) || (svga->left_overscan < 0)) {
                svga->left_overscan = svga->x_add = 0;
                svga->monitor->mon_overscan_x = 0;
            }

            /* - 1 because + 1 but also - 2 to compensate for the + 2 added to vtotal above. */
            svga->y_add = svga->vtotal - svga->vblankend - 1;
            svga->monitor->mon_overscan_y = svga->y_add + abs(svga->vblankstart - svga->dispend);

            if ((dev->dispend >= 2048) || (svga->y_add < 0)) {
                svga->y_add = 0;
                svga->monitor->mon_overscan_y = 0;
            }
        }
    }
#endif

    if (svga->vblankstart < svga->dispend) {
        gd5440_svga_log("DISPEND > VBLANKSTART.\n");
        svga->dispend = svga->vblankstart;
    }

    crtcconst = svga->clock * svga->char_width;
    if (gd5440_ibm8514_active && (svga->dev8514 != NULL)) {
        if (dev->on)
            crtcconst8514 = svga->clock_8514 * 8;
    }
    if (gd5440_xga_active && (svga->xga != NULL)) {
        if (xga->on)
            crtcconst_xga = svga->clock_xga * 8;
    }

#ifdef ENABLE_SVGA_LOG
    vsyncend = (svga->vsyncstart & 0xfffffff0) | (svga->crtc[0x11] & 0x0f);
    if (vsyncend <= svga->vsyncstart)
        vsyncend += 0x00000010;

    hdispend   = svga->crtc[1] + 1;
    hdispstart = ((svga->crtc[3] >> 5) & 3);
    hsyncstart = svga->crtc[4] + ((svga->crtc[5] >> 5) & 3) + 1;
    hsyncend   = (hsyncstart & 0xffffffe0) | (svga->crtc[5] & 0x1f);
    if (hsyncend <= hsyncstart)
        hsyncend += 0x00000020;
#endif

    gd5440_svga_log("Last scanline in the vertical period: %i\n"
             "First scanline after the last of active display: %i\n"
             "First scanline with vertical retrace asserted: %i\n"
             "First scanline after the last with vertical retrace asserted: %i\n"
             "First scanline of blanking: %i\n"
             "First scanline after the last of blanking: %i\n"
             "\n"
             "Last character in the horizontal period: %i\n"
             "First character of active display: %i\n"
             "First character after the last of active display: %i\n"
             "First character with horizontal retrace asserted: %i\n"
             "First character after the last with horizontal retrace asserted: %i\n"
             "First character of blanking: %i\n"
             "First character after the last of blanking: %i\n"
             "\n"
             "\n",
             svga->vtotal, svga->dispend, svga->vsyncstart, vsyncend,
             svga->vblankstart, svga->vblankend,
             svga->htotal, hdispstart, hdispend, hsyncstart, hsyncend,
             svga->hblankstart, svga->hblankend);

    disptime    = svga->htotal * svga->multiplier;
    _dispontime = svga->hdisp_time;

    if (gd5440_ibm8514_active && (svga->dev8514 != NULL)) {
        if (dev->on) {
            disptime8514 = dev->h_total;
            _dispontime8514 = dev->h_disp_time;
        }
    }

    if (gd5440_xga_active && (svga->xga != NULL)) {
        if (xga->on) {
            disptime_xga = xga->h_total;
            _dispontime_xga = xga->h_disp_time;
        }
    }

    if (svga->seqregs[1] & 8) {
        disptime *= 2;
        _dispontime *= 2;
    }

    _dispofftime = disptime - _dispontime;
    _dispontime *= crtcconst;
    _dispofftime *= crtcconst;

    svga->dispontime  = (uint64_t) (_dispontime);
    svga->dispofftime = (uint64_t) (_dispofftime);
    if (svga->dispontime < GD5440_TIMER_USEC)
        svga->dispontime = GD5440_TIMER_USEC;
    if (svga->dispofftime < GD5440_TIMER_USEC)
        svga->dispofftime = GD5440_TIMER_USEC;

    if (gd5440_ibm8514_active && (svga->dev8514 != NULL))
        set_timer |= 1;

    if (gd5440_xga_active && (svga->xga != NULL))
        set_timer |= 2;

    switch (set_timer) {
        default:
        case 0: /*VGA only*/
            gd5440_svga_set_poll(svga);
            break;

        case 1: /*Plus 8514/A*/
            if (dev->on) {
                _dispofftime8514 = disptime8514 - _dispontime8514;
                gd5440_svga_log("DISPTIME8514=%lf, off=%lf, DISPONTIME8514=%lf, CRTCCONST8514=%lf.\n", disptime8514, _dispofftime8514, _dispontime8514, crtcconst8514);
                _dispontime8514 *= crtcconst8514;
                _dispofftime8514 *= crtcconst8514;

                dev->dispontime  = (uint64_t) (_dispontime8514);
                dev->dispofftime = (uint64_t) (_dispofftime8514);
                if (dev->dispontime < GD5440_TIMER_USEC)
                    dev->dispontime = GD5440_TIMER_USEC;
                if (dev->dispofftime < GD5440_TIMER_USEC)
                    dev->dispofftime = GD5440_TIMER_USEC;

                gd5440_ibm8514_set_poll(svga);
            } else
                gd5440_svga_set_poll(svga);
            break;

        case 2: /*Plus XGA*/
            if (xga->on) {
                _dispofftime_xga = disptime_xga - _dispontime_xga;
                _dispontime_xga *= crtcconst_xga;
                _dispofftime_xga *= crtcconst_xga;

                xga->dispontime  = (uint64_t) (_dispontime_xga);
                xga->dispofftime = (uint64_t) (_dispofftime_xga);
                if (xga->dispontime < GD5440_TIMER_USEC)
                    xga->dispontime = GD5440_TIMER_USEC;
                if (xga->dispofftime < GD5440_TIMER_USEC)
                    xga->dispofftime = GD5440_TIMER_USEC;

                gd5440_xga_set_poll(svga);
            } else
                gd5440_svga_set_poll(svga);
            break;

        case 3: /*Plus 8514/A and XGA*/
            if (dev->on) {
                _dispofftime8514 = disptime8514 - _dispontime8514;
                _dispontime8514 *= crtcconst8514;
                _dispofftime8514 *= crtcconst8514;

                dev->dispontime  = (uint64_t) (_dispontime8514);
                dev->dispofftime = (uint64_t) (_dispofftime8514);
                if (dev->dispontime < GD5440_TIMER_USEC)
                    dev->dispontime = GD5440_TIMER_USEC;
                if (dev->dispofftime < GD5440_TIMER_USEC)
                    dev->dispofftime = GD5440_TIMER_USEC;

                gd5440_ibm8514_set_poll(svga);
            } else if (xga->on) {
                _dispofftime_xga = disptime_xga - _dispontime_xga;
                _dispontime_xga *= crtcconst_xga;
                _dispofftime_xga *= crtcconst_xga;

                xga->dispontime  = (uint64_t) (_dispontime_xga);
                xga->dispofftime = (uint64_t) (_dispofftime_xga);
                if (xga->dispontime < GD5440_TIMER_USEC)
                    xga->dispontime = GD5440_TIMER_USEC;
                if (xga->dispofftime < GD5440_TIMER_USEC)
                    xga->dispofftime = GD5440_TIMER_USEC;

                gd5440_xga_set_poll(svga);
            } else
                gd5440_svga_set_poll(svga);
            break;
    }

    if (!svga->force_old_addr)
        gd5440_svga_recalc_remap_func(svga);

    /* Inform the user interface of any DPMS mode changes. */
    if (svga->dpms) {
        if (!svga->dpms_ui) {
            /* Make sure to black out the entire screen to avoid lingering image. */
            int y_add   = gd5440_enable_overscan ? svga->monitor->mon_overscan_y : 0;
            int x_add   = gd5440_enable_overscan ? svga->monitor->mon_overscan_x : 0;
            int y_start = gd5440_enable_overscan ? 0 : (svga->monitor->mon_overscan_y >> 1);
            int x_start = gd5440_enable_overscan ? 0 : (svga->monitor->mon_overscan_x >> 1);
            gd5440_video_wait_for_buffer_monitor(svga->monitor_index);
            memset(svga->monitor->target_buffer->dat, 0, (size_t) svga->monitor->target_buffer->w * svga->monitor->target_buffer->h * 4);
            gd5440_video_blit_memtoscreen_monitor(x_start, y_start, svga->monitor->mon_xsize + x_add, svga->monitor->mon_ysize + y_add, svga->monitor_index);
            gd5440_video_wait_for_buffer_monitor(svga->monitor_index);
            svga->dpms_ui = 1;
            gd5440_ui_sb_set_text_w(gd5440_plat_get_string(STRING_MONITOR_SLEEP));
        }
    } else if (svga->dpms_ui) {
        svga->dpms_ui = 0;
        gd5440_ui_sb_set_text_w(NULL);
    }

    if (gd5440_enable_overscan && (svga->monitor->mon_overscan_x != old_monitor_overscan_x || svga->monitor->mon_overscan_y != old_monitor_overscan_y))
        gd5440_video_force_resize_set_monitor(1, svga->monitor_index);

    svga->force_shifter_bypass = 0;
    if ((svga->hdisp == 320) && (svga->dispend >= 400) && !svga->override && (svga->render != gd5440_svga_render_8bpp_clone_highres)) {
        svga->hdisp <<= 1;
        if (svga->render == gd5440_svga_render_16bpp_highres)
            svga->render = gd5440_svga_render_16bpp_lowres;
        else if (svga->render == gd5440_svga_render_15bpp_highres)
            svga->render = gd5440_svga_render_15bpp_lowres;
        else if (svga->render == gd5440_svga_render_15bpp_mix_highres)
            svga->render = gd5440_svga_render_15bpp_mix_lowres;
        else if (svga->render == gd5440_svga_render_24bpp_highres)
            svga->render = gd5440_svga_render_24bpp_lowres;
        else if (svga->render == gd5440_svga_render_32bpp_highres)
            svga->render = gd5440_svga_render_32bpp_lowres;
        else if (svga->render == gd5440_svga_render_8bpp_highres) {
            svga->render = gd5440_svga_render_8bpp_lowres;
            svga->force_shifter_bypass = 1;
        }
        else
            svga->hdisp >>= 1;
    }

    svga->monitor->mon_interlace = 0;
    if (!svga->override) {
        switch (set_timer) {
            default:
            case 0: /*VGA only*/
                svga->monitor->mon_interlace = !!svga->interlace;
                break;
            case 1: /*Plus 8514/A*/
                if (dev->on)
                    svga->monitor->mon_interlace = !!dev->interlace;
                else
                    svga->monitor->mon_interlace = !!svga->interlace;
                break;
            case 2: /*Plus XGA*/
                if (xga->on)
                    svga->monitor->mon_interlace = !!xga->interlace;
                else
                    svga->monitor->mon_interlace = !!svga->interlace;
                break;
            case 3: /*Plus 8514/A and XGA*/
                if (dev->on)
                    svga->monitor->mon_interlace = !!dev->interlace;
                else if (xga->on)
                    svga->monitor->mon_interlace = !!xga->interlace;
                else
                    svga->monitor->mon_interlace = !!svga->interlace;
                break;
        }
    }
}

static void
gd5440_svga_do_render(gd5440_svga_t *svga)
{
    /* Always render a blank screen and nothing else while in DPMS mode. */
    if (svga->dpms) {
        gd5440_svga_render_blank(svga);
        return;
    }

    if (!svga->override) {
        svga->render_line_offset = svga->start_retrace_latch - svga->crtc[0x4];
        svga->render(svga);
    }

    if (svga->overlay_on) {
        if (!svga->override && svga->overlay_draw)
            svga->overlay_draw(svga, svga->displine + svga->y_add);
        svga->overlay_on--;
        if (svga->overlay_on && svga->interlace)
            svga->overlay_on--;
    }

    if (svga->dac_hwcursor_on) {
        if (!svga->override && svga->dac_hwcursor_draw)
            svga->dac_hwcursor_draw(svga, (svga->displine + svga->y_add + ((svga->dac_hwcursor_latch.y >= 0) ? 0 : svga->dac_hwcursor_latch.y)) & 2047);
        svga->dac_hwcursor_on--;
        if (svga->dac_hwcursor_on && svga->interlace)
            svga->dac_hwcursor_on--;
    }

    if (svga->hwcursor_on) {
        if (!svga->override && svga->hwcursor_draw)
            svga->hwcursor_draw(svga, (svga->displine + svga->y_add + ((svga->hwcursor_latch.y >= 0) ? 0 : svga->hwcursor_latch.y)) & 2047);

        svga->hwcursor_on--;
        if (svga->hwcursor_on && svga->interlace)
            svga->hwcursor_on--;
    }

    if (!svga->override) {
        svga->x_add = svga->left_overscan;
        gd5440_svga_render_overscan_left(svga);
        gd5440_svga_render_overscan_right(svga);
        svga->x_add = svga->left_overscan - svga->scrollcache;
    }
}

void
gd5440_svga_poll(void *priv)
{
    gd5440_svga_t    *svga = (gd5440_svga_t *) priv;
    uint32_t   x;
    uint32_t   blink_delay;
    int        wx;
    int        wy;
    int        ret;
    int        old_ma;

    gd5440_svga_log("SVGA Poll.\n");
    if (!svga->linepos) {
        if (svga->displine == ((svga->hwcursor_latch.y < 0) ? 0 : svga->hwcursor_latch.y) && svga->hwcursor_latch.ena) {
            svga->hwcursor_on      = svga->hwcursor_latch.cur_ysize - svga->hwcursor_latch.yoff;
            svga->hwcursor_oddeven = 0;
        }

        if (svga->displine == (((svga->hwcursor_latch.y < 0) ? 0 : svga->hwcursor_latch.y) + 1) && svga->hwcursor_latch.ena && svga->interlace) {
            svga->hwcursor_on      = svga->hwcursor_latch.cur_ysize - (svga->hwcursor_latch.yoff + 1);
            svga->hwcursor_oddeven = 1;
        }

        if (svga->displine == ((svga->dac_hwcursor_latch.y < 0) ? 0 : svga->dac_hwcursor_latch.y) && svga->dac_hwcursor_latch.ena) {
            svga->dac_hwcursor_on      = svga->dac_hwcursor_latch.cur_ysize - svga->dac_hwcursor_latch.yoff;
            svga->dac_hwcursor_oddeven = 0;
        }

        if (svga->displine == (((svga->dac_hwcursor_latch.y < 0) ? 0 : svga->dac_hwcursor_latch.y) + 1) && svga->dac_hwcursor_latch.ena && svga->interlace) {
            svga->dac_hwcursor_on      = svga->dac_hwcursor_latch.cur_ysize - (svga->dac_hwcursor_latch.yoff + 1);
            svga->dac_hwcursor_oddeven = 1;
        }

        if (svga->displine == svga->overlay_latch.y && svga->overlay_latch.ena) {
            svga->overlay_on      = svga->overlay_latch.cur_ysize - svga->overlay_latch.yoff;
            svga->overlay_oddeven = 0;
        }

        if (svga->displine == svga->overlay_latch.y + 1 && svga->overlay_latch.ena && svga->interlace) {
            svga->overlay_on      = svga->overlay_latch.cur_ysize - svga->overlay_latch.yoff;
            svga->overlay_oddeven = 1;
        }

        gd5440_timer_advance_u64(&svga->timer, svga->dispofftime);
        svga->cgastat |= 1;
        svga->linepos = 1;

        if (svga->dispon) {
            svga->hdisp_on = 1;

            svga->memaddr &= svga->vram_display_mask;
            if (svga->firstline == 2000) {
                svga->firstline = svga->displine;
                gd5440_video_wait_for_buffer_monitor(svga->monitor_index);
            }

            if (svga->hwcursor_on || svga->dac_hwcursor_on || svga->overlay_on)
                svga->changedvram[svga->memaddr >> 12] = svga->changedvram[(svga->memaddr >> 12) + 1] = svga->interlace ? 3 : 2;

            if (svga->vertical_linedbl) {
                old_ma = svga->memaddr;

                svga->displine <<= 1;
                svga->y_add <<= 1;

                gd5440_svga_do_render(svga);

                svga->displine++;

                svga->memaddr = old_ma;

                gd5440_svga_do_render(svga);

                svga->y_add >>= 1;
                svga->displine >>= 1;
            } else
                gd5440_svga_do_render(svga);

            if (svga->lastline < svga->displine)
                svga->lastline = svga->displine;
        }

        svga->displine++;
        if (svga->interlace)
            svga->displine++;
        if ((svga->cgastat & 8) && ((svga->displine & 15) == (svga->crtc[0x11] & 15)) && svga->vslines)
            svga->cgastat &= ~8;
        svga->vslines++;
        if (svga->displine > 2000)
            svga->displine = 0;
    } else {
        gd5440_timer_advance_u64(&svga->timer, svga->dispontime);

        if (svga->adv_flags & FLAG_PANNING_ATI) {
            if (svga->panning_blank) {
                svga->scrollcache = 0;
                svga->half_pixel  = 0;

                svga->x_add       = svga->left_overscan;
            } else {
                svga->scrollcache = (svga->attrregs[0x13] & 0x0f);
                if (!(svga->gdcreg[6] & 1) && !(svga->attrregs[0x10] & 1)) { /*Text mode*/
                    if (svga->seqregs[1] & 1)
                        svga->scrollcache &= 0x07;
                    else {
                        svga->scrollcache++;
                        if (svga->scrollcache > 8)
                            svga->scrollcache = 0;
                    }
                    svga->half_pixel  = 0;
                } else if ((svga->render == gd5440_svga_render_2bpp_lowres) || (svga->render == gd5440_svga_render_2bpp_highres) ||
                           (svga->render == gd5440_svga_render_4bpp_lowres) || (svga->render == gd5440_svga_render_4bpp_highres)) {
                    svga->half_pixel  = 0;
                    svga->scrollcache &= 0x07;
                } else {
                    if (svga->scrollcache > 7)
                        svga->scrollcache = 7;
                    svga->half_pixel  = svga->scrollcache & 0x01;
                    svga->scrollcache = (svga->scrollcache & 0x06) >> 1;
                }

                if ((svga->seqregs[1] & 8) || (svga->render == gd5440_svga_render_8bpp_lowres))
                    svga->scrollcache <<= 1;

                svga->x_add = svga->left_overscan - svga->scrollcache;
            }
        }

        if (svga->dispon)
            svga->cgastat &= ~1;
        svga->hdisp_on = 0;

        svga->linepos = 0;
        if ((svga->scanline == (svga->crtc[11] & 31)) || (svga->scanline == svga->rowcount))
            svga->cursorvisible = 0;
        if (svga->dispon) {
            /* TODO: Verify real hardware behaviour for out-of-range fine vertical scroll
               - S3 Trio64V2/DX: scanline == rowcount, wrapping 5-bit counter. */
            if (svga->linedbl && !svga->linecountff) {
                svga->linecountff = 1;
                svga->memaddr          = svga->memaddr_backup;
            } else if (svga->scanline == svga->rowcount) {
                svga->linecountff = 0;
                svga->scanline          = 0;

                svga->memaddr_backup += (svga->adv_flags & FLAG_NO_SHIFT3) ? svga->rowoffset : (svga->rowoffset << 3);
                if (svga->interlace)
                    svga->memaddr_backup += (svga->adv_flags & FLAG_NO_SHIFT3) ? svga->rowoffset : (svga->rowoffset << 3);

                svga->memaddr_backup &= svga->vram_display_mask;
                svga->memaddr = svga->memaddr_backup;
            } else {
                svga->linecountff = 0;
                svga->scanline++;
                svga->scanline &= 0x1f;
                svga->memaddr = svga->memaddr_backup;
            }
        }

        svga->hsync_divisor ^= 1;

        if (svga->hsync_divisor && (svga->crtc[0x17] & 4))
            return;

        svga->vc++;
        svga->vc &= 0x7ff;

        if (svga->vc == svga->split) {
            ret = 1;

            if (svga->line_compare)
                ret = svga->line_compare(svga);

            if (ret) {
                if (svga->interlace && svga->oddeven)
                    svga->memaddr = svga->memaddr_backup = (svga->rowoffset << 1) + svga->hblank_sub;
                else
                    svga->memaddr = svga->memaddr_backup = svga->hblank_sub;

                svga->memaddr     = (svga->memaddr << 2);
                svga->memaddr_backup = (svga->memaddr_backup << 2);

                svga->scanline = 0;
                if (svga->attrregs[0x10] & 0x20) {
                    if (svga->adv_flags & FLAG_PANNING_ATI)
                        svga->panning_blank = 1;
                    else {
                        svga->scrollcache   = 0;
                        svga->half_pixel    = 0;
                        svga->x_add         = svga->left_overscan;
                    }
                }
            }
        }
        if (svga->vc == svga->dispend) {
            if (svga->vblank_start)
                svga->vblank_start(svga);

            svga->dispon = 0;
            blink_delay  = (svga->crtc[11] & 0x60) >> 5;
            if (svga->crtc[10] & 0x20)
                svga->cursoron = 0;
            else if (blink_delay == 2)
                svga->cursoron = ((svga->blink % 96) >= 48);
            else
                svga->cursoron = svga->blink & (16 + (16 * blink_delay));

            if (!(svga->blink & 15))
                svga->fullchange = 2;

            svga->blink = (svga->blink + 1) & 0x7f;

            for (x = 0; x < ((svga->vram_mask + 1) >> 12); x++) {
                if (svga->changedvram[x])
                    svga->changedvram[x]--;
            }

            if (svga->fullchange)
                svga->fullchange--;
        }
        if (svga->vc == svga->vsyncstart) {
            svga->dispon = 0;
            svga->cgastat |= 8;
            x = svga->hdisp;

            if (svga->interlace && !svga->oddeven)
                svga->lastline++;
            if (svga->interlace && svga->oddeven)
                svga->firstline--;

            wx = x;

            if (!svga->override) {
                if (svga->vertical_linedbl) {
                    wy = (svga->lastline - svga->firstline) << 1;
                    svga->vdisp = wy + 1;
                    gd5440_svga_doblit(wx, wy, svga);
                } else {
                    wy = svga->lastline - svga->firstline;
                    svga->vdisp = wy + 1;
                    gd5440_svga_doblit(wx, wy, svga);
                }
            }

            svga->firstline = 2000;
            svga->lastline  = 0;

            svga->firstline_draw = 2000;
            svga->lastline_draw  = 0;

            svga->oddeven ^= 1;

            svga->monitor->mon_changeframecount = svga->interlace ? 3 : 2;
            svga->vslines                       = 0;

            if (svga->interlace && svga->oddeven)
                svga->memaddr = svga->memaddr_backup = svga->memaddr_latch + (svga->rowoffset << 1) + svga->hblank_sub;
            else
                svga->memaddr = svga->memaddr_backup = svga->memaddr_latch + svga->hblank_sub;

            svga->cursoraddr     = ((svga->crtc[0xe] << 8) | svga->crtc[0xf]) + ((svga->crtc[0xb] & 0x60) >> 5) + svga->ca_adj;
            if (!(svga->adv_flags & FLAG_NO_SHIFT3)) {
                svga->memaddr     = (svga->memaddr << 2);
                svga->memaddr_backup = (svga->memaddr_backup << 2);
            }
            svga->cursoraddr     = (svga->cursoraddr << 2);

            if (svga->vsync_callback)
                svga->vsync_callback(svga);

            svga->start_retrace_latch = svga->crtc[0x4];
        }
#if 0
        if (svga->vc == lines_num) {
#endif
        if (svga->vc == svga->vtotal) {
            svga->vc       = 0;
            svga->scanline       = (svga->crtc[0x8] & 0x1f);
            svga->dispon   = 1;
            svga->displine = (svga->interlace && svga->oddeven) ? 1 : 0;

            if ((svga->adv_flags & FLAG_PANNING_ATI) && svga->panning_blank) {
                svga->scrollcache = 0;
                svga->half_pixel  = 0;

                svga->x_add       = svga->left_overscan;
            } else {
                svga->scrollcache = (svga->attrregs[0x13] & 0x0f);
                if (!(svga->gdcreg[6] & 1) && !(svga->attrregs[0x10] & 1)) { /*Text mode*/
                    if (svga->seqregs[1] & 1)
                        svga->scrollcache &= 0x07;
                    else {
                        svga->scrollcache++;
                        if (svga->scrollcache > 8)
                            svga->scrollcache = 0;
                    }
                    svga->half_pixel  = 0;
                } else if ((svga->render == gd5440_svga_render_2bpp_lowres) || (svga->render == gd5440_svga_render_2bpp_highres) ||
                           (svga->render == gd5440_svga_render_4bpp_lowres) || (svga->render == gd5440_svga_render_4bpp_highres)) {
                    svga->half_pixel  = 0;
                    svga->scrollcache &= 0x07;
                } else {
                    if (svga->scrollcache > 7)
                        svga->scrollcache = 7;
                    svga->half_pixel  = svga->scrollcache & 0x01;
                    svga->scrollcache = (svga->scrollcache & 0x06) >> 1;
                }

                if ((svga->seqregs[1] & 8) || (svga->render == gd5440_svga_render_8bpp_lowres))
                    svga->scrollcache <<= 1;

                svga->x_add = svga->left_overscan - svga->scrollcache;
            }

            svga->linecountff = 0;

            svga->hwcursor_on    = 0;
            svga->hwcursor_latch = svga->hwcursor;

            svga->dac_hwcursor_on    = 0;
            svga->dac_hwcursor_latch = svga->dac_hwcursor;

            svga->overlay_on    = 0;
            svga->overlay_latch = svga->overlay;
        }
        if (svga->scanline == (svga->crtc[10] & 31))
            svga->cursorvisible = 1;
    }
}

uint32_t
gd5440_svga_conv_16to32(UNUSED(struct gd5440_svga_t *svga), uint16_t color, uint8_t bpp)
{
    return (bpp == 15) ? gd5440_video_15to32[color] : gd5440_video_16to32[color];
}

int
gd5440_svga_init(const device_t *info, gd5440_svga_t *svga, void *priv, int memsize,
          void (*recalctimings_ex)(struct gd5440_svga_t *svga),
          uint8_t (*gd5440_video_in)(uint16_t addr, void *priv),
          void (*gd5440_video_out)(uint16_t addr, uint8_t val, void *priv),
          void (*hwcursor_draw)(struct gd5440_svga_t *svga, int displine),
          void (*overlay_draw)(struct gd5440_svga_t *svga, int displine))
{
    svga->priv          = priv;
    svga->monitor_index = gd5440_monitor_index_global;
    svga->monitor       = &gd5440_monitors[svga->monitor_index];

    svga->readmode = 0;

    svga->attrregs[0x11] = 0;
    svga->overscan_color = 0x000000;

    svga->left_overscan           = 8;
    svga->monitor->mon_overscan_x = 16;
    svga->monitor->mon_overscan_y = 32;
    svga->x_add                   = 8;
    svga->y_add                   = 16;
    svga->force_shifter_bypass    = 1;

    svga->crtc[0]           = 63;
    svga->crtc[6]           = 255;
    svga->dispontime        = 1000ULL << 32;
    svga->dispofftime       = 1000ULL << 32;
    svga->bpp               = 8;
    svga->vram              = calloc(memsize + 4096, 1);
    svga->vram_max          = memsize;
    svga->vram_display_mask = svga->vram_mask = memsize - 1;
    svga->decode_mask                         = 0x7fffff;
    svga->changedvram                         = calloc((memsize >> 12) + 1, 1);
    svga->recalctimings_ex                    = recalctimings_ex;
    svga->gd5440_video_in                            = gd5440_video_in;
    svga->gd5440_video_out                           = gd5440_video_out;
    svga->hwcursor_draw                       = hwcursor_draw;
    svga->overlay_draw                        = overlay_draw;
    svga->conv_16to32                         = gd5440_svga_conv_16to32;
    svga->render                              = gd5440_svga_render_blank;

    svga->hwcursor.cur_xsize = svga->hwcursor.cur_ysize = 32;

    svga->dac_hwcursor.cur_xsize = svga->dac_hwcursor.cur_ysize = 32;

    svga->translate_address         = NULL;

    svga->cable_connected = 1;
    svga->ksc5601_english_font_type = 0;

    /* TODO: Move DEVICE_MCA to 16-bit once the device flags have been appropriately corrected. */
    if ((info->flags & DEVICE_MCA) || (info->flags & DEVICE_MCA32) ||
        (info->flags & DEVICE_EISA) || (info->flags & DEVICE_AT32) ||
        (info->flags & DEVICE_OLB) || (info->flags & DEVICE_VLB) ||
        (info->flags & DEVICE_PCI) || (info->flags & DEVICE_AGP)) {
        svga->read = gd5440_svga_read;
        svga->readw = gd5440_svga_readw;
        svga->readl = gd5440_svga_readl;
        svga->write = gd5440_svga_write;
        svga->writew = gd5440_svga_writew;
        svga->writel = gd5440_svga_writel;
        gd5440_mem_mapping_add(&svga->mapping, 0xa0000, 0x20000,
                        gd5440_svga_read, gd5440_svga_readw, gd5440_svga_readl,
                        gd5440_svga_write, gd5440_svga_writew, gd5440_svga_writel,
                        NULL, MEM_MAPPING_EXTERNAL, svga);
    /* The chances of ever seeing a C-BUS (S)VGA card are approximately zero, but you never know. */
    } else if ((info->flags & DEVICE_CBUS) || (info->flags & DEVICE_ISA16)) {
        svga->read = gd5440_svga_read;
        svga->readw = gd5440_svga_readw;
        svga->readl = NULL;
        svga->write = gd5440_svga_write;
        svga->writew = gd5440_svga_writew;
        svga->writel = NULL;
        gd5440_mem_mapping_add(&svga->mapping, 0xa0000, 0x20000,
                        gd5440_svga_read, gd5440_svga_readw, NULL,
                        gd5440_svga_write, gd5440_svga_writew, NULL,
                        NULL, MEM_MAPPING_EXTERNAL, svga);
    } else {
        svga->read = gd5440_svga_read;
        svga->readw = NULL;
        svga->readl = NULL;
        svga->write = gd5440_svga_write;
        svga->writew = NULL;
        svga->writel = NULL;
        gd5440_mem_mapping_add(&svga->mapping, 0xa0000, 0x20000,
                        gd5440_svga_read, NULL, NULL,
                        gd5440_svga_write, NULL, NULL,
                        NULL, MEM_MAPPING_EXTERNAL, svga);
    }

    gd5440_timer_add(&svga->timer, gd5440_svga_poll, svga, 1);

    gd5440_svga_pri = svga;

    svga->ramdac_type = RAMDAC_6BIT;

    svga->map8            = svga->pallook;

    return 0;
}

void
gd5440_svga_close(gd5440_svga_t *svga)
{
    free(svga->changedvram);
    free(svga->vram);

    if (svga->dpms_ui)
        gd5440_ui_sb_set_text_w(NULL);

    gd5440_svga_pri = NULL;
}

uint32_t
gd5440_svga_decode_addr(gd5440_svga_t *svga, uint32_t addr, int write)
{
    int memory_map_mode = (svga->gdcreg[6] >> 2) & 3;

    addr &= 0x1ffff;

    switch (memory_map_mode) {
        case 0:
            break;
        case 1:
            if (addr >= 0x10000)
                return 0xffffffff;
            break;
        case 2:
            addr -= 0x10000;
            if (addr >= 0x8000)
                return 0xffffffff;
            break;
        default:
        case 3:
            addr -= 0x18000;
            if (addr >= 0x8000)
                return 0xffffffff;
            break;
    }

    if (memory_map_mode <= 1) {
        if (write)
            addr += svga->write_bank;
        else
            addr += svga->read_bank;
    }

    return addr;
}

static __inline void
gd5440_svga_write_common(uint32_t addr, uint8_t val, uint8_t linear, void *priv)
{
    gd5440_svga_t *svga       = (gd5440_svga_t *) priv;
    int     writemask2 = svga->writemask;
    int     reset_wm   = 0;
    latch_t vall;
    uint8_t wm         = svga->writemask;
    uint8_t count;
    uint8_t i;

    if (svga->adv_flags & FLAG_ADDR_BY8)
        writemask2 = svga->seqregs[2];

    gd5440_cycles -= svga->monitor->mon_video_timing_write_b;

    if (!linear) {
        gd5440_xga_write_test(addr, val, svga);
        addr = gd5440_svga_decode_addr(svga, addr, 1);
        if (addr == 0xffffffff) {
            gd5440_svga_log("WriteCommon Over.\n");
            return;
        }
    }

    if (!(svga->gdcreg[6] & 1))
        svga->fullchange = 2;

    if ((svga->adv_flags & FLAG_ADDR_BY16) && (svga->writemode == 4 || svga->writemode == 5))
        addr <<= 4;
    else if ((svga->adv_flags & FLAG_ADDR_BY8) && (svga->writemode < 4))
        addr <<= 3;
    else if (((svga->chain4 && (svga->packed_chain4 || svga->force_old_addr)) || svga->fb_only) && (svga->writemode < 4)) {
        writemask2 = 1 << (addr & 3);
        addr &= ~3;
    } else if (svga->chain4 && (svga->writemode < 4)) {
        writemask2 = 1 << (addr & 3);
        if (!linear)
            addr &= ~3;
        addr = ((addr & 0xfffc) << 2) | ((addr & 0x30000) >> 14) | (addr & ~0x3ffff);
    } else if (svga->chain2_write) {
        writemask2 &= 0x5 << (addr & 1);
        addr &= ~1;
        addr <<= 2;
    } else
        addr <<= 2;

    addr &= svga->decode_mask;

    if (svga->translate_address)
        addr = svga->translate_address(addr, priv);

    if (addr >= svga->vram_max) {
        gd5440_svga_log("WriteBankedOver=%08x, val=%02x.\n", addr & svga->vram_mask, val);
        return;
    }

    addr &= svga->vram_mask;

    svga->changedvram[addr >> 12] = svga->monitor->mon_changeframecount;

    count = 4;
    if (svga->adv_flags & FLAG_LATCH8)
        count = 8;

    /* Undocumented Cirrus Logic behavior: The datasheet says that, with EXT_WRITE and FLAG_ADDR_BY8, the write mask only
       changes meaning in write modes 4 and 5, as well as write mode 1. In reality, however, all other write modes are also
       affected, as proven by the Windows 3.1 CL-GD 5422/4 drivers in 8bpp modes. */
    switch (svga->writemode) {
        case 0:
            val = ((val >> (svga->gdcreg[3] & 7)) | (val << (8 - (svga->gdcreg[3] & 7))));
            if ((svga->gdcreg[8] == 0xff) && !(svga->gdcreg[3] & 0x18) && (!svga->gdcreg[1] || svga->set_reset_disabled)) {
                for (i = 0; i < count; i++) {
                    if ((svga->adv_flags & FLAG_EXT_WRITE) && (svga->adv_flags & FLAG_ADDR_BY8)) {
                        if (writemask2 & (0x80 >> i))
                            svga->vram[addr | i] = val;
                    } else {
                        if (writemask2 & (1 << i))
                            svga->vram[addr | i] = val;
                    }
                }
                return;
            } else {
                for (i = 0; i < count; i++) {
                    if (svga->gdcreg[1] & (1 << i))
                        vall.b[i] = !!(svga->gdcreg[0] & (1 << i)) * 0xff;
                    else
                        vall.b[i] = val;
                }
            }
            break;
        case 1:
            for (i = 0; i < count; i++) {
                if ((svga->adv_flags & FLAG_EXT_WRITE) && (svga->adv_flags & FLAG_ADDR_BY8)) {
                    if (writemask2 & (0x80 >> i))
                        svga->vram[addr | i] = svga->latch.b[i];
                } else {
                    if (writemask2 & (1 << i))
                        svga->vram[addr | i] = svga->latch.b[i];
                }
            }
            return;
        case 2:
            for (i = 0; i < count; i++)
                vall.b[i] = !!(val & (1 << i)) * 0xff;

            if (!(svga->gdcreg[3] & 0x18) && (!svga->gdcreg[1] || svga->set_reset_disabled)) {
                for (i = 0; i < count; i++) {
                    if ((svga->adv_flags & FLAG_EXT_WRITE) && (svga->adv_flags & FLAG_ADDR_BY8)) {
                        if (writemask2 & (0x80 >> i))
                            svga->vram[addr | i] = (vall.b[i] & svga->gdcreg[8]) | (svga->latch.b[i] & ~svga->gdcreg[8]);
                    } else {
                        if (writemask2 & (1 << i))
                            svga->vram[addr | i] = (vall.b[i] & svga->gdcreg[8]) | (svga->latch.b[i] & ~svga->gdcreg[8]);
                    }
                }
                return;
            }
            break;
        case 3:
            val = ((val >> (svga->gdcreg[3] & 7)) | (val << (8 - (svga->gdcreg[3] & 7))));
            wm  = svga->gdcreg[8];
            svga->gdcreg[8] &= val;

            for (i = 0; i < count; i++)
                vall.b[i] = !!(svga->gdcreg[0] & (1 << i)) * 0xff;

            reset_wm = 1;
            break;
        default:
            if (svga->ven_write)
                svga->ven_write(svga, val, addr);
            return;
    }

    switch (svga->gdcreg[3] & 0x18) {
        case 0x00: /* Set */
            for (i = 0; i < count; i++) {
                if ((svga->adv_flags & FLAG_EXT_WRITE) && (svga->adv_flags & FLAG_ADDR_BY8)) {
                    if (writemask2 & (0x80 >> i))
                        svga->vram[addr | i] = (vall.b[i] & svga->gdcreg[8]) | (svga->latch.b[i] & ~svga->gdcreg[8]);
                } else {
                    if (writemask2 & (1 << i))
                        svga->vram[addr | i] = (vall.b[i] & svga->gdcreg[8]) | (svga->latch.b[i] & ~svga->gdcreg[8]);
                }
            }
            break;
        case 0x08: /* AND */
            for (i = 0; i < count; i++) {
                if ((svga->adv_flags & FLAG_EXT_WRITE) && (svga->adv_flags & FLAG_ADDR_BY8)) {
                    if (writemask2 & (0x80 >> i))
                        svga->vram[addr | i] = (vall.b[i] | ~svga->gdcreg[8]) & svga->latch.b[i];
                } else {
                    if (writemask2 & (1 << i))
                        svga->vram[addr | i] = (vall.b[i] | ~svga->gdcreg[8]) & svga->latch.b[i];
                }
            }
            break;
        case 0x10: /* OR */
            for (i = 0; i < count; i++) {
                if ((svga->adv_flags & FLAG_EXT_WRITE) && (svga->adv_flags & FLAG_ADDR_BY8)) {
                    if (writemask2 & (0x80 >> i))
                        svga->vram[addr | i] = (vall.b[i] & svga->gdcreg[8]) | svga->latch.b[i];
                } else {
                    if (writemask2 & (1 << i))
                        svga->vram[addr | i] = (vall.b[i] & svga->gdcreg[8]) | svga->latch.b[i];
                }
            }
            break;
        case 0x18: /* XOR */
            for (i = 0; i < count; i++) {
                if ((svga->adv_flags & FLAG_EXT_WRITE) && (svga->adv_flags & FLAG_ADDR_BY8)) {
                    if (writemask2 & (0x80 >> i))
                        svga->vram[addr | i] = (vall.b[i] & svga->gdcreg[8]) ^ svga->latch.b[i];
                } else {
                    if (writemask2 & (1 << i))
                        svga->vram[addr | i] = (vall.b[i] & svga->gdcreg[8]) ^ svga->latch.b[i];
                }
            }
            break;

        default:
            break;
    }

    if (reset_wm)
        svga->gdcreg[8] = wm;
}

static __inline uint8_t
gd5440_svga_read_common(uint32_t addr, uint8_t linear, void *priv)
{
    gd5440_svga_t  *svga       = (gd5440_svga_t *) priv;
    uint32_t latch_addr = 0;
    int      readplane  = svga->readplane;
    uint8_t  count;
    uint8_t  temp;
    uint8_t  ret = 0x00;

    if (svga->adv_flags & FLAG_ADDR_BY8)
        readplane = svga->gdcreg[4] & 7;

    gd5440_cycles -= svga->monitor->mon_video_timing_read_b;

    if (!linear) {
        (void) gd5440_xga_read_test(addr, svga);
        addr = gd5440_svga_decode_addr(svga, addr, 0);
        if (addr == 0xffffffff)
            return 0xff;
    }

    count = 2;
    if (svga->adv_flags & FLAG_LATCH8)
        count = 3;

    latch_addr = (addr << count) & svga->decode_mask;
    count      = (1 << count);

    if (svga->adv_flags & FLAG_ADDR_BY16)
        addr <<= 4;
    else if (svga->adv_flags & FLAG_ADDR_BY8)
        addr <<= 3;
    else if ((svga->chain4 && (svga->packed_chain4 || svga->force_old_addr)) || svga->fb_only) {
        addr &= svga->decode_mask;
        if (svga->translate_address)
            addr = svga->translate_address(addr, priv);
        if (addr >= svga->vram_max)
            return 0xff;
        latch_addr = (addr & svga->vram_mask) & ~3;
        for (uint8_t i = 0; i < count; i++)
            svga->latch.b[i] = svga->vram[latch_addr | i];
        return svga->vram[addr & svga->vram_mask];
    } else if (svga->chain4 && !svga->force_old_addr) {
        readplane = addr & 3;
        addr      = ((addr & 0xfffc) << 2) | ((addr & 0x30000) >> 14) | (addr & ~0x3ffff);
    } else if (svga->chain2_read) {
        readplane = (readplane & 2) | (addr & 1);
        addr &= ~1;
        addr <<= 2;
    } else
        addr <<= 2;

    addr &= svga->decode_mask;

    if (svga->translate_address) {
        latch_addr = svga->translate_address(latch_addr, priv);
        addr       = svga->translate_address(addr, priv);
    }

    /* standard VGA latched access */
    if (latch_addr >= svga->vram_max) {
        for (uint8_t i = 0; i < count; i++)
            svga->latch.b[i] = 0xff;
    } else {
        latch_addr &= svga->vram_mask;

        for (uint8_t i = 0; i < count; i++)
            svga->latch.b[i] = svga->vram[latch_addr | i];
    }

    if (addr >= svga->vram_max)
        return 0xff;

    addr &= svga->vram_mask;

    if (svga->readmode) {
        temp = 0xff;

        for (uint8_t pixel = 0; pixel < 8; pixel++) {
            for (uint8_t plane = 0; plane < count; plane++) {
                if (svga->colournocare & (1 << plane)) {
                    /* If we care about a plane, and the pixel has a mismatch on it, clear its bit. */
                    if (((svga->latch.b[plane] >> pixel) & 1) != ((svga->colourcompare >> plane) & 1))
                        temp &= ~(1 << pixel);
                }
            }
        }

        ret = temp;
    } else
        ret = svga->vram[addr | readplane];

    return ret;
}

void
gd5440_svga_write(uint32_t addr, uint8_t val, void *priv)
{
    gd5440_svga_write_common(addr, val, 0, priv);
}

void
gd5440_svga_write_linear(uint32_t addr, uint8_t val, void *priv)
{
    gd5440_svga_write_common(addr, val, 1, priv);
}

uint8_t
gd5440_svga_read(uint32_t addr, void *priv)
{
    return gd5440_svga_read_common(addr, 0, priv);
}

uint8_t
gd5440_svga_read_linear(uint32_t addr, void *priv)
{
    return gd5440_svga_read_common(addr, 1, priv);
}

void
gd5440_svga_doblit(int wx, int wy, gd5440_svga_t *svga)
{
    int       y_add;
    int       x_add;
    int       y_start;
    int       x_start;
    int       bottom;
    uint32_t *p;
    int       i;
    int       j;
    int       xs_temp;
    int       ys_temp;

    y_add   = gd5440_enable_overscan ? svga->monitor->mon_overscan_y : 0;
    x_add   = gd5440_enable_overscan ? svga->monitor->mon_overscan_x : 0;
#ifdef USE_OLD_CALCULATION
    y_start = gd5440_enable_overscan ? 0 : (svga->monitor->mon_overscan_y >> 1);
    x_start = gd5440_enable_overscan ? 0 : (svga->monitor->mon_overscan_x >> 1);
    bottom  = (svga->monitor->mon_overscan_y >> 1);
#else
    y_start = gd5440_enable_overscan ? 0 : svga->y_add;
    x_start = gd5440_enable_overscan ? 0 : svga->left_overscan;
    bottom  = svga->monitor->mon_overscan_y - svga->y_add;
#endif

    if (svga->vertical_linedbl) {
        y_add <<= 1;
        y_start <<= 1;
        bottom <<= 1;
    }

    if ((wx <= 0) || (wy <= 0))
        return;

    if (svga->vertical_linedbl)
        svga->y_add <<= 1;

    xs_temp = wx;
    ys_temp = wy + 1;
    if (svga->vertical_linedbl)
        ys_temp++;
    if (xs_temp < 64)
        xs_temp = 640;
    if (ys_temp < 32)
        ys_temp = 200;

    if ((svga->crtc[0x17] & 0x80) && ((xs_temp != svga->monitor->mon_xsize) || (ys_temp != svga->monitor->mon_ysize) || gd5440_video_force_resize_get_monitor(svga->monitor_index))) {
        /* Screen res has changed.. fix up, and let them know. */
        svga->monitor->mon_xsize = xs_temp;
        svga->monitor->mon_ysize = ys_temp;

        if ((svga->monitor->mon_xsize > 1984) || (svga->monitor->mon_ysize > 2016)) {
            /* 2048x2048 is the biggest safe render texture, to account for overscan,
               we suppress overscan starting from x 1984 and y 2016. */
            x_add             = 0;
            y_add             = 0;
            gd5440_suppress_overscan = 1;
        } else
            gd5440_suppress_overscan = 0;

        /* Block resolution changes while in DPMS mode to avoid getting a bogus
           screen width (320). We're already rendering a blank screen anyway. */
        if (!svga->dpms)
            set_screen_size_monitor(svga->monitor->mon_xsize + x_add, svga->monitor->mon_ysize + y_add, svga->monitor_index);

        if (gd5440_video_force_resize_get_monitor(svga->monitor_index))
            gd5440_video_force_resize_set_monitor(0, svga->monitor_index);
    }

    if ((wx >= 160) && ((wy + 1) >= 120)) {
        /* Draw (overscan_size - scroll size) lines of overscan on top and bottom. */
        for (i = 0; i < svga->y_add; i++) {
            p = &svga->monitor->target_buffer->line[i & 0x7ff][0];

            for (j = 0; j < (svga->monitor->mon_xsize + x_add); j++)
                p[j] = svga->dpms ? 0 : svga->overscan_color;
        }

        for (i = 0; i < bottom; i++) {
            p = &svga->monitor->target_buffer->line[(svga->monitor->mon_ysize + svga->y_add + i) & 0x7ff][0];

            for (j = 0; j < (svga->monitor->mon_xsize + x_add); j++)
                p[j] = svga->dpms ? 0 : svga->overscan_color;
        }
    }

    gd5440_video_blit_memtoscreen_monitor(x_start, y_start, svga->monitor->mon_xsize + x_add, svga->monitor->mon_ysize + y_add, svga->monitor_index);

    if (svga->vertical_linedbl)
        svga->vertical_linedbl >>= 1;
}

void
gd5440_svga_writeb_linear(uint32_t addr, uint8_t val, void *priv)
{
    gd5440_svga_t *svga = (gd5440_svga_t *) priv;

    if (!svga->fast) {
        gd5440_svga_write_linear(addr, val, priv);
        return;
    }

    addr &= svga->decode_mask;
    if (addr >= svga->vram_max)
        return;
    addr &= svga->vram_mask;
    svga->changedvram[addr >> 12] = svga->monitor->mon_changeframecount;
    svga->vram[addr]              = val;
}

void
gd5440_svga_writew_common(uint32_t addr, uint16_t val, uint8_t linear, void *priv)
{
    gd5440_svga_t *svga = (gd5440_svga_t *) priv;

    if (!svga->fast) {
        gd5440_svga_write_common(addr, (uint8_t) val, linear, priv);
        gd5440_svga_write_common(addr + 1, (uint8_t) (val >> 8), linear, priv);
        return;
    }

    gd5440_cycles -= svga->monitor->mon_video_timing_write_w;

    if (!linear) {
        gd5440_xga_write_test(addr, val & 0xff, svga);
        gd5440_xga_write_test(addr + 1, val >> 8, svga);
        addr = gd5440_svga_decode_addr(svga, addr, 1);

        if (addr == 0xffffffff)
            return;
    }

    addr &= svga->decode_mask;
    if (svga->translate_address) {
        uint32_t addr2 = svga->translate_address(addr, priv);
        if (addr2 < svga->vram_max) {
            svga->vram[addr2 & svga->vram_mask] = val & 0xff;
            svga->changedvram[addr2 >> 12]      = svga->monitor->mon_changeframecount;
        }
        addr2 = svga->translate_address(addr + 1, priv);
        if (addr2 < svga->vram_max) {
            svga->vram[addr2 & svga->vram_mask] = (val >> 8) & 0xff;
            svga->changedvram[addr2 >> 12]      = svga->monitor->mon_changeframecount;
        }
        return;
    }
    if (addr >= svga->vram_max)
        return;
    addr &= svga->vram_mask;

    svga->changedvram[addr >> 12]   = svga->monitor->mon_changeframecount;
    gd5440_write_le16(&svga->vram[addr], val);
}

void
gd5440_svga_writew(uint32_t addr, uint16_t val, void *priv)
{
    gd5440_svga_writew_common(addr, val, 0, priv);
}

void
gd5440_svga_writew_linear(uint32_t addr, uint16_t val, void *priv)
{
    gd5440_svga_writew_common(addr, val, 1, priv);
}

void
gd5440_svga_writel_common(uint32_t addr, uint32_t val, uint8_t linear, void *priv)
{
    gd5440_svga_t *svga = (gd5440_svga_t *) priv;

    if (!svga->fast) {
        gd5440_svga_write_common(addr, val, linear, priv);
        gd5440_svga_write_common(addr + 1, val >> 8, linear, priv);
        gd5440_svga_write_common(addr + 2, val >> 16, linear, priv);
        gd5440_svga_write_common(addr + 3, val >> 24, linear, priv);
        return;
    }

    gd5440_cycles -= svga->monitor->mon_video_timing_write_l;

    if (!linear) {
        gd5440_xga_write_test(addr, val & 0xff, svga);
        gd5440_xga_write_test(addr + 1, (val >> 8) & 0xff, svga);
        gd5440_xga_write_test(addr + 2, (val >> 16) & 0xff, svga);
        gd5440_xga_write_test(addr + 3, (val >> 24) & 0xff, svga);
        addr = gd5440_svga_decode_addr(svga, addr, 1);

        if (addr == 0xffffffff)
            return;
    }

    addr &= svga->decode_mask;
    if (svga->translate_address) {
        uint32_t addr2 = svga->translate_address(addr, priv);
        if (addr2 < svga->vram_max) {
            svga->vram[addr2 & svga->vram_mask] = val & 0xff;
            svga->changedvram[addr2 >> 12]      = svga->monitor->mon_changeframecount;
        }
        addr2 = svga->translate_address(addr + 1, priv);
        if (addr2 < svga->vram_max) {
            svga->vram[addr2 & svga->vram_mask] = (val >> 8) & 0xff;
            svga->changedvram[addr2 >> 12]      = svga->monitor->mon_changeframecount;
        }
        addr2 = svga->translate_address(addr + 2, priv);
        if (addr2 < svga->vram_max) {
            svga->vram[addr2 & svga->vram_mask] = (val >> 16) & 0xff;
            svga->changedvram[addr2 >> 12]      = svga->monitor->mon_changeframecount;
        }
        addr2 = svga->translate_address(addr + 3, priv);
        if (addr2 < svga->vram_max) {
            svga->vram[addr2 & svga->vram_mask] = (val >> 24) & 0xff;
            svga->changedvram[addr2 >> 12]      = svga->monitor->mon_changeframecount;
        }
        return;
    }
    if (addr >= svga->vram_max)
        return;

    addr &= svga->vram_mask;

    svga->changedvram[addr >> 12]   = svga->monitor->mon_changeframecount;
    gd5440_write_le32(&svga->vram[addr], val);
}

void
gd5440_svga_writel(uint32_t addr, uint32_t val, void *priv)
{
    gd5440_svga_writel_common(addr, val, 0, priv);
}

void
gd5440_svga_writel_linear(uint32_t addr, uint32_t val, void *priv)
{
    gd5440_svga_writel_common(addr, val, 1, priv);
}

uint8_t
gd5440_svga_readb_linear(uint32_t addr, void *priv)
{
    const gd5440_svga_t *svga = (gd5440_svga_t *) priv;

    if (!svga->fast)
        return gd5440_svga_read_linear(addr, priv);

    addr &= svga->decode_mask;
    if (addr >= svga->vram_max)
        return 0xff;

    return svga->vram[addr & svga->vram_mask];
}

uint16_t
gd5440_svga_readw_common(uint32_t addr, uint8_t linear, void *priv)
{
    gd5440_svga_t *svga = (gd5440_svga_t *) priv;

    if (!svga->fast)
        return gd5440_svga_read_common(addr, linear, priv) | (gd5440_svga_read_common(addr + 1, linear, priv) << 8);

    gd5440_cycles -= svga->monitor->mon_video_timing_read_w;

    if (!linear) {
        (void) gd5440_xga_read_test(addr, svga);
        (void) gd5440_xga_read_test(addr + 1, svga);
        addr = gd5440_svga_decode_addr(svga, addr, 0);
        if (addr == 0xffffffff)
            return 0xffff;
    }

    addr &= svga->decode_mask;
    if (svga->translate_address) {
        uint8_t  val1  = 0xff;
        uint8_t  val2  = 0xff;
        uint32_t addr2 = svga->translate_address(addr, priv);
        if (addr2 < svga->vram_max)
            val1 = svga->vram[addr2 & svga->vram_mask];
        addr2 = svga->translate_address(addr + 1, priv);
        if (addr2 < svga->vram_max)
            val2 = svga->vram[addr2 & svga->vram_mask];
        return (val2 << 8) | val1;
    }
    if (addr >= svga->vram_max)
        return 0xffff;

    return gd5440_read_le16(&svga->vram[addr & svga->vram_mask]);
}

uint16_t
gd5440_svga_readw(uint32_t addr, void *priv)
{
    return gd5440_svga_readw_common(addr, 0, priv);
}

uint16_t
gd5440_svga_readw_linear(uint32_t addr, void *priv)
{
    return gd5440_svga_readw_common(addr, 1, priv);
}

uint32_t
gd5440_svga_readl_common(uint32_t addr, uint8_t linear, void *priv)
{
    gd5440_svga_t *svga = (gd5440_svga_t *) priv;

    if (!svga->fast)
        return gd5440_svga_read_common(addr, linear, priv) | (gd5440_svga_read_common(addr + 1, linear, priv) << 8) | (gd5440_svga_read_common(addr + 2, linear, priv) << 16) | (gd5440_svga_read_common(addr + 3, linear, priv) << 24);

    gd5440_cycles -= svga->monitor->mon_video_timing_read_l;

    if (!linear) {
        (void) gd5440_xga_read_test(addr, svga);
        (void) gd5440_xga_read_test(addr + 1, svga);
        (void) gd5440_xga_read_test(addr + 2, svga);
        (void) gd5440_xga_read_test(addr + 3, svga);
        addr = gd5440_svga_decode_addr(svga, addr, 0);
        if (addr == 0xffffffff)
            return 0xffffffff;
    }

    addr &= svga->decode_mask;
    if (svga->translate_address) {
        uint8_t  val1  = 0xff;
        uint8_t  val2  = 0xff;
        uint8_t  val3  = 0xff;
        uint8_t  val4  = 0xff;
        uint32_t addr2 = svga->translate_address(addr, priv);
        if (addr2 < svga->vram_max)
            val1 = svga->vram[addr2 & svga->vram_mask];
        addr2 = svga->translate_address(addr + 1, priv);
        if (addr2 < svga->vram_max)
            val2 = svga->vram[addr2 & svga->vram_mask];
        addr2 = svga->translate_address(addr + 2, priv);
        if (addr2 < svga->vram_max)
            val3 = svga->vram[addr2 & svga->vram_mask];
        addr2 = svga->translate_address(addr + 3, priv);
        if (addr2 < svga->vram_max)
            val4 = svga->vram[addr2 & svga->vram_mask];
        return (val4 << 24) | (val3 << 16) | (val2 << 8) | val1;
    }
    if (addr >= svga->vram_max)
        return 0xffffffff;

    return gd5440_read_le32(&svga->vram[addr & svga->vram_mask]);
}

uint32_t
gd5440_svga_readl(uint32_t addr, void *priv)
{
    return gd5440_svga_readl_common(addr, 0, priv);
}

uint32_t
gd5440_svga_readl_linear(uint32_t addr, void *priv)
{
    return gd5440_svga_readl_common(addr, 1, priv);
}

static uint32_t
tvp3026_conv_16to32(gd5440_svga_t* svga, uint16_t color, uint8_t bpp)
{
    uint32_t ret = 0x00000000;

    if (svga->lut_map) {
        if (bpp == 15) {
            uint8_t b = getcolr(svga->pallook[(color & 0x1f) << 3]);
            uint8_t g = getcolg(svga->pallook[(color & 0x3e0) >> 2]);
            uint8_t r = getcolb(svga->pallook[(color & 0x7c00) >> 7]);
            ret = (gd5440_video_15to32[color] & 0xFF000000) | makecol(r, g, b);
        } else {
            uint8_t b = getcolr(svga->pallook[(color & 0x1f) << 3]);
            uint8_t g = getcolg(svga->pallook[(color & 0x7e0) >> 3]);
            uint8_t r = getcolb(svga->pallook[(color & 0xf800) >> 8]);
            ret = (gd5440_video_16to32[color] & 0xFF000000) | makecol(r, g, b);
        }
    } else
        ret = (bpp == 15) ? gd5440_video_15to32[color] : gd5440_video_16to32[color];

    return ret;
}


typedef struct {
    void (*init)(void);
} gd5440_machine_stub_t;
static gd5440_machine_stub_t machines[1];
static int machine = 0;
static void machine_at_acera1g_init(void) {}
static void machine_at_sb486pv_init(void) {}
static void gd5440_mem_mapping_set_base_ignore(gd5440_mem_mapping_t *mapping, uint32_t base)
{
    (void) mapping;
    (void) base;
}
static int gd5440_rom_init_interleaved(gd5440_rom_t *rom, const char *fn_low, const char *fn_high,
                                uint32_t address, int size, int mask, int file_offset, uint32_t flags)
{
    FILE *low;
    FILE *high;
    int i;
    memset(rom, 0, sizeof(*rom));
    rom->rom = (uint8_t *) calloc(1, (size_t) size);
    if (rom->rom == NULL)
        return -1;
    low = fopen(fn_low, "rb");
    high = fopen(fn_high, "rb");
    if (low == NULL || high == NULL) {
        if (low != NULL) fclose(low);
        if (high != NULL) fclose(high);
        free(rom->rom);
        rom->rom = NULL;
        debug_log(DEBUG_ERROR, "[GD5440] Missing interleaved ROM image: %s / %s\r\n", fn_low, fn_high);
        return -1;
    }
    if (file_offset > 0) {
        fseek(low, file_offset, SEEK_SET);
        fseek(high, file_offset, SEEK_SET);
    }
    for (i = 0; i < (size >> 1); i++) {
        int lo = fgetc(low);
        int hi = fgetc(high);
        if (lo == EOF || hi == EOF)
            break;
        rom->rom[i << 1] = (uint8_t) lo;
        rom->rom[(i << 1) + 1] = (uint8_t) hi;
    }
    fclose(low);
    fclose(high);
    rom->sz = size;
    rom->mask = (uint32_t) mask;
    gd5440_mem_mapping_add(&rom->mapping, address, (uint32_t) size, gd5440_rom_read, NULL, NULL, gd5440_rom_write, NULL, NULL, NULL, MEM_MAPPING_EXTERNAL, rom);
    return 0;
}
static void mca_add(uint8_t (*read_cb)(int, void *),
                    void (*write_cb)(int, uint8_t, void *),
                    uint8_t (*feedb_cb)(void *),
                    void *unused,
                    void *priv)
{
    (void) read_cb;
    (void) write_cb;
    (void) feedb_cb;
    (void) unused;
    (void) priv;
}
#define BIOS_GD5401_PATH                "roms/video/cirruslogic/avga1.rom"
#define BIOS_GD5401_ONBOARD_PATH        "roms/machines/drsm35286/qpaw01-6658237d5e3c2611427518.bin"
#define BIOS_GD5402_PATH                "roms/video/cirruslogic/avga2.rom"
#define BIOS_GD5402_ONBOARD_PATH        "roms/machines/cmdsl386sx25/c000.rom"
#define BIOS_GD5420_PATH                "roms/video/cirruslogic/5420.vbi"
#define BIOS_GD5422_PATH                "roms/video/cirruslogic/cl5422.bin"
#define BIOS_GD5426_DIAMOND_A1_ISA_PATH "roms/video/cirruslogic/diamond5426.vbi"
#define BIOS_GD5426_MCA_PATH            "roms/video/cirruslogic/Reply.BIN"
#define BIOS_GD5428_DIAMOND_B1_VLB_PATH "roms/video/cirruslogic/Diamond SpeedStar PRO VLB v3.04.bin"
#define BIOS_GD5428_ISA_PATH            "roms/video/cirruslogic/5428.bin"
#define BIOS_GD5428_MCA_PATH            "roms/video/cirruslogic/SVGA141.ROM"
#define BIOS_GD5428_ONBOARD_ACER_PATH   "roms/machines/acera1g/4alo001.bin"
#define BIOS_GD5428_PATH                "roms/video/cirruslogic/vlbusjapan.BIN"
#define BIOS_GD5428_BOCA_ISA_PATH_1     "roms/video/cirruslogic/boca_gd5428_1.30b_1.bin"
#define BIOS_GD5428_BOCA_ISA_PATH_2     "roms/video/cirruslogic/boca_gd5428_1.30b_2.bin"
#define BIOS_GD5429_PATH                "roms/video/cirruslogic/5429.vbi"
#define BIOS_GD5430_DIAMOND_A8_VLB_PATH "roms/video/cirruslogic/diamondvlbus.bin"
#define BIOS_GD5430_ORCHID_VLB_PATH     "roms/video/cirruslogic/orchidvlbus.bin"
#define BIOS_GD5434_ORCHID_VLB_PATH     "roms/video/cirruslogic/CL5434_Kelvin.BIN"
#define BIOS_GD5430_PATH                "roms/video/cirruslogic/pci.bin"
#define BIOS_GD5434_DIAMOND_A3_ISA_PATH "roms/video/cirruslogic/Diamond Multimedia SpeedStar 64 v2.02 EPROM Backup from ST M27C256B-12F1.BIN"
#define BIOS_GD5434_PATH                "roms/video/cirruslogic/gd5434.BIN"
#define BIOS_GD5436_PATH                "roms/video/cirruslogic/5436.vbi"
#define BIOS_GD5440_PATH                "roms/video/cirruslogic/BIOS.BIN"
#define BIOS_GD5446_PATH                "roms/video/cirruslogic/5446bv.vbi"
#define BIOS_GD5446_STB_PATH            "roms/video/cirruslogic/stb nitro64v.BIN"
#define BIOS_GD5480_PATH                "roms/video/cirruslogic/clgd5480.rom"

#define CIRRUS_ID_CLGD5401              0x88
#define CIRRUS_ID_CLGD5402              0x89
#define CIRRUS_ID_CLGD5420              0x8a
#define CIRRUS_ID_CLGD5422              0x8c
#define CIRRUS_ID_CLGD5424              0x94
#define CIRRUS_ID_CLGD5426              0x90
#define CIRRUS_ID_CLGD5428              0x98
#define CIRRUS_ID_CLGD5429              0x9c
#define CIRRUS_ID_CLGD5430              0xa0
#define CIRRUS_ID_CLGD5432              0xa2
#define CIRRUS_ID_CLGD5434_4            0xa4
#define CIRRUS_ID_CLGD5434              0xa8
#define CIRRUS_ID_CLGD5436              0xac
#define CIRRUS_ID_CLGD5440              0xa0 /* Yes, the 5440 has the same ID as the 5430. */
#define CIRRUS_ID_CLGD5446              0xb8
#define CIRRUS_ID_CLGD5480              0xbc

/* sequencer 0x07 */
#define CIRRUS_SR7_BPP_VGA           0x00
#define CIRRUS_SR7_BPP_SVGA          0x01
#define CIRRUS_SR7_BPP_MASK          0x0e
#define CIRRUS_SR7_BPP_8             0x00
#define CIRRUS_SR7_BPP_16_DOUBLEVCLK 0x02
#define CIRRUS_SR7_BPP_24            0x04
#define CIRRUS_SR7_BPP_16            0x06
#define CIRRUS_SR7_BPP_32            0x08
#define CIRRUS_SR7_ISAADDR_MASK      0xe0

/* sequencer 0x12 */
#define CIRRUS_CURSOR_SHOW      0x01
#define CIRRUS_CURSOR_HIDDENPEL 0x02
#define CIRRUS_CURSOR_LARGE     0x04 /* 64x64 if set, 32x32 if clear */

/* sequencer 0x17 */
#define CIRRUS_BUSTYPE_VLBFAST   0x10
#define CIRRUS_BUSTYPE_PCI       0x20
#define CIRRUS_BUSTYPE_VLBSLOW   0x30
#define CIRRUS_BUSTYPE_ISA       0x38
#define CIRRUS_MMIO_ENABLE       0x04
#define CIRRUS_MMIO_USE_PCIADDR  0x40 /* 0xb8000 if cleared. */
#define CIRRUS_MEMSIZEEXT_DOUBLE 0x80

/* control 0x0b */
#define CIRRUS_BANKING_DUAL            0x01
#define CIRRUS_BANKING_GRANULARITY_16K 0x20 /* set:16k, clear:4k */

/* control 0x30 */
#define CIRRUS_BLTMODE_BACKWARDS       0x01
#define CIRRUS_BLTMODE_MEMSYSDEST      0x02
#define CIRRUS_BLTMODE_MEMSYSSRC       0x04
#define CIRRUS_BLTMODE_TRANSPARENTCOMP 0x08
#define CIRRUS_BLTMODE_PATTERNCOPY     0x40
#define CIRRUS_BLTMODE_COLOREXPAND     0x80
#define CIRRUS_BLTMODE_PIXELWIDTHMASK  0x30
#define CIRRUS_BLTMODE_PIXELWIDTH8     0x00
#define CIRRUS_BLTMODE_PIXELWIDTH16    0x10
#define CIRRUS_BLTMODE_PIXELWIDTH24    0x20
#define CIRRUS_BLTMODE_PIXELWIDTH32    0x30

/* control 0x31 */
#define CIRRUS_BLT_BUSY      0x01
#define CIRRUS_BLT_START     0x02
#define CIRRUS_BLT_RESET     0x04
#define CIRRUS_BLT_FIFOUSED  0x10
#define CIRRUS_BLT_PAUSED    0x20
#define CIRRUS_BLT_APERTURE2 0x40
#define CIRRUS_BLT_AUTOSTART 0x80

/* control 0x33 */
#define CIRRUS_BLTMODEEXT_BACKGROUNDONLY   0x08
#define CIRRUS_BLTMODEEXT_SOLIDFILL        0x04
#define CIRRUS_BLTMODEEXT_COLOREXPINV      0x02
#define CIRRUS_BLTMODEEXT_DWORDGRANULARITY 0x01

#define CL_GD5428_SYSTEM_BUS_MCA           5
#define CL_GD5428_SYSTEM_BUS_VESA          6
#define CL_GD5428_SYSTEM_BUS_ISA           7

#define CL_GD5429_SYSTEM_BUS_VESA          5
#define CL_GD5429_SYSTEM_BUS_ISA           7

#define CL_GD543X_SYSTEM_BUS_PCI           4
#define CL_GD543X_SYSTEM_BUS_VESA          6
#define CL_GD543X_SYSTEM_BUS_ISA           7

typedef struct gd5440_core_t {
    gd5440_mem_mapping_t mmio_mapping;
    gd5440_mem_mapping_t linear_mapping;
    gd5440_mem_mapping_t aperture2_mapping;
    gd5440_mem_mapping_t vgablt_mapping;

    gd5440_svga_t svga;

    int   has_bios;
    int   rev;
    int   bit32;
    gd5440_rom_t bios_rom;

    uint32_t vram_size;
    uint32_t vram_mask;

    uint8_t vclk_n[4];
    uint8_t vclk_d[4];

    struct {
        uint8_t state;
        int     ctrl;
    } ramdac;

    struct {
        uint16_t width;
        uint16_t height;
        uint16_t dst_pitch;
        uint16_t src_pitch;
        uint16_t trans_col;
        uint16_t trans_mask;
        uint16_t height_internal;
        uint16_t msd_buf_pos;
        uint16_t msd_buf_cnt;

        uint8_t status;
        uint8_t mask;
        uint8_t mode;
        uint8_t rop;
        uint8_t modeext;
        uint8_t ms_is_dest;
        uint8_t msd_buf[32];

        uint32_t fg_col;
        uint32_t bg_col;
        uint32_t dst_addr_backup;
        uint32_t src_addr_backup;
        uint32_t dst_addr;
        uint32_t src_addr;
        uint32_t sys_src32;
        uint32_t sys_cnt;

        /* Internal state */
        int pixel_width;
        int pattern_x;
        int x_count;
        int y_count;
        int xx_count;
        int dir;
        int unlock_special;
    } blt;

    struct {
        int      mode;
        uint16_t stride;
        uint16_t r1sz;
        uint16_t r1adjust;
        uint16_t r2sz;
        uint16_t r2adjust;
        uint16_t r2sdz;
        uint16_t wvs;
        uint16_t wve;
        uint16_t hzoom;
        uint16_t vzoom;
        uint8_t  occlusion;
        uint8_t  colorkeycomparemask;
        uint8_t  colorkeycompare;
        int      region1size;
        int      region2size;
        int      colorkeymode;
        uint32_t ck;
    } overlay;

    int pci;
    int vlb;
    int mca;
    int countminusone;
    int vblank_irq;
    int vportsync;

    uint8_t pci_regs[256];
    uint8_t int_line;
    uint8_t unlocked;
    uint8_t status;
    uint8_t extensions;
    uint8_t crtcreg_mask;
    uint8_t aperture_mask;

    uint8_t fc; /* Feature Connector */

    int id;

    uint8_t pci_slot;
    uint8_t irq_state;

    uint8_t pos_regs[8];

    uint32_t vlb_lfb_base;

    uint32_t lfb_base;
    uint32_t vgablt_base;

    int mmio_vram_overlap;

    uint32_t extpallook[256];
    PALETTE  extpal;

    void *i2c;
    void *ddc;
} gd5440_core_t;

static gd5440_video_timings_t timing_gd54xx_isa = { .type = VIDEO_ISA,
                                             .write_b = 3, .write_w = 3, .write_l = 6,
                                             .read_b = 8, .read_w = 8, .read_l = 12 };
static gd5440_video_timings_t timing_gd54xx_vlb = { .type = VIDEO_BUS,
                                             .write_b = 4, .write_w = 4, .write_l = 8,
                                             .read_b = 10, .read_w = 10, .read_l = 20 };
static gd5440_video_timings_t timing_gd54xx_pci = { .type = VIDEO_PCI, .write_b = 4,
                                             .write_w = 4, .write_l = 8, .read_b = 10,
                                             .read_w = 10, .read_l = 20 };

static void
gd5440_core_543x_mmio_write(uint32_t addr, uint8_t val, void *priv);
static void
gd5440_core_543x_mmio_writeb(uint32_t addr, uint8_t val, void *priv);
static void
gd5440_core_543x_mmio_writew(uint32_t addr, uint16_t val, void *priv);
static void
gd5440_core_543x_mmio_writel(uint32_t addr, uint32_t val, void *priv);
static uint8_t
gd5440_core_543x_mmio_read(uint32_t addr, void *priv);
static uint16_t
gd5440_core_543x_mmio_readw(uint32_t addr, void *priv);
static uint32_t
gd5440_core_543x_mmio_readl(uint32_t addr, void *priv);

static void
gd5440_core_recalc_banking(gd5440_core_t *gd54xx);

static void
gd5440_core_543x_recalc_mapping(gd5440_core_t *gd54xx);

static void
gd5440_core_reset_blit(gd5440_core_t *gd54xx);
static void
gd5440_core_start_blit(uint32_t cpu_dat, uint32_t count, gd5440_core_t *gd54xx, gd5440_svga_t *svga);

#define CLAMP(x)                      \
    do {                              \
        if ((x) & ~0xff)              \
            x = ((x) < 0) ? 0 : 0xff; \
    } while (0)

#define DECODE_YCbCr()                      \
    do {                                    \
        int c;                              \
                                            \
        for (c = 0; c < 2; c++) {           \
            uint8_t y1, y2;                 \
            int8_t  Cr, Cb;                 \
            int     dR, dG, dB;             \
                                            \
            y1 = src[0];                    \
            Cr = src[1] - 0x80;             \
            y2 = src[2];                    \
            Cb = src[3] - 0x80;             \
            src += 4;                       \
                                            \
            dR = (359 * Cr) >> 8;           \
            dG = (88 * Cb + 183 * Cr) >> 8; \
            dB = (453 * Cb) >> 8;           \
                                            \
            r[x_write] = y1 + dR;           \
            CLAMP(r[x_write]);              \
            g[x_write] = y1 - dG;           \
            CLAMP(g[x_write]);              \
            b[x_write] = y1 + dB;           \
            CLAMP(b[x_write]);              \
                                            \
            r[x_write + 1] = y2 + dR;       \
            CLAMP(r[x_write + 1]);          \
            g[x_write + 1] = y2 - dG;       \
            CLAMP(g[x_write + 1]);          \
            b[x_write + 1] = y2 + dB;       \
            CLAMP(b[x_write + 1]);          \
                                            \
            x_write = (x_write + 2) & 7;    \
        }                                   \
    } while (0)

/*Both YUV formats are untested*/
#define DECODE_YUV211()                  \
    do {                                 \
        uint8_t y1, y2, y3, y4;          \
        int8_t  U, V;                    \
        int     dR, dG, dB;              \
                                         \
        U  = src[0] - 0x80;              \
        y1 = (298 * (src[1] - 16)) >> 8; \
        y2 = (298 * (src[2] - 16)) >> 8; \
        V  = src[3] - 0x80;              \
        y3 = (298 * (src[4] - 16)) >> 8; \
        y4 = (298 * (src[5] - 16)) >> 8; \
        src += 6;                        \
                                         \
        dR = (309 * V) >> 8;             \
        dG = (100 * U + 208 * V) >> 8;   \
        dB = (516 * U) >> 8;             \
                                         \
        r[x_write] = y1 + dR;            \
        CLAMP(r[x_write]);               \
        g[x_write] = y1 - dG;            \
        CLAMP(g[x_write]);               \
        b[x_write] = y1 + dB;            \
        CLAMP(b[x_write]);               \
                                         \
        r[x_write + 1] = y2 + dR;        \
        CLAMP(r[x_write + 1]);           \
        g[x_write + 1] = y2 - dG;        \
        CLAMP(g[x_write + 1]);           \
        b[x_write + 1] = y2 + dB;        \
        CLAMP(b[x_write + 1]);           \
                                         \
        r[x_write + 2] = y3 + dR;        \
        CLAMP(r[x_write + 2]);           \
        g[x_write + 2] = y3 - dG;        \
        CLAMP(g[x_write + 2]);           \
        b[x_write + 2] = y3 + dB;        \
        CLAMP(b[x_write + 2]);           \
                                         \
        r[x_write + 3] = y4 + dR;        \
        CLAMP(r[x_write + 3]);           \
        g[x_write + 3] = y4 - dG;        \
        CLAMP(g[x_write + 3]);           \
        b[x_write + 3] = y4 + dB;        \
        CLAMP(b[x_write + 3]);           \
                                         \
        x_write = (x_write + 4) & 7;     \
    } while (0)

#define DECODE_YUV422()                      \
    do {                                     \
        int c;                               \
                                             \
        for (c = 0; c < 2; c++) {            \
            uint8_t y1, y2;                  \
            int8_t  U, V;                    \
            int     dR, dG, dB;              \
                                             \
            U  = src[0] - 0x80;              \
            y1 = (298 * (src[1] - 16)) >> 8; \
            V  = src[2] - 0x80;              \
            y2 = (298 * (src[3] - 16)) >> 8; \
            src += 4;                        \
                                             \
            dR = (309 * V) >> 8;             \
            dG = (100 * U + 208 * V) >> 8;   \
            dB = (516 * U) >> 8;             \
                                             \
            r[x_write] = y1 + dR;            \
            CLAMP(r[x_write]);               \
            g[x_write] = y1 - dG;            \
            CLAMP(g[x_write]);               \
            b[x_write] = y1 + dB;            \
            CLAMP(b[x_write]);               \
                                             \
            r[x_write + 1] = y2 + dR;        \
            CLAMP(r[x_write + 1]);           \
            g[x_write + 1] = y2 - dG;        \
            CLAMP(g[x_write + 1]);           \
            b[x_write + 1] = y2 + dB;        \
            CLAMP(b[x_write + 1]);           \
                                             \
            x_write = (x_write + 2) & 7;     \
        }                                    \
    } while (0)

#define DECODE_RGB555()                                                      \
    do {                                                                     \
        int c;                                                               \
                                                                             \
        for (c = 0; c < 4; c++) {                                            \
            uint16_t dat;                                                    \
                                                                             \
            dat = gd5440_read_le16(src);                                         \
            src += 2;                                                        \
                                                                             \
            r[x_write + c] = ((dat & 0x001f) << 3) | ((dat & 0x001f) >> 2);  \
            g[x_write + c] = ((dat & 0x03e0) >> 2) | ((dat & 0x03e0) >> 7);  \
            b[x_write + c] = ((dat & 0x7c00) >> 7) | ((dat & 0x7c00) >> 12); \
        }                                                                    \
        x_write = (x_write + 4) & 7;                                         \
    } while (0)

#define DECODE_RGB565()                                                      \
    do {                                                                     \
        int c;                                                               \
                                                                             \
        for (c = 0; c < 4; c++) {                                            \
            uint16_t dat;                                                    \
                                                                             \
            dat = gd5440_read_le16(src);                                         \
            src += 2;                                                        \
                                                                             \
            r[x_write + c] = ((dat & 0x001f) << 3) | ((dat & 0x001f) >> 2);  \
            g[x_write + c] = ((dat & 0x07e0) >> 3) | ((dat & 0x07e0) >> 9);  \
            b[x_write + c] = ((dat & 0xf800) >> 8) | ((dat & 0xf800) >> 13); \
        }                                                                    \
        x_write = (x_write + 4) & 7;                                         \
    } while (0)

#define DECODE_CLUT()                                  \
    do {                                               \
        int c;                                         \
                                                       \
        for (c = 0; c < 4; c++) {                      \
            uint8_t dat;                               \
                                                       \
            dat = *(uint8_t *) src;                    \
            src++;                                     \
                                                       \
            r[x_write + c] = svga->pallook[dat] >> 0;  \
            g[x_write + c] = svga->pallook[dat] >> 8;  \
            b[x_write + c] = svga->pallook[dat] >> 16; \
        }                                              \
        x_write = (x_write + 4) & 7;                   \
    } while (0)

#define OVERLAY_SAMPLE()                \
    do {                                \
        switch (gd54xx->overlay.mode) { \
            case 0:                     \
                DECODE_YUV422();        \
                break;                  \
            case 2:                     \
                DECODE_CLUT();          \
                break;                  \
            case 3:                     \
                DECODE_YUV211();        \
                break;                  \
            case 4:                     \
                DECODE_RGB555();        \
                break;                  \
            case 5:                     \
                DECODE_RGB565();        \
                break;                  \
        }                               \
    } while (0)

static int
gd5440_core_interrupt_enabled(gd5440_core_t *gd54xx)
{
    return !gd54xx->pci || (gd54xx->svga.gdcreg[0x17] & 0x04);
}

static int
gd5440_core_vga_vsync_enabled(gd5440_core_t *gd54xx)
{
    if (!(gd54xx->svga.crtc[0x11] & 0x20) && (gd54xx->svga.crtc[0x11] & 0x10) &&
        gd5440_core_interrupt_enabled(gd54xx))
        return 1;
    return 0;
}

static void
gd5440_core_update_irqs(gd5440_core_t *gd54xx)
{
    if (!gd54xx->pci)
        return;

    if ((gd54xx->vblank_irq > 0) && gd5440_core_vga_vsync_enabled(gd54xx))
        pci_set_irq(gd54xx->pci_slot, PCI_INTA, &gd54xx->irq_state);
    else
        pci_clear_irq(gd54xx->pci_slot, PCI_INTA, &gd54xx->irq_state);
}

static void
gd5440_core_vblank_start(gd5440_svga_t *svga)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) svga->priv;
    if (gd54xx->vblank_irq >= 0) {
        gd54xx->vblank_irq = 1;
        gd5440_core_update_irqs(gd54xx);
    }
}

/* Returns 1 if the card is a 5422+ */
static int
gd5440_core_is_5422(gd5440_svga_t *svga)
{
    if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5422)
        return 1;
    else
        return 0;
}

static void
gd5440_core_overlay_draw(gd5440_svga_t *svga, int displine)
{
    const gd5440_core_t *gd54xx = (gd5440_core_t *) svga->priv;
    int             shift  = (svga->crtc[0x27] >= CIRRUS_ID_CLGD5446) ? 2 : 0;
    int             h_acc  = svga->overlay_latch.h_acc;
    int             r[8];
    int             g[8];
    int             b[8];
    int             x_read = 4;
    int             x_write = 4;
    uint32_t       *p;
    uint8_t        *src         = &svga->vram[(svga->overlay_latch.addr << shift) & svga->vram_mask];
    int             bpp         = svga->bpp;
    int             bytesperpix = (bpp + 7) / 8;
    uint8_t        *src2        = &svga->vram[(svga->memaddr - (svga->hdisp * bytesperpix)) & svga->vram_display_mask];
    int             occl;
    int             ckval;

    p = &(svga->monitor->target_buffer->line[displine])[gd54xx->overlay.region1size + svga->x_add];
    src2 += gd54xx->overlay.region1size * bytesperpix;

    OVERLAY_SAMPLE();

    for (int x = 0; (x < gd54xx->overlay.region2size) &&
                    ((x + gd54xx->overlay.region1size) < svga->hdisp); x++) {
        if (gd54xx->overlay.occlusion) {
            occl  = 1;
            ckval = gd54xx->overlay.ck;
            if (bytesperpix == 1) {
                if (*src2 == ckval)
                    occl = 0;
            } else if (bytesperpix == 2) {
                if (*((uint16_t *) src2) == ckval)
                    occl = 0;
            } else
                occl = 0;
            if (!occl)
                *p++ = r[x_read] | (g[x_read] << 8) | (b[x_read] << 16);
            src2 += bytesperpix;
        } else
            *p++ = r[x_read] | (g[x_read] << 8) | (b[x_read] << 16);

        h_acc += gd54xx->overlay.hzoom;
        if (h_acc >= 256) {
            if ((x_read ^ (x_read + 1)) & ~3)
                OVERLAY_SAMPLE();
            x_read = (x_read + 1) & 7;

            h_acc -= 256;
        }
    }

    svga->overlay_latch.v_acc += gd54xx->overlay.vzoom;
    if (svga->overlay_latch.v_acc >= 256) {
        svga->overlay_latch.v_acc -= 256;
        svga->overlay_latch.addr += svga->overlay.pitch << 1;
    }
}

static void
gd5440_core_update_overlay(gd5440_core_t *gd54xx)
{
    gd5440_svga_t *svga = &gd54xx->svga;
    int     bpp  = svga->bpp;

    svga->overlay.cur_ysize     = gd54xx->overlay.wve - gd54xx->overlay.wvs + 1;
    gd54xx->overlay.region1size = 32 * gd54xx->overlay.r1sz / bpp +
                                  (gd54xx->overlay.r1adjust * 8 / bpp);
    gd54xx->overlay.region2size = 32 * gd54xx->overlay.r2sz / bpp +
                                  (gd54xx->overlay.r2adjust * 8 / bpp);

    gd54xx->overlay.occlusion = (svga->crtc[0x3e] & 0x80) != 0 && svga->bpp <= 16;

    /* Mask and chroma key ignored. */
    if (gd54xx->overlay.colorkeymode == 0)
        gd54xx->overlay.ck = gd54xx->overlay.colorkeycompare;
    else if (gd54xx->overlay.colorkeymode == 1)
        gd54xx->overlay.ck = gd54xx->overlay.colorkeycompare |
        (gd54xx->overlay.colorkeycomparemask << 8);
    else
        gd54xx->overlay.occlusion = 0;
}

/* Returns 1 if the card supports the 8-bpp/16-bpp transparency color or mask. */
static int
gd5440_core_has_transp(gd5440_svga_t *svga, int mask)
{
    if (((svga->crtc[0x27] == CIRRUS_ID_CLGD5446) || (svga->crtc[0x27] == CIRRUS_ID_CLGD5480)) &&
        !mask)
        return 1; /* 5446 and 5480 have mask but not transparency. */
    if ((svga->crtc[0x27] == CIRRUS_ID_CLGD5426) || (svga->crtc[0x27] == CIRRUS_ID_CLGD5428))
        return 1; /* 5426 and 5428 have both. */
    else
        return 0; /* The rest have neither. */
}

/* Returns 1 if the card is a 5434, 5436/46, or 5480. */
static int
gd5440_core_is_5434(gd5440_svga_t *svga)
{
    if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5434)
        return 1;
    else
        return 0;
}

static void
gd5440_core_set_svga_fast(gd5440_core_t *gd54xx)
{
    gd5440_svga_t   *svga   = &gd54xx->svga;

    if ((svga->crtc[0x27] == CIRRUS_ID_CLGD5422) || (svga->crtc[0x27] == CIRRUS_ID_CLGD5424))
        svga->fast = ((svga->gdcreg[8] == 0xff) && !(svga->gdcreg[3] & 0x18) &&
                      !svga->gdcreg[1]) &&
                      ((svga->chain4 && svga->packed_chain4) || svga->fb_only) &&
                      !(svga->adv_flags & FLAG_ADDR_BY8);
                      /* TODO: needs verification on other Cirrus chips */
    else
        svga->fast = ((svga->gdcreg[8] == 0xff) && !(svga->gdcreg[3] & 0x18) &&
                     !svga->gdcreg[1]) && ((svga->chain4 && svga->packed_chain4) ||
                     svga->fb_only);
}

static void
gd5440_core_out(uint16_t addr, uint8_t val, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;
    uint8_t   old;
    uint8_t   o;
    uint8_t   index;
    uint32_t  o32;

    if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1))
        addr ^= 0x60;

    switch (addr) {
        case 0x3c0:
        case 0x3c1:
            if (!svga->attrff) {
                svga->attraddr = val & 31;
                if ((val & 0x20) != svga->attr_palette_enable) {
                    svga->fullchange          = 3;
                    svga->attr_palette_enable = val & 0x20;
                    gd5440_svga_recalctimings(svga);
                }
            } else {
                o                                   = svga->attrregs[svga->attraddr & 31];
                svga->attrregs[svga->attraddr & 31] = val;
                if (svga->attraddr < 16)
                    svga->fullchange = changeframecount;
                if (svga->attraddr == 0x10 || svga->attraddr == 0x14 || svga->attraddr < 0x10) {
                    for (uint8_t c = 0; c < 16; c++) {
                        if (svga->attrregs[0x10] & 0x80)
                            svga->egapal[c] = (svga->attrregs[c] & 0xf) |
                                              ((svga->attrregs[0x14] & 0xf) << 4);
                        else
                            svga->egapal[c] = (svga->attrregs[c] & 0x3f) |
                                              ((svga->attrregs[0x14] & 0xc) << 4);
                    }
                }
                /*
                   Recalculate timings on change of attribute register
                   0x11 (overscan border color) too.
                 */
                if (svga->attraddr == 0x10) {
                    if (o != val)
                        gd5440_svga_recalctimings(svga);
                } else if (svga->attraddr == 0x11) {
                    if (!(svga->seqregs[0x12] & 0x80)) {
                        svga->overscan_color = svga->pallook[svga->attrregs[0x11]];
                        if (o != val)
                            gd5440_svga_recalctimings(svga);
                    }
                } else if (svga->attraddr == 0x12) {
                    if ((val & 0xf) != svga->plane_mask)
                        svga->fullchange = changeframecount;
                    svga->plane_mask = val & 0xf;
                }
            }
            svga->attrff ^= 1;
            return;

        case 0x3c4:
            svga->seqaddr = val;
            break;
        case 0x3c5:
            if ((svga->seqaddr == 2) && !gd54xx->unlocked) {
                o = svga->seqregs[svga->seqaddr & 0x1f];
                gd5440_svga_out(addr, val, svga);
                if (svga->gdcreg[0xb] & 0x04)
                    svga->seqregs[svga->seqaddr & 0x1f] = (o & 0xf0) | (val & 0x0f);
                return;
            } else if ((svga->seqaddr > 6) && !gd54xx->unlocked)
                return;

            if (svga->seqaddr > 5) {
                o                                   = svga->seqregs[svga->seqaddr & 0x1f];
                svga->seqregs[svga->seqaddr & 0x1f] = val;
                switch (svga->seqaddr) {
                    case 6:
                        val &= 0x17;
                        if (val == 0x12)
                            svga->seqregs[6] = 0x12;
                        else
                            svga->seqregs[6] = 0x0f;
                        if (svga->crtc[0x27] < CIRRUS_ID_CLGD5429)
                            gd54xx->unlocked = (svga->seqregs[6] == 0x12);
                        else
                            gd54xx->unlocked = 1;
                        break;
                    case 0x08:
                        if (gd54xx->i2c)
                            gd5440_i2c_gpio_set(gd54xx->i2c, !!(val & 0x01), !!(val & 0x02));
                        break;
                    case 0x0b:
                    case 0x0c:
                    case 0x0d:
                    case 0x0e: /* VCLK stuff */
                        gd54xx->vclk_n[svga->seqaddr - 0x0b] = val;
                        break;
                    case 0x1b:
                    case 0x1c:
                    case 0x1d:
                    case 0x1e: /* VCLK stuff */
                        gd54xx->vclk_d[svga->seqaddr - 0x1b] = val;
                        break;
                    case 0x10:
                    case 0x30:
                    case 0x50:
                    case 0x70:
                    case 0x90:
                    case 0xb0:
                    case 0xd0:
                    case 0xf0:
                        svga->hwcursor.x = (val << 3) | (svga->seqaddr >> 5);
                        break;
                    case 0x11:
                    case 0x31:
                    case 0x51:
                    case 0x71:
                    case 0x91:
                    case 0xb1:
                    case 0xd1:
                    case 0xf1:
                        svga->hwcursor.y = (val << 3) | (svga->seqaddr >> 5);
                        break;
                    case 0x12:
                        svga->ext_overscan = !!(val & 0x80);
                        if (svga->ext_overscan && (svga->crtc[0x27] >= CIRRUS_ID_CLGD5426))
                            svga->overscan_color = gd54xx->extpallook[2];
                        else
                            svga->overscan_color = svga->pallook[svga->attrregs[0x11]];
                        gd5440_svga_recalctimings(svga);
                        svga->hwcursor.ena = val & CIRRUS_CURSOR_SHOW;
                        if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5422)
                            svga->hwcursor.cur_xsize = svga->hwcursor.cur_ysize =
                                ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5422) &&
                                 (val & CIRRUS_CURSOR_LARGE)) ? 64 : 32;
                        else
                            svga->hwcursor.cur_xsize = 32;

                        if ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5422) &&
                            (svga->seqregs[0x12] & CIRRUS_CURSOR_LARGE))
                            svga->hwcursor.addr = ((gd54xx->vram_size - 0x4000) +
                                                  ((svga->seqregs[0x13] & 0x3c) * 256));
                        else
                            svga->hwcursor.addr = ((gd54xx->vram_size - 0x4000) +
                                                  ((svga->seqregs[0x13] & 0x3f) * 256));
                        break;
                    case 0x13:
                        if ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5422) &&
                            (svga->seqregs[0x12] & CIRRUS_CURSOR_LARGE))
                            svga->hwcursor.addr = ((gd54xx->vram_size - 0x4000) +
                                                  ((val & 0x3c) * 256));
                        else
                            svga->hwcursor.addr = ((gd54xx->vram_size - 0x4000) +
                                                  ((val & 0x3f) * 256));
                        break;
                    case 0x07:
                        svga->packed_chain4 = svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA;
                        if (gd5440_core_is_5422(svga))
                            gd5440_core_543x_recalc_mapping(gd54xx);
                        else
                            svga->seqregs[svga->seqaddr] &= 0x0f;
                        if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5429)
                            svga->set_reset_disabled = svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA;

                        gd5440_core_set_svga_fast(gd54xx);
                        gd5440_svga_recalctimings(svga);
                        break;
                    case 0x17:
                        if (gd5440_core_is_5422(svga))
                            gd5440_core_543x_recalc_mapping(gd54xx);
                        else
                            return;
                        break;

                    default:
                        break;
                }
                return;
            }
            break;
        case 0x3c6:
            if (!gd54xx->unlocked)
                break;
            if (gd54xx->ramdac.state == 4) {
                gd54xx->ramdac.state = 0;
                gd54xx->ramdac.ctrl  = val;
                gd5440_svga_recalctimings(svga);
                return;
            }
            gd54xx->ramdac.state = 0;
            break;
        case 0x3c7:
        case 0x3c8:
            gd54xx->ramdac.state = 0;
            break;
        case 0x3c9:
            gd54xx->ramdac.state = 0;
            svga->dac_status     = 0;
            svga->fullchange     = changeframecount;
            switch (svga->dac_pos) {
                case 0:
                    svga->dac_r = val;
                    svga->dac_pos++;
                    break;
                case 1:
                    svga->dac_g = val;
                    svga->dac_pos++;
                    break;
                case 2:
                    index = svga->dac_addr & 0xff;
                    if (svga->seqregs[0x12] & 2) {
                        index &= 0x0f;
                        gd54xx->extpal[index].r   = svga->dac_r;
                        gd54xx->extpal[index].g   = svga->dac_g;
                        gd54xx->extpal[index].b   = val;
                        gd54xx->extpallook[index] = makecol32(gd5440_video_6to8[gd54xx->extpal[index].r & 0x3f],
                                                              gd5440_video_6to8[gd54xx->extpal[index].g & 0x3f],
                                                              gd5440_video_6to8[gd54xx->extpal[index].b & 0x3f]);
                        if (svga->ext_overscan && (index == 2)) {
                            o32                  = svga->overscan_color;
                            svga->overscan_color = gd54xx->extpallook[2];
                            if (o32 != svga->overscan_color)
                                gd5440_svga_recalctimings(svga);
                        }
                    } else {
                        svga->vgapal[index].r = svga->dac_r;
                        svga->vgapal[index].g = svga->dac_g;
                        svga->vgapal[index].b = val;
                        svga->pallook[index]  = makecol32(gd5440_video_6to8[svga->vgapal[index].r & 0x3f],
                                                          gd5440_video_6to8[svga->vgapal[index].g & 0x3f],
                                                          gd5440_video_6to8[svga->vgapal[index].b & 0x3f]);
                    }
                    svga->dac_addr = (svga->dac_addr + 1) & 255;
                    svga->dac_pos  = 0;
                    break;

                default:
                    break;
            }
            return;
        case 0x3ce:
            /* Per the CL-GD 5446 manual: bits 0-5 are the GDC register index, bits 6-7 are reserved. */
            svga->gdcaddr = val /* & 0x3f*/;
            return;
        case 0x3cf:
            if (((svga->crtc[0x27] <= CIRRUS_ID_CLGD5422) || (svga->crtc[0x27] == CIRRUS_ID_CLGD5424)) &&
                (svga->gdcaddr > 0x1f))
                return;

            o = svga->gdcreg[svga->gdcaddr];

            if ((svga->gdcaddr < 2) && !gd54xx->unlocked)
                svga->gdcreg[svga->gdcaddr] = (svga->gdcreg[svga->gdcaddr] & 0xf0) | (val & 0x0f);
            else if ((svga->gdcaddr <= 8) || gd54xx->unlocked)
                svga->gdcreg[svga->gdcaddr] = val;

            if (svga->gdcaddr <= 8) {
                switch (svga->gdcaddr) {
                    case 0:
                        gd5440_core_543x_mmio_write(0xb8000, val, gd54xx);
                        break;
                    case 1:
                        gd5440_core_543x_mmio_write(0xb8004, val, gd54xx);
                        break;
                    case 2:
                        svga->colourcompare = val;
                        break;
                    case 4:
                        svga->readplane = val & 3;
                        break;
                    case 5:
                        if (svga->gdcreg[0xb] & 0x04)
                            svga->writemode = val & 7;
                        else
                            svga->writemode = val & 3;
                        svga->readmode    = val & 8;
                        svga->chain2_read = val & 0x10;
                        break;
                    case 6:
                        if ((o ^ val) & 0x0c)
                            gd5440_core_543x_recalc_mapping(gd54xx);
                        break;
                    case 7:
                        svga->colournocare = val;
                        break;

                    default:
                        break;
                }

                gd5440_core_set_svga_fast(gd54xx);

                if (((svga->gdcaddr == 5) && ((val ^ o) & 0x70)) ||
                    ((svga->gdcaddr == 6) && ((val ^ o) & 1)))
                    gd5440_svga_recalctimings(svga);
            } else {
                switch (svga->gdcaddr) {
                    case 0x0b:
                        svga->adv_flags = 0;
                        if (svga->gdcreg[0xb] & 0x01)
                            svga->adv_flags = FLAG_EXTRA_BANKS;
                        if (svga->gdcreg[0xb] & 0x02)
                            svga->adv_flags |= FLAG_ADDR_BY8;
                        if (svga->gdcreg[0xb] & 0x04)
                            svga->adv_flags |= FLAG_EXT_WRITE;
                        if (svga->gdcreg[0xb] & 0x08)
                            svga->adv_flags |= FLAG_LATCH8;
                        if ((svga->gdcreg[0xb] & 0x10) && (svga->adv_flags & FLAG_EXT_WRITE))
                            svga->adv_flags |= FLAG_ADDR_BY16;
                        if (svga->gdcreg[0xb] & 0x04)
                            svga->writemode = svga->gdcreg[5] & 7;
                        else if (o & 0x4) {
                            svga->gdcreg[5] &= ~0x04;
                            svga->writemode = svga->gdcreg[5] & 3;
                            svga->adv_flags &= (FLAG_EXTRA_BANKS | FLAG_ADDR_BY8 | FLAG_LATCH8);
                            if (svga->crtc[0x27] != CIRRUS_ID_CLGD5436) {
                                svga->gdcreg[0] &= 0x0f;
                                gd5440_core_543x_mmio_write(0xb8000, svga->gdcreg[0], gd54xx);
                                svga->gdcreg[1] &= 0x0f;
                                gd5440_core_543x_mmio_write(0xb8004, svga->gdcreg[1], gd54xx);
                            }
                            svga->seqregs[2] &= 0x0f;
                        }
                        fallthrough;
                    case 0x09:
                    case 0x0a:
                        gd5440_core_recalc_banking(gd54xx);
                        break;

                    case 0x0c:
                        gd54xx->overlay.colorkeycompare = val;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x0d:
                        gd54xx->overlay.colorkeycomparemask = val;
                        gd5440_core_update_overlay(gd54xx);
                        break;

                    case 0x0e:
                        if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5429) {
                            svga->dpms = (val & 0x06) && ((svga->miscout & ((val & 0x06) << 5)) != 0xc0);
                            gd5440_svga_recalctimings(svga);
                        }
                        break;

                    case 0x10:
                        gd5440_core_543x_mmio_write(0xb8001, val, gd54xx);
                        break;
                    case 0x11:
                        gd5440_core_543x_mmio_write(0xb8005, val, gd54xx);
                        break;
                    case 0x12:
                        gd5440_core_543x_mmio_write(0xb8002, val, gd54xx);
                        break;
                    case 0x13:
                        gd5440_core_543x_mmio_write(0xb8006, val, gd54xx);
                        break;
                    case 0x14:
                        gd5440_core_543x_mmio_write(0xb8003, val, gd54xx);
                        break;
                    case 0x15:
                        gd5440_core_543x_mmio_write(0xb8007, val, gd54xx);
                        break;

                    case 0x20:
                        gd5440_core_543x_mmio_write(0xb8008, val, gd54xx);
                        break;
                    case 0x21:
                        gd5440_core_543x_mmio_write(0xb8009, val, gd54xx);
                        break;
                    case 0x22:
                        gd5440_core_543x_mmio_write(0xb800a, val, gd54xx);
                        break;
                    case 0x23:
                        gd5440_core_543x_mmio_write(0xb800b, val, gd54xx);
                        break;
                    case 0x24:
                        gd5440_core_543x_mmio_write(0xb800c, val, gd54xx);
                        break;
                    case 0x25:
                        gd5440_core_543x_mmio_write(0xb800d, val, gd54xx);
                        break;
                    case 0x26:
                        gd5440_core_543x_mmio_write(0xb800e, val, gd54xx);
                        break;
                    case 0x27:
                        gd5440_core_543x_mmio_write(0xb800f, val, gd54xx);
                        break;

                    case 0x28:
                        gd5440_core_543x_mmio_write(0xb8010, val, gd54xx);
                        break;
                    case 0x29:
                        gd5440_core_543x_mmio_write(0xb8011, val, gd54xx);
                        break;
                    case 0x2a:
                        gd5440_core_543x_mmio_write(0xb8012, val, gd54xx);
                        break;

                    case 0x2c:
                        gd5440_core_543x_mmio_write(0xb8014, val, gd54xx);
                        break;
                    case 0x2d:
                        gd5440_core_543x_mmio_write(0xb8015, val, gd54xx);
                        break;
                    case 0x2e:
                        gd5440_core_543x_mmio_write(0xb8016, val, gd54xx);
                        break;

                    case 0x2f:
                        gd5440_core_543x_mmio_write(0xb8017, val, gd54xx);
                        break;
                    case 0x30:
                        gd5440_core_543x_mmio_write(0xb8018, val, gd54xx);
                        break;

                    case 0x32:
                        gd5440_core_543x_mmio_write(0xb801a, val, gd54xx);
                        break;

                    case 0x33:
                        gd5440_core_543x_mmio_write(0xb801b, val, gd54xx);
                        break;

                    case 0x31:
                        gd5440_core_543x_mmio_write(0xb8040, val, gd54xx);
                        break;

                    case 0x34:
                        gd5440_core_543x_mmio_write(0xb801c, val, gd54xx);
                        break;

                    case 0x35:
                        gd5440_core_543x_mmio_write(0xb801d, val, gd54xx);
                        break;

                    case 0x38:
                        gd5440_core_543x_mmio_write(0xb8020, val, gd54xx);
                        break;

                    case 0x39:
                        gd5440_core_543x_mmio_write(0xb8021, val, gd54xx);
                        break;

                    default:
                        break;
                }
            }
            return;

        case 0x3d4:
            svga->crtcreg = val & gd54xx->crtcreg_mask;
            return;
        case 0x3d5:
            if (!gd54xx->unlocked &&
                ((svga->crtcreg == 0x19) || (svga->crtcreg == 0x1a) ||
                 (svga->crtcreg == 0x1b) || (svga->crtcreg == 0x1d) ||
                 (svga->crtcreg == 0x25) || (svga->crtcreg == 0x27)))
                return;
            if ((svga->crtcreg == 0x25) || (svga->crtcreg == 0x27))
                return;
            if ((svga->crtcreg < 7) && (svga->crtc[0x11] & 0x80))
                return;
            if ((svga->crtcreg == 7) && (svga->crtc[0x11] & 0x80))
                val = (svga->crtc[7] & ~0x10) | (val & 0x10);
            old                       = svga->crtc[svga->crtcreg];
            svga->crtc[svga->crtcreg] = val;

            if (svga->crtcreg == 0x11) {
                if (!(val & 0x10)) {
                    if (gd54xx->vblank_irq > 0)
                        gd54xx->vblank_irq = -1;
                } else if (gd54xx->vblank_irq < 0)
                    gd54xx->vblank_irq = 0;
                gd5440_core_update_irqs(gd54xx);
                if ((val & ~0x30) == (old & ~0x30))
                    old = val;
            }

            if (old != val) {
                /* Overlay registers */
                switch (svga->crtcreg) {
                    case 0x1d:
                        if (((old >> 3) & 7) != ((val >> 3) & 7)) {
                            gd54xx->overlay.colorkeymode = (val >> 3) & 7;
                            gd5440_core_update_overlay(gd54xx);
                        }
                        break;
                    case 0x31:
                        gd54xx->overlay.hzoom = val == 0 ? 256 : val;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x32:
                        gd54xx->overlay.vzoom = val == 0 ? 256 : val;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x33:
                        gd54xx->overlay.r1sz &= ~0xff;
                        gd54xx->overlay.r1sz |= val;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x34:
                        gd54xx->overlay.r2sz &= ~0xff;
                        gd54xx->overlay.r2sz |= val;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x35:
                        gd54xx->overlay.r2sdz &= ~0xff;
                        gd54xx->overlay.r2sdz |= val;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x36:
                        gd54xx->overlay.r1sz &= 0xff;
                        gd54xx->overlay.r1sz |= (val << 8) & 0x300;
                        gd54xx->overlay.r2sz &= 0xff;
                        gd54xx->overlay.r2sz |= (val << 6) & 0x300;
                        gd54xx->overlay.r2sdz &= 0xff;
                        gd54xx->overlay.r2sdz |= (val << 4) & 0x300;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x37:
                        gd54xx->overlay.wvs &= ~0xff;
                        gd54xx->overlay.wvs |= val;
                        svga->overlay.y = gd54xx->overlay.wvs;
                        break;
                    case 0x38:
                        gd54xx->overlay.wve &= ~0xff;
                        gd54xx->overlay.wve |= val;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x39:
                        gd54xx->overlay.wvs &= 0xff;
                        gd54xx->overlay.wvs |= (val << 8) & 0x300;
                        gd54xx->overlay.wve &= 0xff;
                        gd54xx->overlay.wve |= (val << 6) & 0x300;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x3a:
                        svga->overlay.addr &= ~0xff;
                        svga->overlay.addr |= val;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x3b:
                        svga->overlay.addr &= ~0xff00;
                        svga->overlay.addr |= val << 8;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x3c:
                        svga->overlay.addr &= ~0x0f0000;
                        svga->overlay.addr |= (val << 16) & 0x0f0000;
                        svga->overlay.pitch &= ~0x100;
                        svga->overlay.pitch |= (val & 0x20) << 3;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x3d:
                        svga->overlay.pitch &= ~0xff;
                        svga->overlay.pitch |= val;
                        gd5440_core_update_overlay(gd54xx);
                        break;
                    case 0x3e:
                        gd54xx->overlay.mode = (val >> 1) & 7;
                        svga->overlay.ena    = (val & 1) != 0;
                        gd5440_core_update_overlay(gd54xx);
                        break;

                    default:
                        break;
                }

                if (svga->crtcreg < 0xe || svga->crtcreg > 0x10) {
                    if ((svga->crtcreg == 0xc) || (svga->crtcreg == 0xd)) {
                        svga->fullchange = 3;
                        svga->memaddr_latch   = ((svga->crtc[0xc] << 8) | svga->crtc[0xd]) +
                                           ((svga->crtc[8] & 0x60) >> 5);
                    } else {
                        svga->fullchange = changeframecount;
                        gd5440_svga_recalctimings(svga);
                    }
                }
            }
            break;

        default:
            break;
    }
    gd5440_svga_out(addr, val, svga);
}

static uint8_t
gd5440_core_in(uint16_t addr, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;

    uint8_t index;
    uint8_t ret = 0xff;

    if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1))
        addr ^= 0x60;

    switch (addr) {
        case 0x3c2:
            ret = gd5440_svga_in(addr, svga);
            ret |= gd54xx->vblank_irq > 0 ? 0x80 : 0x00;
            break;

        case 0x3c4:
            if (svga->seqregs[6] == 0x12) {
                ret = svga->seqaddr;
                if ((ret & 0x1e) == 0x10) {
                    if (ret & 1)
                        ret = ((svga->hwcursor.y & 7) << 5) | 0x11;
                    else
                        ret = ((svga->hwcursor.x & 7) << 5) | 0x10;
                }
            } else
                ret = svga->seqaddr;
            break;

        case 0x3c5:
            if ((svga->seqaddr == 2) && !gd54xx->unlocked)
                ret = gd5440_svga_in(addr, svga) & 0x0f;
            else if ((svga->seqaddr > 6) && !gd54xx->unlocked)
                ret = 0xff;
            else if (svga->seqaddr > 5) {
                ret = svga->seqregs[svga->seqaddr & 0x3f];
                switch (svga->seqaddr) {
                    case 6:
                        ret = svga->seqregs[6];
                        break;
                    case 0x08:
                        if (gd54xx->i2c) {
                            ret &= 0x7b;
                            if (gd5440_i2c_gpio_get_scl(gd54xx->i2c))
                                ret |= 0x04;
                            if (gd5440_i2c_gpio_get_sda(gd54xx->i2c))
                                ret |= 0x80;
                        }
                        break;
                    case 0x0a:
                        /* Scratch Pad 1 (Memory size for 5402/542x) */
                        ret = svga->seqregs[0x0a] & ~0x1a;
                        if (svga->crtc[0x27] == CIRRUS_ID_CLGD5402) {
                            if ((gd54xx->vram_size >> 10) == 512)
                                ret |= 0x01; /*512K of memory*/
                            else
                                ret &= 0xfe; /*256K of memory*/
                        } else if (svga->crtc[0x27] > CIRRUS_ID_CLGD5402) {
                            switch (gd54xx->vram_size >> 10) {
                                case 512:
                                    ret |= 0x08;
                                    break;
                                case 1024:
                                    ret |= 0x10;
                                    break;
                                case 2048:
                                    ret |= 0x18;
                                    break;

                                default:
                                    break;
                            }
                        }
                        break;
                    case 0x0b:
                    case 0x0c:
                    case 0x0d:
                    case 0x0e:
                        ret = gd54xx->vclk_n[svga->seqaddr - 0x0b];
                        break;
                    case 0x0f: /* DRAM control */
                        ret = svga->seqregs[0x0f] & ~0x98;
                        switch (gd54xx->vram_size >> 10) {
                            case 512:
                                ret |= 0x08; /* 16-bit DRAM data bus width */
                                break;
                            case 1024:
                                ret |= 0x10; /* 32-bit DRAM data bus width for 1M of memory */
                                break;
                            case 2048:
                                /*
                                   32-bit (Pre-5434)/64-bit (5434 and up) DRAM data bus width
                                   for 2M of memory
                                 */
                                ret |= 0x18;
                                break;
                            case 4096:
                                ret |= 0x98; /*64-bit (5434 and up) DRAM data bus width for 4M of memory*/
                                break;

                            default:
                                break;
                        }
                        break;
                    case 0x15: /*Scratch Pad 3 (Memory size for 543x)*/
                        ret = svga->seqregs[0x15] & ~0x0f;
                        if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5430) {
                            switch (gd54xx->vram_size >> 20) {
                                case 1:
                                    ret |= 0x02;
                                    break;
                                case 2:
                                    ret |= 0x03;
                                    break;
                                case 4:
                                    ret |= 0x04;
                                    break;

                                default:
                                    break;
                            }
                        }
                        break;
                    case 0x17:
                        ret = svga->seqregs[0x17] & ~(7 << 3);
                        if (svga->crtc[0x27] <= CIRRUS_ID_CLGD5429) {
                            if ((svga->crtc[0x27] == CIRRUS_ID_CLGD5428) ||
                                (svga->crtc[0x27] == CIRRUS_ID_CLGD5426)) {
                                if (gd54xx->vlb)
                                    ret |= (CL_GD5428_SYSTEM_BUS_VESA << 3);
                                else if (gd54xx->mca)
                                    ret |= (CL_GD5428_SYSTEM_BUS_MCA << 3);
                                else
                                    ret |= (CL_GD5428_SYSTEM_BUS_ISA << 3);
                            } else {
                                if (gd54xx->vlb)
                                    ret |= (CL_GD5429_SYSTEM_BUS_VESA << 3);
                                else
                                    ret |= (CL_GD5429_SYSTEM_BUS_ISA << 3);
                            }
                        } else {
                            if (gd54xx->pci)
                                ret |= (CL_GD543X_SYSTEM_BUS_PCI << 3);
                            else if (gd54xx->vlb)
                                ret |= (CL_GD543X_SYSTEM_BUS_VESA << 3);
                            else
                                ret |= (CL_GD543X_SYSTEM_BUS_ISA << 3);
                        }
                        break;
                    case 0x18:
                        ret = svga->seqregs[0x18] & 0xfe;
                        break;
                    case 0x1b:
                    case 0x1c:
                    case 0x1d:
                    case 0x1e:
                        ret = gd54xx->vclk_d[svga->seqaddr - 0x1b];
                        break;

                    default:
                        break;
                }
                break;
            } else
                ret = gd5440_svga_in(addr, svga);
            break;
        case 0x3c6:
            if (!gd54xx->unlocked)
                ret = gd5440_svga_in(addr, svga);
            else if (gd54xx->ramdac.state == 4) {
                /* CL-GD 5428 does not lock the register when it's read. */
                if (svga->crtc[0x27] != CIRRUS_ID_CLGD5428)
                    gd54xx->ramdac.state = 0;
                ret = gd54xx->ramdac.ctrl;
            } else {
                gd54xx->ramdac.state++;
                if (gd54xx->ramdac.state == 4)
                    ret = gd54xx->ramdac.ctrl;
                else
                    ret = gd5440_svga_in(addr, svga);
            }
            break;
        case 0x3c7:
        case 0x3c8:
            gd54xx->ramdac.state = 0;
            ret                  = gd5440_svga_in(addr, svga);
            break;
        case 0x3c9:
            gd54xx->ramdac.state = 0;
            svga->dac_status     = 3;
            index                = (svga->dac_addr - 1) & 0xff;
            if (svga->seqregs[0x12] & 2)
                index &= 0x0f;
            switch (svga->dac_pos) {
                case 0:
                    svga->dac_pos++;
                    if (svga->seqregs[0x12] & 2)
                        ret = gd54xx->extpal[index].r & 0x3f;
                    else
                        ret = svga->vgapal[index].r & 0x3f;
                    break;
                case 1:
                    svga->dac_pos++;
                    if (svga->seqregs[0x12] & 2)
                        ret = gd54xx->extpal[index].g & 0x3f;
                    else
                        ret = svga->vgapal[index].g & 0x3f;
                    break;
                case 2:
                    svga->dac_pos  = 0;
                    svga->dac_addr = (svga->dac_addr + 1) & 255;
                    if (svga->seqregs[0x12] & 2)
                        ret = gd54xx->extpal[index].b & 0x3f;
                    else
                        ret = svga->vgapal[index].b & 0x3f;
                    break;

                default:
                    break;
            }
            break;
        case 0x3ce:
            ret = svga->gdcaddr & 0x3f;
            break;
        case 0x3cf:
            if (svga->gdcaddr >= 0x10) {
                if ((svga->gdcaddr > 8) && !gd54xx->unlocked)
                    ret = 0xff;
                else if (((svga->crtc[0x27] <= CIRRUS_ID_CLGD5422) ||
                          (svga->crtc[0x27] == CIRRUS_ID_CLGD5424)) &&
                         (svga->gdcaddr > 0x1f))
                    ret = 0xff;
                else
                    switch (svga->gdcaddr) {
                        case 0x10:
                            ret = gd5440_core_543x_mmio_read(0xb8001, gd54xx);
                            break;
                        case 0x11:
                            ret = gd5440_core_543x_mmio_read(0xb8005, gd54xx);
                            break;
                        case 0x12:
                            ret = gd5440_core_543x_mmio_read(0xb8002, gd54xx);
                            break;
                        case 0x13:
                            ret = gd5440_core_543x_mmio_read(0xb8006, gd54xx);
                            break;
                        case 0x14:
                            ret = gd5440_core_543x_mmio_read(0xb8003, gd54xx);
                            break;
                        case 0x15:
                            ret = gd5440_core_543x_mmio_read(0xb8007, gd54xx);
                            break;

                        case 0x20:
                            ret = gd5440_core_543x_mmio_read(0xb8008, gd54xx);
                            break;
                        case 0x21:
                            ret = gd5440_core_543x_mmio_read(0xb8009, gd54xx);
                            break;
                        case 0x22:
                            ret = gd5440_core_543x_mmio_read(0xb800a, gd54xx);
                            break;
                        case 0x23:
                            ret = gd5440_core_543x_mmio_read(0xb800b, gd54xx);
                            break;
                        case 0x24:
                            ret = gd5440_core_543x_mmio_read(0xb800c, gd54xx);
                            break;
                        case 0x25:
                            ret = gd5440_core_543x_mmio_read(0xb800d, gd54xx);
                            break;
                        case 0x26:
                            ret = gd5440_core_543x_mmio_read(0xb800e, gd54xx);
                            break;
                        case 0x27:
                            ret = gd5440_core_543x_mmio_read(0xb800f, gd54xx);
                            break;

                        case 0x28:
                            ret = gd5440_core_543x_mmio_read(0xb8010, gd54xx);
                            break;
                        case 0x29:
                            ret = gd5440_core_543x_mmio_read(0xb8011, gd54xx);
                            break;
                        case 0x2a:
                            ret = gd5440_core_543x_mmio_read(0xb8012, gd54xx);
                            break;

                        case 0x2c:
                            ret = gd5440_core_543x_mmio_read(0xb8014, gd54xx);
                            break;
                        case 0x2d:
                            ret = gd5440_core_543x_mmio_read(0xb8015, gd54xx);
                            break;
                        case 0x2e:
                            ret = gd5440_core_543x_mmio_read(0xb8016, gd54xx);
                            break;

                        case 0x2f:
                            ret = gd5440_core_543x_mmio_read(0xb8017, gd54xx);
                            break;
                        case 0x30:
                            ret = gd5440_core_543x_mmio_read(0xb8018, gd54xx);
                            break;

                        case 0x32:
                            ret = gd5440_core_543x_mmio_read(0xb801a, gd54xx);
                            break;

                        case 0x33:
                            ret = gd5440_core_543x_mmio_read(0xb801b, gd54xx);
                            break;

                        case 0x31:
                            ret = gd5440_core_543x_mmio_read(0xb8040, gd54xx);
                            break;

                        case 0x34:
                            ret = gd5440_core_543x_mmio_read(0xb801c, gd54xx);
                            break;

                        case 0x35:
                            ret = gd5440_core_543x_mmio_read(0xb801d, gd54xx);
                            break;

                        case 0x38:
                            ret = gd5440_core_543x_mmio_read(0xb8020, gd54xx);
                            break;

                        case 0x39:
                            ret = gd5440_core_543x_mmio_read(0xb8021, gd54xx);
                            break;

                        case 0x3f:
                            if (svga->crtc[0x27] == CIRRUS_ID_CLGD5446)
                                gd54xx->vportsync = !gd54xx->vportsync;
                            ret = gd54xx->vportsync ? 0x80 : 0x00;
                            break;

                        default:
                            break;
                    }
            } else {
                if ((svga->gdcaddr < 2) && !gd54xx->unlocked)
                    ret = (svga->gdcreg[svga->gdcaddr] & 0x0f);
                else {
                    if (svga->gdcaddr == 0)
                        ret = gd5440_core_543x_mmio_read(0xb8000, gd54xx);
                    else if (svga->gdcaddr == 1)
                        ret = gd5440_core_543x_mmio_read(0xb8004, gd54xx);
                    else
                        ret = svga->gdcreg[svga->gdcaddr];
                }
            }
            break;
        case 0x3d4:
            ret = svga->crtcreg;
            break;
        case 0x3d5:
            ret = svga->crtc[svga->crtcreg];
            if (((svga->crtcreg == 0x19) || (svga->crtcreg == 0x1a) ||
                (svga->crtcreg == 0x1b) || (svga->crtcreg == 0x1d) ||
                (svga->crtcreg == 0x25) || (svga->crtcreg == 0x27)) &&
                !gd54xx->unlocked)
                ret = 0xff;
            else
                switch (svga->crtcreg) {
                    case 0x22: /*Graphics Data Latches Readback Register*/
                        /*Should this be & 7 if 8 byte latch is enabled? */
                        ret = svga->latch.b[svga->gdcreg[4] & 3];
                        break;
                    case 0x24: /*Attribute controller toggle readback (R)*/
                        ret = svga->attrff << 7;
                        break;
                    case 0x25: /* Part ID */
                        if (svga->crtc[0x27] == CIRRUS_ID_CLGD5434)
                            ret = 0xb0;
                        break;
                    case 0x26: /*Attribute controller index readback (R)*/
                        ret = svga->attraddr & 0x3f;
                        break;
                    case 0x27:                  /*ID*/
                        ret = svga->crtc[0x27]; /*GD542x/GD543x*/
                        break;
                    case 0x28: /*Class ID*/
                        if ((svga->crtc[0x27] == CIRRUS_ID_CLGD5430) ||
                            (svga->crtc[0x27] == CIRRUS_ID_CLGD5440))
                            ret = 0xff; /*Standard CL-GD5430/40*/
                        break;

                    default:
                        break;
                }
            break;
        default:
            ret = gd5440_svga_in(addr, svga);
            break;
    }

    return ret;
}

static void
gd5440_core_recalc_banking(gd5440_core_t *gd54xx)
{
    gd5440_svga_t *svga = &gd54xx->svga;

    if (!gd5440_core_is_5422(svga)) {
        svga->extra_banks[0] = (svga->gdcreg[0x09] & 0x7f) << 12;

        if (svga->gdcreg[0x0b] & CIRRUS_BANKING_DUAL)
            svga->extra_banks[1] = (svga->gdcreg[0x0a] & 0x7f) << 12;
        else
            svga->extra_banks[1] = svga->extra_banks[0] + 0x8000;
    } else {
        if ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5426) && (svga->crtc[0x27] != CIRRUS_ID_CLGD5424) &&
            (svga->gdcreg[0x0b] & CIRRUS_BANKING_GRANULARITY_16K))
            svga->extra_banks[0] = svga->gdcreg[0x09] << 14;
        else
            svga->extra_banks[0] = svga->gdcreg[0x09] << 12;

        if (svga->gdcreg[0x0b] & CIRRUS_BANKING_DUAL) {
            if ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5426) && (svga->crtc[0x27] != CIRRUS_ID_CLGD5424) &&
                (svga->gdcreg[0x0b] & CIRRUS_BANKING_GRANULARITY_16K))
                svga->extra_banks[1] = svga->gdcreg[0x0a] << 14;
            else
                svga->extra_banks[1] = svga->gdcreg[0x0a] << 12;
        } else
            svga->extra_banks[1] = svga->extra_banks[0] + 0x8000;
    }
}

static void
gd5440_core_543x_recalc_mapping(gd5440_core_t *gd54xx)
{
    gd5440_svga_t  *svga = &gd54xx->svga;
    gd5440_xga_t   *xga  = (gd5440_xga_t *) svga->xga;
    uint32_t base;
    uint32_t size;

    gd54xx->aperture_mask = 0x00;

    if (gd54xx->pci && (!(gd54xx->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM))) {
        gd5440_mem_mapping_disable(&svga->mapping);
        gd5440_mem_mapping_disable(&gd54xx->linear_mapping);
        gd5440_mem_mapping_disable(&gd54xx->mmio_mapping);
        return;
    }

    gd54xx->mmio_vram_overlap = 0;

    if (!gd5440_core_is_5422(svga) || !(svga->seqregs[0x07] & 0xf0) ||
        !(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA)) {
        gd5440_mem_mapping_disable(&gd54xx->linear_mapping);
        gd5440_mem_mapping_disable(&gd54xx->aperture2_mapping);
        switch (svga->gdcreg[6] & 0x0c) {
            case 0x0: /*128k at A0000*/
                gd5440_mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x20000);
                svga->banked_mask = 0xffff;
                break;
            case 0x4: /*64k at A0000*/
                gd5440_mem_mapping_set_addr(&svga->mapping, 0xa0000, 0x10000);
                svga->banked_mask = 0xffff;
                if (gd5440_xga_active && (svga->xga != NULL)) {
                    xga->on = 0;
                    gd5440_mem_mapping_set_handler(&svga->mapping, svga->read, svga->readw, svga->readl, svga->write, svga->writew, svga->writel);
                }
                break;
            case 0x8: /*32k at B0000*/
                gd5440_mem_mapping_set_addr(&svga->mapping, 0xb0000, 0x08000);
                svga->banked_mask = 0x7fff;
                break;
            case 0xC: /*32k at B8000*/
                gd5440_mem_mapping_set_addr(&svga->mapping, 0xb8000, 0x08000);
                svga->banked_mask         = 0x7fff;
                gd54xx->mmio_vram_overlap = 1;
                break;

            default:
                break;
        }

        if ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5429) && (svga->seqregs[0x17] & CIRRUS_MMIO_ENABLE) &&
            (svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA)) {
            if (gd54xx->mmio_vram_overlap) {
                gd5440_mem_mapping_disable(&svga->mapping);
                gd5440_mem_mapping_set_addr(&gd54xx->mmio_mapping, 0xb8000, 0x08000);
            } else
                gd5440_mem_mapping_set_addr(&gd54xx->mmio_mapping, 0xb8000, 0x00100);
        } else
            gd5440_mem_mapping_disable(&gd54xx->mmio_mapping);
    } else {
        if ((svga->crtc[0x27] <= CIRRUS_ID_CLGD5429) ||
            (!gd54xx->pci && !gd54xx->vlb)) {
            if (svga->gdcreg[0x0b] & CIRRUS_BANKING_GRANULARITY_16K) {
                base = (svga->seqregs[0x07] & 0xf0) << 16;
                size = 1 * 1024 * 1024;
            } else {
                base = (svga->seqregs[0x07] & 0xe0) << 16;
                size = 2 * 1024 * 1024;
            }
        } else if (gd54xx->pci) {
            base = gd54xx->lfb_base;
#if 0
            if (svga->crtc[0x27] == CIRRUS_ID_CLGD5480)
                size = 32 * 1024 * 1024;
            else
#endif
            if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5436)
                size = 16 * 1024 * 1024;
            else
                size = 4 * 1024 * 1024;
        } else { /*VLB/ISA/MCA*/
            if (gd54xx->vlb_lfb_base != 0x00000000)
                base = gd54xx->vlb_lfb_base;
            else
                base = 128 * 1024 * 1024;
            if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5436)
                size = 16 * 1024 * 1024;
            else
                size = 4 * 1024 * 1024;
        }

        if (size >= (16 * 1024 * 1024))
            gd54xx->aperture_mask = 0x03;

        gd5440_mem_mapping_disable(&svga->mapping);
        gd5440_mem_mapping_set_addr(&gd54xx->linear_mapping, base, size);
        if ((svga->seqregs[0x17] & CIRRUS_MMIO_ENABLE) &&
            (svga->crtc[0x27] >= CIRRUS_ID_CLGD5429)) {
            if (svga->seqregs[0x17] & CIRRUS_MMIO_USE_PCIADDR)
                /* MMIO is handled in the linear read/write functions */
                gd5440_mem_mapping_disable(&gd54xx->mmio_mapping);
            else
                gd5440_mem_mapping_set_addr(&gd54xx->mmio_mapping, 0xb8000, 0x00100);
        } else
            gd5440_mem_mapping_disable(&gd54xx->mmio_mapping);

        if ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5436) &&
            (gd54xx->blt.status & CIRRUS_BLT_APERTURE2) &&
            ((gd54xx->blt.mode & (CIRRUS_BLTMODE_COLOREXPAND | CIRRUS_BLTMODE_MEMSYSSRC)) ==
             (CIRRUS_BLTMODE_COLOREXPAND | CIRRUS_BLTMODE_MEMSYSSRC))) {
            if (svga->crtc[0x27] == CIRRUS_ID_CLGD5480)
                gd5440_mem_mapping_set_addr(&gd54xx->aperture2_mapping,
                                     gd54xx->lfb_base + 16777216, 16777216);
            else
                gd5440_mem_mapping_set_addr(&gd54xx->aperture2_mapping, 0xbc000, 0x04000);
        } else
            gd5440_mem_mapping_disable(&gd54xx->aperture2_mapping);
    }
}

static void
gd5440_core_recalctimings(gd5440_svga_t *svga)
{
    const gd5440_core_t *gd54xx = (gd5440_core_t *) svga->priv;
    uint8_t         clocksel;
    uint8_t         rdmask;
    uint8_t         linedbl = svga->dispend * 9 / 10 >= svga->hdisp;
    uint8_t         m = 0;
    int             d = 0;
    int             n = 0;

    svga->hblankstart = svga->crtc[2];

    if (svga->crtc[0x1b] & ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5424) ? 0xa0 : 0x20)) {
        /*
           Special blanking mode: the blank start and end become components
           of the window generator, and the actual blanking comes from the
           display enable signal.

           This means blanking during overscan, we already calculate it that
           way, so just use the same calculation and force otvercan to 0.
         */
        svga->hblank_end_val = (svga->crtc[3] & 0x1f) | ((svga->crtc[5] & 0x80) ? 0x20 : 0x00) |
                               (((svga->crtc[0x1a] >> 4) & 3) << 6);

        svga->hblank_end_mask = 0x000000ff;

        if (svga->crtc[0x1b] & 0x20) {
            svga->hblankstart = svga->crtc[1]/* + ((svga->crtc[3] >> 5) & 3) + 1*/;
            svga->hblank_end_val = svga->htotal - 1 /* + ((svga->crtc[3] >> 5) & 3)*/;

            /* In this mode, the dots per clock are always 8 or 16, never 9 or 18. */
	    if (!svga->scrblank && svga->attr_palette_enable)
                svga->dots_per_clock = (svga->seqregs[1] & 8) ? 16 : 8;

            svga->monitor->mon_overscan_y = 0;
            svga->monitor->mon_overscan_x = 0;

            /* Also make sure vertical blanking starts on display end. */
            svga->vblankstart = svga->dispend;
        }
    }

    svga->rowoffset = (svga->crtc[0x13]) | (((int) (uint32_t) (svga->crtc[0x1b] & 0x10)) << 4);

    svga->interlace = (svga->crtc[0x1a] & 0x01);

    if (!svga->scrblank && svga->attr_palette_enable) {
        if (!(svga->gdcreg[6] & 1) && !(svga->attrregs[0x10] & 1)) /*Text mode*/
            svga->interlace = 0;
    }

    clocksel = (svga->miscout >> 2) & 3;

    if (!gd54xx->vclk_n[clocksel] || !gd54xx->vclk_d[clocksel])
        svga->clock = (gd5440_cpuclock * (float) (1ULL << 32)) /
                      ((svga->miscout & 0xc) ? 28322000.0 : 25175000.0);
    else {
        n    = gd54xx->vclk_n[clocksel] & 0x7f;
        d    = (gd54xx->vclk_d[clocksel] & 0x3e) >> 1;
        m    = gd54xx->vclk_d[clocksel] & 0x01 ? 2 : 1;
        float   freq = (14318184.0F * ((float) n / ((float) d * m)));
        if (gd5440_core_is_5422(svga)) {
            switch (svga->seqregs[0x07] & (gd5440_core_is_5434(svga) ? 0xe : 6)) {
                case 2:
                    freq /= 2.0F;
                    break;
                case 4:
                    if (!gd5440_core_is_5434(svga))
                        freq /= 3.0F;
                    break;

                default:
                    break;
            }
        }
        svga->clock = (gd5440_cpuclock * (double) (1ULL << 32)) / freq;
    }

    svga->bpp = 8;
    svga->map8 = svga->pallook;
    if (svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA) {
        if (linedbl)
            svga->render = gd5440_svga_render_8bpp_lowres;
        else {
            svga->render = gd5440_svga_render_8bpp_highres;
            if ((svga->dispend == 512) && !svga->interlace && gd5440_core_is_5434(svga)) {
                svga->hdisp <<= 1;
                svga->dots_per_clock <<= 1;
                svga->clock *= 2.0;
            }
        }
    } else if (svga->gdcreg[5] & 0x40)
        svga->render = gd5440_svga_render_8bpp_lowres;

    svga->memaddr_latch |= ((svga->crtc[0x1b] & 0x01) << 16) | ((svga->crtc[0x1b] & 0xc) << 15);

    if (gd54xx->ramdac.ctrl & 0x80) {
        if (gd54xx->ramdac.ctrl & 0x40) {
            if ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5428) || (svga->crtc[0x27] == CIRRUS_ID_CLGD5426))
                rdmask = 0xf;
            else
                rdmask = 0x7;

            switch (gd54xx->ramdac.ctrl & rdmask) {
                case 0:
                    svga->bpp = 15;
                    if (linedbl) {
                        if (gd54xx->ramdac.ctrl & 0x10)
                            svga->render = gd5440_svga_render_15bpp_mix_lowres;
                        else
                            svga->render = gd5440_svga_render_15bpp_lowres;
                    } else {
                        if (gd54xx->ramdac.ctrl & 0x10)
                            svga->render = gd5440_svga_render_15bpp_mix_highres;
                        else
                            svga->render = gd5440_svga_render_15bpp_highres;
                    }
                    break;

                case 1:
                    svga->bpp = 16;
                    if (linedbl)
                        svga->render = gd5440_svga_render_16bpp_lowres;
                    else
                        svga->render = gd5440_svga_render_16bpp_highres;
                    break;

                case 5:
                    if (gd5440_core_is_5434(svga) && (svga->seqregs[0x07] & CIRRUS_SR7_BPP_32)) {
                        svga->bpp = 32;
                        if (linedbl)
                            svga->render = gd5440_svga_render_32bpp_lowres;
                        else
                            svga->render = gd5440_svga_render_32bpp_highres;
                        if (svga->crtc[0x27] < CIRRUS_ID_CLGD5436) {
                            svga->rowoffset *= 2;
                        }
                    } else {
                        svga->bpp = 24;
                        if (linedbl)
                            svga->render = gd5440_svga_render_24bpp_lowres;
                        else
                            svga->render = gd5440_svga_render_24bpp_highres;
                    }
                    break;

                case 8:
                    svga->bpp  = 8;
                    svga->map8 = gd5440_video_8togs;
                    if (linedbl)
                        svga->render = gd5440_svga_render_8bpp_lowres;
                    else
                        svga->render = gd5440_svga_render_8bpp_highres;
                    break;

                case 9:
                    svga->bpp  = 8;
                    svga->map8 = gd5440_video_8to32;
                    if (linedbl)
                        svga->render = gd5440_svga_render_8bpp_lowres;
                    else
                        svga->render = gd5440_svga_render_8bpp_highres;
                    break;

                case 0xf:
                    switch (svga->seqregs[0x07] & CIRRUS_SR7_BPP_MASK) {
                        case CIRRUS_SR7_BPP_32:
                            if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5430) {
                                svga->bpp = 32;
                                if (linedbl)
                                    svga->render = gd5440_svga_render_32bpp_lowres;
                                else
                                    svga->render = gd5440_svga_render_32bpp_highres;
                                svga->rowoffset *= 2;
                            }
                            break;

                        case CIRRUS_SR7_BPP_24:
                            svga->bpp = 24;
                            if (linedbl)
                                svga->render = gd5440_svga_render_24bpp_lowres;
                            else
                                svga->render = gd5440_svga_render_24bpp_highres;
                            break;

                        case CIRRUS_SR7_BPP_16:
                            if ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5428) ||
                                (svga->crtc[0x27] == CIRRUS_ID_CLGD5426)) {
                                svga->bpp = 16;
                                if (linedbl)
                                    svga->render = gd5440_svga_render_16bpp_lowres;
                                else
                                    svga->render = gd5440_svga_render_16bpp_highres;
                            }
                            break;

                        case CIRRUS_SR7_BPP_16_DOUBLEVCLK:
                            svga->bpp = 16;
                            if (linedbl)
                                svga->render = gd5440_svga_render_16bpp_lowres;
                            else
                                svga->render = gd5440_svga_render_16bpp_highres;
                            break;

                        case CIRRUS_SR7_BPP_8:
                            svga->bpp = 8;
                            if (linedbl)
                                svga->render = gd5440_svga_render_8bpp_lowres;
                            else
                                svga->render = gd5440_svga_render_8bpp_highres;
                            break;

                        default:
                            break;
                    }
                    break;

                default:
                    break;
            }
        } else {
            svga->bpp = 15;
            if (linedbl) {
                if (gd54xx->ramdac.ctrl & 0x10)
                    svga->render = gd5440_svga_render_15bpp_mix_lowres;
                else
                    svga->render = gd5440_svga_render_15bpp_lowres;
            } else {
                if (gd54xx->ramdac.ctrl & 0x10)
                    svga->render = gd5440_svga_render_15bpp_mix_highres;
                else
                    svga->render = gd5440_svga_render_15bpp_highres;
            }
        }
    }

    svga->vram_display_mask = (svga->crtc[0x1b] & 0x02) ? gd54xx->vram_mask : 0x3ffff;

    if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5430)
        svga->htotal += ((svga->crtc[0x1c] >> 3) & 0x07);

    if (!(svga->gdcreg[6] & 0x01) && !(svga->attrregs[0x10] & 0x01)) { /*Text mode*/
        if (svga->seqregs[1] & 8)
            svga->render = gd5440_svga_render_text_40;
        else
            svga->render = gd5440_svga_render_text_80;
    }

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA) && (((svga->gdcreg[6] >> 2) & 0x03) != 0x01)) {
        svga->extra_banks[0] = 0;
        svga->extra_banks[1] = 0x8000;
    }
}

static void
gd5440_core_hwcursor_draw(gd5440_svga_t *svga, int displine)
{
    const gd5440_core_t *gd54xx = (gd5440_core_t *) svga->priv;
    int             comb;
    int             b0;
    int             b1;
    uint8_t         dat[2];
    int             offset  = svga->hwcursor_latch.x - svga->hwcursor_latch.xoff;
    int             pitch   = (svga->hwcursor.cur_xsize == 64) ? 16 : 4;
    uint32_t        bgcol   = gd54xx->extpallook[0x00];
    uint32_t        fgcol   = gd54xx->extpallook[0x0f];
    uint8_t         linedbl = svga->dispend * 9 / 10 >= svga->hdisp;

    offset <<= linedbl;

    if (svga->interlace && svga->hwcursor_oddeven)
        svga->hwcursor_latch.addr += pitch;

    for (int x = 0; x < svga->hwcursor.cur_xsize; x += 8) {
        dat[0] = svga->vram[svga->hwcursor_latch.addr & gd54xx->vram_mask];
        if (svga->hwcursor.cur_xsize == 64)
            dat[1] = svga->vram[(svga->hwcursor_latch.addr + 0x08) & gd54xx->vram_mask];
        else
            dat[1] = svga->vram[(svga->hwcursor_latch.addr + 0x80) & gd54xx->vram_mask];
        for (uint8_t xx = 0; xx < 8; xx++) {
            b0   = (dat[0] >> (7 - xx)) & 1;
            b1   = (dat[1] >> (7 - xx)) & 1;
            comb = (b1 | (b0 << 1));
            if (offset >= svga->hwcursor_latch.x) {
                switch (comb) {
                    case 0:
                        /* The original screen pixel is shown (invisible cursor) */
                        break;
                    case 1:
                        /* The pixel is shown in the cursor background color */
                        (svga->monitor->target_buffer->line[displine])[offset + svga->x_add] = bgcol;
                        break;
                    case 2:
                        /* The pixel is shown as the inverse of the original screen pixel
                           (XOR cursor) */
                        (svga->monitor->target_buffer->line[displine])[offset + svga->x_add] ^= 0xffffff;
                        break;
                    case 3:
                        /* The pixel is shown in the cursor foreground color */
                        (svga->monitor->target_buffer->line[displine])[offset + svga->x_add] = fgcol;
                        break;

                    default:
                        break;
                }
            }

            offset++;
        }
        svga->hwcursor_latch.addr++;
    }

    if (svga->hwcursor.cur_xsize == 64)
        svga->hwcursor_latch.addr += 8;

    if (svga->interlace && !svga->hwcursor_oddeven)
        svga->hwcursor_latch.addr += pitch;
}

static void
gd5440_core_rop(gd5440_core_t *gd54xx, uint8_t *res, uint8_t *dst, const uint8_t *src)
{
    switch (gd54xx->blt.rop) {
        case 0x00:
            *res = 0x00;
            break;
        case 0x05:
            *res = *src & *dst;
            break;
        case 0x06:
            *res = *dst;
            break;
        case 0x09:
            *res = *src & ~*dst;
            break;
        case 0x0b:
            *res = ~*dst;
            break;
        case 0x0d:
            *res = *src;
            break;
        case 0x0e:
            *res = 0xff;
            break;
        case 0x50:
            *res = ~*src & *dst;
            break;
        case 0x59:
            *res = *src ^ *dst;
            break;
        case 0x6d:
            *res = *src | *dst;
            break;
        case 0x90:
            *res = ~(*src | *dst);
            break;
        case 0x95:
            *res = ~(*src ^ *dst);
            break;
        case 0xad:
            *res = *src | ~*dst;
            break;
        case 0xd0:
            *res = ~*src;
            break;
        case 0xd6:
            *res = ~*src | *dst;
            break;
        case 0xda:
            *res = ~(*src & *dst);
            break;

        default:
            break;
    }
}

static uint8_t
gd5440_core_get_aperture(gd5440_core_t *gd54xx, uint32_t addr)
{
    uint32_t ap = addr >> 22;
    return (uint8_t) (ap & gd54xx->aperture_mask);
}

static uint32_t
gd5440_core_mem_sys_pos_adj(gd5440_core_t *gd54xx, uint8_t ap, uint32_t pos)
{
    uint32_t ret = pos;

    if ((gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) &&
        !(gd54xx->blt.modeext & CIRRUS_BLTMODEEXT_DWORDGRANULARITY)) {
        switch (ap) {
            case 1:
                ret ^= 1;
                break;
            case 2:
                ret ^= 3;
                break;
        }
    }

    return ret;
}

static uint8_t
gd5440_core_mem_sys_dest_read(gd5440_core_t *gd54xx, uint8_t ap)
{
    uint32_t adj_pos = gd5440_core_mem_sys_pos_adj(gd54xx, ap, gd54xx->blt.msd_buf_pos);
    uint8_t ret = 0xff;

    if (gd54xx->blt.msd_buf_cnt != 0) {
        ret = gd54xx->blt.msd_buf[adj_pos];

        gd54xx->blt.msd_buf_pos++;
        gd54xx->blt.msd_buf_cnt--;

        if (gd54xx->blt.msd_buf_cnt == 0) {
            if (gd54xx->countminusone == 1) {
                gd54xx->blt.msd_buf_pos = 0;
                if ((gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) &&
                    !(gd54xx->blt.modeext & CIRRUS_BLTMODEEXT_DWORDGRANULARITY))
                    gd5440_core_start_blit(0xff, 8, gd54xx, &gd54xx->svga);
                else
                    gd5440_core_start_blit(0xffffffff, 32, gd54xx, &gd54xx->svga);
            } else
                gd5440_core_reset_blit(gd54xx); /* End of blit, do no more. */
        }
    }

    return ret;
}

static void
gd5440_core_mem_sys_src_write(gd5440_core_t *gd54xx, uint8_t val, uint8_t ap)
{
    uint32_t adj_pos = gd5440_core_mem_sys_pos_adj(gd54xx, ap, gd54xx->blt.sys_cnt);

    gd54xx->blt.sys_src32 &= ~(0xff << (adj_pos << 3));
    gd54xx->blt.sys_src32 |= (val << (adj_pos << 3));
    gd54xx->blt.sys_cnt = (gd54xx->blt.sys_cnt + 1) & 3;

    if (gd54xx->blt.sys_cnt == 0) {
        if ((gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) &&
            !(gd54xx->blt.modeext & CIRRUS_BLTMODEEXT_DWORDGRANULARITY)) {
            for (uint8_t i = 0; i < 32; i += 8)
                gd5440_core_start_blit((gd54xx->blt.sys_src32 >> i) & 0xff, 8, gd54xx, &gd54xx->svga);
        } else
            gd5440_core_start_blit(gd54xx->blt.sys_src32, 32, gd54xx, &gd54xx->svga);
    }
}

static void
gd5440_core_write(uint32_t addr, uint8_t val, void *priv)
{
    gd5440_svga_t   *svga   = (gd5440_svga_t *) priv;
    gd5440_core_t *gd54xx = (gd5440_core_t *) svga->local;

    if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
        !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        gd5440_core_mem_sys_src_write(gd54xx, val, 0);
        return;
    }

    gd5440_xga_write_test(addr, val, svga);

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA) && (((svga->gdcreg[6] >> 2) & 0x03) != 0x01)) {
        gd5440_svga_write(addr, val, svga);
        return;
    }

    addr &= svga->banked_mask;
    addr = (addr & 0x7fff) + svga->extra_banks[(addr >> 15) & 1];

    gd5440_svga_write_linear(addr, val, svga);
}

static void
gd5440_core_writew(uint32_t addr, uint16_t val, void *priv)
{
    gd5440_svga_t   *svga   = (gd5440_svga_t *) priv;
    gd5440_core_t *gd54xx = (gd5440_core_t *) svga->local;

    if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
        !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        if ((gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) && (gd54xx->blt.modeext & CIRRUS_BLTMODEEXT_DWORDGRANULARITY))
            val = (val >> 8) | (val << 8);

        gd5440_core_write(addr, val, svga);
        gd5440_core_write(addr + 1, val >> 8, svga);
        return;
    }

    gd5440_xga_write_test(addr, val, svga);
    gd5440_xga_write_test(addr + 1, val >> 8, svga);

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA) && (((svga->gdcreg[6] >> 2) & 0x03) != 0x01)) {
        gd5440_svga_writew(addr, val, svga);
        return;
    }

    addr &= svga->banked_mask;
    addr = (addr & 0x7fff) + svga->extra_banks[(addr >> 15) & 1];

    if (svga->writemode < 4)
        gd5440_svga_writew_linear(addr, val, svga);
    else {
        gd5440_svga_write_linear(addr, val, svga);
        gd5440_svga_write_linear(addr + 1, val >> 8, svga);
    }
}

static void
gd5440_core_writel(uint32_t addr, uint32_t val, void *priv)
{
    gd5440_svga_t   *svga   = (gd5440_svga_t *) priv;
    gd5440_core_t *gd54xx = (gd5440_core_t *) svga->local;

    if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
        !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        if ((gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) && (gd54xx->blt.modeext & CIRRUS_BLTMODEEXT_DWORDGRANULARITY))
            val = ((val & 0xff000000) >> 24) | ((val & 0x00ff0000) >> 8) | ((val & 0x0000ff00) << 8) | ((val & 0x000000ff) << 24);

        gd5440_core_write(addr, val, svga);
        gd5440_core_write(addr + 1, val >> 8, svga);
        gd5440_core_write(addr + 2, val >> 16, svga);
        gd5440_core_write(addr + 3, val >> 24, svga);
        return;
    }

    gd5440_xga_write_test(addr, val, svga);
    gd5440_xga_write_test(addr + 1, val >> 8, svga);
    gd5440_xga_write_test(addr + 2, val >> 16, svga);
    gd5440_xga_write_test(addr + 3, val >> 24, svga);

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA) && (((svga->gdcreg[6] >> 2) & 0x03) != 0x01)) {
        gd5440_svga_writel(addr, val, svga);
        return;
    }

    addr &= svga->banked_mask;
    addr = (addr & 0x7fff) + svga->extra_banks[(addr >> 15) & 1];

    if (svga->writemode < 4)
        gd5440_svga_writel_linear(addr, val, svga);
    else {
        gd5440_svga_write_linear(addr, val, svga);
        gd5440_svga_write_linear(addr + 1, val >> 8, svga);
        gd5440_svga_write_linear(addr + 2, val >> 16, svga);
        gd5440_svga_write_linear(addr + 3, val >> 24, svga);
    }
}

/* This adds write modes 4 and 5 to SVGA. */
static void
gd5440_core_write_modes45(gd5440_svga_t *svga, uint8_t val, uint32_t addr)
{
    uint32_t i;
    uint32_t j;

    switch (svga->writemode) {
        case 4:
            if (svga->adv_flags & FLAG_ADDR_BY16) {
                addr &= svga->decode_mask;

                for (i = 0; i < 8; i++) {
                    if (val & svga->seqregs[2] & (0x80 >> i)) {
                        svga->vram[addr + (i << 1)]     = svga->gdcreg[1];
                        svga->vram[addr + (i << 1) + 1] = svga->gdcreg[0x11];
                    }
                }
            } else {
                addr <<= 1;
                addr &= svga->decode_mask;

                for (i = 0; i < 8; i++) {
                    if (val & svga->seqregs[2] & (0x80 >> i))
                        svga->vram[addr + i] = svga->gdcreg[1];
                }
            }
            break;

        case 5:
            if (svga->adv_flags & FLAG_ADDR_BY16) {
                addr &= svga->decode_mask;

                for (i = 0; i < 8; i++) {
                    j = (0x80 >> i);
                    if (svga->seqregs[2] & j) {
                        svga->vram[addr + (i << 1)]     = (val & j) ?
                                                              svga->gdcreg[1] : svga->gdcreg[0];
                        svga->vram[addr + (i << 1) + 1] = (val & j) ?
                                                              svga->gdcreg[0x11] : svga->gdcreg[0x10];
                    }
                }
            } else {
                addr <<= 1;
                addr &= svga->decode_mask;

                for (i = 0; i < 8; i++) {
                    j = (0x80 >> i);
                    if (svga->seqregs[2] & j)
                        svga->vram[addr + i] = (val & j) ? svga->gdcreg[1] : svga->gdcreg[0];
                }
            }
            break;

        default:
            break;
    }

    svga->changedvram[addr >> 12] = changeframecount;
}

static int
gd5440_core_aperture2_enabled(gd5440_core_t *gd54xx)
{
    const gd5440_svga_t *svga = &gd54xx->svga;

    if (svga->crtc[0x27] < CIRRUS_ID_CLGD5436)
        return 0;

    if (!(gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND))
        return 0;

    if (!(gd54xx->blt.status & CIRRUS_BLT_APERTURE2))
        return 0;

    return 1;
}

static uint8_t
gd5440_core_readb_linear(uint32_t addr, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;

    uint8_t ap = gd5440_core_get_aperture(gd54xx, addr);
    addr &= 0x003fffff; /* 4 MB mask */

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA))
        return gd5440_svga_read_linear(addr, svga);

    if ((addr >= (svga->vram_max - 256)) && (addr < svga->vram_max)) {
        if ((svga->seqregs[0x17] & CIRRUS_MMIO_ENABLE) &&
            (svga->seqregs[0x17] & CIRRUS_MMIO_USE_PCIADDR))
            return gd5440_core_543x_mmio_read(addr & 0x000000ff, gd54xx);
    }

    /*
       Do mem sys dest reads here if the blitter is neither paused,
       nor is there a second aperture.
     */
    if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
        !gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED))
        return gd5440_core_mem_sys_dest_read(gd54xx, ap);

    switch (ap) {
        default:
        case 0:
            break;
        case 1:
            /* 0 -> 1, 1 -> 0, 2 -> 3, 3 -> 2 */
            addr ^= 0x00000001;
            break;
        case 2:
            /* 0 -> 3, 1 -> 2, 2 -> 1, 3 -> 0 */
            addr ^= 0x00000003;
            break;
        case 3:
            return 0xff;
    }

    return gd5440_svga_read_linear(addr, svga);
}

static uint16_t
gd5440_core_readw_linear(uint32_t addr, void *priv)
{
    gd5440_core_t *gd54xx   = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga     = &gd54xx->svga;
    uint32_t  old_addr = addr;

    uint8_t  ap = gd5440_core_get_aperture(gd54xx, addr);
    uint16_t temp;

    addr &= 0x003fffff; /* 4 MB mask */

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA))
        return gd5440_svga_readw_linear(addr, svga);

    if ((addr >= (svga->vram_max - 256)) && (addr < svga->vram_max)) {
        if ((svga->seqregs[0x17] & CIRRUS_MMIO_ENABLE) && (svga->seqregs[0x17] & CIRRUS_MMIO_USE_PCIADDR)) {
            temp = gd5440_core_543x_mmio_readw(addr & 0x000000ff, gd54xx);
            return temp;
        }
    }

    /*
       Do mem sys dest reads here if the blitter is neither paused,
       nor is there a second aperture.
     */
    if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
        !gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        temp = gd5440_core_readb_linear(old_addr, priv);
        temp |= gd5440_core_readb_linear(old_addr + 1, priv) << 8;
        return temp;
    }

    switch (ap) {
        default:
        case 0:
            return gd5440_svga_readw_linear(addr, svga);
        case 2:
            /* 0 -> 3, 1 -> 2, 2 -> 1, 3 -> 0 */
            addr ^= 0x00000002;
            fallthrough;
        case 1:
            temp = gd5440_svga_readb_linear(addr + 1, svga);
            temp |= (gd5440_svga_readb_linear(addr, svga) << 8);

            if (svga->fast)
                gd5440_cycles -= svga->monitor->mon_video_timing_read_w;

            return temp;
        case 3:
            return 0xffff;
    }
}

static uint32_t
gd5440_core_readl_linear(uint32_t addr, void *priv)
{
    gd5440_core_t *gd54xx   = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga     = &gd54xx->svga;
    uint32_t  old_addr = addr;

    uint8_t  ap = gd5440_core_get_aperture(gd54xx, addr);
    uint32_t temp;

    addr &= 0x003fffff; /* 4 MB mask */

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA))
        return gd5440_svga_readl_linear(addr, svga);

    if ((addr >= (svga->vram_max - 256)) && (addr < svga->vram_max)) {
        if ((svga->seqregs[0x17] & CIRRUS_MMIO_ENABLE) && (svga->seqregs[0x17] & CIRRUS_MMIO_USE_PCIADDR)) {
            temp = gd5440_core_543x_mmio_readl(addr & 0x000000ff, gd54xx);
            return temp;
        }
    }

    /*
       Do mem sys dest reads here if the blitter is neither paused,
       nor is there a second aperture.
     */
    if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
        !gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        temp = gd5440_core_readb_linear(old_addr, priv);
        temp |= gd5440_core_readb_linear(old_addr + 1, priv) << 8;
        temp |= gd5440_core_readb_linear(old_addr + 2, priv) << 16;
        temp |= gd5440_core_readb_linear(old_addr + 3, priv) << 24;
        return temp;
    }

    switch (ap) {
        default:
        case 0:
            return gd5440_svga_readl_linear(addr, svga);
        case 1:
            temp = gd5440_svga_readb_linear(addr + 1, svga);
            temp |= (gd5440_svga_readb_linear(addr, svga) << 8);
            temp |= (gd5440_svga_readb_linear(addr + 3, svga) << 16);
            temp |= (gd5440_svga_readb_linear(addr + 2, svga) << 24);

            if (svga->fast)
                gd5440_cycles -= svga->monitor->mon_video_timing_read_l;

            return temp;
        case 2:
            temp = gd5440_svga_readb_linear(addr + 3, svga);
            temp |= (gd5440_svga_readb_linear(addr + 2, svga) << 8);
            temp |= (gd5440_svga_readb_linear(addr + 1, svga) << 16);
            temp |= (gd5440_svga_readb_linear(addr, svga) << 24);

            if (svga->fast)
                gd5440_cycles -= svga->monitor->mon_video_timing_read_l;

            return temp;
        case 3:
            return 0xffffffff;
    }
}

static uint8_t
gd5436_aperture2_readb(UNUSED(uint32_t addr), void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    uint8_t  ap = gd5440_core_get_aperture(gd54xx, addr);

    if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
        gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED))
        return gd5440_core_mem_sys_dest_read(gd54xx, ap);

    return 0xff;
}

static uint16_t
gd5436_aperture2_readw(uint32_t addr, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    uint16_t  ret    = 0xffff;

    if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
        gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        ret = gd5436_aperture2_readb(addr, priv);
        ret |= gd5436_aperture2_readb(addr + 1, priv) << 8;
        return ret;
    }

    return ret;
}

static uint32_t
gd5436_aperture2_readl(uint32_t addr, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    uint32_t  ret    = 0xffffffff;

    if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
        gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        ret = gd5436_aperture2_readb(addr, priv);
        ret |= gd5436_aperture2_readb(addr + 1, priv) << 8;
        ret |= gd5436_aperture2_readb(addr + 2, priv) << 16;
        ret |= gd5436_aperture2_readb(addr + 3, priv) << 24;
        return ret;
    }

    return ret;
}

static void
gd5436_aperture2_writeb(UNUSED(uint32_t addr), uint8_t val, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    uint8_t  ap = gd5440_core_get_aperture(gd54xx, addr);

    if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
        gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED))
        gd5440_core_mem_sys_src_write(gd54xx, val, ap);
}

static void
gd5436_aperture2_writew(uint32_t addr, uint16_t val, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;

    if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
        gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        gd5436_aperture2_writeb(addr, val, gd54xx);
        gd5436_aperture2_writeb(addr + 1, val >> 8, gd54xx);
    }
}

static void
gd5436_aperture2_writel(uint32_t addr, uint32_t val, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;

    if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
        gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        gd5436_aperture2_writeb(addr, val, gd54xx);
        gd5436_aperture2_writeb(addr + 1, val >> 8, gd54xx);
        gd5436_aperture2_writeb(addr + 2, val >> 16, gd54xx);
        gd5436_aperture2_writeb(addr + 3, val >> 24, gd54xx);
    }
}

static void
gd5440_core_writeb_linear(uint32_t addr, uint8_t val, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;

    uint8_t ap       = gd5440_core_get_aperture(gd54xx, addr);

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA)) {
        gd5440_svga_write_linear(addr, val, svga);
        return;
    }

    addr &= 0x003fffff; /* 4 MB mask */

    if ((addr >= (svga->vram_max - 256)) && (addr < svga->vram_max)) {
        if ((svga->seqregs[0x17] & CIRRUS_MMIO_ENABLE) &&
            (svga->seqregs[0x17] & CIRRUS_MMIO_USE_PCIADDR)) {
            gd5440_core_543x_mmio_write(addr & 0x000000ff, val, gd54xx);
            return;
        }
    }

    /*
       Do mem sys src writes here if the blitter is neither paused,
       nor is there a second aperture.
     */
    if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
        !gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        gd5440_core_mem_sys_src_write(gd54xx, val, ap);
        return;
    }

    switch (ap) {
        default:
        case 0:
            break;
        case 1:
            /* 0 -> 1, 1 -> 0, 2 -> 3, 3 -> 2 */
            addr ^= 0x00000001;
            break;
        case 2:
            /* 0 -> 3, 1 -> 2, 2 -> 1, 3 -> 0 */
            addr ^= 0x00000003;
            break;
        case 3:
            return;
    }

    gd5440_svga_write_linear(addr, val, svga);
}

static void
gd5440_core_writew_linear(uint32_t addr, uint16_t val, void *priv)
{
    gd5440_core_t *gd54xx   = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga     = &gd54xx->svga;
    uint32_t  old_addr = addr;
    uint8_t ap         = gd5440_core_get_aperture(gd54xx, addr);

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA)) {
        gd5440_svga_writew_linear(addr, val, svga);
        return;
    }

    addr &= 0x003fffff; /* 4 MB mask */

    if ((addr >= (svga->vram_max - 256)) && (addr < svga->vram_max)) {
        if ((svga->seqregs[0x17] & CIRRUS_MMIO_ENABLE) &&
            (svga->seqregs[0x17] & CIRRUS_MMIO_USE_PCIADDR)) {
            gd5440_core_543x_mmio_writew(addr & 0x000000ff, val, gd54xx);
            return;
        }
    }

    /*
       Do mem sys src writes here if the blitter is neither paused,
       nor is there a second aperture.
     */
    if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
        !gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        gd5440_core_writeb_linear(old_addr, val, gd54xx);
        gd5440_core_writeb_linear(old_addr + 1, val >> 8, gd54xx);
        return;
    }

    if (svga->writemode < 4) {
        switch (ap) {
            default:
            case 0:
                gd5440_svga_writew_linear(addr, val, svga);
                return;
            case 2:
                addr ^= 0x00000002;
            case 1:
                gd5440_svga_writeb_linear(addr + 1, val & 0xff, svga);
                gd5440_svga_writeb_linear(addr, val >> 8, svga);

                if (svga->fast)
                    gd5440_cycles -= svga->monitor->mon_video_timing_write_w;
                return;
            case 3:
                return;
        }
    } else {
        switch (ap) {
            default:
            case 0:
                gd5440_svga_write_linear(addr, val & 0xff, svga);
                gd5440_svga_write_linear(addr + 1, val >> 8, svga);
                return;
            case 2:
                addr ^= 0x00000002;
                fallthrough;
            case 1:
                gd5440_svga_write_linear(addr + 1, val & 0xff, svga);
                gd5440_svga_write_linear(addr, val >> 8, svga);
                return;
            case 3:
                return;
        }
    }
}

static void
gd5440_core_writel_linear(uint32_t addr, uint32_t val, void *priv)
{
    gd5440_core_t *gd54xx   = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga     = &gd54xx->svga;
    uint32_t  old_addr = addr;
    uint8_t ap         = gd5440_core_get_aperture(gd54xx, addr);

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA)) {
        gd5440_svga_writel_linear(addr, val, svga);
        return;
    }

    addr &= 0x003fffff; /* 4 MB mask */

    if ((addr >= (svga->vram_max - 256)) && (addr < svga->vram_max)) {
        if ((svga->seqregs[0x17] & CIRRUS_MMIO_ENABLE) &&
            (svga->seqregs[0x17] & CIRRUS_MMIO_USE_PCIADDR)) {
            gd5440_core_543x_mmio_writel(addr & 0x000000ff, val, gd54xx);
            return;
        }
    }

    /*
       Do mem sys src writes here if the blitter is neither paused,
       nor is there a second aperture.
     */
    if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
        !gd5440_core_aperture2_enabled(gd54xx) && !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        gd5440_core_writeb_linear(old_addr, val, gd54xx);
        gd5440_core_writeb_linear(old_addr + 1, val >> 8, gd54xx);
        gd5440_core_writeb_linear(old_addr + 2, val >> 16, gd54xx);
        gd5440_core_writeb_linear(old_addr + 3, val >> 24, gd54xx);
        return;
    }

    if (svga->writemode < 4) {
        switch (ap) {
            default:
            case 0:
                gd5440_svga_writel_linear(addr, val, svga);
                return;
            case 1:
                gd5440_svga_writeb_linear(addr + 1, val & 0xff, svga);
                gd5440_svga_writeb_linear(addr, val >> 8, svga);
                gd5440_svga_writeb_linear(addr + 3, val >> 16, svga);
                gd5440_svga_writeb_linear(addr + 2, val >> 24, svga);
                return;
            case 2:
                gd5440_svga_writeb_linear(addr + 3, val & 0xff, svga);
                gd5440_svga_writeb_linear(addr + 2, val >> 8, svga);
                gd5440_svga_writeb_linear(addr + 1, val >> 16, svga);
                gd5440_svga_writeb_linear(addr, val >> 24, svga);
                return;
            case 3:
                return;
        }
    } else {
        switch (ap) {
            default:
            case 0:
                gd5440_svga_write_linear(addr, val & 0xff, svga);
                gd5440_svga_write_linear(addr + 1, val >> 8, svga);
                gd5440_svga_write_linear(addr + 2, val >> 16, svga);
                gd5440_svga_write_linear(addr + 3, val >> 24, svga);
                return;
            case 1:
                gd5440_svga_write_linear(addr + 1, val & 0xff, svga);
                gd5440_svga_write_linear(addr, val >> 8, svga);
                gd5440_svga_write_linear(addr + 3, val >> 16, svga);
                gd5440_svga_write_linear(addr + 2, val >> 24, svga);
                return;
            case 2:
                gd5440_svga_write_linear(addr + 3, val & 0xff, svga);
                gd5440_svga_write_linear(addr + 2, val >> 8, svga);
                gd5440_svga_write_linear(addr + 1, val >> 16, svga);
                gd5440_svga_write_linear(addr, val >> 24, svga);
                return;
            case 3:
                return;
        }
    }
}

static uint8_t
gd5440_core_read(uint32_t addr, void *priv)
{
    gd5440_svga_t   *svga   = (gd5440_svga_t *) priv;
    gd5440_core_t *gd54xx = (gd5440_core_t *) svga->local;

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA) && (((svga->gdcreg[6] >> 2) & 0x03) != 0x01))
        return gd5440_svga_read(addr, svga);

    if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
        !(gd54xx->blt.status & CIRRUS_BLT_PAUSED))
        return gd5440_core_mem_sys_dest_read(gd54xx, 0);

    (void) gd5440_xga_read_test(addr, svga);

    addr &= svga->banked_mask;
    addr = (addr & 0x7fff) + svga->extra_banks[(addr >> 15) & 1];
    return gd5440_svga_read_linear(addr, svga);
}

static uint16_t
gd5440_core_readw(uint32_t addr, void *priv)
{
    gd5440_svga_t   *svga   = (gd5440_svga_t *) priv;
    gd5440_core_t *gd54xx = (gd5440_core_t *) svga->local;
    uint16_t  ret;

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA) && (((svga->gdcreg[6] >> 2) & 0x03) != 0x01))
        return gd5440_svga_readw(addr, svga);

    if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
        !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        ret = gd5440_core_read(addr, svga);
        ret |= gd5440_core_read(addr + 1, svga) << 8;
        return ret;
    }

    (void) gd5440_xga_read_test(addr, svga);
    (void) gd5440_xga_read_test(addr + 1, svga);

    addr &= svga->banked_mask;
    addr = (addr & 0x7fff) + svga->extra_banks[(addr >> 15) & 1];
    return gd5440_svga_readw_linear(addr, svga);
}

static uint32_t
gd5440_core_readl(uint32_t addr, void *priv)
{
    gd5440_svga_t   *svga   = (gd5440_svga_t *) priv;
    gd5440_core_t *gd54xx = (gd5440_core_t *) svga->local;
    uint32_t  ret;

    if (!(svga->seqregs[0x07] & CIRRUS_SR7_BPP_SVGA) && (((svga->gdcreg[6] >> 2) & 0x03) != 0x01))
        return gd5440_svga_readl(addr, svga);

    if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
        !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        ret = gd5440_core_read(addr, svga);
        ret |= gd5440_core_read(addr + 1, svga) << 8;
        ret |= gd5440_core_read(addr + 2, svga) << 16;
        ret |= gd5440_core_read(addr + 3, svga) << 24;
        return ret;
    }

    (void) gd5440_xga_read_test(addr, svga);
    (void) gd5440_xga_read_test(addr + 1, svga);
    (void) gd5440_xga_read_test(addr + 2, svga);
    (void) gd5440_xga_read_test(addr + 3, svga);

    addr &= svga->banked_mask;
    addr = (addr & 0x7fff) + svga->extra_banks[(addr >> 15) & 1];
    return gd5440_svga_readl_linear(addr, svga);
}

static int
gd5440_core_543x_do_mmio(gd5440_svga_t *svga, uint32_t addr)
{
    if (svga->seqregs[0x17] & CIRRUS_MMIO_USE_PCIADDR)
        return 1;
    else
        return ((addr & ~0xff) == 0xb8000);
}

static void
gd5440_core_543x_mmio_write(uint32_t addr, uint8_t val, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;
    uint8_t   old;

    if (gd5440_core_543x_do_mmio(svga, addr)) {
        switch (addr & 0xff) {
            case 0x00:
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.bg_col = (gd54xx->blt.bg_col & 0xffffff00) | val;
                else
                    gd54xx->blt.bg_col = (gd54xx->blt.bg_col & 0xff00) | val;
                break;
            case 0x01:
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.bg_col = (gd54xx->blt.bg_col & 0xffff00ff) | (val << 8);
                else
                    gd54xx->blt.bg_col = (gd54xx->blt.bg_col & 0x00ff) | (val << 8);
                break;
            case 0x02:
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.bg_col = (gd54xx->blt.bg_col & 0xff00ffff) | (val << 16);
                break;
            case 0x03:
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.bg_col = (gd54xx->blt.bg_col & 0x00ffffff) | (val << 24);
                break;

            case 0x04:
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.fg_col = (gd54xx->blt.fg_col & 0xffffff00) | val;
                else
                    gd54xx->blt.fg_col = (gd54xx->blt.fg_col & 0xff00) | val;
                break;
            case 0x05:
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.fg_col = (gd54xx->blt.fg_col & 0xffff00ff) | (val << 8);
                else
                    gd54xx->blt.fg_col = (gd54xx->blt.fg_col & 0x00ff) | (val << 8);
                break;
            case 0x06:
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.fg_col = (gd54xx->blt.fg_col & 0xff00ffff) | (val << 16);
                break;
            case 0x07:
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.fg_col = (gd54xx->blt.fg_col & 0x00ffffff) | (val << 24);
                break;

            case 0x08:
                gd54xx->blt.width = (gd54xx->blt.width & 0xff00) | val;
                break;
            case 0x09:
                gd54xx->blt.width = (gd54xx->blt.width & 0x00ff) | (val << 8);
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.width &= 0x1fff;
                else
                    gd54xx->blt.width &= 0x07ff;
                break;
            case 0x0a:
                gd54xx->blt.height = (gd54xx->blt.height & 0xff00) | val;
                break;
            case 0x0b:
                gd54xx->blt.height = (gd54xx->blt.height & 0x00ff) | (val << 8);
                if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5436)
                    gd54xx->blt.height &= 0x07ff;
                else
                    gd54xx->blt.height &= 0x03ff;
                break;
            case 0x0c:
                gd54xx->blt.dst_pitch = (gd54xx->blt.dst_pitch & 0xff00) | val;
                break;
            case 0x0d:
                gd54xx->blt.dst_pitch = (gd54xx->blt.dst_pitch & 0x00ff) | (val << 8);
                gd54xx->blt.dst_pitch &= 0x1fff;
                break;
            case 0x0e:
                gd54xx->blt.src_pitch = (gd54xx->blt.src_pitch & 0xff00) | val;
                break;
            case 0x0f:
                gd54xx->blt.src_pitch = (gd54xx->blt.src_pitch & 0x00ff) | (val << 8);
                gd54xx->blt.src_pitch &= 0x1fff;
                break;

            case 0x10:
                gd54xx->blt.dst_addr = (gd54xx->blt.dst_addr & 0xffff00) | val;
                break;
            case 0x11:
                gd54xx->blt.dst_addr = (gd54xx->blt.dst_addr & 0xff00ff) | (val << 8);
                break;
            case 0x12:
                gd54xx->blt.dst_addr = (gd54xx->blt.dst_addr & 0x00ffff) | (val << 16);
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.dst_addr &= 0x3fffff;
                else
                    gd54xx->blt.dst_addr &= 0x1fffff;

                if ((svga->crtc[0x27] >= CIRRUS_ID_CLGD5436) &&
                    (gd54xx->blt.status & CIRRUS_BLT_AUTOSTART) &&
                    !(gd54xx->blt.status & CIRRUS_BLT_BUSY)) {
                    gd54xx->blt.status |= CIRRUS_BLT_BUSY;
                    gd5440_core_start_blit(0, 0xffffffff, gd54xx, svga);
                }
                break;

            case 0x14:
                gd54xx->blt.src_addr = (gd54xx->blt.src_addr & 0xffff00) | val;
                break;
            case 0x15:
                gd54xx->blt.src_addr = (gd54xx->blt.src_addr & 0xff00ff) | (val << 8);
                break;
            case 0x16:
                gd54xx->blt.src_addr = (gd54xx->blt.src_addr & 0x00ffff) | (val << 16);
                if (gd5440_core_is_5434(svga))
                    gd54xx->blt.src_addr &= 0x3fffff;
                else
                    gd54xx->blt.src_addr &= 0x1fffff;
                break;

            case 0x17:
                gd54xx->blt.mask = val;
                break;
            case 0x18:
                gd54xx->blt.mode = val;
                gd5440_core_543x_recalc_mapping(gd54xx);
                break;

            case 0x1a:
                gd54xx->blt.rop = val;
                break;

            case 0x1b:
                if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5436)
                    gd54xx->blt.modeext = val;
                break;

            case 0x1c:
                gd54xx->blt.trans_col = (gd54xx->blt.trans_col & 0xff00) | val;
                break;
            case 0x1d:
                gd54xx->blt.trans_col = (gd54xx->blt.trans_col & 0x00ff) | (val << 8);
                break;

            case 0x20:
                gd54xx->blt.trans_mask = (gd54xx->blt.trans_mask & 0xff00) | val;
                break;
            case 0x21:
                gd54xx->blt.trans_mask = (gd54xx->blt.trans_mask & 0x00ff) | (val << 8);
                break;

            case 0x40:
                old                = gd54xx->blt.status;
                gd54xx->blt.status = val;
                gd5440_core_543x_recalc_mapping(gd54xx);
                if (!(old & CIRRUS_BLT_RESET) && (gd54xx->blt.status & CIRRUS_BLT_RESET))
                    gd5440_core_reset_blit(gd54xx);
                else if (!(old & CIRRUS_BLT_START) && (gd54xx->blt.status & CIRRUS_BLT_START)) {
                    gd54xx->blt.status |= CIRRUS_BLT_BUSY;
                    gd5440_core_start_blit(0, 0xffffffff, gd54xx, svga);
                }
                break;

            default:
                break;
        }
    } else if (gd54xx->mmio_vram_overlap)
        gd5440_core_write(addr, val, svga);
}

static void
gd5440_core_543x_mmio_writeb(uint32_t addr, uint8_t val, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;

    if (!gd5440_core_543x_do_mmio(svga, addr) && !gd54xx->blt.ms_is_dest && gd54xx->countminusone &&
        !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        gd5440_core_mem_sys_src_write(gd54xx, val, 0);
        return;
    }

    gd5440_core_543x_mmio_write(addr, val, priv);
}

static void
gd5440_core_543x_mmio_writew(uint32_t addr, uint16_t val, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;

    if (gd5440_core_543x_do_mmio(svga, addr)) {
        gd5440_core_543x_mmio_write(addr, val & 0xff, gd54xx);
        gd5440_core_543x_mmio_write(addr + 1, val >> 8, gd54xx);
    } else if (gd54xx->mmio_vram_overlap) {
        if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
            !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
            gd5440_core_543x_mmio_write(addr, val & 0xff, gd54xx);
            gd5440_core_543x_mmio_write(addr + 1, val >> 8, gd54xx);
        } else {
            gd5440_core_write(addr, val, svga);
            gd5440_core_write(addr + 1, val >> 8, svga);
        }
    }
}

static void
gd5440_core_543x_mmio_writel(uint32_t addr, uint32_t val, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;

    if (gd5440_core_543x_do_mmio(svga, addr)) {
        gd5440_core_543x_mmio_write(addr, val & 0xff, gd54xx);
        gd5440_core_543x_mmio_write(addr + 1, val >> 8, gd54xx);
        gd5440_core_543x_mmio_write(addr + 2, val >> 16, gd54xx);
        gd5440_core_543x_mmio_write(addr + 3, val >> 24, gd54xx);
    } else if (gd54xx->mmio_vram_overlap) {
        if (gd54xx->countminusone && !gd54xx->blt.ms_is_dest &&
            !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
            gd5440_core_543x_mmio_write(addr, val & 0xff, gd54xx);
            gd5440_core_543x_mmio_write(addr + 1, val >> 8, gd54xx);
            gd5440_core_543x_mmio_write(addr + 2, val >> 16, gd54xx);
            gd5440_core_543x_mmio_write(addr + 3, val >> 24, gd54xx);
        } else {
            gd5440_core_write(addr, val, svga);
            gd5440_core_write(addr + 1, val >> 8, svga);
            gd5440_core_write(addr + 2, val >> 16, svga);
            gd5440_core_write(addr + 3, val >> 24, svga);
        }
    }
}

static uint8_t
gd5440_core_543x_mmio_read(uint32_t addr, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;
    uint8_t   ret    = 0xff;

    if (gd5440_core_543x_do_mmio(svga, addr)) {
        switch (addr & 0xff) {
            case 0x00:
                ret = gd54xx->blt.bg_col & 0xff;
                break;
            case 0x01:
                ret = (gd54xx->blt.bg_col >> 8) & 0xff;
                break;
            case 0x02:
                if (gd5440_core_is_5434(svga))
                    ret = (gd54xx->blt.bg_col >> 16) & 0xff;
                break;
            case 0x03:
                if (gd5440_core_is_5434(svga))
                    ret = (gd54xx->blt.bg_col >> 24) & 0xff;
                break;

            case 0x04:
                ret = gd54xx->blt.fg_col & 0xff;
                break;
            case 0x05:
                ret = (gd54xx->blt.fg_col >> 8) & 0xff;
                break;
            case 0x06:
                if (gd5440_core_is_5434(svga))
                    ret = (gd54xx->blt.fg_col >> 16) & 0xff;
                break;
            case 0x07:
                if (gd5440_core_is_5434(svga))
                    ret = (gd54xx->blt.fg_col >> 24) & 0xff;
                break;

            case 0x08:
                ret = gd54xx->blt.width & 0xff;
                break;
            case 0x09:
                if (gd5440_core_is_5434(svga))
                    ret = (gd54xx->blt.width >> 8) & 0x1f;
                else
                    ret = (gd54xx->blt.width >> 8) & 0x07;
                break;
            case 0x0a:
                ret = gd54xx->blt.height & 0xff;
                break;
            case 0x0b:
                if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5436)
                    ret = (gd54xx->blt.height >> 8) & 0x07;
                else
                    ret = (gd54xx->blt.height >> 8) & 0x03;
                break;
            case 0x0c:
                ret = gd54xx->blt.dst_pitch & 0xff;
                break;
            case 0x0d:
                ret = (gd54xx->blt.dst_pitch >> 8) & 0x1f;
                break;
            case 0x0e:
                ret = gd54xx->blt.src_pitch & 0xff;
                break;
            case 0x0f:
                ret = (gd54xx->blt.src_pitch >> 8) & 0x1f;
                break;

            case 0x10:
                ret = gd54xx->blt.dst_addr & 0xff;
                break;
            case 0x11:
                ret = (gd54xx->blt.dst_addr >> 8) & 0xff;
                break;
            case 0x12:
                if (gd5440_core_is_5434(svga))
                    ret = (gd54xx->blt.dst_addr >> 16) & 0x3f;
                else
                    ret = (gd54xx->blt.dst_addr >> 16) & 0x1f;
                break;

            case 0x14:
                ret = gd54xx->blt.src_addr & 0xff;
                break;
            case 0x15:
                ret = (gd54xx->blt.src_addr >> 8) & 0xff;
                break;
            case 0x16:
                if (gd5440_core_is_5434(svga))
                    ret = (gd54xx->blt.src_addr >> 16) & 0x3f;
                else
                    ret = (gd54xx->blt.src_addr >> 16) & 0x1f;
                break;

            case 0x17:
                ret = gd54xx->blt.mask;
                break;
            case 0x18:
                ret = gd54xx->blt.mode;
                break;

            case 0x1a:
                ret = gd54xx->blt.rop;
                break;

            case 0x1b:
                if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5436)
                    ret = gd54xx->blt.modeext;
                break;

            case 0x1c:
                ret = gd54xx->blt.trans_col & 0xff;
                break;
            case 0x1d:
                ret = (gd54xx->blt.trans_col >> 8) & 0xff;
                break;

            case 0x20:
                ret = gd54xx->blt.trans_mask & 0xff;
                break;
            case 0x21:
                ret = (gd54xx->blt.trans_mask >> 8) & 0xff;
                break;

            case 0x40:
                ret = gd54xx->blt.status;
                break;

            default:
                break;
        }
    } else if (gd54xx->mmio_vram_overlap)
        ret = gd5440_core_read(addr, svga);
    else if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
             !(gd54xx->blt.status & CIRRUS_BLT_PAUSED))
        ret = gd5440_core_mem_sys_dest_read(gd54xx, 0);

    return ret;
}

static uint16_t
gd5440_core_543x_mmio_readw(uint32_t addr, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;
    uint16_t  ret    = 0xffff;

    if (gd5440_core_543x_do_mmio(svga, addr))
        ret = gd5440_core_543x_mmio_read(addr, gd54xx) | (gd5440_core_543x_mmio_read(addr + 1, gd54xx) << 8);
    else if (gd54xx->mmio_vram_overlap)
        ret = gd5440_core_read(addr, svga) | (gd5440_core_read(addr + 1, svga) << 8);
    else if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
             !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        ret = gd5440_core_543x_mmio_read(addr, priv);
        ret |= gd5440_core_543x_mmio_read(addr + 1, priv) << 8;
        return ret;
    }

    return ret;
}

static uint32_t
gd5440_core_543x_mmio_readl(uint32_t addr, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;
    uint32_t  ret    = 0xffffffff;

    if (gd5440_core_543x_do_mmio(svga, addr))
        ret = gd5440_core_543x_mmio_read(addr, gd54xx) | (gd5440_core_543x_mmio_read(addr + 1, gd54xx) << 8) |
              (gd5440_core_543x_mmio_read(addr + 2, gd54xx) << 16) |
              (gd5440_core_543x_mmio_read(addr + 3, gd54xx) << 24);
    else if (gd54xx->mmio_vram_overlap)
        ret = gd5440_core_read(addr, svga) | (gd5440_core_read(addr + 1, svga) << 8) |
              (gd5440_core_read(addr + 2, gd54xx) << 16) | (gd5440_core_read(addr + 3, gd54xx) << 24);
    else if (gd54xx->countminusone && gd54xx->blt.ms_is_dest &&
             !(gd54xx->blt.status & CIRRUS_BLT_PAUSED)) {
        ret = gd5440_core_543x_mmio_read(addr, priv);
        ret |= gd5440_core_543x_mmio_read(addr + 1, priv) << 8;
        ret |= gd5440_core_543x_mmio_read(addr + 2, priv) << 16;
        ret |= gd5440_core_543x_mmio_read(addr + 3, priv) << 24;
        return ret;
    }

    return ret;
}

static void
gd5480_vgablt_write(uint32_t addr, uint8_t val, void *priv)
{
    addr &= 0x00000fff;

    if ((addr >= 0x00000100) && (addr < 0x00000200))
        gd5440_core_543x_mmio_writeb((addr & 0x000000ff) | 0x000b8000, val, priv);
    else if (addr < 0x00000100)
        gd5440_core_out(0x03c0 + addr, val, priv);
}

static void
gd5480_vgablt_writew(uint32_t addr, uint16_t val, void *priv)
{
    addr &= 0x00000fff;

    if ((addr >= 0x00000100) && (addr < 0x00000200))
        gd5440_core_543x_mmio_writew((addr & 0x000000ff) | 0x000b8000, val, priv);
    else if (addr < 0x00000100) {
        gd5480_vgablt_write(addr, val & 0xff, priv);
        gd5480_vgablt_write(addr + 1, val >> 8, priv);
    }
}

static void
gd5480_vgablt_writel(uint32_t addr, uint32_t val, void *priv)
{
    addr &= 0x00000fff;

    if ((addr >= 0x00000100) && (addr < 0x00000200))
        gd5440_core_543x_mmio_writel((addr & 0x000000ff) | 0x000b8000, val, priv);
    else if (addr < 0x00000100) {
        gd5480_vgablt_writew(addr, val & 0xffff, priv);
        gd5480_vgablt_writew(addr + 2, val >> 16, priv);
    }
}

static uint8_t
gd5480_vgablt_read(uint32_t addr, void *priv)
{
    uint8_t ret = 0xff;

    addr &= 0x00000fff;

    if ((addr >= 0x00000100) && (addr < 0x00000200))
        ret = gd5440_core_543x_mmio_read((addr & 0x000000ff) | 0x000b8000, priv);
    else if (addr < 0x00000100)
        ret = gd5440_core_in(0x03c0 + addr, priv);

    return ret;
}

static uint16_t
gd5480_vgablt_readw(uint32_t addr, void *priv)
{
    uint16_t ret = 0xffff;

    addr &= 0x00000fff;

    if ((addr >= 0x00000100) && (addr < 0x00000200))
        ret = gd5440_core_543x_mmio_readw((addr & 0x000000ff) | 0x000b8000, priv);
    else if (addr < 0x00000100) {
        ret = gd5480_vgablt_read(addr, priv);
        ret |= (gd5480_vgablt_read(addr + 1, priv) << 8);
    }

    return ret;
}

static uint32_t
gd5480_vgablt_readl(uint32_t addr, void *priv)
{
    uint32_t ret = 0xffffffff;

    addr &= 0x00000fff;

    if ((addr >= 0x00000100) && (addr < 0x00000200))
        ret = gd5440_core_543x_mmio_readl((addr & 0x000000ff) | 0x000b8000, priv);
    else if (addr < 0x00000100) {
        ret = gd5480_vgablt_readw(addr, priv);
        ret |= (gd5480_vgablt_readw(addr + 2, priv) << 16);
    }

    return ret;
}

static uint8_t
gd5440_core_color_expand(gd5440_core_t *gd54xx, int mask, int shift)
{
    uint8_t ret;

    if (gd54xx->blt.mode & CIRRUS_BLTMODE_TRANSPARENTCOMP)
        ret = gd54xx->blt.fg_col >> (shift << 3);
    else
        ret = mask ? (gd54xx->blt.fg_col >> (shift << 3)) : (gd54xx->blt.bg_col >> (shift << 3));

    return ret;
}

static int
gd5440_core_get_pixel_width(gd5440_core_t *gd54xx)
{
    int ret = 1;

    switch (gd54xx->blt.mode & CIRRUS_BLTMODE_PIXELWIDTHMASK) {
        case CIRRUS_BLTMODE_PIXELWIDTH8:
            ret = 1;
            break;
        case CIRRUS_BLTMODE_PIXELWIDTH16:
            ret = 2;
            break;
        case CIRRUS_BLTMODE_PIXELWIDTH24:
            ret = 3;
            break;
        case CIRRUS_BLTMODE_PIXELWIDTH32:
            ret = 4;
            break;

        default:
            break;
    }

    return ret;
}

static void
gd5440_core_blit(gd5440_core_t *gd54xx, uint8_t mask, uint8_t *dst, uint8_t target, int skip)
{
    int is_transp;
    int is_bgonly;

    /*
       skip indicates whether or not it is a pixel to be skipped (used for left skip);
       mask indicates transparency or not (only when transparent comparison is enabled):
           color expand: direct pattern bit; 1 = write, 0 = do not write
                         (the other way around in inverse mode);
       normal 8-bpp or 16-bpp: does not match transparent color = write,
                               matches transparent color = do not write
     */

    /* Make sure to always ignore transparency and skip in case of mem sys dest. */
    is_transp = (gd54xx->blt.mode & CIRRUS_BLTMODE_MEMSYSDEST) ?
                    0 : (gd54xx->blt.mode & CIRRUS_BLTMODE_TRANSPARENTCOMP);
    is_bgonly = (gd54xx->blt.mode & CIRRUS_BLTMODE_MEMSYSDEST) ?
                    0 : (gd54xx->blt.modeext & CIRRUS_BLTMODEEXT_BACKGROUNDONLY);
    skip      = (gd54xx->blt.mode & CIRRUS_BLTMODE_MEMSYSDEST) ? 0 : skip;

    if (is_transp) {
        if ((gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) &&
            (gd54xx->blt.modeext & CIRRUS_BLTMODEEXT_COLOREXPINV))
            mask = !mask;

        /* If mask is 1 and it is not a pixel to be skipped, write it. */
        if (mask && !skip)
            *dst = target;
    } else if ((gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) && is_bgonly) {
        /* If mask is 1 or it is not a pixel to be skipped, write it.
           (Skip only background pixels.) */
        if (mask || !skip)
            *dst = target;
    } else {
        /* If if it is not a pixel to be skipped, write it. */
        if (!skip)
            *dst = target;
    }
}

static int
gd5440_core_transparent_comp(gd5440_core_t *gd54xx, uint32_t xx, uint8_t src)
{
    gd5440_svga_t *svga = &gd54xx->svga;
    int     ret  = 1;

    if ((gd54xx->blt.pixel_width <= 2) && gd5440_core_has_transp(svga, 0)) {
        ret = src ^ ((uint8_t *) &(gd54xx->blt.trans_col))[xx];
        if (gd5440_core_has_transp(svga, 1))
            ret &= ~(((uint8_t *) &(gd54xx->blt.trans_mask))[xx]);
        ret = !ret;
    }

    return ret;
}

static void
gd5440_core_pattern_copy(gd5440_core_t *gd54xx)
{
    uint8_t  target;
    uint8_t  src;
    uint8_t *dst;
    int      pattern_y;
    int      pattern_pitch;
    uint32_t bitmask = 0;
    uint32_t pixel;
    uint32_t srca;
    uint32_t srca2;
    uint32_t dsta;
    gd5440_svga_t  *svga = &gd54xx->svga;

    pattern_pitch = gd54xx->blt.pixel_width << 3;

    if (gd54xx->blt.pixel_width == 3)
        pattern_pitch = 32;

    if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND)
        pattern_pitch = 1;

    dsta = gd54xx->blt.dst_addr & gd54xx->vram_mask;
    /* The vertical offset is in the three low-order bits of the Source Address register. */
    pattern_y = gd54xx->blt.src_addr & 0x07;

    /* Mode             Pattern bytes   Pattern line bytes
       ---------------------------------------------------
       Color Expansion    8              1
       8-bpp             64              8
       16-bpp           128             16
       24-bpp           256             32
       32-bpp           256             32
     */

    /* The boundary has to be equal to the size of the pattern. */
    srca = (gd54xx->blt.src_addr & ~0x07) & gd54xx->vram_mask;

    for (uint16_t y = 0; y <= gd54xx->blt.height; y++) {
        /* Go to the correct pattern line. */
        srca2 = srca + (pattern_y * pattern_pitch);
        pixel = 0;
        for (uint16_t x = 0; x <= gd54xx->blt.width; x += gd54xx->blt.pixel_width) {
            if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) {
                if (gd54xx->blt.modeext & CIRRUS_BLTMODEEXT_SOLIDFILL)
                    bitmask = 1;
                else
                    bitmask = svga->vram[srca2 & gd54xx->vram_mask] & (0x80 >> pixel);
            }
            for (int xx = 0; xx < gd54xx->blt.pixel_width; xx++) {
                if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND)
                    src = gd5440_core_color_expand(gd54xx, bitmask, xx);
                else {
                    src     = svga->vram[(srca2 + (x % (gd54xx->blt.pixel_width << 3)) + xx) & gd54xx->vram_mask];
                    bitmask = gd5440_core_transparent_comp(gd54xx, xx, src);
                }
                dst    = &(svga->vram[(dsta + x + xx) & gd54xx->vram_mask]);
                target = *dst;
                gd5440_core_rop(gd54xx, &target, &target, &src);
                if (gd54xx->blt.pixel_width == 3)
                    gd5440_core_blit(gd54xx, bitmask, dst, target, ((x + xx) < gd54xx->blt.pattern_x));
                else
                    gd5440_core_blit(gd54xx, bitmask, dst, target, (x < gd54xx->blt.pattern_x));
            }
            pixel                                                   = (pixel + 1) & 7;
            svga->changedvram[((dsta + x) & gd54xx->vram_mask) >> 12] = changeframecount;
        }
        pattern_y = (pattern_y + 1) & 7;
        dsta += gd54xx->blt.dst_pitch;
    }
}

static void
gd5440_core_reset_blit(gd5440_core_t *gd54xx)
{
    gd54xx->countminusone = 0;
    gd54xx->blt.status &= ~(CIRRUS_BLT_START | CIRRUS_BLT_BUSY | CIRRUS_BLT_FIFOUSED);
}

/* Each blit is either 1 byte -> 1 byte (non-color expand blit)
   or 1 byte -> 8/16/24/32 bytes (color expand blit). */
static void
gd5440_core_mem_sys_src(gd5440_core_t *gd54xx, uint32_t cpu_dat, uint32_t count)
{
    uint8_t *dst;
    uint8_t  exp;
    uint8_t  target;
    int      mask_shift;
    uint32_t byte_pos;
    uint32_t bitmask = 0;
    gd5440_svga_t  *svga = &gd54xx->svga;

    gd54xx->blt.ms_is_dest = 0;

    if (gd54xx->blt.mode & (CIRRUS_BLTMODE_MEMSYSDEST | CIRRUS_BLTMODE_PATTERNCOPY))
        gd5440_core_reset_blit(gd54xx);
    else if (count == 0xffffffff) {
        gd54xx->blt.dst_addr_backup = gd54xx->blt.dst_addr;
        gd54xx->blt.src_addr_backup = gd54xx->blt.src_addr;
        gd54xx->blt.x_count = gd54xx->blt.xx_count = 0;
        gd54xx->blt.y_count                        = 0;
        gd54xx->countminusone                      = 1;
        gd54xx->blt.sys_src32                      = 0x00000000;
        gd54xx->blt.sys_cnt                        = 0;
    } else if (gd54xx->countminusone) {
        if (!(gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) ||
            (gd54xx->blt.modeext & CIRRUS_BLTMODEEXT_DWORDGRANULARITY)) {
            if (!gd54xx->blt.xx_count && !gd54xx->blt.x_count)
                byte_pos = (((gd54xx->blt.mask >> 5) & 3) << 3);
            else
                byte_pos = 0;
            mask_shift = 31 - byte_pos;
            if (!(gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND))
                cpu_dat >>= byte_pos;
        } else
            mask_shift = 7;

        while (mask_shift > -1) {
            if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) {
                bitmask = (cpu_dat >> mask_shift) & 0x01;
                exp     = gd5440_core_color_expand(gd54xx, bitmask, gd54xx->blt.xx_count);
            } else {
                exp     = cpu_dat & 0xff;
                bitmask = gd5440_core_transparent_comp(gd54xx, gd54xx->blt.xx_count, exp);
            }

            dst    = &(svga->vram[gd54xx->blt.dst_addr_backup & gd54xx->vram_mask]);
            target = *dst;
            gd5440_core_rop(gd54xx, &target, &target, &exp);
            if ((gd54xx->blt.pixel_width == 3) && (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND))
                gd5440_core_blit(gd54xx, bitmask, dst, target,
                            ((gd54xx->blt.x_count + gd54xx->blt.xx_count) < gd54xx->blt.pattern_x));
            else
                gd5440_core_blit(gd54xx, bitmask, dst, target, (gd54xx->blt.x_count < gd54xx->blt.pattern_x));

            gd54xx->blt.dst_addr_backup += gd54xx->blt.dir;

            if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND)
                gd54xx->blt.xx_count = (gd54xx->blt.xx_count + 1) % gd54xx->blt.pixel_width;

            svga->changedvram[(gd54xx->blt.dst_addr_backup & gd54xx->vram_mask) >> 12] = changeframecount;

            if (!gd54xx->blt.xx_count) {
                /* 1 mask bit = 1 blitted pixel */
                if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND)
                    mask_shift--;
                else {
                    cpu_dat >>= 8;
                    mask_shift -= 8;
                }

                if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND)
                    gd54xx->blt.x_count = (gd54xx->blt.x_count + gd54xx->blt.pixel_width) % (gd54xx->blt.width + 1);
                else
                    gd54xx->blt.x_count = (gd54xx->blt.x_count + 1) % (gd54xx->blt.width + 1);

                if (!gd54xx->blt.x_count) {
                    gd54xx->blt.y_count = (gd54xx->blt.y_count + 1) % (gd54xx->blt.height + 1);
                    if (gd54xx->blt.y_count)
                        gd54xx->blt.dst_addr_backup = gd54xx->blt.dst_addr +
                                                      (gd54xx->blt.dst_pitch * gd54xx->blt.y_count *
                                                       gd54xx->blt.dir);
                    else
                        /* If we're here, the blit is over, reset. */
                        gd5440_core_reset_blit(gd54xx);
                    /* Stop blitting and request new data if end of line reached. */
                    break;
                }
            }
        }
    }
}

static void
gd5440_core_normal_blit(uint32_t count, gd5440_core_t *gd54xx, gd5440_svga_t *svga)
{
    uint8_t  src   = 0;
    uint8_t  dst;
    uint16_t width = gd54xx->blt.width;
    int      x_max = 0;
    int      shift = 0;
    int      mask = 0;
    uint32_t src_addr = gd54xx->blt.src_addr;
    uint32_t dst_addr = gd54xx->blt.dst_addr;

    x_max = gd54xx->blt.pixel_width << 3;

    gd54xx->blt.dst_addr_backup = gd54xx->blt.dst_addr;
    gd54xx->blt.src_addr_backup = gd54xx->blt.src_addr;
    gd54xx->blt.height_internal = gd54xx->blt.height;
    gd54xx->blt.x_count         = 0;
    gd54xx->blt.y_count         = 0;

    while (count) {
        src  = 0;
        mask = 0;

        if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) {
            mask  = svga->vram[src_addr & gd54xx->vram_mask] & (0x80 >> (gd54xx->blt.x_count / gd54xx->blt.pixel_width));
            shift = (gd54xx->blt.x_count % gd54xx->blt.pixel_width);
            src   = gd5440_core_color_expand(gd54xx, mask, shift);
        } else {
            src = svga->vram[src_addr & gd54xx->vram_mask];
            src_addr += gd54xx->blt.dir;
            mask = 1;
        }
        count--;

        dst                                                   = svga->vram[dst_addr & gd54xx->vram_mask];
        svga->changedvram[(dst_addr & gd54xx->vram_mask) >> 12] = changeframecount;

        gd5440_core_rop(gd54xx, &dst, &dst, (const uint8_t *) &src);

        if ((gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) && (gd54xx->blt.modeext & CIRRUS_BLTMODEEXT_COLOREXPINV))
            mask = !mask;

        /* This handles 8bpp and 16bpp non-color-expanding transparent comparisons. */
        if ((gd54xx->blt.mode & CIRRUS_BLTMODE_TRANSPARENTCOMP) &&
            !(gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) &&
            ((gd54xx->blt.mode & CIRRUS_BLTMODE_PIXELWIDTHMASK) <= CIRRUS_BLTMODE_PIXELWIDTH16) &&
            (src != ((gd54xx->blt.trans_mask >> (shift << 3)) & 0xff)))
            mask = 0;

        if (((gd54xx->blt.width - width) >= gd54xx->blt.pattern_x) &&
            !((gd54xx->blt.mode & CIRRUS_BLTMODE_TRANSPARENTCOMP) && !mask))
            svga->vram[dst_addr & gd54xx->vram_mask] = dst;

        dst_addr += gd54xx->blt.dir;
        gd54xx->blt.x_count++;

        if (gd54xx->blt.x_count == x_max) {
            gd54xx->blt.x_count = 0;
            if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND)
                src_addr++;
        }

        width--;
        if (width == 0xffff) {
            width    = gd54xx->blt.width;
            dst_addr = gd54xx->blt.dst_addr_backup = gd54xx->blt.dst_addr_backup +
                                                     (gd54xx->blt.dst_pitch * gd54xx->blt.dir);
            gd54xx->blt.y_count                    = (gd54xx->blt.y_count + gd54xx->blt.dir) & 7;

            if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) {
                if (gd54xx->blt.x_count != 0)
                    src_addr++;
            } else
                src_addr = gd54xx->blt.src_addr_backup = gd54xx->blt.src_addr_backup +
                                                         (gd54xx->blt.src_pitch * gd54xx->blt.dir);

            dst_addr &= gd54xx->vram_mask;
            gd54xx->blt.dst_addr_backup &= gd54xx->vram_mask;
            src_addr &= gd54xx->vram_mask;
            gd54xx->blt.src_addr_backup &= gd54xx->vram_mask;

            gd54xx->blt.x_count = 0;

            gd54xx->blt.height_internal--;
            if (gd54xx->blt.height_internal == 0xffff) {
                break;
            }
        }
    }

    /* Count exhausted, stuff still left to blit. */
    gd5440_core_reset_blit(gd54xx);
}

static void
gd5440_core_mem_sys_dest(uint32_t count, gd5440_core_t *gd54xx, gd5440_svga_t *svga)
{
    gd54xx->blt.ms_is_dest = 1;

    if (gd54xx->blt.mode & CIRRUS_BLTMODE_PATTERNCOPY) {
        gd5440_fatal("mem sys dest pattern copy not allowed (see 1994 manual)\n");
        gd5440_core_reset_blit(gd54xx);
    } else if (gd54xx->blt.mode & CIRRUS_BLTMODE_COLOREXPAND) {
        gd5440_fatal("mem sys dest color expand not allowed (see 1994 manual)\n");
        gd5440_core_reset_blit(gd54xx);
    } else {
        if (count == 0xffffffff) {
            gd54xx->blt.dst_addr_backup = gd54xx->blt.dst_addr;
            gd54xx->blt.msd_buf_cnt     = 0;
            gd54xx->blt.src_addr_backup = gd54xx->blt.src_addr;
            gd54xx->blt.x_count = gd54xx->blt.xx_count = 0;
            gd54xx->blt.y_count                        = 0;
            gd54xx->countminusone                      = 1;
            count                                      = 32;
        }

        gd54xx->blt.msd_buf_pos = 0;

        while (gd54xx->blt.msd_buf_pos < 32) {
            gd54xx->blt.msd_buf[gd54xx->blt.msd_buf_pos & 0x1f] = svga->vram[gd54xx->blt.src_addr_backup &
                                                                  gd54xx->vram_mask];
            gd54xx->blt.src_addr_backup += gd54xx->blt.dir;
            gd54xx->blt.msd_buf_pos++;

            gd54xx->blt.x_count = (gd54xx->blt.x_count + 1) % (gd54xx->blt.width + 1);

            if (!gd54xx->blt.x_count) {
                gd54xx->blt.y_count = (gd54xx->blt.y_count + 1) % (gd54xx->blt.height + 1);

                if (gd54xx->blt.y_count)
                    gd54xx->blt.src_addr_backup = gd54xx->blt.src_addr +
                                                  (gd54xx->blt.src_pitch * gd54xx->blt.y_count * gd54xx->blt.dir);
                else
                    gd54xx->countminusone = 2; /* Signal end of blit. */
                /* End of line reached, stop and notify regardless of how much we already transferred. */
                break;
            }
        }

        /*
           End of while.

           If the byte count we have blitted are not divisible by 4,
           round them up.
         */
        if (gd54xx->blt.msd_buf_pos & 3)
            gd54xx->blt.msd_buf_cnt = (gd54xx->blt.msd_buf_pos & ~3) + 4;
        else
            gd54xx->blt.msd_buf_cnt = gd54xx->blt.msd_buf_pos;
        gd54xx->blt.msd_buf_pos = 0;
        return;
    }
}

static void
gd5440_core_start_blit(uint32_t cpu_dat, uint32_t count, gd5440_core_t *gd54xx, gd5440_svga_t *svga)
{
    if ((gd54xx->blt.mode & CIRRUS_BLTMODE_BACKWARDS) &&
        !(gd54xx->blt.mode & (CIRRUS_BLTMODE_PATTERNCOPY | CIRRUS_BLTMODE_COLOREXPAND)) &&
        !(gd54xx->blt.mode & CIRRUS_BLTMODE_TRANSPARENTCOMP))
        gd54xx->blt.dir = -1;
    else
        gd54xx->blt.dir = 1;

    gd54xx->blt.pixel_width = gd5440_core_get_pixel_width(gd54xx);

    if (gd54xx->blt.mode & (CIRRUS_BLTMODE_PATTERNCOPY | CIRRUS_BLTMODE_COLOREXPAND)) {
        if (gd54xx->blt.pixel_width == 3)
            gd54xx->blt.pattern_x = gd54xx->blt.mask & 0x1f; /* (Mask & 0x1f) bytes. */
        else
            /* (Mask & 0x07) pixels. */
            gd54xx->blt.pattern_x = (gd54xx->blt.mask & 0x07) * gd54xx->blt.pixel_width;
    } else
        gd54xx->blt.pattern_x = 0; /* No skip in normal blit mode. */

    if (gd54xx->blt.mode & CIRRUS_BLTMODE_MEMSYSSRC)
        gd5440_core_mem_sys_src(gd54xx, cpu_dat, count);
    else if (gd54xx->blt.mode & CIRRUS_BLTMODE_MEMSYSDEST)
        gd5440_core_mem_sys_dest(count, gd54xx, svga);
    else if (gd54xx->blt.mode & CIRRUS_BLTMODE_PATTERNCOPY) {
        gd5440_core_pattern_copy(gd54xx);
        gd5440_core_reset_blit(gd54xx);
    } else
        gd5440_core_normal_blit(count, gd54xx, svga);
}

static uint8_t
gd5440_core_pci_read(UNUSED(int func), int addr, UNUSED(int len), void *priv)
{
    const gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    const gd5440_svga_t   *svga   = &gd54xx->svga;
    uint8_t         ret    = 0x00;

    if ((addr >= 0x30) && (addr <= 0x33) && (!gd54xx->has_bios))
        ret = 0x00;
    else  switch (addr) {
            case 0x00:
                ret = 0x13; /*Cirrus Logic*/
                break;
            case 0x01:
                ret = 0x10;
                break;

            case 0x02:
                ret = svga->crtc[0x27];
                break;
            case 0x03:
                ret = 0x00;
                break;

            case PCI_REG_COMMAND:
                /* Respond to IO and memory accesses */
                ret = gd54xx->pci_regs[PCI_REG_COMMAND];
                break;

            case 0x07:
                ret = 0x02; /*Fast DEVSEL timing*/
                break;

            case 0x08:
                ret = gd54xx->rev; /*Revision ID*/
                break;
            case 0x09:
                ret = 0x00; /*Programming interface*/
                break;

            case 0x0a:
                ret = 0x00; /*Supports VGA interface*/
                break;
            case 0x0b:
                ret = 0x03;
                break;

            case 0x10:
                ret = 0x08; /*Linear frame buffer address*/
                break;
            case 0x11:
                ret = 0x00;
                break;
            case 0x12:
                ret = 0x00;
                break;
            case 0x13:
                ret = gd54xx->lfb_base >> 24;
                if (svga->crtc[0x27] == CIRRUS_ID_CLGD5480)
                    ret &= 0xfe;
                break;

            case 0x14:
                ret = 0x00; /*PCI VGA/BitBLT Register Base Address*/
                break;
            case 0x15:
                ret = (svga->crtc[0x27] == CIRRUS_ID_CLGD5480) ? ((gd54xx->vgablt_base >> 8) & 0xf0) : 0x00;
                break;
            case 0x16:
                ret = (svga->crtc[0x27] == CIRRUS_ID_CLGD5480) ? ((gd54xx->vgablt_base >> 16) & 0xff) : 0x00;
                break;
            case 0x17:
                ret = (svga->crtc[0x27] == CIRRUS_ID_CLGD5480) ? ((gd54xx->vgablt_base >> 24) & 0xff) : 0x00;
                break;

            case 0x2c:
                ret = (svga->crtc[0x27] == CIRRUS_ID_CLGD5480) ? gd54xx->bios_rom.rom[0x7ffc] : 0x00;
                break;
            case 0x2d:
                ret = (svga->crtc[0x27] == CIRRUS_ID_CLGD5480) ? gd54xx->bios_rom.rom[0x7ffd] : 0x00;
                break;
            case 0x2e:
                ret = (svga->crtc[0x27] == CIRRUS_ID_CLGD5480) ? gd54xx->bios_rom.rom[0x7ffe] : 0x00;
                break;

            case 0x30:
                ret = (gd54xx->pci_regs[0x30] & 0x01); /*BIOS ROM address*/
                break;
            case 0x31:
                ret = 0x00;
                break;
            case 0x32:
                ret = gd54xx->pci_regs[0x32];
                break;
            case 0x33:
                ret = gd54xx->pci_regs[0x33];
                break;

            case 0x3c:
                ret = gd54xx->int_line;
                break;
            case 0x3d:
                ret = PCI_INTA;
                break;

            default:
                break;
    }

    return ret;
}

static void
gd5440_core_pci_write(UNUSED(int func), int addr, UNUSED(int len), uint8_t val, void *priv)
{
    gd5440_core_t     *gd54xx = (gd5440_core_t *) priv;
    const gd5440_svga_t *svga   = &gd54xx->svga;
    uint32_t      byte;

    if ((addr >= 0x30) && (addr <= 0x33) && (!gd54xx->has_bios))
        return;

    switch (addr) {
        case PCI_REG_COMMAND:
            gd54xx->pci_regs[PCI_REG_COMMAND] = val & 0x23;
            gd5440_mem_mapping_disable(&gd54xx->vgablt_mapping);
            gd5440_io_removehandler(0x03c0, 0x0020, gd5440_core_in, NULL, NULL, gd5440_core_out, NULL, NULL, gd54xx);
            if (val & PCI_COMMAND_IO)
                gd5440_io_sethandler(0x03c0, 0x0020, gd5440_core_in, NULL, NULL, gd5440_core_out, NULL, NULL, gd54xx);
            if ((val & PCI_COMMAND_MEM) && (gd54xx->vgablt_base != 0x00000000) && (gd54xx->vgablt_base < 0xfff00000))
                gd5440_mem_mapping_set_addr(&gd54xx->vgablt_mapping, gd54xx->vgablt_base, 0x1000);
            if ((gd54xx->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM) && (gd54xx->pci_regs[0x30] & 0x01)) {
                uint32_t addr = (gd54xx->pci_regs[0x32] << 16) | (gd54xx->pci_regs[0x33] << 24);
                gd5440_mem_mapping_set_addr(&gd54xx->bios_rom.mapping, addr, 0x8000);
            } else
                gd5440_mem_mapping_disable(&gd54xx->bios_rom.mapping);
            gd5440_core_543x_recalc_mapping(gd54xx);
            break;

        case 0x13:
            /*
               5480, like 5446 rev. B, has a 32 MB aperture, with the second set used for
               BitBLT transfers.
             */
            if (svga->crtc[0x27] == CIRRUS_ID_CLGD5480)
                val &= 0xfe;
            gd54xx->lfb_base = val << 24;
            gd5440_core_543x_recalc_mapping(gd54xx);
            break;

        case 0x15:
        case 0x16:
        case 0x17:
            if (svga->crtc[0x27] != CIRRUS_ID_CLGD5480)
                return;
            byte = (addr & 3) << 3;
            gd54xx->vgablt_base &= ~(0xff << byte);
            if (addr == 0x15)
                val &= 0xf0;
            gd54xx->vgablt_base |= (val << byte);
            gd5440_mem_mapping_disable(&gd54xx->vgablt_mapping);
            if ((gd54xx->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM) &&
                (gd54xx->vgablt_base != 0x00000000) && (gd54xx->vgablt_base < 0xfff00000))
                gd5440_mem_mapping_set_addr(&gd54xx->vgablt_mapping, gd54xx->vgablt_base, 0x1000);
            break;

        case 0x30:
        case 0x32:
        case 0x33:
            gd54xx->pci_regs[addr] = val;
            if ((gd54xx->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM) && (gd54xx->pci_regs[0x30] & 0x01)) {
                uint32_t addr = (gd54xx->pci_regs[0x32] << 16) | (gd54xx->pci_regs[0x33] << 24);
                gd5440_mem_mapping_set_addr(&gd54xx->bios_rom.mapping, addr, 0x8000);
            } else
                gd5440_mem_mapping_disable(&gd54xx->bios_rom.mapping);
            return;

        case 0x3c:
            gd54xx->int_line = val;
            return;

        default:
            break;
    }
}

static uint8_t
gd5428_mca_read(int port, void *priv)
{
    const gd5440_core_t *gd54xx = (gd5440_core_t *) priv;

    return gd54xx->pos_regs[port & 7];
}

static void
gd5428_mca_write(int port, uint8_t val, void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;

    if (port < 0x102)
        return;

    gd54xx->pos_regs[port & 7] = val;
    gd5440_mem_mapping_disable(&gd54xx->bios_rom.mapping);
    if (gd54xx->pos_regs[2] & 0x01)
        gd5440_mem_mapping_enable(&gd54xx->bios_rom.mapping);
}

static uint8_t
gd5428_mca_feedb(void *priv)
{
    const gd5440_core_t *gd54xx = (gd5440_core_t *) priv;

    return gd54xx->pos_regs[2] & 0x01;
}

static void
gd5440_core_reset(void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;
    gd5440_svga_t   *svga   = &gd54xx->svga;

    memset(svga->crtc, 0x00, sizeof(svga->crtc));
    memset(svga->seqregs, 0x00, sizeof(svga->seqregs));
    memset(svga->gdcreg, 0x00, sizeof(svga->gdcreg));
    svga->crtc[0]     = 63;
    svga->crtc[6]     = 255;
    svga->dispontime  = 1000ULL << 32;
    svga->dispofftime = 1000ULL << 32;
    svga->bpp         = 8;

    gd5440_io_removehandler(0x03c0, 0x0020, gd5440_core_in, NULL, NULL, gd5440_core_out, NULL, NULL, gd54xx);
    gd5440_io_sethandler(0x03c0, 0x0020, gd5440_core_in, NULL, NULL, gd5440_core_out, NULL, NULL, gd54xx);

    gd5440_mem_mapping_disable(&gd54xx->vgablt_mapping);
    if (gd54xx->has_bios && (gd54xx->pci || gd54xx->mca))
        gd5440_mem_mapping_disable(&gd54xx->bios_rom.mapping);

    memset(gd54xx->pci_regs, 0x00, 256);

    gd5440_mem_mapping_disable(&gd54xx->mmio_mapping);
    gd5440_mem_mapping_disable(&gd54xx->linear_mapping);
    gd5440_mem_mapping_disable(&gd54xx->aperture2_mapping);
    gd5440_mem_mapping_disable(&gd54xx->vgablt_mapping);

    gd5440_core_543x_recalc_mapping(gd54xx);
    gd5440_core_recalc_banking(gd54xx);

    svga->hwcursor.yoff = svga->hwcursor.xoff = 0;

    if (gd54xx->id >= CIRRUS_ID_CLGD5420) {
        gd54xx->vclk_n[0] = 0x4a;
        gd54xx->vclk_d[0] = 0x2b;
        gd54xx->vclk_n[1] = 0x5b;
        gd54xx->vclk_d[1] = 0x2f;
        gd54xx->vclk_n[2] = 0x45;
        gd54xx->vclk_d[2] = 0x30;
        gd54xx->vclk_n[3] = 0x7e;
        gd54xx->vclk_d[3] = 0x33;
    } else {
        gd54xx->vclk_n[0] = 0x66;
        gd54xx->vclk_d[0] = 0x3b;
        gd54xx->vclk_n[1] = 0x5b;
        gd54xx->vclk_d[1] = 0x2f;
        gd54xx->vclk_n[2] = 0x45;
        gd54xx->vclk_d[2] = 0x2c;
        gd54xx->vclk_n[3] = 0x7e;
        gd54xx->vclk_d[3] = 0x33;
    }

    svga->extra_banks[1] = 0x8000;

    gd54xx->pci_regs[PCI_REG_COMMAND] = 7;

    gd54xx->pci_regs[0x30] = 0x00;
    gd54xx->pci_regs[0x32] = 0x0c;
    gd54xx->pci_regs[0x33] = 0x00;

    svga->crtc[0x27] = gd54xx->id;

    svga->seqregs[6] = 0x0f;
    if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5429)
        gd54xx->unlocked = 1;
    else
        gd54xx->unlocked = 0;
}

static void *
gd5440_core_init(const device_t *info)
{
    gd5440_core_t   *gd54xx = calloc(1, sizeof(gd5440_core_t));
    gd5440_svga_t     *svga   = &gd54xx->svga;
    int         id     = info->local & 0xff;
    int         vram;
    const char *romfn  = NULL;
    const char *romfn1 = NULL;
    const char *romfn2 = NULL;

    gd54xx->pci   = !!(info->flags & DEVICE_PCI);
    gd54xx->vlb   = !!(info->flags & DEVICE_VLB);
    gd54xx->mca   = !!(info->flags & DEVICE_MCA);
    gd54xx->bit32 = gd54xx->pci || gd54xx->vlb;

    gd54xx->rev      = 0;
    gd54xx->has_bios = 1;

    gd54xx->id = id;

    if (gd54xx->vlb && ((gd54xx->id == CIRRUS_ID_CLGD5430) ||
                        (gd54xx->id == CIRRUS_ID_CLGD5434) ||
                        (gd54xx->id == CIRRUS_ID_CLGD5434_4) ||
                        (gd54xx->id == CIRRUS_ID_CLGD5440)))
        gd54xx->vlb_lfb_base = gd5440_device_get_config_int("lfb_base") << 20;

    switch (id) {
        case CIRRUS_ID_CLGD5401:
        if (info->local & 0x100)
                romfn = BIOS_GD5401_ONBOARD_PATH;
            else
				romfn = BIOS_GD5401_PATH;
			break;

        case CIRRUS_ID_CLGD5402:
            if (info->local & 0x200)
                romfn = NULL;
            else if (info->local & 0x100)
                romfn = BIOS_GD5402_ONBOARD_PATH;
            else
                romfn = BIOS_GD5402_PATH;
            break;

        case CIRRUS_ID_CLGD5420:
            if (info->local & 0x200)
                romfn = NULL;
            else
                romfn = BIOS_GD5420_PATH;
            break;

        case CIRRUS_ID_CLGD5422:
            if (info->local & 0x100) {
                romfn1 = BIOS_GD5428_BOCA_ISA_PATH_1;
                romfn2 = BIOS_GD5428_BOCA_ISA_PATH_2;
            }
            else
            romfn = BIOS_GD5422_PATH;
            break;

        case CIRRUS_ID_CLGD5424:
            if (info->local & 0x200)
                romfn = /*NULL*/ "roms/machines/advantage40xxd/AST101.09A";
            else
                romfn = BIOS_GD5422_PATH;
            break;

        case CIRRUS_ID_CLGD5426:
            if (info->local & 0x200)
                romfn = NULL;
            else {
                if (info->local & 0x100)
                    romfn = BIOS_GD5426_DIAMOND_A1_ISA_PATH;
                else {
                    if (gd54xx->vlb)
                        romfn = BIOS_GD5428_PATH;
                    else if (gd54xx->mca)
                        romfn = BIOS_GD5426_MCA_PATH;
                    else
                        romfn = BIOS_GD5428_ISA_PATH;
                }
            }
            break;

        case CIRRUS_ID_CLGD5428:
            if (info->local & 0x200) {
                if (machines[machine].init == machine_at_acera1g_init)
                    romfn = BIOS_GD5428_ONBOARD_ACER_PATH;
                else
                    romfn            = NULL;
                gd54xx->has_bios = 0;
            } else if (info->local & 0x100)
                    romfn = BIOS_GD5428_DIAMOND_B1_VLB_PATH;
            else {
                if (gd54xx->vlb)
                    romfn = BIOS_GD5428_PATH;
                else if (gd54xx->mca)
                    romfn = BIOS_GD5428_MCA_PATH;
                else
                    romfn = BIOS_GD5428_ISA_PATH;
            }
            break;

        case CIRRUS_ID_CLGD5429:
            romfn = BIOS_GD5429_PATH;
            break;

        case CIRRUS_ID_CLGD5432:
        case CIRRUS_ID_CLGD5434_4:
            if (info->local & 0x200) {
                romfn            = NULL;
                gd54xx->has_bios = 0;
            }
            break;

        case CIRRUS_ID_CLGD5434:
            if (info->local & 0x200) {
                romfn            = NULL;
                gd54xx->has_bios = 0;
            } else if (gd54xx->vlb) {
                romfn = BIOS_GD5434_ORCHID_VLB_PATH;
            } else {
                if (info->local & 0x100)
                    romfn = BIOS_GD5434_DIAMOND_A3_ISA_PATH;
                else
                    romfn = BIOS_GD5434_PATH;
            }
            break;

        case CIRRUS_ID_CLGD5436:
            if ((info->local & 0x200) &&
                (machines[machine].init != machine_at_sb486pv_init)) {
                romfn            = NULL;
                gd54xx->has_bios = 0;
            } else
                romfn = BIOS_GD5436_PATH;
            break;

        case CIRRUS_ID_CLGD5430:
            if (info->local & 0x400) {
                /* CL-GD 5440 */
                gd54xx->rev = 0x47;
                if (info->local & 0x200) {
                    romfn            = NULL;
                    gd54xx->has_bios = 0;
                } else
                    romfn = BIOS_GD5440_PATH;
            } else {
                /* CL-GD 5430 */
                if (info->local & 0x200) {
                    romfn            = NULL;
                    gd54xx->has_bios = 0;
                } else if (gd54xx->pci)
                    romfn = BIOS_GD5430_PATH;
                else if ((gd54xx->vlb) && (info->local & 0x100))
                    romfn = BIOS_GD5430_ORCHID_VLB_PATH;
                else
                    romfn = BIOS_GD5430_DIAMOND_A8_VLB_PATH;
            }
            break;

        case CIRRUS_ID_CLGD5446:
            if (info->local & 0x100)
                romfn = BIOS_GD5446_STB_PATH;
            else
                romfn = BIOS_GD5446_PATH;
            break;

        case CIRRUS_ID_CLGD5480:
            romfn = BIOS_GD5480_PATH;
            break;

        default:
            break;
    }

    if (info->flags & DEVICE_MCA) {
        if (id == CIRRUS_ID_CLGD5428)
            vram              = 1024;
        else
            vram = gd5440_device_get_config_int("memory");

        gd54xx->vram_size = vram << 10;
    } else {
        if (id <= CIRRUS_ID_CLGD5428) {
            if ((id == CIRRUS_ID_CLGD5428) && (info->local & 0x200) && (info->local & 0x1000))
                vram = 1024;
            else if ((id == CIRRUS_ID_CLGD5426) && (info->local & 0x200) && (info->local & 0x1000))
                vram = 1024;
            else if ((id == CIRRUS_ID_CLGD5420) && (info->local & 0x200))
                vram = 512;
            else if (id == CIRRUS_ID_CLGD5401)
                vram = 256;
            else
                vram = gd5440_device_get_config_int("memory");

            gd54xx->vram_size = vram << 10;
        } else {
            if ((id == CIRRUS_ID_CLGD5436) && (info->local & 0x200) && (info->local & 0x1000))
                vram = 1;
            else
                vram              = gd5440_device_get_config_int("memory");
            gd54xx->vram_size = vram << 20;
        }
    }
    gd54xx->vram_mask = gd54xx->vram_size - 1;

    if (romfn)
        gd5440_rom_init(&gd54xx->bios_rom, romfn, 0xc0000, 0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);
    else if (romfn1 && romfn2)
        gd5440_rom_init_interleaved(&gd54xx->bios_rom, BIOS_GD5428_BOCA_ISA_PATH_1, BIOS_GD5428_BOCA_ISA_PATH_2, 0xc0000,
                             0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);

    if ((info->flags & DEVICE_ISA) || (info->flags & DEVICE_ISA16))
        gd5440_video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_gd54xx_isa);
    else if (info->flags & DEVICE_PCI)
        gd5440_video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_gd54xx_pci);
    else
        gd5440_video_inform(VIDEO_FLAG_TYPE_SPECIAL, &timing_gd54xx_vlb);

    if (id >= CIRRUS_ID_CLGD5426) {
        gd5440_svga_init(info, &gd54xx->svga, gd54xx, gd54xx->vram_size,
                  gd5440_core_recalctimings, gd5440_core_in, gd5440_core_out,
                  gd5440_core_hwcursor_draw, gd5440_core_overlay_draw);
    } else {
        gd5440_svga_init(info, &gd54xx->svga, gd54xx, gd54xx->vram_size,
                  gd5440_core_recalctimings, gd5440_core_in, gd5440_core_out,
                  gd5440_core_hwcursor_draw, NULL);
    }
    svga->vblank_start = gd5440_core_vblank_start;
    svga->ven_write    = gd5440_core_write_modes45;
    if ((vram == 1) || (vram >= 256 && vram <= 1024))
        svga->decode_mask = gd54xx->vram_mask;

    svga->read = gd5440_core_read;
    svga->readw = gd5440_core_readw;
    svga->write = gd5440_core_write;
    svga->writew = gd5440_core_writew;
    if (gd54xx->bit32) {
        svga->readl = gd5440_core_readl;
        svga->writel = gd5440_core_writel;
        gd5440_mem_mapping_set_handler(&svga->mapping, gd5440_core_read, gd5440_core_readw, gd5440_core_readl,
                                gd5440_core_write, gd5440_core_writew, gd5440_core_writel);
        gd5440_mem_mapping_add(&gd54xx->mmio_mapping, 0, 0,
                        gd5440_core_543x_mmio_read, gd5440_core_543x_mmio_readw, gd5440_core_543x_mmio_readl,
                        gd5440_core_543x_mmio_writeb, gd5440_core_543x_mmio_writew, gd5440_core_543x_mmio_writel,
                        NULL, MEM_MAPPING_EXTERNAL, gd54xx);
        gd5440_mem_mapping_add(&gd54xx->linear_mapping, 0, 0,
                        gd5440_core_readb_linear, gd5440_core_readw_linear, gd5440_core_readl_linear,
                        gd5440_core_writeb_linear, gd5440_core_writew_linear, gd5440_core_writel_linear,
                        NULL, MEM_MAPPING_EXTERNAL, gd54xx);
        gd5440_mem_mapping_add(&gd54xx->aperture2_mapping, 0, 0,
                        gd5436_aperture2_readb, gd5436_aperture2_readw, gd5436_aperture2_readl,
                        gd5436_aperture2_writeb, gd5436_aperture2_writew, gd5436_aperture2_writel,
                        NULL, MEM_MAPPING_EXTERNAL, gd54xx);
        gd5440_mem_mapping_add(&gd54xx->vgablt_mapping, 0, 0,
                        gd5480_vgablt_read, gd5480_vgablt_readw, gd5480_vgablt_readl,
                        gd5480_vgablt_write, gd5480_vgablt_writew, gd5480_vgablt_writel,
                        NULL, MEM_MAPPING_EXTERNAL, gd54xx);
    } else {
        svga->readl = NULL;
        svga->writel = NULL;
        gd5440_mem_mapping_set_handler(&svga->mapping, gd5440_core_read, gd5440_core_readw, NULL,
                                gd5440_core_write, gd5440_core_writew, NULL);
        gd5440_mem_mapping_add(&gd54xx->mmio_mapping, 0, 0,
                        gd5440_core_543x_mmio_read, gd5440_core_543x_mmio_readw, NULL,
                        gd5440_core_543x_mmio_writeb, gd5440_core_543x_mmio_writew, NULL,
                        NULL, MEM_MAPPING_EXTERNAL, gd54xx);
        gd5440_mem_mapping_add(&gd54xx->linear_mapping, 0, 0,
                        gd5440_core_readb_linear, gd5440_core_readw_linear, NULL,
                        gd5440_core_writeb_linear, gd5440_core_writew_linear, NULL,
                        NULL, MEM_MAPPING_EXTERNAL, gd54xx);
        gd5440_mem_mapping_add(&gd54xx->aperture2_mapping, 0, 0,
                        gd5436_aperture2_readb, gd5436_aperture2_readw, NULL,
                        gd5436_aperture2_writeb, gd5436_aperture2_writew, NULL,
                        NULL, MEM_MAPPING_EXTERNAL, gd54xx);
        gd5440_mem_mapping_add(&gd54xx->vgablt_mapping, 0, 0,
                        gd5480_vgablt_read, gd5480_vgablt_readw, NULL,
                        gd5480_vgablt_write, gd5480_vgablt_writew, NULL,
                        NULL, MEM_MAPPING_EXTERNAL, gd54xx);
    }
    gd5440_io_sethandler(0x03c0, 0x0020, gd5440_core_in, NULL, NULL, gd5440_core_out, NULL, NULL, gd54xx);

    if (gd54xx->pci && (id >= CIRRUS_ID_CLGD5430)) {
        if (info->local & 0x200)
            gd5440_pci_add_card(PCI_ADD_VIDEO, gd5440_core_pci_read, gd5440_core_pci_write, gd54xx, &gd54xx->pci_slot);
        else
            gd5440_pci_add_card(PCI_ADD_NORMAL, gd5440_core_pci_read, gd5440_core_pci_write, gd54xx, &gd54xx->pci_slot);
        gd5440_mem_mapping_disable(&gd54xx->bios_rom.mapping);
    }

    if ((id <= CIRRUS_ID_CLGD5429) || (!gd54xx->pci && !gd54xx->vlb))
        gd5440_mem_mapping_set_base_ignore(&gd54xx->linear_mapping, 0xff000000);

    gd5440_mem_mapping_disable(&gd54xx->mmio_mapping);
    gd5440_mem_mapping_disable(&gd54xx->linear_mapping);
    gd5440_mem_mapping_disable(&gd54xx->aperture2_mapping);
    gd5440_mem_mapping_disable(&gd54xx->vgablt_mapping);

    svga->hwcursor.yoff = svga->hwcursor.xoff = 0;

    if (id >= CIRRUS_ID_CLGD5420) {
        gd54xx->vclk_n[0] = 0x4a;
        gd54xx->vclk_d[0] = 0x2b;
        gd54xx->vclk_n[1] = 0x5b;
        gd54xx->vclk_d[1] = 0x2f;
        gd54xx->vclk_n[2] = 0x45;
        gd54xx->vclk_d[2] = 0x30;
        gd54xx->vclk_n[3] = 0x7e;
        gd54xx->vclk_d[3] = 0x33;
    } else {
        gd54xx->vclk_n[0] = 0x66;
        gd54xx->vclk_d[0] = 0x3b;
        gd54xx->vclk_n[1] = 0x5b;
        gd54xx->vclk_d[1] = 0x2f;
        gd54xx->vclk_n[2] = 0x45;
        gd54xx->vclk_d[2] = 0x2c;
        gd54xx->vclk_n[3] = 0x7e;
        gd54xx->vclk_d[3] = 0x33;
    }

    svga->extra_banks[1] = 0x8000;

    gd54xx->pci_regs[PCI_REG_COMMAND] = 7;

    gd54xx->pci_regs[0x30] = 0x00;
    gd54xx->pci_regs[0x32] = 0x0c;
    gd54xx->pci_regs[0x33] = 0x00;

    svga->crtc[0x27] = id;

    svga->seqregs[6] = 0x0f;

    if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5429)
        gd54xx->unlocked = 1;

    if (gd54xx->mca) {
        gd54xx->pos_regs[0] = svga->crtc[0x27] == CIRRUS_ID_CLGD5426 ? 0x82 : 0x7b;
        gd54xx->pos_regs[1] = svga->crtc[0x27] == CIRRUS_ID_CLGD5426 ? 0x81 : 0x91;
        gd5440_mem_mapping_disable(&gd54xx->bios_rom.mapping);
        mca_add(gd5428_mca_read, gd5428_mca_write, gd5428_mca_feedb, NULL, gd54xx);
        gd5440_io_sethandler(0x46e8, 0x0001, gd5440_core_in, NULL, NULL, gd5440_core_out, NULL, NULL, gd54xx);
    }

    if (gd5440_core_is_5434(svga)) {
        gd54xx->i2c = gd5440_i2c_gpio_init("gd5440_ddc_cl54xx");
        gd54xx->ddc = gd5440_ddc_init(gd5440_i2c_gpio_get_bus(gd54xx->i2c));
    }

    if (svga->crtc[0x27] >= CIRRUS_ID_CLGD5446)
        gd54xx->crtcreg_mask = 0x7f;
    else
        gd54xx->crtcreg_mask = 0x3f;

    gd54xx->overlay.colorkeycompare = 0xff;

    svga->local = gd54xx;

    return gd54xx;
}

static int
gd5401_available(void)
{
    return gd5440_rom_present(BIOS_GD5401_PATH);
}

static int
gd5402_available(void)
{
    return gd5440_rom_present(BIOS_GD5402_PATH);
}

static int
gd5420_available(void)
{
    return gd5440_rom_present(BIOS_GD5420_PATH);
}

static int
gd5422_available(void)
{
    return gd5440_rom_present(BIOS_GD5422_PATH);
}

static int
gd5426_diamond_a1_available(void)
{
    return gd5440_rom_present(BIOS_GD5426_DIAMOND_A1_ISA_PATH);
}

static int
gd5428_available(void)
{
    return gd5440_rom_present(BIOS_GD5428_PATH);
}

static int
gd5428_diamond_b1_available(void)
{
    return gd5440_rom_present(BIOS_GD5428_DIAMOND_B1_VLB_PATH);
}

static int
gd5428_boca_isa_available(void)
{
    return gd5440_rom_present(BIOS_GD5428_BOCA_ISA_PATH_1) && gd5440_rom_present(BIOS_GD5428_BOCA_ISA_PATH_2);
}

static int
gd5428_isa_available(void)
{
    return gd5440_rom_present(BIOS_GD5428_ISA_PATH);
}

static int
gd5426_mca_available(void)
{
    return gd5440_rom_present(BIOS_GD5426_MCA_PATH);
}

static int
gd5428_mca_available(void)
{
    return gd5440_rom_present(BIOS_GD5428_MCA_PATH);
}

static int
gd5429_available(void)
{
    return gd5440_rom_present(BIOS_GD5429_PATH);
}

static int
gd5430_diamond_a8_available(void)
{
    return gd5440_rom_present(BIOS_GD5430_DIAMOND_A8_VLB_PATH);
}

static int
gd5430_available(void)
{
    return gd5440_rom_present(BIOS_GD5430_PATH);
}

static int
gd5434_available(void)
{
    return gd5440_rom_present(BIOS_GD5434_PATH);
}

static int
gd5434_isa_available(void)
{
    return gd5440_rom_present(BIOS_GD5434_PATH);
}

static int
gd5430_orchid_vlb_available(void)
{
    return gd5440_rom_present(BIOS_GD5430_ORCHID_VLB_PATH);
}

static int
gd5434_orchid_vlb_available(void)
{
    return gd5440_rom_present(BIOS_GD5434_ORCHID_VLB_PATH);
}

static int
gd5434_diamond_a3_available(void)
{
    return gd5440_rom_present(BIOS_GD5434_DIAMOND_A3_ISA_PATH);
}

void
gd5440_core_close(void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;

    gd5440_svga_close(&gd54xx->svga);

    if (gd54xx->i2c) {
        gd5440_ddc_close(gd54xx->ddc);
        gd5440_i2c_gpio_close(gd54xx->i2c);
    }

    free(gd54xx);
}

void
gd5440_core_speed_changed(void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;

    gd5440_svga_recalctimings(&gd54xx->svga);
}

void
gd5440_core_force_redraw(void *priv)
{
    gd5440_core_t *gd54xx = (gd5440_core_t *) priv;

    gd54xx->svga.fullchange = gd54xx->svga.monitor->mon_changeframecount;
}


static gd5440_core_t *gd5440_state = NULL;
static uint32_t gd5440_draw_timer = TIMING_ERROR;
static volatile int gd5440_render_thread_run = 0;
static volatile int gd5440_render_thread_alive = 0;
static volatile int gd5440_do_render = 0;
static volatile int gd5440_frame_ready = 0;
static int gd5440_pending_monitor = 0;
static int gd5440_pending_x = 0;
static int gd5440_pending_y = 0;
static int gd5440_pending_w = 640;
static int gd5440_pending_h = 480;
static uint64_t gd5440_fixed_to_ticks(uint64_t fixed_value)
{
    uint64_t ticks = fixed_value >> 32;
    if ((fixed_value & 0xffffffffu) != 0)
        ticks++;
    if (ticks == 0)
        ticks = 1;
    return ticks;
}
static uint64_t gd5440_frame_ticks(const gd5440_svga_t *svga)
{
    uint64_t line_fixed;
    uint64_t lines;
    if (svga == NULL)
        return 1;
    line_fixed = svga->dispontime + svga->dispofftime;
    if (line_fixed == 0)
        line_fixed = 1ULL << 32;
    lines = (svga->vtotal > 0) ? (uint64_t) (svga->vtotal + 1) : 1;
    return gd5440_fixed_to_ticks(line_fixed * lines);
}
static double gd5440_frame_rate(const gd5440_svga_t *svga)
{
    uint64_t frame_ticks;
    uint64_t host_freq;
    if (svga == NULL)
        return 0.0;
    frame_ticks = gd5440_frame_ticks(svga);
    if (frame_ticks == 0)
        return 0.0;
    host_freq = compat_get_host_timer_freq();
    if (host_freq == 0)
        return 0.0;
    return (double) host_freq / (double) frame_ticks;
}
static int gd5440_frame_poll_budget(const gd5440_svga_t *svga)
{
    uint64_t lines;
    int polls_per_line;
    if (svga == NULL)
        return 4096;
    lines = (svga->vtotal > 0) ? (uint64_t) (svga->vtotal + 1) : 1;
    if (lines > 4096)
        lines = 4096;
    polls_per_line = (svga->crtc[0x17] & 4) ? 4 : 2;
    return (int) (lines * (uint64_t) polls_per_line) + 8;
}
static void gd5440_request_present(int monitor_index, int x, int y, int w, int h)
{
    gd5440_pending_monitor = monitor_index;
    gd5440_pending_x = x;
    gd5440_pending_y = y;
    gd5440_pending_w = w;
    gd5440_pending_h = h;
    gd5440_frame_ready = 1;
}
static void gd5440_sync_timers(gd5440_core_t *gd54xx)
{
    gd5440_svga_t *svga;
    double frame_rate;
    uint64_t frame_ticks;
    if (gd54xx == NULL)
        return;
    svga = &gd54xx->svga;
    frame_rate = gd5440_frame_rate(svga);
    frame_ticks = gd5440_frame_ticks(svga);
    if (gd5440_draw_timer != TIMING_ERROR) {
        if (frame_rate > 0.0)
            timing_updateIntervalFreq(gd5440_draw_timer, frame_rate);
        else
            timing_updateInterval(gd5440_draw_timer, frame_ticks);
        timing_timerEnable(gd5440_draw_timer);
    }
}
static void gd5440_recalctimings_bridge(gd5440_svga_t *svga)
{
    gd5440_core_recalctimings(svga);
    gd5440_sync_timers((gd5440_core_t *) svga->priv);
}
static void gd5440_drawCallback(void *dummy)
{
    (void) dummy;
    gd5440_do_render = 1;
}
static void gd5440_render_frame(void)
{
    gd5440_svga_t *svga;
    int budget;
    int monitor_index;
    int x;
    int y;
    int w;
    int h;
    if (gd5440_state == NULL)
        return;
    svga = &gd5440_state->svga;
    gd5440_frame_ready = 0;
    /* Run the imported SVGA state machine forward until it reaches the next frame blit point. */
    budget = gd5440_frame_poll_budget(svga);
    while (gd5440_render_thread_run && !gd5440_frame_ready && (budget-- > 0))
        gd5440_svga_poll(svga);
    if (gd5440_frame_ready) {
        monitor_index = gd5440_pending_monitor;
        x = gd5440_pending_x;
        y = gd5440_pending_y;
        w = gd5440_pending_w;
        h = gd5440_pending_h;
        gd5440_present_pending_frame(monitor_index, x, y, w, h);
        gd5440_frame_ready = 0;
    }
}
static void gd5440_renderThread(void *dummy)
{
    (void) dummy;
    host_set_thread_priority(render_priority);
    while (gd5440_render_thread_run) {
        if (gd5440_do_render) {
            gd5440_do_render = 0;
            if (frontend_video_can_accept_present()) {
                gd5440_render_frame();
            }
        } else {
            host_sleep(1);
        }
    }
    gd5440_render_thread_alive = 0;
    host_end_thread();
}
int gd5440_init(PCI_t *pci, uint8_t slot)
{
    device_t info;
    if (pci == NULL)
        return -1;
    if (!gd5440_rom_present(BIOS_GD5440_PATH)) {
        debug_log(DEBUG_ERROR, "[GD5440] Missing required ROM image: %s\r\n", BIOS_GD5440_PATH);
        return -1;
    }
    compat_host_timer_freq = timing_getFreq();
    compat_video_init();
    compat_pci_bus = pci;
    compat_pci_slot = slot;
    compat_dma_bus = NULL;
    GD5440_TIMER_USEC = (uint64_t) ((compat_get_host_timer_freq() << 32) / 1000000u);
    if (GD5440_TIMER_USEC == 0)
        GD5440_TIMER_USEC = 1;
    memset(&info, 0, sizeof(info));
    info.name = "Cirrus Logic GD5440 PCI";
    info.internal_name = "gd5440";
    info.flags = DEVICE_PCI;
    info.local = CIRRUS_ID_CLGD5440 | 0x400;
    gd5440_state = (gd5440_core_t *) gd5440_core_init(&info);
    if (gd5440_state == NULL)
        return -1;
    gd5440_draw_timer = timing_addTimer(gd5440_drawCallback, NULL, 60.0, TIMING_DISABLED);
    gd5440_render_thread_run = 1;
    gd5440_render_thread_alive = 1;
    gd5440_do_render = 0;
    gd5440_frame_ready = 0;
    if (host_create_thread(gd5440_renderThread, NULL) != 0) {
        gd5440_render_thread_run = 0;
        gd5440_render_thread_alive = 0;
        gd5440_core_close(gd5440_state);
        gd5440_state = NULL;
        return -1;
    }
    gd5440_timer_disable(&gd5440_state->svga.timer);
    gd5440_state->svga.timer.host_timer_id = TIMING_ERROR;
    gd5440_state->svga.recalctimings_ex = gd5440_recalctimings_bridge;
    gd5440_svga_recalctimings(&gd5440_state->svga);
    return 0;
}
void gd5440_reset(void)
{
    if (gd5440_state == NULL)
        return;
    gd5440_core_reset(gd5440_state);
    gd5440_timer_disable(&gd5440_state->svga.timer);
    gd5440_state->svga.timer.host_timer_id = TIMING_ERROR;
    gd5440_state->svga.recalctimings_ex = gd5440_recalctimings_bridge;
    gd5440_svga_recalctimings(&gd5440_state->svga);
    gd5440_do_render = 0;
    gd5440_frame_ready = 0;
}
void gd5440_close(void)
{
    if (gd5440_draw_timer != TIMING_ERROR)
        timing_timerDisable(gd5440_draw_timer);
    if (gd5440_render_thread_alive) {
        gd5440_render_thread_run = 0;
        while (gd5440_render_thread_alive) {
            host_sleep(1);
        }
    }
    if (gd5440_state != NULL) {
        gd5440_core_close(gd5440_state);
        gd5440_state = NULL;
    }
}
