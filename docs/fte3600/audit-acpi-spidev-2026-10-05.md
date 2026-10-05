# acpi-spidev 独立复审：2026-10-05

审计对象：`acpi-spidev` 分支，HEAD
`1ce4c740ac759b69537312f46ac956e39c4a9b53` **及本次审计开始时全部未提交改动**。
下述行号对应此次工作区，不代表该提交中的文件位置。

本轮只审计，没有修改生产代码或原有测试，没有提交、推送或操作真实设备。
复现程序和日志保存在工作区的 `work/` 中。本报告是本轮新增的交付文件。

当时结论：发现四项需要修复的问题，其中两项为 P1。此前测试通过的结果仍然成立，
但其模拟器未覆盖下面的状态和系统行为，不能据此确认真实设备可用。

## 1. P1：FW9369 通信恢复停止手指检测后，可能一直等待中断

位置：`libfprint/drivers/fte3600-fw9369.c:366–374, 633–637, 978–986`。

触发条件：等待按下或释放期间，通信检查失败并经 `wake()` 恢复；随后读取到的
事件不是当前动作需要的按下或明确释放事件。

`wake()` 无条件发送 C0，停止之前的检测模式，并确认 SFR 80 为 50。
事件检查在收到无关事件时，却直接返回 `CAPTURE_WAIT`，没有重新配置并发送 C2
启动检测。`self->armed = TRUE` 仅修改软件标记，并不能启动传感器。
如果硬件不再产生其他中断，动作可能一直等待到取消；无关事件计数上限不能处理
完全没有下一次中断的情形。

验证：在隔离测试副本中约束 C2 启动检测、C0 停止检测，等待中断时必须处于检测状态。
使用实际生产后端代码，首次注入 UP，而动作正在等待 DOWN：

- 启用首次通信失效及恢复：第二次等待中断触发
  `audit_fdt_running should be TRUE`，退出码 -6。
- 保持相同无关 UP 事件，仅禁用首次通信失效：检测保持运行，第二次 DOWN 正常完成采集，退出码 0。

两个场景使用同一组生产对象。对照排除了“无关 UP 测试本身必然失败”的解释。
原有模拟器没有要求产生中断时检测器必须处于运行状态，因而漏掉此问题。

修复方向：记录通信恢复是否停止了检测模式，在需要继续等待时重新布防正确模式，
同时保留释放事件的锁存与确认顺序。不能仅重新设置软件 `armed` 标志。

证据文件：`work/fw9369-audit/reproduce.py`、`result.txt`、`control-result.txt`。
该复现不证明此问题就是公开反馈中关闭后周期性 IRQ 的原因。

## 2. P1：SELinux 标签规则的 change 事件不能保证给现有节点打标签

位置：`scripts/setup-fte3600.sh:117–133`，尤其是 122–124 行。

触发条件：节点已经按基础规则成为 `root:root 0600`，安装 SELinux 策略和标签规则后，
安装器只发送 `udevadm trigger --action=change`；规则要求的属主、属组和权限没有变化。

在核对的 systemd v255 实现中，`apply_mac` 仅对 ADD 事件为真。
节点处理只有在属主、属组、权限发生变化，或 `apply_mac` 为真时，才进入包含显式
SECLABEL 应用的处理块。上述 CHANGE 事件因此可能匹配到规则，却没有真正更新节点标签。
安装器随后检查实际标签并失败；这不是放宽策略权限就能解决的问题。

同样的条件也影响启动时补发事件：GPIO/UIO 首次 ADD 若发生在配对信息完整发布之前，
后续仅补发 CHANGE 不能保证补上标签。

依据：

