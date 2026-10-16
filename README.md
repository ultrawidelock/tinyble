# tinyble

A minimal BLE peripheral host for tinytalk's Matter commissioning on the
nRF52833 (DWM3001CDK). It replaces Zephyr's Bluetooth host and drives Nordic's
SoftDevice Controller unchanged through Zephyr raw HCI (`bt_enable_raw()`,
`bt_send()`). Portable C11, Apache-2.0. RESEARCH.md holds the Phase 0 research
and the milestones.

## Size

Measured 2026-10-16 with `make size` and the NCS v3.3.0 toolchain's
arm-zephyr-eabi-gcc 12.2, `-Os`, Cortex-M4, no LTO, optional behaviour off
(the default):

| | Flash (B) | RAM (B) |
|---|---:|---:|
| `src/tinyble.c` | 2,584 | 81 |
| `src/tb_matter_gatt.c` (the table) | 151 | 0 |
| **tinyble** | **2,735** | **81** |
| plus raw HCI and net_buf (RESEARCH.md, measured) | 1,138 | 1,271 |
| **host side** | **3,873** | **1,352** |
| budget | 6,429 | 2,031 |
| Zephyr host today | 32,605 | 10,641 |

Built as one translation unit, which is what LTO sees, tinyble is 2,695 B
flash; with every option on, 3,115 B per file and 3,071 B as one unit
(measured the same day).

## Optional behaviour

Zephyr answers these; none appeared in the first Apple Home commissioning
trace (RESEARCH.md). Each is off unless defined to 1, and `make test` runs the
tests both ways.

| Macro | On | Off |
|---|---|---|
| `TB_FIND_BY_TYPE` | Find By Type Value on primary services | Error, Request Not Supported |
| `TB_READ_BLOB` | Read Blob | Error, Request Not Supported |
| `TB_TYPE128` | Read By Type on a 128-bit type | Error, Attribute Not Found |
| `TB_SIG_REJECT` | Command Reject for unknown signaling | ignored |
| `TB_SMP_REFUSE` | Pairing Failed, Pairing Not Supported, to any SMP PDU | dropped, counted in `tb_rx_dropped` |
| `TB_ATT_TIMEOUT` | 30 s without a confirmation fails the indication and drops the link | the indication waits for its confirmation or the link |

## Commands

| Command | What it does |
|---|---|
| `make test` | Host tests against a fake controller, options off and on |
| `make asan` | The same under AddressSanitizer and UndefinedBehaviorSanitizer |
| `make size ARM_CC=<arm-gcc>` | Cortex-M4 size; fails over 5,291 B flash or 760 B RAM |

On macOS, where `cc` may be a toolchain shim, pass `CC=/usr/bin/clang`.

## Layout

```
include/tinyble/tinyble.h   API, port seams, application hooks, table format
include/tinyble/tb_matter.h handles in the Matter table
src/tinyble.c               HCI, L2CAP fixed channels, ATT server
src/tb_matter_gatt.c        the Matter commissioning GATT table
tests/host/                 fake controller and tests
trace/                      M0: HCI monitor overlay, RTT drainer, btsnoop converter
```

## What keeps it small

1. One ACL packet carries one whole L2CAP PDU. The ATT MTU is
   min(247 B, controller ACL length - 4 B), so nothing is fragmented going
   out; an inbound PDU split across packets is dropped and counted in
   `tb_rx_dropped`. No reassembly buffer.
2. Nothing is queued. Owed packets are bits, built into the port's HCI buffer
   only when the controller has a free ACL buffer.
3. One const GATT table with Zephyr's handle layout, 4 B per attribute: type,
   flags packed with the length, and a 1 B offset into one value array. No
   dynamic database and no service switch.
4. Link-time hooks, not callback tables. No threads, no locks: the port
   serializes every call.
5. Eight HCI commands: event masks, LE Read Buffer Size, random address,
   advertising parameters, data, enable, Disconnect.

## Where it differs from the Zephyr host

Checked against the first commissioning trace (RESEARCH.md); none mattered
to the iPhone.

| Zephyr | tinyble |
|---|---|
| LE Read Remote Features after connecting | not sent |
| Server MTU 251 B in the door-lock build (`BT_BUF_ACL_RX_SIZE=255` minus 4 B) | 247 B, or less on a smaller controller |
| 0xFFF6 registered and withdrawn at runtime | always present, same handles |
| SMP answered with Pairing Failed | not answered (`TB_SMP_REFUSE`: answered, except Pairing Failed) |
| Find By Type Value matches a 16-bit UUID sent in 128-bit form | same width only |

## Open

- M0: one commissioning traced, on the door-lock with 2M PHY and LE Ping
  off. Still to do: the unmodified image and the 10-run success rate.
  `trace/monitor.conf` puts Zephyr's HCI monitor on RTT buffer 1,
  `trace/rtt_drain.py` captures it, `trace/monitor2btsnoop.py` converts it for
  Wireshark.
- M2: the SDC must be built with `CONFIG_BT_CTLR_DATA_LENGTH_MAX=251` for the
  247 B MTU; at 27 B tinyble falls back to MTU 23 B. Watch `tb_rx_dropped`
  with an iPhone.
- The port (`tb_port_*` on raw HCI) and the tinytalk adapter (`tb_on_*` onto
  `matter_ble.h`) are M2 and M3.
