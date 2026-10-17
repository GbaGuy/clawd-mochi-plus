/*
 * Clawd Mochi firmware — a living Clawd for ESP32-C3 + ST7789 240x240.
 *
 * Based on Clawd Mochi by Yousuf Amanuel (MIT): https://github.com/yousifamanuel/clawd-mochi
 * Wiring (same as the original), VCC → 3.3V only:
 *   SCL→GPIO8  SDA→GPIO10  RST→GPIO2  DC→GPIO1  CS→GPIO4  BL→GPIO3
 *
 * One command set, two ways in:
 *   USB serial, one command per line   (the PC / Claude Code hooks)
 *   http://192.168.4.1/cmd?c=<command> (WiFi AP "ClaWD-Mochi" / clawd1234, web UI at /)
 *
 * Commands:
 *   mood <idle|think|work|wait|happy|sleep> [caption]
 *   say <text>                 caption for 8s, mood unchanged
 *   bl <0|1>                   backlight
 *   clear <rrggbb>             enter draw mode on a blank colour
 *   stroke <rrggbb> x,y;x,y…   draw a polyline (enters draw mode)
 *   ping                       serial replies "pong <uptime ms>"
 *   snap                       serial replies "snap <base64>": the screen at 120x120 RGB565 (also GET /snap)
 *   state                      serial replies the /state JSON
 *   ip                         serial replies "ip <home wifi ip or ->"
 *   wifi <ssid>\t<password>    join home WiFi too (saved; empty = forget) → http://clawd-mochi.local
 *   time <seconds since midnight>
 *   usage <5h%> <week%> <ctx%> <5h reset HH:MM>   ("-" = unknown); shown under the idle face
 *   timer <seconds> [label]    focus countdown on the device (0 = cancel); celebrates at the end
 *   stats <cpu%> <ram%> <gpu%> <cpu°C> <gpu°C>    PC stats page under the idle face
 *   music <playing|paused|stopped> <title>\t<artist>   now-playing page; Clawd bobs along
 *   art begin | art data <base64 chunk> | art end | art none | art colors <bg rrggbb> <ink rrggbb>
 *   musicpos <position s> <length s>   progress bar (extrapolated while playing)
 *                              128x128 RGB565 (little-endian) album art for the now-playing page
 *   weather <temp> <code> <hi> <lo> <hum%>\t<city>\t<Day> <code> <hi> <lo>\t<Day> <code> <hi> <lo>
 *                              (WMO weather codes, °C as sent)
 *   date <yyyy> <m> <d> [weekStart 0=Sun 1=Mon]   calendar page
 *   events clear | events days <bitmask> | events add <when>\t<title>   (Google Calendar)
 *   notifs clear | notifs add <when>\t<text>    notification history page (send oldest first)
 *   notifs wipe                clear history for good (the feed sees it via "ngen" and forgets too)
 *   page <clawd|clock|usage|timer|music|stats|history|weather|calendar|events|notifs|next|prev|auto>   pin a page, or let them take turns
 *   notify <app>\t<summary>    surprised face + "app: summary" for a few seconds
 */
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "mbedtls/base64.h"
#include "web.h"

#define TFT_CS  4
#define TFT_DC  1
#define TFT_RST 2
#define TFT_BLK 3
#define W 240
#define H 240

// Eye geometry from upstream: two tall black bars in the upper half.
#define EYE_W   30
#define EYE_H   60
#define EYE_GAP 120
#define EYE_CY  80

const char* AP_SSID = "ClaWD-Mochi";
const char* AP_PASS = "clawd1234";

const uint32_t FRAME_MS = 40;              // ~25 fps; one full push is ~23 ms of SPI
const uint32_t HAPPY_MS = 4000;            // celebrate, then idle
const uint32_t STALE_MS = 10UL * 60000;    // busy mood with no updates → host gone → idle
const uint32_t SLEEP_MS = 5UL * 60000;     // idle this long → doze
const uint32_t SAY_MS   = 8000;

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 fb(W, H);   // 115 KB frame buffer: draw everything here, push once → no flicker
WebServer server(80);
Preferences prefs;

enum Mood { IDLE, THINK, WORK, WAIT, HAPPY, SLEEP, DRAW };
const char* MOOD_NAMES[] = {"idle", "think", "work", "wait", "happy", "sleep", "draw"};

uint16_t C_ORANGE, C_ALERT, C_INK;
Mood mood = IDLE;
String caption;
uint32_t lastEvent = 0, captionUntil = 0, nextBlink = 0, blinkEnd = 0;

// Idle life: every few seconds Clawd does a little something.
enum Act { A_GLANCE, A_DOUBLEBLINK, A_HAPPY, A_CURIOUS, A_WIGGLE, A_HOP, A_SUSPICIOUS, A_COUNT };
int act = -1;
uint32_t actStart = 0, actEnd = 0, nextAct = 0;
float actX = 0, actY = 0;
bool backlight = true, drawDirty = false;

// Fed by the PC. clockBase = seconds since midnight at millis() == clockAt.
int32_t clockBase = -1;
uint32_t clockAt = 0, usageAt = 0;
int usage5h = -1, usageWeek = -1, usageCtx = -1;
String usageReset;
const uint32_t USAGE_FRESH_MS = 15UL * 60000;

// Focus timer: counts down on the device itself.
uint32_t focusEnd = 0, focusLen = 0;
String focusLabel;

// PC stats, now playing and notifications, fed by `clawd-mochi feed`.
const uint32_t FEED_FRESH_MS = 60000;   // older than this = feed stopped → hide the page
const uint32_t PAGE_MS = 8000;          // pages take turns
const uint32_t NOTIFY_MS = 6000;
uint32_t statsAt = 0, musicAt = 0, surpriseUntil = 0;
int cpuPct = -1, ramPct = -1, gpuPct = -1, cpuTemp = -1, gpuTemp = -1;
bool musicPlaying = false, musicPaused = false;
// Track position (seconds) at musicPosAt, extrapolated while playing; colours picked from the cover.
int musicPos = -1, musicLen = -1;
uint32_t musicPosAt = 0;
uint16_t musicBg = 0, musicInk = 0;
bool musicColors = false;
String musicTitle, musicArtist;

// Album art arrives in base64 chunks; shown only once a whole image landed.
#define ART 128
uint16_t* art = nullptr;   // malloc'd after the frame buffer (it needs the big contiguous block)
const size_t ART_BYTES = ART * ART * 2;
size_t artFill = 0;
bool artReady = false;

