/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for the remote machine monitor (RMM).
 */
#ifndef EMU_RMM_H
#define EMU_RMM_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USE_RMM

extern int rmm_port;             /* [General] rmm_port, 0 = off */
extern int rmm_port_cmdline;     /* --monitor-port, overrides rmm_port, never saved */
extern int rmm_pause_on_connect; /* [General] rmm_pause_on_connect, default 1 */
extern int rmm_sticky_cmdline;   /* --monitor-sticky-pause, never saved */
extern int rmm_sticky_pause;     /* [General] rmm_sticky_pause */
extern volatile int rmm_hard_resets; /* bumped at the end of pc_reset_hard_init() */
extern int rmm_disk_track;      /* [General] rmm_disk_track */

extern void rmm_init(void);
extern void rmm_close(void);
/* Apply config keys staged by config.set. Called at the start of pc_reset_hard_init(). */
extern void rmm_pending_reset_apply(void);

/* Debug executor: exec386_rmm is exec386 with a hook before each instruction,
   used while a breakpoint or step is armed. */
extern void         exec386_rmm(int32_t cycs);  /* cpu/386_dynarec.c */
extern int          rmm_cpu_instruction(void);  /* CPU thread, before each instruction: 1 = stop */
extern volatile int rmm_cpu_halted;             /* stopped, until the monitor has paused */
/* End of cpu_set(): reinstall the debug executor if something is armed. */
extern void rmm_cpu_init(void);

/* mem/mem.c: physical access for the monitor. */
extern uint8_t mem_rmm_readb_phys(uint32_t addr);
extern void    mem_rmm_writeb_phys(uint32_t addr, uint8_t val);

/* video/video.c: capture.screenshot frame copy, 0x00RRGGBB pixels. On 1 the buffer
   is valid until video_rmm_capture_release(). */
extern void video_rmm_capture_init(void);
extern int  video_rmm_capture(const uint32_t **buf, int *w, int *h, uint32_t *seq, int *fresh, int timeout_ms);
extern void video_rmm_capture_release(void);

/* Implemented by the platform UI: request a pause state change from any thread. */
extern void plat_pause_async(int p);
/* Set while do_pause() leaves the CPU thread ack to the caller. UI thread only. */
extern volatile int rmm_pause_nowait;
/* The CPU thread's pause acknowledgement, and clearing it once consumed. */
extern int  rmm_pause_acked(void);
extern void rmm_pause_ack_clear(void);
/* Trigger a main-window QAction by object name on the UI thread:
   1 triggered, 0 no such action, -1 disabled. */
extern int plat_rmm_trigger_action(const char *name);
/* Media slots: dev 0 floppy, 1 CD-ROM. Insert or eject through the media menu,
   path NULL ejects. Blocks until the UI thread has run it. */
extern int  plat_rmm_media_slots(int dev, int *out, int max);
extern void plat_rmm_media(int dev, int slot, const char *path, int wp);
/* Power the emulator off without the exit confirmation. */
extern void plat_rmm_quit(void);

#else

#    define rmm_init()
#    define rmm_close()
#    define rmm_pending_reset_apply()
#    define rmm_cpu_init()

#endif

#ifdef __cplusplus
}
#endif

#endif /*EMU_RMM_H*/
