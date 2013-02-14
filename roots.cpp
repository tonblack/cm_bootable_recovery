/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>

extern "C" {
#include "mtdutils/mtdutils.h"
#include "mtdutils/mounts.h"
}
#include "roots.h"
#include "common.h"
#include "make_ext4fs.h"
#include "partitions.hpp"

#include "libubi.h"
#define DEFAULT_CTRL_DEV "/dev/ubi_ctrl"

static int num_volumes = 0;
static Volume* device_volumes = NULL;
static int format_ubifs_volume(const char* location);

extern struct selabel_handle *sehandle;

static int parse_options(char* options, Volume* volume) {
    char* option;
    while ((option = strtok(options, ","))) {
        options = NULL;

        if (strncmp(option, "flags=", 6) == 0)   continue;
		if (strncmp(option, "length=", 7) == 0) {
            volume->length = strtoll(option+7, NULL, 10);
        } else {
            LOGE("bad option \"%s\"\n", option);
            return -1;
        }
    }
    return 0;
}

void load_volume_table() {
    int alloc = 2;
    device_volumes = (Volume*)malloc(alloc * sizeof(Volume));

    // Insert an entry for /tmp, which is the ramdisk and is always mounted.
    device_volumes[0].mount_point = "/tmp";
    device_volumes[0].fs_type = "ramdisk";
    device_volumes[0].device = NULL;
    device_volumes[0].device2 = NULL;
    device_volumes[0].length = 0;
    num_volumes = 1;

    FILE* fstab = fopen("/etc/recovery.fstab", "r");
    if (fstab == NULL) {
        LOGE("failed to open /etc/recovery.fstab (%s)\n", strerror(errno));
        return;
    }

    char buffer[1024];
    int i;
    while (fgets(buffer, sizeof(buffer)-1, fstab)) {
        for (i = 0; buffer[i] && isspace(buffer[i]); ++i);
        if (buffer[i] == '\0' || buffer[i] == '#') continue;

        char* original = strdup(buffer);

        char* mount_point = strtok(buffer+i, " \t\n");
        char* fs_type = strtok(NULL, " \t\n");
        char* device = strtok(NULL, " \t\n");
        // lines may optionally have a second device, to use if
        // mounting the first one fails.
        char* options = NULL;
        char* device2 = strtok(NULL, " \t\n");
        if (device2) {
            if (device2[0] == '/') {
                options = strtok(NULL, " \t\n");
            } else {
                options = device2;
                device2 = NULL;
            }
        }

        if (mount_point && fs_type && device) {
            while (num_volumes >= alloc) {
                alloc *= 2;
                device_volumes = (Volume*)realloc(device_volumes, alloc*sizeof(Volume));
            }
            device_volumes[num_volumes].mount_point = strdup(mount_point);
            device_volumes[num_volumes].fs_type = strdup(fs_type);
            device_volumes[num_volumes].device = strdup(device);
            device_volumes[num_volumes].device2 =
                device2 ? strdup(device2) : NULL;

            device_volumes[num_volumes].length = 0;
            if (parse_options(options, device_volumes + num_volumes) != 0) {
                LOGE("skipping malformed recovery.fstab line: %s\n", original);
            } else {
                ++num_volumes;
            }
        } else {
            LOGE("skipping malformed recovery.fstab line: %s\n", original);
        }
        free(original);
    }

    fclose(fstab);

    printf("recovery filesystem table\n");
    printf("=========================\n");
    for (i = 0; i < num_volumes; ++i) {
        Volume* v = &device_volumes[i];
        printf("  %d %s %s %s %s %lld\n", i, v->mount_point, v->fs_type,
               v->device, v->device2, v->length);
    }
    printf("\n");
}

Volume* volume_for_path(const char* path) {
    int i;
    for (i = 0; i < num_volumes; ++i) {
        Volume* v = device_volumes+i;
        int len = strlen(v->mount_point);
        if (strncmp(path, v->mount_point, len) == 0 &&
            (path[len] == '\0' || path[len] == '/')) {
            return v;
        }
    }
    return NULL;
}