// Pages (see "Pages" below).
enum Page { P_CLAWD, P_CLOCK, P_USAGE, P_TIMER, P_MUSIC, P_STATS, P_HISTORY, P_WEATHER, P_CALENDAR, P_EVENTS, P_NOTIFS, P_COUNT };
const char* PAGE_NAMES[] = {"clawd", "clock", "usage", "timer", "music", "stats", "history", "weather", "calendar", "events", "notifs"};

// Stats history: every 3 stats messages (~15 s) averaged into one sample; 104 samples ≈ 26 min.
#define HIST 104
#define HIST_EVERY 3
uint8_t hist[3][HIST];                 // cpu, ram, gpu %; ring buffer
int histHead = 0, histCount = 0, histAcc[3] = {0, 0, 0}, histN = 0;

// Google Calendar, via the feed: upcoming events and which days this month have any.
#define MAX_EVENTS 4
struct Event { String when, title; } events[MAX_EVENTS];

// Notification history (newest first), kept and re-sent by the feed.
Event notifs[MAX_EVENTS];
int notifCount = 0;
uint32_t notifGen = 0;   // random at boot, +1 per user "wipe": lets the feed tell a wipe from a reboot
int eventCount = 0;
uint32_t eventDays = 0, eventsAt = 0;   // bit d-1 = day d of the current month has an event
const uint32_t EVENTS_FRESH_MS = 3UL * 3600000;

// Today's date from the PC (`date`), re-sent every ~20 s so midnight rolls over.
int calY = 0, calM = 0, calD = 0, calWeekStart = 0;   // weekStart: 0 = Sunday, 1 = Monday

// Weather, fed by `clawd-mochi feed` (Open-Meteo) every 15 minutes.
const uint32_t WEATHER_FRESH_MS = 3UL * 3600000;
uint32_t wxAt = 0;
int wxTemp = 0, wxCode = 0, wxHi = 0, wxLo = 0, wxHum = 0;
String wxCity;
struct Day { String name; int code, hi, lo; } wxDay[2];
enum Sky { SUN, PARTLY, CLOUD, FOG, RAIN, SNOW, STORM };
int pinnedPage = -1;   // -1 = take turns

// Face values ease toward targets each frame.
// tilt: one eye taller than the other (curious); open: eye height as a fraction
struct Face { float x, y, open, tilt; } cur = {0, 0, 1, 0}, tgt = {0, 0, 1, 0};

// Thinking words, in the spirit of Claude Code's spinner; a new one every few seconds.
const char* VERBS[] = {"Thinking", "Pondering", "Purring", "Noodling", "Clauding", "Cogitating", "Musing",
                       "Brewing", "Percolating", "Ruminating", "Simmering", "Mulling", "Vibing", "Whirring",
                       "Marinating", "Conjuring", "Hatching", "Moseying", "Combobulating", "Schlepping"};
const int VERB_COUNT = sizeof(VERBS) / sizeof(VERBS[0]);
int verb = 0;
uint32_t verbAt = 0;

void setMood(Mood m, const String& cap = "") {
  if (m == THINK && mood != THINK) { verb = random(VERB_COUNT); verbAt = millis(); }
  if (m == DRAW && mood != DRAW) fb.fillScreen(C_ORANGE);
  mood = m;
  caption = cap.substring(0, 60);
  captionUntil = 0;
  lastEvent = millis();
  drawDirty = true;
}

uint16_t hex565(const String& hex) {
  long v = strtol(hex.c_str(), nullptr, 16);
  return tft.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

void setBacklight(bool on) {
  backlight = on;
  digitalWrite(TFT_BLK, on ? HIGH : LOW);
}

void drawStroke(uint16_t col, const String& pts) {
  int px = -1, py = -1;
  for (int i = 0; i < (int)pts.length();) {
    int semi = pts.indexOf(';', i);
    if (semi < 0) semi = pts.length();
    int comma = pts.indexOf(',', i);
    if (comma > i && comma < semi) {
      int x = pts.substring(i, comma).toInt(), y = pts.substring(comma + 1, semi).toInt();
      if (px < 0) fb.fillCircle(x, y, 1, col);
      else for (int d = -1; d <= 1; d++) {   // 3 px thick
        fb.drawLine(px + d, py, x + d, y, col);
        fb.drawLine(px, py + d, x, y + d, col);
      }
      px = x; py = y;
    }
    i = semi + 1;
  }
}

// Split `s` on `sep` into up to n fields; missing ones stay empty.
void fields(const String& s, char sep, String* out, int n) {
  for (int i = 0, from = 0; i < n; i++) {
    if (from > (int)s.length()) { out[i] = ""; continue; }
    int to = i == n - 1 ? s.length() : s.indexOf(sep, from);
    if (to < 0) to = s.length();
    out[i] = s.substring(from, to);
    from = to + 1;
  }
}

int num(const String& s) { return s == "-" || !s.length() ? -1 : s.toInt(); }

void wake() {
  lastEvent = millis();
  if (mood == SLEEP) mood = IDLE;
}

String staIp() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "-";
}

