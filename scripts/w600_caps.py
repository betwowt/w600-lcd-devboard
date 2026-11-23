#!/usr/bin/env python3
"""TW-03 / W600（ThingsTurn AT v1.2.0）能力探测：TCP / DNS / HTTP / MQTT / TLS。

    python scripts/w600_caps.py --port COM10 --baud 921600
"""
from __future__ import annotations

import argparse
import re
import socket
import time

import serial

MQTT_CONNECT = bytes.fromhex(
    "10 16 0004 4d515454 04 02 003c 000a 77363030746573743031"
).replace(b" ", b"")


class Mod:
    def __init__(self, port: str, baud: int):
        self.s = serial.Serial(port, baud, timeout=0.02)
        self.s.dtr = False
        self.s.rts = False

    def drain(self, sec: float) -> bytes:
        t = time.time()
        b = b""
        while time.time() - t < sec:
            b += self.s.read(1 << 20)
        return b

    def cmd(self, line: str, wait: float = 1.0) -> bytes:
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\r\n")
        return self.drain(wait)

    def wait_for(self, token: bytes, timeout: float) -> bytes:
        t = time.time()
        b = b""
        while time.time() - t < timeout:
            b += self.s.read(1 << 20)
            if token in b:
                return b
        return b

    def cipsend(self, data: bytes, timeout: float = 3.0) -> bytes:
        self.s.reset_input_buffer()
        self.s.write(f"AT+CIPSEND={len(data)}\r\n".encode())
        if b">" not in self.wait_for(b">", 1.5):
            return b"NO_PROMPT"
        self.s.write(data)
        return self.wait_for(b"SEND OK", timeout)


def read_ipd(mod: Mod, seconds: float) -> tuple[int, bytes]:
    """读取 +IPD，返回 (字节数, 原始片段)。"""
    n = 0
    first = b""
    buf = b""
    t = time.time()
    while time.time() - t < seconds:
        buf += mod.s.read(1 << 20)
        while True:
            m = re.match(rb"\+IPD,(\d+):", buf)
            if not m:
                i = buf.find(b"+IPD,")
                if i > 0:
                    buf = buf[i:]
                break
            ln = int(m.group(1))
            start = m.end()
            if len(buf) < start + ln:
                break
            if not first:
                first = buf[start:start + ln]
            buf = buf[start + ln:]
            n += ln
    return n, first


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    m = Mod(args.port, args.baud)
    m.cmd("ATE0", 0.5)

    print("== 命令集探测")
    for q in ("AT+HTTPCLIENT?", "AT+HTTPGET?", "AT+MQTTUSERCFG?", "AT+CIPSSLCCONF?",
              "AT+CIPSERVER=?", "AT+CIPSTART?", "AT+CIPRECVMODE?", "AT+CIPMUX?",
              "AT+CIPDNS?", "AT+CIPSTA?", "AT+MDNS?"):
        r = m.cmd(q, 0.7).decode("utf-8", "replace").replace("\r", "").strip()
        r = " ".join(r.split())[:80]
        print(f"  {q:22} -> {r}")

    print("\n== 网络")
    print("  PING 8.8.8.8 :", m.cmd('AT+PING="8.8.8.8"', 4).decode("utf-8", "replace").replace("\r", "").strip())
    r = m.cmd('AT+CIPSTART="TCP","www.baidu.com",80', 8)
    print("  TCP baidu:80 (域名) :", r.decode("utf-8", "replace").replace("\r", "").strip()[:120])
    if b"OK" not in r:
        r = m.cmd('AT+CIPSTART="TCP","39.156.66.10",80', 8)
        print("  TCP 39.156.66.10:80 :", r.decode("utf-8", "replace").replace("\r", "").strip()[:120])
    if b"OK" in r:
        req = b"GET / HTTP/1.0\r\nHost: www.baidu.com\r\nConnection: close\r\n\r\n"
        print("  HTTP GET:", m.cipsend(req).decode("utf-8", "replace").replace("\r", "").strip()[:80])
        n, first = read_ipd(m, 4)
        print(f"  HTTP 收到 {n} B, 首行: {first.splitlines()[0][:60] if first else b'-'}")
        m.cmd("AT+CIPCLOSE", 1)

    print("\n== MQTT (broker 1883, 纯 TCP 手写 CONNECT)")
    for host in ("test.mosquitto.org", "mqtt.eclipseprojects.io"):
        r = m.cmd(f'AT+CIPSTART="TCP","{host}",1883', 8)
        ok = b"OK" in r
        print(f"  {host}: {r.decode('utf-8','replace').replace(chr(13),'').strip()[:60]}")
        if not ok:
            continue
        print("  CIPSEND:", m.cipsend(MQTT_CONNECT).decode("utf-8", "replace").replace("\r", "").strip()[:60])
        n, first = read_ipd(m, 4)
        print(f"  CONNACK: {first.hex()} ({'成功' if first[:4] == b'\\x20\\x02\\x00\\x00' else '见上'})")
        m.cmd("AT+CIPCLOSE", 1)
        break

    print("\n== TLS")
    r = m.cmd('AT+CIPSTART="SSL","www.baidu.com",443', 6)
    print("  SSL connect:", r.decode("utf-8", "replace").replace("\r", "").strip()[:100])
    m.cmd("AT+CIPCLOSE", 1)


if __name__ == "__main__":
    main()
