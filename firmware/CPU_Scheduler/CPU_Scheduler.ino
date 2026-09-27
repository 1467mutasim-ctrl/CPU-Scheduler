// ===========================================================================
//   CPU Scheduler Visualizer  v2  (single file)
//   ESP32 + 1.3" ST7789 240x240 IPS (SPI)  +  SSD1306 OLED (I2C)
//   ─────────────────────────────────────────────────────────────────────────
//   Drop this single .ino into a folder of the same name and open it.
//
//   Hardware
//     IPS  : MOSI=23  SCLK=18  DC=2  RST=4  CS=-1   (TFT_eSPI User_Setup)
//            Module needs SPI_MODE3 (see firmware/TFT_eSPI_setup).
//     OLED : SDA=21   SCL=22   I2C 0x3C
//     BTN  : UP=26  DOWN=32  OK=27  BACK=25         (active LOW, PULLUP)
//
//   Libraries
//     TFT_eSPI · Adafruit_GFX · Adafruit_SSD1306
// ===========================================================================

#include <Wire.h>
#include <TFT_eSPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <limits.h>
#include <math.h>

// ============================== PINS =======================================
#define OLED_SDA    21
#define OLED_SCL    22
#define OLED_ADDR   0x3C
#define OLED_W      128
#define OLED_H      64

#define BTN_UP      26
#define BTN_DOWN    32
#define BTN_OK      27
#define BTN_BACK    25

// ============================== LIMITS =====================================
#define MAX_PROCESSES   6
#define MAX_SLICES      96
#define MAX_SNAPSHOTS   24
#define IPS_W           240
#define IPS_H           240

// ============================== PALETTE (lighter, brighter) ================
//  Slate-blue base, vivid accents.  All values are RGB565.
#define BG_DEEP        0x2A4B   // medium slate ~RGB(40,72,88)
#define BG_PANEL       0x4310   // lighter slate
#define BG_PANEL_HI    0x6CB8   // soft highlight border
#define BG_IDLE        0x4A29   // dim block for idle CPU in Gantt
#define TEXT_HI        0xFFFF
#define TEXT_MID       0xE73C   // light cyan-gray
#define TEXT_DIM       0xB596   // medium gray
#define TEXT_FAINT     0x8430   // dim gray

#define ACCENT_CYAN    0x07FF
#define ACCENT_GREEN   0x6FE0   // bright mint
#define ACCENT_PURPLE  0xBC1F   // lavender
#define ACCENT_ORANGE  0xFD40
#define ACCENT_PINK    0xFB36
#define ACCENT_YELLOW  0xFF24
#define ACCENT_RED     0xF986

static const uint16_t PROCESS_COLORS[MAX_PROCESSES] = {
  ACCENT_CYAN, ACCENT_GREEN, ACCENT_PURPLE, ACCENT_ORANGE, ACCENT_PINK, ACCENT_YELLOW
};

// ============================== TYPES ======================================
enum AppState {
  ST_SPLASH,
  ST_MENU,
  ST_INPUT_NUM,        // pick number of processes
  ST_INPUT_FIELD,      // edit AT/BT/Pri of one process
  ST_INPUT_QUANTUM,    // RR (or Compare-All)
  ST_RUN,              // visualization sub-flow
};

enum AlgId { ALG_FCFS = 0, ALG_SJF, ALG_SRTF, ALG_PRIO, ALG_RR, ALG_COUNT };
#define ALG_COMPARE_ALL (-1)

struct Process {
  int id;
  int arrival;
  int burst;
  int priority;
  int remaining;
  int completion;
  int turnaround;
  int waiting;
  int response;
  bool started;
  int firstRun;
};

struct GanttSlice {
  int processId;     // 0 = idle
  int startTime;
  int endTime;
};

struct ScheduleResult {
  GanttSlice slices[MAX_SLICES];
  int sliceCount;
  int totalDuration;
  float avgWaiting;
  float avgTurnaround;
  float avgResponse;
  float cpuUtilization;
};

struct Snapshot {
  int  time;
  int  readyQueue[MAX_PROCESSES + 2];
  int  readyCount;
  int  runningId;
  int  remaining[MAX_PROCESSES + 1];
  int  sliceUpTo;
  char note[28];
};

// ============================== STRING TABLES ==============================
static const char* ALG_NAMES_LONG[ALG_COUNT] = {
  "First Come First Served",
  "Shortest Job First",
  "Shortest Remaining Time",
  "Priority Scheduling",
  "Round Robin"
};
static const char* ALG_NAMES_SHORT[ALG_COUNT] = { "FCFS", "SJF", "SRTF", "PRIO", "RR" };

static const char* ALG_PROS[ALG_COUNT] = {
  "Very simple to implement.\nFair in arrival order.\nEvery process runs\neventually  no starvation.",
  "Minimum average waiting\ntime when bursts are\nknown in advance.\nGood throughput.",
  "Optimal avg waiting and\nturnaround when bursts\nare known. Responds\nfast to short arrivals.",
  "Critical work runs first.\nFlexible policy via the\npriority number. Good\nfor real-time tasks.",
  "Fair time sharing.\nExcellent avg response.\nNo starvation.\nIdeal for interactive."
};
static const char* ALG_CONS[ALG_COUNT] = {
  "Convoy effect: one long\njob blocks short ones.\nPoor avg waiting time.\nNo priority awareness.",
  "Needs burst time known\nin advance  rarely true.\nLong jobs may starve\nif short ones keep arriving.",
  "Preemption overhead.\nLong jobs can starve\nwhen short jobs keep\narriving. Burst guessing.",
  "Low-priority jobs may\nstarve forever. Needs\naging to be fair.\nRisks priority inversion.",
  "Quantum tuning is hard:\ntoo small adds switch\noverhead, too large\nbehaves like FCFS."
};

// ============================== GLOBALS ====================================
TFT_eSPI tft = TFT_eSPI();
static Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

static Process       g_procs[MAX_PROCESSES];
static int           g_numProcs    = 4;
static int           g_quantum     = 2;
static int           g_currentAlg  = ALG_FCFS;
static ScheduleResult g_results[ALG_COUNT];
static bool          g_resultsValid[ALG_COUNT] = { false };
static AppState      g_state       = ST_SPLASH;

static Snapshot g_snaps[MAX_SNAPSHOTS];
static int      g_snapCount = 0;

static int  g_menuHover = 0;
static int  g_editProc  = 0;
static int  g_editField = 0;   // 0=AT 1=BT 2=Pri

// ============================== BUTTONS ====================================
enum BtnId { B_UP = 0, B_DOWN, B_OK, B_BACK, B_COUNT };
static const uint8_t btnPins[B_COUNT] = { BTN_UP, BTN_DOWN, BTN_OK, BTN_BACK };

static bool          btnLastRaw[B_COUNT]   = { true, true, true, true };
static bool          btnStable[B_COUNT]    = { true, true, true, true };
static unsigned long btnLastChg[B_COUNT]   = { 0, 0, 0, 0 };
static bool          btnPressEdge[B_COUNT] = { false, false, false, false };
static unsigned long btnFirstPress[B_COUNT] = { 0, 0, 0, 0 };
static unsigned long btnLastRepeat[B_COUNT] = { 0, 0, 0, 0 };
static bool          btnRepeatFlag[B_COUNT] = { false, false, false, false };

static const unsigned long DEBOUNCE_MS    = 25;
static const unsigned long REPEAT_DELAY   = 450;
static const unsigned long REPEAT_PERIOD  = 80;

static void buttonsBegin() {
  for (int i = 0; i < B_COUNT; i++) pinMode(btnPins[i], INPUT_PULLUP);
}
static void buttonsUpdate() {
  unsigned long now = millis();
  for (int i = 0; i < B_COUNT; i++) {
    bool raw = digitalRead(btnPins[i]);
    if (raw != btnLastRaw[i]) { btnLastRaw[i] = raw; btnLastChg[i] = now; }
    if (now - btnLastChg[i] > DEBOUNCE_MS && raw != btnStable[i]) {
      btnStable[i] = raw;
      if (btnStable[i] == LOW) {
        btnPressEdge[i]  = true;
        btnFirstPress[i] = now;
        btnLastRepeat[i] = now;
      }
    }
    btnRepeatFlag[i] = false;
    if (btnStable[i] == LOW &&
        now - btnFirstPress[i] > REPEAT_DELAY &&
        now - btnLastRepeat[i] > REPEAT_PERIOD) {
      btnRepeatFlag[i]  = true;
      btnLastRepeat[i]  = now;
    }
  }
}
static bool btnPressed(BtnId id) {
  if (btnPressEdge[id]) { btnPressEdge[id] = false; return true; }
  return false;
}
static bool btnRepeat(BtnId id) { return btnRepeatFlag[id]; }

