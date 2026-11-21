#ifndef PHX_VT_H
#define PHX_VT_H

#include <VideoToolbox/VideoToolbox.h>

#define PhxAVCEncoderID CFSTR("org.nullarc.phoenix.avc.encoder")

#ifdef __cplusplus
extern "C" {
#endif

// Register in this process before creating a compression session with this ID.
OSStatus PhxRegisterAVCEncoder(void);

#ifdef __cplusplus
}
#endif
#endif
