# Windows GPIO writes and Linux reset semantics

[Documentation index](README.md)

<a id="windows-gpio-写值与-linux-reset-语义"></a>

Static verification date: 2026-10-04. Sample: the x64 main driver from package 2.0.3.102, SHA-256
`0a4eb56d843e1c3a2b64e37a1e41053e6f863b9dbd2626c59f7669c35dd55b10`. This document records interface
facts and independent analysis, without disassembly or vendor implementation code.

<a id="已确认的原始写值链"></a>

## Confirmed raw-value call chain

| Layer | RVA | Confirmed fact |
| --- | --- | --- |
| Reset helper | `0x2F5E0` | A one-byte buffer is set successively to `01`, `00`, `01`; the first write is followed by 10 ms and the second by 20 ms |
| GPIO wrapper | `0x31414` | Acquires a lock, passes the original buffer pointer to the GPIO writer, and releases the lock; it does not alter the buffer |
| GPIO writer | `0x3106C` | Checks the pointer and GPIO target handle, then creates a WDF request |
| Preallocated-memory wrapper | Near `0x31202` | `WdfMemoryCreatePreallocated` uses the original buffer pointer with `BufferSize = 1`; the value is neither copied nor converted |
| Request formatting | Near `0x31285` | `WdfIoTargetFormatRequestForIoctl` receives IOCTL **`0x00480004`**; the same WDFMEMORY is passed as input and output memory, with both offset arguments null |
| Request submission | Near `0x3130E` | Sends to that GPIO I/O target; this path has no logical inversion, bitwise inversion, or model/polarity lookup |

Therefore, **the vendor's upper-layer 1/0 values are not converted into different values in this
call chain**. Together with the official interface contract below, they describe GPIO-controller pin
output levels, not logical reset assertion values.

“One byte” is the length of the pin-bitmap buffer for the entire GPIO connection. It is not a
separate Windows boolean reset semantic. For a one-pin connection, bit 0 is relevant; a multi-pin
connection also requires interpreting the other bits according to the official pin order. The
sample's upper layer does not supply a separate reset active-low flag.

<a id="gpio-连接如何选择"></a>

## GPIO connection selection

Resource-parser entry `0x2EB20` enumerates the translated resources supplied by WDF:

- The SPI connection is identified by connection resource class 2, type 2.
- A GPIO I/O connection is identified by class 1, type 2; its 64-bit connection ID is read.
- The first GPIO I/O connection is passed from `0x2EDB4` to `0x2FF10` for opening. Additional GPIO
  connections are only logged; they do not select a different model-specific polarity.
- `0x2FF10` creates an I/O target through `\\.\RESOURCE_HUB\<connection ID>` and opens it with
  `GENERIC_WRITE`; there is no logical-assertion parameter.
- Download-mode reopen entry `0x30ABC` reuses the same saved connection ID.
- The IRQ is created from a separate interrupt resource. There is no evidence that IRQ polarity
  controls inversion of reset I/O values.

In the examined path, this GPIO-resource branch neither reads a reset-polarity field nor derives a
1/0 conversion from registry values, DMI model names, or resource flags. This does not supply the
actual ACPI tables or board-level electrical measurements for every computer.

<a id="a8-的完整时序组合"></a>

## Complete A8 timing sequence

FT9348 and FT9361 share `0x39C50`:

1. Execute the GPIO helper once: raw `01`, wait 10 ms, `00`, wait 20 ms, `01`.
2. Wait 10 ms and execute the same GPIO helper again.
3. Wait the object parameter of 160 ms.
4. Through `0x28C54`, send single-byte SPI `70`, wait 5 ms, then send another `70`; wait a further 2
   ms afterward.

Raw GPIO value 1 must therefore not be passed unchanged to a Linux interface whose value means
`asserted`. With an active-low descriptor, Linux `gpiod_set_value*()` converts logical values to
physical levels. Establish the intended physical waveform before choosing descriptor polarity and
call values. Reset output level and IRQ input trigger polarity are separate concepts.

<a id="windows-官方接口核对"></a>

## Verification against official Windows interfaces

