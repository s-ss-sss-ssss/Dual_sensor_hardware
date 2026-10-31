# MagGrad Dual V1 上下位机通信协议 v2

## 配置与传输

默认编译配置为 TMAG 二进制输出；AK 和 ICM 输出默认关闭，可用 `SENSOR_OUTPUT_AK=1`、`SENSOR_OUTPUT_ICM=1` 启用。`SENSOR_OUTPUT_FORMAT=0` 选择原有 CSV 调试模式。

二进制遥测走 USB CDC 字节流，每次主循环生成恰好一个应用协议帧。USB 包和串口 `read()` 边界都不代表帧边界。二进制模式下 `printf` 初始化与诊断日志只走 USART1，不混入 USB 数据。USART1 为 115200 8N1。USB CDC 串口设置的波特率不限制 USB 实际吞吐。

## ROUND 帧

所有多字节数都是小端，有符号数为二进制补码。

| 偏移 | 长度 | 字段 | 内容 |
| ---: | ---: | --- | --- |
| 0 | 2 | sync | `A5 5A` |
| 2 | 1 | version | `2` |
| 3 | 1 | type | `0x10` (ROUND) |
| 4 | 4 | round_seq | 每轮递增的 `u32`，发送或构帧失败仍递增 |
| 8 | 4 | tick_us | 轮首直接读取的 TIM2 计数，1 μs/tick，`u32` |
| 12 | 2 | payload_len | 所有记录的总字节数，`u16` |
| 14 | N | payload | 连续记录 |
| 14+N | 2 | crc16 | CRC-16/CCITT-FALSE，`u16` |

帧长为 `16 + payload_len`。CRC 多项式 `0x1021`，初值 `0xFFFF`，不反转，末异或 `0`；覆盖字节 `[2:14+payload_len]`。缓冲区容量为 4096 字节，因此当前 payload 最大为 4080 字节。接收端应设置不小于此值的合理上限。协议发送 TIM2 的原始 32 位计数，不是 UTC 或上电以来的 64 位时间。

`round_seq` 和 `tick_us` 均按 `2^32` 回绕；`tick_us` 约每 71 分 35 秒回绕一次。传感器读取完成计数为 `(tick_us + read_done_offset_us) mod 2^32`。若一轮构帧失败，固件丢弃整个轮帧；USB 发送失败也不会重发。后续帧的序号空洞可用于发现丢轮。STATS 的 `errors` 包括传感器访问、构帧和 USB 发送失败累计次数。

## Payload 记录

每条记录为 `record_type:u8 | record_len:u16le | record_body:record_len bytes`。接收端按 `record_len` 前进；未知类型可直接跳过。仅 AK、TMAG、ICM 记录在原有传感器内容前加 `read_done_offset_us:u32le`，即追加记录时 TIM2 计数减去轮首计数。TIM2 为 1 μs/tick 的 32 位自由运行计数器，减法按 `2^32` 回绕。该偏移代表传感器读取完成时间，非传感器内部转换完成时间。PHASE、ERR、STATS 不带此偏移。

| 类型 | 名称 | `record_len` | `record_body`，按顺序 |
| ---: | --- | ---: | --- |
| `0x01` | AK | 15 | `offset_us:u32, bus:u8, mask:u8, hx:i16, hy:i16, hz:i16, drdy:u8, err:u8, dor:u8` |
| `0x02` | TMAG | 14 | `offset_us:u32, ch_mask:u8, addr:u8, x:i16, y:i16, z:i16, drdy:u8, overflow:u8` |
| `0x03` | ICM | 18 | `offset_us:u32, ax:i16, ay:i16, az:i16, gx:i16, gy:i16, gz:i16, temp:i16` |
| `0x04` | PHASE | 12 | `ch1_age_us:u32, ch2_age_us:u32, ch3_age_us:u32` |
| `0xE0` | ERR | 7 | `source:u8, code:u16, detail:u32` |
| `0xF0` | STATS | 26 | `ak_frames:u32, tmag_frames:u32, icm_frames:u32, skipped:u32, errors:u32, device_id:u32, errormap:u16` |

传感器未就绪时该轮省略对应记录并增加 `skipped`。访问失败时可追加 ERR，`source` 的 1/2/3 分别指 AK/TMAG/ICM；`code` 1 指通道选择错误，2 指读取错误。`source=4, code=1` 表示此前 USB 发送失败，`detail` 是累计发送失败次数，在下一次成功发送的轮中可见。ERR 当前不含具体传感器实例。PHASE 每轮一条，记录轮首采样时三路最近上升沿的年龄；从未捕获到边沿的通道为 `0xFFFFFFFF`。STATS 约每秒附加一次，计数从主循环开始累计。

### 设备识别码

`device_id` 在 `main()` 初始化区段计算一次，保存在全局变量 `g_device_id`，随每条 STATS 记录发送。它是 STM32 出厂 96 位 UID 的 CRC-32/ISO-HDLC：输入依次为 `HAL_GetUIDw0()`、`HAL_GetUIDw1()`、`HAL_GetUIDw2()`，每个 32 位字按小端展开，共 12 字节。多项式为 `0x04C11DB7`（反射实现为 `0xEDB88320`），初值 `0xFFFFFFFF`，输入/输出反转，末异或 `0xFFFFFFFF`；上位机可用 `zlib.crc32(struct.pack("<III", uid_w0, uid_w1, uid_w2))` 复算。

