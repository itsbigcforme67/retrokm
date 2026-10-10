// lil' C hardware options. Change these to match your Stack-chan base.
#pragma once
#include "board.h"

// Servo type: 0 = none (face only)
//             1 = SG90-style PWM servos (community Stack-chan boards)
//             2 = SCS0009 serial servos on a community board
//             3 = official M5Stack StackChan body (SCS0009 + servo power
//                 switched by its PY32 IO expander)
#ifndef SERVO_TYPE
#define SERVO_TYPE 3
#endif

// PWM servos: Port C on CoreS3 (the usual Stack-chan wiring)
#define SERVO_PIN_X 18   // pan (left/right)
#define SERVO_PIN_Y 17   // tilt (up/down)

// SCS0009 serial servos (types 2 and 3): UART pins and servo IDs
#define SCS_TX_PIN 6
#define SCS_RX_PIN 7
#define SCS_ID_X 1
#define SCS_ID_Y 2

// Neutral angles and how far lil' C may move (degrees). If the head points
// the wrong way at rest, adjust the centres; flip a sign to reverse an axis.
#define SERVO_X_CENTER 90
#define SERVO_Y_CENTER 85
#define SERVO_X_RANGE  50
#define SERVO_Y_RANGE  15
#define SERVO_X_DIR    1
#define SERVO_Y_DIR    1

// Official body (type 3): raw servo positions for "straight ahead" come from
// the calibration M5Stack's firmware saved (NVS "servo"), else these. Pitch
// 0 degrees is the lowest point; lil' C rests a little above it.
#define M5SC_YAW_ZERO   460
#define M5SC_PITCH_ZERO 620
#define M5SC_PITCH_REST 20

// How far the head may turn to look at a machine away from the desk
#if SERVO_TYPE == 3
#define HEAD_FAR_YAW   125   // the official body's yaw servo allows ~128
#else
#define HEAD_FAR_YAW   80
#endif
#define HEAD_FAR_PITCH 35

#define MAX_RECORD_SECONDS 10
#define TASK_POLL_MS 20000
#define PRESENCE_POLL_MS 700   // asking the bridge where lil' C is
#define VOLUME 160
