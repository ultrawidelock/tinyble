"""Drain RTT up-buffer <idx> through nrfutil memory reads; writes RdOff back.

JLinkRTTLogger did not find the control block on the DWM3001CDK's J-Link OB.
The probe serves one client at a time: run nothing else on it meanwhile.

usage: rtt_drain.py <serial> <cb_addr_hex> <idx> <out_file>
"""
import subprocess
import sys
import time

SN, CB, IDX, OUT = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]), sys.argv[4]
DESC = CB + 0x18 + 24 * IDX  # name, pBuffer, size, WrOff, RdOff, flags


def read(addr, n):
    a = addr & ~3
    k = (addr - a + n + 3) & ~3
    r = subprocess.run(['nrfutil', 'device', 'read', '--serial-number', SN, '--address',
                        hex(a), '--bytes', str(k), '--direct'], capture_output=True, text=True)
    out = b''
    for line in r.stdout.splitlines():
        if line.startswith('0x'):
            for w in line.split('|')[0].split(':')[1].split():
                out += int(w, 16).to_bytes(4, 'little')
    out = out[addr - a:][:n]
    return out if len(out) == n else None


def write_word(addr, value):
    subprocess.run(['nrfutil', 'device', 'write', '--serial-number', SN, '--address',
                    hex(addr), '--value', hex(value), '--direct'], capture_output=True)


d = None
while d is None:
    d = read(DESC, 24)
buf, size = int.from_bytes(d[4:8], 'little'), int.from_bytes(d[8:12], 'little')
print(f'buf {buf:#x} size {size}', flush=True)
total = 0
with open(OUT, 'ab', buffering=0) as f:
    while True:
        w = read(DESC + 12, 8)
        if w is None:
            time.sleep(0.2)
            continue
        wr, rd = int.from_bytes(w[0:4], 'little'), int.from_bytes(w[4:8], 'little')
        if wr != rd and wr < size and rd < size:
            if wr > rd:
                data = read(buf + rd, wr - rd)
            else:
                a, b = read(buf + rd, size - rd), read(buf, wr) if wr else b''
                data = a + b if a is not None and b is not None else None
            if data is None:
                continue
            f.write(data)
            write_word(DESC + 16, wr)
            total += len(data)
            print(f'{time.strftime("%H:%M:%S")} +{len(data)} = {total}', flush=True)
        else:
            time.sleep(0.2)
