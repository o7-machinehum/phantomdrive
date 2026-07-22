#ifndef CRYPTO_H_
#define CRYPTO_H_

#include <stdint.h>
#include <stddef.h>

#ifndef KDF_ROUNDS
#define KDF_ROUNDS 100000U
#endif

#define KDF_SALT_SIZE 8

/* SHA-256: compute hash of data into 32-byte output */
void sha256(const uint8_t *data, size_t len, uint8_t hash_out[32]);

/* KDF: derive 32-byte AES-256 key using PBKDF2-HMAC-SHA256*/
void derive_key(const uint8_t *password, size_t pw_len, uint8_t kdf_salt[KDF_SALT_SIZE], uint8_t key_out[32]);

#endif /* CRYPTO_H_ */
