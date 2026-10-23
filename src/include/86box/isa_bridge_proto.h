/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Wire protocol (v2) for the external ISA bridge (isa_bridge.c).
 *
 *          Shared verbatim by 86Box and whatever sits on the other end
 *          (a Verilator testbench, or a plain C behavioural model), so it
 *          is plain C99 with fixed-width fields and no 86Box includes.
 *
 *          LOCKSTEP, NOT WALL CLOCK. Every request carries the guest's
 *          virtual time (picoseconds since the bridge connected, derived
 *          from 86Box's TSC). The device must advance its own model to
 *          exactly that time, and never past it, before applying the
 *          request. Guest-visible timing (DRQ/IRQ edges, status bits) is
 *          then independent of host load and pacing jitter.
 *
 *          TRANSPORT. One mmap'd file (normally under /dev/shm). One
 *          request is outstanding at a time: 86Box fills req_*, publishes
 *          by bumping req_seq, and spins until resp_seq == req_seq. The
 *          device only ever answers the current req_seq.
 *
 *          SELF-DESCRIBING. The device fills in `desc` before it sets
 *          `magic`: which I/O ranges and memory windows it wants, and how
 *          many IRQ and DMA lines it uses. 86Box maps exactly that. A
 *          range is either relocatable (offset is relative to the base
 *          the user picked under Configure) or ISAB_RANGE_FIXED (offset is
 *          the absolute ISA address, e.g. the VGA ports at 3C0h). For
 *          every IOR/IOW/MEMR/MEMW, req_region is the index of the range
 *          that was hit and req_addr the offset inside that range.
 */
#ifndef EMU_ISA_BRIDGE_PROTO_H
#define EMU_ISA_BRIDGE_PROTO_H

#include <stdint.h>

#define ISAB_MAGIC   0x42415349u /* "ISAB" little-endian */
#define ISAB_VERSION 2u

/* Request types (req_type). */
enum {
    ISAB_NOP     = 0,
    ISAB_SYNC    = 1, /* Advance to req_vtime_ps; report pins. */
    ISAB_IOR     = 2, /* I/O read in io[req_region]. */
    ISAB_IOW     = 3, /* I/O write. */
    ISAB_MEMR    = 4, /* Memory read in mem[req_region]. */
    ISAB_MEMW    = 5, /* Memory write. */
    ISAB_DACK_RD = 6, /* DMA memory->device: req_data = byte/word from
                         memory, req_flags bit0 = terminal count. */
    ISAB_DACK_WR = 7, /* DMA device->memory: device answers resp_data. */
    ISAB_RESET   = 8  /* ISA RESET DRV. */
};

/* DMA request lines reported in resp_drq. Bit n = the card's nth DMA
   channel as configured on the 86Box side (0 = 8-bit, 1 = 16-bit). */
#define ISAB_DRQ_8   0x01u
#define ISAB_DRQ_16  0x02u

/* req_flags */
#define ISAB_FLAG_TC 0x01u

/* Access widths an I/O range accepts (bit mask, in bytes). */
#define ISAB_W1 0x1u
#define ISAB_W2 0x2u
#define ISAB_W4 0x4u

/* Range flags. */
#define ISAB_RANGE_FIXED 0x1u /* offset is an absolute ISA address */

/* Bus the device is a card for (isab_desc_t.bus). The front end for that
   bus decides what the ranges below mean; 86Box refuses a descriptor for
   a bus it has no front end for. Only ISA exists so far. */
#define ISAB_BUS_ISA 0u
#define ISAB_BUS_PCI 1u /* reserved */
#define ISAB_BUS_AGP 2u /* reserved */

/* Memory kinds. */
#define ISAB_MEM_WINDOW 0u /* every access is forwarded as MEMR/MEMW */

#define ISAB_MAX_IO  8
#define ISAB_MAX_MEM 8

typedef struct isab_io_t {
    uint32_t offset; /* from the user's I/O base, or absolute if FIXED */
    uint32_t length; /* ports */
    uint32_t widths; /* ISAB_W* */
    uint32_t flags;  /* ISAB_RANGE_* */
} isab_io_t;

typedef struct isab_mem_t {
    uint32_t kind;   /* ISAB_MEM_* */
    uint32_t offset; /* from the user's memory base, or absolute if FIXED */
    uint32_t length; /* bytes */
    uint32_t flags;  /* ISAB_RANGE_* */
} isab_mem_t;

typedef struct isab_desc_t {
    uint32_t   n_io;
    uint32_t   n_mem;
    uint32_t   n_irq;      /* 0 or 1: the card has an IRQ line */
    uint32_t   n_dma;      /* 0..2: bit0 = 8-bit channel, bit1 = 16-bit */
    uint32_t   caps;       /* reserved, 0 */
    uint32_t   file_bytes; /* size of the whole file */
    uint32_t   bus;        /* ISAB_BUS_*; 0 (ISA) in every older server */
    uint32_t   reserved;
    isab_io_t  io[ISAB_MAX_IO];
    isab_mem_t mem[ISAB_MAX_MEM];
} isab_desc_t;

typedef struct isab_shm_t {
    /* Written once by the device when it creates the file. `magic` last. */
    uint32_t magic;
    uint32_t version;
    uint32_t server_pid;
    uint32_t reserved0;

    /* Request, written by 86Box. req_seq last. */
    uint32_t req_seq;
    uint32_t req_type;
    uint32_t req_addr;   /* offset inside the hit range */
    uint32_t req_data;
    uint32_t req_width;  /* 1, 2 or 4 bytes */
    uint32_t req_flags;
    uint32_t req_region; /* index into desc.io[] / desc.mem[] */
    uint32_t reserved1;
    uint64_t req_vtime_ps;

    /* Response, written by the device. resp_seq last. */
    uint32_t resp_seq;
    uint32_t resp_data;
    uint32_t resp_irq;     /* bit0 = the card's IRQ line level */
    uint32_t resp_drq;     /* ISAB_DRQ_* */
    uint32_t resp_drq_dir; /* same bits: 1 = device->memory (DACK_WR) */
    uint32_t reserved2;
    uint64_t resp_vtime_ps; /* time the model has reached */
    uint64_t resp_next_ps;  /* 0, or when the device wants a SYNC next */

    isab_desc_t desc;
} isab_shm_t;

#define ISAB_CTRL_BYTES 4096

#endif /*EMU_ISA_BRIDGE_PROTO_H*/
