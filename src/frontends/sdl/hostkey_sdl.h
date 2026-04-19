#ifndef HOSTKEY_SDL_H
#define HOSTKEY_SDL_H

#include "../../input/hostkey.h"

#ifdef _WIN32
#include <SDL/SDL.h>
#else
#include <SDL.h>
#endif

#define HOSTKEY_SDL_MAX_EVENTS 1

uint8_t hostkey_sdl_translate(const SDL_KeyboardEvent* event, HOST_KEY_EVENT_t out[HOSTKEY_SDL_MAX_EVENTS]);

#endif
