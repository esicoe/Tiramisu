// USB + HID descriptors for the aim1k tablet.
//
// The device exposes an absolute-mouse collection (driverless cursor, no Windows
// Ink) alongside a vendor-defined collection (OpenTabletDriver). See config.h for
// why both are needed and how the two are kept from fighting over the cursor.
#include <string.h>
#include "tusb.h"
#include "config.h"
#include "hid_report.h"

// ---- HID report descriptor --------------------------------------------------
// Two top-level collections on one interface, selected by report ID:
//
//   1  absolute mouse   Windows moves the cursor with no driver installed.
//                       A mouse is NOT the pen/ink stack, so Windows Ink never
//                       enters the picture - that is the whole point of using a
//                       mouse here rather than a Digitizer/Pen collection.
//   2  vendor-defined   OpenTabletDriver reads this. Windows claims collections
//                       individually and leaves vendor ones alone, so OTD can
//                       open collection 2 even while Windows owns collection 1.
//   3  feature report   host switches the mouse collection on/off (see main.c),
//                       so the two never drive the cursor at the same time.
//
// Pen logical maxima are little-endian: X=11000(0x2AF8), Y=9000(0x2328),
// P=4095(0x0FFF). Keep these in sync with config.h.
static const uint8_t desc_hid_report[] = {
#if !VENDOR_ONLY
    // ---- Collection 1: absolute mouse ----
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, REPORT_ID_MOUSE,
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Button)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x03,        //     Usage Maximum (Button 3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x01,        //     Report Size (1)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x75, 0x05,        //     Report Size (5)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Const) - pad to a byte
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x16, 0x00, 0x00,  //     Logical Minimum (0)
    0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
    0x75, 0x10,        //     Report Size (16)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x02,        //     Input (Data,Var,Abs) - ABSOLUTE, not relative
    0xC0,              //   End Collection
    0xC0,              // End Collection
#endif

    // ---- Collection 2: vendor-defined (OpenTabletDriver) ----
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (Vendor Usage 1)
    0xA1, 0x01,        // Collection (Application)
    0x85, REPORT_ID_PEN,
    0x09, 0x01,        //   Usage (Vendor Usage 1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x07,        //   Report Count (7)  == sizeof(pen_report_t)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    // Feature report: byte 0 is 1 to keep the mouse collection reporting, 0 to
    // silence it so OTD alone drives the cursor.
    0x85, REPORT_ID_MODE,
    0x09, 0x02,        //   Usage (Vendor Usage 2)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x01,        //   Report Count (1)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0xC0               // End Collection
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

// ---- Device descriptor ------------------------------------------------------
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
#if DEBUG_CONSOLE
    // Composite (CDC + HID) needs the IAD triple so Windows binds both functions.
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
#else
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
#endif
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

// ---- Configuration descriptor ----------------------------------------------
#if DEBUG_CONSOLE
enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_HID, ITF_NUM_TOTAL };
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_HID       0x83
#define CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
#else
enum { ITF_NUM_HID = 0, ITF_NUM_TOTAL };
#define EPNUM_HID 0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#endif

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
#if DEBUG_CONSOLE
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#endif
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report), EPNUM_HID,
                       CFG_TUD_HID_EP_BUFSIZE, HID_POLL_MS),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// ---- String descriptors -----------------------------------------------------
static const char *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 }, // 0: English (US)
    USB_MANUFACTURER,             // 1
    USB_PRODUCT,                  // 2
    "aim1k-0001",                 // 3: serial
    "aim1k debug",                // 4: CDC console
};

static uint16_t _desc_str[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if (index == 0) {
        _desc_str[1] = 0x0409;
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
            return NULL;
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

// HID class callbacks (get/set report) live in main.c - the feature report
// carries the mouse-collection on/off switch, which is report-loop state.
