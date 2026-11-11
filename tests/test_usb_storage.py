from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UsbStorageTest(unittest.TestCase):
    def test_real_fat_files_and_removable_media_lifecycle(self):
        idf = Path(os.environ.get("IDF_PATH", Path.home() / ".platformio/packages/framework-espidf"))
        fatfs = idf / "components/fatfs"
        if not (fatfs / "src/ff.c").is_file():
            self.skipTest("ESP-IDF FatFS source unavailable")
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            for name in ("ff.c", "ff.h", "ffconf.h", "ffunicode.c", "diskio.h"):
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
#define CONFIG_FATFS_LFN_HEAP 1
#define CONFIG_FATFS_MAX_LFN 255
""")
            (stage / "sys").mkdir()
            (stage / "sys/lock.h").write_text("""
#pragma once
#include <pthread.h>
typedef pthread_mutex_t *_lock_t;
typedef _lock_t _lock_recursive_t;
void _lock_acquire_recursive(_lock_t *lock);
void _lock_release_recursive(_lock_t *lock);
""")
            binary = stage / "usb_storage"
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-D_GNU_SOURCE", "-pthread", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-include", str(ROOT / "tests/host/compat.h"),
                "-I", str(stage), "-I", str(ROOT / "tests/host/usb_storage_stubs"),
                "-I", str(ROOT / "src/services"), "-I", str(ROOT / "src/drivers"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "include"),
                "-I", str(ROOT / "tests/host"), str(stage / "ff.c"), str(stage / "ffunicode.c"),
                str(ROOT / "tests/host/usb_storage_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)
