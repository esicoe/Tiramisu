// OpenTabletDriver report parser for the aim1k tablet.
//
// Report layout (InputReportLength = 8), matching firmware hid_report.h:
//   [0] report id (1)
//   [1] buttons: bit0 tip switch, bit1 in range, bit2 barrel switch
//   [2..3] X, little-endian uint16, 0..11000  (0.01 mm)
//   [4..5] Y, little-endian uint16, 0..9000
//   [6..7] pressure, little-endian uint16, 0..4095 (stubbed 0 for now)
//
// Targets OpenTabletDriver 0.6.x. For 0.5.x, change the namespace below to
// OpenTabletDriver.Plugin.Tablet.
using System.Numerics;
using OpenTabletDriver.Tablet;

namespace Aim1k
{
    public struct Aim1kReport : ITabletReport, IProximityReport
    {
        public Aim1kReport(byte[] report)
        {
            Raw = report;

            byte buttons = report[1];
            bool tip      = (buttons & 0x01) != 0;
            NearProximity = (buttons & 0x02) != 0;   // in-range / hover
            bool barrel   = (buttons & 0x04) != 0;

            uint x = (uint)(report[2] | (report[3] << 8));
            uint y = (uint)(report[4] | (report[5] << 8));
            uint p = (uint)(report[6] | (report[7] << 8));

            Position = new Vector2(x, y);
            PenButtons = new[] { barrel };

            // The firmware does not yet produce real pressure, so drive OTD's
            // pressure-based tip detection from the tip switch bit: full scale
            // when the tip is down, zero otherwise.
            Pressure = tip ? 4095u : p;

            HoverDistance = 0;
        }

        public byte[] Raw { get; set; }
        public uint ReportID { get; set; }
        public Vector2 Position { get; set; }
        public uint Pressure { get; set; }
        public bool[] PenButtons { get; set; }
        public bool NearProximity { get; set; }
        public uint HoverDistance { get; set; }
    }

    public class Aim1kReportParser : IReportParser<IDeviceReport>
    {
        public IDeviceReport Parse(byte[] report) => new Aim1kReport(report);
    }
}
