# Tests
These tests are used to verify the functionality of the device.

## Integrity Tests
These are tests that verify the devices works as a flash drive, check speeds, read/writes are working etc.

``` bash
# f3 Can be used to write and then read back some data to verify
sudo mount /dev/sda1 /mnt/ # Mount the drive
f3write /mnt/
f3read /mnt/
```

This test was also done after removing the disk, inserting, unlocking, and running f3read to ensure data survives a cycle like this.

## Encryption Tests
These are tests to verify the cryptographic functions are working.

### SHA256 implementation

``` bash
cd test
make sha256_test # To test the sha256 function
./build/sha256_test
```

### PBKDF2-HMAC-SHA256 implementation

``` bash
cd test
make pbkdf2_test
./build/pbkdf2_test
```

This compares the project KDF at 600,000 iterations against OpenSSL.

### AES-CTR Test

First write data through an unlocked Phantomdrive running the AES-CTR firmware,
then unmount it, remove the SD card, and insert the SD card into the computer.
Set the password and device salt in `ctr_test.c`. You can find the salt with:

``` bash
udevadm info --query=property --name=/dev/sdc | grep ID_SERIAL_SHORT
ID_SERIAL_SHORT=HydraUSB3_SN:34FC1FA7145467F7
```

Then fill out the required values in `ctr_test.c`:

``` C
const uint8_t password[] = "pineapple";
uint8_t salt[KDF_SALT_SIZE] = {0x34, 0xfc, 0x1f, 0xa7, 0x14, 0x54, 0x67, 0xf7};
```

``` bash
make ctr_test
sudo ./build/ctr_test /dev/sdX
sudo losetup -Pf --show unencrypted.blob
sudo mount /dev/loop0p1 /mnt/
# Verify your files. You can use f3 if you like.
```

`ctr_test` reads the ciphertext actually written by the device, derives the key
with the project KDF, and decrypts it with OpenSSL AES-256-CTR.

### AES-XTS Test

First write data through an unlocked Phantomdrive running the AES-XTS firmware,
then unmount it, remove the SD card, and insert the SD card into the computer.
Set the password and device salt in `xts_test.c`, then decrypt the raw SD card:

``` bash
cd test
make xts_test
sudo ./build/xts_test /dev/sdX
sudo losetup -Pf --show unencrypted.blob
sudo mount /dev/loop0p1 /mnt/
```

`xts_test` reads the ciphertext actually written by the device, derives the data
and tweak keys with the project KDF, and decrypts it with OpenSSL AES-256-XTS.
