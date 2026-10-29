// RetroKM Xbox pad: an M5Stack Atom S3R that plays an original Xbox
// controller on a console's pad port.
//
// Plug its USB-C into an Xbox-to-USB adapter on the Xbox. It joins WiFi,
// connects to the RetroKM hub as the pad player for one screen ("xbox"), and
// while that screen has the keyboard it plays the hub's game pad: the Elite
// on the laptop drives the Xbox the way the keyboard drives the Octane.
// The game's rumble goes back to the hub, which shakes the real pad.
//
// Log: UDP to the hub's host, port 24853 (python recvfrom, or nc -klu 24853).
// Settings: UDP port 24854 (and USB serial in pc mode):
//   show | name <screen> | label <board name> | hub <host> | player 1-4 | mode pc|xbox | reboot
// (player: which of the console's pad ports this is; one dongle per port)
#include "dongle.h"
#include "servo.h"
#include <errno.h>
#include <ArduinoOTA.h>
#include <HWCDC.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include "secrets.h"
extern "C" {
#include "rkm_proto.h"
}

UsbMode usb_mode = MODE_XBOX;

static Preferences prefs;
static String name, hubHost, label;
static int player = 1;  // which player on its screen this dongle plays
static HWCDC usbSerial;  // pc mode only: in xbox mode the port is the controller

// ------------------------------------------------------------ log over WiFi

static WiFiUDP logUdp;
static char logBuf[4096];
static int logLen;

static void logf(const char *fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof line - 2) n = sizeof line - 2;
  line[n++] = '\n';
  if (logLen + n <= (int)sizeof logBuf) { memcpy(logBuf + logLen, line, n); logLen += n; }
}

static void flushLog() {
  if (!logLen) return;
  if (usb_mode == MODE_PC) usbSerial.write((const uint8_t *)logBuf, logLen);
  if (WiFi.status() == WL_CONNECTED) {
    logUdp.beginPacket(hubHost.c_str(), 24853);
    logUdp.write((const uint8_t *)logBuf, logLen);
    logUdp.endPacket();
  } else if (usb_mode == MODE_XBOX) {
    return;  // keep them for when it is
  }
  logLen = 0;
}

// ------------------------------------------------------------ the pad

static xid_input_t current;
static bool hasPad;  // between ENTER and LEAVE
static uint32_t rumbleSeq;
static xid_rumble_t rumbleNow;

const xid_input_t &current_input() { return current; }

static void setInput(const xid_input_t &in) {
  current = in;
  xid_set_input(in);
}

static void neutral() {
  xid_input_t idle = {};
  setInput(idle);
}

// ------------------------------------------------------------ hub side

// A plain non-blocking lwIP socket, as in the PS/2 keyboard: WiFiClient's
// connected()/available() misjudged a healthy link as dead (stale errno).
static int hubFd = -1;
static bool hubUp, welcomed;
static rkm_parser parser;
static uint32_t lastTry, lastRx;

static void hubClose(const char *why) {
  if (hubFd >= 0) {
    close(hubFd);
    if (why) logf("hub: %s", why);
  }
  hubFd = -1;
  hubUp = welcomed = hasPad = false;
  neutral();
}

static void sendFrame(int type, const unsigned char *p, unsigned len) {
  unsigned char buf[RKM_HDR + RKM_MAX_PAYLOAD];
  unsigned n = rkm_pack(buf, type, p, len);
  if (!hubUp) return;
  for (unsigned off = 0; off < n;) {
    int k = send(hubFd, buf + off, n - off, 0);
    if (k > 0) { off += k; continue; }
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { delay(1); continue; }
    hubClose("send failed, reconnecting");
    return;
  }
}

static void onFrame(void *, int type, const unsigned char *p, unsigned len) {
  lastRx = millis();
  switch (type) {
  case RKM_WELCOME:
    welcomed = len >= 2 && p[1] == RKM_OK;
    if (!welcomed)
      logf("hub refused us: %s", len >= 2 && p[1] == RKM_ERR_NAME
                                     ? "no such screen name in its config" : "version mismatch");
    break;
  case RKM_PING:
    sendFrame(RKM_PONG, nullptr, 0);
    break;
  case RKM_ENTER:
    hasPad = true;
    neutral();
    logf("pad: ours");
    break;
  case RKM_LEAVE:
  case RKM_RESET:
    if (hasPad) logf("pad: gone elsewhere");
    hasPad = false;
    neutral();
    break;
  case RKM_PAD:
    if (len >= RKM_PAD_LEN && p[0] == player - 1 && hasPad) {
      xid_input_t in;
      xid_from_rkm(RKM_GET16(p + 1), p[3], p[4], (int16_t)RKM_GET16(p + 5),
                   (int16_t)RKM_GET16(p + 7), (int16_t)RKM_GET16(p + 9),
                   (int16_t)RKM_GET16(p + 11), &in);
      setInput(in);
    }
    break;
  }
}

