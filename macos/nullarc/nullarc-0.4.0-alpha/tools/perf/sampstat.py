# usage: python3 sampstat.py FILE.sample [N] — inclusive cost per function over non-wait leaf samples; waits by caller
import sys, re, collections
lines=open(sys.argv[1]).read().split('\nTotal number in stack')[0].split('\n')
N=int(sys.argv[2]) if len(sys.argv)>2 else 60
pat=re.compile(r'^([\s+!:|]*)(\d+)\s+(.*?)\s+\(in ([^)]+)\)')
stack=[]; leafs=[]
for l in lines:
    m=pat.match(l)
    if not m: continue
    d=len(m.group(1)); c=int(m.group(2)); f=m.group(3).split('  ')[0]; lib=m.group(4)
    f=re.sub(r'\(.*','',f) if not f.startswith('-[') else f
    while stack and stack[-1][0]>=d: stack.pop()
    if stack: stack[-1][3]-=c
    stack.append([d,c,f+' ['+lib+']',c])
    # record node; self computed later
    leafs.append(stack[-1]); stack[-1].append([s[2] for s in stack])
WAIT={'__workq_kernreturn','mach_msg2_trap','semaphore_timedwait_trap','__psynch_cvwait','semaphore_wait_trap','__ulock_wait2','kevent_id','__psynch_mutexwait','kevent_qos','__select'}
inc=collections.Counter(); waitby=collections.Counter(); busy=0; waits=0
for n in leafs:
    self_=n[3]
    if self_<=0: continue
    path=n[4]; leaf=path[-1].split(' [')[0]
    if leaf in WAIT:
        waits+=self_
        key=next((p for p in reversed(path) if any(k in p for k in ('NVMTL','vulkan','nvmtl','IOKit','QuartzCore','SkyLight'))),path[-2] if len(path)>1 else leaf)
        waitby[leaf+' <- '+key]+=self_; continue
    busy+=self_
    for p in set(path): inc[p]+=self_
print('busy samples',busy,'wait samples',waits)
for f,c in inc.most_common(N): print(f'{c:6d} {100*c/busy:5.1f}%  {f[:150]}')
print('--- waits by nearest interesting caller')
for f,c in waitby.most_common(25): print(f'{c:6d} {f[:160]}')
