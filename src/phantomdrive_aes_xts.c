#include "phantomdrive.h"
#include "phantomdrive_crypto.h"
#include "bot_state.h"
#include "CH56x_ecdc.h"
#include "debug.h"

static uint32_t xts_data_key[8] __attribute__((aligned(16), section(".DMADATA")));
static uint32_t xts_tweak_key[8] __attribute__((aligned(16), section(".DMADATA")));

typedef struct {
	uint32_t w[4];
} xts_block_t;

#define XTS_MAX_BATCH_SECTORS (UDISK_BUF_SIZE / SECTOR_SIZE)

static xts_block_t xts_tweaks[XTS_MAX_BATCH_SECTORS] __attribute__((aligned(16), section(".DMADATA")));

void phantomdrive_crypto_unlock(const uint8_t *password, size_t pw_len,
                                uint8_t salt[KDF_SALT_SIZE])
{
	derive_key(password, pw_len, salt, (uint8_t *)xts_data_key);
	salt[0]++;
	derive_key(password, pw_len, salt, (uint8_t *)xts_tweak_key);
}

static void xts_mul_alpha(xts_block_t *tweak)
{
	uint32_t carry = tweak->w[3] >> 31;

	tweak->w[3] = (tweak->w[3] << 1) | (tweak->w[2] >> 31);
	tweak->w[2] = (tweak->w[2] << 1) | (tweak->w[1] >> 31);
	tweak->w[1] = (tweak->w[1] << 1) | (tweak->w[0] >> 31);
	tweak->w[0] <<= 1;
	if (carry)
		tweak->w[0] ^= 0x87;
}

static void xts_xor_sector(uint8_t *sector, const xts_block_t *initial_tweak)
{
	xts_block_t tweak = *initial_tweak;
	uint32_t *word = (uint32_t *)sector;

	for (size_t block = 0; block < SECTOR_SIZE / 16; block++) {
		word[0] ^= tweak.w[0];
		word[1] ^= tweak.w[1];
		word[2] ^= tweak.w[2];
		word[3] ^= tweak.w[3];
		word += 4;
		xts_mul_alpha(&tweak);
	}
}

static void xts_make_tweaks(uint32_t sd_lba, uint16_t num_sectors)
{
	xts_block_t tweak_input = {0};

	ECDC_Init(MODE_AES_ECB, ECDCCLK_240MHZ, KEYLENGTH_256BIT,
	          (puint32_t)xts_tweak_key, 0);
	ECDC_Excute(SINGLEREGISTER_ENCRY, MODE_LITTLE_ENDIAN);

	for (uint16_t i = 0; i < num_sectors; i++) {
		tweak_input.w[1] = sd_lba + i;
		ECDC_SingleRegister((puint32_t)tweak_input.w,
		                    (puint32_t)xts_tweaks[i].w);
	}
}

static void phantomdrive_xts_buf(uint8_t *buf, uint32_t sd_lba,
                                 uint16_t num_sectors, uint8_t ecdc_mode)
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

	while (num_sectors) {
		uint16_t batch = num_sectors;
		if (batch > XTS_MAX_BATCH_SECTORS)
			batch = XTS_MAX_BATCH_SECTORS;

		xts_make_tweaks(sd_lba, batch);

		for (uint16_t i = 0; i < batch; i++)
			xts_xor_sector(buf + i * SECTOR_SIZE, &xts_tweaks[i]);

		ECDC_Init(MODE_AES_ECB, ECDCCLK_240MHZ, KEYLENGTH_256BIT,
		          (puint32_t)xts_data_key, 0);
		ECDC_Excute(ecdc_mode, MODE_LITTLE_ENDIAN);
		ECDC_SelfDMA((uint32_t)buf, ((uint32_t)batch * SECTOR_SIZE) / 16);

		for (uint16_t i = 0; i < batch; i++)
			xts_xor_sector(buf + i * SECTOR_SIZE, &xts_tweaks[i]);

		buf += (uint32_t)batch * SECTOR_SIZE;
		sd_lba += batch;
		num_sectors -= batch;
	}

	/* Disable ECDC so subsequent eMMC DMA isn't double-encrypted */
	phantomdrive_ecdc_disable_data_path();

#ifdef DEBUG_USB
	if (clog < 5) {
		uint32_t post = *(volatile uint32_t*)orig_buf;
		log_printf("XTS lba=%lu n=%u %08lx->%08lx ctrl=%04x\r\n",
		           orig_lba, orig_num_sectors, pre, post, R16_ECEC_CTRL);
		clog++;
	}
#endif
}

void phantomdrive_encrypt_buf(uint8_t *buf, uint32_t sd_lba, uint16_t num_sectors)
{
	phantomdrive_xts_buf(buf, sd_lba, num_sectors, SELFDMA_ENCRY);
}

void phantomdrive_decrypt_buf(uint8_t *buf, uint32_t sd_lba, uint16_t num_sectors)
{
	phantomdrive_xts_buf(buf, sd_lba, num_sectors, SELFDMA_DECRY);
}
