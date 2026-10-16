# tinyble: Phase 0 research

A minimal BLE peripheral host in portable C11 that carries Matter commissioning
(BTP over the 0xFFF6 GATT service) for tinymatter on an nRF52833, in place of
Zephyr's Bluetooth host, keeping Nordic's SoftDevice Controller (SDC) and MPSL.

Conventions:

- Every figure is labelled **measured (2026-10-16)** or **estimate**.
- `<NCS>` is the nRF Connect SDK v3.3.0 tree. `tinymatter/` is the tinymatter
  repository. Citations are `file:line`.
- Sizes come from GNU ld maps through `measure/mapsize.py` (flash: sections
  below 0x20000000 plus `.data` load images; RAM: sections at 0x20000000 and
  up). On the door-lock map it sums to within 234 B flash and 78 B RAM of the
  linker's own region report.
- The brief arrived truncated after "Deliver a rep". Items 1 and 2 below are
  my reading of the missing text: (1) a per-module baseline, (2) the SDC
  interface a custom host must drive. Items 3 to 6 follow the brief.

## Summary

| Question | Answer |
|---|---|
| Zephyr host today | 32,605 B flash, 10,641 B RAM (measured 2026-10-16, no LTO) |
| Target (20% of the brief's 32,149 B and 10,157 B) | at most 6,429 B flash, 2,031 B RAM |
| Proposed host side (tinyble plus Zephyr raw HCI with minimal pools) | about 5,898 B flash, 1,650 B RAM (estimate built on 1,138 B and 1,271 B measured) |
| Whole-image saving | about 27.9 KB flash, 9.2 KB RAM (estimate) |
| Controller route | SDC through NCS's own Zephyr driver and raw HCI (route B); direct linking ruled out |
| Replace the SDC too? | No. The open-source link layer is larger (measured) and has no MPSL client for Thread coexistence |
| Recorded iPhone trace | None exists. Capturing one is milestone M0 and needs flashing |
| Decisions (item 5) | Research and experimental; renamed tinyble; the SDC only through NCS's supported driver |

## 1. Baseline: where the Bluetooth bytes go

Build: tinymatter 21cff58 exported with `git archive`, `samples/door-lock`,
board `decawave_dwm3001cdk`, NCS v3.3.0, `measure/no-lto.conf`. The linker
reports 342,744 B flash and 102,048 B RAM. The same tree with LTO reports
309,284 B and 101,984 B. Both match the brief. LTO maps name `ltrans` objects,
so the per-module split below is no-LTO only.

Zephyr host, `libsubsys__bluetooth__host.a` (measured 2026-10-16):

| Object | Flash (B) | RAM (B) | What the RAM is |
|---|---:|---:|---|
| hci_core.c | 8,444 | 3,280 | two 1,088 B thread stacks, `bt_dev` 376 B |
| att.c | 7,979 | 1,486 | 786 B ATT TX pool, slabs |
| gatt.c | 4,964 | 206 | |
| conn.c | 4,932 | 1,404 | 780 B ACL TX pool, `acl_conns` 208 B |
| l2cap.c | 1,854 | 85 | |
| id.c | 1,616 | 0 | |
| adv.c | 1,571 | 0 | |
| buf.c | 643 | 4,096 | net_buf pools, the largest 3,120 B |
| uuid.c | 293 | 0 | |
| smp_null.c | 235 | 84 | |
| addr.c, data.c | 74 | 0 | |
| **Host total** | **32,605** | **10,641** | |

The brief gives 32,149 B and 10,157 B for the same build. Mine is 456 B and
484 B higher; the brief's counting method is not recorded, so the gap is
unexplained. The target uses the brief's lower figures, which is the stricter
choice.

Neighbours (measured 2026-10-16):

| Component | Flash (B) | RAM (B) | Fate with tinyble |
|---|---:|---:|---|
| `subsys/bluetooth/common` | 311 | 159 | goes |
| `lib/net_buf` (used by the host) | 887 | 0 | stays, smaller: raw HCI uses it (556 B measured in the minimal-pool build) |
| SDC, `libsoftdevice_controller_peripheral.a` | 39,056 | 2,627 | stays |
| NCS glue: hci_driver.c | 1,369 | 1,975 | stays (route B); 1,692 B of the RAM is `sdc_mempool`, controller memory |
| NCS glue: hci_internal.c | 1,754 | 76 | stays (route B): it decodes the HCI command packets tinyble builds |
| NCS glue: crypto.c | 66 | 0 | stays |
| `libmpsl.a` | 15,908 | 563 | stays (shared with 802.15.4) |
| MPSL Zephyr glue (init, clock, temp, flash sync, FEM, coex) | 2,113 | 1,465 | stays |
| tinymatter `matter_ble_zephyr.c` | 2,960 | 5,907 | rewritten onto tinyble; 4,096 B work-queue stack and 1,024 B RX buffer are tinymatter's |
| tinymatter `matter_btp.c` | 802 | 0 | unchanged |

## 2. The SDC interface a custom host drives

The SDC's own API, which NCS's driver calls on the host's behalf (tinyble does
not call it; see the route decision below):

