#!/usr/bin/env python3
import os, pathlib, subprocess, sys, tempfile
from pathlib import Path
root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('NULLARC_SRC', Path(__file__).resolve().parents[4] / 'nullarc-0.4.0-alpha'))
source_path = root / 'src/phxaccel/accel/nvrm-accel.cpp'
if not source_path.is_file():
    sys.exit(f'missing {source_path}; pass the nullarc source root')
source = source_path.read_text()
start = source.index('/*\n * Frame completion off WindowServer\'s thread.')
end = source.index('static SInt64 gCntIopAsync, gCntIopAsyncRefused, gCntIopAsyncDrop;', start)
block = source[start:end]
fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
using SInt64 = long long;
using IOLock = std::mutex;
static uint64_t fake_now;
static int fake_drops, fake_sysctl;
#define SYSCTL_INT(a,b,name,c,d,e,f) static int sysctl_##name
#define NVACCEL_CNT(a,b,c) static_assert(true, "")
#define kMillisecondScale 1
#define THREAD_UNINT 0
#define NVRM_SS_KERNEL_ABI 1
#include "src/phxaccel/nvrm_surfshare_abi.h"
static IOLock alloc_lock;
static IOLock *IOLockAlloc(void) { return &alloc_lock; }
static void IOLockFree(IOLock *) {}
static bool OSCompareAndSwapPtr(void *old, void *val, void *volatile *p) { if (*p != old) return false; *p = val; return true; }
static void IOLockLock(IOLock *l) { l->lock(); }
static void IOLockUnlock(IOLock *l) { l->unlock(); }
static void IOLockWakeup(IOLock *, void *, bool) {}
static void IOLockSleepDeadline(IOLock *, void *, uint64_t deadline, unsigned) { fake_now = deadline; }
static uint64_t mach_absolute_time(void) { return fake_now; }
static void clock_interval_to_deadline(uint32_t ms, uint32_t, uint64_t *out) { *out = fake_now + ms; }
static void absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns) { *ns = t; }
static SInt64 OSAddAtomic64(SInt64 n, SInt64 *v) { return __atomic_fetch_add(v, n, __ATOMIC_RELAXED); }
'''
checks = r'''
static unsigned mask_seq(unsigned v) { return (v << NVRM_SS_PLANE_GPU_SEQ_SHIFT) & NVRM_SS_PLANE_GPU_SEQ_MASK; }
static void clear_table() {
    gSurfPendLock = &alloc_lock;
    for (unsigned i = 0; i < kSurfPendSlots; ++i) gSurfPend[i].id = gSurfPend[i].seq = 0;
    gSurfPendSeq = 0; __atomic_store_n(&gSurfPendSeqNow, 0, __ATOMIC_RELEASE);
    gCntSurfMarks = gCntSurfWaits = gCntSurfTimeouts = 0; gCntSurfMarkDrops = 0; gSurfWaitMaxUs = 0;
    fake_now = 1000;
}
int main() {
    clear_table();
    const unsigned id = 0x1234;
    unsigned a = nvSurfMarkSeq(id, true, 0);
    unsigned b = nvSurfMarkSeq(id, true, 0);
    assert(a && b && a != b && b == nvSurfSeqSnapshot());
    nvSurfMarkSeq(id, false, a);
    assert(nvSurfWaitReadyEx(id, a));                    // later b does not extend this flip
    nvSurfMarkSeq(id, false, b);
    assert(nvSurfWaitReadyEx(id, b));
    assert(gCntSurfMarkDrops == 0);

    clear_table();
    a = nvSurfMarkSeq(id, true, 0);                      // transaction snapshot
    const unsigned snap = nvSurfSeqSnapshot();
    b = nvSurfMarkSeq(id, true, 0);                      // work committed after the flip
    nvSurfMarkSeq(id, false, a);
    assert(nvSurfWaitReadyEx(id, snap));                 // ready because the newer mark is above snap
    assert(nvSurfMarkSeq(id, false, b) == 0);

    clear_table();
    a = nvSurfMarkSeq(id, true, 0);
    const unsigned old_snap = nvSurfSeqSnapshot();
    b = nvSurfMarkSeq(id, true, 0);
    fake_now = 1000;
    assert(!nvSurfWaitReadyEx(id, old_snap));             // times out on the old mark only
    assert(gCntSurfTimeouts == 1);
    bool old_gone = true, new_alive = false;
    for (unsigned i = 0; i < kSurfPendSlots; ++i) {
        if (gSurfPend[i].seq == a) old_gone = false;
        if (gSurfPend[i].seq == b) new_alive = true;
    }
    assert(old_gone && new_alive);                       // timeout must not erase later work
    nvSurfMarkSeq(id, false, b);

    clear_table();
    std::vector<unsigned> tokens;
    for (unsigned i = 0; i < kSurfPendSlots; ++i) tokens.push_back(nvSurfMarkSeq(1000 + i, true, 0));
    assert(tokens.back() != 0);
    assert(nvSurfMarkSeq(9999, true, 0) == NVRM_SS_NO_CACHE);
    assert(gCntSurfMarkDrops == 1);
    for (unsigned i = 0; i < tokens.size(); ++i) nvSurfMarkSeq(1000 + i, false, tokens[i]);

    clear_table();
    gSurfPendSeq = NVRM_SS_PLANE_GPU_SEQ_MASK >> NVRM_SS_PLANE_GPU_SEQ_SHIFT;
    unsigned wrapped = nvSurfMarkSeq(id, true, 0);
    assert(wrapped == 1);
    assert(nvSurfSeqLe(wrapped, 2));
    assert(!nvSurfSeqLe(2, wrapped));
    nvSurfMarkSeq(id, false, wrapped);
    std::printf("PASS: numbered surface marks (future work ignored, timeout scoped, 512 slots, wrap)\n");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='surfmark-') as td:
    p = pathlib.Path(td)
    (p/'test.cpp').write_text(fixture + block + checks)
    # fixture includes the ABI relative to the source root; compile with cwd=root.
    subprocess.run(['clang++','-std=c++17','-O1','-g','-fsanitize=address,undefined','-I'+str(root), str(p/'test.cpp'), '-o', str(p/'test')], cwd=root, check=True)
    subprocess.run([str(p/'test')], check=True)
