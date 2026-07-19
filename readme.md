![](img/Logo_white_big.png)

Phantomdrive is open source encrypted USB drive with a stealth mechanism to hide its second partition. To decrypt it you must create a file containing your password in the format `password:addpasswordhere`, this is used to derive AES-XTS keys. The drive automatically unmounts itself, remounts the remaining disk and encrypts and decrypts in place. It uses CH569W SoC, which has USB3, SDIO and an AES hardware block. It is programmable over USB using the `wch-ch56x-isp` library.

``` bash
|-- ee             # Hardware files
|-- Makefile
|-- readme.md      # This files
|-- test           # Test the cypto components
|-- ref            # Reference docs
|-- src            # Firmware
|-- tests          # Verification scripts
|-- wch-ch56x-bsp  # Board support package
`-- wch-ch56x-isp  # Programming software
```

# Getting Started
``` bash
git submodule update --init --recursive --checkout --force
cd wch-ch56x-isp
make # Build the ISP tool
```

## Install the toolchain
The HydraUSB3 project requires a patched GCC that supports the
`WCH-Interrupt-fast` interrupt attribute. Download the xpack GCC from the HydraUSB3 project:

```bash
cd /tmp
curl -sL -o riscv-gcc-xpack.tar.gz \
  "https://github.com/hydrausb3/riscv-none-elf-gcc-xpack/releases/download/12.2.0-1/xpack-riscv-none-elf-gcc-12.2.0-1-linux-x64.tar.gz"
tar xzf riscv-gcc-xpack.tar.gz
sudo cp -r xpack-riscv-none-elf-gcc-12.2.0-1/* /usr/local/
```

## Debugging
```
make UART=1 # Enable UART
```

## Build and Flashing the project

Flashing requires the AES mode to be selected explicitly in the same `make` invocation.

``` bash
# Remove flash drive
# While holding boot button, plug in

# Build and flash AES-XTS firmware
make AES_MODE=XTS flash

# Or build and flash AES-CTR firmware
make AES_MODE=CTR flash
```

## Unlocking Drive
``` bash
sudo echo "password:YourPasswordHere13245" > /mnt/unlock.txt
```

# Releasing the hardware
``` bash
./scripts/release_hardware.sh
```

# Security Notes

This code has not been professionally audited. Treat Phantomdrive as an experimental open source hardware/firmware project rather than a formally reviewed security product. The current level of validation is described in the [test README](test/readme.md). I am not responsible for loss of data, security incidents, or other damage resulting from use of this project.

The encrypted area currently uses AES-256-XTS. This is an on-disk format change from the previous AES-CTR implementation, so data written by older firmware will not decrypt correctly without migration or reformatting.

Unlock detection is also content-based. While the device is locked, any write data containing the string `password:` can be interpreted as an unlock attempt; the file does not need to be named `unlock.txt`.
