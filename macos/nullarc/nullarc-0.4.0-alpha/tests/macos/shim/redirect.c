// DYLD_INSERT_LIBRARIES shim: NVMTL_REDIRECT=/path/to/NVMTLDriver makes this process load that binary in place of the
// installed NVMTLDriver.bundle executable. Lets a test app run an uninstalled plugin build (no sudo, WindowServer untouched).
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <objc/runtime.h>
static void *my_dlopen(const char *path, int mode)
{
    const char *r = getenv("NVMTL_REDIRECT");
    if (path && r && *r && strstr(path, "NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver")) {
        static int said; if (!said++) fprintf(stderr, "redirect: %s -> %s\n", path, r);
        path = r;
    } else if (getenv("NVMTL_REDIRECT_TRACE") && path) fprintf(stderr, "dlopen %s\n", path);
    return dlopen(path, mode);
}
// NSBundle finds a loaded bundle's classes by its executable's path; answer for the redirected image instead.
static const char **my_copy_names(const char *image, unsigned int *n)
{
    const char *r = getenv("NVMTL_REDIRECT");
    if (image && r && *r && strstr(image, "NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver")) {
        char real[1024]; image = realpath(r, real) ? strdup(real) : r;
    }
    return objc_copyClassNamesForImage(image, n);
}
// Metal checks that the device class came from the bundle's executable; report the bundle path for the redirected image.
static const char *my_image_name(Class c)
{
    const char *n = class_getImageName(c), *r = getenv("NVMTL_REDIRECT");
    char real[1024];
    if (n && r && *r && realpath(r, real) && !strcmp(n, real))
        return "/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver";
    return n;
}
__attribute__((used)) static struct { const void *n, *o; } interpose[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)my_dlopen, (const void *)dlopen },
    { (const void *)my_image_name, (const void *)class_getImageName },
    { (const void *)my_copy_names, (const void *)objc_copyClassNamesForImage },
};
