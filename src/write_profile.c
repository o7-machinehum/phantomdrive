/* write_profile.c - Optional WRITE10 timing profiler
 * SPDX-License-Identifier: Apache-2.0
 */
#include "write_profile.h"

#ifdef DEBUG

#include "CH56x_common.h"
#include "CH56x_bsp.h"
#include "CH56x_debug_log.h"
#include "CH56x_emmc.h"
#include <string.h>

typedef struct {
    uint64_t usb_cycles;
    uint64_t snoop_cycles;
    uint64_t crypt_cycles;
    uint64_t sd_cycles;
    uint64_t dat0_cycles;
    uint32_t total_start;
    uint32_t chunks;
    uint16_t sectors;
} write_profile_t;

static write_profile_t write_prof;

uint32_t write_profile_now(void)
{
    return ~bsp_get_SysTickCNT_LSB();
}

static uint32_t write_profile_elapsed(uint32_t start)
{
    return write_profile_now() - start;
}

static void write_profile_add_cycles(uint64_t *sum, uint64_t *max, uint32_t cycles)
{
    *sum += cycles;
    if (cycles > *max)
        *max = cycles;
}

static uint32_t write_profile_us(uint64_t cycles)
{
    uint64_t cycles_per_us = bsp_get_nbtick_1us();
    if (cycles_per_us == 0)
        return 0;
    return (uint32_t)(cycles / cycles_per_us);
}

static uint32_t sd_r1_current_state(uint32_t r1)
{
    return (r1 >> 9) & 0x0f;
}

static uint32_t sd_r1_ready_for_data(uint32_t r1)
{
    return (r1 >> 8) & 0x01;
}

void write_profile_reset(uint16_t sectors)
{
    memset(&write_prof, 0, sizeof(write_prof));
    EMMCWriteProfileReset();
    write_prof.sectors = sectors;
    write_prof.total_start = write_profile_now();
}

void write_profile_log(void)
{
    uint32_t total_cycles = write_profile_elapsed(write_prof.total_start);

    cprintf("Wprof u2 sec=%u ch=%lu total=%luus usb=%luus snoop=%luus crypt=%luus sd=%luus dat0=%luus\r\n",
            write_prof.sectors,
            (unsigned long)write_prof.chunks,
            (unsigned long)write_profile_us(total_cycles),
            (unsigned long)write_profile_us(write_prof.usb_cycles),
            (unsigned long)write_profile_us(write_prof.snoop_cycles),
            (unsigned long)write_profile_us(write_prof.crypt_cycles),
            (unsigned long)write_profile_us(write_prof.sd_cycles),
            (unsigned long)write_profile_us(write_prof.dat0_cycles));

    cprintf("Eprof calls=%lu req=%lu done=%lu bkgap=%lu acmd23=%lu/%luus cmd23=%lu/%luus cmd24=%lu/%luus cmd25=%lu/%luus data=%lu/%luus cmd12=%lu/%luus err=%lu/%lu/%lu/%lu/%lu/%lu/%lu\r\n",
            (unsigned long)emmc_write_prof.calls,
            (unsigned long)emmc_write_prof.req_sectors,
            (unsigned long)emmc_write_prof.done_sectors,
            (unsigned long)emmc_write_prof.bkgaps,
            (unsigned long)write_profile_us(emmc_write_prof.acmd23_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.acmd23_max_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd23_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd23_max_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd24_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd24_max_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd25_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd25_max_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.data_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.data_max_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd12_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd12_max_cycles),
            (unsigned long)emmc_write_prof.invalid_addr_errors,
            (unsigned long)emmc_write_prof.acmd23_errors,
            (unsigned long)emmc_write_prof.cmd23_errors,
            (unsigned long)emmc_write_prof.cmd24_errors,
            (unsigned long)emmc_write_prof.cmd25_errors,
            (unsigned long)emmc_write_prof.data_errors,
            (unsigned long)emmc_write_prof.cmd12_errors);

    cprintf("Ewait bkgap=%lu/%luus d0low=%lu/%luus trdone=%lu/%luus\r\n",
            (unsigned long)write_profile_us(emmc_write_prof.bkgap_wait_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.bkgap_wait_max_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.bkgap_dat0_low_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.bkgap_max_dat0_low_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.trandone_wait_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.trandone_wait_max_cycles));

    cprintf("Ecard maxgap=%luus done=%lu if=0x%04lx tm=0x%08lx bc=0x%08lx trst=0x%08lx trif=0x%04lx cmd12=0x%08lx cmd13=%lu/%luus st=0x%08lx state=%lu ready=%lu err=%lu\r\n",
            (unsigned long)write_profile_us(emmc_write_prof.bkgap_wait_max_cycles),
            (unsigned long)(emmc_write_prof.bkgap_max_status & 0xffff),
            (unsigned long)emmc_write_prof.bkgap_max_int_fg,
            (unsigned long)emmc_write_prof.bkgap_max_tran_mode,
            (unsigned long)emmc_write_prof.bkgap_max_block_cfg,
            (unsigned long)emmc_write_prof.trandone_status,
            (unsigned long)emmc_write_prof.trandone_int_fg,
            (unsigned long)emmc_write_prof.cmd12_response,
            (unsigned long)write_profile_us(emmc_write_prof.cmd13_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd13_max_cycles),
            (unsigned long)emmc_write_prof.cmd13_response,
            (unsigned long)sd_r1_current_state(emmc_write_prof.cmd13_response),
            (unsigned long)sd_r1_ready_for_data(emmc_write_prof.cmd13_response),
            (unsigned long)emmc_write_prof.cmd13_errors);
}

