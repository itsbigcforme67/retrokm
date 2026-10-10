// Gallery: pictures and videos Claude sent in your chats, served by the
// bridge already shrunk to fit (96x72 thumbs, 320x240 stills, 10 fps video).
// Included from main.cpp after the shared state and HTTP helpers.
#pragma once

struct MediaItem {
  String id, kind, caption, title;
  int ago;
  uint8_t* thumb = nullptr;
  int thumbLen = 0;
  bool thumbTried = false;
};

static std::vector<MediaItem> media;   // guarded by lock
static volatile bool mediaWanted = false;
static String mediaSeen, mediaNewest;  // newest id seen in the gallery / on bridge
static bool mediaNew = false;
static int mediaScroll = 0, viewIdx = 0;
static bool viewCaption = true;

static M5Canvas photo(&M5.Display);
static volatile bool viewWanted = false, viewReady = false;
static String viewId;

// Video: chunks of VCH frames, three in flight
static const int VCH = 10, VSLOTS = 3, VSLOT_CAP = 320 * 1024;
struct VSlot {
  volatile int chunk = -1;
  uint8_t* buf = nullptr;
  int count = 0;
  int offs[VCH], lens[VCH];
};
static VSlot vslots[VSLOTS];
static volatile bool videoLoadWanted = false, videoReady = false, videoFailed = false;
static volatile int vCurChunk = 0;
static String videoId;
static int vFrames = 0, vFps = 10, vShown = -1;
static int16_t* vAudio = nullptr;
static int vAudioLen = 0;
static uint32_t vStart = 0;

// ------------------------------------------------------------ network side

// Reads a GET body into dst (cap bytes) or a fresh PSRAM buffer; returns length
static int getBytes(const String& path, uint8_t** dst, int cap) {
  HTTPClient http;
  if (!httpBegin(http, path)) return -1;
  int got = -1;
  if (http.GET() == 200) {
    int len = http.getSize();
    if (len > 0 && (cap == 0 || len <= cap)) {
      if (cap == 0) *dst = (uint8_t*)ps_malloc(len);
      if (*dst) {
        WiFiClient* s = http.getStreamPtr();
        got = 0;
        uint32_t t0 = millis();
        while (got < len && millis() - t0 < 15000) {
          int n = s->read(*dst + got, len - got);
          if (n > 0) got += n; else delay(1);
        }
      }
    }
  }
  http.end();
  return got;
}

