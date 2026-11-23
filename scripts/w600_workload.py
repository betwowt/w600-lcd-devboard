#!/usr/bin/env python3
"""按"HTTP + MQTT 物联网终端"的实际用法压测 TW-03 / W600。

    python scripts/w600_workload.py --ssid betwowt --pass xiaobudian1314520

测项：
  HTTP  连续 N 次 TCP+GET（每次新建连接），统计成功率、首字节延迟分布
  MQTT  CONNECT/SUBSCRIBE 后连续发布 N 条，统计回环成功率、发布速率、单条往返延迟
"""
from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import w600_suite as W  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULT_DIR = os.path.join(ROOT, "results")


def one_http(mod: W.Mod, host: str, path: str) -> float | None:
    t0 = time.time()
    mod.s.reset_input_buffer()
    mod.s.write(f'AT+CIPSTART="TCP","{host}",80\r\n'.encode())
    r = mod.wait_for(b"OK", 6)
    if b"OK" not in r or b"CONNECT" not in r:
        return None
    req = (f"GET {path} HTTP/1.0\r\nHost: {host}\r\n"
           f"User-Agent: w600-workload\r\n\r\n").encode()
    if not mod.cipsend(req):
        return None
    rd = W.Ipd(mod, seed=mod.leftover)
    while time.time() - t0 < 6:
        rd.pump(0.05)
        if b"HTTP/" in rd.data:
            return (time.time() - t0) * 1000
    return None


def http_requests(mod: W.Mod, host: str, n: int, path="/") -> dict:
    lat = []
    for i in range(n):
        ttfb = one_http(mod, host, path)
        if ttfb is None:
            # 这块固件多轮 socket 复用会残留，复位后重试一次
            mod.cmd("AT+CIPCLOSE", 0.4)
            mod.reinit(SSID, PASS)
            ttfb = one_http(mod, host, path)
        if ttfb is not None:
            lat.append(round(ttfb))
        mod.s.reset_input_buffer()
        mod.s.write(b"AT+CIPCLOSE\r\n")
        mod.wait_for(b"CLOSED", 1.0)
        time.sleep(0.3)
    ok = len(lat)
    return {"n": n, "ok": ok, "fail": n - ok,
            "ttfb_ms": {"min": min(lat) if lat else None,
                        "avg": round(statistics.mean(lat), 1) if lat else None,
                        "max": max(lat) if lat else None}}


def mqtt_stress(mod: W.Mod, host: str, n: int, topic="t/w600") -> dict:
    pack = bytearray()
    pack += b"\x10\x00"  # CONNECT 占位，长度下面算
    var = b"\x00\x04MQTT\x04\x02\x00\x3c"
    cid = b"\x00\x0aw600work01"
    body = var + cid
    pack = b"\x10" + bytes([len(body)]) + body
    if b"OK" not in mod.cmd(f'AT+CIPSTART="TCP","{host}",1883', 8):
        return {"ok": False}
    t0 = time.time()
    if not mod.cipsend(pack):
        return {"ok": False}
    while time.time() - t0 < 6:
        buf = mod.leftover + mod.s.read(65536)
        mod.leftover = buf
        if b"\x20\x02\x00\x00" in buf:
            break
    else:
        return {"ok": False, "err": "no CONNACK"}

    tb = topic.encode()
    sub_body = b"\x00\x01" + bytes([len(tb) >> 8, len(tb) & 0xFF]) + tb + b"\x00"
    if not mod.cipsend(b"\x82" + bytes([len(sub_body)]) + sub_body):
        return {"ok": False}
    time.sleep(0.5)
    mod.s.reset_input_buffer()

    def send(pkt: bytes) -> bool:
        """模块在异步上报 +IPD 时会回 busy，CIPSEND 拿不到 '>'，需要重试。"""
        for _ in range(3):
            if mod.cipsend(pkt, 2.0):
                return True
            mod.drain(0.2)
        return False

    payload = b"x" * 24
    pkt = b"\x30" + bytes([2 + len(tb) + len(payload)]) + \
        bytes([len(tb) >> 8, len(tb) & 0xFF]) + tb + payload

    rtts = []
    for _ in range(min(5, n)):           # 前 5 条逐条等回环，取 RTT
        t1 = time.time()
        if not send(pkt):
            break
        buf = mod.leftover
        deadline = time.time() + 2
        while time.time() < deadline:
            buf += mod.s.read(65536)
            if b"x" * 24 in buf:
                rtts.append(round((time.time() - t1) * 1000, 1))
                break
        mod.leftover = b""
    mod.s.reset_input_buffer()

    # 背靠背发布，统计管道速率和回环成功率
    collected = bytearray()

    def collect():
        while True:
            d = mod.s.read(1 << 16)
            if not d:
                break
            collected.extend(d)

    sent = 0
    t0 = time.time()
    for _ in range(n):
        collect()                      # CIPSEND 会清缓冲，先把异步 +IPD 收走
        if not send(pkt):
            break
        sent += 1
    send_elapsed = time.time() - t0
    t_end = time.time() + 3
    while time.time() < t_end:
        collect()
    echoed = bytes(collected).count(b"x" * 24)
    mod.cmd("AT+CIPCLOSE", 0.4)
    return {"ok": True, "n": n, "sent": sent, "echoed": echoed,
            "msg_per_s": round(sent / send_elapsed, 1),
            "rtt_ms": {"min": min(rtts) if rtts else None,
                       "avg": round(statistics.mean(rtts), 1) if rtts else None,
                       "max": max(rtts) if rtts else None}}


def main():
    global SSID, PASS
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--pass", dest="password", required=True)
    ap.add_argument("--http-host", default="www.baidu.com")
    ap.add_argument("--http-n", type=int, default=20)
    ap.add_argument("--mqtt-host", default="test.mosquitto.org")
    ap.add_argument("--mqtt-n", type=int, default=50)
    args = ap.parse_args()
    SSID, PASS = args.ssid, args.password

    mod = W.Mod(args.port)
    mod.reinit(SSID, PASS)
    cifsr = mod.cmd("AT+CIFSR", 1.0).decode("utf-8", "replace")
    mac = re.search(r'STAMAC,"([0-9a-fA-F:]{17})"', cifsr)
    mac = mac.group(1).lower() if mac else "unknown"

    mod.reinit(SSID, PASS)
    http = http_requests(mod, args.http_host, args.http_n)

    mod.reinit(SSID, PASS)
    mqtt = mqtt_stress(mod, args.mqtt_host, args.mqtt_n)

    print(f"MAC {mac}")
    print(f"HTTP  {http['ok']}/{http['n']} 成功, TTFB min/avg/max = "
          f"{http['ttfb_ms']['min']}/{http['ttfb_ms']['avg']}/{http['ttfb_ms']['max']} ms")
    print(f"MQTT  发布 {mqtt.get('sent')} 条 ({mqtt.get('msg_per_s')} 条/s), 回环 {mqtt.get('echoed')} 条, "
          f"RTT min/avg/max = "
          f"{mqtt.get('rtt_ms', {}).get('min')}/{mqtt.get('rtt_ms', {}).get('avg')}/"
          f"{mqtt.get('rtt_ms', {}).get('max')} ms")

    os.makedirs(RESULT_DIR, exist_ok=True)
    path = os.path.join(RESULT_DIR, f"{mac.replace(':', '')}_workload_{int(time.time())}.json")
    json.dump({"mac": mac, "http": http, "mqtt": mqtt,
               "ts": time.strftime("%Y-%m-%d %H:%M:%S")},
              open(path, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
    print("结果已保存:", path)


if __name__ == "__main__":
    main()
