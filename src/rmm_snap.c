/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Remote machine monitor snapshots: in-memory savevm/loadvm.
 *          See rmm_snap.h for the visitor contract.
 *
 * Authors: xp5-org
 *
 * Copyright 2026
 *
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#    include <io.h>
#    include <process.h>
#else
#    include <unistd.h>
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/path.h>
#include <86box/thread.h>
#include "cpu.h"
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/nmi.h>
#include <86box/pit.h>
#include <86box/pit_fast.h>
#include <86box/port_6x.h>
#include <86box/port_92.h>
#include <86box/nvr.h>
#include <86box/keyboard.h>
#include <86box/mem.h>
#include <86box/row.h>
#include <86box/fdd.h>
#include <86box/fdc.h>
#include <86box/serial.h>
#include <86box/lpt.h>
#include <86box/char.h>
#include <86box/prt_devs.h>
#include <86box/gameport.h>
#include <86box/flash.h>
#include <86box/hdc.h>
#include <86box/scsi_aha154x.h>
#include <86box/scsi_aic6360.h>
#include <86box/scsi_buslogic.h>
#include <86box/scsi_device.h>
#include <86box/scsi_ncr5380.h>
#include <86box/scsi_ncr53c8xx.h>
#include <86box/scsi_aic7xxx.h>
#include <86box/hdc_ide.h>
#include <86box/cdrom.h>
#include <86box/cdrom_cm153.h>
#include <86box/cdrom_hitachi.h>
#include <86box/cdrom_mitsumi.h>
#include <86box/cdrom_mke.h>
#include <86box/cdrom_philips.h>
#include <86box/cdrom_sony.h>
#include <86box/fdc_ext.h>
#include <86box/sound.h>
#include <86box/snd_opl.h>
#include <86box/video.h>
#include <86box/machine.h>
#include <86box/chipset.h>
#include <86box/sio.h>

/* Super I/O variants no header declares. */
extern const device_t pc87307_15c_device;
extern const device_t pc87307_both_device;
extern const device_t pc97307_device;
extern const device_t isp1020a_device;
extern const device_t mouse_mtouch_device;
extern const device_t mouse_upc_device;
extern const device_t mouse_upc_standalone_device;
extern const device_t ps55da2_device;
extern const device_t ps55db2_device;
extern const device_t qla1040b_device;
extern const device_t qla1080_device;
extern const device_t qla12160a_device;
extern const device_t qla1240_device;
extern const device_t qla1280_device;
extern const device_t ac97_via_device;
extern const device_t ad1881_device;
extern const device_t ak4540_device;
extern const device_t alc100_device;
extern const device_t atiega800p_device;
extern const device_t av9194_device;
extern const device_t cpqega_device;
extern const device_t cs4297_device;
extern const device_t cs4297a_device;
extern const device_t ega_device;
extern const device_t et2000_device;
extern const device_t gen8514_isa_device;
extern const device_t ibm8514_mca_device;
extern const device_t ibm_rgb528_ramdac_device;
extern const device_t icd2061_device;
extern const device_t ice1232_device;
extern const device_t ics9161_device;
extern const device_t inmos_isa_device;
extern const device_t iskra_ega_device;
extern const device_t jega_device;
extern const device_t jvga_device;
extern const device_t pcjx_video_device;
extern const device_t sega_device;
extern const device_t si3036_device;
extern const device_t stac9708_device;
extern const device_t stac9721_device;
extern const device_t tr28023_device;
extern const device_t tvp3026_ramdac_device;
extern const device_t w83971d_device;
extern const device_t wm9701a_device;
extern const device_t xga_device;
extern const device_t mpu401_device;
extern const device_t mpu401_mca_device;
extern const device_t ibm_xma_mca_2mb_device;
extern const device_t ibm_xma_mca_8mb_f7f7_device;
extern const device_t ibm_xma_mca_8mb_f7fe_device;
extern const device_t inboard386_at_device;
extern const device_t inboard386_xt_device;
extern const device_t quadmeg_psq_4mb_76da_device;
extern const device_t quadmeg_psq_4mb_76de_device;
extern const device_t softpower_device;
extern const device_t bugger_device;
extern const device_t mouse_ps2_device;
extern const device_t postcard_device;
extern const device_t unittester_device;
extern const device_t smc_epic100_device;
extern const device_t ibm7690_video_device;
extern const device_t ncr8496_device;
extern const device_t sn76489_device;
extern const device_t colorplus_device;
extern const device_t mda_device;
extern const device_t mouse_cga_lightpen_device;
extern const device_t t3100e_device;
extern const device_t oti067_m300_device;
extern const device_t tvis_vid_device;
extern const device_t pci_dummy_device;
extern const device_t ps2_nvr_2kb_device;
extern const device_t ps2_nvr_8kb_device;
extern const device_t m24_kbd_device;
extern const device_t ati18810_28800_device;
extern const device_t ati18810_mach32_device;
extern const device_t ati18811_0_28800_device;
extern const device_t ati18811_0_mach32_device;
extern const device_t ati18811_1_28800_device;
extern const device_t ati18811_1_mach32_device;
extern const device_t ati68860_ramdac_device;
extern const device_t ati68875_ramdac_device;
extern const device_t att20c504_ramdac_device;
extern const device_t att20c505_ramdac_device;
extern const device_t att490_ramdac_device;
extern const device_t att491_ramdac_device;
extern const device_t att492_ramdac_device;
extern const device_t att498_ramdac_device;
extern const device_t bt481_ramdac_device;
extern const device_t bt484_ramdac_device;
extern const device_t bt485_ramdac_device;
extern const device_t bt485a_ramdac_device;
extern const device_t ch9201_device;
extern const device_t ch9204_device;
extern const device_t gendac_ramdac_device;
extern const device_t icd2047_20_device;
extern const device_t ics1494m_540_device;
extern const device_t ics2494an_304_device;
extern const device_t ics2494an_305_device;
extern const device_t ics2494an_318_device;
extern const device_t ics2494an_324_device;
extern const device_t ics2595_device;
extern const device_t ics90c64a_903_device;
extern const device_t mu9c1880_ramdac_device;
extern const device_t sc11483_ramdac_device;
extern const device_t sc11484_nors2_ramdac_device;
extern const device_t sc11484_ramdac_device;
extern const device_t sc11486_ramdac_device;
extern const device_t sc11487_nors2_ramdac_device;
extern const device_t sc11487_ramdac_device;
extern const device_t sc1502x_ramdac_device;
extern const device_t sc1502x_rs2_ramdac_device;
extern const device_t sdac_ramdac_device;
extern const device_t stg1702_ramdac_device;
extern const device_t stg1703_ramdac_device;
extern const device_t tkd8001_ramdac_device;
extern const device_t tseng_ics5301_ramdac_device;
extern const device_t tseng_ics5341_ramdac_device;
extern const device_t via_vt82c49x_ide_device;
extern const device_t aha1740_device;
extern const device_t aha1740a_device;
extern const device_t aha1742a_device;
extern const device_t aha1744_device;
extern const device_t am53c974_pci_device;
extern const device_t am53c974a_pci_device;
extern const device_t dc390_pci_device;
extern const device_t ncr53c90a_mca_device;
extern const device_t spock_device;
extern const device_t tribble_device;
extern const device_t access_bus_device;
extern const device_t as99127f_device;
extern const device_t as99127f_rev2_device;
extern const device_t gl518sm_2c_device;
extern const device_t gl518sm_2d_device;
extern const device_t gl520sm_2c_device;
extern const device_t gl520sm_2d_device;
extern const device_t lm75_1_4a_device;
extern const device_t lm75_w83781d_device;
extern const device_t lm77_0_48_device;
extern const device_t lm78_al440lx_device;
extern const device_t lm78_device;
extern const device_t mouse_ibm7690_touch_device;
extern const device_t mouse_logibus_device;
extern const device_t mouse_logibus_onboard_device;
extern const device_t mouse_ltserial_device;
extern const device_t mouse_msinport_device;
extern const device_t mouse_msserial_ballpoint_device;
extern const device_t mouse_msserial_device;
extern const device_t mouse_mssystems_bus_device;
extern const device_t mouse_mssystems_device;
extern const device_t mouse_pxl_380_device;
extern const device_t novell_keycard_device;
extern const device_t via_vt82c686_hwm_device;
extern const device_t w83781d_device;
extern const device_t w83781d_p5a_device;
extern const device_t w83782d_device;
#include <86box/pci.h>
#include <86box/acpi.h>
#include <86box/agpgart.h>
#include <86box/ddma.h>
#include <86box/hdc_ide_sff8038i.h>
#include <86box/smbus.h>
#include <86box/apm.h>
#include <86box/nmc93cxx.h>
#include <86box/sis_55xx.h>
#include <86box/usb.h>
#include <86box/network.h>
#include <86box/net_dp8390.h>
#include <86box/rmm.h>
#include <86box/rmm_snap.h>

/* Devices no shared header declares. */
extern const device_t ibm_5161_device;
extern const device_t mcga_device;
extern const device_t ibm5140_device;
extern const device_t pcjx_device;
extern const device_t cassette_device;

#define RMM_SNAP_MAX     32
#define RMM_SNAP_CHUNK   (1 << 20) /* writer I/O size, and cancel granularity */
#define RMM_E_NO_SNAP    7
#define RMM_E_CANNOT     8

extern int         device_rmm_get(int c, const device_t **dev, void **priv);
extern pc_timer_t *timer_head;

typedef struct {
    const device_t *dev;
    void           *priv;
} rmm_snap_devref_t;

struct rmm_snap_t {
    uint8_t *buf;
    size_t   len;     /* bytes written on save, total on restore */
    size_t   cap;
    size_t   pos;     /* restore cursor */
    int      restore;
    int      failed;  /* allocation failure or a size mismatch */
    struct rmm_reloc *rl;    /* restoring into another process: see "relocation" */
    int               local; /* the next raw dump is a local variable, not state */
    int               data;  /* the next raw dump is guest data: holds no pointers */
    const void       *tmpl;  /* live object whose pointers a local dump keeps */
};

/* Where a snapshot's device and RAM buffer data.buf lives. */
typedef enum {
    SNAP_MEM,     /* in memory; queued for the writer if one is configured */
    SNAP_WRITING, /* the writer thread is copying data.buf to path */
    SNAP_DISK,    /* data.buf freed, the bytes are in path */
    SNAP_ERROR,   /* the write failed; data.buf is still in memory */
    SNAP_DEAD     /* deleted while writing; the writer is done with it */
} rmm_snap_state_t;

typedef struct rmm_snapshot_t {
    char               tag[64];
    uint64_t           tsc;
    int                generation; /* rmm_hard_resets when taken */
    int                ndevs;
    rmm_snap_devref_t *devs;       /* the device table when taken */
    void              *media[2 * FDD_NUM];
    uint32_t           cdmedia[CDROM_NUM]; /* cdrom_rmm_snap_media() */
    void              *disks;      /* hdd_image_rmm_save() */
    uint8_t           *reloc;      /* pointer table, see "relocation" */
    size_t             reloclen;
    char              *ident;      /* rmm_snap_ident(): what a loading process must have */
    char              *build;      /* rmm_snap_build_text() */
    char               unplaced[256]; /* timers no dumped struct or module holds, by callback; empty if none */
    int                foreign;    /* loaded from a file: always restored through the pointer table */
    int                external;   /* path is the user's file: never removed */
    uint64_t           data_off;   /* of the DATA section in path */
    char               filetag[64]; /* the tag in path's header */
    rmm_snap_t         data;

    /* Written by the writer thread under snap_lock; cancel is also polled. */
    rmm_snap_state_t   state;
    int                queued;     /* on the writer's queue */
    volatile int       cancel;     /* deleted or replaced while writing */
    uint64_t           seq;
    size_t             filelen;    /* data.len, kept once data.buf is gone */
    char               path[600];
    char               errmsg[128];
    struct rmm_snapshot_t *next;   /* writer queue / zombie list */
} rmm_snapshot_t;

static rmm_snapshot_t *rmm_snaps[RMM_SNAP_MAX];

