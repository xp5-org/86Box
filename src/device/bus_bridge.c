/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          External bridge core: shared memory transport, lockstep clock,
 *          sync timer and companion programs. Bus-neutral, see
 *          bus_bridge.h
 *
 *          POSIX only (mmap). Windows & MacOS not implemented or tested
 *
 * Authors: xp5-org
 *
 *          Copyright 2026
 *
 */
 
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#    include <fcntl.h>
#    include <sched.h>
#    include <signal.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <time.h>
#    include <unistd.h>
#else
#    include <windows.h>
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/config.h>
#include <86box/ini.h>
#include <86box/bus_bridge.h>

#define BB_TIMEOUT_MS 3000
#define BB_RETRY_US   250000.0

static void
bb_disconnect(bus_bridge_t *br)
{
    if (br->ops->unmap != NULL)
        br->ops->unmap(br);
#ifndef _WIN32
    if (br->shm != NULL)
        munmap((void *) br->shm, ISAB_CTRL_BYTES);
    if (br->fd >= 0)
        close(br->fd);
    br->fd = -1;
#endif
    br->shm = NULL;
}

static int
bb_connect(bus_bridge_t *br)
{
#ifdef _WIN32
    (void) br;
    return 0;
#else
    struct stat st;
    void       *m;

    if (br->shm != NULL)
        return 1;

    br->fd = open(br->shm_path, O_RDWR);
    if (br->fd < 0)
        return 0;
    if ((fstat(br->fd, &st) != 0) || (st.st_size < ISAB_CTRL_BYTES)) {
        close(br->fd);
        br->fd = -1;
        return 0;
    }
    m = mmap(NULL, ISAB_CTRL_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, br->fd, 0);
    if (m == MAP_FAILED) {
        close(br->fd);
        br->fd = -1;
        return 0;
    }
    br->shm = (volatile isab_shm_t *) m;
    if (__atomic_load_n(&br->shm->magic, __ATOMIC_ACQUIRE) != ISAB_MAGIC) {
        bb_disconnect(br);
        return 0;
    }
    if (br->shm->version != ISAB_VERSION) {
        if (!br->warned)
            pclog("%s bridge #%i: %s speaks protocol v%u, need v%u\n", br->ops->name, br->inst,
                  br->shm_path, br->shm->version, ISAB_VERSION);
        br->warned = 1;
        bb_disconnect(br);
        return 0;
    }

    memcpy(&br->desc, (const void *) &br->shm->desc, sizeof(isab_desc_t));
    if (br->desc.bus != br->ops->bus) {
        if (!br->warned)
            pclog("%s bridge #%i: %s describes bus %u, not %u\n", br->ops->name, br->inst,
                  br->shm_path, br->desc.bus, br->ops->bus);
        br->warned = 1;
        bb_disconnect(br);
        return 0;
    }
    if (br->desc.n_io > ISAB_MAX_IO)
        br->desc.n_io = ISAB_MAX_IO;
    if (br->desc.n_mem > ISAB_MAX_MEM)
        br->desc.n_mem = ISAB_MAX_MEM;

    /* Resume from wherever the device's sequence counter is, so a
       restarted 86Box can talk to a still-running device. */
    br->seq    = __atomic_load_n(&br->shm->resp_seq, __ATOMIC_ACQUIRE);
    br->warned = 0;
    pclog("%s bridge #%i: connected to %s (device pid %u)\n", br->ops->name, br->inst,
          br->shm_path, br->shm->server_pid);
    br->ops->map(br);
    return 1;
#endif
}

/* Bring the virtual clock up to the current TSC. */
static void
bb_update_vtime(bus_bridge_t *br)
{
    double   cpu_mhz;
    uint64_t now;

#ifdef USE_DYNAREC
    /* Mid-block, the dynarec has not credited this block's cycles yet. */
    if (cpu_use_dynarec)
        update_tsc();
#endif
    now     = tsc;
    cpu_mhz = (double) TIMER_USEC / 4294967296.0;
    /* tsc can be rewritten by the guest (WRMSR); never run time backwards. */
    if ((now > br->last_tsc) && (cpu_mhz > 0.0))
        br->vtime_ps += ((double) (now - br->last_tsc) * 1000000.0) / cpu_mhz;
    br->last_tsc = now;
}

#ifndef _WIN32
static uint64_t
bb_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t) ts.tv_sec * 1000ULL) + (ts.tv_nsec / 1000000);
}
#endif