| Step | Call | Source |
|---|---|---|
| Init | `sdc_init(fault_handler)` | `<NCS>/nrfxlib/softdevice_controller/include/sdc.h:769` |
| Resources | `sdc_cfg_set(tag, type, &cfg)`: peripheral count, buffer counts, adv count, event length | `sdc.h:796`; glue does this at `<NCS>/nrf/subsys/bluetooth/controller/hci_driver.c:981-1043` |
| Features | `sdc_support_adv()`, `sdc_support_peripheral()`, `sdc_support_dle_peripheral()`, `sdc_support_phy_update_peripheral()`; anything not called is dropped by the linker | `sdc.h:914, 943, 1072, 1130`; `hci_driver.c:738, 747, 777, 787` |
| Enable | `sdc_enable(callback, p_mem)`; the callback says HCI output is ready | `sdc.h:818` |
| Commands | one C function per command returning the HCI status, e.g. `sdc_hci_cmd_le_set_adv_params`, `_set_adv_data`, `_set_adv_enable`, `_set_event_mask`, `_set_data_length`, `_write_suggested_default_data_length` | `sdc_hci_cmd_le.h:4308, 4360, 4468, 4114, 5277, 5326` |
| ACL out | `sdc_hci_data_put(buf)`, payload at most 251 B | `sdc_hci.h:101, 53` |
| Events and ACL in | `sdc_hci_get(buf, &type)`, buffer at least `HCI_MSG_BUFFER_MAX_SIZE` = 258 B | `sdc_hci.h:136, 74, 62` |

The SDC is selected by `BT_LL_SOFTDEVICE`, which lives under `CONFIG_BT`
(`<NCS>/nrf/subsys/bluetooth/controller/Kconfig:7`). Two routes were weighed:

| Route | Cost | Status |
|---|---|---|
| A. Link the SDC library from tinyble's own module with `CONFIG_BT=n` and call `sdc_*` directly | smallest; drops hci_internal.c | **Ruled out** (decision, item 5): unsupported by NCS Kconfig |
| B. `CONFIG_BT=y` + `CONFIG_BT_HCI_RAW=y`: NCS's SDC driver unchanged, tinyble speaks HCI packets through `bt_enable_raw()` and `bt_send()` (`<NCS>/zephyr/include/zephyr/bluetooth/hci_raw.h:39, 51`) | hci_raw.c 582 B flash; pools 5,967 B RAM at defaults, **1,271 B RAM** at 2 event and 1 ACL TX buffers; net_buf 556 B flash (all measured 2026-10-16, hci_uart builds) | **Chosen**. Whether 2 event buffers keep up with an iPhone is proven in M2 |

What changes against NCS today on route B:

| Today (NCS) | tinyble |
|---|---|
| Zephyr host packs HCI command packets; hci_internal.c (1,884 lines) unpacks them into `sdc_hci_cmd_*` calls | Same: tinyble packs the handful of commands it needs; hci_internal.c stays |
| hci_driver.c (1,546 lines) feeds Zephyr's HCI driver API | Same driver; hci_raw.c queues each packet on a FIFO tinyble owns |
| Two host threads with 1,088 B stacks; pools of 10 event and 7 ACL TX buffers | No threads: tinyble drains the FIFO in the caller's work context behind the lock seam; 2 event and 1 ACL TX buffers |

## 3. What an iPhone does while Apple Home commissions over BLE

Sources: Zephyr host (Apache-2.0) as the reference answers, connectedhomeip's
BLE layer (`<NCS>/modules/lib/matter/src/ble`), its Darwin central code as the
closest open view of CoreBluetooth use (Apple Home itself is closed), and
tinymatter's port comments, which record behaviour seen with a real iPhone.

**No trace exists.** A search of tinymatter, ultrawidelock and sibling
directories found no RTT transcript, btsnoop or sniffer capture of a BLE
commissioning; the `build/*.log` files are compile output. The brief's "logs
from the tinymatter sample at DBG" therefore could not be used, and making one
needs flashing, which Phase 0 forbids. Rows marked *inferred* need that trace.