void write_profile_count_chunk(void)
{
    write_prof.chunks++;
}

void write_profile_add_usb(uint32_t start)
{
    write_prof.usb_cycles += write_profile_elapsed(start);
}

void write_profile_add_snoop(uint32_t start)
{
    write_prof.snoop_cycles += write_profile_elapsed(start);
}

void write_profile_add_crypt(uint32_t start)
{
    write_prof.crypt_cycles += write_profile_elapsed(start);
}

void write_profile_add_sd(uint32_t start)
{
    write_prof.sd_cycles += write_profile_elapsed(start);
}

void write_profile_add_dat0(uint32_t start)
{
    write_prof.dat0_cycles += write_profile_elapsed(start);
}

void write_profile_emmc_start(uint16_t sectors)
{
    emmc_write_prof.calls++;
    emmc_write_prof.req_sectors += sectors;
}

void write_profile_emmc_invalid_addr(void)
{
    emmc_write_prof.invalid_addr_errors++;
}

void write_profile_emmc_cmd25(uint32_t start, uint8_t status)
{
    write_profile_add_cycles(&emmc_write_prof.cmd25_cycles,
                             &emmc_write_prof.cmd25_max_cycles,
                             write_profile_elapsed(start));
    if (status == CMD_FAILED)
        emmc_write_prof.cmd25_errors++;
}

void write_profile_emmc_bkgap_wait(uint32_t start, uint32_t dat0_low_cycles)
{
    uint32_t cycles = write_profile_elapsed(start);

    emmc_write_prof.bkgap_wait_cycles += cycles;
    emmc_write_prof.bkgap_dat0_low_cycles += dat0_low_cycles;
    if (cycles > emmc_write_prof.bkgap_wait_max_cycles) {
        emmc_write_prof.bkgap_wait_max_cycles = cycles;
        emmc_write_prof.bkgap_max_dat0_low_cycles = dat0_low_cycles;
        emmc_write_prof.bkgap_max_status = R32_EMMC_STATUS;
        emmc_write_prof.bkgap_max_int_fg = R16_EMMC_INT_FG;
        emmc_write_prof.bkgap_max_tran_mode = R32_EMMC_TRAN_MODE;
        emmc_write_prof.bkgap_max_block_cfg = R32_EMMC_BLOCK_CFG;
    }
    emmc_write_prof.bkgaps++;
}

void write_profile_emmc_trandone_wait(uint32_t start)
{
    write_profile_add_cycles(&emmc_write_prof.trandone_wait_cycles,
                             &emmc_write_prof.trandone_wait_max_cycles,
                             write_profile_elapsed(start));
    emmc_write_prof.trandone_status = R32_EMMC_STATUS;
    emmc_write_prof.trandone_int_fg = R16_EMMC_INT_FG;
}

void write_profile_emmc_data(uint32_t start, bool failed)
{
    write_profile_add_cycles(&emmc_write_prof.data_cycles,
                             &emmc_write_prof.data_max_cycles,
                             write_profile_elapsed(start));
    if (failed)
        emmc_write_prof.data_errors++;
}

void write_profile_emmc_cmd12(uint32_t start, uint8_t status)
{
    write_profile_add_cycles(&emmc_write_prof.cmd12_cycles,
                             &emmc_write_prof.cmd12_max_cycles,
                             write_profile_elapsed(start));
    emmc_write_prof.cmd12_response = R32_EMMC_RESPONSE3;
    if (status == CMD_FAILED)
        emmc_write_prof.cmd12_errors++;
}

void write_profile_emmc_cmd13(uint32_t start, uint8_t status, uint32_t response)
{
    write_profile_add_cycles(&emmc_write_prof.cmd13_cycles,
                             &emmc_write_prof.cmd13_max_cycles,
                             write_profile_elapsed(start));
    emmc_write_prof.cmd13_status = status;
    emmc_write_prof.cmd13_response = response;
    if (status == CMD_FAILED)
        emmc_write_prof.cmd13_errors++;
}

void write_profile_emmc_done(uint16_t sectors)
{
    emmc_write_prof.done_sectors += sectors;
}

#endif
