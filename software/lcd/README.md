# W600 点屏例程（ILI9341 240x320）

基于 W600 模组开发板原理图，从 STM32G0B1 参考工程
`stm32g0b1rc-P024C135/HARDWARE/LCD` 移植。

## 接线（原理图核对）

| 信号 | W600 引脚 | 说明 |
| --- | --- | --- |
| SPI_CLK | PA01 | `wm_spi_ck_config(WM_IO_PA_01)`，AF option 3 |
| SPI_MOSI | PB18 | `wm_spi_do_config(WM_IO_PB_18)`，AF option 3 |
| LCD_CS | PB11 | GPIO 软件片选，低有效 |
| LCD_DC | XL9555 P0_3 | I2C GPIO 扩展 |
| LCD_RST | XL9555 P0_0 | 低有效 |
| LCD_PWR | XL9555 P0_2 | 高有效（面板使能/背光电源） |
| XL9555_SCL | PB09 | 软件 I2C（W600 硬件 I2C 无 PB09 SCL 复用） |
| XL9555_SDA | PB12 | 软件 I2C，4.7k 上拉 |
| FLASH_CS | PB15 | 外部 W25Q64 片选，`w25qxx` 驱动管理 |
| TP_RST | XL9555 P0_1 | 触摸复位，低有效 |
| TP_INT | PB10 | 触摸中断（本例程轮询，不用） |
| TOUCH_SCL | PB13 | 触摸 I2C SCL，4.7k 上拉 |
| TOUCH_SDA | PB14 | 触摸 I2C SDA，4.7k 上拉 |

XL9555 地址：A2=1（R16 上拉 3.3V）、A1=0、A0=0 → 7 位地址 `0x24`。
程序启动会扫描 `0x20..0x27`，并打印 SCL/SDA 空闲电平和每个 ACK 地址。
P0_0..3 输出，P0_4..7 按键输入；P1_0..2 为 RGB LED，低有效。

## 显示流程

1. PB15 拉高，释放共享 SPI 总线上的外部 Flash。
2. 软件 I2C 探测 XL9555，写输出锁存与方向寄存器。
3. `LCD_PWR=1`、`LCD_RST=0`，20ms 后释放复位，等 120ms。
4. W600 主机 SPI Mode 0、20MHz，发送 ILI9341 初始化序列。
5. 全屏红/绿/蓝/白自检，然后画彩条和白色触摸画板。
6. 触摸初始化：`TP_RST` 低 10ms、高 120ms，再探测 `0x38`（FT6236）或
   `0x15`（CST816）；两者坐标寄存器都是 `0x02` 计数、`0x03..0x06` 坐标。
7. 循环轮询触摸：在画板上画 3x3 黑点，标题栏 `CLR` 按钮清空画板，底部显示原始坐标。
8. 开机检查 TF 卡：若存在 `/logo.bin`，先同步到外部 W25Q64（`0x700000`，
   256 KB 保留区），然后统一从 Flash 读取并显示 3 秒，再进入上面的演示界面。
   无卡或卡内无文件时直接使用 Flash 中已有的 logo。

## TF 卡 / 开机 logo

| 信号 | W600 引脚 | 说明 |
| --- | --- | --- |
| SD_CS | PB16 | GPIO 片选 |
| SPI_CLK | PA01 | 与 LCD 共用 |
| SPI_MOSI | PB18 | 与 LCD 共用 |
| SPI_MISO | PB17 | 只给 SD 用 |

实测 W600 主机 SPI 读 SD 的 R1 会错位（CMD0 返回 `0xC0`），硬件 mode 0/3
都不行；同一张卡用 GPIO bit-bang 则 CMD0=`01`、CMD8=`01 00 00 01 AA`、
ACMD41=`00`。现在 `logo.c` 从第一个时钟起就只用 GPIO bit-bang，不再先
试硬件路径：

```
SD: CMD0 try 0 -> 01
...
LOGO: done
```

`/logo.bin` 格式（20 字节头）：`"LOGO"` + `u16 w` + `u16 h` + `u16 fmt=1` +
2 字节保留 + `u32 data_size` + `u32 保留/校验`，像素为 RGB565 大端，从上到下。

开机流程：

