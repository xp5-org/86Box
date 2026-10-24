/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Handling of hard disk image files.
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>
 *          Fred N. van Kempen, <decwiz@yahoo.com>
 *
 *          Copyright 2016-2018 Miran Grca.
 *          Copyright 2017-2018 Fred N. van Kempen.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
#ifdef _WIN32
#include <io.h>
#define fsync(fd) _commit(fd)
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/path.h>
#include <86box/plat.h>
#include <86box/random.h>
#include <86box/hdd.h>
#include "minivhd/minivhd.h"
#include "minivhd/internal.h"

#define HDD_IMAGE_RAW 0
#define HDD_IMAGE_HDI 1
#define HDD_IMAGE_HDX 2
#define HDD_IMAGE_VHD 3

typedef struct hdd_image_t {
    FILE     *file; /* Used for HDD_IMAGE_RAW, HDD_IMAGE_HDI, and HDD_IMAGE_HDX. */
    MVHDMeta *vhd;  /* Used for HDD_IMAGE_VHD. */
    uint32_t  base;
    uint32_t  pos;
    uint32_t  last_sector;
    uint8_t   type; /* HDD_IMAGE_RAW, HDD_IMAGE_HDI, HDD_IMAGE_HDX, or HDD_IMAGE_VHD */
    uint8_t   loaded;
    uint8_t   is_block_device; /* 1 if this is a raw block device (e.g., /dev/disk4s1) */
    plat_device_vol_locked_t *locked_drives;
} hdd_image_t;

hdd_image_t hdd_images[HDD_NUM];

static char  empty_sector[512];

#ifdef USE_RMM
/* Monitor snapshot overlay, at the end of this file. */
int         rmm_disk_track; /* [General] rmm_disk_track: keep base copies from the first write on */
static int  rmm_disk_armed;
static void rmm_disk_before_write(uint8_t id, uint32_t sector, uint32_t count);
static void rmm_disk_drop(uint8_t id);
#endif
#ifndef __unix__
static char *empty_sector_1mb;
#endif

#ifdef ENABLE_HDD_IMAGE_LOG
int hdd_image_do_log = ENABLE_HDD_IMAGE_LOG;

