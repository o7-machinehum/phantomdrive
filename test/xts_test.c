#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <openssl/aes.h>
#include <openssl/evp.h>

#include "../src/crypto.h"
#include "../src/phantomdrive.h"

#define KEY_SIZE 32
#define SECTOR_SIZE 512

typedef void (*aes_block_crypt_fn)(const unsigned char *, unsigned char *,
                                   const AES_KEY *);

static void u32be(uint8_t *p, uint32_t x)
{
	p[0] = x >> 24;
	p[1] = x >> 16;
	p[2] = x >> 8;
	p[3] = x;
}

static void ecdc_key_to_aes_key(uint8_t aes_key[32], const uint8_t ecdc_key[32])
{
	for (int i = 0; i < 32; i++)
		aes_key[i] = ecdc_key[31 - i];
}

static void make_tweak_input(uint8_t tweak_input[AES_BLOCK_SIZE], uint32_t lba)
{
	memset(tweak_input, 0, AES_BLOCK_SIZE);
	u32be(tweak_input + 8, lba);
}

static void xts_mul_alpha(uint8_t tweak[AES_BLOCK_SIZE])
{
	uint8_t carry = 0;

	for (int i = 0; i < AES_BLOCK_SIZE; i++) {
		uint8_t next_carry = tweak[i] >> 7;
		tweak[i] = (uint8_t)((tweak[i] << 1) | carry);
		carry = next_carry;
	}

	if (carry)
		tweak[0] ^= 0x87;
}

static void xts_xor_block(uint8_t *block, const uint8_t tweak[AES_BLOCK_SIZE])
{
	for (int i = 0; i < AES_BLOCK_SIZE; i++)
		block[i] ^= tweak[i];
}

static void xts_make_tweak(uint8_t tweak[AES_BLOCK_SIZE], uint32_t lba,
                           AES_KEY *tweak_key)
{
	uint8_t tweak_input[AES_BLOCK_SIZE];

	make_tweak_input(tweak_input, lba);
	AES_encrypt(tweak_input, tweak, tweak_key);
}

static void xts_crypt_sector(uint8_t sector[SECTOR_SIZE], uint32_t lba,
                             AES_KEY *data_key, AES_KEY *tweak_key,
                             aes_block_crypt_fn crypt)
{
	uint8_t tweak[AES_BLOCK_SIZE];
	uint8_t block[AES_BLOCK_SIZE];

	xts_make_tweak(tweak, lba, tweak_key);

	for (int off = 0; off < SECTOR_SIZE; off += AES_BLOCK_SIZE) {
		xts_xor_block(sector + off, tweak);
		crypt(sector + off, block, data_key);
		memcpy(sector + off, block, AES_BLOCK_SIZE);
		xts_xor_block(sector + off, tweak);
		xts_mul_alpha(tweak);
	}
}

static bool evp_xts_encrypt(uint8_t out[SECTOR_SIZE], const uint8_t in[SECTOR_SIZE],
                            const uint8_t data_key[32], const uint8_t tweak_key[32],
                            uint32_t lba)
{
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	uint8_t xts_key[KEY_SIZE * 2];
	uint8_t tweak_input[AES_BLOCK_SIZE];
	int out_len = 0;
	int final_len = 0;
	bool ok = false;

	if (!ctx)
		return false;

	memcpy(xts_key, data_key, 32);
	memcpy(xts_key + 32, tweak_key, 32);
	make_tweak_input(tweak_input, lba);

	if (EVP_CipherInit_ex(ctx, EVP_aes_256_xts(), NULL, xts_key, tweak_input, 1) != 1)
		goto out;
	EVP_CIPHER_CTX_set_padding(ctx, 0);
	if (EVP_CipherUpdate(ctx, out, &out_len, in, SECTOR_SIZE) != 1)
		goto out;
	if (EVP_CipherFinal_ex(ctx, out + out_len, &final_len) != 1)
		goto out;

	ok = (out_len + final_len) == SECTOR_SIZE;

out:
	EVP_CIPHER_CTX_free(ctx);
	return ok;
}

int main(void)
{
	const uint8_t password[] = "pineapple";
	uint8_t salt[KDF_SALT_SIZE] = {0x34, 0xfc, 0x1f, 0xa7, 0x14, 0x54, 0x67, 0xf7};
	uint8_t ecdc_data_key[KEY_SIZE];
	uint8_t ecdc_tweak_key[KEY_SIZE];
	uint8_t data_key[KEY_SIZE];
	uint8_t tweak_key[KEY_SIZE];
	uint8_t plain[SECTOR_SIZE];
	uint8_t manual_cipher[SECTOR_SIZE];
	uint8_t evp_cipher[SECTOR_SIZE];
	uint8_t roundtrip[SECTOR_SIZE];
	uint32_t lba = LOCKED_SECTORS + 1234;
	AES_KEY data_encrypt_key;
	AES_KEY data_decrypt_key;
	AES_KEY tweak_encrypt_key;

	for (int i = 0; i < SECTOR_SIZE; i++)
		plain[i] = (uint8_t)(i ^ (i >> 3) ^ 0x5a);

	derive_key(password, strlen((const char *)password), salt, ecdc_data_key);
	salt[0]++;
	derive_key(password, strlen((const char *)password), salt, ecdc_tweak_key);
	ecdc_key_to_aes_key(data_key, ecdc_data_key);
	ecdc_key_to_aes_key(tweak_key, ecdc_tweak_key);

	AES_set_encrypt_key(data_key, 256, &data_encrypt_key);
	AES_set_decrypt_key(data_key, 256, &data_decrypt_key);
	AES_set_encrypt_key(tweak_key, 256, &tweak_encrypt_key);

	memcpy(manual_cipher, plain, sizeof(manual_cipher));
	xts_crypt_sector(manual_cipher, lba, &data_encrypt_key, &tweak_encrypt_key,
	                 AES_encrypt);

	if (!evp_xts_encrypt(evp_cipher, plain, data_key, tweak_key, lba)) {
		fprintf(stderr, "OpenSSL EVP XTS encryption failed\n");
		return 1;
	}

	if (memcmp(manual_cipher, evp_cipher, SECTOR_SIZE) != 0) {
		fprintf(stderr, "Manual XTS encryption does not match OpenSSL EVP\n");
		return 1;
	}

	memcpy(roundtrip, manual_cipher, sizeof(roundtrip));
	xts_crypt_sector(roundtrip, lba, &data_decrypt_key, &tweak_encrypt_key,
	                 AES_decrypt);

	if (memcmp(roundtrip, plain, SECTOR_SIZE) != 0) {
		fprintf(stderr, "Manual XTS decrypt roundtrip failed\n");
		return 1;
	}

	printf("XTS test passed\n");
	return 0;
}
