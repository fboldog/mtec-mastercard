/* MCCpuBench - measure real background-task progress during disk reads.
 *
 * Unlike scheduler-hook CPU meters, this runs calibrated register-only work
 * in a low-priority task. Hardware interrupt time stops that work, even when
 * Exec still considers the worker task scheduled. The loaded rate divided by
 * the idle rate is therefore useful application CPU availability.
 */
__asm("  .text\n  jmp _start\n");

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <devices/trackdisk.h>
#include <devices/timer.h>

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Device *TimerBase;

#include "gen/cpustubs.h"

LONG start(void);

static const char vers[] __attribute__((used)) =
    "$VER: MCCpuBench 0.1 (9.11.2026)";

#define WORK_STACK       4096
#define DEFAULT_SECONDS  3
#define DEFAULT_READ     (512UL * 1024)
#define READ_WINDOW      (32UL * 1024 * 1024)
#define PASSES           3

struct Worker {
    struct Task task;
    struct Task *parent;
    LONG sigbit;
    ULONG sigmask;
    volatile ULONG stop;
    volatile ULONG count;
    volatile ULONG checksum;
    UBYTE stack[WORK_STACK];
};

static BPTR logfh;
static ULONG efreq;

__attribute__((used)) void *memset(void *d, int c, unsigned long n)
{
    UBYTE *p = d;
    while (n--) *p++ = (UBYTE)c;
    return d;
}

static void out(const char *fmt, LONG *args)
{
    VPrintf((STRPTR)fmt, args);
    if (logfh) {
        VFPrintf(logfh, (STRPTR)fmt, args);
        Flush(logfh);
    }
}

#define P(fmt, ...) do { ULONG a_[] = { 0, ##__VA_ARGS__ }; out(fmt, (LONG *)(a_ + 1)); } while (0)

