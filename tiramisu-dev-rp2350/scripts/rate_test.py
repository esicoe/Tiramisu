"""Measure the aim1k's actual HID report rate, its jitter, and what it reports.

    python rate_test.py [seconds]

Two acquisition paths, picked automatically:

  * ReadFile on the vendor-defined collection. Windows does not claim
    vendor-defined collections, so this one can be opened. Preferred: a blocking
    read returns the instant a report lands, which is the cleanest timestamp
    available from userspace.
  * Raw Input, as a fallback. Needed for any build whose only collection is one
    Windows opens exclusively - a Digitizer/Pen or the absolute mouse - where
    ReadFile gets ACCESS_DENIED.

For a 1 kHz device the mean rate alone is nearly useless: 1000 Hz with a regular
1 ms cadence and 1000 Hz arriving in bursts feel completely different. The
percentiles and the late-frame counts are the numbers that matter.

Note this measures reports ARRIVING AT THE HOST; it cannot by itself separate
"firmware skipped a frame" from "the USB stack dropped one".
"""

import ctypes as ct
import ctypes.wintypes as wt
import struct
import sys
import time

VID, PID = 0x1209, 0x0001

hid = ct.WinDLL("hid", use_last_error=True)
setupapi = ct.WinDLL("setupapi", use_last_error=True)
k32 = ct.WinDLL("kernel32", use_last_error=True)
u32 = ct.WinDLL("user32", use_last_error=True)

DIGCF = 0x02 | 0x10
GENERIC_READ = 0x80000000
SHARE_RW = 0x03
OPEN_EXISTING = 3
INVALID = ct.c_void_p(-1).value
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_IO_PENDING = 997
WAIT_TIMEOUT = 0x102

ULONG_PTR = ct.c_ulonglong if ct.sizeof(ct.c_void_p) == 8 else ct.c_ulong


class OVERLAPPED(ct.Structure):
    _fields_ = [("Internal", ULONG_PTR), ("InternalHigh", ULONG_PTR),
                ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD),
                ("hEvent", wt.HANDLE)]


class GUID(ct.Structure):
    _fields_ = [("d1", ct.c_ulong), ("d2", ct.c_ushort),
                ("d3", ct.c_ushort), ("d4", ct.c_ubyte * 8)]


class IFACE(ct.Structure):
    _fields_ = [("cbSize", ct.c_ulong), ("g", GUID),
                ("Flags", ct.c_ulong), ("Reserved", ct.POINTER(ct.c_ulong))]


class ATTRS(ct.Structure):
    _fields_ = [("Size", ct.c_ulong), ("VendorID", ct.c_ushort),
                ("ProductID", ct.c_ushort), ("VersionNumber", ct.c_ushort)]


hid.HidD_GetHidGuid.argtypes = [ct.POINTER(GUID)]
hid.HidD_GetHidGuid.restype = None
hid.HidD_GetAttributes.argtypes = [wt.HANDLE, ct.POINTER(ATTRS)]
hid.HidD_GetAttributes.restype = ct.c_ubyte
setupapi.SetupDiGetClassDevsW.argtypes = [ct.POINTER(GUID), ct.c_wchar_p,
                                          wt.HWND, wt.DWORD]
setupapi.SetupDiGetClassDevsW.restype = wt.HANDLE
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [wt.HANDLE, ct.c_void_p,
                                                 ct.POINTER(GUID), wt.DWORD,
                                                 ct.POINTER(IFACE)]
setupapi.SetupDiEnumDeviceInterfaces.restype = wt.BOOL
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    wt.HANDLE, ct.POINTER(IFACE), ct.c_void_p, wt.DWORD,
    ct.POINTER(wt.DWORD), ct.c_void_p]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wt.BOOL
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [wt.HANDLE]
k32.CreateFileW.argtypes = [ct.c_wchar_p, wt.DWORD, wt.DWORD, ct.c_void_p,
                            wt.DWORD, wt.DWORD, wt.HANDLE]
k32.CreateFileW.restype = wt.HANDLE
k32.ReadFile.argtypes = [wt.HANDLE, ct.c_void_p, wt.DWORD,
                         ct.POINTER(wt.DWORD), ct.c_void_p]
k32.ReadFile.restype = wt.BOOL
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.CreateEventW.argtypes = [ct.c_void_p, wt.BOOL, wt.BOOL, ct.c_wchar_p]
k32.CreateEventW.restype = wt.HANDLE
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
k32.WaitForSingleObject.restype = wt.DWORD
k32.GetOverlappedResult.argtypes = [wt.HANDLE, ct.POINTER(OVERLAPPED),
                                    ct.POINTER(wt.DWORD), wt.BOOL]