/* Devices whose whole state a visitor covers; others make snapshot.save fail with error 8. */
static const struct {
    const device_t *dev;
    rmm_snap_fn     fn;   /* NULL: the priv is one allocation of size bytes */
    size_t          size;
} rmm_snap_devices[] = {
    { &i8254_fast_device, NULL,             sizeof(pitf_t)    },
    /* The classic PITs, not the fast ones: one pit_t each. */
#define PIT(d) { &d, NULL, sizeof(pit_t) }
    PIT(i8253_device), PIT(i8253_ext_io_device), PIT(i8254_device), PIT(i8254_sec_device),
    PIT(i8254_ext_io_device), PIT(i8254_ps2_device),
    { &vga_device,        vga_rmm_snap,     0                 },
    { &rabbit_device,     rabbit_rmm_snap,  0                 },
    /* XT and PS/2 Model 25 board devices. Other XT-family keyboard controllers are not covered. */
    { &ibm_5161_device,         ibm_5161_rmm_snap,    0 },
    { &keyboard_pc_xt_device,   kbd_xt_rmm_snap,      0 },
    { &kbc_xt86_device,         kbc_xt_rmm_snap,      0 },
    { &kbc_xtclone_device,      kbc_xt_rmm_snap,      0 },
    { &kbc_ps2_m25_device,      ps2_m25_kbc_rmm_snap, 0 },
    { &mcga_device,             mcga_rmm_snap,        0 },
#define PITF(d) { &d, NULL, sizeof(pitf_t) }
    PITF(i8253_fast_device), PITF(i8253_ext_io_fast_device), PITF(i8254_sec_fast_device),
    PITF(i8254_ext_io_fast_device), PITF(i8254_ps2_fast_device),
    { &port_6x_device,    port_6x_rmm_snap, 0                 },
    { &port_92_device,    NULL,             sizeof(port_92_t) },
    { &nvr_at_device,     nvr_at_rmm_snap,  0                 },
    { &kbc_at_device,     kbc_at_rmm_snap,  0                 },
    { &kbc_at_vectra_device, kbc_at_rmm_snap, 0               },
#define KBD(d) { &d, keyboard_at_rmm_snap, 0 }
    KBD(keyboard_at_device), KBD(keyboard_ax_device), KBD(keyboard_ps2_device),
    KBD(keyboard_ps55_device), KBD(keyboard_at_generic_device),
    { &sis_85c496_device,        sis_85c496_rmm_snap, 0 },
    { &sis_85c496_ls486e_device, sis_85c496_rmm_snap, 0 },
    { &row_device,               row_rmm_snap,        0 },
    { &w837x7_device,            w837x7_rmm_snap,     0 },
#define FDC(d) { &d, fdc_rmm_snap, 0 }
    FDC(fdc_xt_device), FDC(fdc_ibm5140_device), FDC(fdc_xt_sec_device), FDC(fdc_xt_ter_device),
    FDC(fdc_xt_qua_device), FDC(fdc_xt_t1x00_device), FDC(fdc_xt_amstrad_device),
    FDC(fdc_xt_tandy_device), FDC(fdc_xt_umc_um8398_device), FDC(fdc_xt_5550_device),
    FDC(fdc_pcjr_device), FDC(fdc_pcjx_device), FDC(fdc_at_device), FDC(fdc_at_sec_device),
    FDC(fdc_at_ter_device), FDC(fdc_at_qua_device), FDC(fdc_at_actlow_device),
    FDC(fdc_at_smc_661_device), FDC(fdc_at_smc_device), FDC(fdc_at_ali_device),
    FDC(fdc_at_winbond_device), FDC(fdc_at_nsc_device), FDC(fdc_at_nsc_pc87310_device),
    FDC(fdc_at_nsc_dp8473_device), FDC(fdc_ps2_device), FDC(fdc_ps2_mca_device),
#define UART(d) { &d, serial_rmm_snap, 0 }
    UART(ns8250_device), UART(ns8250_pcjr_3f8_device), UART(ns8250_pcjr_2f8_device),
    UART(ns16450_device), UART(ns16550_device), UART(ns16650_device), UART(ns16750_device),
    UART(ns16850_device), UART(ns16950_device),
    { &lpt_port_device,          lpt_rmm_snap,        0 },
    { &lpt_epat_device,          epat_rmm_snap,       0 },
    { &lpt_bpck_device,          bpck_rmm_snap,       0 },
    { &lpt_ditto_device,         ditto_rmm_snap,      0 },
    /* Not the PnP game ports: ISAPnP state is not covered. */
#define GAME(d) { &d, gameport_rmm_snap, 0 }
    GAME(gameport_device), GAME(gameport_200_device), GAME(gameport_201_device),
    GAME(gameport_203_device), GAME(gameport_205_device), GAME(gameport_207_device),
    GAME(gameport_208_device), GAME(gameport_209_device), GAME(gameport_20b_device),
    GAME(gameport_20d_device), GAME(gameport_20f_device), GAME(gameport_sio_device),
    GAME(gameport_sio_1io_device),
    { &sst_flash_29ee010_device, sst_rmm_snap,        0 },
#define SST(d) { &d, sst_rmm_snap, 0 }
    SST(amd_flash_29f002nbt_device), SST(amd_flash_29f010a_device), SST(amd_flash_29f020a_device),
    SST(sst_flash_29ee020_device), SST(sst_flash_39lf010_device), SST(sst_flash_39lf016_device),
    SST(sst_flash_39lf020_device), SST(sst_flash_39lf040_device), SST(sst_flash_39lf080_device),
    SST(sst_flash_39lf512_device), SST(sst_flash_39sf010_device), SST(sst_flash_39sf020_device),
    SST(sst_flash_39sf040_device), SST(sst_flash_39sf512_device), SST(sst_flash_49lf002_device),
    SST(sst_flash_49lf003_device), SST(sst_flash_49lf004_device), SST(sst_flash_49lf004c_device),
    SST(sst_flash_49lf008_device), SST(sst_flash_49lf008c_device), SST(sst_flash_49lf016_device),
    SST(sst_flash_49lf020_device), SST(sst_flash_49lf020a_device), SST(sst_flash_49lf030_device),
    SST(sst_flash_49lf040_device), SST(sst_flash_49lf080_device), SST(sst_flash_49lf160_device),
    SST(winbond_flash_w29c010_device), SST(winbond_flash_w29c011a_device), SST(winbond_flash_w29c020_device),
    SST(winbond_flash_w29c040_device), SST(winbond_flash_w29c512_device),
#define IFLASH(d) { &d, intel_flash_rmm_snap, 0 }
    IFLASH(intel_flash_bxb_device), IFLASH(intel_flash_bxt_ami_device), IFLASH(intel_flash_bxt_device),
    IFLASH(micron_flash_t_device), IFLASH(micron_flash_x00_t_device),
    { &intel_flash_e28f0xx_device,         intel_flash_e28f0xx_rmm_snap, 0 },
    { &intel_flash_e28f0xx_cobalt3k_device, intel_flash_e28f0xx_rmm_snap, 0 },
    { &catalyst_flash_device,              catalyst_flash_rmm_snap,      0 },
    { &amd_am28f010_flash_device,          catalyst_flash_rmm_snap,      0 },
    { &amd_flash_am29f016d_device,         amd_flash_rmm_snap,           0 },
    { &ide_pci_2ch_device,       ide_rmm_snap,        0 },
#define IDEU(d) { &d, ide_rmm_snap, 0 }
#define IDEB(d) { &d, ide_board_rmm_snap, 0 }
    IDEU(ide_isa_device), IDEU(ide_isa_2ch_device), IDEU(ide_vlb_device), IDEU(ide_vlb_2ch_device),
    IDEU(ide_pci_device), IDEU(ide_pci_ter_qua_2ch_device),
    IDEB(ide_isa_sec_device), IDEB(ide_vlb_sec_device), IDEB(ide_pci_sec_device),
    IDEB(ide_ter_device), IDEB(ide_ter_pnp_device), IDEB(ide_qua_device), IDEB(ide_qua_pnp_device),
#define STXT(d) { &d, st506_xt_rmm_snap, 0 }
    STXT(st506_xt_xebec_device), STXT(st506_xt_wdxt_gen_device), STXT(st506_xt_dtc5150x_device),
    STXT(st506_xt_st11_m_device), STXT(st506_xt_st11_r_device), STXT(st506_xt_wd1002a_wx1_device),
    STXT(st506_xt_wd1002a_wx1_nobios_device), STXT(st506_xt_wd1002a_27x_device), STXT(st506_xt_wd1004a_wx1_device),
    STXT(st506_xt_wd1004_27x_device), STXT(st506_xt_wd1004a_27x_device), STXT(st506_xt_victor_v86p_device),
    STXT(st506_xt_toshiba_t1200_device),
    { &st506_at_wd1003_device, st506_at_rmm_snap, 0 },
    { &esdi_at_wd1007vse1_device, esdi_at_rmm_snap, 0 },
#define XTA(d) { &d, xta_rmm_snap, 0 }
    XTA(xta_wdxt150_device), XTA(xta_wdxt150_pc3086_device), XTA(xta_hd20_device), XTA(xta_st50x_pc5086_device),
    { &xta_st50x_device, xta_dual_rmm_snap, 0 },
#define XTIDE(d) { &d, xtide_rmm_snap, 0 }
    XTIDE(xtide_device), XTIDE(jride_device), XTIDE(xtide_at_device), XTIDE(xtide_at_2ch_device),
    XTIDE(xtide_acculogic_device), XTIDE(xtide_at_ps2_device), XTIDE(xtide_at_ps2_2ch_device),
#define AHA(d) { &d, aha_rmm_snap, 0 }
    AHA(aha154xa_device), AHA(aha154xb_device), AHA(aha154xc_device), AHA(aha154xcf_device),
    AHA(aha154xcp_device), AHA(aha1640_device),
#define BUSL(d) { &d, buslogic_rmm_snap, 0 }
    BUSL(buslogic_542b_device), BUSL(buslogic_545s_device), BUSL(buslogic_542bh_device),
    BUSL(buslogic_545c_device), BUSL(buslogic_640a_device), BUSL(buslogic_445s_device),
    BUSL(buslogic_445c_device), BUSL(buslogic_958d_pci_device),
    { &aha1520a_device, aic6360_rmm_snap, 0 },
    { &sb16_scsi_device, aic6360_rmm_snap, 0 },
#define N400(d) { &d, ncr53c400_rmm_snap, 0 }
    N400(scsi_lcs6821n_device), N400(scsi_rt1000b_device), N400(scsi_rt1000mc_device),
    N400(scsi_t130_device), N400(scsi_t130b_device), N400(scsi_ls2000_device),
#define T128(d) { &d, t128_rmm_snap, 0 }
    T128(scsi_t128_device), T128(scsi_t228_device), T128(scsi_pas_device),
#define N8XX(d) { &d, ncr53c8xx_rmm_snap, 0 }
    N8XX(ncr53c810_pci_device), N8XX(ncr53c810_onboard_pci_device), N8XX(ncr53c815_pci_device),
    N8XX(ncr53c820_pci_device), N8XX(ncr53c825a_pci_device), N8XX(ncr53c860_pci_device),
    N8XX(ncr53c875_pci_device), N8XX(ncr53c875_onboard_pci_device),
#define AIC7(d) { &d, aic7xxx_rmm_snap, 0 }
    AIC7(aic7880_pci_device), AIC7(aha274x_device), AIC7(aha2940_pci_device),
    AIC7(aha2940u_pci_device), AIC7(aha2944uw_pci_device),
    { &philips_cm153_device, cm153_rmm_snap, 0 },
    { &hitachi_cdrom_isa_device, hitachi_rmm_snap, 0 },
    { &hitachi_cdrom_mca_device, hitachi_rmm_snap, 0 },
    { &philips_cm250_device, cm250_rmm_snap, 0 },
    { &sony_cdu31a_device, sony_rmm_snap, 0 },
    { &sony_creative_device, sony_rmm_snap, 0 },
    { &mitsumi_cdrom_device, mitsumi_rmm_snap, 0 },
    { &mitsumi_cdrom_vis_device, mitsumi_rmm_snap, 0 },
    { &mke_cdrom_device, mke_rmm_snap, 0 },
    { &mke_cdrom_noncreative_device, mke_rmm_snap, 0 },
    { &teac_cdrom_device, mke_rmm_snap, 0 },
    { &teac_cdrom_16bit_device, mke_rmm_snap, 0 },
    { &fdc_compaticard_i_device, compaticard_rmm_snap, 0 },
    { &fdc_compaticard_ii_device, compaticard_rmm_snap, 0 },
    { &fdc_compaticard_iv_device, compaticard_rmm_snap, 0 },
    { &fdc_b215_device, b215_rmm_snap, 0 },
    { &fdc_monster_device, monster_fdc_rmm_snap, 0 },
    /* Intel 440BX board, bx6: northbridge, southbridge, ACPI, APM, SMBus, W83977, serial EEPROMs, port 92. */
#define I4X0(d) { &d, i4x0_rmm_snap, 0 }
    I4X0(i420tx_device), I4X0(i420zx_device), I4X0(i430lx_device), I4X0(i430nx_device), I4X0(i430fx_device),
    I4X0(i430fx_rev02_device), I4X0(i430hx_device), I4X0(i430vx_device), I4X0(i430tx_device), I4X0(i440fx_device),
    I4X0(i440lx_device), I4X0(i440ex_device), I4X0(i440bx_device), I4X0(i440bx_no_agp_device), I4X0(i440gx_device),
    I4X0(i440zx_device),
    { &i440bx_agp_device, pci_bridge_rmm_snap, 0 },
#define PIIX(d) { &d, piix_rmm_snap, 0 }
    PIIX(piix_device), PIIX(piix_no_mirq_device), PIIX(piix_rev02_device), PIIX(piix3_device),
    PIIX(piix3_ioapic_device), PIIX(piix4_device), PIIX(piix4e_device), PIIX(slc90e66_device),
#define ACPI(d) { &d, NULL, sizeof(acpi_t) }
    ACPI(acpi_ali_device), ACPI(acpi_intel_device), ACPI(acpi_via_device), ACPI(acpi_via_596b_device),
    ACPI(acpi_smc_device), ACPI(acpi_sis_5582_device), ACPI(acpi_sis_5595_device),
    { &apm_device, apm_rmm_snap, 0 },
    { &apm_pci_device, apm_rmm_snap, 0 },
    { &apm_pci_acpi_device, apm_rmm_snap, 0 },
    { &piix4_smbus_device, smbus_piix4_rmm_snap, 0 },
    { &via_smbus_device, smbus_piix4_rmm_snap, 0 },
    { &w83977_device, w83977_rmm_snap, 0 },
    { &nmc93cxx_device, nmc93cxx_rmm_snap, 0 },
    { &port_92_pci_device, NULL, sizeof(port_92_t) },
    { &fdc_pii151b_device, NULL, 0 }, /* ROM only */
    { &fdc_pii158b_device, NULL, 0 },
    /* ISA/VLB Cirrus cards up to the 5429. */
#define GD(d) { &d, gd54xx_rmm_snap, 0 }
    GD(gd5401_isa_device), GD(gd5402_isa_device), GD(gd5420_isa_device), GD(gd5422_isa_device),
    GD(gd5424_vlb_device), GD(gd5426_isa_device), GD(gd5426_vlb_device), GD(gd5428_isa_device),
    GD(gd5428_vlb_device), GD(gd5429_isa_device), GD(gd5429_vlb_device),
    { &speaker_device,           speaker_rmm_snap,    0 },
    { &char_loopback_com_device, char_loopback_rmm_snap, 0 },
    { &char_loopback_lpt_device, char_loopback_rmm_snap, 0 },
    /* Named Pipe: host link, no guest state. */
    { &char_pipe_com_device,     char_pipe_rmm_snap,     0 },
    { &char_file_com_device,     char_pipe_rmm_snap,     0 },
    { &char_file_lpt_device,     char_pipe_rmm_snap,     0 },
    { &char_stdio_com_device,    char_pipe_rmm_snap,     0 },
    { &char_serial_passthrough_com_device, char_pipe_rmm_snap, 0 },
    { &char_fujinet_com_device,  char_pipe_rmm_snap,     0 },
    /* Printers: text and PostScript/PCL keep their handshake state. ESC/P renders to a bitmap and stays refused. */
    { &lpt_prt_text_device,      prt_text_rmm_snap,      0 },
    { &lpt_prt_ps_device,        prt_ps_rmm_snap,        0 },
    { &lpt_prt_pcl_device,       prt_ps_rmm_snap,        0 },
    { &char_pipe_lpt_device,     char_pipe_rmm_snap,     0 },
    /* Plain ISA SB16 with Nuked OPL3. Other SB variants and FM chips stay refused. */
    { &sb_16_device,             sb16_rmm_snap,       0 },
    { &ymf262_nuked_opl3_device, nuked_opl3_rmm_snap, 0 },
    /* SiS 5591 AGP board, ms5172: northbridge, southbridge, ACPI/PMU, USB, AGP bridge and GART, IDE, W83877. */
    { &sis_5591_1997_device,         sis_5591_rmm_snap,      0 },
    { &sis_55xx_common_device,       NULL,                   sizeof(sis_55xx_common_t) },
    { &sis_5591_5600_ide_device,     sis_5513_ide_rmm_snap,  0 },
    { &sis_5591_h2p_device,          sis_5591_h2p_rmm_snap,  0 },
    { &sis_5595_1997_p2i_device,     sis_5513_p2i_rmm_snap,  0 },
    { &sis_5595_usb_device,          sis_5572_usb_rmm_snap,  0 },
    { &sis_5595_1997_pmu_device,     sis_5595_pmu_rmm_snap,  0 },
    { &sis_5xxx_agp_device,          pci_bridge_rmm_snap,    0 },
    { &agpgart_device,               NULL,                   sizeof(agpgart_t) },
    { &sff8038i_device,              NULL,                   sizeof(sff8038i_t) },
    { &ddma_device,                  NULL,                   sizeof(ddma_t) },
    { &acpi_sis_5595_1997_device,    NULL,                   sizeof(acpi_t) },
    { &sis5595_smbus_device,         NULL,                   sizeof(smbus_sis5595_t) },
    { &usb_device,                   NULL,                   sizeof(usb_t) },
    { &ne2000_compat_device,         ne2k_rmm_snap,          0 },
    { &dp8390_device,                dp8390_rmm_snap,        0 },
    { &ati_rage128_pro_device,       rage128_rmm_snap,       0 },
    /* sio_82091aa */
    { &i82091aa_device, sio_82091aa_rmm_snap, 0 },
    /* sio_90c50 */
    { &dw90c50_device, sio_90c50_rmm_snap, 0 },
    /* sio_acc3221 */
    { &acc3221_device, sio_acc3221_rmm_snap, 0 },
    /* sio_ali5113 */
    { &ali5113_device, sio_ali5113_rmm_snap, 0 },
    /* sio_ali5123 */
    { &ali5123_device, sio_ali5123_rmm_snap, 0 },
    /* sio_cbm_io */
    { &cbm_io_device, sio_cbm_io_rmm_snap, 0 },
    /* sio_detect */
    /* sio_f82c606 */
    { &f82c606_device, sio_f82c606_rmm_snap, 0 },
    /* sio_f82c710 */
    { &f82c710_device, sio_f82c710_rmm_snap, 0 },
    { &f82c710_pc5086_device, sio_f82c710_rmm_snap, 0 },
    /* sio_fdc37c669 */
    { &fdc37c669_device, sio_fdc37c669_rmm_snap, 0 },
    /* sio_fdc37c67x */
    { &fdc37c67x_device, sio_fdc37c67x_rmm_snap, 0 },
    /* sio_fdc37c6xx */
    { &fdc37c6xx_device, sio_fdc37c6xx_rmm_snap, 0 },
    /* sio_fdc37c93x */
    { &fdc37c93x_device, sio_fdc37c93x_rmm_snap, 0 },
    /* sio_fdc37mx0x */
    { &fdc37mx0x_device, sio_fdc37mx0x_rmm_snap, 0 },
    /* sio_gm82c803ab */
    { &gm82c803ab_device, sio_gm82c803ab_rmm_snap, 0 },
    /* sio_gm82c803c */
    { &gm82c803c_device, sio_gm82c803c_rmm_snap, 0 },
    /* sio_it86x1f */
    { &it8661f_device, sio_it86x1f_rmm_snap, 0 },
    { &it8671f_device, sio_it86x1f_rmm_snap, 0 },
    /* sio_p82c604 */
    { &p82c604_device, sio_p82c604_rmm_snap, 0 },
    /* sio_pc87306 */
    { &pc87306_device, sio_pc87306_rmm_snap, 0 },
    /* sio_pc87307 */
    { &pc87307_15c_device, sio_pc87307_rmm_snap, 0 },
    { &pc87307_both_device, sio_pc87307_rmm_snap, 0 },
    { &pc87307_device, sio_pc87307_rmm_snap, 0 },
    { &pc97307_device, sio_pc87307_rmm_snap, 0 },
    /* sio_pc87309 */
    { &pc87309_device, sio_pc87309_rmm_snap, 0 },
    /* sio_pc87310 */
    { &pc87310_device, sio_pc87310_rmm_snap, 0 },
    /* sio_pc873xx */
    { &pc873xx_device, sio_pc873xx_rmm_snap, 0 },
    /* sio_um8669f */
    { &um8669f_device, sio_um8669f_rmm_snap, 0 },
    /* sio_um866x */
    { &um866x_device, sio_um866x_rmm_snap, 0 },
    /* sio_vl82c113 */
    { &vl82c113_device, sio_vl82c113_rmm_snap, 0 },
    /* sio_vt82c686 */
    { &via_vt82c686_sio_device, sio_vt82c686_rmm_snap, 0 },
    /* access_bus */
    { &access_bus_device, access_bus_rmm_snap, 0 },
    /* ast_readout */
    { &ast_readout_device, ast_readout_rmm_snap, 0 },
    /* dell_jumper */
    { &dell_jumper_device, dell_jumper_rmm_snap, 0 },
    /* hasp */
    { &lpt_hasp_savquest_device, hasp_rmm_snap, 0 },
    /* hwm_gl518sm */
    { &gl518sm_2c_device, hwm_gl518sm_rmm_snap, 0 },
    { &gl518sm_2d_device, hwm_gl518sm_rmm_snap, 0 },
    { &gl520sm_2c_device, hwm_gl518sm_rmm_snap, 0 },
    { &gl520sm_2d_device, hwm_gl518sm_rmm_snap, 0 },
    /* hwm_lm75 */
    { &lm75_1_4a_device, hwm_lm75_rmm_snap, 0 },
    { &lm75_w83781d_device, hwm_lm75_rmm_snap, 0 },
    { &lm77_0_48_device, hwm_lm75_rmm_snap, 0 },
    /* hwm_lm78 */
    { &as99127f_device, hwm_lm78_rmm_snap, 0 },
    { &as99127f_rev2_device, hwm_lm78_rmm_snap, 0 },
    { &lm78_al440lx_device, hwm_lm78_rmm_snap, 0 },
    { &lm78_device, hwm_lm78_rmm_snap, 0 },
    { &w83781d_device, hwm_lm78_rmm_snap, 0 },
    { &w83781d_p5a_device, hwm_lm78_rmm_snap, 0 },
    { &w83782d_device, hwm_lm78_rmm_snap, 0 },
    /* hwm_vt82c686 */
    { &via_vt82c686_hwm_device, hwm_vt82c686_rmm_snap, 0 },
    /* kbc_3270pc */
    { &kbc_3270pc_device, kbc_3270pc_rmm_snap, 0 },
    /* lightpen_pxl380 */
    { &mouse_pxl_380_device, lightpen_pxl380_rmm_snap, 0 },
    /* mouse_bus */
    { &mouse_logibus_device, mouse_bus_rmm_snap, 0 },
    { &mouse_logibus_onboard_device, mouse_bus_rmm_snap, 0 },
    { &mouse_msinport_device, mouse_bus_rmm_snap, 0 },
    /* mouse_ibm7690_tablet */
    { &mouse_ibm7690_touch_device, mouse_ibm7690_tablet_rmm_snap, 0 },
    /* mouse_serial */
    { &mouse_ltserial_device, mouse_serial_rmm_snap, 0 },
    { &mouse_msserial_ballpoint_device, mouse_serial_rmm_snap, 0 },
    { &mouse_msserial_device, mouse_serial_rmm_snap, 0 },
    { &mouse_mssystems_bus_device, mouse_serial_rmm_snap, 0 },
    { &mouse_mssystems_device, mouse_serial_rmm_snap, 0 },
    /* mouse_wacom_tablet */
    /* nec_mate_unk */
    { &nec_mate_unk_device, nec_mate_unk_rmm_snap, 0 },
    /* novell_cardkey */
    { &novell_keycard_device, novell_cardkey_rmm_snap, 0 },
    /* phoenix_486_jumper */
    { &phoenix_486_jumper_device, phoenix_486_jumper_rmm_snap, 0 },
    { &phoenix_486_jumper_monsoon_device, phoenix_486_jumper_rmm_snap, 0 },
    { &phoenix_486_jumper_pb400_device, phoenix_486_jumper_rmm_snap, 0 },
    { &phoenix_486_jumper_pb430_device, phoenix_486_jumper_rmm_snap, 0 },
    { &phoenix_486_jumper_pci_device, phoenix_486_jumper_rmm_snap, 0 },
    { &phoenix_486_jumper_pci_pb600_device, phoenix_486_jumper_rmm_snap, 0 },
    /* radisys_config */
    { &radisys_config_device, radisys_config_rmm_snap, 0 },
    /* smbus_ali7101 */
    { &ali7101_smbus_device, smbus_ali7101_rmm_snap, 0 },
    /* tulip_jumper */
    { &tulip_jumper_device, tulip_jumper_rmm_snap, 0 },
    { &kbc_pc82_device, kbc_xt_rmm_snap, 0 },
    { &kbc_pc_device, kbc_xt_rmm_snap, 0 },
    { &kbc_pravetz_device, kbc_xt_rmm_snap, 0 },
    { &kbc_tandy_device, kbc_xt_rmm_snap, 0 },
    { &kbc_xt_compaq_device, kbc_xt_rmm_snap, 0 },
    { &kbc_xt_device, kbc_xt_rmm_snap, 0 },
    { &kbc_xt_fe2010_device, kbc_xt_rmm_snap, 0 },
    { &kbc_xt_hyundai_device, kbc_xt_rmm_snap, 0 },
    { &kbc_xt_jukost_device, kbc_xt_rmm_snap, 0 },
    { &kbc_xt_lxt3_device, kbc_xt_rmm_snap, 0 },
    { &kbc_xt_olivetti_device, kbc_xt_rmm_snap, 0 },
    { &kbc_xt_t1x00_device, kbc_xt_rmm_snap, 0 },
    { &kbc_xt_zenith_device, kbc_xt_rmm_snap, 0 },
    { &ali5243_agp_device, pci_bridge_rmm_snap, 0 },
    { &ali5247_agp_device, pci_bridge_rmm_snap, 0 },
    { &dec21150_device, pci_bridge_rmm_snap, 0 },
    { &dec21152_device, pci_bridge_rmm_snap, 0 },
    { &i440gx_agp_device, pci_bridge_rmm_snap, 0 },
    { &i440lx_agp_device, pci_bridge_rmm_snap, 0 },
    { &via_apro_agp_device, pci_bridge_rmm_snap, 0 },
    { &via_mvp3_agp_device, pci_bridge_rmm_snap, 0 },
    { &via_vp3_agp_device, pci_bridge_rmm_snap, 0 },
    { &via_vt8601_agp_device, pci_bridge_rmm_snap, 0 },
    /* hdc_esdi_mca */
    { &esdi_integrated_device, hdc_esdi_mca_rmm_snap, 0 },
    { &esdi_ps2_device, hdc_esdi_mca_rmm_snap, 0 },
    { &esdi_upgrade_device, hdc_esdi_mca_rmm_snap, 0 },
    /* hdc_ide_ali5213 */
    { &ide_ali1489_device, hdc_ide_ali5213_rmm_snap, 0 },
    { &ide_ali5213_device, hdc_ide_ali5213_rmm_snap, 0 },
    /* hdc_ide_cmd640 */
    { &ide_cmd640_pci_device, hdc_ide_cmd640_rmm_snap, 0 },
    { &ide_cmd640_pci_legacy_only_device, hdc_ide_cmd640_rmm_snap, 0 },
    { &ide_cmd640_pci_single_channel_device, hdc_ide_cmd640_rmm_snap, 0 },
    { &ide_cmd640_pci_single_channel_legacy_only_device, hdc_ide_cmd640_rmm_snap, 0 },
    { &ide_cmd640_pci_single_channel_sec_device, hdc_ide_cmd640_rmm_snap, 0 },
    { &ide_cmd640_vlb_178_device, hdc_ide_cmd640_rmm_snap, 0 },
    { &ide_cmd640_vlb_device, hdc_ide_cmd640_rmm_snap, 0 },
    { &ide_cmd640_vlb_pri_178_device, hdc_ide_cmd640_rmm_snap, 0 },
    { &ide_cmd640_vlb_pri_device, hdc_ide_cmd640_rmm_snap, 0 },
    { &ide_cmd640_vlb_sec_178_device, hdc_ide_cmd640_rmm_snap, 0 },
    { &ide_cmd640_vlb_sec_device, hdc_ide_cmd640_rmm_snap, 0 },
    /* hdc_ide_cmd646 */
    { &ide_cmd646_device, hdc_ide_cmd646_rmm_snap, 0 },
    { &ide_cmd646_legacy_only_device, hdc_ide_cmd646_rmm_snap, 0 },
    { &ide_cmd646_single_channel_device, hdc_ide_cmd646_rmm_snap, 0 },
    { &ide_cmd646_ter_qua_device, hdc_ide_cmd646_rmm_snap, 0 },
    { &ide_cmd648_ter_qua_device, hdc_ide_cmd646_rmm_snap, 0 },
    { &ide_cmd648_ter_qua_onboard_device, hdc_ide_cmd646_rmm_snap, 0 },
    { &ide_cmd649_ter_qua_device, hdc_ide_cmd646_rmm_snap, 0 },
    /* hdc_ide_hpt366 */
    { &ide_hpt366_ter_qua_onboard_device, hdc_ide_hpt366_rmm_snap, 0 },
    /* hdc_ide_opti611 */
    { &ide_opti611_vlb_device, hdc_ide_opti611_rmm_snap, 0 },
    { &ide_opti611_vlb_sec_device, hdc_ide_opti611_rmm_snap, 0 },
    /* hdc_ide_pc87410 */
    { &ide_pc87410_device, hdc_ide_pc87410_rmm_snap, 0 },
    /* hdc_ide_rz1000 */
    { &ide_rz1000_pci_device, hdc_ide_rz1000_rmm_snap, 0 },
    { &ide_rz1000_pci_single_channel_device, hdc_ide_rz1000_rmm_snap, 0 },
    { &ide_rz1001_pci_device, hdc_ide_rz1000_rmm_snap, 0 },
    /* hdc_ide_um8673f */
    { &ide_um8673f_device, hdc_ide_um8673f_rmm_snap, 0 },
    { &ide_um8886af_device, hdc_ide_um8673f_rmm_snap, 0 },
    /* hdc_ide_w83769f */
    { &ide_w83769f_pci_34_device, hdc_ide_w83769f_rmm_snap, 0 },
    { &ide_w83769f_pci_device, hdc_ide_w83769f_rmm_snap, 0 },
    { &ide_w83769f_pci_single_channel_device, hdc_ide_w83769f_rmm_snap, 0 },
    { &ide_w83769f_vlb_34_device, hdc_ide_w83769f_rmm_snap, 0 },
    { &ide_w83769f_vlb_device, hdc_ide_w83769f_rmm_snap, 0 },
    /* hdc_st506_mca */
    { &st506_ps2_device, hdc_st506_mca_rmm_snap, 0 },
    /* net_3c501 */
    { &threec501_device, net_3c501_rmm_snap, 0 },
    /* net_3c59x_eisa */
    { &threec592_device, net_3c59x_eisa_rmm_snap, 0 },
    { &threec597_device, net_3c59x_eisa_rmm_snap, 0 },
    /* net_pe3 */
    { &pe3_device, net_pe3_rmm_snap, 0 },
    /* net_plip */
    { &plip_device, net_plip_rmm_snap, 0 },
    /* scsi_aha1740 */
    { &aha1740_device, scsi_aha1740_rmm_snap, 0 },
    { &aha1740a_device, scsi_aha1740_rmm_snap, 0 },
    { &aha1742a_device, scsi_aha1740_rmm_snap, 0 },
    { &aha1744_device, scsi_aha1740_rmm_snap, 0 },
    /* scsi_pcscsi */
    { &am53c974_pci_device, scsi_pcscsi_rmm_snap, 0 },
    { &am53c974a_pci_device, scsi_pcscsi_rmm_snap, 0 },
    { &dc390_pci_device, scsi_pcscsi_rmm_snap, 0 },
    { &ncr53c90a_mca_device, scsi_pcscsi_rmm_snap, 0 },
    /* scsi_spock */
    { &spock_device, scsi_spock_rmm_snap, 0 },
    { &tribble_device, scsi_spock_rmm_snap, 0 },
    /* ne2000 family: one nic_t each, plus the dp8390 core row. */
    { &de220p_device, ne2k_rmm_snap, 0 },
    { &ethernext_mc_device, ne2k_rmm_snap, 0 },
    { &ne1000_compat_device, ne2k_rmm_snap, 0 },
    { &ne1000_device, ne2k_rmm_snap, 0 },
    { &ne2000_compat_8bit_device, ne2k_rmm_snap, 0 },
    { &ne2000_device, ne2k_rmm_snap, 0 },
    { &rtl8019as_pnp_device, ne2k_rmm_snap, 0 },
    { &rtl8029as_device, ne2k_rmm_snap, 0 },
    /* acc2168 */
    { &acc2168_device, acc2168_rmm_snap, 0 },
    /* ali1409 */
    { &ali1409_device, ali1409_rmm_snap, 0 },
    /* ali1429 */
    { &ali1429_device, ali1429_rmm_snap, 0 },
    { &ali1429g_device, ali1429_rmm_snap, 0 },
    /* ali1435 */
    { &ali1435_device, ali1435_rmm_snap, 0 },
    /* ali1489 */
    { &ali1489_device, ali1489_rmm_snap, 0 },
    /* ali1531 */
    { &ali1531_device, ali1531_rmm_snap, 0 },
    /* ali1543 */
    { &ali1543_device, ali1543_rmm_snap, 0 },
    { &ali1543c_device, ali1543_rmm_snap, 0 },
    /* ali1621 */
    { &ali1621_device, ali1621_rmm_snap, 0 },
    /* ali6117 */
    { &ali1217_device, ali6117_rmm_snap, 0 },
    { &ali6117d_device, ali6117_rmm_snap, 0 },
    /* asus48625 */
    { &asus48625_device, asus48625_rmm_snap, 0 },
    /* compaq */
    { &compaq_device, compaq_rmm_snap, 0 },
    /* contaq_82c59x */
    { &contaq_82c596a_device, contaq_82c59x_rmm_snap, 0 },
    { &contaq_82c597_device, contaq_82c59x_rmm_snap, 0 },
    /* cs4031 */
    { &cs4031_device, cs4031_rmm_snap, 0 },
    /* cs8230 */
    { &cs8230_device, cs8230_rmm_snap, 0 },
    /* et6000 */
    /* gc100 */
    { &gc100_device, gc100_rmm_snap, 0 },
    { &gc100a_device, gc100_rmm_snap, 0 },
    /* grid1520 */
    { &grid1520_device, grid1520_rmm_snap, 0 },
    /* ims8848 */
    { &ims8848_device, ims8848_rmm_snap, 0 },
    /* intel_420ex */
    { &i420ex_device, intel_420ex_rmm_snap, 0 },
    { &i420ex_ide_device, intel_420ex_rmm_snap, 0 },
    /* intel_82335 */
    { &intel_82335_device, intel_82335_rmm_snap, 0 },
    /* intel_i450kx */
    { &i450kx_device, intel_i450kx_rmm_snap, 0 },
    /* intel_sio */
    { &sio_device, intel_sio_rmm_snap, 0 },
    { &sio_zb_device, intel_sio_rmm_snap, 0 },
    /* isa486c */
    { &isa486c_device, isa486c_rmm_snap, 0 },
    /* olivetti_eva */
    { &olivetti_eva_device, olivetti_eva_rmm_snap, 0 },
    /* opti291 */
    { &opti291_device, opti291_rmm_snap, 0 },
    /* opti391 */
    { &opti381_device, opti391_rmm_snap, 0 },
    { &opti391_device, opti391_rmm_snap, 0 },
    { &opti481_device, opti391_rmm_snap, 0 },
    /* opti495 */
    { &opti493_device, opti495_rmm_snap, 0 },
    { &opti495slc_device, opti495_rmm_snap, 0 },
    { &opti495sx_device, opti495_rmm_snap, 0 },
    /* opti499 */
    { &opti499_device, opti499_rmm_snap, 0 },
    /* opti5x7 */
    { &opti5x7_device, opti5x7_rmm_snap, 0 },
    { &opti5x7_pci_device, opti5x7_rmm_snap, 0 },
    /* opti602 */
    { &opti601_device, opti602_rmm_snap, 0 },
    { &opti602_device, opti602_rmm_snap, 0 },
    /* opti822 */
    { &opti822_device, opti822_rmm_snap, 0 },
    /* opti895 */
    { &opti802g_device, opti895_rmm_snap, 0 },
    { &opti802g_pci_device, opti895_rmm_snap, 0 },
    { &opti895_device, opti895_rmm_snap, 0 },
    /* philips */
    { &philips_device, philips_rmm_snap, 0 },
    /* sanyo */
    { &sanyo_device, sanyo_rmm_snap, 0 },
    /* sis_5511_h2p */
    { &sis_5511_h2p_device, sis_5511_h2p_rmm_snap, 0 },
    /* sis_5571_h2p */
    { &sis_5571_h2p_device, sis_5571_h2p_rmm_snap, 0 },
    /* sis_5571_old */
    /* sis_85c411 */
    { &sis_85c411_device, sis_85c411_rmm_snap, 0 },
    /* sl82c461 */
    { &sl82c461_device, sl82c461_rmm_snap, 0 },
    /* umc_8886 */
    { &umc_8886af_device, umc_8886_rmm_snap, 0 },
    { &umc_8886bf_device, umc_8886_rmm_snap, 0 },
    { &umc_8886f_device, umc_8886_rmm_snap, 0 },
    /* umc_8890 */
    { &umc_8890_device, umc_8890_rmm_snap, 0 },
    /* umc_hb4 */
    { &umc_hb4_device, umc_hb4_rmm_snap, 0 },
    /* via_apollo */
    { &amd640_device, via_apollo_rmm_snap, 0 },
    { &via_apro133_device, via_apollo_rmm_snap, 0 },
    { &via_apro133a_device, via_apollo_rmm_snap, 0 },
    { &via_apro_device, via_apollo_rmm_snap, 0 },
    { &via_mvp3_device, via_apollo_rmm_snap, 0 },
    { &via_vp3_device, via_apollo_rmm_snap, 0 },
    { &via_vpx_device, via_apollo_rmm_snap, 0 },
    { &via_vt8601_device, via_apollo_rmm_snap, 0 },
    /* via_vt82c49x */
    { &via_vt82c49x_device, via_vt82c49x_rmm_snap, 0 },
    { &via_vt82c49x_ide_device, via_vt82c49x_rmm_snap, 0 },
    { &via_vt82c49x_pci_device, via_vt82c49x_rmm_snap, 0 },
    { &via_vt82c49x_pci_ide_device, via_vt82c49x_rmm_snap, 0 },
    /* via_vt82c505 */
    { &via_vt82c505_device, via_vt82c505_rmm_snap, 0 },
    /* m_ps1 */
    { &ps1_2011_device, m_ps1_rmm_snap, 0 },
    { &ps1_2121_device, m_ps1_rmm_snap, 0 },
    /* m_xt_jukost */
    { &jukopc_device, m_xt_jukost_rmm_snap, 0 },
    /* m_at_tandy_vis */
    { &tvis_device, m_at_tandy_vis_rmm_snap, 0 },
    /* vid_clockgen_ch9201 */
    { &ch9201_device, vid_clockgen_ch9201_rmm_snap, 0 },
    /* vid_clockgen_ch9204 */
    { &ch9204_device, vid_clockgen_ch9204_rmm_snap, 0 },
    /* vid_clockgen_icd2047 */
    { &icd2047_20_device, vid_clockgen_icd2047_rmm_snap, 0 },
    /* vid_clockgen_ics1494 */
    { &ics1494m_540_device, vid_clockgen_ics1494_rmm_snap, 0 },
    /* vid_clockgen_ics2494 */
    { &ati18810_28800_device, vid_clockgen_ics2494_rmm_snap, 0 },
    { &ati18810_mach32_device, vid_clockgen_ics2494_rmm_snap, 0 },
    { &ati18811_0_28800_device, vid_clockgen_ics2494_rmm_snap, 0 },
    { &ati18811_0_mach32_device, vid_clockgen_ics2494_rmm_snap, 0 },
    { &ati18811_1_28800_device, vid_clockgen_ics2494_rmm_snap, 0 },
    { &ati18811_1_mach32_device, vid_clockgen_ics2494_rmm_snap, 0 },
    { &ics2494an_304_device, vid_clockgen_ics2494_rmm_snap, 0 },
    { &ics2494an_305_device, vid_clockgen_ics2494_rmm_snap, 0 },
    { &ics2494an_318_device, vid_clockgen_ics2494_rmm_snap, 0 },
    { &ics2494an_324_device, vid_clockgen_ics2494_rmm_snap, 0 },
    /* vid_clockgen_ics2595 */
    { &ics2595_device, vid_clockgen_ics2595_rmm_snap, 0 },
    /* vid_clockgen_ics90c64a */
    { &ics90c64a_903_device, vid_clockgen_ics90c64a_rmm_snap, 0 },
    /* vid_ramdac_ati68860 */
    { &ati68860_ramdac_device, vid_ramdac_ati68860_rmm_snap, 0 },
    /* vid_ramdac_ati68875 */
    { &ati68875_ramdac_device, vid_ramdac_ati68875_rmm_snap, 0 },
    /* vid_ramdac_att20c49x */
    { &att490_ramdac_device, vid_ramdac_att20c49x_rmm_snap, 0 },
    { &att491_ramdac_device, vid_ramdac_att20c49x_rmm_snap, 0 },
    { &att492_ramdac_device, vid_ramdac_att20c49x_rmm_snap, 0 },
    /* vid_ramdac_att2xc498 */
    { &att498_ramdac_device, vid_ramdac_att2xc498_rmm_snap, 0 },
    /* vid_ramdac_bt481 */
    { &bt481_ramdac_device, vid_ramdac_bt481_rmm_snap, 0 },
    /* vid_ramdac_bt48x */
    { &att20c504_ramdac_device, vid_ramdac_bt48x_rmm_snap, 0 },
    { &att20c505_ramdac_device, vid_ramdac_bt48x_rmm_snap, 0 },
    { &bt484_ramdac_device, vid_ramdac_bt48x_rmm_snap, 0 },
    { &bt485_ramdac_device, vid_ramdac_bt48x_rmm_snap, 0 },
    { &bt485a_ramdac_device, vid_ramdac_bt48x_rmm_snap, 0 },
    /* vid_ramdac_mu9c1880 */
    { &mu9c1880_ramdac_device, vid_ramdac_mu9c1880_rmm_snap, 0 },
    /* vid_ramdac_sc1148x */
    { &sc11483_ramdac_device, vid_ramdac_sc1148x_rmm_snap, 0 },
    { &sc11484_nors2_ramdac_device, vid_ramdac_sc1148x_rmm_snap, 0 },
    { &sc11484_ramdac_device, vid_ramdac_sc1148x_rmm_snap, 0 },
    { &sc11486_ramdac_device, vid_ramdac_sc1148x_rmm_snap, 0 },
    { &sc11487_nors2_ramdac_device, vid_ramdac_sc1148x_rmm_snap, 0 },
    { &sc11487_ramdac_device, vid_ramdac_sc1148x_rmm_snap, 0 },
    /* vid_ramdac_sc1502x */
    { &sc1502x_ramdac_device, vid_ramdac_sc1502x_rmm_snap, 0 },
    { &sc1502x_rs2_ramdac_device, vid_ramdac_sc1502x_rmm_snap, 0 },
    /* vid_ramdac_sdac */
    { &gendac_ramdac_device, vid_ramdac_sdac_rmm_snap, 0 },
    { &sdac_ramdac_device, vid_ramdac_sdac_rmm_snap, 0 },
    { &tseng_ics5301_ramdac_device, vid_ramdac_sdac_rmm_snap, 0 },
    { &tseng_ics5341_ramdac_device, vid_ramdac_sdac_rmm_snap, 0 },
    /* vid_ramdac_stg1702 */
    { &stg1702_ramdac_device, vid_ramdac_stg1702_rmm_snap, 0 },
    { &stg1703_ramdac_device, vid_ramdac_stg1702_rmm_snap, 0 },
    /* vid_ramdac_tkd8001 */
    { &tkd8001_ramdac_device, vid_ramdac_tkd8001_rmm_snap, 0 },
    /* vid_ati_rage128_maxx */
    { &ati_rage128_maxx_device, vid_ati_rage128_maxx_rmm_snap, 0 },
    /* vid_ibm_plasma */
    { &ibm_plasma_vga_device, vid_ibm_plasma_rmm_snap, 0 },
    /* No init function: nothing is allocated, so there is no state (machine option holders and the like). */
#define NOSTATE(d) { &d, NULL, 0 }
    NOSTATE(device_external), NOSTATE(device_internal), NOSTATE(device_none), NOSTATE(vid_pc2086_device),
    NOSTATE(vid_pc3086_device), NOSTATE(ibmat_device), NOSTATE(ibmxt286_device),
    NOSTATE(pc900_device), NOSTATE(dataexpert386wb_device), NOSTATE(deskpro386_device),
    NOSTATE(fic4386vcv_device), NOSTATE(opti495_ami_device), NOSTATE(c325ax_device),
    NOSTATE(dells333sl_device), NOSTATE(pbl300sx_device), NOSTATE(at_54tdp_device), NOSTATE(d823_device),
    NOSTATE(td3_device), NOSTATE(acerv62x_device), NOSTATE(al440lx_device), NOSTATE(ax6bc_device),
    NOSTATE(be6ii_device), NOSTATE(bravomst6233_device), NOSTATE(bx6_device), NOSTATE(como_device),
    NOSTATE(ga686_device), NOSTATE(in440ex_device), NOSTATE(in440exd_device), NOSTATE(lx6_device),
    NOSTATE(ms6117_device), NOSTATE(ms6119_device), NOSTATE(ms6147_device), NOSTATE(ms6199va_device),
    NOSTATE(optiplexe1_device), NOSTATE(optiplexgx1_device), NOSTATE(p3v133_device), NOSTATE(p6kdi_device),
    NOSTATE(p6sba_device), NOSTATE(pavilion85xx_device), NOSTATE(rc440bx_device), NOSTATE(s1846_device),
    NOSTATE(se440bx2_device), NOSTATE(vei8_device), NOSTATE(m726mrt_device), NOSTATE(prosignias31x_device),
    NOSTATE(s2dge_device), NOSTATE(precisionv486_device), NOSTATE(pb410a_device), NOSTATE(j403tg_device),
    NOSTATE(win471t_device), NOSTATE(em440_device), NOSTATE(ms6318_device), NOSTATE(hot433a_device),
    NOSTATE(m4li_device), NOSTATE(pb450_device), NOSTATE(pc330_6573_device), NOSTATE(pci400cb_device),
    NOSTATE(sb486pv_device), NOSTATE(zida4dps_device), NOSTATE(batman_device), NOSTATE(p5mp3_device),
    NOSTATE(pci58pl_device), NOSTATE(v12p_device), NOSTATE(bravoms586_device), NOSTATE(d842_device),
    NOSTATE(m54si_device), NOSTATE(plato_device), NOSTATE(powermatev_device), NOSTATE(pt2000_device),
    NOSTATE(zappa_device), NOSTATE(an430tx_device), NOSTATE(awardeval_device), NOSTATE(cu430hx_device),
    NOSTATE(lgibmx52_device), NOSTATE(m5ata_device), NOSTATE(m7shi_device), NOSTATE(ms5146_device),
    NOSTATE(ms5156_device), NOSTATE(p5vxb_device), NOSTATE(pb810_device), NOSTATE(r534f_device),
    NOSTATE(tc430hx_device), NOSTATE(txp4x_device), NOSTATE(ap5s_device), NOSTATE(c5sbm2_device),
    NOSTATE(d943_device), NOSTATE(fmb_device), NOSTATE(monaco_device), NOSTATE(ms5119_device),
    NOSTATE(ms5124_device), NOSTATE(p54tp4xe_device), NOSTATE(thor_device), NOSTATE(vectra52_device),
    NOSTATE(aurora_device), NOSTATE(ficpo6000_device), NOSTATE(vs440fx_device), NOSTATE(zida6dxp_device),
    NOSTATE(cobalt3k_carmel_device), NOSTATE(cobalt3k_pacifica_device), NOSTATE(delhi3_device),
    NOSTATE(g5x_device), NOSTATE(ga5smm_device), NOSTATE(in530_device), NOSTATE(ms5169_device),
    NOSTATE(arb1479_device), NOSTATE(ibm7690_device), NOSTATE(ps2_m25_device),
    NOSTATE(ps2_m30_286_device), NOSTATE(ps2_m30_device), NOSTATE(ps2_model_50_device), NOSTATE(v86p_device),
    NOSTATE(dtk_device), NOSTATE(ibmpc82_device), NOSTATE(ibmpc_device), NOSTATE(ibmxt86_device),
    NOSTATE(ibmxt_device), NOSTATE(ibmxt_inboard386_device), NOSTATE(pc500_device),
    NOSTATE(pc500plus_device), NOSTATE(pc700_device), NOSTATE(to16_device), NOSTATE(vendex_device),
    /* Small board-level devices. */
    { &ioapic_device,                ioapic_rmm_snap,    0 },
    { &ioapic_ami_device,            ioapic_rmm_snap,    0 },
    { &pci_dummy_device,             pci_dummy_rmm_snap, 0 },
    { &ps2_nvr_2kb_device,           ps2_nvr_rmm_snap,   0 },
    { &ps2_nvr_8kb_device,           ps2_nvr_rmm_snap,   0 },
    { &port_6x_olivetti_device,      port_6x_rmm_snap,   0 },
    { &port_6x_ps2_device,           port_6x_rmm_snap,   0 },
    { &port_6x_xi8088_device,        port_6x_rmm_snap,   0 },
    { &port_92_inv_device,           NULL,               sizeof(port_92_t) },
    { &port_92_key_device,           NULL,               sizeof(port_92_t) },
    { &port_92_word_device,          NULL,               sizeof(port_92_t) },
    { &gameport_pnp_device,          gameport_rmm_snap,  0 },
    { &gameport_pnp_1io_device,      gameport_rmm_snap,  0 },
    { &gameport_pnp_6io_device,      gameport_rmm_snap,  0 },
    { &gameport_tm_acm_device,       NULL,               0 }, /* two port pointers; the ports are their own devices */
    /* vid_ati18800 */
    { &ati18800_device, vid_ati18800_rmm_snap, 0 },
    { &ati18800_vga88_device, vid_ati18800_rmm_snap, 0 },
    { &ati18800_wonder_device, vid_ati18800_rmm_snap, 0 },
    /* vid_ati28800 */
    { &ati28800_5_device, vid_ati28800_rmm_snap, 0 },
    { &ati28800_wonder1024d_xl_plus_device, vid_ati28800_rmm_snap, 0 },
    { &ati28800k_device, vid_ati28800_rmm_snap, 0 },
    { &ati28800k_spc4620p_device, vid_ati28800_rmm_snap, 0 },
    { &ati28800k_spc6033p_device, vid_ati28800_rmm_snap, 0 },
    /* vid_ati_mach64 */
    { &mach64ct_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64ct_onboard_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64gtb_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64gtb_onboard_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64gx_isa_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64gx_pci_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64gx_pci_onboard_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64gx_vlb_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64gx_vlb_onboard_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64gx_xpression_vlb_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64vt2_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64vt3_onboard_device, vid_ati_mach64_rmm_snap, 0 },
    { &mach64vt_device, vid_ati_mach64_rmm_snap, 0 },
    /* vid_bochs_vbe */
    { &bochs_svga_device, vid_bochs_vbe_rmm_snap, 0 },
    /* vid_chips_69000 */
    { &chips_69000_device, vid_chips_69000_rmm_snap, 0 },
    { &chips_69000_onboard_device, vid_chips_69000_rmm_snap, 0 },
    /* vid_cl54xx */
    { &gd5401_onboard_device, gd54xx_rmm_snap, 0 },
    { &gd5402_onboard_commodore_device, gd54xx_rmm_snap, 0 },
    { &gd5402_onboard_device, gd54xx_rmm_snap, 0 },
    { &gd5420_onboard_device, gd54xx_rmm_snap, 0 },
    { &gd5424_onboard_device, gd54xx_rmm_snap, 0 },
    { &gd5426_mca_device, gd54xx_rmm_snap, 0 },
    { &gd5426_onboard_device, gd54xx_rmm_snap, 0 },
    { &gd5426_onboard_isa_device, gd54xx_rmm_snap, 0 },
    { &gd5428_mca_jp_device, gd54xx_rmm_snap, 0 },
    { &gd5428_mca_us_device, gd54xx_rmm_snap, 0 },
    { &gd5428_onboard_device, gd54xx_rmm_snap, 0 },
    { &gd5428_onboard_vlb_device, gd54xx_rmm_snap, 0 },
    { &gd5428_vlb_onboard_device, gd54xx_rmm_snap, 0 },
    { &gd5428_vlb_onboard_pb450_device, gd54xx_rmm_snap, 0 },
    { &gd5428_vlb_onboard_tandy_device, gd54xx_rmm_snap, 0 },
    { &gd5430_onboard_pci_device, gd54xx_rmm_snap, 0 },
    { &gd5430_onboard_vlb_device, gd54xx_rmm_snap, 0 },
    { &gd5430_pci_device, gd54xx_rmm_snap, 0 },
    { &gd5430_vlb_device, gd54xx_rmm_snap, 0 },
    { &gd5434_isa_device, gd54xx_rmm_snap, 0 },
    { &gd5434_onboard_pci_device, gd54xx_rmm_snap, 0 },
    { &gd5434_pci_device, gd54xx_rmm_snap, 0 },
    { &gd5434_vlb_device, gd54xx_rmm_snap, 0 },
    { &gd5436_onboard_pci_device, gd54xx_rmm_snap, 0 },
    { &gd5436_onboard_pci_ics_device, gd54xx_rmm_snap, 0 },
    { &gd5436_pci_device, gd54xx_rmm_snap, 0 },
    { &gd5440_onboard_pci_device, gd54xx_rmm_snap, 0 },
    { &gd5440_pci_device, gd54xx_rmm_snap, 0 },
    { &gd5446_pci_device, gd54xx_rmm_snap, 0 },
    { &gd5480_pci_device, gd54xx_rmm_snap, 0 },
    /* vid_et3000 */
    { &et3000_isa_device, vid_et3000_rmm_snap, 0 },
    /* vid_et4000 */
    { &et4000_isa_device, vid_et4000_rmm_snap, 0 },
    { &et4000_mca_device, vid_et4000_rmm_snap, 0 },
    /* vid_et4000w32 */
    { &et4000w32_isa_device, vid_et4000w32_rmm_snap, 0 },
    { &et4000w32_onboard_device, vid_et4000w32_rmm_snap, 0 },
    { &et4000w32_vlb_device, vid_et4000w32_rmm_snap, 0 },
    { &et4000w32i_isa_device, vid_et4000w32_rmm_snap, 0 },
    { &et4000w32i_vlb_device, vid_et4000w32_rmm_snap, 0 },
    { &et4000w32p_pci_device, vid_et4000w32_rmm_snap, 0 },
    { &et4000w32p_vlb_device, vid_et4000w32_rmm_snap, 0 },
    /* vid_ht216 */
    { &g2_gc205_device, vid_ht216_rmm_snap, 0 },
    { &ht216_32_pb410a_device, vid_ht216_rmm_snap, 0 },
    { &ht216_32_standalone_device, vid_ht216_rmm_snap, 0 },
    { &ht216_standalone_device, vid_ht216_rmm_snap, 0 },
    { &radius_svga_multiview_mca_device, vid_ht216_rmm_snap, 0 },
    { &v7_vga_1024i_device, vid_ht216_rmm_snap, 0 },
    { &v7_vram_2_ergo_device, vid_ht216_rmm_snap, 0 },
    /* vid_mga */
    { &millennium_device, vid_mga_rmm_snap, 0 },
    { &millennium_ii_agp_device, vid_mga_rmm_snap, 0 },
    { &millennium_ii_device, vid_mga_rmm_snap, 0 },
    { &mystique_220_device, vid_mga_rmm_snap, 0 },
    { &mystique_device, vid_mga_rmm_snap, 0 },
    /* vid_oak_oti */
    { &oti037_pbl300sx_device, vid_oak_oti_rmm_snap, 0 },
    { &oti037c_device, vid_oak_oti_rmm_snap, 0 },
    { &oti067_ama932j_device, vid_oak_oti_rmm_snap, 0 },
    { &oti067_device, vid_oak_oti_rmm_snap, 0 },
    { &oti067_m300_device, vid_oak_oti_rmm_snap, 0 },
    { &oti077_acer100t_device, vid_oak_oti_rmm_snap, 0 },
    { &oti077_device, vid_oak_oti_rmm_snap, 0 },
    { &oti077_pb400_device, vid_oak_oti_rmm_snap, 0 },
    { &oti077_pcs44c_device, vid_oak_oti_rmm_snap, 0 },
    /* vid_paradise */
    { &paradise_pvga1a_device, vid_paradise_rmm_snap, 0 },
    { &paradise_pvga1a_ncr3302_device, vid_paradise_rmm_snap, 0 },
    { &paradise_pvga1a_pc2086_device, vid_paradise_rmm_snap, 0 },
    { &paradise_pvga1a_pc3086_device, vid_paradise_rmm_snap, 0 },
    { &paradise_wd90c11_device, vid_paradise_rmm_snap, 0 },
    { &paradise_wd90c11_megapc_device, vid_paradise_rmm_snap, 0 },
    { &paradise_wd90c20_5535s_device, vid_paradise_rmm_snap, 0 },
    { &paradise_wd90c30_device, vid_paradise_rmm_snap, 0 },
    { &paradise_wd90c31_device, vid_paradise_rmm_snap, 0 },
    { &paradise_wd90c31_onboard_device, vid_paradise_rmm_snap, 0 },
    /* vid_rtg310x */
    { &realtek_rtg3105_device, vid_rtg310x_rmm_snap, 0 },
    { &realtek_rtg3106_device, vid_rtg310x_rmm_snap, 0 },
    /* vid_s3 */
    { &s3_86c801_isa_device, vid_s3_rmm_snap, 0 },
    { &s3_86c805_isa_device, vid_s3_rmm_snap, 0 },
    { &s3_86c805_onboard_vlb_device, vid_s3_rmm_snap, 0 },
    { &s3_86c805_vlb_device, vid_s3_rmm_snap, 0 },
    { &s3_86c911_isa_device, vid_s3_rmm_snap, 0 },
    { &s3_86c924_isa_device, vid_s3_rmm_snap, 0 },
    { &s3_86c928_isa_device, vid_s3_rmm_snap, 0 },
    { &s3_86c928_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_86c928_vlb_device, vid_s3_rmm_snap, 0 },
    { &s3_trio32_onboard_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_trio32_onboard_vlb_device, vid_s3_rmm_snap, 0 },
    { &s3_trio32_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_trio32_vlb_device, vid_s3_rmm_snap, 0 },
    { &s3_trio64_onboard_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_trio64_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_trio64_vlb_device, vid_s3_rmm_snap, 0 },
    { &s3_trio64v2dx_onboard_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_trio64v2dx_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_trio64vplus_onboard_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_trio64vplus_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_trio64vplus_vlb_device, vid_s3_rmm_snap, 0 },
    { &s3_vision864_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_vision864_vlb_device, vid_s3_rmm_snap, 0 },
    { &s3_vision868_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_vision964_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_vision964_vlb_device, vid_s3_rmm_snap, 0 },
    { &s3_vision968_pci_device, vid_s3_rmm_snap, 0 },
    { &s3_vision968_vlb_device, vid_s3_rmm_snap, 0 },
    /* vid_s3_virge */
    { &s3_trio3d2x_agp_device, vid_s3_virge_rmm_snap, 0 },
    { &s3_trio3d2x_pci_device, vid_s3_virge_rmm_snap, 0 },
    { &s3_virge_325_onboard_pci_device, vid_s3_virge_rmm_snap, 0 },
    { &s3_virge_375_onboard_pci_device, vid_s3_virge_rmm_snap, 0 },
    { &s3_virge_dx_pci_device, vid_s3_virge_rmm_snap, 0 },
    { &s3_virge_gx2_agp_device, vid_s3_virge_rmm_snap, 0 },
    { &s3_virge_gx2_pci_device, vid_s3_virge_rmm_snap, 0 },
    { &s3_virge_gx_pci_device, vid_s3_virge_rmm_snap, 0 },
    { &s3_virge_pci_device, vid_s3_virge_rmm_snap, 0 },
    { &s3_virge_vx_pci_device, vid_s3_virge_rmm_snap, 0 },
    /* vid_sis6306 */
    { &sis6306_onboard_device, vid_sis6306_rmm_snap, 0 },
    /* vid_tandy_vis */
    { &tvis_vid_device, vid_tandy_vis_rmm_snap, 0 },
    /* vid_tgui9440 */
    { &tgui9400cxi_device, vid_tgui9440_rmm_snap, 0 },
    { &tgui9440_onboard_pci_device, vid_tgui9440_rmm_snap, 0 },
    { &tgui9440_pci_device, vid_tgui9440_rmm_snap, 0 },
    { &tgui9440_vlb_device, vid_tgui9440_rmm_snap, 0 },
    { &tgui9660_onboard_pci_device, vid_tgui9440_rmm_snap, 0 },
    { &tgui9660_pci_device, vid_tgui9440_rmm_snap, 0 },
    { &tgui9680_pci_device, vid_tgui9440_rmm_snap, 0 },
    /* vid_ti_cf62011 */
    { &ibm_ps1_2121_device, vid_ti_cf62011_rmm_snap, 0 },
    /* vid_tvga */
    { &tvga8900_device, vid_tvga_rmm_snap, 0 },
    { &tvga8900d_onboard_device, vid_tvga_rmm_snap, 0 },
    { &tvga9000b_device, vid_tvga_rmm_snap, 0 },
    /* vid_vga */
    { &ps1vga_device, vga_rmm_snap, 0 },
    { &ps1vga_mca_device, vga_rmm_snap, 0 },
    /* vid_voodoo_banshee */
    { &velocity_100_agp_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &velocity_200_agp_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &voodoo_3_1000_agp_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &voodoo_3_1000_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &voodoo_3_2000_agp_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &voodoo_3_2000_agp_onboard_8m_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &voodoo_3_2000_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &voodoo_3_3000_agp_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &voodoo_3_3000_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &voodoo_3_3500_agp_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &voodoo_device, voodoo_rmm_snap, 0 },
    { &ibm5140_device, ibm5140_rmm_snap, 0 },
    { &modem_device, modem_rmm_snap, 0 },
    { &lpt_prt_escp_device, prt_escp_rmm_snap, 0 },
    { &cassette_device, cassette_rmm_snap, 0 },
    { &pcjx_device, pcjx_rmm_snap, 0 },
    { &pcjr_device, pcjr_rmm_snap, 0 },
    { &m24_kbd_device, m24_kbd_rmm_snap, 0 },
    { &voodoo_banshee_agp_device, vid_voodoo_banshee_rmm_snap, 0 },
    { &voodoo_banshee_pci_device, vid_voodoo_banshee_rmm_snap, 0 },
    /* vid_cga */
    { &cga_device, vid_cga_rmm_snap, 0 },
    { &cga_pravetz_device, vid_cga_rmm_snap, 0 },
    { &mouse_cga_lightpen_device, vid_cga_rmm_snap, 0 },
    /* vid_cga_colorplus */
    { &colorplus_device, vid_cga_colorplus_rmm_snap, 0 },
    /* vid_cga_compaq */
    { &compaq_cga_2_device, vid_cga_compaq_rmm_snap, 0 },
    { &compaq_cga_device, vid_cga_compaq_rmm_snap, 0 },
    /* vid_cga_compaq_plasma */
    { &compaq_plasma_device, vid_cga_compaq_plasma_rmm_snap, 0 },
    /* vid_cga_ncr */
    { &nga_device, vid_cga_ncr_rmm_snap, 0 },
    /* vid_cga_olivetti */
    { &ogc_device, vid_cga_olivetti_rmm_snap, 0 },
    { &ogc_m24_device, vid_cga_olivetti_rmm_snap, 0 },
    /* vid_cga_quadcolor */
    { &quadcolor_device, vid_cga_quadcolor_rmm_snap, 0 },
    /* vid_cga_toshiba_t1000 */
    { &t1000_video_device, vid_cga_toshiba_t1000_rmm_snap, 0 },
    { &t1200_video_device, vid_cga_toshiba_t1000_rmm_snap, 0 },
    /* vid_cga_toshiba_t3100e */
    { &t3100e_device, vid_cga_toshiba_t3100e_rmm_snap, 0 },
    /* vid_cga_v6355 */
    { &v6355d_device, vid_cga_v6355_rmm_snap, 0 },
    /* vid_f82c425 */
    { &f82c425_video_device, vid_f82c425_rmm_snap, 0 },
    /* vid_genius */
    { &genius_device, vid_genius_rmm_snap, 0 },
    /* vid_hercules */
    { &hercules_device, vid_hercules_rmm_snap, 0 },
    /* vid_hercules_incolor */
    { &incolor_device, vid_hercules_incolor_rmm_snap, 0 },
    /* vid_hercules_plus */
    { &herculesplus_device, vid_hercules_plus_rmm_snap, 0 },
    /* vid_mda */
    { &mda_device, vid_mda_rmm_snap, 0 },
    /* vid_sigma */
    { &sigma_device, vid_sigma_rmm_snap, 0 },
    /* vid_wy700 */
    { &wy700_device, vid_wy700_rmm_snap, 0 },
    /* vid_3270pc */
    { &ibm3270pc_vid_device, vid_3270pc_rmm_snap, 0 },
    /* Every sb_t-based Sound Blaster (AWE and Goldfinch excluded). */
#define SBC(d) { &d, sb16_rmm_snap, 0 }
    SBC(thunderboard_device), SBC(sb_1_device), SBC(sb_15_device), SBC(sb_mcv_device), SBC(sb_2_device),
    SBC(sb_pro_v1_device), SBC(sb_pro_v2_device), SBC(sb_pro_mcv_device), SBC(sb_pro_compat_device),
    SBC(sb_vibra16c_onboard_device), SBC(sb_vibra16c_device), SBC(sb_vibra16cl_onboard_device),
    SBC(sb_vibra16cl_device), SBC(sb_vibra16s_onboard_device), SBC(sb_vibra16s_device),
    SBC(sb_vibra16xv_onboard_device), SBC(sb_vibra16xv_device), SBC(sb_16_reply_mca_device),
    SBC(sb_16_pnp_device), SBC(sb_16_pnp_ide_device), SBC(sb_16_compat_device),
    SBC(sb_16_compat_nompu_device), SBC(ess_488_device), SBC(ess_1488_device), SBC(ess_688_device),
    SBC(ess_ess0100_pnp_device), SBC(ess_1688_device), SBC(ess_1688_compaq_device),
    SBC(ess_ess0102_pnp_device), SBC(ess_ess0968_pnp_688_device), SBC(ess_ess0968_pnp_device),
    SBC(ess_soundpiper_16_mca_device), SBC(ess_soundpiper_32_mca_device), SBC(ess_chipchat_16_mca_device),
    SBC(ess_1788_device), SBC(ess_1888_device), SBC(ess_1888_compaq_device), SBC(ess_1887_device),
    SBC(ess_1868_device), SBC(ess_1869_device), SBC(ess_1869_onboard_device),
    /* snd_adlib */
    { &adlib_device, snd_adlib_rmm_snap, 0 },
    { &adlib_mca_device, snd_adlib_rmm_snap, 0 },
    /* snd_cms */
    { &cms_device, snd_cms_rmm_snap, 0 },
    /* snd_covox */
    { &isadacr0_device, snd_covox_rmm_snap, 0 },
    { &isadacr1_device, snd_covox_rmm_snap, 0 },
    { &soundman_device, snd_covox_rmm_snap, 0 },
    { &soundmasterplus_device, snd_covox_rmm_snap, 0 },
    { &voicemasterkey_device, snd_covox_rmm_snap, 0 },
    /* snd_lpt_dac */
    { &lpt_dac_device, snd_lpt_dac_rmm_snap, 0 },
    { &lpt_dac_ftl_device, snd_lpt_dac_rmm_snap, 0 },
    { &lpt_dac_soundjr_device, snd_lpt_dac_rmm_snap, 0 },
    { &lpt_dac_stereo_device, snd_lpt_dac_rmm_snap, 0 },
    /* snd_lpt_dss */
    { &dss_device, snd_lpt_dss_rmm_snap, 0 },
    /* snd_sn76489 */
    { &ncr8496_device, snd_sn76489_rmm_snap, 0 },
    { &sn76489_device, snd_sn76489_rmm_snap, 0 },
    { &tndy_device, snd_sn76489_rmm_snap, 0 },
    /* snd_ps1 */
    { &ps1snd_device, snd_ps1_rmm_snap, 0 },
    /* snd_pssj */
    { &pssj_1e0_device, snd_pssj_rmm_snap, 0 },
    { &pssj_device, snd_pssj_rmm_snap, 0 },
    { &pssj_isa_device, snd_pssj_rmm_snap, 0 },
    /* snd_mmb */
    { &mmb_device, snd_mmb_rmm_snap, 0 },
    /* snd_csm */
    { &soundmaster_device, snd_csm_rmm_snap, 0 },
    /* snd_opl_esfm */
    { &esfm_esfmu_device, snd_opl_esfm_rmm_snap, 0 },
    /* snd_lpt_adlipt */
    { &lpt_adlipt_device, snd_lpt_adlipt_rmm_snap, 0 },
    { &lpt_opl3_device, snd_lpt_adlipt_rmm_snap, 0 },
    /* snd_lpt_cms */
    { &lpt_cms_device, snd_lpt_cms_rmm_snap, 0 },
    /* snd_lpt_tndlpt */
    { &lpt_tnd_device, snd_lpt_tndlpt_rmm_snap, 0 },
    /* More devices that share an existing visitor's struct. */
    { &sis_5513_ide_device, sis_5513_ide_rmm_snap, 0 },
    { &sis_5572_ide_device, sis_5513_ide_rmm_snap, 0 },
    { &sis_5582_ide_device, sis_5513_ide_rmm_snap, 0 },
    { &sis_5513_p2i_device, sis_5513_p2i_rmm_snap, 0 },
    { &sis_5572_p2i_device, sis_5513_p2i_rmm_snap, 0 },
    { &sis_5582_p2i_device, sis_5513_p2i_rmm_snap, 0 },
    { &sis_5595_p2i_device, sis_5513_p2i_rmm_snap, 0 },
    { &sis_5572_usb_device, sis_5572_usb_rmm_snap, 0 },
    { &sis_5582_usb_device, sis_5572_usb_rmm_snap, 0 },
    { &sis_5591_device, sis_5591_rmm_snap, 0 },
    { &sis_5595_pmu_device, sis_5595_pmu_rmm_snap, 0 },
    { &ym3812_nuked_opl3_device, nuked_opl3_rmm_snap, 0 },
    { &ymf289b_nuked_opl3_device, nuked_opl3_rmm_snap, 0 },
    { &ati_xpert128_pci_device, rage128_rmm_snap, 0 },
    { &ibm7690_video_device, mcga_rmm_snap, 0 },
    /* 82c100 */
    { &ct_82c100_device, ct_82c100_rmm_snap, 0 },
    /* acc2036 */
    { &acc2036_device, acc2036_rmm_snap, 0 },
    /* ali1541 */
    { &ali1541_device, ali1541_rmm_snap, 0 },
    { &ali1541_rowdecode_device, ali1541_rmm_snap, 0 },
    /* compaq_386 */
    { &compaq_386_device, compaq_386_rmm_snap, 0 },
    { &compaq_genoa_device, compaq_386_rmm_snap, 0 },
    /* cs8220 */
    { &cs8220_device, cs8220_rmm_snap, 0 },
    /* headland */
    { &headland_gc10x_device, headland_rmm_snap, 0 },
    { &headland_gc113_device, headland_rmm_snap, 0 },
    { &headland_ht18a_device, headland_rmm_snap, 0 },
    { &headland_ht18b_device, headland_rmm_snap, 0 },
    { &headland_ht18c_device, headland_rmm_snap, 0 },
    { &headland_ht21c_d_device, headland_rmm_snap, 0 },
    { &headland_ht21e_device, headland_rmm_snap, 0 },
    /* neat */
    { &neat_device, neat_rmm_snap, 0 },
    { &neat_sx_device, neat_rmm_snap, 0 },
    /* opti283 */
    { &opti283_device, opti283_rmm_snap, 0 },
    /* opti498 */
    { &opti498_device, opti498_rmm_snap, 0 },
    /* opti55x */
    { &opti55x_device, opti55x_rmm_snap, 0 },
    { &opti55x_noide_device, opti55x_rmm_snap, 0 },
    /* scat */
    { &scat_4_device, scat_rmm_snap, 0 },
    { &scat_device, scat_rmm_snap, 0 },
    { &scat_sx_device, scat_rmm_snap, 0 },
    /* sis_530 */
    { &sis_530_device, sis_530_rmm_snap, 0 },
    /* sis_530_h2p */
    { &sis_530_h2p_device, sis_530_h2p_rmm_snap, 0 },
    /* sis_5511 */
    { &sis_5511_device, sis_5511_rmm_snap, 0 },
    /* sis_5581 */
    { &sis_5581_device, sis_5581_rmm_snap, 0 },
    /* sis_5600 */
    { &sis_5600_1997_device, sis_5600_rmm_snap, 0 },
    { &sis_5600_device, sis_5600_rmm_snap, 0 },
    /* sis_5600_h2p */
    { &sis_5600_h2p_device, sis_5600_h2p_rmm_snap, 0 },
    /* sis_85c4xx */
    { &sis_85c401_device, sis_85c4xx_rmm_snap, 0 },
    { &sis_85c460_device, sis_85c4xx_rmm_snap, 0 },
    { &sis_85c461_device, sis_85c4xx_rmm_snap, 0 },
    { &sis_85c471_device, sis_85c4xx_rmm_snap, 0 },
    /* sis_85c50x */
    { &sis_550x_85c503_device, sis_85c50x_rmm_snap, 0 },
    { &sis_550x_device, sis_85c50x_rmm_snap, 0 },
    { &sis_85c50x_5503_device, sis_85c50x_rmm_snap, 0 },
    { &sis_85c50x_device, sis_85c50x_rmm_snap, 0 },
    /* ut85c50x */
    { &ut85c50x_device, ut85c50x_rmm_snap, 0 },
    /* vl82c59x */
    { &vl82c59x_compaq_device, vl82c59x_rmm_snap, 0 },
    { &vl82c59x_device, vl82c59x_rmm_snap, 0 },
    { &vl82c59x_wildcat_compaq_device, vl82c59x_rmm_snap, 0 },
    { &vl82c59x_wildcat_device, vl82c59x_rmm_snap, 0 },
    /* wd76c10 */
    { &wd76c10_device, wd76c10_rmm_snap, 0 },
    /* net_3c503 */
    { &threec503_device, net_3c503_rmm_snap, 0 },
    /* net_dp83815 */
    { &dp83815_device, net_dp83815_rmm_snap, 0 },
    { &dp83815_onboard_device, net_dp83815_rmm_snap, 0 },
    /* net_pcnet */
    { &pcnet_am79c960_device, net_pcnet_rmm_snap, 0 },
    { &pcnet_am79c960_eb_device, net_pcnet_rmm_snap, 0 },
    { &pcnet_am79c960_vlb_device, net_pcnet_rmm_snap, 0 },
    { &pcnet_am79c961_device, net_pcnet_rmm_snap, 0 },
    { &pcnet_am79c970a_device, net_pcnet_rmm_snap, 0 },
    { &pcnet_am79c970a_onboard_device, net_pcnet_rmm_snap, 0 },
    { &pcnet_am79c973_device, net_pcnet_rmm_snap, 0 },
    { &pcnet_am79c973_onboard_device, net_pcnet_rmm_snap, 0 },
    /* net_smc_epic100 */
    { &smc_epic100_device, net_smc_epic100_rmm_snap, 0 },
    /* net_wd8003 */
    { &ibm_ethernet_efd4_device, net_wd8003_rmm_snap, 0 },
    { &ibm_ethernet_efd5_device, net_wd8003_rmm_snap, 0 },
    { &ibm_ethernet_efe5_device, net_wd8003_rmm_snap, 0 },
    { &wd8003e_device, net_wd8003_rmm_snap, 0 },
    { &wd8003ea_device, net_wd8003_rmm_snap, 0 },
    { &wd8003eb_device, net_wd8003_rmm_snap, 0 },
    { &wd8003eta_device, net_wd8003_rmm_snap, 0 },
    { &wd8013ebt_device, net_wd8003_rmm_snap, 0 },
    { &wd8013epa_device, net_wd8003_rmm_snap, 0 },
    { &threec509b_device, net_3c509b_rmm_snap, 0 },
    { &threec529_mc_device, net_3c509b_rmm_snap, 0 },
    { &threec529_tp_device, net_3c509b_rmm_snap, 0 },
    { &i82557_device, i8255x_rmm_snap, 0 },
    { &i82557b_onboard_device, i8255x_rmm_snap, 0 },
    { &i82558_device, i8255x_rmm_snap, 0 },
    { &i82558b_onboard_device, i8255x_rmm_snap, 0 },
    { &i82559c_onboard_device, i8255x_rmm_snap, 0 },
    { &i82559er_onboard_device, i8255x_rmm_snap, 0 },
    { &nec_pk_ug_x006_device, i8255x_rmm_snap, 0 },
    { &rtl8139c_plus_device, net_rtl8139_rmm_snap, 0 },
    { &dec_tulip_21040_device, net_tulip_rmm_snap, 0 },
    { &dec_tulip_21140_device, net_tulip_rmm_snap, 0 },
    { &dec_tulip_device, net_tulip_rmm_snap, 0 },
    { &bugger_device, bugger_rmm_snap, 0 },
    { &postcard_device, postcard_rmm_snap, 0 },
    { &unittester_device, unittester_rmm_snap, 0 },
    { &mouse_ps2_device, mouse_ps2_rmm_snap, 0 },
    { &ast_nvr_device, ast_nvr_rmm_snap, 0 },
    { &softpower_device, softpower_rmm_snap, 0 },
    { &zenith_scratchpad_device, zenith_scratchpad_rmm_snap, 0 },
    { &inboard386_at_device, inboard386_rmm_snap, 0 },
    { &inboard386_xt_device, inboard386_rmm_snap, 0 },
    { &ibm_xma_mca_2mb_device, mxo_rmm_snap, 0 },
    { &ibm_xma_mca_8mb_f7fe_device, xma_rmm_snap, 0 },
    { &ibm_xma_mca_8mb_f7f7_device, xma_rmm_snap, 0 },
    { &quadmeg_psq_4mb_76da_device, psq_rmm_snap, 0 },
    { &quadmeg_psq_4mb_76de_device, psq_rmm_snap, 0 },
    { &xi8088_device, xi8088_rmm_snap, 0 },
    { &europc_device, europc_rmm_snap, 0 },
    { &tvis_memcard_device, tvis_memcard_rmm_snap, 0 },
    { &pdc20269_device, hdc_ide_pdc202xx_rmm_snap, 0 },
    { &ps1_hdc_device, hdc_xta_ps1_rmm_snap, 0 },
    { &ps2_m25_hdc_device, hdc_xta_ps1_rmm_snap, 0 },
    { &mcide_device, mcide_rmm_snap, 0 },
    /* snd_azt2316a */
    { &azt1605_device, snd_azt2316a_rmm_snap, 0 },
    { &azt2316a_device, snd_azt2316a_rmm_snap, 0 },
    { &azt2316r_device, snd_azt2316a_rmm_snap, 0 },
    { &aztpr16_device, snd_azt2316a_rmm_snap, 0 },
    /* snd_azt2320 */
    { &azt2320_device, snd_azt2320_rmm_snap, 0 },
    /* snd_cs423x */
    { &cs4232_device, snd_cs423x_rmm_snap, 0 },
    { &cs4232_onboard_device, snd_cs423x_rmm_snap, 0 },
    { &cs4235_device, snd_cs423x_rmm_snap, 0 },
    { &cs4235_onboard_device, snd_cs423x_rmm_snap, 0 },
    { &cs4236_onboard_device, snd_cs423x_rmm_snap, 0 },
    { &cs4236b_device, snd_cs423x_rmm_snap, 0 },
    { &cs4236b_onboard_device, snd_cs423x_rmm_snap, 0 },
    { &cs4237b_device, snd_cs423x_rmm_snap, 0 },
    { &cs4238b_device, snd_cs423x_rmm_snap, 0 },
    /* snd_wss */
    { &ncr_business_audio_device, snd_wss_rmm_snap, 0 },
    { &wss_device, snd_wss_rmm_snap, 0 },
    /* snd_ymf701 */
    { &ymf701_device, snd_ymf701_rmm_snap, 0 },
    /* snd_ymf71x */
    { &ymf715_onboard_device, snd_ymf71x_rmm_snap, 0 },
    { &ymf718_device, snd_ymf71x_rmm_snap, 0 },
    { &ymf719_device, snd_ymf71x_rmm_snap, 0 },
    /* snd_mpu401 */
    { &mpu401_device, snd_mpu401_rmm_snap, 0 },
    { &mpu401_mca_device, snd_mpu401_rmm_snap, 0 },
    /* snd_adlibgold */
    { &adgold_device, snd_adlibgold_rmm_snap, 0 },
    /* snd_sensation */
    { &sensationaud_device, snd_sensation_rmm_snap, 0 },
    /* snd_ad1816 */
    { &ad1816_device, snd_ad1816_rmm_snap, 0 },
    /* snd_jazz16 */
    { &jazz16_device, snd_jazz16_rmm_snap, 0 },
    /* snd_opl2_nuked */
    { &ym3812_nuked_opl2_device, nuked_opl2_rmm_snap, 0 },
    /* snd_cqm_nuked */
    { &nuked_cqm_device, nuked_cqm_rmm_snap, 0 },
    /* snd_gus */
    { &gus_ace_device, gus_rmm_snap, 0 },
    { &gus_device, gus_rmm_snap, 0 },
    { &gus_extreme_device, gus_rmm_snap, 0 },
    { &gus_max_device, gus_rmm_snap, 0 },
    { &gus_pnp_compaq_device, gus_rmm_snap, 0 },
    { &gus_pnp_device, gus_rmm_snap, 0 },
    { &gus_pnp_new_device, gus_rmm_snap, 0 },
    { &gus_pnp_nocd_device, gus_rmm_snap, 0 },
    { &gus_v34_device, gus_rmm_snap, 0 },
    { &gus_v37_device, gus_rmm_snap, 0 },
    { &gus_vipermax_device, gus_rmm_snap, 0 },
    /* snd_audiopci */
    { &ct5880_device, es137x_rmm_snap, 0 },
    { &ct5880_onboard_device, es137x_rmm_snap, 0 },
    { &es1370_device, es137x_rmm_snap, 0 },
    { &es1371_device, es137x_rmm_snap, 0 },
    { &es1371_onboard_device, es137x_rmm_snap, 0 },
    { &es1373_device, es137x_rmm_snap, 0 },
    { &es1373_onboard_device, es137x_rmm_snap, 0 },
    /* snd_cmi8x38 */
    { &cmi8338_device, cmi8x38_rmm_snap, 0 },
    { &cmi8338_onboard_device, cmi8x38_rmm_snap, 0 },
    { &cmi8738_6ch_onboard_device, cmi8x38_rmm_snap, 0 },
    { &cmi8738_device, cmi8x38_rmm_snap, 0 },
    { &cmi8738_onboard_device, cmi8x38_rmm_snap, 0 },
    /* snd_ac97_via */
    { &ac97_via_device, ac97_via_rmm_snap, 0 },
    /* snd_solo1 */
    { &ess_solo1_device, solo1_rmm_snap, 0 },
    { &ess_solo1_onboard_device, solo1_rmm_snap, 0 },
    /* snd_ac97_codec */
    { &ad1881_device, ac97_codec_rmm_snap, 0 },
    { &ak4540_device, ac97_codec_rmm_snap, 0 },
    { &alc100_device, ac97_codec_rmm_snap, 0 },
    { &cs4297_device, ac97_codec_rmm_snap, 0 },
    { &cs4297a_device, ac97_codec_rmm_snap, 0 },
    { &ice1232_device, ac97_codec_rmm_snap, 0 },
    { &stac9708_device, ac97_codec_rmm_snap, 0 },
    { &stac9721_device, ac97_codec_rmm_snap, 0 },
    { &tr28023_device, ac97_codec_rmm_snap, 0 },
    { &w83971d_device, ac97_codec_rmm_snap, 0 },
    { &wm9701a_device, ac97_codec_rmm_snap, 0 },
    /* snd_optimc */
    { &acermagic_s20_device, optimc_rmm_snap, 0 },
    { &mirosound_pcm10_device, optimc_rmm_snap, 0 },
    { &opti_82c930_device, optimc_rmm_snap, 0 },
    { &opti_82c931_device, optimc_rmm_snap, 0 },
    /* snd_pas16 */
    { &pas16_device, pas16_rmm_snap, 0 },
    { &pas16d_device, pas16_rmm_snap, 0 },
    { &pas_device, pas16_rmm_snap, 0 },
    { &pasplus_device, pas16_rmm_snap, 0 },
    /* snd_ps1_midi */
    { &ps1midi_device, ps1midi_rmm_snap, 0 },
    SBC(sb_32_pnp_device),
    SBC(sb_awe32_device),
    SBC(sb_awe32_ide_pnp_device),
    SBC(sb_awe32_pnp_device),
    SBC(sb_awe64_device),
    SBC(sb_awe64_gold_device),
    SBC(sb_awe64_ide_device),
    SBC(sb_awe64_value_device),
    { &sb_goldfinch_device, goldfinch_rmm_snap, 0 },
    /* ega (6), 8514/A (2), mach (6) */
    { &ega_device, ega_rmm_snap, 0 }, { &cpqega_device, ega_rmm_snap, 0 }, { &sega_device, ega_rmm_snap, 0 },
    { &atiega800p_device, ega_rmm_snap, 0 }, { &iskra_ega_device, ega_rmm_snap, 0 }, { &et2000_device, ega_rmm_snap, 0 },
    { &gen8514_isa_device, ibm8514_rmm_snap, 0 }, { &ibm8514_mca_device, ibm8514_rmm_snap, 0 },
    NOSTATE(pcjx_video_device), NOSTATE(av9194_device),
    /* vid_jega */
    { &if386jega_device, jega_rmm_snap, 0 },
    { &jega_device, jega_rmm_snap, 0 },
    { &jvga_device, jega_rmm_snap, 0 },
    /* vid_xga */
    { &xga_device, xga_rmm_snap, 0 },
    /* vid_xga */
    { &inmos_isa_device, svga_xga_rmm_snap, 0 },
    /* vid_ati_mach8 */
    { &mach32_isa_device, mach8_rmm_snap, 0 },
    { &mach32_mca_device, mach8_rmm_snap, 0 },
    { &mach32_onboard_pci_device, mach8_rmm_snap, 0 },
    { &mach32_pci_device, mach8_rmm_snap, 0 },
    { &mach32_vlb_device, mach8_rmm_snap, 0 },
    { &mach8_vga_isa_device, mach8_rmm_snap, 0 },
    /* vid_hd44780 */
    { &hd44780_cobalt3k_device, hd44780_rmm_snap, 0 },
    { &hd44780_device, hd44780_rmm_snap, 0 },
    /* vid_clockgen_icd2061 */
    { &icd2061_device, icd2061_rmm_snap, 0 },
    { &ics9161_device, icd2061_rmm_snap, 0 },
    /* vid_ramdac_tvp3026 */
    { &tvp3026_ramdac_device, tvp3026_ramdac_rmm_snap, 0 },
    /* vid_ramdac_ibm_rgb528 */
    { &ibm_rgb528_ramdac_device, ibm_rgb528_ramdac_rmm_snap, 0 },
    /* mouse_upc */
    { &mouse_upc_device, mouse_upc_rmm_snap, 0 },
    { &mouse_upc_standalone_device, mouse_upc_rmm_snap, 0 },
    /* mouse_microtouch_touchscreen */
    { &mouse_mtouch_device, mouse_microtouch_rmm_snap, 0 },
    /* via_pipc */
    { &via_vt8231_device, via_pipc_rmm_snap, 0 },
    { &via_vt82c586b_device, via_pipc_rmm_snap, 0 },
    { &via_vt82c596a_device, via_pipc_rmm_snap, 0 },
    { &via_vt82c596b_device, via_pipc_rmm_snap, 0 },
    { &via_vt82c686a_device, via_pipc_rmm_snap, 0 },
    { &via_vt82c686b_device, via_pipc_rmm_snap, 0 },
    /* stpc */
    { &stpc_atlas_device, stpc_rmm_snap, 0 },
    { &stpc_client_device, stpc_rmm_snap, 0 },
    { &stpc_consumer2_device, stpc_rmm_snap, 0 },
    { &stpc_elite_device, stpc_rmm_snap, 0 },
    /* stpc */
    { &stpc_serial_device, stpc_serial_rmm_snap, 0 },
    /* stpc */
    { &stpc_lpt_device, stpc_lpt_rmm_snap, 0 },
    /* intel_pceb_esc */
    { &esc_device, esc_rmm_snap, 0 },
    /* intel_pceb_esc */
    { &pceb_device, pceb_rmm_snap, 0 },
    /* laserxt */
    { &laserxt_device, lxt_rmm_snap, 0 },
    { &lxt3_device, lxt_rmm_snap, 0 },
    /* scamp */
    { &vlsi_scamp_device, scamp_rmm_snap, 0 },
    /* vl82c480 */
    { &vl82c480_device, vl82c480_rmm_snap, 0 },
    { &vl82c486_device, vl82c480_rmm_snap, 0 },
    /* sis_5581_h2p */
    { &sis_5581_h2p_device, sis_5581_h2p_rmm_snap, 0 },
    /* sis_5571 */
    { &sis_5571_device, sis_5571_rmm_snap, 0 },
    /* scsi_ql1xxx */
    { &isp1020a_device, ql1xxx_rmm_snap, 0 },
    { &qla1040b_device, ql1xxx_rmm_snap, 0 },
    { &qla1080_device, ql1xxx_rmm_snap, 0 },
    { &qla12160a_device, ql1xxx_rmm_snap, 0 },
    { &qla1240_device, ql1xxx_rmm_snap, 0 },
    { &qla1280_device, ql1xxx_rmm_snap, 0 },
    /* vid_ps55da2 */
    { &ps55da2_device, ps55da2_rmm_snap, 0 },
    { &ps55db2_device, ps55da2_rmm_snap, 0 },
    /* m_xt_olivetti */
    { &m19_vid_device, m19_vid_rmm_snap, 0 },
    /* snd_opl_ymfm */
    { &ym2149_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym3526_ymfm_device, ymfm_rmm_snap, 0 },
    { &y8950_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym3812_ymfm_device, ymfm_rmm_snap, 0 },
    { &ymf262_ymfm_device, ymfm_rmm_snap, 0 },
    { &ymf289b_ymfm_device, ymfm_rmm_snap, 0 },
    { &ymf278b_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym2413_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym2423_ymfm_device, ymfm_rmm_snap, 0 },
    { &ymf281_ymfm_device, ymfm_rmm_snap, 0 },
    { &ds1001_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym2151_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym2203_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym2608_ymfm_device, ymfm_rmm_snap, 0 },
    { &ymf288_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym2610_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym2610b_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym2612_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym3438_ymfm_device, ymfm_rmm_snap, 0 },
    { &ymf276_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym2164_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym3806_ymfm_device, ymfm_rmm_snap, 0 },
    { &ym2414_ymfm_device, ymfm_rmm_snap, 0 },
    /* snd_saaym */
    { &saaym_device, saaym_rmm_snap, 0 },
    { &w83877_device,                w83877_rmm_snap,        0 },
    { NULL,               NULL,             0                 }
};

