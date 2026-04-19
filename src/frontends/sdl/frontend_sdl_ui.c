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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "frontend_sdl_ui.h"
#include "../frontend.h"
#include "../../debuglog.h"
#include "../../disk/ata.h"
#include "../../scsi/scsi_cdrom.h"

#define FRONTEND_SDL_UI_FRAME_INTERVAL_MS 33

#ifdef USE_NUKLEAR
#ifdef _WIN32
#define NK_SDL_RENDERER_SDL_H <SDL/SDL.h>
#include <SDL/SDL_syswm.h>
#else
#define NK_SDL_RENDERER_SDL_H <SDL.h>
#include <SDL_syswm.h>
#endif
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_RENDERER_IMPLEMENTATION
#include "../../../third_party/Nuklear/nuklear.h"
#include "../../../third_party/Nuklear/demo/sdl_renderer/nuklear_sdl_renderer.h"
#include "../../../third_party/nativefiledialog-extended/src/include/nfd.h"
#endif

static MACHINE_t* frontend_sdl_ui_machine = NULL;
static uint8_t frontend_sdl_ui_available = 0;
static uint8_t frontend_sdl_ui_visible = 0;
static uint8_t frontend_sdl_ui_inputActive = 0;
static char frontend_sdl_ui_status[256] = "";

#ifdef USE_NUKLEAR
static SDL_Window* frontend_sdl_ui_window = NULL;
static SDL_Renderer* frontend_sdl_ui_renderer = NULL;
static Uint32 frontend_sdl_ui_windowID = 0;
static struct nk_context* frontend_sdl_ui_ctx = NULL;
static uint8_t frontend_sdl_ui_nfdReady = 0;
static uint8_t frontend_sdl_ui_swallowF10Break = 0;
static float frontend_sdl_ui_fontScale = 1.0f;
static uint32_t frontend_sdl_ui_lastFrameTick = 0;
#endif

static void frontend_sdl_ui_setStatus(const char* fmt, ...) {
	va_list args;

	if (fmt == NULL) {
		frontend_sdl_ui_status[0] = 0;
		return;
	}

	va_start(args, fmt);
	vsnprintf(frontend_sdl_ui_status, sizeof(frontend_sdl_ui_status), fmt, args);
	va_end(args);
}

static const char* frontend_sdl_ui_baseName(const char* path) {
	const char* slash;
	const char* backslash;
	const char* last;

	if ((path == NULL) || (*path == 0)) {
		return "(empty)";
	}

	slash = strrchr(path, '/');
	backslash = strrchr(path, '\\');
	last = path;
	if ((slash != NULL) && (slash + 1 > last)) last = slash + 1;
	if ((backslash != NULL) && (backslash + 1 > last)) last = backslash + 1;
	return last;
}

static double frontend_sdl_ui_totalRamMB(void) {
	if (frontend_sdl_ui_machine == NULL) {
		return 0.0;
	}

	return ((1024.0 * 1024.0) + (double)frontend_sdl_ui_machine->extmem) / (1024.0 * 1024.0);
}

static int frontend_sdl_ui_isCdromTargetConfigured(int target) {
	if ((frontend_sdl_ui_machine == NULL) || !frontend_sdl_ui_machine->buslogic_enabled ||
		(target < 0) || (target >= BUSLOGIC_MAX_TARGETS)) {
		return 0;
	}

	return frontend_sdl_ui_machine->scsi_targets[target].present &&
		(frontend_sdl_ui_machine->scsi_targets[target].type == BUSLOGIC_TARGET_CDROM);
}

static int frontend_sdl_ui_isIdeCdromAttached(int select) {
	return ata_get_device_type(select) == ATA_DEVICE_ATAPI_CDROM;
}

static const char* frontend_sdl_ui_ideSlotName(int select) {
	switch (select) {
	case 0:
		return "primary master";
	case 1:
		return "primary slave";
	case 2:
		return "secondary master";
	case 3:
		return "secondary slave";
	default:
		return "unknown";
	}
}

