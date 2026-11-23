#!/usr/bin/env python3
"""下载并解包 Espressif 官方 ESP32-C3 AT 固件到 firmware/。

    python tools/fetch_at.py            # 下载全部推荐版本
    python tools/fetch_at.py 2.4.2      # 只下载指定版本
"""
from __future__ import annotations

import os
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW_ROOT = os.path.join(ROOT, "firmware")
BASE = "https://dl.espressif.com/esp-at/firmwares/esp32c3/"

# 版本 -> 压缩包名。4.1.1 / 3.3 是 4MB 布局；2.4.2 的 app 较小，2MB Flash 可用。
RELEASES = {
    "4.1.1": "ESP32-C3-MINI-1-AT-V4.1.1.0.zip",
    "3.3": "ESP32-C3-MINI-1-AT-V3.3.0.0.zip",
    "2.4.2": "ESP32-C3-MINI-1-AT-V2.4.2.0.zip",
}


def fetch(version: str, name: str) -> None:
    os.makedirs(FW_ROOT, exist_ok=True)
    zip_path = os.path.join(FW_ROOT, name)
    if not os.path.exists(zip_path):
        url = BASE + name
        print(f"下载 {url}")
        with urllib.request.urlopen(url, timeout=120) as r, open(zip_path, "wb") as f:
            total = int(r.headers.get("Content-Length", 0))
            done = 0
            while chunk := r.read(1 << 16):
                f.write(chunk)
                done += len(chunk)
                if total:
                    print(f"\r  {done * 100 // total:3d}%  {done >> 20}MB", end="")
        print()
    print(f"解包 {name}")
    with zipfile.ZipFile(zip_path) as z:
        z.extractall(FW_ROOT)
    os.remove(zip_path)


def main() -> None:
    wanted = sys.argv[1:] or list(RELEASES)
    for short in wanted:
        if short not in RELEASES:
            print(f"未知版本 {short}，可选: {', '.join(RELEASES)}", file=sys.stderr)
            sys.exit(1)
    for short in wanted:
        fetch(short, RELEASES[short])


if __name__ == "__main__":
    main()
