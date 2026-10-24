#include "unlock_file.h"
#include "unlock_file_diskio.h"
#include "ff.h"
#include "diskio.h"
#include <string.h>

#define UNLOCK_FILE_MAX_SIZE 256

static FATFS filesystem;
static char unlock_path[14]; /* Root path plus an 8.3 filename. */

/* A normal memset can be optimized away; volatile keeps password wipes intact. */
static void clear_bytes(void *buffer, size_t size)
{
    volatile uint8_t *byte = buffer;

    while (size-- != 0)
        *byte++ = 0;
}

static void unmount_filesystem(void)
{
    f_mount(NULL, "", 0);
    clear_bytes(&filesystem, sizeof(filesystem));
}

static bool mount_filesystem(void)
{
    if (f_mount(&filesystem, "", 1) != FR_OK)
        return false;

    return filesystem.fs_type == FS_FAT32;
}

bool unlock_file_init(void)
{
    uint8_t work[FF_MAX_SS];
    FRESULT result = f_mount(&filesystem, "", 1);
    bool ready = false;

    if (result == FR_NO_FILESYSTEM ||
        (result == FR_OK && filesystem.fs_type != FS_FAT32)) {
        if (disk_status(0) & STA_NOINIT)
            goto done;

        const MKFS_PARM options = { .fmt = FM_FAT32 | FM_SFD, .n_fat = 2 };
        unlock_file_diskio_set_formatting(true);
        result = f_mkfs("", &options, work, sizeof(work));
        unlock_file_diskio_set_formatting(false);
        if (result != FR_OK)
            goto done;

        result = f_mount(&filesystem, "", 1);
    }

    ready = result == FR_OK && filesystem.fs_type == FS_FAT32;

done:
    unmount_filesystem();
    clear_bytes(work, sizeof(work));
    return ready;
}

static bool matches_password(const uint8_t *contents, size_t size,
                             size_t password_offset, size_t *password_len)
{
    const char prefix[] = "password:";
    const size_t prefix_len = sizeof(prefix) - 1;

    if (password_offset < prefix_len || password_offset >= size)
        return false;
    if (memcmp(contents + password_offset - prefix_len, prefix, prefix_len) != 0)
        return false;

    size_t length = *password_len;
    if (length > size - password_offset)
        length = size - password_offset;
    if (length == 0 || length > UNLOCK_PASSWORD_MAX)
        return false;

    for (size_t offset = 0; offset < length; offset++) {
        if (contents[password_offset + offset] != 0)
            return false;
    }

    size_t end = password_offset + length;
    if (end < size && contents[end] != '\r' && contents[end] != '\n' &&
        contents[end] != '\0')
        return false;

    *password_len = length;
    return true;
}

static bool file_matches(const char *path, uint32_t lba,
                         size_t password_offset, size_t *password_len)
{
    FIL file;
    uint8_t contents[UNLOCK_FILE_MAX_SIZE];
    UINT bytes_read;
    size_t length = *password_len;
    bool found = false;

    if (f_open(&file, path, FA_READ) != FR_OK)
        goto done;

    FSIZE_t size = f_size(&file);
    if (size == 0)
        goto close_file;
    if (size > sizeof(contents))
        goto close_file;

    FRESULT result = f_read(&file, contents, (UINT)size, &bytes_read);
    if (result != FR_OK)
        goto close_file;
    if (bytes_read != size)
        goto close_file;

    if (file.sect == lba)
        found = matches_password(contents, bytes_read, password_offset, &length);

close_file:
    if (f_close(&file) != FR_OK)
        found = false;

done:
    clear_bytes(contents, sizeof(contents));
    if (found)
        *password_len = length;
    return found;
}

bool unlock_file_matches(uint32_t lba, size_t password_offset, size_t *password_len)
{
    DIR directory;
    FILINFO info;
    bool found = false;

    unlock_path[0] = '\0';
    /* A fresh mount sees directory and FAT updates from the latest host writes. */
    if (!mount_filesystem())
        goto done;
    if (f_opendir(&directory, "/") != FR_OK)
        goto done;

    while (1) {
        FRESULT result = f_readdir(&directory, &info);
        if (result != FR_OK || info.fname[0] == '\0')
            break;
        if (info.fattrib & AM_DIR)
            continue;
        if (info.fsize == 0 || info.fsize > UNLOCK_FILE_MAX_SIZE)
            continue;

        unlock_path[0] = '/';
        strcpy(unlock_path + 1, info.fname);
        found = file_matches(unlock_path, lba, password_offset, password_len);
        if (found)
            break;
    }

    if (f_closedir(&directory) != FR_OK)
        found = false;

done:
    unmount_filesystem();
    if (!found)
        unlock_path[0] = '\0';
    return found;
}

bool unlock_file_delete(void)
{
    FIL file;
    uint8_t blank[UNLOCK_FILE_MAX_SIZE] = {0};
    UINT bytes_written;
    bool deleted = false;

    if (unlock_path[0] == '\0')
        return false;
    if (!mount_filesystem())
        goto done;

    if (f_open(&file, unlock_path, FA_WRITE) != FR_OK)
        goto done;

    FSIZE_t size = f_size(&file);
    bool erased = size <= sizeof(blank) &&
                  f_write(&file, blank, (UINT)size, &bytes_written) == FR_OK &&
                  bytes_written == size;

    /* Close flushes the overwritten data before unlink frees its clusters. */
    if (f_close(&file) != FR_OK)
        erased = false;

    if (erased)
        deleted = f_unlink(unlock_path) == FR_OK;

done:
    unmount_filesystem();
    if (deleted)
        unlock_path[0] = '\0';
    return deleted;
}