| # | Procedure (layer) | Started by | Answer tinyble must give | Source |
|---|---|---|---|---|
| 1 | Legacy connectable advertising: Flags 0x06, Service Data 0xFFF6 with 8 B ChipBLEDeviceIdentificationInfo; no scan response | us | ADV_IND every 100 ms to 150 ms (0xA0 and 0xF0 times 0.625 ms) as today | `tinymatter/samples/door-lock/src/main.c:70-91`; `<NCS>/zephyr/include/zephyr/bluetooth/gap.h:62-64`; `tinymatter/ports/zephyr/matter_ble_zephyr.c:664-725` |
| 2 | Connection (LL) | iPhone | LE Connection Complete event; record handle; advertising stops | Core spec, handled by SDC |
| 3 | LL version, features, length, PHY, connection parameter, channel map procedures | iPhone | Answered by the SDC. Host ignores Data Length Change, PHY Update Complete, Connection Update Complete. Keep LE Remote Connection Parameter Request masked; controller behaviour when masked: verify in M2 | SDC (inferred) |
| 4 | Data length update | us, at connect | `LE Set Data Length` to 251 B, as Zephyr does today (`CONFIG_BT_AUTO_DATA_LEN_UPDATE=y`) | `.config` of the measured build |
| 5 | L2CAP Connection Parameter Update Request (code 0x12, CID 0x0005) | us, 300 ms after connect | Interval 15 ms to 30 ms, latency 0, timeout 2 s; accept the iPhone's Response (0x13) | `tinymatter/samples/door-lock/prj.conf` (`BT_CONN_PARAM_UPDATE_TIMEOUT=300`, PREF 12, 24, 0, 200); `<NCS>/zephyr/subsys/bluetooth/host/l2cap.c:2333` |
| 6 | ATT Exchange MTU Request (0x02) | iPhone | Exchange MTU Response, server MTU 247 B. The iPhone settled a BTP fragment of 244 B, so its MTU is at least 247 B (inferred) | `att.c:2817`; `matter_ble_zephyr.c:87`; `tinymatter/src/matter_btp.c:165-195` |
| 7 | Read By Group Type (0x10) on 0x2800: all primary services | iPhone | Group list, then Error 0x0A Attribute Not Found past the last handle | Darwin `discoverServices:nil`, `<NCS>/modules/lib/matter/src/platform/Darwin/BleConnectionDelegateImpl.mm:481`; `att.c:2853` |
| 8 | Read By Type (0x08) on 0x2803: all characteristics | iPhone | Declarations per service, then Error 0x0A | `BleConnectionDelegateImpl.mm:502`; `att.c:2829` |
| 9 | Find Information (0x04): descriptors, to find C2's CCC | iPhone (inferred) | Handle and UUID pairs, then Error 0x0A | `att.c:2821` |
| 10 | Find By Type Value (0x06) | unknown whether iOS uses it | Answer it (cheap) or Error 0x0A | `att.c:2825` |
| 11 | Write Request (0x12) 0x0002 to C2's CCC | iPhone | Write Response (0x13), then and only then any indication. Indicating first made the iPhone unsubscribe and drop the link | `<NCS>/modules/lib/matter/src/platform/Darwin/BlePlatformDelegateImpl.mm:68`; `matter_ble_zephyr.c:114-124, 355-363` |
| 12 | Write Request to C1: BTP handshake request, then BTP fragments | iPhone | Write Response; also accept Write Command (0x52), which C1 advertises | `BlePlatformDelegateImpl.mm:121` (`CBCharacteristicWriteWithResponse`); `matter_ble_zephyr.c:377-379` |
| 13 | Order of 11 and 12 | iPhone | Either order. tinymatter's own comments disagree (subscribe first at `:59-62`, write first at `:81-83`), so both must work | `matter_ble_zephyr.c` |
| 14 | Handle Value Indication (0x1D) on C2, Confirmation (0x1E) back | us | One indication outstanding; 30 s ATT timeout | `<NCS>/zephyr/subsys/bluetooth/host/att_internal.h:23`; `matter_ble_zephyr.c:554-556` |
| 15 | Read Request (0x0A) of GAP Device Name 0x2A00 or Appearance 0x2A01 | iPhone (inferred) | Value from the const table; keep the name at most 22 B so Read Blob (0x0C) is never needed | `<NCS>/zephyr/subsys/bluetooth/host/gatt.c:245-272` |
| 16 | Service Changed (0x2A05) CCC write | iPhone (inferred) | Only if 0x1801 carries Service Changed; Zephyr does today (`CONFIG_BT_GATT_SERVICE_CHANGED=y`). See note below | `gatt.c:1203-1210` |
| 17 | Any other request: Prepare or Execute Write, Read Multiple, Read Multiple Variable, Signed Write | none expected | Error Response, Request Not Supported (0x06); drop commands silently | `att.c:2817-2947` |
| 18 | L2CAP LE Credit Based Connection Request or unknown code | none expected | Refuse (LE_PSM not supported) or Command Reject "not understood" | `l2cap.c:2105, 1137` |
| 19 | SMP Pairing Request (CID 0x0006) | none expected | Pairing Failed, reason 0x05 Pairing Not Supported | `<NCS>/zephyr/subsys/bluetooth/host/smp_null.c:68` |
| 20 | Disconnection | iPhone after commissioning, or us on BTP desync (reason 0x13) | Reset link state, readvertise when free | `matter_ble_zephyr.c:319, 647-656`; `main.c:103-112` |
| 21 | C3 read | iPhone only if the additional-data flag is set | Omit C3: tinymatter advertises the flag as 0 | `<NCS>/modules/lib/matter/src/ble/CHIPBleServiceData.h:55, 109`; `matter_ble_zephyr.c:722` |