static void pollHub() {
  uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (hubFd >= 0) hubClose("WiFi lost");
    return;
  }
  if (hubFd < 0) {  // start connecting
    if (now - lastTry < 2000) return;
    lastTry = now;
    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(RKM_PORT);
    if (inet_pton(AF_INET, hubHost.c_str(), &sa.sin_addr) != 1) {
      hostent *he = gethostbyname(hubHost.c_str());
      if (!he) return;
      memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
    }
    hubFd = socket(AF_INET, SOCK_STREAM, 0);
    if (hubFd < 0) return;
    int one = 1;
    setsockopt(hubFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(hubFd, F_SETFL, fcntl(hubFd, F_GETFL, 0) | O_NONBLOCK);
    if (connect(hubFd, (sockaddr *)&sa, sizeof sa) < 0 && errno != EINPROGRESS) hubClose(nullptr);
    return;
  }
  if (!hubUp) {  // connecting
    fd_set w;
    FD_ZERO(&w);
    FD_SET(hubFd, &w);
    timeval tv = {0, 0};
    if (select(hubFd + 1, nullptr, &w, nullptr, &tv) > 0) {
      int err = 0;
      socklen_t len = sizeof err;
      getsockopt(hubFd, SOL_SOCKET, SO_ERROR, &err, &len);
      if (err) { hubClose(nullptr); return; }
      hubUp = true;
      rkm_parser_init(&parser);
      unsigned char hello[10 + RKM_NAME_MAX];
      unsigned n = rkm_hello(hello, RKM_CAP_PAD, RKM_CS_UTF8, RKM_EOL_LF, 0, 0, 0, name.c_str());
      sendFrame(RKM_HELLO, hello, n);
      lastRx = now;
      logf("hub: connected to %s as \"%s\"", hubHost.c_str(), name.c_str());
    } else if (now - lastTry > 4000) {
      hubClose(nullptr);
    }
    return;
  }
  unsigned char buf[512];
  for (;;) {
    int n = recv(hubFd, buf, sizeof buf, MSG_DONTWAIT);
    if (n > 0) {
      if (rkm_feed(&parser, buf, n, onFrame, nullptr) < 0) { hubClose("bad frame, reconnecting"); return; }
      continue;
    }
    if (n == 0) { hubClose("hub closed the link, reconnecting"); return; }
    if (errno != EAGAIN && errno != EWOULDBLOCK) { hubClose("link error, reconnecting"); return; }
    break;
  }
  // (millis() again: a frame read just now moved lastRx past `now`)
  if (millis() - lastRx > 30000) hubClose("hub went quiet, reconnecting");  // it pings every 5 s
}

// ------------------------------------------------------------ USB events and rumble

static void watchUsb() {
  if (usb_mode != MODE_XBOX) return;
  xid_rumble_t r;
  if (xid_get_rumble(&r, &rumbleSeq) &&
      (r.left != rumbleNow.left || r.right != rumbleNow.right)) {
    rumbleNow = r;
    unsigned char p[5];
    p[0] = (unsigned char)(player - 1);
    RKM_PUT16(p + 1, r.left);   // the big motor
    RKM_PUT16(p + 3, r.right);  // the small one
    sendFrame(RKM_RUMBLE, p, 5);
    logf("rumble %u %u", r.left, r.right);
  }
  static XidStats last;
  XidStats s = xid_stats();
  if (s.mounted != last.mounted)
    logf("usb: %s (xid requests %u, resets %u)", s.mounted ? "the console took the pad" : "unplugged / reset",
         (unsigned)s.xid_reqs, (unsigned)s.resets);
  last = s;
}

// ------------------------------------------------------------ settings

