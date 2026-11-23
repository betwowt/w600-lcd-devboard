# W600-AT

TW-03 / WinnerMicro W600 模组的实测、基线与长跑工具；目录里同时保留了 ESP32-C3 烧 ESP-AT 的脚本（第 2 部分）。

两件事放在这里：

1. **TW-03 / W600 模块的实测与性能基线**（手上这块 2 元清库存模块，AT 固件已是现成的）。
2. **ESP32-C3 烧 ESP-AT 的工具链**（原始需求，等有真 C3 硬件再跑）。

---

# 1. TW-03（WinnerMicro W600，2 元模块）

## 身份与固件

| 项 | 实测 |
| --- | --- |
| 串口 | COM14 不存在；模块在 **COM10**（CH340K） |
| 芯片 | **WinnerMicro W600**，Cortex-M3，288KB SRAM / 1MB Flash，引脚兼容 ESP-12F |
| 固件 | `AT version:1.2.0 (Sep 21 2020)` / `SDK version:3.0.0` / Shenzhen ThingsTurn |
| 协议族 | 不是乐鑫：esptool 连不上，ESP-AT 也烧不进去 |
| 升级 | 需要 `w600tool` 拉 RTS 进 secboot；本板 RTS/DTR 未接复位，目前**无法自动进下载模式**，要手动复位/断电 |

命令集是 **ESP-AT 的子集**（`CWMODE/CWJAP/CIFSR/CIPSTART/CIPSEND/+IPD/PING/UART/RST/GMR/ATE0`），不是 WM_W60X 那套（`WPRT/WSCAN/SKCT` 全 ERROR）。

## 能力实测（全部在 921600 UART 下跑通）

| 能力 | 结论 | 依据 |
| --- | --- | --- |
| TCP client | ✅ | `AT+CIPSTART="TCP","www.baidu.com",80` → `CONNECT`，HTTP 200 |
| TCP server | ❌ | `AT+CIPSERVER=1,8888` → ERROR（无 server 模式） |
| UDP | ✅ | 双向都通；收数据需在 `CIPSTART` 里给第 4 个参数（本地端口） |
| DNS | ✅ | `CIPSTART` 直接用域名，无需先解析 |
| TLS | ✅ **模块内完成** | `AT+CIPSTART="SSL","www.baidu.com",443` 后直接发明文 `GET /`，收到 `HTTP/1.0 200 OK` |
| HTTP | ⚠️ 无专用 AT | 没有 `AT+HTTPCLIENT`；用 TCP/SSL + 手写 `GET` 即可（已验证 200） |
| MQTT | ⚠️ 无专用 AT | 无 `AT+MQTT*`；纯 TCP 手写 MQTT 3.1.1 已验证：`CONNACK=20 02 00 00`、`SUBACK=90 03 00 01 00`、自订阅发布回环收到 `hello` |
| PING | ✅ | `AT+PING="8.8.8.8"` |
| 多路 socket | ❌ | `CIPMUX=1` 不支持，只能单连接 |
| HTTP 与 MQTT 并存 | ❌ | 单 socket：开 HTTP 会顶掉 MQTT，必须重连。产品里把 HTTP 做成低频（如每小时一次），或用第二个模组/主控直连 |

## 速率与延迟（实测）

链路：模块 STA `192.168.8.17`，RSSI −41~−45 dBm，CH7；PC 走有线 `192.168.8.171`。

| 指标 | 实测 | 说明 |
| --- | --- | --- |
| TCP↓ HTTP 大文件 | **83.9 KB/s（0.67 Mbps，93% UART）** | UART 921600 上限 90 KB/s，**瓶颈是 UART 不是 Wi-Fi** |
| UDP↓ 10/20/30/40 KB/s 注入 | 收 8.1 / 16.8 / 23.1 / 32.2 KB/s，丢包 3–11% | 40 KB/s 档仍有 32 KB/s |
| UDP↑（`CIPSEND=2048` 连发） | **32 KB/s** | 单次发送上限 2048 B，命令往返 ~15 ms，AT 管道效率约 35% |
| PING 网关 | 2–14 ms（avg 4–8） | 空口延迟正常 |
| PING 8.8.8.8 | 166–258 ms（avg 184–201），丢 0–2/5 | 公网路径质量一般 |
| MQTT CONNECT→CONNACK | 850–1000 ms | 含公网 RTT ≈200 ms，模块侧处理很快 |
| 整套基线耗时 | ~100 s | 每组测试前 `AT+RST` 隔离，避免 socket 状态机残留 |
| 命令往返 | ~15 ms | `AT+CIPSEND` 从下发到 `>` 提示 |

结论：**W600 的 Wi-Fi 能力被 921600 UART 完全卡住**，AT 方式下拿不到 1 Mbps 以上；换 115200 时上限只有 11.5 KB/s。若真要跑高吞吐，只能用 W600 SDK 自己写 UDP/TCP 测试，或换 ESP32。