1. `w25qxx_init()` 探测 W25Q64/W25Q128；ID 走 GPIO bit-bang，先用 1 MHz，
   失败再降到 512 kHz 重试。
2. SD 卡初始化并挂载 FAT；找到 `/logo.bin` 后校验头部。
3. 若 Flash 中的内容与 SD 文件不同，按 4 KB 扇区擦写并回读校验；
   内容相同则跳过擦写，减少 Flash 磨损。
4. 无论本次是否更新，始终从 Flash `0x700000` 读取 logo 并流式送给 LCD，
   整幅 240x320 图不需要放入 RAM。

Flash 布局：

```text
0x700000 .. 0x73FFFF   logo.bin 保留区（256 KB）
0x7FF000 .. 0x7FFFFF   板卡自检擦除/写入测试区
```

硬件注意：BOM 里 U1 标的是 `W25Q64JWSSIQ`（1.8V），但实际板上 U1 的
VCC/WP/HOLD 网络都是 `+3.3V`；如果实装的是 3.3V 型号（`W25Q64JV/FV`），
这条 BOM 记录可以忽略。当前 ID 为 `FF FF FF` 时，先核对 U1 丝印、PB15
片选和 PB17 MISO；固件在探测前会先发 0xFF/0xAB/0x66+0x99 退出 QPI、
释放掉电和软复位。

## Bad Apple 播放（TF 卡实时流）

固件变体 `badapple` 在开机 logo 之后自动播放 TF 卡根目录的
`/BADAPPLE.BIN`，循环播放，按板上任意按键停止并进入触摸演示。

```powershell
pwsh -File software/build-lcd.ps1 -Variant badapple
```

### 生成 BIN

`tools/bad_apple.py` 用 ffmpeg 解码视频，按硬阈值（默认）或 8x8 Bayer
有序抖动压成 1bpp，默认 240x180、12fps、屏幕居中（`y=70`）。默认输出
**RLE** 帧流（`flags=1`）；`--raw` 输出旧的无压缩固定帧。

Bad Apple 推荐 `--dither none` + RLE：原片本来就是黑白，硬阈值不会引入
有序抖动的棋盘噪点，RLE 才能把 240x180 压到平均约 0.6 KB/帧。同一段
画面用 `--dither ordered` 时噪点会把 RLE 撑到 5-7 倍。

```bash
# 全画质：240x180、18fps、RLE
python tools/bad_apple.py bad_apple.mp4 -o badapple.bin --fps 18 --dither none

# 小尺寸高帧率备选
python tools/bad_apple.py bad_apple.mp4 -o fast.bin --width 192 --height 144 --fps 24 --dither none
python tools/bad_apple.py bad_apple.mp4 -o tiny.bin --width 160 --height 120 --fps 24 --dither none

# 旧格式：无压缩、12fps，固件仍然能播
python tools/bad_apple.py bad_apple.mp4 -o old.bin --raw --fps 12 --dither ordered
```

仓库根目录已生成：

| 文件 | 尺寸 | fps | 格式 | 大小 | 平均流量 |
| --- | --- | --- | --- | --- | --- |
| `BADAPPLE.BIN` | 240x180 | 18 | RLE | 2.33 MB | 619 B/帧，10.9 KiB/s |
| `BADAPPLE_FAST.BIN` | 192x144 | 24 | RLE | 2.34 MB | 466 B/帧，10.9 KiB/s |
| `BADAPPLE_TINY.BIN` | 160x120 | 24 | RLE | 1.82 MB | 363 B/帧，8.5 KiB/s |
| `BADAPPLE_RAW_240X180_12FPS.BIN` | 240x180 | 12 | raw | 14 MB | 5400 B/帧，63 KiB/s |

直接复制要播的那个到 TF 卡并改名为 `BADAPPLE.BIN`（FAT 8.3 名：8 字符
`BADAPPLE` + 扩展名 `BIN`）。当前固件只认这一个文件名。`--fps` 必须是
整数，固件按 `configTICK_RATE_HZ=500` 计帧间隔。

### 文件格式

32 字节小端头 + 连续帧：

