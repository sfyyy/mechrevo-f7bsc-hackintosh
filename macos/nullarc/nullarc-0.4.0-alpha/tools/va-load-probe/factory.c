#include "avd_accel.h"
#include <dlfcn.h>
#include <os/log.h>
#include <stdlib.h>
#include <unistd.h>

__attribute__((visibility("default"), noinline))
int AVD_CreateAVDAccelerator(void **accelerator, const void *settings, void *capabilities)
{
    Dl_info image = {0};
    Dl_info caller = {0};
    dladdr((const void *)&AVD_CreateAVDAccelerator, &image);
    dladdr(__builtin_return_address(0), &caller);
    os_log_t log = os_log_create("org.nullarc.phoenix.va-load-probe", "factory");
    os_log(log, "AVD_CreateAVDAccelerator pid=%d process=%{public}s image=%{public}s caller=%{public}s",
           getpid(), getprogname(), image.dli_fname ? image.dli_fname : "?",
           caller.dli_fname ? caller.dli_fname : "?");
    os_release(log);
    return PhoenixAVDCreate(accelerator, settings, capabilities);
}

__attribute__((visibility("default")))
int AVD_DestroyAVDAccelerator(void *accelerator)
{
    return PhoenixAVDDestroy(accelerator);
}