uint32_t
bus_bridge_xfer(bus_bridge_t *br, uint32_t type, uint32_t region, uint32_t addr,
                uint32_t data, uint32_t width, uint32_t flags)
{
#ifdef _WIN32
    (void) br; (void) type; (void) region; (void) addr; (void) data; (void) width; (void) flags;
    return 0xffffffff;
#else
    volatile isab_shm_t *shm;
    uint64_t             start = 0;
    uint32_t             spins = 0;
    uint32_t             ret;

    bb_update_vtime(br);

    if (!bb_connect(br))
        return 0xffffffff;
    shm = br->shm;

    shm->req_type     = type;
    shm->req_region   = region;
    shm->req_addr     = addr;
    shm->req_data     = data;
    shm->req_width    = width;
    shm->req_flags    = flags;
    shm->req_vtime_ps = (uint64_t) br->vtime_ps;
    __atomic_store_n(&shm->req_seq, ++br->seq, __ATOMIC_RELEASE);

    /* The guest CPU is stalled on this access, exactly as it would be on
       IOCHRDY, so spinning is the right thing to do. Yield after a while
       so a device sharing our core can still run. */
    while (__atomic_load_n(&shm->resp_seq, __ATOMIC_ACQUIRE) != br->seq) {
        if (++spins < 4096)
            continue;
        if (start == 0)
            start = bb_now_ms();
        else if ((bb_now_ms() - start) > BB_TIMEOUT_MS) {
            if (!br->warned)
                pclog("%s bridge #%i: no response from %s in %i ms, "
                      "disconnecting\n", br->ops->name, br->inst, br->shm_path, BB_TIMEOUT_MS);
            br->warned = 1;
            bb_disconnect(br);
            return 0xffffffff;
        }
        sched_yield();
    }

    ret = shm->resp_data;
    if (br->ops->pins != NULL)
        br->ops->pins(br);
    if (!br->in_service && (br->ops->service != NULL)) {
        br->in_service = 1;
        br->ops->service(br);
        br->in_service = 0;
    }
    return ret;
#endif
}

static void
bb_sync_cb(void *priv)
{
    bus_bridge_t *br    = (bus_bridge_t *) priv;
    double        delay = br->sync_us;

    if (br->shm == NULL) {
        /* Not connected: only probe for the device every so often. */
        br->retry_left_us -= br->sync_us;
        if (br->retry_left_us > 0.0) {
            timer_advance_u64(&br->sync_timer, (uint64_t) (br->sync_us * TIMER_USEC));
            return;
        }
        br->retry_left_us = BB_RETRY_US;
    }

    bus_bridge_xfer(br, ISAB_SYNC, 0, 0, 0, 0, 0);

    /* Let the device pull the next SYNC in (e.g. its next sample or
       scanline) rather than waiting out the whole default period. */
    if ((br->shm != NULL) && br->shm->resp_next_ps) {
        double want_us = ((double) br->shm->resp_next_ps - br->vtime_ps) / 1000000.0;

        if (want_us < 1.0)
            want_us = 1.0;
        if (want_us < delay)
            delay = want_us;
    }
    timer_advance_u64(&br->sync_timer, (uint64_t) (delay * TIMER_USEC));
}

int
bus_bridge_open(bus_bridge_t *br, const bus_bridge_ops_t *ops, void *fe, int inst,
                const char *shm_path, double sync_us)
{
    memset(br, 0, sizeof(*br));
#ifndef _WIN32
    br->fd = -1;
#endif
    br->ops     = ops;
    br->fe      = fe;
    br->inst    = inst;
    br->sync_us = (sync_us < 1.0) ? 1.0 : sync_us;
    snprintf(br->shm_path, sizeof(br->shm_path), "%s", shm_path);
    br->last_tsc = tsc;

    timer_add(&br->sync_timer, bb_sync_cb, br, 0);
    timer_set_delay_u64(&br->sync_timer, (uint64_t) (br->sync_us * TIMER_USEC));
    return bb_connect(br);
}

void
bus_bridge_close(bus_bridge_t *br)
{
    bb_disconnect(br);
}

/* ---- Companion programs (RTL server, viewer) ----------------------- */

/* Plain shell command lines from the slot's config section; 86Box only
   starts them, they talk to each other through the shm file. */
static const char *const bb_prog_key[BUS_PROG_COUNT]  = { "server_cmd", "monitor_cmd" };
static const char *const bb_prog_name[BUS_PROG_COUNT] = { "server", "monitor" };

#ifndef _WIN32
static pid_t bb_prog_pid[BUS_PROG_COUNT][BUS_BRIDGE_SLOTS];
#else
static HANDLE bb_prog_proc[BUS_PROG_COUNT][BUS_BRIDGE_SLOTS];
#endif

int
bus_bridge_launch(int slot, const char *sec, int prog)
{
    const char *cmd;

    if ((slot < 0) || (slot >= BUS_BRIDGE_SLOTS) || (prog < 0) || (prog >= BUS_PROG_COUNT))
        return 0;
    cmd = config_get_string((char *) sec, (char *) bb_prog_key[prog], "");
    if ((cmd == NULL) || (*cmd == '\0'))
        return 0;

#ifndef _WIN32
    if (bb_prog_pid[prog][slot] > 0) {
        if (waitpid(bb_prog_pid[prog][slot], NULL, WNOHANG) == 0)
            return 0; /* still running */
        bb_prog_pid[prog][slot] = 0;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
        _exit(127);
    }
    if (pid < 0)
        return 0;
    bb_prog_pid[prog][slot] = pid;
    pclog("bridge slot %i: %s started (pid %i): %s\n", slot, bb_prog_name[prog], (int) pid, cmd);
    return 1;
#else
    if (bb_prog_proc[prog][slot] != NULL) {
        if (WaitForSingleObject(bb_prog_proc[prog][slot], 0) == WAIT_TIMEOUT)
            return 0;
        CloseHandle(bb_prog_proc[prog][slot]);
        bb_prog_proc[prog][slot] = NULL;
    }
    char                line[4096];
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;

    snprintf(line, sizeof(line), "cmd.exe /C %s", cmd);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    bb_prog_proc[prog][slot] = pi.hProcess;
    pclog("bridge slot %i: %s started: %s\n", slot, bb_prog_name[prog], cmd);
    return 1;
#endif
}
