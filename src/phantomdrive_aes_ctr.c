#include "phantomdrive.h"
#include "phantomdrive_crypto.h"
#include "bot_state.h"
#include "CH56x_ecdc.h"
#include "debug.h"

static uint32_t aes_key[8] __attribute__((aligned(16), section(".DMADATA")));

void phantomdrive_crypto_unlock(const uint8_t *password, size_t pw_len,
                                uint8_t salt[KDF_SALT_SIZE])
{
	uint32_t initial_ctr[4] = {0, 0, 0, 0};

	derive_key(password, pw_len, salt, (uint8_t *)aes_key);
	ECDC_Init(MODE_AES_CTR, ECDCCLK_240MHZ, KEYLENGTH_256BIT,
	          (puint32_t)aes_key, (puint32_t)initial_ctr);
}

static void phantomdrive_ecdc_set_sector_nonce(uint32_t sd_lba)
{
	uint32_t ctr[4];
	ctr[0] = 0;
	ctr[1] = sd_lba;
	ctr[2] = 0;
	ctr[3] = 0;
	ECDC_SetCount((puint32_t)ctr);
}

static void phantomdrive_ctr_buf(uint8_t *buf, uint32_t sd_lba, uint16_t num_sectors)
{
#ifdef DEBUG_USB
	static uint8_t clog = 0;
	uint8_t *orig_buf = buf;
	uint32_t orig_lba = sd_lba;
	uint16_t orig_num_sectors = num_sectors;
	uint32_t pre;
#endif

	if (num_sectors == 0)
		return;

#ifdef DEBUG_USB
	pre = *(volatile uint32_t*)buf;
#endif

	for (uint16_t i = 0; i < num_sectors; i++) {
		phantomdrive_ecdc_set_sector_nonce(sd_lba + i);
		ECDC_Excute(SELFDMA_ENCRY, MODE_LITTLE_ENDIAN);
		ECDC_SelfDMA((uint32_t)(buf + i * SECTOR_SIZE), SECTOR_SIZE / 16);
	}

	/* Disable ECDC so subsequent eMMC DMA isn't double-encrypted */
	phantomdrive_ecdc_disable_data_path();

#ifdef DEBUG_USB
	if (clog < 5) {
		uint32_t post = *(volatile uint32_t*)orig_buf;
		log_printf("CTR lba=%lu n=%u %08lx->%08lx ctrl=%04x\r\n",
		           orig_lba, orig_num_sectors, pre, post, R16_ECEC_CTRL);
		clog++;
	}
#endif
}

void phantomdrive_encrypt_buf(uint8_t *buf, uint32_t sd_lba, uint16_t num_sectors)
{
	phantomdrive_ctr_buf(buf, sd_lba, num_sectors);
}

void phantomdrive_decrypt_buf(uint8_t *buf, uint32_t sd_lba, uint16_t num_sectors)
{
	phantomdrive_ctr_buf(buf, sd_lba, num_sectors);
}
