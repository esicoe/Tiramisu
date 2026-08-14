"""Switch the aim1k's absolute-mouse collection on or off from the host.

    python set_mode.py mouse-on     driverless cursor (the plug-in default)
    python set_mode.py mouse-off    silence it so OpenTabletDriver alone drives
    python set_mode.py status

This is the same HID feature report OTD sends via FeatureInitReport [[3, 0]] in
aim1k.json, so it doubles as a way to verify that path independently of OTD.
Replugging the tablet always restores mouse mode.
"""

import ctypes as ct
import ctypes.wintypes as wt
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from rate_test import (hid, k32, iter_paths, caps_of, ATTRS, VID, PID,  # noqa: E402
                       GENERIC_READ, SHARE_RW, OPEN_EXISTING, INVALID)

GENERIC_WRITE = 0x40000000
REPORT_ID_MODE = 3

hid.HidD_SetFeature.argtypes = [wt.HANDLE, ct.c_void_p, ct.c_ulong]
hid.HidD_SetFeature.restype = ct.c_ubyte
hid.HidD_GetFeature.argtypes = [wt.HANDLE, ct.c_void_p, ct.c_ulong]
hid.HidD_GetFeature.restype = ct.c_ubyte


def open_vendor_rw():
    for path in iter_paths():
        h = k32.CreateFileW(path, 0, SHARE_RW, None, OPEN_EXISTING, 0, None)
        if h == INVALID:
            continue
        a = ATTRS()
        a.Size = ct.sizeof(a)
        ok = hid.HidD_GetAttributes(h, ct.byref(a)) and \
            a.VendorID == VID and a.ProductID == PID
        k32.CloseHandle(h)
        if not ok:
            continue
        h = k32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, SHARE_RW,
                            None, OPEN_EXISTING, 0, None)
        if h == INVALID:
            continue
        c = caps_of(h)
        if c and c.InputReportByteLength == 8:
            return h, path, (c.FeatureReportByteLength or 2)
        k32.CloseHandle(h)
    return None, None, 0


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    h, path, flen = open_vendor_rw()
    if h is None:
        print("aim1k vendor collection not found (is it flashed and plugged in?)")
        return 1
    print("device: %s\nfeature report length: %d" % (path, flen))

    buf = ct.create_string_buffer(max(flen, 2))
    try:
        if cmd == "status":
            buf[0] = bytes([REPORT_ID_MODE])
            if not hid.HidD_GetFeature(h, buf, len(buf)):
                print("HidD_GetFeature failed, err %d" % ct.get_last_error())
                return 1
            print("mouse collection is %s" % ("ON" if buf.raw[1] else "OFF"))
            return 0

        if cmd not in ("mouse-on", "mouse-off"):
            print(__doc__)
            return 2

        val = 1 if cmd == "mouse-on" else 0
        buf[0] = bytes([REPORT_ID_MODE])
        buf[1] = bytes([val])
        if not hid.HidD_SetFeature(h, buf, len(buf)):
            print("HidD_SetFeature failed, err %d" % ct.get_last_error())
            return 1
        print("sent mode=%d -> mouse collection %s"
              % (val, "ON" if val else "OFF"))
        return 0
    finally:
        k32.CloseHandle(h)


if __name__ == "__main__":
    sys.exit(main())
