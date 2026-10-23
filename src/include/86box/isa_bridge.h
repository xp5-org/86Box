/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for the external ISA bridge.
 *
 * Authors: xp5-org
 *
 *          Copyright 2026
 *
 */

#ifndef EMU_ISA_BRIDGE_H
#define EMU_ISA_BRIDGE_H

#include <86box/device.h>

#define ISA_BRIDGE_MAX 5

#ifdef __cplusplus
extern "C" {
#endif

/* [Other peripherals] isa_bridge<n>_enabled, slot n = index + 1. */
extern int isa_bridge_enabled[ISA_BRIDGE_MAX];

extern const device_t isa_bridge_device;

/* Config section of slot `inst` (1..ISA_BRIDGE_MAX). */
extern void isa_bridge_section(int inst, char *buf, int len);

/* Give a freshly created slot its own I/O base, memory window, IRQ and
   shared memory file, so it does not land on top of the other slots.
   Only fills in what the section does not have yet. */
extern void isa_bridge_seed_defaults(int inst);

/* Forget everything stored for a slot that was removed. */
extern void isa_bridge_delete_config(int inst);

/* Run the slot's RTL server command (server_cmd), same rules as below. */
extern int isa_bridge_launch_server(int inst);

/* Run the slot's monitor command (a shell command line) unless a
   previous launch of it is still running. Returns 1 if it was started. */
extern int isa_bridge_launch_monitor(int inst);

#ifdef __cplusplus
}
#endif

#endif /*EMU_ISA_BRIDGE_H*/
