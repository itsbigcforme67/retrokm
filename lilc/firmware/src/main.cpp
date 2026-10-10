// lil' C: a Stack-chan that talks to Claude through the bridge on your PC.
//
// There is one lil' C. He lives on the Stack-chan and the Tab5 and wanders to
// the desk's computers; the bridge keeps track of where he is. When he is
// somewhere else this screen shows an empty desk; tap it to call him back.
//
// Face screen: hold anywhere on the face to talk, let go to send.
//              Tap the bottom bar to see your Claude Code tasks (and, on the
//              Tab5, the RetroKM desk map).
// Reply:       drag the caption to scroll, tap the face to go back.
// Tasks:       drag to scroll, tap the header to go back.
#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include "config.h"
#include "head.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.h.example to include/secrets.h and fill it in"
#endif
#include <ArduinoOTA.h>  // updates over WiFi (pio run -e tab5-ota / cores3-ota)
#if HAS_KVM
#include "rkm_panel.h"
#ifndef HUB_HOST
#define HUB_HOST BRIDGE_HOST  // the RetroKM hub usually runs on the bridge's PC
#endif
#ifndef HUB_PORT
#define HUB_PORT 24852
#endif
#endif

static const int W = 320, H = 240;
static const int REC_RATE = 16000;
static const int REC_MAX = REC_RATE * MAX_RECORD_SECONDS;
static const int REC_CHUNK = 320;  // 20 ms

static M5Canvas canvas(&M5.Display);  // the 320x240 UI (scaled up on Tab5)
static Head head;

// Touch in UI coordinates (the UI may be scaled up on a bigger screen).
// Touches that start outside the UI (e.g. on the Tab5 panel) are ignored.
static m5::touch_detail_t uiTouch() {
  auto t = M5.Touch.getDetail();
#if UI_SCALE > 1
  if (t.base_x >= UI_X + W * UI_SCALE || t.base_y >= UI_Y + H * UI_SCALE)
    return m5::touch_detail_t();
  auto f = [](int16_t& x, int16_t& y) {
    x = (x - UI_X) / UI_SCALE;
    y = (y - UI_Y) / UI_SCALE;
  };
  f(t.x, t.y);
  f(t.base_x, t.base_y);
  f(t.prev_x, t.prev_y);
#endif
  return t;
}

// ------------------------------------------------------------ shared state
// The network task (core 0) and the UI loop (core 1) share these.

enum Mode { IDLE, LISTENING, THINKING, SPEAKING, REPLY, TASKS, MEDIA, VIEW, VIDEO, HOP,
            AWAY,   // lil' C is on another device or visiting a computer
            KVM };  // Tab5: the RetroKM desk map has the screen
static volatile Mode mode = IDLE;

struct Task {
  String title, status, detail;
  int ago;
};
static SemaphoreHandle_t lock;
static std::vector<Task> tasks;
static int needsCount = 0, workingCount = 0;
static bool bridgeOk = false, bridgeStt = false;
static String heard, reply, notice;
static String bridgeMediaNewest;  // newest picture id the bridge knows

static int16_t* recBuf;
static volatile int recLen = 0;         // samples
static volatile bool sendPending = false;
static volatile bool tasksWanted = true;
// Spoken answer, one chunk per sentence, filled in by the network task as the
// bridge makes them; the UI plays them in order.
struct Chunk {
  int16_t* buf;
  int len, rate;
  int start, end;  // where the sentence sits in the reply text
};
static const int MAX_CHUNKS = 24;
static Chunk chunks[MAX_CHUNKS];
static volatile int chunkCount = 0;      // fetched so far (network task)
static int chunkPlay = -1;               // playing now (UI)
static volatile bool replyDone = false;  // bridge finished the answer
static volatile bool cancelJob = false;
// "Hopping over" to check on a computer: where it is and what it's called
static volatile bool hopActive = false;
// Directions come from the bridge's desk model (bridge/lilc_place.py): yaw
// and pitch for a neck (+ yaw = towards the user's left), dir x/y (-1..1,
// screen right/up) for the Tab5, whose "neck" is its window on the desktop.
struct Aim {
  float yaw = 0, pitch = 0, dx = 0, dy = 0;
  void read(JsonVariantConst v, const char* prefix = "") {
    String p(prefix);
    yaw = v[p + "yaw"] | 0.0f;
    pitch = v[p + "pitch"] | 0.0f;
    dx = v[p + "dir_x"] | 0.0f;
    dy = v[p + "dir_y"] | 0.0f;
  }
};
static Aim hopAim;
static String hopLabel;
static uint32_t playStart = 0;
// Where lil' C is. There is only one of him; the bridge keeps track.
static volatile bool presKnown = false;  // the bridge answers /api/presence
static volatile bool presHere = true;    // he is on this device
static String presLabel, presKind;       // where he is ("the Tab5"), home/machine
static Aim presAim;                      // which way to look to see him there
static Aim tasksAim;                     // ... and where the Claude sessions run
static volatile int presSeq = -1;        // bumps each time he moves
static volatile uint32_t presArriveAt = 0;  // on his way here: when he gets here
static String presFrom;                  // ... and where he is coming from
static volatile bool summonWanted = false;
static volatile int summonSeq = -1;
static volatile uint32_t summonAt = 0;
// Recording both microphones for the bridge, which listens for where things
// are (bridge/lilc_locate.py): the request (net task), then the recording (UI)
static volatile int recReqId = 0, recReqMs = 0, recReqRate = 48000;
static volatile int stId = 0;            // the recording in hand (0: none)
static int16_t* stBuf = nullptr;
static int stLen = 0, stTotal = 0, stRate = 48000;
static volatile bool stStarted = false, stUpload = false;
static int stLastId = 0;
static float stYaw = 0, stPitch = 0;     // where the head (and its mics) pointed

// ------------------------------------------------------------ network

static bool httpBegin(HTTPClient& http, const String& path) {
  String url = String("http://") + BRIDGE_HOST + ":" + BRIDGE_PORT + path;
  if (!http.begin(url)) return false;
  http.addHeader("X-LilC-Key", BRIDGE_KEY);
  http.addHeader("X-LilC-Dev", BOARD_DEV);  // which of his homes is asking
  http.setTimeout(15000);
  return true;
}

static bool getJson(const String& path, JsonDocument& doc) {
  HTTPClient http;
  if (!httpBegin(http, path)) return false;
  int code = http.GET();
  bool ok = code == 200 && !deserializeJson(doc, http.getStream());
  http.end();
  return ok;
}

static void fetchTasks() {
  JsonDocument doc;
  if (!getJson("/api/ping", doc)) {
    bridgeOk = false;
    return;
  }
  bridgeOk = true;
  bridgeStt = doc["stt"] | false;
  doc.clear();
  if (!getJson("/api/tasks", doc)) return;
  std::vector<Task> fresh;
  for (JsonObject t : doc["tasks"].as<JsonArray>())
    fresh.push_back({t["title"] | "", t["status"] | "", t["detail"] | "",
                     t["ago"] | 0});
  xSemaphoreTake(lock, portMAX_DELAY);
  tasks.swap(fresh);
  needsCount = doc["needs"] | 0;
  bridgeMediaNewest = (const char*)(doc["media_newest"] | "");
  workingCount = doc["working"] | 0;
  xSemaphoreGive(lock);
}

// Where is lil' C? An older bridge doesn't know: then he's always home.
static void fetchPresence() {
  HTTPClient http;
  if (!httpBegin(http, "/api/presence")) return;
  http.setTimeout(3000);
  int code = http.GET();
  JsonDocument doc;
  bool ok = code == 200 && !deserializeJson(doc, http.getStream());
  http.end();
  if (code == 404) {
    presKnown = false;
    presHere = true;
    return;
  }
  if (!ok) return;
  int seq = doc["seq"] | 0;
  bool here = doc["here"] | true;
  JsonVariantConst rec = doc["record"];
  if (!rec.isNull()) {  // the bridge wants both microphones for a moment
    int id = rec["id"] | 0;
    if (id && id != stLastId && id != stId && !recReqId) {
      recReqMs = constrain((int)(rec["ms"] | 4000), 500, 8000);
      recReqRate = rec["rate"] | 48000;
      recReqId = id;
    }
  }
  // Just called him over: an answer from before the bridge moved him is stale
  if (!here && seq == summonSeq && millis() - summonAt < 4000) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  presLabel = (const char*)(doc["label"] | "");
  presKind = (const char*)(doc["kind"] | "");
  presAim.read(doc.as<JsonVariantConst>());
  tasksAim.read(doc.as<JsonVariantConst>(), "tasks_");
  presFrom = (const char*)(doc["from"] | "");
  xSemaphoreGive(lock);
  if (here) presArriveAt = millis() + (uint32_t)(doc["eta_ms"] | 0);
  presSeq = seq;
  presHere = here;
  presKnown = true;
}

static void sendSummon() {
  HTTPClient http;
  if (!httpBegin(http, "/api/summon")) return;
  http.setTimeout(4000);
  http.addHeader("Content-Type", "application/json");
  http.POST("{}");
  http.end();
}

