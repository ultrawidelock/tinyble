# tinyble

A BLE peripheral host for Matter commissioning. It replaces Zephyr's
Bluetooth host and drives the controller, Zephyr's or Nordic's, unchanged
through raw HCI. Portable C11.

6.7 KB flash, 2.5 KB RAM, LTO release build.

## Why it is small

- One 251 B buffer each way. Nothing is queued.
- One const GATT table, 4 B per attribute.
- Link-time hooks. No threads, no locks.
- Eight HCI commands.

## Use

```conf
CONFIG_BT_HCI_RAW=y
CONFIG_TINYBLE=y
CONFIG_TINYBLE_TINYTALK=y    # tinytalk's matter_ble.h on tinyble
CONFIG_TINYTALK_BLE=n
CONFIG_TINYBLE_TINYFOTA=y    # optional: tinyfota's DFU services
```

Optional ATT behaviour: the `TB_*` macros in `include/tinyble/tinyble.h`.

## Test

```sh
make test                    # host, against a fake controller
make asan
make size ARM_CC=<arm-gcc>   # gate: 5.2 KB flash, 0.7 KB RAM
```

Apache-2.0
