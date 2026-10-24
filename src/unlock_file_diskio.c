#include "ff.h"
#include "diskio.h"
#include "bot_state.h"
#include "phantomdrive.h"
#include "unlock_file_diskio.h"
#include <string.h>

#define FILE_IO_TIMEOUT_MS 1000
#define FORMAT_TIMEOUT_MS 120000
#define EMMC_ERROR_FLAGS (RB_EMMC_IF_FIFO_OV | RB_EMMC_IF_TRANERR | \
                          RB_EMMC_IF_DATTMO | RB_EMMC_IF_REIDX_ER | \
                          RB_EMMC_IF_RECRC_WR | RB_EMMC_IF_RE_TMOUT)

/* CH569 SD DMA requires a 16-byte aligned buffer in RAMX. */
static BYTE sector_buffer[SECTOR_SIZE] __attribute__((aligned(16)))
    __attribute__((section(".DMADATA")));
static bool initialized;
static bool formatting;
static uint64_t io_started_ticks;
static uint64_t transfer_started_ticks;

void unlock_file_diskio_set_formatting(bool enabled)
{
    formatting = enabled;
}

static bool io_failed(void)
{
    uint64_t now = bsp_get_tick();
    uint64_t ticks_per_ms = bsp_get_nbtick_1ms();
    uint32_t operation_timeout_ms = formatting ? FORMAT_TIMEOUT_MS : FILE_IO_TIMEOUT_MS;

    return (R16_EMMC_INT_FG & EMMC_ERROR_FLAGS) ||
           TF_EMMCParam.EMMCOpErr ||
           now - transfer_started_ticks >= FILE_IO_TIMEOUT_MS * ticks_per_ms ||
           now - io_started_ticks >= operation_timeout_ms * ticks_per_ms;
}

static bool wait_for_flags(uint16_t flags)
{
    while (1) {
        if (io_failed())
            return false;
        if ((R16_EMMC_INT_FG & flags) == flags) {
            R16_EMMC_INT_FG = flags;
            return true;
        }
    }
}

static bool wait_for_ready(void)
{
    while (1) {
        if (io_failed())
            return false;
        if (EMMCDat0Sta)
            return true;
    }
}

static bool finish_io(bool success)
{
    if (!success) {
        /* Stop the controller's data path before another owner uses SD DMA. */
        uint8_t control = R8_EMMC_CONTROL & ~RB_EMMC_RST_LGC;
        R8_EMMC_CONTROL = control | RB_EMMC_RST_LGC;
        R8_EMMC_CONTROL = control;
        initialized = false;
    }

    R32_EMMC_TRAN_MODE = 0;
    R16_EMMC_INT_FG = 0xffff;
    TF_EMMCParam.EMMCOpErr = 0;
    PFIC_EnableIRQ(EMMC_IRQn);
    return success;
}

static bool transfer_sector(uint32_t sector, bool write)
{
    uint16_t command = RB_EMMC_CKIDX | RB_EMMC_CKCRC | RESP_TYPE_48;

    transfer_started_ticks = bsp_get_tick();
    PFIC_DisableIRQ(EMMC_IRQn);
    R16_EMMC_INT_FG = 0xffff;
    TF_EMMCParam.EMMCOpErr = 0;
    if (!wait_for_ready())
        return finish_io(false);

    R32_EMMC_TRAN_MODE = 0;

    if (write) {
        EMMCSendCmd(sector, command | EMMC_CMD24);
        if (!wait_for_flags(RB_EMMC_IF_CMDDONE))
            return finish_io(false);

        __asm__ volatile("fence" ::: "memory");
        R32_EMMC_TRAN_MODE = RB_EMMC_DMA_DIR;
        R32_EMMC_DMA_BEG1 = (uint32_t)sector_buffer;
        R32_EMMC_BLOCK_CFG = (SECTOR_SIZE << 16) | 1;
    } else {
        R32_EMMC_DMA_BEG1 = (uint32_t)sector_buffer;
        R32_EMMC_BLOCK_CFG = (SECTOR_SIZE << 16) | 1;
        __asm__ volatile("fence" ::: "memory");
        EMMCSendCmd(sector, command | EMMC_CMD17);
        if (!wait_for_flags(RB_EMMC_IF_CMDDONE))
            return finish_io(false);
    }

    if (!wait_for_flags(RB_EMMC_IF_TRANDONE) || !wait_for_ready())
        return finish_io(false);

    __asm__ volatile("fence" ::: "memory");
    return finish_io(true);
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0 || !initialized || !phantomdrive_is_locked() ||
        TF_EMMCParam.EMMCSecNum < LOCKED_SECTORS)
        return STA_NOINIT;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0)
        return STA_NOINIT;

    io_started_ticks = bsp_get_tick();
    initialized = true;
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || buff == NULL || count == 0 ||
        sector >= LOCKED_SECTORS || count > LOCKED_SECTORS - sector)
        return RES_PARERR;
    if (disk_status(pdrv) & STA_NOINIT)
        return RES_NOTRDY;

    for (UINT i = 0; i < count; i++) {
        if (!transfer_sector((uint32_t)(sector + i), false))
            return RES_ERROR;
        memcpy(buff + i * SECTOR_SIZE, sector_buffer, SECTOR_SIZE);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || buff == NULL || count == 0 ||
        sector >= LOCKED_SECTORS || count > LOCKED_SECTORS - sector)
        return RES_PARERR;
    if (disk_status(pdrv) & STA_NOINIT)
        return RES_NOTRDY;

    for (UINT i = 0; i < count; i++) {
        memcpy(sector_buffer, buff + i * SECTOR_SIZE, SECTOR_SIZE);
        if (!transfer_sector((uint32_t)(sector + i), true))
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0)
        return RES_PARERR;
    if (disk_status(pdrv) & STA_NOINIT)
        return RES_NOTRDY;

    if (cmd == CTRL_SYNC) {
        transfer_started_ticks = bsp_get_tick();
        PFIC_DisableIRQ(EMMC_IRQn);
        return finish_io(wait_for_ready()) ? RES_OK : RES_ERROR;
    }
    if (buff == NULL)
        return RES_PARERR;

    switch (cmd) {
        case GET_SECTOR_COUNT:
            *(LBA_t *)buff = LOCKED_SECTORS;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = SECTOR_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
