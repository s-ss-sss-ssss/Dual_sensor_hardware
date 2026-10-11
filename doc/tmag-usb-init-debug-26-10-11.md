# TMAG 初始化 USB CDC ASCII 调试

## 启用和抓取

调试开关在 `Core/Inc/tmag_debug.h`：

```c
#ifndef TMAG_INIT_DEBUG
#define TMAG_INIT_DEBUG 0
#endif
#ifndef TMAG_INIT_DEBUG_WAIT_MS
#define TMAG_INIT_DEBUG_WAIT_MS 5000U
#endif
```

默认关闭。编译开启，无需改协议或连接 UART 引脚：

```powershell
make -j4 all BUILD_DIR=build/tmag-debug EXTRA_DEFS="-DTMAG_INIT_DEBUG=1"
```

该目录已有调试版 `MagGrad_Dual_V1.elf/.bin/.hex`。使用这里的文件烧录，根目录旧产物不会随独立目录构建更新。

正常版使用：

```powershell
make -j4 all BUILD_DIR=build/tmag-normal
```

更换宏时使用新的构建目录或先清理相应目录，避免复用不同宏的对象文件。

使用电脑串口助手打开 USB CDC 端口，接收模式选择 ASCII/文本。115200 line coding 可照常设置，但它不决定 USB 传输速度。复位后 USB 端口可能重新枚举，需要重新打开。

调试模式在原有 2 秒启动等待之外额外等待 5 秒，随后开始初始化。可以通过 `-DTMAG_INIT_DEBUG_WAIT_MS=10000U` 延长等待以便打开端口。建议保存从 `=== MAG READ START ===` 到最后 `MAG init count ...` 的完整文本。

也可以在调试器中将全局变量 `g_tmag_init_debug` 改成 1/0。变量初始值来自宏；要抓完整初始化日志，应在传感器初始化前改变它。当前没有添加 USB 接收命令，也不会因为运行期改变开关而自动重新初始化。

## 输出范围和判读

开启后，现有 printf 启动日志同时发到 USB CDC；UART 输出保留，但不必连接。新增细节仅用于 TMAG 初始化，不逐帧打印运行期磁场数据。

示例：

```text
[TMAG INIT] debug=1 build=Oct 11 2026 ... wait_ms=5000
[TMAG INIT] RESET I2C3/TCA PC8 low=300ms high=300ms
[TMAG INIT] bus=I2C3 timing=0x00200408 kernel_hz=30000000 SCL=1 SDA=1 mask=0x1E
[TMAG INIT] TCA addr=0x70 mask=0x00 hal=0 err=0x00000000
[TMAG INIT] CH1 mask=0x02 select try=1 hal=0 err=0x00000000 SCL=1 SDA=1
[TMAG INIT] CH1 addr=0x34 BEGIN
[TMAG INIT] addr=0x34 BOOT WRITE reg=0x00 wr=0x00 hal=0 err=0x00000000
[TMAG INIT] addr=0x34 MFG try=1 hal=0 err=0x00000000 id=0x5449
[TMAG INIT] addr=0x34 VERIFY FAIL reg=0x02 wr=0x70 rd=0x00
[TMAG INIT] CH1 addr=0x34 RESULT FAIL hal=1 err=0x00000000
```

输出包括：

- 编译时间、实际 TIMINGR、内核时钟及 SCL/SDA 电平。
- TCA 首次取消选通、通道选通和失败后的 deselect/reselect 状态。
- 每颗器件通道/地址、开始与最终结果。
- 开头两次配置写的结果；保留原来的后续 ID 重试流程，不因首次写失败而提前终止。
- 厂家 ID 每次尝试的 HAL 状态及结果。读取失败时 `id=0xFFFF` 是日志占位，不能当作器件实际返回值。
- 每个配置寄存器的写失败、回读失败，或写值与回读值的比较。
- DRDY 成功、状态读取失败或超时，附带最后状态。

HAL 状态：0=OK、1=ERROR、2=BUSY、3=TIMEOUT。错误码是位掩码，可同时包含多位：BERR=0x01、ARLO=0x02、AF/NACK=0x04、TIMEOUT=0x20。

`VERIFY FAIL` 而 `err=0` 表示 HAL 事务成功但回读值不符；不能把它解释成 ACK 失败。每次具体失败的错误码在日志输出和恢复前保存。最终 `RESULT` 仍使用原驱动返回状态，应优先看前面的失败步骤。

## 数据流和测试边界

初始化后仍继续原来的二进制 ROUND 或 CSV 采集输出。调试二进制版本在同一 USB 通道混合 ASCII 启动日志和二进制帧，串口助手在进入采集后可能显示不可打印字符，这是预期行为。正式抓协议数据时关闭调试宏。默认二进制模式不向 USB 发送 printf 文本。

调试日志通过现有阻塞 USB 发送路径输出，会延长初始化步骤，主机未就绪时还可能丢失日志。因此它适合定位失败位置，不能作为无调试版本的精确时序测量。没有改变传感器寄存器值、I²C 时序、恢复脉冲或初始化成功条件。

离线验证：调试二进制、默认关闭二进制、调试 CSV 三个独立目录构建成功，日志没有编译警告或错误。`python -m unittest discover -s tests -v` 的 15 项测试通过；新增测试使用真实 TMAG 驱动模拟 ID/写入/回读/DRDY 故障，并验证默认关闭、全局开关及 main.c 的 USB printf 路由。未进行实物 USB 抓取或烧录。
