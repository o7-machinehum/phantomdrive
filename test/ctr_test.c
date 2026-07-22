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
#define AES_BLOCK_SIZE 16

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
	for (size_t i = 0; i < KEY_SIZE; i++)
		openssl_key[i] = ecdc_key[KEY_SIZE - 1 - i];
}

static int encrypt_counter(EVP_CIPHER_CTX *aes, const uint8_t counter[AES_BLOCK_SIZE],
			   uint8_t stream[AES_BLOCK_SIZE])
{
	int stream_size;
	if (EVP_EncryptUpdate(aes, stream, &stream_size, counter, AES_BLOCK_SIZE) != 1)
		return -1;
	if (stream_size != AES_BLOCK_SIZE)
		return -1;

	return 0;
}

static int decrypt_device(const char *device, const uint8_t key[KEY_SIZE])
{
	int input_fd = open(device, O_RDONLY);
	if (input_fd < 0) {
		perror(device);
		return -1;
	}

	off_t device_size = lseek(input_fd, 0, SEEK_END);
	if (device_size < 0) {
		perror("lseek");
		close(input_fd);
		return -1;
	}

	off_t input_start = (off_t)START_LBA * SECTOR_SIZE;
	if (input_start > device_size) {
		fprintf(stderr, "%s is smaller than the locked area\n", device);
		close(input_fd);
		return -1;
	}

	if (lseek(input_fd, input_start, SEEK_SET) < 0) {
		perror("lseek");
		close(input_fd);
		return -1;
	}

	uint8_t *buffer = malloc(CHUNK_SIZE);
	if (buffer == NULL) {
		perror("malloc");
		close(input_fd);
		return -1;
	}

	int output_fd = open("unencrypted.blob", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (output_fd < 0) {
		perror("unencrypted.blob");
		free(buffer);
		close(input_fd);
		return -1;
	}

	uint8_t openssl_key[KEY_SIZE];
	make_openssl_key(openssl_key, key);

	EVP_CIPHER_CTX *aes = EVP_CIPHER_CTX_new();
	if (aes == NULL) {
		fprintf(stderr, "Failed to create AES context\n");
		close(output_fd);
		free(buffer);
		close(input_fd);
		return -1;
	}
	if (EVP_EncryptInit_ex(aes, EVP_aes_256_ecb(), NULL, openssl_key, NULL) != 1) {
		fprintf(stderr, "Failed to initialize AES\n");
		EVP_CIPHER_CTX_free(aes);
		close(output_fd);
		free(buffer);
		close(input_fd);
		return -1;
	}
	EVP_CIPHER_CTX_set_padding(aes, 0);

	int result = 0;
	off_t data_size = device_size - input_start;
	off_t total = 0;
	while (total < data_size) {
		size_t wanted = CHUNK_SIZE;
		if ((off_t)wanted > data_size - total)
			wanted = data_size - total;

		ssize_t bytes_read = read(input_fd, buffer, wanted);
		if (bytes_read < 0) {
			perror("read");
			result = -1;
			break;
		}
		if (bytes_read == 0) {
			fprintf(stderr, "Unexpected end of device\n");
			result = -1;
			break;
		}
		if (bytes_read % AES_BLOCK_SIZE != 0) {
			fprintf(stderr, "Device size is not a multiple of the AES block size\n");
			result = -1;
			break;
		}

		for (ssize_t offset = 0; offset < bytes_read; offset += AES_BLOCK_SIZE) {
			off_t absolute_offset = total + offset;
			uint32_t lba = START_LBA + absolute_offset / SECTOR_SIZE;
			uint32_t block = (absolute_offset % SECTOR_SIZE) / AES_BLOCK_SIZE;
			uint8_t counter[AES_BLOCK_SIZE] = {0};
			uint8_t stream[AES_BLOCK_SIZE];

			write_u32_be(counter + 8, lba);
			write_u32_be(counter + 12, block);
			if (encrypt_counter(aes, counter, stream) != 0) {
				fprintf(stderr, "Failed to encrypt AES counter\n");
				result = -1;
				break;
			}

			for (size_t i = 0; i < AES_BLOCK_SIZE; i++)
				buffer[offset + i] ^= stream[i];
		}
		if (result != 0)
			break;

		ssize_t bytes_written = write(output_fd, buffer, bytes_read);
		if (bytes_written < 0) {
			perror("write");
			result = -1;
			break;
		}
		if (bytes_written != bytes_read) {
			fprintf(stderr, "Short write to unencrypted.blob\n");
			result = -1;
			break;
		}

		total += bytes_read;
	}

	EVP_CIPHER_CTX_free(aes);
	close(output_fd);
	free(buffer);
	close(input_fd);
	return result;
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/sdX\n", argv[0]);
		return EXIT_FAILURE;
	}

	const uint8_t password[] = "pineapple";
	uint8_t salt[KDF_SALT_SIZE] = {
		0x34, 0xfc, 0x1f, 0xa7, 0x14, 0x54, 0x67, 0xf7,
	};
	uint8_t key[KEY_SIZE];
	size_t pw_len = sizeof(password) - 1;

	if (PKCS5_PBKDF2_HMAC((const char *)password, pw_len,
	                      salt, KDF_SALT_SIZE, KDF_ROUNDS,
	                      EVP_sha256(), sizeof(key), key) != 1) {
		fprintf(stderr, "OpenSSL PBKDF2 failed\n");
		return EXIT_FAILURE;
	}

	printf("Key: ");
	for (size_t i = 0; i < KEY_SIZE; i++)
		printf("%02x", key[i]);
	printf("\nUsing block device: %s\n", argv[1]);

	if (decrypt_device(argv[1], key) != 0)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}
