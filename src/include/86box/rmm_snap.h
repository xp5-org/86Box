/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Remote machine monitor snapshots. Each module has one visitor
 *          serving save and restore; rmm_snap_raw() appends or copies back.
 *          A snapshot restores only into the reset generation it was taken in.
 */
#ifndef EMU_RMM_SNAP_H
#define EMU_RMM_SNAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rmm_snap_t rmm_snap_t;

/* A device's visitor, given the priv its init returned. */
typedef void (*rmm_snap_fn)(rmm_snap_t *s, void *priv);

extern void rmm_snap_raw(rmm_snap_t *s, void *p, size_t n);
extern int  rmm_snap_restoring(const rmm_snap_t *s);
/* The next rmm_snap_raw is of a local variable: fixed up at once. */
extern void rmm_snap_local(rmm_snap_t *s, const void *tmpl);
/* The next rmm_snap_raw is guest data: not scanned for pointers. */
extern void rmm_snap_data(rmm_snap_t *s);

#define RMM_SNAP_VAR(s, v) rmm_snap_raw((s), (void *) &(v), sizeof(v))

/* Core modules, visited in this order by rmm_snap.c. */
extern void cpu_rmm_snap(rmm_snap_t *s);
extern void cpu386_common_rmm_snap(rmm_snap_t *s);
extern void cpu386_dynarec_rmm_snap(rmm_snap_t *s);
extern void x86_rmm_snap(rmm_snap_t *s);
extern void x87_rmm_snap(rmm_snap_t *s);
extern void mem_rmm_snap(rmm_snap_t *s);
extern void io_rmm_snap(rmm_snap_t *s);
extern void pic_rmm_snap(rmm_snap_t *s);
extern void dma_rmm_snap(rmm_snap_t *s);
extern void pci_rmm_snap(rmm_snap_t *s);
extern void pit_rmm_snap(rmm_snap_t *s);
extern void ppi_rmm_snap(rmm_snap_t *s);
extern void nvr_rmm_snap(rmm_snap_t *s);
extern void keyboard_rmm_snap(rmm_snap_t *s);
extern void smram_rmm_snap(rmm_snap_t *s);
extern void fdd_rmm_snap(rmm_snap_t *s);
extern void hdd_image_rmm_snap(rmm_snap_t *s);
extern void cdrom_rmm_snap(rmm_snap_t *s);
/* After every visitor has run: rebuild memory lookups, drop translated code. */
extern void mem_rmm_snap_done(void);
extern void network_rmm_snap(rmm_snap_t *s);

/* Helpers for device visitors. */
extern void io_trap_rmm_snap(rmm_snap_t *s, void *trap);

/* Hard disk image overlay in hdd_image.c. */
extern void       *hdd_image_rmm_save(void);
extern const char *hdd_image_rmm_check(const void *disks, char *why, size_t n);
extern void        hdd_image_rmm_restore(const void *disks);
extern void        hdd_image_rmm_free(void *disks);
extern void        hdd_image_rmm_disarm(void);
/* Snapshot files: image blocks and base hash. */
extern int         hdd_image_rmm_hash(void *disks, char *why, size_t n);
extern uint64_t    hdd_image_rmm_file_len(const void *disks);
extern int         hdd_image_rmm_write(const void *disks, FILE *f);
extern void       *hdd_image_rmm_import(FILE *f, uint64_t len, char *why, size_t n);
/* One line per drive appended to out, e.g. "fdd 0 raw 1474560 rw". */
extern void        fdd_rmm_snap_ident(char *out, size_t n);
extern void        cdrom_rmm_snap_ident(char *out, size_t n);
/* 64-bit hash of n bytes, continuing from h; 0 starts. Not cryptographic. */
extern uint64_t    rmm_snap_hash(uint64_t h, const void *p, size_t n);

/* What a snapshot cannot cover yet: NULL, or the reason. */
extern const char *ide_rmm_snap_blocked(void);
extern const char *fdd_rmm_snap_blocked(void);
extern const char *cdrom_rmm_snap_blocked(void);
/* The mounted floppy media objects, 2 per drive. */
extern void fdd_rmm_snap_media(void *sig[]);
/* A count per CD-ROM drive, bumped by every disc open/close. */
extern void cdrom_rmm_snap_media(uint32_t sig[]);