// ============================== OLED =======================================
static const char* MENU_ITEMS[] = {
  "FCFS",
  "Shortest Job First",
  "Shortest Remaining",
  "Priority Sched.",
  "Round Robin",
  "Compare All",
};
static const int MENU_COUNT = 6;

static void oledBegin() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.clearDisplay();
  oled.setTextWrap(false);
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

// "CPU" on one line, "SCHEDULER" on the next, then a line, then "press OK".
static void oledSplash() {
  oled.clearDisplay();
  oled.drawRoundRect(0, 0, OLED_W, OLED_H, 4, SSD1306_WHITE);
  oled.setTextSize(2);
  // "CPU" = 3 chars * 12 = 36 wide -> center at 64 -> x=46
  oled.setCursor(46, 6);   oled.print(F("CPU"));
  // "SCHEDULER" = 9 chars * 12 = 108 wide -> x=10
  oled.setCursor(10, 26);  oled.print(F("SCHEDULER"));
  oled.drawFastHLine(10, 48, 108, SSD1306_WHITE);
  oled.setTextSize(1);
  // "press  OK" centered
  oled.setCursor(35, 54);  oled.print(F("press  OK"));
  oled.display();
}

static void oledMenu(int hover) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(2, 0); oled.print(F("ALGORITHM"));
  oled.drawFastHLine(0, 9, OLED_W, SSD1306_WHITE);
  int top = hover - 2;
  if (top < 0) top = 0;
  if (top > MENU_COUNT - 5) top = max(0, MENU_COUNT - 5);
  for (int i = 0; i < 5 && (top + i) < MENU_COUNT; i++) {
    int idx = top + i;
    int y   = 12 + i * 11;
    bool sel = (idx == hover);
    if (sel) {
      oled.fillRoundRect(0, y - 1, OLED_W, 10, 1, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    } else {
      oled.setTextColor(SSD1306_WHITE);
    }
    oled.setCursor(4, y);
    oled.print(idx + 1); oled.print(F(". "));
    oled.print(MENU_ITEMS[idx]);
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

static void oledInputHint(const char* algShort, const char* what, const char* val) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(2, 0); oled.print(F("INPUT  "));  oled.print(algShort);
  oled.drawFastHLine(0, 9, OLED_W, SSD1306_WHITE);
  oled.setCursor(2, 14); oled.print(what);
  oled.setTextSize(3);
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(val, 0, 0, &x1, &y1, &w, &h);
  oled.setCursor(OLED_W - w - 4, 28);
  oled.print(val);
  oled.setTextSize(1);
  oled.setCursor(2, 56); oled.print(F("UP/DN  OK=next"));
  oled.display();
}

// Hollow progress box that fills as we advance through pages.
static void oledProgress(const char* algShort, int page, int total) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(2, 0); oled.print(F("RUNNING"));
  oled.drawFastHLine(0, 9, OLED_W, SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(2, 14); oled.print(algShort);
  oled.setTextSize(1);
  char buf[24];
  snprintf(buf, sizeof(buf), "Step %d / %d", page, total);
  oled.setCursor(2, 36); oled.print(buf);
  // hollow box
  const int bx = 2, by = 48, bw = OLED_W - 4, bh = 12;
  oled.drawRoundRect(bx, by, bw, bh, 2, SSD1306_WHITE);
  if (total > 0) {
    int inner = bw - 4;
    int filled = (inner * page) / total;
    if (filled < 0) filled = 0;
    if (filled > inner) filled = inner;
    if (filled > 0) oled.fillRoundRect(bx + 2, by + 2, filled, bh - 4, 1, SSD1306_WHITE);
  }
  oled.display();
}

// ============================== ALGORITHM HELPERS ==========================
static void resetProcessComputed(Process* p, int n) {
  for (int i = 0; i < n; i++) {
    p[i].remaining  = p[i].burst;
    p[i].completion = 0;
    p[i].turnaround = 0;
    p[i].waiting    = 0;
    p[i].response   = 0;
    p[i].started    = false;
    p[i].firstRun   = -1;
  }
}

static void addSlice(ScheduleResult& r, int pid, int s, int e) {
  if (e <= s) return;
  if (r.sliceCount > 0
      && r.slices[r.sliceCount - 1].processId == pid
      && r.slices[r.sliceCount - 1].endTime   == s) {
    r.slices[r.sliceCount - 1].endTime = e;
    return;
  }
  if (r.sliceCount >= MAX_SLICES) return;
  r.slices[r.sliceCount].processId = pid;
  r.slices[r.sliceCount].startTime = s;
  r.slices[r.sliceCount].endTime   = e;
  r.sliceCount++;
}

static void computeAverages(Process* p, int n, ScheduleResult& r, int busy) {
  float w = 0, t = 0, rs = 0;
  for (int i = 0; i < n; i++) {
    w  += p[i].waiting;
    t  += p[i].turnaround;
    rs += p[i].response;
  }
  r.avgWaiting     = w / n;
  r.avgTurnaround  = t / n;
  r.avgResponse    = rs / n;
  r.cpuUtilization = (r.totalDuration > 0) ? (100.0f * busy / r.totalDuration) : 0;
}

static void captureSnap(Process* p, int n, int t, int runningPid,
                        const int* queue, int qLen,
                        int sliceUpTo, const char* note) {
  if (g_snapCount >= MAX_SNAPSHOTS) return;
  Snapshot& s = g_snaps[g_snapCount++];
  s.time      = t;
  s.runningId = runningPid;
  s.readyCount = (qLen > MAX_PROCESSES + 2) ? (MAX_PROCESSES + 2) : qLen;
  for (int i = 0; i < s.readyCount; i++) s.readyQueue[i] = queue[i];
  for (int i = 0; i <= MAX_PROCESSES; i++) s.remaining[i] = 0;
  for (int i = 0; i < n; i++) s.remaining[p[i].id] = p[i].remaining;
  s.sliceUpTo = sliceUpTo;
  strncpy(s.note, note ? note : "", sizeof(s.note) - 1);
  s.note[sizeof(s.note) - 1] = 0;
}

// ============================== ALGORITHMS =================================
//  Each captures snapshots at every scheduling decision when capture=true.

static void runFCFS(Process* p, int n, ScheduleResult& r, bool capture) {
  r.sliceCount = 0;
  resetProcessComputed(p, n);
  if (capture) g_snapCount = 0;
  int ord[MAX_PROCESSES];
  for (int i = 0; i < n; i++) ord[i] = i;
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (p[ord[j]].arrival <  p[ord[i]].arrival ||
         (p[ord[j]].arrival == p[ord[i]].arrival && p[ord[j]].id < p[ord[i]].id)) {
        int sw = ord[i]; ord[i] = ord[j]; ord[j] = sw;
      }
  int t = 0, busy = 0;
  for (int k = 0; k < n; k++) {
    Process& pr = p[ord[k]];
    if (t < pr.arrival) { addSlice(r, 0, t, pr.arrival); t = pr.arrival; }
    if (!pr.started) { pr.firstRun = t; pr.started = true; pr.response = t - pr.arrival; }
    if (capture) {
      int q[MAX_PROCESSES]; int qN = 0;
      for (int kk = k + 1; kk < n; kk++)
        if (p[ord[kk]].arrival <= t && p[ord[kk]].remaining > 0) q[qN++] = p[ord[kk]].id;
      char note[28];
      snprintf(note, sizeof(note), "P%d gets CPU", pr.id);
      captureSnap(p, n, t, pr.id, q, qN, r.sliceCount, note);
    }
    addSlice(r, pr.id, t, t + pr.burst);
    busy += pr.burst; t += pr.burst;
    pr.completion = t;
    pr.turnaround = pr.completion - pr.arrival;
    pr.waiting    = pr.turnaround - pr.burst;
  }
  if (capture) { int q[1]; captureSnap(p, n, t, 0, q, 0, r.sliceCount, "All processes done"); }
  r.totalDuration = t;
  computeAverages(p, n, r, busy);
}

static void runSJF(Process* p, int n, ScheduleResult& r, bool capture) {
  r.sliceCount = 0;
  resetProcessComputed(p, n);
  if (capture) g_snapCount = 0;
  int t = 0, done = 0, busy = 0;
  while (done < n) {
    int sel = -1, minB = INT_MAX;
    for (int i = 0; i < n; i++) {
      if (p[i].remaining > 0 && p[i].arrival <= t) {
        if (p[i].burst < minB ||
           (p[i].burst == minB && sel != -1 && p[i].id < p[sel].id)) {
          minB = p[i].burst; sel = i;
        }
      }
    }
    if (sel == -1) {
      int nxt = INT_MAX;
      for (int i = 0; i < n; i++)
        if (p[i].remaining > 0 && p[i].arrival < nxt) nxt = p[i].arrival;
      addSlice(r, 0, t, nxt); t = nxt; continue;
    }
    Process& pr = p[sel];
    if (!pr.started) { pr.firstRun = t; pr.started = true; pr.response = t - pr.arrival; }
    if (capture) {
      int q[MAX_PROCESSES]; int qN = 0;
      for (int i = 0; i < n; i++)
        if (i != sel && p[i].remaining > 0 && p[i].arrival <= t) q[qN++] = p[i].id;
      char note[28];
      snprintf(note, sizeof(note), "Pick P%d (BT=%d)", pr.id, pr.burst);
      captureSnap(p, n, t, pr.id, q, qN, r.sliceCount, note);
    }
    addSlice(r, pr.id, t, t + pr.burst);
    busy += pr.burst; t += pr.burst;
    pr.remaining  = 0;
    pr.completion = t;
    pr.turnaround = pr.completion - pr.arrival;
    pr.waiting    = pr.turnaround - pr.burst;
    done++;
  }
  if (capture) { int q[1]; captureSnap(p, n, t, 0, q, 0, r.sliceCount, "All processes done"); }
  r.totalDuration = t;
  computeAverages(p, n, r, busy);
}

static void runSRTF(Process* p, int n, ScheduleResult& r, bool capture) {
  r.sliceCount = 0;
  resetProcessComputed(p, n);
  if (capture) g_snapCount = 0;
  int t = 0, done = 0, busy = 0;
  int curPid = -1, sliceStart = 0;
  while (done < n) {
    int sel = -1, minR = INT_MAX;
    for (int i = 0; i < n; i++) {
      if (p[i].remaining > 0 && p[i].arrival <= t) {
        if (p[i].remaining < minR ||
           (p[i].remaining == minR && sel != -1 && p[i].id < p[sel].id)) {
          minR = p[i].remaining; sel = i;
        }
      }
    }
    int thisPid = (sel == -1) ? 0 : p[sel].id;
    if (thisPid != curPid) {
      if (curPid != -1) addSlice(r, curPid, sliceStart, t);
      if (capture && thisPid != 0) {
        int q[MAX_PROCESSES]; int qN = 0;
        for (int i = 0; i < n; i++)
          if (i != sel && p[i].remaining > 0 && p[i].arrival <= t) q[qN++] = p[i].id;
        char note[28];
        if (curPid > 0) snprintf(note, sizeof(note), "P%d preempts P%d", thisPid, curPid);
        else            snprintf(note, sizeof(note), "Start P%d (rem=%d)", thisPid, p[sel].remaining);
        captureSnap(p, n, t, thisPid, q, qN, r.sliceCount, note);
      }
      curPid = thisPid;
      sliceStart = t;
    }
    if (sel != -1) {
      Process& pr = p[sel];
      if (!pr.started) { pr.firstRun = t; pr.started = true; pr.response = t - pr.arrival; }
      pr.remaining--;
      busy++;
      if (pr.remaining == 0) {
        pr.completion = t + 1;
        pr.turnaround = pr.completion - pr.arrival;
        pr.waiting    = pr.turnaround - pr.burst;
        done++;
      }
    }
    t++;
  }
  if (curPid != -1) addSlice(r, curPid, sliceStart, t);
  if (capture) { int q[1]; captureSnap(p, n, t, 0, q, 0, r.sliceCount, "All processes done"); }
  r.totalDuration = t;
  computeAverages(p, n, r, busy);
}

static void runPriority(Process* p, int n, ScheduleResult& r, bool capture) {
  r.sliceCount = 0;
  resetProcessComputed(p, n);
  if (capture) g_snapCount = 0;
  int t = 0, done = 0, busy = 0;
  while (done < n) {
    int sel = -1, minPr = INT_MAX;
    for (int i = 0; i < n; i++) {
      if (p[i].remaining > 0 && p[i].arrival <= t) {
        if (p[i].priority < minPr ||
           (p[i].priority == minPr && sel != -1 && p[i].id < p[sel].id)) {
          minPr = p[i].priority; sel = i;
        }
      }
    }
    if (sel == -1) {
      int nxt = INT_MAX;
      for (int i = 0; i < n; i++)
        if (p[i].remaining > 0 && p[i].arrival < nxt) nxt = p[i].arrival;
      addSlice(r, 0, t, nxt); t = nxt; continue;
    }
    Process& pr = p[sel];
    if (!pr.started) { pr.firstRun = t; pr.started = true; pr.response = t - pr.arrival; }
    if (capture) {
      int q[MAX_PROCESSES]; int qN = 0;
      for (int i = 0; i < n; i++)
        if (i != sel && p[i].remaining > 0 && p[i].arrival <= t) q[qN++] = p[i].id;
      char note[28];
      snprintf(note, sizeof(note), "Pick P%d (pri=%d)", pr.id, pr.priority);
      captureSnap(p, n, t, pr.id, q, qN, r.sliceCount, note);
    }
    addSlice(r, pr.id, t, t + pr.burst);
    busy += pr.burst; t += pr.burst;
    pr.remaining  = 0;
    pr.completion = t;
    pr.turnaround = pr.completion - pr.arrival;
    pr.waiting    = pr.turnaround - pr.burst;
    done++;
  }
  if (capture) { int q[1]; captureSnap(p, n, t, 0, q, 0, r.sliceCount, "All processes done"); }
  r.totalDuration = t;
  computeAverages(p, n, r, busy);
}

static void runRR(Process* p, int n, int quantum, ScheduleResult& r, bool capture) {
  r.sliceCount = 0;
  resetProcessComputed(p, n);
  if (capture) g_snapCount = 0;
  if (quantum < 1) quantum = 1;
  int t = 0, done = 0, busy = 0;
  int queue[MAX_PROCESSES * 32]; int qH = 0, qT = 0;
  bool inQ[MAX_PROCESSES] = { false };

  int firstArr = INT_MAX;
  for (int i = 0; i < n; i++) if (p[i].arrival < firstArr) firstArr = p[i].arrival;
  if (firstArr > 0) { addSlice(r, 0, 0, firstArr); t = firstArr; }

  for (int i = 0; i < n; i++)
    if (p[i].arrival <= t && p[i].remaining > 0 && !inQ[i]) {
      queue[qT++] = i; inQ[i] = true;
    }

  while (done < n) {
    if (qH == qT) {
      int nxt = INT_MAX;
      for (int i = 0; i < n; i++)
        if (p[i].remaining > 0 && p[i].arrival > t && p[i].arrival < nxt)
          nxt = p[i].arrival;
      if (nxt == INT_MAX) break;
      addSlice(r, 0, t, nxt);
      t = nxt;
      for (int i = 0; i < n; i++)
        if (p[i].arrival <= t && p[i].remaining > 0 && !inQ[i]) {
          queue[qT++] = i; inQ[i] = true;
        }
      continue;
    }
    int idx = queue[qH++]; inQ[idx] = false;
    Process& pr = p[idx];
    if (!pr.started) { pr.firstRun = t; pr.started = true; pr.response = t - pr.arrival; }
    if (capture) {
      int q[MAX_PROCESSES + 4]; int qN = 0;
      for (int i = qH; i < qT && qN < MAX_PROCESSES + 2; i++) q[qN++] = p[queue[i]].id;
      char note[28];
      snprintf(note, sizeof(note), "P%d gets CPU (q=%d)", pr.id, quantum);
      captureSnap(p, n, t, pr.id, q, qN, r.sliceCount, note);
    }
    int run = min(quantum, pr.remaining);
    addSlice(r, pr.id, t, t + run);
    pr.remaining -= run;
    busy         += run;
    t            += run;
    for (int i = 0; i < n; i++)
      if (i != idx && p[i].arrival <= t && p[i].remaining > 0 && !inQ[i]) {
        queue[qT++] = i; inQ[i] = true;
      }
    if (pr.remaining > 0) { queue[qT++] = idx; inQ[idx] = true; }
    else {
      pr.completion = t;
      pr.turnaround = pr.completion - pr.arrival;
      pr.waiting    = pr.turnaround - pr.burst;
      done++;
    }
  }
  if (capture) { int q[1]; captureSnap(p, n, t, 0, q, 0, r.sliceCount, "All processes done"); }
  r.totalDuration = t;
  computeAverages(p, n, r, busy);
}

static void runAlgorithm(int algId, Process* p, int n, int quantum,
                         ScheduleResult& r, bool capture) {
  switch (algId) {
    case ALG_FCFS: runFCFS(p, n, r, capture);          break;
    case ALG_SJF:  runSJF(p, n, r, capture);           break;
    case ALG_SRTF: runSRTF(p, n, r, capture);          break;
    case ALG_PRIO: runPriority(p, n, r, capture);      break;
    case ALG_RR:   runRR(p, n, quantum, r, capture);   break;
  }
}

static void runAllAlgorithms() {
  Process scratch[MAX_PROCESSES];
  for (int a = 0; a < ALG_COUNT; a++) {
    for (int i = 0; i < g_numProcs; i++) scratch[i] = g_procs[i];
    runAlgorithm(a, scratch, g_numProcs, g_quantum, g_results[a], false);
    g_resultsValid[a] = true;
  }
}

// ============================== IPS HELPERS ================================
static uint16_t procColor(int pid) {
  if (pid <= 0 || pid > MAX_PROCESSES) return TEXT_FAINT;
  return PROCESS_COLORS[(pid - 1) % MAX_PROCESSES];
}

static void ipsClear() { tft.fillScreen(BG_DEEP); }

// Chrome: big title left, small status right, accent line.
static void ipsChrome(const char* title, const char* rightInfo) {
  tft.fillRect(0, 0, IPS_W, 34, BG_DEEP);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TEXT_HI, BG_DEEP);
  tft.drawString(title, 8, 3);
  if (rightInfo && *rightInfo) {
    tft.setTextDatum(TR_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TEXT_MID, BG_DEEP);
    tft.drawString(rightInfo, IPS_W - 8, 12);
  }
  tft.drawFastHLine(0, 32, IPS_W, BG_PANEL_HI);
  tft.drawFastHLine(0, 33, IPS_W * 2 / 3, ACCENT_CYAN);
}

