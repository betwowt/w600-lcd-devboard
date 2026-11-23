#!/usr/bin/env python3
"""TW-03 / W600 长跑（soak）任务：一直跑，直到手动停止。

每轮：MQTT 发布 1 条并等回环；每 N 轮做一次 HTTP GET；每 M 轮采一次 RSSI。
所有进度写入 results/soak_<MAC>_<启动时间>.log（逐行落盘），
每 10 分钟覆盖写一份 results/soak_live.json 快照，随时可以查看。

自恢复：模块无响应 → AT+RST + 重连；串口异常 → 关闭后重开；
任何未捕获异常都只记录并继续，不会让任务退出。

    python scripts/w600_soak.py --ssid betwowt --pass xiaobudian1314520 --forever
    python scripts/w600_soak.py --minutes 30          # 定时模式
"""
from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import w600_suite as W  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULT_DIR = os.path.join(ROOT, "results")
TOPIC = b"t/w600"
PAYLOAD = b"x" * 24


def mqtt_packets(client_id: str):
    cid = b"\x00" + bytes([len(client_id)]) + client_id.encode()
    body = b"\x00\x04MQTT\x04\x02\x00\x3c" + cid
    connect = b"\x10" + bytes([len(body)]) + body
    sub_body = b"\x00\x01" + bytes([len(TOPIC) >> 8, len(TOPIC) & 0xFF]) + TOPIC + b"\x00"
    subscribe = b"\x82" + bytes([len(sub_body)]) + sub_body
    pub_body = bytes([len(TOPIC) >> 8, len(TOPIC) & 0xFF]) + TOPIC + PAYLOAD
    publish = b"\x30" + bytes([len(pub_body)]) + pub_body
    return connect, subscribe, publish