static void fetchChunk(int id, int start, int end) {
  Chunk c = {nullptr, 0, 16000, start, end};
  HTTPClient http;
  if (id && httpBegin(http, "/api/audio/" + String(id))) {
    const char* hdrs[] = {"X-Rate"};
    http.collectHeaders(hdrs, 1);
    if (http.GET() == 200) {
      int len = http.getSize();
      c.rate = http.header("X-Rate").toInt();
      if (c.rate <= 0) c.rate = 8000;
      c.buf = len > 0 ? (int16_t*)ps_malloc(len) : nullptr;
      if (c.buf) {
        WiFiClient* s = http.getStreamPtr();
        int got = 0;
        uint32_t t0 = millis();
        while (got < len && millis() - t0 < 20000) {
          int n = s->read((uint8_t*)c.buf + got, len - got);
          if (n > 0) got += n; else delay(1);
        }
        c.len = got / 2;
      }
    }
    http.end();
  }
  chunks[chunkCount] = c;
  chunkCount = chunkCount + 1;
}

static void freeChunks() {
  for (int i = 0; i < chunkCount; i++) free(chunks[i].buf);
  chunkCount = 0;
  chunkPlay = -1;
}

static void setNotice(const String& s) {
  xSemaphoreTake(lock, portMAX_DELAY);
  notice = s;
  xSemaphoreGive(lock);
}

static void sendQuestion() {
  HTTPClient http;
  int job = 0;
  if (httpBegin(http, "/api/listen")) {
    http.addHeader("Content-Type", "application/octet-stream");
    http.setTimeout(30000);
    int code = http.POST((uint8_t*)recBuf, recLen * 2);
    JsonDocument doc;
    if (code == 200 && !deserializeJson(doc, http.getString())) job = doc["job"] | 0;
    else setNotice(code == 503 ? "Voice is off on the bridge" : "Bridge not answering");
    http.end();
  }
  uint32_t t0 = millis();
  while (job && !cancelJob && millis() - t0 < 240000) {
    delay(250);
    JsonDocument doc;
    if (!getJson("/api/job/" + String(job), doc)) continue;
    xSemaphoreTake(lock, portMAX_DELAY);
    heard = doc["heard"] | "";
    reply = doc["reply"] | "";
    JsonObject hop = doc["hop"].as<JsonObject>();
    if (hop) {
      hopAim.read(hop);
      hopLabel = (const char*)(hop["label"] | "");
    }
    hopActive = (bool)hop;
    xSemaphoreGive(lock);
    // Fetch each new sentence's audio as soon as the bridge has made it
    JsonArray audio = doc["audio"].as<JsonArray>();
    for (int i = chunkCount; i < (int)audio.size() && i < MAX_CHUNKS && !cancelJob; i++)
      fetchChunk(audio[i]["id"] | 0, audio[i]["start"] | 0, audio[i]["end"] | 0);
    if ((doc["done"] | false) && (chunkCount >= (int)audio.size() || chunkCount >= MAX_CHUNKS)) {
      hopActive = false;
      replyDone = true;
      return;
    }
  }
  if (job && !cancelJob) setNotice("Claude took too long");
  hopActive = false;
  replyDone = true;
}

static void galleryNet();
static bool videoActive();

static void netTask(void*) {
  uint32_t lastPoll = 0;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      bridgeOk = false;
      static uint32_t lastLog = 0;
      if (millis() - lastLog > 5000) {  // shows on the USB serial monitor
        lastLog = millis();
        Serial.printf("[lilc] %s WiFi status %d, connecting to %s\n", BOARD_NAME,
                      (int)WiFi.status(), WIFI_SSID);
      }
      delay(500);
      continue;
    }
    if (sendPending) {
      sendQuestion();
      cancelJob = false;
      sendPending = false;
    }
    if (tasksWanted || millis() - lastPoll > TASK_POLL_MS) {
      tasksWanted = false;
      lastPoll = millis();
      fetchTasks();
    }
    if (summonWanted) {
      summonWanted = false;
      sendSummon();
    }
    if (stStarted) {  // the recording has begun: the bridge starts the sounds
      stStarted = false;
      HTTPClient http;
      if (httpBegin(http, "/api/record/" + String(stId) + "/start")) {
        http.POST("{}");
        http.end();
      }
    }
    if (stUpload) {
      HTTPClient http;
      if (httpBegin(http, "/api/record/" + String(stId))) {
        http.addHeader("Content-Type", "application/octet-stream");
        http.addHeader("X-Rate", String(stRate));
        http.addHeader("X-Channels", "2");
        http.addHeader("X-Head", String(stYaw, 1) + "," + String(stPitch, 1));
        http.setTimeout(30000);
        int code = http.POST((uint8_t*)stBuf, stLen * 2);
        http.end();
        Serial.printf("[lilc] recording %d uploaded: %d\n", stId, code);
      }
      free(stBuf);
      stBuf = nullptr;
      stUpload = false;
      stId = 0;
      setNotice("");
    }
    static uint32_t lastPres = 0;
    if (bridgeOk && !videoActive() && millis() - lastPres > PRESENCE_POLL_MS) {
      lastPres = millis();
      fetchPresence();
    }
    galleryNet();
    delay(videoActive() ? 5 : 50);
  }
}

// ------------------------------------------------------------ text helpers

static std::vector<String> wrap(const String& text, int width) {
  std::vector<String> lines;
  String line, word;
  auto flushWord = [&]() {
    if (!word.length()) return;
    String trial = line.length() ? line + " " + word : word;
    if (canvas.textWidth(trial) > width && line.length()) {
      lines.push_back(line);
      line = word;
    } else {
      line = trial;
    }
    word = "";
  };
  for (size_t i = 0; i <= text.length(); i++) {
    char c = i < text.length() ? text[i] : ' ';
    if (c == '\n') {
      flushWord();
      lines.push_back(line);
      line = "";
    } else if (c == ' ') {
      flushWord();
    } else {
      word += c;
    }
  }
  if (line.length()) lines.push_back(line);
  return lines;
}

static String agoText(int s) {
  if (s < 90) return "now";
  if (s < 3600) return String(s / 60) + "m";
  if (s < 86400) return String(s / 3600) + "h";
  return String(s / 86400) + "d";
}

static uint16_t statusColor(const String& st) {
  if (st == "needs you") return TFT_ORANGE;
  if (st == "working") return TFT_GREEN;
  if (st == "completed" || st == "review ready") return 0x5D7F;  // light blue
  return TFT_DARKGREY;
}

#include "vfd.h"

static String vfdClean(const String& in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\n' || c == '\t') c = ' ';
    if (c == ' ' && out.endsWith(" ")) continue;
    out += (char)toupper(c);
  }
  out.trim();
  return out;
}

#include "gallery.h"

static bool videoActive() { return videoReady; }

// ------------------------------------------------------------ face

static uint32_t blinkAt = 0, lookAt = 0, modeSince = 0, happyUntil = 0;
static float mouthOpen = 0;
static int tasksScroll = 0;
static String vfdText;      // reply, upper-cased for the VFD
static float vfdPos = 0;    // characters scrolled in
static int lastNeeds = -1;
static bool quietCancel = false;

// The face is a recreation of xeyes (X11, 1988): two rimmed oval eyes on the
// classic X root-window weave, pupils chasing the pointer. Here the pointer is
// your finger, or wherever lil' C's head is looking.
static M5Canvas weave(&M5.Display);
static float ptrX = W / 2, ptrY = 100;   // finger, when touching
static float gazeX = 0, gazeY = 0;       // -1..1, both eyes together
// Eyes and neck move separately: lil' C picks a spot to look at (lookX/Y, in
// head degrees). His eyes jump there at once; his head follows a moment
// later, more slowly, while the eyes counter-turn to stay on the spot.
static float lookX = 0, lookY = 0;
static uint32_t lookSince = 0;           // when the spot was picked
static float glanceX = 0, glanceY = 0;   // eyes-only glance at this spot (head stays)
static uint32_t glanceUntil = 0;
// Something drifting past that he follows (imaginary on the Stack-chan; an X
// cursor floats across the Tab5 desktop). Head degrees, like lookX/Y.
static bool tracking = false;
static float objX = 0, objY = 0, objVX = 0, objBaseY = 0;
static uint32_t objSince = 0;
static const uint32_t NECK_DELAY = 200;  // ms between eyes and head moving

// -1..1, roughly bell-shaped (small steps are common, big ones rare)
static float randBell() {
  return (random(-1000, 1001) + random(-1000, 1001) + random(-1000, 1001)) / 3000.0f;
}

// Picks the next spot: usually a small step from where he's looking now,
// pulled back towards the middle the further he has wandered.
static void nextSpot(float& x, float& y, float stepScale) {
  float rx = SERVO_X_RANGE, ry = SERVO_Y_RANGE;
  float away = min(1.0f, (lookX * lookX) / (rx * rx) + (lookY * lookY) / (ry * ry));
  if (random(1000) < away * 450) {  // "where was I?": back to about the middle
    x = randBell() * rx * 0.2f;
    y = randBell() * ry * 0.2f;
    return;
  }
  x = lookX + randBell() * rx * 0.6f * stepScale;
  y = lookY + randBell() * ry * 0.6f * stepScale;
  x -= x * away * 0.4f;  // lean back towards the centre
  y -= y * away * 0.4f;
  x = constrain(x, -rx, rx);
  y = constrain(y, -ry, ry);
}

static void startTracking() {
  tracking = true;
  glanceUntil = 0;
  float dir = random(2) ? 1 : -1;
  objX = -dir * SERVO_X_RANGE * 1.5f;        // comes in from beyond one side
  objVX = dir * (12 + random(24));           // 12..35 degrees a second
  objBaseY = randBell() * SERVO_Y_RANGE * 0.7f;
  objY = objBaseY;
  objSince = millis();
}

