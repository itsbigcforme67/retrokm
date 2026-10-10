// The RetroKM desk map (the touch panel) as a component, so it can run on its
// own (src/main.cpp) or inside another Tab5 firmware (lil' C's desktop).
// The host owns WiFi, M5.begin() and M5.update(); the screen is 1280x720 in
// rotation 1.
#pragma once
#include <stdint.h>

// Once, after M5.begin().  inside: running in another app, so the header
// gets a back button and rkmPanelLoop() can return false.
void rkmPanelBegin(const char* hubHost, int hubPort, bool inside = false);
// Every pass of the host's loop, on screen or not: keeps the hub link up.
void rkmPanelPoll(bool netReady);
// The panel is about to take over the screen: forget old touches, redraw all.
void rkmPanelShow();
// While it is on screen, after M5.update(): reads the touch screen and draws.
// Returns false when the back button was tapped.
bool rkmPanelLoop();
// The hub is connected and has sent the layout.
bool rkmPanelConnected();
uint32_t nowMs();