| 偏移 | 类型 | 内容 |
| --- | --- | --- |
| 0 | char[8] | `"BADAPPL1"` |
| 8 | u16 | 宽（8 的倍数，≤240） |
| 10 | u16 | 高（≤320） |
| 12 | u16 | 屏幕 x |
| 14 | u16 | 屏幕 y |
| 16 | u16 | fps |
| 18 | u16 | flags：0=raw，1=RLE；其它位无效 |
| 20 | u32 | 帧数 |
| 24 | u32 | 未压缩帧字节数 `(宽/8)*高` |
| 28 | u32 | 保留 |

flags=0：每帧固定 `frame_bytes` 字节，行优先、每像素 1 bit、MSB 在左，
1=白、0=黑。

flags=1：每帧先是 u16 长度（小端），bit15=1 表示该帧仍是 raw，低 15 位是
payload 字节数；否则 payload 是 LEB128 游程长度流，从黑色开始黑白交替，
第一个游程为 0 表示首像素是白色，一直解码到 `宽*高` 个像素。RLE 编码器
逐帧比较，payload 不小于 `frame_bytes` 时自动退回 raw（bit15）。

`bad_apple.py` 会先写占位头，编码完再回填帧数和 flags。

手上的 bin 如果是别家播放器的裸 1bpp 帧，需要按上表补头（flags=0）并改成
`(宽/8)*高` 的行宽，不能直接改名使用。

### 刷屏路径

1. RLE 帧：先 `sd_file_read()` 读 2 B 长度，再读 payload；两次都是
   独立的 SD 总线作业，payload 整块一次读完。raw 帧直接一次读进
   `g_frame`。
2. `ba_rle_decode()` 在播放任务里把游程流解成 1bpp 的 `g_frame`；
   raw 帧跳过解码。
3. `lcd_image_1bpp()` 把整个窗口的展开和刷屏合成 **一个** LCD 总线
   作业：每个源字节查 256 项表展开成 8 个 RGB565 像素（16 B），累计
   8 行发一次 `tls_spi_write()`。展开缓冲 3840 B。
4. 按文件 fps 用 500 Hz tick 补睡眠；渲染跟不上时放弃补睡并重新对齐。

串口每 10 秒打印一次：

```
BA: loop 0 frame 179, 18 fps, SD 8.1 ms, LCD 35.2 ms
```

`SD` 包含长度+payload 两次读取，`LCD` 包含 RLE 解码和 LCD 刷屏，都是
10 秒平均（2 ms tick 分辨率）。240x180 的 LCD 数据固定是 86.4 KB，
20 MHz 下约 35 ms，所以帧率上限约 28 fps。RLE 把 SD 侧从 5400 B/帧
压到平均约 620 B/帧，即使卡停在 400 kHz 档也能喂 18 fps；`SD` 仍然
很大时看启动日志的 `SD: data clock ...`。

旧的 `--raw` bin 仍然支持（flags=0）：1 次 SD 读 + 1 次 LCD 写，
240x180/12fps 需要约 63 KiB/s，卡慢时还是会掉帧。固件实测值以串口
`BA: loop ... fps` 为准。

### SD 读取加速

`sd_fs.c` 从 `logo.c` 拆出，logo 和 Bad Apple 共用。为了让 TF 卡喂得动
实时帧：

- 位翻转直接写 W600 GPIO 数据寄存器（PA01/PB18/PB17，地址见
  `wm_regs.h`），不再逐位走 `tls_gpio_write()` 的临界区；
- 初始化后发 CMD59 打开数据块 CRC，读块后校验 CRC16；
- 依次试 8 / 6 / 4 / 3 / 2 / 1 / 0.4 MHz，第一档连续 4 块校验通过即
  采用；8 MHz 档实际是“不加延时循环”，GPIO 位翻转上限约 2 MHz，运行中
  连续读失败会自动降档并打印日志。6/4/3 MHz 档用 2/4/8 次空转补
  setup/hold，给较慢的卡和连线留中间档；
- 逐字节写 PB 影子寄存器，不再每个 bit 读-改-写 `SD_PB_DATA`；MOSI
  设置和 MISO 采样仍是直接 APB 访问；
- 卡不支持 CMD59 时退回 400 kHz 且不校验，功能可用但帧率较低。

SD 初始化、命令应答和 FAT 遍历逻辑保持不变，仍然只在 `spi_bus.c` 的
SD 事务里碰总线。

### 限制