// Own AP always on; also joins the saved home network if there is one.
void startWifi() {
  prefs.begin("mochi", true);
  String ssid = prefs.getString("ssid", ""), pass = prefs.getString("pass", "");
  prefs.end();
  WiFi.disconnect();
  WiFi.mode(ssid.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  if (ssid.length()) WiFi.begin(ssid.c_str(), pass.c_str());   // retries on its own
}

// Returns "" on success, else an error message.
String command(String line) {
  line.trim();
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? "" : line.substring(sp + 1);
  int sp2 = arg.indexOf(' ');
  String a1 = sp2 < 0 ? arg : arg.substring(0, sp2);
  String rest = sp2 < 0 ? "" : arg.substring(sp2 + 1);

  if (cmd == "mood") {
    for (int i = 0; i < DRAW; i++)
      if (a1 == MOOD_NAMES[i]) { setMood((Mood)i, rest); return ""; }
    return "unknown mood";
  }
  if (cmd == "say") {
    caption = arg.substring(0, 60);
    captionUntil = millis() + SAY_MS;
    wake();
    return "";
  }
  if (cmd == "timer") {
    long sec = a1.toInt();
    focusLen = sec > 0 ? sec * 1000UL : 0;
    focusEnd = sec > 0 ? millis() + focusLen : 0;
    focusLabel = rest.substring(0, 18);
    wake();
    return "";
  }
  if (cmd == "stats") {
    String f[5];
    fields(arg, ' ', f, 5);
    cpuPct = num(f[0]); ramPct = num(f[1]); gpuPct = num(f[2]); cpuTemp = num(f[3]); gpuTemp = num(f[4]);
    statsAt = millis();
    int cur[3] = {cpuPct, ramPct, gpuPct};
    for (int k = 0; k < 3; k++) histAcc[k] += max(0, cur[k]);
    if (++histN == HIST_EVERY) {
      for (int k = 0; k < 3; k++) { hist[k][histHead] = constrain(histAcc[k] / HIST_EVERY, 0, 100); histAcc[k] = 0; }
      histHead = (histHead + 1) % HIST;
      histCount = min(histCount + 1, HIST);
      histN = 0;
    }
    return "";
  }
  if (cmd == "music") {
    String f[2];
    fields(rest, '\t', f, 2);
    musicPlaying = a1 == "playing";
    musicPaused = a1 == "paused";
    if (musicPlaying) wake();
    musicTitle = f[0].substring(0, 60);
    musicArtist = f[1].substring(0, 60);
    musicAt = millis();
    return "";
  }
  if (cmd == "musicpos") {             // musicpos <position s> <length s>
    String f[2];
    fields(arg, ' ', f, 2);
    musicPos = num(f[0]); musicLen = num(f[1]);
    musicPosAt = millis();
    return "";
  }
  if (cmd == "art" && a1 == "colors") {   // art colors <bg rrggbb> <ink rrggbb>
    String f[2];
    fields(rest, ' ', f, 2);
    musicBg = hex565(f[0]); musicInk = hex565(f[1]);
    musicColors = true;
    return "";
  }
  if (cmd == "art") {
    if (!art) return "no ram for art";
    if (a1 == "begin") artFill = 0;
    else if (a1 == "none") { artReady = false; musicColors = false; }
    else if (a1 == "end") artReady = artFill == ART_BYTES;
    else if (a1 == "data") {
      size_t got = 0;
      if (mbedtls_base64_decode((uint8_t*)art + artFill, ART_BYTES - artFill, &got,
                                (const uint8_t*)rest.c_str(), rest.length()) != 0) return "bad art chunk";
      artFill += got;
    } else return "unknown art command";
    return "";
  }
  if (cmd == "date") {
    String f[4];
    fields(arg, ' ', f, 4);
    int y = f[0].toInt(), m = f[1].toInt(), d = f[2].toInt();
    if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) return "bad date";
    calY = y; calM = m; calD = d; calWeekStart = f[3] == "1" ? 1 : 0;
    return "";
  }
  if (cmd == "events") {               // events clear | events days <mask> | events add <when>\t<title>
    if (a1 == "clear") { eventCount = 0; eventsAt = millis(); }
    else if (a1 == "days") eventDays = strtoul(rest.c_str(), nullptr, 10);
    else if (a1 == "add" && eventCount < MAX_EVENTS) {
      String f[2];
      fields(rest, '\t', f, 2);
      events[eventCount++] = {f[0].substring(0, 38), f[1]};
      eventsAt = millis();
    } else if (a1 != "add") return "unknown events command";
    return "";
  }
  if (cmd == "notifs") {               // notifs clear|wipe | notifs add <when>\t<text>   (oldest → newest)
    if (a1 == "clear") notifCount = 0;                       // feed re-syncing
    else if (a1 == "wipe") { notifCount = 0; notifGen++; }   // user pressed "clear history"
    else if (a1 == "add") {
      String f[2];
      fields(rest, '\t', f, 2);
      for (int i = min(notifCount, MAX_EVENTS - 1); i > 0; i--) notifs[i] = notifs[i - 1];   // newest on top
      notifs[0] = {f[0].substring(0, 38), f[1]};
      notifCount = min(notifCount + 1, MAX_EVENTS);
    } else return "unknown notifs command";
    return "";
  }
  if (cmd == "weather") {
    String part[4], now[5];
    fields(arg, '\t', part, 4);
    fields(part[0], ' ', now, 5);
    wxTemp = now[0].toInt(); wxCode = now[1].toInt(); wxHi = now[2].toInt(); wxLo = now[3].toInt(); wxHum = now[4].toInt();
    wxCity = part[1];
    for (int d = 0; d < 2; d++) {
      String f[4];
      fields(part[2 + d], ' ', f, 4);
      wxDay[d] = {f[0].substring(0, 3), (int)f[1].toInt(), (int)f[2].toInt(), (int)f[3].toInt()};
    }
    wxAt = millis();
    return "";
  }
  if (cmd == "page") {
    uint32_t now = millis();
    if (a1 == "auto") { pinnedPage = -1; return ""; }
    if (a1 == "next" || a1 == "prev") {
      int from = currentPage(now), step = a1 == "next" ? 1 : P_COUNT - 1;
      for (int i = 1; i <= P_COUNT; i++) {
        int p = (from + i * step) % P_COUNT;
        if (pageAvailable(p, now)) { pinnedPage = p; break; }
      }
      wake();
      return "";
    }
    for (int p = 0; p < P_COUNT; p++)
      if (a1 == PAGE_NAMES[p]) { pinnedPage = p; wake(); return ""; }
    return "unknown page";
  }
  if (cmd == "notify") {
    String f[2];
    fields(arg, '\t', f, 2);
    caption = (f[1].length() ? f[0] + ": " + f[1] : f[0]).substring(0, 60);
    captionUntil = millis() + NOTIFY_MS;
    surpriseUntil = millis() + 1500;
    wake();
    return "";
  }
  if (cmd == "wifi") {
    int tab = arg.indexOf('\t');
    prefs.begin("mochi", false);
    prefs.putString("ssid", tab < 0 ? arg : arg.substring(0, tab));
    prefs.putString("pass", tab < 0 ? "" : arg.substring(tab + 1));
    prefs.end();
    startWifi();
    return "";
  }
  if (cmd == "time") { clockBase = arg.toInt(); clockAt = millis(); return ""; }
  if (cmd == "usage") {
    String f[4];
    fields(arg, ' ', f, 4);
    auto pct = [](const String& s) { return s == "-" || !s.length() ? -1 : constrain(s.toInt(), 0, 100); };
    usage5h = pct(f[0]); usageWeek = pct(f[1]); usageCtx = pct(f[2]);
    usageReset = f[3] == "-" ? "" : f[3];
    usageAt = millis();
    return "";
  }
  if (cmd == "bl")    { setBacklight(arg != "0"); return ""; }
  if (cmd == "clear") { setMood(DRAW); fb.fillScreen(hex565(a1)); return ""; }
  if (cmd == "stroke") {
    if (mood != DRAW) setMood(DRAW);
    drawStroke(hex565(a1), rest);
    drawDirty = true;
    return "";
  }
  return "unknown command";
}

// ── Rendering ────────────────────────────────────────────────

