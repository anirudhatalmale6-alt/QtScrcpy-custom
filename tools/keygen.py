#!/usr/bin/env python3
"""
AniFelix License Key Generator (Hardware-Bound)
Usage: python keygen.py <machine_id> [count]
  machine_id: 12-character hex ID shown in the activation dialog
  count: number of keys to generate (default: 1)

The key is bound to the machine ID - it will only work on that specific computer.
"""

import hashlib
import secrets
import sys

SALT = "QtScrcpy_Salt_2024"

def compute_checksum(prefix_hex: str, machine_id: str) -> str:
    data = prefix_hex.encode('utf-8') + machine_id.encode('utf-8') + SALT.encode('utf-8')
    h = hashlib.sha256(data).digest()

    result = 0
    for i in range(0, len(h), 2):
        val = h[i] << 8
        if i + 1 < len(h):
            val |= h[i + 1]
        result ^= val
    result &= 0xFFFF

    return format(result, '04X')

def generate_key(machine_id: str) -> str:
    part1 = secrets.token_hex(2).upper()
    part2 = secrets.token_hex(2).upper()
    part3 = secrets.token_hex(2).upper()
    prefix = part1 + part2 + part3
    part4 = compute_checksum(prefix, machine_id)
    return f"{part1}-{part2}-{part3}-{part4}"

def validate_key(key: str, machine_id: str) -> bool:
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
    expected = compute_checksum(prefix, machine_id)
    return parts[3] == expected

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <machine_id> [count]")
        print(f"  machine_id: 12-char hex shown in the app activation dialog")
        print(f"  count: number of keys to generate (default: 1)")
        sys.exit(1)

    machine_id = sys.argv[1].strip().upper()
    count = 1
    if len(sys.argv) > 2:
        try:
            count = int(sys.argv[2])
        except ValueError:
            print(f"Invalid count: {sys.argv[2]}")
            sys.exit(1)

    print(f"Machine ID: {machine_id}")
    print(f"Generating {count} license key(s):\n")
    for i in range(count):
        key = generate_key(machine_id)
        valid = validate_key(key, machine_id)
        print(f"  {key}  (valid: {valid})")
    print()