/* -- pointer scan ---------------------------------------------------------------
   Finds 8-byte words in dumped regions that hold addresses of this process.
   Linux only. See releasenotes.txt. */

#define SCAN_MAX     262144
#define SCAN_RANGES  2048

typedef struct {
    uintptr_t lo, hi;
    uint64_t  off;     /* file offset of the mapping */
    int       code;
    char      path[192]; /* the mapped file, "" if anonymous */
} scan_range_t;

typedef struct {
    const char *label;
    uint32_t    region; /* index of the dump within the pass */
    uint32_t    rlen;
    uint32_t    off;
    uint8_t     code;   /* the word points into mapped code, else data */
    int16_t     rng;    /* mapping the word points into, -1 none */
    uint64_t    val;
    uint64_t    base;   /* address of the dumped region */
    int32_t     target; /* dumped region the word points into, else -1 */
    uint32_t    toff;
} scan_hit_t;

typedef struct {
    uint64_t base;
    uint64_t len;
    uint64_t local; /* a local variable, not state: never a target */
} scan_reg_t;

/* Not in the table: which visitor made each dump, for the scan report. */
#define SCAN_REGS 131072
static const char *scan_reglabel[SCAN_REGS];
static scan_reg_t    scan_regs[SCAN_REGS];
static const char   *scan_label = "";
static int           scan_on;
static uint32_t      scan_region;
static scan_hit_t    scan_hits[SCAN_MAX];
static int           scan_nhits;
static int           scan_dropped;
static scan_range_t  scan_ranges[SCAN_RANGES];
static int           scan_nranges;

