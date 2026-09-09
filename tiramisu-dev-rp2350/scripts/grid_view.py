"""Live view of the aim1k hall grid over the firmware's USB CDC debug console.

No third-party deps - ctypes + winreg from the standard library.

    python grid_view.py            live heatmap (move the magnet, watch a cell light)
    python grid_view.py noise      per-sensor noise sigma, min/mean/max
    python grid_view.py raw        one frame of absolute ADC counts
    python grid_view.py baseline   the learned no-magnet baseline
    python grid_view.py cal        recalibrate (keep the pen away)

VERIFYING THE SENSOR MAP: run the live heatmap and touch the magnet to one
physical sensor at a time. The lit cell must appear at the matching (col,row).
If it lights somewhere else, SENSOR_GRID in firmware/include/sensor_map.h is
wrong for that entry - the heatmap tells you exactly where it landed instead.
"""

import ctypes as ct
import ctypes.wintypes as wt
import sys
import time
import winreg

VID_PID = "VID_1209&PID_0001"
COLS, ROWS = 12, 10
COUNT = COLS * ROWS

k32 = ct.WinDLL("kernel32", use_last_error=True)
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3
INVALID = ct.c_void_p(-1).value


class DCB(ct.Structure):
    _fields_ = [("DCBlength", wt.DWORD), ("BaudRate", wt.DWORD),
                ("flags", wt.DWORD), ("wReserved", wt.WORD),
                ("XonLim", wt.WORD), ("XoffLim", wt.WORD),
                ("ByteSize", ct.c_byte), ("Parity", ct.c_byte),
                ("StopBits", ct.c_byte), ("XonChar", ct.c_char),
                ("XoffChar", ct.c_char), ("ErrorChar", ct.c_char),
                ("EofChar", ct.c_char), ("EvtChar", ct.c_char),
                ("wReserved1", wt.WORD)]


class TIMEOUTS(ct.Structure):
    _fields_ = [("ReadIntervalTimeout", wt.DWORD),
                ("ReadTotalTimeoutMultiplier", wt.DWORD),
                ("ReadTotalTimeoutConstant", wt.DWORD),
                ("WriteTotalTimeoutMultiplier", wt.DWORD),
                ("WriteTotalTimeoutConstant", wt.DWORD)]


k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [ct.c_wchar_p, wt.DWORD, wt.DWORD, ct.c_void_p,
                            wt.DWORD, wt.DWORD, wt.HANDLE]
k32.ReadFile.argtypes = [wt.HANDLE, ct.c_void_p, wt.DWORD,
                         ct.POINTER(wt.DWORD), ct.c_void_p]
k32.WriteFile.argtypes = [wt.HANDLE, ct.c_void_p, wt.DWORD,
                          ct.POINTER(wt.DWORD), ct.c_void_p]
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.GetCommState.argtypes = [wt.HANDLE, ct.POINTER(DCB)]
k32.SetCommState.argtypes = [wt.HANDLE, ct.POINTER(DCB)]
k32.SetCommTimeouts.argtypes = [wt.HANDLE, ct.POINTER(TIMEOUTS)]
k32.EscapeCommFunction.argtypes = [wt.HANDLE, wt.DWORD]


def find_port():
    """Locate the aim1k CDC interface's COM port name from the PnP registry."""
    for mi in ("&MI_00", ""):
        key_path = r"SYSTEM\CurrentControlSet\Enum\USB\%s%s" % (VID_PID, mi)
        try:
            root = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key_path)
        except OSError:
            continue
        i = 0
        while True:
            try:
                inst = winreg.EnumKey(root, i)
            except OSError:
                break
            i += 1
            try:
                p = winreg.OpenKey(root, inst + r"\Device Parameters")
                return winreg.QueryValueEx(p, "PortName")[0]
            except OSError:
                pass
    return None