BTP itself stays in tinymatter's core and needs nothing from the host beyond
rows 11 to 14. For reference: handshake magic 0x65 0x6C and version 4 only
(`<NCS>/modules/lib/matter/src/ble/BleLayer.h:89-99`), fragment size the
iPhone's MTU minus 3 B, floor 20 B, cap 244 B (`tinymatter/src/matter_btp.c:165-195`,
`tinymatter/include/tinymatter/matter_btp.h:49-51`), connect-response and ack
timeouts 15,000 ms (`BleConfig.h:215, 226`), ack-send timeout 2,500 ms
(`BLEEndPoint.cpp:81`), receive window at most 6 (`BleConfig.h:166`).
The Matter spec's BTP and advertising sections are not cited by number: no
copy was opened in this session.

Service Changed note: tinytalk registers and withdraws 0xFFF6 at runtime
because macOS answers a descriptor discovery on C1 with CBError 8 and then
poisons its cache for the device (`matter_ble_zephyr.c:365-372`, measured in
ultrawidelock on 2026-08-27). The brief asks for a fixed table. Both are
possible: a fixed table whose 0xFFF6 entry is switched on and off, plus
Service Changed. Which the iPhone needs is a trace question (M0).

## 4. Controller option: replace the SDC too?

Builds: `<NCS>/zephyr/samples/bluetooth/hci_uart` on `nrf52833dk/nrf52833`,
raw HCI with no host, one peripheral link, legacy advertising, DLE 251 B, 2M
PHY (`measure/hci-periph.conf`, plus `sdc-periph.conf` or `ll-sw.conf` and
`ll-sw.overlay`). Coded PHY had to be turned off: it silently forces the
multirole SDC library (`<NCS>/nrf/subsys/bluetooth/controller/Kconfig:303-332`).

| | Keep SDC | Zephyr open-source link layer |
|---|---|---|
| Controller flash | 43,725 B (SDC 40,624 + glue 3,101), measured 2026-10-16 | 55,555 B, measured 2026-10-16 |
| Controller RAM | 4,678 B (2,627 + 2,051), measured 2026-10-16 | 11,995 B, measured 2026-10-16 |
| Whole hci_uart image | 84,196 B flash, 19,840 B RAM, measured | 84,440 B flash, 24,384 B RAM, measured |
| MPSL | 12,950 B + 883 B glue, needed for Thread anyway | needed for Thread anyway |
| Thread coexistence | through MPSL, shipping today | none: the LL has no MPSL client (only a readme mentions MPSL) and claims RADIO, TIMER and RTC itself (`<NCS>/zephyr/modules/hal_nordic/nrfx/nrfx_reserved_resources.h:57-69`). Needs a port of its lower layer onto `mpsl_timeslot_session_open` / `mpsl_timeslot_request` (`<NCS>/nrfxlib/mpsl/include/mpsl_timeslot.h:278-349`, slots 100 µs to 100 ms, `:32-35`) |
| Maturity | Nordic production controller | Kconfig marks it EXPERIMENTAL (`<NCS>/zephyr/subsys/bluetooth/controller/Kconfig:143-148`) |
| Timing risk | low | high: 150 µs inter-frame timing inside timeslots that 802.15.4 can preempt |
| Interop risk | low | medium to high: its own qualification, its own iPhone interop |
| Licence | binary, Nordic ICs only | Apache-2.0 |

In the Thread product MPSL stays either way, so the relevant comparison is
43,725 B against 55,555 B plus a timeslot adapter: replacing the SDC costs at
least 11,830 B flash and 7,317 B RAM (measured difference, before the adapter).