static void drawPidBox(int x, int y, int w, int h, int pid, int font) {
  uint16_t col = procColor(pid);
  tft.fillRoundRect(x, y, w, h, 4, col);
  tft.drawRoundRect(x, y, w, h, 4, BG_DEEP);
  char lbl[6]; snprintf(lbl, sizeof(lbl), "P%d", pid);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(font);
  tft.setTextColor(BG_DEEP, col);
  tft.drawString(lbl, x + w / 2, y + h / 2);
}

static void drawPanel(int x, int y, int w, int h, const char* title, uint16_t accent) {
  tft.fillRoundRect(x, y, w, h, 6, BG_PANEL);
  tft.drawRoundRect(x, y, w, h, 6, BG_PANEL_HI);
  if (title) {
    tft.fillRect(x + 8, y + 8, 28, 3, accent);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TEXT_MID, BG_PANEL);
    tft.drawString(title, x + 8, y + 14);
  }
}

// Draw up to sliceUpTo slices of result, scaled to TOTAL duration.
static void drawMiniGantt(int x, int y, int w, int barH, ScheduleResult& r,
                          int sliceUpTo, bool animate) {
  // bg & frame
  tft.fillRect(x, y, w, barH, BG_PANEL);
  tft.drawRect(x - 1, y - 1, w + 2, barH + 2, BG_PANEL_HI);
  if (r.totalDuration <= 0) return;
  float ppu = (float)w / r.totalDuration;
  int last = (sliceUpTo < r.sliceCount) ? sliceUpTo : r.sliceCount;
  for (int s = 0; s < last; s++) {
    int bx = x + (int)(r.slices[s].startTime * ppu);
    int bw = (int)((r.slices[s].endTime - r.slices[s].startTime) * ppu);
    if (bw < 1) bw = 1;
    uint16_t col = (r.slices[s].processId == 0) ? BG_IDLE
                                                : procColor(r.slices[s].processId);
    if (animate) {
      int steps = (bw < 6) ? 1 : max(2, bw / 6);
      for (int k = 1; k <= steps; k++) {
        int wk = (bw * k) / steps;
        tft.fillRect(bx, y, wk, barH, col);
        delay(6);
      }
    } else {
      tft.fillRect(bx, y, bw, barH, col);
    }
    if (bw >= 16) {
      char lbl[6];
      if (r.slices[s].processId == 0) snprintf(lbl, sizeof(lbl), "--");
      else snprintf(lbl, sizeof(lbl), "P%d", r.slices[s].processId);
      tft.setTextDatum(MC_DATUM);
      tft.setTextFont(1);
      tft.setTextColor(BG_DEEP, col);
      tft.drawString(lbl, bx + bw / 2, y + barH / 2);
    }
    if (bx > x) tft.drawFastVLine(bx, y, barH, BG_DEEP);
  }
}

