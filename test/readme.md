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
# You should see "test
```

### Encryption Tests
Generate a key with our salt, key derivation function, then decrypt our disk. Note that you need to remove the SD card from the device and insert it into your computer with a SD card reader.

``` bash
make kdf
./kdf /dev/sdX # You might need sudo
sudo losetup -Pf --show unencrypted.blob
# You can then mount the loop device like a normal disk.
```
