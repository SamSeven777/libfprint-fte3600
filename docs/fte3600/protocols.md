# FocalTech FTE3600 Wire Protocols and Register Reference

[Documentation Index](README.md) · [Architecture](architecture.md)

This reference consolidates the low-level SPI wire protocols, register maps, frame formats, and command sequences for all 8 supported FocalTech sensor profiles.

---

## 1. Hardware Interfaces and GPIO Reset

### SPI Interface Parameters
- **Mode**: SPI Mode 0 (`CPOL=0, CPHA=0`) or Mode 3 depending on ACPI configuration.
- **Speed**: Standard 12–24 MHz operating clock (safe default: 12 MHz).
- **Word Size**: 8 bits, MSB first.

### Reset & Power Timing
- **Physical Reset Sequence**:
  1. Pull Reset GPIO LOW (active state) for $\ge 20\text{ ms}$.
  2. Release Reset GPIO HIGH (inactive state).
  3. Wait $\ge 10\text{ ms}$ settle time ($\ge 160\text{ ms}$ for A8 firmware reload).

---

## 2. Protocol Family Overview

| Protocol Family | Member Sensors | Geometry | Framing Type | Identification Mechanism |
| :--- | :--- | :---: | :--- | :--- |
| **A8 Family** | FT9348, FT9361 | 96×96, 64×80 | 6-byte header + 2-byte skip + $N$ inverted bytes | Runtime reg `0x14/0x15` (`0x6060`, `0x4050`) |
| **Legacy A8** | FT9338, FT9536 | 88×88, 64×128 | Standard A8 frame after RAM firmware upload | Boot status `0x5858`, `0x4080` |
| **Special (FW9369)** | FW9369 (ID `0x9362`) | 64×80 | Command-driven calibrate & bulk read | C6 mode negotiation + Word ID read `0x9362` |
| **FT93xx (Modern)** | FT9365, FT9769 | 64×80, 40×196 | Direct SPI stream with sample inversion | Word ID read `0x9365`, `0x9391`, `0x9392` |
| **FT9368** | FT9368 | 64×80 | Pramboot loader + App protocol | Special wake handshake `0x5A` + ID read |

---

## 3. Protocol Details by Family

### 3.1 A8 Family Protocol (FT9348, FT9361)

#### Register Access Format
- **Read 1 Byte**: Send `10 EF <reg> 00`, clock out response bytes.
- **Write 1 Byte**: Send `11 EE <reg> <value> 00`.

#### Key Registers
- `0x14, 0x15`: Geometry/Identity signatures (`0x6060` for FT9348, `0x4050` for FT9361).
- `0x1A`: Firmware version (expected `0x30`).
- `0x1D`: Finger presence status (`0x01` or `0xA0` indicates finger contact).
- `0x20`: MCU status (read 2 bytes: `0xA5 0x5A` indicates MCU Idle).
- `0x30`: Config marker (verify `0xBB`).
- `0x76`: Work mode control register.

#### Image Capture Framing
Let $N = \text{Width} \times \text{Height}$. Total transaction size $= N + 8$ bytes.
1. **Send Request**: `04 FB 34 00 <total_hi> <total_lo>` followed by zero-padding.
   - FT9361 ($64 \times 80 = 5120\text{ B}$): Total $= 5128 \implies$ Header: `04 FB 34 00 14 08`
   - FT9348 ($96 \times 96 = 9216\text{ B}$): Total $= 9224 \implies$ Header: `04 FB 34 00 24 08`
2. **Receive & Decode**:
   - First 6 bytes: SPI transport header.
   - Next 2 bytes: Turnaround/padding.
   - Remaining $N$ bytes: Pixel data starting at offset 8.
   - **Inversion**: Each pixel is bitwise inverted: `pixel[i] = ~rx_buffer[8 + i]`.

---

### 3.2 Legacy RAM Recovery (FT9338, FT9536)

FT9338 and FT9536 sensors power up in bootloader mode and require application firmware upload into sensor SRAM on cold boot.

1. **Boot Check**: Read `0x14/0x15`. `0x5858` indicates FT9338; `0x4080` indicates FT9536.
2. **Firmware Upload**:
   - Stream application firmware blocks over SPI using write command `0x12`.
   - Firmware size: `ft9338.bin` (14,184 bytes), `ft9536.bin` (11,934 bytes).
3. **Execution & Handover**: Send jump-to-app command `0x13 0x00 0x00 0x00`.
4. **Post-boot Verification**: Read back register `0x1A` and verify MCU idle status `0xA5 0x5A`. Once loaded, image capture uses standard A8 framing.

---

### 3.3 Special Discovery and FW9369 Protocol (ID 0x9362)

#### Factory Mode Discovery Sequence
1. Send Special Wake byte `0x5A`.
2. Perform Mode C6 configuration loop (up to 31 attempts per pass, 2 configuration passes).
3. Read 16-bit Chip ID register: returns `0x9362`.

#### Operation State Machine
- **Baseline Calibration**: Command `0xC0` triggers background baseline update.
- **Finger Detection**: Poll status register `0x1D` or await hardware IRQ trigger.
- **Image Read**: Send bulk read command `0xC4`, transfer $64 \times 80 = 5120$ pixel bytes.
- **Shutdown / Idle**: Issue command `0xC1` to enter low-power sleep mode and acknowledge pending interrupts.

---

### 3.4 Modern FT93xx Family (FT9365, FT9769)

#### Identification
- Uses 16-bit word-read command `0xC6 <reg_hi> <reg_lo>`.
- Returns `0x9365` for FT9365.
- Returns `0x9391` or `0x9392` for FT9769 ($40 \times 196$ side-key sensor).

#### Capture Pipeline
1. Configure frame exposure and gain registers (`0x40` series).
2. Arm sensor into capture mode (`0x01 = 0x01`).
3. Stream image frame on IRQ event; apply bitwise inversion to reconstruct standard grayscale values.

---

### 3.5 FT9368 Protocol (Dual-Stage Loader)

1. **Pramboot Stage**:
   - Initial handshake reads ROM ID.
   - Upload `ft9368-pramboot.bin` (6,096 bytes).
2. **Application Stage**:
   - Upload `ft9368-app.bin` (27,120 bytes) into main execution space.
   - Jump to main application entry and transition to standard capture state machine.
