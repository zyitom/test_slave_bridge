#!/usr/bin/env python3
"""Rewrite the SSC-Tool-generated SII image (eeprom.h) for the stream bridge.

The SSC Tool project stays the stock SDK ecat_io configuration (see
core0/ssc_overrides/digital_ioObjects.h for why), so the SII it emits still
describes two 32-bit counter PDOs with 4-byte SyncManager buffers. This script
performs the structured edits that make the SII agree with the 48-byte
stream-chunk process data actually implemented by the firmware:

  * SyncManager category: SM2/SM3 default length -> the PD chunk size. The
    SSC's CheckSmSettings() requires the master-programmed SM length to equal
    the mapped PD size exactly, and SOEM programs SMs from this category.
  * TXPDO/RXPDO categories: rebuilt as 32 x UNSIGNED32 entries of
    0x6000/0x7010, mirroring the CoE object dictionary (a single SII/CoE PDO
    entry is limited to 255 bits, hence the split).
  * Strings category: rebuilt with stream-bridge names (same string count and
    order, so all name indices in other categories stay valid).
  * Revision number: bumped so boards already flashed with the stock ecat_io
    SII refresh their emulated EEPROM on next boot (the port layer refreshes
    when the built-in revision is greater than the stored one).

Everything else (ESC configuration area including its checksum, vendor and
product identity, mailbox bootstrap and standard config, General/FMMU/DC
categories) is copied through byte-identically. The output array is padded
with 0xFF to the input length (= ESC_EEPROM_SIZE).

Usage: patch_sii.py INPUT_EEPROM_H OUTPUT_EEPROM_H
"""

import re
import struct
import sys

PD_CHUNK_SIZE = 48   # bytes per direction; must match rmcs_pd.h
ENTRY_COUNT = 12     # 12 x UNSIGNED32 = 48 bytes
REVISION = 3         # bump whenever the SII changes so already-flashed boards
                     # refresh their emulated EEPROM on next boot

CAT_STRINGS = 10
CAT_SYNCM = 41
CAT_TXPDO = 50
CAT_RXPDO = 51
CAT_END = 0xFFFF

SM_TYPE_PD_OUT = 3  # SM2, master -> slave
SM_TYPE_PD_IN = 4   # SM3, slave -> master

DEFTYPE_UDINT = 0x07

# Replacements for the stock string table. Order and count must match the
# generated SII (name indices elsewhere are 1-based positions in this list).
STRINGS = [
    "ECAT_Device",                # 1: group name (General category GroupIdx)
    "rmcs_stream",                # 2: device name/order (shown by masters)
    "Synchron",                   # 3: sync mode name
    "SM-Synchron",                # 4: sync mode name
    "InputStream mapping",        # 5: TxPDO name
    "InputStream",                # 6: TxPDO entry name
    "OutputStream mapping",       # 7: RxPDO name
    "OutputStream",               # 8: RxPDO entry name
]


def parse_eeprom_h(text):
    values = re.findall(r"0[xX]([0-9A-Fa-f]{2})", text)
    if not values:
        raise SystemExit("no byte array found in input")
    return bytearray(int(v, 16) for v in values)


def build_strings_category():
    data = bytearray([len(STRINGS)])
    for s in STRINGS:
        encoded = s.encode("ascii")
        data += bytes([len(encoded)]) + encoded
    if len(data) % 2:
        data += b"\x00"
    return bytes(data)


def patch_syncm_category(data):
    if len(data) % 8:
        raise SystemExit("SyncM category size is not a multiple of 8")
    out = bytearray(data)
    for offset in range(0, len(out), 8):
        sm_type = out[offset + 7]
        if sm_type in (SM_TYPE_PD_OUT, SM_TYPE_PD_IN):
            struct.pack_into("<H", out, offset + 2, PD_CHUNK_SIZE)
    return bytes(out)


def build_pdo_category(stock, pdo_index, entry_index):
    if len(stock) < 8:
        raise SystemExit("PDO category too short")
    # Keep SyncM / DC-sync / name-index / flags from the stock header; only
    # the PDO index (paranoia), entry count and the entry list change.
    header = bytearray(stock[:8])
    struct.pack_into("<H", header, 0, pdo_index)
    header[2] = ENTRY_COUNT
    entry_name_idx = stock[8 + 3] if len(stock) >= 16 else 0
    entries = bytearray()
    for subindex in range(1, ENTRY_COUNT + 1):
        entries += struct.pack(
            "<HBBBBH", entry_index, subindex, entry_name_idx, DEFTYPE_UDINT, 0x20, 0)
    return bytes(header) + bytes(entries)


def rebuild(image):
    total_size = len(image)
    # Bump the revision number (words 0xC..0xD, byte offset 24).
    struct.pack_into("<I", image, 24, REVISION)

    out = bytearray(image[:0x80])  # config area + addresses + size/version
    offset = 0x80
    seen = set()
    while offset + 4 <= total_size:
        cat_type, size_words = struct.unpack_from("<HH", image, offset)
        if cat_type == CAT_END:
            break
        data = bytes(image[offset + 4:offset + 4 + 2 * size_words])
        if len(data) != 2 * size_words:
            raise SystemExit("category 0x%04X overruns the image" % cat_type)
        seen.add(cat_type)

        if cat_type == CAT_STRINGS:
            data = build_strings_category()
        elif cat_type == CAT_SYNCM:
            data = patch_syncm_category(data)
        elif cat_type == CAT_TXPDO:
            data = build_pdo_category(data, 0x1A00, 0x6000)
        elif cat_type == CAT_RXPDO:
            data = build_pdo_category(data, 0x1600, 0x7010)

        if len(data) % 2:
            raise SystemExit("category 0x%04X has odd size" % cat_type)
        out += struct.pack("<HH", cat_type, len(data) // 2) + data
        offset += 4 + 2 * size_words
    else:
        raise SystemExit("no end category found")

    required = {CAT_STRINGS, CAT_SYNCM, CAT_TXPDO, CAT_RXPDO}
    missing = required - seen
    if missing:
        raise SystemExit(
            "input SII lacks categories: %s (not a stock ecat_io image?)"
            % ", ".join("0x%02X" % c for c in sorted(missing)))

    out += struct.pack("<HH", CAT_END, 0xFFFF)
    if len(out) > total_size:
        raise SystemExit("patched SII (%d bytes) exceeds EEPROM size %d" % (len(out), total_size))
    out += b"\xff" * (total_size - len(out))
    return out


def emit_eeprom_h(image):
    lines = [
        "/*",
        " * SII image for the RMCS EtherCAT stream bridge.",
        " * Generated by ecat/tools/patch_sii.py from the SSC-Tool output; do not edit.",
        " */",
        "unsigned char aEepromData[] = {",
    ]
    for i in range(0, len(image), 16):
        row = ",".join("0x%02X" % b for b in image[i:i + 16])
        terminator = "};" if i + 16 >= len(image) else ","
        lines.append(row + terminator)
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    with open(sys.argv[1], "r", encoding="ascii") as f:
        image = parse_eeprom_h(f.read())
    patched = rebuild(image)
    with open(sys.argv[2], "w", encoding="ascii") as f:
        f.write(emit_eeprom_h(patched))
    print("patched SII: %d bytes, SM2/SM3=%d bytes, %d PDO entries per direction"
          % (len(patched), PD_CHUNK_SIZE, ENTRY_COUNT))


if __name__ == "__main__":
    main()
