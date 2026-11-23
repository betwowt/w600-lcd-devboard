#!/usr/bin/env python3
"""PC side of the W600 SDK throughput probe.

Firmware: example/sdkperf (module listens on TCP 3333 sink, 3334 source).

  python scripts/w600_sdk_perf.py --ip 192.168.8.x rx      # PC -> module
  python scripts/w600_sdk_perf.py --ip 192.168.8.x tx      # module -> PC (10 s)
  python scripts/w600_sdk_perf.py --ip 192.168.8.x both
"""
import argparse
import socket
import time

SINK_PORT = 3333
SRC_PORT = 3334


def fmt(nbytes, secs):
    kb = nbytes / 1024.0 / secs
    return f"{nbytes/1e6:.2f} MB in {secs:.2f} s = {kb:.1f} KB/s ({kb*8/1000:.2f} Mbps)"


def rx_test(ip, seconds, mb=4):
    payload = mb * 1024 * 1024
    s = socket.create_connection((ip, SINK_PORT), timeout=5)
    s.settimeout(5)
    buf = b"\xa5" * 1460
    sent = 0
    t0 = time.time()
    try:
        while sent < payload:
            n = s.send(buf)
            sent += n
    except socket.timeout:
        print("rx: send stalled")
    dt = time.time() - t0
    s.shutdown(socket.SHUT_WR)
    try:
        while s.recv(4096):
            pass
    except OSError:
        pass
    s.close()
    print("rx:", fmt(sent, dt))


def tx_test(ip, seconds):
    s = socket.create_connection((ip, SRC_PORT), timeout=5)
    s.settimeout(seconds + 10)
    total = 0
    t0 = time.time()
    while True:
        try:
            b = s.recv(65536)
        except socket.timeout:
            break
        if not b:
            break
        total += len(b)
        if time.time() - t0 > seconds + 5:
            break
    dt = time.time() - t0
    s.close()
    print("tx:", fmt(total, dt))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["rx", "tx", "both"])
    ap.add_argument("--ip", required=True)
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--mb", type=int, default=4, help="rx payload size in MB")
    args = ap.parse_args()
    if args.mode in ("rx", "both"):
        rx_test(args.ip, args.seconds, args.mb)
    if args.mode in ("tx", "both"):
        tx_test(args.ip, args.seconds)


if __name__ == "__main__":
    main()
