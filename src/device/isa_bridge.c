/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          ISA front end of the external bridge: forwards the I/O ranges, memory windows,
 *          one IRQ line and up to two DMA channels to another process (Verilog)
 *          using shared memory file (transport in bus_bridge.c)
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
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/timer.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/rom.h>
#include <86box/dma.h>
#include <86box/pic.h>
#include <86box/device.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/plat_unused.h>
#include <86box/bus_bridge.h>
#include <86box/isa_bridge.h>
#include <86box/isa_bridge_proto.h>

#define ISAB_SECTION_NAME  "External ISA Bridge"
#define ISAB_DMA_BURST_MAX 64

struct isa_bridge_t;

/* What a handler gets as its priv: which bridge, which of its ranges. */
typedef struct isab_range_t {
    struct isa_bridge_t *dev;
    uint32_t             idx;
    uint32_t             base; /* absolute address of the range */
    uint32_t             len;
    int                  active;
    int                  added; /* mem only: mapping struct is in use */
} isab_range_t;

typedef struct isa_bridge_t {
    bus_bridge_t br; /* transport and clock: bus_bridge.c */

    int      inst;
    uint32_t io_base;
    uint32_t mem_base;
    int      irq;
    int      dma8;
    int      dma16;

    isab_range_t  io[ISAB_MAX_IO];
    isab_range_t  mem[ISAB_MAX_MEM];
    mem_mapping_t mapping[ISAB_MAX_MEM];

    rom_t rom;
    int   rom_loaded;

    uint32_t irq_level;
    int      bad_warned;
} isa_bridge_t;

int isa_bridge_enabled[ISA_BRIDGE_MAX];

#ifdef ENABLE_ISA_BRIDGE_LOG
int isa_bridge_do_log = ENABLE_ISA_BRIDGE_LOG;