// Time axis below a Gantt bar.
static void drawGanttAxis(int x, int y, int w, ScheduleResult& r, int sliceUpTo) {
  if (r.totalDuration <= 0) return;
  float ppu = (float)w / r.totalDuration;
  tft.setTextDatum(TC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  tft.drawString("0", x, y + 2);
  tft.drawFastVLine(x, y, 3, TEXT_DIM);
  int last = (sliceUpTo < r.sliceCount) ? sliceUpTo : r.sliceCount;
  for (int s = 0; s < last; s++) {
    int ex = x + (int)(r.slices[s].endTime * ppu);
    if (ex >= x + w - 4) continue;
    tft.drawFastVLine(ex, y, 3, TEXT_DIM);
    char b[6]; snprintf(b, sizeof(b), "%d", r.slices[s].endTime);
    tft.drawString(b, ex, y + 2);
  }
  // total at the far right
  tft.drawFastVLine(x + w, y, 3, TEXT_DIM);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TEXT_MID, BG_DEEP);
  char b[6]; snprintf(b, sizeof(b), "%d", r.totalDuration);
  tft.drawString(b, x + w + 2, y + 2);
}

// ============================== IPS SPLASH / TITLE =========================
static void ipsSplash() {
  ipsClear();
  // big "CPU SCHEDULER"
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(6);
  tft.setTextColor(ACCENT_CYAN, BG_DEEP);
  tft.drawString("CPU", IPS_W / 2, 70);
  tft.setTextFont(4);
  tft.setTextColor(TEXT_HI, BG_DEEP);
  tft.drawString("SCHEDULER", IPS_W / 2, 116);
  // decorative ring of mini bars
  int by = 150, bh = 8;
  for (int i = 0; i < ALG_COUNT; i++) {
    int bw = 36, bx = 14 + i * 42;
    tft.fillRoundRect(bx, by, bw, bh, 2, PROCESS_COLORS[i]);
  }
  tft.setTextFont(2);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  tft.drawString("5 algorithms  -  animated", IPS_W / 2, 184);
  tft.setTextColor(TEXT_FAINT, BG_DEEP);
  tft.drawString("press OK to begin", IPS_W / 2, 212);
}

static void ipsAlgorithmTitleCard(int algId) {
  ipsClear();
  uint16_t col = (algId == ALG_FCFS) ? ACCENT_CYAN
              : (algId == ALG_SJF)  ? ACCENT_GREEN
              : (algId == ALG_SRTF) ? ACCENT_PURPLE
              : (algId == ALG_PRIO) ? ACCENT_ORANGE
              : ACCENT_PINK;
  // sliding stripe
  for (int x = -140; x <= 30; x += 8) {
    if (x > 0) tft.fillRect(0, 70, IPS_W, 90, BG_DEEP);
    tft.fillRect(x, 70, 120, 90, col);
    delay(8);
  }
  tft.fillRect(0, 70, IPS_W, 90, BG_DEEP);
  tft.drawFastHLine(0, 70, IPS_W, col);
  tft.drawFastHLine(0, 159, IPS_W, col);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(6);
  tft.setTextColor(TEXT_HI, BG_DEEP);
  tft.drawString(ALG_NAMES_SHORT[algId], IPS_W / 2, 104);
  tft.setTextFont(2);
  tft.setTextColor(col, BG_DEEP);
  tft.drawString(ALG_NAMES_LONG[algId], IPS_W / 2, 144);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  tft.drawString("computing schedule...", IPS_W / 2, 190);
  delay(650);
}

