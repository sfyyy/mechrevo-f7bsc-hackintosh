#pragma D option quiet
#pragma D option bufsize=8m

/* Which waiting primitive the decode path uses, and how long each one blocks.
 * Run around a playback: the decoder service is VTDecoderXPCService, whatever
 * the client is. build 76 onwards: mode 0 must show IOSleep(1) only, mode 2
 * should show IOLockSleepDeadline hits and almost no IOSleep(1). */

dtrace:::BEGIN
{
    printf("ready\n");
}

fbt:mach_kernel:IOLockSleepDeadline:entry
/substr(execname, 0, 9) == "VTDecoder"/
{
    self->short = timestamp;
}

fbt:mach_kernel:IOLockSleepDeadline:return
/self->short/
{
    @short_us = quantize((timestamp - self->short) / 1000);
    self->short = 0;
}

fbt:mach_kernel:IOSleep:entry
/substr(execname, 0, 9) == "VTDecoder" && arg0 == 1/
{
    @iosleep_calls = count();
}

fbt:mach_kernel:is_io_connect_method:entry
/substr(execname, 0, 9) == "VTDecoder" && arg1 == 0x50484432/
{
    self->ioctl = timestamp;
}

fbt:mach_kernel:is_io_connect_method:return
/self->ioctl/
{
    @ioctl_us = quantize((timestamp - self->ioctl) / 1000);
    self->ioctl = 0;
}
