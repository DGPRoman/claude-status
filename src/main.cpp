// Claude Code session-status display
// Board variant: ESP32-C3 SuperMini + 0.42" OLED
//   - 72x40 SSD1306 OLED (I2C SDA=GPIO5, SCL=GPIO6)
//   - SINGLE-COLOR LED on GPIO8 (active-LOW) — NOT an RGB WS2812.
//     Status is conveyed by BLINK PATTERN, not color.
//
// Protocol over USB-serial, one line per update:  STATE|COUNT\n
//   work  -> slow breathing pulse     Claude is working
//   perm  -> fast urgent blink        needs your permission/confirmation
//   done  -> off                      finished responding
//   idle  -> off                      waiting for your next prompt
//   start -> steady medium            session started
//   end   -> off                      session ended
// COUNT is how many sessions share this state (badge shown when >=2).
// The OLED shows a single big word. The BOOT button (GPIO9) acknowledges an
// alert: the fast blink + screen flash calm down to a steady dim.
//
// Screen power: states with no LED activity (done/idle/end/boot) also power the
// OLED fully DOWN, so an idle device is completely dark — no glow, no burn-in —
// until Claude next needs you. WORK/CONFIRM/START wake the screen back on.

#include <Arduino.h>
#include <U8g2lib.h>

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
static const unsigned long PERM_BLINK_MS = 240;   // full urgent-blink cycle (~4 Hz)
static const uint8_t       PERM_ACK_LVL  = 40;    // dim glow after BOOT ack
static const unsigned long WORK_BREATH_MS = 1800; // breathing cycle
static const uint8_t       WORK_MIN_LVL  = 12;
static const uint8_t       WORK_SPAN_LVL = 170;   // peak = MIN + SPAN
static const uint8_t       START_LVL     = 100;   // steady medium
static const unsigned long FLASH_MS      = 400;   // OLED invert-flash half-period
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

enum State { S_BOOT, S_WORK, S_PERM, S_DONE, S_IDLE, S_START, S_END };

static State state     = S_BOOT;
static int   sessCount = 1;               // sessions sharing the displayed state
static bool  dirty     = true;            // OLED redraw needed
static bool  ack       = false;           // BOOT pressed -> calm the alert
static bool  flashOn   = false;           // OLED invert-flash phase for perm
static bool  oledOn    = true;            // OLED panel power (setPowerSave); off in quiescent states
static unsigned long lastFlash = 0;
static unsigned long alarmStart = 0;       // millis() when the CURRENT active (un-acked)
                                           // CONFIRM alarm began; used only for auto-calm
static bool  alarmArmed = false;           // is a perm alarm currently counting toward calm?
static bool  lastBtn   = HIGH;
static String rx;

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
    case S_DONE:  return "DONE";
    case S_IDLE:  return "READY";
    case S_START: return "START";
    case S_END:   return "IDLE";
    default:      return "claude";
  }
}

// Does this state light the screen? Mirrors the LED: WORK/CONFIRM/START are the
// "something is happening / needs you" states; everything else leaves the OLED
// powered down so an idle device is fully dark.
static bool displayActive(State s) {
  return s == S_WORK || s == S_PERM || s == S_START;
}

// perceived LED brightness 0..255 for the current state at time `now`
static uint8_t ledLevel(State s, unsigned long now) {
  switch (s) {
    case S_PERM:
      if (ack) return PERM_ACK_LVL;
      return (now % PERM_BLINK_MS < PERM_BLINK_MS / 2) ? 255 : 0;
    case S_WORK: {
      unsigned long t  = now % WORK_BREATH_MS;
      unsigned long up = (t < WORK_BREATH_MS / 2) ? t : (WORK_BREATH_MS - t);
      return (uint8_t)(WORK_MIN_LVL + up * WORK_SPAN_LVL / (WORK_BREATH_MS / 2));
    }
    case S_START: return START_LVL;
    default:      return 0;                 // done / idle / end / boot: off
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
  alarmStart = now;
}
static void calmAlarm() {
  ack        = true;
  alarmArmed = false;
}

static void applyLed() {
  uint8_t b = ledLevel(state, millis());
  ledcWrite(LED_CH, LED_ACTIVE_LOW ? (255 - b) : b);
}

static void draw() {
  // Quiescent states (done/idle/end/boot) power the OLED fully down, so an idle
  // device is completely dark — matching the LED, which is also off for these.
  if (!displayActive(state)) {
    if (oledOn) { u8g2.setPowerSave(1); oledOn = false; }
    return;
  }
  if (!oledOn) { u8g2.setPowerSave(0); oledOn = true; }

  const char* w = stateWord(state);
  bool invert = (state == S_PERM && flashOn && !ack);   // full-screen flash

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

  // session-count badge (top-right) for active states shared by >1 session
  if (sessCount >= 2 && (state == S_WORK || state == S_PERM || state == S_DONE)) {
    u8g2.setFont(u8g2_font_6x10_tr);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", sessCount);
    int bw = u8g2.getStrWidth(buf);
    u8g2.drawStr(OLED_W - bw, BADGE_Y, buf);
  }

  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
}

static State parseState(const String& s) {
  if (s == "work")  return S_WORK;
  if (s == "perm")  return S_PERM;
  if (s == "done")  return S_DONE;
  if (s == "idle")  return S_IDLE;
  if (s == "start") return S_START;
  if (s == "end")   return S_END;
  return S_IDLE;
}

// Parse one protocol line: "STATE|COUNT" (COUNT optional).
static void handleLine(String line) {
  line.trim();
  if (!line.length()) return;
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

void setup() {
  Serial.begin(115200);
  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setContrast(255);
  ledcSetup(LED_CH, LED_FREQ, LED_BITS);
  ledcAttachPin(PIN_LED, LED_CH);
  applyLed();
  pinMode(PIN_BTN, INPUT_PULLUP);
  draw();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rx.length()) { handleLine(rx); rx = ""; }
    } else {
      rx += c;
      if (rx.length() > RX_MAX) rx.remove(0, rx.length() - RX_MAX);
    }
  }

  bool btn = digitalRead(PIN_BTN);
  if (lastBtn == HIGH && btn == LOW) { calmAlarm(); dirty = true; }  // press edge
  lastBtn = btn;

  // Failsafe: auto-calm a CONFIRM that has been screaming too long. A missed
  // clear-event (Ctrl+C fires no hook; the VSCode Notification hook is dead) must
  // never strand the device in a forever-blink. This does what a BOOT press would
  // — drops to the steady dim glow — but keeps state == S_PERM so the info stays.
  // Gated on alarmArmed (not a bare timestamp) so it can ONLY fire against a timer
  // that armAlarm() actually started this alarm — never a stale/zero start value.
  if (alarmArmed && (millis() - alarmStart) > PERM_TIMEOUT_MS) {
    calmAlarm();
    dirty = true;
  }

  if (millis() - lastFlash > FLASH_MS) {
    lastFlash = millis();
    flashOn = !flashOn;
    if (state == S_PERM && !ack) dirty = true;
  }

  applyLed();                       // LED refreshed every loop (breathing/blink)
  if (dirty) { draw(); dirty = false; }
  delay(LOOP_MS);
}