static void ipsIdleScreen() {
  ipsClear();
  ipsChrome("Ready", "use OLED menu");
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TEXT_HI, BG_DEEP);
  tft.drawString("Pick an algorithm", IPS_W / 2, 90);
  tft.setTextFont(2);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  tft.drawString("on the OLED display", IPS_W / 2, 124);
  tft.drawString("then enter your processes here", IPS_W / 2, 150);
  // small palette swatch row
  int by = 180, bh = 22;
  for (int i = 0; i < ALG_COUNT; i++) {
    int bw = 36, bx = 14 + i * 42;
    tft.fillRoundRect(bx, by, bw, bh, 4, PROCESS_COLORS[i]);
    char b[4]; snprintf(b, sizeof(b), "P%d", i + 1);
    tft.setTextFont(2);
    tft.setTextColor(BG_DEEP, PROCESS_COLORS[i]);
    tft.drawString(b, bx + bw / 2, by + bh / 2);
  }
}

// ============================== INPUT FLOW HELPERS =========================
static bool needsPriorityInput() {
  return g_currentAlg == ALG_PRIO || g_currentAlg == ALG_COMPARE_ALL;
}
static bool needsQuantumInput() {
  return g_currentAlg == ALG_RR || g_currentAlg == ALG_COMPARE_ALL;
}
static const char* algShortFor() {
  return (g_currentAlg == ALG_COMPARE_ALL) ? "ALL" : ALG_NAMES_SHORT[g_currentAlg];
}

static void drawBigNumberBox(int val, uint16_t accent) {
  const int bw = 140, bh = 84;
  const int bx = (IPS_W - bw) / 2;
  const int by = 80;
  tft.fillRoundRect(bx, by, bw, bh, 10, BG_PANEL);
  tft.drawRoundRect(bx, by, bw, bh, 10, accent);
  tft.fillRect(bx + 8, by + 8, bw - 16, 3, accent);
  char b[8]; snprintf(b, sizeof(b), "%d", val);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(6);
  tft.setTextColor(accent, BG_PANEL);
  tft.drawString(b, IPS_W / 2, by + bh / 2 + 6);
}

static void drawNavHint(const char* msg) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  tft.drawString(msg, IPS_W / 2, IPS_H - 6);
}

// ============================== IPS INPUT PAGES ============================
static void pageInputNum() {
  ipsClear();
  ipsChrome("Configure", algShortFor());
  // big label
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TEXT_HI, BG_DEEP);
  tft.drawString("Number of Processes", IPS_W / 2, 56);
  // big number
  drawBigNumberBox(g_numProcs, ACCENT_CYAN);
  // context below box
  tft.setTextFont(2);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  tft.drawString("processes to simulate (2 - 6)", IPS_W / 2, 184);
  drawNavHint("UP / DN  -  OK to confirm");
}

static void pageInputField() {
  ipsClear();
  char ri[24];
  snprintf(ri, sizeof(ri), "%s - P%d/%d", algShortFor(), g_editProc + 1, g_numProcs);
  ipsChrome("Configure", ri);

  Process& pr = g_procs[g_editProc];
  const char* fname = (g_editField == 0) ? "Arrival Time"
                     :(g_editField == 1) ? "Burst Time"
                     :                     "Priority";
  uint16_t accent = (g_editField == 0) ? ACCENT_CYAN
                  : (g_editField == 1) ? ACCENT_GREEN
                                       : ACCENT_ORANGE;

  // big label "P2 Arrival Time"
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TEXT_HI, BG_DEEP);
  char lbl[24]; snprintf(lbl, sizeof(lbl), "P%d  %s", pr.id, fname);
  tft.drawString(lbl, IPS_W / 2, 56);

  int val = (g_editField == 0) ? pr.arrival
          : (g_editField == 1) ? pr.burst
          :                      pr.priority;
  drawBigNumberBox(val, accent);

  // small summary of all processes so far, font 1
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  tft.drawString("entered so far:", 8, 178);
  int sy = 190;
  for (int i = 0; i < g_numProcs && sy < IPS_H - 14; i++) {
    char buf[40];
    bool hasPri = needsPriorityInput();
    if (hasPri) {
      snprintf(buf, sizeof(buf), "P%d   AT=%-2d  BT=%-2d  Pri=%d",
               g_procs[i].id, g_procs[i].arrival, g_procs[i].burst, g_procs[i].priority);
    } else {
      snprintf(buf, sizeof(buf), "P%d   AT=%-2d  BT=%-2d",
               g_procs[i].id, g_procs[i].arrival, g_procs[i].burst);
    }
    tft.setTextColor((i == g_editProc) ? ACCENT_CYAN : TEXT_DIM, BG_DEEP);
    tft.drawString(buf, 8, sy);
    sy += 10;
  }
  drawNavHint("UP / DN  -  OK next  -  BACK");
}

static void pageInputQuantum() {
  ipsClear();
  ipsChrome("Configure", algShortFor());
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TEXT_HI, BG_DEEP);
  tft.drawString("Time Quantum", IPS_W / 2, 56);
  drawBigNumberBox(g_quantum, ACCENT_PINK);
  tft.setTextFont(2);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  tft.drawString("CPU slice per turn (Round Robin)", IPS_W / 2, 184);
  drawNavHint("UP / DN  -  OK to run  -  BACK");
}

// ============================== IPS RUN PAGES ==============================
//   Input summary, iterations, Gantt, per-process table, metrics, pros/cons.

static void pageInputSummary() {
  ipsClear();
  char rinfo[32];
  if (needsQuantumInput()) snprintf(rinfo, sizeof(rinfo), "summary  q=%d", g_quantum);
  else                     strcpy(rinfo, "input summary");
  ipsChrome(algShortFor(), rinfo);

  bool hasPri = needsPriorityInput();
  int cols = hasPri ? 4 : 3;
  int x0 = 6;
  int totalW = IPS_W - 12;
  int colW[4];
  colW[0] = 48;
  int rest = totalW - colW[0];
  for (int c = 1; c < cols; c++) colW[c] = rest / (cols - 1);

  int rowH = 22;
  // Panel layout:
  //   title area (28) + col-header (20) + data rows + bottom pad (6)
  int panelH = 28 + 20 + g_numProcs * rowH + 6;
  drawPanel(x0, 42, totalW, panelH, "PROCESS PARAMETERS", ACCENT_CYAN);

  const char* H[4] = { "PID", "AT", "BT", "Pri" };
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  int hx = x0;
  int hy = 42 + 38;          // column-header row centerline
  for (int c = 0; c < cols; c++) {
    tft.setTextColor(TEXT_DIM, BG_PANEL);
    tft.drawString(H[c], hx + colW[c] / 2, hy);
    hx += colW[c];
  }
  tft.drawFastHLine(x0 + 8, hy + 10, totalW - 16, BG_PANEL_HI);

  // data rows
  int row0CenterY = hy + 10 + rowH / 2 + 4;   // 4px gap below header line
  for (int i = 0; i < g_numProcs; i++) {
    int y = row0CenterY + i * rowH;
    int x = x0;
    tft.fillRoundRect(x + 6, y - 10, colW[0] - 12, 18, 3, procColor(g_procs[i].id));
    tft.setTextColor(BG_DEEP, procColor(g_procs[i].id));
    char b[6]; snprintf(b, sizeof(b), "P%d", g_procs[i].id);
    tft.drawString(b, x + colW[0] / 2, y - 1);
    x += colW[0];
    int vals[3] = { g_procs[i].arrival, g_procs[i].burst, g_procs[i].priority };
    tft.setTextColor(TEXT_HI, BG_PANEL);
    for (int c = 1; c < cols; c++) {
      char nb[6]; snprintf(nb, sizeof(nb), "%d", vals[c - 1]);
      tft.drawString(nb, x + colW[c] / 2, y - 1);
      x += colW[c];
    }
    delay(35);
  }
  drawNavHint("OK = start  -  BACK = exit");
}

