import re,sys
N=sys.argv[1]
want={'exec':('SysBase','FindTask Forbid Permit AllocMem FreeMem WaitPort GetMsg PutMsg ReplyMsg OpenLibrary CloseLibrary SetFunction AddTask FindPort AddPort Wait Signal AllocSignal FindName DoIO OpenDevice CloseDevice ColdReboot Disable Enable CacheClearU TypeOfMem FreeSignal'),
      'dos':('DOSBase','Open Close Read Write DeleteFile Output IoErr VPrintf VFPrintf PrintFault ReadArgs FreeArgs LockDosList UnLockDosList FindDosEntry DeviceProc Delay')}
# Optional arguments replace the list: lib=Base:Func,Func ...
if len(sys.argv)>2:
    want={}
    for spec in sys.argv[2:]:
        lib,rest=spec.split('=',1); base,names=rest.split(':',1)
        want[lib]=(base,names.replace(',',' '))
out=['/* Generated from the NDK FD files by genstubs.py. */','#include <exec/types.h>']
for lib,(base,names) in want.items():
    bias=0;fns={}
    for l in open(f'{N}/FD/{lib}_lib.fd',encoding='latin1'):
        l=l.strip()
        m=re.match(r'##bias\s+(\d+)',l)
        if m: bias=int(m.group(1));continue
        if l.startswith('##') or l.startswith('*') or not l: continue
        m=re.match(r'(\w+)\((.*?)\)\((.*?)\)',l)
        if m: fns[m.group(1)]=(bias,[a for a in m.group(2).split(',') if a],[r for r in re.split('[,/]',m.group(3)) if r]);bias+=6
    for n in names.split():
        b,args,regs=fns[n]
        assert len(args)==len(regs),n
        ps=', '.join(f'ULONG {a}' for a in args) or 'void'
        body=[f'static inline ULONG {n}_({ps}) {{']
        body.append(f'  register ULONG _d0 __asm("d0"); register ULONG _a6 __asm("a6") = (ULONG){base};')
        cl=['d1','a0','a1','cc','memory']
        for a,r in zip(args,regs):
            if r=='d0': body.append(f'  _d0 = {a};');continue
            body.append(f'  register ULONG _{r} __asm("{r}") = {a};')
        # d0/d1/a0/a1 are scratch in every library call, so an argument passed
        # in one of them must be marked as changed, not as a plain input.
        scratch=[r for r in regs if r in ('d1','a0','a1')]
        inp=[f'"r"(_a6)']+[f'"r"(_{r})' for r in regs if r!='d0' and r not in scratch]
        cl=[c for c in cl if c not in regs]
        outs=['"+r"(_d0)' if 'd0' in regs else '"=r"(_d0)']+[f'"+r"(_{r})' for r in scratch]
        cs=', '.join(f'"{c}"' for c in cl)
        body.append(f'  __asm volatile("jsr a6@(-{b})" : {", ".join(outs)} : {", ".join(inp)} : {cs});')
        body.append('  return _d0;\n}')
        out+=body
        cast=', '.join(f'(ULONG)({a})' for a in 'abcdefgh'[:len(args)])
        margs=', '.join('abcdefgh'[:len(args)])
        out.append(f'#define {n}({margs}) {n}_({cast})')
print('\n'.join(out))
