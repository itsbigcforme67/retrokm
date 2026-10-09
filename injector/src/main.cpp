// RetroKM hardware keyboard: an M5Stack NanoC6 that plays a PS/2 keyboard.
//
// Plug its USB-C into a passive USB-to-PS/2 adapter (the little green ones
// that came with keyboards) and that into a computer's keyboard port.  It
// joins WiFi, connects to the RetroKM hub as a hardware keyboard for one
// screen, and types whatever the hub sends: in the BIOS, at boot menus, at
// an SGI PROM prompt, before any agent could run.
//
// At power-up it looks at the USB pins: a PS/2 port holds them high, a
// USB port (the laptop) holds them low.  On the laptop it stays a normal
// USB device, with a settings console on the serial port:
//   show | name <screen> | hub <host> | swap | reboot
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <soc/usb_serial_jtag_reg.h>
#include "secrets.h"
extern "C" {
#include "rkm_proto.h"
}
#include "ps2kbd.h"
#include "ps2line.h"

static const int USB_DM = 12, USB_DP = 13;  // the USB-C data pins = PS/2 data / clock
static const int LED = 7;                   // NanoC6 blue LED

static Preferences prefs;
static String name, hubHost;
static bool swapPins;

static Ps2Line line;
static Ps2Keyboard kbd;
static SemaphoreHandle_t lock;
static bool ps2Mode;

static WiFiClient hub;
static rkm_parser parser;
static uint32_t lastTry, lastRx;
static bool welcomed;

// ------------------------------------------------------------ USB or PS/2?

// ------------------------------------------------------------ log over WiFi

// In PS/2 mode the USB port is busy being a keyboard, so the log goes to
// the hub's host as UDP (port 24853):  nc -klu 24853
static WiFiUDP logUdp;
static char logBuf[2048];
static int logLen;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

static void logf(const char* fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof line - 2) n = sizeof line - 2;
  line[n++] = '\n';
  portENTER_CRITICAL(&logMux);
  if (logLen + n <= (int)sizeof logBuf) { memcpy(logBuf + logLen, line, n); logLen += n; }
  portEXIT_CRITICAL(&logMux);
}

static void flushLog() {
  static char out[sizeof logBuf];
  int n;
  portENTER_CRITICAL(&logMux);
  n = logLen;
  memcpy(out, logBuf, n);
  logLen = 0;
  portEXIT_CRITICAL(&logMux);
  if (!n) return;
  Serial.write((const uint8_t*)out, n);
  if (WiFi.status() == WL_CONNECTED) {
    logUdp.beginPacket(hubHost.c_str(), 24853);
    logUdp.write((const uint8_t*)out, n);
    logUdp.endPacket();
  }
}

static void usbPads(bool on) {
  if (on) SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  else CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE |
                                                      USB_SERIAL_JTAG_DP_PULLUP);
}

static bool onPs2Port() {
  usbPads(false);
  for (int p : {USB_DM, USB_DP}) {
    gpio_reset_pin((gpio_num_t)p);
    gpio_set_direction((gpio_num_t)p, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)p, GPIO_FLOATING);
  }
  delay(20);
  // A USB port's 15k resistors hold both lines low.  A PS/2 port's pull-ups
  // hold them high, though the computer may be holding the clock low to
  // keep the keyboard quiet while it boots: so either line high = PS/2.
  int highs = 0;
  for (int i = 0; i < 20; i++) {
    highs += gpio_get_level((gpio_num_t)USB_DM) || gpio_get_level((gpio_num_t)USB_DP);
    delay(1);
  }
  return highs >= 15;
}

// ------------------------------------------------------------ PS/2 side

// Runs on its own: answers the computer and sends queued scan codes.
static void ps2Task(void*) {
  line.begin(swapPins ? USB_DM : USB_DP, swapPins ? USB_DP : USB_DM);
  logf("ps2: start, clock=%d data=%d", line.clkHigh(), line.datHigh());
  delay(100);  // a keyboard's self-test takes a moment; then it says AA
  xSemaphoreTake(lock, portMAX_DELAY);
  kbd.powerOn();
  xSemaphoreGive(lock);
  for (;;) {
    bool busy = false;
    if (line.hostWantsToSend()) {
      int b = line.receive();
      xSemaphoreTake(lock, portMAX_DELAY);
      if (b < 0) { kbd.out.push(0xFE); logf("ps2: <- garbled byte"); }  // ask again
      else { kbd.hostByte((uint8_t)b); logf("ps2: <- %02X", b); }
      xSemaphoreGive(lock);
      busy = true;
    } else {
      xSemaphoreTake(lock, portMAX_DELAY);
      int n = kbd.out.n;
      uint8_t next = n ? kbd.out.b[0] : 0;
      xSemaphoreGive(lock);
      if (n) {
        int r = line.send(next);
        if (r == 1) logf("ps2: -> %02X", next);
        else if (r < 0) logf("ps2: -> %02X interrupted", next);
        if (r == 1) {
          xSemaphoreTake(lock, portMAX_DELAY);
          kbd.sent(next);
          memmove(kbd.out.b, kbd.out.b + 1, --kbd.out.n);
          xSemaphoreGive(lock);
        }
        busy = r != 0;
      }
    }
    if (!busy) vTaskDelay(1);  // idle: look again in a millisecond
  }
}

// ------------------------------------------------------------ hub side