Recommendation: do not attempt it. Revisit only if (a) the Nordic licence
blocks something the project needs, (b) a non-Nordic SoC enters scope, or
(c) a purpose-built peripheral-only link layer is wanted as research after the
host ships. A from-scratch peripheral-only link layer could plausibly be
15 KB to 20 KB (estimate), but its timing work under MPSL dominates the cost.

## 5. Decisions

| Question | Decision |
|---|---|
| Scope | Research and experimental |
| Name | tinyble (was tinybluetooth) |
| The SoftDevice Controller | Reached only through NCS's supported Zephyr driver (route B, item 2) |

## 6. Plan

Components (all estimates):

| Component | Scope | Flash (B) | RAM (B) |
|---|---|---:|---:|
| hci | raw HCI seam (`bt_enable_raw`, `bt_send`), pack about 8 commands, parse events, init sequence | 1,500 | 40 (packets live in raw HCI's net_bufs) |
| gap | adv params, data, enable; connection state; disconnect; data length | 400 | 40 |
| l2cap | fixed-CID demux, one-PDU reassembly, Conn Param Update Request and Response, Command Reject, credit-based refuse | 500 | 260 (251 B RX PDU) |
| att | MTU, Find Info, Find By Type Value, Read By Type, Read, Read Blob, Read By Group Type, Write Request and Command, Indication and Confirmation, Error, 30 s timeout | 1,800 | 15 (replies built in the ACL TX net_buf) |
| gatt | const table: 0x1800 name and appearance, 0x1801 (Service Changed if kept), 0xFFF6 C1, C2, CCC; on/off switch | 350 | 8 |
| smp | Pairing Not Supported | 60 | 0 |
| seams | timer, lock, log | 150 | 16 |
| **tinyble** | | **4,760** | **379** |
| hci_raw.c, minimal pools (measured 2026-10-16) | | 582 | 1,271 |
| net_buf (measured 2026-10-16) | | 556 | 0 |
| **Host side** | | **5,898** | **1,650** |
| Target | 20% of 32,149 B and 10,157 B | 6,429 | 2,031 |

Margins are thin: 531 B flash and 381 B RAM. Code copied from Zephyr must be
trimmed to what item 3 needs, not taken whole.

Outside the host: the matter_ble.h adapter on tinyble, about 2,000 B
flash (estimate) against 2,960 B today; its RAM is tinymatter's.

Expected whole-image change (estimate): flash falls by about 27,905 B (host
32,605 + common 311 + net_buf 887, less 5,898); RAM falls by about 9,150 B
(host 10,641 + common 159, less 1,650). The NCS glue stays as it is.

Host test strategy:

1. Host tests first, portable C11 under sanitizers like tinymatter's: a fake
   controller seam feeds HCI events and ACL in and records commands and ACL out.
2. Known-answer tests from recorded exchanges: replay every central PDU of the
   M0 trace and compare our reply byte for byte with Zephyr's reply in the same
   trace. Keep the handle layout identical to Zephyr's so replies compare
   directly.
3. Negative tests: unsupported opcodes, short and over-long PDUs, MTU below
   23 B, writes to unknown handles, a second indication while one is
   outstanding, ATT timeout, disconnect mid-indication, fragmented L2CAP.
4. A fuzz target over the ACL input path.
5. Size gate in CI: `measure/mapsize.py` on the door-lock no-LTO map.

Milestones and gates:

| Milestone | Work | Go / no-go |
|---|---|---|
| M0 | Flash the current Zephyr door-lock build with an HCI monitor (for example Zephyr's RTT monitor output) and record one Apple Home commissioning, connect to disconnect. Also measure today's success rate over 10 runs | Go if the trace holds no procedure outside item 3, or at most 2 new ones that fit the budget |
| M1 | Host core and tests against the fake controller; KAT replay of M0 | Go if all KATs pass and the host builds within 6,429 B flash and 2,031 B RAM |
| M2 | nRF52833 on route B: raw HCI with minimal pools, advertising, one connection, GATT discovery from a phone app, Thread attached | No-go if 2 event and 1 ACL TX buffers drop packets with an iPhone and the larger pools break 2,031 B; stop and reassess |
| M3 | matter_ble.h seam on tinyble; door-lock builds with `CONFIG_TINYTALK_BLE=n` | Go if the image builds and Thread still attaches on the bench |
| M4 | Apple Home on hardware, Thread enabled | Ship if 10 of 10 commissionings succeed (at least the M0 baseline) and Thread operation after commissioning is unchanged |

## Reproduce

```
west build -p always -b decawave_dwm3001cdk -d build/dwm-nolto <tinymatter@21cff58>/samples/door-lock -- -DEXTRA_CONF_FILE=measure/no-lto.conf
west build -p always -b nrf52833dk/nrf52833 -d build/hci-sdcp <NCS>/zephyr/samples/bluetooth/hci_uart -- "-DEXTRA_CONF_FILE=measure/hci-periph.conf;measure/sdc-periph.conf"
west build -p always -b nrf52833dk/nrf52833 -d build/hci-sdcp-min <NCS>/zephyr/samples/bluetooth/hci_uart -- "-DEXTRA_CONF_FILE=measure/hci-periph.conf;measure/sdc-periph.conf;measure/hci-min-pools.conf"
west build -p always -b nrf52833dk/nrf52833 -d build/hci-llsw <NCS>/zephyr/samples/bluetooth/hci_uart -- "-DEXTRA_CONF_FILE=measure/hci-periph.conf;measure/ll-sw.conf" -DEXTRA_DTC_OVERLAY_FILE=measure/ll-sw.overlay
python3 measure/mapsize.py <build>/zephyr/zephyr.map 'subsys/bluetooth/host' 'softdevice_controller' '__nrf__subsys__bluetooth__controller' 'libmpsl'
```

Pass absolute paths to `EXTRA_CONF_FILE`; west runs from the NCS workspace.

## 7. Phase 1 corrections (2026-10-16)

Found while building M0's capture image and tinyble. Each is from the NCS
v3.3.0 source or the door-lock build's `.config`; none is from a trace yet.

| Phase 0 said | Found | Source |
|---|---|---|
| Row 4: DLE 251 B, Zephyr sends LE Set Data Length | The door-lock build runs `CONFIG_BT_CTLR_DATA_LENGTH_MAX=27`: `BT_BUF_ACL_RX_SIZE=255` fails the `<= 251` default and drops it to 27. Zephyr sends LE Set Data Length only on `bt_drv_quirk_no_auto_dle()`, which NCS's driver does not set | `<NCS>/zephyr/subsys/bluetooth/controller/Kconfig:611-617`; `<NCS>/zephyr/subsys/bluetooth/host/conn.c:1840-1853` |
| Row 5: L2CAP Connection Parameter Update Request | Zephyr uses HCI LE Connection Update when both sides support the LL procedure (an iPhone does, inferred) and falls back to L2CAP on 0x1A. It reads remote features first | `conn.c:2129-2166`, `:1805-1813`; `hci_core.c:2052-2064` |
| Row 6: server MTU 247 B | 251 B in this build (`BT_L2CAP_RX_MTU` is `BT_BUF_ACL_RX_SIZE` minus 4 B), inferred | `.config` |
| Row 18: refuse LE credit based requests with LE_PSM not supported | Without `BT_L2CAP_DYNAMIC_CHANNEL` Zephyr answers them with Command Reject, not understood | `l2cap.c:2305-2388` |
| `tinymatter/` | Renamed tinytalk (commit cd964ea) | tinytalk git log |
| M0 image | Building the door-lock with `trace/monitor.conf` adds 688 B flash and 8,192 B RAM: 309,972 B and 110,176 B (measured) | `build/dwm-m0.log` |

tinyble (README.md) measured 3,119 B flash and 85 B RAM on 2026-10-16 against
the 4,760 B and 379 B estimated in section 6.

### Controller trims (measured 2026-10-16)

`hci_uart` on `nrf52833dk/nrf52833` with `hci-periph.conf`, `sdc-periph.conf`
and `hci-min-pools.conf`, one option changed per build. Section 4's builds
also ran at `DATA_LENGTH_MAX=27`, the same trap as the door-lock: its
43,725 B and 4,678 B are for 27 B buffers.

| Change | Flash | RAM | Note |
|---|---:|---:|---|
| Baseline | 84,196 B | 15,144 B | DLE 27 B, SDC 3 TX and 2 RX buffers |
| `BT_CTLR_PHY_2M=n` | -1,104 B | 0 | iPhone stays on 1M PHY |
| `BT_CTLR_LE_PING=n` | -224 B | 0 | LE Ping only runs on an encrypted link |
| `BT_CTLR_CHAN_SEL_2=n`, `BT_CTLR_ASSERT_DEBUG=n` | 0 | 0 | Zephyr link layer options; the SDC ignores them |
| Privacy, filter accept list, connection RSSI off | -1,392 B | 0 | Already off in the door-lock. Derived: all five off gave -2,720 B, minus the rows above |
| `dle251.conf` (real DLE 251 B) | 0 | +1,080 B | All in the controller's memory pool |
| `dle251.conf` plus `sdc-buf1.conf` | 0 | +312 B | One SDC TX and RX buffer; whether it keeps up is an M2 question |

`ctlr-lean.conf` holds the first two. Against the M2 plan (DLE 251 B at the
default 3 and 2 buffers) the trims save 1,328 B flash (sum of the single builds) and 768 B RAM.

On the DWM3001CDK door-lock image (`trace/monitor.conf` plus
`ctlr-lean.conf`, `build/dwm-lean`): 308,660 B flash against M0's 309,972 B,
-1,312 B, RAM unchanged at 110,176 B (measured 2026-10-16). Still to check
on the bench: one Apple Home commissioning on 1M PHY.

```
west build -p always -b nrf52833dk/nrf52833 -d build/o-<x> <NCS>/zephyr/samples/bluetooth/hci_uart -- "-DEXTRA_CONF_FILE=measure/hci-periph.conf;measure/sdc-periph.conf;measure/hci-min-pools.conf;<the change>"
```

### First commissioning trace (2026-10-16)

`build/dwm-lean` (Zephyr host, `ctlr-lean.conf`), one Apple Home
commissioning, captured with `trace/rtt_drain.py` on up-buffer 1 and decoded
with `trace/monitor2btsnoop.py`: 560 HCI packets, the monitor lost 1 ACL RX
and 1 ACL TX record. The trace stays in `build/` (it holds the iPhone's
address). Commissioning succeeded on 1M PHY.

| Section 3 said | Trace shows |
|---|---|
| Row 4: DLE 251 B | DLE stayed 27 B both ways; 185 B PDUs went as 27 B ACL fragments |
| Row 5, and section 7's "Zephyr uses HCI LE Connection Update" | Zephyr read remote features, then sent the L2CAP request anyway, 300 ms after connect; the iPhone accepted |
| Row 6: iPhone MTU at least 247 B (inferred) | The iPhone asked for 185 B; BTP fragment 182 B |
| Rows 7 to 9 | As expected: Read By Group Type, Read By Type 0x2803, Find Information |
| Row 13: order | C1 handshake write first, then Find Information on C2's CCC, then the CCC write |
| Row 15: Read of Device Name | Read By Type on 0x2A00 (16-bit), not Read; Appearance and PPCP never read |
| Row 16: Service Changed CCC (inferred) | Written (0x0002) during discovery |
| Rows 10, 17 to 19 | None: no Find By Type Value, Read, Read Blob, 128-bit Read By Type, Write Command, SMP, or signalling to reject |
| Not listed | One 7 B PDU on fixed CID 0x003A right after connect; Zephyr ignored it |
| Timing | Connect to disconnect 65.9 s: 54.2 s of idle link after discovery before the iPhone's first C1 write, then BTP and PASE in 9.4 s. The idle wait is on the iPhone side; its cause is unknown |

Later the same day the 54.2 s idle was found to be a stale `_matterc` SRP
record on Apple's border router, which Home dialled over Thread first; not a
BLE fault. Fixed in tinytalk (a029e57).

Still open for M0: the unmodified `build/dwm-m0` image and the 10-run
success rate.

After the trace, tinyble put those six behaviours behind macros, off by
default (README.md): 2,735 B flash and 81 B RAM, against 3,115 B and 85 B with
all on (measured 2026-10-16).

### M2 build (2026-10-16)

`samples/m2` (README.md) on `decawave_dwm3001cdk`, no LTO: whole image
83,736 B flash and 16,224 B RAM; host side 4,146 B flash and 1,372 B RAM
(tinyble core 2,563 B and 81 B, port 282 B and 28 B, Matter table 151 B,
hci_raw.c 582 B and 1,263 B, net_buf 568 B), inside the 6,429 B and 2,031 B
budget. Controller as in "Controller trims": peripheral SDC, DLE 251 B, 2M PHY
and LE Ping off, default 3 TX and 2 RX buffers.

tinyble then sent LE Write Suggested Default Data Length (251 B, 2,120 us), as
the Zephyr host does: the SDC sources and headers do not say it starts the
data length update without one. It no longer does: with L2CAP reassembly
(09f9327, "M3 build") the controller stays at 27 B. Not yet run on hardware.

Later the same day `samples/m2` took the five raw-HCI controller lines from
"M3 build": 80,756 B flash, RAM unchanged; with flow control off and the
driver log at warning level, 79,808 B and 15,976 B.

### M3 build (2026-10-16)

`samples/door-lock` (README.md) is tinytalk's door-lock at ec652b4 with
`CONFIG_TINYTALK_BLE=n` and `port/tinytalk/matter_ble_tinyble.c` providing
`matter_ble.h`. It builds; Thread attach on the bench is the open half of the
gate. Compared with tinytalk's own door-lock at the same commit plus
`measure/ctlr-lean.conf`, on `decawave_dwm3001cdk`:

| Build | tinyble flash (B) | tinyble RAM (B) | Zephyr host flash (B) | Zephyr host RAM (B) |
|---|---:|---:|---:|---:|
| LTO | 272,380 | 87,448 | 297,648 | 97,184 |
| no LTO | 301,780 | 87,448 | 330,368 | 97,248 |

- Raw HCI changes controller defaults. tinytalk's prj.conf sets
  `BT_PHY_UPDATE=n` and `BT_HCI_VS=n`, and Zephyr's host keeps privacy, the
  accept list and connection RSSI off; under `BT_HCI_RAW` all five came back
  on. Off again: 3,000 B (no LTO). `BT_CTLR_PHY` has no prompt; it follows
  `BT_PHY_UPDATE`. With 2M and Coded off the SDC has no PHY update procedure
  either way (`nrf/subsys/bluetooth/controller/hci_driver.c`). In tinytalk's
  build `BT_PHY_UPDATE=n` also hides `BT_CTLR_PHY_2M`, so ctlr-lean.conf only
  took LE Ping off there.
- Raw HCI also defaults `BT_HCI_ACL_FLOW_CONTROL=y` (controller-to-host
  flow control, `zephyr/subsys/bluetooth/Kconfig`), which tinyble never
  enables, and logs the SDC driver at info level. Off, and at warning level
  as in tinytalk's build: -512 B flash and -184 B RAM, then -72 B flash (LTO).
- Data length 27 B, as in tinytalk's build, since tinyble reassembles
  inbound L2CAP PDUs and splits outbound ones (one 251 B buffer each way;
  core +89 B flash, +506 B RAM). The samples drop
  `BT_BUF_ACL_RX_SIZE`/`TX_SIZE=251` and `BT_CTLR_DATA_LENGTH_MAX=251` for
  `BT_DATA_LEN_UPDATE=n`. Against DLE 251 B: door-lock -1,520 B flash and
  -1,360 B RAM (LTO), m2 -1,548 B and -1,360 B. The first trace already ran
  at 27 B: the iPhone sent 185 B PDUs as 27 B fragments ("First commissioning
  trace").
- The table includes all three trims. SDC library and driver now 37,629 B
  and 4,489 B RAM against Zephyr host's 37,665 B and 4,678 B (were 39,912 B
  and 5,794 B at DLE 251 B with flow control on).
- Host side, no LTO: 5,625 B flash and 1,226 B RAM (core, table and port
  3,289 B and 731 B, hci_raw.c 889 B and 495 B, net_buf 619 B, 828 B more of
  kernel/poll.c for `k_work_poll`). Zephyr's host: 31,512 B and 10,427 B.
- Adapter 1,437 B and 1,370 B (1,024 B of it the BTP reassembly area), against
  `matter_ble_zephyr.c` at 2,890 B and 1,575 B.
- Threads: tinyble runs on the system work queue (`tb_zephyr_start()`), where
  tinytalk already runs PASE and its timers; no new stack. tinytalk can call
  `matter_ble_send()` from other threads, so the adapter takes the port lock,
  and it calls tinytalk's three callbacks from a work item without that lock:
  they take tinytalk's lock, which a sender waiting on ours may hold.
- `make adapter TINYTALK=<tree>` runs the adapter under sanitizers against a
  scripted phone that uses tinytalk's BTP codec: both orders of handshake
  write and subscribe, a 400 B message in and a 300 B reply out with acks at
  half the window, a drop mid-send, a sequence error, all through 27 B ACL
  packets. 250 checks pass; starting our sequence at 0 instead of 1 failed 7
  of the first 197.

### M3 bench (2026-10-16)

`build/m3-tb` (door-lock on tinyble, 27 B data length, reassembly) on the
DWM3001CDK, one Apple Home commissioning, console drained over RTT
(`trace/rtt_drain.py`, up-buffer 0). Passed the M3 gate:

- BTP up at fragment 182 B, ATT MTU 185 B; PASE completed, so multi-packet
  C1 writes reassembled.
- Thread attached 1,281 ms after AddNOC on the commissioner's dataset; SRP
  registered; CASE over Thread; the iPhone closed BLE (0x13).
- Apple Home added a second fabric over Thread; lock and unlock from Home
  drove the LED and reported to both subscriptions.
- `tb_rx_dropped` read 1 afterwards (0x2000db66 in this build). Which packet
  is unknown: the console does not log drops. The default build also counts
  SMP and unknown-CID PDUs here. A run with `trace/monitor.conf` (raw HCI
  feeds the monitor, `hci_raw.c:167`) would name it.