class Soak:
    def __init__(self, args):
        self.args = args
        self.mod: W.Mod | None = None
        self.mac = "unknown"
        self.connect, self.subscribe, self.publish = mqtt_packets("w6soak01")
        self.mqtt_ok = False
        self.stats = {
            "cycles": 0, "http_ok": 0, "http_fail": 0, "http_ttfb": [],
            "mqtt_pub": 0, "mqtt_echo": 0, "mqtt_rtt": [],
            "reconnects": 0, "resets": 0, "serial_reopen": 0,
            "rssi": [], "errors": [],
        }
        self.started = time.time()
        os.makedirs(RESULT_DIR, exist_ok=True)
        self.log = None
        self.log_path = None
        self.live_path = os.path.join(RESULT_DIR, "soak_live.json")
        self.last_snapshot = 0.0
        self.last_summary = time.time()

    # ---------------- 日志
    def open_log(self):
        stamp = time.strftime("%Y%m%d_%H%M%S", time.localtime(self.started))
        self.log_path = os.path.join(RESULT_DIR, f"soak_{self.mac.replace(':', '')}_{stamp}.log")
        self.log = open(self.log_path, "a", encoding="utf-8", buffering=1)

    def out(self, line: str):
        ts = time.strftime("%H:%M:%S")
        text = f"[{ts}] {line}"
        print(text, flush=True)
        if self.log:
            self.log.write(text + "\n")

    def err(self, msg: str):
        self.stats["errors"].append(f"{time.strftime('%H:%M:%S')} {msg}")
        self.stats["errors"] = self.stats["errors"][-30:]
        self.out(f"ERROR {msg}")

    # ---------------- 连接与恢复
    def connect_module(self) -> bool:
        self.close_module()
        try:
            self.mod = W.Mod(self.args.port)
            st = self.mod.reinit(self.args.ssid, self.args.password)
        except BaseException as e:          # 包括 SystemExit
            self.err(f"connect: {e}")
            self.mod = None
            return False
        m = re.search(r'STAMAC,"([0-9a-fA-F:]{17})"', st["cifsr"])
        if m:
            self.mac = m.group(1).lower()
        if self.log is None:
            self.open_log()
        self.mqtt_ok = False
        return True

    def close_module(self):
        if self.mod is not None:
            try:
                self.mod.s.close()
            except Exception:
                pass
            self.mod = None

    def ensure_module(self) -> bool:
        if self.mod is None:
            return self.connect_module()
        r = self.mod.cmd("AT", 0.5)
        if b"OK" in r:
            return True
        self.err("AT 无响应，尝试复位")
        self.stats["resets"] += 1
        try:
            self.mod.reinit(self.args.ssid, self.args.password)
            self.mqtt_ok = False
            return True
        except BaseException as e:
            self.err(f"reinit 失败: {e}")
            self.stats["serial_reopen"] += 1
            time.sleep(2)
            return self.connect_module()

    # ---------------- 单项操作
    def send_retry(self, pkt: bytes, tries: int = 3) -> bool:
        for _ in range(tries):
            if self.mod.cipsend(pkt, 2.0):
                return True
            self.mod.drain(0.2)
        return False

    def open_tcp(self, host: str, port: int, timeout: float = 6.0) -> bool:
        self.mod.s.reset_input_buffer()
        self.mod.s.write(b"AT+CIPCLOSE\r\n")
        self.mod.wait_for(b"CLOSED", 1.0)
        time.sleep(0.3)
        self.mod.s.write(f'AT+CIPSTART="TCP","{host}",{port}\r\n'.encode())
        r = self.mod.wait_for(b"OK", timeout)
        return b"OK" in r and b"CONNECT" in r

    def mqtt_connect(self) -> bool:
        if not self.open_tcp(self.args.mqtt_host, 1883):
            return False
        if not self.send_retry(self.connect):
            return False
        t = time.time()
        buf = self.mod.leftover
        while time.time() - t < 6:
            buf += self.mod.s.read(1 << 16)
            if b"\x20\x02\x00\x00" in buf:
                self.mod.leftover = b""
                return self.send_retry(self.subscribe)
        return False

    def http_once(self) -> float | None:
        t0 = time.time()
        if not self.open_tcp(self.args.http_host, 80):
            return None
        req = (f"GET / HTTP/1.0\r\nHost: {self.args.http_host}\r\n"
               f"User-Agent: w600-soak\r\n\r\n").encode()
        if not self.send_retry(req):
            return None
        rd = W.Ipd(self.mod, seed=self.mod.leftover)
        while time.time() - t0 < 5:
            rd.pump(0.05)
            if b"HTTP/" in rd.data:
                return (time.time() - t0) * 1000
        return None

    # ---------------- 一轮
    def cycle(self) -> None:
        s = self.stats
        s["cycles"] += 1
        i = s["cycles"]

        if not self.mqtt_ok:
            s["reconnects"] += 1
            self.mqtt_ok = self.mqtt_connect()

        if self.mqtt_ok:
            t1 = time.time()
            if self.send_retry(self.publish):
                s["mqtt_pub"] += 1
                buf = self.mod.leftover
                t_end = time.time() + 1.5
                got = False
                while time.time() < t_end:
                    buf += self.mod.s.read(1 << 16)
                    if PAYLOAD in buf:
                        got = True
                        break
                self.mod.leftover = b""
                if got:
                    s["mqtt_echo"] += 1
                    s["mqtt_rtt"].append(round((time.time() - t1) * 1000, 1))
            else:
                self.mqtt_ok = False

        http_msg = ""
        if i % self.args.http_every == 0:
            ttfb = self.http_once()
            self.mqtt_ok = False                     # 单 socket，HTTP 会顶掉 MQTT
            if ttfb is None:
                s["http_fail"] += 1
                http_msg = "http=FAIL"
            else:
                s["http_ok"] += 1
                s["http_ttfb"].append(round(ttfb))
                http_msg = f"http={ttfb:.0f}ms"

        rssi_msg = ""
        if i % self.args.rssi_every == 0:
            w = self.mod.cmd("AT+CWJAP?", 1.0).decode("utf-8", "replace")
            m = re.search(r",(\d+),(-?\d+)", w)
            if m:
                s["rssi"].append(int(m.group(2)))
                rssi_msg = f"rssi={m.group(2)}"

        self.out(f"#{i:05d} mqtt={'ok' if self.mqtt_ok else 'down'} {http_msg} {rssi_msg}".rstrip())

    # ---------------- 快照
    def snapshot(self, force: bool = False):
        now = time.time()
        if not force and now - self.last_snapshot < self.args.snapshot_minutes * 60:
            return
        self.last_snapshot = now
        s = self.stats

        def pct(a, b):
            return round(a / b * 100, 1) if b else 0.0

        snap = {
            "mac": self.mac,
            "started": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(self.started)),
            "uptime_min": round((now - self.started) / 60, 1),
            "cycles": s["cycles"],
            "http_ok": s["http_ok"], "http_fail": s["http_fail"],
            "http_success_pct": pct(s["http_ok"], s["http_ok"] + s["http_fail"]),
            "http_ttfb_avg_ms": round(statistics.mean(s["http_ttfb"]), 1) if s["http_ttfb"] else None,
            "mqtt_pub": s["mqtt_pub"], "mqtt_echo": s["mqtt_echo"],
            "mqtt_echo_pct": pct(s["mqtt_echo"], s["mqtt_pub"]),
            "mqtt_rtt_avg_ms": round(statistics.mean(s["mqtt_rtt"]), 1) if s["mqtt_rtt"] else None,
            "reconnects": s["reconnects"], "resets": s["resets"],
            "serial_reopen": s["serial_reopen"],
            "rssi_min": min(s["rssi"]) if s["rssi"] else None,
            "rssi_max": max(s["rssi"]) if s["rssi"] else None,
            "rssi_median": statistics.median(s["rssi"]) if s["rssi"] else None,
            "errors": s["errors"][-10:],
            "log": self.log_path,
        }
        tmp = self.live_path + ".tmp"
        json.dump(snap, open(tmp, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
        os.replace(tmp, self.live_path)
        self.out(f"[snapshot] {snap['uptime_min']}min cycles={snap['cycles']} "
                 f"http={snap['http_success_pct']}% mqtt_echo={snap['mqtt_echo_pct']}% "
                 f"resets={snap['resets']}")
        if now - self.last_summary > 3600:
            self.last_summary = now
            self.out(f"[hourly] rssi med={snap['rssi_median']} "
                     f"http_ttfb={snap['http_ttfb_avg_ms']}ms "
                     f"mqtt_rtt={snap['mqtt_rtt_avg_ms']}ms "
                     f"reconnects={snap['reconnects']}")

    # ---------------- 主循环
    def run(self):
        if not self.connect_module():
            self.open_log()
            self.out("首连失败，10s 后重试")
            time.sleep(10)
            if not self.connect_module():
                self.out("仍失败，退出")
                return
        self.out(f"[start] 长跑开始 mac={self.mac} 端口={self.args.port} "
                 f"间隔={self.args.interval}s http每{self.args.http_every}轮 "
                 f"mode={'forever' if self.args.forever else str(self.args.minutes) + 'min'}")
        deadline = None if self.args.forever else time.time() + self.args.minutes * 60

        while True:
            cycle_start = time.time()
            try:
                if not self.ensure_module():
                    self.err("模块不可用，等待重连")
                    time.sleep(5)
                    continue
                self.cycle()
                self.snapshot()
            except BaseException as e:        # 绝不让任务退出
                self.err(f"cycle 异常: {type(e).__name__}: {e}")
                if self.args.verbose:
                    self.out(traceback.format_exc())
                self.stats["serial_reopen"] += 1
                self.close_module()
                time.sleep(3)

            if deadline is not None and time.time() >= deadline:
                break
            rest = self.args.interval - (time.time() - cycle_start)
            if rest > 0:
                time.sleep(rest)

        self.snapshot(force=True)
        self.out(f"[stop] 结束 cycles={self.stats['cycles']} "
                 f"http={self.stats['http_ok']}/{self.stats['http_ok'] + self.stats['http_fail']} "
                 f"mqtt_echo={self.stats['mqtt_echo']}/{self.stats['mqtt_pub']}")
        if self.log:
            self.log.close()
        self.close_module()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM10")
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--pass", dest="password", required=True)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--minutes", type=float, default=0, help="定时模式，0=不限")
    mode.add_argument("--forever", action="store_true", help="一直跑")
    ap.add_argument("--interval", type=float, default=10)
    ap.add_argument("--http-host", default="www.baidu.com")
    ap.add_argument("--mqtt-host", default="test.mosquitto.org")
    ap.add_argument("--http-every", type=int, default=30)
    ap.add_argument("--rssi-every", type=int, default=6)
    ap.add_argument("--snapshot-minutes", type=float, default=10)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    if not args.forever and args.minutes <= 0:
        args.forever = True
    Soak(args).run()


if __name__ == "__main__":
    main()