- 没有音频输出，只有画面。
- 画面按 1bpp 黑白播放。默认 `--dither none` 硬阈值，RLE 压缩最好；
  `--dither ordered` 保留灰度层次，但棋盘噪点会把 RLE 撑大 5-7 倍。
- 全屏 240x320 也能放（`--height 320`），但 LCD 数据 153.6 KB，
  20 MHz 下约 69 ms，帧率上限约 14 fps；这时瓶颈是 LCD，不是 TF 卡。

本轮做了三件事：RLE 把 240x180 的 SD 流量从 5400 B/帧降到约 620 B/帧；
每帧的 SD/LCD 总线作业从 34 次压到 2-3 次；SD 位翻转加了 6/4/3 MHz
中间档。串口 10 秒一次的 `SD ... ms, LCD ... ms` 用来看剩余瓶颈。
`BADAPPLE.BIN`（240x180、18fps）的 LCD 约 35 ms，SD 只要几毫秒；
`BADAPPLE_FAST/TINY.BIN` 把 LCD 数据再减半，可以跑 24fps。

## 板载驱动

`user/` 下新增三个独立驱动，均不依赖 LCD 演示状态：

| 驱动 | 文件 | 硬件 | 说明 |
| --- | --- | --- | --- |
| SPI Flash | `w25qxx.c/h` | W25Q64, CS PB15, CLK/MOSI/MISO 共用 | `w25qxx_init/read/write/erase_sector`；8 MB，4 KB 扇区 |
| 4 按键 | `key.c/h` | XL9555 P0_4..P0_7 | 10 kΩ 上拉、按下为低；`key_scan()` 20 ms 消抖 |
| RGB 灯 | `led.c/h` | XL9555 P1_0..P1_2 | 共阳 +3.3 V、低有效；`led_rgb()`/`led_set()` |

`w25qxx` 的短命令、ID 和状态轮询走 GPIO bit-bang；长块读和页写优先走
主机 SPI DMA。实测同一颗 W25Q64JVSIQ 的主机短读（`tls_spi_read`）返回
`FF`，但 DMA 路径可用，所以启动时用状态寄存器做一次 DMA 自检，失败就整体回退；ID 始终走 bit-bang。
每次 Flash 操作结束后调用 `lcd_spi_resume()` 把 CLK/MOSI/MISO 还给主机
SPI Mode 0，因此 Flash 用完可以立刻继续刷屏。ID 1 MHz（异常时 512 kHz）；
普通读 20 MHz DMA（每次 4 KB）或 8 MHz bit-bang；页写/扇区擦除 20 MHz
DMA 或 4 MHz bit-bang。页编程 256 B 和扇区擦除仍按标准 W25Q 时序。

`key_init()`/`led_init()` 假定 `lcd_init()` 已经完成 XL9555 初始化；
两者只改 P0 方向、P1 输出锁存，不会重新复位面板。

### 驱动自测固件

编译时加 `EXTRA_CCFLAGS=-DDRIVER_TEST`：跳过 SD/logo 和触摸演示，初始化
LCD 后打印 Flash JEDEC ID、读 16 字节，并把 4 个按键映射到 RGB：

| 按键 | 颜色 |
| --- | --- |
| KEY0 | 红 |
| KEY1 | 绿 |
| KEY2 | 蓝 |
| KEY3 | 白（三色全亮） |

启动时先按 R→G→B→W 各亮 0.7 秒，可单独确认每个 LED 通道；按键变化时
串口打印 XL9555 的原始 `IN0`（空闲为 `0xFF`）和解析后的 `keys`，用于区分
“按键没被读到”和“LED 通道坏”。

```bash
cd F:/projects/w600-sdk/example
T="C:/Users/betwowt/AppData/Local/Arduino15/packages/arduino/tools/arm-none-eabi-gcc/7-2017q4/bin"
make -f Makefile.mk APP_BIN_NAME=lcd TARGET=lcd COMPILE=gcc FLASH_SIZE=1M \
  EXTRA_CCFLAGS=-DDRIVER_TEST \
  CC="$T/arm-none-eabi-gcc.exe" AR="$T/arm-none-eabi-ar.exe" \
  ASM="$T/arm-none-eabi-gcc.exe" OBJCOPY="$T/arm-none-eabi-objcopy.exe" \
  LINK="$T/arm-none-eabi-ld.exe" OBJDUMP="$T/arm-none-eabi-objdump.exe"
```

