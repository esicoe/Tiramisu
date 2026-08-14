# aim1k — OpenTabletDriver support

Two files:

- `aim1k.json` — the tablet configuration (VID/PID, physical size, report parser).
- `Aim1kReportParser.cs` — decodes the 8-byte HID report into an OTD report.

## Why both

The firmware exposes a **vendor-defined HID collection** alongside the
absolute-mouse one (see `HID personality` in `../firmware/include/config.h`).
That is deliberate, and it is what makes OTD work at all:

- Windows opens **Digitizer/Pen** collections **exclusively**. With a pen
  descriptor, OTD — or any other userspace reader — gets `ACCESS_DENIED` when it
  tries to open the device. No OTD config can work around that.
- A pen descriptor also puts **Windows Ink** in the path, which is exactly the
  smoothing and press-and-hold behaviour osu! players turn off.

A vendor-defined collection sidesteps both. On its own it would mean nothing
moves the cursor without OTD, which is why the firmware also ships the
absolute-mouse collection beside it: Windows claims collections individually, so
the mouse drives the cursor driverless while the vendor one stays open for OTD.
OTD switches the mouse off when it connects (`FeatureInitReport` below) so the
two never fight. Set `VENDOR_ONLY 1` in `config.h` to drop the mouse collection
entirely, or `MOUSE_DEFAULT_ON 0` to leave it silent until something asks.

## Install

**No plugin DLL is needed.** `Aim1kReportParser.cs` is kept only as a starting
point if you later want proximity or pressure handling; the stock parser covers
the current report exactly.

1. Copy `aim1k.json` into the OTD configurations folder:
   - Windows: `%LOCALAPPDATA%\OpenTabletDriver\Configurations\`

   Creating that folder is safe: OTD **adds** these to its built-in configs
   rather than replacing them (verified on 0.6.7 — 341 configs loaded, all the
   stock ones still present alongside aim1k).

2. **Restart the OTD daemon.** This step is not optional and is the most likely
   thing to waste your time: the daemon enumerates HID devices once at startup,
   so a daemon that was already running when you reflashed the firmware will
   report `Device not found` even though the config loads and the device is
   plainly visible to everything else. Kill `OpenTabletDriver.Daemon.exe` and
   start it again after any reflash that changes the USB descriptors.

3. The tablet appears as **"aim1k tablet"**, 110 × 90 mm. Map your osu! area
   inside that with output mode *Absolute*.

A healthy detection looks like this in the log
(`%LOCALAPPDATA%\OpenTabletDriver\Logs\`):

```
[Device] Initializing device 'aim1k tablet' \\?\hid#vid_1209&pid_0001&mi_02&col02#...
[Device] Using report parser type 'OpenTabletDriver.Plugin.Tablet.TabletReportParser'
[Device] Set device feature: 03-00
[Detect] Found tablet 'aim1k tablet'
```

`col02` confirms it bound to the vendor collection rather than the mouse one, and
`Set device feature: 03-00` is `FeatureInitReport` switching the absolute-mouse
collection off so OTD alone drives the cursor.

## Notes

- `MaxX/Width = 100`, i.e. coordinates are in 0.01 mm — matches the firmware.
- Pressure is currently synthesised from the tip-switch bit (0 or full scale), so
  OTD's pressure-based tip binding works once you wire a tip switch. Until then
  the tablet is hover-only; bind clicks to the keyboard as usual for osu!.
- If OTD does not pick it up, double-check the VID/PID in `aim1k.json` matches
  `USB_VID`/`USB_PID` in `../firmware/include/config.h`.