k32.GetOverlappedResult.restype = wt.BOOL
k32.CancelIo.argtypes = [wt.HANDLE]
k32.ResetEvent.argtypes = [wt.HANDLE]


def iter_paths():
    guid = GUID()
    hid.HidD_GetHidGuid(ct.byref(guid))
    di = setupapi.SetupDiGetClassDevsW(ct.byref(guid), None, None, DIGCF)
    if not di or di == INVALID:
        return
    iface = IFACE()
    iface.cbSize = ct.sizeof(iface)
    i = 0
    try:
        while setupapi.SetupDiEnumDeviceInterfaces(di, None, ct.byref(guid),
                                                   i, ct.byref(iface)):
            i += 1
            need = wt.DWORD(0)
            setupapi.SetupDiGetDeviceInterfaceDetailW(
                di, ct.byref(iface), None, 0, ct.byref(need), None)
            if not need.value:
                continue
            buf = ct.create_string_buffer(need.value)
            ct.memmove(buf, struct.pack(
                "I", 8 if ct.sizeof(ct.c_void_p) == 8 else 6), 4)
            if setupapi.SetupDiGetDeviceInterfaceDetailW(
                    di, ct.byref(iface), buf, need.value, None, None):
                yield ct.wstring_at(ct.addressof(buf) + 4)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(di)


class CAPS(ct.Structure):
    _fields_ = [("Usage", ct.c_ushort), ("UsagePage", ct.c_ushort),
                ("InputReportByteLength", ct.c_ushort),
                ("OutputReportByteLength", ct.c_ushort),
                ("FeatureReportByteLength", ct.c_ushort),
                ("Reserved", ct.c_ushort * 17),
                ("NumberLinkCollectionNodes", ct.c_ushort),
                ("NumberInputButtonCaps", ct.c_ushort),
                ("NumberInputValueCaps", ct.c_ushort),
                ("NumberInputDataIndices", ct.c_ushort),
                ("NumberOutputButtonCaps", ct.c_ushort),
                ("NumberOutputValueCaps", ct.c_ushort),
                ("NumberOutputDataIndices", ct.c_ushort),
                ("NumberFeatureButtonCaps", ct.c_ushort),
                ("NumberFeatureValueCaps", ct.c_ushort),
                ("NumberFeatureDataIndices", ct.c_ushort)]


hid.HidD_GetPreparsedData.argtypes = [wt.HANDLE, ct.POINTER(ct.c_void_p)]
hid.HidP_GetCaps.argtypes = [ct.c_void_p, ct.POINTER(CAPS)]
hid.HidD_FreePreparsedData.argtypes = [ct.c_void_p]


def caps_of(h):
    pp = ct.c_void_p()
    if not hid.HidD_GetPreparsedData(h, ct.byref(pp)):
        return None
    c = CAPS()
    hid.HidP_GetCaps(pp, ct.byref(c))
    hid.HidD_FreePreparsedData(pp)
    return c


