// Claude Code session-status display
// Board variant: ESP32-C3 SuperMini + 0.42" OLED
//   - 72x40 SSD1306 OLED (I2C SDA=GPIO5, SCL=GPIO6)
//   - SINGLE-COLOR LED on GPIO8 (active-LOW) — NOT an RGB WS2812.
//     Status is conveyed by BLINK PATTERN, not color.
//
// Just three states, matching what you actually care about:
//   work -> screen "WORK", LED breathing        Claude is doing something
//   perm -> screen "CONFIRM" pulsing, LED blink  needs YOUR action (so you notice)
//   off  -> screen + LED fully OFF (dark)        idle / finished / not running
//
// Protocol over USB-serial, one line per update:  STATE|COUNT\n
// STATE is work | perm | off (any unknown token is treated as off). COUNT is how
// many sessions share this state (a small badge is shown when >=2). The OLED shows
// a single big word only for work/perm; off powers the panel fully DOWN so an idle
// device is completely dark — no glow, no burn-in. The BOOT button (GPIO9)
// acknowledges a CONFIRM: the pulse calms to a steady dim (and stays calm).

#include <Arduino.h>
#include <U8g2lib.h>

// The transport is chosen at build time (see platformio.ini): the default USB
// build talks over Serial; the WiFi build (-D CS_TRANSPORT_WIFI) serves an HTTP
// endpoint and remembers multiple networks. Everything below the transport
// section is shared, byte-for-byte, by both builds.
#if CS_TRANSPORT_WIFI
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <esp_random.h>
// Defined in the transport section but referenced earlier (debugger / handleLine).
static String getToken();
static void   setToken(String t);
static int    credsCount();
static void   credsForgetAll();
static void   startPortal();
#endif

// ---- pins -----------------------------------------------------------------
static const uint8_t PIN_SDA = 5;
static const uint8_t PIN_SCL = 6;
static const uint8_t PIN_LED = 8;           // single-color onboard LED
static const uint8_t PIN_BTN = 9;           // BOOT button, active LOW
static const bool    LED_ACTIVE_LOW = true; // LED lights when GPIO8 is driven LOW

// ---- LED PWM (LEDC, Arduino core 2.x channel API) -------------------------
static const int     LED_CH   = 0;
static const int     LED_FREQ = 2000;       // Hz
static const int     LED_BITS = 8;          // -> duty 0..255

// ---- animation timing / levels (ms, 0..255) ------------------------------
static const unsigned long PERM_BLINK_MS = 240;   // full CONFIRM pulse cycle (~4 Hz).
                                                  // Drives BOTH the LED blink and the
                                                  // OLED invert, so they pulse in lockstep.
static const uint8_t       PERM_ACK_LVL  = 40;    // dim glow after BOOT ack
static const unsigned long WORK_BREATH_MS = 1800; // breathing cycle
static const uint8_t       WORK_MIN_LVL  = 12;
static const uint8_t       WORK_SPAN_LVL = 170;   // peak = MIN + SPAN
static const unsigned long PERM_TIMEOUT_MS = 60000; // auto-calm a CONFIRM after this
                                                  // long with no new attention, so a
                                                  // missed clear-event (Ctrl+C, or the
                                                  // dead VSCode Notification hook) can
                                                  // never leave it blinking forever
static const size_t        RX_MAX        = 120;   // serial line-buffer cap (protocol lines are short)
static const unsigned long LOOP_MS       = 15;    // main-loop tick

// ---- OLED -----------------------------------------------------------------
static const int OLED_W = 72;
static const int OLED_H = 40;
static const int TEXT_MARGIN = 2;                 // px slack when fitting the word to the width
static const int BADGE_Y     = 9;                 // baseline (px) of the top-right count badge
U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);

enum State { S_OFF, S_WORK, S_PERM };

static State state     = S_OFF;           // start dark until Claude does something
static int   sessCount = 1;               // sessions sharing the displayed state
static bool  dirty     = true;            // OLED redraw needed
static bool  ack       = false;           // BOOT pressed -> calm the alert
static bool  permHigh  = false;           // current CONFIRM pulse phase (LED bright + screen inverted)
static bool  lastPermHigh = false;        // previous tick's permHigh (to redraw only on an edge)
static bool  oledOn    = true;            // OLED panel power (setPowerSave); off in the off state
static unsigned long alarmStart = 0;       // millis() when the CURRENT active (un-acked) CONFIRM
                                           // began; the pulse phase origin AND the auto-calm start
