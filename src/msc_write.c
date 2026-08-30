/* msc_write.c - MSC WRITE10 over USB2
 * SPDX-License-Identifier: Apache-2.0
 */
#include "msc_write.h"
#include "emmc_ops.h"
#include "write_profile.h"
#include "CH56x_usb20_devbulk.h"
#include <stdbool.h>
#include <string.h>

extern void bot_set_sense(uint8_t key, uint8_t asc, uint8_t status);
extern void bot_send_csw(void);

static uint8_t write_chunk_to_sd(uint8_t *buf, uint32_t write_lba, uint16_t chunk_sectors)
{
    uint16_t reqnum = chunk_sectors;
    uint8_t status;
    uint32_t prof_start = write_profile_now();

    PFIC_DisableIRQ(EMMC_IRQn);
    R16_EMMC_INT_FG = 0xffff;
    TF_EMMCParam.EMMCOpErr = 0;
    TF_EMMCParam.EMMCSecSize = SECTOR_SIZE;

    if (chunk_sectors == 0) {
        status = CMD_SUCCESS;
    } else if (write_lba >= TF_EMMCParam.EMMCSecNum ||
               chunk_sectors > TF_EMMCParam.EMMCSecNum - write_lba) {
        status = OP_INVALID_ADD;
    } else {
        /* ACMD23 is best-effort; the CMD25/CMD12 transfer reports status. */
        if (chunk_sectors > 1)
            (void)EMMCCardSetPreErase(&TF_EMMCParam, chunk_sectors);
        status = EMMCCardWriteMulSec(&TF_EMMCParam, &reqnum, buf, write_lba);
    }

    R16_EMMC_INT_FG = 0xffff;
    TF_EMMCParam.EMMCOpErr = 0;
    PFIC_EnableIRQ(EMMC_IRQn);
    write_profile_add_sd(prof_start);

    return status;
}

static uint8_t *alternate_write_buf(uint8_t *buf)
{
    return (buf == UDisk_Out_Buf) ? UDisk_In_Buf : UDisk_Out_Buf;
}

static void receive_chunk_usb2(uint8_t *buf, uint16_t chunk_sectors,
                               bool *first_packet)
{
    uint16_t sectors_received = 0;
    uint32_t prof_start = write_profile_now();

    R8_USB_INT_FG = RB_USB_IF_TRANSFER;

    if (*first_packet) {
        memcpy(buf, endp1Rbuff, SECTOR_SIZE);
        sectors_received = 1;
        *first_packet = false;
    }

    while (sectors_received < chunk_sectors) {
        R32_UEP1_RX_DMA = (uint32_t)(buf + sectors_received * SECTOR_SIZE);
        __asm__ volatile("fence" ::: "memory");
        R8_UEP1_RX_CTRL = (R8_UEP1_RX_CTRL & ~RB_UEP_RRES_MASK) | UEP_R_RES_ACK;

        while (!(R8_USB_INT_FG & RB_USB_IF_TRANSFER)) {
            if (R8_USB_INT_FG & RB_USB_IF_SETUOACT)
                R8_USB_INT_FG = RB_USB_IF_SETUOACT;
        }

        R8_USB_INT_FG = RB_USB_IF_TRANSFER;
        R8_UEP1_RX_CTRL = (R8_UEP1_RX_CTRL & ~RB_UEP_RRES_MASK) | UEP_R_RES_NAK;
        sectors_received++;
    }

    write_profile_add_usb(prof_start);
}

static uint8_t write_received_chunk(uint8_t *buf, uint32_t lba, uint16_t chunk_sectors)
{
    uint32_t physical_lba = compute_physical_lba(lba);
    uint32_t prof_start = write_profile_now();

    write_profile_count_chunk();

    if(phantomdrive_is_locked() && !phantomdrive_get_unlock_pending()) {
        phantomdrive_snoop_write(buf, (uint32_t)chunk_sectors * SECTOR_SIZE);
    }

    write_profile_add_snoop(prof_start);
    if (!phantomdrive_is_locked()) {
        prof_start = write_profile_now();
        phantomdrive_encrypt_buf(buf, physical_lba, chunk_sectors);
        write_profile_add_crypt(prof_start);
    }
    return write_chunk_to_sd(buf, physical_lba, chunk_sectors);
}

static void wait_dat0_ready(void)
{
    uint32_t prof_start = write_profile_now();

    while (!EMMCDat0Sta);
    write_profile_add_dat0(prof_start);
}

static void write_stream_usb2(uint16_t total_sectors, uint32_t lba)
{
    uint16_t sectors_left = total_sectors;
    uint16_t buf_sectors = UDISK_BUF_SIZE / SECTOR_SIZE;
    uint16_t chunk_sectors;
    uint8_t *buf = UDisk_Out_Buf;
    bool first_packet = true;

    uint8_t uep0rxsave = R8_UEP0_RX_CTRL;
    uint8_t uep0txsave = R8_UEP0_TX_CTRL;
    PFIC_DisableIRQ(USBHS_IRQn);
    R8_UEP0_TX_CTRL = UEP_T_RES_NAK;
    R8_UEP0_RX_CTRL = UEP_R_RES_NAK;
    R8_UEP1_RX_CTRL |= RB_UEP_R_AUTOTOG;
    R8_USB_INT_FG = RB_USB_IF_TRANSFER;

    if (sectors_left > 0) {
        chunk_sectors = (sectors_left > buf_sectors) ? buf_sectors : sectors_left;
        receive_chunk_usb2(buf, chunk_sectors, &first_packet);

        while (sectors_left > 0) {
            uint8_t status = write_received_chunk(buf, lba, chunk_sectors);
            if (status != CMD_SUCCESS) {
                bot_set_sense(SENSE_KEY_MEDIUM_ERROR, SENSE_ASC_WRITE_ERROR, CSW_STATUS_FAILED);
                break;
            }

            lba += chunk_sectors;
            sectors_left -= chunk_sectors;
            if (sectors_left == 0) {
                wait_dat0_ready();
                break;
            }

            buf = alternate_write_buf(buf);
            chunk_sectors = (sectors_left > buf_sectors) ? buf_sectors : sectors_left;
            receive_chunk_usb2(buf, chunk_sectors, &first_packet);
            wait_dat0_ready();
        }
    }

    R32_UEP1_RX_DMA = (uint32_t)(uint8_t *)endp1Rbuff;
    R8_UEP1_RX_CTRL &= ~RB_UEP_R_AUTOTOG;
    PFIC_EnableIRQ(USBHS_IRQn);
    R8_UEP0_TX_CTRL = uep0txsave;
    R8_UEP0_RX_CTRL = uep0rxsave;
}

void msc_write_sectors(void)
{
    uint16_t total_sectors = g_bot.transfer_bytes_left / SECTOR_SIZE;
    uint32_t lba = g_bot.current_lba;

    g_bot.transfer_bytes_left = 0;

    write_profile_reset(total_sectors);

    write_stream_usb2(total_sectors, lba);

    write_profile_log();

    if (g_bot.transfer_bytes_left == 0) {
        g_bot.transfer_flags &= ~BOT_FLAG_DATA_OUT;
        bot_send_csw();
    }
}
