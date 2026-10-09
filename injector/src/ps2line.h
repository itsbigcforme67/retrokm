// The wires of a PS/2 port, driven from the device end.
//
// Clock and data are open collector: either side pulls a line low, and the
// computer's resistors pull it up to 5 V otherwise.  We only ever drive low
// or let go (open drain), so we never push 3.3 V against the computer.  The
// device makes the clock (about 12 kHz); the computer can hold the clock low
// to hold us off, or pull data low to say it has a command for us.
#pragma once
#include <Arduino.h>
#include <driver/gpio.h>
#include <rom/ets_sys.h>

class Ps2Line {
 public:
  void begin(int clk, int dat) {
    clk_ = (gpio_num_t)clk;
    dat_ = (gpio_num_t)dat;
    for (gpio_num_t p : {clk_, dat_}) {
      gpio_reset_pin(p);
      gpio_set_level(p, 1);  // let go
      gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT_OD);
      gpio_set_pull_mode(p, GPIO_FLOATING);  // the computer pulls up
    }
  }

  bool clkHigh() const { return gpio_get_level(clk_); }
  bool datHigh() const { return gpio_get_level(dat_); }
  // The computer has a command for us: clock let go, data held low
  bool hostWantsToSend() const { return clkHigh() && !datHigh(); }

  // Send one byte.  1 sent, 0 the computer is holding us off (try again
  // later), -1 it interrupted us mid-byte (send it again).
  int send(uint8_t b) {
    if (!clkHigh() || !datHigh()) return 0;
    ets_delay_us(50);  // the clock must be idle a moment before we start
    if (!clkHigh() || !datHigh()) return 0;
    uint8_t parity = 1;
    for (int i = 0; i < 8; i++) parity ^= (b >> i) & 1;
    int r = 1;
    portENTER_CRITICAL(&mux_);
    for (int i = 0; i < 11; i++) {
      int bit = i == 0 ? 0 : i <= 8 ? (b >> (i - 1)) & 1 : i == 9 ? parity : 1;
      gpio_set_level(dat_, bit);
      ets_delay_us(HALF / 2);
      gpio_set_level(clk_, 0);
      ets_delay_us(HALF);
      gpio_set_level(clk_, 1);
      ets_delay_us(HALF / 2);
      if (i < 10 && !clkHigh()) { r = -1; break; }  // computer took the clock back
    }
    gpio_set_level(dat_, 1);
    portEXIT_CRITICAL(&mux_);
    ets_delay_us(60);
    return r;
  }

  // Read the command the computer is sending (call when hostWantsToSend).
  // The byte, or -1 on a parity error.  bits gets what was seen, for the log.
  int receive(uint16_t* bits = nullptr) {
    int v = 0, ones = 0, ok = 1;
    uint16_t seen = 0;
    portENTER_CRITICAL(&mux_);
    // Some computers (the Octane) take a while after letting go of the clock
    // before they are ready to shift bits out; the spec allows us 15 ms
    ets_delay_us(RX_WAIT);
    for (int i = 0; i < 10; i++) {  // 8 data bits, parity, stop
      gpio_set_level(clk_, 0);  // the computer changes data while the clock is low
      ets_delay_us(RX_HALF);
      gpio_set_level(clk_, 1);
      ets_delay_us(RX_HALF - 8);  // read late in the high half: slow hosts change late
      int bit = datHigh();
      seen |= bit << i;
      if (i < 8) v |= bit << i;
      if (i < 9) ones += bit;
      ets_delay_us(8);
    }
    // acknowledge: hold data low for one more clock
    gpio_set_level(dat_, 0);
    ets_delay_us(HALF / 2);
    gpio_set_level(clk_, 0);
    ets_delay_us(HALF);
    gpio_set_level(clk_, 1);
    ets_delay_us(HALF / 2);
    gpio_set_level(dat_, 1);
    portEXIT_CRITICAL(&mux_);
    if (bits) *bits = seen;
    if (!(ones & 1)) ok = 0;  // odd parity over data + parity bit (a bad stop bit is let pass)
    return ok ? v : -1;
  }

 private:
  static const int HALF = 40;     // µs: clock low and high are 30-50 µs each
  static const int RX_HALF = 48;  // slower while the computer is sending to us
  static const int RX_WAIT = 1000;
  gpio_num_t clk_, dat_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
