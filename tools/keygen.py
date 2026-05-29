#!/usr/bin/env python3
"""
License Key Generator for QtScrcpy Custom Build
Usage: python keygen.py [count]
  count: number of keys to generate (default: 1)
"""

import hashlib
import secrets
import sys
import struct

SALT = "QtScrcpy_Salt_2024"

def compute_checksum(prefix_hex: str) -> str:
    data = prefix_hex.encode('utf-8') + SALT.encode('utf-8')
    h = hashlib.sha256(data).digest()

    result = 0
    for i in range(0, len(h), 2):
        val = h[i] << 8
        if i + 1 < len(h):
            val |= h[i + 1]
        result ^= val
    result &= 0xFFFF

    return format(result, '04X')

def generate_key() -> str:
    part1 = secrets.token_hex(2).upper()
    part2 = secrets.token_hex(2).upper()
    part3 = secrets.token_hex(2).upper()
    prefix = part1 + part2 + part3
    part4 = compute_checksum(prefix)
    return f"{part1}-{part2}-{part3}-{part4}"

def validate_key(key: str) -> bool:
    parts = key.strip().upper().split('-')
    if len(parts) != 4:
        return False
    for p in parts:
        if len(p) != 4:
            return False
        try:
            int(p, 16)
        except ValueError:
            return False
    prefix = parts[0] + parts[1] + parts[2]
    expected = compute_checksum(prefix)
    return parts[3] == expected

if __name__ == '__main__':
    count = 1
    if len(sys.argv) > 1:
        try:
            count = int(sys.argv[1])
        except ValueError:
            print(f"Usage: {sys.argv[0]} [count]")
            sys.exit(1)

    print(f"Generating {count} license key(s):\n")
    for i in range(count):
        key = generate_key()
        valid = validate_key(key)
        print(f"  {key}  (valid: {valid})")
    print()