static void pageIteration(int algId, int snapIdx) {
  Snapshot& s = g_snaps[snapIdx];
  ipsClear();
  char ri[16];
  snprintf(ri, sizeof(ri), "Step %d/%d", snapIdx + 1, g_snapCount);
  ipsChrome(ALG_NAMES_SHORT[algId], ri);

  // TIME = X    (big, left-aligned)
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(ACCENT_CYAN, BG_DEEP);
  char tb[24];
  snprintf(tb, sizeof(tb), "TIME = %d", s.time);
  tft.drawString(tb, 8, 40);

  // note (right side of TIME)
  tft.setTextFont(2);
  tft.setTextColor(TEXT_MID, BG_DEEP);
  tft.drawString(s.note, 8, 70);

  // separator
  tft.drawFastHLine(8, 92, IPS_W - 16, BG_PANEL_HI);

  // READY label + boxes
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TEXT_HI, BG_DEEP);
  tft.drawString("READY:", 8, 100);
  int bx0 = 70, by0 = 96;
  const int boxW = 28, boxH = 22, gap = 3;
  if (s.readyCount == 0) {
    tft.setTextFont(2);
    tft.setTextColor(TEXT_FAINT, BG_DEEP);
    tft.drawString("(empty)", bx0, 102);
  } else {
    int cx = bx0, cy = by0;
    for (int i = 0; i < s.readyCount; i++) {
      if (cx + boxW > IPS_W - 6) { cx = bx0; cy += boxH + 3; }
      drawPidBox(cx, cy, boxW, boxH, s.readyQueue[i], 2);
      cx += boxW + gap;
    }
  }

  // RUN label + box + remaining
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TEXT_HI, BG_DEEP);
  tft.drawString("RUN:", 8, 140);
  if (s.runningId > 0) {
    drawPidBox(78, 132, 50, 30, s.runningId, 4);
    int totBurst = 0;
    for (int i = 0; i < g_numProcs; i++)
      if (g_procs[i].id == s.runningId) totBurst = g_procs[i].burst;
    int rem = s.remaining[s.runningId];
    int used = totBurst - rem;
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TEXT_MID, BG_DEEP);
    char rb[24];
    snprintf(rb, sizeof(rb), "%d / %d left", rem, totBurst);
    tft.drawString(rb, 138, 138);
    // little progress bar for current process
    int pbx = 138, pby = 156, pbw = 92, pbh = 6;
    tft.drawRoundRect(pbx, pby, pbw, pbh, 2, BG_PANEL_HI);
    if (totBurst > 0) {
      int fw = (pbw - 2) * used / totBurst;
      if (fw > 0) tft.fillRoundRect(pbx + 1, pby + 1, fw, pbh - 2, 1, procColor(s.runningId));
    }
  } else {
    tft.setTextFont(2);
    tft.setTextColor(TEXT_FAINT, BG_DEEP);
    tft.drawString("(CPU idle)", 78, 138);
  }

  // mini Gantt
  drawMiniGantt(8, 178, IPS_W - 16, 24, g_results[algId], s.sliceUpTo, false);
  drawGanttAxis(8, 204, IPS_W - 16, g_results[algId], s.sliceUpTo);

  drawNavHint("OK = next  -  BACK = prev");
}

static void pageFinalGantt(int algId) {
  ipsClear();
  ipsChrome(ALG_NAMES_SHORT[algId], "Gantt chart");
  ScheduleResult& r = g_results[algId];

  // stats line
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  char b[40];
  snprintf(b, sizeof(b), "duration = %d ms", r.totalDuration);
  tft.drawString(b, 8, 42);
  tft.setTextDatum(TR_DATUM);
  snprintf(b, sizeof(b), "%d slices", r.sliceCount);
  tft.drawString(b, IPS_W - 8, 42);

  // animated Gantt
  drawMiniGantt(8, 78, IPS_W - 16, 48, r, r.sliceCount, true);
  drawGanttAxis(8, 128, IPS_W - 16, r, r.sliceCount);

  // legend grid
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TEXT_MID, BG_DEEP);
  tft.drawString("Processes used:", 8, 152);
  bool seen[MAX_PROCESSES + 1] = { false };
  int lx = 8, ly = 174;
  const int boxW = 32, boxH = 22;
  for (int s = 0; s < r.sliceCount; s++) {
    int pid = r.slices[s].processId;
    if (pid <= 0 || pid > MAX_PROCESSES || seen[pid]) continue;
    seen[pid] = true;
    if (lx + boxW > IPS_W - 6) { lx = 8; ly += boxH + 4; }
    drawPidBox(lx, ly, boxW, boxH, pid, 2);
    lx += boxW + 6;
  }
  drawNavHint("OK = table  -  BACK = prev");
}

static void pagePerProcessTable(int algId) {
  ipsClear();
  ipsChrome(ALG_NAMES_SHORT[algId], "results");
  const int x0 = 6;
  const int totalW = IPS_W - 12;
  const int cols = 5;
  const int colW = totalW / cols;
  const int rowH = 22;
  const int top = 42;
  int panelH = 28 + 20 + g_numProcs * rowH + 6;

  drawPanel(x0, top, totalW, panelH, "PER-PROCESS METRICS", ACCENT_PURPLE);

  const char* H[5] = { "PID", "CT", "TAT", "WT", "RT" };
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  int hy = top + 38;
  for (int c = 0; c < cols; c++) {
    tft.setTextColor(TEXT_DIM, BG_PANEL);
    tft.drawString(H[c], x0 + c * colW + colW / 2, hy);
  }
  tft.drawFastHLine(x0 + 6, hy + 10, totalW - 12, BG_PANEL_HI);

  int row0CenterY = hy + 10 + rowH / 2 + 4;
  for (int i = 0; i < g_numProcs; i++) {
    int y = row0CenterY + i * rowH;
    int v[5] = {
      g_procs[i].id,
      g_procs[i].completion,
      g_procs[i].turnaround,
      g_procs[i].waiting,
      g_procs[i].response
    };
    tft.fillRoundRect(x0 + 6, y - 10, colW - 12, 18, 3, procColor(g_procs[i].id));
    tft.setTextColor(BG_DEEP, procColor(g_procs[i].id));
    char b[6]; snprintf(b, sizeof(b), "P%d", v[0]);
    tft.drawString(b, x0 + colW / 2, y - 1);
    tft.setTextColor(TEXT_HI, BG_PANEL);
    for (int c = 1; c < cols; c++) {
      char nb[6]; snprintf(nb, sizeof(nb), "%d", v[c]);
      tft.drawString(nb, x0 + c * colW + colW / 2, y - 1);
    }
    delay(40);
  }
  drawNavHint("CT=complete  TAT=turn  WT=wait  RT=resp");
}

// Big metric tile with animated count-up.
static void drawBigMetricTile(int x, int y, int w, int h,
                              const char* label, float value, const char* unit,
                              uint16_t accent) {
  tft.fillRoundRect(x, y, w, h, 6, BG_PANEL);
  tft.drawRoundRect(x, y, w, h, 6, BG_PANEL_HI);
  tft.fillRect(x + 8, y + 8, 36, 4, accent);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(TEXT_MID, BG_PANEL);
  tft.drawString(label, x + 10, y + 16);
  // big number, animated
  int frames = 14;
  for (int f = 1; f <= frames; f++) {
    float v = value * f / frames;
    char b[12];
    if (value >= 100) snprintf(b, sizeof(b), "%.0f", v);
    else              snprintf(b, sizeof(b), "%.2f", v);
    tft.fillRect(x + 8, y + 38, w - 70, h - 46, BG_PANEL);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(6);
    tft.setTextColor(accent, BG_PANEL);
    tft.drawString(b, x + 16, y + 36);
    delay(11);
  }
  // unit big at right
  tft.setTextDatum(MR_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(TEXT_DIM, BG_PANEL);
  tft.drawString(unit, x + w - 14, y + h - 18);
}

static void pageMetricsA(int algId) {
  ipsClear();
  ipsChrome(ALG_NAMES_SHORT[algId], "metrics 1/2");
  ScheduleResult& r = g_results[algId];
  drawBigMetricTile(6,  42, IPS_W - 12, 88, "AVG WAITING TIME",    r.avgWaiting,    "ms", ACCENT_GREEN);
  drawBigMetricTile(6, 138, IPS_W - 12, 88, "AVG TURNAROUND TIME", r.avgTurnaround, "ms", ACCENT_PURPLE);
  drawNavHint("OK = next  -  BACK = prev");
}
static void pageMetricsB(int algId) {
  ipsClear();
  ipsChrome(ALG_NAMES_SHORT[algId], "metrics 2/2");
  ScheduleResult& r = g_results[algId];
  drawBigMetricTile(6,  42, IPS_W - 12, 88, "AVG RESPONSE TIME", r.avgResponse,    "ms", ACCENT_ORANGE);
  drawBigMetricTile(6, 138, IPS_W - 12, 88, "CPU UTILIZATION",   r.cpuUtilization, "%",  ACCENT_CYAN);
  drawNavHint("OK = next  -  BACK = prev");
}

// Multi-line wrapped text rendering.
static void drawTextBlock(const char* s, int x, int y, int maxLineW,
                          int lineH, uint16_t fg, uint16_t bg, int font) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(font);
  tft.setTextColor(fg, bg);
  int cy = y;
  int lineStart = 0;
  int i = 0;
  char buf[80];
  while (true) {
    if (s[i] == 0 || s[i] == '\n') {
      int len = i - lineStart;
      if (len > 0) {
        if (len > 79) len = 79;
        memcpy(buf, s + lineStart, len);
        buf[len] = 0;
        tft.drawString(buf, x, cy);
        cy += lineH;
      } else {
        cy += lineH / 2;
      }
      if (s[i] == 0) break;
      lineStart = i + 1;
    }
    i++;
    if (i > 400) break;
  }
}