## 两块模块实测对比

同一位置、同一 AP（CH7），各跑一次 `w600_suite.py`：

| 指标 | A `28:6d:cd:0c:ec:e2` | B `28:6d:cd:4b:bf:0f` | 差 |
| --- | --- | --- | --- |
| RSSI | −37 dBm | −43 dBm | −6 dB |
| PING gw avg | 7.2 ms | 4.8 ms | −2.4 ms |
| TCP↓ HTTP | 83.9 KB/s | 75.4 KB/s | −10% |
| UDP↓ 40 KB/s 档 | 32.2 KB/s / 丢 6.8% | 34.7 KB/s / 丢 0% | +8% |
| UDP↑ | 32.0 KB/s | 32.1 KB/s | ≈0 |
| MQTT 握手 | 974 ms | 797 ms | −18% |
| TLS | ✅ | ✅ | — |

结论：**同批次，差异在 10% 量级**，主要来自 RSSI 和单次测得的位置噪声，不是个体质量问题。

真实负载（模块 B）：HTTP 连续 10 次 **10/10 成功，TTFB 316–361 ms（avg 337）**；
MQTT CONNECT/SUBSCRIBE 正常，单条发布→回环 RTT 262–1577 ms（avg 300–370，取决于公网），
持续发布约 **2.5 条/s**（受 `CIPSEND` 往返和异步 `+IPD` 挤占限制，不是网络带宽）。

## socket 状态残留：能修吗

**固件层修不了**：没有 AT 源码，也没有 secboot 通道（板上 RTS 未接复位），
`AT+RST` 只是软复位，无法替换固件。残留的成因推测是 `CIPSTART/CIPCLOSE`
在错误路径上没有回收 lwIP pcb，快速重开时旧 socket 还占着唯一实例。

**主机侧可以完全规避，已实测**（连续 8 次 open/close + 10 次 HTTP 全成功）：

1. 用完必须显式 `AT+CIPCLOSE`，等 `CLOSED`。回 `ERROR` 也正常（对端已关，模块已释放）。
2. 关掉后等 **200–300 ms**，让 FIN/TIME_WAIT 走完。
3. `AT+CIPSTATUS` 确认 `STATUS:4`（未连接）再 `CIPSTART`；`STATUS:2` 表示已连 AP、未建链，也可以。
4. `CIPSEND` 拿不到 `>` 或回 `busy p...` 时，排空 200 ms 后**重试**，不要当成致命错误
   （异步 `+IPD` 会打断握手；驱动必须把这类数据保留下来而不是丢）。
5. 连续 2 次失败才 `AT+RST` + 重新 `CWJAP`（约 6–8 s），作为最后手段。
6. 单连接串行使用，不要并发（`CIPMUX=1` 不支持）。

## 稳定性测试

```bash
# 定时跑
python scripts/w600_soak.py --ssid betwowt --pass xiaobudian1314520 \
    --minutes 30 --interval 10 --http-every 10

# 长跑到手动停止（后台常驻）
python scripts/w600_soak.py --ssid betwowt --pass xiaobudian1314520 --forever
```

每轮 MQTT 发布 1 条并等回环；每 `--http-every` 轮做一次 HTTP GET（默认 30 轮 = 5 分钟）；
每 6 轮采一次 RSSI；每 10 分钟覆盖写 `results/soak_live.json` 快照。

自恢复：`AT` 无响应 → `AT+RST` + 重连；串口异常 → 关端口重开；任何异常只记录不退出。

后台常驻启动（脱离终端，关掉窗口也继续跑）：

```powershell
$py = (Get-Command python).Source
Start-Process -FilePath $py -WorkingDirectory (Get-Location) -WindowStyle Hidden `
  -RedirectStandardOutput results\soak_run.out -RedirectStandardError results\soak_run.err `
  -ArgumentList 'scripts/w600_soak.py','--ssid','betwowt','--pass','xiaobudian1314520','--forever' `
  -PassThru | Select-Object -ExpandProperty Id | Set-Content results\soak.pid
