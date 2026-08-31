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
#include <string.h>

void bot_set_sense(uint8_t key, uint8_t asc, uint8_t status)
{
    g_bot.sense_key  = key;
    g_bot.sense_asc  = asc;
    g_bot.csw_status = status;
}

static void bot_stall_active_data_endpoints(void)
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

static bool bot_cbw_has_valid_signature(void)
{
    return (g_cbw_csw.mCBW.mCBW_Sig[0] == 'U') &&
           (g_cbw_csw.mCBW.mCBW_Sig[1] == 'S') &&
           (g_cbw_csw.mCBW.mCBW_Sig[2] == 'B') &&
           (g_cbw_csw.mCBW.mCBW_Sig[3] == 'C');
}

static void bot_begin_cbw_transfer(void)
{
    g_bot.cbw_tag[0] = g_cbw_csw.mCBW.mCBW_Tag[0];
    g_bot.cbw_tag[1] = g_cbw_csw.mCBW.mCBW_Tag[1];
    g_bot.cbw_tag[2] = g_cbw_csw.mCBW.mCBW_Tag[2];
    g_bot.cbw_tag[3] = g_cbw_csw.mCBW.mCBW_Tag[3];

    /* dCBWDataTransferLength is little-endian. Bit 7 of bmCBWFlags
     * describes the data phase from the host's point of view. */
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
}

static void bot_reject_invalid_cbw_signature(void)
{
    bot_set_sense(SENSE_KEY_ILLEGAL_REQUEST, SENSE_ASC_INVALID_COMMAND,
                  CSW_STATUS_PHASE_ERROR);
    g_bot.transfer_flags |= BOT_FLAG_DATA_IN;
    g_bot.transfer_flags |= BOT_FLAG_DATA_OUT;
    bot_stall_active_data_endpoints();
}

static void scsi_copy_response_to_bot_buffer(const uint8_t *response,
                                             uint32_t response_size)
{
    if (g_bot.transfer_bytes_left > response_size)
        g_bot.transfer_bytes_left = response_size;

    /* Fixed SCSI responses reuse the 31-byte CBW/CSW backing buffer. A handler
     * must read any command-specific CDB fields before calling this function. */
    memcpy(g_cbw_csw.buf, response, g_bot.transfer_bytes_left);
    g_response_ptr = g_cbw_csw.buf;
}

static bool read10_accesses_hidden_sd_sectors(void)
{
    uint32_t lba = g_bot.current_lba;
    uint32_t sector_count = g_bot.transfer_bytes_left / SECTOR_SIZE;

    if (sector_count == 0)
        return false;
    if (lba >= LOCKED_SECTORS)
        return true;

    return sector_count > LOCKED_SECTORS - lba;
}

