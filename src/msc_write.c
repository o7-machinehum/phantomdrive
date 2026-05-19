/* msc_write.c - MSC WRITE10 over USB2
 * SPDX-License-Identifier: Apache-2.0
 */
#include "msc_write.h"
#include "emmc_ops.h"
#include "msc_diag.h"
#include "write_profile.h"
#include "CH56x_ecdc.h"
#include "CH56x_usb20_devbulk.h"
#include <string.h>

extern void bot_set_sense(uint8_t key, uint8_t asc, uint8_t status);
extern void bot_send_csw(void);

static void ecdc_prepare_write_sector(uint32_t write_lba)
{
    ovrd_ecdc_disable_data_path();
    ovrd_ecdc_set_sector_nonce(write_lba);
    ECDC_Excute(RAM_TO_PERIPHERAL_ENCRY, MODE_LITTLE_ENDIAN);
}

static uint8_t write_chunk_to_sd_ecdc(uint8_t *buf, uint32_t write_lba, uint16_t *reqnum)
{
    uint32_t cmd_arg_val;
    uint16_t cmd_set_val;
    uint8_t status;
    uint16_t next_sector = 1;
    uint32_t prof_start;
    uint32_t wait_start;

    write_profile_emmc_start(*reqnum);

    if (write_lba > TF_EMMCParam.EMMCSecNum) {
        write_profile_emmc_invalid_addr();
        return OP_INVALID_ADD;
    }

    cmd_arg_val = write_lba;
    cmd_set_val = RB_EMMC_CKIDX |
                  RB_EMMC_CKCRC |
                  RESP_TYPE_48  |
                  EMMC_CMD25;
    prof_start = write_profile_now();
    EMMCSendCmd(cmd_arg_val, cmd_set_val);
    while (1) {
        status = CheckCMDComp(&TF_EMMCParam);
        if (status != CMD_NULL)
            break;
    }
    write_profile_emmc_cmd25(prof_start, status);
    if (status == CMD_FAILED)
        return OP_FAILED;

    ecdc_prepare_write_sector(write_lba);
    R32_EMMC_TRAN_MODE = RB_EMMC_DMA_DIR;
    R32_EMMC_DMA_BEG1 = (uint32_t)buf;
    R32_EMMC_BLOCK_CFG = (TF_EMMCParam.EMMCSecSize << 16) | *reqnum;

    prof_start = write_profile_now();
    wait_start = prof_start;
    while (1) {
        if (R16_EMMC_INT_FG & RB_EMMC_IF_BKGAP) {
            write_profile_emmc_bkgap_wait(wait_start);
            if (next_sector < *reqnum)
                ecdc_prepare_write_sector(write_lba + next_sector++);

            R32_EMMC_RESPONSE3 = 0;
            R16_EMMC_INT_FG = RB_EMMC_IF_BKGAP;
            wait_start = write_profile_now();
        } else if (R16_EMMC_INT_FG & RB_EMMC_IF_TRANDONE) {
            write_profile_emmc_trandone_wait(wait_start);
            ovrd_ecdc_disable_data_path();
            R16_EMMC_INT_FG = RB_EMMC_IF_CMDDONE;
            cmd_arg_val = 0;
            cmd_set_val = RB_EMMC_CKIDX |
                          RB_EMMC_CKCRC |
                          RESP_TYPE_R1b |
                          EMMC_CMD12;
            EMMCSendCmd(cmd_arg_val, cmd_set_val);
            break;
        }

        if (TF_EMMCParam.EMMCOpErr) {
            ovrd_ecdc_disable_data_path();
            write_profile_emmc_data(prof_start, 1);
            return CMD_FAILED;
        }
    }

    write_profile_emmc_data(prof_start, 0);
    prof_start = write_profile_now();
    while (1) {
        status = CheckCMDComp(&TF_EMMCParam);
        if (status != CMD_NULL)
            break;
    }
    write_profile_emmc_cmd12(prof_start, status);

    R16_EMMC_INT_FG = 0xffff;
    *reqnum = (uint16_t)R32_EMMC_STATUS;
    write_profile_emmc_done(*reqnum);

    return status;
}

static uint8_t write_chunk_to_sd(uint8_t *buf, uint32_t write_lba, uint16_t chunk_sectors)
{
    uint16_t reqnum = chunk_sectors;
    uint8_t status;
    uint32_t prof_start = write_profile_now();

    PFIC_DisableIRQ(EMMC_IRQn);
    R16_EMMC_INT_FG = 0xffff;
    TF_EMMCParam.EMMCOpErr = 0;
    TF_EMMCParam.EMMCSecSize = SECTOR_SIZE;

    if (ovrd_state == STATE_UNLOCKED)
        status = write_chunk_to_sd_ecdc(buf, write_lba, &reqnum);
    else
        status = EMMCCardWriteMulSec(&TF_EMMCParam, &reqnum, buf, write_lba);

    R16_EMMC_INT_FG = 0xffff;
    TF_EMMCParam.EMMCOpErr = 0;
    PFIC_EnableIRQ(EMMC_IRQn);
    write_profile_add_sd(prof_start);

    diag_log_write_result(status, chunk_sectors, reqnum);
    return status;
}

static uint8_t *alternate_write_buf(uint8_t *buf)
{
    return (buf == UDisk_Out_Buf) ? UDisk_In_Buf : UDisk_Out_Buf;
}

static void receive_chunk_usb2(uint8_t *buf, uint16_t chunk_sectors, uint32_t lba, uint8_t *first)
{
    uint16_t sectors_received = 0;
    uint32_t prof_start = write_profile_now();

    R8_USB_INT_FG = RB_USB_IF_TRANSFER;
    diag_log_write_chunk(lba, chunk_sectors);

    if (*first) {
        memcpy(buf, endp1Rbuff, SECTOR_SIZE);
        sectors_received = 1;
        *first = 0;
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
    diag_write_checksums_usb2(buf, chunk_sectors);
}

static uint8_t write_received_chunk(uint8_t *buf, uint32_t lba, uint16_t chunk_sectors)
{
    uint32_t physical_lba = compute_physical_lba(lba);
    uint32_t prof_start = write_profile_now();

    write_profile_count_chunk();
    ovrd_snoop_write(buf, (uint32_t)chunk_sectors * SECTOR_SIZE);
    write_profile_add_snoop(prof_start);
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
    uint8_t first = 1;

    uint8_t uep0rxsave = R8_UEP0_RX_CTRL;
    uint8_t uep0txsave = R8_UEP0_TX_CTRL;
    PFIC_DisableIRQ(USBHS_IRQn);
    R8_UEP0_TX_CTRL = UEP_T_RES_NAK;
    R8_UEP0_RX_CTRL = UEP_R_RES_NAK;
    R8_UEP1_RX_CTRL |= RB_UEP_R_AUTOTOG;
    R8_USB_INT_FG = RB_USB_IF_TRANSFER;

    if (sectors_left > 0) {
        chunk_sectors = (sectors_left > buf_sectors) ? buf_sectors : sectors_left;
        receive_chunk_usb2(buf, chunk_sectors, lba, &first);

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
            receive_chunk_usb2(buf, chunk_sectors, lba, &first);
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
