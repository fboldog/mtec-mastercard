/* Reboot - restarts the Amiga. */
__asm("  .text\n  jmp _start\n");

#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

#include "gen/stubs.h"

LONG start(void);

LONG start(void)
{
    SysBase = *(struct ExecBase **)4;
    ColdReboot();
    return RETURN_OK;
}
