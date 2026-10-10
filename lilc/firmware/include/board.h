// lil' C build targets. platformio.ini picks one: -DLILC_CORES3 or -DLILC_TAB5.
//
//   cores3  M5Stack CoreS3 in the Stack-chan body: 320x240 screen, servos.
//   tab5    M5Stack Tab5 (ESP32-P4, WiFi via its ESP32-C6): 1280x720 screen,
//           no head. The face is an xeyes window roaming an X desktop; the
//           320x240 UI is drawn 3x over the left 960x720, and the task list
//           sits in a window on the right.
#pragma once

#if defined(LILC_TAB5)
#define BOARD_NAME   "Tab5"
#define BOARD_DEV    "tab5"      // which of lil' C's homes this is, for the bridge
#define HAS_KVM      1           // the RetroKM desk map, opened from the bottom bar
#define UI_SCALE     3
#define UI_X         0           // where the scaled UI sits on the screen
#define UI_Y         0
#define HAS_PANEL    1           // task list in a window on the right
#define PANEL_W      298
#define PANEL_H      672
// The face is an xeyes window that roams a 1280x720 X desktop (root weave);
// the rest of the UI floats over it
#define DESKTOP      1
#define SCREEN_W     1280
#define SCREEN_H     720
#define SERVO_TYPE   0           // no head
// ESP32-C6 WiFi co-processor on SDIO (M5Stack Tab5 docs)
#define WIFI_SDIO_PINS 12, 13, 11, 10, 9, 8, 15
#else
#define BOARD_NAME   "CoreS3"
#define BOARD_DEV    "stackchan"
#define HAS_KVM      0
#define UI_SCALE     1
#define UI_X         0
#define UI_Y         0
#define HAS_PANEL    0
#define DESKTOP      0
#endif