static void loadSettings() {
  prefs.begin("rkm", false);
  name = prefs.getString("name", RKM_DEFAULT_NAME);
  hubHost = prefs.getString("hub", HUB_HOST);
  label = prefs.isKey("label") ? prefs.getString("label") : name + "pad";
  usb_mode = (UsbMode)prefs.getUChar("mode", MODE_XBOX);
  player = constrain(prefs.getUChar("player", 1), 1, 4);
}

static void command(String in, Print &reply) {
  in.trim();
  if (servo_command(in, reply)) return;
  if (in.startsWith("name ")) { name = in.substring(5); prefs.putString("name", name); hubClose("renamed"); }
  else if (in.startsWith("hub ")) { hubHost = in.substring(4); prefs.putString("hub", hubHost); hubClose("new hub"); }
  else if (in.startsWith("label ")) { label = in.substring(6); prefs.putString("label", label); }
  else if (in.startsWith("player ")) {
    int n = in.substring(7).toInt();
    if (n >= 1 && n <= 4) { player = n; prefs.putUChar("player", n); neutral(); }
  }
  else if (in == "mode pc" || in == "mode xbox") {
    UsbMode m = in == "mode pc" ? MODE_PC : MODE_XBOX;
    prefs.putUChar("mode", m);
    reply.printf("mode %s, rebooting\n", m == MODE_PC ? "pc" : "xbox");
    flushLog();
    delay(200);
    if (usb_mode == MODE_XBOX) xid_switch_to_pc_and_restart();
    ESP.restart();
  }
  else if (in == "reboot") { reply.println("rebooting"); delay(100); ESP.restart(); }
  if (!in.length()) return;
  XidStats s = xid_stats();
  reply.printf("label=%s name=%s player=%d hub=%s mode=%s wifi=%s hub link=%s pad=%s usb=%s "
               "reports=%u xid_reqs=%u rumble=%u,%u\n",
               label.c_str(), name.c_str(), player, hubHost.c_str(), usb_mode == MODE_PC ? "pc" : "xbox",
               WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "joining",
               welcomed ? "up" : "down", hasPad ? "ours" : "elsewhere",
               s.mounted ? "taken" : "idle", (unsigned)s.reports, (unsigned)s.xid_reqs,
               rumbleNow.left, rumbleNow.right);
  reply.println("commands: show | name <screen> | label <board name> | hub <host> | player 1-4 | mode pc|xbox | reboot"
                " | i2c | servo <port> <deg> | servo stop|push|back <deg> | servo backms <ms> | press [ms]");
}

static WiFiUDP ctlUdp;  // echo show | nc -u -w1 <ip> 24854

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
  ctlUdp.write((const uint8_t *)r.text.c_str(), r.text.length());
  ctlUdp.endPacket();
}

static void console() {
  static String in;
  while (usbSerial.available()) {
    char c = usbSerial.read();
    if (c != '\n' && c != '\r') { in += c; continue; }
    command(in, usbSerial);
    in = "";
  }
}

// ------------------------------------------------------------ main

void setup() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  delay(5);
  loadSettings();
  if (!digitalRead(PIN_BUTTON)) usb_mode = MODE_PC;  // held while plugging in
  if (usb_mode == MODE_XBOX) xid_start();  // first, so the console sees the pad at once
  else { usbSerial.begin(); usbSerial.setTxTimeoutMs(0); }
  screen_start();
  logf("boot: %s (%s), %s mode, screen %s player %d", label.c_str(), WiFi.macAddress().c_str(),
       usb_mode == MODE_PC ? "pc" : "xbox", name.c_str(), player);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // power saving would make the pad late
  WiFi.setHostname(label.c_str());
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);  // powered from the console's port

  // updates over WiFi:  ./update.sh xbox
  ArduinoOTA.setHostname(label.c_str());  // reachable as <label>.local
  ArduinoOTA.onStart([] { logf("ota: updating"); flushLog(); });
  ArduinoOTA.begin();
  ctlUdp.begin(24854);
}

void loop() {
  static bool announced;
  if (WiFi.status() == WL_CONNECTED && !announced) {
    announced = true;
    logf("wifi: %s", WiFi.localIP().toString().c_str());
  }
  ArduinoOTA.handle();
  networkCommands();
  if (usb_mode == MODE_PC) console();
  pollHub();
  watchUsb();
  servo_loop();
  flushLog();
  Status st = {label.c_str(), name.c_str(),
               WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("joining WiFi"),
               welcomed, hasPad};
  screen_loop(st);
  delay(1);
}