static int
rmm_snap_scan_load_maps(void)
{
#ifdef __linux__
    char    line[512];
    char    exe[192] = "";
    ssize_t el       = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    FILE   *f        = fopen("/proc/self/maps", "r");

    exe[(el > 0) ? el : 0] = 0;
    scan_nranges           = 0;
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f) && (scan_nranges < SCAN_RANGES)) {
        unsigned long lo, hi, off = 0;
        char          perms[8];
        char          path[192] = "";
        int           k;

        k = sscanf(line, "%lx-%lx %7s %lx %*x:%*x %*u %191[^\n]", &lo, &hi, perms, &off, path);
        if ((k < 4) || (perms[0] != 'r'))
            continue;
        /* Only a file a fresh process maps at the same offset is a module. */
        if ((k == 5) && (!strncmp(path, "/memfd:", 7) || !strncmp(path, "/dev/", 5) || !strncmp(path, "/SYSV", 5) ||
                         strstr(path, " (deleted)")))
            k = 4;
        scan_ranges[scan_nranges].lo   = lo;
        scan_ranges[scan_nranges].hi   = hi;
        scan_ranges[scan_nranges].off  = off;
        scan_ranges[scan_nranges].code = (perms[2] == 'x');
        snprintf(scan_ranges[scan_nranges].path, sizeof(scan_ranges[0].path), "%s", (k == 5) ? path : "");
        /* The binary's .bss follows its last mapping; libraries are excluded. */
        if ((k < 5) && scan_nranges && exe[0] && !strcmp(scan_ranges[scan_nranges - 1].path, exe) &&
            (scan_ranges[scan_nranges - 1].hi == lo)) {
            snprintf(scan_ranges[scan_nranges].path, sizeof(scan_ranges[0].path), "%s",
                     scan_ranges[scan_nranges - 1].path);
            scan_ranges[scan_nranges].off = scan_ranges[scan_nranges - 1].off + (lo - scan_ranges[scan_nranges - 1].lo);
        }
        scan_nranges++;
    }
    fclose(f);
    return 0;
