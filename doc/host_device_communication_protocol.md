# MagGrad Dual V1 上下位机通信协议

## 1. 文档范围

本文档根据当前工程源码整理，描述 STM32H743 下位机向上位机发送
AK09973D、TMAG3001 和可选 ICM42670 数据时使用的通信协议。

当前协议是以下位机连续上传为主的遥测协议。USB 接收回调没有实现命令
缓存或命令解析，因此当前版本不存在上位机到下位机的启停、配置、查询或
应答协议。

主要实现文件：

- `Core/Src/main.c`
- `Core/Src/binary_writer.c`
- `Core/Src/sensor_ak09973d.c`
- `Core/Src/sensor_tmag3001.c`
- `Core/Src/icm42670.c`
- `USB_DEVICE/App/usbd_cdc_if.c`
- `tools/usb_read.py`

## 2. 当前默认配置

`Core/Src/main.c` 中的默认编译配置为：

```c
SENSOR_OUTPUT_ICM   = 0
SENSOR_OUTPUT_AK    = 0
SENSOR_OUTPUT_TMAG  = 1
SENSOR_OUTPUT_FORMAT = SENSOR_OUTPUT_FORMAT_BINARY
```

因此，不带额外编译宏构建时，USB 实际输出：

1. 启动和初始化阶段的 ASCII 调试信息；
2. TMAG3001 二进制数据帧；
3. 约每秒一次的 STATS 帧；
4. TMAG3001 访问失败时的 ERR 帧。

开发板具有 AK09973D 和 TMAG3001 两类磁传感器，但 AK09973D 默认没有
启用输出。需要同时输出两类磁传感器时，可以使用：

```sh
make "EXTRA_DEFS=-DSENSOR_OUTPUT_AK=1 -DSENSOR_OUTPUT_TMAG=1"
```

CSV 调试模式可以使用：

```sh
make "EXTRA_DEFS=-DSENSOR_OUTPUT_FORMAT=0"
```

## 3. 传输接口

### 3.1 USB CDC

正式传感器数据通过 USB Full-Speed CDC ACM 虚拟串口发送。

- USB FS 数据端点最大包长为 64 字节；
- CDC 应用层收发缓冲区均为 2048 字节；
- 上位机示例脚本以 115200 打开虚拟串口；
- USB CDC 的波特率设置不决定 USB 总线的实际传输速率。

USB CDC 是连续字节流。一次上位机 `read()` 可能只取得半帧，也可能同时
取得多帧，USB 包边界不是应用协议帧边界。

下位机等待前一次 CDC 发送完成后再发下一批数据，单次等待超时为 100 ms。
发生超时时，发送函数直接返回，当前代码没有重发或单独记录 USB 丢帧。

### 3.2 USART1

USART1 配置为 115200、8 数据位、1 停止位、无校验、无硬件流控。

`printf()` 输出会同时写到 USB CDC 和 USART1，但二进制传感器帧通过
`USB_Send_Raw()` 发送，只发送到 USB。CSV 主循环也直接调用 USB 发送函数。
因此 USART1 当前主要是调试日志口，不是完整的数据协议接口。

## 4. 二进制协议

### 4.1 通用帧格式

所有多字节字段均采用小端序，有符号数使用二进制补码。

| 字节偏移 | 长度 | 类型 | 字段 | 说明 |
| ---: | ---: | --- | --- | --- |
| 0 | 2 | `u8[2]` | `sync` | 固定为 `0xA5 0x5A` |
| 2 | 1 | `u8` | `version` | 当前协议版本为 `1` |
| 3 | 1 | `u8` | `type` | 帧类型 |
| 4 | 4 | `u32le` | `seq` | 全局帧序号 |
| 8 | 4 | `u32le` | `tick_ms` | MCU 上电后的毫秒计数 |
| 12 | 2 | `u16le` | `payload_len` | Payload 字节数 |
| 14 | N | `u8[N]` | `payload` | 由帧类型决定 |
| 14 + N | 2 | `u16le` | `crc16` | CRC-16/CCITT-FALSE |

完整帧长度为：

```text
frame_len = 16 + payload_len
```

帧类型：

| `type` | 名称 | 说明 |
| ---: | --- | --- |
| `0x01` | AK | AK09973D 数据 |
| `0x02` | TMAG | TMAG3001 数据 |
| `0x03` | ICM | ICM42670 数据 |
| `0xE0` | ERR | 访问错误 |
| `0xF0` | STATS | 累计统计 |