void drawCaption(const String& s, int y, int maxLines = 3) {
  fb.setTextSize(2);
  fb.setTextColor(C_INK);
  const int perLine = 18;   // 12 px glyphs at size 2
  for (int i = 0, line = 0; i < (int)s.length() && line < maxLines; line++) {
    String part = s.substring(i, i + perLine);
    if (i + perLine < (int)s.length()) {   // full line with more to come: break at a space
      int brk = part.lastIndexOf(' ');
      if (brk > 0) part = part.substring(0, brk);
    }
    fb.setCursor((W - (int)part.length() * 12) / 2, y + line * 20);
    fb.print(part);
    i += part.length();
    while (i < (int)s.length() && s[i] == ' ') i++;
  }
}

// Happy ">  <" eyes, from upstream.
void drawChevron(int cx, int cy, int arm, int reach, bool right) {
  for (int t = -9; t <= 9; t++) {
    int s = right ? 1 : -1;
    fb.drawLine(cx - s * reach / 2, cy - arm + t, cx + s * reach / 2, cy + t, C_INK);
    fb.drawLine(cx + s * reach / 2, cy + t, cx - s * reach / 2, cy + arm + t, C_INK);
  }
}

// ── Pages ────────────────────────────────────────────────────
// Full-screen pages take turns (PAGE_MS each) or one is pinned with `page <name>`.
// Claude needing you, a notification or a message always brings Clawd back.

bool musicOn(uint32_t now) { return musicPlaying && musicAt && now - musicAt < FEED_FRESH_MS; }
bool musicLoaded(uint32_t now) { return (musicPlaying || musicPaused) && musicAt && now - musicAt < FEED_FRESH_MS; }

bool pageAvailable(int p, uint32_t now) {
  switch (p) {
    case P_CLAWD: return true;
    case P_CLOCK: return clockBase >= 0;
    case P_USAGE: return usageAt && now - usageAt < USAGE_FRESH_MS;
    case P_TIMER: return focusEnd > now || pinnedPage == P_TIMER;   // pinned: shows "ready"
    case P_MUSIC: return musicLoaded(now);
    case P_STATS: return statsAt && now - statsAt < FEED_FRESH_MS;
    case P_HISTORY: return histCount >= 2 || pinnedPage == P_HISTORY;
    case P_WEATHER: return wxAt && now - wxAt < WEATHER_FRESH_MS;
    case P_CALENDAR: return calY > 0;
    case P_EVENTS: return eventCount > 0 && now - eventsAt < EVENTS_FRESH_MS;
    case P_NOTIFS: return notifCount > 0 || pinnedPage == P_NOTIFS;
  }
  return false;
}

int currentPage(uint32_t now) {
  // a "say" message or notification (timed captions) is urgent; a tool-name caption is not
  bool urgent = mood == WAIT || mood == HAPPY || now < surpriseUntil || captionUntil;
  bool busy = mood == THINK || mood == WORK;
  if (mood == SLEEP || urgent || (busy && pinnedPage < 0)) return P_CLAWD;
  if (pinnedPage >= 0 && pageAvailable(pinnedPage, now)) return pinnedPage;
  int avail[P_COUNT], n = 0;
  for (int p = 0; p < P_COUNT; p++) if (pageAvailable(p, now)) avail[n++] = p;
  return avail[(now / PAGE_MS) % n];
}

String mmss(uint32_t ms) {
  uint32_t s = (ms + 999) / 1000;
  char buf[8];
  snprintf(buf, sizeof buf, "%02lu:%02lu", s / 60, s % 60);
  return buf;
}

void drawCentered(const String& s, int y, int size) {
  fb.setTextSize(size);
  fb.setCursor((W - (int)s.length() * 6 * size) / 2, y);
  fb.print(s);
}

// Clawd page / timer page: the face, with the clock + usage or the countdown below.
void renderFace(uint32_t now) {
  const int lx = (W - (EYE_W * 2 + EYE_GAP)) / 2, rx = lx + EYE_W + EYE_GAP;
  uint16_t bg = (mood == WAIT && (now / 600) % 2) ? C_ALERT : C_ORANGE;
  fb.fillScreen(bg);

  if (mood == HAPPY || idleHappy(now)) {
    int bounce = mood == HAPPY ? -abs(sin(now / 160.0)) * 8 : 0;
    drawChevron(lx + EYE_W / 2, EYE_CY + bounce, EYE_H / 2, EYE_W / 2, true);
    drawChevron(rx + EYE_W / 2, EYE_CY + bounce, EYE_H / 2, EYE_W / 2, false);
  } else {
    int ew = EYE_W * (1 + 0.3f * max(0.0f, 1 - cur.open));     // eyes widen a little as they close
    int hl = max(6, (int)(EYE_H * cur.open * (1 + cur.tilt)));
    int hr = max(6, (int)(EYE_H * cur.open * (1 - cur.tilt)));
    int yc = EYE_CY + (int)cur.y, x = (int)cur.x - (ew - EYE_W) / 2;
    fb.fillRect(lx + x, yc - hl / 2, ew, hl, C_INK);
    fb.fillRect(rx + x, yc - hr / 2, ew, hr, C_INK);
  }

  if (mood == THINK) {                       // a mumbling mouth + "Pondering..."
    int open = 3 + (int)(5 * (0.5 + 0.5 * sin(now / 140.0)) * (0.6 + 0.4 * sin(now / 530.0)));
    fb.fillRoundRect(W / 2 - 14 + (int)cur.x / 2, 136 - open / 2, 28, open, open / 2, C_INK);
    if (now - verbAt > 6000) { verb = (verb + 1 + random(VERB_COUNT - 1)) % VERB_COUNT; verbAt = now; }
    String v = String(VERBS[verb]);
    int dots = (now / 400) % 4;
    fb.setTextColor(C_INK);
    fb.setTextSize(2);
    fb.setCursor((W - (int)(v.length() + 3) * 12) / 2, 172);   // centre as if all 3 dots were there
    fb.print(v);
    for (int i = 0; i < dots; i++) fb.print('.');
  } else if (mood == SLEEP) {                // drifting z
    int t = now % 3000;
    fb.setTextColor(C_INK);
    fb.setTextSize(2 + t / 1000);
    fb.setCursor(160 + t / 60, 60 - t / 50);
    fb.print("z");
  }
  if (now < surpriseUntil) {                 // "!" over the head
    fb.fillRect(116, 8, 8, 18, C_INK);
    fb.fillRect(116, 30, 8, 7, C_INK);
  }
  fb.setTextColor(C_INK);
  if (caption.length()) drawCaption(caption, 175);
  if (focusEnd > now) {                      // keep the countdown visible in a corner
    fb.setTextSize(2);
    fb.setCursor(170, 8);
    fb.print(mmss(focusEnd - now));
  }
}

