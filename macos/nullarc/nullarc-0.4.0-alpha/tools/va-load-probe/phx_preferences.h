#ifndef PHX_PREFERENCES_H
#define PHX_PREFERENCES_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stdlib.h>

/* Run-time switches of the video plugin. The decoder runs inside a system XPC
 * service, so the environment of the client process does not reach it: the
 * environment variable is only a convenience for local runs, the preference
 * file /Library/Preferences/com.phoenix.va.plist is the supported switch. */
static inline bool phxPreferenceBool(const char *environmentName, CFStringRef preferenceName, bool fallback) {
    const char *environment = environmentName ? getenv(environmentName) : NULL;
    if (environment) return !(environment[0] == '0' && environment[1] == '\0');
    bool enabled = fallback;
    CFTypeRef value = CFPreferencesCopyValue(preferenceName, CFSTR("com.phoenix.va"),
                                             kCFPreferencesAnyUser, kCFPreferencesAnyHost);
    if (value) {
        if (CFGetTypeID(value) == CFBooleanGetTypeID())
            enabled = CFBooleanGetValue((CFBooleanRef)value);
        else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
            int64_t number = 0;
            if (CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &number)) enabled = number != 0;
        }
        CFRelease(value);
    }
    return enabled;
}

#endif
