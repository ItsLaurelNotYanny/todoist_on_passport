#!/usr/bin/env python3
"""Send BAR package or file to BlackBerry Passport via LocalSend protocol (Headless CLI).

Interaction Flow:
1. Ensure LocalSend is opened on the BlackBerry Passport.
2. Run this script: python3 scripts/send-to-passport.py [path/to/package.bar]
3. The Passport screen will immediately pop up an authorization dialog: 'Accept / Decline'.
4. Tap 'Accept' on the Passport screen.
5. The package streams directly over Wi-Fi to the device.
6. Tap the received BAR file in Passport File Manager to install via BAR Opener.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TARGET = os.environ.get("PASSPORT_IP", "")
DEFAULT_PORT = 53317
def get_default_bar() -> Path:
    try:
        import xml.etree.ElementTree as ET
        bar_desc = ROOT / "bar-descriptor.xml"
        if bar_desc.is_file():
            tree = ET.parse(bar_desc)
            root_el = tree.getroot()
            el = root_el.find("{http://www.qnx.com/schemas/application/1.0}buildId")
            if el is not None and el.text:
                versioned = ROOT / f"build/bb10/todoist-passport-build-{el.text.strip()}.bar"
                if versioned.is_file():
                    return versioned
    except Exception:
        pass
    return ROOT / "build/bb10/todoist-passport.bar"


DEFAULT_BAR = get_default_bar()


DEFAULT_TIMEOUT = 300


def send_to_localsend(target_host: str, port: int, file_path: Path, timeout: int = DEFAULT_TIMEOUT):
    if not file_path.is_file():
        print(f"Error: file not found: {file_path}", file=sys.stderr)
        return False

    file_size = file_path.stat().st_size
    file_name = file_path.name
    file_id = hashlib.md5(f"{file_name}-{file_size}".encode()).hexdigest()[:12]

    print(f"Calculating SHA-256 for {file_name} ({file_size / (1024*1024):.2f} MB)...")
    hasher = hashlib.sha256()
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            hasher.update(chunk)
    file_sha = hasher.hexdigest()

    print(f"Connecting to LocalSend on BlackBerry Passport ({target_host}:{port})...")

    # Step 1: POST /api/localsend/v2/prepare-upload
    prep_req = {
        "info": {
            "alias": "Dev Machine (Headless)",
            "version": "2.2",
            "deviceModel": "Linux Workstation",
            "deviceType": "desktop",
            "fingerprint": "todoist-dev-cli",
            "port": port,
            "protocol": "http",
            "download": False,
        },
        "files": {
            file_id: {
                "id": file_id,
                "fileName": file_name,
                "size": file_size,
                "fileType": "application/octet-stream",
                "sha256": file_sha,
            }
        },
    }

    req_data = json.dumps(prep_req).encode("utf-8")
    prep_url = f"http://{target_host}:{port}/api/localsend/v2/prepare-upload"

    print("=" * 70)
    print(">>> Prompt triggered on device! Please tap [Accept] on your Passport <<<")
    print("=" * 70)

    try:
        req = urllib.request.Request(
            prep_url,
            data=req_data,
            headers={
                "Content-Type": "application/json",
                "Content-Length": str(len(req_data)),
            },
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            resp_body = resp.read()
            prep_resp = json.loads(resp_body.decode("utf-8"))
    except urllib.error.HTTPError as e:
        if e.code == 403:
            print(f"\n[!] Transfer request timed out or was declined on the BlackBerry Passport (HTTP 403).")
        else:
            print(f"\n[!] HTTP error from device ({target_host}:{port}): {e}")
        return False
    except urllib.error.URLError as e:
        print(f"\n[!] Cannot connect to LocalSend on device ({target_host}:{port}): {e}")
        print("Please verify:")
        print("  1. BlackBerry Passport screen is unlocked and on the same Wi-Fi network.")
        print("  2. LocalSend app is opened and running in the foreground on the Passport.")
        return False
    except Exception as e:
        print(f"\n[!] Handshake error: {e}")
        return False

    session_id = prep_resp.get("sessionId", "")
    file_tokens = prep_resp.get("files", {})
    token = file_tokens.get(file_id, "")

    if not token:
        print(f"[!] Device declined transfer or did not return token: {prep_resp}")
        return False

    print(f"[+] Device accepted offer! Starting transfer (Session: {session_id})...")

    # Step 2: POST /api/localsend/v2/upload streaming
    upload_url = (
        f"http://{target_host}:{port}/api/localsend/v2/upload"
        f"?sessionId={session_id}&fileId={file_id}&token={token}"
    )

    start_time = time.time()
    try:
        with open(file_path, "rb") as f_in:
            req = urllib.request.Request(
                upload_url,
                data=f_in,
                headers={
                    "Content-Type": "application/octet-stream",
                    "Content-Length": str(file_size),
                },
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                if resp.status == 200:
                    elapsed = max(0.001, time.time() - start_time)
                    speed_mbps = (file_size / (1024 * 1024)) / elapsed
                    print(
                        f"[SUCCESS] Transfer complete in {elapsed:.2f}s ({speed_mbps:.2f} MB/s)!"
                    )
                    print(
                        f"Saved to device downloads. Open '{file_name}' in File Manager to install via BAR Opener."
                    )
                    return True
                else:
                    print(f"[!] Transfer aborted with HTTP status: {resp.status}")
                    return False
    except Exception as e:
        print(f"[!] Transfer error: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Send BAR file to BlackBerry Passport via LocalSend"
    )
    parser.add_argument(
        "file",
        nargs="?",
        default=str(DEFAULT_BAR),
        help=f"Path to BAR file (default: {DEFAULT_BAR})",
    )
    parser.add_argument(
        "--target",
        default=DEFAULT_TARGET,
        help="Target Passport IP (default: PASSPORT_IP env var)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"Target LocalSend port (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=DEFAULT_TIMEOUT,
        help=f"Timeout in seconds waiting for user accept (default: {DEFAULT_TIMEOUT})",
    )
    args = parser.parse_args()

    if not args.target:
        parser.error("Target Passport IP is required. Provide --target <IP> or set PASSPORT_IP environment variable.")

    target_file = Path(args.file)
    ok = send_to_localsend(args.target, args.port, target_file, timeout=args.timeout)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
