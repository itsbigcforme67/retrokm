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
//   show | name <screen> | label <board name> | hub <host> | swap | reboot
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <soc/usb_serial_jtag_reg.h>
#include <esp_timer.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_reg.h>
#include "secrets.h"
extern "C" {
#include "rkm_proto.h"
}
#include "ps2kbd.h"
#include "ps2line.h"

static const int USB_DM = 12, USB_DP = 13;  // the USB-C data pins = PS/2 data / clock
static const int LED = 7;                   // NanoC6 blue LED

static Preferences prefs;
static String name, hubHost, label;  // label: this board's own name (octanekb.local)
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
static char logBuf[8192];
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
  if (WiFi.status() != WL_CONNECTED && ps2Mode) return;  // keep them for when it is
  portENTER_CRITICAL(&logMux);
  n = logLen;
  memcpy(out, logBuf, n);
  logLen = 0;
  portEXIT_CRITICAL(&logMux);
  if (!n) return;
  Serial.write((const uint8_t*)out, n);
  if (WiFi.status() == WL_CONNECTED) {
    for (int off = 0; off < n;) {  // packets that fit the network (no fragments)
      int k = n - off > 1200 ? 1200 : n - off;
      while (k < n - off && k > 0 && out[off + k - 1] != '\n') k--;  // end on a line
      if (k <= 0) k = n - off > 1200 ? 1200 : n - off;
      logUdp.beginPacket(hubHost.c_str(), 24853);
      logUdp.write((const uint8_t*)out + off, k);
      logUdp.endPacket();
      off += k;
      delay(2);
    }
  }
}

