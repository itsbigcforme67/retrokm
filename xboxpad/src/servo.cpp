// The power-button servo: an M5Stack Atom Motion base (I2C 0x38, 4 servo ports, own servo
// power and battery) on G38 (SDA) / G39 (SCL) through the Atom slot. i2c tries both pin orders
// and remembers the one that answers.
// The base makes the pulses; we write one angle register per port.
//   i2c                      list what answers on G38/G39, trying both pin orders
//   servo <port 1-4> <deg>   move a port to 0-180
//   servo rest <deg>         angle that keeps the arm off the button (saved)
//   servo push <deg>         angle that presses the button (saved)
//   press [ms]               push, hold (default 500 ms), back to rest; never left pushed
#include "servo.h"
#include <Preferences.h>
#include <Wire.h>

#define SERVO_SDA 38
#define SERVO_SCL 39
#define SERVO_ADDR 0x38
#define SERVO_PORT 1  // the power-button servo's port on the base

static Preferences sp;
static bool begun;
static int sda = SERVO_SDA, scl = SERVO_SCL;
static int restDeg = -1, pushDeg = -1;
static uint32_t releaseAt;  // millis() when a press goes back to rest, 0 = none

static void wireBegin() {
  if (begun) return;
  sp.begin("servo", false);
  if (sp.getBool("swap", false)) { sda = SERVO_SCL; scl = SERVO_SDA; }
  Wire.begin(sda, scl, 100000);
  restDeg = sp.getInt("rest", -1);
  pushDeg = sp.getInt("push", -1);
  begun = true;
}

static bool setAngle(int port, int deg) {
  Wire.beginTransmission(SERVO_ADDR);
  Wire.write(port - 1);  // Atom Motion registers 0x00-0x03: one angle byte per port
  Wire.write(deg);
  return Wire.endTransmission() == 0;
}

void servo_loop() {
  if (releaseAt && (int32_t)(millis() - releaseAt) >= 0) {
    releaseAt = 0;
    setAngle(SERVO_PORT, restDeg);
  }
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
  if (in.startsWith("servo ")) {
    wireBegin();
    String arg = in.substring(6);
    int sp1 = arg.indexOf(' ');
    String what = sp1 < 0 ? arg : arg.substring(0, sp1);
    int deg = sp1 < 0 ? -1 : arg.substring(sp1 + 1).toInt();
    if (deg < 0 || deg > 180) { reply.println("servo: angle 0-180"); return true; }
    if (what == "rest" || what == "push") {
      (what == "rest" ? restDeg : pushDeg) = deg;
      sp.putInt(what.c_str(), deg);
      reply.printf("servo %s = %d (rest=%d push=%d)\n", what.c_str(), deg, restDeg, pushDeg);
      return true;
    }
    int port = what.toInt();
    if (port < 1 || port > 4) { reply.println("servo: port 1-4, rest or push"); return true; }
    reply.printf("servo %d -> %d: %s\n", port, deg, setAngle(port, deg) ? "ok" : "no answer at 0x38");
    return true;
  }
  if (in == "press" || in.startsWith("press ")) {
    wireBegin();
    int ms = in.length() > 6 ? in.substring(6).toInt() : 500;
    if (restDeg < 0 || pushDeg < 0) { reply.println("press: set servo rest and servo push first"); return true; }
    if (ms < 50 || ms > 10000) { reply.println("press: 50-10000 ms"); return true; }
    if (!setAngle(SERVO_PORT, pushDeg)) { reply.println("press: no answer at 0x38"); return true; }
    releaseAt = millis() + ms;
    if (!releaseAt) releaseAt = 1;
    reply.printf("press: %d ms, then back to %d\n", ms, restDeg);
    return true;
  }
  return false;
}
