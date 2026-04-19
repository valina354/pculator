#ifndef _FRONTEND_VNC_H_
#define _FRONTEND_VNC_H_

#include <stdint.h>
#include "../frontend.h"

int frontend_vnc_set_bind_address(const char* addr);
int frontend_vnc_set_port(uint16_t port);
int frontend_vnc_set_password(const char* password);
void frontend_vnc_set_wait_for_client(int wait_for_client);
void frontend_vnc_show_help(void);
FRONTEND_ARG_RESULT_t frontend_vnc_try_parse_arg(int argc, char* argv[], int* index);
int frontend_vnc_register(void);

#endif
