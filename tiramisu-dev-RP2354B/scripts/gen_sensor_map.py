"""Regenerate firmware/include/sensor_map.h from the KiCad PCB.

Ground truth for three independent things, all read straight out of
pcb/tiramisu-dev.kicad_pcb:

  1. WHERE each HAL403SO sits -> its (col,row), from the footprint placement.
  2. HOW each one is wired    -> its (mux, channel), by following the sensor's
     OUT net to a CD74HC4067 input pad.
  3. WHICH ADC input reads each mux -> by following the mux COM net to an
     RP2354B pad and reading that pad's GPIOxx/ADCn function.

Everything comes from `pinfunction` fields in the PCB, never from an assumed
pinout: the CD74HC4067 footprint here does NOT use the pad order you would guess
from the datasheet (pad 1 is COM, not I5), and the ADC net names are reversed
relative to the ADC input numbers (net "ADC0" is on GPIO47 = ADC input 7).

The emitted table is indexed by ADC INPUT, not by a mux letter, so the firmware
can do SENSOR_GRID[adc_input][address] with no reversal step in between.

    python gen_sensor_map.py            # report what the PCB says
    python gen_sensor_map.py --write    # also rewrite sensor_map.h
"""

import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PCB = os.path.join(ROOT, "pcb", "tiramisu-dev.kicad_pcb")
OUT = os.path.join(ROOT, "firmware", "include", "sensor_map.h")

SENSOR_VALUE = "HAL403SO"
MUX_VALUE = "CD74HC4067M"
MCU_VALUE = "RP2354B"


def balanced_blocks(text, tag):
    """Yield each balanced '(tag ...)' substring."""
    needle = "(" + tag
    i, n = 0, len(text)
    while True:
        i = text.find(needle, i)
        if i < 0:
            return
        j = i + len(needle)
        if j < n and text[j] not in " \t\n":
            i = j
            continue
        depth, k, instr = 0, i, False
        while k < n:
            c = text[k]
            if instr:
                if c == "\\":
                    k += 2
                    continue
                if c == '"':
                    instr = False
            elif c == '"':
                instr = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    yield text[i:k + 1]
                    break
            k += 1
        i = k + 1


RE_REF = re.compile(r'\(property "Reference" "([^"]+)"')
RE_VAL = re.compile(r'\(property "Value" "([^"]+)"')
RE_AT = re.compile(r"\(at (-?[\d.]+) (-?[\d.]+)")
RE_PAD = re.compile(r'\(pad "([^"]*)"')
RE_NET = re.compile(r'\(net (?:\d+ )?"([^"]*)"\)')
RE_FN = re.compile(r'\(pinfunction "([^"]*)"')
# pinfunction is "<name>_<padnumber>", e.g. I11_20, COM_1, OUT_2, GPIO40/ADC0_49
RE_GPIO_ADC = re.compile(r"^GPIO(\d+)/ADC(\d+)")


class Part:
    __slots__ = ("ref", "val", "x", "y", "pads")

    def __init__(self, ref, val, x, y, pads):
        self.ref, self.val, self.x, self.y = ref, val, x, y
        self.pads = pads          # padnum -> (pinfunction_name, net)


def pin_name(fn, padnum):
    """Strip the '_<padnumber>' suffix KiCad appends to pinfunction."""
    suffix = "_" + padnum
    return fn[:-len(suffix)] if fn.endswith(suffix) else fn