int ensure_path_mounted(const char* path) {
	if (PartitionManager.Mount_By_Path(path, true))
		return 0;
	else
		return -1;
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 0;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 0;
    }

    mkdir(v->mount_point, 0755);  // in case it doesn't already exist

    if (strcmp(v->fs_type, "yaffs2") == 0) {
        // mount an MTD partition as a YAFFS2 filesystem.
        mtd_scan_partitions();
        const MtdPartition* partition;
        partition = mtd_find_partition_by_name(v->device);
        if (partition == NULL) {
            LOGE("failed to find \"%s\" partition to mount at \"%s\"\n",
                 v->device, v->mount_point);
            return -1;
        }
        return mtd_mount_partition(partition, v->mount_point, v->fs_type, 0);
    } else if (strcmp(v->fs_type, "ubifs") == 0) {
        LOGI("ensure_path_mounted ubifs:  %s %s %s %s\n", v->mount_point, v->fs_type,
               v->device, v->device2);
        libubi_t libubi;
        struct ubi_info ubi_info;
        struct ubi_dev_info dev_info;
        struct ubi_attach_request req;
        int err;
        char value[32] = {0};

        mtd_scan_partitions();
        int mtdn = mtd_get_index_by_name(v->device);
        if (mtdn < 0) {
            LOGE("bad mtd index for %s\n", v->device);
            return -1;
        }

        libubi = libubi_open();
        if (!libubi) {
            LOGE("libubi_open fail\n");
            return -1;
        }

        /*
         * Make sure the kernel is fresh enough and this feature is supported.
         */
        err = ubi_get_info(libubi, &ubi_info);
        if (err) {
            LOGE("cannot get UBI information\n");
            goto out_ubi_close;
        }

        if (ubi_info.ctrl_major == -1) {
            LOGE("MTD attach/detach feature is not supported by your kernel\n");
            goto out_ubi_close;
        }

        req.dev_num = UBI_DEV_NUM_AUTO;
        req.mtd_num = mtdn;
        req.vid_hdr_offset = 0;
        req.mtd_dev_node = NULL;

        // make sure partition is detached before attaching
        ubi_detach_mtd(libubi, DEFAULT_CTRL_DEV, mtdn);

        err = ubi_attach(libubi, DEFAULT_CTRL_DEV, &req);
        if (err) {
            LOGE("cannot attach mtd%d", mtdn);
            goto out_ubi_close;
        }

        /* Print some information about the new UBI device */
        err = ubi_get_dev_info1(libubi, req.dev_num, &dev_info);
        if (err) {
            LOGE("cannot get information about newly created UBI device\n");
            goto out_ubi_detach;
        }

        sprintf(value, "/dev/ubi%d_0", dev_info.dev_num);

        /* Print information about the created device */
        //err = ubi_get_vol_info1(libubi, dev_info.dev_num, 0, &vol_info);
        //if (err) {
        //  LOGE("cannot get information about UBI volume 0");
        //  goto out_ubi_detach;
        //}

        if (mount(value, v->mount_point, v->fs_type,  MS_NOATIME | MS_NODEV | MS_NODIRATIME, NULL )) {
            LOGE("cannot mount ubifs %s to %s\n", value, v->mount_point);
            goto out_ubi_detach;
        }
        LOGI("mount ubifs successful  %s to %s\n", value, v->mount_point);

        libubi_close(libubi);
        return 0;

out_ubi_detach:
        ubi_detach_mtd(libubi, DEFAULT_CTRL_DEV, mtdn);

out_ubi_close:
        libubi_close(libubi);
        return -1;
    } else if (strcmp(v->fs_type, "ext4") == 0 ||
               strcmp(v->fs_type, "vfat") == 0) {
        result = mount(v->device, v->mount_point, v->fs_type,
                       MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
        if (result == 0) return 0;

        if (v->device2) {
            LOGW("failed to mount %s (%s); trying %s\n",
                 v->device, strerror(errno), v->device2);
            result = mount(v->device2, v->mount_point, v->fs_type,
                           MS_NOATIME | MS_NODEV | MS_NODIRATIME, "");
            if (result == 0) return 0;
        }

        LOGE("failed to mount %s (%s)\n", v->mount_point, strerror(errno));
        return -1;
    }

    LOGE("unknown fs_type \"%s\" for %s\n", v->fs_type, v->mount_point);
    return -1;
}

int ensure_path_unmounted(const char* path) {
	if (PartitionManager.UnMount_By_Path(path, true))
		return 0;
	else
		return -1;
    int ret;
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        LOGE("unknown volume for path [%s]\n", path);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted; you can't unmount it.
        return -1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return -1;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        // volume is already unmounted
        return 0;
    }

    if (strcmp(v->fs_type, "ubifs") != 0) {
        return unmount_mounted_volume(mv);
    } else {
        libubi_t libubi;
        struct ubi_info ubi_info;

        unmount_mounted_volume(mv);

        mtd_scan_partitions();
        int mtdn = mtd_get_index_by_name(v->device);
        if (mtdn < 0) {
            LOGE("bad mtd index for %s\n", v->device);
            return -1;
        }

        libubi = libubi_open();
        if (!libubi) {
            LOGE("libubi_open fail\n");
            return -1;
        }

        /*
         * Make sure the kernel is fresh enough and this feature is supported.
         */
        ret = ubi_get_info(libubi, &ubi_info);
        if (ret) {
            LOGE("cannot get UBI information\n");
            goto out_ubi_close;
        }

        if (ubi_info.ctrl_major == -1) {
            LOGE("MTD detach/detach feature is not supported by your kernel\n");
            goto out_ubi_close;
        }

        ret = ubi_detach_mtd(libubi, DEFAULT_CTRL_DEV, mtdn);
        if (ret) {
            LOGE("cannot detach mtd%d\n", mtdn);
            goto out_ubi_close;
        }
        LOGI("detach ubifs successful mtd%d\n", mtdn);

        libubi_close(libubi);
        return 0;

    out_ubi_close:
        libubi_close(libubi);
        return -1;
    }
}