static void pageAdvantages(int algId) {
  ipsClear();
  ipsChrome(ALG_NAMES_SHORT[algId], "advantages");
  // big header
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(ACCENT_GREEN, BG_DEEP);
  tft.drawString("ADVANTAGES", IPS_W / 2, 58);
  tft.drawFastHLine(28, 78, IPS_W - 56, ACCENT_GREEN);
  // body
  drawTextBlock(ALG_PROS[algId], 12, 96, IPS_W - 24, 24, TEXT_HI, BG_DEEP, 2);
  drawNavHint("OK = next  -  BACK = prev");
}
static void pageDisadvantages(int algId) {
  ipsClear();
  ipsChrome(ALG_NAMES_SHORT[algId], "limitations");
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(ACCENT_ORANGE, BG_DEEP);
  tft.drawString("DISADVANTAGES", IPS_W / 2, 58);
  tft.drawFastHLine(28, 78, IPS_W - 56, ACCENT_ORANGE);
  drawTextBlock(ALG_CONS[algId], 12, 96, IPS_W - 24, 24, TEXT_HI, BG_DEEP, 2);
  drawNavHint("OK = next  -  BACK = prev");
}

// ============================== COMPARISON PAGES ===========================
//   "mark all or nothing" tie handling: if every algorithm is at the
//   optimum (e.g. all 100% CPU utilization), nobody gets the green badge.

