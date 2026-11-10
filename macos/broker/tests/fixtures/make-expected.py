#!/usr/bin/env -S python3 -I
# Writes the H3 golden files <name>.expected.txt (DESIGN 9.0.6) from the fixtures, independently of both the daemon
# and the librealsense client: the rs-macos-usbdump text that in-process libusb would print for these devices
# (enumerator-libusb.cpp get_subdevices, device-libusb.cpp, interface-libusb.cpp, endpoint-libusb.h).
# Devices the daemon must hide (DESIGN 2.4.1) are dropped first. HW-2 replaces d455.expected.txt with real output.
#
#   make-expected.py <librealsense source dir>      (the Intel PID list is read from its D400/D500 headers)
import plistlib
import struct
import sys
from pathlib import Path

HERE = Path(__file__).parent
REFUSED = {0x01, 0x02, 0x08, 0x09, 0x0A, 0xE0}


def intel_pids(librealsense):
    pids = set()
    src = Path(librealsense) / 'src/ds'
    for f in (src / 'd400/d400-private.h', src / 'd500/d500-private.h'):
        for line in f.read_text().splitlines():
            line = line.strip()
            if line.startswith('const uint16_t') and '_PID = 0x' in line:
                pids.add(int(line.split('= 0x')[1].split(';')[0], 16))
    return pids


def allowed(dev, pids):
    vid, pid = struct.unpack_from('<HH', dev['dd'], 8)
    if vid == 0x8086 and pid not in pids:
        return False
    if vid not in (0x8086, 0x38E5):
        return False
    for i in dev['ifs']:
        for a in i['alts']:
            cls, sub, proto = a['d'][5], a['d'][6], a['d'][7]
            if cls in REFUSED or (cls == 0x03 and sub == 1 and proto in (1, 2)):
                return False
    return True


def descriptor(raw, length):
    data = bytes(raw[:length]) + bytes(max(0, length - len(raw)))
    return '  desc type=%02x len=%u %s' % (raw[1], length, data.hex())


def dump(devices):
    out = []
    for d in devices:
        bcd_usb, = struct.unpack_from('<H', d['dd'], 2)
        vid, pid = struct.unpack_from('<HH', d['dd'], 8)
        cfg_pos = {}
        for i in d['ifs']:
            mi = cfg_pos.get(i['cfg'], 0)
            cfg_pos[i['cfg']] = mi + 1
            a0 = i['alts'][0]['d']
            if a0[6] == 2 or a0[5] == 0xFE:
                continue
            out.append('info id=%s vid=%04x pid=%04x mi=%u cls=%u uid=%s spec=%04x'
                       % (d['path'], vid, pid, mi, a0[5], d['path'], bcd_usb))
    seen = set()
    for d in devices:
        if not any(i['alts'][0]['d'][6] != 2 and i['alts'][0]['d'][5] != 0xFE for i in d['ifs']) or d['path'] in seen:
            continue
        seen.add(d['path'])
        out.append('device %s' % d['path'])
        assoc = {}
        current, current_cfg = None, None
        for n, i in enumerate(d['ifs']):
            if i['cfg'] != current_cfg:
                current, current_cfg = None, i['cfg']
            a0 = i['alts'][0]['d']
            assoc[n] = []
            if a0[5] == 0x0E:
                if a0[6] == 1:
                    current = n
                if a0[6] == 2 and current is not None:
                    assoc[current].append(a0[2])
        for n, i in enumerate(d['ifs']):
            a0 = i['alts'][0]['d']
            out.append('  intf %u cls=%u sub=%u assoc=[%s]' % (a0[2], a0[5], a0[6], ','.join(map(str, assoc[n]))))
            eps = i['eps']
            for k in range(0, len(eps), 7):
                addr, attr = eps[k + 2], eps[k + 3]
                out.append('    ep %02x type=%u dir=%02x intf=%u' % (addr, attr, 0x80 if addr >= 0x80 else 0, a0[2]))
        out.append(descriptor(d['dd'], d['dd'][0]))
        for i in d['ifs']:
            for a in i['alts']:
                out.append(descriptor(a['d'], a['d'][0]))
                x, k = a['x'], 0
                while k + 2 <= len(x) and x[k] >= 2 and k + x[k] <= len(x):
                    out.append(descriptor(x[k:k + x[k]], x[k]))
                    k += x[k]
    return '\n'.join(out) + ('\n' if out else '')


def main():
    if len(sys.argv) != 2:
        print('usage: make-expected.py <librealsense source dir>', file=sys.stderr)
        return 2
    pids = intel_pids(sys.argv[1])
    for f in sorted(HERE.glob('*.plist')):
        devices = [d for d in plistlib.loads(f.read_bytes())['Devices'] if allowed(d, pids)]
        devices.sort(key=lambda d: d['RegistryID'])
        (HERE / (f.stem + '.expected.txt')).write_text(dump(devices))
        print('%s: %d devices' % (f.stem, len(devices)))


if __name__ == '__main__':
    sys.exit(main())
