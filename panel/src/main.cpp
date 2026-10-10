// RetroKM touch panel firmware for the M5Stack Tab5: the desk map full time.
// The panel itself is lib/rkm_panel (lil' C's Tab5 firmware embeds it too).
//
// The same code runs in a window on Linux (env:native, SDL2) for testing.
#include <M5Unified.h>
#include "secrets.h"
#include "rkm_panel.h"
#if !defined(PANEL_NATIVE)
#include <WiFi.h>
#include <ArduinoOTA.h>
// ESP32-C6 WiFi co-processor on SDIO (M5Stack Tab5 docs)
#define WIFI_SDIO_PINS 12, 13, 11, 10, 9, 8, 15
#endif

void setup() {
  M5.begin();
  M5.Display.setRotation(1);
  M5.Display.setBrightness(140);
#if defined(PANEL_NATIVE)
  const char* host = getenv("PANEL_HUB") ? getenv("PANEL_HUB") : HUB_HOST;
  int port = getenv("PANEL_PORT") ? atoi(getenv("PANEL_PORT")) : HUB_PORT;
  rkmPanelBegin(host, port, getenv("PANEL_EMBEDDED") != nullptr);
#else
  WiFi.setPins(WIFI_SDIO_PINS);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  rkmPanelBegin(HUB_HOST, HUB_PORT);
  // updates over WiFi:  pio run -e tab5-ota -t upload   (finds rkm-panel.local)
  ArduinoOTA.setHostname("rkm-panel");
  ArduinoOTA.onStart([] {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.drawString("updating...", M5.Display.width() / 2, M5.Display.height() / 2);
  });
  ArduinoOTA.begin();
#endif
  rkmPanelShow();
}

void loop() {
  M5.update();
#if defined(PANEL_NATIVE)
  rkmPanelPoll(true);
  if (!rkmPanelLoop()) exit(0);  // (PANEL_EMBEDDED: the back button quits)
  lgfx::delay(8);
#else
  rkmPanelPoll(WiFi.status() == WL_CONNECTED);
  ArduinoOTA.handle();
  static uint32_t lastJoin = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastJoin > 10000) {
    lastJoin = millis();
    WiFi.reconnect();
  }
  rkmPanelLoop();
  delay(2);
#endif
}

#if defined(PANEL_NATIVE)
int user_func(bool* running) {
  setup();
  do { loop(); } while (*running);
  return 0;
}
int main(int, char**) { return lgfx::Panel_sdl::main(user_func, 128); }
#endif