// How far his window has come in: 1 = in place, 0 = gone off the top of the
// screen. He arrives by sliding down from the top and leaves the same way.
static float eyePop = 1;

// Gone: the empty body stays exactly as he left it
static bool bodyEmpty = false;
static void leaveBody() {
  bodyEmpty = true;
  eyePop = 0;
  head.targetX = head.x;
  head.targetY = head.y;
}

// Arriving, he takes the neck as he finds it (it may still face wherever he
// last went) and only starts looking about after a moment
static uint32_t neckHeldUntil = 0;
static void settleIn(uint32_t now) {
  bodyEmpty = false;
  lookX = head.targetX = head.x;
  lookY = head.targetY = head.y;
  neckHeldUntil = now + 2500;
  lookAt = now + 2500;
}

static void lookAtSpot(float x, float y) {
  lookX = x;
  lookY = y;
  lookSince = millis();
  glanceUntil = 0;
}

// The neck position (head degrees) that faces an aim. On the Tab5 the window
// is the head: its range maps onto the desktop's edges.
static void aimNeck(const Aim& a, float& x, float& y) {
#if DESKTOP
  x = -a.dx * SERVO_X_RANGE * 0.96f;
  y = a.dy * SERVO_Y_RANGE * 1.3f;
#else
  x = a.yaw;
  y = a.pitch;
#endif
}

// Eyes first: where to look within the neck's everyday range
static void lookToward(const Aim& a) {
  float x, y;
  aimNeck(a, x, y);
  lookAtSpot(constrain(x, -(float)SERVO_X_RANGE, (float)SERVO_X_RANGE),
             constrain(y, -(float)SERVO_Y_RANGE, (float)SERVO_Y_RANGE));
}

// The neck turns all the way round to face it (as far as it goes)
static void turnToward(const Aim& a) {
  float x, y;
  aimNeck(a, x, y);
  head.lookFar(x, y);
}
static bool following = false;
static float slideOut() {
  float e = constrain(eyePop, 0.0f, 1.0f);
  return (1 - e) * (1 - e) * (1 - e);  // eased: quick to start, gentle to land
}

// A twm-style frame: black border, title bar with iconify button, name,
// stippled highlight and resize button. th: title bar height. The client
// area is (x + 2, y + th + 2, w - 4, h - th - 4).
static void twmFrameOn(LGFX_Sprite& g, int x, int y, int w, int h, const char* name, int th,
                       const lgfx::IFont* font, int shadow) {
  if (shadow) g.fillRect(x + shadow, y + shadow, w, h, TFT_BLACK);  // drop shadow
  g.fillRect(x, y, w, h, TFT_BLACK);
  g.fillRect(x + 2, y + 2, w - 4, th - 2, TFT_WHITE);
  g.fillRect(x + 2, y + th + 2, w - 4, h - th - 4, TFT_WHITE);
  int b = th * 3 / 4, oy = (th + 2 - b) / 2, ox = oy + 1;
  g.drawRect(x + ox, y + oy, b, b, TFT_BLACK);  // iconify button
  g.fillRect(x + ox + b * 5 / 18, y + oy + b * 5 / 18, b * 8 / 18, b * 8 / 18, TFT_BLACK);
  g.setFont(font);
  g.setTextColor(TFT_BLACK);
  int nx = x + ox + b + 7;
  g.drawString(name, nx, y + oy + 2);
  int hx = nx + g.textWidth(name) + 8, hx2 = x + w - (b + ox + 9);
  for (int ly = y + oy + 2; ly <= y + oy + b - 3; ly += 3) g.drawFastHLine(hx, ly, hx2 - hx, TFT_BLACK);
  int rx = x + w - (b + ox + 2);  // resize button
  g.drawRect(rx, y + oy, b, b, TFT_BLACK);
  g.drawRect(rx, y + oy, b * 12 / 18, b * 12 / 18, TFT_BLACK);
  g.drawRect(rx, y + oy, b * 7 / 18, b * 7 / 18, TFT_BLACK);
}
static float eyeCY = 100, eyeRY = 106;

static void makeWeave() {
  static const uint8_t rootWeave[4] = {0x07, 0x0d, 0x0b, 0x0e};  // X's root_weave
  weave.setPsram(true);
  weave.createSprite(W, H);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      weave.drawPixel(x, y, (rootWeave[y & 3] >> (x & 3)) & 1 ? TFT_BLACK : TFT_WHITE);
}

// His eyes are the same everywhere (Stack-chan, Tab5, the pop-up and the
// RetroKM agents' windows): ovals EYE_TALL times as tall as wide, EYE_GAP of
// their width apart, side by side in the middle of the window's client area,
// as big as fit. However the window is sized, they keep that look.
static const float EYE_TALL = 1.5f;
static const float EYE_GAP = 0.2f;  // between the eyes, in eye radii
static void eyeSize(int cw, int ch, int margin, int& rx, int& ry) {
  rx = (int)min((cw - 2.0f * margin) / (4 + EYE_GAP), (ch / 2.0f - margin) / EYE_TALL);
  ry = (int)(rx * EYE_TALL);
}
// The two eye centres for a client area starting at x0, cw wide
static void eyeCentres(int x0, int cw, int rx, int& left, int& right) {
  int d = (int)(rx * (1 + EYE_GAP / 2));
  left = x0 + cw / 2 - d;
  right = x0 + cw / 2 + d;
}

// One xeyes eye on g; (px, py) is the finger in g's coordinates
static void drawXEye(LGFX_Sprite& g, int cx, int cy, int rx, int ry, float pupilScale,
                     float px, float py) {
  int rim = rx * 16 / 100;
  g.fillEllipse(cx, cy, rx, ry, TFT_BLACK);
  g.fillEllipse(cx, cy, rx - rim, ry - rim, TFT_WHITE);
  float prx = rx * 0.24f * pupilScale, pry = prx * 1.2f;
  float ax = rx - rim - prx - 3, ay = ry - rim - pry - 3;
  float dx, dy;
  if (following) {  // like xeyes: each eye aims at the finger
    dx = px - cx;
    dy = py - cy;
  } else {          // both eyes look the same way
    dx = gazeX * ax;
    dy = gazeY * ay;
  }
  // stay inside the white
  float k = sqrtf((dx * dx) / (ax * ax) + (dy * dy) / (ay * ay));
  if (k > 1) { dx /= k; dy /= k; }
  g.fillEllipse(cx + (int)dx, cy + (int)dy, (int)prx, (int)pry, TFT_BLACK);
}

// Background for face screens: the root weave, or, on the desktop build,
// a see-through key colour so the X desktop shows through
static const uint16_t KEY = 0xF81F;
static void faceBackground() {
#if DESKTOP
  canvas.fillScreen(KEY);
#else
  weave.pushSprite(&canvas, 0, 0);
#endif
}

static void label(const String& text, int x, int y, uint16_t color) {
  canvas.setFont(&fonts::DejaVu12);
  int w = canvas.textWidth(text);
  canvas.fillRoundRect(x - 4, y - 3, w + 8, 18, 4, TFT_BLACK);
  canvas.setTextColor(color);
  canvas.drawString(text, x, y);
}

static void drawBar(const String& hint);

