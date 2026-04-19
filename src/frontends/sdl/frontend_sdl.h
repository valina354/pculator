#ifndef _FRONTEND_SDL_H_
#define _FRONTEND_SDL_H_

#include "../frontend.h"

void frontend_sdl_show_help(void);
FRONTEND_ARG_RESULT_t frontend_sdl_try_parse_arg(int argc, char* argv[], int* index);
int frontend_sdl_register(void);

#endif