static bool  alarmArmed = false;           // is a perm alarm currently counting toward calm?
static bool  lastBtn   = HIGH;
static String rx;

// --- debugger bookkeeping --------------------------------------------------
static String        lastLine  = "";   // last protocol line actually handled
static unsigned long lineCount = 0;     // protocol lines received since boot

// Fonts to try, largest first; pick the biggest whose word fits the width.
static const uint8_t* const FONTS[] = {
  u8g2_font_profont22_tr, u8g2_font_profont17_tr,
  u8g2_font_profont15_tr, u8g2_font_profont12_tr,
};
static const int N_FONTS = sizeof(FONTS) / sizeof(FONTS[0]);

static const char* stateWord(State s) {
  switch (s) {
    case S_WORK:  return "WORK";
    case S_PERM:  return "CONFIRM";
    default:      return "off";             // never rendered (panel is dark); debug-only
  }
}

// The wire token that maps to this state (inverse of parseState). Used only by
// the debugger so its readout matches the protocol word the host actually sent.
static const char* wireToken(State s) {
  switch (s) {
    case S_WORK:  return "work";
    case S_PERM:  return "perm";
    default:      return "off";
  }
}

// Does this state light the screen (and the LED)? Only work/perm; off is dark.
static bool displayActive(State s) {
  return s == S_WORK || s == S_PERM;
}

// perceived LED brightness 0..255 for the current state at time `now`. The CONFIRM
// blink phase is measured from alarmStart (not raw millis()) so the pulse begins
// deterministically at the bright peak the instant the alarm arms, and stays in
// lockstep with the OLED invert (which reads the same permHigh phase).
static uint8_t ledLevel(State s, unsigned long now) {
  switch (s) {
    case S_PERM:
      if (ack) return PERM_ACK_LVL;
      return (((now - alarmStart) % PERM_BLINK_MS) < PERM_BLINK_MS / 2) ? 255 : 0;
    case S_WORK: {
      unsigned long t  = now % WORK_BREATH_MS;
      unsigned long up = (t < WORK_BREATH_MS / 2) ? t : (WORK_BREATH_MS - t);
      return (uint8_t)(WORK_MIN_LVL + up * WORK_SPAN_LVL / (WORK_BREATH_MS / 2));
    }
    default:      return 0;                 // off
  }
}

// --- CONFIRM alarm control -------------------------------------------------
// A single pair of helpers owns both `ack` and the auto-calm timer so they can
// never drift out of sync (the earlier bug: two separate code paths set the
// timestamp, and an identical-resend path skipped it, so the failsafe fired
// against a stale start time and the LED calmed almost immediately).
//
// armAlarm(): start (or restart) an urgent, un-acked CONFIRM and its 60s timer.
// calmAlarm(): stop the urgency (BOOT press or the failsafe) — steady dim glow.
static void armAlarm(unsigned long now) {
  ack        = false;
  alarmArmed = true;
  alarmStart = now;                 // also the pulse phase origin: the blink starts
                                    // at its bright peak here, so every fresh CONFIRM
                                    // begins in the same deterministic phase.
}
static void calmAlarm() {
  ack        = true;
  alarmArmed = false;
}

static void applyLed(unsigned long now) {
  uint8_t b = ledLevel(state, now);
  ledcWrite(LED_CH, LED_ACTIVE_LOW ? (255 - b) : b);
}