static void drawFace(bool withBar) {
  uint32_t now = millis();
  faceBackground();

  // Where are the eyes looking?
  auto t = uiTouch();
  if (t.isPressed()) {
    if (!following) { ptrX = t.x; ptrY = t.y; }
    following = true;
    ptrX += (t.x - ptrX) * 0.5f;
    ptrY += (t.y - ptrY) * 0.5f;
  } else {
    following = false;
    float gx, gy;  // +x = viewer's right, +y = down
    if (mode == THINKING) { gx = 0.7f; gy = -0.7f; }
    else if (mode == LISTENING) { gx = 0; gy = 0.3f; }
    else if (mode == SPEAKING || mode == REPLY) { gx = 0; gy = 0.6f; }
    else if (now < glanceUntil) {  // a glance: eyes only, at a spot in the world
      gx = -(glanceX - head.x) / SERVO_X_RANGE * 0.8f;
      gy = -(glanceY - head.y) / SERVO_Y_RANGE * 0.6f;
    }
    else {  // where the spot is, relative to where the head points now
      gx = -(lookX - head.x) / SERVO_X_RANGE * 0.8f;
      gy = -(lookY - head.y) / SERVO_Y_RANGE * 0.6f;
    }
    gx = constrain(gx, -1.0f, 1.0f);
    gy = constrain(gy, -1.0f, 1.0f);
    // Eyes are quick (a saccade takes a tenth of a second or so)
    static uint32_t lastEye = now;
    float k = 1 - expf(-min(0.2f, (now - lastEye) / 1000.0f) * 20.0f);
    lastEye = now;
    gazeX += (gx - gazeX) * k;
    gazeY += (gy - gazeY) * k;
  }

  // Shrink upwards when there is text under the eyes
  bool compact = mode == THINKING || mode == SPEAKING || mode == REPLY;
  // (the window's bottom is eyeCY + eyeRY + 4). Quick, whatever the frame rate
  static uint32_t lastSize = now;
  float ks = 1 - expf(-min(0.1f, (now - lastSize) / 1000.0f) * 14.0f);
  lastSize = now;
  eyeCY += ((compact ? 72 : 100) - eyeCY) * ks;
  eyeRY += ((compact ? 58 : 106) - eyeRY) * ks;
  float pupil = 1 + mouthOpen * 0.6f;  // pupils pulse while talking
#if !DESKTOP  // (the desktop build draws its window in present())
  // His xeyes window covers the screen above the notice line (shorter when
  // there's text under it) and slides down from the top when he arrives
  float out = slideOut();
  if (out < 0.99f) {
    const int th = 16;
    int bottom = (int)(eyeCY + eyeRY + 4);
    int y0 = 2 - (int)(out * (bottom + 10)), wh = bottom - 2;
    twmFrameOn(canvas, 2, y0, W - 8, wh, "xeyes", th, &fonts::DejaVu9, 3);
    int cw = W - 12, ch = wh - th - 4, top = y0 + th + 2, rx, ry;
    eyeSize(cw, ch, 6, rx, ry);
    int el, er;
    eyeCentres(4, cw, rx, el, er);
    drawXEye(canvas, el, top + ch / 2, rx, ry, pupil, ptrX, ptrY);
    drawXEye(canvas, er, top + ch / 2, rx, ry, pupil, ptrX, ptrY);
  }
#endif

#if DESKTOP
  if (tracking && mode == IDLE) {  // the thing he's watching: an X cursor drifting by
    float sx = 160 - objX / SERVO_X_RANGE * 0.7f * 163;
    float sy = 95 - objY / SERVO_Y_RANGE * 45;
    if (sx > -10 && sx < W + 10) {
      canvas.drawWideLine(sx - 5, sy - 5, sx + 5, sy + 5, 2.5f, TFT_WHITE);
      canvas.drawWideLine(sx - 5, sy + 5, sx + 5, sy - 5, 2.5f, TFT_WHITE);
      canvas.drawWideLine(sx - 5, sy - 5, sx + 5, sy + 5, 1.2f, TFT_BLACK);
      canvas.drawWideLine(sx - 5, sy + 5, sx + 5, sy - 5, 1.2f, TFT_BLACK);
    }
  }
#endif
  bool worried = mode == IDLE && needsCount > 0;
  if (worried) {  // sweat drop
    canvas.fillTriangle(298, 14, 291, 28, 305, 28, 0x5D7F);
    canvas.fillCircle(298, 30, 7, 0x5D7F);
    canvas.drawCircle(298, 30, 7, TFT_BLACK);
  }
  if (mode == LISTENING) {
    int secs = recLen / REC_RATE;
    label("   listening " + String(MAX_RECORD_SECONDS - secs) + "s", 10, 10, TFT_LIGHTGREY);
    canvas.fillCircle(16, 18, 5, (now / 400) % 2 ? TFT_RED : 0x8000);
  }
  if (mode == THINKING) {  // thought dots
    for (int i = 0; i < 3; i++) {
      bool on = (now / 300) % 4 > (uint32_t)i;
      canvas.fillCircle(266 + i * 18, 34 - i * 10, 4 + i, TFT_BLACK);
      canvas.fillCircle(266 + i * 18, 34 - i * 10, 2 + i, on ? TFT_WHITE : TFT_DARKGREY);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    String h = heard;
    xSemaphoreGive(lock);
    String txt = vfdClean(h.length() ? "\"" + h + "\"" : "THINKING...");
    float pass = vfdPassLength(txt);
    drawVFD(txt, fmodf((now - modeSince) / 1000.0f * VFD_CPS, pass));
  }

#if DESKTOP
  if (withBar) drawBar("hold my face to talk");
#else
  if (withBar) drawBar("");  // (the Stack-chan doesn't need telling)
#endif
}

// The bottom bar (a slim strip: pictures | (desk |) tasks), and above it a
// notice when there is something to say
static const int BAR_Y = 216;
static void drawBar(const String& hint) {
  xSemaphoreTake(lock, portMAX_DELAY);
  String n = notice;
  int needs = needsCount, working = workingCount;
  xSemaphoreGive(lock);
  if (WiFi.status() != WL_CONNECTED) n = "connecting to WiFi...";
  else if (!bridgeOk) n = "looking for the bridge...";
  if (!n.length()) n = hint;
  canvas.setFont(&fonts::DejaVu12);
  if (n.length()) label(n, (W - canvas.textWidth(n)) / 2, BAR_Y - 20, TFT_LIGHTGREY);

  const int ty = BAR_Y + 6, cy = BAR_Y + 12;
  canvas.fillRect(0, BAR_Y, W, H - BAR_Y, 0x10A2);
  canvas.setTextColor(mediaNew ? TFT_YELLOW : TFT_LIGHTGREY);
  canvas.drawString(mediaNew ? "< new pictures!" : "< pictures", 6, ty);
#if HAS_KVM
  canvas.drawFastVLine(W / 3, BAR_Y + 4, H - BAR_Y - 8, 0x4208);
  canvas.drawFastVLine(W * 2 / 3, BAR_Y + 4, H - BAR_Y - 8, 0x4208);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.setTextDatum(top_center);
  canvas.drawString("desk", W / 2, ty);
  canvas.setTextDatum(top_left);
#else
  canvas.drawFastVLine(W / 2, BAR_Y + 4, H - BAR_Y - 8, 0x4208);
#endif
  // tasks: a green dot per working count, an orange one for those that need you
  int x = W - 6 - canvas.textWidth("tasks >");
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("tasks >", x, ty);
  auto count = [&](int n, uint16_t c) {
    if (!n) return;
    String s(n);
    x -= canvas.textWidth(s) + 18;
    canvas.fillCircle(x + 5, cy, 4, c);
    canvas.setTextColor(c);
    canvas.drawString(s, x + 12, ty);
  };
  count(needs, TFT_ORANGE);
  count(working, TFT_GREEN);
}

// ------------------------------------------------------------ hopping

enum HopPhase { HOP_TURN, HOP_AWAY, HOP_BACK };
static HopPhase hopPhase = HOP_TURN;
static uint32_t hopSince = 0;

// lil' C has gone to check on a computer: an empty desk with a sign
static void drawSign(uint32_t now, const char* line1, const char* line2);
static void drawBRB(uint32_t now) {
  drawSign(now, "BE RIGHT", "BACK!");
  String l = "checking " + hopLabel + "...";
  canvas.setFont(&fonts::DejaVu12);
  label(l, (W - canvas.textWidth(l)) / 2, 190, TFT_LIGHTGREY);
}

// lil' C is somewhere else: the same empty desk, saying where he went
static void drawAway(uint32_t now) {
  drawSign(now, "BACK", "SOON!");
  xSemaphoreTake(lock, portMAX_DELAY);
  String where = presLabel, kind = presKind;
  xSemaphoreGive(lock);
  String l = where.length() ? String("lil' C is ") + (kind == "machine" ? "visiting " : "on ") + where
                            : String("lil' C is out");
  canvas.setFont(&fonts::DejaVu12);
  label(l, (W - canvas.textWidth(l)) / 2, 166, TFT_WHITE);
  drawBar("tap to call him back");
}

// He's on his way here: the sign says so until he arrives
static void drawComing(uint32_t now) {
  drawSign(now, "HOLD ON,", "COMING!");
  xSemaphoreTake(lock, portMAX_DELAY);
  String from = presFrom;
  xSemaphoreGive(lock);
  String l = from.length() ? "lil' C is on his way from " + from : String("lil' C is on his way");
  canvas.setFont(&fonts::DejaVu12);
  label(l, (W - canvas.textWidth(l)) / 2, 166, TFT_WHITE);
  drawBar("");
}

static void drawSign(uint32_t now, const char* line1, const char* line2) {
  faceBackground();
  float sway = sinf(now / 650.0f) * 5;
  int sx = 54 + (int)sway, sy = 64, sw = 212, sh = 96;
  canvas.drawWideLine(160, 26, sx + 24, sy, 1.2f, TFT_BLACK);   // string
  canvas.drawWideLine(160, 26, sx + sw - 24, sy, 1.2f, TFT_BLACK);
  canvas.fillCircle(160, 26, 5, TFT_BLACK);                     // nail
  canvas.fillCircle(160, 26, 2, TFT_LIGHTGREY);
  canvas.fillRect(sx + 4, sy + 4, sw, sh, TFT_BLACK);            // shadow
  canvas.fillRect(sx, sy, sw, sh, TFT_WHITE);
  canvas.drawRect(sx, sy, sw, sh, TFT_BLACK);
  canvas.drawRect(sx + 4, sy + 4, sw - 8, sh - 8, TFT_BLACK);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextDatum(top_center);
  canvas.drawString(line1, sx + sw / 2, sy + 14);
  canvas.drawString(line2, sx + sw / 2, sy + 52);
  canvas.setTextDatum(top_left);
}

static void drawTasks() {
  canvas.fillScreen(TFT_BLACK);
  xSemaphoreTake(lock, portMAX_DELAY);
  std::vector<Task> list = tasks;
  xSemaphoreGive(lock);
  const int rowH = 50, top = 34;
  int maxScroll = max(0, (int)list.size() * rowH - (H - top));
  tasksScroll = constrain(tasksScroll, 0, maxScroll);
  for (size_t i = 0; i < list.size(); i++) {
    int y = top + i * rowH - tasksScroll;
    if (y < top - rowH || y > H) continue;
    const Task& t = list[i];
    uint16_t c = statusColor(t.status);
    bool pulse = t.status == "working" && (millis() / 500) % 2;
    canvas.fillCircle(14, y + 14, 6, pulse ? (uint16_t)0x03E0 : c);
    canvas.setFont(&fonts::DejaVu18);
    canvas.setTextColor(TFT_WHITE);
    String title = t.title;
    while (title.length() > 3 && canvas.textWidth(title) > 236) title.remove(title.length() - 1);
    canvas.drawString(title, 28, y + 4);
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextColor(c);
    String st = t.status + "  " + agoText(t.ago);
    canvas.drawString(st, W - 6 - canvas.textWidth(st), y + 8);
    canvas.setTextColor(TFT_DARKGREY);
    String d = t.detail;
    while (d.length() > 3 && canvas.textWidth(d) > 284) d.remove(d.length() - 1);
    canvas.drawString(d, 28, y + 26);
    canvas.drawFastHLine(28, y + rowH - 3, W - 34, 0x2104);
  }
  if (list.empty()) {
    canvas.setFont(&fonts::DejaVu18);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString(bridgeOk ? "No tasks right now" : "Bridge not found", 60, 110);
  }
  canvas.fillRect(0, 0, W, top - 2, 0x10A2);
  canvas.setFont(&fonts::DejaVu18);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString("< Claude tasks", 8, 7);
}

// ------------------------------------------------------------ audio

static void startListening() {
  tracking = false;
  M5.Speaker.stop();
  if (sendPending) {  // still receiving the last answer: drop it
    cancelJob = true;
    while (sendPending) delay(5);
  }
  freeChunks();
  M5.Speaker.end();
  M5.Mic.begin();
  recLen = 0;
  mode = LISTENING;
  modeSince = millis();
  setNotice("");
}

static void pumpMic() {
  // Keep two chunks queued; M5.Mic fills them in the background
  while (M5.Mic.isRecording() < 2 && recLen + REC_CHUNK <= REC_MAX) {
    M5.Mic.record(recBuf + recLen, REC_CHUNK, REC_RATE);
    recLen += REC_CHUNK;
  }
}

// Both microphones, as left and right, for the bridge (not for talking)
static void startStereo() {
  stId = recReqId;
  recReqId = 0;
  stLastId = stId;
  stRate = recReqRate;
  stTotal = (int)((int64_t)recReqMs * stRate / 1000) * 2;
  stLen = 0;
  stBuf = (int16_t*)ps_malloc(stTotal * 2);
  if (!stBuf) { stId = 0; return; }
  // The microphones are on the head: hold it still while they listen, and
  // tell the bridge which way it pointed
  tracking = false;
  head.targetX = head.x;
  head.targetY = head.y;
  stYaw = head.x;
  stPitch = head.y;
  M5.Speaker.stop();
  M5.Speaker.end();
  auto c = M5.Mic.config();
  c.sample_rate = stRate;
  c.stereo = true;
  M5.Mic.config(c);
  M5.Mic.begin();
  stStarted = true;
  setNotice("listening to the room...");
}

static void pumpStereo() {
  // The whole buffer goes to the microphone in two halves, so it fills
  // without a gap however long a frame takes (small chunks topped up once a
  // frame lost audio and squeezed the timeline)
  int half = (stTotal / 2) & ~1;
  if (stLen == 0) {
    M5.Mic.record(stBuf, half, stRate, true);
    M5.Mic.record(stBuf + half, stTotal - half, stRate, true);
    stLen = stTotal;
  }
  if (stLen == stTotal && !M5.Mic.isRecording()) {  // done: back to normal
    M5.Mic.end();
    auto c = M5.Mic.config();
    c.sample_rate = REC_RATE;
    c.stereo = false;
    M5.Mic.config(c);
    M5.Speaker.begin();
    M5.Speaker.setVolume(VOLUME);
    stUpload = true;
  }
}

static void stopListening(bool send) {
  while (M5.Mic.isRecording()) delay(1);
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(VOLUME);
  if (send) {
    xSemaphoreTake(lock, portMAX_DELAY);
    heard = "";
    reply = "";
    xSemaphoreGive(lock);
    vfdText = "";
    replyDone = false;
    sendPending = true;
    mode = THINKING;
  } else {
    mode = IDLE;
  }
  modeSince = millis();
}

// Picks up reply text as it grows
static void syncReplyText() {
  xSemaphoreTake(lock, portMAX_DELAY);
  String r = reply;
  xSemaphoreGive(lock);
  if (r.length() != vfdText.length()) vfdText = vfdClean(r);
}

// Plays the next sentence if the last one is done; false when none is ready
static bool playNextChunk() {
  while (chunkPlay + 1 < chunkCount) {
    Chunk& c = chunks[++chunkPlay];
    if (!c.buf || !c.len) continue;  // a sentence whose speech failed
    M5.Speaker.playRaw(c.buf, c.len, c.rate, false, 1, 0);
    playStart = millis();
    return true;
  }
  return false;
}

static void startReply() {
  syncReplyText();
  vfdPos = 0;
  if (playNextChunk()) {
    mode = SPEAKING;
  } else if (vfdText.length()) {
    mode = REPLY;
  } else {  // failed; the notice says why
    mode = IDLE;
  }
  modeSince = millis();
}

static void updateMouth() {
  float target = 0;
  if (mode == SPEAKING && chunkPlay >= 0 && chunkPlay < chunkCount && M5.Speaker.isPlaying()) {
    const Chunk& c = chunks[chunkPlay];
    int idx = (int)((millis() - playStart) * (uint64_t)c.rate / 1000);
    long sum = 0;
    int n = 0;
    for (int i = idx; i < idx + c.rate / 40 && i < c.len; i++, n++) sum += abs(c.buf[i]);
    if (n) target = min(1.0f, (float)sum / n / 5000.0f);
    // Keep the VFD in step with the speech: the word being said sits a few
    // cells in from the left
    float f = min(1.0f, (float)idx / max(1, c.len));
    vfdPos = max(vfdPos, c.start + f * (c.end - c.start) + VFD_CELLS * 0.7f);
  }
  mouthOpen += (target - mouthOpen) * 0.5f;
}

// ------------------------------------------------------------ main

static void connectWifi() {
#ifdef WIFI_SDIO_PINS
  WiFi.setPins(WIFI_SDIO_PINS);
#endif
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

#if HAS_PANEL
// Tab5: live task list at full resolution, shown in a window
static M5Canvas panel(&M5.Display);
static uint32_t panelDrawn = 0;

static void drawPanel() {
  panel.fillScreen(TFT_WHITE);
  xSemaphoreTake(lock, portMAX_DELAY);
  std::vector<Task> list = tasks;
  bool ok = bridgeOk;
  xSemaphoreGive(lock);
  int y = 10;
  for (const Task& t : list) {
    if (y > PANEL_H - 70) break;
    uint16_t c = statusColor(t.status);
    if (c == TFT_GREEN) c = rgb565(0, 150, 40);  // darker on white
    panel.fillCircle(14, y + 12, 7, c);
    panel.drawCircle(14, y + 12, 7, TFT_BLACK);
    panel.setFont(&fonts::FreeSans9pt7b);
    panel.setTextColor(TFT_BLACK);
    String title = t.title;
    while (title.length() > 3 && panel.textWidth(title) > PANEL_W - 38) title.remove(title.length() - 1);
    panel.drawString(title, 28, y + 2);
    panel.setFont(&fonts::DejaVu12);
    panel.setTextColor(c);
    panel.drawString(t.status + "  " + agoText(t.ago), 28, y + 26);
    panel.setTextColor(TFT_DARKGREY);
    int lines = 0;
    canvas.setFont(&fonts::DejaVu12);  // wrap() measures with canvas
    for (const String& l : wrap(t.detail, PANEL_W - 38)) {
      if (lines++ == 2) break;
      panel.drawString(l, 28, y + 44 + (lines - 1) * 15);
    }
    y += 50 + max(1, min(lines, 2)) * 15 + 10;
    panel.drawFastHLine(28, y - 6, PANEL_W - 40, TFT_LIGHTGREY);
  }
  if (list.empty()) {
    panel.setFont(&fonts::FreeSans9pt7b);
    panel.setTextColor(TFT_DARKGREY);
    panel.drawString(ok ? "No tasks right now" : "Looking for the bridge...", 16, 20);
  }
}
#endif

#if DESKTOP
// ------------------------------------------------------------ X desktop (Tab5)

static M5Canvas desk(&M5.Display);       // the whole screen, composed per frame
static M5Canvas rootWeave(&M5.Display);  // X root window background
static M5Canvas bgLayer(&M5.Display);    // weave + task window, redrawn every 2 s
static float winX = 260, winY = 120;     // xeyes window position
static const int XE_W = 440, XE_H = 360, TITLE_H = 24;

// desk/rootWeave live in the panel's own layout (720x1280 portrait, 565 not
// byte-swapped) so a frame goes to the screen as plain row copies; they are
// drawn on through setRotation(1) like the landscape display.
static int dIdx0, dStepX, dStepY;  // buffer index of landscape (x, y)

static void makeDesktop() {
  for (M5Canvas* c : {&desk, &rootWeave, &bgLayer}) {
    c->setPsram(true);
    c->setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    c->createSprite(SCREEN_H, SCREEN_W);  // portrait, like the panel
    c->setRotation(1);
  }
  // Learn where landscape pixels land in the buffer
  uint16_t* buf = (uint16_t*)desk.getBuffer();
  auto find = [&](int x, int y) {
    desk.fillScreen(TFT_BLACK);
    desk.drawPixel(x, y, TFT_WHITE);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) if (buf[i]) return i;
    return 0;
  };
  dIdx0 = find(0, 0);
  dStepX = find(1, 0) - dIdx0;
  dStepY = find(0, 1) - dIdx0;
  static const uint8_t bits[4] = {0x07, 0x0d, 0x0b, 0x0e};  // X's root_weave
  for (int y = 0; y < SCREEN_H; y++)
    for (int x = 0; x < SCREEN_W; x++)
      rootWeave.drawPixel(x, y, (bits[y & 3] >> (x & 3)) & 1 ? TFT_BLACK : TFT_WHITE);
}

// A twm-style frame: black border, title bar with iconify button, name,
// stippled highlight and resize button. Returns nothing; client area is
// (x + 2, y + TITLE_H + 2, w - 4, h - TITLE_H - 4).
static void twmFrame(int x, int y, int w, int h, const char* name, M5Canvas& desk = ::desk) {
  twmFrameOn(desk, x, y, w, h, name, TITLE_H, &fonts::FreeSansBold9pt7b, 6);
}

// Copies a normal (byte-swapped 565) sprite onto desk at (x0, y0), scaled up
// `scale` times; keyed skips KEY pixels so the desktop shows through.
// Done by hand: the library's rotated/scaled paths are far too slow here.
static void blitToDesk(M5Canvas& src, int x0, int y0, int scale, bool keyed,
                       M5Canvas& dst = desk) {
  const uint16_t* s = (const uint16_t*)src.getBuffer();
  uint16_t* d = (uint16_t*)dst.getBuffer();
  const int sw = src.width(), sh = src.height();
  const uint16_t key = (uint16_t)((KEY >> 8) | (KEY << 8));
  // Walk so that consecutive writes are neighbours in memory (PSRAM cache)
  bool yFast = abs(dStepY) == 1;
  int outerN = yFast ? sw : sh, innerN = yFast ? sh : sw;
  int outerStep = yFast ? dStepX : dStepY, innerStep = yFast ? dStepY : dStepX;
  int srcOuter = yFast ? 1 : sw, srcInner = yFast ? sw : 1;
  int base0 = dIdx0 + y0 * dStepY + x0 * dStepX;
  for (int o = 0; o < outerN; o++) {
    const uint16_t* sp = s + o * srcOuter;
    for (int a = 0; a < scale; a++) {
      int p = base0 + (o * scale + a) * outerStep;
      for (int i = 0; i < innerN; i++, sp += srcInner) {
        uint16_t v = *sp;
        if (keyed && v == key) { p += scale * innerStep; continue; }
        v = (uint16_t)((v >> 8) | (v << 8));  // to the panel's byte order
        for (int b = 0; b < scale; b++, p += innerStep) d[p] = v;
      }
      sp -= innerN * srcInner;
    }
  }
}

// ---- dirty rectangles: only redraw and send the parts that changed

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};
static Rect xeyesRect;  // where the xeyes window was drawn this frame