#else
    return -1;
#endif
}

static void
rmm_snap_scan_region(const void *p, size_t n, int local, int data)
{
    const uint8_t *b = p;
    uint32_t       r = scan_region++;

    if (r < SCAN_REGS) {
        scan_regs[r].base = (uintptr_t) p;
        scan_regs[r].len  = n;
        scan_regs[r].local = local;
        scan_reglabel[r]   = scan_label;
    }
    if (data)
        return; /* still a region pointers may point into */
    for (size_t off = 0; off + sizeof(uintptr_t) <= n; off += sizeof(uintptr_t)) {
        uintptr_t v;

        memcpy(&v, b + off, sizeof(v));
        if (v < 0x10000)
            continue;
        for (int i = 0; i < scan_nranges; i++) {
            if (v >= scan_ranges[i].lo && v < scan_ranges[i].hi) {
                if (scan_nhits < SCAN_MAX) {
                    scan_hit_t *h = &scan_hits[scan_nhits++];

                    h->label  = scan_label;
                    h->region = r;
                    h->rlen   = (uint32_t) n;
                    h->off    = (uint32_t) off;
                    h->code   = scan_ranges[i].code;
                    h->rng    = (int16_t) i;
                    h->val    = v;
                    h->base   = (uintptr_t) p;
                } else
                    scan_dropped++;
                break;
            }
        }
    }
}

static void rmm_reloc_raw(rmm_snap_t *s, void *p, size_t n);
static void rmm_reloc_fail(rmm_snap_t *s, const char *fmt, ...);
static int  rmm_reloc_layout(const rmm_snap_t *s);

