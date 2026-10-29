// The Xbox presser: WiFi, updates over WiFi, and the servo commands of
// servo.cpp on UDP 24854 (and USB serial at 115200).
//   echo press | nc -u -w1 xboxpress.local 24854
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "secrets.h"
#include "servo.h"

#define NAME "xboxpress"

static WiFiUDP ctlUdp;

struct UdpReply : public Print {
  String text;
  size_t write(uint8_t c) override { text += (char)c; return 1; }
};

static void command(String in, Print &reply) {
  in.trim();
  if (!in.length()) return;
  if (servo_command(in, reply)) return;
  if (in == "reboot") { reply.println("rebooting"); delay(100); ESP.restart(); }
  reply.printf("%s wifi=%s uptime=%lus\n", NAME, WiFi.localIP().toString().c_str(), millis() / 1000);
  reply.println("commands: show | i2c | power | servo <port> <deg> | servo stop|push|back <deg> | "
                "servo backms <ms> | press [ms] | reboot");
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(NAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  ArduinoOTA.setHostname(NAME);  // reachable as xboxpress.local
  ArduinoOTA.begin();
  ctlUdp.begin(24854);
  Serial.println(NAME ": up");
}

void loop() {
  static bool announced;
  if (WiFi.status() == WL_CONNECTED && !announced) {
    announced = true;
    Serial.printf("wifi: %s\n", WiFi.localIP().toString().c_str());
  }
  ArduinoOTA.handle();
  int n = ctlUdp.parsePacket();
  if (n > 0) {
    char buf[128];
    n = ctlUdp.read(buf, sizeof buf - 1);
    buf[n < 0 ? 0 : n] = 0;
    UdpReply r;
    command(String(buf), r);
    ctlUdp.beginPacket(ctlUdp.remoteIP(), ctlUdp.remotePort());
    ctlUdp.write((const uint8_t *)r.text.c_str(), r.text.length());
    ctlUdp.endPacket();
  }
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { line += c; continue; }
    command(line, Serial);
    line = "";
  }
  servo_loop();
  delay(1);
}