static bool overlaps(const Rect& a, const Rect& b) {
  return !a.empty() && !b.empty() && a.x < b.x + b.w && b.x < a.x + a.w &&
         a.y < b.y + b.h && b.y < a.y + a.h;
}

static Rect unite(const Rect& a, const Rect& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  int x1 = min(a.x, b.x), y1 = min(a.y, b.y);
  int x2 = max(a.x + a.w, b.x + b.w), y2 = max(a.y + a.h, b.y + b.h);
  return {x1, y1, x2 - x1, y2 - y1};
}

// Landscape rect -> rect in the panel-layout buffer (clipped to the screen)
static bool bufRect(Rect r, int& bx, int& by, int& bw, int& bh) {
  int x2 = min(SCREEN_W, r.x + r.w), y2 = min(SCREEN_H, r.y + r.h);
  r.x = max(0, r.x);
  r.y = max(0, r.y);
  if (x2 <= r.x || y2 <= r.y) return false;
  int i1 = dIdx0 + r.x * dStepX + r.y * dStepY;
  int i2 = dIdx0 + (x2 - 1) * dStepX + (y2 - 1) * dStepY;
  const int pw = SCREEN_H;  // the buffer is portrait
  int ax = i1 % pw, ay = i1 / pw, cx = i2 % pw, cy = i2 / pw;
  bx = min(ax, cx);
  by = min(ay, cy);
  bw = abs(cx - ax) + 1;
  bh = abs(cy - ay) + 1;
  return true;
}