static void
hdd_image_log(const char *fmt, ...)
{
    va_list ap;

    if (hdd_image_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define hdd_image_log(fmt, ...)
#endif

int
image_is_hdi(const char *s)
{
    if (!strcasecmp(path_get_extension((char *) s), "HDI"))
        return 1;
    else
        return 0;
}

int
image_is_hdx(const char *s, int check_signature)
{
    FILE    *fp;
    uint64_t filelen;
    uint64_t signature;

    if (!strcasecmp(path_get_extension((char *) s), "HDX")) {
        if (check_signature) {
            fp = plat_fopen(s, "rb");
            if (!fp)
                return 0;
            if (fseeko64(fp, 0, SEEK_END))
                fatal("image_is_hdx(): Error while seeking");
            filelen = ftello64(fp);
            if (fseeko64(fp, 0, SEEK_SET))
                fatal("image_is_hdx(): Error while seeking");
            if (filelen < 44) {
                if (fp != NULL)
                    fclose(fp);
                return 0;
            }
            if (fread(&signature, 1, 8, fp) != 8)
                fatal("image_is_hdx(): Error reading signature\n");
            fclose(fp);
            if (signature == 0xD778A82044445459LL)
                return 1;
            else
                return 0;
        } else
            return 1;
    } else
        return 0;
}

int
image_is_vhd(const char *s, int check_signature)
{
    FILE *fp;

    if (!strcasecmp(path_get_extension((char *) s), "VHD")) {
        if (check_signature) {
            fp = plat_fopen(s, "rb");
            if (!fp)
                return 0;

            bool is_vhd = mvhd_file_is_vhd(fp);
            fclose(fp);
            return is_vhd ? 1 : 0;
        } else
            return 1;
    } else
        return 0;
}

void
hdd_image_calc_chs(uint32_t *c, uint32_t *h, uint32_t *s, uint32_t size)
{
    /* Calculate the geometry from size (in MB), using the algorithm provided in
    "Virtual Hard Disk Image Format Specification, Appendix: CHS Calculation" */
    uint64_t ts = ((uint64_t) size) << 11LL;
    uint32_t spt;
    uint32_t heads;
    uint32_t cyl;
    uint32_t cth;

    if (ts > 65535 * 16 * 255)
        ts = 65535 * 16 * 255;

    if (ts >= 65535 * 16 * 63) {
        spt   = 255;
        heads = 16;
        cth   = (uint32_t) (ts / spt);
    } else {
        spt   = 17;
        cth   = (uint32_t) (ts / spt);
        heads = (cth + 1023) / 1024;
        if (heads < 4)
            heads = 4;
        if ((cth >= (heads * 1024)) || (heads > 16)) {
            spt   = 31;
            heads = 16;
            cth   = (uint32_t) (ts / spt);
        }
        if (cth >= (heads * 1024)) {
            spt   = 63;
            heads = 16;
            cth   = (uint32_t) (ts / spt);
        }
    }
    cyl = cth / heads;
    *c  = cyl;
    *h  = heads;
    *s  = spt;
}

static int
prepare_new_hard_disk(uint8_t id, uint64_t full_size)
{
    if (!hdd_images[id].file)
        return -1;

    /* Skip file creation/expansion for block devices */
    if (hdd_images[id].is_block_device) {
        hdd_images[id].last_sector = (uint32_t) (full_size >> 9) - 1;
        hdd_images[id].loaded = 1;
        return 1;
    }

#ifndef __unix__
    uint64_t target_size = (full_size + hdd_images[id].base) - ftello64(hdd_images[id].file);
    uint32_t size;
    uint32_t t;

    t    = (uint32_t) (target_size >> 20);     /* Amount of 1 MB blocks. */
    size = (uint32_t) (target_size & 0xfffff); /* 1 MB mask. */

    empty_sector_1mb = (char *) calloc(1, 1048576);

    /* Temporarily switch off suppression of seen messages so that the
       progress gets displayed. */
    pclog_toggle_suppr();
    pclog("Writing image sectors: [");

    /* First, write all the 1 MB blocks. */
    if (t > 0) {
        for (uint32_t i = 0; i < t; i++) {
            fseek(hdd_images[id].file, 0, SEEK_END);
            fwrite(empty_sector_1mb, 1, 1048576, hdd_images[id].file);
            pclog("#");
        }
    }

    /* Then, write the remainder. */
    if (size > 0) {
        fseek(hdd_images[id].file, 0, SEEK_END);
        fwrite(empty_sector_1mb, 1, size, hdd_images[id].file);
        pclog("#");
    }
    pclog("]\n");
    /* Switch the suppression of seen messages back on. */
    pclog_toggle_suppr();

    free(empty_sector_1mb);
#else
    pclog("Creating hard disk image: ");
    int ret = ftruncate(fileno(hdd_images[id].file), (off_t) (full_size + hdd_images[id].base));

    if (ret) {
        pclog("failed\n");
        fatal("Could not create hard disk image\n");
    }
    pclog("OK!\n");
#endif
    hdd_images[id].last_sector = (uint32_t) (full_size >> 9) - 1;

    hdd_images[id].loaded = 1;

    return 1;
}

void
hdd_image_init(void)
{
    for (uint8_t i = 0; i < HDD_NUM; i++)
        memset(&hdd_images[i], 0, sizeof(hdd_image_t));
}

int
hdd_image_load(int id)
{
    uint32_t sector_size = 512;
    uint32_t zero        = 0;
    uint64_t signature   = 0xD778A82044445459LL;
    uint64_t full_size   = 0;
    uint64_t spt         = 0;
    uint64_t hpc         = 0;
    uint64_t tracks      = 0;
    int      ret;
    uint64_t s         = 0;
    char    *fn        = hdd[id].fn;
    int      is_hdx[2] = { 0, 0 };
    int      is_vhd[2] = { 0, 0 };
    int      vhd_error = 0;

    if (id > 0) {
        for (int i = 0; i < id; i++) {
            if (strcmp(hdd[id].fn, hdd[i].fn) == 0) {
                fatal("Image already loaded: %s\n", hdd[i].fn);
                return 0;
            }
        }
    }

    memset(empty_sector, 0, sizeof(empty_sector));
    if (fn) {
        path_normalize(fn);
    }

    hdd_images[id].base = 0;
    hdd_images[id].is_block_device = 0;

    if (hdd_images[id].loaded) {
        if (hdd_images[id].file) {
            fclose(hdd_images[id].file);
            hdd_images[id].file = NULL;
        } else if (hdd_images[id].vhd) {
            mvhd_close(hdd_images[id].vhd);
            hdd_images[id].vhd = NULL;
        }
        hdd_images[id].loaded = 0;
    }

    if (hdd[id].raw_device || plat_is_block_device(fn)) {
        int64_t dev_size = plat_get_block_device_size(fn);
        if (dev_size <= 0) {
            hdd_image_log("Block device: Unable to get size\n");
            /* Don't clear hdd[id].fn - preserve config even if device is unavailable */
            goto fail_raw;
        }

        hdd_images[id].file = plat_fopen(fn, hdd[id].wp ? "rb" : "rb+");
        if (hdd_images[id].file == NULL) {
            hdd_image_log("Block device: Unable to open\n");
            /* Don't clear hdd[id].fn - preserve config even if device is unavailable */
            goto fail_raw;
        }

        hdd_images[id].locked_drives = plat_lock_volumes(hdd_images[id].file);

        hdd_images[id].is_block_device = 1;
        hdd_images[id].type = HDD_IMAGE_RAW;
        hdd_images[id].base = 0;
        hdd_images[id].pos = 0;

        /* Use provided geometry, or calculate from device size */
        if (hdd[id].spt && hdd[id].hpc && hdd[id].tracks) {
            full_size = ((uint64_t) hdd[id].spt) * ((uint64_t) hdd[id].hpc) * ((uint64_t) hdd[id].tracks) << 9LL;
        } else {
            full_size = (uint64_t) dev_size;
            /* Calculate geometry from size */
            hdd_image_calc_chs(&hdd[id].tracks, &hdd[id].hpc, &hdd[id].spt, (uint32_t)(full_size >> 20));
            full_size = ((uint64_t) hdd[id].spt) * ((uint64_t) hdd[id].hpc) * ((uint64_t) hdd[id].tracks) << 9LL;
        }

        hdd_images[id].last_sector = (uint32_t) (full_size >> 9) - 1;
        hdd_images[id].loaded = 1;

        hdd_image_log("Block device: %s, size=%llu, C=%u H=%u S=%u\n",
                      fn, (unsigned long long)full_size, hdd[id].tracks, hdd[id].hpc, hdd[id].spt);
        return 1;
    }

    is_hdx[0] = image_is_hdx(fn, 0);
    is_hdx[1] = image_is_hdx(fn, 1);

    is_vhd[0] = image_is_vhd(fn, 0);
    is_vhd[1] = image_is_vhd(fn, 1);

    hdd_images[id].pos = 0;

    /* Try to open existing hard disk image */
    if (fn[0] == '.') {
        hdd_image_log("File name starts with .\n");
        memset(hdd[id].fn, 0, sizeof(hdd[id].fn));
        goto fail_raw;
    }
    hdd_images[id].file = plat_fopen(fn, "rb+");
    if (hdd_images[id].file == NULL) {
        /* Failed to open existing hard disk image */
        if (errno == ENOENT) {
            /* Failed because it does not exist,
               so try to create new file */
            if (hdd[id].wp) {
                hdd_image_log("A write-protected image must exist\n");
                memset(hdd[id].fn, 0, sizeof(hdd[id].fn));
                goto fail_raw;
            }

            hdd_images[id].file = plat_fopen(fn, "wb+");
            if (hdd_images[id].file == NULL) {
                hdd_image_log("Unable to open image\n");
                memset(hdd[id].fn, 0, sizeof(hdd[id].fn));
                goto fail_raw;
            } else {
                if (image_is_hdi(fn)) {
                    full_size           = ((uint64_t) hdd[id].spt) * ((uint64_t) hdd[id].hpc) * ((uint64_t) hdd[id].tracks) << 9LL;
                    hdd_images[id].base = 0x1000;
                    fwrite(&zero, 1, 4, hdd_images[id].file);
                    fwrite(&zero, 1, 4, hdd_images[id].file);
                    fwrite(&(hdd_images[id].base), 1, 4, hdd_images[id].file);
                    fwrite(&full_size, 1, 4, hdd_images[id].file);
                    fwrite(&sector_size, 1, 4, hdd_images[id].file);
                    fwrite(&(hdd[id].spt), 1, 4, hdd_images[id].file);
                    fwrite(&(hdd[id].hpc), 1, 4, hdd_images[id].file);
                    fwrite(&(hdd[id].tracks), 1, 4, hdd_images[id].file);
                    for (uint16_t c = 0; c < 0x3f8; c++)
                        fwrite(&zero, 1, 4, hdd_images[id].file);
                    hdd_images[id].type = HDD_IMAGE_HDI;
                } else if (is_hdx[0]) {
                    full_size           = ((uint64_t) hdd[id].spt) * ((uint64_t) hdd[id].hpc) * ((uint64_t) hdd[id].tracks) << 9LL;
                    hdd_images[id].base = 0x28;
                    fwrite(&signature, 1, 8, hdd_images[id].file);
                    fwrite(&full_size, 1, 8, hdd_images[id].file);
                    fwrite(&sector_size, 1, 4, hdd_images[id].file);
                    fwrite(&(hdd[id].spt), 1, 4, hdd_images[id].file);
                    fwrite(&(hdd[id].hpc), 1, 4, hdd_images[id].file);
                    fwrite(&(hdd[id].tracks), 1, 4, hdd_images[id].file);
                    fwrite(&zero, 1, 4, hdd_images[id].file);
                    fwrite(&zero, 1, 4, hdd_images[id].file);
                    hdd_images[id].type = HDD_IMAGE_HDX;
                } else if (is_vhd[0]) {
                    fclose(hdd_images[id].file);
                    MVHDGeom geometry = { 0 };
                    geometry.cyl               = hdd[id].tracks;
                    geometry.heads             = hdd[id].hpc;
                    geometry.spt               = hdd[id].spt;
                    full_size                  = ((uint64_t) hdd[id].spt) * ((uint64_t) hdd[id].hpc) * ((uint64_t) hdd[id].tracks) << 9LL;
                    hdd_images[id].last_sector = (full_size >> 9LL) - 1;

                    if (hdd[id].vhd_blocksize || hdd[id].vhd_parent[0]) {
                        MVHDCreationOptions options = { 0 };
retry_vhd:
                        options.block_size_in_sectors = hdd[id].vhd_blocksize;
                        options.path                  = fn;
                        options.size_in_bytes         = 0;
                        options.geometry              = geometry;
                        if (hdd[id].vhd_parent[0]) {
                            options.type        = MVHD_TYPE_DIFF;
                            options.parent_path = hdd[id].vhd_parent;
                        } else {
                            options.type        = MVHD_TYPE_DYNAMIC;
                            options.parent_path = NULL;
                        }

                        hdd_images[id].vhd = mvhd_create_ex(options, &vhd_error);
                    } else {
                        hdd_images[id].vhd = mvhd_create_fixed(fn, geometry, &vhd_error, NULL);
                    }
                    if (hdd_images[id].vhd == NULL) {
                        /* Don't lock out if the parent of a differential VHD doesn't exist. */
                        if (hdd[id].vhd_parent[0]) {
                            hdd[id].vhd_parent[0] = '\0';
                            goto retry_vhd;
                        }
                        fatal("hdd_image_load(): VHD: Could not create VHD : %s\n", mvhd_strerr(vhd_error));
                    }
                    hdd_images[id].type = HDD_IMAGE_VHD;

                    return 1;
                } else {
                    hdd_images[id].type = HDD_IMAGE_RAW;
                }
                hdd_images[id].last_sector = 0;
            }

            s = full_size = ((uint64_t) hdd[id].spt) * ((uint64_t) hdd[id].hpc) * ((uint64_t) hdd[id].tracks) << 9LL;

            ret = prepare_new_hard_disk(id, full_size);
            if (ret <= 0)
                goto fail_raw;
            return ret;
        } else {
            /* Failed for another reason */
            hdd_image_log("Failed for another reason\n");
fail_raw:
            hdd_images[id].type        = HDD_IMAGE_RAW;
            hdd_images[id].last_sector = (uint32_t) (((uint64_t) hdd[id].spt) * ((uint64_t) hdd[id].hpc) * ((uint64_t) hdd[id].tracks)) - 1;
            return 1;
        }
    } else {
        if (image_is_hdi(fn)) {
            if (fseeko64(hdd_images[id].file, 0x8, SEEK_SET) == -1)
                fatal("hdd_image_load(): HDI: Error seeking to offset 0x8\n");
            if (fread(&(hdd_images[id].base), 1, 4, hdd_images[id].file) != 4)
                fatal("hdd_image_load(): HDI: Error reading base offset\n");
            if (fseeko64(hdd_images[id].file, 0xC, SEEK_SET) == -1)
                fatal("hdd_image_load(): HDI: Error seeking to offest 0xC\n");
            full_size = 0LL;
            if (fread(&full_size, 1, 4, hdd_images[id].file) != 4)
                fatal("hdd_image_load(): HDI: Error reading full size\n");
            if (fseeko64(hdd_images[id].file, 0x10, SEEK_SET) == -1)
                fatal("hdd_image_load(): HDI: Error seeking to offset 0x10\n");
            if (fread(&sector_size, 1, 4, hdd_images[id].file) != 4)
                fatal("hdd_image_load(): HDI: Error reading sector size\n");
            if (sector_size != 512) {
                /* Sector size is not 512 */
                hdd_image_log("HDI: Sector size is not 512\n");
                fclose(hdd_images[id].file);
                hdd_images[id].file = NULL;
                memset(hdd[id].fn, 0, sizeof(hdd[id].fn));
                goto fail_raw;
            }
            if (fread(&spt, 1, 4, hdd_images[id].file) != 4)
                fatal("hdd_image_load(): HDI: Error reading sectors per track\n");
            if (fread(&hpc, 1, 4, hdd_images[id].file) != 4)
                fatal("hdd_image_load(): HDI: Error reading heads per cylinder\n");
            if (fread(&tracks, 1, 4, hdd_images[id].file) != 4)
                fatal("hdd_image_load(): HDI: Error reading number of tracks\n");
            hdd[id].spt         = spt;
            hdd[id].hpc         = hpc;
            hdd[id].tracks      = tracks;
            hdd_images[id].type = HDD_IMAGE_HDI;
        } else if (is_hdx[1]) {
            hdd_images[id].base = 0x28;
            if (fseeko64(hdd_images[id].file, 8, SEEK_SET) == -1)
                fatal("hdd_image_load(): HDX: Error seeking to offset 0x8\n");
            if (fread(&full_size, 1, 8, hdd_images[id].file) != 8)
                fatal("hdd_image_load(): HDX: Error reading full size\n");
            if (fseeko64(hdd_images[id].file, 0x10, SEEK_SET) == -1)
                fatal("hdd_image_load(): HDX: Error seeking to offset 0x10\n");
            if (fread(&sector_size, 1, 4, hdd_images[id].file) != 4)
                fatal("hdd_image_load(): HDX: Error reading sector size\n");
            if (sector_size != 512) {
                /* Sector size is not 512 */
                hdd_image_log("HDX: Sector size is not 512\n");
                fclose(hdd_images[id].file);
                hdd_images[id].file = NULL;
                memset(hdd[id].fn, 0, sizeof(hdd[id].fn));
                goto fail_raw;
            }
            if (fread(&spt, 1, 4, hdd_images[id].file) != 4)
                fatal("hdd_image_load(): HDI: Error reading sectors per track\n");
            if (fread(&hpc, 1, 4, hdd_images[id].file) != 4)
                fatal("hdd_image_load(): HDI: Error reading heads per cylinder\n");
            if (fread(&tracks, 1, 4, hdd_images[id].file) != 4)
                fatal("hdd_image_load(): HDX: Error reading number of tracks\n");
            hdd[id].spt         = spt;
            hdd[id].hpc         = hpc;
            hdd[id].tracks      = tracks;
            hdd_images[id].type = HDD_IMAGE_HDX;
        } else if (is_vhd[1]) {
            fclose(hdd_images[id].file);
            hdd_images[id].file = NULL;
            hdd_images[id].vhd  = mvhd_open(fn, (bool) 0, &vhd_error);
            if (hdd_images[id].vhd == NULL) {
                if (vhd_error == MVHD_ERR_FILE)
                    fatal("hdd_image_load(): VHD: Error opening VHD file '%s': %s\n", fn, strerror(mvhd_errno));
                else
                    fatal("hdd_image_load(): VHD: Error opening VHD file '%s': %s\n", fn, mvhd_strerr(vhd_error));
            } else if (vhd_error == MVHD_ERR_TIMESTAMP) {
                pclog("hdd_image_load(): VHD: Parent/child timestamp mismatch for VHD file '%s'\n", fn);
            }

            hdd[id].tracks        = hdd_images[id].vhd->footer.geom.cyl;
            hdd[id].hpc           = hdd_images[id].vhd->footer.geom.heads;
            hdd[id].spt           = hdd_images[id].vhd->footer.geom.spt;
            hdd[id].vhd_blocksize = (hdd_images[id].vhd->footer.disk_type == MVHD_TYPE_FIXED) ? 0 : (hdd_images[id].vhd->sparse.block_sz / MVHD_SECTOR_SIZE);
            if (hdd_images[id].vhd->parent && hdd_images[id].vhd->parent->filename[0])
                strncpy(hdd[id].vhd_parent, hdd_images[id].vhd->parent->filename, sizeof(hdd[id].vhd_parent) - 1);
            full_size           = ((uint64_t) hdd[id].spt) * ((uint64_t) hdd[id].hpc) * ((uint64_t) hdd[id].tracks) << 9LL;
            hdd_images[id].type = HDD_IMAGE_VHD;
            /* If we're here, this means there is a valid VHD footer in the
               image, which means that by definition, all valid sectors
               are there. */
            hdd_images[id].last_sector = (uint32_t) (full_size >> 9) - 1;
            hdd_images[id].loaded      = 1;
            return 1;
        } else {
            full_size           = ((uint64_t) hdd[id].spt) * ((uint64_t) hdd[id].hpc) * ((uint64_t) hdd[id].tracks) << 9LL;
            hdd_images[id].type = HDD_IMAGE_RAW;
        }
    }

    if (fseeko64(hdd_images[id].file, 0, SEEK_END) == -1)
        fatal("hdd_image_load(): Error seeking to the end of file\n");
    s = ftello64(hdd_images[id].file);
    if (s < (full_size + hdd_images[id].base))
        ret = prepare_new_hard_disk(id, full_size);
    else {
        hdd_images[id].last_sector = (uint32_t) (full_size >> 9) - 1;
        hdd_images[id].loaded      = 1;
        ret                        = 1;
    }

    return ret;
}

int
hdd_image_seek(uint8_t id, uint32_t sector)
{
    off64_t addr = sector;
    addr         = (uint64_t) sector << 9LL;

    hdd_images[id].pos = sector;
    if (hdd_images[id].type != HDD_IMAGE_VHD) {
        if (!hdd_images[id].file || (fseeko64(hdd_images[id].file, addr + hdd_images[id].base, SEEK_SET) == -1)) {
            hdd_image_log("hdd_image_seek(): Error seeking\n");
            return -1;
        }
    }

    return 0;
}

int
hdd_image_read(uint8_t id, uint32_t sector, uint32_t count, uint8_t *buffer)
{
    int    non_transferred_sectors;
    size_t num_read;

    if (hdd_images[id].type == HDD_IMAGE_VHD) {
        hdd_images[id].vhd->error = 0;
        non_transferred_sectors   = mvhd_read_sectors(hdd_images[id].vhd, sector, count, buffer);
        hdd_images[id].pos        = sector + count - non_transferred_sectors - 1;
        if (hdd_images[id].vhd->error)
            return -1;
    } else {
        if (!hdd_images[id].file || (fseeko64(hdd_images[id].file, ((uint64_t) (sector) << 9LL) + hdd_images[id].base, SEEK_SET) == -1)) {
            hdd_image_log("Hard disk image %i: Read error during seek\n", id);
            return -1;
        }

        num_read           = fread(buffer, 512, count, hdd_images[id].file);
        hdd_images[id].pos = sector + num_read;
        if ((num_read < count) && !feof(hdd_images[id].file))
            return -1;
    }

    return 0;
}

uint32_t
hdd_image_get_last_sector(uint8_t id)
{
    return hdd_images[id].last_sector;
}

uint32_t
hdd_sectors(uint8_t id)
{
    return hdd_image_get_last_sector(id) - 1;
}

int
hdd_image_read_ex(uint8_t id, uint32_t sector, uint32_t count, uint8_t *buffer)
{
    uint32_t transfer_sectors = count;
    uint32_t sectors          = hdd_sectors(id);

    if ((sectors - sector) < transfer_sectors)
        transfer_sectors = sectors - sector;

    if (hdd_image_read(id, sector, transfer_sectors, buffer) < 0)
        return -1;

    if (count != transfer_sectors)
        return 1;
    return 0;
}

int
hdd_image_write(uint8_t id, uint32_t sector, uint32_t count, uint8_t *buffer)
{
    int    non_transferred_sectors;
    size_t num_write;

#ifdef USE_RMM
    if (rmm_disk_armed || rmm_disk_track)
        rmm_disk_before_write(id, sector, count);
#endif

    if (hdd_images[id].type == HDD_IMAGE_VHD) {
        hdd_images[id].vhd->error = 0;
        non_transferred_sectors   = mvhd_write_sectors(hdd_images[id].vhd, sector, count, buffer);
        hdd_images[id].pos        = sector + count - non_transferred_sectors - 1;
        if (hdd_images[id].vhd->error)
            return -1;
    } else {
        if (!hdd_images[id].file || (fseeko64(hdd_images[id].file, ((uint64_t) (sector) << 9LL) + hdd_images[id].base, SEEK_SET) == -1)) {
            hdd_image_log("Hard disk image %i: Write error during seek\n", id);
            return -1;
        }

        num_write          = fwrite(buffer, 512, count, hdd_images[id].file);
        hdd_images[id].pos = sector + num_write;
        fflush(hdd_images[id].file);
        if (num_write < count)
            return -1;
    }

    return 0;
}

int
hdd_image_write_ex(uint8_t id, uint32_t sector, uint32_t count, uint8_t *buffer)
{
    uint32_t transfer_sectors = count;
    uint32_t sectors          = hdd_sectors(id);

    if ((sectors - sector) < transfer_sectors)
        transfer_sectors = sectors - sector;

    if (hdd_image_write(id, sector, transfer_sectors, buffer) < 0)
        return -1;

    if (count != transfer_sectors)
        return 1;
    return 0;
}

int
hdd_image_zero(uint8_t id, uint32_t sector, uint32_t count)
{

#ifdef USE_RMM
    if (rmm_disk_armed || rmm_disk_track)
        rmm_disk_before_write(id, sector, count);
#endif
    if (hdd_images[id].type == HDD_IMAGE_VHD) {
        hdd_images[id].vhd->error   = 0;
        int non_transferred_sectors = mvhd_format_sectors(hdd_images[id].vhd, sector, count);
        hdd_images[id].pos          = sector + count - non_transferred_sectors - 1;
        if (hdd_images[id].vhd->error)
            return -1;
    } else {
        memset(empty_sector, 0, 512);

        if (!hdd_images[id].file || (fseeko64(hdd_images[id].file, ((uint64_t) (sector) << 9LL) + hdd_images[id].base, SEEK_SET) == -1)) {
            hdd_image_log("Hard disk image %i: Zero error during seek\n", id);
            return -1;
        }

        for (uint32_t i = 0; i < count; i++) {
            if (feof(hdd_images[id].file))
                break;

            hdd_images[id].pos = sector + i;
            if (!fwrite(empty_sector, 512, 1, hdd_images[id].file))
                return -1;
        }

        fflush(hdd_images[id].file);
    }

    return 0;
}

int
hdd_image_zero_ex(uint8_t id, uint32_t sector, uint32_t count)
{
    uint32_t transfer_sectors = count;
    uint32_t sectors          = hdd_sectors(id);

    if ((sectors - sector) < transfer_sectors)
        transfer_sectors = sectors - sector;

    hdd_image_zero(id, sector, transfer_sectors);

    if (count != transfer_sectors)
        return 1;
    return 0;
}

uint32_t
hdd_image_get_pos(uint8_t id)
{
    return hdd_images[id].pos;
}

uint8_t
hdd_image_get_type(uint8_t id)
{
    return hdd_images[id].type;
}

void
hdd_image_unload(uint8_t id, UNUSED(int fn_preserve))
{
    if (strlen(hdd[id].fn) == 0)
        return;

#ifdef USE_RMM
    rmm_disk_drop(id);
#endif

    if (hdd_images[id].loaded) {
        if (hdd_images[id].file != NULL) {
            fclose(hdd_images[id].file);
            hdd_images[id].file = NULL;
        } else if (hdd_images[id].vhd != NULL) {
            mvhd_close(hdd_images[id].vhd);
            hdd_images[id].vhd = NULL;
        }
        hdd_images[id].loaded = 0;
    }

    hdd_images[id].last_sector = -1;

    memset(hdd[id].fn, 0, sizeof(hdd[id].fn));
}

void
hdd_image_close(uint8_t id)
{
    hdd_image_log("hdd_image_close(%i)\n", id);

    if (!hdd_images[id].loaded)
        return;

#ifdef USE_RMM
    rmm_disk_drop(id);
#endif

    if (hdd_images[id].locked_drives) {
        plat_unlock_volumes(hdd_images[id].locked_drives);
        hdd_images[id].locked_drives = NULL;
    }

    if (hdd_images[id].file != NULL) {
        fclose(hdd_images[id].file);
        hdd_images[id].file = NULL;
    } else if (hdd_images[id].vhd != NULL) {
        mvhd_close(hdd_images[id].vhd);
        hdd_images[id].vhd = NULL;
    }

    memset(&hdd_images[id], 0, sizeof(hdd_image_t));
    hdd_images[id].loaded = 0;
}

void
hdd_image_sync(uint8_t id)
{
    if (!hdd_images[id].loaded)
        return;

    if (hdd_images[id].file != NULL) {
        fflush(hdd_images[id].file);
    }
}

void
hdd_image_sync_all(void)
{
    hdd_image_log("hdd_image_sync_all: syncing all disk images\n");
    for (uint8_t i = 0; i < HDD_NUM; i++) {
        if (hdd_images[i].loaded)
            hdd_image_sync(i);
    }
}

#ifdef USE_RMM
#    include <86box/rmm_snap.h>

/* Monitor snapshots of hard disk images: base and view copies per 4K block.
   See releasenotes.txt. */
#    define RMM_DISK_SECT 8

typedef struct rmm_blob_t {
    uint32_t refs;
    uint8_t  data[RMM_DISK_SECT * 512];
} rmm_blob_t;

typedef struct {
    uint32_t    blk;
    uint8_t     used;
    uint8_t     dirty;  /* written since the last save or restore */
    rmm_blob_t *base;   /* the bytes before the first write */
    rmm_blob_t *view;   /* the bytes at the last save or restore */
    rmm_blob_t *target; /* restore scratch */
} rmm_disk_ent_t;

typedef struct {
    rmm_disk_ent_t *e;     /* open addressing, cap a power of two */
    uint32_t        cap;
    uint32_t        n;
    uint32_t        epoch; /* bumped whenever the image is closed */
    uint8_t         hash_ok;
    uint64_t        hash;  /* rmm_disk_base_hash(), until the overlay is cleared */
} rmm_disk_t;

typedef struct {
    uint32_t    blk;
    rmm_blob_t *blob;
} rmm_disk_ref_t;

/* One per image in a snapshot; hdd_image_rmm_save() returns HDD_NUM. */
typedef struct {
    uint8_t         loaded;
    uint8_t         hash_ok;
    uint32_t        epoch;
    uint32_t        n;
    rmm_disk_ref_t *e;
    uint64_t        sectors;
    uint64_t        hash;  /* of the image when the overlay began: see hdd_image_rmm_hash() */
} rmm_disk_snap_t;

static rmm_disk_t rmm_disks[HDD_NUM];
static int        rmm_disk_busy;

static rmm_blob_t *
rmm_blob_ref(rmm_blob_t *b)
{
    b->refs++;
    return b;
}

static void
rmm_blob_put(rmm_blob_t *b)
{
    if (b && !--b->refs)
        free(b);
}

static rmm_disk_ent_t *
rmm_disk_find(rmm_disk_t *d, uint32_t blk, int add)
{
    uint32_t j;

    if (add && (((d->n + 1) * 2) > d->cap)) {
        uint32_t        cap = d->cap ? (d->cap * 2) : 1024;
        rmm_disk_ent_t *e   = calloc(cap, sizeof(rmm_disk_ent_t));

        if (!e)
            return NULL;
        for (uint32_t i = 0; i < d->cap; i++) {
            if (!d->e[i].used)
                continue;
            for (j = (d->e[i].blk * 2654435761u) & (cap - 1); e[j].used; j = (j + 1) & (cap - 1))
                ;
            e[j] = d->e[i];
        }
        free(d->e);
        d->e   = e;
        d->cap = cap;
    }
    if (!d->cap)
        return NULL;
    for (j = (blk * 2654435761u) & (d->cap - 1);; j = (j + 1) & (d->cap - 1)) {
        if (d->e[j].used && (d->e[j].blk == blk))
            return &d->e[j];
        if (!d->e[j].used) {
            if (!add)
                return NULL;
            d->e[j].used = 1;
            d->e[j].blk  = blk;
            d->n++;
            return &d->e[j];
        }
    }
}

static void
rmm_disk_clear(rmm_disk_t *d)
{
    for (uint32_t i = 0; i < d->cap; i++) {
        if (d->e[i].used) {
            rmm_blob_put(d->e[i].base);
            rmm_blob_put(d->e[i].view);
        }
    }
    free(d->e);
    d->e       = NULL;
    d->cap     = 0;
    d->n       = 0;
    d->hash_ok = 0;
}

/* Block blk of image id, past the overlay; the last block may be short. */
static void
rmm_disk_io(uint8_t id, uint32_t blk, rmm_blob_t *b, int write)
{
    uint64_t first = (uint64_t) blk * RMM_DISK_SECT;
    uint64_t count = RMM_DISK_SECT;
    uint32_t pos   = hdd_images[id].pos;
    int      ret;

    if ((first + count) > ((uint64_t) hdd_images[id].last_sector + 1))
        count = (uint64_t) hdd_images[id].last_sector + 1 - first;
    if (!write)
        memset(b->data, 0, sizeof(b->data));
    rmm_disk_busy = 1;
    if (write)
        ret = hdd_image_write(id, (uint32_t) first, (uint32_t) count, b->data);
    else
        ret = hdd_image_read(id, (uint32_t) first, (uint32_t) count, b->data);
    rmm_disk_busy = 0;
    hdd_images[id].pos = pos;
    if (ret < 0)
        fatal("RMM: hard disk %i: cannot %s block %u of the image for a snapshot\n", id, write ? "write" : "read", blk);
}

static rmm_blob_t *
rmm_disk_read_blob(uint8_t id, uint32_t blk)
{
    rmm_blob_t *b = malloc(sizeof(rmm_blob_t));

    if (!b)
        fatal("RMM: out of memory for the hard disk snapshot overlay\n");
    b->refs = 1;
    rmm_disk_io(id, blk, b, 0);
    return b;
}

static void
rmm_disk_before_write(uint8_t id, uint32_t sector, uint32_t count)
{
    rmm_disk_t *d = &rmm_disks[id];
    uint64_t    last;

    if (rmm_disk_busy || !count || !hdd_images[id].loaded)
        return;
    last = (uint64_t) sector + count - 1;
    if (last > hdd_images[id].last_sector)
        last = hdd_images[id].last_sector;
    for (uint64_t blk = sector / RMM_DISK_SECT; (blk * RMM_DISK_SECT) <= last; blk++) {
        rmm_disk_ent_t *e = rmm_disk_find(d, (uint32_t) blk, 1);

        if (!e)
            fatal("RMM: out of memory for the hard disk snapshot overlay\n");
        if (!e->base) {
            e->base = rmm_disk_read_blob(id, (uint32_t) blk);
            e->view = rmm_blob_ref(e->base);
        }
        e->dirty = 1;
    }
}

/* Image closing: invalidates the overlay and older snapshots. */
static void
rmm_disk_drop(uint8_t id)
{
    rmm_disk_clear(&rmm_disks[id]);
    rmm_disks[id].epoch++;
}

void *
hdd_image_rmm_save(void)
{
    rmm_disk_snap_t *snap = calloc(HDD_NUM, sizeof(rmm_disk_snap_t));

    if (!snap)
        return NULL;
    rmm_disk_armed = 1;
    for (int id = 0; id < HDD_NUM; id++) {
        rmm_disk_t *d = &rmm_disks[id];

        snap[id].loaded  = hdd_images[id].loaded;
        snap[id].epoch   = d->epoch;
        snap[id].sectors = hdd_images[id].loaded ? ((uint64_t) hdd_images[id].last_sector + 1) : 0;
        if (!d->n)
            continue;
        snap[id].e = malloc(d->n * sizeof(rmm_disk_ref_t));
        if (!snap[id].e) {
            hdd_image_rmm_free(snap);
            return NULL;
        }
        for (uint32_t i = 0; i < d->cap; i++) {
            rmm_disk_ent_t *e = &d->e[i];

            if (!e->used)
                continue;
            if (e->dirty) {
                rmm_blob_put(e->view);
                e->view  = rmm_disk_read_blob(id, e->blk);
                e->dirty = 0;
            }
            snap[id].e[snap[id].n].blk  = e->blk;
            snap[id].e[snap[id].n].blob = rmm_blob_ref(e->view);
            snap[id].n++;
        }
    }
    return snap;
}

/* NULL if the images are the ones the snapshot was taken with, else why. */
const char *
hdd_image_rmm_check(const void *p, char *why, size_t n)
{
    const rmm_disk_snap_t *snap = (const rmm_disk_snap_t *) p;

    for (int id = 0; id < HDD_NUM; id++) {
        if ((snap[id].loaded != hdd_images[id].loaded) ||
            (snap[id].loaded && (snap[id].epoch != rmm_disks[id].epoch))) {
            snprintf(why, n, "hard disk %i was closed or changed", id);
            return why;
        }
    }
    return NULL;
}

void
hdd_image_rmm_restore(const void *p)
{
    const rmm_disk_snap_t *snap = (const rmm_disk_snap_t *) p;

    for (int id = 0; id < HDD_NUM; id++) {
        rmm_disk_t *d = &rmm_disks[id];

        if (!snap[id].loaded || !d->cap)
            continue;
        for (uint32_t i = 0; i < d->cap; i++)
            d->e[i].target = d->e[i].base;
        for (uint32_t k = 0; k < snap[id].n; k++) {
            rmm_disk_ent_t *e = rmm_disk_find(d, snap[id].e[k].blk, 0);

            if (!e)
                fatal("RMM: hard disk %i: snapshot block %u is missing from the overlay\n", id, snap[id].e[k].blk);
            e->target = snap[id].e[k].blob;
        }
        for (uint32_t i = 0; i < d->cap; i++) {
            rmm_disk_ent_t *e = &d->e[i];

            if (!e->used)
                continue;
            if (e->dirty || ((e->view != e->target) && memcmp(e->view->data, e->target->data, sizeof(e->view->data))))
                rmm_disk_io(id, e->blk, e->target, 1);
            rmm_blob_put(e->view);
            e->view   = rmm_blob_ref(e->target);
            e->target = NULL;
            e->dirty  = 0;
        }
    }
}

void
hdd_image_rmm_free(void *p)
{
    rmm_disk_snap_t *snap = (rmm_disk_snap_t *) p;

    if (!snap)
        return;
    for (int id = 0; id < HDD_NUM; id++) {
        for (uint32_t k = 0; k < snap[id].n; k++)
            rmm_blob_put(snap[id].e[k].blob);
        free(snap[id].e);
    }
    free(snap);
}

/* The last snapshot is gone: stop keeping copies. */
void
hdd_image_rmm_disarm(void)
{
    if (rmm_disk_track)
        return; /* the copies go back to the first write since the image was opened */
    rmm_disk_armed = 0;
    for (int id = 0; id < HDD_NUM; id++)
        rmm_disk_clear(&rmm_disks[id]);
}

/* -- snapshot files: per-image blocks and base hash -- */

#    define RMM_DISK_FILE_BLK (RMM_DISK_SECT * 512)

typedef struct {
    uint32_t id;
    uint32_t n;
    uint64_t sectors;
    uint64_t hash;
    uint32_t hash_ok;
    uint32_t blksize;
} rmm_disk_filehdr_t;

/* The image as it was when the overlay began. Reads the whole image. */
static uint64_t
rmm_disk_base_hash(uint8_t id)
{
    rmm_disk_t *d     = &rmm_disks[id];
    uint64_t    nsect = (uint64_t) hdd_images[id].last_sector + 1;
    uint64_t    nblk  = (nsect + RMM_DISK_SECT - 1) / RMM_DISK_SECT;
    uint64_t    h     = rmm_snap_hash(0, &nsect, sizeof(nsect));
    rmm_blob_t *b;

    if (d->hash_ok)
        return d->hash;
    if (!(b = malloc(sizeof(rmm_blob_t))))
        fatal("RMM: out of memory hashing hard disk %i\n", id);
    for (uint64_t blk = 0; blk < nblk; blk++) {
        rmm_disk_ent_t *e = rmm_disk_find(d, (uint32_t) blk, 0);

        if (e && e->base)
            h = rmm_snap_hash(h, e->base->data, sizeof(e->base->data));
        else {
            rmm_disk_io(id, (uint32_t) blk, b, 0);
            h = rmm_snap_hash(h, b->data, sizeof(b->data));
        }
    }
    free(b);
    d->hash    = h;
    d->hash_ok = 1;
    return h;
}

/* Fills in missing base hashes. Needs the machine paused. 0, or -1 with why. */
int
hdd_image_rmm_hash(void *p, char *why, size_t n)
{
    rmm_disk_snap_t *snap = (rmm_disk_snap_t *) p;

    for (int id = 0; id < HDD_NUM; id++) {
        if (!snap[id].loaded || snap[id].hash_ok)
            continue;
        if (!hdd_images[id].loaded || (snap[id].epoch != rmm_disks[id].epoch)) {
            snprintf(why, n, "hard disk %i was closed or changed since the snapshot", id);
            return -1;
        }
        snap[id].hash    = rmm_disk_base_hash(id);
        snap[id].hash_ok = 1;
    }
    return 0;
}

/* Bytes hdd_image_rmm_write() writes. */
uint64_t
hdd_image_rmm_file_len(const void *p)
{
    const rmm_disk_snap_t *snap = (const rmm_disk_snap_t *) p;
    uint64_t               len  = sizeof(uint32_t);

    for (int id = 0; id < HDD_NUM; id++)
        if (snap[id].loaded)
            len += sizeof(rmm_disk_filehdr_t) + (uint64_t) snap[id].n * (sizeof(uint32_t) + RMM_DISK_FILE_BLK);
    return len;
}

/* The snapshot's images, for a file. Runs on the writer thread. 0, or -1. */
int
hdd_image_rmm_write(const void *p, FILE *f)
{
    const rmm_disk_snap_t *snap = (const rmm_disk_snap_t *) p;
    uint32_t               cnt  = 0;

    for (int id = 0; id < HDD_NUM; id++)
        cnt += snap[id].loaded;
    if (fwrite(&cnt, sizeof(cnt), 1, f) != 1)
        return -1;
    for (int id = 0; id < HDD_NUM; id++) {
        rmm_disk_filehdr_t h = { 0 };

        if (!snap[id].loaded)
            continue;
        h.id      = id;
        h.n       = snap[id].n;
        h.sectors = snap[id].sectors;
        h.hash    = snap[id].hash;
        h.hash_ok = snap[id].hash_ok;
        h.blksize = RMM_DISK_FILE_BLK;
        if (fwrite(&h, sizeof(h), 1, f) != 1)
            return -1;
        for (uint32_t k = 0; k < snap[id].n; k++) {
            if ((fwrite(&snap[id].e[k].blk, sizeof(uint32_t), 1, f) != 1) ||
                (fwrite(snap[id].e[k].blob->data, RMM_DISK_FILE_BLK, 1, f) != 1))
                return -1;
        }
    }
    return 0;
}

/* A snapshot's images read back from a file. Needs the machine paused.
   Returns what hdd_image_rmm_save() would, or NULL with why. */
void *
hdd_image_rmm_import(FILE *f, uint64_t len, char *why, size_t n)
{
    rmm_disk_filehdr_t hdr[HDD_NUM];
    int64_t            start[HDD_NUM];
    uint8_t            seen[HDD_NUM] = { 0 };
    uint32_t           cnt;
    rmm_disk_snap_t   *snap;

    if ((len < sizeof(cnt)) || (fread(&cnt, sizeof(cnt), 1, f) != 1) || (cnt > HDD_NUM)) {
        snprintf(why, n, "the hard disk section is damaged");
        return NULL;
    }
    len -= sizeof(cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        rmm_disk_filehdr_t h;
        uint64_t           body;

        if ((len < sizeof(h)) || (fread(&h, sizeof(h), 1, f) != 1) || (h.id >= HDD_NUM) || seen[h.id] ||
            (h.blksize != RMM_DISK_FILE_BLK)) {
            snprintf(why, n, "the hard disk section is damaged");
            return NULL;
        }
        len -= sizeof(h);
        body = (uint64_t) h.n * (sizeof(uint32_t) + RMM_DISK_FILE_BLK);
        if (len < body) {
            snprintf(why, n, "the hard disk section is truncated");
            return NULL;
        }
        len -= body;
        seen[h.id]  = 1;
        hdr[h.id]   = h;
        start[h.id] = ftello64(f);
        if (fseeko64(f, (int64_t) body, SEEK_CUR)) {
            snprintf(why, n, "the hard disk section is truncated");
            return NULL;
        }
    }
    /* Everything is checked before the overlay is touched. */
    for (int id = 0; id < HDD_NUM; id++) {
        if (seen[id] != hdd_images[id].loaded) {
            snprintf(why, n, "hard disk %i is %s here but %s in the snapshot", id,
                     hdd_images[id].loaded ? "attached" : "not attached", seen[id] ? "attached" : "not attached");
            return NULL;
        }
        if (!seen[id])
            continue;
        if (hdr[id].sectors != (uint64_t) hdd_images[id].last_sector + 1) {
            snprintf(why, n, "hard disk %i has %" PRIu64 " sectors here, %" PRIu64 " in the snapshot", id,
                     (uint64_t) hdd_images[id].last_sector + 1, hdr[id].sectors);
            return NULL;
        }
        if (!hdr[id].hash_ok) {
            snprintf(why, n, "the snapshot does not say which image hard disk %i was based on", id);
            return NULL;
        }
        if (rmm_disk_base_hash(id) != hdr[id].hash) {
            snprintf(why, n, "hard disk %i: the image is not the one the snapshot was based on "
                     "(it must be the image as it was before the guest first wrote to it)", id);
            return NULL;
        }
    }
    if (!(snap = calloc(HDD_NUM, sizeof(rmm_disk_snap_t))))
        goto oom;
    for (int id = 0; id < HDD_NUM; id++) {
        rmm_disk_t *d = &rmm_disks[id];

        snap[id].loaded  = hdd_images[id].loaded;
        snap[id].epoch   = d->epoch;
        if (!seen[id])
            continue;
        snap[id].sectors = hdr[id].sectors;
        snap[id].hash    = hdr[id].hash;
        snap[id].hash_ok = 1;
        if (!hdr[id].n)
            continue;
        if (!(snap[id].e = calloc(hdr[id].n, sizeof(rmm_disk_ref_t))))
            goto oom;
        fseeko64(f, start[id], SEEK_SET);
        for (uint32_t k = 0; k < hdr[id].n; k++) {
            rmm_disk_ent_t *e;
            rmm_blob_t     *b = malloc(sizeof(rmm_blob_t));
            uint32_t        blk;

            if (!b)
                goto oom;
            if ((fread(&blk, sizeof(blk), 1, f) != 1) || (fread(b->data, RMM_DISK_FILE_BLK, 1, f) != 1) ||
                ((uint64_t) blk * RMM_DISK_SECT >= hdr[id].sectors)) {
                free(b);
                snprintf(why, n, "the hard disk section is damaged");
                hdd_image_rmm_free(snap);
                return NULL;
            }
            b->refs = 1;
            /* The image's block as it is now is its base: it matched the hash. */
            if (!(e = rmm_disk_find(d, blk, 1))) {
                free(b);
                goto oom;
            }
            if (!e->base) {
                e->base = rmm_disk_read_blob(id, blk);
                e->view = rmm_blob_ref(e->base);
            }
            snap[id].e[snap[id].n].blk  = blk;
            snap[id].e[snap[id].n].blob = b;
            snap[id].n++;
        }
    }
    rmm_disk_armed = 1;
    return snap;

oom:
    hdd_image_rmm_free(snap);
    snprintf(why, n, "out of memory loading the hard disk blocks");
    return NULL;
}

/* The drive model's seek position, cache and the image position. */
void
hdd_image_rmm_snap(rmm_snap_t *s)
{
    for (int id = 0; id < HDD_NUM; id++) {
        if (!hdd_images[id].loaded)
            continue;
        RMM_SNAP_VAR(s, hdd[id]);
        RMM_SNAP_VAR(s, hdd_images[id].pos);
    }
}
#endif