### 4.2 CRC

CRC 参数：

```text
算法       CRC-16/CCITT-FALSE
多项式     0x1021
初始值     0xFFFF
输入反转   否
输出反转   否
最终异或   0x0000
```

CRC 覆盖范围从 `version` 开始，到 Payload 最后一个字节结束；不包含同步头
`0xA5 0x5A`，也不包含 CRC 字段自身。计算出的 16 位 CRC 以小端序放在帧尾。

等价表示为：

```text
crc16 = CRC16_CCITT_FALSE(frame[2 : 14 + payload_len])
```

### 4.3 帧序号

AK、TMAG、ICM、ERR 和 STATS 共用一个 `uint32_t` 全局序号。每生成一帧
递增一次，达到 `0xFFFFFFFF` 后自然回绕。

上位机期望下一序号为：

```text
expected = (previous_seq + 1) & 0xFFFFFFFF
```

数据未就绪而被跳过时不产生帧，也不增加序号。部分错误帧追加路径会在确认
输出缓冲区有足够空间之前增加序号，因此序号跳变通常表示丢帧，但也可能是
下位机输出缓冲区不足造成的序号空洞。

### 4.4 时间戳

`tick_ms` 来自 `HAL_GetTick()`：

- 单位为毫秒；
- 起点为 MCU 上电或复位；
- 不是 UTC 或 Unix 时间；
- 约 49.7 天后发生 `uint32_t` 回绕。

每个传感器帧在封装时单独取得时间戳，同一轮扫描中的各传感器时间戳可能
略有差异。

## 5. AK09973D 数据帧

```text
type        = 0x01
payload_len = 11
frame_len   = 27 bytes
```

| Payload 偏移 | 长度 | 类型 | 字段 | 说明 |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | `u8` | `bus` | I2C 总线编号，1 或 2 |
| 1 | 1 | `u8` | `mask` | TCA9548A 通道掩码 |
| 2 | 2 | `i16le` | `hx` | X 轴原始磁场计数 |
| 4 | 2 | `i16le` | `hy` | Y 轴原始磁场计数 |
| 6 | 2 | `i16le` | `hz` | Z 轴原始磁场计数 |
| 8 | 1 | `u8` | `status` | DRDY，取值 0 或 1 |
| 9 | 1 | `u8` | `err` | 磁场溢出，取值 0 或 1 |
| 10 | 1 | `u8` | `dor` | 数据覆盖，取值 0 或 1 |

AK09973D 共 12 颗，由 `(bus, mask)` 唯一定位：

```text
I2C1: mask = 0x02, 0x04, 0x08, 0x10, 0x20, 0x40
I2C2: mask = 0x02, 0x04, 0x08, 0x10, 0x20, 0x40
```

所有 AK09973D 的下游 7 位 I2C 地址都是 `0x10`，所以数据帧不重复上传
该地址。

原始状态寄存器中，固件使用的状态位为：

| 原始位 | 名称 | 协议字段 |
| ---: | --- | --- |
| bit 0 | DRDY | `status` |
| bit 5 | 磁场溢出 | `err` |
| bit 6 | 数据覆盖 | `dor` |

只有 DRDY 置位时才生成 AK 数据帧，否则只增加 STATS 中的 `skipped`。
`hx/hy/hz` 是原始有符号 16 位计数，下位机没有执行物理单位换算、零偏
校准或坐标变换。

## 6. TMAG3001 数据帧

```text
type        = 0x02
payload_len = 10
frame_len   = 26 bytes
```

| Payload 偏移 | 长度 | 类型 | 字段 | 说明 |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | `u8` | `ch_mask` | TCA9548A 通道掩码 |
| 1 | 1 | `u8` | `addr` | 传感器 7 位 I2C 地址 |
| 2 | 2 | `i16le` | `x` | X 轴原始磁场计数 |
| 4 | 2 | `i16le` | `y` | Y 轴原始磁场计数 |
| 6 | 2 | `i16le` | `z` | Z 轴原始磁场计数 |
| 8 | 1 | `u8` | `status` | DRDY，取值 0 或 1 |
| 9 | 1 | `u8` | `flags` | 磁场溢出，取值 0 或 1 |

