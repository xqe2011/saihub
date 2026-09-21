# SAIHub-Mini hardware

A 48 × 28 mm, two-layer ESP32-C5 IO board: USB-C, one BOOT button, a 12-pin right-angle header, and an external dual-band U.FL antenna. Top-side assembly only.

[中文](hardware.zh.md) · [README](../README.md) · [Cookbook](cookbook.md) · [Bring your own board](bring-your-own-board.md) · [KiCad project](../hardware/README.md)

![SAIHub-Mini J2 header pinout](assets/pinout.webp)

## Front edge

Left to right: **12-pin header (J2) → BOOT button → USB-C**. The board width follows these three footprints plus assembly clearances; the connector pins and switch actuator project beyond the outline.

## J2 pinout

Pin 1 is square and marked `3V3`. Looking at the top with the connector edge up, pins run left to right:

| Pin | Name | ESP32-C5 GPIO | Notes |
| --- | --- | --- | --- |
| 1 | 3V3_SW | — | Switched 3.3 V output, enabled by GPIO8, **off by default** |
| 2 | GND | — | |
| 3 | 5V_SW | — | Switched 5 V output, enabled by GPIO9, **off by default** |
| 4 | GND | — | |
| 5 | IO0 | GPIO10 | 3.3 V logic |
| 6 | IO1 | GPIO1 | 3.3 V logic |
| 7 | IO2 | GPIO0 | 3.3 V logic |
| 8 | IO3 | GPIO23 | 3.3 V logic |
| 9 | IO4 | GPIO4 | 3.3 V logic |
| 10 | IO5 | GPIO5 | 3.3 V logic |
| 11 | IO6 | GPIO6 | 3.3 V logic |
| 12 | IO7 | GPIO24 | 3.3 V logic |

Agents drive these through the MCP / REST tools; the mapping lives in firmware `CONFIG_GPIO_LOGICAL_TO_HW` (`main/include/config.h`).

## Power

- **Input**: 5 V / 3 A over USB-C, protected by a 3 A fast fuse and TVS. No PD, no OTG, no source-current detection — just 5.1 kΩ CC pulldowns.
- **3.3 V rail**: SGM6232 buck converter; also feeds an always-on 3.3 V at TP4.
- **Switched outputs**: two TPS2553 power switches feed **3V3_SW** and **5V_SW**. GPIO8 / GPIO9 enable them; both default off and the agent toggles them with `set_output_power_state`.
- **Output protection**: 26.1 kΩ ILIM resistors set a nominal ~1 A limit per channel (0.989 A calculated, 0.908–1.081 A including resistor tolerance). This is a protection threshold, **not a guaranteed continuous 1 A rating**.
- **5V_SW** follows the USB input voltage minus fuse, trace, and switch losses.
- Use a **5 V / 3 A adapter and cable** for external loads; keep outputs off on unqualified USB ports.

## Electrical rules

- Logic pins are **3.3 V only**.
- **Do not back-power** either switched rail.
- Mains voltage belongs on external relay contacts only — never on SAIHub pins.

## Button and test points

| Item | Function |
| --- | --- |
| BOOT (SW1, GPIO28) | Click once while running: Wi-Fi pairing. Hold during reset / power-on: ROM download mode |
| TP1 / TP2 | EN / GND reset pads (top). Short briefly to reset |
| TP3 | Console UART TX (GPIO11, 115200 baud) — firmware logs |
| TP4 | Always-on 3.3 V |
| TP5 / TP6 | Active-low fault outputs of the two power switches |

SW1 is a Panasonic EVQP7A01P (3.5 × 2.9 mm body, 1.35 mm height, side actuator facing the connector edge).

## UART

- UART0 is reserved for the console (logs on TP3).
- The product UART exposed to agents is UART1, routed through the GPIO matrix to any of IO0–IO7 and configured at runtime with `configure_uart`.

## Other onboard parts

- Passive 5020 buzzer on GPIO12 (driven at 4 kHz / 50 % duty).
- Native USB on GPIO13 / GPIO14 (D- / D+), with USBLC6-2SC6 ESD protection.
- ESP32-C5 module: ESPC5-32E-H4, 4 MB flash.

## Antenna

An external dual-band U.FL antenna is **required** for Wi-Fi.

## Design files and status

KiCad 10 project, procurement BOM, schematic and PCB review PDFs: [`hardware/`](../hardware/README.md).

**Prototype status**: fully routed with zero ERC / DRC findings, but not yet fabricated or bench tested. Thermal behavior, supply loading, and USB operation still need verification — see `hardware/agent/docs/prototype-test.md`.
