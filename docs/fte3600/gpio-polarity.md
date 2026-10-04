# Windows GPIO 写值与 Linux reset 语义

静态核查日期：2026-10-04。样本为 2.0.3.102 x64 主驱动，
SHA-256：`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`。
本文仅记录接口事实和独立分析，不包含反汇编或厂商实现。

## 已确认的原始写值链

| 层次 | RVA | 已确认事实 |
| --- | --- | --- |
| 复位辅助入口 | `0x2F5E0` | 一个单字节缓冲依次设置为 `01`、`00`、`01`；第一次后等待 10 ms，第二次后等待 20 ms |
| GPIO 包装 | `0x31414` | 获取锁，原样传递上述缓冲指针，调用 GPIO 写入口，释放锁；不修改缓冲 |
| GPIO 写入口 | `0x3106C` | 检查指针和 GPIO 目标句柄，创建 WDF 请求 |
| 预分配内存包装 | `0x31202` 附近 | `WdfMemoryCreatePreallocated` 使用原始缓冲指针，`BufferSize = 1`；没有复制或转换数值 |
| 请求格式化 | `0x31285` 附近 | `WdfIoTargetFormatRequestForIoctl` 的 IOCTL 数值为 **`0x00480004`**；同一 WDFMEMORY 作为输入和输出内存参数，两个偏移参数均为空 |
| 请求发送 | `0x3130E` 附近 | 向该 GPIO I/O target 发出请求；该路径没有逻辑反相、按位翻转、根据机型或极性查表 |

因此可以确定：**厂商上层传入的 1/0 没有在这条调用链内被转换成另一组值。**
结合下面的官方接口核对，这些值描述 GPIO 控制器引脚的输出电平，
不是 reset 的逻辑断言值。

这里的“一字节”是整个 GPIO 连接的 pin 位图缓冲长度，
不是 Windows 为 reset 提供的单独布尔语义。
单针连接时关注第 0 位；多针连接还需根据官方接口的 pin 顺序解释其他位。
样本上层没有传入单独的 reset active-low 标志。

## GPIO 连接如何选择

资源解析入口 `0x2EB20` 枚举 WDF 提供的翻译后资源：

- SPI 连接按连接资源的 class 2、type 2 识别。
- GPIO I/O 连接按 class 1、type 2 识别；读取 64 位 connection ID。
- 第一个 GPIO I/O 连接经 `0x2EDB4` 交给 `0x2FF10` 打开；
  重复 GPIO 连接只记录日志，没有基于整机型号分配另一套极性。
- `0x2FF10` 通过 `\\.\RESOURCE_HUB\<connection ID>` 创建 I/O target，
  以 `GENERIC_WRITE` 打开；未设置“逻辑断言”参数。
- 下载模式的重开入口 `0x30ABC` 继续使用保存的同一个 connection ID。
- IRQ 则按独立中断资源创建；没有证据表明 reset I/O 值取反由 IRQ 极性控制。

此 GPIO 资源分支在已核查路径中没有读取一个 reset 极性字段，
也没有从注册表、DMI 机型或资源 flags 得出 1/0 的转换。
这不等于已经获得每台机器的实际 ACPI 表或板级电气测量。

## A8 的完整时序组合

FT9348 和 FT9361 共用的 `0x39C50`：

1. 执行上述 GPIO 辅助入口一次：原始值 `01`，10 ms，`00`，20 ms，`01`。
2. 等待 10 ms，再执行同样的 GPIO 辅助入口一次。
3. 等待对象参数 160 ms。
4. 经 `0x28C54` 发送单字节 SPI `70`，等 5 ms，再发一次 `70`；随后等 2 ms。

由此不能把“原始 GPIO 值 1”直接传给语义为 `asserted` 的 Linux ioctl。
Linux `gpiod_set_value*()` 若使用 active-low 描述符会进行逻辑到物理的转换，
必须先确定希望产生的物理波形，再设置描述符极性与调用值。
驱动也应区分 reset 的输出电平与 IRQ 的输入触发极性。

## Windows 官方接口核对

