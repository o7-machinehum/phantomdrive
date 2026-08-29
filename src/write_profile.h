/* write_profile.h - Optional WRITE10 timing profiler
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WRITE_PROFILE_H_
#define WRITE_PROFILE_H_

#include <stdint.h>

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

#endif

#endif /* WRITE_PROFILE_H_ */
