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

/*
  READ10 ring buffer: UDisk_In_Buf, 80 slots x 512 bytes

            USB sends older ready slot
                      |
                      v
  +--------+--------+--------+-------------+-------------+
  | slot 0 | slot 1 | slot 2 | slot 3      | slot 4      |
  | sent   | USB TX | ready  | decrypt now | eMMC fills  | ...
  +--------+--------+--------+-------------+-------------+
             ^        ^          ^              ^
             |        |          |              |
          usb_slot    |      completed_slot   emmc_slot
                      |
               decrypted and waiting
*/

typedef struct {
    uint16_t emmc_done; // Count of how many sectors eMMC has finished reading into RAM
    uint16_t usb_done; // Count of how many sectors have been handled to USB for TX
    uint8_t emmc_slot; // During a read, eMMC writes SD-Card data to... UDisk_In_Buf + emmc_slot * 512
    uint8_t usb_slot; // During a read, USB reads data from... UDisk_In_Buf + usb_slot + emmc_slot * 512
    bool emmc_paused_for_usb_buffer;
    bool usb_first_packet;
} read_pipe_t;

static inline uint8_t next_read_ring_slot(uint8_t slot)
{
    slot++;
    if (slot == READ_RING_SECTORS)
        slot = 0;
    return slot;
}

static inline uint8_t *read_ring_slot(uint8_t slot)
{
    return UDisk_In_Buf + slot * SECTOR_SIZE;
}

static inline bool read_pipe_has_safe_usb_sector(const read_pipe_t *pipe)
{
    return pipe->emmc_done > 1 && pipe->usb_done < pipe->emmc_done - 1;
}

static inline bool read_pipe_has_space_for_emmc(const read_pipe_t *pipe)
{
    return (pipe->emmc_done - pipe->usb_done) < (READ_RING_SECTORS - 2);
}

static void queue_usb_read_sector(read_diag_t *diag, uint8_t send_slot,
                                  uint16_t usb_done)
{
    uint8_t *sector = read_ring_slot(send_slot);

    diag_check_usb_sector(diag, sector, usb_done);

    R32_UEP1_TX_DMA = (uint32_t)(uint8_t *)sector;
    R16_UEP1_T_LEN = SECTOR_SIZE;
    __asm__ volatile("fence" ::: "memory");
    R8_UEP1_TX_CTRL = (R8_UEP1_TX_CTRL & ~RB_UEP_TRES_MASK) | UEP_T_RES_ACK;
}

static void read_stream_usb2(uint32_t actual_lba, uint16_t preqnum)
{
    bool decrypt = !phantomdrive_is_locked();
    read_pipe_t pipe = {
        .emmc_done = 0,
        .usb_done = 0,
        .emmc_slot = 0,
        .usb_slot = 0,
        .emmc_paused_for_usb_buffer = false,
        .usb_first_packet = true,
    };
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
        if (read_pipe_has_safe_usb_sector(&pipe) &&
            (usb_tx_done || pipe.usb_first_packet)) {
            uint8_t send_slot = pipe.usb_slot;

            if (usb_tx_done)
                xfer_count++;

            // Clear USB transfer complete interrupt flag
            R8_USB_INT_FG = RB_USB_IF_TRANSFER;

            pipe.usb_first_packet = false;

            // We can send this slot out
            queue_usb_read_sector(&diag, send_slot, pipe.usb_done);

            pipe.usb_done++;
            pipe.usb_slot = next_read_ring_slot(pipe.usb_slot);
            if (pipe.emmc_paused_for_usb_buffer) {
                pipe.emmc_paused_for_usb_buffer = false;
                emmc_release_gap_stop();
            }
        }

        if (R8_USB_INT_FG & RB_USB_IF_SETUOACT)
            R8_USB_INT_FG = RB_USB_IF_SETUOACT;

        if (R16_EMMC_INT_FG & RB_EMMC_IF_BKGAP) {
            uint8_t completed_slot = pipe.emmc_slot;
            uint8_t *completed_sector = read_ring_slot(completed_slot);

            if (decrypt)
                phantomdrive_crypt_buf(completed_sector, actual_lba + pipe.emmc_done, 1);

            diag_check_sd_sector(&diag, completed_sector, pipe.emmc_done);

            R16_EMMC_INT_FG = RB_EMMC_IF_BKGAP;
            pipe.emmc_done++;
            pipe.emmc_slot = next_read_ring_slot(pipe.emmc_slot);
            R32_EMMC_DMA_BEG1 = (uint32_t)(uint8_t *)read_ring_slot(pipe.emmc_slot);

            if (read_pipe_has_space_for_emmc(&pipe))
                emmc_release_gap_stop();
            else
                pipe.emmc_paused_for_usb_buffer = true;
        }
        else if (R16_EMMC_INT_FG & RB_EMMC_IF_TRANDONE) {
            uint8_t completed_slot = pipe.emmc_slot;
            uint8_t *completed_sector = read_ring_slot(completed_slot);

            if (decrypt)
                phantomdrive_crypt_buf(completed_sector, actual_lba + pipe.emmc_done, 1);

            diag_check_sd_sector(&diag, completed_sector, pipe.emmc_done);

            R16_EMMC_INT_FG = RB_EMMC_IF_TRANDONE | RB_EMMC_IF_CMDDONE;
            pipe.emmc_done++;
            break;
        }
    }

    /* Drain remaining USB sends after eMMC is done */
    while (pipe.usb_done < pipe.emmc_done)
    {
        bool usb_tx_done = R8_USB_INT_FG & RB_USB_IF_TRANSFER;

        if (usb_tx_done)
            R8_UEP1_TX_CTRL = (R8_UEP1_TX_CTRL & ~RB_UEP_TRES_MASK) | UEP_T_RES_NAK;

        if (usb_tx_done || pipe.usb_first_packet) {
            uint8_t send_slot = pipe.usb_slot;

            xfer_count++;
            R8_USB_INT_FG = RB_USB_IF_TRANSFER;
            pipe.usb_first_packet = false;

            queue_usb_read_sector(&diag, send_slot, pipe.usb_done);

            pipe.usb_done++;
            pipe.usb_slot = next_read_ring_slot(pipe.usb_slot);
        }

        if (R8_USB_INT_FG & RB_USB_IF_SETUOACT)
            R8_USB_INT_FG = RB_USB_IF_SETUOACT;
    }

    while (!(R8_USB_INT_FG & RB_USB_IF_TRANSFER));
    R8_USB_INT_FG = RB_USB_IF_TRANSFER;
    R8_UEP1_TX_CTRL = (R8_UEP1_TX_CTRL & ~RB_UEP_TRES_MASK) | UEP_T_RES_NAK;

    emmc_stop_multiblock_read();

#ifdef DEBUG_USB
    diag_log_read_summary(&diag, preqnum, xfer_count, pipe.usb_done);
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