class Console:
    def __init__(self, port):
        self.h = k32.CreateFileW(r"\\.\%s" % port, GENERIC_READ | GENERIC_WRITE,
                                 0, None, OPEN_EXISTING, 0, None)
        if self.h == INVALID:
            raise OSError("cannot open %s (err %d) - is a terminal already "
                          "holding it open?" % (port, ct.get_last_error()))
        d = DCB()
        d.DCBlength = ct.sizeof(DCB)
        k32.GetCommState(self.h, ct.byref(d))
        d.BaudRate, d.ByteSize, d.Parity, d.StopBits = 115200, 8, 0, 0
        # fBinary | fDtrControl=ENABLE | fRtsControl=ENABLE. DTR is not optional:
        # TinyUSB's tud_cdc_connected() is exactly "host asserted DTR", and the
        # firmware ignores console input until it is set.
        d.flags = 0x0001 | 0x0010 | 0x1000
        k32.SetCommState(self.h, ct.byref(d))
        k32.EscapeCommFunction(self.h, 5)   # SETDTR
        k32.EscapeCommFunction(self.h, 3)   # SETRTS
        t = TIMEOUTS(0, 0, 300, 0, 300)     # 300 ms read timeout
        k32.SetCommTimeouts(self.h, ct.byref(t))
        self.buf = b""

    def write(self, s):
        n = wt.DWORD(0)
        b = s.encode()
        k32.WriteFile(self.h, b, len(b), ct.byref(n), None)

    def readline(self, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                return line.decode(errors="replace").strip()
            chunk = ct.create_string_buffer(4096)
            n = wt.DWORD(0)
            if k32.ReadFile(self.h, chunk, 4096, ct.byref(n), None) and n.value:
                self.buf += chunk.raw[:n.value]
            else:
                time.sleep(0.005)
        return None

    def frame(self, tag, timeout=2.0):
        """Read lines until one starts with `tag`; return its COUNT values."""
        end = time.time() + timeout
        while time.time() < end:
            line = self.readline(timeout=end - time.time())
            if not line or not line.startswith(tag + " "):
                continue
            parts = line[len(tag) + 1:].split()
            if len(parts) != COUNT:
                continue
            try:
                return [float(p) for p in parts]
            except ValueError:
                continue
        return None

    def close(self):
        k32.CloseHandle(self.h)


def enable_vt():
    h = k32.GetStdHandle(-11)
    mode = wt.DWORD(0)
    k32.GetConsoleMode(h, ct.byref(mode))
    k32.SetConsoleMode(h, mode.value | 0x0004)   # VIRTUAL_TERMINAL_PROCESSING


# Blue (cold) -> grey -> yellow -> red (hot), as xterm-256 indices.
RAMP = [17, 18, 19, 20, 26, 32, 38, 44, 82, 118, 154, 190, 226, 220, 214, 208, 202, 196]


def heat(v, lo, hi):
    if hi <= lo:
        t = 0.0
    else:
        t = (v - lo) / (hi - lo)
    t = max(0.0, min(1.0, t))
    return RAMP[int(t * (len(RAMP) - 1))]


def draw(vals, peak_idx):
    lo, hi = min(vals), max(vals)
    out = ["\x1b[H\x1b[J", "    " + "".join("%5d" % c for c in range(COLS))]
    for r in range(ROWS):
        row = ["%3d " % r]
        for c in range(COLS):
            i = r * COLS + c
            v = vals[i]
            mark = "*" if i == peak_idx else " "
            row.append("\x1b[48;5;%dm\x1b[38;5;232m%4d%s\x1b[0m"
                       % (heat(v, lo, hi), int(v), mark))
        out.append("".join(row))
    out.append("")
    out.append("min %.0f  max %.0f  peak at col %d row %d  (Ctrl+C to stop)"
               % (lo, hi, peak_idx % COLS, peak_idx // COLS))
    print("\n".join(out), flush=True)


def stats(vals, label, unit=""):
    lo, hi = min(vals), max(vals)
    mean = sum(vals) / len(vals)
    print("%s: min %.2f  mean %.2f  max %.2f %s" % (label, lo, mean, hi, unit))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "live"

    port = find_port()
    if not port:
        print("No aim1k CDC port found. Is DEBUG_CONSOLE=1 in config.h, and the "
              "board flashed with that build?")
        return 1
    print("aim1k debug console on %s" % port)

    con = Console(port)
    time.sleep(0.2)
    con.buf = b""

    try:
        if cmd == "noise":
            con.write("n")
            v = con.frame("N")
            if not v:
                print("no reply")
                return 1
            stats(v, "sigma", "counts")
            print("\nPRESENCE_ENTER_SNR=8 means a peak must clear ~%.0f counts "
                  "at the noisiest sensor." % (8 * max(v)))
            return 0

        if cmd in ("raw", "baseline"):
            tag, key = ("R", "r") if cmd == "raw" else ("B", "b")
            con.write(key)
            v = con.frame(tag)
            if not v:
                print("no reply")
                return 1
            stats(v, cmd, "counts")
            enable_vt()
            draw(v, v.index(max(v)))
            return 0

        if cmd == "probe":
            secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
            con.write("s")
            best, best_i, frames = -1e9, -1, 0
            end = time.time() + secs
            print("hold the magnet on ONE sensor for %.0fs..." % secs)
            while time.time() < end:
                v = con.frame("D", timeout=1.0)
                if not v:
                    continue
                frames += 1
                m = max(v)
                if m > best:
                    best, best_i = m, v.index(m)
            con.write("x")
            if best_i < 0:
                print("no frames received")
                return 1
            print("\n%d frames | strongest deflection %.0f counts "
                  "at col %d row %d (index %d)"
                  % (frames, best, best_i % COLS, best_i // COLS, best_i))
            print("that is ~%.1f sigma above noise" % (best / 5.8))
            return 0

        if cmd == "info":
            con.write("i")
            # The connect banner arrives first; keep only the info reply.
            for _ in range(16):
                line = con.readline(1.0)
                if line and (line.startswith("scan ") or line.startswith("pen ")):
                    print(line)
            return 0

        if cmd == "cal":
            con.write("c")
            print("recalibrating - keep the pen away for a second")
            time.sleep(1.5)
            con.write("i")
            for _ in range(6):
                line = con.readline(1.0)
                if line:
                    print(line)
            return 0

        # live heatmap
        enable_vt()
        con.write("s")
        try:
            while True:
                v = con.frame("D", timeout=1.0)
                if v:
                    draw(v, v.index(max(v)))
        except KeyboardInterrupt:
            con.write("x")
            print("\nstopped")
        return 0
    finally:
        con.close()


if __name__ == "__main__":
    sys.exit(main())
