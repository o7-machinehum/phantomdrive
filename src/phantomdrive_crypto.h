#ifndef PHANTOMDRIVE_CRYPTO_H_
#define PHANTOMDRIVE_CRYPTO_H_

#include <stddef.h>
#include <stdint.h>
#include "crypto.h"

void phantomdrive_crypto_unlock(const uint8_t *password, size_t pw_len,
                                uint8_t salt[KDF_SALT_SIZE]);

#endif /* PHANTOMDRIVE_CRYPTO_H_ */