#ifdef USE_NUKLEAR
static Uint32 frontend_sdl_ui_eventWindowID(const SDL_Event* event) {
	switch (event->type) {
	case SDL_WINDOWEVENT:
		return event->window.windowID;
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		return event->key.windowID;
	case SDL_TEXTEDITING:
	case SDL_TEXTINPUT:
		return event->text.windowID;
	case SDL_MOUSEMOTION:
		return event->motion.windowID;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		return event->button.windowID;
	case SDL_MOUSEWHEEL:
		return event->wheel.windowID;
	default:
		return 0;
	}
}

static void frontend_sdl_ui_initNfdPlatform(void) {
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
	SDL_SysWMinfo wmInfo;

	if (frontend_sdl_ui_window == NULL) {
		return;
	}

	SDL_VERSION(&wmInfo.version);
	if (!SDL_GetWindowWMInfo(frontend_sdl_ui_window, &wmInfo)) {
		return;
	}

	if (wmInfo.subsystem == SDL_SYSWM_WAYLAND) {
		NFD_SetWaylandDisplay(wmInfo.info.wl.display);
	}
#endif
}

static void frontend_sdl_ui_fillParentWindow(nfdwindowhandle_t* parentWindow) {
	SDL_SysWMinfo wmInfo;

	memset(parentWindow, 0, sizeof(*parentWindow));
	if (frontend_sdl_ui_window == NULL) {
		return;
	}

	SDL_VERSION(&wmInfo.version);
	if (!SDL_GetWindowWMInfo(frontend_sdl_ui_window, &wmInfo)) {
		return;
	}

	switch (wmInfo.subsystem) {
#if defined(_WIN32)
	case SDL_SYSWM_WINDOWS:
		parentWindow->type = NFD_WINDOW_HANDLE_TYPE_WINDOWS;
		parentWindow->handle = (void*)wmInfo.info.win.window;
		break;
#endif
#if defined(SDL_VIDEO_DRIVER_COCOA)
	case SDL_SYSWM_COCOA:
		parentWindow->type = NFD_WINDOW_HANDLE_TYPE_COCOA;
		parentWindow->handle = (void*)wmInfo.info.cocoa.window;
		break;
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
	case SDL_SYSWM_X11:
		parentWindow->type = NFD_WINDOW_HANDLE_TYPE_X11;
		parentWindow->handle = (void*)wmInfo.info.x11.window;
		break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
	case SDL_SYSWM_WAYLAND:
		parentWindow->type = NFD_WINDOW_HANDLE_TYPE_WAYLAND;
		parentWindow->handle = (void*)wmInfo.info.wl.surface;
		break;
#endif
	default:
		break;
	}
}

static void frontend_sdl_ui_copyDialogError(char* out, size_t outSize, const char* message) {
	if ((out == NULL) || (outSize == 0)) {
		return;
	}

	if ((message == NULL) || (*message == 0)) {
		snprintf(out, outSize, "unknown error");
		return;
	}

	snprintf(out, outSize, "%s", message);
}

static nfdresult_t frontend_sdl_ui_openDialog(nfdu8char_t** path,
	const nfdu8filteritem_t* filters,
	nfdfiltersize_t filterCount,
	char* errorBuf,
	size_t errorBufSize) {
	nfdopendialogu8args_t args;
	nfdwindowhandle_t parentWindow;
	nfdresult_t result;
	char firstError[256];
	char secondError[256];

	if ((errorBuf != NULL) && (errorBufSize > 0)) {
		errorBuf[0] = 0;
	}

	memset(&args, 0, sizeof(args));
	args.filterList = filters;
	args.filterCount = filterCount;
	frontend_sdl_ui_fillParentWindow(&args.parentWindow);
	parentWindow = args.parentWindow;

	result = NFD_OpenDialogU8_With(path, &args);
	if (result != NFD_ERROR) {
		return result;
	}

	frontend_sdl_ui_copyDialogError(firstError, sizeof(firstError), NFD_GetError());

#if defined(__linux__)
	if (parentWindow.type != NFD_WINDOW_HANDLE_TYPE_UNSET) {
		memset(&args.parentWindow, 0, sizeof(args.parentWindow));
		result = NFD_OpenDialogU8_With(path, &args);
		if (result != NFD_ERROR) {
			return result;
		}

		frontend_sdl_ui_copyDialogError(secondError, sizeof(secondError), NFD_GetError());
		if ((errorBuf != NULL) && (errorBufSize > 0)) {
			if (strcmp(firstError, secondError) == 0) {
				snprintf(errorBuf, errorBufSize, "%s", secondError);
			}
			else {
				snprintf(errorBuf, errorBufSize, "%s; retry without parent: %s", firstError, secondError);
			}
		}
		return result;
	}
#endif

	if ((errorBuf != NULL) && (errorBufSize > 0)) {
		snprintf(errorBuf, errorBufSize, "%s", firstError);
	}
	return result;
}