static void drawCompareChart(int x, int y, int w, int h, const char* title,
                             float values[ALG_COUNT], const char* unit,
                             bool lowerIsBetter) {
  drawPanel(x, y, w, h, title, lowerIsBetter ? ACCENT_GREEN : ACCENT_CYAN);

  // find max for scaling
  float vmax = 0;
  for (int i = 0; i < ALG_COUNT; i++) if (values[i] > vmax) vmax = values[i];
  if (vmax <= 0) vmax = 1;

  // find optimum + tied set
  float opt = lowerIsBetter ? 1e9f : -1e9f;
  for (int i = 0; i < ALG_COUNT; i++)
    if (lowerIsBetter ? (values[i] < opt) : (values[i] > opt)) opt = values[i];
  bool best[ALG_COUNT];
  int  bestCount = 0;
  const float eps = 0.02f;
  for (int i = 0; i < ALG_COUNT; i++) {
    best[i] = (fabsf(values[i] - opt) < eps);
    if (best[i]) bestCount++;
  }
  // if every algorithm ties, nobody gets the badge
  if (bestCount == ALG_COUNT) for (int i = 0; i < ALG_COUNT; i++) best[i] = false;

  // bars
  const int labelW = 40;
  const int valueW = 48;
  const int barX0  = x + 8 + labelW;
  const int barWMax = w - 16 - labelW - valueW;
  const int barAreaY = y + 32;
  const int barAreaH = h - 38;
  const int rowH = barAreaH / ALG_COUNT;

  for (int i = 0; i < ALG_COUNT; i++) {
    int by = barAreaY + i * rowH;
    int bh = rowH - 2;
    if (bh < 8) bh = 8;
    uint16_t col = best[i] ? ACCENT_GREEN : ACCENT_CYAN;
    // alg label (font 1 — tight rows)
    tft.setTextDatum(ML_DATUM);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(best[i] ? ACCENT_GREEN : TEXT_HI, BG_PANEL);
    tft.drawString(ALG_NAMES_SHORT[i], x + 8, by + bh / 2);
    // bar grow animation
    int bw = (int)(barWMax * (values[i] / vmax));
    if (bw < 2) bw = 2;
    int steps = max(2, bw / 8);
    for (int k = 1; k <= steps; k++) {
      int wk = (bw * k) / steps;
      tft.fillRoundRect(barX0, by + 1, wk, bh - 2, 2, col);
      delay(3);
    }
    tft.fillRoundRect(barX0, by + 1, bw, bh - 2, 2, col);
    // value
    char b[12];
    if (values[i] >= 100) snprintf(b, sizeof(b), "%.0f", values[i]);
    else                  snprintf(b, sizeof(b), "%.2f", values[i]);
    tft.setTextDatum(MR_DATUM);
    tft.setTextFont(1);
    tft.setTextColor(TEXT_HI, BG_PANEL);
    tft.drawString(b, x + w - 8, by + bh / 2);
  }
  // unit pill bottom-right
  tft.setTextDatum(BR_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(TEXT_DIM, BG_PANEL);
  tft.drawString(unit, x + w - 8, y + h - 4);
}

static void pageComparisonA() {
  ipsClear();
  ipsChrome("Compare", "1 of 2");
  float wait[ALG_COUNT], tat[ALG_COUNT];
  for (int i = 0; i < ALG_COUNT; i++) {
    wait[i] = g_results[i].avgWaiting;
    tat[i]  = g_results[i].avgTurnaround;
  }
  drawCompareChart(6,  42, IPS_W - 12, 90, "AVG WAITING TIME",     wait, "ms", true);
  drawCompareChart(6, 136, IPS_W - 12, 90, "AVG TURNAROUND TIME",  tat,  "ms", true);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  tft.drawString("green = best (lower is better)", IPS_W / 2, IPS_H - 6);
}

static void pageComparisonB() {
  ipsClear();
  ipsChrome("Compare", "2 of 2");
  float rsp[ALG_COUNT], cpu[ALG_COUNT];
  for (int i = 0; i < ALG_COUNT; i++) {
    rsp[i] = g_results[i].avgResponse;
    cpu[i] = g_results[i].cpuUtilization;
  }
  drawCompareChart(6,  42, IPS_W - 12, 90, "AVG RESPONSE TIME", rsp, "ms", true);
  drawCompareChart(6, 136, IPS_W - 12, 90, "CPU UTILIZATION",   cpu, "%",  false);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(TEXT_DIM, BG_DEEP);
  tft.drawString("if all tied, nobody is marked best", IPS_W / 2, IPS_H - 6);
}

// ============================== RUN ORCHESTRATOR ===========================
//   Page count for a single algo:
//     1 (summary) + N (iterations) + 1 (gantt) + 1 (table)
//     + 2 (metrics A/B) + 2 (pros/cons) + 2 (compare A/B)  =  9 + N
//
//   Compare-All: 1 (summary) + 2 (compare A/B) = 3.

static void renderRunPage(int algId, int p) {
  if (algId == ALG_COMPARE_ALL) {
    if (p == 0)      pageInputSummary();
    else if (p == 1) pageComparisonA();
    else             pageComparisonB();
    return;
  }
  if (p == 0) { pageInputSummary(); return; }
  int iterIdx = p - 1;
  if (iterIdx < g_snapCount) { pageIteration(algId, iterIdx); return; }
  int after = p - 1 - g_snapCount;
  switch (after) {
    case 0: pageFinalGantt(algId);       break;
    case 1: pagePerProcessTable(algId);  break;
    case 2: pageMetricsA(algId);         break;
    case 3: pageMetricsB(algId);         break;
    case 4: pageAdvantages(algId);       break;
    case 5: pageDisadvantages(algId);    break;
    case 6: pageComparisonA();           break;
    case 7: pageComparisonB();           break;
  }
}

static void ipsRunAlgorithm(int algId) {
  runAllAlgorithms();   // populate all results (for comparison pages)
  if (algId != ALG_COMPARE_ALL) {
    Process scratch[MAX_PROCESSES];
    for (int i = 0; i < g_numProcs; i++) scratch[i] = g_procs[i];
    runAlgorithm(algId, scratch, g_numProcs, g_quantum, g_results[algId], true);
    for (int i = 0; i < g_numProcs; i++) g_procs[i] = scratch[i];
    ipsAlgorithmTitleCard(algId);
  }

  int total = (algId == ALG_COMPARE_ALL) ? 3 : (9 + g_snapCount);
  const char* algStr = (algId == ALG_COMPARE_ALL) ? "ALL" : ALG_NAMES_SHORT[algId];

  int p = 0;
  while (p < total) {
    oledProgress(algStr, p + 1, total);
    renderRunPage(algId, p);
    while (true) {
      buttonsUpdate();
      if (btnPressed(B_OK))   { p++; break; }
      if (btnPressed(B_BACK)) {
        if (p == 0) return;
        p--; break;
      }
      delay(15);
    }
  }
}

// ============================== STATE MACHINE ==============================
static void seedDefaults() {
  g_numProcs = 4;
  g_procs[0] = {1, 0, 6, 2, 0,0,0,0,0,false,-1};
  g_procs[1] = {2, 1, 2, 1, 0,0,0,0,0,false,-1};
  g_procs[2] = {3, 2, 8, 3, 0,0,0,0,0,false,-1};
  g_procs[3] = {4, 3, 4, 4, 0,0,0,0,0,false,-1};
  g_procs[4] = {5, 4, 3, 5, 0,0,0,0,0,false,-1};
  g_procs[5] = {6, 5, 5, 6, 0,0,0,0,0,false,-1};
  g_quantum  = 2;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void enterMenu() {
  g_state = ST_MENU;
  for (int i = 0; i < ALG_COUNT; i++) g_resultsValid[i] = false;
  oledMenu(g_menuHover);
  ipsIdleScreen();
}

static void enterInputNum() {
  g_state = ST_INPUT_NUM;
  oledInputHint(algShortFor(), "# Processes", String(g_numProcs).c_str());
  pageInputNum();
}

static void enterInputField() {
  g_state = ST_INPUT_FIELD;
  // ensure field is valid (skip Pri if not needed)
  if (g_editField == 2 && !needsPriorityInput()) g_editField = 0;
  const char* fname = (g_editField == 0) ? "Arrival" : (g_editField == 1) ? "Burst" : "Priority";
  char what[24]; snprintf(what, sizeof(what), "P%d %s", g_procs[g_editProc].id, fname);
  int v = (g_editField == 0) ? g_procs[g_editProc].arrival
        : (g_editField == 1) ? g_procs[g_editProc].burst
        :                      g_procs[g_editProc].priority;
  oledInputHint(algShortFor(), what, String(v).c_str());
  pageInputField();
}

static void enterInputQuantum() {
  g_state = ST_INPUT_QUANTUM;
  oledInputHint(algShortFor(), "Quantum", String(g_quantum).c_str());
  pageInputQuantum();
}

static void finishInputsAndRun() {
  int algToRun = g_currentAlg;
  g_state = ST_RUN;
  ipsRunAlgorithm(algToRun);
  // back to menu
  enterMenu();
}

static void advanceInputField() {
  g_editField++;
  if (g_editField == 2 && !needsPriorityInput()) g_editField = 3;
  if (g_editField > 2) { g_editField = 0; g_editProc++; }
  if (g_editProc >= g_numProcs) {
    // all proc fields done
    if (needsQuantumInput()) enterInputQuantum();
    else                     finishInputsAndRun();
  } else {
    enterInputField();
  }
}

static void retreatInputField() {
  g_editField--;
  if (g_editField == 2 && !needsPriorityInput()) g_editField = 1;
  if (g_editField < 0) {
    g_editProc--;
    if (g_editProc < 0) { enterInputNum(); return; }
    g_editField = needsPriorityInput() ? 2 : 1;
  }
  enterInputField();
}

static void handleMenu() {
  bool changed = false;
  if (btnPressed(B_UP)   || btnRepeat(B_UP))   { g_menuHover = (g_menuHover + MENU_COUNT - 1) % MENU_COUNT; changed = true; }
  if (btnPressed(B_DOWN) || btnRepeat(B_DOWN)) { g_menuHover = (g_menuHover + 1) % MENU_COUNT; changed = true; }
  if (changed) oledMenu(g_menuHover);
  if (btnPressed(B_OK)) {
    if (g_menuHover < ALG_COUNT)        g_currentAlg = g_menuHover;
    else                                g_currentAlg = ALG_COMPARE_ALL;
    g_editProc = 0; g_editField = 0;
    enterInputNum();
  }
}

static void handleInputNum() {
  bool dirty = false;
  if (btnPressed(B_UP)   || btnRepeat(B_UP))   { g_numProcs = clampi(g_numProcs + 1, 2, MAX_PROCESSES); dirty = true; }
  if (btnPressed(B_DOWN) || btnRepeat(B_DOWN)) { g_numProcs = clampi(g_numProcs - 1, 2, MAX_PROCESSES); dirty = true; }
  if (dirty) {
    oledInputHint(algShortFor(), "# Processes", String(g_numProcs).c_str());
    pageInputNum();
  }
  if (btnPressed(B_OK))   { g_editProc = 0; g_editField = 0; enterInputField(); }
  if (btnPressed(B_BACK)) { enterMenu(); }
}

static void handleInputField() {
  int* target = nullptr;
  int  lo = 0, hi = 99;
  switch (g_editField) {
    case 0: target = &g_procs[g_editProc].arrival;  lo = 0; hi = 30; break;
    case 1: target = &g_procs[g_editProc].burst;    lo = 1; hi = 20; break;
    case 2: target = &g_procs[g_editProc].priority; lo = 1; hi = 9;  break;
  }
  bool dirty = false;
  if (btnPressed(B_UP)   || btnRepeat(B_UP))   { *target = clampi(*target + 1, lo, hi); dirty = true; }
  if (btnPressed(B_DOWN) || btnRepeat(B_DOWN)) { *target = clampi(*target - 1, lo, hi); dirty = true; }
  if (dirty) { enterInputField(); /* re-render */ }
  if (btnPressed(B_OK))   advanceInputField();
  if (btnPressed(B_BACK)) retreatInputField();
}

static void handleInputQuantum() {
  bool dirty = false;
  if (btnPressed(B_UP)   || btnRepeat(B_UP))   { g_quantum = clampi(g_quantum + 1, 1, 9); dirty = true; }
  if (btnPressed(B_DOWN) || btnRepeat(B_DOWN)) { g_quantum = clampi(g_quantum - 1, 1, 9); dirty = true; }
  if (dirty) {
    oledInputHint(algShortFor(), "Quantum", String(g_quantum).c_str());
    pageInputQuantum();
  }
  if (btnPressed(B_OK))   finishInputsAndRun();
  if (btnPressed(B_BACK)) {
    // back to last process input
    g_editProc = g_numProcs - 1;
    g_editField = needsPriorityInput() ? 2 : 1;
    enterInputField();
  }
}

// ============================== SETUP / LOOP ===============================
void setup() {
  Serial.begin(115200);
  delay(50);
  buttonsBegin();
  oledBegin();
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(BG_DEEP);
  seedDefaults();
  oledSplash();
  ipsSplash();
  // wait for OK
  while (true) {
    buttonsUpdate();
    if (btnPressed(B_OK)) break;
    delay(15);
  }
  enterMenu();
}

void loop() {
  buttonsUpdate();
  switch (g_state) {
    case ST_MENU:           handleMenu();          break;
    case ST_INPUT_NUM:      handleInputNum();      break;
    case ST_INPUT_FIELD:    handleInputField();    break;
    case ST_INPUT_QUANTUM:  handleInputQuantum();  break;
    default: break;
  }
  delay(8);
}
