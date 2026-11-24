#ifndef PHOENIX_AVD_ACCEL_H
#define PHOENIX_AVD_ACCEL_H

#ifdef __cplusplus
extern "C" {
#endif
int PhoenixAVDCreate(void **accelerator, const void *settings, void *capabilities);
int PhoenixAVDDestroy(void *accelerator);
#ifdef __cplusplus
}
#endif
#endif