static void frontend_sdl_ui_destroyWindow(void) {
	if (frontend_sdl_ui_inputActive && (frontend_sdl_ui_ctx != NULL)) {
		nk_sdl_handle_grab();
		nk_input_end(frontend_sdl_ui_ctx);
		frontend_sdl_ui_inputActive = 0;
	}
	if (frontend_sdl_ui_ctx != NULL) {
		nk_sdl_shutdown();
		frontend_sdl_ui_ctx = NULL;
	}
	if (frontend_sdl_ui_renderer != NULL) {
		SDL_DestroyRenderer(frontend_sdl_ui_renderer);
		frontend_sdl_ui_renderer = NULL;
	}
	if (frontend_sdl_ui_window != NULL) {
		SDL_DestroyWindow(frontend_sdl_ui_window);
		frontend_sdl_ui_window = NULL;
	}
	frontend_sdl_ui_windowID = 0;
	frontend_sdl_ui_swallowF10Break = 0;
	frontend_sdl_ui_visible = 0;
	frontend_sdl_ui_fontScale = 1.0f;
	frontend_sdl_ui_lastFrameTick = 0;
}

static int frontend_sdl_ui_createWindow(void) {
	if (frontend_sdl_ui_window != NULL) {
		return 0;
	}

	frontend_sdl_ui_window = SDL_CreateWindow("Control Panel",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		400, 480,
		SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI);
	if (frontend_sdl_ui_window == NULL) {
		debug_log(DEBUG_ERROR, "[UI] Failed to create control panel window\r\n");
		return -1;
	}

	frontend_sdl_ui_renderer = SDL_CreateRenderer(frontend_sdl_ui_window, -1, SDL_RENDERER_ACCELERATED);
	if (frontend_sdl_ui_renderer == NULL) {
		debug_log(DEBUG_ERROR, "[UI] Failed to create control panel renderer\r\n");
		frontend_sdl_ui_destroyWindow();
		return -1;
	}

	{
		int render_w, render_h;
		int window_w, window_h;
		float scale_x, scale_y;

		SDL_GetWindowSize(frontend_sdl_ui_window, &window_w, &window_h);
		if (!SDL_GetRendererOutputSize(frontend_sdl_ui_renderer, &render_w, &render_h) &&
			(window_w > 0) && (window_h > 0)) {
			scale_x = (float)render_w / (float)window_w;
			scale_y = (float)render_h / (float)window_h;
			SDL_RenderSetScale(frontend_sdl_ui_renderer, scale_x, scale_y);
			frontend_sdl_ui_fontScale = (scale_y > 0.0f) ? scale_y : 1.0f;
		}
		else {
			frontend_sdl_ui_fontScale = 1.0f;
		}
	}

	frontend_sdl_ui_ctx = nk_sdl_init(frontend_sdl_ui_window, frontend_sdl_ui_renderer);
	if (frontend_sdl_ui_ctx == NULL) {
		debug_log(DEBUG_ERROR, "[UI] Failed to initialize Nuklear\r\n");
		frontend_sdl_ui_destroyWindow();
		return -1;
	}

	{
		struct nk_font_atlas* atlas;
		struct nk_font_config config = nk_font_config(0);
		struct nk_font* font;

		nk_sdl_font_stash_begin(&atlas);
		font = nk_font_atlas_add_default(atlas, 13.0f * frontend_sdl_ui_fontScale, &config);
		nk_sdl_font_stash_end();
		if ((font != NULL) && (frontend_sdl_ui_fontScale > 0.0f)) {
			font->handle.height /= frontend_sdl_ui_fontScale;
			nk_style_set_font(frontend_sdl_ui_ctx, &font->handle);
		}
	}
	frontend_sdl_ui_windowID = SDL_GetWindowID(frontend_sdl_ui_window);

	frontend_sdl_ui_lastFrameTick = 0;
	frontend_sdl_ui_initNfdPlatform();
	return 0;
}

