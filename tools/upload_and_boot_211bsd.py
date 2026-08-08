#!/usr/bin/env python3
"""Upload pdpconfig-211bsd.ini and run one boot capture."""

from __future__ import annotations

import subprocess
import sys
from ftplib import FTP
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = "192.168.7.144"
LOCAL = ROOT / "PdpSdCard" / "pdpconfig-211bsd.ini"


def ftp_upload() -> None:
    creds = [("esp32", "esp32"), ("anonymous", ""), ("anonymous", "anonymous")]
    last = None
    for user, passwd in creds:
        ftp = FTP()
        try:
            ftp.connect(HOST, 21, timeout=15)
            ftp.login(user or "anonymous", passwd)
            ftp.set_pasv(True)
            with LOCAL.open("rb") as fh:
                ftp.storbinary("STOR /pdpconfig-211bsd.ini", fh)
            print(f"FTP uploaded as {user!r} ({LOCAL.stat().st_size} bytes)", flush=True)
            ftp.quit()
            return
        except Exception as exc:
            last = exc
            try:
                ftp.close()
            except Exception:
                pass
    raise SystemExit(f"FTP upload failed: {last}")


def main() -> int:
    ftp_upload()
    return subprocess.call([sys.executable, str(ROOT / "tools" / "boot_211bsd_once.py")])


if __name__ == "__main__":
    raise SystemExit(main())
