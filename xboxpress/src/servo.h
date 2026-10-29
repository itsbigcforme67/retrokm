#pragma once
#include <Arduino.h>

bool servo_command(String in, Print &reply);  // true if it was a servo command (it replied)
void servo_loop();