static void frontend_sdl_ui_hideWindow(void) {
	frontend_sdl_ui_destroyWindow();
}

static void frontend_sdl_ui_pickFloppy(uint8_t drive) {
	static const nfdu8filteritem_t filters[] = {
		{ "Floppy images", "img,ima,vfd,flp" },
		{ "All files", "*" }
	};
	nfdu8char_t* path = NULL;
	nfdresult_t result;
	char dialogError[256];

	if ((frontend_sdl_ui_machine == NULL) || !frontend_sdl_ui_nfdReady) {
		frontend_sdl_ui_setStatus("File dialog unavailable");
		return;
	}

	result = frontend_sdl_ui_openDialog(&path, filters, sizeof(filters) / sizeof(filters[0]),
		dialogError, sizeof(dialogError));
	if (result == NFD_OKAY) {
		if (fdc_insert(&frontend_sdl_ui_machine->fdc, drive, path) == 0) {
			frontend_sdl_ui_setStatus("Floppy %u mounted: %s", (unsigned)drive, frontend_sdl_ui_baseName(path));
		}
		else {
			frontend_sdl_ui_setStatus("Failed to mount floppy %u", (unsigned)drive);
		}
		NFD_FreePathU8(path);
	}
	else if (result == NFD_ERROR) {
		frontend_sdl_ui_setStatus("File dialog failed: %s", dialogError);
	}
}

static void frontend_sdl_ui_pickCdrom(uint8_t target) {
	static const nfdu8filteritem_t filters[] = {
		{ "ISO images", "iso" },
		{ "All files", "*" }
	};
	nfdu8char_t* path = NULL;
	nfdresult_t result;
	char dialogError[256];

	if ((frontend_sdl_ui_machine == NULL) || !frontend_sdl_ui_nfdReady) {
		frontend_sdl_ui_setStatus("File dialog unavailable");
		return;
	}
	if (!frontend_sdl_ui_isCdromTargetConfigured(target)) {
		frontend_sdl_ui_setStatus("SCSI CD-ROM target %u is not configured", (unsigned)target);
		return;
	}

	result = frontend_sdl_ui_openDialog(&path, filters, sizeof(filters) / sizeof(filters[0]),
		dialogError, sizeof(dialogError));
	if (result == NFD_OKAY) {
		if (scsi_cdrom_attach(0, target, path) == 0) {
			strncpy(frontend_sdl_ui_machine->scsi_targets[target].path, path, sizeof(frontend_sdl_ui_machine->scsi_targets[target].path) - 1);
			frontend_sdl_ui_machine->scsi_targets[target].path[sizeof(frontend_sdl_ui_machine->scsi_targets[target].path) - 1] = 0;
			frontend_sdl_ui_setStatus("CD-ROM target %u mounted: %s", (unsigned)target, frontend_sdl_ui_baseName(path));
		}
		else {
			frontend_sdl_ui_setStatus("Failed to mount CD-ROM image");
		}
		NFD_FreePathU8(path);
	}
	else if (result == NFD_ERROR) {
		frontend_sdl_ui_setStatus("File dialog failed: %s", dialogError);
	}
}

static void frontend_sdl_ui_pickIdeCdrom(uint8_t select) {
	static const nfdu8filteritem_t filters[] = {
		{ "ISO images", "iso" },
		{ "All files", "*" }
	};
	nfdu8char_t* path = NULL;
	nfdresult_t result;
	char dialogError[256];

	if ((frontend_sdl_ui_machine == NULL) || !frontend_sdl_ui_nfdReady) {
		frontend_sdl_ui_setStatus("File dialog unavailable");
		return;
	}
	if (!frontend_sdl_ui_isIdeCdromAttached(select)) {
		frontend_sdl_ui_setStatus("IDE ATAPI CD-ROM %s is not attached", frontend_sdl_ui_ideSlotName(select));
		return;
	}

	result = frontend_sdl_ui_openDialog(&path, filters, sizeof(filters) / sizeof(filters[0]),
		dialogError, sizeof(dialogError));
	if (result == NFD_OKAY) {
		if (ata_change_cdrom(select, path)) {
			frontend_sdl_ui_setStatus("IDE CD-ROM %s mounted: %s",
				frontend_sdl_ui_ideSlotName(select),
				frontend_sdl_ui_baseName(path));
		}
		else {
			frontend_sdl_ui_setStatus("Failed to mount IDE CD-ROM image");
		}
		NFD_FreePathU8(path);
	}
	else if (result == NFD_ERROR) {
		frontend_sdl_ui_setStatus("File dialog failed: %s", dialogError);
	}
}

