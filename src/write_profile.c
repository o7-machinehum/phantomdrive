/* write_profile.c - Optional WRITE10 timing profiler
 * SPDX-License-Identifier: Apache-2.0
 */
#include "write_profile.h"

#ifdef DEBUG

#include "CH56x_common.h"
#include "CH56x_bsp.h"
#include "debug.h"
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

static uint32_t write_profile_us(uint64_t cycles)
{
    uint64_t cycles_per_us = bsp_get_nbtick_1us();
    if (cycles_per_us == 0)
        return 0;
    return (uint32_t)(cycles / cycles_per_us);
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

    cprintf("Eprof calls=%lu req=%lu done=%lu bkgap=%lu acmd23=%lu/%luus cmd25=%lu/%luus data=%lu/%luus cmd12=%lu/%luus err=%lu/%lu/%lu/%lu/%lu\r\n",
            (unsigned long)emmc_write_prof.calls,
            (unsigned long)emmc_write_prof.req_sectors,
            (unsigned long)emmc_write_prof.done_sectors,
            (unsigned long)emmc_write_prof.bkgaps,
            (unsigned long)write_profile_us(emmc_write_prof.acmd23_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.acmd23_max_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd25_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd25_max_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.data_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.data_max_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd12_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.cmd12_max_cycles),
            (unsigned long)emmc_write_prof.invalid_addr_errors,
            (unsigned long)emmc_write_prof.acmd23_errors,
            (unsigned long)emmc_write_prof.cmd25_errors,
            (unsigned long)emmc_write_prof.data_errors,
            (unsigned long)emmc_write_prof.cmd12_errors);

    cprintf("Ewait bkgap=%lu/%luus trdone=%lu/%luus\r\n",
            (unsigned long)write_profile_us(emmc_write_prof.bkgap_wait_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.bkgap_wait_max_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.trandone_wait_cycles),
            (unsigned long)write_profile_us(emmc_write_prof.trandone_wait_max_cycles));
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

#endif