def load_parts():
    with open(PCB, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    parts = []
    for blk in balanced_blocks(text, "footprint"):
        mref, mval, mat = RE_REF.search(blk), RE_VAL.search(blk), RE_AT.search(blk)
        if not (mref and mval and mat):
            continue
        pads = {}
        for pblk in balanced_blocks(blk, "pad"):
            mp = RE_PAD.search(pblk)
            if not mp:
                continue
            num = mp.group(1)
            mn, mf = RE_NET.search(pblk), RE_FN.search(pblk)
            pads[num] = (pin_name(mf.group(1), num) if mf else "",
                         mn.group(1) if mn else "")
        parts.append(Part(mref.group(1), mval.group(1),
                          float(mat.group(1)), float(mat.group(2)), pads))
    return parts


def cluster(values, tol=2.0):
    uniq = sorted(set(values))
    bins = []
    for v in uniq:
        if bins and abs(v - bins[-1][-1]) <= tol:
            bins[-1].append(v)
        else:
            bins.append([v])
    return ({v: i for i, b in enumerate(bins) for v in b},
            [sum(b) / len(b) for b in bins])


def main():
    parts = load_parts()
    sensors = [p for p in parts if p.val == SENSOR_VALUE]
    muxes = [p for p in parts if p.val == MUX_VALUE]
    mcus = [p for p in parts if p.val == MCU_VALUE]

    print("sensors %d   muxes %d   mcu %d" % (len(sensors), len(muxes), len(mcus)))
    if len(sensors) != 120 or len(muxes) != 8 or len(mcus) != 1:
        print("unexpected part counts - aborting")
        return 1

    # ---- 1. geometry --------------------------------------------------------
    colmap, colc = cluster([p.x for p in sensors])
    rowmap, rowc = cluster([p.y for p in sensors])
    cols, rows = len(colc), len(rowc)
    dx = (colc[-1] - colc[0]) / (cols - 1)
    dy = (rowc[-1] - rowc[0]) / (rows - 1)
    print("grid %d cols x %d rows | pitch %.3f x %.3f mm | span %.2f x %.2f mm"
          % (cols, rows, dx, dy, colc[-1] - colc[0], rowc[-1] - rowc[0]))
    if cols * rows != len(sensors):
        print("  WARNING: %d x %d != %d sensors" % (cols, rows, len(sensors)))
        return 1

    # ---- 2. net -> ADC input, via the MCU's own pin functions ---------------
    net_to_adc, sel_gpio = {}, {}
    for num, (fn, net) in mcus[0].pads.items():
        m = RE_GPIO_ADC.match(fn)
        if m and net:
            net_to_adc[net] = int(m.group(2))
        elif net.startswith("/S") and fn.startswith("GPIO"):
            sel_gpio[net[1:]] = int(re.match(r"GPIO(\d+)", fn).group(1))
    print("\nADC nets (note the reversal):")
    for net in sorted(net_to_adc, key=lambda n: net_to_adc[n]):
        print("  net %-6s -> ADC input %d" % (net, net_to_adc[net]))
    print("select lines: " + ", ".join("%s=GPIO%d" % (k, sel_gpio[k])
                                       for k in sorted(sel_gpio)))

    # ---- 3. mux -> ADC input, and its channel nets --------------------------
    mux_adc = {}
    net_to_mux_channel = {}
    for m in muxes:
        com_net = next((net for fn, net in m.pads.values() if fn.startswith("COM")), None)
        if com_net not in net_to_adc:
            print("  mux %s COM net %r does not reach the MCU" % (m.ref, com_net))
            return 1
        adc = net_to_adc[com_net]
        mux_adc[m.ref] = adc
        for fn, net in m.pads.values():
            mm = re.match(r"^I(\d+)$", fn)
            if mm and net and net != "GND":
                net_to_mux_channel[net] = (adc, int(mm.group(1)), m.ref)

    print("\nmux -> ADC input:")
    for m in sorted(muxes, key=lambda p: mux_adc[p.ref]):
        com = next(net for fn, net in m.pads.values() if fn.startswith("COM"))
        print("  %-4s COM=%-6s -> ADC input %d" % (m.ref, com, mux_adc[m.ref]))

    # ---- 4. sensor -> (adc input, channel) ----------------------------------
    grid = [[0xFFFF] * 16 for _ in range(8)]
    placed, unmapped, clashes = 0, [], []
    for s in sensors:
        out_net = next((net for fn, net in s.pads.values() if fn.startswith("OUT")), None)
        hit = net_to_mux_channel.get(out_net)
        if not hit:
            unmapped.append(s.ref)
            continue
        adc, ch, _ = hit
        idx = rowmap[s.y] * cols + colmap[s.x]
        if grid[adc][ch] != 0xFFFF:
            clashes.append((s.ref, adc, ch))
        grid[adc][ch] = idx
        placed += 1

    print("\nmapped %d/120 sensors" % placed)
    if unmapped:
        print("  UNMAPPED: %s" % ", ".join(sorted(unmapped)[:12]))
    if clashes:
        print("  COLLISIONS: %s" % clashes[:8])
    if placed != 120 or unmapped or clashes:
        return 1

    # Every used channel must be 0..14; channel 15 is tied to GND on every mux.
    used_ch = {c for m in grid for c, v in enumerate(m) if v != 0xFFFF}
    print("channels used: %s" % sorted(used_ch))

    if "--write" not in sys.argv:
        print("\n(dry run - pass --write to update sensor_map.h)")
        return 0

    L = []
    L.append("// AUTO-GENERATED by scripts/gen_sensor_map.py from tiramisu-dev.kicad_pcb")
    L.append("// - do not edit by hand. Re-run it whenever the PCB changes.")
    L.append("// %d x HAL403SO, %d cols x %d rows, %.1f mm pitch."
             % (len(sensors), cols, rows, dx))
    L.append("#pragma once")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("#define SENSOR_COLS   %d" % cols)
    L.append("#define SENSOR_ROWS   %d" % rows)
    L.append("#define SENSOR_COUNT  %d" % (cols * rows))
    L.append("#define SENSOR_PITCH_MM   %.1ff" % dx)
    L.append("#define MUX_COUNT     8")
    L.append("#define MUX_CHANNELS  16")
    L.append("#define MUX_USED_CH   15   // channel 15 is tied to GND on every mux")
    L.append("")
    L.append("// Mux select lines (shared bus). Enables are tied low in hardware.")
    for i in range(4):
        L.append("#define PIN_SEL_S%d  %d" % (i, sel_gpio["S%d" % i]))
    L.append("#define ADC_GPIO_BASE 40  // ADC input i -> GPIO(40+i)")
    L.append("")
    L.append("// grid[adc_input][channel] -> linear sensor index (row*COLS+col),")
    L.append("// 0xFFFF = unused. Indexed by ADC INPUT, so the scanner needs no")
    L.append("// reversal step: the net names ADC0..ADC7 are wired backwards, and")
    L.append("// that is already accounted for here.")
    L.append("static const uint16_t SENSOR_GRID[MUX_COUNT][MUX_CHANNELS] = {")
    by_adc = {mux_adc[m.ref]: m for m in muxes}
    for a in range(8):
        body = ", ".join("0x%04X" % v for v in grid[a])
        com = next(net for fn, net in by_adc[a].pads.values() if fn.startswith("COM"))
        L.append("  { %s }, // ADC in %d = GPIO%d, %s (net %s)"
                 % (body, a, 40 + a, by_adc[a].ref, com))
    L.append("};")
    L.append("")
    L.append("static inline float sensor_x_mm(int idx)"
             "{ return (idx % SENSOR_COLS) * SENSOR_PITCH_MM; }")
    L.append("static inline float sensor_y_mm(int idx)"
             "{ return (idx / SENSOR_COLS) * SENSOR_PITCH_MM; }")
    L.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L))
    print("\nwrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