int format_volume(const char* volume) {
	if (PartitionManager.Wipe_By_Path(volume))
		return 0;
	else
		return -1;
    Volume* v = volume_for_path(volume);
    if (v == NULL) {
        LOGE("unknown volume \"%s\"\n", volume);
        return -1;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", volume);
        return -1;
    }
    if (strcmp(v->mount_point, volume) != 0) {
        LOGE("can't give path \"%s\" to format_volume\n", volume);
        return -1;
    }

    if (ensure_path_unmounted(volume) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(v->fs_type, "yaffs2") == 0 || strcmp(v->fs_type, "mtd") == 0 ||
        strcmp(v->fs_type, "ubifs") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(v->device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", v->device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", v->device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", v->device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n", v->device);
            return -1;
        }
        if (strcmp(v->fs_type, "ubifs") == 0) {
            return format_ubifs_volume(v->device);
        }
        return 0;
    }

    if (strcmp(v->fs_type, "ext4") == 0) {
#ifdef USE_EXT4
/*
        int result = make_ext4fs(v->device, v->length, volume, sehandle);
*/
        int result = 0;
#else
        int result = 0;
#endif
        if (result != 0) {
            LOGE("format_volume: make_extf4fs failed on %s\n", v->device);
            return -1;
        }
        return 0;
    }

    LOGE("format_volume: fs_type \"%s\" unsupported\n", v->fs_type);
    return -1;
}

static int format_ubifs_volume(const char* location) {
    int err;
    struct ubi_info ubi_info;
    struct ubi_dev_info dev_info;
    struct ubi_attach_request req;
    struct ubi_mkvol_request req2;
    char ubinode[16] ={0};

    mtd_scan_partitions();
    int mtdn = mtd_get_index_by_name(location);
    if (mtdn < 0) {
        LOGE("bad mtd index for %s\n", location);
        return -1;
    }

    libubi_t libubi;
    libubi = libubi_open();
    if (!libubi) {
        LOGE("libubi_open fail\n");
        return -1;
    }

    /*
     * Make sure the kernel is fresh enough and this feature is supported.
     */
    err = ubi_get_info(libubi, &ubi_info);
    if (err) {
        LOGE("cannot get UBI information\n");
        goto out_ubi_close;
    }

    if (ubi_info.ctrl_major == -1) {
        LOGE("MTD attach/detach feature is not supported by your kernel\n");
        goto out_ubi_close;
    }

    req.dev_num = UBI_DEV_NUM_AUTO;
    req.mtd_num = mtdn;
    req.vid_hdr_offset = 0;
    req.mtd_dev_node = NULL;

    err = ubi_attach(libubi, DEFAULT_CTRL_DEV, &req);
    if (err) {
        LOGE("cannot attach mtd%d", mtdn);
        goto out_ubi_close;
    }

    /* Print some information about the new UBI device */
    err = ubi_get_dev_info1(libubi, req.dev_num, &dev_info);
    if (err) {
        LOGE("cannot get information about newly created UBI device\n");
        goto out_ubi_detach;
    }

    req2.vol_id = UBI_VOL_NUM_AUTO;
    req2.alignment = 1;
    req2.bytes = dev_info.avail_lebs*dev_info.leb_size;
    req2.name = location;
    req2.vol_type = UBI_DYNAMIC_VOLUME;

    sprintf(ubinode, "/dev/ubi%d", dev_info.dev_num);

    err = ubi_mkvol(libubi, ubinode, &req2);
    if (err < 0) {
        LOGE("cannot UBI create volume %s at %s %d %llu\n", req2.name, ubinode ,err, req2.bytes);
        goto out_ubi_detach;
    }

    ubi_detach_mtd(libubi, DEFAULT_CTRL_DEV, mtdn);
    libubi_close(libubi);
    return 0;

out_ubi_detach:
    ubi_detach_mtd(libubi, DEFAULT_CTRL_DEV, mtdn);

out_ubi_close:
    libubi_close(libubi);
    return -1;
}
