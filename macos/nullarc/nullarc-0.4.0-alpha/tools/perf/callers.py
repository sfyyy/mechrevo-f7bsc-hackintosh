# usage: callers.py FILE.sample FUNC [depth] — inclusive samples of FUNC grouped by caller chain (NVMTL/radv/QuartzCore frames)
import sys,re,collections
lines=open(sys.argv[1]).read().split('\nTotal number in stack')[0].split('\n'); F=sys.argv[2]; D=int(sys.argv[3]) if len(sys.argv)>3 else 4
pat=re.compile(r'^([\s+!:|]*)(\d+)\s+(.*?)\s+\(in ([^)]+)\)')
stack=[];agg=collections.Counter();tot=0
for l in lines:
    m=pat.match(l)
    if not m: continue
    d=len(m.group(1));c=int(m.group(2));f=re.sub(r'\(.*','',m.group(3).split('  ')[0]) if not m.group(3).startswith('-[') else m.group(3).split('  ')[0]
    while stack and stack[-1][0]>=d: stack.pop()
    inside=any(F in s[1] for s in stack)
    stack.append((d,f))
    if F in f and not inside:
        ch=[s[1] for s in stack[:-1] if re.search('nvmtl|NVMTL|radv|CA::|Metal',s[1])][-D:]
        agg[' < '.join(reversed(ch))]+=c; tot+=c
print(F,'total',tot)
for k,c in agg.most_common(15): print(f'{c:6d}  {k[:260]}')