static void
isa_bridge_log(const char *fmt, ...)
{
    va_list ap;

    if (isa_bridge_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define isa_bridge_log(fmt, ...)
#endif

void
isa_bridge_section(int inst, char *buf, int len)
{
    snprintf(buf, len, ISAB_SECTION_NAME " #%i", inst);
}

static uint32_t
isab_xfer(isa_bridge_t *dev, uint32_t type, uint32_t region, uint32_t addr,
          uint32_t data, uint32_t width, uint32_t flags)
{
    return bus_bridge_xfer(&dev->br, type, region, addr, data, width, flags);
}

static uint8_t
isab_io_readb(uint16_t port, void *priv)
{
    isab_range_t *r = (isab_range_t *) priv;

    return (uint8_t) isab_xfer(r->dev, ISAB_IOR, r->idx, port - r->base, 0, 1, 0);
}

static uint16_t
isab_io_readw(uint16_t port, void *priv)
{
    isab_range_t *r = (isab_range_t *) priv;

    return (uint16_t) isab_xfer(r->dev, ISAB_IOR, r->idx, port - r->base, 0, 2, 0);
}

static void
isab_io_writeb(uint16_t port, uint8_t val, void *priv)
{
    isab_range_t *r = (isab_range_t *) priv;

    isab_xfer(r->dev, ISAB_IOW, r->idx, port - r->base, val, 1, 0);
}

static void
isab_io_writew(uint16_t port, uint16_t val, void *priv)
{
    isab_range_t *r = (isab_range_t *) priv;

    isab_xfer(r->dev, ISAB_IOW, r->idx, port - r->base, val, 2, 0);
}

static uint8_t
isab_mem_read(uint32_t addr, void *priv)
{
    isab_range_t *r = (isab_range_t *) priv;

    return (uint8_t) isab_xfer(r->dev, ISAB_MEMR, r->idx, addr - r->base, 0, 1, 0);
}

static void
isab_mem_write(uint32_t addr, uint8_t val, void *priv)
{
    isab_range_t *r = (isab_range_t *) priv;

    isab_xfer(r->dev, ISAB_MEMW, r->idx, addr - r->base, val, 1, 0);
}

/* Map what the model's descriptor asks for. */
static void
isab_map(bus_bridge_t *br)
{
    isa_bridge_t      *dev = (isa_bridge_t *) br->fe;
    const isab_desc_t *d   = &br->desc;

    for (uint32_t i = 0; i < d->n_io; i++) {
        isab_range_t *r      = &dev->io[i];
        uint32_t      base   = (d->io[i].flags & ISAB_RANGE_FIXED) ? d->io[i].offset
                                                                    : (dev->io_base + d->io[i].offset);
        uint32_t      len    = d->io[i].length;
        int           wide   = !!(d->io[i].widths & ISAB_W2);

        if ((len == 0) || (base > 0xffff) || ((base + len) > 0x10000)) {
            if (!dev->bad_warned)
                pclog("ISA bridge #%i: ignoring I/O range %u (base %X, length %X)\n",
                      dev->inst, i, base, len);
            continue;
        }
        r->dev    = dev;
        r->idx    = i;
        r->base   = base;
        r->len    = len;
        r->active = 1;
        io_sethandler((uint16_t) base, (uint16_t) len,
                      isab_io_readb, wide ? isab_io_readw : NULL, NULL,
                      isab_io_writeb, wide ? isab_io_writew : NULL, NULL, r);
        isa_bridge_log("ISA bridge #%i: io[%u] %04X-%04X\n", dev->inst, i, base, base + len - 1);
    }

    for (uint32_t i = 0; i < d->n_mem; i++) {
        isab_range_t *r    = &dev->mem[i];
        uint32_t      base = (d->mem[i].flags & ISAB_RANGE_FIXED) ? d->mem[i].offset
                                                                   : (dev->mem_base + d->mem[i].offset);
        uint32_t      len  = (d->mem[i].length + MEM_GRANULARITY_MASK) & ~MEM_GRANULARITY_MASK;

        /* mem_base 0 = the user left the relocatable windows off. */
        if (!(d->mem[i].flags & ISAB_RANGE_FIXED) && (dev->mem_base == 0))
            continue;
        if ((d->mem[i].kind != ISAB_MEM_WINDOW) || (len == 0) || (base >= 0x100000)) {
            if (!dev->bad_warned)
                pclog("ISA bridge #%i: ignoring memory range %u (base %X, length %X)\n",
                      dev->inst, i, base, len);
            continue;
        }
        r->dev    = dev;
        r->idx    = i;
        r->base   = base;
        r->len    = len;
        r->active = 1;
        if (!r->added) {
            mem_mapping_add(&dev->mapping[i], base, len,
                            isab_mem_read, NULL, NULL, isab_mem_write, NULL, NULL,
                            NULL, MEM_MAPPING_EXTERNAL, r);
            r->added = 1;
        } else {
            mem_mapping_set_addr(&dev->mapping[i], base, len);
            mem_mapping_enable(&dev->mapping[i]);
        }
        isa_bridge_log("ISA bridge #%i: mem[%u] %05X-%05X\n", dev->inst, i, base, base + len - 1);
    }
    dev->bad_warned = 1;
}

static void
isab_unmap(bus_bridge_t *br)
{
    isa_bridge_t *dev = (isa_bridge_t *) br->fe;

    for (uint32_t i = 0; i < ISAB_MAX_IO; i++) {
        isab_range_t *r = &dev->io[i];

        if (r->active) {
            io_removehandler((uint16_t) r->base, (uint16_t) r->len,
                             isab_io_readb, (br->desc.io[i].widths & ISAB_W2) ? isab_io_readw : NULL, NULL,
                             isab_io_writeb, (br->desc.io[i].widths & ISAB_W2) ? isab_io_writew : NULL, NULL, r);
            r->active = 0;
        }
    }
    for (uint32_t i = 0; i < ISAB_MAX_MEM; i++) {
        isab_range_t *r = &dev->mem[i];

        if (r->active) {
            mem_mapping_disable(&dev->mapping[i]);
            r->active = 0;
        }
    }
    /* The connection is gone: let go of the IRQ and DMA lines too. */
    if (dev->irq_level && dev->irq)
        picintc(1 << dev->irq);
    dev->irq_level = 0;
    if (dev->dma8 >= 0)
        dma_set_drq(dev->dma8, 0);
    if (dev->dma16 >= 0)
        dma_set_drq(dev->dma16, 0);
}

static void
isab_pins(bus_bridge_t *br)
{
    isa_bridge_t *dev = (isa_bridge_t *) br->fe;
    uint32_t      irq = (br->desc.n_irq != 0) ? (br->shm->resp_irq & 1) : 0;
    uint32_t      drq = br->shm->resp_drq;

    if (dev->irq && (irq != dev->irq_level)) {
        if (irq)
            picint(1 << dev->irq);
        else
            picintc(1 << dev->irq);
    }
    dev->irq_level = irq;

    if (dev->dma8 >= 0)
        dma_set_drq(dev->dma8, !!(drq & ISAB_DRQ_8));
    if (dev->dma16 >= 0)
        dma_set_drq(dev->dma16, !!(drq & ISAB_DRQ_16));
}

static void
isab_service_dma(bus_bridge_t *br)
{
    isa_bridge_t *dev = (isa_bridge_t *) br->fe;
    int           n;

    for (n = 0; (n < ISAB_DMA_BURST_MAX) && (br->shm != NULL); n++) {
        uint32_t drq = br->shm->resp_drq;
        uint32_t dir = br->shm->resp_drq_dir;
        uint32_t bit;
        int      ch;
        int      val;

        if ((drq & ISAB_DRQ_8) && (dev->dma8 >= 0)) {
            bit = ISAB_DRQ_8;
            ch  = dev->dma8;
        } else if ((drq & ISAB_DRQ_16) && (dev->dma16 >= 0)) {
            bit = ISAB_DRQ_16;
            ch  = dev->dma16;
        } else
            break;

        if (dir & bit) {
            /* Device -> memory. Peek first so a masked channel doesn't
               consume a byte the device can't take back. */
            if (!dma_channel_writable(ch))
                break;
            val = (int) isab_xfer(dev, ISAB_DACK_WR, 0, 0, 0, 0, (bit == ISAB_DRQ_16) ? 2 : 1);
            if (dma_channel_write(ch, (uint16_t) val) == DMA_NODATA)
                break;
        } else {
            val = dma_channel_read(ch);
            if (val == DMA_NODATA)
                break;
            isab_xfer(dev, ISAB_DACK_RD, 0, 0, val & 0xffff, (bit == ISAB_DRQ_16) ? 2 : 1,
                      (val & DMA_OVER) ? ISAB_FLAG_TC : 0);
        }
    }
}

static const bus_bridge_ops_t isab_ops = {
    .bus     = ISAB_BUS_ISA,
    .name    = "ISA",
    .map     = isab_map,
    .unmap   = isab_unmap,
    .pins    = isab_pins,
    .service = isab_service_dma
};

static void
isab_reset(void *priv)
{
    isa_bridge_t *dev = (isa_bridge_t *) priv;

    isab_xfer(dev, ISAB_RESET, 0, 0, 0, 0, 0);
}

int
isa_bridge_launch_server(int inst)
{
    char sec[64];

    isa_bridge_section(inst, sec, sizeof(sec));
    return bus_bridge_launch(inst, sec, BUS_PROG_SERVER);
}

int
isa_bridge_launch_monitor(int inst)
{
    char sec[64];

    isa_bridge_section(inst, sec, sizeof(sec));
    return bus_bridge_launch(inst, sec, BUS_PROG_MONITOR);
}

/* ---- Config helpers for the settings UI ----------------------------- */

static const struct {
    int      io_base;
    uint32_t mem_base;
    int      irq;
    int      dma;
    int      dma16;
} isab_slot_defaults[ISA_BRIDGE_MAX] = {
    { 0x300, 0xd0000,  5,  1,  5 },
    { 0x320, 0xe0000,  7, -1, -1 },
    { 0x340, 0xd8000, 10, -1, -1 },
    { 0x280, 0xe8000,  3, -1, -1 },
    { 0x360, 0xdc000,  2, -1, -1 }
};

void
isa_bridge_seed_defaults(int inst)
{
    char sec[64];
    char path[64];

    if ((inst < 1) || (inst > ISA_BRIDGE_MAX))
        return;
    isa_bridge_section(inst, sec, sizeof(sec));
    config_create_section(sec);

    if (!ini_has_entry(config_find_section(sec), "base"))
        config_set_hex16(sec, "base", isab_slot_defaults[inst - 1].io_base);
    if (!ini_has_entry(config_find_section(sec), "mem_base"))
        config_set_hex20(sec, "mem_base", isab_slot_defaults[inst - 1].mem_base);
    if (!ini_has_entry(config_find_section(sec), "irq"))
        config_set_int(sec, "irq", isab_slot_defaults[inst - 1].irq);
    if (!ini_has_entry(config_find_section(sec), "dma"))
        config_set_int(sec, "dma", isab_slot_defaults[inst - 1].dma);
    if (!ini_has_entry(config_find_section(sec), "dma16"))
        config_set_int(sec, "dma16", isab_slot_defaults[inst - 1].dma16);
    if (!ini_has_entry(config_find_section(sec), "shm_path")) {
        snprintf(path, sizeof(path), "/dev/shm/isabridge%i", inst);
        config_set_string(sec, "shm_path", path);
    }
}

void
isa_bridge_delete_config(int inst)
{
    static const char *const keys[] = { "base", "mem_base", "irq", "dma", "dma16", "rom_enabled",
                                        "rom_fn", "rom_addr", "rom_size", "sync_us", "shm_path",
                                        "server_cmd", "server_auto", "monitor_cmd", "monitor_auto", NULL };
    char                     sec[64];

    if ((inst < 1) || (inst > ISA_BRIDGE_MAX))
        return;
    isa_bridge_section(inst, sec, sizeof(sec));
    if (config_find_section(sec) == NULL)
        return;
    for (int i = 0; keys[i] != NULL; i++)
        config_delete_var(sec, keys[i]);
    ini_delete_section_if_empty(config_get_ini(), config_find_section(sec));
}

/* ---- Device --------------------------------------------------------- */

static void *
isab_init(UNUSED(const device_t *info))
{
    isa_bridge_t *dev = (isa_bridge_t *) calloc(1, sizeof(isa_bridge_t));
    const char   *path;
    char          shm[512];
    char          sec[64];
    int           rom_size;
    uint32_t      rom_addr;

    dev->inst = device_get_instance();
    if (dev->inst < 1)
        dev->inst = 1;
    isa_bridge_section(dev->inst, sec, sizeof(sec));

    dev->io_base  = device_get_config_hex16("base");
    dev->mem_base = device_get_config_hex20("mem_base");
    dev->irq      = device_get_config_int("irq");
    dev->dma8     = device_get_config_int("dma");
    dev->dma16    = device_get_config_int("dma16");
    path          = device_get_config_string("shm_path");
    if ((path == NULL) || (*path == '\0'))
        snprintf(shm, sizeof(shm), "/dev/shm/isabridge%i", dev->inst);
    else
        snprintf(shm, sizeof(shm), "%s", path);

    /* Option ROM, entirely on our side of the bridge. */
    path     = config_get_string(sec, "rom_fn", "");
    rom_addr = (uint32_t) device_get_config_hex20("rom_addr");
    rom_size = device_get_config_int("rom_size");
    if (config_get_int(sec, "rom_enabled", 0) && (path != NULL) && (*path != '\0') && rom_addr && (rom_size > 0)) {
        if (rom_init(&dev->rom, path, rom_addr, rom_size,
                     (rom_size - 1) | MEM_GRANULARITY_MASK, 0, MEM_MAPPING_EXTERNAL) == 0)
            dev->rom_loaded = 1;
        else
            pclog("ISA bridge #%i: cannot load option ROM %s\n", dev->inst, path);
    }

    if (config_get_int(sec, "server_auto", 0))
        isa_bridge_launch_server(dev->inst);

    if (bus_bridge_open(&dev->br, &isab_ops, dev, dev->inst, shm,
                        (double) device_get_config_int("sync_us")))
        isab_xfer(dev, ISAB_RESET, 0, 0, 0, 0, 0);
    else
        pclog("ISA bridge #%i: %s not there yet, will keep retrying\n", dev->inst, shm);

    if (config_get_int(sec, "monitor_auto", 0))
        isa_bridge_launch_monitor(dev->inst);

    return dev;
}

static void
isab_close(void *priv)
{
    isa_bridge_t *dev = (isa_bridge_t *) priv;

    bus_bridge_close(&dev->br);
    if (dev->rom_loaded)
        mem_mapping_disable(&dev->rom.mapping);
    free(dev);
}

static const device_config_t isa_bridge_config[] = {
  // clang-format off
    {
        .name           = "base",
        .description    = "I/O base",
        .type           = CONFIG_HEX16,
        .default_string = NULL,
        .default_int    = 0x300,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "280H", .value = 0x280 },
            { .description = "300H", .value = 0x300 },
            { .description = "320H", .value = 0x320 },
            { .description = "340H", .value = 0x340 },
            { .description = "360H", .value = 0x360 },
            { .description = ""                     }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "mem_base",
        .description    = "Memory window base",
        .type           = CONFIG_HEX20,
        .default_string = NULL,
        .default_int    = 0xd0000,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "Disabled", .value = 0x00000 },
            { .description = "D000H",    .value = 0xd0000 },
            { .description = "D400H",    .value = 0xd4000 },
            { .description = "D800H",    .value = 0xd8000 },
            { .description = "DC00H",    .value = 0xdc000 },
            { .description = "E000H",    .value = 0xe0000 },
            { .description = "E400H",    .value = 0xe4000 },
            { .description = "E800H",    .value = 0xe8000 },
            { .description = "EC00H",    .value = 0xec000 },
            { .description = ""                          }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "irq",
        .description    = "IRQ",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 5,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "None",   .value =  0 },
            { .description = "IRQ 2",  .value =  2 },
            { .description = "IRQ 3",  .value =  3 },
            { .description = "IRQ 4",  .value =  4 },
            { .description = "IRQ 5",  .value =  5 },
            { .description = "IRQ 7",  .value =  7 },
            { .description = "IRQ 9",  .value =  9 },
            { .description = "IRQ 10", .value = 10 },
            { .description = "IRQ 11", .value = 11 },
            { .description = "IRQ 12", .value = 12 },
            { .description = "IRQ 15", .value = 15 },
            { .description = ""                    }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "dma",
        .description    = "Low DMA",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 1,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "None",  .value = -1 },
            { .description = "DMA 0", .value =  0 },
            { .description = "DMA 1", .value =  1 },
            { .description = "DMA 3", .value =  3 },
            { .description = ""                   }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "dma16",
        .description    = "High DMA",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 5,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "None",  .value = -1 },
            { .description = "DMA 5", .value =  5 },
            { .description = "DMA 6", .value =  6 },
            { .description = "DMA 7", .value =  7 },
            { .description = ""                   }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "rom_addr",
        .description    = "Option ROM address",
        .type           = CONFIG_HEX20,
        .default_string = NULL,
        .default_int    = 0xc0000,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "C000H", .value = 0xc0000 },
            { .description = "C800H", .value = 0xc8000 },
            { .description = "CC00H", .value = 0xcc000 },
            { .description = "D000H", .value = 0xd0000 },
            { .description = "D800H", .value = 0xd8000 },
            { .description = "E000H", .value = 0xe0000 },
            { .description = ""                        }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "rom_size",
        .description    = "Option ROM size",
        .type           = CONFIG_SELECTION,
        .default_string = NULL,
        .default_int    = 4096,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = {
            { .description = "2 KB",  .value =  2048 },
            { .description = "4 KB",  .value =  4096 },
            { .description = "8 KB",  .value =  8192 },
            { .description = "16 KB", .value = 16384 },
            { .description = "32 KB", .value = 32768 },
            { .description = "64 KB", .value = 65536 },
            { .description = ""                      }
        },
        .bios           = { { 0 } }
    },
    {
        .name           = "sync_us",
        .description    = "Idle sync period (us)",
        .type           = CONFIG_SPINNER,
        .default_string = NULL,
        .default_int    = 1000,
        .file_filter    = NULL,
        .spinner        = { .min = 1, .max = 30000, .step = 1 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    {
        .name           = "shm_path",
        .description    = "Shared memory file",
        .type           = CONFIG_STRING,
        .default_string = "",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = { { 0 } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
  // clang-format on
};

const device_t isa_bridge_device = {
    .name          = ISAB_SECTION_NAME,
    .internal_name = "isa_bridge",
    .flags         = DEVICE_ISA16,
    .local         = 0,
    .init          = isab_init,
    .close         = isab_close,
    .reset         = isab_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = isa_bridge_config
};
