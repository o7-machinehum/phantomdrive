/*
   Test proceduce
   1. Unlock phantomdrive with your password: "pineapple", or whatever, just change the C below.
   2. Create a partition, move some data there. Call sync.
   3. Unmount, Remove USB drive, remove SD card from drive, insert SD to PC.
   5. make kdf && ./kdf /dev/sdX
   6. You should now have unencrypted.blob
   7. This can now be mounted $ sudo losetup -Pf --show *.img # Setup
*/

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include <openssl/aes.h>

#include "../src/crypto.h"
#include "../src/phantomdrive.h"

#define KEY_SIZE 32
#define SECTOR_SIZE 512
#define START_LBA LOCKED_SECTORS
#define INPUT_SKIP_SECTORS START_LBA
#define CHUNK_SIZE (1024 * 1024)

static void u32be(unsigned char *p, uint32_t x)
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

void decrypt(const char* blob, uint8_t key[32]) {
	int fd = open(blob, O_RDONLY);
	if (fd < 0) {
		perror(blob);
		return;
	}

	off_t size = lseek(fd, 0, SEEK_END);
	if (size < 0) {
		perror("lseek");
		close(fd);
		return;
	}

	off_t input_start = (off_t)INPUT_SKIP_SECTORS * SECTOR_SIZE;
	if (input_start > size) {
		fprintf(stderr, "%s is smaller than INPUT_SKIP_SECTORS\n", blob);
		close(fd);
		return;
	}

	size -= input_start;
	if (lseek(fd, input_start, SEEK_SET) < 0) {
		perror("lseek");
		close(fd);
		return;
	}

	unsigned char *buf = malloc(CHUNK_SIZE);
	if (!buf) {
		perror("malloc");
		close(fd);
		return;
	}

	int out = open("unencrypted.blob", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		perror("unencrypted.blob");
		close(fd);
		free(buf);
		return;
	}

	AES_KEY aes_key;
	uint8_t openssl_key[32];
	ecdc_key_to_aes_key(openssl_key, key);
	AES_set_encrypt_key(openssl_key, 256, &aes_key);

	off_t total = 0;
	while (total < size) {
		size_t wanted = CHUNK_SIZE;
		if ((off_t)wanted > size - total)
			wanted = (size_t)(size - total);

		ssize_t n = read(fd, buf, wanted);
		if (n < 0) {
			perror("read");
			break;
		}
		if (n == 0)
			break;

		for (ssize_t off = 0; off < n; off += AES_BLOCK_SIZE) {
			off_t abs_off = total + off;
			uint32_t lba = START_LBA + (uint32_t)(abs_off / SECTOR_SIZE);
			uint32_t block = (uint32_t)((abs_off % SECTOR_SIZE) / AES_BLOCK_SIZE);
			unsigned char ctr[AES_BLOCK_SIZE] = {0};
			unsigned char stream[AES_BLOCK_SIZE];

			/* ECDC registers hold low counter word first; AES sees the 128-bit value big-endian. */
			u32be(ctr + 8, lba);
			u32be(ctr + 12, block);

			AES_encrypt(ctr, stream, &aes_key);

			for (int i = 0; i < AES_BLOCK_SIZE && off + i < n; i++)
				buf[off + i] ^= stream[i];
		}

		if (write(out, buf, (size_t)n) != n) {
			perror("write");
			break;
		}

		total += n;
	}

	close(fd);
	close(out);

	free(buf);
}

int main(int argc, char *argv[]) {
	// ad56a659e353bee798b7c769ff037ad1c1d0f153ccaf87a48af45a1e82637de0
    const uint8_t password[] = "pineapple";
	uint8_t key_bytes[KEY_SIZE];

	if(argc < 2) {
		printf("Please point to block device ie: ./kdf /dev/sdX\n");
		return 1;
	} else {
		printf("Using block device: %s \n", argv[1]);
	}

	// Function from the firwmare
	derive_key(password, strlen((const char *)password), key_bytes);

	printf("Key: ");
	for(int i = 0; i < KEY_SIZE ; i++) {
        printf("%02x", key_bytes[i]);
	};

	printf("\n");

	// First we use the kdf to generate our key
	// Then we use this key to AES decrypt out disk
	decrypt(argv[1], key_bytes);

	return 0;
}
