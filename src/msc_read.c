/* msc_read.c - MSC READ10: streaming circular buffer from eMMC to USB
 * SPDX-License-Identifier: Apache-2.0
 */
#include "msc_read.h"
#include "emmc_ops.h"
#include "msc_diag.h"
#include "CH56x_usb20_devbulk.h"
#include <stdbool.h>

extern void bot_send_csw(void);

#define READ_RING_SECTORS (UDISK_BUF_SIZE / SECTOR_SIZE)

static inline uint8_t next_read_ring_slot(uint8_t slot)
{
    slot++;
    if (slot == READ_RING_SECTORS)
        slot = 0;
    return slot;
}

static void queue_usb_read_sector(read_diag_t *diag, uint8_t usb_slot,
                                  uint16_t usb_done)
{
    uint8_t *sector = UDisk_In_Buf + usb_slot * SECTOR_SIZE;

    diag_check_usb_sector(diag, sector, usb_done);

    R32_UEP1_TX_DMA = (uint32_t)(uint8_t *)sector;
    R16_UEP1_T_LEN = SECTOR_SIZE;
    __asm__ volatile("fence" ::: "memory");
    R8_UEP1_TX_CTRL = (R8_UEP1_TX_CTRL & ~RB_UEP_TRES_MASK) | UEP_T_RES_ACK;
}

static void read_stream_usb2(uint32_t actual_lba, uint16_t preqnum)
{
    uint16_t sd_done = 0, usb_done = 0;
    uint8_t sd_slot = 0, usb_slot = 0;
    bool sd_paused_for_usb_buffer = false;
    bool usb_first_packet = true;
    read_diag_t diag;

    uint8_t uep0rxsave = R8_UEP0_RX_CTRL;
    uint8_t uep0txsave = R8_UEP0_TX_CTRL;
    PFIC_DisableIRQ(USBHS_IRQn);
    R8_UEP0_TX_CTRL = UEP_T_RES_NAK;
    R8_UEP0_RX_CTRL = UEP_R_RES_NAK;
    R8_UEP1_TX_CTRL |= RB_UEP_T_AUTOTOG;

    diag_read_init(&diag, actual_lba);
    emmc_start_multiblock_read(UDisk_In_Buf, actual_lba, preqnum);

    /* Interleaved eMMC DMA read + USB2 TX polling loop */
    uint16_t xfer_count = 0;
    while (1)
    {
        bool usb_tx_done = R8_USB_INT_FG & RB_USB_IF_TRANSFER;

        if (usb_tx_done)
            R8_UEP1_TX_CTRL = (R8_UEP1_TX_CTRL & ~RB_UEP_TRES_MASK) | UEP_T_RES_NAK;

        /* USB sends one sector behind eMMC so it never sees the sector being filled. */
        if ((sd_done > 1 && usb_done < sd_done - 1) &&
            (usb_tx_done || usb_first_packet)) {
            if (usb_tx_done)
                xfer_count++;

            R8_USB_INT_FG = RB_USB_IF_TRANSFER;
            usb_first_packet = false;
            queue_usb_read_sector(&diag, usb_slot, usb_done);

            usb_done++;
            usb_slot = next_read_ring_slot(usb_slot);
            if (sd_paused_for_usb_buffer) {
                sd_paused_for_usb_buffer = false;
                emmc_release_gap_stop();
            }
        }

        if (R8_USB_INT_FG & RB_USB_IF_SETUOACT)
            R8_USB_INT_FG = RB_USB_IF_SETUOACT;

        if (R16_EMMC_INT_FG & RB_EMMC_IF_BKGAP) {
            uint8_t *sector = UDisk_In_Buf + sd_slot * SECTOR_SIZE;

            if (phantomdrive_state == STATE_UNLOCKED)
                phantomdrive_crypt_buf(sector, actual_lba + sd_done, 1);

            diag_check_sd_sector(&diag, sector, sd_done);

            R16_EMMC_INT_FG = RB_EMMC_IF_BKGAP;
            sd_done++;
            sd_slot = next_read_ring_slot(sd_slot);
            R32_EMMC_DMA_BEG1 = (uint32_t)(uint8_t *)(UDisk_In_Buf + sd_slot * SECTOR_SIZE);

            if ((sd_done - usb_done) < (READ_RING_SECTORS - 2))
                emmc_release_gap_stop();
            else
                sd_paused_for_usb_buffer = true;
        }
        else if (R16_EMMC_INT_FG & RB_EMMC_IF_TRANDONE) {
            uint8_t *sector = UDisk_In_Buf + sd_slot * SECTOR_SIZE;

            if (phantomdrive_state == STATE_UNLOCKED)
                phantomdrive_crypt_buf(sector, actual_lba + sd_done, 1);

            diag_check_sd_sector(&diag, sector, sd_done);

            R16_EMMC_INT_FG = RB_EMMC_IF_TRANDONE | RB_EMMC_IF_CMDDONE;
            sd_done++;
            break;
        }
    }

    /* Drain remaining USB sends after eMMC is done */
    while (usb_done < sd_done)
    {
        bool usb_tx_done = R8_USB_INT_FG & RB_USB_IF_TRANSFER;

        if (usb_tx_done)
            R8_UEP1_TX_CTRL = (R8_UEP1_TX_CTRL & ~RB_UEP_TRES_MASK) | UEP_T_RES_NAK;

        if (usb_tx_done || usb_first_packet) {
            xfer_count++;
            R8_USB_INT_FG = RB_USB_IF_TRANSFER;
            usb_first_packet = false;

            queue_usb_read_sector(&diag, usb_slot, usb_done);

            usb_done++;
            usb_slot = next_read_ring_slot(usb_slot);
        }

        if (R8_USB_INT_FG & RB_USB_IF_SETUOACT)
            R8_USB_INT_FG = RB_USB_IF_SETUOACT;
    }

    while (!(R8_USB_INT_FG & RB_USB_IF_TRANSFER));
    R8_USB_INT_FG = RB_USB_IF_TRANSFER;
    R8_UEP1_TX_CTRL = (R8_UEP1_TX_CTRL & ~RB_UEP_TRES_MASK) | UEP_T_RES_NAK;

    emmc_stop_multiblock_read();

#ifdef DEBUG_USB
    diag_log_read_summary(&diag, preqnum, xfer_count, usb_done);
#endif

    R8_UEP1_TX_CTRL &= ~RB_UEP_T_AUTOTOG;
    PFIC_EnableIRQ(USBHS_IRQn);
    R8_UEP0_TX_CTRL = uep0txsave;
    R8_UEP0_RX_CTRL = uep0rxsave;
}

void msc_read_sectors(void)
{
    uint16_t preqnum = g_bot.transfer_bytes_left / SECTOR_SIZE;
    g_bot.transfer_bytes_left = 0;

    uint32_t actual_lba = compute_physical_lba(g_bot.current_lba);

#ifdef DEBUG_USB
    cprintf("R lba=%lu n=%u\r\n", actual_lba, preqnum);
#endif

    read_stream_usb2(actual_lba, preqnum);

    g_bot.current_lba += preqnum;

    if (g_bot.transfer_bytes_left == 0)
        bot_send_csw();

    if (g_bot.transfer_bytes_left == 0x00)
        g_bot.transfer_flags &= ~BOT_FLAG_DATA_IN;
}