static void frontend_sdl_ui_ejectCdrom(uint8_t target) {
	if ((frontend_sdl_ui_machine == NULL) || !frontend_sdl_ui_isCdromTargetConfigured(target)) {
		frontend_sdl_ui_setStatus("SCSI CD-ROM target %u is not configured", (unsigned)target);
		return;
	}

	if (scsi_cdrom_attach(0, target, NULL) == 0) {
		frontend_sdl_ui_machine->scsi_targets[target].path[0] = 0;
		frontend_sdl_ui_setStatus("CD-ROM target %u ejected", (unsigned)target);
	}
	else {
		frontend_sdl_ui_setStatus("Failed to eject CD-ROM target %u", (unsigned)target);
	}
}

static void frontend_sdl_ui_ejectIdeCdrom(uint8_t select) {
	if ((frontend_sdl_ui_machine == NULL) || !frontend_sdl_ui_isIdeCdromAttached(select)) {
		frontend_sdl_ui_setStatus("IDE ATAPI CD-ROM %s is not attached", frontend_sdl_ui_ideSlotName(select));
		return;
	}

	if (ata_eject_cdrom(select)) {
		frontend_sdl_ui_setStatus("IDE CD-ROM %s ejected", frontend_sdl_ui_ideSlotName(select));
	}
	else {
		frontend_sdl_ui_setStatus("Failed to eject IDE CD-ROM %s", frontend_sdl_ui_ideSlotName(select));
	}
}