static void restoreRect(const Rect& r) {  // background -> desk
  int bx, by, bw, bh;
  if (!bufRect(r, bx, by, bw, bh)) return;
  uint16_t* d = (uint16_t*)desk.getBuffer();
  const uint16_t* s = (const uint16_t*)bgLayer.getBuffer();
  for (int y = by; y < by + bh; y++)
    memcpy(d + y * SCREEN_H + bx, s + y * SCREEN_H + bx, bw * 2);
}

static void pushRect(const Rect* r) {  // desk -> screen (null: everything)
  int bx = 0, by = 0, bw = SCREEN_H, bh = SCREEN_W;
  if (r && !bufRect(*r, bx, by, bw, bh)) return;
  // Unrotated, same pixel format: the library copies whole rows
  M5.Display.setRotation(0);
  M5.Display.setClipRect(bx, by, bw, bh);
  desk.pushSprite(&M5.Display, 0, 0);
  M5.Display.clearClipRect();
  M5.Display.setRotation(1);
}

// Where the UI canvas draws over the desk, as one box per horizontal strip
// (so a bar at the bottom and a badge at the top don't make one huge box)
static const int BANDS = 10, BAND_H = H / BANDS;
static void overlayRects(Rect out[BANDS], uint32_t sums[BANDS]) {
  const uint16_t* s = (const uint16_t*)canvas.getBuffer();
  const uint16_t key = (uint16_t)((KEY >> 8) | (KEY << 8));
  for (int band = 0; band < BANDS; band++) {
    int x1 = W, y1 = H, x2 = -1, y2 = -1;
    uint32_t sum = 2166136261u;  // FNV-1a over the strip: did it change?
    for (int y = band * BAND_H; y < (band + 1) * BAND_H; y++) {
      const uint16_t* row = s + y * W;
      for (int x = 0; x < W; x++) {
        sum = (sum ^ row[x]) * 16777619u;
        if (row[x] != key) {
          x1 = min(x1, x); x2 = max(x2, x);
          y1 = min(y1, y); y2 = max(y2, y);
        }
      }
    }
    sums[band] = sum;
    out[band] = x2 < 0 ? Rect()
        : Rect{x1 * UI_SCALE, y1 * UI_SCALE, (x2 - x1 + 1) * UI_SCALE, (y2 - y1 + 1) * UI_SCALE};
  }
}

static bool faceScreen() {
  return mode == IDLE || mode == LISTENING || mode == THINKING ||
         mode == SPEAKING || mode == REPLY || mode == HOP || mode == AWAY;
}

// The xeyes window drifts across the desktop towards where lil' C looks
static m5::touch_detail_t xeTouch;

// Moves the window towards where lil' C looks; sets xeyesRect
static void placeXeyesWindow() {
  bool textBelow = mode == THINKING || mode == SPEAKING || mode == REPLY;
  float minX = 16, maxX = 962 - XE_W - 16;
  float minY = 16, maxY = (textBelow ? 448 : 548) - XE_H;
  auto& raw = xeTouch = M5.Touch.getDetail();
  float tx, ty;
  if (following && raw.isPressed()) {  // lean towards the finger
    tx = raw.x - XE_W / 2.0f;
    ty = raw.y - XE_H / 2.0f;
    tx = winX + (tx - winX) * 0.3f;
    ty = winY + (ty - winY) * 0.3f;
  } else {
    // The window is his head: it follows the neck, not the eyes. Stretch the
    // neck's range so he uses the whole desktop.
    float nx = -head.gazeX() * 0.8f, ny = -head.gazeY() * 0.6f;
    if (mode == THINKING) { nx = 0.7f; ny = -0.7f; }
    else if (mode == LISTENING) { nx = 0; ny = 0.3f; }
    else if (mode == SPEAKING || mode == REPLY) { nx = 0; ny = 0.6f; }
    tx = minX + (constrain(nx * 0.65f, -0.5f, 0.5f) + 0.5f) * (maxX - minX);
    ty = minY + (constrain(ny * 0.65f, -0.5f, 0.5f) + 0.5f) * (maxY - minY);
  }
  // Glide there at the same speed whatever the frame rate
  static uint32_t last = millis();
  float dt = min(0.25f, (millis() - last) / 1000.0f);
  last = millis();
  float k = 1 - expf(-dt * 3.0f);
  winX += (constrain(tx, minX, maxX) - winX) * k;
  winY += (constrain(ty, minY, max(minY, maxY)) - winY) * k;
  winY = constrain(winY, minY, max(minY, maxY) + 0.0f);

  xeyesRect = {(int)winX, (int)winY, XE_W + 6, XE_H + 6};  // + drop shadow
  xeyesRect.y -= (int)(slideOut() * (winY + XE_H + 12));     // arriving: down from the top
}