static void draw() {
  // The off state powers the OLED fully down, so an idle device is completely dark
  // — matching the LED, which is also off there.
  if (!displayActive(state)) {
    if (oledOn) { u8g2.setPowerSave(1); oledOn = false; }
    return;
  }
  if (!oledOn) { u8g2.setPowerSave(0); oledOn = true; }

  const char* w = stateWord(state);
  bool invert = permHigh;   // full-screen invert, in lockstep with the LED blink

  u8g2.clearBuffer();
  if (invert) u8g2.drawBox(0, 0, OLED_W, OLED_H);
  u8g2.setDrawColor(invert ? 0 : 1);

  const uint8_t* font = FONTS[N_FONTS - 1];
  for (int i = 0; i < N_FONTS; i++) {
    u8g2.setFont(FONTS[i]);
    if (u8g2.getStrWidth(w) <= OLED_W - TEXT_MARGIN) { font = FONTS[i]; break; }
  }
  u8g2.setFont(font);

  int tw   = u8g2.getStrWidth(w);
  int x    = (OLED_W - tw) / 2;
  if (x < 0) x = 0;
  int asc  = u8g2.getAscent();
  int desc = u8g2.getDescent();
  int y    = asc + (OLED_H - (asc - desc)) / 2;
  u8g2.drawStr(x, y, w);

  // session-count badge (top-right) when >1 session shares this on-screen state.
  // draw() has already returned for the off state, so any rendered state qualifies.
  if (sessCount >= 2) {
    u8g2.setFont(u8g2_font_6x10_tr);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", sessCount);
    int bw = u8g2.getStrWidth(buf);
    u8g2.drawStr(OLED_W - bw, BADGE_Y, buf);
  }

  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
}

// --- serial debugger -------------------------------------------------------
// Lets a host "see" the device without eyes on it: dumps the internal logic
// state AND the real OLED framebuffer as ASCII art (72x40, '#'=lit pixel). This
// is the exact buffer last handed to the panel, so it shows precisely what is
// (or, when the panel is powered down, what was last) rendered — including the
// CONFIRM invert-flash and the session-count badge. Triggered by a plain serial
// line (never a STATE|COUNT update), so it can't perturb the displayed state.
//   dump | screen | ?   -> full state header + framebuffer
//   stat | status       -> state header only
static void printDebug(bool withFrame) {
  unsigned long now = millis();
  Serial.println();
  Serial.println(F("=== claude-status debug ==="));
  Serial.print(F("fw        : ")); Serial.println(F(__DATE__ " " __TIME__));
  Serial.print(F("uptime_ms : ")); Serial.println(now);
  Serial.print(F("state     : wire=")); Serial.print(wireToken(state));
  Serial.print(F(" word="));            Serial.print(stateWord(state));
  Serial.print(F(" enum="));            Serial.println((int)state);
  Serial.print(F("count     : ")); Serial.println(sessCount);
  Serial.print(F("ack       : ")); Serial.println(ack ? F("true") : F("false"));
  Serial.print(F("alarmArmed: ")); Serial.println(alarmArmed ? F("true") : F("false"));
  if (alarmArmed) {
    long rem = (long)PERM_TIMEOUT_MS - (long)(now - alarmStart);
    Serial.print(F("alarm_rem : ")); Serial.print(rem < 0 ? 0 : rem);
    Serial.println(F(" ms"));
  }
  Serial.print(F("perm_high : ")); Serial.println(permHigh ? F("true") : F("false"));
  Serial.print(F("panel     : ")); Serial.println(oledOn ? F("ON") : F("OFF (physically dark)"));
  Serial.print(F("led_level : ")); Serial.println(ledLevel(state, now));
  Serial.print(F("last_line : ")); Serial.println(lastLine.length() ? lastLine : String(F("(none)")));
  Serial.print(F("lines_rx  : ")); Serial.println(lineCount);
#if CS_TRANSPORT_WIFI
  Serial.print(F("wifi_ssid : ")); Serial.println(WiFi.isConnected() ? WiFi.SSID() : String(F("(not connected)")));
  Serial.print(F("wifi_ip   : ")); Serial.println(WiFi.localIP().toString());
  Serial.print(F("wifi_rssi : ")); Serial.println(WiFi.isConnected() ? String(WiFi.RSSI()) + F(" dBm") : String(F("-")));
  Serial.print(F("mdns      : ")); Serial.println(F("claude-status.local"));
  Serial.print(F("known_nets: ")); Serial.println(credsCount());
  Serial.print(F("token     : ")); Serial.println(getToken());
#endif
  if (withFrame) {
    Serial.print(F("screen    : 72x40 ('#'=lit, '.'=dark"));
    Serial.println(oledOn ? F(")") : F("; panel powered down, so nothing is physically visible)"));
    const uint8_t* p = u8g2.getBufferPtr();
    const int      stride = u8g2.getBufferTileWidth() * 8;   // bytes per tile row (== 72 here)
    char row[OLED_W + 1];
    row[OLED_W] = '\0';
    for (int y = 0; y < OLED_H; y++) {
      const uint8_t* base = p + (size_t)(y >> 3) * stride;   // tile row for this y
      const uint8_t  bit  = y & 7;                           // LSB = topmost pixel of the tile
      for (int x = 0; x < OLED_W; x++)
        row[x] = ((base[x] >> bit) & 1) ? '#' : '.';
      Serial.println(row);
    }
  }
  Serial.println(F("=== end ==="));
  Serial.flush();
}

