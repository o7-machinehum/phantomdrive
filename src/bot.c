/* bot.c - BOT (Bulk-Only Transport) protocol layer
 *
 * Host PC <--USB--> BOT (this file) <--SCSI--> msc_read/msc_write
 *
 * [ref/USB_MSC_Bulk_Only_Transport_v1.0.pdf]
 * [ref/SCSI_Block_Commands_SBC3_r25.pdf]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "bot.h"
#include "scsi_tables.h"
#include "msc_read.h"
#include "msc_write.h"
#include "phantomdrive.h"
#include "CH56x_usb20_devbulk.h"
#include "debug.h"
#include <string.h>

void bot_set_sense(uint8_t key, uint8_t asc, uint8_t status)
{
    g_bot.sense_key  = key;
    g_bot.sense_asc  = asc;
    g_bot.csw_status = status;
}

void bot_stall_endpoints(void)
{
    if (g_bot.transfer_flags & BOT_FLAG_DATA_IN) {
        R8_UEP1_TX_CTRL = (R8_UEP1_TX_CTRL & ~RB_UEP_TRES_MASK) | UEP_T_RES_STALL;
        g_bot.transfer_flags &= ~BOT_FLAG_DATA_IN;
    }
    if (g_bot.transfer_flags & BOT_FLAG_DATA_OUT) {
        R8_UEP1_RX_CTRL = (R8_UEP1_RX_CTRL & ~RB_UEP_RRES_MASK) | UEP_R_RES_STALL;
        g_bot.transfer_flags &= ~BOT_FLAG_DATA_OUT;
    }
}

static bool read10_enters_hidden_region(void)
{
    uint32_t lba = g_bot.current_lba;
    uint32_t sector_count = g_bot.transfer_bytes_left / SECTOR_SIZE;

    if (sector_count == 0)
        return false;
    if (lba >= LOCKED_SECTORS)
        return true;

    return sector_count > LOCKED_SECTORS - lba;
}

void scsi_parse_rw10_cdb(void)
{
    g_bot.current_lba = (uint32_t)g_cbw_csw.mCBW.mCBW_CB_Buf[2] << 24;
    g_bot.current_lba += (uint32_t)g_cbw_csw.mCBW.mCBW_CB_Buf[3] << 16;
    g_bot.current_lba += (uint32_t)g_cbw_csw.mCBW.mCBW_CB_Buf[4] << 8;
    g_bot.current_lba += (uint32_t)g_cbw_csw.mCBW.mCBW_CB_Buf[5];

    g_bot.transfer_bytes_left = (uint32_t)g_cbw_csw.mCBW.mCBW_CB_Buf[7] << 8;
    g_bot.transfer_bytes_left += (uint32_t)g_cbw_csw.mCBW.mCBW_CB_Buf[8];
    g_bot.transfer_bytes_left *= SECTOR_SIZE;

    g_bot.sectors_done = 0x00;
    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
}

void bot_dispatch_scsi(void)
{
    uint8_t i;

    if ((g_cbw_csw.mCBW.mCBW_Sig[0] == 'U') && (g_cbw_csw.mCBW.mCBW_Sig[1] == 'S') &&
        (g_cbw_csw.mCBW.mCBW_Sig[2] == 'B') && (g_cbw_csw.mCBW.mCBW_Sig[3] == 'C'))
    {
        g_bot.cbw_tag[0] = g_cbw_csw.mCBW.mCBW_Tag[0];
        g_bot.cbw_tag[1] = g_cbw_csw.mCBW.mCBW_Tag[1];
        g_bot.cbw_tag[2] = g_cbw_csw.mCBW.mCBW_Tag[2];
        g_bot.cbw_tag[3] = g_cbw_csw.mCBW.mCBW_Tag[3];

        g_bot.transfer_bytes_left  = (uint32_t)g_cbw_csw.mCBW.mCBW_DataLen[3] << 24;
        g_bot.transfer_bytes_left += (uint32_t)g_cbw_csw.mCBW.mCBW_DataLen[2] << 16;
        g_bot.transfer_bytes_left += (uint32_t)g_cbw_csw.mCBW.mCBW_DataLen[1] << 8;
        g_bot.transfer_bytes_left += (uint32_t)g_cbw_csw.mCBW.mCBW_DataLen[0];

        if (g_bot.transfer_bytes_left) {
            if (g_cbw_csw.mCBW.mCBW_Flag & 0x80)
                g_bot.transfer_flags |= BOT_FLAG_DATA_IN;
            else
                g_bot.transfer_flags |= BOT_FLAG_DATA_OUT;
        }
        g_bot.transfer_flags |= BOT_FLAG_CSW_PENDING;

#ifdef DEBUG_USB
        log_printf("CBW: cmd=0x%02X len=%d\r\n",
                   g_cbw_csw.mCBW.mCBW_CB_Buf[0], g_bot.transfer_bytes_left);
#endif

        switch (g_cbw_csw.mCBW.mCBW_CB_Buf[0])
        {
            case CMD_U_INQUIRY:
                if (g_bot.transfer_bytes_left > INQUIRY_RESPONSE_SIZE)
                    g_bot.transfer_bytes_left = INQUIRY_RESPONSE_SIZE;
                g_inquiry_response[0] = 0x00;
                g_response_ptr = (uint8_t *)g_inquiry_response;
                bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
                break;

            case CMD_U_READ_FORMAT_CAPACITY:
                if (g_bot.device_ready) {
                    if (g_bot.transfer_bytes_left > 0x0C)
                        g_bot.transfer_bytes_left = 0x0C;
                    for (i = 0; i < g_bot.transfer_bytes_left; i++)
                        g_cbw_csw.buf[i] = g_format_capacity_response[i];
                    g_cbw_csw.buf[4] = (g_bot.capacity >> 24) & 0xFF;
                    g_cbw_csw.buf[5] = (g_bot.capacity >> 16) & 0xFF;
                    g_cbw_csw.buf[6] = (g_bot.capacity >> 8) & 0xFF;
                    g_cbw_csw.buf[7] = (g_bot.capacity) & 0xFF;
                    g_response_ptr = g_cbw_csw.buf;
                    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
                } else {
                    bot_set_sense(SENSE_KEY_NOT_READY, SENSE_ASC_MEDIUM_NOT_PRESENT, CSW_STATUS_FAILED);
                    bot_stall_endpoints();
                }
                break;

            case CMD_U_READ_CAPACITY:
                if (g_bot.device_ready) {
                    if (g_bot.transfer_bytes_left > 0x08)
                        g_bot.transfer_bytes_left = 0x08;
                    for (i = 0; i < g_bot.transfer_bytes_left; i++)
                        g_cbw_csw.buf[i] = g_read_capacity_response[i];
                    g_cbw_csw.buf[0] = ((g_bot.capacity - 1) >> 24) & 0xFF;
                    g_cbw_csw.buf[1] = ((g_bot.capacity - 1) >> 16) & 0xFF;
                    g_cbw_csw.buf[2] = ((g_bot.capacity - 1) >> 8) & 0xFF;
                    g_cbw_csw.buf[3] = ((g_bot.capacity - 1)) & 0xFF;
                    g_response_ptr = g_cbw_csw.buf;
                    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
                } else {
                    bot_set_sense(SENSE_KEY_NOT_READY, SENSE_ASC_MEDIUM_NOT_PRESENT, CSW_STATUS_FAILED);
                    bot_stall_endpoints();
                }
                break;

            case CMD_U_READ10:
                if (g_bot.device_ready) {
                    scsi_parse_rw10_cdb();
                    if (phantomdrive_is_locked() &&
                        read10_enters_hidden_region()) {
                        bot_set_sense(SENSE_KEY_ILLEGAL_REQUEST,
                                      SENSE_ASC_LBA_OUT_OF_RANGE,
                                      CSW_STATUS_FAILED);
                        bot_stall_endpoints();
                    }
                } else {
                    bot_set_sense(SENSE_KEY_NOT_READY, SENSE_ASC_MEDIUM_NOT_PRESENT, CSW_STATUS_FAILED);
                    bot_stall_endpoints();
                }
                break;

            case CMD_U_WR_VERIFY10:
            case CMD_U_WRITE10:
                if (g_bot.device_ready)
                    scsi_parse_rw10_cdb();
                else {
                    bot_set_sense(SENSE_KEY_NOT_READY, SENSE_ASC_MEDIUM_NOT_PRESENT, CSW_STATUS_FAILED);
                    bot_stall_endpoints();
                }
                break;

            case CMD_U_MODE_SENSE:
                if (g_bot.device_ready) {
                    if (g_bot.transfer_bytes_left > 0x0C)
                        g_bot.transfer_bytes_left = 0x0C;
                    for (i = 0; i < g_bot.transfer_bytes_left; i++)
                        g_cbw_csw.buf[i] = g_mode_sense6_response[i];
                    g_cbw_csw.buf[4] = (g_bot.capacity >> 24) & 0xFF;
                    g_cbw_csw.buf[5] = (g_bot.capacity >> 16) & 0xFF;
                    g_cbw_csw.buf[6] = (g_bot.capacity >> 8) & 0xFF;
                    g_cbw_csw.buf[7] = (g_bot.capacity) & 0xFF;
                    g_response_ptr = g_cbw_csw.buf;
                } else {
                    bot_set_sense(SENSE_KEY_NOT_READY, SENSE_ASC_MEDIUM_NOT_PRESENT, CSW_STATUS_FAILED);
                    bot_stall_endpoints();
                }
                break;

            case CMD_U_MODE_SENSE2:
                if (g_cbw_csw.mCBW.mCBW_CB_Buf[2] == 0x3F) {
                    if (g_bot.transfer_bytes_left > 0x10)
                        g_bot.transfer_bytes_left = 0x10;
                    for (i = 0; i < g_bot.transfer_bytes_left; i++)
                        g_cbw_csw.buf[i] = g_mode_sense10_response[i];
                    g_cbw_csw.buf[8]  = (g_bot.capacity >> 24) & 0xFF;
                    g_cbw_csw.buf[9]  = (g_bot.capacity >> 16) & 0xFF;
                    g_cbw_csw.buf[10] = (g_bot.capacity >> 8) & 0xFF;
                    g_cbw_csw.buf[11] = (g_bot.capacity) & 0xFF;
                    g_response_ptr = g_cbw_csw.buf;
                } else {
                    bot_set_sense(SENSE_KEY_ILLEGAL_REQUEST, SENSE_ASC_INVALID_COMMAND, CSW_STATUS_FAILED);
                    bot_stall_endpoints();
                }
                break;

            case CMD_U_REQUEST_SENSE:
                g_cbw_csw.ReqSense.ErrorCode = 0x70;
                g_cbw_csw.ReqSense.Reserved1 = 0x00;
                g_cbw_csw.ReqSense.SenseKey = g_bot.sense_key;
                g_cbw_csw.ReqSense.Information[0] = 0x00;
                g_cbw_csw.ReqSense.Information[1] = 0x00;
                g_cbw_csw.ReqSense.Information[2] = 0x00;
                g_cbw_csw.ReqSense.Information[3] = 0x00;
                g_cbw_csw.ReqSense.SenseLength = 0x0A;
                g_cbw_csw.ReqSense.Reserved2[0] = 0x00;
                g_cbw_csw.ReqSense.Reserved2[1] = 0x00;
                g_cbw_csw.ReqSense.Reserved2[2] = 0x00;
                g_cbw_csw.ReqSense.Reserved2[3] = 0x00;
                g_cbw_csw.ReqSense.SenseCode = g_bot.sense_asc;
                g_cbw_csw.ReqSense.SenseCodeQua = 0x00;
                g_cbw_csw.ReqSense.Reserved3[0] = 0x00;
                g_cbw_csw.ReqSense.Reserved3[1] = 0x00;
                g_cbw_csw.ReqSense.Reserved3[2] = 0x00;
                g_cbw_csw.ReqSense.Reserved3[3] = 0x00;
                g_response_ptr = g_cbw_csw.buf;
                g_bot.csw_status = CSW_STATUS_PASSED;
                break;

            case CMD_U_TEST_READY:
                if (g_bot.device_ready)
                    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
                else {
                    bot_set_sense(SENSE_KEY_NOT_READY, SENSE_ASC_MEDIUM_NOT_PRESENT, CSW_STATUS_FAILED);
                    g_bot.transfer_flags |= BOT_FLAG_DATA_IN;
                    bot_stall_endpoints();
                }
                break;

            case CMD_U_PREVT_REMOVE:
            case CMD_U_VERIFY10:
            case CMD_U_START_STOP:
            case CMD_U_SYNC_CACHE:
                bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
                break;

            default:
#ifdef DEBUG_USB
                log_printf("SCSI: unsupported cmd 0x%02X\r\n", g_cbw_csw.mCBW.mCBW_CB_Buf[0]);
#endif
                bot_set_sense(SENSE_KEY_ILLEGAL_REQUEST, SENSE_ASC_INVALID_COMMAND, CSW_STATUS_FAILED);
                g_bot.transfer_flags |= BOT_FLAG_DATA_IN;
                bot_stall_endpoints();
                break;
        }
    }
    else
    {
        bot_set_sense(SENSE_KEY_ILLEGAL_REQUEST, SENSE_ASC_INVALID_COMMAND, CSW_STATUS_PHASE_ERROR);
        g_bot.transfer_flags |= BOT_FLAG_DATA_IN;
        g_bot.transfer_flags |= BOT_FLAG_DATA_OUT;
        bot_stall_endpoints();
    }
}

void bot_handle_bulk_in(void)
{
    if (g_bot.transfer_flags & BOT_FLAG_DATA_IN) {
        if (g_cbw_csw.mCBW.mCBW_CB_Buf[0] == CMD_U_READ10)
            g_bot.read_pending = true;
        else
            bot_send_response_data();
    } else if (g_bot.transfer_flags & BOT_FLAG_CSW_PENDING) {
        bot_send_csw();
    }
}

void bot_handle_bulk_out(uint8_t *pbuf, uint16_t packlen)
{
    uint32_t i;

    if (g_bot.transfer_flags & BOT_FLAG_DATA_OUT) {
        g_bot.write_pending = true;
    } else {
        if (packlen == CBW_SIZE) {
            for (i = 0; i < packlen; i++)
                g_cbw_csw.buf[i] = *pbuf++;

            bot_dispatch_scsi();

            if ((g_bot.transfer_flags & BOT_FLAG_DATA_OUT) == 0x00) {
                if (g_bot.transfer_flags & BOT_FLAG_DATA_IN) {
                    if (g_cbw_csw.mCBW.mCBW_CB_Buf[0] == CMD_U_READ10)
                        g_bot.read_pending = true;
                    else
                        bot_send_response_data();
                } else if (g_bot.csw_status == CSW_STATUS_PASSED) {
                    bot_send_csw();
                }
            }
        }
    }
}

void bot_send_response_data(void)
{
    uint32_t len;

    if (g_bot.transfer_bytes_left > g_bot.pack_size) {
        len = g_bot.pack_size;
        g_bot.transfer_bytes_left -= g_bot.pack_size;
    } else {
        len = g_bot.transfer_bytes_left;
        g_bot.transfer_bytes_left = 0x00;
        g_bot.transfer_flags &= ~BOT_FLAG_DATA_IN;
    }

    memcpy(endp1Tbuff, g_response_ptr, len);
    R16_UEP1_T_LEN = len;
    R32_UEP1_TX_DMA = (uint32_t)(uint8_t *)endp1Tbuff;
    R8_UEP1_TX_CTRL = (R8_UEP1_TX_CTRL & ~RB_UEP_TRES_MASK) | UEP_T_RES_ACK;
}

void bot_send_csw(void)
{
    g_bot.transfer_flags = 0x00;
#ifdef DEBUG_USB
    log_printf("CSW: sta=%d\r\n", g_bot.csw_status);
#endif

    g_cbw_csw.mCSW.mCSW_Sig[0] = 'U';
    g_cbw_csw.mCSW.mCSW_Sig[1] = 'S';
    g_cbw_csw.mCSW.mCSW_Sig[2] = 'B';
    g_cbw_csw.mCSW.mCSW_Sig[3] = 'S';
    g_cbw_csw.mCSW.mCSW_Tag[0] = g_bot.cbw_tag[0];
    g_cbw_csw.mCSW.mCSW_Tag[1] = g_bot.cbw_tag[1];
    g_cbw_csw.mCSW.mCSW_Tag[2] = g_bot.cbw_tag[2];
    g_cbw_csw.mCSW.mCSW_Tag[3] = g_bot.cbw_tag[3];
    g_cbw_csw.mCSW.mCSW_Residue[0] = 0x00;
    g_cbw_csw.mCSW.mCSW_Residue[1] = 0x00;
    g_cbw_csw.mCSW.mCSW_Residue[2] = 0x00;
    g_cbw_csw.mCSW.mCSW_Residue[3] = 0x00;
    g_cbw_csw.mCSW.mCSW_Status = g_bot.csw_status;

    memcpy(endp1Tbuff, (uint8_t *)g_cbw_csw.buf, CSW_SIZE);
    R16_UEP1_T_LEN = CSW_SIZE;
    R32_UEP1_TX_DMA = (uint32_t)(uint8_t *)endp1Tbuff;
    R8_UEP1_TX_CTRL = (R8_UEP1_TX_CTRL & ~RB_UEP_TRES_MASK) | UEP_T_RES_ACK;
}

void bot_poll(void)
{
    if (g_bot.read_pending) {
        // Host wants to read sectors from SD
        g_bot.read_pending = false;
        msc_read_sectors();
    }

    if (g_bot.write_pending) {
        // Host wants to write sectors to SD
        g_bot.write_pending = false;
        msc_write_sectors();

        R8_UEP1_RX_CTRL = (R8_UEP1_RX_CTRL & ~RB_UEP_RRES_MASK) | UEP_R_RES_ACK;
    }

    // If we want to unlock
    if(phantomdrive_get_unlock_pending()) {
        phantomdrive_unlock();
    }
}
