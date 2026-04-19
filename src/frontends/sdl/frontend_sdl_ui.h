#ifndef _FRONTEND_SDL_UI_H_
#define _FRONTEND_SDL_UI_H_

#ifdef _WIN32
#include <SDL/SDL.h>
#else
#include <SDL.h>
#endif
#include "../../machine.h"

int frontend_sdl_ui_init(MACHINE_t* machine);
void frontend_sdl_ui_shutdown(void);
void frontend_sdl_ui_beginInput(void);
void frontend_sdl_ui_endInput(void);
int frontend_sdl_ui_handleEvent(SDL_Event* event);
void frontend_sdl_ui_update(void);
void frontend_sdl_ui_toggle(void);
int frontend_sdl_ui_isAvailable(void);
int frontend_sdl_ui_isVisible(void);

#endif
