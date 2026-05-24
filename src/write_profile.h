/* write_profile.h - Optional WRITE10 timing profiler
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WRITE_PROFILE_H_
#define WRITE_PROFILE_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef DEBUG

uint32_t write_profile_now(void);
void write_profile_reset(uint16_t sectors);
void write_profile_log(void);
void write_profile_count_chunk(void);
void write_profile_add_usb(uint32_t start);
void write_profile_add_snoop(uint32_t start);
void write_profile_add_crypt(uint32_t start);
void write_profile_add_sd(uint32_t start);
void write_profile_add_dat0(uint32_t start);
void write_profile_emmc_start(uint16_t sectors);
void write_profile_emmc_invalid_addr(void);
void write_profile_emmc_cmd25(uint32_t start, uint8_t status);
void write_profile_emmc_bkgap_wait(uint32_t start, uint32_t dat0_low_cycles);
void write_profile_emmc_trandone_wait(uint32_t start);
void write_profile_emmc_data(uint32_t start, bool failed);
void write_profile_emmc_cmd12(uint32_t start, uint8_t status);
void write_profile_emmc_cmd13(uint32_t start, uint8_t status, uint32_t response);
void write_profile_emmc_done(uint16_t sectors);

#else

static inline uint32_t write_profile_now(void) { return 0; }
static inline void write_profile_reset(uint16_t sectors) { (void)sectors; }
static inline void write_profile_log(void) {}
static inline void write_profile_count_chunk(void) {}
static inline void write_profile_add_usb(uint32_t start) { (void)start; }
static inline void write_profile_add_snoop(uint32_t start) { (void)start; }
static inline void write_profile_add_crypt(uint32_t start) { (void)start; }
static inline void write_profile_add_sd(uint32_t start) { (void)start; }
static inline void write_profile_add_dat0(uint32_t start) { (void)start; }
static inline void write_profile_emmc_start(uint16_t sectors) { (void)sectors; }
static inline void write_profile_emmc_invalid_addr(void) {}
static inline void write_profile_emmc_cmd25(uint32_t start, uint8_t status)
{
    (void)start;
    (void)status;
}
static inline void write_profile_emmc_bkgap_wait(uint32_t start, uint32_t dat0_low_cycles)
{
    (void)start;
    (void)dat0_low_cycles;
}
static inline void write_profile_emmc_trandone_wait(uint32_t start) { (void)start; }
static inline void write_profile_emmc_data(uint32_t start, bool failed)
{
    (void)start;
    (void)failed;
}
static inline void write_profile_emmc_cmd12(uint32_t start, uint8_t status)
{
    (void)start;
    (void)status;
}
static inline void write_profile_emmc_cmd13(uint32_t start, uint8_t status, uint32_t response)
{
    (void)start;
    (void)status;
    (void)response;
}
static inline void write_profile_emmc_done(uint16_t sectors) { (void)sectors; }

#endif

#endif /* WRITE_PROFILE_H_ */
