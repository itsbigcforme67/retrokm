// Checks the PS/2 keyboard logic against known scan codes and command
// replies.  Build and run:  g++ -std=c++17 -o /tmp/t test/ps2kbd_test.cpp && /tmp/t
#include <cstdio>
#include <initializer_list>
#include <vector>
#include "../src/ps2kbd.h"

static int fails = 0;
static void expect(const char* what, Ps2Keyboard& k, std::initializer_list<int> want) {
  std::vector<int> got(k.out.b, k.out.b + k.out.n);
  std::vector<int> w(want);
  bool ok = got == w;
  printf("%s %-46s", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    printf(" got");
    for (int v : got) printf(" %02X", v);
    printf(" want");
    for (int v : w) printf(" %02X", v);
    fails++;
  }
  printf("\n");
  k.out.clear();
}

int main() {
  Ps2Keyboard k;
  k.powerOn();                         expect("power on: self-test passed", k, {0xAA});
  k.hostByte(0xFF);                    expect("reset", k, {0xFA, 0xAA});
  k.hostByte(0xF2);                    expect("identify", k, {0xFA, 0xAB, 0x83});
  k.hostByte(0xED); k.hostByte(0x02);  expect("set LEDs (num lock)", k, {0xFA, 0xFA});
  if (k.leds != 2) { printf("FAIL leds\n"); fails++; }
  k.hostByte(0xEE);                    expect("echo", k, {0xEE});
  k.hostByte(0xF3); k.hostByte(0x20);  expect("typematic rate", k, {0xFA, 0xFA});
  k.hostByte(0xF0); k.hostByte(0x00);  expect("which scan code set? (2)", k, {0xFA, 0xFA, 0x02});

  // set 2
  k.key(0x04, 1); k.key(0x04, 0);      expect("set 2: A down/up", k, {0x1C, 0xF0, 0x1C});
  k.key(0x29, 1); k.key(0x29, 0);      expect("set 2: Esc", k, {0x76, 0xF0, 0x76});
  k.key(0x3A, 1);                      expect("set 2: F1", k, {0x05});
  k.key(0x45, 1);                      expect("set 2: F12", k, {0x07});
  k.key(0x49, 1);                      expect("set 2: Insert", k, {0xE0, 0x70});
  k.key(0x52, 1); k.key(0x52, 0);      expect("set 2: Up arrow", k, {0xE0, 0x75, 0xE0, 0xF0, 0x75});
  k.key(0x54, 1);                      expect("set 2: keypad /", k, {0xE0, 0x4A});
  k.key(0x58, 1);                      expect("set 2: keypad Enter", k, {0xE0, 0x5A});
  k.key(0x62, 1);                      expect("set 2: keypad 0", k, {0x70});
  k.key(0x65, 1);                      expect("set 2: Menu key", k, {0xE0, 0x2F});
  k.key(0xE0, 1);                      expect("set 2: left Ctrl", k, {0x14});
  k.key(0xE3, 1);                      expect("set 2: left Windows", k, {0xE0, 0x1F});
  k.key(0xE6, 1);                      expect("set 2: right Alt", k, {0xE0, 0x11});
  k.key(0x16, 1); k.key(0x16, 2); k.key(0x16, 2);
                                       expect("set 2: S held: repeats are makes", k, {0x1B, 0x1B, 0x1B});
  k.key(0x48, 1); k.key(0x48, 0);      expect("set 2: Pause (make only)", k, {0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77});
  k.key(0x04, 0);                      expect("set 2: up for a key not held: nothing", k, {});
  k.releaseAll();                      expect("leave: let go of held keys", k,
                                              {0xF0, 0x1B, 0xF0, 0x05, 0xF0, 0x07, 0xE0, 0xF0, 0x70,
                                               0xE0, 0xF0, 0x4A, 0xE0, 0xF0, 0x5A, 0xF0, 0x70, 0xE0, 0xF0, 0x2F,
                                               0xF0, 0x14, 0xE0, 0xF0, 0x1F, 0xE0, 0xF0, 0x11});

  // SGI: switch to set 3, all keys make/break
  k.hostByte(0xF0); k.hostByte(0x03);  expect("switch to set 3", k, {0xFA, 0xFA});
  k.hostByte(0xF0); k.hostByte(0x00);  expect("which set? (3)", k, {0xFA, 0xFA, 0x03});
  k.hostByte(0xF8);                    expect("set 3: all keys make/break", k, {0xFA});
  k.key(0x07, 1); k.key(0x07, 0);      expect("set 3: D down/up", k, {0x23, 0xF0, 0x23});
  k.key(0x29, 1); k.key(0x29, 0);      expect("set 3: Esc", k, {0x08, 0xF0, 0x08});
  k.key(0xE0, 1); k.key(0xE0, 0);      expect("set 3: left Ctrl", k, {0x11, 0xF0, 0x11});
  k.key(0xE2, 1);                      expect("set 3: left Alt", k, {0x19});
  k.key(0x39, 1);                      expect("set 3: Caps Lock", k, {0x14});
  k.key(0x3A, 1);                      expect("set 3: F1", k, {0x07});
  k.key(0x52, 1);                      expect("set 3: Up arrow", k, {0x63});
  k.key(0x58, 1);                      expect("set 3: keypad Enter", k, {0x79});
  k.key(0x28, 1); k.key(0x28, 2);      expect("set 3 make/break: no repeats", k, {0x5A});
  k.releaseAll();                      expect("set 3 leave", k, {0xF0, 0x5A, 0xF0, 0x14, 0xF0, 0x07, 0xF0, 0x63, 0xF0, 0x79, 0xF0, 0x19});
  k.hostByte(0xF9);                    expect("set 3: all keys make only", k, {0xFA});
  k.key(0x04, 1); k.key(0x04, 0);      expect("set 3 make only: no break", k, {0x1C});
  k.hostByte(0xFC); k.hostByte(0x1C); k.hostByte(0x23); k.hostByte(0xF4);
                                       expect("set 3: per-key list ends at a command", k, {0xFA, 0xFA, 0xFA, 0xFA});
  k.hostByte(0xFF);                    expect("reset goes back to set 2", k, {0xFA, 0xAA});
  if (k.set != 2) { printf("FAIL set after reset\n"); fails++; }
  k.hostByte(0xF5); k.key(0x04, 1);    expect("disabled: no keys", k, {0xFA});
  k.hostByte(0x42);                    expect("unknown command: resend", k, {0xFE});
  printf("%s: %d failure(s)\n", fails ? "FAILED" : "all good", fails);
  return fails != 0;
}