static State parseState(const String& s) {
  if (s == "work") return S_WORK;
  if (s == "perm") return S_PERM;
  return S_OFF;                   // "off" and any unknown/legacy token -> dark
}

// Parse one protocol line: "STATE|COUNT" (COUNT optional).
static void handleLine(String line) {
  line.trim();
  if (!line.length()) return;

  // Debug commands (bidirectional inspection over the same link). Handled before
  // any state parsing so they never change what's displayed. Case-insensitive.
  String low = line;
  low.toLowerCase();
  if (low == "dump" || low == "screen" || low == "?") { printDebug(true);  return; }
  if (low == "stat" || low == "status")               { printDebug(false); return; }
#if CS_TRANSPORT_WIFI
  // WiFi-build maintenance over the same USB link (used by install.sh --wifi to
  // read the auth token, and for hands-on network management via the debugger).
  if (low == "token")           { Serial.print(F("token=")); Serial.println(getToken()); return; }
  if (low.startsWith("token ")) { setToken(line.substring(6)); Serial.print(F("token=")); Serial.println(getToken()); return; }
  if (low == "wifi")            { printDebug(false); return; }   // wifi info lives in the header
  if (low == "forget")          { credsForgetAll(); Serial.println(F("networks forgotten; restarting")); Serial.flush(); delay(200); ESP.restart(); }
  if (low == "reprovision")     { Serial.println(F("entering setup portal")); Serial.flush(); startPortal(); }
#endif

  lastLine = line;
  lineCount++;

  int bar = line.indexOf('|');
  String st = (bar >= 0) ? line.substring(0, bar) : line;
  String ct = (bar >= 0) ? line.substring(bar + 1) : "";
  st.trim(); ct.trim();

  State newState = parseState(st);
  int   newCount = ct.toInt();
  if (newCount < 1) newCount = 1;

  // Identical resend? The aggregator re-emits the current winning state whenever
  // ANY session fires a hook, so a stuck CONFIRM (e.g. after Ctrl+C, which fires
  // no hook to clear it) would otherwise get re-armed on every unrelated event.
  // Ignoring identical lines keeps a BOOT ack "sticky": once you calm the alert
  // it stays calm until the state/count actually changes. NOTE: this early return
  // must NOT re-arm the alarm — an identical CONFIRM resend is not new attention, so
  // its auto-calm timer keeps counting from the ORIGINAL arm, exactly as intended.
  if (newState == state && newCount == sessCount) return;

  // Re-arm the alert (drop a BOOT ack, restart the blink) only for genuinely NEW
  // attention: a changed state, or MORE sessions now in the state. A count
  // DECREASE (e.g. one of two pending CONFIRMs ended) must not re-blink an
  // already-acked alert — nothing new needs you.
  bool reArm = (newState != state) || (newCount > sessCount);

  state = newState;
  sessCount = newCount;
  if (newState == S_PERM) {
    // Genuinely new attention (state change, or more sessions) (re)arms the alarm
    // AND restarts its auto-calm timer. A non-re-arm change into/within perm (e.g.
    // a count DECREASE) leaves an existing ack untouched — nothing new needs you.
    if (reArm) armAlarm(millis());
  } else {
    // Left CONFIRM entirely: drop any pending alarm so it can't linger or re-fire.
    alarmArmed = false;
  }
  dirty = true;                // always redraw (the badge count may have changed)
}

// ==========================================================================
//  Transport layer
//  The "brain" above (state model, draw(), LED, debugger) is identical in every
//  build; only HOW a "STATE|COUNT" line arrives differs. transportBegin() is
//  called once from setup(); transportPoll() every loop and funnels received
//  lines into handleLine() — the single shared entry point.
// ==========================================================================