- [systemd v255 udev-event.c](https://raw.githubusercontent.com/systemd/systemd/v255/src/udev/udev-event.c)：`apply_mac = device_for_action(dev, SD_DEVICE_ADD)`。
- [systemd v255 udev-node.c](https://raw.githubusercontent.com/systemd/systemd/v255/src/udev/udev-node.c)：权限变更或 `apply_mac` 的判断包围显式 SECLABEL 处理。

本项是源代码级确认，没有在 Fedora enforcing 实机上运行安装器。
现有安装测试直接模拟 `stat` 返回目标标签，没有模拟 udev 实际应用标签的条件。

修复方向：对重新核验身份的确切节点采用真正生效的标签设置流程，并覆盖首次 ADD
早于配对信息发布的情况；增加真实标签应用条件的测试，不能只检查触发命令是否被调用。

## 3. P2：GPIO 片选被错误声明为可翻转，探测可能在唤醒前退出

位置：`libfprint/drivers/fte3600-transport.c:523, 662–664`，以及 discovery 的
`IDENTIFY_NEXT_POLARITY` 路径。

触发条件：SPI 控制器使用 GPIO descriptor 驱动片选，当前低有效片选本来正确，
但芯片尚未唤醒，第一轮读取没有身份响应。

用户态无条件声明支持切换 CS 极性。Linux v6.8 stock spidev 在 GPIO 片选模式下，
读取 MODE/MODE32 会隐藏 SPI_CS_HIGH，写入时则有内部强制设置；物理片选极性还涉及
GPIO descriptor。这不是用户态可按普通片选方式翻转并原样回读的能力。
本驱动切换高有效后要求回读一致，因而报错退出，尚未在原本正确的片选下尝试 0x70 唤醒。
glue 的 ACPI 资源数量检查并不能排除控制器采用 GPIO 片选。

依据：[Linux v6.8 drivers/spi/spidev.c](https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/spi/spidev.c)，
MODE/MODE32 读写中的 `use_gpio_descriptors` 和 `spi_get_csgpiod()` 分支。

验证：隔离生命周期测试在 MODE32 回读时模拟该内核行为，设备模型为低有效、需要唤醒的
FT9361。实际生产探测流程在状态 14 返回：

```text
Cannot configure FTE3600 chip-select polarity: Input/output error
no 0x70 wake sent on working CS
```

复现程序退出码 0 表示成功断言了这个失败路径，不代表问题已修复。
证据：`work/audit-2026-10-05/cs_gpio_repro.py`、`cs-gpio-result.txt`。

修复方向：根据实际片选控制方式提供准确的能力信息；不可翻转时仍应允许在当前有效
配置下继续适用的唤醒和恢复流程。本轮没有证据证明 GPD 或 Medion 实际采用 GPIO 片选，
不能直接把该问题归为这两台机器的故障原因。

## 4. P2：配对不完整时卸载遗漏标签恢复，却报告成功

位置：`scripts/setup-fte3600.sh:159–164, 193–198`。

触发条件：glue 已卸载，或 GPIO/UIO 不完整，但 spidev 节点仍存在且曾被设置专用标签。

卸载器把 `pair_data` 失败变成空字符串，因此没有记录任何待恢复的节点。
后续仍删除 SELinux 策略，并因记录为空跳过全部 `restorecon`，最终返回成功。
存活节点的专用标签没有恢复；删除相应策略后，其标签也可能不再有有效的类型定义。

验证：使用现有安装测试环境的隔离副本执行真实卸载脚本，模拟策略已安装、glue 已卸载、
完整配对失败且 spidev 节点存活，结果为：

```text
exit: 0
surviving-spidev-node: True
policy-removed: True
restorecon-calls: []
```

测试只操作临时目录和模拟系统命令，没有修改宿主机策略。
证据：`work/audit_setup_incomplete_pair.py`。

修复方向：为不完整配对保留独立验证存活节点身份和设备号的恢复路径，或者明确报告
清理未完成；不能吞掉配对错误后将卸载视为完全成功，也不能恢复未经确认的其他节点。

## 审计范围与剩余验证

交叉复核了内核 UIO 打开/关闭、移除顺序、复位租约、挂起失效与延迟释放；未在这些路径
发现同等级的新确定问题。这不等于证明不存在其他缺陷。

既有编译与模拟回归记录见 `validation-acpi-spidev-2026-10-04.md`。本轮针对其覆盖缺口
补做隔离复现，没有以重复跑完同一套模拟测试代替系统行为核对。
尚未在 A1、GPD Pocket 3 或 Medion E3224 实机上验证本工作区版本，也没有完成 Fedora
Secure Boot + SELinux enforcing 的完整安装、采集、关闭及恢复验证。

## 后续修复状态

用户随后授权修复并发布。本报告前文保留审计时的失败证据和原始行号；当前代码已修复
这四项问题：FW9369 按需恢复 C2 检测、通过受限 helper 设置确切设备标签、由内核报告
可控 CS 能力、以及独立恢复不完整配对中存活节点的标签。卸载还需先排空 udev 任务并
检查模块未被重新加载，避免在途标签任务覆盖刚恢复的默认标签。

修复后的检查及剩余实机限制见
[发布前验证记录](validation-release-2026-10-05.md)。这些修复不撤回上文对旧模拟器
覆盖不足的结论，也不构成实机验证。
