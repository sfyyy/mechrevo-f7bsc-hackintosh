#ifndef PHX_AVC_IO_H
#define PHX_AVC_IO_H
#include "phx_avc.h"

OSStatus PhxAVCSubmit(io_connect_t connection, const uint8_t *nv12, uint8_t *annexB, size_t *bytes);
#endif
