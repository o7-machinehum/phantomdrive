#include "phantomdrive.h"
#include "phantomdrive_crypto.h"
#include "bot_state.h"
#include "CH56x_ecdc.h"
#include "debug.h"
#include "CH56x_usb20_devbulk.h"
#include <stdbool.h>
#include <string.h>

static volatile uint8_t phantomdrive_state = STATE_LOCKED;

static bool phantomdrive_unlock_pending = false;

static uint8_t pending_pw[128];
static size_t pending_pw_len;
static bool pw_partial;
static size_t prefix_match_len;

static uint8_t salt_bytes[KDF_SALT_SIZE] = {0};
static uint8_t device_id[16];
static bool provisioned_device;

#define UNLOCK_PHRASE_SIZE 16
#define METADATA_WRITE_TIMEOUT_MS 2000

static __attribute__((aligned(16))) uint8_t metadata_buf[SECTOR_SIZE]
	__attribute__((section(".DMADATA")));
static __attribute__((aligned(16))) uint8_t metadata_readback[SECTOR_SIZE]
	__attribute__((section(".DMADATA")));

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

static bool read_metadata_sector(uint8_t *buf, uint32_t lba)
{
	phantomdrive_ecdc_disable_data_path();
	R16_EMMC_INT_FG = 0xffff;
	TF_EMMCParam.EMMCOpErr = 0;
	TF_EMMCParam.EMMCSecSize = SECTOR_SIZE;
	return EMMCCardReadOneSec(&TF_EMMCParam, buf, lba) == OP_SUCCESS;
}

static bool write_metadata_sector(uint32_t lba)
{
	uint16_t sectors = 1;

	phantomdrive_ecdc_disable_data_path();
	R16_EMMC_INT_FG = 0xffff;
	TF_EMMCParam.EMMCOpErr = 0;
	TF_EMMCParam.EMMCSecSize = SECTOR_SIZE;
	if (EMMCCardWriteOneSec(&TF_EMMCParam, &sectors, metadata_buf, lba) != OP_SUCCESS)
		return false;

	/* The card may still be programming after the DMA transfer finishes. */
	uint64_t start = bsp_get_tick();
	uint64_t timeout_ticks = (uint64_t)bsp_get_nbtick_1ms() * METADATA_WRITE_TIMEOUT_MS;
	volatile uint8_t *io_error = &TF_EMMCParam.EMMCOpErr; /* updated by the ISR */
	while (!EMMCDat0Sta) {
		if (*io_error || bsp_get_tick() - start >= timeout_ticks)
			return false;
	}
	if (*io_error)
		return false;

	if (!read_metadata_sector(metadata_readback, lba))
		return false;
	return memcmp(metadata_buf, metadata_readback, SECTOR_SIZE) == 0;
}

static bool load_metadata(void)
{
	uint32_t m1_lba = TF_EMMCParam.EMMCSecNum - PHANTOMDRIVE_METADATA_SECTORS;

	if (!read_metadata_sector(metadata_buf, m1_lba)) {
		log_printf("phantomdrive: cannot read M1\r\n");
		return false;
	}

	provisioned_device = memcmp(metadata_buf + UNLOCK_PHRASE_SIZE,
	                            device_id, sizeof(device_id)) == 0;
	return true;
}

