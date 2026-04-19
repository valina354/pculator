#ifndef HOSTKEY_VNC_H
#define HOSTKEY_VNC_H

#include <rfb/rfb.h>
#include "../../input/hostkey.h"

#define HOSTKEY_VNC_MAX_EVENTS 1

uint8_t hostkey_vnc_translate(rfbKeySym keysym, uint8_t down, HOST_KEY_EVENT_t out[HOSTKEY_VNC_MAX_EVENTS]);

#endif
