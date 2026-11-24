#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

static size_t env_count(char **env) {
    size_t n = 0;
    while (env && env[n]) n++;
    return n;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 4) {
            fprintf(stderr, "usage: %s NVMTLDriver KEY EXPECTED_VALUE\n", argv[0]);
            return 2;
        }

        /* Initialize Foundation before taking the snapshot: a bare CLI process
         * otherwise gains __CF_USER_TEXT_ENCODING during dlopen(). */
        (void)[NSProcessInfo processInfo];

        char **saved = environ;
        size_t before = env_count(saved);
        void *plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!plugin) {
            fprintf(stderr, "dlopen: %s\n", dlerror());
            return 3;
        }

        size_t after = env_count(environ);
        int leaked = 0;
        for (size_t i = 0; i < after; i++) {
            if (!strncmp(environ[i], "NVMTL_", 6) ||
                !strncmp(environ[i], "NVRM_", 5)) {
                leaked++;
            }
        }

        const char *(*get)(const char *) =
            (const char *(*)(const char *))dlsym(plugin, "nvmtl_env_get");
        if (!get) {
            fprintf(stderr, "nvmtl_env_get missing: %s\n", dlerror());
            return 4;
        }
        const char *value = get(argv[2]);

        printf("before_ptr=%p before_count=%zu after_ptr=%p after_count=%zu "
               "same=%d driver_env=%d %s=%s\n",
               (void *)saved, before, (void *)environ, after,
               saved == environ, leaked, argv[2],
               value ? value : "(nil)");

        return (saved == environ && before == after && !leaked &&
                value && !strcmp(value, argv[3])) ? 0 : 1;
    }
}
