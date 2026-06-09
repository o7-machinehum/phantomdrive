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
./sha256_test
```

### Encryption and KDF Tests
Generate a key with our salt + password, then decrypt our disk. Note that you need to remove the SD card from the device and insert it into your computer with a SD card reader. You need to change these two values to your password and the salt. You can get your custom salt like so...

``` bash
udevadm info --query=property --name=/dev/sdc | grep ID_SERIAL_SHORT
ID_SERIAL_SHORT=HydraUSB3_SN:34FC1FA7145467F7
```

Then fill out the required values in the code

``` C
const uint8_t password[] = "pineapple";
uint8_t salt[KDF_SALT_SIZE] = {0x34, 0xfc, 0x1f, 0xa7, 0x14, 0x54, 0x67, 0xf7};
```

``` bash
make kdf
./kdf /dev/sdX # You might need sudo
sudo losetup -Pf --show unencrypted.blob
sudo mount /dev/loop0p1 /mnt/
sudo f3read /mnt/
F3 read 10.0
Copyright (C) 2010 Digirati Internet LTDA.
This is free software; see the source for copying conditions.

                  SECTORS      ok/corrupted/changed/overwritten
Validating file 1.h2w ... 1611032/        0/      0/      0 Avg: 938.80 MB/s
	Min: 3.67 MB/s, Max: 1.28 GB/s, 17 samples

  Data OK: 786.64 MB (1611032 sectors)
Data LOST: 0.00 Bytes (0 sectors)
	       Corrupted: 0.00 Bytes (0 sectors)
	Slightly changed: 0.00 Bytes (0 sectors)
	     Overwritten: 0.00 Bytes (0 sectors)
Average sequential read speed: 947.09 MB/s (1611032 sectors / 830.5ms)
```