```

停止：`Stop-Process -Id (Get-Content results\soak.pid) -Force`（或 `taskkill /PID <pid> /F`）。

2 分钟冒烟：12 轮，HTTP 4/4，MQTT 9/9 回环，RSSI 稳定 −41 dBm。
30 分钟实测：180 轮，HTTP 18/18，MQTT 回环 178/179（99.4%），重连 18 次（= HTTP 次数），复位 0。

10 小时长跑实测（2026-11-17 00:01 → 10:05）：3621 轮，HTTP 120/120，MQTT 回环 3565/3566（99.97%），
MQTT RTT avg 326 ms，HTTP TTFB 每小时均值 642–658 ms 无漂移；重连 121 次全部等于 HTTP 次数；
唯一一次异常是 08:48 外部程序占用 COM10（`PermissionError`），24 s 内自恢复，与模块无关。

## 已知坑

- `CIPSEND` 单次 **≤2048**，2920 起模块直接不回 `>`，且会卡住后续命令。
- TCP 的 `CIPSTART` 里 IP **必须带引号**，不带会回 `IP ERROR`；UDP 必须带第 4 个参数（本地端口）才能收到 `+IPD`。
- 多轮 open/close 后 socket 状态机会残留（出现 `link is not valid` / `ALREADY CONNECTED`），测试脚本里每组之间 `AT+RST` 最稳。
- `AT+UART` 最高 921600（1000000 直接不认）。
- 无 OTA、无证书管理、无 BLE；1MB Flash 里固件已占大头。

## 一键基线与两块模块对比

```bash
# 模块 A
python scripts/w600_suite.py --port COM10 --ssid <SSID> --pass <PASSWORD> --name A
# 换模块 B（自动识别 115200 并切到 921600）
python scripts/w600_suite.py --port COM10 --ssid <SSID> --pass <PASSWORD> --name B
# 并排对比
python scripts/w600_suite.py --compare
```

结果写到 `results/<MAC>_<name>_<ts>.json`，包含固件、MAC、RSSI、信道、两路 PING、UDP↓ 四档速率/丢包、UDP↑、TCP↓、MQTT 握手延迟、TLS 可用性。`--compare` 直接打印 A/B 差值和百分比。

跑之前先关掉 **ATK_XCOM / 串口助手**（会独占 COM10）。模块默认可能是 115200，脚本会自动探测并切到 921600；`AT+RST` 后波特率若回退，也会自动重扫。

对比时的注意点：两块板放在**同一位置**、同一 AP/信道；RSSI 差 3 dB 内可认为同批次；TCP↓ 只要都打满 UART 就说明都正常，UDP↓ 丢包率更能区分个体。

## 2 块钱值不值

- 硬件给到：Wi-Fi 802.11n + Cortex-M3 + 288KB RAM + 1MB Flash + 内置 TLS，同价位（ESP-01S 4–5 元、ESP-12F 6–9 元、ESP32-C3 8–12 元）确实便宜。
- 但 AT 固件是 2020 年的裁剪版：**没有 HTTP/MQTT 专用 AT、没有 server 模式、没有 OTA、没有 BLE**，协议要主控 MCU 自己实现；socket 状态机还有毛刺。
- 对"主控 + 便宜透传"的量产小家电/传感器 OK；要完整 AT 生态、OTA、社区支持，仍然选 ESP32-C3 + ESP-AT（本项目第 2 部分）。
- 清库存意味着**没有长期供货和固件维护**，量产前先确认 BOM 里能否长期拿到，以及 secboot 烧录工装（RTS→RST 必须接上）。

---

# 2. ESP32-C3 烧 ESP-AT

```bash
python tools/fetch_at.py                        # 下载官方包到 firmware/
python scripts/at_tool.py detect -p COM<X>
python scripts/at_tool.py flash  -p COM<X>      # 默认先整片擦除
python scripts/at_tool.py at     -p COM<X> AT+GMR
```

| Flash | 固件包 | 烧法 | 限制 |
| --- | --- | --- | --- |
| 4MB | v4.1.1 / v3.3 | `factory/factory_MINI-1_unfilled.bin` → `0x0` | 官方布局，OTA 可用 |
| 2MB | v2.4.2 | 多 bin + 裁剪分区表，app 在 `0x60000` | 无 ota_1，`AT+CIUPDATE` 不可用 |

2MB 只能用 v2.4.2 的原因：`at_customize`（`0x1e000`/`0x42000`）内部是绝对偏移（mfg_nvs@0x1f000、fatfs@0x47000），基址不能动，app 上限 `0x1a0000`。

| 版本 | `esp-at.bin` | `0x60000 + size` | 2MB |
| --- | --- | --- | --- |
| v4.1.1 | 1,797,936 | 0x216F30 | 否 |
| v3.3 | 1,742,816 | 0x209800 | 否 |
| v2.4.2 | 1,519,360 | 0x1D2F00 | 是 |

2MB 路径会把 `bootloader.bin` / `esp-at.bin` 镜像头的 flash size 改成 2MB。

接线：下载走 UART0（GPIO20 RX / GPIO21 TX，需 DTR→EN、RTS→GPIO9）；官方 MINI-1 AT 口是 UART1（GPIO7 TX / GPIO6 RX，115200 8N1）。

已验证离线部分：分区表生成/边界校验、4MB 与 2MB 烧录计划、固件包筛选、镜像头改写。
未验证：真实 C3 烧录（没有硬件）。