// Serial line reader — shared by both transports. The USB build uses it for
// state + debug; the WiFi build keeps it for the debugger over the still-present
// USB link (and for the token/network maintenance commands in handleLine()).
static void serialPoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rx.length()) { handleLine(rx); rx = ""; }
    } else {
      rx += c;
      if (rx.length() > RX_MAX) rx.remove(0, rx.length() - RX_MAX);
    }
  }
}

#if CS_TRANSPORT_WIFI
// ---- WiFi transport -------------------------------------------------------
static const char*         AP_NAME          = "claude-setup";    // captive-portal SSID
static const char*         MDNS_NAME        = "claude-status";   // -> claude-status.local
static const int           MAX_NETS         = 5;                 // remembered networks
static const unsigned long PORTAL_HOLD_MS   = 2000;              // BOOT hold -> re-provision
static const unsigned long CONNECT_MS       = 15000;             // boot connect budget
static const int           PORTAL_TIMEOUT_S = 180;               // portal gives up after this
static const unsigned long INFO_MS          = 4000;              // how long the "online" screen shows

static Preferences   prefs;
static WiFiMulti     wifiMulti;
static WebServer     server(80);
static String        tokenStr;
static unsigned long btnDownAt     = 0;   // millis() of the current BOOT press (for long-press)
static unsigned long infoUntil     = 0;   // show the connect-info screen until this millis()
static bool          portalPending = false;  // an HTTP /reprovision asked to enter the portal

// -- auth token (guards POST /state) ----------------------------------------
static String genToken() {
  char buf[17];
  snprintf(buf, sizeof(buf), "%08x%08x", (unsigned)esp_random(), (unsigned)esp_random());
  return String(buf);
}
static String getToken() { return tokenStr; }
static void   setToken(String t) { t.trim(); if (t.length()) { tokenStr = t; prefs.putString("token", tokenStr); } }

// -- credential store (NVS): remembers up to MAX_NETS networks, most-recent last.
//    This is what makes the device behave like a phone — it keeps every network
//    you've set up and auto-joins whichever is in range (via WiFiMulti). ----------
static int  credsCount() { int n = prefs.getInt("n", 0); return n < 0 ? 0 : (n > MAX_NETS ? MAX_NETS : n); }
static void credsRead(int i, String& ssid, String& pass) {
  ssid = prefs.getString(("ssid" + String(i)).c_str(), "");
  pass = prefs.getString(("pass" + String(i)).c_str(), "");
}
static void credsWriteAll(String ss[], String ps[], int n) {
  for (int i = 0; i < MAX_NETS; i++) {
    prefs.remove(("ssid" + String(i)).c_str());
    prefs.remove(("pass" + String(i)).c_str());
  }
  for (int i = 0; i < n; i++) {
    prefs.putString(("ssid" + String(i)).c_str(), ss[i]);
    prefs.putString(("pass" + String(i)).c_str(), ps[i]);
  }
  prefs.putInt("n", n);
}
// Add (or refresh) a network. Dedups by SSID (re-adding updates the password and
// moves it to most-recent); evicts the OLDEST when full. Old networks are KEPT, so
// re-provisioning to a new WiFi never forgets the ones you already use.
static void credsAdd(String ssid, String pass) {
  if (!ssid.length()) return;
  String ss[MAX_NETS], ps[MAX_NETS];
  int n = credsCount(), m = 0;
  for (int i = 0; i < n; i++) {
    String s, p; credsRead(i, s, p);
    if (!s.length() || s == ssid) continue;      // drop blanks and any existing copy
    ss[m] = s; ps[m] = p; m++;
  }
  if (m >= MAX_NETS) {                            // full: forget the oldest
    for (int i = 1; i < m; i++) { ss[i - 1] = ss[i]; ps[i - 1] = ps[i]; }
    m = MAX_NETS - 1;
  }
  ss[m] = ssid; ps[m] = pass; m++;
  credsWriteAll(ss, ps, m);
}
static void credsForgetAll() { String ss[MAX_NETS], ps[MAX_NETS]; credsWriteAll(ss, ps, 0); }

