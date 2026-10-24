#!/usr/bin/env python3
"""Verify the packaged native BlackBerry 10 BAR artifact, permissions, assets, and symbol isolation."""

import base64
import hashlib
from pathlib import Path
import struct
import sys
import zipfile


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    package = Path(sys.argv[1]) if len(sys.argv) > 1 else (root / "build/bb10/todoist-passport.bar")
    if not package.is_file():
        raise SystemExit(f"Missing BAR package: {package}")

    expected_permissions = {"run_native", "access_internet"}

    with zipfile.ZipFile(package) as archive:
        assert archive.testzip() is None, "Corrupt BAR archive"
        names = archive.namelist()
        assert not any(n.endswith((".apk", ".dex")) for n in names), "Android payload is forbidden"
        assert len(names) == len(set(names)), "Duplicate package entries"

        manifest = archive.read("META-INF/MANIFEST.MF").decode("utf-8").replace("\r\n", "\n")
        fields = dict(line.split(": ", 1) for line in manifest.splitlines() if ": " in line)

        user_actions = [p for p in fields.get("Entry-Point-User-Actions", "").split(",") if p]
        sys_actions = [p for p in fields.get("Entry-Point-System-Actions", "").split(",") if p]
        permissions = set(user_actions) | set(sys_actions)
        assert permissions == expected_permissions, f"Unexpected package permission set: {permissions!r}"
        assert fields.get("Package-Architecture") == "armle-v7", "Expected armle-v7 Package-Architecture"
        assert fields.get("Package-Name") == "com.todoist.passport", "Expected com.todoist.passport Package-Name"

        # Verify launcher icon (144x144 8-bit RGBA PNG)
        app_icon = archive.read("native/assets/icon.png")
        assert app_icon == (root / "assets/icon.png").read_bytes(), "Packaged icon mismatch"
        assert app_icon[:8] == b"\x89PNG\r\n\x1a\n", "Expected PNG signature for icon.png"
        assert struct.unpack_from(">II", app_icon, 16) == (144, 144), "Expected 144x144 launcher icon"
        assert app_icon[24:26] == bytes((8, 6)), "Expected 8-bit RGBA launcher icon"

        # Verify Cascades QML assets
        for qml_name in ("main.qml", "ActiveFrameCover.qml"):
            rel = f"assets/{qml_name}"
            packaged_qml = archive.read(f"native/{rel}")
            assert packaged_qml == (root / rel).read_bytes(), f"QML asset mismatch: {qml_name}"

        # Guard against Page having eventHandlers (Page is an AbstractPane, not a VisualNode)
        main_qml_text = (root / "assets/main.qml").read_text()
        page_header = main_qml_text.split("shortcuts: [")[0]
        assert "eventHandlers:" not in page_header, "Page cannot have eventHandlers (only VisualNodes like Control/Container/ListView support eventHandlers)"
        assert "filterChanged.connect(function() {\n            taskListView.requestFocus();" not in main_qml_text, (
            "filterChanged handler must not unconditionally steal focus via taskListView.requestFocus() (M5-R003)"
        )

        # Verify bundled CA bundle and licenses
        assert archive.read("native/certs/cacert.pem") == (root / "vendor/sources/cacert.pem").read_bytes()
        for lic_name in ("LICENSE.txt", "curl.txt", "openssl.txt"):
            lic_bytes = archive.read(f"native/licenses/{lic_name}")
            assert len(lic_bytes) > 64, f"Empty or truncated license file: {lic_name}"

        # Verify SHA-512 manifest digests for all packaged assets
        for block in manifest.split("\n\n"):
            data = dict(
                line.split(": ", 1)
                for line in block.replace("\n ", "").splitlines()
                if ": " in line
            )
            if "Archive-Asset-Name" in data:
                raw_asset = archive.read(data["Archive-Asset-Name"])
                digest = base64.urlsafe_b64encode(hashlib.sha512(raw_asset).digest()).decode("ascii").rstrip("=")
                assert digest == data["Archive-Asset-SHA-512-Digest"], (
                    f"Manifest SHA-512 digest mismatch for {data['Archive-Asset-Name']}"
                )

        # Verify ARM ELF binary and private TLS/curl symbol isolation
        elf_files = [n for n in names if not n.endswith("/") and archive.read(n).startswith(b"\x7fELF")]
        assert elf_files == ["native/todoist-passport"], f"Unexpected ELF files in BAR: {elf_files!r}"

        binary = archive.read("native/todoist-passport")
        assert binary[:4] == b"\x7fELF" and binary[4:6] == b"\x01\x01", "Expected 32-bit LSB ELF"
        assert struct.unpack_from("<H", binary, 18)[0] == 40, "Expected ARM (EM_ARM=40) ELF binary"

        shoff = struct.unpack_from("<I", binary, 32)[0]
        shsize, shnum = struct.unpack_from("<HH", binary, 46)
        sections = [struct.unpack_from("<10I", binary, shoff + i * shsize) for i in range(shnum)]
        forbidden_prefixes = (b"SSL_", b"OPENSSL_", b"EVP_", b"X509_", b"CRYPTO_", b"BIO_", b"TLS_", b"curl_")
        for section in sections:
            if section[1] != 11:  # SHT_DYNSYM
                continue
            strings = sections[section[6]]
            table = binary[strings[4] : strings[4] + strings[5]]
            for offset in range(section[4], section[4] + section[5], section[9]):
                name, _, _, info, visibility, defined = struct.unpack_from("<IIIBBH", binary, offset)
                symbol = table[name:].split(b"\0", 1)[0]
                if defined and (info >> 4) in (1, 2) and (visibility & 3) == 0:
                    assert not symbol.startswith(forbidden_prefixes), (
                        f"Private TLS/curl symbol leaked in BAR ELF: {symbol!r}"
                    )

    print("OK: todoist-passport.bar verified (armle-v7, permissions, QML, 144px icon, CA bundle, SHA-512 digests, 0 private TLS symbols)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
