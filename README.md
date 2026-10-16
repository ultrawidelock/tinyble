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
| `src/tinyble.c` | 2,704 | 587 |
| `src/tb_matter_gatt.c` (the table) | 151 | 0 |
| **tinyble** | **2,855** | **587** |
| plus raw HCI and net_buf (M2 image, below) | 1,130 | 459 |
| **host side** | **3,985** | **1,046** |
| budget | 6,429 | 2,031 |
| Zephyr host today | 32,605 | 10,641 |

Of the RAM, 502 B is one L2CAP PDU buffer each way. Built as one translation
unit, which is what LTO sees, tinyble is 2,815 B flash; with every option on,
3,263 B per file and 3,223 B as one unit (measured the same day). In the M2 image, below, the Zephyr build itself
measures the host side.

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

## Zephyr port

`zephyr/` makes the repository a Zephyr module: `CONFIG_TINYBLE=y` (it needs
`CONFIG_BT_HCI_RAW=y`) builds the core, `port/zephyr/tb_port_zephyr.c` and,
by default, the Matter table. The port (`include/tinyble/tb_zephyr.h`) runs
tinyble one of two ways: on a thread of the application's that loops on
`tb_zephyr_wait()`, where every `tb_*` call and hook must stay; or on the
system work queue after `tb_zephyr_start()` (needs `CONFIG_POLL`), where other
threads make `tb_*` calls between `tb_zephyr_lock()` and `tb_zephyr_unlock()`.

`samples/m2` is the M2 bench app: Matter advertising, the Matter table, C1
writes indicated back on C2. Built for `decawave_dwm3001cdk`, no LTO,
measured 2026-10-16:

| | Flash (B) | RAM (B) |
|---|---:|---:|
| tinyble core | 2,652 | 587 |
| port | 274 | 20 |
| Matter table | 151 | 0 |
| hci_raw.c and its pools | 558 | 459 |
| net_buf | 572 | 0 |
| **host side** | **4,207** | **1,066** |
| whole image (SDC, MPSL, kernel) | 78,260 | 14,616 |

```
west build -b decawave_dwm3001cdk -d build/m2-dwm samples/m2
west build -b decawave_dwm3001cdk -d build/m2-dwm-mon samples/m2 -- -DEXTRA_CONF_FILE=<abs>/trace/monitor.conf
```

The second adds the HCI monitor for a bench trace (95,888 B flash, 27,936 B
RAM, measured before the five controller lines below); its RTT control block
is at 0x20000410, so pass that to `trace/rtt_drain.py`.

## tinytalk door-lock

`CONFIG_TINYBLE_TINYTALK=y` (with `CONFIG_TINYTALK_BLE=n`) builds
`port/tinytalk/matter_ble_tinyble.c`, which provides tinytalk's `matter_ble.h`
on tinyble, plus `matter_ble_start()` and `matter_ble_advertise()`
(`include/tinyble/tb_tinytalk.h`). tinyble runs on the system work queue;
tinytalk's callbacks run from a work item without the port lock, so the two
locks never nest the wrong way.

`samples/door-lock` is tinytalk's door-lock sample with BLE on tinyble. Raw
HCI leaves five controller options on that tinytalk's build has off, so the
sample (and `samples/m2`) turns them off: `BT_PHY_UPDATE`, `BT_CTLR_PRIVACY`,
`BT_CTLR_FILTER_ACCEPT_LIST`, `BT_CTLR_CONN_RSSI`, `BT_HCI_VS` (3,000 B, no
LTO). It also turns on controller-to-host ACL flow control, which tinyble
never enables: `BT_HCI_ACL_FLOW_CONTROL=n`, with the SDC driver logging at
warning level as in tinytalk's build, saves 584 B flash and 184 B RAM (LTO). Against tinytalk's own door-lock at the same commit, 2M PHY and LE
Ping off in both, measured 2026-10-16 on `decawave_dwm3001cdk`:

| | Flash (B) | RAM (B) |
|---|---:|---:|
| tinyble door-lock, LTO | 272,380 | 87,448 |
| tinytalk door-lock, Zephyr host, LTO | 297,648 | 97,184 |
| **difference** | **-25,268** | **-9,736** |

Without LTO the map attributes the BLE side:

| | tinyble (B) | Zephyr host (B) |
|---|---:|---:|
| host: core, table, port; Zephyr's host | 3,289 / 731 | 30,625 / 10,427 |
| raw HCI, net_buf, extra k_poll | 2,336 / 495 | 887 / 0 |
| **host side** (budget 6,429 / 2,031) | **5,625 / 1,226** | **31,512 / 10,427** |
| tinytalk adapter | 1,437 / 1,370 | 2,890 / 1,575 |
| SDC library and driver | 37,629 / 4,489 | 37,665 / 4,678 |

Flash / RAM. Both builds keep the controller at 27 B data length; tinyble
reassembles and splits L2CAP PDUs. The work queue mode pulls in 828 B more
of `kernel/poll.c`.

```
TINYTALK=<a tinytalk tree> west build -b decawave_dwm3001cdk -d build/m3-tb samples/door-lock
```

## Commands

| Command | What it does |
|---|---|
| `make test` | Host tests against a fake controller, options off and on |
| `make asan` | The same under AddressSanitizer and UndefinedBehaviorSanitizer |
| `make size ARM_CC=<arm-gcc>` | Cortex-M4 size; fails over 5,291 B flash or 760 B RAM |
| `make adapter TINYTALK=<tree>` | The tinytalk adapter against a scripted phone, tinytalk's BTP codec, sanitizers |

On macOS, where `cc` may be a toolchain shim, pass `CC=/usr/bin/clang`.

## Layout

```
include/tinyble/tinyble.h   API, port seams, application hooks, table format
include/tinyble/tb_matter.h handles in the Matter table
src/tinyble.c               HCI, L2CAP fixed channels, ATT server
src/tb_matter_gatt.c        the Matter commissioning GATT table
include/tinyble/tb_zephyr.h Zephyr port API
include/tinyble/tb_tinytalk.h tinytalk adapter API
port/zephyr/                Zephyr port on raw HCI
port/tinytalk/              tinytalk's matter_ble.h on tinyble
zephyr/                     Zephyr module glue (CMake, Kconfig)
samples/m2/                 M2 bench app
samples/door-lock/          tinytalk's door-lock on tinyble (M3)
tests/host/                 fake controller and tests
tests/tinytalk/             adapter test and Zephyr stubs
trace/                      M0: HCI monitor overlay, RTT drainer, btsnoop converter
```

## What keeps it small

1. One L2CAP PDU each way at a time, in one 251 B buffer each: inbound ACL
   packets are reassembled into one, outbound ones split from the other into
   the controller's ACL length. The controller then needs no data length
   update, which costs it more than both buffers (RESEARCH.md).
2. Nothing is queued. Owed packets are bits, each built when the PDU before
   it has gone.
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
- M2: `samples/m2` builds; the bench test is still to do. Connect a phone
  app, discover, subscribe C2, write C1 and expect it back; watch
  `tb_rx_dropped` (0 means every PDU reassembled) and whether 2
  event buffers and 1 ACL TX buffer keep up.
- M3: passed on the bench (RESEARCH.md "M3 bench"): commissioned into Apple
  Home, Thread attached, lock and unlock work. One inbound packet was
  dropped, cause not yet traced. Next: M4's 10 Apple Home runs.