static void frontend_sdl_ui_drawControls(void) {
	char changeLabel[32], ejectLabel[32];
	int i, hasCdromTargets, hasIdeCdroms;

	if ((frontend_sdl_ui_machine == NULL) || (frontend_sdl_ui_ctx == NULL)) {
		return;
	}

	if (nk_begin(frontend_sdl_ui_ctx, "Control Panel", nk_rect(12, 12, 372, 448),
		NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
		nk_layout_row_dynamic(frontend_sdl_ui_ctx, 18, 1);
		nk_label(frontend_sdl_ui_ctx, "Right Ctrl-F10 hides this window.", NK_TEXT_LEFT);

		if (nk_tree_push(frontend_sdl_ui_ctx, NK_TREE_TAB, "Machine Status", NK_MAXIMIZED)) {
			nk_layout_row_dynamic(frontend_sdl_ui_ctx, 20, 2);
			nk_label(frontend_sdl_ui_ctx, "Machine", NK_TEXT_LEFT);
			nk_label(frontend_sdl_ui_ctx, usemachine, NK_TEXT_LEFT);
			nk_label(frontend_sdl_ui_ctx, "FPS", NK_TEXT_LEFT);
			nk_labelf(frontend_sdl_ui_ctx, NK_TEXT_LEFT, "%.2f", frontend_get_video_fps());
			nk_label(frontend_sdl_ui_ctx, "MIPS", NK_TEXT_LEFT);
			nk_labelf(frontend_sdl_ui_ctx, NK_TEXT_LEFT, "%.2f", currentMIPS);
			nk_label(frontend_sdl_ui_ctx, "RAM", NK_TEXT_LEFT);
			nk_labelf(frontend_sdl_ui_ctx, NK_TEXT_LEFT, "%.0f MB", frontend_sdl_ui_totalRamMB());
			nk_label(frontend_sdl_ui_ctx, "Mouse", NK_TEXT_LEFT);
			nk_label(frontend_sdl_ui_ctx, frontend_is_mouse_grabbed() ? "Grabbed" : "Released", NK_TEXT_LEFT);
			nk_label(frontend_sdl_ui_ctx, "CS:IP", NK_TEXT_LEFT);
			nk_labelf(frontend_sdl_ui_ctx, NK_TEXT_LEFT, "%04X:%08X",
				(uint16_t)frontend_sdl_ui_machine->CPU.savecs,
				frontend_sdl_ui_machine->CPU.saveip);

			nk_layout_row_dynamic(frontend_sdl_ui_ctx, 26, 2);
			if (nk_button_label(frontend_sdl_ui_ctx, "Ctrl-Alt-Del")) {
				frontend_inject_ctrl_alt_del();
			}
			if (nk_button_label(frontend_sdl_ui_ctx, frontend_is_mouse_grabbed() ? "Release Mouse" : "Grab Mouse")) {
				frontend_set_mouse_grab(frontend_is_mouse_grabbed() ? 0 : 1);
			}

			nk_tree_pop(frontend_sdl_ui_ctx);
		}

		if (nk_tree_push(frontend_sdl_ui_ctx, NK_TREE_TAB, "Media", NK_MAXIMIZED)) {
			nk_layout_row_dynamic(frontend_sdl_ui_ctx, 18, 1);
			nk_label(frontend_sdl_ui_ctx, "Floppy Drives", NK_TEXT_LEFT);

			nk_layout_row_dynamic(frontend_sdl_ui_ctx, 20, 1);
			nk_labelf(frontend_sdl_ui_ctx, NK_TEXT_LEFT, "FD0: %s", frontend_sdl_ui_baseName(fdc_get_filename(&frontend_sdl_ui_machine->fdc, 0)));
			nk_layout_row_dynamic(frontend_sdl_ui_ctx, 24, 2);
			if (nk_button_label(frontend_sdl_ui_ctx, "Change FD0")) frontend_sdl_ui_pickFloppy(0);
			if (nk_button_label(frontend_sdl_ui_ctx, "Eject FD0")) {
				fdc_eject(&frontend_sdl_ui_machine->fdc, 0);
				frontend_sdl_ui_setStatus("Floppy 0 ejected");
			}

			nk_layout_row_dynamic(frontend_sdl_ui_ctx, 20, 1);
			nk_labelf(frontend_sdl_ui_ctx, NK_TEXT_LEFT, "FD1: %s", frontend_sdl_ui_baseName(fdc_get_filename(&frontend_sdl_ui_machine->fdc, 1)));
			nk_layout_row_dynamic(frontend_sdl_ui_ctx, 24, 2);
			if (nk_button_label(frontend_sdl_ui_ctx, "Change FD1")) frontend_sdl_ui_pickFloppy(1);
			if (nk_button_label(frontend_sdl_ui_ctx, "Eject FD1")) {
				fdc_eject(&frontend_sdl_ui_machine->fdc, 1);
				frontend_sdl_ui_setStatus("Floppy 1 ejected");
			}

			hasIdeCdroms = 0;
			for (i = 0; i < ATA_TOTAL_DEVICE_COUNT; i++) {
				if (frontend_sdl_ui_isIdeCdromAttached(i)) {
					hasIdeCdroms = 1;
					break;
				}
			}
			if (hasIdeCdroms) {
				nk_layout_row_dynamic(frontend_sdl_ui_ctx, 18, 1);
				nk_label(frontend_sdl_ui_ctx, "IDE ATAPI CD-ROM", NK_TEXT_LEFT);
				for (i = 0; i < ATA_TOTAL_DEVICE_COUNT; i++) {
					const char* path;

					if (!frontend_sdl_ui_isIdeCdromAttached(i)) {
						continue;
					}

					path = ata_get_cdrom_media_path(i);
					nk_layout_row_dynamic(frontend_sdl_ui_ctx, 20, 1);
					nk_labelf(frontend_sdl_ui_ctx, NK_TEXT_LEFT, "CD%d (%s): %s", i,
						frontend_sdl_ui_ideSlotName(i),
						frontend_sdl_ui_baseName(path));
					snprintf(changeLabel, sizeof(changeLabel), "Change CD%d", i);
					snprintf(ejectLabel, sizeof(ejectLabel), "Eject CD%d", i);
					nk_layout_row_dynamic(frontend_sdl_ui_ctx, 24, 2);
					if (nk_button_label(frontend_sdl_ui_ctx, changeLabel)) frontend_sdl_ui_pickIdeCdrom((uint8_t)i);
					if (nk_button_label(frontend_sdl_ui_ctx, ejectLabel)) frontend_sdl_ui_ejectIdeCdrom((uint8_t)i);
				}
			}

			if (frontend_sdl_ui_machine->buslogic_enabled) {
				hasCdromTargets = 0;
				for (i = 0; i < BUSLOGIC_MAX_TARGETS; i++) {
					if (frontend_sdl_ui_isCdromTargetConfigured(i)) {
						hasCdromTargets = 1;
						break;
					}
				}

				nk_layout_row_dynamic(frontend_sdl_ui_ctx, 18, 1);
				nk_label(frontend_sdl_ui_ctx, "SCSI CD-ROM", NK_TEXT_LEFT);
				if (!hasCdromTargets) {
					nk_layout_row_dynamic(frontend_sdl_ui_ctx, 20, 1);
					nk_label(frontend_sdl_ui_ctx, "No configured SCSI CD targets", NK_TEXT_LEFT);
				}
				for (i = 0; i < BUSLOGIC_MAX_TARGETS; i++) {
					if (!frontend_sdl_ui_isCdromTargetConfigured(i)) {
						continue;
					}

					nk_layout_row_dynamic(frontend_sdl_ui_ctx, 20, 1);
					nk_labelf(frontend_sdl_ui_ctx, NK_TEXT_LEFT, "Target %d: %s", i,
						frontend_sdl_ui_baseName(frontend_sdl_ui_machine->scsi_targets[i].path));
					snprintf(changeLabel, sizeof(changeLabel), "Change ID %d", i);
					snprintf(ejectLabel, sizeof(ejectLabel), "Eject ID %d", i);
					nk_layout_row_dynamic(frontend_sdl_ui_ctx, 24, 2);
					if (nk_button_label(frontend_sdl_ui_ctx, changeLabel)) frontend_sdl_ui_pickCdrom((uint8_t)i);
					if (nk_button_label(frontend_sdl_ui_ctx, ejectLabel)) frontend_sdl_ui_ejectCdrom((uint8_t)i);
				}
			}

			nk_tree_pop(frontend_sdl_ui_ctx);
		}

		if (frontend_sdl_ui_status[0] != 0) {
			nk_layout_row_dynamic(frontend_sdl_ui_ctx, 36, 1);
			nk_label_wrap(frontend_sdl_ui_ctx, frontend_sdl_ui_status);
		}
	}
	nk_end(frontend_sdl_ui_ctx);
}
#endif

int frontend_sdl_ui_init(MACHINE_t* machine) {
	frontend_sdl_ui_machine = machine;
	frontend_sdl_ui_available = 0;
	frontend_sdl_ui_visible = 0;
	frontend_sdl_ui_inputActive = 0;
	frontend_sdl_ui_setStatus(NULL);

#ifdef USE_NUKLEAR
	frontend_sdl_ui_window = NULL;
	frontend_sdl_ui_renderer = NULL;
	frontend_sdl_ui_windowID = 0;
	frontend_sdl_ui_ctx = NULL;
	frontend_sdl_ui_swallowF10Break = 0;
	frontend_sdl_ui_fontScale = 1.0f;
	frontend_sdl_ui_lastFrameTick = 0;

	if (NFD_Init() == NFD_OKAY) {
		frontend_sdl_ui_nfdReady = 1;
	}
	else {
		frontend_sdl_ui_nfdReady = 0;
		frontend_sdl_ui_setStatus("%s", NFD_GetError());
	}

	frontend_sdl_ui_available = 1;
#else
	(void)machine;
#endif

	return 0;
}

void frontend_sdl_ui_shutdown(void) {
#ifdef USE_NUKLEAR
	frontend_sdl_ui_destroyWindow();
	if (frontend_sdl_ui_nfdReady) {
		NFD_Quit();
		frontend_sdl_ui_nfdReady = 0;
	}
#endif
	frontend_sdl_ui_available = 0;
	frontend_sdl_ui_visible = 0;
	frontend_sdl_ui_inputActive = 0;
}

void frontend_sdl_ui_beginInput(void) {
#ifdef USE_NUKLEAR
	if (frontend_sdl_ui_visible && (frontend_sdl_ui_ctx != NULL) && !frontend_sdl_ui_inputActive) {
		nk_input_begin(frontend_sdl_ui_ctx);
		frontend_sdl_ui_inputActive = 1;
	}
#endif
}

void frontend_sdl_ui_endInput(void) {
#ifdef USE_NUKLEAR
	if ((frontend_sdl_ui_ctx != NULL) && frontend_sdl_ui_inputActive) {
		nk_sdl_handle_grab();
		nk_input_end(frontend_sdl_ui_ctx);
		frontend_sdl_ui_inputActive = 0;
	}
#endif
}

int frontend_sdl_ui_handleEvent(SDL_Event* event) {
#ifdef USE_NUKLEAR
	Uint32 windowID;

	if ((event == NULL) || (frontend_sdl_ui_window == NULL)) {
		return 0;
	}

	windowID = frontend_sdl_ui_eventWindowID(event);
	if ((windowID == 0) || (windowID != frontend_sdl_ui_windowID)) {
		return 0;
	}

	if ((event->type == SDL_KEYDOWN) && !event->key.repeat &&
		(event->key.keysym.sym == SDLK_F10) && (event->key.keysym.mod & KMOD_RCTRL)) {
		frontend_sdl_ui_swallowF10Break = 1;
		frontend_sdl_ui_toggle();
		return 1;
	}
	if ((event->type == SDL_KEYUP) && frontend_sdl_ui_swallowF10Break &&
		(event->key.keysym.sym == SDLK_F10)) {
		frontend_sdl_ui_swallowF10Break = 0;
		return 1;
	}

	if (event->type == SDL_WINDOWEVENT) {
		if (event->window.event == SDL_WINDOWEVENT_CLOSE) {
			frontend_sdl_ui_hideWindow();
		}
		return 1;
	}

	if (frontend_sdl_ui_visible && (frontend_sdl_ui_ctx != NULL)) {
		nk_sdl_handle_event(event);
	}
	return 1;
#else
	(void)event;
	return 0;
#endif
}

void frontend_sdl_ui_update(void) {
#ifdef USE_NUKLEAR
	uint32_t curTick;

	if (!frontend_sdl_ui_visible || (frontend_sdl_ui_ctx == NULL) || (frontend_sdl_ui_renderer == NULL)) {
		return;
	}
	if (!frontend_sdl_ui_inputActive) {
		nk_input_begin(frontend_sdl_ui_ctx);
		frontend_sdl_ui_inputActive = 1;
	}

	curTick = SDL_GetTicks();
	if ((frontend_sdl_ui_lastFrameTick != 0) &&
		((curTick - frontend_sdl_ui_lastFrameTick) < FRONTEND_SDL_UI_FRAME_INTERVAL_MS)) {
		return;
	}

	nk_sdl_handle_grab();
	nk_input_end(frontend_sdl_ui_ctx);
	frontend_sdl_ui_inputActive = 0;

	frontend_sdl_ui_drawControls();
	SDL_SetRenderDrawColor(frontend_sdl_ui_renderer, 24, 24, 24, 255);
	SDL_RenderClear(frontend_sdl_ui_renderer);
	nk_sdl_render(NK_ANTI_ALIASING_ON);
	SDL_RenderPresent(frontend_sdl_ui_renderer);
	frontend_sdl_ui_lastFrameTick = curTick;

	nk_input_begin(frontend_sdl_ui_ctx);
	frontend_sdl_ui_inputActive = 1;
#endif
}

void frontend_sdl_ui_toggle(void) {
#ifdef USE_NUKLEAR
	if (!frontend_sdl_ui_available) {
		return;
	}

	if (!frontend_sdl_ui_visible) {
		if (frontend_sdl_ui_createWindow() != 0) {
			return;
		}
		frontend_sdl_ui_visible = 1;
		frontend_sdl_ui_lastFrameTick = 0;
		SDL_ShowWindow(frontend_sdl_ui_window);
		SDL_RaiseWindow(frontend_sdl_ui_window);
	}
	else {
		frontend_sdl_ui_hideWindow();
	}
#endif
}

int frontend_sdl_ui_isAvailable(void) {
	return frontend_sdl_ui_available;
}

int frontend_sdl_ui_isVisible(void) {
	return frontend_sdl_ui_visible;
}
