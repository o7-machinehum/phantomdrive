# Fancy Unlock
There have been two major points raised about the drive from the community.

This change implements password verification only. The custom phrase field stays
reserved for later; unlock detection still uses the literal `password:` string.

## Wrong password unlock
If the wrong password is entered, it will derive the wrong AES key and remount the drive with junk data. If the user changes something ie: formats the drive this will corrupt the data. Ideally if the user enters the wrong password it shouldn't unlock.

## Accidental unlock
The device currently snoops for the string "password:", if locked sector is used for a bunch of data, it's possible that this string might show up somewhere in this data and trigger an "accidental unlock".

## Solution
I'm proposing a solution that rejects wrong passwords after provisioning and allows a custom match phrase. It uses two magic sectors at the end of the memory. These sectors are considered to be "consumed" by the firmware and may not be used for data anymore and are not exposed to the OS.

```
[ 8GB ] [          xGB           ] [512B][512B]
  LS               ULS                M1     M2
```

- *LS*: Locked sector, unencrypted.
- *ULS*: Unlocked sector, encrypted. Size based on SD card capacity.
- *M1*: Magic sector 1, unencrypted. Has two parts P1 and P2.
    - P1: 16 bytes reserved for a future custom match phrase, initialized to "password" with zero padding. This field is currently ignored when matching writes.
    - P2: 16 bytes indicating device has been provisioned. Contains the 8-byte unique_id of the device in little-endian order, followed by 8 zero bytes.
- *M2*: Magic Sector 2, encrypted.
    - Contains the same 16-byte device ID, followed by zeros. The entire sector is encrypted using the selected disk AES mode and its physical sector address.

## New Device Flow
It's pseudocode but you get the point.

``` cpp
// Power on
bool provisioned_device = false;
String unique_id = read_device_uid();
if(m1.p2 == unique_id) {
    // This is a provisioned device
    provisioned_device = true;
}
else {
    // This is not a provisioned device
    m1.p1 = "password"; // Set unlock phrase
}

String unlock_phrase = m1.p1;
String password = wait_for_unlock(unlock_phrase);
Key key = derive_aes_key(password);
m2_unencrypted = decrypt_m2(key); // Unencrypt magic sector 2

if(provisioned_device) {
    if(m2_unencrypted == unique_id) {
        // Entered password is correct
        // continue with encryption / decryption
    } else {
        // Entered password is not correct; stay locked.
    }
} else {
    m2 = encrypt(key, unique_id);
    m1.p2 = unique_id;
    provisioned_device = true;
    // continue with encryption / decryption
}
```

## Implementation details

SD reads and writes use whole 512-byte sectors, so M1 is at physical sector
`sector_count - 2` and M2 at `sector_count - 1`. The unlocked capacity excludes
both sectors. M2 is written and read back before committing and reading back M1.
Metadata I/O errors reject the attempt; startup metadata errors prevent USB
enumeration. A rejected attempt reconnects the locked volume and allows retrying.

The existing password snooping and partial-password handling are unchanged.

## Security limitations

- The first unlock attempt provisions the card, including a typo or an accidental match in ordinary data. The default phrase alone does not prevent this.
- M1 exposes the phrase and device ID to anyone reading the raw card, revealing provisioning and weakening plausible deniability. M2 provides an offline password verifier; strong passwords remain necessary ([NIST SP 800-132](https://csrc.nist.gov/pubs/sp/800/132/final)). There is no retry limit.
- Metadata is not authenticated. Erasing or changing P2, or moving the card to another device, makes it look unprovisioned by design. Reprovisioning does not re-encrypt existing data and may leave it inaccessible. Back up M1 and M2 with the data.
- Writing M2 before M1 reduces partial provisioning risk, but does not make the two writes atomic. Power loss or card faults can still damage metadata. Low-level SD controller faults may require a power cycle.
- This verifies the password, not the integrity of stored data. CTR and XTS do not authenticate data; XTS's limitation is described in [NIST SP 800-38E](https://csrc.nist.gov/pubs/sp/800/38/e/final). CTR also reuses a sector's keystream on rewrites, exposing relationships between snapshots.
- The host sees the password, and editors or filesystems may retain other copies. RAM, debug access, and firmware replacement are not hardened against a physical attacker.
- **Existing cards need backup and migration:** provisioning overwrites the final two physical sectors, which older firmware exposed as data. Back up and reformat with this firmware, or safely shrink the existing volume first. Keep the same device, AES mode, KDF rounds, and locked capacity to read existing ciphertext.
