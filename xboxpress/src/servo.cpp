// The power-button servo: an M5Stack Atom Motion base (I2C 0x38, 4 servo ports, own servo
// power and battery) on SERVO_SDA / SERVO_SCL through the Atom slot (G38/G39 on an S3, G25/G21 on
// an Atom Lite). i2c tries both pin orders
// and remembers the one that answers.
// The servo is a continuous-rotation one: the "angle" sets speed and direction, 90 (about)
// stops it. So a press is timed: spin toward the button, then spin back a little longer so
// the arm ends against a backstop (no drift from press to press), then stop.
//   i2c                      list what answers on the I2C pins, trying both pin orders
//   servo <port 1-4> <deg>   raw value to a port (continuous servo: 90 stops, else spins)
//   servo stop <deg>         the value that holds it still (saved, default 90)
//   servo push <deg>         spin toward the button (saved, default 70)
//   servo back <deg>         spin away from it (saved, default 110)
//   servo backms <ms>        extra return time beyond the push, to reach the backstop (saved,
//                            default 100; too much leans on the backstop and can peel the servo off)
//   power                    the base's INA226 (0x40): battery-side voltage and shunt (charging)
//   press [ms]               push for ms (default 300: the two-sided arm on the Xbox, 29 Oct), back for ms + backms, stop
#include "servo.h"
#include <Preferences.h>
#include <Wire.h>

#ifndef SERVO_SDA  // Atom S3 family; platformio.ini passes 25 / 21 for the Atom Lite
#define SERVO_SDA 38
#define SERVO_SCL 39
#endif
#define SERVO_ADDR 0x38
#define INA_ADDR 0x40  // the Atom Motion v1.1's INA226 power monitor
#define SERVO_PORT 1  // the power-button servo's port on the base

static Preferences sp;
static bool begun;
static int sda = SERVO_SDA, scl = SERVO_SCL;
static int stopDeg, pushDeg, backDeg, backMs;
static enum { IDLE, PUSHING, RETURNING } phase;
static uint32_t phaseEnd;  // millis() when the current phase ends
static int pushMs;         // this press's push time: the return runs as long, plus backMs

static void wireBegin() {
  if (begun) return;
  sp.begin("servo", false);
  if (sp.getBool("swap", false)) { sda = SERVO_SCL; scl = SERVO_SDA; }
  Wire.begin(sda, scl, 100000);
  stopDeg = sp.getInt("stop", 90);
  pushDeg = sp.getInt("push", 70);
  backDeg = sp.getInt("back", 110);
  backMs = sp.getInt("backx", 100);
  begun = true;
}

static bool setAngle(int port, int deg) {
  Wire.beginTransmission(SERVO_ADDR);
  Wire.write(port - 1);  // Atom Motion registers 0x00-0x03: one angle byte per port
  Wire.write(deg);
  return Wire.endTransmission() == 0;
}

void servo_loop() {
  if (phase == IDLE || (int32_t)(millis() - phaseEnd) < 0) return;
  if (phase == PUSHING) {
    setAngle(SERVO_PORT, backDeg);
    phase = RETURNING;
    phaseEnd = millis() + pushMs + backMs;
  } else {
    setAngle(SERVO_PORT, stopDeg);
    phase = IDLE;
  }
}

static bool inaRead(uint8_t reg, int16_t &v) {
  Wire.beginTransmission(INA_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(INA_ADDR, 2) != 2) return false;
  v = (int16_t)(Wire.read() << 8 | Wire.read());
  return true;
}

static bool setting(const String &what, int v, Print &reply) {
  int *p = what == "stop" ? &stopDeg : what == "push" ? &pushDeg : what == "back" ? &backDeg
         : what == "backms" ? &backMs : nullptr;
  if (!p) return false;
  bool ms = p == &backMs;
  if (ms ? (v < 0 || v > 1000) : (v < 0 || v > 180)) { reply.println(ms ? "servo: 0-1000 ms" : "servo: 0-180"); return true; }
  *p = v;
  sp.putInt(ms ? "backx" : what.c_str(), v);  // backx: backms used to mean the whole return
  if (p == &stopDeg && phase == IDLE) setAngle(SERVO_PORT, stopDeg);
  reply.printf("servo stop=%d push=%d back=%d backms=%d\n", stopDeg, pushDeg, backDeg, backMs);
  return true;
}

bool servo_command(String in, Print &reply) {
  if (in == "i2c") {
    wireBegin();
    for (int pass = 0; pass < 2; pass++) {  // second pass: SDA and SCL the other way round
      reply.printf("i2c SDA=G%d SCL=G%d:", sda, scl);
      int found = 0;
      for (int a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) { reply.printf(" 0x%02x", a); found++; }
      }
      reply.println(found ? "" : " nothing answers");
      if (found) { sp.putBool("swap", sda != SERVO_SDA); return true; }
      Wire.end();
      int t = sda; sda = scl; scl = t;
      Wire.begin(sda, scl, 100000);
    }
    return true;
  }
  if (in == "power") {
    wireBegin();
    int16_t shunt, bus;
    if (!inaRead(0x01, shunt) || !inaRead(0x02, bus)) { reply.println("power: no answer at 0x40"); return true; }
    // bus: 1.25 mV per bit; shunt: 2.5 uV per bit (the sign says which way current flows)
    reply.printf("power: bus %.3f V, shunt %.3f mV\n", bus * 0.00125, shunt * 0.0025);
    return true;
  }
  if (in.startsWith("servo ")) {
    wireBegin();
    String arg = in.substring(6);
    int sp1 = arg.indexOf(' ');
    String what = sp1 < 0 ? arg : arg.substring(0, sp1);
    int v = sp1 < 0 ? -1 : arg.substring(sp1 + 1).toInt();
    if (sp1 < 0) { reply.printf("servo stop=%d push=%d back=%d backms=%d\n", stopDeg, pushDeg, backDeg, backMs); return true; }
    if (setting(what, v, reply)) return true;
    int port = what.toInt();
    if (port < 1 || port > 4) { reply.println("servo: port 1-4, stop, push, back or backms"); return true; }
    if (v < 0 || v > 180) { reply.println("servo: 0-180"); return true; }
    phase = IDLE;  // a raw value cancels a press in progress
    reply.printf("servo %d -> %d: %s\n", port, v, setAngle(port, v) ? "ok" : "no answer at 0x38");
    return true;
  }
  if (in == "press" || in.startsWith("press ")) {
    wireBegin();
    int ms = in.length() > 6 ? in.substring(6).toInt() : 300;
    if (ms < 20 || ms > 3000) { reply.println("press: 20-3000 ms"); return true; }
    if (!setAngle(SERVO_PORT, pushDeg)) { reply.println("press: no answer at 0x38"); return true; }
    phase = PUSHING;
    pushMs = ms;
    phaseEnd = millis() + ms;
    reply.printf("press: push %d ms at %d, back %d ms at %d, stop at %d\n", ms, pushDeg, ms + backMs, backDeg, stopDeg);
    return true;
  }
  return false;
}