// Timer page: a big countdown inside a progress ring, what it is and when it ends.
void renderTimer(uint32_t now) {
  fb.fillScreen(C_ORANGE);
  fb.setTextColor(C_INK);
  const int cx = W / 2, cy = 116, r = 100;
  bool running = focusEnd > now;
  uint32_t left = running ? focusEnd - now : 0;
  float done = running ? 1.0f - (float)left / focusLen : 0;
  for (int i = 0; i < 120; i++) {                 // ring: thin track, thick done part
    float a = -PI / 2 + i * 2 * PI / 120;
    int x = cx + cos(a) * r, y = cy + sin(a) * r;
    if (i < done * 120) fb.fillCircle(x, y, 6, C_INK);
    else fb.fillCircle(x, y, 1, C_INK);
  }
  drawCentered(running ? (focusLabel.length() ? focusLabel : String("focus")) : String("ready"), cy - 52, 2);
  drawCentered(running ? mmss(left) : String("--:--"), cy - 16, 5);
  if (running && clockBase >= 0) {               // wall-clock time it ends
    uint32_t e = (clockBase + (focusEnd - clockAt) / 1000) % 86400;
    char buf[16];
    snprintf(buf, sizeof buf, "ends %02lu:%02lu", e / 3600, e / 60 % 60);
    drawCentered(buf, cy + 38, 2);
  } else if (!running) {
    drawCentered("start one from", cy + 34, 1);
    drawCentered("the bar or web", cy + 46, 1);
  }
}

