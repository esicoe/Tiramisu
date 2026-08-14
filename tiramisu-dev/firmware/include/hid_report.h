#pragma once
#include <stdint.h>

// Report IDs. Two top-level collections share the interface; the ID selects one.
#define REPORT_ID_MOUSE 1   // absolute mouse  -> Windows moves the cursor
#define REPORT_ID_PEN   2   // vendor-defined  -> OpenTabletDriver reads this
#define REPORT_ID_MODE  3   // feature report  -> host switches the mode

// Button bits in pen_report_t.buttons.
//
// IN_RANGE deliberately sits on bit 0. OpenTabletDriver's built-in
// TabletReportParser reads bits 1 and 2 of this byte as the two pen buttons and
// ignores bit 0, so parking in-range there means the stock parser sees "no
// buttons pressed" and NO plugin DLL is needed. Put it on bit 1 and OTD reports
// a pen button held down for as long as the pen is over the tablet.
#define PEN_IN_RANGE (1u << 0)
#define PEN_BUTTON1  (1u << 1)   // OTD PenButtons[0] - unused, hover-only
#define PEN_BUTTON2  (1u << 2)   // OTD PenButtons[1] - unused, hover-only

// The 7-byte payload behind REPORT_ID_PEN. Byte offsets within the full report
// (id first) are exactly what OTD's TabletReportParser expects:
//   [0] id  [1] buttons  [2:4] X  [4:6] Y  [6:8] pressure
typedef struct __attribute__((packed)) {
    uint8_t  buttons;    // PEN_* bit flags
    uint16_t x;          // 0..LOGICAL_MAX_X (absolute)
    uint16_t y;          // 0..LOGICAL_MAX_Y (absolute)
    uint16_t pressure;   // 0..LOGICAL_MAX_P (stubbed for now)
} pen_report_t;

// The 5-byte payload behind REPORT_ID_MOUSE. Absolute coordinates over the full
// virtual desktop, so the tablet maps 1:1 to the screen with no driver at all.
typedef struct __attribute__((packed)) {
    uint8_t  buttons;    // standard HID mouse buttons; never asserted
    uint16_t x;          // 0..32767 absolute
    uint16_t y;          // 0..32767 absolute
} mouse_report_t;

#define MOUSE_LOGICAL_MAX 32767
