#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>

#include "../src/crypto.h"

#define KEY_SIZE 32

static bool test_pbkdf2(const uint8_t *password, size_t pw_len,
                        uint8_t salt[KDF_SALT_SIZE])
{
	uint8_t expected[KEY_SIZE];
	uint8_t actual[KEY_SIZE];

	if (PKCS5_PBKDF2_HMAC((const char *)password, pw_len,
	                      salt, KDF_SALT_SIZE, KDF_ROUNDS,
	                      EVP_sha256(), sizeof(expected), expected) != 1) {
		fprintf(stderr, "OpenSSL PBKDF2 failed\n");
		return false;
	}

	derive_key(password, pw_len, salt, actual);
	if (memcmp(actual, expected, sizeof(actual)) != 0) {
		fprintf(stderr, "PBKDF2-SHA256 test failed\n");
		return false;
	}

	return true;
}

int main(void)
{
	const uint8_t password[] = "correct horse battery staple";
	uint8_t long_password[80];
	uint8_t salt[KDF_SALT_SIZE] = {
		0x34, 0xfc, 0x1f, 0xa7, 0x14, 0x54, 0x67, 0xf7
	};

	memset(long_password, 'p', sizeof(long_password));
	if (!test_pbkdf2(password, sizeof(password) - 1, salt) ||
	    !test_pbkdf2(long_password, sizeof(long_password), salt))
		return 1;

	printf("PBKDF2-SHA256 test passed\n");
	return 0;
}