void
rmm_snap_raw(rmm_snap_t *s, void *p, size_t n)
{
    if (s->failed)
        return;
    if (scan_on && !s->restore)
        rmm_snap_scan_region(p, n, s->local, s->data);
    s->data = 0;
    if (!s->rl) { /* only the relocating restore uses these */
        s->local = 0;
        s->tmpl  = NULL;
    }
    if (s->restore) {
        if (s->pos + n > s->len) {
            s->failed = 1;
            return;
        }
        if (s->rl) {
            rmm_reloc_raw(s, p, n);
            return;
        }
        memcpy(p, s->buf + s->pos, n);
        s->pos += n;
        return;
    }
    if (s->len + n > s->cap) {
        size_t   cap = s->cap ? s->cap : (1 << 20);
        uint8_t *nb;

        while (cap < s->len + n)
            cap *= 2;
        if (!(nb = realloc(s->buf, cap))) {
            s->failed = 1;
            return;
        }
        s->buf = nb;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
}

int
rmm_snap_restoring(const rmm_snap_t *s)
{
    return s->restore;
}

/* Modules whose device_t are private to their file answer for them. */
extern rmm_snap_fn isartc_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn isamem_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn isarom_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn isapnp_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn spd_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn ide_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn eep_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn ibm5140_power_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn ibm5140_lcd_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn epoch_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn ics9xxx_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn amstrad_rmm_snap_fn(const device_t *dev);
extern rmm_snap_fn tandy_rmm_snap_fn(const device_t *dev);

static rmm_snap_fn (*const rmm_snap_private[])(const device_t *dev) = {
    isartc_rmm_snap_fn, isamem_rmm_snap_fn, isarom_rmm_snap_fn, isapnp_rmm_snap_fn, spd_rmm_snap_fn,
    ide_rmm_snap_fn, eep_rmm_snap_fn, ibm5140_power_rmm_snap_fn, ibm5140_lcd_rmm_snap_fn, epoch_rmm_snap_fn, ics9xxx_rmm_snap_fn,
    amstrad_rmm_snap_fn, tandy_rmm_snap_fn,
};

/* How to save a device: 1 and *fn / *size set, 0 if it has no visitor. */
static int
rmm_snap_lookup(const device_t *dev, rmm_snap_fn *fn, size_t *size)
{
    rmm_snap_fn f;

    for (int i = 0; rmm_snap_devices[i].dev; i++)
        if (rmm_snap_devices[i].dev == dev) {
            *fn   = rmm_snap_devices[i].fn;
            *size = rmm_snap_devices[i].size;
            return 1;
        }
    for (size_t i = 0; i < sizeof(rmm_snap_private) / sizeof(rmm_snap_private[0]); i++)
        if ((f = rmm_snap_private[i](dev))) {
            *fn   = f;
            *size = 0;
            return 1;
        }
    return 0;
}


/* Error 8 text: every device without a visitor, by internal name. */
static int
rmm_snap_unsupported(char *why, size_t n)
{
    const device_t *dev;
    void           *priv;
    size_t          used = 0;
    int             bad  = 0;

    why[0] = 0;
    for (int c = 0; device_rmm_get(c, &dev, &priv); c++) {
        rmm_snap_fn fn;
        size_t      size;

        if (!dev || rmm_snap_lookup(dev, &fn, &size))
            continue;
        if (!bad)
            used = snprintf(why, n, "no snapshot support for:");
        if (used < n)
            used += snprintf(why + used, n - used, " %s", dev->internal_name ? dev->internal_name : dev->name);
        bad++;
    }
    if (!bad) {
        const char *r = ide_rmm_snap_blocked();

        if (!r)
            r = fdd_rmm_snap_blocked();
        if (!r)
            r = cdrom_rmm_snap_blocked();
        if (!r)
            r = mo_rmm_snap_blocked();
        if (!r)
            r = rdisk_rmm_snap_blocked();
        if (!r)
            r = modem_rmm_snap_blocked();
        if (!r)
            r = cassette_rmm_snap_blocked();
        if (r) {
            snprintf(why, n, "cannot snapshot: %s", r);
            bad = 1;
        }
    }
    return bad;
}

static int
rmm_snap_find(const char *tag)
{
    for (int i = 0; i < RMM_SNAP_MAX; i++)
        if (rmm_snaps[i] && !strcmp(rmm_snaps[i]->tag, tag))
            return i;
    return -1;
}

/* The device table as it is now; *n entries, caller frees. */
static rmm_snap_devref_t *
rmm_snap_devtable(int *n)
{
    const device_t    *dev;
    void              *priv;
    rmm_snap_devref_t *t = NULL;

    *n = 0;
    for (int c = 0; device_rmm_get(c, &dev, &priv); c++) {
        if (!dev)
            continue;
        t = realloc(t, (*n + 1) * sizeof(rmm_snap_devref_t));
        t[*n].dev  = dev;
        t[*n].priv = priv;
        (*n)++;
    }
    return t;
}

/* The timer list in order with each timer's contents; rebuilt from this on restore. */
static void
rmm_snap_timers(rmm_snap_t *s)
{
    uint32_t    n = 0;
    pc_timer_t *t;
    pc_timer_t  copy;

    if (!rmm_snap_restoring(s)) {
        for (t = timer_head; t; t = t->next)
            n++;
        RMM_SNAP_VAR(s, n);
        for (t = timer_head; t; t = t->next) {
            rmm_snap_local(s, NULL); /* the restore reads these into locals */
            RMM_SNAP_VAR(s, t);
            rmm_snap_local(s, NULL);
            RMM_SNAP_VAR(s, *t);
        }
        RMM_SNAP_VAR(s, timer_head);
        RMM_SNAP_VAR(s, timer_target);
        return;
    }
    RMM_SNAP_VAR(s, n);
    for (uint32_t i = 0; (i < n) && !s->failed; i++) {
        t = (pc_timer_t *) 1; /* stays 1 if the timer is in no struct or module the table knows */
        rmm_snap_local(s, NULL);
        RMM_SNAP_VAR(s, t);
        if (s->rl && (t == (pc_timer_t *) 1) && !rmm_reloc_layout(s)) {
            rmm_reloc_fail(s, "timer %u is in no dumped struct or module", i);
            break;
        }
        rmm_snap_local(s, (t == (pc_timer_t *) 1) ? NULL : t); /* 1: no object to take the pointers from */
        RMM_SNAP_VAR(s, copy);
        if (t && !s->failed && !rmm_reloc_layout(s))
            *t = copy;
    }
    RMM_SNAP_VAR(s, timer_head);
    RMM_SNAP_VAR(s, timer_target);
}

/* Take every timer off the list; timers not enabled in the snapshot stay disabled. */
static void
rmm_snap_timers_unlink(void)
{
    pc_timer_t *t = timer_head;

    while (t) {
        pc_timer_t *next = t->next;

        t->prev = t->next = NULL;
        t->flags &= ~TIMER_ENABLED;
        t = next;
    }
    timer_head = NULL;
}

static void
rmm_snap_all(rmm_snap_t *s, const rmm_snap_devref_t *devs, int ndevs)
{
#define CORE(label, call) (scan_label = (label), (call))
    CORE("cpu", cpu_rmm_snap(s));
    CORE("cpu386_common", cpu386_common_rmm_snap(s));
    CORE("cpu386_dynarec", cpu386_dynarec_rmm_snap(s));
    CORE("x86", x86_rmm_snap(s));
    CORE("x87", x87_rmm_snap(s));
    CORE("mem", mem_rmm_snap(s));
    CORE("io", io_rmm_snap(s));
    CORE("pic", pic_rmm_snap(s));
    CORE("dma", dma_rmm_snap(s));
    CORE("pci", pci_rmm_snap(s));
    CORE("pit", pit_rmm_snap(s));
    CORE("ppi", ppi_rmm_snap(s));
    CORE("nvr", nvr_rmm_snap(s));
    CORE("network", network_rmm_snap(s));
    CORE("keyboard", keyboard_rmm_snap(s));
    CORE("smram", smram_rmm_snap(s));
    CORE("fdd", fdd_rmm_snap(s));
    CORE("hdd_image", hdd_image_rmm_snap(s));
    CORE("cdrom", cdrom_rmm_snap(s));
    CORE("mo", mo_rmm_snap(s));
    CORE("rdisk", rdisk_rmm_snap(s));
    CORE("tape", tape_rmm_snap(s));
    CORE("fdd_tape", fdd_tape_rmm_snap(s));
    CORE("scsi_disk", scsi_disk_rmm_snap(s));
    CORE("scsi_device", scsi_device_rmm_snap(s));
#undef CORE
    scan_label = "nmi_mask";
    RMM_SNAP_VAR(s, nmi_mask);

    for (int i = 0; i < ndevs; i++) {
        rmm_snap_fn fn   = NULL;
        size_t      size = 0;

        rmm_snap_lookup(devs[i].dev, &fn, &size);
        scan_label = devs[i].dev->internal_name ? devs[i].dev->internal_name : devs[i].dev->name;

        if (fn)
            fn(s, devs[i].priv);
        else
            rmm_snap_raw(s, devs[i].priv, size);
    }

    scan_label = "timers";
    rmm_snap_timers(s);
}

static void
rmm_snap_free(rmm_snapshot_t *snap)
{
    hdd_image_rmm_free(snap->disks);
    free(snap->reloc);
    free(snap->ident);
    free(snap->build);
    free(snap->data.buf);
    free(snap->devs);
    free(snap);
}

/* -- async writer ---------------------------------------------------------------
   With RMM_SNAP_DIR set, a thread writes each snapshot buffer to a file and frees it.
   See releasenotes.txt. */

static void rmm_snap_disarm_if_none(void);
static int  rmm_snap_write_file(rmm_snapshot_t *snap, const char *path, char *err, size_t n);

static mutex_t        *snap_lock;
static thread_t       *w_thread;
static event_t        *w_work_ev;
static event_t        *w_idle_ev;
static volatile int    w_quit;
static volatile int    w_hold; /* test hook: the writer leaves the queue alone */
static int             w_inited;
static char            w_dir[512];
static int             w_keep;
static size_t          w_budget; /* 0: derive from the RAM size */
static uint64_t        w_seq;
static size_t          w_last_len; /* size of the previous snapshot, to pre-size the next */
static rmm_snapshot_t *w_queue;
static rmm_snapshot_t *w_queue_tail;
static rmm_snapshot_t *w_zombies; /* deleted while writing, not yet reaped */

static int
rmm_snap_writer_on(void)
{
    return w_thread != NULL;
}

static void
rmm_snap_lock(void)
{
    if (snap_lock)
        thread_wait_mutex(snap_lock);
}

static void
rmm_snap_unlock(void)
{
    if (snap_lock)
        thread_release_mutex(snap_lock);
}

static void
rmm_snap_remove_file(const rmm_snapshot_t *snap)
{
    char tmp[sizeof(snap->path) + 8];

    if (!snap->path[0] || snap->external)
        return;
    snprintf(tmp, sizeof(tmp), "%s.part", snap->path);
    remove(tmp);
    remove(snap->path);
}


static void
rmm_snap_writer(UNUSED(void *param))
{
    for (;;) {
        thread_wait_event(w_work_ev, -1);
        thread_reset_event(w_work_ev);
        for (;;) {
            rmm_snapshot_t *snap;
            char            err[128] = { 0 };
            int             rc;

            snap = NULL;
            rmm_snap_lock();
            if (!w_hold && (snap = w_queue)) {
                w_queue = snap->next;
                if (!w_queue)
                    w_queue_tail = NULL;
                snap->next   = NULL;
                snap->queued = 0;
                snap->state  = SNAP_WRITING;
            }
            rmm_snap_unlock();
            if (!snap)
                break;

            rc = w_quit ? 1 : rmm_snap_write_file(snap, snap->path, err, sizeof(err));

            rmm_snap_lock();
            if (rc == 1) {
                /* Deleted or shutting down: the file is not wanted. */
                free(snap->data.buf);
                snap->data.buf = NULL;
                snap->data.cap = 0;
                snap->state    = SNAP_DEAD;
                rmm_snap_remove_file(snap);
            } else if (rc == 0) {
                free(snap->data.buf);
                snap->data.buf = NULL;
                snap->data.cap = 0;
                snap->state    = SNAP_DISK;
            } else {
                snprintf(snap->errmsg, sizeof(snap->errmsg), "%s", err);
                snap->state = SNAP_ERROR;
            }
            rmm_snap_unlock();
            thread_set_event(w_idle_ev);
        }
        if (w_quit)
            return;
    }
}

/* Starts the writer once. 0, or -1 with the reason in why. The same dir updates the
   budget; a different dir is refused. */
int
rmm_snap_writer_config(const char *dir, int budget_mb, int keep, char *why, size_t n)
{
    if (!dir || !dir[0] || (strlen(dir) >= sizeof(w_dir) - 40)) {
        snprintf(why, n, "bad snapshot directory");
        return -1;
    }
    if (rmm_snap_writer_on()) {
        if (strcmp(dir, w_dir)) {
            snprintf(why, n, "the snapshot writer already uses '%s'", w_dir);
            return -1;
        }
        if (budget_mb > 0)
            w_budget = (size_t) budget_mb << 20;
        return 0;
    }
    snprintf(w_dir, sizeof(w_dir), "%s", dir);
    if (!plat_dir_check(w_dir) && !plat_dir_create(w_dir) && !plat_dir_check(w_dir)) {
        snprintf(why, n, "cannot create '%s': %s", dir, strerror(errno));
        w_dir[0] = '\0';
        return -1;
    }
    w_budget  = budget_mb > 0 ? (size_t) budget_mb << 20 : 0;
    w_keep    = keep;
    snap_lock = thread_create_mutex();
    w_work_ev = thread_create_event();
    w_idle_ev = thread_create_event();
    w_quit    = 0;
    w_thread  = thread_create(rmm_snap_writer, NULL);
    return 0;
}

/* Test hook: while held, queued snapshots stay in memory. */
void
rmm_snap_writer_hold(int hold)
{
    w_hold = hold;
    if (!hold && rmm_snap_writer_on())
        thread_set_event(w_work_ev);
}

/* Called from the monitor thread before the first save: RMM_SNAP_DIR starts the writer.
   Failure leaves snapshots in memory. */
static void
rmm_snap_writer_init(void)
{
    const char *dir, *v;
    char        why[128];
    int         budget;

    if (w_inited)
        return;
    w_inited = 1;
    if (rmm_snap_writer_on() || !(dir = getenv("RMM_SNAP_DIR")) || !dir[0])
        return;
    v      = getenv("RMM_SNAP_BUDGET_MB");
    budget = v ? atoi(v) : 0;
    v      = getenv("RMM_SNAP_KEEP");
    rmm_snap_writer_config(dir, budget, v && (v[0] == '1'), why, sizeof(why));
}

/* Frees what the writer was still holding for snapshots deleted meanwhile. */
static void
rmm_snap_reap(void)
{
    rmm_snapshot_t *dead = NULL, **pp;

    if (!rmm_snap_writer_on())
        return;
    rmm_snap_lock();
    for (pp = &w_zombies; *pp;) {
        rmm_snapshot_t *z = *pp;

        if (z->state == SNAP_DEAD) {
            *pp     = z->next;
            z->next = dead;
            dead    = z;
        } else
            pp = &z->next;
    }
    rmm_snap_unlock();
    while (dead) {
        rmm_snapshot_t *z = dead;

        dead = z->next;
        rmm_snap_free(z);
    }
}

/* Drops a snapshot the slots no longer hold. */
static void
rmm_snap_release(rmm_snapshot_t *snap)
{
    if (!rmm_snap_writer_on()) {
        rmm_snap_free(snap);
        return;
    }
    rmm_snap_lock();
    if (snap->state == SNAP_WRITING) {
        /* The writer deletes the file, frees the buffer and marks it DEAD. */
        snap->cancel = 1;
        snap->next   = w_zombies;
        w_zombies    = snap;
        rmm_snap_unlock();
        return;
    }
    if (snap->queued) {
        rmm_snapshot_t **pp;

        for (pp = &w_queue; *pp && (*pp != snap); pp = &(*pp)->next)
            ;
        if (*pp) {
            *pp = snap->next;
            if (w_queue_tail == snap) {
                w_queue_tail = NULL;
                for (rmm_snapshot_t *q = w_queue; q; q = q->next)
                    w_queue_tail = q;
            }
        }
        snap->queued = 0;
    }
    rmm_snap_unlock();
    rmm_snap_remove_file(snap);
    rmm_snap_free(snap);
}

static void
rmm_snap_enqueue(rmm_snapshot_t *snap)
{
    if (!rmm_snap_writer_on())
        return;
    snprintf(snap->path, sizeof(snap->path), "%s/rmm_%u_%" PRIu64 ".snap", w_dir,
#ifdef _WIN32
             (unsigned) _getpid(),
#else
             (unsigned) getpid(),
#endif
             snap->seq);
    snap->filelen = snap->data.len;
    snprintf(snap->filetag, sizeof(snap->filetag), "%s", snap->tag);
    rmm_snap_lock();
    snap->next   = NULL;
    snap->queued = 1;
    snap->state  = SNAP_MEM;
    if (w_queue_tail)
        w_queue_tail->next = snap;
    else
        w_queue = snap;
    w_queue_tail = snap;
    rmm_snap_unlock();
    thread_reset_event(w_idle_ev);
    thread_set_event(w_work_ev);
}

/* Bytes of snapshot data still in host memory, and how many are in flight. */
static size_t
rmm_snap_inflight(void)
{
    size_t total = 0;

    rmm_snap_lock();
    for (int i = 0; i < RMM_SNAP_MAX; i++)
        if (rmm_snaps[i] && rmm_snaps[i]->data.buf)
            total += rmm_snaps[i]->data.len;
    for (rmm_snapshot_t *z = w_zombies; z; z = z->next)
        if (z->data.buf)
            total += z->data.len;
    rmm_snap_unlock();
    return total;
}

/* Waits until every queued snapshot is on disk or failed. Returns the number still
   unwritten; *errors counts failed writes. */
int
rmm_snap_flush(int timeout_ms, int *errors)
{
    uint32_t start = plat_get_ticks();
    int      pending;

    if (errors)
        *errors = 0;
    if (!rmm_snap_writer_on())
        return 0;
    for (;;) {
        int bad = 0;

        thread_reset_event(w_idle_ev);
        pending = 0;
        rmm_snap_lock();
        for (int i = 0; i < RMM_SNAP_MAX; i++) {
            if (!rmm_snaps[i])
                continue;
            if (rmm_snaps[i]->queued || (rmm_snaps[i]->state == SNAP_WRITING))
                pending++;
            else if (rmm_snaps[i]->state == SNAP_ERROR)
                bad++;
        }
        rmm_snap_unlock();
        if (errors)
            *errors = bad;
        if (!pending || ((int) (plat_get_ticks() - start) >= timeout_ms))
            break;
        thread_wait_event(w_idle_ev, 20);
    }
    rmm_snap_disarm_if_none();
    return pending;
}

void
rmm_snap_shutdown(void)
{
    if (!rmm_snap_writer_on())
        return;
    w_quit = 1;
    for (int i = 0; i < RMM_SNAP_MAX; i++)
        if (rmm_snaps[i])
            rmm_snaps[i]->cancel = 1;
    thread_set_event(w_work_ev);
    thread_wait(w_thread);
    w_thread = NULL;
    for (int i = 0; i < RMM_SNAP_MAX; i++)
        if (rmm_snaps[i] && !w_keep)
            rmm_snap_remove_file(rmm_snaps[i]);
}

const char *
rmm_snap_state_name(int i, uint64_t *bytes)
{
    static const char *names[] = { "memory", "writing", "disk", "error", "deleted" };
    rmm_snapshot_t    *snap;
    const char        *r;

    if ((i < 0) || (i >= RMM_SNAP_MAX) || !(snap = rmm_snaps[i]))
        return NULL;
    if (bytes)
        *bytes = snap->data.buf ? snap->data.len : snap->filelen;
    if (!rmm_snap_writer_on())
        return names[snap->data.buf ? SNAP_MEM : SNAP_DISK];
    rmm_snap_lock();
    r = (snap->state == SNAP_MEM && snap->queued) ? "queued" : names[snap->state];
    rmm_snap_unlock();
    return r;
}

/* With no snapshot left, the disk overlay stops keeping copies. */
static void
rmm_snap_disarm_if_none(void)
{
    rmm_snap_reap();
    if (w_zombies)
        return; /* their overlay refs are freed by a later reap */
    for (int i = 0; i < RMM_SNAP_MAX; i++)
        if (rmm_snaps[i])
            return;
    hdd_image_rmm_disarm();
}

/* Which dumped region, and the offset in it, each address-looking word points into. */
static void
rmm_snap_scan_classify(void)
{
    for (int i = 0; i < scan_nhits; i++) {
        scan_hit_t *h = &scan_hits[i];

        h->target = -1;
        for (uint32_t r = 0; (r < scan_region) && (r < SCAN_REGS); r++)
            if (!scan_regs[r].local && h->val >= scan_regs[r].base && h->val - scan_regs[r].base < scan_regs[r].len) {
                h->target = (int32_t) r;
                h->toff   = (uint32_t) (h->val - scan_regs[r].base);
                break;
            }
    }
}

/* -- relocation -----------------------------------------------------------------
   Each save also writes a table of address-like words and their targets. Restoring
   into another process fixes them up. See releasenotes.txt. */

#define RELOC_MAGIC 0x31434c52 /* "RLC1" */
#define RELOC_LABEL 32

typedef struct {
    uint32_t magic, nreg, nmod, nptr, nlab;
} reloc_hdr_t;

typedef struct {
    uint64_t base;
    uint64_t len;
    uint64_t local;
} reloc_reg_t;

typedef struct {
    uint64_t lo, hi, off;
    char     path[192];
} reloc_mod_t;

typedef struct {
    uint32_t region, off, kind, target; /* kind: 0 other, 1 module, 2 region */
    uint64_t b;                         /* offset into the module or region */
} reloc_ptr_t;

/* The blob: header, nreg regions, nmod modules, nptr pointers, a label index per
   region, then nlab labels. */
static size_t
reloc_blob_len(const reloc_hdr_t *h)
{
    return sizeof(*h) + (size_t) h->nreg * sizeof(reloc_reg_t) + (size_t) h->nmod * sizeof(reloc_mod_t) +
           (size_t) h->nptr * sizeof(reloc_ptr_t) + (size_t) h->nreg * sizeof(uint32_t) +
           (size_t) h->nlab * RELOC_LABEL;
}

typedef struct rmm_reloc {
    const reloc_reg_t *regs;
    const reloc_mod_t *mods;
    const reloc_ptr_t *ptrs;
    const uint32_t    *lab;     /* nreg: label index of each region */
    const char        *labels;  /* nlab x RELOC_LABEL */
    uint32_t           nreg, nmod, nptr, nlab;
    uint32_t          *first;   /* nreg + 1: first pointer entry of each region */
    uint64_t          *live;    /* live address of each region; 0 for a local */
    uint64_t          *modlive; /* live start of each module */
    uint32_t           cur;     /* regions visited */
    int                layout;  /* first pass: only learns the live address of every dump */
    char               why[200];
} rmm_reloc_t;

static int rmm_force_reloc;

void
rmm_snap_reloc_force(int on)
{
    rmm_force_reloc = on;
}

/* The live timers that sit in no dumped struct and no mapped file: a snapshot file cannot place them. Listed by
   callback (offset into its file's first mapping, for nm). */
static void
rmm_snap_unplaced_timers(char *out, size_t n)
{
    size_t used = 0;
    int    count = 0;

    out[0] = 0;
    for (const pc_timer_t *t = timer_head; t; t = t->next) {
        uintptr_t a      = (uintptr_t) t;
        int       placed = 0;

        for (uint32_t r = 0; (r < scan_region) && (r < SCAN_REGS) && !placed; r++)
            if (!scan_regs[r].local && (a >= scan_regs[r].base) && (a - scan_regs[r].base < scan_regs[r].len))
                placed = 1;
        for (int i = 0; (i < scan_nranges) && !placed; i++)
            if ((scan_ranges[i].path[0] == '/') && (a >= scan_ranges[i].lo) && (a < scan_ranges[i].hi))
                placed = 1;
        if (placed)
            continue;
        count++;
        if (used + 24 < n) {
            uintptr_t cb   = (uintptr_t) t->callback;
            uintptr_t base = 0;

            for (int i = 0; i < scan_nranges; i++)
                if ((cb >= scan_ranges[i].lo) && (cb < scan_ranges[i].hi)) {
                    base = scan_ranges[i].lo - scan_ranges[i].off;
                    break;
                }
            used += snprintf(out + used, n - used, "%scb+%#" PRIxPTR, used ? " " : "", base ? cb - base : cb);
        }
    }
    if (count > 6 && used + 12 < n)
        snprintf(out + used, n - used, " (%d)", count);
}

/* Table for the save that has just run its scan; NULL if it cannot be made. */
static uint8_t *
rmm_snap_reloc_build(size_t *outlen)
{
    int          modidx[SCAN_RANGES];
    int          nmod = 0;
    reloc_hdr_t  hdr;
    reloc_ptr_t *pt;
    uint32_t    *lab;
    const char **labs;
    uint32_t     nlab = 0;
    uint8_t     *blob, *w;
    size_t       len;

    if (scan_dropped || (scan_region > SCAN_REGS) || !scan_nranges)
        return NULL;
    rmm_snap_scan_classify();
    for (int i = 0; i < SCAN_RANGES; i++)
        modidx[i] = -1;
    pt   = calloc(scan_nhits ? scan_nhits : 1, sizeof(reloc_ptr_t));
    lab  = calloc(scan_region ? scan_region : 1, sizeof(uint32_t));
    labs = calloc(scan_region ? scan_region : 1, sizeof(char *));
    if (!pt || !lab || !labs) {
        free(pt);
        free(lab);
        free(labs);
        return NULL;
    }
    for (int i = 0; i < scan_nhits; i++) {
        const scan_hit_t *h = &scan_hits[i];

        pt[i].region = h->region;
        pt[i].off    = h->off;
        /* Into a dumped region first, else a global in a mapped file. */
        if (h->target >= 0) {
            pt[i].kind   = 2;
            pt[i].target = h->target;
            pt[i].b      = h->toff;
        } else if (scan_ranges[h->rng].path[0] == '/') {
            if (modidx[h->rng] < 0)
                modidx[h->rng] = nmod++;
            pt[i].kind   = 1;
            pt[i].target = modidx[h->rng];
            pt[i].b      = h->val - scan_ranges[h->rng].lo;
        }
    }
    /* Labels are the same few strings over and over: by pointer. */
    for (uint32_t r = 0; r < scan_region; r++) {
        const char *l = scan_reglabel[r] ? scan_reglabel[r] : "";
        uint32_t    k;

        for (k = 0; (k < nlab) && (labs[k] != l); k++)
            ;
        if (k == nlab)
            labs[nlab++] = l;
        lab[r] = k;
    }
    hdr.magic = RELOC_MAGIC;
    hdr.nreg  = scan_region;
    hdr.nmod  = nmod;
    hdr.nptr  = scan_nhits;
    hdr.nlab  = nlab;
    len       = reloc_blob_len(&hdr);
    if (!(blob = calloc(1, len))) {
        free(pt);
        free(lab);
        free(labs);
        return NULL;
    }
    w = blob;
    memcpy(w, &hdr, sizeof(hdr));
    w += sizeof(hdr);
    for (uint32_t r = 0; r < hdr.nreg; r++, w += sizeof(reloc_reg_t))
        memcpy(w, &scan_regs[r], sizeof(reloc_reg_t)); /* same layout */
    for (int i = 0; i < SCAN_RANGES; i++) {
        reloc_mod_t m = { 0 };

        if (modidx[i] < 0)
            continue;
        m.lo  = scan_ranges[i].lo;
        m.hi  = scan_ranges[i].hi;
        m.off = scan_ranges[i].off;
        snprintf(m.path, sizeof(m.path), "%s", scan_ranges[i].path);
        memcpy(w + modidx[i] * sizeof(reloc_mod_t), &m, sizeof(m));
    }
    w += nmod * sizeof(reloc_mod_t);
    memcpy(w, pt, hdr.nptr * sizeof(reloc_ptr_t));
    w += hdr.nptr * sizeof(reloc_ptr_t);
    memcpy(w, lab, hdr.nreg * sizeof(uint32_t));
    w += hdr.nreg * sizeof(uint32_t);
    for (uint32_t k = 0; k < nlab; k++)
        snprintf((char *) w + k * RELOC_LABEL, RELOC_LABEL, "%s", labs[k]);
    free(pt);
    free(lab);
    free(labs);
    *outlen = len;
    return blob;
}

static void
rmm_reloc_close(rmm_reloc_t *rl)
{
    if (!rl)
        return;
    free(rl->first);
    free(rl->live);
    free(rl->modlive);
    free(rl);
}

/* Reads the table and finds the saved modules in this process. NULL, with the
   reason in why, if the table does not fit or a module is not loaded. */
static rmm_reloc_t *
rmm_reloc_open(const uint8_t *blob, size_t len, char *why, size_t n)
{
    rmm_reloc_t *rl;
    reloc_hdr_t  hdr;

    if (!blob || (len < sizeof(hdr))) {
        snprintf(why, n, "the snapshot has no pointer table");
        return NULL;
    }
    memcpy(&hdr, blob, sizeof(hdr));
    if ((hdr.magic != RELOC_MAGIC) || (hdr.nreg > SCAN_REGS) || (hdr.nptr > SCAN_MAX) ||
        (hdr.nmod > SCAN_RANGES) || (hdr.nlab > hdr.nreg + 1) || (reloc_blob_len(&hdr) != len)) {
        snprintf(why, n, "the snapshot's pointer table is damaged");
        return NULL;
    }
    if (rmm_snap_scan_load_maps() < 0) {
        snprintf(why, n, "relocating a snapshot needs /proc/self/maps (Linux)");
        return NULL;
    }
    rl          = calloc(1, sizeof(*rl));
    rl->nreg    = hdr.nreg;
    rl->nmod    = hdr.nmod;
    rl->nptr    = hdr.nptr;
    rl->nlab    = hdr.nlab;
    rl->regs    = (const reloc_reg_t *) (blob + sizeof(hdr));
    rl->mods    = (const reloc_mod_t *) (rl->regs + hdr.nreg);
    rl->ptrs    = (const reloc_ptr_t *) (rl->mods + hdr.nmod);
    rl->lab     = (const uint32_t *) (rl->ptrs + hdr.nptr);
    rl->labels  = (const char *) (rl->lab + hdr.nreg);
    rl->first   = calloc(hdr.nreg + 1, sizeof(uint32_t));
    rl->live    = calloc(hdr.nreg ? hdr.nreg : 1, sizeof(uint64_t));
    rl->modlive = calloc(hdr.nmod ? hdr.nmod : 1, sizeof(uint64_t));
    for (uint32_t i = 0; i < hdr.nptr; i++) {
        const reloc_ptr_t *e = &rl->ptrs[i];

        if ((e->region >= hdr.nreg) || (i && (e->region < rl->ptrs[i - 1].region)) ||
            ((e->kind == 1) && (e->target >= hdr.nmod)) || ((e->kind == 2) && (e->target >= hdr.nreg)) ||
            (e->kind > 2) || ((uint64_t) e->off + 8 > rl->regs[e->region].len)) {
            snprintf(why, n, "the snapshot's pointer table is damaged");
            rmm_reloc_close(rl);
            return NULL;
        }
        rl->first[e->region + 1]++;
    }
    for (uint32_t r = 0; r < hdr.nreg; r++) {
        if (rl->lab[r] >= hdr.nlab) {
            snprintf(why, n, "the snapshot's pointer table is damaged");
            rmm_reloc_close(rl);
            return NULL;
        }
        rl->first[r + 1] += rl->first[r];
    }
    for (uint32_t m = 0; m < hdr.nmod; m++) {
        for (int i = 0; i < scan_nranges; i++)
            if (!strcmp(scan_ranges[i].path, rl->mods[m].path) && (scan_ranges[i].off == rl->mods[m].off)) {
                rl->modlive[m] = scan_ranges[i].lo;
                break;
            }
        if (!rl->modlive[m]) {
            snprintf(why, n, "the snapshot was taken with a different build (%.150s is not mapped here)",
                     rl->mods[m].path);
            rmm_reloc_close(rl);
            return NULL;
        }
    }
    return rl;
}

/* Which module or device made dump r. */
static const char *
rl_label(const rmm_reloc_t *rl, uint32_t r)
{
    if (r >= rl->nreg)
        return "?";
    return rl->labels + (size_t) rl->lab[r] * RELOC_LABEL;
}

/* The dump number within its module or device, from 0. */
static int
rl_nth(const rmm_reloc_t *rl, uint32_t r)
{
    uint32_t f = r;

    if (r >= rl->nreg)
        return -1;
    while (f && (rl->lab[f - 1] == rl->lab[r]))
        f--;
    return (int) (r - f);
}

static uint64_t
rmm_reloc_value(const rmm_reloc_t *rl, const reloc_ptr_t *e)
{
    if (e->kind == 1)
        return rl->modlive[e->target] + e->b;
    if (e->kind == 2 && rl->live[e->target])
        return rl->live[e->target] + e->b;
    return 0;
}

static int
rmm_reloc_layout(const rmm_snap_t *s)
{
    return s->rl && s->rl->layout;
}

static void
rmm_reloc_fail(rmm_snap_t *s, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s->rl->why, sizeof(s->rl->why), fmt, ap);
    va_end(ap);
    s->failed = 1;
}

