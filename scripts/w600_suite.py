#!/usr/bin/env python3
"""TW-03 / W600 模块一键性能基线 + 多模块对比。

这块 AT 固件的 socket 状态机不健壮：多轮 open/close 后会残留（`link is not valid`），
所以每组测试前都 AT+RST + 自动重连，保证每个数字都来自干净状态。

    python scripts/w600_suite.py --ssid betwowt --pass xiaobudian1314520 --name A
    python scripts/w600_suite.py --ssid betwowt --pass xiaobudian1314520 --name B
    python scripts/w600_suite.py --compare

数据经 UART 传输，921600 8N1 的上限是 90 KB/s，报告里会给出利用率。
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import socket
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULT_DIR = os.path.join(ROOT, "results")
UDP_PORT = 8888
BAUDS = (115200, 460800, 921600)
MQTT_CONNECT = bytes.fromhex("101600044d5154540402003c000a77363030746573743031")


def local_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("223.5.5.5", 80))
        return s.getsockname()[0]
    finally:
        s.close()


class Mod:
    def __init__(self, port: str):
        self.port = port
        self.s: serial.Serial | None = None
        self.baud = None
        self.leftover = b""
        for baud in BAUDS:
            s = serial.Serial(port, baud, timeout=0.005)
            s.dtr = False
            s.rts = False
            self.s, self.baud = s, baud
            if b"OK" in self.cmd("AT", 0.6):
                return
            s.close()
        raise SystemExit(f"{port} 上模块无响应（{BAUDS} 都试过）")

    # ---- 基础 IO
    def drain(self, sec: float) -> bytes:
        t = time.time()
        b = b""
        while time.time() - t < sec:
            b += self.s.read(1 << 16)
        return b

    def cmd(self, line: str, wait: float = 0.8) -> bytes:
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\r\n")
        return self.drain(wait)

    def wait_for(self, token: bytes, timeout: float) -> bytes:
        t = time.time()
        b = b""
        while time.time() - t < timeout:
            b += self.s.read(1 << 16)
            if token in b:
                return b
        return b

    def cipsend(self, data: bytes, timeout: float = 3.0) -> bool:
        self.s.reset_input_buffer()
        self.s.write(f"AT+CIPSEND={len(data)}\r\n".encode())
        pre = self.wait_for(b">", 1.5)
        if b">" not in pre:
            self.leftover = pre            # 等 '>' 期间可能已经有 +IPD，不能丢
            return False
        self.s.write(data)
        r = self.wait_for(b"SEND OK", timeout)
        post = r.split(b"SEND OK", 1)[1] if b"SEND OK" in r else b""
        self.leftover = pre.split(b">", 1)[1] + post
        return b"SEND OK" in r

    def set_baud(self, baud: int):
        if baud == self.baud:
            return
        if b"OK" in self.cmd(f"AT+UART={baud},8,1,0,0", 1.0):
            self.s.close()
            time.sleep(0.2)
            self.s = serial.Serial(self.port, baud, timeout=0.005)
            self.s.dtr = False
            self.s.rts = False
            self.baud = baud

    # ---- 复位 + 回到可测状态
    def reinit(self, ssid: str | None, password: str | None) -> dict:
        self.s.write(b"AT+RST\r\n")
        time.sleep(2.2)
        self.drain(0.6)
        if b"OK" not in self.cmd("AT", 0.8):
            # 复位后波特率可能回到默认，重新找
            self.s.close()
            for baud in BAUDS:
                s = serial.Serial(self.port, baud, timeout=0.005)
                s.dtr = False
                s.rts = False
                self.s, self.baud = s, baud
                if b"OK" in self.cmd("AT", 0.6):
                    break
            else:
                raise SystemExit("复位后模块失联")
        self.set_baud(921600)
        self.cmd("ATE0", 0.4)
        mode = self.cmd("AT+CWMODE?", 0.6).decode("utf-8", "replace")
        if (m := re.search(r"\+CWMODE:(\d)", mode)) and m.group(1) != "1":
            self.cmd("AT+CWMODE=1", 1.2)
            self.s.write(b"AT+RST\r\n")
            time.sleep(2.2)
        cifsr = self.cmd("AT+CIFSR", 1.0).decode("utf-8", "replace")

        def joined(txt: str) -> bool:
            m = re.search(r'STAIP,"([\d.]+)"', txt)
            return bool(m) and m.group(1) != "0.0.0.0"

        if not joined(cifsr) and ssid:
            self.cmd(f'AT+CWJAP="{ssid}","{password}"', 18)
            cifsr = self.cmd("AT+CIFSR", 1.0).decode("utf-8", "replace")
        if not joined(cifsr):
            raise SystemExit(f"模块没连上 AP：{cifsr[:200]}")
        return {"cifsr": cifsr, "wjap": self.cmd("AT+CWJAP?", 1.0).decode("utf-8", "replace")}


class Ipd:
    def __init__(self, mod: Mod, seed: bytes = b""):
        self.mod = mod
        self.total = 0
        self.first = b""
        self.data = b""
        self.buf = seed

    def pump(self, sec: float = 0.0):
        t = time.time()
        while True:
            self.buf += self.mod.s.read(1 << 16)
            while True:
                m = re.match(rb"\+IPD,(\d+):", self.buf)
                if not m:
                    i = self.buf.find(b"+IPD,")
                    if i > 0:
                        self.buf = self.buf[i:]
                        continue
                    break
                n = int(m.group(1))
                st = m.end()
                if len(self.buf) < st + n:
                    break
                if not self.first:
                    self.first = self.buf[st:st + n]
                if len(self.data) < 8192:
                    self.data += self.buf[st:st + n]
                self.buf = self.buf[st + n:]
                self.total += n
            if time.time() - t >= sec:
                return


def ping(mod: Mod, host: str, n: int = 5) -> dict:
    rtts = []
    for _ in range(n):
        mod.s.reset_input_buffer()
        mod.s.write(f'AT+PING="{host}"\r\n'.encode())
        r = mod.wait_for(b"OK", 2.5)
        m = re.search(rb"\+(\d+)", r)
        if m:
            rtts.append(int(m.group(1)))
    return {"n": n, "ok": len(rtts),
            "min": min(rtts) if rtts else None,
            "avg": round(sum(rtts) / len(rtts), 1) if rtts else None,
            "max": max(rtts) if rtts else None}


def tcp_download(mod: Mod, host="mirrors.aliyun.com", path="/ubuntu/ls-lR.gz",
                 sec=5.0) -> dict:
    r = mod.cmd(f'AT+CIPSTART="TCP","{host}",80', 8)
    if b"OK" not in r:
        return {"ok": False}
    req = (f"GET {path} HTTP/1.0\r\nHost: {host}\r\n"
           f"User-Agent: w600-bench\r\n\r\n").encode()
    t0 = time.time()
    if not mod.cipsend(req):
        return {"ok": False}
    rd = Ipd(mod, seed=mod.leftover)
    while time.time() - t0 < sec:
        rd.pump(0.1)
    dt = time.time() - t0
    mod.cmd("AT+CIPCLOSE", 0.6)
    return {"ok": True, "kbs": round(rd.total / dt / 1024, 1),
            "head": rd.first[:40].decode("latin1")}


def tls_check(mod: Mod) -> dict:
    r = mod.cmd('AT+CIPSTART="SSL","www.baidu.com",443', 8)
    if b"OK" not in r:
        return {"ok": False}
    mod.cipsend(b"GET / HTTP/1.0\r\nHost: www.baidu.com\r\nUser-Agent: w600-bench\r\n\r\n")
    rd = Ipd(mod, seed=mod.leftover)
    t0 = time.time()
    while time.time() - t0 < 6:
        rd.pump(0.2)
        if b"HTTP/" in rd.data:
            break
    mod.cmd("AT+CIPCLOSE", 0.6)
    return {"ok": True, "http_inside_tls": b"HTTP/" in rd.data}


def mqtt(mod: Mod, host="test.mosquitto.org") -> dict:
    r = mod.cmd(f'AT+CIPSTART="TCP","{host}",1883', 8)
    if b"OK" not in r:
        return {"ok": False}
    t0 = time.time()
    if not mod.cipsend(MQTT_CONNECT, 3):
        return {"ok": False}
    buf = mod.leftover
    while time.time() - t0 < 6:
        buf += mod.s.read(1 << 16)
        m = re.search(rb"\+IPD,(\d+):", buf)
        if m:
            n = int(m.group(1))
            st = m.end()
            if len(buf) >= st + n:
                ack = buf[st:st + n]
                mod.cmd("AT+CIPCLOSE", 0.5)
                return {"ok": True, "ms": round((time.time() - t0) * 1000),
                        "connack": ack.hex()}
    mod.cmd("AT+CIPCLOSE", 0.5)
    return {"ok": False}


def udp_down(mod: Mod, sock: socket.socket, module_ip: str,
             tiers=(10, 20, 30, 40), pkt=1400, sec=2.0) -> list[dict]:
    if b"OK" not in mod.cmd(f'AT+CIPSTART="UDP","{local_ip()}",{UDP_PORT},{UDP_PORT}', 6):
        return []
    out = []
    for kb_s in tiers:
        rd = Ipd(mod)
        t0 = time.time()
        sent = 0
        interval = pkt / (kb_s * 1024)
        nxt = t0
        while time.time() - t0 < sec:
            sock.sendto(b"\x5a" * pkt, (module_ip, UDP_PORT))
            sent += pkt
            rd.pump()
            nxt += interval
            d = nxt - time.time()
            if d > 0:
                time.sleep(d)
        rd.pump(0.3)
        dt = time.time() - t0
        out.append({"target_kbs": kb_s,
                    "sent_kbs": round(sent / dt / 1024, 1),
                    "recv_kbs": round(rd.total / dt / 1024, 1),
                    "loss_pct": round((1 - rd.total / max(1, sent)) * 100, 1)})
    mod.cmd("AT+CIPCLOSE", 0.6)
    return out


def udp_up(mod: Mod, sock: socket.socket, module_ip: str, sec=3.0, chunk=2048) -> dict:
    if b"OK" not in mod.cmd(f'AT+CIPSTART="UDP","{local_ip()}",{UDP_PORT},{UDP_PORT}', 6):
        return {"ok": False}
    sock.sendto(b"prime", (module_ip, UDP_PORT))   # 让防火墙放行回程
    time.sleep(0.2)
    sock.setblocking(False)
    t0 = time.time()
    sent = recv = 0
    while time.time() - t0 < sec:
        if not mod.cipsend(b"\x5a" * chunk, 2.0):
            break
        sent += chunk
        try:
            while True:
                recv += len(sock.recvfrom(65536)[0])
        except (BlockingIOError, socket.timeout):
            pass
    dt = time.time() - t0
    mod.cmd("AT+CIPCLOSE", 0.6)
    return {"ok": True, "sent_kbs": round(sent / dt / 1024, 1),
            "recv_kbs": round(recv / dt / 1024, 1)}


def run_suite(port: str, ssid: str | None, password: str | None) -> dict:
    mod = Mod(port)
    info: dict = {"port": port, "pc_ip": local_ip(),
                  "ts": time.strftime("%Y-%m-%d %H:%M:%S")}

    st = mod.reinit(ssid, password)
    info["baud"] = mod.baud
    info["gmr"] = " ".join(mod.cmd("AT+GMR", 1.0).decode("utf-8", "replace").split())[:120]
    cifsr = st["cifsr"]
    info["module_ip"] = re.search(r'STAIP,"([\d.]+)"', cifsr).group(1)
    mac = re.search(r'STAMAC,"([0-9a-fA-F:]{17})"', cifsr)
    info["mac"] = mac.group(1).lower() if mac else "unknown"
    if (m := re.search(r'\+CWJAP:"([^"]*)","([^"]*)",(\d+),(-?\d+)', st["wjap"])):
        info.update(ssid=m.group(1), bssid=m.group(2), channel=int(m.group(3)),
                    rssi=int(m.group(4)))
    info["ping_gw"] = ping(mod, "192.168.8.1")
    info["ping_inet"] = ping(mod, "8.8.8.8")

    mod.reinit(ssid, password)
    info["tcp_down"] = tcp_download(mod)

    mod.reinit(ssid, password)
    info["tls"] = tls_check(mod)

    mod.reinit(ssid, password)
    info["mqtt"] = mqtt(mod)

    mod.reinit(ssid, password)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((info["pc_ip"], UDP_PORT))
    sock.settimeout(0.05)
    info["udp_down"] = udp_down(mod, sock, info["module_ip"])

    mod.reinit(ssid, password)
    info["udp_up"] = udp_up(mod, sock, info["module_ip"])
    sock.close()

    info["uart_cap_kbs"] = round(mod.baud / 10 / 1024, 1)
    mod.s.close()
    return info


def summarize(r: dict) -> None:
    print(f"MAC {r['mac']}  FW {r['gmr'][:60]}")
    print(f"  IP {r['module_ip']}  RSSI {r.get('rssi', '?')} dBm  CH {r.get('channel', '?')}  UART {r['baud']}")
    for k in ("ping_gw", "ping_inet"):
        p = r[k]
        print(f"  {k:10} {p['ok']}/{p['n']}  min/avg/max = {p['min']}/{p['avg']}/{p['max']} ms")
    for t in r["udp_down"]:
        print(f"  UDP↓ 目标 {t['target_kbs']:>2} KB/s -> 实发 {t['sent_kbs']:5.1f} 实收 {t['recv_kbs']:5.1f} KB/s 丢包 {t['loss_pct']:5.1f}%")
    u = r["udp_up"]
    print(f"  UDP↑ {u.get('sent_kbs', 0):5.1f} KB/s (AT 管道) / {u.get('recv_kbs', 0):5.1f} KB/s PC 收到")
    d = r["tcp_down"]
    print(f"  TCP↓ HTTP {d.get('kbs', 0):5.1f} KB/s  ({d.get('kbs', 0) / r['uart_cap_kbs'] * 100:.0f}% UART)  首包 {d.get('head', '')!r}")
    print(f"  MQTT 握手 {r['mqtt'].get('ms', '-')} ms  CONNACK={r['mqtt'].get('connack', '-')}")
    print(f"  TLS  CONNECT={r['tls'].get('ok')}  明文 HTTP 回包={r['tls'].get('http_inside_tls')}")


def compare() -> None:
    files = sorted(glob.glob(os.path.join(RESULT_DIR, "*.json")))
    if len(files) < 2:
        raise SystemExit("至少需要两次结果才能对比")
    a, b = json.load(open(files[-2], encoding="utf-8")), json.load(open(files[-1], encoding="utf-8"))
    print(f"A: {a['mac']} {a['ts']}\nB: {b['mac']} {b['ts']}\n")
    rows = [
        ("RSSI dBm", a.get("rssi"), b.get("rssi")),
        ("PING gw avg ms", a["ping_gw"]["avg"], b["ping_gw"]["avg"]),
        ("PING inet avg ms", a["ping_inet"]["avg"], b["ping_inet"]["avg"]),
        ("UDP↓ 40 档收 KB/s", a["udp_down"][-1]["recv_kbs"] if a["udp_down"] else 0,
         b["udp_down"][-1]["recv_kbs"] if b["udp_down"] else 0),
        ("UDP↓ 40 档丢包 %", a["udp_down"][-1]["loss_pct"] if a["udp_down"] else 0,
         b["udp_down"][-1]["loss_pct"] if b["udp_down"] else 0),
        ("UDP↑ KB/s", a["udp_up"].get("sent_kbs", 0), b["udp_up"].get("sent_kbs", 0)),
        ("TCP↓ KB/s", a["tcp_down"].get("kbs", 0), b["tcp_down"].get("kbs", 0)),
        ("MQTT ms", a["mqtt"].get("ms", 0), b["mqtt"].get("ms", 0)),
        ("TLS ok", a["tls"].get("http_inside_tls"), b["tls"].get("http_inside_tls")),
    ]
    print(f"{'指标':20} {'A':>10} {'B':>10} {'差':>18}")
    for name, x, y in rows:
        delta = ""
        if isinstance(x, (int, float)) and isinstance(y, (int, float)) and x:
            delta = f"{y - x:+.1f}"
            if x > 0:  # RSSI 是负数，百分比没有意义
                delta += f" ({(y - x) / x * 100:+.0f}%)"
        print(f"{name:20} {str(x):>10} {str(y):>10} {delta:>18}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    ap.add_argument("--ssid")
    ap.add_argument("--pass", dest="password")
    ap.add_argument("--compare", action="store_true")
    ap.add_argument("--name", help="结果后缀，如 A / B")
    args = ap.parse_args()

    os.makedirs(RESULT_DIR, exist_ok=True)
    if args.compare:
        compare()
        return

    r = run_suite(args.port, args.ssid, args.password)
    summarize(r)
    suffix = f"_{args.name}" if args.name else ""
    path = os.path.join(RESULT_DIR, f"{r['mac'].replace(':', '')}{suffix}_{int(time.time())}.json")
    json.dump(r, open(path, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
    print(f"\n结果已保存: {path}")
    print("换另一块模块后重跑，再执行: python scripts/w600_suite.py --compare")


if __name__ == "__main__":
    main()
