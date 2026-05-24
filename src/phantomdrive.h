#ifndef PHANTOMDRIVE_H_
#define PHANTOMDRIVE_H_

#include <stdint.h>
#include <stddef.h>

#define STATE_LOCKED    0
#define STATE_UNLOCKED  1
#define LOCKED_SECTORS  (16777216UL)  /* 8GB in sectors */

extern volatile uint8_t phantomdrive_state;
extern uint32_t aes_key[8] __attribute__((section(".DMADATA")));

void phantomdrive_init(void);
void phantomdrive_snoop_write(uint8_t *buf, uint32_t len);
void phantomdrive_poll(void);
void phantomdrive_crypt_buf(uint8_t *buf, uint32_t sd_lba, uint16_t num_sectors);
void phantomdrive_ecdc_disable_data_path(void);

#endif /* PHANTOMDRIVE_H_ */
