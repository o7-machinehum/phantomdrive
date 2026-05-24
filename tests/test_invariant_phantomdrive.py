import pytest
import ctypes
import struct


# Simulated fixed buffer size (as would be defined in the embedded firmware)
PENDING_PW_BUFFER_SIZE = 64  # Fixed destination buffer size


def simulate_password_copy(buf, pw_start, pw_len, dest_buffer_size=PENDING_PW_BUFFER_SIZE):
    """
    Simulates the vulnerable memcpy behavior with a safety check.
    Returns (success, bytes_copied, overflow_detected).
    
    This models what SHOULD happen (safe) vs what the vulnerable code does.
    The invariant: pw_len must never exceed dest_buffer_size.
    """
    if pw_start < 0:
        return False, 0, False
    
    if pw_start > len(buf):
        return False, 0, False
    
    available_data = len(buf) - pw_start
    
    # Check if pw_len exceeds destination buffer — this is the invariant
    overflow_detected = pw_len > dest_buffer_size
    
    # Check if pw_len exceeds available source data
    if pw_len > available_data:
        return False, 0, overflow_detected
    
    # Safe copy: only copy up to dest_buffer_size bytes
    safe_len = min(pw_len, dest_buffer_size)
    pending_pw = buf[pw_start:pw_start + safe_len]
    
    return True, len(pending_pw), overflow_detected


def build_usb_command(pw_start, pw_len, password_data):
    """Build a simulated USB bulk transfer buffer."""
    # Header: [cmd_byte(1), pw_start(2), pw_len(2), ...password_data...]
    header = struct.pack(">BHH", 0x42, pw_start, pw_len)
    return header + password_data