static void sendFrame(int type, const unsigned char* p, unsigned len) {
  unsigned char buf[RKM_HDR + RKM_MAX_PAYLOAD];
  unsigned n = rkm_pack(buf, type, p, len);
  if (hub.connected()) hub.write(buf, n);
}

static void onFrame(void*, int type, const unsigned char* p, unsigned len) {
  lastRx = millis();
  xSemaphoreTake(lock, portMAX_DELAY);
  switch (type) {
  case RKM_WELCOME:
    welcomed = len >= 2 && p[1] == RKM_OK;
    if (!welcomed) Serial.printf("hub refused us: %s\n", len >= 2 && p[1] == RKM_ERR_NAME ?
                                 "no such screen name in its config" : "version mismatch");
    break;
  case RKM_PING: {
    xSemaphoreGive(lock);
    sendFrame(RKM_PONG, nullptr, 0);
    return;
  }
  case RKM_ENTER:  // carry modifiers that are held as the pointer arrives
    if (len >= 5)
      for (int b = 0; b < 8; b++)
        if (p[4] & (1 << b)) kbd.key(0xE0 + b, 1);
    break;
  case RKM_LEAVE:
  case RKM_RESET:
    kbd.releaseAll();
    break;
  case RKM_KEY:
    if (len >= 2 && ps2Mode) kbd.key(p[0], p[1]);
    break;
  }
  xSemaphoreGive(lock);
}

static void pollHub() {
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) { welcomed = false; return; }
  if (!hub.connected()) {
    welcomed = false;
    if (now - lastTry < 2000) return;
    lastTry = now;
    if (!hub.connect(hubHost.c_str(), RKM_PORT, 3000)) {
      Serial.printf("cannot reach the hub at %s:%d\n", hubHost.c_str(), RKM_PORT);
      return;
    }
    hub.setNoDelay(true);
    rkm_parser_init(&parser);
    unsigned char hello[10 + RKM_NAME_MAX];
    unsigned n = rkm_hello(hello, RKM_CAP_KEYS, RKM_CS_UTF8, RKM_EOL_LF, 0, 0, 0, name.c_str());
    sendFrame(RKM_HELLO, hello, n);
    lastRx = now;
    logf("hub: connected to %s as \"%s\" (%s mode)", hubHost.c_str(), name.c_str(), ps2Mode ? "PS/2" : "USB");
    return;
  }
  unsigned char buf[512];
  while (hub.available()) {
    int n = hub.read(buf, sizeof buf);
    if (n <= 0) break;
    if (rkm_feed(&parser, buf, n, onFrame, nullptr) < 0) { hub.stop(); return; }
  }
  if (now - lastRx > 30000) {  // the hub pings every few seconds
    Serial.println("hub went quiet, reconnecting");
    hub.stop();
  }
}

// ------------------------------------------------------------ settings

static void loadSettings() {
  prefs.begin("rkm", false);
  name = prefs.getString("name", RKM_DEFAULT_NAME);
  hubHost = prefs.getString("hub", HUB_HOST);
  swapPins = prefs.getBool("swap", false);
}

static void console() {
  static String in;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { in += c; continue; }
    in.trim();
    if (in.startsWith("name ")) { name = in.substring(5); prefs.putString("name", name); }
    else if (in.startsWith("hub ")) { hubHost = in.substring(4); prefs.putString("hub", hubHost); }
    else if (in == "swap") { swapPins = !swapPins; prefs.putBool("swap", swapPins); }
    else if (in == "reboot") ESP.restart();
    if (in.length()) {
      Serial.printf("name=%s hub=%s pins=%s wifi=%s hub link=%s\n", name.c_str(), hubHost.c_str(),
                    swapPins ? "swapped (D-=clock)" : "normal (D+=clock)",
                    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "joining",
                    welcomed ? "up" : "down");
      Serial.println("commands: show | name <screen> | hub <host> | swap | reboot");
    }
    in = "";
  }
}

// ------------------------------------------------------------ main

void setup() {
  // The computer checks its keyboard very early: start the PS/2 side first
  ps2Mode = onPs2Port();
  loadSettings();
  lock = xSemaphoreCreateMutex();
  if (ps2Mode) xTaskCreate(ps2Task, "ps2", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr);
  else usbPads(true);  // on the laptop: be a USB device again
  Serial.begin(115200);
  if (!ps2Mode) Serial.setDebugOutput(true);  // library errors to the console too
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);
  logf("boot: %s mode, name %s", ps2Mode ? "PS/2" : "USB", name.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // power saving would make keys late
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);  // gentle on a PS/2 port's power; plenty across a room

  // updates over WiFi:  pio run -t upload --upload-port <ip>  (env:ota)
  ArduinoOTA.setHostname(("rkm-kbd-" + name).c_str());
  ArduinoOTA.onStart([] { logf("ota: updating"); flushLog(); });
  ArduinoOTA.begin();
}

void loop() {
  static bool announced;
  if (WiFi.status() == WL_CONNECTED && !announced) {
    announced = true;
    logf("wifi: %s", WiFi.localIP().toString().c_str());
  }
  ArduinoOTA.handle();
  flushLog();
  pollHub();
  if (!ps2Mode) console();
  // LED: steady when the hub knows us, blinking while joining
  digitalWrite(LED, welcomed ? HIGH : (millis() / 250) % 2);
  delay(2);
}
