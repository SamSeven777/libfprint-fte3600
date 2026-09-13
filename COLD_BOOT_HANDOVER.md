# FT9361 (One-Netbook A1) Cold Boot Firmware Upload Handover

## 1. 核心问题与突破性发现 (Executive Summary)

### 问题现象
* **冷启动故障**：在 One-Netbook A1 上断电冷启动进入 Linux，指纹模块必然失败，报错：
  `failed to claim device FT9361 MCU did not return to idle after hardware recovery (00 00)`。
* **暖启动正常**：如果**先开机进入 Windows，再软重启进入 Linux**，指纹模块完全正常工作（匹配阈值设定为 7，录入和比对均秒过）。

### 根本原因 (Root Cause)
1. **RAM-Based 8051 MCU**：FT9361 传感器芯片内部是一个基于 RAM 的 8051 单片机，芯片内部**没有片上 Flash 存储应用固件**。
2. **冷启动状态**：彻底断电开机时，RAM 中的固件丢失，芯片停留在内置的 **ROM Bootloader** 模式，完全不识别应用寄存器（如 0x20 MCU_STATUS、0x14/0x15 Sensor ID），SPI MISO 上读回的数据全为 `00 00`。
3. **Windows 行为**：Windows 驱动（`ftWbioUmdfDriverV2.dll`）在初始化时检查 RAM，发现没有固件时输出日志：
   `Firmware is not exist or version update >>>>>> need load Firmware >>>>>>`
   并从 DLL 内部（文件偏移 `0x22a00`）提取出正好 **10,396 字节**（`0x289c`）的 8051 二进制固件，通过 SPI 烧录入芯片 RAM。
4. **暖启动之所以能用**：Windows 烧录固件后软重启，主板对指纹芯片的电源轨保持供电，RAM 固件不丢，Linux 直接读到 `a5 5a`。
5. **Linux 当前驱动缺失**：原驱动假设硬件有 Flash，只做了 GPIO 硬复位，没有实现 Bootloader 固件下发逻辑。

---

## 2. Windows 驱动逆向工程成果 (Reverse Engineering)

通过反编译 `ftWbioUmdfDriverV2.dll`（反编译文本见 `docs/fte3600/dll_disasm.txt`）：

### 关键函数与地址
* `0x18000ea1a`: `clsFT9361::clsFT9361` 构造函数，加载 10,396 字节固件，固件大小 `0x289c`。
* `0x180007579`: `ft_feature_loadfirmware_DistributeSensorFirmware`
* `0x18000f430`: `clsFT9338Base::ft_sensor_sensorbase_DownloadSensorFirmware`（下发固件总流程）
* `0x180008ac0`: 硬件复位与同步握手
* `0x1800095b0`: GPIO 85 复位脉冲（拉低 5ms，拉高）
* `0x1800098b0`: 寄存器写指令封装（Opcode 0x03 -> SPI `0x09 0xf6 <reg> <val>`；Opcode 0x57 -> SPI `0x55 0xaa`）
* `0x180009730`: `clsSpiDev::ft_interface_base_WriteFirmware`（单包下发 10,403 字节）
* `0x18000438c`: 轮询 MCU 状态，读取寄存器 `0x20` 是否为 `0xa5, 0x5a`

### 完整 SPI 烧录时序与报文
1. **硬件复位**：
   * GPIO 85 拉低（0）
   * 延时 5ms
   * GPIO 85 拉高（1）
2. **Bootloader 同步握手**：
   * 发送 2 字节：`0x55, 0xaa`
3. **5 条 Bootloader 解锁指令**：
   * `0x09, 0xf6, 0xc8, 0xff`
   * `0x09, 0xf6, 0xca, 0xff`
   * `0x09, 0xf6, 0xcb, 0xff`
   * `0x09, 0xf6, 0xb9, 0xbf`
   * `0x09, 0xf6, 0xb9, 0xff`
4. **延时**：
   * 延时 20ms
5. **单包完整下发固件 (10,403 字节)**：
   * 包头 6 字节：`0x05, 0xfa, 0x00, 0x00, 0x28, 0x9c`（起始地址 0x0000，固件长度 0x289c = 10,396 字节）
   * 固件主体：10,396 字节（见 `firmware/ft9361.bin`）
   * 包尾：`0x00`
   * 一次性通过单个 SPI Transaction 发送完毕！
6. **启动轮询**：
   * 延时 30ms ~ 50ms。
   * 读取寄存器 0x20（`0x10, 0xef, 0x20, 0x00, 0x00, 0x00`），读到 `a5 5a` 即宣告加载成功！

---

## 3. 准备好的文件与工具 (Prepared Assets)

项目根目录下已准备好的测试文件：
1. `firmware/ft9361.bin`：从 Windows DLL 精确提取的 10,396 字节固件文件。
2. `tools/test_fw_upload.c`：独立的 C 语言固件烧录验证测试程序。
3. `tools/test_hw.c` / `tools/test_cs.c`：底层硬件与片选探测工具。
4. `docs/fte3600/dll_disasm.txt`：完整的 Windows DLL 汇编代码。

### 编译与运行方法
```bash
# 1. 调整 Linux spidev 缓冲大小（单包 10403 > 8192，必须调大，否则内核报 Message too long）
sudo modprobe -r spidev && sudo modprobe spidev bufsiz=32768

# 2. 编译测试程序
gcc -o tools/test_fw_upload tools/test_fw_upload.c -lgpiod

# 3. 运行测试程序
sudo ./tools/test_fw_upload
```

---

## 4. 下一步排查重点 (Next Steps & Open Issues)

在初次运行 `tools/test_fw_upload` 时，报文均已成功发出，但读取状态仍然全为 `00 00`。请在 A1 上重点排查以下两点：

### 疑点 A：指纹模块电源轨 (Power Rail / D-State)
* 在 Linux 冷启动时，指纹芯片的供电（VDD 3.3V）是否处于关闭状态？
* 检查 ACPI：`\_SB.PCI0.SPI1.FPRT` 或 `\_SB.PCI0.SPI1` 是否有电源方法未执行（例如 `_PS0` / `_PR0`）。
* 是否存在额外的电源使能 GPIO 引脚（在 ACPI DSDT 或 Intel LPSS GPIO 中）？如果芯片没电，SPI MISO 将始终全零。

### 疑点 B：GPIO 85 与 Bootloader 握手时延
* 复位释放（拉高）到发送 `0x55 0xaa` 之间的时间窗口是否需要更精细对齐（如微秒级）？
* 检查 Bootloader 是否在 `0x55 0xaa` 或解锁指令时返回特定的 ACK 字节。