static void fetchMedia() {
  JsonDocument doc;
  if (!getJson("/api/media", doc)) return;
  std::vector<MediaItem> fresh;
  for (JsonObject o : doc["items"].as<JsonArray>()) {
    MediaItem m;
    m.id = o["id"] | "";
    m.kind = o["kind"] | "";
    m.caption = o["caption"] | "";
    m.title = o["title"] | "";
    m.ago = o["ago"] | 0;
    fresh.push_back(m);
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  for (auto& old : media) {  // keep thumbnails we already have
    bool kept = false;
    for (auto& m : fresh)
      if (m.id == old.id) { m.thumb = old.thumb; m.thumbLen = old.thumbLen; m.thumbTried = old.thumbTried; kept = true; }
    if (!kept && old.thumb) free(old.thumb);
  }
  media.swap(fresh);
  xSemaphoreGive(lock);
}

static bool fetchOneThumb() {
  String id;
  xSemaphoreTake(lock, portMAX_DELAY);
  for (auto& m : media)
    if (!m.thumbTried) { m.thumbTried = true; id = m.id; break; }
  xSemaphoreGive(lock);
  if (!id.length()) return false;
  uint8_t* buf = nullptr;
  int len = getBytes("/api/media/" + id + "/thumb", &buf, 0);
  xSemaphoreTake(lock, portMAX_DELAY);
  bool used = false;
  for (auto& m : media)
    if (m.id == id && len > 0) { m.thumb = buf; m.thumbLen = len; used = true; }
  xSemaphoreGive(lock);
  if (!used && buf) free(buf);
  return true;
}

static void fetchView() {
  uint8_t* buf = nullptr;
  int len = getBytes("/api/media/" + viewId + "/image", &buf, 0);
  if (len > 0) photo.drawJpg(buf, len, 0, 0);
  else photo.fillScreen(TFT_BLACK);
  if (buf) free(buf);
  viewReady = true;
}

static bool loadChunk(int chunk) {
  VSlot& s = vslots[chunk % VSLOTS];
  if (s.chunk == chunk) return true;
  s.chunk = -1;
  int len = getBytes("/api/media/" + videoId + "/frames/" + String(chunk * VCH) +
                     "/" + String(VCH), &s.buf, VSLOT_CAP);
  if (len <= 0) return false;
  s.count = 0;
  for (int p = 0; p + 4 <= len && s.count < VCH;) {
    uint32_t n;
    memcpy(&n, s.buf + p, 4);
    if (p + 4 + (int)n > len) break;
    s.offs[s.count] = p + 4;
    s.lens[s.count++] = n;
    p += 4 + n;
  }
  s.chunk = chunk;
  return true;
}

static void prepareVideo() {
  videoFailed = false;
  JsonDocument doc;
  uint32_t t0 = millis();
  for (;;) {  // the bridge converts the video on first request
    doc.clear();
    if (getJson("/api/media/" + videoId + "/video", doc) && (doc["ready"] | false)) break;
    if (millis() - t0 > 180000 || !videoLoadWanted) { videoFailed = true; return; }
    delay(800);
  }
  vFrames = doc["frames"] | 0;
  vFps = doc["fps"] | 10;
  int audioBytes = doc["audio"] | 0;
  if (vAudio) { free(vAudio); vAudio = nullptr; }
  vAudioLen = 0;
  if (audioBytes > 0) {
    uint8_t* a = nullptr;
    int len = getBytes("/api/media/" + videoId + "/audio", &a, 0);
    if (len > 0) { vAudio = (int16_t*)a; vAudioLen = len / 2; }
  }
  for (auto& s : vslots) s.chunk = -1;
  vCurChunk = 0;
  if (!loadChunk(0) || vFrames == 0) { videoFailed = true; return; }
  loadChunk(1);
  videoReady = true;
}

// Called from the network task loop
static void galleryNet() {
  if (mediaWanted) {
    mediaWanted = false;
    fetchMedia();
  }
  if (viewWanted) {
    viewWanted = false;
    fetchView();
  }
  if (videoLoadWanted && !videoReady && !videoFailed) prepareVideo();
  if (videoReady) {
    int c = vCurChunk;
    for (int k = c; k < c + VSLOTS && k * VCH < vFrames; k++)
      if (!loadChunk(k)) break;
  } else if (mode == MEDIA) {
    fetchOneThumb();
  }
}

// ------------------------------------------------------------ screens

static void drawHeader(const String& text) {
  canvas.fillRect(0, 0, W, 32, 0x10A2);
  canvas.setFont(&fonts::DejaVu18);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(text, 8, 7);
}

static void drawPlayBadge(int x, int y) {
  canvas.fillCircle(x, y, 13, 0x0000);
  canvas.fillTriangle(x - 4, y - 7, x - 4, y + 7, x + 7, y, TFT_WHITE);
}

static const int MROW = 78, MTOP = 34;

static void drawMediaList() {
  canvas.fillScreen(TFT_BLACK);
  xSemaphoreTake(lock, portMAX_DELAY);
  int maxScroll = max(0, (int)media.size() * MROW - (H - MTOP));
  mediaScroll = constrain(mediaScroll, 0, maxScroll);
  for (size_t i = 0; i < media.size(); i++) {
    int y = MTOP + i * MROW - mediaScroll;
    if (y < MTOP - MROW || y > H) continue;
    const MediaItem& m = media[i];
    if (m.thumb) canvas.drawJpg(m.thumb, m.thumbLen, 4, y + 2);
    else canvas.fillRect(4, y + 2, 96, 72, 0x2104);
    if (m.kind == "video") drawPlayBadge(52, y + 38);
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextColor(TFT_WHITE);
    std::vector<String> lines = wrap(m.caption, W - 112);
    for (size_t k = 0; k < lines.size() && k < 3; k++) {
      String l = lines[k];
      if (k == 2 && lines.size() > 3) l += "...";
      canvas.drawString(l, 108, y + 4 + k * 15);
    }
    canvas.setTextColor(TFT_DARKGREY);
    String sub = m.title + "  " + agoText(m.ago);
    while (sub.length() > 3 && canvas.textWidth(sub) > W - 112) sub.remove(sub.length() - 1);
    canvas.drawString(sub, 108, y + 54);
  }
  bool empty = media.empty();
  xSemaphoreGive(lock);
  if (empty) {
    canvas.setFont(&fonts::DejaVu18);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString(bridgeOk ? "No pictures yet" : "Bridge not found", 70, 110);
  }
  drawHeader("< Pictures from Claude");
}

static void drawView() {
  if (!viewReady) {
    canvas.fillScreen(TFT_BLACK);
    canvas.setFont(&fonts::DejaVu18);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString("loading...", 115, 110);
    return;
  }
  photo.pushSprite(&canvas, 0, 0);
  if (!viewCaption) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  String cap = viewIdx < (int)media.size() ? media[viewIdx].caption : "";
  int count = media.size();
  xSemaphoreGive(lock);
  canvas.setFont(&fonts::DejaVu12);
  std::vector<String> lines = wrap(cap, W - 16);
  int n = min((int)lines.size(), 4);
  canvas.fillRect(0, H - 6 - n * 15, W, 6 + n * 15, 0x0000);
  canvas.setTextColor(TFT_WHITE);
  for (int k = 0; k < n; k++) canvas.drawString(lines[k], 8, H - 3 - (n - k) * 15);
  canvas.fillRect(0, 0, W, 22, 0x0000);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.drawString("< back", 6, 5);
  String pos = String(viewIdx + 1) + "/" + String(count) + "  swipe for more";
  canvas.drawString(pos, W - 6 - canvas.textWidth(pos), 5);
}

// Draws the current video frame; returns false when the video has ended
static bool drawVideo() {
  if (!videoReady) {
    canvas.fillScreen(TFT_BLACK);
    canvas.setFont(&fonts::DejaVu18);
    canvas.setTextColor(TFT_DARKGREY);
    canvas.drawString(videoFailed ? "Couldn't load that video" : "getting video ready...",
                      videoFailed ? 50 : 70, 110);
    return true;
  }
  if (!vStart) {
    vStart = millis();
    if (vAudio) M5.Speaker.playRaw(vAudio, vAudioLen, 16000, false, 1, 0);
  }
  int f = (millis() - vStart) * vFps / 1000;
  if (f >= vFrames) return false;
  vCurChunk = f / VCH;
  VSlot& s = vslots[(f / VCH) % VSLOTS];
  if (f != vShown && s.chunk == f / VCH && f % VCH < s.count) {
    canvas.drawJpg(s.buf + s.offs[f % VCH], s.lens[f % VCH], 0, 0);
    vShown = f;
  }
  canvas.fillRect(0, H - 3, W * f / vFrames, 3, TFT_RED);  // progress
  return true;
}

// ------------------------------------------------------------ control

static void openGallery() {
  mode = MEDIA;
  mediaScroll = 0;
  mediaWanted = true;
  mediaSeen = mediaNewest;
  mediaNew = false;
}

static void openItem(int idx) {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (idx < 0 || idx >= (int)media.size()) { xSemaphoreGive(lock); return; }
  String id = media[idx].id, kind = media[idx].kind;
  xSemaphoreGive(lock);
  viewIdx = idx;
  if (kind == "video") {
    for (auto& s : vslots)
      if (!s.buf) s.buf = (uint8_t*)ps_malloc(VSLOT_CAP);
    videoId = id;
    videoReady = videoFailed = false;
    vStart = 0;
    vShown = -1;
    videoLoadWanted = true;
    canvas.fillScreen(TFT_BLACK);
    mode = VIDEO;
  } else {
    viewId = id;
    viewReady = false;
    viewWanted = true;
    mode = VIEW;
  }
}

static void closeVideo() {
  M5.Speaker.stop();
  videoLoadWanted = false;
  videoReady = false;
  mode = MEDIA;
}

// Touch handling for the gallery screens; t is this frame's touch detail
static void galleryTouch(const m5::touch_detail_t& t, int& dragLastY) {
  if (mode == MEDIA) {
    if (t.wasPressed()) dragLastY = t.y;
    if (t.isDragging()) {
      mediaScroll += dragLastY - t.y;
      dragLastY = t.y;
    } else if (t.wasClicked()) {
      if (t.y < MTOP) mode = IDLE;
      else openItem((t.y - MTOP + mediaScroll) / MROW);
    }
  } else if (mode == VIEW) {
    if (t.wasFlicked() && abs(t.distanceX()) > 40) {
      int n;
      xSemaphoreTake(lock, portMAX_DELAY);
      n = media.size();
      xSemaphoreGive(lock);
      int next = viewIdx + (t.distanceX() < 0 ? 1 : -1);
      if (next >= 0 && next < n) openItem(next);
    } else if (t.wasClicked()) {
      if (t.y < 30 && viewCaption) mode = MEDIA;
      else viewCaption = !viewCaption;
    }
  } else if (mode == VIDEO) {
    if (t.wasClicked()) closeVideo();
  }
}
