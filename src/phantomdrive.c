#include "phantomdrive.h"
#include "crypto.h"
#include "bot_state.h"
#include "CH56x_ecdc.h"
#include "debug.h"
#include "CH56x_usb20_devbulk.h"
#include <stdbool.h>
#include <string.h>

static volatile uint8_t phantomdrive_state = STATE_LOCKED;

__attribute__((aligned(16))) uint32_t aes_key[8] __attribute__((section(".DMADATA")));

static bool phantomdrive_unlock_pending = false;

static uint8_t pending_pw[128];
static size_t pending_pw_len;
static bool pw_partial;
static size_t prefix_match_len;

static uint8_t salt_bytes[KDF_SALT_SIZE] = {0};

static bool is_password_terminator(uint8_t c)
{
	return c == '\n' || c == '\r' || c == '\0';
}

static void clear_pending_password(void)
{
	memset(pending_pw, 0, sizeof(pending_pw));
	pending_pw_len = 0;
	pw_partial = false;
	prefix_match_len = 0;
}

void phantomdrive_init(uint64_t unique_id)
{
	for (size_t i = 0; i < KDF_SALT_SIZE; i++) {
		salt_bytes[i] = (uint8_t)(unique_id >> (8 * i));
	}

	log_printf("Salt:");
	for (size_t i = 0; i < KDF_SALT_SIZE; i++) {
		cprintf("%02x", salt_bytes[i]);
	}
	cprintf("\r\n");


	R16_ECEC_CTRL = 0;
	R8_ECDC_INT_FG = 0xFF;

	g_bot.capacity = LOCKED_SECTORS;
	phantomdrive_state = STATE_LOCKED;
	log_printf("phantomdrive: locked, %lu sectors\r\n", LOCKED_SECTORS);
}

void phantomdrive_set_unlock_pending(void) {
	phantomdrive_unlock_pending = true;
}

void phantomdrive_clear_unlock_pending(void) {
	phantomdrive_unlock_pending = false;
}

bool phantomdrive_get_unlock_pending(void) {
	return phantomdrive_unlock_pending;
}

bool phantomdrive_is_locked(void) {
	return phantomdrive_state == STATE_LOCKED;
}

void phantomdrive_unlock(void)
{
	log_printf("phantomdrive: deriving key (%u bytes)...\r\n", (unsigned)pending_pw_len);

	uint8_t key_bytes[32];

	// We make a custom salt per-device
	derive_key(pending_pw, pending_pw_len, salt_bytes, key_bytes);

	/* Only for verification/testing purposes
	#ifdef DEBUG
  	log_printf("Key:");
  	for (size_t i = 0; i < sizeof(key_bytes); i++)
  	    cprintf("%02x", key_bytes[i]);
  	cprintf("\r\n");
  	#endif
	*/

	memcpy(aes_key, key_bytes, 32);
	memset(key_bytes, 0, sizeof(key_bytes));
	memset(pending_pw, 0, sizeof(pending_pw));
	pending_pw_len = 0;

	uint32_t initial_ctr[4] = {0, 0, 0, 0};
	ECDC_Init(MODE_AES_CTR, ECDCCLK_240MHZ, KEYLENGTH_256BIT,
	          (puint32_t)aes_key, (puint32_t)initial_ctr);

	g_bot.capacity = TF_EMMCParam.EMMCSecNum - LOCKED_SECTORS;
	phantomdrive_state = STATE_UNLOCKED;

	log_printf("phantomdrive: unlocked, %lu sectors\r\n", g_bot.capacity);

	g_bot.transfer_flags = 0;
	g_bot.read_pending = false;
	g_bot.write_pending = false;
	USB20_Device_Init(DISABLE);
	PFIC_EnableIRQ(USBHS_IRQn);
	USB20_Device_Init(ENABLE);

	log_printf("phantomdrive: re-enumerated\r\n");
	phantomdrive_clear_unlock_pending();
}

static void finish_password_snoop(size_t pw_len)
{
	if (pw_len == 0) {
		clear_pending_password();
		return;
	}

	phantomdrive_set_unlock_pending();
	pw_partial = false;
	prefix_match_len = 0;
	log_printf("phantomdrive: password snooped (%u bytes)\r\n",
	           (unsigned)pw_len);
}