/* A restoring dump of p, n bytes: the table's next region. */
static void
rmm_reloc_raw(rmm_snap_t *s, void *p, size_t n)
{
    rmm_reloc_t   *rl    = s->rl;
    uint32_t       r     = rl->cur++;
    int            local = s->local;
    const uint8_t *tmpl  = s->tmpl ? (const uint8_t *) s->tmpl : (const uint8_t *) p;
    uint64_t       keep_buf[256];
    uint64_t      *keep = keep_buf;
    uint32_t       a, b;

    s->local = 0;
    s->tmpl  = NULL;
    if ((r >= rl->nreg) || (rl->regs[r].len != n)) {
        if (r < rl->nreg)
            rmm_reloc_fail(s, "dump %u (%s #%d) is %zu bytes here, %" PRIu64 " in the snapshot", r, rl_label(rl, r),
                           rl_nth(rl, r), n, rl->regs[r].len);
        else
            rmm_reloc_fail(s, "more dumps here than the snapshot's %u", rl->nreg);
        return;
    }
    a = rl->first[r];
    b = rl->first[r + 1];
    if ((b - a > 256) && !(keep = malloc((size_t) (b - a) * sizeof(uint64_t)))) {
        rmm_reloc_fail(s, "out of memory");
        return;
    }
    for (uint32_t i = a; i < b; i++)
        memcpy(&keep[i - a], tmpl + rl->ptrs[i].off, sizeof(uint64_t));
    rl->live[r] = local ? 0 : (uintptr_t) p;
    memcpy(p, s->buf + s->pos, n);
    s->pos += n;
    for (uint32_t i = a; i < b; i++) {
        const reloc_ptr_t *e = &rl->ptrs[i];
        uint64_t           v = keep[i - a];

        if (local && (e->kind != 0)) {
            if (!(v = rmm_reloc_value(rl, e)) && rl->layout) {
                v = keep[i - a]; /* the first pass cannot know yet; nothing uses it */
            } else if (!v) {
                rmm_reloc_fail(s, "local dump %u (%s) offset %u points into dump %u (%s #%d), which has no live address",
                               r, rl_label(rl, r), e->off, e->target, rl_label(rl, e->target), rl_nth(rl, e->target));
                break;
            }
        } else if (local && (e->kind == 2) && (e->target == r)) {
            v = (uintptr_t) p + e->b;
        }
        memcpy((uint8_t *) p + e->off, &v, sizeof(v));
    }
    if (keep != keep_buf)
        free(keep);
}

/* After the last region: the module and region pointers, now every live
   address is known. 0, or -1 if the dumps did not match the table. */
static int
rmm_reloc_finish(rmm_snap_t *s)
{
    rmm_reloc_t *rl = s->rl;

    if (s->failed)
        return -1;
    if (rl->cur != rl->nreg) {
        snprintf(rl->why, sizeof(rl->why), "%u dumps here, %u in the snapshot", rl->cur, rl->nreg);
        return -1;
    }
    for (uint32_t i = 0; i < rl->nptr; i++) {
        const reloc_ptr_t *e = &rl->ptrs[i];
        uint64_t           v;

        if (!rl->live[e->region] || !e->kind)
            continue;
        if (e->kind == 2 && (e->target == e->region))
            v = rl->live[e->region] + e->b;
        else if (e->kind == 2 && (rl->live[e->target] == 0)) {
            snprintf(rl->why, sizeof(rl->why), "dump %u (%s) offset %u points into dump %u (%s), which has no live address",
                     e->region, rl_label(rl, e->region), e->off, e->target, rl_label(rl, e->target));
            return -1;
        } else if (e->kind == 2)
            v = rl->live[e->target] + e->b;
        else
            v = rl->modlive[e->target] + e->b;
        memcpy((uint8_t *) (uintptr_t) rl->live[e->region] + e->off, &v, sizeof(v));
    }
    return 0;
}

/* -- files ----------------------------------------------------------------------
   Snapshot file layout: header, then sections IDNT BILD RELC DISK DATA END.
   See releasenotes.txt. */

#define RMM_SNAP_FILE_MAGIC   "RMMSNAP1"
#define RMM_SNAP_FILE_VERSION 1
#define RMM_SNAP_TEXT_MAX     (1u << 20)

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t hdrlen;
    uint64_t tsc;
    char     tag[64];
} rmm_snap_filehdr_t;

typedef struct {
    char     id[4];
    uint32_t zero;
    uint64_t len;
} rmm_snap_sect_t;

uint64_t
rmm_snap_hash(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *) p;

    h ^= 0x9e3779b97f4a7c15ull;
    for (; n >= 8; n -= 8, b += 8) {
        uint64_t w;

        memcpy(&w, b, 8);
        h ^= w;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    for (; n; n--, b++) {
        h ^= *b;
        h *= 0x100000001b3ull;
    }
    return h;
}

/* What a process must have to take a snapshot of this one: a line per item. */
static char *
rmm_snap_ident(const rmm_snap_devref_t *devs, int ndevs)
{
    size_t n = 4096 + (size_t) ndevs * 96;
    char  *s = calloc(1, n);
    size_t u;

    if (!s)
        return NULL;
    u = snprintf(s, n, "format %d\nmachine %s\ncpu %s %s\nfpu %d\nmem %u\n", RMM_SNAP_FILE_VERSION,
                 machine_get_internal_name(), cpu_f ? cpu_f->internal_name : "?", cpu_s ? cpu_s->name : "?",
                 fpu_type, (unsigned) mem_size);
    for (int i = 0; (i < ndevs) && (u < n); i++) {
        const device_t *d = devs[i].dev;

        u += snprintf(s + u, n - u, "dev %d %s\n", i, d->internal_name ? d->internal_name : d->name);
    }
    fdd_rmm_snap_ident(s, n);
    cdrom_rmm_snap_ident(s, n);
    return s;
}

/* The mapped files the pointer table needs, each with its size and mtime. */
static char *
rmm_snap_build_text(const uint8_t *blob, size_t len)
{
    reloc_hdr_t        hdr;
    const reloc_mod_t *mods;
    size_t             n, u = 0;
    char              *s;

    if (!blob || (len < sizeof(hdr)))
        return NULL;
    memcpy(&hdr, blob, sizeof(hdr));
    if ((hdr.magic != RELOC_MAGIC) || (reloc_blob_len(&hdr) != len))
        return NULL;
    mods = (const reloc_mod_t *) (blob + sizeof(hdr) + (size_t) hdr.nreg * sizeof(reloc_reg_t));
    n    = 64 + (size_t) hdr.nmod * 256;
    if (!(s = calloc(1, n)))
        return NULL;
    for (uint32_t m = 0; m < hdr.nmod; m++) {
        struct stat st;
        char        path[sizeof(mods[m].path) + 1];

        snprintf(path, sizeof(path), "%s", mods[m].path);
        if (stat(path, &st))
            u += snprintf(s + u, n - u, "mod %s missing\n", path);
        else
            u += snprintf(s + u, n - u, "mod %s %" PRIu64 " %" PRId64 "\n", path, (uint64_t) st.st_size,
                          (int64_t) st.st_mtime);
    }
    return s;
}

/* Paths are 86Box's, not the client's: a relative path depends on 86Box's working directory. */
static int
rmm_snap_path_ok(const char *path, char *why, size_t n)
{
    if (!path_abs((char *) path)) {
        snprintf(why, n, "'%.200s' is not an absolute path", path);
        return 0;
    }
    return 1;
}

/* The first line of a not in b, for error 8. */
static void
rmm_snap_text_diff(const char *a, const char *b, const char *what, char *why, size_t n)
{
    const char *pa = a, *pb = b;

    while (*pa && *pb) {
        const char *ea = strchr(pa, '\n'), *eb = strchr(pb, '\n');
        size_t      la = ea ? (size_t) (ea - pa) : strlen(pa);
        size_t      lb = eb ? (size_t) (eb - pb) : strlen(pb);

        if ((la != lb) || memcmp(pa, pb, la)) {
            snprintf(why, n, "%s differs: here '%.*s', in the snapshot '%.*s'", what, (int) (la > 120 ? 120 : la), pa,
                     (int) (lb > 120 ? 120 : lb), pb);
            return;
        }
        pa += la + (ea ? 1 : 0);
        pb += lb + (eb ? 1 : 0);
    }
    if (*pa || *pb)
        snprintf(why, n, "%s differs: here '%.120s', in the snapshot '%.120s'", what, *pa ? pa : "(nothing)",
                 *pb ? pb : "(nothing)");
    else
        snprintf(why, n, "%s differs", what);
}

static int
rmm_snap_fsync(FILE *f)
{
    if (fflush(f))
        return -1;
#ifdef _WIN32
    return _commit(_fileno(f));
#else
    return fsync(fileno(f));
#endif
}

static int
rmm_snap_sect(FILE *f, const char *id, uint64_t len)
{
    rmm_snap_sect_t h = { { 0 }, 0, len };

    memcpy(h.id, id, 4);
    return (fwrite(&h, sizeof(h), 1, f) == 1) ? 0 : -1;
}

static int
rmm_snap_sect_bytes(FILE *f, const char *id, const void *p, size_t len)
{
    if (rmm_snap_sect(f, id, len))
        return -1;
    return (!len || (fwrite(p, 1, len, f) == len)) ? 0 : -1;
}

/* Writes snap to path through a .part file renamed at the end. Returns 0, 1 if
   cancelled, -1 on error. */
static int
rmm_snap_write_file(rmm_snapshot_t *snap, const char *path, char *err, size_t n)
{
    char               tmp[sizeof(snap->path) + 8];
    rmm_snap_filehdr_t hdr;
    FILE              *f, *src = NULL;
    uint64_t           len  = snap->data.buf ? snap->data.len : snap->filelen;
    uint64_t           done = 0;
    uint8_t           *chunk_buf = NULL;
    int                rc   = 0;

    if (strlen(path) >= sizeof(snap->path)) {
        snprintf(err, n, "snapshot file path too long");
        return -1;
    }
    if (!snap->data.buf) {
        if (!(src = plat_fopen(snap->path, "rb")) || fseeko64(src, (int64_t) snap->data_off, SEEK_SET) ||
            !(chunk_buf = malloc(RMM_SNAP_CHUNK))) {
            snprintf(err, n, "cannot read %s: %s", snap->path, strerror(errno));
            if (src)
                fclose(src);
            return -1;
        }
    }
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    if (!(f = plat_fopen(tmp, "wb"))) {
        snprintf(err, n, "cannot create snapshot file: %s", strerror(errno));
        if (src)
            fclose(src);
        free(chunk_buf);
        return -1;
    }
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, RMM_SNAP_FILE_MAGIC, 8);
    hdr.version = RMM_SNAP_FILE_VERSION;
    hdr.hdrlen  = sizeof(hdr);
    hdr.tsc     = snap->tsc;
    snprintf(hdr.tag, sizeof(hdr.tag), "%s", snap->tag);
    if ((fwrite(&hdr, sizeof(hdr), 1, f) != 1) ||
        rmm_snap_sect_bytes(f, "IDNT", snap->ident, snap->ident ? strlen(snap->ident) : 0) ||
        rmm_snap_sect_bytes(f, "BILD", snap->build, snap->build ? strlen(snap->build) : 0) ||
        rmm_snap_sect_bytes(f, "RELC", snap->reloc, snap->reloc ? snap->reloclen : 0) ||
        rmm_snap_sect(f, "DISK", hdd_image_rmm_file_len(snap->disks)) || hdd_image_rmm_write(snap->disks, f) ||
        rmm_snap_sect(f, "DATA", len)) {
        snprintf(err, n, "write failed: %s", strerror(errno));
        rc = -1;
    }
    if (!rc && (path == snap->path))
        snap->data_off = (uint64_t) ftello64(f); /* the writer's own file: restore reads it back */
    while (!rc && (done < len)) {
        size_t      chunk = (len - done > RMM_SNAP_CHUNK) ? RMM_SNAP_CHUNK : (size_t) (len - done);
        const void *p     = snap->data.buf ? (const void *) (snap->data.buf + done) : (const void *) chunk_buf;

        if (snap->cancel) {
            rc = 1;
            break;
        }
        if (src && (fread(chunk_buf, 1, chunk, src) != chunk)) {
            snprintf(err, n, "%s is truncated", snap->path);
            rc = -1;
            break;
        }
        if (fwrite(p, 1, chunk, f) != chunk) {
            snprintf(err, n, "write failed: %s", strerror(errno));
            rc = -1;
            break;
        }
        done += chunk;
    }
    if (!rc && rmm_snap_sect(f, "END ", 0)) {
        snprintf(err, n, "write failed: %s", strerror(errno));
        rc = -1;
    }
    if (!rc && rmm_snap_fsync(f)) {
        snprintf(err, n, "fsync failed: %s", strerror(errno));
        rc = -1;
    }
    if (fclose(f) && !rc) {
        snprintf(err, n, "close failed: %s", strerror(errno));
        rc = -1;
    }
    if (src)
        fclose(src);
    free(chunk_buf);
    if (!rc) {
#ifdef _WIN32
        remove(path);
#endif
        if (rename(tmp, path)) {
            snprintf(err, n, "rename failed: %s", strerror(errno));
            rc = -1;
        }
    }
    if (rc)
        remove(tmp);
    return rc;
}

