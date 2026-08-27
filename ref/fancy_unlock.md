# Fancy Unlock
There have been two major points raised about the drive from the community.

## Wrong password unlock
If the wrong password is entered, it will derive the wrong AES key and remount the drive with junk data. If the user changes something ie: formats the drive this will corrupt the data. Ideally if the user enters the wrong password it shouldn't unlock.

## Accdential unlock
The device currently snoops for the string "password:", if locked sector is used for a bunch of data, it's possible that this string might show up somewhere in this data and trigger an "accdential unlock"

## Solution
I'm proposing a solution that fixes both of these issues in one nice architecture change. It uses two magic registers at the end of the memory. These registers are considered to be "consumed" by the firmware and may not be used for data anymore and are not exposed to the OS.

```
[ 8GB ] [          xGB           ] [ 32B ][ 16B ]
  LS               ULS                M1     M2
```

- *LS*: Locked sector, unencrypted.
- *ULS*: Unlocked sector, encrypted. Size based on SD card capacity.
- *M1*: Magic sector 1, unencrypted. Has two parts P1 and P2.
    - P1: 16 byte string indicating the match phrase, default to "password".
    - P2: 16 byte string indicating device has been provisioned. Just contains the unique_id of the device.
- *M2*: Magic Sector 2, encrypted.
    - Contains AES(USB ID)

## New Device Flow
It's psudocode but you get the point

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
        // Entered password is not corrext.
    }
} else {
    m2 = encrypt(key, unique_id);
    m1.p2 = unique_id;
    provisioned_device = true;
    // continue with encryption / decryption
}
```