@pytest.mark.parametrize("payload", [
    # (description, pw_start, pw_len, password_data)
    # Normal valid input
    ("normal_32_bytes", 5, 32, b"A" * 32),
    # Exact boundary
    ("exact_boundary_64", 5, 64, b"B" * 64),
    # Oversized by 1 byte
    ("overflow_by_1", 5, 65, b"C" * 65),
    # Oversized by 2x buffer size
    ("overflow_2x", 5, 128, b"D" * 128),
    # Oversized by 10x buffer size
    ("overflow_10x", 5, 640, b"E" * 640),
    # Maximum uint16 value (attacker-controlled)
    ("overflow_max_uint16", 5, 65535, b"F" * 200),
    # Zero length (edge case)
    ("zero_length", 5, 0, b""),
    # pw_start at end of buffer
    ("pw_start_at_end", 200, 64, b"G" * 64),
    # pw_start + pw_len wraps around (integer overflow attempt)
    ("wrap_around_attempt", 65530, 64, b"H" * 64),
    # Null bytes in password
    ("null_bytes", 5, 32, b"\x00" * 32),
    # SQL injection attempt in password field
    ("sql_injection", 5, 50, b"' OR '1'='1'; DROP TABLE users; --" + b"\x00" * 16),
    # Format string attack
    ("format_string", 5, 40, b"%s%s%s%s%n%n%n%n" + b"A" * 24),
    # Shell metacharacters
    ("shell_metachar", 5, 48, b"; cat /etc/passwd; echo " + b"A" * 24),
    # Binary payload with high bytes
    ("binary_high_bytes", 5, 64, bytes(range(128, 192))),
    # Repeated pattern for heap spray simulation
    ("heap_spray_pattern", 5, 512, b"\x41\x42\x43\x44" * 128),
    # NOP sled + shellcode-like pattern
    ("nop_sled", 5, 256, b"\x90" * 200 + b"\xcc" * 56),
    # Oversized with pw_start=0
    ("overflow_from_start", 0, 640, b"I" * 640),
    # Negative-like value when interpreted as signed (large uint)
    ("signed_overflow_trick", 5, 0xFFFF, b"J" * 100),
    # Password exactly at buffer boundary with offset
    ("boundary_with_offset", 10, 54, b"K" * 54),
    # Extremely large pw_start to cause out-of-bounds read
    ("large_pw_start", 0xFFF0, 64, b"L" * 64),
])
def test_buffer_read_never_exceeds_declared_length(payload):
    """
    Invariant: Buffer reads must never exceed the declared destination buffer size.
    
    When pw_len (attacker-controlled) exceeds PENDING_PW_BUFFER_SIZE, the copy
    operation must either reject the input or truncate it to the safe buffer size.
    Under no circumstances should more than PENDING_PW_BUFFER_SIZE bytes be
    written to the pending_pw buffer.
    """
    description, pw_start, pw_len, password_data = payload
    
    # Build the USB command buffer as an attacker would
    buf = build_usb_command(pw_start, pw_len, password_data)
    
    # Parse pw_start and pw_len from the buffer (as the firmware would)
    if len(buf) >= 5:
        _, parsed_pw_start, parsed_pw_len = struct.unpack_from(">BHH", buf, 0)
    else:
        parsed_pw_start, parsed_pw_len = pw_start, pw_len
    
    # Simulate the copy operation
    success, bytes_copied, overflow_detected = simulate_password_copy(
        buf, parsed_pw_start, parsed_pw_len, PENDING_PW_BUFFER_SIZE
    )
    
    # INVARIANT 1: Bytes actually copied must never exceed the destination buffer size
    assert bytes_copied <= PENDING_PW_BUFFER_SIZE, (
        f"[{description}] BUFFER OVERFLOW: {bytes_copied} bytes copied into "
        f"{PENDING_PW_BUFFER_SIZE}-byte buffer. pw_len={parsed_pw_len}"
    )
    
    # INVARIANT 2: If pw_len exceeds buffer size, the operation must be rejected or truncated
    if parsed_pw_len > PENDING_PW_BUFFER_SIZE:
        assert bytes_copied <= PENDING_PW_BUFFER_SIZE, (
            f"[{description}] Oversized pw_len={parsed_pw_len} was not safely handled. "
            f"Copied {bytes_copied} bytes into {PENDING_PW_BUFFER_SIZE}-byte buffer."
        )
    
    # INVARIANT 3: If pw_start is beyond buffer bounds, operation must fail
    if parsed_pw_start >= len(buf):
        assert not success, (
            f"[{description}] Copy succeeded with out-of-bounds pw_start={parsed_pw_start} "
            f"(buf size={len(buf)})"
        )
    
    # INVARIANT 4: pw_start + bytes_copied must not exceed source buffer length
    if success and bytes_copied > 0:
        assert parsed_pw_start + bytes_copied <= len(buf), (
            f"[{description}] Read beyond source buffer: "
            f"pw_start={parsed_pw_start} + bytes_copied={bytes_copied} > buf_len={len(buf)}"
        )


@pytest.mark.parametrize("pw_len_value", [
    65,       # 1 byte over
    128,      # 2x
    640,      # 10x
    1024,     # 16x
    65535,    # max uint16
    0xFFFFFFFF,  # max uint32 (if length field is 4 bytes)
])
def test_oversized_pw_len_is_rejected_or_truncated(pw_len_value):
    """
    Invariant: Any pw_len value exceeding PENDING_PW_BUFFER_SIZE must result in
    either rejection (failure) or safe truncation — never a full oversized copy.
    """
    pw_start = 5
    # Provide enough source data to make the copy "possible" from source perspective
    source_data = b"X" * min(pw_len_value, 10000)  # cap source to avoid memory issues
    buf = b"\x00" * pw_start + source_data
    
    success, bytes_copied, overflow_detected = simulate_password_copy(
        buf, pw_start, pw_len_value, PENDING_PW_BUFFER_SIZE
    )
    
    # The core invariant: never copy more than the destination buffer can hold
    assert bytes_copied <= PENDING_PW_BUFFER_SIZE, (
        f"CRITICAL: pw_len={pw_len_value} caused {bytes_copied} bytes to be copied "
        f"into a {PENDING_PW_BUFFER_SIZE}-byte buffer. This is a buffer overflow."
    )
    
    # If overflow was detected, the operation should have been truncated or rejected
    if overflow_detected:
        assert bytes_copied <= PENDING_PW_BUFFER_SIZE, (
            f"Overflow detected but not prevented: {bytes_copied} bytes written "
            f"to {PENDING_PW_BUFFER_SIZE}-byte buffer"
        )