/* The sections of a snapshot file, found by id: offset of the bytes and length. */
typedef struct {
    rmm_snap_filehdr_t hdr;
    uint64_t           off[5], len[5]; /* IDNT BILD RELC DISK DATA */
} rmm_snap_layout_t;

static const char rmm_snap_sect_ids[5][5] = { "IDNT", "BILD", "RELC", "DISK", "DATA" };

static FILE *
rmm_snap_open_file(const char *path, rmm_snap_layout_t *lay, char *why, size_t n)
{
    FILE    *f;
    uint64_t size;
    int      found = 0;

    memset(lay, 0, sizeof(*lay));
    if (!(f = plat_fopen(path, "rb"))) {
        snprintf(why, n, "cannot open %s: %s", path, strerror(errno));
        return NULL;
    }
    fseeko64(f, 0, SEEK_END);
    size = (uint64_t) ftello64(f);
    fseeko64(f, 0, SEEK_SET);
    if ((fread(&lay->hdr, sizeof(lay->hdr), 1, f) != 1) || memcmp(lay->hdr.magic, "RMMSNAP", 7)) {
        snprintf(why, n, "%s is not a snapshot file", path);
        fclose(f);
        return NULL;
    }
    if ((lay->hdr.version != RMM_SNAP_FILE_VERSION) || (lay->hdr.hdrlen < sizeof(lay->hdr))) {
        snprintf(why, n, "%s is a version %u snapshot file; this 86Box reads version %d", path, lay->hdr.version,
                 RMM_SNAP_FILE_VERSION);
        fclose(f);
        return NULL;
    }
    lay->hdr.tag[sizeof(lay->hdr.tag) - 1] = 0;
    fseeko64(f, lay->hdr.hdrlen, SEEK_SET);
    for (;;) {
        rmm_snap_sect_t s;
        uint64_t        at;

        if (fread(&s, sizeof(s), 1, f) != 1)
            break;
        at = (uint64_t) ftello64(f);
        if (!memcmp(s.id, "END ", 4)) {
            found |= 0x100;
            break;
        }
        if (s.len > size - at)
            break;
        for (int k = 0; k < 5; k++)
            if (!memcmp(s.id, rmm_snap_sect_ids[k], 4)) {
                lay->off[k] = at;
                lay->len[k] = s.len;
                found |= 1 << k;
            }
        fseeko64(f, (int64_t) (at + s.len), SEEK_SET);
    }
    if (found != 0x11f) {
        snprintf(why, n, "%s is truncated or damaged", path);
        fclose(f);
        return NULL;
    }
    return f;
}

/* Section k of an open file as a NUL-terminated buffer; NULL if over max. */
static uint8_t *
rmm_snap_read_sect(FILE *f, const rmm_snap_layout_t *lay, int k, uint64_t max)
{
    uint8_t *p;

    if ((lay->len[k] > max) || !(p = calloc(1, (size_t) lay->len[k] + 1)))
        return NULL;
    fseeko64(f, (int64_t) lay->off[k], SEEK_SET);
    if (lay->len[k] && (fread(p, 1, (size_t) lay->len[k], f) != lay->len[k])) {
        free(p);
        return NULL;
    }
    return p;
}

/* The snapshot's device/RAM buffer: the in-memory one, or read back from its file. */
static int
rmm_snap_load_buf(const rmm_snapshot_t *snap, rmm_snap_t *out, char *why, size_t n)
{
    rmm_snap_layout_t lay;
    FILE             *f;
    uint8_t          *buf;

    memset(out, 0, sizeof(*out));
    if (snap->data.buf) {
        *out = snap->data;
        return 0;
    }
    if (!(f = rmm_snap_open_file(snap->path, &lay, why, n)))
        return -1;
    if (strcmp(lay.hdr.tag, snap->filetag) || (lay.len[4] != snap->filelen) || (lay.off[4] != snap->data_off)) {
        fclose(f);
        snprintf(why, n, "%s is no longer the file of snapshot '%s'", snap->path, snap->tag);
        return -1;
    }
    if (!(buf = malloc(snap->filelen ? snap->filelen : 1))) {
        fclose(f);
        snprintf(why, n, "out of memory reading %" PRIu64 " bytes of snapshot '%s'", (uint64_t) snap->filelen, snap->tag);
        return -1;
    }
    fseeko64(f, (int64_t) snap->data_off, SEEK_SET);
    if (fread(buf, 1, snap->filelen, f) != snap->filelen) {
        fclose(f);
        free(buf);
        snprintf(why, n, "%s is truncated", snap->path);
        return -1;
    }
    fclose(f);
    out->buf = buf;
    out->len = out->cap = snap->filelen;
    return 1; /* the caller frees out->buf */
}

/* The next rmm_snap_raw is guest data: not scanned for pointers. */
void
rmm_snap_data(rmm_snap_t *s)
{
    s->data = 1;
}

/* The next rmm_snap_raw is of a local variable. tmpl is the live object supplying the
   non-state pointers. */
void
rmm_snap_local(rmm_snap_t *s, const void *tmpl)
{
    s->local = 1;
    s->tmpl  = tmpl;
}

/* A save into a scratch buffer that records the address-looking words. Returns
   how many, or -1 with a reason. Needs the machine paused. */
int
rmm_snap_ptrscan(char *why, size_t n)
{
    rmm_snap_t         d = { 0 };
    rmm_snap_devref_t *devs;
    int                ndevs;

    if (rmm_snap_unsupported(why, n))
        return -1;
    if (rmm_snap_scan_load_maps() < 0) {
        snprintf(why, n, "the pointer scan needs /proc/self/maps (Linux)");
        return -1;
    }
    ide_wait_for_async_reads();
    devs         = rmm_snap_devtable(&ndevs);
    scan_nhits   = 0;
    scan_dropped = 0;
    scan_region  = 0;
    scan_on      = 1;
    rmm_snap_all(&d, devs, ndevs);
    scan_on = 0;
    rmm_snap_scan_classify();
    free(d.buf);
    free(devs);
    if (d.failed) {
        snprintf(why, n, "out of memory");
        return -1;
    }
    return scan_nhits + scan_dropped;
}

int
rmm_snap_ptrscan_region(int i, const char **label, uint64_t *base, uint64_t *len, int *local)
{
    if (i < 0 || (uint32_t) i >= scan_region || i >= SCAN_REGS)
        return 0;
    *label = scan_reglabel[i];
    *base  = scan_regs[i].base;
    *len   = scan_regs[i].len;
    *local = (int) scan_regs[i].local;
    return 1;
}

int
rmm_snap_ptrscan_hit(int i, const char **label, unsigned *region, unsigned *rlen, unsigned *off, int *code,
                     uint64_t *val, uint64_t *base, int *target, unsigned *toff)
{
    if (i < 0 || i >= scan_nhits)
        return 0;
    *label  = scan_hits[i].label;
    *region = scan_hits[i].region;
    *rlen   = scan_hits[i].rlen;
    *off    = scan_hits[i].off;
    *code   = scan_hits[i].code;
    *val    = scan_hits[i].val;
    *base   = scan_hits[i].base;
    *target = scan_hits[i].target;
    *toff   = scan_hits[i].toff;
    return 1;
}

/* Puts snap in a free slot, replacing tag's. 0, or error 8; the caller frees. */
static int
rmm_snap_insert(rmm_snapshot_t *snap, char *why, size_t n)
{
    int i;

    /* Saving an existing tag replaces it; the disk overlay stays armed. */
    if ((i = rmm_snap_find(snap->tag)) >= 0) {
        rmm_snap_release(rmm_snaps[i]);
        rmm_snaps[i] = NULL;
    }
    for (i = 0; (i < RMM_SNAP_MAX) && rmm_snaps[i]; i++)
        ;
    if (i == RMM_SNAP_MAX) {
        snprintf(why, n, "at most %d snapshots", RMM_SNAP_MAX);
        return RMM_E_CANNOT;
    }
    rmm_snaps[i] = snap;
    snap->seq    = ++w_seq;
    return 0;
}

int
rmm_snap_save(const char *tag, char *why, size_t n)
{
    rmm_snapshot_t *snap;

    if (rmm_snap_unsupported(why, n))
        return RMM_E_CANNOT;
    rmm_snap_writer_init();
    rmm_snap_reap();
    if (rmm_snap_writer_on()) {
        size_t held   = rmm_snap_inflight();
        size_t want   = w_last_len ? w_last_len : ((size_t) mem_size << 10);
        size_t budget = w_budget ? w_budget : 2 * ((size_t) mem_size << 10) + (256u << 20);

        /* A lone snapshot is always allowed; the cap is on a backlog. */
        if (held && (held + want > budget)) {
            snprintf(why, n, "snapshot writer busy: %zu MB not yet on disk (budget %zu MB)",
                     held >> 20, budget >> 20);
            return RMM_E_CANNOT;
        }
    }
    /* A read started for the live machine finishes into its buffer later. */
    ide_wait_for_async_reads();

    snap = calloc(1, sizeof(rmm_snapshot_t));
    /* Sized up front: growing a 1 GB buffer by doubling copies it repeatedly
       while the guest is paused. */
    {
        size_t est = (w_last_len ? w_last_len : ((size_t) mem_size << 10)) + (16u << 20);

        if ((snap->data.buf = malloc(est)))
            snap->data.cap = est;
    }
    snprintf(snap->tag, sizeof(snap->tag), "%s", tag);
    snap->tsc        = tsc;
    snap->generation = rmm_hard_resets;
    snap->devs       = rmm_snap_devtable(&snap->ndevs);
    snap->ident      = rmm_snap_ident(snap->devs, snap->ndevs);
    fdd_rmm_snap_media(snap->media);
    cdrom_rmm_snap_media(snap->cdmedia);
    if (rmm_snap_scan_load_maps() == 0) {
        scan_nhits   = 0;
        scan_dropped = 0;
        scan_region  = 0;
        scan_on      = 1;
    }
    rmm_snap_all(&snap->data, snap->devs, snap->ndevs);
    if (scan_on) {
        scan_on     = 0;
        rmm_snap_unplaced_timers(snap->unplaced, sizeof(snap->unplaced));
        snap->reloc = rmm_snap_reloc_build(&snap->reloclen);
        snap->build = rmm_snap_build_text(snap->reloc, snap->reloclen);
    }
    if (!snap->data.failed)
        snap->disks = hdd_image_rmm_save();
    if (snap->data.failed || !snap->disks || !snap->ident) {
        snprintf(why, n, "out of memory");
        rmm_snap_free(snap);
        rmm_snap_disarm_if_none();
        return RMM_E_CANNOT;
    }
    /* The writer's file records the image hash, taken once per overlay. */
    if (rmm_snap_writer_on())
        hdd_image_rmm_hash(snap->disks, why, n);
    why[0] = 0;

    if (rmm_snap_insert(snap, why, n)) {
        rmm_snap_free(snap);
        rmm_snap_disarm_if_none();
        return RMM_E_CANNOT;
    }
    w_last_len = snap->data.len;
    rmm_snap_enqueue(snap);
    return 0;
}

int
rmm_snap_restore(const char *tag, char *why, size_t n)
{
    rmm_snapshot_t    *snap;
    rmm_snap_devref_t *now;
    int                nnow;
    int                same;
    int                i = rmm_snap_find(tag);
    rmm_snap_t         d;
    int                loaded;
    rmm_reloc_t       *rl         = NULL;
    char               why_rl[256] = "";

    if (i < 0) {
        snprintf(why, n, "no snapshot '%s'", tag);
        return RMM_E_NO_SNAP;
    }
    snap = rmm_snaps[i];
    rmm_snap_reap();

    /* A reset re-creates devices, so older snapshots cannot restore. */
    now  = rmm_snap_devtable(&nnow);
    same = (snap->generation == rmm_hard_resets) && (nnow == snap->ndevs) &&
           !memcmp(now, snap->devs, nnow * sizeof(rmm_snap_devref_t));
    free(now);
    if (!same) {
        snprintf(why, n, "the machine was reset after snapshot '%s' was %s", tag, snap->foreign ? "loaded" : "taken");
        return RMM_E_CANNOT;
    }
    {
        void *media[2 * FDD_NUM];

        fdd_rmm_snap_media(media);
        if (memcmp(media, snap->media, sizeof(media))) {
            snprintf(why, n, "floppy media were changed after snapshot '%s' was taken", tag);
            return RMM_E_CANNOT;
        }
    }
    {
        uint32_t cdmedia[CDROM_NUM];

        cdrom_rmm_snap_media(cdmedia);
        if (memcmp(cdmedia, snap->cdmedia, sizeof(cdmedia))) {
            snprintf(why, n, "a CD-ROM disc was inserted or ejected after snapshot '%s' was taken", tag);
            return RMM_E_CANNOT;
        }
    }
    if (hdd_image_rmm_check(snap->disks, why, n)) {
        size_t used = strlen(why);

        snprintf(why + used, n - used, " after snapshot '%s' was taken", tag);
        return RMM_E_CANNOT;
    }
    /* A file snapshot holds another process's addresses: always through the pointer table. */
    if ((rmm_force_reloc || snap->foreign) && !(rl = rmm_reloc_open(snap->reloc, snap->reloclen, why, n)))
        return RMM_E_CANNOT;

    /* Everything that can fail comes before the first change to the machine. */
    rmm_snap_lock();
    loaded = rmm_snap_load_buf(snap, &d, why, n);
    if (loaded) /* read back from the file, or failed: nothing shared with the writer */
        rmm_snap_unlock();
    if (loaded < 0) {
        rmm_reloc_close(rl);
        return RMM_E_CANNOT;
    }
    /* loaded == 0: restoring from snap->data.buf; the lock stays held against the writer. */

    ide_wait_for_async_reads();
    hdd_image_rmm_restore(snap->disks);

    rmm_snap_timers_unlink();
    d.restore = 1;
    d.pos     = 0;
    d.failed  = 0;
    d.rl      = rl;
    if (d.rl) {
        /* First pass: where every dump lives now. It writes what the second one writes again. */
        d.rl->layout = 1;
        rmm_snap_all(&d, snap->devs, snap->ndevs);
        if (d.failed)
            fatal("RMM: snapshot '%s' did not lay out (%zu of %zu bytes): %s\n", tag, d.pos, d.len, d.rl->why);
        d.rl->layout = 0;
        d.rl->cur    = 0;
        d.pos        = 0;
        rmm_snap_timers_unlink();
    }
    rmm_snap_all(&d, snap->devs, snap->ndevs);
    if (d.rl && !d.failed && rmm_reloc_finish(&d))
        d.failed = 1;
    if (d.rl)
        snprintf(why_rl, sizeof(why_rl), "%s", d.rl->why);
    rmm_reloc_close(d.rl);
    if (d.failed || (d.pos != d.len))
        fatal("RMM: snapshot '%s' did not restore cleanly (%zu of %zu bytes)%s%s\n", tag, d.pos, d.len,
              why_rl[0] ? ": " : "", why_rl);
    if (loaded)
        free(d.buf);
    else
        rmm_snap_unlock();
    mem_rmm_snap_done();
    return 0;
}

int
rmm_snap_export(const char *tag, const char *path, char *why, size_t n)
{
    rmm_snapshot_t *snap;
    char            err[256] = "";
    int             i        = rmm_snap_find(tag);
    int             rc;

    if (i < 0) {
        snprintf(why, n, "no snapshot '%s'", tag);
        return RMM_E_NO_SNAP;
    }
    snap = rmm_snaps[i];
    if (!rmm_snap_path_ok(path, why, n))
        return RMM_E_CANNOT;
    if (snap->unplaced[0]) {
        snprintf(why, n, "snapshot '%s' holds timers that live in no saved struct, which a file cannot place (callbacks: %s)",
                 tag, snap->unplaced);
        return RMM_E_CANNOT;
    }
    if (!snap->reloc || !snap->build) {
        snprintf(why, n, "snapshot '%s' has no pointer table (it needs /proc/self/maps, Linux), "
                 "so no other process could load it", tag);
        return RMM_E_CANNOT;
    }
    if (hdd_image_rmm_hash(snap->disks, why, n))
        return RMM_E_CANNOT;
    rmm_snap_lock(); /* keeps the writer from dropping data.buf meanwhile */
    rc = rmm_snap_write_file(snap, path, err, sizeof(err));
    rmm_snap_unlock();
    if (rc) {
        snprintf(why, n, "%s: %s", path, err[0] ? err : "cancelled");
        return RMM_E_CANNOT;
    }
    return 0;
}

int
rmm_snap_load(const char *path, const char *tag, char *tag_out, size_t tn, char *why, size_t n)
{
    rmm_snap_layout_t lay;
    rmm_snapshot_t   *snap;
    rmm_reloc_t      *rl;
    FILE             *f;
    char             *ident = NULL, *build = NULL, *mine = NULL, *built = NULL;
    uint8_t          *reloc = NULL;
    void             *disks = NULL;
    int               ndevs;
    int               rc = RMM_E_CANNOT;

    if (!rmm_snap_path_ok(path, why, n) || rmm_snap_unsupported(why, n))
        return RMM_E_CANNOT;
    if (strlen(path) >= sizeof(snap->path)) {
        snprintf(why, n, "path too long");
        return RMM_E_CANNOT;
    }
    if (!(f = rmm_snap_open_file(path, &lay, why, n)))
        return RMM_E_CANNOT;
    if (!tag || !tag[0])
        tag = lay.hdr.tag;
    snap = calloc(1, sizeof(rmm_snapshot_t));
    if (!snap || !(ident = (char *) rmm_snap_read_sect(f, &lay, 0, RMM_SNAP_TEXT_MAX)) ||
        !(build = (char *) rmm_snap_read_sect(f, &lay, 1, RMM_SNAP_TEXT_MAX)) ||
        !(reloc = rmm_snap_read_sect(f, &lay, 2, 64u << 20))) {
        snprintf(why, n, "%s: a section is damaged or too large", path);
        goto out;
    }

    /* This process must be the same build with the same machine. */
    snap->devs = rmm_snap_devtable(&ndevs);
    snap->ndevs = ndevs;
    mine        = rmm_snap_ident(snap->devs, ndevs);
    if (!mine || strcmp(mine, ident)) {
        rmm_snap_text_diff(mine ? mine : "", ident, "the machine", why, n);
        goto out;
    }
    built = rmm_snap_build_text(reloc, (size_t) lay.len[2]);
    if (!built || strcmp(built, build)) {
        if (built)
            rmm_snap_text_diff(built, build, "the build", why, n);
        else
            snprintf(why, n, "%s has no usable pointer table", path);
        goto out;
    }
    if (!(rl = rmm_reloc_open(reloc, (size_t) lay.len[2], why, n)))
        goto out;
    rmm_reloc_close(rl);
    /* Last: it reads the whole of every hard disk image. */
    fseeko64(f, (int64_t) lay.off[3], SEEK_SET);
    if (!(disks = hdd_image_rmm_import(f, lay.len[3], why, n)))
        goto out;

    snprintf(snap->tag, sizeof(snap->tag), "%s", tag);
    snprintf(snap->filetag, sizeof(snap->filetag), "%s", lay.hdr.tag);
    snprintf(snap->path, sizeof(snap->path), "%s", path);
    snap->tsc        = lay.hdr.tsc;
    snap->generation = rmm_hard_resets;
    fdd_rmm_snap_media(snap->media);
    cdrom_rmm_snap_media(snap->cdmedia);
    snap->disks    = disks;
    snap->reloc    = reloc;
    snap->reloclen = (size_t) lay.len[2];
    snap->ident    = ident;
    snap->build    = build;
    snap->foreign  = 1;
    snap->external = 1;
    snap->data_off = lay.off[4];
    snap->filelen  = (size_t) lay.len[4];
    snap->state    = SNAP_DISK;
    disks = NULL;
    reloc = NULL;
    ident = build = NULL;
    if (rmm_snap_insert(snap, why, n))
        goto out;
    snprintf(tag_out, tn, "%s", snap->tag);
    snap = NULL;
    rc   = 0;

out:
    fclose(f);
    if (snap)
        rmm_snap_free(snap);
    hdd_image_rmm_free(disks);
    free(reloc);
    free(ident);
    free(build);
    free(mine);
    free(built);
    if (rc)
        rmm_snap_disarm_if_none();
    return rc;
}

int
rmm_snap_delete(const char *tag)
{
    int i = rmm_snap_find(tag);

    if (i < 0)
        return RMM_E_NO_SNAP;
    rmm_snap_release(rmm_snaps[i]);
    rmm_snaps[i] = NULL;
    rmm_snap_disarm_if_none();
    return 0;
}

int
rmm_snap_slots(void)
{
    return RMM_SNAP_MAX;
}

/* NULL for an empty slot; i runs 0..rmm_snap_slots()-1. */
const char *
rmm_snap_tag(int i, uint64_t *tsc)
{
    if ((i < 0) || (i >= RMM_SNAP_MAX) || !rmm_snaps[i])
        return NULL;
    *tsc = rmm_snaps[i]->tsc;
    return rmm_snaps[i]->tag;
}
