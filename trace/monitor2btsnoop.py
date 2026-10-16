#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Convert a Zephyr Bluetooth monitor stream (RTT up-buffer "btmonitor") to
btsnoop (H4, datalink 1002) for Wireshark, and print one line per packet.

Usage: monitor2btsnoop.py <in.bin> [out.btsnoop]

Stream format: zephyr/subsys/bluetooth/host/monitor.{c,h}. Each packet is
data_len(2) opcode(2) flags(1) hdr_len(1) ext[hdr_len] payload, little endian,
data_len counting everything after itself. Timestamps are TS32 ticks of
1/MONITOR_TS_FREQ s, computed as cycles / (hw_hz // freq): on the nRF RTC
(32,768 Hz) one tick is 3/32768 s, not 100 us.
"""
import struct
import sys

TICK_S = 3 / 32768

CMD, EVT, ACL_TX, ACL_RX, NOTE, ULOG = 2, 3, 4, 5, 12, 13
DROP_NAMES = {1: "cmd", 2: "evt", 3: "acl_rx", 4: "acl_tx", 5: "sco_rx",
              6: "sco_tx", 7: "other"}

HCI_CMDS = {
    0x0406: "Disconnect", 0x0C01: "Set Event Mask", 0x0C03: "Reset",
    0x0C2D: "Read Tx Power", 0x0C63: "Set Event Mask Page 2",
    0x1001: "Read Local Version", 0x1002: "Read Local Commands",
    0x1003: "Read Local Features", 0x1009: "Read BD_ADDR",
    0x2001: "LE Set Event Mask", 0x2002: "LE Read Buffer Size",
    0x2003: "LE Read Local Features", 0x2005: "LE Set Random Address",
    0x2006: "LE Set Adv Params", 0x2007: "LE Read Adv Tx Power",
    0x2008: "LE Set Adv Data", 0x2009: "LE Set Scan Rsp Data",
    0x200A: "LE Set Adv Enable", 0x200F: "LE Read Filter Accept List Size",
    0x2010: "LE Clear Filter Accept List", 0x2016: "LE Read Remote Features",
    0x2018: "LE Rand", 0x201C: "LE Read Supported States",
    0x2022: "LE Set Data Length", 0x2023: "LE Read Suggested Default DL",
    0x2024: "LE Write Suggested Default DL", 0x202F: "LE Read Max Data Length",
    0x2030: "LE Read PHY", 0x2031: "LE Set Default PHY", 0x2032: "LE Set PHY",
    0x2060: "LE Read Buffer Size v2",
}
HCI_EVTS = {
    0x05: "Disconnection Complete", 0x08: "Encryption Change",
    0x0C: "Read Remote Version Complete", 0x0E: "Command Complete",
    0x0F: "Command Status", 0x10: "Hardware Error",
    0x13: "Number Of Completed Packets", 0x1A: "Data Buffer Overflow",
    0xFF: "Vendor",
}
LE_SUBEVTS = {
    0x01: "Connection Complete", 0x03: "Connection Update Complete",
    0x04: "Read Remote Features Complete", 0x05: "LTK Request",
    0x06: "Remote Conn Param Request", 0x07: "Data Length Change",
    0x0A: "Enhanced Connection Complete", 0x0C: "PHY Update Complete",
    0x14: "Channel Selection Algorithm",
}
ATT_OPS = {
    0x01: "Error Rsp", 0x02: "Exchange MTU Req", 0x03: "Exchange MTU Rsp",
    0x04: "Find Info Req", 0x05: "Find Info Rsp",
    0x06: "Find By Type Value Req", 0x07: "Find By Type Value Rsp",
    0x08: "Read By Type Req", 0x09: "Read By Type Rsp", 0x0A: "Read Req",
    0x0B: "Read Rsp", 0x0C: "Read Blob Req", 0x0D: "Read Blob Rsp",
    0x0E: "Read Multiple Req", 0x0F: "Read Multiple Rsp",
    0x10: "Read By Group Type Req", 0x11: "Read By Group Type Rsp",
    0x12: "Write Req", 0x13: "Write Rsp", 0x16: "Prepare Write Req",
    0x17: "Prepare Write Rsp", 0x18: "Execute Write Req",
    0x19: "Execute Write Rsp", 0x1B: "Notification", 0x1D: "Indication",
    0x1E: "Confirmation", 0x20: "Read Multiple Variable Req",
    0x52: "Write Cmd", 0xD2: "Signed Write Cmd",
}
SIG_CODES = {
    0x01: "Command Reject", 0x06: "Disconnection Req", 0x07: "Disconnection Rsp",
    0x12: "Conn Param Update Req", 0x13: "Conn Param Update Rsp",
    0x14: "LE Credit Based Conn Req", 0x15: "LE Credit Based Conn Rsp",
    0x16: "Flow Control Credit", 0x17: "Credit Based Conn Req",
}
SMP_CODES = {0x01: "Pairing Req", 0x05: "Pairing Failed", 0x0B: "Security Req"}

# btsnoop epoch: microseconds from 0000-01-01 to 1970-01-01.
BTSNOOP_EPOCH_US = 0x00DCDDB30F2F8000


def describe_cmd(p):
    op, plen = struct.unpack_from("<HB", p)
    return f"CMD  {HCI_CMDS.get(op, f'0x{op:04x}')} [{p[3:3 + plen].hex()}]"


def describe_evt(p):
    code, plen = p[0], p[1]
    body = p[2:2 + plen]
    if code == 0x3E and body:
        name = LE_SUBEVTS.get(body[0], f"LE 0x{body[0]:02x}")
        return f"EVT  LE {name} [{body[1:].hex()}]"
    if code == 0x0E and len(body) >= 4:
        op = struct.unpack_from("<H", body, 1)[0]
        return (f"EVT  Command Complete {HCI_CMDS.get(op, f'0x{op:04x}')} "
                f"status 0x{body[3]:02x}")
    if code == 0x0F and len(body) >= 4:
        op = struct.unpack_from("<H", body, 2)[0]
        return (f"EVT  Command Status {HCI_CMDS.get(op, f'0x{op:04x}')} "
                f"status 0x{body[0]:02x}")
    return f"EVT  {HCI_EVTS.get(code, f'0x{code:02x}')} [{body.hex()}]"


def describe_acl(p, direction):
    hf, dlen = struct.unpack_from("<HH", p)
    handle, pb = hf & 0x0FFF, (hf >> 12) & 0x3
    data = p[4:4 + dlen]
    head = f"ACL{direction} h{handle}"
    if pb == 0x1:
        return f"{head} continuation {len(data)} B"
    if len(data) < 4:
        return f"{head} short [{data.hex()}]"
    llen, cid = struct.unpack_from("<HH", data)
    pdu = data[4:]
    frag = "" if len(pdu) >= llen else f" (first {len(pdu)} of {llen} B)"
    if cid == 0x0004 and pdu:
        name = ATT_OPS.get(pdu[0], f"ATT 0x{pdu[0]:02x}")
        return f"{head} ATT {name} [{pdu[1:].hex()}]{frag}"
    if cid == 0x0005 and pdu:
        name = SIG_CODES.get(pdu[0], f"0x{pdu[0]:02x}")
        return f"{head} L2CAP {name} [{pdu[1:].hex()}]{frag}"
    if cid == 0x0006 and pdu:
        name = SMP_CODES.get(pdu[0], f"0x{pdu[0]:02x}")
        return f"{head} SMP {name} [{pdu[1:].hex()}]{frag}"
    return f"{head} CID 0x{cid:04x} [{pdu.hex()}]{frag}"


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    raw = open(sys.argv[1], "rb").read()
    out = open(sys.argv[2], "wb") if len(sys.argv) == 3 else None
    if out:
        out.write(b"btsnoop\0" + struct.pack(">II", 1, 1002))

    off, n_pkts, drops = 0, 0, {}
    while off + 6 <= len(raw):
        data_len, opcode, _flags, hdr_len = struct.unpack_from("<HHBB", raw, off)
        end = off + 2 + data_len
        if data_len < 4 + hdr_len or end > len(raw):
            print(f"# truncated or misaligned at byte {off}", file=sys.stderr)
            break
        ext = raw[off + 6:off + 6 + hdr_len]
        payload = raw[off + 6 + hdr_len:end]
        off = end

        ts, i = 0, 0
        while i < len(ext):
            if ext[i] == 8:
                ts = struct.unpack_from("<I", ext, i + 1)[0]
                i += 5
            else:
                name = DROP_NAMES.get(ext[i], str(ext[i]))
                drops[name] = drops.get(name, 0) + ext[i + 1]
                print(f"# DROPPED {ext[i + 1]} {name} before this packet")
                i += 2
        t = ts * TICK_S

        if opcode == CMD:
            h4, flags, line = 0x01, 2, describe_cmd(payload)
        elif opcode == EVT:
            h4, flags, line = 0x04, 3, describe_evt(payload)
        elif opcode == ACL_TX:
            h4, flags, line = 0x02, 0, describe_acl(payload, ">")
        elif opcode == ACL_RX:
            h4, flags, line = 0x02, 1, describe_acl(payload, "<")
        elif opcode in (NOTE, ULOG):
            text = payload[2 + payload[1]:] if opcode == ULOG else payload
            print(f"{t:10.4f}  NOTE {text.decode(errors='replace').rstrip()}")
            continue
        else:
            print(f"{t:10.4f}  MON  opcode {opcode} [{payload.hex()}]")
            continue

        n_pkts += 1
        print(f"{t:10.4f}  {line}")
        if out:
            rec = bytes([h4]) + payload
            us = BTSNOOP_EPOCH_US + int(t * 1e6)
            out.write(struct.pack(">IIIIQ", len(rec), len(rec), flags, 0, us))
            out.write(rec)

    print(f"# {n_pkts} HCI packets, {len(raw) - off} trailing bytes, "
          f"drops {drops or 'none'}", file=sys.stderr)


if __name__ == "__main__":
    main()