同一芯片重新烧录或重启后识别码保持一致；它标识 MCU 所在设备，不区分同板的多个传感器。CRC32 是压缩标识，存在碰撞可能，多设备部署时应检查识别码是否重复。

本次扩展保留 ROUND 协议版本 `2`，STATS 原有前 20 字节不变，末尾偏移 20 增加 4 字节 `device_id`。上位机需支持 `record_len=24`。仓库接收脚本能够将新识别码识别为 8 位十六进制，并通过话题发布。

## 接收

本次时间语义修改仍保持协议版本 `2`：帧头为 14 字节，偏移 8 的时间字段仍为 `u32le`，长度、其他字段偏移和 CRC 规则均不变。上位机应将原 `tick_ms` 按 `tick_us` 微秒计数解释，不能再按毫秒换算；旧固件和新固件无法仅凭版本号区分时间单位，需按使用的固件版本配置。

在持久字节缓存中搜索同步字，读取 14 字节帧头并限制 `payload_len`，等到 `16 + payload_len` 字节齐全后验证版本、类型和 CRC，再逐条检查 `record_len` 是否留在 payload 范围内。校验失败时从当前同步字后一字节重新搜索；保留末尾半帧等待下次读取。整轮以一帧直接交给上层，不需要以 PHASE 记录聚合。参考实现：`python tools/usb_read.py --port COM5 --format binary --duration 10`。

协议版本 1 的接收程序必须更新。USB CDC 接收回调目前没有下行命令解析。CSV 模式仍输出原有 AK、TMAG、ICM、PHASE 文本行。

## Sensor failure latch and errormap

STATS body is now 26 bytes. Existing offsets 0..23 are unchanged; offset 24 contains `errormap:u16le`. ROUND version remains 2. Receivers must accept the new record length. Older 20/24-byte STATS have no errormap: report unknown, not healthy.

Bits 0..11 correspond to fixed sensor IDs 1..12; 1 means failed and disabled. Bits 12..15 are reserved and zero. AK and TMAG share the bitmap and cannot be enabled together. ICM is excluded.

- AK: `sensor_id = (bus - 1) * 6 + channel`, with CH1..CH6 and `mask = 1 << channel`.
- TMAG: `sensor_id = (channel - 1) * 3 + (addr - 0x34) + 1`, with CH1..CH4 and addresses 0x34..0x36.
- IDs remain fixed when records disappear; never infer ID from record order. See [sensor mapping](sensor_id_mapping.md).

The first initialization failure latches the bit until MCU restart. Runtime channel-selection/read failures latch only after 3 consecutive failures for that sensor (SENSOR_ERROR_FAILURE_LIMIT). A successful data read resets its streak, even with DRDY=0. Before the threshold, failed rounds omit measurements and report diagnostics but later rounds retry normally. A failed read emits no measurement record in that round. Subsequent rounds skip the disabled sensor entirely; there is no zero placeholder, cached-value replacement, retry, or automatic re-enable. Other healthy sensors continue producing records. TCA selection failure disables all configured sensors on that channel. DRDY=0 is skipped, not latched as failure.

Runtime binary access failures may emit an ERR record once at detection. ERR is diagnostic metadata, not a measurement. The latched state is reported in each subsequent STATS (approximately once per second); STATS is not emitted every round. Initialization failures are visible in errormap; the existing errors counter remains a runtime counter. With all sensors disabled, PHASE and STATS continue. CSV mode also skips failed sensors but does not add a CSV STATS record.

Red LED is on only when at least three bitmap bits are set. ICM faults do not affect this count. Green LED remains the main-loop heartbeat.

Python decoding (payload is the STATS record body, excluding the TLV header):

```python
import struct

errormap = struct.unpack_from('<H', payload, 24)[0] if len(payload) >= 26 else None
failed_ids = None if errormap is None else [
    sensor_id for sensor_id in range(1, 13)
    if errormap & (1 << (sensor_id - 1))
]
```

For example, `errormap=0x0805` means IDs 1, 3 and 12 are disabled, so the red LED is on. Their measurement records are absent; healthy sensors retain their original IDs. A previously received measurement is historical data and must not be reused as a new sample for a missing record.

## Temporary AK timing extension (38-byte STATS)

The existing first 26 bytes remain unchanged. Append three uint32 little-endian fields:

| Body offset | Field | Unit |
| --- | --- | --- |
| 26 | prev_i2c1_us | microseconds |
| 30 | prev_i2c2_us | microseconds |
| 34 | prev_usb_us | microseconds |

All three describe the immediately preceding acquisition round, not averages over
one second. First-round values are zero. The two bus fields sum elapsed wall time
inside TCA-selection and AK 7-byte read calls, including failures/timeouts and
interrupt preemption, excluding record packing. Disabled sensors are excluded.
This instrumentation is AK binary-mode only; bus fields are zero when AK is disabled.
USB time covers USB_Send_Raw, including waiting for readiness/completion or timeout;
zero means no send attempted if round construction failed. It is not host receive latency.
Use TIM2 1-us counter subtraction with uint32 wraparound. STATS cadence remains
approximately once per second; timings are snapshots, not min/max or averages.
Decode using `struct.unpack_from('<III', payload, 26)` when body length is 38.
Receivers retain support for old 20/24/26-byte STATS; timing is unknown when absent.