static void new_list(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = 0;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static ULONG now(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ev.ev_lo;
}

/* Each unit is one dependency-chained pair of register operations. The count
   is published every 64 units so interrupt time cannot be mistaken for task
   progress merely because Exec did not switch tasks. */
static void worker_main(void)
{
    struct Task *me = (struct Task *)FindTask(0);
    struct Worker *w = (struct Worker *)me->tc_UserData;
    register ULONG x __asm("d1") = 0x13579BDFUL;
    ULONG n = 0;

    Signal(w->parent, w->sigmask);
    while (!w->stop) {
        __asm volatile(
            "moveq #63,d0\n"
            "1: add.l %0,%0\n"
            "addq.l #1,%0\n"
            "dbra d0,1b"
            : "+d"(x) : : "d0", "cc");
        n += 64;
        w->count = n;
    }
    w->checksum = x;
    Signal(w->parent, w->sigmask);
    Wait(0);
}

static struct Worker *start_worker(void)
{
    struct Worker *w = (struct Worker *)AllocMem(sizeof *w, MEMF_PUBLIC | MEMF_CLEAR);
    struct Task *t;

    if (!w)
        return 0;
    w->sigbit = AllocSignal(-1);
    if (w->sigbit < 0) {
        FreeMem(w, sizeof *w);
        return 0;
    }
    w->sigmask = 1UL << w->sigbit;
    w->parent = (struct Task *)FindTask(0);
    t = &w->task;
    t->tc_Node.ln_Type = NT_TASK;
    t->tc_Node.ln_Pri = -10;
    t->tc_Node.ln_Name = (char *)"MCCpuBench worker";
    t->tc_SPLower = w->stack;
    t->tc_SPUpper = w->stack + WORK_STACK;
    t->tc_SPReg = w->stack + WORK_STACK;
    t->tc_UserData = w;
    new_list(&t->tc_MemEntry);
    SetSignal(0, w->sigmask);
    AddTask(t, (APTR)worker_main, 0);
    Wait(w->sigmask);
    return w;
}

static void stop_worker(struct Worker *w)
{
    if (!w)
        return;
    w->stop = 1;
    Wait(w->sigmask);
    RemTask(&w->task);
    FreeSignal(w->sigbit);
    FreeMem(w, sizeof *w);
}

static ULONG elapsed_ms(ULONG ticks)
{
    ULONG ticks_per_ms = efreq / 1000;
    if (!ticks_per_ms)
        return 0;
    return ticks / ticks_per_ms;
}

static ULONG availability10(ULONG loaded_count, ULONG loaded_ticks,
                            ULONG idle_count, ULONG idle_ticks)
{
    ULONG lm = elapsed_ms(loaded_ticks), im = elapsed_ms(idle_ticks);
    ULONG lr, ir;
    if (!lm || !im)
        return 0;
    lr = loaded_count / lm;
    ir = idle_count / im;
    if (!ir)
        return 0;
    return (lr * 1000UL + ir / 2) / ir;
}

static LONG read_pass(struct IOStdReq *io, struct Worker *w, UBYTE *buf,
                      ULONG readsize, ULONG goal_ticks, ULONG *loaded_count,
                      ULONG *loaded_ticks, ULONG *rate)
{
    ULONG c0 = w->count, t0 = now(), elapsed = 0;
    ULONG offset = 0, commands = 0, ms;
    LONG err;

    do {
        io->io_Command = CMD_READ;
        io->io_Data = buf;
        io->io_Length = readsize;
        io->io_Offset = offset;
        io->io_Actual = 0;
        io->io_Error = 0;
        err = (LONG)DoIO(io);
        if (err || io->io_Error || io->io_Actual != readsize) {
            P("Read failed: DoIO %ld io_Error %ld offset %lu actual %lu\n",
              err, (LONG)io->io_Error, offset, io->io_Actual);
            return RETURN_FAIL;
        }
        commands++;
        offset += readsize;
        if (offset + readsize > READ_WINDOW)
            offset = 0;
        elapsed = now() - t0;
    } while (elapsed < goal_ticks);

    ms = elapsed_ms(elapsed);
    if (!ms)
        return RETURN_FAIL;
    *rate = commands * (readsize / 1024) * 1000UL / ms;
    *loaded_count = w->count - c0;
    *loaded_ticks = elapsed;
    P("  %lu commands, %lu ms, %lu KiB/s, worker %lu units\n",
      commands, ms, *rate, *loaded_count);
    return RETURN_OK;
}

LONG start(void)
{
    LONG rc = RETURN_FAIL;
    LONG args[5] = { 0, 0, 0, 0, 0 };
    struct RDArgs *ra = 0;
    const char *device;
    ULONG unit, seconds, readsize;
    struct MsgPort port;
    struct IOStdReq io;
    struct timerequest treq;
    LONG portsig = -1;
    int disk_open = 0, timer_open = 0;
    UBYTE *buf = 0;
    struct Worker *w = 0;
    ULONG idle_c0, idle_count, idle_t0, idle_ticks, idle_ms, idle_rate;
    ULONG sum_avail = 0, sum_rate = 0, k;

    SysBase = *(struct ExecBase **)4;
    DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37);
    if (!DOSBase)
        return rc;
    ra = (struct RDArgs *)ReadArgs(
        (STRPTR)"DEVICE/A,UNIT/N,SECONDS/N,READSIZE/N,LOG/K", args, 0);
    if (!ra) {
        PrintFault(IoErr(), (STRPTR)"MCCpuBench");
        goto done;
    }
    device = (const char *)args[0];
    unit = args[1] ? *(LONG *)args[1] : 1;
    seconds = args[2] ? *(LONG *)args[2] : DEFAULT_SECONDS;
    readsize = args[3] ? (ULONG)*(LONG *)args[3] : DEFAULT_READ;
    if (!seconds || seconds > 30 || readsize < 65536 || readsize > 0x100000 ||
        (readsize & 1023)) {
        P("SECONDS must be 1..30; READSIZE must be 64 KiB..1 MiB and a multiple of 1 KiB\n");
        goto done;
    }
    if (args[4]) {
        logfh = Open((STRPTR)args[4], MODE_NEWFILE);
        if (!logfh) {
            P("Cannot open log %s\n", args[4]);
            goto done;
        }
    }

    memset(&port, 0, sizeof port);
    memset(&io, 0, sizeof io);
    memset(&treq, 0, sizeof treq);
    portsig = AllocSignal(-1);
    if (portsig < 0) {
        P("Cannot allocate I/O signal\n");
        goto done;
    }
    port.mp_Node.ln_Type = NT_MSGPORT;
    port.mp_Flags = PA_SIGNAL;
    port.mp_SigBit = portsig;
    port.mp_SigTask = (struct Task *)FindTask(0);
    new_list(&port.mp_MsgList);
    io.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    io.io_Message.mn_ReplyPort = &port;
    io.io_Message.mn_Length = sizeof io;
    if (OpenDevice((STRPTR)device, unit, (struct IORequest *)&io, 0)) {
        P("Cannot open %s unit %lu\n", (ULONG)device, unit);
        goto done;
    }
    disk_open = 1;
    treq.tr_node.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    treq.tr_node.io_Message.mn_ReplyPort = &port;
    treq.tr_node.io_Message.mn_Length = sizeof treq;
    if (OpenDevice((STRPTR)"timer.device", UNIT_ECLOCK,
                   (struct IORequest *)&treq, 0)) {
        P("Cannot open timer.device\n");
        goto done;
    }
    timer_open = 1;
    TimerBase = treq.tr_node.io_Device;
    {
        struct EClockVal ev;
        efreq = ReadEClock(&ev);
    }
    if (!efreq) {
        P("ReadEClock returned zero frequency\n");
        goto done;
    }
    buf = (UBYTE *)AllocMem(readsize, MEMF_PUBLIC);
    if (!buf) {
        P("Cannot allocate %lu-byte read buffer\n", readsize);
        goto done;
    }
    w = start_worker();
    if (!w) {
        P("Cannot start calibrated worker task\n");
        goto done;
    }

    P("MCCpuBench 0.1: %s unit %lu, %lu KiB reads, worker priority -10\n",
      (ULONG)device, unit, readsize / 1024);
    P("Device version %lu.%lu; buffer %08lx memory flags %08lx; EClock %lu Hz\n",
      (ULONG)io.io_Device->dd_Library.lib_Version,
      (ULONG)io.io_Device->dd_Library.lib_Revision,
      (ULONG)buf, TypeOfMem(buf), efreq);
    P("Idle calibration for nominal %lu seconds...\n", seconds);
    idle_c0 = w->count;
    idle_t0 = now();
    Delay(seconds * 50);
    idle_ticks = now() - idle_t0;
    idle_count = w->count - idle_c0;
    idle_ms = elapsed_ms(idle_ticks);
    idle_rate = idle_ms ? idle_count / idle_ms : 0;
    P("IDLE: %lu ms, worker %lu units, %lu units/ms\n",
      idle_ms, idle_count, idle_rate);
    if (!idle_rate) {
        P("Idle calibration failed\n");
        goto done;
    }

    for (k = 0; k < PASSES; k++) {
        ULONG loaded_count, loaded_ticks, avail, rate;
        P("READ pass %lu:\n", k + 1);
        if (read_pass(&io, w, buf, readsize, idle_ticks, &loaded_count,
                      &loaded_ticks, &rate))
            goto done;
        avail = availability10(loaded_count, loaded_ticks,
                               idle_count, idle_ticks);
        P("  APPLICATION CPU AVAILABLE: %lu.%lu%% (denied %lu.%lu%%)\n",
          avail / 10, avail % 10,
          avail < 1000 ? (1000 - avail) / 10 : 0,
          avail < 1000 ? (1000 - avail) % 10 : 0);
        sum_avail += avail;
        sum_rate += rate;
    }
    P("AVERAGE: %lu KiB/s, application CPU available %lu.%lu%%\n",
      sum_rate / PASSES, (sum_avail / PASSES) / 10,
      (sum_avail / PASSES) % 10);
    P("The availability ratio counts completed worker instructions; interrupt\n"
      "time cannot be classified as idle merely because no task switch occurred.\n");
    rc = RETURN_OK;

done:
    stop_worker(w);
    if (buf)
        FreeMem(buf, readsize);
    if (timer_open)
        CloseDevice((struct IORequest *)&treq);
    if (disk_open)
        CloseDevice((struct IORequest *)&io);
    if (portsig >= 0)
        FreeSignal(portsig);
    if (logfh) {
        Flush(logfh);
        Close(logfh);
    }
    if (ra)
        FreeArgs(ra);
    CloseLibrary((struct Library *)DOSBase);
    return rc;
}