产物仍是 `bin/lcd/lcd.fls`；本次预编译的自测镜像另存为
`bin/lcd/lcd_drivers.fls`。

## 板卡自检固件（LCD + Flash + TF + Wi-Fi）

`board_check.c` 用于整板验证：LCD 显示三行结果，串口打印详细日志。

| 项目 | 测试内容 | 失败时串口 |
| --- | --- | --- |
| LCD | 初始化 + 绘制状态页 | `LCD init failed` |
| W25Q64/W25Q128 | GPIO bit-bang：JEDEC ID、最后 4 KB 扇区擦除/写/读校验 | `FLASH: 0x9F BB ...` |
| TF 卡 | bit-bang 初始化、FAT16/32 挂载 | `SD: CMD0 try ...` |
| Wi-Fi | 扫描周围 AP，显示数量和最强 AP | `BOARD: Wi-Fi scan ...` |

三个 SPI 设备共用 PA01/PB18/PB17。`spi_bus.c` 用单总线任务串行化事务：
调用者把操作投递到队列后阻塞，只有总线任务能改 CS 和时序，内部再用
`g_cs_low_dev` 做 CS 互锁（重复拉低、未持有者拉高、双设备同时选中都会
打印错误并拒绝）。每次事务结束恢复主机 SPI 复用（LCD Mode 0），避免 SD/Flash bit-bang 后
LCD 花屏。
时钟按设备独立设置：LCD 走硬件 SPI Mode 0 20 MHz（失败降 10 MHz）。
W25Q64/W25Q128 的 ID 和状态走 GPIO bit-bang：ID 1 MHz（异常时 512 kHz）。
普通读先探测 20 MHz 主机 SPI DMA，状态寄存器匹配后用 DMA 读 4 KB 块；
失败回退到 8 MHz bit-bang。页写/扇区擦除同样优先 20 MHz 主机 DMA，失败回退到
4 MHz bit-bang。SD 初始化 400 kHz、读写 1 MHz。
`spi_bus_set_clock()`/`spi_bus_bb_delay()` 用于 SD 和 Flash 的 bit-bang。

```bash
cd F:/projects/w600-sdk/example
make -f Makefile.mk clean APP_BIN_NAME=lcd
T="C:/Users/betwowt/AppData/Local/Arduino15/packages/arduino/tools/arm-none-eabi-gcc/7-2017q4/bin"
make -f Makefile.mk APP_BIN_NAME=lcd TARGET=lcd COMPILE=gcc FLASH_SIZE=1M \
  EXTRA_CCFLAGS='-DBOARD_CHECK -DSD_DIAG' \
  CC="$T/arm-none-eabi-gcc.exe" AR="$T/arm-none-eabi-ar.exe" \
  ASM="$T/arm-none-eabi-gcc.exe" OBJCOPY="$T/arm-none-eabi-objcopy.exe" \
  LINK="$T/arm-none-eabi-ld.exe" OBJDUMP="$T/arm-none-eabi-objdump.exe"
```

切换 `EXTRA_CCFLAGS` 时必须先 `clean`，否则旧的 `.o` 会按默认模式复用，
生成的 `lcd.fls` 可能仍是普通 LCD 演示。

预编译镜像：`bin/lcd/lcd_board.fls`（MD5 `7c48490848fc18ccd9f2fbe90a9445a3`，本轮构建）。自检会擦除
W25Q64 最后一个 4 KB 扇区（`0x7FF000`），不会碰固件区。

普通点屏/Flash logo 镜像：`bin/lcd/lcd.fls`（MD5 `d07cfe5d2a692725053977f7afe2839e`，本轮构建）。

## 验证状态

LCD 初始化、XL9555、触摸和 SD bit-bang 已在板上验证。Flash 主机 SPI
在 Mode 0/1/2/3、0x9F/0x90 下全部返回 `FF`，因此驱动改为 GPIO bit-bang；
主机 SPI 只保留给 LCD。逻辑分析仪确认根因是 `flash_bb_cs()` 的
`if (low)` 写反：调用者用 0 表示选中，函数却把 0 当高。修正后 CS 在
时钟期间保持低，`lcd_flashonly.fls` 已读到：