TMAG3001 共 12 颗，由 4 个 TCA 通道、每通道 3 个地址组成，并通过
`(ch_mask, addr)` 唯一定位：

```text
ch_mask = 0x02, 0x04, 0x08, 0x10
addr    = 0x34, 0x35, 0x36
```

通道掩码对应关系：

| `ch_mask` | TCA 通道 |
| ---: | ---: |
| `0x02` | CH1 |
| `0x04` | CH2 |
| `0x08` | CH3 |
| `0x10` | CH4 |

`ch_mask` 是位掩码，不是从 1 开始的通道序号。

TMAG3001 原始 `Conv_Status` 中，固件使用 bit 0 作为 DRDY、bit 1 作为
磁场溢出。bit 4 的 POR 状态没有放入通信帧。只有 DRDY 置位时才生成数据
帧，否则只增加 `skipped`。

`x/y/z` 是默认量程下的原始有符号 16 位计数。下位机没有换算为 mT，
也没有在该帧中上传温度。

## 7. ICM42670 数据帧

ICM42670 是工程中的可选 IMU，默认不输出。

```text
type        = 0x03
payload_len = 14
frame_len   = 30 bytes
```

Payload 依次为 7 个 `i16le`：

```text
ax, ay, az, gx, gy, gz, temp
```

字段均为传感器原始计数。

## 8. ERR 错误帧

```text
type        = 0xE0
payload_len = 7
frame_len   = 23 bytes
```

| Payload 偏移 | 长度 | 类型 | 字段 | 说明 |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | `u8` | `source` | 错误来源 |
| 1 | 2 | `u16le` | `code` | 来源内错误码 |
| 3 | 4 | `u32le` | `detail` | 错误详情 |

错误来源：

| `source` | 来源 |
| ---: | --- |
| `0x01` | AK09973D |
| `0x02` | TMAG3001 |
| `0x03` | ICM42670 |

AK09973D 和 TMAG3001 当前错误码：

| `code` | 含义 |
| ---: | --- |
| `1` | TCA9548A 通道选择失败 |
| `2` | 传感器读取失败 |

对两类磁传感器，`detail` 是 `HAL_I2C_GetError()` 返回的错误位图。
ICM42670 读取错误当前使用 `source=3, code=2, detail=0`。

当前 ERR Payload 没有包含 AK 的 `(bus, mask)` 或 TMAG 的
`(ch_mask, addr)`，所以上位机不能总是确定具体发生错误的传感器实例。

## 9. STATS 统计帧

主循环约每秒发送一次累计统计帧：

```text
type        = 0xF0
payload_len = 20
frame_len   = 36 bytes
```

Payload 为 5 个连续的 `u32le`：

| Payload 偏移 | 字段 | 含义 |
| ---: | --- | --- |
| 0 | `ak_frames` | 成功生成的 AK 数据帧累计数 |
| 4 | `tmag_frames` | 成功生成的 TMAG 数据帧累计数 |
| 8 | `icm_frames` | 成功生成的 ICM 数据帧累计数 |
| 12 | `skipped` | 数据未就绪而跳过的累计次数 |
| 16 | `errors` | TCA 或传感器访问错误累计次数 |

这些值是从主循环开始后的累计值，不是最近一秒的增量。上位机应使用相邻
STATS 帧的计数差和时间差计算速率：

```text
rate = (count_new - count_old) * 1000 / (tick_new - tick_old)
```

`last_stats_ms` 初始为 0，而传感器初始化可能持续数秒，所以进入主循环后
第一帧 STATS 通常立即发送；此后才近似每秒发送一次。

## 10. CSV 调试格式

### 10.1 AK09973D

```text
AK,bus,mask,hx,hy,hz,drdy,err,dor\r\n
```

示例：

```text
AK,1,0x02,-123,456,789,1,0,0
```

AK 访问错误：

```text
AKERR,bus,mask,TCA,hal_i2c_error\r\n
AKERR,bus,mask,READ,hal_i2c_error\r\n
```

### 10.2 TMAG3001

```text
TMAG,ch_mask,addr,x,y,z,drdy,overflow\r\n
```

示例：

```text
TMAG,4,0x35,-120,300,452,1,0
```

CSV 中 `ch_mask` 使用十进制输出，因此示例中的 `4` 表示掩码 `0x04`，
不是 CH4。TMAG CSV 路径中的读取失败通常被直接跳过，没有生成与二进制
ERR 帧等价的完整错误行。