static void scsi_parse_rw10_cdb(void)
{
    /* READ(10) and WRITE(10) encode the LBA and block count big-endian. From
     * this point, transfer_bytes_left tracks the SD transfer, not the CBW. */
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

static void scsi_handle_inquiry(void)
{
    if (g_bot.transfer_bytes_left > INQUIRY_RESPONSE_SIZE)
        g_bot.transfer_bytes_left = INQUIRY_RESPONSE_SIZE;
    g_inquiry_response[0] = 0x00;
    g_response_ptr = g_inquiry_response;
    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
}

static void scsi_handle_read_format_capacity(void)
{
    scsi_copy_response_to_bot_buffer(g_format_capacity_response, 0x0C);
    g_cbw_csw.buf[4] = (g_bot.capacity >> 24) & 0xFF;
    g_cbw_csw.buf[5] = (g_bot.capacity >> 16) & 0xFF;
    g_cbw_csw.buf[6] = (g_bot.capacity >> 8) & 0xFF;
    g_cbw_csw.buf[7] = (g_bot.capacity) & 0xFF;
    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
}

static void scsi_handle_read_capacity(void)
{
    scsi_copy_response_to_bot_buffer(g_read_capacity_response, 0x08);
    /* READ CAPACITY returns the final valid LBA, not the sector count. */
    g_cbw_csw.buf[0] = ((g_bot.capacity - 1) >> 24) & 0xFF;
    g_cbw_csw.buf[1] = ((g_bot.capacity - 1) >> 16) & 0xFF;
    g_cbw_csw.buf[2] = ((g_bot.capacity - 1) >> 8) & 0xFF;
    g_cbw_csw.buf[3] = ((g_bot.capacity - 1)) & 0xFF;
    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
}

static void scsi_handle_read10(void)
{
    scsi_parse_rw10_cdb();
    if (phantomdrive_is_locked() && read10_accesses_hidden_sd_sectors()) {
        bot_set_sense(SENSE_KEY_ILLEGAL_REQUEST, SENSE_ASC_LBA_OUT_OF_RANGE,
                      CSW_STATUS_FAILED);
        bot_stall_active_data_endpoints();
    }

    /* bot_handle_bulk_out() marks the validated read for bot_poll(). */
}

static void scsi_handle_write10(void)
{
    scsi_parse_rw10_cdb();
    /* DATA_OUT remains set so the next EP1 OUT event starts the SD write. */
}

static void scsi_handle_write_and_verify10(void)
{
    /* Verification is not implemented; this command currently behaves exactly
     * like WRITE(10). Keep that limitation explicit at the dispatch boundary. */
    scsi_handle_write10();
}

static void scsi_handle_mode_sense6(void)
{
    scsi_copy_response_to_bot_buffer(g_mode_sense6_response, 0x0C);
    g_cbw_csw.buf[4] = (g_bot.capacity >> 24) & 0xFF;
    g_cbw_csw.buf[5] = (g_bot.capacity >> 16) & 0xFF;
    g_cbw_csw.buf[6] = (g_bot.capacity >> 8) & 0xFF;
    g_cbw_csw.buf[7] = (g_bot.capacity) & 0xFF;
}

static void scsi_handle_mode_sense10(void)
{
    if (g_cbw_csw.mCBW.mCBW_CB_Buf[2] != 0x3F) {
        bot_set_sense(SENSE_KEY_ILLEGAL_REQUEST, SENSE_ASC_INVALID_COMMAND,
                      CSW_STATUS_FAILED);
        bot_stall_active_data_endpoints();
        return;
    }

    scsi_copy_response_to_bot_buffer(g_mode_sense10_response, 0x10);
    g_cbw_csw.buf[8]  = (g_bot.capacity >> 24) & 0xFF;
    g_cbw_csw.buf[9]  = (g_bot.capacity >> 16) & 0xFF;
    g_cbw_csw.buf[10] = (g_bot.capacity >> 8) & 0xFF;
    g_cbw_csw.buf[11] = (g_bot.capacity) & 0xFF;
}

static void scsi_handle_request_sense(void)
{
    /* REQUEST SENSE reports the saved error from the previous command. */
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
}

static void scsi_handle_test_unit_ready(void)
{
    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
}

static void scsi_handle_prevent_allow_removal(void)
{
    /* PhantomDrive has no software-controlled lock for its SD card slot. */
    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
}

static void scsi_handle_verify10(void)
{
    /* PhantomDrive currently acknowledges VERIFY(10) without rereading data. */
    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
}

static void scsi_handle_start_stop_unit(void)
{
    /* START STOP UNIT does not power down or eject PhantomDrive's SD card. */
    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
}

static void scsi_handle_synchronize_cache(void)
{
    /* PhantomDrive keeps no firmware write cache that needs flushing here. */
    bot_set_sense(SENSE_KEY_NO_ERROR, SENSE_ASC_NO_ERROR, CSW_STATUS_PASSED);
}

static void scsi_handle_unsupported(void)
{
    bot_set_sense(SENSE_KEY_ILLEGAL_REQUEST, SENSE_ASC_INVALID_COMMAND,
                  CSW_STATUS_FAILED);
    g_bot.transfer_flags |= BOT_FLAG_DATA_IN;
    bot_stall_active_data_endpoints();
}

static void scsi_dispatch_command(void)
{
    switch (g_cbw_csw.mCBW.mCBW_CB_Buf[0]) {
        case CMD_U_INQUIRY:
            scsi_handle_inquiry();
            break;

        case CMD_U_READ_FORMAT_CAPACITY:
            scsi_handle_read_format_capacity();
            break;

        case CMD_U_READ_CAPACITY:
            scsi_handle_read_capacity();
            break;

        case CMD_U_READ10:
            scsi_handle_read10();
            break;

        case CMD_U_WRITE10:
            scsi_handle_write10();
            break;

        case CMD_U_WR_VERIFY10:
            scsi_handle_write_and_verify10();
            break;

        case CMD_U_MODE_SENSE:
            scsi_handle_mode_sense6();
            break;

        case CMD_U_MODE_SENSE2:
            scsi_handle_mode_sense10();
            break;

        case CMD_U_REQUEST_SENSE:
            scsi_handle_request_sense();
            break;

        case CMD_U_TEST_READY:
            scsi_handle_test_unit_ready();
            break;

        case CMD_U_PREVT_REMOVE:
            scsi_handle_prevent_allow_removal();
            break;

        case CMD_U_VERIFY10:
            scsi_handle_verify10();
            break;

        case CMD_U_START_STOP:
            scsi_handle_start_stop_unit();
            break;

        case CMD_U_SYNC_CACHE:
            scsi_handle_synchronize_cache();
            break;

        default:
            scsi_handle_unsupported();
            break;
    }
}

static void bot_dispatch_scsi(void)
{
    if (!bot_cbw_has_valid_signature()) {
        bot_reject_invalid_cbw_signature();
        return;
    }

    bot_begin_cbw_transfer();
    scsi_dispatch_command();
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

/* Called by USBHS_IRQHandler */
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
