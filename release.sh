#!/usr/bin/env bash

set -euo pipefail

make clean
make AES_MODE=CTR KDF_ROUNDS=100000
make AES_MODE=CTR KDF_ROUNDS=600000
make AES_MODE=XTS KDF_ROUNDS=100000
make AES_MODE=XTS KDF_ROUNDS=600000

cp build/CTR_100K/Phantomdrive_MSC.bin build/phantomdrive_ctr_100k.bin
cp build/CTR_600K/Phantomdrive_MSC.bin build/phantomdrive_ctr_600k.bin
cp build/XTS_100K/Phantomdrive_MSC.bin build/phantomdrive_xts_100k.bin
cp build/XTS_600K/Phantomdrive_MSC.bin build/phantomdrive_xts_600k.bin
