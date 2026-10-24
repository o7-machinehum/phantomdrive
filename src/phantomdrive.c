#include "phantomdrive.h"
#include "phantomdrive_crypto.h"
#include "unlock_file.h"
#include "bot_state.h"
#include "CH56x_ecdc.h"
#include "debug.h"
#include "CH56x_usb20_devbulk.h"
#include <stdbool.h>
#include <string.h>

#define UNLOCK_TIMEOUT_MS 30000

static volatile uint8_t phantomdrive_state = STATE_LOCKED;

static bool phantomdrive_unlock_pending = false;

static uint8_t pending_pw[UNLOCK_PASSWORD_MAX];
static size_t pending_pw_len;
static size_t unlock_pw_len;
static uint32_t pending_pw_lba;
static size_t pending_pw_offset;
static bool pw_partial;
static size_t prefix_match_len;
static uint32_t next_write_lba;
static uint64_t pending_pw_started_ms;

static uint8_t salt_bytes[KDF_SALT_SIZE] = {0};

static bool is_password_terminator(uint8_t c)
{
	return c == '\n' || c == '\r' || c == '\0';
}

static void clear_pending_password(void)
{
	memset(pending_pw, 0, sizeof(pending_pw));
	pending_pw_len = 0;
	unlock_pw_len = 0;
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
	clear_pending_password();
}

void phantomdrive_check_unlock_timeout(void)
{
	if (!phantomdrive_unlock_pending)
		return;

	uint64_t now = bsp_get_tick() / bsp_get_nbtick_1ms();
	if (now - pending_pw_started_ms >= UNLOCK_TIMEOUT_MS)
		phantomdrive_clear_unlock_pending();
}

bool phantomdrive_get_unlock_pending(void) {
	if (!phantomdrive_unlock_pending || pending_pw_len == 0)
		return false;

	unlock_pw_len = pending_pw_len;
	bool found = unlock_file_matches(pending_pw_lba, pending_pw_offset, &unlock_pw_len);
	phantomdrive_check_unlock_timeout();
	return found && phantomdrive_unlock_pending;
}

bool phantomdrive_is_locked(void) {
	return phantomdrive_state == STATE_LOCKED;
}

void phantomdrive_unlock(void)
{
	USB20_Device_Init(DISABLE);
	if (!unlock_file_delete()) {
		PFIC_EnableIRQ(USBHS_IRQn);
		USB20_Device_Init(ENABLE);
		return;
	}

	// We make a custom salt per-device
	phantomdrive_crypto_unlock(pending_pw, unlock_pw_len, salt_bytes);
	memset(pending_pw, 0, sizeof(pending_pw));
	pending_pw_len = 0;

	g_bot.capacity = TF_EMMCParam.EMMCSecNum - LOCKED_SECTORS;
	phantomdrive_state = STATE_UNLOCKED;

	log_printf("phantomdrive: unlocked, %lu sectors\r\n", g_bot.capacity);

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

static uint32_t continue_partial_password(uint8_t *buf, uint32_t len)
{
	size_t end = 0;
	bool saw_terminator = false;

	while (end < len) {
		uint8_t c = buf[end];

		if (is_password_terminator(c)) {
			saw_terminator = true;
			break;
		}
		if (pending_pw_len < sizeof(pending_pw))
			pending_pw[pending_pw_len] = c;
		/* Keep scrubbing after the limit; FatFs will check the logical EOF. */
		if (pending_pw_len <= sizeof(pending_pw))
			pending_pw_len++;
		end++;
	}

	memset(buf, 0, end);

	if (saw_terminator) {
		pw_partial = false;
		finish_password_snoop(pending_pw_len);
		return end + 1;
	}
	return end;
}

static void snoop_new_password(uint8_t *buf, uint32_t len, uint32_t start,
                              uint32_t lba)
{
	const char *prefix = "password:";
	const size_t prefix_len = sizeof("password:") - 1;
	uint32_t i;

	for (i = start; i < len; i++) {
		if (buf[i] == (uint8_t)prefix[prefix_match_len]) {
			prefix_match_len++;

			if (prefix_match_len < prefix_len)
				continue;
		} else {
			prefix_match_len = (buf[i] == (uint8_t)prefix[0]) ? 1 : 0;
			continue;
		}

		size_t pw_start = i + 1;
		size_t pw_end = pw_start;
		bool saw_terminator = false;

		clear_pending_password();
		pending_pw_started_ms = bsp_get_tick() / bsp_get_nbtick_1ms();
		pending_pw_lba = lba + i / SECTOR_SIZE;
		pending_pw_offset = i % SECTOR_SIZE + 1;

		while (pw_end < len) {
			uint8_t c = buf[pw_end];

			if (is_password_terminator(c)) {
				saw_terminator = true;
				break;
			}
			pw_end++;
		}

		size_t pw_len = pw_end - pw_start;
		size_t copy_len = pw_len;
		if (copy_len > sizeof(pending_pw))
			copy_len = sizeof(pending_pw);
		memcpy(pending_pw, buf + pw_start, copy_len);
		pending_pw_len = pw_len;
		if (pending_pw_len > sizeof(pending_pw))
			pending_pw_len = sizeof(pending_pw) + 1;
		memset(buf + pw_start, 0, pw_len);

		if (saw_terminator) {
			finish_password_snoop(pending_pw_len);
		} else {
			pw_partial = true;
			phantomdrive_set_unlock_pending();
		}
		i = pw_end;
	}
}

void phantomdrive_snoop_write(uint8_t *buf, uint32_t len, uint32_t lba)
{
	if (lba != next_write_lba) {
		pw_partial = false;
		prefix_match_len = 0;
	}
	next_write_lba = lba + len / SECTOR_SIZE;

	uint32_t start = 0;
	if (pw_partial)
		start = continue_partial_password(buf, len);

	snoop_new_password(buf, len, start, lba);
}

void phantomdrive_ecdc_disable_data_path(void)
{
	R16_ECEC_CTRL &= ~(RB_ECDC_WRSRAM_EN | RB_ECDC_WRPERI_EN |
	                    RB_ECDC_RDPERI_EN | RB_ECDC_MODE_SEL);
}
