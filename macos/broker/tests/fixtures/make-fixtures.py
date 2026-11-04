#!/usr/bin/env -S python3 -I
# Writes the host-test fixtures (DESIGN 9.0.2). d455.plist is a hand-built D455 layout (VC+VS depth, VC+VS RGB, HID)
# with UVC class descriptors; replace it with `realsense-usbd --dump` output from a connected D455 (HW-2).
import plistlib
import struct
import sys
from pathlib import Path

OUT = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parent)


def dd(vid, pid, bcd_usb=0x0320, bcd_dev=0x5010, cls=0xEF, sub=2, proto=1):
    return struct.pack('<BBHBBBBHHHBBBB', 18, 1, bcd_usb, cls, sub, proto, 9, vid, pid, bcd_dev, 1, 2, 3, 1)


def intf(num, alt, neps, cls, sub, proto=0, iif=0):
    return bytes([9, 4, num, alt, neps, cls, sub, proto, iif])


def ep(addr, attr, mps, interval=0):
    return struct.pack('<BBBBHB', 7, 5, addr, attr, mps, interval)[:7]


def vc_extra(unit_ids_xu, it_id=1, ot_id=4, vs_if=1):
    body = b''
    # input terminal (camera), processing unit 2, extension units, output terminal
    body += bytes([18, 0x24, 2, it_id, 0x01, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0x0A, 0x00])
    body += bytes([13, 0x24, 5, 2, it_id, 0, 0, 3, 0x7F, 0x17, 0, 0, 0])
    src = 2
    for u in unit_ids_xu:
        guid = bytes(range(16 * u, 16 * u + 16))
        body += bytes([29, 0x24, 6, u]) + guid + bytes([32, 1, src, 4, 0xFF, 0xFF, 0xFF, 0xFF, 0])
        src = u
    body += bytes([9, 0x24, 3, ot_id, 0x01, 0x01, 0, src, 0])
    header = struct.pack('<BBBHHIB', 13, 0x24, 1, 0x0150, 13 + len(body), 48000000, 1) + bytes([vs_if])
    return header + body


def vs_extra(ep_addr, terminal, width, height, fourcc=b'YUY2'):
    guid = fourcc + bytes([0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71])
    fmt = bytes([27, 0x24, 4, 1, 1]) + guid + bytes([16, 1, 0, 0, 0, 0])
    frame = struct.pack('<BBBBBHHIIIIBI', 30, 0x24, 5, 1, 0, width, height, width * height * 16 * 30,
                        width * height * 16 * 30, width * height * 2, 333333, 1, 333333)
    total = 14 + len(fmt) + len(frame)
    hdr = struct.pack('<BBBBHBBBBBBBB', 14, 0x24, 1, 1, total, ep_addr, 0, terminal, 0, 0, 0, 1, 0)
    return hdr + fmt + frame


def alt(d, x=b''):
    return {'d': d, 'x': x}


def interface(cfg, alts, eps):
    return {'cfg': cfg, 'alts': alts, 'eps': b''.join(eps)}


def d455(path, registry):
    hid_report = bytes([9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0x40, 0])
    ifs = [
        interface(0, [alt(intf(0, 0, 1, 0x0E, 1, 0, 4), vc_extra([3, 6]))], [ep(0x87, 3, 64, 6)]),
        interface(0, [alt(intf(1, 0, 1, 0x0E, 2, 0, 5), vs_extra(0x82, 4, 848, 480, b'Z16 '))], [ep(0x82, 2, 1024)]),
        interface(0, [alt(intf(2, 0, 1, 0x0E, 2, 0, 6), vs_extra(0x83, 4, 848, 480, b'Y8  '))], [ep(0x83, 2, 1024)]),
        interface(0, [alt(intf(3, 0, 1, 0x0E, 1, 0, 7), vc_extra([4], it_id=9, ot_id=12, vs_if=4))], [ep(0x88, 3, 64, 6)]),
        interface(0, [alt(intf(4, 0, 1, 0x0E, 2, 0, 8), vs_extra(0x84, 12, 1280, 720))], [ep(0x84, 2, 1024)]),
        interface(0, [alt(intf(5, 0, 1, 0x03, 0, 0, 9), hid_report)], [ep(0x85, 3, 64, 1)]),
    ]
    return {
        'path': path, 'dd': dd(0x8086, 0x0B5C), 'ifs': ifs, 'RegistryID': registry, 'DriversAttached': True,
        'ActiveConfig': 0,
        'Mock': {
            'ControlIn': [{'rt': 0xA1, 'rq': 0x87, 'val': 0x0100, 'idx': 0x0001, 'data': bytes(range(48))}],
            'StreamPeriodMs': {'0x82': 33, '0x83': 33, '0x84': 33, '0x85': 5, '0x87': 50, '0x88': 50},
        },
    }


def write(name, devices):
    with open(OUT / name, 'wb') as f:
        plistlib.dump({'Devices': devices}, f, fmt=plistlib.FMT_XML, sort_keys=True)


write('d455.plist', [d455('1-1-1', 0x100000B00)])
write('two-d455.plist', [d455('1-1-1', 0x100000B00), d455('2-1-1', 0x100000A00)])

webcam = {'path': '1-2-3', 'dd': dd(0x046D, 0x085E, cls=0xEF), 'RegistryID': 0x100000C00, 'DriversAttached': True,
          'ActiveConfig': 0,
          'ifs': [interface(0, [alt(intf(0, 0, 1, 0x0E, 1), vc_extra([3]))], [ep(0x81, 3, 16, 8)]),
                  interface(0, [alt(intf(1, 0, 1, 0x0E, 2), vs_extra(0x82, 4, 640, 480))], [ep(0x82, 2, 512)])]}
hub = {'path': '1-3-4', 'dd': dd(0x8086, 0x8000, bcd_usb=0x0300, cls=9, sub=0, proto=3), 'RegistryID': 0x100000D00,
       'DriversAttached': True, 'ActiveConfig': 0,
       'ifs': [interface(0, [alt(intf(0, 0, 1, 0x09, 0))], [ep(0x81, 3, 2, 12)])]}
keyboard = d455('1-4-5', 0x100000E00)
keyboard['dd'] = dd(0x8086, 0x0B3A)
del keyboard['Mock']
keyboard['ifs'].append(interface(0, [alt(intf(6, 0, 1, 0x03, 1, 1), bytes([9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0x3F, 0]))],
                                 [ep(0x86, 3, 8, 10)]))
write('filtered.plist', [webcam, hub, keyboard])

dfu_functional = bytes([9, 0x21, 0x0F, 0xFF, 0x00, 0x00, 0x04, 0x10, 0x01])
recovery = {'path': '1-1-2', 'dd': dd(0x8086, 0x0ADB, bcd_usb=0x0210, cls=0, sub=0, proto=0), 'RegistryID': 0x100000F00,
            'DriversAttached': False, 'ActiveConfig': 0,
            'ifs': [interface(0, [alt(intf(0, 0, 0, 0xFE, 0x01, 0x02), dfu_functional)], []),
                    interface(0, [alt(intf(1, 0, 2, 0xFF, 0x00))], [ep(0x81, 2, 512), ep(0x01, 2, 512)])]}
write('recovery.plist', [recovery])
