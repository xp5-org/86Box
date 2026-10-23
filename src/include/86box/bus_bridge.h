/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Bus-neutral half of the external bridge: the shared memory
 *          transport, the lockstep virtual clock, the idle sync timer and
 *          the companion programs (RTL server, viewer).
 *
 *          A bus front end (isa_bridge.c today; a PCI or AGP one later)
 *          owns everything the bus itself defines: what the descriptor's
 *          ranges turn into, interrupt and DMA wiring, config keys and the
 *          device_t. It hands the core a bus_bridge_ops_t and calls
 *          bus_bridge_xfer() from its handlers. Nothing in here knows
 *          about ports, IRQ numbers or DMA channels, so adding a bus does
 *          not touch an existing front end.
 */
#ifndef EMU_BUS_BRIDGE_H
#define EMU_BUS_BRIDGE_H

#include <stdint.h>
#include <86box/timer.h>
#include <86box/isa_bridge_proto.h>

/* Slot numbers for the companion program tables. Each front end owns a
   fixed block: ISA takes 1..ISA_BRIDGE_MAX (5), the rest are free. */
#define BUS_BRIDGE_SLOTS 16

/* Companion programs; the config key is "<name>_cmd". */
enum { BUS_PROG_SERVER = 0, BUS_PROG_MONITOR, BUS_PROG_COUNT };

struct bus_bridge_t;

typedef struct bus_bridge_ops_t {
    uint32_t    bus;  /* ISAB_BUS_* this front end implements */
    const char *name; /* "ISA": log prefix, "<name> bridge #n: ..." */

    /* A connection was made; br->desc holds the device's descriptor
       (already checked to be for this bus). Map what it asks for. */
    void (*map)(struct bus_bridge_t *br);
    /* The connection is gone: undo map(), drop IRQ and DMA request lines. */
    void (*unmap)(struct bus_bridge_t *br);
    /* A response just arrived: apply its resp_* pin state. */
    void (*pins)(struct bus_bridge_t *br);
    /* After a response, outside any other service call: do work the
       response asked for (DMA). May call bus_bridge_xfer(). Optional. */
    void (*service)(struct bus_bridge_t *br);
} bus_bridge_ops_t;

typedef struct bus_bridge_t {
    const bus_bridge_ops_t *ops;
    void                   *fe; /* the front end's own struct */
    int                     inst;
    double                  sync_us;
    char                    shm_path[512];
    isab_desc_t             desc;

    pc_timer_t sync_timer;
#ifndef _WIN32
    int fd;
#endif
    volatile isab_shm_t *shm; /* NULL while not connected */
    uint32_t             seq;
    uint64_t             last_tsc;
    double               vtime_ps;
    double               retry_left_us;
    int                  warned;
    int                  in_service;
} bus_bridge_t;

/* Start the sync timer and try to connect. Returns 1 if connected now. */
extern int  bus_bridge_open(bus_bridge_t *br, const bus_bridge_ops_t *ops, void *fe, int inst,
                            const char *shm_path, double sync_us);
extern void bus_bridge_close(bus_bridge_t *br);

/* One synchronous request/response; resp_data, or all-ones with nobody
   on the other end. Connects (and so calls ops->map) on demand. */
extern uint32_t bus_bridge_xfer(bus_bridge_t *br, uint32_t type, uint32_t region, uint32_t addr,
                                uint32_t data, uint32_t width, uint32_t flags);

/* Run the program stored under `<prog>_cmd` in config section `sec`
   unless an earlier launch for `slot` is still running. 1 if started. */
extern int bus_bridge_launch(int slot, const char *sec, int prog);

#endif /*EMU_BUS_BRIDGE_H*/