static void continue_partial_password(uint8_t *buf, uint32_t len)
{
	size_t end = 0;
	bool saw_terminator = false;

	while (end < len) {
		uint8_t c = buf[end];

		if (is_password_terminator(c)) {
			saw_terminator = true;
			break;
		}
		if (pending_pw_len >= sizeof(pending_pw))
			break;

		pending_pw[pending_pw_len++] = c;
		end++;
	}

	memset(buf, 0, end);

	if (saw_terminator) {
		pw_partial = false;
		finish_password_snoop(pending_pw_len);
	} else if (pending_pw_len >= sizeof(pending_pw)) {
		// We've used up our entire buffer, password too long
		clear_pending_password();
	}
}

static void snoop_new_password(uint8_t *buf, uint32_t len)
{
	const char *prefix = "password:";
	const size_t prefix_len = sizeof("password:") - 1;
	size_t prefix_start = 0;
	uint32_t i;

	for (i = 0; i < len; i++) {
		if (buf[i] == (uint8_t)prefix[prefix_match_len]) {
			if (prefix_match_len == 0)
				prefix_start = i;

			prefix_match_len++;

			if (prefix_match_len < prefix_len)
				continue;
		} else {
			prefix_match_len = (buf[i] == (uint8_t)prefix[0]) ? 1 : 0;
			prefix_start = i;
			continue;
		}

		size_t pw_start = i + 1;
		size_t pw_end = pw_start;
		bool saw_terminator = false;

		prefix_match_len = 0;

		while (pw_end < len) {
			uint8_t c = buf[pw_end];

			if (is_password_terminator(c)) {
				saw_terminator = true;
				break;
			}
			if ((pw_end - pw_start) >= sizeof(pending_pw))
				break;

			pw_end++;
		}

		size_t pw_len = pw_end - pw_start;
		memcpy(pending_pw, buf + pw_start, pw_len);
		pending_pw_len = pw_len;
		memset(buf + prefix_start, 0, pw_end - prefix_start);

		if (saw_terminator) {
			finish_password_snoop(pw_len);
		} else if (pw_len >= sizeof(pending_pw)) {
			// We've used up our entire buffer, password too long
			clear_pending_password();
		} else {
			pw_partial = true;
		}
		return;
	}
}

void phantomdrive_snoop_write(uint8_t *buf, uint32_t len)
{
	if (pw_partial) {
		continue_partial_password(buf, len);
		return;
	}

	snoop_new_password(buf, len);
}

void phantomdrive_ecdc_set_sector_nonce(uint32_t sd_lba)
{
	uint32_t ctr[4];
	ctr[0] = 0;
	ctr[1] = sd_lba;
	ctr[2] = 0;
	ctr[3] = 0;
	ECDC_SetCount((puint32_t)ctr);
}

void phantomdrive_ecdc_disable_data_path(void)
{
	R16_ECEC_CTRL &= ~(RB_ECDC_WRSRAM_EN | RB_ECDC_WRPERI_EN |
	                    RB_ECDC_RDPERI_EN | RB_ECDC_MODE_SEL);
}

/*
            LBA 0                  LBA 1                  LBA 2
     +----------------+     +----------------+     +----------------+
     | 512 bytes      |     | 512 bytes      |     | 512 bytes      |
     | 32 AES blocks  |     | 32 AES blocks  |     | 32 AES blocks  |
     +----------------+     +----------------+     +----------------+

     LBA 0
     +---------+---------+---------+-----+----------+
     | ctr 0   | ctr 1   | ctr 2   | ... | ctr 31   |
     +---------+---------+---------+-----+----------+

	 - nonce = (sd_lba + ctr)
	 - ctr is added in hardware. The start is loaded with phantomdrive_ecdc_set_sector_nonce()
*/

void phantomdrive_crypt_buf(uint8_t *buf, uint32_t sd_lba, uint16_t num_sectors)
{
	static uint8_t clog = 0;
	uint32_t pre = *(volatile uint32_t*)buf;

	uint16_t i;
	for (i = 0; i < num_sectors; i++) {
		phantomdrive_ecdc_set_sector_nonce(sd_lba + i);
		ECDC_Excute(SELFDMA_ENCRY, MODE_LITTLE_ENDIAN);
		ECDC_SelfDMA((uint32_t)(buf + i * SECTOR_SIZE), SECTOR_SIZE / 16);
	}
	/* Disable ECDC so subsequent eMMC DMA isn't double-encrypted */
	phantomdrive_ecdc_disable_data_path();

#ifdef DEBUG_USB
	if (clog < 5) {
		uint32_t post = *(volatile uint32_t*)buf;
		log_printf("C lba=%lu n=%u %08lx->%08lx ctrl=%04x\r\n",
		           sd_lba, num_sectors, pre, post, R16_ECEC_CTRL);
		clog++;
	}
#endif
}
