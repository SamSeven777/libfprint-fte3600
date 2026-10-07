# FTE3600 Driver Documentation

[Repository Overview](../../README.md) · [Contributing](../../CONTRIBUTING.md)

This documentation covers the architecture, wire protocols, installation, and hardware status for the FocalTech FTE3600 Linux driver stack.

---

## 1. Supported Hardware Matrix

| Sensor Model | Sensor Geometry | Protocol Family | Resolution / ppmm | Firmware Dependency | Form Factor & Applications |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **FT9338** | 88 × 88 | FT9338 (B38) | 508 DPI (20.0 ppmm) | `ft9338.bin` (14 KB) | Square button capacitive |
| **FT9348** | 96 × 96 | FT95A8 (A8 Family) | 508 DPI (20.0 ppmm) | `ft9348.bin` (10 KB) | Large square capacitive |
| **FT9361** | 64 × 80 | FT95A8 (A8 Family) | 508 DPI (20.0 ppmm) | `ft9361.bin` (10 KB) | One-Netbook A1, laptops |
| **FT9536** | 64 × 128 | FT9338 (Legacy 38) | 508 DPI (20.0 ppmm) | `ft9536.bin` (12 KB) | Elongated rectangular sensor |
| **FT9365** | 64 × 80 | FT9365 | 552 DPI (21.73 ppmm) | None (ROM mode) | Samsung Galaxy Book, laptops |
| **FT9368** | 64 × 80 | FT9368 | Unknown (`0.0`) | App + Pramboot | Dual-stage boot capacitive |
| **FW9369** (ID 9362) | 64 × 80 | FW9369 (Special C6) | Unknown (`0.0`) | None (ROM mode) | GPD Pocket 3, handhelds |
| **FT9769** (ID 9391/2) | 40 × 196 | FT9769 | 564 DPI (22.20 ppmm) | None (ROM mode) | Ultra-narrow side power key |

---

## 2. Core Documentation

| Guide | Description |
| :--- | :--- |
| **[Installation & Setup](install.md)** | Dependencies, kernel ACPI glue module, firmware installation, and enrollment |
| **[Medion E3224 Diagnostic](medion-spidev.md)** | Standalone FT9338 startup, explicit candidate boot, and follow-up capture using stock spidev |
| **[Wire Protocols & Registers](protocols.md)** | SPI framing, register maps, reset timing, and protocol sequences for all 8 sensors |
| **[Architecture & Design](architecture.md)** | Driver subsystem layers, dynamic discovery, BRISK matcher, and clean-room provenance |
| **[Hardware Status](status.md)** | Implementation matrix, physical hardware test reports, and validation limits |
| **[Vendor Sequence Validation](validation-2026-10-06.md)** | Reset/sync, B38 identity, C6 corrections, and regression results |
| **[Troubleshooting](troubleshooting.md)** | Diagnostic checklists, common permission / SPI buffer issues, and log inspection |
