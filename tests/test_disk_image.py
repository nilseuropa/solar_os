from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DiskImageTest(unittest.TestCase):
    def test_disk_command_routes_usb_volume_before_image_arguments(self):
        source = (ROOT / "src/shell/solar_os_shell_hardware.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "disk_command.inc").write_text(production_functions(source, ("solar_os_shell_cmd_disk",)))
            binary = stage / "disk_command"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(stage), "-I", str(ROOT / "tests/host"),
                            str(ROOT / "tests/host/disk_command_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_sd_partitions_share_drive_numbers_with_flash_and_images(self):
        source = (ROOT / "src/drivers/sd_card.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "allocation.inc").write_text(production_functions(source, ("alloc_mount",)))
            (stage / "allocation.c").write_text("""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define SD_CARD_MAX_MOUNTS 4
#define ESP_OK 0
typedef uint8_t BYTE;
typedef struct { bool active; uint8_t logical_volume; } sd_card_mount_t;
static sd_card_mount_t mounts[4];
static uint8_t physical_pdrv;
static bool occupied[4] = {true, true, true, false};
static int ff_diskio_get_drive(BYTE *drive) {
    for (unsigned i = 0; i < 4; ++i) if (!occupied[i]) { *drive = i; return 0; }
    return -1;
}
#include "allocation.inc"
int main(void) {
    uint8_t vol = 255;
    assert(alloc_mount(&vol) == &mounts[0] && vol == 0);
    mounts[0] = (sd_card_mount_t){true, 0};
    assert(alloc_mount(&vol) == &mounts[1] && vol == 3);
    occupied[3] = true; assert(!alloc_mount(&vol));
    /* Unmounting the first SD partition permits reusing its logical number,
     * even while an extra SD partition and an image remain mounted. */
    mounts[0].active = false; mounts[1] = (sd_card_mount_t){true, 3};
    assert(alloc_mount(&vol) == &mounts[0] && vol == 0);
    for (unsigned i = 0; i < 4; ++i) mounts[i].active = true;
    assert(!alloc_mount(&vol));
    return 0;
}
""")
            binary = stage / "allocation"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(stage / "allocation.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_real_fat_mount_create_persistence_bounds_and_ownership(self):
        idf = Path(os.environ.get("IDF_PATH", Path.home() / ".platformio/packages/framework-espidf"))
        fatfs = idf / "components/fatfs"
        if not (fatfs / "src/ff.c").is_file():
            self.skipTest("ESP-IDF FatFS source unavailable")
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            for name in ("ff.c", "ff.h", "ffconf.h", "diskio.h"):
                shutil.copy(fatfs / "src" / name, stage / name)
            shutil.copy(fatfs / "diskio/diskio_impl.h", stage)
            (stage / "sdkconfig.h").write_text("""
#define CONFIG_FATFS_CODEPAGE 437
#define CONFIG_FATFS_VOLUME_COUNT 4
#define CONFIG_WL_SECTOR_SIZE 4096
#define CONFIG_FATFS_PER_FILE_CACHE 1
#define CONFIG_FATFS_FS_LOCK 0
#define CONFIG_FATFS_TIMEOUT_MS 1000
#define CONFIG_FATFS_USE_FASTSEEK 0
#define CONFIG_FATFS_USE_LABEL 0
#define CONFIG_FATFS_DONT_TRUST_FREE_CLUSTER_CNT 0
#define CONFIG_FATFS_DONT_TRUST_LAST_ALLOC 0
#define CONFIG_FATFS_USE_DYN_BUFFERS 0
""")
            (stage / "sys").mkdir()
            (stage / "sys/lock.h").write_text("""
#pragma once
#include <pthread.h>
typedef pthread_mutex_t *_lock_t;
typedef _lock_t _lock_recursive_t;
void _lock_acquire(_lock_t *lock);
void _lock_release(_lock_t *lock);
void _lock_acquire_recursive(_lock_recursive_t *lock);
void _lock_release_recursive(_lock_recursive_t *lock);
""")
            (stage / "esp_vfs_fat.h").write_text("""
#pragma once
#include "esp_err.h"
#include "ff.h"
typedef struct { const char *base_path, *fat_drive; size_t max_files; } esp_vfs_fat_conf_t;
esp_err_t esp_vfs_fat_register_cfg(const esp_vfs_fat_conf_t *conf, FATFS **fs);
esp_err_t esp_vfs_fat_unregister_path(const char *path);
""")
            binary = stage / "disk_image"
            subprocess.run(["cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
                            "-Werror", "-Wno-unused-parameter", "-D_GNU_SOURCE",
                            "-ffunction-sections", "-fdata-sections", "-fstack-usage", "-pthread",
                            "-include", str(ROOT / "tests/host/compat.h"),
                            "-I", str(stage), "-I", str(ROOT / "src/services"),
                            "-I", str(ROOT / "src/board"), "-I", str(ROOT / "src/drivers"),
                            "-I", str(ROOT / "tests/host"),
                            str(stage / "ff.c"), str(ROOT / "src/services/solar_os_disk_image.c"),
                            str(ROOT / "src/services/solar_os_storage.c"),
                            str(ROOT / "tests/host/disk_image_test.c"),
                            "-Wl,--gc-sections,--wrap=fsync", "-o", str(binary)], check=True)
            # FatFS FIL embeds a 4 KiB cache in this configuration. Identity
            # checks are also called from FTP's smaller background stack.
            entries = "\n".join(p.read_text() for p in stage.glob("*.su"))
            match = re.search(r":file_identity\t(\d+)\t", entries)
            self.assertIsNotNone(match)
            self.assertLessEqual(int(match[1]), 2048)
            subprocess.run([str(binary), str(stage)], check=True, timeout=30)