// Clock page: the time big, seconds as a thin bar, and the date.
void renderClock(uint32_t now) {
  fb.fillScreen(C_ORANGE);
  fb.setTextColor(C_INK);
  uint32_t s = (clockBase + (now - clockAt) / 1000) % 86400;
  char buf[6];
  snprintf(buf, sizeof buf, "%02lu:%02lu", s / 3600, s / 60 % 60);
  drawCentered(buf, 72, 7);
  fb.fillRect(15, 136, 210 * (s % 60) / 59, 4, C_INK);
  if (calY > 0) {
    static const char* WD[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* MO[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    drawCentered(WD[weekday(calY, calM, calD)], 160, 3);
    drawCentered(String(calD) + " " + MO[calM - 1] + " " + calY, 194, 2);
  }
}

// Music page: the cover big, title and artist under it.
// One line of text that scrolls sideways when it is wider than the screen.
void drawMarquee(const String& s, int y, int size, uint32_t now) {
  fb.setTextSize(size);
  fb.setTextWrap(false);
  int w = s.length() * 6 * size, room = W - 24;
  if (w <= room) {
    fb.setCursor((W - w) / 2, y);
    fb.print(s);
  } else {                                   // pause at the start, then glide, then loop
    int gap = 40, span = w + gap, t = now % (span * 25 + 2000);
    int off = t < 2000 ? 0 : (t - 2000) / 25;
    for (int x = 12 - off; x < W; x += span) { fb.setCursor(x, y); fb.print(s); }
  }
  fb.setTextWrap(true);
}

String mss(int sec) {
  char buf[8];
  snprintf(buf, sizeof buf, "%d:%02d", sec / 60, sec % 60);
  return buf;
}

// Music page: the cover on a background tinted from it, title/artist (scrolling when long),
// a progress bar with times, and a tiny Clawd bobbing along.
void renderMusic(uint32_t now) {
  uint16_t bg = musicColors && artReady ? musicBg : C_ORANGE;
  uint16_t ink = musicColors && artReady ? musicInk : C_INK;
  fb.fillScreen(bg);
  fb.setTextColor(ink);
  const int ax = (W - ART) / 2, ay = 8;
  if (artReady) {
    fb.drawRGBBitmap(ax, ay, art, ART, ART);
  } else {
    fb.drawRect(ax, ay, ART, ART, ink);
    drawCentered("\x0E", ay + ART / 2 - 28, 8);
  }
  drawMarquee(musicTitle, ay + ART + 8, 2, now);
  drawMarquee(musicArtist, ay + ART + 30, 1, now);

  // progress
  const int by = 186, bx = 16, bw = W - 32;
  int pos = musicPos;
  if (pos >= 0 && musicPlaying) pos += (now - musicPosAt) / 1000;
  if (musicLen > 0 && pos >= 0) {
    pos = min(pos, musicLen);
    int fill = bw * pos / musicLen;
    fb.fillRect(bx, by, bw, 2, ink);
    fb.fillRect(bx, by - 1, fill, 4, ink);
    fb.fillCircle(bx + fill, by, 4, ink);
    fb.setTextSize(1);
    fb.setCursor(bx, by + 9);
    fb.print(mss(pos));
    String left = "-" + mss(musicLen - pos);
    fb.setCursor(bx + bw - left.length() * 6, by + 9);
    fb.print(left);
  }

  // tiny Clawd at the bottom, bobbing while it plays
  int bob = musicPlaying ? (int)(3 * sin(now / 250.0)) : 0;
  fb.fillRect(W / 2 - 14, 222 + bob, 6, 12, ink);
  fb.fillRect(W / 2 + 8, 222 - bob, 6, 12, ink);
}

// Stats page: big CPU / RAM / GPU bars with temperatures.
void bigStat(int y, const char* label, int pct, int temp) {
  if (pct < 0) return;
  fb.setTextSize(2);
  fb.setCursor(16, y);
  fb.print(label);
  String v = String(pct) + "%";
  if (temp >= 0) v += "  " + String(temp) + "C";
  fb.setCursor(W - 16 - (int)v.length() * 12, y);
  fb.print(v);
  fb.drawRect(16, y + 22, W - 32, 16, C_INK);
  fb.fillRect(18, y + 24, (W - 36) * pct / 100, 12, C_INK);
}

// Usage page: Claude's 5-hour / weekly / context use, big.
void renderUsage(uint32_t) {
  fb.fillScreen(C_ORANGE);
  fb.setTextColor(C_INK);
  drawCentered("claude usage", 14, 2);
  bigStat(48, "5h", usage5h, -1);
  bigStat(108, "week", usageWeek, -1);
  bigStat(168, "context", usageCtx, -1);
  if (usageReset.length()) drawCentered("5h resets " + usageReset, 214, 1);
}

// History page: CPU / RAM / GPU as filled graphs, oldest left, newest right.
void drawGraph(int k, const char* label, int y, int h) {
  const int x0 = 16, w = 2 * HIST;
  fb.drawRect(x0 - 1, y - 1, w + 2, h + 2, C_INK);
  for (int i = 0; i < histCount; i++) {
    int v = hist[k][(histHead - histCount + i + HIST) % HIST];
    int bh = v * h / 100, x = x0 + w - 2 * (histCount - i);
    fb.fillRect(x, y + h - bh, 2, bh, C_INK);
  }
  int last = hist[k][(histHead - 1 + HIST) % HIST];
  String t = String(label) + " " + last + "%";
  fb.fillRect(x0 + 2, y + 2, t.length() * 12 + 4, 18, C_ORANGE);   // label stays readable over the graph
  fb.setTextSize(2);
  fb.setCursor(x0 + 4, y + 4);
  fb.print(t);
}

void renderHistory(uint32_t) {
  fb.fillScreen(C_ORANGE);
  fb.setTextColor(C_INK);
  if (histCount < 2) { drawCentered("collecting...", 110, 2); return; }
  drawCentered("last " + String((histCount * HIST_EVERY * 5 + 59) / 60) + " min", 8, 2);
  drawGraph(0, "cpu", 32, 56);
  drawGraph(1, "ram", 100, 56);
  drawGraph(2, "gpu", 168, 56);
}

void renderStats(uint32_t) {
  fb.fillScreen(C_ORANGE);
  fb.setTextColor(C_INK);
  drawCentered("this pc", 14, 2);
  bigStat(48, "cpu", cpuPct, cpuTemp);
  bigStat(108, "ram", ramPct, -1);
  bigStat(168, "gpu", gpuPct, gpuTemp);
}

// Weather page: city, a drawn icon, temperature and the next two days.
// WMO weather code → icon kind + words.
Sky skyOf(int code) {
  if (code == 0) return SUN;
  if (code <= 2) return PARTLY;
  if (code == 3) return CLOUD;
  if (code == 45 || code == 48) return FOG;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return SNOW;
  if (code >= 95) return STORM;
  return RAIN;   // drizzle, rain, showers
}
const char* SKY_WORDS[] = {"clear", "partly cloudy", "cloudy", "fog", "rain", "snow", "storm"};

void drawCloud(int cx, int cy, int s, uint16_t col) {
  fb.fillCircle(cx - s, cy, s * 7 / 10, col);
  fb.fillCircle(cx + s * 6 / 10, cy - s / 10, s * 8 / 10, col);
  fb.fillCircle(cx - s / 4, cy - s / 2, s, col);
  fb.fillRoundRect(cx - s * 17 / 10, cy, s * 33 / 10, s * 7 / 10, s / 3, col);
}

void drawSun(int cx, int cy, int r) {
  fb.fillCircle(cx, cy, r, C_INK);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    for (int t = -1; t <= 1; t++)
      fb.drawLine(cx + cos(a) * (r + r / 3) + t, cy + sin(a) * (r + r / 3),
                  cx + cos(a) * (r + r * 4 / 5) + t, cy + sin(a) * (r + r * 4 / 5), C_INK);
  }
}

// s = size unit (~ icon half-width / 1.7)
void drawSky(Sky k, int cx, int cy, int s) {
  switch (k) {
    case SUN: drawSun(cx, cy, s); return;
    case PARTLY:
      drawSun(cx + s * 7 / 10, cy - s * 6 / 10, s * 7 / 10);
      drawCloud(cx - s / 5, cy + s / 3, s * 12 / 10, C_ORANGE);   // cut a gap around the cloud
      drawCloud(cx - s / 5, cy + s / 3, s, C_INK);
      return;
    case FOG:
      for (int i = 0; i < 3; i++) fb.fillRoundRect(cx - s * 3 / 2 + (i % 2) * s / 3, cy - s / 2 + i * s / 2, s * 8 / 3, s / 4 + 1, 2, C_INK);
      return;
    default: break;
  }
  drawCloud(cx, cy - s / 4, s, C_INK);
  int by = cy + s * 3 / 4;
  if (k == RAIN) for (int i = -1; i <= 1; i++)
    for (int t = 0; t < 3; t++) fb.drawLine(cx + i * s * 2 / 3 + t, by, cx + i * s * 2 / 3 - s / 4 + t, by + s * 2 / 3, C_INK);
  if (k == SNOW) for (int i = -1; i <= 1; i++) fb.fillCircle(cx + i * s * 2 / 3, by + s / 3, max(2, s / 6), C_INK);
  if (k == STORM) {
    fb.fillTriangle(cx + s / 4, by - s / 6, cx - s / 3, by + s / 2, cx + s / 10, by + s / 2, C_INK);
    fb.fillTriangle(cx - s / 10, by + s / 3, cx + s / 3, by + s / 3, cx - s / 4, by + s * 11 / 10, C_INK);
  }
}

void printDeg(int v, int size) {   // "21°": the classic font has no usable degree sign
  fb.print(v);
  int x = fb.getCursorX(), y = fb.getCursorY();
  fb.drawCircle(x + size * 2, y + size * 2, size + 1, C_INK);
  fb.drawCircle(x + size * 2, y + size * 2, size, C_INK);
  fb.setCursor(x + size * 6, y);
}

void renderWeather(uint32_t) {
  fb.fillScreen(C_ORANGE);
  fb.setTextColor(C_INK);
  drawCentered(wxCity.substring(0, 18), 10, 2);
  drawSky(skyOf(wxCode), 66, 74, 26);
  fb.setTextSize(5);
  fb.setCursor(124, 52);
  printDeg(wxTemp, 5);
  drawCentered(SKY_WORDS[skyOf(wxCode)], 120, 2);
  String hl = "H " + String(wxHi) + "  L " + String(wxLo) + "  hum " + String(wxHum) + "%";
  drawCentered(hl, 144, 1);
  fb.drawFastHLine(20, 160, W - 40, C_INK);
  for (int d = 0; d < 2; d++) {                 // next two days
    if (!wxDay[d].name.length()) continue;
    int cx = d ? 180 : 60;
    fb.setTextSize(2);
    fb.setCursor(cx - (int)wxDay[d].name.length() * 6, 168);
    fb.print(wxDay[d].name);
    drawSky(skyOf(wxDay[d].code), cx - 30, 204, 10);
    fb.setTextSize(1);
    fb.setCursor(cx - 8, 196);
    fb.print(wxDay[d].hi); fb.print(" / "); fb.print(wxDay[d].lo);
  }
}

// Calendar page: this month's grid, today highlighted.
int weekday(int y, int m, int d) {   // 0 = Sunday (Sakamoto)
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

int daysIn(int y, int m) {
  static const int n[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return m == 2 && leap ? 29 : n[m - 1];
}

void renderCalendar(uint32_t) {
  static const char* MONTHS[] = {"January", "February", "March", "April", "May", "June", "July",
                                 "August", "September", "October", "November", "December"};
  static const char* DAYS = "SMTWTFS";
  fb.fillScreen(C_ORANGE);
  fb.setTextColor(C_INK);
  drawCentered(String(MONTHS[calM - 1]) + " " + calY, 10, 2);

  const int cw = 32, x0 = (W - 7 * cw) / 2, rh = 28, y0 = 50;
  fb.setTextSize(1);
  for (int c = 0; c < 7; c++) {
    fb.setCursor(x0 + c * cw + cw / 2 - 3, 36);
    fb.print(DAYS[(c + calWeekStart) % 7]);
  }
  fb.drawFastHLine(x0, 46, 7 * cw, C_INK);

  int col = (weekday(calY, calM, 1) - calWeekStart + 7) % 7, row = 0;
  for (int d = 1; d <= daysIn(calY, calM); d++) {
    int cx = x0 + col * cw, cy = y0 + row * rh;
    String s(d);
    if (d == calD) {
      fb.fillRoundRect(cx + 2, cy, cw - 4, rh - 4, 6, C_INK);
      fb.setTextColor(C_ORANGE);
    }
    fb.setTextSize(2);
    fb.setCursor(cx + (cw - (int)s.length() * 12) / 2, cy + 4);
    fb.print(s);
    if (eventDays & (1UL << (d - 1)))                  // has events: a bar under the number
      fb.fillRect(cx + cw / 2 - 5, cy + rh - 7, 10, 2, d == calD ? C_ORANGE : C_INK);
    fb.setTextColor(C_INK);
    if (++col == 7) { col = 0; row++; }
  }
}

// List pages (events, notifications): a small "when" line over a big title, four rows.
void renderList(const char* heading, Event* items, int n, const char* empty) {
  fb.fillScreen(C_ORANGE);
  fb.setTextColor(C_INK);
  drawCentered(heading, 10, 2);
  fb.drawFastHLine(20, 32, W - 40, C_INK);
  if (!n) drawCentered(empty, 110, 2);
  for (int i = 0; i < n; i++) {
    int y = 42 + i * 48;
    fb.setTextSize(1);
    fb.setCursor(14, y);
    fb.print(items[i].when);
    fb.setTextSize(2);
    fb.setCursor(14, y + 12);
    fb.print(items[i].title.substring(0, 18));
  }
}

void render(uint32_t now) {
  int page = currentPage(now);
  if (page == P_MUSIC) renderMusic(now);
  else if (page == P_STATS) renderStats(now);
  else if (page == P_HISTORY) renderHistory(now);
  else if (page == P_USAGE) renderUsage(now);
  else if (page == P_WEATHER) renderWeather(now);
  else if (page == P_CALENDAR) renderCalendar(now);
  else if (page == P_EVENTS) renderList("coming up", events, eventCount, "nothing coming up");
  else if (page == P_NOTIFS) renderList("notifications", notifs, notifCount, "all quiet");
  else if (page == P_TIMER) renderTimer(now);
  else if (page == P_CLOCK) renderClock(now);
  else renderFace(now);
}

bool idleHappy(uint32_t now) { return mood == IDLE && act == A_HAPPY && now < actEnd; }

// Idle: breathe, and now and then do something — glance, double blink, happy squint,
// curious head-tilt, wiggle, hop, or a suspicious side-eye. Sleepy late at night.
void idleLife(uint32_t now) {
  tgt.y = 2 * sin(now / 900.0);                    // breathing
  tgt.open = 1 + 0.03 * sin(now / 900.0);
  if (now > nextAct) {
    act = random(A_COUNT);
    static const uint16_t DUR[] = {1500, 450, 1600, 1800, 700, 500, 2200};
    actStart = now;
    actEnd = now + DUR[act] + (act == A_GLANCE ? random(0, 900) : 0);
    nextAct = actEnd + random(2500, 8000);
    actX = random(-28, 29);
    actY = random(-12, 11);
  }
  if (now < actEnd) {
    float t = (now - actStart) / (float)(actEnd - actStart);   // 0..1 through the act
    switch (act) {
      case A_GLANCE: tgt.x = actX; tgt.y = actY; break;
      case A_DOUBLEBLINK: {
        uint32_t ms = now - actStart;
        if (ms < 110 || (ms > 230 && ms < 340)) { tgt.open = cur.open = 0.05; }
        break;
      }
      case A_CURIOUS: tgt.x = 10; tgt.y = -4; tgt.tilt = 0.22; break;
      case A_WIGGLE: tgt.x = 12 * sin((now - actStart) / 55.0); break;
      case A_HOP: cur.y = tgt.y = -18 * sin(PI * t); break;          // no easing: a crisp hop
      case A_SUSPICIOUS: tgt.open = 0.4; tgt.x = -24 + 48 * t; break;
      default: break;                                              // A_HAPPY is drawn as > <
    }
  }
  if (clockBase >= 0) {                                            // sleepy late at night
    uint32_t h = (clockBase + (now - clockAt) / 1000) % 86400 / 3600;
    if (h >= 23 || h < 6) tgt.open *= 0.6;
  }
}

// Pick where the eyes want to be for this mood.
void updateTargets(uint32_t now) {
  tgt = {0, 0, 1};
  switch (mood) {
    case IDLE:
      idleLife(now);
      if (focusEnd > now) tgt = {0, 8, 0.75};                     // focused
      else if (musicPlaying && now - musicAt < FEED_FRESH_MS)
        tgt.y = 6 * sin(now / 250.0);                             // bob along
      break;
    case THINK: tgt = {14, -12, 0.85}; break;
    case WORK:  tgt = {(float)(20 * sin(now / 700.0)), 10, 0.7}; break;
    case WAIT:  tgt = {0, -4, 1.15}; break;
    case SLEEP: tgt = {0, 6, 0.08}; break;
    default: break;
  }
  if (now < surpriseUntil) { tgt.open = 1.3; tgt.y = -8; return; }   // wide-eyed, no blink
  if (mood != SLEEP && mood != HAPPY && !(act == A_HAPPY && now < actEnd)) {
    if (now > nextBlink) { blinkEnd = now + 110; nextBlink = now + random(2000, 6000); }
    if (now < blinkEnd) { tgt.open = 0.05; cur.open = 0.05; }   // blinks snap shut
  }
}

void push() {
  tft.startWrite();
  tft.setAddrWindow(0, 0, W, H);
  tft.writePixels(fb.getBuffer(), W * H);
  tft.endWrite();
}

void tick(uint32_t now) {
  // Mood timeouts.
  if (mood == HAPPY && now - lastEvent > HAPPY_MS) setMood(IDLE);
  if ((mood == THINK || mood == WORK || mood == WAIT) && now - lastEvent > STALE_MS) setMood(IDLE);
  if (mood == IDLE && now - lastEvent > SLEEP_MS && !musicOn(now) && focusEnd <= now) mood = SLEEP;
  if (captionUntil && now > captionUntil) { caption = ""; captionUntil = 0; }
  if (focusEnd && now >= focusEnd) {
    focusEnd = 0;
    if (mood != DRAW) setMood(HAPPY, focusLabel.length() ? focusLabel + " done!" : "time's up!");
  }

  if (mood == DRAW) {
    if (drawDirty) { push(); drawDirty = false; }
    return;
  }
  updateTargets(now);
  cur.x += (tgt.x - cur.x) * 0.25;
  cur.y += (tgt.y - cur.y) * 0.25;
  cur.open += (tgt.open - cur.open) * 0.35;
  cur.tilt += (tgt.tilt - cur.tilt) * 0.2;
  render(now);
  push();
}

// ── Inputs ───────────────────────────────────────────────────

// Screen mirror for the controllers: the frame buffer at half size (120x120 RGB565,
// little-endian), each pixel the average of a 2x2 block.
#define SNAP 120
void snapRow(int y, uint8_t* out) {
  uint16_t* b = fb.getBuffer();
  for (int x = 0; x < SNAP; x++) {
    uint32_t r = 0, g = 0, bl = 0;
    for (int dy = 0; dy < 2; dy++)
      for (int dx = 0; dx < 2; dx++) {
        uint16_t p = b[(y * 2 + dy) * W + x * 2 + dx];
        r += p >> 11; g += (p >> 5) & 63; bl += p & 31;
      }
    uint16_t p = ((r / 4) << 11) | ((g / 4) << 5) | (bl / 4);
    out[2 * x] = p & 0xFF;
    out[2 * x + 1] = p >> 8;
  }
}

// Serial: "snap <base64 of the 120x120 image>" on one line (rows are 240 bytes, a multiple of 3,
// so each row encodes on its own without padding).
void sendSnap() {
  uint8_t row[SNAP * 2];
  unsigned char enc[SNAP * 2 * 4 / 3 + 4];
  size_t n;
  Serial.setTxTimeoutMs(1000);  // a reader is waiting for this one: don't drop bytes
  Serial.print("snap ");
  for (int y = 0; y < SNAP; y++) {
    snapRow(y, row);
    mbedtls_base64_encode(enc, sizeof enc, &n, row, sizeof row);
    Serial.write(enc, n);
  }
  Serial.println();
  Serial.setTxTimeoutMs(0);
}

void pollSerial() {
  static String rx;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      rx.trim();
      if (rx == "ping") Serial.printf("pong %lu\n", millis());
      else if (rx == "state") Serial.println(stateJson());
      else if (rx == "snap") sendSnap();
      else if (rx == "ip") Serial.printf("ip %s\n", staIp().c_str());
      else if (rx.length()) command(rx);
      rx = "";
    } else if (rx.length() < 2048) {
      rx += c;
    }
  }
}

void setupWeb() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", WEB_PAGE); });
  server.on("/cmd", HTTP_GET, [] {
    String err = command(server.arg("c"));
    server.send(err.length() ? 400 : 200, "text/plain", err.length() ? err : "ok");
  });
  server.on("/state", HTTP_GET, [] { server.send(200, "application/json", stateJson()); });
  server.on("/snap", HTTP_GET, [] {
    uint8_t* buf = (uint8_t*)malloc(SNAP * SNAP * 2);
    if (!buf) { server.send(503, "text/plain", "no ram"); return; }
    for (int y = 0; y < SNAP; y++) snapRow(y, buf + y * SNAP * 2);
    server.send_P(200, "application/octet-stream", (const char*)buf, SNAP * SNAP * 2);
    free(buf);
  });
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
  server.begin();
}

