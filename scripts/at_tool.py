#!/usr/bin/env python3
"""ESP-AT 烧录工具（ESP32-C3）：探测芯片 -> 选匹配的 ESP-AT 固件 -> 烧录 -> AT 自检。

目前覆盖 ESP32-C3（4MB 与 2MB flash）。其它芯片会被识别但拒绝烧录，
避免把 ESP-AT 写进非 C3 的芯片。

用法:
    python scripts/at_tool.py detect            # 探测芯片/Flash/MAC
    python scripts/at_tool.py flash             # 自动选固件并烧录
    python scripts/at_tool.py flash --no-erase  # 保留 NVS/用户分区
    python scripts/at_tool.py at                # 发 AT+GMR 自检
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW_ROOT = os.path.join(ROOT, "firmware")
PT_2MB = os.path.join(ROOT, "partitions", "partitions_at_2mb.csv")

FLASH_SIZE_ENUM = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4, 32: 5, 64: 6, 128: 7}
PART_TYPE = {"app": 0x00, "data": 0x01}
PART_SUBTYPE = {
    "ota": 0x00, "phy": 0x01, "nvs": 0x02, "nvs_keys": 0x04, "fat": 0x81,
    "factory": 0x00, "test": 0x20, "ota_0": 0x10, "ota_1": 0x11,
}


def die(msg: str, code: int = 1):
    print(f"错误: {msg}", file=sys.stderr)
    sys.exit(code)


def esptool(*args: str, timeout: int = 180) -> str:
    cmd = [sys.executable, "-m", "esptool", *args]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        die(f"esptool 超时: {' '.join(args)}")
    out = (p.stdout or "") + (p.stderr or "")
    if p.returncode != 0:
        die(f"esptool 失败:\n{out.strip()}")
    return out


def list_ports() -> list[str]:
    import serial.tools.list_ports as lp
    return [p.device for p in lp.comports()]


def identify(port: str) -> dict:
    out = esptool("--port", port, "--before", "default_reset", "flash_id")
    chip = re.search(r"Chip is (ESP32-[A-Z0-9-]+|ESP8266|ESP32)", out)
    flash = re.search(r"Detected flash size:\s*(\d+)\s*(MB|KB)", out)
    mac = re.search(r"MAC:\s*([0-9a-f:]{17})", out, re.I)
    if not chip:
        die(f"{port} 上没检测到乐鑫芯片:\n{out.strip()}")
    mb = None
    if flash:
        mb = int(flash.group(1)) if flash.group(2) == "MB" else max(1, int(flash.group(1)) // 1024)
    return {"port": port, "chip": chip.group(1), "flash_mb": mb,
            "mac": mac.group(1) if mac else "?", "raw": out}


def pick_port(preferred: str | None) -> str:
    if preferred:
        return preferred
    ports = list_ports()
    if not ports:
        die("没有可用串口")
    for port in ports:
        try:
            info = identify(port)
        except SystemExit:
            continue
        print(f"[auto] {port}: {info['chip']} / {info['flash_mb']}MB")
        return port
    die(f"没有串口能识别出乐鑫芯片: {ports}")


# ---------------------------------------------------------------- partition table

def _num(tok: str) -> int:
    tok = tok.strip()
    if tok.lower().startswith("0x"):
        return int(tok, 16)
    if tok.upper().endswith("K"):
        return int(tok[:-1]) * 1024
    if tok.upper().endswith("M"):
        return int(tok[:-1]) * 1024 * 1024
    return int(tok)


def build_partition_table(csv_path: str, flash_mb: int) -> bytes:
    """把 CSV 编译成 esp32 分区表二进制（含 MD5 条目，0xC00 对齐）。"""
    entries = b""
    rows = []
    with open(csv_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, ptype, subtype, offset, size = [x.strip() for x in line.split(",")]
            t = PART_TYPE.get(ptype, _num(ptype) if re.match(r"^(0x|\d)", ptype) else None)
            st = PART_SUBTYPE.get(subtype, _num(subtype) if re.match(r"^(0x|\d)", subtype) else None)
            if t is None or st is None:
                die(f"分区表未知类型: {line}")
            label = name.encode()
            if len(label) > 16:
                die(f"分区名超过 16 字节: {name}")
            rows.append((_num(offset), _num(size), t, st, label, name))

    rows.sort(key=lambda r: r[0])
    prev_end = 0
    for off, size, t, st, label, name in rows:
        if off % 0x1000:
            die(f"分区 {name} 偏移 0x{off:x} 未 4KB 对齐")
        if t == 0 and off % 0x10000:
            die(f"app 分区 {name} 偏移 0x{off:x} 未 64KB 对齐")
        if off < prev_end:
            die(f"分区 {name} 与前一分区重叠")
        if off + size > flash_mb * 1024 * 1024:
            die(f"分区 {name} 结束于 0x{off + size:x}，超出 {flash_mb}MB Flash")
        prev_end = off + size
        entries += struct.pack("<HBBII16sI", 0x50AA, t, st, off, size, label, 0)

    md5 = hashlib.md5(entries).digest()
    table = entries + b"\xeb\xeb" + b"\x00" * 14 + md5
    return table.ljust(0xC00, b"\xff")


def patch_image_flash_size(src: str, dst: str, flash_mb: int) -> None:
    """把镜像头 byte3 的 flash size 改掉，避免 4MB 构建在 2MB 芯片上报错。"""
    data = bytearray(open(src, "rb").read())
    if not data or data[0] != 0xE9:
        die(f"{src} 不是 ESP 镜像")
    data[3] = (data[3] & 0xF0) | FLASH_SIZE_ENUM[flash_mb]
    open(dst, "wb").write(data)


# ---------------------------------------------------------------- firmware packages

def find_packages() -> list[dict]:
    pkgs = []
    for dirpath, _dirs, files in os.walk(FW_ROOT):
        if "download.config" in files and "esp-at.bin" in files:
            app = os.path.join(dirpath, "esp-at.bin")
            pkgs.append({
                "dir": dirpath,
                "app": app,
                "app_size": os.path.getsize(app),
                "factory": [os.path.join(dirpath, "factory", f)
                            for f in os.listdir(os.path.join(dirpath, "factory"))
                            if f.endswith(".bin")] if os.path.isdir(os.path.join(dirpath, "factory")) else [],
            })
    return pkgs


def parse_download_config(path: str) -> tuple[dict, list[tuple[int, str]]]:
    opts, items = {}, []
    toks = open(path, encoding="utf-8").read().split()
    i = 0
    while i < len(toks):
        tok = toks[i]
        if tok.startswith("--"):
            opts[tok.lstrip("-")] = toks[i + 1]
            i += 2
            continue
        items.append((int(tok, 16), toks[i + 1]))
        i += 2
    return opts, items


def choose_package(flash_mb: int) -> dict:
    """返回一个能塞进目标 Flash 的固件包。"""
    pkgs = [p for p in find_packages() if p["app_size"] + 0x60000 <= flash_mb * 1024 * 1024]
    if not pkgs:
        die(f"firmware/ 下没有能放进 {flash_mb}MB 的 ESP-AT 包，先跑 tools/fetch_at.py")
    pkgs.sort(key=lambda p: p["dir"], reverse=True)
    return pkgs[0]


def flash_c3(info: dict, erase: bool, pkg_arg: str | None, baud: str) -> None:
    port, mb = info["port"], info["flash_mb"]
    if info["chip"] != "ESP32-C3":
        die(f"只支持 ESP32-C3，检测到 {info['chip']}")

    if mb >= 4:
        pkg = None
        if pkg_arg:
            pkg = {"dir": pkg_arg, "factory": [os.path.join(pkg_arg, "factory", f)
                                               for f in os.listdir(os.path.join(pkg_arg, "factory"))
                                               if f.endswith(".bin")]}
        else:
            pkg = next((p for p in find_packages()
                        if any("unfilled" in f for f in p["factory"])), None)
        if pkg is None:
            die("没找到 4MB 官方固件包（含 factory/*_unfilled.bin），先跑 tools/fetch_at.py")
        bins = [f for f in pkg["factory"] if "unfilled" in f] or pkg["factory"]
        if not bins:
            die(f"{pkg['dir']} 里没有 factory bin")
        bin_path = sorted(bins)[0]
        print(f"烧录 {os.path.basename(bin_path)} -> 0x0 ({mb}MB Flash)")
        if erase:
            esptool("--port", port, "erase_flash")
        esptool("--port", port, "-b", baud, "--after", "hard_reset", "write_flash", "-z",
                "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", f"{mb}MB",
                "0x0", bin_path)
        return

    if mb != 2:
        die(f"不支持 {mb}MB Flash")

    pkg = choose_package(2)
    opts, items = parse_download_config(os.path.join(pkg["dir"], "download.config"))
    print(f"固件包: {os.path.basename(pkg['dir'])}  app={pkg['app_size'] / 1048576:.2f}MB")
    print(f"烧录 plan: 2MB 裁剪分区表 + 镜像头改为 2MB")

    build_dir = os.path.join(ROOT, "build", "2mb")
    os.makedirs(build_dir, exist_ok=True)
    table = build_partition_table(PT_2MB, 2)
    table_path = os.path.join(build_dir, "partition-table-2mb.bin")
    open(table_path, "wb").write(table)

    write_items = []
    for addr, rel in items:
        src = os.path.join(pkg["dir"], rel)
        if rel.endswith("partition_table/partition-table.bin"):
            write_items.append((addr, table_path))
            continue
        if rel in ("bootloader/bootloader.bin", "esp-at.bin"):
            dst = os.path.join(build_dir, os.path.basename(rel))
            patch_image_flash_size(src, dst, 2)
            src = dst
        if os.path.getsize(src) + addr > 2 * 1024 * 1024:
            die(f"{rel} 放在 0x{addr:x} 会超出 2MB")
        write_items.append((addr, src))

    if erase:
        print("擦除整片 Flash ...")
        esptool("--port", port, "erase_flash")
    args = ["--port", port, "-b", baud, "--after", "hard_reset", "write_flash", "-z",
            "--flash_mode", opts.get("flash_mode", "dio"),
            "--flash_freq", opts.get("flash_freq", "40m"),
            "--flash_size", "2MB"]
    for addr, path in sorted(write_items):
        args += [f"0x{addr:x}", path]
    esptool(*args)


def at_check(port: str, cmd: str = "AT+GMR", baud: int = 115200, wait: float = 1.0) -> str:
    import serial
    s = serial.Serial(port, baud, timeout=0.2)
    s.dtr = False
    s.rts = False
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(cmd.encode() + b"\r\n")
    deadline = time.time() + wait
    buf = b""
    while time.time() < deadline:
        buf += s.read(4096)
    s.close()
    return buf.decode("utf-8", "replace")


def main() -> None:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-p", "--port", help="串口，默认自动探测")

    ap = argparse.ArgumentParser(description="ESP32 AT 固件烧录工具 (ESP32-C3)")
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("detect", parents=[common], help="探测芯片/Flash/MAC")
    pf = sub.add_parser("flash", parents=[common], help="烧录匹配的 ESP-AT 固件")
    pf.add_argument("--no-erase", action="store_true", help="不擦除 Flash")
    pf.add_argument("--pkg", help="指定固件包目录")
    pf.add_argument("-b", "--baud", default="460800")
    pa = sub.add_parser("at", parents=[common], help="发送 AT 命令")
    pa.add_argument("command", nargs="?", default="AT+GMR", help="AT 命令")

    args = ap.parse_args()
    port = args.port or pick_port(None)

    if args.action == "detect":
        info = identify(port)
        print(f"端口     {info['port']}")
        print(f"芯片     {info['chip']}")
        print(f"Flash    {info['flash_mb']}MB")
        print(f"MAC      {info['mac']}")
        print(info["raw"].strip())
        return

    if args.action == "flash":
        info = identify(port)
        print(f"检测: {info['chip']} / {info['flash_mb']}MB @ {port}")
        flash_c3(info, erase=not args.no_erase, pkg_arg=args.pkg, baud=args.baud)
        print("烧录完成，AT 自检:")
        print(at_check(port, wait=3.0))
        return

    print(at_check(port, args.command))


if __name__ == "__main__":
    main()