Microsoft's
[gpio.h](https://raw.githubusercontent.com/microsoft/win32metadata/main/generation/WinSDK/RecompiledIdlHeaders/shared/gpio.h)
defines `IOCTL_GPIO_WRITE_PINS` with the GPIO device type, function number 1, buffered method, and
any-access. Combining the device type `0x48` and control-code layout in
[devioctl.h](https://raw.githubusercontent.com/microsoft/win32metadata/main/generation/WinSDK/RecompiledIdlHeaders/shared/devioctl.h)
gives `(0x48 << 16) | (1 << 2) = 0x00480004`, exactly matching the sample constant.

The
[IOCTL_GPIO_WRITE_PINS documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gpio/ni-gpio-ioctl_gpio_write_pins)
specifies that each input-buffer bit is written to the corresponding GPIO pin in the connection: bit
0 corresponds to its first pin, and the request applies to all output pins in the connection. This
explains the sample's one-byte bitmap and use of the same input/output memory. The
[GPIO_WRITE_PINS_PARAMETERS documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/gpioclx/ns-gpioclx-_gpio_write_pins_parameters)
further states that the callback writes those bits to the pins and that the write operation has no
defined flags. There is no reset active-low conversion parameter here.

For a one-pin connection, **raw `01 → 00 → 01` commands the controller pin high → low → high**, with
10 ms high and 20 ms low. This conclusion follows from program arguments and the GPIO interface
contract; it is not an oscilloscope measurement of the board's voltage. Board-level inverters, power
domains, or unavailable ACPI/OEM characteristics cannot be excluded by one public package.

<a id="linux-逻辑值与物理电平的对应"></a>

## Linux logical values and physical levels

The
[Linux GPIO consumer documentation](https://docs.kernel.org/driver-api/gpio/consumer.html#the-active-low-and-open-drain-semantics)
states that `gpiod_set_value*()` and initial output values used when acquiring descriptors have
logical active/inactive semantics. With an active-low descriptor, logical 1 outputs physical low and
logical 0 outputs physical high.

| Intent | Windows bit | Target controller level | Linux active-low descriptor value |
| --- | ---: | --- | ---: |
| Released before/after reset | 1 | High | 0 |
| Reset pulse | 0 | Low | 1 |

A Linux reset interface using an active-low descriptor should accept whether reset is asserted.
Logical `0 → 1 → 0` then produces the sample's physical high → low → high. Passing the Windows raw
`1 → 0 → 1` directly as logical assertion values would invert the sequence. `GPIOD_OUT_LOW` requests
initial logical 0, which is physical high on a correctly configured active-low output. That argument
alone does not prove the line stays high throughout descriptor acquisition. Its macro name does not
imply physical low, and the call arguments alone cannot establish glitch-free behavior.

In the
[Linux 6.8 ACPI GPIO implementation](https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/gpio/gpiolib-acpi.c),
`acpi_gpio_to_gpiod_flags()` can derive an initial value from `OutputOnly`, pull configuration, and
polarity. `acpi_gpio_update_gpiod_flags()` can override the caller's direction/initial-value flags;
`GPIOD_ASIS` does not automatically bypass this rule. For example, `OutputOnly + PullDown` on an
active-low reset can require initial logical 1, conflicting with the intended released level.
Changing direction after acquisition cannot undo an incorrect pulse during acquisition. Normal
`_DSD` properties also take precedence over driver mappings, so a mapping's `NO_IO_RESTRICTION` flag
cannot be assumed to override every named-property path.

The glue must therefore check consistency of the resource, named reset property, and bias before
acquiring the GPIO, rejecting configurations that conflict with the intended waveform or cannot be
interpreted unambiguously. This source review establishes an initialization risk that must be
handled; electrical validation is still required to demonstrate glitch-free behavior.

The current glue rejects PullDown and unknown bias before acquisition. If `reset-gpios` /
`reset-gpio` is present, exactly one property with one reference is required; it must be active-low
and reference pin 0 of this device's unique GPIO I/O resource. An active-low driver mapping is
registered only when no named property exists. After acquisition, the underlying descriptor's
active-low setting is checked and logical released output is explicitly configured. Under Linux 6.8,
the accepted PullUp/None/Default cases do not change this intended initial value into logical
assertion. Conflicting firmware descriptions therefore cause probe rejection; this is an explicit
compatibility boundary, not a claim of measured glitch-free operation.

The exported reset-only GPIO chip is a separate interface. Its callbacks forward **raw physical
values** to the underlying GPIO so that the userspace line's active-low conversion is applied only
once. This raw forwarding does not bypass validation of the underlying ACPI resource or conceal a
conflicting reset property. See the [current transport architecture](acpi-spidev.md) for the
exported interface.

The active-low default is justified by the analyzed FTE3600 waveform, not by a polarity field in
`GpioIo`, which has none. A named reset property specifying a conflicting polarity must be handled
explicitly; the report must not continue to claim the same waveform. IRQ trigger polarity is
independently obtained from its ACPI interrupt resource.
