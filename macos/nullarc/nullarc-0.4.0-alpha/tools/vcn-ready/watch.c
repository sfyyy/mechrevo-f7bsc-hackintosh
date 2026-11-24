/* LaunchDaemon for the Phoenix system. mesboot publishes NMXAccel after PSP/GFX
 * setup. Observe that publication and full-system wake without polling the GPU. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static io_connect_t powerRoot;
static dispatch_queue_t worker;
static const char helper[] = "/Library/PrivilegedHelperTools/org.nullarc.vcn-ready/ensure.sh";
extern char **environ;

static void checkReady(const char *reason) {
    // Serial checks let a duplicate event see the state produced by the first.
    // Keep the power callback free to acknowledge a new sleep immediately.
    dispatch_async(worker, ^{
        char *args[] = {"/bin/sh", (char *)helper, (char *)reason, NULL};
        pid_t child;
        int status;
        int result = posix_spawn(&child, args[0], NULL, NULL, args, environ);
        if (result) {
            fprintf(stderr, "VCN %s: spawn failed: %s\n", reason, strerror(result));
            return;
        }
        pid_t ended;
        do {
            ended = waitpid(child, &status, 0);
        } while (ended < 0 && errno == EINTR);
        if (ended < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
            fprintf(stderr, "VCN %s: readiness check failed\n", reason);
    });
}

static void serviceMatched(void *context, io_iterator_t matches) {
    io_service_t service;
    while ((service = IOIteratorNext(matches))) {
        IOObjectRelease(service);
        checkReady(context);
    }
}

static void powerChanged(void *context, io_service_t service, natural_t message, void *arg) {
    (void)context;
    (void)service;
    switch (message) {
        case kIOMessageCanSystemSleep:
        case kIOMessageSystemWillSleep:
            IOAllowPowerChange(powerRoot, (intptr_t)arg);
            break;
        case kIOMessageSystemHasPoweredOn:
            checkReady("wake");
            break;
    }
}

int main(void) {
    IONotificationPortRef powerPort = NULL;
    io_object_t notifier;
    io_iterator_t acceleratorMatches, discoveryMatches;
    worker = dispatch_queue_create("org.nullarc.vcn-ready.worker", DISPATCH_QUEUE_SERIAL);
    powerRoot = IORegisterForSystemPower(NULL, &powerPort, powerChanged, &notifier);
    IONotificationPortRef matchPort = IONotificationPortCreate(kIOMainPortDefault);
    if (!powerRoot || !matchPort) {
        fputs("VCN: unable to open IOKit notification ports\n", stderr);
        return 1;
    }
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    CFRunLoopAddSource(loop, IONotificationPortGetRunLoopSource(powerPort), kCFRunLoopDefaultMode);
    CFRunLoopAddSource(loop, IONotificationPortGetRunLoopSource(matchPort), kCFRunLoopDefaultMode);
    kern_return_t result = IOServiceAddMatchingNotification(matchPort, kIOFirstMatchNotification,
            IOServiceMatching("NMXAccel"), serviceMatched, "accelerator", &acceleratorMatches);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "VCN: accelerator match registration failed: 0x%x\n", result);
        return 1;
    }
    // Draining arms the notification, including when NMXAccel already exists.
    serviceMatched("accelerator", acceleratorMatches);
    result = IOServiceAddMatchingNotification(matchPort, kIOFirstMatchNotification,
            IOServiceMatching("PhoenixVALoadProbe"), serviceMatched, "discovery", &discoveryMatches);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "VCN: discovery match registration failed: 0x%x\n", result);
        return 1;
    }
    serviceMatched("discovery", discoveryMatches);
    CFRunLoopRun();
    return 1;
}