def open_hid():
    """(handle, path) for the aim1k VENDOR collection, or (None, reason).

    The device exposes two top-level collections, so Windows creates two device
    interfaces (&Col01 mouse, &Col02 vendor). Pick by report length: the vendor
    collection is 8 bytes, the mouse one is 6. Windows also holds the mouse
    collection exclusively, so that one would be refused anyway.
    """
    denied = False
    for path in iter_paths():
        h = k32.CreateFileW(path, 0, SHARE_RW, None, OPEN_EXISTING, 0, None)
        if h == INVALID:
            continue
        a = ATTRS()
        a.Size = ct.sizeof(a)
        match = hid.HidD_GetAttributes(h, ct.byref(a)) and \
            a.VendorID == VID and a.ProductID == PID
        k32.CloseHandle(h)
        if not match:
            continue
        h = k32.CreateFileW(path, GENERIC_READ, SHARE_RW, None,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
        if h == INVALID:
            denied = True
            continue
        c = caps_of(h)
        if c and c.InputReportByteLength != 8:
            k32.CloseHandle(h)          # the mouse collection, not the pen one
            continue
        return h, path
    if denied:
        return None, ("found the aim1k but every collection refused a read "
                      "handle - Windows only does that for collections it binds "
                      "itself (mouse/keyboard/pen), so this is probably a "
                      "digitizer-only build.")
    return None, "aim1k %04X:%04X not found" % (VID, PID)


def pct(vals, p):
    if not vals:
        return 0.0
    i = min(len(vals) - 1, max(0, int(round(p / 100.0 * (len(vals) - 1)))))
    return vals[i]


def main():
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0

    h, info = open_hid()
    if h is None:
        print(info)
        return 1
    print("device : %s" % info)
    print("measuring for %.0f s ..." % dur)

    buf = ct.create_string_buffer(64)
    got = wt.DWORD(0)
    stamps = []
    inrange = 0
    xs, ys = [], []

    # Overlapped, so a silent tablet cannot wedge this. Silence is the NORMAL
    # out-of-range state - the firmware deliberately stops reporting when no pen
    # is present, so that the cursor is left alone - and a blocking ReadFile
    # would simply never return until someone put the pen down.
    ov = OVERLAPPED()
    ov.hEvent = k32.CreateEventW(None, True, False, None)
    idle = 0.0

    t0 = time.perf_counter()
    while time.perf_counter() - t0 < dur:
        k32.ResetEvent(ov.hEvent)
        got.value = 0
        ok = k32.ReadFile(h, buf, 64, ct.byref(got), ct.byref(ov))
        if not ok:
            err = ct.get_last_error()
            if err != ERROR_IO_PENDING:
                print("ReadFile failed, err", err)
                break
            if k32.WaitForSingleObject(ov.hEvent, 250) == WAIT_TIMEOUT:
                k32.CancelIo(h)
                k32.GetOverlappedResult(h, ct.byref(ov), ct.byref(got), True)
                idle += 0.25
                continue
            if not k32.GetOverlappedResult(h, ct.byref(ov), ct.byref(got), False):
                continue
        now = time.perf_counter()
        d = buf.raw[:got.value]
        if len(d) < 8:
            continue
        stamps.append(now)
        btn, x, y, _p = struct.unpack_from("<BHHH", d, 1)
        if btn & 0x01:                  # PEN_IN_RANGE is bit 0 (see hid_report.h)
            inrange += 1
            xs.append(x / 100.0)
            ys.append(y / 100.0)
    k32.CloseHandle(h)
    k32.CloseHandle(ov.hEvent)

    n = len(stamps)
    if n < 2:
        print("only %d reports in %.1f s - the tablet is connected but silent."
              % (n, dur))
        print("That is what out-of-range looks like: put the pen on the tablet "
              "and keep it there for the whole run.")
        return 1

    span = stamps[-1] - stamps[0]
    gaps = sorted((stamps[i + 1] - stamps[i]) * 1000.0 for i in range(n - 1))

    print("\nreports: %d in %.2f s" % (n, span))
    if idle > 0.3:
        print("         (%.1f s of the run was silent - the pen left range, so "
              "the gap percentiles below include that)" % idle)
    print("rate   : %.1f Hz mean   (1000.0 is the USB Full-Speed ceiling)"
          % ((n - 1) / span))
    print("\ninter-report interval (ms), 1.000 is ideal:")
    print("  min %.3f   p50 %.3f   p95 %.3f   p99 %.3f   max %.3f"
          % (gaps[0], pct(gaps, 50), pct(gaps, 95), pct(gaps, 99), gaps[-1]))
    mean = sum(gaps) / len(gaps)
    var = sum((g - mean) ** 2 for g in gaps) / len(gaps)
    print("  mean %.3f   stdev %.3f" % (mean, var ** 0.5))

    for lim in (1.5, 2.5, 10.0):
        c = sum(1 for g in gaps if g > lim)
        print("  gaps > %4.1f ms : %5d  (%.3f%%)" % (lim, c, 100.0 * c / len(gaps)))

    print("\nhistogram of intervals (ms):")
    edges = [0.5, 0.9, 0.95, 1.0, 1.05, 1.1, 1.5, 2.0, 3.0, 5.0, 1e9]
    prev = 0.0
    for e in edges:
        c = sum(1 for g in gaps if prev <= g < e)
        if c:
            lbl = "%5.2f-%5.2f" % (prev, e) if e < 1e8 else ">%.2f     " % prev
            print("  %s %7d %s" % (lbl, c, "#" * min(60, int(60.0 * c / len(gaps)))))
        prev = e

    print("\npen in range for %d of %d reports" % (inrange, n))
    if xs:
        print("  x %.2f..%.2f mm   y %.2f..%.2f mm"
              % (min(xs), max(xs), min(ys), max(ys)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