/* Device visitors; rmm_snap.c maps each to its device_t. */
extern void nvr_at_rmm_snap(rmm_snap_t *s, void *priv);
extern void port_6x_rmm_snap(rmm_snap_t *s, void *priv);
extern void kbc_at_rmm_snap(rmm_snap_t *s, void *priv);
extern void keyboard_at_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_85c496_rmm_snap(rmm_snap_t *s, void *priv);
extern void row_rmm_snap(rmm_snap_t *s, void *priv);
extern void w837x7_rmm_snap(rmm_snap_t *s, void *priv);
extern void fdc_rmm_snap(rmm_snap_t *s, void *priv);
extern void serial_rmm_snap(rmm_snap_t *s, void *priv);
extern void lpt_rmm_snap(rmm_snap_t *s, void *priv);
extern void gameport_rmm_snap(rmm_snap_t *s, void *priv);
extern void sst_rmm_snap(rmm_snap_t *s, void *priv);
extern void intel_flash_rmm_snap(rmm_snap_t *s, void *priv);
extern void intel_flash_e28f0xx_rmm_snap(rmm_snap_t *s, void *priv);
extern void catalyst_flash_rmm_snap(rmm_snap_t *s, void *priv);
extern void amd_flash_rmm_snap(rmm_snap_t *s, void *priv);
extern void prt_text_rmm_snap(rmm_snap_t *s, void *priv);
extern void prt_ps_rmm_snap(rmm_snap_t *s, void *priv);
struct ad1848_t;
extern void ad1848_rmm_snap_embedded(rmm_snap_t *s, struct ad1848_t *a, void *card, size_t card_size);
extern void snd_azt2316a_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_azt2320_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_cs423x_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_wss_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_ymf701_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_ymf71x_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_mpu401_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_adlibgold_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_sensation_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_ad1816_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_jazz16_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_ide_pdc202xx_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_xta_ps1_rmm_snap(rmm_snap_t *s, void *priv);
extern void mcide_rmm_snap(rmm_snap_t *s, void *priv);
extern void xi8088_rmm_snap(rmm_snap_t *s, void *priv);
extern void europc_rmm_snap(rmm_snap_t *s, void *priv);
extern void tvis_memcard_rmm_snap(rmm_snap_t *s, void *priv);
extern void ast_nvr_rmm_snap(rmm_snap_t *s, void *priv);
extern void inboard386_rmm_snap(rmm_snap_t *s, void *priv);
extern void mxo_rmm_snap(rmm_snap_t *s, void *priv);
extern void psq_rmm_snap(rmm_snap_t *s, void *priv);
extern void softpower_rmm_snap(rmm_snap_t *s, void *priv);
struct softpower_control_t;
extern void softpower_control_rmm_snap(rmm_snap_t *s, struct softpower_control_t *control);
extern void ibm5140_rmm_snap(rmm_snap_t *s, void *priv);
extern void modem_rmm_snap(rmm_snap_t *s, void *priv);
extern void prt_escp_rmm_snap(rmm_snap_t *s, void *priv);
extern const char *modem_rmm_snap_blocked(void);
extern void cassette_rmm_snap(rmm_snap_t *s, void *priv);
extern const char *cassette_rmm_snap_blocked(void);
extern void pcjx_rmm_snap(rmm_snap_t *s, void *priv);
extern void pcjr_rmm_snap(rmm_snap_t *s, void *priv);
extern void m24_kbd_rmm_snap(rmm_snap_t *s, void *priv);
extern void xma_rmm_snap(rmm_snap_t *s, void *priv);
extern void zenith_scratchpad_rmm_snap(rmm_snap_t *s, void *priv);
extern void bugger_rmm_snap(rmm_snap_t *s, void *priv);
extern void postcard_rmm_snap(rmm_snap_t *s, void *priv);
extern void unittester_rmm_snap(rmm_snap_t *s, void *priv);
extern void mouse_ps2_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_tulip_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_rtl8139_rmm_snap(rmm_snap_t *s, void *priv);
extern void i8255x_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_3c509b_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_3c503_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_dp83815_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_pcnet_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_smc_epic100_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_wd8003_rmm_snap(rmm_snap_t *s, void *priv);
extern void ct_82c100_rmm_snap(rmm_snap_t *s, void *priv);
extern void acc2036_rmm_snap(rmm_snap_t *s, void *priv);
extern void ali1541_rmm_snap(rmm_snap_t *s, void *priv);
extern void compaq_386_rmm_snap(rmm_snap_t *s, void *priv);
extern void cs8220_rmm_snap(rmm_snap_t *s, void *priv);
extern void headland_rmm_snap(rmm_snap_t *s, void *priv);
extern void neat_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti283_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti498_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti55x_rmm_snap(rmm_snap_t *s, void *priv);
extern void scat_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_530_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_530_h2p_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5511_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5581_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5600_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5600_h2p_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_85c4xx_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_85c50x_rmm_snap(rmm_snap_t *s, void *priv);
extern void ut85c50x_rmm_snap(rmm_snap_t *s, void *priv);
extern void vl82c59x_rmm_snap(rmm_snap_t *s, void *priv);
extern void wd76c10_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_adlib_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_cms_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_covox_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_lpt_dac_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_lpt_dss_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_sn76489_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_ps1_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_pssj_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_mmb_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_csm_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_opl_esfm_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_lpt_adlipt_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_lpt_cms_rmm_snap(rmm_snap_t *s, void *priv);
extern void snd_lpt_tndlpt_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_cga_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_cga_colorplus_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_cga_compaq_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_cga_compaq_plasma_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_cga_ncr_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_cga_olivetti_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_cga_quadcolor_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_cga_toshiba_t1000_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_cga_toshiba_t3100e_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_cga_v6355_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_f82c425_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_genius_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_hercules_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_hercules_incolor_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_hercules_plus_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_mda_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_sigma_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_wy700_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_3270pc_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ati18800_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ati28800_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ati_mach64_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_bochs_vbe_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_chips_69000_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_et3000_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_et4000_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_et4000w32_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ht216_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_mga_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_oak_oti_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_paradise_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_rtg310x_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_s3_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_s3_virge_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_sis6306_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_tandy_vis_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_tgui9440_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ti_cf62011_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_tvga_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_voodoo_banshee_rmm_snap(rmm_snap_t *s, void *priv);
extern void voodoo_rmm_snap(rmm_snap_t *s, void *priv);
struct voodoo_t;
extern void voodoo_card_rmm_snap(rmm_snap_t *s, struct voodoo_t *v);
struct svga_t;
extern void svga_rmm_snap_vram(rmm_snap_t *s, struct svga_t *svga);
extern void ioapic_rmm_snap(rmm_snap_t *s, void *priv);
extern void pci_dummy_rmm_snap(rmm_snap_t *s, void *priv);
extern void ps2_nvr_rmm_snap(rmm_snap_t *s, void *priv);
extern void m_ps1_rmm_snap(rmm_snap_t *s, void *priv);
extern void m_xt_jukost_rmm_snap(rmm_snap_t *s, void *priv);
extern void m_at_tandy_vis_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_clockgen_ch9201_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_clockgen_ch9204_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_clockgen_icd2047_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_clockgen_ics1494_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_clockgen_ics2494_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_clockgen_ics2595_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_clockgen_ics90c64a_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_ati68860_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_ati68875_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_att20c49x_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_att2xc498_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_bt481_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_bt48x_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_mu9c1880_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_sc1148x_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_sc1502x_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_sdac_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_stg1702_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ramdac_tkd8001_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ati_rage128_maxx_rmm_snap(rmm_snap_t *s, void *priv);
extern void vid_ibm_plasma_rmm_snap(rmm_snap_t *s, void *priv);
extern void acc2168_rmm_snap(rmm_snap_t *s, void *priv);
extern void ali1409_rmm_snap(rmm_snap_t *s, void *priv);
extern void ali1429_rmm_snap(rmm_snap_t *s, void *priv);
extern void ali1435_rmm_snap(rmm_snap_t *s, void *priv);
extern void ali1489_rmm_snap(rmm_snap_t *s, void *priv);
extern void ali1531_rmm_snap(rmm_snap_t *s, void *priv);
extern void ali1543_rmm_snap(rmm_snap_t *s, void *priv);
extern void ali1621_rmm_snap(rmm_snap_t *s, void *priv);
extern void ali6117_rmm_snap(rmm_snap_t *s, void *priv);
extern void asus48625_rmm_snap(rmm_snap_t *s, void *priv);
extern void compaq_rmm_snap(rmm_snap_t *s, void *priv);
extern void contaq_82c59x_rmm_snap(rmm_snap_t *s, void *priv);
extern void cs4031_rmm_snap(rmm_snap_t *s, void *priv);
extern void cs8230_rmm_snap(rmm_snap_t *s, void *priv);
extern void gc100_rmm_snap(rmm_snap_t *s, void *priv);
extern void grid1520_rmm_snap(rmm_snap_t *s, void *priv);
extern void ims8848_rmm_snap(rmm_snap_t *s, void *priv);
extern void intel_420ex_rmm_snap(rmm_snap_t *s, void *priv);
extern void intel_82335_rmm_snap(rmm_snap_t *s, void *priv);
extern void intel_i450kx_rmm_snap(rmm_snap_t *s, void *priv);
extern void intel_sio_rmm_snap(rmm_snap_t *s, void *priv);
extern void isa486c_rmm_snap(rmm_snap_t *s, void *priv);
extern void olivetti_eva_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti291_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti391_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti495_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti499_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti5x7_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti602_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti822_rmm_snap(rmm_snap_t *s, void *priv);
extern void opti895_rmm_snap(rmm_snap_t *s, void *priv);
extern void philips_rmm_snap(rmm_snap_t *s, void *priv);
extern void sanyo_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5511_h2p_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5571_h2p_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_85c411_rmm_snap(rmm_snap_t *s, void *priv);
extern void sl82c461_rmm_snap(rmm_snap_t *s, void *priv);
extern void umc_8886_rmm_snap(rmm_snap_t *s, void *priv);
extern void umc_8890_rmm_snap(rmm_snap_t *s, void *priv);
extern void umc_hb4_rmm_snap(rmm_snap_t *s, void *priv);
extern void via_apollo_rmm_snap(rmm_snap_t *s, void *priv);
extern void via_vt82c49x_rmm_snap(rmm_snap_t *s, void *priv);
extern void via_vt82c505_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_esdi_mca_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_ide_ali5213_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_ide_cmd640_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_ide_cmd646_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_ide_hpt366_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_ide_opti611_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_ide_pc87410_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_ide_rz1000_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_ide_um8673f_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_ide_w83769f_rmm_snap(rmm_snap_t *s, void *priv);
extern void hdc_st506_mca_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_3c501_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_3c59x_eisa_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_pe3_rmm_snap(rmm_snap_t *s, void *priv);
extern void net_plip_rmm_snap(rmm_snap_t *s, void *priv);
extern void scsi_aha1740_rmm_snap(rmm_snap_t *s, void *priv);
extern void scsi_pcscsi_rmm_snap(rmm_snap_t *s, void *priv);
extern void scsi_spock_rmm_snap(rmm_snap_t *s, void *priv);
extern void access_bus_rmm_snap(rmm_snap_t *s, void *priv);
extern void ast_readout_rmm_snap(rmm_snap_t *s, void *priv);
extern void dell_jumper_rmm_snap(rmm_snap_t *s, void *priv);
extern void hasp_rmm_snap(rmm_snap_t *s, void *priv);
extern void hwm_gl518sm_rmm_snap(rmm_snap_t *s, void *priv);
extern void hwm_lm75_rmm_snap(rmm_snap_t *s, void *priv);
extern void hwm_lm78_rmm_snap(rmm_snap_t *s, void *priv);
extern void hwm_vt82c686_rmm_snap(rmm_snap_t *s, void *priv);
extern void kbc_3270pc_rmm_snap(rmm_snap_t *s, void *priv);
extern void lightpen_pxl380_rmm_snap(rmm_snap_t *s, void *priv);
extern void mouse_bus_rmm_snap(rmm_snap_t *s, void *priv);
extern void mouse_ibm7690_tablet_rmm_snap(rmm_snap_t *s, void *priv);
extern void mouse_serial_rmm_snap(rmm_snap_t *s, void *priv);
extern void nec_mate_unk_rmm_snap(rmm_snap_t *s, void *priv);
extern void novell_cardkey_rmm_snap(rmm_snap_t *s, void *priv);
extern void phoenix_486_jumper_rmm_snap(rmm_snap_t *s, void *priv);
extern void radisys_config_rmm_snap(rmm_snap_t *s, void *priv);
extern void smbus_ali7101_rmm_snap(rmm_snap_t *s, void *priv);
extern void tulip_jumper_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_82091aa_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_90c50_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_acc3221_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_ali5113_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_ali5123_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_cbm_io_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_f82c606_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_f82c710_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_fdc37c669_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_fdc37c67x_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_fdc37c6xx_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_fdc37c93x_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_fdc37mx0x_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_gm82c803ab_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_gm82c803c_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_it86x1f_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_p82c604_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_pc87306_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_pc87307_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_pc87309_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_pc87310_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_pc873xx_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_um8669f_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_um866x_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_vl82c113_rmm_snap(rmm_snap_t *s, void *priv);
extern void sio_vt82c686_rmm_snap(rmm_snap_t *s, void *priv);
extern void ide_rmm_snap(rmm_snap_t *s, void *priv);
extern void gd54xx_rmm_snap(rmm_snap_t *s, void *priv);
extern void speaker_rmm_snap(rmm_snap_t *s, void *priv);
extern void char_loopback_rmm_snap(rmm_snap_t *s, void *priv);
extern void char_pipe_rmm_snap(rmm_snap_t *s, void *priv);
extern void sb16_rmm_snap(rmm_snap_t *s, void *priv);
extern void nuked_opl3_rmm_snap(rmm_snap_t *s, void *priv);
extern void saaym_rmm_snap(rmm_snap_t *s, void *priv);
extern void ymfm_rmm_snap(rmm_snap_t *s, void *priv);
extern void m19_vid_rmm_snap(rmm_snap_t *s, void *priv);
extern void ps55da2_rmm_snap(rmm_snap_t *s, void *priv);
extern void ql1xxx_rmm_snap(rmm_snap_t *s, void *priv);
extern void via_pipc_rmm_snap(rmm_snap_t *s, void *priv);
extern void stpc_rmm_snap(rmm_snap_t *s, void *priv);
extern void stpc_serial_rmm_snap(rmm_snap_t *s, void *priv);
extern void stpc_lpt_rmm_snap(rmm_snap_t *s, void *priv);
extern void esc_rmm_snap(rmm_snap_t *s, void *priv);
extern void pceb_rmm_snap(rmm_snap_t *s, void *priv);
extern void lxt_rmm_snap(rmm_snap_t *s, void *priv);
extern void scamp_rmm_snap(rmm_snap_t *s, void *priv);
extern void vl82c480_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5581_h2p_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5571_rmm_snap(rmm_snap_t *s, void *priv);
extern void mouse_upc_rmm_snap(rmm_snap_t *s, void *priv);
extern void mouse_microtouch_rmm_snap(rmm_snap_t *s, void *priv);
struct ega_t;
extern void ega_rmm_snap_vram(rmm_snap_t *s, struct ega_t *ega);
extern void ega_rmm_snap(rmm_snap_t *s, void *priv);
extern void jega_rmm_snap(rmm_snap_t *s, void *priv);
extern void xga_rmm_snap(rmm_snap_t *s, void *priv);
extern void svga_xga_rmm_snap(rmm_snap_t *s, void *priv);
struct ibm8514_t;
extern void ibm8514_rmm_snap_state(rmm_snap_t *s, struct ibm8514_t *dev);
extern void ibm8514_rmm_snap(rmm_snap_t *s, void *priv);
extern void mach8_rmm_snap(rmm_snap_t *s, void *priv);
extern void hd44780_rmm_snap(rmm_snap_t *s, void *priv);
extern void icd2061_rmm_snap(rmm_snap_t *s, void *priv);
extern void tvp3026_ramdac_rmm_snap(rmm_snap_t *s, void *priv);
extern void ibm_rgb528_ramdac_rmm_snap(rmm_snap_t *s, void *priv);
struct emu8k_t;
extern void emu8k_rmm_snap_ram(rmm_snap_t *s, struct emu8k_t *emu8k);
extern void goldfinch_rmm_snap(rmm_snap_t *s, void *priv);
extern void gus_rmm_snap(rmm_snap_t *s, void *priv);
extern void es137x_rmm_snap(rmm_snap_t *s, void *priv);
extern void cmi8x38_rmm_snap(rmm_snap_t *s, void *priv);
extern void ac97_via_rmm_snap(rmm_snap_t *s, void *priv);
extern void solo1_rmm_snap(rmm_snap_t *s, void *priv);
extern void ac97_codec_rmm_snap(rmm_snap_t *s, void *priv);
extern void optimc_rmm_snap(rmm_snap_t *s, void *priv);
extern void pas16_rmm_snap(rmm_snap_t *s, void *priv);
extern void ps1midi_rmm_snap(rmm_snap_t *s, void *priv);
extern void nuked_opl2_rmm_snap(rmm_snap_t *s, void *priv);
extern void nuked_cqm_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5591_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5513_ide_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5591_h2p_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5513_p2i_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5572_usb_rmm_snap(rmm_snap_t *s, void *priv);
extern void sis_5595_pmu_rmm_snap(rmm_snap_t *s, void *priv);
extern void pci_bridge_rmm_snap(rmm_snap_t *s, void *priv);
extern void w83877_rmm_snap(rmm_snap_t *s, void *priv);
extern void ne2k_rmm_snap(rmm_snap_t *s, void *priv);
extern void dp8390_rmm_snap(rmm_snap_t *s, void *priv);
extern void rage128_rmm_snap(rmm_snap_t *s, void *priv);
extern void vga_rmm_snap(rmm_snap_t *s, void *priv);
extern void rabbit_rmm_snap(rmm_snap_t *s, void *priv);
extern void ibm_5161_rmm_snap(rmm_snap_t *s, void *priv);
extern void kbd_xt_rmm_snap(rmm_snap_t *s, void *priv);
extern void kbc_xt_rmm_snap(rmm_snap_t *s, void *priv);
extern void ps2_m25_kbc_rmm_snap(rmm_snap_t *s, void *priv);
extern void ide_board_rmm_snap(rmm_snap_t *s, void *priv);
extern void st506_xt_rmm_snap(rmm_snap_t *s, void *priv);
extern void st506_at_rmm_snap(rmm_snap_t *s, void *priv);
extern void esdi_at_rmm_snap(rmm_snap_t *s, void *priv);
extern void xta_rmm_snap(rmm_snap_t *s, void *priv);
extern void xta_dual_rmm_snap(rmm_snap_t *s, void *priv);
extern void xtide_rmm_snap(rmm_snap_t *s, void *priv);
extern void aha_rmm_snap(rmm_snap_t *s, void *priv);
extern void buslogic_rmm_snap(rmm_snap_t *s, void *priv);
extern void aic6360_rmm_snap(rmm_snap_t *s, void *priv);
extern void ncr53c400_rmm_snap(rmm_snap_t *s, void *priv);
extern void t128_rmm_snap(rmm_snap_t *s, void *priv);
extern void ncr53c8xx_rmm_snap(rmm_snap_t *s, void *priv);
extern void aic7xxx_rmm_snap(rmm_snap_t *s, void *priv);
extern void cm153_rmm_snap(rmm_snap_t *s, void *priv);
extern void hitachi_rmm_snap(rmm_snap_t *s, void *priv);
extern void cm250_rmm_snap(rmm_snap_t *s, void *priv);
extern void sony_rmm_snap(rmm_snap_t *s, void *priv);
extern void mitsumi_rmm_snap(rmm_snap_t *s, void *priv);
extern void mke_rmm_snap(rmm_snap_t *s, void *priv);
extern void compaticard_rmm_snap(rmm_snap_t *s, void *priv);
extern void b215_rmm_snap(rmm_snap_t *s, void *priv);
extern void monster_fdc_rmm_snap(rmm_snap_t *s, void *priv);
extern void i4x0_rmm_snap(rmm_snap_t *s, void *priv);
extern void piix_rmm_snap(rmm_snap_t *s, void *priv);
extern void apm_rmm_snap(rmm_snap_t *s, void *priv);
extern void smbus_piix4_rmm_snap(rmm_snap_t *s, void *priv);
extern void w83977_rmm_snap(rmm_snap_t *s, void *priv);
extern void nmc93cxx_rmm_snap(rmm_snap_t *s, void *priv);
extern void mcga_rmm_snap(rmm_snap_t *s, void *priv);
/* From cdrom_rmm_snap, for the drive's scsi_cdrom_t. */
extern void scsi_cdrom_rmm_snap(rmm_snap_t *s, void *priv);
extern void scsi_disk_rmm_snap(rmm_snap_t *s);
extern void mo_rmm_snap(rmm_snap_t *s);
extern void epat_rmm_snap(rmm_snap_t *s, void *priv);
extern void bpck_rmm_snap(rmm_snap_t *s, void *priv);
extern void ditto_rmm_snap(rmm_snap_t *s, void *priv);
extern void tape_rmm_snap(rmm_snap_t *s);
extern void fdd_tape_rmm_snap(rmm_snap_t *s);
extern void rdisk_rmm_snap(rmm_snap_t *s);
extern const char *mo_rmm_snap_blocked(void);
extern const char *rdisk_rmm_snap_blocked(void);
extern void scsi_device_rmm_snap(rmm_snap_t *s);