static void usbPads(bool on) {
  if (on) SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  else CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG,
                           USB_SERIAL_JTAG_USB_PAD_ENABLE | USB_SERIAL_JTAG_DP_PULLUP |
                           USB_SERIAL_JTAG_DM_PULLUP | USB_SERIAL_JTAG_DP_PULLDOWN |
                           USB_SERIAL_JTAG_DM_PULLDOWN | USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
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

// ------------------------------------------------------------ wire trace

// Records every change on the two lines with a timestamp, so the log shows
// what the computer is really doing.  "trace" (console or network) arms it
// for 20 s; it is also armed for the first 60 s after power-up.
struct Edge { uint32_t us; uint8_t dm, dp; };
static Edge edges[600];
static volatile int nEdges;
static volatile uint32_t traceUntil;
static TaskHandle_t ps2TaskHandle;

static void IRAM_ATTR onEdge(void*) {
  int64_t t = esp_timer_get_time();
  if ((int32_t)(traceUntil - (uint32_t)(t / 1000)) > 0 && nEdges < (int)(sizeof edges / sizeof edges[0])) {
    Edge& e = edges[nEdges];
    e.us = (uint32_t)t;
    e.dm = gpio_get_level((gpio_num_t)USB_DM);
    e.dp = gpio_get_level((gpio_num_t)USB_DP);
    nEdges = nEdges + 1;
  }
  BaseType_t woke = pdFALSE;  // the computer is doing something: look now
  if (ps2TaskHandle) vTaskNotifyGiveFromISR(ps2TaskHandle, &woke);
  portYIELD_FROM_ISR(woke);
}

static void traceArm(uint32_t ms) {
  nEdges = 0;
  traceUntil = (uint32_t)(esp_timer_get_time() / 1000) + ms;
}

// Called from loop(): print what was captured, a line per change
static void traceDump() {
  static int shown;
  static uint32_t base;
  int n = nEdges;
  if (n < shown) shown = 0;  // re-armed
  // eight changes a line: time since the first change (ms) : D- D+
  while (n - shown >= 8 || (n > shown && (int32_t)(traceUntil - (uint32_t)(esp_timer_get_time() / 1000)) <= 0)) {
    char line[200];
    int len = snprintf(line, sizeof line, "wire:");
    for (int k = 0; k < 8 && shown < n; k++, shown++) {
      const Edge& e = edges[shown];
      if (shown == 0) base = e.us;
      len += snprintf(line + len, sizeof line - len, " %.3f:%d%d", (e.us - base) / 1000.0, e.dm, e.dp);
    }
    logf("%s", line);
  }
}

// ------------------------------------------------------------ PS/2 side

// Runs on its own: answers the computer and sends queued scan codes.
static void ps2Task(void*) {
  // Passive adapters wire USB D+ to PS/2 clock and D- to data (the wire
  // trace on the Octane showed it); "swap" is for one done the other way
  line.begin(swapPins ? USB_DM : USB_DP, swapPins ? USB_DP : USB_DM);
  usbPads(false);  // the USB hardware must not own these pins (and no 1.5k pull-up on D+)
  ps2TaskHandle = xTaskGetCurrentTaskHandle();
  gpio_install_isr_service(0);
  for (int p : {USB_DM, USB_DP}) {
    gpio_set_intr_type((gpio_num_t)p, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add((gpio_num_t)p, onEdge, nullptr);
  }
  logf("ps2: start, clock=%d data=%d", line.clkHigh(), line.datHigh());
  delay(100);  // a keyboard's self-test takes a moment; then it says AA
  xSemaphoreTake(lock, portMAX_DELAY);
  kbd.powerOn();
  xSemaphoreGive(lock);
  for (;;) {
    bool busy = false;
    if (line.hostWantsToSend()) {
      uint16_t bits = 0;
      int b = line.receive(&bits);
      static uint32_t bad;
      if (b < 0 && ++bad <= 6) logf("ps2: <- bad frame, bits seen %03X (data, parity, stop)", bits);
      xSemaphoreTake(lock, portMAX_DELAY);
      static uint32_t garbled, lastNote;
      if (b < 0) {  // ask again; note it, but not a thousand times a second
        kbd.out.push(0xFE);
        if (garbled++ == 0 || millis() - lastNote > 3000) {
          logf("ps2: <- garbled byte (%u so far)", (unsigned)garbled);
          lastNote = millis();
        }
      }
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
        if (r == 1 && next != 0xFE) logf("ps2: -> %02X", next);
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
    if (!busy) ulTaskNotifyTake(pdTRUE, 1);  // idle: until a line moves, or a tick
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
  label = prefs.isKey("label") ? prefs.getString("label") : name + "kb";
}

static WiFiUDP ctlUdp;  // the same commands over the network: echo swap | nc -u -w1 <ip> 24854

static void command(String in, Print& reply) {
    in.trim();
    if (in.startsWith("name ")) { name = in.substring(5); prefs.putString("name", name); }
    else if (in.startsWith("hub ")) { hubHost = in.substring(4); prefs.putString("hub", hubHost); }
    else if (in.startsWith("label ")) { label = in.substring(6); prefs.putString("label", label); }
    else if (in == "swap") { swapPins = !swapPins; prefs.putBool("swap", swapPins); }
    else if (in == "drive") {  // can we pull each line low?  (1 ms each; harmless)
      int r[2][2];
      for (int k = 0; k < 2; k++) {
        gpio_num_t p = (gpio_num_t)(k ? USB_DP : USB_DM);
        r[k][0] = gpio_get_level(p);
        gpio_set_level(p, 0);
        ets_delay_us(200);
        r[k][1] = gpio_get_level(p);
        gpio_set_level(p, 1);
        ets_delay_us(200);
      }
      reply.printf("drive: D- idle=%d pulled=%d | D+ idle=%d pulled=%d  (pulled should read 0)\n",
                   r[0][0], r[0][1], r[1][0], r[1][1]);
      reply.printf("pads: usb_conf0=%08lx iomux12=%08lx iomux13=%08lx out_en=%08lx out=%08lx in=%08lx\n",
                   (unsigned long)REG_READ(USB_SERIAL_JTAG_CONF0_REG),
                   (unsigned long)REG_READ(IO_MUX_GPIO12_REG), (unsigned long)REG_READ(IO_MUX_GPIO13_REG),
                   (unsigned long)REG_READ(GPIO_ENABLE_REG), (unsigned long)REG_READ(GPIO_OUT_REG),
                   (unsigned long)REG_READ(GPIO_IN_REG));
      reply.printf("pin12: %08lx pin13: %08lx\n", (unsigned long)REG_READ(GPIO_PIN12_REG),
                   (unsigned long)REG_READ(GPIO_PIN13_REG));
    }
    else if (in == "trace") { traceArm(120000); reply.println("tracing the lines for 2 minutes"); }
    else if (in == "reboot") { reply.println("rebooting"); delay(100); ESP.restart(); }
    if (in.length()) {
      reply.printf("label=%s name=%s hub=%s pins=%s wifi=%s hub link=%s\n", label.c_str(), name.c_str(), hubHost.c_str(),
                    swapPins ? "swapped (D-=clock)" : "normal (D+=clock)",
                    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "joining",
                    welcomed ? "up" : "down");
      reply.println("commands: show | name <screen> | label <board name> | hub <host> | swap | trace | reboot");
    }
}

static void console() {
  static String in;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { in += c; continue; }
    command(in, Serial);
    in = "";
  }
}

// Collects a reply and sends it back to whoever asked, as one packet
struct UdpReply : public Print {
  String text;
  size_t write(uint8_t c) override { text += (char)c; return 1; }
};

static void networkCommands() {
  int n = ctlUdp.parsePacket();
  if (n <= 0) return;
  char buf[128];
  n = ctlUdp.read(buf, sizeof buf - 1);
  buf[n < 0 ? 0 : n] = 0;
  UdpReply r;
  logf("ctl: %s", buf);
  command(String(buf), r);
  ctlUdp.beginPacket(ctlUdp.remoteIP(), ctlUdp.remotePort());
  ctlUdp.write((const uint8_t*)r.text.c_str(), r.text.length());
  ctlUdp.endPacket();
}

// ------------------------------------------------------------ main

void setup() {
  // The computer checks its keyboard very early: start the PS/2 side first
  traceArm(60000);
  ps2Mode = onPs2Port();
  loadSettings();
  lock = xSemaphoreCreateMutex();
  if (ps2Mode) xTaskCreate(ps2Task, "ps2", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr);
  else usbPads(true);  // on the laptop: be a USB device again
  if (!ps2Mode) {
    Serial.begin(115200);  // (this switches the USB pads back on: only on the laptop)
    Serial.setDebugOutput(true);  // library errors to the console too
  }
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LOW);
  logf("boot: %s (%s), %s mode, screen %s", label.c_str(), WiFi.macAddress().c_str(),
       ps2Mode ? "PS/2" : "USB", name.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // power saving would make keys late
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);  // gentle on a PS/2 port's power; plenty across a room

  // updates over WiFi:  pio run -t upload --upload-port <ip>  (env:ota)
  ArduinoOTA.setHostname(label.c_str());  // reachable as <label>.local
  WiFi.setHostname(label.c_str());
  ArduinoOTA.onStart([] { logf("ota: updating"); flushLog(); });
  ArduinoOTA.begin();
  ctlUdp.begin(24854);
}

void loop() {
  static bool announced;
  if (ps2Mode && (REG_READ(USB_SERIAL_JTAG_CONF0_REG) & USB_SERIAL_JTAG_USB_PAD_ENABLE)) {
    usbPads(false);
    logf("usb: pads were switched back on; switched them off again");
  }
  if (WiFi.status() == WL_CONNECTED && !announced) {
    announced = true;
    logf("wifi: %s", WiFi.localIP().toString().c_str());
  }
  ArduinoOTA.handle();
  networkCommands();
  traceDump();
  flushLog();
  pollHub();
  if (!ps2Mode) console();
  // LED: steady when the hub knows us, blinking while joining
  digitalWrite(LED, welcomed ? HIGH : (millis() / 250) % 2);
  delay(2);
}
