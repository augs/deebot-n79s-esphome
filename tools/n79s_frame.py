#!/usr/bin/env python3
"""Deebot N79S mainboard UART framing: CRC-8, escaping, frame build/check.

Reference implementation of PROTOCOL.md sections 2-3.  Run it to self-test
against the protocol's test vectors, or import it:

    >>> from n79s_frame import frame, check_frame
    >>> frame(b"<ctl id='1' td='PlaySound' sid='0'/>")
"""

import sys

ESC = {0x5C: 1, 0x60: 2, 0x0A: 3}
UNESC = {v: k for k, v in ESC.items()}


def crc8(data, crc=0):
    """CRC-8/SMBUS: poly 0x07, init 0x00, no reflection, no xorout."""
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def escape(data):
    out = bytearray()
    for b in data:
        out += bytes((0x5C, ESC[b])) if b in ESC else bytes((b,))
    return bytes(out)


def unescape(data):
    """Return the un-escaped bytes, or None on an invalid escape (as the MCU)."""
    out, esc = bytearray(), False
    for b in data:
        if esc:
            if b not in UNESC:
                return None
            out.append(UNESC[b])
            esc = False
        elif b == 0x5C:
            esc = True
        else:
            out.append(b)
    return bytes(out)


def frame(payload, channel=b"A"):
    """Wrap an XML payload (bytes) in a UART frame."""
    body = payload + b"\x00"
    return b"`F" + channel + escape(body + bytes((crc8(body),))) + b"\n"


def check_frame(f):
    """True if f (one frame, from '`F' through '\\n') has a valid check byte."""
    body = unescape(f[3:-1])
    return body is not None and len(body) > 1 and crc8(body[:-1]) == body[-1]


# Real frames from the wire (PROTOCOL.md section 3).
VECTORS = [
    b"`FC<ctl td='GetDeviceCap'/>\x00\x19\n",
    b"`FA<ctl td='GetBatteryInfo'/>\xf8\n",
    b"`FC<ctl td='DCStatus'><DC name='xmpp' on='1' conn='2'/>\xd8\n",
    b"`FB\x00\x00<ctl td='BatteryInfo'><battery power='067'/></ctl>\x00\xbf\n",
    b"`FB\x00\x00<ctl td='error' errno='104'/>\x00\xe1\n",
    b"`FA<ctl id='1' td='Clean'><clean type='auto' speed='standard' act='s'/></ctl>\x00M\n",
    b"`FA\x00\x00<ctl td='Log' t='946687920' evt='8'/>\xa5\x01\x08Q\xe4\x00\xbd\n",
    b"`FA<ctl id='863'/>\x00\\\x03\n",
    b"`FA<ctl id='50888424' td='PlaySound' sid='0'/>\x00\xdf\n",
    b"`FA\x00\x00<ctl id='50888424' ret='ok'/>\x00\xc5\n",
]


def _selftest():
    assert crc8(b"123456789") == 0xF4
    for v in VECTORS:
        assert check_frame(v), v
    assert frame(b"<ctl id='863'/>") == b"`FA<ctl id='863'/>\x00\\\x03\n"
    assert frame(b"<ctl id='50888424' td='PlaySound' sid='0'/>") == VECTORS[8]
    assert unescape(b"a\\\nb") is None
    assert not check_frame(b"`FA<ctl/>\x00\\\n")
    print("ok: %d vectors" % len(VECTORS))


if __name__ == "__main__":
    _selftest()
    sys.exit(0)