static void drawXeyesWindow() {
  auto& raw = xeTouch;
  int x = xeyesRect.x, y = xeyesRect.y;
  twmFrame(x, y, XE_W, XE_H, "xeyes");
  int cx = x + 2, cy = y + TITLE_H + 2, cw = XE_W - 4, ch = XE_H - TITLE_H - 4;
  int rx, ry;
  eyeSize(cw, ch, 10, rx, ry);
  float pupil = 1 + mouthOpen * 0.6f;
  int el, er;
  eyeCentres(cx, cw, rx, el, er);
  drawXEye(desk, el, cy + ch / 2, rx, ry, pupil, raw.x, raw.y);
  drawXEye(desk, er, cy + ch / 2, rx, ry, pupil, raw.x, raw.y);
}
#endif

// Puts the frame on the screen
static bool desktopStale = false;  // something else drew on the screen (the desk map)

static void present() {
#if DESKTOP
  uint32_t t0 = millis();
  static bool wasFace = false;
  if (desktopStale) { wasFace = false; desktopStale = false; panelDrawn = 0; }
  static Rect prevX, prevO[BANDS];
  static uint32_t prevSum[BANDS];
  bool full = !faceScreen() || !wasFace;
  wasFace = faceScreen();
  if (millis() - panelDrawn > 2000) {  // background: weave + task window
    panelDrawn = millis();
    drawPanel();
    memcpy(bgLayer.getBuffer(), rootWeave.getBuffer(), SCREEN_W * SCREEN_H * 2);
    twmFrame(968, 10, PANEL_W + 4, PANEL_H + TITLE_H + 6, "claude tasks", bgLayer);
    blitToDesk(panel, 970, 10 + TITLE_H + 2, 1, false, bgLayer);
    full = true;
  }
  xeyesRect = Rect();
  bool showXeyes = faceScreen() && !(mode == HOP && hopPhase == HOP_AWAY) &&
                   !(mode == AWAY && presHere) && eyePop > 0.02f;
  if (showXeyes) placeXeyesWindow();
  Rect newO[BANDS];
  uint32_t newSum[BANDS];
  bool bandDirty[BANDS];
  if (faceScreen()) overlayRects(newO, newSum);
  Rect moved = unite(prevX, xeyesRect);
  for (int i = 0; i < BANDS; i++)  // unchanged strips away from the window: leave alone
    bandDirty[i] = full || newSum[i] != prevSum[i] || overlaps(unite(prevO[i], newO[i]), moved);
  if (full) {
    memcpy(desk.getBuffer(), bgLayer.getBuffer(), SCREEN_W * SCREEN_H * 2);
  } else {  // put back the background where things were last frame
    Rect& n = xeyesRect;
    if (n.empty()) {
      restoreRect(prevX);
    } else if (!prevX.empty()) {  // only what the moved window uncovered
      const Rect& p = prevX;
      restoreRect({p.x, p.y, p.w, n.y - p.y});                          // above
      restoreRect({p.x, n.y + n.h, p.w, p.y + p.h - (n.y + n.h)});      // below
      restoreRect({p.x, p.y, n.x - p.x, p.h});                          // left
      restoreRect({n.x + n.w, p.y, p.x + p.w - (n.x + n.w), p.h});      // right
    }
    if (!n.empty()) {  // the corners beside the drop shadow are background
      restoreRect({n.x + XE_W, n.y, 6, 6});
      restoreRect({n.x, n.y + XE_H, 6, 6});
    }
    for (int i = 0; i < BANDS; i++) if (bandDirty[i]) restoreRect(prevO[i]);
  }
  uint32_t tA = millis(), tB = tA;
  if (faceScreen()) {
    if (showXeyes) drawXeyesWindow();
    tB = millis();
    blitToDesk(canvas, 0, 0, UI_SCALE, true);  // the rest of the UI floats on top
  } else {
    blitToDesk(canvas, 0, 0, UI_SCALE, false);
  }
  uint32_t t1 = millis();
  if (full) {
    pushRect(nullptr);
  } else {
    Rect a = unite(prevX, xeyesRect);
    pushRect(&a);
    for (int i = 0; i < BANDS; i++) {
      Rect b = unite(prevO[i], newO[i]);
      if (bandDirty[i] && !b.empty()) pushRect(&b);
    }
  }
  prevX = xeyesRect;
  for (int i = 0; i < BANDS; i++) {
    prevO[i] = faceScreen() ? newO[i] : Rect();
    prevSum[i] = faceScreen() ? newSum[i] : 0;
  }
  static uint32_t frames = 0, sumCompose = 0, sumPush = 0, lastLog = 0;
  static uint32_t sumCopy = 0, sumEyes = 0;
  sumCopy += tA - t0;
  sumEyes += tB - tA;
  frames++;
  sumCompose += t1 - t0;
  sumPush += millis() - t1;
  if (millis() - lastLog > 5000) {  // frame timing on the USB serial log
    Serial.printf("[lilc] %.1f fps, compose %u ms (bg %u, xeyes %u), push %u ms\n",
                  frames * 1000.0f / (millis() - lastLog), sumCompose / frames,
                  sumCopy / frames, sumEyes / frames, sumPush / frames);
    frames = sumCompose = sumPush = sumCopy = sumEyes = 0;
    lastLog = millis();
  }
#elif UI_SCALE > 1
  canvas.pushRotateZoom(&M5.Display, UI_X + W * UI_SCALE / 2.0f, UI_Y + H * UI_SCALE / 2.0f,
                        0, UI_SCALE, UI_SCALE);
#else
  canvas.pushSprite(0, 0);
#endif
}

// The bottom bar: pictures | (desk |) tasks
static void barTap(int x) {
#if HAS_KVM
  if (x >= W / 3 && x < W * 2 / 3) {
    mode = KVM;
    rkmPanelShow();
    modeSince = millis();
    return;
  }
  bool left = x < W / 3;
#else
  bool left = x < W / 2;
#endif
  if (left) {
    openGallery();
  } else {
    mode = TASKS;
    tasksScroll = 0;
    tasksWanted = true;
  }
  modeSince = millis();
}

void setup() {
  Serial.begin(115200);  // status and frame timing on the USB serial port
  auto cfg = M5.config();
  M5.begin(cfg);
  // landscape: CoreS3 is 320x240, Tab5 1280x720
  M5.Display.setRotation(M5.Display.width() < M5.Display.height() ? 1 : M5.Display.getRotation());
  M5.Display.setBrightness(120);
  M5.Speaker.setVolume(VOLUME);
  canvas.setPsram(true);
  canvas.createSprite(W, H);
  canvas.setTextDatum(top_left);
  makeWeave();
  photo.setPsram(true);
  photo.createSprite(W, H);
#if HAS_PANEL
  panel.setPsram(true);
  panel.createSprite(PANEL_W, PANEL_H);
#endif
#if DESKTOP
  makeDesktop();
#endif
  lock = xSemaphoreCreateMutex();
  recBuf = (int16_t*)ps_malloc(REC_MAX * 2);
  head.begin();
  connectWifi();
#if HAS_KVM
  rkmPanelBegin(HUB_HOST, HUB_PORT, true);
#endif
  // updates over WiFi:  pio run -e tab5-ota (or cores3-ota) -t upload
  ArduinoOTA.setHostname("lilc-" BOARD_DEV);
  ArduinoOTA.onStart([] {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSansBold18pt7b);
    M5.Display.drawString("updating...", M5.Display.width() / 2, M5.Display.height() / 2);
  });
  ArduinoOTA.begin();
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
  blinkAt = millis() + 2000;
}

