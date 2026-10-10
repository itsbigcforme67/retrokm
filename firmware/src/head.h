// Stack-chan head movement: SG90 PWM servos or SCS0009 serial servos.
#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include "config.h"

class Head {
 public:
  // Offsets from centre in degrees; +x = robot looks to its left, +y = up
  float targetX = 0, targetY = 0;
  float x = 0, y = 0;

  void begin() {
#if SERVO_TYPE == 1
    ledcSetup(kChX, 50, 16);
    ledcSetup(kChY, 50, 16);
    ledcAttachPin(SERVO_PIN_X, kChX);
    ledcAttachPin(SERVO_PIN_Y, kChY);
#elif SERVO_TYPE == 2
    Serial1.begin(1000000, SERIAL_8N1, SCS_RX_PIN, SCS_TX_PIN);
#elif SERVO_TYPE == 3
    m5scBegin();
#endif
    write(true);
  }

  void look(float dx, float dy) {
    targetX = constrain(dx, -SERVO_X_RANGE, SERVO_X_RANGE);
    targetY = constrain(dy, -SERVO_Y_RANGE, SERVO_Y_RANGE);
  }

  // Turn to face something away from the desk (e.g. a computer behind him)
  void lookFar(float dx, float dy) {
    targetX = constrain(dx, -HEAD_FAR_YAW, HEAD_FAR_YAW);
    targetY = constrain(dy, -SERVO_Y_RANGE, HEAD_FAR_PITCH);
  }

  // Call every frame; eases towards the target
  void update(float speed = 0.12f) {
    x += (targetX - x) * speed;
    y += (targetY - y) * speed;
    write(false);
  }

  // -1..1, for moving the eyes along with the head
  float gazeX() const { return x / SERVO_X_RANGE; }
  float gazeY() const { return y / SERVO_Y_RANGE; }

 private:
  static const int kChX = 6, kChY = 7;  // LEDC channels
  float sentX = 999, sentY = 999;
  uint32_t lastSend = 0;

  void write(bool force) {
    if (!force && fabsf(x - sentX) < 0.3f && fabsf(y - sentY) < 0.3f) return;
    if (!force && millis() - lastSend < 20) return;
    lastSend = millis();
    sentX = x;
    sentY = y;
    float ax = SERVO_X_CENTER + SERVO_X_DIR * x;
    float ay = SERVO_Y_CENTER + SERVO_Y_DIR * y;
#if SERVO_TYPE == 1
    ledcWrite(kChX, pwmDuty(ax));
    ledcWrite(kChY, pwmDuty(ay));
#elif SERVO_TYPE == 2
    scsWrite(SCS_ID_X, ax);
    scsWrite(SCS_ID_Y, ay);
#elif SERVO_TYPE == 3
    (void)ax;
    (void)ay;
    // 0.3125 degrees per step; yaw +x = left, pitch +y = up
    scsPacket(SCS_ID_X, 0x2A, yawZero + (int)(SERVO_X_DIR * x * 3.2f), 20);
    scsPacket(SCS_ID_Y, 0x2A, pitchZero + (int)((M5SC_PITCH_REST + SERVO_Y_DIR * y) * 3.2f), 20);
#else
    (void)ax;
    (void)ay;
#endif
  }

  static uint32_t pwmDuty(float deg) {
    float us = 500.0f + constrain(deg, 0.0f, 180.0f) * (2000.0f / 180.0f);
    return (uint32_t)(us * 65535.0f / 20000.0f);
  }

  int yawZero = M5SC_YAW_ZERO, pitchZero = M5SC_PITCH_ZERO;

  // Official StackChan body: switch on servo power (IO expander pin 0),
  // load M5Stack's saved centre calibration, enable both servos.
  void m5scBegin() {
    const uint8_t addrs[] = {0x6F, 0x71};
    uint8_t addr = 0;
    for (uint32_t t0 = millis(); !addr && millis() - t0 < 1500; delay(200))
      for (uint8_t a : addrs)
        if (M5.In_I2C.start(a, false, 100000) && M5.In_I2C.stop()) { addr = a; break; }
    if (addr) {
      M5.In_I2C.bitOn(addr, 0x03, 0x01, 100000);   // pin 0 = output
      M5.In_I2C.bitOn(addr, 0x09, 0x01, 100000);   // pull-up
      M5.In_I2C.bitOff(addr, 0x0B, 0x01, 100000);  // no pull-down
      M5.In_I2C.bitOn(addr, 0x05, 0x01, 100000);   // servo power on
      delay(200);
    } else {
      log_e("StackChan IO expander not found; servos may stay unpowered");
    }
    Preferences nvs;
    if (nvs.begin("servo", true)) {
      int y = nvs.getInt("zero_pos_1", -1), p = nvs.getInt("zero_pos_2", -1);
      if (y >= 0 && y <= 1000) yawZero = y;
      if (p >= 0 && p <= 1000) pitchZero = p;
      nvs.end();
    }
    Serial1.begin(1000000, SERIAL_8N1, SCS_RX_PIN, SCS_TX_PIN);
    for (uint8_t id : {(uint8_t)SCS_ID_X, (uint8_t)SCS_ID_Y}) {
      // Restore position mode in case the yaw servo was left spinning freely
      // (M5Stack's firmware zeroes the angle limits for that), then torque on
      scsPacket(id, 9, 0, -1);
      scsPacket(id, 11, 1000, -1);
      uint8_t torque[] = {0xFF, 0xFF, id, 4, 0x03, 40, 1, 0};
      torque[7] = ~(id + 4 + 0x03 + 40 + 1);
      Serial1.write(torque, sizeof torque);
      delay(5);
    }
  }

  // Write a 16-bit value (big-endian) to addr; with time >= 0 also send the
  // goal time and a zero speed, as for a goal-position write.
  static void scsPacket(uint8_t id, uint8_t addr, int value, int time) {
    value = constrain(value, 0, 1000);
    uint8_t p[13] = {0xFF, 0xFF, id, 0, 0x03, addr,
                     (uint8_t)(value >> 8), (uint8_t)value};
    int n = 8;
    if (time >= 0) {
      p[n++] = time >> 8; p[n++] = time; p[n++] = 0; p[n++] = 0;
    }
    p[3] = n - 4 + 1;  // params + instruction + checksum
    uint8_t sum = 0;
    for (int i = 2; i < n; i++) sum += p[i];
    p[n++] = ~sum;
    Serial1.write(p, n);
  }

  // SCS0009: 0..1023 covers 300 degrees, 512 = 150 degrees. Stack-chan
  // treats 90 degrees as straight ahead, so 90 maps to the middle (512).
  static void scsWrite(uint8_t id, float deg) {
    int pos = 512 + (int)((deg - 90.0f) * (1024.0f / 300.0f));
    pos = constrain(pos, 0, 1023);
    // Write goal position (0x2A): pos, time, speed, big-endian
    uint8_t p[13] = {0xFF, 0xFF, id, 9, 0x03, 0x2A,
                     (uint8_t)(pos >> 8), (uint8_t)pos, 0, 0, 0, 0, 0};
    uint8_t sum = 0;
    for (int i = 2; i < 12; i++) sum += p[i];
    p[12] = ~sum;
    Serial1.write(p, sizeof p);
  }
};
