#pragma D option quiet
#pragma D option bufsize=64m
#pragma D option switchrate=50hz
#pragma D option ustackframes=40
sched:::off-cpu /pid == $target/ { self->off = timestamp; }
sched:::on-cpu /pid == $target && self->off && timestamp - self->off > 30000000 && timestamp - self->off < 2000000000/ {
    printf("\n@@BLOCK %d tid %d ms %d\n", walltimestamp / 1000000, tid, (timestamp - self->off) / 1000000);
    ustack(); self->off = 0;
}
sched:::on-cpu /pid == $target/ { self->off = 0; }
profile-199 /pid == $target/ { printf("\n@@PROF %d tid %d\n", walltimestamp / 1000000, tid); ustack(); }
tick-35s { exit(0); }