void loop() {
  M5.update();
  uint32_t now = millis();
  static uint32_t lastLoop = now;
  float dt = min(0.1f, (now - lastLoop) / 1000.0f);  // seconds since the last pass
  lastLoop = now;
  auto t = uiTouch();
  static int dragLastY = 0;
  static bool touchOnFace = false;
  // The bridge asked to hear the room: record both microphones (no talking meanwhile)
  if (recReqId && !stId && (mode == IDLE || mode == AWAY)) startStereo();
  if (stId) {
    if (!stUpload) pumpStereo();
    t = m5::touch_detail_t();
  }

  if (WiFi.status() != WL_CONNECTED && now % 10000 < 20) WiFi.reconnect();
  ArduinoOTA.handle();
#if HAS_KVM
  rkmPanelPoll(WiFi.status() == WL_CONNECTED);  // keeps the hub link up
  if (mode == KVM) {  // the desk map has the screen until its back button
    if (!rkmPanelLoop()) {
      mode = IDLE;
      modeSince = now;
      desktopStale = true;
    }
    delay(2);
    return;
  }
#endif

  switch (mode) {
    case IDLE:
      if (bodyEmpty && (presHere || !presKnown)) settleIn(now);  // he's in it again
      if (presKnown && !presHere) {  // he's been called away: off he goes
        mode = AWAY;
        modeSince = now;
        tracking = false;
        lookToward(presAim);
        break;
      }
      if (t.wasPressed()) {
        touchOnFace = t.y < BAR_Y - 4;
        if (touchOnFace) {
          if (!bridgeOk) setNotice("Bridge not found");
          else if (!bridgeStt) setNotice("Voice is off on the bridge");
          else startListening();
        }
      } else if (t.wasClicked() && !touchOnFace) {
        barTap(t.x);
      }
      // Look around now and then
      if (tracking) {  // follow the thing drifting past: eyes lock, head keeps up
        static uint32_t lastObj = now;
        float dt = min(0.1f, (now - lastObj) / 1000.0f);
        lastObj = now;
        objX += objVX * dt;
        objY = objBaseY + sinf((now - objSince) / 600.0f) * SERVO_Y_RANGE * 0.25f;  // bobbing
        lookX = objX;
        lookY = objY;
        lookSince = now - NECK_DELAY;
        head.look(objX, objY);  // the neck can only go so far; the eyes go on
        // Gone too far round for the eyes to follow: let it go, linger there
        if (fabsf(objX - head.x) > SERVO_X_RANGE * 1.15f && fabsf(objX) > SERVO_X_RANGE) {
          tracking = false;
          lookX = constrain(objX, -(float)SERVO_X_RANGE, (float)SERVO_X_RANGE);
          lookY = constrain(objY, -(float)SERVO_Y_RANGE, (float)SERVO_Y_RANGE);
          lookAt = now + 1500 + random(2000);
        }
      } else if (now > lookAt && !stId) {  // (not while the mics listen)
        float x, y;
        int roll = random(100);
        if (roll < 8) {  // something floats by
          startTracking();
        } else if (roll < 63) {  // just a glance with the eyes, then back
          nextSpot(x, y, 0.8f);
          glanceX = x;
          glanceY = y;
          glanceUntil = now + 1200 + random(1500);
          lookAt = glanceUntil + 3000 + random(5000);
        } else {  // eyes first, then the head turns to face it
          nextSpot(x, y, 1.0f);
          lookAtSpot(x, y);
          lookAt = now + 6000 + random(8000);
        }
      }
      if (!tracking && !stId && now - lookSince > NECK_DELAY && now > neckHeldUntil) head.look(lookX, lookY);
      if (lastNeeds >= 0 && needsCount > lastNeeds) {  // a task wants you: glance at its PC (no sound)
        lookToward(tasksAim);
        lookAt = now + 1500;
      }
      lastNeeds = needsCount;
      {  // Claude sent a new picture or video
        xSemaphoreTake(lock, portMAX_DELAY);
        String newest = bridgeMediaNewest;
        xSemaphoreGive(lock);
        if (newest.length() && newest != mediaNewest) {
          if (mediaNewest.length() || mediaSeen.length()) mediaNew = true;
          if (!mediaSeen.length()) mediaSeen = newest;  // first poll after boot
          mediaNewest = newest;
        }
      }
      break;

    case AWAY:
      if (!presKnown) {  // the bridge stopped saying: assume he's home
        mode = IDLE;
        break;
      }
      if (presHere) {  // on his way here: "COMING!", then he slides in
        eyePop = 0;      // (the neck stays as it is: nobody is in it yet)
        if ((int32_t)(now - presArriveAt) >= 0) {
          mode = IDLE;
          modeSince = now;
          settleIn(now);
        }
        if (t.wasClicked() && t.y >= BAR_Y - 4) barTap(t.x);
        break;
      }
      // Leaving: his eyes look first, the neck turns to face where he's
      // going, and only then does he go (his window slides off the top).
      // Once he's gone the body doesn't move at all until he's back.
      if (bodyEmpty) eyePop = 0;  // (back from the task list or pictures: still gone)
      if (eyePop > 0.02f) {
        if (now - modeSince > NECK_DELAY) turnToward(presAim);
        if (now - modeSince > NECK_DELAY + 150 &&
            ((fabsf(head.x - head.targetX) < 5 && fabsf(head.y - head.targetY) < 4) ||
             now - modeSince > 3000))
          eyePop = max(0.0f, eyePop - dt * 2.2f);
        if (eyePop <= 0.02f) leaveBody();
      }
      if (t.wasPressed()) {
        touchOnFace = t.y < BAR_Y - 4;
        if (touchOnFace) {  // call him back
          summonSeq = presSeq;
          summonAt = now;
          summonWanted = true;
          // not gone yet: he just stays; otherwise he's coming
          presArriveAt = now + (eyePop > 0.02f ? 0 : 2800);
          presHere = true;
          xSemaphoreTake(lock, portMAX_DELAY);
          presFrom = presLabel;
          xSemaphoreGive(lock);
          if (eyePop > 0.02f) mode = IDLE;
        }
      } else if (t.wasClicked() && !touchOnFace) {
        barTap(t.x);
      }
      break;

    case MEDIA:
    case VIEW:
    case VIDEO:
      galleryTouch(t, dragLastY);
      break;

    case LISTENING:
      pumpMic();
      head.look(0, SERVO_Y_RANGE * 0.6f);
      if (t.wasReleased() || recLen + REC_CHUNK > REC_MAX) {
        bool longEnough = now - modeSince > 500;
        stopListening(longEnough);
        if (!longEnough && !quietCancel) setNotice("hold the face while you talk");
        quietCancel = false;
      }
      break;

    case THINKING:
      head.look(SERVO_X_RANGE * 0.4f, SERVO_Y_RANGE * 0.5f);
      if (hopActive) {  // off to check on a computer
        mode = HOP;
        hopPhase = HOP_TURN;
        hopSince = now;
        lookToward(hopAim);  // eyes go first
        break;
      }
      // Start talking as soon as the first sentence arrives
      if (chunkCount > 0 || (replyDone && !sendPending)) startReply();
      break;

    case HOP:
      // Turn to look at the machine, "leave" (BE RIGHT BACK!), then return
      if (hopPhase == HOP_TURN) {  // the neck turns first, then he goes
        if (now - hopSince > NECK_DELAY) turnToward(hopAim);
        if (now - hopSince > NECK_DELAY + 150 &&
            ((fabsf(head.x - head.targetX) < 5 && fabsf(head.y - head.targetY) < 4) ||
             now - hopSince > 3000))
          eyePop = max(0.0f, eyePop - dt * 2.2f);
        if (eyePop <= 0.02f) { hopPhase = HOP_AWAY; hopSince = now; leaveBody(); }
      } else if (hopPhase == HOP_AWAY) {
        if (!hopActive) { hopPhase = HOP_BACK; hopSince = now; eyePop = 0; settleIn(now); }
      } else {  // back: his window slides in (into the neck as it is)
        if (now - hopSince > 900) eyePop = min(1.0f, eyePop + dt * 1.8f);
        if (eyePop >= 1 && now - hopSince > 1200) { mode = THINKING; modeSince = now; }
      }
      if (t.wasClicked()) {  // never mind
        cancelJob = sendPending;
        eyePop = 1;
        mode = IDLE;
      }
      break;

    case SPEAKING:
    case REPLY:
      // Tap anywhere: done with this answer. Hold the face: ask a follow-up.
      // Drag the caption: scroll.
      syncReplyText();
      if (mode == SPEAKING) {
        head.look(0, mouthOpen * 4);
        if (!M5.Speaker.isPlaying() && !playNextChunk() && replyDone && !sendPending) {
          mode = REPLY;
          modeSince = now;
          happyUntil = now + 1500;
        }
      }
      if (t.wasPressed()) {
        dragLastY = t.y;
        modeSince = now;
        if (t.y < 150 && bridgeOk && bridgeStt) {
          M5.Speaker.stop();
          startListening();
          quietCancel = true;  // a short tap just closes the answer
        }
      } else if (t.isDragging() && t.base_y >= 150) {
        vfdPos -= t.deltaX() / (float)VFD_CW;  // scrub the display
        vfdPos = max(0.0f, vfdPos);
      } else if (t.wasClicked()) {
        M5.Speaker.stop();
        cancelJob = sendPending;  // stop fetching the rest of the answer
        mode = IDLE;
      }
      if (mode == REPLY && !t.isPressed()) {
        // Marquee: keep scrolling and loop; close after ~20 s, at a loop end
        static uint32_t lastTick = 0;
        float dt = min(0.1f, (now - lastTick) / 1000.0f);
        vfdPos += dt * VFD_CPS;
        if (vfdPos >= vfdPassLength(vfdText)) {
          vfdPos = 0;
          if (now - modeSince > 20000) mode = IDLE;
        }
        lastTick = now;
      }
      break;

    case TASKS:
      if (t.wasPressed()) dragLastY = t.y;
      if (t.isDragging()) {
        tasksScroll += dragLastY - t.y;
        dragLastY = t.y;
      } else if (t.wasClicked() && t.y < 34) {
        mode = IDLE;
      }
      break;
  }
#if HAS_KVM
  if (mode == KVM) return;  // just opened the desk map: it draws from the next pass
#endif

  // the neck pans; big turns (hops, leaving) at a set pace whatever the frame rate
  head.update(mode == HOP || mode == AWAY ? 1 - expf(-dt * 3.0f) : 0.07f);
  if (mode != HOP && mode != AWAY && !bodyEmpty && eyePop < 1)
    eyePop = min(1.0f, eyePop + dt * 1.8f);  // arriving: slide in
  updateMouth();
  if (mode == TASKS) {
    drawTasks();
  } else if (mode == MEDIA) {
    drawMediaList();
  } else if (mode == VIEW) {
    drawView();
  } else if (mode == VIDEO) {
    if (!drawVideo()) closeVideo();
  } else if (mode == HOP && hopPhase == HOP_AWAY) {
    drawBRB(now);
  } else if (mode == AWAY && presHere) {
    drawComing(now);
  } else if (mode == AWAY && eyePop <= 0.02f) {
    drawAway(now);
  } else {
    drawFace(mode == IDLE || mode == AWAY);
    if (mode == SPEAKING || mode == REPLY) {
      drawVFD(vfdText, vfdPos);
      canvas.setFont(&fonts::DejaVu12);
      canvas.setTextColor(0x4208);
      canvas.drawString("tap: done   hold: ask more", 8, 6);
    }
  }
  present();
  delay(10);
}