/* rmm.c entry points; the machine must be paused. 0 on success, else an RMM error code with why. */
extern int         rmm_snap_save(const char *tag, char *why, size_t n);
extern int         rmm_snap_restore(const char *tag, char *why, size_t n);
extern int         rmm_snap_delete(const char *tag);
extern int         rmm_snap_slots(void);
extern const char *rmm_snap_tag(int i, uint64_t *tsc);
/* Snapshot files: export writes tag to path; load reads one into slot tag without
   changing the machine. */
extern int         rmm_snap_export(const char *tag, const char *path, char *why, size_t n);
extern int         rmm_snap_load(const char *path, const char *tag, char *tag_out, size_t tn, char *why, size_t n);

/* Async writer. state is memory, queued, writing, disk or error. */
extern const char *rmm_snap_state_name(int i, uint64_t *bytes);
/* Waits up to timeout_ms for every queued snapshot to reach the disk. Returns
   how many are still unwritten; *errors counts failed writes. */
extern int  rmm_snap_flush(int timeout_ms, int *errors);
/* Starts the writer; 0 or -1 with a reason. budget_mb 0 = 2 x RAM + 256 MB. */
extern void rmm_snap_writer_hold(int hold); /* test hook */
extern int  rmm_snap_writer_config(const char *dir, int budget_mb, int keep, char *why, size_t n);
extern void rmm_snap_shutdown(void);
/* Pointer scan: records 8-byte words of dumped regions that hold an address of this
   process. Returns the hit count, or -1 with a reason. */
extern int rmm_snap_ptrscan(char *why, size_t n);
/* Test hook: restore through the pointer table, as a fresh process would. */
extern void rmm_snap_reloc_force(int on);
extern int rmm_snap_ptrscan_region(int i, const char **label, uint64_t *base, uint64_t *len, int *local);
extern int rmm_snap_ptrscan_hit(int i, const char **label, unsigned *region, unsigned *rlen,
                                unsigned *off, int *code, uint64_t *val, uint64_t *base, int *target,
                                unsigned *toff);

#ifdef __cplusplus
}
#endif

#endif /*EMU_RMM_SNAP_H*/