// -- transient OLED screens for provisioning / first connect ----------------
static void drawSetup() {
  u8g2.setPowerSave(0); oledOn = true;
  u8g2.clearBuffer(); u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_profont15_tr);
  u8g2.drawStr((OLED_W - u8g2.getStrWidth("SETUP")) / 2, 15, "SETUP");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr((OLED_W - u8g2.getStrWidth("join wifi:")) / 2, 27, "join wifi:");
  u8g2.drawStr((OLED_W - u8g2.getStrWidth(AP_NAME)) / 2, 37, AP_NAME);
  u8g2.sendBuffer();
}
static void drawInfo() {
  u8g2.setPowerSave(0); oledOn = true;
  u8g2.clearBuffer(); u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 9, "online");
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(0, 20, WiFi.localIP().toString().c_str());
  u8g2.drawStr(0, 30, "claude-status");
  u8g2.drawStr(0, 38, ".local");
  u8g2.sendBuffer();
}

// -- captive portal: WiFiManager captures ONE network, we APPEND it to our store
//    (keeping the others) and reboot so WiFiMulti picks it up cleanly. ----------
static void startPortal() {
  server.stop();
  MDNS.end();
  ledcWrite(LED_CH, LED_ACTIVE_LOW ? (255 - PERM_ACK_LVL) : PERM_ACK_LVL);  // steady dim = "set me up"
  WiFiManager wm;
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setAPCallback([](WiFiManager*) { drawSetup(); });
  bool ok = wm.startConfigPortal(AP_NAME);
  if (ok) {                                        // connected -> creds are known
    String s = WiFi.SSID(), p = WiFi.psk();
    if (!s.length()) s = wm.getWiFiSSID();
    if (!p.length()) p = wm.getWiFiPass();
    credsAdd(s, p);
  }
  delay(300);
  ESP.restart();                                   // clean slate either way
}

// -- HTTP endpoint ----------------------------------------------------------
static bool authed() {
  if (server.hasArg("t")        && server.arg("t")        == tokenStr) return true;
  if (server.hasHeader("X-Auth") && server.header("X-Auth") == tokenStr) return true;
  return false;
}
static String statusHtml() {
  String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>claude-status</title>"
               "<body style='font-family:sans-serif;max-width:32em;margin:2em auto'><h2>claude-status</h2>");
  h += "<p><b>state:</b> " + String(wireToken(state)) + " (" + String(stateWord(state)) + "), count " + String(sessCount) + "</p>";
  h += "<p><b>ip:</b> " + WiFi.localIP().toString() + " &nbsp; <b>rssi:</b> " + String(WiFi.RSSI()) + " dBm</p>";
  h += F("<p><b>known networks:</b><br>");
  int n = credsCount();
  for (int i = 0; i < n; i++) { String s, p; credsRead(i, s, p); h += "&nbsp;&bull; " + s + "<br>"; }
  h += "</p><p style='color:#888'>fw " + String(F(__DATE__ " " __TIME__)) + "</p>";
  h += F("<p style='color:#888'>POST /state (needs token) drives the display; "
         "POST /reprovision adds a network; POST /forget clears them.</p></body>");
  return h;
}
static void setupRoutes() {
  server.on("/state", HTTP_POST, []() {
    if (!authed()) { server.send(401, "text/plain", "unauthorized\n"); return; }
    // The "STATE|COUNT" line is the request body (Content-Type: text/plain, as the
    // hook sends it). Fall back to a `?m=` query arg so a plain curl — whose default
    // urlencoded content-type would otherwise swallow the body — still works.
    String body = server.arg("plain");
    if (!body.length()) body = server.arg("m");
    handleLine(body);
    server.send(200, "text/plain", "ok\n");
  });
  server.on("/reprovision", HTTP_POST, []() {
    if (!authed()) { server.send(401, "text/plain", "unauthorized\n"); return; }
    server.send(200, "text/plain", "entering setup portal\n");
    portalPending = true;                          // enter the portal from loop(), after replying
  });
  server.on("/forget", HTTP_POST, []() {
    if (!authed()) { server.send(401, "text/plain", "unauthorized\n"); return; }
    credsForgetAll();
    server.send(200, "text/plain", "forgotten; restarting\n");
    delay(200); ESP.restart();
  });
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", statusHtml()); });
  server.onNotFound([]() { server.send(404, "text/plain", "not found\n"); });
  static const char* hdrs[] = { "X-Auth" };
  server.collectHeaders(hdrs, 1);
}

