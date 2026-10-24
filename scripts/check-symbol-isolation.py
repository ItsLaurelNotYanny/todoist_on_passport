#!/usr/bin/env python3
"""Inspect ELF dynamic symbol table (.dynsym) and verify zero exported private TLS symbols."""
import sys
import struct
from pathlib import Path

def check_elf_symbols(elf_path: Path):
    binary = elf_path.read_bytes()
    if not binary.startswith(b"\x7fELF"):
        raise SystemExit(f"Not an ELF binary: {elf_path}")

    # Check 32-bit vs 64-bit
    ei_class = binary[4] # 1 = 32-bit, 2 = 64-bit
    if ei_class != 1:
        # For 64-bit host ELF, read 64-bit header
        shoff = struct.unpack_from("<Q", binary, 40)[0]
        shsize, shnum = struct.unpack_from("<HH", binary, 58)
        shent_size = 64
        sections = [struct.unpack_from("<IIQQQQIIQQ", binary, shoff + i * shsize) for i in range(shnum)]
        # Section types: SHT_DYNSYM = 11
        for section in sections:
            if section[1] != 11:
                continue
            strings_sec = sections[section[6]]
            table = binary[strings_sec[4]:strings_sec[4] + strings_sec[5]]
            entsize = section[9] if section[9] > 0 else 24
            for offset in range(section[4], section[4] + section[5], entsize):
                name, info, other, shndx = struct.unpack_from("<IBBH", binary, offset)
                symbol = table[name:].split(b"\0", 1)[0]
                visibility = other & 3
                bind = info >> 4
                if shndx != 0 and shndx < 0xff00 and bind in (1, 2) and visibility == 0:
                    if symbol.startswith((b"SSL_", b"OPENSSL_", b"EVP_", b"X509_", b"CRYPTO_", b"curl_")):
                        raise SystemExit(f"FAIL: Exported private symbol found in .dynsym: {symbol.decode('latin1', errors='replace')}")
        print(f"OK: Verified symbol isolation for 64-bit ELF {elf_path} (0 private TLS symbols in .dynsym)")
        return

    # 32-bit ELF (BB10 ARMv7)
    shoff = struct.unpack_from("<I", binary, 32)[0]
    shsize, shnum = struct.unpack_from("<HH", binary, 46)
    sections = [struct.unpack_from("<10I", binary, shoff + i * shsize) for i in range(shnum)]
    for section in sections:
        if section[1] != 11: # SHT_DYNSYM
            continue
        strings = sections[section[6]]
        table = binary[strings[4]:strings[4] + strings[5]]
        entsize = section[9] if section[9] > 0 else 16
        for offset in range(section[4], section[4] + section[5], entsize):
            name, _, _, info, visibility, defined = struct.unpack_from("<IIIBBH", binary, offset)
            symbol = table[name:].split(b"\0", 1)[0]
            if defined != 0 and defined < 0xff00 and (info >> 4) in (1, 2) and (visibility & 3) == 0:
                if symbol.startswith((b"SSL_", b"OPENSSL_", b"EVP_", b"X509_", b"CRYPTO_", b"curl_")):
                    raise SystemExit(f"FAIL: Exported private symbol found in .dynsym: {symbol.decode('latin1', errors='replace')}")
    print(f"OK: Verified symbol isolation for 32-bit ELF {elf_path} (0 private TLS symbols in .dynsym)")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(f"Usage: {sys.argv[0]} <elf_file>...")
    for arg in sys.argv[1:]:
        check_elf_symbols(Path(arg))
