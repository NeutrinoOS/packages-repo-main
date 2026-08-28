#!/usr/bin/env python3
"""Decode an intel-uhd RCSDBG1 token and verify its transcription CRC."""

import sys


ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
DECODE = {character: index for index, character in enumerate(ALPHABET)}
DECODE.update({"O": 0, "I": 1, "L": 1})
FIELDS = (
    "flags", "head_qword", "tail_qword", "ctl", "mode", "runlist",
    "els_hi", "els_lo", "csb", "fence_xor_expected", "expected",
    "eir", "esr", "ipeir", "ipehr", "fault", "tlb_hi", "tlb_lo",
)


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def decode_base32(token: str) -> bytes:
    compact = token.upper().replace("RCSDBG1", "").replace("-", "").strip()
    accumulator = 0
    bits = 0
    result = bytearray()
    for character in compact:
        if character.isspace():
            continue
        if character not in DECODE:
            raise ValueError(f"invalid token character {character!r}")
        accumulator = (accumulator << 5) | DECODE[character]
        bits += 5
        if bits >= 8:
            bits -= 8
            result.append((accumulator >> bits) & 0xFF)
    return bytes(result)


def read_varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while offset < len(data):
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7
        if shift > 28:
            raise ValueError("oversized varint")
    raise ValueError("truncated varint")


def main() -> int:
    token = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else sys.stdin.read()
    raw = decode_base32(token)
    if len(raw) < 4:
        raise ValueError("token is too short")
    payload, supplied_crc = raw[:-2], int.from_bytes(raw[-2:], "big")
    calculated_crc = crc16(payload)
    if supplied_crc != calculated_crc:
        raise ValueError(
            f"CRC mismatch: token={supplied_crc:04X} calculated={calculated_crc:04X}"
        )
    if payload[0] != 1:
        raise ValueError(f"unsupported schema {payload[0]}")

    values = {}
    offset = 1
    for field in FIELDS:
        values[field], offset = read_varint(payload, offset)
    if offset != len(payload):
        raise ValueError("unexpected data after final field")

    flags = values.pop("flags")
    values["head"] = values.pop("head_qword") << 3
    values["tail"] = values.pop("tail_qword") << 3
    values["fence"] = values.pop("fence_xor_expected") ^ values["expected"]
    print(
        f"schema=1 fence_complete={flags & 1} ring_idle={(flags >> 1) & 1} "
        f"elsp_clear={(flags >> 2) & 1}"
    )
    order = (
        "head", "tail", "ctl", "mode", "runlist", "els_hi", "els_lo",
        "csb", "fence", "expected", "eir", "esr", "ipeir", "ipehr",
        "fault", "tlb_hi", "tlb_lo",
    )
    print(" ".join(f"{field}=0x{values[field]:x}" for field in order))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as error:
        print(f"decode error: {error}", file=sys.stderr)
        raise SystemExit(2)