static void transportBegin() {
  prefs.begin("claude-status", false);
  tokenStr = prefs.getString("token", "");
  if (!tokenStr.length()) { tokenStr = genToken(); prefs.putString("token", tokenStr); }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  int n = credsCount();
  for (int i = 0; i < n; i++) { String s, p; credsRead(i, s, p); if (s.length()) wifiMulti.addAP(s.c_str(), p.c_str()); }

  bool connected = false;
  if (n > 0) {
    u8g2.setPowerSave(0); oledOn = true;           // brief "wifi..." while we join
    u8g2.clearBuffer(); u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr((OLED_W - u8g2.getStrWidth("wifi...")) / 2, 23, "wifi...");
    u8g2.sendBuffer();
    unsigned long start = millis();
    while (millis() - start < CONNECT_MS) {
      if (wifiMulti.run() == WL_CONNECTED) { connected = true; break; }
      delay(200);
    }
  }
  if (!connected) { startPortal(); return; }       // no known net in range -> portal (never returns)

  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);
  setupRoutes();
  server.begin();
  drawInfo();
  infoUntil = millis() + INFO_MS;
}

static void transportPoll() {
  serialPoll();
  server.handleClient();
}

#else   // ---- USB transport (default) -------------------------------------
static void transportBegin() { /* Serial is already up from setup() */ }
static void transportPoll()  { serialPoll(); }
#endif

void setup() {
  Serial.begin(115200);
  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setContrast(255);
  ledcSetup(LED_CH, LED_FREQ, LED_BITS);
  ledcAttachPin(PIN_LED, LED_CH);
  applyLed(millis());
  pinMode(PIN_BTN, INPUT_PULLUP);
  draw();
  transportBegin();     // USB: no-op. WiFi: join a known network (or open the portal).
}

void loop() {
  transportPoll();                  // serial (+ HTTP, in the WiFi build) -> handleLine()

  // One timestamp for the whole tick, read AFTER transport handling so a just-armed
  // alarm's phase origin is never in the future (which would corrupt the modulo).
  unsigned long now = millis();

  bool btn = digitalRead(PIN_BTN);
  if (lastBtn == HIGH && btn == LOW) {             // press edge: ack a CONFIRM (unchanged)
    calmAlarm(); dirty = true;
#if CS_TRANSPORT_WIFI
    btnDownAt = now;
#endif
  }
  lastBtn = btn;

#if CS_TRANSPORT_WIFI
  // Hold BOOT for PORTAL_HOLD_MS to re-provision (add another WiFi, keeping the
  // ones already stored); or an HTTP /reprovision set portalPending. Either way
  // startPortal() blocks in the captive portal and then restarts the device.
  if ((btn == LOW && (now - btnDownAt) >= PORTAL_HOLD_MS) || portalPending) startPortal();
#endif

  // Failsafe: auto-calm a CONFIRM that has been screaming too long. A missed
  // clear-event (Ctrl+C fires no hook; the VSCode Notification hook is dead) must
  // never strand the device in a forever-blink. This does what a BOOT press would
  // — drops to the steady dim glow — but keeps state == S_PERM so the info stays.
  // Gated on alarmArmed (not a bare timestamp) so it can ONLY fire against a timer
  // that armAlarm() actually started this alarm — never a stale/zero start value.
  if (alarmArmed && (now - alarmStart) > PERM_TIMEOUT_MS) {
    calmAlarm();
    dirty = true;
  }

  // CONFIRM pulse phase: LED bright + screen inverted for the first half of each
  // cycle. Both the LED (via ledLevel) and the OLED invert (via draw) read this
  // same phase off the same `now`, so they blink in perfect lockstep. Redraw only
  // on a phase edge (not every tick) to keep the invert flashing while idle.
  permHigh = (state == S_PERM && !ack) &&
             (((now - alarmStart) % PERM_BLINK_MS) < (PERM_BLINK_MS / 2));
  if (permHigh != lastPermHigh) dirty = true;
  lastPermHigh = permHigh;

  applyLed(now);                    // LED refreshed every loop (breathing/blink)

#if CS_TRANSPORT_WIFI
  // Hold the transient "online" info screen for a few seconds after connecting,
  // then hand back to the normal state render (which, at boot, is off -> dark).
  if (infoUntil) {
    if (now >= infoUntil) { infoUntil = 0; dirty = true; }
    else { delay(LOOP_MS); return; }
  }
#endif

  if (dirty) { draw(); dirty = false; }
  delay(LOOP_MS);
}
