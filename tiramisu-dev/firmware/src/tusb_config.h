#pragma once
// TinyUSB configuration: HID digitizer, plus an optional CDC debug console.
#include "config.h"

#define CFG_TUSB_MCU          OPT_MCU_RP2040   // RP2350 uses the rp2040 port family
#define CFG_TUSB_OS           OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENABLED       1
#define CFG_TUD_ENDPOINT0_SIZE 64

// Classes
#define CFG_TUD_HID           1
#define CFG_TUD_CDC           DEBUG_CONSOLE
#define CFG_TUD_MSC           0
#define CFG_TUD_MIDI          0
#define CFG_TUD_VENDOR        0

// HID buffers
#define CFG_TUD_HID_EP_BUFSIZE 16

// CDC buffers. TX must hold a whole 120-sensor text frame (~700 B) so the debug
// dump never has to be split across report-loop iterations.
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 2048
#define CFG_TUD_CDC_EP_BUFSIZE 64