Microsoft 公布的 [gpio.h](https://raw.githubusercontent.com/microsoft/win32metadata/main/generation/WinSDK/RecompiledIdlHeaders/shared/gpio.h)
将 `IOCTL_GPIO_WRITE_PINS` 定义为 GPIO 设备类型、功能号 1、buffered 方式和 any-access。
结合 [devioctl.h](https://raw.githubusercontent.com/microsoft/win32metadata/main/generation/WinSDK/RecompiledIdlHeaders/shared/devioctl.h)
中的设备类型 `0x48` 和控制码字段布局，得到 `(0x48 << 16) | (1 << 2) = 0x00480004`，
与样本常量精确一致。

[IOCTL_GPIO_WRITE_PINS 官方说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gpio/ni-gpio-ioctl_gpio_write_pins)
规定输入缓冲每一位写到连接中相应的 GPIO 引脚：第 0 位对应连接中的第一个 pin，
并且请求作用于连接中的所有输出 pin。这解释了样本的一字节缓冲及相同的输入/输出内存。
[GPIO_WRITE_PINS_PARAMETERS 官方说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gpioclx/ns-gpioclx-_gpio_write_pins_parameters)
进一步说明回调将这些位写入 pin，并且该写操作没有已定义的 flags；
这里没有 reset active-low 的逻辑转换参数。

据此，单针连接的 **raw `01 → 00 → 01` 是控制器引脚高 → 低 → 高的输出命令**，
其等待为高 10 ms、低 20 ms。这个结论来自程序参数与 GPIO 接口合同，
不是已经在板端用示波器测量到相同电压。
板级反相器、供电域或未取得的 ACPI/OEM 特性仍不能由一个公开包排除。

## Linux 逻辑值与物理电平的对应

[Linux GPIO consumer 官方说明](https://docs.kernel.org/driver-api/gpio/consumer.html#the-active-low-and-open-drain-semantics)
明确 `gpiod_set_value*()` 以及获取描述符时的输出初始值使用逻辑 active/inactive 语义。
active-low 描述符下，逻辑 1 输出物理低，逻辑 0 输出物理高。

| 操作意图 | Windows 写入位 | GPIO 控制器目标电平 | Linux active-low 描述符逻辑值 |
| --- | ---: | --- | ---: |
| 复位前/后的释放状态 | 1 | 高 | 0 |
| 复位脉冲 | 0 | 低 | 1 |

因此使用 active-low reset 描述符的 Linux 复位接口应该接收“是否断言 reset”，
以 `0 → 1 → 0` 产生样本对应的物理 `高 → 低 → 高`，
而不是把 Windows 的原始 `1 → 0 → 1` 直接当逻辑断言值传入。
`GPIOD_OUT_LOW` 请求初始逻辑 0；在已正确配置的 active-low 输出上，这对应物理高。
但这个参数本身不足以证明描述符获取期间已经保持高电平，不能按宏名推断物理低，
也不能只凭调用参数宣称无毛刺。

[Linux 6.8 的 ACPI GPIO 实现](https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/gpio/gpiolib-acpi.c)
中，`acpi_gpio_to_gpiod_flags()` 可根据 `OutputOnly`、pull 配置与极性推导初值，
`acpi_gpio_update_gpiod_flags()` 可将调用者传入的方向/初始值覆盖；
`GPIOD_ASIS` 也不能自动绕开该规则。
例如 `OutputOnly + PullDown` 在 active-low reset 上可能要求初始逻辑 1，
与目标释放电平冲突。获取后再修改方向不能消除获取阶段可能发生的错误脉冲。
正常 `_DSD` 属性又先于 driver mapping 处理，不能假设 mapping 的
`NO_IO_RESTRICTION` 标志可以覆盖所有命名属性路径。

因此桥接驱动必须在获取 GPIO 前检查资源、命名 reset 属性和 bias 的一致性，
拒绝与目标物理波形冲突或无法无歧义解释的配置。
此源代码核查说明了需要处理的初始化风险；最终是否无毛刺仍需电气验证。

本轮桥接实现采用以下一致性规则：获取 GPIO 前拒绝 PullDown 和未知 bias；
命名 `reset-gpios` / `reset-gpio` 属性若存在，必须唯一、只有一个引用、
active-low，并引用本设备唯一的 GPIO I/O 资源的 pin 0。
没有命名属性时才注册 active-low driver mapping。
获取后检查 active-low 并显式配置逻辑释放输出，不使用 raw 接口掩盖属性冲突。
Linux 6.8 下允许的 PullUp/None/Default 不会把上述初始值改为逻辑断言。
因此具有矛盾固件描述的板卡会拒绝 probe；这是明确的兼容性边界，不是已经实测无毛刺。

这是支持 ACPI 资源、清晰 reset 语义的实现方案。默认 active-low 必须记录为
该 FTE3600 协议的已分析物理波形依据，而不是声称由 `GpioIo` 自带极性字段推导。
若命名 reset 属性明确给出不同极性，必须处理它与期望物理波形的矛盾，
不能在报告中继续声称产生了同一波形。IRQ 触发极性独立取其 ACPI 中断资源。