```text
FLASH: CS low=0 MISO=1
FLASH: 0x9F BB = EF 40 17
FLASH: W25Q ready, 8388608 bytes
```

PB15 同时是 W600 默认 hspi_cs，`pre_gpio_config()`/`lcd_spi_resume()` 已把
硬件 SPI CS 改到未引出的 PB00，`spi_bus_cs_low/high` 每次操作前也会把
PB15 切回 GPIO。CS 测试脉冲和 RAWTEST 已从最终镜像删除。

Flash 的扇区擦除/页写、SD logo 同步和最终 LCD logo 显示尚未在这版上重新
跑完整自检；LCD 点屏本身不受影响。普通镜像现在即使 Flash 初始化失败也会
继续探测 SD 并打印 `LOGO: Flash init failed, probing SD anyway`，便于区分
“TF 卡失败”和“Flash 失败后直接返回”。

## 编译 / 烧录

在 `F:\projects\w600-sdk\example` 下：

```bash
make -f Makefile.mk APP_BIN_NAME=lcd TARGET=lcd COMPILE=gcc FLASH_SIZE=1M
```

本机用 Arduino 附带的 GCC 7-2017q4 验证通过（SDK 预编译库是 GCC 4.9/newlib 2.1
时代的，GCC 7 需要 `main.c` 里的 `__ctype_ptr__` 兼容符号）：

```bash
T="C:/Users/betwowt/AppData/Local/Arduino15/packages/arduino/tools/arm-none-eabi-gcc/7-2017q4/bin"
make -f Makefile.mk APP_BIN_NAME=lcd TARGET=lcd COMPILE=gcc FLASH_SIZE=1M \
  CC="$T/arm-none-eabi-gcc.exe" AR="$T/arm-none-eabi-ar.exe" \
  ASM="$T/arm-none-eabi-gcc.exe" OBJCOPY="$T/arm-none-eabi-objcopy.exe" \
  LINK="$T/arm-none-eabi-ld.exe" OBJDUMP="$T/arm-none-eabi-objdump.exe"
```

产物：

- `bin/lcd/lcd_gz.img`（压缩镜像，`make flash` 默认烧这个）
- `bin/lcd/lcd.fls`（整片镜像）

烧录（W600 需要进 secboot：上电/复位时拉低 boot 脚，串口打印 `CCC...`）：

```bash
make -f Makefile.mk APP_BIN_NAME=lcd TARGET=lcd COMPILE=gcc FLASH_SIZE=1M \
     DL_PORT=COM10 DL_BAUD=921600 flash
```

## 调试提示

- **W600 GPIO 输出必须先 `tls_gpio_cfg(pin, OUTPUT, ...)` 再 `tls_gpio_write(pin, 0/1)`**；
  反过来写会导致电平不生效，表现为驱动低但回读仍为高。本板 I2C、LCD_CS、Flash_CS 都按此顺序。
- 串口没有 `XL9555: found ...`：先确认地址为 `0x24`（A2 上拉），再查 PB09/PB12
  上拉、驱动/回读和 U5 供电、焊接。
- 有初始化日志但屏全黑：先量 FPC 11 脚（LCD_PWR）是否为高；若面板使能是低有效，
  把 `xl9555_init()` 里的 `XL9555_P0_LCD_PWR` 改成 0。
- 花屏/闪烁：把 `LCD_SPI_HZ` 从 20MHz 降到 10MHz。
- 红蓝反了：`0x36` 参数从 `0x08` 改成 `0x00`。
- 图像方向不对：按参考工程改 `0x36` 为 `0xC8/0x78/0xA8` 并同步交换宽高。

## 已知假设

- 屏控制器按参考工程认定为 **ILI9341**（初始化序列完全一致）。
- `LCD_PWR` 按高有效处理；若上电后整屏无反应但初始化串口正常，
  先量 FPC 11 脚电压，必要时把 `xl9555_init()` 里的 `XL9555_P0_LCD_PWR` 取反。
- 面板 16Pin FPC 未引出 MISO，因此不读 ID/不读显存。
- 例程占用 PB11/PB12，SDK 默认的 UART1 不能同时使用。