// Live state for the controller page (web /state, serial "state").
String stateJson() {
  bool fresh = usageAt && millis() - usageAt < USAGE_FRESH_MS;
  String cap = caption;
  cap.replace("\\", "\\\\");
  cap.replace("\"", "\\\"");
  String j = String("{\"mood\":\"") + MOOD_NAMES[mood] + "\",\"bl\":" + (backlight ? 1 : 0)
    + ",\"cap\":\"" + cap + "\",\"ip\":\"" + staIp() + "\""
    + ",\"u5\":" + (fresh ? usage5h : -1) + ",\"uw\":" + (fresh ? usageWeek : -1)
    + ",\"uc\":" + (fresh ? usageCtx : -1) + ",\"reset\":\"" + (fresh ? usageReset : "") + "\""
    + ",\"art\":" + (artReady ? 1 : 0) + ",\"ngen\":" + notifGen
    + ",\"page\":\"" + PAGE_NAMES[currentPage(millis())] + "\",\"pin\":\"" + (pinnedPage < 0 ? "auto" : PAGE_NAMES[pinnedPage]) + "\"" + ",\"timer\":" + (focusEnd > millis() ? (long)((focusEnd - millis()) / 1000) : -1) + "}";
  return j;
}

void setup() {
  Serial.setRxBufferSize(8192);   // album art arrives in bursts
  Serial.setTxBufferSize(4096);   // screen mirror replies are 38 KB; a 256 B buffer drops bytes
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never stall the face when nobody is reading USB

  pinMode(TFT_BLK, OUTPUT);
  setBacklight(true);
  SPI.begin(8, -1, 10, TFT_CS);   // SCK=8, MOSI=10
  tft.init(W, H);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);

  C_ORANGE = tft.color565(218, 17, 0);
  C_ALERT  = tft.color565(255, 120, 0);
  C_INK    = ST77XX_BLACK;

  if (!fb.getBuffer()) {   // no RAM for the frame buffer: say so instead of a black screen
    tft.fillScreen(C_ORANGE);
    tft.setCursor(10, 110); tft.setTextSize(2); tft.print("no RAM for fb");
    for (;;) delay(1000);
  }
  art = (uint16_t*)malloc(ART_BYTES);   // no room → music page just shows the note

  startWifi();
  MDNS.begin("clawd-mochi");
  MDNS.addService("http", "tcp", 80);
  setupWeb();

  // Boot card: how to reach the web UI.
  fb.fillScreen(C_ORANGE);
  fb.setTextColor(C_INK);
  fb.setTextSize(3); fb.setCursor(66, 50);  fb.print("Clawd");
  fb.setCursor(66, 80); fb.print("Mochi");
  fb.setTextSize(2);
  fb.setCursor(12, 140); fb.print("WiFi ClaWD-Mochi");
  fb.setCursor(12, 164); fb.print("pass clawd1234");
  fb.setCursor(12, 188); fb.print("http://192.168.4.1");
  push();
  delay(3000);

  randomSeed(esp_random());
  notifGen = esp_random() >> 1;
  setMood(IDLE);
}

void loop() {
  static uint32_t lastFrame = 0;
  pollSerial();
  server.handleClient();
  uint32_t now = millis();
  if (now - lastFrame >= FRAME_MS) { lastFrame = now; tick(now); }
  delay(1);
}
