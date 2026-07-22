#!/usr/bin/env bash

set -euo pipefail

make clean
make AES_MODE=CTR KDF_ROUNDS=100000
make AES_MODE=CTR KDF_ROUNDS=600000
make AES_MODE=XTS KDF_ROUNDS=100000
make AES_MODE=XTS KDF_ROUNDS=600000

cp build/CTR_100K/Phantomdrive_MSC.elf build/phantomdrive_ctr_100k.elf
cp build/CTR_600K/Phantomdrive_MSC.elf build/phantomdrive_ctr_600k.elf
cp build/XTS_100K/Phantomdrive_MSC.elf build/phantomdrive_xts_100k.elf
cp build/XTS_600K/Phantomdrive_MSC.elf build/phantomdrive_xts_600k.elf