### 10.3 ICM42670

```text
ICM,ax,ay,az,gx,gy,gz,temp\r\n
ICMERR,READ\r\n
```

## 11. 上位机流式解析要求

推荐的二进制流解析步骤：

1. 将每次串口读取的数据追加到持久接收缓存；
2. 在缓存中搜索同步字 `0xA5 0x5A`；
3. 缓存不少于 14 字节后读取 `version`、`type` 和 `payload_len`；
4. 对 `payload_len` 设置合理上限，当前上位机脚本使用 128 字节；
5. 等待缓存达到 `16 + payload_len` 字节；
6. 校验版本和 CRC；
7. 校验成功后按 `type` 和精确 Payload 长度解析；
8. 校验失败时，从当前同步字的后一字节重新搜索；
9. 保留缓存末尾的不完整帧，等待下一批串口数据。

下位机上电后会先输出 ASCII 初始化信息，例如：

```text
=== MAG READ START ===
TMAG: init start
...
```

随后才发送二进制帧。上位机不能假设第一个收到的字节就是帧头，必须搜索
同步字并用 CRC 确认帧边界。Payload 本身也可能偶然包含 `0xA5 0x5A`，
因此同步字、长度和 CRC 应组合使用。

工程提供的参考接收程序：

```sh
python tools/usb_read.py --port COM5 --format auto --duration 10
```

在 Windows 上建议显式提供 `--port COMx`。当前自动端口查找逻辑检查设备
路径中是否含 `usb` 或 `modem`，而 Windows 设备名通常只是 `COM5`，可能
无法自动选中。

## 12. 上位机到下位机方向

当前 USB CDC 接收回调仅重新设置接收缓冲区并启动下一次接收，没有保存或
解析收到的数据。USART1 也没有应用层接收中断、DMA 接收或命令状态机。

因此当前版本不支持：

- 数据流启动和停止；
- 传感器选择；
- 采样率或量程配置；
- 状态查询；
- 请求/响应；
- ACK/NACK；
- 参数持久化。

若后续增加下行控制，建议为命令方向使用独立的帧类型，并明确事务号、命令
码、参数长度、ACK/NACK、错误码和超时重试规则。

## 13. PA0～PA3 GPIO 使用核查

### 13.1 配置状态

CubeMX 工程将 PA0、PA1、PA2、PA3 锁定配置为普通 GPIO 输入。在
`MX_GPIO_Init()` 中，四个引脚统一配置为：

```c
GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 |
                      GPIO_PIN_2 | GPIO_PIN_3;
GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
GPIO_InitStruct.Pull = GPIO_NOPULL;
HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
```

即：普通输入、无上拉、无下拉、无外部中断。

### 13.2 应用情况

经核查当前工程应用代码：

- 没有对 `GPIOA, GPIO_PIN_0`～`GPIO_PIN_3` 调用 `HAL_GPIO_ReadPin()`；
- 没有直接读取 `GPIOA->IDR` 的 bit 0～bit 3；
- 没有对 PA0～PA3 配置 EXTI；
- 没有实现 EXTI0～EXTI3 中断处理函数；
- 没有将这些引脚配置为 ADC、定时器输入捕获、串口或其他复用功能；
- 没有与这些引脚对应的应用层名称或业务状态变量。

结论：PA0～PA3 目前只完成了初始化，在固件运行逻辑中没有实际应用。

由于配置为 `GPIO_NOPULL`，如果硬件上这些引脚没有稳定的外部电平，它们会
处于悬空状态。若确定未使用，可以在 CubeMX 中改为模拟模式以关闭数字输入
缓冲、降低噪声敏感性和少量功耗；如果硬件预留给拨码、地址选择或同步输入，
则应根据电路增加合适的上下拉，并在应用代码中明确采样逻辑。

## 14. 已知协议局限

- 默认编译只上传 TMAG3001，AK09973D 虽存在于硬件和协议中但未启用；
- 启动 ASCII 日志和二进制数据共用 USB 字节流；
- USB 发送超时后没有重发机制；
- 磁场数据是原始计数，不含单位、量程标识或校准版本；
- ERR 帧没有包含具体传感器实例标识；
- `skipped` 和 `errors` 没有按传感器类型分别统计；
- 协议没有设备 ID、固件版本或启动会话 ID；
- 当前只有单向遥测，没有下行控制协议。
