#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "../src/crypto.h"
#include "../src/phantomdrive.h"

#define KEY_SIZE 32
#define SECTOR_SIZE 512
#define START_LBA LOCKED_SECTORS
#define CHUNK_SIZE (1024 * 1024)

static void write_u32_be(uint8_t output[4], uint32_t value)
{
	output[0] = value >> 24;
	output[1] = value >> 16;
	output[2] = value >> 8;
	output[3] = value;
}

static void make_openssl_key(uint8_t openssl_key[KEY_SIZE],
                             const uint8_t ecdc_key[KEY_SIZE])
{
	for (int i = 0; i < KEY_SIZE; i++)
		openssl_key[i] = ecdc_key[KEY_SIZE - 1 - i];
}

static int decrypt_sector(EVP_CIPHER_CTX *aes,
                          uint8_t sector[SECTOR_SIZE],
                          const uint8_t key[KEY_SIZE * 2], uint32_t lba)
{
	uint8_t tweak[16] = {0};
	uint8_t final_block[16];
	int decrypted_len;
	int final_len;

	write_u32_be(tweak + 8, lba);
	if (EVP_DecryptInit_ex(aes, EVP_aes_256_xts(), NULL,
	                       key, tweak) != 1) {
		fprintf(stderr, "Failed to initialize AES-XTS\n");
		return -1;
	}

	if (EVP_CIPHER_CTX_set_padding(aes, 0) != 1) {
		fprintf(stderr, "Failed to disable AES-XTS padding\n");
		return -1;
	}

	if (EVP_DecryptUpdate(aes, sector, &decrypted_len,
	                      sector, SECTOR_SIZE) != 1) {
		fprintf(stderr, "Failed to decrypt AES-XTS sector\n");
		return -1;
	}

	if (decrypted_len != SECTOR_SIZE) {
		fprintf(stderr, "Decrypted sector has the wrong size\n");
		return -1;
	}

	if (EVP_DecryptFinal_ex(aes, final_block, &final_len) != 1) {
		fprintf(stderr, "Failed to finalize AES-XTS decryption\n");
		return -1;
	}

	if (final_len != 0) {
		fprintf(stderr, "AES-XTS produced unexpected final data\n");
		return -1;
	}

	return 0;
}

static int decrypt_device(const char *device, const uint8_t data_key[KEY_SIZE],
                          const uint8_t tweak_key[KEY_SIZE])
{
	uint8_t openssl_key[KEY_SIZE * 2];
	int input_fd = open(device, O_RDONLY);
	if (input_fd < 0) {
		perror(device);
		return 1;
	}

	off_t device_size = lseek(input_fd, 0, SEEK_END);
	off_t input_start = (off_t)START_LBA * SECTOR_SIZE;
	if (device_size < 0) {
		perror("lseek");
		close(input_fd);
		return 1;
	}

	if (device_size < input_start) {
		fprintf(stderr, "%s is smaller than the locked area\n", device);
		close(input_fd);
		return 1;
	}

	if (lseek(input_fd, input_start, SEEK_SET) < 0) {
		perror("lseek");
		close(input_fd);
		return 1;
	}

	uint8_t *buffer = malloc(CHUNK_SIZE);
	if (buffer == NULL) {
		perror("malloc");
		close(input_fd);
		return 1;
	}

	EVP_CIPHER_CTX *aes = EVP_CIPHER_CTX_new();
	if (aes == NULL) {
		fprintf(stderr, "Failed to create AES context\n");
		free(buffer);
		close(input_fd);
		return 1;
	}

	make_openssl_key(openssl_key, data_key);
	make_openssl_key(openssl_key + KEY_SIZE, tweak_key);

	off_t data_size = device_size - input_start;
	int output_fd = -1;
	int result = 0;
	for (off_t total = 0; total < data_size;) {
		size_t wanted = CHUNK_SIZE;
		if ((off_t)wanted > data_size - total)
			wanted = (size_t)(data_size - total);

		ssize_t bytes_read = read(input_fd, buffer, wanted);
		if (bytes_read < 0) {
			perror("read");
			result = 1;
			break;
		}

		if (bytes_read == 0) {
			fprintf(stderr, "Unexpected end of device\n");
			result = 1;
			break;
		}

		if (bytes_read % SECTOR_SIZE != 0) {
			fprintf(stderr, "Failed to read complete sectors\n");
			result = 1;
			break;
		}

		for (ssize_t offset = 0; offset < bytes_read;
		     offset += SECTOR_SIZE) {
			uint32_t lba = START_LBA;
			lba += (uint32_t)((total + offset) / SECTOR_SIZE);

			if (decrypt_sector(aes, buffer + offset,
			                   openssl_key, lba) != 0) {
				fprintf(stderr, "XTS decryption failed at LBA %u\n", lba);
				result = 1;
				break;
			}
		}
		if (result != 0)
			break;

		if (total == 0 &&
		    (buffer[510] != 0x55 || buffer[511] != 0xaa)) {
			fprintf(stderr,
			        "Decrypted data has no MBR signature; refusing to write "
			        "unencrypted.blob\n");
			result = 1;
			break;
		}
		if (total == 0)
			printf("Valid MBR signature found\n");

		if (output_fd < 0) {
			output_fd = open("unencrypted.blob",
			                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (output_fd < 0) {
				perror("unencrypted.blob");
				result = 1;
				break;
			}
		}

		ssize_t bytes_written = write(output_fd, buffer, (size_t)bytes_read);
		if (bytes_written < 0) {
			perror("write");
			result = 1;
			break;
		}

		if (bytes_written != bytes_read) {
			fprintf(stderr, "Short write to unencrypted.blob\n");
			result = 1;
			break;
		}

		total += bytes_read;
	}

	close(input_fd);
	if (output_fd >= 0)
		close(output_fd);
	EVP_CIPHER_CTX_free(aes);
	free(buffer);
	return result;
}

int main(int argc, char *argv[])
{
	const uint8_t password[] = "pineapple";

	// ID_SERIAL_SHORT=Phantomdrive_SN:3C FB 1F A7 14 54 6F F6
	uint8_t salt[KDF_SALT_SIZE] = {
		0x3C, 0xfb, 0x1f, 0xa7, 0x14, 0x54, 0x6f, 0xf6
	};
	uint8_t data_key[KEY_SIZE];
	uint8_t tweak_key[KEY_SIZE];
	size_t pw_len = sizeof(password) - 1;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/sdX\n", argv[0]);
		return 1;
	}

	if (PKCS5_PBKDF2_HMAC((const char *)password, pw_len,
	                      salt, KDF_SALT_SIZE, KDF_ROUNDS,
	                      EVP_sha256(), sizeof(data_key), data_key) != 1) {
		fprintf(stderr, "OpenSSL PBKDF2 failed for data key\n");
		return 1;
	}
	salt[0]++;
	if (PKCS5_PBKDF2_HMAC((const char *)password, pw_len,
	                      salt, KDF_SALT_SIZE, KDF_ROUNDS,
	                      EVP_sha256(), sizeof(tweak_key), tweak_key) != 1) {
		fprintf(stderr, "OpenSSL PBKDF2 failed for tweak key\n");
		return 1;
	}

	return decrypt_device(argv[1], data_key, tweak_key);
}