static bool check_password(void)
{
	uint32_t m1_lba = TF_EMMCParam.EMMCSecNum - PHANTOMDRIVE_METADATA_SECTORS;
	uint32_t m2_lba = TF_EMMCParam.EMMCSecNum - 1;

	/* Read M1 again: an earlier provisioning write may have succeeded even
	 * if its readback failed. Never overwrite that password on a retry. */
	if (!load_metadata())
		return false;
	phantomdrive_crypto_unlock(pending_pw, pending_pw_len, salt_bytes);

	if (provisioned_device) {
		if (!read_metadata_sector(metadata_buf, m2_lba))
			return false;
		phantomdrive_decrypt_buf(metadata_buf, m2_lba, 1);

		/* Check all 16 bytes without stopping at the first mismatch. */
		uint8_t difference = 0;
		for (size_t i = 0; i < sizeof(device_id); i++)
			difference |= metadata_buf[i] ^ device_id[i];
		return difference == 0;
	}

	memset(metadata_buf, 0, sizeof(metadata_buf));
	memcpy(metadata_buf, device_id, sizeof(device_id));
	phantomdrive_encrypt_buf(metadata_buf, m2_lba, 1);
	if (!write_metadata_sector(m2_lba))
		return false;

	/* Commit the provisioning marker only after M2 has been written and read
	 * back successfully. Neither metadata sector is exposed over USB. */
	memset(metadata_buf, 0, sizeof(metadata_buf));
	/* P1 is reserved for custom phrases; snooping still uses "password:". */
	memcpy(metadata_buf, "password", sizeof("password") - 1);
	memcpy(metadata_buf + UNLOCK_PHRASE_SIZE, device_id, sizeof(device_id));
	if (!write_metadata_sector(m1_lba))
		return false;
	provisioned_device = true;
	return true;
}

bool phantomdrive_init(uint64_t unique_id)
{
	clear_pending_password();
	phantomdrive_unlock_pending = false;
	phantomdrive_state = STATE_LOCKED;
	g_bot.capacity = LOCKED_SECTORS;
	memset(device_id, 0, sizeof(device_id));
	for (size_t i = 0; i < KDF_SALT_SIZE; i++) {
		salt_bytes[i] = (uint8_t)(unique_id >> (8 * i));
	}
	memcpy(device_id, salt_bytes, KDF_SALT_SIZE);

	log_printf("Salt:");
	for (size_t i = 0; i < KDF_SALT_SIZE; i++) {
		cprintf("%02x", salt_bytes[i]);
	}
	cprintf("\r\n");


	R16_ECEC_CTRL = 0;
	R8_ECDC_INT_FG = 0xFF;
	if (TF_EMMCParam.EMMCSecNum <= LOCKED_SECTORS ||
	    TF_EMMCParam.EMMCSecNum - LOCKED_SECTORS <= PHANTOMDRIVE_METADATA_SECTORS) {
		log_printf("phantomdrive: card too small for volumes and metadata\r\n");
		return false;
	}
	if (!load_metadata())
		return false;
	log_printf("phantomdrive: locked, %lu sectors\r\n", LOCKED_SECTORS);
	return true;
}

void phantomdrive_set_unlock_pending(void) {
	phantomdrive_unlock_pending = true;
}

void phantomdrive_clear_unlock_pending(void) {
	phantomdrive_unlock_pending = false;
}

bool phantomdrive_get_unlock_pending(void) {
	return phantomdrive_unlock_pending && !pw_partial && pending_pw_len != 0;
}

bool phantomdrive_is_locked(void) {
	return phantomdrive_state == STATE_LOCKED;
}

void phantomdrive_unlock(void)
{
	USB20_Device_Init(DISABLE);
	bool accepted = check_password();
	clear_pending_password();

	if (accepted) {
		g_bot.capacity = TF_EMMCParam.EMMCSecNum - LOCKED_SECTORS
		                 - PHANTOMDRIVE_METADATA_SECTORS;
		phantomdrive_state = STATE_UNLOCKED;
		log_printf("phantomdrive: unlocked, %lu sectors\r\n", g_bot.capacity);
	} else {
		log_printf("phantomdrive: password or metadata check failed; staying locked\r\n");
	}

	g_bot.transfer_flags = 0;
	g_bot.read_pending = false;
	g_bot.write_pending = false;
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

void phantomdrive_ecdc_disable_data_path(void)
{
	R16_ECEC_CTRL &= ~(RB_ECDC_WRSRAM_EN | RB_ECDC_WRPERI_EN |
	                    RB_ECDC_RDPERI_EN | RB_ECDC_MODE_SEL);
}
